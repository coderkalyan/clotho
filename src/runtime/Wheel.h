// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Single-level hashed timing wheel that jumps over idle ticks.
//
// Time is an abstract uint64 tick count; the wheel never interprets it. The
// caller owns the mapping from real time to ticks, so granularity (ns/cycle/...)
// is entirely external. The wheel only ever compares and masks tick values.
//
// One revolution covers `slots` ticks (a compile-time power of two). A timer due
// within the current revolution hangs off a slot's intrusive list in O(1); a
// timer due further out spills into a binary min-heap overflow queue ordered by
// deadline, and is cascaded into a slot as the wheel rotates close enough to it.
//
// tick() does not step one tick at a time: a per-slot occupancy bitfield plus a
// find-first-set locates the nearest non-empty slot and jumps `now` straight to
// it, firing only ticks that actually hold timers. An empty wheel jumps directly
// to the earliest overflow deadline.
//
// Each timer carries its own expiry callback as a plain function pointer. The
// wheel is templated on the extra arguments forwarded to that callback, fixed at
// the wheel's type; tick(args...) calls each fired timer as on_expire(t, args...)
// so the callback can reach shared context without capturing.
//
// Allocation: slots and the bitfield are fixed inline arrays and timer nodes are
// caller-owned (intrusive), so the wheel's only allocation is the overflow heap's
// backing vector — which reuses its capacity and only grows for timers more than
// one revolution out.

#include <cstdint>
#include <queue>
#include <vector>

namespace clotho {

// Intrusive timer node. Embed it inside an owning object (an awaiter) and
// recover that owner from a Timer& via offsetof. Carries its own expiry callback
// type-erased; the typed signature lives on the Wheel that schedules it.
struct Timer {
    // Intrusive slot-list link. Unspecified unless linked into a slot (a timer
    // waiting in the overflow heap is held by pointer, not via this link).
    Timer* next = nullptr;
    // Type-erased expiry callback. Set by Wheel::schedule, re-typed on fire.
    void (*on_expire)() = nullptr;
};

// Non-template wheel mechanism: tick math, the slot occupancy bitfield, and the
// overflow cascade. Callback dispatch is left to the typed Wheel wrapper, so the
// argument-agnostic bulk lives in Wheel.cpp rather than the header.
class WheelImpl {
public:
    // Slot count: compile-time, a power of two and a multiple of 64 (so the
    // occupancy bitfield is whole words). One revolution spans `slots` ticks; a
    // delay within it is an O(1) slot insert, one beyond it spills into the
    // O(log n) overflow heap. A wider window therefore keeps more concurrent
    // timers on the fast path — e.g. many independent `always #p` toggles or
    // clocking-block delays in a testbench, whose periods routinely exceed 256.
    // The ceiling is the L1 working set: `advance` sweeps `now & mask` across
    // every slot over a revolution, so the whole slots_ array is touched, and
    // once it stops fitting in L1 the trivial single-clock case regresses (the
    // clock microbench measured neutral at 1024 = 8 KiB of pointers, +7% at 2048,
    // +8% at 4096). 1024 keeps the array L1-resident on a 32 KiB L1D while cutting
    // the populated-wheel bench (512 concurrent prime-period timers) ~19%.
    static constexpr uint64_t slots = 1024;
    static_assert((slots & (slots - 1)) == 0 && slots != 0, "slots must be a power of two");
    static_assert(slots % 64 == 0, "slots must be a multiple of 64");

    WheelImpl() = default;
    // Non-copyable / non-movable: cur_word_ points into occupied_.
    WheelImpl(const WheelImpl&)            = delete;
    WheelImpl& operator=(const WheelImpl&) = delete;

    // Current tick. Starts at zero.
    uint64_t now() const {
        return now_;
    }

    // True iff no timers are scheduled anywhere (neither in a wheel slot nor the
    // overflow heap). Drives the drain loop: tick until empty.
    bool empty() const {
        for (uint64_t word : occupied_) {
            if (word != 0) {
                return false;
            }
        }
        return overflow_.empty();
    }

    // Link `t` to fire at absolute tick `deadline` (must be > now()). The
    // node's callback must already be set.
    void schedule(Timer& t, uint64_t deadline);

    // Jump to the next non-empty tick, detach that tick's timers, and return the
    // head of the singly-linked batch (null if nothing is scheduled at all).
    // now() advances to the fired tick. The caller fires each returned timer.
    Timer* advance();

    // Tick of the earliest scheduled timer, or `never` when nothing is
    // scheduled. Moves nothing.
    static constexpr uint64_t never = ~0ull;
    uint64_t                  next_tick() const;

    // Move now() forward to `tick` without firing anything. Only legal while no
    // timer is due at or before `tick`, i.e. now() <= tick < next_tick(): the
    // run is over and the clock is being set to the instant it ends at.
    void skip_to(uint64_t tick);

private:
    static constexpr uint64_t mask   = slots - 1;
    static constexpr uint64_t nwords = slots / 64;
    static_assert((nwords & (nwords - 1)) == 0, "nwords must be a power of two");

    static uint64_t slot_of(uint64_t tick) {
        return tick & mask;
    }

    void set_occupied(uint64_t s) {
        occupied_[s >> 6] |= 1ull << (s & 63);
    }

    void clear_occupied(uint64_t s) {
        occupied_[s >> 6] &= ~(1ull << (s & 63));
    }

    // One overflow timer plus the deadline it is waiting on (Timer no longer
    // stores its own deadline, so the heap entry carries it).
    struct Overflow {
        uint64_t deadline;
        Timer*   timer;
    };

    // Min-heap order: the earliest deadline is on top.
    struct EarliestFirst {
        bool operator()(const Overflow& a, const Overflow& b) const {
            return a.deadline > b.deadline;
        }
    };

    // Offset (>= 1) to the nearest occupied slot ahead of now_, or 0 if the
    // wheel window holds no timers.
    uint64_t nearest_offset() const;

    // Pull overflow timers now within one revolution into their slots.
    void cascade();

    // Current tick.
    uint64_t now_ = 0;
    // Per-slot intrusive list heads (null when empty).
    Timer* slots_[slots] = {};
    // One bit per slot: set iff slots_[slot] is non-empty. Drives the jump.
    uint64_t occupied_[nwords] = {};
    // The occupied_ word holding the current bucket (now_ & mask), kept in sync
    // with now_. The nearest-slot search masks and ffs's this word first, only
    // walking the rest on a miss.
    uint64_t* cur_word_ = occupied_;
    // Overflow queue for timers more than one revolution out: a binary min-heap
    // ordered by deadline. The wheel's only allocator (its backing vector).
    std::priority_queue<Overflow, std::vector<Overflow>, EarliestFirst> overflow_;
};

// Typed front-end. The template arguments are the extra parameters forwarded to
// each fired timer's callback, fixed at the wheel's type. tick(args...) advances
// to the next non-empty tick and invokes every fired timer as
// on_expire(timer, args...), in list order.
template<class... Args>
class Wheel {
public:
    using Timer = clotho::Timer;
    // Pointer (not reference) parameter so the runtime's C-ABI mirror callback
    // type is token-identical and the cross-ABI cast stays warning-free.
    using Callback = void (*)(Timer*, Args...);

    static constexpr uint64_t slots = WheelImpl::slots;

    uint64_t now() const {
        return impl_.now();
    }

    // True iff nothing is scheduled. See WheelImpl::empty.
    bool empty() const {
        return impl_.empty();
    }

    // See WheelImpl::next_tick.
    static constexpr uint64_t never = WheelImpl::never;

    uint64_t next_tick() const {
        return impl_.next_tick();
    }

    // See WheelImpl::skip_to.
    void skip_to(uint64_t tick) {
        impl_.skip_to(tick);
    }

    // Schedule `t` to fire at `deadline` (> now()), invoking on_expire(t, args...).
    void schedule(Timer& t, uint64_t deadline, Callback on_expire) {
        t.on_expire = reinterpret_cast<void (*)()>(on_expire);
        impl_.schedule(t, deadline);
    }

    // Advance to the next non-empty tick and fire its timers; return false if the
    // wheel was empty (nothing scheduled anywhere). A node is unlinked before its
    // callback runs, so the callback may re-schedule it. Folds the emptiness
    // check into the advance: advance() already returns null iff the wheel is
    // empty, so the drain loop needs neither a separate empty() occupancy scan
    // nor a second ABI-boundary crossing per timestep.
    bool advance(Args... args) {
        Timer* t = impl_.advance();
        if (t == nullptr) {
            return false;
        }
        do {
            Timer* next = t->next;
            t->next     = nullptr;
            reinterpret_cast<Callback>(t->on_expire)(t, args...);
            t = next;
        } while (t != nullptr);
        return true;
    }

    // Advance and fire, ignoring whether anything was pending.
    void tick(Args... args) {
        advance(args...);
    }

private:
    WheelImpl impl_;
};

} // namespace clotho
