#include "ir/LlvmBackend.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

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
#include <llvm/IR/ModuleSlotTracker.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Utils/Cloning.h>

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
    // Real / shortreal: the value is an IEEE-754 double (width 64) or float
    // (width 32) riding Plane.fp; never four_state.
    bool is_real = false;
};

// The type_of memo stores one ValType per instruction; keep it one dense
// 8-byte word (validity lives in a parallel byte vector, not an optional).
static_assert(sizeof(ValType) == 8);

// A lowered value: the must/may planes packed into one i(2W) integer (low W bits
// = must / definitely 1, high W bits = may / could be 1), plus a separate iW `z`
// (high-impedance marker). Packing lets the monotone ops (AND/OR/NOT, select select)
// act on both planes with a single instruction.
struct Plane {
    llvm::Value* mm = nullptr; // 4-state: i(2W) [0,W) must, [W,2W) may. 2-state: iW value.
    llvm::Value* z  = nullptr; // 4-state z plane (iW), or null when never-z. Null for 2-state.
    // A genuine 2-state value: `mm` is a single iW with no unknown/z plane. The
    // accessors (must_of/may_of/width_of/any_unknown) read this flag, so a
    // 2-state value never carries the redundant may/z half. Domain is inferred
    // per value, not per instruction; see the operator dispatch in lower_inst.
    bool two = false;
    // Real / shortreal value (double / float). Exactly one of fp and mm is set:
    // a real SSA value carries fp only; its storage form is the raw bit pattern
    // in a 2-state slot (see the real helpers).
    llvm::Value* fp = nullptr;
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
    // A genuine single-iW 2-state slot: the mm word is `iW`, not `i(2W)`, and
    // there is no z word. Loads yield a two_val Plane; stores write the iW value.
    bool narrow = false;
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
    // Storage-width in-range mask for a bounds-checked dynamic bit/part select
    // (LRM 11.5.1): 1 for each selected bit that is in range. Null when no bounds
    // check applies (whole storage, or a constant in-range select). A read forces
    // x/0 outside it and a write is restricted to it.
    llvm::Value* range_mask = nullptr;
    // A module signal (not a frame local): its store services wait queues. `sig`
    // is meaningful only then.
    bool   is_signal = false;
    Signal sig{ 0 };
    // A string target: `base` (indexed by elem_idx for an array) is the
    // rt_string handle; widths / bit_off / has_z are meaningless.
    bool is_string = false;
    // A dynamic-array / queue handle leaf (inside an aggregate or a class
    // body). Addressed via lv_handle_ptr by the queue / dynamic-array ops;
    // plane Load/Store on it traps (the frontend never emits one).
    bool is_handle = false;
    // Recursive aggregate addressing (a signal whose type contains an unpacked
    // struct). `root_ty` is the signal's whole storage type; `agg_path` is the
    // GEP index vector from `base` to the current sub-object (a leaf slot once a
    // select chain resolves to a scalar/string). A struct field is a constant
    // index, an array element a runtime index — composed uniformly, so arbitrary
    // nesting works. `is_agg` selects this addressing in lv_field_ptr.
    bool                      is_agg  = false;
    llvm::Type*               root_ty = nullptr;
    std::vector<llvm::Value*> agg_path;
    // True once `agg_path` has reached a scalar/string leaf (leaf metadata is
    // filled); false while the lvalue still designates an interior aggregate that
    // a further select must descend.
    bool agg_leaf = false;
    // Whether the aggregate path is fully STATIC (struct fields only, no runtime
    // array index): a static leaf commits via the block-commit shadow, a runtime
    // one via the deferred path. `agg_field_path` records the constant field
    // indices so the target static leaf can be matched in SignalSlot::static_leaves.
    bool                  agg_static = true;
    std::vector<uint32_t> agg_field_path;
    // A dynamic-array or queue element target (LRM 7.5.1 / 7.10): `base` is the
    // heap data pointer loaded from the handle, `elem_idx` the physical element
    // position (the wrapped ring index for a queue), and this is the i1 "index in
    // [0, len)" predicate. When set, a read forces the element default (x/0) out
    // of range and a write is suppressed out of range, so the heap buffer is
    // never accessed past its length.
    llvm::Value* dyn_inrange = nullptr;
};

// A scalar / string leaf of an aggregate signal reachable by a STATIC path
// (struct fields only, never an array index), so its location is a compile-time
// constant GEP. Used for nonblocking staging: the block-commit path can shadow
// and commit these leaves because their addresses are known. A leaf under an
// array dimension is not static (its element index is runtime) and takes the
// deferred path instead, so it is not listed here.
struct AggLeaf {
    // Constant GEP field indices from the aggregate storage root to this leaf
    // slot (each a struct field index; no array indices by construction).
    std::vector<uint32_t> path;
    llvm::StructType*     slot_ty    = nullptr;
    uint32_t              width      = 0;
    bool                  four_state = false;
    bool                  has_z      = false;
    bool                  narrow     = false;
    bool                  is_string  = false;
    // Shadow-struct field indices for nonblocking staging (mm plane, and z plane
    // when has_z), assigned in build_state_layout when the aggregate is an NBA
    // target. Unused otherwise.
    uint32_t sh_mm_field = 0;
    uint32_t sh_z_field  = 0;
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
    // A genuine single-iW 2-state slot (`{ iW }`): the packed must|may word is
    // dropped. Set only for a 2-state signal proven decoupled from every path that
    // still reads the packed form (ABI, NBA, arrays). Loads yield a two_val Plane;
    // stores write the must plane as iW.
    bool narrow = false;
    // Powers up as z rather than x: an undriven 4-state net (LRM 6.6). Implies
    // has_z (undriven nets are a may-z seed).
    bool init_z = false;
    // Leaf element is real / shortreal: the slot stores the IEEE-754 bit
    // pattern in a 2-state word (has_z is false, four_state is false).
    bool is_real = false;
    // Unpacked array (memory): `width` is the leaf element width; `elem_count`
    // the total leaf-element count (product of all dimensions).
    bool     is_array   = false;
    uint32_t elem_count = 0;
    // Nonblocking-assignment staging. Set only for signals that are NBA targets.
    // A target gets shadow `i(2W)` / `iW` fields mirroring its live slot in the
    // root's shadow struct; a target with any dynamic (runtime-indexed) select site
    // also gets an iW dirty mask field. nba_id is a dense index into the meta table;
    // the sh_* members are field indices into the shadow struct.
    bool     nba           = false;
    bool     nba_partial   = false;
    // Any bit/part-select NBA site with a runtime index/offset (BitSelect /
    // DynPartSelect). Such a signal carries a runtime dirty-mask shadow field
    // (sh_mask_field); a static-only partial target instead gets a compile-time
    // constant mask baked into its per-block commit code, no field.
    bool     nba_dyn       = false;
    // Scalar (non-array) NBA target committed per straight-line block (whole and
    // partial alike). False only for array-element targets, which use the runtime
    // deferred queue.
    bool     nba_block     = false;
    // Array-element NBA target with any within-element bit/part-select site: its
    // deferred records carry a per-write mask (whole-element sites store all-ones).
    // A memory written only whole-element (mem[i] <= x) needs no mask.
    bool     arr_masked    = false;
    uint32_t nba_id        = 0;
    uint32_t sh_mm_field   = 0;
    uint32_t sh_z_field    = 0;
    uint32_t sh_mask_field = 0;
    // A string (or array-of-string) signal: the slot is an opaque rt_string
    // handle (`clotho.string`), has_z is false, and `nba` is never set (the
    // frontend rejects nonblocking assignment to strings).
    bool is_string = false;
    // An aggregate signal (its type contains an unpacked struct at some nesting
    // depth): `storage_ty` is the recursive mirror built by agg_storage_ty
    // (nested StructType / ArrayType / leaf slots), addressed by a GEP path
    // (LValue::agg_path) that a MemberSelect / ElemSelect chain composes. `slot_ty`
    // is null and `width` unused (an aggregate has no flat value); whole-object
    // ops decompose in the frontend. `static_leaves` lists the scalar/string
    // leaves reachable by a struct-field-only (static) path, used for nonblocking
    // staging (a leaf under an array is runtime and takes the deferred path).
    bool                  is_agg = false;
    std::vector<AggLeaf>  static_leaves;
    // The signal's interned IR type (aggregate recursion needs it: emit_agg_init,
    // the deferred-runtime-index guard). Set for every slot.
    Ref ir_type = Ref::Invalid;
    // An aggregate nonblocking target: static leaves are committed from an
    // aggregate shadow (each static leaf owns a shadow slot); the block-commit
    // BlockSig `mask` is reinterpreted as a static-leaf-set (bit k = leaf k
    // written). Set by nba_classify; implies `nba` and `nba_block`.
    bool nba_agg = false;
    // Any deferred aggregate NBA site is a partial (bit/part-select) leaf write:
    // its records carry a per-plane mask and the commit does a byte-wise masked
    // read-modify-write instead of a memcpy (mirrors `arr_masked`). Whole-leaf
    // writes to a masked aggregate use an all-ones mask.
    bool agg_masked = false;
    // A dynamic-array signal: the slot is an opaque rt_dynarr handle
    // (`clotho.dynarr`), has_z is false. Elements live in its heap buffer,
    // reached by the ElemSelect dynarr path; the handle powers on { null, 0 }
    // (an empty array) and is freed at teardown.
    bool is_dynarr = false;
    // A queue signal: the slot is an opaque rt_queue ring handle
    // (`clotho.queue`), has_z is false. Elements live in its heap ring buffer,
    // reached by the ElemSelect queue path; the handle powers on
    // { null, 0, 0, 0 } (an empty queue) and is freed at teardown.
    bool is_queue = false;
    // Index of the instance-scope anchor this signal is addressed through
    // (Anchor / storage_ptr); no_anchor for an unscoped root-level signal,
    // which is addressed directly off @clotho.state.
    static constexpr uint32_t no_anchor = ~0u;
    uint32_t anchor = no_anchor;
};

// One instance-scope occurrence: the addressing anchor for every signal the
// scope declares. Each scope owns its own state global
// (@clotho.inst.<hier>.state, own signals only — children own theirs), so a
// signal access is `scope global + def-local offset`: a real symbol plus a
// def-content-derived addend, the relocation shape per-definition compilation
// units and object caching need. A definition's code never embeds
// whole-design layout. Unique-definition instances bake the symbol directly
// (link-time constant, free at run time); a scope of a reused definition
// loads a base pointer main stores at startup when the body is shareable
// (one shared body cannot bake N instance symbols).
struct Anchor {
    // The scope's own state global (@clotho.inst.<hier>.state), created by
    // emit_state_global.
    llvm::GlobalVariable* state_gv = nullptr;
    // The scope's definition state struct (GEP source type for member access).
    llvm::StructType* ty = nullptr;
    // Hierarchical instance name (diagnostic / symbol naming).
    std::string name;
    // True when the scope's definition state type is instantiated more than
    // once: def-relative code addresses through the instance context.
    bool indirect = false;
    // Lowest process index owned by this scope (~0 when it owns none).
    uint32_t first_proc = ~0u;
    // This scope's own scheduled-flag array (@clotho.inst.<hier>.sched,
    // [n_own x i32]) and its type; null when the scope owns no processes. An
    // owned group's flag is `sched_gv + def-local dense ordinal` — a real
    // symbol plus a def-content addend (relocatable), or ctx field 2 + the
    // same ordinal in a shareable reused-definition body.
    llvm::GlobalVariable* sched_gv = nullptr;
    llvm::ArrayType*      sched_ty = nullptr;
    // This scope's NBA shadow: the per-def staging struct type (null when
    // the scope has no NBA shadow fields), its own global
    // (@clotho.inst.<hier>.shadow), and the opaque base-pointer global
    // (@clotho.inst.<hier>.shadow_base, lazy) every unshared shadow access
    // loads — one load, preserving the mis-speculation opacity (see
    // shadow_ptr) with a def-stable symbol.
    llvm::StructType*     shadow_ty      = nullptr;
    llvm::GlobalVariable* shadow_gv      = nullptr;
    llvm::GlobalVariable* shadow_base_gv = nullptr;
    // Lowest VCD dump-var index of the scope's own dumped signals (-1: none).
    // Touch sites address a dumped signal as vcd base + def-local offset.
    int32_t first_vcd = -1;

    // Instance context (reused definitions): ONE per-instance global
    // (@clotho.inst.<hier>.ctx) holding every base a def-relative body needs,
    // laid out per definition so twin instances load identical field
    // constants — the single symbol a shared body later swaps for a
    // parameter. Filled by main at startup. Layout:
    //   0             ptr  state base (the scope's storage)
    //   1             ptr  shadow sub-struct base (null when no NBA shadows)
    //   2             ptr  the scope's sched-flag array base (Anchor::sched_gv)
    //   3             i32  first_vcd
    //   ctx_blk_base  i32  first NBA block id, one per def-local single-scope
    //                      group ordinal (0 when the group registers none)
    //   ctx_wait_base ptr  wait bucket bases (sta / dyn / dyn-len), def-local
    //                      slot order from the bucket walk
    // ctx_ty is shared across instances of one definition; the global is
    // created lazily by the first def-relative use.
    llvm::StructType*     ctx_ty        = nullptr;
    llvm::GlobalVariable* ctx_gv        = nullptr;
    uint32_t              ctx_blk_base  = 0;
    uint32_t              ctx_wait_base = 0;
    // Def-local single-scope group count (sizes the blk fields).
    uint32_t n_groups = 0;
};

// One function-frame local: its `{ i(2W), iW }` slot type and width / domain.
// The frame is an LLVM struct with one field per local, in Local-handle order,
// so the local's index IS its GEP field index. No wait-queue / NBA machinery
// (frame locals have neither).
struct FrameSlot {
    llvm::StructType* slot_ty    = nullptr;
    uint32_t          width      = 0;
    bool              four_state = false;
    // A genuine single-iW 2-state local (`{ iW }`). The ABI wire is still the
    // packed word, so copy-in projects into the iW slot and copy-out lifts back.
    bool narrow = false;
    // A string local: the slot is an rt_string handle; init/deinit bracket the
    // call.
    bool is_string = false;
    // An aggregate local (its type contains an unpacked struct): `storage_ty` is
    // the recursive mirror (agg_storage_ty), addressed by an is_agg LValue rooted
    // at the frame slot. Passed / returned by pointer; copy-in/out is a whole-
    // object copy. `slot_ty` is null and `width` unused.
    bool        is_agg     = false;
    llvm::Type* storage_ty = nullptr;
    // The local's interned IR type (aggregate recursion needs it).
    Ref ir_type = Ref::Invalid;
    // A dynamic-array or queue local: the slot is a handle; it powers on empty
    // ({0}) and is freed at every body exit, like a string local. `handle_type`
    // is the interned handle type (picks the runtime slot / delete function).
    bool is_dynarr = false;
    bool is_queue  = false;
    Ref  handle_type = Ref::Invalid;
    // A ref formal: the local has no real frame storage (its dead field is kept
    // only so Local index == field index). Loads / stores redirect to the passed
    // actual pointer, recorded in ref_ptr_ by the callee prologue.
    bool is_ref = false;
};

// Raised on a construct the backend cannot lower; caught at the top level.
struct BackendError {
    std::string message;
};

// Per-compilation-unit emission state: the llvm::Module IR is currently being
// emitted into, plus the memo mapping symbols defined in *other* units to this
// module's local external declarations (Backend::sym). Prep for the
// per-definition module split: today the whole design is one unit and every
// symbol is local, so the memo stays empty; the split instantiates one env per
// unit and swaps the active one between emission passes.
struct ModuleEnv {
    llvm::Module& llmod;
    // Defining GlobalValue -> this module's declaration of it. Identity
    // mappings are not stored (sym returns the definition directly).
    ankerl::unordered_dense::map<llvm::GlobalValue*, llvm::GlobalValue*> decls;
    // Internal constants with non-unique names, duplicated per module rather
    // than externalized: rt_elem_ops descriptors keyed by element type Ref,
    // and the write-only string sink. Memoized per env so each unit creates
    // its own copies.
    ankerl::unordered_dense::map<uint32_t, llvm::Constant*> elem_ops;
    llvm::GlobalVariable* str_sink = nullptr;
};

// Builds the shared state struct and lowers every process. One instance per
// emit; `lower_process` is reused across processes (the per-process value tables
// are reset each time).
class Backend {
public:
    Backend(const Module& mod, llvm::LLVMContext& ctx, llvm::Module& llmod, const SimulationConfig& config)
      : mod_(mod)
      , ctx_(ctx)
      , root_env_(std::make_unique<ModuleEnv>(llmod))
      , env_(root_env_.get())
      , b_(ctx)
      , config_(config) {}

    // Definition-unit modules created by the split emission (lower_group loop),
    // in creation order. The caller links them alongside the root module.
    std::vector<std::unique_ptr<llvm::Module>> take_unit_modules() {
        return std::move(unit_modules_);
    }

    void run() {
        build_state_types();
        emit_state_global(); // before any body: storage_ptr resolves through the anchors
        if (std::getenv("CLOTHO_DEDUP_STAT") != nullptr) {
            report_dedup_stat();
        }
        compute_subscription_order();
        compute_fusion_groups(); // before build_wait_queues: bucket capacities count group subscriptions
        if (std::getenv("CLOTHO_COMB_FUSE_STAT") != nullptr) {
            report_comb_fuse_stat();
        }
        compute_domain_spec();   // after fusion (thunk_group_): reads spec_group_ in lower_group
        build_vcd_index();       // before build_nba_state: fills vcd_index_ / design_uses_vcd_
        build_wait_queues();
        build_instance_ctx_layout(); // after build_wait_queues: ctx sizes need wait slots
        build_delay_event_thunks(); // after build_wait_queues: thunks read the event globals
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
        std::vector<llvm::Function*> group_fns;
        for (uint32_t g = 0; g < groups_.size(); ++g) {
            const uint32_t ua = group_unit_anchor_[g];
            env_ = config_.split && ua != SignalSlot::no_anchor ? &unit_env(anchors_[ua])
                                                                : root_env_.get();
            group_fns.push_back(lower_group(groups_[g]));
        }
        env_ = root_env_.get();
        if (!shape_sets_.empty()) {
            report_dedup_shapes(group_fns);
        }
        if (config_.dedup && config_.thunk) {
            share_twin_groups(group_fns);
        }
        // Blocks are discovered during the lowering above; size the block-commit
        // globals and emit the commit / enqueue functions now, before emit_main
        // wires the NBA-region drain that references them.
        finalize_block_nba();
        emit_main(group_fns);
        // Export thunks come last: they reference @clotho.state (created in
        // emit_main) and every SV function's declaration.
        emit_dpi_exports();
    }

    // Per-process "scheduled" flags, one i32 per process: non-zero while the
    // process is already queued to run this instant. Persistent (always-block)
    // wait entries use it to avoid being woken more than once per instant when
    // several of their sensitivity signals change together. Flags live in one
    // array per owning instance scope (@clotho.inst.<hier>.sched, indexed by a
    // dense def-local ordinal — twin instances lower identical ordinals) plus
    // a root @clotho.proc_sched for unowned (PreInit / package) processes.
    void build_sched_flags() {
        const auto procs = mod_.processes();
        auto*      i32   = llvm::Type::getInt32Ty(ctx_);
        proc_sched_anchor_.assign(std::max<size_t>(procs.size(), 1), ~0u);
        proc_sched_ord_.assign(std::max<size_t>(procs.size(), 1), 0);
        std::vector<uint32_t> owned(anchors_.size(), 0);
        uint32_t              n_root = 0;
        for (uint32_t k = 0; k < procs.size(); ++k) {
            const uint32_t sc = procs[k].scope;
            if (sc != Process::no_scope && sc < anchor_of_scope_.size() &&
                anchor_of_scope_[sc] != SignalSlot::no_anchor) {
                const uint32_t aid    = anchor_of_scope_[sc];
                proc_sched_anchor_[k] = aid;
                proc_sched_ord_[k]    = owned[aid]++;
            } else {
                proc_sched_ord_[k] = n_root++;
            }
        }
        for (uint32_t a = 0; a < anchors_.size(); ++a) {
            if (owned[a] == 0) {
                continue;
            }
            Anchor& an  = anchors_[a];
            an.sched_ty = llvm::ArrayType::get(i32, owned[a]);
            an.sched_gv = new llvm::GlobalVariable(env_->llmod, an.sched_ty, false,
                                                   llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantAggregateZero::get(an.sched_ty),
                                                   "clotho.inst." + an.name + ".sched");
        }
        sched_arr_ty_ = llvm::ArrayType::get(i32, std::max<uint32_t>(n_root, 1));
        sched_gv_     = new llvm::GlobalVariable(env_->llmod, sched_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                                 llvm::ConstantAggregateZero::get(sched_arr_ty_), "clotho.proc_sched");
    }

    // Constant pointer to process k's scheduled flag in its owner's array.
    llvm::Constant* sched_flag_ptr(uint32_t k) {
        const uint32_t aid  = proc_sched_anchor_[k];
        auto*          base = aid != ~0u ? sym(anchors_[aid].sched_gv) : sym(sched_gv_);
        auto*          ty   = aid != ~0u ? anchors_[aid].sched_ty : sched_arr_ty_;
        return llvm::ConstantExpr::getInBoundsGetElementPtr(
          ty, base, llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(proc_sched_ord_[k]) });
    }

private:
    // -- Cross-unit symbol references --

    // Use a global (defined in whichever unit created it) as an operand in the
    // module currently being emitted. Identity while the definition is local —
    // always the case with one design-wide unit — otherwise a matching external
    // declaration created once per (env, symbol) and memoized in env_->decls.
    // Every use of a cached GlobalVariable*/Function* member as an IR operand
    // must route through here so the per-definition split can relink it.
    llvm::Function* sym(llvm::Function* f) {
        if (f->getParent() == &env_->llmod) {
            return f;
        }
        auto [it, inserted] = env_->decls.try_emplace(f, nullptr);
        if (inserted) {
            // First cross-unit use: the definition must be visible to the JIT
            // linker, so promote it out of local linkage.
            if (f->hasLocalLinkage()) {
                f->setLinkage(llvm::GlobalValue::ExternalLinkage);
            }
            it->second = llvm::Function::Create(f->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
                                                f->getName(), env_->llmod);
        }
        return llvm::cast<llvm::Function>(it->second);
    }
    llvm::GlobalVariable* sym(llvm::GlobalVariable* g) {
        if (g->getParent() == &env_->llmod) {
            return g;
        }
        auto [it, inserted] = env_->decls.try_emplace(g, nullptr);
        if (inserted) {
            if (g->hasLocalLinkage()) {
                g->setLinkage(llvm::GlobalValue::ExternalLinkage);
            }
            auto* decl = new llvm::GlobalVariable(env_->llmod, g->getValueType(), g->isConstant(),
                                                  llvm::GlobalValue::ExternalLinkage, nullptr, g->getName());
            decl->setAlignment(g->getAlign());
            it->second = decl;
        }
        return llvm::cast<llvm::GlobalVariable>(it->second);
    }

    // The definition unit's env for an anchored scope, created (with its
    // llvm::Module) on first use. Units are keyed by definition state-struct
    // identity: twin instances of one definition land in one unit.
    ModuleEnv& unit_env(const Anchor& a) {
        auto [it, inserted] = unit_env_of_.try_emplace(a.ty, nullptr);
        if (inserted) {
            std::string name = a.ty->getName().str();
            if (name.ends_with(".state")) {
                name.resize(name.size() - 6);
            }
            auto m = std::make_unique<llvm::Module>("clotho.def." + name, ctx_);
            m->setTargetTriple(root_env_->llmod.getTargetTriple());
            m->setDataLayout(root_env_->llmod.getDataLayout());
            unit_envs_.push_back(std::make_unique<ModuleEnv>(*m));
            unit_modules_.push_back(std::move(m));
            it->second = unit_envs_.back().get();
        }
        return *it->second;
    }

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
    // The all-X packed i(2W) must|may word: every bit X (must 0, may 1), i.e.
    // the high W bits set. Also the must|may half of all-Z.
    llvm::ConstantInt* x_mm(uint32_t width) const {
        return llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * width, width));
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
    // `narrow` requests the genuine single-iW 2-state slot (`{ iW }`, named
    // `%bit.W`) instead of the packed must|may word — used for a 2-state signal
    // proven decoupled from every path that still reads the packed form (ABI, NBA,
    // arrays). Only valid for a 2-state (four_state == false), never-z slot.
    llvm::StructType* slot_type(uint32_t width, bool four_state, bool has_z, bool narrow = false) {
        for (const auto& [w, four, z, nr, ty] : slot_types_) {
            if (w == width && four == four_state && z == has_z && nr == narrow) {
                return ty;
            }
        }
        const std::string name = std::string(four_state ? "logic." : "bit.")
                               + (narrow ? "" : (has_z ? "" : "nz.")) + std::to_string(width);
        std::vector<llvm::Type*> fields{ int_ty(narrow ? width : 2 * width) };
        if (has_z) {
            fields.push_back(int_ty(width));
        }
        auto* ty = llvm::StructType::create(ctx_, fields, name);
        slot_types_.emplace_back(width, four_state, has_z, narrow, ty);
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

    // The opaque rt_dynarr handle slot type (`%clotho.dynarr = { ptr, i64 }`),
    // memoized. Must mirror the runtime rt_dynarr layout ({ void* data, u64 len }).
    llvm::StructType* dynarr_slot_ty() {
        if (dynarr_slot_ty_ == nullptr) {
            dynarr_slot_ty_ =
              llvm::StructType::create(ctx_, { raw_ptr_ty(), b_.getInt64Ty() }, "clotho.dynarr");
        }
        return dynarr_slot_ty_;
    }

    // Whether a type is a dynamic array (LRM 7.5).
    bool is_dynarr_type(Ref type) const {
        return std::get<Type>(mod_.pool.get(type)).kind == Type::Kind::DynArray;
    }

    // Per-element slot facts for a dynamic array's element (a POD scalar in phase
    // 1). Mirrors the scalar-signal slot choice: 4-state keeps its z plane
    // (`{ i2W, iW }`); 2-state and real take the narrow single-iW form.
    struct DynElem {
        llvm::StructType* slot_ty    = nullptr;
        uint32_t          width      = 0;
        bool              four_state = false;
        bool              has_z      = false;
        bool              narrow     = false;
        // Managed element shapes: a string handle slot, or a nested container
        // (dynamic array / queue) handle slot. Width / domain fields are
        // meaningless for these.
        bool is_string = false;
        bool is_handle = false;
        Ref  elem_type{};
    };
    DynElem dynarr_elem(Ref array_type) {
        const Type& at = std::get<Type>(mod_.pool.get(array_type));
        DynElem     e;
        e.elem_type    = at.element;
        const Type& et = std::get<Type>(mod_.pool.get(at.element));
        if (et.kind == Type::Kind::String) {
            e.is_string = true;
            e.slot_ty   = str_slot_ty();
            return e;
        }
        if (et.kind == Type::Kind::DynArray || et.kind == Type::Kind::Queue) {
            e.is_handle = true;
            e.slot_ty   = handle_slot_ty(at.element);
            return e;
        }
        const Layout lay = mod_.pool.layout(at.element);
        e.width      = lay.width == 0 ? 1 : lay.width;
        e.four_state = lay.domain == Domain::Four;
        e.has_z      = e.four_state;
        e.narrow     = !e.four_state; // 2-state / real element -> single iW slot
        e.slot_ty    = slot_type(e.width, e.four_state, e.has_z, e.narrow);
        return e;
    }

    // Byte size of one element slot (the runtime stride).
    uint64_t dynarr_stride(const DynElem& e) {
        return env_->llmod.getDataLayout().getTypeAllocSize(e.slot_ty);
    }

    // The rt_elem_ops mirror type:
    // { i32 kind, i32 nleaves, i64 stride, ptr proto, ptr inner, ptr leaves }.
    // Must match the runtime struct exactly.
    llvm::StructType* elem_ops_ty() {
        if (elem_ops_ty_ == nullptr) {
            elem_ops_ty_ = llvm::StructType::create(
              ctx_,
              { b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt64Ty(), raw_ptr_ty(), raw_ptr_ty(),
                raw_ptr_ty() },
              "clotho.elemops");
        }
        return elem_ops_ty_;
    }

    // The element power-on byte image as a typed constant: X planes for a
    // 4-state scalar, zero for 2-state / real, the empty inline handle for a
    // string, the null handle for a nested container.
    llvm::Constant* elem_proto_const(const DynElem& e) {
        if (e.is_string) {
            std::vector<uint8_t> bytes(sizeof(rt_string), 0);
            bytes[sizeof(rt_string) - 1] = static_cast<uint8_t>(rt_string_inline_cap);
            return llvm::ConstantStruct::get(
              e.slot_ty, { llvm::ConstantDataArray::get(ctx_, bytes) });
        }
        if (e.is_handle) {
            return llvm::Constant::getNullValue(e.slot_ty);
        }
        llvm::Constant*              mm = e.narrow ? zero(e.width) : x_mm(e.width);
        std::vector<llvm::Constant*> fields{ mm };
        if (e.has_z) {
            fields.push_back(zero(e.width));
        }
        return llvm::ConstantStruct::get(e.slot_ty, fields);
    }

    // The constant rt_elem_ops descriptor for a container's element type,
    // memoized per element type Ref. Nested containers chain through `inner`
    // (finite: type nesting is a DAG). Aggregate elements are frontend-guarded,
    // so no leaf table is ever emitted yet.
    llvm::Constant* elem_ops_global(Ref container_type) {
        const Type& at = std::get<Type>(mod_.pool.get(container_type));
        if (const auto it = env_->elem_ops.find(static_cast<uint32_t>(at.element));
            it != env_->elem_ops.end()) {
            return it->second;
        }
        const DynElem e     = dynarr_elem(container_type);
        uint32_t      kind  = 0;
        llvm::Constant* inner = llvm::Constant::getNullValue(raw_ptr_ty());
        if (e.is_string) {
            kind = 1;
        } else if (e.is_handle) {
            const Type& et = std::get<Type>(mod_.pool.get(at.element));
            kind  = et.kind == Type::Kind::DynArray ? 2 : 3;
            inner = elem_ops_global(at.element);
        }
        auto* proto = new llvm::GlobalVariable(env_->llmod, e.slot_ty, /*isConstant=*/true,
                                               llvm::GlobalValue::InternalLinkage,
                                               elem_proto_const(e), "clotho.elem.proto");
        auto* init = llvm::ConstantStruct::get(
          elem_ops_ty(),
          { b_.getInt32(kind), b_.getInt32(0), b_.getInt64(dynarr_stride(e)), proto, inner,
            llvm::Constant::getNullValue(raw_ptr_ty()) });
        auto* gv = new llvm::GlobalVariable(env_->llmod, elem_ops_ty(), /*isConstant=*/true,
                                            llvm::GlobalValue::InternalLinkage, init,
                                            "clotho.elem.ops");
        env_->elem_ops.emplace(static_cast<uint32_t>(at.element), gv);
        return gv;
    }

    // Sort comparator descriptor for the element of `array_type` (LRM 7.12.2
    // sort()/rsort()). `kind`: 0 = unsigned integral, 1 = signed integral,
    // 2 = real. Derived from the element value type; the frontend has already
    // restricted the element to a 2-state integral (width <= 64) or real.
    struct SortDesc {
        uint32_t kind;
        uint32_t width;
    };
    SortDesc dynarr_sort_desc(Ref array_type) const {
        const Ref     elem = std::get<Type>(mod_.pool.get(array_type)).element;
        const ValType vt   = from_ref(elem);
        return { vt.is_real ? 2u : (vt.is_signed ? 1u : 0u), vt.width };
    }

    // The opaque rt_queue ring handle slot type
    // (`%clotho.queue = { ptr, i64, i64, i64 }`), memoized. Must mirror the
    // runtime rt_queue layout ({ void* base, u64 head, u64 len, u64 cap }).
    llvm::StructType* queue_slot_ty() {
        if (queue_slot_ty_ == nullptr) {
            queue_slot_ty_ = llvm::StructType::create(
              ctx_, { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty() },
              "clotho.queue");
        }
        return queue_slot_ty_;
    }

    // Whether a type is a queue (LRM 7.10).
    bool is_queue_type(Ref type) const {
        return std::get<Type>(mod_.pool.get(type)).kind == Type::Kind::Queue;
    }

    // A heap-backed handle value type (dynamic array or queue): both share the
    // by-pointer, deep-copy subroutine ABI, differing only in the runtime slot
    // struct and the copy / free functions.
    bool is_handle_type(Ref type) const {
        return is_dynarr_type(type) || is_queue_type(type);
    }
    llvm::StructType* handle_slot_ty(Ref type) {
        return is_queue_type(type) ? queue_slot_ty() : dynarr_slot_ty();
    }
    // Deep-copy one handle into another (value semantics), picking the runtime
    // function and element descriptor from the type.
    void emit_handle_assign(llvm::Value* dst, llvm::Value* src, Ref type) {
        b_.CreateCall(is_queue_type(type) ? queue_assign_fn() : dynarr_assign_fn(),
                      { dst, src, elem_ops_global(type) });
    }
    // Destroy a handle's elements and free its storage (reset to empty).
    void emit_handle_delete(llvm::Value* handle, Ref type) {
        b_.CreateCall(is_queue_type(type) ? queue_delete_all_fn() : dynarr_delete_fn(),
                      { handle, elem_ops_global(type) });
    }
    // Pointer to the rt handle slot an lvalue designates: the resolved
    // aggregate leaf slot for a struct-member / class-property / array-element
    // handle, or the bare base for a whole signal / frame local.
    llvm::Value* lv_handle_ptr(const LValue& lv) {
        return lv.is_agg ? agg_slot_ptr(lv) : lv.base;
    }

    // Whether a type is a string, or an unpacked array whose leaf element is.
    bool is_string_type(Ref type) const {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            return is_string_type(t.element);
        }
        return t.kind == Type::Kind::String;
    }

    // The scalar leaf type of a (possibly nested) unpacked-array type.
    Ref leaf_type(Ref type) const {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            return leaf_type(t.element);
        }
        return type;
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

    // Whether a type contains an unpacked struct anywhere (at any array/struct
    // nesting depth). Such a signal is routed through the recursive aggregate
    // storage / addressing / init path, not the flat scalar-or-array fast paths.
    bool contains_struct(Ref type) const {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            return contains_struct(t.element);
        }
        return t.kind == Type::Kind::Struct && !t.is_packed;
    }

    // Whether a type takes the recursive aggregate storage mirror: it contains
    // an unpacked struct, or a fixed array dimension over a heap-handle
    // (dynamic array / queue) element whose per-cell init/deinit/copy only the
    // recursive walks provide. Flat string arrays keep the dedicated
    // string-array path (is_string_type is tested first at both call sites).
    bool needs_agg_storage(Ref type) const {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            const Type::Kind ek = std::get<Type>(mod_.pool.get(t.element)).kind;
            if (ek == Type::Kind::DynArray || ek == Type::Kind::Queue) {
                return true;
            }
            return needs_agg_storage(t.element);
        }
        return t.kind == Type::Kind::Struct && !t.is_packed;
    }

    // Whether a type owns heap anywhere (string / dynamic array / queue leaf
    // at any nesting depth), so its aggregate storage needs a deinit walk.
    bool has_heap_leaf(Ref type) const {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        switch (t.kind) {
            case Type::Kind::String:
            case Type::Kind::DynArray:
            case Type::Kind::Queue:
                return true;
            case Type::Kind::UnpackedArray:
                return has_heap_leaf(t.element);
            case Type::Kind::Struct: {
                if (t.is_packed) {
                    return false;
                }
                for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
                    if (has_heap_leaf(f.type)) {
                        return true;
                    }
                }
                return false;
            }
            default:
                return false;
        }
    }

    // Element count of one unpacked-array dimension.
    uint32_t array_dim(const Type& t) const {
        return (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
    }

    // Leaf slot type for a scalar / real aggregate leaf. A 4-state leaf carries a
    // z plane conservatively (there is no per-leaf may-z analysis under an
    // aggregate), so z assigned to a member is representable; a 2-state / real
    // leaf is the narrow single-iW form.
    llvm::StructType* agg_leaf_slot_ty(Ref type) {
        const Layout   lay  = mod_.pool.layout(type);
        const uint32_t w    = lay.width == 0 ? 1 : lay.width;
        const bool     four = lay.domain == Domain::Four;
        return slot_type(w, four, /*has_z=*/four, /*narrow=*/!four);
    }

    // Recursive LLVM storage type mirroring an (aggregate) IR type: a nested
    // StructType per unpacked struct, one ArrayType per unpacked-array dimension,
    // and a leaf slot for each scalar / string / real. Unlike the flat memory
    // path this does NOT flatten array dimensions, so arbitrary nesting
    // (array-of-struct, struct-with-array-member, ...) is a direct mirror.
    llvm::Type* agg_storage_ty(Ref type) {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            return llvm::ArrayType::get(agg_storage_ty(t.element), array_dim(t));
        }
        if (t.kind == Type::Kind::Struct && !t.is_packed) {
            std::vector<llvm::Type*> fields;
            for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
                fields.push_back(agg_storage_ty(f.type));
            }
            return llvm::StructType::create(ctx_, fields, "clotho.agg");
        }
        if (t.kind == Type::Kind::String) {
            return str_slot_ty();
        }
        if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue) {
            return handle_slot_ty(type);
        }
        return agg_leaf_slot_ty(type);
    }

    // The heap object-body LLVM type of a class (LRM 8.3): a StructType mirroring
    // the property table, one field per property (`agg_storage_ty` of its type — a
    // class-handle property is a leaf i64 slot, not a nested body, which is what
    // makes a self-referential class resolve without a fixpoint). Cached so the
    // body pointer's GEP source type (`HandleDeref` root_ty) and the MemberSelect
    // field GEPs share one identity. Distinct from `agg_storage_ty(classRef)`,
    // which yields the *handle* leaf slot (a class value is a handle, not a body).
    llvm::StructType* class_body_ty(Ref class_ref) {
        const auto key = static_cast<uint32_t>(class_ref);
        if (const auto it = class_body_ty_.find(key); it != class_body_ty_.end()) {
            return it->second;
        }
        const Type&              t = std::get<Type>(mod_.pool.get(class_ref));
        std::vector<llvm::Type*> fields;
        for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
            fields.push_back(agg_storage_ty(f.type));
        }
        auto* st              = llvm::StructType::create(ctx_, fields, "clotho.class.body");
        class_body_ty_[key]   = st;
        return st;
    }

    // Power-on the properties of a freshly-allocated class object body (LRM 8.8):
    // the caller has already zeroed the block (2-state leaves and class-handle
    // properties power on 0 / null), so this writes X into 4-state leaves and
    // initializes string leaves, per-property, reusing emit_agg_init.
    void emit_class_body_init(Ref class_ref, llvm::Value* body_ptr) {
        const Type&               t       = std::get<Type>(mod_.pool.get(class_ref));
        llvm::Type*               root_ty = class_body_ty(class_ref);
        std::vector<llvm::Value*> path;
        uint32_t                  f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            path.push_back(b_.getInt32(f++));
            emit_agg_init(fld.type, body_ptr, path, root_ty);
            path.pop_back();
        }
    }

    // memcpy a whole aggregate from `src` to `dst` (both pointers to its storage).
    // Valid because the frontend passes an aggregate by value only when it is
    // trivially copyable (no heap-owning leaf), so its byte image is the value.
    void emit_agg_storage_copy(llvm::Value* dst, llvm::Value* src, Ref type) {
        llvm::Type* st = agg_storage_ty(type);
        const auto  sz = env_->llmod.getDataLayout().getTypeAllocSize(st);
        const auto  al = env_->llmod.getDataLayout().getABITypeAlign(st);
        b_.CreateMemCpy(dst, al, src, al, sz);
    }

    // Fill an LValue's leaf metadata (slot type, width, domain flags) from the
    // scalar / string / handle leaf type a select chain has resolved to.
    // Conservative z for a 4-state leaf, matching agg_leaf_slot_ty.
    void fill_agg_leaf(LValue& lv, Ref leaf_type) {
        const Type& t = std::get<Type>(mod_.pool.get(leaf_type));
        if (t.kind == Type::Kind::String) {
            lv.is_string = true;
            lv.slot_ty   = str_slot_ty();
            return;
        }
        if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue) {
            // A handle leaf is consumed only by the queue / dynamic-array ops
            // (which address it via lv_handle_ptr) and by ElemSelect descent;
            // a plane Load/Store on it is a lowering bug and traps.
            lv.is_handle = true;
            lv.slot_ty   = handle_slot_ty(leaf_type);
            return;
        }
        const Layout   lay  = mod_.pool.layout(leaf_type);
        const uint32_t w    = lay.width == 0 ? 1 : lay.width;
        const bool     four = lay.domain == Domain::Four;
        lv.store_width = w;
        lv.width       = w;
        lv.has_z       = four;
        lv.narrow      = !four;
        lv.slot_ty     = slot_type(w, four, four, !four);
        lv.bit_off     = b_.getInt32(0);
    }

    // Enumerate an aggregate's scalar/string leaves reachable by a STATIC path
    // (struct fields only). Recursion stops at an unpacked-array dimension — a
    // leaf under an array has a runtime element index, so it is not static and
    // takes the deferred nonblocking path, not the block-commit shadow. `path`
    // accumulates the constant GEP field indices from the aggregate root.
    void collect_static_leaves(Ref type, std::vector<uint32_t>& path, std::vector<AggLeaf>& out) {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            return;
        }
        // A handle leaf (dynamic array / queue) can never be nonblocking-staged
        // (guarded in the frontend), so it gets no shadow entry.
        if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue) {
            return;
        }
        if (t.kind == Type::Kind::Struct && !t.is_packed) {
            uint32_t f = 0;
            for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
                path.push_back(f++);
                collect_static_leaves(fld.type, path, out);
                path.pop_back();
            }
            return;
        }
        AggLeaf leaf;
        leaf.path = path;
        if (t.kind == Type::Kind::String) {
            leaf.is_string = true;
            leaf.slot_ty   = str_slot_ty();
        } else {
            const Layout lay = mod_.pool.layout(type);
            leaf.width       = lay.width == 0 ? 1 : lay.width;
            leaf.four_state  = lay.domain == Domain::Four;
            leaf.has_z       = leaf.four_state;
            leaf.narrow      = !leaf.four_state;
            leaf.slot_ty     = slot_type(leaf.width, leaf.four_state, leaf.four_state, !leaf.four_state);
        }
        out.push_back(leaf);
    }

    // Whether an IR type is a scalar / string / real leaf (as opposed to an
    // unpacked struct or array that a select must descend further).
    bool is_leaf_type(Ref type) const {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        return t.kind != Type::Kind::UnpackedArray && !(t.kind == Type::Kind::Struct && !t.is_packed);
    }

    // Derive member `field_index`'s lvalue from an aggregate lvalue: push the
    // constant field index onto the aggregate GEP path and resolve the leaf
    // metadata if the member is a scalar/string (a struct/array member stays
    // interior for a further select). Shared by MemberSelect and the AggEq
    // member walk.
    LValue agg_member(LValue lv, Ref struct_type, uint32_t field_index) {
        const Type& st = std::get<Type>(mod_.pool.get(struct_type));
        const Ref   mt = mod_.pool.struct_fields(st.fields_id)[field_index].type;
        lv.agg_path.push_back(b_.getInt32(field_index));
        lv.agg_field_path.push_back(field_index);
        lv.agg_leaf = is_leaf_type(mt);
        if (lv.agg_leaf) {
            fill_agg_leaf(lv, mt);
        }
        return lv;
    }

    // Build one instance scope's state struct: its own signals' slots (field k
    // == scope signal k). Children are NOT nested as fields — each scope owns
    // its own state global, so a child scope recurses into its own anchor and
    // struct. Assigns every covered signal's scope-relative GEP path ({k}).
    // Struct types are deduplicated per module definition — instances of one
    // definition (same parameterization) produce identical field lists and
    // share one identified type. Each call is one scope occurrence and records
    // one Anchor; storage_ptr addresses signals anchor-relative.
    void scope_type(const InstanceScope& scope, std::vector<bool>& claimed, const std::string& hier) {
        const auto aid = static_cast<uint32_t>(anchors_.size());
        anchors_.push_back(Anchor{ .name = hier });
        if (scope.id >= anchor_of_scope_.size()) {
            anchor_of_scope_.resize(scope.id + 1, SignalSlot::no_anchor);
        }
        anchor_of_scope_[scope.id] = aid;
        std::vector<llvm::Type*> fields;
        for (uint32_t k = 0; k < scope.signals.size(); ++k) {
            const auto si = static_cast<uint32_t>(scope.signals[k]);
            if (si >= slots_.size() || claimed[si]) {
                throw BackendError{ "malformed instance hierarchy: signal claimed twice" };
            }
            claimed[si] = true;
            slots_[si].path   = { k };
            slots_[si].anchor = aid;
            fields.push_back(slots_[si].storage_ty);
        }
        for (const InstanceScope& child : scope.children) {
            scope_type(child, claimed, hier + "." + child.name);
        }

        for (const auto& [def, flds, ty] : def_types_) {
            if (def == scope.def_name && flds == fields) {
                anchors_[aid].ty = ty;
                return;
            }
        }
        auto* ty = llvm::StructType::create(ctx_, fields, scope.def_name + ".state");
        def_types_.emplace_back(scope.def_name, fields, ty);
        anchors_[aid].ty = ty;
    }

    // Build the design's state types: per-signal slot / storage types, one
    // struct per module definition (own signals only — every instance scope
    // owns a separate state global, children own theirs), and the root
    // `%clotho.state` = { <unscoped signals...> }. Unscoped signals only occur
    // for a module built without hierarchy info (they form one flat root
    // scope). The NBA shadow region is a separate root-level global, not part
    // of any definition struct, because staging is a backend artifact of which
    // signals the design nonblocking-assigns, not part of a module's declared
    // state.
    void build_state_types() {
        const auto sigs = mod_.signals();
        slots_.resize(sigs.size());
        // Classify NBA targets up front so the slot loop can keep an NBA-target
        // 2-state signal on the packed word (its shadow / commit still expect it).
        nba_classify();

        // A slot keeps its z word when the may-z analysis cannot prove it z-free
        // or the task ABI needs z storage behind it. Elision is analysis-driven,
        // not domain-driven: a 2-state signal assigned a z-carrying expression
        // round-trips the z bit today (Resize does not clear z across domains),
        // and that behavior is preserved — ordinary 2-state signals are never-z
        // through the analysis anyway.
        const std::vector<bool> may_z =
          config_.z_elision ? compute_may_z(mod_) : std::vector<bool>(sigs.size(), true);
        const std::vector<bool> ptr_arg = ptr_arg_signals();
        // Semantics, not optimization: computed regardless of config_.z_elision.
        // An undriven 4-state net powers up z (LRM 6.6); everything else x / 0.
        const std::vector<bool> undriven = compute_undriven(mod_);

        for (uint32_t i = 0; i < sigs.size(); ++i) {
            const Layout lay = mod_.pool.layout(sigs[i].type);
            const Type&  ty  = std::get<Type>(mod_.pool.get(sigs[i].type));

            SignalSlot& slot = slots_[i];
            slot.ir_type     = sigs[i].type;
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
            // A signal whose type contains an unpacked struct (at any nesting),
            // or a fixed array over a queue / dynamic-array element, is an
            // aggregate: a recursive storage mirror (nested StructType /
            // ArrayType / leaf slots), addressed by a composed GEP path. Its
            // scalar/string leaves reachable by a static (struct-field-only) path
            // are enumerated for nonblocking staging.
            if (needs_agg_storage(sigs[i].type)) {
                slot.is_agg     = true;
                slot.has_z      = false;
                slot.storage_ty = agg_storage_ty(sigs[i].type);
                std::vector<uint32_t> path;
                collect_static_leaves(sigs[i].type, path, slot.static_leaves);
                continue;
            }
            // A dynamic-array slot is an opaque rt_dynarr handle ({ data, len }):
            // no planes, no z word; elements live in its heap buffer.
            if (ty.kind == Type::Kind::DynArray) {
                slot.is_dynarr  = true;
                slot.has_z      = false;
                slot.slot_ty    = dynarr_slot_ty();
                slot.storage_ty = slot.slot_ty;
                continue;
            }
            // A queue slot is an opaque rt_queue ring handle ({ base, head, len,
            // cap }): no planes, no z word; elements live in its heap ring buffer.
            if (ty.kind == Type::Kind::Queue) {
                slot.is_queue   = true;
                slot.has_z      = false;
                slot.slot_ty    = queue_slot_ty();
                slot.storage_ty = slot.slot_ty;
                continue;
            }
            // A real / shortreal slot stores the IEEE-754 bit pattern in a
            // 2-state word: never z, so it takes the narrow single-iW form
            // below (pool layout gives Domain::Two for floats).
            slot.is_real     = mod_.pool.is_float(leaf_type(sigs[i].type));
            slot.init_z      = undriven[i] && sigs[i].is_net && slot.four_state;
            slot.has_z       = may_z[i] || ptr_arg[i] || slot.init_z;
            // A 2-state signal drops the packed must|may word for a genuine iW slot
            // (or `[N x { iW }]` memory). The only requirement is never-z: a 2-state
            // value carries no z, and has_z is forced only by a task out/inout target
            // (the packed pointer ABI) or a may-z analysis result — both of which
            // keep the packed word. Every other 2-state path (scalar/array NBA
            // shadow + commit, $readmem via rt_readmem) is width-driven off `narrow`.
            slot.narrow = !slot.four_state && !slot.has_z;
            slot.slot_ty     = slot_type(slot.width, slot.four_state, slot.has_z, slot.narrow);
            if (ty.kind == Type::Kind::UnpackedArray) {
                slot.is_array   = true;
                slot.elem_count = array_leaf_count(sigs[i].type);
                slot.storage_ty = llvm::ArrayType::get(slot.slot_ty, slot.elem_count);
            } else {
                slot.storage_ty = slot.slot_ty;
            }
        }

        std::vector<llvm::Type*> root_fields;
        std::vector<bool>        claimed(sigs.size(), false);
        for (const InstanceScope& top : mod_.tops()) {
            scope_type(top, claimed, top.name);
        }
        // A definition state type produced by more than one scope occurrence is
        // a reused module: those instances address indirectly (see Anchor).
        for (Anchor& a : anchors_) {
            uint32_t uses = 0;
            for (const Anchor& b : anchors_) {
                uses += b.ty == a.ty ? 1u : 0u;
            }
            a.indirect = uses > 1;
        }
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            if (!claimed[i]) {
                slots_[i].path = { static_cast<uint32_t>(root_fields.size()) };
                root_fields.push_back(slots_[i].storage_ty);
            }
        }

        // Shadow staging fields for NBA targets. A shadow mirrors the live
        // i(2W) / iW planes; a partial target adds a dirty mask. Fields are
        // grouped into one sub-struct per instance scope (per-definition type,
        // deduplicated like the state structs), then the unscoped signals'
        // fields flat behind them: an anchored signal's sh_* indices are
        // def-local, so twin instances stage through identical constants
        // (shadow_ptr resolves the instance base). Root shadow layout is
        // otherwise as before — one struct behind the opaque shadow base.
        if (n_nba_ > 0) {
            const auto add_fields = [&](SignalSlot& slot, std::vector<llvm::Type*>& sf) {
                // An aggregate target shadows each static leaf's mm (and z, for a
                // 4-state leaf) plane; the commit selects leaves by the block's
                // static-leaf-set mask. A runtime-path-only aggregate (deferred
                // records) needs no block shadow at all.
                if (slot.is_agg) {
                    if (slot.nba_agg) {
                        for (AggLeaf& m : slot.static_leaves) {
                            m.sh_mm_field = static_cast<uint32_t>(sf.size());
                            sf.push_back(int_ty(m.narrow ? m.width : 2 * m.width));
                            if (m.has_z) {
                                m.sh_z_field = static_cast<uint32_t>(sf.size());
                                sf.push_back(int_ty(m.width));
                            }
                        }
                    }
                    return;
                }
                slot.sh_mm_field = static_cast<uint32_t>(sf.size());
                // A narrow 2-state target shadows a single iW word (no z shadow).
                sf.push_back(int_ty(slot.narrow ? slot.width : 2 * slot.width));
                // A never-z target stages no z shadow; sh_z_field stays unused.
                if (slot.has_z) {
                    slot.sh_z_field = static_cast<uint32_t>(sf.size());
                    sf.push_back(int_ty(slot.width));
                }
                // Only a dynamic-select target needs a runtime dirty-mask field; a
                // static-select target's mask is a compile-time immediate in its
                // block commit.
                if (slot.nba_dyn) {
                    slot.sh_mask_field = static_cast<uint32_t>(sf.size());
                    sf.push_back(int_ty(slot.width));
                }
            };

            std::vector<llvm::Type*> sf;
            std::vector<std::tuple<llvm::StructType*, std::vector<llvm::Type*>, llvm::StructType*>>
              shadow_defs;
            for (uint32_t a = 0; a < anchors_.size(); ++a) {
                std::vector<llvm::Type*> af;
                for (uint32_t i = 0; i < sigs.size(); ++i) {
                    if (slots_[i].anchor == a && slots_[i].nba) {
                        add_fields(slots_[i], af);
                    }
                }
                if (af.empty()) {
                    continue;
                }
                Anchor& an = anchors_[a];
                for (const auto& [ty, flds, st] : shadow_defs) {
                    if (ty == an.ty && flds == af) {
                        an.shadow_ty = st;
                        break;
                    }
                }
                if (an.shadow_ty == nullptr) {
                    an.shadow_ty = llvm::StructType::create(ctx_, af, an.ty->getName().str() + ".shadow");
                    shadow_defs.emplace_back(an.ty, std::move(af), an.shadow_ty);
                }
            }
            for (uint32_t i = 0; i < sigs.size(); ++i) {
                if (slots_[i].anchor == SignalSlot::no_anchor && slots_[i].nba) {
                    add_fields(slots_[i], sf);
                }
            }
            // Anchored scopes' staging lives in per-instance shadow globals
            // (Anchor::shadow_gv); the root @clotho.nba.shadow holds only the
            // unscoped signals' fields, and is skipped entirely when every
            // NBA target is scope-owned.
            if (!sf.empty()) {
                shadow_ty_ = llvm::StructType::create(ctx_, sf, "clotho.nba.shadow");
            }
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
        const LvalueRoot r = lvalue_root(ir, lv);
        if (ir.tag(r.root) != Inst::Tag::Signal) {
            throw BackendError{ "unsupported nonblocking-assignment lvalue" };
        }
        return { ir.payload(r.root).signal, r.whole };
    }

    // Whether an lvalue select chain contains a runtime-indexed select (BitSelect /
    // DynPartSelect), i.e. its written-bit mask is not a compile-time constant. A
    // constant-index BitSelect is treated as dynamic too (conservative — the field is
    // allocated but the commit still uses a constant mask when bit_off folds).
    static bool lvalue_has_dynamic_select(const Ir& ir, Index lv) {
        for (;;) {
            switch (ir.tag(lv)) {
                case Inst::Tag::BitSelect:
                    return true;
                case Inst::Tag::DynPartSelect:
                    return true;
                case Inst::Tag::PartSelect:
                case Inst::Tag::ElemSelect:
                case Inst::Tag::MemberSelect:
                    lv = ir.payload(lv).op_extra.op;
                    break;
                default:
                    return false;
            }
        }
    }

    // Whether an lvalue chain contains a bit/part-select (a within-element select
    // for a memory: BitSelect / PartSelect / DynPartSelect), as opposed to only an
    // ElemSelect array index. Determines whether an array target needs a write mask.
    static bool lvalue_has_subselect(const Ir& ir, Index lv) {
        for (;;) {
            switch (ir.tag(lv)) {
                case Inst::Tag::BitSelect:
                    lv = ir.payload(lv).binary.l;
                    return true;
                case Inst::Tag::PartSelect:
                case Inst::Tag::DynPartSelect:
                    return true;
                case Inst::Tag::ElemSelect:
                case Inst::Tag::MemberSelect:
                    lv = ir.payload(lv).op_extra.op;
                    break;
                default:
                    return false;
            }
        }
    }

    // Whether an lvalue chain contains an unpacked-array element select (a
    // runtime element index). For an aggregate NBA target this discriminates a
    // static-path leaf (block-commit shadow) from a runtime one (deferred path).
    static bool lvalue_has_elem_select(const Ir& ir, Index lv) {
        for (;;) {
            switch (ir.tag(lv)) {
                case Inst::Tag::ElemSelect:
                    return true;
                case Inst::Tag::BitSelect:
                    lv = ir.payload(lv).binary.l;
                    break;
                case Inst::Tag::PartSelect:
                case Inst::Tag::DynPartSelect:
                case Inst::Tag::MemberSelect:
                    lv = ir.payload(lv).op_extra.op;
                    break;
                default:
                    return false;
            }
        }
    }

    // Mark NBA-target signals reached by one IR body (a process or a task): set
    // `nba`, `nba_partial` (any bit/part-select site), `nba_dyn` (any runtime-indexed
    // select), and collect unpacked-array targets for the runtime deferred queue.
    void nba_classify_ir(const Ir& ir) {
        const auto sigs = mod_.signals();
        for (size_t k = 0; k < ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            // A nonblocking intra-assignment delay (`x <= #d y`) targets a whole
            // integral scalar (frontend-restricted). Its target is a real NBA
            // target for wake purposes (gets nba_id + meta), but the value is held
            // in a deferred record on the wheel, not the shadow, so it is tracked
            // separately for the intra-commit loop.
            if (ir.tag(idx) == Inst::Tag::StoreNbDelay) {
                const NbaTarget t = nba_target(ir, ir.payload(idx).op_extra.op);
                slots_[static_cast<uint32_t>(t.sig)].nba = true;
                if (std::find(intra_nba_sigs_.begin(), intra_nba_sigs_.end(), t.sig) ==
                    intra_nba_sigs_.end()) {
                    intra_nba_sigs_.push_back(t.sig);
                }
                continue;
            }
            // A nonblocking intra-assignment event (`x <= @e y`): the target is an
            // intra target (deferred record + NBA-region commit), and the trigger
            // (signal, edge) gets a dense id so both wake paths can fire its records.
            if (ir.tag(idx) == Inst::Tag::StoreNbEvent) {
                const auto      info = ir.extra_data<StoreNbEventInfo>(ir.payload(idx).op_extra.extra);
                const NbaTarget t    = nba_target(ir, ir.payload(idx).op_extra.op);
                slots_[static_cast<uint32_t>(t.sig)].nba = true;
                if (std::find(intra_nba_sigs_.begin(), intra_nba_sigs_.end(), t.sig) ==
                    intra_nba_sigs_.end()) {
                    intra_nba_sigs_.push_back(t.sig);
                }
                int32_t& tid = trigger_id_[static_cast<uint32_t>(info.trig)][info.edge];
                if (tid < 0) {
                    tid = static_cast<int32_t>(n_triggers_++);
                }
                continue;
            }
            if (ir.tag(idx) == Inst::Tag::EventTriggerNb) {
                // `->> e` defers its wake to the NBA region; record the event so
                // the region scans it. (Distinct events dedup; the list is tiny.)
                const Signal sig = ir.payload(idx).signal;
                if (std::find(nb_event_sigs_.begin(), nb_event_sigs_.end(), sig) == nb_event_sigs_.end()) {
                    nb_event_sigs_.push_back(sig);
                }
                continue;
            }
            if (ir.tag(idx) == Inst::Tag::EventTriggerNbDelayed) {
                // `->> #d e` schedules a detached wheel timer; record the event so
                // a per-event on-expiry thunk is generated.
                const Signal sig = ir.extra_data<EventDelayInfo>(ir.payload(idx).op_extra.extra).event;
                if (std::find(nb_delay_event_sigs_.begin(), nb_delay_event_sigs_.end(), sig) ==
                    nb_delay_event_sigs_.end()) {
                    nb_delay_event_sigs_.push_back(sig);
                }
                continue;
            }
            if (ir.tag(idx) != Inst::Tag::StoreNb) {
                continue;
            }
            const Index     lv_idx = ir.payload(idx).binary.l;
            const NbaTarget t      = nba_target(ir, lv_idx);
            // Nonblocking assignment into an aggregate. A STATIC-path leaf (struct
            // fields only) commits through the per-leaf aggregate shadow (block
            // commit). A leaf reached through a runtime array index has no
            // compile-time address, so it defers a (dest-slot-pointer, value)
            // record replayed by memcpy — the aggregate analogue of the array
            // deferred queue. A signal can take both paths (different sites).
            if (contains_struct(sigs[static_cast<uint32_t>(t.sig)].type)) {
                SignalSlot& s       = slots_[static_cast<uint32_t>(t.sig)];
                const bool  runtime = lvalue_has_elem_select(ir, lv_idx);
                const bool  partial = lvalue_has_subselect(ir, lv_idx);
                s.nba               = true;
                // A static whole-leaf write commits from the block shadow; a
                // runtime array index OR a partial (bit/part-select) leaf write
                // defers (the partial one needs a masked read-modify-write).
                if (runtime || partial) {
                    if (std::find(agg_nba_sigs_.begin(), agg_nba_sigs_.end(), t.sig) == agg_nba_sigs_.end()) {
                        agg_nba_sigs_.push_back(t.sig);
                    }
                    s.agg_masked = s.agg_masked || partial;
                } else {
                    s.nba_agg = true;
                }
                continue;
            }
            // An unpacked-array element NBA uses the runtime deferred queue, not
            // the per-signal shadow; record the array signal for wake servicing.
            // (slot.is_array is not set until the placement loop below, so test
            // the type directly here.)
            if (std::get<Type>(mod_.pool.get(sigs[static_cast<uint32_t>(t.sig)].type)).kind ==
                Type::Kind::UnpackedArray) {
                if (std::find(array_nba_sigs_.begin(), array_nba_sigs_.end(), t.sig) == array_nba_sigs_.end()) {
                    array_nba_sigs_.push_back(t.sig);
                }
                // A within-element bit/part-select (below the element index) means
                // this array's deferred records must carry a write mask.
                if (lvalue_has_subselect(ir, lv_idx)) {
                    slots_[static_cast<uint32_t>(t.sig)].arr_masked = true;
                }
                continue;
            }
            SignalSlot& s = slots_[static_cast<uint32_t>(t.sig)];
            s.nba         = true;
            s.nba_partial = s.nba_partial || !t.whole;
            s.nba_dyn     = s.nba_dyn || (!t.whole && lvalue_has_dynamic_select(ir, lv_idx));
        }
    }

    // Find every NBA-target signal across all processes and tasks and mark its slot:
    // `nba`, `nba_partial`, and a dense `nba_id` (signal order). A task NBA stages
    // into the same per-signal shadow and commits in its caller's NBA region, so its
    // targets must be classified here too. Bucket assignment happens later in
    // build_nba_state once widths are final.
    void nba_classify() {
        trigger_id_.assign(mod_.signals().size(), { -1, -1, -1 });
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

    // Signals passed by pointer as a non-In (output / inout / ref) actual anywhere
    // in the design, in a function `call` or a `task_call`. The pointer ABI reads /
    // writes the signal's `{ i(2W), iW }` slot (copy-out stores through `&signal.z`;
    // a ref aliases it live), so such a signal must keep its z word and packed form
    // even when proven never-z. A storage/ABI constraint, not a may-z fact, hence
    // separate from compute_may_z. (An alternative — a caller-stack z scratch word
    // for elided targets — is sound but adds a second mechanism for a rare
    // construct.)
    std::vector<bool> ptr_arg_signals() const {
        std::vector<bool> out(mod_.signals().size(), false);
        const auto        mark = [&](const Ir& ir, FunctionId fid, ExtraSlice argslice) {
            const Function& callee = mod_.function(fid);
            const auto      args   = ir.region(argslice);
            for (size_t a = 0; a < args.size(); ++a) {
                if (callee.arg_dirs.empty() || callee.arg_dirs[a] == ArgDir::In) {
                    continue;
                }
                const Index root = lvalue_root(ir, args[a]).root;
                if (ir.tag(root) == Inst::Tag::Signal) {
                    out[static_cast<uint32_t>(ir.payload(root).signal)] = true;
                }
            }
        };
        const auto scan = [&](const Ir& ir) {
            for (size_t k = 0; k < ir.inst_count(); ++k) {
                const auto idx = static_cast<Index>(k);
                if (ir.tag(idx) == Inst::Tag::TaskCall) {
                    const auto ti = ir.extra_data<TaskCallInfo>(ir.payload(idx).extra);
                    mark(ir, ti.func, ti.args);
                } else if (ir.tag(idx) == Inst::Tag::Call) {
                    const auto ci = ir.extra_data<CallInfo>(ir.payload(idx).extra);
                    mark(ir, ci.func, ci.args);
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
    // memory), addressed through its owning scope's own state global:
    // `@clotho.inst.<hier>.state + the scope-relative field path`. A real
    // symbol plus a def-local addend, so the GEP folds to a link-time-constant
    // address and the code bytes never depend on whole-design layout. A
    // shareable reused-definition body instead loads the base pointer from its
    // instance context (one shared body cannot bake N instance symbols). An
    // unscoped signal (flat root scope) keeps the direct root GEP.
    llvm::Value* storage_ptr(const SignalSlot& s) {
        std::vector<llvm::Value*> idx;
        idx.push_back(b_.getInt32(0));
        if (s.anchor == SignalSlot::no_anchor) {
            for (const uint32_t f : s.path) {
                idx.push_back(b_.getInt32(f));
            }
            return b_.CreateInBoundsGEP(state_ty_, sym(state_global_), idx);
        }
        Anchor&      a    = anchors_[s.anchor];
        llvm::Value* base = a.indirect && def_relative_for(s)
                              ? ctx_field(a, 0, llvm::PointerType::getUnqual(ctx_))
                              : static_cast<llvm::Value*>(sym(a.state_gv));
        for (const uint32_t f : s.path) {
            idx.push_back(b_.getInt32(f));
        }
        return b_.CreateInBoundsGEP(a.ty, base, idx);
    }

    // Pointer to one field of the root NBA shadow struct, addressed through a base
    // pointer loaded from a global rather than a constant offset from `state`.
    //
    // NOTE: the indirection is a deliberate defense against CPU memory-dependence
    // mis-speculation, not an abstraction nicety. The shadow region mirrors the
    // live signal region at a constant distance, so state-relative addressing
    // gives a shadow store and a nearby live load of a *different* signal the same
    // (base register, displacement mod 1024) form whenever the distance's low bits
    // line up (e.g. 256 32-bit NBA targets). Zen 4's memory-renaming predictor
    // keys on exactly that form and falsely forwards the store to the load; each
    // false match is a pipeline resync (~1 per staged signal, 3x sim time on
    // register-file-shaped designs, at any codegen level). Loading the shadow base
    // from a global keeps codegen from folding shadow addresses into
    // state+constant, so the colliding form cannot arise.
    // Whether the code being emitted should address `owner`'s per-instance
    // runtime structures def-relatively. True only inside a single-scope group
    // of `owner`'s own instance — the population body sharing can cover.
    // Everything else (cross-instance fused groups, tasks, commit functions,
    // cross-hierarchy references) is unshareable by construction and keeps the
    // absolute/folded form, which the optimizer amortizes to one hoisted base:
    // a 32-member cross-instance fused NBA thunk measured +23% -O sim with
    // unconditional indirection (32 per-instance base loads GVN cannot merge).
    // Either form addresses the same storage; this is codegen policy only.
    bool def_relative_for(const SignalSlot& owner) const {
        return sched_anchor_ != SignalSlot::no_anchor && owner.anchor == sched_anchor_;
    }

    // `owner` is the signal whose (def-local, for an anchored signal) shadow
    // field is addressed. A reused definition's own single-scope group loads
    // its instance's shadow base (@clotho.inst.*.shadow); every other context
    // folds the sub-struct index through the root base; unscoped signals keep
    // the flat root field. Every path loads a base pointer, preserving the
    // opacity requirement above.
    llvm::Value* shadow_ptr(const SignalSlot& owner, uint32_t field) {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        if (owner.anchor != SignalSlot::no_anchor) {
            // An NBA target's anchor always carries a shadow struct (the
            // fields were assigned per anchor in build_state_types).
            Anchor&      a    = anchors_[owner.anchor];
            llvm::Value* base = a.indirect && def_relative_for(owner)
                                  ? ctx_field(a, 1, ptr)
                                  : b_.CreateLoad(ptr, anchor_shadow_base(a));
            return b_.CreateInBoundsGEP(a.shadow_ty, base, { b_.getInt32(0), b_.getInt32(field) });
        }
        llvm::Value* base = b_.CreateLoad(ptr, shadow_base_gv());
        return b_.CreateInBoundsGEP(shadow_ty_, base, { b_.getInt32(0), b_.getInt32(field) });
    }

    // Per-instance opaque shadow base (@clotho.inst.<hier>.shadow_base):
    // created on first unshared shadow access, filled by main right after the
    // ctx fills. Same one-load opacity as the root shadow_base_gv.
    llvm::GlobalVariable* anchor_shadow_base(Anchor& a) {
        if (a.shadow_base_gv == nullptr) {
            auto* ptr        = llvm::PointerType::getUnqual(ctx_);
            a.shadow_base_gv = new llvm::GlobalVariable(env_->llmod, ptr, false,
                                                        llvm::GlobalValue::InternalLinkage,
                                                        llvm::ConstantPointerNull::get(ptr),
                                                        "clotho.inst." + a.name + ".shadow_base");
        }
        return sym(a.shadow_base_gv);
    }

    // Create the state globals — one per instance scope
    // (@clotho.inst.<hier>.state, own signals only) plus the root
    // @clotho.state for unscoped signals and the standalone NBA shadow global
    // — and record each anchor's lowest owned process index, before any body
    // is emitted (storage_ptr resolves through the anchors). Every global's
    // initializer is its power-on state (see emit_main's comment). A reused
    // definition's shareable accesses instead go through its instance context
    // (ensure_ctx), which main fills at startup before init_state and every
    // process.
    void emit_state_global() {
        auto* state_gv = new llvm::GlobalVariable(env_->llmod, state_ty_, false,
                                                  llvm::GlobalValue::InternalLinkage,
                                                  root_state_initializer(), "clotho.state");
        state_gv->setAlignment(llvm::Align(64));
        state_global_ = state_gv;

        for (Anchor& a : anchors_) {
            a.state_gv = new llvm::GlobalVariable(env_->llmod, a.ty, false,
                                                  llvm::GlobalValue::InternalLinkage,
                                                  scope_state_initializer(a),
                                                  "clotho.inst." + a.name + ".state");
            // ABI alignment (no explicit override): small scope structs pack
            // into shared cache lines; 64-byte over-alignment measured +1.5%
            // hazard3 sim from scattering hot signals across lines.
        }

        for (Anchor& a : anchors_) {
            if (a.shadow_ty != nullptr) {
                a.shadow_gv = new llvm::GlobalVariable(env_->llmod, a.shadow_ty, false,
                                                       llvm::GlobalValue::InternalLinkage,
                                                       llvm::ConstantAggregateZero::get(a.shadow_ty),
                                                       "clotho.inst." + a.name + ".shadow");
            }
        }
        if (shadow_ty_ != nullptr) {
            shadow_global_ = new llvm::GlobalVariable(env_->llmod, shadow_ty_, false,
                                                      llvm::GlobalValue::InternalLinkage,
                                                      llvm::ConstantAggregateZero::get(shadow_ty_),
                                                      "clotho.nba.shadow");
        }

        const auto procs = mod_.processes();
        for (uint32_t k = 0; k < procs.size(); ++k) {
            const uint32_t sc = procs[k].scope;
            if (sc == Process::no_scope || sc >= anchor_of_scope_.size() ||
                anchor_of_scope_[sc] == SignalSlot::no_anchor) {
                continue;
            }
            uint32_t& first = anchors_[anchor_of_scope_[sc]].first_proc;
            first           = std::min(first, k);
        }

    }

    // Lazily create the instance context global (see Anchor). The type is
    // computed by build_instance_ctx_layout; only instances actually addressed
    // def-relatively materialize a global (and a fill in main).
    llvm::GlobalVariable* ensure_ctx(Anchor& a) {
        if (a.ctx_gv == nullptr) {
            a.ctx_gv = new llvm::GlobalVariable(env_->llmod, a.ctx_ty, false,
                                                llvm::GlobalValue::InternalLinkage,
                                                llvm::ConstantAggregateZero::get(a.ctx_ty),
                                                "clotho.inst." + a.name + ".ctx");
        }
        return sym(a.ctx_gv);
    }

    // A def-relative load of one instance-context field.
    llvm::Value* ctx_field(Anchor& a, uint32_t field, llvm::Type* ty) {
        auto* gep = llvm::ConstantExpr::getInBoundsGetElementPtr(
          a.ctx_ty, ensure_ctx(a), llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(field) });
        return b_.CreateLoad(ty, gep);
    }

    // Materialize the group's deferred def-relative context at the current
    // insert point (a function entry, so it dominates every use): the sched-flag
    // pointer (instance sched base + def-local ordinal) and, when the group can
    // register NBA blocks, the instance's block-id base (block_register adds the
    // group-local block ordinal). No-op when lower_group chose the absolute
    // constants; a dead base load in a group that ends up registering no block
    // is swept by the tier-0 dce pass.
    void materialize_group_ctx() {
        if (sched_ptr_ != nullptr) {
            return;
        }
        Anchor&      a    = anchors_[sched_anchor_];
        llvm::Value* base = ctx_field(a, 2, llvm::PointerType::getUnqual(ctx_));
        sched_ptr_ = b_.CreateInBoundsGEP(b_.getInt32Ty(), base, b_.getInt32(sched_ordinal_));
    }

    // Global holding &state->shadow; created on first use, initialized in
    // emit_main right after the state allocation (before any process runs).
    llvm::GlobalVariable* shadow_base_gv() {
        if (shadow_base_gv_ == nullptr) {
            auto* ptr       = llvm::PointerType::getUnqual(ctx_);
            shadow_base_gv_ = new llvm::GlobalVariable(env_->llmod, ptr, false, llvm::GlobalValue::InternalLinkage,
                                                       llvm::ConstantPointerNull::get(ptr), "clotho.nba.shadow_base");
        }
        return sym(shadow_base_gv_);
    }

    // Pointer to an lvalue's mm (field 0) or z (field 1) word, resolving the
    // selected leaf element for a memory target.
    llvm::Value* lv_field_ptr(const LValue& lv, uint32_t field) {
        if (lv.is_agg) {
            // Walk the composed GEP path from the aggregate root to the leaf slot,
            // then select the leaf's mm (0) / z (1) field. Struct fields and array
            // indices are indistinguishable to the GEP — both are index operands.
            std::vector<llvm::Value*> idx;
            idx.reserve(lv.agg_path.size() + 2);
            idx.push_back(b_.getInt32(0));
            for (llvm::Value* step : lv.agg_path) {
                idx.push_back(step);
            }
            idx.push_back(b_.getInt32(field));
            return b_.CreateInBoundsGEP(lv.root_ty, lv.base, idx);
        }
        if (lv.array_ty != nullptr) {
            llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
            return b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx, b_.getInt32(field) });
        }
        return b_.CreateStructGEP(lv.slot_ty, lv.base, field);
    }
    // Pointer to a ref actual's slot, passed to the callee as the aliased storage.
    // A whole scalar is its slot pointer; an unpacked-array element is `&array[idx]`
    // (its element slot). The selection must be a whole element — the callee GEPs it
    // as a packed `{ i(2W), iW }` slot, with no bit/part offset. The frontend guards
    // to these shapes (is_whole_ref_actual); this rejects anything else defensively.
    llvm::Value* ref_actual_ptr(const LValue& lv) {
        const bool whole = !lv.is_string && lv.range_mask == nullptr &&
                           is_zero_const(lv.bit_off) && lv.width == lv.store_width;
        if (!whole) {
            throw BackendError{ "ref actual must be a whole variable or unpacked-array element" };
        }
        if (lv.array_ty != nullptr) {
            llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
            return b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx });
        }
        return lv.base;
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

    // Lower one fusion group (usually a singleton) to one coroutine. Members of
    // a fused group share one frame, one sched flag (the first member's slot),
    // and one wait suspend; their bodies run back-to-back per wake, in
    // scheduled-rank order (see compute_fusion_groups for the legality
    // argument). The per-member value tables are swapped in around each
    // member's body.
    llvm::Function* lower_group(const std::vector<uint32_t>& members) {
        const auto     all        = mod_.processes();
        const uint32_t proc_index = members.front();
        const Process& proc       = all[proc_index];
        ir_                       = &proc.ir;
        frame_slots_.clear(); // a process has no frame; deinit sees only temps

        // This group's scheduled-flag slot: &clotho.proc_sched[proc_index]. A
        // constant GEP so it is valid in any block of the function. Dedup prep:
        // a group fully inside one reused-definition instance instead loads the
        // instance's sched base and indexes the def-local ordinal — the same
        // slot, materialized at function entry (materialize_sched_ptr); a
        // unique-definition or cross-instance group keeps the folded constant.
        sched_anchor_      = SignalSlot::no_anchor;
        sched_group_       = group_of_[proc_index];
        const uint32_t gaid = group_anchor_of_[sched_group_];
        if (gaid != SignalSlot::no_anchor) {
            sched_anchor_  = gaid;
            sched_ordinal_ = proc_sched_ord_[proc_index];
        }
        sched_ptr_ = sched_anchor_ != SignalSlot::no_anchor ? nullptr : sched_flag_ptr(proc_index);

        // Same-eligibility NBA prep: the group's block registrations pass
        // (instance block base + group-local ordinal) instead of the absolute
        // block id — identical value (a group's blocks are id-contiguous by
        // construction: block_register only runs while its group lowers), in the
        // def-relative form a shared body needs. Main stores the base into the
        // instance ctx after lowering, once the ids are known (ctx_blk_fills_).
        group_first_block_ = -1;

        // Name a process by its index: @<module>.p<index>. Source-offset naming
        // would collide once the design has multiple instances of one module —
        // their processes share the same AST source location. The index is unique
        // across the flattened module; proc.loc still carries the source mapping.
        // A fused group is named by its first member plus its size.
        std::string name = mod_.name + ".p" + std::to_string(proc_index);
        if (members.size() > 1) {
            name += ".f" + std::to_string(members.size());
        }

        // Per-member table setup, run before lowering anything of that member.
        const auto select_member = [&](uint32_t m) {
            ir_            = &all[m].ir;
            const size_t n = ir_->inst_count();
            values_.assign(n, Plane{});
            lvals_.assign(n, std::optional<LValue>{});
            types_.resize(n);
            types_valid_.assign(n, 0);
        };

        // Coroutine elision: a suspension-free always-with-sensitivity group (see
        // is_simple_always) carries no state across activations, so instead of a
        // coroutine it lowers to a plain thunk — void @<name>.thunk(state, wheel)
        // — run once per activation. The seed subscribes a tagged pointer to it
        // and the drain calls it directly (no frame, no suspend switch, no
        // CoroSplit). The sched-flag timeline matches the coroutine always-loop:
        // the flag is held set across the body and cleared at the end, so
        // same-instant re-triggers behave identically.
        if (thunk_group_[group_of_[proc_index]]) {
            auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
            auto* fnty   = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty, ptr_ty }, false);
            fn_ = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, name + ".thunk", env_->llmod);
            state_ = fn_->getArg(0);
            state_->setName("state");
            wheel_ = fn_->getArg(1);
            wheel_->setName("wheel");
            fn_->addParamAttr(0, llvm::Attribute::NoAlias);
            awaiter_          = nullptr;
            record_           = nullptr;
            fork_record_      = nullptr;
            cur_is_task_      = false;
            lowering_preinit_ = false;
            hdl_              = nullptr;
            auto* entry_bb = llvm::BasicBlock::Create(ctx_, "entry", fn_);
            b_.SetInsertPoint(entry_bb);
            materialize_group_ctx();
            // Run each member body per activation, freeing its string temporaries
            // afterward — a thunk has no frame to persist them in (a per-call cost
            // paid only by string-using bodies). frame_slots_ is empty for a
            // process, so deinit_body_strings frees exactly this member's temps.
            const auto lower_bodies = [&] {
                for (const uint32_t m : members) {
                    select_member(m);
                    str_vals_.assign(ir_->inst_count(), nullptr);
                    build_str_temps();
                    build_dynarr_temps();
                    lower_region(ir_->root());
                    deinit_body_strings();
                }
                b_.CreateStore(b_.getInt32(0), sched_ptr_); // re-arm: ready to be woken
                b_.CreateRetVoid();
            };
            const uint32_t g = group_of_[proc_index];
            if (!spec_group_[g]) {
                lower_bodies();
                return fn_;
            }
            // Domain-specialized group: validate the read set reader-side. Load
            // each tracked 4-state signal's packed word and test it clean (no
            // x/z bit); short-circuit to the generic body on the first dirty
            // signal. When every read is clean the assume-clean fast body is
            // exact (see compute_domain_spec). The guard loads CSE with the
            // body's own loads of the same signals (early-cse), so the marginal
            // guard cost is the x/z tests, not the loads — and a persistently
            // dirty read set exits after ~one test. No write-side maintenance:
            // the check reads the committed live state at dispatch, so nothing
            // is paid at write sites (unlike the tag/counter scheme this
            // replaced, whose per-write retest was the whole ungated residual).
            auto* gen_bb  = llvm::BasicBlock::Create(ctx_, "dom.gen", fn_);
            auto* fast_bb = llvm::BasicBlock::Create(ctx_, "dom.fast", fn_);
            for (const uint32_t si : group_reads_[g]) {
                const LValue   lv = signal_lvalue(static_cast<Signal>(si));
                llvm::Value*   mm = load_lv_mm(lv); // i2W packed must|may word
                const uint32_t w  = lv.store_width;
                llvm::Value*   xz = b_.CreateTrunc(
                  b_.CreateXor(mm, b_.CreateLShr(mm, llvm::ConstantInt::get(mm->getType(), w))), int_ty(w));
                auto* next = llvm::BasicBlock::Create(ctx_, "dom.chk", fn_);
                b_.CreateCondBr(b_.CreateICmpEQ(xz, zero(w)), next, gen_bb);
                b_.SetInsertPoint(next);
            }
            const auto bump = [&](llvm::GlobalVariable* gv) {
                if (!dom_debug_) {
                    return; // counters exist only under CLOTHO_DOM_DEBUG (gv is null otherwise)
                }
                auto* p = llvm::ConstantExpr::getInBoundsGetElementPtr(
                  dom_cnt_ty_, sym(gv), llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(g) });
                b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt64Ty(), p), b_.getInt64(1)), p);
            };
            b_.CreateBr(fast_bb);
            b_.SetInsertPoint(gen_bb);
            bump(dom_ngen_gv_);
            lower_bodies();
            b_.SetInsertPoint(fast_bb);
            bump(dom_nfast_gv_);
            assume_clean_ = true;
            lower_bodies();
            assume_clean_ = false;
            return fn_;
        }

        // A process lowers to a switched-resume LLVM coroutine:
        // `ptr @<module>.<offset>(ptr noalias state, ptr wheel)`. The ramp runs the
        // body until the first #delay (or to completion) and returns the coroutine
        // handle; the scheduler resumes/destroys through that handle. A #delay (see
        // lower_delay) schedules a wheel timer and suspends; the timer's callback
        // resumes the coroutine when the delay elapses. A delay-free process runs
        // straight to the final suspend, exactly like the old plain function.
        auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto* fnty   = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
        fn_          = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, name, env_->llmod);
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
        record_           = nullptr;
        fork_record_      = nullptr;
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
        materialize_group_ctx();
        auto* id   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_id), { b_.getInt32(0), null, null, null });
        auto* need = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_alloc), { id });
        b_.CreateCondBr(need, alloc_bb, begin_bb);

        // Dynamic frame: rt_alloc(coro.size). The sim owns frame memory so a frame
        // outlives the ramp while the process is suspended on the wheel.
        b_.SetInsertPoint(alloc_bb);
        // Over-allocate by coro.align: with an over-aligned frame (e.g. an i128
        // plane spill) CoroSplit lays out fields past coro.size unless the
        // allocation covers the realignment slack (LLVM coroutine contract).
        auto* size  = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_size, { b_.getInt64Ty() }), {});
        auto* align = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_align, { b_.getInt64Ty() }), {});
        auto* mem   = b_.CreateCall(alloc_fn(), { b_.CreateAdd(size, align) });
        b_.CreateBr(begin_bb);

        b_.SetInsertPoint(begin_bb);
        auto* frame = b_.CreatePHI(ptr_ty, 2);
        frame->addIncoming(null, entry);
        frame->addIncoming(mem, alloc_bb);
        hdl_        = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_begin), { id, frame });
        coro_begin_ = llvm::cast<llvm::Instruction>(hdl_);
        // String rvalue temporaries live in the coroutine frame (CoroSplit spills
        // any that survive a suspend) and are constructed once, in the ramp —
        // per member, with each member's inst-indexed pointers snapshotted for
        // its body lowering below. (str_temp_list_ ends up holding only the
        // last member's temps; it feeds deinit_body_strings, which an
        // always-loop group never reaches.)
        std::vector<std::vector<llvm::Value*>> member_strs;
        std::vector<std::vector<llvm::Value*>> member_dynarr;
        for (const uint32_t m : members) {
            select_member(m); // sizes the type cache build_str_temps consults
            str_vals_.assign(ir_->inst_count(), nullptr);
            build_str_temps();
            build_dynarr_temps();
            member_strs.push_back(str_vals_);
            member_dynarr.push_back(dynarr_ret_);
        }
        ir_ = &proc.ir;

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
        // Per-process $urandom RNG (LRM §18.13): allocate the xoshiro256++ state
        // in the coroutine frame and seed it once here in the ramp, before the
        // body loop, so it persists across activations (the loop back-edge is
        // below this point) and CoroSplit frames it. The seed selector is the
        // process index — an independent, run-stable stream per process,
        // schedule-independent; $urandom(seed) reseeds in place. Forked-child
        // seeding from the parent's stream lands with fork.
        rng_slot_ = nullptr;
        if (process_uses_urandom(proc)) {
            rng_slot_ = b_.CreateAlloca(llvm::ArrayType::get(b_.getInt64Ty(), 4), nullptr, "urng");
            b_.CreateCall(urandom_seed_fn(), { rng_slot_, b_.getInt64(proc_index) });
        }
        const bool always_loop = proc.activation == Activation::Always && !proc.wait_events.empty();
        if (always_loop) {
            // An always block with a sensitivity list. It is subscribed to its
            // signals' static queues from the seed loop (emit_active_run) and left
            // parked at the initial suspend; the first wake resumes here. So the
            // loop is body-first: run the bodies for the trigger that woke us,
            // re-arm the scheduled flag, suspend until the next trigger, repeat.
            // The bodies therefore run only on a trigger, never at time zero, with
            // the flag held set across them (it is cleared just before the
            // suspend), so the group's own stores can never re-wake it mid-body.
            auto* loop_bb = llvm::BasicBlock::Create(ctx_, "always.loop", fn_);
            b_.CreateBr(loop_bb);
            b_.SetInsertPoint(loop_bb);
            for (size_t i = 0; i < members.size(); ++i) {
                select_member(members[i]);
                str_vals_   = member_strs[i];
                dynarr_ret_ = member_dynarr[i];
                lower_region(ir_->root());              // member body (per trigger)
            }
            b_.CreateStore(b_.getInt32(0), sched_ptr_); // re-arm: ready to be woken
            emit_wait_suspend();                        // park until next trigger
            b_.CreateBr(loop_bb);
            // The forever back-edge means the shared final-suspend tail below is
            // unreachable; build it in a dead block so the IR stays well-formed.
            b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "always.exit", fn_));
        } else {
            // Never fused: a non-always (or sensitivity-less) group is a singleton.
            select_member(proc_index);
            str_vals_   = member_strs.front();
            dynarr_ret_ = member_dynarr.front();
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

    // The native LLVM type of a DPI-C scalar (LRM 35.5.6): the by-value C type
    // each SV scalar maps to. Wider SV types never reach here (frontend-guarded).
    llvm::Type* dpi_c_llvm_ty(DpiCType t) {
        switch (t) {
            case DpiCType::Void:
                return b_.getVoidTy();
            case DpiCType::Byte:
            case DpiCType::BitScalar:   // svBit
            case DpiCType::LogicScalar: // svLogic
                return b_.getInt8Ty();
            case DpiCType::Shortint:
                return b_.getInt16Ty();
            case DpiCType::Int:
                return b_.getInt32Ty();
            case DpiCType::Longint:
                return b_.getInt64Ty();
            case DpiCType::Real:
                return b_.getDoubleTy();
            case DpiCType::Shortreal:
                return b_.getFloatTy();
            case DpiCType::Chandle:
                return raw_ptr_ty(); // void*
            case DpiCType::String:
                return raw_ptr_ty(); // char* (input const char* by value; return char*)
            case DpiCType::BitVec:
            case DpiCType::LogicVec:
            case DpiCType::OpenArray:
                // A packed vector / open array is always passed by pointer and is
                // never a return type; declare_dpi_import emits the pointer directly,
                // so this is never reached for a by-value slot.
                throw BackendError{ "internal: DPI vec / open array has no by-value C type" };
        }
        throw BackendError{ "internal: unknown DPI C type" };
    }

    // A packed-vector DPI arg (svBitVecVal* / svLogicVecVal*), passed by pointer in
    // every direction — as opposed to a by-value scalar.
    static bool is_dpi_vec(DpiCType ct) {
        return ct == DpiCType::BitVec || ct == DpiCType::LogicVec;
    }

    // Declare a DPI-C import (LRM 35) as an external C function with its native
    // scalar signature (no `state` param, no plane-pair expansion). extern_name
    // resolves at JIT link time against a --dpi-lib shared object. A `pure` import
    // is side-effect-free, so mark it readnone + willreturn + nounwind so the
    // optimizer can CSE / hoist / DCE the call.
    llvm::Function* declare_dpi_import(FunctionId id) {
        const Function&          f   = mod_.function(id);
        auto*                    ptr = llvm::PointerType::getUnqual(ctx_);
        std::vector<llvm::Type*> params;
        params.reserve(f.dpi_arg_ctypes.size());
        for (size_t k = 0; k < f.dpi_arg_ctypes.size(); ++k) {
            const ArgDir   dir = f.arg_dirs.empty() ? ArgDir::In : f.arg_dirs[k];
            const DpiCType ct  = f.dpi_arg_ctypes[k];
            // A packed vector is a pointer in every direction (svBitVecVal* /
            // svLogicVecVal*); an open array is a pointer (svOpenArrayHandle); a
            // scalar is by value for input, by pointer for output / inout (the C
            // function writes through it).
            const bool by_ptr = is_dpi_vec(ct) || ct == DpiCType::OpenArray || dir != ArgDir::In;
            params.push_back(by_ptr ? ptr : dpi_c_llvm_ty(ct));
        }
        auto* fnty   = llvm::FunctionType::get(dpi_c_llvm_ty(f.dpi_ret_ctype), params, false);
        auto  callee = env_->llmod.getOrInsertFunction(f.extern_name, fnty);
        auto* fn     = llvm::dyn_cast<llvm::Function>(callee.getCallee());
        if (fn == nullptr) {
            throw BackendError{ "DPI import '" + f.extern_name +
                                "' conflicts with an existing symbol of a different type" };
        }
        if (f.is_pure) {
            fn->setDoesNotAccessMemory();
            fn->setWillReturn();
            fn->setDoesNotThrow();
        }
        return fn;
    }

    // Create (but do not yet lower) a function's declaration, so calls — including
    // recursive and forward ones — resolve before any body is emitted.
    llvm::Function* declare_function(FunctionId id) {
        const Function& f = mod_.function(id);
        if (f.is_dpi) {
            return declare_dpi_import(id);
        }
        if (f.is_task) {
            return declare_task(id);
        }
        auto*                    ptr = llvm::PointerType::getUnqual(ctx_);
        std::vector<llvm::Type*> params;
        params.push_back(ptr); // state
        // TODO: pass a 2-state formal as a single iW param (and a 2-state result as
        // a bare iW) instead of the packed (mm, z) pair. The wire is packed today —
        // a 2-state arg lifts to must==may with a constant-0 z, which LLVM folds —
        // so this is a call-ABI size micro-opt, not a correctness gap.
        for (size_t k = 0; k < f.param_types.size(); ++k) {
            const Ref     pt  = f.param_types[k];
            const ArgDir dir = f.arg_dirs.empty() ? ArgDir::In : f.arg_dirs[k];
            if (dir == ArgDir::Ref) {
                params.push_back(ptr); // one pointer to the actual's packed slot
                continue;
            }
            // A dynamic array or queue is one handle pointer regardless of
            // direction (the callee copies in / out through it); never a (mm, z) pair.
            if (is_handle_type(pt)) {
                params.push_back(ptr); // &actual (rt_dynarr* / rt_queue*)
                continue;
            }
            if (dir == ArgDir::Out || dir == ArgDir::Inout) {
                params.push_back(ptr); // &caller_temp.mm
                params.push_back(ptr); // &caller_temp.z (copy-out at each return)
                continue;
            }
            if (is_string_type(pt) || contains_struct(pt)) {
                // A string handle or an aggregate's storage passes by pointer (In
                // only — the frontend guards aggregate out/inout formals); the
                // callee copy-in gives value semantics. (A dynamic array is
                // handled above, one pointer in every direction.)
                params.push_back(ptr);
                continue;
            }
            const uint32_t w = width_of(pt);
            params.push_back(int_ty(2 * w)); // must|may
            params.push_back(int_ty(w));     // z
        }
        // A string, aggregate, dynamic-array, or queue result returns through a
        // trailing out-param the caller owns (its call-site temp) — no plane form.
        llvm::Type* rty = fn_return_ty(f.return_type);
        if (is_string_type(f.return_type) || contains_struct(f.return_type) ||
            is_handle_type(f.return_type)) {
            rty = b_.getVoidTy();
            params.push_back(ptr);
        }
        auto* fnty = llvm::FunctionType::get(rty, params, false);
        auto* fn   = llvm::Function::Create(fnty, llvm::Function::InternalLinkage,
                                            mod_.name + ".fn." + f.name, env_->llmod);
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
            if (f.arg_dirs[k] == ArgDir::Ref) {
                params.push_back(ptr); // one pointer to the actual's packed slot
            } else if (is_handle_type(f.param_types[k])) {
                params.push_back(ptr); // &actual (rt_dynarr* / rt_queue*), any direction
            } else if (f.arg_dirs[k] == ArgDir::In) {
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
                                            mod_.name + ".task." + f.name, env_->llmod);
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
    // Whether formal k occupies a single LLVM parameter (a ref pointer, or a
    // string input handle) rather than the (mm, z) pair every other formal uses.
    bool one_param_formal(const Function& f, size_t k) const {
        if (!f.arg_dirs.empty() && f.arg_dirs[k] == ArgDir::Ref) {
            return true;
        }
        // An aggregate is one pointer (In only — out/inout aggregates are guarded).
        if (contains_struct(f.param_types[k])) {
            return true;
        }
        // A dynamic array or queue is one handle pointer in every direction (input /
        // output / inout all pass &actual; the callee copies in/out through it).
        if (is_handle_type(f.param_types[k])) {
            return true;
        }
        return is_string_type(f.param_types[k]) && (!f.is_task || f.arg_dirs[k] == ArgDir::In);
    }

    std::vector<uint32_t> param_indices(const Function& f) const {
        std::vector<uint32_t> idx(f.param_types.size());
        uint32_t              cur = f.is_task ? 2 : 1;
        for (size_t k = 0; k < f.param_types.size(); ++k) {
            idx[k] = cur;
            cur += one_param_formal(f, k) ? 1 : 2;
        }
        return idx;
    }

    // LLVM param index of the task's record slot: right after the last formal.
    uint32_t task_record_param(const Function& f) const {
        const auto idx = param_indices(f);
        if (idx.empty()) {
            return 2;
        }
        return idx.back() + (one_param_formal(f, f.param_types.size() - 1) ? 1 : 2);
    }

    // Build a function's frame struct: one slot field per local, in Local-handle
    // order (a Local's index is its GEP field index).
    // Locals that cannot use the narrow iW slot (they stay packed `{ i(2W), iW }`):
    //   - passed as a task-call output / inout argument (the callee writes them back
    //     through the packed-word pointer ABI), or
    //   - passed as a ref argument to a function or task (the callee GEPs the passed
    //     pointer with a packed slot type; the actual's storage must match).
    // A plain function's own outputs are by-value and dropped, so only these apply.
    std::vector<bool> frame_out_targets(const Function& f) const {
        std::vector<bool> out(f.local_types.size(), false);
        const Ir&         ir = f.ir;
        const auto        mark_packed_args = [&](FunctionId fid, ExtraSlice argslice) {
            const Function& callee = mod_.function(fid);
            const auto      args   = ir.region(argslice);
            for (size_t a = 0; a < args.size(); ++a) {
                if (callee.arg_dirs.empty() || callee.arg_dirs[a] == ArgDir::In) {
                    continue;
                }
                const Index root = lvalue_root(ir, args[a]).root;
                if (ir.tag(root) == Inst::Tag::Local) {
                    out[ir.payload(root).local.index] = true;
                }
            }
        };
        for (size_t k = 0; k < ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            if (ir.tag(idx) == Inst::Tag::TaskCall) {
                const auto ti = ir.extra_data<TaskCallInfo>(ir.payload(idx).extra);
                mark_packed_args(ti.func, ti.args);
            } else if (ir.tag(idx) == Inst::Tag::Call) {
                const auto ci = ir.extra_data<CallInfo>(ir.payload(idx).extra);
                mark_packed_args(ci.func, ci.args);
            }
        }
        return out;
    }

    void build_frame_layout(const Function& f) {
        const size_t n = f.local_types.size();
        frame_slots_.assign(n, FrameSlot{});
        ref_ptr_.assign(n, nullptr);
        const std::vector<bool>  out_target = frame_out_targets(f);
        std::vector<llvm::Type*> fields(n);
        for (uint32_t i = 0; i < n; ++i) {
            const Layout lay           = mod_.pool.layout(f.local_types[i]);
            frame_slots_[i].ir_type    = f.local_types[i];
            frame_slots_[i].width      = lay.width == 0 ? 1 : lay.width;
            frame_slots_[i].four_state = lay.domain == Domain::Four;
            // A ref formal (local k for formal k) has no own storage: its dead field
            // is kept for index alignment and loads / stores redirect to the passed
            // actual pointer. The redirect GEPs a packed slot, so force one here.
            const bool is_ref_formal = i < f.arg_dirs.size() && f.arg_dirs[i] == ArgDir::Ref;
            frame_slots_[i].is_ref   = is_ref_formal;
            if (needs_agg_storage(f.local_types[i])) {
                frame_slots_[i].is_agg     = true;
                frame_slots_[i].storage_ty = agg_storage_ty(f.local_types[i]);
                fields[i]                  = frame_slots_[i].storage_ty;
                continue;
            }
            if (is_string_type(f.local_types[i])) {
                frame_slots_[i].is_string = true;
                frame_slots_[i].slot_ty   = str_slot_ty();
                fields[i]                 = frame_slots_[i].slot_ty;
                continue;
            }
            // A ref dynamic-array / queue formal aliases the caller's handle (no
            // owned frame slot, no init/copy/free): fall through to the dead-field
            // path, and local_lvalue redirects its base to the passed pointer.
            if (is_handle_type(f.local_types[i]) && !is_ref_formal) {
                frame_slots_[i].is_dynarr    = is_dynarr_type(f.local_types[i]);
                frame_slots_[i].is_queue     = is_queue_type(f.local_types[i]);
                frame_slots_[i].handle_type  = f.local_types[i];
                frame_slots_[i].slot_ty      = handle_slot_ty(f.local_types[i]);
                fields[i]                    = frame_slots_[i].slot_ty;
                continue;
            }
            // A 2-state local uses the genuine iW slot; the packed-word ABI wire is
            // bridged by copy-in/out. A 4-state local keeps its z field (the ABI and
            // copy-in/out use it; LLVM folds the zero-z traffic on its own). A local
            // passed as a task out/inout target — or a ref formal / ref actual — stays
            // packed (the packed-word pointer ABI writes it back / aliases it).
            frame_slots_[i].narrow  = !frame_slots_[i].four_state && !out_target[i] && !is_ref_formal;
            // A non-narrow slot keeps its z field: a 4-state local needs it, and a
            // packed 2-state out-target local is written back through the (mm, z)
            // pointer ABI. A narrow local has no z.
            frame_slots_[i].slot_ty = slot_type(frame_slots_[i].width, frame_slots_[i].four_state,
                                                !frame_slots_[i].narrow, frame_slots_[i].narrow);
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
            // A ref formal's field is dead (accesses redirect to the actual); no init.
            if (s.is_ref) {
                continue;
            }
            llvm::Value*     slot = b_.CreateStructGEP(frame_ty_, frame_, i);
            if (s.is_agg) {
                // Recursive fill (X into 4-state leaves, init string leaves); an
                // input formal's slot is then overwritten by copy-in.
                std::vector<llvm::Value*> path;
                emit_agg_init(s.ir_type, slot, path, s.storage_ty);
                continue;
            }
            if (s.is_string) {
                b_.CreateCall(str_init_fn(), { slot });
                continue;
            }
            if (s.is_dynarr || s.is_queue) {
                // Empty handle ({0}): the frame alloca is not zeroed, so store the
                // empty handle (null buffer pointer, zero length).
                b_.CreateStore(llvm::Constant::getNullValue(s.slot_ty), slot);
                continue;
            }
            // A narrow 2-state local is a single iW field (comes up 0); a 4-state
            // local is the packed word (X) plus a cleared z field.
            if (s.narrow) {
                b_.CreateStore(zero(s.width), b_.CreateStructGEP(s.slot_ty, slot, 0));
                continue;
            }
            llvm::Value*     mm   = s.four_state ? x_mm(s.width) : zero(2 * s.width);
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
            } else if (frame_slots_[i].is_dynarr || frame_slots_[i].is_queue) {
                // A dynamic-array / queue local owns its heap buffer (value
                // semantics): free it at every body exit so it does not leak.
                emit_handle_delete(b_.CreateStructGEP(frame_ty_, frame_, i),
                                   frame_slots_[i].handle_type);
            } else if (frame_slots_[i].is_agg && !frame_slots_[i].is_ref &&
                       has_heap_leaf(frame_slots_[i].ir_type)) {
                // An aggregate local with string / handle leaves owns their heap
                // buffers at any nesting depth: recursive teardown.
                std::vector<llvm::Value*> path;
                emit_agg_deinit(frame_slots_[i].ir_type, b_.CreateStructGEP(frame_ty_, frame_, i),
                                path, frame_slots_[i].storage_ty);
            }
        }
        for (llvm::Value* temp : str_temp_list_) {
            b_.CreateCall(str_deinit_fn(), { temp });
        }
        for (const auto& [temp, type] : dynarr_temp_list_) {
            emit_handle_delete(temp, type);
        }
    }

    // Lower a function body into its (already declared) LLVM function. Reuses the
    // shared instruction lowering — Arg reads a parameter, Local addresses the
    // frame, Ret is a real return, Call invokes another function.
    void lower_function(FunctionId id) {
        sched_anchor_ = SignalSlot::no_anchor; // functions are not a shareable group
        const Function& f = mod_.function(id);
        if (f.is_dpi) {
            // A DPI import is only an external declaration (declare_dpi_import);
            // its body is native C, so there is nothing to lower.
            return;
        }
        ir_ = &f.ir;
        cur_fn_           = &f;
        cur_is_task_      = false;
        param_llvm_index_ = param_indices(f);
        build_frame_layout(f);

        fn_            = fn_decls_[static_cast<uint32_t>(id)];
        const size_t n = ir_->inst_count();
        values_.assign(n, Plane{});
        lvals_.assign(n, std::optional<LValue>{});
        types_.resize(n);
        types_valid_.assign(n, 0);
        str_vals_.assign(n, nullptr);

        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
        b_.SetInsertPoint(entry);
        state_ = fn_->getArg(0);
        state_->setName("state");

        frame_ = b_.CreateAlloca(frame_ty_, nullptr, "frame");
        init_frame();
        emit_fn_agg_copyin(f);
        build_str_temps();
        build_dynarr_temps();
        bind_ref_formals(f);
        emit_copyin(f);

        lower_region(ir_->root());
        // The frontend appends a trailing Ret, so the entry path always terminates;
        // guard defensively in case a body lowered to nothing (an implicit return
        // still copies output / inout formals back).
        if (b_.GetInsertBlock()->getTerminator() == nullptr) {
            emit_copyout(f);
            deinit_body_strings();
            // Check the LLVM return type, not the IR one: a string / dynamic-array
            // result is void at the ABI (it returns through a trailing out-param).
            if (fn_->getReturnType()->isVoidTy()) {
                b_.CreateRetVoid();
            } else {
                b_.CreateRet(llvm::ConstantAggregateZero::get(fn_->getReturnType()));
            }
        }

        cur_fn_ = nullptr;
        frame_  = nullptr;
    }

    // Copy each aggregate input formal's caller storage (passed by pointer) into
    // its frame slot at function entry (value semantics). Scalar / string formals
    // copy in via the frontend's Store(Local, Arg); an aggregate formal emits no
    // Arg, so the copy is done here. Aggregate output / inout function formals are
    // guarded in the frontend, so only inputs occur.
    void emit_fn_agg_copyin(const Function& f) {
        for (uint32_t k = 0; k < f.param_types.size(); ++k) {
            if (!contains_struct(f.param_types[k])) {
                continue;
            }
            emit_agg_storage_copy(b_.CreateStructGEP(frame_ty_, frame_, k),
                                  fn_->getArg(param_llvm_index_[k]), f.param_types[k]);
        }
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
        types_.resize(n);
        types_valid_.assign(n, 0);
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
        // Over-allocate by coro.align: with an over-aligned frame (e.g. an i128
        // plane spill) CoroSplit lays out fields past coro.size unless the
        // allocation covers the realignment slack (LLVM coroutine contract).
        auto* size  = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_size, { b_.getInt64Ty() }), {});
        auto* align = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_align, { b_.getInt64Ty() }), {});
        auto* mem   = b_.CreateCall(alloc_fn(), { b_.CreateAdd(size, align) });
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
        sched_anchor_      = SignalSlot::no_anchor;
        group_first_block_ = -1;

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
        build_dynarr_temps();
        bind_ref_formals(f);
        emit_copyin(f);
        lower_region(ir_->root());
        branch_to(epilogue_bb);

        // Epilogue: copy output / inout formals back through their pointers, then
        // resume the caller (the record's parent, if any) and fall into the final
        // suspend. In the synchronous path the caller never wrote a parent (it reads
        // the task `done` and resumes inline), so the load is null and nothing is
        // enqueued; in the blocked path the caller stored its handle before
        // suspending, so we push it onto the active queue here.
        b_.SetInsertPoint(epilogue_bb);
        emit_copyout(f);
        deinit_body_strings(); // string locals + temps die with the call
        llvm::Value* rec    = fn_->getArg(task_record_param(f));
        auto*        has_bb = llvm::BasicBlock::Create(ctx_, "task.wake", fn_);
        if (f.is_fork_child) {
            // Fork-child epilogue (LRM 9.3.2): the record is { i32 count, ptr
            // parent } shared by the fork's children. Decrement the live count;
            // the child that drops it to zero resumes the joined parent (null
            // until the parent has spawned all children and suspended — a child
            // that finishes synchronously during the spawn loop just decrements).
            auto*        cntp   = b_.CreateStructGEP(fork_record_ty(), rec, 0);
            llvm::Value* cnt    = b_.CreateSub(b_.CreateLoad(b_.getInt32Ty(), cntp), b_.getInt32(1));
            b_.CreateStore(cnt, cntp);
            llvm::Value* parent = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(fork_record_ty(), rec, 1));
            llvm::Value* last   = b_.CreateICmpEQ(cnt, b_.getInt32(0));
            llvm::Value* wake   = b_.CreateAnd(last, b_.CreateICmpNE(parent, null));
            b_.CreateCondBr(wake, has_bb, final_bb);
            b_.SetInsertPoint(has_bb);
            emit_active_push(b_, parent);
            b_.CreateBr(final_bb);
        } else {
            llvm::Value* parent = b_.CreateLoad(ptr_ty, rec);
            b_.CreateCondBr(b_.CreateICmpNE(parent, null), has_bb, final_bb);
            b_.SetInsertPoint(has_bb);
            emit_active_push(b_, parent);
            b_.CreateBr(final_bb);
        }

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

    // Record each ref formal's passed actual pointer into ref_ptr_ (Local k == formal
    // k), so local_lvalue aliases the actual's storage live. Called in the callee
    // prologue of both functions and tasks. A ref formal takes no copy-in / copy-out.
    void bind_ref_formals(const Function& f) {
        for (uint32_t k = 0; k < f.param_types.size(); ++k) {
            if (!f.arg_dirs.empty() && f.arg_dirs[k] == ArgDir::Ref) {
                ref_ptr_[k] = fn_->getArg(param_llvm_index_[k]);
            }
        }
    }

    // Prologue (function and task): copy each formal's parameter into its frame
    // Local (Local k == formal k). Inputs read their value planes; inouts load
    // through their caller pointer; outputs keep the frame's default (X / 0) until
    // the body assigns them. Ref formals are bound separately (bind_ref_formals) and
    // skipped here.
    void emit_copyin(const Function& f) {
        for (uint32_t k = 0; k < f.param_types.size(); ++k) {
            // An aggregate formal is passed by pointer and copied in by
            // emit_fn_agg_copyin (functions only; aggregate task args are guarded).
            if (contains_struct(f.param_types[k])) {
                continue;
            }
            const uint32_t w    = width_of(f.param_types[k]);
            const uint32_t base = param_llvm_index_[k];
            if (f.arg_dirs[k] == ArgDir::Ref) {
                continue;
            }
            if (is_handle_type(f.param_types[k])) {
                // Deep copy the caller's handle into the frame handle for input /
                // inout (value semantics; init_frame left the slot empty). Output
                // keeps the empty frame — it is filled by the body, copied out at
                // return.
                if (f.arg_dirs[k] == ArgDir::In || f.arg_dirs[k] == ArgDir::Inout) {
                    emit_handle_assign(b_.CreateStructGEP(frame_ty_, frame_, k), fn_->getArg(base),
                                       f.param_types[k]);
                }
                continue;
            }
            if (f.arg_dirs[k] == ArgDir::In) {
                if (is_string_type(f.param_types[k])) {
                    // Deep copy the caller's handle into the frame local (value
                    // semantics; the frame slot was init'd by init_frame).
                    b_.CreateCall(str_assign_fn(),
                                  { b_.CreateStructGEP(frame_ty_, frame_, k), fn_->getArg(base) });
                    continue;
                }
                lower_store(local_lvalue(k), Plane{ fn_->getArg(base), fn_->getArg(base + 1) });
            } else if (f.arg_dirs[k] == ArgDir::Inout) {
                llvm::Value* mm = b_.CreateLoad(int_ty(2 * w), fn_->getArg(base));
                llvm::Value* z  = b_.CreateLoad(int_ty(w), fn_->getArg(base + 1));
                lower_store(local_lvalue(k), Plane{ mm, z });
            }
        }
    }

    // Copy each output / inout formal's frame Local back through its caller pointer
    // (LRM 13.5 copy-out at completion). For a task this runs once in the completion
    // epilogue; for a function it runs before every `ret` (each return point).
    void emit_copyout(const Function& f) {
        for (uint32_t k = 0; k < f.param_types.size(); ++k) {
            // In needs no copy-out; a ref formal is live (aliased), never copied back.
            if (f.arg_dirs[k] == ArgDir::In || f.arg_dirs[k] == ArgDir::Ref) {
                continue;
            }
            const uint32_t base = param_llvm_index_[k];
            // A dynamic-array / queue output / inout formal deep-copies its frame
            // handle back into the caller's handle (the single passed pointer).
            if (is_handle_type(f.param_types[k])) {
                emit_handle_assign(fn_->getArg(base), b_.CreateStructGEP(frame_ty_, frame_, k),
                                   f.param_types[k]);
                continue;
            }
            // The caller's out-target is still the packed word; lift a narrow local.
            const Plane    v    = to_four(lower_load(local_lvalue(k)));
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
            return sym(enqueue_fn_);
        }
        auto* ptr   = llvm::PointerType::getUnqual(ctx_);
        auto* fnty  = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr }, false);
        enqueue_fn_ = llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.enqueue_timer", env_->llmod);

        llvm::IRBuilder<> tb(ctx_);
        tb.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", enqueue_fn_));
        llvm::Value* hp  = tb.CreateStructGEP(awaiter_ty(), enqueue_fn_->getArg(0), 2);
        llvm::Value* hdl = tb.CreateLoad(ptr, hp);
        emit_active_push(tb, hdl);
        tb.CreateRetVoid();
        return sym(enqueue_fn_);
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
        // Schedule-relative: the runtime adds `now` so this #delay crosses the ABI
        // once (rt_wheel_schedule_rel) rather than twice (rt_wheel_now then
        // rt_wheel_schedule).
        b_.CreateCall(wheel_schedule_rel_fn(), { wheel_, aw, ticks, enqueue_trampoline() });
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
        emit_entry(wait_sta_ty_[i][edge], sym(wait_sta_gv_[i][edge]), b_.getInt32(slot), handle, flag_ptr);
    }

    // One-shot (intra-body @(sig)) registration: append this coroutine to the
    // signal's dynamic queue, bumping .dyn.len. No bounds check — capacity is the
    // count of distinct processes that intra-body-wait on the signal, and a process
    // holds at most one dynamic slot per signal at a time.
    void emit_dynamic_register(Signal sig, uint32_t edge) {
        const auto   i       = static_cast<uint32_t>(sig);
        llvm::Value* len_ptr = wait_base(wait_slot_len_[i][edge], wait_dyn_len_gv_[i][edge], i);
        llvm::Value* q       = wait_base(wait_slot_dyn_[i][edge], wait_dyn_gv_[i][edge], i);
        auto*        i32     = b_.getInt32Ty();
        llvm::Value* len     = b_.CreateLoad(i32, len_ptr);
        emit_entry(wait_dyn_ty_[i][edge], q, len, hdl_, sched_ptr_);
        b_.CreateStore(b_.CreateAdd(len, b_.getInt32(1)), len_ptr);
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

    // Run every `final` block once, in source order, at the end of simulation
    // (LRM 9.2.3). A final block is zero-time, so — like the pre-init pass — one
    // resume runs its body to completion; destroy reclaims the frame. Emitted as
    // a separate `sim_finals(state, wheel)` so rt_sim_run can invoke it on both
    // the natural-drain and $finish exit paths.
    void emit_finals_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
        auto* ptr_ty    = llvm::PointerType::getUnqual(ctx_);
        auto* proc_fnty = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
        const auto all  = mod_.processes();
        for (uint32_t g = 0; g < procs.size(); ++g) {
            if (all[groups_[g].front()].activation != Activation::Final) {
                continue;
            }
            llvm::Value* hdl = b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
            b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
            b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
        }
    }

    // -- Top-level entry --
    //
    // Emit `i32 @main()`: heap-allocate the shared state, power it up, build the
    // timing wheel, then run every process through the active task queue and drain
    // the wheel (advancing time, resuming delayed/woken processes) until idle,
    // then run the `final` blocks. Returns 0.
    void emit_main(const std::vector<llvm::Function*>& procs) {
        sched_anchor_ = SignalSlot::no_anchor; // startup code addresses absolutely
        auto* ptr_ty  = llvm::PointerType::getUnqual(ctx_);
        auto* subfnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty, ptr_ty }, false);

        // sim_drain(state, wheel): seed every non-final process and run the event
        // loop to quiescence. Split out of main so rt_sim_run can wrap it in the
        // $finish catch — a $finish longjmps back into rt_sim_run, which sits below
        // main's frame, so the heap state pointer and the wheel alloca stay live
        // and the final blocks still observe the settled design state.
        auto* drain = llvm::Function::Create(subfnty, llvm::Function::InternalLinkage, "sim_drain", env_->llmod);
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", drain));
        emit_active_run(drain->getArg(0), drain->getArg(1), procs);
        b_.CreateRetVoid();

        // sim_finals(state, wheel): run every `final` block once.
        auto* finals = llvm::Function::Create(subfnty, llvm::Function::InternalLinkage, "sim_finals", env_->llmod);
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", finals));
        emit_finals_run(finals->getArg(0), finals->getArg(1), procs);
        b_.CreateRetVoid();

        auto* i32  = llvm::Type::getInt32Ty(ctx_);
        auto* fnty = llvm::FunctionType::get(i32, false);
        auto* main = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, "main", env_->llmod);

        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", main);
        b_.SetInsertPoint(entry);

        // The design state struct holds every signal, including large memories.
        // It is a single internal global whose initializer is the whole power-on
        // state (`state_initializer`) — the optimizer can then constant-fold the
        // fixed base into every signal GEP, and no per-signal init store is
        // emitted (only arrays keep a runtime fill loop; see `init_state`).
        // 64-byte aligned to cover any packed-vector alignment in the struct.
        // Created up front by emit_state_global (bodies address it through the
        // instance anchors); reused-definition anchor bases are stored here,
        // before init_state and any process, so every later load sees them.
        llvm::Value* state = sym(state_global_);
        const auto ctx_ptr = [&](Anchor& a, uint32_t field) {
            return b_.CreateInBoundsGEP(a.ctx_ty, sym(a.ctx_gv), { b_.getInt32(0), b_.getInt32(field) });
        };
        for (Anchor& a : anchors_) {
            if (a.ctx_gv == nullptr) {
                continue;
            }
            b_.CreateStore(sym(a.state_gv), ctx_ptr(a, 0));
            if (a.shadow_ty != nullptr) {
                b_.CreateStore(sym(a.shadow_gv), ctx_ptr(a, 1));
            }
            if (a.sched_gv != nullptr) {
                b_.CreateStore(sym(a.sched_gv), ctx_ptr(a, 2));
            }
            if (a.first_vcd >= 0) {
                b_.CreateStore(b_.getInt32(static_cast<uint32_t>(a.first_vcd)), ctx_ptr(a, 3));
            }
            for (const Signal sj : anchor_signals_[static_cast<size_t>(&a - anchors_.data())]) {
                if (sj == Signal{ ~0u }) {
                    continue;
                }
                const auto si = static_cast<uint32_t>(sj);
                for (uint32_t e = 0; e < 3; ++e) {
                    if (wait_slot_sta_[si][e] != no_wait_slot) {
                        b_.CreateStore(sym(wait_sta_gv_[si][e]),
                                       ctx_ptr(a, a.ctx_wait_base + wait_slot_sta_[si][e]));
                    }
                    if (wait_slot_dyn_[si][e] != no_wait_slot) {
                        b_.CreateStore(sym(wait_dyn_gv_[si][e]),
                                       ctx_ptr(a, a.ctx_wait_base + wait_slot_dyn_[si][e]));
                        b_.CreateStore(sym(wait_dyn_len_gv_[si][e]),
                                       ctx_ptr(a, a.ctx_wait_base + wait_slot_len_[si][e]));
                    }
                }
            }
        }
        for (const auto& [aid, field, first_id] : ctx_blk_fills_) {
            b_.CreateStore(b_.getInt32(first_id), ctx_ptr(anchors_[aid], field));
        }
        init_state();
        // Shadow bases fill only when some shadow access loaded them (lazy):
        // the root base covers unscoped targets, the per-instance bases each
        // scope's unshared accesses; shareable reused-definition accesses go
        // through the ctx fills above instead.
        if (shadow_global_ != nullptr && shadow_base_gv_ != nullptr) {
            b_.CreateStore(sym(shadow_global_), sym(shadow_base_gv_));
        }
        for (Anchor& a : anchors_) {
            if (a.shadow_base_gv != nullptr) {
                b_.CreateStore(sym(a.shadow_gv), sym(a.shadow_base_gv));
            }
        }

        // Register the single DPI scope (svGetScope / user-data, LRM 35.5.3) before
        // any process runs, so a context import can read it. Only when DPI is used.
        if (design_uses_dpi()) {
            b_.CreateCall(rt_fn("rt_svdpi_scope_init", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() }),
                          { b_.CreateGlobalString(mod_.top_name),
                            b_.getInt32(static_cast<uint32_t>(mod_.top_name.size())) });
        }

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

        // Run the drain under the $finish catch, then the final blocks. Both exit
        // paths (natural drain, $finish) return here with `state` still live.
        b_.CreateCall(sim_run_fn(), { state, wheel, drain, finals });

        // Release heap owned by string signals. Runs on both exit paths now (the
        // $finish catch returns here); a $finish inside a final block still
        // longjmps past this, its heap reclaimed at process exit.
        for (const SignalSlot& s : slots_) {
            if (s.is_string) {
                emit_string_slot_op(s, str_deinit_fn());
            } else if (s.is_dynarr || s.is_queue) {
                emit_handle_delete(storage_ptr(s), s.ir_type);
            } else if (s.is_agg && has_heap_leaf(s.ir_type)) {
                // Aggregate signal with string / handle leaves: recursive
                // teardown of every heap-owning leaf.
                std::vector<llvm::Value*> path;
                emit_agg_deinit(s.ir_type, storage_ptr(s), path, s.storage_ty);
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
        // Static capacities count what the seed loop will actually subscribe:
        // one entry per group (signal, edge) — a fused group subscribes its
        // canonical set once, not once per member. The scan reads every slot
        // up to the capacity, so over-allocation would read a null flag ptr.
        for (const std::vector<WaitEvent>& evs : group_events_) {
            for (const WaitEvent& w : evs) {
                c.sta[static_cast<uint32_t>(w.signal)][static_cast<uint32_t>(w.edge)]++;
            }
        }
        for (const Process& p : mod_.processes()) {
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
                for (const WaitEdge& we : p.ir.wait_edges(p.ir.payload(idx).extra)) {
                    const std::pair<Signal, uint32_t> key{ we.signal, we.edge };
                    if (!std::ranges::contains(seen, key)) {
                        seen.push_back(key);
                        c.dyn[static_cast<uint32_t>(we.signal)][we.edge]++;
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
                    for (const WaitEdge& we : f.ir.wait_edges(f.ir.payload(idx).extra)) {
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
        const auto sigs = mod_.signals();
        WaitCounts c    = wait_counts();

        // Module-dedup prep, option (a): every instance of a reused definition
        // gets the per-def MAXIMUM bucket capacity for each def-local signal,
        // so twin bodies emit identical fixed-trip service scans regardless of
        // per-instance fan-out (subscribers cross instance boundaries through
        // ports). A padded slot is zeroinit — null flag pointer — and the
        // inline scan skips it (emit_static_scan_rt `padded`). The true
        // per-instance counts are kept for the NBA meta table, whose
        // commit-side scans stay exact and check-free.
        wait_sta_true_.assign(sigs.size(), { 0, 0, 0 });
        wait_sta_padded_.assign(sigs.size(), { 0, 0, 0 });
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            wait_sta_true_[i] = c.sta[i];
        }
        // Def-local signal table per scope occurrence: slot.path's last
        // element is the signal's field index within its own scope. Shared by
        // the capacity padding below, the instance-context layout, and block
        // translation (anchor_signals_).
        anchor_signals_.assign(anchors_.size(), {});
        std::vector<std::vector<Signal>>& by_anchor = anchor_signals_;
        {
            for (uint32_t i = 0; i < sigs.size(); ++i) {
                const SignalSlot& sl = slots_[i];
                if (sl.anchor == SignalSlot::no_anchor) {
                    continue;
                }
                const auto k = sl.path.back();
                auto&      v = by_anchor[sl.anchor];
                if (k >= v.size()) {
                    v.resize(k + 1, Signal{ ~0u });
                }
                v[k] = static_cast<Signal>(i);
            }
            ankerl::unordered_dense::map<llvm::StructType*, std::vector<uint32_t>> groups;
            for (uint32_t a = 0; a < anchors_.size(); ++a) {
                groups[anchors_[a].ty].push_back(a);
            }
            for (const auto& [ty, members] : groups) {
                if (members.size() < 2) {
                    continue;
                }
                const size_t nsig = by_anchor[members.front()].size();
                for (size_t j = 0; j < nsig; ++j) {
                    for (uint32_t e = 0; e < 3; ++e) {
                        uint32_t max_sta = 0, max_dyn = 0;
                        uint32_t min_sta = ~0u;
                        for (const uint32_t a : members) {
                            const auto si = static_cast<uint32_t>(by_anchor[a][j]);
                            max_sta       = std::max(max_sta, c.sta[si][e]);
                            min_sta       = std::min(min_sta, c.sta[si][e]);
                            max_dyn       = std::max(max_dyn, c.dyn[si][e]);
                        }
                        // The padded (null-checked) scan shape must be uniform
                        // across every instance of the def — including the one
                        // whose true count equals the maximum — or twin bodies
                        // would still diverge.
                        for (const uint32_t a : members) {
                            const auto si = static_cast<uint32_t>(by_anchor[a][j]);
                            wait_sta_padded_[si][e] = min_sta < max_sta ? 1 : 0;
                            c.sta[si][e]            = max_sta;
                            c.dyn[si][e]            = max_dyn;
                        }
                    }
                }
            }
        }
        wait_sta_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        wait_sta_ty_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        wait_sta_cap_.assign(sigs.size(), { 0, 0, 0 });
        wait_dyn_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        wait_dyn_ty_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        wait_dyn_len_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
        sta_next_.assign(sigs.size(), { 0, 0, 0 });
        event_trig_gv_.assign(sigs.size(), nullptr);
        event_pending_gv_.assign(sigs.size(), nullptr);

        auto* i8   = llvm::Type::getInt8Ty(ctx_);
        auto* i32  = llvm::Type::getInt32Ty(ctx_);
        auto* i64  = llvm::Type::getInt64Ty(ctx_);
        auto* elem = wait_elem_ty();
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            if (sigs[i].is_event) {
                // Never-triggered sentinel: UINT64_MAX never equals a real tick,
                // so `.triggered` reads 0 until the first `-> e`.
                event_trig_gv_[i] = new llvm::GlobalVariable(
                  env_->llmod, i64, false, llvm::GlobalValue::InternalLinkage,
                  llvm::ConstantInt::get(i64, ~uint64_t{ 0 }),
                  "clotho.event_trig." + sigs[i].name);
                // Nonblocking-trigger pending flag (`->> e`), cleared in the NBA
                // region. Allocated for every event; the NBA scan only visits the
                // ones actually `->>`-triggered (nb_event_sigs_).
                event_pending_gv_[i] = new llvm::GlobalVariable(
                  env_->llmod, i8, false, llvm::GlobalValue::InternalLinkage,
                  llvm::ConstantInt::get(i8, 0), "clotho.event_pending." + sigs[i].name);
            }
            for (uint32_t e = 0; e < 3; ++e) {
                const std::string base = "clotho.wait." + sigs[i].name + "." ;
                if (c.sta[i][e] > 0) {
                    auto* arr           = llvm::ArrayType::get(elem, c.sta[i][e]);
                    wait_sta_ty_[i][e]  = arr;
                    wait_sta_cap_[i][e] = c.sta[i][e];
                    wait_sta_gv_[i][e]  = new llvm::GlobalVariable(
                      env_->llmod, arr, false, llvm::GlobalValue::InternalLinkage,
                      llvm::ConstantAggregateZero::get(arr), base + "sta." + edge_suffix(e));
                }
                if (c.dyn[i][e] > 0) {
                    auto* arr          = llvm::ArrayType::get(elem, c.dyn[i][e]);
                    wait_dyn_ty_[i][e] = arr;
                    wait_dyn_gv_[i][e] = new llvm::GlobalVariable(
                      env_->llmod, arr, false, llvm::GlobalValue::InternalLinkage,
                      llvm::ConstantAggregateZero::get(arr), base + "dyn." + edge_suffix(e));
                    wait_dyn_len_gv_[i][e] = new llvm::GlobalVariable(
                      env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                      llvm::ConstantInt::get(i32, 0), base + "dyn." + edge_suffix(e) + ".len");
                }
            }
        }

        // Def-local wait-bucket slots (module-dedup prep): for a reused
        // definition, body-side queue access goes through a def-local slot in
        // the instance context (ctx_wait_base + slot) instead of a per-instance
        // queue global. Slot numbering follows the def-local signal walk and
        // the (padded, so def-uniform) bucket existence — identical across
        // twin instances by construction. main fills the pointers at startup.
        wait_slot_sta_.assign(sigs.size(), { no_wait_slot, no_wait_slot, no_wait_slot });
        wait_slot_dyn_.assign(sigs.size(), { no_wait_slot, no_wait_slot, no_wait_slot });
        wait_slot_len_.assign(sigs.size(), { no_wait_slot, no_wait_slot, no_wait_slot });
        for (uint32_t a = 0; a < anchors_.size(); ++a) {
            if (!anchors_[a].indirect) {
                continue;
            }
            uint32_t next = 0;
            for (const Signal sj : by_anchor[a]) {
                if (sj == Signal{ ~0u }) {
                    continue;
                }
                const auto si = static_cast<uint32_t>(sj);
                for (uint32_t e = 0; e < 3; ++e) {
                    if (wait_sta_gv_[si][e] != nullptr) {
                        wait_slot_sta_[si][e] = next++;
                    }
                    if (wait_dyn_gv_[si][e] != nullptr) {
                        wait_slot_dyn_[si][e] = next++;
                        wait_slot_len_[si][e] = next++;
                    }
                }
            }
        }
    }

    // Compute each instance context's type (see Anchor): shared per definition,
    // fields sized from the def-local single-scope group count and wait-bucket
    // slots. Runs after fusion grouping and wait-queue sizing, before lowering.
    // Also attributes each fusion group to its scope anchor + def-local
    // ordinal (the shareable-group identity every def-relative site keys on).
    void build_instance_ctx_layout() {
        const auto all = mod_.processes();
        group_anchor_of_.assign(groups_.size(), SignalSlot::no_anchor);
        group_def_ordinal_.assign(groups_.size(), ~0u);
        group_unit_anchor_.assign(groups_.size(), SignalSlot::no_anchor);
        std::vector<uint32_t> scope_counter(anchors_.size(), 0);
        for (uint32_t g = 0; g < groups_.size(); ++g) {
            const uint32_t sc   = all[groups_[g].front()].scope;
            bool           same = sc != Process::no_scope;
            for (const uint32_t m : groups_[g]) {
                same = same && all[m].scope == sc;
            }
            if (!same || sc >= anchor_of_scope_.size() ||
                anchor_of_scope_[sc] == SignalSlot::no_anchor) {
                continue;
            }
            const uint32_t aid = anchor_of_scope_[sc];
            // Any single-scope group belongs to its definition's emission
            // unit (split mode), whether or not the definition is reused.
            group_unit_anchor_[g] = aid;
            if (!anchors_[aid].indirect || anchors_[aid].first_proc == ~0u) {
                continue;
            }
            group_anchor_of_[g]   = aid;
            group_def_ordinal_[g] = scope_counter[aid]++;
        }
        // Twin instances see the same def-local group count; size blk fields by
        // the definition-wide maximum so layouts agree even if a member's group
        // list degenerated (they should be equal by construction).
        ankerl::unordered_dense::map<llvm::StructType*, uint32_t> def_groups;
        for (uint32_t a = 0; a < anchors_.size(); ++a) {
            auto& n = def_groups[anchors_[a].ty];
            n       = std::max(n, scope_counter[a]);
        }

        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        auto* i32 = b_.getInt32Ty();
        ankerl::unordered_dense::map<llvm::StructType*, llvm::StructType*> def_ctx;
        for (Anchor& a : anchors_) {
            if (!a.indirect) {
                continue;
            }
            a.n_groups     = def_groups[a.ty];
            a.ctx_blk_base = 4;
            uint32_t nwait = 0;
            for (const Signal sj : anchor_signals_[&a - anchors_.data()]) {
                if (sj == Signal{ ~0u }) {
                    continue;
                }
                const auto si = static_cast<uint32_t>(sj);
                for (uint32_t e = 0; e < 3; ++e) {
                    nwait += wait_slot_sta_[si][e] != no_wait_slot ? 1 : 0;
                    nwait += wait_slot_dyn_[si][e] != no_wait_slot ? 2 : 0;
                }
            }
            a.ctx_wait_base = a.ctx_blk_base + a.n_groups;
            const auto [it, fresh] = def_ctx.try_emplace(a.ty, nullptr);
            if (fresh) {
                std::vector<llvm::Type*> f{ ptr, ptr, ptr, i32 };
                f.insert(f.end(), a.n_groups, i32);
                f.insert(f.end(), nwait, ptr);
                it->second = llvm::StructType::create(
                  ctx_, f, a.ty->getName().str() + ".ctx");
            }
            a.ctx_ty = it->second;
        }
    }

    // Static-subscription order: a topological sort of processes over the
    // blocking-write wake graph (edge P -> Q iff P blocking-writes a signal in
    // Q's sensitivity list). Wait-queue bucket entries fill in subscription
    // order and the FIFO drain pops in bucket order, so subscribing in this
    // order runs producers before consumers within a settle wave: an acyclic
    // blocking cascade settles in one wave regardless of source order.
    //
    // NBA writes are deliberately not edges — a commit lands between drains and
    // starts a fresh wave, so they cannot cause same-wave re-evaluation, and
    // excluding them removes the clocked feedback that would otherwise make
    // every sequential design one big SCC.
    //
    // Cycles (combinational loops): Tarjan SCC + condensation order, so only
    // the processes inside a loop keep declaration order among themselves;
    // everything downstream of the loop is still placed after it. Convergence
    // is unaffected either way — re-wake-on-change still iterates SCCs.
    void compute_subscription_order() {
        const auto     all    = mod_.processes();
        const auto     nprocs = static_cast<uint32_t>(all.size());
        const uint32_t nsigs  = static_cast<uint32_t>(mod_.signals().size());
        sub_order_.resize(nprocs);
        for (uint32_t k = 0; k < nprocs; ++k) {
            sub_order_[k] = k;
        }
        wake_adj_.assign(nprocs, {});
        scc_nontrivial_.assign(nprocs, 0);
        if (nprocs < 2) {
            return;
        }

        // Blocking-written signals of one IR body: Store roots plus out/inout
        // TaskCall actuals (copy-out writes the actual in the caller). Roots
        // that are not signals (function locals / formals) are skipped.
        const auto body_writes = [&](const Ir& ir, std::vector<Signal>& out, std::vector<FunctionId>& calls) {
            for (size_t k = 0; k < ir.inst_count(); ++k) {
                const auto idx = static_cast<Index>(k);
                if (ir.tag(idx) == Inst::Tag::Store) {
                    const LvalueRoot r = lvalue_root(ir, ir.payload(idx).binary.l);
                    if (ir.tag(r.root) == Inst::Tag::Signal) {
                        out.push_back(ir.payload(r.root).signal);
                    }
                }
                else if (ir.tag(idx) == Inst::Tag::TaskCall) {
                    const auto      ti     = ir.extra_data<TaskCallInfo>(ir.payload(idx).extra);
                    const Function& callee = mod_.function(ti.func);
                    const auto      args   = ir.region(ti.args);
                    for (size_t a = 0; a < args.size(); ++a) {
                        if (callee.arg_dirs[a] == ArgDir::In) {
                            continue;
                        }
                        const LvalueRoot r = lvalue_root(ir, args[a]);
                        if (ir.tag(r.root) == Inst::Tag::Signal) {
                            out.push_back(ir.payload(r.root).signal);
                        }
                    }
                    calls.push_back(ti.func);
                }
            }
        };

        // Per-function write sets, closed transitively over calls (memoized
        // DFS; an in-progress callee — recursion — contributes what it has so
        // far, an over- or under-approximation that only perturbs ordering).
        const auto                       nf = static_cast<uint32_t>(mod_.function_count());
        std::vector<std::vector<Signal>> fn_writes(nf);
        std::vector<uint8_t>             fn_state(nf, 0); // 0 new, 1 visiting, 2 done
        const auto close_fn = [&](this const auto& self, uint32_t f) -> void {
            if (fn_state[f] != 0) {
                return;
            }
            fn_state[f] = 1;
            std::vector<FunctionId> calls;
            body_writes(mod_.function(static_cast<FunctionId>(f)).ir, fn_writes[f], calls);
            for (const FunctionId c : calls) {
                self(static_cast<uint32_t>(c));
                const auto& cw = fn_writes[static_cast<uint32_t>(c)];
                fn_writes[f].insert(fn_writes[f].end(), cw.begin(), cw.end());
            }
            fn_state[f] = 2;
        };

        // Readers: processes statically subscribed to each signal (any edge).
        std::vector<std::vector<uint32_t>> readers(nsigs);
        for (uint32_t k = 0; k < nprocs; ++k) {
            if (all[k].activation != Activation::Always) {
                continue;
            }
            for (const WaitEvent& w : all[k].wait_events) {
                readers[static_cast<uint32_t>(w.signal)].push_back(k);
            }
        }

        // Adjacency, deduplicated with a per-source stamp. Kept as a member:
        // the comb-fusion pass reuses these edges for component grouping.
        std::vector<std::vector<uint32_t>>& adj = wake_adj_;
        std::vector<uint32_t>               stamp(nprocs, UINT32_MAX);
        for (uint32_t k = 0; k < nprocs; ++k) {
            std::vector<Signal>     writes;
            std::vector<FunctionId> calls;
            body_writes(all[k].ir, writes, calls);
            for (const FunctionId c : calls) {
                close_fn(static_cast<uint32_t>(c));
                const auto& cw = fn_writes[static_cast<uint32_t>(c)];
                writes.insert(writes.end(), cw.begin(), cw.end());
            }
            for (const Signal s : writes) {
                for (const uint32_t q : readers[static_cast<uint32_t>(s)]) {
                    if (q == k || stamp[q] == k) {
                        continue;
                    }
                    stamp[q] = k;
                    adj[k].push_back(q);
                }
            }
        }

        // Iterative Tarjan. SCCs complete in reverse condensation-topo order
        // (an SCC finishes only after everything it reaches), so assigning
        // descending ranks as they complete numbers SCCs in topo order.
        std::vector<uint32_t> scc_rank(nprocs, 0);
        uint32_t              next_rank = nprocs; // descending as SCCs complete
        {
            constexpr uint32_t    none = UINT32_MAX;
            std::vector<uint32_t> index(nprocs, none);
            std::vector<uint32_t> low(nprocs, 0);
            std::vector<uint8_t>  on_stack(nprocs, 0);
            std::vector<uint32_t> stack;
            uint32_t              next_index = 0;
            struct Frame {
                uint32_t v;
                uint32_t child;
            };
            std::vector<Frame> dfs;
            for (uint32_t root = 0; root < nprocs; ++root) {
                if (index[root] != none) {
                    continue;
                }
                dfs.push_back({ root, 0 });
                index[root] = low[root] = next_index++;
                stack.push_back(root);
                on_stack[root] = 1;
                while (!dfs.empty()) {
                    Frame& f = dfs.back();
                    if (f.child < adj[f.v].size()) {
                        const uint32_t w = adj[f.v][f.child++];
                        if (index[w] == none) {
                            index[w] = low[w] = next_index++;
                            stack.push_back(w);
                            on_stack[w] = 1;
                            dfs.push_back({ w, 0 });
                        }
                        else if (on_stack[w]) {
                            low[f.v] = std::min(low[f.v], index[w]);
                        }
                        continue;
                    }
                    const uint32_t v = f.v;
                    dfs.pop_back();
                    if (!dfs.empty()) {
                        low[dfs.back().v] = std::min(low[dfs.back().v], low[v]);
                    }
                    if (low[v] == index[v]) {
                        --next_rank;
                        uint32_t w;
                        do {
                            w = stack.back();
                            stack.pop_back();
                            on_stack[w] = 0;
                            scc_rank[w] = next_rank;
                        } while (w != v);
                    }
                }
            }
        }

        // Nontrivial SCC membership (multi-process combinational loop): those
        // processes have no producer-before-consumer order, so comb fusion
        // must leave them on the re-wake iteration path.
        {
            std::vector<uint32_t> scc_size(nprocs, 0);
            for (uint32_t k = 0; k < nprocs; ++k) {
                scc_size[scc_rank[k]]++;
            }
            for (uint32_t k = 0; k < nprocs; ++k) {
                scc_nontrivial_[k] = scc_size[scc_rank[k]] > 1 ? 1 : 0;
            }
        }

        // Order by longest-path level in the condensation, not by raw SCC
        // number: distinct SCC numbers would also order UNRELATED processes by
        // arbitrary DFS completion, gratuitously shuffling them against
        // declaration order (measured as a few percent of extra comb
        // re-evaluation on picorv32). Levels give every process the earliest
        // slot its producers allow; everything else keeps declaration order
        // via the stable sort.
        const uint32_t        nscc = nprocs - next_rank;
        const auto            dense = [&](uint32_t p) { return scc_rank[p] - next_rank; };
        std::vector<uint32_t> level(nscc, 0);
        std::vector<uint32_t> by_rank = sub_order_;
        std::stable_sort(by_rank.begin(), by_rank.end(),
                         [&](uint32_t a, uint32_t b) { return scc_rank[a] < scc_rank[b]; });
        // Cross-SCC edges always point to a higher rank (the successor SCC
        // completed first), so one ascending pass relaxes in topo order.
        for (const uint32_t u : by_rank) {
            for (const uint32_t v : adj[u]) {
                if (dense(v) != dense(u)) {
                    level[dense(v)] = std::max(level[dense(v)], level[dense(u)] + 1);
                }
            }
        }
        std::stable_sort(sub_order_.begin(), sub_order_.end(),
                         [&](uint32_t a, uint32_t b) { return level[dense(a)] < level[dense(b)]; });
    }

    // True when a process is a suspension-free always-with-sensitivity: an
    // Always with a static sensitivity list whose body has no suspension point
    // (#delay, intra-body @/wait, task call) and no blocking write to a signal
    // in its own sensitivity set. Such a process re-runs its whole body on each
    // trigger with no state carried across activations, so it can lower to a
    // plain per-activation thunk instead of a coroutine (see the thunk path in
    // lower_group) and can be fused with peers sharing its sensitivity. Shared
    // by compute_fusion_groups and the thunk decision.
    // True when instruction `i` in `ir` is an array shuffle (DynArrayShuffle /
    // QueueShuffle), which draws from the process frame RNG like $urandom.
    static bool is_shuffle(const Ir& ir, Index i) {
        const Inst::Tag tag = ir.tag(i);
        return tag == Inst::Tag::DynArrayShuffle || tag == Inst::Tag::QueueShuffle;
    }

    // True when a process body contains any node that draws from the per-process
    // frame RNG ($urandom family or an array shuffle), so lower_group must
    // materialize and seed the frame RNG.
    static bool process_uses_urandom(const Process& p) {
        for (size_t k = 0; k < p.ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            switch (p.ir.tag(idx)) {
                case Inst::Tag::Urandom:
                case Inst::Tag::UrandomSeeded:
                case Inst::Tag::UrandomRange:
                    return true;
                default:
                    if (is_shuffle(p.ir, idx)) {
                        return true;
                    }
                    break;
            }
        }
        return false;
    }

    bool is_simple_always(const Process& p) const {
        if (p.activation != Activation::Always || p.wait_events.empty()) {
            return false;
        }
        for (size_t k = 0; k < p.ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            switch (p.ir.tag(idx)) {
                case Inst::Tag::Delay:
                case Inst::Tag::Wait:
                case Inst::Tag::TaskCall:
                // A named-event trigger has a scheduling side effect (it wakes
                // waiters) and `.triggered` reads per-event state outside the
                // pure-function-of-signal-reads model that thunk elision and comb
                // fusion assume, so keep an event-touching process a coroutine.
                case Inst::Tag::EventTrigger:
                case Inst::Tag::EventTriggerNb:
                case Inst::Tag::EventTriggerNbDelayed:
                case Inst::Tag::EventTriggered:
                case Inst::Tag::Fork:
                    return false;
                // The $urandom family keeps its per-process RNG state in the
                // coroutine frame (LRM §18.13 thread stability), so a thunk (no
                // frame) cannot host it — force a coroutine. $random / $dist_*
                // carry their state in an explicit seed variable or the global
                // stream, so they stay thunk/fusion eligible.
                case Inst::Tag::Urandom:
                case Inst::Tag::UrandomSeeded:
                case Inst::Tag::UrandomRange:
                    return false;
                // An array shuffle draws from the frame RNG (same reason as the
                // $urandom family); other dynarr/queue ops carry no frame state
                // and stay thunk-eligible, so only Shuffle disqualifies.
                case Inst::Tag::DynArrayShuffle:
                case Inst::Tag::QueueShuffle:
                    return false;
                // A deferred store (`x <= #d y` / `x <= @e y`) arms a record with an
                // observable future effect; keep it out of thunk/fused groups (a
                // spurious comb-fused run would arm a spurious store), routing it
                // to a plain coroutine.
                case Inst::Tag::StoreNbDelay:
                case Inst::Tag::StoreNbEvent:
                    return false;
                case Inst::Tag::Store: {
                    const LvalueRoot r = lvalue_root(p.ir, p.ir.payload(idx).binary.l);
                    if (p.ir.tag(r.root) != Inst::Tag::Signal) {
                        break;
                    }
                    // Membership in the raw wait list is identical to membership
                    // in the deduped canonical set.
                    const auto sig = static_cast<uint32_t>(p.ir.payload(r.root).signal);
                    for (const WaitEvent& w : p.wait_events) {
                        if (static_cast<uint32_t>(w.signal) == sig) {
                            return false;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return true;
    }

    // Comb-fusion eligibility (analysis only — grouping lands with the comb
    // fusion phase). A member of a union-sensitivity fused group can be run
    // when its own trigger did NOT fire. Such a spurious run is unobservable —
    // hence LRM-legal under the as-if rule — only when the body is a pure
    // deterministic function of its signal reads and every write is
    // change-detected: unchanged reads recompute unchanged values, and
    // service_wait_queue skips the wake scan when old == new, so nothing
    // downstream can tell the run happened. The verdict enumerates exactly the
    // conditions that argument needs; purity is a whitelist, so a new IR tag
    // defaults to ineligible.
    enum class CombFuse : uint8_t {
        Eligible,
        // Fails is_simple_always: suspension point, task call, self-sens
        // write, or not an Always with a static sensitivity.
        NotSimpleAlways,
        // Sensitivity has a posedge/negedge entry. Comb fusion is level-only;
        // edge-triggered members belong to V1 identical-sens fusion.
        EdgeSensitive,
        // Body op with an observable effect or non-deterministic value
        // ($display, file I/O, $random, $time, string ops, ...).
        ImpureOp,
        // Function call: purity closure over callees not implemented yet.
        HasCall,
        // Nonblocking store: commits in the NBA region, outside the group's
        // execution, which breaks the internal-signal subscription reasoning.
        NbaWrite,
        // Store whose root is a string or unpacked array: those store paths'
        // change detection is not verified, so the no-change argument does
        // not (yet) cover them.
        UnsupportedWrite,
        // Reads a signal absent from the sensitivity list: a spurious run
        // would observe a value the unfused schedule never samples (e.g.
        // explicit `@(a)` reading `b`).
        ReadOutsideSens,
        // Count of verdicts, for histogram sizing.
        Count,
    };

    CombFuse comb_fuse_verdict(const Process& p) const {
        if (!is_simple_always(p)) {
            return CombFuse::NotSimpleAlways;
        }
        for (const WaitEvent& w : p.wait_events) {
            if (w.edge != Edge::Any) {
                return CombFuse::EdgeSensitive;
            }
        }
        std::vector<uint32_t> reads;
        for (size_t k = 0; k < p.ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            switch (p.ir.tag(idx)) {
                // Pure value and control ops: no effect beyond their result.
                case Inst::Tag::Constant:
                case Inst::Tag::Arg:
                case Inst::Tag::Signal:
                case Inst::Tag::Local:
                case Inst::Tag::BitSelect:
                case Inst::Tag::PartSelect:
                case Inst::Tag::DynPartSelect:
                case Inst::Tag::ElemSelect:
                case Inst::Tag::Add:
                case Inst::Tag::Sub:
                case Inst::Tag::Mul:
                case Inst::Tag::Div:
                case Inst::Tag::Mod:
                case Inst::Tag::Pow:
                case Inst::Tag::Band:
                case Inst::Tag::Bor:
                case Inst::Tag::Bxor:
                case Inst::Tag::Bxnor:
                case Inst::Tag::Bnot:
                case Inst::Tag::RedAnd:
                case Inst::Tag::RedOr:
                case Inst::Tag::RedXor:
                case Inst::Tag::Ctz:
                case Inst::Tag::Clz:
                case Inst::Tag::CountBits:
                case Inst::Tag::OneHot:
                case Inst::Tag::OneHot0:
                case Inst::Tag::IsUnknown:
                case Inst::Tag::Sll:
                case Inst::Tag::Srl:
                case Inst::Tag::Sra:
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
                case Inst::Tag::InsideSet:
                case Inst::Tag::FAdd:
                case Inst::Tag::FSub:
                case Inst::Tag::FMul:
                case Inst::Tag::FDiv:
                case Inst::Tag::FPow:
                case Inst::Tag::FNeg:
                case Inst::Tag::FEq:
                case Inst::Tag::FNe:
                case Inst::Tag::FLt:
                case Inst::Tag::FLe:
                case Inst::Tag::FGt:
                case Inst::Tag::FGe:
                case Inst::Tag::ItoF:
                case Inst::Tag::FtoI:
                case Inst::Tag::FtoITrunc:
                case Inst::Tag::FBitcast:
                case Inst::Tag::FResize:
                case Inst::Tag::RealMathUnary:
                case Inst::Tag::RealMathBinary:
                case Inst::Tag::Land:
                case Inst::Tag::Lor:
                case Inst::Tag::Lnot:
                case Inst::Tag::Select:
                case Inst::Tag::SelectLazy:
                case Inst::Tag::Concat:
                case Inst::Tag::Repl:
                case Inst::Tag::StreamReverse:
                case Inst::Tag::Resize:
                case Inst::Tag::Cond:
                case Inst::Tag::Switch:
                case Inst::Tag::Loop:
                case Inst::Tag::Break:
                case Inst::Tag::Continue:
                case Inst::Tag::Clog2:
                    break;
                case Inst::Tag::Load: {
                    const LvalueRoot r = lvalue_root(p.ir, p.ir.payload(idx).unary);
                    if (p.ir.tag(r.root) == Inst::Tag::Signal) {
                        reads.push_back(static_cast<uint32_t>(p.ir.payload(r.root).signal));
                    }
                    break;
                }
                case Inst::Tag::Store: {
                    const LvalueRoot r = lvalue_root(p.ir, p.ir.payload(idx).binary.l);
                    if (p.ir.tag(r.root) != Inst::Tag::Signal) {
                        break;
                    }
                    const auto  sig = static_cast<uint32_t>(p.ir.payload(r.root).signal);
                    const auto& ty  = std::get<Type>(mod_.pool.get(mod_.signals()[sig].type));
                    if (ty.kind == Type::Kind::String || ty.kind == Type::Kind::UnpackedArray) {
                        return CombFuse::UnsupportedWrite;
                    }
                    break;
                }
                case Inst::Tag::StoreNb:
                case Inst::Tag::StoreNbDelay:
                case Inst::Tag::StoreNbEvent:
                    return CombFuse::NbaWrite;
                case Inst::Tag::Call:
                    return CombFuse::HasCall;
                default:
                    return CombFuse::ImpureOp;
            }
        }
        // Every read signal must be in the sensitivity list, else a spurious
        // run observes state the unfused schedule never samples. Verified here
        // rather than trusting the frontend's inference: explicit lists and
        // any inference gap fail mechanically.
        for (const uint32_t s : reads) {
            bool in_sens = false;
            for (const WaitEvent& w : p.wait_events) {
                if (static_cast<uint32_t>(w.signal) == s) {
                    in_sens = true;
                    break;
                }
            }
            if (!in_sens) {
                return CombFuse::ReadOutsideSens;
            }
        }
        return CombFuse::Eligible;
    }

    // Comb-fusion coverage probe: verdict histogram over Always processes plus
    // the count V1 fusion left in singleton groups (the comb-fusion
    // opportunity). Measurement only, printed when CLOTHO_COMB_FUSE_STAT is
    // set; the grouping itself is not landed.
    // Module-deduplication detector (CLOTHO_DEDUP_STAT): canonicalize each
    // attributed process body to definition-relative form and report how many
    // bodies across instances of reused definitions are byte-identical — the
    // shareable population one compiled body per definition would cover, and
    // what blocks the rest. Read-only; sharing itself is future work.
    //
    // Canonical form: every payload-level Signal reference under the process's
    // home instance subtree is rewritten to its def-relative field path; a
    // reference outside the subtree stays absolute (an unscoped/package signal
    // is the same signal for every instance, so twins still match on it; a
    // cross-instance hierarchical reference differs per instance and honestly
    // blocks matching). V1 keeps the extra buffer raw, so a body carrying a
    // signal inside an extra record (Wait / StoreNbEvent / ReadMem / FreadMem /
    // EventTriggerNbDelayed) or a FunctionId (per-instance function copies)
    // hashes instance-specific — counted as blockers, not miscounted as twins.
    void report_dedup_stat() {
        struct Key {
            llvm::StructType* ty;
            uint64_t          hash;
            bool              operator==(const Key&) const = default;
        };
        struct KeyHash {
            uint64_t operator()(const Key& k) const {
                return ankerl::unordered_dense::detail::wyhash::hash(
                  reinterpret_cast<uintptr_t>(k.ty) ^ k.hash);
            }
        };

        // Instantiation count per definition state type (the anchors' reuse rule).
        ankerl::unordered_dense::map<llvm::StructType*, uint32_t> ty_uses;
        for (const Anchor& a : anchors_) {
            ty_uses[a.ty]++;
        }

        const auto all = mod_.processes();
        uint64_t attributed = 0, eligible = 0, covered = 0, blocked_call = 0, blocked_extra_sig = 0,
                 blocked_xhier = 0;
        ankerl::unordered_dense::map<Key, uint32_t, KeyHash> twins;
        std::vector<std::pair<uint32_t, Key>>                proc_keys;
        std::vector<uint8_t>                                 buf;
        for (const Process& p : all) {
            if (p.scope == Process::no_scope || p.scope >= anchor_of_scope_.size() ||
                anchor_of_scope_[p.scope] == SignalSlot::no_anchor) {
                continue;
            }
            ++attributed;
            const Anchor& home = anchors_[anchor_of_scope_[p.scope]];
            if (ty_uses[home.ty] < 2) {
                continue; // unique definition: nothing to share with
            }
            ++eligible;

            buf.clear();
            const auto put = [&](const void* d, size_t n) {
                const auto* b = static_cast<const uint8_t*>(d);
                buf.insert(buf.end(), b, b + n);
            };
            const auto put32 = [&](uint32_t v) { put(&v, 4); };
            bool       xhier = false;
            const auto canon_signal = [&](Signal sig) {
                const SignalSlot& s = slot_of(sig);
                // Own-scope signal: canonical by its def-local field path.
                // (Scopes no longer nest, so a descendant instance's signal is
                // instance-specific like any other cross-hierarchy reference.)
                if (s.anchor != SignalSlot::no_anchor &&
                    &anchors_[s.anchor] == &home) {
                    put32(1u);
                    for (const uint32_t f : s.path) {
                        put32(f);
                    }
                    return;
                }
                // Unscoped/package signal: identical for every instance, still
                // shareable. A signal under another instance: instance-specific.
                xhier = xhier || s.anchor != SignalSlot::no_anchor;
                put32(2u);
                put32(static_cast<uint32_t>(sig));
            };

            put32(static_cast<uint32_t>(p.activation));
            put32(p.self_start ? 1u : 0u);
            for (const WaitEvent& w : p.wait_events) {
                canon_signal(w.signal);
                put32(static_cast<uint32_t>(w.edge));
            }
            bool      calls = false, extra_sig = false;
            const Ir& ir    = p.ir;
            for (uint32_t k = 0; k < ir.inst_count(); ++k) {
                const auto tag = ir.tag(static_cast<Index>(k));
                put(&tag, 1);
                const Inst::Payload& pl = ir.payload(static_cast<Index>(k));
                if (payload_kind(tag) == Inst::PayloadKind::Signal) {
                    canon_signal(pl.signal);
                } else {
                    put(&pl, sizeof(pl));
                }
                calls = calls || tag == Inst::Tag::Call || tag == Inst::Tag::TaskCall;
                extra_sig = extra_sig || tag == Inst::Tag::Wait || tag == Inst::Tag::StoreNbEvent ||
                            tag == Inst::Tag::ReadMem || tag == Inst::Tag::FreadMem ||
                            tag == Inst::Tag::EventTriggerNbDelayed;
            }
            // Raw extra buffer: region slices, operand runs, packed strings —
            // body-local and deterministic, except the embedded-Signal records
            // flagged above.
            const std::span<const uint32_t> extra = ir.extra_raw();
            put(extra.data(), extra.size() * sizeof(uint32_t));
            blocked_call += calls ? 1 : 0;
            blocked_extra_sig += extra_sig ? 1 : 0;
            blocked_xhier += xhier ? 1 : 0;
            const Key key{ home.ty,
                           ankerl::unordered_dense::detail::wyhash::hash(buf.data(), buf.size()) };
            twins[key]++;
            proc_keys.emplace_back(static_cast<uint32_t>(&p - all.data()), key);
        }

        uint64_t groups2 = 0;
        for (const auto& [k, n] : twins) {
            if (n >= 2) {
                ++groups2;
                covered += n;
            }
        }
        // Shape mode (CLOTHO_DEDUP_STAT=2): remember the twin sets so
        // report_dedup_shapes can compare the lowered LLVM functions after the
        // groups exist.
        const char* mode = std::getenv("CLOTHO_DEDUP_STAT");
        if (mode != nullptr && std::string_view(mode) == "2") {
            ankerl::unordered_dense::map<Key, uint32_t, KeyHash> set_of;
            for (const auto& [proc, key] : proc_keys) {
                if (twins[key] < 2) {
                    continue;
                }
                const auto [it, fresh] = set_of.try_emplace(
                  key, static_cast<uint32_t>(shape_sets_.size()));
                if (fresh) {
                    shape_sets_.push_back({});
                }
                shape_sets_[it->second].push_back(proc);
            }
        }
        std::fprintf(stderr,
                     "dedup: %llu/%llu attributed procs in reused defs; %llu shareable in %llu "
                     "twin groups (%.1f%%); instance-specific hints: call=%llu extra-sig=%llu "
                     "xhier=%llu\n",
                     (unsigned long long)eligible, (unsigned long long)attributed,
                     (unsigned long long)covered, (unsigned long long)groups2,
                     eligible != 0 ? 100.0 * static_cast<double>(covered) / static_cast<double>(eligible)
                                   : 0.0,
                     (unsigned long long)blocked_call, (unsigned long long)blocked_extra_sig,
                     (unsigned long long)blocked_xhier);
    }

    // Shape-equality proof (CLOTHO_DEDUP_STAT=2): for every twin set the
    // detector found, print each member's lowered LLVM function and compare
    // them after normalizing the only legitimately instance-specific tokens —
    // the function's own name and the @clotho.inst.* symbols (numbered by
    // first occurrence per function, so two distinct symbols at one position
    // versus one reused symbol still differ). A remaining mismatch is a
    // per-instance bake the def-relative families missed; sharing must not
    // proceed past a DIFF. Only singleton fusion groups compare — a fused
    // function mixes members, so per-process comparison is meaningless there.
    static std::string normalize_shape(llvm::Function* fn, llvm::ModuleSlotTracker* mst = nullptr) {
        std::string raw;
        {
            llvm::raw_string_ostream os(raw);
            if (mst != nullptr) {
                // Function::print(OS, AAW*) hides Value's slot-tracker overload.
                fn->Value::print(os, *mst);
            } else {
                fn->print(os);
            }
        }
        const auto ident = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '.' || c == '$' || c == '-';
        };
        ankerl::unordered_dense::map<std::string, uint32_t> inst_ids;
        std::string out;
        out.reserve(raw.size());
        size_t i = 0;
        while (i < raw.size()) {
            if (raw[i] != '@') {
                out += raw[i++];
                continue;
            }
            size_t j = i + 1;
            while (j < raw.size() && ident(raw[j])) {
                ++j;
            }
            const std::string tok = raw.substr(i, j - i);
            if (tok.starts_with("@clotho.inst.")) {
                const auto [it, fresh] =
                  inst_ids.try_emplace(tok, static_cast<uint32_t>(inst_ids.size()));
                out += "@INST" + std::to_string(it->second);
            } else if (tok == "@" + fn->getName().str()) {
                out += "@FN";
            } else {
                out += tok;
            }
            i = j;
        }
        return out;
    }

    void report_dedup_shapes(const std::vector<llvm::Function*>& group_fns) {
        uint64_t ok = 0, differ = 0, skipped = 0;
        for (size_t k = 0; k < shape_sets_.size(); ++k) {
            const std::vector<uint32_t>& procs = shape_sets_[k];
            bool singleton = true;
            for (const uint32_t p : procs) {
                singleton = singleton && groups_[group_of_[p]].size() == 1;
            }
            if (!singleton) {
                ++skipped;
                continue;
            }
            const std::string first = normalize_shape(group_fns[group_of_[procs.front()]]);
            bool              same  = true;
            for (size_t m = 1; m < procs.size() && same; ++m) {
                const std::string other = normalize_shape(group_fns[group_of_[procs[m]]]);
                if (other != first) {
                    same = false;
                    // First mismatching line, for diagnosis.
                    std::istringstream a(first), b(other);
                    std::string        la, lb;
                    while (std::getline(a, la) && std::getline(b, lb)) {
                        if (la != lb) {
                            std::fprintf(stderr, "dedup-shape DIFF set %zu (procs %u vs %u):\n  - %s\n  + %s\n",
                                         k, procs.front(), procs[m], la.c_str(), lb.c_str());
                            break;
                        }
                    }
                }
            }
            ok += same ? 1 : 0;
            differ += same ? 0 : 1;
        }
        std::fprintf(stderr,
                     "dedup-shape: %zu twin sets: %llu identical, %llu DIFFER, %llu skipped (fused)\n",
                     shape_sets_.size(), (unsigned long long)ok, (unsigned long long)differ,
                     (unsigned long long)skipped);
    }

    // -- Module-instance body sharing (dedup V1) --

    // The single @clotho.inst.* global a function references, or null when it
    // references none, more than one, or one from a PHI operand (a shape the
    // ctx-to-parameter rewrite below does not handle). Eligible bodies
    // reference exactly their own instance context: every def-relative site
    // routes through ctx_field and def_relative_for restricts that to the
    // group's own anchor.
    static llvm::GlobalVariable* only_inst_global(llvm::Function* fn) {
        llvm::GlobalVariable* found = nullptr;
        bool                  bad   = false;
        auto scan = [&](auto&& self, llvm::Constant* c) -> void {
            if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(c)) {
                if (gv->getName().starts_with("clotho.inst.")) {
                    bad   = bad || (found != nullptr && found != gv);
                    found = gv;
                }
                return;
            }
            if (llvm::isa<llvm::GlobalValue>(c)) {
                return;
            }
            for (const llvm::Use& op : c->operands()) {
                if (auto* oc = llvm::dyn_cast<llvm::Constant>(op)) {
                    self(self, oc);
                }
            }
        };
        for (llvm::BasicBlock& bb : *fn) {
            for (llvm::Instruction& in : bb) {
                for (const llvm::Use& op : in.operands()) {
                    auto* c = llvm::dyn_cast<llvm::Constant>(op);
                    if (c == nullptr) {
                        continue;
                    }
                    llvm::GlobalVariable* before = found;
                    scan(scan, c);
                    bad = bad || (llvm::isa<llvm::PHINode>(in) && found != before);
                }
            }
        }
        return bad ? nullptr : found;
    }

    static bool constant_uses(const llvm::Constant* c, const llvm::GlobalVariable* gv) {
        if (c == gv) {
            return true;
        }
        if (llvm::isa<llvm::GlobalValue>(c)) {
            return false;
        }
        for (const llvm::Use& op : c->operands()) {
            if (auto* oc = llvm::dyn_cast<llvm::Constant>(op); oc != nullptr && constant_uses(oc, gv)) {
                return true;
            }
        }
        return false;
    }

    // Clone `rep` into the definition's shared body: same (ptr, ptr) -> void
    // signature, with every constant reference to the rep instance's ctx
    // global rewritten to the first parameter. noinline keeps ONE compiled
    // copy — the inliner would otherwise re-expand the body into every stub,
    // undoing the compile-time win.
    llvm::Function* clone_with_ctx_param(llvm::Function* rep, llvm::GlobalVariable* ctx_gv) {
        auto* shared = llvm::Function::Create(rep->getFunctionType(), llvm::Function::InternalLinkage,
                                              rep->getName() + ".shared", *rep->getParent());
        llvm::ValueToValueMapTy vmap;
        auto                    ni = shared->arg_begin();
        for (llvm::Argument& a : rep->args()) {
            vmap[&a] = &*ni;
            ++ni;
        }
        llvm::SmallVector<llvm::ReturnInst*, 4> rets;
        llvm::CloneFunctionInto(shared, rep, vmap, llvm::CloneFunctionChangeType::LocalChangesOnly, rets);
        // After the clone: CloneFunctionInto copies the rep's attributes,
        // which would reset these.
        shared->setDSOLocal(true);
        shared->addFnAttr(llvm::Attribute::NoInline);
        llvm::Argument* ctx_arg = shared->getArg(0);
        ctx_arg->setName("ictx");
        for (llvm::BasicBlock& bb : *shared) {
            for (llvm::Instruction& in : bb) {
                for (unsigned i = 0; i < in.getNumOperands(); ++i) {
                    llvm::Value* op = in.getOperand(i);
                    if (op == ctx_gv) {
                        in.setOperand(i, ctx_arg);
                        continue;
                    }
                    auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(op);
                    if (ce == nullptr || !constant_uses(ce, ctx_gv)) {
                        continue;
                    }
                    llvm::Instruction* repl = ce->getAsInstruction();
                    repl->insertBefore(in.getIterator());
                    for (unsigned k = 0; k < repl->getNumOperands(); ++k) {
                        if (repl->getOperand(k) == ctx_gv) {
                            repl->setOperand(k, ctx_arg);
                        }
                    }
                    in.setOperand(i, repl);
                }
            }
        }
        return shared;
    }

    // Replace `fn`'s body with a musttail stub into the shared body, passing
    // this instance's ctx global as the first argument (identical (ptr, ptr)
    // prototypes, so the subscription/dispatch ABI is untouched).
    void rewrite_to_stub(llvm::Function* fn, llvm::Function* shared, llvm::GlobalVariable* ctx_gv) {
        fn->deleteBody();
        llvm::IRBuilderBase::InsertPoint saved = b_.saveIP();
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
        auto* call = b_.CreateCall(shared->getFunctionType(), shared, { ctx_gv, fn->getArg(1) });
        call->setTailCallKind(llvm::CallInst::TCK_MustTail);
        b_.CreateRetVoid();
        b_.restoreIP(saved);
    }

    // Share one compiled body across twin instances of a reused definition.
    // Candidates: thunk groups attributed to a reused-definition instance
    // (group_anchor_of_) whose lowered function leaves the state argument
    // unused (thunk bodies address storage through the anchors) and
    // references exactly its own instance-context global. Groups bucket by
    // normalize_shape — literal IR identity modulo the function's own name
    // and that one symbol — so the transformation applies only where it is
    // provably exact; the canonical-hash detector (report_dedup_stat) is a
    // statistic, not the gate. Each bucket of >= 2 keeps one shared body (a
    // clone of the first member with the ctx global replaced by a parameter)
    // and every member becomes a two-instruction musttail stub passing its
    // own ctx. Per-member NBA blocks / commit functions / ctx fills, VCD
    // indices, and wait machinery are untouched — members registered their
    // own during lowering; only body code is shared. Runs before
    // finalize_block_nba / emit_main (nothing reads group function bodies,
    // only their addresses). Disabled by --no-dedup.
    void share_twin_groups(const std::vector<llvm::Function*>& group_fns) {
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t   eligible = 0;
        struct Cand {
            uint32_t              g;
            llvm::GlobalVariable* ctx;
        };
        // Cheap structural pre-key: definition identity (ctx type) and size
        // counts — NOT the def-local ordinal: twins whose ordinal never
        // reaches the IR (no NBA block, sched constant equal by shape) may
        // share across ordinals, and shape equality is the actual gate. Only
        // pre-buckets of >= 2 pay the IR print — a singleton can share with
        // nothing.
        struct PreKey {
            const void* ctx_ty;
            uint64_t    sizes;
            bool        operator==(const PreKey&) const = default;
        };
        struct PreKeyHash {
            uint64_t operator()(const PreKey& k) const {
                return ankerl::unordered_dense::detail::wyhash::hash(
                  reinterpret_cast<uintptr_t>(k.ctx_ty) ^ k.sizes);
            }
        };
        ankerl::unordered_dense::map<PreKey, std::vector<Cand>, PreKeyHash> pre;
        for (uint32_t g = 0; g < groups_.size(); ++g) {
            if (!thunk_group_[g] || group_anchor_of_[g] == SignalSlot::no_anchor) {
                continue;
            }
            llvm::Function* fn = group_fns[g];
            if (!fn->getArg(0)->use_empty()) {
                continue;
            }
            llvm::GlobalVariable* ctx = only_inst_global(fn);
            if (ctx == nullptr || ctx != anchors_[group_anchor_of_[g]].ctx_gv) {
                continue;
            }
            ++eligible;
            uint64_t insts = 0, bbs = 0;
            for (const llvm::BasicBlock& bb : *fn) {
                ++bbs;
                insts += bb.size();
            }
            const Anchor& a = anchors_[group_anchor_of_[g]];
            pre[PreKey{ a.ctx_ty, bbs << 24 | insts << 40 }].push_back({ g, ctx });
        }
        llvm::ModuleSlotTracker mst(&env_->llmod, /*ShouldInitializeAllMetadata=*/false);
        ankerl::unordered_dense::map<std::string, std::vector<Cand>> buckets;
        for (auto& [pk, cs] : pre) {
            if (cs.size() < 2) {
                continue;
            }
            for (const Cand& c : cs) {
                buckets[normalize_shape(group_fns[c.g], &mst)].push_back(c);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        uint64_t sets = 0, members = 0;
        for (auto& [shape, cs] : buckets) {
            if (cs.size() < 2) {
                continue;
            }
            ++sets;
            members += cs.size();
            llvm::Function* shared = clone_with_ctx_param(group_fns[cs.front().g], cs.front().ctx);
            for (const Cand& c : cs) {
                rewrite_to_stub(group_fns[c.g], shared, c.ctx);
            }
        }
        if (std::getenv("CLOTHO_DEDUP_STAT") != nullptr) {
            const auto t2 = std::chrono::steady_clock::now();
            const auto ms = [](auto d) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
            };
            std::fprintf(stderr,
                         "dedup-share: %llu bodies -> %llu shared (%llu eligible; bucket %lld ms, "
                         "rewrite %lld ms)\n",
                         (unsigned long long)members, (unsigned long long)sets,
                         (unsigned long long)eligible, (long long)ms(t1 - t0), (long long)ms(t2 - t1));
        }
    }

    void report_comb_fuse_stat() const {
        const auto all = mod_.processes();
        uint64_t   counts[static_cast<size_t>(CombFuse::Count)] = {};
        uint64_t   always = 0, singleton_eligible = 0;
        for (uint32_t k = 0; k < all.size(); ++k) {
            if (all[k].activation != Activation::Always) {
                continue;
            }
            ++always;
            const CombFuse v = comb_fuse_verdict(all[k]);
            counts[static_cast<size_t>(v)]++;
            if (v == CombFuse::Eligible && groups_[group_of_[k]].size() == 1) {
                ++singleton_eligible;
            }
        }
        std::fprintf(stderr,
                     "comb-fuse: %llu/%llu always procs eligible (%llu unfused singletons); "
                     "excluded: not-simple=%llu edge=%llu impure=%llu call=%llu nba=%llu "
                     "write=%llu read!sens=%llu\n",
                     (unsigned long long)counts[static_cast<size_t>(CombFuse::Eligible)],
                     (unsigned long long)always, (unsigned long long)singleton_eligible,
                     (unsigned long long)counts[static_cast<size_t>(CombFuse::NotSimpleAlways)],
                     (unsigned long long)counts[static_cast<size_t>(CombFuse::EdgeSensitive)],
                     (unsigned long long)counts[static_cast<size_t>(CombFuse::ImpureOp)],
                     (unsigned long long)counts[static_cast<size_t>(CombFuse::HasCall)],
                     (unsigned long long)counts[static_cast<size_t>(CombFuse::NbaWrite)],
                     (unsigned long long)counts[static_cast<size_t>(CombFuse::UnsupportedWrite)],
                     (unsigned long long)counts[static_cast<size_t>(CombFuse::ReadOutsideSens)]);
        if (!comb_group_sizes_.empty()) {
            uint64_t members = 0, mx = 0;
            for (const uint32_t s : comb_group_sizes_) {
                members += s;
                mx = std::max<uint64_t>(mx, s);
            }
            std::fprintf(stderr, "comb-fuse: %zu comb groups, %llu members (max %llu): ",
                         comb_group_sizes_.size(), (unsigned long long)members,
                         (unsigned long long)mx);
            std::vector<uint32_t> sorted(comb_group_sizes_.begin(), comb_group_sizes_.end());
            std::sort(sorted.rbegin(), sorted.rend());
            for (size_t i = 0; i < sorted.size() && i < 24; ++i) {
                std::fprintf(stderr, "%u ", sorted[i]);
            }
            std::fprintf(stderr, "%s\n", sorted.size() > 24 ? "..." : "");
        }
    }

    // V1 static process fusion: partition processes into groups that lower to a
    // single coroutine each. A group's members share an identical canonical
    // sensitivity set and self_start flag, so the fused wake set equals every
    // member's own wake set — each body still runs exactly when its own trigger
    // fires, and activation counts are unchanged by construction (no
    // over-evaluation possible). Running members back-to-back fixes one of the
    // LRM-allowed same-instant orders (members are ordered by their
    // sub_order_ rank, matching the unfused schedule).
    //
    // A process is fusable only when:
    //  - it is an Always with a static sensitivity list;
    //  - its body has no suspension point (no #delay, no intra-body @/wait, no
    //    task call — a suspended fused body would stall its siblings);
    //  - it does not blocking-write a signal in its own sensitivity set (the
    //    write would re-wake an already-ran sibling in the unfused schedule,
    //    but lands mid-body under the group's held sched flag when fused).
    void compute_fusion_groups() {
        const auto all    = mod_.processes();
        const auto nprocs = static_cast<uint32_t>(all.size());
        groups_.clear();
        group_of_.assign(nprocs, 0);
        group_events_.clear();

        // Position of each process in sub_order_ (its scheduled rank).
        std::vector<uint32_t> pos(nprocs, 0);
        for (uint32_t i = 0; i < nprocs; ++i) {
            pos[sub_order_[i]] = i;
        }

        const auto canonical = [](const std::vector<WaitEvent>& evs) {
            std::vector<std::pair<uint32_t, uint32_t>> key;
            key.reserve(evs.size());
            for (const WaitEvent& w : evs) {
                key.emplace_back(static_cast<uint32_t>(w.signal), static_cast<uint32_t>(w.edge));
            }
            std::sort(key.begin(), key.end());
            key.erase(std::unique(key.begin(), key.end()), key.end());
            return key;
        };

        // Bucket fusable processes by (canonical events, self_start), preserving
        // first-seen order; everything else is its own singleton group. Group
        // size is capped so one fused coroutine cannot grow arbitrarily large
        // (function size / frame size / compile time).
        constexpr size_t group_cap = 256;
        std::map<std::pair<std::vector<std::pair<uint32_t, uint32_t>>, bool>, uint32_t> by_key;
        for (uint32_t k = 0; k < nprocs; ++k) {
            const Process& p = all[k];
            if (config_.fuse && is_simple_always(p)) {
                const auto key = std::make_pair(canonical(p.wait_events), p.self_start);
                const auto it  = by_key.find(key);
                if (it != by_key.end() && groups_[it->second].size() < group_cap) {
                    groups_[it->second].push_back(k);
                    group_of_[k] = it->second;
                    continue;
                }
                by_key[key] = static_cast<uint32_t>(groups_.size());
            }
            group_of_[k] = static_cast<uint32_t>(groups_.size());
            groups_.push_back({ k });
        }

        // Comb fusion: merge dependent comb-eligible singletons into
        // rank-contiguous chunks running producer-before-consumer in one
        // activation, subscribed to the union sensitivity (see
        // docs/design/comb-fusion.md for the LRM as-if argument). Two facts
        // carry the legality: the sched flag is held across the fused body,
        // so an intra-group store cannot re-enqueue the group (the downstream
        // member already runs later in the same activation with final
        // values); and change detection makes a spuriously-run member (pure,
        // reads unchanged) write no changes and wake nobody. Grouping is by
        // connected component of the wake graph so independent cones are
        // never yoked together (a union sensitivity across unrelated cones
        // would run every body per trigger); nontrivial SCCs (comb loops)
        // keep re-wake fixpoint iteration.
        if (config_.fuse && config_.comb_fuse && nprocs >= 2) {
            std::vector<uint8_t> cand(nprocs, 0);
            for (const auto& g : groups_) {
                if (g.size() != 1) {
                    continue;
                }
                const uint32_t k = g.front();
                if (scc_nontrivial_[k] == 0 && comb_fuse_verdict(all[k]) == CombFuse::Eligible) {
                    cand[k] = 1;
                }
            }
            // Interior guard: only members whose ENTIRE sensitivity is
            // candidate-written may merge. A member with a private external
            // trigger (e.g. star fan-out: `assign c_i = s ^ x_i` with hot
            // x_i) would drag the whole group awake on every private change —
            // measured 10.9x slower than unfused on a 256-consumer star.
            // Interior members wake only via changes propagating through the
            // component, which is exactly the wave the fused topo order
            // replaces. Non-interior candidates stay singletons but still
            // provide edges/writes (their stores wake the groups they feed).
            std::vector<uint8_t> cand_writes(static_cast<uint32_t>(mod_.signals().size()), 0);
            for (uint32_t k = 0; k < nprocs; ++k) {
                if (cand[k] == 0) {
                    continue;
                }
                const Ir& ir = all[k].ir;
                for (size_t j = 0; j < ir.inst_count(); ++j) {
                    const auto idx = static_cast<Index>(j);
                    if (ir.tag(idx) != Inst::Tag::Store) {
                        continue;
                    }
                    const LvalueRoot r = lvalue_root(ir, ir.payload(idx).binary.l);
                    if (ir.tag(r.root) == Inst::Tag::Signal) {
                        cand_writes[static_cast<uint32_t>(ir.payload(r.root).signal)] = 1;
                    }
                }
            }
            std::vector<uint8_t> interior(nprocs, 0);
            for (uint32_t k = 0; k < nprocs; ++k) {
                if (cand[k] == 0) {
                    continue;
                }
                interior[k] = 1;
                for (const WaitEvent& w : all[k].wait_events) {
                    if (cand_writes[static_cast<uint32_t>(w.signal)] == 0) {
                        interior[k] = 0;
                        break;
                    }
                }
            }
            // Union-find over wake edges between candidates. self_start must
            // match: the group makes one seed decision (comb populations are
            // uniformly self_start in practice; a mismatch just splits).
            std::vector<uint32_t> uf(nprocs);
            for (uint32_t k = 0; k < nprocs; ++k) {
                uf[k] = k;
            }
            const auto find = [&](uint32_t x) {
                while (uf[x] != x) {
                    uf[x] = uf[uf[x]];
                    x     = uf[x];
                }
                return x;
            };
            for (uint32_t k = 0; k < nprocs; ++k) {
                if (cand[k] == 0) {
                    continue;
                }
                for (const uint32_t q : wake_adj_[k]) {
                    if (cand[q] != 0 && all[q].self_start == all[k].self_start) {
                        uf[find(k)] = find(q);
                    }
                }
            }
            // Component members in scheduled-rank order (sub_order_ iteration
            // yields them pre-sorted), then chunked at the group cap; a
            // leftover chunk of one stays a singleton. Only interior members
            // merge; components are still discovered over all candidates so
            // two interior members joined through a non-interior producer
            // land in one group.
            std::vector<std::vector<uint32_t>> comp(nprocs);
            for (const uint32_t k : sub_order_) {
                if (interior[k] != 0) {
                    comp[find(k)].push_back(k);
                }
            }
            bool merged = false;
            comb_group_sizes_.clear();
            for (const auto& c : comp) {
                for (size_t off = 0; off + 1 < c.size(); off += group_cap) {
                    const size_t end = std::min(c.size(), off + group_cap);
                    if (end - off < 2) {
                        break;
                    }
                    const uint32_t g0 = group_of_[c[off]];
                    for (size_t j = off + 1; j < end; ++j) {
                        const uint32_t m = c[j];
                        groups_[group_of_[m]].clear();
                        groups_[g0].push_back(m);
                        group_of_[m] = g0;
                        merged       = true;
                    }
                    comb_group_sizes_.push_back(static_cast<uint32_t>(end - off));
                }
            }
            // Compact the emptied singleton slots.
            if (merged) {
                std::vector<uint32_t> remap(groups_.size(), 0);
                uint32_t              w = 0;
                for (uint32_t g = 0; g < groups_.size(); ++g) {
                    if (!groups_[g].empty()) {
                        remap[g] = w;
                        if (w != g) {
                            groups_[w] = std::move(groups_[g]);
                        }
                        ++w;
                    }
                }
                groups_.resize(w);
                for (uint32_t k = 0; k < nprocs; ++k) {
                    group_of_[k] = remap[group_of_[k]];
                }
            }
        }

        // Members run in scheduled-rank order (the order the unfused FIFO would
        // have popped them); the group subscribes at its earliest member's rank.
        group_events_.resize(groups_.size());
        for (uint32_t g = 0; g < groups_.size(); ++g) {
            std::sort(groups_[g].begin(), groups_[g].end(),
                      [&](uint32_t a, uint32_t b) { return pos[a] < pos[b]; });
            if (groups_[g].size() == 1) {
                group_events_[g] = all[groups_[g].front()].wait_events;
                continue;
            }
            // Fused: subscribe the canonical deduped union of member events
            // once (wait_counts sizes the buckets to match). For V1 groups
            // the union equals the front member's set; comb groups need the
            // true union.
            std::vector<WaitEvent> evs;
            for (const uint32_t m : groups_[g]) {
                evs.insert(evs.end(), all[m].wait_events.begin(), all[m].wait_events.end());
            }
            for (const auto& [s, e] : canonical(evs)) {
                group_events_[g].push_back(WaitEvent{ static_cast<Signal>(s), static_cast<Edge>(e) });
            }
        }

        // A group is thunk-eligible when its (front) member is a suspension-free
        // always-with-sensitivity: fused members all passed is_simple_always, so
        // the front's verdict holds for the group. Computed here so lower_group
        // and the seed/drain read one table. Empty is fine when --no-thunk.
        thunk_group_.assign(groups_.size(), false);
        if (config_.thunk) {
            for (uint32_t g = 0; g < groups_.size(); ++g) {
                thunk_group_[g] = is_simple_always(all[groups_[g].front()]);
            }
        }
    }

    // -- Dynamic domain specialization (opt-in: config_.domain_spec) --
    //
    // A 4-state signal rarely changes domain at runtime: typically x until
    // reset, then clean (no x/z bit) for the rest of the run. When enabled,
    // every thunk group whose body meets spec_eligible_body is compiled twice —
    // the generic 4-state body, plus a fast body lowered as if every 4-state
    // value were 2-state (assume_clean_ makes type_of report 4-state as
    // 2-state, routing every operator through the native 2-state paths; stores
    // lift back to the packed planes via to_four, so storage and outside
    // readers see identical bits). Dispatch is reader-side (lower_group): at
    // each activation the group's tracked read-set signals are loaded and
    // tested for cleanliness, short-circuiting to the generic body on the first
    // dirty one. The fast body runs only when the whole read set is clean,
    // where 2-state and 4-state lowering agree exactly, so results are
    // identical by construction — only speed changes. Reader-side validation
    // carries no write-side cost: nothing is maintained at store / NBA-commit
    // sites.
    //
    // No profitability gate. The feature is off by default and applies to
    // every eligible group when opted in — the fast body's 4-state<->2-state
    // boundary conversions are amortized only by op-dense bodies, so it is not
    // a universal win, but choosing that trade-off is the caller's job, not a
    // magic-number heuristic in the backend. See docs/design/domain-spec.md.

    // Whether one member body may be lowered under assume_clean_. Allow-list:
    // any tag not listed keeps the group generic, so newly added IR tags are
    // safe by default. Rejected classes:
    //  - x-generators (Div / Mod / Pow: a clean-input op whose 4-state result
    //    can be x, which the 2-state path folds to 0 — observably different);
    //  - dynamic bit/part selects (an out-of-range or unknown index reads x in
    //    4-state but 0 on the 2-state path; TODO: admit provably in-range
    //    selects) and array element selects (per-element cleanliness is not
    //    tracked);
    //  - Call (the callee body is lowered generically and may blocking-write
    //    x into this group's read set mid-activation, invalidating the
    //    entry-time dispatch);
    //  - runtime write-backs ($fgets / $*scanf / $fread / $readmem / $ferror:
    //    they can deposit x mid-body through a pointer);
    //  - strings (separate value machinery, no need to prove it x-safe);
    //  - constants carrying x/z bits (their must-plane projection would lose
    //    the x/z, e.g. casez wildcard patterns).
    // Debug companion of spec_eligible_body: the numeric Inst::Tag of the
    // first rejecting instruction, or -1. Kept trivially in sync by calling
    // the same predicate per single-instruction window.
    int spec_first_reject(const Ir& ir) {
        for (size_t k = 0; k < ir.inst_count(); ++k) {
            if (!spec_inst_ok(ir, static_cast<Index>(k))) {
                return static_cast<int>(ir.tag(static_cast<Index>(k)));
            }
        }
        return -1;
    }

    bool spec_eligible_body(const Ir& ir) {
        for (size_t k = 0; k < ir.inst_count(); ++k) {
            if (!spec_inst_ok(ir, static_cast<Index>(k))) {
                return false;
            }
        }
        return true;
    }

    // The tightest provable bound (in bits) on a select index value: the
    // lowering zero-extends the index's must bits, so its width bounds it by
    // 2^w - 1 — but the frontend resizes every index to 32 bits first, so
    // peel widening Resizes of unsigned sources back to the true source
    // width (a zext preserves the bound; a sext of a signed source can turn
    // a negative into a huge zext'd index, so peeling stops there).
    uint32_t spec_index_bound_width(const Ir& ir, Index idx) {
        uint32_t w = type_of(idx).width;
        while (ir.tag(idx) == Inst::Tag::Resize) {
            const Index   src = ir.payload(idx).op_type.op;
            const ValType st  = type_of(src);
            if (st.is_signed && type_of(idx).width > st.width) {
                break;
            }
            w   = std::min(w, st.width);
            idx = src;
        }
        // A constant index (e.g. a localparam, typed 32-bit) is bounded by its
        // actual value's bit length. Its x/z-freedom was already established:
        // the eligibility scan visits operands before users (flat store is in
        // creation order) and rejects any x/z-carrying constant outright.
        if (ir.tag(idx) == Inst::Tag::Constant) {
            const Ref                      ref   = ir.payload(idx).ref;
            const Value&                   v     = std::get<Value>(mod_.pool.get(ref));
            const std::span<const uint8_t> bytes = mod_.pool.bytes(ref);
            const uint32_t                 pb    = plane_bytes(mod_.pool.layout(v.type).width);
            uint32_t                       bits  = 0;
            for (uint32_t bi = 0; bi < pb && bi < bytes.size(); ++bi) {
                if (bytes[bi] != 0) {
                    bits = 8 * bi + (32 - static_cast<uint32_t>(std::countl_zero(uint32_t{ bytes[bi] })));
                }
            }
            w = std::min(w, bits);
        }
        return w;
    }

    // One instruction of the eligibility scan (see spec_eligible_body).
    bool spec_inst_ok(const Ir& ir, Index idx) {
        {
            const Inst::Payload& p = ir.payload(idx);
            switch (ir.tag(idx)) {
                case Inst::Tag::Constant: {
                    const Value& v = std::get<Value>(mod_.pool.get(p.ref));
                    if (std::get<Type>(mod_.pool.get(v.type)).kind == Type::Kind::String) {
                        return false;
                    }
                    const Layout lay = mod_.pool.layout(v.type);
                    if (lay.domain == Domain::Four) {
                        const auto     bytes = mod_.pool.bytes(p.ref);
                        const uint32_t pb    = plane_bytes(lay.width);
                        for (uint32_t bi = pb; bi < 2 * pb && bi < bytes.size(); ++bi) {
                            if (bytes[bi] != 0) {
                                return false; // x/z bits in the literal
                            }
                        }
                    }
                    break;
                }
                case Inst::Tag::Signal: {
                    const SignalSlot& s = slots_[static_cast<uint32_t>(p.signal)];
                    if (s.is_string || s.is_array) {
                        return false;
                    }
                    break;
                }
                // A dynamic bit/part select is admitted when it provably cannot
                // go out of range: the lowering zero-extends the index's must
                // bits (lower_inst BitSelect/DynPartSelect), so a w-bit index is
                // bounded by 2^w - 1 regardless of signedness, and a fast-body
                // index is clean by construction — in range, the 2-state select
                // equals the 4-state one exactly. Anything wider falls back to
                // generic (an out-of-range read is x in 4-state, 0 in 2-state).
                case Inst::Tag::BitSelect: {
                    const ValType  base = type_of(p.binary.l);
                    const uint32_t iw   = spec_index_bound_width(ir, p.binary.r);
                    if (iw < 32 && (1u << iw) <= base.width) {
                        break;
                    }
                    return false;
                }
                case Inst::Tag::DynPartSelect: {
                    const auto     dps  = ir.extra_data<DynPartSelect>(p.op_extra.extra);
                    const ValType  base = type_of(p.op_extra.op);
                    const uint32_t ow   = spec_index_bound_width(ir, dps.offset);
                    if (ow < 32 && (1u << ow) - 1 + dps.width <= base.width) {
                        break;
                    }
                    return false;
                }
                // NOTE: PartSelect is the constant [msb:lsb] form; slang rejects
                // an out-of-range constant select of a packed type, so it cannot
                // read x on a clean base.
                case Inst::Tag::PartSelect:
                case Inst::Tag::Load:
                case Inst::Tag::Store:
                case Inst::Tag::StoreNb:
                case Inst::Tag::Add:
                case Inst::Tag::Sub:
                case Inst::Tag::Mul:
                case Inst::Tag::Band:
                case Inst::Tag::Bor:
                case Inst::Tag::Bxor:
                case Inst::Tag::Bxnor:
                case Inst::Tag::Bnot:
                case Inst::Tag::RedAnd:
                case Inst::Tag::RedOr:
                case Inst::Tag::RedXor:
                case Inst::Tag::Ctz:
                case Inst::Tag::Clz:
                case Inst::Tag::Sll:
                case Inst::Tag::Srl:
                case Inst::Tag::Sra:
                case Inst::Tag::Eq:
                case Inst::Tag::Ne:
                case Inst::Tag::Lt:
                case Inst::Tag::Le:
                case Inst::Tag::Gt:
                case Inst::Tag::Ge:
                // Case/wildcard equality with clean operands is a plain compare;
                // wildcard-carrying patterns were rejected at their Constant.
                case Inst::Tag::CaseEq:
                case Inst::Tag::CasezEq:
                case Inst::Tag::CasexEq:
                case Inst::Tag::WildEq:
                case Inst::Tag::InsideSet:
                // Reals never carry x/z; conversions from clean operands and
                // real math results are always defined.
                case Inst::Tag::FAdd:
                case Inst::Tag::FSub:
                case Inst::Tag::FMul:
                case Inst::Tag::FDiv:
                case Inst::Tag::FPow:
                case Inst::Tag::FNeg:
                case Inst::Tag::FEq:
                case Inst::Tag::FNe:
                case Inst::Tag::FLt:
                case Inst::Tag::FLe:
                case Inst::Tag::FGt:
                case Inst::Tag::FGe:
                case Inst::Tag::ItoF:
                case Inst::Tag::FtoI:
                case Inst::Tag::FtoITrunc:
                case Inst::Tag::FBitcast:
                case Inst::Tag::FResize:
                case Inst::Tag::RealMathUnary:
                case Inst::Tag::RealMathBinary:
                case Inst::Tag::Land:
                case Inst::Tag::Lor:
                case Inst::Tag::Lnot:
                case Inst::Tag::Select:
                case Inst::Tag::SelectLazy:
                case Inst::Tag::Concat:
                case Inst::Tag::Repl:
                case Inst::Tag::StreamReverse:
                case Inst::Tag::Resize:
                case Inst::Tag::Cond:
                case Inst::Tag::Switch:
                case Inst::Tag::Loop:
                case Inst::Tag::Break:
                case Inst::Tag::Continue:
                case Inst::Tag::Display:
                case Inst::Tag::FDisplay:
                case Inst::Tag::Finish:
                case Inst::Tag::CaseViol:
                case Inst::Tag::Severity:
                case Inst::Tag::Fflush:
                case Inst::Tag::Fclose:
                case Inst::Tag::FopenFd:
                case Inst::Tag::FopenMcd:
                case Inst::Tag::Fgetc:
                case Inst::Tag::Ungetc:
                case Inst::Tag::Feof:
                case Inst::Tag::Ftell:
                case Inst::Tag::Fseek:
                case Inst::Tag::Rewind:
                case Inst::Tag::TimeFormat:
                case Inst::Tag::SimTime:
                case Inst::Tag::Random:
                // $clog2 of a clean operand is never x.
                case Inst::Tag::Clog2:
                    break;
                default:
                    return false;
            }
        }
        return true;
    }

    // Decide which thunk groups get a specialized fast body and record each
    // one's tracked read set (its 4-state scalar signal loads) in
    // group_reads_. Reader-side validation (lower_group) recomputes the
    // fast/generic choice from live signal words at each dispatch, so no
    // persistent per-signal / per-group tracking state is built here. Runs
    // after build_state_types (slot facts) and compute_fusion_groups
    // (thunk_group_).
    void compute_domain_spec() {
        const auto all     = mod_.processes();
        const auto ngroups = static_cast<uint32_t>(groups_.size());
        spec_group_.assign(ngroups, false);
        group_reads_.assign(ngroups, {});
        if (!config_.domain_spec) {
            return;
        }
        dom_debug_ = getenv("CLOTHO_DOM_DEBUG") != nullptr;

        bool any_spec = false;
        for (uint32_t g = 0; g < ngroups; ++g) {
            if (!thunk_group_[g]) {
                continue;
            }
            bool                  ok = true;
            std::vector<uint32_t> reads;
            for (const uint32_t m : groups_[g]) {
                const Ir& ir = all[m].ir;
                // The eligibility scan queries operand types (bounded-select
                // proofs), so bind the member like select_member does; lowering
                // re-binds later. assume_clean_ is off here — real domains.
                ir_ = &ir;
                types_.resize(ir.inst_count());
                types_valid_.assign(ir.inst_count(), 0);
                if (!spec_eligible_body(ir)) {
                    if (dom_debug_) {
                        fprintf(stderr, "DOM reject g=%u proc=%u insts=%zu first_bad=%d\n", g, m,
                                ir.inst_count(), spec_first_reject(ir));
                    }
                    ok = false;
                    break;
                }
                for (size_t k = 0; k < ir.inst_count(); ++k) {
                    const auto idx = static_cast<Index>(k);
                    if (ir.tag(idx) != Inst::Tag::Load) {
                        continue;
                    }
                    const LvalueRoot r = lvalue_root(ir, ir.payload(idx).unary);
                    if (ir.tag(r.root) != Inst::Tag::Signal) {
                        continue;
                    }
                    const auto        si = static_cast<uint32_t>(ir.payload(r.root).signal);
                    const SignalSlot& s  = slots_[si];
                    if (s.four_state && !s.is_array && !s.is_string) {
                        reads.push_back(si);
                    }
                }
            }
            std::sort(reads.begin(), reads.end());
            reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
            // A group with no 4-state reads gains nothing from a second body:
            // its generic lowering already runs the native 2-state paths.
            if (!ok || reads.empty()) {
                continue;
            }
            // No profitability gate: domain specialization is a fully opt-in
            // feature (config_.domain_spec, off by default), so when it is on
            // every eligible group is specialized. The fast body pays
            // 4-state<->2-state boundary conversions that only op-dense bodies
            // amortize, so it is not a universal win — but that trade-off is
            // the caller's to make by opting in, not a static heuristic baked
            // into the backend. See docs/design/domain-spec.md.
            if (dom_debug_) {
                fprintf(stderr, "DOM group g=%u procs=[", g);
                for (const uint32_t m : groups_[g]) {
                    fprintf(stderr, "%u ", m);
                }
                fprintf(stderr, "] reads=[");
                for (const uint32_t si : reads) {
                    fprintf(stderr, "%u:%s ", si, mod_.signals()[si].name.c_str());
                }
                fprintf(stderr, "]\n");
            }
            spec_group_[g]   = true;
            any_spec         = true;
            group_reads_[g]  = std::move(reads);
        }

        // Reader-side validation carries no persistent tracking state: the
        // fast/generic choice is recomputed from live signal words at each
        // dispatch (see lower_group), so there are no per-signal tags or
        // per-group counters to build here. Debug-only dispatch counters are
        // the sole globals, created under CLOTHO_DOM_DEBUG.
        if (!any_spec || !dom_debug_) {
            return;
        }
        auto* i64   = llvm::Type::getInt64Ty(ctx_);
        auto* i32t  = llvm::Type::getInt32Ty(ctx_);
        dom_cnt_ty_ = llvm::ArrayType::get(i64, std::max<uint32_t>(ngroups, 1));
        dom_nfast_gv_ = new llvm::GlobalVariable(env_->llmod, dom_cnt_ty_, false, llvm::GlobalValue::ExternalLinkage,
                                                 llvm::ConstantAggregateZero::get(dom_cnt_ty_), "clotho.dom_nfast");
        dom_ngen_gv_ = new llvm::GlobalVariable(env_->llmod, dom_cnt_ty_, false, llvm::GlobalValue::ExternalLinkage,
                                                llvm::ConstantAggregateZero::get(dom_cnt_ty_), "clotho.dom_ngen");
        new llvm::GlobalVariable(env_->llmod, i32t, true, llvm::GlobalValue::ExternalLinkage,
                                 llvm::ConstantInt::get(i32t, ngroups), "clotho.dom_ngroups");
    }

    // Build the nonblocking-assignment update state: a constant per-signal metadata
    // table (live/shadow/mask offsets plus the wait-queue base/cap/len pointers the
    // block commit services from). Must run after build_state_layout (slot offsets,
    // nba flags) and build_wait_queues (queue globals).
    void build_nba_state() {
        auto* i32   = llvm::Type::getInt32Ty(ctx_);
        auto* ptr   = llvm::PointerType::getUnqual(ctx_);
        auto* arr3p = llvm::ArrayType::get(ptr, 3);
        auto* arr3i = llvm::ArrayType::get(i32, 3);
        nba_meta_ty_ = llvm::StructType::get(ctx_, { arr3p, arr3i, arr3p, arr3p });

        const auto sigs = mod_.signals();

        // Every scalar (non-array) NBA target commits through the block-commit path.
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            SignalSlot& s = slots_[i];
            if (s.nba) {
                s.nba_block = !s.is_array;
            }
        }

        // Constant metadata, one record per nba-id (signal order, matching
        // nba_id). The commit loop walks state with runtime byte offsets read
        // as the per-(signal, edge) wait-queue base/cap/len pointers the block commit
        // hands to emit_nba_service. Live/shadow addressing uses compile-time offsets
        // (storage_ptr / shadow_ptr), so no byte offsets are baked here.
        auto*                        nullp = llvm::ConstantPointerNull::get(ptr);
        std::vector<llvm::Constant*> metas;
        // Parallel to `metas` (nba_id order): the dump-var index of each NBA-target
        // signal, or -1 when it is not a dumped scalar. Read in the commit loop to
        // mark the signal in the VCD dirty set.
        std::vector<llvm::Constant*> nba_vcd;
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            const SignalSlot& s = slots_[i];
            if (!s.nba) {
                continue;
            }
            // Parallel to metas: this NBA target's dump-var index, or -1 when it is
            // not a dumped scalar. The commit loop reads it to mark the signal dirty.
            nba_vcd.push_back(llvm::ConstantInt::get(
              i32, static_cast<uint64_t>(static_cast<int64_t>(design_uses_vcd_ ? vcd_index_[i] : -1))));
            std::array<llvm::Constant*, 3> sb{}, sc{}, db{}, dl{};
            for (uint32_t e = 0; e < 3; ++e) {
                sb[e] = wait_sta_gv_[i][e] ? llvm::cast<llvm::Constant>(sym(wait_sta_gv_[i][e])) : nullp;
                sc[e] = llvm::ConstantInt::get(i32, wait_sta_true_[i][e]);
                db[e] = wait_dyn_gv_[i][e] ? llvm::cast<llvm::Constant>(sym(wait_dyn_gv_[i][e])) : nullp;
                dl[e] = wait_dyn_len_gv_[i][e] ? llvm::cast<llvm::Constant>(sym(wait_dyn_len_gv_[i][e])) : nullp;
            }
            metas.push_back(llvm::ConstantStruct::get(
              nba_meta_ty_, { llvm::ConstantArray::get(arr3p, sb), llvm::ConstantArray::get(arr3i, sc),
                              llvm::ConstantArray::get(arr3p, db), llvm::ConstantArray::get(arr3p, dl) }));
        }
        if (metas.empty()) {
            metas.push_back(llvm::ConstantAggregateZero::get(nba_meta_ty_));
        }
        nba_meta_arr_ty_ = llvm::ArrayType::get(nba_meta_ty_, metas.size());
        nba_meta_gv_     = new llvm::GlobalVariable(env_->llmod, nba_meta_arr_ty_, true, llvm::GlobalValue::InternalLinkage,
                                                    llvm::ConstantArray::get(nba_meta_arr_ty_, metas), "clotho.nba_meta");

        // Parallel dump-var index array (nba_id order), consumed by the commit loop
        // to mark committed signals dirty in the VCD dirty set.
        if (design_uses_vcd_ && !nba_vcd.empty()) {
            auto* ty    = llvm::ArrayType::get(i32, nba_vcd.size());
            nba_vcd_gv_ = new llvm::GlobalVariable(env_->llmod, ty, true, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantArray::get(ty, nba_vcd), "clotho.nba_vcd");
        }

        // Declare the block-commit enqueue stub up front so StoreNb lowering can call
        // it (its body + the worklist/pending globals are emitted post-lowering by
        // finalize_block_nba, once the block count is known). Declared only when some
        // signal actually takes the block path.
        const bool any_block = std::any_of(slots_.begin(), slots_.end(), [](const SignalSlot& s) { return s.nba_block; });
        if (any_block) {
            auto* fnty = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), { i32 }, false);
            nba_block_enqueue_fn_ =
              llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.nba_block_enqueue", env_->llmod);
        }
    }

    // Register the current straight-line block (the live insert basic block) as the
    // owner of a whole-signal scalar NBA to `sig`. The first whole-scalar StoreNb in
    // a block emits one enqueue call (deduped at runtime by block_pending); every
    // target in the block is gathered into the block's commit function. Keyed on the
    // LLVM insert block, so back-to-back fused members that share one block coalesce
    // into a single enqueue + commit.
    void block_register(Signal sig, const llvm::APInt& mask, bool dynamic) {
        // Open a new block on the first NBA of a segment (the enqueue call is emitted
        // once, here, at that first store's insert point; it runs unconditionally
        // because same-epoch stores are straight-line co-executed).
        if (cur_block_epoch_ != static_cast<int64_t>(cur_seg_epoch_)) {
            cur_block_id_    = static_cast<uint32_t>(block_commits_.size());
            block_commits_.push_back({});
            cur_block_epoch_ = static_cast<int64_t>(cur_seg_epoch_);
            llvm::Value* id  = b_.getInt32(cur_block_id_);
            if (sched_anchor_ != SignalSlot::no_anchor) {
                Anchor&        a     = anchors_[sched_anchor_];
                const uint32_t field = a.ctx_blk_base + group_def_ordinal_[sched_group_];
                if (group_first_block_ < 0) {
                    group_first_block_ = static_cast<int64_t>(cur_block_id_);
                    ctx_blk_fills_.emplace_back(sched_anchor_, field, cur_block_id_);
                }
                id = b_.CreateAdd(
                  ctx_field(a, field, b_.getInt32Ty()),
                  b_.getInt32(cur_block_id_ - static_cast<uint32_t>(group_first_block_)));
            }
            b_.CreateCall(sym(nba_block_enqueue_fn_), { id });
        }
        std::vector<BlockSig>& sigs = block_commits_[cur_block_id_].sigs;
        auto it = std::find_if(sigs.begin(), sigs.end(), [&](const BlockSig& b) { return b.sig == sig; });
        if (it == sigs.end()) {
            sigs.push_back({ sig, mask, dynamic });
        } else {
            it->dynamic = it->dynamic || dynamic;
            if (!dynamic) {
                it->mask |= mask; // accumulate this block's static writes to the signal
            }
        }
    }

    // Sentinel stored in block_next[id] when block id is not enqueued (doubles as
    // the dedup marker); distinct from null, which marks the chain end.
    llvm::Constant* block_not_queued() {
        return llvm::ConstantExpr::getIntToPtr(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1),
                                               llvm::PointerType::getUnqual(ctx_));
    }
    // Threaded-dispatch tail for block `id`'s commit function, emitted at its single
    // exit block after the copy/service work: reset this block's link to NOT_QUEUED,
    // then musttail-call the next pending block (or ret at the chain end).
    void emit_block_commit_tail(uint32_t id) {
        auto*        ptr   = llvm::PointerType::getUnqual(ctx_);
        auto*        nullp = llvm::ConstantPointerNull::get(ptr);
        llvm::Value* nextp = b_.CreateInBoundsGEP(block_next_ty_, sym(block_next_gv_), { b_.getInt32(0), b_.getInt32(id) });
        llvm::Value* next  = b_.CreateLoad(ptr, nextp);
        b_.CreateStore(block_not_queued(), nextp); // reset self for next instant
        auto* fn     = b_.GetInsertBlock()->getParent();
        auto* tailbb = llvm::BasicBlock::Create(ctx_, "thr.tail", fn);
        auto* retbb  = llvm::BasicBlock::Create(ctx_, "thr.ret", fn);
        b_.CreateCondBr(b_.CreateICmpEQ(next, nullp), retbb, tailbb);

        b_.SetInsertPoint(tailbb);
        auto* cty  = llvm::FunctionType::get(b_.getVoidTy(), { ptr }, false);
        auto* call = b_.CreateCall(cty, next, { state_ });
        call->setTailCallKind(llvm::CallInst::TCK_MustTail);
        b_.CreateRetVoid();

        b_.SetInsertPoint(retbb);
        b_.CreateRetVoid();
    }

    // Emit block `id`'s commit function `void(state)`: one straight-line copy body per
    // whole-signal scalar target the block writes (shadow planes -> live state) and,
    // where the target has waiters, service its wait queues; then the threaded tail.
    void emit_block_commit_fn(uint32_t id) {
        auto* voidty = llvm::Type::getVoidTy(ctx_);
        auto* ptr    = llvm::PointerType::getUnqual(ctx_);
        auto* fnty   = llvm::FunctionType::get(voidty, { ptr }, false);
        auto* fn     = llvm::Function::Create(fnty, llvm::Function::InternalLinkage,
                                              "clotho.nba.commit_blk" + std::to_string(id), env_->llmod);
        block_commits_[id].fn = fn;

        // Save/restore the builder + current-state pointer: storage_ptr / shadow_ptr
        // read through `state`, emit_nba_service builds control flow at the insert point.
        llvm::IRBuilderBase::InsertPoint saved       = b_.saveIP();
        llvm::Value*                     saved_state = state_;
        state_      = fn->getArg(0);
        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
        b_.SetInsertPoint(entry);
        emit_block_commit_body(id);
        emit_block_commit_tail(id);
        b_.restoreIP(saved);
        state_ = saved_state;
    }

    // Block `id`'s commit body: one straight-line copy per signal (no loop), using
    // compile-time slot/shadow offsets. Emits into the current insert point (state_
    // already set to the commit fn's arg). No-waiter targets skip the edge compute +
    // wait-queue scan entirely.
    void emit_block_commit_body(uint32_t id) {
        for (const BlockSig& bs : block_commits_[id].sigs) {
            const SignalSlot& s       = slot_of(bs.sig);
            if (s.nba_agg) {
                emit_agg_commit(bs.sig, bs.mask);
                continue;
            }
            const uint32_t    W       = s.width;
            auto*             mmty     = int_ty(s.narrow ? W : 2 * W);
            auto*             zty      = int_ty(W);
            llvm::Value*      live     = storage_ptr(s);
            llvm::Value*      live_mm  = b_.CreateStructGEP(s.slot_ty, live, 0);
            llvm::Value*      sh_mm    = shadow_ptr(s, s.sh_mm_field);
            llvm::Value*      live_z   = s.has_z ? b_.CreateStructGEP(s.slot_ty, live, 1) : nullptr;
            llvm::Value*      sh_z     = s.has_z ? shadow_ptr(s, s.sh_z_field) : nullptr;
            llvm::Value*      old_mm   = b_.CreateLoad(mmty, live_mm);
            llvm::Value*      new_mm   = nullptr;
            llvm::Value*      old_z    = nullptr;
            llvm::Value*      new_z    = nullptr;

            if (!bs.dynamic && bs.mask.isAllOnes()) {
                // Whole-signal (or full-width) target: copy the planes directly.
                new_mm = b_.CreateLoad(mmty, sh_mm);
                b_.CreateStore(new_mm, live_mm);
                if (s.has_z) {
                    old_z = b_.CreateLoad(zty, live_z);
                    new_z = b_.CreateLoad(zty, sh_z);
                    b_.CreateStore(new_z, live_z);
                }
            } else {
                // Masked RMW: mask is a compile-time immediate for a static-select
                // target, or the runtime dirty field for a dynamic-select one.
                llvm::Value* maskw = bs.dynamic
                                       ? static_cast<llvm::Value*>(b_.CreateLoad(zty, shadow_ptr(s, s.sh_mask_field)))
                                       : static_cast<llvm::Value*>(llvm::ConstantInt::get(zty, bs.mask));
                // A narrow (iW) word masks directly; a packed (i2W) word replicates
                // the W-bit mask into both must|may halves.
                llvm::Value* mm_mask = s.narrow ? maskw
                                                : b_.CreateOr(b_.CreateZExt(maskw, mmty),
                                                              b_.CreateShl(b_.CreateZExt(maskw, mmty),
                                                                           llvm::ConstantInt::get(mmty, W)));
                llvm::Value* sh_mm_v = b_.CreateLoad(mmty, sh_mm);
                new_mm = b_.CreateOr(b_.CreateAnd(old_mm, b_.CreateNot(mm_mask)), b_.CreateAnd(sh_mm_v, mm_mask));
                b_.CreateStore(new_mm, live_mm);
                if (s.has_z) {
                    old_z                = b_.CreateLoad(zty, live_z);
                    llvm::Value* sh_z_v  = b_.CreateLoad(zty, sh_z);
                    new_z = b_.CreateOr(b_.CreateAnd(old_z, b_.CreateNot(maskw)), b_.CreateAnd(sh_z_v, maskw));
                    b_.CreateStore(new_z, live_z);
                }
                if (bs.dynamic) {
                    b_.CreateStore(zero(W), shadow_ptr(s, s.sh_mask_field)); // clear for next instant
                }
            }
            if (signal_has_waiters(bs.sig)) {
                llvm::Value* meta = b_.CreateInBoundsGEP(nba_meta_arr_ty_, sym(nba_meta_gv_),
                                                         { b_.getInt32(0), b_.getInt32(s.nba_id) });
                emit_nba_service(bs.sig, meta, old_mm, new_mm, W, s.narrow, old_z, new_z);
            }
            // Mark a dumped scalar target dirty in the VCD set (rt_vcd_touch ignores
            // the -1 index of a non-dumped signal). Commit is unconditional, so a
            // no-change commit over-touches; the flush diff suppresses it.
            if (design_uses_vcd_ && nba_vcd_gv_ != nullptr) {
                emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
                    return llvm::SmallVector<llvm::Value*, 2>{ b_.CreateLoad(
                      b_.getInt32Ty(), b_.CreateInBoundsGEP(nba_vcd_gv_->getValueType(), sym(nba_vcd_gv_),
                                                            { b_.getInt32(0), b_.getInt32(s.nba_id) })) };
                });
            }
        }
    }

    // Pointer to an aggregate leaf's mm (0) / z (1) field, via the leaf's static
    // constant GEP path from the aggregate storage root.
    llvm::Value* agg_leaf_ptr(llvm::Value* base, llvm::Type* root_ty,
                              const std::vector<uint32_t>& path, uint32_t field) {
        std::vector<llvm::Value*> idx;
        idx.reserve(path.size() + 2);
        idx.push_back(b_.getInt32(0));
        for (const uint32_t f : path) {
            idx.push_back(b_.getInt32(f));
        }
        idx.push_back(b_.getInt32(field));
        return b_.CreateInBoundsGEP(root_ty, base, idx);
    }

    // Commit an aggregate NBA target within a block: copy each static leaf whose
    // leaf-set bit is set from its shadow slot to live storage, then wake level
    // (Any-edge) readers if any leaf changed. A leaf is a whole slot (not a bit
    // range), so this copies planes, not masked words; edge sensitivity on an
    // aggregate member is rejected in the frontend, so Any is the only bucket.
    void emit_agg_commit(Signal sig, const llvm::APInt& mask) {
        const SignalSlot& s       = slot_of(sig);
        llvm::Value*      live    = storage_ptr(s);
        llvm::Value*      changed = b_.getFalse();
        for (uint32_t k = 0; k < s.static_leaves.size(); ++k) {
            if (!mask[k]) {
                continue;
            }
            const AggLeaf& m       = s.static_leaves[k];
            auto*          mmty    = int_ty(m.narrow ? m.width : 2 * m.width);
            llvm::Value*   live_mm = agg_leaf_ptr(live, s.storage_ty, m.path, 0);
            llvm::Value*   old_mm  = b_.CreateLoad(mmty, live_mm);
            llvm::Value*   new_mm  = b_.CreateLoad(mmty, shadow_ptr(s, m.sh_mm_field));
            b_.CreateStore(new_mm, live_mm);
            changed = b_.CreateOr(changed, b_.CreateICmpNE(old_mm, new_mm));
            if (m.has_z) {
                auto*        zty    = int_ty(m.width);
                llvm::Value* live_z = agg_leaf_ptr(live, s.storage_ty, m.path, 1);
                llvm::Value* old_z  = b_.CreateLoad(zty, live_z);
                llvm::Value* new_z  = b_.CreateLoad(zty, shadow_ptr(s, m.sh_z_field));
                b_.CreateStore(new_z, live_z);
                changed = b_.CreateOr(changed, b_.CreateICmpNE(old_z, new_z));
            }
        }
        if (signal_has_waiters(sig)) {
            auto* fn   = b_.GetInsertBlock()->getParent();
            auto* svc  = llvm::BasicBlock::Create(ctx_, "uagg.svc", fn);
            auto* cont = llvm::BasicBlock::Create(ctx_, "uagg.cont", fn);
            b_.CreateCondBr(changed, svc, cont);
            b_.SetInsertPoint(svc);
            service_any_waiters(sig);
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
        }
    }

    // Finalize the block-commit path after all bodies are lowered: size the
    // threaded-dispatch globals to the discovered block count, emit each block's
    // commit function, and emit the enqueue stub body. No-op when the path was never
    // used.
    void finalize_block_nba() {
        sched_anchor_ = SignalSlot::no_anchor; // commit machinery addresses absolutely
        if (nba_block_enqueue_fn_ == nullptr) {
            return;
        }
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        // A declared-but-unused stub (no block ever registered) still needs a body to
        // verify; it is never called.
        const uint32_t n = std::max<uint32_t>(static_cast<uint32_t>(block_commits_.size()), 1);

        // Threaded-dispatch globals. Must exist before the commit-fn loop below,
        // whose emitted tails reference block_next. Links init to NOT_QUEUED, head
        // to null (empty chain).
        block_next_ty_ = llvm::ArrayType::get(ptr, n);
        block_next_gv_ = new llvm::GlobalVariable(
          env_->llmod, block_next_ty_, false, llvm::GlobalValue::InternalLinkage,
          llvm::ConstantArray::get(block_next_ty_, std::vector<llvm::Constant*>(n, block_not_queued())),
          "clotho.nba_block_next");
        block_head_gv_ = new llvm::GlobalVariable(env_->llmod, ptr, false, llvm::GlobalValue::InternalLinkage,
                                                  llvm::ConstantPointerNull::get(ptr), "clotho.nba_block_head");

        std::vector<llvm::Constant*> fns;
        for (uint32_t id = 0; id < block_commits_.size(); ++id) {
            emit_block_commit_fn(id);
            fns.push_back(sym(block_commits_[id].fn));
        }
        if (fns.empty()) {
            fns.push_back(llvm::ConstantPointerNull::get(ptr));
        }
        block_commit_fns_ty_ = llvm::ArrayType::get(ptr, fns.size());
        block_commit_fns_gv_ = new llvm::GlobalVariable(env_->llmod, block_commit_fns_ty_, true,
                                                        llvm::GlobalValue::InternalLinkage,
                                                        llvm::ConstantArray::get(block_commit_fns_ty_, fns),
                                                        "clotho.nba_block_commit_fns");

        // Enqueue stub body: if block id is not queued (block_next[id] == NOT_QUEUED),
        // push it on the intrusive chain (block_next[id] = block_head;
        // block_head = commit_fns[id]); else it is already pending, so do nothing.
        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", nba_block_enqueue_fn_);
        auto* push  = llvm::BasicBlock::Create(ctx_, "push", nba_block_enqueue_fn_);
        auto* done  = llvm::BasicBlock::Create(ctx_, "done", nba_block_enqueue_fn_);
        llvm::IRBuilder<> tb(entry);
        llvm::Value*      id    = nba_block_enqueue_fn_->getArg(0);
        llvm::Value*      nextp = tb.CreateInBoundsGEP(block_next_ty_, sym(block_next_gv_), { tb.getInt32(0), id });
        tb.CreateCondBr(tb.CreateICmpEQ(tb.CreateLoad(ptr, nextp), block_not_queued()), push, done);
        tb.SetInsertPoint(push);
        tb.CreateStore(tb.CreateLoad(ptr, sym(block_head_gv_)), nextp);
        llvm::Value* fn = tb.CreateLoad(
          ptr, tb.CreateInBoundsGEP(block_commit_fns_ty_, sym(block_commit_fns_gv_), { tb.getInt32(0), id }));
        tb.CreateStore(fn, sym(block_head_gv_));
        tb.CreateBr(done);
        tb.SetInsertPoint(done);
        tb.CreateRetVoid();
    }

    // The dense trigger id for a (signal, edge) that fires a `<= @e` deferred store,
    // or -1. Both wake paths call this to decide whether to fire event records.
    int32_t trigger_id(Signal sig, uint32_t edge) const {
        return trigger_id_.empty() ? -1 : trigger_id_[static_cast<uint32_t>(sig)][edge];
    }

    bool signal_has_waiters(Signal sig) const {
        const auto i = static_cast<uint32_t>(sig);
        for (uint32_t e = 0; e < 3; ++e) {
            // A `<= @e` trigger has no coroutine waiter but its edge must still be
            // watched so a store/commit fires its deferred records.
            if (wait_sta_gv_[i][e] != nullptr || wait_dyn_gv_[i][e] != nullptr ||
                trigger_id(sig, e) >= 0) {
                return true;
            }
        }
        return false;
    }

    // Whether any waiter subscribed a level (any-change) wait on the signal. Only
    // these consume the z word in edge classification, so a store may skip the z
    // snapshot when none exist.
    bool signal_has_level_waiters(Signal sig) const {
        const auto i = static_cast<uint32_t>(sig);
        const auto e = static_cast<uint32_t>(Edge::Any);
        return wait_sta_gv_[i][e] != nullptr || wait_dyn_gv_[i][e] != nullptr;
    }

    // Load a signal's packed must|may state word (field 0 of the slot struct).
    llvm::Value* load_mm(Signal sig) {
        const SignalSlot& s      = slot_of(sig);
        llvm::Value*      mm_ptr = b_.CreateStructGEP(s.slot_ty, storage_ptr(s), 0);
        return b_.CreateAlignedLoad(
          int_ty(2 * s.width), mm_ptr,
          env_->llmod.getDataLayout().getABITypeAlign(int_ty(2 * s.width)));
    }

    // Load the packed must|may word of an lvalue's selected storage: the whole
    // signal for a plain target, or one element for an unpacked-array element.
    // Used to snapshot old / new state around a blocking store so wait-queue
    // servicing detects an element-level change, not just element 0.
    // Bit width of a slot's stored value word: iW for a narrow 2-state slot, the
    // packed i(2W) otherwise.
    uint32_t slot_mm_bits(const LValue& lv) const {
        return lv.narrow ? lv.store_width : 2 * lv.store_width;
    }

    llvm::Value* load_lv_mm(const LValue& lv) {
        return b_.CreateLoad(int_ty(slot_mm_bits(lv)), lv_mm_ptr(lv));
    }

    llvm::Value* load_lv_z(const LValue& lv) {
        return b_.CreateLoad(int_ty(lv.store_width), lv_z_ptr(lv));
    }

    // Pre-write snapshot of an lvalue's selected storage, taken so wait-queue
    // servicing can classify the old -> new transition. `mm` is null when the
    // write never wakes anyone; `z` is non-null only when a level waiter exists
    // to consume the z word (a z-only x<->z transition is a value change,
    // LRM 9.4.2, but edges never read z).
    struct ServiceSnapshot {
        llvm::Value* mm = nullptr;
        llvm::Value* z  = nullptr;
    };

    // A blocking write to a module signal must service the signal's wait queues
    // with the old -> new transition. This pair brackets every such write path
    // (Store, plus the runtime pointer write-backs: $fgets / $sformat / $ferror /
    // $*scanf / scalar $fread): snapshot the selected storage before the write,
    // then detect the transition and service after. The snapshot is empty — and
    // servicing a no-op — for a target that never wakes anyone: a frame local, a
    // signal without waiters, or a PreInit store (variable declaration initializers
    // generate no events).
    ServiceSnapshot service_snapshot(const LValue& lv) {
        const bool service = !lowering_preinit_ && lv.is_signal && signal_has_waiters(lv.sig);
        if (!service) {
            return {};
        }
        ServiceSnapshot old;
        old.mm = load_lv_mm(lv);
        if (lv.has_z && signal_has_level_waiters(lv.sig)) {
            old.z = load_lv_z(lv);
        }
        return old;
    }

    void service_after_write(const LValue& lv, ServiceSnapshot old) {
        if (old.mm != nullptr) {
            service_wait_queue(lv.sig, old.mm, load_lv_mm(lv), old.z,
                               old.z != nullptr ? load_lv_z(lv) : nullptr);
        }
        // Mark the signal changed for the VCD dirty set. Independent of wait-queue
        // servicing: a dumped signal with no waiters still needs a touch.
        vcd_touch_lvalue(lv);
    }

    // One old -> new transition's wake facts: which edge buckets fire. A fact is
    // null when its bucket was not requested (no waiters of that kind).
    struct EdgeFacts {
        llvm::Value* pos     = nullptr; // Table 9-2 posedge (a move toward 1)
        llvm::Value* neg     = nullptr; // Table 9-2 negedge (a move toward 0)
        llvm::Value* changed = nullptr; // any value change (level waits)
    };

    // Classify a store's old -> new transition of a packed must|may word into wake
    // facts per LRM 9.4.2 / Table 9-2. The edge state of bit 0 forms the lattice
    // 0 < x/z < 1 — x and z encode identically in the planes (must 0 / may 1), and
    // the LRM defines no edge between them — so posedge is any move up the lattice
    // (0->1, 0->x, x->1) and negedge any move down. A 2-state slot cannot hold x,
    // so its classifier stays the plain 0<->1 compare. Edges read only the LSB of
    // each plane (LRM 9.4.2: an edge is detected only on the LSB).
    //
    // `changed` compares whole words: both planes and, when the slot carries a z
    // word (old_z / new_z non-null), the z word too — x->z is a value change for a
    // level wait even though the planes agree. Facts whose need flag is false stay
    // null and emit no code.
    EdgeFacts emit_edge_facts(llvm::Value* old_mm, llvm::Value* new_mm, uint32_t width,
                              bool four_state, llvm::Value* old_z, llvm::Value* new_z,
                              bool need_pos, bool need_neg, bool need_any) {
        auto*     i1 = b_.getInt1Ty();
        EdgeFacts f;
        if (need_pos || need_neg) {
            llvm::Value* old_m = b_.CreateTrunc(old_mm, i1); // must LSB
            llvm::Value* new_m = b_.CreateTrunc(new_mm, i1);
            if (four_state) {
                auto*        sh      = llvm::ConstantInt::get(old_mm->getType(), width);
                llvm::Value* old_may = b_.CreateTrunc(b_.CreateLShr(old_mm, sh), i1);
                llvm::Value* new_may = b_.CreateTrunc(b_.CreateLShr(new_mm, sh), i1);
                if (need_pos) {
                    f.pos = b_.CreateAnd(b_.CreateAnd(b_.CreateNot(old_m), new_may),
                                         b_.CreateOr(b_.CreateNot(old_may), new_m));
                }
                if (need_neg) {
                    f.neg = b_.CreateAnd(b_.CreateAnd(old_may, b_.CreateNot(new_m)),
                                         b_.CreateOr(b_.CreateNot(new_may), old_m));
                }
            } else {
                if (need_pos) {
                    f.pos = b_.CreateAnd(b_.CreateNot(old_m), new_m);
                }
                if (need_neg) {
                    f.neg = b_.CreateAnd(old_m, b_.CreateNot(new_m));
                }
            }
        }
        if (need_any) {
            f.changed = b_.CreateICmpNE(old_mm, new_mm);
            if (old_z != nullptr) {
                f.changed = b_.CreateOr(f.changed, b_.CreateICmpNE(old_z, new_z));
            }
        }
        return f;
    }

    // Service a signal's wait queues after a blocking assignment to it. Computes
    // the transition's edge facts once, then for each per-edge bucket that exists
    // guards its whole scan with a single branch on the matching fact — no
    // per-entry edge check. Emits nothing for a signal with no waiters. old_z /
    // new_z are the z-word snapshots, or null when the slot has no z word or no
    // level waiter consumes it.
    //
    // The three buckets are serviced independently (not else-if): a 0->1 LSB
    // transition is both `changed` (any) and `pos`, so it services both buckets.
    void service_wait_queue(Signal sig, llvm::Value* old_mm, llvm::Value* new_mm,
                            llvm::Value* old_z, llvm::Value* new_z) {
        const auto i = static_cast<uint32_t>(sig);
        if (!signal_has_waiters(sig)) {
            return;
        }
        const auto has_bucket = [&](Edge e) {
            const auto k = static_cast<uint32_t>(e);
            return wait_sta_gv_[i][k] != nullptr || wait_dyn_gv_[i][k] != nullptr ||
                   trigger_id(sig, k) >= 0;
        };
        const SignalSlot& slot = slot_of(sig);
        const EdgeFacts   f =
          emit_edge_facts(old_mm, new_mm, slot.width, slot.four_state, old_z, new_z,
                          has_bucket(Edge::Positive), has_bucket(Edge::Negative),
                          has_bucket(Edge::Any));

        // Bucket -> matching fact. `pos`/`neg` each imply `changed`, so the Any
        // bucket also fires on an LSB transition.
        const std::array<std::pair<uint32_t, llvm::Value*>, 3> buckets = {
            std::make_pair(static_cast<uint32_t>(Edge::Any), f.changed),
            std::make_pair(static_cast<uint32_t>(Edge::Positive), f.pos),
            std::make_pair(static_cast<uint32_t>(Edge::Negative), f.neg),
        };
        for (const auto& [e, fact] : buckets) {
            if (fact == nullptr) {
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
            fire_event_trigger(sig, e);
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
        }
    }

    // If (sig, edge) fires a `<= @e` deferred store, count down its records and
    // move any that reach zero to their target ready lists (rt_intra_event_fire).
    // Called from both wake paths inside a block already guarded by the edge fact.
    void fire_event_trigger(Signal sig, uint32_t edge) {
        const int32_t tid = trigger_id(sig, edge);
        if (tid >= 0) {
            b_.CreateCall(intra_event_fire_fn(), { b_.getInt32(static_cast<uint32_t>(tid)) });
        }
    }

    // Static sub-queue scan (one edge bucket): a fixed-trip loop over the
    // persistent subscriptions for this (signal, edge). The bucket's edge already
    // matched the transition (the caller guarded the whole scan), so there is no
    // per-entry edge check. Emitted via the generic runtime scan with a constant
    // non-null base and constant cap: simplifycfg (lower_coroutines) folds the
    // dead null guard and the constant trip count still lets the optimizer
    // unroll. Membership never changes — no compaction, no length.
    // Body-side base pointer of a wait bucket: loaded from the owning
    // instance's wait-pointer table when its definition is reused (def-local
    // slot constant — twin bodies stay identical), the absolute queue global
    // otherwise. Commit functions / the seed loop keep the globals directly.
    llvm::Value* wait_base(uint32_t slot, llvm::Constant* gv, uint32_t sig) {
        if (slot == no_wait_slot || !def_relative_for(slots_[sig])) {
            return sym(llvm::cast<llvm::GlobalVariable>(gv));
        }
        Anchor& a = anchors_[slots_[sig].anchor];
        return ctx_field(a, a.ctx_wait_base + slot, llvm::PointerType::getUnqual(ctx_));
    }

    void service_static_queue(Signal sig, uint32_t edge) {
        const auto i = static_cast<uint32_t>(sig);
        emit_static_scan_rt(wait_base(wait_slot_sta_[i][edge], wait_sta_gv_[i][edge], i),
                            b_.getInt32(wait_sta_cap_[i][edge]), wait_sta_padded_[i][edge] != 0);
    }

    // Dynamic sub-queue scan (one edge bucket): a straight loop over the live
    // one-shot waiters for this (signal, edge), via the generic runtime scan
    // with a constant non-null base. The whole bucket is consumed (length
    // resets to 0); the firing process deregisters its stale entries from any
    // OTHER buckets itself on resume (eager removal in lower_wait).
    void service_dynamic_queue(Signal sig, uint32_t edge) {
        const auto i = static_cast<uint32_t>(sig);
        emit_dynamic_scan_rt(wait_base(wait_slot_dyn_[i][edge], wait_dyn_gv_[i][edge], i),
                             wait_base(wait_slot_len_[i][edge], wait_dyn_len_gv_[i][edge], i));
    }

    // Lower `-> e` (LRM 15.5.2): stamp the current sim tick into the event's
    // trigger timestamp (so `.triggered` reads 1 for the rest of the step) and
    // wake every waiter now. A named event has no value, so — unlike a signal
    // store — there is no edge computation: it services the Any-edge static and
    // dynamic queues unconditionally. `@(e)` and `always @(e)` both subscribe the
    // Any bucket (edge None -> Any), so this reaches every waiter.
    void lower_event_trigger(Signal sig) {
        emit_event_fire(sig, wheel_);
    }

    // Generate a detached on-expiry thunk per `->> #d e` target event. The wheel
    // calls it as on_expire(rt_timer* t, void* ctx) with ctx == the wheel (see the
    // drain's rt_wheel_advance ctx); it stamps the event's trigger time and
    // services its waiters, then frees the timer node the schedule site allocated.
    void build_delay_event_thunks() {
        event_delay_thunk_.assign(mod_.signals().size(), nullptr);
        if (nb_delay_event_sigs_.empty()) {
            return;
        }
        auto* ptr  = raw_ptr_ty();
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr }, false);
        for (const Signal sig : nb_delay_event_sigs_) {
            const auto i  = static_cast<uint32_t>(sig);
            auto*      fn = llvm::Function::Create(fnty, llvm::Function::InternalLinkage,
                                                   "clotho.ev_delay." + mod_.signal(sig).name, env_->llmod);
            event_delay_thunk_[i] = fn;
            b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
            emit_event_fire(sig, fn->getArg(1));         // ctx (arg 1) is the wheel
            b_.CreateCall(free_fn(), { fn->getArg(0) }); // free the detached timer node
            b_.CreateRetVoid();
        }
    }

    // Stamp event `sig`'s trigger time (from `wheel`) and service its Any-edge
    // static/dynamic wait queues. Shared by `-> e` in a process body (wheel_) and
    // the NBA-region `->> e` service (the drain's local wheel — wheel_ is stale
    // there, so it must be passed in).
    void emit_event_fire(Signal sig, llvm::Value* wheel) {
        const auto   i   = static_cast<uint32_t>(sig);
        llvm::Value* now = b_.CreateCall(wheel_now_fn(), { wheel });
        b_.CreateStore(now, sym(event_trig_gv_[i]));
        const auto any = static_cast<uint32_t>(Edge::Any);
        if (wait_sta_gv_[i][any] != nullptr) {
            service_static_queue(sig, any);
        }
        if (wait_dyn_gv_[i][any] != nullptr) {
            service_dynamic_queue(sig, any);
        }
    }

    // Lower `e.triggered` (LRM 15.5.3) to a definite 2-state 1-bit: 1 iff the last
    // trigger tick equals the current tick (true across all delta cycles of the
    // step, false once time advances — no per-step reset needed).
    Plane lower_event_triggered(Signal sig) {
        const auto   i     = static_cast<uint32_t>(sig);
        llvm::Value* stamp = b_.CreateLoad(b_.getInt64Ty(), sym(event_trig_gv_[i]));
        llvm::Value* now   = b_.CreateCall(wheel_now_fn(), { wheel_ });
        return { b_.CreateICmpEQ(stamp, now), nullptr, true };
    }

    // Compacting removal of this coroutine's entries from one (signal, edge)
    // dynamic bucket, used on resume from a multi-signal wait: the firing bucket
    // already cleared us, but the others still hold our now-stale entry. Slides
    // every entry whose handle != hdl_ down to fill the gaps and trims the length:
    //   w = 0; for i in 0..len: if Q[i].handle != hdl_ { Q[w++] = Q[i] } ; len = w
    void emit_dynamic_remove(Signal sig, uint32_t edge) {
        const auto        i      = static_cast<uint32_t>(sig);
        llvm::Value*      q      = wait_base(wait_slot_dyn_[i][edge], wait_dyn_gv_[i][edge], i);
        auto*             arr_ty = wait_dyn_ty_[i][edge];
        llvm::Value*      len_gv = wait_base(wait_slot_len_[i][edge], wait_dyn_len_gv_[i][edge], i);
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
    // global used as a FIFO ring (active events may fire in any order per the LRM,
    // so the discipline is free). FIFO over LIFO is deliberate: wakes enqueue
    // behind the current batch, so combinational cascades settle breadth-first —
    // each process runs at most once per wave and the worst case is bounded by
    // net depth, where LIFO's depth-first re-entry is exponential on dense
    // fan-in. It also runs same-instant processes in declaration order.
    // Created once, lazily, so the timer callback (built while
    // lowering a process) and emit_active_run share it.
    // Number of fork-child bodies in the design = Σ fork widths (each fork child
    // is one Function). Bounds the extra active-ring slots `fork … join` needs.
    size_t fork_child_count() const {
        size_t n = 0;
        for (size_t k = 0; k < mod_.function_count(); ++k) {
            if (mod_.function(static_cast<FunctionId>(k)).is_fork_child) {
                ++n;
            }
        }
        return n;
    }

    void ensure_active_queue() {
        if (active_gv_ != nullptr) {
            return;
        }
        auto*      ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*      i32    = llvm::Type::getInt32Ty(ctx_);
        // One LIVE slot per process suffices even with tasks: the scheduled flag
        // dedupes wakes, and (without fork) each process is a single call-stack
        // with at most one ready coroutine at a time — a task handle, or the
        // parent it re-enqueues on completion, reuses that chain's one slot.
        // Rounded up to a power of two so the monotone head/tail indices wrap
        // with a mask (total pushes per drain are unbounded — a process may be
        // re-woken after it runs — only the live count is bounded).
        //
        // `fork … join` (LRM 9.3.2) adds live children: a `join` parent blocks
        // (leaving the ring — its handle sits in the join record), so children
        // never accumulate across loop iterations or recursion depth. Peak ready
        // children <= the number of distinct fork-child bodies (Σ fork widths),
        // since `join` serializes each fork's reuse. Reserve one slot per child
        // (docs/design/fork-join.md). join_any / join_none are unbounded and
        // rejected by the frontend until V2's growable ring.
        const auto cap =
          std::bit_ceil(std::max<size_t>(mod_.processes().size() + fork_child_count(), 1));
        active_mask_   = static_cast<uint32_t>(cap - 1);

        queue_arr_ty_ = llvm::ArrayType::get(ptr_ty, cap);
        active_gv_    = new llvm::GlobalVariable(env_->llmod, queue_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                                 llvm::ConstantAggregateZero::get(queue_arr_ty_), "clotho.active");
        // Ring indices: pop at head, push at tail, empty iff equal. Monotone
        // u32s (masked on access), so u32 wraparound is harmless.
        active_head_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantInt::get(i32, 0), "clotho.active_head");
        active_tail_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantInt::get(i32, 0), "clotho.active_tail");
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
        inactive_gv_ = new llvm::GlobalVariable(env_->llmod, queue_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                                llvm::ConstantAggregateZero::get(queue_arr_ty_), "clotho.inactive");
        inactive_top_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                                    llvm::ConstantInt::get(i32, 0), "clotho.inactive_top");
    }

    // Push a handle onto the active ring: queue[tail++ & mask] = handle.
    void emit_active_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
        ensure_active_queue();
        auto*        i32  = b.getInt32Ty();
        llvm::Value* tl   = b.CreateLoad(i32, sym(active_tail_gv_));
        llvm::Value* idx  = b.CreateAnd(tl, b.getInt32(active_mask_));
        llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, sym(active_gv_), { b.getInt32(0), idx });
        b.CreateStore(handle, slot);
        b.CreateStore(b.CreateAdd(tl, b.getInt32(1)), sym(active_tail_gv_));
    }

    // Push a handle onto the inactive queue: inactive[top++] = handle.
    void emit_inactive_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
        ensure_inactive_queue();
        auto*        i32  = b.getInt32Ty();
        llvm::Value* sp   = b.CreateLoad(i32, sym(inactive_top_gv_));
        llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, sym(inactive_gv_), { b.getInt32(0), sp });
        b.CreateStore(handle, slot);
        b.CreateStore(b.CreateAdd(sp, b.getInt32(1)), sym(inactive_top_gv_));
    }

    // -- NBA update region --
    //
    // After the active queue drains, the NBA region commits every staged update:
    // Load edge element `e` of meta array field `field`. Meta layout (per nba-id):
    // 0 sta_base[3], 1 sta_cap[3], 2 dyn_base[3], 3 dyn_len[3] — the wait-queue
    // pointers emit_nba_service scans per edge.
    llvm::Value* meta_at(llvm::Value* meta, unsigned field, uint32_t e, llvm::Type* ty) {
        return b_.CreateLoad(
          ty, b_.CreateInBoundsGEP(nba_meta_ty_, meta, { b_.getInt32(0), b_.getInt32(field), b_.getInt32(e) }));
    }

    // Generic static-queue scan (base + capacity values), null base => no
    // waiters. Serves both callers: service_static_queue passes a constant
    // global and cap; the NBA commit loop passes values read from meta. Each
    // entry wakes its process only if its scheduled flag is 0, then sets the
    // flag — collapsing a multi-signal sensitivity (the same process
    // subscribed under several signals) into a single wake.
    // `padded`: the bucket is capacity-padded to its definition's maximum (see
    // build_wait_queues), so trailing slots are zeroinit — skip a null flag
    // pointer instead of dereferencing it. Exact buckets keep the check-free
    // scan.
    void emit_static_scan_rt(llvm::Value* base, llvm::Value* cap, bool padded = false) {
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
        if (padded) {
            auto* live = llvm::BasicBlock::Create(ctx_, "nsta.live", fn);
            b_.CreateCondBr(b_.CreateICmpNE(fptr, llvm::ConstantPointerNull::get(ptr)), live, cont);
            b_.SetInsertPoint(live);
        }
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

    // Generic dynamic-queue scan (base + length pointer values), null base =>
    // skip. Serves both callers, like emit_static_scan_rt. Wakes every live
    // entry (flag-deduped) and resets the length to 0.
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
    // the meta record (runtime) so this drives any signal with one body. Which
    // per-edge queues are populated is not known at compile time here (the commit
    // loop is shared per bucket), so all three facts are computed. `width` is the
    // bucket's static width; old_z / new_z are null for a never-z bucket. A
    // narrow bucket's word is a single iW with no may plane, so it must take the
    // plain 0<->1 classifier — deriving `may` via LShr on the iW word would read
    // 0 and suppress every posedge. A packed 2-state target keeps must == may in
    // every stored word, so the 4-state classifier stays exact for it.
    void emit_nba_service(Signal sig, llvm::Value* meta, llvm::Value* old_mm, llvm::Value* new_mm,
                          uint32_t width, bool narrow, llvm::Value* old_z, llvm::Value* new_z) {
        const EdgeFacts f = emit_edge_facts(old_mm, new_mm, width, /*four_state=*/!narrow, old_z,
                                            new_z, /*need_pos=*/true, /*need_neg=*/true,
                                            /*need_any=*/true);
        const std::array<std::pair<uint32_t, llvm::Value*>, 3> buckets = {
            std::make_pair(static_cast<uint32_t>(Edge::Any), f.changed),
            std::make_pair(static_cast<uint32_t>(Edge::Positive), f.pos),
            std::make_pair(static_cast<uint32_t>(Edge::Negative), f.neg),
        };
        for (const auto& [e, fact] : buckets) {
            auto* fn   = b_.GetInsertBlock()->getParent();
            auto* body = llvm::BasicBlock::Create(ctx_, "nsvc.body", fn);
            auto* cont = llvm::BasicBlock::Create(ctx_, "nsvc.cont", fn);
            b_.CreateCondBr(fact, body, cont);
            b_.SetInsertPoint(body);
            auto* ptr = raw_ptr_ty();
            auto* i32 = b_.getInt32Ty();
            emit_static_scan_rt(meta_at(meta, 0, e, ptr), meta_at(meta, 1, e, i32));
            emit_dynamic_scan_rt(meta_at(meta, 2, e, ptr), meta_at(meta, 3, e, ptr));
            // An NBA commit to a `<= @e` trigger fires its records too (the other
            // wake path is a blocking store, in service_wait_queue).
            fire_event_trigger(sig, e);
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
        }
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
        auto* ptr_ty        = llvm::PointerType::getUnqual(ctx_);
        auto* i32           = b_.getInt32Ty();
        auto* proc_fnty     = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
        const bool uses_vcd = design_uses_vcd();

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
        b_.CreateStore(b_.getInt32(0), sym(active_head_gv_));
        b_.CreateStore(b_.getInt32(0), sym(active_tail_gv_));
        const bool has_inactive = inactive_gv_ != nullptr; // some #0 was lowered
        if (has_inactive) {
            b_.CreateStore(b_.getInt32(0), sym(inactive_top_gv_));
        }
        // The array-NBA record buffers are process-global; clear any state a prior
        // simulation in this process left staged (e.g. a $finish between staging and
        // the NBA commit) so this run starts empty.
        if (!array_nba_sigs_.empty() || !agg_nba_sigs_.empty()) {
            b_.CreateCall(nba_arr_reset_fn(), {});
        }
        // Same for the deferred intra-assignment record pools — reclaim any records
        // a prior $finish left armed on the wheel.
        if (!intra_nba_sigs_.empty()) {
            b_.CreateCall(intra_reset_fn(), {});
        }
        const auto all = mod_.processes();

        // Pre-init pass first (LRM 6.8): apply variable declaration initializers
        // before any Initial / Always process is created or subscribed. A PreInit
        // body has no timing, so resuming it once runs it to completion; its stores
        // generate no events (see lower_process), so the always blocks created
        // below subscribe to already-settled values. Run before the seed loop so no
        // wait queue is scanned before priming.
        for (uint32_t g = 0; g < procs.size(); ++g) {
            if (all[groups_[g].front()].activation != Activation::PreInit) {
                continue;
            }
            llvm::Value* hdl = b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
            b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
            b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
        }

        // One handle per fusion group; a group's activation facts come from its
        // first member (identical across fused members by the group key).
        std::vector<llvm::Value*> hdls(procs.size(), nullptr);
        for (uint32_t g = 0; g < procs.size(); ++g) {
            const uint32_t k = groups_[g].front();
            const Process& p = all[k];
            if (p.activation == Activation::PreInit) {
                continue; // already run above
            }
            if (p.activation == Activation::Final) {
                continue; // deferred to emit_finals_run at end of simulation
            }
            // A thunk group is not ramped (no frame): its "handle" is the thunk
            // function pointer with the low bit set, so the drain calls it
            // directly instead of resuming a coroutine. Function addresses are
            // aligned, so bit 0 is a free tag; coro handles (rt_alloc) leave it
            // clear. The tagged pointer flows through subscription / self_start
            // push unchanged.
            llvm::Value* hdl = thunk_group_[g]
              ? b_.CreateIntToPtr(
                  b_.CreateOr(b_.CreatePtrToInt(sym(procs[g]), b_.getInt64Ty()), b_.getInt64(1)), ptr_ty)
              : b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
            hdls[g]          = hdl;
            if (p.activation == Activation::Always && !p.wait_events.empty()) {
                // Subscribed in the topo-ordered pass below.
                //
                // A continuously-active process (continuous assign / port copy /
                // always_comb) also runs once now, so it settles from the pre-init
                // values rather than waiting for an input edge that a declaration
                // initializer never generates. It runs body-first then parks on its
                // (subscribed-below) wait, so later input changes still wake it.
                //
                // Mark it scheduled as it is enqueued: a seed-time store from a
                // process draining earlier in the same first drain (an initial
                // block) services this static subscription and would re-push the
                // already-queued handle without the flag, so N such processes
                // overflow the nproc-sized active queue. The flag is cleared
                // when the body reaches its wait-suspend (lower_process always-loop).
                if (p.self_start) {
                    llvm::Constant* flag = sched_flag_ptr(k);
                    b_.CreateStore(b_.getInt32(1), flag);
                    emit_active_push(b_, hdl);
                }
            } else {
                emit_active_push(b_, hdl);
            }
        }

        // Static subscriptions, in topological order of the blocking-write wake
        // graph (compute_subscription_order): bucket entry order is subscription
        // order, the FIFO drain pops in bucket order, so a settle wave runs
        // producers before consumers and each acyclic cascade settles in one
        // wave. A separate pass so only wake order changes — creation and
        // enqueue order above stay declaration order. All subscriptions still
        // land before the drain, so no scan reads an empty slot.
        // A fused group subscribes once, at its earliest member's rank, with the
        // group's canonical event set and the group's flag/handle.
        std::vector<bool> subscribed(procs.size(), false);
        for (const uint32_t k : sub_order_) {
            const uint32_t g = group_of_[k];
            const uint32_t f = groups_[g].front();
            if (subscribed[g] || all[f].activation != Activation::Always || all[f].wait_events.empty()) {
                continue;
            }
            subscribed[g]        = true;
            llvm::Constant* flag = sched_flag_ptr(f);
            for (const WaitEvent& w : group_events_[g]) {
                emit_static_subscribe(w.signal, static_cast<uint32_t>(w.edge), hdls[g], flag);
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
        // Any thunk group present means the active queue can carry tagged thunk
        // pointers, so the drain must discriminate; a coro-only design keeps the
        // straight resume path (no per-pop tag test).
        const bool has_thunk = std::any_of(thunk_group_.begin(), thunk_group_.end(),
                                            [](bool t) { return t; });
        auto* thunk_fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty, ptr_ty }, false);
        // NBA region: only when the design has nonblocking assignments. Entered when
        // the active queue drains; if any update is staged it commits them all and
        // re-drains the active queue at the SAME instant; otherwise it falls through
        // to the wheel. So active and NBA iterate until both settle before time moves.
        const bool has_nba = !array_nba_sigs_.empty() || !agg_nba_sigs_.empty() ||
                             !block_commits_.empty() || !nb_event_sigs_.empty() ||
                             !intra_nba_sigs_.empty();
        auto*      ncheck  = has_nba ? llvm::BasicBlock::Create(ctx_, "nba.check", fn) : wcheck;
        auto*      ncommit = has_nba ? llvm::BasicBlock::Create(ctx_, "nba.commit", fn) : nullptr;
        // Inactive region: only when the design has a #0. When the active queue
        // drains, promote the whole inactive batch back into the (now empty)
        // active queue and re-drain at the SAME instant — before NBA. So active
        // and inactive iterate until both settle, then NBA, then the wheel.
        auto* icheck   = has_inactive ? llvm::BasicBlock::Create(ctx_, "inactive.check", fn) : ncheck;
        auto* ipromote = has_inactive ? llvm::BasicBlock::Create(ctx_, "inactive.promote", fn) : nullptr;
        b_.CreateBr(check);

        // Drain the active ring: while head != tail, pop active[head++ & mask]
        // and resume it.
        b_.SetInsertPoint(check);
        llvm::Value* hd = b_.CreateLoad(i32, sym(active_head_gv_));
        llvm::Value* tl = b_.CreateLoad(i32, sym(active_tail_gv_));
        b_.CreateCondBr(b_.CreateICmpNE(hd, tl), body, icheck);

        b_.SetInsertPoint(body);
        b_.CreateStore(b_.CreateAdd(hd, b_.getInt32(1)), sym(active_head_gv_));
        llvm::Value* idx  = b_.CreateAnd(hd, b_.getInt32(active_mask_));
        llvm::Value* slot = b_.CreateInBoundsGEP(queue_arr_ty_, sym(active_gv_), { b_.getInt32(0), idx });
        llvm::Value* hdl  = b_.CreateLoad(ptr_ty, slot);
        if (has_thunk) {
            // Low bit set => a tagged thunk pointer: call it directly (no frame,
            // nothing to destroy). Clear => a coroutine handle: resume/done/
            // destroy as before.
            auto* thunk_bb = llvm::BasicBlock::Create(ctx_, "active.thunk", fn);
            auto* coro_bb  = llvm::BasicBlock::Create(ctx_, "active.coro", fn);
            auto* addr     = b_.CreatePtrToInt(hdl, b_.getInt64Ty());
            b_.CreateCondBr(b_.CreateTrunc(addr, b_.getInt1Ty()), thunk_bb, coro_bb);

            b_.SetInsertPoint(thunk_bb);
            auto* fnp = b_.CreateIntToPtr(b_.CreateAnd(addr, ~static_cast<uint64_t>(1)), ptr_ty);
            b_.CreateCall(thunk_fnty, fnp, { state, wheel });
            b_.CreateBr(cont);

            b_.SetInsertPoint(coro_bb);
        }
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
            llvm::Value* n = b_.CreateLoad(i32, sym(inactive_top_gv_));
            b_.CreateCondBr(b_.CreateICmpNE(n, b_.getInt32(0)), ipromote, ncheck);

            // for (i = 0; i < n; ++i) active[i] = inactive[i];
            // head = 0, tail = n (ring empty here, n <= cap); inactive_top = 0;
            // then loop back to the drain.
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
            llvm::Value* src = b_.CreateInBoundsGEP(queue_arr_ty_, sym(inactive_gv_), { b_.getInt32(0), i });
            llvm::Value* dst = b_.CreateInBoundsGEP(queue_arr_ty_, sym(active_gv_), { b_.getInt32(0), i });
            b_.CreateStore(b_.CreateLoad(ptr_ty, src), dst);
            i->addIncoming(b_.CreateAdd(i, b_.getInt32(1)), pbody);
            b_.CreateBr(phead);

            b_.SetInsertPoint(pdone);
            b_.CreateStore(b_.getInt32(0), sym(active_head_gv_));
            b_.CreateStore(n, sym(active_tail_gv_));
            b_.CreateStore(b_.getInt32(0), sym(inactive_top_gv_));
            b_.CreateBr(check);
        }

        if (has_nba) {
            // Any staged update? OR the block chain head and the deferred array-write
            // queue's length; if so, commit them and loop back to drain the processes
            // the commits just woke.
            b_.SetInsertPoint(ncheck);
            llvm::Value* any = b_.getFalse();
            if (!array_nba_sigs_.empty() || !agg_nba_sigs_.empty()) {
                // rt_nba_arr_any is the total across the shared array + aggregate buffers.
                any = b_.CreateOr(any, b_.CreateICmpNE(b_.CreateCall(nba_arr_any_fn(), {}), b_.getInt32(0)));
            }
            if (!block_commits_.empty()) {
                any = b_.CreateOr(any, b_.CreateICmpNE(b_.CreateLoad(ptr_ty, sym(block_head_gv_)),
                                                       llvm::ConstantPointerNull::get(ptr_ty)));
            }
            if (!intra_nba_sigs_.empty()) {
                any = b_.CreateOr(any, b_.CreateICmpNE(b_.CreateCall(intra_any_fn(), {}), b_.getInt32(0)));
            }
            for (const Signal sig : nb_event_sigs_) {
                // A pending `->> e` this instant.
                llvm::Value* pend = b_.CreateLoad(b_.getInt8Ty(), sym(event_pending_gv_[static_cast<uint32_t>(sig)]));
                any = b_.CreateOr(any, b_.CreateICmpNE(pend, b_.getInt8(0)));
            }
            b_.CreateCondBr(any, ncommit, wcheck);

            b_.SetInsertPoint(ncommit);
            // Block-commit scalar NBAs: call the head block's commit function; it
            // musttail-chains through every pending block and returns here.
            if (!block_commits_.empty()) {
                auto*        cfn    = b_.GetInsertBlock()->getParent();
                auto*        run    = llvm::BasicBlock::Create(ctx_, "nblk.run", cfn);
                auto*        after  = llvm::BasicBlock::Create(ctx_, "nblk.after", cfn);
                llvm::Value* h      = b_.CreateLoad(ptr_ty, sym(block_head_gv_));
                b_.CreateCondBr(b_.CreateICmpNE(h, llvm::ConstantPointerNull::get(ptr_ty)), run, after);
                b_.SetInsertPoint(run);
                b_.CreateStore(llvm::ConstantPointerNull::get(ptr_ty), sym(block_head_gv_)); // empty chain for next instant
                auto* cty = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty }, false);
                b_.CreateCall(cty, h, { state });
                b_.CreateBr(after);
                b_.SetInsertPoint(after);
            }
            // Deferred writes through a runtime array index: replay each array's
            // typed records and each aggregate's memcpy records (both from the
            // shared rt_nba_arr_* buffers), waking Any-edge readers, then reset.
            if (!array_nba_sigs_.empty() || !agg_nba_sigs_.empty()) {
                for (const Signal sig : array_nba_sigs_) {
                    emit_array_commit(sig, state);
                }
                for (const Signal sig : agg_nba_sigs_) {
                    emit_agg_array_commit(sig, state);
                }
                // VCD: touch each dumped array that took an NBA write this instant,
                // so the flush scans its elements only on write instants (not every
                // instant). The runtime queue has no per-element index, so touch the
                // whole element range; the flush diff still emits only what changed.
                if (uses_vcd) {
                    for (const Signal sig : array_nba_sigs_) {
                        if (vcd_index_[static_cast<uint32_t>(sig)] < 0) {
                            continue;
                        }
                        auto* fn     = b_.GetInsertBlock()->getParent();
                        auto* svc    = llvm::BasicBlock::Create(ctx_, "vcd.arr.svc", fn);
                        auto* nextbb = llvm::BasicBlock::Create(ctx_, "vcd.arr.next", fn);
                        // Nonzero record count == the array took writes this instant
                        // (records are not reset until nba_arr_reset below).
                        auto* dirty  = b_.CreateCall(nba_arr_count_fn(), { b_.getInt32(arr_idx(sig)) });
                        b_.CreateCondBr(b_.CreateICmpNE(dirty, b_.getInt32(0)), svc, nextbb);
                        b_.SetInsertPoint(svc);
                        vcd_touch_array(sig);
                        b_.CreateBr(nextbb);
                        b_.SetInsertPoint(nextbb);
                    }
                }
                b_.CreateCall(nba_arr_reset_fn(), {});
            }
            // Deferred nonblocking intra-assignment stores that fired this instant:
            // each site commits its ready records (last fire wins), wakes readers,
            // and returns the records to its pool.
            if (!intra_nba_sigs_.empty()) {
                for (const Signal sig : intra_nba_sigs_) {
                    emit_intra_commit(sig);
                }
            }
            // Nonblocking event triggers (`->> e`, LRM 15.5.4): for each pending
            // event, stamp the trigger time and wake its Any-edge waiters, then
            // clear the flag. Done in the NBA region so the wake lands after this
            // instant's nonblocking updates; the loop-back to `check` then drains
            // the woken waiters.
            for (const Signal sig : nb_event_sigs_) {
                const auto   i    = static_cast<uint32_t>(sig);
                auto*        cfn  = b_.GetInsertBlock()->getParent();
                auto*        fire = llvm::BasicBlock::Create(ctx_, "nbev.fire", cfn);
                auto*        skip = llvm::BasicBlock::Create(ctx_, "nbev.skip", cfn);
                llvm::Value* pend = b_.CreateLoad(b_.getInt8Ty(), sym(event_pending_gv_[i]));
                b_.CreateCondBr(b_.CreateICmpNE(pend, b_.getInt8(0)), fire, skip);
                b_.SetInsertPoint(fire);
                b_.CreateStore(b_.getInt8(0), sym(event_pending_gv_[i]));
                emit_event_fire(sig, wheel); // stamp now + service Any static/dynamic queues
                b_.CreateBr(skip);
                b_.SetInsertPoint(skip);
            }
            b_.CreateBr(check);
        }

        // Queue empty: this instant has fully settled. Sample the VCD dump at the
        // current time (the postponed region, LRM §21.7) before the wheel advances,
        // so each variable's final settled value is recorded once per instant.
        b_.SetInsertPoint(wcheck);
        if (uses_vcd) {
            b_.CreateCall(vcd_flush_fn(), { b_.CreateCall(wheel_now_fn(), { wheel }) });
        }
        // Advance one instant (firing any timers, which enqueue their handles) and
        // re-drain. rt_wheel_advance folds the pending-check into the tick — one ABI
        // crossing per timestep, no separate occupancy scan — and returns non-zero
        // iff it fired something (an idle wheel ends the sim). The tick ctx is the
        // wheel: coroutine-timer trampolines ignore it, but a detached `->> #d e`
        // on-expiry thunk reads the current time from it (build_delay_event_thunks).
        llvm::Value* ticked = b_.CreateCall(wheel_advance_fn(), { wheel, wheel });
        b_.CreateCondBr(b_.CreateICmpNE(ticked, b_.getInt32(0)), check, wdone);

        b_.SetInsertPoint(wtick); // unreachable now; kept so block wiring stays valid
        b_.CreateBr(wdone);

        b_.SetInsertPoint(wdone);
        if (uses_vcd) {
            b_.CreateCall(vcd_close_fn(), {});
        }
    }

    // -- Coroutine intrinsics / frame allocator --

    // Declare (or fetch) an llvm.coro.* intrinsic. `tys` supplies the overload
    // type for the few overloaded ones (e.g. coro.size -> i64).
    llvm::Function* coro_intr(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> tys = {}) {
        return llvm::Intrinsic::getOrInsertDeclaration(&env_->llmod, id, tys);
    }

    // -- Runtime ABI (runtime/Runtime.h C ABI) --
    //
    // rt_fn declares (or fetches) one runtime entry point; each *_fn helper
    // below pins one C signature. Every pointer parameter is an opaque ptr, so
    // the LLVM-side signature is the C one with pointers erased; a helper
    // carries a comment only where the C signature is not obvious from it.

    llvm::FunctionCallee rt_fn(const char* name, llvm::Type* ret, llvm::ArrayRef<llvm::Type*> params) {
        return env_->llmod.getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
    }
    llvm::Type* raw_ptr_ty() {
        return llvm::PointerType::getUnqual(ctx_);
    }

    // Coroutine frame allocation, sim-owned: ptr rt_alloc(i64) / void rt_free(ptr).
    llvm::FunctionCallee alloc_fn() {
        return rt_fn("rt_alloc", raw_ptr_ty(), { b_.getInt64Ty() });
    }
    llvm::FunctionCallee free_fn() {
        return rt_fn("rt_free", b_.getVoidTy(), { raw_ptr_ty() });
    }

    // Deferred array-NBA record buffers (rt_nba_arr_*). The runtime owns the
    // growable per-array buffers; the backend defines and fills the fixed-stride
    // records and emits the monomorphized commit.
    llvm::FunctionCallee nba_arr_slot_fn() {
        return rt_fn("rt_nba_arr_slot", raw_ptr_ty(), { b_.getInt32Ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee nba_arr_base_fn() {
        return rt_fn("rt_nba_arr_base", raw_ptr_ty(), { b_.getInt32Ty() });
    }
    llvm::FunctionCallee nba_arr_count_fn() {
        return rt_fn("rt_nba_arr_count", b_.getInt32Ty(), { b_.getInt32Ty() });
    }
    llvm::FunctionCallee nba_arr_any_fn() {
        return rt_fn("rt_nba_arr_any", b_.getInt32Ty(), {});
    }
    llvm::FunctionCallee nba_arr_reset_fn() {
        return rt_fn("rt_nba_arr_reset", b_.getVoidTy(), {});
    }

    // Deferred nonblocking intra-assignment stores (rt_intra_*, LRM 9.4.5).
    llvm::FunctionCallee intra_alloc_fn() {
        return rt_fn("rt_intra_alloc", raw_ptr_ty(), { b_.getInt32Ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee intra_cb_fn() {
        return rt_fn("rt_intra_cb", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee intra_ready_base_fn() {
        return rt_fn("rt_intra_ready_base", raw_ptr_ty(), { b_.getInt32Ty() });
    }
    llvm::FunctionCallee intra_ready_count_fn() {
        return rt_fn("rt_intra_ready_count", b_.getInt32Ty(), { b_.getInt32Ty() });
    }
    llvm::FunctionCallee intra_any_fn() {
        return rt_fn("rt_intra_any", b_.getInt32Ty(), {});
    }
    llvm::FunctionCallee intra_release_fn() {
        return rt_fn("rt_intra_release", b_.getVoidTy(), { b_.getInt32Ty() });
    }
    llvm::FunctionCallee intra_reset_fn() {
        return rt_fn("rt_intra_reset", b_.getVoidTy(), {});
    }
    llvm::FunctionCallee intra_event_register_fn() {
        return rt_fn("rt_intra_event_register", b_.getVoidTy(), { b_.getInt32Ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee intra_event_fire_fn() {
        return rt_fn("rt_intra_event_fire", b_.getVoidTy(), { b_.getInt32Ty() });
    }

    // Per intra-NBA signal: dense id and the record StructType { ptr timer.next,
    // ptr timer.on_expire, i32 idx, i32 seq, i32 count, iMM mm_val, [iW z_val] }.
    // The rt_timer (fields 0-1) sits at offset 0 so the record base is the timer
    // pointer handed to the wheel; the runtime reads `idx` (field 2) to route the
    // fire and owns `seq` (field 3); `count` (field 4) is the `@e` event countdown
    // (unused by `#d`). Value fields: mm=5, z=6.
    uint32_t intra_idx(Signal sig) const {
        const auto it = std::find(intra_nba_sigs_.begin(), intra_nba_sigs_.end(), sig);
        return static_cast<uint32_t>(it - intra_nba_sigs_.begin());
    }
    llvm::StructType* intra_rec_ty(const SignalSlot& s) {
        auto*                    ptr = raw_ptr_ty();
        auto*                    i32 = b_.getInt32Ty();
        std::vector<llvm::Type*> f   = { ptr, ptr, i32, i32, i32,
                                         int_ty(s.narrow ? s.width : 2 * s.width) };
        if (s.has_z) {
            f.push_back(int_ty(s.width));
        }
        return llvm::StructType::get(ctx_, f);
    }

    // Per array-NBA signal: dense id (buffer index) and the record StructType
    // { ptr mm_ptr, iMM mm_val, [ptr z_ptr, iW z_val], [iW mask] }.
    uint32_t arr_idx(Signal sig) const {
        const auto it = std::find(array_nba_sigs_.begin(), array_nba_sigs_.end(), sig);
        return static_cast<uint32_t>(it - array_nba_sigs_.begin());
    }
    llvm::StructType* arr_rec_ty(const SignalSlot& s) {
        auto*                    ptr  = raw_ptr_ty();
        std::vector<llvm::Type*> f    = { ptr, int_ty(s.narrow ? s.width : 2 * s.width) };
        if (s.has_z) {
            f.push_back(ptr);
            f.push_back(int_ty(s.width));
        }
        if (s.arr_masked) {
            f.push_back(int_ty(s.width));
        }
        return llvm::StructType::get(ctx_, f);
    }
    // Field indices within the record (mm_ptr=0, mm_val=1 are fixed).
    uint32_t arr_rec_z_ptr(const SignalSlot&) const { return 2; }
    uint32_t arr_rec_z_val(const SignalSlot&) const { return 3; }
    uint32_t arr_rec_mask(const SignalSlot& s) const { return s.has_z ? 4 : 2; }

    // String runtime (rt_string_*); every ptr is an rt_string handle except
    // where noted.
    // -- Dynamic array runtime (rt_dynarr_*, LRM 7.5) --
    // void rt_dynarr_new(rt_dynarr*, i64 stride, i64 n, const void* proto)
    // void rt_dynarr_new(rt_dynarr*, const rt_elem_ops*, u64 n)
    llvm::FunctionCallee dynarr_new_fn() {
        return rt_fn("rt_dynarr_new", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
    }
    // void rt_dynarr_new_copy(rt_dynarr*, i64 stride, i64 n, const void* proto, const rt_dynarr* src)
    // void rt_dynarr_new_copy(rt_dynarr*, const rt_elem_ops*, u64 n, const rt_dynarr* src)
    llvm::FunctionCallee dynarr_new_copy_fn() {
        return rt_fn("rt_dynarr_new_copy", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
    }
    // void rt_dynarr_delete(rt_dynarr*)
    // void rt_dynarr_delete(rt_dynarr*, const rt_elem_ops*)
    llvm::FunctionCallee dynarr_delete_fn() {
        return rt_fn("rt_dynarr_delete", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    // void rt_dynarr_assign(rt_dynarr* dst, const rt_dynarr* src, i64 stride)
    // void rt_dynarr_assign(rt_dynarr*, const rt_dynarr*, const rt_elem_ops*)
    llvm::FunctionCallee dynarr_assign_fn() {
        return rt_fn("rt_dynarr_assign", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
    }
    // void rt_dynarr_reverse(rt_dynarr*, i64 stride)  (LRM 7.12.2)
    llvm::FunctionCallee dynarr_reverse_fn() {
        return rt_fn("rt_dynarr_reverse", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty() });
    }
    // void rt_dynarr_sort(rt_dynarr*, i64 stride, i32 kind, i32 width, i32 rev)
    llvm::FunctionCallee dynarr_sort_fn() {
        return rt_fn("rt_dynarr_sort", b_.getVoidTy(),
                     { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty() });
    }
    // void rt_dynarr_shuffle(rt_dynarr*, i64 stride, i64* rng)  (LRM 7.12.2)
    llvm::FunctionCallee dynarr_shuffle_fn() {
        return rt_fn("rt_dynarr_shuffle", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
    }

    // -- Queue runtime (rt_queue_*, LRM 7.10) --
    // void rt_queue_push_{back,front}(rt_queue*, i64 stride, i64 bound, const void* elem)
    // void rt_queue_push_{back,front}(rt_queue*, const rt_elem_ops*, u64 bound, const void* elem)
    llvm::FunctionCallee queue_push_fn(bool front) {
        return rt_fn(front ? "rt_queue_push_front" : "rt_queue_push_back", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
    }
    // void rt_queue_pop_{back,front}(rt_queue*, i64 stride, void* out)
    // void rt_queue_pop_{back,front}(rt_queue*, const rt_elem_ops*, void* out)
    llvm::FunctionCallee queue_pop_fn(bool front) {
        return rt_fn(front ? "rt_queue_pop_front" : "rt_queue_pop_back", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
    }
    // void rt_queue_insert(rt_queue*, i64 stride, i64 bound, i64 i, const void* elem)
    // void rt_queue_insert(rt_queue*, const rt_elem_ops*, u64 bound, u64 i, const void* elem)
    llvm::FunctionCallee queue_insert_fn() {
        return rt_fn("rt_queue_insert", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), b_.getInt64Ty(), raw_ptr_ty() });
    }
    // void rt_queue_delete_idx(rt_queue*, i64 stride, i64 i)
    // void rt_queue_delete_idx(rt_queue*, const rt_elem_ops*, u64 i)
    llvm::FunctionCallee queue_delete_idx_fn() {
        return rt_fn("rt_queue_delete_idx", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
    }
    // void rt_queue_delete_all(rt_queue*)
    // void rt_queue_delete_all(rt_queue*, const rt_elem_ops*)
    llvm::FunctionCallee queue_delete_all_fn() {
        return rt_fn("rt_queue_delete_all", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    // void rt_queue_assign(rt_queue* dst, const rt_queue* src, i64 stride)
    // void rt_queue_assign(rt_queue*, const rt_queue*, const rt_elem_ops*)
    llvm::FunctionCallee queue_assign_fn() {
        return rt_fn("rt_queue_assign", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
    }
    // void rt_queue_reverse(rt_queue*, i64 stride)  (LRM 7.12.2)
    llvm::FunctionCallee queue_reverse_fn() {
        return rt_fn("rt_queue_reverse", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty() });
    }
    // void rt_queue_sort(rt_queue*, i64 stride, i32 kind, i32 width, i32 rev)
    llvm::FunctionCallee queue_sort_fn() {
        return rt_fn("rt_queue_sort", b_.getVoidTy(),
                     { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty() });
    }
    // void rt_queue_shuffle(rt_queue*, i64 stride, i64* rng)  (LRM 7.12.2)
    llvm::FunctionCallee queue_shuffle_fn() {
        return rt_fn("rt_queue_shuffle", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
    }

    llvm::FunctionCallee str_init_fn() {
        return rt_fn("rt_string_init", b_.getVoidTy(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_deinit_fn() {
        return rt_fn("rt_string_deinit", b_.getVoidTy(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_assign_fn() {
        return rt_fn("rt_string_assign", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    // void rt_string_assign_bytes(rt_string*, const char* bytes, i64 len)
    llvm::FunctionCallee str_assign_bytes_fn() {
        return rt_fn("rt_string_assign_bytes", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
    }
    llvm::FunctionCallee str_len_fn() {
        return rt_fn("rt_string_len", b_.getInt64Ty(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_data_fn() {
        return rt_fn("rt_string_data", raw_ptr_ty(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_concat_fn() {
        return rt_fn("rt_string_concat", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_repl_fn() {
        return rt_fn("rt_string_repl", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
    }
    llvm::FunctionCallee str_cmp_fn() {
        return rt_fn("rt_string_cmp", b_.getInt32Ty(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_icmp_fn() {
        return rt_fn("rt_string_icmp", b_.getInt32Ty(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_substr_fn() {
        return rt_fn("rt_string_substr", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_upper_fn() {
        return rt_fn("rt_string_toupper", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    // void rt_enum_name(rt_string* dst, const u8* val, u32 vbytes, u32 count,
    //                   const u8* values, const char* const* names, const u32* lens)
    llvm::FunctionCallee enum_name_fn() {
        return rt_fn("rt_enum_name", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty(),
                       raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_lower_fn() {
        return rt_fn("rt_string_tolower", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    llvm::FunctionCallee str_putc_fn() {
        return rt_fn("rt_string_putc", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_getc_fn() {
        return rt_fn("rt_string_getc", b_.getInt32Ty(), { raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_from_packed_fn() {
        return rt_fn("rt_string_from_packed", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_to_packed_fn() {
        return rt_fn("rt_string_to_packed", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_atoi_fn() {
        return rt_fn("rt_string_atoi", b_.getInt32Ty(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_atohex_fn() {
        return rt_fn("rt_string_atohex", b_.getInt32Ty(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_atooct_fn() {
        return rt_fn("rt_string_atooct", b_.getInt32Ty(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_atobin_fn() {
        return rt_fn("rt_string_atobin", b_.getInt32Ty(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_atoreal_fn() {
        return rt_fn("rt_string_atoreal", b_.getDoubleTy(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee str_itoa_fn() {
        return rt_fn("rt_string_itoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_hextoa_fn() {
        return rt_fn("rt_string_hextoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_octtoa_fn() {
        return rt_fn("rt_string_octtoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_bintoa_fn() {
        return rt_fn("rt_string_bintoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee str_realtoa_fn() {
        return rt_fn("rt_string_realtoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getDoubleTy() });
    }

    // Timing wheel (rt_wheel_*); the ptr parameter is the rt_wheel.
    llvm::FunctionCallee wheel_init_fn() {
        return rt_fn("rt_wheel_init", b_.getVoidTy(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee wheel_deinit_fn() {
        return rt_fn("rt_wheel_deinit", b_.getVoidTy(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee wheel_now_fn() {
        return rt_fn("rt_wheel_now", b_.getInt64Ty(), { raw_ptr_ty() });
    }
    llvm::FunctionCallee random_fn() {
        return rt_fn("rt_random", b_.getInt32Ty(), {});
    }
    // rt_dist_<op>(i32 seed, i32 params...) -> i64 {seed:32, draw:32}. `nparam`
    // is the count of distribution parameters after the seed (1 or 2).
    llvm::FunctionCallee dist_fn(const char* name, unsigned nparam) {
        std::vector<llvm::Type*> params(nparam + 1, b_.getInt32Ty());
        return rt_fn(name, b_.getInt64Ty(), params);
    }
    // Reference-routine symbol for a distribution selector (LRM §20.15.2).
    static const char* dist_symbol(uint32_t fn) {
        switch (static_cast<DistOp>(fn)) {
            case DistOp::Uniform:
                return "rt_dist_uniform";
            case DistOp::Normal:
                return "rt_dist_normal";
            case DistOp::Exponential:
                return "rt_dist_exponential";
            case DistOp::Poisson:
                return "rt_dist_poisson";
            case DistOp::ChiSquare:
                return "rt_dist_chi_square";
            case DistOp::StudentT:
                return "rt_dist_t";
            case DistOp::Erlang:
                return "rt_dist_erlang";
        }
        throw BackendError{ "internal: unknown dist op" };
    }
    // void rt_urandom_seed(i64* state, i64 seed): splitmix64 expansion of a
    // 64-bit selector into the process's xoshiro256++ frame state.
    llvm::FunctionCallee urandom_seed_fn() {
        return rt_fn("rt_urandom_seed", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty() });
    }
    llvm::FunctionCallee wheel_pending_fn() {
        return rt_fn("rt_wheel_pending", b_.getInt32Ty(), { raw_ptr_ty() });
    }
    // void rt_wheel_schedule(rt_wheel*, rt_timer*, i64 deadline, rt_timer_fn)
    llvm::FunctionCallee wheel_schedule_fn() {
        return rt_fn("rt_wheel_schedule", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
    }
    // void rt_wheel_schedule_rel(rt_wheel*, rt_timer*, i64 delay, rt_timer_fn)
    llvm::FunctionCallee wheel_schedule_rel_fn() {
        return rt_fn("rt_wheel_schedule_rel", b_.getVoidTy(),
                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
    }
    // void rt_wheel_tick(rt_wheel*, void* ctx)
    llvm::FunctionCallee wheel_tick_fn() {
        return rt_fn("rt_wheel_tick", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
    }
    // int rt_wheel_advance(rt_wheel*, void* ctx)
    llvm::FunctionCallee wheel_advance_fn() {
        return rt_fn("rt_wheel_advance", b_.getInt32Ty(), { raw_ptr_ty(), raw_ptr_ty() });
    }

    // Power-up the state struct: undriven 4-state nets come up Z (LRM 6.6 —
    // must 0, may 1, z all-ones), other 4-state signals X (z 0), 2-state 0.
    // Runtime power-on init: only unpacked arrays (memories). Every scalar
    // signal's power-on value — X for a 4-state plane, an empty inline string
    // handle, a clear nba dirty mask — is a compile-time constant folded into
    // the state global's initializer (`state_initializer`), so no per-signal
    // GEP+store is emitted here. Arrays stay a runtime loop: materialising a
    // large memory's X-fill as a global constant would bloat the JIT object's
    // `.data` (a non-zero pattern cannot go in `.bss`), so it is filled in
    // O(1) emitted instructions instead.
    void init_state() {
        for (const SignalSlot& s : slots_) {
            if (s.is_agg) {
                std::vector<llvm::Value*> path;
                emit_agg_init(s.ir_type, storage_ptr(s), path, s.storage_ty);
                continue;
            }
            if (!s.is_array) {
                continue;
            }
            if (s.is_string) {
                emit_string_slot_op(s, str_init_fn());
            } else {
                emit_array_init(s);
            }
        }
    }

    // Recursive power-on fill for an aggregate signal: X into every 4-state leaf
    // and rt_string_init on every string leaf. 2-state / real leaves are already
    // zero from the state global's zeroinitializer. Array dimensions become
    // runtime loops (keeping init O(size) in emitted instructions), struct fields
    // straight-line GEPs. `root_ty` is the aggregate's storage type; `path`
    // accumulates GEP indices from its root.
    void emit_agg_init(Ref type, llvm::Value* base, std::vector<llvm::Value*>& path, llvm::Type* root_ty) {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            const uint32_t n     = array_dim(t);
            auto*          i32   = b_.getInt32Ty();
            auto*          fn    = b_.GetInsertBlock()->getParent();
            auto*          head  = llvm::BasicBlock::Create(ctx_, "ainit.head", fn);
            auto*          body  = llvm::BasicBlock::Create(ctx_, "ainit.body", fn);
            auto*          done  = llvm::BasicBlock::Create(ctx_, "ainit.done", fn);
            auto*          entry = b_.GetInsertBlock();
            b_.CreateBr(head);
            b_.SetInsertPoint(head);
            auto* idx = b_.CreatePHI(i32, 2);
            idx->addIncoming(b_.getInt32(0), entry);
            b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(n)), body, done);
            b_.SetInsertPoint(body);
            path.push_back(idx);
            emit_agg_init(t.element, base, path, root_ty);
            path.pop_back();
            idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
            b_.CreateBr(head);
            b_.SetInsertPoint(done);
            return;
        }
        if (t.kind == Type::Kind::Struct && !t.is_packed) {
            uint32_t f = 0;
            for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
                path.push_back(b_.getInt32(f++));
                emit_agg_init(fld.type, base, path, root_ty);
                path.pop_back();
            }
            return;
        }
        // Leaf: GEP through the path to the slot, then init only if it needs a
        // non-zero power-on (4-state -> X, string -> handle).
        std::vector<llvm::Value*> idx;
        idx.reserve(path.size() + 2);
        idx.push_back(b_.getInt32(0));
        for (llvm::Value* s : path) {
            idx.push_back(s);
        }
        if (t.kind == Type::Kind::String) {
            b_.CreateCall(str_init_fn(), { b_.CreateInBoundsGEP(root_ty, base, idx) });
            return;
        }
        if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue) {
            // Empty handle (null buffer, zero counts). A signal slot is already
            // zero from the state global's initializer; a frame alloca is not.
            b_.CreateStore(llvm::Constant::getNullValue(handle_slot_ty(type)),
                           b_.CreateInBoundsGEP(root_ty, base, idx));
            return;
        }
        const Layout lay  = mod_.pool.layout(type);
        const bool   four = lay.domain == Domain::Four;
        if (!four) {
            return; // 2-state / real: zeroinitializer is the power-on value.
        }
        const uint32_t w = lay.width == 0 ? 1 : lay.width;
        idx.push_back(b_.getInt32(0)); // mm field of the leaf slot
        b_.CreateStore(x_mm(w), b_.CreateInBoundsGEP(root_ty, base, idx));
    }

    // Recursive teardown for an aggregate's heap-owning leaves: rt_string_deinit
    // on every string leaf, rt_dynarr_delete / rt_queue_delete_all on every
    // handle leaf. The mirror of emit_agg_init; plane leaves need nothing. Call
    // sites gate on has_heap_leaf so a plane-only aggregate emits no walk.
    void emit_agg_deinit(Ref type, llvm::Value* base, std::vector<llvm::Value*>& path,
                         llvm::Type* root_ty) {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            if (!has_heap_leaf(t.element)) {
                return;
            }
            const uint32_t n     = array_dim(t);
            auto*          i32   = b_.getInt32Ty();
            auto*          fn    = b_.GetInsertBlock()->getParent();
            auto*          head  = llvm::BasicBlock::Create(ctx_, "adeinit.head", fn);
            auto*          body  = llvm::BasicBlock::Create(ctx_, "adeinit.body", fn);
            auto*          done  = llvm::BasicBlock::Create(ctx_, "adeinit.done", fn);
            auto*          entry = b_.GetInsertBlock();
            b_.CreateBr(head);
            b_.SetInsertPoint(head);
            auto* idx = b_.CreatePHI(i32, 2);
            idx->addIncoming(b_.getInt32(0), entry);
            b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(n)), body, done);
            b_.SetInsertPoint(body);
            path.push_back(idx);
            emit_agg_deinit(t.element, base, path, root_ty);
            path.pop_back();
            idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
            b_.CreateBr(head);
            b_.SetInsertPoint(done);
            return;
        }
        if (t.kind == Type::Kind::Struct && !t.is_packed) {
            uint32_t f = 0;
            for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
                path.push_back(b_.getInt32(f++));
                emit_agg_deinit(fld.type, base, path, root_ty);
                path.pop_back();
            }
            return;
        }
        const bool is_str    = t.kind == Type::Kind::String;
        const bool is_handle = t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue;
        if (!is_str && !is_handle) {
            return;
        }
        std::vector<llvm::Value*> idx;
        idx.reserve(path.size() + 1);
        idx.push_back(b_.getInt32(0));
        for (llvm::Value* s : path) {
            idx.push_back(s);
        }
        llvm::Value* slot = b_.CreateInBoundsGEP(root_ty, base, idx);
        if (is_str) {
            b_.CreateCall(str_deinit_fn(), { slot });
        } else {
            emit_handle_delete(slot, type);
        }
    }

    // Constant power-on initializer for one state global. Every state struct
    // is now flat — field k is signal k's storage (scope-relative path {k}) —
    // so the initializer is a direct per-field loop: each signal gets its
    // power-on constant (slot_const), array leaves zeroinitializer (filled at
    // runtime by `init_state`). The shadow global is all-zero (written before
    // read; masks power on clear).
    llvm::Constant* state_init_fields(llvm::StructType* ty,
                                      const std::vector<const SignalSlot*>& by_field) {
        std::vector<llvm::Constant*> elems;
        elems.reserve(ty->getNumElements());
        for (uint32_t f = 0; f < ty->getNumElements(); ++f) {
            elems.push_back(by_field[f] != nullptr
                              ? slot_const(*by_field[f])
                              : llvm::Constant::getNullValue(ty->getElementType(f)));
        }
        return llvm::ConstantStruct::get(ty, elems);
    }

    llvm::Constant* root_state_initializer() {
        std::vector<const SignalSlot*> by_field(state_ty_->getNumElements(), nullptr);
        for (const SignalSlot& s : slots_) {
            if (s.anchor == SignalSlot::no_anchor && !s.path.empty()) {
                by_field[s.path.front()] = &s;
            }
        }
        return state_init_fields(state_ty_, by_field);
    }

    llvm::Constant* scope_state_initializer(const Anchor& a) {
        const auto aid = static_cast<uint32_t>(&a - anchors_.data());
        std::vector<const SignalSlot*> by_field(a.ty->getNumElements(), nullptr);
        for (const SignalSlot& s : slots_) {
            if (s.anchor == aid) {
                by_field[s.path.front()] = &s;
            }
        }
        return state_init_fields(a.ty, by_field);
    }

    // Power-on constant for one signal's storage. Arrays and string arrays are
    // zeroinitialized (filled at runtime by `init_state`); a scalar string is
    // the empty inline handle (`rt_string_init`: byte 31 == str_inline_cap);
    // a scalar plane is X (4-state) or 0 (2-state) with the z word set for an
    // undriven-net may-z seed.
    llvm::Constant* slot_const(const SignalSlot& s) {
        if (s.is_array || s.is_agg) {
            // Zeroinitialized; the runtime fill (init_state -> emit_array_init /
            // emit_agg_init) writes X into 4-state leaves and inits string leaves.
            return llvm::ConstantAggregateZero::get(s.storage_ty);
        }
        if (s.is_dynarr) {
            // Empty array handle: { null data, 0 len } == zeroinitializer.
            return llvm::ConstantAggregateZero::get(s.storage_ty);
        }
        if (s.is_queue) {
            // Empty queue handle: { null base, 0 head, 0 len, 0 cap } == zeroinitializer.
            return llvm::ConstantAggregateZero::get(s.storage_ty);
        }
        if (s.is_string) {
            // Empty inline handle (mirrors rt_string_init): the length-marker byte
            // (index sizeof(rt_string)-1) holds rt_string_inline_cap for len 0, and
            // byte[0] is the NUL terminator (already 0 from the zero-fill).
            std::vector<uint8_t> bytes(sizeof(rt_string), 0);
            bytes[sizeof(rt_string) - 1] = static_cast<uint8_t>(rt_string_inline_cap);
            return llvm::ConstantStruct::get(
              s.slot_ty, { llvm::ConstantDataArray::get(ctx_, bytes) });
        }
        llvm::Constant* mm =
          s.narrow ? zero(s.width) : (s.four_state ? x_mm(s.width) : zero(2 * s.width));
        std::vector<llvm::Constant*> fields{ mm };
        if (s.has_z) {
            fields.push_back(s.init_z ? ones(s.width) : zero(s.width));
        }
        return llvm::ConstantStruct::get(s.slot_ty, fields);
    }

    // Apply `callee` (rt_string_init / rt_string_deinit) to a string signal's
    // handle, or to every element handle of an array-of-string signal (a runtime
    // loop, like emit_array_init).
    void emit_string_slot_op(const SignalSlot& s, llvm::FunctionCallee callee) {
        llvm::Value* base = storage_ptr(s);
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
    void emit_array_init(const SignalSlot& s) {
        const uint32_t w   = s.width;
        auto*          i32 = b_.getInt32Ty();
        llvm::Value*   mm  = s.narrow ? zero(w) : (s.four_state ? x_mm(w) : zero(2 * w));
        auto*        arr_ty = llvm::cast<llvm::ArrayType>(s.storage_ty);
        llvm::Value* base   = storage_ptr(s);

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
        // Bump the NBA segment epoch on entry and exit so a nested region's NBAs
        // never share a block-commit with the enclosing straight-line run (a block
        // must not span a conditional body). See block_register / cur_seg_epoch_.
        ++cur_seg_epoch_;
        for (Index i : region) {
            lower_inst(i);
            if (b_.GetInsertBlock()->getTerminator() != nullptr) {
                break;
            }
        }
        ++cur_seg_epoch_;
    }

    // Branch to `bb` unless the current block is already terminated (e.g. a region
    // that ended in a `ret`). Keeps merge edges from being appended after a `ret`.
    void branch_to(llvm::BasicBlock* bb) {
        if (b_.GetInsertBlock()->getTerminator() == nullptr) {
            b_.CreateBr(bb);
        }
    }

    // -- Type inference (memoized) --

    // Fast-body domain projection (compute_domain_spec): while assume_clean_ is
    // set, every 4-state value type reads as 2-state, so the whole body lowers
    // through the native 2-state operator paths. Sound because the fast body
    // only runs when its read set is proven clean at runtime, and eligibility
    // rejected every construct that could produce or consume x/z. Applied both
    // here (covers compute_type's hardcoded 4-state results — comparisons,
    // Clog2, Signal) and in from_ref (covers its direct conversion-target
    // callers). The types_ cache is reset per body pass (select_member), so
    // fast and generic passes never share entries.
    ValType domain_fake(ValType t) const {
        if (assume_clean_ && t.four_state && !t.is_string && !t.is_real) {
            t.four_state = false;
        }
        return t;
    }

    ValType type_of(Index i) {
        const auto slot = static_cast<uint32_t>(i);
        if (types_valid_[slot] != 0) {
            return types_[slot];
        }
        const ValType t    = domain_fake(compute_type(i));
        types_[slot]       = t;
        types_valid_[slot] = 1;
        return t;
    }

    ValType from_ref(Ref type) const {
        const Type& ty = std::get<Type>(mod_.pool.get(type));
        if (ty.kind == Type::Kind::String) {
            return { 0, false, false, true };
        }
        const Layout lay = mod_.pool.layout(type);
        const bool fl = mod_.pool.is_float(type);
        // Reals are signed quantities (LRM 11.8.1).
        return domain_fake({ lay.width == 0 ? 1 : lay.width, lay.domain == Domain::Four,
                             ty.is_signed || fl, false, fl });
    }

    // The element type Ref of the queue whose handle lvalue is `queue_lv`. The
    // lvalue is a plain Signal (queue-of-queue is rejected at the frontend), so
    // the queue type comes straight from the signal table.
    Ref queue_elem_type(Index queue_lv) {
        const Signal sig = ir_->payload(queue_lv).signal;
        return std::get<Type>(mod_.pool.get(mod_.signal(sig).type)).element;
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
                return { s.width, s.four_state, s.is_signed, s.is_string && !s.is_array,
                         s.is_real && !s.is_array };
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
            case Inst::Tag::MemberSelect: {
                // The result is the selected member's own type.
                const auto   ms     = ir_->extra_data<MemberSelect>(p.op_extra.extra);
                const Type&  st     = std::get<Type>(mod_.pool.get(ms.struct_type));
                const auto   fields = mod_.pool.struct_fields(st.fields_id);
                return from_ref(fields[ms.field_index].type);
            }
            case Inst::Tag::ClassNew:
                // The object handle (a 2-state 64-bit value, per layout(Class)).
                return from_ref(p.ref);
            case Inst::Tag::Load:
                return type_of(p.unary);
            case Inst::Tag::SimTime:
                return { 64, false, false };
            case Inst::Tag::EventTriggered:
                // `e.triggered` (LRM 15.5.3): a definite 2-state 1-bit `bit`.
                return { 1, false, false };
            case Inst::Tag::Random:
                return { 32, false, true };
            case Inst::Tag::Clog2:
                // $clog2 returns a 32-bit signed `integer` (4-state: the result
                // is X when the operand carries x/z).
                return { 32, true, true };
            case Inst::Tag::Ctz:
            case Inst::Tag::Clz:
                // A definite 2-state 32-bit bit index (counts of the must plane).
                return { 32, false, false };
            case Inst::Tag::CountBits:
                // $countbits family: a definite 2-state 32-bit signed `int`
                // (LRM 20.9 return type). Never X — it counts, it does not
                // propagate.
                return { 32, false, true };
            case Inst::Tag::OneHot:
            case Inst::Tag::OneHot0:
            case Inst::Tag::IsUnknown:
                // Definite 1-bit qualifier results (LRM 20.9); never X.
                return { 1, false, false };
            case Inst::Tag::FopenFd:
            case Inst::Tag::FopenMcd:
                // $fopen returns a 32-bit descriptor integer.
                return { 32, false, true };
            case Inst::Tag::Fgetc:
            case Inst::Tag::Ungetc:
            case Inst::Tag::Feof:
            case Inst::Tag::Ftell:
            case Inst::Tag::Fseek:
            case Inst::Tag::Rewind:
            case Inst::Tag::Fgets:
            case Inst::Tag::Ferror:
            case Inst::Tag::Fscanf:
            case Inst::Tag::Sscanf:
            case Inst::Tag::FreadWord:
            case Inst::Tag::FreadMem:
                // The file read / positioning / error / scan functions return i32.
                return { 32, false, true };
            case Inst::Tag::TestPlusArgs:
            case Inst::Tag::ValuePlusArgs:
                // LRM §21.6: an always-known (never x/z) boolean 'int'.
                return { 32, false, true };
            case Inst::Tag::DynArrayNew:
            case Inst::Tag::DynArrayNewInit:
            case Inst::Tag::DynArraySize:
            case Inst::Tag::DynArrayDelete:
            case Inst::Tag::DynArrayCopy:
                // Only size() is a value (a definite 2-state int); new / delete /
                // copy are statements whose type is never queried.
                return { 32, false, true };
            case Inst::Tag::QueuePopBack:
            case Inst::Tag::QueuePopFront:
                // One element of the queue's element type, straight off the
                // payload's Queue type.
                return from_ref(std::get<Type>(mod_.pool.get(p.op_type.type)).element);
            case Inst::Tag::QueueSize:
            case Inst::Tag::QueuePushBack:
            case Inst::Tag::QueuePushFront:
            case Inst::Tag::QueueInsert:
            case Inst::Tag::QueueDeleteIdx:
            case Inst::Tag::QueueDeleteAll:
            case Inst::Tag::QueueCopy:
                // size() is a definite 2-state int; the rest are statements
                // whose type is never queried.
                return { 32, false, true };
            case Inst::Tag::DynArrayReverse:
            case Inst::Tag::DynArraySort:
            case Inst::Tag::DynArrayRsort:
            case Inst::Tag::DynArrayShuffle:
            case Inst::Tag::QueueReverse:
            case Inst::Tag::QueueSort:
            case Inst::Tag::QueueRsort:
            case Inst::Tag::QueueShuffle:
                // Ordering methods (LRM 7.12.2) are void statements; their type
                // is never queried.
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
            case Inst::Tag::StreamReverse:
                // A bit permutation: the operand's width and domain.
                return type_of(p.op_imm.op);
            case Inst::Tag::Select:
                return type_of(ir_->extra_data<Select>(p.op_extra.extra).a);
            case Inst::Tag::SelectLazy:
                return type_of(ir_->extra_data<SelectLazy>(p.op_extra.extra).then_val);

            case Inst::Tag::FAdd:
            case Inst::Tag::FSub:
            case Inst::Tag::FMul:
            case Inst::Tag::FDiv:
            case Inst::Tag::FPow:
                return type_of(p.binary.l);
            case Inst::Tag::FNeg:
                return type_of(p.unary);
            // Real compares are definite (reals carry no x/z): 1-bit two-state.
            case Inst::Tag::FEq:
            case Inst::Tag::FNe:
            case Inst::Tag::FLt:
            case Inst::Tag::FLe:
            case Inst::Tag::FGt:
            case Inst::Tag::FGe:
                return { 1, false, false };
            case Inst::Tag::ItoF:
            case Inst::Tag::FtoI:
            case Inst::Tag::FtoITrunc:
            case Inst::Tag::FBitcast:
            case Inst::Tag::FResize:
                return from_ref(p.op_type.type);
            // Every LRM 20.8.2 routine returns a real.
            case Inst::Tag::RealMathUnary:
            case Inst::Tag::RealMathBinary:
                return { 64, false, true, false, true };

            // $dist_* / seeded $random: a packed i64 {seed:32, draw:32}, sliced
            // by the frontend. A definite 2-state word (never x/z).
            case Inst::Tag::DistUnary:
            case Inst::Tag::DistBinary:
                return { 64, false, false };
            // $urandom / $urandom_range: a 32-bit unsigned `int` (2-state).
            case Inst::Tag::Urandom:
            case Inst::Tag::UrandomSeeded:
            case Inst::Tag::UrandomRange:
                return { 32, false, false };

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
            case Inst::Tag::AggEq:
            case Inst::Tag::InsideSet:
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
                // getc / s[i] return `byte`, which is signed (LRM 6.16.3 and
                // Table 6-8), so 0xff displays as -1 under %d.
                return { 8, false, true };
            case Inst::Tag::StrUpper:
            case Inst::Tag::StrLower:
            case Inst::Tag::StrSubstr:
            case Inst::Tag::StrFromPacked:
            case Inst::Tag::Sformatf:
            case Inst::Tag::EnumName:
                return { 0, false, false, true };
            case Inst::Tag::StrToPacked:
                return from_ref(p.op_type.type);
            case Inst::Tag::StrPutc:
                return { 0, false, false };
            case Inst::Tag::StrAtoi:
            case Inst::Tag::StrAtohex:
            case Inst::Tag::StrAtooct:
            case Inst::Tag::StrAtobin:
                // LRM 6.16.9: declared return type `integer` (4-state), though
                // the conversion itself is always definite — no x/z is
                // reachable from a string's bytes.
                return { 32, true, true };
            case Inst::Tag::StrAtoreal:
                return { 64, false, true, false, true };
            case Inst::Tag::StrItoa:
            case Inst::Tag::StrHextoa:
            case Inst::Tag::StrOctoa:
            case Inst::Tag::StrBintoa:
            case Inst::Tag::StrRealtoa:
                return { 0, false, false };

            case Inst::Tag::Store:
            case Inst::Tag::StoreNb:
            case Inst::Tag::StoreNbDelay:
            case Inst::Tag::StoreNbEvent:
            case Inst::Tag::AggCopy:
            case Inst::Tag::Cond:
            case Inst::Tag::Switch:
            case Inst::Tag::Loop:
            case Inst::Tag::Break:
            case Inst::Tag::Continue:
            case Inst::Tag::HandleDeref:
                // An lvalue producer (rooted at the object body); no value type.
            case Inst::Tag::Ret:
            case Inst::Tag::RetVoid:
            case Inst::Tag::Delay:
            case Inst::Tag::Wait:
            case Inst::Tag::Fork:
            case Inst::Tag::Display:
            case Inst::Tag::FDisplay:
            case Inst::Tag::TimeFormat:
            case Inst::Tag::Finish:
            case Inst::Tag::CaseViol:
            case Inst::Tag::Severity:
            case Inst::Tag::Fflush:
            case Inst::Tag::Fclose:
            case Inst::Tag::Sformat:
            case Inst::Tag::ReadMem:
            case Inst::Tag::TaskCall:
            case Inst::Tag::DumpFile:
            case Inst::Tag::DumpVars:
            case Inst::Tag::DumpOff:
            case Inst::Tag::DumpOn:
            case Inst::Tag::DumpAll:
            case Inst::Tag::DumpFlush:
            case Inst::Tag::EventTrigger:
            case Inst::Tag::EventTriggerNb:
            case Inst::Tag::EventTriggerNbDelayed:
                return { 0, false, false };
        }
        return { 0, false, false };
    }

    // -- Value / lvalue access --

    Plane value_of(Index i) const {
        return values_[static_cast<uint32_t>(i)];
    }
    void set_value(Index i, Plane p) {
        // Keep a value's encoding in lockstep with its IR type's domain. slang
        // unifies operand *types* (inserting conversions), not our Plane flags, so
        // the "operands share a domain" invariant the operator dispatch relies on
        // holds only if every value's `two` flag matches type_of(i). Producers that
        // always emit the packed form (Arg, Call, SimTime, string ops, ...) are
        // projected to a single iW here when their type is 2-state; the projection
        // is lossless because such a value has must == may and no z.
        const ValType t = type_of(i);
        // A real value stays fp-only; pack/unpack happens at the storage / ABI
        // boundaries (see the real helpers).
        if (!t.is_string && !t.is_real) {
            if (t.four_state && p.two) {
                p = to_four(p);
            } else if (!t.four_state && !p.two) {
                p = two_val(must_of(p));
            }
        }
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
        llvm::Value* ptr = nullptr;
        if (lv.array_ty != nullptr) {
            llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
            ptr = b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx });
        } else if (lv.is_agg) {
            // A string leaf inside an aggregate (struct member / class property)
            // is reached through the aggregate GEP path, not the bare base.
            ptr = agg_slot_ptr(lv);
        } else {
            ptr = lv.base;
        }
        if (lv.dyn_inrange != nullptr) {
            // Out-of-range dynamic-array / queue string element: redirect to the
            // writable sink handle, reset to owned-empty right here — an OOB
            // read yields "" (Table 7-1 string default) and an OOB write /
            // mutating method lands in the sink and is discarded (LRM 7.5.1).
            llvm::Value* sink = str_sink_global();
            b_.CreateCall(str_deinit_fn(), { sink });
            b_.CreateCall(str_init_fn(), { sink });
            ptr = b_.CreateSelect(lv.dyn_inrange, ptr, sink);
        }
        return ptr;
    }

    // The shared out-of-range string-element sink: a writable rt_string handle
    // that powers on owned-empty and is reset before every use (lv_string_ptr).
    llvm::Value* str_sink_global() {
        if (env_->str_sink == nullptr) {
            std::vector<uint8_t> bytes(sizeof(rt_string), 0);
            bytes[sizeof(rt_string) - 1] = static_cast<uint8_t>(rt_string_inline_cap);
            auto* init = llvm::ConstantStruct::get(
              str_slot_ty(), { llvm::ConstantDataArray::get(ctx_, bytes) });
            env_->str_sink = new llvm::GlobalVariable(env_->llmod, str_slot_ty(), /*isConstant=*/false,
                                                 llvm::GlobalValue::InternalLinkage, init,
                                                 "clotho.str.sink");
        }
        return env_->str_sink;
    }

    // Whether an instruction produces a fresh string value that needs its own
    // 32-byte handle (as opposed to yielding a pointer at existing storage).
    bool produces_string_temp(Index i) {
        switch (ir_->tag(i)) {
            case Inst::Tag::StrUpper:
            case Inst::Tag::StrLower:
            case Inst::Tag::StrSubstr:
            case Inst::Tag::StrFromPacked:
            case Inst::Tag::Sformatf:
            case Inst::Tag::EnumName:
                return true;
            case Inst::Tag::Constant:
            case Inst::Tag::Concat:
            case Inst::Tag::Repl:
            case Inst::Tag::Call:
            // A pop of a string-element queue relocates the element into the
            // instruction's own temp handle (lower_queue_pop).
            case Inst::Tag::QueuePopBack:
            case Inst::Tag::QueuePopFront:
                return type_of(i).is_string;
            default:
                return false;
        }
    }

    // A string Store whose RHS concat builds directly into the store target,
    // making the store itself a no-op. Matches the exact frontend shape of
    // `s = {...}` — consecutive [Concat, lvalue leaf, Store] — so nothing reads
    // the target between the concat write and the store, and the concat has no
    // earlier consumer. rt_string_concat is alias-safe for a target appearing as
    // a prefix part (s = {s, chunk} appends in place, the O(n^2)->O(n) fix for
    // repeated append); a whole-string leaf target keeps the handle derivable in
    // the prologue, before the leaf instruction itself lowers.
    bool forwarded_string_store(Index j) {
        if (ir_->tag(j) != Inst::Tag::Store) {
            return false;
        }
        const Inst::Payload& p  = ir_->payload(j);
        const auto           ji = static_cast<uint32_t>(j);
        if (static_cast<uint32_t>(p.binary.r) + 2 != ji || static_cast<uint32_t>(p.binary.l) + 1 != ji) {
            return false;
        }
        if (ir_->tag(p.binary.r) != Inst::Tag::Concat || !type_of(p.binary.r).is_string) {
            return false;
        }
        switch (ir_->tag(p.binary.l)) {
            case Inst::Tag::Signal: {
                const SignalSlot& s = slot_of(ir_->payload(p.binary.l).signal);
                return s.is_string && !s.is_array;
            }
            case Inst::Tag::Local: {
                const uint32_t idx = ir_->payload(p.binary.l).local.index;
                return idx < frame_slots_.size() && frame_slots_[idx].is_string;
            }
            default:
                return false;
        }
    }

    // Alloca + init one handle per string-producing instruction, in the body
    // prologue (entry block of a function; right after coro.begin of a process /
    // task). Producers overwrite their temp in place, so a loop reuses its
    // capacity instead of allocating each iteration. Paired with
    // deinit_body_strings at the body's exits. A concat consumed by an adjacent
    // forwarded store gets the store target's handle instead of a temp (never
    // init'd — the target already holds a live string).
    void build_str_temps() {
        str_temp_list_.clear();
        for (size_t k = 0; k < ir_->inst_count(); ++k) {
            const auto i = static_cast<Index>(k);
            if (!produces_string_temp(i)) {
                continue;
            }
            if (ir_->tag(i) == Inst::Tag::Concat && k + 2 < ir_->inst_count() &&
                forwarded_string_store(static_cast<Index>(k + 2))) {
                const Inst::Payload& lp = ir_->payload(static_cast<Index>(k + 1));
                const LValue lv = ir_->tag(static_cast<Index>(k + 1)) == Inst::Tag::Signal
                                    ? signal_lvalue(lp.signal)
                                    : local_lvalue(lp.local.index);
                set_str(i, lv_string_ptr(lv));
                continue;
            }
            auto* temp = b_.CreateAlloca(str_slot_ty(), nullptr, "str.tmp");
            temp->setAlignment(llvm::Align(alignof(rt_string)));
            b_.CreateCall(str_init_fn(), { temp });
            set_str(i, temp);
            str_temp_list_.push_back(temp);
        }
    }

    // Alloca an empty result handle for each dynamic-array-returning call in the
    // current body (the callee copies its return value into it). The handle is the
    // call's lvalue, so a `z = f()` assignment / `f().size()` / `foreach(f())`
    // reads it; it is freed at every body exit (deinit_body_strings).
    void build_dynarr_temps() {
        dynarr_temp_list_.clear();
        dynarr_ret_.assign(ir_->inst_count(), nullptr);
        for (size_t k = 0; k < ir_->inst_count(); ++k) {
            const auto i = static_cast<Index>(k);
            if (ir_->tag(i) != Inst::Tag::Call) {
                continue;
            }
            const auto ci  = ir_->extra_data<CallInfo>(ir_->payload(i).extra);
            const Ref  rty = mod_.function(ci.func).return_type;
            if (!is_handle_type(rty)) {
                continue;
            }
            auto* slot = handle_slot_ty(rty);
            auto* temp = b_.CreateAlloca(slot, nullptr, "handle.ret");
            temp->setAlignment(llvm::Align(8));
            b_.CreateStore(llvm::Constant::getNullValue(slot), temp); // empty handle
            dynarr_ret_[k] = temp;
            dynarr_temp_list_.push_back({ temp, rty });
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
                // A bit-select is bounds-checked against its container (LRM
                // 11.5.1): an out-of-range or x/z index reads x / writes nothing.
                const Plane        idxp   = value_of(p.binary.r);
                llvm::Value* const cbase  = lv.bit_off != nullptr ? lv.bit_off : b_.getInt32(0);
                const uint32_t     cwidth = lv.width;
                llvm::Value*       idx    = cast(must_of(idxp), 32, false);
                lv.bit_off = lv.bit_off != nullptr ? b_.CreateAdd(lv.bit_off, idx) : idx;
                lv.width   = 1;
                set_select_bounds(lv, cbase, cwidth, 1, any_unknown(idxp));
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
                // Bounds-checked against its container (LRM 11.5.1): bits of the
                // selection that fall outside read x / are not written; a wholly
                // out-of-range or x/z offset makes the whole select out of range.
                LValue             lv     = lvalue_of(p.op_extra.op);
                const auto         dps    = ir_->extra_data<DynPartSelect>(p.op_extra.extra);
                const Plane        offp   = value_of(dps.offset);
                llvm::Value* const cbase  = lv.bit_off != nullptr ? lv.bit_off : b_.getInt32(0);
                const uint32_t     cwidth = lv.width;
                llvm::Value*       off    = cast(must_of(offp), 32, false);
                lv.bit_off = lv.bit_off != nullptr ? b_.CreateAdd(lv.bit_off, off) : off;
                lv.width   = dps.width;
                set_select_bounds(lv, cbase, cwidth, dps.width, any_unknown(offp));
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
                if (at.kind == Type::Kind::DynArray) {
                    // Dynamic array (LRM 7.5): the base lvalue addresses the
                    // { data, len } handle. Load the heap data pointer and length,
                    // and build a fresh element lvalue over `[0 x elemslot]` at the
                    // 0-based index, carrying the in-range predicate so the shared
                    // load/store applies bounds (out-of-range read -> default,
                    // write -> no-op; the heap is never touched past `len`).
                    const DynElem e       = dynarr_elem(es.array_type);
                    llvm::Value*  handle  = lv_handle_ptr(lv);
                    llvm::Value*  dataptr = b_.CreateLoad(
                      raw_ptr_ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 0));
                    llvm::Value* len = b_.CreateLoad(
                      b_.getInt64Ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 1));
                    llvm::Value* idx64 = cast(must_of(value_of(es.index)), 64, false);
                    if (e.is_string) {
                        // String element: an rt_string handle slot in the heap
                        // buffer. Bounds ride lv_string_ptr's sink redirect.
                        LValue sel;
                        sel.base        = dataptr;
                        sel.is_string   = true;
                        sel.slot_ty     = e.slot_ty;
                        sel.array_ty    = llvm::ArrayType::get(e.slot_ty, 0);
                        sel.elem_idx    = cast(idx64, 32, false);
                        sel.is_signal   = lv.is_signal;
                        sel.sig         = lv.sig;
                        sel.dyn_inrange = b_.CreateICmpULT(idx64, len);
                        set_lvalue(i, sel);
                        return;
                    }
                    LValue       el;
                    el.base        = dataptr;
                    el.slot_ty     = e.slot_ty;
                    el.has_z       = e.has_z;
                    el.narrow      = e.narrow;
                    el.array_ty    = llvm::ArrayType::get(e.slot_ty, 0);
                    el.elem_idx    = cast(idx64, 32, false);
                    el.store_width = e.width;
                    el.bit_off     = b_.getInt32(0);
                    el.width       = e.width;
                    el.is_signal   = lv.is_signal;
                    el.sig         = lv.sig;
                    el.dyn_inrange = b_.CreateICmpULT(idx64, len);
                    set_lvalue(i, el);
                    return;
                }
                if (at.kind == Type::Kind::Queue) {
                    // Queue (LRM 7.10): the base lvalue addresses the
                    // { base, head, len, cap } ring handle. Element i lives at the
                    // physical slot (head + i) mod cap; build a fresh element
                    // lvalue over `[0 x elemslot]` at that physical index with the
                    // in-range predicate i < len (an empty queue has len == 0, so
                    // the modulo divisor is clamped to 1 to keep it defined on the
                    // never-taken out-of-range path).
                    const DynElem e       = dynarr_elem(es.array_type);
                    llvm::Value*  handle  = lv_handle_ptr(lv);
                    llvm::Value*  baseptr = b_.CreateLoad(
                      raw_ptr_ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 0));
                    llvm::Value* head = b_.CreateLoad(
                      b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 1));
                    llvm::Value* len = b_.CreateLoad(
                      b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 2));
                    llvm::Value* cap = b_.CreateLoad(
                      b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 3));
                    llvm::Value* idx64 = cast(must_of(value_of(es.index)), 64, false);
                    llvm::Value* safe_cap =
                      b_.CreateSelect(b_.CreateICmpEQ(cap, b_.getInt64(0)), b_.getInt64(1), cap);
                    llvm::Value* phys = b_.CreateURem(b_.CreateAdd(head, idx64), safe_cap);
                    if (e.is_string) {
                        LValue sel;
                        sel.base        = baseptr;
                        sel.is_string   = true;
                        sel.slot_ty     = e.slot_ty;
                        sel.array_ty    = llvm::ArrayType::get(e.slot_ty, 0);
                        sel.elem_idx    = cast(phys, 32, false);
                        sel.is_signal   = lv.is_signal;
                        sel.sig         = lv.sig;
                        sel.dyn_inrange = b_.CreateICmpULT(idx64, len);
                        set_lvalue(i, sel);
                        return;
                    }
                    LValue       el;
                    el.base        = baseptr;
                    el.slot_ty     = e.slot_ty;
                    el.has_z       = e.has_z;
                    el.narrow      = e.narrow;
                    el.array_ty    = llvm::ArrayType::get(e.slot_ty, 0);
                    el.elem_idx    = cast(phys, 32, false);
                    el.store_width = e.width;
                    el.bit_off     = b_.getInt32(0);
                    el.width       = e.width;
                    el.is_signal   = lv.is_signal;
                    el.sig         = lv.sig;
                    el.dyn_inrange = b_.CreateICmpULT(idx64, len);
                    set_lvalue(i, el);
                    return;
                }
                // A variable index into a fixed-size unpacked array has no static
                // guarantee of staying in range (LRM 7.4.5 / 11.5.1), and an X/Z
                // index is itself out of range (Table 7-1 draws no distinction
                // between an unknown index and one that misses the declared
                // bound -- both read the element default / no-op the write).
                // Normalize to a 0-based position and gate the element with the
                // same dyn_inrange machinery a dynamic-array/queue element uses:
                // out-of-range read yields the element default, write is a
                // no-op, the storage itself is never addressed out-of-bounds. An
                // unsigned compare also catches a normalized index that went
                // negative (wrapped to a huge unsigned value).
                const Plane  idxp = value_of(es.index);
                llvm::Value* idx  = cast(must_of(idxp), 32, false);
                // Normalize the declared-range coordinate to a 0-based position.
                llvm::Value* norm = at.left <= at.right ? b_.CreateSub(idx, b_.getInt32(at.left))
                                                        : b_.CreateSub(b_.getInt32(at.left), idx);
                const uint32_t count = (at.left <= at.right ? at.right - at.left : at.left - at.right) + 1;
                llvm::Value*   inrange =
                    b_.CreateAnd(b_.CreateICmpULT(norm, b_.getInt32(count)), b_.CreateNot(any_unknown(idxp)));
                lv.dyn_inrange = lv.dyn_inrange != nullptr ? b_.CreateAnd(lv.dyn_inrange, inrange)
                                                           : inrange;
                if (lv.is_agg) {
                    // Recursive storage is one ArrayType per dimension (not
                    // flattened): push a nested-GEP index step. A runtime element
                    // index makes the path non-static (deferred nonblocking path).
                    // The GEP step itself still uses the raw (unclamped) norm --
                    // an out-of-range dimension is caught by dyn_inrange above,
                    // which gates the eventual leaf load/store, not the GEP path.
                    lv.agg_path.push_back(norm);
                    lv.agg_static = false;
                    lv.agg_leaf   = is_leaf_type(at.element);
                    if (lv.agg_leaf) {
                        fill_agg_leaf(lv, at.element);
                    }
                    set_lvalue(i, lv);
                    return;
                }
                // Flat memory: advance the single flattened leaf index by
                // normalized_index * inner-dimension leaf count.
                const uint32_t stride = array_leaf_count(at.element);
                llvm::Value*   off    = stride == 1 ? norm : b_.CreateMul(norm, b_.getInt32(stride));
                lv.elem_idx           = lv.elem_idx != nullptr ? b_.CreateAdd(lv.elem_idx, off) : off;
                lv.bit_off            = b_.getInt32(0);
                set_lvalue(i, lv);
                return;
            }
            case Inst::Tag::MemberSelect: {
                const auto ms = ir_->extra_data<MemberSelect>(p.op_extra.extra);
                set_lvalue(i, agg_member(lvalue_of(p.op_extra.op), ms.struct_type, ms.field_index));
                return;
            }
            case Inst::Tag::ClassNew: {
                // Allocate the object body, zero it (2-state / handle leaves power
                // on 0 / null), X-init 4-state leaves and init string leaves, and
                // yield the handle as an i64 (reference semantics: the pointer is
                // the value). Lifetime is leaked in V1 (LRM 8.4 objects are GC'd;
                // a program cannot observe a free) — see docs/design/classes.md §7.
                const Ref      class_ref = p.ref;
                llvm::Type*    body_ty   = class_body_ty(class_ref);
                const uint64_t size      = env_->llmod.getDataLayout().getTypeAllocSize(body_ty);
                llvm::Value*   body      = b_.CreateCall(alloc_fn(), { b_.getInt64(size) });
                b_.CreateMemSet(body, b_.getInt8(0), b_.getInt64(size), llvm::MaybeAlign(64));
                emit_class_body_init(class_ref, body);
                set_value(i, two_val(b_.CreatePtrToInt(body, b_.getInt64Ty())));
                return;
            }
            case Inst::Tag::HandleDeref: {
                // Turn a class handle value (i64 pointer bits) into an aggregate
                // lvalue rooted at the object body. No copy — the handle IS the
                // object (reference semantics), unlike a value struct addressed
                // inline. A following MemberSelect addresses a property.
                llvm::Value* handle = must_of(value_of(p.op_type.op));
                LValue       lv{};
                lv.base      = b_.CreateIntToPtr(handle, raw_ptr_ty());
                lv.root_ty   = class_body_ty(p.op_type.type);
                lv.is_agg    = true;
                lv.is_signal = false;
                set_lvalue(i, lv);
                return;
            }

            case Inst::Tag::Load: {
                const LValue& lv = lvalue_of(p.unary);
                if (lv.is_handle) {
                    // Defense in depth: a queue / dynamic-array handle has no
                    // plane value; every legal consumer goes through the
                    // dedicated ops (frontend value_cat dispatch).
                    throw BackendError{ "plane load of a queue / dynamic-array handle" };
                }
                if (lv.is_string) {
                    // A string load is the storage handle pointer, not a copy:
                    // every mutation point (Store, string ops) deep-copies, and
                    // producers write into distinct temps.
                    set_str(i, lv_string_ptr(lv));
                    return;
                }
                const ValType t = type_of(i);
                Plane         v = lv.dyn_inrange != nullptr ? dyn_elem_load(lv) : lower_load(lv);
                if (t.is_real) {
                    v = real_plane(planes_to_real(v, t.width));
                }
                set_value(i, v);
                return;
            }
            case Inst::Tag::Store: {
                // A blocking assignment stores, then services the target signal's
                // wait queue: any process waiting for an edge this store produced
                // is woken. Snapshot the old value first so the transition can be
                // detected from old -> new.
                const LValue& lv      = lvalue_of(p.binary.l);
                if (lv.is_handle) {
                    throw BackendError{ "plane store to a queue / dynamic-array handle" };
                }
                if (lv.is_string) {
                    // A forwarded concat already built its result in the target
                    // (build_str_temps handed it the target's handle).
                    if (forwarded_string_store(i)) {
                        return;
                    }
                    // Deep copy. Strings are guarded out of sensitivity lists in
                    // the frontend, so there is no wait queue to service.
                    b_.CreateCall(str_assign_fn(), { lv_string_ptr(lv), str_of(p.binary.r) });
                    return;
                }
                if (lv.dyn_inrange != nullptr) {
                    // Out-of-range write is a no-op (LRM 7.5.1): guard the store
                    // (and its wait servicing) behind the in-range predicate.
                    dyn_elem_store(lv, value_of(p.binary.r));
                    return;
                }
                // The snapshot covers the selected storage (whole signal, or one
                // array element) so an element write wakes Any-edge waiters on a
                // change to that element rather than always reading element 0.
                const ServiceSnapshot old = service_snapshot(lv);
                lower_store(lv, value_of(p.binary.r));
                service_after_write(lv, old);
                return;
            }
            // A nonblocking assignment samples its RHS now but defers the write: it
            // stages the value into the target's shadow and enqueues the target for
            // the NBA update region (see emit_nba_region), which commits shadow ->
            // live and services wait queues only after every active process has run.
            case Inst::Tag::StoreNb: {
                const LValue& lv  = lvalue_of(p.binary.l);
                Plane         rhs = value_of(p.binary.r);
                if (type_of(p.binary.r).is_real) {
                    rhs = real_to_planes(fp_of(rhs));
                }
                // Deferral splits three ways: an unpacked-array element defers a
                // typed record; an aggregate leaf reached through a runtime array
                // index defers a memcpy record; everything else (a plain signal,
                // scalar select, or aggregate static leaf) uses the block shadow.
                const bool agg_partial =
                  lv.is_agg && lv.agg_leaf && (lv.width != lv.store_width || !is_zero_const(lv.bit_off));
                if (lv.is_signal && slot_of(lv.sig).is_array) {
                    emit_array_nba(lv, rhs);
                } else if (lv.is_agg && (!lv.agg_static || agg_partial)) {
                    emit_agg_nba(lv, rhs);
                } else {
                    lower_store_nb(lv, rhs);
                }
                return;
            }
            case Inst::Tag::AggCopy: {
                // Whole-object blocking copy: one memcpy of the storage. The
                // frontend emits this only for a trivially copyable object, so the byte
                // image is the value (no heap-owning leaf to deep-copy).
                const auto   info = ir_->extra_data<AggCopyInfo>(p.op_extra.extra);
                const LValue dlv  = lvalue_of(p.op_extra.op);
                const LValue slv  = lvalue_of(info.src);
                llvm::Value* dst;
                llvm::Value* src;
                llvm::Type*  st;
                if (dlv.is_agg) {
                    // An aggregate (contains a struct) uses the recursive storage,
                    // which is deterministic by type — both sides share the layout.
                    dst = agg_slot_ptr(dlv);
                    src = agg_slot_ptr(slv);
                    st  = agg_storage_ty(info.type);
                } else {
                    // A plain unpacked array: its slot layout is may-z-analysed per
                    // signal, so a memcpy is byte-valid only when both arrays landed
                    // on the same layout (the common never-z / matching case). A
                    // may-z mismatch (one array assigned z, the other not) would
                    // differ in the z plane; reject it rather than miscopy.
                    const SignalSlot& ds = slot_of(dlv.sig);
                    const SignalSlot& ss = slot_of(slv.sig);
                    if (ds.storage_ty != ss.storage_ty) {
                        throw BackendError{ "whole-array copy between arrays of differing "
                                            "storage layout is not supported" };
                    }
                    dst = dlv.base;
                    src = slv.base;
                    st  = ds.storage_ty;
                }
                const auto sz = env_->llmod.getDataLayout().getTypeAllocSize(st);
                const auto al = env_->llmod.getDataLayout().getABITypeAlign(st);
                // A blocking whole-object copy into a signal must wake its Any-edge
                // readers (a continuous / port copy or a comb process feeds off the
                // destination), but only when the bytes actually change — an
                // unconditional wake would never settle a feedback delta wave. Detect
                // the change with a memcmp of old dest vs new source *before* the copy.
                const bool service = !lowering_preinit_ && dlv.is_signal &&
                                     signal_has_waiters(dlv.sig);
                llvm::Value* changed = nullptr;
                if (service) {
                    auto cmp = rt_fn("memcmp", b_.getInt32Ty(),
                                     { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
                    changed   = b_.CreateICmpNE(
                      b_.CreateCall(cmp, { dst, src, b_.getInt64(sz) }), b_.getInt32(0));
                }
                b_.CreateMemCpy(dst, al, src, al, sz);
                if (service) {
                    auto* fn   = b_.GetInsertBlock()->getParent();
                    auto* svc  = llvm::BasicBlock::Create(ctx_, "aggcopy.svc", fn);
                    auto* cont = llvm::BasicBlock::Create(ctx_, "aggcopy.cont", fn);
                    b_.CreateCondBr(changed, svc, cont);
                    b_.SetInsertPoint(svc);
                    service_any_waiters(dlv.sig);
                    b_.CreateBr(cont);
                    b_.SetInsertPoint(cont);
                }
                return;
            }

            // Nonblocking intra-assignment delay (`x <= #d y`, LRM 9.4.5): sample
            // the RHS now into a deferred record, arm it on the wheel for `now + d`,
            // and continue without suspending. The NBA region of `now + d` commits
            // it (emit_intra_commit).
            case Inst::Tag::StoreNbDelay: {
                const auto    info  = ir_->extra_data<StoreNbDelayInfo>(p.op_extra.extra);
                const LValue& lv    = lvalue_of(p.op_extra.op);
                const Plane   rhs   = value_of(info.value);
                llvm::Value*  ticks = cast(must_of(value_of(info.delay)), 64, false);
                emit_intra_nba_arm(lv, rhs, ticks);
                return;
            }

            // Nonblocking intra-assignment event (`x <= @e y`, LRM 9.4.5): sample the
            // RHS + count now into a deferred record, register it on the trigger's
            // edge, continue without suspending. The trigger's wake path fires it and
            // the NBA region commits (emit_intra_commit).
            case Inst::Tag::StoreNbEvent: {
                const auto    info  = ir_->extra_data<StoreNbEventInfo>(p.op_extra.extra);
                const LValue& lv    = lvalue_of(p.op_extra.op);
                const Plane   rhs   = value_of(info.value);
                llvm::Value*  count = must_of(value_of(info.count));
                emit_intra_event_arm(lv, rhs, count,
                                     static_cast<uint32_t>(trigger_id(info.trig, info.edge)));
                return;
            }

            case Inst::Tag::DynArrayNew:
                lower_dyn_array_new(i);
                return;
            case Inst::Tag::DynArrayNewInit:
                lower_dyn_array_new_init(i);
                return;
            case Inst::Tag::DynArraySize:
                lower_dyn_array_size(i);
                return;
            case Inst::Tag::DynArrayDelete: {
                const auto& pd = ir_->payload(i).op_type;
                emit_handle_delete(lv_handle_ptr(lvalue_of(pd.op)), pd.type);
                return;
            }
            case Inst::Tag::DynArrayCopy:
                lower_dyn_array_copy(i);
                return;

            case Inst::Tag::QueueSize:
                lower_queue_size(i);
                return;
            case Inst::Tag::QueuePushBack:
                lower_queue_push(i, /*front=*/false);
                return;
            case Inst::Tag::QueuePushFront:
                lower_queue_push(i, /*front=*/true);
                return;
            case Inst::Tag::QueuePopBack:
                lower_queue_pop(i, /*front=*/false);
                return;
            case Inst::Tag::QueuePopFront:
                lower_queue_pop(i, /*front=*/true);
                return;
            case Inst::Tag::QueueInsert:
                lower_queue_insert(i);
                return;
            case Inst::Tag::QueueDeleteIdx:
                lower_queue_delete_idx(i);
                return;
            case Inst::Tag::QueueDeleteAll:
                lower_queue_delete_all(i);
                return;
            case Inst::Tag::QueueCopy:
                lower_queue_copy(i);
                return;

            case Inst::Tag::DynArrayReverse:
                lower_dyn_array_reverse(i);
                return;
            case Inst::Tag::DynArraySort:
                lower_dyn_array_sort(i, /*reversed=*/false);
                return;
            case Inst::Tag::DynArrayRsort:
                lower_dyn_array_sort(i, /*reversed=*/true);
                return;
            case Inst::Tag::DynArrayShuffle:
                lower_dyn_array_shuffle(i);
                return;
            case Inst::Tag::QueueReverse:
                lower_queue_reverse(i);
                return;
            case Inst::Tag::QueueSort:
                lower_queue_sort(i, /*reversed=*/false);
                return;
            case Inst::Tag::QueueRsort:
                lower_queue_sort(i, /*reversed=*/true);
                return;
            case Inst::Tag::QueueShuffle:
                lower_queue_shuffle(i);
                return;

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
                // Result signedness follows the base (op1); the exponent (op2) is
                // self-determined. Both flags feed Table 11-4's signed corners.
                const ValType lt = type_of(p.binary.l);
                const ValType rt = type_of(p.binary.r);
                set_value(i, lower_pow(value_of(p.binary.l), value_of(p.binary.r), lt.is_signed, rt.is_signed));
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

            case Inst::Tag::Clog2:
                set_value(i, lower_clog2(value_of(p.unary)));
                return;

            case Inst::Tag::Ctz:
            case Inst::Tag::Clz:
                set_value(i, lower_bit_index(ir_->tag(i), value_of(p.unary)));
                return;

            case Inst::Tag::CountBits:
                set_value(i,
                          lower_count_bits(value_of(p.op_extra.op),
                                           ir_->extra_data<CountBitsInfo>(p.op_extra.extra).mask));
                return;

            case Inst::Tag::OneHot:
                set_value(i, lower_onehot(value_of(p.unary), /*allow_zero=*/false));
                return;

            case Inst::Tag::OneHot0:
                set_value(i, lower_onehot(value_of(p.unary), /*allow_zero=*/true));
                return;

            case Inst::Tag::IsUnknown:
                set_value(i, lower_isunknown(value_of(p.unary)));
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

            case Inst::Tag::AggEq: {
                const auto info = ir_->extra_data<AggEqInfo>(p.op_extra.extra);
                set_value(i, lower_agg_eq(lvalue_of(p.op_extra.op), lvalue_of(info.rhs), info.type));
                return;
            }

            case Inst::Tag::WildEq:
                set_value(i, lower_wildeq(value_of(p.binary.l), value_of(p.binary.r)));
                return;

            case Inst::Tag::InsideSet:
                set_value(i, lower_inside_set(i));
                return;

            case Inst::Tag::FAdd:
            case Inst::Tag::FSub:
            case Inst::Tag::FMul:
            case Inst::Tag::FDiv:
            case Inst::Tag::FPow:
                set_value(i, lower_real_arith(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;
            case Inst::Tag::FNeg:
                set_value(i, real_plane(b_.CreateFNeg(fp_of(value_of(p.unary)))));
                return;
            case Inst::Tag::FEq:
            case Inst::Tag::FNe:
            case Inst::Tag::FLt:
            case Inst::Tag::FLe:
            case Inst::Tag::FGt:
            case Inst::Tag::FGe:
                set_value(i, lower_real_compare(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;

            case Inst::Tag::ItoF: {
                // The must half reads x/z bits as 0 (LRM 6.12.1); signedness
                // comes from the source operand type.
                const ValType st = type_of(p.op_type.op);
                const ValType dt = from_ref(p.op_type.type);
                llvm::Value*  iv = must_of(value_of(p.op_type.op));
                llvm::Value*  fv =
                  st.is_signed ? b_.CreateSIToFP(iv, fp_ty(dt.width)) : b_.CreateUIToFP(iv, fp_ty(dt.width));
                set_value(i, real_plane(fv));
                return;
            }
            case Inst::Tag::FtoI:
            case Inst::Tag::FtoITrunc:
                set_value(i, lower_ftoi(ir_->tag(i), value_of(p.op_type.op), from_ref(p.op_type.type)));
                return;
            case Inst::Tag::FBitcast: {
                const ValType st = type_of(p.op_type.op);
                const ValType dt = from_ref(p.op_type.type);
                if (dt.is_real) {
                    // $bitstoreal / $bitstoshortreal: integral pattern -> float.
                    llvm::Value* pat = cast(must_of(value_of(p.op_type.op)), dt.width, false);
                    set_value(i, real_plane(b_.CreateBitCast(pat, fp_ty(dt.width))));
                } else {
                    // $realtobits / $shortrealtobits: float -> integral pattern.
                    llvm::Value* pat = b_.CreateBitCast(fp_of(value_of(p.op_type.op)), int_ty(st.width));
                    pat              = cast(pat, dt.width, false);
                    set_value(i, packed(pat, pat, zero(dt.width)));
                }
                return;
            }
            case Inst::Tag::FResize: {
                const ValType st = type_of(p.op_type.op);
                const ValType dt = from_ref(p.op_type.type);
                llvm::Value*  v  = fp_of(value_of(p.op_type.op));
                if (st.width < dt.width) {
                    v = b_.CreateFPExt(v, fp_ty(dt.width));
                } else if (st.width > dt.width) {
                    v = b_.CreateFPTrunc(v, fp_ty(dt.width));
                }
                set_value(i, real_plane(v));
                return;
            }
            case Inst::Tag::RealMathUnary:
                set_value(i, lower_real_math_unary(i));
                return;
            case Inst::Tag::RealMathBinary:
                set_value(i, lower_real_math_binary(i));
                return;

            case Inst::Tag::Land:
            case Inst::Tag::Lor:
                set_value(i, lower_logical(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
                return;
            case Inst::Tag::Lnot:
                set_value(i, lower_lnot(value_of(p.unary)));
                return;

            case Inst::Tag::Select: {
                const auto select = ir_->extra_data<Select>(p.op_extra.extra);
                if (type_of(select.a).is_string) {
                    throw BackendError{ "internal: string value in select" };
                }
                if (type_of(select.a).is_real) {
                    set_value(i, lower_real_mux(value_of(p.op_extra.op), value_of(select.a), value_of(select.b)));
                } else {
                    set_value(i, lower_select(value_of(p.op_extra.op), value_of(select.a), value_of(select.b)));
                }
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
            case Inst::Tag::StreamReverse:
                set_value(i, lower_stream_reverse(value_of(p.op_imm.op), p.op_imm.imm));
                return;
            case Inst::Tag::Resize: {
                // Truncate or extend all planes (sign per target type).
                if (type_of(p.op_type.op).is_string) {
                    throw BackendError{ "internal: string value in resize" };
                }
                const ValType to = from_ref(p.op_type.type);
                // Depth guard: an integer resize would silently zext / trunc a
                // float bit pattern. The frontend must emit ItoF / FtoI /
                // FResize instead.
                if (type_of(p.op_type.op).is_real || to.is_real) {
                    throw BackendError{ "internal: resize on a real value" };
                }
                // Resize is also the domain-conversion site: slang lowers an
                // implicit 2-state <-> 4-state conversion (LRM 6.3.1) as a
                // ConversionExpression -> Resize to the target type.
                set_value(i, convert(value_of(p.op_type.op), to.width, to.four_state, to.is_signed));
                return;
            }

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
            case Inst::Tag::StrAtoi:
            case Inst::Tag::StrAtohex:
            case Inst::Tag::StrAtooct:
            case Inst::Tag::StrAtobin: {
                using T          = Inst::Tag;
                const T tag      = ir_->tag(i);
                auto    fn       = tag == T::StrAtoi    ? str_atoi_fn()
                                  : tag == T::StrAtohex ? str_atohex_fn()
                                  : tag == T::StrAtooct ? str_atooct_fn()
                                                        : str_atobin_fn();
                llvm::Value* v = b_.CreateCall(fn, { str_of(p.unary) });
                set_value(i, packed(v, v, zero(32)));
                return;
            }
            case Inst::Tag::StrAtoreal: {
                llvm::Value* v = b_.CreateCall(str_atoreal_fn(), { str_of(p.unary) });
                set_value(i, real_plane(v));
                return;
            }
            case Inst::Tag::StrItoa:
            case Inst::Tag::StrHextoa:
            case Inst::Tag::StrOctoa:
            case Inst::Tag::StrBintoa: {
                using T          = Inst::Tag;
                const T tag      = ir_->tag(i);
                auto    fn       = tag == T::StrItoa      ? str_itoa_fn()
                                  : tag == T::StrHextoa   ? str_hextoa_fn()
                                  : tag == T::StrOctoa    ? str_octtoa_fn()
                                                          : str_bintoa_fn();
                const LValue& lv = lvalue_of(p.binary.l);
                llvm::Value*  v =
                  cast(must_of(value_of(p.binary.r)), 32, type_of(p.binary.r).is_signed);
                b_.CreateCall(fn, { lv_string_ptr(lv), v });
                return;
            }
            case Inst::Tag::StrRealtoa: {
                const LValue& lv = lvalue_of(p.binary.l);
                llvm::Value*  v  = fp_of(value_of(p.binary.r));
                b_.CreateCall(str_realtoa_fn(), { lv_string_ptr(lv), v });
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
            case Inst::Tag::EnumName: {
                // Materialize the member table: one contiguous value-bytes blob
                // plus parallel name-pointer / length globals, then let the runtime
                // byte-compare the operand's must plane against each member value.
                const auto         info   = ir_->extra_data<EnumNameInfo>(p.op_extra.extra);
                const auto         start  = static_cast<uint32_t>(info.entries.start);
                const auto         end    = static_cast<uint32_t>(info.entries.end);
                constexpr uint32_t stride = sizeof(EnumNameEntry) / sizeof(uint32_t);
                llvm::Type* const  i32    = b_.getInt32Ty();
                std::string                  blob; // count * vbytes value bytes
                std::vector<llvm::Constant*> names, lens;
                uint32_t                     vbytes = 0;
                for (uint32_t w = start; w < end; w += stride) {
                    const auto e = ir_->extra_data<EnumNameEntry>(static_cast<ExtraIndex>(w));
                    vbytes       = e.val_len;
                    blob += ir_->extra_bytes(e.val_at, e.val_len);
                    names.push_back(b_.CreateGlobalString(ir_->extra_bytes(e.name_at, e.name_len)));
                    lens.push_back(llvm::ConstantInt::get(i32, e.name_len));
                }
                const auto count   = static_cast<uint32_t>(names.size());
                auto*      ptr_arr = llvm::ArrayType::get(raw_ptr_ty(), count);
                auto*      len_arr = llvm::ArrayType::get(i32, count);
                auto*      blob_c  = llvm::ConstantDataArray::get(
                  ctx_, llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(blob.data()), blob.size()));
                auto make_gv = [&](llvm::Type* ty, llvm::Constant* init, const char* nm) {
                    return new llvm::GlobalVariable(env_->llmod, ty, true, llvm::GlobalValue::InternalLinkage, init, nm);
                };
                llvm::Value* vg = make_gv(blob_c->getType(), blob_c, "clotho.enum.vals");
                llvm::Value* ng = make_gv(ptr_arr, llvm::ConstantArray::get(ptr_arr, names), "clotho.enum.names");
                llvm::Value* lg = make_gv(len_arr, llvm::ConstantArray::get(len_arr, lens), "clotho.enum.lens");
                // Spill the operand's must plane to a byte buffer for the compare.
                const uint32_t spill  = plane_bytes(type_of(p.op_extra.op).width);
                llvm::Value*   valbuf = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(spill), "enum.val");
                b_.CreateAlignedStore(must_of(value_of(p.op_extra.op)), valbuf, llvm::Align(1));
                b_.CreateCall(enum_name_fn(),
                              { str_of(i), valbuf, b_.getInt32(vbytes), b_.getInt32(count), vg, ng, lg });
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

            case Inst::Tag::Switch:
                lower_switch(i);
                return;

            case Inst::Tag::SelectLazy:
                lower_select_lazy(i);
                return;

            case Inst::Tag::Loop:
                lower_loop(i);
                return;

            case Inst::Tag::Break:
                // Terminator: branch to the innermost loop's exit block. slang
                // guarantees a break appears only inside a loop.
                b_.CreateBr(loop_targets_.back().second);
                return;
            case Inst::Tag::Continue:
                // Terminator: branch to the innermost loop's step block (which
                // re-tests the predicate via the back-edge).
                b_.CreateBr(loop_targets_.back().first);
                return;

            case Inst::Tag::Delay:
                // A suspend ends the current NBA segment: post-suspend NBAs stage at a
                // later instant and must commit in a different NBA region.
                ++cur_seg_epoch_;
                lower_delay(p.unary);
                return;

            case Inst::Tag::Wait:
                ++cur_seg_epoch_;
                lower_wait(p.extra);
                return;

            case Inst::Tag::EventTrigger:
                // `-> e` (LRM 15.5.2): stamp the trigger time and wake every
                // waiter now. Zero-time — the current process does not suspend, so
                // it does not end the NBA segment.
                lower_event_trigger(p.signal);
                return;

            case Inst::Tag::EventTriggerNb:
                // `->> e` (LRM 15.5.4): mark the event pending; the NBA region
                // stamps + wakes. The triggering process does not block.
                b_.CreateStore(b_.getInt8(1), sym(event_pending_gv_[static_cast<uint32_t>(p.signal)]));
                return;

            case Inst::Tag::EventTriggerNbDelayed: {
                // `->> #d e` (LRM 15.5.4): schedule a detached wheel timer that
                // fires the event's on-expiry thunk `d` ticks from now. The process
                // does not block. The timer node (a bare rt_timer = {next, on_expire}
                // = 2 pointers) is heap-allocated here and freed by the thunk.
                const Signal ev       = ir_->extra_data<EventDelayInfo>(p.op_extra.extra).event;
                llvm::Value* ticks    = cast(must_of(value_of(p.op_extra.op)), 64, false);
                llvm::Value* deadline = b_.CreateAdd(b_.CreateCall(wheel_now_fn(), { wheel_ }), ticks);
                llvm::Value* node     = b_.CreateCall(alloc_fn(), { b_.getInt64(16) });
                b_.CreateCall(wheel_schedule_fn(),
                              { wheel_, node, deadline, sym(event_delay_thunk_[static_cast<uint32_t>(ev)]) });
                return;
            }

            case Inst::Tag::Display:
                lower_display(i);
                return;

            case Inst::Tag::FDisplay:
                lower_fdisplay(i);
                return;

            case Inst::Tag::TimeFormat:
                lower_timeformat(i);
                return;

            case Inst::Tag::Finish:
                // $finish / $stop: rt_finish prints the banner then exits, never
                // returning (noreturn), so the optimizer prunes the region tail.
                lower_finish(i);
                return;

            case Inst::Tag::CaseViol:
                // priority-case violation: rt_case_violation warns and returns
                // (non-fatal), so the region continues normally after it.
                lower_case_viol(i);
                return;

            case Inst::Tag::Severity:
                // $info / $warning / $error / $fatal report (LRM 20.10) or an
                // immediate-assertion default failure (LRM 16.3): rt_severity
                // renders the message and returns (non-fatal). $fatal's
                // termination is a separate Finish following this node.
                lower_severity(i);
                return;

            case Inst::Tag::Fflush:
                lower_fflush(i);
                return;

            case Inst::Tag::FopenFd:
                lower_fopen_fd(i);
                return;

            case Inst::Tag::FopenMcd:
                lower_fopen_mcd(i);
                return;

            case Inst::Tag::Fclose:
                lower_fclose(i);
                return;

            case Inst::Tag::Fgetc:
                lower_file_unary(i, "rt_fgetc");
                return;
            case Inst::Tag::Feof:
                lower_file_unary(i, "rt_feof");
                return;
            case Inst::Tag::Ftell:
                lower_file_unary(i, "rt_ftell");
                return;
            case Inst::Tag::Rewind:
                lower_file_unary(i, "rt_rewind");
                return;
            case Inst::Tag::Ungetc:
                lower_ungetc(i);
                return;
            case Inst::Tag::Fseek:
                lower_fseek(i);
                return;

            case Inst::Tag::Fgets:
                lower_fgets(i);
                return;

            case Inst::Tag::Sformat:
                lower_sformat(i);
                return;

            case Inst::Tag::Sformatf:
                lower_sformatf(i);
                return;

            case Inst::Tag::Ferror:
                lower_ferror(i);
                return;

            case Inst::Tag::Fscanf:
            case Inst::Tag::Sscanf:
                lower_scanf(i);
                return;

            case Inst::Tag::TestPlusArgs:
                lower_test_plusargs(i);
                return;

            case Inst::Tag::ValuePlusArgs:
                lower_value_plusargs(i);
                return;

            case Inst::Tag::FreadWord:
                lower_fread_word(i);
                return;

            case Inst::Tag::FreadMem:
                lower_fread_mem(i);
                return;

            case Inst::Tag::ReadMem:
                lower_readmem(i);
                return;

            case Inst::Tag::DumpFile:
                lower_dump_file(i);
                return;

            case Inst::Tag::DumpVars:
                lower_dump_vars();
                return;

            case Inst::Tag::DumpOff:
            case Inst::Tag::DumpOn:
            case Inst::Tag::DumpAll:
            case Inst::Tag::DumpFlush:
                lower_dump_control(ir_->tag(i));
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

            case Inst::Tag::EventTriggered:
                set_value(i, lower_event_triggered(p.signal));
                return;

            case Inst::Tag::Random: {
                // The PRNG draw is always known (no x/z): must == may, z == 0.
                llvm::Value* draw = b_.CreateCall(random_fn(), {});
                set_value(i, packed(draw, draw, zero(32)));
                return;
            }

            case Inst::Tag::DistUnary: {
                // $dist_* / seeded $random: call the reference routine with the
                // seed word and parameter(s) (must planes, sign-extended to
                // i32); the packed {seed, draw} i64 is a definite 2-state value
                // the frontend slices.
                const auto&  p    = ir_->payload(i).op_extra;
                const auto   info = ir_->extra_data<DistUnaryInfo>(p.extra);
                llvm::Value* seed = cast(must_of(value_of(p.op)), 32, true);
                llvm::Value* p1   = cast(must_of(value_of(info.p1)), 32, true);
                set_value(i, two_val(b_.CreateCall(dist_fn(dist_symbol(info.fn), 1), { seed, p1 })));
                return;
            }
            case Inst::Tag::DistBinary: {
                const auto&  p    = ir_->payload(i).op_extra;
                const auto   info = ir_->extra_data<DistBinaryInfo>(p.extra);
                llvm::Value* seed = cast(must_of(value_of(p.op)), 32, true);
                llvm::Value* p1   = cast(must_of(value_of(info.p1)), 32, true);
                llvm::Value* p2   = cast(must_of(value_of(info.p2)), 32, true);
                set_value(
                  i, two_val(b_.CreateCall(dist_fn(dist_symbol(info.fn), 2), { seed, p1, p2 })));
                return;
            }

            case Inst::Tag::Urandom:
                set_value(i, lower_urandom());
                return;

            case Inst::Tag::UrandomSeeded: {
                // $urandom(seed): reseed the process RNG from `seed`, then draw.
                llvm::Value* seed = cast(must_of(value_of(ir_->payload(i).unary)), 64, false);
                b_.CreateCall(urandom_seed_fn(), { rng_slot_, seed });
                set_value(i, lower_urandom());
                return;
            }

            case Inst::Tag::UrandomRange: {
                // $urandom_range(maxval, minval): Lemire multiply-shift of one
                // draw into [lo, hi]; swap when min > max (LRM §18.13.2). The
                // negligible bias of the un-rejected multiply-shift is acceptable
                // (the algorithm is LRM-unspecified).
                llvm::Value* mx = cast(must_of(value_of(ir_->payload(i).binary.l)), 32, false);
                llvm::Value* mn = cast(must_of(value_of(ir_->payload(i).binary.r)), 32, false);
                llvm::Value* ge = b_.CreateICmpUGE(mx, mn);
                llvm::Value* hi = b_.CreateSelect(ge, mx, mn);
                llvm::Value* lo = b_.CreateSelect(ge, mn, mx);
                // range = (hi - lo) + 1, in [1, 2^32], in 64 bits.
                llvm::Value* range =
                  b_.CreateAdd(b_.CreateZExt(b_.CreateSub(hi, lo), b_.getInt64Ty()), b_.getInt64(1));
                // draw32 = high 32 bits of the 64-bit result (already zero-extended
                // into the low half by the shift).
                llvm::Value* draw32 = b_.CreateLShr(emit_xoshiro_next(), 32);
                llvm::Value* off =
                  b_.CreateTrunc(b_.CreateLShr(b_.CreateMul(draw32, range), 32), b_.getInt32Ty());
                set_value(i, two_val(b_.CreateAdd(lo, off)));
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
                const ValType t = from_ref(p.arg.type);
                Plane         v{ fn_->getArg(base), fn_->getArg(base + 1) };
                if (t.is_real) {
                    v = real_plane(planes_to_real(v, t.width));
                }
                set_value(i, v);
                return;
            }
            case Inst::Tag::Ret:
                lower_ret(p.unary);
                return;
            case Inst::Tag::RetVoid:
                lower_ret_void();
                return;
            case Inst::Tag::Call:
                lower_call_inst(i);
                return;
            case Inst::Tag::TaskCall:
                // A task call may suspend the caller (task with #delay / @event) and
                // stages its own NBAs into the shared shadow; end the segment so the
                // caller's pre/post-call NBAs never share a block with it.
                ++cur_seg_epoch_;
                lower_task_call_inst(i);
                ++cur_seg_epoch_;
                return;
            case Inst::Tag::Fork:
                // A fork spawns child coroutines and (for `join`) suspends the
                // parent; end the segment so pre/post-fork NBAs never share a block.
                ++cur_seg_epoch_;
                lower_fork_inst(i);
                ++cur_seg_epoch_;
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

        // A real constant is its IEEE-754 bit pattern in the val plane.
        if (mod_.pool.is_float(v.type)) {
            llvm::Value* pat = const_from_bytes(bytes.subspan(0, pb), w);
            return real_plane(b_.CreateBitCast(pat, fp_ty(w)));
        }

        // The pool stores canonical val/xz; ingest to must/may/z so 0/1/x/z stay
        // distinct (case-equality needs to tell a literal x from a literal z).
        // VPI: x=(val1,xz1), z=(val0,xz1) => must=val&~xz, may=val|xz, z=xz&~val.
        llvm::Value* val = const_from_bytes(bytes.subspan(0, pb), w);
        // A 2-state constant has no x/z: its value is the single iW word.
        if (lay.domain != Domain::Four) {
            return two_val(val);
        }
        llvm::Value* xz = const_from_bytes(bytes.subspan(pb, pb), w);
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
        if (s.is_agg) {
            // The aggregate root; a following MemberSelect / ElemSelect chain
            // extends agg_path and (at a scalar/string leaf) fills the leaf
            // metadata. A bare aggregate lvalue is never loaded/stored directly.
            LValue lv{};
            lv.base      = storage_ptr(s);
            lv.root_ty   = s.storage_ty;
            lv.is_agg    = true;
            lv.is_signal = true;
            lv.sig       = sig;
            return lv;
        }
        return LValue{ .base        = storage_ptr(s),
                       .slot_ty     = s.slot_ty,
                       .has_z       = s.has_z,
                       .narrow      = s.narrow,
                       .array_ty    = s.is_array ? llvm::cast<llvm::ArrayType>(s.storage_ty) : nullptr,
                       .store_width = s.width,
                       .bit_off     = b_.getInt32(0),
                       .width       = s.width,
                       .is_signal   = true,
                       .sig         = sig,
                       .is_string   = s.is_string };
    }

    // Whole-storage lvalue for a function-frame local (per-call frame struct). A ref
    // formal has no frame storage: its base is the passed actual pointer (recorded
    // in ref_ptr_ by the prologue), through which every load / store is live.
    LValue local_lvalue(uint32_t index) {
        const FrameSlot& s = frame_slots_[index];
        if (s.is_agg) {
            LValue lv{};
            lv.base      = b_.CreateStructGEP(frame_ty_, frame_, index);
            lv.root_ty   = s.storage_ty;
            lv.is_agg    = true;
            lv.is_signal = false;
            return lv;
        }
        llvm::Value* base = s.is_ref ? ref_ptr_[index] : b_.CreateStructGEP(frame_ty_, frame_, index);
        return LValue{ .base        = base,
                       .slot_ty     = s.slot_ty,
                       .has_z       = !s.narrow,
                       .narrow      = s.narrow,
                       .store_width = s.width,
                       .bit_off     = b_.getInt32(0),
                       .width       = s.width,
                       .is_signal   = false,
                       .sig         = Signal{ 0 },
                       .is_string   = s.is_string };
    }

    Plane lower_load(const LValue& lv) {
        const uint32_t w  = lv.store_width;

        // Narrow 2-state slot: the stored word IS the iW value.
        if (lv.narrow) {
            llvm::Value* v = b_.CreateLoad(int_ty(w), lv_mm_ptr(lv));
            if (lv.width == w && is_zero_const(lv.bit_off)) {
                return two_val(v);
            }
            // A bounds-checked select aligns poison-free and reads 0 outside range
            // (LRM 11.5.1: an out-of-range read of a 2-state value is 0).
            if (lv.range_mask != nullptr) {
                llvm::Value* shifted = align_shl(v, b_.CreateNeg(lv.bit_off), w);
                llvm::Value* valid   = align_shl(lv.range_mask, b_.CreateNeg(lv.bit_off), w);
                return two_val(cast(b_.CreateAnd(shifted, valid), lv.width, false));
            }
            llvm::Value* off = cast(lv.bit_off, w, false);
            return two_val(cast(b_.CreateLShr(v, off), lv.width, false));
        }

        llvm::Value*   mm = b_.CreateLoad(int_ty(2 * w), lv_mm_ptr(lv));
        // A never-z slot stores no z word; its z plane is a constant zero.
        llvm::Value* z = lv.has_z ? static_cast<llvm::Value*>(b_.CreateLoad(int_ty(w), lv_z_ptr(lv))) : zero(w);
        if (lv.width == w && is_zero_const(lv.bit_off)) {
            return { mm, z };
        }
        const Plane full = { mm, z };
        // A bounds-checked select aligns poison-free and forces x on the bits that
        // fall outside the vector (LRM 11.5.1: must=0, may=1, z=0).
        if (lv.range_mask != nullptr) {
            llvm::Value* down  = b_.CreateNeg(lv.bit_off);
            llvm::Value* must  = align_shl(must_of(full), down, w);
            llvm::Value* may   = align_shl(may_of(full), down, w);
            llvm::Value* zp    = align_shl(z, down, w);
            llvm::Value* valid = align_shl(lv.range_mask, down, w);
            llvm::Value* oob   = b_.CreateNot(valid);
            return packed(cast(b_.CreateAnd(must, valid), lv.width, false),
                          cast(b_.CreateOr(may, oob), lv.width, false),
                          cast(b_.CreateAnd(zp, valid), lv.width, false));
        }
        // Bit/part select: unpack, shift each plane down by the offset, re-pack.
        llvm::Value* off = cast(lv.bit_off, w, false);
        return packed(cast(b_.CreateLShr(must_of(full), off), lv.width, false),
                      cast(b_.CreateLShr(may_of(full), off), lv.width, false),
                      cast(b_.CreateLShr(z, off), lv.width, false));
    }

    // Bounds-checked load of a dynamic-array element (LRM 7.5.1): in range, load
    // from the heap buffer; out of range, yield the element default (X for
    // 4-state, 0 for 2-state). The heap is only dereferenced on the in-range
    // path, so an out-of-range or negative index never touches memory. Returns
    // the same Plane shape lower_load would (narrow -> two_val; else { i2W, iW }),
    // so the caller's real / domain handling is unchanged.
    Plane dyn_elem_load(const LValue& lv) {
        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* inbb  = llvm::BasicBlock::Create(ctx_, "dyn.load.in", fn);
        auto* defbb = llvm::BasicBlock::Create(ctx_, "dyn.load.def", fn);
        auto* cont  = llvm::BasicBlock::Create(ctx_, "dyn.load.cont", fn);
        b_.CreateCondBr(lv.dyn_inrange, inbb, defbb);

        b_.SetInsertPoint(inbb);
        LValue clean       = lv;
        clean.dyn_inrange  = nullptr;
        const Plane vin    = lower_load(clean);
        auto*       inend  = b_.GetInsertBlock();
        b_.CreateBr(cont);

        b_.SetInsertPoint(defbb);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        if (lv.narrow) {
            auto* phi = b_.CreatePHI(int_ty(lv.width), 2);
            phi->addIncoming(vin.mm, inend);
            phi->addIncoming(zero(lv.width), defbb);
            return two_val(phi);
        }
        auto* mmphi = b_.CreatePHI(int_ty(2 * lv.width), 2);
        mmphi->addIncoming(vin.mm, inend);
        mmphi->addIncoming(x_mm(lv.width), defbb);
        auto* zphi = b_.CreatePHI(int_ty(lv.width), 2);
        zphi->addIncoming(z_of(vin), inend);
        zphi->addIncoming(zero(lv.width), defbb);
        return { mmphi, zphi };
    }

    // Bounds-checked store of a dynamic-array element (LRM 7.5.1): the store and
    // its wait-queue servicing run only when the index is in [0, len); an
    // out-of-range write is a no-op.
    void dyn_elem_store(const LValue& lv, Plane rhs) {
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* dobb = llvm::BasicBlock::Create(ctx_, "dyn.store.do", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "dyn.store.cont", fn);
        b_.CreateCondBr(lv.dyn_inrange, dobb, cont);

        b_.SetInsertPoint(dobb);
        LValue clean              = lv;
        clean.dyn_inrange         = nullptr;
        const ServiceSnapshot old = service_snapshot(clean);
        lower_store(clean, rhs);
        service_after_write(clean, old);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
    }

    // Dynamic-array operations over the handle lvalue (LRM 7.5). Element access
    // rides the ElemSelect dynarr path, not these. Element facts come from the
    // payload's own array-type handle, so each op works for a module signal or
    // a frame-local dynamic array alike. Handles resolve through lv_handle_ptr
    // so a struct-member / class-property array works too.
    void lower_dyn_array_new(Index i) {
        const auto&   p  = ir_->payload(i).op_extra;
        const auto    da = ir_->extra_data<DynArrayArg>(p.extra);
        const DynElem e  = dynarr_elem(da.array_type);
        llvm::Value*  n  = cast(must_of(value_of(da.arg)), 64, false);
        b_.CreateCall(dynarr_new_fn(),
                      { lv_handle_ptr(lvalue_of(p.op)), elem_ops_global(da.array_type), n });
    }

    void lower_dyn_array_new_init(Index i) {
        const auto&   p  = ir_->payload(i).op_extra;
        const auto    da = ir_->extra_data<DynArrayNewInitInfo>(p.extra);
        const DynElem e  = dynarr_elem(da.array_type);
        llvm::Value*  n  = cast(must_of(value_of(da.size)), 64, false);
        b_.CreateCall(dynarr_new_copy_fn(),
                      { lv_handle_ptr(lvalue_of(p.op)), elem_ops_global(da.array_type), n,
                        lv_handle_ptr(lvalue_of(da.src)) });
    }

    void lower_dyn_array_size(Index i) {
        llvm::Value* handle = lv_handle_ptr(lvalue_of(ir_->payload(i).op_type.op));
        llvm::Value* len =
          b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 1));
        set_value(i, two_val(cast(len, 32, false)));
    }

    void lower_dyn_array_copy(Index i) {
        const auto& p  = ir_->payload(i).op_extra;
        const auto  da = ir_->extra_data<DynArrayArg>(p.extra);
        b_.CreateCall(dynarr_assign_fn(),
                      { lv_handle_ptr(lvalue_of(p.op)), lv_handle_ptr(lvalue_of(da.arg)),
                        elem_ops_global(da.array_type) });
    }

    // Array ordering methods (LRM 7.12.2). Each carries only the handle lvalue +
    // interned type (op_type payload). Reverse is element-agnostic; Sort/Rsort
    // pass the value comparator descriptor derived from the element type;
    // Shuffle threads the process frame RNG slot.
    void lower_dyn_array_reverse(Index i) {
        const auto&   p = ir_->payload(i).op_type;
        const DynElem e = dynarr_elem(p.type);
        b_.CreateCall(dynarr_reverse_fn(), { lvalue_of(p.op).base, b_.getInt64(dynarr_stride(e)) });
    }

    void lower_dyn_array_sort(Index i, bool reversed) {
        const auto&    p = ir_->payload(i).op_type;
        const DynElem  e = dynarr_elem(p.type);
        const SortDesc d = dynarr_sort_desc(p.type);
        b_.CreateCall(dynarr_sort_fn(), { lvalue_of(p.op).base, b_.getInt64(dynarr_stride(e)),
                                          b_.getInt32(d.kind), b_.getInt32(d.width),
                                          b_.getInt32(reversed ? 1 : 0) });
    }

    void lower_dyn_array_shuffle(Index i) {
        const auto&   p = ir_->payload(i).op_type;
        const DynElem e = dynarr_elem(p.type);
        // rng_slot_ is non-null: process_uses_urandom flags a shuffling process,
        // so lower_group allocates and seeds the frame RNG.
        assert(rng_slot_ != nullptr && "shuffle needs the frame RNG");
        b_.CreateCall(dynarr_shuffle_fn(),
                      { lvalue_of(p.op).base, b_.getInt64(dynarr_stride(e)), rng_slot_ });
    }

    // An alloca placed at the top of the current function's entry block (not the
    // current insert point), so a scratch buffer used inside a loop body is
    // allocated once per activation rather than growing the stack per iteration.
    llvm::AllocaInst* entry_alloca(llvm::Type* ty, const llvm::Twine& name) {
        auto*             fn = b_.GetInsertBlock()->getParent();
        auto&             eb = fn->getEntryBlock();
        llvm::IRBuilder<> ab(&eb, eb.getFirstInsertionPt());
        return ab.CreateAlloca(ty, nullptr, name);
    }

    // An lvalue addressing a single element slot at raw pointer `ptr` (a scratch
    // alloca), for the shared lower_load / lower_store plane machinery.
    LValue elem_scratch_lvalue(llvm::Value* ptr, const DynElem& e) {
        LValue lv;
        lv.base        = ptr;
        lv.slot_ty     = e.slot_ty;
        lv.has_z       = e.has_z;
        lv.narrow      = e.narrow;
        lv.array_ty    = llvm::ArrayType::get(e.slot_ty, 0);
        lv.elem_idx    = b_.getInt32(0);
        lv.store_width = e.width;
        lv.bit_off     = b_.getInt32(0);
        lv.width       = e.width;
        lv.is_signal   = false;
        return lv;
    }

    // The element power-on default as an in-register constant (X for 4-state, 0
    // for 2-state / real), used to pre-fill a pop scratch so an empty-queue pop
    // yields the default.
    llvm::Constant* elem_default_const(const DynElem& e) {
        llvm::Constant*              mm = e.narrow ? zero(e.width) : x_mm(e.width);
        std::vector<llvm::Constant*> fields{ mm };
        if (e.has_z) {
            fields.push_back(zero(e.width));
        }
        return llvm::ConstantStruct::get(e.slot_ty, fields);
    }

    // Queue operations over the handle lvalue (LRM 7.10.2). Indexed element
    // access q[i] rides the ElemSelect queue path, not these. Element values
    // cross to the runtime through a scratch element slot (push / insert stage
    // the value in; pop copies out). Handles resolve through lv_handle_ptr so a
    // struct-member / class-property queue works too.
    void lower_queue_size(Index i) {
        llvm::Value* handle = lv_handle_ptr(lvalue_of(ir_->payload(i).op_type.op));
        llvm::Value* len =
          b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 2));
        set_value(i, two_val(cast(len, 32, false)));
    }

    void lower_queue_push(Index i, bool front) {
        const auto&   p     = ir_->payload(i).op_extra;
        const auto    qa    = ir_->extra_data<QueueArg>(p.extra);
        const DynElem e     = dynarr_elem(qa.queue_type);
        llvm::Value*  ops   = elem_ops_global(qa.queue_type);
        llvm::Value*  bound = b_.getInt64(std::get<Type>(mod_.pool.get(qa.queue_type)).left);
        if (e.is_string) {
            // A string value is already an rt_string handle pointer; the runtime
            // deep-copies it into the new slot, so it is passed directly.
            b_.CreateCall(queue_push_fn(front),
                          { lv_handle_ptr(lvalue_of(p.op)), ops, bound, str_of(qa.arg) });
            return;
        }
        auto* scratch = entry_alloca(e.slot_ty, "q.elem");
        lower_store(elem_scratch_lvalue(scratch, e), value_of(qa.arg));
        b_.CreateCall(queue_push_fn(front), { lv_handle_ptr(lvalue_of(p.op)), ops, bound, scratch });
    }

    void lower_queue_pop(Index i, bool front) {
        const auto&   p   = ir_->payload(i).op_type;
        const DynElem e   = dynarr_elem(p.type);
        llvm::Value*  ops = elem_ops_global(p.type);
        if (e.is_string) {
            // The result handle is this instruction's string temp
            // (produces_string_temp). Reset it to the owned-empty state, then
            // the runtime RELOCATES the popped element into it (an empty pop
            // leaves the empty string, the Table 7-1 default).
            llvm::Value* out = str_of(i);
            b_.CreateCall(str_deinit_fn(), { out });
            b_.CreateCall(str_init_fn(), { out });
            b_.CreateCall(queue_pop_fn(front), { lv_handle_ptr(lvalue_of(p.op)), ops, out });
            return;
        }
        auto* scratch = entry_alloca(e.slot_ty, "q.elem");
        // Pre-fill with the default: the runtime leaves an empty-queue pop
        // untouched, so the load below then reads the element default.
        b_.CreateStore(elem_default_const(e), scratch);
        b_.CreateCall(queue_pop_fn(front), { lv_handle_ptr(lvalue_of(p.op)), ops, scratch });
        Plane         v = lower_load(elem_scratch_lvalue(scratch, e));
        const ValType t = type_of(i);
        if (t.is_real) {
            v = real_plane(planes_to_real(v, t.width));
        }
        set_value(i, v);
    }

    void lower_queue_insert(Index i) {
        const auto&   p     = ir_->payload(i).op_extra;
        const auto    qi    = ir_->extra_data<QueueInsertInfo>(p.extra);
        const DynElem e     = dynarr_elem(qi.queue_type);
        llvm::Value*  ops   = elem_ops_global(qi.queue_type);
        llvm::Value*  idx   = cast(must_of(value_of(qi.idx)), 64, false);
        llvm::Value*  bound = b_.getInt64(std::get<Type>(mod_.pool.get(qi.queue_type)).left);
        if (e.is_string) {
            b_.CreateCall(queue_insert_fn(),
                          { lv_handle_ptr(lvalue_of(p.op)), ops, bound, idx, str_of(qi.val) });
            return;
        }
        auto* scratch = entry_alloca(e.slot_ty, "q.elem");
        lower_store(elem_scratch_lvalue(scratch, e), value_of(qi.val));
        b_.CreateCall(queue_insert_fn(), { lv_handle_ptr(lvalue_of(p.op)), ops, bound, idx, scratch });
    }

    void lower_queue_delete_idx(Index i) {
        const auto&  p   = ir_->payload(i).op_extra;
        const auto   qa  = ir_->extra_data<QueueArg>(p.extra);
        llvm::Value* idx = cast(must_of(value_of(qa.arg)), 64, false);
        b_.CreateCall(queue_delete_idx_fn(),
                      { lv_handle_ptr(lvalue_of(p.op)), elem_ops_global(qa.queue_type), idx });
    }

    void lower_queue_delete_all(Index i) {
        const auto& p = ir_->payload(i).op_type;
        emit_handle_delete(lv_handle_ptr(lvalue_of(p.op)), p.type);
    }

    void lower_queue_copy(Index i) {
        const auto& p  = ir_->payload(i).op_extra;
        const auto  qa = ir_->extra_data<QueueArg>(p.extra);
        b_.CreateCall(queue_assign_fn(),
                      { lv_handle_ptr(lvalue_of(p.op)), lv_handle_ptr(lvalue_of(qa.arg)),
                        elem_ops_global(qa.queue_type) });
    }

    // Queue ordering methods (LRM 7.12.2) — same shape as the dynarr variants
    // over the ring's logical order (op_type payload: handle lvalue + type).
    void lower_queue_reverse(Index i) {
        const auto&  p      = ir_->payload(i).op_type;
        llvm::Value* stride = b_.getInt64(dynarr_stride(dynarr_elem(p.type)));
        b_.CreateCall(queue_reverse_fn(), { lvalue_of(p.op).base, stride });
    }

    void lower_queue_sort(Index i, bool reversed) {
        const auto&    p      = ir_->payload(i).op_type;
        llvm::Value*   stride = b_.getInt64(dynarr_stride(dynarr_elem(p.type)));
        const SortDesc d      = dynarr_sort_desc(p.type);
        b_.CreateCall(queue_sort_fn(), { lvalue_of(p.op).base, stride, b_.getInt32(d.kind),
                                         b_.getInt32(d.width), b_.getInt32(reversed ? 1 : 0) });
    }

    void lower_queue_shuffle(Index i) {
        const auto&  p      = ir_->payload(i).op_type;
        llvm::Value* stride = b_.getInt64(dynarr_stride(dynarr_elem(p.type)));
        assert(rng_slot_ != nullptr && "shuffle needs the frame RNG");
        b_.CreateCall(queue_shuffle_fn(), { lvalue_of(p.op).base, stride, rng_slot_ });
    }

    void lower_store(const LValue& lv, Plane rhs) {
        // A real RHS stores as its bit pattern; reals have no bit-selects, so
        // a real store always takes a whole-storage path (narrow real slot:
        // the single iW word below).
        if (rhs.fp != nullptr) {
            rhs = real_to_planes(fp_of(rhs));
        }
        const uint32_t s_width = lv.store_width;
        llvm::Value*   mp      = lv_mm_ptr(lv);

        // Narrow 2-state slot: store the single iW value (the must plane of a
        // 4-state RHS drops any x/z to 0 per LRM 6.3.1; a 2-state RHS is the value
        // itself). No z word.
        if (lv.narrow) {
            llvm::Value* v = must_of(rhs);
            if (lv.width == s_width && is_zero_const(lv.bit_off)) {
                b_.CreateStore(v, mp);
                return;
            }
            llvm::Value* old  = b_.CreateLoad(int_ty(s_width), mp);
            llvm::Value* mask = store_mask(lv, s_width);
            b_.CreateStore(merge_sel(old, v, lv, s_width, mask), mp);
            return;
        }

        // The packed slot still holds i(2W); a 2-state RHS lifts to it.
        rhs = to_four(rhs);

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
        llvm::Value* mask = store_mask(lv, s_width);
        llvm::Value* must = merge_sel(must_of(old), must_of(rhs), lv, s_width, mask);
        llvm::Value* may  = merge_sel(may_of(old), may_of(rhs), lv, s_width, mask);
        llvm::Value* z    = merge_sel(old.z, rhs.z, lv, s_width, mask);
        b_.CreateStore(packed(must, may, z).mm, mp);
        if (lv.has_z) {
            b_.CreateStore(z, zp);
        }
    }

    // Stage a nonblocking assignment: write the sampled RHS into the target's shadow
    // (whole-signal direct, or merged-with-mask for a part-select target) and register
    // the enclosing block on the threaded commit chain (with this site's mask). Repeat
    // NBAs to one signal in a block accumulate in the shadow. No wait queues are
    // serviced here — that happens in the NBA region after the active drain.
    void lower_store_nb(const LValue& lv, Plane rhs) {
        const SignalSlot& s  = slot_of(lv.sig);

        // Aggregate static-path leaf: stage the whole leaf into its shadow slot
        // and register its leaf-set bit. The value is leaf-width (frontend
        // Resize), so no bit merge. (Runtime-path aggregate NBA is rejected in
        // nba_classify, so the path is static here.)
        if (lv.is_agg) {
            const int      k = agg_static_leaf_index(s, lv.agg_field_path);
            const AggLeaf& m = s.static_leaves[static_cast<uint32_t>(k)];
            if (m.narrow) {
                b_.CreateStore(must_of(rhs), shadow_ptr(s, m.sh_mm_field));
            } else {
                const Plane f = to_four(rhs);
                b_.CreateStore(f.mm, shadow_ptr(s, m.sh_mm_field));
                if (m.has_z) {
                    b_.CreateStore(f.z, shadow_ptr(s, m.sh_z_field));
                }
            }
            block_register(lv.sig, leaf_set_mask(s, static_cast<uint32_t>(k)), /*dynamic=*/false);
            return;
        }

        llvm::Value* mp = shadow_ptr(s, s.sh_mm_field);

        // Narrow 2-state target: stage the single iW value into the iW shadow. No z.
        if (s.narrow) {
            llvm::Value* v = must_of(rhs);
            if (!s.nba_partial) {
                b_.CreateStore(v, mp);
            } else {
                llvm::Value* old   = b_.CreateLoad(int_ty(s.width), mp);
                llvm::Value* bmask = store_mask(lv, s.width);
                b_.CreateStore(merge_sel(old, v, lv, s.width, bmask), mp);
                stage_dyn_mask(s, bmask);
            }
            register_block_store(lv, s);
            return;
        }

        rhs                  = to_four(rhs);
        // A never-z target stages no z shadow (and its commit loop skips z).
        llvm::Value* zp = s.has_z ? shadow_ptr(s, s.sh_z_field) : nullptr;

        if (!s.nba_partial) {
            // Whole-signal target: the RHS is already signal-width (frontend Resize),
            // so the shadow planes store directly — no mask, no read-modify-write.
            b_.CreateStore(rhs.mm, mp);
            if (s.has_z) {
                b_.CreateStore(rhs.z, zp);
            }
        } else {
            // Partial-capable target: merge the selected bits into the shadow. A
            // whole-signal site here selects every bit, so it merges the full word.
            const Plane  old   = { b_.CreateLoad(int_ty(2 * s.width), mp),
                                   s.has_z ? static_cast<llvm::Value*>(b_.CreateLoad(int_ty(s.width), zp))
                                           : zero(s.width) };
            llvm::Value* bmask = store_mask(lv, s.width);
            llvm::Value* must  = merge_sel(must_of(old), must_of(rhs), lv, s.width, bmask);
            llvm::Value* may   = merge_sel(may_of(old), may_of(rhs), lv, s.width, bmask);
            llvm::Value* z     = merge_sel(old.z, rhs.z, lv, s.width, bmask);
            b_.CreateStore(packed(must, may, z).mm, mp);
            if (s.has_z) {
                b_.CreateStore(z, zp);
            }
            stage_dyn_mask(s, bmask);
        }
        register_block_store(lv, s);
    }

    // For a dynamic-mask signal, OR this site's written bits into its runtime dirty
    // mask shadow field (the commit reads it). Static-only signals have no field and
    // use a compile-time mask instead.
    void stage_dyn_mask(const SignalSlot& s, llvm::Value* bmask) {
        if (!s.nba_dyn) {
            return;
        }
        llvm::Value* maskp = shadow_ptr(s, s.sh_mask_field);
        b_.CreateStore(b_.CreateOr(b_.CreateLoad(int_ty(s.width), maskp), bmask), maskp);
    }

    // Compile-time mask (signal-width APInt) of the bits this static site writes:
    // all-ones for a whole-signal site, else the constant [lsb, lsb+width) range.
    // Precondition: the site is static (constant bit_off), i.e. the signal is not
    // nba_dyn.
    llvm::APInt site_const_mask(const LValue& lv, const SignalSlot& s) const {
        if (lv.width == s.width && is_zero_const(lv.bit_off)) {
            return llvm::APInt::getAllOnes(s.width);
        }
        const uint64_t lsb = llvm::cast<llvm::ConstantInt>(lv.bit_off)->getZExtValue();
        return llvm::APInt::getBitsSet(s.width, lsb, lsb + lv.width);
    }

    // Index of the aggregate static leaf whose constant field path equals `fpath`
    // (the MemberSelect chain's field indices). Linear over the small static-leaf
    // list; -1 if not found (never, for a well-formed static-path NBA).
    int agg_static_leaf_index(const SignalSlot& s, const std::vector<uint32_t>& fpath) const {
        for (uint32_t k = 0; k < s.static_leaves.size(); ++k) {
            if (s.static_leaves[k].path == fpath) {
                return static_cast<int>(k);
            }
        }
        return -1;
    }

    // Leaf-set mask for an aggregate block registration: one bit per static leaf,
    // bit k set for the written leaf k. Unioned per block; the aggregate commit
    // copies the leaves whose bit is set. Width is the static-leaf count — an
    // aggregate signal's BlockSig mask lives in this space.
    llvm::APInt leaf_set_mask(const SignalSlot& s, uint32_t leaf) const {
        llvm::APInt m(static_cast<uint32_t>(s.static_leaves.size()), 0);
        m.setBit(leaf);
        return m;
    }

    // Register this NBA store's target on the current block, recording the commit
    // mask: a runtime dirty-mask field for a dynamic-select signal, else the union
    // of this site's compile-time mask.
    void register_block_store(const LValue& lv, const SignalSlot& s) {
        if (s.nba_dyn) {
            block_register(lv.sig, llvm::APInt(s.width, 0), /*dynamic=*/true);
        } else {
            block_register(lv.sig, site_const_mask(lv, s), /*dynamic=*/false);
        }
    }


    // Deferred-buffer index for an aggregate NBA signal: the aggregate buffers
    // follow the array buffers in the shared runtime index space (rt_nba_arr_*).
    uint32_t agg_buf_idx(Signal sig) const {
        const auto it = std::find(agg_nba_sigs_.begin(), agg_nba_sigs_.end(), sig);
        return static_cast<uint32_t>(array_nba_sigs_.size()) +
               static_cast<uint32_t>(it - agg_nba_sigs_.begin());
    }

    // Largest leaf slot byte size across an aggregate type (sizes the deferred
    // record's payload so any leaf's value fits).
    uint32_t agg_max_leaf_bytes(Ref type) {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::UnpackedArray) {
            return agg_max_leaf_bytes(t.element);
        }
        if (t.kind == Type::Kind::Struct && !t.is_packed) {
            uint32_t m = 0;
            for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
                m = std::max(m, agg_max_leaf_bytes(f.type));
            }
            return m;
        }
        if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue) {
            // A handle leaf is never a nonblocking target (frontend-guarded),
            // so it contributes nothing to the record payload size.
            return 0;
        }
        llvm::Type* slot = t.kind == Type::Kind::String ? static_cast<llvm::Type*>(str_slot_ty())
                                                        : agg_leaf_slot_ty(type);
        return static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(slot));
    }

    // Deferred aggregate-NBA record: { ptr dest_slot, i32 nbytes, [max x i8]
    // payload [, [max x i8] mask] }. `payload` holds the new leaf value in the
    // leaf's own slot byte layout. Without a mask the commit memcpies `nbytes` of
    // it to `dest_slot`; with a mask (any partial site in the aggregate) the
    // commit does a byte-wise masked read-modify-write. Either way a leaf of any
    // width / domain replays uniformly (no per-leaf monomorphization).
    llvm::StructType* agg_rec_ty(const SignalSlot& s) {
        const uint32_t maxb   = agg_max_leaf_bytes(s.ir_type);
        auto*          bytes  = llvm::ArrayType::get(b_.getInt8Ty(), maxb);
        std::vector<llvm::Type*> f{ raw_ptr_ty(), b_.getInt32Ty(), bytes };
        if (s.agg_masked) {
            f.push_back(bytes);
        }
        return llvm::StructType::get(ctx_, f);
    }

    // Pointer to the leaf SLOT an aggregate lvalue designates (its agg_path,
    // without the final mm/z field select).
    llvm::Value* agg_slot_ptr(const LValue& lv) {
        std::vector<llvm::Value*> idx;
        idx.reserve(lv.agg_path.size() + 1);
        idx.push_back(b_.getInt32(0));
        for (llvm::Value* step : lv.agg_path) {
            idx.push_back(step);
        }
        return b_.CreateInBoundsGEP(lv.root_ty, lv.base, idx);
    }

    // Stage a nonblocking write to an aggregate leaf reached through a runtime
    // array index: reserve a record and fill it with the destination slot pointer,
    // the leaf's slot byte count, and the new value written in the leaf's own slot
    // layout. The NBA region replays it by memcpy (later write to the same leaf
    // wins). Whole-leaf only — a partial leaf write is guarded in the frontend.
    void emit_agg_nba(const LValue& lv, Plane rhs) {
        const SignalSlot& s      = slot_of(lv.sig);
        const uint32_t    W      = lv.store_width;
        const llvm::Align a1{ 1 };
        auto*             i8     = b_.getInt8Ty();
        auto*             rt     = agg_rec_ty(s);
        const auto        stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
        llvm::Value* rec = b_.CreateCall(nba_arr_slot_fn(), { b_.getInt32(agg_buf_idx(lv.sig)), b_.getInt32(stride) });
        b_.CreateAlignedStore(agg_slot_ptr(lv), b_.CreateStructGEP(rt, rec, 0), a1);
        const auto nbytes = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(lv.slot_ty));
        b_.CreateAlignedStore(b_.getInt32(nbytes), b_.CreateStructGEP(rt, rec, 1), a1);

        // Byte offset of the leaf slot's z field (4-state leaves only).
        const uint32_t zoff = (!lv.narrow && lv.has_z)
                                ? static_cast<uint32_t>(env_->llmod.getDataLayout().getStructLayout(lv.slot_ty)->getElementOffset(1))
                                : 0;
        // Write a plane value into the payload at the slot's field offset.
        auto put = [&](llvm::Value* region, llvm::Value* v, uint32_t off) {
            llvm::Value* p = off == 0 ? region : b_.CreateInBoundsGEP(i8, region, { b_.getInt32(off) });
            b_.CreateAlignedStore(v, p, a1);
        };

        llvm::Value* payload = b_.CreateStructGEP(rt, rec, 2);
        // The value, positioned at the leaf's bit offset (partial select), in slot
        // layout: mm plane at offset 0, z plane at zoff.
        if (lv.narrow) {
            put(payload, place_val(lv, must_of(rhs), W), 0);
        } else {
            const Plane f  = to_four(rhs);
            llvm::Value* mm = packed(place_val(lv, must_of(f), W), place_val(lv, may_of(f), W),
                                     place_val(lv, f.z, W)).mm;
            put(payload, mm, 0);
            if (lv.has_z) {
                put(payload, place_val(lv, f.z, W), zoff);
            }
        }
        // The write mask, same layout: which bits this store touches (all-ones for
        // a whole-leaf write). Padding bytes of the mask stay garbage but only
        // ever gate the dest slot's own padding, which is never read.
        if (s.agg_masked) {
            llvm::Value* mask   = store_mask(lv, W);
            llvm::Value* mask_p = b_.CreateStructGEP(rt, rec, 3);
            if (lv.narrow) {
                put(mask_p, mask, 0);
            } else {
                put(mask_p, packed(mask, mask, zero(W)).mm, 0);
                if (lv.has_z) {
                    put(mask_p, mask, zoff);
                }
            }
        }
    }

    // Replay an aggregate's deferred records: memcpy each record's payload to its
    // destination slot (push order, later write wins), then wake level (Any-edge)
    // readers if any record was staged.
    void emit_agg_array_commit(Signal sig, llvm::Value* /*state*/) {
        const SignalSlot& s      = slot_of(sig);
        const uint32_t    idx    = agg_buf_idx(sig);
        auto*             rt     = agg_rec_ty(s);
        const auto        stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
        auto*             i8     = b_.getInt8Ty();
        llvm::Value*      base   = b_.CreateCall(nba_arr_base_fn(), { b_.getInt32(idx) });
        llvm::Value*      count  = b_.CreateCall(nba_arr_count_fn(), { b_.getInt32(idx) });

        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* head  = llvm::BasicBlock::Create(ctx_, "aggnba.head", fn);
        auto* body  = llvm::BasicBlock::Create(ctx_, "aggnba.body", fn);
        auto* done  = llvm::BasicBlock::Create(ctx_, "aggnba.done", fn);
        auto* entry = b_.GetInsertBlock();
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        auto* k = b_.CreatePHI(b_.getInt32Ty(), 2);
        k->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(k, count), body, done);
        b_.SetInsertPoint(body);
        llvm::Value* rec     = b_.CreateInBoundsGEP(i8, base, { b_.CreateMul(k, b_.getInt32(stride)) });
        llvm::Value* dest    = b_.CreateAlignedLoad(raw_ptr_ty(), b_.CreateStructGEP(rt, rec, 0), llvm::Align{ 1 });
        llvm::Value* nbytes  = b_.CreateAlignedLoad(b_.getInt32Ty(), b_.CreateStructGEP(rt, rec, 1), llvm::Align{ 1 });
        llvm::Value* payload = b_.CreateStructGEP(rt, rec, 2);
        if (!s.agg_masked) {
            b_.CreateMemCpy(dest, llvm::MaybeAlign{ 1 }, payload, llvm::MaybeAlign{ 1 }, nbytes);
        } else {
            // Byte-wise masked read-modify-write: dest[j] = (dest[j] & ~m) | (val[j] & m).
            // Width-agnostic — a mask byte of 0 leaves dest[j] unchanged, so an
            // unselected bit (or a garbage padding byte gating dest padding) is a
            // no-op. Mirrors the array masked commit without monomorphizing width.
            llvm::Value* mask_r = b_.CreateStructGEP(rt, rec, 3);
            auto*        bh     = llvm::BasicBlock::Create(ctx_, "aggnba.bh", fn);
            auto*        bb     = llvm::BasicBlock::Create(ctx_, "aggnba.bb", fn);
            auto*        bd     = llvm::BasicBlock::Create(ctx_, "aggnba.bd", fn);
            auto*        pre    = b_.GetInsertBlock();
            b_.CreateBr(bh);
            b_.SetInsertPoint(bh);
            auto* j = b_.CreatePHI(b_.getInt32Ty(), 2);
            j->addIncoming(b_.getInt32(0), pre);
            b_.CreateCondBr(b_.CreateICmpULT(j, nbytes), bb, bd);
            b_.SetInsertPoint(bb);
            llvm::Value* dj  = b_.CreateInBoundsGEP(i8, dest, { j });
            llvm::Value* m   = b_.CreateLoad(i8, b_.CreateInBoundsGEP(i8, mask_r, { j }));
            llvm::Value* v   = b_.CreateLoad(i8, b_.CreateInBoundsGEP(i8, payload, { j }));
            llvm::Value* old = b_.CreateLoad(i8, dj);
            b_.CreateStore(b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(m)), b_.CreateAnd(v, m)), dj);
            j->addIncoming(b_.CreateAdd(j, b_.getInt32(1)), b_.GetInsertBlock());
            b_.CreateBr(bh);
            b_.SetInsertPoint(bd);
        }
        k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);
        b_.SetInsertPoint(done);

        if (signal_has_waiters(sig)) {
            auto* svc  = llvm::BasicBlock::Create(ctx_, "aggnba.svc", fn);
            auto* cont = llvm::BasicBlock::Create(ctx_, "aggnba.cont", fn);
            b_.CreateCondBr(b_.CreateICmpNE(count, b_.getInt32(0)), svc, cont);
            b_.SetInsertPoint(svc);
            service_any_waiters(sig);
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
        }
    }

    // Stage a nonblocking write to an unpacked-array element: reserve a fixed-stride
    // record slot in this array's deferred buffer (rt_nba_arr_slot) and fill it with
    // the destination pointer(s), the positioned value, and (for a masked target) the
    // write mask. The NBA region replays it. Element width is unrestricted — the
    // value is a native iMM word of any size.
    void emit_array_nba(const LValue& lv, Plane rhs) {
        if (lv.dyn_inrange != nullptr) {
            // A variable-indexed element outside the array's declared bound
            // (LRM 7.4.5 / 11.5.1): the write is a no-op, so no record is
            // staged at all -- lv_mm_ptr(lv) below would otherwise compute an
            // out-of-bounds destination pointer that the NBA region later
            // dereferences unconditionally when replaying the record.
            auto* fn   = b_.GetInsertBlock()->getParent();
            auto* dobb = llvm::BasicBlock::Create(ctx_, "arrnba.do", fn);
            auto* cont = llvm::BasicBlock::Create(ctx_, "arrnba.cont", fn);
            b_.CreateCondBr(lv.dyn_inrange, dobb, cont);

            b_.SetInsertPoint(dobb);
            LValue clean      = lv;
            clean.dyn_inrange = nullptr;
            emit_array_nba(clean, rhs);
            b_.CreateBr(cont);

            b_.SetInsertPoint(cont);
            return;
        }
        const uint32_t    W = lv.store_width;
        const SignalSlot& s = slot_of(lv.sig);
        const llvm::Align a1{ 1 }; // records live in an unaligned byte buffer

        // A bounds-checked select restricts the mask to the in-range bits and aligns
        // the value poison-free (LRM 11.5.1); a whole/constant target's mask folds to
        // all-ones and is elided (unmasked signals carry no mask field).
        llvm::Value* mask_w = s.arr_masked ? store_mask(lv, W) : nullptr;
        llvm::Value* mm_val;
        if (s.narrow) {
            mm_val = place_val(lv, must_of(rhs), W); // iW
        } else {
            rhs               = to_four(rhs);
            llvm::Value* must = place_val(lv, must_of(rhs), W);
            llvm::Value* may  = place_val(lv, may_of(rhs), W);
            llvm::Value* zval = place_val(lv, rhs.z, W);
            mm_val            = packed(must, may, zval).mm; // i2W: must | (may << W)
        }

        llvm::StructType* rt     = arr_rec_ty(s);
        const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
        llvm::Value*      rec    = b_.CreateCall(nba_arr_slot_fn(), { b_.getInt32(arr_idx(lv.sig)), b_.getInt32(stride) });
        b_.CreateAlignedStore(lv_mm_ptr(lv), b_.CreateStructGEP(rt, rec, 0), a1);
        b_.CreateAlignedStore(mm_val, b_.CreateStructGEP(rt, rec, 1), a1);
        if (s.has_z) {
            // has_z implies a packed (non-narrow) element, so rhs is 4-state here.
            b_.CreateAlignedStore(lv_z_ptr(lv), b_.CreateStructGEP(rt, rec, arr_rec_z_ptr(s)), a1);
            b_.CreateAlignedStore(place_val(lv, rhs.z, W), b_.CreateStructGEP(rt, rec, arr_rec_z_val(s)), a1);
        }
        if (s.arr_masked) {
            b_.CreateAlignedStore(mask_w, b_.CreateStructGEP(rt, rec, arr_rec_mask(s)), a1);
        }
    }

    // Replay array `sig`'s deferred records in push order (later write to an element
    // wins): a monomorphized loop of typed masked/plain stores into the live element
    // slots, then wake its Any-edge readers if it took any write.
    // Arm a nonblocking intra-assignment delay (`x <= #d y`): reserve a stable
    // record from the target signal's pool, fill it with the sampled value planes
    // (monomorphized per width / z-ness), and schedule it on the wheel for
    // `now + ticks` with rt_intra_cb. The process does not suspend. The NBA region
    // of the fired instant commits the record (emit_intra_commit).
    void emit_intra_nba_arm(const LValue& lv, Plane rhs, llvm::Value* ticks) {
        const SignalSlot& s      = slot_of(lv.sig);
        const llvm::Align a1{ 1 }; // record lives in an unaligned pooled block
        llvm::StructType* rt     = intra_rec_ty(s);
        const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
        const uint32_t    idx    = intra_idx(lv.sig);

        llvm::Value* rec = b_.CreateCall(intra_alloc_fn(), { b_.getInt32(idx), b_.getInt32(stride) });
        b_.CreateStore(b_.getInt32(idx), b_.CreateStructGEP(rt, rec, 2)); // idx routes the fire

        // Value planes: a whole-scalar target is signal-width (frontend Resize), so
        // no positioning is needed — store the planes directly.
        llvm::Value* mm_val;
        if (s.narrow) {
            mm_val = must_of(rhs);
        } else {
            rhs    = to_four(rhs);
            mm_val = rhs.mm; // i2W: must | (may << W)
        }
        b_.CreateAlignedStore(mm_val, b_.CreateStructGEP(rt, rec, 5), a1);
        if (s.has_z) {
            b_.CreateAlignedStore(rhs.z, b_.CreateStructGEP(rt, rec, 6), a1);
        }

        // Arm the wheel for `now + ticks`. A zero delay cannot arm the wheel
        // (deadline must be > now) and its update lands in the current instant's
        // NBA region, so push the record straight onto the ready list (rt_intra_cb).
        // A compile-time-constant delay emits only the taken path; a runtime delay
        // that happens to be zero branches to the same immediate path.
        auto* nullp     = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        auto  schedule  = [&] {
            llvm::Value* now = b_.CreateCall(wheel_now_fn(), { wheel_ });
            b_.CreateCall(wheel_schedule_fn(),
                          { wheel_, rec, b_.CreateAdd(now, ticks), intra_cb_fn().getCallee() });
        };
        auto immediate = [&] { b_.CreateCall(intra_cb_fn(), { rec, nullp }); };

        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(ticks)) {
            if (c->isZero()) {
                immediate();
            } else {
                schedule();
            }
            return;
        }
        auto* zbb   = llvm::BasicBlock::Create(ctx_, "intra.zero", fn_);
        auto* nzbb  = llvm::BasicBlock::Create(ctx_, "intra.sched", fn_);
        auto* contb = llvm::BasicBlock::Create(ctx_, "intra.armed", fn_);
        b_.CreateCondBr(b_.CreateICmpEQ(ticks, b_.getInt64(0)), zbb, nzbb);
        b_.SetInsertPoint(zbb);
        immediate();
        b_.CreateBr(contb);
        b_.SetInsertPoint(nzbb);
        schedule();
        b_.CreateBr(contb);
        b_.SetInsertPoint(contb);
    }

    // Arm a nonblocking intra-assignment event (`x <= @e y` / `repeat(N) @e`):
    // reserve a record, fill the sampled value planes and the countdown, and
    // register it on trigger `trig_id`. The process does not suspend. When the
    // trigger edge fires (blocking store or NBA commit), rt_intra_event_fire counts
    // the record down and, at zero, moves it to the target ready list for the NBA
    // region (emit_intra_commit). Same value/plane layout as emit_intra_nba_arm.
    void emit_intra_event_arm(const LValue& lv, Plane rhs, llvm::Value* count, uint32_t trig_id) {
        const SignalSlot& s      = slot_of(lv.sig);
        const llvm::Align a1{ 1 };
        llvm::StructType* rt     = intra_rec_ty(s);
        const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
        const uint32_t    idx    = intra_idx(lv.sig);

        llvm::Value* rec = b_.CreateCall(intra_alloc_fn(), { b_.getInt32(idx), b_.getInt32(stride) });
        b_.CreateStore(b_.getInt32(idx), b_.CreateStructGEP(rt, rec, 2));
        b_.CreateStore(cast(count, 32, false), b_.CreateStructGEP(rt, rec, 4)); // event countdown

        llvm::Value* mm_val;
        if (s.narrow) {
            mm_val = must_of(rhs);
        } else {
            rhs    = to_four(rhs);
            mm_val = rhs.mm;
        }
        b_.CreateAlignedStore(mm_val, b_.CreateStructGEP(rt, rec, 5), a1);
        if (s.has_z) {
            b_.CreateAlignedStore(rhs.z, b_.CreateStructGEP(rt, rec, 6), a1);
        }
        b_.CreateCall(intra_event_register_fn(), { b_.getInt32(trig_id), rec });
    }

    // Commit intra-NBA signal `sig`'s fired records in the NBA region: store each
    // record's sampled planes to the live signal (later fire wins, per the arbitrary
    // order LRM allows among same-instant updates), then wake edge readers once on
    // the net old->new change, and return the records to the pool. Mirrors the
    // whole-signal block-commit body, looped over the ready list.
    void emit_intra_commit(Signal sig) {
        const SignalSlot& s    = slot_of(sig);
        const uint32_t    W    = s.width;
        auto*             i32  = b_.getInt32Ty();
        auto*             ptr  = raw_ptr_ty();
        auto*             mmty = int_ty(s.narrow ? W : 2 * W);
        auto*             zty  = int_ty(W);
        auto*             rt   = intra_rec_ty(s);
        const llvm::Align a1{ 1 };
        const uint32_t    idx  = intra_idx(sig);

        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* run   = llvm::BasicBlock::Create(ctx_, "intra.run", fn);
        auto* head  = llvm::BasicBlock::Create(ctx_, "intra.head", fn);
        auto* body  = llvm::BasicBlock::Create(ctx_, "intra.body", fn);
        auto* after = llvm::BasicBlock::Create(ctx_, "intra.after", fn);
        auto* skip  = llvm::BasicBlock::Create(ctx_, "intra.skip", fn);

        // No fired record for this signal -> nothing to commit / wake / release.
        llvm::Value* n = b_.CreateCall(intra_ready_count_fn(), { b_.getInt32(idx) });
        b_.CreateCondBr(b_.CreateICmpNE(n, b_.getInt32(0)), run, skip);

        b_.SetInsertPoint(run);
        llvm::Value* base    = b_.CreateCall(intra_ready_base_fn(), { b_.getInt32(idx) });
        llvm::Value* live    = storage_ptr(s);
        llvm::Value* live_mm = b_.CreateStructGEP(s.slot_ty, live, 0);
        llvm::Value* live_z  = s.has_z ? b_.CreateStructGEP(s.slot_ty, live, 1) : nullptr;
        llvm::Value* old_mm  = b_.CreateLoad(mmty, live_mm);
        llvm::Value* old_z   = s.has_z ? b_.CreateLoad(zty, live_z) : nullptr;
        auto*        pre     = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* i = b_.CreatePHI(i32, 2);
        i->addIncoming(b_.getInt32(0), pre);
        b_.CreateCondBr(b_.CreateICmpULT(i, n), body, after);

        b_.SetInsertPoint(body);
        llvm::Value* recp = b_.CreateLoad(ptr, b_.CreateInBoundsGEP(ptr, base, i));
        b_.CreateStore(b_.CreateAlignedLoad(mmty, b_.CreateStructGEP(rt, recp, 5), a1), live_mm);
        if (s.has_z) {
            b_.CreateStore(b_.CreateAlignedLoad(zty, b_.CreateStructGEP(rt, recp, 6), a1), live_z);
        }
        i->addIncoming(b_.CreateAdd(i, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);

        b_.SetInsertPoint(after);
        // Wake edge readers once, on the net change over all committed records.
        if (signal_has_waiters(sig)) {
            llvm::Value* new_mm = b_.CreateLoad(mmty, live_mm);
            llvm::Value* new_z  = s.has_z ? b_.CreateLoad(zty, live_z) : nullptr;
            llvm::Value* meta   = b_.CreateInBoundsGEP(nba_meta_arr_ty_, sym(nba_meta_gv_),
                                                       { b_.getInt32(0), b_.getInt32(s.nba_id) });
            emit_nba_service(sig, meta, old_mm, new_mm, W, s.narrow, old_z, new_z);
        }
        if (design_uses_vcd_ && nba_vcd_gv_ != nullptr) {
            emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
                return llvm::SmallVector<llvm::Value*, 2>{ b_.CreateLoad(
                  i32, b_.CreateInBoundsGEP(nba_vcd_gv_->getValueType(), sym(nba_vcd_gv_),
                                            { b_.getInt32(0), b_.getInt32(s.nba_id) })) };
            });
        }
        b_.CreateCall(intra_release_fn(), { b_.getInt32(idx) });
        b_.CreateBr(skip);

        b_.SetInsertPoint(skip);
    }

    void emit_array_commit(Signal sig, llvm::Value* state) {
        (void)state;
        const SignalSlot& s      = slot_of(sig);
        const uint32_t    W      = s.width;
        auto*             i32    = b_.getInt32Ty();
        auto*             ptr    = raw_ptr_ty();
        auto*             i8     = b_.getInt8Ty();
        auto*             mmty   = int_ty(s.narrow ? W : 2 * W);
        auto*             zty    = int_ty(W);
        auto*             rt     = arr_rec_ty(s);
        const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
        const llvm::Align a1{ 1 };
        // The live element pointer arrives opaque from the record, but the design
        // state is rt_alloc'd (64-byte aligned) and array elements sit at naturally
        // aligned offsets, so each element word meets its ABI alignment.
        const llvm::Align mm_al  = env_->llmod.getDataLayout().getABITypeAlign(mmty);
        const llvm::Align z_al   = env_->llmod.getDataLayout().getABITypeAlign(zty);

        llvm::Value* n    = b_.CreateCall(nba_arr_count_fn(), { b_.getInt32(arr_idx(sig)) });
        llvm::Value* base = b_.CreateCall(nba_arr_base_fn(), { b_.getInt32(arr_idx(sig)) });
        auto*        fn    = b_.GetInsertBlock()->getParent();
        auto*        head  = llvm::BasicBlock::Create(ctx_, "arr.head", fn);
        auto*        body  = llvm::BasicBlock::Create(ctx_, "arr.body", fn);
        auto*        done  = llvm::BasicBlock::Create(ctx_, "arr.done", fn);
        auto*        entry = b_.GetInsertBlock();
        b_.CreateBr(head);

        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, done);

        b_.SetInsertPoint(body);
        llvm::Value* recp   = b_.CreateInBoundsGEP(i8, base, b_.CreateMul(idx, b_.getInt32(stride)));
        llvm::Value* mm_ptr = b_.CreateLoad(ptr, b_.CreateStructGEP(rt, recp, 0));
        llvm::Value* mm_val = b_.CreateAlignedLoad(mmty, b_.CreateStructGEP(rt, recp, 1), a1);
        llvm::Value* mask   = nullptr;
        if (s.arr_masked) {
            mask = b_.CreateAlignedLoad(zty, b_.CreateStructGEP(rt, recp, arr_rec_mask(s)), a1);
            llvm::Value* mask2 = s.narrow ? mask
                                          : b_.CreateOr(b_.CreateZExt(mask, mmty),
                                                        b_.CreateShl(b_.CreateZExt(mask, mmty),
                                                                     llvm::ConstantInt::get(mmty, W)));
            llvm::Value* old   = b_.CreateAlignedLoad(mmty, mm_ptr, mm_al);
            b_.CreateAlignedStore(
              b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(mask2)), b_.CreateAnd(mm_val, mask2)), mm_ptr, mm_al);
        } else {
            b_.CreateAlignedStore(mm_val, mm_ptr, mm_al);
        }
        if (s.has_z) {
            llvm::Value* z_ptr = b_.CreateLoad(ptr, b_.CreateStructGEP(rt, recp, arr_rec_z_ptr(s)));
            llvm::Value* z_val = b_.CreateAlignedLoad(zty, b_.CreateStructGEP(rt, recp, arr_rec_z_val(s)), a1);
            if (s.arr_masked) {
                llvm::Value* old = b_.CreateAlignedLoad(zty, z_ptr, z_al);
                b_.CreateAlignedStore(
                  b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(mask)), b_.CreateAnd(z_val, mask)), z_ptr, z_al);
            } else {
                b_.CreateAlignedStore(z_val, z_ptr, z_al);
            }
        }
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);

        b_.SetInsertPoint(done);
        // Wake the array's level readers if it took any write this pass.
        if (signal_has_waiters(sig)) {
            auto* svc  = llvm::BasicBlock::Create(ctx_, "arr.svc", fn);
            auto* cont = llvm::BasicBlock::Create(ctx_, "arr.svc.cont", fn);
            b_.CreateCondBr(b_.CreateICmpNE(n, b_.getInt32(0)), svc, cont);
            b_.SetInsertPoint(svc);
            service_any_waiters(sig);
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
        }
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

    // (old & ~mask) | ((ins << off) & mask)
    llvm::Value* merge_bits(llvm::Value* old, llvm::Value* ins, llvm::Value* off, llvm::Value* mask,
                            llvm::Value* nmask) {
        llvm::Value* shifted = b_.CreateAnd(b_.CreateShl(ins, off), mask);
        return b_.CreateOr(b_.CreateAnd(old, nmask), shifted);
    }

    // -- Dynamic bit/part-select bounds (LRM 11.5.1) --
    //
    // A dynamic bit-select or indexed part-select can address bits outside the
    // vector: an out-of-range read yields x (0 for a 2-state slot) and an
    // out-of-range write is a no-op; a partially out-of-range select keeps the
    // in-range bits and applies the rule bit-by-bit to the rest. An x/z index
    // makes the whole select out of range. The raw run-time offset can be any
    // i32 (negative, or past the width), so all masking / shifting below is
    // poison-free: shift amounts are always clamped below the shifted type's
    // width. The LValue's `range_mask` (storage-width) marks the in-range bits.

    // Low `n` bits set: (1 << n) - 1 for n in [0, W]; poison-free (n >= W -> all
    // ones). `n` is an i32.
    llvm::Value* low_ones(uint32_t W, llvm::Value* n) {
        llvm::Value* ge   = b_.CreateICmpUGE(n, b_.getInt32(W));
        llvm::Value* safe = cast(b_.CreateSelect(ge, b_.getInt32(0), n), W, false);
        llvm::Value* one  = llvm::ConstantInt::get(int_ty(W), 1);
        llvm::Value* m    = b_.CreateSub(b_.CreateShl(one, safe), one);
        return b_.CreateSelect(ge, ones(W), m);
    }

    // W-bit mask with bits [lo, hi) set (lo <= hi, both i32 in [0, W]).
    llvm::Value* range_bits(uint32_t W, llvm::Value* lo, llvm::Value* hi) {
        return b_.CreateAnd(low_ones(W, hi), b_.CreateNot(low_ones(W, lo)));
    }

    // `v` shifted by the signed i32 `s`: left when s >= 0, right by |s| when
    // s < 0. Poison-free and saturating — |s| >= W yields 0 (every bit leaves the
    // W-bit word), the correct behavior when aligning an out-of-range selection.
    llvm::Value* align_shl(llvm::Value* v, llvm::Value* s, uint32_t W) {
        llvm::Value* neg  = b_.CreateICmpSLT(s, b_.getInt32(0));
        llvm::Value* amt  = b_.CreateSelect(neg, b_.CreateNeg(s), s);
        llvm::Value* ovf  = b_.CreateICmpUGE(amt, b_.getInt32(W));
        llvm::Value* samt = cast(b_.CreateSelect(ovf, b_.getInt32(0), amt), W, false);
        llvm::Value* pick = b_.CreateSelect(neg, b_.CreateLShr(v, samt), b_.CreateShl(v, samt));
        return b_.CreateSelect(ovf, zero(W), pick);
    }

    // Signed min / max on i64 (for clamping a select's range to its container).
    llvm::Value* smin(llvm::Value* a, llvm::Value* b) { return b_.CreateSelect(b_.CreateICmpSLT(a, b), a, b); }
    llvm::Value* smax(llvm::Value* a, llvm::Value* b) { return b_.CreateSelect(b_.CreateICmpSGT(a, b), a, b); }

    // Set `lv.range_mask` for a dynamic bit/part select, per LRM 11.5.1. The
    // select covers storage bits [lv.bit_off, lv.bit_off + selw); the in-range
    // bits are that range clamped to the container [cbase, cbase + cwidth) (the
    // whole slot for a direct select, the inner select's span for a nested one).
    // An x/z index (`idx_unknown`) makes every bit out of range.
    void set_select_bounds(LValue& lv, llvm::Value* cbase, uint32_t cwidth, uint32_t selw,
                           llvm::Value* idx_unknown) {
        const uint32_t sw    = lv.store_width;
        auto*          i64t  = int_ty(64);
        llvm::Value*   base  = b_.CreateSExt(cbase, i64t);
        llvm::Value*   end   = b_.CreateAdd(base, llvm::ConstantInt::get(i64t, cwidth));
        llvm::Value*   off   = b_.CreateSExt(lv.bit_off, i64t);
        llvm::Value*   lo    = smax(smin(off, end), base);
        llvm::Value*   hi    = smax(smin(b_.CreateAdd(off, llvm::ConstantInt::get(i64t, selw)), end), base);
        llvm::Value*   mask  = range_bits(sw, b_.CreateTrunc(lo, int_ty(32)), b_.CreateTrunc(hi, int_ty(32)));
        lv.range_mask        = b_.CreateSelect(idx_unknown, zero(sw), mask);
    }

    // Storage-width write mask for a partial store: the range-restricted mask for
    // a bounds-checked dynamic select, or the naive shifted mask otherwise.
    llvm::Value* store_mask(const LValue& lv, uint32_t sw) {
        if (lv.range_mask != nullptr) {
            return lv.range_mask;
        }
        return b_.CreateShl(cast(ones(lv.width), sw, false), cast(lv.bit_off, sw, false));
    }

    // Align a select's iW value to its storage bit position (poison-free for a
    // bounds-checked select, a plain left shift otherwise).
    llvm::Value* place_val(const LValue& lv, llvm::Value* v_iw, uint32_t sw) {
        llvm::Value* vW = cast(v_iw, sw, false);
        if (lv.range_mask != nullptr) {
            return align_shl(vW, lv.bit_off, sw);
        }
        return b_.CreateShl(vW, cast(lv.bit_off, sw, false));
    }

    // Merge one plane of a partial store: (old & ~mask) | (aligned ins & mask),
    // with the alignment poison-free and the mask range-restricted for a
    // bounds-checked dynamic select. `mask` is the shared store_mask(lv, sw).
    llvm::Value* merge_sel(llvm::Value* old, llvm::Value* ins_iw, const LValue& lv, uint32_t sw,
                           llvm::Value* mask) {
        llvm::Value* placed = b_.CreateAnd(place_val(lv, ins_iw, sw), mask);
        return b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(mask)), placed);
    }

    static bool is_zero_const(llvm::Value* v) {
        auto* c = llvm::dyn_cast<llvm::ConstantInt>(v);
        return c != nullptr && c->isZero();
    }

    // -- System tasks --

    // The runtime argument descriptor, mirroring `rt_arg` in runtime/Runtime.h:
    //   { i32 width, i32 flags, ptr val, ptr xz, ptr z }
    // Each plane is a pointer to a ceil(width/64)-word little-endian buffer
    // (built in build_rt_args), so operands of any width are carried in full.
    llvm::StructType* arg_struct_ty() {
        return llvm::StructType::get(
          ctx_, { b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
    }

    // void rt_fwrite(i32 desc, const char* fmt, i32 argc, const rt_arg* argv)
    llvm::FunctionCallee fwrite_fn() {
        auto* i32 = b_.getInt32Ty();
        return rt_fn("rt_fwrite", b_.getVoidTy(), { i32, raw_ptr_ty(), i32, raw_ptr_ty() });
    }

    // i32 rt_fopen_mcd(const char* name, i32 name_len)
    llvm::FunctionCallee fopen_mcd_fn() {
        return rt_fn("rt_fopen_mcd", b_.getInt32Ty(), { raw_ptr_ty(), b_.getInt32Ty() });
    }

    // i32 rt_fopen_fd(const char* name, i32 name_len, const char* mode, i32 mode_len)
    llvm::FunctionCallee fopen_fd_fn() {
        auto* i32 = b_.getInt32Ty();
        return rt_fn("rt_fopen_fd", i32, { raw_ptr_ty(), i32, raw_ptr_ty(), i32 });
    }

    // void rt_fclose(i32 desc)
    llvm::FunctionCallee fclose_fn() {
        return rt_fn("rt_fclose", b_.getVoidTy(), { b_.getInt32Ty() });
    }

    // void rt_timeformat_init(i64 tick_fs, i32 default_units)
    llvm::FunctionCallee timeformat_init_fn() {
        return rt_fn("rt_timeformat_init", b_.getVoidTy(), { b_.getInt64Ty(), b_.getInt32Ty() });
    }

    // void rt_timeformat_set(i32 units, i32 precision, const char* suffix, i32 suffix_len, i32 min_width)
    llvm::FunctionCallee timeformat_set_fn() {
        auto* i32 = b_.getInt32Ty();
        return rt_fn("rt_timeformat_set", b_.getVoidTy(), { i32, i32, raw_ptr_ty(), i32, i32 });
    }

    // Lower a $timeformat: evaluate the units / precision / min_width operands and
    // hand them, plus the literal suffix, to the runtime setter. The operands were
    // lowered earlier in this region, so their values are available.
    void lower_timeformat(Index i) {
        const auto   info  = ir_->extra_data<TimeFormatInfo>(ir_->payload(i).extra);
        llvm::Value* units = cast(must_of(value_of(info.units)), 32, true);
        llvm::Value* prec  = cast(must_of(value_of(info.precision)), 32, true);
        llvm::Value* width = cast(must_of(value_of(info.min_width)), 32, true);

        const std::string suffix  = ir_->extra_bytes(info.suffix, info.suffix_len);
        llvm::Value*      sfx_ptr = b_.CreateGlobalString(suffix);
        b_.CreateCall(timeformat_set_fn(),
                      { units, prec, sfx_ptr, b_.getInt32(static_cast<uint32_t>(suffix.size())), width });
    }

    // void rt_finish(ptr banner, i64 now, i32 arg) — noreturn; prints the exit
    // banner (LRM Table 20-1) then exits the process. Kept separate from the
    // plain rt_fn helpers: the noreturn attribute must land on the declaration so
    // the optimizer prunes the dead tail after a $finish.
    llvm::FunctionCallee finish_fn() {
        auto fn = rt_fn("rt_finish", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty() });
        if (auto* f = llvm::dyn_cast<llvm::Function>(fn.getCallee())) {
            f->setDoesNotReturn();
        }
        return fn;
    }

    // Lower a $finish / $stop: pass the runtime the compile-time banner prefix,
    // the current simulation time, and the diagnostic level, then it prints the
    // banner and exits. The call never returns, so the rest of the region is dead
    // but stays valid IR — rt_finish is noreturn and the optimizer prunes the
    // unreachable tail.
    //
    // TODO: a $finish / $stop inside a pure function reports simulation time 0.
    // Functions take (state) but no wheel argument (only processes and tasks do,
    // see the function ABI in declare_function / lower_task), so the current
    // time is unreachable here and the banner falls back to 0 — which reads wrong
    // if a design actually triggers it (e.g. `if (err) $finish;` in a function).
    // The location is still exact. $time has the same root cause but is guarded
    // loudly in the frontend (lower_sysfunc); the clean fix for both is to thread
    // the wheel into the function ABI, after which this fallback can go.
    void lower_finish(Index i) {
        const auto        info   = ir_->extra_data<FinishInfo>(ir_->payload(i).extra);
        const std::string banner = ir_->extra_bytes(info.banner, info.banner_len);
        llvm::Value*      now = has_wheel()
                                  ? static_cast<llvm::Value*>(b_.CreateCall(wheel_now_fn(), { wheel_ }))
                                  : static_cast<llvm::Value*>(b_.getInt64(0));
        b_.CreateCall(finish_fn(),
                      { b_.CreateGlobalString(banner), now, b_.getInt32(info.arg) });
    }

    // void rt_case_violation(ptr banner, i64 now, i32 kind) — warns and returns
    // (non-fatal). kind selects the message (0 = no match, 1 = overlap).
    llvm::FunctionCallee case_violation_fn() {
        return rt_fn("rt_case_violation", b_.getVoidTy(),
                     { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty() });
    }

    // Lower a unique/priority case violation report: pass the compile-time banner
    // prefix, the current simulation time, and the message kind; the runtime
    // warns and returns, so the region continues. The wheel fallback mirrors
    // lower_finish: a case inside a pure function has no wheel and reports
    // time 0 (same TODO as $finish).
    void lower_case_viol(Index i) {
        const auto        info   = ir_->extra_data<CaseViolInfo>(ir_->payload(i).extra);
        const std::string banner = ir_->extra_bytes(info.banner, info.banner_len);
        llvm::Value*      now = has_wheel()
                                  ? static_cast<llvm::Value*>(b_.CreateCall(wheel_now_fn(), { wheel_ }))
                                  : static_cast<llvm::Value*>(b_.getInt64(0));
        b_.CreateCall(case_violation_fn(),
                      { b_.CreateGlobalString(banner), now, b_.getInt32(info.kind) });
    }

    // void rt_severity(i32 level, ptr banner, ptr fmt, i32 argc, ptr argv) —
    // renders the message and writes the labelled diagnostic to stderr, then
    // returns (non-fatal).
    llvm::FunctionCallee severity_fn() {
        return rt_fn("rt_severity", b_.getVoidTy(),
                     { b_.getInt32Ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
    }

    // Lower a severity report ($info / $warning / $error / $fatal, or an
    // immediate-assertion default failure): build the message descriptor array
    // and call the runtime, passing the severity label, compile-time banner, and
    // format string. Needs no wheel (the report carries no simulation time), so
    // it lowers cleanly inside a pure function too.
    void lower_severity(Index i) {
        const auto  info = ir_->extra_data<SeverityInfo>(ir_->payload(i).extra);
        const std::string banner = ir_->extra_bytes(info.banner, info.banner_len);
        const std::string fmt    = ir_->extra_bytes(info.fmt, info.fmt_len);

        const std::span<const Index> args = ir_->region(info.args);
        llvm::Value*                 arr  = build_rt_args(args, "severity task");
        b_.CreateCall(severity_fn(), { b_.getInt32(static_cast<int32_t>(info.level)),
                                       b_.CreateGlobalString(banner), b_.CreateGlobalString(fmt),
                                       b_.getInt32(static_cast<uint32_t>(args.size())), arr });
    }

    // Whether wheel_ points at a live timing wheel: true in a process or task
    // body, false in a pure function (no wheel parameter).
    bool has_wheel() const {
        return cur_fn_ == nullptr || cur_is_task_;
    }

    // void rt_sim_run(ptr state, ptr wheel, ptr drain, ptr finals) — wraps the
    // drain in the $finish catch (a setjmp target) so a $finish returns here with
    // state live, then runs the final blocks (LRM 9.2.3). See runtime/Runtime.cpp.
    llvm::FunctionCallee sim_run_fn() {
        auto* ptr = raw_ptr_ty();
        return rt_fn("rt_sim_run", b_.getVoidTy(), { ptr, ptr, ptr, ptr });
    }

    // void rt_fflush(i32 desc) — flush a descriptor's output buffers (0 => all).
    llvm::FunctionCallee fflush_fn() {
        return rt_fn("rt_fflush", b_.getVoidTy(), { b_.getInt32Ty() });
    }

    // Lower a $fflush: flush the descriptor operand's buffers (a constant 0 for the
    // no-argument form, which the runtime reads as "flush everything").
    void lower_fflush(Index i) {
        llvm::Value* desc = cast(must_of(value_of(ir_->payload(i).unary)), 32, true);
        b_.CreateCall(fflush_fn(), { desc });
    }

    // Lower a two-argument $fopen: spill the filename and mode to stack byte
    // buffers and call the FD opener; the result is the descriptor.
    void lower_fopen_fd(Index i) {
        const auto& p                   = ir_->payload(i).binary;
        const auto [name_ptr, name_len] = spill_ascii(p.l, "fopen.name");
        const auto [mode_ptr, mode_len] = spill_ascii(p.r, "fopen.mode");
        llvm::Value* desc               = b_.CreateCall(
          fopen_fd_fn(), { name_ptr, b_.getInt32(name_len), mode_ptr, b_.getInt32(mode_len) });
        // The descriptor is a plain known integer (no x/z): must == may, z == 0.
        set_value(i, packed(desc, desc, zero(32)));
    }

    // Lower a one-argument $fopen: spill the filename and open a fresh MCD.
    void lower_fopen_mcd(Index i) {
        const auto [name_ptr, name_len] = spill_ascii(ir_->payload(i).unary, "fopen.name");
        llvm::Value* desc = b_.CreateCall(fopen_mcd_fn(), { name_ptr, b_.getInt32(name_len) });
        set_value(i, packed(desc, desc, zero(32)));
    }

    // Lower a $fclose: close the descriptor operand.
    void lower_fclose(Index i) {
        llvm::Value* desc = cast(must_of(value_of(ir_->payload(i).unary)), 32, true);
        b_.CreateCall(fclose_fn(), { desc });
    }

    // The file read / positioning system functions take i32 operands and
    // return i32; the results are always-known integers.
    // $fgetc / $feof / $ftell / $rewind: one descriptor operand.
    void lower_file_unary(Index i, const char* fn) {
        llvm::Value* fd   = cast(must_of(value_of(ir_->payload(i).unary)), 32, true);
        auto*        i32  = b_.getInt32Ty();
        auto*        fnty = llvm::FunctionType::get(i32, { i32 }, false);
        llvm::Value* r    = b_.CreateCall(env_->llmod.getOrInsertFunction(fn, fnty), { fd });
        set_value(i, packed(r, r, zero(32)));
    }

    // $ungetc(c, fd).
    void lower_ungetc(Index i) {
        const auto&  p    = ir_->payload(i).binary;
        llvm::Value* c    = cast(must_of(value_of(p.l)), 32, true);
        llvm::Value* fd   = cast(must_of(value_of(p.r)), 32, true);
        auto*        i32  = b_.getInt32Ty();
        auto*        fnty = llvm::FunctionType::get(i32, { i32, i32 }, false);
        llvm::Value* r = b_.CreateCall(env_->llmod.getOrInsertFunction("rt_ungetc", fnty), { c, fd });
        set_value(i, packed(r, r, zero(32)));
    }

    // $fseek(fd, offset, operation).
    void lower_fseek(Index i) {
        const auto&  p    = ir_->payload(i).op_extra;
        const auto   info = ir_->extra_data<FseekInfo>(p.extra);
        llvm::Value* fd   = cast(must_of(value_of(p.op)), 32, true);
        llvm::Value* off  = cast(must_of(value_of(info.offset)), 32, true);
        llvm::Value* op   = cast(must_of(value_of(info.operation)), 32, true);
        auto*        i32  = b_.getInt32Ty();
        auto*        fnty = llvm::FunctionType::get(i32, { i32, i32, i32 }, false);
        llvm::Value* r = b_.CreateCall(env_->llmod.getOrInsertFunction("rt_fseek", fnty), { fd, off, op });
        set_value(i, packed(r, r, zero(32)));
    }

    // i32 rt_fgets(i8* mm, i8* z, i32 width, i32 fd, i32 flags)
    llvm::FunctionCallee fgets_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* fnty = llvm::FunctionType::get(i32, { ptr, ptr, i32, i32, i32 }, false);
        return env_->llmod.getOrInsertFunction("rt_fgets", fnty);
    }

    // Lower a $fgets: hand the runtime the destination vector's plane pointers plus
    // its bit width and the descriptor. The runtime reads a line, stores it
    // right-justified into the must|may plane, clears the z plane, and returns the
    // character count.
    void lower_fgets(Index i) {
        const Index dst                  = ir_->payload(i).binary.l;
        const auto [mm, z, width, flags] = string_dest(dst, "$fgets");
        llvm::Value* fd                  = cast(must_of(value_of(ir_->payload(i).binary.r)), 32, true);
        const ServiceSnapshot old          = service_snapshot(lvalue_of(dst));
        llvm::Value* r = b_.CreateCall(fgets_fn(), { mm, z, b_.getInt32(width), fd, b_.getInt32(flags) });
        service_after_write(lvalue_of(dst), old);
        set_value(i, packed(r, r, zero(32)));
    }

    // Resolve a whole packed-vector lvalue to (must|may plane ptr, z plane ptr, bit
    // width, runtime flags) for a string write-back ($fgets / $sformat / $ferror).
    // Flags bit0 marks a narrow 2-state slot, so the runtime writes only the iW
    // value word. Part-select and non-byte-multiple targets loud-fail: the runtime
    // fills a whole byte-aligned vector. `who` names the caller.
    std::tuple<llvm::Value*, llvm::Value*, uint32_t, uint32_t> string_dest(Index dst, const char* who) {
        const LValue& lv = lvalue_of(dst);
        if (lv.is_string) {
            // TODO: file write-back into a dynamic string object (rt_string).
            throw BackendError{ std::string(who) + " into a string object is not supported" };
        }
        if (lv.width != lv.store_width) {
            // TODO: a part-select target (s[15:0]) writes only a slice.
            throw BackendError{ std::string(who) + " into a part-select target is not supported" };
        }
        if (lv.store_width % 8 != 0) {
            // TODO: a non-byte-multiple width leaves the must|may plane halves
            // byte-unaligned; targets are byte vectors in practice.
            throw BackendError{ std::string(who) +
                                " into a target whose width is not a multiple of 8 is not supported" };
        }
        // A never-z target (z elision) has no z word; pass null and the runtime
        // skips the z write.
        llvm::Value* mm = lv_mm_ptr(lv);
        llvm::Value* z =
          lv.has_z ? lv_z_ptr(lv) : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        return { mm, z, lv.store_width, lv.narrow ? 1u : 0u };
    }

    // void rt_sformat(i8* mm, i8* z, i32 width, const char* fmt, i32 argc, const rt_arg* argv, i32 flags)
    llvm::FunctionCallee sformat_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr, i32, ptr, i32, ptr, i32 }, false);
        return env_->llmod.getOrInsertFunction("rt_sformat", fnty);
    }

    // void rt_sformat_str(rt_string* dst, const char* fmt, i32 argc, const rt_arg* argv)
    llvm::FunctionCallee sformat_str_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr, b_.getInt32Ty(), ptr }, false);
        return env_->llmod.getOrInsertFunction("rt_sformat_str", fnty);
    }

    // Lower a $sformat / $swrite: build the format-arg descriptor array and hand
    // it, the format string, and the destination — a string object's handle, or
    // a packed vector's planes — to the runtime.
    void lower_sformat(Index i) {
        const auto        info = ir_->extra_data<SformatInfo>(ir_->payload(i).extra);
        const std::string fmt  = ir_->extra_bytes(info.fmt, info.fmt_len);

        const std::span<const Index> args = ir_->region(info.args);
        llvm::Value*                 arr  = build_rt_args(args, "$sformat / $swrite");
        llvm::Value*                 fmt_ptr = b_.CreateGlobalString(fmt);
        llvm::Value*                 argc    = b_.getInt32(static_cast<uint32_t>(args.size()));
        const LValue&                lv      = lvalue_of(info.dst);
        if (lv.is_string) {
            b_.CreateCall(sformat_str_fn(), { lv_string_ptr(lv), fmt_ptr, argc, arr });
            return;
        }
        const auto [mm, z, width, flags] = string_dest(info.dst, "$sformat / $swrite");
        const ServiceSnapshot old          = service_snapshot(lvalue_of(info.dst));
        b_.CreateCall(sformat_fn(), { mm, z, b_.getInt32(width), fmt_ptr, argc, arr, b_.getInt32(flags) });
        service_after_write(lvalue_of(info.dst), old);
    }

    // Lower a $sformatf: format into the instruction's string temp.
    void lower_sformatf(Index i) {
        const auto        info = ir_->extra_data<SformatfInfo>(ir_->payload(i).extra);
        const std::string fmt  = ir_->extra_bytes(info.fmt, info.fmt_len);

        const std::span<const Index> args = ir_->region(info.args);
        llvm::Value*                 arr  = build_rt_args(args, "$sformatf");
        b_.CreateCall(sformat_str_fn(),
                      { str_of(i), b_.CreateGlobalString(fmt),
                        b_.getInt32(static_cast<uint32_t>(args.size())), arr });
    }

    // i32 rt_ferror(i32 fd, i8* mm, i8* z, i32 width, i32 flags)
    llvm::FunctionCallee ferror_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* fnty = llvm::FunctionType::get(i32, { i32, ptr, ptr, i32, i32 }, false);
        return env_->llmod.getOrInsertFunction("rt_ferror", fnty);
    }

    // Lower a $ferror: pass the descriptor plus the destination string's planes; the
    // runtime writes the message and returns the error code.
    void lower_ferror(Index i) {
        const Index dst                  = ir_->payload(i).binary.l;
        const auto [mm, z, width, flags] = string_dest(dst, "$ferror");
        llvm::Value* fd                  = cast(must_of(value_of(ir_->payload(i).binary.r)), 32, true);
        const ServiceSnapshot old          = service_snapshot(lvalue_of(dst));
        llvm::Value* r = b_.CreateCall(ferror_fn(), { fd, mm, z, b_.getInt32(width), b_.getInt32(flags) });
        service_after_write(lvalue_of(dst), old);
        set_value(i, packed(r, r, zero(32)));
    }

    // { ptr mm, ptr z, i32 width, i32 flags } — mirrors rt_scan_arg in runtime/Runtime.h.
    llvm::StructType* scan_arg_struct_ty() {
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        return llvm::StructType::get(ctx_, { ptr, ptr, b_.getInt32Ty(), b_.getInt32Ty() });
    }

    // i32 rt_fscanf(i32 fd, const char* fmt, i32 argc, const rt_scan_arg* args)
    llvm::FunctionCallee fscanf_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* fnty = llvm::FunctionType::get(i32, { i32, ptr, i32, ptr }, false);
        return env_->llmod.getOrInsertFunction("rt_fscanf", fnty);
    }

    // i32 rt_sscanf(const char* buf, i32 len, const char* fmt, i32 argc, const rt_scan_arg* args)
    llvm::FunctionCallee sscanf_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* fnty = llvm::FunctionType::get(i32, { ptr, i32, ptr, i32, ptr }, false);
        return env_->llmod.getOrInsertFunction("rt_sscanf", fnty);
    }

    // Lower a $fscanf / $sscanf: build the rt_scan_arg[] descriptor array from the
    // output lvalues, then call the runtime scanner with the source (an FD, or a
    // spilled string for $sscanf). Each output target must be whole-storage and
    // <=64 bits (the runtime writes one 64-bit word per conversion).
    void lower_scanf(Index i) {
        const auto        info = ir_->extra_data<ScanfInfo>(ir_->payload(i).extra);
        const std::string fmt  = ir_->extra_bytes(info.fmt, info.fmt_len);

        const std::span<const Index> outs = ir_->region(info.args);
        const auto                   n    = static_cast<uint32_t>(outs.size());

        llvm::StructType*         sty = scan_arg_struct_ty();
        llvm::Value*              arr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        // Old-value snapshots for wait-queue servicing, parallel to `outs`
        // (null where a target needs no servicing).
        std::vector<ServiceSnapshot> olds(n);
        if (n > 0) {
            arr = b_.CreateAlloca(sty, b_.getInt32(n), "scan_args");
            for (uint32_t k = 0; k < n; ++k) {
                const LValue& lv = lvalue_of(outs[k]);
                if (lv.is_string) {
                    // TODO: $*scanf write-back into a dynamic string object.
                    throw BackendError{ "$fscanf / $sscanf into a string object is not supported" };
                }
                if (lv.width != lv.store_width) {
                    // TODO: a part-select scan target (x[7:0]).
                    throw BackendError{ "$fscanf / $sscanf into a part-select target is not supported" };
                }
                // A never-z target (z elision) passes a null z pointer. Flags bit0
                // marks a narrow 2-state slot: the runtime writes only the iW value
                // word (parsed x/z digits drop to 0 per LRM 6.3.1).
                llvm::Value* mm  = lv_mm_ptr(lv);
                llvm::Value* z   = lv.has_z ? lv_z_ptr(lv)
                                            : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
                llvm::Value* elt = b_.CreateConstInBoundsGEP1_32(sty, arr, k);
                b_.CreateStore(mm, b_.CreateStructGEP(sty, elt, 0));
                b_.CreateStore(z, b_.CreateStructGEP(sty, elt, 1));
                b_.CreateStore(b_.getInt32(lv.store_width), b_.CreateStructGEP(sty, elt, 2));
                b_.CreateStore(b_.getInt32(lv.narrow ? 1u : 0u), b_.CreateStructGEP(sty, elt, 3));
                olds[k] = service_snapshot(lv);
            }
        }

        llvm::Value* fmt_ptr = b_.CreateGlobalString(fmt);
        llvm::Value* r       = nullptr;
        if (ir_->tag(i) == Inst::Tag::Sscanf) {
            const auto [buf, len] = spill_ascii(info.source, "sscanf.buf");
            r = b_.CreateCall(sscanf_fn(), { buf, b_.getInt32(len), fmt_ptr, b_.getInt32(n), arr });
        } else {
            llvm::Value* fd = cast(must_of(value_of(info.source)), 32, true);
            r               = b_.CreateCall(fscanf_fn(), { fd, fmt_ptr, b_.getInt32(n), arr });
        }
        // Every conversion has landed; wake anything a written target changed
        // (an unconverted target reloads its old value and stays quiet).
        for (uint32_t k = 0; k < n; ++k) {
            service_after_write(lvalue_of(outs[k]), olds[k]);
        }
        set_value(i, packed(r, r, zero(32)));
    }

    // i32 rt_test_plusargs(const char* s, i32 len)
    llvm::FunctionCallee test_plusargs_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* fnty = llvm::FunctionType::get(i32, { ptr, i32 }, false);
        return env_->llmod.getOrInsertFunction("rt_test_plusargs", fnty);
    }

    // Lower $test$plusargs(str): the query string is a compile-time literal
    // (frontend-checked), so its bytes go straight to the runtime scan.
    void lower_test_plusargs(Index i) {
        const auto        info = ir_->extra_data<TestPlusArgsInfo>(ir_->payload(i).extra);
        const std::string str  = ir_->extra_bytes(info.str, info.str_len);
        llvm::Value*       str_ptr = b_.CreateGlobalString(str);
        llvm::Value*       r       = b_.CreateCall(
          test_plusargs_fn(), { str_ptr, b_.getInt32(static_cast<uint32_t>(str.size())) });
        set_value(i, packed(r, r, zero(32)));
    }

    // i32 rt_value_plusargs(const char* prefix, i32 prefix_len, i32 conv, const rt_scan_arg* arg)
    llvm::FunctionCallee value_plusargs_fn() {
        auto* ptr  = llvm::PointerType::getUnqual(ctx_);
        auto* i32  = b_.getInt32Ty();
        auto* fnty = llvm::FunctionType::get(i32, { ptr, i32, i32, ptr }, false);
        return env_->llmod.getOrInsertFunction("rt_value_plusargs", fnty);
    }

    // Lower $value$plusargs(user_string, var): build a single rt_scan_arg
    // descriptor for the output lvalue (as lower_scanf does per conversion),
    // then call the runtime with the compile-time-literal prefix + format
    // char. The target must be whole-storage and <=64 bits, like a scanf
    // target.
    void lower_value_plusargs(Index i) {
        const Index dst  = ir_->payload(i).op_extra.op;
        const auto  info = ir_->extra_data<ValuePlusArgsInfo>(ir_->payload(i).op_extra.extra);
        const std::string prefix = ir_->extra_bytes(info.prefix, info.prefix_len);

        const LValue& lv = lvalue_of(dst);
        if (lv.is_string) {
            // TODO: $value$plusargs into a dynamic string object.
            throw BackendError{ "$value$plusargs into a string object is not supported" };
        }
        if (lv.width != lv.store_width) {
            // TODO: a part-select $value$plusargs target (x[7:0]).
            throw BackendError{ "$value$plusargs into a part-select target is not supported" };
        }

        llvm::StructType* sty = scan_arg_struct_ty();
        llvm::Value*      arg = b_.CreateAlloca(sty, nullptr, "plusarg_val");
        llvm::Value*       mm = lv_mm_ptr(lv);
        llvm::Value*       z  = lv.has_z ? lv_z_ptr(lv)
                                         : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        b_.CreateStore(mm, b_.CreateStructGEP(sty, arg, 0));
        b_.CreateStore(z, b_.CreateStructGEP(sty, arg, 1));
        b_.CreateStore(b_.getInt32(lv.store_width), b_.CreateStructGEP(sty, arg, 2));
        b_.CreateStore(b_.getInt32(lv.narrow ? 1u : 0u), b_.CreateStructGEP(sty, arg, 3));

        const ServiceSnapshot old        = service_snapshot(lv);
        llvm::Value*          prefix_ptr = b_.CreateGlobalString(prefix);
        llvm::Value*           r          = b_.CreateCall(
          value_plusargs_fn(),
          { prefix_ptr, b_.getInt32(static_cast<uint32_t>(prefix.size())), b_.getInt32(info.conv), arg });
        service_after_write(lv, old);
        set_value(i, packed(r, r, zero(32)));
    }

    // Spill a packed-ASCII string value's must-bits to a stack byte buffer and
    // return (pointer, byte length). The value is a packed vector (a string literal
    // or a reg holding characters); the runtime reconstructs the string from these
    // bytes. Shared by $readmem (filename) and $fopen (name / mode).
    std::pair<llvm::Value*, uint32_t> spill_ascii(Index v, const char* name) {
        const uint32_t bytes = std::max<uint32_t>((type_of(v).width + 7) / 8, 1);
        llvm::Value*   buf   = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(bytes), name);
        b_.CreateAlignedStore(must_of(value_of(v)), buf, llvm::Align(1));
        return { buf, bytes };
    }

    // void rt_readmem(const char* path, i32 path_len, i8* mm_base, i8* z_base,
    //                 i32 stride, i32 count, i32 width, i64 decl_left, i64 decl_right,
    //                 i64 start, i64 finish, i32 flags)
    llvm::FunctionCallee readmem_fn() {
        auto* ptr = raw_ptr_ty();
        auto* i32 = b_.getInt32Ty();
        auto* i64 = b_.getInt64Ty();
        return rt_fn("rt_readmem", b_.getVoidTy(), { ptr, i32, ptr, ptr, i32, i32, i32, i64, i64, i64, i64, i32 });
    }

    // i32 rt_fread(i32 fd, i8* mm, i8* z, i32 stride, i32 count, i32 width, i32 four,
    //              i64 decl_left, i64 decl_right, i64 start, i64 max_count, i32 flags)
    llvm::FunctionCallee fread_fn() {
        auto* ptr = raw_ptr_ty();
        auto* i32 = b_.getInt32Ty();
        auto* i64 = b_.getInt64Ty();
        return rt_fn("rt_fread", i32, { i32, ptr, ptr, i32, i32, i32, i32, i64, i64, i64, i64, i32 });
    }

    // Lower a $fread into a single vector: one element, no addressing. The runtime
    // reads ceil(width/8) bytes MSB-first into the destination planes.
    void lower_fread_word(Index i) {
        const Index dst = ir_->payload(i).binary.l;

        const LValue& lv = lvalue_of(dst);
        if (lv.is_string) {
            throw BackendError{ "$fread into a string object is not supported" };
        }
        if (lv.width != lv.store_width) {
            throw BackendError{ "$fread into a part-select target is not supported" };
        }
        // A never-z target (z elision) passes a null z pointer; a narrow 2-state
        // slot sets flags bit2 so the runtime writes only the iW value word.
        llvm::Value* mm = lv_mm_ptr(lv);
        llvm::Value* z =
          lv.has_z ? lv_z_ptr(lv) : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        llvm::Value* fd = cast(must_of(value_of(ir_->payload(i).binary.r)), 32, true);

        const uint32_t four   = type_of(dst).four_state ? 1u : 0u;
        const uint32_t flags  = lv.narrow ? 4u : 0u;
        const ServiceSnapshot old = service_snapshot(lv);
        llvm::Value*   r      = b_.CreateCall(fread_fn(),
                                              { fd, mm, z, b_.getInt32(0), b_.getInt32(1), b_.getInt32(lv.store_width),
                                                b_.getInt32(four), b_.getInt64(0), b_.getInt64(0), b_.getInt64(0),
                                                b_.getInt64(0), b_.getInt32(flags) });
        service_after_write(lv, old);
        set_value(i, packed(r, r, zero(32)));
    }

    // Lower a $fread into a memory: reuse the $readmem geometry (plane bases,
    // stride, count, declared bounds) with optional start-address / element-count
    // operands, and read binary bytes MSB-first per element.
    void lower_fread_mem(Index i) {
        const Index fdi  = ir_->payload(i).op_extra.op;
        const auto  info = ir_->extra_data<FreadMemInfo>(ir_->payload(i).op_extra.extra);

        const SignalSlot& s = slot_of(info.mem);
        if (!s.is_array) {
            throw BackendError{ "$fread memory target is not a memory" };
        }
        // Element i's planes at base + i*stride (one slot struct per element); a
        // never-z memory stores no z word, so pass a null z_base.
        auto*        arr_ty  = llvm::cast<llvm::ArrayType>(s.storage_ty);
        llvm::Value* storage = storage_ptr(s);
        llvm::Value* mm_base =
          b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(0) });
        llvm::Value* z_base =
          s.has_z ? b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(1) })
                  : llvm::cast<llvm::Value>(llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)));
        const auto  stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(s.slot_ty));
        const Type& at     = std::get<Type>(mod_.pool.get(mod_.signal(info.mem).type));

        llvm::Value*   fd     = cast(must_of(value_of(fdi)), 32, true);
        const auto     i64_of = [&](Index idx) { return cast(must_of(value_of(idx)), 64, true); };
        llvm::Value*   start  = info.has_start ? i64_of(info.start) : b_.getInt64(0);
        llvm::Value*   cnt    = info.has_count ? i64_of(info.count) : b_.getInt64(0);
        const uint32_t flags  = (info.has_start ? 1u : 0u) | (info.has_count ? 2u : 0u) | (s.narrow ? 4u : 0u);

        llvm::Value* r = b_.CreateCall(
          fread_fn(), { fd, mm_base, z_base, b_.getInt32(stride), b_.getInt32(s.elem_count),
                        b_.getInt32(s.width), b_.getInt32(s.four_state ? 1u : 0u), b_.getInt64(at.left),
                        b_.getInt64(at.right), start, cnt, b_.getInt32(flags) });
        // A whole-array write has no per-element edge facts; wake the memory's
        // Any-edge readers when any byte landed, mirroring the deferred
        // array-NBA commit (rt_nba_arr_dirty -> service_any_waiters).
        if (!lowering_preinit_ && signal_has_waiters(info.mem)) {
            auto* fn   = b_.GetInsertBlock()->getParent();
            auto* svc  = llvm::BasicBlock::Create(ctx_, "fread.svc", fn);
            auto* cont = llvm::BasicBlock::Create(ctx_, "fread.cont", fn);
            b_.CreateCondBr(b_.CreateICmpNE(r, b_.getInt32(0)), svc, cont);
            b_.SetInsertPoint(svc);
            service_any_waiters(info.mem);
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
        }
        if (!lowering_preinit_) {
            vcd_touch_array(info.mem);   // bulk write: mark the array for the dump
        }
        set_value(i, packed(r, r, zero(32)));
    }

    // Lower a $readmemh / $readmemb: hand the runtime the filename bytes (spilled
    // to a stack buffer, or read from the rt_string handle for a string-typed
    // filename) plus the destination memory's plane bases (element 0) and geometry. The
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

        // A string-object filename (LRM §21.4 allows a string-typed expression) is
        // read straight from its rt_string handle: bytes in natural order, so the
        // runtime is told to skip the packed-vector byte reversal (flags bit 4).
        // Otherwise spill the filename's must-bits to a byte buffer: the value is
        // a packed-ASCII vector (string literal or reg), so its low bytes are the
        // path.
        const ValType fty          = type_of(fname);
        llvm::Value*  path         = nullptr;
        llvm::Value*  path_len     = nullptr;
        const bool    fname_is_str = fty.is_string;
        if (fname_is_str) {
            llvm::Value* handle = str_of(fname);
            path                = b_.CreateCall(str_data_fn(), { handle });
            path_len            = b_.CreateTrunc(b_.CreateCall(str_len_fn(), { handle }), b_.getInt32Ty());
        } else {
            const uint32_t fbytes = std::max<uint32_t>((fty.width + 7) / 8, 1);
            path                  = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(fbytes), "readmem.path");
            path_len              = b_.getInt32(fbytes);
            b_.CreateAlignedStore(must_of(value_of(fname)), path, llvm::Align(1));
        }

        // The runtime addresses element i's planes at base + i*stride, so hand it
        // element 0's typed field pointers and the slot struct's alloc size.
        auto*        arr_ty  = llvm::cast<llvm::ArrayType>(s.storage_ty);
        llvm::Value* storage = storage_ptr(s);
        llvm::Value* mm_base =
          b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(0) });
        // A never-z memory (always the case for a 2-state element type) stores no
        // z words; a null z_base tells the runtime to skip the z writes. A
        // 4-state readmem target is a z seed in compute_may_z, so it always keeps
        // its z field.
        llvm::Value* z_base =
          s.has_z ? b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(1) })
                  : llvm::cast<llvm::Value>(llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)));
        const auto stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(s.slot_ty));

        const Type& at = std::get<Type>(mod_.pool.get(mod_.signal(info.target).type));

        const auto   i64_of = [&](Index idx) { return cast(must_of(value_of(idx)), 64, true); };
        llvm::Value* start  = info.has_start ? i64_of(info.start) : b_.getInt64(0);
        llvm::Value* finish = info.has_finish ? i64_of(info.finish) : b_.getInt64(0);

        const uint32_t flags = (info.hex ? 1u : 0u) | (info.has_start ? 2u : 0u) |
                               (info.has_finish ? 4u : 0u) | (s.four_state ? 8u : 0u) |
                               (fname_is_str ? 16u : 0u) | (s.narrow ? 32u : 0u);

        b_.CreateCall(readmem_fn(),
                      { path, path_len, mm_base, z_base, b_.getInt32(stride),
                        b_.getInt32(s.elem_count), b_.getInt32(s.width), b_.getInt64(at.left),
                        b_.getInt64(at.right), start, finish, b_.getInt32(flags) });
        if (!lowering_preinit_) {
            vcd_touch_array(info.target);   // bulk write: mark the array for the dump
        }
        // TODO: $readmem services no wait queues — a process already waiting on
        // the memory sleeps through the load (pre-existing gap). rt_readmem would
        // need a did-write result (it returns void) to gate an Any-edge wake the
        // way lower_fread_mem does.
    }

    // -- Value-change dump (VCD, LRM §21.7; runtime in src/runtime/Runtime.cpp,
    //    design in docs/VCD.md) --

    llvm::FunctionCallee vcd_open_fn() {
        return rt_fn("rt_vcd_open", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty() });
    }
    // void rt_vcd_begin(rt_wheel*, i64 tick_fs, i32 units)
    llvm::FunctionCallee vcd_begin_fn() {
        return rt_fn("rt_vcd_begin", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee vcd_scope_fn() {
        return rt_fn("rt_vcd_scope", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() });
    }
    llvm::FunctionCallee vcd_upscope_fn() {
        return rt_fn("rt_vcd_upscope", b_.getVoidTy(), {});
    }
    // void rt_vcd_var(i8* name, i32 len, i32 width, i32 flags, i8* mm, i8* z)
    llvm::FunctionCallee vcd_var_fn() {
        auto* ptr = raw_ptr_ty();
        auto* i32 = b_.getInt32Ty();
        return rt_fn("rt_vcd_var", b_.getVoidTy(), { ptr, i32, i32, i32, ptr, ptr });
    }
    // void rt_vcd_array(i8* name, i32 len, i32 count, i32 width, i32 flags, i8* mm_base,
    //                   i8* z_base, i32 stride, i64 left, i64 right)
    llvm::FunctionCallee vcd_array_fn() {
        auto* ptr = raw_ptr_ty();
        auto* i32 = b_.getInt32Ty();
        auto* i64 = b_.getInt64Ty();
        return rt_fn("rt_vcd_array", b_.getVoidTy(), { ptr, i32, i32, i32, i32, ptr, ptr, i32, i64, i64 });
    }
    llvm::FunctionCallee vcd_start_fn() {
        return rt_fn("rt_vcd_start", b_.getVoidTy(), {});
    }
    llvm::FunctionCallee vcd_touch_fn() {
        return rt_fn("rt_vcd_touch", b_.getVoidTy(), { b_.getInt32Ty() });
    }
    // The runtime rt_vcd_active byte, non-zero only while VCD dumping is live.
    llvm::Constant* vcd_active_gv() {
        return env_->llmod.getOrInsertGlobal("rt_vcd_active", b_.getInt8Ty());
    }
    // Emit a VCD touch call guarded by the runtime active flag, so a design that
    // compiles in $dumpvars (possibly plusarg-guarded) but is not dumping pays
    // only a predictable-not-taken load+branch per store, not a runtime call.
    // `make_args` runs inside the taken branch, so index loads (the NBA vcd
    // table, a reused instance's vcd base) stay off the inactive hot path —
    // the point of the gate.
    void emit_guarded_vcd_touch(llvm::FunctionCallee                                  fn,
                                llvm::function_ref<llvm::SmallVector<llvm::Value*, 2>()> make_args) {
        llvm::Function*   f      = b_.GetInsertBlock()->getParent();
        llvm::Value*      active = b_.CreateLoad(b_.getInt8Ty(), vcd_active_gv());
        llvm::BasicBlock* touch  = llvm::BasicBlock::Create(ctx_, "vcd.touch", f);
        llvm::BasicBlock* cont   = llvm::BasicBlock::Create(ctx_, "vcd.cont", f);
        b_.CreateCondBr(b_.CreateICmpNE(active, b_.getInt8(0)), touch, cont);
        b_.SetInsertPoint(touch);
        b_.CreateCall(fn, make_args());
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
    llvm::FunctionCallee vcd_flush_fn() {
        return rt_fn("rt_vcd_flush", b_.getVoidTy(), { b_.getInt64Ty() });
    }
    llvm::FunctionCallee vcd_close_fn() {
        return rt_fn("rt_vcd_close", b_.getVoidTy(), {});
    }
    llvm::FunctionCallee vcd_nullary_fn(const char* name) {
        return rt_fn(name, b_.getVoidTy(), {});
    }

    // Any Dump* instruction anywhere in the design? Gates the per-instant flush and
    // the close at the end of the sim loop, so a design with no dump task pays
    // nothing.
    bool design_uses_vcd() const {
        for (const Process& p : mod_.processes()) {
            for (uint32_t k = 0; k < p.ir.inst_count(); ++k) {
                switch (p.ir.tag(static_cast<Index>(k))) {
                    case Inst::Tag::DumpFile:
                    case Inst::Tag::DumpVars:
                    case Inst::Tag::DumpOff:
                    case Inst::Tag::DumpOn:
                    case Inst::Tag::DumpAll:
                    case Inst::Tag::DumpFlush:
                        return true;
                    default:
                        break;
                }
            }
        }
        return false;
    }

    // Assign each dumped scalar signal its runtime dump-var index, in the exact
    // order rt_vcd_var / rt_vcd_array register variables (the emit_vcd_var walk):
    // an unpacked array consumes elem_count indices (its always-scanned elements),
    // a scalar consumes one. Precomputed before any process body so a store's touch
    // uses a compile-time-constant index. Kept in lockstep with emit_vcd_var /
    // emit_vcd_scope_tree; a byte-diff of the dump verifies the two match.
    void build_vcd_index() {
        design_uses_vcd_ = design_uses_vcd();
        if (!design_uses_vcd_) {
            return;
        }
        vcd_index_.assign(mod_.signals().size(), -1);
        uint32_t   next = 0;
        const auto tops = mod_.tops();
        if (tops.empty()) {
            for (uint32_t si = 0; si < mod_.signals().size(); ++si) {
                vcd_index_signal(static_cast<Signal>(si), next);
            }
        } else {
            for (const InstanceScope& top : tops) {
                vcd_index_scope(top, next);
            }
        }
    }

    void vcd_index_scope(const InstanceScope& sc, uint32_t& next) {
        const uint32_t aid =
          sc.id < anchor_of_scope_.size() ? anchor_of_scope_[sc.id] : SignalSlot::no_anchor;
        for (const Signal sig : sc.signals) {
            const uint32_t before = next;
            vcd_index_signal(sig, next);
            if (next != before && aid != SignalSlot::no_anchor && anchors_[aid].first_vcd < 0) {
                anchors_[aid].first_vcd = static_cast<int32_t>(before);
            }
        }
        for (const InstanceScope& child : sc.children) {
            vcd_index_scope(child, next);
        }
    }

    void vcd_index_signal(Signal sig, uint32_t& next) {
        const SignalInfo& info = mod_.signal(sig);
        if (info.name.empty()) {
            return;
        }
        const SignalSlot& s = slot_of(sig);
        if (s.is_string) {
            return;   // never registers (emit_vcd_var loud-fails on a string)
        }
        if (s.is_array) {
            // Record the first element's dump-var index; element k is base + k. An
            // element write touches base + normalized index (blocking) or the whole
            // range (NBA), instead of being scanned every instant.
            vcd_index_[static_cast<uint32_t>(sig)] = static_cast<int32_t>(next);
            next += s.elem_count;
            return;
        }
        vcd_index_[static_cast<uint32_t>(sig)] = static_cast<int32_t>(next++);
    }

    llvm::FunctionCallee vcd_touch_range_fn() {
        return rt_fn("rt_vcd_touch_range", b_.getVoidTy(), { b_.getInt32Ty(), b_.getInt32Ty() });
    }

    // The rt_vcd_touch index of a dumped signal: def-relative (instance vcd base
    // + def-local offset) when the owner is a reused instance, the folded
    // constant otherwise — dedup prep, same split as storage / sched anchors.
    llvm::Value* vcd_index_value(Signal sig, int32_t base) {
        const SignalSlot& s = slot_of(sig);
        if (s.anchor != SignalSlot::no_anchor && def_relative_for(s)) {
            Anchor& a = anchors_[s.anchor];
            if (a.indirect && a.first_vcd >= 0) {
                llvm::Value* b0 = ctx_field(a, 3, b_.getInt32Ty());
                return b_.CreateAdd(b0, b_.getInt32(static_cast<uint32_t>(base - a.first_vcd)));
            }
        }
        return b_.getInt32(static_cast<uint32_t>(base));
    }

    // Emit rt_vcd_touch for a blocking write to a dumped signal, marking it in the
    // dirty set. A scalar touches its index; an array element touches base + its
    // normalized element index (a whole-array store touches the range). A no-op for
    // a non-dumped signal or a PreInit store (before $dumpvars, subsumed by the
    // initial dump).
    void vcd_touch_lvalue(const LValue& lv) {
        if (!design_uses_vcd_ || lowering_preinit_ || !lv.is_signal) {
            return;
        }
        const int32_t base = vcd_index_[static_cast<uint32_t>(lv.sig)];
        if (base < 0) {
            return;
        }
        const SignalSlot& s = slot_of(lv.sig);
        if (s.is_array) {
            if (lv.elem_idx != nullptr) {
                emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
                    return llvm::SmallVector<llvm::Value*, 2>{
                        b_.CreateAdd(vcd_index_value(lv.sig, base), lv.elem_idx)
                    };
                });
            } else {
                emit_guarded_vcd_touch(vcd_touch_range_fn(), [&] {
                    return llvm::SmallVector<llvm::Value*, 2>{ vcd_index_value(lv.sig, base),
                                                               b_.getInt32(s.elem_count) };
                });
            }
            return;
        }
        emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
            return llvm::SmallVector<llvm::Value*, 2>{ vcd_index_value(lv.sig, base) };
        });
    }

    // Emit an rt_vcd_touch_range for a dumped array signal (whole element range).
    // Used at the array-NBA commit, where the runtime queue does not carry the
    // per-element index, and after $readmem / $fread bulk writes.
    void vcd_touch_array(Signal sig) {
        if (!design_uses_vcd_) {
            return;
        }
        const int32_t base = vcd_index_[static_cast<uint32_t>(sig)];
        if (base >= 0) {
            emit_guarded_vcd_touch(vcd_touch_range_fn(), [&] {
                return llvm::SmallVector<llvm::Value*, 2>{ vcd_index_value(sig, base),
                                                           b_.getInt32(slot_of(sig).elem_count) };
            });
        }
    }

    // $dumpfile: record the output path. A string-object filename is read straight
    // from its rt_string handle (natural byte order, forward=1); otherwise the
    // value is a packed-ASCII vector whose must-bits spill to a byte buffer
    // (forward=0), the same split as $readmem's filename.
    void lower_dump_file(Index i) {
        const Index   fname = ir_->payload(i).unary;
        const ValType fty   = type_of(fname);
        llvm::Value*  path  = nullptr;
        llvm::Value*  len   = nullptr;
        uint32_t      fwd   = 0;
        if (fty.is_string) {
            llvm::Value* handle = str_of(fname);
            path                = b_.CreateCall(str_data_fn(), { handle });
            len                 = b_.CreateTrunc(b_.CreateCall(str_len_fn(), { handle }), b_.getInt32Ty());
            fwd                 = 1;
        } else {
            const uint32_t fbytes = std::max<uint32_t>((fty.width + 7) / 8, 1);
            path                  = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(fbytes), "dumpfile.path");
            len                   = b_.getInt32(fbytes);
            b_.CreateAlignedStore(must_of(value_of(fname)), path, llvm::Align(1));
        }
        b_.CreateCall(vcd_open_fn(), { path, len, b_.getInt32(fwd) });
    }

    // $dumpvars (whole-design form): walk the design scope tree, registering every
    // dumpable signal with the runtime, then arm the dump.
    void lower_dump_vars() {
        b_.CreateCall(vcd_begin_fn(),
                      { wheel_, b_.getInt64(mod_.tick_fs), b_.getInt32(mod_.timeformat_units) });
        const auto tops = mod_.tops();
        if (tops.empty()) {
            emit_vcd_scope(mod_.name);
            for (uint32_t s = 0; s < mod_.signals().size(); ++s) {
                emit_vcd_var(static_cast<Signal>(s));
            }
            b_.CreateCall(vcd_upscope_fn(), {});
        } else {
            for (const InstanceScope& top : tops) {
                emit_vcd_scope_tree(top);
            }
        }
        b_.CreateCall(vcd_start_fn(), {});
    }

    void emit_vcd_scope(std::string_view name) {
        llvm::Value* np = b_.CreateGlobalString(llvm::StringRef(name.data(), name.size()), "vcd.scope");
        b_.CreateCall(vcd_scope_fn(), { np, b_.getInt32(static_cast<uint32_t>(name.size())) });
    }

    void emit_vcd_scope_tree(const InstanceScope& sc) {
        emit_vcd_scope(sc.name);
        for (const Signal sig : sc.signals) {
            emit_vcd_var(sig);
        }
        for (const InstanceScope& child : sc.children) {
            emit_vcd_scope_tree(child);
        }
        b_.CreateCall(vcd_upscope_fn(), {});
    }

    // Register one dumped signal: leaf name, width/flags, and live plane pointers.
    // An unpacked array expands to one $var per element (rt_vcd_array). A
    // whole-design dump must represent every declared signal, so an unrepresentable
    // one (string, element wider than 64 bits, multidimensional array) loud-fails
    // rather than being silently omitted; an unnamed compiler temporary
    // is not a declared variable and is skipped.
    void emit_vcd_var(Signal sig) {
        const SignalInfo& info = mod_.signal(sig);
        if (info.name.empty()) {
            return;
        }
        if (info.is_event) {
            // A named event (LRM 15.5) has no dumpable value; its 1-bit placeholder
            // slot is never written. A whole-design dump must represent every
            // declared signal, so loud-fail rather than emit a phantom wire.
            throw BackendError{ "VCD dump of event '" + info.name + "' is not supported" };
        }
        const SignalSlot& s = slot_of(sig);
        if (s.is_string) {
            throw BackendError{ "VCD dump of string signal '" + info.name + "' is not supported" };
        }
        std::string leaf = info.name;
        if (const auto pos = leaf.rfind('.'); pos != std::string::npos) {
            leaf = leaf.substr(pos + 1);
        }
        auto*          nullp = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        const uint32_t flags = (s.four_state ? 1u : 0u) | (info.is_net ? 2u : 0u) | (s.narrow ? 4u : 0u);
        llvm::Value*   np    = b_.CreateGlobalString(leaf, "vcd.var");
        llvm::Value*   nlen  = b_.getInt32(static_cast<uint32_t>(leaf.size()));

        if (!s.is_array) {
            llvm::Value* storage = storage_ptr(s);
            llvm::Value* mm      = b_.CreateStructGEP(s.slot_ty, storage, 0);
            llvm::Value* zp = s.has_z ? b_.CreateStructGEP(s.slot_ty, storage, 1) : llvm::cast<llvm::Value>(nullp);
            b_.CreateCall(vcd_var_fn(), { np, nlen, b_.getInt32(s.width), b_.getInt32(flags), mm, zp });
            return;
        }

        // Unpacked array (memory): one $var per element. Multidimensional arrays
        // (a nested UnpackedArray element type) need per-dimension index naming and
        // are deferred.
        const Type& at = std::get<Type>(mod_.pool.get(info.type));
        if (std::get<Type>(mod_.pool.get(at.element)).kind == Type::Kind::UnpackedArray) {
            throw BackendError{ "VCD dump of multidimensional array '" + info.name + "' is not supported" };
        }
        auto*        arr_ty  = llvm::cast<llvm::ArrayType>(s.storage_ty);
        llvm::Value* base    = storage_ptr(s);
        llvm::Value* mm_base = b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(0) });
        llvm::Value* z_base =
          s.has_z ? b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(1) })
                  : llvm::cast<llvm::Value>(nullp);
        const auto stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(s.slot_ty));
        b_.CreateCall(vcd_array_fn(), { np, nlen, b_.getInt32(s.elem_count), b_.getInt32(s.width),
                                        b_.getInt32(flags), mm_base, z_base, b_.getInt32(stride),
                                        b_.getInt64(at.left), b_.getInt64(at.right) });
    }

    // $dumpoff / $dumpon / $dumpall / $dumpflush: forward to the matching runtime
    // control entry.
    void lower_dump_control(Inst::Tag tag) {
        switch (tag) {
            case Inst::Tag::DumpOff:
                b_.CreateCall(vcd_nullary_fn("rt_vcd_off"), {});
                return;
            case Inst::Tag::DumpOn:
                b_.CreateCall(vcd_nullary_fn("rt_vcd_on"), {});
                return;
            case Inst::Tag::DumpAll:
                b_.CreateCall(vcd_nullary_fn("rt_vcd_all"), {});
                return;
            case Inst::Tag::DumpFlush:
                b_.CreateCall(vcd_nullary_fn("rt_vcd_dumpflush"), {});
                return;
            default:
                throw BackendError{ "internal: non-control dump tag" };
        }
    }

    // Lower a void `return` (no operand): a task branches to its copy-out +
    // caller-resume epilogue; a void function copies out formals, releases the
    // body's strings, and returns void.
    void lower_ret_void() {
        if (cur_is_task_) {
            b_.CreateBr(task_epilogue_bb_);
            return;
        }
        emit_copyout(*cur_fn_);
        deinit_body_strings();
        b_.CreateRetVoid();
    }

    // Lower a non-void function `return`: a real `ret`, so it terminates the
    // block and any instructions after it in the region are unreachable
    // (lower_region stops). The result is packed into the `{ i(2W), iW }` return
    // struct (string / aggregate / handle results copy out through the trailing
    // out-param instead).
    void lower_ret(Index value) {
        // A function copies output / inout formals back through their pointers at
        // each return point (LRM 13.5), before releasing the body and returning.
        emit_copyout(*cur_fn_);
        if (is_string_type(cur_fn_->return_type)) {
            // Copy the result out through the trailing out-param handle BEFORE
            // releasing the body's strings (the value may be one of them).
            b_.CreateCall(str_assign_fn(),
                          { fn_->getArg(fn_->arg_size() - 1), str_of(value) });
            deinit_body_strings();
            b_.CreateRetVoid();
            return;
        }
        if (contains_struct(cur_fn_->return_type)) {
            // Copy the aggregate result (the Ret operand is its lvalue) out through
            // the trailing storage-pointer out-param the caller owns.
            emit_agg_storage_copy(fn_->getArg(fn_->arg_size() - 1),
                                  agg_slot_ptr(lvalue_of(value)), cur_fn_->return_type);
            deinit_body_strings();
            b_.CreateRetVoid();
            return;
        }
        if (is_handle_type(cur_fn_->return_type)) {
            // Deep-copy the returned handle (an lvalue) into the caller's result
            // handle (the trailing out-param), before freeing the body's handles
            // (the value may be a frame local or a call result temp).
            emit_handle_assign(fn_->getArg(fn_->arg_size() - 1), lvalue_of(value).base,
                               cur_fn_->return_type);
            deinit_body_strings();
            b_.CreateRetVoid();
            return;
        }
        // The return ABI is the packed (must|may, z) pair; lift a 2-state result
        // (a real result returns as its bit-pattern planes).
        Plane pl = value_of(value);
        pl       = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
        auto* rty = llvm::cast<llvm::StructType>(fn_->getReturnType());
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
        if (f.is_dpi) {
            lower_dpi_call(i, ci, f);
            return;
        }
        llvm::Function*           cf = sym(fn_decls_[static_cast<uint32_t>(ci.func)]);
        std::vector<llvm::Value*> argv;
        argv.push_back(state_);
        // A ref actual that is a signal with waiters: the callee writes it live, so
        // its wait queue must be serviced with the call's net old -> new transition.
        // Snapshot each before the (atomic, zero-time) function call; service after.
        std::vector<std::pair<LValue, ServiceSnapshot>> ref_svc;
        size_t k = 0;
        for (Index a : ir_->region(ci.args)) {
            const ArgDir dir = f.arg_dirs.empty() ? ArgDir::In : f.arg_dirs[k];
            if (is_handle_type(f.param_types[k]) && dir != ArgDir::Ref) {
                // A dynamic array / queue passes one handle pointer (the actual) in
                // every by-value direction; the callee copies in / out through it.
                // (A ref handle falls to the Ref branch below, aliasing the pointer.)
                argv.push_back(lvalue_of(a).base);
            } else if (dir == ArgDir::Ref) {
                // Pass one pointer to the actual's slot (scalar, array element, or a
                // dynamic-array / queue handle); the callee aliases it.
                const LValue& lv = lvalue_of(a);
                argv.push_back(is_handle_type(f.param_types[k]) ? lv.base : ref_actual_ptr(lv));
                if (!is_handle_type(f.param_types[k])) {
                    ref_svc.emplace_back(lv, service_snapshot(lv));
                }
            } else if (dir == ArgDir::Out || dir == ArgDir::Inout) {
                // An output / inout temp: pass pointers to its mm and z planes; the
                // callee copies out through them (the caller copies the temp back to
                // the actual after the call — frontend emit_call_copyouts).
                const LValue& lv = lvalue_of(a);
                argv.push_back(lv_mm_ptr(lv));
                argv.push_back(lv_z_ptr(lv));
            } else if (contains_struct(f.param_types[k])) {
                // An aggregate actual passes its storage pointer (the frontend
                // emitted it as an lvalue); the callee copies it in.
                argv.push_back(agg_slot_ptr(lvalue_of(a)));
            } else if (is_string_type(f.param_types[k])) {
                argv.push_back(str_of(a));
            } else {
                Plane pl = value_of(a);
                pl       = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
                argv.push_back(pl.mm);
                argv.push_back(pl.z);
            }
            ++k;
        }
        if (contains_struct(f.return_type)) {
            // Safe-temp aggregate return: the callee memcpies its result into a
            // caller stack temp we pass as the trailing out-param; the call result
            // is that temp lvalue (the frontend copies it into the target).
            auto*        st  = agg_storage_ty(f.return_type);
            llvm::Value* tmp = b_.CreateAlloca(st, nullptr, "call.agg.ret");
            argv.push_back(tmp);
            b_.CreateCall(cf->getFunctionType(), cf, argv);
            LValue lv{};
            lv.base    = tmp;
            lv.root_ty = st;
            lv.is_agg  = true;
            set_lvalue(i, lv);
            return;
        }
        if (is_string_type(f.return_type)) {
            // The callee assigns the result into this call's temp handle (the
            // trailing out-param); str_of(i) was pre-set by build_str_temps.
            argv.push_back(str_of(i));
            b_.CreateCall(cf->getFunctionType(), cf, argv);
            for (auto& [lv, snap] : ref_svc) {
                service_after_write(lv, snap);
            }
            return;
        }
        if (is_handle_type(f.return_type)) {
            // The callee copies its return handle into this call's result handle
            // (the trailing out-param, alloc'd by build_dynarr_temps). Publish it as
            // the call's lvalue so downstream consumers (assign / .size() / element)
            // read it directly.
            llvm::Value* temp = dynarr_ret_[static_cast<uint32_t>(i)];
            LValue       lv;
            lv.base    = temp;
            lv.slot_ty = handle_slot_ty(f.return_type);
            set_lvalue(i, lv);
            argv.push_back(temp);
            b_.CreateCall(cf->getFunctionType(), cf, argv);
            for (auto& [lv2, snap] : ref_svc) {
                service_after_write(lv2, snap);
            }
            return;
        }
        llvm::Value* r = b_.CreateCall(cf->getFunctionType(), cf, argv);
        for (auto& [lv, snap] : ref_svc) {
            service_after_write(lv, snap);
        }
        if (f.return_type != mod_.pool.void_type) {
            Plane         pl{ b_.CreateExtractValue(r, { 0 }), b_.CreateExtractValue(r, { 1 }) };
            const ValType t = from_ref(f.return_type);
            if (t.is_real) {
                pl = real_plane(planes_to_real(pl, t.width));
            }
            set_value(i, pl);
        }
    }

    // 32-bit chunk count of a width-W packed vector (the svBitVecVal / svLogicVecVal
    // element count).
    static uint32_t dpi_nchunks(uint32_t w) {
        return (w + 31) / 32;
    }

    llvm::FunctionCallee dpi_pack_logicvec_fn() {
        auto* p = raw_ptr_ty();
        return rt_fn("rt_dpi_pack_logicvec", b_.getVoidTy(), { p, p, p, b_.getInt32Ty(), p });
    }
    llvm::FunctionCallee dpi_unpack_logicvec_fn() {
        auto* p = raw_ptr_ty();
        return rt_fn("rt_dpi_unpack_logicvec", b_.getVoidTy(), { p, b_.getInt32Ty(), p, p, p });
    }
    // void rt_dpi_str_out(rt_string* dst, const char* p) — copy a returned C string.
    llvm::FunctionCallee dpi_str_out_fn() {
        auto* p = raw_ptr_ty();
        return rt_fn("rt_dpi_str_out", b_.getVoidTy(), { p, p });
    }

    // Spill an iW plane into a fresh entry-block buffer of `nchunks` 32-bit words
    // (zero-extended above W), returned as a pointer the runtime reads as uint32*.
    llvm::Value* dpi_spill_words(llvm::Value* plane, uint32_t nchunks) {
        auto* bufty = int_ty(nchunks * 32);
        auto* buf   = entry_alloca(bufty, "dpi.words");
        b_.CreateStore(b_.CreateZExtOrTrunc(plane, bufty), buf);
        return buf;
    }
    // Load an iW value back from an `nchunks`-word buffer.
    llvm::Value* dpi_load_words(llvm::Value* buf, uint32_t nchunks, uint32_t w) {
        return b_.CreateTrunc(b_.CreateLoad(int_ty(nchunks * 32), buf), int_ty(w));
    }

    // A fresh svVecVal scratch: nchunks 32-bit words for a bit vector, 2*nchunks
    // (interleaved {aval,bval}) for a logic vector.
    llvm::Value* dpi_vec_alloca(DpiCType ct, uint32_t nchunks) {
        return entry_alloca(int_ty((ct == DpiCType::BitVec ? 1u : 2u) * nchunks * 32), "dpi.vec");
    }

    // Fill an svVecVal scratch from a value Plane (input build / inout copy-in).
    void dpi_vec_copyin(DpiCType ct, Plane pl, uint32_t w, llvm::Value* scratch) {
        const uint32_t nc = dpi_nchunks(w);
        if (ct == DpiCType::BitVec) {
            // svBitVecVal: the (2-state) value plane chunked is already the layout.
            b_.CreateStore(b_.CreateZExtOrTrunc(must_of(pl), int_ty(nc * 32)), scratch);
            return;
        }
        // svLogicVecVal: spill must/may/z; the runtime computes aval/bval + interleave.
        llvm::Value* zv = (pl.two || pl.z == nullptr) ? zero(w) : pl.z;
        b_.CreateCall(dpi_pack_logicvec_fn(), { dpi_spill_words(must_of(pl), nc),
                                                dpi_spill_words(may_of(pl), nc),
                                                dpi_spill_words(zv, nc), b_.getInt32(nc), scratch });
    }

    // Reconstruct a value Plane from an svVecVal scratch (output / inout copy-out).
    Plane dpi_vec_copyout(DpiCType ct, llvm::Value* scratch, uint32_t w) {
        const uint32_t nc = dpi_nchunks(w);
        if (ct == DpiCType::BitVec) {
            return two_val(dpi_load_words(scratch, nc, w));
        }
        llvm::Value* must = entry_alloca(int_ty(nc * 32), "dpi.must");
        llvm::Value* may  = entry_alloca(int_ty(nc * 32), "dpi.may");
        llvm::Value* zv   = entry_alloca(int_ty(nc * 32), "dpi.z");
        b_.CreateCall(dpi_unpack_logicvec_fn(), { scratch, b_.getInt32(nc), must, may, zv });
        return packed(dpi_load_words(must, nc, w), dpi_load_words(may, nc, w),
                      dpi_load_words(zv, nc, w));
    }

    // The LLVM struct mirroring the runtime rt_svopen descriptor (Runtime.h):
    // { ptr data, i32 left, i32 right, i32 nelem, i32 elem_bytes, i32 elem_width,
    // i32 is_logic }. Memoized; must match the C layout exactly.
    llvm::StructType* rt_svopen_ty() {
        if (rt_svopen_ty_ == nullptr) {
            auto* i32 = b_.getInt32Ty();
            rt_svopen_ty_ = llvm::StructType::create(
              ctx_, { raw_ptr_ty(), i32, i32, i32, i32, i32, i32 }, "clotho.svopen");
        }
        return rt_svopen_ty_;
    }
    llvm::StructType* rt_svopen_ty_ = nullptr;

    // A marshalled open-array output/inout argument to unpack back after the call
    // (inline IR, emit_dpi_oa_unpack_ir). Only recorded for a marshalled (packed)
    // element in a non-input direction — a named-scalar zero-copy element has the C
    // writing the design storage directly.
    struct OaMarshalBack {
        bool              is_logic;
        llvm::Value*      buf;     // svVecVal scratch the C wrote
        llvm::Value*      storage; // `[N x slot]` design storage
        llvm::StructType* slot;    // element slot type
        uint32_t          ew;
        uint32_t          n;
        uint32_t          nch;
        uint32_t          has_z;
    };

    // Element GEP into a marshalled svVecVal buffer: `[N*nch x elem]` where elem is
    // `{ i32, i32 }` (svLogicVecVal) or `i32` (svBitVecVal). `flat` is the element's
    // flat position k*nch + c.
    llvm::Value* dpi_oa_buf_elem(llvm::Value* buf, uint32_t total, bool is_logic, llvm::Value* flat) {
        auto* i32   = b_.getInt32Ty();
        auto* elem  = is_logic ? static_cast<llvm::Type*>(llvm::StructType::get(ctx_, { i32, i32 }))
                               : static_cast<llvm::Type*>(i32);
        auto* arrty = llvm::ArrayType::get(elem, total);
        return b_.CreateInBoundsGEP(arrty, buf, { b_.getInt32(0), flat });
    }

    // Inline per-element pack (design `[N x slot]` storage -> svVecVal buffer) for a
    // marshalled open-array element, any width. N is a compile-time constant (fixed
    // unpacked-array actual), so this is a constant-trip loop; each element loads the
    // native iW / i2W slot fields and LLVM legalizes the ops at any width — the
    // unaligned `may` region (mm bits [W, 2W)) is a single `lshr i2W, W`, no
    // hand-rolled funnel shift, and no separate C marshal to keep in sync.
    void emit_dpi_oa_pack_ir(llvm::Value* storage, llvm::Value* buf, llvm::StructType* slot,
                             uint32_t n, uint32_t ew, uint32_t nch, bool has_z, bool is_logic) {
        auto* iW    = int_ty(ew);
        auto* i32   = b_.getInt32Ty();
        auto* arrty = llvm::ArrayType::get(slot, n);
        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* head  = llvm::BasicBlock::Create(ctx_, "oapk.head", fn);
        auto* body  = llvm::BasicBlock::Create(ctx_, "oapk.body", fn);
        auto* done  = llvm::BasicBlock::Create(ctx_, "oapk.done", fn);
        auto* entry = b_.GetInsertBlock();
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        auto* k = b_.CreatePHI(i32, 2);
        k->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(k, b_.getInt32(n)), body, done);
        b_.SetInsertPoint(body);

        auto*        slotptr = b_.CreateInBoundsGEP(arrty, storage, { b_.getInt32(0), k });
        llvm::Value* aval    = nullptr; // iW: value-or-X
        llvm::Value* bval    = nullptr; // iW: X-or-Z (null for 2-state)
        if (is_logic) {
            auto* i2w  = int_ty(2 * ew);
            auto*        mm = b_.CreateLoad(i2w, b_.CreateStructGEP(slot, slotptr, 0));
            llvm::Value* z  = has_z ? static_cast<llvm::Value*>(
                                        b_.CreateLoad(iW, b_.CreateStructGEP(slot, slotptr, 1)))
                                    : static_cast<llvm::Value*>(llvm::ConstantInt::get(iW, 0));
            auto* must = b_.CreateTrunc(mm, iW);
            auto* may  = b_.CreateTrunc(b_.CreateLShr(mm, llvm::ConstantInt::get(i2w, ew)), iW);
            aval       = b_.CreateAnd(may, b_.CreateNot(z));
            bval       = b_.CreateXor(must, may);
        } else {
            // 2-state: the value is the low W bits of field 0 (narrow iW, or the must
            // region of a non-narrow i2W) — mirrors the C pack_bit low-W read.
            auto* raw = b_.CreateLoad(slot->getElementType(0), b_.CreateStructGEP(slot, slotptr, 0));
            aval      = b_.CreateZExtOrTrunc(raw, iW);
        }
        auto* base = b_.CreateMul(k, b_.getInt32(nch));
        for (uint32_t c = 0; c < nch; ++c) {
            auto* flat  = b_.CreateAdd(base, b_.getInt32(c));
            auto* eptr  = dpi_oa_buf_elem(buf, n * nch, is_logic, flat);
            auto* shamt = llvm::ConstantInt::get(iW, 32u * c); // 32c < ew, never poison
            // ZExtOrTrunc, not Trunc: iW may be narrower than i32 (w < 32).
            auto* ac    = b_.CreateZExtOrTrunc(b_.CreateLShr(aval, shamt), i32);
            if (is_logic) {
                auto* i32x2 = llvm::StructType::get(ctx_, { i32, i32 });
                auto* bc    = b_.CreateZExtOrTrunc(b_.CreateLShr(bval, shamt), i32);
                b_.CreateStore(ac, b_.CreateStructGEP(i32x2, eptr, 0));
                b_.CreateStore(bc, b_.CreateStructGEP(i32x2, eptr, 1));
            } else {
                b_.CreateStore(ac, eptr);
            }
        }
        k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
    }

    // Inline per-element unpack (svVecVal buffer -> design storage) — the reverse of
    // emit_dpi_oa_pack_ir, for an output/inout element of any width. Reassembles
    // aval/bval from the chunks, reconstructs mm = must | (may << W) with a single
    // `shl i2W`, and writes the z field when present.
    void emit_dpi_oa_unpack_ir(llvm::Value* buf, llvm::Value* storage, llvm::StructType* slot,
                               uint32_t n, uint32_t ew, uint32_t nch, bool has_z, bool is_logic) {
        auto* iW    = int_ty(ew);
        auto* i32   = b_.getInt32Ty();
        auto* arrty = llvm::ArrayType::get(slot, n);
        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* head  = llvm::BasicBlock::Create(ctx_, "oaupk.head", fn);
        auto* body  = llvm::BasicBlock::Create(ctx_, "oaupk.body", fn);
        auto* done  = llvm::BasicBlock::Create(ctx_, "oaupk.done", fn);
        auto* entry = b_.GetInsertBlock();
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        auto* k = b_.CreatePHI(i32, 2);
        k->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(k, b_.getInt32(n)), body, done);
        b_.SetInsertPoint(body);

        auto*        i32x2 = llvm::StructType::get(ctx_, { i32, i32 });
        auto*        base  = b_.CreateMul(k, b_.getInt32(nch));
        llvm::Value* aval  = llvm::ConstantInt::get(iW, 0);
        llvm::Value* bval  = llvm::ConstantInt::get(iW, 0);
        for (uint32_t c = 0; c < nch; ++c) {
            auto* flat  = b_.CreateAdd(base, b_.getInt32(c));
            auto* eptr  = dpi_oa_buf_elem(buf, n * nch, is_logic, flat);
            auto* shamt = llvm::ConstantInt::get(iW, 32u * c);
            auto* ac    = is_logic ? b_.CreateLoad(i32, b_.CreateStructGEP(i32x2, eptr, 0))
                                   : b_.CreateLoad(i32, eptr);
            // ZExtOrTrunc, not ZExt: iW may be narrower than i32 (w < 32), and the
            // trunc also drops the chunk's undefined bits above W.
            aval = b_.CreateOr(aval, b_.CreateShl(b_.CreateZExtOrTrunc(ac, iW), shamt));
            if (is_logic) {
                auto* bc = b_.CreateLoad(i32, b_.CreateStructGEP(i32x2, eptr, 1));
                bval     = b_.CreateOr(bval, b_.CreateShl(b_.CreateZExtOrTrunc(bc, iW), shamt));
            }
        }
        auto* slotptr = b_.CreateInBoundsGEP(arrty, storage, { b_.getInt32(0), k });
        if (is_logic) {
            auto* i2w  = int_ty(2 * ew);
            auto* must = b_.CreateAnd(aval, b_.CreateNot(bval));
            auto* may  = b_.CreateOr(aval, bval);
            auto* mm   = b_.CreateOr(b_.CreateZExt(must, i2w),
                                     b_.CreateShl(b_.CreateZExt(may, i2w), llvm::ConstantInt::get(i2w, ew)));
            b_.CreateStore(mm, b_.CreateStructGEP(slot, slotptr, 0));
            if (has_z) {
                auto* z = b_.CreateAnd(bval, b_.CreateNot(aval));
                b_.CreateStore(z, b_.CreateStructGEP(slot, slotptr, 1));
            }
        } else {
            b_.CreateStore(b_.CreateZExtOrTrunc(aval, slot->getElementType(0)),
                           b_.CreateStructGEP(slot, slotptr, 0));
        }
        k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
    }

    // The ir::Type Ref of a DPI open-array actual: resolve the operand's lvalue leaf
    // to its root Signal / Local / Arg and read that root's declared type (the whole
    // unpacked-array type, carrying element / left / right). The actual is always a
    // whole array (build_call_args passes its lvalue leaf).
    Ref open_array_type_ref(Index a) {
        const Index root = lvalue_root(*ir_, a).root;
        switch (ir_->tag(root)) {
            case Inst::Tag::Signal:
                return mod_.signal(ir_->payload(root).signal).type;
            case Inst::Tag::Local:
                return ir_->payload(root).local.type;
            case Inst::Tag::Arg:
                return ir_->payload(root).arg.type;
            default:
                throw BackendError{ "internal: DPI open-array actual is not a "
                                    "signal / local / arg root" };
        }
    }

    // Materialize an rt_svopen descriptor on the stack from its seven fields (data
    // pointer + six i32s). Fields may be runtime Values (dynamic-array length) or
    // constants (fixed array), so all are passed as Values.
    llvm::Value* emit_svopen(llvm::Value* data, llvm::Value* left, llvm::Value* right,
                             llvm::Value* nelem, llvm::Value* elem_bytes, llvm::Value* ew,
                             llvm::Value* is_logic) {
        auto* ty   = rt_svopen_ty();
        auto* desc = entry_alloca(ty, "dpi.oa.desc");
        b_.CreateStore(data, b_.CreateStructGEP(ty, desc, 0));
        b_.CreateStore(left, b_.CreateStructGEP(ty, desc, 1));
        b_.CreateStore(right, b_.CreateStructGEP(ty, desc, 2));
        b_.CreateStore(nelem, b_.CreateStructGEP(ty, desc, 3));
        b_.CreateStore(elem_bytes, b_.CreateStructGEP(ty, desc, 4));
        b_.CreateStore(ew, b_.CreateStructGEP(ty, desc, 5));
        b_.CreateStore(is_logic, b_.CreateStructGEP(ty, desc, 6));
        return desc;
    }

    // Build the svOpenArrayHandle for a dynamic-array open-array actual (LRM 7.5).
    // The actual is a runtime `{ data, len }` handle; for a named-scalar element the
    // heap buffer is already a contiguous C `T[len]`, so the descriptor points
    // straight at it (zero-copy, all directions — the C reads/writes the heap in
    // place; a fixed-size view, so it never resizes). Bounds are the array's natural
    // 0-based [0 : len-1]. Packed elements (a runtime-sized marshal scratch) are a
    // follow-up. NOTE: an empty array (len == 0) yields right == -1; a C consumer
    // must use svSize, not svLow..svHigh, to bound an empty array.
    llvm::Value* build_dpi_dynarray_desc(const LValue& lv, Ref arr_ref, DpiCType elem_ct) {
        const DynElem e            = dynarr_elem(arr_ref);
        const bool    named_scalar = elem_ct == DpiCType::Byte || elem_ct == DpiCType::Shortint ||
                                  elem_ct == DpiCType::Int || elem_ct == DpiCType::Longint;
        if (!named_scalar || !e.narrow) {
            throw BackendError{ "DPI open-array over a dynamic array supports only "
                                "byte/shortint/int/longint (zero-copy) elements so far" };
        }
        llvm::Value* handle = lv.base; // &rt_dynarr { data, len }
        llvm::Value* data =
          b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 0));
        llvm::Value* len = b_.CreateZExtOrTrunc(
          b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 1)),
          b_.getInt32Ty());
        const uint32_t ew = e.width;
        return emit_svopen(data, b_.getInt32(0), b_.CreateSub(len, b_.getInt32(1)), len,
                           b_.getInt32(ew / 8), b_.getInt32(ew), b_.getInt32(0));
    }

    // Build the svOpenArrayHandle (rt_svopen*) for a DPI open-array argument
    // (LRM 35.5.6.2). Named-scalar (byte/shortint/int/longint) elements are zero-copy
    // — Clotho's narrow `[N x iW]` element storage is already a contiguous C array —
    // so the descriptor points straight at the design array (the C reads and, for
    // output/inout, writes it directly). Packed bit/logic elements are marshalled in
    // inline LLVM IR into a contiguous svVecVal buffer: packed in for input/inout,
    // and recorded in `backs` for the unpack-back after the call for output/inout.
    llvm::Value* build_dpi_open_array_desc(Index a, DpiCType elem_ct, ArgDir dir,
                                           std::vector<OaMarshalBack>& backs) {
        const LValue lv = lvalue_of(a);
        const Ref    arr_ref = open_array_type_ref(a);
        const Type&  at      = std::get<Type>(mod_.pool.get(arr_ref));
        if (at.kind == Type::Kind::DynArray) {
            // Dynamic array actual (LRM 7.5): a runtime `{ data, len }` handle. A
            // named-scalar element is zero-copy — the heap buffer is already the C
            // array; packed/queue elements (marshal to a runtime-sized scratch) are a
            // follow-up (see docs/design/dpi-c.md).
            return build_dpi_dynarray_desc(lv, arr_ref, elem_ct);
        }
        if (at.kind != Type::Kind::UnpackedArray) {
            // Queue actuals need a ring-de-indexed marshal; deferred.
            throw BackendError{ "DPI open-array actual must be a fixed-size unpacked "
                                "array or dynamic array (queue deferred)" };
        }
        if (lv.array_ty == nullptr || lv.slot_ty == nullptr) {
            throw BackendError{ "internal: DPI open-array actual is not a whole memory" };
        }
        const uint32_t n     = array_dim(at);
        const int32_t  left  = static_cast<int32_t>(at.left);
        const int32_t  right = static_cast<int32_t>(at.right);
        const uint32_t ew    = lv.store_width; // element bit width
        const uint32_t nch   = (ew + 31) / 32; // svVecVal words per marshalled element
        const bool     pack_in  = dir != ArgDir::Out;   // input / inout read the actual
        const bool     copy_out = dir != ArgDir::In;    // output / inout write it back

        // Use the actual element storage slot (`lv.slot_ty`) — not a conservative
        // reconstruction — so may-z elision (a `{ i2W }` slot with no z word) is
        // reflected in the has_z flag the marshal needs.
        llvm::StructType* slot   = lv.slot_ty;
        const auto        stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(slot));
        llvm::Value*      storage = lv.base; // whole `[N x slot]` storage pointer

        llvm::Value* data;
        uint32_t     elem_bytes;
        uint32_t     is_logic;
        switch (elem_ct) {
            case DpiCType::Byte:
            case DpiCType::Shortint:
            case DpiCType::Int:
            case DpiCType::Longint: {
                // Zero-copy requires the narrow single-iW element slot (stride ==
                // C element size). A never-narrow named scalar (its z word forced by
                // a task out/inout use elsewhere) would not be C-layout — marshal it
                // as a bit vector instead (the mm plane's low W bits are the value).
                if (lv.narrow && stride == ew / 8) {
                    // The C reads/writes the design array directly; an output/inout
                    // write lands in storage with no copy-back (and, per DPI, does not
                    // trigger wait/NBA scheduling on the array — a documented gap).
                    data       = storage;
                    elem_bytes = ew / 8;
                    is_logic   = 0;
                    break;
                }
                auto* buf = entry_alloca(llvm::ArrayType::get(b_.getInt32Ty(), n * nch), "dpi.oa.bit");
                if (pack_in) {
                    emit_dpi_oa_pack_ir(storage, buf, slot, n, ew, nch, /*has_z=*/false, /*is_logic=*/false);
                }
                if (copy_out) {
                    backs.push_back({ false, buf, storage, slot, ew, n, nch, 0 });
                }
                data       = buf;
                elem_bytes = 4 * nch;
                is_logic   = 0;
                break;
            }
            case DpiCType::BitVec: {
                auto* buf = entry_alloca(llvm::ArrayType::get(b_.getInt32Ty(), n * nch), "dpi.oa.bit");
                if (pack_in) {
                    emit_dpi_oa_pack_ir(storage, buf, slot, n, ew, nch, /*has_z=*/false, /*is_logic=*/false);
                }
                if (copy_out) {
                    backs.push_back({ false, buf, storage, slot, ew, n, nch, 0 });
                }
                data       = buf;
                elem_bytes = 4 * nch; // svBitVecVal[nch]
                is_logic   = 0;
                break;
            }
            case DpiCType::LogicVec: {
                auto* svl = llvm::StructType::get(ctx_, { b_.getInt32Ty(), b_.getInt32Ty() });
                auto* buf = entry_alloca(llvm::ArrayType::get(svl, n * nch), "dpi.oa.logic");
                const uint32_t has_z = lv.has_z ? 1u : 0u;
                if (pack_in) {
                    emit_dpi_oa_pack_ir(storage, buf, slot, n, ew, nch, has_z != 0, /*is_logic=*/true);
                }
                if (copy_out) {
                    backs.push_back({ true, buf, storage, slot, ew, n, nch, has_z });
                }
                data       = buf;
                elem_bytes = 8 * nch; // svLogicVecVal[nch] {aval,bval}
                is_logic   = 1;
                break;
            }
            default:
                throw BackendError{ "internal: bad DPI open-array element ctype" };
        }

        return emit_svopen(data, b_.getInt32(static_cast<uint32_t>(left)),
                           b_.getInt32(static_cast<uint32_t>(right)), b_.getInt32(n),
                           b_.getInt32(elem_bytes), b_.getInt32(ew), b_.getInt32(is_logic));
    }

    // Lower a `call` to a DPI-C import (LRM 35): marshal each argument — scalar by
    // value (input) or through a C-native scratch pointer (output / inout), packed
    // vector always through an svBitVecVal* / svLogicVecVal* scratch — call the
    // external symbol directly (no state pointer), and convert the C return back to
    // a Plane. The frontend guards unsupported types, so no ref servicing.
    void lower_dpi_call(Index i, const CallInfo& ci, const Function& f) {
        llvm::Function*           cf = sym(fn_decls_[static_cast<uint32_t>(ci.func)]);
        std::vector<llvm::Value*> argv;
        // output / inout scratches to read back into the caller temp after the
        // call; the frontend's CopyOutArg then copies temp -> actual. `cty` is null
        // for a vec scratch (read back via dpi_vec_copyout instead).
        struct DpiOut {
            Index        temp;
            DpiCType     ct;
            llvm::Value* scratch;
            llvm::Type*  cty;
            uint32_t     w;
        };
        std::vector<DpiOut>        outs;
        std::vector<OaMarshalBack> oa_backs; // marshalled open-array out/inout copy-backs
        size_t                     k = 0;
        for (Index a : ir_->region(ci.args)) {
            const DpiCType ct  = f.dpi_arg_ctypes[k];
            const ArgDir   dir = f.arg_dirs.empty() ? ArgDir::In : f.arg_dirs[k];
            const uint32_t w   = width_of(f.param_types[k]);
            // Scratch allocas are entry-block-hoisted (entry_alloca), so a DPI call
            // in a loop does not grow the stack per iteration.
            if (ct == DpiCType::String) {
                // input string -> const char*: the handle's bytes are already NUL-
                // terminated (rt_string_data), so pass the pointer directly, no
                // copy. Valid for the call's duration; the C must not retain it.
                // output / inout string is frontend-guarded.
                argv.push_back(b_.CreateCall(str_data_fn(), { str_of(a) }));
            } else if (ct == DpiCType::OpenArray) {
                // Open array (LRM 35.5.6.2): build the svOpenArrayHandle descriptor
                // (zero-copy or marshalled) and pass it; a marshalled output/inout is
                // recorded in oa_backs for the unpack-back after the call.
                argv.push_back(
                  build_dpi_open_array_desc(a, f.dpi_arg_elem_ctypes[k], dir, oa_backs));
            } else if (is_dpi_vec(ct)) {
                // Packed vector: always by pointer. Input builds the scratch from the
                // value; output leaves it empty for the C to fill; inout copies in.
                llvm::Value* scratch = dpi_vec_alloca(ct, dpi_nchunks(w));
                if (dir == ArgDir::In) {
                    dpi_vec_copyin(ct, value_of(a), w, scratch);
                } else if (dir == ArgDir::Inout) {
                    dpi_vec_copyin(ct, lower_load(lvalue_of(a)), w, scratch);
                }
                argv.push_back(scratch);
                if (dir != ArgDir::In) {
                    outs.push_back({ a, ct, scratch, nullptr, w });
                }
            } else if (dir == ArgDir::In) {
                argv.push_back(dpi_arg_to_c(ct, value_of(a)));
            } else {
                llvm::Type*  cty     = dpi_c_llvm_ty(ct);
                llvm::Value* scratch = entry_alloca(cty, "dpi.out");
                if (dir == ArgDir::Inout) {
                    b_.CreateStore(dpi_arg_to_c(ct, lower_load(lvalue_of(a))), scratch);
                }
                argv.push_back(scratch);
                outs.push_back({ a, ct, scratch, cty, w });
            }
            ++k;
        }
        llvm::Value* r = b_.CreateCall(cf->getFunctionType(), cf, argv);
        for (const DpiOut& o : outs) {
            const Plane pl = is_dpi_vec(o.ct)
                               ? dpi_vec_copyout(o.ct, o.scratch, o.w)
                               : dpi_c_to_plane(o.ct, b_.CreateLoad(o.cty, o.scratch));
            lower_store(lvalue_of(o.temp), pl);
        }
        // Marshalled open-array output/inout: unpack the C-written svVecVal scratch
        // back into the design element storage (inline IR, native iW/i2W). A
        // zero-copy element was written in place, not here.
        for (const OaMarshalBack& o : oa_backs) {
            emit_dpi_oa_unpack_ir(o.buf, o.storage, o.slot, o.n, o.ew, o.nch, o.has_z != 0, o.is_logic);
        }
        if (f.dpi_ret_ctype == DpiCType::String) {
            // The C returns a NUL-terminated char*; copy it into this call's result
            // handle (str_of(i), alloc'd by build_str_temps). C keeps ownership.
            b_.CreateCall(dpi_str_out_fn(), { str_of(i), r });
        } else if (f.dpi_ret_ctype != DpiCType::Void) {
            set_value(i, dpi_c_to_plane(f.dpi_ret_ctype, r));
        }
    }

    // Whether the design has any DPI import or export — gates the svScope init.
    bool design_uses_dpi() const {
        if (!mod_.dpi_exports.empty()) {
            return true;
        }
        for (size_t i = 0; i < mod_.function_count(); ++i) {
            if (mod_.function(static_cast<FunctionId>(i)).is_dpi) {
                return true;
            }
        }
        return false;
    }

    // Emit one C-ABI thunk per `export "DPI-C"` (LRM 35.5). Runs last (after
    // emit_main creates @clotho.state and every SV function is declared).
    void emit_dpi_exports() {
        for (const DpiExport& ex : mod_.dpi_exports) {
            emit_dpi_export(ex);
        }
    }

    // The C-visible parameter type of an export formal: an input scalar passes by
    // value; output/inout scalars, packed vectors (any direction), and input
    // strings pass by pointer — mirroring declare_dpi_import.
    llvm::Type* dpi_export_c_param_ty(DpiCType ct, ArgDir dir) {
        if (is_dpi_vec(ct) || ct == DpiCType::String || dir != ArgDir::In) {
            return raw_ptr_ty();
        }
        return dpi_c_llvm_ty(ct);
    }

    // Convert a Plane (a value the C side provided) into the SV function's input
    // formal ABI — the packed (mm, z) pair, appended to argv (real via
    // real_to_planes). Shared by scalar and vec input args.
    void dpi_push_plane_arg(std::vector<llvm::Value*>& argv, Plane pl) {
        const Plane four = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
        argv.push_back(four.mm);
        argv.push_back(four.z);
    }

    // A C-callable entry point `<c_ret> <cid>(<c_args>)` (external linkage) that
    // marshals C arguments into the SV function's plane ABI, calls it against the
    // design-state global, and marshals results back to C. Input scalar/chandle/
    // real/vec and input string args; output/inout scalar/chandle/real/vec args;
    // scalar/chandle/real/void return (the frontend guards the rest). The marshal
    // reuses the import helpers in reverse.
    void emit_dpi_export(const DpiExport& ex) {
        const Function& f    = mod_.function(ex.func);
        llvm::Function* svfn = sym(fn_decls_[static_cast<uint32_t>(ex.func)]);

        std::vector<llvm::Type*> cparams;
        cparams.reserve(ex.arg_ctypes.size());
        for (size_t k = 0; k < ex.arg_ctypes.size(); ++k) {
            cparams.push_back(dpi_export_c_param_ty(ex.arg_ctypes[k], ex.arg_dirs[k]));
        }
        auto* fnty  = llvm::FunctionType::get(dpi_c_llvm_ty(ex.ret_ctype), cparams, false);
        auto* thunk = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, ex.extern_name, env_->llmod);
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", thunk));

        // Output / inout copy-back after the call: the (mm, z) plane cells the SV
        // function wrote, plus the C pointer to store the result through.
        struct ExpOut {
            llvm::Value* c_ptr;   // the C output pointer (scalar*) or svVecVal*
            llvm::Value* mm_ptr;  // SV out ABI: i(2W) cell
            llvm::Value* z_ptr;   // SV out ABI: iW cell
            DpiCType     ct;
            uint32_t     w;
        };
        std::vector<ExpOut>       outs;
        std::vector<llvm::Value*> str_temps; // input rt_string handles to deinit

        std::vector<llvm::Value*> argv;
        argv.push_back(sym(state_global_));
        for (size_t k = 0; k < ex.arg_ctypes.size(); ++k) {
            const DpiCType ct     = ex.arg_ctypes[k];
            const ArgDir   dir    = ex.arg_dirs[k];
            const uint32_t w      = width_of(f.param_types[k]);
            llvm::Value*   cparam = thunk->getArg(k);

            if (ct == DpiCType::String) { // input only (guarded)
                auto* h = entry_alloca(str_slot_ty(), "dpi.exp.str");
                b_.CreateCall(str_init_fn(), { h });
                b_.CreateCall(dpi_str_out_fn(), { h, cparam }); // h = the C const char*
                argv.push_back(h);
                str_temps.push_back(h);
            } else if (dir == ArgDir::In) {
                // Input value -> plane. A vec unpacks the C svVecVal; a scalar decodes
                // the C value directly.
                const Plane pl = is_dpi_vec(ct) ? dpi_vec_copyout(ct, cparam, w)
                                                : dpi_c_to_plane(ct, cparam);
                dpi_push_plane_arg(argv, pl);
            } else {
                // output / inout: the SV ABI is two pointers to (i(2W) mm, iW z)
                // cells the callee writes. inout copies the C value in first.
                llvm::Value* mm_ptr = entry_alloca(int_ty(2 * w), "dpi.exp.mm");
                llvm::Value* z_ptr  = entry_alloca(int_ty(w), "dpi.exp.z");
                if (dir == ArgDir::Inout) {
                    const Plane pl = is_dpi_vec(ct)
                                       ? dpi_vec_copyout(ct, cparam, w)
                                       : dpi_c_to_plane(ct, b_.CreateLoad(dpi_c_llvm_ty(ct), cparam));
                    const Plane four = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
                    b_.CreateStore(four.mm, mm_ptr);
                    b_.CreateStore(four.z, z_ptr);
                }
                argv.push_back(mm_ptr);
                argv.push_back(z_ptr);
                outs.push_back({ cparam, mm_ptr, z_ptr, ct, w });
            }
        }

        llvm::Value* r = b_.CreateCall(svfn->getFunctionType(), svfn, argv);

        // Copy each output / inout result back to its C pointer.
        for (const ExpOut& o : outs) {
            Plane pl{ b_.CreateLoad(int_ty(2 * o.w), o.mm_ptr), b_.CreateLoad(int_ty(o.w), o.z_ptr) };
            if (is_dpi_vec(o.ct)) {
                dpi_vec_copyin(o.ct, pl, o.w, o.c_ptr); // plane -> C svVecVal
            } else {
                if (o.ct == DpiCType::Real || o.ct == DpiCType::Shortreal) {
                    pl = real_plane(planes_to_real(pl, o.w));
                }
                b_.CreateStore(dpi_arg_to_c(o.ct, pl), o.c_ptr);
            }
        }
        for (llvm::Value* h : str_temps) {
            b_.CreateCall(str_deinit_fn(), { h });
        }

        if (ex.ret_ctype == DpiCType::Void) {
            b_.CreateRetVoid();
            return;
        }
        // Unpack the SV {mm, z} return struct into a Plane, then marshal to C.
        Plane         rp{ b_.CreateExtractValue(r, { 0 }), b_.CreateExtractValue(r, { 1 }) };
        const ValType t = from_ref(f.return_type);
        if (t.is_real) {
            rp = real_plane(planes_to_real(rp, t.width));
        }
        b_.CreateRet(dpi_arg_to_c(ex.ret_ctype, rp));
    }

    // Marshal an SV scalar value (a Plane the frontend resized to the formal type)
    // to its DPI-C native argument (LRM 35.5.6). A 2-state integer passes its value
    // plane; a scalar logic packs to svLogic (0/1/2=Z/3=X); a real rides its fp.
    llvm::Value* dpi_arg_to_c(DpiCType t, Plane p) {
        switch (t) {
            case DpiCType::Byte:
                return cast(must_of(p), 8, true);
            case DpiCType::Shortint:
                return cast(must_of(p), 16, true);
            case DpiCType::Int:
                return cast(must_of(p), 32, true);
            case DpiCType::Longint:
                return cast(must_of(p), 64, true);
            case DpiCType::Real:
            case DpiCType::Shortreal:
                return fp_of(p);
            case DpiCType::Chandle:
                // The i64 handle bits reinterpreted as an opaque void*.
                return b_.CreateIntToPtr(cast(must_of(p), 64, false), raw_ptr_ty());
            case DpiCType::BitScalar:
                return cast(must_of(p), 8, false); // zext the 1-bit value to svBit
            case DpiCType::LogicScalar: {
                // svLogic: sv_0=0, sv_1=1, sv_z=2, sv_x=3. z wins; else 1 for a
                // definite 1; else 3 (X) for any other unknown; else 0. The formal
                // is width 1. A 4-state Plane may carry no z plane (never-z), so
                // materialize the z bit safely rather than through z_of (nullptr).
                const uint32_t w    = width_of(p);
                llvm::Value*   zbit = (p.two || p.z == nullptr) ? b_.getFalse()
                                                                : b_.CreateICmpNE(p.z, zero(w));
                return b_.CreateSelect(
                  zbit, b_.getInt8(2),
                  b_.CreateSelect(b_.CreateICmpNE(must_of(p), zero(w)), b_.getInt8(1),
                                  b_.CreateSelect(b_.CreateICmpNE(may_of(p), zero(w)),
                                                  b_.getInt8(3), b_.getInt8(0))));
            }
            default:
                throw BackendError{ "internal: DPI void argument" };
        }
    }

    // Convert a DPI-C native return value back to a Plane of the SV return type.
    Plane dpi_c_to_plane(DpiCType t, llvm::Value* r) {
        switch (t) {
            case DpiCType::Byte:
            case DpiCType::Shortint:
            case DpiCType::Int:
            case DpiCType::Longint:
                return two_val(r); // the C iW is the 2-state value plane
            case DpiCType::Real:
            case DpiCType::Shortreal:
                return real_plane(r);
            case DpiCType::Chandle:
                // The returned void* reinterpreted as the i64 handle value.
                return two_val(b_.CreatePtrToInt(r, int_ty(64)));
            case DpiCType::BitScalar:
                return two_val(b_.CreateTrunc(r, int_ty(1)));
            case DpiCType::LogicScalar: {
                // Decode svLogic (i8): 0->0, 1->1, 2->Z, 3->X into must/may/z (i1).
                llvm::Value* must = b_.CreateICmpEQ(r, b_.getInt8(1));
                llvm::Value* may  = b_.CreateICmpNE(r, b_.getInt8(0));
                llvm::Value* z    = b_.CreateICmpEQ(r, b_.getInt8(2));
                return packed(must, may, z);
            }
            default:
                throw BackendError{ "internal: DPI void return has no value" };
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

    // The fork join-record type { i32 count, ptr parent }, shared by a fork's
    // children (see lower_fork_inst and the fork-child epilogue in lower_task).
    llvm::StructType* fork_record_ty() {
        if (fork_record_ty_ == nullptr) {
            fork_record_ty_ = llvm::StructType::create(
              ctx_, { b_.getInt32Ty(), llvm::PointerType::getUnqual(ctx_) }, "clotho.fork_record");
        }
        return fork_record_ty_;
    }

    // The current coroutine's fork join-record slot: one frame-resident alloca
    // reused across this coroutine's fork sites (a `join` completes before the
    // next fork runs). Hoisted to coro.begin so it spills into the frame — it must
    // outlive both the children's suspends and the parent's join suspend.
    llvm::Value* fork_record_slot() {
        if (fork_record_ == nullptr) {
            llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
            fork_record_ = ab.CreateAlloca(fork_record_ty(), nullptr, "fork_record");
        }
        return fork_record_;
    }

    // Lower a `Fork` (LRM 9.3.2, V1 = `join`): spawn each child coroutine sharing
    // one join record, then suspend the parent until the live count reaches zero.
    void lower_fork_inst(Index i) {
        const auto fi     = ir_->extra_data<ForkInfo>(ir_->payload(i).extra);
        const auto kids   = ir_->region(fi.children); // FunctionId u32s
        auto*      ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto*      null   = llvm::ConstantPointerNull::get(ptr_ty);
        auto*      i32    = b_.getInt32Ty();
        auto*      rty    = fork_record_ty();

        // Init the shared record { count = W, parent = null }.
        llvm::Value* rec = fork_record_slot();
        b_.CreateStore(b_.getInt32(static_cast<uint32_t>(kids.size())),
                       b_.CreateStructGEP(rty, rec, 0));
        b_.CreateStore(null, b_.CreateStructGEP(rty, rec, 1));

        // Spawn each child: ramp (state, wheel, rec) -> handle, resume inline to its
        // first suspend or completion. A child that completes synchronously has
        // already decremented the count (parent still null -> no wake); destroy its
        // frame. A child that suspends is parked in the wheel / a wait queue and
        // decrements + wakes on completion.
        for (const Index kid : kids) {
            llvm::Function* cf  = sym(fn_decls_[static_cast<uint32_t>(kid)]);
            llvm::Value*    hdl = b_.CreateCall(cf->getFunctionType(), cf, { state_, wheel_, rec });
            b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
            llvm::Value* done = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { hdl });
            auto*        dz   = llvm::BasicBlock::Create(ctx_, "fork.child.done", fn_);
            auto*        nx   = llvm::BasicBlock::Create(ctx_, "fork.child.next", fn_);
            b_.CreateCondBr(done, dz, nx);
            b_.SetInsertPoint(dz);
            b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
            b_.CreateBr(nx);
            b_.SetInsertPoint(nx);
        }

        // Join: if every child already finished (count 0), fall through; else record
        // our handle and suspend — the last child to finish resumes us.
        llvm::Value* cnt  = b_.CreateLoad(i32, b_.CreateStructGEP(rty, rec, 0));
        auto*        susp = llvm::BasicBlock::Create(ctx_, "fork.join.suspend", fn_);
        auto*        cont = llvm::BasicBlock::Create(ctx_, "fork.join.cont", fn_);
        b_.CreateCondBr(b_.CreateICmpEQ(cnt, b_.getInt32(0)), cont, susp);

        b_.SetInsertPoint(susp);
        b_.CreateStore(hdl_, b_.CreateStructGEP(rty, rec, 1));
        emit_suspend("fork.join.resume");
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
    }

    // Lower a task enable (LRM 13.3). Create the task coroutine, copy inputs in /
    // pass output-inout temp pointers, drive it once, and either finish inline (the
    // task consumed no time) or park the caller until the task completes and resumes
    // it. Only valid inside a coroutine — the frontend never emits a TaskCall in a
    // plain function.
    void lower_task_call_inst(Index i) {
        const auto      ti = ir_->extra_data<TaskCallInfo>(ir_->payload(i).extra);
        const Function& tf = mod_.function(ti.func);
        llvm::Function* cf = sym(fn_decls_[static_cast<uint32_t>(ti.func)]);
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
        // Snapshot each ref-actual signal with waiters before the call; the wait
        // queue is serviced at the continuation with the net old -> new transition.
        // NOTE: a task that writes the same ref signal across a #delay (multiple time
        // steps) wakes level/edge waiters only once, at task completion, with the net
        // transition — not per intermediate write. Correct for a whole-signal ref
        // written in one time step (the common case) and for functions (atomic).
        // Per-write mid-suspend waking needs callee-side servicing (a generic
        // runtime wake keyed by signal id); TODO once that exists.
        std::vector<std::pair<LValue, ServiceSnapshot>> ref_svc;
        for (uint32_t k = 0; k < args.size(); ++k) {
            if (is_handle_type(tf.param_types[k]) && tf.arg_dirs[k] != ArgDir::Ref) {
                // One handle pointer for every by-value direction; callee copies
                // in / out through it.
                argv.push_back(lvalue_of(args[k]).base);
            } else if (tf.arg_dirs[k] == ArgDir::Ref) {
                // Pass one pointer to the actual's slot (scalar, array element, or a
                // dynamic-array / queue handle); the callee aliases it.
                const LValue& lv = lvalue_of(args[k]);
                argv.push_back(is_handle_type(tf.param_types[k]) ? lv.base : ref_actual_ptr(lv));
                if (!is_handle_type(tf.param_types[k])) {
                    ref_svc.emplace_back(lv, service_snapshot(lv));
                }
            } else if (tf.arg_dirs[k] == ArgDir::In) {
                if (is_string_type(tf.param_types[k])) {
                    argv.push_back(str_of(args[k]));
                    continue;
                }
                Plane pl = value_of(args[k]);
                pl       = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
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
        for (auto& [lv, snap] : ref_svc) {
            service_after_write(lv, snap);
        }
    }

    // Build the runtime rt_arg[] descriptor array shared by every $display-family
    // format call: stack-allocate one slot per value operand plus one shared word
    // buffer holding every integral operand's must / xz / z planes (ceil(w/64)
    // words each, zero-extended above the operand width). The descriptor's plane
    // fields point into that buffer, so an operand of any width is passed in full.
    // Returns the descriptor array pointer (null when there are no operands).
    // `who` is unused now that no width is rejected; kept for call-site clarity.
    llvm::Value* build_rt_args(std::span<const Index> args, const char* /*who*/) {
        const auto n = static_cast<uint32_t>(args.size());
        if (n == 0) {
            return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        }

        // Pass 1: size the shared plane buffer (three planes per integral operand,
        // one word per real; strings carry the handle in `val` directly).
        std::vector<uint32_t> words(n, 0);
        uint32_t              total = 0;
        for (uint32_t k = 0; k < n; ++k) {
            const ValType t = type_of(args[k]);
            if (t.is_string) {
                continue;
            }
            if (t.is_real) {
                words[k] = 1;
                total += 1;
                continue;
            }
            words[k] = (t.width + 63) / 64;
            total += 3 * words[k];
        }

        llvm::StructType* sty    = arg_struct_ty();
        llvm::Value*      arr    = b_.CreateAlloca(sty, b_.getInt32(n), "fmt_args");
        auto*             i64    = b_.getInt64Ty();
        llvm::Value*      planes = total != 0 ? b_.CreateAlloca(i64, b_.getInt32(total), "fmt_planes") : nullptr;
        auto*             null   = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));

        // Store an iW plane, zero-extended to a whole number of words, into the
        // shared buffer at word offset `off`; return the element pointer.
        uint32_t off = 0;
        auto     put = [&](llvm::Value* plane, uint32_t nwords) {
            llvm::Value* ep = b_.CreateConstInBoundsGEP1_32(i64, planes, off);
            b_.CreateAlignedStore(b_.CreateZExtOrBitCast(plane, int_ty(nwords * 64)), ep, llvm::Align(8));
            off += nwords;
            return ep;
        };

        for (uint32_t k = 0; k < n; ++k) {
            const Plane   p    = value_of(args[k]);
            const ValType t    = type_of(args[k]);
            llvm::Value*  elt  = b_.CreateConstInBoundsGEP1_32(sty, arr, k);
            if (t.is_string) {
                // A string-object argument (%s): flag bit 1, val = the rt_string
                // handle pointer itself; the width / plane fields are unused.
                store_field(sty, elt, 0, b_.getInt32(0));
                store_field(sty, elt, 1, b_.getInt32(2));
                store_field(sty, elt, 2, str_of(args[k]));
                store_field(sty, elt, 3, null);
                store_field(sty, elt, 4, null);
                continue;
            }
            if (t.is_real) {
                // A real rides one `val` word as a double bit pattern (flags bit
                // 2); a shortreal widens to double first so the runtime handles
                // one representation. Reals are signed and have no x/z.
                llvm::Value* fv = fp_of(p);
                if (t.width == 32) {
                    fv = b_.CreateFPExt(fv, b_.getDoubleTy());
                }
                store_field(sty, elt, 0, b_.getInt32(64));
                store_field(sty, elt, 1, b_.getInt32(0b101u));
                store_field(sty, elt, 2, put(b_.CreateBitCast(fv, i64), 1));
                store_field(sty, elt, 3, null);
                store_field(sty, elt, 4, null);
                continue;
            }
            // xz = must ^ may marks every unknown bit (X and Z); z marks the Z
            // subset. The runtime distinguishes them per bit.
            llvm::Value* must = must_of(p);
            llvm::Value* xz   = b_.CreateXor(must, may_of(p));
            store_field(sty, elt, 0, b_.getInt32(t.width));
            store_field(sty, elt, 1, b_.getInt32(t.is_signed ? 1u : 0u));
            store_field(sty, elt, 2, put(must, words[k]));
            store_field(sty, elt, 3, put(xz, words[k]));
            store_field(sty, elt, 4, put(z_of(p), words[k]));
        }
        return arr;
    }

    // Lower a $display / $write / $fdisplay / $fwrite: build the descriptor array
    // and call the runtime formatter with the destination descriptor.
    void lower_display(Index i) {
        // $display / $write target the standard-output MCD (bit 0).
        emit_display(ir_->extra_data<DisplayInfo>(ir_->payload(i).extra), b_.getInt32(1));
    }

    void lower_fdisplay(Index i) {
        // $fdisplay / $fwrite carry an explicit descriptor operand.
        const auto& p = ir_->payload(i).op_extra;
        emit_display(ir_->extra_data<DisplayInfo>(p.extra), cast(must_of(value_of(p.op)), 32, true));
    }

    void emit_display(const DisplayInfo& info, llvm::Value* desc) {
        std::string fmt = ir_->extra_bytes(info.fmt, info.fmt_len);
        if (info.newline) {
            fmt += '\n';
        }

        const std::span<const Index> args = ir_->region(info.args);
        llvm::Value*                 arr  = build_rt_args(args, "$display / $write");
        llvm::Value*                 fmt_ptr = b_.CreateGlobalString(fmt);
        b_.CreateCall(fwrite_fn(), { desc, fmt_ptr, b_.getInt32(static_cast<uint32_t>(args.size())), arr });
    }

    void store_field(llvm::StructType* sty, llvm::Value* elt, unsigned idx, llvm::Value* v) {
        b_.CreateStore(v, b_.CreateStructGEP(sty, elt, idx));
    }

    // -- Plane helpers --

    static uint32_t bits_of(llvm::Value* v) {
        return llvm::cast<llvm::IntegerType>(v->getType())->getBitWidth();
    }
    // Value width W of a plane. A 2-state value's mm IS the iW value; a 4-state
    // value packs must|may into i(2W).
    static uint32_t width_of(Plane a) {
        return a.two ? bits_of(a.mm) : bits_of(a.mm) / 2;
    }

    // The must (low W) and may (high W) halves of a plane. For a 2-state value
    // both are the value itself (must == may, no unknown bits).
    llvm::Value* must_of(Plane a) {
        if (a.mm == nullptr) {
            // Depth guard: a real value reached an integer-plane lowering; the
            // frontend must convert reals explicitly (float tags), never route
            // them through integer ops.
            throw BackendError{ "internal: integer plane of a real value" };
        }
        return a.two ? a.mm : b_.CreateTrunc(a.mm, int_ty(width_of(a)));
    }
    llvm::Value* may_of(Plane a) {
        if (a.two) {
            return a.mm;
        }
        const uint32_t w = width_of(a);
        return b_.CreateTrunc(b_.CreateLShr(a.mm, w), int_ty(w));
    }

    // The z plane, materialized as a zero constant for a 2-state (never-z) value.
    llvm::Value* z_of(Plane a) {
        return a.two ? zero(width_of(a)) : a.z;
    }

    // Wrap a single iW value as a 2-state Plane.
    Plane two_val(llvm::Value* v) {
        return { v, nullptr, true };
    }

    // Lift a 2-state value into the 4-state must|may encoding (must == may == v,
    // z = 0); a no-op on an already-4-state value. Used at storage / ABI sinks
    // that still consume the packed i(2W) word.
    Plane to_four(Plane a) {
        if (!a.two) {
            return a;
        }
        const uint32_t w = bits_of(a.mm);
        return packed(a.mm, a.mm, zero(w));
    }

    // Pack iW must + iW may (+ iW z) into a Plane (mm is i(2W)).
    Plane packed(llvm::Value* must, llvm::Value* may, llvm::Value* z) {
        const uint32_t w    = bits_of(must);
        auto*          wide = int_ty(2 * w);
        llvm::Value*   mm   = b_.CreateOr(b_.CreateZExt(must, wide), b_.CreateShl(b_.CreateZExt(may, wide), w));
        return { mm, z };
    }

    // i1: does this value contain any X/Z bit? Unknown bits are exactly must!=may.
    // A 2-state value never has any.
    llvm::Value* any_unknown(Plane a) {
        if (a.two) {
            return b_.getFalse();
        }
        llvm::Value* u = b_.CreateXor(must_of(a), may_of(a));
        return b_.CreateICmpNE(u, llvm::ConstantInt::get(u->getType(), 0));
    }

    // Pack an i1 must + i1 may into a width-1 Plane (z cleared).
    Plane bit_plane(llvm::Value* must_i1, llvm::Value* may_i1) {
        return packed(must_i1, may_i1, zero(1));
    }

    // Cast all planes to width `to`. No-op when already that width — avoids a
    // pointless unpack/repack of the packed mm. Preserves the value's domain: a
    // 2-state value stays a single iW.
    Plane cast_plane(Plane a, uint32_t to, bool sign) {
        if (width_of(a) == to) {
            return a;
        }
        if (a.two) {
            return two_val(cast(a.mm, to, sign));
        }
        return packed(cast(must_of(a), to, sign), cast(may_of(a), to, sign), cast(a.z, to, sign));
    }

    // Convert a value to a target domain + width. Domain projection follows LRM
    // 1800-2023 6.3.1: assigning a 4-state value to a 2-state target drops x/z to
    // 0 (that is exactly the must plane, since must is 0 for both x and z);
    // widening a 2-state value to 4-state duplicates it into both planes with z=0.
    // Width resize (sign-aware) happens after the domain projection.
    Plane convert(Plane a, uint32_t to_width, bool four_state, bool sign) {
        if (four_state && a.two) {
            a = to_four(a);
        } else if (!four_state && !a.two) {
            a = two_val(must_of(a));
        }
        return cast_plane(a, to_width, sign);
    }

    // Combined per-bit X/Z mask (iW) of two same-width operands, tested once
    // for the pair. The X mask of a packed value is `must ^ may`, which sits
    // in the low W bits of `mm ^ (mm >> W)`. X-ness is nonlinear, so each
    // operand's mask is taken separately before OR-ing (merging the mm words
    // first would be wrong).
    llvm::Value* xz_mask(Plane a, Plane b, uint32_t w) {
        llvm::Value* xa = b_.CreateXor(a.mm, b_.CreateLShr(a.mm, w));
        llvm::Value* xb = b_.CreateXor(b.mm, b_.CreateLShr(b.mm, w));
        return b_.CreateTrunc(b_.CreateOr(xa, xb), int_ty(w));
    }

    // -- Real (floating point) helpers --
    //
    // A real SSA value rides Plane.fp. Its storage / ABI form is the IEEE-754
    // bit pattern duplicated across must and may (a legal known value; the
    // must => may invariant holds) with z = 0, so state layout, change
    // detection, NBA staging, and the function ABI stay integer. Pack / unpack
    // happens only at these boundaries; instcombine folds the chains between
    // back-to-back float ops.

    llvm::Type* fp_ty(uint32_t width) {
        return width == 64 ? static_cast<llvm::Type*>(b_.getDoubleTy()) : b_.getFloatTy();
    }

    Plane real_plane(llvm::Value* fp) {
        return Plane{ .fp = fp };
    }

    llvm::Value* fp_of(Plane a) {
        if (a.fp == nullptr) {
            throw BackendError{ "internal: real op on a non-real value" };
        }
        return a.fp;
    }

    // Bit pattern -> storage planes (must = may = pattern, z = 0).
    Plane real_to_planes(llvm::Value* fp) {
        const uint32_t w   = fp->getType()->isDoubleTy() ? 64 : 32;
        llvm::Value*   pat = b_.CreateBitCast(fp, int_ty(w));
        return packed(pat, pat, zero(w));
    }

    // Storage planes -> float value (the must half is the bit pattern).
    llvm::Value* planes_to_real(Plane a, uint32_t width) {
        return b_.CreateBitCast(must_of(a), fp_ty(width));
    }

    Plane lower_real_arith(Inst::Tag tag, Plane a, Plane b) {
        llvm::Value* x = fp_of(a);
        llvm::Value* y = fp_of(b);
        switch (tag) {
            case Inst::Tag::FAdd:
                return real_plane(b_.CreateFAdd(x, y));
            case Inst::Tag::FSub:
                return real_plane(b_.CreateFSub(x, y));
            case Inst::Tag::FMul:
                return real_plane(b_.CreateFMul(x, y));
            case Inst::Tag::FDiv:
                return real_plane(b_.CreateFDiv(x, y));
            default:
                // FPow: C pow semantics cover the corners LRM 11.4.3 leaves
                // unspecified.
                return real_plane(b_.CreateBinaryIntrinsic(llvm::Intrinsic::pow, x, y));
        }
    }

    // Real comparison -> definite 1-bit (reals carry no x/z). Ordered
    // predicates except FNe, which is unordered so NaN != NaN yields 1,
    // matching C.
    Plane lower_real_compare(Inst::Tag tag, Plane a, Plane b) {
        llvm::Value* x = fp_of(a);
        llvm::Value* y = fp_of(b);
        llvm::Value* c = nullptr;
        switch (tag) {
            case Inst::Tag::FEq:
                c = b_.CreateFCmpOEQ(x, y);
                break;
            case Inst::Tag::FNe:
                c = b_.CreateFCmpUNE(x, y);
                break;
            case Inst::Tag::FLt:
                c = b_.CreateFCmpOLT(x, y);
                break;
            case Inst::Tag::FLe:
                c = b_.CreateFCmpOLE(x, y);
                break;
            case Inst::Tag::FGt:
                c = b_.CreateFCmpOGT(x, y);
                break;
            default:
                c = b_.CreateFCmpOGE(x, y);
                break;
        }
        return bit_plane(c, c);
    }

    // Float -> integral. FtoI rounds half away from zero (LRM 6.12.1, the
    // assignment / cast conversion); FtoITrunc truncates toward zero ($rtoi,
    // LRM 20.5). Both go through a saturating signed 64-bit intermediate:
    // fptosi.sat avoids poison on out-of-range / NaN inputs (behavior the LRM
    // leaves undefined), and the final width cast wraps like an integer
    // assignment.
    Plane lower_ftoi(Inst::Tag tag, Plane v, ValType to) {
        llvm::Value* f = fp_of(v);
        if (tag == Inst::Tag::FtoI) {
            f = b_.CreateUnaryIntrinsic(llvm::Intrinsic::round, f);
        }
        llvm::Value* wide = b_.CreateIntrinsic(llvm::Intrinsic::fptosi_sat, { int_ty(64), f->getType() }, { f });
        llvm::Value* r    = cast(wide, to.width, true);
        return packed(r, r, zero(to.width));
    }

    // Mux with real alternatives. Definite condition selects normally; an
    // ambiguous (x/z) condition compares the alternatives with == and yields
    // the common value, else the real default 0.0 (LRM 11.4.11). NaN arms
    // compare unequal, so they fall to 0.0.
    Plane lower_real_mux(Plane cond, Plane a, Plane b) {
        llvm::Value* known = b_.CreateICmpEQ(must_of(cond), may_of(cond));
        llvm::Value* ctrue = b_.CreateICmpNE(must_of(cond), zero(1));
        llvm::Value* x     = fp_of(a);
        llvm::Value* y     = fp_of(b);
        llvm::Value* sel   = b_.CreateSelect(ctrue, x, y);
        llvm::Value* eq    = b_.CreateFCmpOEQ(x, y);
        llvm::Value* amb   = b_.CreateSelect(eq, x, llvm::ConstantFP::get(x->getType(), 0.0));
        return real_plane(b_.CreateSelect(known, sel, amb));
    }

    // A double-typed libm routine, resolved from the host process by the JIT
    // (DynamicLibrarySearchGenerator in run_jit).
    llvm::Value* libm_call(const char* name, llvm::Value* x, llvm::Value* y = nullptr) {
        auto* dbl = b_.getDoubleTy();
        if (y != nullptr) {
            return b_.CreateCall(env_->llmod.getOrInsertFunction(name, dbl, dbl, dbl), { x, y });
        }
        return b_.CreateCall(env_->llmod.getOrInsertFunction(name, dbl, dbl), { x });
    }

    // rotl64(x, k) = (x << k) | (x >> (64 - k)).
    llvm::Value* rotl64(llvm::Value* x, unsigned k) {
        return b_.CreateOr(b_.CreateShl(x, k), b_.CreateLShr(x, 64 - k));
    }

    // One xoshiro256++ step against the process frame RNG (rng_slot_): load the
    // four state words, produce the 64-bit result, advance and store the state.
    // Inlined at each $urandom site (no per-draw runtime call).
    llvm::Value* emit_xoshiro_next() {
        auto* i64ty = b_.getInt64Ty();
        auto* arrty = llvm::ArrayType::get(i64ty, 4);
        llvm::Value* s[4];
        for (int k = 0; k < 4; ++k) {
            auto* ptr =
              b_.CreateInBoundsGEP(arrty, rng_slot_, { b_.getInt32(0), b_.getInt32(k) });
            s[k] = b_.CreateLoad(i64ty, ptr);
        }
        // result = rotl(s0 + s3, 23) + s0.
        llvm::Value* result = b_.CreateAdd(rotl64(b_.CreateAdd(s[0], s[3]), 23), s[0]);
        // State update (order matters: reads see already-updated words).
        llvm::Value* t   = b_.CreateShl(s[1], 17);
        llvm::Value* n2  = b_.CreateXor(s[2], s[0]);
        llvm::Value* n3  = b_.CreateXor(s[3], s[1]);
        llvm::Value* n1  = b_.CreateXor(s[1], n2);
        llvm::Value* n0  = b_.CreateXor(s[0], n3);
        n2               = b_.CreateXor(n2, t);
        n3               = rotl64(n3, 45);
        llvm::Value* ns[4] = { n0, n1, n2, n3 };
        for (int k = 0; k < 4; ++k) {
            auto* ptr =
              b_.CreateInBoundsGEP(arrty, rng_slot_, { b_.getInt32(0), b_.getInt32(k) });
            b_.CreateStore(ns[k], ptr);
        }
        return result;
    }

    // $urandom (LRM §18.13.1): the high 32 bits of one xoshiro256++ draw.
    Plane lower_urandom() {
        llvm::Value* hi = b_.CreateTrunc(b_.CreateLShr(emit_xoshiro_next(), 32), b_.getInt32Ty());
        return two_val(hi);
    }

    Plane lower_real_math_unary(Index i) {
        const auto&  p = ir_->payload(i).op_imm;
        llvm::Value* x = fp_of(value_of(p.op));
        using O = RealMathOp;
        namespace In = llvm::Intrinsic;
        switch (static_cast<O>(p.imm)) {
            // LRM Table 20-4 names the C math function each maps to. The
            // classic routines lower to LLVM intrinsics (constant-foldable);
            // the inverse / hyperbolic ones call libm directly.
            case O::Ln:
                return real_plane(b_.CreateUnaryIntrinsic(In::log, x));
            case O::Log10:
                return real_plane(b_.CreateUnaryIntrinsic(In::log10, x));
            case O::Exp:
                return real_plane(b_.CreateUnaryIntrinsic(In::exp, x));
            case O::Sqrt:
                return real_plane(b_.CreateUnaryIntrinsic(In::sqrt, x));
            case O::Floor:
                return real_plane(b_.CreateUnaryIntrinsic(In::floor, x));
            case O::Ceil:
                return real_plane(b_.CreateUnaryIntrinsic(In::ceil, x));
            case O::Sin:
                return real_plane(b_.CreateUnaryIntrinsic(In::sin, x));
            case O::Cos:
                return real_plane(b_.CreateUnaryIntrinsic(In::cos, x));
            case O::Tan:
                return real_plane(libm_call("tan", x));
            case O::Asin:
                return real_plane(libm_call("asin", x));
            case O::Acos:
                return real_plane(libm_call("acos", x));
            case O::Atan:
                return real_plane(libm_call("atan", x));
            case O::Sinh:
                return real_plane(libm_call("sinh", x));
            case O::Cosh:
                return real_plane(libm_call("cosh", x));
            case O::Tanh:
                return real_plane(libm_call("tanh", x));
            case O::Asinh:
                return real_plane(libm_call("asinh", x));
            case O::Acosh:
                return real_plane(libm_call("acosh", x));
            case O::Atanh:
                return real_plane(libm_call("atanh", x));
            case O::Pow:
            case O::Atan2:
            case O::Hypot:
                break;
        }
        throw BackendError{ "internal: two-operand routine on RealMathUnary" };
    }

    Plane lower_real_math_binary(Index i) {
        const auto&  p    = ir_->payload(i).op_extra;
        const auto   info = ir_->extra_data<RealMathBinaryInfo>(p.extra);
        llvm::Value* x    = fp_of(value_of(p.op));
        llvm::Value* y    = fp_of(value_of(info.rhs));
        using O = RealMathOp;
        switch (static_cast<O>(info.fn)) {
            case O::Pow:
                return real_plane(b_.CreateBinaryIntrinsic(llvm::Intrinsic::pow, x, y));
            case O::Atan2:
                return real_plane(libm_call("atan2", x, y));
            case O::Hypot:
                return real_plane(libm_call("hypot", x, y));
            default:
                throw BackendError{ "internal: one-operand routine on RealMathBinary" };
        }
    }

    // -- Arithmetic: any X/Z in an operand makes the whole result X. --
    //
    // Lowered as a branch, not a select: the X case is rare, so it is split into a
    // cold block that just materialises the all-X constant (one move-immediate),
    // while the hot block does a single W-bit add/sub/mul of the value and
    // duplicates it across both planes (known bits have must == may).

    Plane lower_arith(Inst::Tag tag, Plane a, Plane b) {
        const uint32_t w    = width_of(a);

        // 2-state: a single native op on the value; no X rule, no plane duplication.
        // Both operands must be 2-state for this -- a 4-state/2-state mix (LRM
        // 5.7: any 4-state operand makes the result 4-state) still needs the
        // packed-plane path below, which assumes both operands already arrived
        // as i(2W) (xz_mask reads a.mm/b.mm raw, not through must_of/may_of).
        // Promote the narrower side instead of trusting the dispatch on `a`
        // alone -- a mismatched-domain pair here is malformed LLVM IR (an i(2W)
        // vs iW operand pair), not just a wrong answer.
        if (a.two && !b.two) {
            a = to_four(a);
        } else if (!a.two && b.two) {
            b = to_four(b);
        } else if (a.two) {
            switch (tag) {
                case Inst::Tag::Add: return two_val(b_.CreateAdd(a.mm, b.mm));
                case Inst::Tag::Sub: return two_val(b_.CreateSub(a.mm, b.mm));
                default:             return two_val(b_.CreateMul(a.mm, b.mm)); // Mul
            }
        }

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

        llvm::Value* unknown = b_.CreateICmpNE(xz_mask(a, b, w), zero(w));

        auto* x_bb   = llvm::BasicBlock::Create(ctx_, "arith.x", fn_);
        auto* val_bb = llvm::BasicBlock::Create(ctx_, "arith.val", fn_);
        auto* end_bb = llvm::BasicBlock::Create(ctx_, "arith.end", fn_);

        llvm::BranchInst* br = b_.CreateCondBr(unknown, x_bb, val_bb);
        br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

        // Cold: whole result is X.
        b_.SetInsertPoint(x_bb);
        llvm::Value* x_word = x_mm(w);
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
        mm->addIncoming(x_word, x_bb);
        mm->addIncoming(val_mm, val_bb);
        return { mm, zero(w) };
    }

    // Divide and modulo. LRM 1800-2023 §11.4.3: integer division truncates toward
    // zero, modulo takes the sign of the dividend, and a zero divisor or any X/Z
    // bit in either operand makes the whole result X.
    Plane lower_divmod(Inst::Tag tag, Plane a, Plane b, bool sign) {
        const uint32_t w      = width_of(a);
        const bool     is_div = tag == Inst::Tag::Div;

        // 2-state: divide/modulo by zero is x, which collapses to 0 in a 2-state
        // result (verified against iverilog: `int`/`bit` div/mod by 0 -> 0). Guard
        // the divisor to avoid poison, then select 0 on a zero divisor. Signed
        // INT_MIN / -1 is also poison, patched to the wrapped value like the
        // 4-state path.
        //
        // Both operands must be 2-state for this fast path -- a 4-state/2-state
        // mix (LRM 5.7: any 4-state operand makes the result 4-state) still
        // needs the packed-plane path below, which assumes both operands
        // already arrived as i(2W) (xz_mask reads a.mm/b.mm raw). Promote the
        // narrower side instead of trusting the dispatch on `a` alone -- a
        // mismatched-domain pair here is malformed LLVM IR (an i(2W) vs iW
        // operand to the same icmp/select), observed to crash inside LLVM's
        // constant folder rather than just miscompile.
        if (a.two && !b.two) {
            a = to_four(a);
        } else if (!a.two && b.two) {
            b = to_four(b);
        } else if (a.two) {
            llvm::Value* dz = b_.CreateICmpEQ(b.mm, zero(w));
            llvm::Value* r;
            if (!sign) {
                llvm::Value* safe = b_.CreateSelect(dz, ones(w), b.mm);
                r = is_div ? b_.CreateUDiv(a.mm, safe) : b_.CreateURem(a.mm, safe);
            } else {
                llvm::Value* neg1 = b_.CreateICmpEQ(b.mm, llvm::ConstantInt::getAllOnesValue(int_ty(w)));
                llvm::Value* bad  = b_.CreateOr(dz, neg1);
                llvm::Value* safe = b_.CreateSelect(bad, llvm::ConstantInt::get(int_ty(w), 1), b.mm);
                r = is_div ? b_.CreateSelect(neg1, b_.CreateNeg(a.mm), b_.CreateSDiv(a.mm, safe))
                           : b_.CreateSelect(neg1, zero(w), b_.CreateSRem(a.mm, safe));
            }
            return two_val(b_.CreateSelect(dz, zero(w), r));
        }

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
        llvm::Value* unknown = b_.CreateICmpNE(xz_mask(a, b, w), zero(w));
        llvm::Value* divzero = b_.CreateICmpEQ(bm, zero(w));
        llvm::Value* cold    = b_.CreateOr(unknown, divzero);

        auto* x_bb   = llvm::BasicBlock::Create(ctx_, "divmod.x", fn_);
        auto* val_bb = llvm::BasicBlock::Create(ctx_, "divmod.val", fn_);
        auto* end_bb = llvm::BasicBlock::Create(ctx_, "divmod.end", fn_);

        llvm::BranchInst* br = b_.CreateCondBr(cold, x_bb, val_bb);
        br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

        b_.SetInsertPoint(x_bb);
        llvm::Value* x_word = x_mm(w);
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
        mm->addIncoming(x_word, x_bb);
        mm->addIncoming(val_mm, val_bb);
        return { mm, zero(w) };
    }

    // Power (**). LRM 1800-2023 §11.4.3: the exponent is self-determined (so it may
    // be a different width from the base/result), any X/Z bit makes the result X, and
    // the positive-exponent table corners (exp 0 -> 1, 0**n -> 0, 1**n -> 1) fall out
    // of exponentiation by squaring. Two's-complement multiply is sign-agnostic, so
    // the same loop is bit-correct for a signed base at a non-negative exponent.
    //
    // A negative exponent is reachable only when the exponent type is signed; it
    // triggers Table 11-4's negative-op2 corners, computed branchlessly (op1==0 -> X,
    // op1==1 -> 1, signed op1==-1 -> parity of op2, else -> 0). base_signed gates the
    // op1==-1 case: an unsigned all-ones is a large positive, so it takes the `else`.
    Plane lower_pow(Plane a, Plane b, bool base_signed, bool exp_signed) {
        const uint32_t w  = width_of(a);
        const uint32_t ew = width_of(b);

        // 2-state: reuse the 4-state exponentiation-by-squaring loop on the lifted
        // operands (no X bits, so the loop's cold X block folds away), then project
        // the definite result back to a single iW. Avoids duplicating the delicate
        // loop; a 0**neg -> X result projects to 0 via must_of (LRM 6.3.1).
        if (a.two) {
            return two_val(must_of(lower_pow(to_four(a), to_four(b), base_signed, exp_signed)));
        }

        // Past the a.two return the base is 4-state, but the exponent may still be
        // 2-state (mixed domains: a 4-state base with a 2-state exponent, e.g. the
        // common `x ** 3` where a bare literal is 2-state). The 4-state paths below
        // read the exponent's packed i(2*ew) word, so lift it — otherwise the
        // X-mask's `b.mm >> ew` shifts an iW value by its full width (poison).
        b = to_four(b);

        // Width-1 base and exponent, both unsigned: index the truth table (exp 0 -> 1,
        // 1**n -> 1, 0**(n>0) -> 0, any X -> X). The LUT is unsigned-only; a signed
        // operand falls through to the general path (which handles the -1 corner). A
        // wider exponent stays on the runtime path.
        if (w == 1 && ew == 1 && !base_signed && !exp_signed) {
            return single_bit_lut(POW1_LUT, a, b);
        }

        auto* wide = int_ty(2 * w);
        auto* wty  = int_ty(w);
        auto* ety  = int_ty(ew);

        // X/Z in either operand -> all-X. The operands can differ in width, so each
        // mask is tested at its own width and the two feed one cold branch.
        llvm::Value* xa      = b_.CreateXor(a.mm, b_.CreateLShr(a.mm, w));
        llvm::Value* xb      = b_.CreateXor(b.mm, b_.CreateLShr(b.mm, ew));
        llvm::Value* ua      = b_.CreateICmpNE(b_.CreateTrunc(xa, int_ty(w)), zero(w));
        llvm::Value* ub      = b_.CreateICmpNE(b_.CreateTrunc(xb, int_ty(ew)), zero(ew));
        llvm::Value* unknown = b_.CreateOr(ua, ub);

        auto* x_bb    = llvm::BasicBlock::Create(ctx_, "pow.x", fn_);
        auto* known_bb = llvm::BasicBlock::Create(ctx_, "pow.known", fn_);
        auto* val_bb  = llvm::BasicBlock::Create(ctx_, "pow.val", fn_);
        auto* end_bb  = llvm::BasicBlock::Create(ctx_, "pow.end", fn_);

        llvm::BranchInst* br = b_.CreateCondBr(unknown, x_bb, known_bb);
        br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

        b_.SetInsertPoint(x_bb);
        llvm::Value* x_word = x_mm(w);
        b_.CreateBr(end_bb);

        // Operands known. Seeds are materialized here so they dominate both the loop
        // (non-negative exponent) and the negative-exponent corner block below.
        b_.SetInsertPoint(known_bb);
        llvm::Value* base0 = must_of(a);
        llvm::Value* exp0  = must_of(b);

        // Negative-exponent corners (Table 11-4), reachable only for a signed
        // exponent. Branchless: is0 -> X, is1 or (-1 with even op2) -> 1, -1 with
        // odd op2 -> all-ones (-1), otherwise 0.
        llvm::BasicBlock* neg_bb = nullptr;
        llvm::Value*      neg_mm = nullptr;
        if (exp_signed) {
            neg_bb = llvm::BasicBlock::Create(ctx_, "pow.neg", fn_);
            b_.CreateCondBr(b_.CreateICmpSLT(exp0, zero(ew)), neg_bb, val_bb);

            b_.SetInsertPoint(neg_bb);
            llvm::Value* all_ones = llvm::ConstantInt::getAllOnesValue(wty);
            llvm::Value* is0      = b_.CreateICmpEQ(base0, zero(w));
            llvm::Value* is1      = b_.CreateICmpEQ(base0, llvm::ConstantInt::get(wty, 1));
            llvm::Value* ism1     = base_signed ? b_.CreateICmpEQ(base0, all_ones) : b_.getFalse();
            llvm::Value* odd      = b_.CreateTrunc(exp0, b_.getInt1Ty());
            llvm::Value* one_like = b_.CreateOr(is1, b_.CreateAnd(ism1, b_.CreateNot(odd)));
            llvm::Value* negone   = b_.CreateAnd(ism1, odd);
            llvm::Value* val      = b_.CreateSelect(
              negone, all_ones, b_.CreateSelect(one_like, llvm::ConstantInt::get(wty, 1), zero(w)));
            neg_mm = b_.CreateSelect(is0, x_mm(w), packed(val, val, zero(w)).mm);
            b_.CreateBr(end_bb);

            b_.SetInsertPoint(val_bb);
        } else {
            b_.CreateBr(val_bb);
            b_.SetInsertPoint(val_bb);
        }

        // Hot: exponentiation by squaring at the result width W. Every product wraps
        // modulo 2^W, the correct result for width W; the wide (>64-bit) multiply
        // legalizes to the __multi3 libcall resolved in the JIT. The exponent keeps
        // its self-determined width so wide exponents are not truncated. The loop
        // always runs once (exp 0 -> one no-op iteration leaving result 1), which
        // yields the non-negative table corners directly.
        auto* loop = llvm::BasicBlock::Create(ctx_, "pow.loop", fn_);
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
        llvm::PHINode* mm = b_.CreatePHI(wide, neg_bb ? 3 : 2);
        mm->addIncoming(x_word, x_bb);
        mm->addIncoming(val_mm, loop);
        if (neg_bb) {
            mm->addIncoming(neg_mm, neg_bb);
        }
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

        // 2-state: one native bitwise op; xnor is ~(xor). Both operands must be
        // 2-state for this -- a 4-state/2-state mix (LRM 5.7: any 4-state
        // operand makes the result 4-state) still needs the packed-plane path
        // below, which assumes both operands already arrived as i(2W). Promote
        // the narrower side instead of trusting the dispatch on `a` alone -- a
        // mismatched-domain pair here is malformed LLVM IR, not just a wrong
        // answer.
        if (a.two && !b.two) {
            a = to_four(a);
        } else if (!a.two && b.two) {
            b = to_four(b);
        } else if (a.two) {
            switch (tag) {
                case Inst::Tag::Band:  return two_val(b_.CreateAnd(a.mm, b.mm));
                case Inst::Tag::Bor:   return two_val(b_.CreateOr(a.mm, b.mm));
                case Inst::Tag::Bxor:  return two_val(b_.CreateXor(a.mm, b.mm));
                default:               return two_val(b_.CreateNot(b_.CreateXor(a.mm, b.mm))); // Bxnor
            }
        }
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
        // path.
        llvm::Value* u     = xz_mask(a, b, w);
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
        // 2-state: a plain complement.
        if (a.two) {
            return two_val(b_.CreateNot(a.mm));
        }
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
        // Definite 4-state 1-bit result (compute_type); a 2-state operand lifts and
        // the any_unknown terms fold away, leaving the native reduction.
        a = to_four(a);
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

    // $clog2 (LRM §20.8.1): ceil(log2(operand)), operand unsigned, $clog2(0) == 0,
    // -> a 32-bit `integer`. For a known operand of width W, the ceiling equals the
    // bit index one past the top set bit of `operand - 1`, i.e. `W - clz(operand-1)`;
    // `operand == 0` is special-cased to 0 (it would otherwise wrap to all-ones and
    // yield W). Any x/z bit in the operand makes the whole result X, matching
    // iverilog (the LRM does not define x/z operands here).
    // Ctz / Clz: index of the lowest / highest definite-1 bit of the operand's
    // must plane, or the operand width when it has none. x/z bits (must = 0)
    // never count as set, matching `bit === 1'b1`. The result is always definite
    // (the must plane is known), so no X-propagation.
    Plane lower_bit_index(Inst::Tag tag, Plane a) {
        const uint32_t w   = width_of(a);
        llvm::Value*   val = must_of(a);  // definite-1 bits
        llvm::Value*   idx = nullptr;
        if (tag == Inst::Tag::Ctz) {
            // cttz(val, zero_poison=false) returns w when val == 0 (no set bit).
            idx = b_.CreateIntrinsic(llvm::Intrinsic::cttz, { int_ty(w) }, { val, b_.getFalse() });
        }
        else {
            // Highest set bit index = (w-1) - clz(val); w when val == 0.
            llvm::Value* clz = b_.CreateIntrinsic(llvm::Intrinsic::ctlz, { int_ty(w) }, { val, b_.getFalse() });
            llvm::Value* hb  = b_.CreateSub(llvm::ConstantInt::get(int_ty(w), w - 1), clz);
            idx = b_.CreateSelect(b_.CreateICmpEQ(val, zero(w)), llvm::ConstantInt::get(int_ty(w), w), hb);
        }
        return two_val(cast(idx, 32, false));
    }

    // $countbits / $countones / $onehot* / $isunknown (LRM 20.9): count the
    // operand bits whose 4-state value is selected by `mask` (a CountBitsClass
    // OR-mask). Per-bit codes from the planes: definite 1 = must; definite 0 =
    // ~may; unknown = may & ~must; z = unknown & z-plane; x = unknown & ~z. The
    // selected classes (disjoint per bit) OR together, then popcount. The result
    // is definite (2-state) — counting never propagates X.
    // 32-bit definite popcount of the operand bits whose 4-state class is in
    // `mask` (LRM 20.9).
    llvm::Value* count_bits_pop(Plane a, uint32_t mask) {
        a = to_four(a);
        const uint32_t w       = width_of(a);
        llvm::Value*   m       = must_of(a);
        llvm::Value*   M       = may_of(a);
        llvm::Value*   z       = z_of(a);
        llvm::Value*   unknown = b_.CreateAnd(M, b_.CreateNot(m));
        llvm::Value*   sel     = zero(w);
        if (mask & CountZero) {
            sel = b_.CreateOr(sel, b_.CreateNot(M));
        }
        if (mask & CountOne) {
            sel = b_.CreateOr(sel, m);
        }
        if (mask & CountX) {
            sel = b_.CreateOr(sel, b_.CreateAnd(unknown, b_.CreateNot(z)));
        }
        if (mask & CountZ) {
            sel = b_.CreateOr(sel, b_.CreateAnd(unknown, z));
        }
        llvm::Value* pop = b_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, sel);
        return cast(pop, 32, false);
    }

    Plane lower_count_bits(Plane a, uint32_t mask) {
        return two_val(count_bits_pop(a, mask));
    }

    // $onehot / $onehot0 (LRM 20.9): popcount of the definite-1 bits compared
    // against 1. x/z bits are never definite 1, so the result is definite.
    Plane lower_onehot(Plane a, bool allow_zero) {
        llvm::Value* pop = count_bits_pop(a, CountOne);
        llvm::Value* r   = allow_zero ? b_.CreateICmpULE(pop, b_.getInt32(1))
                                      : b_.CreateICmpEQ(pop, b_.getInt32(1));
        return two_val(r);
    }

    // $isunknown (LRM 20.9): the union of the x and z classes is exactly the
    // unknown plane, so this is one reduction, not a popcount-compare.
    Plane lower_isunknown(Plane a) {
        a = to_four(a);
        llvm::Value* unknown = b_.CreateAnd(may_of(a), b_.CreateNot(must_of(a)));
        return two_val(b_.CreateICmpNE(unknown, zero(width_of(a))));
    }

    Plane lower_clog2(Plane a) {
        const uint32_t w   = width_of(a);
        llvm::Value*   val = must_of(a); // operand treated as unsigned

        // W - clz(val - 1), computed in a width that holds the count (<= W) and the
        // 32-bit result; ctlz's zero-is-poison flag is false so val == 1 (val-1 == 0)
        // yields clz == W and thus a count of 0.
        const uint32_t cw    = std::max(w, 32u);
        llvm::Value*   vm1   = b_.CreateSub(val, llvm::ConstantInt::get(int_ty(w), 1));
        llvm::Value*   clz   = b_.CreateIntrinsic(llvm::Intrinsic::ctlz, { int_ty(w) }, { vm1, b_.getFalse() });
        llvm::Value*   count = b_.CreateSub(llvm::ConstantInt::get(int_ty(cw), w), cast(clz, cw, false));
        llvm::Value*   is_zero = b_.CreateICmpEQ(val, zero(w));
        llvm::Value*   known   = b_.CreateSelect(is_zero, zero(cw), count);
        llvm::Value*   res32   = cast(known, 32, false);

        // X-propagate: any unknown operand bit -> all-X result (must = 0, may = 1s).
        llvm::Value* unknown = any_unknown(a);
        llvm::Value* must    = b_.CreateSelect(unknown, zero(32), res32);
        llvm::Value* may     = b_.CreateSelect(unknown, ones(32), res32);
        return packed(must, may, zero(32));
    }

    // -- Shifts: X/Z in the count makes the whole result X; value planes shift. --

    Plane lower_shift(Inst::Tag tag, Plane a, Plane amt, bool sign) {
        const uint32_t w  = width_of(a);
        const uint32_t aw = width_of(amt);

        // 2-state value: one native shift with the same out-of-range handling. An
        // x/z in the (self-determined) amount makes the result x, which collapses
        // to 0 in a 2-state result.
        if (a.two) {
            const bool     arith = tag == Inst::Tag::Sra && sign;
            const uint32_t cw    = std::max(aw, w);
            llvm::Value*   av    = must_of(amt);
            llvm::Value*   oob   = b_.CreateICmpUGE(cast(av, cw, false), llvm::ConstantInt::get(int_ty(cw), w));
            llvm::Value*   safe =
              b_.CreateSelect(oob, llvm::ConstantInt::get(int_ty(w), w == 0 ? 0 : w - 1), cast(av, w, false));
            llvm::Value* r;
            switch (tag) {
                case Inst::Tag::Sll: r = b_.CreateShl(a.mm, safe); break;
                case Inst::Tag::Srl: r = b_.CreateLShr(a.mm, safe); break;
                default:             r = arith ? b_.CreateAShr(a.mm, safe) : b_.CreateLShr(a.mm, safe); break;
            }
            // Logical shifts (and unsigned >>>) zero on out-of-range; arithmetic
            // >>> already yields sign fill from the clamp to w-1.
            if (!arith) {
                r = b_.CreateSelect(oob, zero(w), r);
            }
            return two_val(b_.CreateSelect(any_unknown(amt), zero(w), r));
        }

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

    // -- Comparisons -> 1-bit. X rules differ per operator class (see
    // lower_compare): ==/!= are X only when genuinely ambiguous (LRM 11.4.5),
    // ordered relationals are X whenever either operand has any x/z (11.4.4). --

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

    // Whole-aggregate `==` (LRM 11.2.2): recursively AND-reduce per-leaf
    // logical equality over the member walk. Any definite member mismatch is
    // 0, else X if any member is unknown, else 1 — the Land reduction carries
    // LRM 11.4.5 X-propagation exactly.
    Plane lower_agg_eq(const LValue& lhs, const LValue& rhs, Ref type) {
        const Type& t = std::get<Type>(mod_.pool.get(type));
        if (t.kind == Type::Kind::Struct && !t.is_packed) {
            const auto fields = mod_.pool.struct_fields(t.fields_id);
            Plane      acc{};
            for (uint32_t k = 0; k < fields.size(); ++k) {
                const Plane e = lower_agg_eq(agg_member(lhs, type, k), agg_member(rhs, type, k),
                                             fields[k].type);
                acc = k == 0 ? e : lower_logical(Inst::Tag::Land, acc, e);
            }
            return acc;
        }
        if (t.kind == Type::Kind::String) {
            llvm::Value* c = b_.CreateCall(str_cmp_fn(), { lv_string_ptr(lhs), lv_string_ptr(rhs) });
            llvm::Value* bit = b_.CreateICmpEQ(c, b_.getInt32(0));
            return bit_plane(bit, bit);
        }
        const ValType vt = from_ref(type);
        Plane         pa = lower_load(lhs);
        Plane         pb = lower_load(rhs);
        if (vt.is_real) {
            pa = real_plane(planes_to_real(pa, vt.width));
            pb = real_plane(planes_to_real(pb, vt.width));
        }
        return lower_compare(Inst::Tag::Eq, pa, pb, vt.is_signed);
    }

    Plane lower_compare(Inst::Tag tag, Plane a, Plane b, bool sign) {
        // The result is always a definite 4-state bit (compute_type), so lift a
        // 2-state operand into the packed encoding; with no x/z the any_unknown
        // guards below fold to a single native icmp.
        a = to_four(a);
        b = to_four(b);

        // Equality keeps a branchless mask form (and a width-1 LUT). LRM 11.4.5:
        // the result is X only if unknown bits make the relation AMBIGUOUS. A bit
        // position where both operands are known and differ decides it outright
        // (== is 0, != is 1) no matter what the x/z bits are; only "no definite
        // difference but some x/z bit" yields X. Unknown bits are exactly
        // must != may (z included), so with 2-state operands `unk` is the zero
        // constant and everything below folds to a single native icmp.
        if (tag == Inst::Tag::Eq || tag == Inst::Tag::Ne) {
            if (width_of(a) == 1) {
                // One bit position: any x/z operand bit is ambiguous, which is
                // exactly the LUT's "either X -> X" rule.
                return single_bit_lut(tag == Inst::Tag::Eq ? EQ1_LUT : NE1_LUT, a, b);
            }
            const uint32_t w    = width_of(a);
            llvm::Value*   am   = must_of(a);
            llvm::Value*   bm   = must_of(b);
            llvm::Value*   unk  = b_.CreateOr(b_.CreateXor(am, may_of(a)), b_.CreateXor(bm, may_of(b)));
            llvm::Value*   diff = b_.CreateICmpNE(b_.CreateAnd(b_.CreateXor(am, bm), b_.CreateNot(unk)), zero(w));
            llvm::Value*   ambiguous = b_.CreateAnd(b_.CreateNot(diff), b_.CreateICmpNE(unk, zero(w)));
            // diff -> definite 0/1; else ambiguous -> X; else all bits known
            // equal -> definite 1/0.
            if (tag == Inst::Tag::Eq) {
                llvm::Value* eq = b_.CreateNot(diff);
                return bit_plane(b_.CreateAnd(eq, b_.CreateNot(ambiguous)), eq);
            }
            return bit_plane(diff, b_.CreateOr(diff, ambiguous));
        }

        // Ordered relationals: any x/z yields X, exactly like arithmetic, so guard
        // on it. The hot block is one native compare on the data (must) bits; the
        // cold block is the all-X word (must 0, may 1).
        //
        // NOTE: Unlike ==/!=, this pessimism is normative, not a shortcut. LRM
        // 11.4.4: "If either operand of a relational operator contains an
        // unknown (x) or high-impedance (z) value, then the result shall be
        // 1'bx" — even when known high bits alone would decide the relation
        // (iverilog agrees). Do not "sharpen" this path.
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
        // The result is a definite 4-state bit; lift operands to the packed form the
        // masking below needs (z / may planes materialized). 2-state operands carry
        // no x/z, so the case forms collapse to a plain == after folding.
        a = cast_plane(to_four(a), w, false);
        b = cast_plane(to_four(b), w, false);

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
    // wildcards (that bit position is not compared); the remaining positions are
    // compared "as for the logical equality" operator, i.e. with 11.4.5's
    // ambiguity rule. Masking both operands to the kept (non-wildcard) bits and
    // running `==` gives exactly that: the right is known on kept bits so only a
    // left x/z can make a kept position unknown, and a definite mismatch on any
    // kept position still decides the result.
    // `inside` (LRM 11.4.13): OR-reduce the wildcard-eq singular members and
    // the >=/<= range members. lower_logical carries the LRM 11.4.5
    // X-propagation of the reduction.
    Plane lower_inside_set(Index i) {
        const auto& p    = ir_->payload(i).op_extra;
        const auto  info = ir_->extra_data<InsideSetInfo>(p.extra);
        const Plane sel  = value_of(p.op);
        const bool  sign = type_of(p.op).is_signed;

        std::optional<Plane> acc;
        const auto add = [&](Plane pred) {
            acc = acc ? lower_logical(Inst::Tag::Lor, *acc, pred) : pred;
        };
        for (const Index v : ir_->region(info.values)) {
            add(lower_wildeq(sel, value_of(v)));
        }
        const std::span<const Index> rngs = ir_->region(info.ranges);
        for (size_t k = 0; k + 1 < rngs.size(); k += 2) {
            const Plane ge = lower_compare(Inst::Tag::Ge, sel, value_of(rngs[k]), sign);
            const Plane le = lower_compare(Inst::Tag::Le, sel, value_of(rngs[k + 1]), sign);
            add(lower_logical(Inst::Tag::Land, ge, le));
        }
        return *acc;
    }

    Plane lower_wildeq(Plane a, Plane b) {
        const uint32_t w     = std::max(width_of(a), width_of(b));
        // Definite 4-state bit result; lift operands. 2-state operands have no
        // wildcard (x/z) bits, so ==? folds to a plain ==.
        a                    = cast_plane(to_four(a), w, false);
        b                    = cast_plane(to_four(b), w, false);
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
        // Definite 4-state 1-bit result; lift operands (self-determined, so either
        // may be 2-state) into the packed encoding. With no x/z the truth of each
        // operand is just "nonzero" and the body folds to a native boolean op.
        a = to_four(a);
        b = to_four(b);

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
        // Definite 4-state 1-bit result; lift a 2-state operand (!a folds to a==0).
        a = to_four(a);
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

    // -- Select: known condition selects (one packed select); unknown merges bitwise
    // (X where the arms disagree). --

    Plane lower_select(Plane cond, Plane a, Plane b) {
        // 2-state arms: a native select. slang unifies the two arms to a common
        // type, so both are 2-state here. A 4-state condition may be x (LRM
        // 1800-2023 11.4.11: bitwise merge, x where the arms disagree); in a
        // 2-state result that x collapses to 0, which is exactly `a & b`.
        if (a.two && b.two) {
            llvm::Value* ct  = b_.CreateICmpNE(must_of(cond), zero(1));
            llvm::Value* sel = b_.CreateSelect(ct, a.mm, b.mm);
            if (cond.two) {
                return two_val(sel);
            }
            llvm::Value* known = b_.CreateICmpEQ(must_of(cond), may_of(cond));
            return two_val(b_.CreateSelect(known, sel, b_.CreateAnd(a.mm, b.mm)));
        }
        // Defensive: a lone 2-state arm is lifted so the packed body's direct plane
        // access is uniform.
        a = to_four(a);
        b = to_four(b);

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

        // 2-state result: every part is 2-state (a concat is 4-state if any part
        // is). Assemble the single iW word directly.
        if (!type_of(i).four_state) {
            llvm::Value* acc   = zero(w);
            bool         first = true;
            for (Index part : parts) {
                const uint32_t pw = type_of(part).width;
                if (!first) {
                    acc = b_.CreateShl(acc, pw);
                }
                acc   = b_.CreateOr(acc, cast(must_of(value_of(part)), w, false));
                first = false;
            }
            return two_val(acc);
        }

        llvm::Value* must = zero(w);
        llvm::Value* may  = zero(w);
        llvm::Value* z    = zero(w);
        // The first part is OR'd in without a preceding shift: a shift by the
        // full result width (which happens when the concat has a single operand,
        // pw == w) is poison in LLVM, and shifting the still-zero accumulator is
        // a no-op anyway. A 2-state part contributes must == may and a zero z
        // plane (z_of), so it packs into the 4-state result unchanged.
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
            z     = b_.CreateOr(z, cast(z_of(pv), w, false));
            first = false;
        }
        return packed(must, may, z);
    }

    // {<<S{x}}: reverse the order of the operand's S-bit blocks, sliced from
    // the LSB (a final partial block keeps its own width; block 0 lands at the
    // MSB). Applied to each plane independently — a pure bit permutation.
    llvm::Value* reverse_blocks(llvm::Value* v, uint32_t w, uint32_t slice) {
        const uint32_t n     = (w + slice - 1) / slice;
        llvm::Value*   acc   = zero(w);
        bool           first = true;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t lo = i * slice;
            const uint32_t bw = std::min<uint32_t>((i + 1) * slice, w) - lo;
            llvm::Value*   blk =
              b_.CreateAnd(b_.CreateLShr(v, lo),
                           llvm::ConstantInt::get(ctx_, llvm::APInt::getLowBitsSet(w, bw)));
            // Skip the shift before the first block: for a single block bw == w,
            // and a shift by the full width is poison in LLVM.
            if (!first) {
                acc = b_.CreateShl(acc, bw);
            }
            acc   = b_.CreateOr(acc, blk);
            first = false;
        }
        return acc;
    }

    Plane lower_stream_reverse(Plane v, uint32_t slice) {
        const uint32_t w = width_of(v);
        if (v.two) {
            return two_val(reverse_blocks(v.mm, w, slice));
        }
        return packed(reverse_blocks(must_of(v), w, slice), reverse_blocks(may_of(v), w, slice),
                      reverse_blocks(z_of(v), w, slice));
    }

    Plane lower_repl(Plane v, uint32_t part_width, uint32_t count) {
        // TODO: a zero replication count ({0{x}}) should yield a 0-width value;
        // here w becomes 0 and int_ty(0) is widened to i1, so it leaks one bit.
        const uint32_t w    = part_width * count;

        // 2-state: replicate the single word.
        if (v.two) {
            llvm::Value* p   = cast(v.mm, w, false);
            llvm::Value* acc = zero(w);
            for (uint32_t c = 0; c < count; ++c) {
                if (c != 0) {
                    acc = b_.CreateShl(acc, part_width);
                }
                acc = b_.CreateOr(acc, p);
            }
            return two_val(acc);
        }

        llvm::Value*   pm   = cast(must_of(v), w, false);
        llvm::Value*   pM   = cast(may_of(v), w, false);
        llvm::Value*   pz   = cast(z_of(v), w, false);
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
        // A cold branch (violation check) biases weights toward the else path so
        // codegen lays the then-block out of line.
        llvm::MDNode* weights =
          regs.unlikely ? llvm::MDBuilder(ctx_).createBranchWeights(1, 2000) : nullptr;
        b_.CreateCondBr(take, then_bb, else_bb, weights);

        b_.SetInsertPoint(then_bb);
        lower_region(ir_->region(regs.then_));
        branch_to(cont_bb);

        b_.SetInsertPoint(else_bb);
        lower_region(ir_->region(regs.else_));
        branch_to(cont_bb);

        b_.SetInsertPoint(cont_bb);
    }

    // Multiway `case` -> llvm switch. Packs the selector planes into an integer
    // match key: for a 2-state selector the key is the bare value; for 4-state
    // it is {may^must, must} with `must` on the low W bits. A clean value keys
    // on its low W bits alone (its unknown half is zero), so the clean cases sit
    // densely in [0, 2^W) and LLVM can build a jump table; any x/z selector sets
    // a bit in the high half, landing outside every case value and falling to
    // the default. This exactly reproduces `===`: the frontend supplies each
    // clean label's value as its case key and guarantees the keys are unique.
    void lower_switch(Index i) {
        const Inst::Payload& p    = ir_->payload(i);
        const auto           info = ir_->extra_data<SwitchInfo>(p.op_extra.extra);
        const Plane          sel  = value_of(p.op_extra.op);

        const uint32_t w    = width_of(sel);
        llvm::Value*   must = must_of(sel);
        llvm::Value*   key  = must;
        uint32_t       keyw = w;
        if (!sel.two) {
            keyw            = 2 * w;
            llvm::Type* kty = int_ty(keyw);
            llvm::Value* u  = b_.CreateXor(must, may_of(sel));
            key = b_.CreateOr(b_.CreateZExt(must, kty), b_.CreateShl(b_.CreateZExt(u, kty), w));
        }
        llvm::Type* kty = int_ty(keyw);

        const std::span<const Index> arm_words = ir_->region(info.arms);
        constexpr uint32_t           arm_len   = sizeof(SwitchArm) / sizeof(uint32_t);
        const uint32_t               n_arms    = static_cast<uint32_t>(arm_words.size()) / arm_len;

        auto* default_bb = llvm::BasicBlock::Create(ctx_, "case.default", fn_);
        auto* cont_bb    = llvm::BasicBlock::Create(ctx_, "case.end", fn_);

        llvm::SwitchInst* sw = b_.CreateSwitch(key, default_bb, n_arms);
        for (uint32_t a = 0; a < n_arms; ++a) {
            const auto arm = ir_->extra_data<SwitchArm>(
              static_cast<ExtraIndex>(static_cast<uint32_t>(info.arms.start) + a * arm_len));
            auto*      arm_bb = llvm::BasicBlock::Create(ctx_, "case.arm", fn_);

            const std::span<const Index> kw = ir_->region(arm.keys);
            for (size_t k = 0; k + 1 < kw.size(); k += 2) {
                const uint64_t val = static_cast<uint64_t>(kw[k]) | (static_cast<uint64_t>(kw[k + 1]) << 32);
                sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(kty), val), arm_bb);
            }

            b_.SetInsertPoint(arm_bb);
            lower_region(ir_->region(arm.body));
            branch_to(cont_bb);
        }

        b_.SetInsertPoint(default_bb);
        lower_region(ir_->region(info.default_));
        branch_to(cont_bb);

        b_.SetInsertPoint(cont_bb);
    }

    // Value-producing short-circuit conditional (&&/||). Branches on the
    // predicate's must bit (a definite 2-state value), lowers only the taken
    // branch's region, and merges the two branch values with a PHI. The
    // side-effecting right operand lives inside the then-region, so it is never
    // evaluated when the predicate is 0. LLVM folds the whole thing back to a
    // straight-line select when both branches are side-effect free.
    void lower_select_lazy(Index i) {
        const Inst::Payload& p    = ir_->payload(i);
        const auto           sel  = ir_->extra_data<SelectLazy>(p.op_extra.extra);
        const Plane          pred = value_of(p.op_extra.op);

        llvm::Value* take = b_.CreateICmpNE(must_of(pred), zero(1));

        auto* then_bb = llvm::BasicBlock::Create(ctx_, "sel.then", fn_);
        auto* else_bb = llvm::BasicBlock::Create(ctx_, "sel.else", fn_);
        auto* cont_bb = llvm::BasicBlock::Create(ctx_, "sel.end", fn_);
        b_.CreateCondBr(take, then_bb, else_bb);

        // The two arms may disagree in Plane domain (a 2-state constant against a
        // 4-state Land/Lor result), so each is normalized to the instruction's own
        // domain inside its block — the PHIs need identical value types.
        const bool four = type_of(i).four_state;
        const auto norm = [&](Index v) { return four ? to_four(value_of(v)) : Plane{ must_of(value_of(v)), nullptr, true }; };

        b_.SetInsertPoint(then_bb);
        lower_region(ir_->region(sel.then_));
        const Plane       tv       = norm(sel.then_val);
        llvm::BasicBlock* then_end = b_.GetInsertBlock();
        branch_to(cont_bb);

        b_.SetInsertPoint(else_bb);
        lower_region(ir_->region(sel.else_));
        const Plane       ev       = norm(sel.else_val);
        llvm::BasicBlock* else_end = b_.GetInsertBlock();
        branch_to(cont_bb);

        b_.SetInsertPoint(cont_bb);
        llvm::PHINode* mm = b_.CreatePHI(tv.mm->getType(), 2);
        mm->addIncoming(tv.mm, then_end);
        mm->addIncoming(ev.mm, else_end);
        if (!four) {
            set_value(i, two_val(mm));
            return;
        }
        llvm::PHINode* z = b_.CreatePHI(tv.z->getType(), 2);
        z->addIncoming(tv.z, then_end);
        z->addIncoming(ev.z, else_end);
        set_value(i, { mm, z });
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

        // do-while runs the body before the first test; every other loop tests
        // first. `continue` -> step, `break` -> exit, innermost loop only.
        b_.CreateBr(lp.bottom_test ? body_bb : head_bb);

        // Header: recompute the predicate, then test it. must=1 only for a
        // definite 1, so an X/Z condition exits the loop.
        b_.SetInsertPoint(head_bb);
        lower_region(ir_->region(lp.cond));
        llvm::Value* take = b_.CreateICmpNE(must_of(value_of(lp.pred)), zero(1));
        b_.CreateCondBr(take, body_bb, exit_bb);

        b_.SetInsertPoint(body_bb);
        loop_targets_.push_back({ step_bb, exit_bb });
        lower_region(ir_->region(lp.body));
        loop_targets_.pop_back();
        branch_to(step_bb); // a `return`/`break`/`continue` in the body terminates it; don't double-branch

        // Step, then the back-edge to re-test the condition.
        b_.SetInsertPoint(step_bb);
        lower_region(ir_->region(lp.step));
        b_.CreateBr(head_bb);

        b_.SetInsertPoint(exit_bb);
    }

    // -- State --

    const Module&           mod_;
    llvm::LLVMContext&      ctx_;
    // Root compilation unit (the module handed in by build_modules) and the
    // active unit all IR emission goes through — swapped to a definition
    // unit's env around each single-scope group's lowering, root otherwise.
    std::unique_ptr<ModuleEnv> root_env_;
    ModuleEnv*                 env_;
    // Definition-unit envs and their modules, keyed by definition state-struct
    // identity (one unit per elaborated definition content).
    std::vector<std::unique_ptr<ModuleEnv>>                       unit_envs_;
    std::vector<std::unique_ptr<llvm::Module>>                    unit_modules_;
    ankerl::unordered_dense::map<llvm::StructType*, ModuleEnv*>   unit_env_of_;
    llvm::IRBuilder<>       b_;
    // The root `%clotho.state` struct: unscoped signals only (instance scopes
    // own their own state globals, see Anchor::state_gv).
    llvm::StructType*       state_ty_ = nullptr;
    // The NBA shadow struct (valid iff non-null) and its standalone global.
    llvm::StructType*       shadow_ty_     = nullptr;
    llvm::GlobalVariable*   shadow_global_ = nullptr;
    // Global holding &shadow, the opaque base every shadow access goes
    // through (see shadow_ptr for the mis-speculation hazard it defuses).
    llvm::GlobalVariable*   shadow_base_gv_ = nullptr;
    // The root state global (@clotho.state, unscoped signals), created by
    // emit_state_global; also the vestigial `state` argument main threads
    // through the process/drain ABI.
    llvm::GlobalVariable*   state_global_ = nullptr;
    // Slot-type memo, one `{ i(2W)[, iW] }` per (width, domain, has_z).
    std::vector<std::tuple<uint32_t, bool, bool, bool, llvm::StructType*>> slot_types_;
    // Per-definition state-struct memo: (definition name, field types, type).
    // Instances of one definition share the type; a same-named definition with
    // different fields (different parameterization) gets its own entry.
    std::vector<std::tuple<std::string, std::vector<llvm::Type*>, llvm::StructType*>> def_types_;
    std::vector<SignalSlot> slots_;
    // Instance-scope anchors, one per scope occurrence (see Anchor); filled by
    // scope_type, symbols created by emit_state_global.
    std::vector<Anchor> anchors_;
    // InstanceScope::id -> anchor index (no_anchor when the id is unknown).
    std::vector<uint32_t> anchor_of_scope_;
    // Twin sets (process indices) recorded by report_dedup_stat under
    // CLOTHO_DEDUP_STAT=2, compared post-lowering by report_dedup_shapes.
    std::vector<std::vector<uint32_t>> shape_sets_;
    // Per fusion group: owning scope anchor (single-scope reused-def groups
    // only, else no_anchor) and def-local group ordinal. Built by
    // build_instance_ctx_layout; the shareable-group identity.
    std::vector<uint32_t> group_anchor_of_;
    std::vector<uint32_t> group_def_ordinal_;
    // Owning scope anchor of ANY single-scope group (unique or reused def),
    // no_anchor for cross-instance / unscoped groups: the split-emission unit
    // assignment (see unit_env / the lower_group loop in run()).
    std::vector<uint32_t> group_unit_anchor_;
    // Def-local signal table per anchor (slot.path's last element), kept from
    // build_wait_queues for the ctx layout and block-signal translation.
    std::vector<std::vector<Signal>> anchor_signals_;

    // VCD dirty set (meaningful iff design_uses_vcd_). vcd_index_[sig] is the
    // runtime dump-var index of a dumped scalar signal, or -1 when the signal is
    // not a touched scalar (an unnamed temporary, a string, or an unpacked array —
    // array elements are always-scanned, not instrumented). nba_vcd_gv_ is a
    // parallel [n_nba x i32] of those indices, read in the NBA commit loop.
    bool                  design_uses_vcd_ = false;
    std::vector<int32_t>  vcd_index_;
    llvm::GlobalVariable* nba_vcd_gv_ = nullptr;

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
    // Per-ref-formal actual pointer, indexed by Local index; null for non-ref
    // locals. Bound in the callee prologue (bind_ref_formals) from the passed
    // pointer parameter, then read by local_lvalue to alias the actual live.
    std::vector<llvm::Value*> ref_ptr_;
    // The current function's frame struct (one field per Local, in order).
    llvm::StructType*       frame_ty_    = nullptr;
    // While lowering a task: the shared copy-out + caller-resume epilogue block a
    // Ret branches to. The caller-side record slot { ptr parent } for task calls
    // this coroutine makes (a process or a task can be a task caller).
    llvm::BasicBlock*       task_epilogue_bb_ = nullptr;
    // Innermost-first stack of the enclosing loops' (step, exit) blocks; a
    // `continue` branches to `.first`, a `break` to `.second`.
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loop_targets_;
    llvm::AllocaInst*       record_           = nullptr;
    // Per-function fork join-record slot { i32 count, ptr parent } (lazy, reused
    // across a coroutine's fork sites — `join` serializes them). Reset per fn.
    llvm::AllocaInst*       fork_record_      = nullptr;
    llvm::StructType*       fork_record_ty_   = nullptr; // module-lived, built once

    // Per-process scratch, reset in lower_process.
    const Ir*           ir_         = nullptr;
    llvm::Function*     fn_         = nullptr;
    llvm::Value*        state_      = nullptr;
    llvm::Value*        wheel_      = nullptr;
    llvm::Value*        hdl_        = nullptr;
    llvm::Instruction*  coro_begin_ = nullptr;
    llvm::AllocaInst*   awaiter_    = nullptr;
    // The current process's $urandom RNG state: a [4 x i64] xoshiro256++ alloca
    // in the coroutine frame, seeded once in the ramp. Null for a process that
    // never calls $urandom (no state materialized) or a thunk (which the
    // $urandom-forces-coroutine rule keeps $urandom out of).
    llvm::Value*        rng_slot_   = nullptr;
    // Reused scratch for staging array-NBA value/mask bytes before a push call
    // (one per process, hoisted to coro.begin so a loop of element NBAs does not
    // grow the stack). Layout: [16]mm_val [16]mm_mask [8]z_val [8]z_mask.
    llvm::BasicBlock*   coro_cleanup_bb_ = nullptr;
    llvm::BasicBlock*   coro_ret_bb_     = nullptr;
    // The current coroutine's scheduled flag: &clotho.proc_sched[this process] for
    // a process (a Constant GEP), or a frame-resident i32 alloca for a task.
    llvm::Value*        sched_ptr_       = nullptr;
    // Deferred def-relative sched addressing chosen by lower_group (see
    // materialize_sched_ptr); no_anchor when the absolute constant is used.
    // sched_group_ is the fusion-group index currently lowering (feeds the
    // def-local blk-field ordinal in block_register).
    uint32_t sched_anchor_  = SignalSlot::no_anchor;
    uint32_t sched_ordinal_ = 0;
    uint32_t sched_group_   = 0;
    // First NBA block id of the group currently lowering (-1 until a block
    // opens); block_register keys its ctx-relative ids off it.
    int64_t group_first_block_ = -1;
    // (anchor, ctx field, first block id) triples main stores at startup.
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> ctx_blk_fills_;
    // The shared timer callback (pushes a woken handle onto the active queue) and
    // the active/inactive queue globals, all built once per module. The inactive
    // queue holds processes parked on a zero delay (#0): drained into active at
    // the same instant, after active settles but before NBA.
    llvm::Function*                     enqueue_fn_        = nullptr;
    llvm::GlobalVariable*               active_gv_         = nullptr;
    llvm::GlobalVariable*               active_head_gv_    = nullptr;
    llvm::GlobalVariable*               active_tail_gv_    = nullptr;
    llvm::GlobalVariable*               inactive_gv_       = nullptr;
    llvm::GlobalVariable*               inactive_top_gv_   = nullptr;
    llvm::ArrayType*                    queue_arr_ty_      = nullptr;
    uint32_t                            active_mask_       = 0;
    // Process indices in static-subscription order (compute_subscription_order).
    std::vector<uint32_t>               sub_order_;
    // Blocking-write wake graph (writer -> statically-subscribed reader) and
    // per-process membership in a nontrivial SCC (a multi-process
    // combinational loop), both retained by compute_subscription_order for
    // the comb-fusion pass in compute_fusion_groups.
    std::vector<std::vector<uint32_t>>  wake_adj_;
    std::vector<uint8_t>                scc_nontrivial_;
    // Sizes of the groups the comb-fusion pass formed (measurement only, for
    // the CLOTHO_COMB_FUSE_STAT probe).
    std::vector<uint32_t>               comb_group_sizes_;
    // Fusion groups (compute_fusion_groups): each group lowers to one
    // coroutine. groups_[g] lists member process indices in scheduled-rank
    // order; group_of_ maps a process to its group; group_events_[g] is the
    // (signal, edge) set the group subscribes (canonical deduped for fused
    // groups, the member's raw list for singletons).
    std::vector<std::vector<uint32_t>>  groups_;
    std::vector<uint32_t>               group_of_;
    std::vector<std::vector<WaitEvent>> group_events_;
    // Per-group: lower as a plain per-activation thunk (config_.thunk && the
    // group is suspension-free), not a coroutine. Drives lower_group's emission
    // and the seed/drain dispatch, kept as one table so all three agree.
    std::vector<bool>                   thunk_group_;
    // Dynamic domain specialization (compute_domain_spec). spec_group_: this
    // thunk group carries a second, assume-clean body chosen reader-side at
    // dispatch. group_reads_[g]: the group's tracked read set (4-state scalar
    // signal indices) the dispatch guard tests for cleanliness. assume_clean_
    // flips type_of/from_ref to report 4-state as 2-state while the fast body
    // lowers.
    bool                                assume_clean_ = false;
    std::vector<bool>                   spec_group_;
    std::vector<std::vector<uint32_t>>  group_reads_;
    // CLOTHO_DOM_DEBUG: count fast/generic dispatches per group
    // (clotho.dom_nfast / dom_ngen) and dump the read sets / rejections to
    // stderr — run_jit prints the runtime dispatch counts after the sim
    // (Jit.cpp). Diagnosis only; zero cost when the env var is unset (checked
    // at compile time). Requires --domain-spec to be on.
    bool                                dom_debug_ = false;
    llvm::GlobalVariable*               dom_nfast_gv_ = nullptr;
    llvm::GlobalVariable*               dom_ngen_gv_  = nullptr;
    llvm::ArrayType*                    dom_cnt_ty_   = nullptr;
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
    // True (pre-padding) per-instance subscriber counts, and whether the bucket
    // is padded past them to the definition maximum (see build_wait_queues).
    std::vector<std::array<uint32_t, 3>> wait_sta_true_;
    std::vector<std::array<uint8_t, 3>>  wait_sta_padded_;
    // Def-local wait-table slots per (signal, edge): ctx field ordinal (past
    // ctx_wait_base) in the owning instance's context, or ~0 when the signal's
    // owner is not a reused definition (body-side code then uses the absolute
    // queue global).
    static constexpr uint32_t no_wait_slot = ~0u;
    std::vector<std::array<uint32_t, 3>> wait_slot_sta_;
    std::vector<std::array<uint32_t, 3>> wait_slot_dyn_;
    std::vector<std::array<uint32_t, 3>> wait_slot_len_;

    std::vector<std::array<llvm::GlobalVariable*, 3>>  wait_dyn_gv_;
    std::vector<std::array<llvm::ArrayType*, 3>>       wait_dyn_ty_;
    std::vector<std::array<llvm::GlobalVariable*, 3>>  wait_dyn_len_gv_;
    // Next free static slot per (signal, edge), handed out during lowering
    // (parallels the capacities computed in build_wait_queues).
    std::vector<std::array<uint32_t, 3>>               sta_next_;
    // Per named-event trigger timestamp (clotho.event_trig.<name>), an i64 holding
    // the sim tick of the last `-> e`, or UINT64_MAX if never triggered. Indexed
    // by signal handle; null for a non-event signal. `.triggered` compares this to
    // the current tick (LRM 15.5.3). Built in build_wait_queues.
    std::vector<llvm::GlobalVariable*>                 event_trig_gv_;
    // Per named-event nonblocking-trigger pending flag (clotho.event_pending.<name>),
    // an i8 set by `->> e` and cleared after the NBA region stamps + services the
    // event (LRM 15.5.4). Indexed by signal handle; null for a non-event signal.
    // Built in build_wait_queues.
    std::vector<llvm::GlobalVariable*>                 event_pending_gv_;
    // Event signals that are targets of a `->> e` somewhere in the design; the NBA
    // region scans exactly these for a pending nonblocking trigger. Collected in
    // nba_classify_ir.
    std::vector<Signal>                                nb_event_sigs_;
    // Event signals that are targets of a delayed `->> #d e`; each gets a detached
    // on-expiry thunk (event_delay_thunk_). Collected in nba_classify_ir.
    std::vector<Signal>                                nb_delay_event_sigs_;
    // Per delayed-trigger event: the wheel-timer on_expire thunk that stamps the
    // trigger time and services the event's waiters. Indexed by signal handle;
    // null for a non-target. Built in build_delay_event_thunks.
    std::vector<llvm::Function*>                       event_delay_thunk_;
    // Per-process scheduled-flag placement (build_sched_flags): owning anchor
    // (~0 = root array) and dense def-local ordinal within that owner's array.
    std::vector<uint32_t>               proc_sched_anchor_;
    std::vector<uint32_t>               proc_sched_ord_;
    // Root scheduled-flag array (clotho.proc_sched) for unowned processes.
    llvm::GlobalVariable*               sched_gv_     = nullptr;
    llvm::ArrayType*                    sched_arr_ty_ = nullptr;

    // Nonblocking-assignment update state, built in build_nba_state(). A StoreNb
    // stages its RHS into the target's shadow and registers its block; the NBA region
    // commits the shadows into live state and services wait queues via nba_meta.
    uint32_t                n_nba_  = 0; // distinct NBA-target signals
    llvm::GlobalVariable*   nba_meta_gv_    = nullptr; // [n_nba x NbaMeta] constants
    llvm::ArrayType*        nba_meta_arr_ty_ = nullptr;
    llvm::StructType*       nba_meta_ty_    = nullptr;
    // Unpacked-array signals that are nonblocking-assignment targets. Their
    // per-element writes go through the runtime deferred queue (rt_nba_arr_*)
    // rather than the per-signal shadow; after a commit, each is woken if any
    // queued write touched it.
    std::vector<Signal>     array_nba_sigs_;
    // Aggregate signals with a nonblocking write reaching a leaf through a
    // runtime array index. Each such write defers a (dest-slot-pointer, nbytes,
    // payload) record into a per-aggregate runtime buffer (indexed after the
    // array buffers), replayed by memcpy in the NBA region; the signal is then
    // woken (Any-edge) if any record touched it.
    std::vector<Signal>     agg_nba_sigs_;

    // Whole-scalar targets of a nonblocking intra-assignment delay (`x <= #d y`,
    // StoreNbDelay). Each gets a dense id; a per-target record (rt_intra_*) holds
    // the sampled value on the wheel until `now + d`, then the NBA region commits
    // it (emit_intra_commit). See docs/design/intra-assignment-timing.md.
    std::vector<Signal>     intra_nba_sigs_;

    // Per (signal, edge): the dense trigger id if that signal+edge fires a
    // nonblocking `<= @e` deferred store (StoreNbEvent), else -1. A trigger
    // signal's edges must be watched (signal_has_waiters) even with no coroutine
    // waiter, so both wake paths (blocking store, NBA commit) fire its records.
    // Indexed [signal][edge]; empty when the design has no `<= @e`.
    std::vector<std::array<int32_t, 3>> trigger_id_;
    uint32_t                            n_triggers_ = 0;

    // Block-commit NBA state. A whole-signal scalar NBA stages its shadow as before,
    // then registers its enclosing straight-line segment ("block") once per instant
    // via nba_block_enqueue_fn_ onto the threaded chain (block_next / block_head). The
    // NBA region calls the chain head; each block's monomorphized commit function
    // (block_commits_[id].fn) copies that block's shadows -> live, services their
    // waiters, then musttail-calls the next pending block. Segments are keyed on
    // cur_seg_epoch_ so back-to-back fused members coalesce into one block.
    // One NBA target the block writes, with the commit mask: `dynamic` means the mask
    // is a runtime dirty field (sh_mask_field); otherwise `mask` is the compile-time
    // union of the block's static writes to the signal (all-ones for a whole target).
    struct BlockSig {
        Signal      sig;
        llvm::APInt mask;
        bool        dynamic = false;
    };
    struct BlockCommit {
        std::vector<BlockSig> sigs;         // targets, in first-store order
        llvm::Function*       fn = nullptr; // monomorphized commit, void(state)
    };
    std::vector<BlockCommit>                    block_commits_;
    // Source-IR straight-line segment counter, bumped at every structured-region
    // boundary (lower_region entry/exit) and in-region suspend (Delay / Wait /
    // TaskCall). NBAs sharing an epoch are unconditionally co-executed with no
    // suspend between, so they commit together safely; a block never spans a
    // conditional body or a suspend. Keyed on the epoch, NOT the LLVM basic block,
    // because 4-state RHS arithmetic splits the LLVM block mid-statement.
    uint32_t                cur_seg_epoch_   = 0;
    int64_t                 cur_block_epoch_ = -1;  // epoch cur_block_id_ was opened at (-1: none)
    uint32_t                cur_block_id_    = 0;
    llvm::Function*         nba_block_enqueue_fn_ = nullptr;    // void(i32 id), body emitted post-lowering
    llvm::GlobalVariable*   block_commit_fns_gv_  = nullptr;    // [n_block x ptr] commit fn table
    llvm::ArrayType*        block_commit_fns_ty_  = nullptr;
    // Threaded-dispatch state. An intrusive chain of pending blocks: block_next[id]
    // holds the next pending block's commit-fn pointer, `null` = chain end, sentinel
    // 1 (block_not_queued) = not enqueued (also the dedup marker). block_head points
    // at the first pending block's commit fn. Each commit fn musttail-calls its
    // successor; the NBA region just calls the head.
    llvm::GlobalVariable*   block_next_gv_        = nullptr;    // [n_block x ptr] intrusive links
    llvm::ArrayType*        block_next_ty_        = nullptr;
    llvm::GlobalVariable*   block_head_gv_        = nullptr;    // ptr: first pending block's commit fn

    std::vector<Plane>                 values_;
    std::vector<std::optional<LValue>> lvals_;
    // Per-instruction type_of memo. Validity lives in a parallel byte vector
    // instead of std::optional<ValType> (12B/entry from the engaged flag +
    // padding): entries stay 8B dense, and a body reset only clears the
    // bitmap — stale types_ values are gated by types_valid_ == 0.
    std::vector<ValType> types_;
    std::vector<uint8_t> types_valid_;
    // rt_string handle pointers of string-valued instructions, parallel to
    // values_ (a string never flows as a Plane).
    std::vector<llvm::Value*> str_vals_;
    // The current body's string rvalue temporaries, deinit'd at its exits.
    std::vector<llvm::Value*> str_temp_list_;
    // Memoized `%clotho.string` handle slot type.
    llvm::StructType* str_slot_ty_ = nullptr;
    // Memoized class object-body StructType per Kind::Class Ref (see class_body_ty).
    ankerl::unordered_dense::map<uint32_t, llvm::StructType*> class_body_ty_;
    // Memoized `%clotho.dynarr` handle slot type ({ ptr data, i64 len }).
    llvm::StructType* dynarr_slot_ty_ = nullptr;
    // Result-handle temporaries for dynamic-array / queue-returning calls in the
    // current body: alloca'd empty in the prologue, passed as the callee's trailing
    // out-param, and freed (by handle type) at every body exit.
    std::vector<std::pair<llvm::Value*, Ref>> dynarr_temp_list_;
    // Per-call-instruction result handle (null for non-dynarr-returning calls),
    // parallel to str_vals_ and snapshotted per fused member the same way — the
    // coroutine body lowering resets lvals_ via select_member, so the mapping
    // cannot live there.
    std::vector<llvm::Value*> dynarr_ret_;
    // Memoized `%clotho.queue` handle slot type ({ ptr base, i64 head, len, cap }).
    llvm::StructType* queue_slot_ty_ = nullptr;
    llvm::StructType* elem_ops_ty_ = nullptr;
};

// Lower the coroutine intrinsics emitted for each process and clean up the
// generated code (tier-0 pipeline: fast compile, no loop/GVN passes). CoroSplit
// (with CoroEarly/CoroCleanup) is mandatory: without it the llvm.coro.*
// intrinsics survive to codegen and the module won't compile. The pre-split
// sroa/early-cse/simplifycfg/instcombine run inside the CGSCC nest (after
// inlining, before split) so promoted values never reach the coroutine frame;
// the post-split function passes clean the resume functions, which do not exist
// earlier. instcombine is what folds the z-elision zero-plane traffic and the
// plane-extraction trunc/lshr chains; instsimplify is not enough (measured:
// no sim-time win). no-verify-fixpoint: LLVM 22
// errors if instcombine's single iteration does not converge on our
// non-canonical IR. Returns false (reason on errs()) only if the pipeline
// string is rejected.
bool lower_coroutines(llvm::Module& m, bool o3) {
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
    if (o3) {
        // Aggressive tier: LLVM's full default<O3> pipeline, which contains
        // the mandatory coro passes (CoroEarly / CoroSplit in the CGSCC nest /
        // CoroCleanup) plus the loop/GVN/vectorizer stack tier 0 omits.
        mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    }
    else if (auto err = pb.parsePassPipeline(mpm,
                                        "coro-early,"
                                        "cgscc(inline,function(sroa,early-cse,simplifycfg,instcombine<no-verify-fixpoint>),coro-split),"
                                        "function(simplifycfg,instcombine<no-verify-fixpoint>,dce),"
                                        "coro-cleanup")) {
        llvm::errs() << "llvm backend: bad coroutine pipeline: " << llvm::toString(std::move(err)) << "\n";
        return false;
    }
    mpm.run(m, mam);
    return true;
}

} // namespace

std::vector<std::unique_ptr<llvm::Module>> build_modules(const Module& m, llvm::LLVMContext& ctx,
                                                         const SimulationConfig& config) {
    auto root = std::make_unique<llvm::Module>(m.name.empty() ? "clotho" : m.name, ctx);

    // Pin the modules to the host target's triple and DataLayout before any IR
    // is built: the state/frame struct layouts, the NBA meta table's baked byte
    // offsets, and load/store alignments are all DataLayout-derived, so they
    // must be computed against the layout codegen will actually use. Unit
    // modules copy the root's triple/DataLayout at creation (unit_env).
    {
        static std::once_flag once;
        std::call_once(once, [] { llvm::InitializeNativeTarget(); });
        auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!jtmb) {
            llvm::errs() << "llvm backend: cannot detect host target: "
                         << llvm::toString(jtmb.takeError()) << "\n";
            return {};
        }
        auto dl = jtmb->getDefaultDataLayoutForTarget();
        if (!dl) {
            llvm::errs() << "llvm backend: no host data layout: " << llvm::toString(dl.takeError())
                         << "\n";
            return {};
        }
        root->setTargetTriple(jtmb->getTargetTriple());
        root->setDataLayout(*dl);
    }

    std::vector<std::unique_ptr<llvm::Module>> mods;
    try {
        Backend be(m, ctx, *root, config);
        be.run();
        mods = be.take_unit_modules();
    } catch (const BackendError& e) {
        llvm::errs() << "llvm backend error: " << e.message << "\n";
        return {};
    }
    mods.insert(mods.begin(), std::move(root));

    // Verify every freshly-built (pre-split) module. With optimize off, stop
    // here and hand back the raw modules (coro intrinsics intact).
    for (const auto& mod : mods) {
        if (llvm::verifyModule(*mod, &llvm::errs())) {
            return {};
        }
    }
    if (!config.optimize) {
        return mods;
    }
    // Lower the coroutines per module and verify again — the second pass
    // guards against malformed coro lowering.
    for (const auto& mod : mods) {
        if (!lower_coroutines(*mod, config.o3)) {
            return {};
        }
        if (llvm::verifyModule(*mod, &llvm::errs())) {
            return {};
        }
    }

    return mods;
}

bool emit_llvm(const Module& m, llvm::raw_ostream& os, const SimulationConfig& config) {
    llvm::LLVMContext ctx;
    auto              mods = build_modules(m, ctx, config);
    if (mods.empty()) {
        return false;
    }
    for (const auto& mod : mods) {
        mod->print(os, nullptr);
    }
    return true;
}

} // namespace clotho::ir
