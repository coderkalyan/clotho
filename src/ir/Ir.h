#pragma once

// Process IR — models a single SystemVerilog process or process-like construct
// (always block, function, task). Frozen and immutable once built.
//
// Control flow is structured: there are no basic blocks. Region-bearing
// instructions (e.g. cond) hold nested instruction sequences as index ranges in
// the extra side-table. The top-level body is the IR's root region. All
// instructions live in one flat columnar store; the region tree gives ordering.
//
// Design:
//   * Index handles, not pointers. Every cross-reference is a 4-byte
//     strongly-typed enum index, so the stores stay relocatable and compact.
//   * Structure-of-arrays instruction store. The instruction is split into
//     parallel tag / payload / source-location columns (held directly by Ir)
//     so a pass that only reads tags never pulls payloads into cache.
//   * Compact untagged payload union (<=8 bytes). The discriminant lives in
//     the separate tag column and selects the active member via payload_kind().
//   * Flat u32 "extra" side-table for variable-length / overflow operands and
//     region bodies. Structs whose every field is a 4-byte handle are overlaid
//     onto the buffer with std::bit_cast.
//
// `Ir` is the immutable, finished product. `Builder` (Builder.h) owns the
// growable build-time state and hands back an `Ir` from finish().

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "Array.h"
#include "InternPool.h"

namespace clotho::ir {

// SSA-style handle to an instruction's result / the instruction itself.
enum class Index : uint32_t {};
// No instruction: the initial value of a handle assigned only on some paths, so
// a use on a path that never assigned it indexes out of range instead of reading
// instruction 0.
inline constexpr Index kNoIndex = static_cast<Index>(~uint32_t{ 0 });
// Offset into the flat u32 extra side-table.
enum class ExtraIndex : uint32_t {};
// Opaque, frontend-defined per-instruction source location (no reserved values).
// A point, not an extent: the instruction column is one of the hottest arrays in
// the compiler, so it stays 8 bytes per instruction. Constructs a report or an
// editor points AT carry a SourceSpan instead (see below).
enum class SourceLoc : uint64_t {};

// Source extent of one construct: [start, end], both inclusive-of-the-token-start
// in the frontend's encoding. `end == start` means only a point is known — a
// compiler-synthesized construct, or a diagnostic reported at a bare location.
//
// Deliberately NOT stored per instruction. It is attached to the construct-level
// entities that span-based tooling actually addresses (a process, a function, a
// diagnostic), where the count is thousands rather than millions and 16 bytes is
// free. Signals get a bare SourceLoc instead: there can be a million of them and
// "go to declaration" only needs a point.
struct SourceSpan {
    SourceLoc start{ 0 };
    SourceLoc end{ 0 };

    // True when the span carries any location at all. SourceLoc{0} is the
    // frontend's "no location" value (buffer id 0 is slang's invalid buffer).
    [[nodiscard]] bool valid() const {
        return start != SourceLoc{ 0 };
    }
};

// What a resolver can say about a source construct: enough to print a location,
// and enough to draw the snippet-and-underline block under it.
struct SourceText {
    // "<file>:<line>:<col>", or "<unknown>" when the location cannot be
    // resolved.
    std::string where;
    // The source line the construct starts on, verbatim and without its
    // newline. Empty when unavailable, which callers must tolerate.
    std::string line;
    // 1-based line and column of the construct's first character; 0 when
    // unknown.
    uint32_t line_no = 0;
    uint32_t column  = 0;
    // 1-based column one past the construct's last character, so the underline
    // spans [column, end_column). Equals `column` when no extent was recorded
    // or the construct runs onto another line — draw a single caret then.
    uint32_t end_column = 0;
};

// Optional hook turning a source extent into the above. The span's `end` is
// SourceLoc{0} for a construct recorded as a bare point.
//
// The IR core cannot decode a SourceLoc itself: the (buffer, offset) mapping
// lives in the slang-aware frontend, and keeping this library free of slang is
// the point of the clotho_ir_core / clotho split. So a caller that wants
// located output passes a resolver (in practice one built over
// `SourceContext`). Default-constructed means "no locations available", which
// is what tests want -- golden IR text must not depend on file paths.
using LocResolver = std::function<SourceText(SourceSpan)>;

// Opaque handle to a module signal (net / variable / port). Resolved by the
// enclosing module; the process IR never interprets it.
enum class Signal : uint32_t {};
// Handle to a function in the enclosing module's function table; the callee of a
// `call`. Resolved by the module, like Signal.
enum class FunctionId : uint32_t {};

// Which real math system function a `real_math` invokes (LRM §20.8.2, Table
// 20-4). Each takes real operands and returns a real; the LRM requires each to
// match the equivalent C standard math library function. Pow / Atan2 / Hypot
// take two operands, all others one. Kept in lockstep with the backend
// dispatch in LlvmBackend.
enum class RealMathOp : uint32_t {
    // $ln(x) -> C log(x).
    Ln,
    Log10,
    Exp,
    Sqrt,
    // $pow(x, y) -> C pow; also the lowering of the real ** operator (LRM
    // 11.4.3 leaves 0**negative and negative**non-integral unspecified — C pow
    // behavior is used).
    Pow,
    Floor,
    Ceil,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    // $atan2(y, x).
    Atan2,
    Hypot,
    Sinh,
    Cosh,
    Tanh,
    Asinh,
    Acosh,
    Atanh,
};

// Which probabilistic distribution function a `dist_call` selects (LRM
// §20.15.2). Kept in lockstep with the rt_dist_* runtime entries and the
// backend dispatch. Uniform doubles as the seeded $random(seed) form
// (§20.15.1), which is rtl_dist_uniform over the full int32 span.
enum class DistOp : uint32_t {
    Uniform,
    Normal,
    Exponential,
    Poisson,
    ChiSquare,
    StudentT,
    Erlang,
};

// A struct may be overlaid on the extra buffer iff it is trivially copyable, a
// whole number of u32 words wide, and padding-free (every field a 4-byte
// handle/enum — flags are u32, never bool). Padding would bit_cast
// indeterminate bytes into the extra buffer, making the IR bytes
// non-deterministic across identical runs (a hazard for hashing and future
// serialization); has_unique_object_representations rejects it at compile time.
template<class T>
concept ExtraPayload =
  std::is_trivially_copyable_v<T> && (sizeof(T) % sizeof(uint32_t) == 0) && std::has_unique_object_representations_v<T>;

// Half-open range [start, end) of u32 words in the extra buffer. Doubles as a
// region: a run of instruction indices in nested-execution order.
struct ExtraSlice {
    // First word covered by the slice.
    ExtraIndex start;
    // One past the last word covered by the slice.
    ExtraIndex end;
};

// Decode a record written to an extra side-table by Builder::add_extra. Shared by
// every reader of the table: a finished `Ir`, a `Builder` still growing one, and
// the `IrView` that value typing reads either through.
template<ExtraPayload T>
T decode_extra(std::span<const uint32_t> extra, ExtraIndex at) {
    constexpr size_t        n    = sizeof(T) / sizeof(uint32_t);
    const auto              base = static_cast<uint32_t>(at);
    std::array<uint32_t, n> words{};
    for (size_t i = 0; i < n; ++i) {
        words[i] = extra[base + i];
    }

    return std::bit_cast<T>(words);
}

// A slice of the extra table reinterpreted as a run of instruction indices.
//
// NOTE: viewing u32 extra words as enum-class handles (here and in
// wait_edges) is formally an aliasing violation — an enum is not
// aliasing-compatible with its underlying type ([basic.lval]). Accepted
// deliberately: the representation is identical, every compiler this
// builds on reads it as intended, and the alternative (per-element
// copies) costs the zero-copy view this layout exists for.
inline std::span<const Index> decode_slice(std::span<const uint32_t> extra, ExtraSlice s) {
    const auto a = static_cast<uint32_t>(s.start);
    const auto b = static_cast<uint32_t>(s.end);

    return { reinterpret_cast<const Index*>(extra.data() + a), b - a };
}

// Extra payload for `strength_res`: the driver slot values and a parallel run
// of packed per-driver (strength0<<8 | strength1) constants (one u32 word per
// driver, low 16 bits used). `values` are the SSA operands; `strengths` are
// compile-time constants read directly by the backend fold.
struct StrengthResInfo {
    // The driver slot values, in driver order (the SSA operands).
    ExtraSlice values;
    // Packed (strength0<<8 | strength1) per driver, parallel to `values`.
    ExtraSlice strengths;
};

// Extra payload for `select`: the two selected values (the condition is the op).
struct Select {
    // Value chosen when the condition is true.
    Index a;
    // Value chosen when the condition is false.
    Index b;
};

// Extra payload for `store_nb_delay`: the sampled RHS value and the delay in
// ticks. The whole-signal lvalue is the enclosing op_extra `op`.
struct StoreNbDelayInfo {
    // Value stored after the delay (sampled in the current instant).
    Index value;
    // Delay in scaled ticks before the store lands.
    Index delay;
};

// One (trigger signal, edge) of a nonblocking intra-assignment event. Two u32
// fields, so a run of them overlays the extra buffer's words exactly -- the same
// packing WaitEdge uses for a `wait` event list.
struct IntraTrigger {
    // Signal whose edge counts down the store.
    Signal signal;
    // Edge to trigger on (ir::Edge value).
    uint32_t edge;
};

// Extra payload for `store_nb_event`: the sampled RHS value, the repeat count
// (constant 1 for a plain `@e`), and the (trigger signal, edge) list the store
// waits on. The whole-signal lvalue is the enclosing op_extra `op`.
struct StoreNbEventInfo {
    // Value stored when the trigger fires (sampled in the current instant).
    Index value;
    // Number of trigger occurrences to wait for (sampled now); 1 for plain `@e`.
    Index count;
    // The triggers, as back-to-back IntraTrigger records. A plain `@e` lists one;
    // `@(a or b)` lists each, and the store matures on whichever brings the
    // countdown to zero FIRST -- it is then removed from the others (LRM 9.4.5's
    // "N occurrences of the event", where the event is the whole list).
    ExtraSlice trigs;
};

// Extra payload for `part_select`: a constant [msb:lsb] slice of the base.
struct PartSelect {
    // Most-significant selected bit.
    uint32_t msb;
    // Least-significant selected bit.
    uint32_t lsb;
};

// Extra payload for `dyn_part_select`: a runtime-offset, constant-width slice of
// the base (a SystemVerilog indexed part-select `base[off +: width]` /
// `base[off -: width]`). `offset` is the value of the low selected bit (the
// frontend already folds the `-:` form to its low bit); `width` is the constant
// number of selected bits.
struct DynPartSelect {
    // Value of the least-significant selected bit position.
    Index offset;
    // Number of selected bits (constant).
    uint32_t width;
};

// Extra payload for `elem_select`: a runtime element index into an unpacked
// array (the base is the op). `index` is the selector value (declared-range
// coordinate, normalized against the array's bounds in the backend);
// `array_type` is the interned UnpackedArray type being indexed, giving the
// element stride and the declared [left:right] bounds.
struct ElemSelect {
    // Element index value, in declared-range coordinates.
    Index index;
    // The UnpackedArray type being indexed.
    Ref array_type;
    // Whether the ACCESS this select serves writes the place it designates.
    //
    // Only a select whose base is a DEFERRED associative-array element reads it:
    // resolving that base needs get_ptr (a read must not create the entry, LRM
    // 7.8.6) or put_ptr (a write must), and unlike a bare `a[k]` — where the
    // Load / Store consumer picks — a second select must resolve the base eagerly to
    // do its own ring / bounds arithmetic, before any consumer is in sight. Every
    // other base resolves the same either way and ignores this.
    //
    // A word rather than a bool: an extra payload must have no padding
    // (ExtraPayload's unique-object-representation requirement).
    uint32_t for_write = 0;
};

// Extra payload for `arr_slice`: a constant-bounds slice of a fixed unpacked
// array (LRM 7.4.5, `busB = busA[7:6]`). The op is the base array lvalue; the
// slice is position-0-anchored (a downstream elem_select's index normalizes
// against `slice_type`'s bounds, added to the base pre-advance), so it reads /
// writes as an ordinary array of `slice_type`. `first_pos` is the 0-based flat
// position of the slice's first element within the base array's storage.
struct ArrSlice {
    // The slice's own UnpackedArray type (gives element stride + declared bounds).
    Ref slice_type;
    // 0-based storage position of the slice's first element in the base array.
    uint32_t first_pos;
};

// Extra payload for `agg_copy`: a whole-object blocking copy of one aggregate
// lvalue into another (both of `type`). The op is the destination lvalue; `src`
// the source lvalue. Lowered to a single llvm.memcpy of the aggregate storage —
// valid only when the aggregate has no string (heap-owning) leaf, which the
// frontend checks before emitting it.
struct AggCopyInfo {
    // Source aggregate lvalue.
    Index src;
    // The aggregate type (gives the storage size to copy).
    Ref type;
};

// Extra payload for `inside_set`: the `inside` operator's membership test of
// the op against a set (LRM 11.4.13), OR-reduced across members with LRM
// 11.4.5 X-propagation. The OR is order-insensitive, so members are grouped
// by kind: `values` is a run of singular member values, each tested with
// wildcard equality (only the member's own x/z bits are wildcards); `ranges`
// is a run of back-to-back [lo, hi] operand pairs tested as
// `sel >= lo && sel <= hi`. All operands are pre-resized to the selector type.
struct InsideSetInfo {
    // Singular member values (wildcard-equality tests).
    ExtraSlice values;
    // [lo, hi] range operand pairs, two indices per range.
    ExtraSlice ranges;
    // Nonzero when the ranges compare as signed numbers: the selector's type decides
    // (LRM 11.4.13 tests `sel >= lo && sel <= hi`, LRM 11.8.1). Chosen by the
    // frontend; equality against the singular members does not depend on it.
    uint32_t ranges_signed;
};

// Extra payload for `pow`: the exponent and the signedness of both operands, which
// decide Table 11-4's negative-operand corners. The base is the instruction's op;
// the exponent is self-determined and may differ in width and domain.
struct PowInfo {
    // The exponent value.
    Index exp;
    // Nonzero when the base is signed.
    uint32_t base_signed;
    // Nonzero when the exponent is signed.
    uint32_t exp_signed;
};

// Extra payload for `agg_eq`: whole-aggregate equality of two unpacked
// aggregate lvalues of `type` (LRM 11.2.2). The op is the left lvalue.
struct AggEqInfo {
    // Right-hand aggregate lvalue.
    Index rhs;
    // The aggregate type (gives the member layout to compare).
    Ref type;
};

// Extra payload for `member_select`: a member of an unpacked struct (the base is
// the op). Unlike a packed struct member (a constant part-select over a flat
// bit-vector), an unpacked struct is an aggregate whose members have their own
// storage, so a member is addressed by its declaration-order field index into
// the struct's backing StructType, not a bit offset. `field_index` is the
// member's position in declaration order; `struct_type` is the interned
// Kind::Struct (is_packed=false) type, giving the member layout.
struct MemberSelect {
    // Declaration-order field index of the selected member.
    uint32_t field_index;
    // The unpacked Kind::Struct type being accessed.
    Ref struct_type;
};

// Extra payload for `iface_ref`: which elaborated interface instance a virtual
// interface handle names, as its InstanceScope id. The backend maps the id to
// that instance's runtime context record and yields its address.
struct IfaceInst {
    uint32_t scope;
};

// Extra payload for `iface_signal`: which member of the interface definition is
// being reached through the handle, named by a REPRESENTATIVE signal -- any real
// instance's copy of that member. Every instance of one definition lays its
// members out identically, so the representative supplies the def-local field
// index while the handle supplies the base. See docs/design/interfaces.md.
struct IfaceMember {
    Signal signal;
};

// Extra payload for `dyn_array_new` / `dyn_array_copy`: the tag's single
// non-handle operand (element count for New, source handle for Copy) plus the
// interned DynArray type (element stride / slot), independent of the target
// signal — so the op works for a module signal or a frame local. The handle
// lvalue is the instruction's op.
struct DynArrayArg {
    // Element count (new) or source handle lvalue (copy).
    Index arg;
    // The interned DynArray type of the target.
    Ref array_type;
};

// Extra payload for `dyn_array_new_init`: arr = new[size](src) (LRM 7.5.1) —
// reallocate to `size` elements, then copy min(size, src.len) elements from
// the source handle lvalue. The handle lvalue is the op.
struct DynArrayNewInitInfo {
    // Element count operand.
    Index size;
    // Source dynamic-array handle lvalue.
    Index src;
    // The interned DynArray type of the target.
    Ref array_type;
};

// Extra payload for the one-operand queue tags (QueuePushBack / QueuePushFront
// / QueueDeleteIdx / QueueCopy): the tag's single non-handle operand plus the
// interned Queue type of the target (element stride / slot / bound),
// independent of the target signal — so the op works for a module signal or a
// frame local. The handle lvalue is the instruction's op.
struct QueueArg {
    // Element value (push), position (delete-idx), or source handle (copy).
    Index arg;
    // The interned Queue type of the target.
    Ref queue_type;
};

// Extra payload for `queue_insert`: q.insert(idx, e) inserts the element value
// `val` before position `idx` (LRM 7.10.2.4). The handle lvalue is the op.
struct QueueInsertInfo {
    // Position operand.
    Index idx;
    // Element value operand.
    Index val;
    // The interned Queue type of the target.
    Ref queue_type;
};

// Extra payload for `queue_append_range`: append `src`'s live elements at
// logical indices [lo, hi] (inclusive, i64 values; a full-queue append passes
// lo = 0 and hi = a to-end sentinel) onto the dst handle, materializing a queue
// slice `q[a:b]` (LRM 7.10.4) or a queue-typed concatenation operand. Both the
// dst (op) and src are handle lvalues; the dst never aliases src.
struct QueueSliceInfo {
    // Source queue handle lvalue.
    Index src;
    // Low / high logical index operands (i64 values).
    Index lo;
    Index hi;
    // The interned Queue type of the target.
    Ref queue_type;
};

// Extra payload for the one-operand associative-array tags (AssocSelect,
// AssocExists, AssocDeleteKey, AssocCopy, AssocFirst/Last/Next/Prev). `arg` is
// the key value (select/exists/delete-key), the source handle lvalue (copy), or
// the key-variable lvalue (first/last/next/prev). The handle lvalue is the op.
struct AssocArg {
    // Key value / source handle / key-variable lvalue, per the tag.
    Index arg;
    // The interned Assoc type of the target (element = value, fields_id = key,
    // right = wildcard flag).
    Ref assoc_type;
    // Whether the access this select serves writes the place it designates; read
    // only when the BASE is itself a deferred assoc element (`a[k1][k2]`), for the
    // same reason ElemSelect::for_write exists. A word, not a bool: see ElemSelect.
    uint32_t for_write = 0;
};

// Extra payload for `read_mem`: a $readmemh / $readmemb load of a file into an
// unpacked-array memory. The filename value is the instruction's `op`: either a
// packed-ASCII vector (the backend spills its must-bits to a byte buffer) or a
// string value (the backend reads the rt_string handle's bytes at the call site).
// `target` is the destination memory signal. `start` / `finish` are optional
// address-bound operands (valid per `has_start` / `has_finish`). `hex` selects hex
// vs binary digits.
struct ReadMemInfo {
    // Destination memory signal.
    Signal target;
    // Optional first / last address operands (declared-range coordinates).
    Index start;
    Index finish;
    // 1 for $readmemh, 0 for $readmemb.
    uint32_t hex;
    // Whether the start / finish operands are present.
    uint32_t has_start;
    uint32_t has_finish;
};

// Extra payload for `call`: the callee plus its already-lowered argument values,
// in formal-parameter order (each resized to the parameter type by the frontend).
struct CallInfo {
    // Function being called, resolved against the module's function table.
    FunctionId func;
    // The argument value operands, in order.
    ExtraSlice args;
};

// Extra payload for `task_call`: the callee task plus one operand per formal, in
// declaration order. An input formal's operand is a value (resized to the
// parameter type); an output / inout formal's operand is an lvalue leaf
// (Signal / Local) naming a caller temp the backend passes by pointer and the
// frontend copies to/from the actual around the call. Directions come from the
// callee's Function::arg_dirs.
struct TaskCallInfo {
    // Task being called, resolved against the module's function table.
    FunctionId func;
    // One operand per formal, in order (value for input, lvalue for output/inout).
    ExtraSlice args;
};

// "Not an interface dispatch": VirtualCallInfo::iface_index of an ordinary
// virtual call, which resolves through the receiver's vtable slot instead.
inline constexpr uint32_t kNoIface = UINT32_MAX;

// Extra payload for `CallVirtual` (LRM 8.20): a dynamically-dispatched virtual
// method call. Resolved via the object's vtable at run time rather than a static
// FunctionId.
struct VirtualCallInfo {
    // The statically-resolved method (the one slang bound by the handle's static
    // type). Every override shares its signature and return type (LRM 8.20), so it
    // supplies both the indirect-call FunctionType and this call's result type; it
    // is NOT the function actually called.
    FunctionId func;
    // Vtable slot index of the virtual method (stable across the whole hierarchy).
    uint32_t slot;
    // Design-wide interface-method index when this call dispatches through an
    // interface-class HANDLE (LRM 8.26): `slot` is then meaningless, because slot
    // numbers are per single-inheritance hierarchy and two unrelated implementers
    // of one interface do not agree on one. kNoIface for an ordinary virtual call.
    // See docs/design/interface-classes.md §5.
    uint32_t iface_index;
    // Static class type of the receiver: gives the object-body layout (the vtable
    // pointer sits at body field 0 of a polymorphic class).
    Ref class_ref;
    // Argument value operands (this handle first, then the formals), like CallInfo.
    ExtraSlice args;
};

// Join discipline of a fork block (LRM 9.3.2). V1 implements Join only.
enum class ForkKind : uint32_t {
    Join,     // parent blocks until all children complete
    JoinAny,  // parent blocks until the first child completes (V2)
    JoinNone, // parent does not block (V2)
};

// Extra payload for `Fork` (LRM 9.3.2). Each child statement of the fork was
// lowered into its own fork-child task (Function::is_fork_child); `children`
// holds their FunctionIds as raw u32s (one per parallel process).
//
// `args` are the GC-box HANDLES of the enclosing body's frame locals that any
// child reads or writes -- a fork inside a task or a function, where a child's
// references to the enclosing formals and automatics cannot resolve to module
// signals. Each is passed to EVERY child as an ArgDir::In formal, in this order,
// so formal k of each child holds the box for args[k]'s variable (lower/Statements.cpp's
// lower_fork / lower/Subroutines.cpp's build_fork_child). They are rvalues, not places: the shared
// variable lives in the box, which the GC keeps alive off each child's own frame,
// so a child may outlive the activation that declared the variable. That holds for
// every join kind, `join` included -- see docs/design/fork-join.md V4 for why the
// predecessor's ArgDir::Ref pointer into the parent frame was not sound even
// there. The list is shared rather than per-child because a child that never
// touches a capture only carries one unused scalar parameter, and one list keeps
// the children's ABIs identical. Empty for a fork in a process body, which is
// every fork whose parent variables are module signals.
struct ForkInfo {
    // FunctionIds of the child bodies, stored as u32 (FunctionId is a u32 enum).
    ExtraSlice children;
    // Captured box-handle operands, one In formal per entry in every child.
    ExtraSlice args;
    ForkKind   kind;
};

// Which built-in `process` method (LRM 9.7) a ProcQuery / ProcCtl / ProcWait
// instruction performs. Split across three tags rather than one, so value-ness and
// suspension are properties of the TAG (which every analysis keys on) rather than of
// the payload: ProcQuery yields a value, ProcCtl does not, ProcWait parks.
// See docs/design/process-class.md §10.
// u32-wide (not u8) so ProcCtlInfo has no padding: the ExtraPayload concept
// requires a unique object representation.
enum class ProcOp : uint32_t {
    // ProcQuery.
    Self,         // process::self() -> a handle on the calling thread
    Status,       // p.status() -> the `state` enum value
    RandstateGet, // p.get_randstate() -> string
    // ProcCtl.
    Kill,         // p.kill(): end p and every descendant
    Suspend,      // p.suspend()
    Resume,       // p.resume()
    Srandom,      // p.srandom(seed)
    RandstateSet, // p.set_randstate(s)
    DisableFork,  // `disable fork` (LRM 9.6.3): kill every descendant of self
    // ProcWait.
    Await,        // p.await(): park until p completes
    WaitFork,     // `wait fork`: park until every descendant completes
};

// Extra payload for the three `process` tags.
struct ProcCtlInfo {
    // SSA operands, in order: the receiver handle (absent for `self`, `disable fork`
    // and `wait fork`, which name the calling thread), then the value argument of
    // `srandom` / `set_randstate`.
    ExtraSlice args;
    // Result type, for ProcQuery only: the `process` class Ref for Self, the 32-bit
    // enum for Status, the string type for RandstateGet. Invalid elsewhere.
    Ref    type = Ref::Invalid;
    ProcOp op   = ProcOp::Self;
};

// Which functional-coverage primitive a CoverQuery / CoverCtl instruction
// performs (LRM 19). Split across two tags for the reason the `process` family is
// split across three: value-ness is a property of the TAG every analysis keys on,
// not of the payload. See docs/design/functional-coverage.md.
//
// A covergroup instance is runtime-owned storage, not a collected object, and a
// covergroup handle is its 64-bit address; the whole static coverage model (bins,
// ranges, options) is emitted as read-only constants beside the design, so every
// op here is a call into the coverage runtime with no IR-visible state.
// u32-wide (not u8) so CoverInfo has no padding: the ExtraPayload concept
// requires a unique object representation.
enum class CoverOp : uint32_t {
    // CoverQuery.
    New,           // `cg_type ident = new` -> a covergroup handle
    Ctx,           // the enclosing object captured at construction (embedded groups)
    First,         // the first live instance of a covergroup type, or null
    Next,          // the next instance after this one, or null
    InstCoverage,  // h.get_inst_coverage() -> real
    InstCovered,   // its covered-bin count (the `ref int` first argument)
    InstTotal,     // its total-bin count (the `ref int` second argument)
    PointCoverage, // h.cp.get_inst_coverage() -> real
    PointCovered,
    PointTotal,
    TypeCoverage, // cg_type::get_coverage() -> real, over every instance
    TypeCovered,
    TypeTotal,
    DesignCoverage, // $get_coverage() (LRM 19.9)
    // CoverCtl.
    Hit,     // one coverpoint's sampled value: match bins and count
    Start,   // h.start() / h.stop()
    SetName, // h.set_inst_name(s)
};

// Extra payload for the two coverage tags.
struct CoverInfo {
    // SSA operands, in order: the covergroup handle (absent for the type-level and
    // design-level queries, which name a type index or nothing), then the op's
    // value arguments -- for Hit the sampled value and its unknown-bit mask, for
    // Start the 1-bit enable, for SetName the string.
    ExtraSlice args;
    // Result type, for CoverQuery only: the 64-bit handle type for New / Ctx /
    // First / Next, the real type for a coverage percentage, 32-bit signed for a
    // bin count. Invalid for CoverCtl.
    Ref type = Ref::Invalid;
    // Which covergroup type in Module::cover_types this op addresses. Meaningful
    // for New and the type-level queries; the instance ops read the type off the
    // instance record instead.
    uint32_t type_idx = 0;
    // Which coverpoint of that type, for Hit and the coverpoint-level queries.
    uint32_t point = 0;
    CoverOp  op    = CoverOp::New;
};

// Which wait-queue primitive a SyncWait / SyncCtl instruction performs, for the
// built-in `semaphore` (LRM 15.3) and `mailbox #(T)` (LRM 15.4). Split across two
// tags for the reason the `process` family is split across three: suspension is a
// property of the TAG, which every analysis keys on.
// u32-wide (not u8) so SyncInfo has no padding: the ExtraPayload concept requires a
// unique object representation.
enum class SyncOp : uint32_t {
    // SyncWait: link this thread onto the queue (if it is not already on it) and
    // park. A node stays linked across an unsatisfied retest, which is what
    // preserves arrival order (LRM 15.3.3 / 15.4.5: "their arrival order shall be
    // preserved").
    Park,
    // SyncCtl: take this thread's node off the queue, then wake the new head --
    // one `put` of several keys can satisfy several waiters, and only the thread
    // leaving knows the queue changed.
    Leave,
    // SyncCtl: wake the head of the queue, if any. The woken thread re-tests.
    WakeHead,
    // SyncCtl: wake every waiter on the queue. `mailbox::peek` waiters take this,
    // because a peek consumes nothing (LRM 15.4.7's "any process blocked in either
    // a peek() or get() operation shall become unblocked").
    WakeAll,
};

// Extra payload for the two wait-queue tags.
struct SyncInfo {
    // SSA operands, in order: the queue's head-word lvalue, then its tail-word
    // lvalue. Both are ordinary members of the object's synthetic body, so the
    // backend needs no layout knowledge -- the frontend selects the field.
    ExtraSlice args;
    // Wait SITE identity, unique design-wide. A `Park` and the `Leave` that
    // discharges it carry the same site, which is how they share one
    // frame-resident node (the backend allocates one per site per body).
    uint32_t site = 0;
    SyncOp   op   = SyncOp::Park;
};

// Extra payload for `cond`: the then / else regions (the condition is the op).
struct Regions {
    // Instructions executed when the condition is true.
    ExtraSlice then_;
    // Instructions executed when the condition is false.
    ExtraSlice else_;
    // Hint (1/0) that the condition is rarely true; biases backend branch
    // weights toward the else path (e.g. a violation check).
    uint32_t unlikely = 0;
};

// Extra payload for `select_lazy`: a value-producing conditional (the predicate
// is the op). Unlike `cond` (statement-only) each branch designates a result
// value; the backend merges them with a PHI. Used for short-circuit &&/||,
// where the then-branch evaluates the right operand and the else-branch yields
// the dominating constant.
struct SelectLazy {
    // Instructions evaluated when the predicate is a definite 1.
    ExtraSlice then_;
    // Value produced by the then-branch.
    Index then_val;
    // Instructions evaluated when the predicate is 0.
    ExtraSlice else_;
    // Value produced by the else-branch.
    Index else_val;
};

// Extra payload for `loop`. `cond` holds the instructions that recompute the
// predicate each iteration; `pred` is the 1-bit predicate index defined within
// `cond` (a constant 1 when the source condition is omitted). `body` is the
// loop body; `step` is the per-iteration update (empty for while / forever).
struct LoopInfo {
    // Instructions recomputing the predicate at the top of each iteration.
    ExtraSlice cond;
    // The 1-bit predicate value, defined inside `cond`.
    Index pred;
    // Loop body, executed while the predicate is a definite 1.
    ExtraSlice body;
    // Per-iteration update, executed after the body (empty for while / forever).
    ExtraSlice step;
    // do-while (1/0): run the body once before the first predicate test. The
    // backend branches entry -> body instead of entry -> header (LRM 12.7.1).
    uint32_t bottom_test;
};

// Extra payload for `named_block`: the block's statements, plus the label a
// `disable` names it by. Only a named block that some `disable` in the same
// lowering unit targets gets an instruction at all -- an ordinary begin/end is
// flattened into its enclosing region, since its name is otherwise only visible
// to %m -- because `disable` needs a place to branch TO: the end of the block,
// which can sit in the middle of a region (LRM 9.6.2 example 2).
struct BlockInfo {
    // Statements of the block.
    ExtraSlice body;
    // Id the Disable / DisableRemote insts targeting this block carry. Allocated
    // per targeted block symbol, and slang gives every elaborated instance its
    // own StatementBlockSymbol, so one id already names one block IN one instance
    // -- there is no separate instance dimension to resolve.
    uint32_t label;
    // Whether some `disable` names this block from outside it (a sibling block, or
    // another process). Only such a block carries the occupant bookkeeping a
    // remote abort needs; a purely lexical target stays a bare region plus a
    // branch, as in V1.
    uint32_t remote;
};

// A BlockInfo::remote / block-target id naming no target: the block is a purely
// lexical `disable` target, so it carries no occupant bookkeeping.
inline constexpr uint32_t kNoDisableTarget = UINT32_MAX;

// Extra payload for one arm of a `switch`: the constant case-value keys that
// select this arm, and the arm's statement body. `keys` is a run of u64 case
// values (2 words each; decode with Ir::region); several keys share one body
// for a grouped case item (`2, 3: stmt`). `body` is the region executed on a
// match.
struct SwitchArm {
    // Constant case-value keys selecting this arm (u64 each).
    ExtraSlice keys;
    // Statement region run on a match.
    ExtraSlice body;
};

// Extra payload for `switch` (payload: op_extra -> SwitchInfo; the op is the
// selector value). A constant, deduped, wildcard-free `case` lowered to a
// multiway branch: the backend packs the selector's planes into an integer key
// ({may^must, must}, must on the low bits) so a clean value keys on its bare
// bits (dense -> jump table) and any x/z selector lands outside the clean key
// range and falls to `default_`. `arms` is a run of SwitchArm records.
struct SwitchInfo {
    // Run of SwitchArm records (each sizeof(SwitchArm)/4 words).
    ExtraSlice arms;
    // Default region, run when no key matches (empty when the case had none).
    ExtraSlice default_;
    // Selector VALUE width, 1..64, or 0 when the frontend cannot state one and
    // coverage must decline.
    //
    // Not the key-encoding width: a 4-state selector's key packs value and unknown
    // bits into 2*width, which is not the space fullness is asking about. Recorded
    // because coverage cannot recover it -- keys are bare u64 and the IR has no
    // type query for an instruction's result.
    uint32_t sel_width;
    // Non-zero when the selector is 4-state. A fully enumerated case is then
    // complete for SYNTHESIS but not for simulation: `case` matches with ===, so an
    // x/z selector matches no arm and the signal holds. Legalization takes the
    // synthesis reading and reports the divergence.
    uint32_t sel_four;
    // Case pragmas the source carried: bit 0 `full_case`, bit 1 `parallel_case`.
    // `unique`/`priority`/`unique0` map onto the same two bits.
    //
    // Recorded rather than acted on at lowering time. yosys's load-bearing
    // full_case handling lives in its FRONTEND, so its legalizer cannot undo a hold
    // the lowering already committed to; §8 makes the base slot a property of the
    // analysis precisely so that cannot happen here.
    uint32_t pragmas;
};

// One value operand of a format-family op ($display, $sformat, $sformatf, the
// severity tasks, ...): the value, and whether the decimal conversions print it as
// a signed number (LRM 21.2.1.2). Signedness is the language's, not a property of
// the value's bits, so the frontend says it here rather than the backend guessing it
// from a type.
struct FmtArg {
    Index value;
    bool  is_signed = false;
};

// Extra payload for `display` / `fdisplay`: a $display / $write / $fdisplay /
// $fwrite system task. The format-string bytes live packed little-endian
// (4 chars per u32 word) at `fmt` for `fmt_len` bytes; `args` is the slice of
// value operands the format specifiers consume, in order; `newline` is 1 for
// the $*display forms (append '\n') and 0 for the $*write forms. The
// destination is the tag's: Display writes the standard-output MCD, FDisplay
// carries the descriptor as its primary operand.
struct DisplayInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // 1 => append a trailing newline ($display / $fdisplay); 0 => none.
    uint32_t newline;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
    // One word per operand of `args`: 1 when it prints as a signed number.
    ExtraSlice signs;
};

// Extra payload for `finish`: a $finish / $stop. `arg` is the diagnostic level
// (0/1/2; LRM Table 20-1: 0 prints nothing, 1 time+location, 2 adds resource
// stats). `banner`/`banner_len` are the packed bytes of the compile-time banner
// prefix "<file>:<line>: <task>" that the runtime completes with the simulation
// time when `arg` >= 1; the task name is baked in so $finish and $stop differ.
struct FinishInfo {
    // First extra word of the packed banner-prefix bytes.
    ExtraIndex banner;
    // Banner-prefix length in bytes.
    uint32_t banner_len;
    // Diagnostic level (0/1/2), defaulting to 1 when the source omits it.
    uint32_t arg;
};

// Extra payload for `case_viol`: a unique/priority violation report (LRM
// 12.4.2 / 12.5.3). `banner` / `banner_len` are the packed bytes of the
// compile-time "<file>:<line>: " prefix; `kind` selects the message (0: no
// item matched, 1: overlapping items).
struct CaseViolInfo {
    // First extra word of the packed banner-prefix bytes.
    ExtraIndex banner;
    // Banner-prefix length in bytes.
    uint32_t banner_len;
    // Violation-message selector: 0 no-match, 1 overlap.
    uint32_t kind;
};

// Extra payload naming a compiler-synthesized zero-formal task that some
// instruction defers into a later region: the print body of `strobe_pend`
// (Postponed) and `monitor_set` (LRM 21.2.2), and the action block of
// `reactive_pend` (Reactive, LRM 4.4.2.5). One FunctionId with one meaning, so
// the three share a payload rather than declare identical structs.
struct StrobePendInfo {
    // The synthesized zero-formal task: a $strobe / $monitor format-args body,
    // or a concurrent assertion's action block.
    FunctionId func;
};

// Extra payload for `expect_wait`: which monitor's attempt the executing process
// is about to block on (LRM 16.17). An index into Module::monitors, which is what
// the backend keys the monitor's waiter queue by.
//
// The verdict is deliberately NOT here. An `expect`'s pass and fail code are
// ordinary Reactive-region action blocks the monitor pends (Tag::ReactivePend),
// exactly as an assert's are, so the process that resumes needs to know only THAT
// its attempt resolved -- never how.
struct ExpectInfo {
    uint32_t monitor = 0;
};

// Extra payload for `randc_next`: the state of one `randc` property's cycle (LRM
// 18.6). The 256-bit "already used this cycle" bitmap is the instruction's `op`
// operand, so this record carries only what does not fit there.
//
// The RNG lvalue lives here rather than in a second payload slot because op_extra
// holds exactly one operand plus one extra handle, and both lvalues are genuine
// operands: Ir::operands() reports this one too, so a def-use walk sees the stream
// the pick advances instead of just the bitmap it marks.
struct RandcInfo {
    // The object's xoshiro256++ state lvalue (4 x `bit [63:0]`, LRM 18.14.2).
    Index rng;
    // Values in one cycle: `1 << width`, with width <= 8 (wider randc is refused).
    uint32_t nvals;
};

// Severity level of a `severity` instruction. Selects the diagnostic label the
// runtime prints (LRM 20.10); Fatal is `$fatal` (the frontend follows the report
// with a Finish carrying the finish number, so the report itself never
// terminates).
enum class Severity : uint32_t {
    Info,
    Warning,
    Error,
    Fatal,
};

// Extra payload for `severity`: an $info / $warning / $error / $fatal report
// (LRM 20.10) or an immediate-assertion default failure (LRM 16.3). `banner` /
// `banner_len` are the packed bytes of the compile-time "<file>:<line>: " prefix
// the runtime completes with the message. `fmt` / `fmt_len` are the packed
// $display-style format bytes; `args` the value operands its specifiers consume
// (both empty for a bare `$error;` or a default assertion failure). `level` is
// the ir::Severity label. `defer` selects the scheduling region for a deferred
// immediate assertion action (LRM 16.4): 0 = immediate (the report/action runs
// where it is lowered), 1 = observed deferred (`assert #0`, matures + fires in
// the Observed region), 2 = final deferred (`assert final`, Postponed region).
struct SeverityInfo {
    // First extra word of the packed banner-prefix bytes.
    ExtraIndex banner;
    // Banner-prefix length in bytes.
    uint32_t banner_len;
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // ir::Severity label.
    uint32_t level;
    // Deferral region: 0 immediate, 1 observed (#0), 2 final. See LRM 16.4.
    uint32_t defer;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
    // As DisplayInfo::signs.
    ExtraSlice signs;
};

// Extra payload for `fseek`: $fseek(fd, offset, operation) — the descriptor is
// the instruction's op; these are the remaining two value operands.
struct FseekInfo {
    // Byte-offset operand.
    Index offset;
    // Seek-origin operand (0/1/2 per C fseek whence).
    Index operation;
};

// Extra payload for `real_math_binary`: the second real operand plus the
// ir::RealMathOp selecting the routine (Pow / Atan2 / Hypot); the first
// operand is the instruction's op.
struct RealMathBinaryInfo {
    // Second real operand (exponent / x / second leg).
    Index rhs;
    // ir::RealMathOp value.
    uint32_t fn;
};

// Extra payload for `dist_unary`: a one-parameter distribution function
// ($dist_exponential / $dist_poisson / $dist_chi_square / $dist_t, LRM
// §20.15.2). The seed word is the instruction's op.
struct DistUnaryInfo {
    // The distribution parameter (mean / degrees of freedom).
    Index p1;
    // ir::DistOp value.
    uint32_t fn;
};

// Extra payload for `dist_binary`: a two-parameter distribution function
// ($dist_uniform / $dist_normal / $dist_erlang, LRM §20.15.2) or seeded
// $random (§20.15.1, Uniform over the full int32 span). The seed word is the
// instruction's op.
struct DistBinaryInfo {
    // First distribution parameter.
    Index p1;
    // Second distribution parameter.
    Index p2;
    // ir::DistOp value.
    uint32_t fn;
};

// `SformatInfo::fmt_value` / `SformatfInfo::fmt_value` is absent: the format is
// the constant bytes at `fmt`, validated and %p-expanded at lower time.
inline constexpr Index kNoFormatValue = static_cast<Index>(~uint32_t{ 0 });

// Design ticks per the calling module's time unit -- the multiplier a %t operand
// needs (LRM §20.4.3) -- carried as two words rather than one uint64_t: an extra
// payload must have no padding (see the ExtraPayload concept), and an 8-byte
// member would introduce some in the odd-worded SformatfInfo.
struct TickScale {
    uint32_t lo;
    uint32_t hi;

    uint64_t value() const {
        return (uint64_t{ hi } << 32) | uint64_t{ lo };
    }

    static TickScale of(uint64_t v) {
        return TickScale{ static_cast<uint32_t>(v), static_cast<uint32_t>(v >> 32) };
    }
};

// Extra payload for `sformat`: $sformat / $swrite (LRM §21.3.3) format the value
// args per the packed format-string bytes at `fmt` (`fmt_len` bytes) and store the
// result into the destination string lvalue `dst`. `args` is the value-operand
// slice the specifiers consume.
struct SformatInfo {
    // Destination string lvalue.
    Index dst;
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
    // As DisplayInfo::signs.
    ExtraSlice signs;
    // The format as a runtime STRING VALUE, for the $sformat form LRM §21.3.3
    // exempts from the literal-format rule; kNoFormatValue on the ordinary path,
    // where `fmt` / `fmt_len` carry the text instead. Never both.
    Index fmt_value;
    // Only read with `fmt_value`: with a constant format the frontend has already
    // scaled every %t operand (lower/Assign.cpp scale_to_ticks).
    TickScale tick_scale;
};

// Extra payload for `sformatf`: $sformatf (LRM §21.3.3) formats the value args
// per the packed format-string bytes at `fmt` (`fmt_len` bytes) and yields a
// new string value (the instruction's string temp).
struct SformatfInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
    // As DisplayInfo::signs.
    ExtraSlice signs;
    // As SformatInfo::fmt_value.
    Index fmt_value;
    // As SformatInfo::tick_scale.
    TickScale tick_scale;
};

// Extra payload for `fscanf` / `sscanf` (LRM §21.3.4): parse `source` per the
// packed format-string bytes at `fmt` (`fmt_len` bytes) into the output lvalues
// `args`, returning the conversion count. The tag says what `source` is: an FD
// value (Fscanf) or a packed-ASCII string value the backend spills (Sscanf).
struct ScanfInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // Source operand: an FD value (Fscanf) or a packed string value (Sscanf).
    Index source;
    // Output lvalue operands, one per conversion, in order.
    ExtraSlice args;
};

// Extra payload for `fread_mem`: $fread(mem, fd[, start[, count]]) reads raw bytes
// into the unpacked-array memory `mem`. The fd value is the instruction's `op`;
// `start` / `count` are optional address / element-count operands (valid per has_*).
struct FreadMemInfo {
    // Destination memory signal.
    Signal mem;
    // Optional first-address and max-element-count operands.
    Index start;
    Index count;
    // Whether the start / count operands are present.
    uint32_t has_start;
    uint32_t has_count;
};

// Extra payload for `test_plusargs`: $test$plusargs(str) (LRM §21.6). `str` is
// the packed query-string bytes (a compile-time-literal argument; a runtime
// string operand is a documented gap, see lower/SystemTasks.cpp).
struct TestPlusArgsInfo {
    // First extra word of the packed query-string bytes.
    ExtraIndex str;
    // Query-string length in bytes.
    uint32_t str_len;
};

// Extra payload for `value_plusargs`: $value$plusargs(user_string, var) (LRM
// §21.6). `prefix` is the plusarg_string portion of user_string (everything
// before the trailing `%conv`); `conv` is the lowercase format character
// (d/o/b/h/s — e/f/g are a documented gap, real conversion needs the same
// ASCII->real scan $fscanf/$sscanf's own %f/%e/%g gap needs). The
// instruction's `op` (in the enclosing op_extra payload) is the output lvalue,
// and may be a `string` object -- for `s` only, the clause's "no conversion"
// form, where the plusarg remainder is the value verbatim.
struct ValuePlusArgsInfo {
    // First extra word of the packed plusarg_string prefix bytes.
    ExtraIndex prefix;
    // Prefix length in bytes.
    uint32_t prefix_len;
    // Lowercase format conversion character ('d'/'o'/'b'/'h'/'s').
    uint32_t conv;
};

// Extra payload for `timeformat`: a $timeformat system task. `units`,
// `precision`, and `min_width` are value operands (general runtime rvalues); the
// suffix string is appended after the formatted number and lives packed
// little-endian (4 chars per u32 word) at `suffix` for `suffix_len` bytes.
struct TimeFormatInfo {
    // Time-unit exponent operand (10^units seconds).
    Index units;
    // Fractional-digit count operand.
    Index precision;
    // Minimum field width operand (right-justifies the number + suffix).
    Index min_width;
    // First extra word of the packed suffix bytes.
    ExtraIndex suffix;
    // Suffix length in bytes.
    uint32_t suffix_len;
};

// Extra payload for `str_putc`: s[i] = c / s.putc(i, c). The string lvalue is
// the op; these are the index and character value operands.
struct StrPutcInfo {
    // Character position operand.
    Index index;
    // Character value operand (low 8 bits used).
    Index ch;
};

// Extra payload for `str_substr`: s.substr(i, j). The source string is the op;
// these are the inclusive start / end position operands.
struct StrSubstrInfo {
    // First selected position operand.
    Index i;
    // Last selected position operand (inclusive).
    Index j;
};

// One row of an enum's name table: a member value (low 64 bits) and its source
// name, packed little-endian at `name_at` for `name_len` bytes.
struct EnumNameEntry {
    // First extra word of the member value's must-plane bytes ((width+7)/8 bytes,
    // little-endian). Stored as bytes rather than a word so a base wider than 64
    // bits round-trips.
    ExtraIndex val_at;
    // Member value length in bytes ((width+7)/8), identical across all entries.
    uint32_t val_len;
    // First extra word of the packed name bytes.
    ExtraIndex name_at;
    // Name length in bytes.
    uint32_t name_len;
};

// Extra payload for `enum_name`: the enum's member table, a run of back-to-back
// EnumNameEntry records the runtime scans for a value match.
struct EnumNameInfo {
    // The member rows, as back-to-back EnumNameEntry records.
    ExtraSlice entries;
    // 1 when a value matching no member renders as a decimal leaf rather than ""
    // — LRM 21.2.1.6's rule for `%p` of an enum, against LRM 6.19.2's "" for
    // `.name()`.
    uint32_t fmt_fallback;
    // Nonzero when the enum's base type is signed, so the decimal fallback prints
    // a negative value with its sign.
    uint32_t is_signed;
};

// One member of a tagged union's name table, in tag order (its slang
// `fieldIndex`), so a report can name the tag a union actually holds.
struct TaggedNameEntry {
    // First extra word of the packed name bytes.
    ExtraIndex name_at;
    // Name length in bytes.
    uint32_t name_len;
};

// Extra payload for `tagged_check`: everything the LRM 11.9 diagnostic needs
// about one guarded tagged-union member access. All of it is compile-time; only
// the tag value itself arrives as the instruction's operand.
struct TaggedCheckInfo {
    // fieldIndex of the accessed member — the one tag value this access is
    // consistent with (LRM 11.9).
    uint32_t expected;
    // 1 for an assignment, 0 for a read. LRM 11.9 makes both an error and the
    // report names which one happened.
    uint32_t is_write;
    // Packed name bytes of the accessed member.
    ExtraIndex name_at;
    uint32_t   name_len;
    // Every member name in tag order, as back-to-back TaggedNameEntry records.
    ExtraSlice names;
};

// One member of a tagged union's `%p` format table, in tag order.
struct TaggedFmtEntry {
    // First extra word of the packed member name bytes.
    ExtraIndex name_at;
    // Name length in bytes.
    uint32_t name_len;
    // The member's own payload width in bits, which is what LRM 21.2.1.6 prints
    // ("the currently valid element") rather than the union's full payload region.
    // Zero for a `void` member, whose tag prints with no `:value` at all.
    uint32_t width;
    // Nonzero when the member's type is signed, so its decimal payload prints with
    // a sign (21.2.1.6 prints a singular element as it would print unformatted).
    uint32_t is_signed;
};

// Extra payload for `tagged_fmt_p`: what the runtime needs to render a tagged
// union as LRM 21.2.1.6's `tag:value`. The active member is only known at run
// time, so unlike a struct this cannot be expanded to literal text at lower time.
struct TaggedFmtInfo {
    // Total width of the flat {tag, gap, payload} value.
    uint32_t width;
    // High discriminant bits; 0 for a single-member tagged union.
    uint32_t tag_bits;
    // One entry per member, in tag order, as back-to-back TaggedFmtEntry records.
    ExtraSlice members;
};

// Which 4-state bit values a CountBits op tallies (LRM 20.9 $countbits control
// bits). Bits are OR-combined into CountBitsInfo::mask; $countones is CountOne
// alone, $isunknown is CountX | CountZ.
enum CountBitsClass : uint32_t {
    CountZero = 1u << 0,
    CountOne  = 1u << 1,
    CountX    = 1u << 2,
    CountZ    = 1u << 3,
};

// Extra payload for `count_bits`: the operand is the op; this selects which
// per-bit 4-state values are counted (a CountBitsClass OR-mask).
struct CountBitsInfo {
    // OR of CountBitsClass bits.
    uint32_t mask;
};

// Extra payload for `wait`: one (signal, edge) event of an intra-body event
// control. A `@(a or posedge b)` wait carries several of these in a row.
struct WaitEdge {
    // Signal whose change can wake the process.
    Signal signal;
    // Edge to wait for (ir::Edge value).
    uint32_t edge;
    // For an EVENT wait only: the SSA value of the event handle to park on, which
    // may name any object (an array element, a class property, a formal), not just
    // the one `signal` declares. kNoWaitHandle when the wait names a signal
    // directly -- including a plain `@(e)` on a declared event, which resolves
    // through `signal` so the common case stays a folded constant address.
    //
    // Index{0} is a real instruction, so the sentinel is the max value, like
    // kNoDisableTarget -- and it is the DEFAULT, so every existing construction
    // site (which uses designated initializers naming only signal and edge) keeps
    // meaning "no handle" instead of silently naming instruction 0.
    Index handle = static_cast<Index>(~uint32_t{ 0 });
    // For a wait on a member reached through a VIRTUAL INTERFACE (LRM 25.9): the
    // SSA value of the handle. `signal` is then a representative of the member and
    // this names the instance whose waiter list the park goes onto. Same sentinel
    // and same defaulting rule as `handle`, with which it is mutually exclusive.
    Index inst = static_cast<Index>(~uint32_t{ 0 });
};

// `WaitEdge::handle` is absent: this wait names its signal directly.
inline constexpr Index kNoWaitHandle = static_cast<Index>(~uint32_t{ 0 });

// Extra payload for `wait`: the list of events the process suspends on. The
// process wakes when ANY listed (signal, edge) fires. `events` is a run of
// packed WaitEdge records (each two words wide) in the extra buffer.
struct WaitInfo {
    // The (signal, edge) events, as back-to-back WaitEdge records.
    ExtraSlice events;
};

// Extra payload for `event_trigger_nb_delayed` (`->> #d e`): the target event
// signal. The delay (in wheel ticks) is the instruction's `op` operand.
// Extra payload for `event_trigger_nb_event` (`->> @(x) e`): the edge to watch and
// the `repeat (n)` countdown. The event handle is the instruction's `op` operand.
struct EventDetachInfo {
    // Signal whose edge matures the subscription.
    Signal signal;
    // Edge to watch (ir::Edge value).
    uint32_t edge;
    // Number of occurrences before the trigger fires (1 for the bare form).
    uint32_t count;
};

static_assert(ExtraPayload<ExtraSlice>);
static_assert(ExtraPayload<CallInfo>);
static_assert(ExtraPayload<TaskCallInfo>);
static_assert(ExtraPayload<VirtualCallInfo>);
static_assert(ExtraPayload<ForkInfo>);
static_assert(ExtraPayload<Select>);
static_assert(ExtraPayload<PartSelect>);
static_assert(ExtraPayload<DynPartSelect>);
static_assert(ExtraPayload<ElemSelect>);
static_assert(ExtraPayload<ArrSlice>);
static_assert(ExtraPayload<MemberSelect>);
static_assert(ExtraPayload<AggCopyInfo>);
static_assert(ExtraPayload<AggEqInfo>);
static_assert(ExtraPayload<InsideSetInfo>);
static_assert(ExtraPayload<DynArrayArg>);
static_assert(ExtraPayload<DynArrayNewInitInfo>);
static_assert(ExtraPayload<QueueArg>);
static_assert(ExtraPayload<QueueInsertInfo>);
static_assert(ExtraPayload<QueueSliceInfo>);
static_assert(ExtraPayload<AssocArg>);
static_assert(ExtraPayload<ReadMemInfo>);
static_assert(ExtraPayload<Regions>);
static_assert(ExtraPayload<LoopInfo>);
static_assert(ExtraPayload<SwitchArm>);
static_assert(ExtraPayload<SwitchInfo>);
static_assert(ExtraPayload<DisplayInfo>);
static_assert(ExtraPayload<FinishInfo>);
static_assert(ExtraPayload<CaseViolInfo>);
static_assert(ExtraPayload<StrobePendInfo>);
static_assert(ExtraPayload<ExpectInfo>);
static_assert(ExtraPayload<RandcInfo>);
static_assert(ExtraPayload<SeverityInfo>);
static_assert(ExtraPayload<FseekInfo>);
static_assert(ExtraPayload<RealMathBinaryInfo>);
static_assert(ExtraPayload<DistUnaryInfo>);
static_assert(ExtraPayload<DistBinaryInfo>);
static_assert(ExtraPayload<SformatInfo>);
static_assert(ExtraPayload<SformatfInfo>);
static_assert(ExtraPayload<ScanfInfo>);
static_assert(ExtraPayload<FreadMemInfo>);
static_assert(ExtraPayload<TimeFormatInfo>);
static_assert(ExtraPayload<WaitEdge>);
static_assert(ExtraPayload<WaitInfo>);
static_assert(ExtraPayload<StrPutcInfo>);
static_assert(ExtraPayload<StrSubstrInfo>);
static_assert(ExtraPayload<EnumNameEntry>);
static_assert(ExtraPayload<EnumNameInfo>);
static_assert(ExtraPayload<TaggedNameEntry>);
static_assert(ExtraPayload<TaggedCheckInfo>);
static_assert(ExtraPayload<TaggedFmtEntry>);
static_assert(ExtraPayload<TaggedFmtInfo>);
static_assert(ExtraPayload<CountBitsInfo>);
static_assert(ExtraPayload<TestPlusArgsInfo>);
static_assert(ExtraPayload<ValuePlusArgsInfo>);

struct Inst {
    enum class Tag : uint8_t {
        // -- Leaves --
        // Interned constant value (payload: ref).
        Constant,
        // Function / task parameter by position (payload: arg). Read-only; a
        // formal is copied into a Local at entry so the body can mutate it.
        Arg,
        // Reference to a module signal (payload: signal).
        Signal,
        // Reference to a function-frame local slot by index (payload: local). An
        // lvalue leaf like Signal, but backed by per-call stack storage (automatic
        // lifetime) rather than the persistent state buffer. Holds formals, the
        // return variable, and body-local declarations of a function.
        Local,

        // -- Lvalue derivation --
        // Single-bit select base[index] (payload: binary).
        BitSelect,
        // Constant part-select base[msb:lsb] (payload: op_extra -> PartSelect).
        PartSelect,
        // Runtime-offset, constant-width part-select base[off +: w] / base[off -: w]
        // (payload: op_extra -> DynPartSelect). The offset is a runtime value; the
        // backend rides the same shift/mask load/store path as a dynamic BitSelect.
        DynPartSelect,
        // Unpacked-array element select base[index] (payload: op_extra ->
        // ElemSelect). Distinct from BitSelect: the stride is the element's
        // storage size, not one bit, and the index is normalized against the
        // array's declared bounds.
        ElemSelect,
        // Constant-bounds slice of a fixed unpacked array base[hi:lo] (payload:
        // op_extra -> ArrSlice). Yields a position-0-anchored sub-array lvalue;
        // an lvalue-only op, consumed by the per-element aggregate copy / eq /
        // store paths, never lowered as a value.
        ArrSlice,
        // Unpacked-struct member select base.field (payload: op_extra ->
        // MemberSelect). The base is an unpacked struct aggregate; the member is
        // addressed by declaration-order field index into the backing StructType,
        // not a bit offset (a packed struct member is a PartSelect instead).
        MemberSelect,
        // Address of one elaborated interface instance, as a virtual interface
        // handle (payload: extra -> IfaceInst). A 2-state 64-bit value; the
        // constant an `vif = bus;` assignment stores. LRM 25.9.
        IfaceRef,
        // Reach a member of the interface instance a virtual interface handle
        // names (payload: op_extra -> op is the handle value, extra ->
        // IfaceMember). Yields the member's lvalue: the representative signal's
        // own lvalue with its state base taken from the handle instead of from
        // the owning instance's symbol. A null handle is a located fatal.
        IfaceSignal,
        // Class object `new` (LRM 8.7): allocate the heap object body, power-on
        // init its properties, and yield the object handle as a 2-state 64-bit
        // value (payload: ref -> the Kind::Class type). The constructor call is
        // emitted separately by the frontend (a normal call with the handle spliced
        // as `this`). Reference semantics: the handle is a bare pointer.
        ClassNew,
        // Dereference a class object handle to an aggregate lvalue rooted at the
        // object body (payload: op_type -> op is the handle value, type is the
        // Kind::Class). Produces an is_agg lvalue whose base is the body pointer, so
        // a following MemberSelect addresses a property. The one op that turns a
        // reference handle into a body lvalue (contrast a value struct, addressed
        // inline).
        HandleDeref,
        // Dynamic type test behind `$cast` (LRM 8.16): is the object a handle
        // refers to of the payload class, or of a class derived from it? (payload:
        // op_type -> op is the handle value, type is the destination Kind::Class).
        // Yields a 1-bit 2-state value. A null handle answers 1 -- assigning null
        // to a cast-compatible destination is legal, and the frontend only emits
        // this op once the two types are known to be cast compatible.
        ClassIsA,

        // -- State access --
        // Read the current value of an lvalue (payload: unary).
        Load,
        // Blocking assignment, lhs = rhs (payload: binary).
        Store,
        // Nonblocking assignment, lhs <= rhs (payload: binary).
        StoreNb,
        // Nonblocking intra-assignment delay, lhs <= #d rhs (LRM 9.4.5): sample
        // rhs now (this instant), schedule the store of the sampled value for
        // `now + d` without blocking the process (payload: op_extra ->
        // StoreNbDelayInfo; the op is the whole-signal lvalue).
        StoreNbDelay,
        // Nonblocking intra-assignment event, lhs <= @e rhs / lhs <= repeat(N) @e
        // rhs (LRM 9.4.5): sample rhs (and N) now, store the sampled value after N
        // occurrences of the trigger edge, without blocking the process (payload:
        // op_extra -> StoreNbEventInfo; the op is the whole-signal lvalue).
        StoreNbEvent,
        // Inertial continuous-assignment delay, `assign #d lhs = rhs` (LRM 10.3.3):
        // arm/reschedule a detached epoch-stamped store of the freshly-evaluated rhs
        // for `now + d`, cancelling any still-pending store that a change supersedes
        // (pulse rejection). Reuses StoreNbDelayInfo {value, delay}; the op is the
        // whole-signal lvalue.
        StoreInertialDelay,
        // Whole-object blocking copy of one aggregate into another (payload:
        // op_extra -> AggCopyInfo). Lowered to llvm.memcpy of the storage; the
        // frontend emits it only for a string-free aggregate.
        AggCopy,
        // 1-bit 4-state equality of two whole unpacked aggregates (payload:
        // op_extra -> AggEqInfo; op is the left lvalue). The backend expands
        // the member walk, AND-reducing per-leaf `==` results so LRM 11.4.5
        // X-propagation is carried exactly (docs/LRM_NOTES.md, note 13); `!=` is a Lnot
        // of this.
        AggEq,

        // -- Dynamic-array operations on a handle lvalue (LRM 7.5) --
        // Each lowers to its rt_dynarr_* runtime call; element access rides
        // the ordinary ElemSelect path over the handle's heap buffer, not
        // these tags.
        // arr = new[size]: (re)allocate to `size` elements, each the element's
        // power-on value (LRM 7.5.1; payload: op_extra -> DynArrayArg, whose
        // arg is the element count; op is the handle lvalue).
        DynArrayNew,
        // arr = new[size](src): like DynArrayNew, then copy min(size, src.len)
        // elements from the source handle (payload: op_extra ->
        // DynArrayNewInitInfo; op is the handle lvalue).
        DynArrayNewInit,
        // arr.size() -> 32-bit int (payload: op_type — op is the handle
        // lvalue, type the interned DynArray type).
        DynArraySize,
        // arr.delete(): free the storage, size -> 0 (payload: op_type).
        DynArrayDelete,
        // arr = other: deep copy of another dynamic array's storage (value
        // semantics; payload: op_extra -> DynArrayArg, whose arg is the source
        // handle lvalue; op is the destination handle lvalue).
        DynArrayCopy,
        // arr = q: deep copy of a QUEUE into a dynamic array (LRM 7.5.1's
        // resizeable assignment; payload: op_extra -> DynArrayArg, whose arg is
        // the SOURCE queue handle lvalue, op is the destination dynarray handle
        // lvalue). Distinct from DynArrayCopy because the source is an
        // rt_queue (ring-indexed), not an rt_dynarr (linear) -- the mirror of
        // QueueCopyDyn below.
        DynArrayCopyQueue,

        // -- Queue operations on a handle lvalue (LRM 7.10.2) --
        // Each lowers to its rt_queue_* runtime call; indexed element access
        // q[i] rides the ElemSelect queue path (ring math over the handle's
        // buffer), not these tags.
        // q.size() -> 32-bit int (payload: op_type — op is the handle lvalue,
        // type the interned Queue type).
        QueueSize,
        // q.push_back(e) / q.push_front(e): append the element value (payload:
        // op_extra -> QueueArg; op is the handle lvalue). On a bounded queue an
        // overflowing element is dropped from the opposite end.
        QueuePushBack,
        QueuePushFront,
        // q.pop_back() / q.pop_front(): remove and return the tail / head
        // element — the element default on an empty queue (payload: op_type,
        // like QueueSize).
        QueuePopBack,
        QueuePopFront,
        // q.insert(idx, e) (payload: op_extra -> QueueInsertInfo; op is the
        // handle lvalue).
        QueueInsert,
        // q.delete(idx): remove the element at idx (payload: op_extra ->
        // QueueArg, whose arg is the position; op is the handle lvalue).
        QueueDeleteIdx,
        // q.delete(): remove all elements, size -> 0 (payload: op_type).
        QueueDeleteAll,
        // q = other: deep copy of another queue's storage (value semantics;
        // payload: op_extra -> QueueArg, whose arg is the source handle
        // lvalue; op is the destination handle lvalue).
        QueueCopy,
        // Nonblocking `dst <= src` deep copy of a whole CONTAINER handle -- a queue,
        // a dynamic array or an associative array (payload: op_extra -> QueueArg,
        // arg is the source handle lvalue, op is the dst handle lvalue, and
        // queue_type is the container's interned type, which selects the kind).
        //
        // A container's value is a heap handle, not a plane slot, so it has no
        // per-signal shadow to stage into the way a scalar nonblocking assignment
        // does. Instead the Active region deep-copies the source into a staging
        // handle in a per-signal record buffer, and the NBA region MOVES that
        // buffer into the live handle -- an ownership transfer, so the commit costs
        // no second copy and the target's prior elements are freed exactly once
        // (LRM 7.5 / 7.9 / 7.10, 10.4.2).
        HandleCopyNb,
        // q = d: deep copy of a dynamic array into a queue (LRM 7.10; payload:
        // op_extra -> QueueArg, whose arg is the SOURCE dynarray handle lvalue,
        // op is the destination queue handle lvalue). Distinct from QueueCopy
        // because the source is an rt_dynarr, not an rt_queue.
        QueueCopyDyn,
        // Append src[lo:hi] onto the dst handle (payload: op_extra ->
        // QueueSliceInfo; op is the dst handle lvalue), materializing a queue
        // slice or a queue-typed concatenation operand (LRM 7.10.4).
        QueueAppendRange,

        // -- Array ordering methods on a handle lvalue (LRM 7.12.2) --
        // Each lowers to its rt_{dynarr,queue}_* runtime call; all carry only
        // the handle lvalue + interned type (payload: op_type, like
        // DynArrayDelete). Reverse / Shuffle are element-agnostic; Sort / Rsort
        // are value-keyed (2-state integral width<=64 or real, guarded in the
        // frontend). Shuffle draws from the calling thread's RNG (forcing a
        // process that does to a coroutine); the backend supplies the state
        // pointer at lowering.
        DynArrayReverse,
        DynArraySort,
        DynArrayRsort,
        DynArrayShuffle,
        QueueReverse,
        QueueSort,
        QueueRsort,
        QueueShuffle,

        // -- Associative-array operations on a handle lvalue (LRM 7.8/7.9) --
        // Each lowers to its rt_assoc_* runtime call; keys cross the ABI as
        // normalized order-preserving byte spans, built by a backend converter
        // keyed on the interned Assoc type (element = value, fields_id = key,
        // right = wildcard flag). See docs/design/dynamic-arrays.md §4a/§6.
        // a[k] element select producing an lvalue (payload: op_extra ->
        // AssocArg, whose arg is the key value/lvalue; op is the handle lvalue).
        // A Load of it calls rt_assoc_get_ptr (absent -> element default, no
        // create); a Store calls rt_assoc_put_ptr (create + default-fill). The
        // get/put dispatch is deferred to lower_load/lower_store.
        AssocSelect,
        // a.num() / a.size() -> 32-bit int entry count (payload: op_type).
        AssocNum,
        // a.exists(k) -> 32-bit int 0/1 (payload: op_extra -> AssocArg, arg is
        // the key value; op is the handle lvalue).
        AssocExists,
        // a.delete(k): remove one key if present (payload: op_extra ->
        // AssocArg, arg is the key value; op is the handle lvalue).
        AssocDeleteKey,
        // a.delete(): remove all entries, num -> 0 (payload: op_type).
        AssocDeleteAll,
        // The `'{default: v}` slot (LRM 7.9.11) as an lvalue, so a Store of the
        // pattern's default value reuses the ordinary element-store path
        // (payload: op_type). A read of an absent key resolves to this slot
        // instead of the Table 7-1 element default. Invisible to
        // num/exists/first/next, and it outlives a.delete().
        AssocDefault,
        // Whole-array replacement: drop every entry AND the pattern default
        // (payload: op_type). What a `'{...}` literal assigns onto, as opposed to
        // a.delete(), which keeps the default.
        AssocReset,
        // a = other: deep copy of another assoc's entries (value semantics;
        // payload: op_extra -> AssocArg, arg is the source handle lvalue).
        AssocCopy,
        // a.first(k)/last(k)/next(k)/prev(k): ordered traversal (LRM 7.9.3).
        // Return 32-bit int 1 if a key was written, 0 at the end. `arg` is the
        // key-variable LVALUE: first/last write the min/max key into it; next/
        // prev read its current value and write the successor/predecessor. The
        // backend normalizes the current key and denormalizes the result key.
        AssocFirst,
        AssocLast,
        AssocNext,
        AssocPrev,

        // -- Arithmetic (payload: binary, except Pow) --
        // Add / Sub / Mul are the same bits whatever the operands' signedness.
        Add,
        Sub,
        Mul,
        // Division and remainder of unsigned / signed operands (LRM 11.4.3). The
        // frontend picks the opcode from the operation's signedness (LRM 11.8.1).
        UDiv,
        SDiv,
        UMod,
        SMod,
        // Power (payload: op_extra -> PowInfo; op is the base). The base's and the
        // self-determined exponent's signedness are instruction data.
        Pow,

        // -- Bitwise (payload: binary, except Bnot) --
        Band,
        Bor,
        Bxor,
        // Bitwise xnor ^~ / ~^ (payload: binary).
        Bxnor,
        // Bitwise invert ~ (payload: unary).
        Bnot,

        // Strong wire/tri multi-driver resolution of two driver values (LRM
        // 6.6.1, no drive strengths), the lattice join z ⊑ {0,1} ⊑ x: a
        // high-Z operand contributes nothing (identity), two operands that
        // agree keep the value, a 0-vs-1 (or any x) conflict is x, all-z is z.
        // Associative + commutative, so K drivers fold left through this.
        // Result is always 4-state at the operand width (payload: binary).
        // Emitted only by the multi-driver-net resolver (lower/Nets.cpp
        // emit_wire_resolvers); single-driver nets never use it.
        WireRes,
        // Wired-AND / wired-OR multi-driver resolution (LRM 6.6.2 / 6.6.3) for
        // wand/triand and wor/trior nets. Same slot machinery and z-identity as
        // WireRes, only a different per-bit reducer: wand forces 0 on any 0
        // driver, wor forces 1 on any 1 driver, else x on any x, else z. Both
        // associative + commutative, so K drivers fold left (payload: binary).
        WandRes,
        WorRes,
        // Drive-strength wire/tri multi-driver resolution (LRM 6.6.1, 28.11):
        // an N-ary fold (payload: extra -> StrengthResInfo, a slice of driver
        // slot values plus a parallel slice of packed per-driver (strength0,
        // strength1) constants). Per bit, the strongest side wins — stronger 0
        // beats weaker 1 and vice versa; equal top strength on both sides is X;
        // no driver (all Z) is Z. With every driver at the default Strong this
        // degenerates to WireRes. Emitted only by emit_wire_resolvers when a net
        // has any non-default drive strength; N operands, not pairwise, because
        // the winner needs every driver's strength at once.
        StrengthRes,
        // Resistive pull applied after a tri0/tri1 net's drivers are resolved
        // (LRM 6.6.4): a bit still Z (all drivers off) is pulled to the immediate
        // (0 for tri0, 1 for tri1); a definitely-driven or X bit is unchanged.
        // A post-fold unary transform, not a per-pair rule — the pull only fills
        // Z once every driver has folded. Result never carries Z (payload:
        // op_imm — op is the resolved value, imm the pull value).
        NetPull,

        // -- Reductions, e.g. &x (payload: unary) --
        RedAnd,
        RedOr,
        RedXor,

        // Index of the lowest (Ctz) / highest (Clz) definite-1 bit of the
        // operand's must plane, or the operand width when it has no definite-1
        // bit (payload: unary). x/z bits are not definite 1, so they never
        // count. Result is a definite (2-state) 32-bit count. Used to lower the
        // one-hot `case (1'b1)` reverse-case idiom to a jump table: the winning
        // arm is the lowest (ascending source order) or highest (descending) set
        // bit index. See lower/Statements.cpp build_onehot_plan.
        Ctz,
        Clz,

        // -- Shifts (payload: binary) --
        // Shift left logical.
        Sll,
        // Shift right logical.
        Srl,
        // Shift right arithmetic: replicates the sign bit. The frontend emits it
        // only for `>>>` of a signed operand (LRM 11.4.10); an unsigned `>>>` is Srl.
        Sra,

        // -- Comparisons, -> 1-bit (payload: binary) --
        // Equality does not depend on signedness; the ordered compares come in an
        // unsigned and a signed form, picked by the frontend from the operands'
        // common type (LRM 11.4.4, 11.8.1).
        Eq,
        Ne,
        ULt,
        ULe,
        UGt,
        UGe,
        SLt,
        SLe,
        SGt,
        SGe,

        // -- Case-equality, -> definite 1-bit (payload: binary) --
        // Unlike Eq, these never yield X: they compare the 4-state planes
        // directly and return a known 0/1, matching SV case-statement semantics.
        // CaseEq is `===` (every bit, incl. x/z, must match exactly); CasezEq /
        // CasexEq treat z (resp. x and z) bits in either operand as don't-care.
        CaseEq,
        CasezEq,
        CasexEq,

        // Wildcard equality `==?` (payload: binary). x/z bits in the RIGHT operand
        // are wildcards; unlike the case-equality tags this is X-propagating — a
        // left-operand x/z on a non-wildcard bit yields X.
        WildEq,
        // The `inside` operator as an expression (payload: op_extra ->
        // InsideSetInfo; op is the selector), -> 1-bit 4-state. The backend
        // expands the member tests and OR-reduces them; the statement twin
        // (`case ... inside`) has its own switch machinery in the frontend.
        InsideSet,

        // -- Floating point (real / shortreal; LRM 6.12) --
        // Operands and result share one float type: the frontend inserts all
        // int/real and real/shortreal conversions (LRM 11.8.2), so mixed
        // operands never reach these tags. Reals carry no x/z.
        // Arithmetic (payload: binary).
        FAdd,
        FSub,
        FMul,
        FDiv,
        // Power ** on reals (payload: binary). C pow semantics for the corners
        // LRM 11.4.3 leaves unspecified.
        FPow,
        // Negation -x (payload: unary). A real fneg, not Sub-from-zero: 0.0 - 0.0
        // is +0.0 but -(0.0) must be -0.0.
        FNeg,
        // Comparisons, -> definite 1-bit, never X (payload: binary). FNe is
        // an unordered compare so NaN != NaN yields 1, matching C.
        FEq,
        FNe,
        FLt,
        FLe,
        FGt,
        FGe,
        // Conversions (payload: op_type = operand + target type).
        // Integral -> float, of an unsigned / signed operand; x/z bits read as 0
        // (LRM 6.12.1).
        UItoF,
        SItoF,
        // Float -> integral, rounding half away from zero (LRM 6.12.1) — the
        // assignment / cast conversion.
        FtoI,
        // Float -> integral, truncating toward zero — $rtoi (LRM 20.5).
        FtoITrunc,
        // Bit-pattern reinterpret between a float type and the same-width
        // integral type: $realtobits / $bitstoreal / $shortrealtobits /
        // $bitstoshortreal (LRM 20.5). Direction from operand vs target type.
        FBitcast,
        // real <-> shortreal value conversion (fpext / fptrunc, LRM 6.12.1).
        FResize,
        // One-operand real math system function — $ln / $sqrt / $sin / ...
        // (payload: op_imm — op is the real operand, imm the ir::RealMathOp).
        // LRM §20.8.2: each matches the C math library function.
        RealMathUnary,
        // Two-operand real math system function — $pow / $atan2 / $hypot
        // (payload: op_extra -> RealMathBinaryInfo; op is the first operand).
        RealMathBinary,

        // -- Logical, -> 1-bit --
        // Logical and (payload: binary).
        Land,
        // Logical or (payload: binary).
        Lor,
        // Logical not (payload: unary).
        Lnot,
        // Logical implication a -> b, LRM 11.4.7: logically equivalent to
        // (!a || b), but its own op rather than composed Lnot+Lor -- -> is one
        // of the LRM 11.3.5 short-circuit operators (skip b when a is
        // definitely false), which the frontend handles by conditionally not
        // lowering b at all (lower_short_circuit); this op is only reached on
        // the not-skipped path, where both operands are already-lowered
        // values (payload: binary).
        Limplies,
        // Logical equivalence a <-> b, LRM 11.4.7: unlike -> and &&/||, LRM
        // 11.3.5 does NOT list <-> as short-circuiting -- "each of the two
        // operands... shall be evaluated exactly once" -- so the frontend
        // always lowers both operands (payload: binary).
        Lequiv,

        // -- Selection / structural --
        // Ternary cond ? a : b (payload: op_extra -> Select).
        Select,
        // Value-producing short-circuit conditional (payload: op_extra ->
        // SelectLazy). The op is a definite 2-state predicate; the then-branch
        // (predicate 1) and else-branch each yield a value, merged by the
        // backend. Used for &&/|| so the right operand is not evaluated when the
        // left already decides the result (LRM 11.3.5, 11.4.7). Lowers to
        // then/else/cont basic blocks with a PHI, like a lazy Select.
        SelectLazy,
        // Concatenation {a, b, ...} (payload: extra -> index slice).
        Concat,
        // Replication {count{value}} (payload: repl).
        Repl,
        // Streaming-concat block reversal {<<S{x}} (LRM 11.4.14.2): reverse
        // the order of the operand's S-bit blocks, sliced from the LSB (a
        // final partial block keeps its own width; block 0 lands at the MSB).
        // A pure per-plane bit permutation of the operand's width (payload:
        // op_imm — op is the built stream value, imm the slice size S).
        StreamReverse,

        // -- Width and domain --
        // Conversions of an integral value to a target type (payload: op_type: the
        // operand and the type of the result, whose width and domain are the
        // target's). The extension is in the opcode: the language, not the value's
        // bits, decides whether a widening fills with zeros or with a copy of the
        // top bit (LRM 11.8.2, 11.8.3, 6.24.1), and the frontend chooses.
        // Widening, in the operand's domain: fill the new high bits with zeros / with
        // the top bit.
        ZExt,
        SExt,
        // Narrowing, in the operand's domain: keep the low bits.
        Trunc,
        // The domain alone, at the operand's width: a 2-state value gains an x/z
        // plane that is zero, and the x and z bits of a 4-state value become 0
        // (LRM 6.3.1).
        ToFour,
        ToTwo,

        // -- Control flow --
        // If / else; condition + then/else regions (payload: op_extra -> Regions).
        Cond,
        // Multiway branch for a constant, deduped, wildcard-free `case` (payload:
        // op_extra -> SwitchInfo; the op is the selector value). Lowers to an
        // llvm switch over a packed key; unmatched keys (incl. any x/z selector)
        // run the default region. See SwitchInfo.
        Switch,
        // Return a value from a non-void function (payload: unary, the return
        // value). A real terminator: instructions after it in the same region
        // are unreachable. Early `return` works without any flag.
        Ret,
        // Return from a void function, task, or fork-child body (payload: none).
        // A terminator like Ret; tasks branch to their copy-out epilogue instead
        // of a real `ret`.
        RetVoid,
        // Call a function (payload: extra -> CallInfo). The result is this
        // instruction's value (absent for a void function called as a statement).
        Call,
        // Dynamically-dispatched virtual method call (payload: extra ->
        // VirtualCallInfo, LRM 8.20). Loads the callee from the receiver object's
        // vtable (indexed by slot) and calls it indirectly; otherwise identical to
        // Call (same arg ABI, same result). Only for virtual FUNCTIONS in V1.
        CallVirtual,
        // Enable a task (payload: extra -> TaskCallInfo). A statement, never a
        // value: it creates the task coroutine, drives it, and blocks the calling
        // coroutine until the task completes (LRM 13.3). Only legal inside a
        // coroutine (a process or another task), never a plain function.
        TaskCall,
        // Dynamically-dispatched virtual TASK call (payload: extra ->
        // VirtualCallInfo, LRM 8.20). Loads the task ramp function from the receiver
        // object's vtable and drives it over the task coroutine ABI; otherwise
        // identical to TaskCall.
        TaskCallVirtual,
        // Loop with a per-iteration condition; `for` / `while` / `forever` all
        // lower to this (payload: extra -> LoopInfo). The `for` init runs in the
        // enclosing region before this inst; `while` / `forever` have empty step.
        // Lowers to header / body / step / exit blocks with a back-edge.
        Loop,
        // Loop control: `break` exits the innermost enclosing Loop (branches to
        // its exit block); `continue` skips to the innermost Loop's step block
        // (payload: none). Both are terminators — instructions after them in the
        // same region are unreachable. slang rejects either outside a loop.
        Break,
        Continue,
        // A named sequential block targeted by a `disable` in the same lowering
        // unit (payload: extra -> BlockInfo). Lowers to its body followed by an
        // end block, which is where a Disable branches to.
        NamedBlock,
        // `disable <named block>` (LRM 9.6.2), lexical form: leave the enclosing
        // NamedBlock carrying the same id, resuming at the statement after that
        // block (payload: label). A terminator, exactly like Break, and it binds
        // lexically -- the id names an enclosing block of this unit.
        Disable,
        // `disable <named block>` / `disable <task>` (LRM 9.6.2), remote form: mark
        // the activation(s) the target names as disabled and force-wake them
        // (payload: label, here the target id rather than an enclosing label). A
        // block target has one occupant slot; a task target has an activation list,
        // because 9.6.2 ends EVERY activation. NOT a terminator -- the disabling
        // process carries on -- and a no-op when nothing is executing the target,
        // which is 9.6.2's "if the block is not currently executing, the disable has
        // no effect".
        DisableRemote,
        // Leave THIS task activation through its epilogue, skipping copy-out (LRM
        // 9.6.2 leaves output/inout results unspecified on a disable). A terminator,
        // emitted after a DisableRemote whose target is the enclosing task, which is
        // 9.6.2 example 4: the activation running the `disable` is one of the ones
        // that has to end, and it is the only one no force-wake can reach.
        DisableAbort,
        // Drop a procedure's pending deferred-assertion reports (LRM 16.4.4): bump
        // its generation counter, exactly as an activation start does (payload:
        // label -- kNoDisableTarget for the procedure being lowered, else the
        // `disable` target id whose owning procedure is flushed). Emitted
        // immediately before a `disable` of that procedure's outermost scope.
        DeferFlush,
        // Time delay `#n`: suspend the process for `n` ticks (payload: unary, the
        // tick count). Lowers to a timer scheduled on the wheel plus a coroutine
        // suspend; the wheel resumes the process when the delay elapses.
        Delay,
        // Event wait `@(edge signal)` / `@(a or posedge b)`: suspend until ANY
        // listed (signal, edge) fires (payload: wait -> WaitInfo). Lowers to
        // registering the process on each event's wait queue plus a coroutine
        // suspend; a matching blocking assignment to one of the signals wakes it.
        Wait,
        // Bind a NEW event object and yield its handle (payload: none). LRM Table
        // 6-7 makes "New event" the default value of an event variable, so this is
        // emitted wherever event storage comes into existence and no constant handle
        // can be baked in: an automatic-lifetime local. Static storage gets its
        // handle from the state initializer, and aggregate / container element leaves
        // are constructed by their own init walks.
        EventNew,
        // Blocking trigger of a named event `-> e` (payload: unary -> the event
        // HANDLE value, a 64-bit { slot, gen }). Resolves the handle to its event
        // object, records the trigger time (for `.triggered`) and wakes every
        // waiter on the object's list in the current instant (LRM 15.5.2). An event
        // has no value, so there is no edge computation. A null handle resolves to
        // the reserved null object, whose waiter list is always empty -- the no-op
        // LRM 15.5.5 requires of `-> null`.
        EventTrigger,
        // The `e.triggered` event property (payload: unary -> the event handle),
        // -> a definite 1-bit value: 1 iff the event was triggered in the current
        // time step, else 0 (LRM 15.5.3). Lowers to a compare of the object's
        // recorded trigger time against the current sim time; a null handle reads
        // the null object's never-triggered sentinel and answers 0.
        EventTriggered,
        // Nonblocking trigger of a named event `->> e` (payload: unary -> the event
        // handle). Unlike EventTrigger, the wake is deferred to the NBA region of
        // the current time step (LRM 15.5.1): it links the object onto the pending
        // chain; the NBA region then stamps the trigger time and wakes the waiters.
        // The triggering process does not block.
        EventTriggerNb,
        // Event-controlled nonblocking trigger `->> @(x) e` / `->> repeat(n) @(x) e`
        // (payload: op_extra -> op is the event HANDLE value, extra ->
        // EventDetachInfo names the (signal, edge) to watch and the repeat count).
        // Registers a detached one-shot subscription on that edge; when it matures
        // the referenced event is triggered in the NBA region (LRM 15.5.1). The
        // triggering process does not block, and no coroutine hosts the wait.
        EventTriggerNbEvent,
        // Delayed nonblocking trigger `->> #d e` (payload: binary -> l is the delay
        // in wheel ticks, r is the event HANDLE value). Schedules a detached wheel
        // timer carrying the handle that, on expiry, stamps the trigger time and
        // wakes the event's waiters (LRM 15.5.1). The triggering process does not
        // block. The event-controlled form (`->> @(x) e`) is not lowered here.
        EventTriggerNbDelayed,
        // Fork block `fork … join` (payload: extra -> ForkInfo). A statement that
        // spawns each child body as its own coroutine (a fork-child task, whose
        // only formals are the ref captures in ForkInfo::args), then — for `join` —
        // suspends the parent until all children complete. `ForkKind::Join` and
        // `JoinAny` park the parent and are legal only inside a coroutine (a process
        // or a task); `JoinNone` parks nobody and is legal in a plain FUNCTION body
        // too, which is what LRM 13.4.4 requires (docs/design/fork-join.md §V5).
        // LRM 9.3.2.
        Fork,

        // -- String ops (operands are string values unless noted) --
        // s.len() -> 32-bit signed (payload: unary).
        StrLen,
        // s[i] / s.getc(i) -> 8-bit 2-state; 0 when i is out of range (payload:
        // binary — l is the string, r the index).
        StrGetc,
        // s[i] = c / s.putc(i, c): mutate a string lvalue in place; a no-op when
        // i is out of range or c's low byte is 0 (payload: op_extra ->
        // StrPutcInfo; the op is the string lvalue, like Store's lhs).
        StrPutc,
        // a.compare(b) / a.icompare(b) -> 32-bit signed, strcmp order (payload:
        // binary).
        StrCmp,
        StrICmp,
        // s.toupper() / s.tolower() -> new string value (payload: unary).
        StrUpper,
        StrLower,
        // {count{s}} -> new string value: `s` repeated `count` times, where
        // `count` is a RUNTIME operand (payload: binary — l is the string, r the
        // count). Distinct from Repl, which replicates a PACKED value and needs
        // its count as an elaboration-time immediate because the result's width
        // is that count times the operand's. A string result has no width, so
        // nothing here has to be known before the run. See lower_expr's
        // Replication case for the LRM 11.4.12.1 deviation this exists for.
        StrRepl,
        // s.substr(i, j) -> new string value; "" when the range is invalid
        // (payload: op_extra -> StrSubstrInfo; the op is the source string).
        StrSubstr,
        // e.name() -> new string value: the source name of the enum member whose
        // value equals the operand, or "" if none matches (LRM 6.19.2). Payload:
        // op_extra -> EnumNameInfo (the member table); op is the enum value.
        EnumName,
        // %p of a queue / dynamic array -> new string value: the runtime renders
        // '{e0, e1, ...} over the runtime element count (LRM 21.2.1.6). Payload:
        // op_type — op is the handle lvalue, type the interned Queue / DynArray
        // type (element format descriptor).
        ContainerFmtP,
        // Guard one tagged-union member access against the union's current tag
        // (LRM 11.9: "An attempt to read or assign a value whose type is
        // inconsistent with the tag results in a run-time error"). Yields no
        // value; it either falls through or ends the run. Payload: op_extra ->
        // TaggedCheckInfo; op is the extracted tag value, whose 4-state compare
        // against TaggedCheckInfo::expected decides. An x/z tag (LRM 11.9: an
        // uninitialized tagged union is undefined "includ[ing] the tag bits")
        // matches nothing and so reports.
        TaggedCheck,
        // %p of a tagged union -> new string value: the runtime picks the member the
        // tag selects and renders LRM 21.2.1.6's `tag:value` (a void member prints
        // its tag alone). Payload: op_extra -> TaggedFmtInfo; op is the flat union
        // value.
        TaggedFmtP,
        // string'(packed) -> string: bytes MSB-first with 0x00 bytes dropped
        // (payload: unary — the packed value).
        StrFromPacked,
        // Packed cast of a string: characters pack MSB-first, right-justified to
        // the target type's width (payload: op_type — op is the string, type the
        // target packed type).
        StrToPacked,
        // Append a packed value's bytes to a string lvalue, MSB-first, with the
        // same 8-bit-chunk rule as StrFromPacked (a 0x00 chunk is dropped, LRM
        // 6.16): one operand of a streaming concatenation whose destination is a
        // string (LRM 11.4.14), which converts through exactly that rule -- so
        // this and a constant-folded stream agree (payload: binary — l is the
        // string lvalue, like Store's lhs, r is the packed value).
        StrAppendPacked,
        // s.atoi() / s.atohex() / s.atooct() / s.atobin() -> 32-bit signed
        // `integer` (LRM 6.16.9: the declared type is 4-state, though the
        // conversion itself never yields x — it scans leading digits, and '_'
        // separators, of the given base and stops at the first other character
        // or end of string; 0 if none matched. No leading whitespace or sign
        // handling, and overflow truncates silently (payload: unary — the
        // source string).
        StrAtoi,
        StrAtohex,
        StrAtooct,
        StrAtobin,
        // s.atoreal() -> real (LRM 6.16.10): parses a real-literal-like prefix,
        // stopping at the first non-conforming character; 0.0 if none matched.
        // Unlike the atoi family, leading whitespace and an optional '+' are
        // consumed first (payload: unary).
        StrAtoreal,
        // s.itoa(i) / s.hextoa(i) / s.octtoa(i) / s.bintoa(i): format `i` (a
        // 4-state `integer`; x/z bits read as 0, the same must-plane
        // simplification as StrFromPacked) into the string lvalue as sign +
        // magnitude in the given base, lowercase digits (LRM 6.16.11-6.16.14;
        // payload: binary — l is the string lvalue, like Store's lhs, r is the
        // integer value).
        StrItoa,
        StrHextoa,
        StrOctoa,
        StrBintoa,
        // s.realtoa(r): format `r` into the string lvalue at full round-trip
        // precision (LRM 6.16.15; payload: binary — l the string lvalue, r the
        // real value).
        StrRealtoa,

        // -- System tasks --
        // $display / $write: format string + variadic args to standard output
        // (payload: display -> DisplayInfo).
        Display,
        // $fdisplay / $fwrite: like Display, but writes the MCD / FD given by
        // the descriptor operand (payload: fdisplay — op_extra whose op is the
        // descriptor and extra a DisplayInfo).
        FDisplay,
        // $strobe / $strobeb / $strobeo / $strobeh (LRM 21.2.2): register the
        // compiler-synthesized zero-formal task `func` (payload: extra ->
        // StrobePendInfo) to run once in the Postponed region of the current
        // time step, just before simulation time advances. `func`'s body holds
        // the format args' evaluation (a Display), not this instruction — the
        // whole point of $strobe is that evaluation itself is deferred to the
        // end of the time step, not merely the print, so nothing here is
        // re-evaluated later. A process containing this tag is forced to a
        // coroutine (see is_simple_always) so its call site can ramp `func`'s
        // coroutine frame through the task-call record ABI.
        StrobePend,
        // Schedule the compiler-synthesized zero-formal task `func` (payload:
        // extra -> StrobePendInfo) into the Reactive region of the current time
        // slot. LRM 4.4.2.5: "During property evaluation, pass/fail code shall
        // be scheduled in the Reactive region of the current time slot" -- so
        // this is how an Observed-region monitor hands the action block of its
        // verdict (LRM 4.4.2.6) to the one region allowed to run it.
        //
        // `func` is ramped per execution, never once: an assertion can fail on
        // any number of clock ticks and each failure needs its own frame. A
        // process containing this tag is forced to a coroutine (see
        // is_simple_always) so its call site can ramp `func` through the
        // task-call record ABI, exactly as StrobePend is.
        ReactivePend,
        // $monitor / $fmonitor (LRM 21.2.2): register the zero-formal monitor body
        // `func` as the single active monitor (payload: extra -> StrobePendInfo).
        // The backend stores its ramp function pointer; the Postponed-region drain
        // runs it each subsequent time step. A new MonitorSet replaces the prior.
        MonitorSet,
        // The change-detecting print inside a monitor body (payload: op_extra — op
        // is the destination descriptor, extra a DisplayInfo). Renders the format +
        // args and emits to the descriptor only if the output changed since the
        // last emit (rt_monitor_emit).
        MonitorEmit,
        // $monitoron / $monitoroff (payload: none): enable / disable the monitor.
        MonitorOn,
        MonitorOff,
        // Two-argument $fopen(name, mode): open a single file descriptor with
        // the C stdio mode string, -> 32-bit FD value (payload: binary — l is
        // the filename value, r the mode string; both packed-ASCII vectors the
        // backend spills). LRM §21.3. Lowers to rt_fopen_fd.
        FopenFd,
        // One-argument $fopen(name): open a fresh multichannel descriptor for
        // writing, -> 32-bit MCD value (payload: unary, the filename value).
        // LRM §21.3. Lowers to rt_fopen_mcd.
        FopenMcd,
        // $fclose: close a file descriptor (payload: unary, the descriptor value).
        // LRM §21.3.2. Lowers to an rt_fclose call.
        Fclose,
        // -- File read / positioning system functions (LRM §21.3) --
        // Each -> a 32-bit integer, lowered to its rt_* call.
        // $fgetc(fd) -> next byte, or -1 at EOF (payload: unary, the fd).
        Fgetc,
        // $ungetc(c, fd) -> c, or -1 on failure (payload: binary — l is the
        // character, r the fd, in call order).
        Ungetc,
        // $feof(fd) -> nonzero once EOF has been seen (payload: unary).
        Feof,
        // $ftell(fd) -> current byte offset, or -1 (payload: unary).
        Ftell,
        // $fseek(fd, offset, operation) -> 0 on success, -1 on failure
        // (payload: op_extra -> FseekInfo; op is the fd).
        Fseek,
        // $rewind(fd) -> 0, seek to start (payload: unary).
        Rewind,
        // $fgets: read a line from a descriptor into a packed-vector lvalue, ->
        // character count (payload: binary — l is the destination lvalue, r the
        // descriptor value). LRM §21.3.4. Lowers to rt_fgets.
        Fgets,
        // $sformat / $swrite: format value args and store the result into a string
        // lvalue (payload: extra -> SformatInfo). LRM §21.3.3. Lowers to rt_sformat.
        Sformat,
        // $sformatf: format value args, -> new string value (payload: extra ->
        // SformatfInfo). LRM §21.3.3. Lowers to rt_sformat_str into the
        // instruction's string temp.
        Sformatf,
        // $ferror: write a descriptor's error message into a string lvalue, ->
        // error code (payload: binary — l is the destination lvalue, r the
        // descriptor value). LRM §21.3.6. Lowers to an rt_ferror call.
        Ferror,
        // $fscanf: parse an FD source into output lvalues per a format, ->
        // conversion count (payload: extra -> ScanfInfo, whose source is the FD
        // value). LRM §21.3.4. Lowers to an rt_fscanf call.
        Fscanf,
        // $sscanf: like Fscanf, but the source is a packed-ASCII string value
        // the backend spills (payload: extra -> ScanfInfo). Lowers to rt_sscanf.
        Sscanf,
        // $fread into a single packed-vector lvalue (payload: binary — l is the
        // destination lvalue, r the descriptor value), -> bytes read. LRM
        // §21.3.4. Lowers to an rt_fread call.
        FreadWord,
        // $fread into an unpacked-array memory (payload: op_extra -> FreadMemInfo;
        // op is the fd value), -> bytes read. LRM §21.3.4. Lowers to an rt_fread call.
        FreadMem,
        // $timeformat: set the %t format (units / precision / suffix / min width)
        // for all later %t specifiers (payload: timeformat -> TimeFormatInfo).
        // LRM §20.4.3. Lowers to an rt_timeformat_set call.
        TimeFormat,
        // $finish / $stop: end the simulation and exit (payload: extra ->
        // FinishInfo, carrying the diagnostic level and the compile-time banner
        // prefix). LRM §20.2 has $stop suspend and $finish exit; with no resume
        // mechanism a suspend is observationally an exit, so both are treated
        // identically apart from the banner's task name.
        Finish,
        // unique/priority violation report (payload: extra -> CaseViolInfo).
        // LRM 12.5.3 / 12.4.2. Non-fatal — lowers to an rt_observed_case_violation
        // call that queues the report for the Observed region (LRM 4.4.2.4).
        CaseViol,
        // Severity system task report ($info / $warning / $error / $fatal, LRM
        // 20.10) or the default failure of an immediate assertion (LRM 16.3),
        // both non-terminating (payload: extra -> SeverityInfo, carrying the
        // severity level, compile-time banner prefix, and $display-style format
        // + value args). Lowers to an rt_severity call that renders the message
        // and writes the labelled diagnostic to stderr, then returns. $fatal's
        // termination is a separate Finish the frontend emits after this report.
        Severity,
        // $fflush: flush a descriptor's output buffers (payload: unary, the
        // descriptor value; the no-argument form passes a constant 0, which the
        // runtime treats as "flush all"). LRM §21.3.6. Lowers to an rt_fflush call.
        Fflush,
        // $system: run a shell command via the C system() (payload: op_imm — op
        // is the command string value, imm the has-arg flag: 1 for $system(cmd),
        // 0 for the no-arg $system() which passes NULL). -> i32, the raw system()
        // return (a POSIX wait-status). LRM §20.17.1. Gated behind
        // --allow-system (see SystemAccess); lowers to an rt_system call that
        // itself re-checks the access tier as a backstop.
        System,
        // $readmemh / $readmemb: load a hex/binary file into an unpacked-array
        // memory (payload: op_extra -> ReadMemInfo; op is the filename value). LRM
        // §21.4. Lowers to an rt_readmem call.
        ReadMem,

        // -- Value-change dump (VCD, LRM §21.7; see docs/VCD.md) --
        // $dumpfile("f.vcd"): set the output path (payload: unary, the filename
        // value). Lowers to an rt_vcd_open call.
        DumpFile,
        // $dumpvars: begin dumping (payload: none). Only the whole-design form
        // ($dumpvars / $dumpvars(0)) is supported; a scope-subset selector is
        // rejected at the frontend. The backend enumerates every dumpable signal,
        // emits the descriptor table, and lowers this to an rt_vcd_start call.
        DumpVars,
        // $dumplimit(n): stop dumping once the file reaches `n` bytes (payload:
        // unary, the byte count). Lowers to an rt_vcd_limit call.
        DumpLimit,
        // $dumpoff / $dumpon / $dumpall / $dumpflush (payload: none). Suspend (with
        // an all-x checkpoint) / resume / force-dump-now / flush-to-disk. Each
        // lowers to the matching rt_vcd_* control call.
        DumpOff,
        DumpOn,
        DumpAll,
        DumpFlush,

        // -- System functions --
        // $time / $stime: read the simulation clock, -> 64-bit unsigned (payload:
        // scale). Lowers to a `rt_wheel_now` call; the value is always known (no
        // x/z). The wheel counts in the design's smallest time unit; the backend
        // rounds the tick count down by `scale` (the calling module's unit / the
        // smallest unit) to the module time unit (LRM §20.3.1). `scale` is 1 when
        // unitless or the module owns the smallest unit. $stime truncates the
        // result to 32 bits via a Trunc.
        SimTime,
        // $random: IEEE Annex N reference PRNG, -> 32-bit signed two-state (payload:
        // none). Lowers to an `rt_random` call; the value is always known (no
        // x/z). The unseeded (process-global) form only; the seeded form
        // `$random(seed)` lowers to DistBinary(Uniform, ...) instead.
        Random,
        // A probabilistic distribution function (LRM §20.15.2), split by
        // parameter count; DistBinary also covers the seeded $random(seed)
        // (§20.15.1, Uniform over the full int32 span). Payload: op_extra —
        // op is the seed word, extra a DistUnaryInfo / DistBinaryInfo carrying
        // the parameter(s) and the ir::DistOp. Lowers to the matching
        // rt_dist_* call, returning an i64 with the updated seed packed in
        // [32,64) and the signed integer draw in [0,32); the frontend slices
        // the seed back to the inout lvalue and takes the draw. The result is
        // always known (no x/z).
        DistUnary,
        DistBinary,
        // $urandom (LRM §18.13.1): a per-process xoshiro256++ draw, -> a 32-bit
        // unsigned two-state `int`. No operand (payload: none); reads and
        // advances the calling thread's xoshiro256++ state, inlined by the
        // backend. A process containing this tag is forced to a coroutine (the
        // state lives in its frame) — see is_simple_always; in a function or task
        // it is the caller's state, an implicit parameter of the call ABI.
        Urandom,
        // $urandom(seed) (LRM §18.13.1): reseed the process RNG from `seed`
        // (payload: unary) then draw, -> a 32-bit unsigned `int`.
        UrandomSeeded,
        // $urandom_range(maxval, minval) (LRM §18.13.2): an inlined Lemire
        // multiply-shift of one process RNG draw into [min, max] (payload:
        // binary l=maxval, r=minval; the backend swaps when min > max), -> a
        // 32-bit unsigned `int`.
        UrandomRange,
        // $clog2: ceil(log2(operand)) with the operand treated as unsigned and
        // $clog2(0) == 0, -> a 32-bit signed `integer` (payload: unary). LRM
        // §20.8.1. The operand's own width bounds the count; any x/z bit in it
        // makes the whole result X (iverilog oracle — the LRM is silent). Lowers
        // to a count-leading-zeros: `operand == 0 ? 0 : W - clz(operand - 1)`.
        Clog2,
        // $countbits / $countones / $onehot / $onehot0 / $isunknown building
        // block (payload: op_extra -> CountBitsInfo; the op is the counted
        // value). Tallies the operand's bits whose 4-state value is in the
        // info mask (LRM 20.9), -> a definite 2-state 32-bit `int`. $countones
        // masks CountOne; $isunknown is `CountBits(CountX|CountZ) != 0`; the
        // one-hot forms have their own tags below. A constant operand folds in
        // slang and never reaches this tag.
        CountBits,
        // $onehot / $onehot0: exactly-one / at-most-one definite-1 bit in the
        // operand (LRM 20.9), -> a definite 1-bit result (payload: unary).
        // x/z bits are not definite 1, so they never count.
        OneHot,
        OneHot0,
        // $isunknown: nonzero iff any operand bit is x/z (LRM 20.9), -> a
        // definite 1-bit result (payload: unary).
        IsUnknown,
        // $test$plusargs(str) (payload: extra -> TestPlusArgsInfo), -> a 1-bit
        // definite boolean. LRM §21.6: nonzero iff some command-line plusarg's
        // prefix matches every character of `str`. Lowers to an
        // rt_test_plusargs call.
        TestPlusArgs,
        // $value$plusargs(user_string, var) (payload: op_extra ->
        // ValuePlusArgsInfo; op is the output lvalue), -> a 1-bit definite
        // boolean. LRM §21.6: on a matching plusarg, converts its remainder per
        // the user_string's format character into the lvalue and returns
        // nonzero; otherwise leaves the lvalue untouched and returns 0. Lowers
        // to an rt_value_plusargs call.
        ValuePlusArgs,
        // -- Appended, and deliberately: a tag's numeric value is part of every IR
        // digest tests/sva_test.cpp pins, so a new opcode goes at the END rather
        // than beside its relatives. Inserting one mid-enum renumbers every tag
        // after it and re-hashes every body containing one, with the instruction
        // count untouched -- a diff that looks like a lowering change and is not. --
        // `expect` (LRM 16.17): block the executing process until the monitor
        // named by the payload (extra -> ExpectInfo) resolves the attempt this
        // execution asked for, then resume it in the REACTIVE region.
        //
        // Three parts, and only the middle one is this instruction. The REQUEST is
        // an ordinary Store the frontend emits just before this, into the monitor's
        // hidden request signal: it is what turns a monitor whose property starts an
        // attempt every cycle into the one-shot LRM 16.17 asks for, consumed by the
        // next clocking edge's seed. This instruction PARKS the caller in that
        // monitor's waiter queue and suspends. The WAKE is ExpectWake, emitted into
        // the monitor's own Observed process.
        //
        // Reactive and not the parked body's own region: the verdict exists only
        // once the property has been evaluated in Observed, and LRM 4.4.2.5 hands
        // everything a property decides to the Reactive region of that same time
        // slot. So the destination is a property of `expect` itself, which is why
        // this suspension point does not consult the home region the way a wheel
        // timer or an event scan has to.
        //
        // A process containing this tag is forced to a coroutine (see
        // is_simple_always): it has a suspension point, and the handle the waiter
        // queue holds is that coroutine's.
        ExpectWait,
        // The wake half of an `expect` (payload: op_imm — op is a 1-bit value that
        // is high in the cycle the attempt resolved, imm the monitor index). Drains
        // that monitor's waiter queue onto the reactive ring.
        //
        // Emitted by emit_monitor into the monitor's Observed process and nowhere
        // else, so unlike ExpectWait it can never appear in a body a user wrote --
        // no reactive-safety rule needs to mention it.
        //
        // Every waiter in the queue belongs to the attempt that just resolved, and
        // that is an invariant the frontend maintains rather than something checked
        // here: a process may only join the queue while no attempt is in flight (see
        // the refusal in lower_concurrent_assertion), so the queue is always exactly
        // the cohort of the next attempt to resolve.
        ExpectWake,
        // -- Constrained randomization (LRM 18). All three reach their state through
        // an LVALUE operand rather than an object handle plus a layout rule: the
        // rand block is ordinary struct fields, so the frontend selects the field and
        // the backend needs no knowledge of where the block sits. --
        // One xoshiro256++ step of an OBJECT's RNG (payload: unary -- the operand is
        // the state lvalue, a 4-element unpacked array of `bit [63:0]`, LRM 18.14.2),
        // -> the high 32 bits as a definite 2-state unsigned value. Exactly what
        // Urandom computes, on a caller-supplied stream instead of the thread's.
        //
        // Deliberately NOT in ir_draws_rng and NOT blocks_thunk, unlike Urandom. That
        // stream is frame-resident, so an elided coroutine has nowhere to keep it;
        // this one is heap-resident in the object, so a thunk-lowered body restarts
        // no stream and owns none. Grouping this with $urandom would be a regression,
        // not a fix.
        ObjRandom,
        // Reseed an object's RNG from one 64-bit selector (payload: binary -- l is
        // the state lvalue, r the seed value). Produces NO value. The splitmix64
        // expansion rt_urandom_seed already performs for a process, which is what
        // `obj.srandom(seed)` and the seeding of a fresh object (LRM 18.14.2) need.
        //
        // Heap-resident state, so the same two exclusions as ObjRandom apply.
        ObjRandSeed,
        // One cyclic-randomization pick for a `randc` property (LRM 18.6) (payload:
        // op_extra -> RandcInfo; op is that property's 256-bit used-this-cycle bitmap
        // lvalue, RandcInfo::rng the object's RNG state lvalue), -> a definite 2-state
        // unsigned 32-bit value in [0, nvals).
        //
        // Picks uniformly among the values whose bit is still CLEAR, then sets that
        // bit; when no bit is clear the bitmap is cleared first and a new cycle
        // starts. That is what makes each cycle a permutation of [0, nvals) with no
        // permutation array to store. Lowers to an rt_randc_next call.
        //
        // Heap-resident state, so the same two exclusions as ObjRandom apply.
        RandcNext,

        // -- Process control (the `process` class, LRM 9.7) --
        // Every one carries `extra -> ProcCtlInfo`, whose `op` selects the method.
        // A thread's control block is native, untraced storage (a static slot per
        // process, a pooled record per fork child) and a `process` handle is an
        // ordinary class handle onto it; see docs/design/process-class.md.
        //
        // Value-producing: `process::self()`, `p.status()`, `p.get_randstate()`.
        ProcQuery,
        // Statement, zero-time: `p.kill()`, `p.suspend()`, `p.resume()`,
        // `p.srandom(seed)`, `p.set_randstate(s)`, and `disable fork` (LRM 9.6.3),
        // which is `kill` over the calling thread's descendants.
        ProcCtl,
        // Statement that PARKS the calling thread: `p.await()` and `wait fork`. Like
        // ExpectWait, a body holding one is forced to a coroutine (is_simple_always)
        // and the tag blocks the thunk path.
        ProcWait,

        // -- Wait queue of a built-in synchronization object (LRM 15.3, 15.4) --
        // The park/wake primitive `semaphore` and `mailbox #(T)` are built out of;
        // every one carries `extra -> SyncInfo`. The object itself is an ordinary
        // class object whose synthetic body holds the resource (a key count, a
        // message FIFO) plus one FIFO wait queue per resource, and the frontend
        // expands each method into ordinary IR around these two tags.
        // See docs/design/mailbox-and-semaphore.md.
        //
        // Statement that PARKS the calling thread on a queue, -> 1 when it resumed
        // as that queue's head (the frontend ANDs that with its own resource test).
        // Like ProcWait it joins tag_suspends and blocks the thunk path.
        SyncWait,
        // Statement, zero-time: leave a queue (`Leave`, which also hands off to the
        // new head) or wake it (`WakeHead` / `WakeAll`).
        SyncCtl,

        // -- Interface classes (LRM 8.26, docs/design/interface-classes.md) --
        // Does the object a handle refers to belong to a class that implements the
        // payload interface class? (payload: op_type -> op is the handle value,
        // type is the interned interface Kind::Class.) Yields a 1-bit 2-state
        // value; a null handle answers 1, exactly as ClassIsA does and for the same
        // reason (LRM 8.26.5 defers a null source to 8.16). Appended at the end of
        // this enum deliberately: inserting mid-enum renumbers every later tag and
        // changes the monitor digests sva_test pins.
        ClassImplements,

        // -- Program control tasks (LRM 24.7, docs/design/program-blocks.md §5.2) --
        // `$exit()`: a statement with no payload and no result. It terminates every
        // `initial` procedure of the program instance the CALLING THREAD originates
        // in, plus all of their descendant threads.
        //
        // The instance is deliberately NOT carried here. 24.7 scopes the effect by
        // the thread ("from a thread or its descendent thread originating in an
        // initial procedure of a program block"), not by the lexical site, so the
        // owner is a property of the body this op ends up in: the backend reads it
        // off the program `initial` being lowered, or off a per-function owner
        // closure for a fork child or a subroutine, and refuses loudly where no
        // single owner exists. Appended at the end of this enum deliberately, like
        // ClassImplements above: inserting mid-enum renumbers every later tag and
        // changes the monitor digests sva_test pins.
        ProgramExit,

        // -- Functional coverage (LRM 19, docs/design/functional-coverage.md) --
        // Every one carries `extra -> CoverInfo`, whose `op` selects the primitive.
        // A covergroup instance is runtime-owned, immortal storage (LRM 19.11.3
        // aggregates type coverage over instances the design has dropped), and a
        // covergroup handle is its address, so none of this touches the collector.
        // Appended at the end of this enum deliberately, like the two tags above:
        // inserting mid-enum renumbers every later tag and changes the monitor
        // digests sva_test pins.
        //
        // Value-producing: `new`, the registry walk, and every coverage query.
        CoverQuery,
        // Statement, zero-time: a coverpoint sample, `start`/`stop`,
        // `set_inst_name`.
        CoverCtl,
    };

    // Which union member a tag activates. Drives payload_kind() / operands().
    enum class PayloadKind : uint8_t {
        None,
        // The label member: a lexical branch target, not an SSA operand.
        Label,
        // The u64 scale member (sim_time). No SSA operands.
        Scale,
        Ref,
        Signal,
        Unary,
        Binary,
        OpType,
        Arg,
        Local,
        Repl,
        // The op_imm member: one operand plus a 32-bit immediate.
        OpImm,
        OpExtra,
        Extra,
        Call,
        CallVirtual,
        TaskCall,
        Display,
        // The op_extra member with a DisplayInfo record: op is the descriptor.
        FDisplay,
        TimeFormat,
        Wait,
        Sformat,
        Sformatf,
        Scanf,
        Severity,
    };

    union Payload {
        // Inactive / nullary placeholder.
        uint32_t none;
        // Label of the enclosing NamedBlock a `disable` leaves (disable). Not an
        // operand: the target is a lexical region, resolved by the backend
        // against the stack of blocks it is currently inside.
        uint32_t label;
        // Time-unit multiplier for sim_time: $time returns the wheel tick count
        // rounded down by this to the calling module's time unit. 1 == unitless.
        uint64_t scale;
        // Interned constant value (constant).
        Ref ref;
        // Module signal handle (signal).
        Signal signal;
        // Single operand (load, bnot, lnot, red_*, ret).
        Index unary;
        // Two operands (arithmetic, bitwise, shift, compare, store, bit_select).
        struct {
            // Left operand.
            Index l;
            // Right operand.
            Index r;
        } binary;
        // Operand plus a target type (resize).
        struct {
            // Value being converted.
            Index op;
            // Target type.
            Ref type;
        } op_type;
        // Parameter position plus its type (arg).
        struct {
            // Zero-based argument position.
            uint32_t position;
            // Parameter type.
            Ref type;
        } arg;
        // Function-frame local slot index plus its type (local).
        struct {
            // Index into the function's local table / frame layout.
            uint32_t index;
            // Storage type of the local.
            Ref type;
        } local;
        // Replicated value plus a constant count (repl).
        struct {
            // Value being replicated.
            Index value;
            // Replication count.
            uint32_t count;
        } repl;
        // One operand plus a 32-bit immediate (real_math_unary's RealMathOp,
        // stream_reverse's slice size).
        struct {
            // The operand value.
            Index op;
            // Tag-defined immediate.
            uint32_t imm;
        } op_imm;
        // Primary operand plus a handle to an extra struct (select, part_select, cond).
        struct {
            // Condition / base operand.
            Index op;
            // Handle to the op's extra payload.
            ExtraIndex extra;
        } op_extra;
        // Handle to a variable-length operand slice (concat) or to a tag's
        // extra record with no primary operand (loop, call, display, wait,
        // timeformat, ...). The record type is selected by the tag's
        // PayloadKind.
        ExtraIndex extra;
    };

    // Discriminant selecting the active payload member.
    Tag tag;
    // Per-tag operand data.
    Payload payload;
};

static_assert(sizeof(Inst::Payload) <= 8, "payload must stay compact");
static_assert(std::is_trivially_copyable_v<Inst::Payload>);

// Maps an instruction tag to the active payload union member. Constexpr in the
// header so per-instruction dispatch (printer, operand walks, passes) inlines
// to a dense-switch lookup instead of an out-of-line call.
constexpr Inst::PayloadKind payload_kind(Inst::Tag tag) {
    using T = Inst::Tag;
    using K = Inst::PayloadKind;
    switch (tag) {
        case T::Constant:
            return K::Ref;
        case T::Signal:
            return K::Signal;
        case T::EventNew:
            return K::None;
        case T::Arg:
            return K::Arg;
        case T::Local:
            return K::Local;

        case T::EventTrigger:
        case T::EventTriggered:
        case T::EventTriggerNb:
        case T::Load:
        case T::Bnot:
        case T::RedAnd:
        case T::RedOr:
        case T::RedXor:
        case T::Lnot:
        case T::FNeg:
        case T::Ret:
        case T::Delay:
        case T::Fclose:
        case T::FopenMcd:
        case T::Fgetc:
        case T::Feof:
        case T::Ftell:
        case T::Rewind:
        case T::Fflush:
        case T::DumpFile:
        case T::DumpLimit:
        case T::StrLen:
        case T::StrUpper:
        case T::StrLower:
        case T::StrFromPacked:
        case T::StrAtoi:
        case T::StrAtohex:
        case T::StrAtooct:
        case T::StrAtobin:
        case T::StrAtoreal:
        case T::Clog2:
        case T::Ctz:
        case T::Clz:
        case T::OneHot:
        case T::OneHot0:
        case T::IsUnknown:
        case T::UrandomSeeded:
        case T::ObjRandom:
            return K::Unary;

        case T::BitSelect:
        case T::Store:
        case T::StoreNb:
        case T::Add:
        case T::Sub:
        case T::Mul:
        case T::UDiv:
        case T::SDiv:
        case T::UMod:
        case T::SMod:
        case T::Band:
        case T::Bor:
        case T::Bxor:
        case T::Bxnor:
        case T::WireRes:
        case T::WandRes:
        case T::WorRes:
        case T::Sll:
        case T::Srl:
        case T::Sra:
        case T::Eq:
        case T::Ne:
        case T::ULt:
        case T::ULe:
        case T::UGt:
        case T::UGe:
        case T::SLt:
        case T::SLe:
        case T::SGt:
        case T::SGe:
        case T::CaseEq:
        case T::CasezEq:
        case T::CasexEq:
        case T::WildEq:
        case T::Land:
        case T::Lor:
        case T::Limplies:
        case T::Lequiv:
        case T::StrGetc:
        case T::StrCmp:
        case T::StrICmp:
        case T::StrRepl:
        case T::StrItoa:
        case T::StrHextoa:
        case T::StrOctoa:
        case T::StrBintoa:
        case T::StrRealtoa:
        case T::StrAppendPacked:
        case T::FAdd:
        case T::FSub:
        case T::FMul:
        case T::FDiv:
        case T::FPow:
        case T::FEq:
        case T::FNe:
        case T::FLt:
        case T::FLe:
        case T::FGt:
        case T::FGe:
        case T::UrandomRange:
        // The write-back file reads: l is the destination lvalue (like Store's
        // lhs), r the descriptor value.
        case T::Fgets:
        case T::Ferror:
        case T::FreadWord:
        // Two-argument $fopen: l is the filename, r the mode string.
        case T::FopenFd:
        // $ungetc: l is the character, r the descriptor.
        case T::Ungetc:
        // `->> #d e`: l is the delay in ticks, r is the event handle.
        case T::EventTriggerNbDelayed:
        // obj_rand_seed: l is the object's RNG state lvalue, r the 64-bit seed.
        case T::ObjRandSeed:
            return K::Binary;

        case T::ZExt:
        case T::SExt:
        case T::Trunc:
        case T::ToFour:
        case T::ToTwo:
        case T::StrToPacked:
        case T::UItoF:
        case T::SItoF:
        case T::FtoI:
        case T::FtoITrunc:
        case T::FBitcast:
        case T::FResize:
        case T::HandleDeref:
        case T::ClassIsA:
        case T::ClassImplements:
        // Handle-lvalue queue / dynamic-array ops with no extra operand: op is
        // the handle, type the interned Queue / DynArray type.
        case T::QueueSize:
        case T::QueuePopBack:
        case T::QueuePopFront:
        case T::QueueDeleteAll:
        case T::ContainerFmtP:
        case T::DynArraySize:
        case T::DynArrayDelete:
        // Array ordering methods (LRM 7.12.2): op is the handle, type the
        // interned DynArray / Queue type; no extra operand.
        case T::DynArrayReverse:
        case T::DynArraySort:
        case T::DynArrayRsort:
        case T::DynArrayShuffle:
        case T::QueueReverse:
        case T::QueueSort:
        case T::QueueRsort:
        case T::QueueShuffle:
        // Associative-array ops with no extra operand: op is the handle, type
        // the interned Assoc type.
        case T::AssocNum:
        case T::AssocDeleteAll:
        case T::AssocDefault:
        case T::AssocReset:
            return K::OpType;
        case T::ClassNew:
            return K::Ref;
        case T::Repl:
            return K::Repl;
        case T::Concat:
        case T::IfaceRef:
        case T::StrengthRes:
        case T::Loop:
        case T::NamedBlock:
            return K::Extra;
        case T::Disable:
        case T::DisableRemote:
        case T::DeferFlush:
            return K::Label;
        case T::Call:
            return K::Call;
        case T::CallVirtual:
        case T::TaskCallVirtual:
            return K::CallVirtual; // both carry VirtualCallInfo
        case T::TaskCall:
            return K::TaskCall;
        case T::Display:
            return K::Display;
        case T::FDisplay:
            return K::FDisplay;
        case T::Sformat:
            return K::Sformat;
        case T::Sformatf:
            return K::Sformatf;
        case T::Fscanf:
        case T::Sscanf:
            return K::Scanf;
        case T::RealMathUnary:
        case T::StreamReverse:
        case T::NetPull:
        case T::System:
        // expect_wake: op is the resolved bit, imm the monitor index.
        case T::ExpectWake:
            return K::OpImm;
        case T::TimeFormat:
            return K::TimeFormat;
        case T::Break:
        case T::Continue:
        case T::DisableAbort:
        case T::ProgramExit:
        case T::RetVoid:
        case T::DumpVars:
        case T::DumpOff:
        case T::DumpOn:
        case T::DumpAll:
        case T::DumpFlush:
        case T::Random:
        case T::Urandom:
            return K::None;
        case T::SimTime:
            return K::Scale;
        case T::Finish:
        case T::CaseViol:
        case T::TestPlusArgs:
        case T::StrobePend:
        case T::ReactivePend:
        case T::MonitorSet:
        // expect_wait: extra -> ExpectInfo, the monitor whose queue holds the
        // parked handle. No operands -- the request store is a separate Store.
        case T::ExpectWait:
            return K::Extra;
        // process control: extra -> ProcCtlInfo, whose `args` run is the operands
        case T::ProcQuery:
        case T::ProcCtl:
        case T::ProcWait:
        // wait queue: extra -> SyncInfo, whose `args` run is the two queue-word
        // lvalues.
        case T::SyncWait:
        case T::SyncCtl:
        // functional coverage: extra -> CoverInfo, whose `args` run is the operands.
        case T::CoverQuery:
        case T::CoverCtl:
            return K::Extra;
        case T::MonitorEmit:
            return K::FDisplay; // op = descriptor, extra = DisplayInfo
        case T::MonitorOn:
        case T::MonitorOff:
            return K::None;
        case T::Fork:
            // Children are FunctionIds (not SSA operands), walked from ForkInfo;
            // its captured-lvalue operands live in the same record (see
            // extra_operand_shape), so it rides the Extra path.
            return K::Extra;
        case T::Severity:
            return K::Severity;
        case T::Wait:
            return K::Wait;

        case T::StoreNbDelay:
        case T::StoreInertialDelay:
        case T::StoreNbEvent:
        case T::Select:
        case T::SelectLazy:
        case T::PartSelect:
        case T::DynPartSelect:
        case T::ElemSelect:
        case T::ArrSlice:
        case T::MemberSelect:
        case T::IfaceSignal:
        case T::AggCopy:
        case T::AggEq:
        case T::DynArrayNew:
        case T::DynArrayNewInit:
        case T::DynArrayCopy:
        case T::DynArrayCopyQueue:
        case T::QueuePushBack:
        case T::QueuePushFront:
        case T::QueueInsert:
        case T::QueueDeleteIdx:
        case T::QueueCopy:
        case T::HandleCopyNb:
        case T::QueueCopyDyn:
        case T::QueueAppendRange:
        case T::AssocSelect:
        case T::AssocExists:
        case T::AssocDeleteKey:
        case T::AssocCopy:
        case T::AssocFirst:
        case T::AssocLast:
        case T::AssocNext:
        case T::AssocPrev:
        case T::Cond:
        case T::Switch:
        case T::Fseek:
        case T::RealMathBinary:
        case T::DistUnary:
        case T::DistBinary:
        case T::InsideSet:
        case T::Pow:
        case T::FreadMem:
        case T::ReadMem:
        case T::StrPutc:
        case T::StrSubstr:
        case T::EnumName:
        case T::TaggedCheck:
        case T::TaggedFmtP:
        case T::CountBits:
        case T::ValuePlusArgs:
        case T::EventTriggerNbEvent:
        case T::RandcNext:
            return K::OpExtra;
    }

    return K::None;
}

// How Ir::operands() reads the SSA operands of a PayloadKind::Extra tag.
//
// PayloadKind says which union member a tag activates, but every Extra tag
// activates the same one (`extra`) while pointing at a different record, so it
// cannot say how to decode that record. This table supplies the missing half.
// Guessing is unsound: an ExtraSlice is a {start, end} pair, so decoding a
// record that is not one yields a span over unrelated words -- either far out
// of bounds (FinishInfo's {banner, banner_len} with len < banner wraps to a
// huge count) or, worse, quietly in bounds and meaningless (ForkInfo's
// children are FunctionIds, LoopInfo's cond is a region).
enum class ExtraOperandShape : uint8_t {
    // Not an Extra-payload tag; operands come from the payload member instead.
    // Never observed for a real Extra tag -- see the static_assert below.
    NotExtra,
    // The record holds no SSA operands: regions, FunctionIds, packed bytes or
    // constants only.
    None,
    // `extra` is a bare ExtraSlice that is itself the operand run.
    SliceIsOperands,
    // `extra` is a StrengthResInfo whose `values` slice is the operand run.
    StrengthResValues,
    // `extra` is a ForkInfo whose `args` slice is the operand run (the captured
    // frame lvalues each child aliases); `children` are FunctionIds, not operands.
    ForkArgs,
    // `extra` is a ProcCtlInfo whose `args` slice is the operand run (the receiver
    // handle, then any value argument); the op selector and result type are not.
    ProcArgs,
    // `extra` is a SyncInfo whose `args` slice is the operand run (the queue's
    // head-word and tail-word lvalues); the site id and op selector are not.
    SyncArgs,
    // `extra` is a CoverInfo whose `args` slice is the operand run (the covergroup
    // handle, then the op's value arguments); the type index, coverpoint index, op
    // selector and result type are not.
    CoverArgs,
};

// Constexpr for the same reason as payload_kind(): operand walks inline it.
constexpr ExtraOperandShape extra_operand_shape(Inst::Tag tag) {
    using T = Inst::Tag;
    using S = ExtraOperandShape;
    switch (tag) {
        case T::Concat:
            return S::SliceIsOperands;
        case T::StrengthRes:
            return S::StrengthResValues;
        // fork: the children are FunctionIds, but the captured frame lvalues a
        // child aliases through its ref formals ARE operands (ForkInfo::args).
        case T::Fork:
            return S::ForkArgs;
        // process control: the receiver handle and any value argument are operands;
        // the op selector and result type are not.
        case T::ProcQuery:
        case T::ProcCtl:
        case T::ProcWait:
            return S::ProcArgs;
        // wait queue: the two queue-word lvalues are operands; the site and op are
        // not.
        case T::SyncWait:
        case T::SyncCtl:
            return S::SyncArgs;
        // functional coverage: the handle and the op's value arguments are operands;
        // the type / coverpoint indices and the op selector are not.
        case T::CoverQuery:
        case T::CoverCtl:
            return S::CoverArgs;
        // loop: the predicate and regions are not top-level operands (the
        // predicate is defined inside the cond region, walked via region()).
        case T::Loop:
        // named_block: a body region and a label, no operands.
        case T::NamedBlock:
        // strobe_pend / monitor_set / reactive_pend: the target is a FunctionId;
        // the deferred body re-evaluates its own args.
        case T::StrobePend:
        case T::ReactivePend:
        case T::MonitorSet:
        // expect_wait: a monitor index only.
        case T::ExpectWait:
        // iface_ref: a scope id only.
        case T::IfaceRef:
        // finish / case_viol / test_plusargs: banner / query bytes only.
        case T::Finish:
        case T::CaseViol:
        case T::TestPlusArgs:
            return S::None;
        default:
            return S::NotExtra;
    }
}

// A new Extra tag must be classified above. The default arm is deliberately
// safe rather than exhaustive -- listing all 200-odd non-Extra tags would just
// restate payload_kind() -- so this is what turns forgetting one into a build
// break instead of a fabricated operand span. Values outside the enum answer
// None from payload_kind() and so pass vacuously.
constexpr bool extra_operand_shapes_total() {
    for (unsigned v = 0; v <= 0xFF; ++v) {
        const auto tag = static_cast<Inst::Tag>(v);
        if (payload_kind(tag) == Inst::PayloadKind::Extra && extra_operand_shape(tag) == ExtraOperandShape::NotExtra) {
            return false;
        }
    }

    return true;
}

static_assert(extra_operand_shapes_total(), "every PayloadKind::Extra tag needs an extra_operand_shape() case");

// Operands a tag carries that `Ir::operands` cannot put in its single span.
//
// operands() returns one `std::span`, so a tag whose variadic run lives in the
// extra buffer cannot also expose a scalar that lives elsewhere -- the two are
// not contiguous and copying the run out is the cost the zero-copy view exists
// to avoid. Seven tags are in that position. This table names what each one keeps
// back, and `Ir::for_each_operand` visits it, so a walk that must see every use
// (def-use, liveness, DCE) has a total interface while the span stays fast.
//
// Two tails are CONDITIONAL -- WaitHandles (an edge may name its signal directly)
// and SformatfFmt (the format is a value only for the runtime-format form) -- so
// a tail-classified tag does not always contribute an operand.
enum class ExtraOperandTail : uint8_t {
    // operands() is already total for this tag.
    None,
    // + payload.op_extra.op: the file descriptor of fdisplay / monitor_emit,
    // whose variadic value args occupy the span.
    Descriptor,
    // + SformatInfo::dst: $sformat's destination lvalue. An lvalue IS an
    // operand by this codebase's convention -- Store's lhs, AggCopy's op/src
    // and every queue handle are all reported. Plus SformatInfo::fmt_value when
    // the format is a runtime string value rather than constant bytes.
    SformatDst,
    // + SformatfInfo::fmt_value, when $sformatf's format is a runtime string
    // value. $sformatf has no destination operand -- its result IS the value --
    // so this is the whole tail.
    SformatfFmt,
    // + ScanfInfo::source: the string or descriptor being scanned. The span
    // holds the output lvalues.
    ScanfSource,
    // + InsideSetInfo::values and ::ranges: `inside`'s member and [lo:hi]
    // range operands, two slices, neither of which is the span (which holds
    // only the selector, payload.op_extra.op).
    InsideSetMembers,
    // + WaitInfo::events[k].handle for each event edge that carries one. The
    // handles are strided three words apart inside the packed WaitEdge run, so
    // they are not a slice and cannot be a span.
    WaitHandles,
};

// Constexpr for the same reason as extra_operand_shape(): operand walks inline
// it. Same fail-closed shape too -- a short case list plus a default, with the
// static_assert below turning a missed tag into a build break.
constexpr ExtraOperandTail extra_operand_tail(Inst::Tag tag) {
    using T = Inst::Tag;
    using E = ExtraOperandTail;
    switch (tag) {
        case T::FDisplay:
        case T::MonitorEmit:
            return E::Descriptor;
        case T::Sformat:
            return E::SformatDst;
        case T::Fscanf:
        case T::Sscanf:
            return E::ScanfSource;
        case T::Sformatf:
            return E::SformatfFmt;
        case T::InsideSet:
            return E::InsideSetMembers;
        case T::Wait:
            return E::WaitHandles;
        default:
            return E::None;
    }
}

// Four of the five tails are implied by the payload kind, so they can be
// checked as a biconditional: a new tag on PayloadKind::FDisplay / Sformat /
// Sformatf / Scanf fails the build until it is classified, and a tag wrongly
// given one of those tails fails too. InsideSetMembers is tag-specific
// (InsideSet shares PayloadKind::OpExtra with ~40 others), so it gets the
// one-way check.
constexpr bool extra_operand_tails_total() {
    for (unsigned v = 0; v <= 0xFF; ++v) {
        const auto tag  = static_cast<Inst::Tag>(v);
        const auto tail = extra_operand_tail(tag);
        const auto kind = payload_kind(tag);

        if ((kind == Inst::PayloadKind::FDisplay) != (tail == ExtraOperandTail::Descriptor)) {
            return false;
        }
        if ((kind == Inst::PayloadKind::Sformat) != (tail == ExtraOperandTail::SformatDst)) {
            return false;
        }
        if ((kind == Inst::PayloadKind::Scanf) != (tail == ExtraOperandTail::ScanfSource)) {
            return false;
        }
        if ((kind == Inst::PayloadKind::Sformatf) != (tail == ExtraOperandTail::SformatfFmt)) {
            return false;
        }
        if (tail == ExtraOperandTail::InsideSetMembers && tag != Inst::Tag::InsideSet) {
            return false;
        }
    }

    return true;
}

static_assert(extra_operand_tails_total(),
              "a tag on PayloadKind::FDisplay / Sformat / Sformatf / Scanf needs its "
              "extra_operand_tail() case");

// Whether `tag` converts an integral value to another width or domain without
// otherwise changing it: the instructions a pass looks through to find the value.
constexpr bool is_conversion(Inst::Tag tag) {
    return tag == Inst::Tag::ZExt || tag == Inst::Tag::SExt || tag == Inst::Tag::Trunc || tag == Inst::Tag::ToFour ||
           tag == Inst::Tag::ToTwo;
}

// A read-only window onto the columns of a body, finished or still growing:
// what value typing (Typing.h) reads, so an `Ir` and a `Builder` are typed by
// the same function. Views do not own; they are valid until the body they
// look at changes.
struct IrView {
    std::span<const Inst::Tag>     tags;
    std::span<const Inst::Payload> payloads;
    std::span<const uint32_t>      extra;

    Inst::Tag tag(Index i) const {
        return tags[static_cast<uint32_t>(i)];
    }

    const Inst::Payload& payload(Index i) const {
        return payloads[static_cast<uint32_t>(i)];
    }

    template<ExtraPayload T>
    T extra_data(ExtraIndex at) const {
        return decode_extra<T>(extra, at);
    }

    std::span<const Index> region(ExtraSlice r) const {
        return decode_slice(extra, r);
    }
};

// Immutable, finished process IR: a structure of arrays (parallel tag / payload
// / location columns indexed by Index) plus a flat extra side-table. The body
// is the root region. Constructed by Builder::finish(); default construction
// yields an inert empty body (no pool), so an Ir-holding record (Function,
// Process) can be slotted before its body is built and move-assigned later.
class Ir {
public:
    Ir() = default;

    // The columns value typing reads.
    IrView view() const {
        return { { tags_.data(), tags_.size() }, { payloads_.data(), payloads_.size() }, extra_raw() };
    }

    // Intern pool backing this IR's constant / type handles. Valid only on a
    // built Ir (Builder::finish()), never on a default-constructed one.
    const InternPool& pool() const {
        return *pool_;
    }

    // Total number of instructions across all regions.
    size_t inst_count() const {
        return tags_.size();
    }

    // End of the source construct an instruction came from, exclusive, or
    // SourceLoc{0} when unrecorded. Sparse on purpose: a 16-byte SourceSpan per
    // instruction is exactly what the note on SourceSpan rules out, and only a
    // diagnostic that UNDERLINES a construct rather than pointing at it needs
    // more than `loc`. The frontend records one for index expressions, so a
    // bounds report can highlight the whole index the user wrote.
    SourceLoc end(Index i) const {
        const auto it = ends_.find(static_cast<uint32_t>(i));
        return it == ends_.end() ? SourceLoc{ 0 } : it->second;
    }

    Inst::Tag tag(Index i) const {
        return tags_[static_cast<uint32_t>(i)];
    }

    const Inst::Payload& payload(Index i) const {
        return payloads_[static_cast<uint32_t>(i)];
    }

    // Opaque source location of an instruction.
    SourceLoc loc(Index i) const {
        return locs_[static_cast<uint32_t>(i)];
    }

    // The top-level instruction sequence of the process body.
    std::span<const Index> root() const {
        return index_slice(root_);
    }

    // The whole extra side-table, raw. For content hashing / serialization
    // (regions, operand runs, packed strings, records — byte-stable since the
    // padding-free ExtraPayload guarantee); typed access stays extra_data().
    std::span<const uint32_t> extra_raw() const {
        return { extra_.data(), extra_.size() };
    }

    // The instructions of a region (e.g. a cond's then/else branch), in order.
    std::span<const Index> region(ExtraSlice r) const {
        return index_slice(r);
    }

    // A slice of the extra table holding plain words rather than operands (the
    // per-operand flags of a format-family op).
    std::span<const uint32_t> words(ExtraSlice r) const {
        const auto a = static_cast<uint32_t>(r.start);
        return { extra_.data() + a, static_cast<uint32_t>(r.end) - a };
    }

    // Decode a struct previously written with Builder::add_extra.
    template<ExtraPayload T>
    T extra_data(ExtraIndex at) const {
        return decode_extra<T>(extra_raw(), at);
    }

    // Decode `len` raw bytes previously packed at `at` by Builder (little-endian,
    // 4 chars per word). Used for $display format strings.
    std::string extra_bytes(ExtraIndex at, uint32_t len) const {
        std::string s(len, '\0');
        const auto  base = static_cast<uint32_t>(at);
        for (uint32_t i = 0; i < len; ++i) {
            s[i] = static_cast<char>((extra_[base + i / 4] >> (8 * (i % 4))) & 0xff);
        }

        return s;
    }

    // The (signal, edge) events of a `wait` instruction's WaitInfo, in order,
    // viewed directly over the extra buffer: WaitEdge is two u32-sized fields
    // (ExtraPayload), so the packed records overlay the words exactly. Valid
    // while this Ir lives, like every other extra view.
    // The (signal, edge) triggers of a `store_nb_event` instruction's
    // StoreNbEventInfo, viewed directly over the extra buffer (see wait_edges).
    std::span<const IntraTrigger> intra_trigs(ExtraIndex info) const {
        const StoreNbEventInfo si     = extra_data<StoreNbEventInfo>(info);
        const auto             start  = static_cast<uint32_t>(si.trigs.start);
        constexpr uint32_t     stride = sizeof(IntraTrigger) / sizeof(uint32_t);
        const uint32_t         count  = (static_cast<uint32_t>(si.trigs.end) - start) / stride;

        return { reinterpret_cast<const IntraTrigger*>(extra_.data() + start), count };
    }

    std::span<const WaitEdge> wait_edges(ExtraIndex info) const {
        const WaitInfo     wi     = extra_data<WaitInfo>(info);
        const auto         start  = static_cast<uint32_t>(wi.events.start);
        constexpr uint32_t stride = sizeof(WaitEdge) / sizeof(uint32_t);
        const uint32_t     count  = (static_cast<uint32_t>(wi.events.end) - start) / stride;

        return { reinterpret_cast<const WaitEdge*>(extra_.data() + start), count };
    }

    // Fill `scratch` with the SSA operands of an instruction and return a view
    // of them. Fixed-arity ops use the scratch buffer; variadic ops (concat)
    // return a view straight into the extra buffer. Region bodies are not
    // operands — walk them via region(), and note that a value a region
    // designates as its result (LoopInfo::pred, SelectLazy::then_val /
    // else_val) is defined inside that region, so it is not a top-level
    // operand either.
    //
    // Total over every tag, and sound for every tag: each index returned is a
    // real operand of `inst`, so it is in bounds for this body and was created
    // before `inst`. Extra-payload tags decode via extra_operand_shape(), which
    // a new tag must be added to or the build breaks; the result is never a
    // span reinterpreted from the wrong record.
    //
    // NOT exhaustive for the six tags extra_operand_tail() names: a tag whose
    // args are an extra slice can also carry a scalar operand outside it, and
    // one span cannot express both without copying the run out of the extra
    // buffer — the cost this zero-copy view exists to avoid. That is a
    // deliberate contract, not a gap: this returns the *span-expressible*
    // operands, and `for_each_operand` below is the total walk. A pass that
    // must see every use (def-use, liveness, DCE) MUST use that one.
    std::span<const Index> operands(Index inst, std::array<Index, 4>& scratch) const;

    // Visit every SSA operand of `inst` exactly once, in operand order.
    //
    // Total AND exhaustive, where operands() is only total: it yields the span
    // then whatever extra_operand_tail() says that tag holds back. Callback
    // rather than a span because the two runs are not contiguous, so no single
    // view can cover them and a buffer big enough for a variadic run would have
    // to allocate. This costs nothing extra for the ~205 tags with no tail.
    //
    // Region bodies are still not operands (see operands() above).
    template<typename Fn>
    void for_each_operand(Index inst, Fn&& fn) const {
        std::array<Index, 4> scratch{};
        for (const Index op : operands(inst, scratch)) {
            fn(op);
        }

        const Inst::Payload& p = payload(inst);
        switch (extra_operand_tail(tag(inst))) {
            case ExtraOperandTail::None:
                return;
            case ExtraOperandTail::Descriptor:
                fn(p.op_extra.op);
                return;
            case ExtraOperandTail::SformatDst: {
                const auto info = extra_data<SformatInfo>(p.extra);
                fn(info.dst);
                if (info.fmt_value != kNoFormatValue) {
                    fn(info.fmt_value);
                }
                return;
            }
            case ExtraOperandTail::SformatfFmt: {
                const auto info = extra_data<SformatfInfo>(p.extra);
                if (info.fmt_value != kNoFormatValue) {
                    fn(info.fmt_value);
                }
                return;
            }
            case ExtraOperandTail::ScanfSource:
                fn(extra_data<ScanfInfo>(p.extra).source);
                return;
            case ExtraOperandTail::InsideSetMembers: {
                const auto info = extra_data<InsideSetInfo>(p.op_extra.extra);
                for (const Index op : index_slice(info.values)) {
                    fn(op);
                }
                for (const Index op : index_slice(info.ranges)) {
                    fn(op);
                }
                return;
            }
            case ExtraOperandTail::WaitHandles:
                for (const WaitEdge& e : wait_edges(p.extra)) {
                    if (e.handle != kNoWaitHandle) {
                        fn(e.handle);
                    }
                }
                return;
        }
    }

private:
    friend class Builder;

    // A slice reinterpreted as a run of instruction indices.
    std::span<const Index> index_slice(ExtraSlice s) const {
        return decode_slice(extra_raw(), s);
    }

    // Constructed atomically by Builder::finish().
    Ir(const InternPool&                                 pool,
       std::vector<Inst::Tag>                            tags,
       std::vector<Inst::Payload>                        payloads,
       std::vector<SourceLoc>                            locs,
       ankerl::unordered_dense::map<uint32_t, SourceLoc> ends,
       std::vector<uint32_t>                             extra,
       ExtraSlice                                        root)
      : pool_(&pool)
      , tags_(std::move(tags))
      , payloads_(std::move(payloads))
      , locs_(std::move(locs))
      , ends_(std::move(ends))
      , extra_(std::move(extra))
      , root_(root) {}

    // Intern pool backing this IR's handles; null only when default-constructed.
    const InternPool* pool_ = nullptr;
    // Instruction tags.
    Array<Inst::Tag> tags_;
    // Per-instruction payloads, parallel to tags_.
    Array<Inst::Payload> payloads_;
    // Per-instruction opaque source locations, parallel to tags_.
    Array<SourceLoc> locs_;
    // Construct ends, keyed by instruction index; see `end`. Empty unless the
    // frontend recorded an extent, which it does only for index expressions.
    ankerl::unordered_dense::map<uint32_t, SourceLoc> ends_;
    // Flat u32 side-table for region bodies, operand slices, and extra structs.
    Array<uint32_t> extra_;
    // Top-level region: the process body (empty when default-constructed).
    ExtraSlice root_{};
};

// Result of walking an lvalue's select chain to its root storage leaf.
struct LvalueRoot {
    // The root leaf (a Signal / Local / Arg — whatever the chain bottoms out
    // on; callers switch on its tag).
    Index root;
    // False when any select was traversed: the lvalue names a bit / part /
    // element of the root, not the whole storage.
    bool whole;
};

// Walk an lvalue select chain (bit / part / element selects) to its root leaf.
// The single walker every lvalue-classifying pass shares (NBA targeting, task
// copy-out targets, may-z storage resolution), so they cannot drift apart when
// a select form is added.
LvalueRoot lvalue_root(const Ir& ir, Index lv);

} // namespace clotho::ir
