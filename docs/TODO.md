# TODO — gaps to full IEEE 1800-2023

This file lists what Clotho does not do yet. It is not a changelog and not a
feature list: whatever the LRM describes and this file does not list is meant
to work, and when a gap closes its entry is deleted. Every missing construct
here is refused loudly by the compiler rather than accepted silently
(`docs/PRINCIPLES.md`); entries marked (performance) are not refusals.

Rules, enforced by `tests/todo_test.cpp`:

- One entry per gap, named by the LRM construct and clause, and phrased as what
  is missing. Never "X works except Y": name Y.
- An entry is at most three lines. Why a gap is hard or how to close it belongs
  in `docs/design/`; why a change was made belongs in its commit message.
- `[ ]` is an open gap. `[~]` groups the open pieces of a partly implemented
  construct, listed as `[ ]` children; while that work is active it may carry
  one `Works so far:` line, removed when the work stops.
- No source line numbers, internal phase labels, measurements, oracle results
  or history. Those go stale; the code and the commit log keep them.
- Rewrite, split, merge or delete any entry, including ones you did not write.

Good:

    - [ ] **`?:` over a queue, dynamic array or associative array** (11.4.11) —
      an ambiguous condition needs both arms staged in container temps.

Bad (a done feature, a narrowed claim, and history):

    - [~] **`?:` over aggregates** — landed for structs and fixed arrays; a
      queue arm is refused for now, measured 46 IR lines vs 84, which used to ...

Milestones, in order; lower ones unblock higher ones:

- **M1 — Verilog-2005 RTL.** Typical synthesizable Verilog-2005 runs end to end.
- **M2 — Icarus Verilog parity.** Everything Icarus Verilog handles: full
  Verilog-2005 plus its SystemVerilog subset and tooling.
- **M3 — Common SystemVerilog.** What open-source tools accept: user types,
  packages, interfaces, dynamic data, basic classes, immediate assertions.
- **M4 — Industrial SystemVerilog and UVM.** A real UVM testbench runs: full
  OOP, randomization, concurrent assertions, coverage, DPI.
- **M5 — Full LRM.** Everything else: VPI, specify and timing, nettypes,
  configurations, checkers, the complete system task library.

## M1 — Verilog-2005 RTL

### Operators & expressions

- [ ] **Table 7-1 default of an unpacked union** (7.3, Table 7-1) — the first member's default
  is not produced, so an ambiguous `?:` over an aggregate holding an unpacked union is refused.
- [ ] **`?:` over a queue, dynamic array or associative array** (11.4.11) — an ambiguous
  condition needs both arms staged in container temps.
- [ ] **Aggregate `==` over a struct with an associative-array member** — refused, and with it an
  ambiguous `?:` over such a struct under a 4-state condition.
- [ ] **`?:` over aggregates of a type distinct from the target's** (11.4.11, 6.22.2) — arms
  equivalent to, but interned differently from, the assignment target need a converting copy.
- [ ] **Short-circuit `&&&` conjuncts** (12.4) — every conjunct of a `cond_predicate` is
  evaluated, so `if (h != null &&& h.x)` fatals on a null `h`.
- [ ] **In-order `case` item comparison** (12.5) — all item-label predicates are evaluated
  before dispatch, so a trapping later label fatals even when an earlier one matches.
- [ ] **Lazy `unique` / `unique0` / `priority` `if` conditions** (12.4.2) — every arm condition
  is evaluated up front, so a trapping arm fatals even when an earlier arm is taken.

### Subroutines

- [ ] **DPI export from a shared library** (35) — a `--dpi-lib` `.so`, loaded by dlopen, cannot
  resolve the design's exported SV subroutines.
- [ ] **Class-method call in a function** — a function body cannot call a class method.
- [ ] **Nonblocking assignment to a function frame local** (13.4.4) — refused.
- [ ] **Automatic-lifetime `event` local** (6.21, Table 6-7) — each activation needs a fresh
  event object.
- [ ] **Bulk default reset of an automatic aggregate (performance)** (6.21) — the Table 6-7
  default is stored per leaf; a trivially copyable aggregate could copy its prototype image.
- [ ] **`$strobe` / `$monitor` in a function** — the deferred body needs a task-call record slot,
  which a function lacks.
- [ ] **Nonblocking assignment to a task frame local or `output` / `inout` formal** — refused.

### Memories & arrays

- [ ] **Single-record nonblocking whole-array copy (performance)** — each element of an
  unpacked-array NBA is deferred separately instead of as one memcpy record.
- [~] **Unpacked-array slices** (7.4.5) — two slice shapes are still refused.
  - [ ] **Slice of an array with struct or array elements** — refused.
  - [ ] **By-value read of a slice** — a constant or indexed slice as a concatenation operand
    or `%p` argument is refused.
- [ ] **Sliced memory target for `$readmem*` / `$fread`** — a slice destination is refused.

### Elaboration & structure

- [~] **Runtime-indexed aggregate parameter** (6.20, 11.5.1) — two value shapes are refused.
  - [ ] **Wildcard-index associative-array parameter** — cannot be materialized for a runtime
    read.
  - [ ] **Queue or associative-array parameter with aggregate elements** — cannot be seeded
    for a runtime read.
- [ ] **`defparam` across top-level modules** — a path into another top is refused.
- [ ] **`defparam` that changes an implicitly typed parameter's type** (6.20.2, 23.10.1) — the
  parameter keeps its original initializer's type, so the override is refused.

### Procedural control

- [ ] **`disable` of a target that can suspend in a callee** (9.6.2) — a target parked in a
  time-consuming task call, a `fork` or an `expect` cannot be aborted down the enable chain.

## M2 — Icarus Verilog parity

### Time-consuming procedural code

- [~] **Intra-assignment timing** (9.4.5) — target shapes and qualifiers still refused; see
  docs/design/intra-assignment-timing.md.
  - [ ] **String or whole-container nonblocking target** (`s <= #5 t`, `q <= #2 r`) —
    needs an owned-copy record that is freed on every drop path.
  - [ ] **`iff` on a nonblocking intra-assignment event** (9.4.2.3) — only enabled
    occurrences may count down, so a `repeat` count cannot reuse the body guard.
  - [ ] **Runtime-offset select of a very wide vector as a nonblocking target** —
    needs a heap-side record payload.
  - [ ] **Conditional RHS over a whole aggregate in a nonblocking form** — refused.
  - [ ] **Aggregate target with a container member in a nonblocking form** — refused.
  - [ ] **Indexed array-slice target in a nonblocking form** (`a[i +: n] <= #5 x`) —
    refused.
  - [ ] **String or container operand of a streaming target in a nonblocking form** —
    refused.
  - [ ] **Blocking form onto a string or a whole unpacked array** (`s = #5 t`,
    `mem = #5 other`) — the store path evaluates the LHS before the RHS.
  - [ ] **Blocking form onto an assignment-pattern lvalue** — refused.
  - [ ] **Intra-assignment timing on a force target** (10.6.2), also as a concatenation
    operand — suppression is decided at commit, not when the update is armed.
- [ ] **Per-event `iff` in a multi-event list** (9.4.2.3) — `@(posedge a iff x or b)` needs
  identification of the waking source.
- [ ] **Event object reclamation** (15.5.5.2) — event objects are never freed.
- [ ] **More than 1024 concurrent detached `fork` children** — past the activation
  bound the scheduler buffers report a fatal; see docs/design/fork-join.md.
- [ ] **Wake time of a `ref` write after a suspend** — a task writing a
  `ref` actual after a suspend wakes its waiters at the task's return, not at the write.
- [~] **Event control and `wait` on a class property** (9.4.2, 9.4.3) — whole-aggregate and
  container-valued waits are refused.
  - [ ] **`@` over a container property expression** (9.4.2) — needs the value
    snapshotted in the waiter and compared on each wake.
  - [ ] **`wait` on an unpacked struct, union or fixed unpacked-array property**
    (9.4.3) — needs a whole-aggregate change compare.
  - [ ] **Non-null `chandle` property transition** — not exercised end to end; making one
    needs DPI.
- [ ] **Virtual-interface wait in a `$unit` class body bound by a later top** — a
  multi-top design binds such a body at the first top, so the wait stays never-bound.
- [ ] **Implicit `@*` that collects no signal event inside a procedural body** —
  refused.
- [ ] **`wait` on a container member reached through a virtual interface** (9.4.3) —
  `wait(vif.q.size())` is not exercised end to end.
- [~] **Event control and `wait` on a fork-captured automatic** (9.4.2, 9.4.3, 9.3.2) — only
  scalar-valued `wait` is supported.
  - [ ] **`@` on a fork-captured automatic** (9.4.2) — needs a value-change
    classification the capture box does not provide.
  - [ ] **`wait` on a fork-captured unpacked struct, union or fixed unpacked array**
    (9.4.3) — the capture box has no aggregate change notifier.

### Continuous / structural timing

- [~] **Continuous assignment delay** (10.3.3) — missing pieces:
  - [ ] **Rise / fall / turn-off delays** — `assign #(r, f)` and `assign #(r, f, t)`.
  - [ ] **Delay on a select, concatenation or aggregate target**.
  - [ ] **Delay on a partial driver of a resolved net** — its slot powers up all-x, so the
    undriven bits would fold as x instead of z.
- [~] **Gate delay** (28.16) — missing pieces:
  - [ ] **Rise / fall / turn-off delays** — `#(r, f)` and `#(r, f, t)` on any gate or
    switch, including `cmos` / `rcmos` and the turn-off delay of tri-state gates (28.6).
  - [ ] **Delayed gate with a bit-addressed output on a single-driver net**.
  - [ ] **Delayed gate as a partial driver of a resolved net**.
- [~] **Net delay** (28.16) — missing pieces:
  - [ ] **Rise / fall / turn-off delays** — on `wire #(r, f, t) w;` and on a net
    declaration assignment `wire #(r, f) w = e;` (10.3.3).
  - [ ] **Net delay on an unpacked-aggregate net** — resolution folds packed planes only;
    needs a leaf-wise inertial replay.
  - [ ] **Net delay on a user-defined-nettype net** — needs nettype resolution functions.

### Procedural continuous assignment & force

- [ ] **`force` of an input or inout port target** (10.6.2) — the port-copy driver does not
  honour the force.
- [ ] **Intra-assignment timing on a write to a `force` target** (10.6.2) — suppression must
  be decided when the deferred update commits, but the deferred record carries no gate.
- [ ] **`force` / `release` of a concatenation** (10.6.2) — a concatenation of nets or net
  bit/part-selects as the target.

### Real numbers

- [ ] **Concurrent fractional-delay timers (performance)** (3.14) — fine-tick periods past
  the wheel window spill to the overflow heap; needs a hierarchical wheel or a coarser tick.
- [ ] **Negative, NaN or infinite real delay** — not clamped, as for the integer path.
- [ ] **Time unit of a `$unit`-scope subroutine or class** (3.14.2.2) — with no `timeunit`
  in `$unit`, `#d` and `$time` count raw wheel ticks; needs a fixed default timescale.
- [ ] **`%c`, `%u` and `%z` of a real argument**.

### Observability & debug

- [ ] **Located backend refusals outside instruction lowering** — refusals from the
  program-region pre-passes, DPI and class-descriptor emission, and VCD variable
  emission name their subject but carry no source location.
- [ ] **`-W` and `--diag-json` for design diagnostics** — both are accepted, but `-W`
  does not change what a design reports and nothing writes the JSON.
- [ ] **Selective VCD dump** — `$dumpvars` scope-subset and sparse selection; only a
  whole-design dump is produced.
- [ ] **VCD of a string signal** (21.7.2) — the format has no `string` var type; needs a
  format with variable-length values such as FST.
- [ ] **VCD of a multidimensional array**.
- [ ] **VCD of an unpacked struct or unpacked array** — a dump over one is refused; needs
  one `$var` per leaf.
- [ ] **VCD of a container or class-handle signal** — a dump over a queue, dynamic array,
  associative array or class handle is refused.
- [ ] **`$monitor` with `$time` or `%t` in its arguments** (21.2.2) — change detection
  compares rendered output, so it reprints every time step.
- [ ] **`$strobe` and `$monitor` inside a function** (21.2.2) — the deferred body needs a
  task-call record slot that only a coroutine has.
- [ ] **`$timeformat` non-literal suffix and zero-argument reset form** — only a
  string-literal suffix is accepted.
- [ ] **Clotho IR dump of a design that calls `$monitor`** — `clotho build --emit ir`
  crashes with a segmentation fault partway through printing.
- [ ] **Freeing a finished simulation** — frames, events and allocations of processes still
  suspended when a simulation ends are not freed, so a host running several leaks them.

### Executables (`clotho build`)

- [ ] **Cross-compiling an executable** — `build` writes one only for the machine it runs
  on; another target needs its executable runtime and C library startup files.

### File and string I/O

- [ ] **`$fgets` into a part-select or non-byte-multiple target**.
- [~] **`$fscanf` / `$sscanf`** (21.3.4.3) — missing pieces:
  - [ ] **Target wider than 64 bits or not a whole variable**.
  - [ ] **`%e`, `%f`, `%g` conversions and any `real` destination**.
  - [ ] **`%t`, `%u`, `%z`, `%v` and `%m` conversions**.
  - [ ] **`%s` into an unpacked array of `byte`** — needs an element-ordered store
    (`b[0]` is the first character) rather than a right-justified plane.
- [~] **`$value$plusargs`** (21.6) — missing pieces:
  - [ ] **Part-select destination** — refused for every conversion.
  - [ ] **`%e`, `%f`, `%g` conversions and any `real` destination** — needs an ASCII-to-real
    scan and a real write-back.
  - [ ] **Numeric conversion into a `string` destination** — `%d` and friends must store the
    converted value as an integral-to-string assignment (6.16).
  - [ ] **Non-literal search string** — also missing for `$test$plusargs`.
- [ ] **Parse caching across runs (performance)** — the Driver, SourceManager and syntax
  trees are rebuilt each run; needs a content-addressed cache.
- [ ] **Run-start reset of wait queues, observed/deferred queues and string pools** — not
  returned to power-on between designs in one process (docs/design/cooperative-finish.md).
- [ ] **`-G` override naming a parameter no top-level module has** — accepted without a
  diagnostic, so a mistyped name simulates the design with its defaults.
- [~] **Non-literal `$sformat` / `$sformatf` format** (21.3.3) — missing pieces:
  - [ ] **`%m` and `%l`** — the scope and `library.cell` names never reach the renderer.
  - [ ] **Aggregate or container argument** — refused at compile time.
  - [ ] **Operand wider than 64 bits under `%e`, `%f`, `%g`**.
  - [ ] **Empty argument (adjacent commas)** (21.2.1) — contributes format text, which is
    unknown at compile time.

### Structural primitives

- [~] **Gate output terminals** — missing pieces:
  - [ ] **Member of a packed-struct net driven by a single gate** — `s.lo[0]` has no static
    terminal; only the multi-driver resolved form works.
  - [ ] **Concatenation or part-select output**.
- [ ] **Bit-select gate output (performance)** — each update rewrites the whole word and
  drops thunk/fusion eligibility; needs per-bit packed-signal fusion.
- [ ] **Compile time of long gate chains (performance)** — LLVM compile time grows
  superlinearly with the number of chained gate instances (`examples/gates_bench.v`).
- [ ] **Bidirectional pass switches** (28.8) — `tran`, `rtran`, `tranif0`, `tranif1`,
  `rtranif0`, `rtranif1`; conduction is a net merge the one-pass driver fold cannot express.
- [ ] **User-defined primitives** — combinational and sequential UDPs.

### Net types, strengths, resolution

- [ ] **`trireg` nets** — strength and charge storage.
- [~] **Multiple-driver net resolution** (6.6) — missing pieces:
  - [ ] **Unpacked-array and unpacked-struct nets** — needs per-element slots and resolvers.
  - [ ] **Concatenation-operand driver** — needs a split into per-net stores.
  - [ ] **Cross-scope partial driver** — a constant bit/part-select driver from a different
    scope than the net's other drivers, entering through an input port.
- [~] **Drive strengths** — missing pieces:
  - [ ] **Strength ranges** — an x/z value on an asymmetric-strength driver resolves to a
    definite value where the range model yields x.
  - [ ] **Drive strength on `wand` / `wor` nets**.
  - [ ] **`pullup` / `pulldown` on a bit- or part-select target** (28.5).
  - [ ] **Strength reduction through gates** — including the r-variants (`rnmos`, `rpmos`,
    `rcmos`), which behave as their nonresistive forms (28.14).

### Hierarchy reach

- [ ] **Cross-top hierarchical reference** — `top2.sig` from `top1`; each top lowers with
  its own signal map.
- [ ] **Hierarchical path through a generate-block instance or an instance-array element**
  — `g.u.sig` and `arr[i].sig` fail at elaboration.
- [~] **`inout` / `ref` port aliasing** (23.2.2.1, 23.3.3) — a port and its connection must be
  one whole net each, of one width; missing pieces:
  - [ ] **Port that is a select, or connection that is a select or concatenation** — needs
    sub-signal aliasing.
  - [ ] **Connection to a net of another width** — the bits they share connect and the rest of
    the wider side is unconnected; needs sub-signal aliasing.
  - [ ] **Two ports bound to one internal net** (`module m(a, a)`) — the two connected nets
    become one net; needs merging the nets of one scope.
- [ ] **Port expression that is not a select of one internal net** (23.2.2.1, 23.2.2.2) — a
  non-ANSI concatenation port `module m({a, b})` and an ANSI `.p(expr)` port are refused.

### Timing checks (basic)

- [ ] **`specify` blocks** — module path delays (`(a => b) = 3;`) and the timing checks
  `$setup`, `$hold`, `$setuphold`, `$width`, `$period`.

## M3 — Common SystemVerilog

### Data types

- [ ] **2-state formal in the call ABI (performance)** — a 2-state formal is still passed as a
  4-state (mm, z) plane pair whose z plane is constant 0.
- [ ] **Nonblocking assignment to a string character** (10.4.2) — `s[i] <= c` is refused; it needs
  a masked character record rather than a whole-string one.
- [ ] **NBA to a class property** — `c.x <= v`, of any type including `string`, is refused; NBA
  staging is keyed by signal and an object body has none. See docs/design/classes.md §8.
- [ ] **Intra-assignment delay on a string or container NBA** — `s <= #5 t` and `q <= #2 r` are
  refused; the deferred record has no owned-handle tail. See
  docs/design/intra-assignment-timing.md.
- [ ] **Non-constant string replication count** (11.4.12.1) — deliberate deviation, kept open to
  revisit: `{m{"-"}}` with a runtime `m` is accepted when the result is a `string`.
- [ ] **`string` ports** — a string port, including a string reached through a struct member or a
  container element, is refused.
- [ ] **String event control / sensitivity** — `@(s)` and inferred sensitivity on a string are
  refused.
- [ ] **`case ... inside` string range bound** (11.4.13) — a `[lo:hi]` item over a string selector
  is refused.
- [ ] **Enum `next` / `prev` with a non-constant step** — only a constant step is lowered.
- [~] **Slice of an unpacked array with aggregate elements** (7.4.5) — the backend has no slice
  over aggregate element storage.
  - [ ] **Struct-bearing array slice as an actual** — refused as a subroutine argument and as an
    `input` port connection.
  - [ ] **Slice copy between aggregate frame locals** — `o = x[0:1];` and `return x[0:1];` are
    refused.
  - [ ] **Dynamic-array slice with aggregate elements** (7.4.5) — a struct or heap-handle element
    is refused.
- [ ] **Array slice as a `ref` actual** (13.5.2) — no slice has an anchored place a pointer can
  name, so it is refused.
- [ ] **Streaming concatenation as an unpacked-aggregate actual** (11.4.14.3) — an lvalue on an
  `output` / `inout` formal and an rvalue on an `input` formal are refused.
- [ ] **Dynamic-array rvalue materialization** — a dynamic-array value that is not a variable has
  no temp-handle path as a container element value; queues have one.
- [ ] **`inout` / `ref` aggregate ports** — a port copy is a continuous assignment and needs
  change detection over aggregate storage.
- [ ] **Aggregate port driven by a `'{...}` pattern or call** — such a connection to an aggregate
  port is unsupported.
- [ ] **Event control on a `real` expression** — `@(x.r)` and other `real`-typed member, select or
  expression operands are refused; only integral operands lower.
- [ ] **Unpacked union with a non-integral member** (7.3) — a `real`, `string`, unpacked
  aggregate or handle member is refused; members overlay one flat bit plane.
- [~] **Tagged unions** (7.3.2) — open pieces of the unpacked form, formatting and the dump.
  - [ ] **Non-integral member of an unpacked tagged union** — a `real`, `string`, unpacked
    aggregate or class payload is refused.
  - [ ] **Unpacked tagged union in `force` / `release`, DPI and container elements** — each
    position is refused for the unpacked form.
  - [ ] **`%p` of a queue or dynamic array of tagged unions** (21.2.1.6) — refused; the runtime
    container descriptor carries no member-name table.
  - [ ] **VCD of a tagged union** (21.7) — dumped as one flat anonymous `$var`, so the active
    member name is not recoverable.
  - [ ] **Latent: all-`void` unpacked tagged union width** — `max_width + tag_bits - 1`
    underflows when both terms are zero; masked because the width lands at 1.
- [ ] **Associative-array literal under NBA** (7.9.11) — `a <= '{k: v}` is refused; a keyed
  literal has no upfront size to stage into a temp handle.
- [ ] **Associative-array literal with an aggregate value element** (7.9.11) — refused; it needs a
  per-leaf store per entry.
- [ ] **Event element of a queue or dynamic array** (7.5, 7.10) — refused; each element needs its
  own event object and the event slot array is sized at compile time.
- [ ] **Locator methods on an associative array** (7.12) — `find`, `find_index`, `min` and the
  rest are refused; they need key traversal.
- [ ] **`item.index(d)` for `d > 1`** (7.12.4) — the second and later dimension indices are
  refused.
- [ ] **Virtual-interface element in a fixed-array ordering method** (7.12) — the ordering mirror
  refuses the element type.
- [ ] **Unpacked array with a negative bound** — `int a[-1:1]` is refused.
- [ ] **`map`** (7.12.5) — `q.map(x) with (...)` is not lowered; the result keeps the receiver's
  container kind with the clause's element type.
- [ ] **Multidimensional dynamic arrays** — refused.
- [ ] **Array query dimension argument beyond 1 on a dynamically sized operand** (20.7) — a nested
  dynamic dimension, as in a queue of queues, is refused.
- [ ] **Bounded queue `[$:N]` under whole assignment** — `qb = other` from a queue, dynamic array
  or fixed array keeps every source element instead of truncating to the bound.
- [ ] **`===` / `!==` over a container with a 4-state leaf** (11.4.5) — a 4-state integral, `real`
  or associative-array leaf is refused.
- [ ] **Logical `==` / `!=` over an associative array with a 4-state value element** (11.4.5) —
  refused; X propagation differs from a byte compare.
- [ ] **Associative-array key mixing a class handle with a variable-width component** (7.8) — a
  `string`, queue, dynamic-array or associative-array component beside the handle is refused.
- [ ] **Assignment-pattern lvalue over a container or handle** (10.9) — a queue, dynamic-array or
  associative-array right-hand side, or a `string` / handle member, is refused.
- [ ] **Bit-stream casting** (6.24.3) — `{<<{...}}` / `{>>{...}}` as cast operands.
- [ ] **Open question: `$cast` into a packed tagged union** (11.9) — the bit cast is not
  tag-checked and can install a tag/payload pair no `tagged` expression builds; is that legal?

### Operators & expressions

- [ ] **Concatenation as a compound-assignment target** — `{a, b} += x` is refused.
- [ ] **String element as a compound-assignment target** — `s[i] += c` is refused.
- [~] **`inside` operator** (11.4.13) — missing set-member and range forms:
  - [ ] **Associative-array set member** — needs the first/next key walk (7.9.4) rather
    than an index loop.
  - [ ] **Queue or dynamic-array set member with non-singular elements** — refused.
  - [ ] **Tolerance ranges** — `[c +/- d]` and `[c +%- d]` are refused.
  - [ ] **Unbounded `$` range bounds** — `[$:a]` and `[a:$]` are refused.
- [~] **Streaming operators** (11.4.14) — forms that need a bit-level stream buffer:
  - [ ] **`with` clause on a stream operand** — refused.
  - [ ] **Width not a multiple of the destination granule** — e.g. `bit [5:0] q[$]` into a
    string, or a 12-bit value into a byte queue, is refused.
  - [ ] **Block reversal of a dynamically sized stream** (11.4.14.2) — `{<<S{...}}` into a
    string, queue or dynamic array is refused; block placement needs the total width.
  - [ ] **Unpack from a dynamically sized source** (11.4.14.3) — `{>>{q}} = s` is refused.

### Procedural control

- [ ] **`case … inside` tolerance ranges** (12.5.4) — `[a +/- b]` and `[a +%- b]` labels
  are refused.
- [ ] **`case … inside` unbounded range bounds** (12.5.4) — `[$:a]` and `[a:$]` labels are
  refused.
- [ ] **`unique` / `unique0` / `priority` violation maturation** (12.4.2) — a pending report
  is not flushed when the statement re-executes in the same time step, so glitches report.
- [ ] **`always_ff` non-sequential body warning** (9.2.2.4) — no diagnostic is issued when
  the body does not describe sequential logic.
- [ ] **`foreach` skipping an outer dimension over a nested runtime one** (12.7.3) —
  `foreach (a[, i])` over a container of containers is refused.

### Hierarchy & namespaces

- [~] **Interfaces and modports** (25) — missing pieces:
  - [ ] **Interface net driven from both sides** — a net continuously driven from inside and
    outside the interface is refused, as for `inout` ports.
  - [ ] **Modport `export` methods** — refused.
  - [ ] **Modport expression over a select or concatenation** — `.name(sig[hi:lo])` and
    concatenation forms are refused.
- [ ] **`bind`** — binding a module or interface into another scope is refused.
- [ ] **Nested and extern module declarations** — refused.
- [ ] **`export pkg::*;` of a name the exporting package never references** — the name does
  not resolve through the export (slang lookup limitation).
- [ ] **Checkers and nettypes declared in a package** — refused when referenced.

### Classes (basic OOP)

- [ ] **Bare instance-method call with no receiver in scope** (8.7) — refused when neither
  an object under construction nor an enclosing method supplies `this`.
- [ ] **Static class property of a struct type with member defaults** (8.9) — with no
  initializer of its own it silently reads the power-on default, not the member defaults.
- [ ] **Class type identity across modules** — a `$unit` or package class referenced
  from two modules lowers as two distinct types.

### Assertions (immediate)

- [ ] **Immediate `cover` statistics** (16.3) — no per-statement coverage count and no
  end-of-simulation report.
- [~] **Deferred immediate assertions** (16.4) — missing pieces:
  - [ ] **Subroutine-call action block** — a user task or void function as the action is
    refused.
  - [ ] **`$display` / `$write` / `$fatal` action** — refused.
  - [ ] **Deferred assertion inside a function or task** — refused.
  - [ ] **`disable` as a flush point** (16.4.4) — refused.
  - [ ] **Action block scheduled in the Reactive region** — the report matures through the
    deferred queue instead of scheduling the action block in Reactive.
- [ ] **`$countbits` with a non-constant control bit** — refused.
- [ ] **`$isunbounded` of a hierarchical reference** — refused; slang does not
  constant-evaluate it.

## M4 — Industrial SystemVerilog + UVM

### Full object orientation

- [~] **Garbage collection** (8.4) — collector coverage and policy still missing.
  - [ ] **Collector tracing of event, `mailbox`, `semaphore`, `process` and
    `std::weak_reference` handles** (8.4) — not yet described to the collector.
  - [ ] **Evacuating collection (performance)** — the heap is non-moving.
  - [ ] **Multi-mutator collection (performance)** — collection assumes one mutator.
- [ ] **Vtable-forced method with an unlowerable signature** — a return or formal type that
  fails to lower aborts the whole compilation instead of stubbing that one method.
- [ ] **Class field typed as its own descendant** — `Base` holding `Helper` holding
  `Derived extends Base` crashes the backend with no diagnostic; root cause unknown.
- [~] **Class scope and nesting** — open pieces of class scope resolution.
  - [ ] **Nested classes** — a class declared inside another class.
  - [ ] **`::` on a parameterized class specialization** — `C#(args)::` resolution.
- [ ] **`$cast` to a `chandle` destination** (6.24.2) — refused; a chandle carries no type
  identity to test.

### Constrained randomization

- [ ] **Constraint blocks** — relational, `inside`, `->`, `if`/`else`, `foreach`,
  `solve … before`, `dist`, `soft` and `unique` constraints; there is no solver.
- [ ] **Inline constraints** — `randomize() with { … }`, on an object and on
  `std::randomize`.
- [ ] **`constraint_mode()`** — enabling and disabling constraint blocks.
- [~] **`rand` / `randc` variables** — random variable shapes still refused.
  - [ ] **`randc` wider than 8 bits** (18.6) — only up to 8 bits is accepted.
  - [ ] **`rand real`** — a real-typed random variable.
  - [ ] **`rand` dynamic arrays, queues, associative arrays and unpacked structs** — not
    accepted as random variables.
  - [ ] **More than 64 `rand` properties in one class chain** — the per-object `rand_mode`
    mask is 64 bits.
  - [ ] **`randomize(x)` naming a non-`rand` property** — in-line random variable control
    over a state variable.
  - [ ] **`rand_mode()` on a subelement** — mode control below a whole property.
- [ ] **`randcase` weight wider than 32 bits** (18.16) — needs several 32-bit draws stitched.
- [ ] **Object `get_randstate()` / `set_randstate()`** — the object-side randstate pair is
  refused.

### Concurrency & synchronization

- [ ] **`wait fork` / `disable fork` in an assertion action block** (9.6.1, 9.6.3) — the
  action block runs on the monitor's thread, not one of its own.
- [ ] **Typeless mailbox** (15.4.9) — `dynamic_type` needs a dynamically typed element and a
  run-time type identity for the `get` check and `try_get`'s negative return.
- [ ] **`mailbox #(event)`** — an event-typed message.
- [ ] **Class derived from `semaphore` or `mailbox`** — extending a built-in class.
- [ ] **`std::weak_reference`** (8.30) — the class is refused.
- [~] **Clocking blocks** (14) — open pieces of clocking-block support.
  - [ ] **Input skew other than `#1step`/`#0`, output skew other than `#0`** — needs a
    per-clockvar history keyed by time; an edge skew needs a second event per block.
  - [ ] **Net-typed clocking output** (14.16) — needs a `'z`-initialized
    `(strong1, strong0)` driver joined to the net's resolution.
  - [ ] **Cycle-delayed drive `cb.sig <= ##n expr`** (14.16.2) — needs pending drives in a
    ring indexed by cycle offset.
  - [ ] **Bit-select or slice drive target** — a synchronous drive of part of a clockvar.
  - [ ] **Clocking item bound to an expression** (14.5) — slices and concatenations.
  - [ ] **Clocking event other than one `posedge`/`negedge` signal** — any other edge kind
    or a multi-signal event.
  - [ ] **Assertion item declared inside a clocking block** (14.3, 16.16) — refused.
  - [ ] **Sampled value function in procedural code** (16.9.3) — needs history rings keyed
    to the default clocking.
- [~] **Program blocks** — open pieces of program-block support.
  - [ ] **Clocking block or concurrent assertion clocked by a program signal** (24.3.1) —
    the scheduler does not iterate from the reactive region set back through Observed;
    see docs/design/program-blocks.md §10.
  - [ ] **`$exit` in a subroutine no single program instance owns** (24.7) — the exiting
    program must come from the calling thread.
  - [ ] **`$exit` inside a function** (24.7) — a function has no abort exit for its caller
    to poll.

### Concurrent assertions (SVA)

- [~] **Sequence and property operators** (16.9, 16.12) — open operator shapes:
  - [ ] **Abort over a multi-cycle implication antecedent** — attempts part-way through the
    antecedent each owe a verdict that the shared trigger cannot count.
  - [ ] **`disable iff` inside a named property** — refused.
  - [ ] **Repetition with a zero lower bound** — it admits the empty sequence.
  - [ ] **Goto and non-consecutive repetition over a compound sequence** — only
    single-expression operands lower.
  - [ ] **Sequence `and` in a consequent** — both operands must be bounded.
  - [ ] **Property operator over an operand that raises its own obligation** — a monitor has
    one trigger; only `nexttime[n]` and `always [m:n]` operands lower.
  - [ ] **Non-packed sampled variable** (16.5, 16.6) — aggregate, container, string and
    event operands of an assertion expression are refused.
- [~] **Sampled value functions** (16.9.3) — open pieces:
  - [ ] **Gating expression on `$past`** — refused.
  - [ ] **Explicit clocking argument** (16.12.14) — refused on `$past`, `$rose`, `$fell`,
    `$stable` and `$changed`; unclocked contexts such as `disable iff` need it.
  - [ ] **Non-constant `$past` tick count** — refused.
  - [ ] **Operand that is an expression** — the operand must be a plain variable.
  - [ ] **Use outside an assertion expression** — refused.
- [~] **Function calls in assertion expressions** (16.9.1, 4.4.2) — open pieces:
  - [ ] **User function call** — its body is not traced to add the variables it reads to the
    sampled set.
  - [ ] **System function outside the 20.9 bit-vector family** — e.g. `$time`.
- [~] **Unbounded `always`, `s_eventually` and `until`** — open pieces:
  - [ ] **Multi-cycle operand of an unbounded `always` or left operand of `until`** — needs
    a second subset construction over the determinised operand.
  - [ ] **Property mixing tracking disciplines** — e.g. `always (a |=> b)`; the tracking
    mode is per monitor.
- [~] **`implies` and `iff`** — a monitor has one end-of-trace disposition where each state
  needs its own; see docs/design/sva-property-layer.md:
  - [ ] **`implies` with operands of equal strength** — includes the default weak form.
  - [ ] **`iff`** — always refused.
- [ ] **Implication with an unbounded antecedent** — e.g. `b[=2] |=> c`; the trigger is
  nondeterministic, so attempts cannot be counted per state.
- [ ] **`expect` statement** — needs an action block that can suspend in Reactive.
- [ ] **User task call in an assertion action block** — the task body is not traced for what
  it may suspend on.
- [~] **Sequence methods** — open pieces:
  - [ ] **`.triggered` on a same-clock sequence** — not lowered.
  - [ ] **`.matched`** (16.13.6) — needs multiclock sequences.
- [~] **Assertion control tasks** (20.12) — open pieces:
  - [ ] **Non-constant arguments** — need a run-time dispatch over every assertion in scope.
  - [ ] **Control of immediate, deferred and `expect` assertions** — they lower inline with
    no per-assertion state to gate.
  - [ ] **`unique` / `priority` violation control** — not gated.
- [ ] **Multiclock sequences and properties** (16.13) — needs a per-letter clock attribute
  and a rewritten automaton step; also blocks `@$global_clock` beside another clock.
- [ ] **Local variables and sequence match items** — needs a register automaton that carries
  a valuation per attempt.
- [ ] **Recursive properties** (16.12.4) — needs a fixpoint over the property graph, not a
  Glushkov lowering.

### Functional coverage

- [ ] **`iff` guards on covergroups, coverpoints and bins** — not lowered.
- [ ] **`with function sample(args)`** — a covergroup sampled with caller-supplied values.
- [ ] **`wildcard` bins** — not lowered.
- [ ] **Covergroup formals and non-constant bin bounds** (19.5.1) — the bin set becomes
  per-instance and must be built at construction.
- [ ] **Cross coverage** — implicit coverpoints, automatic cross bins, `binsof` /
  `intersect` / `!` / `&&` / `||`, `with (expr) [matches N]`, `cross_retain_auto_bins`,
  `cross_num_print_missing`.
- [ ] **Transition bins** — `trans_list`, `[* r]` / `[-> r]` / `[= r]`,
  `default sequence` and overlapping matches.
- [~] **Coverage options and accounting** — open pieces:
  - [ ] **`type_option.merge_instances`** (19.11.1) — union by bin name, with
    `max(at_least)` across instances.
  - [ ] **`option.per_instance` reporting and `option.get_inst_coverage`** — not implemented.
  - [ ] **`type_option.weight` / `goal` / `comment`, `option.name` / `comment`** — not
    implemented.
  - [ ] **`option.detect_overlap`** — no overlap warnings.
- [ ] **Real coverpoints and `type_option.real_interval`** — need a second bin algebra over
  floats.
- [ ] **`covergroup extends`** (19.4.1) — slang hides an inherited coverpoint behind a
  transparent member.
- [ ] **`type_option.strobe`** — Postponed-region sampling.
- [ ] **`distribute_first`** — not implemented.
- [ ] **Set-expression bins** (19.5.1.2, 19.6.1.4) — not implemented.
- [ ] **Block events `@@(begin blk)` / `@@(end blk)`** — no block entry or exit hook exists.
- [ ] **Coverage database tasks** (19.9) — `$set_coverage_db_name`, `$load_coverage_db`,
  `$coverage_merge`, `$coverage_save`, `$coverage_control`, `$coverage_get` and
  `$coverage_get_max`; the LRM defines no database format.
- [ ] **Immediate `cover` hit count** — nothing is counted or reported, and nothing is
  refused.
- [ ] **`cover property` hit report** — hit counts are kept but never reported.

### Foreign interface

- [ ] **DPI import `output` / `inout` string argument** — C writes back an unbounded
  `char*` with no size contract.
- [ ] **DPI export `output` / `inout` string argument and string return** — the returned
  `char*` has no defined lifetime and the same unbounded copy-out.
- [ ] **DPI export open-array arguments** — needs open-array formals in the general
  subroutine ABI.
- [ ] **DPI open-array actual of a dynamic array of packed elements** — needs a
  runtime-sized marshal scratch.
- [ ] **DPI multi-dimensional open arrays** — needs `svGetArrElemPtr2/3` and a recursive
  descriptor.
- [ ] **One `c_identifier` exported from several scopes** — slang drops the duplicate
  exports before Clotho can see them.
- [ ] **`svScope` handles for non-top instances** — `svGetScopeFromName` cannot name a
  nested instance.
- [~] **DPI import and export tasks** (35.8) — open pieces; see docs/design/dpi-c.md:
  - [ ] **Exported task that consumes time** — each in-flight imported-task call needs its
    own machine stack; a suspending exported task is a run-time fatal.
  - [ ] **Disable protocol** (35.9) — `svIsDisabledState`, `svAckDisabledState` and honouring
    the imported task's `int` return.
  - [ ] **`svGetCallerInfo`** (35.9) — not provided; a C call to it fails to resolve.

### UVM enablers

- [~] **Virtual interfaces** (25.9) — open pieces; see docs/design/interfaces.md:
  - [ ] **Member of an interface instantiated inside the interface a handle names**
    (`vif.i.d`) — the context record does not carry its children's.
  - [ ] **`edge` / `iff` qualifier on an event control over an expression of members** —
    refused.
  - [ ] **Intra-assignment timing on a nonblocking write through a handle** — `vif.d <= #5 x`;
    the delayed-update record has no instance context.
  - [ ] **Virtual interface handle as a queue, dynamic array or associative array element**
    — refused.
- [ ] **Property expansion for a class-handle `%p` operand** (21.2.1.6) — a direct operand
  prints only its class name.
- [ ] **Handle argument to a runtime `$sformat` format** (21.2.1.6) — the renderer cannot
  tell whether it meets `%p` or a numeric conversion.

## M5 — Full LRM (IEEE 1800-2023)

### Net & primitive completeness

- [ ] **User-defined nettypes** — `nettype` declarations and their resolution
  functions.
- [ ] **Generic interconnect** — `interconnect` nets.
- [ ] **`trireg` charge storage** — capacitance strengths and charge decay.
- [ ] **Sequential UDP edge tables** — full edge-sensitive tables, including `*`, `r`
  and `f` notation.

### Timing & back-annotation

- [ ] **Specify path forms** — edge-sensitive, state-dependent, conditional and
  `ifnone` paths, and path polarity.
- [ ] **Full timing-check set** — `$recovery`, `$removal`, `$recrem`, `$skew`,
  `$timeskew`, `$fullskew`, `$nochange`, and `$setuphold` with notifiers.
- [ ] **SDF annotation** — `$sdf_annotate`.
- [ ] **Pulse-style controls** — pulse filtering control in specify blocks.
- [ ] **Hierarchical and fractional time units** — full per-scope `timeunit` /
  `timeprecision` and precision-correct rounding of fractional delays.

### Configuration & checkers

- [ ] **Library maps and configurations** — `library`, `config`, `design`, `cell` and
  `instance` binding, `liblist` and `use`.
- [ ] **Checkers** — `checker` / `endchecker` declarations, their assertions, free
  variables and instantiation.
- [ ] **`randsequence` weight wider than 32 bits** (18.17) — needs several 32-bit draws
  stitched (18.16); `randcase` shares the limit.
- [ ] **Nested `rand join`** (18.17.5) — a `rand join` operand that is itself a `rand join`.

### Language corners

- [~] **Pattern matching** (12.6) — subjects outside the flat integral form:
  - [ ] **Unpacked aggregate subject** — `case ... matches` or a structure pattern over an
    unpacked struct or array.
  - [ ] **Non-integral tagged-union payload** — a pattern over an unpacked tagged
    union with a `real`, `string`, unpacked aggregate or class member.
- [ ] **`let` declarations** — full `let` support, including `let` in sequences and
  checkers.
- [ ] **Recursive properties** — a property that instantiates itself.
- [ ] **Full constraint language** — array reduction methods and functions in
  constraints, `disable soft`, iterative constraints, constraint inheritance.
- [ ] **Coverage corners** — real coverpoints, type coverage, cross `binsof`
  expressions, coverage merge and database; see docs/design/functional-coverage.md.
- [ ] **Attributes** — semantic meaning for `(* ... *)` attributes beyond
  parse-through.

### Complete system task / function library

- [ ] **`$bits` over every type** — the remaining query-function operand types.
- [ ] **Extended VCD** — the `$dumpports` family.
- [ ] **`$writememh` / `$writememb`** — writing memory contents to a file.
- [ ] **Address-ranged `$readmemh` / `$readmemb`** — the start and finish address
  arguments.
- [ ] **PLA modeling tasks** — `$async$and$array`, `$sync$nand$plane` and the rest of
  the family.
- [ ] **Stochastic queue tasks** — `$q_initialize`, `$q_add`, `$q_remove`, `$q_full`,
  `$q_exam`.
- [ ] **`$random` family completeness** — the remaining random-number system
  functions.
- [ ] **`$stop` diagnostic levels** — the level argument of the simulation-control
  tasks.

### PLI / VPI

- [ ] **VPI** — object-model access (`vpi_handle`, `vpi_get`, `vpi_get_value`),
  value-change and start/end-of-simulation callbacks, `vpi_put_value`.
- [ ] **Assertion and coverage APIs** — the VPI assertion and coverage extensions.
- [ ] **Compiler directive semantics** — `` `define `` with arguments, conditionals,
  `` `include ``, `` `line ``, `` `resetall ``, `` `default_nettype ``, `` `celldefine `` and
  `` `pragma `` are not verified to take their semantic effect.
