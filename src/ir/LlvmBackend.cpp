#include "ir/LlvmBackend.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include "ir/InternPool.h"
#include "ir/Ir.h"
#include "runtime/Runtime.h"

// LLVM backend rationale.
//
// 4-state lowering (must/may/z, three planes). Each bit is carried as a pair of
// monotone planes plus a z marker:
//   * `must` — the bit is definitely 1.
//   * `may`  — the bit could be 1.
//   * `z`    — the bit is high-impedance (collapses to X for logic).
// Per bit: 0 = (must 0, may 0), 1 = (must 1, may 1), X = (must 0, may 1); the
// invariant is must => may. Z is encoded as X in must/may with its z plane set.
// This makes the common logic ops monotone and cheap: AND/OR are just per-plane
// &/|, NOT is a swap+complement. The z plane passes through loads/stores and is
// cleared by the logic ops (their result is never Z); we leave it to LLVM to
// elide z where it proves dead rather than dropping it here.
// To reconstruct canonical 4-state when needed: xz = must ^ may, val = must.
//
// State / ABI. Signals are persistent module state held in one opaque byte
// buffer (the LLVM equivalent of `u8* align(8)`). Clotho owns the layout: it
// computes a fixed byte offset for each signal's must|may and z fields at compile
// time (signals packed in decreasing alignment order) and hands every process
// function a bare `ptr`. Load/Store reach a field via a constant-offset i8 GEP
// and an aligned load/store — no LLVM struct layout is involved.
//
// Inferred types. The IR stores no per-instruction result type, so the backend
// recomputes width / domain / signedness for each value by walking operands
// (the frontend already inserted Resize nodes to width-match binary operands).
//
// Scope (initial). Nonblocking assignment is lowered identically to blocking
// (no two-phase next-state staging yet); event / sensitivity gating is left
// to a future top-level sim loop; Arg / Ret (functions, tasks), unpacked arrays
// and memories are unsupported and rejected.

namespace clotho::ir {

namespace {

// Inferred type of a lowered value: its packed width, whether it can carry X/Z
// (a meaningful xz plane), and its signedness (drives sext-vs-zext and signed
// compares).
struct ValType {
    uint32_t width      = 0;
    bool     four_state = false;
    bool     is_signed  = false;
};

// A lowered value: the must/may planes packed into one i(2W) integer (low W bits
// = must / definitely 1, high W bits = may / could be 1), plus a separate iW `z`
// (high-impedance marker). Packing lets the monotone ops (AND/OR/NOT, mux select)
// act on both planes with a single instruction.
struct Plane {
    llvm::Value* mm = nullptr; // i(2W): [0,W) must, [W,2W) may
    llvm::Value* z  = nullptr; // iW
};

// A resolved assignment target: a constant/runtime bit offset and width into a
// signal's state fields. `bit_off` is an i32; whole-signal targets have offset 0
// and width equal to the signal width.
struct LValue {
    Signal       sig;
    llvm::Value* bit_off = nullptr;
    uint32_t     width   = 0;
};

// Per-signal placement in the state buffer plus its cached layout facts. A
// signal occupies two fields: the packed i(2W) must|may word at `off_mm` and the
// iW z word at `off_z` (both absolute byte offsets into the buffer).
struct SignalSlot {
    uint32_t off_mm     = 0;
    uint32_t off_z      = 0;
    uint32_t width      = 0;
    bool     four_state = false;
    bool     is_signed  = false;
};

// Raised on a construct the backend cannot lower; caught at the top level.
struct BackendError {
    std::string message;
};

// Builds the shared state struct and lowers every process. One instance per
// emit; `lower_process` is reused across processes (the per-process value tables
// are reset each time).
class Backend {
public:
    Backend(const Module& mod, llvm::LLVMContext& ctx, llvm::Module& llmod)
      : mod_(mod)
      , ctx_(ctx)
      , llmod_(llmod)
      , b_(ctx) {}

    void run() {
        build_state_layout();
        build_wait_queues();
        build_sched_flags();

        // Lower every process and seed every one at startup. An `initial` runs its
        // body once; an `always` with a sensitivity list immediately subscribes to
        // its signals and suspends (its body runs only when woken). Both kinds are
        // created suspended and enter the active queue together.
        // TODO: an `always` / continuous assign with no sensitivity list (e.g. a
        // continuous `assign`, whose RHS sensitivity is not yet inferred) still
        // runs its body just once — it does not re-trigger on input changes.
        std::vector<llvm::Function*> procs;
        const auto                   all = mod_.processes();
        for (uint32_t i = 0; i < all.size(); ++i) {
            procs.push_back(lower_process(all[i], i));
        }
        emit_main(procs);
    }

    // Per-process "scheduled" flags, one i32 per process: clotho.proc_sched[k] is
    // non-zero while process k is already queued to run this instant. Persistent
    // (always-block) wait entries use it to avoid being woken more than once per
    // instant when several of their sensitivity signals change together.
    void build_sched_flags() {
        const auto n   = std::max<size_t>(mod_.processes().size(), 1);
        auto*      i32 = llvm::Type::getInt32Ty(ctx_);
        sched_arr_ty_  = llvm::ArrayType::get(i32, n);
        sched_gv_      = new llvm::GlobalVariable(llmod_, sched_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                                  llvm::ConstantAggregateZero::get(sched_arr_ty_), "clotho.proc_sched");
    }

private:
    // -- LLVM type helpers --

    llvm::IntegerType* int_ty(uint32_t width) const {
        return llvm::IntegerType::get(ctx_, width == 0 ? 1 : width);
    }
    llvm::ConstantInt* zero(uint32_t width) const {
        return llvm::ConstantInt::get(int_ty(width), 0);
    }
    llvm::ConstantInt* ones(uint32_t width) const {
        return llvm::ConstantInt::get(ctx_, llvm::APInt::getAllOnes(width == 0 ? 1 : width));
    }

    // Cast an integer value to width `to` (zero/sign extend or truncate).
    llvm::Value* cast(llvm::Value* v, uint32_t to, bool sign) {
        const auto from = llvm::cast<llvm::IntegerType>(v->getType())->getBitWidth();
        if (from == to) {
            return v;
        }
        if (from > to) {
            return b_.CreateTrunc(v, int_ty(to));
        }
        return sign ? b_.CreateSExt(v, int_ty(to)) : b_.CreateZExt(v, int_ty(to));
    }

    // -- State buffer layout --

    // Byte size and natural alignment (capped at 8) of an integer field of the
    // given bit width, both derived from the canonical plane sizing.
    static uint32_t field_bytes(uint32_t bits) {
        return plane_bytes(bits);
    }
    static uint32_t field_align(uint32_t bits) {
        return std::min<uint32_t>(8, plane_bytes(bits));
    }
    static uint32_t round_up(uint32_t x, uint32_t a) {
        return (x + a - 1) & ~(a - 1);
    }

    // Lay out the single module's signals into one opaque byte buffer. Each signal
    // owns a `{ i(2W) must|may, iW z }` slot; slots are packed in decreasing
    // alignment order (stable on declaration order) so padding stays minimal.
    // `slots_` stays indexed by signal handle — only the byte offsets reflect the
    // packed order.
    void build_state_layout() {
        const auto sigs = mod_.signals();
        slots_.resize(sigs.size());

        std::vector<uint32_t> order(sigs.size());
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            const Layout lay = mod_.pool.layout(sigs[i].type);
            const Type&  ty  = std::get<Type>(mod_.pool.get(sigs[i].type));

            SignalSlot& slot = slots_[i];
            slot.width       = lay.width == 0 ? 1 : lay.width;
            slot.four_state  = lay.domain == Domain::Four;
            slot.is_signed   = ty.is_signed;
            order[i]         = i;
        }

        // Stable sort by slot alignment (= align of the 2W must|may word) desc.
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return field_align(2 * slots_[a].width) > field_align(2 * slots_[b].width);
        });

        uint32_t cursor = 0;
        for (uint32_t i : order) {
            SignalSlot&    slot       = slots_[i];
            const uint32_t mm_bits    = 2 * slot.width;
            const uint32_t slot_align = field_align(mm_bits);
            const uint32_t z_rel      = round_up(field_bytes(mm_bits), field_align(slot.width));

            const uint32_t slot_size = round_up(z_rel + field_bytes(slot.width), slot_align);
            cursor                   = round_up(cursor, slot_align);
            slot.off_mm              = cursor;
            slot.off_z               = cursor + z_rel;
            cursor += slot_size;
        }

        state_bytes_ = std::max<uint32_t>(1, round_up(cursor, 8));
    }

    const SignalSlot& slot_of(Signal s) const {
        return slots_[static_cast<uint32_t>(s)];
    }

    // -- Per-process lowering --

    llvm::Function* lower_process(const Process& proc, uint32_t proc_index) {
        ir_            = &proc.ir;
        const size_t n = ir_->inst_count();
        values_.assign(n, Plane{});
        lvals_.assign(n, std::optional<LValue>{});
        types_.assign(n, std::optional<ValType>{});

        // This process's scheduled-flag slot: &clotho.proc_sched[proc_index]. A
        // constant GEP so it is valid in any block of the function.
        sched_ptr_ = llvm::ConstantExpr::getInBoundsGetElementPtr(
          sched_arr_ty_, sched_gv_, llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(proc_index) });

        // Name a process by the source byte offset of the AST node that produced
        // it: @<module>.<offset>. The IR's SourceLoc packs {buffer:28|offset:36};
        // the low 36 bits are that byte offset.
        constexpr uint64_t offset_mask = (uint64_t{ 1 } << 36) - 1;
        const uint64_t     offset      = static_cast<uint64_t>(proc.loc) & offset_mask;
        const std::string  name        = mod_.name + "." + std::to_string(offset);

        // A process lowers to a switched-resume LLVM coroutine:
        // `ptr @<module>.<offset>(ptr noalias state, ptr wheel)`. The ramp runs the
        // body until the first #delay (or to completion) and returns the coroutine
        // handle; the scheduler resumes/destroys through that handle. A #delay (see
        // lower_delay) schedules a wheel timer and suspends; the timer's callback
        // resumes the coroutine when the delay elapses. A delay-free process runs
        // straight to the final suspend, exactly like the old plain function.
        auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto* fnty   = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
        fn_          = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, name, llmod_);
        // Mark it pre-split so the CoroSplit pass picks it up.
        fn_->setPresplitCoroutine();
        state_ = fn_->getArg(0);
        state_->setName("state");
        wheel_ = fn_->getArg(1);
        wheel_->setName("wheel");
        // The state pointer is the sole module memory the process touches; noalias
        // lets the allocator rematerialize signal loads instead of spilling them.
        fn_->addParamAttr(0, llvm::Attribute::NoAlias);
        awaiter_ = nullptr;

        auto* none     = llvm::ConstantTokenNone::get(ctx_);
        auto* null     = llvm::ConstantPointerNull::get(ptr_ty);
        auto* entry    = llvm::BasicBlock::Create(ctx_, "entry", fn_);
        auto* alloc_bb = llvm::BasicBlock::Create(ctx_, "coro.alloc", fn_);
        auto* begin_bb = llvm::BasicBlock::Create(ctx_, "coro.begin", fn_);

        // Identify the coroutine; coro.alloc lets the optimizer skip the dynamic
        // frame allocation when the frame can live on the caller's stack.
        b_.SetInsertPoint(entry);
        auto* id   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_id), { b_.getInt32(0), null, null, null });
        auto* need = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_alloc), { id });
        b_.CreateCondBr(need, alloc_bb, begin_bb);

        // Dynamic frame: rt_alloc(coro.size). The sim owns frame memory so a frame
        // outlives the ramp while the process is suspended on the wheel.
        b_.SetInsertPoint(alloc_bb);
        auto* size = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_size, { b_.getInt64Ty() }), {});
        auto* mem  = b_.CreateCall(alloc_fn(), { size });
        b_.CreateBr(begin_bb);

        b_.SetInsertPoint(begin_bb);
        auto* frame = b_.CreatePHI(ptr_ty, 2);
        frame->addIncoming(null, entry);
        frame->addIncoming(mem, alloc_bb);
        hdl_        = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_begin), { id, frame });
        coro_begin_ = llvm::cast<llvm::Instruction>(hdl_);

        // Shared epilogue, built up front so #delay suspends and the final suspend
        // can all branch to it. cleanup frees the frame (coro.free -> rt_free); ret
        // ends the coroutine and returns the handle to whoever resumed it.
        auto* trap_bb     = llvm::BasicBlock::Create(ctx_, "coro.trap", fn_);
        auto* free_bb     = llvm::BasicBlock::Create(ctx_, "coro.free", fn_);
        coro_cleanup_bb_  = llvm::BasicBlock::Create(ctx_, "coro.cleanup", fn_);
        coro_ret_bb_      = llvm::BasicBlock::Create(ctx_, "coro.ret", fn_);

        b_.SetInsertPoint(coro_cleanup_bb_);
        auto* freed = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_free), { id, hdl_ });
        b_.CreateCondBr(b_.CreateICmpNE(freed, null), free_bb, coro_ret_bb_);

        b_.SetInsertPoint(free_bb);
        b_.CreateCall(free_fn(), { freed });
        b_.CreateBr(coro_ret_bb_);

        b_.SetInsertPoint(coro_ret_bb_);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_end), { hdl_, b_.getFalse(), none });
        b_.CreateRet(hdl_);

        // Initial suspend: the ramp builds the frame and immediately suspends here,
        // returning the handle WITHOUT running the body. The scheduler runs the body
        // on the first coro.resume. This makes "create a process" and "wake a
        // process" the same operation — resume a handle — so both flow through the
        // active queue uniformly. The default edge returns to the creator; case 0
        // (first resume) enters the body; case 1 is a destroy before first resume.
        b_.SetInsertPoint(begin_bb);
        auto* start_bb = llvm::BasicBlock::Create(ctx_, "coro.start", fn_);
        auto* init     = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getFalse() });
        auto* sw_init  = b_.CreateSwitch(init, coro_ret_bb_, 2);
        sw_init->addCase(b_.getInt8(0), start_bb);
        sw_init->addCase(b_.getInt8(1), coro_cleanup_bb_);

        // Process body. lower_region appends into start_bb (and into any blocks a
        // #delay / @-wait suspend creates), leaving the insert point on a
        // terminator-less block that falls through to the final suspend.
        b_.SetInsertPoint(start_bb);
        const bool always_loop = proc.activation == Activation::Always && !proc.wait_events.empty();
        if (always_loop) {
            // An always block with a sensitivity list. It is subscribed to its
            // signals' static queues from the seed loop (emit_active_run) and left
            // parked at the initial suspend; the first wake resumes here. So the
            // loop is body-first: run the body for the trigger that woke us, re-arm
            // the scheduled flag, suspend until the next trigger, repeat. The body
            // therefore runs only on a trigger, never at time zero, with the flag
            // held set across the body (it is cleared just before the suspend).
            auto* loop_bb = llvm::BasicBlock::Create(ctx_, "always.loop", fn_);
            b_.CreateBr(loop_bb);
            b_.SetInsertPoint(loop_bb);
            lower_region(ir_->root());                  // body (per trigger)
            b_.CreateStore(b_.getInt32(0), sched_ptr_); // re-arm: ready to be woken
            emit_wait_suspend();                        // park until next trigger
            b_.CreateBr(loop_bb);
            // The forever back-edge means the shared final-suspend tail below is
            // unreachable; build it in a dead block so the IR stays well-formed.
            b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "always.exit", fn_));
        } else {
            lower_region(ir_->root());
        }

        // Final suspend: the default edge returns the (now-done) handle to the
        // resumer, which destroys it; case 1 is the destroy re-entry; resuming a
        // finished coroutine (case 0) is undefined, so trap.
        auto* final_bb = llvm::BasicBlock::Create(ctx_, "coro.final", fn_);
        b_.CreateBr(final_bb);
        b_.SetInsertPoint(final_bb);
        auto* res = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getTrue() });
        auto* sw  = b_.CreateSwitch(res, coro_ret_bb_, 2);
        sw->addCase(b_.getInt8(0), trap_bb);
        sw->addCase(b_.getInt8(1), coro_cleanup_bb_);

        b_.SetInsertPoint(trap_bb);
        b_.CreateUnreachable();
        return fn_;
    }

    // The awaiter embedded in the coroutine frame for #delay: an rt_timer
    // ({ next, on_expire }) followed by the coroutine handle. &awaiter aliases the
    // rt_timer the wheel links; the resume trampoline recovers the handle from
    // field 2. One per process, reused across delays (a process waits on at most
    // one timer at a time).
    llvm::StructType* awaiter_ty() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llvm::StructType::get(ctx_, { ptr, ptr, ptr });
    }

    // Lazily allocate the awaiter, placed just after coro.begin so CoroSplit spills
    // it into the frame (its address escapes to the wheel and it is live across the
    // suspend).
    llvm::Value* awaiter() {
        if (awaiter_ == nullptr) {
            // Insert right after coro.begin — using the block + iterator form so it
            // works even when coro.begin is the last instruction so far (a #delay as
            // the process's first statement).
            llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
            awaiter_ = ab.CreateAlloca(awaiter_ty(), nullptr, "awaiter");
        }
        return awaiter_;
    }

    // The shared timer callback: void @clotho.enqueue_timer(ptr timer, ptr ctx).
    // When a timer fires it does not resume the coroutine directly; it recovers the
    // handle from the awaiter and pushes it onto the active queue. The scheduler
    // resumes it (with everything else woken at the same instant) when it next
    // drains the queue. Built once per module.
    llvm::Function* enqueue_trampoline() {
        if (enqueue_fn_ != nullptr) {
            return enqueue_fn_;
        }
        auto* ptr   = llvm::PointerType::getUnqual(ctx_);
        auto* fnty  = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr }, false);
        enqueue_fn_ = llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.enqueue_timer", llmod_);

        llvm::IRBuilder<> tb(ctx_);
        tb.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", enqueue_fn_));
        llvm::Value* hp  = tb.CreateStructGEP(awaiter_ty(), enqueue_fn_->getArg(0), 2);
        llvm::Value* hdl = tb.CreateLoad(ptr, hp);
        emit_active_push(tb, hdl);
        tb.CreateRetVoid();
        return enqueue_fn_;
    }

    // Lower a #delay: schedule a wheel timer for `now + amount` ticks that resumes
    // this coroutine, then suspend. Execution continues in a fresh resume block.
    void lower_delay(Index amount) {
        auto* aw = awaiter();
        // Stash the handle so the timer callback can resume us.
        b_.CreateStore(hdl_, b_.CreateStructGEP(awaiter_ty(), aw, 2));

        llvm::Value* ticks    = cast(must_of(value_of(amount)), 64, false);
        llvm::Value* now      = b_.CreateCall(wheel_now_fn(), { wheel_ });
        llvm::Value* deadline = b_.CreateAdd(now, ticks);
        b_.CreateCall(wheel_schedule_fn(), { wheel_, aw, deadline, enqueue_trampoline() });

        // Suspend. The save token pairs the suspend with this resume point; the
        // default edge returns control to the resumer (coro.ret), case 0 resumes
        // here, case 1 is a destroy while suspended.
        auto* save   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_save), { hdl_ });
        auto* susp   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { save, b_.getFalse() });
        auto* resume = llvm::BasicBlock::Create(ctx_, "coro.resume", fn_);
        auto* sw     = b_.CreateSwitch(susp, coro_ret_bb_, 2);
        sw->addCase(b_.getInt8(0), resume);
        sw->addCase(b_.getInt8(1), coro_cleanup_bb_);
        b_.SetInsertPoint(resume);
    }

    // Store a { handle, edge, flag_ptr } entry at a chosen slot of a queue array.
    void emit_entry(llvm::ArrayType* arr_ty, llvm::Value* q, llvm::Value* slot_idx, uint32_t edge,
                    llvm::Value* handle, llvm::Value* flag_ptr) {
        llvm::StructType* elem = wait_elem_ty();
        llvm::Value*      slot = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), slot_idx });
        b_.CreateStore(handle, b_.CreateStructGEP(elem, slot, 0));
        b_.CreateStore(b_.getInt8(static_cast<uint8_t>(edge)), b_.CreateStructGEP(elem, slot, 1));
        b_.CreateStore(flag_ptr, b_.CreateStructGEP(elem, slot, 2));
    }

    // Persistent (always-block) subscription: write a process's entry into the
    // signal's static queue at a compile-time slot. Slots are handed out in
    // process / wait-event order, matching the capacities build_wait_queues
    // computed, so each array fills exactly once and is never compacted. Issued
    // from the seed loop (emit_active_run) with the process's ramp handle and
    // scheduled-flag pointer — by plain stores, never by resuming the coroutine,
    // so it does not inhibit coro-elide of other processes. All subscriptions land
    // before any servicing, so the fixed-trip scans never read an empty slot.
    void emit_static_subscribe(Signal sig, uint32_t edge, llvm::Value* handle, llvm::Value* flag_ptr) {
        const auto     i    = static_cast<uint32_t>(sig);
        const uint32_t slot = sta_next_[i]++;
        emit_entry(wait_sta_ty_[i], wait_sta_gv_[i], b_.getInt32(slot), edge, handle, flag_ptr);
    }

    // One-shot (intra-body @(sig)) registration: append this coroutine to the
    // signal's dynamic queue, bumping .dyn.len. No bounds check — capacity is the
    // count of distinct processes that intra-body-wait on the signal, and a process
    // holds at most one dynamic slot per signal at a time.
    void emit_dynamic_register(Signal sig, uint32_t edge) {
        const auto   i      = static_cast<uint32_t>(sig);
        auto*        len_gv = wait_dyn_len_gv_[i];
        auto*        i32    = b_.getInt32Ty();
        llvm::Value* len    = b_.CreateLoad(i32, len_gv);
        emit_entry(wait_dyn_ty_[i], wait_dyn_gv_[i], len, edge, hdl_, sched_ptr_);
        b_.CreateStore(b_.CreateAdd(len, b_.getInt32(1)), len_gv);
    }

    // Suspend the coroutine and continue in a fresh resume block, woken by a wait
    // queue rather than a wheel timer. Like a #delay's suspend: the default edge
    // returns to the resumer, case 0 resumes here, case 1 is a destroy while
    // suspended.
    void emit_wait_suspend() {
        auto* save   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_save), { hdl_ });
        auto* susp   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { save, b_.getFalse() });
        auto* resume = llvm::BasicBlock::Create(ctx_, "coro.wait_resume", fn_);
        auto* sw     = b_.CreateSwitch(susp, coro_ret_bb_, 2);
        sw->addCase(b_.getInt8(0), resume);
        sw->addCase(b_.getInt8(1), coro_cleanup_bb_);
        b_.SetInsertPoint(resume);
    }

    // Lower an intra-body `@(edge sig)` event wait: register on the signal's
    // dynamic queue and suspend. A matching blocking store wakes us
    // (service_dynamic_queue pushes the handle onto the active queue and removes
    // the entry); the scheduler resumes us in the fresh resume block.
    void lower_wait(Signal sig, uint32_t edge) {
        emit_dynamic_register(sig, edge);
        emit_wait_suspend();
    }

    // -- Top-level entry --
    //
    // Emit `i32 @main()`: stack-allocate the shared state, power it up, build the
    // timing wheel, then run every process through the active task queue and drain
    // the wheel (advancing time, resuming delayed/woken processes) until idle.
    // Returns 0.
    void emit_main(const std::vector<llvm::Function*>& procs) {
        auto* i32  = llvm::Type::getInt32Ty(ctx_);
        auto* fnty = llvm::FunctionType::get(i32, false);
        auto* main = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, "main", llmod_);

        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", main);
        b_.SetInsertPoint(entry);

        auto*           buf_ty = llvm::ArrayType::get(b_.getInt8Ty(), state_bytes_);
        llvm::AllocaInst* state  = b_.CreateAlloca(buf_ty, nullptr, "state");
        state->setAlignment(llvm::Align(8));
        init_state(state);

        // Timing wheel: the event scheduler. Processes schedule timers on it at
        // #delay and the drain loop ticks it to advance time and resume them.
        auto* wheel_ty = llvm::ArrayType::get(b_.getInt8Ty(), sizeof(rt_wheel));
        auto* wheel    = b_.CreateAlloca(wheel_ty, nullptr, "wheel");
        wheel->setAlignment(llvm::Align(alignof(rt_wheel)));
        b_.CreateCall(wheel_init_fn(), { wheel });

        emit_active_run(state, wheel, procs);

        b_.CreateCall(wheel_deinit_fn(), { wheel });
        b_.CreateRet(llvm::ConstantInt::get(i32, 0));
    }

    // Per-signal capacities for the two wait queues.
    struct WaitCounts {
        // Static queue: one slot per sensitivity wait event (an always block's
        // @(edge sig)). Membership is fixed for the whole run, so this is exact.
        std::vector<uint32_t> sta;
        // Dynamic queue: one slot per distinct process that does an intra-body
        // @(sig). A process holds at most one such slot per signal at a time.
        std::vector<uint32_t> dyn;
    };

    WaitCounts wait_counts() const {
        const auto    n = mod_.signals().size();
        WaitCounts    c{ std::vector<uint32_t>(n, 0), std::vector<uint32_t>(n, 0) };
        for (const Process& p : mod_.processes()) {
            for (const WaitEvent& w : p.wait_events) {
                c.sta[static_cast<uint32_t>(w.signal)]++;
            }
            std::vector<Signal> seen;
            for (size_t i = 0; i < p.ir.inst_count(); ++i) {
                const auto idx = static_cast<Index>(i);
                if (p.ir.tag(idx) != Inst::Tag::Wait) {
                    continue;
                }
                const Signal s   = p.ir.payload(idx).wait.signal;
                bool         dup = false;
                for (Signal e : seen) {
                    dup = dup || e == s;
                }
                if (!dup) {
                    seen.push_back(s);
                    c.dyn[static_cast<uint32_t>(s)]++;
                }
            }
        }
        return c;
    }

    // One wait-queue entry, shared by both the static and dynamic queues:
    // { ptr handle, i8 edge, ptr flag_ptr }.
    //   handle   — the suspended coroutine to resume.
    //   edge     — the ir::Edge it waits for (i8).
    //   flag_ptr — &clotho.proc_sched[process]. The static queue reads it to avoid
    //              waking a process twice when several of its sensitivity signals
    //              change together. The dynamic queue only stores it (groundwork
    //              for multi-signal intra-body waits); it does not read it yet.
    // The persistent/transient distinction is now encoded by which queue the entry
    // lives in (.sta vs .dyn), not a per-entry flag.
    llvm::StructType* wait_elem_ty() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        auto* i8  = b_.getInt8Ty();
        return llvm::StructType::get(ctx_, { ptr, i8, ptr });
    }

    // Per-signal wait queues, each created only if it has any waiter:
    //
    //   clotho.wait.<sig>.sta  — persistent always-block subscriptions. Filled at
    //     startup (priming) and never pushed/popped; serviced by a fixed-trip scan
    //     (capacity is a compile-time constant, so the optimizer can unroll it).
    //     No length global.
    //   clotho.wait.<sig>.dyn  — one-shot intra-body @(sig) waiters, with a
    //     .dyn.len of live entries; serviced by a compacting scan that removes
    //     each waiter as it fires (as the single merged queue did before).
    //
    // Splitting them keeps the hot path lean: an always-only signal carries just a
    // fixed array (no len, no compaction), an initial-only signal just the dynamic
    // queue. Both share the { handle, edge, flag_ptr } element type.
    void build_wait_queues() {
        const auto       sigs = mod_.signals();
        const WaitCounts c    = wait_counts();
        wait_sta_gv_.assign(sigs.size(), nullptr);
        wait_sta_ty_.assign(sigs.size(), nullptr);
        wait_sta_cap_.assign(sigs.size(), 0);
        wait_dyn_gv_.assign(sigs.size(), nullptr);
        wait_dyn_ty_.assign(sigs.size(), nullptr);
        wait_dyn_len_gv_.assign(sigs.size(), nullptr);
        sta_next_.assign(sigs.size(), 0);

        auto* i32  = llvm::Type::getInt32Ty(ctx_);
        auto* elem = wait_elem_ty();
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            const std::string name = "clotho.wait." + sigs[i].name;
            if (c.sta[i] > 0) {
                auto* arr        = llvm::ArrayType::get(elem, c.sta[i]);
                wait_sta_ty_[i]  = arr;
                wait_sta_cap_[i] = c.sta[i];
                wait_sta_gv_[i]  = new llvm::GlobalVariable(llmod_, arr, false, llvm::GlobalValue::InternalLinkage,
                                                            llvm::ConstantAggregateZero::get(arr), name + ".sta");
            }
            if (c.dyn[i] > 0) {
                auto* arr       = llvm::ArrayType::get(elem, c.dyn[i]);
                wait_dyn_ty_[i] = arr;
                wait_dyn_gv_[i] = new llvm::GlobalVariable(llmod_, arr, false, llvm::GlobalValue::InternalLinkage,
                                                           llvm::ConstantAggregateZero::get(arr), name + ".dyn");
                wait_dyn_len_gv_[i] =
                  new llvm::GlobalVariable(llmod_, i32, false, llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantInt::get(i32, 0), name + ".dyn.len");
            }
        }
    }

    bool signal_has_waiters(Signal sig) const {
        const auto i = static_cast<uint32_t>(sig);
        return wait_sta_gv_[i] != nullptr || wait_dyn_gv_[i] != nullptr;
    }

    // Load a signal's packed must|may state word (the i(2W) at off_mm).
    llvm::Value* load_mm(Signal sig) {
        const SignalSlot& s = slot_of(sig);
        return b_.CreateAlignedLoad(
          int_ty(2 * s.width), field_ptr(state_, s.off_mm), llvm::Align(field_align(2 * s.width)));
    }

    // i1: does the edge `e_i8` (an ir::Edge) fire for a transition with these
    // precomputed facts? Positive = a 0->1 LSB change, Negative = 1->0, Any = any
    // change of the packed state.
    llvm::Value* edge_matches(llvm::Value* e_i8, llvm::Value* pos, llvm::Value* neg, llvm::Value* changed) {
        auto* i8 = b_.getInt8Ty();
        auto  is = [&](Edge ed) {
            return b_.CreateICmpEQ(e_i8, llvm::ConstantInt::get(i8, static_cast<uint8_t>(ed)));
        };
        llvm::Value* m = b_.CreateOr(b_.CreateAnd(is(Edge::Positive), pos), b_.CreateAnd(is(Edge::Negative), neg));
        return b_.CreateOr(m, b_.CreateAnd(is(Edge::Any), changed));
    }

    // Service a signal's wait queues after a blocking assignment to it. Computes
    // the transition's edge facts once, then scans whichever queues exist. Emits
    // nothing for a signal with no waiters.
    void service_wait_queue(Signal sig, llvm::Value* old_mm, llvm::Value* new_mm) {
        const auto i = static_cast<uint32_t>(sig);
        if (wait_sta_gv_[i] == nullptr && wait_dyn_gv_[i] == nullptr) {
            return;
        }
        // Edge-detection facts, computed once for all entries. The LSB of `must` is
        // bit 0 of the packed word; Any compares the whole word.
        llvm::Value* old_lsb = b_.CreateTrunc(old_mm, b_.getInt1Ty());
        llvm::Value* new_lsb = b_.CreateTrunc(new_mm, b_.getInt1Ty());
        llvm::Value* pos     = b_.CreateAnd(b_.CreateNot(old_lsb), new_lsb);
        llvm::Value* neg     = b_.CreateAnd(old_lsb, b_.CreateNot(new_lsb));
        llvm::Value* changed = b_.CreateICmpNE(old_mm, new_mm);

        if (wait_sta_gv_[i] != nullptr) {
            service_static_queue(sig, pos, neg, changed);
        }
        if (wait_dyn_gv_[i] != nullptr) {
            service_dynamic_queue(sig, pos, neg, changed);
        }
    }

    // Static queue scan: a fixed-trip loop over the persistent subscriptions (the
    // capacity is a compile-time constant, so the optimizer may unroll). Membership
    // never changes — no compaction, no length. Each matching entry wakes its
    // process, but only if it is not already scheduled this instant (its flag is
    // 0), then sets the flag; that flag is what collapses a multi-signal sensitivity
    // (the same process subscribed under several signals) into a single wake.
    void service_static_queue(Signal sig, llvm::Value* pos, llvm::Value* neg, llvm::Value* changed) {
        const auto        i      = static_cast<uint32_t>(sig);
        auto*             q      = wait_sta_gv_[i];
        auto*             arr_ty = wait_sta_ty_[i];
        const uint32_t    cap    = wait_sta_cap_[i];
        llvm::StructType* elem   = wait_elem_ty();
        auto*             ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*             i8     = b_.getInt8Ty();
        auto*             i32    = b_.getInt32Ty();

        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* head  = llvm::BasicBlock::Create(ctx_, "sta.head", fn);
        auto* body  = llvm::BasicBlock::Create(ctx_, "sta.body", fn);
        auto* mblk  = llvm::BasicBlock::Create(ctx_, "sta.match", fn);
        auto* wake  = llvm::BasicBlock::Create(ctx_, "sta.wake", fn);
        auto* cont  = llvm::BasicBlock::Create(ctx_, "sta.cont", fn);
        auto* done  = llvm::BasicBlock::Create(ctx_, "sta.done", fn);
        auto* entry = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(cap)), body, done);

        b_.SetInsertPoint(body);
        llvm::Value* src   = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), idx });
        llvm::Value* h     = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 0));
        llvm::Value* e     = b_.CreateLoad(i8, b_.CreateStructGEP(elem, src, 1));
        llvm::Value* fptr  = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 2));
        b_.CreateCondBr(edge_matches(e, pos, neg, changed), mblk, cont);

        b_.SetInsertPoint(mblk);
        llvm::Value* already = b_.CreateICmpNE(b_.CreateLoad(i32, fptr), b_.getInt32(0));
        b_.CreateCondBr(already, cont, wake);

        b_.SetInsertPoint(wake);
        emit_active_push(b_, h);
        b_.CreateStore(b_.getInt32(1), fptr); // mark scheduled
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        llvm::Value* next = b_.CreateAdd(idx, b_.getInt32(1));
        b_.CreateBr(head);
        idx->addIncoming(next, cont);

        b_.SetInsertPoint(done);
    }

    // Dynamic queue scan: a compacting loop over the live one-shot waiters. Read
    // index i scans [0, len), write index w keeps the survivors; a matching entry
    // is woken and removed (dropped), a non-matching one is compacted down. The new
    // length is the final w. (The flag is stored in each entry but not consulted —
    // today's intra-body waits are single-signal, so a waiter is in just one queue.)
    void service_dynamic_queue(Signal sig, llvm::Value* pos, llvm::Value* neg, llvm::Value* changed) {
        const auto        i      = static_cast<uint32_t>(sig);
        auto*             q      = wait_dyn_gv_[i];
        auto*             arr_ty = wait_dyn_ty_[i];
        auto*             len_gv = wait_dyn_len_gv_[i];
        llvm::StructType* elem   = wait_elem_ty();
        auto*             ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*             i8     = b_.getInt8Ty();
        auto*             i32    = b_.getInt32Ty();

        llvm::Value* n     = b_.CreateLoad(i32, len_gv);
        auto*        fn    = b_.GetInsertBlock()->getParent();
        auto*        head  = llvm::BasicBlock::Create(ctx_, "dyn.head", fn);
        auto*        body  = llvm::BasicBlock::Create(ctx_, "dyn.body", fn);
        auto*        wake  = llvm::BasicBlock::Create(ctx_, "dyn.wake", fn);
        auto*        keep  = llvm::BasicBlock::Create(ctx_, "dyn.keep", fn);
        auto*        cont  = llvm::BasicBlock::Create(ctx_, "dyn.cont", fn);
        auto*        done  = llvm::BasicBlock::Create(ctx_, "dyn.done", fn);
        auto*        entry = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* i_phi = b_.CreatePHI(i32, 2);
        auto* w_phi = b_.CreatePHI(i32, 2);
        i_phi->addIncoming(b_.getInt32(0), entry);
        w_phi->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(i_phi, n), body, done);

        b_.SetInsertPoint(body);
        llvm::Value* src  = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), i_phi });
        llvm::Value* h    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 0));
        llvm::Value* e    = b_.CreateLoad(i8, b_.CreateStructGEP(elem, src, 1));
        llvm::Value* fptr = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 2));
        b_.CreateCondBr(edge_matches(e, pos, neg, changed), wake, keep);

        // wake: push and remove (drop the slot, w unchanged).
        b_.SetInsertPoint(wake);
        emit_active_push(b_, h);
        b_.CreateBr(cont);

        // keep: compact the survivor down to slot w (a no-op store when w == i).
        b_.SetInsertPoint(keep);
        llvm::Value* dst = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), w_phi });
        b_.CreateStore(h, b_.CreateStructGEP(elem, dst, 0));
        b_.CreateStore(e, b_.CreateStructGEP(elem, dst, 1));
        b_.CreateStore(fptr, b_.CreateStructGEP(elem, dst, 2));
        llvm::Value* w_inc = b_.CreateAdd(w_phi, b_.getInt32(1));
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        auto* w_next = b_.CreatePHI(i32, 2);
        w_next->addIncoming(w_phi, wake);
        w_next->addIncoming(w_inc, keep);
        llvm::Value* i_next = b_.CreateAdd(i_phi, b_.getInt32(1));
        b_.CreateBr(head);
        i_phi->addIncoming(i_next, cont);
        w_phi->addIncoming(w_next, cont);

        b_.SetInsertPoint(done);
        b_.CreateStore(w_phi, len_gv);
    }

    // The active task queue: ready-to-run coroutine handles held in a fixed-size
    // global used as a LIFO stack (active events may fire in any order per the LRM,
    // so the stack discipline is free). Capacity is the total process count — the
    // most that can ever be ready at once. Created once, lazily, so the timer
    // callback (built while lowering a process) and emit_active_run share it.
    void ensure_active_queue() {
        if (queue_gv_ != nullptr) {
            return;
        }
        auto*      ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*      i32    = llvm::Type::getInt32Ty(ctx_);
        const auto cap    = std::max<size_t>(mod_.processes().size(), 1);

        queue_arr_ty_ = llvm::ArrayType::get(ptr_ty, cap);
        queue_gv_     = new llvm::GlobalVariable(llmod_, queue_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                                 llvm::ConstantAggregateZero::get(queue_arr_ty_), "clotho.active");
        // Stack top: index of the next free slot. Starts at 0 (empty).
        top_gv_ = new llvm::GlobalVariable(llmod_, i32, false, llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantInt::get(i32, 0), "clotho.active_top");
    }

    // Push a handle onto the active queue: queue[top++] = handle.
    void emit_active_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
        ensure_active_queue();
        auto*        i32  = b.getInt32Ty();
        llvm::Value* sp   = b.CreateLoad(i32, top_gv_);
        llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, queue_gv_, { b.getInt32(0), sp });
        b.CreateStore(handle, slot);
        b.CreateStore(b.CreateAdd(sp, b.getInt32(1)), top_gv_);
    }

    // Run the simulation: seed the active queue with every process, then alternate
    // draining the active queue with ticking the wheel.
    //
    // Draining the queue pops a handle and resumes its coroutine — the first resume
    // runs the body from the start (each process is created suspended at its initial
    // suspend); a later resume continues it past a #delay or @-wait. A resume that
    // finishes the coroutine (coro.done) destroys it (an always block loops forever
    // and never finishes; it just stays suspended in its wait queues).
    //
    // When the queue empties, one rt_wheel_tick advances time to the next instant
    // and fires every timer scheduled there; each timer's callback pushes its
    // handle onto the active queue rather than resuming inline. Control then loops
    // back to drain the queue, so a whole time-instant's worth of woken processes
    // runs together. Repeat until no timers remain.
    void emit_active_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
        ensure_active_queue();
        auto* ptr_ty    = llvm::PointerType::getUnqual(ctx_);
        auto* i32       = b_.getInt32Ty();
        auto* proc_fnty = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);

        // Init: empty the stack, then create every process (the ramp call builds
        // the frame and stops at the initial suspend, no body yet).
        //
        // An always block with a sensitivity list is subscribed to its signals'
        // static queues right here, by plain stores of its ramp handle — never by
        // resuming it (resuming would defeat coro-elide of the other processes).
        // It is left parked at the initial suspend and NOT enqueued; the first
        // blocking store that matches its edge wakes it, resuming straight into its
        // body. Subscribing before the drain guarantees every static slot is filled
        // before any servicing scans it. Everything else (initial blocks,
        // sensitivity-less always / continuous assigns) is enqueued to run during
        // the drain.
        b_.CreateStore(b_.getInt32(0), top_gv_);
        const auto all = mod_.processes();
        for (uint32_t k = 0; k < procs.size(); ++k) {
            llvm::Value*   hdl = b_.CreateCall(proc_fnty, procs[k], { state, wheel });
            const Process& p   = all[k];
            if (p.activation == Activation::Always && !p.wait_events.empty()) {
                llvm::Constant* flag = llvm::ConstantExpr::getInBoundsGetElementPtr(
                  sched_arr_ty_, sched_gv_, llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(k) });
                for (const WaitEvent& w : p.wait_events) {
                    emit_static_subscribe(w.signal, static_cast<uint32_t>(w.edge), hdl, flag);
                }
            } else {
                emit_active_push(b_, hdl);
            }
        }

        auto* fn      = b_.GetInsertBlock()->getParent();
        auto* check   = llvm::BasicBlock::Create(ctx_, "active.check", fn);
        auto* body    = llvm::BasicBlock::Create(ctx_, "active.body", fn);
        auto* destroy = llvm::BasicBlock::Create(ctx_, "active.destroy", fn);
        auto* cont    = llvm::BasicBlock::Create(ctx_, "active.cont", fn);
        auto* wcheck  = llvm::BasicBlock::Create(ctx_, "wheel.check", fn);
        auto* wtick   = llvm::BasicBlock::Create(ctx_, "wheel.tick", fn);
        auto* wdone   = llvm::BasicBlock::Create(ctx_, "wheel.done", fn);
        auto* null    = llvm::ConstantPointerNull::get(ptr_ty);
        b_.CreateBr(check);

        // Drain the active queue: while top != 0, pop queue[--top] and resume it.
        b_.SetInsertPoint(check);
        llvm::Value* sp = b_.CreateLoad(i32, top_gv_);
        b_.CreateCondBr(b_.CreateICmpNE(sp, b_.getInt32(0)), body, wcheck);

        b_.SetInsertPoint(body);
        llvm::Value* idx  = b_.CreateSub(sp, b_.getInt32(1));
        b_.CreateStore(idx, top_gv_);
        llvm::Value* slot = b_.CreateInBoundsGEP(queue_arr_ty_, queue_gv_, { b_.getInt32(0), idx });
        llvm::Value* hdl  = b_.CreateLoad(ptr_ty, slot);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        llvm::Value* fin = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { hdl });
        b_.CreateCondBr(fin, destroy, cont);

        b_.SetInsertPoint(destroy);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        b_.CreateBr(check);

        // Queue empty: if any timer remains, advance one instant (firing its timers,
        // which enqueue their handles) and go drain the queue again.
        b_.SetInsertPoint(wcheck);
        llvm::Value* pending = b_.CreateCall(wheel_pending_fn(), { wheel });
        b_.CreateCondBr(b_.CreateICmpNE(pending, b_.getInt32(0)), wtick, wdone);

        b_.SetInsertPoint(wtick);
        b_.CreateCall(wheel_tick_fn(), { wheel, null });
        b_.CreateBr(check);

        b_.SetInsertPoint(wdone);
    }

    // -- Coroutine intrinsics / frame allocator --

    // Declare (or fetch) an llvm.coro.* intrinsic. `tys` supplies the overload
    // type for the few overloaded ones (e.g. coro.size -> i64).
    llvm::Function* coro_intr(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> tys = {}) {
        return llvm::Intrinsic::getOrInsertDeclaration(&llmod_, id, tys);
    }
    // ptr rt_alloc(i64) — coroutine frame allocation, sim-owned.
    llvm::FunctionCallee alloc_fn() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llmod_.getOrInsertFunction("rt_alloc", llvm::FunctionType::get(ptr, { b_.getInt64Ty() }, false));
    }
    // void rt_free(ptr) — coroutine frame deallocation.
    llvm::FunctionCallee free_fn() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llmod_.getOrInsertFunction("rt_free", llvm::FunctionType::get(b_.getVoidTy(), { ptr }, false));
    }

    // -- Timing wheel runtime (runtime/Runtime.h C ABI) --

    // void rt_wheel_init(rt_wheel*) / void rt_wheel_deinit(rt_wheel*)
    llvm::FunctionCallee wheel_init_fn() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llmod_.getOrInsertFunction("rt_wheel_init", llvm::FunctionType::get(b_.getVoidTy(), { ptr }, false));
    }
    llvm::FunctionCallee wheel_deinit_fn() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llmod_.getOrInsertFunction("rt_wheel_deinit", llvm::FunctionType::get(b_.getVoidTy(), { ptr }, false));
    }
    // i64 rt_wheel_now(const rt_wheel*)
    llvm::FunctionCallee wheel_now_fn() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llmod_.getOrInsertFunction("rt_wheel_now", llvm::FunctionType::get(b_.getInt64Ty(), { ptr }, false));
    }
    // i32 rt_wheel_pending(const rt_wheel*)
    llvm::FunctionCallee wheel_pending_fn() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llmod_.getOrInsertFunction("rt_wheel_pending", llvm::FunctionType::get(b_.getInt32Ty(), { ptr }, false));
    }
    // void rt_wheel_schedule(rt_wheel*, rt_timer*, i64 deadline, rt_timer_fn)
    llvm::FunctionCallee wheel_schedule_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr, b_.getInt64Ty(), ptr }, false);
        return llmod_.getOrInsertFunction("rt_wheel_schedule", fnty);
    }
    // void rt_wheel_tick(rt_wheel*, void* ctx)
    llvm::FunctionCallee wheel_tick_fn() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llmod_.getOrInsertFunction("rt_wheel_tick", llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr }, false));
    }

    // Power-up the state struct: 4-state signals come up X (must 0, may 1, z 0),
    // 2-state signals come up 0.
    void init_state(llvm::Value* state) {
        for (const SignalSlot& s : slots_) {
            // X packs as the high W bits of the i(2W) must|may word (may=1, must=0).
            llvm::Value* mm = s.four_state
                                ? llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * s.width, s.width))
                                : zero(2 * s.width);
            b_.CreateAlignedStore(mm, field_ptr(state, s.off_mm), llvm::Align(field_align(2 * s.width)));
            b_.CreateAlignedStore(zero(s.width), field_ptr(state, s.off_z), llvm::Align(field_align(s.width)));
        }
    }

    // Pointer to a state field at a constant byte offset (a folded i8 GEP).
    llvm::Value* field_ptr(llvm::Value* base, uint32_t off) {
        return b_.CreateConstInBoundsGEP1_32(b_.getInt8Ty(), base, off);
    }

    // Lower a region's instructions in order into the current insert block.
    void lower_region(std::span<const Index> region) {
        for (Index i : region) {
            lower_inst(i);
        }
    }

    // -- Type inference (memoized) --

    ValType type_of(Index i) {
        const auto slot = static_cast<uint32_t>(i);
        if (types_[slot]) {
            return *types_[slot];
        }
        const ValType t = compute_type(i);
        types_[slot]    = t;
        return t;
    }

    ValType from_ref(Ref type) const {
        const Layout lay = mod_.pool.layout(type);
        const Type&  ty  = std::get<Type>(mod_.pool.get(type));
        return { lay.width == 0 ? 1 : lay.width, lay.domain == Domain::Four, ty.is_signed };
    }

    ValType compute_type(Index i) {
        const Inst::Payload& p = ir_->payload(i);
        switch (ir_->tag(i)) {
            case Inst::Tag::Constant:
                return from_ref(std::get<Value>(mod_.pool.get(p.ref)).type);
            case Inst::Tag::Arg:
                return from_ref(p.arg.type);
            case Inst::Tag::Resize:
                return from_ref(p.op_type.type);
            case Inst::Tag::Signal: {
                const SignalSlot& s = slot_of(p.signal);
                return { s.width, s.four_state, s.is_signed };
            }
            case Inst::Tag::BitSelect: {
                const ValType base = type_of(p.binary.l);
                return { 1, base.four_state, false };
            }
            case Inst::Tag::PartSelect: {
                const auto ps = ir_->extra_data<PartSelect>(p.op_extra.extra);
                return { ps.msb - ps.lsb + 1, type_of(p.op_extra.op).four_state, false };
            }
            case Inst::Tag::Load:
                return type_of(p.unary);

            case Inst::Tag::Add:
            case Inst::Tag::Sub:
            case Inst::Tag::Mul:
            case Inst::Tag::Band:
            case Inst::Tag::Bor:
            case Inst::Tag::Bxor:
            case Inst::Tag::Sll:
            case Inst::Tag::Srl:
            case Inst::Tag::Sra:
                return type_of(p.binary.l);
            case Inst::Tag::Bnot:
                return type_of(p.unary);
            case Inst::Tag::Mux:
                return type_of(ir_->extra_data<Mux>(p.op_extra.extra).a);

            case Inst::Tag::RedAnd:
            case Inst::Tag::RedOr:
            case Inst::Tag::RedXor:
            case Inst::Tag::Eq:
            case Inst::Tag::Ne:
            case Inst::Tag::Lt:
            case Inst::Tag::Le:
            case Inst::Tag::Gt:
            case Inst::Tag::Ge:
            case Inst::Tag::CaseEq:
            case Inst::Tag::CasezEq:
            case Inst::Tag::CasexEq:
            case Inst::Tag::Land:
            case Inst::Tag::Lor:
            case Inst::Tag::Lnot:
                return { 1, true, false };

            case Inst::Tag::Concat: {
                std::array<Index, 3> scratch;
                uint32_t             width = 0;
                bool                 four  = false;
                for (Index part : ir_->operands(i, scratch)) {
                    const ValType t = type_of(part);
                    width += t.width;
                    four = four || t.four_state;
                }
                return { width, four, false };
            }
            case Inst::Tag::Repl: {
                const ValType v = type_of(p.repl.value);
                return { v.width * p.repl.count, v.four_state, false };
            }

            case Inst::Tag::Store:
            case Inst::Tag::StoreNb:
            case Inst::Tag::Cond:
            case Inst::Tag::Loop:
            case Inst::Tag::Ret:
            case Inst::Tag::Delay:
            case Inst::Tag::Wait:
            case Inst::Tag::Display:
                return { 0, false, false };
        }
        return { 0, false, false };
    }

    // -- Value / lvalue access --

    Plane value_of(Index i) const {
        return values_[static_cast<uint32_t>(i)];
    }
    void set_value(Index i, Plane p) {
        values_[static_cast<uint32_t>(i)] = p;
    }

    const LValue& lvalue_of(Index i) const {
        return *lvals_[static_cast<uint32_t>(i)];
    }
    void set_lvalue(Index i, LValue lv) {
        lvals_[static_cast<uint32_t>(i)] = lv;
    }

    // -- Instruction dispatch --

    void lower_inst(Index i) {
        const Inst::Payload& p = ir_->payload(i);
        switch (ir_->tag(i)) {
            // Lvalue derivation: record a descriptor, emit no value.
            case Inst::Tag::Signal:
                set_lvalue(i, LValue{ p.signal, b_.getInt32(0), slot_of(p.signal).width });
                return;
            case Inst::Tag::BitSelect: {
                const LValue& base = lvalue_of(p.binary.l);
                // TODO: an X/Z or out-of-range index uses only its must bits,
                // silently selecting the wrong bit. LRM: a read with an unknown /
                // out-of-range index yields X; such a write is a no-op.
                llvm::Value* idx = cast(must_of(value_of(p.binary.r)), 32, false);
                set_lvalue(i, LValue{ base.sig, b_.CreateAdd(base.bit_off, idx), 1 });
                return;
            }
            case Inst::Tag::PartSelect: {
                const LValue& base = lvalue_of(p.op_extra.op);
                const auto    ps   = ir_->extra_data<PartSelect>(p.op_extra.extra);
                llvm::Value*  off  = b_.CreateAdd(base.bit_off, b_.getInt32(ps.lsb));
                set_lvalue(i, LValue{ base.sig, off, ps.msb - ps.lsb + 1 });
                return;
            }

            case Inst::Tag::Load:
                set_value(i, lower_load(lvalue_of(p.unary)));
                return;
            case Inst::Tag::Store: {
                // A blocking assignment stores, then services the target signal's
                // wait queue: any process waiting for an edge this store produced
                // is woken. Snapshot the old value first so the transition can be
                // detected from old -> new.
                const LValue& lv      = lvalue_of(p.binary.l);
                const bool    service = signal_has_waiters(lv.sig);
                llvm::Value*  old_mm  = service ? load_mm(lv.sig) : nullptr;
                lower_store(lv, value_of(p.binary.r), type_of(p.binary.r));
                if (service) {
                    service_wait_queue(lv.sig, old_mm, load_mm(lv.sig));
                }
                return;
            }
            // TODO(nonblocking): StoreNb is lowered identically to a blocking Store
            // — no two-phase next-state staging, and no wait-queue servicing — so
            // RAW-dependent NBAs in one process (e.g. `a <= b; b <= a;`) copy
            // instead of swapping.
            case Inst::Tag::StoreNb:
                lower_store(lvalue_of(p.binary.l), value_of(p.binary.r), type_of(p.binary.r));
                return;

            case Inst::Tag::Constant:
                set_value(i, lower_constant(p.ref));
                return;

            case Inst::Tag::Add:
            case Inst::Tag::Sub:
            case Inst::Tag::Mul:
                set_value(i, lower_arith(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;

            case Inst::Tag::Band:
            case Inst::Tag::Bor:
            case Inst::Tag::Bxor:
                set_value(i, lower_bitwise(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;
            case Inst::Tag::Bnot:
                set_value(i, lower_bnot(value_of(p.unary)));
                return;

            case Inst::Tag::RedAnd:
            case Inst::Tag::RedOr:
            case Inst::Tag::RedXor:
                set_value(i, lower_reduce(ir_->tag(i), value_of(p.unary)));
                return;

            case Inst::Tag::Sll:
            case Inst::Tag::Srl:
            case Inst::Tag::Sra:
                set_value(
                  i,
                  lower_shift(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r), type_of(p.binary.l).is_signed));
                return;

            case Inst::Tag::Eq:
            case Inst::Tag::Ne:
            case Inst::Tag::Lt:
            case Inst::Tag::Le:
            case Inst::Tag::Gt:
            case Inst::Tag::Ge:
                set_value(i,
                          lower_compare(
                            ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r), type_of(p.binary.l).is_signed));
                return;

            case Inst::Tag::CaseEq:
            case Inst::Tag::CasezEq:
            case Inst::Tag::CasexEq:
                set_value(i, lower_case_compare(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;

            case Inst::Tag::Land:
            case Inst::Tag::Lor:
                set_value(i, lower_logical(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;
            case Inst::Tag::Lnot:
                set_value(i, lower_lnot(value_of(p.unary)));
                return;

            case Inst::Tag::Mux: {
                const auto mux = ir_->extra_data<Mux>(p.op_extra.extra);
                set_value(i, lower_mux(value_of(p.op_extra.op), value_of(mux.a), value_of(mux.b)));
                return;
            }
            case Inst::Tag::Concat:
                set_value(i, lower_concat(i));
                return;
            case Inst::Tag::Repl:
                set_value(i, lower_repl(value_of(p.repl.value), type_of(p.repl.value).width, p.repl.count));
                return;
            case Inst::Tag::Resize:
                set_value(i, lower_resize(value_of(p.op_type.op), type_of(p.op_type.op), from_ref(p.op_type.type)));
                return;

            case Inst::Tag::Cond:
                lower_cond(i);
                return;

            case Inst::Tag::Loop:
                lower_loop(i);
                return;

            case Inst::Tag::Delay:
                lower_delay(p.unary);
                return;

            case Inst::Tag::Wait:
                lower_wait(p.wait.signal, p.wait.edge);
                return;

            case Inst::Tag::Display:
                lower_display(i);
                return;

            case Inst::Tag::Arg:
                throw BackendError{ "arg (function/task parameter) not supported by the process backend" };
            case Inst::Tag::Ret:
                throw BackendError{ "ret (function/task return) not supported by the process backend" };
        }
    }

    // -- Leaves --

    Plane lower_constant(Ref ref) {
        const Value&                   v     = std::get<Value>(mod_.pool.get(ref));
        const Layout                   lay   = mod_.pool.layout(v.type);
        const uint32_t                 w     = lay.width == 0 ? 1 : lay.width;
        const std::span<const uint8_t> bytes = mod_.pool.bytes(ref);
        const uint32_t                 pb    = plane_bytes(lay.width);

        // The pool stores canonical val/xz; ingest to must/may/z so 0/1/x/z stay
        // distinct (case-equality needs to tell a literal x from a literal z).
        // VPI: x=(val1,xz1), z=(val0,xz1) => must=val&~xz, may=val|xz, z=xz&~val.
        llvm::Value* val = const_from_bytes(bytes.subspan(0, pb), w);
        llvm::Value* xz  = lay.domain == Domain::Four ? const_from_bytes(bytes.subspan(pb, pb), w) : zero(w);
        return packed(b_.CreateAnd(val, b_.CreateNot(xz)), b_.CreateOr(val, xz), b_.CreateAnd(xz, b_.CreateNot(val)));
    }

    // Build an iW constant from little-endian canonical plane bytes.
    llvm::ConstantInt* const_from_bytes(std::span<const uint8_t> bytes, uint32_t width) {
        const unsigned        words = (width + 63) / 64;
        std::vector<uint64_t> buf(words, 0);
        std::memcpy(buf.data(), bytes.data(), std::min<size_t>(bytes.size(), words * 8));
        return llvm::ConstantInt::get(ctx_, llvm::APInt(width, llvm::ArrayRef<uint64_t>(buf.data(), words)));
    }

    // -- State access --

    Plane lower_load(const LValue& lv) {
        const SignalSlot& s = slot_of(lv.sig);
        llvm::Value*      mm = b_.CreateAlignedLoad(
          int_ty(2 * s.width), field_ptr(state_, s.off_mm), llvm::Align(field_align(2 * s.width)));
        llvm::Value* z = b_.CreateAlignedLoad(int_ty(s.width), field_ptr(state_, s.off_z), llvm::Align(field_align(s.width)));
        if (lv.width == s.width && is_zero_const(lv.bit_off)) {
            return { mm, z };
        }
        // Bit/part select: unpack, shift each plane down by the offset, re-pack.
        const Plane  full = { mm, z };
        llvm::Value* off  = cast(lv.bit_off, s.width, false);
        return packed(cast(b_.CreateLShr(must_of(full), off), lv.width, false),
                      cast(b_.CreateLShr(may_of(full), off), lv.width, false),
                      cast(b_.CreateLShr(z, off), lv.width, false));
    }

    void lower_store(const LValue& lv, Plane rhs, ValType rhs_ty) {
        const SignalSlot& s     = slot_of(lv.sig);
        llvm::Value*      mp     = field_ptr(state_, s.off_mm);
        llvm::Value*      zp     = field_ptr(state_, s.off_z);
        const llvm::Align mm_al  = llvm::Align(field_align(2 * s.width));
        const llvm::Align z_al   = llvm::Align(field_align(s.width));
        (void)rhs_ty;

        // Whole-signal store: the frontend context-sizes the RHS to the lvalue
        // width (via an inserted Resize), so rhs already matches the signal width
        // and the planes store directly.
        if (lv.width == s.width && is_zero_const(lv.bit_off)) {
            b_.CreateAlignedStore(rhs.mm, mp, mm_al);
            b_.CreateAlignedStore(rhs.z, zp, z_al);
            return;
        }

        // Partial store: read-modify-write each plane at the signal width.
        const Plane  old =
          { b_.CreateAlignedLoad(int_ty(2 * s.width), mp, mm_al), b_.CreateAlignedLoad(int_ty(s.width), zp, z_al) };
        llvm::Value* off   = cast(lv.bit_off, s.width, false);
        llvm::Value* mask  = b_.CreateShl(cast(ones(lv.width), s.width, false), off);
        llvm::Value* nmask = b_.CreateNot(mask);
        llvm::Value* must  = merge_bits(must_of(old), cast(must_of(rhs), s.width, false), off, mask, nmask);
        llvm::Value* may   = merge_bits(may_of(old), cast(may_of(rhs), s.width, false), off, mask, nmask);
        llvm::Value* z     = merge_bits(old.z, cast(rhs.z, s.width, false), off, mask, nmask);
        b_.CreateAlignedStore(packed(must, may, z).mm, mp, mm_al);
        b_.CreateAlignedStore(z, zp, z_al);
    }

    // (old & ~mask) | ((ins << off) & mask)
    llvm::Value* merge_bits(llvm::Value* old, llvm::Value* ins, llvm::Value* off, llvm::Value* mask,
                            llvm::Value* nmask) {
        llvm::Value* shifted = b_.CreateAnd(b_.CreateShl(ins, off), mask);
        return b_.CreateOr(b_.CreateAnd(old, nmask), shifted);
    }

    static bool is_zero_const(llvm::Value* v) {
        auto* c = llvm::dyn_cast<llvm::ConstantInt>(v);
        return c != nullptr && c->isZero();
    }

    // -- System tasks --

    // The runtime argument descriptor, mirroring `rt_arg` in runtime/Runtime.h:
    //   { i32 width, i32 flags, i64 val, i64 xz, i64 z }
    // val/xz/z carry the low 64 bits of each plane (wider values are truncated).
    llvm::StructType* arg_struct_ty() {
        return llvm::StructType::get(
          ctx_, { b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty() });
    }

    // void rt_display(const char* fmt, i32 argc, const rt_arg* argv)
    llvm::FunctionCallee display_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, b_.getInt32Ty(), ptr }, false);
        return llmod_.getOrInsertFunction("rt_display", fnty);
    }

    // Lower a $display / $write: stack-allocate an rt_arg[] descriptor array, pack
    // each value operand's planes into it, and call the runtime formatter.
    // TODO: each operand's planes are truncated to the low 64 bits (rt_arg is
    // i64), so values wider than 64 bits print silently wrong. Pass full width.
    void lower_display(Index i) {
        const auto  info = ir_->extra_data<DisplayInfo>(ir_->payload(i).display);
        std::string fmt  = ir_->extra_bytes(info.fmt, info.fmt_len);
        if (info.newline) {
            fmt += '\n';
        }

        const std::span<const Index> args = ir_->region(info.args);
        const auto                   n    = static_cast<uint32_t>(args.size());

        llvm::StructType* sty = arg_struct_ty();
        llvm::Value*      arr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        if (n > 0) {
            arr = b_.CreateAlloca(sty, b_.getInt32(n), "display_args");
            for (uint32_t k = 0; k < n; ++k) {
                const Plane   p    = value_of(args[k]);
                const ValType t    = type_of(args[k]);
                llvm::Value*  must = must_of(p);
                llvm::Value*  may  = may_of(p);
                // xz = must ^ may marks every unknown bit (X and Z); z marks the Z
                // subset. The runtime distinguishes them per bit.
                llvm::Value* xz  = b_.CreateXor(must, may);
                llvm::Value* elt = b_.CreateConstInBoundsGEP1_32(sty, arr, k);
                store_field(sty, elt, 0, b_.getInt32(t.width));
                store_field(sty, elt, 1, b_.getInt32(t.is_signed ? 1u : 0u));
                store_field(sty, elt, 2, cast(must, 64, false));
                store_field(sty, elt, 3, cast(xz, 64, false));
                store_field(sty, elt, 4, cast(p.z, 64, false));
            }
        }

        llvm::Value* fmt_ptr = b_.CreateGlobalString(fmt);
        b_.CreateCall(display_fn(), { fmt_ptr, b_.getInt32(n), arr });
    }

    void store_field(llvm::StructType* sty, llvm::Value* elt, unsigned idx, llvm::Value* v) {
        b_.CreateStore(v, b_.CreateStructGEP(sty, elt, idx));
    }

    // -- Plane helpers --

    static uint32_t bits_of(llvm::Value* v) {
        return llvm::cast<llvm::IntegerType>(v->getType())->getBitWidth();
    }
    // Value width W of a packed plane (mm is i(2W)).
    static uint32_t width_of(Plane a) {
        return bits_of(a.mm) / 2;
    }

    // The must (low W) and may (high W) halves of a packed plane.
    llvm::Value* must_of(Plane a) {
        return b_.CreateTrunc(a.mm, int_ty(width_of(a)));
    }
    llvm::Value* may_of(Plane a) {
        const uint32_t w = width_of(a);
        return b_.CreateTrunc(b_.CreateLShr(a.mm, w), int_ty(w));
    }

    // Pack iW must + iW may (+ iW z) into a Plane (mm is i(2W)).
    Plane packed(llvm::Value* must, llvm::Value* may, llvm::Value* z) {
        const uint32_t w    = bits_of(must);
        auto*          wide = int_ty(2 * w);
        llvm::Value*   mm   = b_.CreateOr(b_.CreateZExt(must, wide), b_.CreateShl(b_.CreateZExt(may, wide), w));
        return { mm, z };
    }

    // i1: does this value contain any X/Z bit? Unknown bits are exactly must!=may.
    llvm::Value* any_unknown(Plane a) {
        llvm::Value* u = b_.CreateXor(must_of(a), may_of(a));
        return b_.CreateICmpNE(u, llvm::ConstantInt::get(u->getType(), 0));
    }

    // Pack an i1 must + i1 may into a width-1 Plane (z cleared).
    Plane bit_plane(llvm::Value* must_i1, llvm::Value* may_i1) {
        return packed(must_i1, may_i1, zero(1));
    }

    // Cast all planes to width `to`. No-op when already that width — avoids a
    // pointless unpack/repack of the packed mm.
    Plane cast_plane(Plane a, uint32_t to, bool sign) {
        if (width_of(a) == to) {
            return a;
        }
        return packed(cast(must_of(a), to, sign), cast(may_of(a), to, sign), cast(a.z, to, sign));
    }

    // -- Arithmetic: any X/Z in an operand makes the whole result X. --
    //
    // Lowered as a branch, not a select: the X case is rare, so it is split into a
    // cold block that just materialises the all-X constant (one move-immediate),
    // while the hot block does a single W-bit add/sub/mul of the value and
    // duplicates it across both planes (known bits have must == may).

    Plane lower_arith(Inst::Tag tag, Plane a, Plane b) {
        const uint32_t w    = width_of(a);
        auto*          wide = int_ty(2 * w);

        // Any X/Z bit in either operand, tested once for the pair. The per-bit X
        // mask of a packed value is `must ^ may`, which sits in the low W bits of
        // `mm ^ (mm >> W)`. OR the two operands' masks and do a single nonzero
        // test instead of checking a and b separately. (The values can't be merged
        // before this step: X-ness is nonlinear, so OR-ing mm first is wrong.)
        llvm::Value* xa      = b_.CreateXor(a.mm, b_.CreateLShr(a.mm, w));
        llvm::Value* xb      = b_.CreateXor(b.mm, b_.CreateLShr(b.mm, w));
        llvm::Value* xmask   = b_.CreateTrunc(b_.CreateOr(xa, xb), int_ty(w));
        llvm::Value* unknown = b_.CreateICmpNE(xmask, zero(w));

        auto* x_bb   = llvm::BasicBlock::Create(ctx_, "arith.x", fn_);
        auto* val_bb = llvm::BasicBlock::Create(ctx_, "arith.val", fn_);
        auto* end_bb = llvm::BasicBlock::Create(ctx_, "arith.end", fn_);

        llvm::BranchInst* br = b_.CreateCondBr(unknown, x_bb, val_bb);
        br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

        // Cold: whole result is X = every bit (must 0, may 1) = the high-half ones.
        b_.SetInsertPoint(x_bb);
        llvm::Value* x_mm = llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * w, w));
        b_.CreateBr(end_bb);

        // Hot: one W-bit op on the value, duplicated into both planes.
        b_.SetInsertPoint(val_bb);
        llvm::Value* am = must_of(a);
        llvm::Value* bm = must_of(b);
        llvm::Value* r  = nullptr;
        switch (tag) {
            case Inst::Tag::Add:
                r = b_.CreateAdd(am, bm);
                break;
            case Inst::Tag::Sub:
                r = b_.CreateSub(am, bm);
                break;
            default:
                r = b_.CreateMul(am, bm);
                break;
        }
        llvm::Value* val_mm = packed(r, r, zero(w)).mm;
        b_.CreateBr(end_bb);

        b_.SetInsertPoint(end_bb);
        llvm::PHINode* mm = b_.CreatePHI(wide, 2);
        mm->addIncoming(x_mm, x_bb);
        mm->addIncoming(val_mm, val_bb);
        return { mm, zero(w) };
    }

    // -- Bitwise: AND/OR are monotone, so one op on the packed planes does both;
    // the result is never Z, so z is cleared. --

    Plane lower_bitwise(Inst::Tag tag, Plane a, Plane b) {
        const uint32_t w = width_of(a);
        if (tag == Inst::Tag::Band) {
            return { b_.CreateAnd(a.mm, b.mm), zero(w) };
        }
        if (tag == Inst::Tag::Bor) {
            return { b_.CreateOr(a.mm, b.mm), zero(w) };
        }
        // Bxor (non-monotone): unpack. Unknown iff either bit unknown; value is xor.
        llvm::Value* am = must_of(a);
        llvm::Value* aM = may_of(a);
        llvm::Value* bm = must_of(b);
        llvm::Value* bM = may_of(b);
        llvm::Value* u  = b_.CreateOr(b_.CreateXor(am, aM), b_.CreateXor(bm, bM));
        llvm::Value* k  = b_.CreateXor(am, bm);
        return packed(b_.CreateAnd(b_.CreateNot(u), k), b_.CreateOr(u, k), zero(w));
    }

    Plane lower_bnot(Plane a) {
        // NOT is must' = ~may, may' = ~must: complement both planes and swap them.
        // Complement is one full ~ on the packed value; the swap is a rotate-left
        // by W (the two halves trade places), emitted as a funnel shift. NOT and
        // rotate commute, so order does not matter.
        const uint32_t w       = width_of(a);
        auto*          wide    = int_ty(2 * w);
        llvm::Value*   notted  = b_.CreateNot(a.mm);
        llvm::Value*   amount  = llvm::ConstantInt::get(wide, w);
        llvm::Value*   swapped = b_.CreateIntrinsic(llvm::Intrinsic::fshl, { wide }, { notted, notted, amount });
        return { swapped, zero(w) };
    }

    // -- Reductions -> 1-bit (chained AND/OR/XOR over the bits). --

    Plane lower_reduce(Inst::Tag tag, Plane a) {
        const uint32_t w  = width_of(a);
        llvm::Value*   am = must_of(a);
        llvm::Value*   aM = may_of(a);
        if (tag == Inst::Tag::RedXor) {
            llvm::Value* unknown = any_unknown(a);
            llvm::Value* pop     = b_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, am);
            llvm::Value* parity  = b_.CreateTrunc(pop, int_ty(1));
            return bit_plane(b_.CreateAnd(b_.CreateNot(unknown), parity), b_.CreateOr(unknown, parity));
        }
        if (tag == Inst::Tag::RedAnd) {
            // must: all bits definitely 1; may: all bits could be 1.
            return bit_plane(b_.CreateICmpEQ(am, ones(w)), b_.CreateICmpEQ(aM, ones(w)));
        }
        // RedOr: must: any bit definitely 1; may: any bit could be 1.
        return bit_plane(b_.CreateICmpNE(am, zero(w)), b_.CreateICmpNE(aM, zero(w)));
    }

    // -- Shifts: X/Z in the count makes the whole result X; value planes shift. --

    Plane lower_shift(Inst::Tag tag, Plane a, Plane amt, bool sign) {
        const uint32_t w  = width_of(a);
        llvm::Value*   sh = cast(must_of(amt), w, false);
        llvm::Value*   am = must_of(a);
        llvm::Value*   aM = may_of(a);
        llvm::Value*   must;
        llvm::Value*   may;
        llvm::Value*   z;
        switch (tag) {
            case Inst::Tag::Sll:
                must = b_.CreateShl(am, sh);
                may  = b_.CreateShl(aM, sh);
                z    = b_.CreateShl(a.z, sh);
                break;
            case Inst::Tag::Srl:
                must = b_.CreateLShr(am, sh);
                may  = b_.CreateLShr(aM, sh);
                z    = b_.CreateLShr(a.z, sh);
                break;
            default: // Sra: arithmetic, replicate the sign (and its unknown-ness)
                must = sign ? b_.CreateAShr(am, sh) : b_.CreateLShr(am, sh);
                may  = sign ? b_.CreateAShr(aM, sh) : b_.CreateLShr(aM, sh);
                z    = sign ? b_.CreateAShr(a.z, sh) : b_.CreateLShr(a.z, sh);
                break;
        }
        llvm::Value* unknown = any_unknown(amt);
        return packed(b_.CreateSelect(unknown, zero(w), must),
                      b_.CreateSelect(unknown, ones(w), may),
                      b_.CreateSelect(unknown, zero(w), z));
    }

    // -- Comparisons -> 1-bit; X/Z in either operand yields X. --

    Plane lower_compare(Inst::Tag tag, Plane a, Plane b, bool sign) {
        llvm::CmpInst::Predicate pred;
        switch (tag) {
            case Inst::Tag::Eq:
                pred = llvm::CmpInst::ICMP_EQ;
                break;
            case Inst::Tag::Ne:
                pred = llvm::CmpInst::ICMP_NE;
                break;
            case Inst::Tag::Lt:
                pred = sign ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_ULT;
                break;
            case Inst::Tag::Le:
                pred = sign ? llvm::CmpInst::ICMP_SLE : llvm::CmpInst::ICMP_ULE;
                break;
            case Inst::Tag::Gt:
                pred = sign ? llvm::CmpInst::ICMP_SGT : llvm::CmpInst::ICMP_UGT;
                break;
            default:
                pred = sign ? llvm::CmpInst::ICMP_SGE : llvm::CmpInst::ICMP_UGE;
                break;
        }
        llvm::Value* cmp     = b_.CreateICmp(pred, must_of(a), must_of(b));
        llvm::Value* unknown = b_.CreateOr(any_unknown(a), any_unknown(b));
        return bit_plane(b_.CreateAnd(b_.CreateNot(unknown), cmp), b_.CreateOr(unknown, cmp));
    }

    // -- Case-equality (SV `case`/`casez`/`casex`) -> definite 1-bit. --
    //
    // Unlike `lower_compare`, the result is always known (must == may): the case
    // statement compares 4-state planes bit-exactly rather than propagating X.
    //   CaseEq  (===): every bit of must/may/z must be equal.
    //   CasezEq:  z bits in either operand are don't-care (mask = z_a | z_b).
    //   CasexEq:  x and z bits in either operand are don't-care
    //             (mask = unknown_a | unknown_b, since both x and z have must!=may).
    Plane lower_case_compare(Inst::Tag tag, Plane a, Plane b) {
        const uint32_t w  = std::max(width_of(a), width_of(b));
        a                 = cast_plane(a, w, false);
        b                 = cast_plane(b, w, false);
        llvm::Value* am   = must_of(a);
        llvm::Value* aM   = may_of(a);
        llvm::Value* bm   = must_of(b);
        llvm::Value* bM   = may_of(b);

        llvm::Value* eq;
        if (tag == Inst::Tag::CaseEq) {
            eq = b_.CreateAnd(b_.CreateAnd(b_.CreateICmpEQ(am, bm), b_.CreateICmpEQ(aM, bM)),
                              b_.CreateICmpEQ(a.z, b.z));
        } else {
            llvm::Value* dc = tag == Inst::Tag::CasezEq
                                ? b_.CreateOr(a.z, b.z)
                                : b_.CreateOr(b_.CreateXor(am, aM), b_.CreateXor(bm, bM));
            llvm::Value* keep = b_.CreateNot(dc);
            eq                = b_.CreateAnd(
              b_.CreateICmpEQ(b_.CreateAnd(am, keep), b_.CreateAnd(bm, keep)),
              b_.CreateICmpEQ(b_.CreateAnd(aM, keep), b_.CreateAnd(bM, keep)));
        }
        return bit_plane(eq, eq);
    }

    // -- Logical and/or/not -> 1-bit, three-valued. --

    // Per-operand truth (i1): definitely-true (some must bit) / definitely-false
    // (no may bit). Unknown is neither.
    struct Truth {
        llvm::Value* t;
        llvm::Value* f;
    };
    Truth truth_of(Plane a) {
        const uint32_t w = width_of(a);
        llvm::Value*   t = b_.CreateICmpNE(must_of(a), zero(w));
        llvm::Value*   f = b_.CreateICmpEQ(may_of(a), zero(w));
        return { t, f };
    }

    Plane lower_logical(Inst::Tag tag, Plane a, Plane b) {
        const Truth ta = truth_of(a);
        const Truth tb = truth_of(b);
        if (tag == Inst::Tag::Land) {
            // must = both true; may = not (either false).
            return bit_plane(b_.CreateAnd(ta.t, tb.t), b_.CreateNot(b_.CreateOr(ta.f, tb.f)));
        }
        // Lor: must = either true; may = not (both false).
        return bit_plane(b_.CreateOr(ta.t, tb.t), b_.CreateNot(b_.CreateAnd(ta.f, tb.f)));
    }

    Plane lower_lnot(Plane a) {
        const Truth t = truth_of(a);
        // true -> 0, false -> 1, x -> x: must = was-false, may = not was-true.
        return bit_plane(t.f, b_.CreateNot(t.t));
    }

    // -- Mux: known condition selects (one packed select); unknown merges bitwise
    // (X where the arms disagree). --

    Plane lower_mux(Plane cond, Plane a, Plane b) {
        const uint32_t w          = width_of(a);
        llvm::Value*   cond_known = b_.CreateICmpEQ(must_of(cond), may_of(cond));
        llvm::Value*   cond_true  = b_.CreateICmpNE(must_of(cond), zero(1));

        llvm::Value* sel_mm = b_.CreateSelect(cond_true, a.mm, b.mm);
        llvm::Value* sel_z  = b_.CreateSelect(cond_true, a.z, b.z);

        // Unknown condition: interval hull per bit. Low half (must) ANDs, high half
        // (may) ORs; a low-bit mask keeps each half's result.
        llvm::Value* lowmask = llvm::ConstantInt::get(ctx_, llvm::APInt::getLowBitsSet(2 * w, w));
        llvm::Value* merge   = b_.CreateOr(b_.CreateAnd(b_.CreateAnd(a.mm, b.mm), lowmask),
                                           b_.CreateAnd(b_.CreateOr(a.mm, b.mm), b_.CreateNot(lowmask)));

        return { b_.CreateSelect(cond_known, sel_mm, merge), b_.CreateSelect(cond_known, sel_z, zero(w)) };
    }

    // -- Concat / Repl: first operand occupies the most-significant bits. --

    Plane lower_concat(Index i) {
        const uint32_t       w = type_of(i).width;
        std::array<Index, 3> scratch;
        const auto           parts = ir_->operands(i, scratch);

        llvm::Value* must = zero(w);
        llvm::Value* may  = zero(w);
        llvm::Value* z    = zero(w);
        for (Index part : parts) {
            const uint32_t pw = type_of(part).width;
            const Plane    pv = value_of(part);
            must              = b_.CreateOr(b_.CreateShl(must, pw), cast(must_of(pv), w, false));
            may               = b_.CreateOr(b_.CreateShl(may, pw), cast(may_of(pv), w, false));
            z                 = b_.CreateOr(b_.CreateShl(z, pw), cast(pv.z, w, false));
        }
        return packed(must, may, z);
    }

    Plane lower_repl(Plane v, uint32_t part_width, uint32_t count) {
        // TODO: a zero replication count ({0{x}}) should yield a 0-width value;
        // here w becomes 0 and int_ty(0) is widened to i1, so it leaks one bit.
        const uint32_t w    = part_width * count;
        llvm::Value*   pm   = cast(must_of(v), w, false);
        llvm::Value*   pM   = cast(may_of(v), w, false);
        llvm::Value*   pz   = cast(v.z, w, false);
        llvm::Value*   must = zero(w);
        llvm::Value*   may  = zero(w);
        llvm::Value*   z    = zero(w);
        for (uint32_t c = 0; c < count; ++c) {
            must = b_.CreateOr(b_.CreateShl(must, part_width), pm);
            may  = b_.CreateOr(b_.CreateShl(may, part_width), pM);
            z    = b_.CreateOr(b_.CreateShl(z, part_width), pz);
        }
        return packed(must, may, z);
    }

    // -- Resize: truncate or extend all planes (sign per target). --

    Plane lower_resize(Plane v, ValType from, ValType to) {
        (void)from;
        return cast_plane(v, to.width, to.is_signed);
    }

    // -- Control flow: if/else with two regions. X/Z condition takes else. --

    void lower_cond(Index i) {
        const Inst::Payload& p    = ir_->payload(i);
        const auto           regs = ir_->extra_data<Regions>(p.op_extra.extra);
        const Plane          cond = value_of(p.op_extra.op);

        // must=1 holds only for a definite 1 (invariant must => may), so X/Z fall
        // through to else automatically.
        llvm::Value* take = b_.CreateICmpNE(must_of(cond), zero(1));

        auto* then_bb = llvm::BasicBlock::Create(ctx_, "then", fn_);
        auto* else_bb = llvm::BasicBlock::Create(ctx_, "else", fn_);
        auto* cont_bb = llvm::BasicBlock::Create(ctx_, "endif", fn_);
        b_.CreateCondBr(take, then_bb, else_bb);

        b_.SetInsertPoint(then_bb);
        lower_region(ir_->region(regs.then_));
        b_.CreateBr(cont_bb);

        b_.SetInsertPoint(else_bb);
        lower_region(ir_->region(regs.else_));
        b_.CreateBr(cont_bb);

        b_.SetInsertPoint(cont_bb);
    }

    // -- Control flow: for/while/forever. The cond region recomputes the
    // predicate each iteration; X/Z exits the loop (same definite-1 rule as
    // lower_cond). State lives in memory, so the back-edge needs no PHIs. --

    void lower_loop(Index i) {
        const Inst::Payload& p  = ir_->payload(i);
        const auto           lp = ir_->extra_data<LoopInfo>(p.extra);

        auto* head_bb = llvm::BasicBlock::Create(ctx_, "loop.head", fn_);
        auto* body_bb = llvm::BasicBlock::Create(ctx_, "loop.body", fn_);
        auto* step_bb = llvm::BasicBlock::Create(ctx_, "loop.step", fn_);
        auto* exit_bb = llvm::BasicBlock::Create(ctx_, "loop.exit", fn_);

        b_.CreateBr(head_bb);

        // Header: recompute the predicate, then test it. must=1 only for a
        // definite 1, so an X/Z condition exits the loop.
        b_.SetInsertPoint(head_bb);
        lower_region(ir_->region(lp.cond));
        llvm::Value* take = b_.CreateICmpNE(must_of(value_of(lp.pred)), zero(1));
        b_.CreateCondBr(take, body_bb, exit_bb);

        b_.SetInsertPoint(body_bb);
        lower_region(ir_->region(lp.body));
        b_.CreateBr(step_bb);

        // Step, then the back-edge to re-test the condition.
        b_.SetInsertPoint(step_bb);
        lower_region(ir_->region(lp.step));
        b_.CreateBr(head_bb);

        b_.SetInsertPoint(exit_bb);
    }

    // -- State --

    const Module&           mod_;
    llvm::LLVMContext&      ctx_;
    llvm::Module&           llmod_;
    llvm::IRBuilder<>       b_;
    // Total size of the opaque state buffer in bytes (min 1, rounded up to 8).
    uint32_t                state_bytes_ = 1;
    std::vector<SignalSlot> slots_;

    // Per-process scratch, reset in lower_process.
    const Ir*           ir_         = nullptr;
    llvm::Function*     fn_         = nullptr;
    llvm::Value*        state_      = nullptr;
    llvm::Value*        wheel_      = nullptr;
    llvm::Value*        hdl_        = nullptr;
    llvm::Instruction*  coro_begin_ = nullptr;
    llvm::AllocaInst*   awaiter_    = nullptr;
    llvm::BasicBlock*   coro_cleanup_bb_ = nullptr;
    llvm::BasicBlock*   coro_ret_bb_     = nullptr;
    // &clotho.proc_sched[this process], set per process in lower_process.
    llvm::Constant*     sched_ptr_       = nullptr;
    // The shared timer callback (pushes a woken handle onto the active queue) and
    // the active queue globals, all built once per module.
    llvm::Function*                     enqueue_fn_   = nullptr;
    llvm::GlobalVariable*               queue_gv_     = nullptr;
    llvm::GlobalVariable*               top_gv_       = nullptr;
    llvm::ArrayType*                    queue_arr_ty_ = nullptr;
    // Per-signal wait queues, indexed by Signal handle; null where the signal has
    // no waiter of that kind. Static (.sta): fixed-capacity persistent
    // subscriptions, `wait_sta_cap_` the compile-time trip count, no length.
    // Dynamic (.dyn): one-shot waiters with `wait_dyn_len_gv_` live count. Built in
    // build_wait_queues; serviced by service_static_queue / service_dynamic_queue.
    std::vector<llvm::GlobalVariable*>  wait_sta_gv_;
    std::vector<llvm::ArrayType*>       wait_sta_ty_;
    std::vector<uint32_t>               wait_sta_cap_;
    std::vector<llvm::GlobalVariable*>  wait_dyn_gv_;
    std::vector<llvm::ArrayType*>       wait_dyn_ty_;
    std::vector<llvm::GlobalVariable*>  wait_dyn_len_gv_;
    // Next free static slot per signal, handed out during lowering (parallels the
    // capacities computed in build_wait_queues).
    std::vector<uint32_t>               sta_next_;
    // Per-process scheduled flags (clotho.proc_sched), built once in run().
    llvm::GlobalVariable*               sched_gv_     = nullptr;
    llvm::ArrayType*                    sched_arr_ty_ = nullptr;
    std::vector<Plane>                  values_;
    std::vector<std::optional<LValue>>  lvals_;
    std::vector<std::optional<ValType>> types_;
};

// Lower the coroutine intrinsics emitted for each process. CoroSplit (with
// CoroEarly/CoroCleanup) is mandatory: without it the llvm.coro.* intrinsics
// survive to codegen and the module won't compile. The mem2reg/sroa/simplifycfg
// cleanup shrinks the frame, and inline + coro-elide stack-allocate the frame for
// processes that never suspend, collapsing the coroutine back to straight-line
// code. Returns false (reason on errs()) only if the pipeline string is rejected.
bool lower_coroutines(llvm::Module& m) {
    llvm::PassBuilder         pb;
    llvm::LoopAnalysisManager     lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager    cgam;
    llvm::ModuleAnalysisManager   mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    llvm::ModulePassManager mpm;
    if (auto err = pb.parsePassPipeline(mpm,
                                        "coro-early,"
                                        "function(mem2reg,sroa,simplifycfg),"
                                        "cgscc(inline,coro-split,coro-annotation-elide),"
                                        "function(coro-elide,simplifycfg),"
                                        "coro-cleanup")) {
        llvm::errs() << "llvm backend: bad coroutine pipeline: " << llvm::toString(std::move(err)) << "\n";
        return false;
    }
    mpm.run(m, mam);
    return true;
}

} // namespace

std::unique_ptr<llvm::Module> build_module(const Module& m, llvm::LLVMContext& ctx, bool optimize) {
    auto llmod = std::make_unique<llvm::Module>(m.name.empty() ? "clotho" : m.name, ctx);

    try {
        Backend(m, ctx, *llmod).run();
    } catch (const BackendError& e) {
        llvm::errs() << "llvm backend error: " << e.message << "\n";
        return nullptr;
    }

    // Verify the freshly-built (pre-split) module. With optimize off, stop here
    // and hand back this raw module (coro intrinsics intact) for inspection.
    if (llvm::verifyModule(*llmod, &llvm::errs())) {
        return nullptr;
    }
    if (!optimize) {
        return llmod;
    }
    // Lower the coroutines and verify again — the second pass guards against
    // malformed coro lowering.
    if (!lower_coroutines(*llmod)) {
        return nullptr;
    }
    if (llvm::verifyModule(*llmod, &llvm::errs())) {
        return nullptr;
    }

    return llmod;
}

bool emit_llvm(const Module& m, llvm::raw_ostream& os, bool optimize) {
    llvm::LLVMContext ctx;
    auto              llmod = build_module(m, ctx, optimize);
    if (!llmod) {
        return false;
    }

    llmod->print(os, nullptr);
    return true;
}

} // namespace clotho::ir
