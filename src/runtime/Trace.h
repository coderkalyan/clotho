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
    // The parent's park at a `join` or a `join_any`, on the fork's join record.
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

// What an activation was created for: rt_trace_control::kind of an origin. The bodies the
// scheduler runs once (`clotho.run_once`) are named by their region.
enum {
    RT_TRACE_ORG_PROCESS = 32,
    RT_TRACE_ORG_FORK = 33,
    RT_TRACE_ORG_TASK = 34,
    RT_TRACE_ORG_PEND = 35,
    RT_TRACE_ORG_STROBE = 36,
    RT_TRACE_ORG_MONITOR = 37,
    RT_TRACE_ORG_EXPORT = 38,
    RT_TRACE_ORG_PREINIT = 39,
    RT_TRACE_ORG_PREPONED = 40,
    RT_TRACE_ORG_OBSERVED = 41,
    RT_TRACE_ORG_RE_NBA = 42,
    RT_TRACE_ORG_FINAL = 43,
};

// The facts of a seed row's processes, ORed into the kind of its origin (RT_TRACE_ORG_PROCESS)
// above RT_TRACE_KIND, which is the kind proper.
enum {
    RT_TRACE_KIND = 0xff,
    // An `initial` or an `always` procedure, not one that starts late (R17), and not a program's
    // `initial`, which starts in the Reactive region.
    RT_TRACE_ORG_PROCEDURE = 1u << 8,
    // A continuously active process (Process::self_start): evaluated at time zero (R15).
    RT_TRACE_ORG_SELF_START = 1u << 9,
    // A group with an always_comb or always_latch member: its time-zero run follows the start of
    // every procedure (R17).
    RT_TRACE_ORG_LATE = 1u << 10,
    // A program's `initial`: its thread is reactive for its whole life (R25).
    RT_TRACE_ORG_PROGRAM = 1u << 11,
    // A reactive-home continuous process: a program's port copy, `assign` or resolver (R25).
    RT_TRACE_ORG_REACTIVE_CONT = 1u << 12,
};

// How rt_trace_block reports an activation and a `disable` target, which is the head of the
// target's list of occupants.
enum {
    // The activation links its node on the list: it enters a block some `disable` names from
    // outside, or the task some `disable` names.
    RT_TRACE_BLOCK_ENTER = 0,
    // It unlinks the node: the end of the block, an exit that skips it, the epilogue.
    RT_TRACE_BLOCK_EXIT = 1,
    // The IR says the activation is activity enabled within the target for its whole life: a fork
    // child of a targeted block or task (Function::fork_targets). Reported by the fork, once the
    // spawn has announced the child; R23 checks that the child's ramp linked it.
    RT_TRACE_BLOCK_INHERIT = 2,
    // The ramp of a fork child links its node, before the spawn that announces the child.
    RT_TRACE_BLOCK_RAMP = 3,
};

// Why rt_trace_abort marks an activation to leave: a `disable` of a block or task, or a `kill`
// (`disable fork` too) of its thread.
enum {
    RT_TRACE_ABORT_DISABLE = 0,
    RT_TRACE_ABORT_KILL = 1,
};

// The join of a fork (rt_trace_fork): `join` waits for every child, `join_any` for the first and
// `join_none` for none.
enum {
    RT_TRACE_JOIN_ALL = 0,
    RT_TRACE_JOIN_ANY = 1,
    RT_TRACE_JOIN_NONE = 2,
};

// The ring a wake pushes onto, in bits 8 and up of rt_trace_wake's cause: the Active ring, the
// Reactive ring, or the reactive continuous ring a program's port copies and assigns drain from
// before program code.
enum {
    RT_TRACE_QUEUE_SHIFT = 8,
    RT_TRACE_QUEUE_ACTIVE = 0,
    RT_TRACE_QUEUE_REACTIVE = 1,
    RT_TRACE_QUEUE_CONT = 2,
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
    // The fork child a parent's join is due on: `x` is the join record the parent parked on.
    RT_TRACE_CHILD = 9,
    // A blocked callee that completed: `x` is the task's record.
    RT_TRACE_CALLEE = 10,
    // `disable` or `kill`: the activation was marked to leave (rt_trace_abort).
    RT_TRACE_FORCE = 11,
    // A semaphore or mailbox handing over: `x` is the head word of the queue.
    RT_TRACE_SYNC = 12,
    // The completion of a thread that `await` or `wait fork` waits for: `x` is the thread that
    // completed, null for the waiters of a `wait fork` barrier.
    RT_TRACE_AWAIT = 13,
    // `p.resume()`.
    RT_TRACE_PROC = 14,
    // The verdict of the attempt an `expect` waits for: `x` is the instance whose monitor reached
    // it.
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

// The bracket that reported a change (rt_trace_change_site::path): the write paths of
// scheduler-rules.md R6 each reach one of them.
enum {
    // A store's snapshot and its compare (service_after_write): a blocking store, a runtime
    // write-back ($fgets, $sscanf, $fread, ...), a call's output or `ref` bracket at its return.
    RT_TRACE_CHG_STORE = 0,
    // The commit of an NBA: a block's shadow, an array element or interface member record, a
    // timed record (emit_block_commit_body, emit_array_commit, emit_intra_commit).
    RT_TRACE_CHG_COMMIT = 1,
    // A store through the wide-select path, which compares the words around its offset.
    RT_TRACE_CHG_WINDOW = 2,
    // A store to a waited-on class property, which fires the property's notifiers.
    RT_TRACE_CHG_PROPERTY = 3,
    // A write through a `ref` formal, which the wake thunk of the call services.
    RT_TRACE_CHG_REF = 4,
    // A string's content compare.
    RT_TRACE_CHG_STRING = 5,
    // A write the code does not classify, which wakes every level waiter: the commit of an
    // array's or an aggregate's records, `$readmem` and `$fread` into a memory, a container
    // mutation.
    RT_TRACE_CHG_LEVEL = 6,
};

// What a bracket saw of one write (rt_trace_change's `seen`): bit 0 of the must, may and z
// planes before and after, whether a plane and the z word differ, the facts the code computed,
// and which of these the bracket knows.
enum {
    RT_TRACE_SEEN_OLD_MUST = 1u << 0,
    RT_TRACE_SEEN_OLD_MAY  = 1u << 1,
    RT_TRACE_SEEN_OLD_Z    = 1u << 2,
    RT_TRACE_SEEN_NEW_MUST = 1u << 3,
    RT_TRACE_SEEN_NEW_MAY  = 1u << 4,
    RT_TRACE_SEEN_NEW_Z    = 1u << 5,
    // A 4-state slot: its may plane is read (a 2-state slot is its must plane alone).
    RT_TRACE_SEEN_FOUR = 1u << 6,
    // The slot has a z word.
    RT_TRACE_SEEN_HAS_Z = 1u << 7,
    // Bit 0 of the must and may planes is known, before and after; bit 0 of the z word too.
    RT_TRACE_SEEN_BIT0 = 1u << 8,
    RT_TRACE_SEEN_ZBIT = 1u << 9,
    // A must or may bit differs, and whether that is known; the same for the z word.
    RT_TRACE_SEEN_DIFF       = 1u << 10,
    RT_TRACE_SEEN_DIFF_KNOWN = 1u << 11,
    RT_TRACE_SEEN_ZDIFF       = 1u << 12,
    RT_TRACE_SEEN_ZDIFF_KNOWN = 1u << 13,
    // The facts the code computed (Table 9-2's posedge and negedge, and `changed`), and which
    // it computed: a fact no waiter kind of the signal reads is not computed (S19).
    RT_TRACE_SEEN_POS       = 1u << 16,
    RT_TRACE_SEEN_NEG       = 1u << 17,
    RT_TRACE_SEEN_ANY       = 1u << 18,
    RT_TRACE_SEEN_POS_KNOWN = 1u << 19,
    RT_TRACE_SEEN_NEG_KNOWN = 1u << 20,
    RT_TRACE_SEEN_ANY_KNOWN = 1u << 21,
    // A write the code does not classify (RT_TRACE_CHG_LEVEL): it wakes every level waiter, and
    // nothing of its bits is known.
    RT_TRACE_SEEN_LEVEL = 1u << 24,
};

// How the checker reads the value a signal's storage holds (R6.2): `count` elements `stride`
// bytes apart, each a must|may word of `bits0` bits at its start (the value alone for a 2-state
// slot, so `bits0` is `width`; `2 * width` for a 4-state one, the may plane from bit `width`)
// and, when `bits1` is not zero, a z word of `bits1` bits at byte `z_off`. Only these bits are
// the value: the bytes around them are padding.
typedef struct rt_trace_shape {
    uint32_t width;
    uint32_t bits0;
    uint32_t bits1;
    uint32_t z_off;
    uint32_t stride;
    uint32_t count;
} rt_trace_shape;

// A bracket that reports changes, constant per site.
typedef struct rt_trace_change_site {
    // "<file>:<line>:<col>" of the statement, "<unknown>" without a resolver.
    const char* where;
    // The written signal's declared name, or null where the code is shared by instances that
    // bind it differently, or the write is not to a signal.
    const char* name;
    uint32_t    path; // RT_TRACE_CHG_*
} rt_trace_change_site;

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
// `shape` says how to read the value at `x` (null: the checker reads nothing there: an
// event, a string, a container or an aggregate).
typedef struct rt_trace_entry {
    const void*           x;
    const char*           name;
    uint64_t              edge;
    const rt_trace_shape* shape;
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

// One entry of a guarded group's member lists (S9): member `member` of the group lists the storage at `x`
// for `edge` (RT_TRACE_POS ...), which is the sensitivity its guard tests.
typedef struct rt_trace_member_entry {
    const void* x;
    uint32_t    edge;
    uint32_t    member;
} rt_trace_member_entry;

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
    // Writes the brackets reported (rt_trace_change).
    uint64_t changes;
    // Block entries and exits, and activations a `disable` or `kill` marked.
    uint64_t blocks;
    uint64_t aborts;
    // Forks, and the joins their parents passed.
    uint64_t forks;
    uint64_t joins;
    // Members of guarded groups that ran (S9).
    uint64_t member_runs;
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

// The members of the guarded group `a` (S9): `list` is each member's own sensitivity as the IR gives it (an
// event object is not listed), `members` the number of members. `exact` says that no two events of the group
// share a trigger bit, so a member runs only when one of its own events fired or the wake named none;
// otherwise a sibling's event that shares its bit runs it too.
void rt_trace_members(void* a, const rt_trace_member_entry* list, uint32_t n, uint32_t members, uint32_t exact);

// Member `i` of the guarded group `a` starts its body in this activation.
void rt_trace_member(void* a, uint32_t i);

// Add one entry to the list of the park that follows, at a RT_TRACE_CTL_EVENT control: the
// activation `a` waits for `x` (`shape`: as rt_trace_entry's).
void rt_trace_waits_for(void* a, const void* x, const char* name, uint64_t edge, const rt_trace_shape* shape);

// A write to the storage at `x` (a signal's own, or null where no waiter names it) was seen by
// a bracket, which says what it saw (RT_TRACE_SEEN_*). It precedes the wakes the write makes.
void rt_trace_change(const rt_trace_change_site* site, const void* x, uint32_t seen);

// `a` parks at control `c`. `arg` is a delay control's ticks (0: the `#0` region); for the parks
// that wait on a record or a queue (a join, a task call, a semaphore or mailbox, `await`, `wait
// fork`, `expect`) it is the address that names it, which the wake that ends the park names too
// (B43). Bits 1 and 2 of `a` are the thread's home tag (2: reactive), which says whether a `#0`
// waits in the Inactive batch or the Re-Inactive one.
void rt_trace_park(void* a, const rt_trace_control* c, uint64_t arg);

// Something pushes `a` onto a ring (RT_TRACE_SEED ...): `x` and `fact` name the signal
// and change, or the event object or record, where the cause has one. The low byte of
// `cause` is the cause; from RT_TRACE_QUEUE_SHIFT up, the ring the wake pushes onto.
void rt_trace_wake(void* a, uint32_t cause, const void* x, uint32_t fact);

// `a` is about to run (RT_TRACE_RING ...). Returns 0 when the run must be skipped: the
// dispatch breaks a rule, or an earlier event did.
int32_t rt_trace_dispatch(void* a, uint32_t how);

// `a` runs again past its park at control `c`.
void rt_trace_resume(void* a, const rt_trace_control* c);

// `a` reached its final suspend.
void rt_trace_end(void* a);

// `a` links its node on the list of occupants of `target`, the head of the list of the block or
// task a `disable` names, or unlinks it, or is within it for its whole life as the IR says
// (RT_TRACE_BLOCK_*).
void rt_trace_block(void* a, const void* target, uint32_t how);

// `a` executes a `disable` of the block or task whose list of occupants is `target` (null: `a`
// is not an activation). Every occupant of the list is marked by the walk that follows.
void rt_trace_disable(void* a, const void* target);

// A `disable` (`target`: the list it walks) or a `kill` or `disable fork` (`target` null) marks
// activation `a` to leave (RT_TRACE_ABORT_*). It leaves `target` (rt_trace_block) or ends before
// it parks again, except at a join or a task call, which hold their wake until the child or
// callee completes.
void rt_trace_abort(void* a, const void* target, uint32_t kind);

// `a` forks `width` children onto the join record `rec`, which the children's epilogues, the join's
// park and the parent's wake name alike (RT_TRACE_JOIN_*); `a` is null in a function, which has no
// activation and no join. The children are announced by the spawns that follow, each of them
// created by `a`. A fork on an address a record had starts a new record.
void rt_trace_fork(void* a, const void* rec, uint32_t kind, uint32_t width);

// Fork child `a` completes into the join record `rec`: before the decrement of the record's own
// count, and so before the wake of the parent that this completion is due to make.
void rt_trace_child_done(void* a, const void* rec);

// `a` is past the join of the fork on `rec`, which it either did not park for (the children it waits
// for had completed already) or was woken from.
void rt_trace_join(void* a, const void* rec);

// The scheduler runs the bodies of one region through `clotho.run_once` (`kind`:
// RT_TRACE_ORG_PREINIT ...), or has done (0).
void rt_trace_once(uint32_t kind);

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
