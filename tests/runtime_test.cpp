#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/Runtime.h"

namespace {

// Shared state threaded through rt_wheel_tick as the opaque context, so the
// non-capturing C callback can reach test bookkeeping and the wheel itself.
struct Context {
    rt_wheel*        wheel = nullptr;
    std::vector<int> order;
};

// An awaiter-like object embedding an intrusive rt_timer, mirroring how a design
// would own its timer node.
struct Awaiter {
    rt_timer timer{};
    int      id       = 0;
    uint64_t fired_at = 0;
};

// Recover the owning Awaiter from its embedded timer node.
Awaiter& owner(rt_timer* t) {
    return *reinterpret_cast<Awaiter*>(reinterpret_cast<char*>(t) - offsetof(Awaiter, timer));
}

// Log the fire order and the tick each timer fired at.
void record(rt_timer* t, void* ctx) {
    auto&    c = *static_cast<Context*>(ctx);
    Awaiter& a = owner(t);
    a.fired_at = rt_wheel_now(c.wheel);
    c.order.push_back(a.id);
}

} // namespace

TEST_CASE("the runtime wheel jumps over empty ticks and fires its timers in order") {
    rt_wheel w;
    rt_wheel_init(&w);
    Context c{ .wheel = &w };
    Awaiter a{ .id = 1 };
    Awaiter b{ .id = 2 };

    rt_wheel_schedule(&w, &a.timer, 5, &record);
    rt_wheel_schedule(&w, &b.timer, 2, &record);
    CHECK(rt_wheel_now(&w) == 0);

    rt_wheel_tick(&w, &c); // tick 1 is empty -> jump straight to 2
    CHECK(rt_wheel_now(&w) == 2);
    REQUIRE(c.order.size() == 1);
    CHECK(c.order[0] == 2);

    rt_wheel_tick(&w, &c); // jump to 5
    CHECK(rt_wheel_now(&w) == 5);
    REQUIRE(c.order.size() == 2);
    CHECK(c.order[1] == 1);
    CHECK(a.fired_at == 5);
    CHECK(b.fired_at == 2);

    rt_wheel_deinit(&w);
}

TEST_CASE("a runtime-wheel callback may re-arm its own timer across a deadline beyond one revolution") {
    rt_wheel w;
    rt_wheel_init(&w);
    Context c{ .wheel = &w };
    Awaiter a{ .id = 7 };

    // A deadline several revolutions out exercises the overflow heap and cascade.
    rt_wheel_schedule(&w, &a.timer, 4 * 256 + 3, &record);
    rt_wheel_tick(&w, &c);

    CHECK(rt_wheel_now(&w) == 4 * 256 + 3);
    REQUIRE(c.order.size() == 1);
    CHECK(c.order[0] == 7);
    CHECK(a.fired_at == 4 * 256 + 3);

    rt_wheel_deinit(&w);
}

TEST_CASE("rt_wheel_tick on an idle runtime wheel is a no-op") {
    rt_wheel w;
    rt_wheel_init(&w);
    Context c{ .wheel = &w };

    rt_wheel_tick(&w, &c);
    CHECK(rt_wheel_now(&w) == 0);
    CHECK(c.order.empty());

    rt_wheel_deinit(&w);
}
