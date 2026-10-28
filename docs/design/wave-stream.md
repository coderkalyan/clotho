# Wave stream — live value-change streaming to a waveform viewer

Clotho's second dump backend: instead of formatting VCD text into a file, publish
the **backend-neutral record stream** (`docs/VCD.md` §4) into a shared-memory ring
that an external viewer maps and decodes. This document is the design of record
for the clotho side and the **normative wire specification**. The consumer side —
tide's ingest, riptide's renderer — is designed outside this repository and takes
the wire from this file.

`docs/VCD.md` §1 already names this backend: *"most importantly **direct viewer
integration** (stream value changes to a waveform viewer over shared memory /
socket rather than a file). The record boundary in §4 is designed to make these
drop-in."* This is that work. Nothing here changes the producer, the sample
point, the diff, or the sim loop.

Read `docs/VCD.md` first. This document assumes §3 (value encoding), §4 (the
producer/record/sink split), §5.3 (the producer) and §8 (sim-loop wiring).

---

## 1. Why this exists

Watching a simulation through a VCD file costs three things a live viewer should
not pay:

- **Formatting.** Binary→ASCII per bit, then the reader's ASCII→binary per bit.
  Both are pure waste when producer and consumer agree on a binary encoding.
  `docs/VCD.md` §4.1 already identifies formatting as the expensive stage.
- **The file.** A minute of a busy design is gigabytes on disk that nobody keeps.
- **Latency.** Nothing appears until clotho's 1 MiB output buffer fills
  (`g_vcd.iobuf`, `src/runtime/Runtime.cpp:4647`; the `FILE` itself is
  deliberately unbuffered, `:5142-5144`), and the reader has no way to know a new
  instant landed except by polling file size and re-parsing a tail it cannot
  frame without rescanning.

The record stream has none of these problems: it is already binary, already
self-describing, already chunkable at time markers, and already handed off at
instant boundaries. It needs a transport and a metadata header. That is the
whole of phase 1.

### What this is not

- Not a replacement for VCD. `$dumpfile` keeps working; the sinks compose
  (§4.4), so a run can write a VCD *and* feed a viewer.
- Not FST. FST is a different sink behind the same seam (`docs/VCD.md` §11
  backlog); this work builds the seam FST will reuse.
- Not run control. Starting, stopping and stepping the simulation from the
  viewer is a separate channel and a separate design; see §9.

---

## 2. Topology

```
┌─ riptide (Electron main) ──────────────────────────────────────────────┐
│  spawn: clotho run --wave-stream=<endpoint> [--wave-stream-wait] …     │
└───────────────────────────────────────────────────────────────────────┘
                              │
┌─ riptide (native addon, Rust ingest thread) ──────────────────────────┐
│                                                                       │
│   control  ── AF_UNIX / named pipe, Content-Length JSON frames ──►    │  clotho
│              hello / ready / resync / bye / fatal                     │  (sim
│                                                                       │   thread
│   notify   ◄─ pipe or eventfd, 1 byte per publish ───────────────     │   +
│                                                                       │   sink
│   data     ◄─ mmap'd ring, records, zero copy ───────────────────     │   thread)
└───────────────────────────────────────────────────────────────────────┘
```

Three channels, each doing one job:

| Channel | Carries | Why not merged |
|---|---|---|
| Control | Handshake, the shm path, resync requests, fatal errors | JSON is versionable and greppable; it is ~10 messages for the whole run |
| Notify | One byte per publish | Lets the consumer `poll()` instead of spinning at 60 Hz on an atomic |
| Data | Every record, in order | Must be zero-copy; must not be interleaved with anything |

**The ring is the single ordered stream.** The header (scopes, variables,
timescale) travels through the ring as records, not as JSON — it is the same
machinery, it can be megabytes for a large design, and putting it in the ring
means there is exactly one ordering domain and no "did the header arrive before
the first change?" question. The control channel carries only what must be
exchanged *before* the ring exists, plus out-of-band requests.

### 2.1 Who is the server

**Clotho listens; the viewer connects.** Clotho owns the shm lifetime and the
data; a viewer may attach late, detach, crash, and reattach. `--wave-stream-wait`
blocks the start of simulation until a viewer attaches, which is the only way to
see time 0 of a fast design.

This also means a headless CI run can stream to a recorder, and two viewers can
never fight over who owns the buffer (§4.6: one consumer, enforced).

### 2.2 Platform scope

v1 targets **Linux and macOS**: `AF_UNIX` socket, a file-backed `mmap` in
`$TMPDIR`, and a `pipe(2)` (Linux: `eventfd`) for notification, both fds sent
over the socket with `SCM_RIGHTS`.

Windows maps cleanly and is deliberately deferred, not designed out:
`CreateFileMapping`/`MapViewOfFile` for the ring, an auto-reset `Event` for
notification, and a named pipe for control. The framing layer is fd-agnostic
(`FrameReader`/`FrameWriter` take a bare `int fd`, `src/transport/Transport.h:49`,
`:76`) but has **no** listener, connect, or named-pipe backend today: clotho's
entire Windows transport surface is `_setmode(_O_BINARY)` plus `_read`/`_write`
shims (`src/transport/Transport.cpp:203-208`, `:32-40`). `src/dap/Design.cpp:256`
is a Windows *child-spawn* path over anonymous `CreatePipe` handles, not a
control transport — there is nothing to reuse there either. The wire format is
identical on both platforms; the transport is net-new work. Refuse
`--wave-stream` on Windows in v1 with a diagnostic that says so, rather than
half-working.

---

## 3. Wire specification (normative)

Everything in this section is the contract. A change to any of it is a `proto`
version bump.

### 3.1 Byte order and alignment

- **Little-endian, always.** Today's in-process records are host-endian
  (`memcpy` of raw `uint64_t`, `src/runtime/Runtime.cpp:4935-4951`), which is
  correct in-process and unspecified on the wire. The stream sink pins LE and
  asserts it: `static_assert(std::endian::native == std::endian::little)`. No
  big-endian target is supported; if one ever is, the sink byteswaps and the
  reader does not change.
- **Byte-packed, unaligned.** Records have no padding and no alignment
  guarantee, exactly as in-process. A reader `memcpy`s scalars out — which
  `vcd_write_records` already does into `vcd_writer_scratch`
  (`src/runtime/Runtime.cpp:5028-5032`), so this is not a new burden.
- Sizes below are exact. `u16`/`u32`/`u64`/`i64` are LE two's complement.

### 3.2 Ring header

The first 4096 bytes of the mapping. Mutable counters sit on their own 64-byte
lines so producer and consumer never share a cache line.

| Offset | Type | Field | Writer | Notes |
|---|---|---|---|---|
| 0 | `u32` | `magic` | once | `0x57484441` = `"ADHW"` LE |
| 4 | `u16` | `proto` | once | `1` |
| 6 | `u16` | `header_bytes` | once | `4096` |
| 8 | `u64` | `capacity` | once | data-region bytes, **power of two** |
| 16 | `u64` | `data_offset` | once | `4096` |
| 24 | `u64` | `tick_fs` | once | femtoseconds per tick (`ir::Module::tick_fs`) |
| 32 | `i32` | `timescale_units` | once | `ir::Module::timeformat_units` (`src/ir/Module.h:485`) — the `$timeformat` default exponent, **advisory** (see §10.2) |
| 36 | `u32` | `flags` | once | bit 0: overflow mode is *drop* (else *block*) |
| 40 | `u32` | `pid` | once | clotho's pid, for a stale-file check |
| 44 | `u32` | `_reserved` | — | must be zero |
| 64 | `u64` | `write_seq` | producer | total bytes ever written; `release` store |
| 128 | `u64` | `read_seq` | consumer | total bytes ever consumed; `release` store |
| 192 | `u64` | `drop_seq` | producer | lowest sequence number still intact; `read_seq < drop_seq` means those bytes are gone (drop mode only) |
| 256 | `u32` | `state` | producer | 0 init, 1 streaming, 2 paused, 3 ended (§9.1) |
| 260 | `u32` | `end_reason` | producer | see §3.5 tag 11; with `state = paused`, the pause reason |
| 264 | `u64` | `end_tick` | producer | tick at which streaming ended, or at which the simulation paused |

`write_seq` and `read_seq` are **monotone byte counters**, never wrapped
indices. A byte at sequence `s` lives at `data_offset + (s & (capacity - 1))`.
`capacity` is a power of two so the mask is free.

Once-written fields are written before the consumer is told the mapping exists
(the `ready` message, §3.6) and never change.

### 3.3 Publish protocol

The producer writes a **batch** — one or more *whole* instants, never a partial
one — then publishes it with a single release store:

```
producer:                                  consumer:
  fill data[write_seq .. write_seq+n)        w = write_seq.load(acquire)
  write_seq.store(write_seq + n, release)    decode data[read_seq .. w)
  notify()                                   read_seq.store(w, release)
```

The acquire/release pair is the only synchronization. There is no lock on the
data region.

Invariants a consumer may rely on:

1. **An instant never splits a batch.** This already holds in-process: buffers
   are sized to hold a full handoff batch plus one more whole instant
   (`src/runtime/Runtime.cpp:5277-5294`) and handoff happens only at instant
   boundaries (`:5372`). A consumer that stops decoding at `write_seq` therefore
   always stops at a consistent point.
2. **A record never wraps.** When the tail of the ring cannot hold the next
   record whole, the producer emits a `Pad` record (§3.5 tag 7) covering the
   remainder and starts the record at offset 0. A consumer walking a batch
   therefore never has to reassemble a record from two spans.
3. **Records are ordered.** `Time` markers are non-decreasing and, in practice,
   strictly increasing per emitted instant (the wheel's `now` *"starts at zero,
   advances only through rt_wheel_tick"*, `src/runtime/Runtime.h:1148-1149`, and
   the only two writes to `now_` both increase it, `src/runtime/Wheel.cpp:59-63`;
   verified over 2001 instants in `tests/vcd_test.cpp:320-360`, which asserts
   `times == 2001` at `:357`).
4. **Only changes are sent**, never snapshots, except the three force-all
   triggers and the `$dumpoff` checkpoint (§3.7).

### 3.4 Backpressure and overflow

Two policies, selected by `--wave-stream-overflow`:

**`block` (default).** The producer waits until
`write_seq - read_seq + n <= capacity`. A viewer that stops reading throttles the
simulation. This is the correct default for a debugging tool: the waveform is
complete, and a paused viewer is usually a deliberate act. This is also exactly
today's in-process policy (`vcd_cv_free.wait`,
`src/runtime/Runtime.cpp:5095`) and `docs/VCD.md` §6's stated principle: *"fixed
capacity + backpressure, never grow."*

> **Detach on disconnect.** Blocking forever on a *dead* consumer would hang the
> simulation. If the control socket has closed, the sink detaches: it logs one
> warning naming the tick, sets `state = ended`, `end_reason = detached`, and
> the run continues with the stream sink removed. A crashed viewer must never
> wedge a batch job.

**`drop`.** The producer overwrites unread bytes and publishes the new floor in
`drop_seq` — the lowest sequence number still intact, which is
`write_seq - capacity` once the ring has lapped. The consumer detects being
lapped (`write_seq - read_seq > capacity`, equivalently
`read_seq < drop_seq`), resumes decoding at `drop_seq`, and **synthesizes a gap**
for `[read_seq, drop_seq)`. The producer writes no gap record: only the consumer
knows what it missed.

> `drop_seq` is a *position*, not a count of destroyed bytes. A consumer resumes
> at it directly, so it must be a sequence number in the same space as
> `write_seq`/`read_seq`.

A gap is representable without any new concept. The consumer emits, for every
variable, an all-`x` sample at the last tick it saw intact, and the next real
value it decodes ends the unknown interval. That is semantically exact — "we do
not know what happened here" — and needs nothing from tide beyond the `x` state
it already has. After a gap the consumer normally sends `resync` (§3.6) to get a
full snapshot and re-anchor.

### 3.5 Record tags

Tags 0 and 1 are today's in-process records, byte-identical. Everything else is
new and exists because an external consumer, unlike the in-process VCD sink,
receives no metadata out of band.

| Tag | Name | Payload | Total bytes |
|---|---|---|---|
| 0 | `Change` | `u32 idx`, `u32 nwords`, `must[nwords]u64`, `may[nwords]u64`, `z[nwords]u64` | `9 + 24·nwords` |
| 1 | `Time` | `u64 tick` | 9 |
| 2 | `ScopeEnter` | `u16 name_len`, `u8 name[name_len]` | `3 + name_len` |
| 3 | `ScopeExit` | — | 1 |
| 4 | `VarDecl` | `u32 idx`, `u32 width`, `u8 flags`, `u16 name_len`, `u8 name[name_len]` | `12 + name_len` |
| 5 | `ArrayDecl` | `u32 base_idx`, `u32 count`, `u32 width`, `u8 flags`, `i64 left`, `i64 right`, `u16 name_len`, `u8 name[name_len]` | `32 + name_len` |
| 6 | `HeaderEnd` | — | 1 |
| 7 | `Pad` | `u32 len` — skip `len` bytes counting this record's tag | `len` (≥ 5) |
| 8 | `DumpOff` | — | 1 |
| 9 | `DumpOn` | — | 1 |
| 10 | `DumpAll` | — | 1 |
| 11 | `End` | `u32 reason`, `u64 tick` | 13 |

- `flags` is the existing `RT_VCD_FLAG_*` set verbatim
  (`src/runtime/Runtime.h:537-540`): bit 0 four-state, bit 1 net, bit 2 narrow
  (2-state slot), bit 3 real.
- `Change.nwords` is `ceil(width/64)` (`vcd_nwords`,
  `src/runtime/Runtime.cpp:4580`) and is carried per record so a consumer can
  frame the stream without the variable table.
- Plane order is **must, may, z**, each `nwords` LE `u64`, low word first. The
  contract is the encoder `vcd_rec_change` (`src/runtime/Runtime.cpp:4944-4951`)
  over the layout `vcd_read_planes` produces (`must = out`, `may = out + nw`,
  `z = out + 2*nw`, `:4790-4794`); the record-stream comment at `:4611-4617` and
  the word order at `:4587` corroborate it. For a *narrow* (2-state) variable the
  producer still emits three planes with `may == must` and `z == 0`
  (`:4769-4777` scalar, `:4797-4803` multi-word) — the wire is uniform, the slot
  is not.
- Per-bit decode is `docs/VCD.md` §3, unchanged: `z=1 → z`, else `may=0 → 0`,
  else `must=1 → 1`, else `x`.
- For a `real`/`shortreal` variable (`flags` bit 3) `must[0]` is the IEEE-754 bit
  pattern; `must[0] != may[0]` is the all-`x` `$dumpoff` checkpoint, which the
  VCD sink prints as `NaN` (`:4832-4835`).
- `End.reason`: 0 `finish` ($finish or normal end), 1 `error` (a runtime fatal),
  2 `detached` (consumer gone, §3.4), 3 `limit` ($dumplimit reached).
- Unknown tags are a **fatal protocol error** for the consumer, not something to
  skip. A stream it cannot fully frame is a stream it cannot trust, and `proto`
  exists to prevent this.

### 3.6 Header sequence and control messages

Ordering on the ring, for the whole run:

```
ScopeEnter/ScopeExit/VarDecl/ArrayDecl …          (registration order)
HeaderEnd
( Time, Change* )*                                (steady state)
( DumpOff | DumpOn | DumpAll )                    (interleaved at instant boundaries)
End
```

**The header is complete before the first `Time`.** This is a protocol
invariant, not an accident: `$dumpvars` registers the whole design in one burst
inside an `initial` block, then calls `rt_vcd_start`
(`src/ir/llvm/Vcd.cpp, lower_dump_limit/lower_dump_vars`), and only whole-design `$dumpvars` is
accepted (`src/lower/SystemTasks.cpp, lower_dumpvars`). A consumer may therefore build its
hierarchy once and never grow it. Incremental declaration after `HeaderEnd` is
reserved for a future `proto` and is a protocol violation in v1.

The record sequence mirrors, one for one, the calls the runtime already
receives: `rt_vcd_scope` → `ScopeEnter`, `rt_vcd_upscope` → `ScopeExit`,
`rt_vcd_var` → `VarDecl`, `rt_vcd_array` → `ArrayDecl`, `rt_vcd_start` →
`HeaderEnd`. **No new metadata plumbing is needed anywhere in the compiler or
the runtime's callers** — the sink simply stops throwing the names away (§4.2).

Control messages, `Content-Length`-framed JSON over the socket (reusing
`src/transport/`, which already does exactly this framing for LSP and DAP and
knows nothing about JSON):

*Consumer → clotho*

| Message | Fields | Meaning |
|---|---|---|
| `hello` | `proto`, `features[]`, `frame_us`, `overflow` | Attach. `frame_us` is the consumer's target frame interval and sets the time-based publish cadence (§4.5). `overflow` requests a policy; clotho may refuse. |
| `resync` | — | Emit a full snapshot of every variable at the next settled instant (`force_all`). Used after a gap or a late attach. |
| `bye` | — | Detach cleanly. Clotho removes the sink and continues. |

*Clotho → consumer*

| Message | Fields | Meaning |
|---|---|---|
| `ready` | `proto`, `shm{path,bytes}`, `tick_fs`, `timescale_units`, `overflow`, `clotho{version,commit}` | Mapping exists and the header records are being written. The notify fd rides on this message via `SCM_RIGHTS`. `tick_fs` is authoritative; `timescale_units` is advisory (§10.2). |
| `fatal` | `code`, `message` | Unrecoverable; the socket closes after. |
| `state` | `state`, `tick`, `reason` | Simulator state changed — `running` / `paused` / `ended`. Sent **whoever caused the transition**, including a DAP client or a `$stop` in the design. See §9.1. |

A `proto` mismatch is refused in `ready` with `fatal`/`proto_mismatch` naming
both versions. No silent degradation and no feature sniffing beyond `features[]`.

### 3.7 `$dumpoff` / `$dumpon` / `$dumpall` semantics on the wire

These already exist and already produce records; the stream just needs the
consumer to interpret them correctly.

- `$dumpoff` emits an **all-`x` checkpoint for every variable**, then deactivates
  (checkpoint `src/runtime/Runtime.cpp:5313-5329`, deactivation `:5331-5332`,
  forced handoff `:5333`). Those are real value records, not a control frame, and
  a consumer must ingest them as data. Tag 8 follows so the consumer knows the
  burst was a checkpoint rather than the design going unknown.
- `$dumpon` (`rt_vcd_on`, `:5383-5389`) and `$dumpall` (`rt_vcd_all`,
  `:5391-5395`) set `force_all`, so the next instant re-emits every variable.
  Tags 9 and 10 mark them. `$dumpvars` arms the same flag through `rt_vcd_start`
  (`:5304`).
- Shadows are deliberately left stale across `$dumpoff` (`v.seen = true` with an
  un-updated shadow, `:5324-5328`); correctness depends on `$dumpon` forcing
  all. A consumer must not assume the value it last saw is still the shadow.

---

## 4. Clotho implementation

### 4.1 What has to change, and what must not

**Must not change:** the producer (`rt_vcd_flush`), the diff against the shadow,
the sample point in `emit_active_run`, the touch hooks, the record encoding for
tags 0 and 1, or any observable behaviour of the VCD file sink. `tests/vcd_test.cpp`
is the regression gate: it must pass unmodified, byte for byte.

**Must change:** four things, in dependency order.

### 4.2 Materialize the sink seam

`docs/VCD.md` §4.2 specifies an `rt_dump_sink` vtable and an
`rt_vcd_vartable`. **Neither exists** — the sink is `vcd_write_records` calling
`vcd_emit` directly (`src/runtime/Runtime.cpp:5008-5036`), and `VcdVar` stores
no name, no scope path and no net bit (`:4588-4609`); registration writes
`$scope`/`$var` lines straight into the output buffer and retains **no name and
no scope path** (`vcd_register`, `:5173-5208`; it does keep width, flags, plane
pointers, shadow and idcode, and folds the net bit into the `$var` keyword at
`:5195-5197` before discarding it).

Build what the doc already specifies, extended with the registration calls the
doc's version omitted:

```c
// A backend-neutral value-change sink. Registration and value changes both go
// through it, so a sink that needs names (FST, viewer) gets them, and one that
// does not (the VCD text writer) ignores them.
typedef struct rt_dump_sink {
    void (*scope_enter)(void* ctx, const char* name, uint32_t len);
    void (*scope_exit)(void* ctx);
    void (*var)(void* ctx, uint32_t idx, const char* name, uint32_t len,
                uint32_t width, uint32_t flags);
    void (*array)(void* ctx, uint32_t base, const char* name, uint32_t len,
                  uint32_t count, uint32_t width, uint32_t flags,
                  int64_t left, int64_t right);
    void (*header_end)(void* ctx);
    void (*records)(void* ctx, const uint8_t* buf, size_t len);  // tags 0/1, a whole batch
    void (*control)(void* ctx, uint32_t tag);                    // tags 8/9/10
    void (*flush)(void* ctx);                                    // durability point
    void (*close)(void* ctx, uint32_t reason, uint64_t tick);
    void* ctx;
} rt_dump_sink;
```

Note `records` takes a **whole batch of encoded bytes**, not one decoded change.
This is deliberate: the VCD sink already wants to walk raw bytes
(`vcd_write_records`), and the stream sink wants to `memcpy` them into the ring
with no decode at all. A per-change callback would force both to pay for a
decode neither needs.

Registration keeps writing the VCD header when a VCD sink is attached; it
additionally *records* the names so a second sink can have them. The minimum
retention is a leaf name per variable plus the enter/exit structure — which is
what the callbacks above deliver without storing anything in `VcdVar`, keeping
that struct at its current 128 bytes and out of the flush's hot loop.

Sink registration becomes a small fixed array (`kMaxSinks = 2`), not a `vector`:
the zero-allocation contract (`docs/VCD.md` §6) applies.

**Proof:** `tests/vcd_test.cpp` passes unmodified. The VCD sink is now one
implementation of the vtable rather than inline code, and nothing about its
output changed.

### 4.3 CLI: dumping without editing the design

Clotho has **no dump-related CLI flag at all** today (`src/main.cpp:117-193`);
dumping exists only if the SystemVerilog source calls `$dumpfile`/`$dumpvars`.
A viewer integration cannot require the user to edit their design.

New flags on `clotho run`:

| Flag | Meaning |
|---|---|
| `--dump[=PATH]` | Enable whole-design dumping as if `$dumpvars` ran at time 0; `PATH` defaults to `dump.vcd` |
| `--dump-format=vcd\|none` | `none` = register variables but attach no file sink (stream-only) |
| `--wave-stream=ENDPOINT` | Listen on `ENDPOINT` (AF_UNIX path) and attach the stream sink |
| `--wave-stream-wait` | Block the start of simulation until a viewer attaches |
| `--wave-stream-bytes=N` | Ring capacity, power of two, default 64 MiB |
| `--wave-stream-overflow=block\|drop` | §3.4, default `block` |

`--wave-stream` implies `--dump --dump-format=none` unless `--dump` is also
given, in which case both sinks run.

**The gating problem.** `design_uses_vcd()` (`src/ir/llvm/Vcd.cpp`)
is a whole-design scan for `$dump*`; when it is false the backend emits no touch
hooks, no flush call and no close
call, so no amount of runtime configuration can produce a record.
The predicate must become `design_uses_vcd() || cfg.dump_forced`, with
`dump_forced` set from the CLI and threaded through `ir::SimulationConfig`
(`src/ir/SimConfig.h:95` — the type is `SimulationConfig`; there is no
`SimConfig` type). This is the one change that reaches into the
backend.

Enabling dumping from the CLI must also synthesize the `$dumpfile`/`$dumpvars`
call sequence at time 0. The cleanest place is the same lowering that handles the
tasks (`src/lower/SystemTasks.cpp, lower_dumpvars`): when `dump_forced` is set and the design
contains no `$dumpvars`, emit the whole-design form into an implicit initial
process. Note there is nothing to reuse in the frontend: `lower_dumpvars`
(`src/lower/SystemTasks.cpp`) only *validates* and emits a bare
`b.dump_vars(loc(call))`. The whole-design enumeration lives in the
backend — `lower_dump_vars` walking `mod_.tops()` through `emit_vcd_scope_tree`
and `emit_vcd_var` (`src/ir/llvm/Vcd.cpp`), exactly as
`docs/VCD.md` §9 describes — and it is reached unchanged.

### 4.4 Composable sinks

`vcd_writer_loop` currently formats each batch to text (`:5061-5085`). It
becomes: for each attached sink, `sink->records(buf, len)`. The VCD sink formats;
the stream sink copies into the ring. Two sinks means one extra pass over the
record bytes, which is a `memcpy`-bound walk over data already in L2 — cheap
relative to formatting, and it buys "record a VCD while watching live", which is
what a debugging session actually wants.

Ordering across sinks does not need to be coordinated: each sink sees the same
batches in the same order, and they share no state.

### 4.5 Publish cadence — the latency floor

Handoff fires at `recbuf.size() >= kVcdHandoffBytes` (64 KiB,
`src/runtime/Runtime.cpp:5372`) or at `$dumpflush`/close. **For a file that is
correct; for a live viewer it is an unbounded latency floor** — a quiet design
can hold records for seconds.

Add a time bound. At the existing handoff site:

```cpp
if (g_vcd.recbuf.size() >= kVcdHandoffBytes) {
    vcd_handoff();
} else if (!g_vcd.recbuf.empty() && ++g_vcd.instants_since_clock >= kClockStride) {
    g_vcd.instants_since_clock = 0;
    if (now_ns() - g_vcd.last_publish_ns >= g_vcd.publish_interval_ns) {
        vcd_handoff();
    }
}
```

- `publish_interval_ns` comes from the consumer's `frame_us` in `hello`
  (16667 µs at 60 fps), so the viewer sets its own cadence.
- `kClockStride` amortizes the clock read. A settled instant can be tens of
  nanoseconds; a `clock_gettime`/`rdtsc` per instant would be measurable. A
  stride of 256 caps the clock cost at ~0.1 ns/instant while bounding extra
  latency to 256 instants, which is microseconds for any design fast enough to
  care.
- With no stream sink attached, `publish_interval_ns` is `UINT64_MAX` and the
  branch is one register compare — the file path keeps today's behaviour
  exactly.

**This is the only change to the sim-thread hot path.** It is one increment and
one compare per settled instant when not streaming.

### 4.6 The ring sink

- One consumer, enforced: the listener accepts a second connection only to
  answer `fatal`/`busy` and close. Two readers of an SPSC ring is a correctness
  bug, not a feature to support.
- The mapping is created at `hello`, sized from `--wave-stream-bytes`, and
  unlinked immediately after the consumer maps it (`ready` → consumer maps →
  consumer acks by advancing `read_seq`; clotho `unlink`s), so a crash leaves no
  file behind. `pid` in the header lets a stale mapping be recognized anyway.
- The sink never allocates after `hello`. It owns the mapping and a `pipe`/
  `eventfd` write end; both are set up during the handshake.
- `records()` is a bounded `memcpy` into the ring plus the release store. On the
  writer thread, not the sim thread — the sim thread's only new cost is §4.5.
- Notification is coalesced: write one byte only if an `notified` flag is clear;
  the consumer clears it after draining. One `write(2)` per publish (~60/s).

### 4.7 Signal classes clotho refuses

`emit_vcd_var` throws `BackendError` for events, strings and multidimensional
arrays (`src/ir/llvm/Vcd.cpp, emit_vcd_var`), which **fails the whole
compilation**. Today that is tolerable because dumping is opt-in in the source.
With `--dump`/`--wave-stream` it becomes a trap: a user asks for waves and their
design stops building.

Change it to skip-with-diagnostic: emit a `Warning`-severity `Diagnostic`
(`origin = Unsupported`, code `dump-unsupported-var`) naming the variable and
why, register nothing for it, and continue. This mirrors what consumers already
do — `tide_vcd::load` routes a variable it has no representation for to
`skipped_vars` (`tide/crates/tide-vcd/src/load.rs:63`, incremented at `:375`) and
one it can describe but not store to `unstorable_vars` (`:599`) — and it is the
LRM-neutral behaviour: `$dumpvars` on a design containing an unsupported variable
is not an error, the variable is simply not in the dump.

This is a prerequisite for `--dump`, not an optional nicety.

### 4.8 Reals

A `real`/`shortreal` variable streams with `flags` bit 3 and an IEEE-754 pattern
in `must[0]` (§3.5). Consumers that cannot store reals (tide's
`Type::is_storable()` is true only for `Logic`) declare the variable and store no
samples — the same outcome as VCD today. No special case on the clotho side.

---

## 5. What a consumer must implement

Restated as a checklist, so a second consumer (a recorder, another viewer) can be
written from this document alone:

1. Connect, send `hello`, receive `ready`, map the shm read-only, receive the
   notify fd.
2. Validate `magic`, `proto`, `header_bytes`, `capacity` is a power of two.
3. Loop: `poll()` the notify fd → `w = write_seq.load(acquire)` → decode
   `[read_seq, w)` → `read_seq.store(w, release)`.
4. In drop mode, first check `w - read_seq > capacity`; if so resume at
   `drop_seq`, synthesize an all-`x` gap for `[read_seq, drop_seq)`, and send
   `resync`.
5. Decode records per §3.5. Build the hierarchy from tags 2–5, stop expecting
   metadata at tag 6, ingest values from tags 0/1, honour 8/9/10 per §3.7, stop
   at tag 11.
6. Transcode planes to your own value representation. must/may/z maps to
   bounds-plus-z with `min = must`, `max = may`, `z = z`, the mapping tide uses.
7. On `End` or socket close, finalize.

**Reference consumer.** `tools/wave-stream-dump` (new — there is no `tools/`
directory today) attaches to a stream and writes a VCD. It links `clotho_runtime`
(`CMakeLists.txt:284`) **and requires W0 first**: today every sink entity lives
inside the anonymous namespace spanning `src/runtime/Runtime.cpp:4577-5121`, so
`vcd_write_records`, `vcd_emit` and the engine have internal linkage and are
unreachable from another translation unit. The only exported surface is the
`extern "C" rt_vcd_*` functions, which consume live plane pointers, not records.
§4.2 is what makes this tool possible. Its
output must be **byte-identical** to the file the same run produces with
`--dump`. That is the strongest available proof of the whole path: the same
records, two transports, one output. It is also the regression test for every
future protocol change and doubles as a standalone recorder.

---

## 6. Testing

| Level | What | Where |
|---|---|---|
| Unchanged | Every existing VCD assertion, after the sink refactor | `tests/vcd_test.cpp`, unmodified |
| Unit | Ring: wrap, `Pad` insertion, full/empty, lapping, power-of-two masking | new `tests/wave_ring_test.cpp`, no simulation |
| Unit | Record encode/decode round-trip for every tag, incl. `nwords > 1` | same |
| Integration | `--dump --wave-stream` on a design; `wave-stream-dump` output byte-equals the `--dump` file | new `tests/wave_stream_test.cpp`. Use `tests/Subprocess.h`'s re-exec mechanism (`:38`), **not** `dap_test.cpp`'s hand-rolled `fork`+`exec` `Session` (`:78-280`): `Subprocess.h:17-19` explicitly rejects `fork()` after a JIT and its thread pool exist |
| Integration | `--wave-stream-wait` sees time 0; late attach + `resync` sees a full snapshot | same |
| Integration | Consumer killed mid-run → sink detaches, simulation completes, VCD is intact | same |
| Integration | Drop mode: undersized ring on a busy design → consumer observes `drop_seq` advancing past `read_seq` and the gap is where the data was | same |
| Stress | The 2001-instant ordering test, over the stream | extend `tests/vcd_test.cpp:320-360`'s design |

---

## 7. Performance targets

The stream sink should be **cheaper than the VCD sink**: it does no formatting.
Numbers to establish before tuning anything (wall-clock across process runs is
not comparable, so use same-run ratios or `perf stat -e cycles`).

| Metric | Target | Rationale |
|---|---|---|
| Sim-thread overhead, streaming vs `--dump-format=none` with no sink | < 2% | §4.5 is one increment + one compare per instant |
| Sim-thread overhead, stream sink vs VCD file sink | strictly lower | no ASCII formatting |
| Instant settle → `write_seq` visible to the consumer | ≤ 100 µs at p99 | one `memcpy` + release store on the writer thread |
| Bytes per change record | `9 + 24·nwords` | 33 B for a scalar; ~4× a VCD scalar line, traded for zero formatting on both sides |
| Ring capacity for 16.7 ms at full producer rate | fits in 64 MiB default | measure on `examples/gates_bench.v`, `examples/zbench_mem.v` |

The 33-bytes-per-scalar-change figure is worth stating plainly: **the stream is
larger on the wire than VCD text** for narrow signals, because it carries three
full planes and no compression. That is the right trade for shared memory (no
disk, no syscall per byte, no formatting on either side) and the wrong trade for
a file, which is why this is a separate sink and not a replacement. If the wire
volume ever matters, the fix is a narrow-scalar record form (tag 13:
`u32 idx, u8 state`, 6 bytes) — designed out of v1 deliberately, since a second
encoding for the same information is complexity that must earn its place with a
measurement.

---

## 8. Phasing

Each phase is independently mergeable and has its own proof.

**W0 — sink seam.** §4.2. `rt_dump_sink` with registration callbacks; VCD sink
becomes an implementation; sink array. *Proof: `tests/vcd_test.cpp` unmodified
and green.*

**W1 — dump from the CLI.** §4.3 + §4.7. `--dump`, `--dump-format`,
`dump_forced` through `SimulationConfig` into `design_uses_vcd()`, synthesized
`$dumpvars`, unsupported variables skipped with a diagnostic instead of aborting
the build. *Proof: a design with no `$dump*` produces a VCD under `--dump`; a
design containing an `event` still builds and dumps everything else.*

**W2 — ring and transport.** §3, §4.6, §2. Ring, handshake, notify, listener,
one-consumer enforcement, detach-on-disconnect, and the §9.1 `state` message —
`ended` is the only transition clotho can produce today, but the field and the
message ship now so run control does not force a `proto` bump. *Proof:
`wave-stream-dump` output byte-equals `--dump` output on every design in
`examples/`; a `$finish` mid-run produces `state = ended` with the right tick.*

**W3 — cadence and overflow.** §4.5, §3.4 drop mode, `resync`. *Proof: p99
publish latency under the target; drop mode's gap lands where the data was.*

**W4 — Windows.** §2.2.

W0 and W1 are useful on their own and unblock the riptide side, which needs only
the wire spec to start. W2 is the point at which the two repos must agree.

---

## 9. Simulator control is a different channel

Riptide will eventually want to start, stop, step and restart the simulation.
Recording the constraints here so this document does not get retrofitted:

- **Clotho has no run control today.** The simulation is one monolithic JIT'd
  `sim_drain` called once (created at `src/ir/llvm/Scheduler.cpp, emit_main`, body
  emitted by `emit_active_run`, invoked via `rt_sim_run`,
  `src/runtime/Runtime.cpp:3480`; looked up at `src/ir/Jit.cpp:586` and entered
  by the single `fn()` call at `:622`). There is no re-entrant step entry point
  and no way to return from the drain and re-enter.
- **`docs/design/dap.md` §4.1 already settled the topology**: inversion — the
  simulation parks *itself* in a runtime callback (`rt_dbg_stop`) gated by a byte
  (`rt_dbg_active`, modelled on `rt_finishing` and `rt_vcd_active`) while a
  session thread services the protocol. That mechanism is the hard part, it is
  protocol-independent, and it is a prerequisite for every option below. **The
  protocol choice is therefore a cheap, late decision.**
- **The adapter's current stage says nothing about protocol fit.** clotho's DAP
  adapter is at D0 — launch-and-report, spawning `clotho run` as a child with no
  control channel, refusing `continue`/`next`/`pause` by name from `kPlanned`
  (`src/dap/Server.cpp:362`) — but that is an implementation state that
  `rt_dbg_active` erases. It must not be cited as a reason to pick a wire.
  Likewise `docs/design/dap.md:449` (*"Waveform view | Not DAP"*) rules the
  **data plane** out of DAP; it is silent on run control.
- **What actually argues against DAP for the riptide↔clotho leg**, once the
  implementation catches up: (a) DAP's state model is *positional* — frames,
  scopes, variables — while a viewer's axis is **time**, and none of
  run-until-tick, step-one-instant, step-one-delta or step-one-clock-edge has a
  DAP spelling (`SteppingGranularity` is `statement | line | instruction`), so
  the whole vocabulary is `clotho/*` custom requests regardless; (b) DAP has one
  client, one `seq` space and one lifecycle, and `disconnect` defaults to
  terminating the debuggee — whereas a detaching viewer must never stop the run
  (§3.4); (c) the wave socket must exist anyway, and control on it is **causally
  ordered against the data**, which a separate DAP session cannot give.
- **What argues *for* DAP**, and is worth conceding: its lifecycle state machine
  is mature, and `setBreakpoints` on a source line is a genuinely good fit that a
  native protocol would have to reinvent (identity, verification, rebinding).
  Source breakpoints are DAP's home ground; time-stepping is not.

### 9.1 What phase 1 must reserve: state notification

Independent of who ends up owning control, **riptide has to behave correctly when
something else drives the simulation** — a DAP client stopping at a breakpoint, a
`$stop` in the design, an operator pausing from the CLI. A viewer that keeps
following a simulation that is not running is simply wrong.

That is a *notification* requirement, not a control one, and it belongs in phase 1
because it is a wire field: the ring header already carries `state`, `end_reason`
and `end_tick` for stream termination (§3.2). Generalizing `state` to include
`paused`, and adding the `state` control message (§3.6), costs nothing now and
avoids a `proto` bump the first time run control lands. Clotho publishes the
transition; the consumer stops following, parks its cursor at `tick`, and shows
`reason`.

### 9.2 The shape that follows

One C++ `SimControl` seam built on `rt_dbg_active`, with the protocols layered on
top rather than baked in:

| Direction | Protocol | Why |
|---|---|---|
| editor → clotho | **DAP** | mature lifecycle, source breakpoints — DAP's home ground |
| riptide → clotho | small native set on the wave socket | time-axis vocabulary is custom either way; causal ordering with the data |
| clotho → riptide | the wave socket `state` message, **phase 1** | riptide must react to *any* driver |
| editor/clotho → viewer | **WCP** | the opposite direction; riptide already implements it |

Designing the riptide→clotho set is phase 2. Nothing here forecloses the
alternative: if `SimControl` is kept protocol-neutral, exposing it through DAP as
well is a proxy, not a rewrite.

---

## 10. Open questions

1. **Ring capacity default.** 64 MiB is a guess. It should be "one frame of the
   busiest design we care about, times a safety factor" — measurable once W2
   lands.
2. **`Module::tick_fs` and `timeformat_units` disagree on an untimescaled
   design.** `tick_fs` defaults to 1 ("1 when unitless") and `timeformat_units`
   to 0 (`src/ir/Module.h:480-485`), i.e. 1 fs and 10⁰ s — contradictory. Worse,
   `timeformat_units` is **discarded**: `rt_vcd_begin` opens with `(void)units;`
   (`src/runtime/Runtime.cpp:5128`) and derives the `$timescale` from `tick_fs`
   alone (`:5149`), so `Runtime.h:548-549`'s claim that both "set the timescale"
   is wrong and no consumer has ever exercised the value. The wire carries
   `tick_fs` as authoritative and `timescale_units` as advisory (§3.2), which is
   correct either way — but a consumer rendering a time axis should ignore the
   latter until clotho validates it. Separately, the power-of-ten assumption is
   asserted only in a code comment (`:4712-4713`), and
   `vcd_write_timescale`'s raw-femtosecond fallback (`:4734-4736`) is
   **unreachable**: the unit table ends in `{1, "fs"}` (`:4721`) and
   `tick_fs % 1 == 0` always holds.
3. **AOT.** `run` is JIT-only and `rt_vcd_*` is exposed through
   `llvm::orc::SymbolMap` (declared `src/ir/Jit.cpp:171`, `rt_vcd_*` entries
   `:235-243` and `:256-262`, `rt_vcd_active` as a data symbol `:247-250`,
   installed by `jd.define` at `:415`). Whether
   `clotho build --emit llvm` + external link can reach the stream sink is
   unresolved and does not block phase 1.
4. **`g_vcd.always` is dead.** Both registration sites pass `always_scan = false`
   (`src/runtime/Runtime.cpp:5220`, `:5260`), yet the flush's hot loop loads it
   every instant (`:5355-5362`). Unrelated to streaming, but the sink refactor
   touches that code and it should be deleted or revived while it is open.
5. **Multiple viewers.** One consumer is enforced (§4.6). If two viewers ever
   matter, the answer is a fan-out recorder process, not a multi-consumer ring.
