# Bringing up a debug adapter

Scope out `clotho dap`: a stdio Debug Adapter Protocol server that stops, steps
and inspects a running simulation. Expands `lsp.md` §8.10 and §9's one
scheduled row (`| — | **DAP debug adapter** — separate protocol, same binary
(§8.10) | L3 | medium | an RTL debugger; not an LSP stage |`,
`docs/design/lsp.md:1282`), which is currently two sentences.

Status: **D0 has landed.** `clotho dap` (`src/dap/`, target `clotho_dap`)
serves the base protocol over stdio — framing, `seq` discipline, the
`initialize` → `initialized` → `configurationDone` → `launch` handshake — runs
a design as a child process, streams its stdout and stderr as `output` events,
and reports `exited` + `terminated` with the design's own status. Every
stop-shaped capability is absent and every stop-shaped request is refused by
name with the stage that implements it. Verified end to end and covered by 13
protocol tests in `tests/dap_test.cpp`, which drive the real process over
pipes. §5 is what that is and is not; D1 onward is unbuilt.

The framing layer moved out from under the language server on the way:
`src/lsp/Transport.{h,cpp}` is now `src/transport/` in namespace
`clotho::transport`, its own dependency-free target, linked by `clotho_lsp` and
`clotho_dap` alike (§8.1). Nothing about it changed — it already declared
itself JSON-agnostic in its own header comment (`Transport.h:13-16`).

The headline: **the protocol is nearly free and the target is not.** DAP's base
protocol is byte-identical to LSP's, so the wire layer was a move rather than a
rewrite, and glaze was already pinned and in the graph
(`CMakeLists.txt:217-222`). What is
expensive is that Clotho's simulation is a *single monolithic JIT'd function
entered exactly once* (`Jit.cpp:594`) with no debug info, no unwind data, no
process registry and no runtime symbol table. A debugger cannot stop it from
outside; it can only stop where the generated code already calls back into the
runtime. Everything below follows from that one fact.

The consequence for ordering is pleasant and non-obvious: **the single most
valuable feature — stop on assertion failure and show me where — needs no
codegen change at all**, because four report choke points already exist and
already carry a compile-time `file:line` banner. It is D1. Statement stepping,
which sounds like day one, is D3 and is the only item that requires new
per-statement code emission.

---

## 1. Where we actually are

**The wire layer is done and does not know it.** `FrameReader` / `FrameWriter`
/ `configure_stdio` (`Transport.h:34,72,88`) are ten entities across
300 lines that know only `Content-Length`, descriptors and `read(2)`/`write(2)`.
`FrameReader::extract` already skips every header that is not case-insensitively
`content-length` (`Transport.cpp:103-105`), so DAP's optional
`Content-Type: application/vscode-jsonrpc; charset=utf-8` costs nothing. The
batching semantics that make LSP `$/cancelRequest` work (`Transport.h:22-32`)
make DAP `cancel` work for the same reason. Reusable verbatim; only the
namespace and path say `lsp`, and there are two consumers
(`Server.cpp:29`, `Transport.cpp:1`).

**glaze is already the JSON layer, on conventions that transfer.** v8.0.0 by
FetchContent with a full-SHA pin (`CMakeLists.txt:217-222`), `PRIVATE` to
`clotho_lsp` on the stated rule that no header names a JSON type
(`CMakeLists.txt:429-431`). Pure C++ reflection over plain aggregates — there
is not one `glz::meta` specialization in the repo. The two patterns a DAP
server needs both exist: the deferred-payload envelope
(`glz::raw_json_view params`, `Server.cpp:98`) and the templated response
writer (`ResultResponse<Result>`, `Server.cpp:112-117`, `:1088-1095`).

**The server topology transfers; the server does not.** Everything from
`Server.cpp:60` to `:1356` is inside an anonymous namespace, and `serve()`
(`Lsp.h:21`) is the sole export, so nothing is linkable. What ports as *design*
is roughly 250 of 1364 lines: `Channel<T>` (`:469-522`), the
reader/writer/worker three-thread topology with a single-owner main loop
(`:809-864`), the sorted dispatch table with a compile-time sortedness
`static_assert` (`:696-735`), the batch-parse-then-cancel sequence
(`:951-998`), the parent-process watchdog (`:560-573`, `:1164-1172`), and
`std::_Exit` termination (`:863`).

**Every IR instruction already carries a source location.** `Ir::loc(Index)`
(`src/ir/Ir.h:2262`) is backed by a dense column parallel to `tags_`
(`Ir.h:2427`); `Builder::emit(const Inst&, SourceLoc)` (`Builder.h:501`) takes
the location as a mandatory parameter with no default and pushes it
unconditionally, and `Builder::finish` asserts the columns stay equal length
(`Builder.cpp:58`). There is no representation for an unlocated instruction.
Construct-level spans exist too: `Process::span` (`Module.h:189`),
`Function::span` (`Module.h:432`), `AssertionMonitor::span` (`Module.h:329`),
`SignalInfo::loc` (`Module.h:42`).

**Locations survive lowering and already reach a protocol.**
`LowerResult::sources` is a never-null `shared_ptr<const SourceContext>`
(`Lower.h`), and `SourceContext` provides `resolve` → `{file,line,column}`
(`SourceContext.h:61`), `full_path` (`:81`, added precisely because the printed
path cannot become a URI) and `offset` (`:97`). The LSP's
`range_of(const SourceContext&, ir::SourceSpan, const LineIndex&)`
(`Server.cpp:409`) and `uri_from_path` (`:456`) are the conversion, and
`src/lsp/LineIndex.h` is the line arithmetic. All four are reusable; DAP is
1-based where LSP is 0-based, which is a `+1` in one place.

**The codegen knobs a debug mode needs already exist and already say so.**
`fuse` (`SimConfig.h:119`), `comb_fuse` (`:125`), `thunk` (`:132`),
`dedup` (`:140`), `outline` (`:147`) each carry a comment of the form
*"Disable for bisection / debugging; semantics are identical either way."*
They are deliberately not on the CLI. §4.2.

### 1.1 Blockers, verified

1. **The simulation has no re-entrant step and cannot be driven from outside.**
   `emit_main` (`ir/llvm/Scheduler.cpp`) emits `i32 @clotho_main()` which calls
   `rt_sim_run(state, wheel, drain, finals)` (`Runtime.h:364`), and
   `rt_sim_run` (`runtime/Scheduler.cpp`) is literally
   `drain(state, wheel); finals(state, wheel); sim_teardown();`. `sim_drain` is
   one monolithic JIT'd function holding the entire LRM 4.4 region model as
   basic blocks (`emit_active_run`, `ir/llvm/Scheduler.cpp`). The host
   calls it through `lookup("clotho_main")` → `rt_run(&config, fn)`
   (`Jit.cpp:585,594`) and gets control back when the run is over. There is no
   scheduler callback, no step entry point, and no way to return out of the
   drain and re-enter it. **This is the largest architectural fact in the
   document and §4.1 is its consequence.**

2. **Between two suspension points, a process body is straight-line native
   code with zero polls.** The only suspension mechanism is
   `emit_suspend` (`ir/llvm/Scheduler.cpp`) wrapping `llvm.coro.suspend`,
   and the complete set of sites is: initial suspend, the always-loop
   wait, `@`/`wait`, `#delay` and `#0`,
   a blocking task call, and `fork`/`join`. The one flag
   generated code tests to leave the loop is `rt_finishing` (`Runtime.h:124`),
   checked **only on the queue-empty edge** of the Active and Reactive drains
   (`ir/llvm/Scheduler.cpp, emit_active_run`) — deliberately: the comment there
   records that testing it after each resume measured +2.2% on picorv32. So
   the runtime cannot be halted between statements today.

3. **No LLVM debug info is emitted, anywhere.** Two independent repo-wide
   greps for `DIBuilder|DILocation|DISubprogram|setDebugLoc|DebugLoc|llvm.dbg`
   return zero matches. There is not even a `"Debug Info Version"` module flag.
   Locations reach machine code only as baked C string constants at four report
   sites (`loc_text`, `ir/llvm/Backend.cpp`; `clotho.bounds.site`;
   `clotho.null.site`; severity/`$finish` banners).

4. **JIT'd frames carry no unwind data.** Stated in `Runtime.h:116-117` —
   it is why `$finish` was converted from `longjmp` to a cooperative return. So
   a native stack walk of design code is unavailable even in principle, and the
   only JIT symbolization is the opt-in perf jitdump listener
   (`Jit.cpp:137-157`), which needs `--perf-jitdump` and an `LLVM_USE_PERF`
   build.

5. **No runtime process registry, id, or name.** A process is an LLVM
   coroutine (`coro.id`/`coro.begin`, `ir/llvm/Subroutines.cpp, lower_group`) whose frame
   comes from `rt_alloc` (`Runtime.h:1098`); the only runtime handle is the
   opaque `ptr` the ramp returns, living in the active ring
   (`@clotho.active`, a bare `[cap x ptr]`), a wait queue,
   a wheel timer, or a join record. Worse, process↔runnable is
   not 1:1 — fusion merges several `Process`es into one coroutine
   (`compute_fusion_groups`), thunk elision makes some processes
   frameless tagged function pointers, twin dedup shares one
   body across instances, and `Preponed`/`Observed`/`Final` processes are
   ramped→resumed→destroyed inline with no persistent identity at all.

6. **No runtime name→signal map.** Hierarchical names exist at compile time
   (`SignalInfo::name`, `Module.h:36`; `InstanceScope`, `Module.h:69-83`) and
   transiently as `rt_vcd_scope`/`rt_vcd_var` argument strings, but
   `vcd_register` (`runtime/Vcd.cpp`) writes the name into the VCD file
   and **does not retain it**. `vcd_index_` (`ir/llvm/Backend.h`) is
   compile-time and forward-only. DPI has exactly one flat scope
   (`Svdpi.cpp:21-30`).

7. **Change detection is conditionally emitted.** `service_after_write`
   (`ir/llvm/Events.cpp`) is emitted only when `signal_has_waiters(sig)`
   or the signal has a VCD index, and VCD indices are assigned only
   when the design source contains `$dumpvars`. A signal
   nobody waits on and nobody dumps has no change detection emitted at all.

8. **Frame locals have no names.** `Function::local_types` is
   `std::vector<Ref>` — types only (`Module.h:402`) — and the backend already
   reports the consequence: `bounds_name` returns the literal `"<local>"` for
   anything that is not a signal (`ir/llvm/Sanitizers.cpp`).
   `fn_local()` (`lower/Lowerer.cpp`) pushes a type and drops the slang symbol.

9. **Three paths `std::exit` past every possible hook**:
   `rt_oscillation_abort` (`runtime/Scheduler.cpp`), `rt_null_handle`
   (`runtime/Reports.cpp`, and `[[noreturn]]` in the header, `Runtime.h:288`), and
   allocation failure (`runtime/Reports.cpp, alloc_fatal`).

10. **There is no project model to hand a session.** `lsp.md` §11.6 records it:
    Clotho has no config file and no project concept; file sets and include
    paths are entirely slang's `SourceLoader` flags. §4.4 decides what a
    `launch` request resolves against.

11. **`clotho_lsp` must not host the adapter.** `CMakeLists.txt:423-431` states
    that the target's job is *partly to make the build fail if the server ever
    reaches for LLVM*. An adapter executes the design, so it links `clotho_ir`
    → LLVM 22 (`CMakeLists.txt:394`, `:131-133`) by construction. Separate
    target, mandatory.

12. **`Wheel` cannot be queried.** `WheelImpl::advance()` (`Wheel.h:95`) both
    advances and detaches; there is no peek-next-deadline API, and `Timer`
    (`Wheel.h:38`) has no owner back-pointer — the awaiter is recovered by
    `offsetof` from the embedding object. So "which processes are waiting on
    time, and until when" is not enumerable, and "run until time T" cannot be
    planned without stepping.

---

## 2. What DAP gives, and what it demands

Verified against `microsoft/debug-adapter-protocol@main`
(`debugAdapterProtocol.json`), not from memory.

| Need | Verdict | Detail |
|---|---|---|
| Framing | **identical to LSP** | `Content-Length` + optional `Content-Type`, UTF-8 body. `src/transport/Transport.*` verbatim, unchanged |
| Envelope | **differs, and is simpler** | `{seq, type}` base; request adds `{command, arguments}`, response adds `{request_seq, success, command, message?, body?}`, event adds `{event, body?}`. Ids are `int32` only — no string-or-int variant, so `Id` (`Server.cpp:80`) collapses to `int64_t` |
| `seq` discipline | **new obligation** | Every message in each direction carries a monotonic `seq` starting at 1. Today `send()` (`Server.cpp:1082`) queues an already-serialized string; `seq` must be stamped at serialization time on the dispatch thread, before the string enters `outgoing_` |
| Cancellation | **have the mechanism** | `cancel{requestId?, progressId?}`, best-effort by spec. The LSP's batch-prescan (`Server.cpp:951-998`) is the same shape |
| Lifecycle | **inverted vs LSP** | `initialize` → response → **adapter sends `initialized` event** → `setBreakpoints`/`setExceptionBreakpoints` → `configurationDone` → `launch`. LSP's `initialized` is client→server; DAP's is server→client. Plus `disconnect`, `terminate`, `restart` |
| Error reporting | **differs** | No JSON-RPC codes. `success:false` + `message` (short, machine-facing, `_enum: cancelled|notStopped`) + optional `body.error` as a structured `Message{id, format, variables}` |
| Program output | **have a channel** | `output` event with `category: console|important|stdout|stderr|telemetry` — the natural home for `$display` and for adapter logging, which the LSP has no analogue of (it sends nothing to the client but diagnostics) |
| Exception filters | **exactly our shape** | `Capabilities.exceptionBreakpointFilters: ExceptionBreakpointsFilter[]` with `{filter, label, description?, default?, supportsCondition?, conditionDescription?}`, plus `exceptionInfo` for the detail pane. **`default` is a C++ keyword** — the first `glz::meta` specialization in the repo (§8.3) |
| Data breakpoints | **exactly our shape** | `dataBreakpointInfo(name, variablesReference?)` → `dataId`, then `setDataBreakpoints` with `accessType: read|write|readWrite`. A `dataId` is an opaque adapter string, so a hierarchical signal path is a legal one |
| Stepping granularity | **fixed at three values** | `SteppingGranularity: statement | line | instruction`, gated on `supportsSteppingGranularity`. There is **no** extension point for simulator granularities; §3.5 decides what to do about it |
| Breakpoint modes | **an extension point, for breakpoints only** | `Capabilities.breakpointModes: BreakpointMode[]` — `{mode, label, description?, appliesTo}` — the mode string round-trips into `setBreakpoints`. Applies to source/instruction breakpoints, not to stepping |
| Independent thread resume | **must be declined** | `supportsSingleThreadExecutionRequests` + `singleThread` on `continue`/`next`. Meaningless here: one scheduler drives every process, so resuming one process while freezing others is not a state the simulator can be in. Always `allThreadsStopped: true`, capability off |
| Reverse execution | **exists in the protocol** | `supportsStepBack`, `stepBack`, `reverseContinue`. This is what a simulator can offer that a software debugger cannot (snapshot + deterministic replay) and it is out of scope for every phase below; §7 |
| Custom requests | **allowed, client must know them** | Adapter-specific commands are legal but invisible to a generic client. The universally available extension surface is the debug console: `evaluate` with `context: "repl"` |

---

## 3. The mapping, filled in

### 3.1 DAP threads ← simulation processes

A DAP "thread" is an independently-stoppable unit with its own stack. A
SystemVerilog process is an independently-*scheduled* unit that shares one
scheduler. The fit is good for enumeration and inspection and wrong for
resumption, so the mapping is: **one DAP thread per elaborated `Process`, all
stopped together, all resumed together** (`allThreadsStopped: true`,
`supportsSingleThreadExecutionRequests` off).

Thread id = the index into `Module::processes()`, offset so 0 is never a valid
id. Thread name = the owning instance's hierarchical path plus the activation
kind and declaration line — `top.cpu.regfile: always_ff @ regfile.sv:41`, built
from `Process::scope` (`Module.h:196`) → the `InstanceScope` tree
(`Module.h:69`) and `Process::span` (`Module.h:189`).

Blocker 5 is what this costs: nothing at runtime maps a coroutine handle back
to a process index. The fix is a **static process table** emitted at seed time
— `clotho.dbg.procs`, one record per process holding `{sched-flag address,
span, scope id, activation}`, registered once through a new
`rt_dbg_register_procs`. All four inputs already exist at codegen
(`Module::processes()`, `proc_sched_ord_`/`sched_flag_ptr`, `Process::span`,
`Process::scope`). Fusion, thunk elision and dedup break the 1:1 assumption, so
this only holds under the debug codegen mode of §4.2.

Which thread is "current" when stopped: the one whose body is executing. That
needs a single `clotho.dbg.cur_proc` global stored on entry to each process
body and restored on exit — one store per activation, in debug mode only.

**Processes that are not threads.** `Preponed`, `Observed` and `Final`
processes are ramped, resumed and destroyed inline (`src/ir/llvm/`)
and have no persistent identity. They get a thread id while running and
disappear from `threads` afterwards, which is exactly how DAP models
short-lived threads (`thread` event, `reason: started|exited`).

### 3.2 DAP stack trace ← the scope chain

DAP's `stackTrace` is an ordered list of frames, innermost first, each with an
optional `source` + `line`/`column` and a `presentationHint`. Clotho has three
kinds of frame and they are wildly different in cost:

| Frame kind | Source | Cost | Phase |
|---|---|---|---|
| **Instance hierarchy** — `top` → `top.cpu` → `top.cpu.regfile` | `InstanceScope` tree (`Module.h:69-83`), static, already walked for `rt_vcd_scope`/`rt_vcd_upscope` | free, except `InstanceScope` carries **no** `SourceLoc` (`Module.h:71-87`) — one frontend field | D2 |
| **Process** — `always_ff @(posedge clk)` | `Process::span` (`Module.h:189`) + the current statement location | cheap once the process table (§3.1) and a current-location store exist | D2 / D3 |
| **Task / function calls** | none — see below | expensive | D5 |

The last row is the hard one. Functions lower to real LLVM calls and tasks to
real coroutines (`Module.h:383-385`; `lower_task_call_inst`,
`ir/llvm/Processes.cpp, lower_fork_inst / ir/llvm/Subroutines.cpp, lower_task_call_inst`), so a logical chain exists — but a blocked task
stores only *its caller's own handle* in a heap record for resumption,
which is a single parent pointer, not a walkable stack. Add
blockers 3 and 4: no debug info, no unwind data. Across a suspension the
caller's native frame is gone entirely. **A task/function call stack must be an
explicit shadow stack that Clotho maintains itself**, pushed and popped by
emitted code in debug mode. That is real work and it is D5.

The presentation is worth stating because it is the differentiated bit: the
frames a hardware engineer wants are *mostly not call frames*. Showing
`top.cpu.regfile` → `always_ff` → `write_reg()` in one list, with the instance
frames marked `presentationHint: "subtle"` and given the instantiation site as
their source, is a view no software debugger produces.

`scopes` then hangs off a frame: an instance frame gets one `Locals`-kind scope
listing that instance's signals (`InstanceScope::signals`); a process frame
gets its enclosing instance's signals plus, later, its own automatics.
`variables` needs §4.3.

### 3.3 DAP exception breakpoints ← severity tasks and assertion failures

This is the best-supported mapping and the reason it is D1: **four choke points
already exist, and all four already carry a compile-time `file:line` banner.**

```c
void rt_severity(int32_t level, const char* banner, const char* fmt,
                 uint32_t argc, const rt_arg* argv);                  /* Runtime.h:150 */
void rt_defer_report(int32_t level, uint64_t is_final, const void* gen,
                     uint64_t gen_val, const char* banner, const char* fmt,
                     uint32_t argc, const rt_arg* argv);              /* Runtime.h:159 */
void rt_observed_case_violation(const char* banner, uint64_t now, int32_t kind); /* Runtime.h:137 */
void rt_finish_coop(const char* banner, uint64_t now, int32_t arg);   /* Runtime.h:119 */
```

Everything reportable routes through them: `$info`/`$warning`/`$error`/`$fatal`
(`lower/SystemTasks.cpp, lower_severity_task`), immediate assertion failure — which lowers to
`severity(Error, banner_at(site), "assertion failed", …)` (`lower/Lowerer.cpp`)
— deferred immediate assertions (`lower/Assertions.cpp, lower_deferred_assertion`), concurrent
assertion action blocks, `unique`/`priority` case violations
(`ir/llvm/SystemTasks.cpp, lower_case_viol`), and `$finish`/`$stop`.

Proposed filter set, with defaults:

| `filter` | `label` | Fires on | `default` |
|---|---|---|---|
| `fatal` | Fatal (`$fatal`) | `rt_severity(level=3)` | **on** |
| `error` | Error (`$error`, failed assertion) | `rt_severity(level=2)`, `rt_defer_report(level=2)` | **on** |
| `warning` | Warning (`$warning`) | `rt_severity(level=1)` | off |
| `info` | Info (`$info`) | `rt_severity(level=0)` | off |
| `case-violation` | `unique`/`priority` violation | `rt_observed_case_violation` | off |
| `stop` | `$stop` | `rt_finish_coop` with the `$stop` flavour | **on** |
| `finish` | `$finish` | `rt_finish_coop` | off |
| `bounds` | Out-of-range index | `rt_bounds_report` (`Runtime.h:256`) | off |

Two things this needs beyond the hooks. First, the banner is a *pre-rendered*
`"<file>:<line>: "` string (`banner_at`, `lower/Lowerer.cpp`); a `stopped` event
needs structured `(path, line, column)`. The fix is the pattern the codebase
already uses for exactly this: a constant site record beside the call, modelled
on `struct rt_bounds_site` (`Runtime.h:190-224`) which already carries `where`,
the source line text, `line_no`, `column` and `end_column`. Extending the
severity ABI to take a site pointer improves the existing diagnostics too.
Second, blocker 9: `rt_oscillation_abort`, `rt_null_handle` and allocation
failure `std::exit` past every hook. Under a debug session they must park
first — a real fix, small, and it makes those three failures debuggable for the
first time.

`exceptionInfo` then returns `{exceptionId: filter, description: the rendered
message, breakMode: "always", details: {message, stackTrace}}`.

### 3.4 DAP data breakpoints ← signal change watchpoints

DAP's flow is two-step: `dataBreakpointInfo(name, variablesReference?)` returns
an opaque `dataId` and the access types it supports, then `setDataBreakpoints`
arms them. A hierarchical signal path is a legal `dataId`, so the protocol side
is trivial. The mechanism is where the work is, and the template already exists:

```cpp
// src/ir/llvm/Vcd.cpp, emit_guarded_vcd_touch -- expands to `if (<gate byte>) { call fn(args...) }`,
// with make_args() run inside the taken branch so index math stays off the cold path.
void emit_guarded_vcd_touch(llvm::FunctionCallee fn,
                            llvm::function_ref<llvm::SmallVector<llvm::Value*, 2>()> make_args);
```

`rt_vcd_touch(uint32_t idx)` (`Runtime.h:579`) already fires after every
blocking store to a dumped signal (`service_after_write`, `:6474-6476`) and in
the NBA commit loop (`:6259-6264`, `:12497-12502`), behind a load of the
`rt_vcd_active` byte (`Runtime.h:578`). One predictable branch when off. That
is precisely the shape a watchpoint wants.

Three gaps, all consequences of blocker 7 and all resolved the same way — by a
debug-mode symbol table (§4.3) that is *not* the VCD index:

- VCD indices are assigned only if the design contains `$dumpvars`
  (`:14226-14229`). A watchpoint must not require the design to dump.
- `service_after_write` is emitted only when `signal_has_waiters(sig)`
  (`:6457`). Watchable signals need it unconditionally, which is a real cost
  and therefore debug-mode-only.
- The VCD touch reports only *that* a variable changed, deferred to the
  Postponed flush. A watchpoint must stop *at the store*, and DAP wants the old
  and new value in the `stopped` description, so the callout takes the slot
  address and the pre-write snapshot — which `service_snapshot`
  (`ir/llvm/Events.cpp`) already computes.

`read` access type is not offerable: reads are not instrumented and
instrumenting them would be a different order of cost. `dataBreakpointInfo`
returns `accessTypes: ["write"]` only. Per PRINCIPLES.md this must be *declined*,
not silently ignored — an armed `read` breakpoint returns
`verified: false` with a message.

Read-only aside, this is the mapping with the best value-to-cost ratio after
D1, because "stop when this net changes" is a thing every RTL engineer wants.

### 3.5 DAP stepping ← statements, delta cycles, time steps

`SteppingGranularity` is a closed three-value enum — `statement | line |
instruction` — and there is no extension point. Clotho has at least five
meaningful advance units: expression, statement, source line, delta cycle
(one Active→NBA→Active turn), and time step (one `rt_wheel_advance`). They do
not fit, and pretending otherwise is the silent acceptance PRINCIPLES.md forbids.

Three options were considered:

1. **Overload the enum.** `statement` → SV statement, `line` → source line,
   `instruction` → delta cycle. Rejected: it lies. A client that offers
   "step instruction" and disassembly gets neither, and `disassemble` would
   have to be declined right next to it.
2. **A stateful step mode** set by a custom request, reinterpreting `next`.
   Rejected: invisible in the UI, and the same button does different things at
   different times.
3. **Standard three for source-level stepping; the debug console for
   simulation-time advance.** Recommended.

Under (3):

| Verb | Meaning |
|---|---|
| `next` (`statement`, `line`) | Run to the next statement / line boundary **in the current process**, stepping over task and function calls. If the process suspends first, stop where it resumes. |
| `stepIn` | Enter the task or function called at the current statement; otherwise identical to `next`. |
| `stepOut` | Run to the return of the current task/function frame. Requires the shadow stack (§3.2), so D5. |
| `continue` | Free-run until a breakpoint, exception filter, watchpoint or end of simulation. |
| `pause` | Stop at the next stop point reached by any process. |

and the console (`evaluate`, `context: "repl"`), which every DAP client
exposes and which is where gdb-style extras conventionally live:

```
step delta            advance one delta cycle, stop at the top of the next Active region
step time             advance one time step (one rt_wheel_advance)
run 100ns             free-run until simulation time advances by 100 ns
run to 1us            free-run until absolute time 1 us
run settle            free-run to the end of the current instant (Postponed)
```

Each is answered with an `output` event and then a `stopped` event with
`reason: "step"` and a `description` naming what happened ("delta cycle 3 at
1500 ns"). The same commands ship later as custom requests
(`clotho/stepDelta`, `clotho/runUntil`) so a Clotho-aware extension can bind
keys to them, but the console works in every client on day one and needs no
extension at all.

**A "line" is not always one place.** Fusion and unrolling aside, a single
source line inside a generate loop exists once per elaborated instance. DAP
handles this: `setBreakpoints` returns one `Breakpoint` per requested
`SourceBreakpoint`, but a `stopped` event carries `hitBreakpointIds[]`, and
`breakpointLocations` reports what is actually stoppable in a range. Stepping
stays scoped to the current thread — i.e. one instance — which is the
behaviour that makes sense and the one that falls out of §3.1.

### 3.6 What has no DAP shape, and what has no Clotho shape

| Concept | Home |
|---|---|
| Waveform view | Not DAP. VCD already exists (`docs/VCD.md`); WCP to a viewer is an adjacent product, as `lsp.md:638-640` already notes |
| Coverage | Not DAP. `docs/TODO.md` |
| `disassemble`, `readMemory`, `writeMemory`, instruction breakpoints | Declined. There is no meaningful instruction stream to show a hardware engineer, and blocker 3 means we could not annotate it |
| `stepBack`, `reverseContinue` | The real differentiator (§7), out of scope for D0–D5 |
| `setVariable` / `setExpression` on a signal | Deliberately deferred. Depositing a value mid-instant is `force`-shaped and has LRM semantics that must be got right, not bolted onto a debugger |
| `goto` / `gotoTargets` | Meaningless: there is no program counter to move |
| `restartFrame` | Meaningless for the same reason |
| Loaded sources / modules | `loadedSources` maps cleanly onto the compiled file set; cheap, low value, unscheduled |

---

## 4. The four structural problems

Everything else is ordinary work. These four decide the architecture.

### 4.1 The simulation cannot be stepped from outside, so it stops itself

Blockers 1 and 2 together mean the usual debugger topology — a controller that
single-steps a target — is not available and cannot be made available without
restructuring `emit_active_run`. The inversion:

**A stop is the simulation parking itself inside a runtime callback while
something else services the protocol.**

D0 already puts the design in its own process, for reasons that had nothing to
do with stopping and everything to do with §4.4 — its stdout is not the
protocol's, and a design that aborts takes only itself down. That leaves the
shape of the stop handshake as the one genuinely open architectural question,
and it is D1's to settle (§8.10). The two candidates:

```
  in-process                              worker process (what D0 already has)

  reader ─▶ session thread ─▶ writer      reader ─▶ session thread ─▶ writer
                 │ ▲                                     │ ▲
          resume │ │ stop + state reads          resume  │ │  stop + state reads
                 ▼ │                                     ▼ │      (over a pipe)
            sim thread                              design process
       run_jit ▶ main() ▶ sim_drain           run_jit ▶ main() ▶ sim_drain
       parks in rt_dbg_stop() on a condvar    parks in rt_dbg_stop() on a read
```

Either way the handshake is stop-the-world: the simulation publishes its stop
reason, signals, and blocks. While it is blocked, design state is quiescent by
construction — which is the *only* thing that makes blocker 11's "zero
synchronization on design state" safe.

In-process makes a `variables` response a pointer read; the worker makes it a
round trip, but keeps the session alive across blocker 9's three `std::exit`
paths, which fire exactly when a design is most worth debugging. D0 does not
have to choose, because it reads no state at all.

There is an in-tree precedent for the in-process shape — the VCD writer thread
and its condvar handshake (`runtime/Vcd.cpp, vcd_writer_loop`) — and D0 itself is the
precedent for the other.

Consequences worth stating plainly:

- **Stop granularity is exactly the set of places the generated code calls the
  runtime.** D1 gets the four report sites for free. Everything finer requires
  emitting new callouts, and every emitted callout costs a branch.
- **`pause` is not instantaneous.** It sets a gate byte; the simulation stops
  at the next stop point. Under D1's stop set that could be a long time, or
  never. `pause` therefore arrives with D3, when statement stop points exist.
  Before that the capability is off — declined, not faked.
- **The gate is one byte, tested inline.** `rt_vcd_active` (`Runtime.h:578`)
  and `rt_finishing` (`:124`) are the existing precedents, both plain
  non-atomic bytes, both read by generated code. `rt_dbg_active` joins them.
  Cross-thread writes to it are the same benign race those two already are; the
  handshake that follows is properly synchronized.

### 4.2 The generated code is not the code the user wrote, unless we ask

Five transforms are on by default and each breaks a debugger assumption:
`fuse` (N processes → one coroutine), `comb_fuse` (dependent combinational
processes merged), `thunk` (suspension-free process → a frameless function
pointer), `dedup` (one body shared across twin instances), `outline`
(fused group split across definition units). Above them, `inline` runs even at
tier 0 (`ir/llvm/LlvmBackend.cpp, optimize_module`), and `sroa`/`instcombine`/`dce` destroy frame
locals.

So: **`clotho dap` compiles the design in a debug mode.** `SimConfig` gains
`bool debug = false`; setting it forces `fuse`, `comb_fuse`, `thunk`, `dedup`
and `outline` off, and enables the debug emissions each phase adds. This is not
a new idea being invented here — every one of those five fields already carries
the comment *"Disable for bisection / debugging; semantics are identical either
way"* (`SimConfig.h:119,125,132,140,147`).

Two rules make this honest:

1. **Debug mode changes performance and observability, never semantics.** The
   knobs are already documented as semantics-preserving and already exist for
   differential testing, which is the proof harness: the D-phase acceptance
   test is that the existing suite passes identically with `debug` on.
2. **The user is told.** The adapter emits an `output` event at launch naming
   the mode, and `clotho run --debug` is a real CLI flag so the same build is
   reproducible outside the debugger. This is the ordinary `-O0` vs `-O2`
   bargain and it should read like one.

One thing debug mode does *not* buy: machine codegen is already
`CodeGenOptLevel::None` unless `o3` (`Jit.cpp:123-124`), so line fidelity at
the machine level is better than a typical JIT to begin with.

### 4.3 There is no runtime symbol table, and VCD's is the wrong one

Blocker 6. `variables`, `dataBreakpointInfo` and any name-based `evaluate` all
need name → storage at runtime, and the only existing registration is VCD's,
which is `$dumpvars`-gated, leaf-name-only (`src/ir/llvm/`) and
throws the name away after writing the header (`runtime/Vcd.cpp, vcd_register`).

The answer is a **static debug symbol table emitted as constant data**, not a
runtime-built one: in debug mode, `clotho.dbg.scopes` and `clotho.dbg.vars`,
walked from the `InstanceScope` tree the backend already walks for VCD, holding
per variable `{hierarchical name, decl SourceLoc, width, flags (four-state,
signed, narrow, array…), plane pointers}`. Plane pointers into the state buffer
are stable for the run — `Runtime.h:527-531` says so about the VCD ones for
exactly this reason. One `rt_dbg_register(...)` call at startup, and the
existing `SignalSlot` (`ir/llvm/Backend.h`) already holds every field.

Two dependent pieces:

- **A plane→SystemVerilog-literal formatter.** Values are plane pairs
  `{ i(2W) must|may, iW z }` (`ir/llvm/Backend.h`) with `z_elision` and
  `narrow` variants. `rt_container_format_p` (`Runtime.h:882`) formats
  containers only; there is no scalar formatter. `vcd_read_planes`
  (`runtime/Vcd.cpp`) is the existing plane-assembly code to build on. DAP
  `variables` also wants a `type` string, which the interned IR type gives.
- **Frame locals stay out.** Blocker 8 plus `sroa` means a task's automatics
  have neither a name nor a stable address. Both are fixable — retain the
  symbol name in `fn_local` (`lower/Lowerer.cpp`), keep frame slots in memory
  under debug mode — and both are D5.

This table subsumes VCD's registration, and unifying them later is a cleanup,
not a prerequisite.

### 4.4 A session is a process, and it only runs once

`lsp.md` §4.3 concluded that the cancellable unit is an OS process. The same
argument lands harder here: blocker 9's three `std::exit` paths, and
`tests/Subprocess.h:7-19`'s recorded hazard that a design ending in `$finish`
must not run in a process that continues.

So, v1: **`clotho dap` serves exactly one session and one run, then exits.**
That is the default DAP topology anyway — a client spawns a fresh adapter per
debug session. `restart` is declined with an error response and the capability
is off; the client respawns. `disconnect` and `terminate` end the process.

That the design *itself* is already a child process is a D0 decision, and it
cost nothing: `clotho run` spawned through the adapter's own path
(`self_exe`, exactly the `argv[0]` discovery `lsp.md` §11.5 established) needs
no argument parsing in the adapter at all, keeps `$display` off descriptor 1
without a single dup, and gives `terminate` and `disconnect` something real to
do. What is *not* yet built is the control channel that would make that child
a debuggable worker; §4.1 and §8.10 are where that is decided.

**Where the design comes from.** `clotho dap` takes no arguments, following
`clotho lsp`'s precedent (`main.cpp:411-431`) and for the same stated reason —
accepting flags and ignoring them is silent acceptance. The `launch` request
carries the configuration, and to avoid a second vocabulary for the same
things (cli.md: *"Never a second parser, never a second help"*), its primary
field is the `clotho run` argument tail:

```json
{
  "type": "clotho", "request": "launch", "name": "Debug testbench",
  "args": ["-y", "rtl", "--top", "tb", "tb/tb.sv", "+seed=3"],
  "cwd": "${workspaceFolder}",
  "stopOnEntry": true
}
```

`args` reaches the design process as the tail of its own command line, so the
existing `register_options` + `slang::driver::Driver` path is the only thing
that ever parses it — the adapter itself does not look inside. `cwd` and
`stopOnEntry` are the two DAP-shaped additions. `attach` is declined: there is
no running simulation to attach to.

---

## 5. Day one

D0 is the scaffolding, and the point of drawing the line here is that it ships
something real while touching neither the runtime nor the backend.

**What D0 is,** as landed. `clotho dap` as a command
(`src/main.cpp`, sharing the no-arguments arm with `lsp`);
`src/lsp/Transport.{h,cpp}` moved to `src/transport/` in namespace
`clotho::transport` as its own target with no dependency beyond libc, linked
by `clotho_lsp` and `clotho_dap` alike; the DAP envelope and `seq` discipline
in glaze, reflected off plain aggregates exactly as the language server's are
(`src/dap/Server.cpp`); the sorted dispatch table with its compile-time
sortedness check; the lifecycle (`initialize` → `initialized` event →
configuration → `configurationDone` → `launch` →
`process` → `output`… → `exited` → `terminated`); the reader/session/writer
thread topology plus a fourth thread parked on the design's pipes; the design
itself as a child process (`src/dap/Design.cpp`) with its stdout and stderr
arriving as `output` events; `disconnect` and `terminate`, both of which
really do end the design; and a refusal, by name and with its stage, for every
command not yet implemented.

**What D0 does for a user.** Press F5 in the editor, watch the design compile
and run, see its output in the debug console, see the exit code, and stop it.
That is `clotho run` without leaving the editor, and it is the whole
integration surface every later phase plugs into.

**What D0 deliberately does not do.** No stopping, at all. Every stop-shaped
capability is reported `false` and every stop-shaped request returns
`success: false` with a message saying which phase implements it. There is no
partially-working breakpoint.

Cost, measured: it is the language server's topology with a different envelope,
no document store, no debounce and no analysis worker, plus a child process and
the thread that reads it. `src/dap/` is 3 files; `clotho_transport` is the
language server's own framing layer, unchanged.

---

## 6. Sequencing

`D<n>` labels are local to this document; `L<n>` refer to `lsp.md` §9. Every
stage ships standalone value.

| # | Stage | Needs | Cost | Standalone value |
|---|---|---|---|---|
| **D0** ✅ | Transport extraction, `clotho dap`, envelope + lifecycle, launch, `output` events, exit reporting | — | small | run a design from the editor, output in the console |
| **D1** | Stop on assertion / severity / `$stop`; `setExceptionBreakpoints` + `exceptionInfo`; site records on the four report ABIs; park-and-resume handshake (§4.1); one synthetic thread, one frame from the site | D0 | small | **stop where the assertion failed** — the highest-value feature in the document, and no backend change |
| **D2** | `SimConfig::debug` (§4.2); process table + `threads`; instance-hierarchy `stackTrace`; debug symbol table + scalar formatter (§4.3); `scopes`/`variables` over signals | D1 | medium | inspect the whole design state at the moment it broke |
| **D3** | Statement stop points (a sparse statement index on `Ir`, §8.1); `setBreakpoints`, `breakpointLocations`; `next`/`stepIn`; `pause`; console `step delta` / `step time` / `run to` | D2 | medium | a real source-level debugger |
| **D4** | Unconditional change detection for watchable signals; `dataBreakpointInfo`/`setDataBreakpoints`; old/new value in the stop description | D2 | medium | **stop when this net changes** — the second differentiated feature |
| **D5** | Shadow call stack; local names retained in `Function`; task/function frames; `stepOut`; `evaluate` over hierarchical names; LSP `textDocument/inlineValue` (`lsp.md:916`) | D3 | large | task/function debugging and inline values in the editor |
| — | A control channel to the design process, `restart`, crash isolation (§4.1, §4.4) | D1 | medium | survive a design that aborts; not a stage |
| — | Snapshot + reverse execution (§7) | D2 | large | reverse execution of a running design; not scheduled |

Ordering claims worth arguing over:

- **D1 before threads.** A stack of one frame with the right file and line beats
  a thread list with nothing to inspect, and it costs an order of magnitude
  less. Assertion debugging is also the single most common RTL debugging task.
- **D2 before D3.** Breakpoints without variables are a worse product than
  variables without breakpoints, because at an assertion stop the question is
  always "what were the values", never "let me step".
- **D4 does not depend on D3.** A watchpoint stop is a stop at a store, not at
  a statement, so it needs D2's symbol table and nothing from stepping. If
  stepping proves expensive, D4 can overtake it.
- **`pause` is D3, not D0.** Without statement stop points it can only stop at
  a report site, which for a healthy design is never. Shipping a Pause button
  that usually does nothing is worse than not having one.

---

## 7. What "good" would have to mean, measurably

1. **Debug mode costs less than 3× simulation time** against the same design
   built normally, measured on `hazard3_core` and `picorv32`. Anything worse
   and users will debug with `$display` instead, which is the actual competitor.
2. **Debug mode is bit-identical in behaviour.** The full test suite passes
   with `SimConfig::debug` forced on. Non-negotiable; §4.2 rule 1.
3. **Stop-to-`variables` under 100 ms** on a design with 100k signals, because
   an inspector that pauses the UI is one nobody opens twice.
4. **Every declined request declines loudly.** Zero requests that return
   `success: true` with an empty or invented body. PRINCIPLES.md's never-silently-
   accept rule applied to a protocol.
5. **The differentiator, when it comes, is reverse execution.** A deterministic
   event-driven simulator with a bounded state vector can checkpoint and replay;
   `supportsStepBack` and `reverseContinue` are already in the protocol and
   already in every client's UI. It is not
   scheduled because it needs D2's state model first, but it is the reason to
   build the state model well.

---

## 8. Open decisions

1. **~~Where the shared transport lives~~ — RESOLVED: `src/transport/`,
   namespace `clotho::transport`, its own target.** No dependency beyond libc,
   linked by `clotho_lsp` and `clotho_dap` alike, so neither links through the
   other and the LLVM-free invariant on the language server survives contact
   with a debugger that must link the JIT. Not one line of the framing code
   changed. The alternative — `clotho_dap PRIVATE clotho_lsp` — extracts only
   `Transport.cpp.o` in practice but drags `clotho_lsp`'s `PUBLIC clotho` edge
   along and reads backwards.
2. **Statement boundaries: a new `Inst::Tag`, or a sparse index?** Recommended:
   a sparse sorted statement-start index on `Ir`, alongside the existing sparse
   `ends_` map (`Ir.h:2430`), populated by `Lower` only when asked. A new tag
   would touch every switch over `Inst::Tag` (`Ir.h:916`) for a fact that is
   pure metadata. D3's to settle.
3. **`glz::meta` enters the codebase at D1.** `ExceptionBreakpointsFilter`'s
   `default` field is a C++ keyword, so pure reflection cannot express it and a
   `glz::meta` / `glz::object` specialization is required — the first in the
   repo. D0 needed no such struct and has none; confine it to that one type
   when it arrives.
4. **~~`error_on_missing_keys`~~ — RESOLVED: leave it `false`, validate what
   matters by hand.** The glaze default, and what the language server already
   relies on. The two fields whose absence a zero value cannot reveal are
   `type` and `command`, and the dispatcher checks both: a message that is not
   `type: "request"` is logged and dropped, and an empty `command` never
   reaches a handler (`src/dap/Server.cpp`).
5. **~~`$stop` under a debug session~~ — APPROVED, unbuilt.** Today `$stop` is
   treated identically to `$finish` (`Runtime.h:98-101`) — defensible for a
   batch simulator and wrong for an interactive one, where `$stop` means
   exactly "break here". With a debug session attached and the `stop` filter
   on, `$stop` parks; detached behaviour is unchanged. Lands with D1's filter
   set, and is the one semantic change in this document.
6. **~~Simulation-time stepping~~ — RESOLVED: console commands, not a
   granularity.** §3.5 option 3. The alternative is lying about `instruction`,
   and a client that offers "step instruction" also offers `disassemble`.
7. **~~`--debug` on the CLI~~ — APPROVED, unbuilt.** `SimConfig::debug` forces
   `fuse`/`comb_fuse`/`thunk`/`dedup`/`outline` off, `clotho dap` always uses
   it, and `clotho run --debug` exposes the same build so a debug-mode failure
   is reproducible and bisectable from a shell. A `run`/`build`-scoped option,
   so it needs a `check_scope` row (`main.cpp:265-276`). Lands with D2, which
   is the first stage that needs process↔coroutine to be 1:1.
8. **~~Testing~~ — RESOLVED: deadline-driven live session.**
   `tests/dap_test.cpp` drives the real `clotho dap` over fork/exec pipes,
   after `lsp_test.cpp`'s `LiveSession`, with two changes the protocol forces:
   the message type is DAP's, and waiting is bounded by a deadline rather than
   by a count of interleaved messages — the language server's `response(id)`
   gives up after 8 (`lsp_test.cpp:289`), and one chatty testbench emits more
   `output` events than that before its first response. Events seen while
   waiting are kept, so a test can assert on them afterwards. Windows has no
   coverage: the harness is POSIX-only, as the language server's is.
9. **~~`docs/design/cli.md` did not document `lsp`~~ — FIXED.** Its
   `commands:` block listed only build/run/lint/synth; it now lists both
   servers and says why neither takes arguments.
10. **How the design is stopped, and therefore where it lives — OPEN, D1's to
    settle.** §4.1. D0 runs the design as a child process because that was the
    cheapest correct thing for a stage that reads no state: no argument parsing
    in the adapter, no descriptor games to keep `$display` off the protocol
    stream, and a design that aborts cannot take the session with it. D1 must
    decide whether the stop handshake keeps that boundary — a control channel
    over a pipe, paying a round trip per state read but surviving blocker 9's
    three `std::exit` paths — or moves the simulation into the adapter as a
    thread parked on a condvar, where `variables` is a pointer read and any
    abort ends the session. The argument for keeping the boundary is that a
    design under debug is broken by definition; the argument against is that
    every later stage pays for it.
