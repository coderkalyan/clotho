# TODO — Road to full IEEE 1800-2023

This is a TODO **spec**, not a changelog or design doc. Treat it as such: when a
line item is partially implemented, split it so the unimplemented remainder
stays tracked. Clotho must never silently accept a construct it does not
correctly lower; every unimplemented gap below is guarded by a loud
frontend `fail`, or must grow one when its surrounding feature lands.

Items are organized by the five delivery milestones, then grouped by logical
similarity. Within each group, unimplemented items come first for visibility,
then completed items as one-liners. Lower milestones unblock higher ones.

- **M1 — Verilog-2005 RTL.** Run typical synthesizable V2005 (picorv32, small
  custom designs) end to end.
- **M2 — Icarus Verilog parity.** Everything iverilog handles: full V2005 plus
  the SV subset and tooling (VCD, tasks, gates, UDPs, real, file I/O).
- **M3 — Common SystemVerilog.** The SV that icarus/verilator/yosys accept:
  user types, packages, interfaces, dynamic data, basic classes, immediate
  assertions.
- **M4 — Industrial SV + UVM.** Run a real UVM testbench (suggest OpenTitan's
  Ibex UVM env, or the smaller CV32E40P / a minimal UVM agent as a stepping
  stone): full OOP, randomization, concurrent assertions, coverage, DPI.
- **M5 — Full LRM.** No exceptions or compromises: complete system
  tasks/functions, VPI/PLI, specify/timing, nettypes, config, checkers.

M1–M3 make Clotho usable in the open-source community; M4 brings it to
professional parity; M5 makes it complete.

---

## M1 — Verilog-2005 RTL

### Operators & expressions

- [x] **Division / modulo / power** (`/`, `%`, `**`) — all widths, signed+unsigned
  (`**` signed base/exponent corners per Table 11-4).
- [x] **Signed arithmetic** — sign extension, signed compare/divide/modulo/shift,
  mixed-sign rule (§11.8.1), `$signed`/`$unsigned`, shift-amount >= width.
- [x] **Multi-bit `if` / `?:` / loop condition** — reduced to 1-bit truth (§12.4, §12.7).
- [x] **Lazy `?:` arm evaluation** (LRM 11.4.11).

### Subroutines

- [ ] **Output / inout string formals** (function and task) — the copy-out path
  passes plane pointers, not `rt_string` handles.
- [x] **DPI-C import function** (LRM 35) — `import "DPI-C" [pure] function` over
  the scalar-mapped types (byte/shortint/int/longint/real/shortreal/bit/logic;
  by-value, direct native `call`, ~1.8ns/call), packed vectors (bit[N]→
  svBitVecVal, logic[N]→svLogicVecVal via rt_dpi_pack/unpack_logicvec), and
  `chandle` (opaque i64↔void*), and `string` (input arg via rt_string_data
  zero-copy — needs the rt_string NUL invariant — and return via rt_dpi_str_out);
  `input`/`output`/`inout` args, scalar/chandle/string return, `pure`+`context`.
  **export "DPI-C"**: C calls into SV — a C-ABI thunk marshals args and calls the
  lowered SV function, which addresses its own instance's state absolutely, so an
  export binds at lowering time and needs no scope lookup; functions,
  `input`/`output`/`inout` scalar/chandle/real/vec args, `input` string,
  scalar/chandle/real/void return, any number of tops. Loader: `--dpi-lib` takes a
  `.o`/`.a` (JIT-linked via addObjectFile — both directions) or a `.so` (dlopen,
  import only; refused when the design exports). Export direction is checked
  against verilator (not a 4-state oracle — it drops x/z on logic vecs). See
  docs/design/dpi-c.md. **svdpi.h runtime** (Annex H): packed-vector
  bit/part-select accessors (svGet/PutBitsel/Partsel Bit/Logic, svDpiVersion) +
  per-top scope table (svGetScope/svSetScope/svGetNameFromScope/
  svGetScopeFromName/svPut+GetUserData), and 1-D open-array accessors
  (svSize/svLeft/svRight/svLow/svHigh/svIncrement/svGetArrElemPtr1/
  svGet+PutBit|LogicArrElem1VecVal) in src/runtime/Svdpi.cpp, header
  src/runtime/svdpi.h, exposed as JIT absolute symbols. **1-D open arrays**
  (LRM 35.5.6.2, all directions, any element width): named-scalar element zero-copy
  (narrow [N×iW] storage is the C array; output/inout write it in place), packed
  bit/logic element marshal via inline LLVM IR at any width (emit_dpi_oa_pack/
  unpack_ir — constant-trip loop over the native iW/i2W design slot, N is a compile-
  time constant; LLVM legalizes must=trunc / may=lshr-i2W-W / reconstruct=shl-i2W-W,
  no hand funnel shift, z field gated on the slot's real has_z); output/inout unpack
  back after the call; svOpenArrayHandle = rt_svopen descriptor built per call. No C
  marshal — one mechanism at all widths (an earlier ≤64-bit autovectorized C fast
  path was dropped: thin edge only at -O0, standing two-impl hazard). Actuals: fixed
  unpacked array + dynamic array (LRM 7.5; named-scalar element zero-copy off the
  runtime {data,len} handle — verilator rejects dynarray open-array actuals; an
  empty array is handled robustly rather than fatally). Remaining DPI gaps are
  tracked as `[ ]` items under M4 "Foreign interface".
- [ ] **Class-method call in a function**.
- [ ] **Nonblocking assignment in a function**.
- [x] **Procedural-block variable lifetime** (LRM 6.21) — a declaration in a
  procedural block defaults to STATIC lifetime (one instance, initializer evaluated
  once before time zero, LRM 10.5), and an `automatic` one has the lifetime of the
  BLOCK: it is initialized on every entry, with its initializer if it has one, else
  with the type's default (LRM 6.8, Table 6-7). Neither reset happened before, in a
  process or inside a subroutine, so both lifetimes behaved identically and neither
  matched the LRM: an automatic declaration with no initializer kept the previous
  entry's value (a container local in a loop body accumulated across iterations),
  and a static one re-ran its initializer on every entry. `is_static_local` no
  longer requires a subroutine (the LIFETIME decides when the initializer runs, and
  a process local's storage is likewise persistent), and `emit_default_init` writes
  the Table 6-7 default at the declaration's own position: x / 0 per integral leaf,
  0.0 real, `""` string, `null` handle, an emptied container, per member / element
  for an aggregate. LRM 6.21's worked example now prints its documented
  `1 2 3 1 2 3 1 2 3` / `1 2 3 4 5 6 7 8 9`. A block re-entered WITHIN one call is
  covered too: the frame is per call, so `init_frame` alone was not enough.
  lower_var_decl still clears fn_ctx_ around the PreInit lowering, so a static
  initializer that reaches for a frame local fails loud rather than mis-resolving.
  An `event` local is skipped: its power-on value is its own handle, baked into the
  state initializer, and re-running a default init would unbind it. An
  automatic-lifetime event local is refused instead (Table 6-7's "new event" per
  activation would need a fresh object).
  * Perf follow-up: the aggregate default is unrolled per leaf, and tier-0 does not
    fold it (measured: 279 stores for `automatic byte mem[256]`). A bulk reset needs
    an op that memcpys the type's proto image, valid only for a trivially-copyable
    aggregate — a managed leaf has to keep the per-leaf path, which frees the
    previous entry's string / container instead of leaking it.
- [ ] **`$time` in a function**.
- [ ] **NBA to a task frame local / output-inout formal**.
- [x] **Functions** — combinational callables (not inlined); `automatic`/`static`
  locals, recursion, early `return`, callee-descending sensitivity, NBA to a
  module signal / `static` local / array element (LRM 13.4.4 — same shadow-staging
  path as a task; frame-local NBA guarded).
- [x] **ref / const ref args** (13.5.2) — a pointer to the actual's storage,
  uniform for every storage class, so the formal may be any type at all (an
  `event` formal aliases its 64-bit handle word like a class handle) and
  the actual any whole place: a container element, an assoc element, a struct
  member, a class property, nested to any depth. A ref into a container's heap
  storage additionally carries that container's relocation epoch and is re-checked
  on every dereference, so a push or resize under a live ref is a located fatal
  rather than a dangling write (`--loose-stale-ref` downgrades it). Every other
  shape aliases storage that cannot move, and narrows to one plain pointer with no
  check — decided per formal from its actuals, unioned across each vtable slot.
  Nothing for the report rides on the call: the formal's name is the callee's, and
  the container's comes from the epoch pointer. See docs/design/data-model.md §3.
- [x] **Function output / inout args** (13.5) — pointer copy-in/copy-out ABI,
  coexists with the return value.
- [x] **Tasks** (13.3) — time-consuming coroutines with per-call frames;
  input/output/inout args, `automatic`/`static` locals, `#delay`/`@event`/`$time`/
  recursion, NBA to module signal / `static` local / array element.

### Memories & arrays

- [x] **Nonblocking whole-array copy** of an unpacked array (per-element
  deferred; single-memcpy record is a perf follow-up).
- [x] **Nonblocking `'{...}` pattern** to an unpacked array.
- [x] **Whole unpacked-array `==` / `!=`** (per-leaf AggEq, X-propagating).
- [x] **Fixed unpacked-array slice** `busB = busA[7:6]` (LRM 7.4.5): copy /
  equality / lvalue / NBA target, either direction. Flat-storage elements
  only; an aggregate-element slice and a whole by-value slice read (concat /
  `%p`) stay guarded.
- [x] **Indexed part-select slice** `a[i +: n]` / `a[i -: n]` of an unpacked array
  (LRM 7.4.5, constant width + runtime base): copy source / target, NBA either
  side, equality operand, and `input` / `output` / `inout` subroutine argument,
  over both endiannesses, a struct's array member, and a multidimensional array's
  outer dimension. Expanded at lower time into one ordinary element access per
  position (`emit_indexed_slice_copy`), which is what gives it LRM 7.4.5's
  per-position invalid-index rule for free: a position past the bound reads the
  element default and drops its write, and an x/z base invalidates every
  position. The base is evaluated once. A read is
  staged in a canonical temp, so the remaining gaps are the ones the constant-
  bounds form has: aggregate / string elements, and a general by-value read.
- [ ] **Unpacked arrays of `string`**.
- [x] **Unpacked array as a function / task local, by-value argument, or return**
  (LRM 13.4/13.5): canonical aggregate frame storage; value semantics via a
  canonical-temp copy-in (the flat-vs-canonical z-elision layout hazard).
  `input` / `output` / `inout` all work for both functions and tasks; a task's
  copy-out rides its completion epilogue, so it lands when the task returns.
  String / handle-element arrays and `ref` aggregate formals stay guarded.
- [ ] **Sliced memory target** for `$readmem*` / `$fread`.
- [x] **Unpacked arrays / memories** — element-addressed; decl, read,
  blocking/nonblocking write, nested bit/part-select, multidimensional,
  descending ranges, X power-on, async-read sensitivity.
- [x] **`$readmemh` / `$readmemb`** (§21.4).
- [x] **`$readmem*` / `$fread` element width > 64 bits**.

### Elaboration & structure

- [x] **Module variable declaration initializer** (LRM 6.8) — applied via a
  run-once `PreInit` process before any Initial/Always.
- [x] **Runtime-indexed constant table** (LRM 6.20 + 11.5.1) — a parameter /
  localparam with an aggregate value read at a non-constant index (`localparam
  int P[0:3] = '{...}; x = P[i]`), the constant lookup-table idiom. A parameter
  has no storage and a runtime index defeats the fold, so the value is
  materialized as an unscoped read-only signal, in one of two ways. A value with
  a plane image (integral / real / enum / packed struct / handle leaves, fixed
  unpacked arrays of them, and unpacked structs) is interned whole
  (`SignalInfo::const_data`) and baked into the state initializer: no elaboration
  loop, no time-zero fill, every read an ordinary indexed load. A value holding a
  heap-backed leaf (string, queue, dynamic / associative array) has no static
  form, so it is seeded once in the `PreInit` process instead, from the
  elaborated value rather than the initializer expression (a defparam / config
  override has no initializer). Covers module / package / class parameters,
  per-instance overrides, multidimensional and 4-state tables (x/z round-trip),
  whole-array copy, by-value actuals and `foreach`; scalar `string` and `null`
  parameters fold too. Refused loudly: a wildcard-index associative array, and a
  queue / associative array of aggregate elements. See
  docs/design/const-tables.md.
- [x] **Net declaration assignment** (`wire w = a & b;`, LRM 6.7.1).
- [x] **`generate`** — `for`/`if`/`case` and nesting.
- [x] **`defparam`** — parameter override by hierarchical path; cross-top paths
  unsupported.
- [x] **Instance arrays** (`foo u[N:M] (...)`, LRM 23.3.6) — multidimensional,
  bit-sliced port connections.
- [x] **Multiple top-level modules** — each flattened into the one design,
  shared timeline; cross-top hierarchical refs unsupported (see M2 Hierarchy reach).

### Procedural control

- [x] **`disable`** of a named block and of a task (LRM 9.6.2) — all three forms
  lower and simulate. LEXICAL in a process body: the block becomes an
  `ir::NamedBlock` region and `ir::Disable` branches to its end, a structured
  multi-level exit (SV has no labelled `break`), with no runtime at all. REMOTE
  (`ir::DisableRemote`): a sibling block, a block in another process, a block in a
  task or fork child, or a TASK — every activation the target names is marked and
  force-woken, and each aborts itself at its own poll point. LEXICAL in a body with
  several activations: both, since 9.6.2 ends them all. LRM 16.4.4's outermost-scope
  flush lands too, lexically and remotely, as an `ir::DeferFlush` generation bump.

  A disabled task's caller resumes after the task-enabling statement, which is what
  the existing epilogue already does, so an aborted activation takes that epilogue
  with copy-out skipped (9.6.2 leaves output/inout results unspecified). The
  machinery is one activation list per target: a frame-resident doubly-linked node,
  linked on entry and unlinked on exit, plus a claim on the `#delay` park record so
  the wheel and a `disable` cannot both wake the same park. No epoch, no process
  table, no `Wheel.h` change, no runtime change.

  Matches iverilog on 9.6.2 examples 1, 2, 3 (both readings), 4 and 5, plus: a target
  parked on a delay whose LATER delays must still land on time, an `always` body that
  re-arms, two instances disabled independently, two activations of one task ended by
  one statement, `#0` inside a target, a fork child from either side, and an
  assertion action block as the disabler. A design that uses only the lexical form
  pays nothing — picorv32's emitted LLVM is byte-identical.

  Still refused: a target whose body can park where the abort cannot claim the park
  (a TIME-CONSUMING task call, a fork, an `expect`). 9.6.2 disables downward along
  the enable chain, so the nested-call case needs the callee's activation ended too,
  which is a second list walk.
- [x] **`repeat (N)` loop** — negative or X/Z count runs zero times. The hidden
  counter and bound are per-*activation* cells (`make_temp_cell`): a frame Local
  inside anything with a frame (task, function, fork child, action block), a
  module Signal only in a process, which has none and is single-activation, so
  process IR is unchanged. Module-signal storage made every activation share one
  cell, so a recursive `repeat (3)` two levels deep ran 5 body iterations instead
  of 39 and two forked calls to one task ended on whichever count expired first.
  `foreach`'s snapshotted bound and its associative-array step flag share the
  mechanism, as does `lhs = repeat (n) @e rhs` (LRM 9.4.5).
  A **real count** is legal (12.7.2 wants only a numeric expression) and rounds
  half away from zero into a signed 64-bit count (LRM 6.12.1, `coerce`'s `FtoI`),
  so `repeat (2.5)` runs 3 times, `repeat (2.4)` 2 and `repeat (-0.6)` none; 64
  bits because that is where `FtoI` saturates instead of wrapping.
- [x] **`final` block** (LRM 9.2.3) — source order, zero-time.
- [x] **Looping `always` with timing control** (`always #5 clk = ~clk;`).

### System tasks

- [x] **`$random`** — unseeded V2005 signed-32 PRNG.
- [x] **Seeded `$random(seed)`** (LRM §20.15.1) — reads the inout seed lvalue,
  advances the reference stream, writes the updated seed back.
- [x] **`%0d` / width / left-justify format flags** (§21.2.1.2).
- [x] **`$display` / `$write` operands wider than 64 bits** — all radices multi-word.
- [x] **`%m`** — hierarchical scope name.
- [x] **`%u` / `%z`** — unformatted 2-/4-value output.

---

## M2 — Icarus Verilog parity

### Time-consuming procedural code

- [ ] **Intra-assignment timing remainder** (LRM 9.4.5) — a multi-signal trigger
  `@(a or b)`, an `iff` qualifier on the trigger, and the target shapes that still
  have no lvalue encoding to defer: a concatenation, a whole unpacked struct or
  array, a string, and a queue / dynamic-array / associative-array element (none of
  which plain NBA lowers either). (Blocking, nonblocking `#d`, and nonblocking
  `@e`/`repeat(N) @e` all done below, now including bit/part-select, unpacked-array
  element, packed/unpacked struct field, and real targets.)
- [x] **`iff` event-control qualifier** (`@(posedge clk iff en)`, LRM 9.4.2.3).
  Single-event only, lowered as a body/`@`-loop guard (`if (cond)`). Per-event
  `iff` in a multi-event list (`@(posedge a iff x or ...)`) still rejected loud —
  needs waking-source identification the shared-wake engine lacks.
- [x] **Event handles and every event storage class** (LRM 15.5) — `event` is a
  reference type: a 2-state 64-bit handle `{ u32 slot, u32 gen }` naming an event
  OBJECT, laid out and passed exactly like a class handle. `a = b` merges onto one
  sync object, `a = null` unbinds (`-> null` is a no-op, `null.triggered` is 0,
  `@(null)` blocks -- 15.5.5.2 leaves it undefined), `==`/`!=`/`===`/`!==` are
  identity and `if (e)` is the Boolean test (15.5.5.3).

  LRM Table 6-7 makes an event's default initial value a **New event**, so every
  event-typed storage location binds its own object when that storage comes into
  existence: a declared variable, an **unpacked-array element** (any dimension), an
  **unpacked-struct member**, a **class property** (per instance and `static`), an
  **automatic-lifetime local** (a fresh object per activation), and a **queue /
  dynamic-array / associative-array element**. A scalar declared event takes a
  compile-time slot in a static block; everything else draws from a chunked runtime
  pool whose chunks never move, so an object address survives growth while a
  process is parked on its waiter list.

  Waiters are an intrusive doubly-linked list on the object -- persistent nodes in a
  contiguous array, one-shot nodes frame-resident -- so the number of simultaneous
  waiters is unbounded. `@(<any event expression>)` works: a `WaitEdge` carries an
  optional handle operand, and a sensitivity list that would need one is left in the
  body, where it re-reads the handle each pass (a static subscription is issued from
  the seed loop and cannot re-resolve). Events pass as **subroutine arguments** in
  every direction and across **module ports** (aliasing, not copying, in both
  directions -- `inout event` is illegal SV).

  `->> e` defers to the NBA region through a runtime pending chain; `->> #d e`
  schedules one detached wheel timer carrying the handle; **`->> @(x) e` /
  `->> repeat(n) @(x) e`** register a detached one-shot subscription on the edge,
  growable because a loop makes one per pass. **`wait_order`** (15.5.4) desugars to
  one park per stage, each waiting on the events still ahead of it, with the
  else-arm on an out-of-order trigger and a run-time error when it is absent.

  Event operations work **inside a function**: the timing wheel is a module global,
  so a body with no wheel parameter still reads the clock. A monomorphism analysis
  keeps an event that is never assigned folded to a constant object address, so a
  design that does not merge pays nothing.

  Objects are never freed: LRM 15.5.5.2's reclamation is garbage collection, which
  Clotho does not have (class bodies leak by the same direction).
  Design: docs/design/event-handles.md, docs/design/named-events.md.
- [x] **`join_any` / `join_none`** (LRM 9.3.2, V2) — the join kinds differ at run
  time in one join-record field (`wake_at`: 0 for `join`, W-1 for `join_any`, never
  reached for `join_none`) and in when a child starts. `join` keeps resuming children
  inline at the fork; the detaching kinds must not run a child the parent is not
  waiting for, so `join_none` queues every child and `join_any` runs them only until
  one completes. Their record is heap-allocated per fork SITE and reference-counted,
  because the parent no longer outlives its children; `join`'s stays a frame slot and
  its codegen is byte-identical. Unbounded live children saturate the activation
  bound, which sizes the rings, the `#0` batches and the wait buckets alike.
  A capture of an enclosing task frame is refused for the detaching kinds (the ref
  ABI needs the frame to outlive the child) — lifts with V4.
  Design: docs/design/fork-join.md §"V2 scope".
- [x] **`fork … join_none` inside a function** (LRM 13.4.4) — the only kind the LRM
  allows there ("fork-join_none constructs shall be allowed inside a function", with
  children holding anything a task may); `join` / `join_any` in a function are
  illegal SV and slang rejects them itself, so `lower_fork`'s non-task check is now
  kind-specific. The spawn path needed almost nothing a function lacks — `state`,
  `rng` and (with process control) the caller's `proc` block are parameters, the
  wheel is the `clotho.wheel` global, the record is heap and refcounted, the region
  is a compile-time bit, and a detaching kind carries no captures. One line did need
  gating: V3 published the heap record into the `coro_begin_`-anchored
  `fork_live_slot`, which `join_none` never needs (it cannot park, so no abort can
  intervene) — now `!none`, which also removes two stores from every detached fork
  in a process-control design. The real work was 13.4.4's second paragraph: the
  calling thread must belong to an `initial` / `always` procedure, so
  `check_spawn_roots` (a spawn-reachability fixpoint over the call graph) plus
  `Process::spawn_root` report a static-variable initializer, a continuous
  assignment, a `final` block, a Preponed / Observed / Re-NBA process, a `$strobe` /
  `$monitor` body and a DPI export, each with the fork site and a "called from here"
  note. All five accepted the spawn silently before, and the `final` one leaked the
  record and the child frame. `reject_resume_once` now closes over calls as the
  backend backstop. Children spawned in a function belong to the CALLING thread, so
  `wait fork` / `disable fork` in the caller reach them (LRM 9.6.3). A child still
  cannot read the function's own formals or locals — exactly what the LRM's own
  `watch_for_zero` example does — which lifts with V4's per-spawn copies, and
  `$time` in a function stays refused for a timescale reason (LRM 20.3.1), not an
  ABI one. Design: docs/design/fork-join.md §"V5 scope".
- [ ] **`fork`-child capture of a local the ref ABI cannot carry** — an unpacked
  array / struct / string / real / queue / class-handle local, or a class property
  reached through the enclosing method's implicit `this`. Each is refused by name;
  they lift when `ref` arguments cover those types (docs/design/ref-args.md). Under
  `join_any` / `join_none` NO capture is carried, whatever its type: the ABI hands
  out a pointer into the enclosing frame and only `join` keeps that frame alive.
- [x] **`wait fork` / `disable fork`** — landed, but not here: the live-descendant SET
  V2's count-and-`wake_at` record could not carry is the `process` control-block tree
  (LRM 9.7), so both statements came with it. `disable fork` is `kill` over the
  caller's subtree; `wait fork` is `first_child == null`, exact because a completing
  thread re-parents its live children. Design: docs/design/process-class.md §8.
- [ ] **Per-spawn automatic-variable lifetime** — what the classic
  `for (int i…) fork … i … join_none` needs (fork-join.md V4). Refused by name rather
  than reading a shared final `i`. Note the capture refusal for the detaching kinds is
  now load-bearing for a second reason: a killed parent's frame goes away under its
  live children, so V4's per-spawn copies are what would make a capture safe there.
- [ ] **1024 concurrent detached children** is the ceiling: an unbounded spawn edge
  saturates the activation bound, and past it the scheduler-buffer guards report a
  located FATAL. Lifting it means a spill on the already-emitted overflow edge; the
  dynamic wait bucket, which is scanned, is the hard half (fork-join.md §V2.3).
- [x] **Tasks** — time-consuming (see Subroutines).
- [x] **Intra-assignment timing** (LRM 9.4.5) — blocking (`x = #5 y;`,
  `x = @(edge) y;`, `x = repeat(N) @(edge) y;`), nonblocking `#d` (deferred wheel
  record, last-executed-wins per LRM 10.4.2), nonblocking `@e` / `repeat(N) @e`
  (deferred record on the trigger edge). A **real** target works on every form,
  including the event ones: `emit_intra_event_arm` and `emit_intra_nba_arm` both
  fill the deferred record through `fill_intra_payload`, whose first act is
  `real_to_planes`, so the record carries the IEEE-754 bit pattern in its narrow
  slot — the event form's claimed gap was stale and is deleted (two cases in
  tests/intra_assign_test.cpp, values from iverilog 13). The repeat count may be
  **real** in either form and rounds per LRM 6.12.1 (`repeat (1.6) @e` waits two
  edges). A count <= 0 assigns as if there were no repeat in the blocking form; the
  nonblocking form still waits, its record countdown being unsigned — a TODO in
  `lower_assign`. Design: docs/design/intra-assignment-timing.md.
- [x] **`wait (expr)` statement** (LRM 9.4.3) — level-sensitive suspend; subsumes
  `wait(e.triggered)`.
- [x] **Named events** (LRM 15.5) — `event e;`, `-> e`, `->> e`, `->> #d e`,
  `@(e)` / `always @(e)`, `wait(e.triggered)`, at module, package, generate,
  procedural-block and `static`-local scope. An event is an OBJECT (trigger tick +
  intrusive waiter list + `->>` pending state) in the `clotho.events` array; its
  waiters are a doubly-linked list of frame- or array-resident nodes, so the number
  of simultaneous waiters is unbounded. Design: docs/design/named-events.md, and
  docs/design/event-handles.md for the handle layer.
- [x] **`fork` / `join`** (LRM 9.3.2, V1) — each child a coroutine; the parent
  joins all. Also inside a **task** body (and an assertion action block, which is
  lowered as one): a child takes the enclosing frame's packed integral /
  dynamic-array locals as `ref` formals, and the active-ring bound counts task
  activations rather than child bodies. The ring-overflow fatal the design doc
  promised is landed too (`rt_ring_overflow`).
  Two bounds that a fork child reaches but that were still sized as if it did not
  exist are fixed: the dynamic wait bucket counted `mod_.processes().size()`
  waiters, so a width-4 fork on one edge stored past the end of the bucket, and the
  `#0` batches were unguarded bump stores, so exceeding the saturated ring cap died
  with no diagnostic. The activation fixpoint is now `body_activations()`, shared by
  the ring and the bucket, and `emit_len_full_check` guards every bump-pushed
  buffer — both behind the existing `design_forks()` gate, so a design without a
  fork emits byte-identical code.
  Design: docs/design/fork-join.md.

### Continuous / structural timing

- [x] **Continuous-assign delay** (`assign #N lhs = rhs;`) — inertial delay on
  the driven net (LRM 10.3.3), pulse rejection via an epoch-cancelled detached
  record on the timing wheel, committed in the active region ahead of the drain
  (`emit_inertial_arm` / `rt_inertial_decide`). A delay on a whole driver of a
  multiply-driven (resolved) net rides that driver's own slot store, ahead of
  resolution (`store_driver_slot`). Gaps (guarded loud): rise/fall/turn-off delay
  triples `#(a,b,c)`, a select / concat / aggregate target, a net wider than 64
  value bits, and a delay on a PARTIAL driver of a resolved net (its slot powers
  up all-x, so the undriven bits would fold as x instead of z until the first
  store lands).
- [x] **Single-value gate delay** (`and #2 (...)`, LRM 28.16) — inertial, reusing
  the continuous-assign delay record (`emit_gate_as_process` -> `store_inertial_delay`).
  A gate driving a multiply-driven (resolved) net delays its own slot, and
  `cmos #d` / `rcmos #d` delay each of the two halves LRM 28.9 decomposes them
  into (identical waveforms to `nmos #d` + `pmos #d` under iverilog 13). Gaps
  (guarded loud): rise/fall/turn-off triples `#(a,b,c)` (now also rejected on
  cmos/rcmos), a bit-addressed output on a single-driver net, and a delay on a
  partial driver of a resolved net.
- [ ] **Net delays** (`wire #3 w;`) — delays every driver of the net (added to each
  driver's own delay, LRM 10.3.3); distinct from a continuous/gate assign delay.

### Procedural continuous assignment & force

- [x] **`force` / `release`** on a whole-scalar **variable** (integral or real; LRM 10.6.2,
  Phase 1): per-target hidden `force_site` state, gated ordinary writers, continuous
  re-eval process, last-force-wins. Includes **release-reestablish** of a variable
  driven by a module-level `assign` (the driver is made force_site-sensitive so
  release re-evaluates it). Input/inout-port targets guarded loud (port-copy driver
  is not force_site-aware). The gate keys off the write's ROOT, not its shape, so
  every ordinary write rooted at the target is suppressed: whole-variable,
  bit/part/element-select, packed-struct member, read-modify-write (`x += 1`,
  `x++`), a forced operand of a concatenation lvalue, and a task/function output
  copy-back — all byte-identical to iverilog 13. See
  docs/design/force-release.md.
- [x] **`assign` / `deassign`** (procedural continuous assignment to a variable,
  LRM 10.6.1, Phase 1b): second override layer below force (force > assign >
  ordinary procedural), `assign_site` state + gated re-eval process; `release`
  reverts to an active `assign` (the assign's re-eval process is force_site-aware);
  `deassign` holds; both last-wins. Full ladder byte-identical to iverilog.
- [x] **`force` / `release`** on **nets** (LRM 10.6.2, Phase 1.5): resolver-level
  override — a forced net always resolves through `emit_wire_resolvers`, which injects
  `force_site != 0 ? force_val : <driver fold>` and reverts on `release`. Uniform over
  single-driver, gate-driven, multi-driver/resolved, and undriven (reverts to z) nets;
  continuous RHS tracking, which iverilog does not do (it evaluates the
  net-force RHS once — its own documented limitation).
- [x] **`force` / `release`** on a constant **bit/part-select of a net** (LRM 10.6.2,
  Phase 2): per-force-site `{active, range-wide val, [lo,hi]}`; the net resolver stores
  the driver fold then positionally overlays each active site's bits (z-preserving),
  reverting on release. Non-overlapping sites exact; byte-identical to iverilog.
  Bit/part-select of a **variable** is LRM-illegal (§10.6.2) yet accepted by
  iverilog — rejected loud pending a decision (see below).
- [x] **Shape-agnostic write gate** for force/assign targets (LRM 10.6.2 suppresses the
  *update*, not the statement): one `store_or_gate` helper wraps the store alone, so the
  RHS, the lvalue's selectors and a read-modify-write's read still run (`x++` under force
  yields the forced value and leaves it standing). The write predicate is
  `pca_info_of_lvalue` — `force_info_of(lvalue_root(lhs))` — replacing the old
  force-STATEMENT predicate. Covers selects, packed-struct members, compound
  assignment, `++`/`--`, per-operand gating of a concatenation lvalue, and the
  output-argument copy-back — the last three were silently overriding the force before.
  Intra-assignment timing on a write to a force target (`x = #5 e`) is guarded loud: the
  update commits after the delay, so the gate must ride the deferred record.
- [ ] **`force` / `release`** on a **concatenation** of nets/selects (LRM 10.6.2), and
  the LRM-illegal-but-iverilog-accepted variable bit/part-select force.

### Real numbers

- [x] **`case` on a real selector / label** (LRM 12.5) — 12.5 words matching as a
  bit-for-bit `===` and Table 11-1 bars `===` on a real, so the standard defines the
  comparison only for integral operands; a real carries no x/z, which is the whole
  content of the 4-state rule, so it degenerates to IEEE-754 equality (the same call
  clotho already makes for strings, 6.16.9). A real selector or a real item label
  routes the whole statement to `lower_case_real`, which matches with `FEq` after
  widening both sides to `real` — an integral operand converts first (11.8.1), so
  `case (2.0) 2:` hits; `-0.0` matches `0.0`; a NaN selector matches nothing and
  takes `default`. It keeps 12.5's lazy first-match chain, so a later label's side
  effects never run once an earlier item matched; keeps the `unique` / `unique0` /
  `priority` 12.5.3 overlap and no-match violation checks, built from the same
  per-item predicates; and compares a `case ... inside` range member with `FGe` /
  `FLe` (11.4.13 membership is relational, which Table 11-1 does define for reals).
  The `(* full_case *)` / `(* parallel_case *)` analysis is skipped — it enumerates
  the selector's integral value space. The same matching rule applies to the `case`
  PROPERTY (16.12.16, `eval_case_match`). `casez` / `casex` over a real stay
  rejected, by slang's binder rather than by clotho: it refuses a non-integral
  selector for either keyword, reading 12.5.1's do-not-care positions as bit
  positions. iverilog accepts that spelling and treats it as a plain case, so
  this is a deliberate deviation. Still guarded loud on the real path exactly as
  on the integral one: `case ... inside` tolerance ranges (`+/-`, `+%-`) and `$`
  range bounds.
- [x] **Fractional (real) delays** (LRM 3.14 / 22.7) — a real `#delay` (procedural,
  intra-assign, continuous, gate) rounds to its module's time precision then scales
  to wheel ticks (`FtoI` half-away, LRM 6.12.1). The wheel tick is the design-wide
  GCD of every module's *unit* plus the *precision* of every scope that actually
  contains a real delay -- including inside a subroutine body
  (`scope_has_own_real_delay` / `design_tick_fs` / `global_scope_tick_fs` in
  `src/Lower.cpp`), so an integer-only design keeps the coarse minimum-unit tick and
  its generated IR is byte-identical to before — zero cost. A constant `#1.5` folds
  to a single tick constant; a runtime `#(rvar)` costs one round + one integer
  multiply. Follow-ups (only real-delay designs, and only these shapes, pay): (a)
  many *concurrent* fractional timers whose fine-tick periods all exceed the wheel's
  1024-slot window spill to the O(log n) overflow heap (~2.5x on a 64-timer
  microbench; a single/sparse clock is free — the wheel jumps straight to the lone
  overflow deadline). A hierarchical wheel (below) removes this; a cheaper partial
  win is to set the tick to the GCD of a module's *constant* real-delay values
  instead of its full precision when the module has no runtime real delay. (b)
  negative / NaN / inf real delays are not clamped (parity with the integer path).
- [x] **Subroutine delays and `$time` use the declaring scope's timescale**
  (LRM 3.14.2.3) — a `#5` or a `$time` read inside a task/function is in the time
  units of the scope that *declares* it, not the one that calls it. Clotho lowers
  each subroutine body once and shares it across call sites, so the caller's
  multiplier left at most one caller correct: a `1ns` package task called from a
  `1us` module delayed 5 us, silently, 1000x off. `function_id` now sets
  `time_mul_` / `prec_over_tick_` from `sub.getParentScope()->getTimeScale()`
  around the body build (`apply_time_scale`), which makes memoization sound —
  the body's scaling depends only on the declaration. Two supporting fixes: the
  wheel tick folds in the unit (and real-delay precision) of any package /
  `$unit` scope that declares a subroutine (`global_scope_tick_fs`), since those
  sit outside the instance tree and a coarser tick cannot represent their delays
  at all; and `scope_has_own_real_delay` now reads subroutine and class-method
  bodies, so a real delay inside a task rounds to its own precision (`#1.5` in a
  `1ns/1ps` task was rounding to 2 ns instead of 1500 ps). Cross-checked against
  iverilog; see tests/task_test.cpp.
- [x] **`real` / `shortreal` types** — storage, arithmetic, conversions, literals,
  parameters, comparisons, `?:`, sensitivity / NBA / continuous / ports / unpacked
  arrays, display formats.
- [x] **`$realtime`** — rejected inside plain functions (no wheel in the ABI).
- [x] **Real `$display` formats** (`%f`, `%e`, `%g`) and the 21 real math system
  functions (LRM 20.8.2). A real under `%d`/`%h`/`%x`/`%o`/`%b` also converts to
  its rounded integer (LRM 21.2.1.2 / Table 21-1, rounding per 6.12.1) and formats
  in the requested base. Still guarded loud: `%c` and `%u`/`%z` of a real.

### Observability & debug

- [ ] **VCD sparse / scope-subset selection** (whole-design dump only today).
- [ ] **VCD of a string signal** — blocked on the format, not the engine: IEEE
  1800-2023 21.7.2 defines no `string` var type, and iverilog and Verilator both
  omit string signals from the dump entirely. Belongs in FST, which carries
  variable-length values natively.
- [ ] **VCD of a multidimensional array**.
- [x] **VCD of a signal with no `$var` form is refused, loudly** — a $dumpvars over
  a design declaring a string, a container (queue / dynamic array / associative
  array), an unpacked struct or a class handle used to be silently wrong: the queue
  got `$var reg 1 ! q $end` aliased to its RING POINTER (never changing), the class
  handle dumped a heap address, and the unpacked struct SEGV'd in codegen on its
  null leaf slot type. `check_vcd_scope` now refuses the dump at lowering, naming
  the offending declaration, and the backend keeps a matching throw as the second
  line of defence (a compiler-synthesized signal is skipped instead — it is not
  design state). Walks exactly the set the backend registers, so a package-scope
  variable, which is never dumped, does not trigger it. The follow-up that removes
  the most common cause is dumping an unpacked struct / array as one `$var` per
  leaf.
- [x] **VCD of a named event** — `$var event 1`, and the bare pulse `1<id>` at
  each instant the event triggers (all three forms: `-> e`, `->> e`, `->> #d e`,
  hooked at the single `emit_event_fire` stamp point). The runtime diffs the
  event's last-trigger tick, so repeated triggers in one instant collapse to one
  pulse; a forced dump and the `$dumpoff` checkpoint never invent one, which
  iverilog does at time 0, `$dumpall` and `$dumpon`.
- [x] **`$monitor` / `$monitoron` / `$monitoroff`** (+ radix `$monitorb/o/h`,
  LRM 21.2.2) — one persistent monitor (a new `$monitor` replaces the previous),
  reprinting at the Postponed region of every time step only when an argument
  changed. A synthesized zero-formal body (`build_deferred_body`) renders the arg
  list and calls `rt_monitor_emit`, which prints only when the rendered output
  differs from the last (render-compare change-detection — no per-signal wiring).
  The body's ramp function pointer is registered via `MonitorSet`/`rt_monitor_set`;
  the drain (`emit_active_run`, after the `$strobe` loop, gated by
  `design_uses_monitor_`) indirectly ramps + resumes it each settled instant.
  `$monitoron`/`$monitoroff` toggle (reon reprints). Rejected inside a plain
  function (no wheel), like `$strobe`. Caveat: change-detection is on the rendered
  output, so `$time`/`%t` in the arg list reprints every step.
- [ ] **`$timeformat` dynamic-string suffix & zero-arg reset form** (only a
  string-literal suffix accepted).
- [x] **VCD dump** (`$dumpfile`, `$dumpvars`, `$dumpon`/`$dumpoff`, `$dumpall`,
  `$dumpflush`, `$dumplimit`) — whole-design dump, background writer thread,
  activity-driven dirty set, memories, reals, events (docs/VCD.md).
- [x] **`$timeformat` + `%t`**.
- [x] **`$strobe`** (LRM 21.2.2) — `$strobe`/`$strobeb`/`$strobeo`/`$strobeh`;
  a synthesized zero-formal task holds the deferred arg evaluation, ramped (not
  resumed) at the call site and drained once per settled instant in the
  Postponed region, just before the wheel advances. Rejected inside a plain
  function (no wheel in the ABI) and of an automatic-lifetime argument (its
  storage cannot outlive the call — slang's own binder catches this, matching
  iverilog).

### File and string I/O

- [ ] **Non-byte-multiple / part-select `$fgets` target**.
- [ ] **Scanf target width** — targets must be whole-storage and <= 64 bits.
- [x] **Streaming file output** — `$fstrobe` / `$fmonitor` (+ radix). The first
  argument is the destination descriptor; the synthesized deferred body targets
  that fd. `$fstrobe` reuses the `$strobe` Postponed drain; `$fmonitor` reuses the
  `$monitor` per-instant run and change-detection (`build_deferred_body` handles
  both the fd and monitor variants).
- [ ] **`$value$plusargs` `%e`/`%f`/`%g` (real) conversion**.
- [ ] **`$value$plusargs` runtime (non-literal) `str` argument**.
- [~] **Persist the Driver / SourceManager across runs** for buffer + parse
  caching (SM/loader/trees are public so a cache layer can sit above them).
  Within-process retention has landed: `lower_*` returns a `SourceContext`
  (`src/SourceContext.h`) owning the Driver / SourceManager / Compilation, so an
  `ir::SourceLoc` stays resolvable after lowering returns. Reuse ACROSS runs
  (parse caching) is still outstanding and needs the content-addressed cache.
- [ ] **`param-overrides` (`-G`), config/library-map (`--libmap`)** not yet surfaced.
- [x] **File output** (§21.3) — `$fopen` (MCD + FD), `$fclose`, `$fdisplay`/`$fwrite`,
  `$fflush`.
- [x] **File input / positioning** (§21.3.4–5) — `$fgetc`, `$ungetc`, `$fgets`,
  `$feof`, `$ftell`, `$fseek`, `$rewind`. `$fgets` into a `string` object
  reads the whole line with no fixed-width cap (`rt_fgets_str`); a
  packed-vector destination keeps the width-capped right-justified path.
- [x] **String formatting** (§21.3.3, §21.3.6) — `$sformat`, `$swrite`,
  `$ferror`. `$ferror` into a `string` object writes the message via
  `rt_ferror_str`, same error-code semantics as the packed-vector path.
- [x] **Format scanning** (§21.3.4) — `$fscanf`, `$sscanf`.
- [x] **Binary read** (§21.3.4) — `$fread` into a scalar or unpacked-array memory.
- [x] **Command-line plusargs** (§21.6) — `$test$plusargs` / `$value$plusargs`.
- [x] **slang `driver::Driver` frontend** — `-D`/`+define+`, `-I`/`+incdir+`, `-f`
  command files, `-y`/`-v` libraries, `--top`, `--timescale`, `--std`; single-unit
  compile. `--loose-timescale` accepts a design mixing elements that declare a
  time scale with elements that do not (LRM 3.14.2.2), giving the un-annotated
  ones `--timescale`'s value or `1ns/1ns`; refused by default, since a missing
  unit otherwise makes a `#5` mean whatever the design's finest unit happens to
  be.

### Structural primitives

- [x] **Tristate gates** `bufif0`/`bufif1`/`notif0`/`notif1` (LRM 28.6, Table
  28-5) — lowered as continuous-assign drivers that emit Z when disabled (X when
  the control is x/z; the weak L/H strengths collapse to X). Ride
  the existing driver-resolution path, so multiple tri-state drivers on one net
  resolve. `gate_tristate_value` in `Lower.cpp`.
- [x] **Gate delays** — single `#d` inertial delay landed (see Continuous /
  structural timing above). Remaining: rise/fall `#(r,f)` triples and the third
  (turn-off) delay of the tri-state gates (LRM 28.6).
- [x] **Drive strengths on gates** — the gate output's `(strength0, strength1)`
  threads to its driver slot and competes in the strength fold (see Drive
  strengths below); no charge / strength-reduction-through-gates.
- [x] **Gate outputs on 2-state targets** — the coercion policy is LRM 6.11.2's
  automatic 4-state -> 2-state conversion (x and z become 0), applied to the gate's
  1-bit fold by `gate_output_value` in `Lower.cpp` so a gate matches the equivalent
  continuous assign, whose conversion slang inserts on the RHS. Reaches a `bit`
  variable (LRM 10.3.2 allows one primitive output to drive a variable) and a
  2-state member of a 4-state packed net; a 2-state NET does not exist (LRM 6.7.1,
  enforced by slang's `isValidForNet`), so the question never had a net form. A
  SINGLE gate driving a packed-net member is still refused loud (`unsupported gate
  output terminal`: `gate_select_base` wants a `NamedValue` base, so `s.lo[0]` has
  no static (signal, bit) terminal) — only the resolved, multi-driver spelling of
  that shape works.
- [ ] **Concat / part-select gate output lvalues**.
- [ ] **Bit-select gate outputs** (one bit of a packed wire) — pay a whole-word
  read-modify-write and drop out of thunk/fusion eligibility; per-bit
  packed-signal fusion is the follow-up.
- [x] **MOS switches** — `nmos`/`pmos`/`rnmos`/`rpmos` (LRM 28.7, Table 28-6) and
  `cmos`/`rcmos` (LRM 28.9). Lowered on the tri-state gate path with a pass-device
  reading of a floating data input: a z data bit propagates as z instead of
  reading as x, so the whole DATA=z row is z. A `cmos` becomes the two drivers
  LRM 28.9 defines it as and folds through net resolution. No strength reduction
  through the r-variants, so `rnmos` ≡ `nmos` (LRM 28.14).
  `mos_switch_of` / `lower_cmos_switch` in `Lower.cpp`.
- [ ] **Bidirectional pass switches** — `tran`/`rtran` and `tranif0`/`tranif1`/
  `rtranif0`/`rtranif1` (LRM 28.8). Conduction is a net merge, not a driver: the
  unconditional pair could alias both terminals onto one signal, but the
  conditional ones need each side to see the other's resolved value excluding its
  own contribution, which the one-pass driver-slot fold cannot express.
- [ ] **UDPs** — combinational and sequential user-defined primitives.
- [x] **Combinational gate primitives** — `and`/`or`/`nand`/`nor`/`xor`/`xnor`/
  `buf`/`not`: n-input/output forms, instance arrays, x/z truth tables; lowered to
  self-starting continuous-assign comb processes.

### Net types, strengths, resolution

NOTE: anything here that can drive z (tri nets, tristate primitives, z-defaulted
unconnected ports) must seed the z-elision analysis (`compute_may_z`,
`ir/ZElision.cpp`) when implemented.

- [~] **Resolved / strength net kinds** — `wand`/`triand` (wired-AND),
  `wor`/`trior` (wired-OR), `tri0`/`tri1` (resistive Pull to 0/1), and
  `supply0`/`supply1` (Supply-strength constant 0/1 that beats a Strong driver;
  a Supply pull always folds as a strength driver, `net_pull_strength`) modeled
  (multi-driver resolution below). Remaining: `trireg` (strength / charge).
- [~] **Multiple-driver resolution** — default resolution (LRM 6.6) done for
  drivers of a **packed** net sharing one instance scope: whole-net drivers stage
  their value, constant packed bit/part-select drivers stage it with a Z
  background so each bit resolves only across its own drivers; a resolver process
  folds every driver's slot (`build_driver_slot_value` / `emit_wire_resolvers` in
  Lower.cpp). Reducers: `wire`/`tri` (`WireRes`, X on conflict), `wand`/`triand`
  (`WandRes`, wired-AND), `wor`/`trior` (`WorRes`, wired-OR) — all z ⊑ {0,1} ⊑ x
  lattice folds; `tri0`/`tri1` fold as wire, then `NetPull` fills undriven bits
  with 0/1 (LRM 6.6.4), always resolved (a driverless net still pulls). A slot is
  cut from the net's own type, hence always 4-state and always able to hold the Z
  background (LRM 6.7.1 admits no 2-state net; a 2-state MEMBER of a 4-state packed
  net converts per LRM 6.11.2 and resolves normally, so what used to be a guard is
  now the invariant `internal: resolved net storage is not 4-state`). Gate-primitive
  outputs stage into slots like any other driver. Remaining, all still loud-failing:
  unpacked-array / unpacked-struct nets (per-element slots + per-element resolver),
  concatenation-operand drivers (split into per-net stores), and cross-scope drivers
  (e.g. a shared interface net — the slots would span scopes). Non-overlapping
  constant bit/part-select drivers stay on the single-driver fast path.
- [~] **Drive strengths** — `(strong0, weak1)` etc. on continuous assigns, net
  declarations, and gates lower and resolve. Each driver slot carries a
  compile-time packed `(strength0, strength1)`; a net with any non-default
  strength folds through `StrengthRes` (level-by-level per-side-max, `Lower.cpp`
  `emit_wire_resolvers` / backend `lower_strength_res`) instead of `WireRes`.
  A `tri0`/`tri1` pull is injected as a synthetic Pull-strength driver so a
  Weak driver correctly loses to it. Symmetric strengths, open-drain /
  open-source (highz on one side), and pull competition all resolve exactly.
  `pullup`/`pulldown` (LRM 28.5) lower as a lone permanent Pull-strength driver
  slot of 1 / 0 (`lower_pull_source`), forcing the net resolved so a stronger
  driver wins and an all-off net holds the pull value.
  Remaining, all deferred: **strength ranges** (an x/z value on an
  *asymmetric*-strength driver spans two strengths — resolved to a definite
  value where the range model would yield x), drive strengths on
  `wand`/`wor` nets (loud-failing), `pullup`/`pulldown` on a bit/part-select
  target, strength reduction through gates, and `trireg` charge.
- [x] **Single-driver, z-defaulting net kinds** — `wire`, `tri`, `uwire`
  (single-driver `uwire`, LRM 6.6.7, complete).

### Hierarchy reach

- [ ] **Cross-top hierarchical refs** (`top2.sig` from `top1`) — each top has its
  own Lowerer / `sigmap_`.
- [ ] **Path into an instance nested in a generate block** (`g.u.sig`) or an
  instance-array element (`arr[i].sig`) — reported as a slang elaboration error
  before lowering. (Signals declared directly in a generate-loop block `g[i].sig`
  work.)
- [~] **`inout` / `ref` ports** (LRM 23.3.3) — shared-storage collapse: the child
  port's internal net/variable aliases the parent connection's signal
  (`build_inout_aliases`, no copy process), so both sides read/write one net and
  net resolution folds any drivers. Whole-signal connection with a single-scope
  effective driver works (either direction). Deferred, loud: a bit-select /
  part-select / concat connection (needs sub-signal aliasing the whole-signal
  collapse cannot express), and a bidirectional bus with drivers in two instance
  scopes (the general cross-scope multi-driver limitation below, not
  inout-specific).
- [x] **Cross-hierarchy references** (`top.u1.sig`, LRM 23.6) — reads and writes
  any direction, bit/part-select, `@(edge)`, blocking and nonblocking assign.
- [x] **Unconnected ports** — unconnected input reads `z` (LRM 6.6).

### Timing checks (basic)

- [ ] **`specify` blocks** — module path delays (`(a => b) = 3;`) and the common
  timing checks `$setup`, `$hold`, `$setuphold`, `$width`, `$period`. (Full set
  and SDF annotation are M5.)

---

## M3 — Common SystemVerilog

### Data types

- [ ] **2-state call-ABI micro-opt** — the function/task ABI still passes a 2-state
  formal as the packed (mm, z) pair (constant-0 z). Size opt, not a correctness gap.
- [ ] **NBA to a string**.
- [ ] **NBA to a class property** (`c.x <= v`) — legal SV, refused loudly. Every
  NBA staging buffer (the scalar shadow slot, the aggregate image, the container
  handle record) is indexed by `Signal`; an object body is a heap block with no
  signal, so a property reaches none of them. Needs staging keyed by destination
  ADDRESS rather than by signal, with last-write-wins resolved by comparing
  addresses at commit. See docs/design/classes.md §8.
- [ ] **Intra-assignment delay on a container NBA** (`q <= #2 r`) — refused loudly.
  The intra-assignment path stages an integral or real value, an unpacked-array
  element or a struct field; a container handle needs the HandleCopyNb record to
  survive to the delayed commit. See docs/design/intra-assignment-timing.md.
- [ ] **Non-constant string replication count**.
- [ ] **String ports** — including a string reached through a struct member or a
  container element, which the old array-only walk let through to the plane-copy
  path (now guarded).
- [ ] **String event control / sensitivity**.
- [ ] **`?:` / `case` over strings**.
- [x] **Strings inside structs / queues / dynamic arrays** — recursive aggregate
  string leaves (init/deinit/copy walks) and rt_elem_ops string elements. A
  string as an associative-array VALUE element is now supported (a[k] routes the
  string handle through the OOB sink); string KEYS are supported.
- [ ] **enum non-constant `next`/`prev` step**.
- [x] **Nonblocking whole-object copy / equality over an unpacked-array member**
  — NBA of a struct with an array member decomposes per leaf (deferred queue);
  `==` / `!=` recurse into array members (per-element AggEq).
- [x] **Aggregate task arguments, and `output`/`inout` function arguments** —
  one storage pointer per aggregate formal in every by-value direction
  (`declare_task` mirrors `declare_function`); the callee copies in at its
  prologue (a task: at the body's start block) and copies an `output` / `inout`
  formal back at return (a task: in the completion epilogue, after any delay).
  Covered for nesting, recursion, fork concurrency, a class method task, and
  struct-with-array / array-of-struct / 2-D / slice actuals.
- [x] **`ref` aggregate formal** — subsumed by the uniform ref above; an aggregate
  ref aliases its image pointer with no copy either way.
- [ ] **Non-lvalue aggregate actual** (`'{...}` pattern / call result) — needs a
  materialized caller temp; guarded loud today for functions and tasks alike.
- [ ] **String-containing aggregate by value** through a function.
- [x] **Whole aggregate in a general rvalue context** — `$display %p`
  (LRM 21.2.1.6, lower-time `'{...}` expansion; a tagged union and a
  queue / dynamic array render at runtime instead) and streaming-concat operand
  (LRM 11.4.14, leaf bit-serialization). A whole-array by-value read outside
  these contexts routes through the function-ABI copy-in, not a flat load.
- [ ] **inout / ref aggregate PORTS** (the subroutine side is done). A port copy is
  a continuous assignment, so it needs change detection over aggregate storage.
- [ ] **Aggregate port driven by a `'{...}` pattern / call**.
- [x] **Event control on a member / select / expression** (`@(posedge x.f)`,
  `@(arr[i])`, part-selects). `event_signal` synthesizes a temp net continuously
  assigned from the expression and subscribes to it (bit 0 = the expression's
  LSB, exact for edge; whole word for level). One delta of latency vs. a direct
  signal. Integral operands only; real / aggregate operands rejected loud.
- [x] **Unions** (LRM 7.3) — packed and unpacked. Both lower to a flat plane
  value (`Kind::Struct`, is_packed, is_union): a packed union is the widest
  member's width (all equal, 7.3.1); an unpacked union is sized to the widest
  member, every member overlaying the LSB (the natural, verilator-matching
  overlay). Member access reuses the packed part-select path
  with no backend changes; whole-union assignment, NBA, VCD, and change detection
  all ride the normal plane-signal path. Unpacked-union members are restricted to
  integral types (real / string / unpacked aggregate / handle members gated loud —
  no flat-bit overlay).
- [x] **Tagged unions** (LRM 7.3.2) — both packed and unpacked lower to the same
  flat `{tag, payload}` bit-vector: the tag in the high `tagBits` bits (packed:
  slang's `tagBits`/width; unpacked: synthesized `ceil(log2 n)`), the member
  payload overlaying the LSB. The `tagged Member(v)` setter
  (`TaggedUnionExpression`) is the only thing that moves the tag; it concatenates
  tag / gap / payload, and the gap between a narrow payload and the tag takes the
  power-on default (x for 4-state, 0 for 2-state), matching slang const-eval so
  runtime and folded-parameter forms agree. **Every member access — read and
  write — is tag-checked at run time** (LRM 11.9: an attempt to read or assign a
  value inconsistent with the tag "results in a run-time error"), emitted from the
  single `lower_member_lvalue` funnel as a `TaggedCheck` node — one 4-state
  compare plus a branch weighted toward the fall-through, into the
  `rt_tagged_access` located fatal. A member write requires the tag to *already*
  select that member and never retags (LRM 7.3.2: "The tag and value can only be
  updated together consistently using a statically type-checked tagged union
  expression"); an x/z tag selects no member and reports. The check is elided for a
  plain union and for a single-member tagged union, and is never reached from a
  pattern-matched arm. `%p` prints `'{tag:value}` with the payload at the active
  member's own width and signedness (LRM 21.2.1.6, runtime `TaggedFmtP` /
  `rt_tagged_format_p`);
  `%b` stays the raw N-bit value, gap bits included. Member read, whole-union
  assign / `==` / VCD ride the flat path. Members restricted to integral (+ void).
  Pattern matching (`case matches` / `if matches` / `?: matches`, LRM 12.6) is
  implemented — see Pattern matching under M5 Language corners. iverilog /
  verilator have no tagged-union support, so neither can cross-check this
  shape; the 11.9 run-time check is enforced because 11.9 requires
  it. See docs/design/tagged-unions.md.
  Remaining gaps:
  - [ ] Non-integral member of an **unpacked** tagged union (`real` / `string` /
    unpacked aggregate / class payload) — the Phase 3 gap, gated loud.
  - [ ] `force`/`release`, DPI, and queue / dynamic-array / associative-array
    element positions loud-fail for the **unpacked** tagged-union form only (all
    key on slang's `isIntegral()`); the packed form works in each.
  - [ ] `%p` of a queue / dynamic array of tagged unions — rejected loud; the
    runtime container descriptor carries no member-name table.
  - [ ] VCD dumps a tagged union as one flat anonymous `$var`. LRM 21.7 has no
    tagged-union notion so this is defensible, but no variant name is recoverable
    from the dump.
  - [ ] Latent: an all-`void` unpacked tagged union computes
    `max_width + tag_bits - 1` with both terms zero, underflowing. Currently
    masked — the width lands at 1 and VCD shows `$var reg 1` — so there is no
    observable misbehaviour. Recorded as latent, not as a bug.
- [x] **Unpacked-array full slicing / whole-array assignment** beyond the memcpy
  path (ArrSlice IR node for constant bounds, position-wise element expansion for
  an indexed part-select; per-element copy/eq/store; see Memories & arrays).
- [x] **Nonblocking `'{...}` to an unpacked array**.
- [x] **Associative-array `'{...}` literals** (LRM 7.9.11) — `'{key:value, …}`
  and the `default:` value a read of an absent key yields, at a declaration or as
  an assignment, on a module variable / procedural local / frame storage. The
  default is per-array runtime state (an `AssocMap` slot, not an entry): it is
  invisible to `num`/`exists`/`first`/`next`, survives `a.delete()`, travels with a
  whole-array copy, and does NOT seed a created entry. Before
  this the initializer was **silently dropped** (a zero-width store into the
  handle slot), which is what made it a bug rather than a gap. Guarded loud: a
  nonblocking literal, and an aggregate value element (needs a per-leaf store per
  entry). Dynamic-array / queue `'{...}` literals already worked.
- [x] **Non-POD dynamic-array / queue elements** (LRM 7.5 / 7.10) — every element
  kind but an associative array or an event: integral of any width, real, `string`,
  **class handle**, **unpacked struct / union or fixed unpacked array** (with
  string / container leaves at any depth), and a **nested queue / dynamic array**.
  Both containers share one gate (`guard_container_element`) because they share
  DynElem and the `rt_elem_ops` descriptor.
  * A class-handle element is a plain 64-bit POD word: objects are never freed
    (classes.md §7), so it needs no lifecycle, `null` is the all-zero Table 7-1
    default, and `==` is the identity compare a word compare gives.
  * An aggregate element slot holds the recursive storage image, and
    `dyn_agg_elem_lvalue` hands back a HEAP-ANCHORED aggregate lvalue — base
    advanced to the physical slot, `dyn_inrange` carried — so `q[i].f`, `q[i][j]`,
    whole-element copies, `foreach` and equality all reuse the ordinary aggregate
    machinery with no container-specific code. Whole elements cross the
    `rt_queue_*` ABI by pointer (no ABI change: `elem` / `out` were already
    `void*`); a `'{...}` actual stages into a canonical aggregate temp.
  * A managed leaf (string / nested container) inside an aggregate element rides a
    new **kind-4 `rt_elem_ops` leaf table**: the backend emits an offset-sorted
    `{offset, ops}` array beside the descriptor, and the runtime deep-copies each
    leaf and raw-copies the POD gaps between them (that walk is why the order is a
    contract). The runtime side already existed and was dead code.
  * A nested container element is a handle slot chaining through
    `rt_elem_ops::inner`. Out of range it redirects to a per-element-type sink
    handle, emptied at every access, so `q[99].size()` is 0 and `q[99].push_back(x)`
    is dropped (LRM 7.4.5) instead of addressing the heap past the live length —
    the same shape as the string-element sink.
  * A pop RELOCATES the element into a body-lifetime buffer, so the buffer owns
    whatever heap storage it carries: the pop site releases the previous contents
    (a pop in a loop) and every body exit releases the last (`build_agg_elem_temps`
    + `deinit_body_strings`). Verified leak-free and double-free-free under
    ASan/LSan.
  * Fixed en route: `str_slot_ty` was `{ [32 x i8] }` (align 1) while `rt_string`
    is `alignas(8)`, so a string leaf inside ANY aggregate (a plain struct signal
    included) was accessed misaligned — UB that UBSan reported 13 times on a
    two-member struct. The mirror now leads with an i64.
  An associative-array element works too, through the `rt_elem_assoc` (kind 5)
  descriptor. Still guarded loud: an event element, which needs one event OBJECT per
  element; the object slot array is sized at compile time from the declared events
  (docs/design/event-handles.md §9).
- [x] **NBA to a dynamic array** — and to an associative array. `HandleCopyNb`
  stages a deep copy in the Active region and MOVES it into the live handle in the
  NBA region, generalizing the queue's path to all three container kinds. A
  literal / `new[n]` right-hand side is still refused (it builds in place rather
  than deep-copying a handle the commit can move).
- [x] **Array ordering methods** (LRM 7.12.2) — `reverse` / `sort` / `rsort` /
  `shuffle` on a dynamic array or queue (`rt_dynarr_*` / `rt_queue_*` helpers).
  `reverse` / `shuffle` are element-agnostic; `sort` / `rsort` are value-keyed,
  restricted to a 2-state integral (width <= 64) or real element. `shuffle` draws
  from the calling thread's RNG (which forces its process to a coroutine, and
  reaches a subroutine body through the call ABI). Guarded loud: `sort` / `rsort`
  `with` iterator clause; wide (> 64-bit) elements; and 4-state `sort` ordering —
  deferred deliberately because the LRM (7.12.2) leaves x/z placement
  unspecified: a 4-state `<` yields x rather than a total order, so any ordering
  would be an invented one, and guarding loud beats silently picking.
- [x] **Array locator + reduction methods** (LRM 7.12.1 / 7.12.3) and the `with`
  iterator clause (7.12 / 7.12.4) — `find` / `find_index` / `find_first` /
  `find_first_index` / `find_last` / `find_last_index` / `min` / `max` /
  `unique` / `unique_index`, and `sum` / `product` / `and` / `or` / `xor`, on a
  dynamic array or queue. Lowered as one IR loop over the container, not a
  runtime helper: the clause is arbitrary IR, and going through the ordinary
  expression path makes 4-state, wide, real, string and struct elements work with
  no per-type comparator (so none of `sort`'s element restrictions apply here).
  A locator fills a fresh result queue; a reduction folds at the result type's
  width, so a `byte` sum wraps (7.12.3). `item.index` resolves through a binding
  stack, so a nested clause and `unique`'s dedup scan both keep their own index.
  Empty-array reductions answer 0 rather than the fold identity and 4-state
  `min`/`max` skip unknowns. An unpacked struct / array element works through
  the same loop: the iterator variable binds by
  aggregate copy and a locator's result queue receives whole elements, so
  `q.find(x) with (x.f > 2)` / `q.sum(x) with (x.f)` are ordinary. Guarded loud: a
  fixed-size unpacked-array or associative-array receiver (7.12 allows both; the
  former needs declared-range index mapping, the latter key traversal), and an
  aggregate element with NO `with` clause — the bare form makes the element itself
  the key (7.12.1), and an unpacked aggregate has no ordering or fold.
- [ ] **`with` clause on `sort` / `rsort`** (LRM 7.12.2) — needs a key-buffer
  permutation sort in the runtime; the iterator infra it shares with the locator
  methods now exists. Plus 4-state `sort` ordering (deferred: the LRM leaves x/z
  placement unspecified) and wide-element `sort`. Full scope in
  `docs/design/array-methods.md`.
- [ ] **`map`** (LRM 7.12.5, 1800-2023 only) — the one array manipulation method
  slang binds that clotho does not lower, so `q.map(x) with (x * 2)` hits the
  container-method guard (`--std 1800-2023`; older versions reject it in the
  binder). It is the locator loop with the CLAUSE VALUE collected instead of the
  element, so it reuses the `with`-iterator infra; the result keeps the receiver's
  container kind with the clause's element type (queue -> queue, dynamic ->
  dynamic, assoc -> same key type, fixed -> same range).
- [ ] **Multidimensional dynamic arrays**.
- [x] **`%p` of a container with a non-scalar or wide element** (LRM 21.2.1.6) —
  the runtime formatter renders an element from a flat descriptor (one word, no
  member names), which covers a single-word integral, a real and a string. An
  aggregate, a nested container, an element wider than one word and a tagged union
  now render through a frontend loop instead: one `$sformatf` per element built by
  the ORDINARY aggregate expansion, concatenated over the live element count. So
  member names, nested arrays, string leaves and tag names all print exactly as
  they do outside a container (`'{'{a:1, s:"hi"}, ...}`). Still guarded: an
  associative-array leaf, and a class-handle element (no handle rendering at all).
- [x] **NBA to a whole queue** (`q <= rhs`, LRM 10.4.2) — `QueueCopyNb` stages a
  deep copy of the RHS in the Active region (per-queue `rt_nba_arr` buffer), then
  `rt_queue_move`s it into the live handle in the NBA region (managed-element
  safe). Pattern/concat/slice RHS reuse the materialize path.
- [n/a] **NBA to a queue element** (`q[i] <= x`) — **illegal SystemVerilog**, not
  a gap. LRM 10.4.2 forbids a nonblocking assignment to an element of a
  dynamically-sized array (queue / dynamic array); slang rejects it before Clotho
  ("nonblocking assignments to elements of dynamically sized arrays are not
  allowed"), which sidesteps the sample-vs-commit realloc hazard by disallowing
  the construct. Nothing to implement.
- [x] **Queue slice `q[a:b]`** (LRM 7.10.4) — materialized element-wise into a
  temp handle via `QueueAppendRange` (`rt_queue_append_range`), usable in any
  rvalue position; bounds runtime-clamped. Dynamic-array slice still guarded.
- [x] **Non-variable queue assignment** (concat `{q,e}` / pattern `'{...}`) —
  `materialize_queue_rvalue` builds a temp handle (alias-safe for `q = {q,e}`),
  then a deep copy into the target; splices whole-queue operands, pushes scalars.
  A queue that IS an element or a member of something (`q = qq[i]`, `q = s.inner`)
  needs no temp: it already has a handle lvalue.
- [x] **Queue / dynamic-array equality** (`==` / `!=`, LRM 11.2.2/11.4.5) — direct
  and as an aggregate member, reached from the AggEq leaf walk: size mismatch is a
  definite 0, else per-element 4-state compare with x-propagation. Fixed a prior
  silent miscompile (handle compared as a value). A single-word integral / real /
  string element goes through `rt_container_eq`; an aggregate, a nested container
  or an element wider than one word walks element-wise in the backend
  (`emit_container_eq_loop`), recursing through the same aggregate compare — which
  is also what gives a queue of queues (and a struct holding a queue) a compare.
- [ ] **Associative arrays** — `[*]`/typed keys, `.exists`, `.first`/`.next`,
  `.num`, `.delete`.
- [ ] **Bit-stream casting** (`{<<{}}` / `{>>{}}` cast operands, LRM 6.24.3).
- [ ] **Casts to/from enum / struct / union**. `$cast` is tracked under M4. Open
  question, low confidence that it is a real violation: `$cast` into a **packed
  tagged union** is a plain bit cast and is *not* tag-checked, so it can install a
  tag/payload pairing no `tagged` expression could build — which LRM 11.9's
  "completely type-checked" accesses then trip over. Packed-to-packed bit casts
  are legal, so this may be correct as it stands; needs a reading. Note the entry
  previously claimed union casts were unsupported, which under-claimed what
  actually happens.
- [x] **2-state and sized integer types** — `bit`/`byte`/`shortint`/`int`/`longint`
  as genuine 2-state; 4-state<->2-state conversion (LRM 6.3.1).
- [x] **`string` type** (LRM 6.16) — SSO storage, assignment, concat, comparisons,
  `s[i]` read/write, the method set, string<->packed casts, `%s`, by-value
  args/returns, fixed unpacked arrays, string conditions.
- [x] **`enum`** — base type, member folding, methods (LRM 6.19:
  `first`/`last`/`num`/`next`/`prev`/`name`).
- [x] **`struct`** — packed member select/assignment; unpacked struct / arbitrary
  aggregate nesting (recursive storage mirror, composed-GEP path); read,
  blocking/nonblocking write, whole-object copy / `==` `!=`, by-value through
  functions, across module ports, aggregate net declaration assignment. Design:
  docs/design/unpacked-struct.md.
- [x] **Assignment patterns `'{...}`** (LRM 10.9) — positional, keyed
  (member / array-index / `default`), and replicated forms over packed structs /
  vectors / multi-dim packed arrays, unpacked structs, and fixed unpacked arrays.
- [x] **Assignment-pattern lvalue destructuring** (`'{a,b,c} = rhs` / typed
  `U'{a,b,c} = rhs`, LRM 10.9) — positional only, over a packed value (bit-slice,
  shares `store_concat`), an unpacked struct, or a fixed unpacked array
  (structural per-element walk); nested pattern/concat members recurse. Element
  count / keyed-or-replicated-as-lvalue / compound-assignment are guarded loud
  (slang itself rejects most of these at binding time already). Queue / dynamic
  / associative-array right-hand side, and a string / handle-typed member, are
  guarded loud (not supported); iverilog and verilator both reject the syntax
  outright.
- [x] **`typedef`** — alias resolution, canonicalized at `lower_type`.
- [x] **Packed multidimensional arrays** — element / range / nested bit & part
  select, read + lvalue, constant + dynamic indices, arbitrary nesting (LRM
  7.4.5/7.4.6). Array query functions `$size` / `$dimensions` /
  `$unpacked_dimensions` / `$left` / `$right` / `$low` / `$high` / `$increment`
  (LRM 20.7).
- [x] **Dynamic arrays** — `[]`, `new[n]` / `new[n](src)`, `.size()`, `.delete()`,
  element r/w, whole-array deep-copy, `foreach`, full subroutine ABI.
- [x] **Checked container allocation** — every heap buffer a design's containers
  need routes through `rt_alloc_checked` / `rt_alloc_elems_size` (Runtime.h).
  Three silent failures are now located fatals instead of a corrupt heap: a
  `new[n]` count that arrived zero-extended (so `new[-1]` asked for 4294967295
  elements — the count is `int64_t` at the ABI now, the backend sign-extends a
  signed count and zero-extends an unsigned one, and the runtime rejects `n < 0`),
  an element-count-times-stride product formed by wrapping multiplication
  (`__builtin_mul_overflow` now), and a `malloc` that simply returned null. Each
  reports through an `rt_alloc_site` constant the backend emits beside the call,
  so the diagnostic carries the file position, source line and caret rather than
  naming the runtime. The only ceiling is `size_t`. Covers dynamic arrays,
  queues, strings (including the `{N{s}}` replication product) and associative
  arrays; coroutine frames (`rt_alloc`) keep their own path.
- [x] **Queues** — `.size()`, push/pop/insert/delete, `q[i]` / `q[$]`, `foreach`,
  whole-queue deep-copy, bounded `q[$:N]`, full subroutine ABI.
- [x] **Associative arrays** (LRM 7.8/7.9) — all key kinds (integral of any
  width / enum / packed / `string` / `[*]`-wildcard / **class handle** (7.8.3) /
  **unpacked struct or fixed array** (7.8.5), unified as normalized
  order-preserving byte spans, see docs/design/dynamic-arrays.md §4a), index
  r/w, `.num`/`.size`/`.exists`/`.delete(k)`/`.delete`/`.first`/`.last`/`.next`/
  `.prev`, ascending-key `foreach`, whole-array deep-copy, `'{key:value,
  default:v}` literals (7.9.11), full subroutine ABI.
  Value element: every kind a queue / dynamic-array element takes (one shared
  rule, `guard_container_element`) — integral / real / `string` / class handle /
  unpacked struct or array including one with string or container leaves /
  a nested queue, dynamic array or associative array (`rt_elem_assoc`). Member,
  element and whole-value r/w, equality, absent-key default. `a[k]` resolves its
  entry by DIRECTION: a mutating method creates it, a query reads through get_ptr
  and falls back to the empty-container default, so `a["absent"].size()` is 0 and
  leaves `num()` alone (LRM 7.8.6).
  Still guarded loud, each for a stated reason (docs/design/data-model.md §7): an
  element select THROUGH a container-valued entry (`a[k][i]`) needs composed
  deferral, since the select would have to pick a direction that belongs to its
  consumer; and a `string` or dynamically-sized member inside a struct KEY needs
  the order-preserving escaped encoding (the normalized span would otherwise be
  variable-length). Whole-array NBA works (`HandleCopyNb`); a literal RHS under
  NBA does not. An x/z index is invalid per 7.8.4 / 7.8.5: the read yields the
  element default, the write is dropped without creating an entry, `exists`
  answers 0 and `delete` is a no-op. iverilog cannot parse associative arrays at
  all, and verilator creates an entry on an absent read, violating LRM 7.8, so
  neither models this behaviour.
- [x] **Casting (scalar/packed)** — static / size / sign / const cast,
  string<->packed, real<->int.
- [x] **Parameterized types / type parameters** (LRM 6.20.3).

### Operators & expressions

- [ ] **Concatenation compound-assignment lvalue** (`{a,b} += x`).
- [ ] **String-element compound-assignment lvalue** (`s[i] += c`).
- [ ] **`inside` string / real left operand**.
- [ ] **`inside` unpacked-array set member** (`sel inside {arr}`).
- [ ] **`inside` tolerance ranges** (`[c +/- d]`).
- [ ] **`inside` unbounded `$` range bounds**.
- [x] **Part-select / dynamic bit-select OOB bounds check** (LRM 11.5.1: OOB read
  -> X (2-state: 0), write -> no-op, per bit for a partially out-of-range access;
  an x/z index is wholly out of range). Clamped against the container's declared
  range in both directions, negative indices included; `--sanitize=bounds` reports
  the same sites. A **runtime-offset** select of a vector wider than 4096 bits goes
  through the runtime word-indexed path (`rt_wide_sel` / `rt_wide_ins`) rather than
  a whole-value variable shift, which cost O(width^2) JIT time and made a 65 Kbit
  vector (ivtest pr722) appear to hang; blocking and nonblocking, any selection
  width. See tests/wide_select_test.cpp.
- [ ] **Dynamic part-select** (`sig[msb_expr:lsb_expr]`) — variable bounds
  (const-bounds only today).
- [ ] **Streaming operators LHS** — unpack (assignment target); `with` clause;
  dynamic/string operands.
- [x] **Logical implication / equivalence** (`->`, `<->`) in expressions
  (LRM 11.4.7) — dedicated `Limplies`/`Lequiv` IR ops with direct 3-valued
  truth tables. `->` short-circuits (skips lowering the right operand
  entirely when the left is definitely false, LRM 11.3.5); `<->` always
  lowers both operands exactly once (LRM 11.3.5 excludes it from the
  short-circuit list). iverilog does not implement `->`'s short-circuit but
  otherwise agrees on truth-table values.
- [x] **Increment / decrement** (`++`, `--`, pre and post, LRM 11.4.2).
- [x] **Compound assignments** (`+= -= *= /= %= &= |= ^= <<= >>= <<<= >>>=`,
  LRM 11.4.1) — single-evaluation LHS.
- [x] **Assignment as an rvalue** (`y = (x += 1)`, LRM 11.3.1).
- [x] **Case-equality operators** (`===`, `!==`).
- [x] **Wildcard equality** (`==?`, `!=?`) — wildcards only the right operand's x/z.
- [x] **Short-circuit `&&` / `||`** — value-producing `SelectLazy` (LRM 11.3.5, 11.4.7).
- [x] **`inside` operator** (LRM 11.4.13) — integral scalar / packed operands
  (members compare with `==?`, so a member's x/z bits are do-not-cares and the
  left operand's are not) and REAL operands, which 11.4.13 compares with `==`
  because they are nonintegral: `FEq` per value member and `FGe` / `FLe` for a
  range, folded with the same OR reduction. An UNPACKED-ARRAY member is traversed
  elementwise ("descending into the array until reaching a singular value"): a
  fixed-size member unrolls and recurses through nested dimensions, a queue or
  dynamic array walks at runtime (11.4.13's own example is a queue member).
  iverilog 13 cannot compile `inside` at all. Still guarded loud: a string left
  operand, an associative-array member (its traversal is the 7.9.4 first/next
  key walk, not an index loop), a queue / dynamic-array member whose elements
  are themselves non-singular, tolerance ranges and `$` bounds.
- [x] **Indexed part-select** (`sig[base +: width]`, `sig[base -: width]`).
- [x] **Streaming operators RHS** (`{<<{...}}`, `{>>{...}}`) — pack, fixed-width,
  nested.

### Procedural control

- [ ] **`case … inside` tolerance ranges** (`[a +/- b]`, `[a +%- b]`).
- [ ] **`case … inside` unbounded range bounds** (`[$:a]`, `[a:$]`).
- [~] **`unique`/`unique0`/`priority` Observed-region reporting** — reports now
  emit in the Observed region (LRM 4.4.2.4) via the deferred report queue
  (`rt_observed_flush`), not the Active region. Still missing: per-statement
  *maturation* (flush-on-re-execution) for glitch immunity per LRM 12.4.2, which
  lands with deferred immediate assertions (they share the queue).
- [ ] **`&&&` / `matches` predicates** in `if`/`?:`, and `case … matches` pattern
  matching.
- [ ] **`always_ff` non-sequential-behavior warning** (LRM 9.2.2.4) — the block
  executes correctly; slang emits no diagnostic when the body is not sequential.
- [x] **`foreach`** (12.7.3) — fixed-size arrays + `string`; dynamic arrays and
  queues iterate the runtime size; associative arrays iterate keys in ascending
  order via first/next (a `[*]`-wildcard index has no key type, so foreach over
  it is illegal per LRM and rejected by slang).
- [x] **`do … while`**.
- [x] **`break` / `continue`**.
- [x] **`case … inside`** (12.5.4 / 11.4.13) — singular labels via wildcard
  equality, `[lo:hi]` ranges via inclusive membership; jump-tables a run of
  clean-constant singular labels.
- [x] **`unique` / `unique0` / `priority` on `case` and `if`** — no-match and
  overlap violation reports (LRM 12.5.3 / 12.4.2).
- [x] **`final`** blocks (see M1 Procedural control).
- [x] **`void`-cast calls, function/task default args, named arg binding** — slang
  fills defaults / reorders named args into the positional list.

### Hierarchy & namespaces

- [ ] **Interfaces & modports, remaining** — bidirectional/multi-driver interface
  nets (a net continuously driven from both sides fails loud, shared with `inout`;
  single-driver works), modport `export` methods, and structural modport
  expression ports (`.name(sig[hi:lo])` / concat; the whole-signal rename form
  `.name(sig)` works). Virtual interfaces (LRM 25.9) are under M4, below.
- [x] **Interfaces & modports** (LRM 25) — interface instantiation, plain and
  modport-restricted interface ports, generic interface ports (`interface p`),
  modport views/directions, whole-signal modport expression (rename) ports,
  signals/tasks/functions in interfaces, parameterized interfaces, interface
  arrays, and interface ports forwarded through submodules. Storage is shared
  (no copy process): every reference — plain-port, modport, rename, or
  hierarchical — canonicalizes to the interface instance's own `ValueSymbol`
  (`canonical_value_symbol` in `src/Lower.cpp`). iverilog has no interface
  support. Design: docs/design/interfaces.md.
- [ ] **`bind`** — bind a module/interface into another scope.
- [ ] **Nested / extern module declarations**.
- [ ] **`export pkg::*;` pure pass-through** — a name the exporting package never
  itself references fails to resolve (slang lookup limitation). The
  internally-referenced form works.
- [ ] **Package-scope classes / covergroups / checkers / nettypes** — hit their
  own (pre-existing) unsupported-construct guard if referenced.
- [x] **Packages** — `package`/`endpackage`; `import` (explicit `p::name` and
  wildcard `p::*`), bare `pkg::name` resolution, `$unit` scope, package/`$unit`-scope
  variables (one shared signal). Partial `export` re-export and package-importing-
  package verified vs iverilog.

### Classes (basic OOP)

- [x] **Class declaration & objects** (Phase 1) — properties (scalar / real /
  string / nested struct-array / self-referential handle), methods (functions +
  tasks, `this`, recursion), constructor `new` + default property initializers,
  field read/write, handle assignment (reference semantics), `null`, `==`/`!=`
  identity, pass-by-value through the subroutine ABI. A class handle is a 2-state
  64-bit value (like `chandle`); the object body reuses the unpacked-aggregate
  storage. Lifetime leaks in V1 (LRM 8.4 objects are GC'd — unobservable). See
  docs/design/classes.md; verified vs iverilog in tests/class_test.cpp.
  Deferred within Phase 1, each guarded loud: nonblocking assignment to a class
  property; class handles as queue / dynamic-array / assoc-array elements;
  `$display("%p")` of a handle. A property initializer may now CALL a method of its
  own class (LRM 8.7 runs initializers as part of construction, so `this` is in
  scope; `ctor_this_` carries the object under construction from the `new` site and
  is consulted ahead of the enclosing frame's `this`, which the old code handed to
  the callee whenever the `new` sat inside another object's method — a silent
  miscompile, not just a refusal). A bare instance-method call with neither an
  object under construction nor a frame `this` is still guarded loud. A property
  initializer that READS a sibling property resolves the same way, through
  `value_lvalue`'s own `ctor_this_` arm: it used to die as an unknown signal when
  the `new` sat outside a method, and silently read the ENCLOSING object's
  same-named property when it sat inside one.
- [x] **Shallow copy** (`dst = new src`, LRM 8.12) — allocates a second object and
  copies every non-static property member-wise, running neither the constructor nor
  the declaration initializers (8.12 step 1). What each property does follows from
  its type, through the same `emit_agg_copy` an aggregate assignment uses: a string
  / queue / dynamic array / associative array property deep-copies, a class-handle
  property copies as a handle, so the copy and the source share what they point at
  (8.12: "objects, however, are not copied, only their handles"). The object
  allocated is of the SOURCE's class, not the expression's static type: every
  class carries a synthesized copy thunk in a reserved descriptor slot
  (`kCopySlot`), so `new base_handle` dispatches to the concrete class and allocates
  its size (iverilog 13 asserts on the polymorphic case, so it cannot cover
  this). A null source is the usual located fatal. Randomization state (8.12 step 2)
  is moot: `rand`/`randc` properties and covergroups are refused upstream.
- [x] **Null-handle dereference is a located fatal** (LRM 8.4) — accessing a
  non-static property, or dispatching a virtual method, through a handle that
  holds no object is illegal with an indeterminate result the LRM lets an
  implementation error on. A handle is raw pointer bits, so the untrapped outcome
  was `inttoptr 0` and an unattributed segfault. Every `HandleDeref` and every
  vtable load now branches on a compare against zero (`emit_null_handle_check`,
  weighted 1:2^20 into a `cold noreturn` block ending in `unreachable`) into
  `rt_null_handle`, which prints a compiler-style diagnostic — file position,
  source line, caret under the handle expression — and exits nonzero. The
  frontend records the object expression's span on the deref so `obj.prop`
  underlines `obj`; the deref rooted directly at `ClassNew` (a `new`'s own
  property default-initializer stores) skips the guard. A non-virtual method
  called on a null handle is caught at the first property its body touches; one
  that touches none runs, which the LRM's "indeterminate" permits.
- [x] **Forward typedef classes** (`typedef class C;`, LRM 6.18 / 8.27) — the
  class-resolution half needed no work: `lower_class_type`'s intern-first
  cache is keyed on the `ClassType` symbol pointer, which slang resolves a
  forward declaration and its later full definition to identically, so mutual
  cross-class handle references (2-way and N-way cycles) fall out for free.
  The one real gap was `lower_members`, which walked module/generate-block
  members through an exhaustive switch that had no case for
  `SymbolKind::ForwardingTypedef` (the forward declaration itself, as
  opposed to the class it names) and loud-failed on it — the declaration is
  legal only inside a module/generate scope's member list, so every forward
  typedef anywhere but the top compilation-unit scope hit this. Fixed by
  adding it to `lower_members`' inert-member case list (it carries no
  type/value/statement content, same as `TypeAlias`). Covered (iverilog has no
  class support): 2-way and 3-way mutual handle references (fields, method
  params, method calls through the handle, null comparison), a forward decl
  after its definition, duplicate forward decls in one scope, and the
  LRM-mandated error for a forward decl that never resolves. See
  tests/class_test.cpp.
- [x] **Single inheritance** (`extends`, `super`, non-virtual override, LRM 8.13)
  — Phase 2. Prefix layout: base properties lay first in the object body
  (`element` records the base Ref, `lower_class_type` copies the base's flattened
  field table as the prefix), so an inherited property keeps its base-relative
  field index and an upcast is the same pointer. A non-virtual method binds
  statically (slang resolves the SubroutineSymbol by the handle's static type;
  the per-symbol method table needs no change). Base construction (LRM 8.15) runs
  at each constructor's entry from `getBaseConstructorCall()` — implicit no-arg,
  `extends B(args)` (class-scope args), or an explicit `super.new(args)` (the body
  statement is a no-op marker); `emit_base_construction` / `emit_ctor_call` recurse
  through a constructor-less intermediate class. Whole-chain property defaults apply
  base-first at allocation. `super.method()` reuses the ordinary method-call path.
  A **virtual** method is guarded loud (Phase 3 needs a vtable); the built-in
  `randomize` virtual is skipped via `MethodFlags::BuiltIn` (iverilog cannot
  resolve inherited methods from a derived method body). See
  docs/design/classes.md and tests/class_test.cpp.
- [x] **Access control & statics** (Phase 2) — `static` properties are one slot
  shared across all instances (and inherited by derived classes), persistent
  module-signal storage keyed by the property symbol (`declare_static_prop`), not
  part of the per-object body; initialized once at time zero in the PreInit
  process (LRM 8.9.2). Reachable as `obj.p`, `C::p`, or a bare `p` in a method
  (all routed to the shared signal in `value_lvalue` / `lower_member_lvalue`).
  `static` methods reuse the free-subroutine path (no `this`). `local` /
  `protected` / `const` carry no runtime obligation (slang enforces at
  elaboration). An aggregate / container initializer (unpacked struct or array,
  queue, dynamic array, associative array) routes through `emit_struct_store` into
  the shared slot, the same element-wise path a module-level declaration
  initializer takes; a `string` initializer stores whole, bypassing `coerce`
  (whose `Resize` rejects a string value). Lowered with `fn_ctx_` / `ctor_this_`
  cleared, since LRM 8.11 scopes `this` to non-static methods only. Still missing:
  a static property of aggregate type with FIELD-level defaults and no initializer
  of its own reads the power-on default instead (`declare_static_prop` returns early
  on a null initializer) — silent, not guarded.
- [ ] **Cross-module class type identity** — the class type cache is per-Lowerer,
  so a `$unit`/package-scope class referenced by two modules interns two distinct
  (nominal) Refs; V1 exercises classes only within one module. Route
  `lower_class_type` through `globals_` like subroutines when this matters.

### Assertions (immediate)

- [ ] **Immediate `cover` count + report** (LRM 16.3 / 16.5) — the observable
  pass-action-on-true behavior is done; the per-point coverage count and
  end-of-sim report belong with a dedicated coverage engine.
- [~] **Deferred immediate assertions** (`assert #0`, `assert final`, LRM 16.4) —
  Phase 1 done: observed + final, per-process generation flush (glitch
  suppression), args sampled at execution, Observed/Postponed
  maturation (`rt_defer_report` / `rt_defer_flush_*`).
  Multi-instance is supported (each instance flattens to its own coroutine +
  generation global). Remaining (each guarded loud): user task / void-function
  action blocks (generic deferred-call ABI), `$display`/`$write`/`$fatal`
  actions, deferred assertions inside a function/task (generation threaded
  through the call ABI), and `disable`-based flush points (16.4.4). The
  observed/final distinction now has the Reactive region it was waiting on
  (`@clotho.reactive`), but a deferred report still matures through the deferred
  queue instead of scheduling an action block into Reactive, so the LRM's
  Reactive-region double-report is still not reproduced.
- [x] **Immediate assertions** — `assert` / `assume` / `cover` (LRM 16.3) with
  pass/fail action blocks; branch selection verified vs iverilog.
- [x] **Severity system tasks** — `$error`, `$fatal`, `$warning`, `$info` (LRM 20.10).
- [x] **`$signed` / `$unsigned` / `$clog2`**.
- [x] **Utility system functions** — `$bits`, `$countones`, `$countbits`,
  `$onehot`/`$onehot0`, `$isunknown`, `$typename`, `$isunbounded` (LRM 20.6.1).
  Sub-gaps:
  - [ ] `$countbits` with a **non-constant** control bit.
  - [ ] `$typename` of an enum / struct / union / class / interface / dynamic
    type (lands with each type's own support).
  - [ ] `$isunbounded` of a **hierarchical** reference (slang refuses to
    constant-evaluate it, and Clotho has no cross-instance constant context).

---

## M4 — Industrial SystemVerilog + UVM

### Full object orientation

- [~] **Polymorphism** — `virtual` **functions** with dynamic dispatch (LRM 8.20)
  landed. Every class carries a descriptor pointer at object-body field 0
  (`class_body_ty`); each class registers a per-concrete-type dispatch
  table (`Module::set_class_desc`) whose slots are keyed by method NAME per hierarchy
  (base virtuals 0..k-1, an override — implicit or of a pure prototype — reuses the
  base slot of the same name, a new virtual appends, `compute_vtable` in Lower.cpp).
  A virtual call through a handle lowers to the new `CallVirtual` IR op (loads the
  callee from the object's vtable, `lower_virtual_call_inst`); a `super.method()`
  binds statically (detected syntactically). **Abstract classes** (`virtual class`,
  LRM 8.21) with concrete virtual methods lower normally; a direct `new` is
  rejected. **Pure virtual** methods (LRM 8.21) dispatch on the concrete type
  through an abstract base handle, with and without arguments, and via an internal
  call — the name-keyed slot matches the concrete override (which slang does not
  link to the prototype via `getOverride()`), and an abstract class's un-overridden
  slot is filled by a stub whose signature is rebuilt from the `MethodPrototypeSymbol`
  (slang's associated subroutine is degenerate: null `thisVar` / `returnValVar`,
  empty `getArguments()`); the stub is never executed. A virtual method returning a
  **non-scalar** (string / struct / queue / dynamic array) dispatches like any
  virtual call — the call-temp scanners recognize `CallVirtual`. Covered:
  override, inherited body, internal call, `super`, derived-introduced slot,
  abstract + pure with/without args, implicit override, multi-level abstract.
  A **virtual task** dispatches on the concrete type over the task (coroutine) ABI
  — the vtable slot holds the task ramp pointer, `TaskCallVirtual` loads it and
  drives the coroutine exactly like a direct `TaskCall`; time-consuming (`#delay`)
  virtual tasks and non-virtual tasks calling virtual ones internally work. The
  `CallVirtual`/`TaskCallVirtual` signature source is `function_id(sub)` (concrete)
  or the prototype stub (pure), NOT the receiver's vtable — so it is available even
  while the receiver class's own vtable is mid-computation (a virtual method whose
  body virtual-calls a sibling). A **pure virtual task** dispatches over the task
  (coroutine) ABI too: slang types a task prototype's return as `void`, so the stub's
  existing void body is already right and the only task-specific part is the `is_task`
  flag on its `Function` record, which is what selects the
  `(state, rng, wheel, <formals>, record)` signature a `TaskCallVirtual` drives the
  override's ramp through. A **pure virtual with a non-scalar return** reuses
  `build_function`'s fall-through return shape, so the stub's signature is the one
  every override was built with: an unpacked struct / fixed array and a container
  (queue, dynamic array, assoc) `Ret` their frame lvalue for the backend's trailing
  return out-param, while a string handle and a class handle `Ret` a loaded value. The
  stub stays never-executed (an abstract class is never `new`'d, so its vtable is
  never materialized). Covered: a pure virtual task with an argument,
  time-consuming and internally enabled; string, unpacked-struct and queue
  returns.
  An **`extern virtual`** method (LRM 8.24) dispatches too: the class member is the
  prototype and the body sits outside the class, so it is not a member
  `SubroutineSymbol` and a table filled only from members left the slot empty —
  reported as an internal error, since the un-overridden slot then found no pure
  prototype to stub. `compute_vtable` now also walks each class's
  `MethodPrototypeSymbol`s and fills the slot from `getSubroutine()`, the
  out-of-block implementation; a prototype with none is pure and still gets the
  stub. Covered: an abstract root whose extern body is the slot's implementation, a
  derived class overriding one extern with another and inheriting the other, a leaf
  overriding an inherited extern with an inline body, functions and tasks.
  **Deferred, guarded loud:** a pure virtual returning a by-value aggregate that owns
  heap (a string member) — the same per-leaf-deep-copy gap `build_function` guards for
  a concrete body, which is why this bullet stays `[~]`.
- [x] **Parameterized classes** (`class C #(...)`, LRM 8.25) — type and value
  parameters, param-sized members, methods over the type parameter, inheritance
  from a specialization (`extends base#(5)`), per-specialization static members,
  and a value parameter read via an instance (`obj.param`, LRM 8.5) or scope
  resolution (`C#(N)::param`). slang monomorphizes each `C#(args)` into a distinct,
  fully parameter-substituted `ClassType` (`genericClass` points to the generic
  definition), so each specialization lowers as an independent nominal class —
  distinct members, own-FunctionId methods, own static signals, keyed per
  `ClassType`. Lowering needed only the guard lifted plus folding an `obj.param`
  member to the specialization constant. (Cross-module
  specialization identity inherits the existing per-Lowerer class-cache limitation
  below.)
- [ ] **Interface classes** — `interface class`, `implements`, multiple.
- [~] **Class scope & nesting** — out-of-block method bodies (`function C::f();`,
  LRM 8.24) work, in a module or a package scope, for functions, tasks, `static`
  methods, `new`, and `virtual` (see Polymorphism above). Remaining: nested classes
  and `::` on parameterized specializations.
- [x] **`$cast`** (LRM 6.24.2 / 8.16) — all destinations. A numeric `$cast`
  converts and always succeeds (returns 1); an enum `$cast` succeeds only if the
  source is a legal member value (runtime `===` membership over `enum_members`);
  a class-handle `$cast` is the dynamic down-cast. Every class carries a
  descriptor pointer at object-body field 0 whose **ancestor display** (Cohen's
  display, `display[depth] == id`, one load and one compare, ids unique but
  unordered so nothing needs a whole-design numbering pass) answers the test. Only
  LRM 8.16 case 2 reaches run time: a cast to the source's own class or an
  ancestor of it, and a literal `null` source, always succeed; two unrelated class
  types always fail. A source that is null at run time succeeds and assigns null
  (8.16 is ambiguous; the reading is recorded at `lower_cast` and in
  docs/design/classes.md). The function form returns the 1/0; the task form
  reports a run-time error on failure, for an enum destination as well. All of it
  dispatches through `lower_cast`. Deferred: a `chandle` destination (guarded
  loud — a chandle carries no type identity), and a destination or source that is
  an **interface class** (`implements` is itself unimplemented; the descriptor
  needs an implemented-interface set beside the display).

### Constrained randomization

- [ ] **Constraint blocks** — relational, `inside`, implication `->`, `if/else`,
  `foreach`, `solve … before`, `dist`, `soft`, uniqueness `unique{}`. Guarded loud,
  each by its own message: a `constraint` block in a class, an inline `with { … }`
  on either `randomize` form, and `constraint_mode`. The generated `$randomize` body
  is shaped around the seam a solver drops into
  (docs/design/constrained-randomization.md §7).
- [x] **`rand` / `randc` and `randomize()`, unconstrained** (LRM 18.3/18.4/18.6/18.8/
  18.11/18.12/18.14.2) — a class carrying a `rand` property grows a rand block of
  ordinary fields (its own xoshiro256++ state, a 64-bit rand_mode mask, a 256-bit
  cycle bitmap per `randc`), and two reserved vtable slots alongside `kCopySlot` hold
  per-class synthesized bodies: `$randomize(this, mask)` and
  `$randops(this, op, arg)`. The mask is a filter the body intersects with the
  object's own mode word, so one body serves `randomize()`, `randomize(x, y)`,
  `randomize(null)` and the recursion into a `rand` handle. Draws come from the
  OBJECT's generator, seeded at `new` from one `$urandom` draw of the creating thread
  (LRM 18.14.2) — which is also why no analysis pass changed: the seeding draw is an
  ordinary `Urandom` instruction, so a process that constructs a randomizable object
  already owns its frame state and leaves the thunk path. An unconstrained field is
  uniform over its full range, an enum over its distinct labels, a 4-state field
  never x or z; a `rand` handle is recursed into, never replaced, and a null one is
  skipped. `pre_randomize` / `post_randomize`, per-property and object-level
  `rand_mode`, `srandom`, `std::randomize` (which draws from the calling thread — no
  object) and `randc` all land with it. Three IR tags — `ObjRandom`, `ObjRandSeed`,
  `RandcNext` — all taking lvalue operands so the backend needs no layout knowledge,
  plus one runtime helper, `rt_randc_next`. Guarded loud: `randc` wider than 8 bits
  (LRM 18.6 allows the cap), `rand real`, `rand` dynamic arrays / queues /
  associative arrays / unpacked structs, more than 64 rand properties in one chain,
  `randomize(x)` naming a non-rand property, and `rand_mode` on a subelement. Design:
  docs/design/constrained-randomization.md.
- [x] **`$urandom` / `$urandom_range`** (LRM 18.13) — per-process xoshiro256++
  RNG in the coroutine frame (thread-stable by process index), inlined draw +
  Lemire range map. Stream intentionally not iverilog-identical (LRM
  §18.13 leaves the algorithm unspecified). Legal anywhere: a function or task
  runs in its caller's thread, so the state pointer is an implicit parameter of
  the call ABI (`declare_function`) and a draw inside a subroutine continues the
  calling process's stream.
- [x] **`randcase`** (LRM 18.16) — weighted random branch selection: each weight
  evaluated once, summed as unsigned at the widest weight's precision, one
  `$urandom_range` draw picking the first branch whose cumulative weight passes
  it. A zero-weight branch never runs and an all-zero sum runs none (the LRM's
  warning is optional). The fixed-precision sum wraps per the literal LRM
  reading, where verilator widens instead. Guarded loud: a
  weight wider than 32 bits (needs the LRM's several-draw stitching).
- [x] **Random stability across threads** (LRM 18.14.1) — a body that can outlive
  the site that ramped it owns its generator instead of sharing the pointer: a
  fork child, and the deferred `$strobe` / `$monitor` / action bodies. Each is
  seeded at its ramp from one draw of the ramping thread's state, so a child's
  stream is reproducible and independent of its siblings'. A DPI export thunk and
  the `$monitor` ramp in `main` have no calling thread at all and use a
  module-wide state seeded at startup.
- [x] **`srandom` / `get_randstate` / `set_randstate`** (LRM 9.7, 18.14) — the
  `process` methods: the thread's generator state rides its control block, so all three
  reach a thread through a handle. `srandom` is `$urandom(seed)`'s splitmix64
  expansion; the state renders as 64 hex digits and round-trips. A design that uses any
  of the three gives every thread its OWN generator (the target of `p.srandom()` is not
  statically known), gated so a `$urandom`-only design keeps the ownership
  `build_rng_fns` computes. A foreign string is a located fatal, LRM 18.14.4 leaving it
  undefined.
- [ ] **Random stability & seeding, remaining** — the OBJECT-side randstate pair
  (`obj.get_randstate()` / `obj.set_randstate()`), which shares the two `clotho.str`
  helpers the `process` trio above now provides and stays guarded loud until it uses
  them. The per-object RNG for class `randomize` and `obj.srandom()` landed with
  randomization; the thread-side trio landed with `process`.

### Concurrency & synchronization

- [x] **`wait fork`, `disable fork`** (LRM 9.6.1 / 9.6.3) — both landed with the
  `process` control blocks, which is where the live-descendant set they needed came
  from: `disable fork` is `kill` over the caller's subtree with the caller spared, and
  `wait fork` is the one-pointer test `first_child == null`, exact because a completing
  thread re-parents its live children (so a grandchild of a finished child still
  counts, as 9.6.3 requires). Both stay refused inside an assertion action block,
  which runs on the MONITOR's thread rather than one of its own.
  Design: docs/design/process-class.md §8.
- [x] **`std::semaphore` / `std::mailbox #(T)`** (LRM 15.3 / 15.4) — both are
  ordinary class objects whose synthetic body carries the resource and its wait
  queues: a signed 64-bit key count for `semaphore` (15.3.1 lets the count grow past
  the constructor's), and for `mailbox` an UNBOUNDED queue of T plus three queues of
  waiters (receive, send, peek). The bound is enforced in emitted IR, NOT by the
  queue type, whose overflow silently drops the far end instead of blocking. So the
  whole payload surface — int, string, class handle, 96-bit vector, unpacked struct
  with a string leaf, nested queue — comes from the container element descriptor with
  no mailbox-specific code, as does GC tracing of a handle payload.

  New surface is two IR tags: `SyncWait` (parks on a queue, -> 1 when it resumed as
  that queue's head) and `SyncCtl` (`Leave` / `WakeHead` / `WakeAll`); everything
  else expands in the frontend into ordinary IR over the body. No runtime symbol, no
  pool, no ABI change, no `DesignUses` gate. The waiter node is frame-resident and
  STAYS linked across an unsatisfied retest — that is what preserves LRM 15.3.3 /
  15.4.5 arrival order, and it is why the abort test lives inside `SyncWait`: a retry
  loop never reaches the end of its instruction, the same shape that made a killed
  `await` unkillable. A thread killed while blocked in `get` / `put` / `peek`
  terminates, and a suspended one does not consume.

  Head-of-line admission per 15.3.3 and Accellera's Interpretation #1: a blocked head
  blocks a later smaller request, where Verilator 5.050 serves the later one and then
  deadlocks the head — so those cases are pinned from the LRM text. `peek` is exempt
  (it consumes nothing) and a `put` wakes every peeker before the receive head, which
  is the only order that makes 15.4.7's broad unblock observable. `try_*` stay
  availability-only (15.3.4, 15.4.4/6/8).

  Guarded loud, each with a TODO: the **typeless mailbox** (15.4.9's `dynamic_type`,
  legal SV that slang type-checks) — it needs a dynamically typed element plus a
  run-time type identity for 15.4.5's check and 15.4.6's negative return;
  `mailbox #(event)` (an element's construction mints an event object the copy then
  overwrites, and event objects are never freed); deriving from either built-in
  class; and a **negative bound**, which 15.4.1 calls illegal and which reports at
  run time since the bound need not be constant. 20 cases in `tests/sync_test.cpp`.
  `std::weak_reference` (LRM 8.30) keeps the std-package guard alone.
  Design: docs/design/mailbox-and-semaphore.md.
- [x] **`process` class** (LRM 9.7) — all nine methods: `self`, `status`, `kill`,
  `await`, `suspend`, `resume`, `srandom`, `get_randstate`, `set_randstate`. A
  thread's control block is native, untraced storage in two tiers -- a static slot per
  process (no allocation, a constant address, the starting state in the initializer)
  and a pooled record per fork child, the only unbounded tier -- and a `process`
  handle is an ordinary class handle onto it, created lazily at the first `self()` and
  memoized so identity is a pointer compare. Identity travels the call ABI beside
  `rng`, because a task or function is not a thread.

  `kill` is the landed `disable` abort per thread rather than per static target: mark
  the subtree, force-wake each exactly once through the shared claim-or-flag protocol,
  and let every activation leave through its ordinary exit -- so a thread parked behind
  a time-consuming task call unwinds its whole frame chain, which is the case remote
  `disable` still refuses. `suspend`/`resume` desensitise nothing (the wheel cannot
  cancel, a static bucket cannot compact): a suspended thread replays the wake it
  already received, which is what makes `resume` continue it in the current time step.

  Everything is behind `DesignUses::process`, verified byte-identical `--emit llvm` on
  every design that does not name it. `std::weak_reference` keeps the std-package
  guard, now keyed by class name. 27 cases in `tests/process_test.cpp`, clean under
  ASan/UBSan/LSan. Design: docs/design/process-class.md.
- [~] **Clocking blocks** (LRM 14) — `clocking`/`endclocking` elaborates in a
  module, an interface (so a modport-scoped block comes free) or a program, and
  each block becomes one `ir::Activation::Observed` process that samples every
  input clockvar and then toggles the block's event, in that order (LRM 14.13).
  Putting it in Observed is what makes `@(cb)`, `@(cb.sig)` and
  `@(posedge cb.sig)` ordinary waits: an Observed store wakes an Active process,
  and the drain re-settles, so a waiter runs in the first Active region after
  Observed exactly as LRM 14.10 says. A clockvar is its own storage registered
  under the `ClockVarSymbol` in `sigmap_`, NOT an alias of the sampled signal, so
  expression lowering is untouched. `input #1step` (the LRM 14.4 default) reads the
  Preponed shadow the assertion sampler already maintains — one copy per variable,
  shared (`sampled_shadow_of`) — and an explicit `input #0` reads live in Observed.
  Synchronous drives (LRM 14.16) evaluate their RHS immediately into per-clockvar
  pending state and are applied by a per-block `ir::Activation::ReNba` process
  (new region, `emit_renba_run`), which is where 14.16 puts the value change and
  the only point late enough to see a drive issued by a `@(cb)` waiter or by a
  program's Reactive code. `##n` / `##0` (LRM 14.11) wait on the default
  clocking's event, with an integral count only — 14.11 wants "a non-negative
  integer value" and slang's `requireIntegral` rejects a real one, so lowering
  keeps it as an invariant (`internal: a real cycle delay count reached lowering`)
  rather than a gap, unlike a repeat count, which 12.7.2 admits as real.
  `default clocking` / `global clocking` / `$global_clock`
  supply an assertion's leading clocking event (LRM 16.16(a), 14.14). See
  `tests/clocking_test.cpp` and docs/design/clocking-blocks.md.

  Refused loudly, each with a TODO: any **input skew** other than `#1step`/`#0`
  and any **output skew** other than `#0` (a nonzero skew samples or drives at a
  time the edge does not name, so it needs a per-clockvar history keyed by time,
  and an edge-based skew is a second event per block); a **net-typed output**
  (LRM 14.16 wants a `'z`-initialized `(strong1, strong0)` driver joined to the
  net's resolution, not a direct write); `cb.sig <= ##n expr` (one pending slot
  per clockvar cannot hold two drives maturing in different cycles, which 14.16.2
  requires — it needs a ring indexed by cycle offset); a **bit-select or slice**
  drive target (the pending value is one whole-signal store); a **clocking item
  bound to an expression** rather than a signal (LRM 14.5 slices and
  concatenations); a non-posedge/negedge or multi-signal **clocking event**; an
  **assertion item declared inside a block** (LRM 14.3 with 16.16(b)'s rules); and
  a **sampled value function in procedural code** (LRM 16.9.3), which needs
  history rings keyed to the default clocking rather than to a monitor.
- [~] **Program blocks** — `program`/`endprogram` elaborate and simulate. A
  program instance goes through the ordinary instance path (own scope,
  variables, functions, non-suspending tasks), and its `initial` procedures run
  in the Reactive region per LRM 24.3 (`ir::Activation::Reactive`, seeded onto
  `@clotho.reactive`). A program initial is a real process, so it may suspend on
  `@`, `wait` or a nonzero `#delay` and resumes back into Reactive — each
  suspend wakes in Active, re-queues onto the reactive ring and suspends again
  (`emit_suspend`), because a waker does not know its sleeper's region. The LRM
  24.6 implicit `$finish` (simulation ends when the last program initial
  completes) is implemented as a countdown of live program initials. The region
  order against an Active-region cascade is covered by
  `tests/sva/diff_program_ref.sv` and `tests/program_test.cpp`.

  Before this, `program` was accepted and silently lowered as a **module** —
  Active-region initials, no implicit finish — a silent miscompile that
  predated the region set. Remaining gaps, all guarded loud: program **ports**
  (24.3 clocking/skew semantics unimplemented), **`$exit`** (24.5, needs program
  termination), **`#0`** inside a program (would resume through Inactive rather
  than Re-Inactive, 4.4.2.7), and a **suspending task** called from a program
  (the re-entry would push the task's handle, not the caller's). `always` in a
  program, nested programs and anonymous-program members are rejected by slang.

### Concurrent assertions (SVA)

- [~] **Sequences & properties** — `assert`/`assume`/`cover property` compile,
  simulate and report: `@(posedge|negedge clk)` written explicitly, inferred
  from the enclosing procedure (LRM 16.14.6), or taken from a clocking block —
  `default clocking` (LRM 16.16(a)), `global clocking` / `@($global_clock)`
  (14.14), or an `@(cb)` that names one (16.14.6(c)(1)) — plus `disable iff`,
  `|->`/`|=>`, `##n`/`##[m:n]`, `strong()`/`weak()`, and arbitrary boolean guards
  (lowered through negation normal form, so `!` never reaches libsva). Sampled
  values come from a Preponed region (`ir::Activation::Preponed`) that shadows
  each assertion variable; the automaton step is emitted as ordinary IR, so there
  is no new opcode. The region mapping is now the one LRM 4.4.2 specifies: the
  monitor runs in Observed (`ir::Activation::Observed`) and its pass/fail action
  block is scheduled into Reactive (`Inst::Tag::ReactivePend`), never run inline.
  Action blocks work. The SEQUENCE layer (LRM 16.9) is COMPLETE: `##n`/`##[m:n]`,
  `or`, `intersect`, `and`, `throughout`, `within`, `first_match`, repetition
  `[*]`/`[=]`/`[->]`, and named `sequence`/`property`/`let` declarations all
  lower, exercised cycle-for-cycle by the fixtures under `tests/sva/`.
  The PROPERTY layer (LRM 16.12) is now PARTLY done:
  implication, a bare sequence property and `strong`/`weak` around one, plus the
  whole BOUNDED group — `nexttime`, `nexttime[n]`, `always [m:n]`,
  `eventually [m:n]`, each with its `s_` form, `#-#`/`#=#`, property-level
  `and`/`or`, and the `if`/`else` and `case` forms (desugared to a disjunction of
  followed-bys, which is the shape that makes an untaken branch contribute
  nothing instead of holding vacuously) — plus the ABORT group: `accept_on`,
  `reject_on`, `sync_accept_on` and `sync_reject_on`, including the abort ABOVE an
  implication, which is the shape every real use writes. Covered by
  `tests/sva/diff_{nexttime,alwaysrange,eventuallyrange,followedby,propif,abort}.sv`,
  plus `diff_strongprop.sv` and `diff_propcase.sv`.
  An abort is scoped to a subtree rather than the whole monitor, so
  `ir::AssertionMonitor` carries an abort table (root, accept-vs-reject,
  sampled-vs-live, resolves-attempt, covers-trigger, state mask) and the emitted
  step has arms for it. Three things worth knowing: an accept DISCHARGES an
  attempt where `disable iff` abandons it with no verdict; the plain forms read
  live values like `disable iff` and carry a one-bit latch so a pulse strictly
  BETWEEN two ticks is still seen; and an abort covering a trigger resolves at
  MATCH time, the cycle the antecedent match ends, which for `|=>` is one cycle
  before the obligation would be seeded. Where two aborts fire on one attempt the
  outermost wins, so the abort list is scanned backwards.
  Remaining (each guarded loud): UNBOUNDED `always`/`eventually` and the `until`
  family (phase 3 — the obligation has no bounded length, so the number of
  attempts pending at once is unbounded); `not`, and with it `implies` and `iff`
  (phase 4 — a complement needs the operand determinised and swaps the weak/strong
  disposition); an abort covering an implication whose ANTECEDENT takes more than
  one cycle (several attempts can be part-way through it when the condition fires,
  each owing its own verdict, and the trigger is their union rather than a
  per-attempt ring, so the count cannot be recovered — one-cycle antecedents are
  exact and lower);
  `disable iff` inside a named property, local variables and sequence match
  items, recursive properties, a repetition with a zero lower bound (it admits
  the empty sequence, which libsva bans), goto/non-consecutive repetition over a
  compound sequence, multiclock, hierarchical or non-packed sampled variables,
  and an action block containing a nonzero delay / event control. A plain nonblocking
  assignment in an action block now works, committing in Re-NBA (LRM 4.4.2.8).
  An `and` in a consequent is refused where an `intersect` is accepted, because
  `and` runs to the later of its two ends and so needs both operands bounded;
  the diagnostic names that. A property operator whose operand raises an
  obligation of its own is refused too — a monitor has one trigger and nowhere to
  put a second — except over a `nexttime[n]` or an `always [m:n]`, where libsva
  moves the shift onto the antecedent instead. See
  docs/design/concurrent-assertions.md and docs/design/sva-property-layer.md.
- [x] **Sampled value functions** — `$rose`, `$fell`, `$stable`, `$changed`,
  `$past`, `$sampled`, including `$past(v, n)` for `n > 1`. `$past` is the
  primitive: a per-monitor history ring shifted once per CLOCK TICK at the end of
  the monitor body, as deep as the largest constant `n` asks for; the others
  desugar onto it. Ring entries are seeded with the variable's time-zero value,
  NOT the type default: before enough ticks have elapsed the only value that has
  actually been sampled is the one at time zero, and seeding the type default
  would manufacture a change that never occurred. Covered by
  `tests/sva/diff_past.sv` and `tests/sva/diff_valuechange.sv`. Still refused,
  each guarded loud: a gating expression or an explicit clocking argument on
  `$past`, an explicit clocking argument on a value change function, a
  non-constant tick count, any of the six
  over an expression rather than a plain variable, and any of them outside an
  assertion expression. In a `disable iff` or asynchronous `accept_on` /
  `reject_on` condition `$sampled` now lowers — LRM 16.9.3 blesses it there and it
  resolves to the Preponed shadow alone, touching no history ring and no
  per-attempt state — while the other five stay refused, because 16.9.3 infers no
  clocking event in that position and 16.12.14 requires an explicit clock argument
  for them, which is the gap above; slang diagnoses the unclocked
  disable-condition spelling itself. See docs/design/concurrent-assertions.md §4.1.
- [~] **Property layer phase 3** — DONE. Unbounded `always p`, `s_eventually p`
  and all four `until` forms lower and simulate, tracked by a new COUNTED mode:
  the obligation is determinised totally and each of its states carries a
  `uint64_t` count of the attempts sitting there, so a verdict says how many
  attempts it belongs to. `libsva/test/merge.zig` (`zig build merge`) is why it
  is shaped that way and not as the single merged vector the design doc first
  proposed: over every trace of 6 and 8 cycles a merged vector is unsound (a
  two-cycle counterexample on a bounded `a ##1 b`), `first_match`'s
  determinisation does not rescue it because it is partial and a dead attempt has
  no state to be seen in, and only the counted total construction is exact on
  cycle sets, counts and end-of-trace pendency at once. `tests/sva/diff_until.sv`,
  `diff_alwaysinf.sv` and `diff_seventually.sv` carry hand-derived expectations
  that clotho now reproduces exactly, including `diff_seventually.sv`'s six cover
  hits where a merged vector would have scored two. The goldens in those files
  are still `TO BE FILLED`, so the expectations there remain hand-derived.

  Two refusals remain from this work, both loud and both narrower than the old
  unbounded refusal they replace. A property mixing disciplines —
  `always (a |=> b)` and friends — is `SVA_ERROR_MODE_MIX`, by decision: tracking
  mode is monitor-level. And the operand of an unbounded `always`, or the LEFT
  operand of an `until`, must match in a single cycle, because both positions are
  universal over cycles and "p holds at every cycle of the window" is a statement
  about overlapping attempts of p that one counter cannot carry. The fix is a
  second subset construction over the determinised operand, where a dead
  sub-attempt becomes a visible state; cost is exponential in the operand and
  capped like every other blow-up. Multi-cycle operands in the EXISTENTIAL
  positions — `s_eventually p`, an `until`'s right operand — work today.
- [~] **Property layer phase 4** — `not` is DONE; `implies` and `iff` are
  structurally limited. The complement is the total determinisation with two
  masks exchanged: `last` becomes the dead (empty-subset) states, and the states
  whose subset meets the operand's exits get a `never` guard, so nothing in
  `Monitor` or in a backend knows a `not` was involved and `sva_report` needs no
  new field. A complement does NOT force counted mode — the operand-matched
  states are deleted, so the ordinary emptiness reading is restored and the ring
  holds it. `not pending == pending` is what cost real work: ordinary
  co-reachability pruning would resolve a pending verdict at the cycle the
  outcome became inevitable, so `prune` gained an exemption mask, pinned by
  `not (a ##1 (b or !b))` and by `not not p` agreeing with `p`.
  `tests/sva/diff_not.sv` reproduces an independently hand-derived LRM
  expectation exactly: 28 reports across seven ticks, four of them at end of
  trace, and seven cover hits.

  What is limited, and it is not conservatism: `implies` reduces to
  `(not p) or q`, `not` inverts the strength of the operand it complements, and a
  monitor carries ONE end-of-trace disposition — so `implies` lowers only when
  its operand strengths DIFFER, and `iff` never lowers, since each direction
  forces its operands opposite and the outer conjunction always sees a mixed
  pair. Weak-implies-weak, which is the default spelling, is therefore refused.
  Not conservative: with both operands weak, a trace where `q` definitively
  failed while `p` is pending ends with the implication FAILING, and a trace
  where both are pending ends with it HOLDING — one automaton, one strength bit,
  two dispositions.
- [ ] **Retire `MIXED_STRENGTH`** — the lift that makes `implies` and `iff`
  usable, and the one thing standing between phase 4 and complete LRM 16.12. A
  per-state end-of-trace disposition, OR-combined over the live vector and folded
  conjunctively at product-construction time, replaces the single strength bit.
  It changes what `sva_monitor_strength` means, so it is an ABI decision and not
  a refactor. Three separate refusals point at it.
- [ ] **Determinise the trigger** — what would restore an unbounded antecedent
  (`b[=2] |=> c`, `(a ##[1:$] b) |-> c`), refused since the trigger began
  counting attempts. The bound is not about counter width: the
  trigger is a nondeterministic Glushkov automaton, so one attempt is a SET of
  live states, "how many attempts sit in this state" is not well formed, and
  identity is needed because two threads of one attempt can reach exits together
  where LRM 16.9 has ONE match — set union is idempotent, addition is not.
  Determinised, an attempt is a single state and a plain count is exact with no
  bound at all. Reuses `Glushkov.determinise` but a trigger must keep running
  past a match, needs its own discipline flag and tables in the C ABI, and wants
  the subset blow-up capped under `state_limit` like `first_match`.
- [ ] **`expect`** — blocked behind giving an assertion action block a real
  coroutine so it can suspend and resume in the Reactive region. That one
  scheduler change is what lifted most of the action-block whitelist (a delay, an
  event control, `wait` and a `fork` are all allowed now); a user task call is the
  entry still refused, since the body is not traced for what it may suspend on.
  Not an SVA problem; see concurrent-assertions.md §9.3's whitelist.
- [ ] **`.matched`** — needs multiclock (LRM 16.13.6). `.triggered` on a plain
  same-clock sequence does not, and is the near-term half.
- [x] **Assertion control** — `$asserton`/`$assertoff`/`$assertkill`, the four
  pass/fail action tasks, `$assertnonvacuouson`/`$assertvacuousoff` and the
  constant-argument subset of `$assertcontrol` (LRM 20.12), over concurrent
  assertions. Per-assertion state in negative logic, addressed by scope or by
  label with `$dumpvars` `levels` counting; `off` gates the seed alone so an
  attempt in flight still reports, `kill` rides the `disable iff` reset, and the
  action filters gate the pend without touching the automaton. Still open:
  non-constant arguments (refused, and they need a run-time dispatch over every
  assertion in scope), control over immediate/deferred/`expect` assertions
  (refused when the design has one, since they lower inline with no state to
  gate), and `unique`/`priority` violation control.

**Deliberately deferred, recorded so it is not lost.** Each is a separate
project with its own algorithm, not tail cleanup on the property layer:

- [ ] **Multiclock sequences and properties** (LRM 16.13) — libsva's monitor has
  exactly one clock and the automaton model is "one clock edge is one cycle", so
  this needs a per-letter clock attribute, a rewrite of `advance`/`seed`/`satisfy`
  and of clotho's emitted step, plus a clock-domain partition in the frontend.
  Gates `.matched`, an explicit clocking argument on a sampled value function,
  and `@$global_clock` beside another clock. Roughly 1.5k lines. Its
  `default`/`global clocking` half is no longer blocked: clocking blocks landed
  (LRM 14), and a single `default clocking` / `global clocking` / `@(cb)` clock
  already resolves to an ordinary single-signal event, so what is left here really
  is the per-letter clock attribute and the automaton rewrite.
- [ ] **Local variables and sequence match items** — an attempt is a bitset of
  automaton states and a local variable is per-thread data; the merge that makes
  the bitset work is exactly what destroys the ability to carry a valuation.
  Needs a register automaton (determinise-with-data) in libsva plus per-slot
  storage in clotho's monitor state. The deepest item on the list.
- [ ] **Recursive properties** (LRM 16.12.4) — a fixpoint over the property
  graph, a different construction from the Glushkov lowering, not a tag. Tracked
  again under M5 language corners.

### Functional coverage

- [ ] **`covergroup`** — `coverpoint`, `bins` (explicit, ranges, arrays),
  transition bins, `wildcard`, `illegal_bins`/`ignore_bins`, `cross`.
- [ ] **Coverage methods & options** — `sample()`, `get_coverage()`,
  `get_inst_coverage()`, per-instance options, `option`/`type_option`.

### Foreign interface

- [ ] **DPI import `output`/`inout` string** — the C writes an unbounded
  NUL-terminated `char*` back with no size contract, so there is no safe fixed
  copy-out buffer (frontend `fail`).
- [ ] **DPI export `output`/`inout` string + string return** — returned-`char*`
  lifetime / the same unbounded copy-out (frontend `fail`).
- [ ] **DPI export open-array args** — needs open-array formals in the general SV
  function ABI (a runtime-bounded array parameter), not just the call-site marshal.
- [ ] **DPI open-array: packed-element dynamic-array actual + multi-dimensional** —
  a packed dynarray element needs a runtime-sized marshal scratch (backend guard);
  multi-dim needs `svGetArrElemPtr2/3` + a recursive descriptor. (A queue actual is
  blocked upstream in slang as a type mismatch, not implementable here.)
- [ ] **One c_identifier exported from several scopes** — `svSetScope` picking which
  instance a shared C name reaches. Blocked upstream: slang's `checkDPIMethods` keeps
  one cId->subroutine map and drops the duplicates before `getDPIExports()` is
  visible, so Clotho cannot even see the second scope. Needs walking the instances by
  hand, a `DpiExport` carrying a (scope, FunctionId) list, and a runtime scope-index
  switch in the thunk.
- [ ] **`svScope` handles for non-top instances** — a scope is registered per top, so
  `svGetScopeFromName` cannot name a nested instance. Exports themselves bind
  absolutely at lowering time, so this is only about the scope API's reach.
- [ ] **DPI import/export tasks** — a task can consume time; the native boundary is
  coroutine-shaped (import) / wheel-from-C (export). Functions only today.
- [ ] **`svGetCallerInfo` + the DPI disable API** — not provided; C calling one hits
  an unresolved symbol.
- [x] **DPI-C** — import (scalar/vec/chandle/string, in/out/inout, pure/context, 1-D
  open arrays in/out/inout at any element width, dynamic-array zero-copy actuals) and
  export V1 (C→SV, functions, scalar/chandle/real/vec/string args) landed, with the
  svdpi.h accessors (bit/part-select, one scope per top, open-array). See the M1
  Subroutines entry and docs/design/dpi-c.md.

### UVM enablers

- [~] **Virtual interfaces** (LRM 25.9) — a handle is the 64-bit address of one
  elaborated interface instance's runtime context record (the per-instance
  descriptor module dedup already builds: state base, NBA shadow base, sched base,
  dump-var base, wait-bucket bases). A member reached through it is the ordinary
  signal lvalue with that base substituted — the representative's def-local field
  index, the handle's instance — which is what a reused definition's own body
  already emits for itself. slang resolves `vif.sig` against a cached placeholder
  instance body, so the member symbol is matched to a real instance by
  `InstanceBodySymbol::hasSameType` (`VifDefs`), and that representative supplies
  every def-local constant. Declaration, `null`, `==` / `!=`, assignment from an
  instance / another handle / `null`, modport-qualified and parameterized handles,
  handles as class properties, constructor and task arguments and array elements,
  member reads and blocking writes with correct wake and VCD on the bound instance,
  interface events triggered through a handle, and `@(edge vif.sig)` /
  `wait(vif.x)` — which park a frame-resident node on an intrusive waiter list in
  the definition's state, the event-object list reused, so there is no bucket
  identity or capacity to know at compile time. Signals of an interface a handle
  can name keep the canonical type-derived shape, the `ref`-actual rule.
  **Nonblocking writes** (`vif.d <= x`) move the *member* — every writer of it,
  handle or direct — off the per-signal block shadow, which two handles naming twin
  instances would collide in, onto the deferred-record path, whose record carries
  the destination pointer resolved at statement-execution time plus the instance
  context the replay wakes against. A **clocking block** through a handle (25.9.1 /
  25.9.2) reads that instance's sampled storage and drives that instance's
  `drive_pend` pair, so its own Re-NBA process performs the drive and 14.16.2's
  last-write-wins falls out per instance. An **interface subroutine** through a
  handle is lowered once for the definition with the handle as formal 0
  (`Function::takes_iface_ctx`), installed body-wide by the backend, so one body
  serves every instance; a bare sibling call passes the context on. That body-wide
  install is why `CtxOverride` now carries an anchor and `def_relative_for` gates on
  it — a hierarchical reference out of the interface has a different `ctx_ty`. An
  event control on an **expression** over members is desugared into the sample /
  park / re-evaluate shape `wait` uses, never a continuous net (which would freeze
  onto whichever instance was bound at time zero); an `edge` / `iff` qualifier on
  such an expression stays guarded loud. A member of an interface the design never
  instantiates is an unconditional run-time fatal with a
  `virtual-interface-never-bound` warning up front, not a refusal. A member in a
  **generate block** of the interface resolves too — a generate block is not a
  separate instance, and the representative is found by ordinal path rather than by
  name. An `event` member is waited on through its own object, the mechanism its
  trigger fires. An NBA inside a subroutine reached through a handle takes the
  record path like any other handle write. The LRM 25.9 `SBusTransactor` example
  runs verbatim. Remaining, guarded loud:
  - [ ] A member of an interface **instantiated inside** the interface a handle
    names (`vif.i.d`) — a separate instance with a separate context record; needs
    the record to carry its children's. A handle taken directly to the inner
    instance works.
  - [ ] `virtual` handles in a **queue / dynamic / associative array** — the
    generic container-of-handles gap below, shared with class handles.

  Design: docs/design/interfaces.md.
- [~] **String formatting** — `%p` (LRM 21.2.1.6). The four structural `shall`
  cases the clause names beyond unpacked arrays/structs now print correctly
  instead of falling through to a bare decimal: a tagged union as `'{tag:value}`
  (`'{Valid:42}`, void member `'{Invalid}`), a plain union as its first declared
  member (`'{a:-85}`), a packed struct with named elements (`'{a:-85, b:42}`),
  and an enum as its enumeration name with a base-type fallback (`GREEN`, else
  `3`) — nested leaves included, via the slang `Type` threaded alongside the
  lowered `ir::Type` through the `%p` expansion. Leaves print in **decimal** at
  their own signedness, the clause's "as they would unformatted" (21.2.1.1's
  decimal default) — they used to be `'h<hex>`, copied from verilator, which is a
  `shall` violation. A real leaf prints `%g`, a string (variable or literal)
  quoted; both used to segfault or print the packed literal's decimal value.
  Also fixed: `%%p` in a format string
  corrupted the output (the splice did not skip `%%` escapes, so it substituted
  inside the literal and left the real specifier for the runtime formatter to abort
  on), and a `%p` operand was lowered twice (a call operand ran twice). Remaining:
  - [ ] `%p` of a class / chandle / event / virtual-interface handle — rejected
  - [ ] `%0p` — a field width / flags on the whole pattern is rejected loud.
  - [ ] `%p` of a queue / dynamic array of tagged unions — rejected loud.
  - [ ] `%p` of an associative-array leaf inside an aggregate — rejected loud.
- [ ] **Containers of handles** — queues/dynamic/associative arrays of class
  objects, and class objects holding them.

---

## M5 — Full LRM (IEEE 1800-2023)

### Net & primitive completeness

- [ ] **User-defined nettypes** (`nettype`) and resolution functions,
  `interconnect` nets.
- [ ] **Charge storage** — `trireg` with capacitance strengths, charge decay.
- [ ] **Sequential UDPs** — full edge-sensitive tables including `*`/`r`/`f` notation.

### Timing & back-annotation

- [ ] **Full `specify`** — edge-sensitive paths, state-dependent paths, conditional
  paths, `ifnone`, polarity, full timing-check set (`$recovery`, `$removal`,
  `$recrem`, `$skew`, `$timeskew`, `$fullskew`, `$nochange`, `$setuphold` with
  notifiers).
- [ ] **SDF annotation** (`$sdf_annotate`) and pulse-style controls.
- [ ] **Hierarchical / fractional timeunit & precision** — full `timeunit`/
  `timeprecision` per scope and precision-correct fractional-delay rounding.
- [x] **`$printtimescale` with a scope argument** (LRM §20.4 / §21.3.1.2) — the
  optional argument names an instance anywhere in the design (upward, downward or
  sideways), `$unit`, or `$root`; the target's timescale and hierarchical path are
  compile-time constants, so the task still folds to a literal `$display` with no
  IR or runtime support. A scope with no timescale now reports Clotho's unitless
  base (`1s / 1s`, byte-identical to iverilog) instead of failing.

### Configuration & checkers

- [ ] **Library map / `config`** — `library`/`config`/`design`/`cell`/`instance`
  binding, `liblist`/`use`.
- [ ] **Checkers** (`checker`/`endchecker`) — assertions, free variables,
  instantiation.
- [ ] **`randsequence`** — grammar-based stimulus generation.

### Language corners

- [x] **Pattern matching** (LRM 12.6) — Phase 2 of tagged unions
  (docs/design/tagged-unions.md). `case (x) matches` (`StatementKind::PatternCase`
  → first-match if/else chain), `if (x matches pat)`, and `x matches pat ? a : b`,
  all with `&&&` filters, over all five pattern kinds (Wildcard / Variable /
  Constant / Tagged / Structure). `lower_pattern` returns a 1-bit predicate and
  binds `PatternVarSymbol`s (declare_storage + store, the foreach-iterator
  mechanism); sub-values are extracted from the flat subject by shift/resize.
  The `?:` form single-evaluates its condition (11.4.11) across the branchless
  and lazy select paths. unique/priority/unique0 pattern case reports the same
  no-match / overlap violations as a plain case (LRM 12.5.3 wording).
  casez/casex `matches` (LRM 12.6.1) wildcard the z/x label bits over
  tag + member compares (pattern_cmp threads CaseEq/CasezEq/CasexEq through
  lower_pattern) — which is what 12.6.1 requires rather than a literal label
  compare. `unique` / `unique0 if (expr matches pattern)` works
  too: it was the third `matches` guard site (the Phase 2 plan named only two, so
  it survived undocumented and kept rejecting with `pattern matching in conditions
  is not supported`, a `&&&` filter failing one line earlier as an unsupported
  multi-condition `if`). Legal SV — LRM 12.4.2 attaches `unique_priority` to
  `if (cond_predicate)` and 12.6.2 makes a pattern a `cond_predicate`; `priority if
  (... matches ...)` already worked. `lower_unique_if` now collects each arm's whole
  condition list and lowers it through `lower_conditions`, the helper the plain and
  priority chains already use, evaluating every arm's predicate once up front —
  which 12.4.2 permits for unique/unique0, since overlap detection has to continue
  past the first true condition. Binding a non-selected arm's pattern variables is
  harmless: each is a pure sub-value extraction, read only inside the arm its own
  predicate guards. Same discipline as `lower_pattern_case_checked`. Follow-ups
  (gated loud): pattern case / structure pattern over an unpacked **aggregate**
  subject — an unpacked *tagged union* is not caught by that guard and does
  work, since it lowers to a flat integral — and Phase 3 non-integral
  unpacked-tagged member payloads.
- [ ] **`let` declarations** (full, including in sequences/checkers).
- [ ] **Recursive properties**. `global clocking` and `$global_clock` landed with
  clocking blocks (LRM 14.14); what remains of them is `@$global_clock` beside
  another clock, which is the multiclock item above.
- [ ] **Full constraint language** — array reduction methods in constraints,
  functions in constraints, `disable soft`, iterative constraints, constraint
  inheritance.
- [ ] **Coverage corners** — real coverpoints, type coverage, cross with `binsof`
  expressions, coverage merge/database semantics.
- [ ] **Attributes** (`(* ... *)`) — parse-through today via slang; expose where
  semantically meaningful.

### Complete system task / function library

- [ ] **Remaining query functions** — `$bits` on all types.
- [ ] **EVCD** — `$dumpports` family.
- [ ] **`$writememh` / `$writememb`**, address-ranged `$readmem*`.
- [ ] **PLA modeling tasks** (`$async$and$array`, `$sync$nand$plane`, …).
- [ ] **Stochastic / queue tasks** (`$q_initialize`, `$q_add`, `$q_remove`,
  `$q_full`, `$q_exam`).
- [x] **Probabilistic distribution functions** (`$dist_uniform`,
  `$dist_normal`, `$dist_exponential`, `$dist_poisson`, `$dist_chi_square`,
  `$dist_t`, `$dist_erlang`) and seeded `$random(seed)` — the IEEE reference
  algorithm (byte-identical to iverilog), inout-seed
  writeback shared with `$random(seed)`.
- [x] **`$system`** (LRM §20.17.1) — shell command execution via C `system()`,
  task and function forms. Refused by default; gated behind an explicit
  `--allow-system=const|full` operator opt-in with a compile-time gate plus a
  runtime backstop (a security-motivated deviation).
- [ ] **`$random` family completeness** and command/severity tasks
  (`$exit` semantics, `$stop` levels at `Lower.cpp:1021`).
- [x] **Conversion functions** — `$itor`, `$rtoi`, `$bitstoreal`, `$realtobits`,
  `$shortrealtobits`, `$bitstoshortreal`.

### PLI / VPI

- [ ] **VPI** — object-model access (`vpi_handle`/`vpi_get`/`vpi_get_value`), value
  change and simulation-phase callbacks, `vpi_put_value`.
- [ ] **Assertion & coverage APIs** (VPI extensions).
- [ ] **Compiler directives** — complete `` `define `` (with args),
  `` `ifdef``/`` `ifndef``/`` `elsif``, `` `include``, `` `line``, `` `resetall``,
  `` `default_nettype``, `` `celldefine``, `` `pragma`` (slang preprocesses;
  ensure each is honored semantically).
