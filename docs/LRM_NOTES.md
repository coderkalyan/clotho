# LRM notes

Where IEEE 1800-2023 is ambiguous, contradicts itself, or Clotho deliberately departs from it, the decision is listed here and recorded in full at the code that implements it. Each entry names the clause, states Clotho's reading, and points to where the reasoning lives.

## 1. Integer-to-string conversion of a negative value (6.16.11-6.16.14)

The clauses do not say how `itoa`, `hextoa`, `octtoa` and `bintoa` render a
negative argument. Clotho writes `-` followed by the magnitude in the method's
base, so `hextoa(-256)` is `-100`, and never the two's-complement bit pattern.
An x or z bit of the argument reads as 0.

Code: `src/runtime/Strings.cpp` (`str_toa_base`). Tests: `tests/string_test.cpp` ("itoa family formats sign + magnitude per LRM 6.16.11-6.16.14").

## 2. `realtoa` precision (6.16.15)

The clause names no precision or format. Clotho prints at full round-trip
precision (`std::to_chars` with no precision argument) rather than at a fixed
number of significant digits.

Code: `src/runtime/Strings.cpp` (`rt_string_realtoa`). Tests: `tests/string_test.cpp` ("realtoa formats at full round-trip precision").

## 3. Unpacked union layout (7.3)

7.3 fixes a bit-level overlay only for packed unions. Clotho gives an unpacked
union one flat value as wide as its widest member, with every member overlaying
the least significant bits: a member select is a low part-select, and a write to
a narrower member leaves the bits above it untouched. Members must be integral.

Code: `src/lower/Types.cpp` (`lower_type`), `src/lower/SystemTasks.cpp` (`expand_p_packed`). Tests: `tests/struct_test.cpp` ("unpacked union overlays members at the LSB (flat value, LRM 7.3)", "unpacked union: whole-union copy and nesting in an unpacked struct").

## 4. Reading an element that does not exist (7.4.5, 7.10.2, Table 7-1)

An out-of-range or x/z-indexed array read, and `pop_front` or `pop_back` on an
empty queue, yield the value Table 7-1 gives the element type: 0 for a 2-state
element and x for a 4-state one. A 2-state element never reads as x.

Code: `src/ir/llvm/StateLayout.cpp` (`slot_default_const`). Tests: `tests/memory_bounds_test.cpp` ("out-of-range read from a fixed-size unpacked array yields the element default", "an unknown (x) index into a fixed-size unpacked array is out of range"), `tests/queue_test.cpp` ("queue pop from empty 2-state queue is default 0", "queue 4-state defaults to x").

## 5. Out-of-range positions of an indexed array slice (7.4.5)

`a[i +: n]` and `a[i -: n]` expand position by position, and each position
follows the invalid-index rule on its own, as 11.5.1 does for a packed
part-select: a position outside the array reads the element default and drops
its write, while in-range positions are unaffected. An x or z bit in `i` makes
every position invalid, so the read is all default and the write does nothing.

Code: `src/lower/Expr.cpp` (`emit_indexed_slice_copy`, `materialize_indexed_slice`). Tests: `tests/assign_pattern_test.cpp` ("an out-of-range indexed slice reads the element default per position", "an x/z indexed slice base is an invalid index (LRM 7.4.5)").

## 6. Overflowing a bounded queue (7.10.4)

7.10.4 does not say which end an overflowing push gives up. Clotho always keeps
the first `bound + 1` elements, so `push_front` drops the old tail, `push_back`
onto a full queue has no effect, and `insert` follows the same rule.

Code: `src/runtime/Containers.cpp` (`q_apply_bound_ops`). Tests: `tests/queue_test.cpp` ("queue bounded push drops excess", "queue slice/concat into a bounded queue drops the tail").

## 7. Unknown elements in `min` and `max` (7.12.1)

7.12.1 does not say where an element with unknown bits ranks. The first element
seeds the result and a later one replaces it only when its comparison with the
result is a definite 1, so a later element with unknown bits never replaces the
result, and a tie keeps the earlier element.

Code: `src/lower/Containers.cpp` (`lower_array_iter_method`). Tests: `tests/array_locator_test.cpp` ("array min/max over 4-state elements skips unknowns").

## 8. Reduction of an empty array (7.12.3)

7.12.3 gives no result for an empty array. Every reduction method (`sum`,
`product`, `and`, `or`, `xor`) of an empty array returns 0, not the operator's
identity, so `product` and `and` give 0 as well.

Code: `src/lower/Containers.cpp` (`lower_array_iter_method`). Tests: `tests/array_locator_test.cpp` ("array reduction of an empty array is zero").

## 9. Inertial continuous-assignment delay (10.3.3)

A delayed continuous assignment updates its net in the Active region, not in the
NBA region where a procedural `<= #d` commits. Clotho commits the delayed value
when the timing wheel reaches that time step, before any Active-region process
runs, so every process in that time step reads the new value.

Code: `src/ir/llvm/Backend.h` (`inertial_sigs_`), `src/ir/llvm/Scheduler.cpp` (`emit_active_run`). Tests: `tests/continuous_assign_delay_test.cpp` ("inertial CA delay: pulse rejection and no-restart on same-value change").

## 10. Re-forcing a forced variable (10.6.2)

A second `force` on a variable supersedes the first, so a later change to the
first force's right-hand side does not reach the variable. Each textual force
site has its own id, and only the site whose id is current drives the variable.

Code: `src/lower/Lowerer.h` (`ForceInfo`). Tests: `tests/force_release_test.cpp` ("force: re-force is last-wins, old RHS link broken"). Design: `docs/design/force-release.md` (Model).

## 11. Keyed assignment pattern over a descending packed array (10.9)

In `logic [3:0][7:0]` key 0 names the least significant byte, so `'{0:x, 3:y}`
puts `y` at the most significant end. slang orders a keyed pattern's elements by
ascending index and puts the lowest at the most significant end, which is wrong
for a descending array, so Clotho reverses the element order there. Positional
and replicated patterns stay in source order.

Code: `src/lower/Assign.cpp` (`lower_packed_pattern`). Tests: `tests/assign_pattern_test.cpp` ("packed array pattern: keyed, descending reverses (slang-bug workaround)").

## 12. Assignment-pattern lvalue member widths (10.9)

10.9 requires each member of an assignment-pattern lvalue to have the same number
of bits as its right-hand element. Deviation: Clotho does not enforce that, and a
mismatched member truncates or extends exactly as the plain assignment the
pattern stands for. Not gated.

Code: `src/lower/Assign.cpp` (`store_pattern_elem`). Tests: `tests/assign_pattern_test.cpp` ("assignment-pattern lvalue: per-element width truncates like an ordinary assignment").

## 13. Equality of unpacked aggregates with unknown members (11.2.2, 11.4.5)

`==` and `!=` on unpacked structs, unpacked arrays and queues fold the
per-member logical equality: a definite mismatch in any member gives 0,
otherwise an unknown member gives x, otherwise 1. Queues of different sizes
compare as a definite 0.

Code: `src/ir/Ir.h` (`AggEq`), `src/ir/llvm/Ops.cpp` (`lower_agg_eq`). Tests: `tests/struct_test.cpp` ("unpacked struct equality is field-wise; X makes it ambiguous (LRM 11.4.5)"), `tests/assign_pattern_test.cpp` ("whole unpacked-array equality is per-element, X-propagating"), `tests/queue_test.cpp` ("queue equality with x-propagation").

## 14. Short-circuit of `->` (11.3.5)

11.3.5 lists `->` with the short-circuit operators, so `a -> b` skips evaluating
`b` when `a` is a definite 0. `<->` is not on that list and 11.4.7 evaluates each
of its operands exactly once, so its right operand always runs.

Code: `src/lower/Expr.cpp` (`lower_short_circuit`). Tests: `tests/function_test.cpp` ("-> short-circuits but <-> always evaluates its right operand").

## 15. `0 ** <negative>` in a 2-state result (11.4.3, 6.3.1)

Table 11-4 gives x for `0 ** n` with a negative `n`. A 2-state result converts
that x to 0 as 6.3.1 does for any 4-state value, and a 4-state result keeps the
x.

Code: `src/ir/llvm/Ops.cpp` (`lower_pow`). Tests: `tests/llvm_backend_test.cpp` ("0 ** negative coerces X to 0 in a 2-state result").

## 16. Nested streaming example (11.4.14.2)

The clause lists `{<<2{ {<<4{4'b1101}} }}` as generating `'b1110`, but its own
streaming rules give `'b0111`. Clotho follows the rules and treats the example
as a typo.

Tests: `tests/streaming_test.cpp` ("stream << nested").

## 17. Strings in conditions and logical operators (12.4, Table 11-1)

The LRM does not define the truth of a string. The condition of an `if`,
`while`, `for` or `?:` is true when the string is non-empty. `!` on a string is
rejected, because Table 11-1 allows logical operators only on integral and real
operands.

Code: `src/lower/Statements.cpp` (`lower_predicate`), `src/lower/Expr.cpp` (`lower_unary`). Tests: `tests/string_test.cpp` ("if on a string tests emptiness", "ternary with a string condition selects by emptiness", "logical negation of a string is rejected").

## 18. `unique` and `priority` violation reports (12.4.2, 12.5.3)

12.4.2 and 12.5.3 defer a violation report to the Observed region, and 12.4.2.1
clears a pending report when its process reaches a flush point first. Clotho
queues the report and issues it in the Observed region. Deviation: it does not
clear pending reports at flush points, so a statement that violates on several
settle passes of one time step reports once per pass. Not gated; the gap is
listed in `docs/TODO.md`.

Code: `src/ir/llvm/SystemTasks.cpp` (`lower_case_viol`), `src/runtime/Reports.cpp` (`rt_observed_case_violation`, `rt_observed_flush`). Tests: `tests/case_test.cpp` ("priority case: Observed-region report is stamped with the settled instant"). Design: `docs/design/x-semantics.md` (Free wins needing no new semantics).

## 19. `case` with a real selector (12.5)

12.5 words case matching as a bit-for-bit `===`, which Table 11-1 does not allow
on a real operand. A real has no x or z bit to tell `===` from `==`, so Clotho
matches a real selector with each label by IEEE-754 equality: `-0.0` matches
`0.0` and a NaN matches no label. slang rejects `casez` and `casex` over a real
selector, and Clotho keeps that rejection.

Code: `src/lower/Statements.cpp` (`real_case_label_pred`). Tests: `tests/case_test.cpp` ("case with a real selector compares by value", "a real case stops the search at the first match"), `tests/real_test.cpp` ("casez / casex over a real is rejected").

## 20. Dynamic arrays, queues and associative arrays as inputs (13.5)

These containers are values, so an `input` formal of one of their types gets a
deep copy at the call, and the callee's writes, resizes and deletes never reach
the caller's container.

Code: `src/ir/llvm/Subroutines.cpp` (`emit_copyin`), `src/ir/llvm/Containers.cpp` (`emit_handle_assign`). Tests: `tests/dynamic_array_test.cpp` ("dynamic array input argument is copy-in (value semantics)"), `tests/queue_test.cpp` ("queue input argument is copy-in (value semantics)"), `tests/associative_array_test.cpp` ("assoc input argument is copy-in").

## 21. Immediate `cover` (16.3)

An immediate `cover` runs its pass action when its expression is true and does
nothing when it is false. Deviation: Clotho keeps no hit count and prints no
coverage report for it. Not gated.

Code: `src/lower/Assertions.cpp` (`lower_immediate_assertion`). Tests: `tests/severity_test.cpp` ("immediate cover runs the pass action when true", "immediate cover does nothing when false: no pass action, no report"). Design: `docs/design/functional-coverage.md` (The precedent that is already silently wrong).

## 22. `disable` of a procedure's outermost block (16.4.4)

Disabling the outermost scope of a procedure drops its pending deferred
assertion reports, and they are never reported. The `disable` bumps the
procedure's report generation exactly as an activation start does, which makes
every queued report stale.

Code: `src/ir/Ir.h` (`DeferFlush`). Tests: `tests/loop_test.cpp` ("disable of a procedure's outermost block drops its deferred reports"). Design: `docs/design/disable.md` (Assertion interaction (16.4.4 / 16.14.6.4)).

## 23. One verdict per assertion attempt (16.12.1)

The LRM does not say whether an attempt whose antecedent matches more than once
owes one verdict or one per obligation. Clotho gives the attempt one: it fails at
its first failing obligation and passes when all of its obligations have passed,
and `cover property` counts attempts, not obligations. A monitor on the counted
discipline still reports per obligation, because a counted obligation has no
bounded lifetime to carry an attempt id through.

Code: `src/lower/Lowerer.cpp` (`attempt_ring_size`). Tests: `tests/sva_test.cpp` ("a twice-matching attempt reports only its first failing obligation", "a cover of an implication counts a twice-matching attempt once"), `tests/sva/diff_attemptverdict.sv`, `tests/sva/diff_multimatch.sv`. Design: `docs/design/concurrent-assertions.md` (What this document does not establish).

## 24. Attempts that share an obligation (16.14.1)

16.14.1 evaluates the consequent once per antecedent match, so attempts whose
matches end in the same cycle share one obligation and are each still owed a
verdict. Clotho tells them apart with attempt ids, or with per-state counts on
the counted discipline, and reports each. An antecedent with no bounded longest
match is refused rather than under-reported, and an abort over a multi-cycle
antecedent resolves every attempt in flight.

Code: `src/lower/Lowerer.cpp` (`attempt_ring_size`), `src/lower/SvaMonitor.cpp` (`emit_counted_step`), `src/Sva.cpp` (`abort_on`). Tests: `tests/sva_test.cpp` ("many attempts resolving in one cycle are reported once each", "goto and non-consecutive repetition in an antecedent are refused", "an abort over a multi-cycle antecedent resolves every attempt in flight"). Design: `docs/design/sva-property-layer.md` (Why Tier B needs a different discipline, and what it is).

## 25. `$urandom` generator (18.13)

18.13 requires a deterministic, per-thread stable generator but leaves the
algorithm open. Each process and spawned child owns a xoshiro256++ state, seeded
through splitmix64 from its process index, a draw of its parent's stream, or the
`$urandom(seed)` argument. A function or task draws from its caller's state, and
`shuffle` draws from the process's stream.

Code: `src/runtime/Random.cpp` (`rt_urandom_seed`). Tests: `tests/sysfunc_test.cpp` ("$urandom is a deterministic per-process stream (pinned)", "two processes each get an independent $urandom stream"), `tests/array_ordering_test.cpp` ("dynamic array + queue shuffle is a stable permutation").

## 26. `randcase` weight sum (18.16)

18.16 forms the weight sum with unsigned addition at the widest weight's
precision. Clotho applies that literally, so a sum that does not fit wraps
(`8'd200 + 8'd100` is 44 in 8 bits, and the first branch always wins), and an
all-zero sum takes no branch.

Code: `src/lower/Random.cpp` (`lower_randcase`). Tests: `tests/randcase_test.cpp` ("randcase sum precision is the widest weight (LRM 18.16)", "randcase sum wraps at the weight precision (LRM 18.16)").

## 27. `$bits` of a string (20.6.2)

`$bits` of a `string` is evaluated at run time as 8 bits per current character,
so the empty string gives 0.

Code: `src/lower/SystemTasks.cpp` (`lower_bit_query`). Tests: `tests/sysfunc_test.cpp` ("$bits of a dynamic string is 8 per character").

## 28. Severity report layout (20.10)

The LRM requires a severity report to name the file and line of the call, the
hierarchical name of the scope the call is made in, and the simulation time, but
fixes neither their order nor the words around them. `$info`, `$warning`,
`$error`, `$fatal` and the default failure report of an assertion (16.3, 16.14,
16.17) each write a single line,
`<LABEL>: <file>:<line>: <scope> at <time>[: <message>]`, to stderr after
flushing stdout. `<scope>` is what `%m` prints at the same place: the instance
path, then the enclosing task, function, named block or assertion label (23.6).
`<time>` is written as the `$finish` banner writes it, and `: <message>` is left
out when the call has none. The default failure of an assertion is a `$error`
made in the assertion's own scope, so a labelled `ap: assert ...` reports
`top.ap`; iverilog reports `top` for it.

`$fatal` "terminates the simulation with an error code" (20.10): the run ends
with exit status 1, after its `final` blocks, whatever its finish number.
`$finish` of any level, `$error`, `$warning` and a failed assertion leave the
status 0.

Code: `src/runtime/Reports.cpp` (`severity_line`, `rt_severity`), `src/lower/Lowerer.cpp` (`severity_banner`). Tests: `tests/severity_test.cpp` ("severity tasks render label + formatted message to stderr", "a labelled immediate assertion's report names the label"), `tests/exit_status_test.cpp` ("exit status: $fatal ends the run with status 1, whatever its finish number").

## 29. `$system` (20.17.1)

Deviation: `$system` runs a shell command, so Clotho refuses it at compile time by
default. `--allow-system const` admits a command that folds to a constant at
elaboration and `--allow-system full` admits any command. The function form
returns the `system()` value unchanged, so its encoding is the C library's.

Code: `src/lower/SystemTasks.cpp` (`lower_system_call`). Tests: `tests/system_test.cpp` ("$system is refused at compile time without --allow-system", "$system const tier requires a constant command string", "$system function form returns the raw system() status"). Design: `docs/design/cli.md` (Options).

## 30. `-` flag and leading `0` in a format specification (21.2.1.2)

Specifiers parse as `% [-] [width] [. precision] conv`. Deviation: `-`
left-justifies the field with trailing spaces, although the LRM defines no such
flag. Not gated. A leading `0` belongs to the width (`%0d` is the minimal form),
so the integer conversions have no zero-pad flag, while the real conversions read
it as C's zero-pad flag.

Code: `src/runtime/Format.h` (`parse_format_spec`, `FormatSpec`), `src/runtime/Format.cpp` (`render_arg`). Tests: `tests/display_test.cpp` ("left justification").

## 31. Explicit field width on `%h`, `%o` and `%b` (21.2.1.2)

In the clause's examples, `%3h` of `32'h5` prints `005`. A width given in the
format overrides the automatic sizing: Clotho reduces the value to its minimal digits, zero-pads it
to the width, and never truncates.

Code: `src/runtime/Format.cpp` (`render_arg`). Tests: `tests/display_test.cpp` ("explicit field widths pad and expand per LRM 21.2.1.2", "display operands wider than 64 bits (LRM 21.2)").

## 32. `%p` output (21.2.1.6)

An integral leaf prints as unformatted decimal, and the members of an unpacked
struct print by name, as in `'{a:171, b:42}`. White space between elements is
the implementation's, and Clotho separates them with `, `. A tagged union whose
tag is x, z or names no member has no valid member, so its whole value prints as
one decimal leaf.

Code: `src/lower/SystemTasks.cpp` (`expand_p_lvalue`, `expand_p_packed`), `src/runtime/Containers.cpp` (`rt_tagged_format_p`). Tests: `tests/display_test.cpp` ("%p renders unpacked aggregates as assignment patterns (LRM 21.2.1.6)"), `tests/struct_test.cpp` ("whole unpacked struct as a $display %p operand renders a pattern", "%p of a tagged union whose tag is x falls back to the flat value"). Design: `docs/design/tagged-unions.md` (`%p` (LRM 21.2.1.6)).

## 33. Leading NUL bytes under `%s` (21.2.1.7)

A packed `%s` operand drops its leading 0x00 bytes, as the clause's text and the
21.2.1.1 example require. A 0x00 byte after the first non-zero byte prints as a space.

Code: `src/runtime/Format.cpp` (`render_arg`). Tests: `tests/display_test.cpp` ("%s drops leading NUL bytes of a packed operand").

## 34. `$fread` reaching end of file inside an element (21.3.4)

The clause does not say what the unread bytes of a partly read element hold.
Clotho packs the bytes it read into the high end of the element and zero-fills
the rest; the return value counts the bytes read.

Code: `src/runtime/Io.cpp` (`rt_fread`). Tests: `tests/llvm_backend_test.cpp` ("$fread into a memory with elements wider than 64 bits").

## 35. Program implicit `$finish` with woken work pending (24.3)

The LRM does not say whether work that the last program `initial` woke, such as
an `always` block sensitive to a variable it stored, runs before the implicit
`$finish`. Clotho stops at once and empties the run queues exactly as an explicit
`$finish` does, so that work does not run.

Code: `src/ir/llvm/Scheduler.cpp` (`emit_program_end`), `src/ir/llvm/SystemTasks.cpp` (`lower_finish`). Tests: `tests/program_test.cpp` ("implicit finish does not drain work the last program initial woke").

## 36. Re-export through `export p::*` (26.4)

A package's own imports are not visible through a further import of that package
unless it exports them. `export p::*` re-exports a name when the package's own
body also references that name somewhere. Clotho takes this resolution from
slang as it is, and the fully general case has no independent confirmation.

Tests: `tests/package_test.cpp` ("package-internal import does not leak without export", "export re-export via a further import").

## 37. Weak L and H results of three-state gates and MOS switches (28.6, 28.7)

Tables 28-5 and 28-6 give L (0 or z) or H (1 or z) for an x or z control.
Deviation: Clotho has no strength model, so these results are x. Not gated. A MOS
switch still passes a z data bit through as z.

Code: `src/lower/Nets.cpp` (`gate_tristate_value`). Tests: `tests/gate_test.cpp` ("three-state gate truth tables match LRM table 28-5, including X/Z", "MOS switch truth tables match LRM table 28-6, including the z pass-through").

## 38. MOS switch strength (28.13, 28.14)

Deviation: Clotho has no strength on a net's value, so a MOS switch drives strong
whatever its data input carries (28.13), and `rnmos`, `rpmos` and `rcmos` do not
reduce it (28.14, Table 28-8). That is exact when nothing the strength could be
compared with is there, so it is gated: `check_mos_switch_strength` propagates the
strength levels a switch can output (a net's levels are any level when a driver of
it has an explicit weak or pull strength, it is a pullup / pulldown target or a
tri0 / tri1 net, otherwise strong plus what the switches driving it output) and
refuses a switch whose output net is resolved against another driver or a weak pull
unless the strengths order as strong does: every switch on the net outputs exactly
strong, or the switches are the net's only drivers and output one single level, or
the switch is its net's only driver. A data input that is not a net, or a net
reached through an input or output port, is strong: Clotho models those port
connections as continuous assignments (4.9.6, 23.3.3), which reset the strength;
a simulator that collapses the nets (23.3.3.7) keeps it, and so can differ. An
inout port aliases the net, so its strength is kept.

Code: `src/lower/Nets.cpp` (`mos_switch_of`, `lower_cmos_switch`). Tests: `tests/gate_test.cpp` ("MOS switch truth tables match LRM table 28-6, including the z pass-through", "a cmos switch drives its output from both halves independently").

## 39. Delay with an unknown bit (9.4.1)

9.4.1 reads a delay that "evaluates to an unknown or high-impedance value" as a
zero delay, and does not say whether a value with only some bits x or z is
unknown. Clotho reads any x or z bit as making it so, as 11.4.4 does for an
operand that "contains an unknown (x) or high-impedance (z) value", so
`#(4'b1x01)` waits 0 and not the 9 its known bits spell. The rule covers every
delay form, though only the procedural delay control states it: the
intra-assignment delay (9.4.5) and the `->>` delay (15.5.1) are that control,
and the continuous-assignment, gate and net delays (10.3.3, 28.16) take the same
`delay_value` without restating it. A compile-time unknown nonblocking
intra-assignment delay is a plain `<=`, as a compile-time `#0` is.

Code: `src/lower/SystemTasks.cpp` (`scaled_delay`), `src/lower/Assign.cpp` (`intra_nb_delay_is_const_zero`). Tests: `tests/llvm_backend_test.cpp` ("a delay with an x or z bit is a zero delay", "an unknown delay is zero before the time unit multiplier"), `tests/event_test.cpp` ("->> #(x) e: a delay with an x or z bit is a zero delay"), `tests/continuous_assign_delay_test.cpp`, `tests/gate_test.cpp` and `tests/net_delay_test.cpp` (one case each). Oracle: iverilog 13 for the procedural, intra-assignment, `assign` and gate forms; it implements no net delay and no `->>`.

## 40. Sign and range of a `%t` operand (21.2.1.1, 20.4.3)

Table 21-1 describes `%t` only as "Display in current time format", and 20.4.3
says what `$timeformat` sets (units, precision, suffix, minimum width); neither
says what a negative operand prints, or a time too large to count. Clotho reads
the argument as the value it is: a negative signed or real operand is a negative
time, written with a `-` that counts toward the field width, as for `%d`. The
sign is the operand's signedness, not its top bit, so an unsigned all-ones
operand stays positive. A constant and a runtime format string print the same
text, and so does iverilog.

A time whose tick count (the operand times the module's ticks per unit) does not
fit 63 bits is shown as the largest count of its sign, +-(2**63 - 1) ticks, for an
integral operand of any width and for a real alike, as a real delay beyond that
count saturates (`saturating_ticks`), instead of keeping the low 64 bits of the
product. iverilog prints the exact count. An operand with an x or z bit has no
count at all: note 41.

Code: `src/lower/Assign.cpp` (`scale_to_ticks`), `src/lower/SystemTasks.cpp` (`saturating_ticks`), `src/runtime/Format.cpp` (`render_time`, `coerce_dyn_arg`, `integral_time_ticks`, `real_time_ticks`). Tests: `tests/llvm_backend_test.cpp` ("%t of a negative operand prints its sign in a constant and in a runtime format", "%t of a time beyond 2**63 ticks saturates in a constant and in a runtime format"), `tests/real_test.cpp` ("a real delay beyond 2**63 ticks saturates instead of wrapping into an early event").

## 41. `%t` of an operand with an x or z bit (21.2.1.3, 20.4.3)

21.2.1.3 gives the rules for displaying an unknown or high-impedance value only
for `%d`, `%h`/`%x`/`%o` and `%b`; Table 21-1 and 20.4.3 say nothing for `%t`.
`%t` writes a decimal number, so Clotho applies the `%d` rule to it: the whole
number is replaced by one character, `x` when every bit of the operand is x, `z`
when every bit is z, `X` when some bit is x but not every one is, `Z` when some
bit is z and none is x. The `$timeformat` suffix and minimum field width (or an
explicit width) still apply, so under `$timeformat(-9, 2, " ns", 12)` an unknown
operand prints `        x ns`. No known number is ever printed for an unknown
value, and the precision and the unit conversion have nothing to act on. A
constant and a runtime format string print the same text.

iverilog is not followed: it prints the character as the last digit of the
number, scaled by the unit conversion and padded to the precision (`x000`,
`x.00 ns`, `0.00x`), and for a finer unit than the display unit it prints a known
`0 ns` for an unknown value.

Code: `src/runtime/Format.cpp` (`unknown_decimal_char`, `render_arg`, `coerce_dyn_arg`), `src/lower/Assign.cpp` (`scale_to_ticks`). Tests: `tests/llvm_backend_test.cpp` ("%t of an operand with an x or z bit prints one character, in a constant and in a runtime format").

## 42. Implicit return variable of a static function (13.4.1, 13.4.2, 6.21)

13.4.1: "The function definition shall implicitly declare a variable, internal to
the function, with the same name as the function." 13.4.2: "Functions defined
within a module, interface, program, or package default to being static, with all
declared items being statically allocated. These items shall be shared across all
uses of the function executing concurrently." 6.21 gives a static variable the
whole simulation as its lifetime and initializes only an automatic one "on each
entry to the call"; 6.8: "A variable shall store a value from one assignment to
the next". No clause resets the variable when a static function is entered, and
13.3.2 says in so many words that a static task's variables "shall retain their
values between invocations". So Clotho gives the implicit variable of a static
function its type's default value once and keeps it from call to call:
`function int g(int x); g = g + x; endfunction` returns 5 and then 10 for two calls
with 5, a recursive call finds what the call below it left, and each instance of a
module has its own. An `automatic` function, a function of an `automatic` module and
every class method (8.6; 8.10: a `static` method's lifetime is that of the method
within the class, not of its variables) start the variable afresh on every call.

`return expr;` hands the caller `expr` and does not write the variable. 13.4.1 only
says the `return` statement "shall override any value assigned to the function
name", and never says it assigns it, so `function int h(int x); if (x < 0) return
100; h = h + x; endfunction` returns 5, 100, 10 for `h(5)`, `h(-1)`, `h(5)`.

A constant function call is evaluated by slang at elaboration with fresh variables
on every call (13.4.3), which leaves the simulation-time variable alone, as the
LRM says: `localparam P1 = g(5), P2 = g(5)` is 5 and 5, and the first run-time
`g(5)` still returns 5.

The variable only gets storage that outlives the call where the entry value can be
seen: a read of it, or the end of the body, on a path that has not overwritten the
whole variable first, or a function that can be re-entered through its own body.
Any other function behaves identically with a fresh cell per call and keeps it.

iverilog 13 and Verilator 5.050 reset the variable on every call (iverilog keeps a
queue return value and resets an `int`), so neither is followed.

Code: `src/lower/Subroutines.cpp` (`declare_static_return`, `implicit_return_ref`, `build_function`), `src/lower/Scans.cpp` (`static_return_needs_storage`). Tests: `tests/function_test.cpp` ("the implicit return variable of a static function keeps its value between calls" and the tests after it).

## 43. Port coercion to inout (23.3.3.1)

"A port that is declared as input (output) but used as an output (input) or inout may be coerced to inout. If not coerced to inout, a warning shall be issued." Clotho copies the value across an input or output port (23.3.3: "a continuous assignment from source to sink") and does not coerce by default. A coerced port makes its inner and outer net one net, so a copy and a coercion differ only where a value travels against the port's direction. A net port is therefore an error, not a warning, in exactly two cases:

- an **input** port whose net the module drives itself (an `assign`, a gate output, a child's output port): the outer net sees that driver when coerced and does not when copied;
- an **output** port whose outer net has other drivers (another assignment, gate or output port reaching the same bits, or the pull of a `tri0` / `tri1` / supply net), when the module **reads** its own net: it sees the resolved value when coerced and only its own drivers when copied. "The same bits" is decided as the driver tracking decides it: constant and genvar selects of a packed, multi-dimensional or unpacked-array net (an element, a part select, an indexed `+:` / `-:` select) are told apart per instance, so the instances of a generate loop that each drive their own slice of one net, as basejump_stl's `bsg_wormhole_concentrator_in` does with `.data_o(fifo_data_lo[i])`, do not count against each other. A net's continuous drivers cannot be indexed at run time (slang rejects it), so the whole-net fallback covers only a shape the tracking cannot place.

A module that only drives its output net cannot tell, and any number of output ports driving one net is the multiple-driver case 23.3.3.2 permits, so those are accepted. A variable port is never coerced (23.3.3.2), and an unconnected port has no outer net. The message says which case it is and names `--allow-port-coercion`.

`--allow-port-coercion` coerces exactly those ports: the inner net takes the outer net's storage, as for an `inout` port, so every driver of both resolves together. It needs a port connected to a whole net of its own width; a variable, a select, a concatenation, an expression or another width is an error. slang's own `port-coercion` warnings ("coerced to 'inout'") are not shown, since they flag every net with a port driver beside another driver and claim a coercion that did not happen. iverilog always coerces, so it matches the flag and not the default.

Code: `src/lower/Types.cpp` (`needs_port_coercion`, `build_inout_aliases`). Tests: `tests/xhier_test.cpp` ("an input port the module drives is refused unless coercion is allowed", "an output port read by its module while its net has other drivers is refused unless coercion is allowed", "ports whose coercion changes nothing are accepted by default"). Design: `docs/design/cli.md` (Options).

## 44. The time-zero run of `always_comb` / `always_latch` (9.2.2.2, 9.2.2.3)

9.2.2.2: the procedure "is automatically triggered once at time zero, after all
initial and always procedures have been started so that the outputs of the procedure
are consistent with the inputs"; 9.2.2.3 applies it to `always_latch`. "Started"
does not say how far the other procedures have run, so Clotho takes the simple
reading its FIFO active ring gives: every `initial` and sensitivity-less `always`
runs up to its first suspension, then each `always_comb` / `always_latch` runs once,
whatever the source order. A comb written above an `initial` that reads its output
at time zero therefore reads the unsettled value (`x`), and a `disable` of a block
of a later `initial` finds the block started and parked, so it ends it (9.6.2).
iverilog does the same.

A continuous assignment also evaluates once at time zero but is not a procedure,
so alone it is not deferred; 4.9.1 leaves its order against the procedures open. One
that comb fusion puts in the same group as an `always_comb` starts with it, because a
group is one body: that is inside the same freedom (4.7 lets active events run in any
order), and splitting every `assign` / `always_comb` cone to avoid it cost 1.9x on a
mixed design.

Code: `src/ir/llvm/Scheduler.cpp` (`emit_active_run`, `deferred_comb`), `src/ir/Module.h` (`Process::starts_after_procedures`), `src/lower/Statements.cpp` (`lower_procedural_block`). Tests: `tests/comb_fusion_test.cpp` ("always_comb and always_latch run once at time zero after every initial has started").

## 45. Width of an untyped parameter set to an unbased unsized literal (6.20.2, 5.7.1)

`parameter P = '1;` meets two rules. 5.7.1 makes `'1` "an unsized unsigned
single-bit value" that "in a self-determined context ... shall have a width of 1
bit". 6.20.2 gives a parameter with no type or range "for which the final value
assigned to it is unsized" an implied range whose MSB is "an
implementation-dependent value of at least 31". Clotho applies 6.20.2, the rule
written for this declaration, so `P` is 32 bits of ones. slang folds the
parameter the same way. iverilog and Verilator make `P` one bit wide.

Code: slang's parameter elaboration (no Clotho code). Tests: `tests/unbased_test.cpp` ("an untyped parameter set to '1 is at least 32 bits of ones").

## 46. `disable` of a block a process has already left (9.6.2, 12.8)

A process that leaves a named block by `break`, `continue` or `return` is no
longer in it. A later `disable` of that block terminates nothing: 9.6.2 ends "the
activity of a task or a named block", and the block has none. The process keeps
running where it is. iverilog 13 still counts the process as inside the block,
aborts whatever it is waiting on, and resumes it after the block, so a loop left by
`break` runs again. Clotho follows the LRM.

Code: `src/ir/llvm/Inst.cpp` (`leave_blocks`). Tests: `tests/loop_test.cpp` ("an exit that skips a block's end takes the activation off its list", subcase "a `break` out of a block that a later disable names"). Oracle: iverilog 13 disagrees, as above.

## 47. `foreach` with every loop variable omitted (12.7.3)

12.7.3 lets any loop variable be omitted "to indicate no iteration over that
dimension", and its example skips one dimension without changing how often the
body runs. Taken to the case with no loop variables at all, that rule runs the body
once, even over an empty array. The LRM never states this case. iverilog and
Verilator both run the body zero times, and so does Clotho, with a
`foreach-no-loop-variables` warning that the loop is poorly formed.

Code: `src/lower/Statements.cpp` (`lower_foreach`). Tests: `tests/loop_test.cpp` ("foreach with no loop variables never runs its body and warns"). Oracle: iverilog 13 and Verilator 5.050 agree.

## 48. Sorting reals that contain NaN (7.12.2)

7.12.2 sorts "in ascending order" by the relational operators, and 6.12 only
says a real is an IEEE 754 double. A comparison with NaN is false in IEEE 754, so
`<` is not an ordering over a set that holds a NaN: a NaN is neither before nor
after any number, and a comparison sort over that relation has no defined result
(`std::sort` is undefined, and the numbers around the NaN come out misordered).
Clotho orders NaN after every number and treats all NaNs as equal, so `sort` puts
NaNs last and `rsort`, the mirror, puts them first. The numbers themselves always
come out in order. The runtime sort is unstable among equal NaNs and numbers
(equal elements are byte-equal there); a `with` clause sort is stable, so equal
NaN keys keep their input order.

Code: `src/runtime/Containers.cpp` (`slot_less`), `src/lower/Sort.cpp` (`sort_right_wins`). Tests: `tests/array_ordering_test.cpp` ("sorting reals with a NaN orders the numbers and puts NaN last", "sorting by a real key with a NaN"). Oracle: none; the LRM does not say, and iverilog does not implement these methods on reals.

## 49. Force and release of a net with a net delay (10.6.2, 10.3.3, 28.16, 4.9.2)

28.16 delays "the time it takes from any driver on the net changing value to the
time when the net value is updated", and 10.3.3 delays "any value change that is to
be applied to [the net] by some other statement". A force is not a driver, and
neither clause says the net delay applies to it: 10.6.2 has it override all drivers
until a release, and 4.9.2 makes it an active update event. Clotho puts the force on
the net value, past the net delay, so a force and a release take effect at once, a
driver change that is on its way lands under an active force, and a release shows
what the drivers have delivered. A force that the net delay itself delayed would
keep a release from taking effect "immediately", which 10.6.2 requires. 38.34 reads
the same way: `vpiForceFlag` forces the object "with no delay (same as the
SystemVerilog procedural force)".

A release while a driver change is on its way is the case the two clauses seem to pull
apart. 10.6.2: "When released, the net shall immediately be assigned the value
determined by the drivers of the net". 10.3.3: "any value change that is to be applied
to wireA by some other statement shall be delayed for ten time units before it takes
effect". Reading (decided by the project owner): the net takes the value its drivers
determine now, at once, as 10.6.2 says. 28.16 defines a net delay by "any driver on the
net changing value", and a release is not a driver changing value; 38.34's "with no
delay (same as the SystemVerilog procedural force)" is the same for force and release.
The delayed value is overwritten with the drivers' value for the freed bits, so a change
still in flight lands as a no-op when it matches, and a later driver change goes through
the net delay as usual. Only the bits of a force that is active move. iverilog 13
implements no net delay; for a driver's own delay (`assign #5 w = a`) it takes the
driver's delayed output at the release, which is a different thing.

Code: `src/lower/Nets.cpp` (`emit_wire_resolvers`, `release_net`). Tests: `tests/net_delay_test.cpp` ("net delay: a driver change on its way lands under an active force", "net delay: a release takes the drivers' value at once, past a change in flight", "net delay: a forced bit and part release to the drivers' value, each at once"). Oracle: none for a net delay; iverilog 13 agrees on the driver-delay form of the first.
