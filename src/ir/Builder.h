#pragma once

// Process IR construction. `Builder` owns the growable build-time storage
// (instruction columns + extra side-table) and produces an immutable `Ir` via
// finish(). The builder is spent after finish().
//
// Structured control flow: instructions accumulate into the current region.
// open_region()/close_region() bracket a nested body (e.g. a cond branch);
// close_region() returns the region's slice, to be handed to cond(). The
// outermost (root) region is sealed by finish().
//
// One emit method per instruction shape. Variants sharing a payload
// (arithmetic, bitwise, ...) go through the generic unary()/binary().

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "InternPool.h"
#include "Ir.h"

namespace clotho::ir {

class Builder {
public:
    explicit Builder(InternPool& pool)
      : pool_(pool) {}

    // -- Leaves --
    Index constant(Ref value, SourceLoc loc);
    Index arg(uint32_t position, Ref type, SourceLoc loc);
    Index signal(Signal sig, SourceLoc loc);
    // A function-frame local slot (lvalue leaf); `index` selects the slot, `type`
    // gives its storage width / domain.
    Index local(uint32_t index, Ref type, SourceLoc loc);

    // -- Named events (LRM 15.5) --
    // `-> e`: blocking trigger of the event whose synthetic signal is `sig`.
    Index event_new(SourceLoc loc);
    Index event_trigger(Index handle, SourceLoc loc);
    // `e.triggered`: 1-bit "triggered this time step" property of event `sig`.
    Index event_triggered(Index handle, SourceLoc loc);
    // `->> e`: nonblocking trigger of event `sig` (deferred to the NBA region).
    Index event_trigger_nb(Index handle, SourceLoc loc);
    // `->> #d e`: delayed nonblocking trigger; `delay` is the tick count.
    Index event_trigger_nb_delayed(Index handle, Index delay, SourceLoc loc);
    Index event_trigger_nb_event(Index handle, Signal sig, uint32_t edge, uint32_t count,
                                 SourceLoc loc);

    // -- Generic ops sharing a payload shape --
    // tag must be a unary-payload op (load, bnot, lnot, red_*, ret).
    Index unary(Inst::Tag tag, Index op, SourceLoc loc);
    // tag must be a binary-payload op (arithmetic, bitwise, shift, compare,
    // logical and/or, store, store_nb, bit_select).
    Index binary(Inst::Tag tag, Index l, Index r, SourceLoc loc);

    // Nonblocking intra-assignment delay `lvalue <= #delay value` (LRM 9.4.5):
    // sample `value` now, store it to `lvalue` after `delay` ticks.
    Index store_nb_delay(Index lvalue, Index value, Index delay, SourceLoc loc);

    // Inertial continuous-assignment delay `assign #delay lvalue = value` (LRM
    // 10.3.3): arm a detached epoch-stamped store of `value` for `now + delay`,
    // cancelling any pending store a change supersedes (pulse rejection).
    Index store_inertial_delay(Index lvalue, Index value, Index delay, SourceLoc loc);

    // Nonblocking intra-assignment event `lvalue <= repeat(count) @(edge trig) value`
    // (LRM 9.4.5): sample `value` and `count` now, store to `lvalue` after `count`
    // occurrences of `trig`'s `edge`. `count` is a constant 1 for a plain `@e`.
    Index store_nb_event(Index lvalue, Index value, Index count, Signal trig, uint32_t edge, SourceLoc loc);

    // -- Irregular shapes --
    Index resize(Index op, Ref type, SourceLoc loc);

    // Emit a float conversion (ItoF / FtoI / FtoITrunc / FBitcast / FResize)
    // of `op` to `type`.
    Index float_cvt(Inst::Tag tag, Index op, Ref type, SourceLoc loc);

    // Emit a real math system function call (LRM §20.8.2).
    Index real_math(RealMathOp op, std::span<const Index> args, SourceLoc loc);
    Index repl(Index value, uint32_t count, SourceLoc loc);
    // {<<slice{...}} block reversal of the built stream value `op` (LRM
    // 11.4.14.2). The result keeps op's width.
    Index stream_reverse(Index op, uint32_t slice, SourceLoc loc);
    // Resistive pull of a resolved tri0/tri1 net value: a still-Z bit becomes
    // `pull` (0 or 1), everything else is unchanged (LRM 6.6.4). Keeps op's width.
    Index net_pull(Index op, uint32_t pull, SourceLoc loc);
    // Drive-strength multi-driver resolution: fold `slots` (driver values) by
    // per-driver strength (`strengths`, packed strength0<<8 | strength1, parallel
    // to slots) into one 4-state net value. See Inst::Tag::StrengthRes.
    Index strength_res(std::span<const Index> slots, std::span<const uint16_t> strengths, SourceLoc loc);
    Index select(Index cond, Index a, Index b, SourceLoc loc);
    Index concat(std::span<const Index> parts, SourceLoc loc);
    Index part_select(Index base, uint32_t msb, uint32_t lsb, SourceLoc loc);
    // Runtime-offset, constant-width part-select base[offset +: width]. `offset` is
    // the already-lowered value of the low selected bit.
    Index dyn_part_select(Index base, Index offset, uint32_t width, SourceLoc loc);
    // Unpacked-array element select base[index]. `array_type` is the interned
    // UnpackedArray type being indexed (gives element stride + declared bounds).
    Index elem_select(Index base, Index index, Ref array_type, SourceLoc loc);
    // Constant-bounds slice of a fixed unpacked array. `slice_type` is the
    // slice's own UnpackedArray type; `first_pos` the 0-based storage position
    // of its first element in the base array. Yields a position-0-anchored
    // sub-array lvalue (see Inst::Tag::ArrSlice).
    Index arr_slice(Index base, Ref slice_type, uint32_t first_pos, SourceLoc loc);
    // Unpacked-struct member select base.field. `field_index` is the member's
    // declaration-order position; `struct_type` is the interned unpacked
    // Kind::Struct type being accessed (gives per-member layout).
    Index member_select(Index base, uint32_t field_index, Ref struct_type, SourceLoc loc);
    // Class `new` (LRM 8.7): allocate + power-on the object body of `class_type`
    // (a Kind::Class), yielding the object handle (a 2-state 64-bit value). The
    // constructor call is emitted separately.
    Index class_new(Ref class_type, SourceLoc loc);
    // Dereference a class object handle value to an aggregate lvalue rooted at the
    // object body. `handle` is the loaded handle value; `class_type` the Kind::Class
    // giving the body layout. A following member_select addresses a property.
    Index handle_deref(Index handle, Ref class_type, SourceLoc loc);
    // Virtual interface handle naming the elaborated interface instance whose
    // InstanceScope id is `scope` (LRM 25.9).
    Index iface_ref(uint32_t scope, SourceLoc loc);
    // Member of the interface instance `handle` names, as an lvalue. `rep` is any
    // real instance's copy of that member: it supplies the def-local field index,
    // the handle supplies the instance base.
    Index iface_signal(Index handle, Signal rep, SourceLoc loc);
    // `$cast` dynamic type test (LRM 8.16): 1 when the object `handle` refers to
    // is of `class_type` or of a class derived from it, else 0. A null handle
    // answers 1; the frontend emits this only for a cast-compatible pair.
    Index class_is_a(Index handle, Ref class_type, SourceLoc loc);
    // Whole-object blocking copy `dst = src` of two aggregate lvalues of `type`
    // (lowered to llvm.memcpy of the storage; caller ensures no string leaf).
    Index agg_copy(Index dst, Index src, Ref type, SourceLoc loc);
    // 1-bit 4-state equality of two whole unpacked-aggregate lvalues of `type`
    // (the backend expands the per-leaf compare + Land reduction).
    Index agg_eq(Index lhs, Index rhs, Ref type, SourceLoc loc);
    // Dynamic-array operations on the handle lvalue `arr` (LRM 7.5). new /
    // delete / copy are statements; size yields a 32-bit int. Each emits its
    // dedicated DynArray* tag. `array_type` is the interned DynArray type
    // (element stride / slot).
    // arr = new[size] [ (init) ]: reallocate; with `has_init`, copy from the
    // source handle lvalue `init`.
    Index dyn_array_new(Index arr, Index size, Index init, bool has_init, Ref array_type, SourceLoc loc);
    // arr.size(): current element count.
    Index dyn_array_size(Index arr, Ref array_type, SourceLoc loc);
    // arr.delete(): free storage, size -> 0.
    Index dyn_array_delete(Index arr, Ref array_type, SourceLoc loc);
    // dst = src: deep copy of another dynamic array (value semantics).
    Index dyn_array_copy(Index dst, Index src, Ref array_type, SourceLoc loc);
    // arr.reverse() (LRM 7.12.2): reverse element order in place.
    Index dyn_array_reverse(Index arr, Ref array_type, SourceLoc loc);
    // arr.sort() (`reversed` false) / arr.rsort() (`reversed` true): value order.
    Index dyn_array_sort(Index arr, bool reversed, Ref array_type, SourceLoc loc);
    // arr.shuffle() (LRM 7.12.2): randomize element order via the process RNG.
    Index dyn_array_shuffle(Index arr, Ref array_type, SourceLoc loc);
    // Queue operations on the handle lvalue `q` (LRM 7.10.2). size / pop* yield
    // a value; push* / insert / delete / copy are statements. Each emits its
    // dedicated Queue* tag. `queue_type` is the interned Queue type (element
    // stride / slot / bound).
    // q.size(): current element count (32-bit int).
    Index queue_size(Index q, Ref queue_type, SourceLoc loc);
    // q.push_back(e) / q.push_front(e): append the element value `elem`.
    Index queue_push(Index q, Index elem, bool front, Ref queue_type, SourceLoc loc);
    // q.pop_back() / q.pop_front(): remove and return the tail / head element.
    Index queue_pop(Index q, bool front, Ref queue_type, SourceLoc loc);
    // q.insert(idx, e): insert `elem` before position `idx`.
    Index queue_insert(Index q, Index idx, Index elem, Ref queue_type, SourceLoc loc);
    // q.delete(idx): remove the element at `idx`. q.delete(): remove all.
    Index queue_delete(Index q, Index idx, bool has_idx, Ref queue_type, SourceLoc loc);
    // dst = src: deep copy of another queue (value semantics).
    Index queue_copy(Index dst, Index src, Ref queue_type, SourceLoc loc);
    // dst <= src: nonblocking deep copy (staged in Active, moved into dst in the
    // NBA region).
    Index handle_copy_nb(Index dst, Index src, Ref queue_type, SourceLoc loc);
    // dst = src: deep copy of a dynamic array into a queue (src is a dynarray
    // handle lvalue).
    Index queue_copy_dynarr(Index dst, Index src, Ref queue_type, SourceLoc loc);
    // Append src[lo:hi] (inclusive, i64 values; full-queue append passes lo = 0,
    // hi = INT64_MAX) onto the dst handle — a queue slice / concatenation operand.
    Index queue_append_range(Index dst, Index src, Index lo, Index hi, Ref queue_type, SourceLoc loc);
    // q.reverse() (LRM 7.12.2): reverse element order in place.
    Index queue_reverse(Index q, Ref queue_type, SourceLoc loc);
    // q.sort() (`reversed` false) / q.rsort() (`reversed` true): value order.
    Index queue_sort(Index q, bool reversed, Ref queue_type, SourceLoc loc);
    // q.shuffle() (LRM 7.12.2): randomize element order via the process RNG.
    Index queue_shuffle(Index q, Ref queue_type, SourceLoc loc);
    // Associative-array operations on the handle lvalue `a` (LRM 7.8/7.9).
    // `assoc_type` is the interned Assoc type (value element + key type). See
    // docs/design/dynamic-arrays.md §4a/§6.
    // a[key] element select producing an lvalue: a Load calls rt_assoc_get_ptr
    // (absent -> element default, no create); a Store calls rt_assoc_put_ptr.
    Index assoc_select(Index a, Index key, Ref assoc_type, SourceLoc loc);
    // a.num() / a.size(): entry count (32-bit int).
    Index assoc_num(Index a, Ref assoc_type, SourceLoc loc);
    // a.exists(key): 32-bit int 0/1.
    Index assoc_exists(Index a, Index key, Ref assoc_type, SourceLoc loc);
    // a.delete(key): remove one key if present. a.delete(): remove all.
    Index assoc_delete(Index a, Index key, bool has_key, Ref assoc_type, SourceLoc loc);
    // The `'{default: v}` slot as an lvalue (LRM 7.9.11): a Store into it sets the
    // value a read of an absent key yields.
    Index assoc_default(Index a, Ref assoc_type, SourceLoc loc);
    // Whole-array replacement: drop every entry and the pattern default.
    Index assoc_reset(Index a, Ref assoc_type, SourceLoc loc);
    // dst = src: deep copy of another associative array (value semantics).
    Index assoc_copy(Index dst, Index src, Ref assoc_type, SourceLoc loc);
    // a.first(k)/last(k)/next(k)/prev(k) (LRM 7.9.3): ordered traversal. `key`
    // is the key-variable lvalue; returns 32-bit int 1 if a key was written,
    // 0 at the end. `which` selects first/last/next/prev.
    enum class AssocStep { First, Last, Next, Prev };
    Index assoc_traverse(Index a, Index key, AssocStep which, Ref assoc_type, SourceLoc loc);
    // `unlikely` hints that the condition is rarely true (e.g. a violation
    // check), so the backend biases branch weights toward the else path.
    Index cond(Index condition, ExtraSlice then_region, ExtraSlice else_region, SourceLoc loc, bool unlikely = false);
    // One arm of a `switch`: the constant case-value keys that select it and the
    // arm's already-closed body region. Several keys share one body for a grouped
    // case item. See Builder::switch_.
    struct SwitchArmBuild {
        std::span<const uint64_t> keys;
        ExtraSlice                body;
    };
    // Multiway branch for a constant, deduped, wildcard-free `case`. `selector`
    // is the value the backend packs into the match key; `arms` map key sets to
    // bodies; `default_region` runs on no match (pass an empty region when the
    // case had no default). See ir::SwitchInfo.
    Index switch_(Index                           selector,
                  std::span<const SwitchArmBuild> arms,
                  ExtraSlice                      default_region,
                  uint32_t                        sel_width,
                  uint32_t                        sel_four,
                  uint32_t                        pragmas,
                  SourceLoc                       loc);
    // Value-producing short-circuit conditional (&&/||). `pred` is a definite
    // 2-state bit; the then-branch (pred 1) yields `then_val`, the else-branch
    // yields `else_val`. The two values must share a width; the backend merges
    // them with a PHI. See ir::SelectLazy.
    Index select_lazy(Index      pred,
                      ExtraSlice then_region,
                      Index      then_val,
                      ExtraSlice else_region,
                      Index      else_val,
                      SourceLoc  loc);
    // for / while / forever / do-while. `cond_region` recomputes `pred` each
    // iteration; `step_region` is the per-iteration update (empty for
    // while / forever). `bottom_test` runs the body before the first test
    // (do-while).
    Index loop(ExtraSlice cond_region,
               Index      pred,
               ExtraSlice body_region,
               ExtraSlice step_region,
               bool       bottom_test,
               SourceLoc  loc);
    // break / continue: jump out of / to the next iteration of the innermost loop.
    Index break_(SourceLoc loc);
    Index continue_(SourceLoc loc);
    // A named sequential block some `disable` targets, and the two `disable`
    // forms (LRM 9.6.2). `disable` leaves the enclosing block carrying `label`,
    // resuming after it; `disable_remote` marks and force-wakes whatever
    // activation is inside the target region and carries on. `remote` on the
    // block means some `disable` names it from outside, so it needs the occupant
    // bookkeeping -- see ir::BlockInfo. `defer_flush` drops this process's
    // pending deferred reports (LRM 16.4.4).
    Index named_block(ExtraSlice body_region, uint32_t label, bool remote, SourceLoc loc);
    Index disable(uint32_t label, SourceLoc loc);
    Index disable_remote(uint32_t target, SourceLoc loc);
    Index defer_flush(uint32_t target, SourceLoc loc);
    Index disable_abort(SourceLoc loc);
    // Return from a void function / task / fork-child body (no operand).
    Index ret_void(SourceLoc loc);

    // -- String ops (unary/binary-shaped ones go through unary()/binary()) --
    // s[i] = c / s.putc(i, c): mutate the string lvalue `str` in place.
    Index str_putc(Index str, Index index, Index ch, SourceLoc loc);
    // s.substr(i, j): a new string value from the source string `str`.
    Index str_substr(Index str, Index i, Index j, SourceLoc loc);
    // One row of the enum_name member table: the member's value as its must-plane
    // bytes ((width+7)/8, little-endian; the same length for every row) and its
    // source name.
    struct EnumNameRow {
        std::span<const uint8_t> value;
        std::string_view         name;
    };
    // e.name(): a new string value naming the enum member whose value equals
    // `val`. `members` is the value/source-name table in declaration order; the
    // runtime byte-compares `val`'s must plane against each row. With
    // `fmt_fallback` set, a value matching no member renders as a decimal leaf
    // instead of "" — LRM 21.2.1.6's rule for `%p` of an enum, as against LRM
    // 6.19.2's "" for `.name()`.
    Index enum_name(Index val, std::span<const EnumNameRow> members, bool fmt_fallback, SourceLoc loc);
    // Guard a tagged-union member access against the union's current tag (LRM
    // 11.9). `tag` is the extracted tag value, `expected` the accessed member's
    // fieldIndex, `is_write` true for an assignment, `member` the accessed
    // member's name, and `names` every member name in tag order (so the report
    // can name the tag actually held). Yields no value: the access either falls
    // through or the run ends.
    Index tagged_check(Index                             tag,
                       uint32_t                          expected,
                       bool                              is_write,
                       std::string_view                  member,
                       std::span<const std::string_view> names,
                       SourceLoc                         loc);
    // One member of the tagged_fmt_p table: its source name, its own payload width
    // in bits (0 for a `void` member), and whether that payload is signed.
    struct TaggedFmtRow {
        std::string_view name;
        uint32_t         width;
        bool             is_signed;
    };
    // %p of a tagged union (LRM 21.2.1.6): a new string value holding
    // `tag:value` for whichever member `value`'s tag selects. `width` is the flat
    // value's total width and `tag_bits` its high discriminant bits.
    Index tagged_fmt_p(Index                          value,
                       uint32_t                       width,
                       uint32_t                       tag_bits,
                       std::span<const TaggedFmtRow>  members,
                       SourceLoc                      loc);
    // Packed cast of a string to target packed type `type`.
    Index str_to_packed(Index str, Ref type, SourceLoc loc);

    // $countbits / $countones / $onehot* / $isunknown building block: tally the
    // bits of `op` whose 4-state value is selected by `mask` (a CountBitsClass
    // OR-mask). Yields a definite 2-state 32-bit int.
    Index count_bits(Index op, uint32_t mask, SourceLoc loc);

    // `sel inside { ... }` (LRM 11.4.13): `values` are the singular members
    // (wildcard-equality tests), `ranges` back-to-back [lo, hi] operand pairs.
    // All must already be resized to the selector type. Yields a 1-bit 4-state
    // value.
    Index inside_set(Index sel, std::span<const Index> values, std::span<const Index> ranges, SourceLoc loc);

    // -- Timing --
    // Event wait `@(a or posedge b ...)`: suspend until ANY listed (signal, edge)
    // fires. Each WaitEdge carries an ir::Edge value as u32, keeping the builder
    // free of the Module-level enum. The list must be non-empty.
    Index wait(std::span<const WaitEdge> events, SourceLoc loc);

    // -- Calls --
    // Call function `func` with `args` (in formal-parameter order, each already
    // resized to its parameter type). The result is this instruction's value.
    Index call(FunctionId func, std::span<const Index> args, SourceLoc loc);

    // Dynamically-dispatched virtual method call (LRM 8.20). `func` is the
    // statically-resolved method (supplies the signature + result type); `slot` is
    // its vtable slot; `class_ref` the receiver's static class type; `args` is the
    // receiver handle followed by the formals (each resized to its parameter type).
    Index call_virtual(FunctionId func, uint32_t slot, Ref class_ref, std::span<const Index> args, SourceLoc loc);

    // Dynamically-dispatched virtual TASK call (LRM 8.20) — the task (coroutine)
    // ABI counterpart of call_virtual. Same operand shape.
    Index task_call_virtual(FunctionId func, uint32_t slot, Ref class_ref, std::span<const Index> args, SourceLoc loc);

    // Enable task `func`. `args` has one operand per formal in order: an rvalue for
    // an input formal, an lvalue leaf (a caller temp) for an output / inout formal,
    // and an aggregate lvalue for an unpacked struct / whole-array formal in any
    // direction (the callee copies it in / out through that one pointer).
    // A statement, never a value; the caller blocks until the task completes.
    Index task_call(FunctionId func, std::span<const Index> args, SourceLoc loc);

    // Fork `fork … join` (LRM 9.3.2): spawn each child body as a concurrent
    // coroutine, then join per `kind`. `children` are the fork-child task
    // FunctionIds. `captures` are the enclosing frame lvalues every child aliases
    // through an ArgDir::Ref formal, in formal order (empty for a fork in a
    // process, whose parent variables are module signals). A statement, never a
    // value.
    Index fork(std::span<const FunctionId> children,
               std::span<const Index>     captures,
               ForkKind                   kind,
               SourceLoc                  loc);

    // -- System tasks --
    // $display / $write: a literal format string, a newline flag (true for
    // $display), and the already-lowered value args its specifiers consume. The
    // backend writes to standard output.
    Index display(std::string_view fmt, bool newline, std::span<const Index> args, SourceLoc loc);
    // $fdisplay / $fwrite: like display, but `fd` selects the destination
    // descriptor (an MCD or FD value).
    Index fdisplay(Index fd, std::string_view fmt, bool newline, std::span<const Index> args, SourceLoc loc);
    // $strobe / $strobeb / $strobeo / $strobeh (LRM 21.2.2): register the
    // zero-formal deferred-print task `func` to run once in the Postponed
    // region of the current time step. A statement, never a value.
    Index strobe_pend(FunctionId func, SourceLoc loc);
    // Schedule `func`, a zero-formal action-block task, into the Reactive region
    // of the current time slot (LRM 4.4.2.5). Emitted from an Observed-region
    // body, which evaluates the property but may not run its pass/fail code
    // itself. A statement, never a value.
    Index reactive_pend(FunctionId func, SourceLoc loc);
    // `expect` (LRM 16.17): park the executing process in `monitor`'s waiter queue
    // until the attempt it just requested resolves, then resume it in the Reactive
    // region. The request itself is a Store the caller emits first. A statement.
    Index expect_wait(uint32_t monitor, SourceLoc loc);
    // The wake half: `resolved` high means `monitor`'s in-flight attempt reached a
    // verdict this cycle, so every waiter it holds goes onto the reactive ring.
    // Emitted into the monitor's own Observed process. A statement.
    Index expect_wake(Index resolved, uint32_t monitor, SourceLoc loc);
    // $monitor / $fmonitor (LRM 21.2.2): register `func` as the single active
    // monitor body (replacing any prior one), enabled, running each subsequent
    // time step in the Postponed region. A statement.
    Index monitor_set(FunctionId func, SourceLoc loc);
    // The change-detecting print inside a monitor body: render `fmt`+`args` and
    // emit to `fd` only if the rendered output changed since the last emit.
    Index monitor_emit(Index fd, std::string_view fmt, std::span<const Index> args, SourceLoc loc);
    // $monitoron (on=true) / $monitoroff (on=false): enable / disable the monitor.
    Index monitor_enable(bool on, SourceLoc loc);
    // Two-argument $fopen: open `name` with the C stdio mode string `mode`,
    // yielding a 32-bit FD.
    Index fopen_fd(Index name, Index mode, SourceLoc loc);
    // One-argument $fopen: open a fresh MCD on `name` for writing.
    Index fopen_mcd(Index name, SourceLoc loc);
    // $fclose: close the descriptor `handle`. No value.
    Index fclose(Index handle, SourceLoc loc);
    // $system: run `cmd` as a shell command. `has_arg` is 1 for $system(cmd),
    // 0 for the no-arg $system() (which execs the C system(NULL) probe); `cmd`
    // is ignored when 0. Yields the raw i32 system() return.
    Index system(Index cmd, uint32_t has_arg, SourceLoc loc);
    // $fseek(fd, offset, operation): seek the descriptor. Yields a 32-bit
    // integer (0 / -1). $fgetc / $feof / $ftell / $rewind ride the generic
    // unary() and $ungetc the generic binary() with their dedicated tags.
    Index fseek(Index fd, Index offset, Index operation, SourceLoc loc);
    // $fgets: read a line from descriptor `fd` into the packed-vector lvalue `dst`.
    // Yields the character count.
    Index fgets(Index dst, Index fd, SourceLoc loc);
    // $sformat / $swrite: format `args` per `fmt` and store the result into the
    // string lvalue `dst`. No value.
    Index sformat(Index dst, std::string_view fmt, std::span<const Index> args, SourceLoc loc);
    // $sformatf: format `args` per `fmt`. Yields a new string value.
    Index sformatf(std::string_view fmt, std::span<const Index> args, SourceLoc loc);
    // %p of a queue / dynamic array: render `'{e0, e1, ...}` over the runtime
    // element count. `handle` is the container lvalue, `container_type` the
    // interned Queue / DynArray type. Yields a new string value.
    Index container_fmt_p(Index handle, Ref container_type, SourceLoc loc);
    // $ferror: write descriptor `fd`'s error message into the string lvalue `dst`.
    // Yields the error code.
    Index ferror(Index dst, Index fd, SourceLoc loc);
    // $fscanf / $sscanf: parse `source` (an FD value, or a packed string value when
    // `is_string`) per `fmt` into the output lvalues `args`. Yields the conversion
    // count. `is_string` selects the Sscanf tag.
    Index scanf(Index source, bool is_string, std::string_view fmt, std::span<const Index> args, SourceLoc loc);
    // $fread(reg, fd): read raw bytes from `fd` into the packed-vector lvalue `dst`.
    // Yields the byte count.
    Index fread_word(Index dst, Index fd, SourceLoc loc);
    // $fread(mem, fd[, start[, count]]): read raw bytes from `fd` into the memory
    // signal `mem`. Yields the byte count.
    Index fread_mem(Index fd, Signal mem, Index start, Index count, bool has_start, bool has_count, SourceLoc loc);
    // $finish / $stop: end the simulation and exit. `banner` is the compile-time
    // prefix "<file>:<line>: <task>"; `arg` is the diagnostic level (0/1/2, LRM
    // Table 20-1). No SSA operands.
    Index finish(std::string_view banner, uint32_t arg, SourceLoc loc);
    // Unique/priority case violation report (LRM 12.5.3). `banner` is the
    // compile-time prefix "<file>:<line>: "; `kind` selects the message (0 = no
    // item matched, 1 = overlapping items). Non-fatal, warns and returns. No SSA
    // operands.
    Index case_violation(std::string_view banner, uint32_t kind, SourceLoc loc);
    // Severity report ($info / $warning / $error / $fatal, LRM 20.10, or an
    // immediate-assertion default failure, LRM 16.3). `level` is the ir::Severity
    // label; `banner` the compile-time prefix "<file>:<line>: "; `fmt` + `args`
    // the $display-style message (both empty for a bare `$error;`). Non-fatal,
    // renders and returns; $fatal's termination is a following finish().
    Index severity(uint32_t               level,
                   std::string_view       banner,
                   std::string_view       fmt,
                   std::span<const Index> args,
                   SourceLoc              loc,
                   uint32_t               defer = 0);
    // $fflush: flush the descriptor `handle`'s output buffers. The no-argument
    // form passes a constant 0 (the runtime flushes every open stream).
    Index fflush(Index handle, SourceLoc loc);
    // $readmemh / $readmemb: load a file into the memory signal `target`. `filename`
    // is the path value; `start` / `finish` are optional address bounds (pass any
    // Index when the matching has_* is false). `hex` selects hex vs binary.
    Index read_mem(Signal    target,
                   Index     filename,
                   Index     start,
                   Index     finish,
                   bool      hex,
                   bool      has_start,
                   bool      has_finish,
                   SourceLoc loc);
    // $timeformat: set the %t format. `units`, `precision`, and `min_width` are
    // already-lowered value operands; `suffix` is a literal string appended after
    // the formatted number.
    Index timeformat(Index units, Index precision, std::string_view suffix, Index min_width, SourceLoc loc);

    // -- Value-change dump (VCD; see docs/VCD.md) --
    // $dumpfile: set the output path to the `filename` value.
    Index dump_file(Index filename, SourceLoc loc);
    // $dumpvars (whole-design form): begin dumping every dumpable signal. The
    // backend supplies the signal set; this node carries no operands.
    Index dump_vars(SourceLoc loc);
    // $dumplimit: stop dumping once the file reaches `bytes` bytes.
    Index dump_limit(Index bytes, SourceLoc loc);
    // $dumpoff / $dumpon / $dumpall / $dumpflush. `tag` selects the control; it
    // must be one of the Dump* control tags.
    Index dump_control(Inst::Tag tag, SourceLoc loc);

    // -- System functions --
    // $time / $stime: read the simulation clock. Yields a 64-bit unsigned value
    // rounded down by `scale` to the calling module's time unit (1 == unitless).
    Index sim_time(uint64_t scale, SourceLoc loc);
    // $random: a 32-bit signed PRNG draw. No operands.
    Index random(SourceLoc loc);
    // A distribution function (LRM §20.15.2) or seeded $random (§20.15.1): the
    // DistOp plus its value operands (the seed word, then the 1-2 parameters —
    // the count selects the DistUnary / DistBinary tag). Yields an i64 with the
    // updated seed packed in [32,64) and the signed draw in [0,32).
    Index dist_call(DistOp op, std::span<const Index> args, SourceLoc loc);
    // $urandom (LRM §18.13.1): a 32-bit unsigned per-thread xoshiro256++ draw.
    // No operands; reads/advances the calling thread's RNG, which the backend
    // supplies (a process's frame state, or a subroutine's caller's).
    Index urandom(SourceLoc loc);
    // $urandom(seed) (LRM §18.13.1): reseed that RNG from `seed`, then draw.
    Index urandom_seeded(Index seed, SourceLoc loc);
    // $urandom_range(maxval, minval) (LRM §18.13.2): one draw mapped into
    // [min, max] (the backend swaps when min > max).
    Index urandom_range(Index maxval, Index minval, SourceLoc loc);
    // The object RNG of LRM §18.14.2. Its state is four 64-bit words held IN the
    // object rather than in the calling thread's frame, so `rng_lv` is an lvalue of
    // a 4-element unpacked array of `bit [63:0]`: the caller selects the field and
    // neither of the next two ops needs to know the class layout.
    //
    // One xoshiro256++ step of that state, yielding its high 32 bits as a 32-bit
    // unsigned value -- $urandom's draw, on the object's own stream.
    Index obj_random(Index rng_lv, SourceLoc loc);
    // Reseed that state from the 64-bit value `seed`, splitmix64-expanded exactly as
    // a process's own reseed is. Yields no value; `obj.srandom(seed)` and the seeding
    // of a freshly constructed object both lower to this.
    Index obj_rand_seed(Index rng_lv, Index seed, SourceLoc loc);
    // One cyclic-randomization pick for a `randc` property (LRM §18.6): draw
    // uniformly among the values of [0, nvals) whose bit is still clear in `seen_lv`
    // -- an lvalue of a 4-element unpacked array of `bit [63:0]`, that property's
    // 256-bit used-this-cycle bitmap -- set that bit, and yield the value. An
    // exhausted bitmap is cleared first, starting the next cycle, so every cycle is a
    // permutation. `nvals` is `1 << width` with width <= 8.
    Index randc_next(Index seen_lv, Index rng_lv, uint32_t nvals, SourceLoc loc);
    // $test$plusargs(str): nonzero iff a command-line plusarg's prefix matches
    // `str`. No SSA operands.
    Index test_plusargs(std::string_view str, SourceLoc loc);
    // $value$plusargs(user_string, var): on a matching plusarg, convert its
    // remainder per `conv` ('d'/'o'/'b'/'h'/'s') into the output lvalue `dst`
    // and yield nonzero; otherwise leave `dst` untouched and yield 0. `prefix`
    // is the plusarg_string portion of user_string.
    Index value_plusargs(Index dst, std::string_view prefix, char conv, SourceLoc loc);

    // -- Regions --
    // Begin a nested region; subsequent emits accumulate into it until closed.
    void open_region();
    // End the current region and return its instruction slice.
    ExtraSlice close_region();

    // -- Partial-emission rollback --
    // A record of every growable column's length, for discarding a self-contained
    // run of emission that turned out to be unusable.
    //
    // This exists for error recovery in the frontend. A lowering guard throws from
    // arbitrary depth, and most of the time that abandons a Builder local to one
    // process, so the partial emission dies with it. But declaration and static
    // class property initializers all share ONE pre-init Builder across every
    // member of a scope, so a guard tripping inside one would otherwise leave its
    // half-emitted instructions -- and, worse, any region it opened -- in a
    // builder that later members keep writing to. `finish()` treats whatever
    // remains in `scratch_` as the root region, so an unclosed region is not a
    // crash: it silently absorbs its contents into the body. Rolling back is what
    // keeps that from happening.
    struct Mark {
        uint32_t insts;
        uint32_t extra;
        uint32_t scratch;
        uint32_t regions;
    };

    [[nodiscard]] Mark mark() const {
        return { static_cast<uint32_t>(tags_.size()),
                 static_cast<uint32_t>(extra_.size()),
                 static_cast<uint32_t>(scratch_.size()),
                 static_cast<uint32_t>(region_starts_.size()) };
    }

    // Discard everything emitted since `m`. Sound only when the discarded run is
    // self-contained -- no instruction at or after `m.insts` may be referenced
    // from before it, which holds for an abandoned member because its result was
    // never handed to anything earlier. Callers outside that situation have no
    // business here.
    void rollback(Mark m);

    // Record the exclusive end of the source construct `i` came from, so a
    // diagnostic can underline it rather than point at its first character.
    // Sparse — see Ir::end. Recording one for an instruction that is shared
    // (a folded constant) only widens an underline, never misdirects it.
    void set_end(Index i, SourceLoc end) {
        ends_.emplace(static_cast<uint32_t>(i), end);
    }

    // Freeze the built state into an Ir, sealing the root region. The builder is
    // spent afterwards.
    Ir finish();

private:
    // Shared body of display / fdisplay: pack the format bytes, the value-arg
    // slice, and the DisplayInfo record into the extra buffer.
    ExtraIndex display_info(std::string_view fmt, bool newline, std::span<const Index> args);

    Index emit(const Inst& inst, SourceLoc loc) {
        const auto index = static_cast<uint32_t>(tags_.size());
        tags_.push_back(inst.tag);
        payloads_.push_back(inst.payload);
        locs_.push_back(loc);
        scratch_.push_back(static_cast<Index>(index));

        return static_cast<Index>(index);
    }

    // Append a struct to the extra buffer; returns its offset handle.
    template<ExtraPayload T>
    ExtraIndex add_extra(const T& value) {
        constexpr size_t n     = sizeof(T) / sizeof(uint32_t);
        const auto       words = std::bit_cast<std::array<uint32_t, n>>(value);
        const auto       start = static_cast<uint32_t>(extra_.size());
        extra_.insert(extra_.end(), words.begin(), words.end());

        return static_cast<ExtraIndex>(start);
    }

    // Append raw bytes to the extra buffer, packed little-endian 4 chars per
    // word; returns the offset of the first word. Decode with Ir::extra_bytes.
    ExtraIndex add_bytes(std::string_view s) {
        const auto start = static_cast<uint32_t>(extra_.size());
        for (size_t i = 0; i < s.size(); i += 4) {
            uint32_t word = 0;
            for (size_t k = 0; k < 4 && i + k < s.size(); ++k) {
                word |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + k])) << (8 * k);
            }
            extra_.push_back(word);
        }

        return static_cast<ExtraIndex>(start);
    }

    // Append a run of instruction indices to the extra buffer; returns the slice.
    ExtraSlice add_slice(std::span<const Index> indices) {
        const auto start = static_cast<uint32_t>(extra_.size());
        for (Index i : indices) {
            extra_.push_back(static_cast<uint32_t>(i));
        }
        const auto end = static_cast<uint32_t>(extra_.size());

        return { static_cast<ExtraIndex>(start), static_cast<ExtraIndex>(end) };
    }

    // Append a run of raw u32 words (e.g. packed strength constants) to the extra
    // buffer; returns the slice. Unlike add_slice these are not SSA operands.
    ExtraSlice add_u32_slice(std::span<const uint32_t> words) {
        const auto start = static_cast<uint32_t>(extra_.size());
        for (uint32_t w : words) {
            extra_.push_back(w);
        }
        const auto end = static_cast<uint32_t>(extra_.size());

        return { static_cast<ExtraIndex>(start), static_cast<ExtraIndex>(end) };
    }

    // Intern pool backing constant / type references.
    InternPool& pool_;
    // Growable SoA instruction columns under construction.
    std::vector<Inst::Tag>     tags_;
    std::vector<Inst::Payload> payloads_;
    std::vector<SourceLoc>     locs_;
    // Sparse construct ends, keyed by instruction index; see set_end.
    ankerl::unordered_dense::map<uint32_t, SourceLoc> ends_;
    // Flat u32 side-table under construction.
    std::vector<uint32_t> extra_;
    // Instruction indices of all open regions, innermost segment last.
    std::vector<Index> scratch_;
    // Start offset into scratch_ of each open nested region.
    std::vector<size_t> region_starts_;
};

} // namespace clotho::ir
