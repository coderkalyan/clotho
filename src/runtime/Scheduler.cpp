// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Simulation kernel: time wheel, run loop, processes, NBA and intra-assignment staging, finish.

#include "Gc.h"
#include "RuntimeInternal.h"

using namespace rt_internal;

namespace {

const RuntimeWheel* unwrap(const rt_wheel* w) {
    return reinterpret_cast<const RuntimeWheel*>(w->storage);
}

clotho::Timer& unwrap(rt_timer* t) {
    return *reinterpret_cast<clotho::Timer*>(t);
}

// One array-NBA signal's deferred-write buffer: a growable byte buffer of
// fixed-stride records (defined and filled by the backend) plus a live count.
// Reused across timesteps (count reset, capacity kept).
struct ArrQueue {
    std::vector<uint8_t> buf;
    uint32_t             count = 0;
};

std::vector<ArrQueue> arr_queues; // indexed by dense array-nba id
uint32_t              arr_total = 0; // records staged this pass, across all arrays

} // namespace

extern "C" void* rt_nba_arr_slot(uint32_t idx, uint32_t stride) {
    if (idx >= arr_queues.size()) {
        arr_queues.resize(idx + 1);
    }
    ArrQueue&    q    = arr_queues[idx];
    const size_t need = static_cast<size_t>(q.count + 1) * stride;
    if (q.buf.size() < need) {
        // Geometric growth; resize copies existing records, so slot pointers the
        // backend already wrote stay valid (they were filled before this grow).
        q.buf.resize(std::max<size_t>(need, q.buf.size() * 2));
    }
    void* p = q.buf.data() + static_cast<size_t>(q.count) * stride;
    q.count++;
    arr_total++;
    return p;
}

extern "C" void* rt_nba_arr_base(uint32_t idx) {
    // An array with no writes this pass may have no buffer yet; its commit loop
    // runs zero iterations, so a null base is never dereferenced.
    return idx < arr_queues.size() ? arr_queues[idx].buf.data() : nullptr;
}

extern "C" uint32_t rt_nba_arr_count(uint32_t idx) {
    return idx < arr_queues.size() ? arr_queues[idx].count : 0;
}

extern "C" uint32_t rt_nba_arr_any(void) {
    return arr_total;
}

extern "C" void rt_nba_arr_reset(void) {
    for (ArrQueue& q : arr_queues) {
        q.count = 0;
    }
    arr_total = 0;
}
extern "C" {
uint32_t rt_intra_total = 0;
}
namespace {

// One deferred nonblocking-intra-assignment site (a target signal). `pool` owns
// every record ever allocated for the site (stable addresses — the wheel holds
// pointers into them across time); `freelist` holds the currently-free ones;
// `ready` holds records that have fired and await the NBA-region commit. A record
// cycles pool-backed: freelist -> (armed on wheel) -> ready -> freelist.
struct IntraSite {
    std::vector<std::unique_ptr<unsigned char[]>> pool;
    std::vector<void*>                             freelist;
    std::vector<void*>                             ready;
    // Byte size every record in `pool` was allocated at. The backend monomorphizes
    // the record per target signal, so this is constant for the life of a design --
    // but `intra_sites` is process-global and outlives one, and a dense site id is
    // reused from zero by the next design. Reusing a pooled block for a WIDER
    // record would let the fill run off its end, so a changed stride retires the
    // pool (see rt_intra_alloc).
    uint32_t stride = 0;
    // Length of the ready-list prefix that a record maturing now must not be ordered
    // before: up to and including the last plain NBA copied onto the list
    // (rt_intra_now). See intra_ready_push.
    size_t plain_end = 0;
};

std::vector<IntraSite> intra_sites; // indexed by dense intra-nba id
uint32_t               intra_seq = 0; // monotonic arm-order stamp

// Per inertial continuous-assignment site (LRM 10.3.3): the current deschedule
// epoch and whether a store is currently in flight (so a same-value change
// keeps its timer, LRM 10.3.3 step b). The pending VALUE itself lives in
// `inertial_bufs` below, not here — this struct never sizes with the site's
// width, so it stays fixed-size regardless of net width.
struct InertialState {
    uint64_t epoch   = 0;
    bool     pending = false;
};
std::vector<InertialState> inertial_states; // indexed by dense intra-nba id

// Per inertial site, the persistent byte buffer holding the last value armed
// (rt_inertial_buf). Sized once, on the site's first call — a site's width is
// fixed after elaboration, so this never resizes again. The backend owns the
// byte layout (mm plane [+ z plane]) and does every value compare itself via
// `icmp` on values loaded from this buffer; the runtime never interprets these
// bytes, exactly like the general rt_intra_rec pool.
std::vector<std::unique_ptr<unsigned char[]>> inertial_bufs;      // indexed by dense intra-nba id
std::vector<uint32_t>                         inertial_buf_bytes; // allocated size of each, parallel

// Per trigger id (dense (trigger-signal, edge)): records armed on `x <= @e y`
// waiting for that edge. Growable — an unbounded number can arm between two edges.
// One record's registration on one trigger list. `ids` / `n` describe the WHOLE
// set of triggers the record was registered on -- a compile-time constant array
// the backend emitted for the statement -- so that when this record matures the
// fire path can take it off the other lists. `ids` is null for the single-trigger
// case, which has nothing to remove.
struct IntraPending {
    void*           rec;
    const uint32_t* ids;
    uint32_t        n;
};

// Pending lists by trigger id. A dense (signal, edge) id indexes `intra_event_pending`;
// an RT_TRIG_EVENT id indexes `intra_event_pending_obj` by event slot, so the sparse
// high bit never sizes a vector.
std::vector<std::vector<IntraPending>> intra_event_pending;
std::vector<std::vector<IntraPending>> intra_event_pending_obj;

// The list for `id`, or null when nothing was ever registered on it. Callers must not
// hold the pointer across a call that can register or drop on another id.
std::vector<IntraPending>* intra_list_find(uint32_t id) {
    auto&          v   = (id & RT_TRIG_EVENT) != 0 ? intra_event_pending_obj : intra_event_pending;
    const uint32_t idx = id & ~static_cast<uint32_t>(RT_TRIG_EVENT);
    return idx < v.size() ? &v[idx] : nullptr;
}

std::vector<IntraPending>& intra_list_get(uint32_t id) {
    auto&          v   = (id & RT_TRIG_EVENT) != 0 ? intra_event_pending_obj : intra_event_pending;
    const uint32_t idx = id & ~static_cast<uint32_t>(RT_TRIG_EVENT);
    if (idx >= v.size()) {
        v.resize(idx + 1);
    }
    return v[idx];
}

// Drop `rec` from trigger `trig_id`'s pending list by pointer identity, keeping
// the survivors' order. Used for first-fire-wins removal across a multi-signal
// trigger: the record has matured on another of its triggers and must not be
// counted down again -- and cannot merely be MARKED, because a committed record
// returns to its site freelist and a stale pointer would later match a reused one.
void intra_event_drop(uint32_t trig_id, const void* rec) {
    std::vector<IntraPending>* list = intra_list_find(trig_id);
    if (list == nullptr) {
        return;
    }
    size_t w = 0;
    for (const IntraPending& e : *list) {
        if (e.rec != rec) {
            (*list)[w++] = e;
        }
    }
    list->resize(w);
}

// Move a record whose delay or event just completed onto its target's ready list, in
// ascending seq so the commit applies same-instant updates last-executed-wins (LRM
// 10.4.2). Same-instant counts are small, so the linear insert is cheap.
//
// Never before a plain NBA copied onto the list: that write executed after every
// record ahead of it matured, so it is later than them (LRM 4.6 b). A record that
// matures after it commits after it, as every record always did after the plain NBAs
// of its instant; the LRM text does not say which is later there (docs/design/
// intra-assignment-timing.md, "Order against plain NBAs").
void intra_ready_push(rt_intra_rec* rec) {
    IntraSite&          site  = intra_sites[rec->idx];
    std::vector<void*>& ready = site.ready;
    size_t              pos   = ready.size();
    while (pos > site.plain_end && reinterpret_cast<rt_intra_rec*>(ready[pos - 1])->seq > rec->seq) {
        --pos;
    }
    ready.insert(ready.begin() + static_cast<std::ptrdiff_t>(pos), rec);
    rt_intra_total++;
}

} // namespace

extern "C" void* rt_intra_alloc(uint32_t idx, uint32_t stride) {
    if (idx >= intra_sites.size()) {
        intra_sites.resize(idx + 1);
    }
    IntraSite& site = intra_sites[idx];
    if (site.stride != stride) {
        // A different design (or a differently monomorphized signal) now owns this
        // dense id: its records are a different size, so nothing pooled here can be
        // reused. Within one design the stride never changes, so this fires only at
        // a run boundary -- where rt_intra_reset has already emptied `ready`, making
        // the drop safe. Without it, a run whose record is wider than the previous
        // run's silently writes past the end of a recycled block; an embedder that
        // runs two designs in one process (the test runner does) hits it.
        rt_intra_total -= static_cast<uint32_t>(site.ready.size());
        site.ready.clear();
        site.plain_end = 0;
        site.freelist.clear();
        site.pool.clear();
        site.stride = stride;
    }
    void* p;
    if (!site.freelist.empty()) {
        p = site.freelist.back();
        site.freelist.pop_back();
    } else {
        site.pool.push_back(std::make_unique<unsigned char[]>(stride));
        p = site.pool.back().get();
    }
    // Stamp arm order so the commit applies same-instant updates last-executed-wins.
    reinterpret_cast<rt_intra_rec*>(p)->seq = intra_seq++;
    return p;
}

extern "C" void rt_intra_cb(rt_timer* timer, void* /*ctx*/) {
    // The record base is the timer (timer at offset 0), so recover it by cast.
    intra_ready_push(reinterpret_cast<rt_intra_rec*>(timer));
}

extern "C" void rt_intra_now(void* rec) {
    // The record's update event is created by the statement that filled it, so it is
    // later than everything already on the list (its seq is the newest stamp) and
    // earlier than anything that matures from here on.
    IntraSite& site = intra_sites[static_cast<rt_intra_rec*>(rec)->idx];
    site.ready.push_back(rec);
    site.plain_end = site.ready.size();
    rt_intra_total++;
}

extern "C" void* rt_inertial_buf(uint32_t idx, uint32_t nbytes) {
    if (idx >= inertial_bufs.size()) {
        inertial_bufs.resize(idx + 1);
        inertial_buf_bytes.resize(inertial_bufs.size(), 0);
    }
    if (!inertial_bufs[idx] || inertial_buf_bytes[idx] < nbytes) {
        // Same process-global-outlives-a-design hazard as rt_intra_alloc: the buffer
        // is sized to the site's monomorphized pending-value width, and the next
        // design reuses the dense id for a possibly wider one.
        inertial_bufs[idx]     = std::make_unique<unsigned char[]>(nbytes);
        inertial_buf_bytes[idx] = nbytes;
    }
    return inertial_bufs[idx].get();
}

extern "C" uint64_t rt_inertial_decide(uint32_t idx, uint32_t same_as_pending, uint32_t same_as_cur) {
    if (idx >= inertial_states.size()) {
        inertial_states.resize(idx + 1);
    }
    InertialState& s = inertial_states[idx];
    // LRM 10.3.3 step b: if the new RHS equals the value already scheduled to
    // propagate, keep the existing timer running — do not reschedule.
    if (s.pending && same_as_pending) {
        return 0;
    }
    // Any other change deschedules the pending store: bump the epoch so a stale
    // record still on the wheel is dropped when it fires (rt_inertial_cb).
    s.epoch++;
    // LRM 10.3.3 step c: the new RHS already equals the current net value, so
    // schedule nothing (and there is no longer a pending store).
    if (same_as_cur) {
        s.pending = false;
        return 0;
    }
    // LRM 10.3.3 step d: arm a fresh store; the caller stamps this epoch and
    // stores the new planes into rt_inertial_buf's buffer.
    s.pending = true;
    return s.epoch;
}

extern "C" void rt_inertial_cb(rt_timer* timer, void* /*ctx*/) {
    auto* rec = reinterpret_cast<rt_intra_rec*>(timer);
    if (rec->idx < inertial_states.size() && rec->epoch == inertial_states[rec->idx].epoch) {
        intra_ready_push(rec); // still current -> commit this instant (pre-active for CA)
        return;
    }
    // Descheduled by a later RHS change: reclaim the record unfired.
    intra_sites[rec->idx].freelist.push_back(rec);
}
namespace {

// -- Detached event subscriptions: `->> @(x) e` (LRM 15.5.1) --
//
// A one-shot registration keyed by the same dense (signal, edge) trigger id the
// intra-assignment event mechanism uses. Growable, because the number of live
// registrations is what a static wait bucket cannot bound -- a `->> @(x) e` in a
// loop makes one per pass, all waiting on the same edge.
//
// Each record is a handle plus a countdown, so `->> repeat(n) @(x) e` is the same
// thing with n > 1: the edge decrements, and only the record reaching zero fires.

struct EventDetach {
    uint64_t handle;
    uint32_t count;
};

// Indexed like intra_event_pending: dense (signal, edge) ids in one vector, event-object
// ids (RT_TRIG_EVENT | slot) in the other by slot.
std::vector<std::vector<EventDetach>> event_detach_pending;
std::vector<std::vector<EventDetach>> event_detach_pending_obj;
std::vector<uint64_t>                 event_detach_ready;

std::vector<EventDetach>* detach_list_find(uint32_t id) {
    auto&          v   = (id & RT_TRIG_EVENT) != 0 ? event_detach_pending_obj : event_detach_pending;
    const uint32_t idx = id & ~static_cast<uint32_t>(RT_TRIG_EVENT);
    return idx < v.size() ? &v[idx] : nullptr;
}

std::vector<EventDetach>& detach_list_get(uint32_t id) {
    auto&          v   = (id & RT_TRIG_EVENT) != 0 ? event_detach_pending_obj : event_detach_pending;
    const uint32_t idx = id & ~static_cast<uint32_t>(RT_TRIG_EVENT);
    if (idx >= v.size()) {
        v.resize(idx + 1);
    }
    return v[idx];
}

} // namespace

extern "C" void rt_event_detach_register(uint32_t trig_id, uint64_t handle, uint32_t count) {
    detach_list_get(trig_id).push_back(EventDetach{ handle, count == 0 ? 1u : count });
}

// The edge fired: decrement every record on this id, move the matured ones to the
// ready list and compact the rest. Returns how many matured.
extern "C" uint32_t rt_event_detach_fire(uint32_t trig_id) {
    event_detach_ready.clear();
    std::vector<EventDetach>* found = detach_list_find(trig_id);
    if (found == nullptr) {
        return 0;
    }
    auto&  list = *found;
    size_t w    = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (--list[i].count == 0) {
            event_detach_ready.push_back(list[i].handle);
        } else {
            list[w++] = list[i];
        }
    }
    list.resize(w);
    return static_cast<uint32_t>(event_detach_ready.size());
}

extern "C" uint64_t rt_event_detach_ready(uint32_t i) {
    return i < event_detach_ready.size() ? event_detach_ready[i] : 0;
}

extern "C" void rt_intra_event_register(uint32_t trig_id, void* rec) {
    intra_list_get(trig_id).push_back(IntraPending{ rec, nullptr, 0 });
}

extern "C" void rt_intra_event_register_group(const uint32_t* ids, uint32_t n, void* rec) {
    for (uint32_t k = 0; k < n; ++k) {
        // Two listed events can be the SAME object (LRM 15.5.5.1: `event b = a;`, or
        // `b = a`), and one occurrence must count the record down once, not once per
        // name. The second entry would wrap its countdown below zero and later mature a
        // reused record of the same target early.
        bool seen = false;
        for (uint32_t j = 0; j < k; ++j) {
            seen = seen || ids[j] == ids[k];
        }
        if (!seen) {
            intra_list_get(ids[k]).push_back(IntraPending{ rec, ids, n });
        }
    }
}

extern "C" void rt_intra_event_fire(uint32_t trig_id) {
    if (intra_list_find(trig_id) == nullptr) {
        return;
    }
    size_t w = 0; // survivors compacted in place
    // Indexed, and the list re-found each step: a maturing multi-trigger record drops
    // itself from its OTHER lists, and while that never touches this one (the removal
    // skips trig_id), a reference held across it would depend on which vector got
    // touched. Re-finding keeps the invariant local.
    for (size_t i = 0; i < intra_list_find(trig_id)->size(); ++i) {
        const IntraPending e   = (*intra_list_find(trig_id))[i];
        auto*              rec = reinterpret_cast<rt_intra_rec*>(e.rec);
        if (--rec->count != 0) {
            (*intra_list_find(trig_id))[w++] = e; // more edges to wait for
            continue;
        }
        intra_ready_push(rec); // last awaited edge -> commit this instant
        // First fire wins: this record is done, so take it off every OTHER trigger
        // it was listed on. Skipping `trig_id` is what keeps the loop above stable.
        for (uint32_t k = 0; k < e.n; ++k) {
            if (e.ids[k] != trig_id) {
                intra_event_drop(e.ids[k], e.rec);
            }
        }
    }
    intra_list_find(trig_id)->resize(w);
}

extern "C" void* rt_intra_ready_base(uint32_t idx) {
    return idx < intra_sites.size() ? intra_sites[idx].ready.data() : nullptr;
}

extern "C" uint32_t rt_intra_ready_count(uint32_t idx) {
    return idx < intra_sites.size() ? static_cast<uint32_t>(intra_sites[idx].ready.size()) : 0;
}

extern "C" uint32_t rt_intra_any(void) {
    return rt_intra_total;
}

extern "C" void rt_intra_release(uint32_t idx) {
    if (idx >= intra_sites.size()) {
        return;
    }
    IntraSite& site = intra_sites[idx];
    rt_intra_total -= static_cast<uint32_t>(site.ready.size());
    for (void* p : site.ready) {
        site.freelist.push_back(p);
    }
    site.ready.clear();
    site.plain_end = 0;
}

// The pair for a site whose records stage a class handle (an aggregate or an array with a
// handle leaf, LRM 9.4.5). Until its delay elapses a record may hold the ONLY reference to
// the object it carries -- the program can drop every other one in between -- and a record
// is native memory no root table describes, so from alloc to release it is a conservative
// GC root. Everything else is rt_intra_alloc / rt_intra_release.
//
// A record still in flight when the run ends is never released here; its range goes with
// the rest of the heap's registrations at rt_gc_shutdown (Gc.cpp, release_heap).
extern "C" void* rt_intra_alloc_rooted(uint32_t idx, uint32_t stride) {
    void* rec = rt_intra_alloc(idx, stride);
    rt_gc_range_add(rec, stride);
    return rec;
}

extern "C" void rt_intra_release_rooted(uint32_t idx) {
    if (idx < intra_sites.size()) {
        for (void* rec : intra_sites[idx].ready) {
            rt_gc_range_remove(rec);
        }
    }
    rt_intra_release(idx);
}

extern "C" void rt_intra_reset(void) {
    // A fresh simulation has nothing scheduled, so every pooled record is free —
    // including any this-process left armed on the wheel at a prior $finish.
    for (IntraSite& site : intra_sites) {
        site.freelist.clear();
        for (const auto& rec : site.pool) {
            site.freelist.push_back(rec.get());
        }
        site.ready.clear();
        site.plain_end = 0;
    }
    // `stride` is deliberately kept: it is what lets the next design's first
    // rt_intra_alloc notice that this site's pooled records are the wrong size and
    // retire them, instead of filling a narrower block.
    for (std::vector<IntraPending>& pend : intra_event_pending) {
        pend.clear();
    }
    for (std::vector<IntraPending>& pend : intra_event_pending_obj) {
        pend.clear();
    }
    // A fresh simulation has no pending inertial store and starts at epoch 0.
    for (InertialState& s : inertial_states) {
        s = {};
    }
    rt_intra_total = 0;
    intra_seq   = 0;
}

extern "C" void rt_wheel_init(rt_wheel* wheel) {
    new (wheel->storage) RuntimeWheel;
}

extern "C" void rt_wheel_deinit(rt_wheel* wheel) {
    unwrap(wheel)->~RuntimeWheel();
}

extern "C" uint64_t rt_wheel_now(const rt_wheel* wheel) {
    return unwrap(wheel)->now();
}

extern "C" void rt_wheel_schedule(rt_wheel* wheel, rt_timer* timer, uint64_t deadline, rt_timer_fn on_expire) {
    unwrap(wheel)->schedule(unwrap(timer), deadline, on_expire);
}

extern "C" void rt_wheel_schedule_rel(rt_wheel* wheel, rt_timer* timer, uint64_t delay, rt_timer_fn on_expire) {
    // now + delay computed runtime-side so a #delay crosses the ABI once, not
    // twice (rt_wheel_now then rt_wheel_schedule).
    //
    // The sum saturates at the last instant, 2**64 - 1: a delay that would land past
    // the end of time is never reached, and wrapping it would fire it in the past
    // (a delay of 2**64 - 1 at time 5 would fire at 4). A saturated timer fires at
    // the last instant, after everything else. At the last instant itself nothing
    // lies ahead, so the timer is not armed and its waiter stays parked. Only the
    // overflow test is on the hot path: it is the carry of the add.
    RuntimeWheel&  w        = *unwrap(wheel);
    const uint64_t now      = w.now();
    uint64_t       deadline = now + delay;
    if (deadline < now) [[unlikely]] {
        if (now == UINT64_MAX) {
            return;
        }
        deadline = UINT64_MAX;
    }
    w.schedule(unwrap(timer), deadline, on_expire);
}

extern "C" void rt_wheel_tick(rt_wheel* wheel, void* ctx) {
    unwrap(wheel)->tick(ctx);
}
namespace {

// Why rt_wheel_advance refused to start the next time step, for rt_sim_run to tell
// the ways a run ends from outside the design apart.
enum class Halt : uint8_t { None, StopTime, Interrupt };

Halt g_halt = Halt::None;

// The stop tick of "no stop time armed". An armed stop tick is clamped below it
// (rt_run), so it is never confused with one. The last instant is a time a step can
// reach (a delay past the end of time saturates there, rt_wheel_schedule_rel), so
// "no tick can reach it" is not what makes it a sentinel.
constexpr uint64_t kNoStopTick = RuntimeWheel::never;

// The last tick that runs once `--stop-time` has armed one (rt_run).
uint64_t g_stop_tick = kNoStopTick;

// Set by the SIGINT handler rt_sim_run installs and read at the time-step
// boundary. volatile sig_atomic_t is the one type the language lets a signal
// handler write.
volatile std::sig_atomic_t g_interrupt = 0;

} // namespace

extern "C" int rt_wheel_advance(rt_wheel* wheel, void* ctx) {
    RuntimeWheel& w = *unwrap(wheel);
    // The boundary between two time steps: the one that just settled is complete
    // and sampled, and nothing of the next has run. It is the only point a run can
    // be ended from outside the design without leaving a step half done, so it is
    // where an interrupt and the stop time are acted on. A run that uses neither
    // pays two predictable tests per step.
    if (g_interrupt != 0) [[unlikely]] {
        g_halt = Halt::Interrupt;
        return 0;
    }
    if (g_stop_tick != kNoStopTick) [[unlikely]] {
        // Every event at or before the stop tick runs. When the next one is past
        // it, the run ends at the stop tick itself rather than at the last step
        // that ran, so the finals and the report read the time the user asked for.
        // An idle wheel has no next step: the design ended first, and that is
        // not a stop. `never` is what next_tick reports for an idle wheel but also
        // the tick of a timer at the last instant, so idleness is asked of empty().
        // That runs only when the next step is past the stop tick.
        const uint64_t next = w.next_tick();
        if (next > g_stop_tick && !w.empty()) {
            w.skip_to(g_stop_tick);
            g_halt = Halt::StopTime;
            return 0;
        }
    }
    return w.advance(ctx) ? 1 : 0;
}
namespace rt_internal {

// Where rt_finish returns control: the run_jit frame that started the
// simulation, or null when the design runs standalone.
// Append the simulation time `now` (in design ticks) as "<value> <unit>",
// reduced to the largest SI unit that divides it evenly (fs / ps / ns / us / ms
// / s). A unitless design (tick_fs == 0) prints the raw tick count. Used only by
// the $finish banner, so a plain best-effort rendering — not the %t format.
void append_finish_time(std::string& out, uint64_t now) {
    const uint64_t tick_fs = timeformat.tick_fs;
    if (tick_fs == 0) {
        out += fmt::to_string(now);
        return;
    }
    // Total femtoseconds; 128-bit to avoid overflow on a coarse precision.
    const unsigned __int128 fs = static_cast<unsigned __int128>(now) * tick_fs;
    static const struct {
        uint64_t    div;
        const char* unit;
    } table[] = { { 1000000000000000ull, "s" }, { 1000000000000ull, "ms" },
                  { 1000000000ull, "us" },       { 1000000ull, "ns" },
                  { 1000ull, "ps" },             { 1ull, "fs" } };
    // At now == 0 every unit divides; pick the design precision's unit so the
    // output reads "0 <precision-unit>" rather than "0 s".
    const unsigned __int128 pick = fs == 0 ? tick_fs : fs;
    for (const auto& e : table) {
        if (pick % e.div == 0) {
            out += fmt::to_string(static_cast<uint64_t>(fs / e.div));
            out += ' ';
            out += e.unit;
            return;
        }
    }
}

} // namespace rt_internal
namespace {

// Peak resident set (in kilobytes) and total process CPU time, for the
// $finish(2) resource line. Normalizing the unit here matters: getrusage
// reports ru_maxrss in kilobytes on Linux but in *bytes* on the BSDs (macOS
// included), so reading the field raw would overstate macOS by 1024x. Windows
// has no getrusage at all and goes through the Win32 equivalents.
struct FinishStats {
    uint64_t peak_rss_kb;
    double   cpu_seconds;
};

FinishStats finish_stats() {
#if defined(_WIN32)
    // FILETIME counts 100-nanosecond ticks; the creation/exit pair is unused.
    FILETIME creation = {}, exited = {}, kernel = {}, user = {};
    double   cpu = 0.0;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user)) {
        const auto ticks = [](const FILETIME& ft) {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        cpu = static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
    }
    PROCESS_MEMORY_COUNTERS pmc = {};
    uint64_t                rss_kb = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        rss_kb = static_cast<uint64_t>(pmc.PeakWorkingSetSize) / 1024;
    }
    return {rss_kb, cpu};
#else
    rusage ru = {};
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
                       static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
#if defined(__APPLE__)
    const uint64_t rss_kb = static_cast<uint64_t>(ru.ru_maxrss) / 1024;
#else
    const uint64_t rss_kb = static_cast<uint64_t>(ru.ru_maxrss);
#endif
    return {rss_kb, cpu};
#endif
}

} // namespace

unsigned char rt_finishing = 0;
namespace {

// Everything a $finish does before control leaves: the Observed drain, the
// banner, and closing the dump.
void finish_report(const char* banner, uint64_t now, int32_t arg) {
    // $finish / $stop: print the exit banner (LRM Table 20-1), then end the
    // simulation. arg 0 prints nothing; 1 prints time + location; 2 adds a
    // resource-usage line. The banner is a simulator diagnostic (not design
    // output), so it goes to stderr, keeping it out of the design's stdout data
    // stream. LRM Table 20-1 fixes the content (time + location) but not the
    // stream; iverilog puts it on stdout, but per PRINCIPLES.md strict equivalence
    // on LRM-unspecified behavior is not required, and stderr keeps captured
    // $display output clean. Flush stdout first so any buffered $display text
    // is committed before the banner.
    //
    // $finish ends the current time step, so drain any Observed-region case
    // violations queued this instant before we stop — the sim loop's own
    // Observed flush at the settled point is never reached on this path.
    // Deferred immediate assertion reports (LRM 16.4) are deliberately NOT
    // flushed here: they mature only IN the Observed / Postponed region, so a
    // $finish that preempts that region discards the un-matured reports, which
    // is what the LRM 16.4.1 maturation model implies.
    rt_observed_flush();
    if (arg >= 1) {
        std::fflush(stdout);
        std::string line = banner;
        line += " called at ";
        append_finish_time(line, now);
        if (arg >= 2) {
            const FinishStats st = finish_stats();
            char              stats[128];
            std::snprintf(stats, sizeof(stats),
                          "\n  Memory: %" PRIu64 " KB  CPU time: %.3f seconds", st.peak_rss_kb,
                          st.cpu_seconds);
            line += stats;
        }
        line += '\n';
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }
    // Flush and close the VCD while the design's stack (state + wheel the engine
    // points at) is still live: the drain stops before its own close at
    // wheel-drain. A no-op when no dump is open.
    rt_vcd_close();
}

} // namespace

extern "C" void rt_finish_coop(const char* banner, uint64_t now, int32_t arg) {
    finish_report(banner, now, arg);
    rt_finishing = 1;
    // Returns. The caller is a JIT'd body, which leaves through its own
    // epilogue; each frame between here and the drain loop is released by its
    // owner, and the drain sees rt_finishing and stops.
}

extern "C" void rt_oscillation_abort(const rt_wheel* wheel) {
    // Read `now` from the wheel here rather than materializing it in the JIT'd
    // drain loop: this abort is a cold error path, and keeping rt_wheel_now out
    // of the generated IR preserves the delay path's single-ABI-crossing shape
    // (rt_wheel_schedule_rel only; see the drain-loop guard).
    const uint64_t now = rt_wheel_now(wheel);
    // A single time step activated more than the delta-cycle limit of processes
    // without settling: a zero-delay oscillation the scheduler cannot converge
    // (e.g. two `always @*` blocks writing a shared variable each is sensitive
    // to). clotho's per-store wake semantics do not coalesce transient writes the
    // way a net-change scheduler would, so such a design fails to converge.
    // Terminate loudly instead of hanging (a diagnostic, not a design value).
    std::fflush(stdout);
    std::string line = "FATAL: simulation did not converge at ";
    append_finish_time(line, now);
    line += " (zero-delay oscillation: delta-cycle activation limit exceeded)\n";
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
    rt_vcd_close();
    rt_finishing = 1;
    // A non-converging design cannot be handed back to the host mid-instant --
    // there is no settled state to return -- so this one path still ends the
    // process. std::exit, not a longjmp: it is portable, and it keeps the
    // nonzero status that distinguishes a hang from a clean run.
    std::exit(1);
}

extern "C" void rt_ring_overflow(uint32_t ring, uint32_t capacity) {
    // The scheduler's queues are sized statically from the design
    // (ensure_active_queue, wait_counts): one slot per process, plus one per
    // concurrently-live `fork` child. Reaching this means that count was too small
    // -- an unbounded shape the static bound saturates on (recursion into a forking
    // task), or a bug in the bound itself. The alternative to reporting is a store
    // past the end of the buffer, overwriting a live coroutine handle or whatever
    // global follows it, which corrupts the scheduler and surfaces as a hang or a
    // wrong answer with no diagnostic.
    //
    // Selector order matches the kRing* constants in src/ir/llvm/Backend.h.
    static constexpr const char* kNames[] = {
        "active scheduler ring", "reactive scheduler ring", "inactive (#0) batch",
        "reactive inactive (#0) batch", "event wait queue",
    };
    const uint64_t now = rt_diag_now();
    std::fflush(stdout);
    std::string line = "FATAL: the ";
    line += ring < std::size(kNames) ? kNames[ring] : "scheduler queue";
    line += " overflowed at ";
    append_finish_time(line, now);
    line += fmt::format(" (capacity {} entries)\n", capacity);
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_oscillation_abort: there is no settled state to hand
    // back mid-instant, so this path ends the process with a nonzero status.
    std::exit(1);
}

// LRM 9.7: "it shall be an error to call this task on the current process, i.e., a
// process cannot wait for its own completion." Dynamic, because the target is a
// handle value -- so this is the report, laid out like rt_null_handle.
extern "C" void rt_proc_await_self(const rt_null_site* site, uint64_t now) {
    io_ensure_init();
    std::fflush(stdout);

    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));

    std::string when;
    append_finish_time(when, now);

    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " process::await() on the current process");
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column), '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " the handle names the thread that is awaiting at {}, which can never "
                             "complete while it waits",
               when);
    fmt::print(stderr, "\n");
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " LRM 9.7 makes awaiting the current process an error");
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    std::exit(1);
}
namespace {

// ---- Deferred immediate assertion reports (LRM 16.4) ----
//
// A deferred assertion's action (a single severity system task, or the default
// $error) is rendered here when the assertion executes in the Active region —
// its arguments are sampled at that instant (LRM 16.4.1) — but its emission is
// delayed: an observed (`#0`) report matures + fires in the Observed region, a
// final report in the Postponed region. Each queued report is tagged with a
// per-process generation counter address and the counter's value at queue time.
// The owning process bumps its generation at every activation (a flush point,
// LRM 16.4.2), so a report left over from a superseded activation has a stale
// generation and is dropped at maturation instead of firing — this is the
// glitch suppression the construct exists for. All same-mode reports leave the
// queue at their maturation region regardless (fired if current, dropped if
// stale), so the queue never carries a report across time steps.
struct DeferReport {
    std::string     line;     // fully rendered report line, newline included
    const uint64_t* gen;      // owning process's generation counter
    uint64_t        gen_val;  // generation at queue time
    bool            is_final; // final (Postponed) vs observed (#0, Observed)
};
std::vector<DeferReport> g_defer;

} // namespace

extern "C" void rt_defer_report(int32_t level, uint64_t is_final, const void* gen, uint64_t gen_val,
                                const char* banner, const char* fmt, uint32_t argc,
                                const rt_arg* argv) {
    io_ensure_init();
    // Rendered, and stamped with the time, now: the arguments are sampled at the
    // point the assertion executes, and maturation is in the same instant.
    std::string line = severity_line(level, banner, fmt, argc, argv);
    g_defer.push_back(
      { std::move(line), static_cast<const uint64_t*>(gen), gen_val, is_final != 0 });
}

// Mature the queued reports of one mode: fire those whose process generation is
// still current (not superseded by a later activation) and drop the rest; every
// same-mode report leaves the queue. `final_pass` selects final vs observed.
static void defer_mature(bool final_pass) {
    if (g_defer.empty()) {
        return;
    }
    std::fflush(stdout);
    std::vector<DeferReport> keep;
    bool                     wrote = false;
    for (DeferReport& r : g_defer) {
        if (r.is_final != final_pass) {
            keep.push_back(std::move(r));
            continue;
        }
        if (r.gen == nullptr || *r.gen == r.gen_val) {
            std::fputs(r.line.c_str(), stderr);
            wrote = true;
        }
    }
    g_defer.swap(keep);
    if (wrote) {
        std::fflush(stderr);
    }
}

extern "C" void rt_defer_flush_observed() {
    defer_mature(/*final_pass=*/false);
}

extern "C" void rt_defer_flush_final() {
    defer_mature(/*final_pass=*/true);
}
namespace {

// Return the process-global simulation state to its power-on condition. Called
// at the end of every run, so the next simulation in this process starts clean.
//
// This matters because several of these outlive the JIT session that created
// their contents: g_monitor.ramp is a function pointer INTO the JIT'd code, and
// g_strobe_queue holds coroutine handles. run_jit destroys the LLJIT when it
// returns, so anything left here dangles, and the next simulation's Postponed
// drain would call or resume it. Only a host that runs more than one simulation
// per process can observe that -- the CLI runs exactly one -- but clotho_test
// runs it a thousand times over.
void sim_teardown() {
    g_monitor = MonitorState{};
    g_strobe_queue.clear();
    g_strobe_read = 0;
    g_observed_case.clear();
    g_defer.clear();
    rt_nba_arr_reset();
    rt_intra_reset();
    // A stop time belongs to the run it was armed for.
    g_stop_tick = kNoStopTick;
    // Functional coverage (LRM 19): drop the model and every instance now that
    // the report is written. The model is emitted as JIT-owned constant data, so
    // holding a pointer to it past the run would dangle the moment the host tears
    // the JIT down -- and a NEXT design in the same process, one with no
    // covergroup and therefore no rt_cover_init, would otherwise inherit this
    // one's model and re-report it. The CLI runs one simulation per process;
    // clotho_test runs thousands.
    rt_cover_shutdown();
}

void on_sigint(int) {
    g_interrupt = 1;
}

// Catches SIGINT for the span of one drain, so that Ctrl-C ends the run at the
// next time-step boundary (rt_wheel_advance) instead of killing it mid-step with
// a dump half written. The handler only sets a flag: nothing else a simulator
// does is async-signal-safe.
//
// The handler is one-shot on purpose. A design that never reaches a boundary, a
// process spinning with no timing control, would otherwise leave Ctrl-C with
// nothing to do, so the SECOND SIGINT meets the default action and ends the
// process at once. SA_RESETHAND gives POSIX that; the Windows C runtime resets a
// signal() handler itself before calling it.
//
// The previous disposition comes back when the guard ends, because the JIT also
// runs inside the test runner and the language server, whose handlers must not
// be left replaced. A SIGINT that was ignored on entry (a command a
// non-interactive shell put in the background) stays ignored.
class InterruptGuard {
public:
    InterruptGuard() {
        g_interrupt = 0;
#if defined(_WIN32)
        previous_ = std::signal(SIGINT, on_sigint);
        if (previous_ == SIG_ERR) {
            return;
        }
        if (previous_ == SIG_IGN) {
            std::signal(SIGINT, SIG_IGN);
            return;
        }
        installed_ = true;
#else
        struct sigaction current = {};
        if (sigaction(SIGINT, nullptr, &current) != 0 || current.sa_handler == SIG_IGN) {
            return;
        }
        struct sigaction act = {};
        act.sa_handler       = on_sigint;
        act.sa_flags         = SA_RESETHAND | SA_RESTART;
        sigemptyset(&act.sa_mask);
        installed_ = sigaction(SIGINT, &act, &previous_) == 0;
#endif
    }

    ~InterruptGuard() {
        if (!installed_) {
            return;
        }
#if defined(_WIN32)
        std::signal(SIGINT, previous_);
#else
        sigaction(SIGINT, &previous_, nullptr);
#endif
    }

    InterruptGuard(const InterruptGuard&)            = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

private:
#if defined(_WIN32)
    void (*previous_)(int) = SIG_DFL;
#else
    struct sigaction previous_ = {};
#endif
    bool installed_ = false;
};

// "clotho: <what> <time>" on stderr: the simulator saying why it stopped, after
// the design's own buffered output. The time is the one the run stopped at.
void report_halt(const char* what) {
    std::fflush(stdout);
    std::string line = what;
    append_finish_time(line, rt_diag_now());
    line += '\n';
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

// The exit status of the last run, which rt_run returns.
int32_t g_run_status = 0;

// The exit status of an interrupted run: 128 + SIGINT, which is what a shell
// reports for a process that SIGINT killed.
constexpr int32_t kInterruptedStatus = 130;

} // namespace

extern "C" void rt_sim_run(void* state, void* wheel, void (*drain)(void*, void*),
                           void (*finals)(void*, void*)) {
    // Run the simulation, then the `final` blocks (LRM 9.2.3), so the finals see
    // the settled design state on every exit path. `drain` returns normally
    // whether it ran out of work, a $finish stopped it or the run was ended from
    // outside the design (rt_wheel_advance), so there is one path; only an
    // interrupt skips the finals.
    rt_finishing = 0; // a prior run in this process may have set it
    g_halt       = Halt::None;
    g_fatal_seen = false;
    // Publish the wheel so a cold diagnostic (an allocation failure) can report
    // the simulation time without every allocating call carrying one.
    g_diag_wheel = static_cast<const rt_wheel*>(wheel);
    {
        const InterruptGuard sigint;
        drain(state, wheel);
    }
    // The drain stopped at a time-step boundary, so the design's state is settled
    // and the dump has every step that ran, whichever way it ended.
    const Halt halt = g_halt;
    if (halt == Halt::Interrupt) {
        // The user asked for the run to end, not for it to wrap up: no finals.
        report_halt("clotho: interrupted at ");
    } else {
        if (halt == Halt::StopTime) {
            // As a $finish at the stop time would, the finals follow the report.
            report_halt("clotho: stopped at --stop-time ");
        }
        finals(state, wheel);
    }
    // Functional coverage (LRM 19.9: the database is written "at the end of a
    // simulation run"). Here rather than from a synthesized `final` process
    // because every exit path -- natural drain, $finish, interrupt, stop time --
    // passes through this one point, and because the report must not be able to
    // observe a design that a `final` block is still changing. A design with no
    // covergroup writes nothing: rt_cover_report returns immediately on an empty
    // model. An interrupted run reports what it covered so far.
    rt_cover_report();
    if (halt != Halt::None) {
        // The dump is closed by the drain; this is every other open file, which the
        // process would flush on exit but a host that keeps running would not.
        std::fflush(nullptr);
    }
    g_run_status = halt == Halt::Interrupt ? kInterruptedStatus : (g_fatal_seen ? 1 : 0);
    g_diag_wheel = nullptr;
    sim_teardown();
}
namespace {

// Coroutine frames may be over-aligned (16-byte plane spills) and malloc
// guarantees only 16, so allocate with an explicit alignment (Os.h). rt_alloc
// returns null on failure rather than throwing: it is called from JIT'd code
// across a C ABI with no unwind tables.
constexpr size_t kFrameAlign = 64;

// A process or event pool chunk. The pools have no way to hand a failed
// allocation back to the design, so running out of memory here ends the run.
void* alloc_pool_chunk(size_t bytes) {
    void* chunk = clotho::rt::os_aligned_alloc(bytes, kFrameAlign);
    if (chunk == nullptr) {
        format_fatal(fmt::format("out of memory: the system refused {} bytes for the scheduler's pools", bytes));
    }
    return chunk;
}

// -- Named event objects (LRM 15.5) --
//
// One slot space over two tiers: the backend's static block is chunk 0 (its slots
// are the scalar declared events, so their addresses fold to constants at compile
// time), and every other event storage location -- an array element, a struct
// member, a class property, an automatic local, a container element -- draws from
// the chunks after it. Chunks are never resized or moved, so an object address
// stays valid across growth even while a process is parked on its waiter list.

constexpr uint32_t kEventChunk = 256;

// chunk 0 is the backend's static block, of g_event_static objects; every later
// chunk holds kEventChunk pool objects.
std::vector<rt_event*> g_event_chunks;
uint32_t               g_event_static = 0;
uint32_t               g_event_pooled = 0; // pool objects handed out so far

} // namespace

extern "C" void* rt_event_ptr(uint32_t slot) {
    if (slot < g_event_static) {
        return g_event_chunks[0] + slot;
    }
    const uint32_t i = slot - g_event_static;
    return g_event_chunks[1 + (i / kEventChunk)] + (i % kEventChunk);
}

extern "C" uint64_t rt_event_new(void) {
    const uint32_t i = g_event_pooled++;
    if (i % kEventChunk == 0) {
        // A fresh chunk, zeroed: waiters / pending / next_pend all start clear.
        g_event_chunks.push_back(static_cast<rt_event*>(alloc_pool_chunk(sizeof(rt_event) * kEventChunk)));
        std::memset(g_event_chunks.back(), 0, sizeof(rt_event) * kEventChunk);
    }
    rt_event* e  = g_event_chunks[1 + (i / kEventChunk)] + (i % kEventChunk);
    e->last_trig = ~uint64_t{ 0 }; // never triggered; `.triggered` reads 0
    e->vcd       = -1;             // an object reached through a handle is not dumped
    // The slot is past the static block, and gen is 0 for the run: nothing is ever
    // freed (LRM 15.5.5.2 reclamation is GC), so a slot is never reused and its
    // generation never advances.
    return static_cast<uint64_t>(g_event_static + i);
}

extern "C" uint32_t rt_event_trig_id(const void* obj) {
    const auto* p = static_cast<const rt_event*>(obj);
    if (p >= g_event_chunks[0] && p < g_event_chunks[0] + g_event_static) {
        return RT_TRIG_EVENT | static_cast<uint32_t>(p - g_event_chunks[0]);
    }
    for (size_t c = 1; c < g_event_chunks.size(); ++c) {
        if (p >= g_event_chunks[c] && p < g_event_chunks[c] + kEventChunk) {
            const auto within = static_cast<uint32_t>(p - g_event_chunks[c]);
            return RT_TRIG_EVENT | (g_event_static + static_cast<uint32_t>(c - 1) * kEventChunk + within);
        }
    }
    return RT_TRIG_EVENT; // unreachable for an object the pool handed out
}
namespace {

// -- Process control blocks (the `process` class, LRM 9.7) --
//
// A chunked pool with a freelist, for the one tier that is dynamic: fork children.
// A statically created process's block lives in the backend's `clotho.proc_pcb`
// array and never comes from here (RT_PROC_STATIC marks it, so a reference count
// reaching zero on one is not a free). Chunks are never resized or moved, so a
// block address stays valid across growth while a thread is parked on it or an
// object names it. See docs/design/process-class.md §4.1.

constexpr uint32_t kProcChunk = 64;

std::vector<rt_proc*> g_proc_chunks;
uint32_t              g_proc_used = 0;        // blocks handed out of the last chunk
rt_proc*              g_proc_free = nullptr;  // freelist, threaded through next_sib

rt_proc* proc_take() {
    if (g_proc_free != nullptr) {
        rt_proc* p  = g_proc_free;
        g_proc_free = p->next_sib;
        std::memset(p, 0, sizeof(rt_proc));
        return p;
    }
    if (g_proc_chunks.empty() || g_proc_used == kProcChunk) {
        g_proc_chunks.push_back(static_cast<rt_proc*>(alloc_pool_chunk(sizeof(rt_proc) * kProcChunk)));
        std::memset(g_proc_chunks.back(), 0, sizeof(rt_proc) * kProcChunk);
        g_proc_used = 0;
    }
    return g_proc_chunks.back() + g_proc_used++;
}

// Unlink `p` from its parent's child list. Leaves `p`'s own links dangling; every
// caller either frees `p` or re-links it immediately.
void proc_unlink(rt_proc* p) {
    if (p->prev_sib != nullptr) {
        p->prev_sib->next_sib = p->next_sib;
    } else if (p->parent != nullptr) {
        p->parent->first_child = p->next_sib;
    }
    if (p->next_sib != nullptr) {
        p->next_sib->prev_sib = p->prev_sib;
    }
}

void proc_link_child(rt_proc* parent, rt_proc* child) {
    child->parent   = parent;
    child->prev_sib = nullptr;
    child->next_sib = parent != nullptr ? parent->first_child : nullptr;
    if (child->next_sib != nullptr) {
        child->next_sib->prev_sib = child;
    }
    if (parent != nullptr) {
        parent->first_child = child;
    }
}

void proc_maybe_free(rt_proc* p) {
    if (p->refs != 0 || (p->ctl & RT_PROC_STATIC) != 0) {
        return;
    }
    // Back on the freelist, threaded through next_sib. The block is memset on
    // reuse, so no field needs clearing here.
    p->next_sib = g_proc_free;
    g_proc_free = p;
}

} // namespace

// Every process-global growable pool back to power-on (declared Runtime.h:1472).
// The backend emits one call at the top of the design's main, before anything can
// allocate. Frees run before re-registrations, and the pointer that came out of
// the incoming JIT module is installed last.
//
// Why a run-START seam and not sim_teardown (:4534). Teardown runs inside
// rt_sim_run, strictly BEFORE the emitted signal deinit and rt_gc_shutdown, and a
// process object's finalizer calls rt_proc_obj_release out of exactly that later
// window; $finish leaves suspended child frames whose blocks still carry
// refs != 0; and the fatal paths (rt_oscillation_abort :3496, rt_null_handle,
// alloc_fatal :4325) std::exit(1) without ever reaching rt_sim_run's tail.
// Teardown drops what would dangle into dead JIT code the moment a run ends;
// run-init guarantees a clean start on the paths teardown never reaches. Neither
// subsumes the other, so both stay -- as does rt_sim_run's own `rt_finishing = 0`.
//
// Deliberately NOT reset here. GC state belongs to rt_gc_init (emitted right
// beside this call), which preserves g.stats on purpose so a finished run leaves
// a readable oracle -- the same run-start rule this seam generalises
// (Gc.cpp:2984-2987). VCD belongs to rt_vcd_close / vcd_shutdown_writer. And the
// host-armed configuration -- g_plusargs, g_system_access, g_ref_stale_warn -- is
// installed by the embedder BEFORE the module runs (src/ir/Jit.cpp:681/686/692),
// so touching it here would discard the run's own options.
extern "C" void rt_run_init(void* event_static_block, uint32_t event_static_count) {
    // 1. Process control blocks, first: the only pool that can hold pointers into
    // the outgoing module, since rt_proc::parent chains through blocks a dead
    // design's fork tree linked. proc_take (:5824) allocates chunks and never
    // frees them, which is the leak docs/design/process-class.md §7 accounted for.
    // There is no static first chunk to preserve: a statically created process
    // lives in the backend's clotho.proc_pcb array, is addressed by direct GEP,
    // and is kept off the freelist by RT_PROC_STATIC (Runtime.h:1520-1530), so
    // every chunk here came from this pool.
    for (rt_proc* chunk : g_proc_chunks) {
        clotho::rt::os_aligned_free(chunk);
    }
    g_proc_chunks.clear();
    g_proc_used = 0;
    g_proc_free = nullptr;

    // 2. Event objects. Chunk 0 is the PREVIOUS module's static block -- a JIT
    // global, not ours to free -- so the walk starts at 1. Re-registering chunk 0
    // alone would orphan every chunk rt_event_new (:5791) had allocated, which is
    // what this pool's reset did before it moved here: freeing 1..N is a fix.
    for (size_t i = 1; i < g_event_chunks.size(); ++i) {
        clotho::rt::os_aligned_free(g_event_chunks[i]);
    }
    g_event_chunks.clear();
    g_event_chunks.push_back(static_cast<rt_event*>(event_static_block));
    g_event_static = event_static_count;
    g_event_pooled = 0;

    // 3. Detached `->> @(x) e` subscriptions, keyed by the dense trigger id the
    // next design reuses from zero. A record surviving a $finish would let
    // rt_event_detach_fire (:2594) hand rt_event_detach_ready a handle minted by
    // the destroyed module.
    event_detach_pending.clear();
    event_detach_pending_obj.clear();
    event_detach_ready.clear();

    // 4, 5. The two queues the drain prologue also resets, conditionally
    // (docs/design/cooperative-finish.md §2). Doing it from here as well covers
    // the runs that take a fatal exit or never enter a drain at all.
    rt_nba_arr_reset();
    rt_intra_reset();

    // 6. File I/O. An unclosed $fopen holds its channel forever and the MCD space
    // is 30 wide, so ~30 leaky designs in one process make $fopen return 0 for
    // every design after them. Close what the design left open, then return both
    // tables to power-on so io_ensure_init (:539) re-primes them lazily -- which
    // requires mcd_channels[0] to end up null, since it doubles as the init flag.
    // Only the design's own streams are closed: mcd_channels[0] IS stdout and
    // fd_streams[0..2] are stdin/stdout/stderr (:529-535), and closing those would
    // take the process's own output down with them.
    for (int b = 1; b < 31; ++b) {
        if (mcd_channels[b] != nullptr) {
            std::fclose(mcd_channels[b]);
        }
    }
    mcd_channels = {};
    for (size_t i = 3; i < fd_streams.size(); ++i) {
        if (fd_streams[i] != nullptr) {
            std::fclose(fd_streams[i]);
        }
    }
    fd_streams.clear();

    // 7. Per-site diagnostic suppression. Both are keyed on the addresses of JIT
    // globals, so carrying them across runs grows them without bound AND charges a
    // new design's first report against a dead design's count.
    g_bounds_hits.clear();
    g_ref_stale_seen.clear();

    // 8. The unseeded $random stream, back to the 0 that :4904-4906 says makes an
    // unseeded run draw the same sequence every time -- which held only for the
    // first design in a process, since rt_random (:4937) mutates the state in
    // place and nothing else ever writes it.
    g_random_seed = 0;
}

extern "C" void* rt_proc_alloc(void* parent) {
    rt_proc* p = proc_take();
    p->ctl     = RT_PROC_RUNNING;
    p->refs    = 1; // the live thread
    proc_link_child(static_cast<rt_proc*>(parent), p);
    return p;
}

extern "C" void* rt_proc_mark_kill(void* pcb, uint32_t with_self) {
    auto*    root  = static_cast<rt_proc*>(pcb);
    rt_proc* chain = nullptr;
    // Depth-first over the subtree, using `parent` to backtrack rather than a stack:
    // the tree is arbitrarily deep (a UVM phase forks per component) and this runs
    // inside a simulation step.
    rt_proc* p = with_self != 0 ? root : root->first_child;
    while (p != nullptr) {
        const uint32_t state = p->ctl & RT_PROC_STATE_MASK;
        if (state != RT_PROC_FINISHED && state != RT_PROC_KILLED && (p->ctl & RT_PROC_ABORT) == 0) {
            // KILLED at the instant `kill` runs, not when the thread gets round to
            // unwinding: LRM 9.7 defines the state as "process was forcibly killed",
            // and a caller that kills a thread and then reads its status must not see
            // WAITING. The thread still has to reach its own poll point to release its
            // frames, which is what the ABORT flag drives.
            p->ctl = (p->ctl & ~static_cast<uint32_t>(RT_PROC_STATE_MASK)) | RT_PROC_KILLED |
                     RT_PROC_ABORT;
            p->next_marked = chain;
            chain          = p;
        }
        if (p->first_child != nullptr) {
            p = p->first_child;
            continue;
        }
        while (p != root && p->next_sib == nullptr) {
            p = p->parent;
        }
        if (p == root) {
            break;
        }
        p = p->next_sib;
    }
    return chain;
}

extern "C" void* rt_proc_complete(void* pcb, void** fork_waiters_out) {
    auto*          p       = static_cast<rt_proc*>(pcb);
    const uint32_t killed  = p->ctl & RT_PROC_ABORT;
    p->ctl  = (p->ctl & ~static_cast<uint32_t>(RT_PROC_STATE_MASK | RT_PROC_ABORT)) |
             (killed != 0 ? RT_PROC_KILLED : RT_PROC_FINISHED);
    // Re-parent live children onto this thread's own parent, so a later
    // `disable fork` / `kill` still reaches them (LRM 9.6.3: descendants include
    // those of children that already terminated), and so "no live descendants" stays
    // exactly `first_child == nullptr`.
    rt_proc* child = p->first_child;
    while (child != nullptr) {
        rt_proc* next = child->next_sib;
        proc_link_child(p->parent, child);
        child = next;
    }
    p->first_child = nullptr;
    rt_proc* parent = p->parent;
    proc_unlink(p);
    p->parent   = nullptr;
    p->next_sib = nullptr;
    p->prev_sib = nullptr;
    if (fork_waiters_out != nullptr) {
        *fork_waiters_out = nullptr;
        // `wait fork`: the caller is due when its subtree is empty, which after
        // re-parenting is exactly first_child == NULL.
        if (parent != nullptr && (parent->ctl & RT_PROC_WAIT_FORK) != 0 &&
            parent->first_child == nullptr) {
            parent->ctl &= ~static_cast<uint32_t>(RT_PROC_WAIT_FORK);
            *fork_waiters_out = parent->waiters;
            parent->waiters   = nullptr;
        }
    }
    void* w    = p->waiters;
    p->waiters = nullptr;
    return w;
}

extern "C" void rt_proc_adopt(void* to, void* from) {
    auto*    np    = static_cast<rt_proc*>(to);
    auto*    p     = static_cast<rt_proc*>(from);
    rt_proc* child = p->first_child;
    while (child != nullptr) {
        // `next` before the re-link: proc_link_child rewrites child->next_sib.
        rt_proc* next = child->next_sib;
        proc_link_child(np, child);
        child = next;
    }
    p->first_child = nullptr;
}

extern "C" void rt_proc_thread_done(void* pcb) {
    auto* p = static_cast<rt_proc*>(pcb);
    if (p->refs != 0) {
        p->refs -= 1;
    }
    proc_maybe_free(p);
}

extern "C" void rt_proc_obj_retain(void* pcb) {
    static_cast<rt_proc*>(pcb)->refs += 1;
}

// The finalizer's half of the object <-> block pair (docs/design/process-class.md
// §4.2): the object is being reclaimed, so the block's weak memo of it stops naming
// it -- a later `self()` then mints a fresh object instead of returning a dead one --
// and the reference the object held on the block drops. The block returns to the
// pool when that was the last reference, i.e. the thread has completed too.
extern "C" void rt_proc_obj_release(void* pcb, [[maybe_unused]] void* obj) {
    auto* p = static_cast<rt_proc*>(pcb);
    // emit_proc_object mints an object only while the memo is null, so the memo names
    // the finalizing object from its creation until here.
    assert(p->obj == obj && "a process object finalizes while it is its block's memo");
    p->obj = nullptr;
    if (p->refs != 0) {
        p->refs -= 1;
    }
    proc_maybe_free(p);
}

extern "C" uint32_t rt_proc_pool_blocks(void) {
    if (g_proc_chunks.empty()) {
        return 0;
    }
    return static_cast<uint32_t>((g_proc_chunks.size() - 1) * kProcChunk + g_proc_used);
}

#ifdef CLOTHO_EXTRA_ASSERTS
// NOTE: Under extra asserts a frame carries its size in a kFrameAlign-byte
// header, and rt_free overwrites the whole frame with kFreedFrame before
// releasing it. Code that reads a frame after its coroutine was destroyed then
// reads a non-null wild pointer and fails at once, instead of depending on
// whether something reused the memory in between.
constexpr uint8_t kFreedFrame = 0xA5;

extern "C" void* rt_alloc(uint64_t size) {
    auto* base =
      static_cast<uint8_t*>(clotho::rt::os_aligned_alloc(static_cast<size_t>(size) + kFrameAlign, kFrameAlign));
    if (base == nullptr) {
        return nullptr;
    }
    std::memcpy(base, &size, sizeof size);
    return base + kFrameAlign;
}

extern "C" void rt_free(void* p) {
    if (p == nullptr) {
        return;
    }
    uint8_t* base = static_cast<uint8_t*>(p) - kFrameAlign;
    uint64_t size = 0;
    std::memcpy(&size, base, sizeof size);
    std::memset(p, kFreedFrame, static_cast<size_t>(size));
    clotho::rt::os_aligned_free(base);
}
#else
extern "C" void* rt_alloc(uint64_t size) {
    return clotho::rt::os_aligned_alloc(static_cast<size_t>(size), kFrameAlign);
}

extern "C" void rt_free(void* p) {
    clotho::rt::os_aligned_free(p);
}
#endif

// ---- Host entry ----

// Defined last so every process-global it arms is in scope.
int32_t rt_run(const rt_run_config* config, int32_t (*design_main)(void)) {
    g_plusargs.clear();
    g_plusargs.reserve(config->plusarg_count);
    for (uint32_t k = 0; k < config->plusarg_count; ++k) {
        g_plusargs.emplace_back(config->plusargs[k], config->plusarg_lens[k]);
    }
    g_system_access  = config->system_access;
    g_ref_stale_warn = config->loose_stale_ref;
    g_stop_tick      = kNoStopTick;
    if (config->has_stop_time != 0) {
        // A unitless design has no tick size to divide by: it counts in
        // femtoseconds, as rt_timeformat_init does.
        const uint64_t tick  = config->tick_fs == 0 ? 1 : config->tick_fs;
        // Rounded up: a stop time between two ticks cannot run the step it falls
        // short of, and stopping a tick early would drop events the user asked for.
        const uint64_t ticks = config->stop_fs / tick + (config->stop_fs % tick != 0 ? 1 : 0);
        g_stop_tick          = std::min(ticks, kNoStopTick - 1);
    }

    // The design's main returns 0 whichever way the run ended; a $fatal (LRM
    // 20.10) or an interrupt is the runtime's to report.
    const int32_t rc = design_main();
    return rc != 0 ? rc : g_run_status;
}
namespace {

// A unit `--stop-time` accepts, and how many femtoseconds it is.
struct TimeUnit {
    std::string_view name;
    uint64_t         femtoseconds;
};

constexpr uint64_t kFemtosecondsPerSecond = 1'000'000'000'000'000;

constexpr TimeUnit kTimeUnits[] = {
    { "fs", 1 },
    { "ps", 1'000 },
    { "ns", 1'000'000 },
    { "us", 1'000'000'000 },
    { "ms", 1'000'000'000'000 },
    { "s", kFemtosecondsPerSecond },
};

// The units as a sentence lists them.
constexpr std::string_view kTimeUnitList = "fs, ps, ns, us, ms or s";

} // namespace

int32_t rt_parse_stop_time(const char* text, uint64_t len, uint64_t* femtoseconds, char* error, uint64_t cap) {
    const std::string_view value(text, static_cast<size_t>(len));
    const auto             fail = [&](std::string_view why) {
        if (cap > 0) {
            const auto end =
              fmt::format_to_n(error, static_cast<size_t>(cap - 1), "invalid --stop-time '{}': {}", value, why);
            *end.out = '\0';
        }
        return 0;
    };

    // Digits only: no sign and no space.
    size_t   digits   = 0;
    uint64_t count    = 0;
    bool     overflow = false;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        const uint64_t d = static_cast<uint64_t>(value[digits] - '0');
        overflow         = overflow || count > (UINT64_MAX - d) / 10;
        count            = count * 10 + d;
        ++digits;
    }
    if (digits == 0) {
        return fail(fmt::format("write an integer and a unit, e.g. 100ns or 5us (units: {})", kTimeUnitList));
    }
    const std::string_view unit_name = value.substr(digits);
    if (unit_name.empty()) {
        return fail(fmt::format("the unit is missing; write e.g. {}ns (units: {})", value, kTimeUnitList));
    }
    const TimeUnit* unit = nullptr;
    for (const TimeUnit& u : kTimeUnits) {
        if (u.name == unit_name) {
            unit = &u;
        }
    }
    if (unit == nullptr) {
        return fail(fmt::format("unknown unit '{}' (units: {})", unit_name, kTimeUnitList));
    }
    // An out-of-range count is also too large, whatever the unit.
    if (overflow || count > UINT64_MAX / unit->femtoseconds) {
        return fail(fmt::format("too large; the longest time is about {} s", UINT64_MAX / kFemtosecondsPerSecond));
    }
    if (count == 0) {
        return fail("must be greater than zero");
    }
    *femtoseconds = count * unit->femtoseconds;
    return 1;
}
