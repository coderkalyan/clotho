// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// The scheduler trace (docs/design/scheduler-trace.md): the scheduling events the
// generated code and the scheduler report, and the checker that tests the rules of
// docs/design/scheduler-rules.md §3 over them while the run goes on (TraceCheck.cpp).
//
// The constants and record layouts below name no symbol, so the backend that emits
// the events can see them in every build. The functions exist only in a
// CLOTHO_EXTRA_ASSERTS build: a shipped build declares, defines, exposes and emits
// none of them.

#include <stdint.h>

// What a park waits for: rt_trace_control::kind of a control.
enum {
    // The leading control of an always that is subscribed once (S1), and a thunk
    // group's: the activation waits for its static list (rt_trace_subscribe).
    RT_TRACE_CTL_STATIC = 0,
    // An event control or `wait`, and the leading list an always that parks at an
    // intra-body control waits for at its head. The list comes with each park.
    RT_TRACE_CTL_EVENT = 1,
    // `#d`, and `#0`.
    RT_TRACE_CTL_DELAY = 2,
    // The parent's park at a `join` or `join_any`.
    RT_TRACE_CTL_JOIN = 3,
    // A task enable whose callee blocked.
    RT_TRACE_CTL_CALL = 4,
    // `wait fork`.
    RT_TRACE_CTL_WAIT_FORK = 5,
    // `p.await()`.
    RT_TRACE_CTL_AWAIT = 6,
    // A semaphore's or mailbox's wait queue.
    RT_TRACE_CTL_SYNC = 7,
    // `expect`.
    RT_TRACE_CTL_EXPECT = 8,
    // A thread parked by `p.suspend()`, until `p.resume()`.
    RT_TRACE_CTL_SUSPENDED = 9,
};

// What an activation was created for: rt_trace_control::kind of an origin.
enum {
    RT_TRACE_ORG_PROCESS = 32,
    RT_TRACE_ORG_FORK = 33,
    RT_TRACE_ORG_TASK = 34,
    RT_TRACE_ORG_PEND = 35,
    RT_TRACE_ORG_STROBE = 36,
    RT_TRACE_ORG_MONITOR = 37,
    RT_TRACE_ORG_EXPORT = 38,
};

// What pushed an activation: the cause of rt_trace_wake.
enum {
    // The seed queues a group that starts now.
    RT_TRACE_SEED = 0,
    // A fork queues a child it does not run inline.
    RT_TRACE_SPAWN = 1,
    // An assertion queues the action block it ramped.
    RT_TRACE_PEND = 2,
    // A store's scan of a static bucket, of a dynamic bucket, and of a virtual
    // interface member's waiter list: `x` is the signal, `fact` the change.
    RT_TRACE_STATIC = 3,
    RT_TRACE_DYNAMIC = 4,
    RT_TRACE_VIF = 5,
    // A trigger of an event object, or of a class property's change notifier: `x`.
    RT_TRACE_EVENT = 6,
    // A `#d` timer that expired.
    RT_TRACE_TIMER = 7,
    // The promotion of the `#0` batch.
    RT_TRACE_INACTIVE = 8,
    // The fork child a parent's join is due on.
    RT_TRACE_CHILD = 9,
    // A blocked callee that completed.
    RT_TRACE_CALLEE = 10,
    // `disable` or `kill`.
    RT_TRACE_FORCE = 11,
    // A semaphore or mailbox handing over.
    RT_TRACE_SYNC = 12,
    // The completion of a thread that `await` or `wait fork` waits for.
    RT_TRACE_AWAIT = 13,
    // `p.resume()`.
    RT_TRACE_PROC = 14,
    // The verdict of the attempt an `expect` waits for.
    RT_TRACE_EXPECT = 15,
};

// The edge a list entry waits for, and the change a signal cause carries: posedge,
// negedge, any change (ir::Edge's order), and the trigger of an event object.
enum {
    RT_TRACE_POS = 0,
    RT_TRACE_NEG = 1,
    RT_TRACE_ANY = 2,
    RT_TRACE_OBJ = 3,
};

// How a dispatch starts an activation.
enum {
    // Popped from a ring: the activation was queued.
    RT_TRACE_RING = 0,
    // Resumed at its creation, on the stack of its creator (S15).
    RT_TRACE_INLINE = 1,
    // Created, run to its end and destroyed in one go (S12, `$strobe`, `$monitor`).
    RT_TRACE_RUN_ONCE = 2,
};

// The region of a time slot a scheduler block is in (rt_trace_region, rt_trace_nba_pass).
enum {
    RT_TRACE_REG_ACTIVE = 0,
    RT_TRACE_REG_INACTIVE = 1,
    RT_TRACE_REG_NBA = 2,
    RT_TRACE_REG_OBSERVED = 3,
    RT_TRACE_REG_REACTIVE = 4,
    RT_TRACE_REG_RE_INACTIVE = 5,
    RT_TRACE_REG_RE_NBA = 6,
    RT_TRACE_REG_POSTPONED = 7,
    // The top of a slot once the wheel has fired its timers: the Preponed samples and the
    // commit of continuous assignments' delayed updates (S26), before the Active drain.
    RT_TRACE_REG_PREPONED = 8,
    // rt_sim_run starts the final blocks.
    RT_TRACE_REG_FINAL = 9,
};

// The staging path that created a nonblocking update (S23), in the low byte of
// rt_trace_nba_site::flags, and the flags above it. A timed update's path is how it is released
// (rt_trace_nba_release).
enum {
    // A block's shadow (lower_store_nb, S22).
    RT_TRACE_NBA_BLOCK = 0,
    // The record of an unpacked-array element or of a member written through a virtual
    // interface (emit_array_nba).
    RT_TRACE_NBA_ARRAY = 1,
    // The record of an aggregate leaf (emit_agg_nba).
    RT_TRACE_NBA_AGG = 2,
    // A `<= #0` record, created when its statement executes.
    RT_TRACE_NBA_ZERO = 3,
    // A `<= #d` record, created when its timer fires.
    RT_TRACE_NBA_TIMER = 4,
    // A `<= @e` record, created when its event occurs.
    RT_TRACE_NBA_EVENT = 5,
    RT_TRACE_NBA_PATHS = 6,
    // The target can be forced or procedurally assigned (StoreNbGated): its commit may drop
    // the update, which is R14's to judge, so its bytes are not compared.
    RT_TRACE_NBA_GATED = 0x100,
    // The update's bytes are not known to the trace: a runtime-offset select of a very wide
    // vector stages through rt_wide_ins. Its target's bytes are not compared.
    RT_TRACE_NBA_OPAQUE = 0x200,
};

// How an event object is triggered (rt_trace_trigger).
enum {
    // `-> e`, at once.
    RT_TRACE_TRIGGER_NOW = 0,
    // Every form of `->>`: the update event of a nonblocking trigger, which belongs to an NBA
    // pass.
    RT_TRACE_TRIGGER_NB = 1,
};

// One park site, or the origin of an activation: a constant the backend emits per
// site. Two lowerings of one site may emit two records, which are one control when
// the kind and the location agree.
typedef struct rt_trace_control {
    // "<file>:<line>:<col>" of the construct, "<unknown>" without a resolver.
    const char* where;
    uint32_t    kind; // RT_TRACE_CTL_* or RT_TRACE_ORG_*
} rt_trace_control;

// One entry of a list: the address of a signal's storage in its own instance and the
// edge, or an event object and RT_TRACE_OBJ. `name` is the signal's declared name, or
// null where the code that names it is shared by instances that bind it differently.
typedef struct rt_trace_entry {
    const void* x;
    const char* name;
    uint64_t    edge;
} rt_trace_entry;

// What a nonblocking statement says of its updates, constant per statement. The update's
// planes are the target's own, as the code stores them: a vector's `must | may << W` word
// (just the value for a 2-state target) of `bits0` bits and, when the target has one, its
// z word of `bits1` bits (zero: none).
typedef struct rt_trace_nba_site {
    // "<file>:<line>:<col>" of the statement, "<unknown>" without a resolver.
    const char* where;
    // The target's declared name, or null where the code is shared by instances that bind
    // it differently.
    const char* name;
    // The staging path (RT_TRACE_NBA_*) and RT_TRACE_NBA_GATED / RT_TRACE_NBA_OPAQUE.
    uint32_t flags;
    uint32_t bits0;
    uint32_t bits1;
} rt_trace_nba_site;

// What a run reported, for the suite's summary line.
typedef struct rt_trace_counts {
    uint64_t runs;
    uint64_t events;
    uint64_t spawns;
    uint64_t parks;
    uint64_t wakes;
    uint64_t dispatches;
    // Nonblocking updates created, by staging path (RT_TRACE_NBA_*).
    uint64_t creates[RT_TRACE_NBA_PATHS];
    uint64_t nba_passes;
    uint64_t pends;
    uint64_t triggers;
    uint64_t finishes;
} rt_trace_counts;

#ifdef CLOTHO_EXTRA_ASSERTS

#ifdef __cplusplus
extern "C" {
#endif

// -- Events of the generated code --
//
// An activation `a` is a coroutine handle, or the address of a thunk group's wake pair
// with bit 0 set; the home tag bits a parked handle carries (bits 1 and 2) are ignored.

// A new activation, created by `parent` (null for the seed and the schedulers' own
// runs) for `origin` (null: none known).
void rt_trace_spawn(void* a, const rt_trace_control* origin, void* parent);

// Add `n` entries to the static list of `a`, which a park at a RT_TRACE_CTL_STATIC
// control waits for: the seed's subscription of an always or a thunk group.
void rt_trace_subscribe(void* a, const rt_trace_entry* list, uint32_t n);

// The same for one event object, found when the seed links the persistent node.
void rt_trace_subscribe_event(void* a, const void* obj, const char* name);

// Add one entry to the list of the park that follows, at a RT_TRACE_CTL_EVENT control: the
// activation `a` waits for `x`.
void rt_trace_waits_for(void* a, const void* x, const char* name, uint64_t edge);

// `a` parks at control `c`: a delay control waits `delay` ticks (0: the `#0` region).
void rt_trace_park(void* a, const rt_trace_control* c, uint64_t delay);

// Something pushes `a` onto a ring (RT_TRACE_SEED ...): `x` and `fact` name the signal
// and change, or the event object, where the cause has one.
void rt_trace_wake(void* a, uint32_t cause, const void* x, uint32_t fact);

// `a` is about to run (RT_TRACE_RING ...). Returns 0 when the run must be skipped: the
// dispatch breaks a rule, or an earlier event did.
int32_t rt_trace_dispatch(void* a, uint32_t how);

// `a` runs again past its park at control `c`.
void rt_trace_resume(void* a, const rt_trace_control* c);

// `a` reached its final suspend.
void rt_trace_end(void* a);

// The scheduler enters `region` (RT_TRACE_REG_*): the Active drain's entries, the Inactive,
// Observed, Reactive, Re-Inactive and Postponed regions, and the top of a slot. The NBA and
// Re-NBA regions are the passes of rt_trace_nba_pass.
void rt_trace_region(uint32_t region);

// A nonblocking update is created when its statement executes: a plain NBA, and a `<= #0`.
// `dst0` and `dst1` are the addresses of the target's planes (`dst1` null: no z word);
// `val0`, `mask0`, `val1` and `mask1` are the update's bits and the bits it writes, in the
// planes' own layout and positioned at the target's bit offset, as word buffers of the
// planes' width rounded up to 64-bit words (`mask` null: every bit).
void rt_trace_nba_create(const rt_trace_nba_site* site, void* dst0, void* dst1, const void* val0,
                         const void* mask0, const void* val1, const void* mask1);

// A timed update is armed: `rec` is the record that will release it, `<= #d` for a timer and
// `<= @e` for an event, and the planes are those of rt_trace_nba_create. The update is
// created when the record is released (rt_trace_nba_release).
void rt_trace_nba_stage(const void* rec, const rt_trace_nba_site* site, void* dst0, void* dst1,
                        const void* val0, const void* mask0, const void* val1, const void* mask1);

// A timer or an occurrence of an event releases `rec`: `path` is RT_TRACE_NBA_TIMER or
// RT_TRACE_NBA_EVENT, and `moment` the scheduler's number for the occurrences that released
// records together (zero for a timer: a wheel tick is one moment).
void rt_trace_nba_release(const void* rec, uint32_t path, uint32_t moment);

// The NBA or Re-NBA commit begins (`begin` nonzero) or ends; `region` is RT_TRACE_REG_NBA or
// RT_TRACE_REG_RE_NBA.
void rt_trace_nba_pass(uint32_t begin, uint32_t region);

// A `->>` links event object `obj` onto the nonblocking pend chain.
void rt_trace_pend(const void* obj);

// Event object `obj` triggers: RT_TRACE_TRIGGER_NOW or RT_TRACE_TRIGGER_NB.
void rt_trace_trigger(const void* obj, uint32_t kind);

// -- Events of the scheduler (runtime/Scheduler.cpp) --

// A run starts: forget every activation and the last run's violation.
void rt_trace_begin(void);

// Time advances to `now`.
void rt_trace_slot(uint64_t now);

// The slot has settled and time is about to move on, or the run to end: every region is
// empty.
void rt_trace_slot_end(void);

// A `$finish` was taken.
void rt_trace_finish(void);

// Whether a violation has ended the run: rt_wheel_advance reads it as it reads the
// stop time.
int32_t rt_trace_halted(void);

// -- For the host, after the run --

// The rule the last run's first violation broke ("R5.1" ...), or null when it broke none.
const char* rt_trace_rule(void);

// The report of that violation: the rule, the time, the activation, the control and
// cause, and the last events.
const char* rt_trace_report(void);

// What every run of this process reported so far.
void rt_trace_totals(rt_trace_counts* out);

#ifdef __cplusplus
}
#endif

#endif // CLOTHO_EXTRA_ASSERTS
