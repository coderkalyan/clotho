# Static unrolling for trivially decidable loops

Scoping. Phase 1 is `for` loops whose trip count is a compile-time constant;
everything else declines and says so.

## 1. Why this exists

Legalization currently treats every loop the same way (§ process-legalization,
"Loops: four decisions"): descend for legality, leave the guard unchanged, contribute
nothing to completeness, and mark every store `in_loop`. That is correct and
conservative, and it costs three things:

* **Classification.** A signal written only inside a loop is always a latch, because
  a loop body may run zero times. For a loop with a constant trip count that is
  simply false.
* **Rendering.** `materialize` emits a loop body once with an inline caveat, and the
  round trip excludes those signals — 46 on hazard3, 2 on picorv32. They are the
  only exclusions left.
* **Guards.** A store in the `step` region is under-guarded when the body can
  `break`, the same defect the body itself had before jumps became guard terms.

## 2. Where it happens, and why that is not IR rewriting

**In the walk, not in the IR.** Unrolling here means *iterating the analysis*, not
producing new instructions: the walker visits the body N times with the induction
variable pinned to a different constant each time. No `Builder`, no new `Index`, no
second IR. `ir::Eval` already pins signals and folds cones; this is that mechanism
applied N times.

This matters beyond purity. An unrolled IR would double the simulator's code size
for no benefit — simulation wants the loop. Keeping unrolling inside the analysis
means the two consumers of the same IR get what each needs, which is the whole
premise of the shared frontend.

## 3. What "trivially decidable" means

A loop qualifies when **all** of the following hold. Anything else declines.

1. **A single induction signal `S`.** The instruction immediately preceding the loop
   is `store(signal(S), C)` with `C` a constant.
2. **The predicate reads only `S` and constants.** Any other signal in the
   predicate's cone means the trip count depends on runtime state.
3. **The step assigns only `S`,** with a value cone reading only `S` and constants.
4. **The body does not assign `S`.** Otherwise the step does not control the
   induction.
5. **The predicate goes false within the cap** (§7), evaluated concretely.
6. **No nested `Loop` in the body that itself declines.** Nested trivially-decidable
   loops unroll recursively; one that declines poisons its parent, because the
   parent cannot know what the inner one wrote.

Deliberately *not* required: that the body be free of `break`/`continue` (§6), or
that the writes be whole (§8).

## 4. The algorithm

```
v := init constant
k := 0
loop:
    if Eval(pred, pins = {S := v}) is not True: stop        # normal termination
    if k >= cap: decline the whole loop                     # never guess
    walk(body, guard, pins = {S := v})                      # one iteration
    v' := Eval(step_rhs, pins = {S := v})
    if v' is not fully known: decline
    v := v'; k := k + 1
```

Three properties worth stating:

* **Termination is decided, not assumed.** The predicate is evaluated, not
  pattern-matched against `i < N`. `i != 7`, `i <= N-1` and a countdown all work if
  they fold.
* **The pin composes with the existing one.** `ir::Eval` already takes several pins
  (added for M5's synchronous residue), so an unrolled iteration inside a pinned
  reset walk is just three pins instead of two.
* **The walk is unchanged.** It already threads a guard and already narrows it past
  jumps. Unrolling adds a pin and a loop around the call.

## 5. Prerequisite: `ir::Eval` must learn arithmetic

Today `Eval` handles `Band Bnot Bor Bxor CaseEq Constant Eq Land Lnot Load Lor Ne
RedAnd RedOr Resize Select SelectLazy` — enough for reset predicates, and **not
enough for a single loop bound**. Required additions:

| opcode | needed for |
|---|---|
| `Lt` `Le` `Gt` `Ge` | the termination test |
| `Add` `Sub` | `i = i + 1`, countdowns |
| `Mul` `Sll` `Srl` | `i = i * 2`, `i <<= 1` — common enough to include |

**Signedness is the trap.** `Eval` has no type query (the same gap that made
`SwitchInfo` carry `sel_width`), so it cannot tell a signed compare from an unsigned
one. Rather than add a payload bit, fold a relational op only when **both operands
have their most significant bit clear at the compared width**, where the two
readings agree. Loop counters satisfy this trivially; anything else declines. No IR
change, no wrong answer.

## 6. `break` and `continue` stop being the same thing

Today both mean "the rest of this body does not run", which is all the walk asks.
Under unrolling they differ, and getting this wrong would be silent:

* **`break` in iteration `k` under condition `p`** skips the rest of iteration `k`
  *and every later iteration*. The accumulated `!p` must carry **forward across the
  iteration boundary**.
* **`continue` in iteration `k`** skips the rest of iteration `k` only. Iteration
  `k+1` starts with the guard as it was before.

This falls out of the existing narrowing rule if unrolled iterations are walked as
one flat sequence and the guard is threaded between them for `break` but reset for
`continue`. `region_jumps` already distinguishes the opcodes; the walk currently
does not, and must.

An unmodelled jump (`Jumps::Maybe`) inside a loop being unrolled declines the loop
rather than the process — the loop is what cannot be represented.

## 6A. Measured against the RTL that has to work

44 `for` loops across hazard3 and picorv32. Every bound is a **parameter** —
`W_DATA`, `XLEN`, `PMP_REGIONS`, `NUM_IRQS`, `FIFO_DEPTH`, `regfile_size` — and not
one is a literal. That is the load-bearing assumption of this whole scope, so it was
checked rather than assumed: elaboration resolves parameters before lowering, and
`for (i = 0; i < W; ...)` with `W = 8` reaches the IR as

```
%6 = constant(signed logic[31:0] 0x00000008)
%7 = lt(%5, %6)
```

So condition §3.2 ("the predicate reads only `S` and constants") is satisfied by
every loop in both designs. Trip counts top out around 32 (`W_DATA`, `XLEN`,
`regfile_size`), which is what sets the cap below.

Distribution, for sizing: hazard3_pmp 11, hazard3_alu 6, hazard3_triggers 6,
hazard3_irq_ctrl 4, the rest 1-3 each; picorv32 3.

## 7. Caps, and what declining means

| cap | proposed | rationale |
|---|---|---|
| iterations per loop | 256 | hazard3 and picorv32 top out at 32 (`W_DATA`) |
| unrolled stores per loop | 4096 | guards the `N x body` product, not just `N` |
| nesting depth | 4 | the product is what explodes |

Declining is per loop and leaves today's behaviour exactly: guard unchanged, no
completeness contribution, stores marked `in_loop`. A declined loop is therefore
never *worse* than now, which makes this safe to enable by default. A
`Finding::LoopNotUnrolled` carries the reason, since "why is this still a latch" is
the question a user will actually ask.

## 8. What phase 1 buys — and what it does not

**Buys:**

* Whole-signal writes inside a constant-trip loop become definitely assigned, so
  those signals stop being latches.
* Stores become concrete: no `in_loop`, so `materialize` renders them and the round
  trip covers them.
* Per-iteration guards are exact, including across `break`.

**Does not buy — and this is the honest part.** The commonest loop idiom in real RTL
is a *partial* write:

```systemverilog
for (i = 0; i < 32; i = i + 1) op_a_rev[i] = op_a[31 - i];
```

Unrolled, that is 32 stores to `op_a_rev[0]`..`op_a_rev[31]` — each a `BitSelect`
lvalue, none whole, so `must_assign` still counts none of them and the signal is
still a latch. **Unrolling alone does not reclassify hazard3's bit loops.** What does
is bit-level completeness: accumulate a written-bit mask per signal and treat the
signal as assigned when the mask covers its width. That is a small, cheap follow-on
(a `uint64_t` mask, or a bitset above 64 bits) and it is *only* reachable once the
indices are constants — which is what unrolling provides.

Phase 1 is therefore the enabler, and phase 2 is where the visible win lands. Both
should be measured on hazard3's 46 excluded signals; anything less than most of them
becoming renderable and combinational means the scoping was wrong.

## 9. Risks

* **Analysis time.** `N x body` walks, three times over (base, per-trigger, sync).
  hazard3 is 2.0 ms today; a 32x unroll of its bit loops is the first thing to
  measure, and the caps exist for this.
* **Guard arena growth.** Prefixes are shared, so N iterations of the same body share
  everything above the iteration; the growth is in leaves, not paths.
* **The induction variable's own stores.** `i = i + 1` cannot be recorded per
  iteration: the value is a constant the IR has no `Index` for, and inventing one is
  rewriting. Phase 1 leaves `S`'s own stores exactly as today (single entry,
  `in_loop` retained). A consumer wanting `i`'s final value can have it as a plain
  integer on the loop's record; it is known, it just is not an `Index`.
* **Behaviour change for existing consumers.** Signals move from Latch to Comb. That
  is more precise, and the round trip over two CPUs is the check.

## 10. Milestones

| | | acceptance |
|---|---|---|
| **U1** | `Eval` arithmetic + relational, MSB-clear signedness rule | folds `i < 32` and `i + 1` for every iteration of a 32-trip loop |
| **U2** | recognition + decline, no unrolling yet | the six §3 conditions, each with a test that declines for the right reason |
| **U3** | unroll in the walk, pins threaded | whole-signal writes in a constant-trip loop are definitely assigned |
| **U4** | `break` forward, `continue` local | the two differ, proven by a test where the distinction changes the guard |
| **U5** | bit-level completeness (phase 2) | hazard3's bit-reversal signals become Comb and render; round trip still exact |

U1–U2 are independently shippable and change no behaviour. U3 is the first one that
moves a verdict.
