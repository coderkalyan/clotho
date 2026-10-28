# VCD dump — design & implementation reference

Clotho value-change dumping. This document is the design of record: semantics,
the sim-thread ↔ ring ↔ backend boundary, the zero-allocation contract, the
formatter spec, and the phased implementation plan. Read before changing any
`$dump*` lowering or the `rt_vcd_*` runtime.

The design is deliberately factored so the sim thread never knows about VCD:
the producer emits a **backend-neutral value-change record stream**, and a
swappable **sink** turns records into bytes. This is what lets later work add
multithreaded formatting and alternate backends (FST, direct viewer
integration) without touching the producer or the sim loop.

---

## 1. Scope & status

VCD is an M2 (Icarus parity) feature (`docs/TODO.md`). What ships today:

- **Whole-design dumping**, value-change content cross-checked against iverilog.
  Signals of any width, four-state x/z, unpacked-array memories (one `$var` per
  element), reals and shortreals, and named events. Everything not covered is
  loud-failed (PRINCIPLES.md: never silently accept an unimplemented construct) — a
  string signal, a multidimensional array, or a `$dumpvars` scope subset stops
  the run rather than dumping a wrong or partial file.
- **All seven dump tasks**: `$dumpfile`, `$dumpvars` (whole-design form),
  `$dumpoff`, `$dumpon`, `$dumpall`, `$dumpflush`, `$dumplimit`.
- **Producer / record / sink split** (§4) with formatting and IO on a background
  writer thread (§11), and an **activity-driven dirty set** — a settled instant
  costs O(changes), not O(signals).
- **Crash recovery is buffer-granularity.** Rendered bytes sit in a fixed 1 MiB
  buffer; a hard crash loses at most that tail, and `$dumpflush` / `$finish` /
  normal end force it out. The signal-handler durability of §7 is not built.

Still open, in rough priority order: sparse hierarchical `$dumpvars` scope
subsets; string signals and multidimensional arrays; EVCD `$dumpports`;
multithreaded formatting and an io_uring IO backend; the crash handler. §11
tracks each with the reason it is not done. String signals are blocked on the
format rather than on the engine: IEEE 1800-2023 §21.7.2 has no `string` var
type, and both iverilog and Verilator silently omit a string signal from the
dump — FST, which carries variable-length values natively, is where that
belongs.

Future backends: **FST** (compressed), and most importantly **direct viewer
integration** (stream value changes to a waveform viewer over shared memory /
socket rather than a file). The record boundary in §4 is designed to make these
drop-in. The viewer backend is specified in
[`design/wave-stream.md`](design/wave-stream.md), which owns the wire format,
the transport, and the `rt_dump_sink` seam §4.2 describes but has not yet built.

---

## 2. Semantics (LRM §21.7)

Tasks: `$dumpfile`, `$dumpvars`, `$dumpoff`, `$dumpon`, `$dumpall`,
`$dumpflush`, `$dumplimit`.

- `$dumpfile("f.vcd")` — set output path. Once.
- `$dumpvars` — select variables and begin dumping. No args, `$dumpvars(0)`, or
  `$dumpvars(0, <every top>)` = whole design, the only form implemented; a
  nonzero level or a scope / variable subset loud-fails in the frontend.
- `$dumpoff` / `$dumpon` — suspend / resume. `$dumpoff` writes an all-`x`
  checkpoint for every dumped var (`rNaN` for a real, which has no x);
  `$dumpon` re-dumps current values.
- `$dumpall` — dump current values of all selected vars now.
- `$dumpflush` — flush buffered output to disk, keep dumping.
- `$dumplimit(n)` — stop dumping once the file reaches `n` bytes; `n` = 0 is
  unlimited. The cap is tested at instant boundaries, so the file ends with a
  whole instant followed by a `$comment` naming the cap, and the run continues
  undumped (a later `$dumpon` / `$dumpall` does not restart it).

### 2.1 The sampling rule — the decision that shapes everything

**VCD records one settled value per variable per simulation time, not every
intermediate store.** Recording each blocking store would diverge from iverilog
byte-for-byte and bloat the file with deltas/glitches. The correct sample point
is the *postponed* region — after the active, inactive (`#0`), and NBA regions
have all settled for that instant.

In clotho's sim loop that point is exact: `emit_active_run`
(`src/ir/llvm/Scheduler.cpp`), the block reached when the active queue, the
inactive batch, and the NBA region are all drained and the wheel is about to
advance (`wcheck`, immediately before `rt_wheel_tick`). `rt_wheel_now(wheel)`
there is the instant that just settled. **Sample there, once per settled
instant** (including time 0).

### 2.2 No-change reruns must be suppressed

A process re-triggered on an input edge routinely recomputes and stores the
value a net already held; an NBA can commit an identical value. These are
normal, not bugs, and must produce **no** VCD line. Suppression falls out of
diffing the settled value against the last *dumped* value (the shadow, §5.2):
a rerun-same-value, and a glitch-back (A→B→A within one instant), both compare
equal to the shadow and emit nothing.

---

## 3. Value encoding → VCD character

A signal's state slot is `{ i(2W) must|may, iW z }` (`ir/llvm/Backend.h` header
comment): low `W` bits = must (definitely 1), high `W` = may (could be 1),
invariant must⇒may; Z is must=0/may=1 with the z-plane bit set. A z-elided
(never-z) signal stores no z word — treat its z plane as constant zero.

Per-bit decode (2-bit test):

```
z   = 1  -> 'z'
may = 0  -> '0'
must= 1  -> '1'
else     -> 'x'      (may=1, must=0)
```

---

## 4. The sim-thread ↔ ring ↔ backend boundary

This is the load-bearing architecture. Three stages, a neutral record stream
between them:

```
 PRODUCER (sim thread)              RECORD RING                 SINK / BACKEND
 ─────────────────────             ─────────────               ───────────────
 store of dumped sig:              neutral, self-              open(vartable)
   rt_vcd_touch(idx)               describing,                 time(tick)
                                   chunkable at                change(idx, planes)
 settled instant (wcheck):         TimeMarkers:                off() / on()
   rt_vcd_flush(tick):               TimeMarker{tick}          flush() / close()
     for each touched sig:           ValueChange{idx,           ── VCD: format ascii
       diff vs shadow                  planes}                  ── FST: encode
       if changed:                     Off / On / Close        ── viewer: push shm
         update shadow
         push record  ───────────►
```

### 4.1 Why this split

- **Producer is backend-agnostic.** It emits raw `(var_idx, must/may/z planes)`
  and time markers. It never formats VCD ASCII. FST encoding, viewer streaming,
  and VCD text all live in the sink.
- **Diff is on the producer (sim thread), before the ring.** Diffing is a cheap
  integer compare (O(width/64) words); formatting is the expensive part
  (binary→ASCII per bit). Filtering no-change on the sim side means the ring
  carries only genuine changes — minimal traffic for every backend — and,
  crucially, means the sink needs **no prior-value state**: each record is
  self-contained.
- **Stateless, chunkable records enable multithreaded formatting.**
  Because a chunk boundary is always a `TimeMarker` and every `ValueChange`
  carries full planes, any run of records starting at a `TimeMarker` formats
  independently. VCD lines within one instant are order-independent (anything
  after `#time` may be in any order); instants are ordered. So later work shards
  the record stream by instant-chunk across a thread pool and gathers formatted
  bytes back in instant order — no producer or formatter change required.

### 4.2 Sink interface (runtime)

The seam that exists today is the **record stream**, not a vtable: the producer
appends `TimeMarker` / `ValueChange` / control records to a byte buffer, and one
concrete consumer (`vcd_drain`, on the writer thread) turns them into VCD text.
Nothing else in the runtime reads records, so a second backend is a second
drain, not a registration.

The vtable below is the shape that second backend forces, and it is written down
here so the producer is not designed around a single consumer — but it is a
plan, not code:

```c
// A backend-neutral value-change sink. NOT IMPLEMENTED: today vcd_drain is
// called directly. design/wave-stream.md is what materializes this.
typedef struct rt_dump_sink {
    void (*open)(void* ctx, const rt_vcd_vartable* vars);   // header / var defs
    void (*time)(void* ctx, uint64_t tick);                 // #<tick>
    void (*change)(void* ctx, uint32_t var, const rt_plane* p); // one value change
    void (*off)(void* ctx);                                 // $dumpoff checkpoint
    void (*on)(void* ctx);                                  // $dumpon
    void (*flush)(void* ctx);                               // durability point
    void (*close)(void* ctx);                               // final flush + close
    void* ctx;
} rt_dump_sink;
```

The var table would be the neutral description: per var a leaf name, scope path,
width, 4-state flag, net-vs-var. VCD-specific idcodes, FST handles, and viewer
ids are each sink's business — the VCD writer already keeps its idcode bytes in
the var table entry it owns.

---

## 5. Runtime engine

### 5.1 Registration is call-based, not a descriptor blob

At the `$dumpvars` site the backend walks the `InstanceScope` tree (`Module.h`)
and emits calls in tree order: `rt_vcd_begin`, then `rt_vcd_scope` /
`rt_vcd_var` / `rt_vcd_array` / `rt_vcd_upscope`, then `rt_vcd_start`. Each
`rt_vcd_var` passes the signal's **live plane pointers** — GEPs into
`%clotho.state`, constants after `DataLayout` — plus its leaf name, width and
flags (four-state, net, narrow, real). `rt_vcd_array` registers a whole unpacked
array as one `$var` per element from a base pointer and a stride, so the emitted
design does not grow with the array.

No blob, no offset math in the runtime, no state base: the runtime stores the
pointers it was handed, and they stay valid for the run. `SignalInfo::name`
holds the full hierarchical path; the leaf is the tail after the scope prefix.

### 5.2 State (allocated once at `rt_vcd_start`)

- **Var table** — one entry per dumped var: plane pointers, width, word count,
  flags, shadow offset, idcode bytes.
- **Epoch flags** — `epoch[i]`, one byte per var, set by `rt_vcd_touch` and
  cleared as the flush scans past it. One dense byte array parallel to the var
  table, so the per-instant scan never touches the fat var struct.
- **Scan scratch** — `5 * max_nwords + 2` words: one variable's must/may/z
  planes plus the raw must|may read buffer behind them.
- **Record buffers** — a bounded pool of fixed-size byte buffers, recycled
  between the sim thread and the writer, plus the writer's 1 MiB `FILE` buffer.

### 5.3 Producer

- `rt_vcd_touch(var_idx)` — `epoch[idx] = 1`, guarded by `rt_vcd_active` and an
  index bound (a non-dumped NBA target passes `-1` and is ignored). Setting a
  flag is idempotent, so there is no dirty list to dedup against and no ring to
  overflow. `rt_vcd_touch_range(base, count)` marks a whole array write.
- `rt_vcd_flush(tick)` — the settled-instant sample. Walks var indices in order,
  reading + diffing only the vars whose epoch byte is set and clearing it as it
  passes. On a real change: update the shadow, emit the
  `#tick` record lazily (once, only if something changed) and a value-change
  record. Index order is what makes the output byte-identical to a full scan.
  Emits nothing (not even a `#time`) for an all-no-change instant.
- `$dumpvars` / `$dumpall` / `$dumpon` arm `force_all`, which scans every var
  once regardless of the flags. `$dumpoff` emits the all-`x` checkpoint (`rNaN`
  for a real) and clears `active`.

### 5.4 VCD sink — the formatter (this must match iverilog byte-for-byte)

Emitted forms:
- Time: `#<decimal tick>\n`, once per non-empty instant. Fast unsigned→decimal.
- Scalar (W=1): `<char><idcode>\n` (e.g. `1!`).
- Vector: `b<bits> <idcode>\n`.
- Real: `r<decimal> <idcode>\n`, the shortest round-tripping decimal
  (`std::to_chars`) of the slot's IEEE-754 bits — a `shortreal` widens to double
  so the file carries one representation. A real has no x/z, so the `$dumpoff`
  checkpoint prints `rNaN`.
- Named event: `1<idcode>\n` at each instant the event triggers, and nothing at
  any other instant — an event has occurrences, not a value, so there is no
  complement and no power-on state. Multiple triggers in one instant collapse to
  the single pulse the format can express. The var table watches the event's
  64-bit last-trigger tick instead of a value slot, so the ordinary shadow diff
  is the trigger detector; the never-triggered sentinel is `UINT64_MAX`, which
  is what the shadow is seeded with so a trigger at tick 0 still reads as a
  change. A forced dump (`$dumpvars` / `$dumpall` / `$dumpon`) resyncs that
  shadow *without* emitting unless the event fired in that same instant, and the
  `$dumpoff` checkpoint skips events — both would otherwise report an occurrence
  that never happened (iverilog reports all three).
- Idcode: precomputed per var (dense printable ASCII 33..126), stored in the var
  table → emit is a `memcpy`.

**Leading-bit compression (§21.7.2.1) — asymmetric, keyed on `c = char(MSB)`.**
The reader left-extends the printed value to full width by the leftmost digit:
`0`→extend 0, `x`→extend x, `z`→extend z, but `1`→extend **0**. So the writer:

```
c=='0': drop leading 0s; print from the highest 1/x/z bit down. All-zero -> "0".
c=='1': print all W bits (cannot compress — reader would extend with 0).
c=='x': print one 'x', then from the highest non-x bit down. All-x -> "x".
c=='z': print one 'z', then from the highest non-z bit down. All-z -> "z".
```

Values of any width are held as `nwords = ceil(W/64)` little-endian 64-bit words
per plane; the record stream is a byte buffer carrying `nwords` inline, so a wide
value travels through the writer thread like any other. The must|may split of a
`> 64`-bit slot uses a bignum right-shift by `W` (`vcd_shr`).

**Fast path.** Precompute `has_xz = (may != must) || (z != 0)`.
- `!has_xz` (common): pure binary. `c` is `0`/`1`. Find the top set bit via
  `clz` (that is leading-zero suppression, no post-trim) and emit via a
  nibble→4-char (16-entry) or byte→8-char (256-entry) LUT, walking 64-bit words
  for wide buses. Branch-free per chunk.
- `has_xz` (rare): per-bit decode into scratch, then apply the `c`-keyed rule.

**Copy minimization.** Format directly into the current output buffer; rotate
buffers when near-full. No temp-then-copy. Scratch (itoa, wide-4-state chars) is
a single preallocated buffer sized to the max var width.

### 5.5 Writer thread

The sim thread fills a record buffer and, at instant boundaries only, hands a
full one to the writer through a bounded, recycled pool. Whole buffers move
under one lock — there is no per-record locking, and an instant never splits
across buffers, so the writer can format a buffer without looking at any other.
Backpressure is the pool running dry: the sim thread waits for a recycled
buffer, and never for the disk otherwise.

Ownership is what keeps this lock-light: the writer owns the file, the var table
is read-only after `rt_vcd_start`, and the shadow is sim-thread-only.
`$dumpflush` drains the pool and pushes the `FILE` buffer to the OS;
`rt_vcd_close` (including the `rt_finish` path) hands off the tail and joins
before closing.

The touch indices come from the compile-time `vcd_index_` map (`build_vcd_index`
walks in registration order), so a store site passes a constant. A scalar NBA
commit reads its index out of the parallel `clotho.nba_vcd` array beside the NBA
queue; a non-dumped target passes `-1`.

---

## 6. Zero-allocation contract

After the first `$dumpvars`, the simulation runs to completion with **zero VCD
heap allocations**, regardless of activity or file size. Every structure in §5.2
is sized from compile-time-known bounds (whole-design dump ⇒ `N_dumped` and
total plane/idcode bytes are constants) and allocated once at `rt_vcd_start`.

The hot path — per store (`touch`), per settled instant (`flush`: diff, format,
append), per change, per `write()` — touches only preallocated structures. No
`malloc`, no `vector` growth, no per-change string.

Principle (same as the wheel's inline slots and rt_string/rt_wheel caller-owned
storage; TigerStyle's static-allocation preference): **fixed capacity +
backpressure, never grow.** When an instant's output exceeds the buffer set, the
producer blocks while a buffer drains via `write()` and is reused in place — it
never allocates a bigger buffer. The record-buffer pool works the same way: when
it is exhausted the producer blocks until the writer recycles one. Bursts are
absorbed by sizing, not growth.

Caveats: (1) `rt_vcd_start` itself does the one-time allocations — reducible to
a single arena, or fully zero-heap via caller-owned storage (backend sizes a
state region like `rt_wheel`); the multi-MB output buffers are best left as one
`mmap`/`malloc` at start rather than bloating `%clotho.state` when not dumping.
(2) `$dumpvars` runs during simulation (in an `initial`), so it is technically a
sim-time allocation — but a single setup event, not per-timestep.

---

## 7. Safety / crash recovery

Requirement: on a sim crash, recover most of the VCD up to a reasonable
checkpoint with only minor cleanup.

- **Append-only, instant-aligned flushes.** The file always ends at a clean,
  parseable record. Bytes handed to `write()` live in the kernel page cache and
  survive a *process* crash (SIGSEGV) even without fsync — so everything up to
  the last flushed instant is recoverable, with at most a truncated trailing
  line ("minor cleanup").
- **`$finish`.** `run_jit` wraps execution in setjmp/longjmp (`Jit.cpp`);
  `rt_vcd_close` (final flush + drain + fsync + close) hooks there and on the
  normal `wdone` exit. The runtime caches the wheel pointer at `open` so close
  can flush the in-progress instant's `now`.
- **Ctrl-C and `--stop-time`.** Both end the run at a time-step boundary
  (`rt_wheel_advance`), through the same `wdone` exit as a natural end, so the
  dump is closed after the step that last ran and ends at a whole instant
  (docs/design/cooperative-finish.md, "Ending a run from outside the design").
- **Crash handler (not built).** The plan is a SIGSEGV/SIGABRT handler doing an
  async-signal-safe `write()` of any already-formatted queued buffers, then
  re-raising. The unformatted record tail is lost (formatting is not
  signal-safe) — bounded, acceptable. Until then a hard crash loses the
  un-flushed tail, at most the 1 MiB `FILE` buffer plus whatever the writer
  thread had not yet formatted.
- **`$dumpflush`** forces a drain + `write()`; optional periodic `fsync` for
  power-loss durability (not needed for process-crash recovery).

---

## 8. Sim-loop wiring

- **Sample point.** `emit_active_run` `wcheck`, before `rt_wheel_tick`:
  `if (rt_vcd_active) rt_vcd_flush(rt_wheel_now(wheel))`. Reached once per
  settled instant (active + inactive + NBA all drained). Time 0 included on
  first arrival.
- **Touch hooks.** Blocking store (`service_after_write`), scalar NBA commit
  (`emit_nba_commit_bucket`, index from `clotho.nba_vcd`), and array NBA commit
  (whole-array `rt_vcd_touch_range`): `if (rt_vcd_active) rt_vcd_touch(idx)`.
  The dumped set is compile-time known, so a non-dumped signal emits no hook,
  and a design with no `$dump*` call at all emits none.
- **Close.** `wdone` and the `run_jit` longjmp handler call `rt_vcd_close`.

---

## 9. IR / plumbing (mirrors `$readmem`)

`Inst::Tag`s — `DumpFile`, `DumpVars`, `DumpLimit`, `DumpOff`, `DumpOn`,
`DumpAll`, `DumpFlush` — with a payload only where one is needed: the filename
value for `DumpFile`, the byte cap for `DumpLimit`. `DumpVars` carries none,
because the only form accepted is the whole-design one. Builder methods →
backend `lower_dump*` → `rt_vcd_*` calls, the same path as
`lower_readmem`→`rt_readmem`. The dumped set is enumerated by the backend from
the module signal table and the scope tree; a scope-subset selector is rejected
at the frontend.

---

## 10. Implemented

- [x] IR: the seven tags above + Builder methods (Ir.h/.cpp, Builder, Printer).
- [x] Frontend: lowers all seven tasks; `$dumpvars(0, <tops>)` accepted as
      whole-design, a nonzero level / scope subset / variable loud-fails.
- [x] Backend: registers the scope tree and dumpable signals at `$dumpvars`
      through the call-based ABI (§5.1, live plane pointers); instruments stores
      and NBA commits with `rt_vcd_touch`, and every event trigger form through
      the one `emit_event_fire` choke point; flushes at `wcheck`; closes at
      `wdone`. Unpacked arrays register per element; unnamed temporaries are
      skipped; a string or multidimensional-array signal loud-fails.
- [x] Runtime: dirty-set flush (§5.3) + record stream + writer thread + VCD
      formatter (header, §5.4 compression, reals, events, `$dumplimit`), storage
      allocated once at start.
- [x] JIT: `rt_vcd_*` symbols registered; `rt_vcd_close` on the `rt_finish`
      (pre-longjmp) path and at `wdone`.
- [x] Tests — 24 cases in `tests/vcd_test.cpp`: scalars and vectors with
      no-change suppression, header/timescale, per-bit x and z, nested
      hierarchy, `$dumpoff`/`$dumpon`, unpacked-array memories (including
      descending ranges and per-element NBA writes), `> 64`-bit vectors, wide
      four-state values, writer-thread ordering under a high-activity trace,
      dirty-set re-emission, `$dumpvars(0, top)` ≡ bare, a dump opened from a
      task, reals (values, NaN checkpoint, real memories and NBA targets), both
      `$dumplimit` forms, and named events (declaration, all three trigger
      forms, one pulse per instant, a trigger at tick 0, no phantom pulse from a
      forced dump, submodule scopes).

Note: value-change content matches iverilog, but the VCD is **not** byte-
identical — identifier-code spelling, header cosmetics, and signal aliasing
differ. Tests assert clotho's own deterministic output and structural
properties, not a byte diff against iverilog.

## 11. Open work

- Sparse `$dumpvars`: a nonzero level or a scope / variable subset. The
  registration walk is already per-scope, so this is a frontend selector plus a
  filtered walk — no runtime change.
- Signals clotho refuses to dump rather than dump wrongly: strings (no standard
  VCD var type exists — see §1) and multidimensional arrays. EVCD `$dumpports`
  is the same shape of work.
- Multithreaded formatting: shard the record stream by instant-chunk across a
  pool (records are already stateless and chunkable at time markers), ordered
  gather into the IO stage; io_uring IO backend.
- Alternate sinks behind the same record contract: FST, and the live viewer
  stream (designed in [`design/wave-stream.md`](design/wave-stream.md); it also
  materializes the §4.2 sink vtable, which does not exist in code today).
- Durability: the SIGSEGV/SIGABRT handler of §7.
- Formatter fast path: 2-state LUT (`clz` + nibble/byte table) for wide buses.
- Array writes whose element index is not known at the write site (an NBA array
  commit, `$readmem` / `$fread`) touch the whole element range, so the next
  flush diffs every element of that array. Bounded for a regfile, O(depth) for
  a big RAM; a per-element index on the NBA queue would remove it.
