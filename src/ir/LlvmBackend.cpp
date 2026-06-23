#include "ir/LlvmBackend.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

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

#include "ir/InternPool.h"
#include "ir/Ir.h"

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
// State / ABI. Signals are persistent module state. We build one LLVM struct
// with three iW fields per signal (must, may, z) and hand every process function
// a pointer to it; Load reads those fields, Store writes them.
//
// Inferred types. The IR stores no per-instruction result type, so the backend
// recomputes width / domain / signedness for each value by walking operands
// (the frontend already inserted Resize nodes to width-match binary operands).
//
// Scope (initial). Nonblocking assignment is lowered identically to blocking
// (no two-phase next-state staging yet); clock-edge / sensitivity gating is left
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

// Per-signal placement in the state struct plus its cached layout facts.
struct SignalSlot {
    // Index of this signal's field in the state struct.
    unsigned field = 0;
    // The signal's value type: a `{ i(2W) must|may, iW z }` struct (e.g. %logic.8).
    llvm::StructType* ty         = nullptr;
    uint32_t          width      = 0;
    bool              four_state = false;
    bool              is_signed  = false;
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
        build_state_struct();
        std::vector<llvm::Function*> initials;
        for (const Process& proc : mod_.processes()) {
            llvm::Function* f = lower_process(proc);
            if (proc.kind == ProcessKind::Initial) {
                initials.push_back(f);
            }
        }
        emit_main(initials);
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

    // -- Signal value types --

    // The named, deduplicated `{ i(2W) must|may, iW z }` struct for a signal type.
    // 4-state types are %logic.<width>, 2-state %bit.<width>; each is emitted at
    // most once and only because a signal uses it.
    llvm::StructType* value_type(Ref type) {
        const Layout      lay  = mod_.pool.layout(type);
        const uint32_t    w    = lay.width == 0 ? 1 : lay.width;
        const bool        four = lay.domain == Domain::Four;
        const std::string name = (four ? "logic." : "bit.") + std::to_string(w);

        const auto it = value_types_.find(name);
        if (it != value_types_.end()) {
            return it->second;
        }
        auto* st           = llvm::StructType::create(ctx_, { int_ty(2 * w), int_ty(w) }, name);
        value_types_[name] = st;
        return st;
    }

    // -- State struct --

    void build_state_struct() {
        const auto               sigs = mod_.signals();
        std::vector<llvm::Type*> fields;
        slots_.reserve(sigs.size());
        for (const SignalInfo& s : sigs) {
            const Layout lay = mod_.pool.layout(s.type);
            const Type&  ty  = std::get<Type>(mod_.pool.get(s.type));

            SignalSlot slot;
            slot.width      = lay.width == 0 ? 1 : lay.width;
            slot.four_state = lay.domain == Domain::Four;
            slot.is_signed  = ty.is_signed;
            slot.ty         = value_type(s.type);
            slot.field      = static_cast<unsigned>(fields.size());
            fields.push_back(slot.ty);
            slots_.push_back(slot);
        }

        state_ty_ = llvm::StructType::create(ctx_, fields, mod_.name + ".state");
    }

    const SignalSlot& slot_of(Signal s) const {
        return slots_[static_cast<uint32_t>(s)];
    }

    // -- Per-process lowering --

    llvm::Function* lower_process(const Process& proc) {
        ir_            = &proc.ir;
        const size_t n = ir_->inst_count();
        values_.assign(n, Plane{});
        lvals_.assign(n, std::optional<LValue>{});
        types_.assign(n, std::optional<ValType>{});

        // Name a process by the source byte offset of the AST node that produced
        // it: @<module>.<offset>. The IR's SourceLoc packs {buffer:28|offset:36};
        // the low 36 bits are that byte offset.
        constexpr uint64_t offset_mask = (uint64_t{ 1 } << 36) - 1;
        const uint64_t     offset      = static_cast<uint64_t>(proc.loc) & offset_mask;
        const std::string  name        = mod_.name + "." + std::to_string(offset);
        auto*              fnty =
          llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), { llvm::PointerType::getUnqual(ctx_) }, false);
        fn_    = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, name, llmod_);
        state_ = fn_->getArg(0);
        state_->setName("state");
        // The state pointer is the sole memory the process touches; noalias lets
        // the allocator rematerialize signal loads instead of spilling them.
        fn_->addParamAttr(0, llvm::Attribute::NoAlias);

        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
        b_.SetInsertPoint(entry);

        lower_region(ir_->root());
        b_.CreateRetVoid();
        return fn_;
    }

    // -- Top-level entry --
    //
    // Emit `i32 @main()`: stack-allocate the shared state, power it up, then call
    // every initial block in process-table order (initial blocks are unordered, so
    // any order is legal). Returns 0. Suspension / scheduling are out of scope —
    // each callee runs straight through.
    void emit_main(const std::vector<llvm::Function*>& initials) {
        auto* i32  = llvm::Type::getInt32Ty(ctx_);
        auto* fnty = llvm::FunctionType::get(i32, false);
        auto* main = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, "main", llmod_);

        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", main);
        b_.SetInsertPoint(entry);

        llvm::Value* state = b_.CreateAlloca(state_ty_, nullptr, "state");
        init_state(state);
        for (llvm::Function* f : initials) {
            b_.CreateCall(f, { state });
        }
        b_.CreateRet(llvm::ConstantInt::get(i32, 0));
    }

    // Power-up the state struct: 4-state signals come up X (must 0, may 1, z 0),
    // 2-state signals come up 0.
    void init_state(llvm::Value* state) {
        for (const SignalSlot& s : slots_) {
            llvm::Value* sp = b_.CreateStructGEP(state_ty_, state, s.field);
            llvm::Value* mp = b_.CreateStructGEP(s.ty, sp, 0);
            llvm::Value* zp = b_.CreateStructGEP(s.ty, sp, 1);
            // X packs as the high W bits of the i(2W) must|may word (may=1, must=0).
            llvm::Value* mm = s.four_state
                                ? llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * s.width, s.width))
                                : zero(2 * s.width);
            b_.CreateStore(mm, mp);
            b_.CreateStore(zero(s.width), zp);
        }
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
            case Inst::Tag::Ret:
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
                llvm::Value*  idx  = cast(must_of(value_of(p.binary.r)), 32, false);
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
            case Inst::Tag::Store:
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

        // The pool stores canonical val/xz; ingest to must/may (constants never z).
        llvm::Value* val = const_from_bytes(bytes.subspan(0, pb), w);
        llvm::Value* xz  = lay.domain == Domain::Four ? const_from_bytes(bytes.subspan(pb, pb), w) : zero(w);
        return packed(b_.CreateAnd(val, b_.CreateNot(xz)), b_.CreateOr(val, xz), zero(w));
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
        const SignalSlot& s  = slot_of(lv.sig);
        llvm::Value*      sp = b_.CreateStructGEP(state_ty_, state_, s.field);
        llvm::Value*      mm = b_.CreateLoad(int_ty(2 * s.width), b_.CreateStructGEP(s.ty, sp, 0));
        llvm::Value*      z  = b_.CreateLoad(int_ty(s.width), b_.CreateStructGEP(s.ty, sp, 1));
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
        const SignalSlot& s  = slot_of(lv.sig);
        llvm::Value*      sp = b_.CreateStructGEP(state_ty_, state_, s.field);
        llvm::Value*      mp = b_.CreateStructGEP(s.ty, sp, 0);
        llvm::Value*      zp = b_.CreateStructGEP(s.ty, sp, 1);
        (void)rhs_ty;

        // Whole-signal store: the frontend context-sizes the RHS to the lvalue
        // width (via an inserted Resize), so rhs already matches the signal width
        // and the planes store directly.
        if (lv.width == s.width && is_zero_const(lv.bit_off)) {
            b_.CreateStore(rhs.mm, mp);
            b_.CreateStore(rhs.z, zp);
            return;
        }

        // Partial store: read-modify-write each plane at the signal width.
        const Plane  old   = { b_.CreateLoad(int_ty(2 * s.width), mp), b_.CreateLoad(int_ty(s.width), zp) };
        llvm::Value* off   = cast(lv.bit_off, s.width, false);
        llvm::Value* mask  = b_.CreateShl(cast(ones(lv.width), s.width, false), off);
        llvm::Value* nmask = b_.CreateNot(mask);
        llvm::Value* must  = merge_bits(must_of(old), cast(must_of(rhs), s.width, false), off, mask, nmask);
        llvm::Value* may   = merge_bits(may_of(old), cast(may_of(rhs), s.width, false), off, mask, nmask);
        llvm::Value* z     = merge_bits(old.z, cast(rhs.z, s.width, false), off, mask, nmask);
        b_.CreateStore(packed(must, may, z).mm, mp);
        b_.CreateStore(z, zp);
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

    // -- State --

    const Module&           mod_;
    llvm::LLVMContext&      ctx_;
    llvm::Module&           llmod_;
    llvm::IRBuilder<>       b_;
    llvm::StructType*       state_ty_ = nullptr;
    std::vector<SignalSlot> slots_;
    // Deduplicated signal value types, keyed by name (logic.<w> / bit.<w>).
    ankerl::unordered_dense::map<std::string, llvm::StructType*> value_types_;

    // Per-process scratch, reset in lower_process.
    const Ir*                           ir_    = nullptr;
    llvm::Function*                     fn_    = nullptr;
    llvm::Value*                        state_ = nullptr;
    std::vector<Plane>                  values_;
    std::vector<std::optional<LValue>>  lvals_;
    std::vector<std::optional<ValType>> types_;
};

} // namespace

bool emit_llvm(const Module& m, llvm::raw_ostream& os) {
    llvm::LLVMContext ctx;
    llvm::Module      llmod(m.name.empty() ? "clotho" : m.name, ctx);

    try {
        Backend(m, ctx, llmod).run();
    } catch (const BackendError& e) {
        llvm::errs() << "llvm backend error: " << e.message << "\n";
        return false;
    }

    if (llvm::verifyModule(llmod, &llvm::errs())) {
        return false;
    }

    llmod.print(os, nullptr);
    return true;
}

} // namespace clotho::ir
