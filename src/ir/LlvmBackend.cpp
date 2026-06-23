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
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>

#include "ir/InternPool.h"
#include "ir/Ir.h"

// LLVM backend rationale.
//
// 4-state lowering (dual plane). clotho values are 4-state (0/1/X/Z), stored as
// two bit-planes: `val` and `xz` (xz bit set => the bit is X or Z). LLVM has no
// 4-state integer, so each SSA value lowers to a *pair* of iW values, and every
// operation derives both planes of its result from both planes of its operands.
// 2-state values carry an all-zero xz plane, so one code path serves both.
//
// State / ABI. Signals are persistent module state. We build one LLVM struct
// with two iW fields per signal (val then xz) and hand every process function a
// pointer to it; Load reads those fields, Store writes them.
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

// A lowered value as two bit-planes of identical integer type iW.
struct Plane {
    llvm::Value* val = nullptr;
    llvm::Value* xz  = nullptr;
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
    // The signal's value type: a `{val, xz}` plane struct (e.g. %logic.8).
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
        for (const Process& proc : mod_.processes()) {
            lower_process(proc);
        }
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

    // The named, deduplicated `{val, xz}` struct for a signal type. 4-state types
    // are %logic.<width>, 2-state %bit.<width>; each is emitted at most once and
    // only because a signal uses it.
    llvm::StructType* value_type(Ref type) {
        const Layout      lay  = mod_.pool.layout(type);
        const uint32_t    w    = lay.width == 0 ? 1 : lay.width;
        const bool        four = lay.domain == Domain::Four;
        const std::string name = (four ? "logic." : "bit.") + std::to_string(w);

        const auto it = value_types_.find(name);
        if (it != value_types_.end()) {
            return it->second;
        }
        auto* st           = llvm::StructType::create(ctx_, { int_ty(w), int_ty(w) }, name);
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

    void lower_process(const Process& proc) {
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
                llvm::Value*  idx  = cast(value_of(p.binary.r).val, 32, false);
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

        llvm::Value* val = const_from_bytes(bytes.subspan(0, pb), w);
        llvm::Value* xz  = lay.domain == Domain::Four ? const_from_bytes(bytes.subspan(pb, pb), w) : zero(w);
        return { val, xz };
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
        const SignalSlot& s   = slot_of(lv.sig);
        llvm::Value*      sp  = b_.CreateStructGEP(state_ty_, state_, s.field);
        llvm::Value*      vp  = b_.CreateStructGEP(s.ty, sp, 0);
        llvm::Value*      xp  = b_.CreateStructGEP(s.ty, sp, 1);
        llvm::Value*      val = b_.CreateLoad(int_ty(s.width), vp);
        llvm::Value*      xz  = b_.CreateLoad(int_ty(s.width), xp);
        if (lv.width == s.width && is_zero_const(lv.bit_off)) {
            return { val, xz };
        }
        llvm::Value* off = cast(lv.bit_off, s.width, false);
        val              = cast(b_.CreateLShr(val, off), lv.width, false);
        xz               = cast(b_.CreateLShr(xz, off), lv.width, false);
        return { val, xz };
    }

    void lower_store(const LValue& lv, Plane rhs, ValType rhs_ty) {
        const SignalSlot& s    = slot_of(lv.sig);
        llvm::Value*      rval = cast(rhs.val, lv.width, false);
        llvm::Value*      rxz  = cast(rhs.xz, lv.width, false);
        (void)rhs_ty;

        llvm::Value* sp = b_.CreateStructGEP(state_ty_, state_, s.field);
        llvm::Value* vp = b_.CreateStructGEP(s.ty, sp, 0);
        llvm::Value* xp = b_.CreateStructGEP(s.ty, sp, 1);

        if (lv.width == s.width && is_zero_const(lv.bit_off)) {
            b_.CreateStore(rval, vp);
            b_.CreateStore(rxz, xp);
            return;
        }

        llvm::Value* off   = cast(lv.bit_off, s.width, false);
        llvm::Value* mask  = b_.CreateShl(cast(ones(lv.width), s.width, false), off);
        llvm::Value* nmask = b_.CreateNot(mask);
        write_masked(vp, int_ty(s.width), nmask, mask, b_.CreateShl(cast(rval, s.width, false), off));
        write_masked(xp, int_ty(s.width), nmask, mask, b_.CreateShl(cast(rxz, s.width, false), off));
    }

    // field = (old & ~mask) | (ins & mask)
    void write_masked(llvm::Value* ptr, llvm::Type* ty, llvm::Value* nmask, llvm::Value* mask, llvm::Value* ins) {
        llvm::Value* old  = b_.CreateLoad(ty, ptr);
        llvm::Value* kept = b_.CreateAnd(old, nmask);
        b_.CreateStore(b_.CreateOr(kept, b_.CreateAnd(ins, mask)), ptr);
    }

    static bool is_zero_const(llvm::Value* v) {
        auto* c = llvm::dyn_cast<llvm::ConstantInt>(v);
        return c != nullptr && c->isZero();
    }

    // -- Per-bit mask helpers (operate on whole planes) --

    llvm::Value* def1(Plane a) {
        return b_.CreateAnd(a.val, b_.CreateNot(a.xz));
    }
    llvm::Value* def0(Plane a) {
        return b_.CreateAnd(b_.CreateNot(a.val), b_.CreateNot(a.xz));
    }
    // i1: does this value contain any X/Z bit?
    llvm::Value* any_unknown(Plane a) {
        return b_.CreateICmpNE(a.xz, llvm::ConstantInt::get(a.xz->getType(), 0));
    }
    // i1: is any bit a definite 1 / definite 0?
    llvm::Value* any_one(Plane a) {
        llvm::Value* d = def1(a);
        return b_.CreateICmpNE(d, llvm::ConstantInt::get(d->getType(), 0));
    }
    llvm::Value* any_zero(Plane a) {
        llvm::Value* d = def0(a);
        return b_.CreateICmpNE(d, llvm::ConstantInt::get(d->getType(), 0));
    }

    // Pack an i1 (val) + i1 (xz) into a width-1 Plane.
    Plane bit_plane(llvm::Value* val_i1, llvm::Value* xz_i1) {
        return { b_.CreateZExt(val_i1, int_ty(1)), b_.CreateZExt(xz_i1, int_ty(1)) };
    }

    // -- Arithmetic: any X/Z in an operand makes the whole result X. --

    Plane lower_arith(Inst::Tag tag, Plane a, Plane b) {
        const uint32_t w = llvm::cast<llvm::IntegerType>(a.val->getType())->getBitWidth();
        llvm::Value*   r = nullptr;
        switch (tag) {
            case Inst::Tag::Add:
                r = b_.CreateAdd(a.val, b.val);
                break;
            case Inst::Tag::Sub:
                r = b_.CreateSub(a.val, b.val);
                break;
            default:
                r = b_.CreateMul(a.val, b.val);
                break;
        }
        llvm::Value* unknown = b_.CreateOr(any_unknown(a), any_unknown(b));
        llvm::Value* val     = b_.CreateSelect(unknown, ones(w), r);
        llvm::Value* xz      = b_.CreateSelect(unknown, ones(w), zero(w));
        return { val, xz };
    }

    // -- Bitwise X/Z (per-bit). --
    //
    // The val plane is meaningful only where xz==0; where xz==1 (X/Z) the val bit
    // is don't-care, so these need not canonicalize it. The xz (unknown) plane is
    // computed exactly. Naming the planes ah/al (a.xz/a.val) etc, these are the
    // minimal forms: the val plane is plain 2-state &/|/^/~, and the unknown plane
    // accounts for the absorbing 0 (AND) / 1 (OR).

    Plane lower_bitwise(Inst::Tag tag, Plane a, Plane b) {
        if (tag == Inst::Tag::Bxor) {
            // Unknown iff either operand bit is unknown; value is plain xor.
            return { b_.CreateXor(a.val, b.val), b_.CreateOr(a.xz, b.xz) };
        }
        // Shared AND high-plane: unknown unless a forcing 0 is present.
        llvm::Value* t_and = b_.CreateAnd(b_.CreateOr(a.xz, a.val), b_.CreateOr(b.xz, b_.CreateAnd(a.xz, b.val)));
        if (tag == Inst::Tag::Band) {
            return { b_.CreateAnd(a.val, b.val), t_and };
        }
        // Bor: high plane is ah ^ bh ^ t_and.
        llvm::Value* xz = b_.CreateXor(b_.CreateXor(a.xz, b.xz), t_and);
        return { b_.CreateOr(a.val, b.val), xz };
    }

    Plane lower_bnot(Plane a) {
        // Known bits invert; unknown bits stay unknown (val don't-care).
        return { b_.CreateNot(a.val), a.xz };
    }

    // -- Reductions -> 1-bit. --

    Plane lower_reduce(Inst::Tag tag, Plane a) {
        if (tag == Inst::Tag::RedXor) {
            llvm::Value* unknown = any_unknown(a);
            llvm::Value* pop     = b_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, a.val);
            llvm::Value* parity  = b_.CreateTrunc(pop, int_ty(1));
            llvm::Value* val     = b_.CreateOr(b_.CreateZExt(unknown, int_ty(1)), parity);
            return { val, b_.CreateZExt(unknown, int_ty(1)) };
        }
        llvm::Value* unknown = any_unknown(a);
        if (tag == Inst::Tag::RedAnd) {
            llvm::Value* not_zero = b_.CreateNot(any_zero(a)); // no definite-0 bit
            llvm::Value* x        = b_.CreateAnd(not_zero, unknown);
            return bit_plane(not_zero, x);
        }
        // RedOr
        llvm::Value* one = any_one(a);
        llvm::Value* x   = b_.CreateAnd(b_.CreateNot(one), unknown);
        return bit_plane(b_.CreateOr(one, x), x);
    }

    // -- Shifts: X/Z in the count makes the whole result X; value planes shift. --

    Plane lower_shift(Inst::Tag tag, Plane a, Plane amt, bool sign) {
        const uint32_t w  = llvm::cast<llvm::IntegerType>(a.val->getType())->getBitWidth();
        llvm::Value*   sh = cast(amt.val, w, false);
        llvm::Value*   val;
        llvm::Value*   xz;
        switch (tag) {
            case Inst::Tag::Sll:
                val = b_.CreateShl(a.val, sh);
                xz  = b_.CreateShl(a.xz, sh);
                break;
            case Inst::Tag::Srl:
                val = b_.CreateLShr(a.val, sh);
                xz  = b_.CreateLShr(a.xz, sh);
                break;
            default: // Sra: arithmetic, replicate sign (and its x/z-ness)
                val = sign ? b_.CreateAShr(a.val, sh) : b_.CreateLShr(a.val, sh);
                xz  = sign ? b_.CreateAShr(a.xz, sh) : b_.CreateLShr(a.xz, sh);
                break;
        }
        llvm::Value* unknown = any_unknown(amt);
        return { b_.CreateSelect(unknown, ones(w), val), b_.CreateSelect(unknown, ones(w), xz) };
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
        llvm::Value* cmp     = b_.CreateICmp(pred, a.val, b.val);
        llvm::Value* unknown = b_.CreateOr(any_unknown(a), any_unknown(b));
        return bit_plane(b_.CreateOr(cmp, unknown), unknown);
    }

    // -- Logical and/or/not -> 1-bit, three-valued. --

    // Per-operand truth (i1): definitely-true, definitely-false, unknown.
    struct Truth {
        llvm::Value* t;
        llvm::Value* f;
        llvm::Value* x;
    };
    Truth truth_of(Plane a) {
        llvm::Value* t = any_one(a);
        llvm::Value* f = b_.CreateAnd(b_.CreateNot(t), b_.CreateNot(any_unknown(a)));
        llvm::Value* x = b_.CreateAnd(b_.CreateNot(t), b_.CreateNot(f));
        return { t, f, x };
    }

    Plane lower_logical(Inst::Tag tag, Plane a, Plane b) {
        const Truth  ta = truth_of(a);
        const Truth  tb = truth_of(b);
        llvm::Value* res_t;
        llvm::Value* res_f;
        if (tag == Inst::Tag::Land) {
            res_f = b_.CreateOr(ta.f, tb.f);
            res_t = b_.CreateAnd(ta.t, tb.t);
        } else {
            res_t = b_.CreateOr(ta.t, tb.t);
            res_f = b_.CreateAnd(ta.f, tb.f);
        }
        llvm::Value* x = b_.CreateAnd(b_.CreateNot(res_t), b_.CreateNot(res_f));
        return bit_plane(b_.CreateOr(res_t, x), x);
    }

    Plane lower_lnot(Plane a) {
        const Truth t = truth_of(a);
        // true -> 0, false -> 1, x -> x
        return bit_plane(b_.CreateOr(t.f, t.x), t.x);
    }

    // -- Mux: known condition selects; unknown merges bitwise (X where differ). --

    Plane lower_mux(Plane cond, Plane a, Plane b) {
        llvm::Value* cond_known = b_.CreateICmpEQ(cond.xz, zero(1));
        llvm::Value* cond_true  = b_.CreateICmpNE(cond.val, zero(1));

        llvm::Value* sel_val = b_.CreateSelect(cond_true, a.val, b.val);
        llvm::Value* sel_xz  = b_.CreateSelect(cond_true, a.xz, b.xz);

        llvm::Value* diff    = b_.CreateOr(b_.CreateXor(a.val, b.val), b_.CreateXor(a.xz, b.xz));
        llvm::Value* ndiff   = b_.CreateNot(diff);
        llvm::Value* unk_val = b_.CreateOr(b_.CreateAnd(a.val, ndiff), diff);
        llvm::Value* unk_xz  = b_.CreateOr(b_.CreateAnd(a.xz, ndiff), diff);

        return { b_.CreateSelect(cond_known, sel_val, unk_val), b_.CreateSelect(cond_known, sel_xz, unk_xz) };
    }

    // -- Concat / Repl: first operand occupies the most-significant bits. --

    Plane lower_concat(Index i) {
        const uint32_t       w = type_of(i).width;
        std::array<Index, 3> scratch;
        const auto           parts = ir_->operands(i, scratch);

        llvm::Value* val = zero(w);
        llvm::Value* xz  = zero(w);
        for (Index part : parts) {
            const uint32_t pw = type_of(part).width;
            const Plane    pv = value_of(part);
            val               = b_.CreateOr(b_.CreateShl(val, pw), cast(pv.val, w, false));
            xz                = b_.CreateOr(b_.CreateShl(xz, pw), cast(pv.xz, w, false));
        }
        return { val, xz };
    }

    Plane lower_repl(Plane v, uint32_t part_width, uint32_t count) {
        const uint32_t w   = part_width * count;
        llvm::Value*   val = zero(w);
        llvm::Value*   xz  = zero(w);
        llvm::Value*   pv  = cast(v.val, w, false);
        llvm::Value*   px  = cast(v.xz, w, false);
        for (uint32_t c = 0; c < count; ++c) {
            val = b_.CreateOr(b_.CreateShl(val, part_width), pv);
            xz  = b_.CreateOr(b_.CreateShl(xz, part_width), px);
        }
        return { val, xz };
    }

    // -- Resize: truncate or extend both planes (sign per target). --

    Plane lower_resize(Plane v, ValType from, ValType to) {
        (void)from;
        return { cast(v.val, to.width, to.is_signed), cast(v.xz, to.width, to.is_signed) };
    }

    // -- Control flow: if/else with two regions. X/Z condition takes else. --

    void lower_cond(Index i) {
        const Inst::Payload& p    = ir_->payload(i);
        const auto           regs = ir_->extra_data<Regions>(p.op_extra.extra);
        const Plane          cond = value_of(p.op_extra.op);

        llvm::Value* known = b_.CreateICmpEQ(cond.xz, zero(1));
        llvm::Value* nz    = b_.CreateICmpNE(cond.val, zero(1));
        llvm::Value* take  = b_.CreateAnd(known, nz);

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
