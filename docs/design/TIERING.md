# Tier-Up Profiling and Code Swap (design sketch)

Captured from a design discussion 2026-07-06; revisit before implementing.
Context: tier 0 (fast pipeline + FastISel) landed; a `default<O3>` tier is
planned; no interpreter/baseline tier exists yet.

---

Two separable mechanisms: deciding *what/when* to promote (profiling), and
*how* to swap running code (the hard part). General JIT machinery first, then
what's Clotho-specific — because Clotho's shape changes the answer a lot.

## Standard machinery (V8 / JVM / .NET style)

**Profiling** — tier-0 code carries cheap counters: one per function entry,
one per loop back-edge (so a function entered once but spinning a hot loop
still gets caught). Counter crosses threshold → function enqueued for
background compilation at the higher tier. Alternatives: sampling (a timer
interrupt records PCs; zero instrumentation overhead, coarser) — .NET and some
JVMs mix both. Thresholds are usually cost-aware: hotter threshold for bigger
functions, since compile time is the price.

**Swap** — two cases:

- *Next call*: function entry goes through one indirection (a
  slot/trampoline); background compile finishes, slot is atomically
  repointed. Trivial.
- *Currently executing* (hot loop that never returns): **on-stack
  replacement** — at a loop back-edge safepoint, live state in the tier-0
  frame is mapped into the tier-1 frame layout and execution resumes
  mid-function in new code. This is the expensive, bug-rich machinery; JITs
  only build it because long-running loops otherwise never benefit.

## Why Clotho is a much easier target

**Suspends are free safepoints.** Every process is a coroutine that parks at
`@(...)`/`#delay` constantly — thousands of times per second of sim time. You
never need OSR: nothing needs replacing *mid-execution*, because execution is
almost always suspended. The scheduler is the single dispatch point.

**The real problem is coroutine frame layout, not code swap.** Tier-0 and
tier-1 compiles of the same process run CoroSplit under different
optimization, producing *different frame layouts* (different spill sets). You
can't repoint a live frame's resume pointer at tier-1 code. Three ways out:

1. **Quiescent-point migration**: most hot processes are `always` blocks
   parked at their steady-state wait with essentially no live locals —
   everything persistent lives in the signal state struct, which has identical
   layout across tiers (backend-deterministic, independent of the pass
   pipeline). At end of a time step, for each process parked at its
   steady-state suspend: destroy tier-0 frame, create tier-1 frame, run it to
   the same parked wait (the self-start path already does exactly this at t0),
   re-register its awaiter in the wait queues. Processes mid-body (rare) keep
   tier-0 code until they next park.
2. Pin the frame ABI so layouts match — fights CoroSplit, don't.
3. Compile both tiers upfront — defeats the latency goal.

Option 1 is the design; the wait-queue re-registration is the fiddly bit
(queue entries point at awaiters inside frames).

**Profiling can live entirely in the runtime — no codegen support.** The
scheduler already touches every activation: the active-queue push (with its
`proc_sched` dedup) and wheel ticks can bump a per-process counter for free.
Resume-count correlates well with work in event-driven sim; back-edge counters
only matter for processes doing huge delay-free internal loops, which are rare
and scheduler-bounded.

## Pragmatic staging for Clotho

- **v0 — no per-process profiling at all**: wall-clock trigger. Sim still
  running after ~250ms → kick one background O3 compile of the *whole module*
  (ORC handles concurrent compilation; same IR module, different pipeline),
  swap everything at the next quiescent point via migration above. Profiling
  degenerates to "when", not "what" — right trade at Clotho's current scale,
  since designs are one module and O3-compiling everything in the background
  costs only background CPU.
- **v1 — per-process counters** if whole-module O3 compile time gets painful
  on big designs: promote only processes above a resume-count threshold, one
  function each, keeping tier-0 for the cold 90%.
- The interpreter/baseline tier slots *under* this unchanged: it's just
  tier-minus-1 with the same scheduler-dispatch swap point; frames become
  interpreter-owned structures, which actually makes migration easier, not
  harder.

The quiescent-point migration invariant ("steady-state suspends carry no live
frame state") is a codegen contract the backend should start honoring
deliberately rather than accidentally.
