// Internal to the LLVM backend: the Backend class and the value helpers its files share.
// Only src/ir/llvm/*.cpp include this.

#pragma once

#include "ir/LlvmBackend.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstring>
#include <format>
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
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ModuleSlotTracker.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>

#include "ir/Analysis.h"
#include "ir/InternPool.h"
#include "ir/Ir.h"
#include "ir/TypeModel.h"
#include "ir/Typing.h"
#include "runtime/Gc.h"
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
// Z elision. The may-z analysis (Analysis.h) proves most signals can never carry a
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
// offsets/padding (the module carries the target's DataLayout).
//
// Inferred types. The IR stores no per-instruction result type; the shared
// typing (ir/Typing.h) derives width / domain / signedness for each value from
// its operands and declared types, the same derivation the IR verifier checks.
//
// Scope (initial). Nonblocking assignment is lowered identically to blocking
// (no two-phase next-state staging yet); event / sensitivity gating is left
// to a future top-level sim loop; Arg / Ret (functions, tasks), unpacked arrays
// and memories are unsupported and rejected.

namespace clotho::ir::backend {

// A lowered value: the must/may planes packed into one i(2W) integer (low W bits
// = must / definitely 1, high W bits = may / could be 1), plus a separate iW `z`
// (high-impedance marker). Packing lets the monotone ops (AND/OR/NOT, select select)
// act on both planes with a single instruction.
struct Plane {
    llvm::Value* mm = nullptr; // 4-state: i(2W) [0,W) must, [W,2W) may. 2-state: iW value.
    llvm::Value* z  = nullptr; // 4-state z plane (iW), or null when never-z. Null for 2-state.
    // The encoding of `mm`: a 2-state value is a single iW with no unknown/z plane,
    // and the accessors (must_of/may_of/width_of/any_unknown) read this flag, so a
    // 2-state value never carries the redundant may/z half. For an instruction's
    // value the encoding is its IR type's domain, the single source of truth:
    // set_value asserts they agree and an operator never converts its operands.
    // Storage and the call ABI hold a value in the encoding of its type's domain
    // too: a 2-state signal, local, member or element is the native iW slot.
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

// One pending `--sanitize=bounds` check: an index the backend could not prove
// inside the bounds of what it indexes. Recorded where the access is built --
// the only place that knows the container's identity and bounds -- and reported
// where the access finally happens, which is the only place that knows whether
// it is a read or a write. An access whose direction is known at the point it
// is built (a string putc, a queue method) skips the LValue and reports on the
// spot through emit_bounds_report.
struct BoundsCheck {
    // i1: the index landed inside the bounds. Literally the predicate the LRM
    // behaviour is built from wherever one already exists, so the report can
    // never disagree with what the access then does.
    llvm::Value* inrange = nullptr;
    // i64: the index as the design wrote it, in the container's own coordinates
    // and extended with the index expression's own signedness.
    llvm::Value* index = nullptr;
    // i1: the index carried x/z bits, which makes `index` meaningless. Null
    // where the index cannot be unknown (a 2-state expression).
    llvm::Value* unknown = nullptr;
    // i64: the container's size at the moment of the access, for one bounded by
    // a runtime length (dynamic array, queue, string). Null for a declared
    // range, which lives in the site record instead.
    llvm::Value* extent = nullptr;
    // i1: only part of an indexed part-select fell outside (LRM 11.5.1 keeps the
    // in-range bits). Null for an all-or-nothing access.
    llvm::Value* partial = nullptr;
    // ptr: the site's constant rt_bounds_site record (file, source line, name,
    // index kind, declared bounds). Everything compile-time about the access
    // lives there, so only the runtime values travel as call arguments.
    llvm::Constant* site = nullptr;
};

// Storage width above which a runtime-offset bit / part select is done by the
// runtime (word-indexed) instead of inline (whole-value variable shift). The inline
// form costs O(width) instructions and the helper a flat 182-311 (2-/4-state, read /
// write); they cross between 190 and 290 bits, so 256 puts the helper where it wins at
// every width above it (wide_dyn_storage has the measurements). A select with a
// constant offset never reaches the helper's call.
constexpr uint32_t wide_dyn_select_bits = 256;

// Operand width above which multiply, divide and modulo are runtime calls
// (rt_wide_mul, rt_wide_udiv, ...) instead of LLVM's own lowering; at or below it
// they are native two-register arithmetic. LLVM lowers a wider `udiv` to a bit-serial
// loop (3.5 us at 256 bits against 34 ns at 128, 3.3 ms at 4096) and a wider `mul` to
// every partial product, whose compile time grows 12x per doubling past 4096 bits.
// One threshold serves all three: a 128-bit value is two registers, and the runtime
// call costs more than the native sequence there.
constexpr uint32_t wide_arith_bits = 128;

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
    // (LRM 11.5.1): 1 for each selected bit that is in range, at every level of a
    // nested select. Null when no bounds check applies (whole storage, or a
    // constant in-range select). A read forces x/0 outside it and a write is
    // restricted to it.
    llvm::Value* range_mask = nullptr;
    // `range_mask` was set by an enclosing dynamic select and this lvalue is a
    // constant part-select inside that select's window (`a[i][3:0]`, `sa[i].lo`),
    // so the mask still covers the whole enclosing window. A read only looks at
    // the selected bits and uses it as is; a write has to narrow it to its own
    // bits (store_mask), or it would clear the rest of the window (LRM 11.5.1).
    bool range_mask_outer = false;
    // i1: every dynamic select on the way to here (`a[i][j]`, `sa[i].f[j]`) has a
    // known index and a window inside its container. A select nested in an invalid
    // one is invalid, so set_select_bounds folds this into its own verdict. Null
    // when no dynamic select has been applied yet.
    llvm::Value* nest_valid = nullptr;
    // A runtime-offset select on storage too wide to shift inline: the load / store
    // goes through rt_wide_sel / rt_wide_ins, which index the containing 64-bit word.
    // `sel_base` / `sel_cwidth` are the container the selection is clamped to (the
    // whole slot for a direct select, the inner select's span for a nested one) and
    // `sel_unknown` is the i1 "wholly invalid" flag: an index has an x/z bit, at
    // this select or at one it is nested in, or an enclosing select is out of
    // range. Set instead of `range_mask`, whose construction is itself O(storage
    // width).
    bool         wide_dyn    = false;
    llvm::Value* sel_base    = nullptr;
    uint32_t     sel_cwidth  = 0;
    llvm::Value* sel_unknown = nullptr;
    // A module signal (not a frame local): its store services wait queues. `sig`
    // is meaningful only then.
    bool   is_signal = false;
    Signal sig{ 0 };
    // Reached through a virtual interface handle (LRM 25.9): the runtime address
    // of the owning instance's context record. `sig` is then a REPRESENTATIVE of
    // the member -- some instance's copy, naming which member and supplying every
    // def-local constant -- and this pointer says which instance actually owns the
    // storage, the wait buckets, the NBA shadow and the dump-var base. Null for an
    // ordinary signal, whose owner is fixed at compile time.
    llvm::Value* vif_ctx = nullptr;
    // A string target: `base` (indexed by elem_idx for an array) is the
    // rt_string handle; widths / bit_off / has_z are meaningless.
    bool is_string = false;
    // A dynamic-array / queue handle leaf (inside an aggregate or a class
    // body). Addressed via lv_handle_ptr by the queue / dynamic-array ops;
    // plane Load/Store on it traps (the frontend never emits one).
    bool is_handle = false;
    // The element / leaf type behind `is_handle`, needed to reach its rt_elem_ops
    // and its out-of-range sink. Only set for a container ELEMENT (where
    // dyn_inrange applies); a handle leaf inside an aggregate leaves it invalid.
    Ref handle_type{};
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
    // Pending `--sanitize=bounds` reports, one per runtime array index taken on
    // the way to this element. Always empty unless the sanitizer is on, so an
    // unsanitized build emits byte-identical code. Drained by the load / store
    // lowering into rt_bounds_report calls; purely a report channel, the LRM's
    // own out-of-range behaviour stays driven by `dyn_inrange` above.
    std::vector<BoundsCheck> bounds_checks;
    // Relocation-epoch witnesses of the containers this place is nested inside,
    // OUTERMOST FIRST; empty when the place is stable (a module signal, a frame
    // local, a fixed unpacked array element, a class property).
    //
    // Only `ref` consumes it: forming a ref through a heap-rooted place records
    // these pointers and their current values in the fat ref, so the callee detects
    // a restructuring that happened while the ref was live
    // (docs/design/data-model.md §3.1). One entry is appended per container element
    // select / assoc entry; subsequent member / bit steps carry the chain unchanged,
    // since those only extend `agg_path` and `bit_off`.
    //
    // A CHAIN rather than one pointer because level n's witness lives in level n-1's
    // buffer: `a[k][i]`'s inner queue handle sits in the map's payload, so reading
    // its epoch is only safe once the map's own epoch has been checked. Appending
    // (rather than overwriting, which is what a single field forced) is what makes
    // the nested place checkable at all — see emit_ref_epoch_check.
    std::vector<llvm::Value*> epoch_chain;
    // An associative-array element target (LRM 7.8/7.9). Unlike dyn/queue the
    // element pointer is not yet resolved: the get_ptr (read) vs put_ptr (write)
    // choice depends on Load/Store context, so it is deferred. `assoc_handle` is
    // the rt_assoc handle pointer. The key is normalized LAZILY, inside
    // assoc_elem_lvalue, from `assoc_key_arg` / `assoc_key_type` (the un-normalized
    // key expression and the assoc's own type) rather than eagerly here at
    // AssocSelect-lowering time: a VARIABLE key (docs/design/assoc-keys.md P1)
    // normalizes into the shared rt_keybuf_* scratch, and this deferred lvalue can
    // be resolved TWICE for one AssocSelect (`a[k] += 1` reads via get_ptr, then
    // writes via put_ptr) with unrelated code -- another assoc access, most
    // plainly -- possibly running in between; a span built once and cached across
    // that gap could be overwritten before the second resolution reads it. Building
    // it fresh inside each resolution keeps every rt_keybuf_begin/append/span/end
    // call self-contained, so no span the backend hands to the runtime ever
    // outlives its one consuming call. `assoc_ops` is the value element's
    // rt_elem_ops descriptor. The value element slot facts (slot_ty / has_z /
    // narrow / width / store_width / bit_off / array_ty) are filled as for a dyn
    // element. A Load resolves via rt_assoc_get_ptr (absent -> element default); a
    // Store via rt_assoc_put_ptr.
    bool            is_assoc       = false;
    llvm::Value*    assoc_handle   = nullptr;
    Index           assoc_key_arg  = Index{ 0 };
    Ref             assoc_key_type = Ref::Invalid;
    llvm::Constant* assoc_ops      = nullptr;
    // The assoc's VALUE element type, so the resolver can ask type-level questions
    // about it (an event element's Table 7-1 default is not a constant image).
    Ref             assoc_elem_type = Ref::Invalid;
    // Value element kind for a managed (string / nested-container) assoc value.
    // Set on the deferred lvalue; assoc_elem_lvalue lifts them onto the resolved
    // element lvalue (is_string / is_handle) so the Load/Store dispatch routes a
    // handle instead of a plane. Both false for an integral / real value.
    bool assoc_val_string = false;
    bool assoc_val_handle = false;
    // The array's `'{default: v}` slot rather than a keyed entry (LRM 7.9.11):
    // resolved through rt_assoc_default_ptr, which always yields a live slot, so
    // there is no key span and no found-check.
    bool assoc_default = false;
    // -- Generational write barrier (docs/design/garbage-collection.md §7) -------
    //
    // The class object this place lives INSIDE, as a raw body pointer, or null when
    // the place is not inside one. Non-null means a store here can put a class handle
    // into memory that a YOUNG collection does not scan: the owner may be OLD, and an
    // OLD object is treated as live-and-opaque rather than traced (Gc.h:80-95). Such a
    // store therefore needs rt_gc_remember on the owner (emit_gc_barrier).
    //
    // Set at exactly one place -- the HandleDeref that turns a handle into an object
    // lvalue -- which is what makes "no barrier for design state, statics or frame
    // locals" true by construction rather than by enumerating them: none of those
    // lvalues is rooted at a HandleDeref, so none can carry an owner. Cleared by
    // agg_member when the selected member cannot reach a class handle, so
    // `obj.count = 5` emits nothing, and carried unchanged through every element step
    // (which only extend agg_path / elem_idx) exactly as epoch_ptr is.
    //
    // The invariant, which every propagation site preserves: non-null implies a class
    // handle can land at or under this place, inside that owner.
    llvm::Value* gc_owner = nullptr;
    // This assoc's key CONTAINS a class handle (rt_elem_ops carries its key-handle
    // offsets, `assoc_key_handle_offsets`), so creating an entry stores a handle into
    // the map's key storage -- a reference the owner acquires even when the VALUE is an
    // int, and one that no further member selection is party to. Lifted onto the assoc
    // lvalue at AssocSelect and consumed by assoc_elem_lvalue's put_ptr path, which is
    // where the key is actually written.
    bool assoc_key_handle = false;
    // The OUTERMOST container on the path from `gc_owner` to this place whose handle
    // still lives inside the owner's BODY, with its interned type. Non-null only once a
    // select has re-based into that container's malloc'd payload, which is exactly when
    // a `ref` formed here hands the callee a payload-interior pointer that
    // rt_gc_find_object cannot resolve -- so the conservative frame scan cannot reach
    // the owner and the collector's re-log cannot carry it forward across a suspension.
    //
    // The outermost one, and never the inner element: an element address is inside a
    // payload that an outer ring growth reallocates, whereas a container handle in the
    // object body is at a fixed address and never relocates. Registering the inner one
    // would leave the collector reading freed memory after exactly the restructuring
    // the ref epoch exists to detect (:22568-22569).
    //
    // Consumed only by lower_task_call_inst, which registers it as a dynamically-scoped
    // container root for the call's duration (emit_gc_ref_container_add).
    llvm::Value* gc_container_root = nullptr;
    Ref          gc_container_type = Ref::Invalid;
    // -- Class-property change notifier (LRM 9.4.2, docs/design/classes.md) ------
    //
    // The notifiers of the class property this place selects, when some `@` / `wait`
    // in the design waits on it, plus the object body they live in. Null for every
    // other place -- and null for EVERY place in a design that waits on no property,
    // which is what makes that design pay nothing.
    //
    // Set at exactly one place, agg_member, off the interned class Ref plus the
    // property's field index. A blocking store then classifies its own old -> new
    // transition and wakes the notifiers whose edge it produced -- literally
    // service_wait_queue's job, with the waiter lists reached through the object
    // instead of through per-signal globals.
    const ChangeNotify* chg         = nullptr;
    llvm::Value*        chg_body    = nullptr;
    llvm::Type*         chg_body_ty = nullptr;
    // The SIGNAL whose Any bucket a mutation of this place must wake, when the
    // place defers into a container living in signal storage -- a module-level
    // assoc array, or one in a static class property's shared slot. The
    // signal-side analog of `chg`, carried onto the deferred assoc element at
    // exactly the two sites `chg` is, and consumed by notify_container_mutation.
    // `is_signal` itself cannot carry this: a deferred assoc ELEMENT is not
    // plane-addressable signal storage, and setting it would send Store servicing
    // down the mm-snapshot path with a null base.
    bool   mut_sig_valid = false;
    Signal mut_sig{ 0 };
};

// Why a container handle pointer is being taken (lv_handle_ptr).
//
// Not cosmetic and not defaulted: `Mutate` is where a waited-on CONTAINER class
// property fires its change notifier (LRM 9.4.3), so every call site -- present
// and future -- has to answer the question. That is what keeps the mutation
// surface closed, since a mutation classified as a read is a permanent park.
enum class HandleUse : uint8_t {
    // The container's contents are only read through this pointer. Resolves an
    // absent associative-array entry with get_ptr, creating nothing (LRM 7.8.6).
    Read,
    // The op about to run replaces, extends, shrinks or reorders the container.
    Mutate,
    // Addressing a place INSIDE the container, or handing the pointer to someone
    // who mutates LATER. Resolves for write, like `Mutate`, but fires nothing:
    // whoever performs the mutation notifies at the point it happens.
    Place,
};

// Why an rt_string handle pointer is being taken (lv_string_ptr).
//
// The same device HandleUse is, for the same reason and with the same absence of a
// default. A waited-on STRING class property fires its change notifier from a
// content comparison bracketing the write (LRM 9.4.2 / 6.16), so every call site
// has to say whether it is about to write; `Write` is checked against an open
// bracket, so a site that forgets one fails loudly at compile time instead of
// parking a waiter forever.
enum class StringUse : uint8_t {
    // The characters are only read through this pointer.
    Read,
    // The op about to run replaces the string's content. Must sit inside a
    // str_chg_snapshot / str_chg_after bracket.
    Write,
    // Handing the pointer to someone who writes LATER -- a `ref` / `out` / `inout`
    // formal, a forwarded concat target, an NBA staging record. Fires nothing here:
    // whoever performs the write notifies, or the call bracket does.
    Place,
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
    // Per-bit power-on z mask for a PARTIALLY-driven 4-state net: bit set
    // == undriven == powers on z (its driven bits keep the x default, overwritten
    // by their self-start driver at t0). Empty unless the net is partially driven
    // (compute_partial_undriven); the z-plane initializer uses it in place of the
    // whole-signal init_z / zero. Implies has_z. For an unpacked array the mask
    // spans every leaf, bit `leaf ordinal * width + bit` (emit_array_init).
    std::vector<uint64_t> undriven_mask;
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
    // Nonblocking intra-assignment target (`x <= #d y` / `x <= @e y`) with any
    // site that writes less than the whole destination: its deferred records
    // carry a per-write mask and the commit does a read-modify-write. Set by
    // classify_intra_target, which applies the masking rule of the shape's
    // plain-NBA sibling (nba_partial for a scalar, arr_masked / agg_masked for an
    // array element or aggregate leaf).
    bool     intra_masked  = false;
    // Nonblocking intra-assignment target whose destination is NOT a link-time
    // constant address: a member written through a virtual interface handle, where
    // the handle picks the instance. Its deferred records carry the resolved
    // destination pointer (and the instance context for the wake) the way an array
    // element's do, and emit_intra_commit loads them per record instead of hoisting
    // storage_ptr. Set by classify_intra_target from vif_rec, the same fact that
    // routes the member's PLAIN NBA to the pointer-carrying array queue.
    //
    // LRM 10.4.2 is what makes this the semantics and not an optimization: "If
    // variable_lvalue requires an evaluation, such as an index expression, class
    // handle, or virtual interface reference, it shall be evaluated at the same
    // time as the expression on the right-hand side." Capturing the resolved
    // destination at arm time IS that rule.
    bool     intra_ptr     = false;
    // Longest `<= @(a or b)` trigger list over the sites targeting this signal.
    // 0 or 1 means every site names one trigger, and the record carries no trigger
    // array at all -- byte-identical codegen to before multi-trigger support. Above
    // that, each record holds the dense ids IT registered on, so the first trigger
    // to mature it can take it off the others (rt_intra_event_register_group).
    uint32_t intra_trigs   = 0;
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
    // An associative-array signal: the slot is an opaque rt_assoc handle
    // (`clotho.assoc`), has_z is false. Entries live in its heap map, reached by
    // the AssocSelect path; the handle powers on { null } (an empty map) and is
    // freed at teardown.
    bool is_assoc = false;
    // Index of the instance-scope anchor this signal is addressed through
    // (Anchor / storage_ptr); no_anchor for an unscoped root-level signal,
    // which is addressed directly off @clotho.state.
    static constexpr uint32_t no_anchor = ~0u;
    uint32_t anchor = no_anchor;
    // A nonblocking target some virtual interface writes (LRM 25.9). Its writes
    // defer a record carrying the destination pointer AND the instance context,
    // instead of the per-signal block shadow: a handle supplies only a
    // representative of the member, so two handles naming twin instances would
    // stage into one shadow and commit into one instance. Every writer of such a
    // member takes the record path, or the two would commit out of execution
    // order (LRM 10.4.2) -- block commits run before records.
    bool vif_rec = false;
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
    // First state-struct field of the definition's virtual-interface waiter-list
    // heads (see scope_type); equal to the scope's own signal count.
    uint32_t vif_head_base = 0;
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

// One function-frame local: its slot type and width / domain. A 2-state local is
// `{ iW }`, a 4-state one `{ i(2W), iW }`.
// The frame is an LLVM struct with one field per local, in Local-handle order,
// so the local's index IS its GEP field index. No wait-queue / NBA machinery
// (frame locals have neither).
struct FrameSlot {
    llvm::StructType* slot_ty    = nullptr;
    uint32_t          width      = 0;
    bool              four_state = false;
    // A 2-state local, the native single-iW slot (`{ iW }`), which the call ABI
    // addresses and passes as it is.
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
    // A dynamic-array, queue, or associative-array local: the slot is a handle;
    // it powers on empty ({0}) and is freed at every body exit, like a string
    // local. `handle_type` is the interned handle type (picks the runtime slot /
    // delete function).
    bool is_dynarr = false;
    bool is_queue  = false;
    bool is_assoc  = false;
    Ref  handle_type = Ref::Invalid;
    // A ref formal: the local has no real frame storage (its dead field is kept
    // only so Local index == field index). Loads / stores redirect to the passed
    // actual pointer, recorded in ref_ptr_ by the callee prologue.
    bool is_ref = false;
};

// A `ref` formal's fat pointer, as bound from the callee's parameters
// (docs/design/data-model.md §3). `slot` aliases the actual's storage — a plane
// slot, a managed handle, or an aggregate image. `epochs` addresses the relocation
// epoch of each container the actual was nested inside, OUTERMOST FIRST, paired
// with that epoch's value at the call in `expects`; an unused level (a call site
// bound shallower than the formal's declared depth) passes the
// `clotho.ref.noepoch` zero word, which never compares stale. Every dereference
// re-checks the pairs in order and reports through `site`; the order is what makes
// a nested witness safe to read, since level n's epoch lives in level n-1's buffer.
//
// `formal` is the callee's own name for the binding, used to build the
// per-dereference site record a staleness report prints (§3.3).
struct RefBinding {
    llvm::Value*              slot = nullptr;
    std::vector<llvm::Value*> epochs;
    std::vector<llvm::Value*> expects;
    std::string               formal;
};

// Raised on a construct the backend cannot lower; caught at the top level and
// rendered as an ordinary located diagnostic (report_backend_error).
struct BackendError {
    std::string message;
    // Source extent of the offending construct. Filled in by lower_region for
    // any guard tripped while lowering an instruction, so the ~50 refusals under
    // it need not thread a location by hand -- the same trick lower/Lowerer.h's
    // cur_range_ plays for the frontend's own ~290 lowering guards. Stays
    // invalid for a whole-design guard (an instance-hierarchy or DPI-signature
    // check), which has no construct to point at.
    SourceSpan span{};
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
    llvm::GlobalVariable* str_sink  = nullptr;
    // The read-only empty-string image an ambiguous string ternary yields
    // (str_empty_global).
    llvm::GlobalVariable* str_empty = nullptr;
    // Write-only sink handles for out-of-range nested-container elements, keyed by
    // element type Ref (handle_sink_global).
    ankerl::unordered_dense::map<uint32_t, llvm::GlobalVariable*> handle_sinks;
    // The shared zero word a `ref` through a stable place aims its epoch at
    // (no_epoch_global).
    llvm::GlobalVariable* no_epoch = nullptr;
};

// Builds the shared state struct and lowers every process. One instance per
// emit; `lower_process` is reused across processes (the per-process value tables
// are reset each time).
class Backend {
public:
    Backend(const Module& mod, llvm::LLVMContext& ctx, llvm::Module& llmod, const SimulationConfig& config,
            const LocResolver& resolve)
      : mod_(mod)
      , ctx_(ctx)
      , root_env_(std::make_unique<ModuleEnv>(llmod))
      , env_(root_env_.get())
      , b_(ctx)
      , config_(config)
      , resolve_(resolve) {}

    std::vector<std::unique_ptr<llvm::Module>> take_unit_modules();

    void run();

    void build_sched_flags();

    llvm::IntegerType* sched_word_ty();

    uint64_t unclassified_trig_bits() const;

    uint64_t member_mask(uint32_t g, size_t i) const;

    llvm::Constant* sched_flag_ptr(uint32_t k);

private:
    // -- Cross-unit symbol references --

    llvm::Function* sym(llvm::Function* f);
    llvm::GlobalVariable* sym(llvm::GlobalVariable* g);

    bool unit_emission() const;

    llvm::Value* abs_id_const(const std::string& key, uint32_t value, uint32_t addend);

    bool abs_id_eligible(const SignalSlot& s) const;

    llvm::Value* arr_id_value(Signal sig);

    llvm::Value* agg_id_value(Signal sig);

    llvm::Value* intra_id_value(Signal sig);

    llvm::Value* trigger_id_value(Signal sig, uint32_t edge);

    ModuleEnv& unit_env(const Anchor& a);

    // -- LLVM type helpers --

    llvm::IntegerType* int_ty(uint32_t width) const;
    llvm::ConstantInt* zero(uint32_t width) const;
    llvm::ConstantInt* ones(uint32_t width) const;
    llvm::ConstantInt* x_mm(uint32_t width) const;

    llvm::ConstantInt* init_z_plane(const SignalSlot& s) const;

    llvm::Value* cast(llvm::Value* v, uint32_t to, bool sign);

    // -- State types --

    llvm::StructType* slot_type(uint32_t width, bool four_state, bool has_z, bool narrow = false);

    llvm::StructType* str_slot_ty();

    llvm::Constant* str_empty_const();

    uint32_t handle_epoch_field(Ref type) const;

    bool is_dynarr_type(Ref type) const;

    bool is_assoc_type(Ref type) const;

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
        // An aggregate (unpacked struct / fixed unpacked array) element. `agg_ty`
        // is its recursive storage type; the slot holds the flat aggregate image.
        // Only trivially-copyable (POD-leaf) aggregates reach here (frontend
        // guard) -- a managed leaf would need the kind-4 rt_elem_ops leaf table.
        bool        is_agg = false;
        llvm::Type* agg_ty = nullptr;
        Ref         elem_type{};
    };
    DynElem elem_desc(Ref elem_type);
    DynElem dynarr_elem(Ref array_type);

    uint64_t dynarr_stride(const DynElem& e);

    // The %p / == runtime descriptor of a queue / dynamic-array container: the
    // element format facts (kind 0 int / 1 real / 2 string, plane layout) plus
    // the container flavor. Shared by rt_container_format_p and rt_container_eq.
    struct ContainerFmt {
        int32_t  is_queue  = 0;
        int32_t  kind      = 0;
        uint32_t width     = 0;
        uint32_t is_signed = 0;
        uint32_t four      = 0;
        uint32_t has_z     = 0;
        uint32_t z_off     = 0;
        uint64_t stride    = 0;
    };
    ContainerFmt container_fmt(Ref container_type);

    // -- Garbage collection: the rt_gc_* ABI mirrors -----------------------------
    //
    // src/runtime/Gc.h declares these as part of the JIT-callable C ABI, so each
    // gc_*_ty mirror (declared with the runtime ABI below) must match its C struct
    // BIT FOR BIT -- the same "must match the runtime struct exactly" contract
    // elem_ops_ty carries, and the same
    // lockstep two-file edit. The two things that cannot be checked structurally,
    // the vptr-to-map offset and the offset of the request flag generated code
    // polls, are cross-checked at run time by rt_gc_init against
    // gc_map_vptr_offset() and gc_mutator_request_offset() below (Gc.h:167-173).
    //
    // A design that declares no class pays essentially nothing for the collector, and
    // that is by construction rather than by a shortcut. An earlier draft emitted the
    // collector only for a design with a non-empty class table, because
    // examples/wait_bench.v measured +10.3% simulate time with no class anywhere in
    // it, all of it rt_gc_frame_link / rt_gc_frame_unlink per process activation plus
    // one out-of-line rt_gc_safepoint per wheel advance. Both costs are gone now: the
    // safepoint is an inlined load-test-branch (emit_gc_safepoint) and a coroutine
    // frame is registered only when it can hold a reference (frame_may_hold_gc_ref),
    // which no frame of a class-free design can. What the shortcut bought was a
    // discontinuity -- declaring one class turned the full per-activation and
    // per-tick price back on for the whole design -- whereas a design with two
    // long-lived objects and no churn should pay a not-taken branch per wheel advance
    // and nothing per activation (docs/design/garbage-collection.md §5.2).

    // rt_gc_frame (Gc.h:137-142): { ptr prev, ptr next, ptr base, i64 bytes }.
    //
    // Field indices, named for the same reason GcMutField's are: emit_gc_frame_link
    // and emit_gc_frame_unlink write these slots inline, so a reorder here has to be
    // a reorder of names and not of anonymous integers. `prev` being field 0 is load
    // bearing -- both emitters aim a store at &node->prev by using the node pointer
    // itself -- and both assert it against the DataLayout.
    static constexpr unsigned kGcFramePrev  = 0;
    static constexpr unsigned kGcFrameNext  = 1;
    static constexpr unsigned kGcFrameBase  = 2;
    static constexpr unsigned kGcFrameBytes = 3;

    uint32_t gc_frame_off(unsigned field);

    // Field indices into gc_mutator_ty(), in rt_gc_mutator declaration order
    // (Gc.h:163-182). Named so that every offset below reads as a field name rather
    // than as a magic argument to getElementOffset, and so that inserting a field in
    // the mirror without renumbering here cannot compile to a plausible-looking
    // wrong offset.
    enum class GcMutField : unsigned {
        Cursor      = 0,
        Limit       = 1,
        BudgetLeft  = 2,
        BlockIndex  = 3,
        Request     = 4,
        BlockBase   = 5,
        StartBitmap = 6,
        FinBitmap   = 7,
        FramesHead  = 8,
    };

    int32_t gc_mutator_bytes();

    uint32_t gc_mut_off(GcMutField f);

    llvm::Value* gc_mut_ptr(GcMutField f, const char* nm);

    int32_t gc_mutator_request_offset();

    llvm::Constant* agg_proto_const(Ref type, llvm::Type* storage);

    llvm::GlobalVariable* agg_proto_global(Ref type);

    llvm::Constant* elem_proto_const(const DynElem& e);

    void collect_elem_leaves(Ref type, uint64_t base, llvm::Type* storage, std::vector<llvm::Constant*>& out);

    llvm::Constant* gc_offsets_global(const std::vector<uint32_t>& offsets,
                                      const std::string&           nm);

    void agg_key_handle_offsets(Ref type, uint32_t base, std::vector<uint32_t>& out);

    std::vector<uint32_t> assoc_key_handle_offsets(Ref assoc_type);

    bool assoc_key_is_class_handle(Ref assoc_type);

    std::vector<uint32_t> elem_handle_offsets(Ref elem_type, const DynElem& e);

    llvm::Constant* elem_ops_for(Ref elem_type);
    llvm::Constant* elem_ops_global(Ref container_type);

    // Sort comparator descriptor for the element of `array_type` (LRM 7.12.2
    // sort()/rsort()). `kind`: 0 = unsigned integral, 1 = signed integral,
    // 2 = real, 3 = unsigned 4-state integral, 4 = signed 4-state integral.
    // Derived from the element value type; the frontend has already restricted the
    // element to an integral of width <= 64 or a real (sort_uses_iter_path routes
    // everything else through the IR permutation sort). The 4-state kinds read the
    // slot's must/may planes, which is why they are separate kinds rather than a
    // flag on the other two -- a 2-state element must not pay a plane test.
    struct SortDesc {
        uint32_t kind;
        uint32_t width;
    };
    SortDesc dynarr_sort_desc(Ref array_type) const;

    bool is_queue_type(Ref type) const;

    bool is_handle_type(Ref type) const;
    llvm::StructType* handle_slot_ty(Ref type);
    void emit_handle_assign(llvm::Value* dst, llvm::Value* src, Ref type);
    void emit_handle_delete(llvm::Value* handle, Ref type);
    llvm::Value* handle_sink_global(Ref elem_type);

    llvm::Value* lv_handle_ptr(const LValue& lv, HandleUse use);

    void notify_container_mutation(const LValue& lv);

    void service_signal_container_mutation(Signal sig);

    void fire_prop_notifier(const LValue& lv, Edge edge);

    llvm::Value* str_chg_snapshot(const LValue& lv);

    void str_chg_after(const LValue& lv, llvm::Value* snap);

    bool is_string_type(Ref type) const;

    Ref leaf_type(Ref type) const;

    uint32_t array_leaf_count(Ref type) const;

    bool contains_struct(Ref type) const;

    bool is_agg_abi(Ref type) const;

    bool needs_agg_storage(Ref type) const;

    bool has_heap_leaf(Ref type) const;

    uint32_t array_dim(const Type& t) const;

    llvm::StructType* agg_leaf_slot_ty(Ref type);

    llvm::Type* agg_storage_ty(Ref type);

    uint32_t class_header_words(Ref class_ref) const;

    llvm::StructType* class_body_ty(Ref class_ref);

    llvm::StructType* class_desc_ty(size_t slots);

    uint32_t iface_bitmap_words() const;

    unsigned desc_map_field() const;
    unsigned desc_vtable_field() const;

    int64_t desc_field_vptr_offset(unsigned field);

    int32_t gc_map_vptr_offset();

    llvm::GlobalVariable* class_desc_global(Ref class_ref);

    uint32_t class_depth(Ref class_ref) const;

    llvm::Constant* class_vptr(Ref class_ref);

    llvm::Constant* null_object_global();

    // -- Per-class trace map (docs/design/garbage-collection.md §3.2) ------------

    void collect_class_handles(Ref type, uint64_t base, llvm::Type* storage,
                               std::vector<uint32_t>& out);

    bool reaches_class_handle(Ref type);

    std::vector<uint32_t> class_handle_offsets(Ref class_ref);

    std::vector<llvm::Constant*> class_elem_leaves(Ref class_ref);

    llvm::Constant* class_finalizer_fn(Ref class_ref);

    llvm::Constant* class_gc_map_global(Ref class_ref);

    uint64_t gc_object_bytes(Ref class_ref);

    // -- Container roots (docs/design/garbage-collection.md §3.3, Gc.h:226-268) ---
    //
    // A container's payload is malloc'd, not GC-heap memory, so neither existing root
    // path finds the handles inside it: a precise root table describes state-buffer
    // WORDS, and a conservative frame scan sees only a pointer to a ring the collector
    // rejects for not being in a block. A handle-bearing container is therefore traced
    // from its handle, precisely, through its element descriptor -- and something has
    // to name that handle.
    //
    // WHICH containers are roots is decided by where the container's own storage
    // lives, and getting it wrong in either direction is a bug: a class-property
    // container registered as a root leaks its whole subtree forever, and a
    // frame-local container left unregistered frees live objects.
    //
    //   design state / static class property -> a root. A constant table registered
    //       once at startup (emit_gc_root_tables); the storage never moves.
    //   frame local, or a body temporary       -> a root while the activation lives.
    //       rt_gc_container_add beside the existing init, _remove beside the existing
    //       deinit (init_frame / build_*_temps and deinit_body_strings).
    //   class property                         -> NOT a root. Reached through the
    //       owning object's rt_gc_map leaves (class_elem_leaves), so a container
    //       inside a dead object keeps nothing alive.
    //
    // The third row is why this walk is NOT wired into emit_agg_init / emit_agg_deinit
    // even though those already visit every container leaf: emit_class_body_init and
    // class_finalizer_fn call them for a class BODY, so a registration hidden in that
    // walk would silently make every class-property container a root. Keeping the
    // registration in its own walk means the class paths cannot register anything by
    // construction, rather than by remembering to pass the right flag at eight call
    // sites.

    // One handle-bearing container inside a storage image: its byte offset from the
    // image base, and its own type -- which names both the runtime kind and the
    // rt_elem_ops descriptor.
    struct GcContainer {
        uint64_t offset;
        Ref      type;
    };

    void collect_gc_containers(Ref type, uint64_t base, llvm::Type* storage,
                               std::vector<GcContainer>& out);

    uint32_t gc_container_kind(Ref container_type);

    // Register (Add) or unregister (Remove) every handle-bearing container inside a
    // FRAME-RESIDENT image at `base`, whose storage is a root for exactly as long as
    // the activation lives (Gc.h:239-241).
    //
    // Add and Remove share one walk, so they visit the same containers in the same
    // order and the pairing is structural. Statically a body may emit several Removes
    // for one Add -- deinit_body_strings runs at every `ret` -- but only one exit
    // executes, so dynamically each add is matched exactly once, which is the same
    // discipline the emit_handle_delete calls beside it already rely on.
    enum class GcRootOp : uint8_t { Add, Remove };
    void emit_gc_container_roots(GcRootOp op, Ref type, llvm::Value* base,
                                 llvm::Type* storage);

    void emit_class_body_init(Ref class_ref, llvm::Value* body_ptr);

    void emit_agg_storage_copy(llvm::Value* dst, llvm::Value* src, Ref type);
    void fill_agg_leaf(LValue& lv, Ref leaf_type);

    void collect_static_leaves(Ref type, std::vector<uint32_t>& path, std::vector<AggLeaf>& out);

    bool is_leaf_type(Ref type) const;

    LValue agg_member(LValue lv, Ref struct_type, uint32_t field_index);

    LValue agg_elem(LValue lv, Ref array_type, uint32_t pos);

    void scope_type(const InstanceScope& scope, std::vector<bool>& claimed, const std::string& hier);

    void compute_vif_reachable();

    void collect_vif_nba();

    void spread_by_definition(std::vector<bool>& mark);

    void collect_vif_waits();

    void build_state_types();

    // Walk a StoreNb's lvalue chain to its root signal, reporting whether the
    // target is the whole signal (a bare Signal lvalue) or a bit/part-select.
    struct NbaTarget {
        Signal sig;
        bool   whole;
    };
    NbaTarget nba_target(const Ir& ir, Index lv) const;

    static bool lvalue_has_dynamic_select(const Ir& ir, Index lv);

    static bool lvalue_has_subselect(const Ir& ir, Index lv);

    static bool lvalue_has_elem_select(const Ir& ir, Index lv);

    bool lvalue_is_string_place(const Ir& ir, Index lv) const;

    Ref lvalue_place_type(const Ir& ir, Index lv) const;

    void classify_intra_target(const Ir& ir, Index lv_idx, const NbaTarget& t);

    void classify_ordered_plain(const Ir& ir);

    void nba_classify_ir(const Ir& ir);

    void nba_classify();

    const SignalSlot& slot_of(Signal s) const;

    llvm::Value* storage_ptr(const SignalSlot& s);

    llvm::Value* vif_head_ptr(Signal sig, uint32_t edge);

    bool def_relative_for(const SignalSlot& owner) const;

    llvm::Value* shadow_ptr(const SignalSlot& owner, uint32_t field);

    llvm::GlobalVariable* anchor_shadow_base(Anchor& a);

    void emit_state_global();

    llvm::GlobalVariable* ensure_ctx(Anchor& a);

    llvm::Value* ctx_field(Anchor& a, uint32_t field, llvm::Type* ty);

    // Install a runtime instance context for the duration of one access (see
    // LValue::vif_ctx). Every def-relative base -- storage, NBA shadow, wait
    // buckets, dump-var index -- then resolves against that instance instead of
    // the one whose body is being emitted.
    //
    // The ANCHOR is half the override: a record only describes the definition it
    // was laid out for, so it may serve only signals of that anchor. A body-wide
    // install (a ctx-parameterized subroutine) can otherwise reach a signal of
    // some other scope -- a hierarchical reference out of the interface -- whose
    // ctx_ty is a different layout, and GEP a plausible offset off the wrong
    // record. ctx_override_for is the gate every base consults.
    class CtxOverride {
    public:
        CtxOverride(Backend& be, llvm::Value* ctx, uint32_t anchor)
          : be_(be)
          , saved_(be.ctx_override_)
          , saved_anchor_(be.ctx_override_anchor_) {
            if (ctx != nullptr && anchor != SignalSlot::no_anchor) {
                be_.ctx_override_        = ctx;
                be_.ctx_override_anchor_ = anchor;
            }
        }
        ~CtxOverride() {
            be_.ctx_override_        = saved_;
            be_.ctx_override_anchor_ = saved_anchor_;
        }
        CtxOverride(const CtxOverride&)            = delete;
        CtxOverride& operator=(const CtxOverride&) = delete;

    private:
        Backend&     be_;
        llvm::Value* saved_;
        uint32_t     saved_anchor_;
    };

    uint32_t lvalue_ctx_anchor(const LValue& lv) const;

    bool ctx_override_for(const SignalSlot& owner) const;

    void materialize_group_ctx();

    llvm::GlobalVariable* shadow_base_gv();

    llvm::Value* lv_field_ptr(const LValue& lv, uint32_t field);
    llvm::Constant* slot_default_const(llvm::StructType* slot_ty, uint32_t width, bool narrow,
                                       bool four_state, bool has_z);

    RefBinding ref_actual_arg(const LValue& raw);

    void push_ref_epochs(std::vector<llvm::Value*>& argv, const RefBinding& rb, uint32_t depth);

    llvm::Value* ref_actual_ptr(const LValue& lv);

    llvm::Value* lv_mm_ptr(const LValue& lv);
    llvm::Value* lv_z_ptr(const LValue& lv);

    // -- Per-process lowering --

    llvm::Function* lower_group(const std::vector<uint32_t>& members);

    // -- Functions --
    //
    // Each ir::Function becomes a plain (non-coroutine) LLVM function
    //   <result> @<mod>.fn.<name>(ptr state, ptr rng, <formals...>)
    // where a scalar formal or result crosses by domain: a 2-state one as one iW
    // (one pointer to the iW slot for an out / inout), a 4-state one as the
    // (i(2W), iW) plane pair ((ptr, ptr) for an out / inout; `{ i(2W), iW }` for a
    // result). Void return / no result for a void function. Locals live in a
    // per-call stack frame buffer, so recursion and `automatic` lifetime are just
    // ordinary stack frames; module signals are still reached through `state`.

    uint32_t width_of(Ref type) const;

    bool is_void(Ref type) const;

    llvm::Type* fn_return_ty(Ref return_type);

    bool abi_four_state(Ref type) const;

    void push_scalar_arg(std::vector<llvm::Value*>& argv, Plane pl, bool four);

    void push_scalar_out_arg(std::vector<llvm::Value*>& argv, const LValue& lv, bool four);

    llvm::Type* dpi_c_llvm_ty(DpiCType t);

    static bool is_dpi_vec(DpiCType ct);

    llvm::Function* declare_dpi_import(FunctionId id);

    llvm::Function* declare_function(FunctionId id);

    llvm::Function* declare_task(FunctionId id);

    uint32_t ref_epoch_depth(const Function& f, size_t k) const;

    bool ref_needs_epoch(const Function& f, size_t k) const;

    uint32_t formal_param_count(const Function& f, size_t k) const;

    uint32_t home_param_index(bool is_task) const;

    std::vector<uint32_t> param_indices(const Function& f) const;

    uint32_t task_record_param(const Function& f) const;

    void build_frame_layout(const Function& f);

    void init_frame();

    void deinit_body_strings();

    void lower_function(FunctionId id);

    void emit_agg_copyin(const Function& f);

    // -- Tasks --
    //
    // A task is a switched-resume coroutine (like a process) that also carries a
    // per-call frame (like a function). The ramp builds the frame and parks at the
    // initial suspend; the caller (a process or another task) drives it via
    // lower_task_call_inst. The body may suspend on #delay / @event, in which case
    // the caller suspends too; on completion the task copies output / inout formals
    // back through their pointers and resumes the caller (the record's parent).
    // Installs the instance context of a ctx-parameterized subroutine (see
    // Function::takes_iface_ctx) for its whole body. Held by value in the body
    // lowerer so it unwinds with the frame; the override itself is materialized
    // once the entry block exists, by arm().
    class BodyCtx {
    public:
        BodyCtx(Backend& be, const Function& f)
          : be_(be)
          , saved_(be.body_ctx_fn_) {
            be_.body_ctx_fn_ = f.takes_iface_ctx ? &f : nullptr;
        }
        ~BodyCtx() {
            be_.body_ctx_fn_ = saved_;
            be_.ctx_override_        = nullptr;
            be_.ctx_override_anchor_ = SignalSlot::no_anchor;
        }
        BodyCtx(const BodyCtx&)            = delete;
        BodyCtx& operator=(const BodyCtx&) = delete;

    private:
        Backend&        be_;
        const Function* saved_;
    };

    void arm_body_ctx();

    void lower_task(FunctionId id);

    void bind_ref_formals(const Function& f);

    void emit_copyin(const Function& f);

    void emit_copyout(const Function& f);

    llvm::StructType* awaiter_ty();

    llvm::Value* awaiter();

    llvm::Function* native_cb_trampoline(const char* name, llvm::FunctionCallee target,
                                         llvm::Function*& cache);

    llvm::Function* intra_cb_trampoline();

    llvm::Function* inertial_cb_trampoline();

    llvm::Function* enqueue_trampoline();

    void emit_suspend(const char* label);

    void lower_delay(Index amount);

    void emit_delay_timer(llvm::Value* ticks);

    void emit_entry(llvm::ArrayType* arr_ty, llvm::Value* q, llvm::Value* slot_idx, llvm::Value* handle,
                    llvm::Value* flag_ptr);

    void emit_static_subscribe(Signal sig, uint32_t edge, llvm::Value* handle, llvm::Value* flag_ptr);

    void emit_dynamic_register(Signal sig, uint32_t edge);

    void emit_wait_suspend();

    void lower_wait(ExtraIndex info);

    void emit_finals_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs);

    bool has_preponed() const;

    void emit_preponed_run(llvm::Value* state, llvm::Value* wheel,
                           const std::vector<llvm::Function*>& procs);

    bool has_observed() const;

    bool has_program() const;

    bool has_program_initial() const;

    void emit_observed_run(llvm::Value* state, llvm::Value* wheel,
                           const std::vector<llvm::Function*>& procs);

    bool has_renba() const;

    void emit_renba_run(llvm::Value* state, llvm::Value* wheel,
                        const std::vector<llvm::Function*>& procs);

    bool is_reactive_fn(FunctionId id) const;

    void build_program_fns();

    bool is_program_fn(FunctionId id) const;

    void collect_home_callees(const Ir&                                   ir,
                              const std::vector<std::vector<FunctionId>>& slot_fns,
                              const std::vector<std::vector<FunctionId>>& iface_fns,
                              std::vector<FunctionId>&                    out) const;

    void build_shared_home_fns();

    bool is_reactive_home_fn(FunctionId id) const;

    bool is_shared_home_fn(FunctionId id) const;

    llvm::Constant* program_live_ptr();

    void build_program_instances();

    void build_program_exit_owners();

    uint32_t program_exit_owner_of(FunctionId id) const;

    void lower_program_exit(Index i);

    uint32_t program_inst_of(uint32_t proc_index) const;

    llvm::Value* program_inst_live_ptr(uint32_t inst);

    void emit_program_end(uint32_t proc_index);

    void build_reactive_fns();

    void reject_unsupported_reactive(const std::string& where, const Ir& ir);

    bool reaches_fork(const Ir& ir);

    void reject_resume_once(const Ir& ir, std::string_view region, std::string_view what);

    void emit_main(const std::vector<llvm::Function*>& procs);

    // -- Precise global roots (docs/design/garbage-collection.md §5) -------------

    void emit_gc_root_tables();

    void signal_handle_offsets(const SignalSlot& s, uint64_t base, std::vector<uint32_t>& out);

    void signal_gc_containers(const SignalSlot& s, uint64_t base, std::vector<GcContainer>& out);

    llvm::Constant* gc_roots_global(llvm::Constant* base, const std::vector<uint32_t>& offsets,
                                    uint32_t index);

    llvm::Constant* gc_containers_global(llvm::Constant* base,
                                         const std::vector<GcContainer>& entries,
                                         uint32_t                        index);

    llvm::Constant* gc_config_global();

    llvm::Constant* cover_array(llvm::StructType*                   row_ty,
                                const std::vector<llvm::Constant*>& rows,
                                const std::string&                  nm);

    llvm::Constant* cover_config_global();

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

    static bool edge_is_handle(const WaitEdge& e);

    WaitCounts wait_counts() const;

    llvm::StructType* wait_elem_ty();

    llvm::StructType* event_obj_ty();

    llvm::StructType* event_node_ty();

    static constexpr uint32_t kEventTrig    = 0;
    static constexpr uint32_t kEventWaiters = 1;
    static constexpr uint32_t kEventPending = 2;
    // The event's runtime dump-var index, or -1 when it is not dumped. Carried in
    // the object because a fire reached through a handle does not know which
    // declared event it landed on, and the VCD dirty set is index-based.
    static constexpr uint32_t kEventVcd     = 3;
    // Next object on the NBA pending chain, or null when this one is not on it.
    static constexpr uint32_t kEventNextPend = 4;

    static constexpr uint32_t kNodeNext   = 0;
    static constexpr uint32_t kNodePrev   = 1;
    static constexpr uint32_t kNodeHandle = 2;
    static constexpr uint32_t kNodeFlag   = 3;
    // The object this node is linked onto. Recorded at link time so unlinking is
    // correct even when the event VARIABLE is reassigned while we are parked --
    // `@(a)` captures the object a named when the wait executed (LRM 15.5.5), and
    // the node must come off that object, not off whatever `a` names on resume.
    static constexpr uint32_t kNodeOwner  = 4;

    llvm::Value* event_obj(Signal sig);

    llvm::Value* event_obj_of(llvm::Value* handle);

    llvm::Value* load_event_handle(Signal sig);

    llvm::Value* event_field(llvm::Value* obj, uint32_t field);

    static const char* edge_suffix(uint32_t edge);
    void assign_event_slots();

    void analyze_event_monomorphism();

    bool event_is_mono(Signal sig) const;

    llvm::Value* event_obj_for(Signal sig);

    void build_wait_queues();

    void build_instance_ctx_layout();

    void compute_subscription_order();

    static bool is_shuffle(const Ir& ir, Index i);

    static bool ir_draws_rng(const Ir& ir);

    void build_rng_fns();

    void build_rng_global();

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

    void report_dedup_stat();

    static std::string normalize_shape(llvm::Function* fn, llvm::ModuleSlotTracker* mst = nullptr);

    void report_dedup_shapes(const std::vector<llvm::Function*>& group_fns);

    // -- Module-instance body sharing (dedup V1) --

    static llvm::GlobalVariable* only_inst_global(llvm::Function* fn);

    static bool constant_uses(const llvm::Constant* c, const llvm::GlobalVariable* gv);

    llvm::Function* clone_with_ctx_param(llvm::Function* rep, llvm::GlobalVariable* ctx_gv);

    void rewrite_to_stub(llvm::Function* fn, llvm::Function* shared, llvm::GlobalVariable* ctx_gv);

    void share_twin_groups(const std::vector<llvm::Function*>& group_fns);

    static bool scan_finish(const Ir& ir, std::vector<FunctionId>* out);

    std::vector<uint8_t> compute_may_finish(std::vector<uint8_t>* direct_out = nullptr) const;

    void report_finish_stat() const;

    void report_comb_fuse_stat() const;

    void compute_fusion_groups();

    void compute_trigger_masks();

    uint8_t colour_of(const WaitEvent& w) const;

    uint64_t trig_bits(Signal sig, uint32_t edge) const;

    void build_nba_state();

    void block_register(Signal sig, const llvm::APInt& mask, bool dynamic);

    llvm::Constant* block_not_queued();
    void emit_block_commit_tail(uint32_t id);

    void emit_block_commit_fn(uint32_t id);

    void emit_block_commit_body(uint32_t id);

    llvm::Value* agg_leaf_ptr(llvm::Value* base, llvm::Type* root_ty,
                              const std::vector<uint32_t>& path, uint32_t field);

    void emit_agg_commit(Signal sig, const llvm::APInt& mask);

    void finalize_block_nba();

    int32_t trigger_id(Signal sig, uint32_t edge) const;

    bool signal_has_waiters(Signal sig) const;

    bool signal_has_level_waiters(Signal sig) const;

    // Whether some waiter of `sig` (a static or dynamic queue entry, a virtual
    // interface waiter, or a `<= @e` trigger) is subscribed to `edge`.
    bool signal_has_bucket(Signal sig, Edge e) const;

    llvm::Value* load_mm(Signal sig);

    uint32_t slot_mm_bits(const LValue& lv) const;

    llvm::Value* load_lv_mm(const LValue& lv);

    llvm::Value* load_lv_z(const LValue& lv);

    // What a store through the wide-select path (LValue::wide_dyn) can change of its
    // target, snapshotted in place of the whole slot: K consecutive 64-bit words
    // around the select's offset in each plane that matters, plus the two words
    // that hold bit 0 of the must and may planes, which is all an edge reads
    // (LRM 9.4.2). A pointer is null for a plane or fact nobody consumes; `mm`,
    // `may`, `z`, `lsb_mm` and `lsb_may` are the values loaded through them
    // (wide_window_snapshot, service_wide_window).
    struct WideWindow {
        uint32_t     words     = 0;
        uint32_t     width     = 0; // the slot's value width W
        bool         four      = false;
        bool         need_any  = false;
        bool         need_edge = false;
        llvm::Value* mm_ptr    = nullptr;
        llvm::Value* may_ptr   = nullptr;
        llvm::Value* z_ptr     = nullptr;
        llvm::Value* lsb_mm_ptr  = nullptr;
        llvm::Value* lsb_may_ptr = nullptr;
        llvm::Value* mm        = nullptr;
        llvm::Value* may       = nullptr;
        llvm::Value* z         = nullptr;
        llvm::Value* lsb_mm    = nullptr;
        llvm::Value* lsb_may   = nullptr;
    };

    // Pre-write snapshot of an lvalue's selected storage, taken so wait-queue
    // servicing can classify the old -> new transition. `mm` is null when the
    // write never wakes anyone; `z` is non-null only when a level waiter exists
    // to consume the z word (a z-only x<->z transition is a value change,
    // LRM 9.4.2, but edges never read z).
    struct ServiceSnapshot {
        llvm::Value* mm = nullptr;
        llvm::Value* z  = nullptr;
        // Whether the target is a change-notified SCALAR class property rather than
        // a signal is read back off the lvalue, which service_after_write holds too;
        // nothing about it can change across the store. That covers every type whose
        // value is one stored word: integral, a class / chandle / virtual interface
        // handle (the word IS the value) and real / shortreal (a 2-state IEEE-754
        // pattern), all classified by the same emit_edge_facts.
        //
        // A waited-on CONTAINER property: there is nothing to snapshot (its value is
        // the container's contents), so the bracket carries only the fact that the
        // notify is owed, and service_after_write fires it unconditionally. Set by
        // the callers that hand a container's handle to a callee, which is the one
        // mutation shape that must notify AFTER the write rather than at it.
        bool container = false;
        // A waited-on STRING property: the pre-write content copy, compared with
        // rt_string_cmp after the write (str_chg_snapshot). Non-null only for a
        // string place carrying a notifier.
        llvm::Value* str = nullptr;
        // The words of a wide-select store's target it can touch, in place of `mm` /
        // `z`, which stay null (wide_window_snapshot).
        std::optional<WideWindow> wide;
    };

    ServiceSnapshot service_snapshot(const LValue& lv);

    void service_after_write(const LValue& lv, ServiceSnapshot old);

    std::optional<WideWindow> wide_window_snapshot(const LValue& lv);

    void load_wide_window(WideWindow& w);

    void service_wide_window(const LValue& lv, const WideWindow& old);

    // One old -> new transition's wake facts: which edge buckets fire. A fact is
    // null when its bucket was not requested (no waiters of that kind).
    struct EdgeFacts {
        llvm::Value* pos     = nullptr; // Table 9-2 posedge (a move toward 1)
        llvm::Value* neg     = nullptr; // Table 9-2 negedge (a move toward 0)
        llvm::Value* changed = nullptr; // any value change (level waits)
    };

    EdgeFacts emit_edge_facts(llvm::Value* old_mm, llvm::Value* new_mm, uint32_t width,
                              bool four_state, llvm::Value* old_z, llvm::Value* new_z,
                              bool need_pos, bool need_neg, bool need_any);

    // Table 9-2 posedge / negedge from the LSBs of the must and may planes before
    // and after a store (`old_may` / `new_may` are null for a 2-state slot).
    EdgeFacts emit_edge_bits(llvm::Value* old_m, llvm::Value* new_m, llvm::Value* old_may,
                             llvm::Value* new_may, bool need_pos, bool need_neg);

    void service_wait_queue(Signal sig, llvm::Value* old_mm, llvm::Value* new_mm,
                            llvm::Value* old_z, llvm::Value* new_z);

    // Fire the buckets a transition's facts select.
    void service_wake_buckets(Signal sig, const EdgeFacts& f);

    void service_change_notify(const LValue& lv,
                               llvm::Value*  old_mm,
                               llvm::Value*  new_mm,
                               llvm::Value*  old_z,
                               llvm::Value*  new_z);

    void fire_event_trigger(Signal sig, uint32_t edge);
    void emit_trigger_fire(llvm::Value* trig_id);

    llvm::Value* wait_base(uint32_t slot, llvm::Constant* gv, uint32_t sig);

    void service_static_queue(Signal sig, uint32_t edge);

    void service_dynamic_queue(Signal sig, uint32_t edge);

    llvm::StructType* event_timer_ty();

    llvm::Function* event_delay_thunk();

    void emit_event_pend_guarded(llvm::Value* handle);

    void emit_event_pend(llvm::Value* obj);

    void emit_event_pend_drain(llvm::Value* wheel);

    void emit_event_fire(llvm::Value* obj, llvm::Value* wheel, llvm::Value* trig_id = nullptr);

    void emit_event_fire_guarded(llvm::Value* handle, llvm::Value* wheel);

    void emit_event_wake_list(llvm::Value* obj);

    void emit_event_link(llvm::Value* obj, llvm::Value* node, llvm::Value* handle,
                         llvm::Value* flag, bool record_owner = true);

    void emit_event_unlink(llvm::Value* node, llvm::Value* obj = nullptr);

    llvm::Value* next_static_event_node();

    Plane lower_event_triggered(llvm::Value* obj);

    void emit_dynamic_remove(Signal sig, uint32_t edge);

    size_t fork_child_count() const;

    bool design_forks();

    // Ceiling on the fork-child slots the ring reserves. Two shapes have no static
    // bound at all (below), and a slot is one pointer, so saturating here spends
    // 8 KiB per ring in exchange for never sizing the ring by guesswork.
    static constexpr size_t kMaxForkRingSlots = 1024;

    // Peak number of simultaneously live activations of each function body,
    // relaxed over the static call / spawn graph. Shared by the two bounds that
    // have to stop assuming "one process is one call chain": the active ring's
    // fork-child slots (fork_ring_slots) and the dynamic wait bucket's capacity
    // (wait_counts).
    //
    // A ring entry is the head of one blocked call chain. A process that called a
    // task, or a fork child that called one, has at most ONE resumable coroutine
    // at a time (the caller suspends until the callee completes), so the peak
    // number of live entries is the number of live CHAINS -- and a chain is rooted
    // either at a process (one activation each; a process is ramped once) or at a
    // fork-child activation.
    //
    // A fork-child body belongs to exactly one fork site in exactly one enclosing
    // body, and `join` blocks that body until every child of the site is done, so
    // a site contributes one live child activation per child per live activation
    // of the body containing it -- never accumulating across loop iterations.
    // Hence
    //
    //     mult(process) = 1
    //     mult(f)       = Σ over call / spawn edges (b -> f) of mult(b)
    //
    // where a spawn edge is a Fork naming child f and a call edge is any
    // Call / TaskCall of f. The sum over edges (not over distinct sources) is what
    // makes this survive a shared task body: `fork t(); t(); join` gives t two
    // edges from the same process and so mult(t) = 2, and a width-2 fork inside t
    // then needs 4 slots, not 2. A bound of one slot per child body would assume
    // `join` serialized each fork's reuse, which holds for a single-activation
    // process and is false for a task body shared by several live callers.
    //
    // Relaxed to a fixpoint over the static graph; a DAG converges in at most as
    // many rounds as it has functions. Two shapes make no finite bound possible,
    // and both report `exact = false` with every entry saturated to
    // kMaxForkRingSlots, leaning on the runtime overflow checks rather than
    // pretending to a number:
    //   - recursion (a cycle in the graph): unbounded by construction;
    //   - a virtual task call, followed only to its statically named target (the
    //     bound collect_callees already documents), so a forking override reached
    //     through a handle would otherwise go uncounted.
    //
    // A third shape is unbounded in the child rather than in the graph: a
    // `join_any` / `join_none` site leaves siblings live while the parent walks on,
    // so a loop around the fork accumulates children with no static limit at all.
    // Its children (and everything they call) saturate to the ceiling, which is what
    // sizes every buffer they can occupy -- the rings, the `#0` batches and the
    // dynamic wait buckets. Past the ceiling the guards report; nothing corrupts.
    struct BodyActivations {
        // Live activations per FunctionId.
        std::vector<size_t> mult;
        // Reachable from a fork child through call / spawn edges (a fork child
        // itself included). These are exactly the frames the process-chain bound
        // does not already account for, so a capacity that has a chain term adds
        // only these.
        std::vector<uint8_t> from_fork;
        // False when no finite bound exists and `mult` is saturated throughout.
        bool exact = false;
    };

    BodyActivations body_activations() const;

    size_t fork_ring_slots() const;

    void ensure_active_queue();

    void ensure_inactive_queue();

    // Buffer selectors for rt_ring_overflow's diagnostic. Kept in step with the
    // names the runtime prints (src/runtime/Scheduler.cpp).
    static constexpr uint32_t kRingActive      = 0;
    static constexpr uint32_t kRingReactive    = 1;
    static constexpr uint32_t kRingInactive    = 2;
    static constexpr uint32_t kRingReInactive  = 3;
    static constexpr uint32_t kRingWaitDynamic = 4;

    void emit_ring_full_check(llvm::IRBuilder<>& b, llvm::Value* head_ptr, llvm::Value* tail,
                              llvm::Value* ring);

    void emit_len_full_check(llvm::IRBuilder<>& b, llvm::Value* len, uint32_t cap, uint32_t which);

    bool ring_guard_wanted();

    bool park_claim_wanted() const;

    void emit_active_push(llvm::IRBuilder<>& b, llvm::Value* handle);

    void emit_inactive_push(llvm::IRBuilder<>& b, llvm::Value* handle);

    void ensure_reactive_queue();

    void ensure_re_inactive_queue();

    void emit_reactive_push(llvm::IRBuilder<>& b, llvm::Value* handle);

    void emit_re_inactive_push(llvm::IRBuilder<>& b, llvm::Value* handle);

    // -- Where a parked coroutine comes back --
    //
    // A handle parked for a later wake carries its own destination region, in
    // bit 1 of the pointer. Reactive code that blocks -- an action block on an
    // event, a program `initial` on a #delay -- has to be resumed into the
    // REACTIVE ring, and the sites that do the waking cannot work that out for
    // themselves: one static subscription array, one wheel timer callback and
    // one join record each hold handles belonging to bodies with different home
    // regions, so no statically specialized wake site is possible. The
    // suspending body, on the other hand, knows the answer at lowering time.
    //
    // A bit in the pointer rather than a field beside it. The record that
    // decides this is wait_elem_ty: there is one entry per subscription per
    // edge across the whole design, statically sized, so widening it spends
    // real memory in every design -- including every design with no reactive
    // code at all, which is nearly all of them. The tag spends nothing to
    // store, and one `and`, one `icmp` and three `select`s to read, all on a
    // pointer the wake site has already loaded.
    //
    // Bit 1, because bit 0 already means "thunk, not coroutine" (the seed loop
    // in emit_active_run, and the thunk split in both drains). The two tags are
    // mutually exclusive by construction -- a thunk group is elided ordinary
    // code, never reactive -- so the test below is for bit 1 set AND bit 0
    // clear, and the mask is applied only when that holds. A thunk's function
    // pointer therefore never reads as reactive and never has its low bits
    // rewritten, which keeps this free of any alignment assumption beyond the
    // bit-0 one the thunk tag already makes. Coroutine frames come from
    // rt_alloc, whose low three bits are free.
    static constexpr uint64_t kReactiveHomeTag = 2;

    bool reactive_home() const;

    llvm::Value* tag_reactive_home(llvm::IRBuilder<>& b, llvm::Value* handle, bool reactive);

    llvm::Value* home_ptr();

    llvm::Value* current_home(llvm::IRBuilder<>& b);

    bool in_coroutine_body() const;

    llvm::Value* callee_home();

    llvm::Value* parked_handle(llvm::IRBuilder<>& b, llvm::Value* handle);

    static bool ir_has_expect_wait(const Ir& ir);

    static bool ir_has_delay(const Ir& ir);

    void emit_wake_push(llvm::IRBuilder<>& b, llvm::Value* parked);

    void emit_zero_delay_push(llvm::IRBuilder<>& b, llvm::Value* handle);

    void emit_region_push(llvm::IRBuilder<>& b, llvm::Value* handle);

    // Run the reactive-region arm or the active one according to this body's home.
    // Folded to whichever arm the lexical answer names, except for a body whose
    // home travels with the thread (body_home_is_runtime_: a shared subroutine,
    // LRM 24.3.1, or one that has run an `expect`, 16.17), where it is a branch.
    //
    // A branch and not a select, unlike parked_handle: that one ORs a bit onto a
    // pointer, so both answers are the same instruction. These two destinations are
    // different data structures -- a ring with head/tail versus a batch with a top
    // -- with their own overflow guards, so there is nothing to merge.
    //
    // Each arm may open blocks of its own (the guards do), so the arm's own last
    // block is what branches to the continuation rather than the block created here.
    template<typename Reactive, typename Active>
    void emit_by_home(llvm::IRBuilder<>& b, Reactive&& reactive, Active&& active) {
        // No reactive region set in this design at all: nothing was ever tagged,
        // neither reactive queue exists, and there is no choice to make.
        if (!body_home_is_runtime_ || !design_uses_reactive_) {
            if (reactive_home()) {
                reactive();
            } else {
                active();
            }
            return;
        }
        llvm::Function* fn   = b.GetInsertBlock()->getParent();
        auto*           rbb  = llvm::BasicBlock::Create(ctx_, "home.reactive", fn);
        auto*           abb  = llvm::BasicBlock::Create(ctx_, "home.active", fn);
        auto*           cont = llvm::BasicBlock::Create(ctx_, "home.cont", fn);
        b.CreateCondBr(b.CreateICmpNE(current_home(b), b.getInt8(0)), rbb, abb);
        b.SetInsertPoint(rbb);
        reactive();
        b.CreateBr(cont);
        b.SetInsertPoint(abb);
        active();
        b.CreateBr(cont);
        b.SetInsertPoint(cont);
    }

    llvm::Value* meta_at(llvm::Value* meta, unsigned field, uint32_t e, llvm::Type* ty);

    void emit_static_scan_rt(llvm::Value* base, llvm::Value* cap, bool padded = false, uint64_t trig = 0);

    void emit_dynamic_scan_rt(llvm::Value* base, llvm::Value* lenptr);

    void emit_nba_service(Signal sig, llvm::Value* meta, llvm::Value* old_mm, llvm::Value* new_mm,
                          uint32_t width, bool narrow, llvm::Value* old_z, llvm::Value* new_z);

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
    // Max process activations at a single time instant before the drain is
    // declared a non-converging zero-delay oscillation and aborted. Far above any
    // real single-instant settle wave (clotho's largest benches settle in well
    // under 500k activations per timestep — combchain ~400k), with a ~25x margin,
    // so it never trips on legitimate designs; low enough to abort in a few
    // seconds (before a typical test-harness timeout mistakes it for a hang).
    static constexpr uint64_t kOscillationLimit = 10'000'000;

    llvm::Value* emit_nba_any_staged();

    void emit_nba_commit(llvm::Value* state, llvm::Value* wheel);

    void emit_active_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs);

    // -- Coroutine intrinsics / frame allocator --

    llvm::Function* coro_intr(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> tys = {});

    // -- Runtime ABI (runtime/Runtime.h, Gc.h, Coverage.h C ABI; RuntimeAbi.cpp) --
    //
    // rt_fn declares (or fetches) one runtime entry point; each *_fn helper
    // below pins one C signature. Every pointer parameter is an opaque ptr, so
    // the LLVM-side signature is the C one with pointers erased; a helper
    // carries a comment only where the C signature is not obvious from it. Each
    // *_ty below mirrors a runtime C struct and must match it field for field.

    llvm::FunctionCallee rt_fn(const char* name, llvm::Type* ret, llvm::ArrayRef<llvm::Type*> params);
    llvm::Type* raw_ptr_ty();

    llvm::StructType* arg_struct_ty();
    llvm::FunctionCallee fwrite_fn();

    llvm::FunctionCallee strobe_pend_fn();
    llvm::FunctionCallee strobe_pop_fn();

    llvm::FunctionCallee monitor_emit_fn();
    llvm::FunctionCallee monitor_set_fn();
    llvm::FunctionCallee monitor_enable_fn();
    llvm::FunctionCallee monitor_active_ramp_fn();

    llvm::FunctionCallee timeformat_init_fn();
    llvm::FunctionCallee timeformat_set_fn();

    llvm::FunctionCallee finish_coop_fn();
    llvm::FunctionCallee observed_case_violation_fn();
    llvm::FunctionCallee observed_flush_fn();
    llvm::FunctionCallee severity_fn();
    llvm::FunctionCallee defer_report_fn();
    llvm::FunctionCallee defer_flush_observed_fn();
    llvm::FunctionCallee defer_flush_final_fn();
    llvm::StructType* bounds_site_ty();
    llvm::FunctionCallee bounds_report_fn();

    llvm::FunctionCallee wide_sel_fn();
    llvm::FunctionCallee wide_ins_fn();
    llvm::FunctionCallee wide_arith_fn(const char* name);

    llvm::StructType* null_site_ty();
    llvm::FunctionCallee null_handle_fn();

    llvm::StructType* ref_site_ty();
    llvm::FunctionCallee ref_stale_fn();
    llvm::FunctionCallee ref_epoch_regress_fn();
    llvm::StructType* ref_name_ty();

    llvm::StructType* tagged_site_ty();
    llvm::FunctionCallee tagged_access_fn();

    llvm::StructType* alloc_site_ty();
    llvm::FunctionCallee sim_run_fn();

    llvm::FunctionCallee fopen_mcd_fn();
    llvm::FunctionCallee fopen_fd_fn();
    llvm::FunctionCallee fclose_fn();
    llvm::FunctionCallee system_fn();
    llvm::FunctionCallee fflush_fn();
    llvm::FunctionCallee fgets_fn();
    llvm::FunctionCallee fgets_str_fn();
    llvm::FunctionCallee sformat_fn();
    llvm::FunctionCallee sformat_str_fn();
    llvm::StructType* fmt_dyn_struct_ty();
    llvm::FunctionCallee ferror_fn();
    llvm::FunctionCallee ferror_str_fn();
    llvm::StructType* scan_arg_struct_ty();
    llvm::FunctionCallee fscanf_fn();
    llvm::FunctionCallee sscanf_fn();
    llvm::FunctionCallee readmem_fn();
    llvm::FunctionCallee fread_fn();

    llvm::FunctionCallee vcd_open_fn();
    llvm::FunctionCallee vcd_begin_fn();
    llvm::FunctionCallee vcd_scope_fn();
    llvm::FunctionCallee vcd_upscope_fn();
    llvm::FunctionCallee vcd_var_fn();
    llvm::FunctionCallee vcd_array_fn();
    llvm::FunctionCallee vcd_start_fn();
    llvm::FunctionCallee vcd_limit_fn();
    llvm::FunctionCallee vcd_touch_fn();
    llvm::FunctionCallee vcd_flush_fn();
    llvm::FunctionCallee vcd_close_fn();
    llvm::FunctionCallee vcd_nullary_fn(const char* name);
    llvm::FunctionCallee vcd_touch_range_fn();

    llvm::FunctionCallee random_fn();

    llvm::FunctionCallee dist_fn(const char* name, unsigned nparam);

    llvm::FunctionCallee urandom_seed_fn();

    llvm::FunctionCallee randc_next_fn();

    llvm::FunctionCallee test_plusargs_fn();
    llvm::FunctionCallee value_plusargs_fn();
    llvm::FunctionCallee value_plusargs_str_fn();

    llvm::FunctionCallee nba_arr_slot_fn();
    llvm::FunctionCallee nba_arr_base_fn();
    llvm::FunctionCallee nba_arr_count_fn();
    llvm::FunctionCallee nba_arr_any_fn();
    llvm::FunctionCallee nba_arr_reset_fn();

    llvm::StructType* elem_ops_ty();
    llvm::StructType* elem_leaf_ty();

    llvm::StructType* dynarr_slot_ty();
    llvm::FunctionCallee dynarr_new_fn();
    llvm::FunctionCallee dynarr_new_copy_fn();
    llvm::FunctionCallee dynarr_delete_fn();
    llvm::FunctionCallee dynarr_assign_fn();
    llvm::FunctionCallee dynarr_move_fn();

    llvm::FunctionCallee dynarr_reverse_fn();
    llvm::FunctionCallee dynarr_sort_fn();
    llvm::FunctionCallee dynarr_shuffle_fn();

    llvm::StructType* queue_slot_ty();
    llvm::FunctionCallee dynarr_assign_queue_fn();
    llvm::FunctionCallee queue_push_fn(bool front);
    llvm::FunctionCallee queue_pop_fn(bool front);
    llvm::FunctionCallee queue_insert_fn();
    llvm::FunctionCallee queue_delete_idx_fn();
    llvm::FunctionCallee queue_delete_all_fn();
    llvm::FunctionCallee queue_assign_dynarr_fn();
    llvm::FunctionCallee queue_assign_fn();
    llvm::FunctionCallee queue_move_fn();
    llvm::FunctionCallee queue_append_range_fn();
    llvm::FunctionCallee queue_reverse_fn();
    llvm::FunctionCallee queue_sort_fn();
    llvm::FunctionCallee queue_shuffle_fn();
    llvm::FunctionCallee container_fmt_p_fn();
    llvm::FunctionCallee tagged_fmt_p_fn();
    llvm::FunctionCallee container_eq_fn();

    llvm::FunctionCallee keybuf_begin_fn();
    llvm::FunctionCallee keybuf_append_fn();
    llvm::FunctionCallee keybuf_span_fn();
    llvm::FunctionCallee keybuf_end_fn();
    llvm::FunctionCallee key_span_eq_fn();

    llvm::StructType* assoc_slot_ty();
    llvm::FunctionCallee assoc_get_ptr_fn();
    llvm::FunctionCallee assoc_put_ptr_fn();
    llvm::FunctionCallee assoc_exists_fn();
    llvm::FunctionCallee assoc_delete_key_fn();
    llvm::FunctionCallee assoc_delete_all_fn();
    llvm::FunctionCallee assoc_default_ptr_fn();
    llvm::FunctionCallee assoc_reset_fn();
    llvm::FunctionCallee assoc_num_fn();
    llvm::FunctionCallee assoc_end_fn(bool last);
    llvm::FunctionCallee assoc_step_fn(bool prev);
    llvm::FunctionCallee assoc_assign_fn();
    llvm::FunctionCallee assoc_move_fn();

    llvm::FunctionCallee str_init_fn();
    llvm::FunctionCallee str_deinit_fn();
    llvm::FunctionCallee str_assign_fn();
    llvm::FunctionCallee str_move_fn();
    llvm::FunctionCallee str_assign_bytes_fn();
    llvm::FunctionCallee str_len_fn();
    llvm::FunctionCallee str_data_fn();
    llvm::FunctionCallee str_concat_fn();
    llvm::FunctionCallee str_repl_fn();
    llvm::FunctionCallee str_repl_invalid_fn();
    llvm::FunctionCallee str_cmp_fn();
    llvm::FunctionCallee str_icmp_fn();
    llvm::FunctionCallee str_substr_fn();
    llvm::FunctionCallee str_upper_fn();
    llvm::FunctionCallee enum_name_fn();
    llvm::FunctionCallee str_lower_fn();
    llvm::FunctionCallee str_putc_fn();
    llvm::FunctionCallee str_getc_fn();
    llvm::FunctionCallee str_from_packed_fn();
    llvm::FunctionCallee str_append_packed_fn();
    llvm::FunctionCallee str_to_packed_fn();
    llvm::FunctionCallee str_atoi_fn();
    llvm::FunctionCallee str_atohex_fn();
    llvm::FunctionCallee str_atooct_fn();
    llvm::FunctionCallee str_atobin_fn();
    llvm::FunctionCallee str_atoreal_fn();
    llvm::FunctionCallee str_itoa_fn();
    llvm::FunctionCallee str_hextoa_fn();
    llvm::FunctionCallee str_octtoa_fn();
    llvm::FunctionCallee str_bintoa_fn();
    llvm::FunctionCallee str_realtoa_fn();

    llvm::FunctionCallee alloc_fn();
    llvm::FunctionCallee free_fn();

    llvm::FunctionCallee event_detach_register_fn();
    llvm::FunctionCallee event_detach_fire_fn();
    llvm::FunctionCallee event_detach_ready_fn();
    llvm::FunctionCallee event_new_fn();
    llvm::FunctionCallee event_ptr_fn();
    llvm::FunctionCallee event_trig_id_fn();
    llvm::FunctionCallee run_init_fn();

    llvm::FunctionCallee proc_alloc_fn();
    llvm::FunctionCallee proc_complete_fn();
    llvm::FunctionCallee proc_mark_kill_fn();
    llvm::FunctionCallee proc_adopt_fn();
    llvm::FunctionCallee proc_thread_done_fn();
    llvm::FunctionCallee proc_obj_retain_fn();
    llvm::FunctionCallee proc_obj_release_fn();
    llvm::FunctionCallee proc_await_self_fn();
    llvm::FunctionCallee proc_randstate_get_fn();
    llvm::FunctionCallee proc_randstate_set_fn();

    llvm::FunctionCallee wheel_init_fn();
    llvm::FunctionCallee wheel_deinit_fn();
    llvm::FunctionCallee oscillation_abort_fn();
    llvm::FunctionCallee ring_overflow_fn();
    llvm::FunctionCallee dpi_export_task_suspended_fn();
    llvm::FunctionCallee dpi_export_task_no_task_frame_fn();
    llvm::FunctionCallee wheel_now_fn();
    llvm::FunctionCallee wheel_schedule_rel_fn();
    llvm::FunctionCallee wheel_advance_fn();

    llvm::FunctionCallee intra_alloc_fn();
    llvm::FunctionCallee intra_alloc_rooted_fn();
    llvm::FunctionCallee intra_cb_fn();
    llvm::FunctionCallee intra_now_fn();
    llvm::Constant*      intra_total_gv();
    llvm::FunctionCallee intra_ready_base_fn();
    llvm::FunctionCallee intra_ready_count_fn();
    llvm::FunctionCallee intra_any_fn();
    llvm::FunctionCallee intra_release_fn();
    llvm::FunctionCallee intra_release_rooted_fn();
    llvm::FunctionCallee intra_reset_fn();

    llvm::FunctionCallee inertial_buf_fn();
    llvm::FunctionCallee inertial_decide_fn();
    llvm::FunctionCallee inertial_cb_fn();

    llvm::FunctionCallee intra_event_register_fn();
    llvm::FunctionCallee intra_event_register_group_fn();
    llvm::FunctionCallee intra_event_fire_fn();

    llvm::FunctionCallee dpi_pack_logicvec_fn();
    llvm::FunctionCallee dpi_unpack_logicvec_fn();
    llvm::FunctionCallee dpi_str_out_fn();

    llvm::StructType* rt_svopen_ty();

    llvm::StructType* gc_map_ty();
    llvm::StructType* gc_roots_ty();
    llvm::StructType* gc_container_ty();
    llvm::StructType* gc_container_table_ty();
    llvm::StructType* gc_frame_ty();
    llvm::StructType* gc_mutator_ty();
    llvm::StructType* gc_config_ty();
    llvm::FunctionCallee gc_init_fn();
    llvm::FunctionCallee gc_shutdown_fn();
    llvm::FunctionCallee gc_mutator_fn();
    llvm::FunctionCallee gc_alloc_fn();
    llvm::FunctionCallee gc_roots_add_fn();
    llvm::FunctionCallee gc_containers_add_fn();
    llvm::FunctionCallee gc_container_add_fn();
    llvm::FunctionCallee gc_container_remove_fn();
    llvm::FunctionCallee gc_alloc_slow_fn();
    llvm::FunctionCallee gc_safepoint_slow_fn();
    llvm::FunctionCallee gc_remember_fn();

    llvm::StructType* cover_range_ty();
    llvm::StructType* cover_bin_ty();
    llvm::StructType* cover_point_ty();
    llvm::StructType* cover_type_ty();
    llvm::StructType* cover_config_ty();
    llvm::FunctionCallee cover_init_fn();
    llvm::FunctionCallee cover_new_fn();
    llvm::FunctionCallee cover_ctx_fn();
    llvm::FunctionCallee cover_first_fn();
    llvm::FunctionCallee cover_next_fn();
    llvm::FunctionCallee cover_hit_fn();
    llvm::FunctionCallee cover_start_fn();
    llvm::FunctionCallee cover_set_name_fn();
    llvm::FunctionCallee cover_inst_query_fn(const char* name, llvm::Type* ret);
    llvm::FunctionCallee cover_point_query_fn(const char* name, llvm::Type* ret);
    llvm::FunctionCallee cover_type_query_fn(const char* name, llvm::Type* ret);
    llvm::FunctionCallee cover_design_coverage_fn();

    // -- Coroutine frame registry (docs/design/garbage-collection.md §2, F2) -----

    bool frame_may_hold_gc_ref(const Function& f);

    bool frame_may_hold_gc_ref(const Process& p);

    bool ir_may_hold_gc_ref(const Ir& ir);

    void emit_gc_frame_link(llvm::Value* frame_bytes);

    void emit_gc_frame_unlink();

    // -- Allocation (docs/design/garbage-collection.md §4.3, F3) ------------------

    bool gc_alloc_is_inline(Ref class_ref);

    void emit_gc_bitmap_set(GcMutField f, llvm::Value* byi, llvm::Value* bit, const char* nm);

    void emit_gc_start_bits(llvm::Value* start, bool fin);

    llvm::Value* emit_gc_alloc_fast(Ref class_ref);

    // -- Safepoints (docs/design/garbage-collection.md §2, F1) --------------------

    llvm::Value* gc_mutator_cur();

    void emit_gc_safepoint();

    void emit_gc_barrier(llvm::Value* owner);

    void emit_gc_barrier_for(const LValue& lv);

    void note_gc_container_root(LValue& lv, const LValue& parent, llvm::Value* handle,
                                Ref container_type);

    llvm::Value* emit_gc_ref_container_add(const LValue& lv);

    uint32_t intra_idx(Signal sig) const;
    bool              is_intra_target(Signal sig) const;
    llvm::StructType* intra_rec_ty(const SignalSlot& s);
    uint32_t intra_rec_mask(const SignalSlot& s) const;
    uint32_t intra_rec_mm_ptr(const SignalSlot& s) const;
    uint32_t intra_rec_z_ptr(const SignalSlot& s) const;
    uint32_t intra_rec_ctx(const SignalSlot& s) const;
    uint32_t intra_rec_trigs(const SignalSlot& s) const;
    bool intra_rec_rooted(const SignalSlot& s);
    uint32_t intra_agg_payload(const SignalSlot& s);

    uint32_t arr_idx(Signal sig) const;
    llvm::StructType* arr_rec_ty(const SignalSlot& s);
    uint32_t arr_rec_z_ptr(const SignalSlot&) const;
    uint32_t arr_rec_z_val(const SignalSlot&) const;
    uint32_t arr_rec_mask(const SignalSlot& s) const;
    uint32_t arr_rec_ctx(const SignalSlot& s) const;

    void build_wheel_global();

    static const char* dist_symbol(uint32_t fn);

    void init_state();

    void emit_agg_init(Ref type, llvm::Value* base, std::vector<llvm::Value*>& path, llvm::Type* root_ty);

    void emit_agg_deinit(Ref type, llvm::Value* base, std::vector<llvm::Value*>& path,
                         llvm::Type* root_ty);

    void emit_agg_deep_copy(Ref                        type,
                            llvm::Value*               dst,
                            llvm::Value*               src,
                            std::vector<llvm::Value*>& path,
                            llvm::Type*                root_ty);

    llvm::Constant* state_init_fields(llvm::StructType* ty,
                                      const std::vector<const SignalSlot*>& by_field);

    llvm::Constant* root_state_initializer();

    llvm::Constant* scope_state_initializer(const Anchor& a);

    llvm::Constant* const_leaf(llvm::StructType* slot_ty, uint32_t width, bool four_state, bool narrow,
                               bool has_z, std::span<const uint8_t> planes);

    llvm::Constant* const_slot(const SignalSlot& s, std::span<const uint8_t> planes);

    llvm::Constant* const_agg(Ref type, llvm::Type* storage, std::span<const uint8_t>& cursor);

    llvm::Constant* const_data_init(const SignalSlot& s, Ref const_data);

    llvm::Constant* slot_const(const SignalSlot& s);

    void emit_string_slot_op(const SignalSlot& s, llvm::FunctionCallee callee);

    void emit_array_init(const SignalSlot& s);

    llvm::Value* flat_elem_ptr(const LValue& lv);

    llvm::Value* agg_image_ptr(const LValue& lv);

    llvm::StructType* image_elem_slot(const LValue& lv, Ref type);

    llvm::Constant* agg_copy_proto(Ref type, const LValue& slv);

    void emit_agg_copy(const AggCopyInfo& info, const LValue& dlv, const LValue& slv,
                       llvm::Value* src_override);

    void emit_agg_ret_copy(llvm::Value* dst, const LValue& slv, Ref type);

    void emit_array_convert_copy(llvm::Value* d_base, llvm::Value* s_base,
                                 llvm::StructType* d_slot, llvm::StructType* s_slot,
                                 bool d_narrow, bool s_narrow, uint32_t leaf_width,
                                 uint32_t count, Signal dsig, bool service);

    void emit_array_zdiff_copy(llvm::Value* d_base, llvm::Value* s_base,
                               llvm::StructType* d_slot, llvm::StructType* s_slot,
                               uint32_t leaf_width, bool four_state, uint32_t count, Signal dsig,
                               bool service);

    llvm::Value* field_ptr_var(llvm::Value* base, llvm::Value* off);

    void lower_region(std::span<const Index> region);

    void branch_to(llvm::BasicBlock* bb);

    // -- Types --

    // The type of the value instruction `i` of the current body produces.
    ValueType type_of(Index i) const {
        return types_[static_cast<uint32_t>(i)];
    }

    Ref queue_elem_type(Index queue_lv);

    // -- Value / lvalue access --

    Plane value_of(Index i) const;
    void set_value(Index i, Plane p);

    const LValue& lvalue_of(Index i) const;
    void set_lvalue(Index i, LValue lv);

    llvm::Value* str_of(Index i) const;
    void set_str(Index i, llvm::Value* handle);

    llvm::Value* lv_string_ptr(const LValue& lv, StringUse use);

    llvm::Value* str_sink_global();

    llvm::Value* str_empty_global();

    bool produces_string_temp(Index i);

    bool forwarded_string_store(Index j);

    FunctionId call_func_id(Index i) const;

    void build_str_temps();

    void build_dynarr_temps();

    void build_agg_elem_temps();

    Ref call_return_type(Index i) const;

    void emit_agg_proto_fill(llvm::Value* dst, const DynElem& e);

    // -- Instruction dispatch --

    void lower_inst(Index i);

    // -- Leaves --

    Plane lower_constant(Ref ref);

    void lower_string_constant(Index i, Ref ref);

    void lower_string_concat(Index i);

    Plane lower_string_relation(Inst::Tag tag, Index l, Index r);

    llvm::APInt apint_from_bytes(std::span<const uint8_t> bytes, uint32_t width);

    llvm::ConstantInt* const_from_bytes(std::span<const uint8_t> bytes, uint32_t width);

    // -- State access --

    LValue signal_lvalue(Signal sig);

    LValue local_lvalue(uint32_t index);

    Plane lower_load(const LValue& lv);

    std::vector<llvm::Value*> wide_dyn_args(const LValue& lv, llvm::Value* mmp, llvm::Value* zp,
                                            uint32_t width, bool four);

    std::vector<llvm::Value*> wide_dyn_args(const LValue& lv);

    static uint32_t sel_buf_bits(uint32_t n);

    llvm::AllocaInst* sel_plane_buf(uint32_t n, const char* name);

    Plane wide_dyn_load(const LValue& lv);

    void wide_dyn_store_planes(const LValue& lv, llvm::Value* mmp, llvm::Value* zp, uint32_t width,
                               bool four, Plane rhs);

    void wide_dyn_store(const LValue& lv, Plane rhs);

    Plane dyn_elem_load(const LValue& lv);

    void dyn_elem_store(const LValue& lv, Plane rhs);

    void lower_dyn_array_new(Index i);

    void lower_dyn_array_new_init(Index i);

    void lower_dyn_array_size(Index i);

    void lower_dyn_array_copy(Index i);

    void lower_dyn_array_copy_queue(Index i);

    void lower_dyn_array_reverse(Index i);

    void lower_dyn_array_sort(Index i, bool reversed);

    void lower_dyn_array_shuffle(Index i);

    llvm::AllocaInst* entry_alloca(llvm::Type* ty, const llvm::Twine& name);

    LValue dyn_agg_elem_lvalue(const LValue&  container,
                               const DynElem& e,
                               llvm::Value*   data,
                               llvm::Value*   slot_index,
                               llvm::Value*   inrange);

    LValue elem_scratch_lvalue(llvm::Value* ptr, const DynElem& e);

    llvm::Constant* elem_default_const(const DynElem& e);

    void lower_queue_size(Index i);

    void lower_queue_push(Index i, bool front);

    void lower_queue_pop(Index i, bool front);

    void lower_queue_insert(Index i);

    void lower_queue_delete_idx(Index i);

    void lower_queue_delete_all(Index i);

    void lower_queue_copy(Index i);

    void lower_queue_copy_dynarr(Index i);

    void lower_queue_append_range(Index i);

    void lower_queue_reverse(Index i);

    void lower_queue_sort(Index i, bool reversed);

    void lower_queue_shuffle(Index i);

    // -- Associative arrays (LRM 7.8/7.9). The value element rides the dynarr
    // element slot machinery; keys are normalized to order-preserving byte spans
    // (docs/design/dynamic-arrays.md §4a) built here from the interned key type. --

    // The key kind facts of an assoc type: wildcard (`[*]`), string, an unpacked
    // aggregate (struct / fixed array, LRM 7.8.5), or a typed integral key of width
    // W with signedness. A packed struct / enum / class handle reduces to the
    // integral case via its layout width (a class handle is a 2-state 64-bit
    // pointer word, so its normalized key is its identity — LRM 7.8.3).
    struct AssocKeyInfo {
        bool     wildcard    = false;
        bool     is_string   = false;
        bool     is_agg      = false;
        // True when `is_agg` and some leaf (through struct / fixed-array nesting)
        // is a string / queue / dynamic array / associative array: the span has
        // no compile-time bound, so it normalizes into the shared rt_keybuf_*
        // scratch (normalize_variable_agg_key) instead of an entry-block alloca.
        // False keeps today's fixed encoding byte for byte, with no runtime call
        // (docs/design/assoc-keys.md P1).
        bool     is_variable = false;
        Ref      key_ty      = Ref::Invalid;
        uint32_t W           = 0;
        bool     is_signed   = false;
    };

    bool key_type_is_variable(Ref type);

    AssocKeyInfo assoc_key_info(Ref assoc_type);

    uint32_t agg_key_bytes(Ref type);

    uint32_t assoc_key_bytes(const AssocKeyInfo& info);

    // A normalized key: the byte span the runtime hashes / compares / sorts, plus
    // an optional i1 "the index is valid" predicate. Null `valid` means statically
    // valid (a 2-state or string key); otherwise it is false exactly when the key
    // holds an x/z bit, which LRM 7.8.4 / 7.8.5 make an invalid index (7.8.6: a
    // read yields the element default and a write is dropped, with no entry
    // created).
    struct AssocKeySpan {
        llvm::Value* ptr   = nullptr;
        llvm::Value* len   = nullptr;
        llvm::Value* valid = nullptr;
    };

    void emit_key_bytes(llvm::Value*     mustval,
                        uint32_t         W,
                        bool             is_signed,
                        llvm::Value*     scratch,
                        llvm::ArrayType* arr_ty,
                        uint32_t         off);

    std::pair<llvm::Value*, llvm::Value*> normalize_integral_key(llvm::Value* mustval, const AssocKeyInfo& info);

    uint32_t emit_agg_key_bytes(Ref              type,
                                const LValue&    lv,
                                llvm::Value*     scratch,
                                llvm::ArrayType* arr_ty,
                                uint32_t         off,
                                llvm::Value*&    any_x,
                                bool             is_signed);

    AssocKeySpan normalize_agg_key(const LValue& key_lv, const AssocKeyInfo& info);

    void emit_variable_agg_key_bytes(Ref type, const LValue& lv, llvm::Value* any_x_slot, bool is_signed);

    AssocKeySpan normalize_variable_agg_key(const LValue& key_lv, const AssocKeyInfo& info);

    llvm::Value* integral_key_valid(Plane pl);

    AssocKeySpan normalize_assoc_key_value(Index key, Ref assoc_type);

    AssocKeySpan normalize_assoc_key_lvalue(const LValue& key_lv, Ref assoc_type);

    void denormalize_assoc_key(llvm::Value* kptr, llvm::Value* klen, Ref assoc_type,
                               const LValue& key_lv, Index at);

    llvm::Value* denormalize_key_word(llvm::Value* kptr,
                                      uint32_t     off,
                                      uint32_t     W,
                                      bool         is_signed,
                                      uint32_t     store_width);

    void denormalize_agg_key(Ref type, const LValue& lv, llvm::Value* kptr, uint32_t& off, bool is_signed);

    void denormalize_variable_agg_key(Ref           type,
                                      const LValue& lv,
                                      llvm::Value*  kbase,
                                      llvm::Value*& off,
                                      Index         at,
                                      bool          is_signed);

    LValue assoc_elem_lvalue(const LValue& lv, bool for_write);

    void lower_assoc_num(Index i);

    void lower_assoc_exists(Index i);

    void lower_assoc_delete_key(Index i);

    void lower_assoc_delete_all(Index i);

    void lower_assoc_reset(Index i);

    void lower_assoc_copy(Index i);

    void lower_assoc_traverse(Index i, Inst::Tag tag);

    void lower_store(const LValue& lv, Plane rhs);

    void lower_store_nb(const LValue& lv, Plane rhs);

    void stage_dyn_mask(const SignalSlot& s, llvm::Value* bmask);

    void wide_dyn_stage_mask(const LValue& lv, const SignalSlot& s);

    llvm::APInt site_const_mask(const LValue& lv, const SignalSlot& s) const;

    int agg_static_leaf_index(const SignalSlot& s, const std::vector<uint32_t>& fpath) const;

    llvm::APInt leaf_set_mask(const SignalSlot& s, uint32_t leaf) const;

    void register_block_store(const LValue& lv, const SignalSlot& s);

    uint32_t agg_buf_idx(Signal sig) const;

    uint32_t handle_nba_slot(Signal sig) const;

    uint32_t handle_buf_idx(Signal sig) const;

    llvm::StructType* handle_rec_ty(Ref container_type);

    void emit_handle_move(llvm::Value* dst, llvm::Value* src, Ref type);

    void emit_handle_whole_nba(Index i);

    void emit_handle_whole_commit(Signal sig);

    uint32_t str_nba_slot(Signal sig) const;

    uint32_t str_buf_idx(Signal sig) const;

    llvm::StructType* str_rec_ty();

    void emit_string_whole_nba(const LValue& lv, Index rhs);

    void emit_string_whole_commit(Signal sig);

    uint32_t agg_max_leaf_bytes(Ref type);

    llvm::StructType* agg_rec_ty(const SignalSlot& s);

    llvm::Value* agg_arg_ptr(const LValue& lv);

    llvm::Value* agg_slot_ptr(const LValue& lv);

    void emit_agg_nba(const LValue& lv, Plane rhs);

    void emit_agg_array_commit(Signal sig, llvm::Value* /*state*/);

    void emit_array_nba(const LValue& lv, Plane rhs);

    llvm::Value* intra_pack_mm(const SignalSlot& s, Plane& rhs);

    void fill_intra_payload(const LValue& lv, Plane rhs, llvm::StructType* rt, llvm::Value* rec);

    void fill_intra_agg_payload(const LValue& lv, Plane rhs, llvm::StructType* rt, llvm::Value* rec);

    void emit_intra_nba_arm(const LValue& lv, Plane rhs, llvm::Value* ticks);

    void emit_intra_nba_ordered(const LValue& lv, Plane rhs);

    void emit_inertial_arm(const LValue& lv, Plane rhs, llvm::Value* ticks);

    void emit_intra_event_arm(const LValue& lv, Plane rhs, llvm::Value* count,
                              llvm::ArrayRef<llvm::Value*> trig_ids);

    void emit_intra_commit(Signal sig);

    void emit_intra_agg_commit(Signal sig);

    void emit_array_commit(Signal sig, llvm::Value* state);

    void service_any_waiters(Signal sig);

    llvm::Value* merge_bits(llvm::Value* old, llvm::Value* ins, llvm::Value* off, llvm::Value* mask,
                            llvm::Value* nmask);

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

    llvm::Value* low_ones(uint32_t W, llvm::Value* n);

    llvm::Value* range_bits(uint32_t W, llvm::Value* lo, llvm::Value* hi);

    llvm::Value* align_shl(llvm::Value* v, llvm::Value* s, uint32_t W);

    llvm::Value* smin(llvm::Value* a, llvm::Value* b);
    llvm::Value* smax(llvm::Value* a, llvm::Value* b);

    // How much of a dynamic bit/part select landed inside its container. Both
    // are i1; `partial` is only ever true for a part-select wide enough to
    // straddle the boundary (LRM 11.5.1 keeps the in-range bits of one).
    struct SelectVerdict {
        llvm::Value* fully   = nullptr;
        llvm::Value* partial = nullptr;
    };

    // `constant_window`: the select is a compile-time window at a constant offset
    // (a constant part-select hanging over its vector), so its mask folds to a
    // constant and needs no runtime kernel even on storage too wide to shift.
    SelectVerdict set_select_bounds(LValue& lv, llvm::Value* cbase, uint32_t cwidth, uint32_t selw,
                                    llvm::Value* idx_unknown, bool constant_window = false);

    static bool wide_dyn_storage(uint32_t sw);

    // -- Select indexes at their own width and signedness (LRM 7.4.5, 11.5.1) --
    //
    // An index is compared as the VALUE it holds, not as its low 32 bits or
    // zero-extended: a 2-bit signed -1 is -1, and 2**32 + 1 is not 1.

    // A bit / part-select index operand as the i32 added to a bit offset; a value
    // an i32 cannot hold saturates, which lands out of range in any container.
    llvm::Value* select_index(Plane idx, ValueType t);

    // An unpacked-array element: `pos` is its 0-based position (meaningful only
    // when `in_range`), `in_range` the verdict without the x/z term.
    struct ElemPos {
        llvm::Value* pos      = nullptr;
        llvm::Value* in_range = nullptr;
    };

    ElemPos fixed_elem_pos(Plane idx, ValueType t, uint32_t left, uint32_t right);

    // A dynamic-array / queue index `idx < len`; `idx64` addresses the element
    // only when `below`.
    struct ElemBelow {
        llvm::Value* idx64 = nullptr;
        llvm::Value* below = nullptr;
    };

    ElemBelow elem_index_below(Plane idx, ValueType t, llvm::Value* len);

    llvm::Value* store_mask(const LValue& lv, uint32_t sw);

    llvm::Value* place_val(const LValue& lv, llvm::Value* v_iw, uint32_t sw);

    llvm::Value* merge_sel(llvm::Value* old, llvm::Value* ins_iw, const LValue& lv, uint32_t sw,
                           llvm::Value* mask);

    static bool is_zero_const(llvm::Value* v);

    static bool selects_whole(const LValue& lv);

    // -- System tasks --

    void lower_timeformat(Index i);

    void lower_strobe_pend(Index i);

    void lower_reactive_pend(Index i);

    std::pair<llvm::GlobalVariable*, llvm::GlobalVariable*> expect_queue(uint32_t monitor);

    void lower_expect_wait(Index i);

    void lower_expect_wake(Index i);

    llvm::Value* pend_record_slot();

    void emit_finish_exit();

    void emit_finish_check(bool callee_may_finish);

    bool callee_may_finish(FunctionId id) const;

    bool virtual_call_may_finish() const;

    void lower_finish(Index i);

    void lower_case_viol(Index i);

    SourceText loc_text(Index i) const;

    llvm::Value* bounds_index(Index idx_expr);

    // The rt_bounds_what enum (Runtime.h), which picks the report's wording.
    enum class BoundsWhat : int32_t {
        Element   = 0,
        Bit       = 1,
        Part      = 2,
        Char      = 3,
        QueueOp   = 4,
        EmptyPop  = 5,
        AssocKey  = 6,
    };

    llvm::Constant* bounds_site_global(const SourceText& idx, const SourceText& acc,
                                       const std::string& name, BoundsWhat what, int32_t left,
                                       int32_t right, int32_t width = 1);

    std::string bounds_name(const LValue& lv) const;

    void add_elem_bounds_check(LValue& target, Index at, const LValue& named, Index idx_expr,
                               llvm::Value* inrange, llvm::Value* unknown, llvm::Value* extent,
                               int32_t left = 0, int32_t right = 0);

    void add_select_bounds_check(LValue& target, Index at, Index off_expr, const SelectVerdict& v,
                                 BoundsWhat what, uint32_t container_width, uint32_t sel_width);

    void emit_string_bounds_check(Index at, llvm::Value* str, Index idx_expr, const std::string& name,
                                  bool is_write);

    void emit_queue_bounds_check(Index at, llvm::Value* handle, Index idx_expr, const LValue& named,
                                 bool allow_append);

    void emit_empty_pop_bounds_check(Index at, llvm::Value* handle, const LValue& named);

    void emit_assoc_key_bounds_check(llvm::Value* kvalid, const LValue& lv, bool for_write);

    void emit_bounds_report(const BoundsCheck& bc, bool is_write);

    void emit_bounds_reports(const LValue& lv, bool is_write);

    // The rt_null_site record (Runtime.h): { ptr where, ptr line, i32 line_no,
    // i32 column, i32 end_column }. Field order and types mirror the C struct
    // exactly, so the two change together.
    // Which handle kind a null guard is protecting, so the report names it (LRM 8.4
    // for a class, 25.9 for a virtual interface). Mirrors rt_null_kind.
    enum class NullKind : int32_t { Class = 0, Iface = 1 };

    llvm::Constant* null_site_global(const SourceText& at, NullKind kind);

    void emit_null_handle_check(llvm::Value* handle, Index at, NullKind kind = NullKind::Class);

    llvm::Constant* no_epoch_global();

    llvm::Constant* ref_site_global(const SourceText& at, std::string_view formal);

    void emit_ref_name_table();

    void emit_ref_epoch_check(const RefBinding& rb);

    void lower_tagged_check(Index i);

    llvm::Constant* tagged_site_global(const SourceText& at, const TaggedCheckInfo& info);

    llvm::Constant* alloc_site_global(const SourceText& at, std::string_view what);

    void lower_severity(Index i);

    void lower_fflush(Index i);

    void lower_fopen_fd(Index i);

    void lower_fopen_mcd(Index i);

    void lower_system(Index i);

    void lower_cover_query(Index i);

    void lower_cover_ctl(Index i);

    void lower_fclose(Index i);

    void lower_file_unary(Index i, const char* fn);

    void lower_ungetc(Index i);

    void lower_fseek(Index i);

    void lower_fgets(Index i);

    std::tuple<llvm::Value*, llvm::Value*, uint32_t, uint32_t> string_dest(Index dst, const char* who);

    // The format string and the rt_fmt_dyn context of a $sformat / $sformatf.
    //
    // A constant format is a private global whose bytes the renderer walks, and a
    // null context: the frontend already validated every specifier, matched every
    // descriptor to its conversion, spliced %m / %l in and expanded %p away, so
    // the runtime has nothing left to decide. Identical code to before.
    //
    // A runtime format (LRM §21.3.3) is the string handle's own character buffer.
    // rt_string keeps it NUL-terminated (rt_string_data, the same guarantee the DPI
    // `string` argument path relies on), so the pointer goes straight through with
    // no copy: the renderer only reads it, within this call, and never retains it.
    // An empty string yields a pointer to a NUL and renders nothing. Text past an
    // embedded NUL is unreachable -- a `const char*` format cannot express one --
    // which is why the context carries the call site: a report about the format's
    // contents points at the statement.
    struct FormatArg {
        llvm::Value* fmt;
        llvm::Value* dyn;
    };
    FormatArg format_arg(Index i, Index fmt_value, ExtraIndex fmt, uint32_t fmt_len, TickScale scale);

    void lower_sformat(Index i);

    void lower_sformatf(Index i);

    void lower_ferror(Index i);

    void lower_scanf(Index i);

    void lower_test_plusargs(Index i);

    void lower_value_plusargs(Index i);

    std::pair<llvm::Value*, uint32_t> spill_ascii(Index v, const char* name);

    // A filename / source ASCII argument as a runtime { ptr, len, forward } triple.
    // A dynamic string is read straight from its rt_string handle in natural byte
    // order (forward = 1); a packed-ASCII vector spills its must-bits little-endian
    // (forward = 0, byte 0 = last char). Same split as $readmem / $dumpfile; the
    // runtime's read_ascii_arg reverses only the packed form.
    struct AsciiArg {
        llvm::Value* ptr;
        llvm::Value* len;
        llvm::Value* fwd;
    };
    AsciiArg spill_ascii_arg(Index v, const char* name);

    void lower_fread_word(Index i);

    void lower_fread_mem(Index i);

    void lower_readmem(Index i);

    // -- Value-change dump (VCD, LRM §21.7; runtime in src/runtime/Vcd.cpp,
    //    design in docs/VCD.md) --

    bool design_has_finish();

    llvm::Constant* finishing_gv();

    llvm::Constant* vcd_active_gv();
    void emit_guarded_vcd_touch(llvm::FunctionCallee                                  fn,
                                llvm::function_ref<llvm::SmallVector<llvm::Value*, 2>()> make_args,
                                llvm::Value* extra = nullptr);

    bool design_uses_vcd() const;

    // Any StrobePend instruction anywhere in the design (a process body, or a
    // task body it calls — $strobe is legal in either, never in a plain
    // function)? Scans functions too.

    // Any Observed-region report source in the design? Gates the per-instant
    // Observed-region flush (see emit_active_run). Currently a unique/priority
    // case violation (CaseViol); deferred immediate assertions will extend this.
    // CaseViol can live in a task body a process calls, so scan functions too.

    // Does this process body carry a deferred immediate assertion, i.e. a
    // Severity node with a non-zero `defer` (LRM 16.4)? A deferred assertion in a
    // called function/task is rejected in the frontend, so a process scan covers
    // every case the backend must handle.

    // Any deferred immediate assertion (LRM 16.4) in the design? Gates the
    // Observed / Postponed deferred-report flushes in emit_active_run.

    llvm::Value* defer_gen_ptr();

    llvm::Value* defer_gen_ptr_for(uint32_t g);

    void emit_defer_gen_bump();

    void emit_defer_gen_bump_at(llvm::Value* gen);

    // Any $monitor / $fmonitor registration in the design? Gates the per-instant
    // Postponed-region monitor run (see emit_active_run).

    void build_vcd_index();

    void vcd_index_scope(const InstanceScope& sc, uint32_t& next);

    void vcd_index_signal(Signal sig, uint32_t& next);

    llvm::Value* vcd_index_value(Signal sig, int32_t base);

    void vcd_touch_lvalue(const LValue& lv);

    void vcd_touch_signal(Signal sig);

    void vcd_touch_array(Signal sig);

    void vcd_touch_event_obj(llvm::Value* obj);

    void vcd_touch_event_unused(Signal sig);

    void lower_dump_file(Index i);

    void lower_dump_limit(Index i);

    void lower_dump_vars();

    void emit_vcd_scope(std::string_view name);

    void emit_vcd_scope_tree(const InstanceScope& sc);

    const char* vcd_unrepresentable(Ref type) const;

    void emit_vcd_var(Signal sig);

    void lower_dump_control(Inst::Tag tag);

    void lower_ret_void();

    void lower_ret(Index value);

    void lower_call_inst(Index i);

    llvm::Value* load_vtable_callee(const VirtualCallInfo& vci, Index at);

    void lower_virtual_call_inst(Index i);

    void lower_call_common(Index i, const Function& f, llvm::FunctionType* fnty,
                           llvm::Value* callee, ExtraSlice arg_slice);

    static uint32_t dpi_nchunks(uint32_t w);

    llvm::Value* dpi_spill_words(llvm::Value* plane, uint32_t nchunks);
    llvm::Value* dpi_load_words(llvm::Value* buf, uint32_t nchunks, uint32_t w);

    llvm::Value* dpi_vec_alloca(DpiCType ct, uint32_t nchunks);

    void dpi_vec_copyin(DpiCType ct, Plane pl, uint32_t w, llvm::Value* scratch);

    Plane dpi_vec_copyout(DpiCType ct, llvm::Value* scratch, uint32_t w);

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

    llvm::Value* dpi_oa_buf_elem(llvm::Value* buf, uint32_t total, bool is_logic, llvm::Value* flat);

    void emit_dpi_oa_pack_ir(llvm::Value* storage, llvm::Value* buf, llvm::StructType* slot,
                             uint32_t n, uint32_t ew, uint32_t nch, bool has_z, bool is_logic);

    void emit_dpi_oa_unpack_ir(llvm::Value* buf, llvm::Value* storage, llvm::StructType* slot,
                               uint32_t n, uint32_t ew, uint32_t nch, bool has_z, bool is_logic);

    Ref open_array_type_ref(Index a);

    llvm::Value* emit_svopen(llvm::Value* data, llvm::Value* left, llvm::Value* right,
                             llvm::Value* nelem, llvm::Value* elem_bytes, llvm::Value* ew,
                             llvm::Value* is_logic);

    llvm::Value* build_dpi_dynarray_desc(const LValue& lv, Ref arr_ref, DpiCType elem_ct);

    llvm::Value* build_dpi_open_array_desc(Index a, DpiCType elem_ct, ArgDir dir,
                                           std::vector<OaMarshalBack>& backs);

    llvm::GlobalVariable* dpi_caller_gv(llvm::GlobalVariable*& slot, llvm::Type* ty, const char* name);
    llvm::GlobalVariable* dpi_in_task_gv();
    llvm::GlobalVariable* dpi_caller_proc_gv();
    llvm::GlobalVariable* dpi_caller_home_gv();
    llvm::GlobalVariable* dpi_caller_rng_gv();

    // The caller identity an import-TASK call site publishes for the duration of
    // the call, and the saved previous values to restore afterwards. Import tasks
    // NEST -- an exported task may call another import task -- so this is a
    // save/restore, not a set/clear: on the way out the enclosing frame's identity
    // has to come back, including the 0 that means "the innermost DPI frame is an
    // imported function".
    struct DpiCallerSave {
        llvm::Value* in_task = nullptr;
        llvm::Value* proc    = nullptr;
        llvm::Value* home    = nullptr;
        llvm::Value* rng     = nullptr;
    };

    DpiCallerSave dpi_publish_caller();

    void dpi_restore_caller(const DpiCallerSave& saved);

    void lower_dpi_call(Index i, const CallInfo& ci, const Function& f);

    bool design_uses_dpi() const;

    void emit_dpi_exports();

    llvm::Type* dpi_export_c_param_ty(DpiCType ct, ArgDir dir);

    // One `output` / `inout` formal of an export thunk, recorded during the
    // argument marshal and copied back after the callee returns: the cells the
    // exported subroutine wrote (the value cell, and the z cell of a 4-state
    // formal), plus the C pointer to store through.
    struct ExpOut {
        llvm::Value* c_ptr;  // the C output pointer (scalar*) or svVecVal*
        llvm::Value* mm_ptr; // SV out ABI: the iW cell, or i(2W) when 4-state
        llvm::Value* z_ptr;  // SV out ABI: iW cell, null when 2-state
        DpiCType     ct;
        uint32_t     w;
        bool         four; // the formal's domain
    };

    void emit_dpi_export_copyout(const std::vector<ExpOut>&       outs,
                                 const std::vector<llvm::Value*>& str_temps);

    void emit_dpi_export(const DpiExport& ex);

    llvm::Value* dpi_arg_to_c(DpiCType t, Plane p);

    Plane dpi_c_to_plane(DpiCType t, llvm::Value* r);

    llvm::Value* record_slot();

    llvm::StructType* fork_record_ty(bool detached);

    llvm::Value* fork_live_slot();

    void emit_fork_record_abandon();

    llvm::Value* fork_record_slot();

    void emit_fork_record_release(llvm::IRBuilder<>& b, llvm::Value* rec);

    void lower_fork_inst(Index i);

    void lower_task_call_inst(Index i);

    llvm::Value* build_rt_args(std::span<const Index> args, std::span<const uint32_t> signs);

    void lower_display(Index i);

    void lower_fdisplay(Index i);

    void emit_display(const DisplayInfo& info, llvm::Value* desc);

    void emit_display_to(const DisplayInfo& info, llvm::Value* desc, llvm::FunctionCallee sink);

    void store_field(llvm::StructType* sty, llvm::Value* elt, unsigned idx, llvm::Value* v);

    // -- Plane helpers --

    static uint32_t bits_of(llvm::Value* v);
    static uint32_t width_of(Plane a);

    llvm::Value* must_of(Plane a);
    llvm::Value* may_of(Plane a);

    llvm::Value* z_of(Plane a);

    Plane two_val(llvm::Value* v);
    Plane known_plane(llvm::Value* v, bool four_state);
    Plane in_type_domain(Plane p, ValueType t);

    Plane to_four(Plane a);

    Plane packed(llvm::Value* must, llvm::Value* may, llvm::Value* z);

    llvm::Value* any_unknown(Plane a);

    Plane bit_plane(llvm::Value* must_i1, llvm::Value* may_i1);

    Plane cast_plane(Plane a, uint32_t to, bool sign);

    Plane lower_conversion(Inst::Tag tag, Plane a, ValueType to);

    llvm::Value* xz_mask(Plane a, Plane b, uint32_t w);

    Plane lower_net_res(Inst::Tag tag, Plane a, Plane b);

    Plane lower_strength_res(Index i);

    Plane lower_net_pull(Plane a, uint32_t pull);

    // -- Real (floating point) helpers --
    //
    // A real SSA value rides Plane.fp. Its storage / ABI form is the IEEE-754
    // bit pattern duplicated across must and may (a legal known value; the
    // must => may invariant holds) with z = 0, so state layout, change
    // detection, NBA staging, and the function ABI stay integer. Pack / unpack
    // happens only at these boundaries; instcombine folds the chains between
    // back-to-back float ops.

    llvm::Type* fp_ty(uint32_t width);

    Plane real_plane(llvm::Value* fp);

    llvm::Value* fp_of(Plane a);

    Plane real_to_planes(llvm::Value* fp);

    llvm::Value* planes_to_real(Plane a, uint32_t width);

    Plane lower_real_arith(Inst::Tag tag, Plane a, Plane b);

    Plane lower_real_compare(Inst::Tag tag, Plane a, Plane b);

    Plane lower_ftoi(Inst::Tag tag, Plane v, ValueType to);

    Plane lower_real_mux(Plane cond, Plane a, Plane b);

    llvm::Value* lower_string_mux(Plane cond, Index a, Index b);

    llvm::Value* libm_call(const char* name, llvm::Value* x, llvm::Value* y = nullptr);

    llvm::Value* rotl64(llvm::Value* x, unsigned k);

    llvm::Value* emit_xoshiro_next(llvm::Value* rng);

    Plane lower_urandom(llvm::Value* rng = nullptr);

    Plane lower_real_math_unary(Index i);

    Plane lower_real_math_binary(Index i);

    // -- Arithmetic: any X/Z in an operand makes the whole result X. --
    //
    // Lowered as a branch, not a select: the X case is rare, so it is split into a
    // cold block that just materialises the all-X constant (one move-immediate),
    // while the hot block does a single W-bit add/sub/mul of the value and
    // duplicates it across both planes (known bits have must == may).

    Plane lower_arith(Inst::Tag tag, Plane a, Plane b);

    Plane lower_divmod(Inst::Tag tag, Plane a, Plane b);

    // A multiply, divide or modulo of two iW values through the runtime routine `routine`
    // (rt_wide_mul, rt_wide_udiv, ...): spill both to word buffers, call, reload.
    llvm::Value* wide_arith(const char* routine, llvm::Value* a, llvm::Value* b);

    // a * b as iW, wrapping: LLVM's `mul` up to `wide_arith_bits`, rt_wide_mul above.
    llvm::Value* lower_mul(llvm::Value* a, llvm::Value* b);

    Plane lower_pow(Plane a, Plane b, bool base_signed, bool exp_signed);

    // -- Bitwise: AND/OR are monotone, so one op on the packed planes does both;
    // the result is never Z, so z is cleared. --

    Plane single_bit_lut(uint32_t lut, Plane a, Plane b);

    Plane lower_bitwise(Inst::Tag tag, Plane a, Plane b);

    Plane lower_bnot(Plane a);

    // -- Reductions -> 1-bit (chained AND/OR/XOR over the bits). --

    Plane lower_reduce(Inst::Tag tag, Plane a);

    Plane lower_bit_index(Inst::Tag tag, Plane a);

    llvm::Value* count_bits_pop(Plane a, uint32_t mask);

    Plane lower_count_bits(Plane a, uint32_t mask);

    Plane lower_onehot(Plane a, bool allow_zero);

    Plane lower_isunknown(Plane a);

    Plane lower_clog2(Plane a);

    // -- Shifts: X/Z in the count makes the whole result X; value planes shift. --

    Plane lower_shift(Inst::Tag tag, Plane a, Plane amt);

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

    std::pair<llvm::Value*, llvm::Value*> container_len_base(llvm::Value* handle, Ref type);

    llvm::Value* queue_ring_slot(llvm::Value* head, llvm::Value* idx, llvm::Value* cap);

    LValue container_elem_lvalue(llvm::Value* handle, llvm::Value* data, Ref type, const DynElem& e, llvm::Value* idx);

    LValue tmp_elem_lvalue(llvm::Value* data, const DynElem& e, llvm::Value* idx);

    Plane emit_container_eq_loop(const LValue& lhs, const LValue& rhs, Ref type, const DynElem& e);

    Plane lower_agg_eq(const LValue& lhs, const LValue& rhs, Ref type);

    Plane lower_compare(Inst::Tag tag, Plane a, Plane b);

    Plane lower_case_compare(Inst::Tag tag, Plane a, Plane b);

    Plane lower_inside_set(Index i);

    Plane lower_wildeq(Plane a, Plane b);

    // -- Logical and/or/not -> 1-bit, three-valued. --

    // Per-operand truth (i1): definitely-true (some must bit) / definitely-false
    // (no may bit). Unknown is neither.
    struct Truth {
        llvm::Value* t;
        llvm::Value* f;
    };
    Truth truth_of(Plane a);

    // i1: whether a 2-state value is nonzero, its truth in a logical operator.
    llvm::Value* nonzero(Plane a);

    Plane lower_logical(Inst::Tag tag, Plane a, Plane b);

    Plane lower_lnot(Plane a);

    Plane lower_limplies(Plane a, Plane b);

    Plane lower_lequiv(Plane a, Plane b);

    // -- Select: known condition selects (one packed select); unknown merges bitwise
    // (X where the arms disagree). --

    Plane lower_select(Plane cond, Plane a, Plane b);

    // -- Concat / Repl: first operand occupies the most-significant bits. --

    Plane lower_concat(Index i);

    llvm::Value* reverse_blocks(llvm::Value* v, uint32_t w, uint32_t slice);

    Plane lower_stream_reverse(Plane v, uint32_t slice);

    Plane lower_repl(Plane v, uint32_t part_width, uint32_t count);

    // -- Control flow: if/else with two regions. X/Z condition takes else. --

    void lower_cond(Index i);

    void lower_switch(Index i);

    void lower_select_lazy(Index i);

    // -- Control flow: for/while/forever. The cond region recomputes the
    // predicate each iteration; X/Z exits the loop (same definite-1 rule as
    // lower_cond). State lives in memory, so the back-edge needs no PHIs. --

    void lower_loop(Index i);

    // -- Control flow: a named block a `disable` targets (LRM 9.6.2). The block
    // needs an end block because a `disable` resumes AFTER it, which is a point
    // in the middle of the enclosing region, not any loop's exit. --

    void lower_named_block(Index i);

    llvm::Value* block_node(uint32_t label);

    llvm::Value* event_wait_node(uint32_t k);

    void lower_disable_remote(Index i);

    void emit_node_force_wake(llvm::Value* node, llvm::Value* hdl, llvm::BasicBlock* after);

    void lower_disable_list(uint32_t target);

    void emit_task_abort_poll();

    void emit_disable_poll();

    static bool tag_suspends(Inst::Tag t);

    llvm::Value* emit_abort_pending();

    void emit_wait_abort_exit(llvm::BasicBlock* leave);

    llvm::StructType* pcb_ty();

    // The full rt_proc field table (Runtime.h). Fields only the runtime touches are
    // listed too, so the indices stay a complete map of the layout.
    static constexpr uint32_t                  kPcbCtl        = 0;
    [[maybe_unused]] static constexpr uint32_t kPcbRefs       = 1;
    static constexpr uint32_t                  kPcbActs       = 2;
    static constexpr uint32_t                  kPcbWaiters    = 3;
    static constexpr uint32_t                  kPcbRng        = 4;
    [[maybe_unused]] static constexpr uint32_t kPcbJoinRec    = 5;
    static constexpr uint32_t                  kPcbObj        = 6;
    [[maybe_unused]] static constexpr uint32_t kPcbParent     = 7;
    static constexpr uint32_t                  kPcbFirstChild = 8;
    [[maybe_unused]] static constexpr uint32_t kPcbNextSib    = 9;
    [[maybe_unused]] static constexpr uint32_t kPcbPrevSib    = 10;
    static constexpr uint32_t                  kPcbNextMarked = 11;

    // LRM 9.7 `process::state`, the values slang's synthesized enum assigns, plus
    // the flags that share the word (RT_PROC_* in Runtime.h).
    static constexpr uint32_t kProcFinished  = 0;
    static constexpr uint32_t kProcRunning   = 1;
    static constexpr uint32_t kProcWaiting   = 2;
    static constexpr uint32_t kProcSuspended = 3;
    static constexpr uint32_t kProcKilled    = 4;
    static constexpr uint32_t kProcStateMask = 7;
    static constexpr uint32_t kProcStatic    = 1u << 8;
    // `kill` requested: the thread aborts at its next poll point.
    static constexpr uint32_t kProcAbort     = 1u << 9;
    // Woken while SUSPENDED, so `resume` owes it a push rather than a re-park.
    static constexpr uint32_t kProcWakePend  = 1u << 10;
    // Suspended itself: no source can wake it, only `resume`.
    static constexpr uint32_t kProcSelfPark  = 1u << 11;
    // Parked in `wait fork`, woken when the last live descendant completes.
    static constexpr uint32_t kProcWaitFork  = 1u << 12;

    llvm::Value* proc_arg();

    llvm::Value* pcb_field(llvm::Value* pcb, uint32_t field);

    llvm::GlobalVariable* proc_pcb_table();

    llvm::Value* proc_pcb_slot(uint32_t proc_index);

    llvm::Value* program_root_pcb(uint32_t inst);

    void emit_proc_state(uint32_t from, uint32_t to);

    llvm::Value* emit_proc_object(llvm::Value* pcb, Ref class_ref);

    llvm::Value* proc_pcb_of(llvm::Value* handle, Index at);

    llvm::StructType* proc_obj_body_ty();

    void emit_proc_wake_waiters(llvm::Value* head);

    void emit_proc_complete();

    void emit_pcb_acts_link();

    void emit_pcb_acts_unlink();

    llvm::BasicBlock* proc_abort_exit() const;

    void emit_proc_abort_poll();

    void lower_proc_kill(llvm::Value* target, bool with_self, bool branch_on_self = true);

    llvm::Value* proc_wait_node();

    void lower_proc_query(Index i);

    void emit_proc_park_while_suspended();

    void emit_proc_suspend(llvm::Value* pcb);

    void emit_proc_resume(llvm::Value* pcb);

    void lower_wait_fork();

    void emit_proc_publish_rng();

    llvm::Value* proc_rng_of(llvm::Value* pcb);

    void lower_proc_ctl(Index i);

    void lower_proc_wait(Index i);

    void emit_proc_unlink_node(llvm::Value* node);

    llvm::StructType* sync_node_ty();
    static constexpr unsigned kSyncNext   = 0;
    static constexpr unsigned kSyncPrev   = 1;
    // The parked coroutine handle, region tag intact (parked_handle), and the
    // parked thread's scheduled-flag address -- the two words a wait-bucket entry
    // holds, read back by the wake walk.
    static constexpr unsigned kSyncHandle = 2;
    static constexpr unsigned kSyncFlag   = 3;
    // The queue this node is on: the head word's address, then the tail word's.
    // Null `q_head` means "not linked", which is what makes the enqueue idempotent
    // across an unsatisfied retest (FIFO: the node keeps its arrival position).
    static constexpr unsigned kSyncQHead  = 4;
    static constexpr unsigned kSyncQTail  = 5;

    llvm::Value* sync_wait_node(uint32_t site);

    llvm::Value* sync_word_ptr(Index lv);

    llvm::Value* sync_word_load(llvm::Value* word);

    void sync_word_store(llvm::Value* word, llvm::Value* node);

    void emit_sync_enqueue(llvm::Value* node, llvm::Value* qhead, llvm::Value* qtail);

    void emit_sync_unlink(llvm::Value* node);

    void emit_sync_wake(llvm::Value* qhead, bool all);

    void lower_sync_wait(Index i);

    void lower_sync_ctl(Index i);

    llvm::GlobalVariable* disable_table(llvm::GlobalVariable*& gv, const char* name, llvm::Type* elem,
                                        llvm::Constant* init);

    llvm::Value* disable_slot(llvm::IRBuilder<>& b, llvm::GlobalVariable*& gv, const char* name,
                              llvm::Type* elem, llvm::Constant* init, uint32_t target);

    llvm::StructType* disable_node_ty();

    llvm::Value* disable_head_ptr(llvm::IRBuilder<>& b, uint32_t t);

    void emit_disable_link(uint32_t target, llvm::Value* node, llvm::Value* flag = nullptr);

    void emit_disable_unlink(uint32_t target, llvm::Value* node);

    size_t block_index(uint32_t label) const;

    llvm::BasicBlock* leave_blocks(size_t floor, llvm::BasicBlock* to);

    llvm::BasicBlock* leave_from(size_t top, size_t floor, llvm::BasicBlock* to);

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
    // The one per-target table the remote `disable` form needs (LRM 9.6.2), lazily
    // created in the root unit and indexed by target id: the head of the list of
    // activations currently inside that block, or running that task.
    llvm::GlobalVariable*   disable_head_gv_  = nullptr;
    // The static per-process control blocks (LRM 9.7): one array element per
    // process, at a constant address, with the starting state carried by the
    // initializer. A fork child's block comes from the runtime pool instead.
    llvm::GlobalVariable*   proc_pcb_gv_      = nullptr;
    llvm::ArrayType*        proc_pcb_arr_ty_  = nullptr;
    ankerl::unordered_dense::map<llvm::StructType*, ModuleEnv*>   unit_env_of_;
    // Root-defined integer-id value globals (abs_id_const): name -> global.
    ankerl::unordered_dense::map<std::string, llvm::GlobalVariable*> idval_gvs_;
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
    // The timing wheel's storage (clotho.wheel). A global so a function body, which
    // has no wheel parameter, can still read the clock.
    llvm::GlobalVariable*   wheel_gv_     = nullptr;
    // DPI caller identity across a native C frame (LRM 35.8), lazily created by
    // dpi_caller_gv and written with save/restore around every import-TASK call
    // (lower_dpi_call). An export FUNCTION thunk needs none of this -- C drove that
    // call, so there is no calling thread -- but an export TASK runs on behalf of
    // the process that entered the import task, and that process's identity has to
    // cross the C frame somehow. A global is enough because exactly one of
    // {simulation stack, C} runs at a time in T1, and save/restore handles the
    // nesting an exported task calling another import task creates.
    //
    // in_import_task is the one that is ALWAYS emitted once the design has DPI
    // tasks: it answers 35.8's legality question ("is the innermost DPI frame a
    // task?"), which is not conditional on whether the design happens to mention
    // `process`. The other three carry ABI arguments and so are gated on exactly
    // the conditions that put those arguments in the task signature.
    llvm::GlobalVariable* dpi_in_task_gv_     = nullptr; // i8: an import-task frame is live
    llvm::GlobalVariable* dpi_caller_proc_gv_ = nullptr; // ptr: calling thread's PCB
    llvm::GlobalVariable* dpi_caller_home_gv_ = nullptr; // i8: calling thread's home region
    llvm::GlobalVariable* dpi_caller_rng_gv_  = nullptr; // ptr: calling thread's RNG state
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
    // `--stat=dedup-sets`, compared post-lowering by report_dedup_shapes.
    std::vector<std::vector<uint32_t>> shape_sets_;
    // Per fusion group: owning scope anchor (single-scope reused-def groups
    // only, else no_anchor) and def-local group ordinal. Built by
    // build_instance_ctx_layout; the shareable-group identity.
    std::vector<uint32_t> group_anchor_of_;
    std::vector<uint32_t> group_def_ordinal_;
    // Owning scope anchor of ANY single-scope group (unique or reused def),
    // no_anchor for cross-instance / unscoped groups, and the group's dense
    // def-local ordinal among them: the split-emission unit assignment (see
    // unit_env / the lower_group loop in run()) and the blk absolute-id key.
    std::vector<uint32_t> group_unit_anchor_;
    std::vector<uint32_t> group_unit_ordinal_;
    // Def-local signal table per anchor (slot.path's last element), kept from
    // build_wait_queues for the ctx layout and block-signal translation.
    std::vector<std::vector<Signal>> anchor_signals_;
    // The ctx-parameterized subroutine currently being lowered, or null (BodyCtx).
    const Function* body_ctx_fn_ = nullptr;
    // Signals reachable through a virtual interface handle (compute_vif_reachable):
    // every signal of every instance of an interface definition some `virtual`
    // handle names. They keep the canonical type-derived shape.
    std::vector<bool> vif_sig_;
    // Per (signal, edge): does some `@(edge vif.sig)` park on this member? Set by
    // collect_vif_waits over the representative each such wait names, then spread
    // to every instance of the definition so the state layout stays def-uniform.
    // vif_head_ord_ is the head's def-local ordinal past Anchor::vif_head_base.
    std::vector<std::array<bool, 3>>     vif_wait_;
    std::vector<std::array<uint32_t, 3>> vif_head_ord_;
    // Members some handle nonblocking-writes (see SignalSlot::vif_rec).
    std::vector<bool> vif_nba_;
    // Instance context to resolve def-relative bases against, while emitting an
    // access through a virtual interface handle (see CtxOverride). Null outside
    // one, when the owning instance is the one whose body is being emitted.
    llvm::Value* ctx_override_        = nullptr;
    uint32_t     ctx_override_anchor_ = SignalSlot::no_anchor;

    // VCD dirty set (meaningful iff design_uses_vcd_). vcd_index_[sig] is the
    // runtime dump-var index of a dumped scalar signal, or -1 when the signal is
    // not a touched scalar (an unnamed temporary, a string, or an unpacked array —
    // array elements are always-scanned, not instrumented). nba_vcd_gv_ is a
    // parallel [n_nba x i32] of those indices, read in the NBA commit loop.
    bool                  design_uses_vcd_ = false;
    // Any StrobePend instruction anywhere in the design? Gates the per-instant
    // Postponed-region drain loop (emit_active_run), so a design with no
    // $strobe pays nothing.
    // Whole-design analysis results (Analysis.h), computed once at the top of
    // run() and read by every stage. Replaces what used to be a dozen private
    // analyses recomputed across several walks of the instruction arrays.
    DesignFacts facts_;
    bool design_uses_strobe_   = false;
    bool design_uses_monitor_  = false;
    bool design_uses_observed_ = false;
    bool design_uses_defer_    = false;
    // Any remote `disable` (LRM 9.6.2). Gates the per-target occupant tables, the
    // force-wake, the post-suspend poll, and the ring-overflow guard's second
    // reason to exist.
    bool design_uses_disable_  = false;
    // Any `process` class op (LRM 9.7). Gates the control-block machinery: the ABI
    // argument carrying the calling thread's block, the pooled block a fork child
    // takes, the status stores at every park, and the completion hook.
    bool design_uses_process_  = false;
    // Any ReactivePend, or any process that runs in Observed. Gates the entire
    // reactive region set: both queues, the drain blocks, and the Observed run.
    // A design with neither emits none of it.
    bool design_uses_reactive_ = false;
    // Functions reachable as reactive code, indexed by FunctionId
    // (build_reactive_fns). One of build_shared_home_fns' two seeds.
    std::vector<bool> reactive_fns_;
    // Whether any reactive body contains a #delay, i.e. whether the re-inactive
    // batch can ever be pushed. Decides eagerly, so lower_finish always sees the
    // queue it has to empty, whatever order the bodies lower in.
    bool reactive_has_delay_ = false;
    // Functions reachable from program code -- a program block's `initial` or a
    // program's continuous assign / port copy -- indexed by FunctionId
    // (build_program_fns). The other of build_shared_home_fns' two seeds.
    std::vector<bool> program_fns_;
    // The two answers build_shared_home_fns computes, indexed by FunctionId:
    // whether reactive-home code reaches the body at all, and whether ORDINARY code
    // reaches it too -- in which case it has no lexical home and reads the calling
    // thread's instead (LRM 24.3.1).
    std::vector<bool> reactive_home_fns_;
    std::vector<bool> shared_home_fns_;
    // Whether ANY body is shared, i.e. whether the `home` ramp parameter exists.
    // Design-wide for the same reason `proc` is (home_param_index): one vtable slot,
    // one FunctionType. False for every design with no shared body, which then emits
    // exactly the signatures it did before this existed.
    bool design_uses_home_arg_ = false;
    // Whether the body currently lowering is a program block's `initial`. Drives
    // the LRM 24.3 implicit finish; false for everything else, so no other design
    // pays for it.
    bool in_program_body_ = false;
    // Whether the body currently lowering is a reactive-home CONTINUOUS process
    // (ir::Process::reactive_home): a program's continuous assign or port copy,
    // LRM 24.3.1. Reactive-home like a program `initial` but not one of them, so
    // it is kept apart from in_program_body_, which also drives the implicit
    // finish countdown that only `initial` procedures may count against.
    bool in_reactive_home_body_ = false;
    // Whether the SUBROUTINE body currently lowering has a Reactive lexical home,
    // i.e. is in reactive_home_fns_. Says nothing about a shared body, whose home is
    // body_home_is_runtime_'s business; see reactive_home.
    bool in_reactive_home_fn_ = false;
    // Number of program `initial` processes, i.e. the initial value of the live
    // counter the LRM 24.3 implicit finish counts down. Zero when the design has
    // no program at all, which is what elides the whole mechanism.
    uint32_t program_proc_count_ = 0;
    llvm::GlobalVariable* program_live_gv_ = nullptr;
    // The LRM 24.3 program-INSTANCE partition (build_program_instances): how many
    // `initial` procedures each program instance declares, and which instance each
    // process belongs to (kNoProgramInst for a process that is not a program
    // `initial`). Both empty for a design with no program.
    static constexpr uint32_t kNoProgramInst = ~0u;
    // A subroutine no single program instance owns (build_program_exit_owners): two
    // instances reach it, or a program instance and a design process both do. Only
    // `$exit` cares, and it refuses rather than guess (LRM 24.7 answers by thread).
    static constexpr uint32_t kAmbiguousProgramInst = ~1u;
    // Per instance, the process indices of its `initial` procedures, in process
    // order. Its size is the number of program instances, and each entry's size is
    // that instance's live-`initial` count at time zero.
    std::vector<std::vector<uint32_t>> program_inst_procs_;
    std::vector<uint32_t>              program_inst_of_proc_;
    // Per FunctionId: the program instance whose `initial`s reach that subroutine,
    // kNoProgramInst for none, kAmbiguousProgramInst for more than one owner.
    std::vector<uint32_t> program_exit_owner_;
    // The program instance a `$exit` in the body currently lowering names. Set per
    // body: the instance itself for a program `initial`, the owner-closure answer for
    // a subroutine or fork child, kNoProgramInst for design code. Distinct from
    // emit_program_end's own lookup, which asks a different question (which instance
    // is the process that is ENDING part of) and happens to coincide for a process.
    uint32_t cur_program_inst_ = kNoProgramInst;
    // [ninst x i32] live-`initial` counters and [ninst x pcb] termination roots,
    // both created on first use by a program body that can have descendants.
    llvm::GlobalVariable* program_inst_live_gv_     = nullptr;
    llvm::ArrayType*      program_inst_live_arr_ty_ = nullptr;
    llvm::GlobalVariable* program_root_gv_          = nullptr;
    llvm::ArrayType*      program_root_arr_ty_      = nullptr;
    // Where the body currently lowering runs LRM 24.3's end-of-`initial` rules
    // (emit_program_end). Non-null only while a program `initial` body lowers.
    llvm::BasicBlock* program_end_bb_ = nullptr;
    // Current group's deferred-assertion generation-counter group id (-1 = none);
    // set per group in lower_group. Per-group generation globals, created lazily.
    int32_t                            cur_defer_group_ = -1;
    std::vector<llvm::GlobalVariable*> defer_gen_gv_;
    std::vector<int32_t>  vcd_index_;
    llvm::GlobalVariable* nba_vcd_gv_ = nullptr;

    // Function declarations, indexed by FunctionId; filled before any body is
    // lowered so calls (including recursive / forward) resolve.
    // Instruction currently being lowered; the ref dereference guard reports
    // against it. Set by lower_inst, read only by emit_ref_epoch_check.
    Index cur_inst_ = Index{ 0 };
    std::vector<llvm::Function*> fn_decls_;
    // &Function -> FunctionId, for the ABI helpers that only receive a reference.
    ankerl::unordered_dense::map<const Function*, uint32_t> fn_ids_;
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
    bool                    silent_stores_ = false;
    // Compile/simulation knobs (z elision, ...); see SimConfig.h.
    SimulationConfig        config_;
    // Renders a SourceLoc for a sanitizer report baked into the emitted code.
    // Empty for a caller with no SourceContext (the tests); see loc_text.
    const LocResolver&      resolve_;
    llvm::Value*           frame_ = nullptr;
    std::vector<FrameSlot> frame_slots_;
    // Per-ref-formal fat pointer, indexed by Local index; all-null for a non-ref
    // local. Bound in the callee prologue (bind_ref_formals) from the four passed
    // parameters, then read by local_lvalue to alias the actual live and check its
    // container's relocation epoch.
    std::vector<RefBinding> ref_ptr_;
    // The current function's frame struct (one field per Local, in order).
    llvm::StructType*       frame_ty_    = nullptr;
    // While lowering a task: the shared copy-out + caller-resume epilogue block a
    // Ret branches to. The caller-side record slot { ptr parent } for task calls
    // this coroutine makes (a process or a task can be a task caller).
    llvm::BasicBlock*       task_epilogue_bb_ = nullptr;
    // Innermost-first stack of the enclosing loops' (step, exit) blocks; a
    // `continue` branches to `.first`, a `break` to `.second`.
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loop_targets_;
    // block_targets_.size() when each loop of loop_targets_ was entered: a `break` or
    // `continue` leaves every targeted block opened inside the loop (leave_blocks).
    std::vector<size_t> loop_block_depth_;
    // Innermost-first stack of the enclosing named blocks' end blocks; a `disable`
    // branches to the end block of the id it carries. Empty unless the unit
    // contains a `disable` (only a targeted block is emitted at all). `remote` is
    // the same id when some `disable` names the block from outside, and
    // kNoDisableTarget otherwise: only a remotely-named block is polled after a
    // suspend, so a purely lexical target costs exactly what it did in V1.
    struct OpenNamedBlock {
        uint32_t          label;
        uint32_t          remote;
        llvm::Value*      node;
        llvm::BasicBlock* end;
        // Paths that leave this block, and every remotely-named block opened inside
        // it, to a continuation: built lazily by leave_from and shared by every exit
        // with the same continuation.
        struct Landing {
            llvm::BasicBlock* to;
            size_t            floor;
            llvm::BasicBlock* landing;
        };
        std::vector<Landing> leave = {};
    };
    std::vector<OpenNamedBlock> block_targets_;
    llvm::AllocaInst*       record_           = nullptr;
    // This activation's node on its task's `disable` list, or null when the task
    // being lowered is not a target. Frame-resident (hoisted after coro.begin).
    llvm::Value*            disable_node_     = nullptr;
    // The nodes a fork child links in its RAMP, one per enclosing targeted block
    // (ir::Function::fork_targets), with the target id each is on. Empty outside a
    // fork child. Until the body starts they publish `spawn_sched_`, a word held at 1,
    // as their scheduled flag, so a `disable` that lands while the child is only
    // queued marks it without pushing it a second time.
    std::vector<std::pair<uint32_t, llvm::Value*>> fork_disable_nodes_;
    llvm::Value*                                   spawn_sched_ = nullptr;
    // Per targeted named block in the body being lowered: its frame-resident list
    // node. Reset per function alongside disable_node_.
    ankerl::unordered_dense::map<uint32_t, llvm::Value*> block_nodes_;
    // Frame-resident event waiter nodes for the body being lowered, by event index
    // within a wait (event_wait_node). Cleared with block_nodes_ per body.
    ankerl::unordered_dense::map<uint32_t, llvm::Value*> event_wait_nodes_;
    // Frame-resident waiter nodes for the body being lowered, by wait SITE
    // (SyncInfo::site), so a `Park` and the `Leave` that discharges it share one
    // node. Cleared with the maps above, per body.
    ankerl::unordered_dense::map<uint32_t, llvm::Value*> sync_nodes_;
    // Per-function `join` record slot { i32 count, ptr parent, ... } (lazy, reused
    // across a coroutine's join sites — `join` serializes them, and its parent
    // outlives every child). Reset per fn. join_any / join_none heap-allocate
    // instead, per fork site (fork_record_ty).
    llvm::AllocaInst*       fork_record_      = nullptr;
    // The record TYPES are module-lived. `join` uses the short { count, parent };
    // join_any / join_none use { count, parent, wake_at, refs }.
    llvm::StructType*       fork_record_ty_          = nullptr;
    llvm::StructType*       fork_record_ty_detached_ = nullptr;

    // Per-process scratch, reset in lower_process.
    const Ir*           ir_         = nullptr;
    llvm::Function*     fn_         = nullptr;
    llvm::Value*        state_      = nullptr;
    llvm::Value*        wheel_      = nullptr;
    llvm::Value*        hdl_        = nullptr;
    llvm::Instruction*  coro_begin_ = nullptr;
    llvm::AllocaInst*   awaiter_    = nullptr;
    // This coroutine's rt_gc_frame registry node: an alloca placed just after
    // coro.begin, so CoroSplit hoists it into the heap frame (see
    // emit_gc_frame_link). Null when frame_may_hold_gc_ref said this frame can hold
    // no reference, so it was never registered -- and reset per ramp.
    llvm::Value*        gc_frame_node_ = nullptr;
    // The hoisted rt_gc_mutator_current() result for the function being emitted: one
    // call feeding every inlined safepoint poll in it (gc_mutator_cur). Not reset per
    // function -- the cache is keyed on the owning llvm::Function instead, so a
    // missed reset cannot leak an SSA value across a function boundary.
    llvm::Value*        gc_mutator_cur_ = nullptr;
    // The RNG the body being lowered draws from (LRM 18.13): a pointer to a
    // [4 x i64] xoshiro256++ state. Never null while a body lowers -- a process
    // that needs one owns a coroutine-frame alloca, a function or task takes the
    // caller's pointer as ABI parameter 1, and everything else (a thunk group, a
    // process the closure proved cannot reach a draw) gets the module-wide
    // fallback state, which no draw can reach.
    llvm::Value*        rng_slot_   = nullptr;
    // The control block of the thread whose body is being lowered (LRM 9.7): a
    // constant slot for a process, the pooled block for a fork child, the caller's
    // for a task. Null in a function (not a thread) and in any design that never
    // names `process`.
    llvm::Value*        pcb_        = nullptr;
    // This activation's node on its thread's activation stack (LRM 9.7), or null in a
    // body that is not part of a thread.
    llvm::Value*        acts_node_  = nullptr;
    // The heap join record this body is parked on (fork_live_slot), or null.
    llvm::Value*        fork_live_  = nullptr;
    // The module-wide fallback xoshiro256++ state, seeded once in main. It backs
    // the two entries that have no calling process at all -- a DPI export thunk
    // called from C, and the $monitor body the Postponed drain ramps -- and is
    // what a process with no draw of its own hands to a callee.
    llvm::GlobalVariable* rng_global_ = nullptr;
    // Per function: this body, or something it can reach, draws from the RNG.
    // Decides which processes materialize a frame state and which bodies own one;
    // the ABI parameter itself is unconditional (see declare_function).
    std::vector<bool>   rng_fns_;
    // Per function: this body owns its RNG rather than sharing its caller's,
    // because it can outlive the ramp site whose pointer it was handed -- a fork
    // child (LRM 9.3.2) and the deferred $strobe / $monitor / action-block bodies.
    // Seeded at the ramp from one draw of the ramping context's state, which is
    // the per-thread seeding LRM 18.14.1 requires of a spawned process.
    std::vector<bool>   rng_own_;
    // Per process (indexed as Module::processes()): reaches a draw.
    std::vector<bool>   rng_procs_;
    // Reused scratch for staging array-NBA value/mask bytes before a push call
    // (one per process, hoisted to coro.begin so a loop of element NBAs does not
    // grow the stack). Layout: [16]mm_val [16]mm_mask [8]z_val [8]z_mask.
    llvm::BasicBlock*   coro_cleanup_bb_ = nullptr;
    llvm::BasicBlock*   coro_final_bb_   = nullptr;
    std::optional<bool> design_has_finish_;
    // Per-subroutine may-finish closure; see compute_may_finish.
    std::vector<uint8_t> may_finish_;
    llvm::BasicBlock*   coro_ret_bb_     = nullptr;
    // The current coroutine's scheduled flag: &clotho.proc_sched[this process] for
    // a process (a Constant GEP), or a frame-resident i32 alloca for a task.
    llvm::Value*        sched_ptr_       = nullptr;
    // The body's runtime home-region slot (home_ptr), an i8 alloca spilled into
    // the coroutine frame. Null unless body_home_is_runtime_ -- almost every body
    // folds its home at lowering time instead.
    llvm::Value*        home_ptr_        = nullptr;
    // The gated `home` ramp parameter of the SUBROUTINE body currently lowering:
    // the calling thread's home region (LRM 24.3.1). Null in a process body, which
    // is a thread root and has no caller, and in a design with no shared body.
    llvm::Value* home_arg_ = nullptr;
    // Whether the body currently lowering must read its home from the running
    // thread rather than fold it: it is SHARED between reactive-home and ordinary
    // callers (LRM 24.3.1 / 24.5), or it executes an `expect`, whose resume moves
    // the thread into the Reactive region for good (LRM 16.17 + 24.3.1). Such a
    // body goes through home_ptr_ at every park instead of folding a constant.
    bool                body_home_is_runtime_ = false;
    // Deferred def-relative sched addressing chosen by lower_group (see
    // materialize_sched_ptr); no_anchor when the absolute constant is used.
    // sched_group_ is the fusion-group index currently lowering (feeds the
    // def-local blk-field ordinal in block_register).
    uint32_t sched_anchor_  = SignalSlot::no_anchor;
    uint32_t sched_ordinal_ = 0;
    uint32_t sched_group_   = 0;
    // Outlined cross-instance member currently lowering: its owning anchor
    // (~0 = none / not outlining) and def-local process ordinal, the
    // member-scoped idval key for its NBA blocks (see lower_group).
    uint32_t outline_anchor_ = ~0u;
    uint32_t outline_ord_    = 0;
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
    // JIT-local forwarders for the two native wheel-expiry callbacks; see
    // native_cb_trampoline.
    llvm::Function*                     intra_cb_tramp_    = nullptr;
    llvm::Function*                     inertial_cb_tramp_ = nullptr;
    llvm::GlobalVariable*               active_gv_         = nullptr;
    llvm::GlobalVariable*               active_head_gv_    = nullptr;
    llvm::GlobalVariable*               active_tail_gv_    = nullptr;
    llvm::GlobalVariable*               osc_count_gv_      = nullptr;
    llvm::GlobalVariable*               inactive_gv_       = nullptr;
    llvm::GlobalVariable*               inactive_top_gv_   = nullptr;
    // The reactive region set (LRM 4.4.2.6 / 4.4.2.7), duals of the two above:
    // the reactive ring holds action-block handles a ReactivePend queued from the
    // Observed region, the re-inactive batch holds reactive code parked on a #0.
    // Both reuse queue_arr_ty_ and active_mask_. Null unless the design has
    // reactive code (design_uses_reactive_).
    llvm::GlobalVariable*               reactive_gv_       = nullptr;
    llvm::GlobalVariable*               reactive_head_gv_  = nullptr;
    llvm::GlobalVariable*               reactive_tail_gv_  = nullptr;
    llvm::GlobalVariable*               re_inactive_gv_    = nullptr;
    llvm::GlobalVariable*               re_inactive_top_gv_ = nullptr;
    llvm::GlobalVariable*               pend_record_gv_     = nullptr;
    // Per-`expect` waiter queues (LRM 16.17), keyed by monitor index and created
    // on demand by expect_queue: {array of parked handles, count}. Same shape as
    // the inactive batch -- push at the top, drain the whole thing -- because an
    // expect's cohort is drained in one go and never partially.
    //
    // Sized like the rings, and the same bound justifies it exactly: a process can
    // be parked on at most one `expect` at a time, so one slot per process is
    // enough for every waiter one monitor can hold.
    ankerl::unordered_dense::map<uint32_t, std::pair<llvm::GlobalVariable*, llvm::GlobalVariable*>> expect_queue_;
    llvm::ArrayType*                    queue_arr_ty_      = nullptr;
    uint32_t                            active_mask_       = 0;
    // Memoized design_forks(); only a forking design pays the ring-overflow check.
    std::optional<bool>                 design_forks_;
    // Process indices in static-subscription order (compute_subscription_order).
    std::vector<uint32_t>               sub_order_;
    // Blocking-write wake graph (writer -> statically-subscribed reader) and
    // per-process membership in a nontrivial SCC (a multi-process
    // combinational loop), both retained by compute_subscription_order for
    // the comb-fusion pass in compute_fusion_groups.
    std::vector<std::vector<uint32_t>>  wake_adj_;
    std::vector<uint8_t>                scc_nontrivial_;
    // Sizes of the groups the comb-fusion pass formed (measurement only, for
    // the `--stat=comb-fuse` probe).
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
    // Per-(signal, edge) trigger colour: the wake word bit a wake of that pair
    // sets, or 0 for "no guarded group waits on it". Design-global so the mask
    // is a constant at the scan site (compute_trigger_masks).
    std::vector<std::array<uint8_t, 3>> trig_colour_;
    // Per-group: guard each member on its own sensitivity, the per-member masks
    // in member order, and the union of those masks. Empty / false / 0 for a
    // group that runs every member unconditionally, which is every group with
    // `regions` off.
    std::vector<bool>                   group_guarded_;
    std::vector<std::vector<uint64_t>>  group_member_mask_;
    std::vector<uint64_t>               group_trig_mask_;
    // Any group guarded at all. Gates whether an unclassified wake has to set
    // every trigger bit, so a design with no guarded group emits the wake sites
    // it always did.
    bool                                any_guarded_     = false;
    uint32_t                            trig_collisions_ = 0;
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
    // The design-wide event object array (clotho.events) and its LLVM type. One
    // slot per declared named event, plus the reserved null object at slot 0.
    // Built in build_wait_queues; addressed by event_slot_ through event_obj().
    llvm::GlobalVariable*                              events_gv_ = nullptr;
    llvm::ArrayType*                                   events_arr_ty_ = nullptr;
    // Slot of each signal's event object, or 0 for a non-event signal.
    std::vector<uint32_t>                              event_slot_;
    // Per signal: true when an event variable is never assigned, so it names one
    // object for the whole run and every reference folds to a constant address.
    std::vector<bool>                                  event_mono_;
    // Number of event object slots, including the reserved null object at 0.
    uint32_t                                           event_slot_count_ = 1;
    // Contiguous nodes for persistent (`always @(e)`) event waiters
    // (clotho.event_nodes), one per static subscription, plus the next free index.
    // Null when the design has no persistent event waiter.
    llvm::GlobalVariable*                              event_nodes_gv_ = nullptr;
    llvm::ArrayType*                                   event_nodes_arr_ty_ = nullptr;
    uint32_t                                           event_node_next_ = 0;
    // True when the design contains any `->> e`, so the NBA region emits the
    // pending-chain test and drain. The chain itself is a runtime list threaded
    // through the event objects (clotho.event_pend_head), because a handle-reached
    // event cannot be enumerated at compile time.
    bool                                               design_uses_nb_events_ = false;
    // True when the design contains any `->> @(x) e`, so a signal wake also drains
    // the detached-subscription list for its (signal, edge).
    bool                                               design_uses_detached_events_ = false;
    // True when some `x <= @e v` or `->> @(e) f` names a declared event as its
    // control. An event wakes through its object, not a signal edge, so every event
    // fire then also counts down the records registered on that object's trigger id.
    bool design_uses_event_triggers_ = false;
    // Head of the NBA pending chain (clotho.event_pend_head).
    llvm::GlobalVariable*                              event_pend_head_gv_ = nullptr;
    // The design's single detached `->> #d e` on-expiry thunk (clotho.ev_delay).
    // One rather than one-per-event: the timer node carries the handle.
    llvm::Function*                                    event_delay_thunk_ = nullptr;
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
    // Queue signals that are a whole nonblocking-assignment target (`q <= rhs`).
    // Each `q <= rhs` deep-copies the RHS into a staged rt_queue in a per-queue
    // runtime buffer (indexed after the array + aggregate buffers) in the Active
    // region; the NBA region moves that buffer into the live handle and wakes the
    // signal (Any-edge). See docs/design/queue-nba-and-nonpod.md.
    std::vector<Signal>     handle_nba_sigs_;
    // Container type of each handle_nba_sigs_ entry, parallel to it. Recorded from
    // the op rather than re-derived from the signal; see the registration comment.
    std::vector<Ref>        handle_nba_types_;
    // Signals taking a whole-string nonblocking assignment (`s <= t`). Their records
    // are the fourth range of the shared rt_nba_arr_* index space; a string needs
    // its own because the staged value is an OWNED 32-byte handle, not a plane word
    // and not a container handle (the record's move is rt_string_move).
    std::vector<Signal> string_nba_sigs_;

    // Whole-scalar targets of a nonblocking intra-assignment delay (`x <= #d y`,
    // StoreNbDelay). Each gets a dense id; a per-target record (rt_intra_*) holds
    // the sampled value on the wheel until `now + d`, then the NBA region commits
    // it (emit_intra_commit). See docs/design/intra-assignment-timing.md.
    std::vector<Signal>     intra_nba_sigs_;

    // The subset of intra_nba_sigs_ that are inertial continuous-assignment delay
    // targets (`assign #d x = y`, StoreInertialDelay). Unlike a procedural `<= #d`
    // (a nonblocking assign that commits in the NBA region), a continuous assign
    // updates its net in the active region — visible to same-instant processes — so
    // these commit in a pre-active phase after the wheel tick (emit_active_run),
    // ahead of the active drain. See LRM 10.3.3 (docs/LRM_NOTES.md, note 9).
    std::vector<Signal>     inertial_sigs_;

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
    // The type of every instruction of the current body (ir/Typing.h), computed
    // when the body starts lowering.
    std::vector<ValueType> types_;
    // rt_string handle pointers of string-valued instructions, parallel to
    // values_ (a string never flows as a Plane).
    std::vector<llvm::Value*> str_vals_;
    // The current body's string rvalue temporaries, deinit'd at its exits.
    std::vector<llvm::Value*> str_temp_list_;
    // Open str_chg_snapshot brackets. Consulted only by lv_string_ptr's `Write`
    // check, and only for a property some `@` / `wait` names, so it is the
    // completeness device HandleUse::Mutate is for containers: a string write site
    // that forgets its bracket fails the build instead of parking a waiter forever.
    uint32_t str_chg_open_ = 0;
    // Memoized `%clotho.string` handle slot type.
    llvm::StructType* str_slot_ty_ = nullptr;
    // Memoized class object-body StructType per Kind::Class Ref (see class_body_ty).
    ankerl::unordered_dense::map<uint32_t, llvm::StructType*> class_body_ty_;
    // Memoized reaches_class_handle() per type Ref. Design-wide, not per module: the
    // answer depends only on the intern pool.
    ankerl::unordered_dense::map<uint32_t, bool> reaches_class_handle_;
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
    // Owning result buffers for aggregate-element queue pops in the current body,
    // with the element type each holds: released at every body exit
    // (build_agg_elem_temps).
    std::vector<std::pair<llvm::Value*, Ref>> agg_elem_temp_list_;
    // Per-pop-instruction result buffer (null for a non-aggregate element), snapshot
    // per fused member exactly like dynarr_ret_.
    std::vector<llvm::Value*> agg_elem_ret_;
    // Memoized rt_elem_leaf mirror type ({ i64 offset, ptr ops }).
    llvm::StructType* elem_leaf_ty_ = nullptr;
    // Memoized `%clotho.queue` handle slot type ({ ptr base, i64 head, len, cap }).
    llvm::StructType* queue_slot_ty_ = nullptr;
    // Memoized `%clotho.assoc` handle slot type ({ ptr impl }).
    llvm::StructType* assoc_slot_ty_ = nullptr;
    llvm::StructType* elem_ops_ty_ = nullptr;
    // Memoized rt_gc_* ABI mirrors (src/runtime/Gc.h); null until first asked for.
    llvm::StructType* gc_map_ty_     = nullptr;
    llvm::StructType* gc_roots_ty_   = nullptr;
    llvm::StructType* gc_frame_ty_   = nullptr;
    // The coverage model's four record types plus its config (Coverage.h).
    llvm::StructType* cover_range_ty_  = nullptr;
    llvm::StructType* cover_bin_ty_    = nullptr;
    llvm::StructType* cover_point_ty_  = nullptr;
    llvm::StructType* cover_type_ty_   = nullptr;
    llvm::StructType* cover_config_ty_ = nullptr;
    // rt_gc_container / rt_gc_container_table (Gc.h:249-261), the container-root ABI.
    llvm::StructType* gc_container_ty_       = nullptr;
    llvm::StructType* gc_container_table_ty_ = nullptr;
    llvm::StructType* gc_config_ty_  = nullptr;
    llvm::StructType* gc_mutator_ty_ = nullptr;
};

} // namespace clotho::ir::backend
