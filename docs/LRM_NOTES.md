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

## 6. Overflowing a bounded queue (7.10.5)

7.10.5 says a write to a bounded queue behaves "exactly as if the queue were unbounded
except that if, after any operation that writes to a bounded queue variable, that
variable has any elements beyond its bound, then all such out-of-bounds elements shall be
discarded and a warning shall be issued", and 7.10.3 says `insert` or `push_front` on a
bounded queue deletes "the highest-numbered element" when the new size exceeds the bound.
Clotho keeps the first `bound + 1` elements, on every write: whole assignment (from a
queue, a dynamic or fixed array, a slice, a concatenation or pattern, a function result,
a task output copied back, a nonblocking assignment, an input formal's copy-in, an element
of a container of bounded queues), `push_back`, `push_front` and `insert`. So `push_front`
drops the old tail, `push_back` onto a full queue has no effect, and `insert` follows
the same rule. Every overflowing operation prints one run-time warning on stderr, never
suppressed, naming the queue (when it is a design variable) and giving the element count
it could not hold; Icarus Verilog also warns on each.

Code: `src/runtime/Queue.cpp` (`q_apply_bound_ops`, `rt_queue_drop_tail`), `src/runtime/Containers.cpp` (`rt_queue_assign`, `rt_queue_assign_dynarr`), `src/runtime/Reports.cpp` (`rt_queue_overflow_report`). Tests: `tests/queue_test.cpp` ("queue bounded push drops excess", "queue slice/concat into a bounded queue drops the tail", "a bounded queue keeps its first N+1 elements on every write path").

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
(which 12.5.3.1 applies to `case`) discards a pending report when its process reaches
a violation report flush point first: a resume after suspending on an event control or
`wait`, or an `always_comb` / `always_latch` resumed by a dependent signal. These are
the first two flush points of 16.4.2; its third, disabling the process's outermost
scope, is not one for violation reports: a report whose process is disabled still
matures in the Observed region, though a deferred assertion's report is dropped there
(16.4.4). The two share one generation counter per process; a `disable` bumps its
high half, which a violation report ignores. Clotho queues the report and issues it in the Observed region, and drops it if its thread
reached a flush point meanwhile: an activation start, or a resume from an
event control or `wait`, including one inside a task or function-called task the
thread entered (the generation travels with the call, docs/design/disable.md). So a
zero-delay glitch of the condition reports nothing, as a deferred assertion's does.
A fork child is a thread of its own and flushes its own reports. Not gated. The
resumes from `wait fork` and `wait_order` are not flush points, as note 54 says for
deferred assertions, but a process that holds only unique/priority reports is not
refused them: its reports there are kept.

Code: `src/ir/llvm/SystemTasks.cpp` (`lower_case_viol`, `gen_global`), `src/runtime/Reports.cpp` (`rt_observed_case_violation`, `rt_observed_flush`, `rt_gen_cell_alloc`), `src/lower/Statements.cpp` (`DeferredAssertionScan`), `src/lower/Timing.cpp` (`flush_deferred_on_resume`). Tests: `tests/case_test.cpp` ("priority case: Observed-region report is stamped with the settled instant", "unique / priority report: ..."). Design: `docs/design/disable.md` (16.4.2's other flush point). Oracle: LRM-derived; iverilog ignores `unique` and cannot parse `unique if`.

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
nothing when it is false. The LRM gives it no statistics format, so Clotho counts the
hits (the expression found true) of each site, per instance, and lists them in the
`--coverage` report; a deferred `cover #0` / `cover final` counts at maturation, so a
hit a later activation of its process supersedes is not one. `cover property` and
`cover sequence` list attempts and matches. The report layout is Clotho's. Without
`--coverage` nothing is printed.

Code: `src/lower/Assertions.cpp` (`lower_immediate_assertion`, `lower_deferred_assertion`, `add_cover_report`), `src/lower/SvaMonitor.cpp` (`emit_cover_attempt`), `src/runtime/Reports.cpp` (`cover_file`, `cover_sites_write`). Tests: `tests/severity_test.cpp` ("immediate cover runs the pass action when true", "immediate cover does nothing when false: no pass action, no report"), `tests/coverage_test.cpp` ("coverage: an immediate cover counts its hits ..."). Design: `docs/design/functional-coverage.md` (Cover statements).

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
all-zero sum takes no branch. The same holds past 32 bits: `64'hFFFF_FFFF_FFFF_FFFF
+ 64'd2` is 1, and a negative `longint` weight is its unsigned value.

The draw over a sum of at most 32 bits is one `$urandom_range(sum - 1, 0)`. A wider
sum takes the multi-draw scheme 18.16 names ("multiple calls to `$urandom_range()`
to handle numbers greater than 32 bits") without fixing it: `ceil(width / 32)`
`$urandom` words are stitched, masked to the bit length of `sum - 1` and redrawn
while the result is not below the sum, which keeps the split exactly uniform. The
draw count is then data-dependent (under two attempts on average) but a function of
the process stream alone, so it stays thread-stable. `randsequence` rule weights
(18.17.1) share the draw.

Code: `src/lower/Random.cpp` (`lower_randcase`, `emit_weight_draw`). Tests: `tests/randcase_test.cpp` ("randcase sum precision is the widest weight (LRM 18.16)", "randcase sum wraps at the weight precision (LRM 18.16)", "randcase weights wider than 32 bits split by their ratio (LRM 18.16)", "randcase wide weights are unsigned and the sum wraps (LRM 18.16)"), `tests/randsequence_test.cpp` ("randsequence rule weights wider than 32 bits (LRM 18.17.1)").

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
`top.ap`; iverilog reports `top` for it. The instance's path is read from the
instance and joined to the text around it only when the line is issued, so a
call site that does not report builds no banner.

`$fatal` "terminates the simulation with an error code" (20.10): the run ends
with exit status 1, after its `final` blocks, whatever its finish number.
`$finish` of any level, `$error`, `$warning` and a failed assertion leave the
status 0.

Code: `src/runtime/Reports.cpp` (`severity_line`, `rt_severity`), `src/lower/Lowerer.cpp` (`emit_report`). Tests: `tests/severity_test.cpp` ("severity tasks render label + formatted message to stderr", "a labelled immediate assertion's report names the label"), `tests/exit_status_test.cpp` ("exit status: $fatal ends the run with status 1, whatever its finish number").

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
the rest; the return value counts the bytes read. Zero-fill follows the clause's
"The data loaded from the file are taken as 2-value data ... It is not possible
to read a value of x or z using $fread": an element `$fread` writes holds only 0
and 1. iverilog 13 keeps the unread bits' old value, so a 9-bit `9'bx` with one
byte left reads `1xxxxxxxx` there and `100000000` here.

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

## 50. Program input ports are settled before program code reads them (24.3.1, 24.3.2, 4.7)

24.3.1: "Statements and constructs within a program block that are sensitive to changes (e.g., update events) on design signals are scheduled in the Reactive region. [...] The continuous assignment assign tclk = clk; would also be scheduled in the Reactive region. Likewise, initial procedures within program blocks are scheduled in the Reactive region." 24.3.2: "Program ports are program-scope objects", and "the driving and resolution of nets on the other side of a program port connection also occurs in the reactive region set". So a program's port copy and `assign` are Reactive-region events, in the region its own code runs in. 4.7: "active events can be taken off the Active or Reactive event region and processed in any order", so no clause says whether the copy that feeds an input port runs before the program thread that reads it.

Clotho keeps the copy and the `assign` in the Reactive region, where 24.3.1 and 24.3.2 put them, and picks the order 4.7 leaves open: every queued reactive continuous process (a program's port copy, its `assign`, and the resolver of a net it drives) runs before any program procedural code, at the start of the region and again each time program code wakes one. A program thread therefore never reads a port or continuous assign that has an update pending: `initial $display(a)` on an input port prints the design's value at time zero, not `x`, and a `#5` read in the slot where the design writes the port's source reads the new value. Settling the copy in the Active region would give the same reads but contradicts 24.3.1. The first-in-first-out order used before made both reads depend on seed order and wake order.

Code: `src/ir/llvm/Scheduler.cpp` (`emit_active_run`, `emit_wake_push`, `ensure_reactive_cont_queue`), `src/ir/llvm/Backend.h` (`kReactiveContTag`). Tests: `tests/program_test.cpp` ("a program initial reads an input port settled at time zero", "a program thread resuming in the slot of a design write reads the port settled", "a program waiter on an input port wakes on the design change, not the settle", "a program initial reads its own assign of an input port settled", "a port copy woken behind a queued program thread still settles before it reads"). Oracle: Verilator 5.050 agrees on every case; iverilog 13 agrees except where the program resumes in the slot of the design write, where it prints the old value because it runs program code in the Active region (`docs/design/program-blocks.md` §2). Design: `docs/design/program-blocks.md` §3.1.

## 51. Order of a timed nonblocking update against other nonblocking updates (4.6, 10.4.2, 15.5.1)

4.6 b) says "NBAs shall be performed in the order the statements were executed", and
10.4.2 that "if there is clear ordering of the execution of a set of nonblocking
assignments, then the order of the resulting updates of the destination of the
nonblocking assignments shall be the same as the ordering of the execution". Neither
says when the update of `x <= #d v` or `x <= @e v` is created, and so which of two
statements was "executed" first once one is timed. 15.5.1 says it for `->>`: it
"creates a nonblocking assign update event in the time in which the delay control
expires or the event control occurs". Clotho applies that reading to `<=` as one rule:
an update is created when its statement executes (a plain NBA, a zero delay), when its
delay elapses (at the start of that instant, before any process runs) or when its event
occurs, and updates are performed in the order they were created. Updates created at one
moment, which is one wheel tick, one occurrence of an event (for a signal, one value
change, however many of that signal's own edge controls it satisfies) or the leaves of one
statement, are performed in the order their statements were executed. The events that `->>`
triggers in the NBA region are such updates too, and trigger in the order the `->>`
statements ran.

A signal's own controls share a moment; a control on a select or an expression of it does
not. `@(posedge c[0])`, a struct member, an array element or an operator expression is
waited on through a net that Clotho synthesizes for it and drives with a continuous
process (`event_signal`, `synth_event_net`; the wait engine detects edges on whole signals),
and that net changes one step after the signal. Its records are released in a separate,
later moment than those of `@(c)` and `@(posedge c)`, so `w <= @(posedge c[0]) 10;
w <= @(posedge c) 20; c = 1;` ends with `w` 10 (the control on `c` released first, and
the one on `c[0]` was created last), whichever statement was executed first, while
`v <= @(posedge c) 10; v <= @(c) 20;` ends with `v` 20. The clauses do not say how soon
an expression is noticed to have changed. iverilog 13 prints 10 for the first design too.

So a plain NBA executed after a timed update was created wins over it, and one executed
before commits before it. Both are decided by every reading. Two cases are not, and the
other reading, ordering by the execution of the statements, gives the other answer:

- A plain NBA executed after the timed statement and before its update is created:
  `x <= @e 1; #1; x <= 2; -> e;` ends with `x` 1. Ordering by execution gives 2.
- Two event-controlled updates whose events occur in the opposite order to their
  statements: `x <= @e1 1; x <= @e2 2; -> e2; -> e1;` ends with `x` 1, and so does the
  same with a write to another bit or element in between. Ordering by execution gives 2.
  A `<= #d` update and an event-controlled one released in its instant are the same case:
  the delayed update is created first and the event's wins, whichever statement was
  executed first.

iverilog 13 applies an event-controlled update at the occurrence, in the active region.
It agrees with Clotho for two event-controlled updates and wherever the plain NBA executes
after the occurrence, and prints the other answer wherever a plain NBA executes before the
occurrence or a `#d` update is involved. Verilator 5.050 orders by source position (swap
the `initial` blocks of the first case and its answer changes) and does not support
`repeat (n) @e`. Neither decides, so the choice is Clotho's, made once.

Code: `src/runtime/Scheduler.cpp` (`intra_ready_push`, `rt_intra_now`, `rt_intra_moment`), `src/ir/llvm/Events.cpp` (`emit_release_moment`, `emit_event_fire`, `emit_event_pend_drain`), `src/ir/llvm/Stores.cpp` (`emit_intra_nba_ordered`, `emit_nba_service`), `src/lower/Timing.cpp` (`event_signal`, `synth_event_net`). Tests: `tests/intra_assign_test.cpp` ("a plain NBA executed before the event commits before the update", "event-controlled updates commit in the order their events occurred", "a delayed update precedes an event-controlled one released in its instant", "updates released by one change keep the order of their statements", "an edge control on a select is a later moment than the signal's own", "nonblocking event triggers occur in the order they executed"). Oracle: none that decides; iverilog 13 and Verilator 5.050 differ as above, and iverilog 13 does not implement `->>`.

## 52. An ambiguous strength against an unambiguous one at an equal level (28.12.3, 28.12.2)

28.12.3 rule b drops the levels of an ambiguous signal that are "smaller than or equal to" the strength of the unambiguous signal it combines with. Read literally, a definite 1 at strong would beat an x driver whose strength0 is strong (the x range loses its strong0 level), though 28.12.2 makes two signals of equal strength and opposite value x. Clotho reads the rule as 28.12.3 introduces it, the unambiguous signal combining "with a component" of the ambiguous one: that component, a strong0, meets the strong 1 at equal strength and opposite value, so 28.12.2 applies and the result is x. iverilog 13 gives the same value for every pair of drivers (value 0, 1, x or z, each with any legal strength pair, 9216 cases). The pairwise rules are not associative: with three drivers some combinations (about 0.5%) give different values in different orders, and the LRM fixes no order. The N drivers of a net therefore resolve in one step, independent of order: 0 when the strongest definite 0 is stronger than every strength a 1 can have (a definite 1 or any x driver's strength1), 1 symmetrically, z when nothing drives, x otherwise. In every order-dependent triple checked that is one of the pairwise-order results. iverilog folds pairwise and differs from it on 10 of 6000 random triples: 5 are order-dependent triples, and in the other 5 every order of the rules as read here gives x where iverilog gives a definite value (one order of the literal rule b gives that value). A range that holds one value and high-Z (the L and H of 28.12.2) is x, as in note 37.

Code: `src/ir/llvm/Ops.cpp` (`lower_strength_res`). Tests: `tests/net_resolution_test.cpp` ("an x driver with asymmetric strengths contributes a strength range", "two drivers of every value and strength pair resolve by LRM 28.12"). Oracle: iverilog 13.

## 53. `$clog2` of an argument with an x or z bit (20.8.1)

20.8.1 defines `$clog2` as the ceiling of the log base 2 of its argument, "treated as
an unsigned value", and says nothing about an argument with an x or z bit. Clotho
gives all x (a 32-bit `integer` of x bits) wherever the call is evaluated: at run
time, in a localparam or other constant expression, and in a constant operand of a
procedural statement. Where the language needs a known number (a vector width, a
range bound, a replication count) the x has no value to use, so it is slang's error
"value must not have any unknown bits", located at the declaration or expression that
needs the number. A 2-state destination converts the x to 0 as 6.3.1 does for any
4-state value.

iverilog 13 gives all x for a constant and for a run-time operand. Verilator 5.050
gives all x for a constant but, at run time, a 2-state variable has already turned
the operand's x bits into 0: `n = 8'bx; $clog2(n)` is 0 and `8'b0000_1x01` reads as
`8'b0000_1001` (4). slang's constant evaluator also flattened the unknown bits to 0,
so a constant gave a number and `logic [$clog2(8'bx)-1:0]` was a two-bit vector.
`patches/slang-clog2-unknown.patch` makes it x, as the run-time lowering already was.

Code: `patches/slang-clog2-unknown.patch` (slang's `Clog2Function::eval`), `src/ir/llvm/Ops.cpp` (`lower_clog2`). Tests: `tests/llvm_backend_test.cpp` ("$clog2 propagates X when the operand has unknown bits", "$clog2 of a constant with an x or z bit is x in a localparam", "$clog2 of a constant with an x or z bit is x in a procedural statement", "an x result of $clog2 where a known value is required is an error at the declaration"). Oracle: iverilog 13; Verilator 5.050 differs at run time.

## 54. What flushes a deferred assertion's reports (16.4.2, 16.4.4, 9.4.3, 9.6.1, 14.11)

16.4.2 lists three flush points, and for the first, a process "having been suspended earlier
due to reaching an event control or wait statement" that "resumes execution", says neither
which constructs those are nor when a wake is a resume. Clotho flushes when a process
resumes from an event control (including one on an assignment, and `@(e iff g)` once the
guard holds) and from a `wait (expr)` that suspended it. It does not flush for a `wait`
whose condition is already true (the process never suspended), for a wake that finds the
condition or the `iff` guard still false (the process stays suspended, and a report queued
before it matures in the instant it is in), for a delay (`#n`, `#0`), or for a cycle delay
`##n`: the clause names only event controls and wait statements, and its own a3 / a4
example relies on a delay not flushing.

`wait fork` and `wait_order` are `wait_statement`s in the grammar (A.6.5), but the clause does
not say it means them, and whether a `wait fork` that finds no child suspended is not known
to the frontend, so a process that holds a deferred assertion is refused both. A task the
process calls that waits on an event control flushes the process's reports where it
resumes, as the process's own event control does: the generation of the calling thread is
published through the call, for a virtual task and a task reached through other tasks too.

16.4.4 says disabling "a non-outermost scope of a procedure does not cause flushing", so a
`disable` of a block nested in the process, from the process or from another one, leaves the
report to mature even though the process resumes from it. That holds whatever the process was
parked on: one parked on an event control inside the block is also leaving it because of the
disable and not because its event occurred, and 16.4.2's first bullet, read alone, would
flush it. Clotho takes 16.4.4's explicit sentence. Disabling "a specific deferred assertion"
by its label cancels its reports; that is refused (docs/TODO.md).

Code: `src/lower/Timing.cpp` (`flush_deferred_on_resume`, `lower_event_wait`, `lower_wait_stmt`, `lower_wait_order`), `src/lower/Assertions.cpp` (`design_has_reports`, `task_flushes_reports`), `src/lower/Statements.cpp` (`lower_disable`, the `wait fork` case), `src/ir/llvm/Inst.cpp` (`DeferFlush` in a task). Tests: `tests/severity_test.cpp` ("deferred assertion: resuming from an event control or a wait statement flushes the report", "deferred assertion: what is not a resume from an event control keeps the pending report", "deferred assertion: wait fork and wait_order are refused", "deferred assertion: a wait inside a called task flushes the calling process's report"), `tests/loop_test.cpp` ("a disable flushes a deferred report only at the outermost scope, for every deferred statement", "a disabled block's process parked on an event keeps its pending report unless it is the outermost"). Oracle: none; iverilog rejects deferred assertions and Verilator reports at the point of execution.

## 55. `$fread` start address outside the memory (21.3.4.4)

21.3.4.4 says `start` "shall be used as the address of the first element in the memory
to be loaded" and its example (`up[10:20]`, start 12 loads `up[12]`) makes it an address
in the declared range, whichever way the range is written. It does not say what a
`start` outside that range means. Clotho reads it as naming no element: nothing is read,
the result is 0, the file position does not move, the memory is untouched, and a run-time
warning (`$fread start address -1 is outside the memory range [0:31]`) is issued. A
`start` of -1 on `mem[0:31]` used to load 32 bytes from address 0 up, losing the byte for
-1, and a `start` of 40 already read nothing. iverilog 13 gives the same result and warning.

The rest of the clause does decide the neighbouring cases, and Clotho follows it. `count`
is "the maximum number of locations in mem that shall be loaded", so a `count` that runs
past the highest address is cut off at the end of the range, silently (iverilog warns); a
`count` of 0 or below loads nothing. Loading counts up from `start` through the highest
address for an ascending and a descending range alike. The clause is silent on an x or z
bit in `start` or `count`: Clotho reads it as 0, as iverilog does, so an all-x `start` is
the lowest address.

`start` and `count` are unsigned when their expression is (11.8.3 widens by the operand's
own signedness). `reg [7:0] s = 200` is address 200, not -56; that was also wrong for the
`start` and `finish` of `$readmemh` / `$readmemb`, which share the operand lowering.

What a short final element holds is note 34, which the clause does not decide either.

Code: `src/runtime/Io.cpp` (`rt_fread`), `src/runtime/Reports.cpp` (`rt_fread_range_report`), `src/lower/SystemTasks.cpp` (`lower_address_arg`), `src/ir/llvm/SystemTasks.cpp` (`lower_fread_mem`). Tests: `tests/llvm_backend_test.cpp` ("$fread with a start address outside the memory reads nothing and warns", "$fread start and count follow the declared address range", "$fread and $readmem take an unsigned start as an address"). Oracle: iverilog 13 (`ivltests/fread.v`).

## 56. Negative, NaN and infinite real delays (9.4.1, 6.12.1)

9.4.1 reads a negative delay as a two's-complement unsigned time, and 3.14.1 rounds a
delay to the time precision before use but does not say how a tie rounds, so Clotho
takes 6.12.1's rule for a real converted to an integer, away from zero. It rounds
first and then applies 9.4.1, so `#(-1.5)` under a 1ns precision rounds to -2 and
waits 2**64 - 2 ticks, which never fires, and `#(-0.4)` rounds to 0 and fires at
once. The LRM says nothing of NaN or infinity. Clotho reads NaN as 0, as LLVM's
saturating float-to-integer conversion gives, and saturates an infinity, like any
tick count beyond 63 bits, at the largest count of its sign, so `#(1.0/0.0)` and
`#(-1.0/0.0)` never fire. The rule covers the delay control, intra-assignment
delays and continuous-assignment delays alike.

Code: `src/lower/SystemTasks.cpp` (`scaled_delay`, `saturating_ticks`), `src/ir/llvm/Ops.cpp` (`lower_ftoi`). Tests: `tests/real_test.cpp` ("a negative or NaN real delay reads as an unsigned time after rounding", "a real delay beyond 2**63 ticks saturates instead of wrapping into an early event"). Oracle: iverilog 13 for the negative and runtime NaN cases; its runtime and constant answers disagree for NaN and infinity.

## 57. Inferred sensitivity through a handle (9.4.2.2, 9.2.2.2.1, 25.9)

9.4.2.2 adds "all nets and variables that are read by the statement" to `@*`, so a
class property read through a handle (`obj.x`) and a virtual-interface member
(`vif.a`) wake an `@*` on a change of the member and on a change of the handle, which
is itself a variable read. 9.2.2.2.1 says "References to class objects and method calls
of class objects do not add anything to the sensitivity list of an always_comb, except
for any contributions from argument expressions", so `always_comb` over `obj.x` waits
on neither the member nor `obj`. A method call of a class object is the same: the
handle it is called on adds nothing, and neither does anything the method's body reads
(a module variable included), while its argument expressions do, and so does what only
selects the object (`objs[i].get()` waits on `i`). `@*`, `wait` and `@(expr)` read the
handle of such a call as they read any variable (9.4.2.2, 9.4.3). A virtual interface is
not a class object, so `always_comb` over `vif.a` waits on the member and on `vif`, and
a method called through one is a hierarchical call, analyzed as a normal function. A
static property is a variable of the class, not a reference through an object, and
keeps its own signal, and a static method called by its class scope is a normal
function too. A body that reads through a handle cannot be statically subscribed, so it
waits in its own loop and an `always_comb` of that kind is self-starting and queued late,
so its time-zero run is note 44's. iverilog 13 runs an always_comb that calls a method
again when the handle or a variable the method reads changes, against the clause; the
decision follows the clause.

Code: `src/lower/Scans.cpp` (`SensitivityCollector::handle(MemberAccessExpression)`, `handle(CallExpression)`), `src/lower/Timing.cpp` (`emit_inferred_park`, `lower_inferred_loop`). Tests: `tests/event_test.cpp` ("always @* wakes on a class property and on its handle" and the five after it), `tests/class_test.cpp` (the two on a method call and an always_comb). No oracle parses the property and virtual-interface designs; iverilog 13 parses the method-call ones and disagrees, as said.

## 58. Event control over a handle's members, in a list (9.4.2, 9.4.2.1, 9.4.2.3, 8.4, 25.9)

9.4.2: "A non-edge implicit event shall be detected on any change in the value of the
expression. However, an edge event shall be detected only on the LSB of the expression."
9.4.2.1 makes a list the logical OR of its events, and 9.4.2.3 gates a term with `iff`:
"This type of expression is evaluated when a changes and not when enable changes." A term
that reaches a class property or a virtual-interface member names its storage only once its
handle is read, so the wait cannot be a static subscription or a net driven from time zero.
Clotho waits in one loop (`lower_sampled_event_wait`, the one a method call in an event
control uses too, docs/design/classes.md) that samples every term of a list made only of
such terms, a property or a member by itself included: it parks on
everything the terms read (a member's waiter list in the instance a handle names, a
property's any-change notifier in the object, a signal's bucket) and, on each wake, compares
every term with its sample: a change of value, or for `posedge`, `negedge` and `edge` the
Table 9-2 edge of bit 0, with the term's own `iff` evaluated at that point. The list resumes
when any term fires. A wake does not say which source woke it, so a change that reverts
before the waiter runs is missed (docs/design/scheduler-rules.md R9). That is why a list
mixing such a term, one that reads a property or a member inside a larger expression, with
a signal or an event is refused for now: the signal's and the event's
own waits catch what the comparison loses (a pulse that reverts, a second trigger in the step
the wait began in), and keeping that needs a per-registration fired marker in the wait node.
A property or a member by itself in a list with signals and events keeps its own wait source
and the signal's pulse. `->> @(m) e` and `x <= @(m) y` over such a member are refused for the
same reason: they register a detached subscription on a signal and edge.

8.4 makes an access through a null object handle illegal, with an indeterminate result that
"implementations may issue an error", and 25.9 says "Attempting to use a null virtual
interface shall result in a fatal run-time error". An explicit control evaluates its
expression where the wait begins, so a null handle in any term of a list, or in a lone
compound term, is a located fatal at the wait, as for `@(obj.x)`. An inferred `@*` is
different (note 57): it reads nothing until its body runs, so it waits on the handle.

Code: `src/lower/Timing.cpp` (`classify_event_control`, `lower_sampled_event_wait`), `src/lower/Scans.cpp` (`event_expr_reads_heap`, `WaitPropScan::handle(EventListControl)`). Tests: `tests/event_test.cpp` ("an event list with a class property term wakes on each of its terms" and the three after it), `tests/interface_test.cpp` ("virtual interface: an event list over member expressions" and the four after it), `tests/heap_wait_test.cpp` (the four on a list of heap terms). No oracle parses these designs; the plain-signal analogs of the edge and list traces agree with iverilog 13.

## 59. `$bits` of a class data type (20.6.2, 6.24.3)

20.6.2 defines `$bits ( data_type )` and sizes a data type only by example: for a struct of
`logic valid; bit [8:1] data;` it "shall return 9, the number of data bits needed by a
variable of type MyType", and "The $bits function can be used as an elaboration-time constant
when used on fixed-size data types". 6.24.3 lists "Unpacked arrays, structures, or classes of
the preceding types" among the bit-stream types. Neither gives a class type a size: a handle
of the type can refer to an object of any derived class, and 11.4.14.1 streams "each data
member of the referenced object, and not the handle itself", the members of a base class
first, and skips a null handle. Clotho keeps the value slang gives the type: the bits of its
properties, the base class's first, so `$bits(B)` is 32 for `class B; int a; endclass` and
`$bits(D)` is 96 for `class D extends B; int b, c; endclass`, in a statement and as an
elaboration-time constant. The expression form on a handle has the 11.4.14.1 answer, the
members of the object it refers to (96 for a `B` handle to a `D` object, 0 for a null one),
which no type fixes, so it is refused until class-handle streaming exists.

Code: `src/lower/SystemTasks.cpp` (the `$bits` fold in `lower_sysfunc`, `bits_of_expr`). Tests: `tests/sysfunc_test.cpp` ("$bits of a class data type is the bits of its properties, base class first", "$bits of an operand that holds a class handle is refused"). iverilog 13 rejects the class data type ("Invalid data type for $bits()"), so no oracle covers it; the reading keeps slang's value.

## 60. A runtime width on an indexed queue slice (7.4.5, 7.10.1)

Deliberate deviation. 7.4.5 says "The size of the part-select or slice shall be constant, but the
position can be variable", and 7.4.6 writes the slice `A[i+:c]` with a constant `c`. 7.10.1 exempts a
queue's `Q[a:b]` only: "the slice bounds may be arbitrary integral expressions and, in particular,
are not required to be constant expressions". It does not mention `Q[i +: n]`. Clotho also accepts a
runtime width `n` there (slang accepts it too). The slice is well defined for any `n`: it is the
`Q[a:b]` slice with `a = i`, `b = i + n - 1` for `+:` and `a = i - n + 1`, `b = i` for `-:`, so it clamps
against the queue's length and yields the empty queue for a width below 1 exactly as `Q[a:b]` does.
Rejecting it would break designs that run correctly.

Code: `src/lower/Containers.cpp` (`append_queue_pieces`, the indexed range-select case). Tests: `tests/queue_test.cpp` ("queue indexed part-select slice", whose `q[2 +: n]` case has a runtime width). Oracle: none; iverilog 13 crashes compiling any indexed part-select of a queue.

## 61. `option.detect_overlap` reports only `bins` bins (19.7, Table 19-1)

Table 19-1: "When true, a warning is issued if there is an overlap between the range list (or transition list) of two bins of a coverpoint." The clause does not say whether `ignore_bins` and `illegal_bins` are among "two bins". 19.5.5 removes an ignored value from every coverage bin and 19.5.6 flags an illegal one, so sharing values with them is what they are written to do. Clotho warns, once per pair, only between bins declared with `bins`.

Code: `src/lower/Coverage.cpp` (`cover_report_overlaps`). Tests: `tests/coverage_test.cpp` ("coverage: detect_overlap warns on overlapping bins").

## 62. `start()` and `stop()` on a coverpoint (19.8, Table 19-5)

Table 19-5 gives `start()` and `stop()` to covergroup, coverpoint and cross, as "Starts collecting coverage information" and "Stops collecting coverage information", and says no more. Clotho reads the covergroup's switch and each coverpoint's as independent: a coverpoint counts a sample only while both are on, and neither call undoes the other.

Code: `src/runtime/Coverage.cpp` (`rt_cover_start_point`, `rt_cover_hit`). Tests: `tests/coverage_test.cpp` ("coverage: stop and start on a coverpoint").

## 63. Which objects own an RNG (18.14.1, 18.14.2, 18.13.3-18.13.5)

18.14.1 gives every object "an independent RNG" seeded at `new` from the creating thread (or the initialization RNG for a static declaration initializer). A generator nothing can observe is not observable, so Clotho gives one only to a class that can be the receiver of `randomize`, `srandom`, `get_randstate` or `set_randstate`: a compile-time scan marks the static receiver type of every such call (an implicit receiver is the enclosing class), and a class owns the generator when any class in its base chain is marked, since a base-typed handle may hold a derived object. Other classes keep no RNG and draw nothing at `new`. This is indistinguishable from giving every object an RNG: the generator is read only through those four methods, and an unmarked class has none of them called on it, while the one draw at `new` comes from the creating thread's stream, which the observable `$urandom` values of an unmarked class's creator would otherwise shift. It differs from "every object draws" in exactly that shift, and only for a design that has no way to see the object's RNG: Clotho keeps the existing pinned streams of such designs.

Code: `src/lower/Scans.cpp` (`RngReceiverScan`), `src/lower/Classes.cpp` (`lower_class_type`), `src/lower/Lower.cpp`. Tests: `tests/randomize_test.cpp` ("object randstate and srandom on a class with no rand property", "only a class used for randomization draws from the creating thread at new").

## 64. Numeric conversion into a `string` destination (21.3.4.3, 21.6, 6.16, 5.7.1)

`$fscanf`, `$sscanf` and `$value$plusargs` store a `%d`, `%h`, `%o`, `%b` or `%c`
value into a `string` as an integral-to-string assignment (6.16): the value is cast
to string, each successive 8 bits a character, and every "\0" is dropped. No clause
gives the value's width, and the cast needs it. Decided reading: the value is sized
like an unsized literal (5.7.1), at least 32 bits, signed for `%d`, wider when its
digits need it, with x and z digits read as 0. So `"-5"` under `%d` is the four
characters FF FF FF FB, the same string as `string'(-5)`, and 16706 is "AB". iverilog
13 asserts on this case.

## 65. A real under `%c`, `%u` and `%z` (21.2.1.1)

21.2.1.1 lets the integer specifiers take integral data, and `%u` / `%z` transfer the
2-value / 4-value "binary representation of the specified data". For a real argument
Clotho takes `%c` of its rounded integer (6.12.1, ties away from zero), whose low 8
bits are the character, as iverilog does, and `%u` / `%z` of its IEEE 754 bit pattern,
64 bits for a real and 32 for a shortreal, with no unknown bits. slang rejects `%u` and
`%z` of a real in a literal format, so only a runtime format reaches it.

## 66. Addresses of a multidimensional memory in `$readmem*` and `$fread` (21.4, 21.4.3, 21.3.4.4)

21.4.3 fills a multidimensional unpacked array "in row-major order, with each dimension's
entries ranging from low to high address", whichever way its ranges are declared. 21.4 says
"The start_addr and finish_addr arguments apply to the addresses of the unpacked array
selected by memory_name. This address range represents the highest dimension of data in
the filename", and 21.4.3 that "Address entries in the file exclusively address the highest
dimension's words". 21.3.4.4 gives `$fread` the same memory: `start` is "the address of the
first element" and `count` "the maximum number of locations in mem".

Decided reading (with the user): `start` and `finish` of `$readmemh` / `$readmemb`, `@`
addresses in the data file, and `start` and `count` of `$fread` all address the highest
dimension. A word of it is a row of the lower dimensions, whole, filled row-major, so
`$fread`'s `count` is a number of rows and a finish below the start loads rows downward. A
row the data leaves unfinished keeps its other leaves ("the remaining subwords are left
unchanged"), and an `@` address abandons the row in progress. For one dimension this is the
old behaviour.

iverilog 13 agrees for a load that names no address, and differs when one is given: it
takes `start`, `finish`, `count` and `@` as flat leaf indices (`$readmemh(f, m, 1, 2)` on
`m[0:2][0:3]` loads leaves 1 and 2, where Clotho loads rows 1 and 2). The LRM is the source
of truth here, so the tests that name an address are derived from it.

Code: `src/runtime/Io.cpp` (`RowOrder`, `rt_readmem`, `rt_fread`), `src/ir/llvm/SystemTasks.cpp` (`mem_inner_dims`). Tests: `tests/llvm_backend_test.cpp` ("$readmemh fills a whole multi-dimensional memory row-major", "$readmem fills a multi-dimensional 4-state, 2-state and wide memory", "$fread fills a whole multi-dimensional memory row-major", "a data file address names a word of the highest dimension", "$readmem start and finish name words of the highest dimension", "$fread start and count are words of the highest dimension"). Oracle: iverilog 13 where it agrees.

## 67. Slices, elements and invalid targets of `$readmem*` and `$fread` (21.4, 7.4.5, 11.5.1)

21.4 lets `memory_name` be "an unpacked array or a partially indexed multidimensional
unpacked array that resolves to a lesser dimensioned unpacked array", with "Higher order
dimensions ... specified with an index" and the lowest "with slice syntax". The addresses
"apply to the addresses of the unpacked array selected by memory_name", and "When slice
syntax is used ... any start_addr and finish_addr arguments shall fall within the bounds
of the slice's range". Clotho takes the array the argument names as the memory to load,
with its own bounds as addresses: `a[2:5]` has addresses 2 to 5, `m[1]` those of its
dimension, and the default start is the lowest of them, whichever way they are declared
(note 66 gives the rows of an array of several dimensions). An indexed slice
`a[i +: n]` / `a[i -: n]` is the slice `a[i:i+n-1]` / `a[i-n+1:i]` (11.5.1) and has the
same addresses, the indices it selects: `a[2 +: 4]` and `a[2:5]` are addressed 2 to 5, whatever
range slang types the slice with. Decided reading (with the user).

7.4.5 makes writing an array through an invalid index a no-operation, and says nothing of
a slice that is partly outside its array, or of a load. Clotho applies the rule to each
word, as it does to a write of a slice, so the words of a slice outside the array are
dropped where they fall and the others keep their place: `a[1 -: 3]` of `a[0:7]` takes
its second word for `a[0]`. A file's word that falls outside is reported by `$readmem*`
and its bytes are read and dropped by `$fread`. A target that names no word at all, an
element index outside the array or with an x or z bit, a slice whose base has one, or a
slice wholly outside the array, loads nothing: no file is opened, no byte is read, and
`$fread` returns 0.

iverilog 13 accepts none of these target shapes ("Array cannot be indexed by a range"), so
the tests take as oracle its load of the whole memory that places the same words
(`a[2:5]` is `a` with start 2 and finish 5, `m[1]` leaves 4 to 7), and derive the rest
from the clauses above.

Code: `src/lower/SystemTasks.cpp` (`lower_mem_target`), `src/ir/llvm/SystemTasks.cpp` (`resolve_mem_target`), `src/runtime/Io.cpp` (`rt_readmem`, `rt_fread`). Tests: `tests/llvm_backend_test.cpp` ("$readmemh into a constant slice of a memory", "$readmemh into an element or a slice of an element of a multi-dimensional memory", "$readmemh into a slice at a variable position", "$readmemh through an invalid index loads nothing", "$fread into a slice or an element of a memory"), `tests/runtime_test.cpp` ("rt_readmem loads the words that exist and drops the others").

## 68. Dynamic arrays and queues as the target of `$readmem*` and `$fread` (21.4.1, 21.3.4.4, 20.7)

21.4.1 says "When loading dynamic arrays and queues, the current size of the array is
fixed; the array shall not be resized", and nothing of their addresses. 20.7 gives them a
range, `$left` 0 and `$right` the size minus 1 "for a queue or dynamic array dimension",
so Clotho loads one as the memory `[0:size-1]` at the size it has when the task runs. The
default start is 0, and `start`, `finish`, `@` addresses and `$fread`'s `start` and
`count` are indices into it; a number addressed past the last element is dropped with the
warning a memory's last address gives, and no element is created. 21.3.4.4 names neither
container, and `$fread` takes the same reading. A dynamic array of fixed arrays is a
memory of several dimensions whose highest is the array (note 66), and a slice `d[a:b]`
or `d[i +: n]` of a dynamic array has the addresses and placement note 67 gives a slice of
a fixed array.

A slice of a queue, `q[a:b]`, `q[i +: n]` or `q[i -: n]`, is the elements 7.10.1 gives it:
from the first bound, but not below 0, to the last, but not past `$`, and none when that
range is empty or a bound has an x or z bit (`q[i +: n]` is `q[i:i+n-1]` and `q[i -: n]` is
`q[i-n+1:i]`, 11.5.1). As a value it is a new queue, but 21.4 lets the memory be written
"with slice syntax", and the load fills the elements of `q` that the slice selects, as it
fills those of a slice of a fixed array, never resizing `q` (21.4.1). Decided reading (with
the user): the addresses are the indices of `q`, so `q[2:$]` of six elements has the
addresses 2 to 5 and its default start is 2, and `start`, `finish` and the `@` addresses of
the file name indices of `q` that lie in what is left of the slice once the queue has
clipped it (`q[3:9]` of six is [3:5], note 69). Every slice has one rule (note 67): an
indexed slice of a fixed or dynamic array and of a queue is addressed by the indices it
selects, as `[a:b]` is. A slice that selects no
element names no word, as an empty queue does below: nothing is opened or read.

A dynamic array or queue with no element has `$right` -1, which is no address at all: the
load names no word, as a slice wholly outside its array does (note 67), so no file is
opened or read, and `$fread` returns 0 with the file where it was.

A load that changes an element wakes whoever waits on the array or queue, as a store to
one does (9.4.2, 9.4.3); a load that changes none wakes nobody, as for a memory.

iverilog 13 takes neither container as a memory argument ("$readmemh's second argument
must be a memory"), so the tests take as oracle its load of the fixed memory `[0:n-1]`
with the same file and arguments, and derive the rest from the clauses above.

Code: `src/lower/SystemTasks.cpp` (`is_mem_target_shape`, `lower_mem_target`, `lower_queue_slice_target`), `src/ir/llvm/SystemTasks.cpp` (`resolve_mem_target`, `finish_mem_load`), `src/runtime/Io.cpp` (`MemDest`, `rt_readmem`, `rt_fread`). Tests: `tests/llvm_backend_test.cpp` ("$readmemh into a dynamic array or a queue takes the addresses 0 to size - 1", "$fread into a dynamic array or a queue fills the words it holds", "$readmem into a dynamic array or a queue of 4-state, 2-state and wide elements", "$readmemh into a slice of a dynamic array places its words like a fixed memory", "$readmemh into a slice of a queue loads the elements it selects, by queue index", "$readmemh into a slice of a queue that selects no element does nothing", "$readmemh into a slice of a queue takes start, finish and file addresses inside the slice", "$fread into a slice of a queue fills the elements it selects", "$readmem into a slice of a queue of 4-state, 2-state, wide and array elements", "a load into a slice of a queue wakes the waiters of its elements", "$readmem and $fread into a slice of a queue held by a struct, a class, a ref argument or another container", "$readmemh into a dynamic array of fixed arrays, an element or a slice of one", "a load into an empty dynamic array or queue does nothing", "a load into a dynamic array or a queue wakes the waiters of its elements", "a load into a dynamic array local to a task"), `tests/runtime_test.cpp` ("rt_readmem walks the words of a queue's ring from its head slot").

## 69. File addresses and word counts against the range a `$readmem*` names (21.4)

21.4: "When addressing information is specified both in the system task and in the data file, the
addresses in the data file shall be within the address range specified by the system task
arguments; otherwise, an error message is issued, and the load operation is terminated." Clotho
takes the range the task names as `start` through `finish` (either order), `start` through the
highest address of the array when only `start` is given, and all of the array when it names
no address. An `@` address of the file outside it is an error, located at the call
(`$readmemh: address 1 in 'f.hex' is outside the address range [2:5] of the load`), and ends
the load. "Terminated" is not a rollback: the words loaded before the address stay, wake the
array's waiters, and the rest of the file is not read. iverilog 13 does the same, for a
descending range, a descending declaration and an array that starts above 0 too.

"When slice syntax is used in the memory_name argument, any start_addr and finish_addr arguments
shall fall within the bounds of the slice's range". A `start` or `finish` outside the range of
a slice (`a[2:5]`, `a[i +: 3]`, a slice of a dynamic array) is an error, and nothing is loaded:
no file is opened. For a `start` or `finish` outside a whole memory or a row of a
multi-dimensional one the clause says nothing, and that load is unchanged: the words whose
address has no element are dropped, with a warning per address.

21.4 also asks for a warning when "the number of data words in the file differs from the number
of words in the range implied by the start through finish addresses and no address
specifications appear within the data file", the words the file holds being loaded from the
start address and the others left unmodified. Clotho warns of a file with fewer words once it
has read it (`$readmemh: 'f.hex' holds fewer words than the range [0:7]`), and of one with
more at the first extra word, which is ignored with the rest of the file. The second
is also issued when the file has addresses, where the clause asks for nothing and the extra
words have nowhere to go. A file with an `@` address, or a load that an error
ended, is not short of words. For a memory of several dimensions a word of the file is a
number, so the range holds one for each leaf of each of its rows (note 66); iverilog counts
the flat leaves and warns differently there.

iverilog 13 has no slice, so the slice tests take the whole-memory load with the same
addresses as oracle for the words placed, and derive the errors from the clause.

Code: `src/runtime/Io.cpp` (`rt_readmem`), `src/runtime/Reports.cpp` (`mem_load_report`), `src/ir/llvm/SystemTasks.cpp` (`lower_readmem`). Tests: `tests/llvm_backend_test.cpp` ("$readmem ends the load at a file address outside the range the task names", "$readmem into a slice with a start or finish outside its range loads nothing", "$readmem warns of a file with fewer or more words than its range unless it has addresses"). Oracle: iverilog 13 for the first and the third.

## 70. Associative arrays as the target of `$readmem*` and `$fread` (21.4.1, 21.3.4.4, 7.8, 21.4.3)

21.4.1: "When loading associative arrays, indices shall be of integral types. Loading an address
creates an element of that index, if it does not already exist. When an associative array's
index is of an enumerated type, address entries in the pattern file are in numeric format and
correspond to the numeric values associated with the elements of the enumerated type." So the
address of a word is the value of an index: `@h` names the element at index `h`, and the load
creates it. slang rejects a `$readmem*` of an array with a non-integral index; it does not
check a `$fread`, so Clotho does (an index with no integral value has no address, 21.3.4.4
gives it none).

The clause does not say where a load that names no address starts. 21.4 makes the default
start address "the lowest address in the memory", and an associative array has none: it is
sparse, and the lowest value of its index type is `-2147483648` for an `int`, where creating an
element would be absurd. Decided with the user: a load with no `start_addr` and no `@` before
its first number starts at **address 0** and counts up. With `start_addr` it starts there; with
`start_addr` and `finish_addr` it goes the way they order (down when `finish_addr` is below),
as for a memory. `$fread` has the same gap (21.3.4.4: "the lowest numbered location in the
memory shall be used") and takes the same reading. For an enumerated index, 0 is a number
whether it is a member or not; Clotho does not check that an address names a member.

The remaining choices are Clotho's, the clause being silent. The addresses are the values of
the index type: a signed index has the negative ones, and counting stops at its largest value
(2^W - 1 for an unsigned one), where a further number is the "more words than the range"
warning of note 69; an `@` address outside the index type is the error of that note. A
file's `@` address has no sign, so `@ff` is 255, outside a `byte` index, and not -1. The
tasks take `start`, `finish` and `@` as 64-bit numbers, so the addresses of a wider index
are its 64-bit values. A wildcard `[*]` index (7.8.1) is an unsigned one of 64 bits. As for
a memory, a file of fewer words than a bounded range (`start` and `finish` both given) is warned
of, and one of any length is not for an array whose range has no end.

An element is created by the first number loaded into it, so an empty file or an invalid target
creates none. One that is itself an unpacked array (`logic [7:0] aa [int][0:2]`) is a word of
21.4.3: the address names the element, whose leaves the numbers fill row-major, and a row the
data leaves unfinished keeps its other leaves (note 66). An entry that is itself an array or a
container (`aa[k]`, `aa[k][1:2]`, `nn[k]` of an `int nn[int][int]`) is loaded as the array
it is, and is created if it is not there (7.8.7: "the target of an assignment or actual to an
argument passed by reference"); an x or z key names no array, and nothing is loaded. An
associative array of associative arrays, dynamic arrays or queues as a whole is still refused
with the other arrays of containers (note 68).

A load that creates or changes an element wakes whoever waits on the array, as a store to it does
(9.4.2, 9.4.3), whatever the number of its elements; a load that changes none wakes nobody.

iverilog 13 cannot parse an associative array, so the tests take as oracle its load of the
fixed memory the same addresses name (the element at address `n` is word `n` of a memory
`[0:max]`, with the same file and the same start and finish), compared in the design, and derive
the rest from the clauses above.

Code: `src/runtime/Io.cpp` (`AssocWords`, `load_words`, `read_words`, `rt_readmem_assoc`, `rt_fread_assoc`), `src/ir/llvm/SystemTasks.cpp` (`resolve_mem_target`, `lower_readmem`, `lower_fread_mem`), `src/lower/SystemTasks.cpp` (`is_mem_target_shape`). Tests: `tests/llvm_backend_test.cpp` ("$readmemh into an associative array creates the elements the addresses name", "$readmemh into an associative array addresses the values of its index type", "$readmem into an associative array of 4-state, 2-state, wide and array elements", "$readmemh into an entry of an associative array of arrays or of associative arrays", "$readmemh into an associative array that is a property, a member, an element or an interface member", "a load into an associative array wakes the waiters of its elements", "$fread into an associative array creates the elements the bytes fill", "a memory load into an associative array that names no element or no file creates none", "a $fread into an associative array without an integral index is refused").

## 71. An x or z bit of an associative-array index that the cast to the index type drops (7.8.4, 7.8.6)

7.8.4: "The index expression shall be evaluated in terms of a cast to the index type" and "A
4-state index expression containing x or z is invalid". 7.8.6: a read through an invalid
index issues a warning and yields the value of a nonexistent entry (Table 7-1), and a write
through one "shall be ignored" with a warning. Clotho tests the index expression as written,
before the cast, so an x or z bit that the cast would drop still makes the index invalid:
`4'b1x01` indexing an `int` array is not entry 9, and a 64-bit `logic` with x in its high
half indexing an `int` array is not entry 0. The clause does not say whether a bit that only
the cast removes counts; this reading is the user's. A cast to a 4-state type at least as wide
as the expression keeps every bit, and the backend finds the key invalid as it always has.

The rule holds wherever an index names an entry: a read, a write, a read-modify-write
(`a[k]++`, `a[k] += 1`), an entry that is a queue, a struct or another associative array
(`aq[k].push_back(1)`, `as[k].x = 1`, `n[k][j]`), an actual of a `ref` argument (7.8.7
allocates the entry of an index that is valid), `exists` (0) and `delete` (nothing is
removed). The warning is the one every invalid index gets, under `--sanitize=bounds`
(docs/design/assoc-keys.md, P4b); `exists` and `delete` say nothing, as 7.9.2 has deleting a
missing entry issue no warning. `first`, `last`, `next` and `prev` take a variable of the
index type by `ref` (7.9.4-7.9.8) and are not changed.

iverilog 13 cannot parse an associative array, so the tests derive from the clauses above,
with a 4-state index of the type's own width, which was always invalid, as the control.

Code: `src/lower/Containers.cpp` (`lower_assoc_key`). Tests: `tests/associative_array_test.cpp` ("assoc invalid (x/z) index: the cast to a 2-state or narrower index type does not hide it", "assoc 4-state index without an x or z bit names the entry of its value through the cast", "assoc invalid (x/z) index: literals, queue and struct entries and nested arrays", "assoc invalid (x/z) index into a 2-state index type warns under --sanitize=bounds").

## 72. Bounds of a queue slice read as a value (7.10.1, 11.8.3)

7.10.1: "If either a or b are 4-state expressions containing x or z values, it yields the empty
queue {}", `Q[a:b]` with `a < 0` is `Q[0:b]` and with `b > $` is `Q[a:$]`. Each bound is compared
by its own signedness and width (11.8.3): an unsigned `bit [7:0]` holding 200 is 200, a `byte`
holding -2 is below 0. The same holds for `q[i +: n]` and `q[i -: n]` (an x or z bit in `i` or `n`
makes the slice empty). iverilog 13 gets an x bound (all the elements up to the other bound) and an
unsigned narrow bound (read as negative) wrong, so the test is derived from the clauses.

Code: `src/lower/SystemTasks.cpp` (`lower_queue_bounds`), `src/ir/llvm/Containers.cpp` (`lower_queue_append_range`). Tests: `tests/queue_test.cpp` ("queue slice read: x/z bound is empty and a bound keeps its own signedness").

## 73. `$readmem*` start or finish outside a whole memory (21.4)

21.4 has start and finish "fall within the bounds of the slice's range" when the memory is a
slice and is silent for a whole memory. Decided reading (with the user): a whole memory takes the
same rule by analogy. A start or finish outside its bounds, in either direction (`mem[0:7]` with
2, 10 or -1, 3 or 10, 2), is a located error, nothing is loaded and the simulation goes on
(`start address 10 is outside the memory range [0:7]`). This holds for a dynamic array or queue
(`[0:size-1]`) too. iverilog 13 agrees ("Start|Finish address N is out of bounds"). `$fread`
keeps note 55 (a start outside reads nothing and warns). Reading before this: the load went
on from the clipped address and loaded the words that fell inside.

Code: `src/runtime/Io.cpp` (`rt_readmem`). Tests: `tests/llvm_backend_test.cpp` ("$readmem with a start or finish outside a whole memory loads nothing").

## 74. `next` and `prev` with an x or z index argument (7.9.6, 7.9.7, 7.8.4, 7.8.6)

7.9.6 / 7.9.7 define `next` / `prev` by "the given index argument" and are silent on one with an
x or z bit; 7.8.4 makes a 4-state index with x or z invalid. Decided reading (with the user): it
is an invalid index (7.8.6): the call returns 0, leaves the argument unchanged and issues the
warning other invalid associative indices get (under `--sanitize=bounds`). It used to be read
as 0 (`next` found the entry after 0). `first` and `last` do not read their argument and are
unchanged.

Code: `src/ir/llvm/AssocArrays.cpp` (`lower_assoc_traverse`). Tests: `tests/associative_array_test.cpp` ("assoc next/prev with an x or z index argument finds nothing and warns").
