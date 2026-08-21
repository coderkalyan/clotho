# Cooperative `$finish`

Replace the `longjmp` out of live JIT'd coroutine frames with an ordinary
return, without paying for it in designs that never call `$finish`.

## Why

Three open problems are the same defect.

| symptom | where |
|---|---|
| Windows cannot run any design that calls `$finish` (process dies, exit 127, both arches) | docs/CI.md |
| linux-arm64 SIGSEGVs intermittently, ~3 failures in 14 CI runs | docs/CI.md |
| the test harness must run `$finish` designs in a child process | tests/Subprocess.h |

`rt_finish` longjmps to a `jmp_buf` installed by `rt_sim_run` (and, outside the
drain, by `run_jit`). The jump crosses live coroutine frames belonging to every
process on the stack. Consequences:

- **Windows.** `longjmp` there is SEH-based: it *unwinds*, and unwinding needs
  valid unwind data for every frame it crosses. JIT'd frames register none, so
  the unwind aborts the process instead of arriving at the `setjmp`. glibc and
  Darwin restore registers without walking handlers, which is the only reason
  the current design works at all.
- **Everywhere.** The abandoned frames leak, and any runtime global caught
  mid-update stays that way. `emit_active_run` already compensates at *startup*
  — `nba_arr_reset`, `intra_reset`, both commented "a prior `$finish`" — which
  is the wrong end of the problem and does not cover the rest (wait queues, the
  observed/deferred queues, string pools, monitor and RNG state).

## The overhead constraint

Today the no-finish path costs **nothing**: `longjmp` is free until taken, and
`noreturn` on `rt_finish` lets the optimizer prune the dead tail after a
`$finish`. Any cooperative scheme must not regress that. Three properties keep
it at or near zero:

1. **The finishing function pays nothing.** A `Finish` op becomes a branch to
   its own function's epilogue. Everything after it in that block is dead by
   construction (LRM 20.2 — simulation ends), so the tail stays `unreachable`
   and prunes exactly as it does now. `noreturn` comes off `rt_finish`, but the
   call site keeps the same shape.

2. **Only *may-finish* call sites pay.** A memoized transitive closure over the
   call graph — the same shape as `close_fn` (LlvmBackend.cpp:4397) — marks a
   function or task may-finish if its own IR contains `Finish` (or `$fatal`, or
   the oscillation abort) or if it calls a may-finish callee. Only those call
   sites get a post-call test. In typical RTL `$finish` sits in one testbench
   `initial` block that calls nothing, so **no call site is instrumented at
   all**.

3. **The drain loop tests the flag once per settle wave, not per activation.**
   Measured, because the first attempt got this wrong: testing after each resume
   cost **+2.2%** on picorv32 (`sim_drain` is codegen'd at `-O0`/FastISel, so a
   load and a branch on the per-activation path are real instructions). Moving
   the test onto the queue-empty edge — and having the finishing body empty the
   active ring itself, so its wave ends at once — brought that to **+0.95%**.
   A design containing no `$finish` at all skips the test entirely and pays
   nothing.

Phase 0 measured (2) before any of it was committed to. `--stat=finish`
prints the closure and the call sites it would instrument:

| case | processes | with `$finish` | subroutines | may-finish | instrumented call sites |
|---|---|---|---|---|---|
| picorv32 | 201 | 2 | 7 | 2 | **4** |
| hazard3 | 1006 | 2 | 6 | 0 | **0** |
| coremark_cv32e40p | 2199 | 3 | 2 | 0 | **0** |

Two of the three real cores need no instrumentation at all; picorv32's four
sites are its testbench AXI error handlers (`handle_axi_rvalid` /
`handle_axi_bvalid`, both containing a direct `$finish`). The premise holds:
the scheme is free in the designs that matter.

## Protocol

`rt_finish_coop` sets `rt_finishing` and returns instead of jumping. Nothing in
the simulator longjmps any more; the only surviving non-local exit is the
oscillation abort, which `std::exit(1)`s because a non-converging design has no
settled state to hand back.

- **Process / task coroutine body.** Branch to the coroutine's cleanup, so it
  returns normally, runs `coro.end`, and frees its own frame. The leak goes away
  as a side effect.
- **May-finish call site.** Test the flag after the call; if set, branch to the
  caller's epilogue. A function returns a poison value — it is dead, nothing
  reads it.
- **Drain loop.** Exits when the flag is set. `rt_sim_run` then runs `final`
  blocks and returns normally; its inner `setjmp` disappears.
- **Teardown.** After the finals, destroy the coroutine frames still parked at a
  suspend (the scheduler holds every handle from the seed loop), close the VCD,
  and reset the runtime globals. The startup compensations in `emit_active_run`
  move here, where they belong.

## Phases

| # | scope | verified by |
|---|---|---|
| 0 | **done** — may-finish closure, no codegen change; `--stat=finish` prints the instrumented-call-site population | measured above: 4 / 0 / 0 |
| 1 | **done** — cooperative return for process bodies + drain loop; longjmp retained for subroutine bodies (phase 2) | suite 1206/1206; picorv32 runs; +0.95% sim |
| 2 | **done** — cooperative return from tasks and functions, may-finish call-site propagation, `longjmp` and both `jmp_buf`s deleted | suite 1206/1206; picorv32 runs; +1.4% sim |
| 3 | teardown/reset; delete tests/Subprocess.h; run two simulations in one process | the suite passes in-process; arm64 flake gone over 20 runs |

Each phase is independently landable and independently revertable.

## Risks

- **`$finish` inside a DPI-C export.** The design calls out to C, which calls
  back into a JIT'd export that finishes. A cooperative return cannot unwind the
  foreign C frames in between, and today's `longjmp` only "works" by skipping
  them (already dubious for any C++ frame with a destructor). Needs either a
  loud guard or a documented longjmp fallback confined to that boundary. This is
  the one case that may not fully convert.
- **`$finish` inside a plain function** reports the real simulation time: the
  wheel is a module global (`clotho.wheel`) that every body binds, so there is no
  0 fallback (the old `TODO` at the `finish_fn` declaration claimed one and was
  stale; tests/llvm_backend_test.cpp covers it).
- **`$stop`** shares the path and currently terminates like `$finish`; behavior
  is unchanged here, not corrected.
- **Perf.** Measured on picorv32 (`--time` sim split, true same-tree
  interleaved min-of-5): +0.95% after phase 1, **+1.4%** after phase 2. The
  increment is its four instrumented call sites, which sit on the testbench's
  hot AXI path. hazard3 and cv32e40p instrument nothing and should be free.

## Non-goals

Convergence with iverilog on `$stop`, and the LRM-correct interactive-`$stop`
semantics. Untouched.
