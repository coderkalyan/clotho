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

#include <cstdarg>
#include <cstdio>
#include <cstring>
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

enum class Ev : uint8_t { Spawn, Park, Wake, Dispatch, Resume, End, Slot };

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
    uint64_t                a;
    uint64_t                x;
    uint64_t                now;
    const rt_trace_control* c;
    Ev                      ev;
    uint8_t                 cause; // a wake's cause, a dispatch's mode, a park's delay flag
    uint8_t                 fact;
};

constexpr size_t kRing = 256;

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
    if (r.ev != Ev::Slot) {
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
        default: break;
    }
    return out;
}

void record(Ev ev, uintptr_t a, uint64_t x, const rt_trace_control* c, uint8_t cause, uint8_t fact) {
    g.ring[g.seq % kRing] = Rec{ a, x, g.now, c, ev, cause, fact };
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
    const char* title = std::strcmp(rule, "R5.1") == 0   ? "wake"
                        : std::strcmp(rule, "R5.2") == 0 ? "dispatch"
                        : std::strcmp(rule, "R5.3") == 0 ? "resume"
                                                         : "end";
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
            if (r.ev != Ev::Slot && r.a == a) {
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

} // namespace

extern "C" void rt_trace_begin(void) {
    g.acts.clear();
    g.names.clear();
    g.now     = 0;
    g.halted  = false;
    g.seq     = 0;
    g.rule.clear();
    g.report.clear();
    g.run     = {};
    g.counted = false;
}

extern "C" void rt_trace_spawn(void* a_, const rt_trace_control* origin, void* parent) {
    if (g.halted) {
        return;
    }
    if (!g.counted) {
        g.counted = true;
        ++g.total.runs;
    }
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
                fail("R5.1", "wake of an activation that is not waiting for it: " + why, a, cause_text(cause, x, fact));
                return;
            }
            break;
        }
    }
    act->state = State::Queued;
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
    g.now = now;
    record(Ev::Slot, 0, now, nullptr, 0, 0);
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
