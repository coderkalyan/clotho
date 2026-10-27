#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/Wheel.h"

using namespace clotho;

namespace {

struct Context;
// The wheel forwards a shared Context& to every fire callback, so non-capturing
// function pointers can still reach test state and the wheel itself.
using TestWheel = Wheel<Context&>;

struct Context {
    TestWheel*       wheel = nullptr;
    std::vector<int> order;
    int              rearm_count = 0;
};

// An awaiter-like object embedding an intrusive Timer.
struct Awaiter {
    Timer    timer;
    int      id       = 0;
    uint64_t deadline = 0;
    uint64_t fired_at = 0;
    bool     fired    = false;
};

// Recover the owning Awaiter from its embedded Timer node.
Awaiter& owner(Timer* t) {
    return *reinterpret_cast<Awaiter*>(reinterpret_cast<char*>(t) - offsetof(Awaiter, timer));
}

// Mark the owner fired and log the order it fired in.
void record(Timer* t, Context& c) {
    Awaiter& a = owner(t);
    a.fired    = true;
    a.fired_at = c.wheel->now();
    c.order.push_back(a.id);
}

// Re-arm self three ticks out, up to three times.
void rearm(Timer* t, Context& c) {
    c.order.push_back(static_cast<int>(c.wheel->now()));
    if (++c.rearm_count < 3) {
        c.wheel->schedule(*t, c.wheel->now() + 3, &rearm);
    }
}

} // namespace

TEST_CASE("tick skips empty ticks to the next non-empty time, firing in order") {
    TestWheel w;
    Context   c{ .wheel = &w };
    Awaiter   a{ .id = 1 };
    Awaiter   b{ .id = 2 };
    Awaiter   d{ .id = 3 };

    w.schedule(a.timer, 5, &record);
    w.schedule(b.timer, 2, &record);
    w.schedule(d.timer, 5, &record);

    w.tick(c); // tick 1 is empty -> jump straight to 2
    CHECK(w.now() == 2);
    REQUIRE(c.order.size() == 1);
    CHECK(c.order[0] == 2);

    w.tick(c); // jump to 5, fire both timers there
    CHECK(w.now() == 5);
    REQUIRE(c.order.size() == 3);
    CHECK(b.fired_at == 2);
    CHECK(a.fired_at == 5);
    CHECK(d.fired_at == 5);
}

TEST_CASE("deadlines beyond one revolution overflow and cascade back in") {
    TestWheel      w;
    Context        c{ .wheel = &w };
    const uint64_t far  = TestWheel::slots + 10; // > one revolution: overflows
    const uint64_t near = TestWheel::slots - 1;  // last slot of the revolution
    Awaiter        a{ .id = 1 };
    Awaiter        b{ .id = 2 };
    w.schedule(a.timer, far, &record);
    w.schedule(b.timer, near, &record);

    w.tick(c);
    CHECK(w.now() == near);
    REQUIRE(c.order.size() == 1);
    CHECK(c.order[0] == 2);
    CHECK_FALSE(a.fired); // far one cascaded into the wheel but not yet due

    w.tick(c);
    CHECK(w.now() == far);
    REQUIRE(c.order.size() == 2);
    CHECK(c.order[1] == 1);
    CHECK(a.fired_at == far);
}

TEST_CASE("an empty wheel jumps straight to the earliest overflow deadline") {
    TestWheel      w;
    Context        c{ .wheel = &w };
    Awaiter        a{ .id = 7 };
    const uint64_t d = TestWheel::slots * 4 + 3;
    w.schedule(a.timer, d, &record);

    w.tick(c);
    CHECK(w.now() == d);
    REQUIRE(c.order.size() == 1);
    CHECK(c.order[0] == 7);
}

TEST_CASE("tick on an idle wheel is a no-op") {
    TestWheel w;
    Context   c{ .wheel = &w };
    w.tick(c);
    CHECK(w.now() == 0);
    CHECK(c.order.empty());
}

TEST_CASE("a callback may re-arm its own timer") {
    TestWheel w;
    Context   c{ .wheel = &w };
    Awaiter   a{ .id = 1 };
    w.schedule(a.timer, 3, &rearm);

    while (w.now() < 9) {
        w.tick(c);
    }

    REQUIRE(c.order.size() == 3);
    CHECK(c.order[0] == 3); // fired at ticks 3, 6, 9
    CHECK(c.order[1] == 6);
    CHECK(c.order[2] == 9);
}

TEST_CASE("nearest-slot search wraps around the slot array") {
    TestWheel w;
    Context   c{ .wheel = &w };

    // Move now_ near the top of the revolution, into a high bitfield word.
    Awaiter seed{ .id = 0 };
    w.schedule(seed.timer, TestWheel::slots - 3, &record);
    w.tick(c);
    CHECK(w.now() == TestWheel::slots - 3);

    // Schedule at the far end of the window: its bucket index sits *below* the
    // current bucket, so the search must wrap the full revolution to find it.
    Awaiter        wrapped{ .id = 9 };
    const uint64_t deadline = (TestWheel::slots - 3) + (TestWheel::slots - 1);
    w.schedule(wrapped.timer, deadline, &record);

    w.tick(c);
    CHECK(w.now() == deadline);
    REQUIRE(c.order.size() == 2);
    CHECK(c.order[1] == 9);
}

TEST_CASE("a crowd of timers across wheel and overflow each fire once on time") {
    TestWheel            w;
    Context              c{ .wheel = &w };
    std::vector<Awaiter> awaiters(200);

    for (int i = 0; i < 200; ++i) {
        awaiters[i].id       = i;
        awaiters[i].deadline = 1 + static_cast<uint64_t>(i) * 7; // distinct, spanning overflow
        w.schedule(awaiters[i].timer, awaiters[i].deadline, &record);
    }

    // Each distinct deadline is its own non-empty tick: 200 jumps fire all 200.
    for (int n = 0; n < 200; ++n) {
        w.tick(c);
        CHECK(static_cast<size_t>(n + 1) == c.order.size());
    }

    CHECK(w.now() == 1 + 199 * 7);
    for (const Awaiter& a : awaiters) {
        CHECK(a.fired);
        CHECK(a.fired_at == a.deadline);
    }
}

TEST_CASE("next_tick names the earliest timer without moving the wheel") {
    TestWheel w;
    CHECK(w.next_tick() == TestWheel::never);

    // Only in the overflow heap: more than one revolution out.
    Awaiter overflow{ .id = 1 };
    w.schedule(overflow.timer, TestWheel::slots * 3 + 1, &record);
    CHECK(w.next_tick() == TestWheel::slots * 3 + 1);

    // A timer inside the wheel window is sooner than anything in overflow.
    Awaiter in_window{ .id = 2 };
    w.schedule(in_window.timer, 17, &record);
    CHECK(w.next_tick() == 17);
    CHECK(w.now() == 0);
    CHECK_FALSE(in_window.fired);
}

TEST_CASE("next_tick follows the wheel as it fires timers") {
    TestWheel w;
    Context   c{ .wheel = &w };
    Awaiter   a{ .id = 1 };
    Awaiter   b{ .id = 2 };
    w.schedule(a.timer, 4, &record);
    w.schedule(b.timer, 9, &record);

    CHECK(w.next_tick() == 4);
    w.tick(c);
    CHECK(w.next_tick() == 9);
    w.tick(c);
    CHECK(w.next_tick() == TestWheel::never);
}

TEST_CASE("skip_to moves the clock without firing, and later timers still fire on their ticks") {
    TestWheel w;
    Context   c{ .wheel = &w };
    Awaiter   soon{ .id = 1 };
    Awaiter   later{ .id = 2 };
    w.schedule(soon.timer, 10, &record);
    w.schedule(later.timer, 30, &record);

    w.skip_to(7);
    CHECK(w.now() == 7);
    CHECK(c.order.empty());
    CHECK(w.next_tick() == 10);

    // A timer scheduled from the new now() is relative to it.
    Awaiter fresh{ .id = 3 };
    w.schedule(fresh.timer, w.now() + 2, &record);

    w.tick(c);
    CHECK(w.now() == 9);
    w.tick(c);
    CHECK(w.now() == 10);
    w.tick(c);
    CHECK(w.now() == 30);
    REQUIRE(c.order.size() == 3);
    CHECK(c.order[0] == 3);
    CHECK(c.order[1] == 1);
    CHECK(c.order[2] == 2);
    CHECK(fresh.fired_at == 9);
    CHECK(soon.fired_at == 10);
    CHECK(later.fired_at == 30);
}

TEST_CASE("skip_to to the current tick moves nothing") {
    TestWheel w;
    Awaiter   a{ .id = 1 };
    w.schedule(a.timer, 5, &record);
    w.skip_to(0);
    CHECK(w.now() == 0);
    CHECK(w.next_tick() == 5);
}

TEST_CASE("skip_to brings overflow timers the clock has come within a revolution of into the wheel") {
    TestWheel      w;
    Context        c{ .wheel = &w };
    Awaiter        far{ .id = 1 };
    const uint64_t deadline = TestWheel::slots * 2 + 5; // overflow: over a revolution out
    w.schedule(far.timer, deadline, &record);

    // Skipped to within one revolution of it, the deadline is a wheel timer.
    w.skip_to(TestWheel::slots + 10);
    CHECK(w.next_tick() == deadline);

    // A closer timer from here fires first, and the far one still fires on its tick.
    Awaiter near{ .id = 2 };
    w.schedule(near.timer, w.now() + 3, &record);
    w.tick(c);
    CHECK(w.now() == TestWheel::slots + 13);
    w.tick(c);
    CHECK(w.now() == deadline);
    REQUIRE(c.order.size() == 2);
    CHECK(c.order[0] == 2);
    CHECK(c.order[1] == 1);
}
