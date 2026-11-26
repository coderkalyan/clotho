// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The checker of the scheduler trace (docs/design/scheduler-trace.md): the events of
// Trace.h as they arrive, and rule R5 of docs/design/scheduler-rules.md §3 as predicates
// over them. Online: each call updates the state of one activation and tests the event
// against it, and the first violation ends the run before its consequences.
//
// Built only with CLOTHO_EXTRA_ASSERTS. It includes Trace.h and the standard library and
// nothing of the code it checks: lists come from the IR facts the backend reports at each
// park, causes from what fired, never from the buckets or the ready lists, which are the
// speedups under test.

#ifdef CLOTHO_EXTRA_ASSERTS

#include "Trace.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

// The states of rule R5. A thunk group is Parked at its static control between runs.
enum class State : uint8_t { Spawned, Parked, Queued, Running, Ended };

const char* state_name(State s) {
    switch (s) {
        case State::Spawned: return "spawned, not yet queued or run";
        case State::Parked: return "parked";
        case State::Queued: return "queued";
        case State::Running: return "running";
        case State::Ended: return "ended";
    }
    return "?";
}

enum class Ev : uint8_t {
    Spawn,
    Park,
    Wake,
    Dispatch,
    Resume,
    End,
    Slot,
    SlotEnd,
    Region,
    Create,
    Stage,
    Release,
    PassBegin,
    PassEnd,
    Pend,
    Trigger,
    Finish,
    Change,
    Block,
    Disable,
    Abort,
    Fork,
    ChildDone,
    Join,
    Member,
};

struct Entry {
    const void*           x;
    const char*           name;
    uint64_t              edge;
    // How to read the value at `x`, and the bytes it held at the park (R6b); null and empty for
    // an event or a storage the checker does not read.
    const rt_trace_shape* shape = nullptr;
    std::vector<uint8_t>  snap;
};

// What a thread's home region is, as far as the trace knows it (R25): a program's `initial` and
// what it forks and calls is reactive, a design process is not, and what the trace cannot tell
// (a DPI export, a `$strobe` body) is Unknown.
enum class Home : uint8_t { Active, Reactive, Unknown };

// An abort that a `disable` or a `kill` made of an activation and the activation has not obeyed
// yet: `target` is the list a `disable` walked, or zero for a `kill`.
struct Mark {
    uintptr_t target;
    uint8_t   kind;
};

// A `disable` that has to reach an occupant of its list, and has not yet (R23).
struct AbortOwed {
    uintptr_t act;
    uintptr_t target;
};

// One entry of a member's own list in a guarded group (S9): the member lists the storage at `x` for `edge`.
struct MEntry {
    uintptr_t x;
    uint32_t  edge;
    uint32_t  member;
};

// What a member of a guarded group did in the activation under way: bit 0, a write of its own list owes
// it a run (and what the write was); bit 1, it ran.
constexpr uint8_t kMemberOwed = 1;
constexpr uint8_t kMemberRan  = 2;

struct MState {
    uint8_t   bits = 0;
    uintptr_t x    = 0;
    uint32_t  edge = 0;
};

struct Act {
    State                   state  = State::Spawned;
    const rt_trace_control* origin = nullptr;
    // The control of the last park, which the resume of that park names.
    const rt_trace_control* park   = nullptr;
    uintptr_t               parent = 0;
    // A delay park: the tick it ends at, or zero for `#0`.
    uint64_t                deadline = 0;
    bool                    zero     = false;
    // When the activation was last queued, in events so far, and by what: R24 asks of a
    // dispatch after a `$finish` whether the activation was woken by the exit that follows it.
    uint64_t                queued_at    = 0;
    uint8_t                 queued_cause = 0;
    // What a park at a static control waits for, and at an event control.
    std::vector<Entry>      stat;
    std::vector<Entry>      list;
    // The entries reported for the park that has not been reported yet.
    std::vector<Entry>      pending;
    // Counts the parks, so that a wake owed to one park is not owed to the next.
    uint64_t                park_seq = 0;
    // The ring a Queued activation sits on (RT_TRACE_QUEUE_*), and the home tag of the thread at its
    // last park, which says which batch a `#0` waits in.
    uint8_t                 ring          = 0;
    bool                    home_reactive = false;
    // The thread's home, as the trace knows it (R25).
    Home                    home          = Home::Unknown;
    // What the last park waits on, for the parks that name a record or a queue (B43).
    uintptr_t               rec           = 0;
    bool                    dispatched    = false;
    // Owed until the first dispatch: a continuously active process evaluates at time zero (R15), a
    // seeded procedure starts before a late group does (R17), a pended report runs in Postponed (R4).
    bool                    zero_owed     = false;
    bool                    proc_owed     = false;
    bool                    strobe_owed   = false;
    // The lists of occupants the activation is on (R23), the ones the IR says it is within for its
    // whole life, and the aborts it has not obeyed.
    std::vector<uintptr_t>  blocks;
    std::vector<uintptr_t>  inherits;
    std::vector<Mark>       marks;
    // R22: the join record of this activation's latest fork, whose children it creates next; the record
    // of the fork this activation is a child of; whether it completed into it.
    uintptr_t               forked    = 0;
    uintptr_t               member_of = 0;
    bool                    completed = false;
    // S9: how many members a guarded group has (zero: it is not one), each member's own list sorted by
    // storage, what each did in this activation, whether no two events of the group share a trigger bit,
    // and whether a wake in this activation named no trigger, which sets every bit and runs every member.
    uint32_t                members = 0;
    bool                    exact   = false;
    bool                    all_ok  = false;
    std::vector<MEntry>     mentries;
    std::vector<MState>     mstate;
};

// A table from an address to a value, by open addressing: an activation per frame
// address, and a signal's name per storage address. Zero is no key.
template <typename V>
class AddrMap {
public:
    V* find(uintptr_t key) {
        if (keys_.empty()) {
            return nullptr;
        }
        for (size_t i = slot(key);; i = (i + 1) & mask_) {
            if (keys_[i] == key) {
                return &vals_[i];
            }
            if (keys_[i] == 0) {
                return nullptr;
            }
        }
    }

    V& at(uintptr_t key) {
        if ((count_ + 1) * 4 > keys_.size() * 3) {
            grow();
        }
        for (size_t i = slot(key);; i = (i + 1) & mask_) {
            if (keys_[i] == key) {
                return vals_[i];
            }
            if (keys_[i] == 0) {
                keys_[i] = key;
                ++count_;
                return vals_[i];
            }
        }
    }

    // Forget everything; a table that has grown large comes back to a small one, so a
    // suite of small runs does not clear a large table for each.
    void clear() {
        if (keys_.size() > 4096) {
            keys_.clear();
            vals_.clear();
            mask_ = 0;
        } else {
            keys_.assign(keys_.size(), 0);
            vals_.assign(vals_.size(), V{});
        }
        count_ = 0;
    }

    // Calls `f(key, value)` for every entry.
    template <typename F>
    void for_each(F&& f) {
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] != 0) {
                f(keys_[i], vals_[i]);
            }
        }
    }

private:
    size_t slot(uintptr_t key) const {
        return static_cast<size_t>((key >> 3) * 0x9e3779b97f4a7c15ull >> 17) & mask_;
    }

    void grow() {
        const size_t               size = keys_.empty() ? 256 : keys_.size() * 2;
        std::vector<uintptr_t>     old_keys = std::move(keys_);
        std::vector<V>             old_vals = std::move(vals_);
        keys_.assign(size, 0);
        vals_.assign(size, V{});
        mask_  = size - 1;
        count_ = 0;
        for (size_t i = 0; i < old_keys.size(); ++i) {
            if (old_keys[i] != 0) {
                at(old_keys[i]) = std::move(old_vals[i]);
            }
        }
    }

    std::vector<uintptr_t> keys_;
    std::vector<V>         vals_;
    size_t                 mask_  = 0;
    size_t                 count_ = 0;
};

// One event as the report prints it.
struct Rec {
    uint64_t                 a;
    uint64_t                 x;
    uint64_t                 now;
    const rt_trace_control*  c;
    const rt_trace_nba_site* site;
    Ev                       ev;
    uint8_t                  cause; // a wake's cause, a dispatch's mode, a park's delay flag, a path or a region
    uint8_t                  fact;
    // A wake's ring (RT_TRACE_QUEUE_*).
    uint8_t                  queue;
};

constexpr size_t kRing = 256;

// -- Nonblocking updates (R12) --

// The 64-bit words that hold `bits` bits.
constexpr uint32_t words_of(uint32_t bits) {
    return (bits + 63) / 64;
}

// An event a `<= @e` record waits for (R11): a signal's storage and an edge, or an event object.
struct EWait {
    uintptr_t x;
    uint32_t  edge;
};

// One nonblocking update from its creation until a pass applies it: where it goes and the bits it
// writes, as the code stores them. Updates are applied in the order they were created.
struct Upd {
    const rt_trace_nba_site* site = nullptr;
    // The address of each plane (zero: the target has no second plane) and its key in the pass.
    uintptr_t                dst[2] = { 0, 0 };
    uint32_t                 key[2] = { 0, 0 };
    // RT_TRACE_NBA_*: how it was created.
    uint32_t                 path   = 0;
    // A timed update's release moment, which orders it by `arm` among the records that one
    // occurrence released together; zero for an update that is a moment of its own.
    uint64_t                 moment = 0;
    uint64_t                 arm    = 0;
    uint64_t                 tick   = 0;
    uint32_t                 region = 0;
    // Plane 0's value, then its mask, then plane 1's: each `words_of` the plane's bits.
    std::vector<uint64_t>    w;
    // A `<= #d` record's deadline; a `<= @e` record's count, the occurrences of its control seen since it
    // was armed, and the events it waits for (R11).
    uint64_t                 deadline = 0;
    uint32_t                 count    = 0;
    uint32_t                 occ      = 0;
    std::vector<EWait>       waits;

    uint32_t        bits(int p) const { return p == 0 ? site->bits0 : site->bits1; }
    const uint64_t* val(int p) const { return w.data() + (p == 0 ? 0 : 2 * words_of(site->bits0)); }
    const uint64_t* mask(int p) const { return val(p) + words_of(bits(p)); }
};

// A plane of design memory that some update of the pass writes, and what it held when the pass
// began.
struct Key {
    uintptr_t             dst;
    uint32_t              bits;
    // A forced target, or one whose bits the trace does not know: not compared.
    bool                  skip;
    std::vector<uint64_t> base;
};

// A `<= @e` record that waits for occurrences of one event: its address, and the arm that made it (the
// address is reused by the next arm of its site).
struct EWatch {
    uintptr_t rec;
    uint64_t  arm;
};

// A `->>` linked onto the pend chain and not yet triggered.
struct Pend {
    uintptr_t obj;
    uint64_t  tick;
    uint32_t  region;
};

// What the last write of a storage did, by the checker's own reading of its bits (R7), and in which
// step of the run it was made: a wake by a store follows its change with no dispatch, park or region
// between (R6.2).
struct ChgFacts {
    uint64_t step = 0;
    bool     pos  = false;
    bool     neg  = false;
    bool     any  = false;
};

// A wake that a change owes to an activation parked as it was at that moment (R6.1), or that the
// completion of a fork's child owes to its parent parked at the join (`join`, R22.3: `x` is the record).
struct Owed {
    uintptr_t act;
    uint64_t  seq;
    uintptr_t x;
    uint64_t  edge;
    bool      join = false;
};

// A fork on its join record (R22): who forked, how it joins, how many children it announced, and how
// many have been created and have completed since.
struct Fork {
    uintptr_t parent  = 0;
    uint8_t   kind    = RT_TRACE_JOIN_ALL;
    uint32_t  width   = 0;
    uint32_t  spawned = 0;
    uint32_t  done    = 0;
};

// An inertial record (R16) from the arm that made it until its update is applied or it is dropped: Live until a
// later store supersedes it or the wheel fires it, Fired (and waiting for its site's commit) once the runtime
// took it for the latest of its site, Done after.
enum class IState : uint8_t { Live, Superseded, Fired, Done };

struct InertRec {
    uint32_t idx      = 0;
    IState   state    = IState::Done;
    uint64_t armed_at = 0;
    uint64_t deadline = 0;
    uint64_t epoch    = 0;
};

// What the checker holds of an inertial site (the dense id of its target): the record its latest arm made,
// which nothing has superseded or fired, and the epoch its last decision asked a record to be armed with until
// the arm that does.
struct InertSite {
    uintptr_t live   = 0;
    uint64_t  expect = 0;
};

struct Checker {
    AddrMap<Act>         acts;
    AddrMap<const char*> names;
    uint64_t             now    = 0;
    bool                 halted = false;
    Rec                  ring[kRing] = {};
    uint64_t             seq        = 0;
    std::string          rule;
    std::string          report;
    rt_trace_counts      run   = {};
    rt_trace_counts      total = {};
    bool                 counted = false;
    // The region the scheduler is in (RT_TRACE_REG_*), and how many slots have begun.
    uint32_t             region  = RT_TRACE_REG_PREPONED;
    uint64_t             slot_no = 0;
    // R24: a `$finish` was taken and the finals have not begun; `finish_at` counts the events by
    // then.
    bool                 finished  = false;
    bool                 in_final  = false;
    uint64_t             finish_at = 0;
    // The NBA pass in progress.
    bool                 in_pass     = false;
    uint32_t             pass_region = 0;
    // The updates created and not applied, in creation order: the first `split` are the ones the
    // pass in progress began with. `keys` are the planes they write, the first `sorted_keys` of
    // them sorted by address.
    std::vector<Upd>     pending;
    size_t               split = 0;
    std::vector<Key>     keys;
    size_t               sorted_keys = 0;
    // `<= #d` and `<= @e` records armed and not yet released, by record, and the order they were
    // armed in.
    AddrMap<Upd>         armed;
    uint64_t             arm_seq = 0;
    // R11: the armed `<= @e` records that wait for each event or signal, by its address, and the records the
    // last occurrence of whose control has come and that nothing has created yet.
    AddrMap<std::vector<EWatch>> ewatch;
    std::vector<EWatch>          rel_owed;
    // The `->>` linked and not yet triggered, oldest first.
    std::deque<Pend>     pends;
    // The word buffers of retired updates, so that a design that stages an update every cycle
    // allocates none.
    std::vector<std::vector<uint64_t>> spare;
    // R6: the activations that list a storage or an event object, by its address (stale ones
    // are dropped as they are found), the last write of each storage, and the wakes owed.
    AddrMap<std::vector<uintptr_t>> watch;
    AddrMap<ChgFacts>               chg;
    std::vector<Owed>               owed;
    uint64_t                        step = 0;
    // R1, R2, R25: how many activations each ring holds, how many wait at a `#0` (by the batch their
    // thread's home names), and how many of the queued ones are a program's continuous processes.
    int32_t                         n_queued[4]    = {};
    int32_t                         n_zero[2]      = {};
    int32_t                         n_cont_queued  = 0;
    // R15, R17, R4: activations owed their first dispatch.
    int32_t                         zero_owed      = 0;
    int32_t                         proc_owed      = 0;
    int32_t                         strobes_owed   = 0;
    // The region whose bodies clotho.run_once is running (RT_TRACE_ORG_PREINIT ...), or zero.
    uint32_t                        once           = 0;
    // R23: the activations on each list of occupants, by the list (stale ones are skipped), the lists
    // a fork child's ramp linked before the spawn that announces it, and the occupants a `disable`
    // still has to reach.
    AddrMap<std::vector<uintptr_t>> occ;
    AddrMap<std::vector<uintptr_t>> ramp;
    std::vector<AbortOwed>          abort_owed;
    // R22: the forks, by join record (a new fork on an address starts a new one).
    AddrMap<Fork>                   forks;
    // R16: the inertial records by address and the sites by dense id; how many records are Live, how many fired
    // and wait for a commit, and how many of those the wheel fired (they are due before the Active region).
    AddrMap<InertRec>               inert;
    std::vector<InertSite>          inert_sites;
    int32_t                         inert_live          = 0;
    int32_t                         inert_waiting       = 0;
    int32_t                         inert_wheel_waiting = 0;
};

Checker g;

// What the handle's tag bits never are: bit 0 stays (a thunk group), bits 1 and 2 are the
// home region of a parked handle.
uintptr_t norm(const void* a) {
    return reinterpret_cast<uintptr_t>(a) & ~static_cast<uintptr_t>(6);
}

// The kind of a control or an origin, without the facts of a seed row (RT_TRACE_ORG_PROCEDURE ...).
uint32_t kind_of(const rt_trace_control* c) {
    return c != nullptr ? (c->kind & RT_TRACE_KIND) : ~0u;
}

// Whether the activation's origin carries `fact` (RT_TRACE_ORG_SELF_START ...).
bool has_fact(const Act& act, uint32_t fact) {
    return act.origin != nullptr && (act.origin->kind & fact) != 0;
}

// Whether the activation waits at a `#0`.
bool zero_parked(const Act& act) {
    return act.state == State::Parked && act.zero && act.park != nullptr && act.park->kind == RT_TRACE_CTL_DELAY;
}

// The counts of what each ring and each `#0` batch hold follow the state of each activation: a
// state is left with count(act, -1) and the next one entered with count(act, +1).
void count(const Act& act, int32_t by) {
    if (act.state == State::Queued) {
        g.n_queued[act.ring] += by;
        if (has_fact(act, RT_TRACE_ORG_REACTIVE_CONT)) {
            g.n_cont_queued += by;
        }
    } else if (zero_parked(act)) {
        g.n_zero[act.home_reactive ? 1 : 0] += by;
    }
}

void appendf(std::string& out, const char* format, ...) {
    char    buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    out += buf;
}

const char* control_name(uint32_t kind) {
    switch (kind & RT_TRACE_KIND) {
        case RT_TRACE_CTL_STATIC: return "static sensitivity control";
        case RT_TRACE_CTL_EVENT: return "event control";
        case RT_TRACE_CTL_DELAY: return "delay control";
        case RT_TRACE_CTL_JOIN: return "join";
        case RT_TRACE_CTL_CALL: return "task call";
        case RT_TRACE_CTL_WAIT_FORK: return "wait fork";
        case RT_TRACE_CTL_AWAIT: return "await";
        case RT_TRACE_CTL_SYNC: return "semaphore or mailbox wait";
        case RT_TRACE_CTL_EXPECT: return "expect";
        case RT_TRACE_CTL_SUSPENDED: return "process suspend";
        case RT_TRACE_ORG_PROCESS: return "process";
        case RT_TRACE_ORG_FORK: return "fork child";
        case RT_TRACE_ORG_TASK: return "task call";
        case RT_TRACE_ORG_PEND: return "assertion action block";
        case RT_TRACE_ORG_STROBE: return "$strobe";
        case RT_TRACE_ORG_MONITOR: return "$monitor";
        case RT_TRACE_ORG_EXPORT: return "DPI export call";
        case RT_TRACE_ORG_PREINIT: return "pre-init body";
        case RT_TRACE_ORG_PREPONED: return "Preponed body";
        case RT_TRACE_ORG_OBSERVED: return "Observed body";
        case RT_TRACE_ORG_RE_NBA: return "Re-NBA body";
        case RT_TRACE_ORG_FINAL: return "final block";
    }
    return "?";
}

const char* cause_name(uint32_t cause) {
    switch (cause) {
        case RT_TRACE_SEED: return "the seed";
        case RT_TRACE_SPAWN: return "a fork that queues its child";
        case RT_TRACE_PEND: return "an assertion that pends its action block";
        case RT_TRACE_STATIC: return "a static subscription";
        case RT_TRACE_DYNAMIC: return "a dynamic waiter";
        case RT_TRACE_VIF: return "a virtual interface waiter";
        case RT_TRACE_EVENT: return "an event trigger";
        case RT_TRACE_TIMER: return "a timer";
        case RT_TRACE_INACTIVE: return "the promotion of the #0 batch";
        case RT_TRACE_CHILD: return "a fork child's completion";
        case RT_TRACE_CALLEE: return "a blocked callee's completion";
        case RT_TRACE_FORCE: return "disable or kill";
        case RT_TRACE_SYNC: return "a semaphore or mailbox hand-over";
        case RT_TRACE_AWAIT: return "a thread's completion";
        case RT_TRACE_PROC: return "process::resume";
        case RT_TRACE_EXPECT: return "an expect verdict";
    }
    return "?";
}

const char* edge_name(uint64_t edge) {
    switch (edge) {
        case RT_TRACE_POS: return "posedge";
        case RT_TRACE_NEG: return "negedge";
        case RT_TRACE_ANY: return "any change of";
        case RT_TRACE_OBJ: return "event";
    }
    return "?";
}

const char* ev_name(Ev ev) {
    switch (ev) {
        case Ev::Spawn: return "spawn";
        case Ev::Park: return "park";
        case Ev::Wake: return "wake";
        case Ev::Dispatch: return "dispatch";
        case Ev::Resume: return "resume";
        case Ev::End: return "end";
        case Ev::Slot: return "slot";
        case Ev::SlotEnd: return "slot end";
        case Ev::Region: return "region";
        case Ev::Create: return "nba create";
        case Ev::Stage: return "nba stage";
        case Ev::Release: return "nba release";
        case Ev::PassBegin: return "nba pass begin";
        case Ev::PassEnd: return "nba pass end";
        case Ev::Pend: return "pend";
        case Ev::Trigger: return "trigger";
        case Ev::Finish: return "finish";
        case Ev::Change: return "change";
        case Ev::Block: return "block";
        case Ev::Disable: return "disable";
        case Ev::Abort: return "abort";
        case Ev::Fork: return "fork";
        case Ev::ChildDone: return "child done";
        case Ev::Join: return "join";
        case Ev::Member: return "member";
    }
    return "?";
}

// The events that name an activation.
bool is_act_event(Ev ev) {
    switch (ev) {
        case Ev::Spawn:
        case Ev::Park:
        case Ev::Wake:
        case Ev::Dispatch:
        case Ev::Resume:
        case Ev::End:
        case Ev::Block:
        case Ev::Disable:
        case Ev::Abort:
        case Ev::Fork:
        case Ev::ChildDone:
        case Ev::Join:
        case Ev::Member: return true;
        default: return false;
    }
}

const char* region_name(uint32_t r) {
    switch (r) {
        case RT_TRACE_REG_ACTIVE: return "active";
        case RT_TRACE_REG_INACTIVE: return "inactive";
        case RT_TRACE_REG_NBA: return "NBA";
        case RT_TRACE_REG_OBSERVED: return "observed";
        case RT_TRACE_REG_REACTIVE: return "reactive";
        case RT_TRACE_REG_RE_INACTIVE: return "re-inactive";
        case RT_TRACE_REG_RE_NBA: return "re-NBA";
        case RT_TRACE_REG_POSTPONED: return "postponed";
        case RT_TRACE_REG_PREPONED: return "top-of-slot";
        case RT_TRACE_REG_FINAL: return "final";
    }
    return "?";
}

const char* path_name(uint32_t p) {
    switch (p) {
        case RT_TRACE_NBA_BLOCK: return "plain NBA (block shadow)";
        case RT_TRACE_NBA_ARRAY: return "plain NBA (array element or interface member record)";
        case RT_TRACE_NBA_AGG: return "plain NBA (aggregate leaf record)";
        case RT_TRACE_NBA_ZERO: return "`<= #0`";
        case RT_TRACE_NBA_TIMER: return "`<= #d` released by its timer";
        case RT_TRACE_NBA_EVENT: return "`<= @e` released by its event";
    }
    return "?";
}

const char* chg_path_name(uint32_t p) {
    switch (p) {
        case RT_TRACE_CHG_STORE: return "store";
        case RT_TRACE_CHG_COMMIT: return "NBA commit";
        case RT_TRACE_CHG_WINDOW: return "wide-select store";
        case RT_TRACE_CHG_PROPERTY: return "class property store";
        case RT_TRACE_CHG_REF: return "ref write";
        case RT_TRACE_CHG_STRING: return "string compare";
        case RT_TRACE_CHG_LEVEL: return "level wake";
    }
    return "?";
}

// What a rule's id names, for the first line of a report.
const char* rule_title(const char* rule) {
    static const struct {
        const char* id;
        const char* title;
    } titles[] = {
        { "R5.1", "wake" },
        { "R5.2", "dispatch" },
        { "R5.3", "resume" },
        { "R5.4", "end" },
        { "R6.1", "wake owed" },
        { "R6.2", "wake without a change" },
        { "R6.3", "change not woken" },
        { "R7.1", "edge fact" },
        { "R7.2", "change fact" },
        { "R12.1", "nba order" },
        { "R12.2", "nba left unapplied" },
        { "R12.3", "nba pass" },
        { "R20.1", "->> region" },
        { "R20.2", "->> order" },
        { "R20.3", "->> left untriggered" },
        { "R24.2", "run after $finish" },
        { "R1.1", "region order" },
        { "R1.2", "region left with work" },
        { "R2.2", "#0 batch" },
        { "R2.3", "timer" },
        { "R4.1", "run-once region" },
        { "R4.2", "pended report" },
        { "R15.1", "time-zero start" },
        { "R17.1", "late start" },
        { "R23.1", "block occupancy" },
        { "R23.2", "disable reach" },
        { "R23.3", "inherited block" },
        { "R23.4", "abort obeyed" },
        { "R23.5", "force wake" },
        { "R25.1", "home region" },
        { "R25.2", "continuous before program" },
        { "R22.1", "child start" },
        { "R22.2", "child completion" },
        { "R22.3", "join wake" },
        { "R22.4", "join" },
        { "R5.5", "member run" },
        { "R6.4", "member run owed" },
        { "R16.1", "inertial decision" },
        { "R16.2", "inertial arm" },
        { "R16.3", "inertial fire" },
        { "R16.4", "inertial supersede" },
        { "R16.5", "inertial apply" },
        { "R11.1", "timed update" },
        { "R11.2", "event update" },
    };
    for (const auto& t : titles) {
        if (std::strcmp(rule, t.id) == 0) {
            return t.title;
        }
    }
    return "?";
}

const char* how_name(uint32_t how) {
    switch (how) {
        case RT_TRACE_RING: return "from a ring";
        case RT_TRACE_INLINE: return "inline";
        case RT_TRACE_RUN_ONCE: return "run once";
    }
    return "?";
}

const char* block_how_name(uint32_t how) {
    switch (how) {
        case RT_TRACE_BLOCK_ENTER: return "enters";
        case RT_TRACE_BLOCK_EXIT: return "leaves";
        case RT_TRACE_BLOCK_INHERIT: return "is within";
        case RT_TRACE_BLOCK_RAMP: return "links in its ramp";
    }
    return "?";
}

const char* queue_name(uint32_t q) {
    switch (q) {
        case RT_TRACE_QUEUE_ACTIVE: return "active";
        case RT_TRACE_QUEUE_REACTIVE: return "reactive";
        case RT_TRACE_QUEUE_CONT: return "reactive continuous";
    }
    return "?";
}

const char* join_name(uint32_t kind) {
    switch (kind) {
        case RT_TRACE_JOIN_ALL: return "join";
        case RT_TRACE_JOIN_ANY: return "join_any";
        case RT_TRACE_JOIN_NONE: return "join_none";
    }
    return "?";
}

std::string signal_text(const void* x) {
    char buf[32];
    if (const char* const* n = g.names.find(reinterpret_cast<uintptr_t>(x)); n != nullptr && *n != nullptr) {
        std::snprintf(buf, sizeof(buf), " (0x%llx)", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(x)));
        return std::string(*n) + buf;
    }
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(x)));
    return buf;
}

std::string control_text(const rt_trace_control* c) {
    if (c == nullptr) {
        return "no control";
    }
    return std::string(control_name(c->kind)) + " at " + (c->where != nullptr ? c->where : "<unknown>");
}

std::string address_text(uintptr_t a) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(a));
    return buf;
}

std::string list_text(const std::vector<Entry>& v) {
    if (v.empty()) {
        return "nothing";
    }
    std::string out;
    for (size_t i = 0; i < v.size() && i < 12; ++i) {
        out += i == 0 ? "" : ", ";
        out += std::string(edge_name(v[i].edge)) + " " + signal_text(v[i].x);
    }
    return v.size() > 12 ? out + ", ..." : out;
}

// "the process at design.sv:3:1 (0x...)": what an activation is, for a report.
std::string act_text(uintptr_t a) {
    std::string out = address_text(a);
    const Act*  act = g.acts.find(a);
    if (act == nullptr) {
        return out + " (never spawned)";
    }
    out += act->origin != nullptr ? " (" + control_text(act->origin) + ")" : " (origin not reported)";
    if (act->parent != 0) {
        const Act* parent = g.acts.find(act->parent);
        out += " created by " + address_text(act->parent);
        if (parent != nullptr && parent->origin != nullptr) {
            out += " (" + control_text(parent->origin) + ")";
        }
    }
    return out;
}

std::string rec_text(const Rec& r) {
    std::string out;
    appendf(out, "t=%llu %s", static_cast<unsigned long long>(r.now), ev_name(r.ev));
    if (is_act_event(r.ev)) {
        out += " " + address_text(static_cast<uintptr_t>(r.a));
    }
    switch (r.ev) {
        case Ev::Wake:
            out += std::string(" by ") + cause_name(r.cause);
            if (r.cause == RT_TRACE_STATIC || r.cause == RT_TRACE_DYNAMIC || r.cause == RT_TRACE_VIF) {
                out += std::string(": ") + edge_name(r.fact) + " " + signal_text(reinterpret_cast<const void*>(r.x));
            } else if (r.cause == RT_TRACE_EVENT) {
                out += ": event " + signal_text(reinterpret_cast<const void*>(r.x));
            }
            break;
        case Ev::Dispatch: out += std::string(" ") + how_name(r.cause); break;
        case Ev::Park:
        case Ev::Resume: out += " at " + control_text(r.c); break;
        case Ev::Region: out += std::string(" ") + region_name(r.cause); break;
        case Ev::PassBegin:
        case Ev::PassEnd: out += std::string(" of the ") + region_name(r.cause) + " region"; break;
        case Ev::Create:
        case Ev::Stage:
        case Ev::Release:
            out += std::string(": ") + path_name(r.cause) + " to " + signal_text(reinterpret_cast<const void*>(r.a));
            if (r.site != nullptr && r.site->where != nullptr) {
                out += std::string(" by the statement at ") + r.site->where;
            }
            break;
        case Ev::Pend: out += " event " + signal_text(reinterpret_cast<const void*>(r.a)); break;
        case Ev::Trigger:
            out += std::string(r.cause == RT_TRACE_TRIGGER_NB ? " ->> " : " -> ") + "event " +
                   signal_text(reinterpret_cast<const void*>(r.a));
            break;
        case Ev::Change:
            out += " of " + signal_text(reinterpret_cast<const void*>(r.a)) + " (" + chg_path_name(r.cause) + ")";
            break;
        case Ev::Block: out += std::string(" ") + block_how_name(r.cause) + " the list at " + address_text(r.x); break;
        case Ev::Disable: out += " of the list at " + address_text(r.x); break;
        case Ev::Abort:
            out += r.cause == RT_TRACE_ABORT_KILL ? ": marked by a kill" : ": marked by a disable of the list at " + address_text(r.x);
            break;
        case Ev::Fork: out += std::string(": ") + join_name(r.cause) + " on the record at " + address_text(r.x); break;
        case Ev::ChildDone: out += " into the record at " + address_text(r.x); break;
        case Ev::Join: out += " past the join of the record at " + address_text(r.x); break;
        case Ev::Member: appendf(out, ": member %llu starts", static_cast<unsigned long long>(r.x)); break;
        default: break;
    }
    return out;
}

void record(Ev ev, uintptr_t a, uint64_t x, const rt_trace_control* c, uint8_t cause, uint8_t fact,
            const rt_trace_nba_site* site = nullptr, uint8_t queue = 0) {
    g.ring[g.seq % kRing] = Rec{ a, x, g.now, c, site, ev, cause, fact, queue };
    ++g.seq;
    ++g.run.events;
    ++g.total.events;
}

// The first violation: the rule, what broke it, and the activation's and the slot's last
// events. Ends the run: every later event is ignored and every dispatch refused.
void fail(const char* rule, const std::string& what, uintptr_t a, const std::string& cause) {
    g.halted = true;
    g.rule   = rule;
    g.report.clear();
    const char* title = rule_title(rule);
    appendf(g.report, "rule %s (%s): ", rule, title);
    g.report += what + "\n";
    appendf(g.report, "  time:       tick %llu\n", static_cast<unsigned long long>(g.now));
    if (a != 0) {
        g.report += "  activation: " + act_text(a) + "\n";
        if (const Act* act = g.acts.find(a); act != nullptr) {
            g.report += std::string("  state:      ") + state_name(act->state);
            if (act->state == State::Parked && act->park != nullptr) {
                g.report += " at " + control_text(act->park);
                const uint32_t kind = act->park->kind;
                if (kind == RT_TRACE_CTL_STATIC) {
                    g.report += ", waiting for " + list_text(act->stat);
                } else if (kind == RT_TRACE_CTL_EVENT) {
                    g.report += ", waiting for " + list_text(act->list);
                } else if (kind == RT_TRACE_CTL_DELAY) {
                    appendf(g.report, act->zero ? ", for the #0 region" : ", until tick %llu",
                            static_cast<unsigned long long>(act->deadline));
                }
            }
            g.report += "\n";
        }
    }
    if (!cause.empty()) {
        g.report += "  cause:      " + cause + "\n";
    }
    const uint64_t first = g.seq > kRing ? g.seq - kRing : 0;
    if (a != 0) {
        g.report += "  last events of this activation:\n";
        int shown = 0;
        for (uint64_t i = g.seq; i > first && shown < 8; --i) {
            const Rec& r = g.ring[(i - 1) % kRing];
            if (is_act_event(r.ev) && r.a == a) {
                g.report += "    " + rec_text(r) + "\n";
                ++shown;
            }
        }
    }
    g.report += "  last events of the run:\n";
    int shown = 0;
    for (uint64_t i = g.seq; i > first && shown < 12; --i, ++shown) {
        g.report += "    " + rec_text(g.ring[(i - 1) % kRing]) + "\n";
    }
}

// The activation an event names, or a violation of the trace's own contract when no spawn
// announced it: a ramp the backend does not report.
Act* find(const char* rule, uintptr_t a, const char* what) {
    Act* act = a != 0 ? g.acts.find(a) : nullptr;
    if (act == nullptr) {
        fail(rule, std::string(what) + " of an activation that no spawn announced: a ramp the trace does not report", a, "");
    }
    return act;
}

// A signal cause (x, fact) matches an entry (x, edge) when the fact implies the edge: a
// posedge is a posedge, an edge and a change; a negedge likewise; a change only a change.
bool listed(const std::vector<Entry>& v, const void* x, uint32_t fact) {
    for (const Entry& e : v) {
        if (e.x != x) {
            continue;
        }
        if (fact == RT_TRACE_OBJ ? e.edge == RT_TRACE_OBJ : (e.edge != RT_TRACE_OBJ && (e.edge == RT_TRACE_ANY || e.edge == fact))) {
            return true;
        }
    }
    return false;
}

// Whether a wake names another record or queue than the park it reaches waits on (B43). Either
// side naming none says nothing.
bool other_record(const Act& act, const void* x) {
    return act.rec != 0 && x != nullptr && reinterpret_cast<uintptr_t>(x) != act.rec;
}

// Whether the completion just counted is the one the join of `f` is due to wake its parent at: the
// last of a `join`, the first of a `join_any`, and none of a `join_none`, whose parent never parks.
bool join_due(const Fork& f) {
    return f.kind == RT_TRACE_JOIN_ALL ? f.done == f.width : (f.kind == RT_TRACE_JOIN_ANY && f.done == 1);
}

// Why the child may not be resumed at its creation, or empty (R22.1): a `join_none` child starts when
// its parent blocks or ends, and a `join_any` sibling is not run once the parent is due to go on.
std::string child_start_refusal(const Act& act) {
    const Fork* f = act.member_of != 0 ? g.forks.find(act.member_of) : nullptr;
    if (f == nullptr) {
        return "";
    }
    if (f->kind == RT_TRACE_JOIN_NONE) {
        return "a join_none child ran at the fork: it starts when its parent blocks or ends";
    }
    if (f->kind == RT_TRACE_JOIN_ANY && f->done != 0) {
        return "a join_any sibling ran after the first child had completed: the parent goes on, and the others start when it blocks or ends";
    }
    return "";
}

// Empty when `cause` may wake `act`, parked as it is; otherwise why not.
std::string refusal(const Act& act, uint32_t cause, const void* x, uint32_t fact) {
    const rt_trace_control* c    = act.park;
    const uint32_t          kind = c != nullptr ? c->kind : ~0u;
    switch (cause) {
        case RT_TRACE_STATIC:
            if (kind != RT_TRACE_CTL_STATIC) {
                return "a static subscription woke an activation that is not parked at its static control";
            }
            return listed(act.stat, x, fact) ? "" : "the static control does not list this change";
        case RT_TRACE_DYNAMIC:
        case RT_TRACE_VIF:
            if (kind != RT_TRACE_CTL_EVENT) {
                return "a waiter's registration woke an activation that is not parked at an event control";
            }
            return listed(act.list, x, fact) ? "" : "the event control does not list this change";
        case RT_TRACE_EVENT:
            if (kind == RT_TRACE_CTL_EVENT) {
                return listed(act.list, x, RT_TRACE_OBJ) ? "" : "the event control does not list this event";
            }
            if (kind == RT_TRACE_CTL_STATIC) {
                return listed(act.stat, x, RT_TRACE_OBJ) ? "" : "the static control does not list this event";
            }
            return "an event woke an activation that is not parked at an event control";
        case RT_TRACE_TIMER:
            if (kind != RT_TRACE_CTL_DELAY || act.zero) {
                return "a timer woke an activation that is not parked at a #d delay";
            }
            if (act.deadline != g.now) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "the delay ends at tick %llu, not at tick %llu",
                              static_cast<unsigned long long>(act.deadline), static_cast<unsigned long long>(g.now));
                return buf;
            }
            return "";
        case RT_TRACE_INACTIVE:
            return kind == RT_TRACE_CTL_DELAY && act.zero ? "" : "the #0 batch woke an activation that is not parked at a #0";
        case RT_TRACE_CHILD:
            if (kind != RT_TRACE_CTL_JOIN) {
                return "a fork child woke a parent that is not parked at a join";
            }
            return other_record(act, x) ? "a fork child woke a parent that is parked at the join of another fork" : "";
        case RT_TRACE_CALLEE:
            if (kind != RT_TRACE_CTL_CALL) {
                return "a callee woke a caller that is not parked at its task call";
            }
            return other_record(act, x) ? "a callee woke a caller that is parked on the record of another task call" : "";
        case RT_TRACE_AWAIT:
            if (kind != RT_TRACE_CTL_AWAIT && kind != RT_TRACE_CTL_WAIT_FORK) {
                return "a thread's completion woke an activation that is not parked at an await or a wait fork";
            }
            return kind == RT_TRACE_CTL_AWAIT && other_record(act, x)
                     ? "a thread's completion woke an await on another thread"
                     : "";
        case RT_TRACE_SYNC:
            if (kind != RT_TRACE_CTL_SYNC) {
                return "a semaphore or mailbox woke an activation that is not parked at its queue";
            }
            return other_record(act, x) ? "a semaphore or mailbox woke a waiter of another queue" : "";
        case RT_TRACE_EXPECT:
            if (kind != RT_TRACE_CTL_EXPECT) {
                return "an expect verdict woke an activation that is not parked at an expect";
            }
            return other_record(act, x) ? "an expect verdict woke an expect of another instance" : "";
        case RT_TRACE_PROC:
            return kind == RT_TRACE_CTL_SUSPENDED ? "" : "process::resume woke a thread that is not parked by suspend";
        case RT_TRACE_FORCE:
            return "";
        default:
            return std::string(cause_name(cause)) + " woke an activation that has been parked";
    }
}

std::string cause_text(uint32_t cause, const void* x, uint32_t fact) {
    std::string out = cause_name(cause);
    if (cause == RT_TRACE_STATIC || cause == RT_TRACE_DYNAMIC || cause == RT_TRACE_VIF) {
        out += std::string(": ") + edge_name(fact) + " " + signal_text(x);
    } else if (cause == RT_TRACE_EVENT) {
        out += ": event " + signal_text(x);
    } else if (cause == RT_TRACE_TIMER) {
        appendf(out, ": expired at tick %llu", static_cast<unsigned long long>(g.now));
    } else if (x != nullptr && (cause == RT_TRACE_CHILD || cause == RT_TRACE_CALLEE || cause == RT_TRACE_AWAIT ||
                                cause == RT_TRACE_SYNC || cause == RT_TRACE_EXPECT)) {
        out += ": of the record or queue at " + address_text(reinterpret_cast<uintptr_t>(x));
    }
    return out;
}

void add_entries(std::vector<Entry>& to, const rt_trace_entry* list, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        to.push_back(Entry{ list[i].x, list[i].name, list[i].edge, list[i].shape, {} });
        if (list[i].name != nullptr) {
            g.names.at(reinterpret_cast<uintptr_t>(list[i].x)) = list[i].name;
        }
    }
}

// -- Nonblocking updates, `->>` and `$finish` (R12, R20, R24) --

std::vector<uint64_t> take_words() {
    if (g.spare.empty()) {
        return {};
    }
    std::vector<uint64_t> w = std::move(g.spare.back());
    g.spare.pop_back();
    return w;
}

// A run begins for the checker with the first event the generated code reports. The scheduler's own
// events (the release of a record, the end of a slot, a `$finish`) say nothing of a run whose code
// reports none, which is how a run with CLOTHO_NO_TRACE_CHECK=1 looks in a build that has the checker.
void touch_run() {
    if (!g.counted) {
        g.counted = true;
        ++g.total.runs;
    }
}

void retire(Upd& u) {
    if (u.w.capacity() != 0 && g.spare.size() < 4096) {
        u.w.clear();
        g.spare.push_back(std::move(u.w));
    }
}

// The wake that a cooperative exit makes after a `$finish`: the completion of a callee, a fork
// child or a thread, handing control back so that each frame leaves through its own epilogue.
bool completion_cause(uint32_t cause) {
    return cause == RT_TRACE_CHILD || cause == RT_TRACE_CALLEE || cause == RT_TRACE_AWAIT;
}

// The regions the Reactive set runs: updates and `->>` created there belong to its Re-NBA region.
bool reactive_set(uint32_t region) {
    return region == RT_TRACE_REG_OBSERVED || region == RT_TRACE_REG_REACTIVE || region == RT_TRACE_REG_RE_INACTIVE ||
           region == RT_TRACE_REG_RE_NBA;
}

std::string hex_text(const uint64_t* w, uint32_t n) {
    if (n == 0) {
        return "0x0";
    }
    uint32_t top = n;
    while (top > 1 && w[top - 1] == 0) {
        --top;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(w[top - 1]));
    std::string out = buf;
    for (uint32_t i = top - 1; i-- > 0;) {
        if (out.size() > 70) {
            return out + "...";
        }
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(w[i]));
        out += buf;
    }
    return out;
}

std::string hex_text(const std::vector<uint64_t>& w) {
    return hex_text(w.data(), static_cast<uint32_t>(w.size()));
}

// "plain NBA ... by the statement at design.sv:5:3, created at tick 4 in the active region ...".
std::string upd_text(const Upd& u) {
    std::string out = path_name(u.path);
    out += std::string(" by the statement at ") + (u.site->where != nullptr ? u.site->where : "<unknown>");
    appendf(out, ", created at tick %llu in the %s region", static_cast<unsigned long long>(u.tick), region_name(u.region));
    if ((u.site->flags & RT_TRACE_NBA_OPAQUE) != 0) {
        return out + ": its bits are not known to the trace";
    }
    out += ": value " + hex_text(u.val(0), words_of(u.bits(0))) + ", mask " + hex_text(u.mask(0), words_of(u.bits(0)));
    if (u.dst[1] != 0) {
        out += "; z word " + hex_text(u.val(1), words_of(u.bits(1))) + ", mask " + hex_text(u.mask(1), words_of(u.bits(1)));
    }
    return out;
}

uint64_t load_word(const void* p, uint32_t i) {
    uint64_t w;
    std::memcpy(&w, static_cast<const unsigned char*>(p) + static_cast<size_t>(i) * 8, sizeof(w));
    return w;
}

// One plane of an update from the word buffers the code filled: a null value is zero and a null
// mask is every bit. The bits above the plane's width are zero.
void copy_plane(uint64_t* out, const void* val, const void* mask, uint32_t bits) {
    const uint32_t n = words_of(bits);
    if (n == 0) {
        return;
    }
    const uint64_t top = bits % 64 != 0 ? (uint64_t{ 1 } << (bits % 64)) - 1 : ~uint64_t{ 0 };
    for (uint32_t i = 0; i < n; ++i) {
        out[i]     = val != nullptr ? load_word(val, i) : 0;
        out[n + i] = mask != nullptr ? load_word(mask, i) : ~uint64_t{ 0 };
    }
    out[n - 1] &= top;
    out[2 * n - 1] &= top;
}

Upd make_upd(const rt_trace_nba_site* site, const void* dst0, const void* dst1, const void* val0, const void* mask0,
             const void* val1, const void* mask1) {
    Upd u;
    u.site         = site;
    u.path         = site->flags & 0xff;
    u.dst[0]       = reinterpret_cast<uintptr_t>(dst0);
    u.dst[1]       = site->bits1 != 0 ? reinterpret_cast<uintptr_t>(dst1) : 0;
    const uint32_t n0 = words_of(site->bits0);
    const uint32_t n1 = u.dst[1] != 0 ? words_of(site->bits1) : 0;
    u.w            = take_words();
    u.w.assign(static_cast<size_t>(2) * (n0 + n1), 0);
    copy_plane(u.w.data(), val0, mask0, site->bits0);
    if (n1 != 0) {
        copy_plane(u.w.data() + 2 * n0, val1, mask1, site->bits1);
    }
    return u;
}

// The bits of a plane of design memory, as `words_of(bits)` words.
void read_plane(uintptr_t dst, uint32_t bits, std::vector<uint64_t>& out) {
    out.assign(words_of(bits), 0);
    if (bits == 0) {
        return;
    }
    std::memcpy(out.data(), reinterpret_cast<const void*>(dst), (bits + 7) / 8);
    if (bits % 64 != 0) {
        out.back() &= (uint64_t{ 1 } << (bits % 64)) - 1;
    }
}

// cur = (cur & ~mask) | (value & mask) for plane `p` of `u`.
void apply_plane(std::vector<uint64_t>& cur, const Upd& u, int p) {
    const uint64_t* v = u.val(p);
    const uint64_t* m = u.mask(p);
    for (size_t i = 0; i < cur.size(); ++i) {
        cur[i] = (cur[i] & ~m[i]) | (v[i] & m[i]);
    }
}

bool skips(const Upd& u) {
    return (u.site->flags & (RT_TRACE_NBA_GATED | RT_TRACE_NBA_OPAQUE)) != 0;
}

// Within each run of consecutive updates from `from` on that share a release moment, order them by
// the order their statements armed them: updates created at one moment are performed in the order
// their statements executed (LRM_NOTES 51). Every other update is a moment of its own.
void sort_moments(std::vector<Upd>& v, size_t from) {
    for (size_t i = from; i < v.size();) {
        size_t j = i + 1;
        if (v[i].moment != 0) {
            while (j < v.size() && v[j].moment == v[i].moment) {
                ++j;
            }
        }
        // An insertion sort: a run is short, and it is stable and calls no library (the executable
        // runtime links none: libc++'s stable_sort wants a nothrow operator new).
        for (size_t a = i + 1; a < j; ++a) {
            for (size_t b = a; b > i && v[b - 1].arm > v[b].arm; --b) {
                std::swap(v[b - 1], v[b]);
            }
        }
        i = j;
    }
}

// The key of the plane at `dst` in the pass in progress, made (and its bytes read) when no update
// of the pass has named it yet. A word two updates name with different widths is not compared.
uint32_t key_index(uintptr_t dst, uint32_t bits, bool skip) {
    const auto first = g.keys.begin();
    const auto mid   = first + static_cast<std::ptrdiff_t>(g.sorted_keys);
    auto       found = std::lower_bound(first, mid, dst, [](const Key& k, uintptr_t d) { return k.dst < d; });
    if (found == mid || found->dst != dst) {
        found = std::find_if(mid, g.keys.end(), [dst](const Key& k) { return k.dst == dst; });
    }
    if (found != g.keys.end() && found->dst == dst) {
        if (skip || found->bits != bits) {
            found->skip = true;
        }
        return static_cast<uint32_t>(found - first);
    }
    g.keys.push_back(Key{ dst, bits, skip, {} });
    read_plane(dst, bits, g.keys.back().base);
    return static_cast<uint32_t>(g.keys.size() - 1);
}

// The pass begins: the updates created so far are the ones it must apply, in creation order, and
// the planes they write are read as they are now.
void begin_pass() {
    sort_moments(g.pending, 0);
    g.split = g.pending.size();
    g.keys.clear();
    for (const Upd& u : g.pending) {
        for (int p = 0; p < 2; ++p) {
            if (u.dst[p] != 0) {
                g.keys.push_back(Key{ u.dst[p], u.bits(p), skips(u), {} });
            }
        }
    }
    std::sort(g.keys.begin(), g.keys.end(), [](const Key& a, const Key& b) { return a.dst < b.dst; });
    size_t kept = 0;
    for (size_t i = 0; i < g.keys.size(); ++i) {
        if (kept > 0 && g.keys[kept - 1].dst == g.keys[i].dst) {
            g.keys[kept - 1].skip = g.keys[kept - 1].skip || g.keys[i].skip || g.keys[kept - 1].bits != g.keys[i].bits;
        } else {
            g.keys[kept++] = std::move(g.keys[i]);
        }
    }
    g.keys.resize(kept);
    for (Key& k : g.keys) {
        read_plane(k.dst, k.bits, k.base);
    }
    g.sorted_keys = g.keys.size();
    for (Upd& u : g.pending) {
        for (int p = 0; p < 2; ++p) {
            if (u.dst[p] != 0) {
                u.key[p] = key_index(u.dst[p], u.bits(p), skips(u));
            }
        }
    }
}

// One plane of one update, in the order the pass applies them to its target.
struct Ent {
    uint32_t key;
    uint32_t idx;
    uint8_t  plane;
};

// The words of one plane that the updates give in creation order: the ones created before the pass
// began (`ents` from `e` up to `m`), then the ones released during it up to `upto` (to `f`).
std::vector<uint64_t> expected_plane(const Key& k, const std::vector<Ent>& ents, size_t e, size_t m, size_t upto) {
    std::vector<uint64_t> cur = k.base;
    for (size_t t = e; t < upto; ++t) {
        (void)m;
        apply_plane(cur, g.pending[ents[t].idx], ents[t].plane);
    }
    return cur;
}

void fail_order(const Key& k, const std::vector<Ent>& ents, size_t e, size_t m, size_t f, const std::vector<uint64_t>& mem) {
    const std::string who = signal_text(reinterpret_cast<const void*>(k.dst)) + (ents[e].plane == 0 ? "" : " (z word)");
    std::string what;
    appendf(what, "after the NBA pass at tick %llu, ", static_cast<unsigned long long>(g.now));
    what += who + " does not hold what its updates give in creation order";
    std::string cause = "the pass began with " + hex_text(k.base) + " and ended with " + hex_text(mem) +
                        "; the updates created before it began give " + hex_text(expected_plane(k, ents, e, m, m));
    if (m < f) {
        cause += ", and with the ones released during it " + hex_text(expected_plane(k, ents, e, m, f));
    }
    cause += "\n    its updates, in creation order:";
    for (size_t t = e; t < f; ++t) {
        appendf(cause, "\n      %zu. ", t - e + 1);
        cause += upd_text(g.pending[ents[t].idx]);
        cause += t >= m ? " (released during the pass)" : "";
    }
    fail("R12.1", what, 0, cause);
}

// The pass ends: each plane an update writes must hold what the updates that were created before
// the pass began give, applied in creation order, followed by a prefix of the ones released during
// it, which the commit of their own site may or may not have reached. The ones it did not reach stay
// pending for the next pass (B42).
void settle_pass() {
    std::vector<Upd>& pend = g.pending;
    sort_moments(pend, g.split);
    std::vector<Ent> ents;
    ents.reserve(pend.size() * 2);
    for (uint32_t i = 0; i < pend.size(); ++i) {
        for (int p = 0; p < 2; ++p) {
            if (pend[i].dst[p] != 0) {
                ents.push_back(Ent{ pend[i].key[p], i, static_cast<uint8_t>(p) });
            }
        }
    }
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.key != b.key ? a.key < b.key : a.idx < b.idx; });
    // What each plane of each update released during the pass says of whether the pass applied it:
    // the plane holds what the updates give with it (applied) or without it (not applied), or the
    // same either way, because the update changed nothing there.
    std::vector<uint8_t>  forced_applied(pend.size(), 0);
    std::vector<uint8_t>  forced_not(pend.size(), 0);
    // The update released during the pass before it on the same plane, or -1.
    std::vector<int32_t>  before(pend.size() * 2, -1);
    std::vector<uint64_t> cur;
    std::vector<uint64_t> mem;
    for (size_t e = 0; e < ents.size();) {
        size_t f = e;
        while (f < ents.size() && ents[f].key == ents[e].key) {
            ++f;
        }
        const Key& k = g.keys[ents[e].key];
        size_t     m = e;
        while (m < f && ents[m].idx < g.split) {
            ++m;
        }
        if (k.skip) {
            e = f;
            continue;
        }
        cur = k.base;
        for (size_t t = e; t < m; ++t) {
            apply_plane(cur, pend[ents[t].idx], ents[t].plane);
        }
        read_plane(k.dst, k.bits, mem);
        // The counts of the updates released during the pass that the plane's bytes are consistent
        // with: the least and the most.
        long least = -1;
        long most  = -1;
        if (cur == mem) {
            least = most = 0;
        }
        for (size_t t = m; t < f; ++t) {
            apply_plane(cur, pend[ents[t].idx], ents[t].plane);
            if (cur == mem) {
                most = static_cast<long>(t - m) + 1;
                least = least < 0 ? most : least;
            }
        }
        if (least < 0) {
            fail_order(k, ents, e, m, f, mem);
            return;
        }
        for (size_t t = m; t < f; ++t) {
            const long     v = static_cast<long>(t - m);
            const uint32_t i = ents[t].idx;
            forced_applied[i] = forced_applied[i] != 0 || least > v ? 1 : 0;
            forced_not[i]     = forced_not[i] != 0 || most <= v ? 1 : 0;
            if (t > m) {
                before[static_cast<size_t>(i) * 2 + ents[t].plane] = static_cast<int32_t>(ents[t - 1].idx);
            }
        }
        e = f;
    }
    // Decided in creation order: an update was applied unless a plane shows it was not, or an update
    // before it on a plane it names was not (the commit applies updates in creation order); an update
    // that no plane tells either way was applied, which only a no-op can be.
    std::vector<uint8_t> applied(pend.size(), 1);
    for (size_t i = g.split; i < pend.size(); ++i) {
        bool can = true;
        for (int p = 0; p < 2; ++p) {
            const int32_t b = before[i * 2 + static_cast<size_t>(p)];
            can             = can && (b < 0 || applied[static_cast<size_t>(b)] != 0);
        }
        if (forced_applied[i] != 0 && forced_not[i] != 0) {
            fail("R12.3", std::string("an update was applied to one plane of its target and not to the other: ") + upd_text(pend[i]), 0,
                 "");
            return;
        }
        if (forced_applied[i] != 0 && !can) {
            fail("R12.3",
                 std::string("an update was applied while an update created before it for the same plane was not: ") + upd_text(pend[i]),
                 0, "");
            return;
        }
        applied[i] = forced_not[i] == 0 && can ? 1 : 0;
    }
    std::vector<Upd> next;
    for (size_t i = 0; i < pend.size(); ++i) {
        if (i >= g.split && applied[i] == 0) {
            next.push_back(std::move(pend[i]));
        } else {
            retire(pend[i]);
        }
    }
    pend.swap(next);
    g.split = 0;
    g.keys.clear();
    g.sorted_keys = 0;
}

// The first inertial record that fired and waits for its site's commit, for a report.
std::string inert_waiting_text() {
    std::string out;
    g.inert.for_each([&](uintptr_t, InertRec& r) {
        if (out.empty() && r.state == IState::Fired) {
            appendf(out, "site %u: armed at tick %llu for tick %llu, fired, not applied", r.idx,
                    static_cast<unsigned long long>(r.armed_at), static_cast<unsigned long long>(r.deadline));
        }
    });
    return out;
}

// R16: at the end of a slot, a record whose deadline is this tick or past and that neither the wheel fired nor a
// later store superseded was lost by the wheel. Returns whether it failed.
bool poll_inertial() {
    if (g.inert_live == 0 || g.finished) {
        return false;
    }
    bool failed = false;
    g.inert.for_each([&](uintptr_t, InertRec& r) {
        if (failed || r.state != IState::Live || r.deadline == UINT64_MAX || r.deadline > g.now) {
            return;
        }
        std::string text;
        appendf(text, "site %u: armed at tick %llu for tick %llu, and the slot of tick %llu is over", r.idx,
                static_cast<unsigned long long>(r.armed_at), static_cast<unsigned long long>(r.deadline),
                static_cast<unsigned long long>(g.now));
        fail("R16.3", "an inertial record was never fired at its deadline: the wheel lost it", 0, text);
        failed = true;
    });
    return failed;
}

// Nothing may be pending at a point the regions that apply NBAs and trigger `->>` are behind:
// `what` names it. With `active_only`, only what the Active set created is owed (the Reactive
// set's Re-NBA region is still ahead). Returns whether it failed.
bool closure(const char* what, bool active_only) {
    for (const Upd& u : g.pending) {
        if (!active_only || !reactive_set(u.region)) {
            fail("R12.2", std::string("an NBA update is still unapplied when ") + what, 0,
                 "created: " + upd_text(u) + "; no NBA pass applied it");
            return true;
        }
    }
    for (const Pend& p : g.pends) {
        if (!active_only || !reactive_set(p.region)) {
            std::string cause;
            appendf(cause, "pended at tick %llu in the %s region: event ", static_cast<unsigned long long>(p.tick),
                    region_name(p.region));
            cause += signal_text(reinterpret_cast<const void*>(p.obj));
            fail("R20.3", std::string("a `->>` is still untriggered when ") + what, 0, cause);
            return true;
        }
    }
    if (!active_only && g.inert_waiting > 0 && !g.finished) {
        fail("R16.5", std::string("an inertial update that was fired is still unapplied when ") + what, 0, inert_waiting_text());
        return true;
    }
    return false;
}

// -- Waiters, writes and design memory (R6, R7) --

uint32_t rank_of(bool must, bool may) {
    return must ? 2 : (may ? 1 : 0);
}

const char* rank_name(uint32_t rank) {
    return rank == 2 ? "1" : (rank == 1 ? "x or z" : "0");
}

// The value bits at `x`, as the shape says: for each element the must|may word then the z word,
// the bytes past the last bit cleared.
void read_value(const void* x, const rt_trace_shape* s, std::vector<uint8_t>& out) {
    out.clear();
    const uint32_t n0 = (s->bits0 + 7) / 8;
    const uint32_t n1 = (s->bits1 + 7) / 8;
    const auto     put = [&](const uint8_t* p, uint32_t bits, uint32_t n) {
        const size_t at = out.size();
        out.insert(out.end(), p, p + n);
        if (n != 0 && bits % 8 != 0) {
            out[at + n - 1] &= static_cast<uint8_t>((1u << (bits % 8)) - 1);
        }
    };
    for (uint32_t e = 0; e < s->count; ++e) {
        const uint8_t* p = static_cast<const uint8_t*>(x) + static_cast<size_t>(e) * s->stride;
        put(p, s->bits0, n0);
        if (n1 != 0) {
            put(p + s->z_off, s->bits1, n1);
        }
    }
}

// The rank of bit 0 of the first element of a value read_value took.
uint32_t snap_rank(const rt_trace_shape* s, const std::vector<uint8_t>& v) {
    if (v.empty()) {
        return 0;
    }
    const bool must = (v[0] & 1) != 0;
    bool       may  = false;
    if (s->bits0 > s->width) {
        may = ((v[s->width / 8] >> (s->width % 8)) & 1) != 0;
    }
    return rank_of(must, may);
}

// Whether the change from the bytes an entry saw at its park to `now` satisfies its edge.
bool satisfied(const Entry& e, const std::vector<uint8_t>& now) {
    if (e.edge == RT_TRACE_ANY) {
        return e.snap != now;
    }
    if (e.edge != RT_TRACE_POS && e.edge != RT_TRACE_NEG) {
        return false;
    }
    const uint32_t ro = snap_rank(e.shape, e.snap);
    const uint32_t rn = snap_rank(e.shape, now);
    return e.edge == RT_TRACE_POS ? rn > ro : rn < ro;
}

std::string bytes_text(const std::vector<uint8_t>& v) {
    std::string out = "0x";
    char        buf[4];
    for (size_t i = 0; i < v.size() && i < 16; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", v[v.size() - 1 - i]);
        out += buf;
    }
    return v.size() > 16 ? out + "..." : out;
}

void snapshot_entry(Entry& e) {
    if (e.shape != nullptr) {
        read_value(e.x, e.shape, e.snap);
    } else {
        e.snap.clear();
    }
}

// Whether the activation still has a reason to be woken by `x`: its static list names it, or it is
// parked at an event control that does.
bool lists_x(const Act& act, uintptr_t x) {
    for (const Entry& e : act.stat) {
        if (reinterpret_cast<uintptr_t>(e.x) == x) {
            return true;
        }
    }
    if (act.state == State::Parked && act.park != nullptr && act.park->kind == RT_TRACE_CTL_EVENT) {
        for (const Entry& e : act.list) {
            if (reinterpret_cast<uintptr_t>(e.x) == x) {
                return true;
            }
        }
    }
    return false;
}

void watch_add(const void* x_, uintptr_t a) {
    const uintptr_t x = reinterpret_cast<uintptr_t>(x_);
    if (x == 0) {
        return;
    }
    std::vector<uintptr_t>& v = g.watch.at(x);
    if (!v.empty() && v.back() == a) {
        return;
    }
    const size_t n = v.size();
    if (n >= 16 && (n & (n - 1)) == 0) {
        size_t kept = 0;
        for (const uintptr_t w : v) {
            const Act* act = g.acts.find(w);
            if (act != nullptr && lists_x(*act, x)) {
                v[kept++] = w;
            }
        }
        v.resize(kept);
    }
    v.push_back(a);
}

// The entries of member `i`'s own list, for a report.
std::string member_list_text(const Act& act, uint32_t i) {
    std::string out;
    int         n = 0;
    for (const MEntry& e : act.mentries) {
        if (e.member != i) {
            continue;
        }
        if (n++ == 8) {
            return out + ", ...";
        }
        out += n == 1 ? "" : ", ";
        out += std::string(edge_name(e.edge)) + " " + signal_text(reinterpret_cast<const void*>(e.x));
    }
    return n == 0 ? "nothing" : out;
}

// A write of `x` satisfies the entries of the guarded group's members that it satisfies: each such member is
// owed a run in this activation, unless it has run in it already (S9). A fused group runs its producers before
// its consumers, so a write that comes after a member ran finds it past, and owes it nothing in this activation.
void owe_members(Act& act, uintptr_t x, bool pos, bool neg, bool any) {
    const auto first = std::lower_bound(act.mentries.begin(), act.mentries.end(), x,
                                        [](const MEntry& e, uintptr_t v) { return e.x < v; });
    for (auto it = first; it != act.mentries.end() && it->x == x; ++it) {
        const bool hit = (it->edge == RT_TRACE_ANY && any) || (it->edge == RT_TRACE_POS && pos) ||
                         (it->edge == RT_TRACE_NEG && neg);
        if (!hit || it->member >= act.members) {
            continue;
        }
        MState& s = act.mstate[it->member];
        if (act.state == State::Running && (s.bits & kMemberRan) != 0) {
            continue;
        }
        if ((s.bits & kMemberOwed) == 0) {
            s.x    = x;
            s.edge = it->edge;
        }
        s.bits |= kMemberOwed;
    }
}

// The activation of a guarded group parks at its static control again: every member that a write owed a run
// has run (R6.4), and the next activation starts from nothing. Returns false after a violation.
bool settle_members(uintptr_t a, Act& act) {
    bool ok = true;
    for (uint32_t j = 0; j < act.members && ok && !g.finished; ++j) {
        const MState& s = act.mstate[j];
        if ((s.bits & (kMemberOwed | kMemberRan)) == kMemberOwed) {
            std::string what;
            appendf(what, "member %u of the %u members of a fused group did not run before the group went back to sleep", j,
                    act.members);
            fail("R6.4", what, a,
                 std::string(edge_name(s.edge)) + " " + signal_text(reinterpret_cast<const void*>(s.x)) +
                   " changed while the group ran or waited, and the member lists it (" + member_list_text(act, j) + ")");
            ok = false;
        }
    }
    for (MState& s : act.mstate) {
        s = MState{};
    }
    act.all_ok = false;
    return ok;
}

// A write (or, for `obj`, a trigger) of `x`: each activation parked at a control that lists an
// entry the change satisfies is owed a wake before the next dispatch, park or region.
void owe(const void* x_, bool pos, bool neg, bool any, bool obj) {
    const uintptr_t x = reinterpret_cast<uintptr_t>(x_);
    const auto*     v = x != 0 ? g.watch.find(x) : nullptr;
    if (v == nullptr) {
        return;
    }
    for (const uintptr_t a : *v) {
        Act* act = g.acts.find(a);
        if (act != nullptr && act->members != 0 && !obj) {
            owe_members(*act, x, pos, neg, any);
        }
        if (act == nullptr || act->state != State::Parked || act->park == nullptr) {
            continue;
        }
        const uint32_t kind = act->park->kind;
        if (kind != RT_TRACE_CTL_STATIC && kind != RT_TRACE_CTL_EVENT) {
            continue;
        }
        for (const Entry& e : kind == RT_TRACE_CTL_STATIC ? act->stat : act->list) {
            if (reinterpret_cast<uintptr_t>(e.x) != x) {
                continue;
            }
            const bool hit = obj ? e.edge == RT_TRACE_OBJ
                                 : ((e.edge == RT_TRACE_ANY && any) || (e.edge == RT_TRACE_POS && pos) ||
                                    (e.edge == RT_TRACE_NEG && neg));
            if (hit) {
                g.owed.push_back(Owed{ a, act->park_seq, x, e.edge });
                break;
            }
        }
    }
}

// The point a `disable`'s walk must have reached every occupant of its list by: the next dispatch,
// park, region or `disable`. An occupant the walk missed would run on (R23.2).
void settle_aborts() {
    if (g.abort_owed.empty()) {
        return;
    }
    const AbortOwed o = g.abort_owed.front();
    g.abort_owed.clear();
    fail("R23.2", "a disable did not reach an activation that is inside the block or task it names: it would run on", o.act,
         "the list at " + address_text(o.target) + " held it, and the walk of that list marked no such activation");
}

// An occurrence of the control of the armed `<= @e` records that wait for `x`: a write that satisfies the
// edge of an entry, or a trigger of the event object (R11.2). A record counts an occurrence once, however
// many of its entries it satisfies, and the one that completes its count is owed its release at once.
void count_occurrence(uintptr_t x, bool pos, bool neg, bool any, bool obj) {
    const auto* v = g.finished ? nullptr : g.ewatch.find(x);
    if (v == nullptr) {
        return;
    }
    for (const EWatch& w : *v) {
        Upd* u = g.armed.find(w.rec);
        if (u == nullptr || u->site == nullptr || u->arm != w.arm || u->count == 0 || u->occ >= u->count) {
            continue;
        }
        bool hit = false;
        for (const EWait& e : u->waits) {
            hit = hit || (e.x == x && (obj ? e.edge == RT_TRACE_OBJ
                                           : ((e.edge == RT_TRACE_ANY && any) || (e.edge == RT_TRACE_POS && pos) ||
                                              (e.edge == RT_TRACE_NEG && neg))));
        }
        if (hit && ++u->occ == u->count) {
            g.rel_owed.push_back(w);
        }
    }
}

// R11.1: at the end of a slot, a `<= #d` record whose delay ended in it or before has been released by its
// timer. Returns whether it failed.
bool poll_timers() {
    if (g.finished) {
        return false;
    }
    bool failed = false;
    g.armed.for_each([&](uintptr_t, Upd& u) {
        if (failed || u.site == nullptr || u.path != RT_TRACE_NBA_TIMER || u.deadline == UINT64_MAX || u.deadline > g.now) {
            return;
        }
        std::string text = "armed: " + upd_text(u);
        appendf(text, "; its delay ends at tick %llu, and the slot of tick %llu is over", static_cast<unsigned long long>(u.deadline),
                static_cast<unsigned long long>(g.now));
        fail("R11.1", "the delay of a `<= #d` update elapsed and no update was created: its timer never released it", 0, text);
        failed = true;
    });
    return failed;
}

// The point a `<= @e` record whose count an occurrence completed must have been created by: the next
// dispatch, park or region (R11.2).
void settle_releases() {
    for (const EWatch& o : g.rel_owed) {
        const Upd* u = g.armed.find(o.rec);
        if (u != nullptr && u->site != nullptr && u->arm == o.arm) {
            std::string what;
            appendf(what, "the last of the %u occurrences a `<= @e` update counts came, and the update was not created", u->count);
            g.rel_owed.clear();
            fail("R11.2", what, 0, "armed: " + upd_text(*u));
            return;
        }
    }
    g.rel_owed.clear();
}

// The point a write's wakes must have been made by: every activation still parked as it was is a
// violation.
void settle_owed() {
    settle_aborts();
    if (g.halted) {
        return;
    }
    settle_releases();
    if (g.halted || g.owed.empty()) {
        return;
    }
    for (const Owed& o : g.owed) {
        const Act* act = g.acts.find(o.act);
        if (act != nullptr && act->state == State::Parked && act->park_seq == o.seq) {
            if (o.join) {
                const Fork* f     = g.forks.find(o.x);
                std::string cause = "the completion that a ";
                cause += f != nullptr ? join_name(f->kind) : "join";
                cause += " is due to wake its parent at came, and the parent was still parked at the join when the next dispatch, park or region came";
                fail("R22.3", "the parent of a fork was not woken by the completion its join is due at", o.act, cause);
                break;
            }
            std::string cause = std::string(edge_name(o.edge)) + " " + signal_text(reinterpret_cast<const void*>(o.x)) +
                                " changed, and the activation was still parked when the next dispatch, park or region came";
            fail("R6.1", "an activation parked on a change that happened was not woken", o.act, cause);
            break;
        }
    }
    g.owed.clear();
}

// R6b: at the end of a slot, an activation still parked whose entry the change from its bytes at the
// park to its bytes now satisfies was never woken, by a store no bracket reported or one it skipped.
// R2.3: an activation still parked at a `#d` whose deadline is now or past was never woken by its
// timer.
void poll_waiters() {
    std::vector<uint8_t> now;
    bool                 bad = false;
    g.acts.for_each([&](uintptr_t a, Act& act) {
        if (bad || act.state != State::Parked || act.park == nullptr) {
            return;
        }
        const uint32_t kind = act.park->kind;
        if (kind == RT_TRACE_CTL_DELAY && !act.zero && g.now != UINT64_MAX && act.deadline <= g.now) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "the delay ends at tick %llu and the slot of tick %llu is over: no timer woke it",
                          static_cast<unsigned long long>(act.deadline), static_cast<unsigned long long>(g.now));
            fail("R2.3", "a delay control was not resumed at its deadline", a, buf);
            bad = true;
            return;
        }
        if (kind != RT_TRACE_CTL_STATIC && kind != RT_TRACE_CTL_EVENT) {
            return;
        }
        for (const Entry& e : kind == RT_TRACE_CTL_STATIC ? act.stat : act.list) {
            if (e.shape == nullptr || e.snap.empty()) {
                continue;
            }
            read_value(e.x, e.shape, now);
            if (satisfied(e, now)) {
                const std::string cause = std::string(edge_name(e.edge)) + " " + signal_text(e.x) + ": its bytes were " +
                                          bytes_text(e.snap) + " when the activation parked and are " + bytes_text(now) + " now";
                fail("R6.3", "the value an activation waits on changed while it was parked, and no wake came", a, cause);
                bad = true;
                return;
            }
        }
    });
}

// -- Regions, rings, time zero and blocks (R1, R2, R4, R15, R17, R23, R25) --

// The first activation `pred` holds of, or zero.
template <typename P>
uintptr_t first_act(P&& pred) {
    uintptr_t found = 0;
    g.acts.for_each([&](uintptr_t a, Act& act) {
        if (found == 0 && pred(act)) {
            found = a;
        }
    });
    return found;
}

// Whether control moved on from a region that still has work (R1.2): `active` asks for the Active
// set's ring, `reactive` for the Reactive set's two, and `batches` for the `#0` activations waiting
// in each set's Inactive batch. A promotion of a batch is what starts when one waits, so it is not
// asked for there.
bool left_with_work(const char* when, bool active, bool reactive, bool batches) {
    const bool in_active   = active && g.n_queued[RT_TRACE_QUEUE_ACTIVE] != 0;
    const bool zero_active = active && batches && g.n_zero[0] != 0;
    const bool in_reactive = reactive && (g.n_queued[RT_TRACE_QUEUE_REACTIVE] != 0 || g.n_queued[RT_TRACE_QUEUE_CONT] != 0);
    const bool zero_react  = reactive && batches && g.n_zero[1] != 0;
    if (!in_active && !zero_active && !in_reactive && !zero_react) {
        return false;
    }
    const uintptr_t who = first_act([&](const Act& act) {
        if (act.state == State::Queued) {
            return act.ring == RT_TRACE_QUEUE_ACTIVE ? in_active : in_reactive;
        }
        return zero_parked(act) && (act.home_reactive ? zero_react : zero_active);
    });
    std::string cause;
    if (const Act* act = who != 0 ? g.acts.find(who) : nullptr; act != nullptr) {
        cause = act->state == State::Queued ? std::string("queued on the ") + queue_name(act->ring) + " ring and never dispatched"
                                            : std::string("parked at a #0 that no promotion released");
    }
    fail("R1.2", std::string("control moved on while an earlier region still had work: ") + when, who, cause);
    return true;
}

// Whether the scheduler may enter region `to` from `from`, within one slot (R1.1). The regions
// come in the order of the slot, and one with no code is skipped, so going forward is always
// possible. Going back is how work in a later region reaches an earlier one: the Active region
// takes it from every region but Postponed, and the Reactive region from the two regions of its own
// set that can hand it work. Nothing follows Postponed in the slot, and the top of the slot is first.
bool legal_region_step(uint32_t from, uint32_t to) {
    const auto rank = [](uint32_t r) {
        switch (r) {
            case RT_TRACE_REG_PREPONED: return 0;
            case RT_TRACE_REG_ACTIVE: return 1;
            case RT_TRACE_REG_INACTIVE: return 2;
            case RT_TRACE_REG_NBA: return 3;
            case RT_TRACE_REG_OBSERVED: return 4;
            case RT_TRACE_REG_REACTIVE: return 5;
            case RT_TRACE_REG_RE_INACTIVE: return 6;
            case RT_TRACE_REG_RE_NBA: return 7;
            case RT_TRACE_REG_POSTPONED: return 8;
            default: return 9;
        }
    };
    if (to == RT_TRACE_REG_PREPONED) {
        return from == RT_TRACE_REG_PREPONED;
    }
    if (rank(to) >= rank(from)) {
        return true;
    }
    if (to == RT_TRACE_REG_ACTIVE) {
        return from != RT_TRACE_REG_POSTPONED;
    }
    return to == RT_TRACE_REG_REACTIVE && (from == RT_TRACE_REG_RE_INACTIVE || from == RT_TRACE_REG_RE_NBA);
}

// Whether a body the scheduler runs once runs where its region is (R4.1): a `$strobe` or `$monitor`
// body in Postponed, the others between the markers of the region clotho.run_once was told they
// belong to (a run that reports no markers is not asked).
bool run_once_region_ok(const Act& act) {
    switch (kind_of(act.origin)) {
        case RT_TRACE_ORG_STROBE:
        case RT_TRACE_ORG_MONITOR: return g.region == RT_TRACE_REG_POSTPONED;
        default: break;
    }
    switch (g.once) {
        case RT_TRACE_ORG_PREINIT: return g.region == RT_TRACE_REG_PREPONED && g.slot_no == 0;
        case RT_TRACE_ORG_PREPONED: return g.region == RT_TRACE_REG_PREPONED;
        case RT_TRACE_ORG_OBSERVED: return g.region == RT_TRACE_REG_OBSERVED;
        case RT_TRACE_ORG_RE_NBA: return g.region == RT_TRACE_REG_REACTIVE;
        case RT_TRACE_ORG_FINAL: return g.in_final;
        default: return true;
    }
}

bool has_block(const Act& act, uintptr_t t) {
    return std::find(act.blocks.begin(), act.blocks.end(), t) != act.blocks.end();
}

// An activation links its node on the list at `t`. A block cannot be inside itself, so an activation
// is on a list once (R23.1). Returns false after a violation.
bool block_enter(uintptr_t a, Act& act, uintptr_t t) {
    if (has_block(act, t)) {
        fail("R23.1", "an activation links its node on a list it is already on: a block is never inside itself", a,
             "the list at " + address_text(t));
        return false;
    }
    act.blocks.push_back(t);
    std::vector<uintptr_t>& v = g.occ.at(t);
    if (std::find(v.begin(), v.end(), a) == v.end()) {
        v.push_back(a);
    }
    return true;
}


} // namespace

extern "C" void rt_trace_begin(void) {
    g.acts.clear();
    g.names.clear();
    g.armed.clear();
    g.ewatch.clear();
    g.rel_owed.clear();
    g.now     = 0;
    g.halted  = false;
    g.seq     = 0;
    g.rule.clear();
    g.report.clear();
    g.run     = {};
    g.counted = false;
    g.region  = RT_TRACE_REG_PREPONED;
    g.slot_no = 0;
    g.finished  = false;
    g.in_final  = false;
    g.finish_at = 0;
    g.in_pass     = false;
    g.pass_region = 0;
    for (Upd& u : g.pending) {
        retire(u);
    }
    g.pending.clear();
    g.split = 0;
    g.keys.clear();
    g.sorted_keys = 0;
    g.arm_seq     = 0;
    g.pends.clear();
    g.watch.clear();
    g.chg.clear();
    g.owed.clear();
    g.step = 0;
    for (int32_t& n : g.n_queued) {
        n = 0;
    }
    for (int32_t& n : g.n_zero) {
        n = 0;
    }
    g.n_cont_queued = 0;
    g.zero_owed     = 0;
    g.proc_owed     = 0;
    g.strobes_owed  = 0;
    g.once          = 0;
    g.occ.clear();
    g.ramp.clear();
    g.abort_owed.clear();
    g.forks.clear();
    g.inert.clear();
    g.inert_sites.clear();
    g.inert_live          = 0;
    g.inert_waiting       = 0;
    g.inert_wheel_waiting = 0;
}

extern "C" void rt_trace_spawn(void* a_, const rt_trace_control* origin, void* parent) {
    if (g.halted) {
        return;
    }
    touch_run();
    const uintptr_t a = norm(a_);
    ++g.run.spawns;
    ++g.total.spawns;
    if (a == 0) {
        fail("R5.3", "spawn of a null activation", 0, "");
        return;
    }
    Act& act = g.acts.at(a);
    // The incarnation that used this address before is gone, and what it still owed or held goes
    // with it.
    count(act, -1);
    g.zero_owed    -= act.zero_owed ? 1 : 0;
    g.proc_owed    -= act.proc_owed ? 1 : 0;
    g.strobes_owed -= act.strobe_owed ? 1 : 0;
    act.state  = State::Spawned;
    act.origin = origin;
    act.park   = nullptr;
    act.parent = norm(parent);
    act.deadline = 0;
    act.zero   = false;
    act.stat.clear();
    act.list.clear();
    act.pending.clear();
    act.ring          = 0;
    act.home_reactive = false;
    act.rec           = 0;
    act.dispatched    = false;
    act.zero_owed     = false;
    act.proc_owed     = false;
    act.strobe_owed   = false;
    act.blocks.clear();
    act.inherits.clear();
    act.marks.clear();
    act.forked    = 0;
    act.member_of = 0;
    act.completed = false;
    act.members = 0;
    act.exact   = false;
    act.all_ok  = false;
    act.mentries.clear();
    act.mstate.clear();
    // A thread's home is a program's or a design's by where it starts (R25); what it forks or
    // calls takes the home of the thread that does.
    switch (kind_of(origin)) {
        case RT_TRACE_ORG_PROCESS:
            act.home = (origin->kind & (RT_TRACE_ORG_PROGRAM | RT_TRACE_ORG_REACTIVE_CONT)) != 0 ? Home::Reactive : Home::Active;
            break;
        case RT_TRACE_ORG_PEND: act.home = Home::Reactive; break;
        case RT_TRACE_ORG_FORK:
        case RT_TRACE_ORG_TASK: {
            const Act* creator = act.parent != 0 ? g.acts.find(act.parent) : nullptr;
            act.home           = creator != nullptr ? creator->home : Home::Unknown;
            break;
        }
        default: act.home = Home::Unknown; break;
    }
    if (has_fact(act, RT_TRACE_ORG_SELF_START)) {
        act.zero_owed = true;
        ++g.zero_owed;
    }
    if (kind_of(origin) == RT_TRACE_ORG_STROBE) {
        act.strobe_owed = true;
        ++g.strobes_owed;
    }
    record(Ev::Spawn, a, 0, origin, 0, 0);
    // A fork child belongs to the latest fork of its creator (R22).
    if (kind_of(origin) == RT_TRACE_ORG_FORK && act.parent != 0) {
        const Act* creator = g.acts.find(act.parent);
        Fork*      f       = creator != nullptr && creator->forked != 0 ? g.forks.find(creator->forked) : nullptr;
        if (f != nullptr) {
            ++f->spawned;
            act.member_of = creator->forked;
        }
    }
    // The lists its ramp linked it on before this event announced it.
    if (std::vector<uintptr_t>* linked = g.ramp.find(a); linked != nullptr) {
        for (const uintptr_t t : *linked) {
            if (!block_enter(a, act, t)) {
                break;
            }
        }
        linked->clear();
    }
}

extern "C" void rt_trace_subscribe(void* a_, const rt_trace_entry* list, uint32_t n) {
    if (g.halted) {
        return;
    }
    if (Act* act = find("R5.3", norm(a_), "subscription"); act != nullptr) {
        add_entries(act->stat, list, n);
        for (uint32_t i = 0; i < n; ++i) {
            watch_add(list[i].x, norm(a_));
        }
    }
}

extern "C" void rt_trace_subscribe_event(void* a_, const void* obj, const char* name) {
    if (g.halted) {
        return;
    }
    if (Act* act = find("R5.3", norm(a_), "subscription"); act != nullptr) {
        const rt_trace_entry e{ obj, name, RT_TRACE_OBJ, nullptr };
        add_entries(act->stat, &e, 1);
        watch_add(obj, norm(a_));
    }
}

extern "C" void rt_trace_waits_for(void* a_, const void* x, const char* name, uint64_t edge, const rt_trace_shape* shape) {
    if (g.halted) {
        return;
    }
    if (Act* act = find("R5.3", norm(a_), "park list"); act != nullptr) {
        const rt_trace_entry e{ x, name, edge, shape };
        add_entries(act->pending, &e, 1);
    }
}

extern "C" void rt_trace_park(void* a_, const rt_trace_control* c, uint64_t arg) {
    if (g.halted) {
        return;
    }
    settle_owed();
    if (g.halted) {
        return;
    }
    const uintptr_t a = norm(a_);
    ++g.run.parks;
    ++g.total.parks;
    Act* act = find("R5.3", a, "park");
    if (act == nullptr) {
        return;
    }
    if (act->state == State::Ended) {
        fail("R5.4", "park of an activation that already ended", a, "");
        return;
    }
    if (act->state != State::Running && act->state != State::Spawned) {
        fail("R5.3", std::string("park of an activation that is ") + state_name(act->state) +
                       ", not running: a resume the trace does not report",
             a, "");
        return;
    }
    const uint32_t kind = c != nullptr ? c->kind : ~0u;
    // A disable or a kill marked the activation to leave, and every park but the two that run their
    // child or callee inline first comes after the poll that takes it out (R23.4).
    if (!act->marks.empty() && !g.finished && kind != RT_TRACE_CTL_JOIN && kind != RT_TRACE_CTL_CALL) {
        const Mark& m = act->marks.front();
        fail("R23.4", "an activation parked again after a disable or a kill marked it to leave: it ran on past the poll that was to take it out",
             a,
             m.kind == RT_TRACE_ABORT_KILL ? std::string("marked by a kill of its thread")
                                           : "marked by a disable of the list at " + address_text(m.target));
        return;
    }
    // A guarded group goes back to sleep: the members its writes owed a run have run (R6.4).
    if (act->members != 0 && kind == RT_TRACE_CTL_STATIC && !settle_members(a, *act)) {
        return;
    }
    count(*act, -1);
    act->state = State::Parked;
    act->park  = c;
    act->list.clear();
    if (c != nullptr && c->kind == RT_TRACE_CTL_EVENT) {
        act->list.swap(act->pending);
    }
    act->pending.clear();
    ++act->park_seq;
    ++g.step;
    if (c != nullptr && c->kind == RT_TRACE_CTL_EVENT) {
        for (Entry& e : act->list) {
            snapshot_entry(e);
            watch_add(e.x, a);
        }
    } else if (c != nullptr && c->kind == RT_TRACE_CTL_STATIC) {
        for (Entry& e : act->stat) {
            snapshot_entry(e);
        }
    }
    // The thread's home tag, which says which batch a `#0` waits in; a delay's ticks, or the record
    // or queue a park names.
    act->home_reactive = (reinterpret_cast<uintptr_t>(a_) & 3) == 2;
    act->zero          = kind == RT_TRACE_CTL_DELAY && arg == 0;
    act->deadline      = kind != RT_TRACE_CTL_DELAY ? 0 : (arg > UINT64_MAX - g.now ? UINT64_MAX : g.now + arg);
    act->rec = kind == RT_TRACE_CTL_JOIN || kind == RT_TRACE_CTL_CALL || kind == RT_TRACE_CTL_WAIT_FORK ||
                       kind == RT_TRACE_CTL_AWAIT || kind == RT_TRACE_CTL_SYNC || kind == RT_TRACE_CTL_EXPECT
                 ? static_cast<uintptr_t>(arg)
                 : 0;
    count(*act, +1);
    record(Ev::Park, a, arg, c, 0, 0);
}

extern "C" void rt_trace_wake(void* a_, uint32_t cause_word, const void* x, uint32_t fact) {
    if (g.halted) {
        return;
    }
    const uint32_t  cause = cause_word & 0xff;
    const uint8_t   queue = static_cast<uint8_t>((cause_word >> RT_TRACE_QUEUE_SHIFT) & 3);
    const uintptr_t a     = norm(a_);
    ++g.run.wakes;
    ++g.total.wakes;
    Act* act = find("R5.1", a, "wake");
    if (act == nullptr) {
        return;
    }
    record(Ev::Wake, a, reinterpret_cast<uintptr_t>(x), act->park, static_cast<uint8_t>(cause), static_cast<uint8_t>(fact), nullptr,
           queue);
    switch (act->state) {
        case State::Ended:
            fail("R5.4", "wake of an activation that already ended: its frame is gone", a, cause_text(cause, x, fact));
            return;
        case State::Queued:
            fail("R5.1", "wake of an activation that is already queued: it would run twice", a, cause_text(cause, x, fact));
            return;
        case State::Running:
            fail("R5.1", "wake of an activation that is running: it would be resumed again at its next park", a,
                 cause_text(cause, x, fact));
            return;
        case State::Spawned:
            if (cause != RT_TRACE_SEED && cause != RT_TRACE_SPAWN && cause != RT_TRACE_PEND) {
                fail("R5.1", "wake of an activation that has not parked", a, cause_text(cause, x, fact));
                return;
            }
            break;
        case State::Parked: {
            if (cause == RT_TRACE_SEED || cause == RT_TRACE_SPAWN || cause == RT_TRACE_PEND) {
                fail("R5.1", "a start of an activation that has run", a, cause_text(cause, x, fact));
                return;
            }
            const std::string why = refusal(*act, cause, x, fact);
            if (!why.empty()) {
                fail("R5.1", std::string("wake of an activation that is not waiting for it: ") + why, a,
                     cause_text(cause, x, fact));
                return;
            }
            break;
        }
    }
    // A wake by a store follows a write of that storage that the facts of the checker's own
    // reading say satisfies the wake (R6.2).
    if (cause == RT_TRACE_STATIC || cause == RT_TRACE_DYNAMIC || cause == RT_TRACE_VIF) {
        const ChgFacts* c  = g.chg.find(reinterpret_cast<uintptr_t>(x));
        const bool      ok = c != nullptr && c->step == g.step &&
                        (fact == RT_TRACE_POS ? c->pos : (fact == RT_TRACE_NEG ? c->neg : c->any));
        if (!ok) {
            fail("R6.2", "a wake by a store of a value that no bracket reported changing that way", a, cause_text(cause, x, fact));
            return;
        }
    }
    // The wake of a parent parked at a join comes at the completion its join is due at (R22.3).
    if (cause == RT_TRACE_CHILD && !g.finished) {
        if (const Fork* f = g.forks.find(reinterpret_cast<uintptr_t>(x)); f != nullptr && !join_due(*f)) {
            std::string text;
            appendf(text, "%u of the %u children had completed; a join is woken by the last and a join_any by the first", f->done,
                    f->width);
            fail("R22.3", std::string("the parent of a ") + join_name(f->kind) + " was woken by a completion its join is not due at", a,
                 text);
            return;
        }
    }
    // Past a `$finish` only the completions that make each frame leave wake anything (R24).
    if (g.finished && !completion_cause(cause)) {
        fail("R24.1", "wake after $finish: code of the finishing thread, or of something it woke, ran on", a,
             cause_text(cause, x, fact));
        return;
    }
    // A `disable` or a `kill` wakes the activations its walk marked, and no other (R23.5). A run
    // whose walks report no abort at all says nothing of what its force wakes were for.
    if (cause == RT_TRACE_FORCE && act->marks.empty() && g.run.aborts != 0 && !g.finished) {
        fail("R23.5", "an activation was force-woken that no disable or kill had marked to leave", a, cause_text(cause, x, fact));
        return;
    }
    // An `expect` verdict puts the thread in the Reactive region for the rest of its life (R25).
    if (cause == RT_TRACE_EXPECT) {
        act->home = Home::Reactive;
    }
    // The ring a wake pushes onto is the one its thread's home names (R25.1). The seed's own choice
    // for a program's constant-source assign is the Active ring, which settles it before Reactive
    // starts, so the seed is not asked about the continuous ones.
    if (act->home != Home::Unknown && !g.finished && !(cause == RT_TRACE_SEED && has_fact(*act, RT_TRACE_ORG_REACTIVE_CONT)) &&
        (queue != RT_TRACE_QUEUE_ACTIVE) != (act->home == Home::Reactive)) {
        fail("R25.1",
             act->home == Home::Reactive ? "a thread of a program was queued for the Active region"
                                         : "a thread of the design was queued for the Reactive region",
             a, cause_text(cause, x, fact) + std::string(", onto the ") + queue_name(queue) + " ring");
        return;
    }
    count(*act, -1);
    act->state        = State::Queued;
    act->ring         = queue;
    act->queued_at    = g.seq;
    act->queued_cause = static_cast<uint8_t>(cause);
    // A wake that names no trigger sets every bit of a guarded group, which then runs every member (S9).
    if (act->members != 0 && cause != RT_TRACE_STATIC) {
        act->all_ok = true;
    }
    count(*act, +1);
    // A procedure the seed queues has to run before a late group's first run (R17).
    if (cause == RT_TRACE_SEED && has_fact(*act, RT_TRACE_ORG_PROCEDURE) && !act->proc_owed) {
        act->proc_owed = true;
        ++g.proc_owed;
    }
}

extern "C" int32_t rt_trace_dispatch(void* a_, uint32_t how) {
    if (g.halted) {
        return 0;
    }
    settle_owed();
    if (g.halted) {
        return 0;
    }
    ++g.step;
    const uintptr_t a = norm(a_);
    ++g.run.dispatches;
    ++g.total.dispatches;
    Act* act = find("R5.2", a, "dispatch");
    if (act == nullptr) {
        return 0;
    }
    record(Ev::Dispatch, a, 0, act->park, static_cast<uint8_t>(how), 0);
    if (act->state == State::Ended) {
        fail("R5.4", "dispatch of an activation that already ended: its frame is gone", a, std::string("dispatched ") + how_name(how));
        return 0;
    }
    const State want = how == RT_TRACE_RING ? State::Queued : State::Spawned;
    if (act->state != want) {
        fail("R5.2",
             std::string("dispatch ") + how_name(how) + " of an activation that is " + state_name(act->state) +
               (how == RT_TRACE_RING ? ", not queued" : ", not freshly created"),
             a, std::string("dispatched ") + how_name(how));
        return 0;
    }
    if (g.finished) {
        // Past a `$finish` the one dispatch is the exit's: the activation that the completion of a
        // thread woke after it, which leaves through its own epilogue (R24).
        const bool exit_wake = how == RT_TRACE_RING && act->queued_at >= g.finish_at && completion_cause(act->queued_cause);
        if (!exit_wake) {
            fail("R24.2",
                 std::string("dispatch ") + how_name(how) + " after $finish: an activation that was queued before it, or one that started after it, runs",
                 a, std::string("dispatched ") + how_name(how));
            return 0;
        }
    } else if (how == RT_TRACE_RING) {
        // A ring is drained in the Active and Reactive regions, each its own (R1.1, R25.1); the top of
        // the slot, before the first of them, is not asked.
        const bool in_reactive = g.region == RT_TRACE_REG_REACTIVE;
        const bool in_active   = g.region == RT_TRACE_REG_ACTIVE;
        if (!in_reactive && !in_active && g.region != RT_TRACE_REG_PREPONED) {
            fail("R1.1", "an activation was popped from a ring outside the Active and Reactive regions", a,
                 std::string("the scheduler is in the ") + region_name(g.region) + " region");
            return 0;
        }
        if ((in_reactive || in_active) && in_reactive != (act->ring != RT_TRACE_QUEUE_ACTIVE)) {
            fail("R25.1", in_reactive ? "an activation queued for the Active region ran in the Reactive region"
                                      : "an activation queued for the Reactive region ran in the Active region",
                 a, std::string("queued on the ") + queue_name(act->ring) + " ring");
            return 0;
        }
        // A program's continuous processes settle its port copies before program code reads them: while
        // one is queued, no program code is popped (R25.2).
        if (in_reactive && act->ring == RT_TRACE_QUEUE_REACTIVE && g.n_cont_queued > 0 &&
            !has_fact(*act, RT_TRACE_ORG_REACTIVE_CONT)) {
            const uintptr_t cont = first_act([](const Act& o) { return o.state == State::Queued && has_fact(o, RT_TRACE_ORG_REACTIVE_CONT); });
            fail("R25.2", "program code ran while a program's continuous process was still queued: it would read its inputs stale", a,
                 "still queued: " + act_text(cont));
            return 0;
        }
    } else if (how == RT_TRACE_RUN_ONCE && !run_once_region_ok(*act)) {
        fail("R4.1", "a body the scheduler runs once ran outside the region it belongs to", a,
             std::string("the scheduler is in the ") + region_name(g.region) + " region");
        return 0;
    } else if (how == RT_TRACE_INLINE) {
        const std::string why = child_start_refusal(*act);
        if (!why.empty()) {
            fail("R22.1", why, a, "dispatched inline");
            return 0;
        }
    }
    if (!act->dispatched) {
        act->dispatched = true;
        // An activity enabled within a targeted block is on its list before it first runs (R23.3).
        if (!g.finished) {
            for (const uintptr_t t : act->inherits) {
                if (!has_block(*act, t)) {
                    fail("R23.3", "a fork child runs before it is an occupant of a block or task it was forked inside: a disable of it would miss the child", a,
                         "the list at " + address_text(t) + " does not hold it");
                    return 0;
                }
            }
            // A group with an always_comb member starts after every procedure the seed queued (R17).
            if (has_fact(*act, RT_TRACE_ORG_LATE) && g.proc_owed > 0) {
                const uintptr_t proc = first_act([](const Act& o) { return o.proc_owed; });
                fail("R17.1", "an always_comb or always_latch started before every procedure had", a,
                     "not yet started: " + act_text(proc));
                return 0;
            }
        }
        if (act->zero_owed) {
            act->zero_owed = false;
            --g.zero_owed;
        }
        if (act->proc_owed) {
            act->proc_owed = false;
            --g.proc_owed;
        }
    }
    if (act->strobe_owed) {
        act->strobe_owed = false;
        --g.strobes_owed;
    }
    count(*act, -1);
    act->state = State::Running;
    return 1;
}

extern "C" void rt_trace_resume(void* a_, const rt_trace_control* c) {
    if (g.halted) {
        return;
    }
    const uintptr_t a   = norm(a_);
    Act*            act = find("R5.3", a, "resume");
    if (act == nullptr) {
        return;
    }
    record(Ev::Resume, a, 0, c, 0, 0);
    if (act->state == State::Ended) {
        fail("R5.4", "resume of an activation that already ended: its frame is gone", a, "resumed at " + control_text(c));
        return;
    }
    if (act->state != State::Running) {
        fail("R5.3", std::string("resume of an activation that is ") + state_name(act->state) + ", not running", a,
             "resumed at " + control_text(c));
        return;
    }
    const rt_trace_control* p = act->park;
    const bool same = p == c || (p != nullptr && c != nullptr && p->kind == c->kind &&
                                 (p->where == c->where || (p->where != nullptr && c->where != nullptr && std::strcmp(p->where, c->where) == 0)));
    if (!same) {
        fail("R5.3", "resume at a control that is not the last park: a park the trace does not report", a,
             "resumed at " + control_text(c) + ", last parked at " + control_text(p));
        return;
    }
    // The thread past an `expect` is in the Reactive region for the rest of its life (R25).
    if (c != nullptr && c->kind == RT_TRACE_CTL_EXPECT) {
        act->home = Home::Reactive;
    }
}

extern "C" void rt_trace_end(void* a_) {
    if (g.halted) {
        return;
    }
    const uintptr_t a   = norm(a_);
    Act*            act = find("R5.3", a, "end");
    if (act == nullptr) {
        return;
    }
    record(Ev::End, a, 0, act->park, 0, 0);
    if (act->state == State::Ended) {
        fail("R5.4", "end of an activation that already ended", a, "");
        return;
    }
    if (act->state != State::Running) {
        fail("R5.3", std::string("end of an activation that is ") + state_name(act->state) + ", not running", a, "");
        return;
    }
    // Every exit of a targeted block, a `return`, a `break`, an abort, takes the node off the list
    // first: one left on it would be walked after its frame is gone (R23.1).
    if (!act->blocks.empty() && !g.finished) {
        fail("R23.1", "an activation ended inside a block or task some disable names: its node would stay linked, and the next walk of the list would follow a dangling pointer", a,
             "still on the list at " + address_text(act->blocks.front()));
        return;
    }
    act->marks.clear();
    act->state = State::Ended;
}

extern "C" void rt_trace_block(void* a_, const void* target, uint32_t how) {
    if (g.halted) {
        return;
    }
    touch_run();
    ++g.run.blocks;
    ++g.total.blocks;
    const uintptr_t a = norm(a_);
    const uintptr_t t = reinterpret_cast<uintptr_t>(target);
    if (how == RT_TRACE_BLOCK_RAMP) {
        // A fork child's ramp runs before the spawn that announces the child: the list is the
        // child's once it is.
        record(Ev::Block, a, t, nullptr, static_cast<uint8_t>(how), 0);
        g.ramp.at(a).push_back(t);
        return;
    }
    Act* act = find("R5.3", a, "block event");
    if (act == nullptr) {
        return;
    }
    record(Ev::Block, a, t, act->park, static_cast<uint8_t>(how), 0);
    switch (how) {
        case RT_TRACE_BLOCK_ENTER:
            if (!g.finished) {
                block_enter(a, *act, t);
            }
            break;
        case RT_TRACE_BLOCK_EXIT: {
            const auto it = std::find(act->blocks.begin(), act->blocks.end(), t);
            if (it == act->blocks.end()) {
                if (!g.finished) {
                    fail("R23.1", "an activation unlinks its node from a list it is not on", a, "the list at " + address_text(t));
                }
                return;
            }
            act->blocks.erase(it);
            // Leaving the block is the poll that obeys a disable of it.
            size_t kept = 0;
            for (size_t i = 0; i < act->marks.size(); ++i) {
                if (act->marks[i].target != t) {
                    act->marks[kept++] = act->marks[i];
                }
            }
            act->marks.resize(kept);
            break;
        }
        case RT_TRACE_BLOCK_INHERIT: act->inherits.push_back(t); break;
        default: break;
    }
}

extern "C" void rt_trace_disable(void* a_, const void* target) {
    if (g.halted) {
        return;
    }
    touch_run();
    // An earlier `disable`'s walk is over by now.
    settle_aborts();
    if (g.halted) {
        return;
    }
    const uintptr_t t = reinterpret_cast<uintptr_t>(target);
    record(Ev::Disable, norm(a_), t, nullptr, 0, 0);
    if (g.finished) {
        return;
    }
    // Every occupant of the list is on its way to be marked by the walk that follows: one it
    // misses runs on (R23.2).
    const std::vector<uintptr_t>* v = g.occ.find(t);
    if (v == nullptr) {
        return;
    }
    for (const uintptr_t w : *v) {
        const Act* act = g.acts.find(w);
        if (act == nullptr || act->state == State::Ended || !has_block(*act, t)) {
            continue;
        }
        bool known = false;
        for (const AbortOwed& o : g.abort_owed) {
            known = known || (o.act == w && o.target == t);
        }
        if (!known) {
            g.abort_owed.push_back(AbortOwed{ w, t });
        }
    }
}

extern "C" void rt_trace_abort(void* a_, const void* target, uint32_t kind) {
    if (g.halted) {
        return;
    }
    touch_run();
    ++g.run.aborts;
    ++g.total.aborts;
    const uintptr_t a   = norm(a_);
    const uintptr_t t   = reinterpret_cast<uintptr_t>(target);
    Act*            act = find("R5.3", a, "abort");
    if (act == nullptr) {
        return;
    }
    record(Ev::Abort, a, t, act->park, static_cast<uint8_t>(kind), 0);
    if (g.finished) {
        return;
    }
    // The walk reached it.
    for (size_t i = 0; i < g.abort_owed.size(); ++i) {
        if (g.abort_owed[i].act == a && g.abort_owed[i].target == t) {
            g.abort_owed.erase(g.abort_owed.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    for (const Mark& m : act->marks) {
        if (m.target == t && m.kind == kind) {
            return;
        }
    }
    act->marks.push_back(Mark{ t, static_cast<uint8_t>(kind) });
}

extern "C" void rt_trace_fork(void* a_, const void* rec_, uint32_t kind, uint32_t width) {
    if (g.halted) {
        return;
    }
    touch_run();
    ++g.run.forks;
    ++g.total.forks;
    const uintptr_t a   = norm(a_);
    const uintptr_t rec = reinterpret_cast<uintptr_t>(rec_);
    Act*            act = nullptr;
    if (a != 0) {
        act = find("R5.3", a, "fork");
        if (act == nullptr) {
            return;
        }
    }
    record(Ev::Fork, a, rec, act != nullptr ? act->park : nullptr, static_cast<uint8_t>(kind), 0);
    g.forks.at(rec) = Fork{ a, static_cast<uint8_t>(kind), width, 0, 0 };
    if (act != nullptr) {
        act->forked = rec;
    }
}

extern "C" void rt_trace_child_done(void* a_, const void* rec_) {
    if (g.halted) {
        return;
    }
    const uintptr_t a   = norm(a_);
    const uintptr_t rec = reinterpret_cast<uintptr_t>(rec_);
    Act*            act = find("R5.3", a, "child completion");
    if (act == nullptr) {
        return;
    }
    record(Ev::ChildDone, a, rec, act->park, 0, 0);
    Fork* f = g.forks.find(rec);
    if (f == nullptr) {
        if (!g.finished) {
            fail("R22.2", "a fork child completes into a record that no fork announced: a fork the trace does not report", a,
                 "the record at " + address_text(rec));
        }
        return;
    }
    if (!g.finished) {
        if (act->member_of != 0 && act->member_of != rec) {
            fail("R22.2", "a fork child completes into the record of another fork: its count would be wrong, and so would the wake", a,
                 "created for the record at " + address_text(act->member_of) + ", completes into the one at " + address_text(rec));
            return;
        }
        if (act->completed) {
            fail("R22.2", "a fork child completes twice: the join would count it twice", a, "the record at " + address_text(rec));
            return;
        }
    }
    act->completed = true;
    ++f->done;
    if (g.finished || f->parent == 0 || !join_due(*f)) {
        return;
    }
    // This is the completion the parent's join is due at: if the parent is parked at it, it is woken
    // before anything else runs.
    const Act* parent = g.acts.find(f->parent);
    if (parent != nullptr && parent->state == State::Parked && parent->park != nullptr && parent->park->kind == RT_TRACE_CTL_JOIN &&
        parent->rec == rec) {
        g.owed.push_back(Owed{ f->parent, parent->park_seq, rec, 0, true });
    }
}

extern "C" void rt_trace_join(void* a_, const void* rec_) {
    if (g.halted) {
        return;
    }
    touch_run();
    ++g.run.joins;
    ++g.total.joins;
    const uintptr_t a   = norm(a_);
    const uintptr_t rec = reinterpret_cast<uintptr_t>(rec_);
    Act*            act = find("R5.3", a, "join");
    if (act == nullptr) {
        return;
    }
    record(Ev::Join, a, rec, act->park, 0, 0);
    if (g.finished) {
        return;
    }
    const Fork* f = g.forks.find(rec);
    if (f == nullptr) {
        fail("R22.4", "a join of a record that no fork announced: a fork the trace does not report", a, "the record at " + address_text(rec));
        return;
    }
    if (f->parent != a) {
        fail("R22.4", "an activation passes the join of a fork that another made", a,
             "the record at " + address_text(rec) + " was forked by " + address_text(f->parent));
        return;
    }
    std::string text;
    if (f->spawned != f->width) {
        appendf(text, "the fork announced %u children and had created %u when its parent joined", f->width, f->spawned);
        fail("R22.4", "a join passed before every child of its fork was created: the count it waits on can never reach its due value", a, text);
        return;
    }
    const bool ok = f->kind == RT_TRACE_JOIN_ALL ? f->done == f->width : (f->kind == RT_TRACE_JOIN_ANY ? f->done >= 1 : true);
    if (!ok) {
        appendf(text, "%u of the %u children had completed", f->done, f->width);
        fail("R22.4", std::string("the parent of a ") + join_name(f->kind) + " went on before its children had completed", a, text);
    }
}

extern "C" void rt_trace_members(void* a_, const rt_trace_member_entry* list, uint32_t n, uint32_t members, uint32_t exact) {
    if (g.halted) {
        return;
    }
    Act* act = find("R5.3", norm(a_), "member list");
    if (act == nullptr) {
        return;
    }
    act->members = members;
    act->exact   = exact != 0;
    act->all_ok  = false;
    act->mentries.clear();
    for (uint32_t i = 0; i < n; ++i) {
        act->mentries.push_back(MEntry{ reinterpret_cast<uintptr_t>(list[i].x), list[i].edge, list[i].member });
    }
    std::sort(act->mentries.begin(), act->mentries.end(), [](const MEntry& l, const MEntry& r) { return l.x < r.x; });
    act->mstate.assign(members, MState{});
}

extern "C" void rt_trace_member(void* a_, uint32_t i) {
    if (g.halted) {
        return;
    }
    touch_run();
    const uintptr_t a   = norm(a_);
    Act*            act = find("R5.3", a, "member run");
    if (act == nullptr) {
        return;
    }
    record(Ev::Member, a, i, act->park, 0, 0);
    if (i >= act->members) {
        std::string what;
        appendf(what, "member %u", i);
        fail("R5.3", "a member of a guarded group runs, and the seed did not report its members: a group the trace does not report",
             a, what);
        return;
    }
    ++g.run.member_runs;
    ++g.total.member_runs;
    if (g.finished) {
        return;
    }
    MState& s = act->mstate[i];
    // The guard lets a member through on a bit of its own, or on every bit a wake that names no trigger sets;
    // where two events of the group share a bit, a sibling's event does the same (S9).
    if (!act->all_ok && act->exact && (s.bits & kMemberOwed) == 0) {
        std::string what;
        appendf(what, "member %u of the %u members of a fused group ran, and none of its own events fired in this activation: its guard let it through", i,
                act->members);
        fail("R5.5", what, a, "the member lists " + member_list_text(*act, i));
        return;
    }
    s.bits = static_cast<uint8_t>((s.bits & ~kMemberOwed) | kMemberRan);
}

extern "C" void rt_trace_inertial_decide(uint32_t idx, uint32_t same_pending, uint32_t same_cur, uint64_t epoch) {
    if (g.halted || g.finished) {
        return;
    }
    touch_run();
    if (idx >= g.inert_sites.size()) {
        g.inert_sites.resize(idx + 1);
    }
    InertSite&  s = g.inert_sites[idx];
    std::string text;
    appendf(text, "site %u: equal to the pending value %u, equal to the current value %u, epoch asked %llu", idx, same_pending,
            same_cur, static_cast<unsigned long long>(epoch));
    // The same value again keeps the timer of the pending update running: nothing is armed (LRM 10.3.3 b).
    if (s.live != 0 && same_pending != 0) {
        if (epoch != 0) {
            fail("R16.1", "a store that evaluated to the value its pending update already holds armed a new record: the timer restarted",
                 0, text);
        }
        return;
    }
    // Any other value supersedes the pending update, which is how a pulse shorter than the delay is rejected ...
    if (s.live != 0) {
        if (InertRec* r = g.inert.find(s.live); r != nullptr) {
            r->state = IState::Superseded;
            --g.inert_live;
        }
        s.live = 0;
    }
    // ... and schedules a new one, unless the value is the target's current one (c, d). A record the runtime still
    // takes for pending after it fired is tolerated: it keeps a value that the net already holds.
    if (same_cur != 0) {
        if (epoch != 0) {
            fail("R16.1", "a store that evaluated to the target's current value armed an update: the pending one is cancelled and nothing is scheduled",
                 0, text);
            return;
        }
    } else if (epoch == 0 && same_pending == 0) {
        fail("R16.1", "a store that evaluated to a new value armed no update: the net would never take it", 0, text);
        return;
    }
    s.expect = epoch;
}

extern "C" void rt_trace_inertial_arm(uint32_t idx, const void* rec_, uint64_t ticks, uint64_t epoch) {
    if (g.halted || g.finished) {
        return;
    }
    touch_run();
    if (idx >= g.inert_sites.size()) {
        g.inert_sites.resize(idx + 1);
    }
    InertSite& s = g.inert_sites[idx];
    if (epoch == 0 || epoch != s.expect) {
        std::string text;
        appendf(text, "site %u: armed for epoch %llu, the store's decision asked for %llu", idx, static_cast<unsigned long long>(epoch),
                static_cast<unsigned long long>(s.expect));
        fail("R16.2", "an inertial record was armed that the store's decision did not ask for", 0, text);
        return;
    }
    s.expect            = 0;
    ++g.run.inertial_arms;
    ++g.total.inertial_arms;
    const uintptr_t rec = reinterpret_cast<uintptr_t>(rec_);
    g.inert.at(rec)     = InertRec{ idx, IState::Live, g.now, ticks > UINT64_MAX - g.now ? UINT64_MAX : g.now + ticks, epoch };
    s.live              = rec;
    ++g.inert_live;
}

extern "C" void rt_trace_inertial_fire(const void* rec_, uint32_t current) {
    if (g.halted || g.finished) {
        return;
    }
    touch_run();
    const uintptr_t rec = reinterpret_cast<uintptr_t>(rec_);
    InertRec*       r   = g.inert.find(rec);
    if (r == nullptr || (r->state != IState::Live && r->state != IState::Superseded)) {
        fail("R16.3", "an inertial record fired that no store armed, or that had fired already", 0, "the record at " + address_text(rec));
        return;
    }
    std::string text;
    appendf(text, "site %u: armed at tick %llu for tick %llu", r->idx, static_cast<unsigned long long>(r->armed_at),
            static_cast<unsigned long long>(r->deadline));
    if (r->deadline != UINT64_MAX && g.now != r->deadline) {
        std::string what;
        appendf(what, "an inertial record fired at tick %llu, and was armed for tick %llu", static_cast<unsigned long long>(g.now),
                static_cast<unsigned long long>(r->deadline));
        fail("R16.3", what, 0, text);
        return;
    }
    const bool live = r->state == IState::Live;
    if ((current != 0) != live) {
        fail("R16.4",
             current != 0 ? "a record that a later store superseded was applied: a pulse shorter than the delay got through"
                          : "a record that no later store had superseded was dropped: its update is lost",
             0, text);
        return;
    }
    if (live) {
        --g.inert_live;
        if (InertSite& s = g.inert_sites[r->idx]; s.live == rec) {
            s.live = 0;
        }
    }
    if (current == 0) {
        r->state = IState::Done;
        return;
    }
    r->state = IState::Fired;
    ++g.inert_waiting;
    if (r->deadline > r->armed_at) {
        ++g.inert_wheel_waiting;
    }
}

extern "C" void rt_trace_inertial_applied(const void* rec_) {
    if (g.halted || g.finished) {
        return;
    }
    InertRec* r = g.inert.find(reinterpret_cast<uintptr_t>(rec_));
    if (r == nullptr || r->state != IState::Fired) {
        return;
    }
    // The wheel's records are applied by the commit at the top of their slot, before the Active region (S26); a
    // zero delay cannot arm the wheel, and its update lands in the NBA region of the slot.
    const bool wheel = r->deadline > r->armed_at;
    if (wheel ? g.region != RT_TRACE_REG_PREPONED : !g.in_pass) {
        fail("R16.5",
             wheel ? "an inertial update the wheel fired was applied outside the commit at the top of its slot"
                   : "a zero-delay inertial update was applied outside an NBA pass",
             0, std::string("the scheduler is in the ") + region_name(g.region) + " region");
        return;
    }
    r->state = IState::Done;
    --g.inert_waiting;
    if (wheel) {
        --g.inert_wheel_waiting;
    }
}

extern "C" void rt_trace_once(uint32_t kind) {
    if (g.halted) {
        return;
    }
    g.once = kind;
}

extern "C" void rt_trace_slot(uint64_t now) {
    if (g.halted) {
        return;
    }
    settle_owed();
    if (g.halted) {
        return;
    }
    ++g.step;
    g.now    = now;
    g.region = RT_TRACE_REG_PREPONED;
    ++g.slot_no;
    record(Ev::Slot, 0, now, nullptr, 0, 0);
}

extern "C" void rt_trace_slot_end(void) {
    if (g.halted || !g.counted) {
        return;
    }
    record(Ev::SlotEnd, 0, 0, nullptr, 0, 0);
    settle_owed();
    if (g.halted) {
        return;
    }
    if (g.in_pass) {
        fail("R12.3", "the slot ends inside an NBA pass", 0, "");
        return;
    }
    if (closure("the slot ends", false)) {
        return;
    }
    if (poll_inertial() || poll_timers()) {
        return;
    }
    // Time advances only when every region is empty (R1.2). A slot with no region reported (a hand-built
    // sequence; a run always enters the Active region) has no region that was left.
    if (!g.finished && g.region != RT_TRACE_REG_PREPONED) {
        if (left_with_work("the slot ends", true, true, true)) {
            return;
        }
        // A pended `$strobe` runs in the Postponed region of its slot (R4.2).
        if (g.strobes_owed > 0) {
            const uintptr_t who = first_act([](const Act& act) { return act.strobe_owed; });
            fail("R4.2", "a $strobe pended in this slot never ran: its Postponed region did not drain it", who, "");
            return;
        }
        // A continuously active process is evaluated at time zero (R15).
        if (g.slot_no == 0 && g.zero_owed > 0) {
            const uintptr_t who = first_act([](const Act& act) { return act.zero_owed; });
            fail("R15.1", "a continuously active process was not evaluated at time zero: the seed never ran it", who, "");
            return;
        }
    }
    poll_waiters();
}

extern "C" void rt_trace_finish(void) {
    if (g.halted || !g.counted) {
        return;
    }
    ++g.run.finishes;
    ++g.total.finishes;
    record(Ev::Finish, 0, 0, nullptr, 0, 0);
    if (g.in_final) {
        return;
    }
    g.finished  = true;
    g.finish_at = g.seq;
    // What the finishing slot left staged is never applied: the run ends here (decision 5).
    for (Upd& u : g.pending) {
        retire(u);
    }
    g.pending.clear();
    g.split = 0;
    g.pends.clear();
}

extern "C" void rt_trace_region(uint32_t region) {
    if (g.halted || (region == RT_TRACE_REG_FINAL && !g.counted)) {
        return;
    }
    settle_owed();
    if (g.halted) {
        return;
    }
    ++g.step;
    if (region != RT_TRACE_REG_FINAL) {
        touch_run();
    }
    record(Ev::Region, 0, 0, nullptr, static_cast<uint8_t>(region), 0);
    if (region != RT_TRACE_REG_FINAL && !g.finished) {
        const uint32_t from = g.region;
        if (!legal_region_step(from, region)) {
            fail("R1.1",
                 std::string("the ") + region_name(region) + " region begins after the " + region_name(from) +
                   " region, which it does not follow",
                 0, "");
            return;
        }
        // The commit at the top of the slot has applied every inertial update the wheel fired, before the Active
        // region reads the nets (S26).
        if (region == RT_TRACE_REG_ACTIVE && from == RT_TRACE_REG_PREPONED && g.inert_wheel_waiting > 0) {
            fail("R16.5",
                 "an inertial update that the wheel fired was not applied before the Active region began: its readers would see the old value",
                 0, inert_waiting_text());
            return;
        }
        // A region begins with every earlier one empty (R1.2), and a batch of `#0` is promoted whole
        // (R2.2).
        const auto batch_left = [&](bool reactive) {
            if (g.n_zero[reactive ? 1 : 0] == 0) {
                return false;
            }
            const uintptr_t who = first_act([&](const Act& act) { return zero_parked(act) && act.home_reactive == reactive; });
            fail("R2.2", "a #0 was left out of the promotion of its batch: every process parked at a #0 becomes active together", who,
                 "still parked at a #0 once the batch was promoted");
            return true;
        };
        bool failed = false;
        switch (region) {
            case RT_TRACE_REG_ACTIVE: failed = from == RT_TRACE_REG_INACTIVE && batch_left(false); break;
            case RT_TRACE_REG_INACTIVE:
                failed = from == RT_TRACE_REG_ACTIVE && left_with_work("the Inactive region begins", true, false, false);
                break;
            case RT_TRACE_REG_OBSERVED:
                failed = from == RT_TRACE_REG_ACTIVE && left_with_work("the Observed region begins", true, false, true);
                break;
            case RT_TRACE_REG_REACTIVE:
                if (from == RT_TRACE_REG_RE_INACTIVE) {
                    failed = batch_left(true);
                } else if (from == RT_TRACE_REG_ACTIVE || from == RT_TRACE_REG_OBSERVED) {
                    failed = left_with_work("the Reactive region begins", true, false, true);
                }
                break;
            case RT_TRACE_REG_RE_INACTIVE:
                failed = from == RT_TRACE_REG_REACTIVE && left_with_work("the Re-Inactive region begins", false, true, false);
                break;
            case RT_TRACE_REG_POSTPONED:
                failed = (from == RT_TRACE_REG_ACTIVE || from == RT_TRACE_REG_OBSERVED || from == RT_TRACE_REG_REACTIVE) &&
                         left_with_work("the Postponed region begins", true, true, true);
                break;
            default: break;
        }
        if (failed) {
            return;
        }
    }
    switch (region) {
        case RT_TRACE_REG_OBSERVED:
        case RT_TRACE_REG_REACTIVE:
        case RT_TRACE_REG_RE_INACTIVE:
            // What the Active set staged was applied and triggered before the Reactive set runs.
            if (closure((std::string("the ") + region_name(region) + " region begins").c_str(), true)) {
                return;
            }
            break;
        case RT_TRACE_REG_POSTPONED:
            if (closure("the postponed region begins", false)) {
                return;
            }
            break;
        case RT_TRACE_REG_FINAL:
            // The finals run: the exit of a `$finish` is over.
            g.finished = false;
            g.in_final = true;
            break;
        default:
            break;
    }
    g.region = region;
}

extern "C" void rt_trace_nba_create(const rt_trace_nba_site* site, void* dst0, void* dst1, const void* val0,
                                    const void* mask0, const void* val1, const void* mask1) {
    if (g.halted) {
        return;
    }
    touch_run();
    const uint32_t path = site->flags & 0xff;
    if (path < RT_TRACE_NBA_PATHS) {
        ++g.run.creates[path];
        ++g.total.creates[path];
    }
    record(Ev::Create, reinterpret_cast<uintptr_t>(dst0), 0, nullptr, static_cast<uint8_t>(path), 0, site);
    if (site->name != nullptr) {
        g.names.at(reinterpret_cast<uintptr_t>(dst0)) = site->name;
    }
    if (path != RT_TRACE_NBA_BLOCK && path != RT_TRACE_NBA_ARRAY && path != RT_TRACE_NBA_AGG && path != RT_TRACE_NBA_ZERO) {
        fail("R12.3", "an update that a statement executes carries the path of a timed update", 0, "");
        return;
    }
    Upd u    = make_upd(site, dst0, dst1, val0, mask0, val1, mask1);
    u.tick   = g.now;
    u.region = g.region;
    if (g.finished) {
        fail("R24.1", "an NBA statement ran after $finish: code of the finishing thread, or of something it woke, ran on", 0,
             "created: " + upd_text(u));
        return;
    }
    if (g.in_pass) {
        fail("R12.3", "an NBA statement ran inside an NBA pass: the region that applies updates runs no process", 0,
             "created: " + upd_text(u));
        return;
    }
    g.pending.push_back(std::move(u));
}

extern "C" void rt_trace_nba_stage(const void* rec, uint64_t ticks, const rt_trace_nba_site* site, void* dst0, void* dst1,
                                   const void* val0, const void* mask0, const void* val1, const void* mask1) {
    if (g.halted) {
        return;
    }
    touch_run();
    const uint32_t path = site->flags & 0xff;
    record(Ev::Stage, reinterpret_cast<uintptr_t>(dst0), 0, nullptr, static_cast<uint8_t>(path), 0, site);
    if (site->name != nullptr) {
        g.names.at(reinterpret_cast<uintptr_t>(dst0)) = site->name;
    }
    if (path != RT_TRACE_NBA_TIMER && path != RT_TRACE_NBA_EVENT) {
        fail("R12.3", "a record is armed with the path of an update that a statement creates at once", 0, "");
        return;
    }
    Upd u    = make_upd(site, dst0, dst1, val0, mask0, val1, mask1);
    u.tick   = g.now;
    u.region = g.region;
    u.arm    = ++g.arm_seq;
    // A timer releases the update when its delay ends (R11.1).
    u.deadline = path == RT_TRACE_NBA_TIMER ? (ticks > UINT64_MAX - g.now ? UINT64_MAX : g.now + ticks) : 0;
    if (g.finished) {
        fail("R24.1", "an NBA statement ran after $finish: code of the finishing thread, or of something it woke, ran on", 0,
             "armed: " + upd_text(u));
        return;
    }
    if (g.in_pass) {
        fail("R12.3", "an NBA statement ran inside an NBA pass: the region that applies updates runs no process", 0,
             "armed: " + upd_text(u));
        return;
    }
    Upd& slot = g.armed.at(reinterpret_cast<uintptr_t>(rec));
    retire(slot);
    slot = std::move(u);
}

extern "C" void rt_trace_nba_event(const void* rec_, uint32_t count, const void* x_, uint32_t edge) {
    if (g.halted || g.finished) {
        return;
    }
    touch_run();
    const uintptr_t rec = reinterpret_cast<uintptr_t>(rec_);
    const uintptr_t x   = reinterpret_cast<uintptr_t>(x_);
    Upd*            u   = g.armed.find(rec);
    if (u == nullptr || u->site == nullptr || u->path != RT_TRACE_NBA_EVENT) {
        fail("R11.2", "an event was reported for a record that no `<= @e` statement armed", 0, "the record at " + address_text(rec));
        return;
    }
    u->count    = count == 0 ? 1 : count;
    bool listed = false;
    for (const EWait& w : u->waits) {
        listed = listed || (w.x == x && w.edge == edge);
    }
    if (!listed) {
        u->waits.push_back(EWait{ x, edge });
    }
    std::vector<EWatch>& v = g.ewatch.at(x);
    for (const EWatch& w : v) {
        if (w.rec == rec && w.arm == u->arm) {
            return;
        }
    }
    // Watchers whose record has been released since are dropped as the list grows.
    const size_t n = v.size();
    if (n >= 16 && (n & (n - 1)) == 0) {
        size_t kept = 0;
        for (const EWatch& w : v) {
            const Upd* o = g.armed.find(w.rec);
            if (o != nullptr && o->site != nullptr && o->arm == w.arm) {
                v[kept++] = w;
            }
        }
        v.resize(kept);
    }
    v.push_back(EWatch{ rec, u->arm });
}

extern "C" void rt_trace_nba_release(const void* rec, uint32_t path, uint32_t moment) {
    if (g.halted || !g.counted) {
        return;
    }
    Upd* armed = g.armed.find(reinterpret_cast<uintptr_t>(rec));
    if (armed == nullptr || armed->site == nullptr) {
        record(Ev::Release, 0, 0, nullptr, static_cast<uint8_t>(path), 0);
        fail("R12.3", "a timer or an event released an update that no statement armed: a staging path the trace does not report",
             0, "");
        return;
    }
    if (armed->path != path) {
        fail("R12.3",
             std::string("a record armed for ") + path_name(armed->path) + " was released as " + path_name(path), 0,
             "armed: " + upd_text(*armed));
        return;
    }
    // The timer releases it at the tick its delay ends, and an event when the last occurrence it counts
    // comes (R11); a record the trace was told no event of is not asked.
    if (!g.finished) {
        if (path == RT_TRACE_NBA_TIMER && armed->deadline != UINT64_MAX && g.now != armed->deadline) {
            std::string what;
            appendf(what, "a `<= #d` update was created at tick %llu, and its delay ends at tick %llu",
                    static_cast<unsigned long long>(g.now), static_cast<unsigned long long>(armed->deadline));
            fail("R11.1", what, 0, "armed: " + upd_text(*armed));
            return;
        }
        if (path == RT_TRACE_NBA_EVENT && !armed->waits.empty() && armed->occ < armed->count) {
            std::string what;
            appendf(what, "a `<= @e` update was created before the last occurrence of its control that it counts: %u of the %u occurrences had come",
                    armed->occ, armed->count);
            fail("R11.2", what, 0, "armed: " + upd_text(*armed));
            return;
        }
    }
    Upd u = std::move(*armed);
    armed->site = nullptr;
    armed->w.clear();
    if (path < RT_TRACE_NBA_PATHS) {
        ++g.run.creates[path];
        ++g.total.creates[path];
    }
    record(Ev::Release, u.dst[0], 0, nullptr, static_cast<uint8_t>(path), 0, u.site);
    u.tick   = g.now;
    u.region = g.region;
    // One wheel tick is one moment; an occurrence of an event is the scheduler's own number.
    u.moment = path == RT_TRACE_NBA_TIMER ? (uint64_t{ 1 } << 62) | g.slot_no : (uint64_t{ 2 } << 62) | moment;
    if (g.finished) {
        fail("R24.1", "an NBA update was created after $finish: code of the finishing thread, or of something it woke, ran on", 0,
             "created: " + upd_text(u));
        return;
    }
    g.pending.push_back(std::move(u));
    if (g.in_pass) {
        // Created while the pass applies the others: the planes it names are read before anything of
        // the pass changes them, which only a word the pass already named could have.
        Upd& made = g.pending.back();
        for (int p = 0; p < 2; ++p) {
            if (made.dst[p] != 0) {
                made.key[p] = key_index(made.dst[p], made.bits(p), skips(made));
            }
        }
    }
}

extern "C" void rt_trace_nba_pass(uint32_t begin, uint32_t region) {
    if (g.halted) {
        return;
    }
    touch_run();
    record(begin != 0 ? Ev::PassBegin : Ev::PassEnd, 0, 0, nullptr, static_cast<uint8_t>(region), 0);
    if (region != RT_TRACE_REG_NBA && region != RT_TRACE_REG_RE_NBA) {
        fail("R12.3", "an NBA pass of a region that is neither the NBA nor the Re-NBA region", 0, "");
        return;
    }
    if (begin == 0) {
        if (!g.in_pass || g.pass_region != region) {
            fail("R12.3", std::string("an NBA pass of the ") + region_name(region) + " region ends that did not begin", 0, "");
            return;
        }
        g.in_pass = false;
        settle_pass();
        return;
    }
    ++g.run.nba_passes;
    ++g.total.nba_passes;
    if (g.in_pass) {
        fail("R12.3", "an NBA pass begins inside another", 0, "");
        return;
    }
    // The NBA region follows the Active drain and the Re-NBA region the Reactive one: a pass that
    // begins from anywhere else was entered out of order.
    const uint32_t from = region == RT_TRACE_REG_RE_NBA ? RT_TRACE_REG_REACTIVE : RT_TRACE_REG_ACTIVE;
    if (g.region != from) {
        fail("R12.3",
             std::string("the ") + region_name(region) + " region begins from the " + region_name(g.region) +
               " region, not from the " + region_name(from) + " region it follows",
             0, "");
        return;
    }
    // The NBA region begins with the Active region and the Inactive batch empty (R1.2); the Re-NBA
    // region's own bodies wake what is queued when its pass begins.
    if (region == RT_TRACE_REG_NBA && !g.finished && left_with_work("the NBA region begins", true, false, true)) {
        return;
    }
    g.in_pass     = true;
    g.pass_region = region;
    g.region      = region;
    begin_pass();
}

extern "C" void rt_trace_pend(const void* obj) {
    if (g.halted) {
        return;
    }
    touch_run();
    ++g.run.pends;
    ++g.total.pends;
    const uintptr_t o = reinterpret_cast<uintptr_t>(obj);
    record(Ev::Pend, o, 0, nullptr, 0, 0);
    if (g.finished) {
        fail("R24.1", "a `->>` ran after $finish: code of the finishing thread, or of something it woke, ran on", 0,
             "event " + signal_text(obj));
        return;
    }
    for (const Pend& p : g.pends) {
        if (p.obj == o) {
            std::string cause = "event " + signal_text(obj);
            appendf(cause, " pended at tick %llu", static_cast<unsigned long long>(p.tick));
            fail("R20.2", "a `->>` links an event that is already on the pend chain: the chain would loop", 0, cause);
            return;
        }
    }
    g.pends.push_back(Pend{ o, g.now, g.region });
}

extern "C" void rt_trace_trigger(const void* obj, uint32_t kind) {
    if (g.halted) {
        return;
    }
    touch_run();
    ++g.run.triggers;
    ++g.total.triggers;
    const uintptr_t o = reinterpret_cast<uintptr_t>(obj);
    record(Ev::Trigger, o, 0, nullptr, static_cast<uint8_t>(kind), 0);
    if (g.finished) {
        fail("R24.1", "an event triggered after $finish: code of the finishing thread, or of something it woke, ran on", 0,
             "event " + signal_text(obj));
        return;
    }
    count_occurrence(o, false, false, false, true);
    owe(obj, false, false, false, true);
    if (kind != RT_TRACE_TRIGGER_NB) {
        return;
    }
    if (!g.in_pass) {
        fail("R20.1", std::string("a `->>` triggers outside an NBA pass, in the ") + region_name(g.region) + " region", 0,
             "event " + signal_text(obj) + ": its update event belongs to the NBA or Re-NBA region of the slot");
        return;
    }
    if (g.pends.empty()) {
        fail("R20.2", "a `->>` triggers an event that no statement pended", 0, "event " + signal_text(obj));
        return;
    }
    if (g.pends.front().obj != o) {
        std::string cause = "triggered: event " + signal_text(obj) + "; the oldest `->>` not yet triggered is on event " +
                            signal_text(reinterpret_cast<const void*>(g.pends.front().obj));
        appendf(cause, ", pended at tick %llu", static_cast<unsigned long long>(g.pends.front().tick));
        fail("R20.2", "`->>` triggers fire out of the order they were pended in", 0, cause);
        return;
    }
    g.pends.pop_front();
}

extern "C" void rt_trace_change(const rt_trace_change_site* site, const void* x, uint32_t seen) {
    if (g.halted) {
        return;
    }
    touch_run();
    ++g.run.changes;
    ++g.total.changes;
    record(Ev::Change, reinterpret_cast<uintptr_t>(x), seen, nullptr, static_cast<uint8_t>(site->path), 0);
    if (site->name != nullptr && x != nullptr) {
        g.names.at(reinterpret_cast<uintptr_t>(x)) = site->name;
    }
    const auto has_seen = [&](uint32_t f) { return (seen & f) != 0; };
    bool       pos = false;
    bool       neg = false;
    bool       any = false;
    if (has_seen(RT_TRACE_SEEN_LEVEL)) {
        any = true;
    } else {
        const bool     four = has_seen(RT_TRACE_SEEN_FOUR);
        const uint32_t ro   = rank_of(has_seen(RT_TRACE_SEEN_OLD_MUST), four && has_seen(RT_TRACE_SEEN_OLD_MAY));
        const uint32_t rn   = rank_of(has_seen(RT_TRACE_SEEN_NEW_MUST), four && has_seen(RT_TRACE_SEEN_NEW_MAY));
        const bool     bit0 = has_seen(RT_TRACE_SEEN_BIT0);
        if (bit0) {
            pos = rn > ro;
            neg = rn < ro;
            const struct {
                uint32_t known;
                uint32_t value;
                bool     expected;
                const char* name;
            } facts[] = { { RT_TRACE_SEEN_POS_KNOWN, RT_TRACE_SEEN_POS, pos, "posedge" },
                          { RT_TRACE_SEEN_NEG_KNOWN, RT_TRACE_SEEN_NEG, neg, "negedge" } };
            for (const auto& f : facts) {
                if (has_seen(f.known) && has_seen(f.value) != f.expected) {
                    std::string cause = std::string("bit 0 went from ") + rank_name(ro) + " to " + rank_name(rn) +
                                        " (Table 9-2: 0 < x = z < 1), so the " + f.name + " fact must be " +
                                        (f.expected ? "true" : "false") + " and the code computed " +
                                        (f.expected ? "false" : "true") + "; write of " +
                                        signal_text(x) + " at " + (site->where != nullptr ? site->where : "<unknown>");
                    fail("R7.1", std::string("the ") + f.name + " fact of a write disagrees with Table 9-2", 0, cause);
                    return;
                }
            }
        } else {
            pos = has_seen(RT_TRACE_SEEN_POS_KNOWN) && has_seen(RT_TRACE_SEEN_POS);
            neg = has_seen(RT_TRACE_SEEN_NEG_KNOWN) && has_seen(RT_TRACE_SEEN_NEG);
        }
        if (has_seen(RT_TRACE_SEEN_DIFF_KNOWN)) {
            const bool diff  = has_seen(RT_TRACE_SEEN_DIFF);
            const bool zdiff = has_seen(RT_TRACE_SEEN_ZDIFF_KNOWN) && has_seen(RT_TRACE_SEEN_ZDIFF);
            // A z word the bracket did not read (no level waiter consumes it) leaves `changed` known
            // only to be true when a plane differs.
            const bool exact = !has_seen(RT_TRACE_SEEN_HAS_Z) || has_seen(RT_TRACE_SEEN_ZDIFF_KNOWN);
            if (has_seen(RT_TRACE_SEEN_ANY_KNOWN)) {
                const bool code = has_seen(RT_TRACE_SEEN_ANY);
                if ((exact && code != (diff || zdiff)) || (!exact && diff && !code)) {
                    std::string cause = std::string("the planes ") + (diff ? "differ" : "are equal") + ", the z word " +
                                        (has_seen(RT_TRACE_SEEN_ZDIFF_KNOWN) ? (zdiff ? "differs" : "is equal") : "was not read") +
                                        ", and the code computed `changed` = " + (code ? "true" : "false") + "; write of " +
                                        signal_text(x) + " at " + (site->where != nullptr ? site->where : "<unknown>");
                    fail("R7.2", "the `changed` fact of a write disagrees with the planes it wrote", 0, cause);
                    return;
                }
            }
            any = diff || zdiff;
        } else if (has_seen(RT_TRACE_SEEN_ANY_KNOWN)) {
            any = has_seen(RT_TRACE_SEEN_ANY);
        }
        any = any || pos || neg;
    }
    if (x != nullptr) {
        g.chg.at(reinterpret_cast<uintptr_t>(x)) = ChgFacts{ g.step, pos, neg, any };
        count_occurrence(reinterpret_cast<uintptr_t>(x), pos, neg, any, false);
        owe(x, pos, neg, any, false);
    }
}

extern "C" int32_t rt_trace_halted(void) {
    return g.halted ? 1 : 0;
}

extern "C" const char* rt_trace_rule(void) {
    return g.halted ? g.rule.c_str() : nullptr;
}

extern "C" const char* rt_trace_report(void) {
    return g.report.c_str();
}

extern "C" void rt_trace_totals(rt_trace_counts* out) {
    *out = g.total;
}

#endif // CLOTHO_EXTRA_ASSERTS
