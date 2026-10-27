# LRM notes

Where IEEE 1800-2023 is ambiguous, contradicts itself, or Clotho deliberately departs from it, the decision is listed here and recorded in full at the code that implements it. Each entry names the clause, states Clotho's reading, and points to where the reasoning lives.

## 1. Integer-to-string conversion of a negative value (6.16.11-6.16.14)

The clauses do not say how `itoa`, `hextoa`, `octtoa` and `bintoa` render a
negative argument. Clotho writes `-` followed by the magnitude in the method's
base, so `hextoa(-256)` is `-100`, and never the two's-complement bit pattern.
An x or z bit of the argument reads as 0.

Code: `src/runtime/Runtime.cpp` (`str_toa_base`). Tests: `tests/string_test.cpp` ("itoa family formats sign + magnitude per LRM 6.16.11-6.16.14").

## 2. `realtoa` precision (6.16.15)

The clause names no precision or format. Clotho prints at full round-trip
precision (`std::to_chars` with no precision argument) rather than at a fixed
number of significant digits.

Code: `src/runtime/Runtime.cpp` (`rt_string_realtoa`). Tests: `tests/string_test.cpp` ("realtoa formats at full round-trip precision").

## 3. Unpacked union layout (7.3)

7.3 fixes a bit-level overlay only for packed unions. Clotho gives an unpacked
union one flat value as wide as its widest member, with every member overlaying
the least significant bits: a member select is a low part-select, and a write to
a narrower member leaves the bits above it untouched. Members must be integral.

Code: `src/Lower.cpp` (`lower_type`, `expand_p_packed`). Tests: `tests/struct_test.cpp` ("unpacked union overlays members at the LSB (flat value, LRM 7.3)", "unpacked union: whole-union copy and nesting in an unpacked struct").

## 4. Reading an element that does not exist (7.4.5, 7.10.2, Table 7-1)

An out-of-range or x/z-indexed array read, and `pop_front` or `pop_back` on an
empty queue, yield the value Table 7-1 gives the element type: 0 for a 2-state
element and x for a 4-state one. A 2-state element never reads as x.

Code: `src/ir/LlvmBackend.cpp` (`slot_default_const`). Tests: `tests/memory_bounds_test.cpp` ("out-of-range read from a fixed-size unpacked array yields the element default", "an unknown (x) index into a fixed-size unpacked array is out of range"), `tests/queue_test.cpp` ("queue pop from empty 2-state queue is default 0", "queue 4-state defaults to x").

## 5. Out-of-range positions of an indexed array slice (7.4.5)

`a[i +: n]` and `a[i -: n]` expand position by position, and each position
follows the invalid-index rule on its own, as 11.5.1 does for a packed
part-select: a position outside the array reads the element default and drops
its write, while in-range positions are unaffected. An x or z bit in `i` makes
every position invalid, so the read is all default and the write does nothing.

Code: `src/Lower.cpp` (`emit_indexed_slice_copy`, `materialize_indexed_slice`). Tests: `tests/assign_pattern_test.cpp` ("an out-of-range indexed slice reads the element default per position", "an x/z indexed slice base is an invalid index (LRM 7.4.5)").

## 6. Overflowing a bounded queue (7.10.4)

7.10.4 does not say which end an overflowing push gives up. Clotho always keeps
the first `bound + 1` elements, so `push_front` drops the old tail, `push_back`
onto a full queue has no effect, and `insert` follows the same rule.

Code: `src/runtime/Runtime.cpp` (`q_apply_bound_ops`). Tests: `tests/queue_test.cpp` ("queue bounded push drops excess", "queue slice/concat into a bounded queue drops the tail").

## 7. Unknown elements in `min` and `max` (7.12.1)

7.12.1 does not say where an element with unknown bits ranks. The first element
seeds the result and a later one replaces it only when its comparison with the
result is a definite 1, so a later element with unknown bits never replaces the
result, and a tie keeps the earlier element.

Code: `src/Lower.cpp` (`lower_array_iter_method`). Tests: `tests/array_locator_test.cpp` ("array min/max over 4-state elements skips unknowns").

## 8. Reduction of an empty array (7.12.3)

7.12.3 gives no result for an empty array. Every reduction method (`sum`,
`product`, `and`, `or`, `xor`) of an empty array returns 0, not the operator's
identity, so `product` and `and` give 0 as well.

Code: `src/Lower.cpp` (`lower_array_iter_method`). Tests: `tests/array_locator_test.cpp` ("array reduction of an empty array is zero").

## 9. Inertial continuous-assignment delay (10.3.3)

A delayed continuous assignment updates its net in the Active region, not in the
NBA region where a procedural `<= #d` commits. Clotho commits the delayed value
when the timing wheel reaches that time step, before any Active-region process
runs, so every process in that time step reads the new value.

Code: `src/ir/LlvmBackend.cpp` (`inertial_sigs_`, `emit_active_run`). Tests: `tests/continuous_assign_delay_test.cpp` ("inertial CA delay: pulse rejection and no-restart on same-value change").

## 10. Re-forcing a forced variable (10.6.2)

A second `force` on a variable supersedes the first, so a later change to the
first force's right-hand side does not reach the variable. Each textual force
site has its own id, and only the site whose id is current drives the variable.

Code: `src/Lower.cpp` (`ForceInfo`). Tests: `tests/force_release_test.cpp` ("force: re-force is last-wins, old RHS link broken"). Design: `docs/design/force-release.md` (Model).

## 11. Keyed assignment pattern over a descending packed array (10.9)

In `logic [3:0][7:0]` key 0 names the least significant byte, so `'{0:x, 3:y}`
puts `y` at the most significant end. slang orders a keyed pattern's elements by
ascending index and puts the lowest at the most significant end, which is wrong
for a descending array, so Clotho reverses the element order there. Positional
and replicated patterns stay in source order.

Code: `src/Lower.cpp` (`lower_packed_pattern`). Tests: `tests/assign_pattern_test.cpp` ("packed array pattern: keyed, descending reverses (slang-bug workaround)").

## 12. Assignment-pattern lvalue member widths (10.9)

10.9 requires each member of an assignment-pattern lvalue to have the same number
of bits as its right-hand element. Deviation: Clotho does not enforce that, and a
mismatched member truncates or extends exactly as the plain assignment the
pattern stands for. Not gated.

Code: `src/Lower.cpp` (`store_pattern_elem`). Tests: `tests/assign_pattern_test.cpp` ("assignment-pattern lvalue: per-element width truncates like an ordinary assignment").

## 13. Equality of unpacked aggregates with unknown members (11.2.2, 11.4.5)

`==` and `!=` on unpacked structs, unpacked arrays and queues fold the
per-member logical equality: a definite mismatch in any member gives 0,
otherwise an unknown member gives x, otherwise 1. Queues of different sizes
compare as a definite 0.

Code: `src/ir/Ir.h` (`AggEq`), `src/ir/LlvmBackend.cpp` (`lower_agg_eq`). Tests: `tests/struct_test.cpp` ("unpacked struct equality is field-wise; X makes it ambiguous (LRM 11.4.5)"), `tests/assign_pattern_test.cpp` ("whole unpacked-array equality is per-element, X-propagating"), `tests/queue_test.cpp` ("queue equality with x-propagation").

## 14. Short-circuit of `->` (11.3.5)

11.3.5 lists `->` with the short-circuit operators, so `a -> b` skips evaluating
`b` when `a` is a definite 0. `<->` is not on that list and 11.4.7 evaluates each
of its operands exactly once, so its right operand always runs.

Code: `src/Lower.cpp` (`lower_short_circuit`). Tests: `tests/function_test.cpp` ("-> short-circuits but <-> always evaluates its right operand").

## 15. `0 ** <negative>` in a 2-state result (11.4.3, 6.3.1)

Table 11-4 gives x for `0 ** n` with a negative `n`. A 2-state result converts
that x to 0 as 6.3.1 does for any 4-state value, and a 4-state result keeps the
x.

Code: `src/ir/LlvmBackend.cpp` (`lower_pow`). Tests: `tests/llvm_backend_test.cpp` ("0 ** negative coerces X to 0 in a 2-state result").

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

Code: `src/Lower.cpp` (`lower_predicate`, `lower_unary`). Tests: `tests/string_test.cpp` ("if on a string tests emptiness", "ternary with a string condition selects by emptiness", "logical negation of a string is rejected").

## 18. `unique` and `priority` violation reports (12.4.2, 12.5.3)

12.4.2 and 12.5.3 defer a violation report to the Observed region, and 12.4.2.1
clears a pending report when its process reaches a flush point first. Clotho
queues the report and issues it in the Observed region. Deviation: it does not
clear pending reports at flush points, so a statement that violates on several
settle passes of one time step reports once per pass. Not gated; the gap is
listed in `docs/TODO.md`.

Code: `src/ir/LlvmBackend.cpp` (`lower_case_viol`), `src/runtime/Runtime.cpp` (`rt_observed_case_violation`, `rt_observed_flush`). Tests: `tests/case_test.cpp` ("priority case: Observed-region report is stamped with the settled instant"). Design: `docs/design/x-semantics.md` (Free wins needing no new semantics).

## 19. `case` with a real selector (12.5)

12.5 words case matching as a bit-for-bit `===`, which Table 11-1 does not allow
on a real operand. A real has no x or z bit to tell `===` from `==`, so Clotho
matches a real selector with each label by IEEE-754 equality: `-0.0` matches
`0.0` and a NaN matches no label. slang rejects `casez` and `casex` over a real
selector, and Clotho keeps that rejection.

Code: `src/Lower.cpp` (`real_case_label_pred`). Tests: `tests/case_test.cpp` ("case with a real selector compares by value", "a real case stops the search at the first match"), `tests/real_test.cpp` ("casez / casex over a real is rejected").

## 20. Dynamic arrays, queues and associative arrays as inputs (13.5)

These containers are values, so an `input` formal of one of their types gets a
deep copy at the call, and the callee's writes, resizes and deletes never reach
the caller's container.

Code: `src/ir/LlvmBackend.cpp` (`emit_copyin`, `emit_handle_assign`). Tests: `tests/dynamic_array_test.cpp` ("dynamic array input argument is copy-in (value semantics)"), `tests/queue_test.cpp` ("queue input argument is copy-in (value semantics)"), `tests/associative_array_test.cpp` ("assoc input argument is copy-in").

## 21. Immediate `cover` (16.3)

An immediate `cover` runs its pass action when its expression is true and does
nothing when it is false. Deviation: Clotho keeps no hit count and prints no
coverage report for it. Not gated.

Code: `src/Lower.cpp` (`lower_immediate_assertion`). Tests: `tests/severity_test.cpp` ("immediate cover runs the pass action when true", "immediate cover does nothing when false: no pass action, no report"). Design: `docs/design/functional-coverage.md` (The precedent that is already silently wrong).

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

Code: `src/Lower.cpp` (`attempt_ring_size`). Tests: `tests/sva_test.cpp` ("a twice-matching attempt reports only its first failing obligation", "a cover of an implication counts a twice-matching attempt once"), `tests/sva/diff_attemptverdict.sv`, `tests/sva/diff_multimatch.sv`. Design: `docs/design/concurrent-assertions.md` (What this document does not establish).

## 24. Attempts that share an obligation (16.14.1)

16.14.1 evaluates the consequent once per antecedent match, so attempts whose
matches end in the same cycle share one obligation and are each still owed a
verdict. Clotho tells them apart with attempt ids, or with per-state counts on
the counted discipline, and reports each. An antecedent with no bounded longest
match is refused rather than under-reported, and an abort over a multi-cycle
antecedent resolves every attempt in flight.

Code: `src/Lower.cpp` (`attempt_ring_size`, `emit_counted_step`), `src/Sva.cpp` (`abort_on`). Tests: `tests/sva_test.cpp` ("many attempts resolving in one cycle are reported once each", "goto and non-consecutive repetition in an antecedent are refused", "an abort over a multi-cycle antecedent resolves every attempt in flight"). Design: `docs/design/sva-property-layer.md` (Why Tier B needs a different discipline, and what it is).

## 25. `$urandom` generator (18.13)

18.13 requires a deterministic, per-thread stable generator but leaves the
algorithm open. Each process and spawned child owns a xoshiro256++ state, seeded
through splitmix64 from its process index, a draw of its parent's stream, or the
`$urandom(seed)` argument. A function or task draws from its caller's state, and
`shuffle` draws from the process's stream.

Code: `src/runtime/Runtime.cpp` (`rt_urandom_seed`). Tests: `tests/sysfunc_test.cpp` ("$urandom is a deterministic per-process stream (pinned)", "two processes each get an independent $urandom stream"), `tests/array_ordering_test.cpp` ("dynamic array + queue shuffle is a stable permutation").

## 26. `randcase` weight sum (18.16)

18.16 forms the weight sum with unsigned addition at the widest weight's
precision. Clotho applies that literally, so a sum that does not fit wraps
(`8'd200 + 8'd100` is 44 in 8 bits, and the first branch always wins), and an
all-zero sum takes no branch.

Code: `src/Lower.cpp` (`lower_randcase`). Tests: `tests/randcase_test.cpp` ("randcase sum precision is the widest weight (LRM 18.16)", "randcase sum wraps at the weight precision (LRM 18.16)").

## 27. `$bits` of a string (20.6.2)

`$bits` of a `string` is evaluated at run time as 8 bits per current character,
so the empty string gives 0.

Code: `src/Lower.cpp` (`lower_bit_query`). Tests: `tests/sysfunc_test.cpp` ("$bits of a dynamic string is 8 per character").

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

Code: `src/runtime/Runtime.cpp` (`severity_line`, `rt_severity`), `src/Lower.cpp` (`severity_banner`). Tests: `tests/severity_test.cpp` ("severity tasks render label + formatted message to stderr", "a labelled immediate assertion's report names the label").

## 29. `$system` (20.17.1)

Deviation: `$system` runs a shell command, so Clotho refuses it at compile time by
default. `--allow-system const` admits a command that folds to a constant at
elaboration and `--allow-system full` admits any command. The function form
returns the `system()` value unchanged, so its encoding is the C library's.

Code: `src/Lower.cpp` (`lower_system_call`). Tests: `tests/system_test.cpp` ("$system is refused at compile time without --allow-system", "$system const tier requires a constant command string", "$system function form returns the raw system() status"). Design: `docs/design/cli.md` (Options).

## 30. `-` flag and leading `0` in a format specification (21.2.1.2)

Specifiers parse as `% [-] [width] [. precision] conv`. Deviation: `-`
left-justifies the field with trailing spaces, although the LRM defines no such
flag. Not gated. A leading `0` belongs to the width (`%0d` is the minimal form),
so the integer conversions have no zero-pad flag, while the real conversions read
it as C's zero-pad flag.

Code: `src/runtime/Format.h` (`parse_format_spec`, `FormatSpec`), `src/runtime/Runtime.cpp` (`render_arg`). Tests: `tests/display_test.cpp` ("left justification").

## 31. Explicit field width on `%h`, `%o` and `%b` (21.2.1.2)

In the clause's examples, `%3h` of `32'h5` prints `005`. A width given in the
format overrides the automatic sizing: Clotho reduces the value to its minimal digits, zero-pads it
to the width, and never truncates.

Code: `src/runtime/Runtime.cpp` (`render_arg`). Tests: `tests/display_test.cpp` ("explicit field widths pad and expand per LRM 21.2.1.2", "display operands wider than 64 bits (LRM 21.2)").

## 32. `%p` output (21.2.1.6)

An integral leaf prints as unformatted decimal, and the members of an unpacked
struct print by name, as in `'{a:171, b:42}`. White space between elements is
the implementation's, and Clotho separates them with `, `. A tagged union whose
tag is x, z or names no member has no valid member, so its whole value prints as
one decimal leaf.

Code: `src/Lower.cpp` (`expand_p_lvalue`, `expand_p_packed`), `src/runtime/Runtime.cpp` (`rt_tagged_format_p`). Tests: `tests/display_test.cpp` ("%p renders unpacked aggregates as assignment patterns (LRM 21.2.1.6)"), `tests/struct_test.cpp` ("whole unpacked struct as a $display %p operand renders a pattern", "%p of a tagged union whose tag is x falls back to the flat value"). Design: `docs/design/tagged-unions.md` (`%p` (LRM 21.2.1.6)).

## 33. Leading NUL bytes under `%s` (21.2.1.7)

A packed `%s` operand drops its leading 0x00 bytes, as the clause's text and the
21.2.1.1 example require. A 0x00 byte after the first non-zero byte prints as a space.

Code: `src/runtime/Runtime.cpp` (`render_arg`). Tests: `tests/display_test.cpp` ("%s drops leading NUL bytes of a packed operand").

## 34. `$fread` reaching end of file inside an element (21.3.4)

The clause does not say what the unread bytes of a partly read element hold.
Clotho packs the bytes it read into the high end of the element and zero-fills
the rest; the return value counts the bytes read.

Code: `src/runtime/Runtime.cpp` (`rt_fread`). Tests: `tests/llvm_backend_test.cpp` ("$fread into a memory with elements wider than 64 bits").

## 35. Program implicit `$finish` with woken work pending (24.3)

The LRM does not say whether work that the last program `initial` woke, such as
an `always` block sensitive to a variable it stored, runs before the implicit
`$finish`. Clotho stops at once and empties the run queues exactly as an explicit
`$finish` does, so that work does not run.

Code: `src/ir/LlvmBackend.cpp` (`emit_program_end`, `lower_finish`). Tests: `tests/program_test.cpp` ("implicit finish does not drain work the last program initial woke").

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

Code: `src/Lower.cpp` (`gate_tristate_value`). Tests: `tests/gate_test.cpp` ("three-state gate truth tables match LRM table 28-5, including X/Z", "MOS switch truth tables match LRM table 28-6, including the z pass-through").

## 38. Resistive switches (28.14)

Deviation: with no strength model there is nothing for the strength reduction of
28.14 to act on, so `rnmos`, `rpmos` and `rcmos` behave exactly as `nmos`, `pmos`
and `cmos`. Not gated.

Code: `src/Lower.cpp` (`mos_switch_of`, `lower_cmos_switch`). Tests: `tests/gate_test.cpp` ("MOS switch truth tables match LRM table 28-6, including the z pass-through", "a cmos switch drives its output from both halves independently").
