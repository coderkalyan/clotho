# The SVA property layer (LRM 16.12) — scoping

What is left of concurrent assertions after the sequence layer, and what it costs.

Status: **scoping. Nothing implemented.** Every operator below is refused today
with `this property operator is not supported yet`. The sequence layer (LRM 16.9)
is complete, with fixtures under `tests/sva/` covering it; see
`concurrent-assertions.md` for the architecture this builds on.

This is the largest remaining piece of SVA, and unlike the sequence layer it is
not one job. It splits along a line that is not obvious from the LRM's own
grouping, and getting that split right is most of the value of this document.

---

## 1. The inventory

From `slang/ast/expressions/AssertionExpr.h`, the operators clotho does not
lower. Unary (`UnaryAssertionExpr`, `:437`), each carrying an
`std::optional<SequenceRange> range` (`:446`):

| operator | with a range | without |
|---|---|---|
| `nexttime` / `s_nexttime` | `nexttime[n] p` | `nexttime p` = `nexttime[1] p` |
| `always` / `s_always` | `always [m:n] p` | `always p` |
| `eventually` / `s_eventually` | `eventually [m:n] p` | `s_eventually p` |
| `not` | — | `not p` |

Binary (`BinaryAssertionExpr`, `:474`): `until`, `s_until`, `until_with`,
`s_until_with`, `implies`, `iff`, `#-#` (`OverlappedFollowedBy`), `#=#`
(`NonOverlappedFollowedBy`).

Plus three node kinds: `AbortAssertionExpr` (`:629` — `accept_on`, `reject_on`,
and their `sync_` forms, distinguished by `isSync`), `ConditionalAssertionExpr`
(`:668` — the `if`/`else` property form) and `CaseAssertionExpr` (`:706`).

libsva's property layer is four tags: `prop_seq_weak`, `prop_seq_strong`,
`prop_implies_overlap`, `prop_implies_nonoverlap`. Everything above needs
library work as well as frontend work.

---

## 2. The line that matters: bounded versus unbounded

libsva's `Monitor` tracks an obligation as a **ring of `depth` state vectors**,
where `depth` is the obligation's longest match. That is exact rather than
heuristic: an attempt seeded in cycle `t` has matched or died by `t + depth - 1`,
so a free slot always exists (`../libsva/src/Monitor.zig:19-25`). An obligation
with no bounded longest match is rejected outright with
`SVA_ERROR_UNBOUNDED_OBLIGATION`.

That model is a perfect fit for a *sequence* obligation and a poor fit for half
the property layer, because a property like `always p` never completes. It is
also why the split is not the LRM's:

**Tier A — bounded. Fits the existing architecture.**
`nexttime[n]`, `s_nexttime[n]`, `always [m:n]`, `s_always [m:n]`,
`eventually [m:n]`, `s_eventually [m:n]`, `#-#`, `#=#`, the `if`/`else` and
`case` forms, and property-level `and` / `or`. Each has a finite horizon, so each
compiles to an obligation with a computable longest match and the existing ring
tracks it unchanged.

**Tier B — unbounded. Needs a second tracking discipline.**
`always p`, `s_eventually p`, and the four `until` forms. These have no longest
match by construction.

**Cross-cutting — `not`, and hence `implies` and `iff`.** Negation is not a
horizon question; §5.

The practical consequence is that **Tier A is most of the operator count and
almost none of the risk**, and it can land without touching `Monitor`'s tracking
model at all. A scoping that treats "the property layer" as one project
overstates the cost by a wide margin.

---

## 3. Why Tier B needs a different discipline, and what it is

The ring exists for a specific reason: merging two sequence attempts into one
state vector is unsound, because one attempt's threads can die while another's
live and the union hides the failure
(`../libsva/src/Monitor.zig:8-17`). That argument is about *sequence* attempts
with distinct live sets.

It does not apply to an unbounded safety property. `always p` is not a set of
overlapping attempts; it is one claim about every cycle so far, and its state is
a single bit. `p until q` is likewise one pending claim per start, and — this is
the load-bearing part — **two attempts of `p until q` started at different cycles
that are both still pending are in the same state**, because the property's
future depends only on future `p` and `q`. So merging *is* sound here, and one
state vector suffices.

That is the standard observation behind LTL-to-automaton translation: the
property's obligation is characterised by a formula, and equal formulas merge.
The concrete shapes:

| property | state | fails when | at end of trace |
|---|---|---|---|
| `always p` | none beyond `p`'s own monitor | `p` fails in any cycle | holds |
| `p until q` | one bit: "still waiting for q" | `p` fails while waiting | weak: holds. strong (`s_until`): fails |
| `s_eventually p` | one bit: "not yet seen" | never during the trace | fails if never seen |

So Tier B is not "make the ring unbounded" — it is "add a mode where the
obligation is a single persistent state vector with different verdict rules".
That is a smaller change to `Monitor` than it sounds, and it is additive: a
monitor is either ring-tracked (today) or single-state (new), decided at lowering
time by whether the obligation is bounded.

**MEASURED, and the answer is "half of it".** `libsva/test/merge.zig`, run with
`zig build merge`, tracks one obligation two ways over every trace of 6 and 8
cycles over 2 atoms: a vector per attempt (the ring, unbounded), and one merged
vector seeded every cycle. It reports whether the two agree on the cycles a
failure lands in, on the cycles a match lands in, and on how many attempts
resolved in each. Three results, and they redraw this section:

1. **Merging is unsound in general, and the counterexample is two cycles long.**
   For `a ##1 b` on `a=1,b=0` twice then quiet, the attempt started in cycle 0
   fails in cycle 1 and the merged vector says nothing, because the attempt
   started in cycle 1 has just seeded the same state. Union hides death exactly
   as `Monitor.zig:14-17` says. This is not an unbounded-obligation problem: the
   shape is bounded and ring-tracked today.
2. **For the Tier B shapes themselves, merging preserves the cycle sets and
   loses the counts.** `always a`, `a until_with b`, `s_eventually b` and
   `b[->1]` all agree with per-attempt tracking on *when* a verdict lands and
   disagree on *how many* attempts it belongs to — up to 8 in an 8-cycle trace.
   Since "sva: one verdict per attempt" clotho reports one verdict per attempt — a verdict belongs to
   an attempt, as `concurrent-assertions.md` §13 records — so the count is
   observable and the loss is a wrong answer, not an approximation.
3. **Determinising as libsva determinises today does NOT fix it.** Wrapping each
   shape in `first_match` — which the same run confirms is verdict-preserving
   for an obligation, since a monitor resolves an attempt at its first match
   anyway — leaves every unsound case unsound. `Monitor.zig:22-25` says sound
   tracking "needs the obligation determinised"; that is not sufficient as
   written. The missing half is **totality**: `first_match`'s construction drops
   an edge when a subset is empty, so a dead attempt still has no state to be
   seen in. A trap state is what makes death observable, and it is the thing to
   build.

**What is exact, and what it costs.** The same harness runs a third discipline:
a total subset construction where each live subset carries a COUNT of the
attempts sitting in it. It reproduces per-attempt tracking exactly — cycle sets,
counts, and the number pending at end of trace — for every shape, bounded and
unbounded alike. Its cost splits in a way that decides the data structure:

- the number of counters is small and **does not grow with the trace**: 0, 1 or
  2 for every shape here, identical at 6 and at 8 cycles. It is a property of the
  determinised automaton, which is what `Options.limit` already bounds.
- the multiplicity inside a counter **does** grow with the trace: 5 at 6 cycles
  and 7 at 8 for `s_eventually`, `until_with` and `##[1:$]`. So a slot carries an
  integer, not a bit, and phase 3 needs a saturating width decision.

So Tier B is not "one persistent state vector with different verdict rules". It
is **a bounded vector of (determinised state, attempt count) slots**, which
subsumes the ring — the ring is this with the count pinned at one and the states
left nondeterministic — and it is the same construction phase 4 needs. The table
above is still the right description of the three properties' *verdicts*; it is
the state column that was wrong.

---

## 4. Tier A in detail

Most of Tier A desugars onto what exists, which is worth checking before adding
tags:

- `nexttime p` ≡ `##1 p`, and `nexttime[n] p` ≡ `##n p`. If `p` is a sequence
  this is already expressible; if `p` is a property it needs the delay to apply
  at the property layer, which today it cannot. A `prop_delay` tag is the
  cheapest addition in the whole layer.
- `always [m:n] p` ≡ the conjunction of `p` at each offset in `[m:n]`, i.e.
  `nexttime[m] p and … and nexttime[n] p`. Needs property-level `and`.
- `eventually [m:n] p` ≡ the same with `or`.
- `s #-# p` ("followed by") is the dual of implication: `s` matches **and** `p`
  holds from its end. `#=#` is the non-overlapping form. Structurally these are
  the implication construction with the verdict inverted, so they are close to
  free given `prop_implies_*` exists.
- `if (c) p else q` and the `case` form are guarded selection between properties.
  With property-level `and`/`or` and a boolean guard they desugar; a dedicated
  tag would only save states.

So Tier A plausibly reduces to **three new tags** — property-level `and`, `or`
and delay — plus desugaring in clotho. That is a much smaller library change than
the operator count suggests, and the desugarings are worth measuring against
dedicated tags before committing to either (a desugared `always [1:100] p` is a
100-way conjunction).

**AS BUILT (libsva `c3bc468`, `format_version = 7`): four parameterised tags, not
three.** The prediction above was right about the count being small and wrong
about the factoring, in three places:

1. **The delay tag became a range tag, and it absorbed `always` and `eventually`
   outright.** `sva_prop_shift(kind, lo, hi, p)` is one construction covering
   `nexttime[n]` (`lo == hi`), `eventually [m:n]` (`SVA_SHIFT_SOME`, the range read
   disjunctively) and `always [m:n]` (`SVA_SHIFT_EVERY`, read conjunctively). So
   the two operators this section expected to desugar onto property-level `and`
   and `or` never do: the range is a parameter of one tag rather than an n-way
   junction built in the front end.
2. **`and` and `or` became one tag, not two.** `sva_prop_junction(kind, …)`.
3. **The followed-bys needed a tag after all.** They are *not* "the implication
   construction with the verdict inverted": LRM 16.12.9's `not (s |-> not p)`
   turns a universal over antecedent matches into an existential, so the
   antecedent joins the OBLIGATION and never becomes a trigger.
   `sva_prop_followed_by` is a different construction, not a flag on
   `prop_implies_*`.

The prediction about `if`/`case` held: they need no tag. The shape is a
disjunction of **followed-bys** and not of implications, which this section did
not pin down and which is the whole of why it works — see §11.

The measurement this section asked for was taken, and it settles the
`always [1:100]` worry: a bounded `always` costs states **linear** in the width of
the range, not exponential, because two conjuncts only have a reachable product
where their copies of `p` are live at once. That window is `|p|` cycles wide
however wide the range is. The dedicated tag therefore earns its place by keeping
the semantics and the refusals in one place, not by beating a desugaring on size —
it produces exactly the state counts the sequence-layer spelling does.

---

## 5. Negation, and why it is the hard one

`not p` cannot be desugared and does not fit either tier cleanly.

The problem is that an automaton-based monitor answers "did this match", and
negation needs "did this *not* match" — a complement. For a nondeterministic
automaton that requires determinisation. libsva now has determinisation
machinery: `first_match` is implemented as subset construction keyed by
`(subset, cube)`, cutting the valuation space into cells so the result stays
homogeneous (`../libsva/README.md`, and the commit that added it). That is a real
precedent and it is the reason `not` is now plausible rather than blocked.

Three complications beyond the construction itself:

1. **Weak/strong duality.** LRM 16.12.2: a pending weak property holds at end of
   trace and a strong one fails. Negation must swap them, so `not` cannot be a
   pure automaton operation — it also inverts the strength the monitor carries.
2. **Three-valued verdicts.** While a property is pending it is neither true nor
   false. Negating "pending" is "pending", so the monitor's verdict lattice has
   to be explicit about it. Today the ring encodes this implicitly (occupied
   means pending); a negation-aware model needs it named.
3. **Cost.** Determinisation is exponential in the worst case. `first_match`
   caps it with `Options.limit` and reports a diagnostic naming the node; `not`
   should reuse exactly that mechanism rather than inventing a second one.

`implies` and `iff` both reduce to `not` plus `or`, so all three land together or
not at all.

---

## 6. The abort operators

`accept_on(b) p` / `reject_on(b) p` and their `sync_` variants are not temporal
operators — they are *resets* scoped to one property. `disable iff` is already
exactly this mechanism at the assertion level (`sva_monitor_reset`, evaluated
from live values per LRM 16.15), so the machinery exists and the question is
scoping it to a subtree rather than the whole monitor.

The `sync_` forms sample the condition; the plain forms do not, mirroring
`disable iff`. `accept_on` additionally *discharges* rather than abandons, which
the reset path does not currently distinguish.

**CORRECTION — the sampling sentence above is wrong on the LRM.** 16.12.14 puts
*both* pairs on sampled values: "The operators accept_on and reject_on are
evaluated at the granularity of the SIMULATION TIME STEP like disable iff, but
their abort condition is evaluated USING SAMPLED VALUE", and the `sync_` forms
read "sampled value AS FOR accept_on and reject_on". `disable iff` is the one that
reads live values (16.15). The async/sync split is only about *when* the sampled
condition is looked at: at every time step, or only at those carrying a clocking
event. `tests/sva/diff_abort.sv` is deliberately built to stay off the corner
where that distinction is observable.

The `discharges rather than abandons` half was right, and libsva draws that
distinction: `sva_monitor_abort_effect` reports accept versus reject, where an
accept is a pass a `cover` counts and a reset leaves no verdict at all.

**What was built, and which reading it follows.** clotho reads the plain forms
LIVE and the `sync_` forms SAMPLED — the original sentence above, not the
CORRECTION's literal LRM reading. That is a deliberate choice against the letter
of 16.12.14, and it is made on grounds that do not depend on the letter: the
plain forms are the asynchronous pair, and 16.12.14 itself ties their
granularity to `disable iff`, which 16.15 reads live. Reading their condition
live keeps the one asynchronous abort channel consistent, rather than giving two
constructs that share a granularity two different notions of "now".

Mechanically the plain forms reuse `disable iff`'s live-value path exactly rather
than adding a second one, which is what `Lower.cpp`'s `root_reads_live` names.
One thing the reset path did not need: a live condition can go high and fall
again strictly BETWEEN two ticks, and the monitor only acts on an edge, so each
asynchronous abort carries a one-bit latch (`MonitorState::abort_seen`) set at any
time slot the condition holds and consumed at the next edge. That makes the
condition a property of the whole cycle rather than of its final instant, which is
what lets `diff_abort.sv`'s `abx` pulse discharge an attempt at all.

The earlier **[INFERENCE]** that this was the second-cheapest group after Tier A
held up: no change to the tracking model, no new per-attempt state, and the one
new piece of monitor state is the latch above.

---

## 7. What clotho needs

Very little, which is the pleasant part. The monitor tables are consumed
generically: the automaton step is emitted as ordinary IR from
`ir::AssertionMonitor`, with no new opcode and no backend involvement
(`concurrent-assertions.md` §7). Adding property operators changes what libsva
puts in those tables, not how clotho reads them.

The exceptions:

- A Tier B monitor is single-state rather than ring-tracked, so
  `ir::AssertionMonitor` needs a mode flag and the emitted step needs the
  corresponding branch. Small, and localised to the monitor emission.
- `not` changes the verdict lattice, so the report path needs to know a property
  can fail without the obligation emptying.
- The desugarings in §4 are clotho-side work if they are done in clotho rather
  than in libsva. Prefer libsva: the sequence layer's experience was that
  desugaring in the frontend produces worse automata than a direct construction
  (`throughout` was +0 states as a tag), and clotho cannot measure state counts.

---

## 8. Sequencing

Each phase ships operators a user can write.

**Phase 1 — Tier A desugarings. DONE.** In libsva as `sva_prop_shift`,
`sva_prop_junction` and `sva_prop_followed_by` (see §4 for how the tag count
landed at four rather than three); in clotho as arms in `Lowering::property`.
What a user can write today: `nexttime`, `nexttime[n]`, `s_nexttime[n]`,
`always [m:n]`, `s_always [m:n]`, `eventually [m:n]`, `s_eventually [m:n]`,
`#-#`, `#=#`, property-level `and`/`or`, and the `if`/`else` and `case` forms.
No change to the tracking model, as predicted.

Each has a fixture under `tests/sva/` exercising it: `diff_nexttime.sv`,
`diff_alwaysrange.sv`, `diff_eventuallyrange.sv`, `diff_followedby.sv`,
`diff_propif.sv`, `diff_strongprop.sv` and `diff_propcase.sv`.

Two things worth recording because they are not obvious from the operator list:

- **`and`/`or` are spelled identically at both layers and slang builds one node
  kind for both**, so the operands decide which builder runs, not the operator.
  Two sequences keep the sequence-layer builder — one automaton, and what LRM 16.12
  prescribes for that spelling; a property operand on either side switches to
  `sva_prop_junction`. `Lowering::is_property_expr` is that decision.
- **The guarded selection forms are a disjunction of FOLLOWED-BYS, not of
  implications** — see §11.

**Phase 2 — the abort operators. DONE.** All four of `accept_on`, `reject_on`,
`sync_accept_on` and `sync_reject_on` lower, including the shape essentially every
real use writes: the abort ABOVE an implication. `tests/sva/diff_abort.sv` pins
all nine reports (`FAIL 25` x4, `45` x3, `75` x2, nothing at 105).

Both blockers this section used to record are gone, and the shape of each fix is
worth keeping.

*What was clotho's:* `ir::AssertionMonitor` now carries an `aborts` table
(`AssertionAbort`: root, effect, sampled, resolves_attempt, covers_trigger, mask),
copied out of the lowered monitor in `Sva.cpp` from libsva's `sva_monitor_abort_*`
read-out, and the emitted step has the arms for it. It stays plain data, so
`clotho_ir_core` is still libsva-free.

*What was libsva's:* `lowerAbort` used to reject any operand whose plan carried a
trigger. It now records whether an abort's scope reaches the antecedent
(`sva_monitor_abort_covers_trigger`) and carries it through. The earlier
**[INFERENCE]** that the restriction was incidental rather than fundamental was
right, and for the reason given: a monitor has at most one trigger, every operator
that could put an abort beside a second one refuses a trigger-carrying operand
already, so an abort above a trigger covers *all* of its attempts uniformly and
nothing has to tell them apart.

**Pushing the abort down is still not an equivalent rewrite** — that is why the
support was worth building rather than desugaring around. Below the implication
the abort only arms once the consequent starts, one cycle after the attempt begins
for `|=>`; above it, the abort covers the attempt's own cycle too. `diff_abort.sv`
is built on exactly that difference, and its expected output was fixed before
clotho could lower any of this rather than back-filled from a passing run.

Three things about the emitted step that the ABI alone does not tell you:

- **A trigger-covering abort resolves at MATCH time, not seed time.** The cycle
  the antecedent match ends, which for `|=>` is one cycle BEFORE the obligation
  would be seeded. Reading the condition at seed time looks one cycle late and
  loses the report entirely — `diff_abort.sv`'s t=75 is exactly that cycle, and
  the attempt has to resolve in its own start cycle. The match is consumed: it
  takes the abort's verdict and `fires` is forced low, which suppresses the seed
  for `|->` and `|=>` at once. An attempt that has already been seeded is the
  ordinary mask case and needs none of this.
- **Where two aborts fire on one attempt in the same cycle, the OUTERMOST wins**
  (`accept_on(a) (reject_on(b) p)` with both high holds). Outermost is LAST in the
  monitor's abort list, because lowering appends outward, so both arms scan the
  list backwards.
- **A resolved attempt's antecedent threads ARE cleared**, and they have to be, or
  an already-resolved attempt could complete its antecedent later and impose an
  obligation that then fails. Grouped, dropping the resolved ids out of the
  per-state id sets does it and the trigger vector is then pruned to the states
  that still hold one; a counted monitor has no such fold and clears outright.

*The residual refusal that used to be here is gone.* An abort covering an
implication whose antecedent takes MORE THAN ONE CYCLE to match used to be refused
with `SVA_ERROR_UNSUPPORTED_ABORT_SCOPE`, because several attempts can sit part-way
through such an antecedent when the condition fires, each owing its own verdict,
and a union-tracked trigger cannot count them. The per-attempt ids count them, so
it lowers: the abort's scope is the union of the id sets over ALL trigger states
rather than over the exits, and every attempt in it takes the abort's verdict —
one each, accept and reject alike. The ids came from the per-attempt verdict
rule (§3); this lift is a consequence of that counting rather than a change of
its own.

Not distinguished, and deliberately: an abort landing on an attempt whose
antecedent does NOT match resolves vacuously in the same tick. The LRM gives no
answer on whether that vacuous resolution is reportable, so clotho reports only
where the antecedent match completes — the rule the rest of the layer already
follows, and the one that keeps every report tied to an attempt that actually
got under way (`diff_abort.sv`'s D2 is that shape).

**Phase 3 — Tier B, the counted-subset discipline.** `always`, `s_eventually`,
and the four `until` forms. This is the architecture change: obligation tracking
becomes a bounded vector of (determinised state, attempt count) slots, a mode
flag on `ir::AssertionMonitor` says which discipline a monitor uses, and clotho's
emitted step gains the matching branch. §3 records the measurement that settled
the shape — a single merged vector is NOT enough, and `first_match`'s
determinisation is not enough either, because it is partial.

**Phase 4 — `not`, and with it `implies` and `iff`.** The complement of the same
total determinisation, plus the weak/strong swap and an explicit three-valued
verdict.

**Phases 3 and 4 share their prerequisite and should be sequenced as one
project**, which is a change from what this section used to say. Both need a
determinisation that is TOTAL: phase 3 so a dead attempt occupies a trap state
the merged tracker can see, phase 4 so a complement accepts every continuation.
`first_match`'s `Subsets` / `Determinisation` / `firstMatchCells` / `reach`
(`Glushkov.zig:1736-1946`) are reusable verbatim; its driver
(`lowerFirstMatch:1986`) is not, because it drops empty-subset edges and
truncates successors at accepting states. Build that once, then phase 3 and
phase 4 land on it in either order — phase 4 still last, since it is the only one
that changes what a verdict *is*.

Phases 1 and 2 are independent of both and are done.

---

## 9. Verification

The bar is unchanged and it is not negotiable: every operator needs a fixture
under `tests/sva/` that pins its reports, plus libsva-side differential testing
against a direct recursive semantics and a mutation sweep.

Two specific hazards this layer introduces that the sequence layer did not:

- **Vacuous fixtures.** `first_match` taught us that an operator can be provably
  a no-op in the position a fixture naturally puts it in. Property operators are
  worse for this: `always p` in a design where `p` never fails is
  indistinguishable from no assertion at all. Every fixture must produce both a
  pass and a failure, and the failure must be attributable to the operator.
- **End-of-trace behaviour is where weak and strong differ**, and it is the
  easiest thing to get wrong and the easiest to leave untested, because a fixture
  that always resolves before `$finish` never exercises it. Every Tier B and
  every `not` fixture needs a case still pending when the trace ends.

~~Before Phase 3, verify the §3 merging claim directly.~~ **Done —
`libsva/test/merge.zig`, `zig build merge`, results in §3.** It went further than
this paragraph asked, because the first answer was "unsound" rather than a clean
yes or no: it measures three disciplines rather than two, over every trace of 6
and 8 cycles, and reports cycle sets, per-cycle counts and end-of-trace pendency
separately. The last of those is what turned "merging loses nothing for `until`"
into "merging loses the attempt count for `until`", which is a wrong answer under
the per-attempt reporting rule "sva: one verdict per attempt" established.

Keep the harness. Every new tracking discipline should be run through it before
it is built in `Monitor`, and it is 400 lines that cost 6 seconds.

---

## 10. What this does not establish

- ~~Whether Tier A's desugarings beat dedicated tags on state count.
  `always [1:100] p` as a 100-way conjunction is the case to measure.~~
  **Measured — see §4.** Linear in the range either way, so there is nothing to
  win on size; the tag earns its place by keeping the semantics and the refusals
  in one place.
- The real cost of `not`. Determinisation is exponential in the worst case and
  the observed blow-up for `first_match` was workload-dependent (6 → 25 states on
  overlapping multi-literal guards). No SVA workload exists to measure against.
- ~~Whether the single-state discipline composes with the ring one.~~ **Partly
  settled by §3's measurement**: the counted-subset discipline reproduces the
  ring's verdicts on bounded obligations too, so the two are one mechanism rather
  than two that have to compose. What is still open is the *shell* case — a
  property mixing a bounded implication with an unbounded `until` puts a trigger
  under an unbounded obligation, and `propBound` refuses it today with the shift
  node as culprit. Monitor-level mode with a loud refusal of the mixed shape is
  the cheap first cut.
- How wide an attempt counter has to be. §3 measures multiplicity growing one per
  cycle in the worst case (7 pending at 8 cycles for `s_eventually`), so it is
  unbounded in trace length and needs a saturating width and a documented
  behaviour at saturation. Nothing in the ABI carries a count today.
- Recursive properties (LRM 16.12.4), which are refused separately and are not
  part of this scoping.
- ~~Whether any of this interacts with the per-obligation versus per-attempt
  reporting question.~~ **It does, and it is now the governing constraint**:
  "sva: one verdict per attempt" settled that a verdict belongs to an attempt,
  and §3's measurement shows a merged vector cannot say how many attempts a
  verdict covers. That is what forces the counter.


---

## 11. The guarded selection forms, as built

`if (c) p else q` and the `case` form need no tag, which §4 predicted. What §4 did
not pin down is the shape, and the shape is the whole reason it works:

```
if (c) p else q   ≡   (c #-# p) or (!c #-# q)
```

A disjunction of **followed-bys**, not of implications. Exactly one guard holds, so
the disjunct whose guard fails has no antecedent match — and a followed-by without
a witness *fails* rather than holding vacuously, so it contributes nothing and the
taken branch alone decides the verdict. Put implications there instead and the
untaken branch holds vacuously, the disjunction always passes, and the monitor can
never report anything: a silent no-op that no fixture asserting only "it lowers"
would catch. libsva's own test, `the conditional and case forms are a disjunction
of followed-bys`, is the reference for this.

Two details the LRM supplies and clotho must not lose:

- **An unwritten `else`, and a `case` with no matching item and no `default`, are
  the TRUE property and not an absence** (16.12.16, 16.12.17: the evaluation
  "succeeds and returns true (vacuously)"). So both become `guard #-# 1'b1` rather
  than a missing disjunct — which is what `diff_propcase.sv`'s `cs` versus `csd`
  pair is built to separate.
- **`case` is a linear search that stops at the first match** (16.12.17), and
  duplicate labels across items are legal, so item *i*'s guard is its own labels
  matching AND no earlier item's. The default's guard is the conjunction of every
  item's miss.

One wrinkle worth knowing about, because it is the only thing in either phase that
changed a data structure clotho hands to its backend. A `case` item guard is
`case_expr === label`, and **slang never builds that comparison**: it binds the case
expression and every label as separate self-determined expressions
(`CaseAssertionExpr::fromSyntax`). There is no single `Expression` to make an atom
of, so `sva::Atom` grew a second expression pointer and `eval_atom` emits the
comparison itself — `CaseEq`, because 16.12.17 defers to the 12.5 matching rules,
which compare bit for bit and are x/z-sensitive — over the common type of the two
operands, by the same widening rule `common_case_type` applies to a `case`
statement.