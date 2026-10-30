# Process legalization

Scoping doc for S6's first half: turning procedural always-blocks into the hardware
they imply. Status: design, not started.

Three things drive it, in the user's priority order: **be fast** (yosys `proc` is
slow), **be state of the art on coverage**, and **provide a mode that materialises
the implicit mux structure explicitly**, later honouring `full_case`/`parallel_case`.

Everything measured below was executed locally. yosys is `0.67+92`
(git `30fe16c7f`), Verilator `5.050`, Icarus `13.0`. Claims that
were read but not run are marked, and §10 lists what could not be established.

---

## 1. Terminology, because the field has no single word

| term | community | scope |
|---|---|---|
| **if-conversion** / predication | compilers, HLS | the control→mux step; in SSA terms, replacing a phi with a select. The closest thing to a standard term. |
| **latch inference** | RTL synthesis, LRM-adjacent | just the missing-else/hold sub-problem. slang spells it `diag::InferredLatch`. |
| "convert decision trees to multiplexers" | yosys | `proc_mux.cc:440`, the pass description |
| **desequentialization** | LLHD/CIRCT | `llhd-deseq`, "convert sequential processes to registers" |

"Process legalization" is not a term of art. Internally it is fine; externally,
say **if-conversion** for the mux step and **latch inference** for the hold
question, or nobody will know what is meant.

One naming trap inherited from §3.6: *legalization here means diagnose and
decompose, not rewrite* — the opposite of the LLVM ISel sense.

---

## 2. The performance target, measured on both sides

hazard3 (`tests/perf/cases/hazard3/src/*.v`, ~5k lines of real RISC-V RTL):

| | time |
|---|---|
| yosys `read_verilog` + `hierarchy` | 395 ms |
| yosys **`proc`** | **18,500 ms** |
| Clotho entire frontend today (parse + elaborate + lower + `DesignFacts`) | **51 ms** |
| — of which Clotho's whole-design analysis walk | 11.5 ms |

`proc` costs **47× the parse** on the same input. Per-sub-pass, on hazard3:

```
proc_clean      -4 ms      proc_rom        -22 ms
proc_rmdead    140 ms      proc_mux     18,266 ms   <-- 98.7%
proc_prune       4 ms      proc_dlatch     135 ms
proc_init      -11 ms      proc_dff        495 ms
proc_arst       33 ms      proc_memwr     -480 ms
                           proc_clean      862 ms
```

It is `proc_mux`, and nothing else is close. **The target is that legalization costs
about what `DesignFacts` costs — one more walk, order 10 ms — i.e. roughly three
orders of magnitude below yosys.** That is not optimism; §3 shows the gap is
structural, and §2.3 is honest about what the comparison does and does not mean.

### 2.1 Why `proc_mux` is 18 seconds

Two compounding costs, both confirmed in source and by measurement.

**Quadratic in signals per decision tree.** `proc_mux` walks the *entire* case tree
once per "snippet" (a maximal run of bits never split by an assignment):

```cpp
for (int idx : sigsnip.snippets)                                  // proc_mux.cc:427
    signal_to_mux_tree(mod, …, &proc->root_case, sig, …);         // :434  full walk
// and inside, for every visited case:
for (auto &action : cs->actions) {                                 // :326  NOT pruned
    sig.replace(action.first, action.second, &result);             // :327
    action.first.remove2(sig, &action.second);                     // :328  destructive
}
```

`swcache.check(sw)` prunes irrelevant *switches* but never the per-case action loop.
Measured: **3.96× per doubling** of snippet count — 32→0.032 s, 512→**7.14 s**.

hazard3 has **823 snippets total, one process with 412 and another with 174** —
squarely in the quadratic regime.

**78% of that is throwaway hash tables.** `perf` on the 256-snippet case:
`pool<SigBit>::insert` 36.9%, `SigSpec::replace` 33.1%, `pool::count` 3.8%. Both
`replace` (`rtlil.cc:4908`) and `remove2` (`rtlil.cc:5006`) **allocate a fresh hash
table per call and discard it**, and hashlib does an integer modulo by a prime on
every probe (`hashlib.h:435-441`).

The destructive `remove2` is what makes the per-snippet re-walk survivable at all —
it is an amortisation hack **that requires a mutable IR**. Clotho's no-rewriting
invariant forbids it, which is fine, because the fix is to not need it.

### 2.2 The other cliff: exponential coverage analysis

`BitPatternPool` (`kernel/bitpattern.h:174`) is an explicit sum-of-cubes; `take()`
splits every matching cube on every don't-care bit. Measured on a leading-zero
detector: **2.24× per selector bit**, 12.7 s / 1 GB at W=24, **`std::bad_alloc` at
4 GB for W=32**. Open bugs #3268 (since 2022) and #5839; PR #5852 proposed an effort
limit and was closed unmerged, the reviewers concluding the real fix is a BDD and
that bailing out is unsafe because it forces spurious latch inference.

Two mitigating facts, both important for us:
- It is gated behind `is_simple_parallel_case` — a cheap "all patterns fully-defined
  and pairwise distinct" check. **Only wildcard `casez`/`casex` pays.**
- It exists **solely to group arms into `$pmux` vs chained `$mux`** — a QoR decision,
  not a correctness one. Clotho may approximate or skip it.

Clotho's existing `full_case` coverage check (landed in `lower/Statements.cpp`) already takes a
different approach: direct enumeration of the value space, capped at 16 bits. That
is O(2^W) *memory* but linear in covered values, with a hard predictable ceiling
rather than an unpredictable cube explosion. Widening it is §7.

### 2.3 What the comparison is not

yosys `proc` emits **cells**; Clotho's legalization emits **facts**. Those are not
the same work, and quoting 18,500 ms vs 10 ms as a like-for-like speedup would be
dishonest. Two things keep the comparison meaningful anyway:

- Cell creation is a minority of the 18.5 s. yosys builds ~12.5k cells at ~7k
  cells/s ≈ 1.8 s worst case; the other ~16.7 s is re-walking and re-hashing.
- The *decisions* are identical: which signals mux on which conditions, what holds,
  what is a flop. Clotho has to make every one of them.

So: comparable on analysis, and Clotho additionally never pays the cell-construction
tail because there is no netlist to build until S7 asks for one.

---

## 3. Why Clotho is structurally faster, cost by cost

| yosys cost | Clotho |
|---|---|
| N tree walks, one per snippet | **1 walk, all signals at once** — the guard stack is shared, so the marginal cost of another written signal is one vector push |
| `pool<SigBit>` per call, hashed with an integer modulo | `Signal` is a `u32` index → **direct array indexing, no hashing at all** |
| per-bit `SigSpec` work, representation flapping between packed and unpacked | per-signal, with part-selects carried as a masked sub-assignment |
| destructive `remove2` to amortise the re-walks | not needed; nothing is re-walked |
| CFG → mux-tree recovery (CIRCT: `SCFToControlFlow` → `mem2reg` → `RemoveControlFlow`) | **the tree is already a tree** — `cond` with regions, `switch` with explicit arms plus a default |
| `proc_dlatch` scans the whole module twice, unconditionally, 19% of `proc` on an all-flop design | completeness falls out of the same walk that built the mux |
| storage inference re-derived from the *netlist* (`proc_dlatch`/`proc_dff` contain **zero** references to `root_case`) | derived from the tree, where the information actually is |
| CE and sync reset need a *separate* pass (`opt_dff`) — `proc` infers async reset only | one walk produces the full decomposition |

That last row is worth dwelling on. Verified: on a register with async reset, sync
reset and enable, `proc` alone yields `$adff + $dff + 4×$mux`; only after `opt_dff`
do you get `$adffe + $sdffe`. **Clotho's S6 table already promises enable and sync
reset, so it is targeting strictly more than yosys `proc` does — in one pass.**

---

## 4. The core algorithm

One recursive walk over the region tree, carrying a **guard stack**, producing per
written signal an ordered list of guarded assignments.

```
walk(region, guard):
  for inst in region:
    Store(sig, val)       -> emit (guard, val) into list[sig]
    Cond(c, then, else)   -> walk(then, guard ∧ c)
                             walk(else, guard ∧ ¬c)
    Switch(sel, arms, df) -> for arm in arms: walk(arm.body, guard ∧ (sel ∈ arm.keys))
                             walk(df,         guard ∧ (sel ∉ ⋃ keys))
```

`guard` is a stack of `(Index, polarity)` or `(Index, key-set)` entries — the
predicates already in the IR. Nothing is rebuilt symbolically; the guard is a path,
not an expression.

Complexity: **O(instructions) traversal + O(assignments) emission.** Linear, and it
visits each instruction exactly once regardless of how many signals the process
writes. That is the whole ballgame against `proc_mux`'s O(S²).

**The mux tree is the list.** Folding right-to-left over the guards, with the base
case being whatever the signal holds when no guard fires:

```
y = g₁ ? v₁ : (g₂ ? v₂ : … : ⟨base⟩)
```

**Completeness is a property of the same list**, not a second analysis. And it is
mostly structural, which is what keeps it cheap:

| shape | complete? | cost |
|---|---|---|
| `if` with an `else`, both assigning | yes | free — both arms present |
| `if` with no `else` | **no** | free |
| `switch` with a `default` | yes | free |
| `switch`, no default, all arms assign | needs key coverage | §7 |
| assignment before the branch (`y = 0;` then `if…`) | yes | free — an unguarded entry at the head |

Only the fourth row needs coverage analysis at all, and the common cases (an
enumerated state register, a one-hot decoder) are decidable by counting.

`base` is the interesting slot, and it is exactly where the three tools differ:

- **complete** → `base` is unreachable → pure combinational
- **incomplete, level-sensitive** → `base` is the signal's own previous value → **latch**
- **incomplete, edge-sensitive** → `base` is the previous value → **enable** on a flop
- **`full_case` asserted** → `base` is don't-care → no latch (§8)

### 4.1 Storage inference: condition-based, not structural

The single most valuable architectural finding from the survey.

yosys matches async reset **structurally** — the reset test must be the *sole
outermost* switch: `while (proc->root_case.switches.size() == 1)` (`proc_arst.cc:198`).
Nest it and recognition fails, and then `proc_dff` hard-errors.

CIRCT matches **conditionally** — it computes a boolean drive condition per drive and
tests it against register templates, so syntactic nesting is irrelevant
(`LLHD/Transforms/Deseq.cpp`). `[INFERENCE]` — read, not executed.

This is not academic. Measured, on `always_ff @(posedge clk or posedge rst) if (en) begin if (rst) q<=0; else q<=a; end else if (rst) q<=0;`:

| tool | result |
|---|---|
| yosys | **hard error** — "Multiple edge sensitive events found for this signal!" |
| Icarus | accepts |

**A guarded-assignment list is already a drive-condition representation.** Clotho's
guard for the `q<=0` store is `¬en ∧ rst` in one branch and `en ∧ rst` in the other;
matching "the condition under which q takes a constant, and it is implied by an edge
trigger" is a property of the guard set, not of where the `if` sits. So Clotho gets
the case yosys rejects — **for free, from the representation choice**, not from
extra work.

That also makes it a first-class **mismatch finding** for S5: the shape is legal,
its asynchronicity lives in the guard set rather than in the syntax, and Clotho can
classify it and say so.

### 4.2 Trigger classification: copy CIRCT's encoding

CIRCT encodes each trigger as a `(past, present)` bit pair and resolves the whole
question as a truth table over ≤5 variables, brute-forcing all `1<<3 = 8`
clock/reset polarity assignments (`Deseq.cpp:1143`). Constant time, exhaustive, no
heuristics. Directly portable, and far cheaper than yosys's recursive
`check_signal` — which does a **full `mod->cells()` linear scan at every recursion
level** (`proc_arst.cc:39`) with no driver index.

CIRCT's limits are worth *not* copying: it bails at `triggers.size() > 2`
(`Deseq.cpp:462`) and `valueTable.size() > 3` (`:1055`), so it fails the two-async-reset
and async-set-plus-reset rows that yosys handles. Take the encoding, not the caps.

### 4.3 Per signal, not per process

Measured (`f07`): a block that async-resets only *some* of the signals it writes
splits into `$adff` + `$dff` + `$mux`. Icarus refuses it outright ("Not all bits of
'q2' are asynchronously set or reset. This is not supported").

`§3.6`'s table already says the unit is **(process × written signal)**. The
guarded-assignment list is naturally per signal, so this is automatic — but a naive
"is this block a flop?" classifier gets it wrong.

---

## 4A. Resets, in detail

Resets are where the tools measured diverge most, and where the divergences are
silicon-visible. This section is a 28-case corpus
executed against yosys `0.67+92`, cross-checked for legality against Icarus `13.0`
and Verilator `5.050`, with the semantic claims verified by simulation rather than
asserted.

Clotho lowers **all 28 cases** today with no diagnostic — the frontend has no
trouble with any of them. Everything below is about what a legalizer must decide.

### 4A.1 The measured matrix

All cases are `always @(posedge clk or <reset edge>)` on an 8-bit `q` unless noted.
Cells are after `proc; opt_clean`.

| # | shape | yosys result |
|---|---|---|
| R01 | `negedge rst_n` / `if (!rst_n)` | `$adff` |
| R02 | `posedge rst` / `if (rst)` | `$adff` |
| R26 | `if (~rst_n)` rather than `!` | `$adff` |
| R05 | `if (rst) … else if (en) …` | `$adff` + `$mux` |
| R30 | same, with `begin`/`end` | `$adff` + `$mux` |
| R11 | `if (rst) q<='x` | `$adff` |
| R20 | reset value `{4'ha,4'h5}` | `$adff` |
| R27 | reset value a parameter | `$adff` |
| R15 | reset signal read as data in the else arm | `$adff` |
| R16 | sync reset only | `$dff` + `$mux` |
| R17 | async + sync reset | `$adff` + `$mux` |
| R28 | two signals, different reset values | 2× `$adff` |
| R14 | `if (rst) q<=q` (self-assigning) | `$dff` + `$mux` — folded to an **enable**, no warning |
| R06 | two async resets, same value | `$adff` + `$reduce_or` |
| R23 | `wire rst_any = rst\|rst2` as the trigger | `$adff` + `$or` |
| R07 | two async resets, **different** values | `$dffsr` + 3× `$mux`, **W** "Complex async reset" |
| R08 | async set **and** reset | `$dffsr` + 3× `$mux`, **W** |
| R09 | non-constant reset value | `$aldff`, **W** "Async reset value not constant" |
| R03 | `negedge rst_n` / `if (rst_n)` — polarity mismatch | `$aldff`, **W** |
| R29 | `posedge rst_n` / `if (!rst_n)` — inverse mismatch | `$aldff`, **W** |
| R25b | partial reset, other signal **inside** the else | `$adff` + `$dff` + `$mux` |
| **R04** | reset nested under an enable | **REJECTED** — "Multiple edge sensitive events" |
| **R24** | R04 written as `always_ff` | **REJECTED** — same |
| **R13** | `if (rst \|\| rst2)`, both in the sensitivity list | **REJECTED** — same |
| **R22** | `if (rst && en)` | **REJECTED** — same |
| **R19** | conditional *inside* the reset arm | **REJECTED** — "yields non-constant value" |
| **R25c** | partial reset, other signal **outside** the if | **REJECTED** — same |

**Six rejections, and all six are legal SystemVerilog** — Icarus compiles and
Verilator lints every one with zero errors.

### 4A.2 The smoking gun

**R13 vs R23 are semantically identical and yosys accepts only one.** Verified by
simulation — pulsing either reset with no clock gives `q=00` in both designs, on
both reset paths:

```systemverilog
// REJECTED
always @(posedge clk or posedge rst or posedge rst2) if (rst||rst2) q<=0; else q<=a;
// ACCEPTED  ->  $adff + $or
wire rst_any = rst | rst2;
always @(posedge clk or posedge rst_any)             if (rst_any) q<=0; else q<=a;
```

Hoisting the `||` into a wire is the practitioner's workaround, and needing it is
the whole indictment: the difference is spelling, not meaning.

### 4A.3 Where yosys is right but fails badly

R25c deserves care, because the obvious reading is wrong and I nearly recorded it.

```systemverilog
// R25b -- ACCEPTED           | // R25c -- REJECTED
if (rst) q<=0;                | if (rst) q<=0; else q<=a;
else begin q<=a; q2<=a; end   | q2<=a;
```

These look like the same design. They are not. Simulated, after a reset pulse with
no intervening clock edge and `a` changed in between:

| | q | q2 |
|---|---|---|
| R25b | `00` | `aa` — held, the assignment is inside the else |
| R25c | `00` | `55` — **updated on the reset edge: an async load** |

So `q2` in R25c genuinely is an asynchronously-loaded register, and rejecting it as
a simple flop is substantively correct. **yosys's failure is that it rejects the
whole block**, including `q`, which is a perfectly ordinary `$adff`. The right
answer is per-signal (§4.3): classify `q` as a flop, classify `q2` as an async load,
and report only `q2`.

This is also the case that proves the diagnostic matters as much as the decision. A
designer who wrote R25c almost certainly meant R25b, and "async reset yields
non-constant value 8'mmmmmmmm" does not say so.

### 4A.4 Root cause: a structural gate

All six rejections trace to one line. `proc_arst` only looks at a reset when the
reset test is the **sole outermost switch**:

```cpp
while (proc->root_case.switches.size() == 1)   // proc_arst.cc:198
```

and it matches the condition against the trigger with a recursive syntactic peeler
(`check_signal`, `:33-84`) that unwraps `$reduce_or`, `$reduce_bool`, `$logic_not`,
`$not`, `$eq/$eqx`, `$ne/$nex` against constants while tracking polarity. Anything
the peeler does not recognise, or any nesting that breaks the sole-outermost-switch
precondition, falls through to `proc_dff`, which then finds two edge triggers it
cannot account for and errors with a message about the *sensitivity list* rather
than about the shape that actually defeated it.

`check_signal` additionally does a full `mod->cells()` linear scan at every
recursion level with no driver index — cheap only because `proc_arst` runs before
`proc_mux`, when the module is nearly empty.

### 4A.5 Clotho's rule: partial evaluation, then the test you already have

Syntactic position never enters it. The rule is **pin the trigger and fold**, and
then ask the completeness question §4 already needs for latch inference.

Note this supersedes an earlier formulation of this section that spoke of
substituting into collected guards. That version implies proving a disjunction of
conjunctions is a tautology, which is co-NP and unnecessary — you never collect the
guards, you fold in place and let the tree carry the coverage.

**Step 1 — identify the clock.** The trigger appearing in no guard. Zero, or more
than one, is the ambiguous-clock rejection `proc_dff` also makes. This is also
position-free: in R15 the reset is read as *data* in the else arm and still
classifies, because appearing in a guard is what matters, not appearing at all.

**Step 2 — for each non-clock trigger `r`, pin `r` asserted and constant-fold the
body's predicates.** A three-valued evaluator (0 / 1 / unknown) over the predicate
cone. Branches whose predicate resolves collapse to the taken arm; the rest stay.

The cones are tiny. R04's `rst` predicate is the whole of

```
%5 = red_or(load(signal(s1)))
```

three instructions, and R22's `rst && en` is a `select_lazy` over five. Pinning
`s1 := 1` in R04 collapses the body to `if (en) q<=0; else q<=0;`.

**Step 3 — classify the residue** with the ordinary completeness test:

| residue | classification |
|---|---|
| one constant on every path | **async reset**, that constant |
| one non-constant expression on every path | **async load** — diagnose, do not invent a cell |
| two or more constants, still covering | async reset with a **conditional value** |
| some path assigns the data, or no path assigns | **not a reset**, and the residue is the explanation |

**Step 4 — the all-deasserted residue is the next-state cone**, from which sync
reset and enable fall out by the same folding.

#### Why there is no SAT solver here

Coverage is **structural**: `if (en) … else …` is exhaustive because both arms are
present, not because `en ∨ ¬en` was proven. The tree carries it. That is a direct
consequence of the IR being structured and it is what makes this cheap rather than
clever. The only shape needing real coverage analysis is a `switch` with no default
— §7's machinery, and the rare case.

#### The six worked through

| case | pin | residue | verdict |
|---|---|---|---|
| R04 nested under enable | `rst:=1` | `if (en) q<=0; else q<=0;` | reset, 0 ✓ |
| R24 same as `always_ff` | `rst:=1` | identical — the keyword never enters the reasoning | reset, 0 ✓ |
| R13 `if (rst\|\|rst2)` | `rst:=1` then `rst2:=1` | `lor(1,·)`→1, body is `q<=0` both times | two resets, same value → collapse ✓ |
| R19 conditional reset arm | `rst:=1` | `if (en) q<=0; else q<=8'hFF;` | covering, two constants → conditional reset value |
| R22 `if (rst && en)` | `rst:=1` | `select_lazy` folds to `land(1,en)` = `en`; body is `if (en) q<=0; else q<=a;` | **reject** — under `rst` with `en` low the block assigns `a` |
| R25c partial reset | `rst:=1` | `q`: `q<=0`. `q2`: `q2<=a`, unguarded | `q` is a reset ✓, `q2` is an async load — report `q2` only |

R22's residue *is* its diagnostic: "reset condition `rst && en` is not implied by
the `posedge rst` trigger: with `en` low the block still fires on `rst` and assigns
`a`". Against yosys's "Multiple edge sensitive events found for this signal!", which
describes the sensitivity list — a place with nothing wrong in it.

#### Cost

One fold-walk per non-clock trigger; designs have one, occasionally two. So ~2× the
base walk, touching only predicate cones. yosys's `check_signal` by contrast does a
full `mod->cells()` scan at every recursion level with no driver index.

#### Prerequisites and open problems

- **The three-valued partial evaluator does not exist.** Clotho has no constant
  folder (S2 is unstarted). This needs a narrower one — evaluate a predicate cone
  with one signal pinned — but it is real work and a hard prerequisite, not a
  detail.
- **Failure is "could not prove", not "wrong shape".** Better than a pattern peeler,
  but a reset condition routed through a function call or a value the evaluator
  cannot see still folds to unknown and degrades to a conservative rejection.
- **Edge on an expression** (`@(posedge (g & e))`) — resolved, see §4A.9. The
  frontend already materialises it into a hidden signal, so triggers are always
  `Signal` and this rule is unaffected. A residual case remains for expressions used
  as *resets*.
- **Reset value depending on the reset** (`if (rst) q <= {7'b0, rst}`) folds cleanly
  to `8'h01`, but whether that matches what a synthesiser builds is unverified.

### 4A.9 Edge on an expression

`always @(posedge (g & e))` looked like a hole in §4A.5 — the trigger is not a
signal, so "pin it asserted" has no obvious meaning. Measured, most of it is a
non-problem.

**The frontend already decomposes it.** `@(posedge (g & e))` lowers to two
processes: a level-sensitive helper computing the expression into a hidden signal,
and the real process triggered on that signal.

```
s4: logic __event0 (internal)
process[0] always @s0 @s1        %4 = band(load(s0), load(s1)); store(s4, ...)
process[1] always @posedge s4    q <= d
```

So `WaitEdge` holds a `Signal` because the frontend guarantees it, and the pin-and-
fold rule is unaffected. Verified semantically against iverilog over six
observations including a same-timestep glitch to 1 — byte-identical.

**The residual case is an expression used as a reset.** Materialisation relocates
the problem rather than solving it. For
`@(posedge clk or posedge (a|b)) if (a|b) q<=0; else q<=d;` the trigger is the
hidden signal, but the guard lowers to `red_or(bor(load(a), load(b)))` — recomputed
from the leaves, never mentioning the trigger. Pinning it folds nothing: the trigger
and the guard reach the same value through two independent cones.

The scope is narrow, because the recommended spelling does not have the problem:

```systemverilog
wire rst_any = a | b;                              // guard -> red_or(load(s5))
always @(posedge clk or posedge rst_any)           // reads the trigger: folds
  if (rst_any) q <= 0; else q <= d;
```

This is R13-vs-R23 one level up — the hoisted form works, the inline form does not.

**Answers, ranked.**

| | |
|---|---|
| **(a) cone equivalence** | structurally hash the trigger's driver cone and the guard cone; equal ⇒ pinning folds. Not implication proving — recognising that the source wrote the same expression twice. Memoised recursive hash over tag + payload + operand hashes. |
| **(b) reject, naming the fix** | "the sensitivity list and the body compute `(a\|b)` independently and cannot be proven equal — hoist it to a named net". Actionable, and it is what practitioners do anyway. |
| **(c) general implication** | no. Wrong cost, and it is the road that gave `BitPatternPool` two open OOM bugs. |

**(a) is cheap but not free**, and the cost is structural rather than
computational: the driver cone is already a result of this analysis (process 0
classifies as Comb driving the hidden signal with an unconditional cone), but using
it means **a comb process driving a trigger must be classified before the sequential
process it triggers**. A signal→driver map with lazy memoised classification
suffices — no topological sort — but it is new structure, and it needs a guard for
the case where the driver is conditional or itself sequential, which falls back
to (b).

**Decision: ship (b) in M5, add (a) on demand.** The wire form covers the common
case for free, (b) tells the user exactly what to change, and (a) drags in ordering
machinery for a spelling that is rarer than its own workaround.

**Two obligations this surfaced.**

- **Gated clocks become trivially detectable, and must be reported.** A trigger
  signal combinationally driven inside the design *is* a gated clock — real
  structure, lint-worthy for timing and CDC, and an ICG cell or a
  clock enable is the cleaner alternative. The test is "the trigger's driver is a Comb process", which
  the analysis produces anyway.
- **Synthetic names must never reach a diagnostic.** `__event0` does not exist in
  the source. The process carries the right span (the expression itself), so the
  information is there, but a finding naming the signal is unreadable. A rendering
  rule for `Note`, and another argument for structured operands over strings.

### 4A.10 Ordering, and why the async entry is a pair list

Reset ordering is silicon-visible, and getting the data structure right for it
matters more than getting the analysis right — the analysis was already correct.

Measured on `@(posedge clk or posedge arst)`, pulsing `arst` with `srst` high and
no clock edge:

| body | q |
|---|---|
| `if (arst) q<=0; else if (srst) q<=8'hAA; else q<=d;` | **00** |
| `if (srst) q<=8'hAA; else if (arst) q<=0; else q<=d;` | **aa** |

The second is **not** "a sync reset given priority over the async one". The block
fires on `arst`'s edge and `srst`'s branch is what executes, so the `srst` constant
is loaded **asynchronously**: under the arst edge, `q <= srst ? 8'hAA : 8'h00`.
Realizing it needs CLR active on `arst && !srst` and SET on `arst && srst`, per bit
according to `8'hAA` — a `$dffsr` with masks.

Pin-and-fold produces exactly that: pinning `arst := 1` gives the residue
`if (srst) q<=8'hAA; else q<=8'h00;`, which *is* the condition/value pair list.

**But the earlier `AsyncReset { Trigger; Ref }` could not hold it.** Keyed on the
trigger with one value, it cannot express `arst && !srst → 0` alongside
`arst && srst → 8'hAA`, because the condition involves `srst`, which is not a
trigger. Corrected in §11.5 to `AsyncReset { Index condition; Ref value; }`, a
priority-ordered list taken verbatim from the residue.

#### The collapse case, and a second smoking gun

These two are **identical on all five observations simulated** — clocked,
arst-with-srst-high, clean clock, srst+clock, arst alone:

```systemverilog
if (arst) q<=0; else if (srst) q<=0; else q<=d;   // yosys: $adff + $mux
if (srst) q<=0; else if (arst) q<=0; else q<=d;   // yosys: ERROR
```

Both are idiomatic. yosys rejects the second — "Multiple edge sensitive events
found for this signal!" — because the structural gate (§4A.4) needs the reset test
to be the sole outermost switch.

Pin `arst := 1` on the second: residue is `if (srst) q<=0; else q<=0;`, **one
constant**, so it collapses to a single `(true, 0)` pair and classifies as a plain
async reset. The collapse is the ordinary completeness test, not a special case.

This is a sharper example than R13/R23 (§4A.2), where the two spellings were not
equally natural. Here both are what a designer would write without thinking.

#### Realizability is the target tool's problem

| residue after pinning | classification | realizable as |
|---|---|---|
| one constant | async reset | `$adff` |
| two constants, covering | conditional reset value, with conditions | `$dffsr`, per-bit SET/CLR |
| any non-constant on any path | **async load** | nothing standard — flag it |

Clotho emits the pair list and the conditions. Whether one primitive holds them is
downstream, which is the line target-independent synthesis is supposed to draw.

### 4A.6 What must still be rejected, and why the message matters

Unsynthesizable as written: no clock among the triggers;
more than one clock; both edges of one clock; mixed edge and level sensitivity.
`proc_dff`'s five errors are the cleanest existing statement of this set and are
worth copying as a checklist.

But the rejections above show the failure mode to avoid: **a message about the
sensitivity list when the problem is the body**. Clotho's rejections should name the
guard that defeated the match and the substitution that exposed it. For R22 that is
"reset condition `rst && en` is not implied by the `posedge rst` trigger: with `en`
low, the block still fires on `rst` and assigns `a`" — which tells the designer what
to change.

### 4A.7 Divergences worth reporting as findings

Each of these is a shape whose hardware reading can quietly differ from its
simulation reading, which makes them S5 material rather than merely legalization
concerns:

- **Nested reset** (R04, R24). The asynchronicity lives in the guard set rather than
  in the syntax, so the shape can be re-read as a *synchronous* reset without
  anything looking wrong. Clotho classifies it by pin-and-fold and can say so.
- **Polarity mismatch** (R03, R29). `negedge rst_n` with `if (rst_n)` is almost
  certainly a typo, and it is *legal*: it means an async load of the else-arm value.
  yosys builds `$aldff` and warns. Clotho should diagnose it as a likely inversion
  bug rather than either silently building it or refusing.
- **Async set and reset together** (R08). yosys builds `$dffsr`, but a target may
  have no flip-flop cell with both, so it is a portability finding.

### 4A.8 Escape hatches

Inference is heuristic, and a user must be able to override it. Clotho should have an
attribute path for that reason, sharing the plumbing S7 needs. Design it in from the
start rather than discovering later that inference is occasionally wrong and there is
no override.

---

## 5. Coverage: where the bar actually is

77-case corpus executed against yosys, Icarus and Verilator. The
decision-relevant parts:

### 5.1 Clotho starts with a large unearned advantage

**Every yosys rejection below is a *classic-frontend* rejection.** `yosys-slang`
exists and covers far more, but is a plugin (ABI-broken against the local 0.67+92).
Clotho parses with slang natively, so **Clotho's coverage floor is slang's** — full
IEEE-1800 elaboration. The gaps below are legalizer gaps, not parser gaps, and
Clotho simply does not have the parser ones.

Concretely, yosys's classic frontend rejects all of these, every one of which is
legal SystemVerilog that slang elaborates:

| construct | yosys classic frontend |
|---|---|
| `while` (static bound) | **error** |
| `do…while` | **error** |
| `forever` + `disable` | **error** |
| `foreach` | **error** |
| `break` / `continue` / `return` / `disable` | **error** (all six forms) |
| `case … inside` | **error** — cannot lex `4'b1???` |
| unpacked struct target | **error** |
| recursive function | **SIGSEGV** (rc 139) |

That is the widest capability spread in the matrix and the clearest "common in real
RTL, poorly served by the open stack" gap. Clotho inherits none of it.

### 5.2 What legalization must actually handle

Supported by all of yosys, Icarus and Verilator, so table stakes: nested if/else;
case/casez/casex with and
without default; `unique`/`priority`/`unique0`; static-bound `for`; automatic
functions; tasks with output args; classic async reset; sync reset; DDR as two
blocks; edge on an expression; all *packed* assignment targets (struct field, packed
array element, union field, part-select, indexed part-select, dynamic bit-select,
concat LHS).

*Rejected*, so a ready-made rejection spec — `proc_dff`'s five errors are
the cleanest statement of it: multiple edge-sensitive events; multiple always events;
any-edge sensitivity; mixed always/edge/level; missing edge-sensitive event. Yosys,
Icarus and Verilator, as measured here, reject multi-clock and dual-edge-in-one-block.

The frontier, where tools genuinely differ and Clotho can lead:

| row | state of the art | Clotho's position |
|---|---|---|
| **reset in a nested `if`** | yosys errors on it, though it is legal SystemVerilog | §4A gets it right by construction |
| two async resets / async set+reset | yosys `$dffsr`; CIRCT bails; Icarus refuses | achievable — it is a guard-set shape |
| non-constant reset value | yosys `$aldff` + warning (and YosysHQ themselves published "Why Asynchronous Load Flip-Flops Should Be Avoided") | classify and diagnose rather than invent a cell |
| partial async reset | yosys splits; Icarus refuses | free (§4.3) |
| jump statements in a synthesizable block | sv2v has the reference *encoding* | §5.3 |
| ROM inference from a case tree | yosys `proc_rom` only | §5.4 — flagged as the highest-value single feature to copy |
| latch policy | yosys is the **only** surveyed tool that hard-errors on `always_comb`-with-latch and `always_latch`-without | Clotho already warns; the policy axis is §9 |

### 5.3 Jump statements: take sv2v's encoding — LANDED, and smaller than expected

`sv2v`'s `Convert/Jump.hs` uses a 2-bit `_sv2v_jump` ordered
`none=0 < continue=1 < break=2 < return=3`, so "keep running" is the single
comparison `jump < break`. Statements after a jump get wrapped in
`if (jump == none)`; loop guards become `cond && keepRunning`; increments become
`Mux(keepRunning, incr, unchanged)`; nested loops save and restore through a
`_sv2v_jump_<depth>` stack. `[UNVERIFIED]` — source read, not executed.

The prediction that this "maps directly onto a structured IR — the jump variable
becomes another guard term" is right, and it is stronger than it sounds: **sv2v
needs a variable only because it emits Verilog and has nowhere else to keep the
state.** An analysis producing guarded lists puts it in the guard and needs no
state at all for the shapes that matter:

| shape | effect |
|---|---|
| bare `break` / `continue` | nothing after it in that body is recorded |
| `if (p) break;` | everything after is guarded by `!p` |
| both arms jump | nothing after is recorded |
| anything subtler | `Finding::UnmodelledJump` — reported, not under-guarded |

The last row is the honest boundary: a jump buried under further structure is where
sv2v's variable earns its keep, and inventing that state is a rewrite this analysis
does not do.

**This was a live soundness bug, not a missing feature.** `Break` and `Continue`
were already classified synthesizable while the walk ignored them entirely, so a
store after a jump recorded an EMPTY guard — the analysis said it always ran when it
only runs while no break has fired. Measured before the fix: `guard_depth = 0` on
exactly the store that needed `!(i == n)`. Neither hazard3 nor picorv32 uses jumps
in RTL, so nothing measured was wrong, which is precisely why it survived.

### 5.4 ROM inference

`proc_rom` converts a case tree to `$meminit` + `$memrd` when five conditions hold
(`proc_rom.cc:52-56`): no nested switch, all arms assign the same signal set, all
RHS constant, all compare values fully-defined, coverage total or a default present.
Plus two density heuristics (`:120-128`): ≥8 distinct values and `(1<<abits)/nvals ≤ 4`,
address width ≤30. The source carries its own `// TODO: better density heuristic?`.

A naive legalizer builds an S-deep mux chain for a decoder instead. The five
conditions are all cheap predicates over the guarded-assignment list, so this is
low-cost to add and it is the single biggest QoR item on the list.

---

## 6. The fill-in mode

The user's research feature: materialise the implicit structure — insert the missing
`else`, the implicit default, the hold — so it is visible.

### 6.1 No surveyed tool ships this

Verified across sv2v, CIRCT, slang, Surelog/UHDM and morty: **none of them emits
SystemVerilog with the implicit parts written in.** sv2v does not do it at all —
`always_comb` becomes `always @*` with the body untouched (`AlwaysKW.hs:194-200`),
and it has no latch logic anywhere in `src/Convert/`. CIRCT and yosys materialise
into a netlist IR, not back into source.

The closest working prior art is `yosys proc -noopt; write_verilog`, which is two
commands and produces the maximally explicit form — every decision point visible,
with `2'hx` marking the redundant sibling subtrees that `opt_expr` would collapse.
Worth keeping as a differential oracle.

So this is, among those tools, a new *source-level* output, and it is a real reason for
the feature to exist beyond the user's own research use.

### 6.2 The representation problem, and the three answers

Writing the explicit form needs a self-reference — `y = cond ? val : y` — which is a
combinational loop unless it becomes a latch. The survey found exactly two strategies
in the surveyed tools, plus one that suits Clotho better:

**yosys: build the loop, then cut it.** The self-reference is created verbatim by the
*frontend*, not by `proc_mux`: `genrtlil.cc:419-427` seeds `$0\y = \y` as the root
action, and `genrtlil.cc:691-760` equalises case arms by inserting
`temp = <incoming value>` as the first action of every arm, synthesising a missing
default the same way. It is not a loop because `$0\y` (next) and `\y` (current) are
distinct wires separated by the process boundary. `proc_dlatch` later proves where
the feedback closes and cuts it, paying with a state element.

**CIRCT: never build it.** Carry a `(value, enable)` pair; "hold" is encoded as *not
driving*. `llhd.drv %sig, %val after %t if %en`. `[INFERENCE]` on composition, each
link source-verified.

**Clotho: the guarded-assignment list already is the answer.** The list plus an
explicit `base` slot *is* the explicit form, and it never creates a loop because it
is not a netlist — `base` is a named slot with four possible fillings (§4), one of
which is "the signal's previous value" and is exactly what a latch means. Rendering
it is a printing problem, not a representation problem.

That resolves the architectural question the feature seemed to pose. **No IR change,
no self-reference, no loop.** The fill-in mode is a *renderer over the analysis*, and
therefore:

- it ships **before** S7, as a dump alongside `--emit=ir`
- it becomes SV emission when S7 lands, with the same data behind it
- it costs nothing extra — the analysis runs anyway

### 6.3 A trap worth recording

`always_comb` with an inferred latch is a **hard error in yosys**, so you cannot use
`always_comb` to observe the latch shape at all — the survey had to use `always @*`.
Clotho's fill-in mode must render the latch it *would* infer even in a block where
policy forbids it, or the mode is useless on exactly the code you want to inspect.

---

## 7. Coverage analysis, and the exponential to avoid

Needed for: `switch` without a default (is it full?), `parallel_case` verification,
and `proc_rom`'s totality condition. Clotho already has a version of this in
`check_case_pragmas`, capped at 16 selector bits.

Three options, and the yosys history says pick carefully:

| approach | cost | fails at |
|---|---|---|
| **direct enumeration** (Clotho today) | O(2^W) memory, linear in covered values | a hard, predictable ceiling — currently W>16 |
| **sum-of-cubes** (yosys `BitPatternPool`) | exponential in *wildcard density* | unpredictably; OOM at W=32, two open bugs since 2022 |
| **BDD** | the reviewers' conclusion on PR #5852 | complexity, a new dependency |

Direct enumeration is the better failure mode: it declines predictably at a stated
width instead of exploding on wildcard density. Widen the cap by counting rather than
materialising — for `parallel_case` and fullness the *counts* suffice, and a
non-overlapping arm set contributes `2^popcount(wildcards)` each, which needs no
bitmap at all. That handles the common wide cases (one-hot, enumerated state) with no
allocation. Fall back to declining, never to guessing (§9).

Note the escape hatch: `parallel_case` skips the analysis entirely, and yosys
**auto-detects** parallelism when all arm patterns are fully-defined and pairwise
distinct — so the expensive path is much narrower than "any case statement".

---

## 8. `full_case` / `parallel_case` — phase two

The mechanics, measured on identical sources differing only by the attribute:

**`full_case` changes one leaf value, not the tree shape.** The synthesized default
arm's value: `\y` (hold) without it, `1'x` (don't-care) with it. Everything else
follows. Cells after `proc`: 8 including a `$dlatch` without, **3 and no latch** with.

**`parallel_case` does change the shape**: a priority `$mux` chain becomes a single
one-hot `$pmux`.

In Clotho's terms this is pleasingly small:

| attribute | effect on the guarded-assignment list |
|---|---|
| `full_case` | `base` becomes **don't-care** instead of hold → no latch |
| `parallel_case` | the list is **unordered** instead of priority-ordered |

Both are one field. That is why it is safely a phase two.

**The critical warning**, and it is easy to get wrong: yosys's load-bearing
`full_case` handling is in its **frontend** (`genrtlil.cc:748-758`), not its
legalizer. `proc_mux`'s `get_full_case_bits` is only a secondary refinement. A
legalizer that honours `full_case` while the lowering has already committed to a hold
will not suppress the latch. In Clotho the equivalent is: **the attribute must reach
whoever fills the `base` slot**, and since that is the analysis rather than the
lowering, Clotho's structure makes this easier than yosys's — but only if `base` is
genuinely a slot and not baked in at lowering time. Design constraint, recorded now.

Also note `unique`/`priority`/`unique0` map onto these: yosys's parser sets
`priority → full_case`, `unique0 → parallel_case`, `unique → both`
(`verilog_parser.y:2953-2996`), and sv2v does the same translation. Clotho already
reads all three keywords.

---

## 9. What to reject, and how

The never-silently-accept rule applies with force here, because the failure mode is
a silently wrong netlist. Three tiers:

**Reject loudly** — unsynthesizable as written: multiple
clocks in one block; both edges of one clock; mixed edge and level sensitivity; a
non-constant loop bound where unrolling is impossible; suspension (`Delay`, `Wait`)
outside a recognised clock wait; `Fork`; `TaskCall` that cannot be inlined; the
non-synthesizable opcode surface (I/O, strings, containers, randomization, reals, VCD).

**Decline to analyse, and say so** — decidable in principle, not by this
implementation: a selector wider than the coverage cap; a non-constant case label;
`case … inside` coverage. Silence with a stated reason, never a guess. The §6.3
lesson from the pragma checks applies: a finding is only worth acting on if its
absence means something.

**Diagnose and continue** — legal, synthesizable, probably not intended: an inferred
latch in a block not declared `always_latch`; a reset that only some written signals
receive; a non-constant reset value; an incomplete `case` under `full_case`
(already landed).

Inference is heuristic and needs an escape hatch (§4A.8). **Design the attribute path
rather than pretending inference is always right**, and note it needs the same
attribute plumbing S7 needs.

### 9.1 Types with no hardware form

The opcode surface above is necessary but not sufficient: `Signal`, `Load` and
`Store` are perfectly ordinary hardware opcodes, and it is the TYPE that can have no
meaning. `ir::layout()` reports width 0 for a string, a queue / dynamic array /
associative array and an unpacked struct, and 64 for a class handle, so a
declaration written from the layout width alone said `logic q;` for a queue and the
renderer then emitted `q[0] = x` as a BIT select of it — a netlist that is not even
legal source, produced silently.

So legality also asks `unsynthesizable_type` (Legalize.h) about every signal a
process touches, and the writer asks it again before declaring one:

- A process touching such a signal is `UnsynthesizableOp`, exactly as if it held a
  container opcode. This is what makes a whole-container copy process (the flattened
  writer's `s_q = qq`) decline instead of rendering as a 1-bit assignment.
- A signal of such a type is not declared; a comment names it and its kind. A PORT of
  such a type is dropped from the module header and from every instantiation that
  connected it, again with a comment — an omitted declaration reads as a writer bug,
  a named refusal does not.
- An unpacked array, by contrast, IS representable: it keeps its declared dimensions
  (`logic [7:0] mem [0:3];`, and `[7:4]` verbatim for a descending range, since the
  renderer emits the DECLARED index). Flattening it to the leaf width, which is what
  the width-only declaration did, silently turned every element select into a bit
  select.

---

## 10. What was not established

- **CIRCT** — source-read only, not built or run. The claim that its condition-based
  matcher handles the nested-reset case is reasoned from `Deseq.cpp`, not observed.
- **sv2v** — `Jump.hs` read, not executed.
- **IEEE 1364.1-2002**, the withdrawn RTL-synthesis-subset standard, was not
  obtained. It is the formal historical statement of this bar and is worth getting
  before finalising the coverage list.
- **ASIC-side `initial` handling** — FPGA side verified (yosys infers init values);
  no ASIC tool was run.
- Clotho's own **cost estimate** for the walk is an extrapolation from `DesignFacts`
  (11.5 ms on hazard3), not a measurement of code that exists.

---

## 11. Library shape

Legalization lands as **`clotho_legalize`**, a new static library, in-house, no
stability or ABI constraints.

An extraction-for-external-reuse variant was explored and set aside. Recorded
because two of its conclusions are worth keeping and one is not:

| from the extraction design | kept? |
|---|---|
| the exporter's exhaustive `switch` doubles as the legality gate | **kept** — it is the right mechanism regardless (§11.3) |
| results are index-handle, relocatable, pointer-free | **kept** — cheap, and it is what makes them cacheable |
| a narrow ~60-opcode export IR, versioned, types pre-flattened | **dropped** — it exists only to survive a language boundary |
| C ABI, serialisation, `SourceLoc` resolved at the boundary | **dropped** |

Dropping the export IR is the big simplification: legalization consumes
`const ir::Module&` directly, uses the `InternPool` for types, and shares headers
with `ir_core` freely.

### 11.1 Target and dependency rule

```
clotho_ir_core ──┬── clotho_ir        (LlvmBackend, Jit)   +LLVM
   Ir, Module,   ├── clotho           (Lower)              +slang
   Builder,      └── clotho_legalize                       NEW
   Printer,
   Analysis,
   Eval (new)
```

Peer of `clotho_ir`, not a layer inside `ir_core`, and the test is *"does
`clotho run` need it?"* — `LlvmBackend` never consumes legalization, so the
simulator should not link it. Every consumer that does (S5 mismatch, S7 writer,
S8 LSP, clock-wave) is off the simulation critical path.

**Invariant: zero LLVM symbols, zero slang symbols**, enforced the way S0 enforced
it for `ir_core` — `nm` in CI, not a comment.

### 11.2 Layout

```
src/ir/Eval.{h,cpp}          three-valued partial evaluator      -> ir_core
src/legalize/
  Legalize.{h,cpp}           Options, Result, run() -- the walk
  Guard.h                    guard arena and atoms
  Storage.{h,cpp}            classification + reset matching
  Coverage.cpp               case fullness / parallelism
  Findings.h                 Finding enum, Note
  Materialize.{h,cpp}        the fill-in renderer
```

`Eval` goes **down into `ir_core`**, not into `legalize`, because it has three
consumers: legalization's pin-and-fold (§4A.5), S2's `Builder` folding, and
coverage. It is the one genuinely new piece of shared machinery and the hard
prerequisite §4A.5 calls out.

### 11.3 The legality gate

An exhaustive `switch` over `Inst::Tag`, each arm either "legalizes as ⟨shape⟩" or
"not synthesizable, reason ⟨R⟩". `-Werror=switch` makes a new opcode a build break
until someone classifies it.

Same fail-closed discipline as `extra_operand_shape` and `extra_operand_tail`, both
of which were verified this session by reintroducing the defect and watching the
build fail. Keep it even without an ABI to defend: 211 tags, ~136 of them
simulation-only, and silently treating an unclassified one as synthesizable is the
worst available failure.

### 11.4 Guards: a persistent arena, not copied stacks

A guard is a path — a conjunction of atoms. The obvious representation, snapshotting
the stack per store, is O(depth) per store and re-copies every shared prefix.

Instead: a **persistent list into an arena**.

```cpp
enum class GuardId : uint32_t { True = 0 };
struct GuardNode { GuardId parent; Atom atom; };
struct Atom { Index pred; AtomKind kind; uint32_t data; };  // CondTrue|CondFalse|Arm|Default
```

Push is one append and O(1); snapshotting a guard is copying a `u32`; prefixes are
shared automatically, which is exactly what a decision tree gives you. Memory is
O(branch edges), not O(paths × depth).

This is also what makes §4A.5's fold cheap: pinning a trigger walks the atom chain,
and shared prefixes fold once.

### 11.5 API

```cpp
namespace clotho::legalize {

struct Options {
    bool     honour_full_case     = false;   // phase 2, §8
    bool     honour_parallel_case = false;
    uint32_t coverage_bit_cap     = 16;      // decline above, never guess
    bool     keep_guards          = false;   // retain the arena for Materialize
};

Result run(const ir::Module&, const ir::DesignFacts* facts, Options = {});

struct Result {
    std::vector<ProcessResult> processes;    // indexed as Module::processes()
    std::vector<GuardNode>     guards;       // empty unless keep_guards
    std::vector<Note>          notes;
};

struct ProcessResult {
    Legality             legality;
    std::vector<Storage> storage;            // one per WRITTEN signal
};

struct Storage {
    Signal      signal;
    StorageKind kind;      // Comb | Latch | Flop | AsyncLoad | Memory | Unclassified
    GuardId     guard;     // the path, when kept
    Decomp      decomp;    // clock, edge, async list, sync reset, enable, next
};

// The async entry is a (condition, value) PAIR LIST, priority-ordered -- not a
// (trigger, value) pair. §4A.10: the condition can involve signals that are not
// triggers, and one trigger can carry several values.
struct AsyncReset { Index condition; Ref value; };
}
```

`facts` is a pointer and may be null: absent means the `may_x`-dependent findings
are skipped, not guessed.

### 11.6 Three rules

**Never decides severity.** The library says "signal `y` is a latch"; whether that
is an error is `DiagnosticPolicy`'s call, which belongs to the caller. `Note`
carries a `Finding` enum plus structured operands — signal, span, the guard that
defeated a match — and no severity. The frontend maps `Note` → `Diagnostic`.

**Never renders.** No `SourceContext`, no formatting. A `describe(const Note&)`
convenience is fine, on the same terms as `Diagnostic::rendered`; anything but
printing ignores it.

**Never mutates.** Input is `const ir::Module&`. The no-rewriting invariant
expressed as a signature.

### 11.7 How it gets verified

- **The 28-case reset corpus** (§4A) as classification tests.
- **Differential against yosys** where yosys agrees: `$adff` ⇔ Flop-with-async,
  `$dlatch` ⇔ Latch, `$dff` ⇔ Flop. Divergences are either a Clotho bug or one of
  the six §4A.1 shapes yosys rejects — and which it is must be stated per case, not
  assumed.
- **Materialize round-trip, no external tool and no S7 needed**: render the filled-in
  form, feed it back through Clotho's own frontend, re-run legalization, and require
  an identical classification. Self-checking, and it exercises exactly the code path
  S7 will depend on.
- **Totality**: every written signal gets exactly one classification, asserted over
  the whole corpus — the same shape of sweep as `operands_test`.
- **Budget**: hazard3 legalization measured against the 11.5 ms `DesignFacts` walk,
  in CI, as a number that must not regress.

### 11.8 Explicitly out

Cells and netlists; technology mapping; anything timing; the SV writer (S7);
severity policy; mux-tree minimisation and ROM inference in v1 (§5.4 is a later
QoR item, not a v1 obligation).

---

## 12. Sequencing

Each milestone is independently shippable and has an acceptance test that does not
depend on the next one.

**M1 — `ir::Eval`. LANDED** ("ir: partial evaluator for folding a cone with one signal pinned"). The three-valued partial evaluator in
`ir_core`: pin a signal to a constant, fold a predicate cone, return 0 / 1 /
unknown. Absorption (`x & 0`, `x | 1`) is load-bearing, not an optimisation:
without it R13's multi-reset idiom cannot fold at all. Two gaps found later by the
corpus and closed: the *eager* ternary needed the same treatment as the lazy one
(M4), and an unknown lazy-select predicate still folds when both arms agree (M3).

**M2 — the walk. LANDED** ("legalize: the guarded-assignment walk"). `clotho_legalize` stood up as a peer of
`clotho_ir`, not a layer inside `clotho_ir_core` — the simulator never consumes
legalization and does not link it; zero LLVM and zero slang symbols, checked with
`nm`. Guard arena, per-signal guarded lists, and the legality gate as a
`synthesizable` field on the existing exhaustive `TagInfo` table rather than a
parallel switch, so a new opcode is classified once for purity, thunk-blocking and
synthesizability together. 62 tags are pure and synthesizable, 22 pure but not
hardware, 6 synthesizable but not pure.

Two corrections the tests forced. Hold is a per-*path* property, not a comparison
of sibling arms: `q = b; if (en) q = a;` assigns on every path. It is a must-assign
lattice — sequence unions, `cond` intersects, `switch` intersects arms with its
default, and a *missing* default contributes nothing. And the walk has to descend
into `Loop`, or `always begin #5 ...; end` — a forever wrapper — never has its
`Delay` seen and passes the legality gate.

**M3 — classification. LANDED** ("legalize: classify storage from structural completeness", corpus "legalize: run the 28-case reset corpus as a test"). Comb / Latch / Flop
/ AsyncLoad by pinned re-walk rather than pattern peeling. *Measured:* Clotho
agrees with yosys 0.67+92 on all 22 corpus cases yosys accepts, and classifies all
6 it hard-errors on (R04, R13, R19, R24, R25, R25c). The corpus lives in
`tests/legalize/reset/` and runs in CI.

R22 diverges deliberately: pinning `rst` in `if (rst && en)` leaves `if (en) q<=0;
else q<=a`, so the edge *does* assign, with a mux value. `AsyncLoad` describes that.
yosys rejects it by complaining about the sensitivity list, which is not where the
problem is — so the plan's "reject R22" is now "classify R22 as what it is", and
M5's acceptance changes accordingly.

Known limits, both conservative: `is_constant` sees through the width and domain conversions and `Concat`
but does not fold arithmetic, so `8'h0F + 1` as a reset value reads as an async
load; and a reset routed through a function call still degrades to a rejection.

**M4 — Materialize. LANDED** ("legalize: materialize a design back to SystemVerilog"). The fill-in renderer (§6), accepted by
the §11.7 round trip over the whole corpus.

Two design facts came out of making that pass. **Flattening is wrong**: one guarded
`if` per store reads naturally and destroys exhaustiveness, so every signal
re-lowers as held and every reset stops being a reset. The guard arena is a prefix
tree, so the renderer rebuilds the nesting exactly. **The fill-in is asymmetric**:
for an edge-triggered signal a missing arm renders as `else q <= q`, an ordinary
feedback mux and exactly what a clock enable is; in a level-sensitive block the
same text is a combinational loop, so a latch's hold is described (`// q holds:
latch`) and never spelled. Spelling an unreachable base as `q <= 'x` — which looked
like the symmetric answer, and is what §6 originally proposed — is actively harmful:
the unguarded store survives into every pinned residue and makes ordinary flops
classify as async loads.

`full_case` / `parallel_case` are absent rather than stubbed; the pragma data is M7.

**M5 — decomposition. LANDED.** Clock and edge, async pair lists, sync reset,
enable, next-state cone. *Measured:* all four §4A.10 orderings decompose as the
simulations said they must, and R22 now carries a `Finding::AsyncLoad` naming the
signal, with the conditions in its async entries.

Two shape corrections against §11.5. `Entry::value` is an `ir::Index`, not a `Ref`:
a `Ref` cannot hold an async LOAD, whose value is a cone by definition, and
constness is already answerable with the value in hand — one shape covers reset,
conditional reset and load instead of three. And the async list is grouped
per-trigger (`AsyncGroup`), because a design with two asynchronous resets needs to
say which edge carries which values.

`enable` is `¬hold` and nothing more. When a signal is not always enabled the
enabling condition is the disjunction of its entries' conditions, which the caller
can form; legalize does not, because building it would mean creating IR.

`ir::Eval` grew a multi-pin form. The synchronous body is what runs with *every*
async trigger deasserted, and one pin cannot express two of them.

The quadratic came back and measurement caught it again. The sync residue was
computed per signal, taking hazard3 from 0.95 ms to 2.4 ms; it depends only on
which trigger is the clock, so it is memoised on that and hazard3 is 1.2 ms — one
extra walk per distinct clock, which is what M5 should cost. Second time this exact
mistake has been made in this file, and second time only the real-design
measurement showed it.

**M6 — coverage. LANDED.** Fullness of a defaultless case, by counting.

The exponential the doc warned about never materialises, because the question is
narrower than it looked: a `Switch` only ever carries fully-defined, deduplicated
keys — `find_switch_run` rejects `hasUnknown()`, so casez / casex / unknown keys go
down the cond-chain path and never reach a switch. With no wildcards to expand and
no overlaps to subtract, fullness is `distinct keys == 2^width`: O(arms), no
allocation, exact at any width the frontend can state. Neither yosys's
wildcard-density explosion nor enumeration's width ceiling applies.

`SwitchInfo` gained `sel_width` and `sel_four`, because coverage cannot recover
them — keys are bare u64 and the IR has no type query for an instruction's result.
`sel_width` is the selector's VALUE width, deliberately not the key encoding: a
4-state key packs value and unknown bits into `2*width`, and asking `2^(2W)` of it
was the first version's bug. Width 0 means the frontend declined to state one (the
one-hot lowering, whose key is a bit index from ctz — a one-hot case is full only if
the selector is provably one-hot, which the switch alone cannot say), and coverage
declines rather than guessing.

**A new finding falls out: `FullCaseXSelector`.** A fully enumerated case over a
4-state selector is complete for the synthesiser and *not* for the simulator —
`case` matches with `===`, so an x/z selector matches no arm and the signal holds.
Combinational for one, a latch for the other, from one source. Legalization takes
the synthesis reading and names the divergence rather than silently picking a side;
it fires 6 times on picorv32 and twice on hazard3.

Cost, measured: hazard3 1.2 ms -> 2.1 ms, picorv32 0.8 -> 1.2. Defaultless cases
used to short-circuit in `must_assign` and now have every arm intersected, which is
the work coverage buys. Still linear, still ~6000x under yosys's `proc`.

**M7 — `full_case` / `parallel_case`. LANDED.** One field each, as predicted, plus
one thing the sketch did not anticipate.

`full_case` is `Options::honour_full_case`, off by default because the LRM says an
uncovered selector holds and that has to be Clotho's default answer whatever the
source claims. Turned on, the base slot becomes don't-care instead of hold and the
latch disappears. §8's warning holds up exactly: this only works because the base is
a property of the ANALYSIS. yosys must do it in its frontend, since by the time its
legalizer runs the lowering has committed to a hold.

The claim is reported where it is relied upon. `Finding::FullCasePragmaUnsound`
fires when the pragma is honoured, the arms do not cover the selector, and the
default assigns nothing — that is, when the pragma is what removed the hold, which
is precisely when simulation and synthesis will disagree. A truthful `full_case` is
a redundant claim and gets no note; a `priority case` with a real default is
complete anyway and gets none either.

`unique` / `priority` / `unique0` map onto the two bits the way yosys's parser and sv2v do.
That surfaced the thing the sketch missed: `unique` also lowers a violation report,
and it lands in the DEFAULT region. So the default is not empty, and treating
"declared full" as "no default" was not enough — the default has to be excluded
from the intersection as *unreachable*, which is what the pragma actually claims.

It also surfaced an over-rejection. `Inst::Tag::CaseViol` was unsynthesizable, so
every `unique case` in a modern design rejected its whole process. Unlike `$display`
this is not user-written I/O: it is the language's own simulation-time assertion,
with no hardware of its own, and its synthesis meaning is exactly the pragma pair
already being honoured. Now synthesizable.

`parallel_case` is carried to the guard atoms for consumers that want a one-hot mux,
and for a case that lowered to a `switch` it is provably a restatement: the frontend
deduplicates keys, so the arms are disjoint whether or not anyone says so. It
carries information only on the cond-chain path, where casez patterns can overlap.

**M8+ — QoR and breadth.** ROM inference (§5.4), jump statements (§5.3) if real
designs need them.

M1–M4 are the useful core: they make every always-block in a design inspectable as
the hardware it implies — the research feature, the LSP content, and the synthesis
frontend's foundation — while emitting nothing and adding no dependency.

### Open, before M1

- **`ir::Eval`'s reach.** Folding stops at signal loads, but a reset condition
  through a function call needs either inlining or an honest "cannot prove". Pick
  one before building, because it sets the coverage ceiling.

(Edge-on-an-expression was open here and is now resolved in §4A.9; its residual
case, cone equivalence for an expression *reset*, is deferred past M5 by choice.)
