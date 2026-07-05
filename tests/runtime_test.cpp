#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

TEST_CASE("rt_readmem with a null z_base loads values without touching z memory") {
    // Two adjacent 8-bit elements, mm word (2 bytes) each, no z word: stride 2.
    // A z write through a null-z layout would clobber the next element's mm word.
    char path[] = "/tmp/clotho_rt_readmem_noz.hex";
    std::FILE* f = std::fopen(path, "w");
    REQUIRE(f != nullptr);
    std::fputs("a5 3c\n", f);
    std::fclose(f);

    // The filename crosses the ABI as a right-justified packed-ASCII vector
    // (byte 0 = last character).
    std::vector<char> packed;
    for (size_t k = std::strlen(path); k-- > 0;) {
        packed.push_back(path[k]);
    }

    uint8_t mm[4] = { 0xee, 0xee, 0xee, 0xee };
    rt_readmem(packed.data(), static_cast<uint32_t>(packed.size()), mm, nullptr, 2, 2, 8, 0, 1, 0, 0,
               /*flags: hex, 2-state*/ 1);

    // must = may = value for a 2-state load: mm word is val | (val << 8).
    CHECK(mm[0] == 0xa5);
    CHECK(mm[1] == 0xa5);
    CHECK(mm[2] == 0x3c);
    CHECK(mm[3] == 0x3c);
    std::remove(path);
}

TEST_CASE("rt_nba_arr push/commit with a null z_ptr merges only the mm word") {
    uint8_t mm = 0x00;
    uint8_t z_guard = 0x77; // would be corrupted by a stray z write

    const uint8_t val  = 0x0f;
    const uint8_t mask = 0xff;
    rt_nba_arr_push(0, &mm, &val, &mask, 1, nullptr, nullptr, nullptr, 0);
    REQUIRE(rt_nba_arr_pending() == 1);
    rt_nba_arr_commit();
    CHECK(rt_nba_arr_dirty(0) == 1);
    rt_nba_arr_reset();

    CHECK(mm == 0x0f);
    CHECK(z_guard == 0x77);
}
