// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Wheel.h"

#include <bit>
#include <cassert>

namespace clotho {

void WheelImpl::schedule(Timer& t, uint64_t deadline) {
    assert(deadline > now_ && "deadline must be in the future");

    if (deadline - now_ < slots) {
        // Due within this revolution: hash straight into the slot, push-front.
        const uint64_t s = slot_of(deadline);
        t.next           = slots_[s];
        slots_[s]        = &t;
        set_occupied(s);
        return;
    }

    // Further out: park in the overflow heap, keyed by deadline.
    overflow_.push({ deadline, &t });
}

uint64_t WheelImpl::nearest_offset() const {
    const uint64_t c    = now_ & mask;
    const uint64_t cw   = c >> 6;
    const uint64_t cb   = c & 63;
    const uint64_t base = cw * 64; // bucket of bit 0 of the current word

    // Hot path: bits strictly above the current bucket in its own word (the
    // current bucket itself is always empty). One mask, one find-first-set.
    uint64_t word = *cur_word_ & (~0ull << cb) & ~(1ull << cb);
    if (word != 0) {
        return base + static_cast<uint64_t>(std::countr_zero(word)) - c;
    }

    // Following words, in circular order.
    for (uint64_t n = 1; n < nwords; ++n) {
        const uint64_t wi = (cw + n) & (nwords - 1);
        if (occupied_[wi] != 0) {
            const uint64_t s = wi * 64 + static_cast<uint64_t>(std::countr_zero(occupied_[wi]));
            return (s - c) & mask;
        }
    }

    // Wrap back into the current word's low bits (below the current bucket).
    word = *cur_word_ & ((1ull << cb) - 1);
    if (word != 0) {
        const uint64_t s = base + static_cast<uint64_t>(std::countr_zero(word));
        return (s - c) & mask;
    }

    return 0;
}

Timer* WheelImpl::advance() {
    const uint64_t k = nearest_offset();
    if (k != 0) {
        // Nearest wheel timer is always sooner than any overflow deadline.
        now_ += k;
    } else if (!overflow_.empty()) {
        // Wheel window empty: skip straight to the earliest overflow deadline.
        now_ = overflow_.top().deadline;
    } else {
        return nullptr;
    }
    cur_word_ = occupied_ + ((now_ & mask) >> 6);

    cascade();

    const uint64_t s    = slot_of(now_);
    Timer*         head = slots_[s];
    slots_[s]           = nullptr;
    clear_occupied(s);

    return head;
}

uint64_t WheelImpl::next_tick() const {
    // The same order advance() takes: a timer in the wheel window is always
    // sooner than any overflow deadline.
    if (const uint64_t k = nearest_offset(); k != 0) {
        return now_ + k;
    }
    if (!overflow_.empty()) {
        return overflow_.top().deadline;
    }
    return never;
}

void WheelImpl::skip_to(uint64_t tick) {
    assert(tick >= now_ && tick < next_tick() && "a timer is due before the tick the clock skips to");

    // No timer fires, so every one in a slot stays where schedule() hashed it:
    // each deadline is above `tick` and below its old now_ + slots, hence below
    // tick + slots, and the slot of `tick` itself stays empty. Only the overflow
    // heap can hold timers the wheel window has now reached.
    now_      = tick;
    cur_word_ = occupied_ + ((now_ & mask) >> 6);
    cascade();
}

void WheelImpl::cascade() {
    // Everything now reachable within one revolution moves out of overflow into
    // its slot. The heap yields the earliest deadlines first.
    const uint64_t horizon = now_ + (slots - 1);
    while (!overflow_.empty() && overflow_.top().deadline <= horizon) {
        const Overflow e = overflow_.top();
        overflow_.pop();

        const uint64_t s = slot_of(e.deadline);
        e.timer->next    = slots_[s];
        slots_[s]        = e.timer;
        set_occupied(s);
    }
}

} // namespace clotho
