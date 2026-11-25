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
};

struct Entry {
    const void* x;
    const char* name;
    uint64_t    edge;
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
};

constexpr size_t kRing = 256;

// -- Nonblocking updates (R12) --

// The 64-bit words that hold `bits` bits.
constexpr uint32_t words_of(uint32_t bits) {
    return (bits + 63) / 64;
}

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

// A `->>` linked onto the pend chain and not yet triggered.
struct Pend {
    uintptr_t obj;
    uint64_t  tick;
    uint32_t  region;
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
    // The `->>` linked and not yet triggered, oldest first.
    std::deque<Pend>     pends;
    // The word buffers of retired updates, so that a design that stages an update every cycle
    // allocates none.
    std::vector<std::vector<uint64_t>> spare;
};

Checker g;

// What the handle's tag bits never are: bit 0 stays (a thunk group), bits 1 and 2 are the
// home region of a parked handle.
uintptr_t norm(const void* a) {
    return reinterpret_cast<uintptr_t>(a) & ~static_cast<uintptr_t>(6);
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
    switch (kind) {
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
        case Ev::End: return true;
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
        { "R12.1", "nba order" },
        { "R12.2", "nba left unapplied" },
        { "R12.3", "nba pass" },
        { "R20.1", "->> region" },
        { "R20.2", "->> order" },
        { "R20.3", "->> left untriggered" },
        { "R24.1", "effect after $finish" },
        { "R24.2", "run after $finish" },
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
        default: break;
    }
    return out;
}

void record(Ev ev, uintptr_t a, uint64_t x, const rt_trace_control* c, uint8_t cause, uint8_t fact,
            const rt_trace_nba_site* site = nullptr) {
    g.ring[g.seq % kRing] = Rec{ a, x, g.now, c, site, ev, cause, fact };
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
            return kind == RT_TRACE_CTL_JOIN ? "" : "a fork child woke a parent that is not parked at a join";
        case RT_TRACE_CALLEE:
            return kind == RT_TRACE_CTL_CALL ? "" : "a callee woke a caller that is not parked at its task call";
        case RT_TRACE_AWAIT:
            return kind == RT_TRACE_CTL_AWAIT || kind == RT_TRACE_CTL_WAIT_FORK
                     ? ""
                     : "a thread's completion woke an activation that is not parked at an await or a wait fork";
        case RT_TRACE_SYNC:
            return kind == RT_TRACE_CTL_SYNC ? "" : "a semaphore or mailbox woke an activation that is not parked at its queue";
        case RT_TRACE_EXPECT:
            return kind == RT_TRACE_CTL_EXPECT ? "" : "an expect verdict woke an activation that is not parked at an expect";
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
    }
    return out;
}

void add_entries(std::vector<Entry>& to, const rt_trace_entry* list, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        to.push_back(Entry{ list[i].x, list[i].name, list[i].edge });
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
    return false;
}

} // namespace

extern "C" void rt_trace_begin(void) {
    g.acts.clear();
    g.names.clear();
    g.armed.clear();
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
    Act& act   = g.acts.at(a);
    act.state  = State::Spawned;
    act.origin = origin;
    act.park   = nullptr;
    act.parent = norm(parent);
    act.deadline = 0;
    act.zero   = false;
    act.stat.clear();
    act.list.clear();
    act.pending.clear();
    record(Ev::Spawn, a, 0, origin, 0, 0);
}

extern "C" void rt_trace_subscribe(void* a_, const rt_trace_entry* list, uint32_t n) {
    if (g.halted) {
        return;
    }
    if (Act* act = find("R5.3", norm(a_), "subscription"); act != nullptr) {
        add_entries(act->stat, list, n);
    }
}

extern "C" void rt_trace_subscribe_event(void* a_, const void* obj, const char* name) {
    if (g.halted) {
        return;
    }
    if (Act* act = find("R5.3", norm(a_), "subscription"); act != nullptr) {
        const rt_trace_entry e{ obj, name, RT_TRACE_OBJ };
        add_entries(act->stat, &e, 1);
    }
}

extern "C" void rt_trace_waits_for(void* a_, const void* x, const char* name, uint64_t edge) {
    if (g.halted) {
        return;
    }
    if (Act* act = find("R5.3", norm(a_), "park list"); act != nullptr) {
        const rt_trace_entry e{ x, name, edge };
        add_entries(act->pending, &e, 1);
    }
}

extern "C" void rt_trace_park(void* a_, const rt_trace_control* c, uint64_t delay) {
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
    act->state = State::Parked;
    act->park  = c;
    act->list.clear();
    if (c != nullptr && c->kind == RT_TRACE_CTL_EVENT) {
        act->list.swap(act->pending);
    }
    act->pending.clear();
    act->zero     = delay == 0;
    act->deadline = delay > UINT64_MAX - g.now ? UINT64_MAX : g.now + delay;
    record(Ev::Park, a, delay, c, 0, 0);
}

extern "C" void rt_trace_wake(void* a_, uint32_t cause, const void* x, uint32_t fact) {
    if (g.halted) {
        return;
    }
    const uintptr_t a = norm(a_);
    ++g.run.wakes;
    ++g.total.wakes;
    Act* act = find("R5.1", a, "wake");
    if (act == nullptr) {
        return;
    }
    record(Ev::Wake, a, reinterpret_cast<uintptr_t>(x), act->park, static_cast<uint8_t>(cause), static_cast<uint8_t>(fact));
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
    // Past a `$finish` only the completions that make each frame leave wake anything (R24).
    if (g.finished && !completion_cause(cause)) {
        fail("R24.1", "wake after $finish: code of the finishing thread, or of something it woke, ran on", a,
             cause_text(cause, x, fact));
        return;
    }
    act->state        = State::Queued;
    act->queued_at    = g.seq;
    act->queued_cause = static_cast<uint8_t>(cause);
}

extern "C" int32_t rt_trace_dispatch(void* a_, uint32_t how) {
    if (g.halted) {
        return 0;
    }
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
    }
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
    act->state = State::Ended;
}

extern "C" void rt_trace_slot(uint64_t now) {
    if (g.halted) {
        return;
    }
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
    if (g.in_pass) {
        fail("R12.3", "the slot ends inside an NBA pass", 0, "");
        return;
    }
    closure("the slot ends", false);
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
    if (region != RT_TRACE_REG_FINAL) {
        touch_run();
    }
    record(Ev::Region, 0, 0, nullptr, static_cast<uint8_t>(region), 0);
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

extern "C" void rt_trace_nba_stage(const void* rec, const rt_trace_nba_site* site, void* dst0, void* dst1, const void* val0,
                                   const void* mask0, const void* val1, const void* mask1) {
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
