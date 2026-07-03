#include "ir/LlvmBackend.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
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
#include <llvm/Support/TargetSelect.h>

#include "ir/InternPool.h"
#include "ir/Ir.h"
#include "ir/ZElision.h"
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
// cleared by the logic ops (their result is never Z).
// To reconstruct canonical 4-state when needed: xz = must ^ may, val = must.
//
// Z elision. compute_may_z (ZElision.cpp) proves most signals can never carry a
// z bit; a proven never-z signal stores no z word in its slot (see slot_type)
// and its loads materialize a constant-zero z plane instead. SSA values,
// function ABIs, and frame locals keep their z plane regardless — the zero
// constants fold in LLVM — so only persistent storage changes shape.
//
// State / ABI. Signals are persistent module state laid out with LLVM's type
// system. Every signal owns a `{ i(2W) must|may, iW z }` slot struct — without
// the z word when the signal is proven never-z (an
// unpacked-array signal an `[N x slot]` of leaf slots); every module definition
// gets one identified struct type listing its scope's slots followed by one
// field per child instance (the child definition's struct, replicated per
// instantiation); the root `%clotho.state` composes the top-level instances
// plus the NBA shadow region. Each process takes a `ptr` to the root and
// reaches a field through constant-index GEPs, so LLVM computes and owns all
// offsets/padding (the module carries the host target's DataLayout).
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
    // A string value: carried as an rt_string handle pointer (str_vals_), never
    // as a Plane. width / four_state are meaningless when set.
    bool is_string = false;
};

// A lowered value: the must/may planes packed into one i(2W) integer (low W bits
// = must / definitely 1, high W bits = may / could be 1), plus a separate iW `z`
// (high-impedance marker). Packing lets the monotone ops (AND/OR/NOT, mux select)
// act on both planes with a single instruction.
struct Plane {
    llvm::Value* mm = nullptr; // i(2W): [0,W) must, [W,2W) may
    llvm::Value* z  = nullptr; // iW
};

// Single-bit 4-state truth tables, packed into one i32 immediate so a width-1 op
// is a shift+mask instead of an unpack/recombine. A 2-bit `code` carries one bit
// in the Plane encoding: bit0 = must, bit1 = may. Legal codes are 00=0, 11=1,
// 10=X; 01 violates must=>may and is unreachable at runtime.
//
// build_lut packs the 16 (a, b) outcomes at bit (((a<<2)|b)<<1). Illegal operand
// codes fill 0: LLVM has no per-bit poison, and the slot is never indexed because
// the invariant holds, so a harmless 2-state value is fine.
using code = unsigned;

constexpr bool legal(code c) {
    return c != 0b01;
}

template <class F>
constexpr uint32_t build_lut(F f) {
    uint32_t t = 0;
    for (code a = 0; a < 4; ++a) {
        for (code b = 0; b < 4; ++b) {
            const code o = legal(a) && legal(b) ? f(a, b) & 0b11u : 0u;
            t |= o << (((a << 2) | b) << 1);
        }
    }
    return t;
}

// XOR is non-monotone (X ^ anything = X), so the result is X iff either operand
// is X, else the must-bit xor. Same logic as the wide path, evaluated at compile
// time over the 2-bit domain.
constexpr code xor4(code a, code b) {
    const code u = ((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u; // either X -> X
    const code k = (a ^ b) & 1u;                           // value: must ^ must
    return (((u | k) & 1u) << 1) | (~u & k & 1u);          // (may<<1) | must
}

constexpr uint32_t XOR1_LUT = build_lut(xor4); // 0x23A200E0

// 1-bit add/sub truncate the carry/borrow, so the result is the sum bit a^b;
// like all arithmetic, any X operand makes it X. That is exactly the XOR table,
// but derive it independently from the addition so a future encoding change is
// caught rather than silently aliasing.
constexpr code add4(code a, code b) {
    const code u = ((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u; // either X -> X
    const code k = ((a & 1u) + (b & 1u)) & 1u;             // sum bit: must + must
    return (((u | k) & 1u) << 1) | (~u & k & 1u);          // (may<<1) | must
}

constexpr uint32_t ADD1_LUT = build_lut(add4);
static_assert(ADD1_LUT == XOR1_LUT, "1-bit add must equal 1-bit xor");

// 1-bit subtract truncates the borrow, so the result is a^b — again the XOR
// table. Derived from subtraction and asserted equal.
constexpr code sub4(code a, code b) {
    const code u = ((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u; // either X -> X
    const code k = ((a & 1u) - (b & 1u)) & 1u;             // difference bit
    return (((u | k) & 1u) << 1) | (~u & k & 1u);
}

constexpr uint32_t SUB1_LUT = build_lut(sub4);
static_assert(SUB1_LUT == XOR1_LUT, "1-bit sub must equal 1-bit xor");

// 1-bit multiply: the value is a&b, but multiply is arithmetic, so any X operand
// makes the result X (0*x = x, unlike the monotone bitwise AND where 0&x = 0).
// Distinct table.
constexpr code mul4(code a, code b) {
    const code u = ((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u; // arith rule: any X -> X
    const code k = (a & 1u) & (b & 1u);                    // product bit: must * must
    return (((u | k) & 1u) << 1) | (~u & k & 1u);
}

constexpr uint32_t MUL1_LUT = build_lut(mul4); // 0xE0A20020
static_assert(MUL1_LUT != XOR1_LUT, "1-bit mul must differ from xor (0*x = x, not 0)");

// 1-bit divide: a/1 = a, but a zero divisor is X (like any divide-by-zero) and
// any X operand is X. `~b & 1` is the divisor-known-zero term.
constexpr code div4(code a, code b) {
    const code uz = (((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u) | (~b & 1u); // X or b==0
    const code k  = a & 1u;                                               // a / 1 = a
    return (((uz | k) & 1u) << 1) | (~uz & k & 1u);
}

constexpr uint32_t DIV1_LUT = build_lut(div4); // 0xE2A20022

// 1-bit modulo: a % 1 = 0 always; a zero divisor is X; any X operand is X.
constexpr code mod4(code a, code b) {
    const code uz = (((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u) | (~b & 1u); // X or b==0
    const code k  = 0u;                                                   // a % 1 = 0
    return (((uz | k) & 1u) << 1) | (~uz & k & 1u);
}

constexpr uint32_t MOD1_LUT = build_lut(mod4); // 0x22A20022
static_assert(DIV1_LUT != MOD1_LUT, "1-bit div (=a) and mod (=0) must differ");

// 1-bit power (unsigned): exp 0 -> 1, else base^1 = base; any X operand is X.
constexpr code pow4(code a, code b) {
    const code u = ((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u;
    const code k = (b & 1u) ? (a & 1u) : 1u;                              // b ? a : 1
    return (((u | k) & 1u) << 1) | (~u & k & 1u);
}

constexpr uint32_t POW1_LUT = build_lut(pow4); // 0xE3A20023

// 1-bit == is XNOR of the value bits (equal -> 1), with the X rule (any X -> X).
// This is the X-propagating `==`/`!=`, not the case operators `===`/`!==`.
constexpr code eq4(code a, code b) {
    const code u = ((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u;  // either X -> X
    const code k = (~((a & 1u) ^ (b & 1u))) & 1u;           // a == b
    return (((u | k) & 1u) << 1) | (~u & k & 1u);
}

constexpr uint32_t EQ1_LUT = build_lut(eq4); // 0xE0A20023

// 1-bit != is the value xor, with the X rule — the same table as XOR.
constexpr code ne4(code a, code b) {
    const code u = ((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u;
    const code k = ((a & 1u) ^ (b & 1u)) & 1u;              // a != b
    return (((u | k) & 1u) << 1) | (~u & k & 1u);
}

constexpr uint32_t NE1_LUT = build_lut(ne4);
static_assert(NE1_LUT == XOR1_LUT, "1-bit != must equal 1-bit xor");

// 1-bit bitwise xnor is ~(a ^ b) with the X rule — the same table as ==.
constexpr code bxnor4(code a, code b) {
    const code u = ((a ^ (a >> 1)) | (b ^ (b >> 1))) & 1u;
    const code k = (~((a & 1u) ^ (b & 1u))) & 1u;          // ~(a ^ b)
    return (((u | k) & 1u) << 1) | (~u & k & 1u);
}

constexpr uint32_t BXNOR1_LUT = build_lut(bxnor4);
static_assert(BXNOR1_LUT == EQ1_LUT, "1-bit xnor must equal 1-bit ==");

// A resolved assignment target: a `{ i(2W) must|may, iW z }` slot somewhere in
// typed storage (the module state struct for a signal, or the per-call frame
// struct for a function local) plus a bit offset / width selecting part of it.
// `bit_off` is an i32; a whole-storage target has offset 0 and width ==
// store_width.
struct LValue {
    // Pointer to the slot struct — or, for an unpacked-array signal, to the
    // whole `[N x slot]` storage (`elem_idx` then selects the leaf).
    llvm::Value* base = nullptr;
    // The leaf slot type `{ i(2W), iW }` of the addressed storage.
    llvm::StructType* slot_ty = nullptr;
    // Whether the slot stores a z word (field 1). False for a never-z signal;
    // loads then materialize a zero z plane and stores drop theirs.
    bool has_z = true;
    // The `[N x slot]` storage type when `base` addresses a whole memory; null
    // for plain scalar storage.
    llvm::ArrayType* array_ty = nullptr;
    // Flat leaf-element index (i32) into `array_ty`, accumulated across nested
    // ElemSelects; null when the target is a plain signal / frame local.
    llvm::Value* elem_idx = nullptr;
    // Full storage width in bits (the leaf element width for a memory).
    uint32_t store_width = 0;
    // Bit offset of the selection within the storage (i32).
    llvm::Value* bit_off = nullptr;
    // Selected width in bits (== store_width for a whole-storage target).
    uint32_t width = 0;
    // A module signal (not a frame local): its store services wait queues. `sig`
    // is meaningful only then.
    bool   is_signal = false;
    Signal sig{ 0 };
    // A string target: `base` (indexed by elem_idx for an array) is the
    // rt_string handle; widths / bit_off / has_z are meaningless.
    bool is_string = false;
};

// Per-signal placement in the state struct plus its cached layout facts. A
// signal owns one `{ i(2W) must|may, iW z }` slot struct (an unpacked array an
// `[elem_count x slot]` of leaf slots), reached from the root state pointer by
// the constant GEP field indices in `path`.
struct SignalSlot {
    // The leaf slot type `{ i(2W), iW }` (of the element, for a memory).
    llvm::StructType* slot_ty = nullptr;
    // The signal's whole storage: slot_ty, or `[elem_count x slot_ty]`.
    llvm::Type* storage_ty = nullptr;
    // GEP field indices from the root state struct to this signal's storage.
    std::vector<uint32_t> path;
    uint32_t width      = 0;
    bool     four_state = false;
    bool     is_signed  = false;
    // Whether the slot stores a z word: the signal may carry z (compute_may_z)
    // or is a task out/inout target (the task ABI stores z through a pointer to
    // it). A never-z slot is just `{ i(2W) }`.
    bool has_z = true;
    // Powers up as z rather than x: an undriven 4-state net (LRM 6.6). Implies
    // has_z (undriven nets are a may-z seed).
    bool init_z = false;
    // Unpacked array (memory): `width` is the leaf element width; `elem_count`
    // the total leaf-element count (product of all dimensions).
    bool     is_array   = false;
    uint32_t elem_count = 0;
    // Nonblocking-assignment staging. Set only for signals that are NBA targets.
    // A target gets shadow `i(2W)` / `iW` fields mirroring its live slot in the
    // root's shadow struct; a target with any bit/part-select site also gets an
    // iW dirty mask field. nba_id is a dense index into the pending flags / meta
    // table; nba_bucket indexes the per-(width, partial) commit worklist; the
    // sh_* members are field indices into the shadow struct.
    bool     nba           = false;
    bool     nba_partial   = false;
    uint32_t nba_id        = 0;
    uint32_t nba_bucket    = 0;
    uint32_t sh_mm_field   = 0;
    uint32_t sh_z_field    = 0;
    uint32_t sh_mask_field = 0;
    // A string (or array-of-string) signal: the slot is an opaque rt_string
    // handle (`clotho.string`), has_z is false, and `nba` is never set (the
    // frontend rejects nonblocking assignment to strings).
    bool is_string = false;
};

// One NBA commit bucket: every NBA-target signal of a given width and partial-ness
// shares a worklist (of nba-ids) and a width-specialized commit loop. Splitting by
// width lets each loop type its plane loads/stores exactly (LLVM integer widths are
// static); splitting full vs partial keeps whole-signal commits free of mask/RMW.
struct NbaBucket {
    uint32_t              width;          // signal width (the commit loop bakes it in)
    bool                  partial;        // mask-RMW commit vs whole copy
    bool                  has_z;          // targets with a z word commit it; never-z loops skip it
    llvm::ArrayType*      ty   = nullptr; // [cap x i32] of nba-ids
    llvm::GlobalVariable* gv   = nullptr;
    llvm::GlobalVariable* top  = nullptr; // i32 live count
};

// One function-frame local: its `{ i(2W), iW }` slot type and width / domain.
// The frame is an LLVM struct with one field per local, in Local-handle order,
// so the local's index IS its GEP field index. No wait-queue / NBA machinery
// (frame locals have neither).
struct FrameSlot {
    llvm::StructType* slot_ty    = nullptr;
    uint32_t          width      = 0;
    bool              four_state = false;
    // A string local: the slot is an rt_string handle; init/deinit bracket the
    // call.
    bool is_string = false;
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
    Backend(const Module& mod, llvm::LLVMContext& ctx, llvm::Module& llmod, const SimulationConfig& config)
      : mod_(mod)
      , ctx_(ctx)
      , llmod_(llmod)
      , b_(ctx)
      , config_(config) {}

    void run() {
        build_state_types();
        build_wait_queues();
        build_sched_flags();
        build_nba_state();

        // Functions and tasks: declare every one up front (so calls resolve,
        // including recursion and forward references), then lower their bodies. A
        // task is a coroutine (lower_task); a function is a plain call.
        const size_t nf = mod_.function_count();
        fn_decls_.assign(nf, nullptr);
        for (uint32_t i = 0; i < nf; ++i) {
            fn_decls_[i] = declare_function(static_cast<FunctionId>(i));
        }
        for (uint32_t i = 0; i < nf; ++i) {
            if (mod_.function(static_cast<FunctionId>(i)).is_task) {
                lower_task(static_cast<FunctionId>(i));
            } else {
                lower_function(static_cast<FunctionId>(i));
            }
        }

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

    // -- State types --

    // The `{ i(2W) must|may, iW z }` slot struct for a value of the given width
    // and domain — just `{ i(2W) }` when the slot needs no z word (a proven
    // never-z signal; frame locals always keep z). Identified and memoized per
    // (width, domain, has_z) so every signal of one shape shares one named LLVM
    // type (%logic.8, %bit.nz.4, ...); the single-field form keeps the struct
    // wrapper so GEP paths and array-of-slot addressing stay uniform.
    llvm::StructType* slot_type(uint32_t width, bool four_state, bool has_z) {
        for (const auto& [w, four, z, ty] : slot_types_) {
            if (w == width && four == four_state && z == has_z) {
                return ty;
            }
        }
        const std::string name =
          std::string(four_state ? "logic." : "bit.") + (has_z ? "" : "nz.") + std::to_string(width);
        std::vector<llvm::Type*> fields{ int_ty(2 * width) };
        if (has_z) {
            fields.push_back(int_ty(width));
        }
        auto* ty = llvm::StructType::create(ctx_, fields, name);
        slot_types_.emplace_back(width, four_state, has_z, ty);
        return ty;
    }

    // The opaque rt_string handle slot type (`%clotho.string = { [32 x i8] }`),
    // memoized. A struct wrapper keeps GEP paths and array-of-slot addressing
    // uniform with the plane slots.
    llvm::StructType* str_slot_ty() {
        if (str_slot_ty_ == nullptr) {
            str_slot_ty_ = llvm::StructType::create(
              ctx_, { llvm::ArrayType::get(b_.getInt8Ty(), sizeof(rt_string)) }, "clotho.string");
        }
        return str_slot_ty_;
    }

    // Whether a type is a string, or an unpacked array whose leaf element is.
    bool is_string_type(Ref type) const {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            return is_string_type(t.element);
        }
        return t.kind == Type::Kind::String;
    }

    // Total leaf-element count of an unpacked-array type (product of all
    // dimensions); 1 for a scalar leaf.
    uint32_t array_leaf_count(Ref type) const {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            const uint32_t count = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
            return count * array_leaf_count(t.element);
        }
        return 1;
    }

    // Byte offset of a GEP field-index path from the root state struct, per the
    // module's DataLayout. Used where the runtime needs raw offsets (the NBA
    // commit loop's meta table).
    uint32_t path_offset(std::span<const uint32_t> path) const {
        const llvm::DataLayout& dl  = llmod_.getDataLayout();
        llvm::Type*             ty  = state_ty_;
        uint64_t                off = 0;
        for (const uint32_t f : path) {
            if (auto* st = llvm::dyn_cast<llvm::StructType>(ty)) {
                off += dl.getStructLayout(st)->getElementOffset(f);
                ty = st->getElementType(f);
            } else {
                auto* at = llvm::cast<llvm::ArrayType>(ty);
                off += f * dl.getTypeAllocSize(at->getElementType());
                ty = at->getElementType();
            }
        }
        return static_cast<uint32_t>(off);
    }

    // Build one instance scope's state struct: its signals' slots (field k ==
    // scope signal k), then one field per child instance. Assigns every covered
    // signal's GEP path; `path` holds the field indices from the root to this
    // scope. Struct types are deduplicated per module definition — instances of
    // one definition (same parameterization) produce identical field lists and
    // share one identified type, so the type is replicated per instantiation.
    llvm::StructType* scope_type(const InstanceScope& scope, std::vector<uint32_t>& path,
                                 std::vector<bool>& claimed) {
        std::vector<llvm::Type*> fields;
        for (uint32_t k = 0; k < scope.signals.size(); ++k) {
            const auto si = static_cast<uint32_t>(scope.signals[k]);
            if (si >= slots_.size() || claimed[si]) {
                throw BackendError{ "malformed instance hierarchy: signal claimed twice" };
            }
            claimed[si] = true;
            slots_[si].path = path;
            slots_[si].path.push_back(k);
            fields.push_back(slots_[si].storage_ty);
        }
        const auto nsig = static_cast<uint32_t>(scope.signals.size());
        for (uint32_t c = 0; c < scope.children.size(); ++c) {
            path.push_back(nsig + c);
            fields.push_back(scope_type(scope.children[c], path, claimed));
            path.pop_back();
        }

        for (const auto& [def, flds, ty] : def_types_) {
            if (def == scope.def_name && flds == fields) {
                return ty;
            }
        }
        auto* ty = llvm::StructType::create(ctx_, fields, scope.def_name + ".state");
        def_types_.emplace_back(scope.def_name, fields, ty);
        return ty;
    }

    // Build the design's state types: per-signal slot / storage types, one
    // struct per module definition composed nested along the instance tree, and
    // the root `%clotho.state` = { <top instances...>, <unscoped signals...>,
    // <NBA shadow struct> }. Unscoped signals only occur for a module built
    // without hierarchy info (they form one flat root scope). The NBA shadow
    // region lives at the root, not inside the definition structs, because
    // staging is a backend artifact of which signals the design nonblocking-
    // assigns, not part of a module's declared state.
    void build_state_types() {
        const auto sigs = mod_.signals();
        slots_.resize(sigs.size());

        // A slot keeps its z word when the may-z analysis cannot prove it z-free
        // or the task ABI needs z storage behind it. Elision is analysis-driven,
        // not domain-driven: a 2-state signal assigned a z-carrying expression
        // round-trips the z bit today (Resize does not clear z across domains),
        // and that behavior is preserved — ordinary 2-state signals are never-z
        // through the analysis anyway.
        const std::vector<bool> may_z =
          config_.z_elision ? compute_may_z(mod_) : std::vector<bool>(sigs.size(), true);
        const std::vector<bool> task_out = task_out_targets();
        // Semantics, not optimization: computed regardless of config_.z_elision.
        // An undriven 4-state net powers up z (LRM 6.6); everything else x / 0.
        const std::vector<bool> undriven = compute_undriven(mod_);

        for (uint32_t i = 0; i < sigs.size(); ++i) {
            const Layout lay = mod_.pool.layout(sigs[i].type);
            const Type&  ty  = std::get<Type>(mod_.pool.get(sigs[i].type));

            SignalSlot& slot = slots_[i];
            slot.width       = lay.width == 0 ? 1 : lay.width;
            slot.four_state  = lay.domain == Domain::Four;
            slot.is_signed   = ty.is_signed;
            // A string slot is an opaque rt_string handle: no planes, no z word.
            if (is_string_type(sigs[i].type)) {
                slot.is_string = true;
                slot.has_z     = false;
                slot.slot_ty   = str_slot_ty();
                if (ty.kind == Type::Kind::UnpackedArray) {
                    slot.is_array   = true;
                    slot.elem_count = array_leaf_count(sigs[i].type);
                    slot.storage_ty = llvm::ArrayType::get(slot.slot_ty, slot.elem_count);
                } else {
                    slot.storage_ty = slot.slot_ty;
                }
                continue;
            }
            slot.init_z      = undriven[i] && sigs[i].is_net && slot.four_state;
            slot.has_z       = may_z[i] || task_out[i] || slot.init_z;
            slot.slot_ty     = slot_type(slot.width, slot.four_state, slot.has_z);
            if (ty.kind == Type::Kind::UnpackedArray) {
                slot.is_array   = true;
                slot.elem_count = array_leaf_count(sigs[i].type);
                slot.storage_ty = llvm::ArrayType::get(slot.slot_ty, slot.elem_count);
            } else {
                slot.storage_ty = slot.slot_ty;
            }
        }

        nba_classify();

        std::vector<llvm::Type*> root_fields;
        std::vector<bool>        claimed(sigs.size(), false);
        for (const InstanceScope& top : mod_.tops()) {
            std::vector<uint32_t> path{ static_cast<uint32_t>(root_fields.size()) };
            root_fields.push_back(scope_type(top, path, claimed));
        }
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            if (!claimed[i]) {
                slots_[i].path = { static_cast<uint32_t>(root_fields.size()) };
                root_fields.push_back(slots_[i].storage_ty);
            }
        }

        // Shadow staging fields for NBA targets, in nba-id order. A shadow
        // mirrors the live i(2W) / iW planes; a partial target adds a dirty mask.
        if (n_nba_ > 0) {
            std::vector<llvm::Type*> sf;
            for (uint32_t i = 0; i < sigs.size(); ++i) {
                SignalSlot& slot = slots_[i];
                if (!slot.nba) {
                    continue;
                }
                slot.sh_mm_field = static_cast<uint32_t>(sf.size());
                sf.push_back(int_ty(2 * slot.width));
                // A never-z target stages no z shadow; sh_z_field stays unused.
                if (slot.has_z) {
                    slot.sh_z_field = static_cast<uint32_t>(sf.size());
                    sf.push_back(int_ty(slot.width));
                }
                if (slot.nba_partial) {
                    slot.sh_mask_field = static_cast<uint32_t>(sf.size());
                    sf.push_back(int_ty(slot.width));
                }
            }
            shadow_ty_    = llvm::StructType::create(ctx_, sf, "clotho.nba.shadow");
            shadow_field_ = static_cast<uint32_t>(root_fields.size());
            root_fields.push_back(shadow_ty_);
        }

        state_ty_ = llvm::StructType::create(ctx_, root_fields, "clotho.state");
    }

    // Walk a StoreNb's lvalue chain to its root signal, reporting whether the
    // target is the whole signal (a bare Signal lvalue) or a bit/part-select.
    struct NbaTarget {
        Signal sig;
        bool   whole;
    };
    NbaTarget nba_target(const Ir& ir, Index lv) const {
        bool whole = true;
        for (;;) {
            switch (ir.tag(lv)) {
                case Inst::Tag::Signal: return { ir.payload(lv).signal, whole };
                case Inst::Tag::BitSelect: lv = ir.payload(lv).binary.l; whole = false; continue;
                case Inst::Tag::PartSelect: lv = ir.payload(lv).op_extra.op; whole = false; continue;
                case Inst::Tag::DynPartSelect: lv = ir.payload(lv).op_extra.op; whole = false; continue;
                case Inst::Tag::ElemSelect: lv = ir.payload(lv).op_extra.op; whole = false; continue;
                default: throw BackendError{ "unsupported nonblocking-assignment lvalue" };
            }
        }
    }

    // Mark NBA-target signals reached by one IR body (a process or a task): set
    // `nba`, `nba_partial` (any bit/part-select site), and collect unpacked-array
    // targets for the runtime deferred queue.
    void nba_classify_ir(const Ir& ir) {
        const auto sigs = mod_.signals();
        for (size_t k = 0; k < ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            if (ir.tag(idx) != Inst::Tag::StoreNb) {
                continue;
            }
            const NbaTarget t = nba_target(ir, ir.payload(idx).binary.l);
            // An unpacked-array element NBA uses the runtime deferred queue, not
            // the per-signal shadow; record the array signal for wake servicing.
            // (slot.is_array is not set until the placement loop below, so test
            // the type directly here.)
            if (std::get<Type>(mod_.pool.get(sigs[static_cast<uint32_t>(t.sig)].type)).kind ==
                Type::Kind::UnpackedArray) {
                if (std::find(array_nba_sigs_.begin(), array_nba_sigs_.end(), t.sig) == array_nba_sigs_.end()) {
                    array_nba_sigs_.push_back(t.sig);
                }
                continue;
            }
            SignalSlot& s = slots_[static_cast<uint32_t>(t.sig)];
            s.nba         = true;
            s.nba_partial = s.nba_partial || !t.whole;
        }
    }

    // Find every NBA-target signal across all processes and tasks and mark its slot:
    // `nba`, `nba_partial`, and a dense `nba_id` (signal order). A task NBA stages
    // into the same per-signal shadow and commits in its caller's NBA region, so its
    // targets must be classified here too. Bucket assignment happens later in
    // build_nba_state once widths are final.
    void nba_classify() {
        for (const Process& p : mod_.processes()) {
            nba_classify_ir(p.ir);
        }
        for (uint32_t i = 0; i < mod_.function_count(); ++i) {
            const Function& f = mod_.function(static_cast<FunctionId>(i));
            if (f.is_task) {
                nba_classify_ir(f.ir);
            }
        }
        n_nba_          = 0;
        const auto sigs = mod_.signals();
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            if (slots_[i].nba) {
                slots_[i].nba_id = n_nba_++;
            }
        }
    }

    // Signals used as a task out/inout actual anywhere in the design. The task
    // ABI passes `&signal.z` and stores through it at copy-out (emit_task_copyout),
    // so such a signal must keep its z word even when proven never-z. A storage/
    // ABI constraint, not a may-z fact, hence separate from compute_may_z.
    // (An alternative — a caller-stack z scratch word for elided targets — is
    // sound but adds a second mechanism for a rare construct.)
    std::vector<bool> task_out_targets() const {
        std::vector<bool> out(mod_.signals().size(), false);
        const auto        scan = [&](const Ir& ir) {
            for (size_t k = 0; k < ir.inst_count(); ++k) {
                const auto idx = static_cast<Index>(k);
                if (ir.tag(idx) != Inst::Tag::TaskCall) {
                    continue;
                }
                const auto      ti     = ir.extra_data<TaskCallInfo>(ir.payload(idx).extra);
                const Function& callee = mod_.function(ti.func);
                const auto      args   = ir.region(ti.args);
                for (size_t a = 0; a < args.size(); ++a) {
                    if (callee.arg_dirs[a] == ArgDir::In) {
                        continue;
                    }
                    Index lv = args[a];
                    for (bool walking = true; walking;) {
                        switch (ir.tag(lv)) {
                            case Inst::Tag::BitSelect: lv = ir.payload(lv).binary.l; break;
                            case Inst::Tag::PartSelect:
                            case Inst::Tag::DynPartSelect:
                            case Inst::Tag::ElemSelect: lv = ir.payload(lv).op_extra.op; break;
                            default: walking = false; break;
                        }
                    }
                    if (ir.tag(lv) == Inst::Tag::Signal) {
                        out[static_cast<uint32_t>(ir.payload(lv).signal)] = true;
                    }
                }
            }
        };
        for (const Process& p : mod_.processes()) {
            scan(p.ir);
        }
        for (uint32_t i = 0; i < mod_.function_count(); ++i) {
            scan(mod_.function(static_cast<FunctionId>(i)).ir);
        }
        return out;
    }

    const SignalSlot& slot_of(Signal s) const {
        return slots_[static_cast<uint32_t>(s)];
    }

    // Pointer to a signal's whole storage (its slot struct, or its [N x slot]
    // memory) within the state struct `base` points at: one constant-index GEP
    // along the slot's field path.
    llvm::Value* storage_ptr(llvm::Value* base, const SignalSlot& s) {
        std::vector<llvm::Value*> idx;
        idx.reserve(s.path.size() + 1);
        idx.push_back(b_.getInt32(0));
        for (const uint32_t f : s.path) {
            idx.push_back(b_.getInt32(f));
        }
        return b_.CreateInBoundsGEP(state_ty_, base, idx);
    }

    // Pointer to one field of the root NBA shadow struct.
    llvm::Value* shadow_ptr(uint32_t field) {
        return b_.CreateInBoundsGEP(
          state_ty_, state_, { b_.getInt32(0), b_.getInt32(shadow_field_), b_.getInt32(field) });
    }

    // Pointer to an lvalue's mm (field 0) or z (field 1) word, resolving the
    // selected leaf element for a memory target.
    llvm::Value* lv_field_ptr(const LValue& lv, uint32_t field) {
        if (lv.array_ty != nullptr) {
            llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
            return b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx, b_.getInt32(field) });
        }
        return b_.CreateStructGEP(lv.slot_ty, lv.base, field);
    }
    llvm::Value* lv_mm_ptr(const LValue& lv) {
        return lv_field_ptr(lv, 0);
    }
    llvm::Value* lv_z_ptr(const LValue& lv) {
        // A never-z slot has no field 1; any consumer must branch on has_z
        // before asking for the pointer.
        assert(lv.has_z);
        return lv_field_ptr(lv, 1);
    }

    // -- Per-process lowering --

    llvm::Function* lower_process(const Process& proc, uint32_t proc_index) {
        ir_            = &proc.ir;
        const size_t n = ir_->inst_count();
        values_.assign(n, Plane{});
        lvals_.assign(n, std::optional<LValue>{});
        types_.assign(n, std::optional<ValType>{});
        str_vals_.assign(n, nullptr);
        frame_slots_.clear(); // a process has no frame; deinit sees only temps

        // This process's scheduled-flag slot: &clotho.proc_sched[proc_index]. A
        // constant GEP so it is valid in any block of the function.
        sched_ptr_ = llvm::ConstantExpr::getInBoundsGetElementPtr(
          sched_arr_ty_, sched_gv_, llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(proc_index) });

        // Name a process by its index: @<module>.p<index>. Source-offset naming
        // would collide once the design has multiple instances of one module —
        // their processes share the same AST source location. The index is unique
        // across the flattened module; proc.loc still carries the source mapping.
        const std::string name = mod_.name + ".p" + std::to_string(proc_index);

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
        awaiter_          = nullptr;
        nba_scratch_      = nullptr;
        record_           = nullptr;
        cur_is_task_      = false;
        lowering_preinit_ = proc.activation == Activation::PreInit;

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
        // String rvalue temporaries live in the coroutine frame (CoroSplit spills
        // any that survive a suspend) and are constructed once, in the ramp.
        build_str_temps();

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
        // A completing process releases its string temporaries. (An always loop
        // never reaches here — its temps persist, bounded and capacity-reused; a
        // $finish longjmps out and the heap is reclaimed at process exit.)
        deinit_body_strings();
        auto* res = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getTrue() });
        auto* sw  = b_.CreateSwitch(res, coro_ret_bb_, 2);
        sw->addCase(b_.getInt8(0), trap_bb);
        sw->addCase(b_.getInt8(1), coro_cleanup_bb_);

        b_.SetInsertPoint(trap_bb);
        b_.CreateUnreachable();
        return fn_;
    }

    // -- Functions --
    //
    // Each ir::Function becomes a plain (non-coroutine) LLVM function
    //   { i(2Wr), iWr } @<mod>.fn.<name>(ptr state, <arg mm/z pairs...>)
    // (void return / no result for a void function). Locals live in a per-call
    // stack frame buffer, so recursion and `automatic` lifetime are just ordinary
    // stack frames; module signals are still reached through `state`.

    // The packed width W of a value type (the ==0 -> 1 scalar rule).
    uint32_t width_of(Ref type) const {
        const uint32_t w = mod_.pool.layout(type).width;
        return w == 0 ? 1 : w;
    }

    bool is_void(Ref type) const {
        return type == mod_.pool.void_type;
    }

    // The LLVM type a function returns: void, or the `{ i(2W), iW }` result planes.
    llvm::Type* fn_return_ty(Ref return_type) {
        if (is_void(return_type)) {
            return b_.getVoidTy();
        }
        const uint32_t w = width_of(return_type);
        return llvm::StructType::get(ctx_, { int_ty(2 * w), int_ty(w) });
    }

    // Create (but do not yet lower) a function's declaration, so calls — including
    // recursive and forward ones — resolve before any body is emitted.
    llvm::Function* declare_function(FunctionId id) {
        const Function& f = mod_.function(id);
        if (f.is_task) {
            return declare_task(id);
        }
        auto*                    ptr = llvm::PointerType::getUnqual(ctx_);
        std::vector<llvm::Type*> params;
        params.push_back(ptr); // state
        for (Ref pt : f.param_types) {
            if (is_string_type(pt)) {
                params.push_back(ptr); // rt_string handle (callee copy-in gives value semantics)
                continue;
            }
            const uint32_t w = width_of(pt);
            params.push_back(int_ty(2 * w)); // must|may
            params.push_back(int_ty(w));     // z
        }
        // A string result returns through a trailing out-param handle the caller
        // owns (its call-site temp) — a 32-byte handle has no plane struct form.
        llvm::Type* rty = fn_return_ty(f.return_type);
        if (is_string_type(f.return_type)) {
            rty = b_.getVoidTy();
            params.push_back(ptr);
        }
        auto* fnty = llvm::FunctionType::get(rty, params, false);
        auto* fn   = llvm::Function::Create(fnty, llvm::Function::InternalLinkage,
                                            mod_.name + ".fn." + f.name, llmod_);
        fn->addParamAttr(0, llvm::Attribute::NoAlias);
        return fn;
    }

    // A task is a switched-resume coroutine. Its signature is:
    //   ptr @<mod>.task.<name>(ptr noalias state, ptr wheel,
    //                          <per formal>, ptr record)
    // where each input formal contributes (i(2W) mm, iW z) value planes and each
    // output / inout formal contributes (ptr mm_ptr, ptr z_ptr) into a caller temp.
    // `record` is a { ptr parent } slot: the body stores the completing coroutine's
    // continuation there (see lower_task / lower_task_call_inst). It returns the
    // coroutine handle, like a process.
    llvm::Function* declare_task(FunctionId id) {
        const Function&          f   = mod_.function(id);
        auto*                    ptr = llvm::PointerType::getUnqual(ctx_);
        std::vector<llvm::Type*> params;
        params.push_back(ptr); // state
        params.push_back(ptr); // wheel
        for (size_t k = 0; k < f.param_types.size(); ++k) {
            if (f.arg_dirs[k] == ArgDir::In) {
                if (is_string_type(f.param_types[k])) {
                    params.push_back(ptr); // rt_string handle
                    continue;
                }
                const uint32_t w = width_of(f.param_types[k]);
                params.push_back(int_ty(2 * w)); // must|may
                params.push_back(int_ty(w));     // z
            } else {
                params.push_back(ptr); // &caller_temp.mm
                params.push_back(ptr); // &caller_temp.z
            }
        }
        params.push_back(ptr); // record { ptr parent }
        auto* fnty = llvm::FunctionType::get(ptr, params, false);
        auto* fn   = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage,
                                            mod_.name + ".task." + f.name, llmod_);
        fn->setPresplitCoroutine();
        fn->addParamAttr(0, llvm::Attribute::NoAlias);
        return fn;
    }

    // LLVM param index of formal k's first parameter (state, wheel, then 2 per
    // formal). The record slot follows all formals.
    // LLVM arg index of each formal's first parameter for `f`'s signature shape:
    // functions start after `state`, tasks after `state, wheel`; a string input
    // formal is one ptr, everything else two params. Replaces a fixed
    // 1 + 2k / 2 + 2k stride, which string formals break.
    std::vector<uint32_t> param_indices(const Function& f) const {
        std::vector<uint32_t> idx(f.param_types.size());
        uint32_t              cur = f.is_task ? 2 : 1;
        for (size_t k = 0; k < f.param_types.size(); ++k) {
            idx[k] = cur;
            const bool in_string =
              is_string_type(f.param_types[k]) && (!f.is_task || f.arg_dirs[k] == ArgDir::In);
            cur += in_string ? 1 : 2;
        }
        return idx;
    }

    // LLVM param index of the task's record slot: right after the last formal.
    uint32_t task_record_param(const Function& f) const {
        const auto idx = param_indices(f);
        if (idx.empty()) {
            return 2;
        }
        const bool last_string =
          is_string_type(f.param_types.back()) && f.arg_dirs.back() == ArgDir::In;
        return idx.back() + (last_string ? 1 : 2);
    }

    // Build a function's frame struct: one slot field per local, in Local-handle
    // order (a Local's index is its GEP field index).
    void build_frame_layout(const Function& f) {
        const size_t n = f.local_types.size();
        frame_slots_.assign(n, FrameSlot{});
        std::vector<llvm::Type*> fields(n);
        for (uint32_t i = 0; i < n; ++i) {
            const Layout lay           = mod_.pool.layout(f.local_types[i]);
            frame_slots_[i].width      = lay.width == 0 ? 1 : lay.width;
            frame_slots_[i].four_state = lay.domain == Domain::Four;
            if (is_string_type(f.local_types[i])) {
                frame_slots_[i].is_string = true;
                frame_slots_[i].slot_ty   = str_slot_ty();
                fields[i]                 = frame_slots_[i].slot_ty;
                continue;
            }
            // Frame slots always keep z: the task ABI and copy-in/out use the z
            // field, and LLVM folds the zero-z traffic on its own.
            frame_slots_[i].slot_ty = slot_type(frame_slots_[i].width, frame_slots_[i].four_state, true);
            fields[i]                  = frame_slots_[i].slot_ty;
        }
        frame_ty_ = llvm::StructType::create(ctx_, fields, mod_.name + ".fn." + f.name + ".frame");
    }

    // Power up the frame: 4-state locals come up X (must 0, may 1), 2-state 0; z
    // is cleared. Formal-parameter locals are immediately overwritten by copy-in;
    // an unwritten return variable thus reads X (its uninitialized value).
    void init_frame() {
        for (uint32_t i = 0; i < frame_slots_.size(); ++i) {
            const FrameSlot& s    = frame_slots_[i];
            llvm::Value*     slot = b_.CreateStructGEP(frame_ty_, frame_, i);
            if (s.is_string) {
                b_.CreateCall(str_init_fn(), { slot });
                continue;
            }
            llvm::Value*     mm   = s.four_state
                                      ? llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * s.width, s.width))
                                      : zero(2 * s.width);
            b_.CreateStore(mm, b_.CreateStructGEP(s.slot_ty, slot, 0));
            b_.CreateStore(zero(s.width), b_.CreateStructGEP(s.slot_ty, slot, 1));
        }
    }

    // Release every string owned by the current body: frame-local string slots
    // plus the string rvalue temporaries. Emitted before each function `ret`, in
    // a task's completion epilogue, and before a process's final suspend.
    void deinit_body_strings() {
        for (uint32_t i = 0; i < frame_slots_.size(); ++i) {
            if (frame_slots_[i].is_string) {
                b_.CreateCall(str_deinit_fn(), { b_.CreateStructGEP(frame_ty_, frame_, i) });
            }
        }
        for (llvm::Value* temp : str_temp_list_) {
            b_.CreateCall(str_deinit_fn(), { temp });
        }
    }

    // Lower a function body into its (already declared) LLVM function. Reuses the
    // shared instruction lowering — Arg reads a parameter, Local addresses the
    // frame, Ret is a real return, Call invokes another function.
    void lower_function(FunctionId id) {
        const Function& f = mod_.function(id);
        ir_               = &f.ir;
        cur_fn_           = &f;
        cur_is_task_      = false;
        param_llvm_index_ = param_indices(f);
        build_frame_layout(f);

        fn_            = fn_decls_[static_cast<uint32_t>(id)];
        const size_t n = ir_->inst_count();
        values_.assign(n, Plane{});
        lvals_.assign(n, std::optional<LValue>{});
        types_.assign(n, std::optional<ValType>{});
        str_vals_.assign(n, nullptr);

        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
        b_.SetInsertPoint(entry);
        state_ = fn_->getArg(0);
        state_->setName("state");
        fn_->addParamAttr(0, llvm::Attribute::NoAlias);

        frame_ = b_.CreateAlloca(frame_ty_, nullptr, "frame");
        init_frame();
        build_str_temps();

        lower_region(ir_->root());
        // The frontend appends a trailing Ret, so the entry path always terminates;
        // guard defensively in case a body lowered to nothing.
        if (b_.GetInsertBlock()->getTerminator() == nullptr) {
            deinit_body_strings();
            if (is_void(f.return_type)) {
                b_.CreateRetVoid();
            } else {
                b_.CreateRet(llvm::ConstantAggregateZero::get(fn_->getReturnType()));
            }
        }

        cur_fn_ = nullptr;
        frame_  = nullptr;
    }

    // -- Tasks --
    //
    // A task is a switched-resume coroutine (like a process) that also carries a
    // per-call frame (like a function). The ramp builds the frame and parks at the
    // initial suspend; the caller (a process or another task) drives it via
    // lower_task_call_inst. The body may suspend on #delay / @event, in which case
    // the caller suspends too; on completion the task copies output / inout formals
    // back through their pointers and resumes the caller (the record's parent).
    void lower_task(FunctionId id) {
        const Function& f = mod_.function(id);
        ir_               = &f.ir;
        cur_fn_           = &f;
        cur_is_task_      = true;
        param_llvm_index_ = param_indices(f);
        build_frame_layout(f);

        fn_            = fn_decls_[static_cast<uint32_t>(id)];
        const size_t n = ir_->inst_count();
        values_.assign(n, Plane{});
        lvals_.assign(n, std::optional<LValue>{});
        types_.assign(n, std::optional<ValType>{});
        str_vals_.assign(n, nullptr);

        auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
        state_       = fn_->getArg(0);
        state_->setName("state");
        wheel_ = fn_->getArg(1);
        wheel_->setName("wheel");
        awaiter_ = nullptr;
        record_  = nullptr;

        auto* none     = llvm::ConstantTokenNone::get(ctx_);
        auto* null     = llvm::ConstantPointerNull::get(ptr_ty);
        auto* entry    = llvm::BasicBlock::Create(ctx_, "entry", fn_);
        auto* alloc_bb = llvm::BasicBlock::Create(ctx_, "coro.alloc", fn_);
        auto* begin_bb = llvm::BasicBlock::Create(ctx_, "coro.begin", fn_);

        b_.SetInsertPoint(entry);
        auto* cid  = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_id), { b_.getInt32(0), null, null, null });
        auto* need = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_alloc), { cid });
        b_.CreateCondBr(need, alloc_bb, begin_bb);

        b_.SetInsertPoint(alloc_bb);
        auto* size = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_size, { b_.getInt64Ty() }), {});
        auto* mem  = b_.CreateCall(alloc_fn(), { size });
        b_.CreateBr(begin_bb);

        b_.SetInsertPoint(begin_bb);
        auto* frame_phi = b_.CreatePHI(ptr_ty, 2);
        frame_phi->addIncoming(null, entry);
        frame_phi->addIncoming(mem, alloc_bb);
        hdl_        = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_begin), { cid, frame_phi });
        coro_begin_ = llvm::cast<llvm::Instruction>(hdl_);

        // Frame buffer and the task's own scheduled flag, placed just after
        // coro.begin so CoroSplit spills them into the coroutine frame (both live
        // across suspends; the frame's address escapes to callees). The flag backs
        // intra-body @event waits, exactly like a process's proc_sched slot.
        frame_     = b_.CreateAlloca(frame_ty_, nullptr, "frame");
        sched_ptr_ = b_.CreateAlloca(b_.getInt32Ty(), nullptr, "task_sched");

        auto* trap_bb    = llvm::BasicBlock::Create(ctx_, "coro.trap", fn_);
        auto* free_bb    = llvm::BasicBlock::Create(ctx_, "coro.free", fn_);
        coro_cleanup_bb_ = llvm::BasicBlock::Create(ctx_, "coro.cleanup", fn_);
        coro_ret_bb_     = llvm::BasicBlock::Create(ctx_, "coro.ret", fn_);

        b_.SetInsertPoint(coro_cleanup_bb_);
        auto* freed = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_free), { cid, hdl_ });
        b_.CreateCondBr(b_.CreateICmpNE(freed, null), free_bb, coro_ret_bb_);

        b_.SetInsertPoint(free_bb);
        b_.CreateCall(free_fn(), { freed });
        b_.CreateBr(coro_ret_bb_);

        b_.SetInsertPoint(coro_ret_bb_);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_end), { hdl_, b_.getFalse(), none });
        b_.CreateRet(hdl_);

        // Initial suspend: park after building the frame, return the handle without
        // running the body. The caller runs the body on the first resume (case 0).
        b_.SetInsertPoint(begin_bb);
        auto* start_bb = llvm::BasicBlock::Create(ctx_, "coro.start", fn_);
        auto* init     = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getFalse() });
        auto* sw_init  = b_.CreateSwitch(init, coro_ret_bb_, 2);
        sw_init->addCase(b_.getInt8(0), start_bb);
        sw_init->addCase(b_.getInt8(1), coro_cleanup_bb_);

        // The completion epilogue and the final suspend are shared by the
        // fall-through end and every explicit `return` (lower_ret branches here).
        auto* epilogue_bb = llvm::BasicBlock::Create(ctx_, "task.epilogue", fn_);
        auto* final_bb    = llvm::BasicBlock::Create(ctx_, "coro.final", fn_);
        task_epilogue_bb_ = epilogue_bb;

        // Body: initialize the frame, copy parameters in (per direction), then run.
        b_.SetInsertPoint(start_bb);
        init_frame();
        build_str_temps();
        emit_task_copyin(f);
        lower_region(ir_->root());
        branch_to(epilogue_bb);

        // Epilogue: copy output / inout formals back through their pointers, then
        // resume the caller (the record's parent, if any) and fall into the final
        // suspend. In the synchronous path the caller never wrote a parent (it reads
        // the task `done` and resumes inline), so the load is null and nothing is
        // enqueued; in the blocked path the caller stored its handle before
        // suspending, so we push it onto the active queue here.
        b_.SetInsertPoint(epilogue_bb);
        emit_task_copyout(f);
        deinit_body_strings(); // string locals + temps die with the call
        llvm::Value* parent = b_.CreateLoad(ptr_ty, fn_->getArg(task_record_param(f)));
        auto*        has_bb = llvm::BasicBlock::Create(ctx_, "task.wake", fn_);
        b_.CreateCondBr(b_.CreateICmpNE(parent, null), has_bb, final_bb);
        b_.SetInsertPoint(has_bb);
        emit_active_push(b_, parent);
        b_.CreateBr(final_bb);

        b_.SetInsertPoint(final_bb);
        auto* res = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getTrue() });
        auto* sw  = b_.CreateSwitch(res, coro_ret_bb_, 2);
        sw->addCase(b_.getInt8(0), trap_bb);
        sw->addCase(b_.getInt8(1), coro_cleanup_bb_);

        b_.SetInsertPoint(trap_bb);
        b_.CreateUnreachable();

        cur_fn_           = nullptr;
        cur_is_task_      = false;
        frame_            = nullptr;
        task_epilogue_bb_ = nullptr;
    }

    // Prologue: copy each formal's parameter into its frame Local (Local k == formal
    // k). Inputs read their value planes; inouts load through their caller pointer;
    // outputs keep the frame's default (X / 0) until the body assigns them.
    void emit_task_copyin(const Function& f) {
        for (uint32_t k = 0; k < f.param_types.size(); ++k) {
            const uint32_t w    = width_of(f.param_types[k]);
            const uint32_t base = param_llvm_index_[k];
            if (f.arg_dirs[k] == ArgDir::In) {
                if (is_string_type(f.param_types[k])) {
                    // Deep copy the caller's handle into the frame local (value
                    // semantics; the frame slot was init'd by init_frame).
                    b_.CreateCall(str_assign_fn(),
                                  { b_.CreateStructGEP(frame_ty_, frame_, k), fn_->getArg(base) });
                    continue;
                }
                lower_store(local_lvalue(k), Plane{ fn_->getArg(base), fn_->getArg(base + 1) }, from_ref(f.param_types[k]));
            } else if (f.arg_dirs[k] == ArgDir::Inout) {
                llvm::Value* mm = b_.CreateLoad(int_ty(2 * w), fn_->getArg(base));
                llvm::Value* z  = b_.CreateLoad(int_ty(w), fn_->getArg(base + 1));
                lower_store(local_lvalue(k), Plane{ mm, z }, from_ref(f.param_types[k]));
            }
        }
    }

    // Epilogue: copy each output / inout formal's frame Local back through its
    // caller pointer (LRM 13.5 copy-out at completion).
    void emit_task_copyout(const Function& f) {
        for (uint32_t k = 0; k < f.param_types.size(); ++k) {
            if (f.arg_dirs[k] == ArgDir::In) {
                continue;
            }
            const uint32_t base = param_llvm_index_[k];
            const Plane    v    = lower_load(local_lvalue(k));
            b_.CreateStore(v.mm, fn_->getArg(base));
            b_.CreateStore(v.z, fn_->getArg(base + 1));
        }
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

    // Suspend the coroutine and continue in a fresh resume block named `label`.
    // The save token pairs the suspend with this resume point; the default edge
    // returns control to the resumer (coro.ret), case 0 resumes in the new block,
    // case 1 is a destroy while suspended. Shared by every suspend site (#delay,
    // #0, event wait).
    void emit_suspend(const char* label) {
        auto* save   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_save), { hdl_ });
        auto* susp   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { save, b_.getFalse() });
        auto* resume = llvm::BasicBlock::Create(ctx_, label, fn_);
        auto* sw     = b_.CreateSwitch(susp, coro_ret_bb_, 2);
        sw->addCase(b_.getInt8(0), resume);
        sw->addCase(b_.getInt8(1), coro_cleanup_bb_);
        b_.SetInsertPoint(resume);
    }

    // Lower a #delay, then suspend; execution continues in a fresh resume block.
    //
    // A zero delay (#0) does not advance time: per the LRM the process parks in
    // the inactive queue and is promoted back to active at the same instant,
    // after everything else there settles. So route ticks==0 to the inactive
    // queue (no wheel timer, no awaiter) and a nonzero delay to a wheel timer for
    // `now + ticks`. When the amount is a compile-time constant we emit only the
    // taken path; otherwise we branch at runtime (covers a delay expression that
    // happens to evaluate to 0, which is also inactive).
    void lower_delay(Index amount) {
        llvm::Value* ticks = cast(must_of(value_of(amount)), 64, false);

        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(ticks)) {
            if (c->isZero()) {
                emit_inactive_push(b_, hdl_);
            } else {
                emit_delay_timer(ticks);
            }
            emit_suspend("coro.resume");
            return;
        }

        // Runtime amount: branch on zero. Both arms fall through to one suspend.
        auto* zero  = llvm::BasicBlock::Create(ctx_, "delay.zero", fn_);
        auto* timer = llvm::BasicBlock::Create(ctx_, "delay.timer", fn_);
        auto* susp  = llvm::BasicBlock::Create(ctx_, "delay.susp", fn_);
        b_.CreateCondBr(b_.CreateICmpEQ(ticks, b_.getInt64(0)), zero, timer);

        b_.SetInsertPoint(zero);
        emit_inactive_push(b_, hdl_);
        b_.CreateBr(susp);

        b_.SetInsertPoint(timer);
        emit_delay_timer(ticks);
        b_.CreateBr(susp);

        b_.SetInsertPoint(susp);
        emit_suspend("coro.resume");
    }

    // Schedule a wheel timer for `now + ticks` that resumes this coroutine: stash
    // the handle in the awaiter so the timer callback can find it, then arm the
    // wheel. Caller emits the suspend.
    void emit_delay_timer(llvm::Value* ticks) {
        auto* aw = awaiter();
        b_.CreateStore(hdl_, b_.CreateStructGEP(awaiter_ty(), aw, 2));
        llvm::Value* now      = b_.CreateCall(wheel_now_fn(), { wheel_ });
        llvm::Value* deadline = b_.CreateAdd(now, ticks);
        b_.CreateCall(wheel_schedule_fn(), { wheel_, aw, deadline, enqueue_trampoline() });
    }

    // Store a { handle, flag_ptr } entry at a chosen slot of a queue array. The
    // edge is no longer stored per entry — it is encoded by which sub-queue the
    // entry lives in.
    void emit_entry(llvm::ArrayType* arr_ty, llvm::Value* q, llvm::Value* slot_idx, llvm::Value* handle,
                    llvm::Value* flag_ptr) {
        llvm::StructType* elem = wait_elem_ty();
        llvm::Value*      slot = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), slot_idx });
        b_.CreateStore(handle, b_.CreateStructGEP(elem, slot, 0));
        b_.CreateStore(flag_ptr, b_.CreateStructGEP(elem, slot, 1));
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
        const uint32_t slot = sta_next_[i][edge]++;
        emit_entry(wait_sta_ty_[i][edge], wait_sta_gv_[i][edge], b_.getInt32(slot), handle, flag_ptr);
    }

    // One-shot (intra-body @(sig)) registration: append this coroutine to the
    // signal's dynamic queue, bumping .dyn.len. No bounds check — capacity is the
    // count of distinct processes that intra-body-wait on the signal, and a process
    // holds at most one dynamic slot per signal at a time.
    void emit_dynamic_register(Signal sig, uint32_t edge) {
        const auto   i      = static_cast<uint32_t>(sig);
        auto*        len_gv = wait_dyn_len_gv_[i][edge];
        auto*        i32    = b_.getInt32Ty();
        llvm::Value* len    = b_.CreateLoad(i32, len_gv);
        emit_entry(wait_dyn_ty_[i][edge], wait_dyn_gv_[i][edge], len, hdl_, sched_ptr_);
        b_.CreateStore(b_.CreateAdd(len, b_.getInt32(1)), len_gv);
    }

    // Suspend the coroutine and continue in a fresh resume block, woken by a wait
    // queue rather than a wheel timer. Like a #delay's suspend: the default edge
    // returns to the resumer, case 0 resumes here, case 1 is a destroy while
    // suspended.
    void emit_wait_suspend() {
        emit_suspend("coro.wait_resume");
    }

    // Lower an intra-body `@(a or posedge b ...)` event wait: register on EVERY
    // listed (signal, edge)'s dynamic queue, arm our scheduled flag, then suspend
    // once. A matching blocking store to any of the signals wakes us
    // (service_dynamic_queue pushes our handle onto the active queue and clears
    // that bucket's length); the scheduler resumes us in the fresh resume block.
    //
    // Flag arming: service_dynamic_queue now reads the flag, so we must store 0 to
    // our &proc_sched slot before suspending or we would never be woken again.
    //
    // Eager removal (multi-signal only): when we resume, the bucket that fired is
    // already cleared, but the OTHER buckets still hold our stale entry. Left in
    // place they would spuriously wake us later and, in a forever loop, grow those
    // buckets past their capacity (overflow). So compact ourselves out of each
    // bucket on resume. A single-signal wait is in exactly one bucket, cleared on
    // fire, so it needs no removal.
    void lower_wait(ExtraIndex info) {
        const auto events = ir_->wait_edges(info);
        for (const WaitEdge& e : events) {
            emit_dynamic_register(e.signal, e.edge);
        }
        b_.CreateStore(b_.getInt32(0), sched_ptr_); // arm: ready to be woken
        emit_wait_suspend();                        // park until any event fires
        if (events.size() > 1) {
            for (const WaitEdge& e : events) {
                emit_dynamic_remove(e.signal, e.edge);
            }
        }
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

        llvm::AllocaInst* state = b_.CreateAlloca(state_ty_, nullptr, "state");
        init_state(state);

        // Timing wheel: the event scheduler. Processes schedule timers on it at
        // #delay and the drain loop ticks it to advance time and resume them.
        auto* wheel_ty = llvm::ArrayType::get(b_.getInt8Ty(), sizeof(rt_wheel));
        auto* wheel    = b_.CreateAlloca(wheel_ty, nullptr, "wheel");
        wheel->setAlignment(llvm::Align(alignof(rt_wheel)));
        b_.CreateCall(wheel_init_fn(), { wheel });

        // Seed the %t formatter with the design tick size (fs) and the default
        // time-unit exponent (smallest design precision) before any process runs.
        b_.CreateCall(timeformat_init_fn(),
                      { b_.getInt64(mod_.tick_fs), b_.getInt32(static_cast<uint32_t>(mod_.timeformat_units)) });

        emit_active_run(state, wheel, procs);

        // Release heap owned by string signals. A $finish longjmps out of the
        // design before reaching this, so it covers designs that drain
        // naturally; a $finish run's string heap is reclaimed at process exit.
        for (const SignalSlot& s : slots_) {
            if (s.is_string) {
                emit_string_slot_op(state, s, str_deinit_fn());
            }
        }

        b_.CreateCall(wheel_deinit_fn(), { wheel });
        b_.CreateRet(llvm::ConstantInt::get(i32, 0));
    }

    // Per-(signal, edge) capacities for the two wait queues. Indexed
    // [signal][static_cast<uint32_t>(edge)] (0=pos, 1=neg, 2=any).
    struct WaitCounts {
        // Static queue: one slot per sensitivity wait event (an always block's
        // @(edge sig)). Membership is fixed for the whole run, so this is exact.
        std::vector<std::array<uint32_t, 3>> sta;
        // Dynamic queue: one slot per distinct process that does an intra-body
        // @(edge sig). A process holds at most one such slot per (signal, edge)
        // at a time.
        std::vector<std::array<uint32_t, 3>> dyn;
    };

    WaitCounts wait_counts() const {
        const auto n = mod_.signals().size();
        WaitCounts c{ std::vector<std::array<uint32_t, 3>>(n, { 0, 0, 0 }),
                      std::vector<std::array<uint32_t, 3>>(n, { 0, 0, 0 }) };
        for (const Process& p : mod_.processes()) {
            for (const WaitEvent& w : p.wait_events) {
                c.sta[static_cast<uint32_t>(w.signal)][static_cast<uint32_t>(w.edge)]++;
            }
            // Gather the distinct (signal, edge) pairs this process intra-body
            // waits on, across every Wait's event list. A process is a single
            // coroutine suspended at one wait at a time, so it holds at most one
            // dynamic slot per (signal, edge) — count each distinct pair once.
            std::vector<std::pair<Signal, uint32_t>> seen;
            for (size_t i = 0; i < p.ir.inst_count(); ++i) {
                const auto idx = static_cast<Index>(i);
                if (p.ir.tag(idx) != Inst::Tag::Wait) {
                    continue;
                }
                for (const WaitEdge& we : p.ir.wait_edges(p.ir.payload(idx).wait)) {
                    const Signal   s   = we.signal;
                    const uint32_t e   = we.edge;
                    bool           dup = false;
                    for (const auto& pr : seen) {
                        dup = dup || (pr.first == s && pr.second == e);
                    }
                    if (!dup) {
                        seen.emplace_back(s, e);
                        c.dyn[static_cast<uint32_t>(s)][e]++;
                    }
                }
            }
        }

        // Tasks also do intra-body @event waits, but a task is a coroutine created
        // per call and not counted above. Without fork, each process is a single
        // call-stack, so at most one task frame per process chain is suspended on a
        // given (signal, edge) at once — bound the extra dynamic slots by the
        // process count for every (signal, edge) any task waits on. (#delay uses the
        // wheel, not these queues, so it needs no capacity here.)
        const auto chains = static_cast<uint32_t>(mod_.processes().size());
        if (chains > 0) {
            std::vector<std::array<bool, 3>> task_waits(n, { false, false, false });
            for (size_t fi = 0; fi < mod_.function_count(); ++fi) {
                const Function& f = mod_.function(static_cast<FunctionId>(fi));
                if (!f.is_task) {
                    continue;
                }
                for (size_t i = 0; i < f.ir.inst_count(); ++i) {
                    const auto idx = static_cast<Index>(i);
                    if (f.ir.tag(idx) != Inst::Tag::Wait) {
                        continue;
                    }
                    for (const WaitEdge& we : f.ir.wait_edges(f.ir.payload(idx).wait)) {
                        task_waits[static_cast<uint32_t>(we.signal)][we.edge] = true;
                    }
                }
            }
            for (uint32_t s = 0; s < n; ++s) {
                for (uint32_t e = 0; e < 3; ++e) {
                    if (task_waits[s][e]) {
                        c.dyn[s][e] += chains;
                    }
                }
            }
        }
        return c;
    }

    // One wait-queue entry, shared by both the static and dynamic queues:
    // { ptr handle, ptr flag_ptr }.
    //   handle   — the suspended coroutine to resume.
    //   flag_ptr — &clotho.proc_sched[process]. Both queues read it to avoid
    //              waking a process twice when several of the signals it waits on
    //              change together (a multi-signal sensitivity or a multi-signal
    //              @(a or b) intra-body wait).
    // The persistent/transient distinction is encoded by which queue the entry
    // lives in (.sta vs .dyn); the edge is encoded by which per-edge sub-queue it
    // lives in (.pos/.neg/.any). Neither is a per-entry field anymore.
    llvm::StructType* wait_elem_ty() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llvm::StructType::get(ctx_, { ptr, ptr });
    }

    // The .pos/.neg/.any suffix for an edge bucket index (0=pos, 1=neg, 2=any).
    static const char* edge_suffix(uint32_t edge) {
        switch (static_cast<Edge>(edge)) {
            case Edge::Positive: return "pos";
            case Edge::Negative: return "neg";
            case Edge::Any: return "any";
        }
        return "any";
    }

    // Per-signal wait queues, split into up to three per-edge sub-queues, each
    // created only if it has at least one waiter:
    //
    //   clotho.wait.<sig>.sta.{pos,neg,any}  — persistent always-block
    //     subscriptions for that edge. Filled at startup (priming) and never
    //     pushed/popped; serviced by a fixed-trip scan (capacity is a compile-time
    //     constant, so the optimizer can unroll it). No length global.
    //   clotho.wait.<sig>.dyn.{pos,neg,any}  — one-shot intra-body @(edge sig)
    //     waiters for that edge, each with its own .len of live entries; serviced
    //     by a straight scan that wakes every entry (they all share the matched
    //     edge) and resets the length.
    //
    // Splitting by edge hoists the per-entry edge test out of the scan: servicing
    // guards each whole bucket with a single branch on the matching transition
    // fact. Both kinds share the { handle, flag_ptr } element type.
    void build_wait_queues() {
        const auto       sigs = mod_.signals();
        const WaitCounts c    = wait_counts();
        wait_sta_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        wait_sta_ty_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        wait_sta_cap_.assign(sigs.size(), { 0, 0, 0 });
        wait_dyn_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        wait_dyn_ty_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        wait_dyn_len_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        sta_next_.assign(sigs.size(), { 0, 0, 0 });

        auto* i32  = llvm::Type::getInt32Ty(ctx_);
        auto* elem = wait_elem_ty();
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            for (uint32_t e = 0; e < 3; ++e) {
                const std::string base = "clotho.wait." + sigs[i].name + "." ;
                if (c.sta[i][e] > 0) {
                    auto* arr           = llvm::ArrayType::get(elem, c.sta[i][e]);
                    wait_sta_ty_[i][e]  = arr;
                    wait_sta_cap_[i][e] = c.sta[i][e];
                    wait_sta_gv_[i][e]  = new llvm::GlobalVariable(
                      llmod_, arr, false, llvm::GlobalValue::InternalLinkage,
                      llvm::ConstantAggregateZero::get(arr), base + "sta." + edge_suffix(e));
                }
                if (c.dyn[i][e] > 0) {
                    auto* arr          = llvm::ArrayType::get(elem, c.dyn[i][e]);
                    wait_dyn_ty_[i][e] = arr;
                    wait_dyn_gv_[i][e] = new llvm::GlobalVariable(
                      llmod_, arr, false, llvm::GlobalValue::InternalLinkage,
                      llvm::ConstantAggregateZero::get(arr), base + "dyn." + edge_suffix(e));
                    wait_dyn_len_gv_[i][e] = new llvm::GlobalVariable(
                      llmod_, i32, false, llvm::GlobalValue::InternalLinkage,
                      llvm::ConstantInt::get(i32, 0), base + "dyn." + edge_suffix(e) + ".len");
                }
            }
        }
    }

    // Build the nonblocking-assignment update state: per-(width, partial) commit
    // worklists, the dense pending-flag array, and a constant per-signal metadata
    // table (live/shadow/mask offsets plus the wait-queue base/cap/len pointers the
    // generic commit loop services from). Must run after build_state_layout (slot
    // offsets, nba flags) and build_wait_queues (queue globals).
    void build_nba_state() {
        auto* i8    = llvm::Type::getInt8Ty(ctx_);
        auto* i32   = llvm::Type::getInt32Ty(ctx_);
        auto* ptr   = llvm::PointerType::getUnqual(ctx_);
        auto* arr3p = llvm::ArrayType::get(ptr, 3);
        auto* arr3i = llvm::ArrayType::get(i32, 3);
        nba_meta_ty_ = llvm::StructType::get(ctx_, { i32, i32, i32, i32, i32, arr3p, arr3i, arr3p, arr3p });

        const auto sigs = mod_.signals();

        // Assign each NBA target to a (width, partial) bucket.
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            SignalSlot& s = slots_[i];
            if (!s.nba) {
                continue;
            }
            uint32_t bi = 0;
            for (; bi < nba_buckets_.size(); ++bi) {
                if (nba_buckets_[bi].width == s.width && nba_buckets_[bi].partial == s.nba_partial &&
                    nba_buckets_[bi].has_z == s.has_z) {
                    break;
                }
            }
            if (bi == nba_buckets_.size()) {
                nba_buckets_.push_back(NbaBucket{ s.width, s.nba_partial, s.has_z, nullptr, nullptr, nullptr });
            }
            s.nba_bucket = bi;
        }
        std::vector<uint32_t> caps(nba_buckets_.size(), 0);
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            if (slots_[i].nba) {
                caps[slots_[i].nba_bucket]++;
            }
        }
        for (uint32_t bi = 0; bi < nba_buckets_.size(); ++bi) {
            NbaBucket&        b    = nba_buckets_[bi];
            const std::string name = "clotho.nba.w" + std::to_string(b.width) + (b.partial ? ".part" : ".full") +
                                     (b.has_z ? "" : ".noz");
            b.ty  = llvm::ArrayType::get(i32, std::max<uint32_t>(caps[bi], 1));
            b.gv  = new llvm::GlobalVariable(llmod_, b.ty, false, llvm::GlobalValue::InternalLinkage,
                                             llvm::ConstantAggregateZero::get(b.ty), name);
            b.top = new llvm::GlobalVariable(llmod_, i32, false, llvm::GlobalValue::InternalLinkage,
                                             llvm::ConstantInt::get(i32, 0), name + ".top");
        }

        // Dense dedup flags, one per nba-id.
        nba_pending_ty_ = llvm::ArrayType::get(i8, std::max<uint32_t>(n_nba_, 1));
        nba_pending_gv_ = new llvm::GlobalVariable(llmod_, nba_pending_ty_, false, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantAggregateZero::get(nba_pending_ty_),
                                                   "clotho.nba_pending");

        // Constant metadata, one record per nba-id (signal order, matching
        // nba_id). The commit loop walks state with runtime byte offsets read
        // from here, so the DataLayout offsets of the typed fields are baked in
        // as i32 constants.
        const llvm::DataLayout&      dlay = llmod_.getDataLayout();
        auto*                        nullp = llvm::ConstantPointerNull::get(ptr);
        std::vector<llvm::Constant*> metas;
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            const SignalSlot& s = slots_[i];
            if (!s.nba) {
                continue;
            }
            const uint32_t live_off = path_offset(s.path);
            const std::array<uint32_t, 2> sh_mm_path{ shadow_field_, s.sh_mm_field };
            const std::array<uint32_t, 2> sh_z_path{ shadow_field_, s.sh_z_field };
            const std::array<uint32_t, 2> sh_mask_path{ shadow_field_, s.sh_mask_field };
            const uint32_t off_mm = live_off;
            // A never-z slot has no z field (nor a z shadow); its commit bucket
            // never reads these, so bake benign zeros.
            const uint32_t off_z =
              s.has_z ? live_off + static_cast<uint32_t>(dlay.getStructLayout(s.slot_ty)->getElementOffset(1)) : 0;
            const uint32_t off_nmm  = path_offset(sh_mm_path);
            const uint32_t off_nz   = s.has_z ? path_offset(sh_z_path) : 0;
            const uint32_t off_mask = s.nba_partial ? path_offset(sh_mask_path) : 0;

            std::array<llvm::Constant*, 3> sb{}, sc{}, db{}, dl{};
            for (uint32_t e = 0; e < 3; ++e) {
                sb[e] = wait_sta_gv_[i][e] ? llvm::cast<llvm::Constant>(wait_sta_gv_[i][e]) : nullp;
                sc[e] = llvm::ConstantInt::get(i32, wait_sta_cap_[i][e]);
                db[e] = wait_dyn_gv_[i][e] ? llvm::cast<llvm::Constant>(wait_dyn_gv_[i][e]) : nullp;
                dl[e] = wait_dyn_len_gv_[i][e] ? llvm::cast<llvm::Constant>(wait_dyn_len_gv_[i][e]) : nullp;
            }
            metas.push_back(llvm::ConstantStruct::get(
              nba_meta_ty_, { llvm::ConstantInt::get(i32, off_mm), llvm::ConstantInt::get(i32, off_z),
                              llvm::ConstantInt::get(i32, off_nmm), llvm::ConstantInt::get(i32, off_nz),
                              llvm::ConstantInt::get(i32, off_mask), llvm::ConstantArray::get(arr3p, sb),
                              llvm::ConstantArray::get(arr3i, sc), llvm::ConstantArray::get(arr3p, db),
                              llvm::ConstantArray::get(arr3p, dl) }));
        }
        if (metas.empty()) {
            metas.push_back(llvm::ConstantAggregateZero::get(nba_meta_ty_));
        }
        nba_meta_arr_ty_ = llvm::ArrayType::get(nba_meta_ty_, metas.size());
        nba_meta_gv_     = new llvm::GlobalVariable(llmod_, nba_meta_arr_ty_, true, llvm::GlobalValue::InternalLinkage,
                                                    llvm::ConstantArray::get(nba_meta_arr_ty_, metas), "clotho.nba_meta");
    }

    bool signal_has_waiters(Signal sig) const {
        const auto i = static_cast<uint32_t>(sig);
        for (uint32_t e = 0; e < 3; ++e) {
            if (wait_sta_gv_[i][e] != nullptr || wait_dyn_gv_[i][e] != nullptr) {
                return true;
            }
        }
        return false;
    }

    // Load the packed must|may word of an lvalue's selected storage: the whole
    // signal for a plain target, or one element for an unpacked-array element.
    // Used to snapshot old / new state around a blocking store so wait-queue
    // servicing detects an element-level change, not just element 0.
    llvm::Value* load_lv_mm(const LValue& lv) {
        return b_.CreateLoad(int_ty(2 * lv.store_width), lv_mm_ptr(lv));
    }

    // Service a signal's wait queues after a blocking assignment to it. Computes
    // the transition's edge facts once, then for each per-edge bucket that exists
    // guards its whole scan with a single branch on the matching fact — no
    // per-entry edge check. Emits nothing for a signal with no waiters.
    //
    // The three buckets are serviced independently (not else-if): a 0->1 LSB
    // transition is both `changed` (any) and `pos`, so it services both buckets.
    void service_wait_queue(Signal sig, llvm::Value* old_mm, llvm::Value* new_mm) {
        const auto i = static_cast<uint32_t>(sig);
        if (!signal_has_waiters(sig)) {
            return;
        }
        // Edge-detection facts, computed once for all entries. The LSB of `must` is
        // bit 0 of the packed word; Any compares the whole word.
        llvm::Value* old_lsb = b_.CreateTrunc(old_mm, b_.getInt1Ty());
        llvm::Value* new_lsb = b_.CreateTrunc(new_mm, b_.getInt1Ty());
        llvm::Value* pos     = b_.CreateAnd(b_.CreateNot(old_lsb), new_lsb);
        llvm::Value* neg     = b_.CreateAnd(old_lsb, b_.CreateNot(new_lsb));
        llvm::Value* changed = b_.CreateICmpNE(old_mm, new_mm);

        // Bucket -> matching fact. `pos`/`neg` each imply `changed`, so the Any
        // bucket also fires on an LSB transition.
        const std::array<std::pair<uint32_t, llvm::Value*>, 3> buckets = {
            std::make_pair(static_cast<uint32_t>(Edge::Any), changed),
            std::make_pair(static_cast<uint32_t>(Edge::Positive), pos),
            std::make_pair(static_cast<uint32_t>(Edge::Negative), neg),
        };
        for (const auto& [e, fact] : buckets) {
            if (wait_sta_gv_[i][e] == nullptr && wait_dyn_gv_[i][e] == nullptr) {
                continue;
            }
            auto* fn   = b_.GetInsertBlock()->getParent();
            auto* body = llvm::BasicBlock::Create(ctx_, "wait.body", fn);
            auto* cont = llvm::BasicBlock::Create(ctx_, "wait.cont", fn);
            b_.CreateCondBr(fact, body, cont);

            b_.SetInsertPoint(body);
            if (wait_sta_gv_[i][e] != nullptr) {
                service_static_queue(sig, e);
            }
            if (wait_dyn_gv_[i][e] != nullptr) {
                service_dynamic_queue(sig, e);
            }
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
        }
    }

    // Static sub-queue scan (one edge bucket): a fixed-trip loop over the
    // persistent subscriptions for this (signal, edge). The bucket's edge already
    // matched the transition (the caller guarded the whole scan), so there is no
    // per-entry edge check. The capacity is a compile-time constant, so the
    // optimizer may unroll. Membership never changes — no compaction, no length.
    // Each entry wakes its process, but only if it is not already scheduled this
    // instant (its flag is 0), then sets the flag; that flag collapses a
    // multi-signal sensitivity (the same process subscribed under several signals)
    // into a single wake.
    void service_static_queue(Signal sig, uint32_t edge) {
        const auto        i      = static_cast<uint32_t>(sig);
        auto*             q      = wait_sta_gv_[i][edge];
        auto*             arr_ty = wait_sta_ty_[i][edge];
        const uint32_t    cap    = wait_sta_cap_[i][edge];
        llvm::StructType* elem   = wait_elem_ty();
        auto*             ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*             i32    = b_.getInt32Ty();

        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* head  = llvm::BasicBlock::Create(ctx_, "sta.head", fn);
        auto* body  = llvm::BasicBlock::Create(ctx_, "sta.body", fn);
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
        llvm::Value* src     = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), idx });
        llvm::Value* h       = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 0));
        llvm::Value* fptr    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 1));
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

    // Dynamic sub-queue scan (one edge bucket): a straight loop over the live
    // one-shot waiters for this (signal, edge). The bucket's edge already matched
    // the transition (the caller guarded the whole scan), so every entry fires.
    // Each entry wakes its process, but only if it is not already scheduled this
    // instant (its flag is 0), then sets the flag — collapsing a multi-signal
    // intra-body wait (the same process registered under several signals firing
    // together) into a single wake. The whole bucket is consumed, so the length
    // resets to 0; the firing process deregisters its stale entries from any OTHER
    // buckets itself on resume (eager removal in lower_wait).
    void service_dynamic_queue(Signal sig, uint32_t edge) {
        const auto        i      = static_cast<uint32_t>(sig);
        auto*             q      = wait_dyn_gv_[i][edge];
        auto*             arr_ty = wait_dyn_ty_[i][edge];
        auto*             len_gv = wait_dyn_len_gv_[i][edge];
        llvm::StructType* elem   = wait_elem_ty();
        auto*             ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*             i32    = b_.getInt32Ty();

        llvm::Value* n     = b_.CreateLoad(i32, len_gv);
        auto*        fn    = b_.GetInsertBlock()->getParent();
        auto*        head  = llvm::BasicBlock::Create(ctx_, "dyn.head", fn);
        auto*        body  = llvm::BasicBlock::Create(ctx_, "dyn.body", fn);
        auto*        wake  = llvm::BasicBlock::Create(ctx_, "dyn.wake", fn);
        auto*        cont  = llvm::BasicBlock::Create(ctx_, "dyn.cont", fn);
        auto*        done  = llvm::BasicBlock::Create(ctx_, "dyn.done", fn);
        auto*        entry = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* i_phi = b_.CreatePHI(i32, 2);
        i_phi->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(i_phi, n), body, done);

        b_.SetInsertPoint(body);
        llvm::Value* src     = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), i_phi });
        llvm::Value* h       = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 0));
        llvm::Value* fptr    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 1));
        llvm::Value* already = b_.CreateICmpNE(b_.CreateLoad(i32, fptr), b_.getInt32(0));
        b_.CreateCondBr(already, cont, wake);

        b_.SetInsertPoint(wake);
        emit_active_push(b_, h);
        b_.CreateStore(b_.getInt32(1), fptr); // mark scheduled
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        llvm::Value* i_next = b_.CreateAdd(i_phi, b_.getInt32(1));
        b_.CreateBr(head);
        i_phi->addIncoming(i_next, cont);

        b_.SetInsertPoint(done);
        b_.CreateStore(b_.getInt32(0), len_gv);
    }

    // Compacting removal of this coroutine's entries from one (signal, edge)
    // dynamic bucket, used on resume from a multi-signal wait: the firing bucket
    // already cleared us, but the others still hold our now-stale entry. Slides
    // every entry whose handle != hdl_ down to fill the gaps and trims the length:
    //   w = 0; for i in 0..len: if Q[i].handle != hdl_ { Q[w++] = Q[i] } ; len = w
    void emit_dynamic_remove(Signal sig, uint32_t edge) {
        const auto        i      = static_cast<uint32_t>(sig);
        auto*             q      = wait_dyn_gv_[i][edge];
        auto*             arr_ty = wait_dyn_ty_[i][edge];
        auto*             len_gv = wait_dyn_len_gv_[i][edge];
        llvm::StructType* elem   = wait_elem_ty();
        auto*             ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*             i32    = b_.getInt32Ty();

        llvm::Value* n     = b_.CreateLoad(i32, len_gv);
        auto*        fn    = b_.GetInsertBlock()->getParent();
        auto*        head  = llvm::BasicBlock::Create(ctx_, "rm.head", fn);
        auto*        body  = llvm::BasicBlock::Create(ctx_, "rm.body", fn);
        auto*        keep  = llvm::BasicBlock::Create(ctx_, "rm.keep", fn);
        auto*        cont  = llvm::BasicBlock::Create(ctx_, "rm.cont", fn);
        auto*        done  = llvm::BasicBlock::Create(ctx_, "rm.done", fn);
        auto*        entry = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        auto* w   = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        w->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, done);

        b_.SetInsertPoint(body);
        llvm::Value* src  = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), idx });
        llvm::Value* h    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 0));
        llvm::Value* f    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 1));
        llvm::Value* mine = b_.CreateICmpEQ(h, hdl_);
        b_.CreateCondBr(mine, cont, keep); // ours -> drop; else -> keep

        b_.SetInsertPoint(keep);
        llvm::Value* dst = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), w });
        b_.CreateStore(h, b_.CreateStructGEP(elem, dst, 0));
        b_.CreateStore(f, b_.CreateStructGEP(elem, dst, 1));
        llvm::Value* w_keep = b_.CreateAdd(w, b_.getInt32(1));
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        auto* w_next = b_.CreatePHI(i32, 2);
        w_next->addIncoming(w, body);
        w_next->addIncoming(w_keep, keep);
        llvm::Value* idx_next = b_.CreateAdd(idx, b_.getInt32(1));
        b_.CreateBr(head);
        idx->addIncoming(idx_next, cont);
        w->addIncoming(w_next, cont);

        b_.SetInsertPoint(done);
        b_.CreateStore(w, len_gv);
    }

    // The active task queue: ready-to-run coroutine handles held in a fixed-size
    // global used as a LIFO stack (active events may fire in any order per the LRM,
    // so the stack discipline is free). Capacity is the total process count — the
    // most that can ever be ready at once. Created once, lazily, so the timer
    // callback (built while lowering a process) and emit_active_run share it.
    void ensure_active_queue() {
        if (active_gv_ != nullptr) {
            return;
        }
        auto*      ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*      i32    = llvm::Type::getInt32Ty(ctx_);
        // One slot per process suffices even with tasks: the queue is a LIFO drained
        // to empty each region, and (without fork) each process is a single
        // call-stack with at most one ready coroutine at a time — a task handle, or
        // the parent it re-enqueues on completion, reuses that chain's one slot.
        const auto cap    = std::max<size_t>(mod_.processes().size(), 1);

        queue_arr_ty_ = llvm::ArrayType::get(ptr_ty, cap);
        active_gv_    = new llvm::GlobalVariable(llmod_, queue_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                                 llvm::ConstantAggregateZero::get(queue_arr_ty_), "clotho.active");
        // Stack top: index of the next free slot. Starts at 0 (empty).
        active_top_gv_ = new llvm::GlobalVariable(llmod_, i32, false, llvm::GlobalValue::InternalLinkage,
                                                  llvm::ConstantInt::get(i32, 0), "clotho.active_top");
    }

    // The inactive queue: same shape and capacity as the active queue, holding
    // handles parked on a zero delay (#0). Promoted into the active queue at the
    // same instant, after active drains and before NBA (see emit_active_run).
    void ensure_inactive_queue() {
        if (inactive_gv_ != nullptr) {
            return;
        }
        ensure_active_queue(); // queue_arr_ty_ is shared by both queues
        auto* i32    = llvm::Type::getInt32Ty(ctx_);
        inactive_gv_ = new llvm::GlobalVariable(llmod_, queue_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                                llvm::ConstantAggregateZero::get(queue_arr_ty_), "clotho.inactive");
        inactive_top_gv_ = new llvm::GlobalVariable(llmod_, i32, false, llvm::GlobalValue::InternalLinkage,
                                                    llvm::ConstantInt::get(i32, 0), "clotho.inactive_top");
    }

    // Push a handle onto the active queue: queue[top++] = handle.
    void emit_active_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
        ensure_active_queue();
        auto*        i32  = b.getInt32Ty();
        llvm::Value* sp   = b.CreateLoad(i32, active_top_gv_);
        llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, active_gv_, { b.getInt32(0), sp });
        b.CreateStore(handle, slot);
        b.CreateStore(b.CreateAdd(sp, b.getInt32(1)), active_top_gv_);
    }

    // Push a handle onto the inactive queue: inactive[top++] = handle.
    void emit_inactive_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
        ensure_inactive_queue();
        auto*        i32  = b.getInt32Ty();
        llvm::Value* sp   = b.CreateLoad(i32, inactive_top_gv_);
        llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, inactive_gv_, { b.getInt32(0), sp });
        b.CreateStore(handle, slot);
        b.CreateStore(b.CreateAdd(sp, b.getInt32(1)), inactive_top_gv_);
    }

    // -- NBA update region --
    //
    // After the active queue drains, the NBA region commits every staged update:
    // each (width, partial) bucket worklist is streamed by a width-specialized loop
    // that copies (or mask-RMWs) the shadow into live state and services the wait
    // queue. Per-signal offsets and queue pointers come from the constant meta
    // table, so the loop body is generic (no per-signal code, no indirect dispatch).

    // Load a scalar i32 field (0..4) or an array element (fields 5..8) from a meta
    // record pointer. Meta layout: 0 off_mm, 1 off_z, 2 off_nmm, 3 off_nz,
    // 4 off_mask, 5 sta_base[3], 6 sta_cap[3], 7 dyn_base[3], 8 dyn_len[3].
    llvm::Value* meta_i32(llvm::Value* meta, unsigned field) {
        return b_.CreateLoad(b_.getInt32Ty(),
                             b_.CreateInBoundsGEP(nba_meta_ty_, meta, { b_.getInt32(0), b_.getInt32(field) }));
    }
    llvm::Value* meta_ptr_at(llvm::Value* meta, unsigned field, uint32_t e) {
        return b_.CreateLoad(
          llvm::PointerType::getUnqual(ctx_),
          b_.CreateInBoundsGEP(nba_meta_ty_, meta, { b_.getInt32(0), b_.getInt32(field), b_.getInt32(e) }));
    }
    llvm::Value* meta_i32_at(llvm::Value* meta, unsigned field, uint32_t e) {
        return b_.CreateLoad(
          b_.getInt32Ty(),
          b_.CreateInBoundsGEP(nba_meta_ty_, meta, { b_.getInt32(0), b_.getInt32(field), b_.getInt32(e) }));
    }

    // Generic static-queue scan (runtime base + capacity), null base => no waiters.
    // Mirrors service_static_queue but reads its trip count at runtime from meta.
    void emit_static_scan_rt(llvm::Value* base, llvm::Value* cap) {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* elem = wait_elem_ty();
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* head = llvm::BasicBlock::Create(ctx_, "nsta.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx_, "nsta.body", fn);
        auto* wake = llvm::BasicBlock::Create(ctx_, "nsta.wake", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "nsta.cont", fn);
        auto* done = llvm::BasicBlock::Create(ctx_, "nsta.done", fn);
        auto* entry = b_.GetInsertBlock();
        b_.CreateCondBr(b_.CreateICmpNE(base, llvm::ConstantPointerNull::get(ptr)), head, done);

        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, cap), body, done);

        b_.SetInsertPoint(body);
        llvm::Value* src  = b_.CreateInBoundsGEP(elem, base, idx);
        llvm::Value* h    = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, src, 0));
        llvm::Value* fptr = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, src, 1));
        b_.CreateCondBr(b_.CreateICmpNE(b_.CreateLoad(i32, fptr), b_.getInt32(0)), cont, wake);

        b_.SetInsertPoint(wake);
        emit_active_push(b_, h);
        b_.CreateStore(b_.getInt32(1), fptr);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), cont);
        b_.CreateBr(head);

        b_.SetInsertPoint(done);
    }

    // Generic dynamic-queue scan (runtime base + length pointer), null base => skip.
    // Wakes every live entry (flag-deduped) and resets the length to 0.
    void emit_dynamic_scan_rt(llvm::Value* base, llvm::Value* lenptr) {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* elem = wait_elem_ty();
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* chk   = llvm::BasicBlock::Create(ctx_, "ndyn.chk", fn);
        auto* head  = llvm::BasicBlock::Create(ctx_, "ndyn.head", fn);
        auto* body  = llvm::BasicBlock::Create(ctx_, "ndyn.body", fn);
        auto* wake  = llvm::BasicBlock::Create(ctx_, "ndyn.wake", fn);
        auto* cont  = llvm::BasicBlock::Create(ctx_, "ndyn.cont", fn);
        auto* reset = llvm::BasicBlock::Create(ctx_, "ndyn.reset", fn);
        auto* done  = llvm::BasicBlock::Create(ctx_, "ndyn.done", fn);
        b_.CreateCondBr(b_.CreateICmpNE(base, llvm::ConstantPointerNull::get(ptr)), chk, done);

        b_.SetInsertPoint(chk);
        llvm::Value* n = b_.CreateLoad(i32, lenptr);
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), chk);
        b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, reset);

        b_.SetInsertPoint(body);
        llvm::Value* src  = b_.CreateInBoundsGEP(elem, base, idx);
        llvm::Value* h    = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, src, 0));
        llvm::Value* fptr = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, src, 1));
        b_.CreateCondBr(b_.CreateICmpNE(b_.CreateLoad(i32, fptr), b_.getInt32(0)), cont, wake);

        b_.SetInsertPoint(wake);
        emit_active_push(b_, h);
        b_.CreateStore(b_.getInt32(1), fptr);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), cont);
        b_.CreateBr(head);

        b_.SetInsertPoint(reset);
        b_.CreateStore(b_.getInt32(0), lenptr);
        b_.CreateBr(done);

        b_.SetInsertPoint(done);
    }

    // Service an NBA target's wait queues after its shadow committed to live. Same
    // edge facts as service_wait_queue, but the per-edge queue pointers come from
    // the meta record (runtime) so this drives any signal with one body.
    void emit_nba_service(llvm::Value* meta, llvm::Value* old_mm, llvm::Value* new_mm) {
        auto*        i1      = b_.getInt1Ty();
        llvm::Value* old_lsb = b_.CreateTrunc(old_mm, i1);
        llvm::Value* new_lsb = b_.CreateTrunc(new_mm, i1);
        llvm::Value* pos     = b_.CreateAnd(b_.CreateNot(old_lsb), new_lsb);
        llvm::Value* neg     = b_.CreateAnd(old_lsb, b_.CreateNot(new_lsb));
        llvm::Value* changed = b_.CreateICmpNE(old_mm, new_mm);
        const std::array<std::pair<uint32_t, llvm::Value*>, 3> buckets = {
            std::make_pair(static_cast<uint32_t>(Edge::Any), changed),
            std::make_pair(static_cast<uint32_t>(Edge::Positive), pos),
            std::make_pair(static_cast<uint32_t>(Edge::Negative), neg),
        };
        for (const auto& [e, fact] : buckets) {
            auto* fn   = b_.GetInsertBlock()->getParent();
            auto* body = llvm::BasicBlock::Create(ctx_, "nsvc.body", fn);
            auto* cont = llvm::BasicBlock::Create(ctx_, "nsvc.cont", fn);
            b_.CreateCondBr(fact, body, cont);
            b_.SetInsertPoint(body);
            emit_static_scan_rt(meta_ptr_at(meta, 5, e), meta_i32_at(meta, 6, e));
            emit_dynamic_scan_rt(meta_ptr_at(meta, 7, e), meta_ptr_at(meta, 8, e));
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
        }
    }

    // Stream one (width, partial) bucket worklist: commit each staged signal's
    // shadow into live state (whole copy, or mask-RMW for a partial bucket), service
    // its wait queue, clear its pending flag, then reset the worklist. Width is the
    // bucket's compile-time width, so the plane loads/stores are exactly typed.
    void emit_nba_commit_bucket(const NbaBucket& bk, llvm::Value* state) {
        const uint32_t    W     = bk.width;
        auto*             i32   = b_.getInt32Ty();
        auto*             mmty  = int_ty(2 * W);
        auto*             zty   = int_ty(W);
        // The runtime-offset i8 GEPs below erase the field types, so the loads/
        // stores carry the fields' ABI alignment explicitly.
        const llvm::Align mm_al = llmod_.getDataLayout().getABITypeAlign(mmty);
        const llvm::Align z_al  = llmod_.getDataLayout().getABITypeAlign(zty);

        auto*        fn    = b_.GetInsertBlock()->getParent();
        auto*        head  = llvm::BasicBlock::Create(ctx_, "ncommit.head", fn);
        auto*        body  = llvm::BasicBlock::Create(ctx_, "ncommit.body", fn);
        auto*        cont  = llvm::BasicBlock::Create(ctx_, "ncommit.cont", fn);
        auto*        done  = llvm::BasicBlock::Create(ctx_, "ncommit.done", fn);
        llvm::Value* n     = b_.CreateLoad(i32, bk.top);
        auto*        entry = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, done);

        b_.SetInsertPoint(body);
        llvm::Value* id        = b_.CreateLoad(i32, b_.CreateInBoundsGEP(bk.ty, bk.gv, { b_.getInt32(0), idx }));
        llvm::Value* meta      = b_.CreateInBoundsGEP(nba_meta_arr_ty_, nba_meta_gv_, { b_.getInt32(0), id });
        llvm::Value* live_mm_p = field_ptr_var(state, meta_i32(meta, 0));
        // A never-z bucket's targets store no z word / z shadow; skip z entirely.
        llvm::Value* live_z_p = bk.has_z ? field_ptr_var(state, meta_i32(meta, 1)) : nullptr;
        llvm::Value* sh_mm_p  = field_ptr_var(state, meta_i32(meta, 2));
        llvm::Value* sh_z_p   = bk.has_z ? field_ptr_var(state, meta_i32(meta, 3)) : nullptr;
        llvm::Value* old_mm   = b_.CreateAlignedLoad(mmty, live_mm_p, mm_al);
        llvm::Value* new_mm   = nullptr;
        if (!bk.partial) {
            new_mm = b_.CreateAlignedLoad(mmty, sh_mm_p, mm_al);
            b_.CreateAlignedStore(new_mm, live_mm_p, mm_al);
            if (bk.has_z) {
                llvm::Value* new_z = b_.CreateAlignedLoad(zty, sh_z_p, z_al);
                b_.CreateAlignedStore(new_z, live_z_p, z_al);
            }
        } else {
            llvm::Value* mask_p = field_ptr_var(state, meta_i32(meta, 4));
            llvm::Value* mask   = b_.CreateAlignedLoad(zty, mask_p, z_al);
            llvm::Value* sh_mm  = b_.CreateAlignedLoad(mmty, sh_mm_p, mm_al);
            // Replicate the W-bit mask into both halves of the packed must|may word.
            llvm::Value* maskw = b_.CreateZExt(mask, mmty);
            llvm::Value* mask2 = b_.CreateOr(maskw, b_.CreateShl(maskw, llvm::ConstantInt::get(mmty, W)));
            new_mm             = b_.CreateOr(b_.CreateAnd(old_mm, b_.CreateNot(mask2)), b_.CreateAnd(sh_mm, mask2));
            b_.CreateAlignedStore(new_mm, live_mm_p, mm_al);
            if (bk.has_z) {
                llvm::Value* old_z = b_.CreateAlignedLoad(zty, live_z_p, z_al);
                llvm::Value* sh_z  = b_.CreateAlignedLoad(zty, sh_z_p, z_al);
                llvm::Value* new_z = b_.CreateOr(b_.CreateAnd(old_z, b_.CreateNot(mask)), b_.CreateAnd(sh_z, mask));
                b_.CreateAlignedStore(new_z, live_z_p, z_al);
            }
            b_.CreateAlignedStore(zero(W), mask_p, z_al); // clear dirty mask for next slot
        }
        emit_nba_service(meta, old_mm, new_mm);
        b_.CreateStore(b_.getInt8(0),
                       b_.CreateInBoundsGEP(nba_pending_ty_, nba_pending_gv_, { b_.getInt32(0), id }));
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), cont);
        b_.CreateBr(head);

        b_.SetInsertPoint(done);
        b_.CreateStore(b_.getInt32(0), bk.top);
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
        b_.CreateStore(b_.getInt32(0), active_top_gv_);
        const bool has_inactive = inactive_gv_ != nullptr; // some #0 was lowered
        if (has_inactive) {
            b_.CreateStore(b_.getInt32(0), inactive_top_gv_);
        }
        const auto all = mod_.processes();

        // Pre-init pass first (LRM 6.8): apply variable declaration initializers
        // before any Initial / Always process is created or subscribed. A PreInit
        // body has no timing, so resuming it once runs it to completion; its stores
        // generate no events (see lower_process), so the always blocks created
        // below subscribe to already-settled values. Run before the seed loop so no
        // wait queue is scanned before priming.
        for (uint32_t k = 0; k < procs.size(); ++k) {
            if (all[k].activation != Activation::PreInit) {
                continue;
            }
            llvm::Value* hdl = b_.CreateCall(proc_fnty, procs[k], { state, wheel });
            b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
            b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
        }

        for (uint32_t k = 0; k < procs.size(); ++k) {
            const Process& p = all[k];
            if (p.activation == Activation::PreInit) {
                continue; // already run above
            }
            llvm::Value* hdl = b_.CreateCall(proc_fnty, procs[k], { state, wheel });
            if (p.activation == Activation::Always && !p.wait_events.empty()) {
                llvm::Constant* flag = llvm::ConstantExpr::getInBoundsGetElementPtr(
                  sched_arr_ty_, sched_gv_, llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(k) });
                for (const WaitEvent& w : p.wait_events) {
                    emit_static_subscribe(w.signal, static_cast<uint32_t>(w.edge), hdl, flag);
                }
                // A continuously-active process (continuous assign / port copy /
                // always_comb) also runs once now, so it settles from the pre-init
                // values rather than waiting for an input edge that a declaration
                // initializer never generates. It runs body-first then parks on its
                // (already-subscribed) wait, so later input changes still wake it.
                if (p.self_start) {
                    emit_active_push(b_, hdl);
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
        // NBA region: only when the design has nonblocking assignments. Entered when
        // the active queue drains; if any update is staged it commits them all and
        // re-drains the active queue at the SAME instant; otherwise it falls through
        // to the wheel. So active and NBA iterate until both settle before time moves.
        const bool has_nba = !nba_buckets_.empty() || !array_nba_sigs_.empty();
        auto*      ncheck  = has_nba ? llvm::BasicBlock::Create(ctx_, "nba.check", fn) : wcheck;
        auto*      ncommit = has_nba ? llvm::BasicBlock::Create(ctx_, "nba.commit", fn) : nullptr;
        // Inactive region: only when the design has a #0. When the active queue
        // drains, promote the whole inactive batch back into the (now empty)
        // active queue and re-drain at the SAME instant — before NBA. So active
        // and inactive iterate until both settle, then NBA, then the wheel.
        auto* icheck   = has_inactive ? llvm::BasicBlock::Create(ctx_, "inactive.check", fn) : ncheck;
        auto* ipromote = has_inactive ? llvm::BasicBlock::Create(ctx_, "inactive.promote", fn) : nullptr;
        b_.CreateBr(check);

        // Drain the active queue: while top != 0, pop active[--top] and resume it.
        b_.SetInsertPoint(check);
        llvm::Value* sp = b_.CreateLoad(i32, active_top_gv_);
        b_.CreateCondBr(b_.CreateICmpNE(sp, b_.getInt32(0)), body, icheck);

        b_.SetInsertPoint(body);
        llvm::Value* idx  = b_.CreateSub(sp, b_.getInt32(1));
        b_.CreateStore(idx, active_top_gv_);
        llvm::Value* slot = b_.CreateInBoundsGEP(queue_arr_ty_, active_gv_, { b_.getInt32(0), idx });
        llvm::Value* hdl  = b_.CreateLoad(ptr_ty, slot);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        llvm::Value* fin = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { hdl });
        b_.CreateCondBr(fin, destroy, cont);

        b_.SetInsertPoint(destroy);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        b_.CreateBr(check);

        if (has_inactive) {
            // Active queue is empty here. If anything parked on a #0, move the
            // whole batch into the active queue (it is empty, so a plain copy)
            // and re-drain. Promoting the batch all at once is what makes every
            // #0 process at this instant become active together, per the LRM.
            b_.SetInsertPoint(icheck);
            llvm::Value* n = b_.CreateLoad(i32, inactive_top_gv_);
            b_.CreateCondBr(b_.CreateICmpNE(n, b_.getInt32(0)), ipromote, ncheck);

            // for (i = 0; i < n; ++i) active[i] = inactive[i];
            // active_top = n; inactive_top = 0; then loop back to the drain.
            auto* phead = llvm::BasicBlock::Create(ctx_, "inactive.copy", fn);
            auto* pbody = llvm::BasicBlock::Create(ctx_, "inactive.copy.body", fn);
            auto* pdone = llvm::BasicBlock::Create(ctx_, "inactive.copy.done", fn);
            b_.SetInsertPoint(ipromote);
            b_.CreateBr(phead);

            b_.SetInsertPoint(phead);
            auto* i = b_.CreatePHI(i32, 2);
            i->addIncoming(b_.getInt32(0), ipromote);
            b_.CreateCondBr(b_.CreateICmpULT(i, n), pbody, pdone);

            b_.SetInsertPoint(pbody);
            llvm::Value* src = b_.CreateInBoundsGEP(queue_arr_ty_, inactive_gv_, { b_.getInt32(0), i });
            llvm::Value* dst = b_.CreateInBoundsGEP(queue_arr_ty_, active_gv_, { b_.getInt32(0), i });
            b_.CreateStore(b_.CreateLoad(ptr_ty, src), dst);
            i->addIncoming(b_.CreateAdd(i, b_.getInt32(1)), pbody);
            b_.CreateBr(phead);

            b_.SetInsertPoint(pdone);
            b_.CreateStore(n, active_top_gv_);
            b_.CreateStore(b_.getInt32(0), inactive_top_gv_);
            b_.CreateBr(check);
        }

        if (has_nba) {
            // Any staged update? OR each scalar bucket's live count and the deferred
            // array-write queue's length; if so, commit them all and loop back to
            // drain the processes the commits just woke.
            b_.SetInsertPoint(ncheck);
            llvm::Value* any = b_.getFalse();
            for (const NbaBucket& bk : nba_buckets_) {
                any = b_.CreateOr(any, b_.CreateICmpNE(b_.CreateLoad(i32, bk.top), b_.getInt32(0)));
            }
            if (!array_nba_sigs_.empty()) {
                any = b_.CreateOr(any, b_.CreateICmpNE(b_.CreateCall(nba_arr_pending_fn(), {}), b_.getInt32(0)));
            }
            b_.CreateCondBr(any, ncommit, wcheck);

            b_.SetInsertPoint(ncommit);
            for (const NbaBucket& bk : nba_buckets_) {
                emit_nba_commit_bucket(bk, state);
            }
            // Deferred array-element writes: replay them, then wake the Any-edge
            // readers of any array a write touched, then clear the queue.
            if (!array_nba_sigs_.empty()) {
                b_.CreateCall(nba_arr_commit_fn(), {});
                for (const Signal sig : array_nba_sigs_) {
                    if (!signal_has_waiters(sig)) {
                        continue;
                    }
                    auto* fn      = b_.GetInsertBlock()->getParent();
                    auto* svc     = llvm::BasicBlock::Create(ctx_, "nba.arr.svc", fn);
                    auto* nextbb  = llvm::BasicBlock::Create(ctx_, "nba.arr.next", fn);
                    auto* dirty   = b_.CreateCall(nba_arr_dirty_fn(), { b_.getInt32(static_cast<uint32_t>(sig)) });
                    b_.CreateCondBr(b_.CreateICmpNE(dirty, b_.getInt32(0)), svc, nextbb);
                    b_.SetInsertPoint(svc);
                    service_any_waiters(sig);
                    b_.CreateBr(nextbb);
                    b_.SetInsertPoint(nextbb);
                }
                b_.CreateCall(nba_arr_reset_fn(), {});
            }
            b_.CreateBr(check);
        }

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
    // -- Deferred array-NBA queue (rt_nba_arr_*) --
    llvm::FunctionCallee nba_arr_push_fn() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        auto* i32 = b_.getInt32Ty();
        auto* fnty =
          llvm::FunctionType::get(b_.getVoidTy(), { i32, ptr, ptr, ptr, i32, ptr, ptr, ptr, i32 }, false);
        return llmod_.getOrInsertFunction("rt_nba_arr_push", fnty);
    }
    llvm::FunctionCallee nba_arr_pending_fn() {
        return llmod_.getOrInsertFunction("rt_nba_arr_pending", llvm::FunctionType::get(b_.getInt32Ty(), {}, false));
    }
    llvm::FunctionCallee nba_arr_commit_fn() {
        return llmod_.getOrInsertFunction("rt_nba_arr_commit", llvm::FunctionType::get(b_.getVoidTy(), {}, false));
    }
    llvm::FunctionCallee nba_arr_dirty_fn() {
        return llmod_.getOrInsertFunction("rt_nba_arr_dirty",
                                          llvm::FunctionType::get(b_.getInt32Ty(), { b_.getInt32Ty() }, false));
    }
    llvm::FunctionCallee nba_arr_reset_fn() {
        return llmod_.getOrInsertFunction("rt_nba_arr_reset", llvm::FunctionType::get(b_.getVoidTy(), {}, false));
    }

    // -- String runtime (rt_string_*, runtime/Runtime.h) --

    llvm::FunctionCallee str_rt_fn(const char* name, llvm::Type* ret, llvm::ArrayRef<llvm::Type*> params) {
        return llmod_.getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
    }
    llvm::Type* raw_ptr_ty() {
        return llvm::PointerType::getUnqual(ctx_);
    }
    llvm::FunctionCallee str_init_fn() {
        return str_rt_fn("rt_string_init", b_.getVoidTy(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_deinit_fn() {
        return str_rt_fn("rt_string_deinit", b_.getVoidTy(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_assign_fn() {
        return str_rt_fn("rt_string_assign", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_assign_bytes_fn() {
        return str_rt_fn("rt_string_assign_bytes", b_.getVoidTy(),
                         { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
    }
    llvm::FunctionCallee str_len_fn() {
        return str_rt_fn("rt_string_len", b_.getInt64Ty(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_concat_fn() {
        return str_rt_fn("rt_string_concat", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_repl_fn() {
        return str_rt_fn("rt_string_repl", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
    }
    llvm::FunctionCallee str_cmp_fn() {
        return str_rt_fn("rt_string_cmp", b_.getInt32Ty(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_icmp_fn() {
        return str_rt_fn("rt_string_icmp", b_.getInt32Ty(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_substr_fn() {
        return str_rt_fn("rt_string_substr", b_.getVoidTy(),
                         { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_upper_fn() {
        return str_rt_fn("rt_string_toupper", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_lower_fn() {
        return str_rt_fn("rt_string_tolower", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_putc_fn() {
        return str_rt_fn("rt_string_putc", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_getc_fn() {
        return str_rt_fn("rt_string_getc", b_.getInt32Ty(), { raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_from_packed_fn() {
        return str_rt_fn("rt_string_from_packed", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_to_packed_fn() {
        return str_rt_fn("rt_string_to_packed", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
    }

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
    // i32 rt_random(void)
    llvm::FunctionCallee random_fn() {
        return llmod_.getOrInsertFunction("rt_random", llvm::FunctionType::get(b_.getInt32Ty(), {}, false));
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

    // Power-up the state struct: undriven 4-state nets come up Z (LRM 6.6 —
    // must 0, may 1, z all-ones), other 4-state signals X (z 0), 2-state 0.
    void init_state(llvm::Value* state) {
        for (const SignalSlot& s : slots_) {
            if (s.is_string) {
                emit_string_slot_op(state, s, str_init_fn());
                continue;
            }
            if (s.is_array) {
                emit_array_init(state, s);
                continue;
            }
            // X (and the must|may half of Z) packs as the high W bits of the
            // i(2W) must|may word (may=1, must=0).
            llvm::Value* mm = s.four_state
                                ? llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * s.width, s.width))
                                : zero(2 * s.width);
            llvm::Value* slot = storage_ptr(state, s);
            b_.CreateStore(mm, b_.CreateStructGEP(s.slot_ty, slot, 0));
            if (s.has_z) {
                b_.CreateStore(s.init_z ? ones(s.width) : zero(s.width),
                               b_.CreateStructGEP(s.slot_ty, slot, 1));
            }
            // A partial target's dirty mask must start clear; the shadow itself is
            // written before it is read, so it needs no initialisation.
            if (s.nba_partial) {
                b_.CreateStore(zero(s.width),
                               b_.CreateInBoundsGEP(state_ty_, state,
                                                    { b_.getInt32(0), b_.getInt32(shadow_field_),
                                                      b_.getInt32(s.sh_mask_field) }));
            }
        }
    }

    // Apply `callee` (rt_string_init / rt_string_deinit) to a string signal's
    // handle, or to every element handle of an array-of-string signal (a runtime
    // loop, like emit_array_init).
    void emit_string_slot_op(llvm::Value* state, const SignalSlot& s, llvm::FunctionCallee callee) {
        llvm::Value* base = storage_ptr(state, s);
        if (!s.is_array) {
            b_.CreateCall(callee, { base });
            return;
        }
        auto* arr_ty = llvm::cast<llvm::ArrayType>(s.storage_ty);
        auto* i32    = b_.getInt32Ty();
        auto* fn     = b_.GetInsertBlock()->getParent();
        auto* head   = llvm::BasicBlock::Create(ctx_, "strop.head", fn);
        auto* body   = llvm::BasicBlock::Create(ctx_, "strop.body", fn);
        auto* done   = llvm::BasicBlock::Create(ctx_, "strop.done", fn);
        auto* entry  = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(s.elem_count)), body, done);

        b_.SetInsertPoint(body);
        b_.CreateCall(callee, { b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), idx }) });
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), body);
        b_.CreateBr(head);

        b_.SetInsertPoint(done);
    }

    // Initialise an unpacked-array signal: a runtime loop over its elements
    // storing the per-element power-on value (X for 4-state, 0 for 2-state) into
    // each leaf slot. A loop keeps init O(elem_count) emitted instructions, so a
    // large memory does not bloat the IR.
    void emit_array_init(llvm::Value* state, const SignalSlot& s) {
        const uint32_t w   = s.width;
        auto*          i32 = b_.getInt32Ty();
        llvm::Value*   mm  = s.four_state
                               ? llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * w, w))
                               : zero(2 * w);
        auto*        arr_ty = llvm::cast<llvm::ArrayType>(s.storage_ty);
        llvm::Value* base   = storage_ptr(state, s);

        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* head  = llvm::BasicBlock::Create(ctx_, "ainit.head", fn);
        auto* body  = llvm::BasicBlock::Create(ctx_, "ainit.body", fn);
        auto* done  = llvm::BasicBlock::Create(ctx_, "ainit.done", fn);
        auto* entry = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(s.elem_count)), body, done);

        b_.SetInsertPoint(body);
        b_.CreateStore(mm, b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), idx, b_.getInt32(0) }));
        if (s.has_z) {
            // An undriven net array's elements default to z like any net.
            b_.CreateStore(s.init_z ? ones(w) : zero(w),
                           b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), idx, b_.getInt32(1) }));
        }
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), body);
        b_.CreateBr(head);

        b_.SetInsertPoint(done);
    }

    // Pointer to a state field at a runtime byte offset (the NBA commit loop
    // reads its offsets from the meta table, erasing the field types). Offsets
    // are DataLayout field offsets, so accesses stay naturally aligned even
    // though the GEP is dynamic.
    llvm::Value* field_ptr_var(llvm::Value* base, llvm::Value* off) {
        return b_.CreateInBoundsGEP(b_.getInt8Ty(), base, off);
    }

    // Lower a region's instructions in order into the current insert block. A `ret`
    // (function early return) terminates the block; the rest of the region is then
    // unreachable, so stop emitting into the dead tail.
    void lower_region(std::span<const Index> region) {
        for (Index i : region) {
            lower_inst(i);
            if (b_.GetInsertBlock()->getTerminator() != nullptr) {
                return;
            }
        }
    }

    // Branch to `bb` unless the current block is already terminated (e.g. a region
    // that ended in a `ret`). Keeps merge edges from being appended after a `ret`.
    void branch_to(llvm::BasicBlock* bb) {
        if (b_.GetInsertBlock()->getTerminator() == nullptr) {
            b_.CreateBr(bb);
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
        const Type& ty = std::get<Type>(mod_.pool.get(type));
        if (ty.kind == Type::Kind::String) {
            return { 0, false, false, true };
        }
        const Layout lay = mod_.pool.layout(type);
        return { lay.width == 0 ? 1 : lay.width, lay.domain == Domain::Four, ty.is_signed };
    }

    ValType compute_type(Index i) {
        const Inst::Payload& p = ir_->payload(i);
        switch (ir_->tag(i)) {
            case Inst::Tag::Constant:
                return from_ref(std::get<Value>(mod_.pool.get(p.ref)).type);
            case Inst::Tag::Arg:
                return from_ref(p.arg.type);
            case Inst::Tag::Local:
                return from_ref(p.local.type);
            case Inst::Tag::Call:
                return from_ref(mod_.function(ir_->extra_data<CallInfo>(p.extra).func).return_type);
            case Inst::Tag::Resize:
                return from_ref(p.op_type.type);
            case Inst::Tag::Signal: {
                const SignalSlot& s = slot_of(p.signal);
                return { s.width, s.four_state, s.is_signed, s.is_string && !s.is_array };
            }
            case Inst::Tag::BitSelect: {
                const ValType base = type_of(p.binary.l);
                return { 1, base.four_state, false };
            }
            case Inst::Tag::PartSelect: {
                const auto ps = ir_->extra_data<PartSelect>(p.op_extra.extra);
                return { ps.msb - ps.lsb + 1, type_of(p.op_extra.op).four_state, false };
            }
            case Inst::Tag::DynPartSelect: {
                const auto dps = ir_->extra_data<DynPartSelect>(p.op_extra.extra);
                return { dps.width, type_of(p.op_extra.op).four_state, false };
            }
            case Inst::Tag::ElemSelect: {
                // The result is one element of the indexed array.
                const auto  es = ir_->extra_data<ElemSelect>(p.op_extra.extra);
                const Type& at = std::get<Type>(mod_.pool.get(es.array_type));
                return from_ref(at.element);
            }
            case Inst::Tag::Load:
                return type_of(p.unary);
            case Inst::Tag::SimTime:
                return { 64, false, false };
            case Inst::Tag::Random:
                return { 32, false, true };

            case Inst::Tag::Add:
            case Inst::Tag::Sub:
            case Inst::Tag::Mul:
            case Inst::Tag::Div:
            case Inst::Tag::Mod:
            // Power's result shares the left operand's type; the exponent is
            // self-determined and does not widen the result.
            case Inst::Tag::Pow:
            case Inst::Tag::Band:
            case Inst::Tag::Bor:
            case Inst::Tag::Bxor:
            case Inst::Tag::Bxnor:
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
            case Inst::Tag::WildEq:
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
                    if (t.is_string) { // string concat (all parts are strings)
                        return { 0, false, false, true };
                    }
                    width += t.width;
                    four = four || t.four_state;
                }
                return { width, four, false };
            }
            case Inst::Tag::Repl: {
                const ValType v = type_of(p.repl.value);
                if (v.is_string) {
                    return { 0, false, false, true };
                }
                return { v.width * p.repl.count, v.four_state, false };
            }

            case Inst::Tag::StrLen:
            case Inst::Tag::StrCmp:
            case Inst::Tag::StrICmp:
                return { 32, false, true };
            case Inst::Tag::StrGetc:
                return { 8, false, false };
            case Inst::Tag::StrUpper:
            case Inst::Tag::StrLower:
            case Inst::Tag::StrSubstr:
            case Inst::Tag::StrFromPacked:
                return { 0, false, false, true };
            case Inst::Tag::StrToPacked:
                return from_ref(p.op_type.type);
            case Inst::Tag::StrPutc:
                return { 0, false, false };

            case Inst::Tag::Store:
            case Inst::Tag::StoreNb:
            case Inst::Tag::Cond:
            case Inst::Tag::Loop:
            case Inst::Tag::Ret:
            case Inst::Tag::Delay:
            case Inst::Tag::Wait:
            case Inst::Tag::Display:
            case Inst::Tag::TimeFormat:
            case Inst::Tag::Finish:
            case Inst::Tag::Fflush:
            case Inst::Tag::ReadMem:
            case Inst::Tag::TaskCall:
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

    // rt_string handle pointer of a string-valued instruction (parallel to
    // values_; null for non-string values).
    llvm::Value* str_of(Index i) const {
        return str_vals_[static_cast<uint32_t>(i)];
    }
    void set_str(Index i, llvm::Value* handle) {
        str_vals_[static_cast<uint32_t>(i)] = handle;
    }

    // The rt_string handle pointer of a string lvalue (one array element when
    // elem_idx selects it).
    llvm::Value* lv_string_ptr(const LValue& lv) {
        if (lv.array_ty != nullptr) {
            llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
            return b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx });
        }
        return lv.base;
    }

    // Whether an instruction produces a fresh string value that needs its own
    // 32-byte handle (as opposed to yielding a pointer at existing storage).
    bool produces_string_temp(Index i) {
        switch (ir_->tag(i)) {
            case Inst::Tag::StrUpper:
            case Inst::Tag::StrLower:
            case Inst::Tag::StrSubstr:
            case Inst::Tag::StrFromPacked:
                return true;
            case Inst::Tag::Constant:
            case Inst::Tag::Concat:
            case Inst::Tag::Repl:
            case Inst::Tag::Call:
                return type_of(i).is_string;
            default:
                return false;
        }
    }

    // Alloca + init one handle per string-producing instruction, in the body
    // prologue (entry block of a function; right after coro.begin of a process /
    // task). Producers overwrite their temp in place, so a loop reuses its
    // capacity instead of allocating each iteration. Paired with
    // deinit_body_strings at the body's exits.
    void build_str_temps() {
        str_temp_list_.clear();
        for (size_t k = 0; k < ir_->inst_count(); ++k) {
            const auto i = static_cast<Index>(k);
            if (!produces_string_temp(i)) {
                continue;
            }
            auto* temp = b_.CreateAlloca(str_slot_ty(), nullptr, "str.tmp");
            temp->setAlignment(llvm::Align(alignof(rt_string)));
            b_.CreateCall(str_init_fn(), { temp });
            set_str(i, temp);
            str_temp_list_.push_back(temp);
        }
    }

    // -- Instruction dispatch --

    void lower_inst(Index i) {
        const Inst::Payload& p = ir_->payload(i);
        switch (ir_->tag(i)) {
            // Lvalue derivation: record a descriptor, emit no value.
            case Inst::Tag::Signal:
                set_lvalue(i, signal_lvalue(p.signal));
                return;
            case Inst::Tag::Local:
                set_lvalue(i, local_lvalue(p.local.index));
                return;
            case Inst::Tag::BitSelect: {
                LValue lv = lvalue_of(p.binary.l);
                if (lv.is_string) {
                    // String indexing lowers to StrGetc / StrPutc in the frontend.
                    throw BackendError{ "internal: string value in bit_select" };
                }
                // TODO: an X/Z or out-of-range index uses only its must bits,
                // silently selecting the wrong bit. LRM: a read with an unknown /
                // out-of-range index yields X; such a write is a no-op.
                llvm::Value* idx = cast(must_of(value_of(p.binary.r)), 32, false);
                lv.bit_off       = b_.CreateAdd(lv.bit_off, idx);
                lv.width         = 1;
                set_lvalue(i, lv);
                return;
            }
            case Inst::Tag::PartSelect: {
                LValue     lv = lvalue_of(p.op_extra.op);
                const auto ps = ir_->extra_data<PartSelect>(p.op_extra.extra);
                lv.bit_off    = b_.CreateAdd(lv.bit_off, b_.getInt32(ps.lsb));
                lv.width      = ps.msb - ps.lsb + 1;
                set_lvalue(i, lv);
                return;
            }
            case Inst::Tag::DynPartSelect: {
                // Runtime low-bit offset, constant width: add the offset to the
                // lvalue's bit_off exactly as a dynamic BitSelect does, then the
                // shared load/store path shifts/masks by the runtime bit_off.
                // TODO: an X/Z or out-of-range offset uses only its must bits and is
                // not bounds-checked (LRM 11.5.1: OOB read -> X, write -> no-op).
                LValue       lv  = lvalue_of(p.op_extra.op);
                const auto   dps = ir_->extra_data<DynPartSelect>(p.op_extra.extra);
                llvm::Value* off = cast(must_of(value_of(dps.offset)), 32, false);
                lv.bit_off       = b_.CreateAdd(lv.bit_off, off);
                lv.width         = dps.width;
                set_lvalue(i, lv);
                return;
            }
            case Inst::Tag::ElemSelect: {
                // Advance the lvalue's flat leaf index to the selected element:
                // elem_idx += normalized_index * leaf-elements-per-selected-element
                // (1 for a leaf select; the inner dimensions' product for an outer
                // dimension of a multidimensional memory). The base lvalue's
                // slot_ty and store_width already describe a leaf element (set by
                // the array's signal slot), so this only shifts to the element and
                // resets any inherited bit offset.
                LValue         lv     = lvalue_of(p.op_extra.op);
                const auto     es     = ir_->extra_data<ElemSelect>(p.op_extra.extra);
                const Type&    at     = std::get<Type>(mod_.pool.get(es.array_type));
                const uint32_t stride = array_leaf_count(at.element);
                // TODO: an X/Z or out-of-range index uses only its must bits, so it
                // selects a wrong / out-of-bounds element. LRM: an out-of-range read
                // yields X and such a write is a no-op.
                llvm::Value* idx = cast(must_of(value_of(es.index)), 32, false);
                // Normalize the declared-range coordinate to a 0-based position.
                llvm::Value* norm = at.left <= at.right ? b_.CreateSub(idx, b_.getInt32(at.left))
                                                        : b_.CreateSub(b_.getInt32(at.left), idx);
                llvm::Value* off  = stride == 1 ? norm : b_.CreateMul(norm, b_.getInt32(stride));
                lv.elem_idx       = lv.elem_idx != nullptr ? b_.CreateAdd(lv.elem_idx, off) : off;
                lv.bit_off        = b_.getInt32(0);
                set_lvalue(i, lv);
                return;
            }

            case Inst::Tag::Load: {
                const LValue& lv = lvalue_of(p.unary);
                if (lv.is_string) {
                    // A string load is the storage handle pointer, not a copy:
                    // every mutation point (Store, string ops) deep-copies, and
                    // producers write into distinct temps.
                    set_str(i, lv_string_ptr(lv));
                    return;
                }
                set_value(i, lower_load(lv));
                return;
            }
            case Inst::Tag::Store: {
                // A blocking assignment stores, then services the target signal's
                // wait queue: any process waiting for an edge this store produced
                // is woken. Snapshot the old value first so the transition can be
                // detected from old -> new.
                const LValue& lv      = lvalue_of(p.binary.l);
                if (lv.is_string) {
                    // Deep copy. Strings are guarded out of sensitivity lists in
                    // the frontend, so there is no wait queue to service.
                    b_.CreateCall(str_assign_fn(), { lv_string_ptr(lv), str_of(p.binary.r) });
                    return;
                }
                // Frame locals have no waiters; only a module signal store services
                // wait queues. A PreInit store (variable declaration initializer)
                // generates no events, so it never services regardless of waiters.
                const bool    service = !lowering_preinit_ && lv.is_signal && signal_has_waiters(lv.sig);
                // Snapshot the selected storage (whole signal, or one array element)
                // so an element write wakes Any-edge waiters on a change to that
                // element rather than always reading element 0.
                llvm::Value*  old_mm  = service ? load_lv_mm(lv) : nullptr;
                lower_store(lv, value_of(p.binary.r), type_of(p.binary.r));
                if (service) {
                    service_wait_queue(lv.sig, old_mm, load_lv_mm(lv));
                }
                return;
            }
            // A nonblocking assignment samples its RHS now but defers the write: it
            // stages the value into the target's shadow and enqueues the target for
            // the NBA update region (see emit_nba_region), which commits shadow ->
            // live and services wait queues only after every active process has run.
            case Inst::Tag::StoreNb: {
                const LValue& lv = lvalue_of(p.binary.l);
                // An unpacked-array element NBA defers through the runtime queue; a
                // plain signal / scalar select uses the per-signal shadow.
                if (lv.is_signal && slot_of(lv.sig).is_array) {
                    emit_array_nba(lv, value_of(p.binary.r));
                } else {
                    lower_store_nb(lv, value_of(p.binary.r));
                }
                return;
            }

            case Inst::Tag::Constant: {
                const Value& v = std::get<Value>(mod_.pool.get(p.ref));
                if (std::get<Type>(mod_.pool.get(v.type)).kind == Type::Kind::String) {
                    lower_string_constant(i, p.ref);
                    return;
                }
                set_value(i, lower_constant(p.ref));
                return;
            }

            case Inst::Tag::Add:
            case Inst::Tag::Sub:
            case Inst::Tag::Mul:
                set_value(i, lower_arith(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;

            case Inst::Tag::Div:
            case Inst::Tag::Mod: {
                const ValType lt = type_of(p.binary.l);
                // Wide operands legalize to the 128-bit __udivti3 / __umodti3 (and signed
                // siblings) libcalls, resolved from compiler-rt/libgcc in the JIT (see run_jit).
                set_value(i, lower_divmod(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r), lt.is_signed));
                return;
            }

            case Inst::Tag::Pow: {
                const ValType lt = type_of(p.binary.l);
                if (lt.is_signed || type_of(p.binary.r).is_signed) {
                    // TODO: signed ** carries the negative-base/negative-exponent corners of LRM
                    // Table 11-4 (op1 == -1 parity, |op1| > 1 with op2 < 0 -> 0, etc.); only the
                    // unsigned path is lowered.
                    throw BackendError{ "signed power operator (**) not yet supported" };
                }
                set_value(i, lower_pow(value_of(p.binary.l), value_of(p.binary.r)));
                return;
            }

            case Inst::Tag::Band:
            case Inst::Tag::Bor:
            case Inst::Tag::Bxor:
            case Inst::Tag::Bxnor:
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
                if (type_of(p.binary.l).is_string) {
                    set_value(i, lower_string_relation(ir_->tag(i), p.binary.l, p.binary.r));
                    return;
                }
                set_value(i,
                          lower_compare(
                            ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r), type_of(p.binary.l).is_signed));
                return;

            case Inst::Tag::CaseEq:
            case Inst::Tag::CasezEq:
            case Inst::Tag::CasexEq:
                set_value(i, lower_case_compare(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;

            case Inst::Tag::WildEq:
                set_value(i, lower_wildeq(value_of(p.binary.l), value_of(p.binary.r)));
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
                if (type_of(mux.a).is_string) {
                    throw BackendError{ "internal: string value in mux" };
                }
                set_value(i, lower_mux(value_of(p.op_extra.op), value_of(mux.a), value_of(mux.b)));
                return;
            }
            case Inst::Tag::Concat:
                if (type_of(i).is_string) {
                    lower_string_concat(i);
                    return;
                }
                set_value(i, lower_concat(i));
                return;
            case Inst::Tag::Repl:
                if (type_of(i).is_string) {
                    b_.CreateCall(str_repl_fn(),
                                  { str_of(i), str_of(p.repl.value), b_.getInt64(p.repl.count) });
                    return;
                }
                set_value(i, lower_repl(value_of(p.repl.value), type_of(p.repl.value).width, p.repl.count));
                return;
            case Inst::Tag::Resize:
                if (type_of(p.op_type.op).is_string) {
                    throw BackendError{ "internal: string value in resize" };
                }
                set_value(i, lower_resize(value_of(p.op_type.op), type_of(p.op_type.op), from_ref(p.op_type.type)));
                return;

            case Inst::Tag::StrLen: {
                llvm::Value* n = b_.CreateCall(str_len_fn(), { str_of(p.unary) });
                llvm::Value* v = b_.CreateTrunc(n, int_ty(32));
                set_value(i, packed(v, v, zero(32)));
                return;
            }
            case Inst::Tag::StrGetc: {
                llvm::Value* idx =
                  cast(must_of(value_of(p.binary.r)), 32, type_of(p.binary.r).is_signed);
                llvm::Value* c = b_.CreateCall(str_getc_fn(), { str_of(p.binary.l), idx });
                llvm::Value* v = b_.CreateTrunc(c, int_ty(8));
                set_value(i, packed(v, v, zero(8)));
                return;
            }
            case Inst::Tag::StrPutc: {
                const auto    sp = ir_->extra_data<StrPutcInfo>(p.op_extra.extra);
                const LValue& lv = lvalue_of(p.op_extra.op);
                llvm::Value*  idx =
                  cast(must_of(value_of(sp.index)), 32, type_of(sp.index).is_signed);
                llvm::Value* ch = cast(must_of(value_of(sp.ch)), 32, false);
                b_.CreateCall(str_putc_fn(), { lv_string_ptr(lv), idx, ch });
                return;
            }
            case Inst::Tag::StrCmp:
            case Inst::Tag::StrICmp: {
                auto         fn = ir_->tag(i) == Inst::Tag::StrCmp ? str_cmp_fn() : str_icmp_fn();
                llvm::Value* c  = b_.CreateCall(fn, { str_of(p.binary.l), str_of(p.binary.r) });
                set_value(i, packed(c, c, zero(32)));
                return;
            }
            case Inst::Tag::StrUpper:
            case Inst::Tag::StrLower: {
                auto fn = ir_->tag(i) == Inst::Tag::StrUpper ? str_upper_fn() : str_lower_fn();
                b_.CreateCall(fn, { str_of(i), str_of(p.unary) });
                return;
            }
            case Inst::Tag::StrSubstr: {
                const auto   ss = ir_->extra_data<StrSubstrInfo>(p.op_extra.extra);
                llvm::Value* si = cast(must_of(value_of(ss.i)), 32, type_of(ss.i).is_signed);
                llvm::Value* sj = cast(must_of(value_of(ss.j)), 32, type_of(ss.j).is_signed);
                b_.CreateCall(str_substr_fn(), { str_of(i), str_of(p.op_extra.op), si, sj });
                return;
            }
            case Inst::Tag::StrFromPacked: {
                // Spill the operand's val plane to a stack buffer (X bits read as
                // 0 — the runtime consumes must-bits only, a documented
                // simplification of the LRM cast).
                const ValType  t   = type_of(p.unary);
                const uint32_t pb  = plane_bytes(t.width);
                llvm::Value*   buf = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(pb), "str.pack");
                b_.CreateAlignedStore(must_of(value_of(p.unary)), buf, llvm::Align(1));
                b_.CreateCall(str_from_packed_fn(), { str_of(i), buf, b_.getInt32(t.width) });
                return;
            }
            case Inst::Tag::StrToPacked: {
                const ValType  to  = from_ref(p.op_type.type);
                const uint32_t pb  = plane_bytes(to.width);
                llvm::Value*   buf = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(pb), "str.unpack");
                b_.CreateCall(str_to_packed_fn(), { str_of(p.op_type.op), buf, b_.getInt32(to.width) });
                llvm::Value* v = b_.CreateAlignedLoad(int_ty(to.width), buf, llvm::Align(1));
                set_value(i, packed(v, v, zero(to.width)));
                return;
            }

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
                lower_wait(p.wait);
                return;

            case Inst::Tag::Display:
                lower_display(i);
                return;

            case Inst::Tag::TimeFormat:
                lower_timeformat(i);
                return;

            case Inst::Tag::Finish:
                lower_finish();
                return;

            case Inst::Tag::Fflush:
                lower_fflush();
                return;

            case Inst::Tag::ReadMem:
                lower_readmem(i);
                return;

            case Inst::Tag::SimTime: {
                // The clock value is always known: must == may, z == 0. The wheel
                // counts in the design's smallest time unit; scale the tick count
                // down to the calling module's time unit, rounding to nearest
                // (LRM §20.3.1). `scale` is a power of ten, so scale/2 is exact and
                // (now + scale/2) / scale rounds half up.
                llvm::Value* now = b_.CreateCall(wheel_now_fn(), { wheel_ });
                if (p.scale > 1) {
                    now = b_.CreateUDiv(b_.CreateAdd(now, b_.getInt64(p.scale / 2)),
                                        b_.getInt64(p.scale));
                }
                set_value(i, packed(now, now, zero(64)));
                return;
            }

            case Inst::Tag::Random: {
                // The PRNG draw is always known (no x/z): must == may, z == 0.
                llvm::Value* draw = b_.CreateCall(random_fn(), {});
                set_value(i, packed(draw, draw, zero(32)));
                return;
            }

            case Inst::Tag::Arg: {
                // A function / task parameter, located via the per-signature index
                // table (a string formal is one ptr, others two params).
                const uint32_t k    = p.arg.position;
                const uint32_t base = param_llvm_index_[k];
                if (is_string_type(cur_fn_->param_types[k])) {
                    set_str(i, fn_->getArg(base));
                    return;
                }
                set_value(i, Plane{ fn_->getArg(base), fn_->getArg(base + 1) });
                return;
            }
            case Inst::Tag::Ret:
                lower_ret(p.unary);
                return;
            case Inst::Tag::Call:
                lower_call_inst(i);
                return;
            case Inst::Tag::TaskCall:
                lower_task_call_inst(i);
                return;
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

    // A string literal: copy its interned characters into this instruction's
    // pre-built temp from a private constant global. Length is passed explicitly
    // — never rely on the NUL byte CreateGlobalString appends.
    void lower_string_constant(Index i, Ref ref) {
        const std::span<const uint8_t> bytes = mod_.pool.bytes(ref);
        llvm::Value*                   glob  = b_.CreateGlobalString(
          llvm::StringRef(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "str.lit");
        b_.CreateCall(str_assign_bytes_fn(), { str_of(i), glob, b_.getInt64(bytes.size()) });
    }

    // String concatenation: gather the part handles into a stack array and hand
    // them to the runtime, which builds into this instruction's temp (alias-safe
    // even when the temp aliases a part).
    void lower_string_concat(Index i) {
        std::array<Index, 3> scratch;
        const auto           parts  = ir_->operands(i, scratch);
        const auto           n      = static_cast<uint32_t>(parts.size());
        auto*                arr_ty = llvm::ArrayType::get(llvm::PointerType::getUnqual(ctx_), n);
        llvm::Value*         arr    = b_.CreateAlloca(arr_ty, nullptr, "str.parts");
        for (uint32_t k = 0; k < n; ++k) {
            b_.CreateStore(str_of(parts[k]),
                           b_.CreateInBoundsGEP(arr_ty, arr, { b_.getInt32(0), b_.getInt32(k) }));
        }
        b_.CreateCall(str_concat_fn(), { str_of(i), arr, b_.getInt32(n) });
    }

    // A string comparison operator: rt_string_cmp then a native compare against
    // zero. Strings carry no X/Z, so the 1-bit result is always definite.
    Plane lower_string_relation(Inst::Tag tag, Index l, Index r) {
        llvm::Value*             c = b_.CreateCall(str_cmp_fn(), { str_of(l), str_of(r) });
        llvm::CmpInst::Predicate pred;
        switch (tag) {
            case Inst::Tag::Eq: pred = llvm::CmpInst::ICMP_EQ; break;
            case Inst::Tag::Ne: pred = llvm::CmpInst::ICMP_NE; break;
            case Inst::Tag::Lt: pred = llvm::CmpInst::ICMP_SLT; break;
            case Inst::Tag::Le: pred = llvm::CmpInst::ICMP_SLE; break;
            case Inst::Tag::Gt: pred = llvm::CmpInst::ICMP_SGT; break;
            default:            pred = llvm::CmpInst::ICMP_SGE; break;
        }
        llvm::Value* bit = b_.CreateICmp(pred, c, b_.getInt32(0));
        return bit_plane(bit, bit);
    }

    // Build an iW constant from little-endian canonical plane bytes.
    llvm::ConstantInt* const_from_bytes(std::span<const uint8_t> bytes, uint32_t width) {
        const unsigned        words = (width + 63) / 64;
        std::vector<uint64_t> buf(words, 0);
        std::memcpy(buf.data(), bytes.data(), std::min<size_t>(bytes.size(), words * 8));
        return llvm::ConstantInt::get(ctx_, llvm::APInt(width, llvm::ArrayRef<uint64_t>(buf.data(), words)));
    }

    // -- State access --

    // Whole-storage lvalue for a module signal (state struct).
    LValue signal_lvalue(Signal sig) {
        const SignalSlot& s = slot_of(sig);
        return LValue{ .base        = storage_ptr(state_, s),
                       .slot_ty     = s.slot_ty,
                       .has_z       = s.has_z,
                       .array_ty    = s.is_array ? llvm::cast<llvm::ArrayType>(s.storage_ty) : nullptr,
                       .store_width = s.width,
                       .bit_off     = b_.getInt32(0),
                       .width       = s.width,
                       .is_signal   = true,
                       .sig         = sig,
                       .is_string   = s.is_string };
    }

    // Whole-storage lvalue for a function-frame local (per-call frame struct).
    LValue local_lvalue(uint32_t index) {
        const FrameSlot& s = frame_slots_[index];
        return LValue{ .base        = b_.CreateStructGEP(frame_ty_, frame_, index),
                       .slot_ty     = s.slot_ty,
                       .store_width = s.width,
                       .bit_off     = b_.getInt32(0),
                       .width       = s.width,
                       .is_signal   = false,
                       .sig         = Signal{ 0 },
                       .is_string   = s.is_string };
    }

    Plane lower_load(const LValue& lv) {
        const uint32_t w  = lv.store_width;
        llvm::Value*   mm = b_.CreateLoad(int_ty(2 * w), lv_mm_ptr(lv));
        // A never-z slot stores no z word; its z plane is a constant zero.
        llvm::Value* z = lv.has_z ? static_cast<llvm::Value*>(b_.CreateLoad(int_ty(w), lv_z_ptr(lv))) : zero(w);
        if (lv.width == w && is_zero_const(lv.bit_off)) {
            return { mm, z };
        }
        // Bit/part select: unpack, shift each plane down by the offset, re-pack.
        const Plane  full = { mm, z };
        llvm::Value* off  = cast(lv.bit_off, w, false);
        return packed(cast(b_.CreateLShr(must_of(full), off), lv.width, false),
                      cast(b_.CreateLShr(may_of(full), off), lv.width, false),
                      cast(b_.CreateLShr(z, off), lv.width, false));
    }

    void lower_store(const LValue& lv, Plane rhs, ValType rhs_ty) {
        const uint32_t s_width = lv.store_width;
        llvm::Value*   mp      = lv_mm_ptr(lv);
        (void)rhs_ty;

        // Whole-storage store: the frontend context-sizes the RHS to the lvalue
        // width (via an inserted Resize), so rhs already matches the storage width
        // and the planes store directly. A never-z slot drops the RHS z plane —
        // the may-z analysis proved it zero here.
        if (lv.width == s_width && is_zero_const(lv.bit_off)) {
            b_.CreateStore(rhs.mm, mp);
            if (lv.has_z) {
                b_.CreateStore(rhs.z, lv_z_ptr(lv));
            }
            return;
        }

        // Partial store: read-modify-write each plane at the storage width.
        llvm::Value* zp  = lv.has_z ? lv_z_ptr(lv) : nullptr;
        const Plane  old = { b_.CreateLoad(int_ty(2 * s_width), mp),
                             lv.has_z ? static_cast<llvm::Value*>(b_.CreateLoad(int_ty(s_width), zp))
                                      : zero(s_width) };
        llvm::Value* off   = cast(lv.bit_off, s_width, false);
        llvm::Value* mask  = b_.CreateShl(cast(ones(lv.width), s_width, false), off);
        llvm::Value* nmask = b_.CreateNot(mask);
        llvm::Value* must  = merge_bits(must_of(old), cast(must_of(rhs), s_width, false), off, mask, nmask);
        llvm::Value* may   = merge_bits(may_of(old), cast(may_of(rhs), s_width, false), off, mask, nmask);
        llvm::Value* z     = merge_bits(old.z, cast(rhs.z, s_width, false), off, mask, nmask);
        b_.CreateStore(packed(must, may, z).mm, mp);
        if (lv.has_z) {
            b_.CreateStore(z, zp);
        }
    }

    // Stage a nonblocking assignment: write the sampled RHS into the target's
    // shadow (whole-signal direct, or merged-with-mask for a part-select target)
    // and append the target to its commit worklist (deduped by the pending flag, so
    // repeat NBAs to one signal in a slot accumulate in the shadow). No wait queues
    // are serviced here — that happens in the NBA region after the active drain.
    void lower_store_nb(const LValue& lv, Plane rhs) {
        const SignalSlot& s  = slot_of(lv.sig);
        llvm::Value*      mp = shadow_ptr(s.sh_mm_field);
        // A never-z target stages no z shadow (and its commit loop skips z).
        llvm::Value* zp = s.has_z ? shadow_ptr(s.sh_z_field) : nullptr;

        if (!s.nba_partial) {
            // Whole-signal target: the RHS is already signal-width (frontend Resize),
            // so the shadow planes store directly — no mask, no read-modify-write.
            b_.CreateStore(rhs.mm, mp);
            if (s.has_z) {
                b_.CreateStore(rhs.z, zp);
            }
        } else {
            // Partial-capable target: merge the selected bits into the shadow and OR
            // the touched bits into the dirty mask. A whole-signal site here selects
            // every bit, so it merges the full word and sets the whole mask.
            llvm::Value* maskp = shadow_ptr(s.sh_mask_field);
            const Plane  old   = { b_.CreateLoad(int_ty(2 * s.width), mp),
                                   s.has_z ? static_cast<llvm::Value*>(b_.CreateLoad(int_ty(s.width), zp))
                                           : zero(s.width) };
            llvm::Value* off   = cast(lv.bit_off, s.width, false);
            llvm::Value* bmask = b_.CreateShl(cast(ones(lv.width), s.width, false), off);
            llvm::Value* nmask = b_.CreateNot(bmask);
            llvm::Value* must  = merge_bits(must_of(old), cast(must_of(rhs), s.width, false), off, bmask, nmask);
            llvm::Value* may   = merge_bits(may_of(old), cast(may_of(rhs), s.width, false), off, bmask, nmask);
            llvm::Value* z     = merge_bits(old.z, cast(rhs.z, s.width, false), off, bmask, nmask);
            b_.CreateStore(packed(must, may, z).mm, mp);
            if (s.has_z) {
                b_.CreateStore(z, zp);
            }
            llvm::Value* cur = b_.CreateLoad(int_ty(s.width), maskp);
            b_.CreateStore(b_.CreateOr(cur, bmask), maskp);
        }
        emit_nba_enqueue(s);
    }

    // The reused array-NBA staging struct type: [16]mm_val [16]mm_mask [8]z_val
    // [8]z_mask, sized for an element width up to 64 bits (mm word i128 = 16B).
    llvm::StructType* nba_scratch_ty() {
        auto* i8 = b_.getInt8Ty();
        return llvm::StructType::get(ctx_, { llvm::ArrayType::get(i8, 16), llvm::ArrayType::get(i8, 16),
                                             llvm::ArrayType::get(i8, 8), llvm::ArrayType::get(i8, 8) });
    }
    // Lazily allocate the staging struct at coro.begin so it is reused across every
    // element-NBA site (a loop of element NBAs must not grow the stack). Mirrors
    // awaiter().
    llvm::Value* nba_scratch() {
        if (nba_scratch_ == nullptr) {
            llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
            nba_scratch_ = ab.CreateAlloca(nba_scratch_ty(), nullptr, "nba.scratch");
        }
        return nba_scratch_;
    }

    // Stage a nonblocking write to an unpacked-array element into the runtime
    // deferred queue (rt_nba_arr_push). Builds the value and a write-mask at the
    // element width (selected bits placed at bit_off), stores them to reused
    // scratch, and hands the live element's destination pointers to the queue; the
    // NBA region replays the masked merge after the active drain.
    void emit_array_nba(const LValue& lv, Plane rhs) {
        const uint32_t W = lv.store_width;
        if (W > 64) {
            // TODO: element width > 64 needs a larger staging buffer (mm word > i128).
            throw BackendError{ "nonblocking write to an unpacked array element wider than 64 bits is not supported" };
        }
        llvm::Value* off    = cast(lv.bit_off, W, false);
        llvm::Value* mask_w = b_.CreateShl(cast(ones(lv.width), W, false), off);
        llvm::Value* must   = b_.CreateShl(cast(must_of(rhs), W, false), off);
        llvm::Value* may    = b_.CreateShl(cast(may_of(rhs), W, false), off);
        llvm::Value* zval   = b_.CreateShl(cast(rhs.z, W, false), off);
        llvm::Value* mm_val = packed(must, may, zval).mm;          // i2W: must | (may << W)
        llvm::Value* mm_msk = packed(mask_w, mask_w, zero(W)).mm;  // i2W: mask in both halves

        llvm::Value*      st  = nba_scratch();
        llvm::StructType* ty  = nba_scratch_ty();
        const llvm::Align a1{ 1 };
        auto*             nullp = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        llvm::Value*      mvp   = b_.CreateStructGEP(ty, st, 0);
        llvm::Value*      mkp   = b_.CreateStructGEP(ty, st, 1);
        b_.CreateAlignedStore(mm_val, mvp, a1);
        b_.CreateAlignedStore(mm_msk, mkp, a1);
        // A never-z element slot stores no z word: hand the runtime a null z_ptr
        // with zero z bytes so the commit merges only the must|may word.
        llvm::Value* zvp = nullp;
        llvm::Value* zkp = nullp;
        llvm::Value* z_ptr = nullp;
        uint32_t     z_bytes = 0;
        if (lv.has_z) {
            zvp = b_.CreateStructGEP(ty, st, 2);
            zkp = b_.CreateStructGEP(ty, st, 3);
            b_.CreateAlignedStore(zval, zvp, a1);
            b_.CreateAlignedStore(mask_w, zkp, a1);
            z_ptr   = lv_z_ptr(lv);
            z_bytes = (W + 7) / 8;
        }

        llvm::Value*   mm_ptr   = lv_mm_ptr(lv);
        const uint32_t mm_bytes = (2 * W + 7) / 8;
        b_.CreateCall(nba_arr_push_fn(),
                      { b_.getInt32(static_cast<uint32_t>(lv.sig)), mm_ptr, mvp, mkp, b_.getInt32(mm_bytes), z_ptr,
                        zvp, zkp, b_.getInt32(z_bytes) });
    }

    // Wake the Any-edge waiters of an array signal unconditionally (an element
    // write has no edge semantics; readers re-evaluate). Used after the deferred
    // queue commits. Arrays only ever carry Any-edge (level) sensitivity — slang
    // rejects edge events on memory elements (@(posedge mem[i])) — so the Any
    // bucket is the only one a memory can have.
    void service_any_waiters(Signal sig) {
        const auto i = static_cast<uint32_t>(sig);
        const auto e = static_cast<uint32_t>(Edge::Any);
        if (wait_sta_gv_[i][e] != nullptr) {
            service_static_queue(sig, e);
        }
        if (wait_dyn_gv_[i][e] != nullptr) {
            service_dynamic_queue(sig, e);
        }
    }

    // Append an NBA target to its bucket worklist, but only the first time it is
    // staged this slot (pending flag 0 -> 1). The flag is cleared when the entry is
    // committed, so the next slot re-enqueues; it also bounds the worklist to one
    // live entry per signal, making the bucket capacity (its signal count) exact.
    void emit_nba_enqueue(const SignalSlot& s) {
        auto*             i8    = b_.getInt8Ty();
        auto*             i32   = b_.getInt32Ty();
        const NbaBucket&  bk    = nba_buckets_[s.nba_bucket];
        llvm::Value*      flagp = b_.CreateInBoundsGEP(nba_pending_ty_, nba_pending_gv_,
                                                       { b_.getInt32(0), b_.getInt32(s.nba_id) });
        llvm::Value*      already = b_.CreateICmpNE(b_.CreateLoad(i8, flagp), b_.getInt8(0));
        auto*             fn      = b_.GetInsertBlock()->getParent();
        auto*             push    = llvm::BasicBlock::Create(ctx_, "nba.push", fn);
        auto*             cont    = llvm::BasicBlock::Create(ctx_, "nba.pushcont", fn);
        b_.CreateCondBr(already, cont, push);

        b_.SetInsertPoint(push);
        b_.CreateStore(b_.getInt8(1), flagp);
        llvm::Value* top  = b_.CreateLoad(i32, bk.top);
        llvm::Value* slot = b_.CreateInBoundsGEP(bk.ty, bk.gv, { b_.getInt32(0), top });
        b_.CreateStore(b_.getInt32(s.nba_id), slot);
        b_.CreateStore(b_.CreateAdd(top, b_.getInt32(1)), bk.top);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
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

    // void rt_timeformat_init(i64 tick_fs, i32 default_units)
    llvm::FunctionCallee timeformat_init_fn() {
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { b_.getInt64Ty(), b_.getInt32Ty() }, false);
        return llmod_.getOrInsertFunction("rt_timeformat_init", fnty);
    }

    // void rt_timeformat_set(i32 units, i32 precision, const char* suffix, i32 suffix_len, i32 min_width)
    llvm::FunctionCallee timeformat_set_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { i32, i32, ptr, i32, i32 }, false);
        return llmod_.getOrInsertFunction("rt_timeformat_set", fnty);
    }

    // Lower a $timeformat: evaluate the units / precision / min_width operands and
    // hand them, plus the literal suffix, to the runtime setter. The operands were
    // lowered earlier in this region, so their values are available.
    void lower_timeformat(Index i) {
        const auto   info  = ir_->extra_data<TimeFormatInfo>(ir_->payload(i).timeformat);
        llvm::Value* units = cast(must_of(value_of(info.units)), 32, true);
        llvm::Value* prec  = cast(must_of(value_of(info.precision)), 32, true);
        llvm::Value* width = cast(must_of(value_of(info.min_width)), 32, true);

        const std::string suffix  = ir_->extra_bytes(info.suffix, info.suffix_len);
        llvm::Value*      sfx_ptr = b_.CreateGlobalString(suffix);
        b_.CreateCall(timeformat_set_fn(),
                      { units, prec, sfx_ptr, b_.getInt32(static_cast<uint32_t>(suffix.size())), width });
    }

    // void rt_finish(void) — noreturn; exits the process.
    llvm::FunctionCallee finish_fn() {
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), false);
        auto  fn   = llmod_.getOrInsertFunction("rt_finish", fnty);
        if (auto* f = llvm::dyn_cast<llvm::Function>(fn.getCallee())) {
            f->setDoesNotReturn();
        }
        return fn;
    }

    // Lower a $finish / $stop: call the runtime, which exits the process. The call
    // never returns, so the rest of the region is dead, but it stays valid IR —
    // rt_finish is marked noreturn and the optimizer prunes the unreachable tail.
    void lower_finish() {
        b_.CreateCall(finish_fn(), {});
    }

    // void rt_fflush(void) — flush all open output buffers.
    llvm::FunctionCallee fflush_fn() {
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), false);
        return llmod_.getOrInsertFunction("rt_fflush", fnty);
    }

    // Lower a $fflush (no-arg form): flush the runtime's output buffers.
    void lower_fflush() {
        b_.CreateCall(fflush_fn(), {});
    }

    // void rt_readmem(const char* path, i32 path_len, i8* mm_base, i8* z_base,
    //                 i32 stride, i32 count, i32 width, i64 decl_left, i64 decl_right,
    //                 i64 start, i64 finish, i32 flags)
    llvm::FunctionCallee readmem_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* i64  = b_.getInt64Ty();
        auto* fnty = llvm::FunctionType::get(
          b_.getVoidTy(), { ptr, i32, ptr, ptr, i32, i32, i32, i64, i64, i64, i64, i32 }, false);
        return llmod_.getOrInsertFunction("rt_readmem", fnty);
    }

    // Lower a $readmemh / $readmemb: spill the filename to a stack buffer and hand
    // the runtime the destination memory's plane bases (element 0) and geometry. The
    // runtime opens the file and writes element planes straight into `state`. The mm
    // and z words of element i sit at the plane bases + i*stride (one slot struct
    // per element), so the two region bases plus the stride address every element.
    void lower_readmem(Index i) {
        const auto        info  = ir_->extra_data<ReadMemInfo>(ir_->payload(i).op_extra.extra);
        const Index       fname = ir_->payload(i).op_extra.op;
        const SignalSlot& s     = slot_of(info.target);
        if (!s.is_array) {
            throw BackendError{ "$readmem target is not a memory" };
        }
        if (s.width > 64) {
            // TODO: element width > 64 needs multi-word value assembly in rt_readmem
            // (the value/mask accumulators are u64 / unsigned __int128 today).
            throw BackendError{ "$readmem into a memory with elements wider than 64 bits is not supported" };
        }

        // Spill the filename's must-bits to a byte buffer: the value is a packed-
        // ASCII vector (string literal or reg), so its low bytes are the path.
        const ValType  fty    = type_of(fname);
        const uint32_t fbytes = std::max<uint32_t>((fty.width + 7) / 8, 1);
        llvm::Value*   path   = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(fbytes), "readmem.path");
        b_.CreateAlignedStore(must_of(value_of(fname)), path, llvm::Align(1));

        // The runtime addresses element i's planes at base + i*stride, so hand it
        // element 0's typed field pointers and the slot struct's alloc size.
        auto*        arr_ty  = llvm::cast<llvm::ArrayType>(s.storage_ty);
        llvm::Value* storage = storage_ptr(state_, s);
        llvm::Value* mm_base =
          b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(0) });
        // A never-z memory (always the case for a 2-state element type) stores no
        // z words; a null z_base tells the runtime to skip the z writes. A
        // 4-state readmem target is a z seed in compute_may_z, so it always keeps
        // its z field.
        llvm::Value* z_base =
          s.has_z ? b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(1) })
                  : llvm::cast<llvm::Value>(llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)));
        const auto stride = static_cast<uint32_t>(llmod_.getDataLayout().getTypeAllocSize(s.slot_ty));

        const Type& at = std::get<Type>(mod_.pool.get(mod_.signal(info.target).type));

        const auto   i64_of = [&](Index idx) { return cast(must_of(value_of(idx)), 64, true); };
        llvm::Value* start  = info.has_start ? i64_of(info.start) : b_.getInt64(0);
        llvm::Value* finish = info.has_finish ? i64_of(info.finish) : b_.getInt64(0);

        const uint32_t flags = (info.hex ? 1u : 0u) | (info.has_start ? 2u : 0u) |
                               (info.has_finish ? 4u : 0u) | (s.four_state ? 8u : 0u);

        b_.CreateCall(readmem_fn(),
                      { path, b_.getInt32(fbytes), mm_base, z_base, b_.getInt32(stride),
                        b_.getInt32(s.elem_count), b_.getInt32(s.width), b_.getInt64(at.left),
                        b_.getInt64(at.right), start, finish, b_.getInt32(flags) });
    }

    // Lower a function `return`: a real `ret`, so it terminates the block and any
    // instructions after it in the region are unreachable (lower_region stops).
    // A void function ignores the operand and returns void; otherwise the result
    // is packed into the `{ i(2W), iW }` return struct.
    void lower_ret(Index value) {
        if (cur_is_task_) {
            // A task is a coroutine: `return` runs the copy-out + caller-resume
            // epilogue and falls into the final suspend, never a real `ret`.
            b_.CreateBr(task_epilogue_bb_);
            return;
        }
        if (is_string_type(cur_fn_->return_type)) {
            // Copy the result out through the trailing out-param handle BEFORE
            // releasing the body's strings (the value may be one of them).
            b_.CreateCall(str_assign_fn(),
                          { fn_->getArg(fn_->arg_size() - 1), str_of(value) });
            deinit_body_strings();
            b_.CreateRetVoid();
            return;
        }
        if (cur_fn_->return_type == mod_.pool.void_type) {
            deinit_body_strings();
            b_.CreateRetVoid();
            return;
        }
        const Plane  pl  = value_of(value);
        auto*        rty = llvm::cast<llvm::StructType>(fn_->getReturnType());
        llvm::Value* agg = llvm::UndefValue::get(rty);
        agg              = b_.CreateInsertValue(agg, pl.mm, { 0 });
        agg              = b_.CreateInsertValue(agg, pl.z, { 1 });
        deinit_body_strings();
        b_.CreateRet(agg);
    }

    // Lower a `call`: pass the state pointer plus each argument as its (must|may,
    // z) plane pair (the frontend has resized each to its parameter width), then
    // unpack the returned struct into a Plane. A void callee yields no value.
    void lower_call_inst(Index i) {
        const auto                ci = ir_->extra_data<CallInfo>(ir_->payload(i).extra);
        const Function&           f  = mod_.function(ci.func);
        llvm::Function*           cf = fn_decls_[static_cast<uint32_t>(ci.func)];
        std::vector<llvm::Value*> argv;
        argv.push_back(state_);
        size_t k = 0;
        for (Index a : ir_->region(ci.args)) {
            if (is_string_type(f.param_types[k])) {
                argv.push_back(str_of(a));
            } else {
                const Plane pl = value_of(a);
                argv.push_back(pl.mm);
                argv.push_back(pl.z);
            }
            ++k;
        }
        if (is_string_type(f.return_type)) {
            // The callee assigns the result into this call's temp handle (the
            // trailing out-param); str_of(i) was pre-set by build_str_temps.
            argv.push_back(str_of(i));
            b_.CreateCall(cf->getFunctionType(), cf, argv);
            return;
        }
        llvm::Value* r = b_.CreateCall(cf->getFunctionType(), cf, argv);
        if (f.return_type != mod_.pool.void_type) {
            set_value(i, Plane{ b_.CreateExtractValue(r, { 0 }), b_.CreateExtractValue(r, { 1 }) });
        }
    }

    // The caller's record slot { ptr parent }: a single ptr alloca placed after the
    // caller's coro.begin (so it is frame-resident across the task-call suspend) and
    // reused by every task call in this coroutine — a coroutine is in at most one
    // task call at a time. The task writes nothing to it on a synchronous return; on
    // a blocking call the caller stores its handle here before suspending and the
    // task reads it to resume the caller on completion.
    llvm::Value* record_slot() {
        if (record_ == nullptr) {
            llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
            record_ = ab.CreateAlloca(llvm::PointerType::getUnqual(ctx_), nullptr, "task_record");
        }
        return record_;
    }

    // Lower a task enable (LRM 13.3). Create the task coroutine, copy inputs in /
    // pass output-inout temp pointers, drive it once, and either finish inline (the
    // task consumed no time) or park the caller until the task completes and resumes
    // it. Only valid inside a coroutine — the frontend never emits a TaskCall in a
    // plain function.
    void lower_task_call_inst(Index i) {
        const auto      ti = ir_->extra_data<TaskCallInfo>(ir_->payload(i).extra);
        const Function& tf = mod_.function(ti.func);
        llvm::Function* cf = fn_decls_[static_cast<uint32_t>(ti.func)];
        auto*           ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*           null   = llvm::ConstantPointerNull::get(ptr_ty);

        // Reset the record so a synchronous task reads a null parent (no enqueue).
        llvm::Value* record = record_slot();
        b_.CreateStore(null, record);

        // Ramp arguments: state, wheel, per-formal planes / pointers, record.
        const std::span<const Index> args = ir_->region(ti.args);
        std::vector<llvm::Value*>    argv;
        argv.push_back(state_);
        argv.push_back(wheel_);
        for (uint32_t k = 0; k < args.size(); ++k) {
            if (tf.arg_dirs[k] == ArgDir::In) {
                if (is_string_type(tf.param_types[k])) {
                    argv.push_back(str_of(args[k]));
                    continue;
                }
                const Plane pl = value_of(args[k]);
                argv.push_back(pl.mm);
                argv.push_back(pl.z);
            } else {
                // An output / inout temp: pass pointers to its mm and z planes.
                const LValue& lv = lvalue_of(args[k]);
                argv.push_back(lv_mm_ptr(lv));
                argv.push_back(lv_z_ptr(lv));
            }
        }
        argv.push_back(record);

        llvm::Value* task_hdl = b_.CreateCall(cf->getFunctionType(), cf, argv);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { task_hdl });
        llvm::Value* done = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { task_hdl });

        auto* sync_bb  = llvm::BasicBlock::Create(ctx_, "task.sync", fn_);
        auto* async_bb = llvm::BasicBlock::Create(ctx_, "task.async", fn_);
        auto* cont_bb  = llvm::BasicBlock::Create(ctx_, "task.cont", fn_);
        b_.CreateCondBr(done, sync_bb, async_bb);

        // Synchronous: the task ran to completion with no time. It already copied
        // outputs back; destroy it and continue inline.
        b_.SetInsertPoint(sync_bb);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { task_hdl });
        b_.CreateBr(cont_bb);

        // Blocked: record our handle so the task resumes us when it completes, then
        // suspend. The task drives itself through the wheel / wait queues and the
        // scheduler destroys it on completion (coro.done in the drain loop).
        b_.SetInsertPoint(async_bb);
        b_.CreateStore(hdl_, record);
        emit_suspend("task.resume");
        b_.CreateBr(cont_bb);

        b_.SetInsertPoint(cont_bb);
    }

    // Lower a $display / $write: stack-allocate an rt_arg[] descriptor array, pack
    // each value operand's planes into it, and call the runtime formatter.
    // TODO: rt_arg carries only the low 64 bits of each plane, so an operand wider
    // than 64 bits cannot be passed in full. Until the descriptor grows a
    // full-width path, such an operand is loud-failed below rather than printed
    // with its high bits silently dropped (docs/TODO.md, M1 $display).
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
                if (t.is_string) {
                    // A string argument: flag bit 1, val = the handle pointer.
                    llvm::Value* selt = b_.CreateConstInBoundsGEP1_32(sty, arr, k);
                    store_field(sty, selt, 0, b_.getInt32(0));
                    store_field(sty, selt, 1, b_.getInt32(2));
                    store_field(sty, selt, 2, b_.CreatePtrToInt(str_of(args[k]), b_.getInt64Ty()));
                    store_field(sty, selt, 3, b_.getInt64(0));
                    store_field(sty, selt, 4, b_.getInt64(0));
                    continue;
                }
                if (t.width > 64) {
                    throw BackendError{ "$display / $write of an operand wider than 64 bits is not "
                                        "supported (it would print with its high bits truncated)" };
                }
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

        // Width-1 add/sub/mul: index the per-op truth table instead of branching.
        // Add/sub truncate the carry/borrow to a^b; mul is a&b. All three keep the
        // arithmetic X rule (any X operand -> X), which the LUTs bake in.
        if (w == 1) {
            switch (tag) {
                case Inst::Tag::Add:
                    return single_bit_lut(ADD1_LUT, a, b);
                case Inst::Tag::Sub:
                    return single_bit_lut(SUB1_LUT, a, b);
                default: // Mul (lower_arith only handles add/sub/mul)
                    return single_bit_lut(MUL1_LUT, a, b);
            }
        }

        auto* wide = int_ty(2 * w);

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

    // Divide and modulo. LRM 1800-2023 §11.4.3: integer division truncates toward
    // zero, modulo takes the sign of the dividend, and a zero divisor or any X/Z
    // bit in either operand makes the whole result X.
    Plane lower_divmod(Inst::Tag tag, Plane a, Plane b, bool sign) {
        const uint32_t w      = width_of(a);
        const bool     is_div = tag == Inst::Tag::Div;

        // Width-1: index the truth table instead of branching. DIV1/MOD1 bake in
        // the X rule and the divide-by-zero -> X case.
        if (w == 1) {
            return single_bit_lut(is_div ? DIV1_LUT : MOD1_LUT, a, b);
        }

        auto* wide = int_ty(2 * w);

        llvm::Value* am = must_of(a);
        llvm::Value* bm = must_of(b);

        // One cold branch folds the X/Z test and the divide-by-zero test together:
        // both map to an all-X result, so a single not-taken branch is optimal. For
        // 2-state operands the X/Z half is statically zero and instcombine drops it,
        // leaving just `divisor == 0`.
        llvm::Value* xa      = b_.CreateXor(a.mm, b_.CreateLShr(a.mm, w));
        llvm::Value* xb      = b_.CreateXor(b.mm, b_.CreateLShr(b.mm, w));
        llvm::Value* xmask   = b_.CreateTrunc(b_.CreateOr(xa, xb), int_ty(w));
        llvm::Value* unknown = b_.CreateICmpNE(xmask, zero(w));
        llvm::Value* divzero = b_.CreateICmpEQ(bm, zero(w));
        llvm::Value* cold    = b_.CreateOr(unknown, divzero);

        auto* x_bb   = llvm::BasicBlock::Create(ctx_, "divmod.x", fn_);
        auto* val_bb = llvm::BasicBlock::Create(ctx_, "divmod.val", fn_);
        auto* end_bb = llvm::BasicBlock::Create(ctx_, "divmod.end", fn_);

        llvm::BranchInst* br = b_.CreateCondBr(cold, x_bb, val_bb);
        br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

        b_.SetInsertPoint(x_bb);
        llvm::Value* x_mm = llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * w, w));
        b_.CreateBr(end_bb);

        // Hot: divisor guaranteed nonzero. Unsigned is a direct udiv/urem. Signed
        // sdiv/srem are poison for INT_MIN / -1, so sanitize that divisor to 1 and
        // substitute the wrapped result (INT_MIN for /, 0 for %), which is the
        // two's-complement overflow value at width W.
        b_.SetInsertPoint(val_bb);
        llvm::Value* r = nullptr;
        if (!sign) {
            r = is_div ? b_.CreateUDiv(am, bm) : b_.CreateURem(am, bm);
        }
        else {
            llvm::Value* neg1 = b_.CreateICmpEQ(bm, llvm::ConstantInt::getAllOnesValue(int_ty(w)));
            llvm::Value* safe = b_.CreateSelect(neg1, llvm::ConstantInt::get(int_ty(w), 1), bm);
            if (is_div) {
                r = b_.CreateSelect(neg1, b_.CreateNeg(am), b_.CreateSDiv(am, safe));
            }
            else {
                r = b_.CreateSelect(neg1, zero(w), b_.CreateSRem(am, safe));
            }
        }
        llvm::Value* val_mm = packed(r, r, zero(w)).mm;
        b_.CreateBr(end_bb);

        b_.SetInsertPoint(end_bb);
        llvm::PHINode* mm = b_.CreatePHI(wide, 2);
        mm->addIncoming(x_mm, x_bb);
        mm->addIncoming(val_mm, val_bb);
        return { mm, zero(w) };
    }

    // Power (**), unsigned. LRM 1800-2023 §11.4.3: the exponent is self-determined
    // (so it may be a different width from the base/result), any X/Z bit makes the
    // result X, and the unsigned table corners (exp 0 -> 1, 0**n -> 0, 1**n -> 1)
    // fall out of exponentiation by squaring.
    Plane lower_pow(Plane a, Plane b) {
        const uint32_t w  = width_of(a);
        const uint32_t ew = width_of(b);

        // Width-1 base and exponent: index the truth table (exp 0 -> 1, 1**n -> 1,
        // 0**(n>0) -> 0, any X -> X). A wider exponent stays on the runtime path.
        if (w == 1 && ew == 1) {
            return single_bit_lut(POW1_LUT, a, b);
        }

        auto* wide = int_ty(2 * w);

        // X/Z in either operand -> all-X. The operands can differ in width, so each
        // mask is tested at its own width and the two feed one cold branch.
        llvm::Value* xa      = b_.CreateXor(a.mm, b_.CreateLShr(a.mm, w));
        llvm::Value* xb      = b_.CreateXor(b.mm, b_.CreateLShr(b.mm, ew));
        llvm::Value* ua      = b_.CreateICmpNE(b_.CreateTrunc(xa, int_ty(w)), zero(w));
        llvm::Value* ub      = b_.CreateICmpNE(b_.CreateTrunc(xb, int_ty(ew)), zero(ew));
        llvm::Value* unknown = b_.CreateOr(ua, ub);

        auto* x_bb   = llvm::BasicBlock::Create(ctx_, "pow.x", fn_);
        auto* val_bb = llvm::BasicBlock::Create(ctx_, "pow.val", fn_);
        auto* end_bb = llvm::BasicBlock::Create(ctx_, "pow.end", fn_);

        llvm::BranchInst* br = b_.CreateCondBr(unknown, x_bb, val_bb);
        br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

        b_.SetInsertPoint(x_bb);
        llvm::Value* x_mm = llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * w, w));
        b_.CreateBr(end_bb);

        // Hot: exponentiation by squaring at the result width W. Every product wraps
        // modulo 2^W, the correct result for width W; the wide (>64-bit) multiply
        // legalizes to the __multi3 libcall resolved in the JIT. The exponent keeps
        // its self-determined width so wide exponents are not truncated. The loop
        // always runs once (exp 0 -> one no-op iteration leaving result 1), which
        // yields the unsigned table corners directly.
        b_.SetInsertPoint(val_bb);
        auto*        wty   = int_ty(w);
        auto*        ety   = int_ty(ew);
        // Seed values are materialized in val_bb so they dominate the loop's entry edge.
        llvm::Value* base0 = must_of(a);
        llvm::Value* exp0  = must_of(b);
        auto*        loop  = llvm::BasicBlock::Create(ctx_, "pow.loop", fn_);
        b_.CreateBr(loop);

        b_.SetInsertPoint(loop);
        llvm::PHINode* result = b_.CreatePHI(wty, 2);
        llvm::PHINode* base   = b_.CreatePHI(wty, 2);
        llvm::PHINode* exp    = b_.CreatePHI(ety, 2);
        result->addIncoming(llvm::ConstantInt::get(wty, 1), val_bb);
        base->addIncoming(base0, val_bb);
        exp->addIncoming(exp0, val_bb);

        // Multiply result by base when the low exponent bit is set; always square
        // the base and shift the exponent down.
        llvm::Value* odd      = b_.CreateTrunc(exp, b_.getInt1Ty());
        llvm::Value* product  = b_.CreateMul(result, base);
        llvm::Value* next_res = b_.CreateSelect(odd, product, result);
        llvm::Value* next_bas = b_.CreateMul(base, base);
        llvm::Value* next_exp = b_.CreateLShr(exp, llvm::ConstantInt::get(ety, 1));
        llvm::Value* more     = b_.CreateICmpNE(next_exp, zero(ew));

        // Result mantissa for the loop->end edge: emitted before the terminator so it
        // dominates the predecessor's branch (PHI-incoming requirement).
        llvm::Value* val_mm = packed(next_res, next_res, zero(w)).mm;

        b_.CreateCondBr(more, loop, end_bb);
        result->addIncoming(next_res, loop);
        base->addIncoming(next_bas, loop);
        exp->addIncoming(next_exp, loop);

        b_.SetInsertPoint(end_bb);
        llvm::PHINode* mm = b_.CreatePHI(wide, 2);
        mm->addIncoming(x_mm, x_bb);
        mm->addIncoming(val_mm, loop);
        return { mm, zero(w) };
    }

    // -- Bitwise: AND/OR are monotone, so one op on the packed planes does both;
    // the result is never Z, so z is cleared. --

    // Evaluate a single-bit binary op by indexing its packed truth table: the two
    // 2-bit operand codes form idx = (a<<2)|b and the result code sits at bit
    // idx*2. One variable shift of the i32 immediate selects it — no unpack, no
    // branch. The shift count must match the LUT width, so widen the operands to
    // i32 (free in codegen); the result truncs back to the 2-bit plane.
    Plane single_bit_lut(uint32_t lut, Plane a, Plane b) {
        llvm::Value* aw = b_.CreateZExt(a.mm, int_ty(32));
        llvm::Value* bw = b_.CreateZExt(b.mm, int_ty(32));
        llvm::Value* sh = b_.CreateOr(b_.CreateShl(aw, 3), b_.CreateShl(bw, 1));
        llvm::Value* t  = b_.CreateLShr(b_.getInt32(lut), sh);
        return { b_.CreateTrunc(t, int_ty(2)), zero(1) };
    }

    Plane lower_bitwise(Inst::Tag tag, Plane a, Plane b) {
        const uint32_t w = width_of(a);
        if (tag == Inst::Tag::Band) {
            return { b_.CreateAnd(a.mm, b.mm), zero(w) };
        }
        if (tag == Inst::Tag::Bor) {
            return { b_.CreateOr(a.mm, b.mm), zero(w) };
        }
        if (tag == Inst::Tag::Bxnor) {
            if (w == 1) {
                return single_bit_lut(BXNOR1_LUT, a, b);
            }
            // Wider xnor = ~(xor): lower_bnot does must' = ~may, may' = ~must over
            // the xor result, which is exactly the negated value with the same X mask.
            return lower_bnot(lower_bitwise(Inst::Tag::Bxor, a, b));
        }
        if (w == 1) {
            return single_bit_lut(XOR1_LUT, a, b);
        }
        // Wider Bxor (non-monotone). When neither operand has an X/Z bit, XOR is
        // monotone like AND/OR — must == may in each operand, so the packed `a ^ b`
        // is already the answer. Real designs hit that case ~all the time per site,
        // so guard the cheap path and fall back to the full unpack only on X. The
        // combined X-mask `u` is computed once for the test and reused by the slow
        // path. The X-mask of a packed value is its low W bits of `mm ^ (mm >> W)`
        // (must ^ may); X-ness is nonlinear, so each operand's mask is taken
        // separately before OR-ing.
        llvm::Value* xa    = b_.CreateXor(a.mm, b_.CreateLShr(a.mm, w));
        llvm::Value* xb    = b_.CreateXor(b.mm, b_.CreateLShr(b.mm, w));
        llvm::Value* u     = b_.CreateTrunc(b_.CreateOr(xa, xb), int_ty(w));
        llvm::Value* known = b_.CreateICmpEQ(u, zero(w));

        auto* fast_bb = llvm::BasicBlock::Create(ctx_, "xor.known", fn_);
        auto* slow_bb = llvm::BasicBlock::Create(ctx_, "xor.x", fn_);
        auto* end_bb  = llvm::BasicBlock::Create(ctx_, "xor.end", fn_);

        llvm::BranchInst* br = b_.CreateCondBr(known, fast_bb, slow_bb);
        br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1u << 20, 1));

        // Hot: both operands known -> one packed xor carries both planes.
        b_.SetInsertPoint(fast_bb);
        llvm::Value* fast_mm = b_.CreateXor(a.mm, b.mm);
        b_.CreateBr(end_bb);

        // Cold: at least one X bit. Reuse `u`; value is the must-bit xor, X where u.
        b_.SetInsertPoint(slow_bb);
        llvm::Value* am      = must_of(a);
        llvm::Value* bm      = must_of(b);
        llvm::Value* k       = b_.CreateXor(am, bm);
        llvm::Value* slow_mm = packed(b_.CreateAnd(b_.CreateNot(u), k), b_.CreateOr(u, k), zero(w)).mm;
        b_.CreateBr(end_bb);

        b_.SetInsertPoint(end_bb);
        llvm::PHINode* mm = b_.CreatePHI(int_ty(2 * w), 2);
        mm->addIncoming(fast_mm, fast_bb);
        mm->addIncoming(slow_mm, slow_bb);
        return { mm, zero(w) };
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
        const uint32_t aw = width_of(amt);
        llvm::Value*   am = must_of(a);
        llvm::Value*   aM = may_of(a);

        // Detect a shift amount >= width at the amount's untruncated width. LLVM
        // shift ops are poison once the amount reaches the operand width (LRM
        // §11.4.10 instead defines the result), and truncating the amount to `w`
        // first would alias multiples of 2**w down to 0. Compare in a width that
        // holds both the amount and `w`.
        const uint32_t cw  = std::max(aw, w);
        llvm::Value*   oob = b_.CreateICmpUGE(cast(must_of(amt), cw, false),
                                              llvm::ConstantInt::get(int_ty(cw), w));

        // Clamp to w-1 so every LLVM shift below is well-defined; the truncated
        // amount is exact whenever the shift is in range (a value < w fits in w
        // bits), and the out-of-range result is patched in afterwards.
        llvm::Value* sh   = cast(must_of(amt), w, false);
        llvm::Value* safe = b_.CreateSelect(oob, llvm::ConstantInt::get(int_ty(w), w == 0 ? 0 : w - 1), sh);

        const bool   arith = tag == Inst::Tag::Sra && sign;
        llvm::Value* must;
        llvm::Value* may;
        llvm::Value* z;
        switch (tag) {
            case Inst::Tag::Sll:
                must = b_.CreateShl(am, safe);
                may  = b_.CreateShl(aM, safe);
                z    = b_.CreateShl(a.z, safe);
                break;
            case Inst::Tag::Srl:
                must = b_.CreateLShr(am, safe);
                may  = b_.CreateLShr(aM, safe);
                z    = b_.CreateLShr(a.z, safe);
                break;
            default: // Sra: arithmetic (signed) replicates the sign and its
                     // unknown-ness; unsigned `>>>` is logical.
                must = arith ? b_.CreateAShr(am, safe) : b_.CreateLShr(am, safe);
                may  = arith ? b_.CreateAShr(aM, safe) : b_.CreateLShr(aM, safe);
                z    = arith ? b_.CreateAShr(a.z, safe) : b_.CreateLShr(a.z, safe);
                break;
        }

        // Out-of-range: logical shifts (and unsigned `>>>`) produce 0. The
        // arithmetic `>>>` already yields all sign bits from the clamp to w-1.
        if (!arith) {
            must = b_.CreateSelect(oob, zero(w), must);
            may  = b_.CreateSelect(oob, zero(w), may);
            z    = b_.CreateSelect(oob, zero(w), z);
        }

        llvm::Value* unknown = any_unknown(amt);
        return packed(b_.CreateSelect(unknown, zero(w), must),
                      b_.CreateSelect(unknown, ones(w), may),
                      b_.CreateSelect(unknown, zero(w), z));
    }

    // -- Comparisons -> 1-bit; X/Z in either operand yields X. --

    // Build a 1-bit 4-state result split on `unknown`: the cold block takes the
    // X-case word from `cold_mm()`, the hot block computes a known i1 from
    // `fast_bit()` and splats it across both planes. `unknown` is the only read of
    // the may plane, so once it is statically false (2-state) the guard folds away
    // and just the hot op survives. Mirrors the arithmetic guard.
    template <class ColdF, class FastF>
    Plane branchy_bit(llvm::Value* unknown, ColdF cold_mm, FastF fast_bit) {
        auto* x_bb   = llvm::BasicBlock::Create(ctx_, "bit.x", fn_);
        auto* val_bb = llvm::BasicBlock::Create(ctx_, "bit.val", fn_);
        auto* end_bb = llvm::BasicBlock::Create(ctx_, "bit.end", fn_);

        llvm::BranchInst* br = b_.CreateCondBr(unknown, x_bb, val_bb);
        br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

        b_.SetInsertPoint(x_bb);
        llvm::Value* x_mm = cold_mm();
        b_.CreateBr(end_bb);

        b_.SetInsertPoint(val_bb);
        llvm::Value* c      = fast_bit();
        llvm::Value* val_mm = bit_plane(c, c).mm;
        b_.CreateBr(end_bb);

        b_.SetInsertPoint(end_bb);
        llvm::PHINode* mm = b_.CreatePHI(int_ty(2), 2);
        mm->addIncoming(x_mm, x_bb);
        mm->addIncoming(val_mm, val_bb);
        return { mm, zero(1) };
    }

    Plane lower_compare(Inst::Tag tag, Plane a, Plane b, bool sign) {
        // Equality keeps the branchless X-mask (and a width-1 LUT): == / != are
        // cheap and fold the X mask straight into the result.
        if (tag == Inst::Tag::Eq || tag == Inst::Tag::Ne) {
            if (width_of(a) == 1) {
                return single_bit_lut(tag == Inst::Tag::Eq ? EQ1_LUT : NE1_LUT, a, b);
            }
            const auto pred =
              tag == Inst::Tag::Eq ? llvm::CmpInst::ICMP_EQ : llvm::CmpInst::ICMP_NE;
            llvm::Value* cmp     = b_.CreateICmp(pred, must_of(a), must_of(b));
            llvm::Value* unknown = b_.CreateOr(any_unknown(a), any_unknown(b));
            return bit_plane(b_.CreateAnd(b_.CreateNot(unknown), cmp), b_.CreateOr(unknown, cmp));
        }

        // Ordered relationals: any x/z yields X, exactly like arithmetic, so guard
        // on it. The hot block is one native compare on the data (must) bits; the
        // cold block is the all-X word (must 0, may 1).
        llvm::CmpInst::Predicate pred;
        switch (tag) {
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
        llvm::Value* unknown = b_.CreateOr(any_unknown(a), any_unknown(b));
        return branchy_bit(
          unknown, [&] { return llvm::ConstantInt::get(int_ty(2), 0b10); },
          [&] { return b_.CreateICmp(pred, must_of(a), must_of(b)); });
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
        const uint32_t w = std::max(width_of(a), width_of(b));
        a                = cast_plane(a, w, false);
        b                = cast_plane(b, w, false);

        llvm::Value* eq;
        if (tag == Inst::Tag::CaseEq) {
            // === : bit-exact equality. The packed mm word carries both the must
            // and may planes, so a single icmp on the word covers them at once;
            // the z plane is compared alongside. One icmp per plane, no unpack.
            eq = b_.CreateAnd(b_.CreateICmpEQ(a.mm, b.mm), b_.CreateICmpEQ(a.z, b.z));
        } else {
            // casez/casex mask out don't-care bits, so they need the planes split.
            llvm::Value* am   = must_of(a);
            llvm::Value* aM   = may_of(a);
            llvm::Value* bm   = must_of(b);
            llvm::Value* bM   = may_of(b);
            llvm::Value* dc   = tag == Inst::Tag::CasezEq
                                  ? b_.CreateOr(a.z, b.z)
                                  : b_.CreateOr(b_.CreateXor(am, aM), b_.CreateXor(bm, bM));
            llvm::Value* keep = b_.CreateNot(dc);
            eq                = b_.CreateAnd(
              b_.CreateICmpEQ(b_.CreateAnd(am, keep), b_.CreateAnd(bm, keep)),
              b_.CreateICmpEQ(b_.CreateAnd(aM, keep), b_.CreateAnd(bM, keep)));
        }
        return bit_plane(eq, eq);
    }

    // Wildcard equality `==?`. LRM 1800-2023 §11.4.6: x/z in the RIGHT operand are
    // wildcards (that bit position is not compared); on the remaining positions it
    // is an ordinary X-propagating `==`, so a left-operand x/z there yields X.
    // Masking both operands to the kept (non-wildcard) bits and running `==` gives
    // exactly that: the right is known on kept bits so it never injects X, while a
    // left x/z on a kept bit trips the unknown path.
    Plane lower_wildeq(Plane a, Plane b) {
        const uint32_t w     = std::max(width_of(a), width_of(b));
        a                    = cast_plane(a, w, false);
        b                    = cast_plane(b, w, false);
        auto*          wide  = int_ty(2 * w);
        llvm::Value*   unkb  = b_.CreateXor(must_of(b), may_of(b)); // x/z bits of b
        llvm::Value*   keep  = b_.CreateNot(unkb);
        llvm::Value*   keepW = b_.CreateOr(b_.CreateZExt(keep, wide),
                                           b_.CreateShl(b_.CreateZExt(keep, wide), w));
        Plane ma = { b_.CreateAnd(a.mm, keepW), b_.CreateAnd(a.z, keep) };
        Plane mb = { b_.CreateAnd(b.mm, keepW), b_.CreateAnd(b.z, keep) };
        return lower_compare(Inst::Tag::Eq, ma, mb, false);
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
        // At width 1 the truth of each operand is the bit itself, so &&/|| are the
        // monotone bitwise &/| — one packed op, no truth reduction. It also degrades
        // straight to a plain &/| once the may plane is elided, unlike a LUT.
        if (width_of(a) == 1 && width_of(b) == 1) {
            llvm::Value* mm =
              tag == Inst::Tag::Land ? b_.CreateAnd(a.mm, b.mm) : b_.CreateOr(a.mm, b.mm);
            return { mm, zero(1) };
        }

        // Multi-bit: with no x/z each operand's truth is just "nonzero", so &&/||
        // are a plain boolean on the data bits. An x/z is rarer and can still be
        // dominated (0 && x = 0), so the cold block keeps the full 3-valued logic.
        const uint32_t wa      = width_of(a);
        const uint32_t wb      = width_of(b);
        llvm::Value*   unknown = b_.CreateOr(any_unknown(a), any_unknown(b));
        return branchy_bit(
          unknown,
          [&] {
              const Truth ta = truth_of(a);
              const Truth tb = truth_of(b);
              return tag == Inst::Tag::Land
                       ? bit_plane(b_.CreateAnd(ta.t, tb.t), b_.CreateNot(b_.CreateOr(ta.f, tb.f))).mm
                       : bit_plane(b_.CreateOr(ta.t, tb.t), b_.CreateNot(b_.CreateAnd(ta.f, tb.f))).mm;
          },
          [&] {
              llvm::Value* at = b_.CreateICmpNE(must_of(a), zero(wa));
              llvm::Value* bt = b_.CreateICmpNE(must_of(b), zero(wb));
              return tag == Inst::Tag::Land ? b_.CreateAnd(at, bt) : b_.CreateOr(at, bt);
          });
    }

    Plane lower_lnot(Plane a) {
        // At width 1, !a is the same operation as ~a (negate the single truth bit),
        // so reuse the bnot plane swap+complement — cheaper than a truth pair, and
        // already optimal (one not + one rotate), so no LUT is warranted.
        if (width_of(a) == 1) {
            return lower_bnot(a);
        }
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
        // The first part is OR'd in without a preceding shift: a shift by the
        // full result width (which happens when the concat has a single operand,
        // pw == w) is poison in LLVM, and shifting the still-zero accumulator is
        // a no-op anyway.
        bool first = true;
        for (Index part : parts) {
            const uint32_t pw = type_of(part).width;
            const Plane    pv = value_of(part);
            if (!first) {
                must = b_.CreateShl(must, pw);
                may  = b_.CreateShl(may, pw);
                z    = b_.CreateShl(z, pw);
            }
            must  = b_.CreateOr(must, cast(must_of(pv), w, false));
            may   = b_.CreateOr(may, cast(may_of(pv), w, false));
            z     = b_.CreateOr(z, cast(pv.z, w, false));
            first = false;
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
        // Skip the shift on the first copy: for a single replication ({1{x}})
        // part_width == w, and a shift by the full width is poison in LLVM.
        for (uint32_t c = 0; c < count; ++c) {
            if (c != 0) {
                must = b_.CreateShl(must, part_width);
                may  = b_.CreateShl(may, part_width);
                z    = b_.CreateShl(z, part_width);
            }
            must = b_.CreateOr(must, pm);
            may  = b_.CreateOr(may, pM);
            z    = b_.CreateOr(z, pz);
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
        branch_to(cont_bb);

        b_.SetInsertPoint(else_bb);
        lower_region(ir_->region(regs.else_));
        branch_to(cont_bb);

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
        branch_to(step_bb); // a `return` in the body terminates it; don't double-branch

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
    // The root `%clotho.state` struct: top-instance state structs, unscoped
    // signals, then the NBA shadow struct (when the design has NBAs).
    llvm::StructType*       state_ty_ = nullptr;
    // The NBA shadow struct and its field index in the root (valid iff non-null).
    llvm::StructType*       shadow_ty_    = nullptr;
    uint32_t                shadow_field_ = 0;
    // Slot-type memo, one `{ i(2W)[, iW] }` per (width, domain, has_z).
    std::vector<std::tuple<uint32_t, bool, bool, llvm::StructType*>> slot_types_;
    // Per-definition state-struct memo: (definition name, field types, type).
    // Instances of one definition share the type; a same-named definition with
    // different fields (different parameterization) gets its own entry.
    std::vector<std::tuple<std::string, std::vector<llvm::Type*>, llvm::StructType*>> def_types_;
    std::vector<SignalSlot> slots_;

    // Function declarations, indexed by FunctionId; filled before any body is
    // lowered so calls (including recursive / forward) resolve.
    std::vector<llvm::Function*> fn_decls_;
    // Set while lowering a function or task body (null during a process). `frame_`
    // is the current call's frame; `frame_slots_` / `frame_ty_` its layout.
    // `cur_is_task_` selects the coroutine return path (epilogue) for Ret.
    const Function*         cur_fn_      = nullptr;
    bool                    cur_is_task_ = false;
    // LLVM arg index of each of the current function/task's formals (see
    // param_indices) — string formals make the stride non-uniform.
    std::vector<uint32_t> param_llvm_index_;
    // Set while lowering a PreInit process: its stores apply variable declaration
    // initializers and must generate no events, so wait-queue servicing is skipped.
    bool                    lowering_preinit_ = false;
    // Compile/simulation knobs (z elision, ...); see SimConfig.h.
    SimulationConfig        config_;
    llvm::Value*            frame_       = nullptr;
    std::vector<FrameSlot>  frame_slots_;
    // The current function's frame struct (one field per Local, in order).
    llvm::StructType*       frame_ty_    = nullptr;
    // While lowering a task: the shared copy-out + caller-resume epilogue block a
    // Ret branches to. The caller-side record slot { ptr parent } for task calls
    // this coroutine makes (a process or a task can be a task caller).
    llvm::BasicBlock*       task_epilogue_bb_ = nullptr;
    llvm::AllocaInst*       record_           = nullptr;

    // Per-process scratch, reset in lower_process.
    const Ir*           ir_         = nullptr;
    llvm::Function*     fn_         = nullptr;
    llvm::Value*        state_      = nullptr;
    llvm::Value*        wheel_      = nullptr;
    llvm::Value*        hdl_        = nullptr;
    llvm::Instruction*  coro_begin_ = nullptr;
    llvm::AllocaInst*   awaiter_    = nullptr;
    // Reused scratch for staging array-NBA value/mask bytes before a push call
    // (one per process, hoisted to coro.begin so a loop of element NBAs does not
    // grow the stack). Layout: [16]mm_val [16]mm_mask [8]z_val [8]z_mask.
    llvm::AllocaInst*   nba_scratch_ = nullptr;
    llvm::BasicBlock*   coro_cleanup_bb_ = nullptr;
    llvm::BasicBlock*   coro_ret_bb_     = nullptr;
    // The current coroutine's scheduled flag: &clotho.proc_sched[this process] for
    // a process (a Constant GEP), or a frame-resident i32 alloca for a task.
    llvm::Value*        sched_ptr_       = nullptr;
    // The shared timer callback (pushes a woken handle onto the active queue) and
    // the active/inactive queue globals, all built once per module. The inactive
    // queue holds processes parked on a zero delay (#0): drained into active at
    // the same instant, after active settles but before NBA.
    llvm::Function*                     enqueue_fn_        = nullptr;
    llvm::GlobalVariable*               active_gv_         = nullptr;
    llvm::GlobalVariable*               active_top_gv_     = nullptr;
    llvm::GlobalVariable*               inactive_gv_       = nullptr;
    llvm::GlobalVariable*               inactive_top_gv_   = nullptr;
    llvm::ArrayType*                    queue_arr_ty_      = nullptr;
    // Per-signal wait queues, indexed by Signal handle and then by edge bucket
    // (`[signal][static_cast<uint32_t>(edge)]`: 0=pos, 1=neg, 2=any); null where the
    // signal has no waiter of that (kind, edge). Splitting by edge hoists the
    // edge check out of the scan into one branch per bucket. Static (.sta):
    // fixed-capacity persistent subscriptions, `wait_sta_cap_` the compile-time
    // trip count, no length. Dynamic (.dyn): one-shot waiters with `wait_dyn_len_gv_`
    // live count. Built in build_wait_queues; serviced by service_static_queue /
    // service_dynamic_queue.
    std::vector<std::array<llvm::GlobalVariable*, 3>>  wait_sta_gv_;
    std::vector<std::array<llvm::ArrayType*, 3>>       wait_sta_ty_;
    std::vector<std::array<uint32_t, 3>>               wait_sta_cap_;
    std::vector<std::array<llvm::GlobalVariable*, 3>>  wait_dyn_gv_;
    std::vector<std::array<llvm::ArrayType*, 3>>       wait_dyn_ty_;
    std::vector<std::array<llvm::GlobalVariable*, 3>>  wait_dyn_len_gv_;
    // Next free static slot per (signal, edge), handed out during lowering
    // (parallels the capacities computed in build_wait_queues).
    std::vector<std::array<uint32_t, 3>>               sta_next_;
    // Per-process scheduled flags (clotho.proc_sched), built once in run().
    llvm::GlobalVariable*               sched_gv_     = nullptr;
    llvm::ArrayType*                    sched_arr_ty_ = nullptr;

    // Nonblocking-assignment update state, built in build_nba_state(). A StoreNb
    // stages its RHS into the target's shadow and appends the target to its bucket
    // worklist (deduped by nba_pending); the NBA region streams each bucket through
    // a width-specialized commit loop that copies/RMWs shadow -> live and services
    // the wait queue. Empty (no NBAs) leaves all of this null/zero.
    std::vector<NbaBucket>  nba_buckets_;
    uint32_t                n_nba_  = 0; // distinct NBA-target signals
    llvm::GlobalVariable*   nba_pending_gv_ = nullptr; // [n_nba x i8] dedup flags
    llvm::ArrayType*        nba_pending_ty_ = nullptr;
    llvm::GlobalVariable*   nba_meta_gv_    = nullptr; // [n_nba x NbaMeta] constants
    llvm::ArrayType*        nba_meta_arr_ty_ = nullptr;
    llvm::StructType*       nba_meta_ty_    = nullptr;
    // Unpacked-array signals that are nonblocking-assignment targets. Their
    // per-element writes go through the runtime deferred queue (rt_nba_arr_*)
    // rather than the per-signal shadow; after a commit, each is woken if any
    // queued write touched it.
    std::vector<Signal>     array_nba_sigs_;
    std::vector<Plane>                  values_;
    std::vector<std::optional<LValue>>  lvals_;
    std::vector<std::optional<ValType>> types_;
    // rt_string handle pointers of string-valued instructions, parallel to
    // values_ (a string never flows as a Plane).
    std::vector<llvm::Value*> str_vals_;
    // The current body's string rvalue temporaries, deinit'd at its exits.
    std::vector<llvm::Value*> str_temp_list_;
    // Memoized `%clotho.string` handle slot type.
    llvm::StructType* str_slot_ty_ = nullptr;
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

std::unique_ptr<llvm::Module> build_module(const Module& m, llvm::LLVMContext& ctx,
                                           const SimulationConfig& config) {
    auto llmod = std::make_unique<llvm::Module>(m.name.empty() ? "clotho" : m.name, ctx);

    // Pin the module to the host target's triple and DataLayout before any IR is
    // built: the state/frame struct layouts, the NBA meta table's baked byte
    // offsets, and load/store alignments are all DataLayout-derived, so they
    // must be computed against the layout codegen will actually use.
    {
        static std::once_flag once;
        std::call_once(once, [] { llvm::InitializeNativeTarget(); });
        auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!jtmb) {
            llvm::errs() << "llvm backend: cannot detect host target: "
                         << llvm::toString(jtmb.takeError()) << "\n";
            return nullptr;
        }
        auto dl = jtmb->getDefaultDataLayoutForTarget();
        if (!dl) {
            llvm::errs() << "llvm backend: no host data layout: " << llvm::toString(dl.takeError())
                         << "\n";
            return nullptr;
        }
        llmod->setTargetTriple(jtmb->getTargetTriple());
        llmod->setDataLayout(*dl);
    }

    try {
        Backend(m, ctx, *llmod, config).run();
    } catch (const BackendError& e) {
        llvm::errs() << "llvm backend error: " << e.message << "\n";
        return nullptr;
    }

    // Verify the freshly-built (pre-split) module. With optimize off, stop here
    // and hand back this raw module (coro intrinsics intact) for inspection.
    if (llvm::verifyModule(*llmod, &llvm::errs())) {
        return nullptr;
    }
    if (!config.optimize) {
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

bool emit_llvm(const Module& m, llvm::raw_ostream& os, const SimulationConfig& config) {
    llvm::LLVMContext ctx;
    auto              llmod = build_module(m, ctx, config);
    if (!llmod) {
        return false;
    }

    llmod->print(os, nullptr);
    return true;
}

} // namespace clotho::ir
