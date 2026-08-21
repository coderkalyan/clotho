# X semantics: simulation vs synthesis

Design study for stage S3 of `synthesis-and-lsp-scoping.md`. Scope: what to build so
Clotho can execute *both* the LRM's X semantics and synthesis's, and make the divergence
between them a measurable product.

Status: design. Not started. §7 is the proposal; §1-§6 are the grounding, and they changed
the proposal substantially from the one-flag sketch in `synthesis-and-lsp-scoping.md`
§3.3 — which was **wrong** in its central premise (§2.1) and **over-promised on
performance** (§4).

Normative quotes are from IEEE Std 1800-2023. Other claims are sourced; where a
conclusion is mine it says `[INFERENCE]`.

---

## 1. Clotho's baseline, measured against the LRM

Read out of the backend, not assumed. This is what `--x=lrm` must keep.

| construct | Clotho today | mechanism |
|---|---|---|
| `if (x)` | **else branch** | `lower_predicate` reduces with `RedOr`; `lower_cond` takes `must_of(cond) != 0`, and must⇒may makes X give must=0 |
| `case (x)`, no matching item | **default** (or holds, if no default) | `lower_switch` packs `must` + `(must^may)` into the key, so an X selector matches no 2-state arm |
| `case (x)` with an `x` item | **matches that item** | the same key encoding carries x-ness, so LRM case-equality works |
| `x ? a : b` | **bitwise merge** — must = a&b, may = a\|b | `lower_select`: `cond_known` selects, else per-bit interval hull |
| `posedge` on 0→x, x→1 | **fires** | `emit_edge_facts`: `pos = !old_must && new_may && (!old_may \|\| new_must)` |
| `posedge` on x→x, 1→x | does not fire | same expression |
| arithmetic with any x | **all bits x** | plane math |
| `0 & x`, `1 \| x` | **0**, **1** | controlling values honoured |
| `casez`/`casex` | wildcards per LRM | `CasezEq`/`CasexEq` |

§11.4.11 mandates the ternary merge: "If `cond_predicate` evaluates to an ambiguous value
(x or z), then both the first expression and the second expression shall be evaluated, and
compared for logical equivalence... if the `cond_predicate` evaluates to an ambiguous value
and the expressions are not logically equivalent, their results shall be combined bit by
bit using **Table 11-20**." (Table 11-20, "Ambiguous condition results for conditional
operator": agreeing bits pass, all else x. Aggregates fall to Table 7-1 instead.)

Corroborated: Icarus 13.0 gives `if=0110`, `ternary=01xx`, `case-with-default=1111` for
`a=4'b0101, b=4'b0110, sel=1'bx` — identical to Clotho.

### 1.1 The truth rule: three published statements, two of them wrong

This one rule is stated three times by the LRM's own technical editor, and the majority
verdict is false:

| source | claim | correct? |
|---|---|---|
| Sutherland 2005 | true iff "at least one bit of the expression must be a logic 1, **and none of the bits** a logic X or Z" | **no** |
| Sutherland & Mills, "Standard Gotchas" (2006) §6.10 | "If any bits of the expression is one, and (GOTCHA!) no bits are Z or X, then the expression is true"; "in order for a 4-state expression to be true or false, no bits in the expression can have a Z or X value" | **no** |
| Sutherland, DVCon 2013 | "true if any bit is a 1... the value `4'b010x` will evaluate as TRUE, hiding the X in the least-significant bit" | **yes** |

The wrong ones are the more emphatic: 6.10 is a numbered gotcha built around `4'b010z`, the
very example that disproves it.

**Measured on iverilog 13.0 and Clotho, identical output:**
`4'b010x` → true, `4'b010z` → true (6.10's own worked example), `4'b000x` → false,
`4'b0000` → false, `4'b0010` → true.

The sharpest datum is a ternary with the same predicate: `4'b010x ? 1 : 0` returns **1**. Had
the predicate been *ambiguous*, §11.4.11 would force both arms and merge 1 against 0 through
Table 11-20, giving x. It returns 1, so the predicate is definitely true — independent
confirmation through a different clause.

**And the rule is normatively grounded, just not in §12.4:**

- **§11.4.7** establishes a *three-way* classification, not the two-way one §12.4 implies:
  "An **ambiguous truth value** results in 1'bx", and "The result of the evaluation of a
  logical operation shall be 1'b1 (defined as true), 1'b0 (defined as false), or, if the
  result is ambiguous, the unknown value (1'bx)."
- **§11.4.9 / Table 11-17** (reduction unary OR) supplies the computation. Row 1 is all
  ones — `1|0`, `1|1`, `1|x`, `1|z` all give 1 — and the reduction "shall apply the operator
  between the first bit of the operand and the second... between the 1-bit result of the
  prior step and the next bit". So a single definite-1 bit forces the whole reduction to a
  definite 1.

Together: `4'b010x` does **not** have an ambiguous truth value, because bit 1 is definitely
1, so the condition is definitely true. Clotho's `RedOr` is right on normative grounds and
matches the iverilog 13.0 measurement above.

**What does not hold up is §12.4's own shortcut.** The clause offers "the following two
statements express the same logic: `if (expression)` / `if (expression != 0)`" — but `!=` is
X-pessimistic (§11.4.5: "The == and != operators may result in 1'bx if any of their operands
contains an x or z"), so `4'b010x != 0` yields x, hence false, contradicting the operative
rule. That sentence is informative and misleading; worth recording as a documented LRM
wart, but it is not an ambiguity in the semantics.

**Methodology consequence, and the reason this section is long.** Fluent, confident,
widely-cited EDA-semantics prose from the most authoritative possible author does not survive
execution here — twice, on the same rule, in the more emphatic direction. Clotho's semantics
are therefore pinned to normative clauses plus executable oracles, and secondary literature
is used for *motivation and history only*, never as a specification. Every construct in §1 is
a measurement, not a reading.

This also matters practically. Had we implemented the majority (2005/2006) reading, every
`if` on a partially-X vector would be classified X-controlled and would fire spuriously under
merge mode — a false-positive source on the very axis where §6.2 says no published baseline
exists.

### 1.2 The trigger condition is the LRM's own trichotomy

After `RedOr`, the reduced predicate's planes *are* §11.4.7's three buckets:

| planes | §11.4.7 term | action |
|---|---|---|
| `must != 0` | defined as **true** | take the then-branch |
| `may == 0` | defined as **false** | take the else-branch |
| `may & ~must` | **ambiguous truth value** | **the only X-controlled case** |

So `may & ~must` is not a convenient encoding choice — it is the direct mechanisation of the
standard's own classification, and it is already computed. That gives the trigger condition
the whole mode pivots on a normative footing rather than an implementation one. Real
conditions route through `FNe`-vs-zero and string conditions through `StrLen`+`RedOr`, both
definite, so neither can leak a false X-control.

### 1.3 A test-coverage gap, now closed

Three adjacent tests pinned the X-is-false direction, and **all three used 1-bit
conditions**: `case_test.cpp` "one-hot: x/z bit is not a definite 1" (`4'b010x`, but with
1-bit selects under `case (1'b1)`), "no definite-1 bit hits default" (`4'bxxxx`, 1-bit
selects), and `wait_test.cpp` "wait treats X as false" (1-bit `g = 1'bx`).

**Nothing asserted the definite-1-dominates direction on a multi-bit vector.** A reduction
implemented as "any bit unknown ⇒ X" — i.e. the 2005/2006 published rule — passes all three,
but would make every partially-X vector condition X-controlled and fire X-propagation reports spuriously.
Easy to introduce, hard to notice, and on the one axis with no published baseline.

Closed by `case_test.cpp` "a vector condition with a definite 1 bit is definitely true
(LRM 11.4.7, Table 11-17)", which pins both directions as a pair: `4'b010x` ⇒ true (definite
1 dominates), `4'b000x` ⇒ false (ambiguous truth value). The second line is also exactly the
case that *should* become X-controlled once merge mode exists, so the pair doubles as the
boundary spec for §7.

### 1.4 The asymmetry at the heart of everything

**The LRM specifies ternary-merge for `?:` (§11.4.11) and else-fallthrough for `if`
(§12.4).** Same predicate, same two candidates, two different answers.

That is not a wart to tolerate; it is the whole design space. Clotho's proposed `merge`
mode (§7.1) applies the ternary function to `if` and `case`: bits on which the two arms
agree pass through, and bits on which they differ go to X.

So `merge` needs no new combination rule. **It is the LRM's own §11.4.11 rule applied
uniformly to control flow.** We already implement the merge function, in `lower_select`.

**The merge is defined for integral results only.** A `real` or `string` arm has no
bits to disagree on, so §11.4.11's table cannot apply. Both fall back to the arms'
common value when they compare equal, and to the type's default (`0.0`, `""`) when
they differ — `lower_real_mux` and `lower_string_mux` in the backend. That is the
same answer slang's constant evaluator gives (`ConditionalExpression::evalImpl`
returns `type->getDefaultValue()` for a non-integral, non-array ambiguous ternary),
and it keeps "agreement passes through, disagreement degrades" as the single rule
across every value category. iverilog matches on `real` (`x ? 1.0 : 2.0` prints
`0.000000`) and rejects a string ternary outright; verilator instead takes the false
arm for a string, which is a plain LRM deviation, not a second reading.

---

## 2. Where simulation and synthesis actually differ

### 2.1 Synthesis has no X at all — the premise correction

`synthesis-and-lsp-scoping.md` §3.3 proposed "X resolves to 0, drop the may plane". That is
**not** what synthesis does, and building it would have modelled nothing real.

Turpin (ARM, 2003), §2.1 "Synthesis Semantics: Don't-Care": "An important point
to note is that all don't-care X's are resolved during minimization (so different target
libraries will not change this mapping)." He also notes there that "a single X-assignment
(e.g. default of a case) can in fact represent multiple don't-cares, each of which can be
assigned to different value[s]" — so the completion space is per-bit, not per-assignment.

(His often-quoted "in the case of a netlist there will be no don't-cares" is a
parenthetical in §4.2 about equivalence-checking comparison modes, not a
synthesis-semantics claim. Same fact, weaker provenance; §2.1 carries the argument on
its own.)

Synthesis does not *propagate* a don't-care; it **consumes** it. Each is bound once, during
Boolean minimisation, to whichever constant minimises better, and that binding is baked into
the netlist and is library-independent.

His §5.2/Table 3 makes the space concrete: two X's in one case default yield **four** legal
implementations of a single output, and "what if the don't-care is minimized to 1'b1 — if you
run netlist simulations you could fail a previously passing test!"

Three consequences, each invalidating part of the original sketch:

1. **X→0 is not synthesis-faithful.** It is one arbitrary completion, and a systematically
   biased one. Turpin notes X→1 is a *better* bug-finder, because active-high logic fires.
2. **Per-evaluation randomisation is wrong.** A netlist is a fixed function. Randomising per
   evaluation models a circuit that cannot exist and manufactures failures no silicon can
   produce.
3. **Bit-level X-propagation is also wrong** as a *synthesis* model. Synthesis never
   propagates a don't-care.

The faithful model: **bind each X-source once, at elaboration, to a constant; behave as that
constant thereafter.**

**`bind` is not novel, and the prior art is this same paper.** Its §6.1.2 proposes
exactly it: "A more realistic way of investigating the effects of synthesis minimization
with RTL simulation is to minimize the don't-care X's in the exact same way as your
synthesis tool (but without producing a netlist)". Verilator's `--x-assign unique` is the same model
mechanised.

So the semantics are 23-year-old received wisdom, independently reconstructed here —
reassuring about correctness, and it means §7.6 must not claim the *mode* as
differentiated. What is ours is that that version needs a synthesis run in the loop and
yields a separate artifact to simulate, whereas this is a mode of one engine: the
LRM-vs-`bind` differential runs from one elaboration in one process, which is what makes
the mismatch measurable rather than a two-tool workflow.

### 2.2 Three divergences, not one

| # | divergence | direction | measure it? |
|---|---|---|---|
| (a) | **RTL X-optimism** — RTL takes one branch where hardware could take either | hides **real** bugs | yes — the main target |
| (b) | **Minimisation binding** — RTL says X; the netlist says a constant minimisation chose | changes behaviour silently | yes — `bind` with N seeds samples the completion space |
| (c) | **Gate-sim X-pessimism** — netlist *simulation* says X where hardware is definite | **false alarms** | yes, but **we should not** |

(c) is an artefact of 4-state gate models losing correlation, not a property of hardware.
`assign b = a & ~a;` with `a=x`: hardware gives 0 always; gate sim gives X because the model
cannot see the inputs are correlated (Turpin; also Cobham Gaisler GRLIB-AN-0010 on the
tied-input XOR).

Yosys makes the judgment concrete: `opt_expr` rewrites `$xor` with equal bits to 0 and maps
x/z inputs of `$and`→0, `$or`→1, disabled only by `-keepdc`. So **synthesis is right here and
the gate simulator is wrong.** A synthesis-like mode follows the synthesiser.

**(a) and (c) are not symmetric and must never be reported as one number.** (a) is a
missed bug; (c) is a wasted afternoon — time spent debugging the cause of a pessimistic X
only to find there is no design problem at all.

### 2.3 Turpin's five tiers — the map worth keeping

1. **4-State** — Verilog simulation (Clotho today).
2. **Don't-Care** — synthesis. X is a licence to choose.
3. **2-State** — netlist simulation.
4. **2-State Consistency / Equality** — logic equivalence checking.
5. **2-State Sequential** — formal property checking. X as a free variable, all completions
   proven. *The gold standard.*

`--x=merge` approximates tier 5 dynamically along exercised paths; `--x=bind` samples tier
2/3 one completion at a time. Neither is tier 5. Saying so plainly is more useful than
implying otherwise.

### 2.4 Constructs where a synthesis-faithful mode changes behaviour

| construct | LRM sim | synthesis |
|---|---|---|
| `default: y = 'x;` | propagates X | don't-care; bound at minimisation |
| incomplete `case`, no directive | latch / holds | latch / holds — **agrees** |
| incomplete `case` + `full_case`/`unique` | holds or X | unspecified inputs become don't-cares |
| `casex` selector x | wildcard match | don't-care; bound at minimisation (§2.1) |
| `sel ? a : 'x` | X when !sel | mux undef input **deleted**; becomes `a` (Yosys `opt_expr -mux_undef`) |

### 2.4.1 The 2-state fullness rule

Clotho's fullness check judges a decision as synthesised logic sees it (§2.1), in two clauses:

- **Only 2-state selector values need covering.** Synthesised logic never sees an x or z
  selector, and under plain `case` an item with an x or z bit matches only an identical x
  or z (§12.5), so such an item covers no 2-state value.
- **A default assigned before the decision counts as full coverage.** If every variable
  the decision assigns is assigned **before** it, every path leaves each of them assigned,
  so no uncovered selector value needs storage.

That second clause is the same mechanism used for latch prevention, approached from the
synthesis side — so **one rule covers both the fullness check and the latch story**,
worth exploiting in S6 rather than implementing twice.

### 2.5 The trap: X-checking code dies silently

Once X is bound to a constant, `=== 1'bx` is always false and `$isunknown` always 0.
Verilator documents the consequence — "An === comparison to X will always be false, so that
Verilog code which checks for uninitialized logic will not fire" — and folds `$isunknown` to
constant 0 unconditionally.

So `--x=bind` silently disables exactly the RTL self-checks a user would rely on to detect
what the mode is modelling. **Clotho must diagnose `=== x` / `!== x` / `$isunknown` under
`bind`, not silently answer false.** A correctness-of-the-tool issue, not a nicety.

The standard has its own version of the hazard. §12.5.3, NOTE: "By specifying `unique` or
`priority`, it is not necessary to code a default case to trap unexpected case values." The
standard actively encourages dropping the X-trapping default, which is the main pure-RTL
X-detection hook.

---

## 3. The X-propagation design space

Clotho's two candidate semantics for an ambiguous condition, beside the LRM's and
Verilator's:

- **merge / ternary** — bits on which the two arms agree pass, differing bits → X.
- **force / pessimistic** — every variable the arms may write goes to X, whether or not
  they agree.
- **LRM** — the else branch, as §12.4 specifies.
- **value substitution** — Verilator: one frozen constant per X site per run.

### 3.1 What the instrumentation actually is

Not an operator-algebra change. Expressions already follow the LRM's 4-state operator
tables; `merge` targets the X-optimism of statements that pick one arm on an ambiguous
condition. So Clotho instruments **three constructs**: `if`, `case`, and the body of an
edge-triggered `always` (whose arms are: the body runs, or the state holds). The
proposed desugaring: when the condition is ambiguous (§1.2's `may & ~must`; §3.3 for
edges), report the site, execute every arm it allows (§3.2) on its own copy of the
state, and give every variable that any of those arms may write the merge of the arms'
results (`merge`) or X (`force`). An arm that does not write a variable contributes the
value it had before the statement.

Architecturally good news: `--x=merge` need not touch arithmetic.

### 3.2 Two details of the proposed semantics

- **Partially-unknown case selectors enumerate over the X bits, not over all arms.** The
  arms an X selector allows are those its 2-state completions select. Selector `2'b0X`
  against labels `00`/`01`/`default` completes to `00` or `01`, so `merge` and `force`
  both consider only those two arms, and **neither executes `default`**, which no
  completion reaches.
- **Branch merging leaves expression-level pessimism alone.** With `sel=x, a=b=1`,
  `assign out = (sel&a) | (~sel&b)` is X under the LRM's bitwise tables (§11.4.8) although
  both completions of `sel` give **1**; likewise `arr[x]` reads X under the LRM even when
  all elements agree. Neither has a branch to merge; folding them is a separate,
  expression-level repair (§7.4 item 5).

### 3.3 Edge semantics are the hard part

The body of an edge-triggered `always` runs on any listed event, and nothing in the syntax
says which one is the clock. Merging on *every* ambiguous edge is therefore
over-pessimistic: an X on an async reset would count as a possible clock edge and merge
captured D into a flop whose real alternatives are reset and hold. So flip-flop inference
is a prerequisite for edge X-prop rather than a later refinement.

**Clotho gets this for free from S6.** Storage inference gives clock / edge / async reset /
enable / next-state as a decomposition, which is exactly what edge X-prop needs. The
proposed semantics: `0→x` is a *possible* posedge (merge hold-Q with captured D), `1→x`
is no edge, and `x→1` is a definite posedge.

### 3.4 Scoping and escape hatches are mandatory

Non-synthesisable models must be exempt, so a mode without scoping is unusable: clock
generators, behavioural memories and CDC-jitter models are legitimately X-ridden. The
granularity needed is module / instance / subtree × on-off, plus a process attribute
(`(* clotho_x_off *)`, §7.1) for one-off exemptions. A scope specification lists only
exceptions: anything it does not name follows the mode flag.

### 3.5 Verilator, precisely

Per-run **and** per-site, never per-evaluation — established three ways: the source comment
("Replace 5'bx_1_x with a module global we init to a random value", plus an explicit
"OPTIMIZE: Must not collapse this initial back into the equation"), the generated C++
(`__Vxrand*` written only in the constructor), and runtime measurement (constant within a run,
varying by seed, reproducible on seed reuse).

Copy: the `--x-assign` / `--x-initial` split, and seed control with seed reporting. **Avoid
the footgun:** `--x-initial unique` is Verilator's compile-time default, but
`+verilator+rand+reset` defaults to 0 — so a default build randomises *nothing* unless the
user knows to pass a runtime plusarg. The feature is silently inert.

### 3.6 Formal, and a correction to my own assumption

I assumed "X is a free variable, all completions checked" for formal generally. Right for
**property checking**, which treats an X as a free variable; **wrong for equivalence checking**: an RTL X is a don't-care (§2.1),
so a netlist that realises *any* completion is correct, and demanding agreement on all of
them would reject legal netlists `[INFERENCE]`. And a comparison of combinational cones
cannot see an X held in a register.

---

## 4. Performance: the honest picture

**Clotho's own measurements do not support a speed argument for 2-state, and I should not
have made one.**

The shape is structural: the per-op saving is
real but small.

Clotho's data shows it is design-dependent and often small: `domain-spec` measured
**4.7× on `xdomain`** (an X-heavy microbenchmark) and **+11% regression on picorv32** — a
realistic core got *slower*, from boundary conversions.

Honest expectation for `--x=bind`:

- Storage genuinely shrinks 3× (`{i2W must|may, iW z}` → `{iW}`), which should relieve
  the dTLB and page-walk pressure that multi-MB state creates. That part is structural,
  not speculative.
- Compute savings are real per-op but small in aggregate. Expect single digits on
  realistic RTL; large wins only on X-heavy designs.
- `--x=merge` / `--x=force` are *slower*: speculative writes and per-bit tracking add work
  at every instrumented decision point, and speculation is in tension with our parallel
  scheduling, fusion and thunk paths (§7.2).
- A second-order cost that can dominate the first, in *simulated* time: where `lrm` takes
  the else branch and settles to a definite value, `merge` leaves the result X, so a
  design can need more simulated time, or more reset cycles, to reach a fully defined
  state `[INFERENCE]`.

**This is the second time in this project I have predicted a performance win from first
principles and been wrong** (the first was S1's walk consolidation, which measured as zero).
Rule adopted: performance claims get measured before they are written down. `--x=bind` is
justified by *semantics*, not speed; any speedup is a side effect.

---

## 5. Initialisation must be a separate axis

Not a detail — what a flop powers up to is decided by the target, and three kinds of
target differ in what they can promise:

| target | uninitialised flop powers up to |
|---|---|
| **declared-init target** | the **declared initial value**, if the RTL gives one |
| **fixed-constant target** | **0**, always; a declared 1 can only be faked, by inverting the flop's D and Q |
| **ASIC** | arbitrary but definite — `[INFERENCE]` |

A flag that silently picks one is indefensible for the other two. Verilator independently
reached the same conclusion and split `--x-assign` from `--x-initial` — corroboration that the
split is right rather than fussy.

---

## 6. Verdicts on received practice

### 6.1 The practices, judged

1. **"2-state sim with random init instead of X."** — **SITUATIONAL.** The speed premise
   does not hold (§4); random init samples one of 2^n states. Still the best *reset-bug*
   finder; useless against X-optimism from any other source.
2. **"Use `casex` for don't-cares."** — **BAD** as a bare construct. Wildcards apply to the
   *selector*, so one stray uninitialised bit silently matches the wrong item (Cummings:
   "borders on malpractice"). Mills' 2012 wording is narrower and fairer — "Do not use casex
   for RTL coding **without** other X-trapping monitoring". **The modern fix postdates every
   classic paper on this topic: SV-2009 `case ... inside` / the `==?` operator, whose masking
   is one-sided (RHS only), so an X in the case *expression* is no longer masked.** Clotho
   already implements both (`InsideSet`, `WildEq`), so it can recommend a remedy it can also
   execute.
3. **"Use `full_case` / `parallel_case`."** — **BAD.** They tell the synthesiser what the
   simulator cannot see. Cummings' sharpest line, and the one that most supports this project:
   **"The full_case and parallel_case directives are always most dangerous when they work!"** —
   divergence magnitude tracks optimisation benefit. The surviving legitimate use is narrower
   than "one-hot FSM": specifically the **inverse-case** style, `case (1'b1)` with `state[i]`
   as items, plus a latch-preventing default assignment before the case.
4. **"`unique`/`priority` are the safe replacement."** — **SITUATIONAL**, correcting an
   over-claim I had already written.
   - **They do not prevent latches.** The cause is incomplete *output* assignment, not
     incomplete case-item coverage, so a `unique case` **with** a default still infers latches
     if not every output is assigned on every branch. Mills: they "only illuminate, or warn
     against, SOME of the conditions that cause latches."
   - **Severity is unspecified by the standard.** §12.4.2/§12.5.3 never say "error" — only
     "violation report" and "a tool-specific violation report mechanism", with control
     delegated to the assertion control tasks of §20.11. Accellera's redline of the
     SV-3.1a text shows "shall be a run-time error" struck and replaced by "warning",
     and the current standard does not restore it. So this is a weaker oracle than
     "warning" — it is *whatever the implementation chooses*.
   - `unique0` is normatively exempt from the no-match check: "If the case is qualified as
     `unique0`, the implementation shall not issue a violation report if no case_item
     matches." And reports may be compile-time: "A violation report may be issued at compile
     time if it is possible then to determine the violation."
5. **"Initialise all RTL registers to 0 for determinism."** — **BAD.** Verifies one extreme,
   unlikely silicon state and hides bugs (an unconnected active-low input reads as asserted).
   SystemVerilog's own 4→2-state X→0 rule shares the defect.
6. **"Gate-level X-pessimism is a tool artifact to work around."** — **BAD FRAMING.**
   Individual instances are artifacts (`a & ~a`), but the aggregate is meaningful: force-mode
   X is the projection of the X behaviour of *any* gate implementation. Wrong to dismiss;
   wrong to chase as if it were hardware.
7. **"Assign X in the `case` default to catch illegal states."** — **SITUATIONAL.** The best
   pure-RTL trick available and it defeats X-latching — but it is swallowed downstream by
   X-optimistic `if`, adds false pessimism, and perturbs synthesis. Sound only paired with an
   assertion. Note §12.5.3's NOTE actively discourages it (§2.5).
8. **"GLS is how you catch X issues."** — **SITUATIONAL.** Complete GLS remains the only
   approach that catches *all* X-optimism bugs, but it needs a netlist, runs a fraction
   of the tests, and debugs a transformed view. RTL X-propagation (§3) can run on every
   RTL test before a netlist exists, so Clotho treats it as the first line and GLS as a
   residual check.

### 6.2 The number that should drive the design

**38,490 versus 16.** Piper/Vimjam (DVCon 2012) report 38,490 X-optimism hits on one bus
against 16 on the actual bug signals. A ~2400:1 noise ratio on a real bus makes such reports hard to act on, and **no source
reviewed quantifies the false-positive rate of X-propagation.** Baddam names the false-*negative* mechanism (self-gating),
Sutherland names the risk, Thatte documents a bug escaping X-prop entirely — but the
false-positive rate is unmeasured. That gap is the opening: the differentiated work is
not the semantics, it is **suppressing the 38,490**, and putting a number on it produces a
figure that does not currently exist.

---

## 7. Proposal

### 7.1 Two orthogonal axes

**`--x=<mode>` — what an X does.**

| mode | semantics | purpose | ship |
|---|---|---|---|
| `lrm` | today's behaviour | compliance; the default | **default** |
| `merge` | ternary merge extended to `if` / `case` / edge blocks | find X-optimism bugs | **yes** |
| `bind` | each X-source bound once per run to a constant | the only synthesis-faithful mode; sample completions across seeds | **yes** |
| `force` | any X control ⇒ X on every variable the arms may write | GLS triage — "would gate sim show an X here" | **later** |

`force` is deferred, not designed out: it is `merge`'s instrumentation with the merge function
replaced by constant-X, so it is a small addition on demand. Deferred because §2.2 argues it
models a gate-simulation artifact rather than hardware.

**`--x-init=<mode>` — where uninitialised state starts.** `x` (default, LRM) | `0` | `1` |
`rand`. Separate because §5's three targets cannot be reconciled. `rand` binds per flop per
run from the seed.

Both axes take a scope specification, because non-synthesisable models must be exempt (§3.4):
per-instance/module/tree plus an `(* clotho_x_off *)` process attribute.

### 7.2 What each mode costs to build

- **`bind`** is cheapest and most valuable. A frontend change: each X literal and each
  uninitialised 4-state signal becomes a per-run constant. Reuses the landed 2-state lowering
  (`two-state.md`) and the `narrow` slot path; needs **no** backend merge machinery; gets the
  3× storage reduction.
- **`merge`** is the expensive one. Its trigger is already computed — §1.2's `may & ~must` on
  the reduced predicate is exactly §11.4.7's ambiguous-truth-value case, and nothing else
  needs instrumenting. The cost is not detection but *effect*: merging `if` requires evaluating
  both arms and combining their **writes**, which in a structured-IR/coroutine model means a
  write log or shadow state, i.e. speculative execution. §3.1's desugaring is a usable
  specification; speculative execution is in tension with parallel scheduling, so it must
  be checked against comb-fusion and thunk elision, not bolted on.
- **Edge X-prop should wait for S6.** Getting it right needs the flop decomposition
  (§3.3). Doing it before S6 means merging on every ambiguous edge including async
  resets, which is over-pessimistic for the reason given there.

### 7.3 Free wins needing no new semantics

Worth shipping before, or independently of, any mode:

1. **Surface `unique`/`priority` violations as divergence findings.** Clotho already
   implements them. Sutherland: "These violation reports are
   important! They indicate that state values are occurring that the gate-level implementation
   will not decode." Tempered per §6.1(4): a *partial* oracle, of implementation-chosen
   severity, blind to latch inference.
   **This also upgrades a known gap from nice-to-have to spec-required.** Reports are
   deferred to the Observed region, but glitch-immunity maturation is
   unfinished. §12.5.3.1 is normative — "These violation checks **shall** be immune to false
   violation reports due to zero-delay glitches in the active region set" — and §12.4.2.1
   specifies the whole mechanism: evaluation at execution time, reporting "deferred until the
   Observed region"; "a pending violation report is scheduled... on a violation report queue
   associated with the currently executing process"; "If a violation report flush point is
   reached in a process, its violation report queue is cleared"; and "In the Observed region of
   each simulation time step, each pending violation report shall mature or be confirmed for
   reporting. Once a report matures, it shall no longer be flushed." Flush points are
   enumerated: resumption from an event control or `wait`, and resumption of an
   `always_comb`/`always_latch` on a dependent-signal transition. Until maturation lands,
   Observed-region deferral alone still admits false reports.
2. **Report the `if`-vs-`?:` asymmetry** (§1.4). A design whose author believes the two are
   interchangeable already diverges from the LRM, in the direction people do not expect.
   Statically detectable.
3. **`case` X-selector reachability.** Under `lrm` an X selector reaches `default`; under
   `bind` it never can. A static finding.
4. **Diagnose `=== x` / `$isunknown` under `bind`** (§2.5) rather than silently answering
   false.
5. **Recommend `case ... inside` / `==?` wherever `casex` appears** (§6.1(2)) — a fix Clotho
   already implements.
6. **Record §1.1's `!= 0` shortcut** as a documented LRM wart.

### 7.4 Attacking the 38,490

**First, a distinction that is easy to get wrong: static cleanliness proofs do not reduce
the report count.** If `may_x` clears a site, the runtime trigger (`may & ~must`) never
fires there, so it reports nothing whether instrumented or not — and the 38,490 are all
`may_x = true` by construction, since it is a may-analysis and every site where an X
actually arrives is in its set. Elision buys **instrumentation cost**, and it never stops
amplification, because a clean site had no arriving X to amplify. Noise and overhead are
disjoint problems and the following list is ordered accordingly.

**Report-count reducers, by leverage:**

1. **Root-cause attribution.** The largest win, and a *reporting-structure* fix rather than
   an analysis one. The 38,490 are one root cause — a handful of uninitialised registers or
   a reset gap — observed at 38,490 reachable decision points. Attribute each dynamic
   finding to the X-source it descended from, group, and report once per source with the
   fan-out as a multiplicity. Nothing is suppressed; the same information stops being
   printed 38,490 times.
2. **Scope exemption** (§3.4). Behavioural memories, clock generators, CDC-jitter models.
   Mandatory, not optional: without it the first behavioural model in the design buries
   every real finding.
3. **Time gating.** Suppress during the reset window, where the whole design is
   legitimately X. S6's storage inference says when reset releases.
4. **`observable` backward closure.** The one analysis here that genuinely removes reports,
   and it is the *opposite direction* to `may_x`: forward reachability asks "can an X get
   here", backward closure asks "if this branch had gone the other way, would anything
   visible differ". A site where X arrives but the divergence dies before any output,
   `$display` or assertion is a real don't-care. Kills findings `may_x` cannot.
5. **X-pessimism repair** (§2.2c). Folding `a & ~a`, `(sel&a)|(~sel&b)` and `arr[x]`
   all-agree means fewer X's *exist* — removing both the report and everything it would
   have amplified into. Precision, not elision. The rationale generalises: an X index
   whose candidate elements all agree has no observable ambiguity, so reporting it is
   pure noise.
6. **Write-set precision.** §3.1's desugaring gives every variable the ambiguous
   statement may write a merged or X value, so the width of that write set bounds how
   much X is *injected* and then amplified. `body_writes` already exists in `DesignFacts`.
7. **Control-vs-data weighting.** Turpin's `CountEnable`: an X reaching a clock enable or
   FSM next-state decides behaviour, one reaching a datum only corrupts a value. Different
   severities. `[INFERENCE]` — the weighting is mine, the hazard is his.

**Overhead reducers, which fund the above:** use `DesignFacts` to elide instrumentation at
`may_x`-clean sites. Merge-mode instrumentation is expensive (speculative writes, per-bit
tracking), so eliding the provably-clean majority is what makes items 5 and 6 affordable on
what remains. That is the real justification for elision — a budget argument, second-order
to noise, not a noise reducer itself.

**Neither, but required: report coverage of the check.** Publish an
instrumented/instrumentable ratio and tell the user to investigate anything below 100%.
A checker silently covering 40% of a design is worse than none, because it is trusted.
This is an honesty mechanism, not a noise or cost one.

### 7.5 How we verify it

- **`merge` against Yosys `xprop`.** The find that de-risks the whole mode: Yosys has a real,
  ternary-*exact* X-propagation pass; it is open, local and SAT-queryable. Verified:
  `sel?a:b` with `sel=x, a=b=1` gives defined `1`; `a&b` with `a=x, b=0` gives defined `0`.
  **An executable oracle for merge semantics that we can run locally.**
- **`bind` against itself across seeds.** N seeds are N legal completions, and a
  disagreement between seeds is itself the finding. N is a knob, so the cost is bounded by
  choice rather than combinatorics.
- **`lrm` against Icarus**, as today. Already matches on every construct in §1.
- **Oracle caveat for the `unique`/`priority` work:** iverilog implements only the no-match
  check and explicitly ignores overlap ("Case unique/unique0 qualities are ignored"), so it is
  **not** a valid oracle for the overlap half. No executable reference for that half is
  available to us, so §12.5.3 is the reference and our own tests are the pin.
- **The mode matrix must agree where it provably must.** If `may_x` proves a signal never-X,
  all modes must produce byte-identical results for it. A strong, cheap, whole-corpus
  invariant — the same shape as the `--no-z-elision` differential.

### 7.6 What is genuinely differentiated

1. **Not the `bind` semantics.** Turpin proposed them in 2003 (§2.1) and Verilator ships the
   model. What the tools reviewed lack is a *simulator* that offers them as a mode: Verilator
   substitutes values with no X representation at all, Icarus is strict LRM with no mode
   switch, CXXRTL has no X representation, and Yosys `xprop` is a netlist transform for
   formal rather than a simulation mode. So the claim is "first open-source simulator with
   selectable synthesis-faithful X semantics", not "first synthesis-faithful X semantics".
2. **All modes from one engine, so the divergence is computable.** Clotho can run `lrm`
   and `bind` from one elaboration in one process and report where they differ, instead of
   leaving the user to diff two waveform dumps. Only cheap because the frontend and cache
   are shared.
3. **No reviewed source quantifies X-prop's false-positive rate** (§6.2). We can measure and publish
   it, and §7.4 is the plan to reduce it.
4. **`bind` is faithful to what synthesis actually does** (§2.1), where `merge` is
   neither LRM (it departs from §12.4) nor synthesis (it models no single netlist). The
   mismatch `bind` models is real: in RTL an X is a don't-care to synthesis (§2.1) but an
   unknown value to simulation (§6.3.1) — and that gap can hide bugs that show up in
   silicon.
5. **S6 synergy.** Storage inference gives correct edge X-prop by construction, rather
   than as a retrofit.

### 7.7 Open questions

- **Does `merge` need speculative writes, or is detect-only enough?** If the goal is finding
  divergence rather than propagating it, we may only need to detect that the arms *differ* —
  still evaluating both, but never committing or merging. Cheaper, and better diagnostics.
  Decide before implementing.
- **Does `merge` compose with comb-fusion and thunk elision?** Speculative writes and
  parallel scheduling are in tension. Ours must compose or refuse, explicitly.
- **Is `force` worth it?** §2.2 argues it models a gate-simulation artifact rather than
  hardware. Deferred; revisit with a user.

### 7.8 Sequencing within S3

1. `--x-init` axis + `bind` mode (frontend; reuses landed 2-state lowering).
2. The §7.3 free wins.
3. Divergence report + seed discipline; measure the false-positive rate.
4. `merge` — after S6, verified against Yosys `xprop`.
5. `force` — only on demand.

Write the X-resolution table for every LRM X-producing construct (div/mod by zero, OOB select,
uninitialised, contention, x/z literals) and record the deviation before any of it
lands. Every mode except `lrm` is a deliberate, gated deviation, which the project permits only
behind a non-default flag — and all of these are.
