#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"
#include "ir/SimConfig.h"
#include "runtime/Gc.h"
#include "runtime/Runtime.h"

using namespace clotho;

// std::weak_reference (LRM 8.30).
//
// What a weak reference does while its referent is held is the same at every
// collection cadence, and those cases are compared across all three, as the
// collector's own tests are (tests/gc_test.cpp). When a referent that nothing else
// holds is cleared is the collector's to choose (8.30.3's note), so those cases say
// what each cadence must do: never clears nothing, and a cadence that collects
// clears what it can no longer reach otherwise.
//
// Every design creates its referents inside a function and drops them there. A
// handle that lived in an initial block's own frame can stay in that frame as a
// stale word, which the collector reads conservatively and honours, and the case
// would then be about that word.

namespace {

namespace fs = std::filesystem;

struct Result {
    std::string out;
    rt_gc_stats stats = {};
};

// Lower `source`, run it in this process and return what it printed with the
// collector's counters.
Result run(std::string_view source, const ir::SimulationConfig& config) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));
    ir::run_jit(*r.module, nullptr, config);
    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    Result result;
    std::rewind(tmp);
    char   buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) {
        result.out.append(buf, n);
    }
    std::fclose(tmp);
    rt_gc_get_stats(&result.stats);
    return result;
}

// The diagnostics of a design that must not lower, at the language version named.
// A version is an option of the driver, which reads its source from a file.
std::string refused(std::string_view source, std::string_view std_arg) {
    const fs::path path = fs::temp_directory_path() / (test::detail::unique_stem() + ".sv");
    std::ofstream(path) << source;
    const std::string      file   = path.string();
    const std::string      std_op = "--std=" + std::string(std_arg);
    const std::string_view args[] = { std_op, file };
    const LowerResult      r      = lower_sources(args);
    std::error_code        ec;
    fs::remove(path, ec);
    CHECK(!r.ok);
    return r.diagnostic_text();
}

// The three cadences of tests/gc_test.cpp: a budget no run can spend, a budget of
// one block, and a collection wherever one may run.
ir::SimulationConfig never() {
    ir::SimulationConfig cfg;
    cfg.gc_nursery_bytes = UINT64_MAX;
    return cfg;
}

ir::SimulationConfig ordinary(uint32_t full_every = 0) {
    ir::SimulationConfig cfg;
    cfg.gc_nursery_bytes = RT_GC_BLOCK_BYTES;
    cfg.gc_full_every    = full_every;
    return cfg;
}

ir::SimulationConfig stress() {
    ir::SimulationConfig cfg = ordinary();
    cfg.gc_stress            = true;
    return cfg;
}

// A design whose output no collection may change.
void check_cadences(std::string_view source, const std::string& expected) {
    const Result quiet = run(source, never());
    CHECK(quiet.out == expected);
    CHECK(quiet.stats.collections == 0u);
    CHECK(quiet.stats.weak_cleared == 0u);
    CHECK(run(source, ordinary()).out == expected);
    const Result busy = run(source, stress());
    CHECK(busy.out == expected);
    CHECK(busy.stats.collections > 0u);
}

// The decimal number after `label` in `text`, or -1.
long number_after(const std::string& text, std::string_view label) {
    const size_t at = text.find(label);
    if (at == std::string::npos) {
        return -1;
    }
    return std::strtol(text.c_str() + at + label.size(), nullptr, 10);
}

} // namespace

TEST_CASE("weak_reference: new, get, clear and get_id (LRM 8.30.2 to 8.30.5)") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; string tag; endclass
          class D extends C; int extra; endclass
          typedef std::weak_reference #(C) cref;
          typedef std::weak_reference #(D) dref;
          C c, as_base; D d;
          cref w1, w2, of_null;
          longint id;
          initial begin
            int bad;
            bad = 0;
            c = new; c.v = 5; c.tag = "five";
            w1 = new(c); w2 = new(c); of_null = new(null);
            if (w1 == w2) bad++;
            if (w1.get() != w2.get()) bad++;
            if (w1.get() != c) bad++;
            as_base = w1.get();
            if (as_base.v != 5 || as_base.tag != "five") bad++;
            if (of_null.get() != null) bad++;
            w1.clear();
            if (w1.get() != null) bad++;
            if (w2.get() != c) bad++;
            w1.clear();
            if (w1.get() != null) bad++;
            of_null.clear();
            if (of_null.get() != null) bad++;
            d = new; as_base = d;
            if (cref::get_id(null) != 0) bad++;
            id = cref::get_id(c);
            if (id == 0 || id != cref::get_id(c) || id != w2.get_id(c)) bad++;
            if (cref::get_id(as_base) != dref::get_id(d)) bad++;
            if (cref::get_id(as_base) == id || dref::get_id(d) == 0) bad++;
            $display("api %0d", bad);
          end
        endmodule
    )";
    check_cadences(src, "api 0\n");
}

TEST_CASE("weak_reference: in objects and containers, through calls, copied and derived from") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; endclass
          typedef std::weak_reference #(C) cref;
          class Holder; cref w; cref q[$]; cref by_name[string]; endclass
          class Counted extends std::weak_reference #(C);
            int uses;
            function new(C c, int start); super.new(c); uses = start; endfunction
            function int value();
              C c;
              c = get();
              uses++;
              return c == null ? -1 : c.v;
            endfunction
          endclass
          function automatic cref pass(cref w); return w; endfunction
          C c; Holder h; cref made, copy, as_base; Counted counted;
          initial begin
            int bad;
            bad = 0;
            c = new; c.v = 9;
            h = new;
            h.w = new(c);
            made = new(c);
            h.q.push_back(made);
            h.by_name["a"] = pass(h.w);
            if (h.q[0].get() != c || h.by_name["a"].get() != c || h.by_name["a"] != h.w) bad++;
            copy = new h.w;
            if (copy == h.w || copy.get() != c) bad++;
            copy.clear();
            if (h.w.get() != c || copy.get() != null) bad++;
            counted = new(c, 3);
            if (counted.value() != 9 || counted.uses != 4 || counted.get() != c) bad++;
            as_base = counted;
            if (as_base.get() != c) bad++;
            counted.clear();
            if (counted.value() != -1 || as_base.get() != null || counted.uses != 5) bad++;
            $display("shapes %0d", bad);
          end
        endmodule
    )";
    check_cadences(src, "shapes 0\n");
}

// A referent something still holds is never cleared, whatever holds it: a variable,
// another object, a container, or a second referent's property. Each weak reference
// has a twin, and the two answer alike at every look (8.30.1: every weak reference
// to a referent is cleared in one step).
TEST_CASE("weak_reference: a referent that is still held is never cleared") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; string tag; C next; endclass
          class Junk; int v; string tag; endclass
          typedef std::weak_reference #(C) cref;
          C direct; C in_queue[$]; C by_key[int]; C chain;
          cref w[4], twin[4];
          Junk sink;
          int bad;
          function automatic void make();
            C c;
            for (int i = 0; i < 4; i++) begin
              c = new; c.v = 10 + i; c.tag = "held";
              w[i] = new(c); twin[i] = new(c);
              case (i)
                0: direct = c;
                1: in_queue.push_back(c);
                2: by_key[7] = c;
                default: begin chain = new; chain.next = c; end
              endcase
            end
          endfunction
          function automatic void churn();
            for (int i = 0; i < 1500; i++) begin
              sink = new; sink.v = 32'h5A5A5A5A; sink.tag = "junk";
            end
          endfunction
          function automatic void look();
            C a;
            for (int i = 0; i < 4; i++) begin
              a = w[i].get();
              if (a == null || a.v != 10 + i || a.tag != "held" || twin[i].get() != a) bad++;
            end
          endfunction
          initial begin
            bad = 0;
            make();
            repeat (6) begin churn(); look(); #1; look(); end
            $display("held %0d", bad);
          end
        endmodule
    )";
    check_cadences(src, "held 0\n");
}

// The referents nothing holds go; the ones something holds stay; and the held ones
// go in their turn once dropped. Half of a hundred referents are kept in an array.
//
// A young collection frees only what was allocated since the last collection, so the
// kept half, which has survived several, is cleared only by a full collection.
TEST_CASE("weak_reference: an unheld referent is cleared by the collection that finds it so") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; string tag; endclass
          class Junk; int v; string tag; endclass
          typedef std::weak_reference #(C) cref;
          cref w[100], twin[100];
          C keep[100];
          Junk sink;
          function automatic void make();
            C c;
            for (int i = 0; i < 100; i++) begin
              c = new; c.v = i; c.tag = "ref";
              w[i] = new(c); twin[i] = new(c);
              if (i % 2 == 0) keep[i] = c;
            end
          endfunction
          function automatic void drop();
            for (int i = 0; i < 100; i++) keep[i] = null;
          endfunction
          function automatic void churn();
            for (int i = 0; i < 1500; i++) begin
              sink = new; sink.v = 32'h5A5A5A5A; sink.tag = "junk";
            end
          endfunction
          function automatic void tally(input string label, input bit kept);
            C a, b;
            int cleared, bad;
            cleared = 0; bad = 0;
            for (int i = 0; i < 100; i++) begin
              a = w[i].get(); b = twin[i].get();
              if (a != b) bad++;
              if (a != null && (a.v != i || a.tag != "ref")) bad++;
              if (kept && i % 2 == 0 && a != keep[i]) bad++;
              if (a == null) cleared++;
            end
            $display("%s %0d bad %0d", label, cleared, bad);
          endfunction
          initial begin
            make();
            repeat (6) begin churn(); #1; end
            tally("first", 1);
            drop();
            repeat (6) begin churn(); #1; end
            tally("second", 0);
          end
        endmodule
    )";
    SUBCASE("never: nothing is cleared") {
        const Result r = run(src, never());
        CHECK(r.out == "first 0 bad 0\nsecond 0 bad 0\n");
        CHECK(r.stats.weak_cleared == 0u);
    }
    SUBCASE("stress: the unheld half, then the rest") {
        const Result r = run(src, stress());
        CHECK(r.out == "first 50 bad 0\nsecond 100 bad 0\n");
        CHECK(r.stats.weak_cleared == 200u);
    }
    SUBCASE("every collection full") {
        const Result r = run(src, ordinary(1));
        CHECK(number_after(r.out, "first ") >= 45);
        CHECK(number_after(r.out, "first ") <= 50);
        CHECK(number_after(r.out, "second ") >= 95);
        CHECK(r.out.find("first") != std::string::npos);
        CHECK(r.out.find("bad 0\nsecond") != std::string::npos);
        CHECK(r.out.ends_with("bad 0\n"));
    }
    SUBCASE("young collections leave an old referent alone, and it stays whole") {
        // One full collection is always first; every later one here is young. The
        // kept half is old by the time it is dropped, so nothing after that frees
        // it, and get() goes on returning objects whose fields are intact.
        const Result r     = run(src, ordinary(1u << 24));
        const long   first = number_after(r.out, "first ");
        CHECK(first >= 45);
        CHECK(first <= 50);
        CHECK(number_after(r.out, "second ") == first);
        CHECK(r.out.ends_with("bad 0\n"));
        CHECK(r.stats.young_collections > 0u);
    }
}

// A weak reference is itself an ordinary object (8.30.1): dropped, it is collected
// whether or not its referent lives on, and the referent is none the worse for it.
TEST_CASE("weak_reference: a dropped weak reference is collected while its referent lives") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; string tag; endclass
          typedef std::weak_reference #(C) cref;
          C held; cref last;
          function automatic void spin();
            cref w;
            for (int i = 0; i < 4000; i++) begin
              w = new(held);
              if (w.get() != held) held.v = -1;
            end
            last = w;
          endfunction
          initial begin
            held = new; held.v = 21; held.tag = "held";
            repeat (4) begin spin(); #1; end
            $display("%0d %s %0d", held.v, held.tag, last.get() == held);
          end
        endmodule
    )";
    check_cadences(src, "21 held 1\n");
    const Result r = run(src, stress());
    CHECK(r.stats.objects_allocated >= 16001u);
    CHECK(r.stats.objects_live < 100u);
    CHECK(r.stats.weak_cleared == 0u);
}

// clear() is a store like any other, so a wait that reads get() sees it at once
// (8.30.3), as does one whose weak reference variable is pointed elsewhere. A shallow
// copy is a weak reference of its own, with its own waiters, and so is an object of
// a class derived from weak_reference.
TEST_CASE("weak_reference: wait and @ on get() wake when the reference is cleared by clear()") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; endclass
          typedef std::weak_reference #(C) cref;
          class Named extends std::weak_reference #(C);
            string name;
            function new(C c, string name); super.new(c); this.name = name; endfunction
            task watch();
              wait (get() == null);
              $display("%s %0t", name, $time);
            endtask
          endclass
          C c;
          cref a, b, other, cleared, copy;
          Named named;
          initial begin
            c = new;
            a = new(c); b = new(c); other = new(c); cleared = new(null);
            copy = new a;
            named = new(c, "derived");
            fork
              begin wait (a.get() == null); $display("wait %0t", $time); end
              begin @(b.get()); $display("event %0t", $time); end
              begin wait (other.get() == null); $display("retargeted %0t", $time); end
              begin wait (copy.get() == null); $display("copy %0t", $time); end
              named.watch();
            join_none
            #3 a.clear();
            #1 b.clear();
            #1 a.clear();
            #2 other = cleared;
            #2 copy.clear();
            #2 named.clear();
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    check_cadences(src, "wait 3\nevent 4\nretargeted 7\ncopy 9\nderived 11\ndone 12\n");
}

// The collector clearing a weak reference wakes the same waits (8.30.3: "either by
// the user or by the memory management system"). The wake comes in the time step
// after the collection, so the times are the cadence's and only the order is fixed.
TEST_CASE("weak_reference: a wait on get() wakes when the collector clears the reference") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; endclass
          class Junk; int v; string tag; endclass
          typedef std::weak_reference #(C) cref;
          class Watcher;
            cref w;
            task run();
              wait (w.get() == null);
              $display("method woke");
            endtask
          endclass
          cref w;
          Watcher watcher;
          Junk sink;
          function automatic void make();
            C c;
            c = new; c.v = 3;
            w = new(c);
            watcher = new;
            watcher.w = new(c);
          endfunction
          function automatic void churn();
            for (int i = 0; i < 1500; i++) begin
              sink = new; sink.v = 32'h5A5A5A5A; sink.tag = "junk";
            end
          endfunction
          initial begin
            make();
            fork
              begin wait (w.get() == null); $display("wait woke"); end
              begin @(w.get()); $display("event woke"); end
              watcher.run();
            join_none
            repeat (8) begin churn(); #1; end
            $display("end %0d", w.get() == null);
          end
        endmodule
    )";
    SUBCASE("never: the referent is never found unheld, so nothing wakes") {
        const Result r = run(src, never());
        CHECK(r.out == "end 0\n");
    }
    // Which waiter prints first is the order the collector reached the two weak
    // references in, which nothing fixes.
    const auto all_woke = [](const std::string& out) {
        return out.size() == std::string_view("wait woke\nevent woke\nmethod woke\nend 1\n").size() &&
               out.find("wait woke\n") != std::string::npos && out.find("event woke\n") != std::string::npos &&
               out.find("method woke\n") != std::string::npos && out.ends_with("end 1\n");
    };
    SUBCASE("stress") {
        const Result r = run(src, stress());
        CHECK_MESSAGE(all_woke(r.out), r.out);
        CHECK(r.stats.weak_cleared == 2u);
    }
    SUBCASE("ordinary") {
        const Result r = run(src, ordinary(1));
        CHECK_MESSAGE(all_woke(r.out), r.out);
        CHECK(r.stats.weak_cleared == 2u);
    }
}

// A collection run from inside an allocation is in the middle of a statement, and
// keeps every referent: a weak reference changes only between time steps, so two
// reads in one time step agree whatever was allocated between them. The referent
// here is unheld from the start, and the next time step finds it gone.
TEST_CASE("weak_reference: a weak reference does not change within a time step") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; endclass
          class Junk; int v; string tag; endclass
          typedef std::weak_reference #(C) cref;
          cref w;
          Junk sink;
          int changed;
          function automatic void make();
            C c;
            c = new; c.v = 3;
            w = new(c);
          endfunction
          function automatic void churn();
            for (int i = 0; i < 3000; i++) begin
              sink = new; sink.v = 32'h5A5A5A5A; sink.tag = "junk";
              if (w.get() == null) changed++;
            end
          endfunction
          initial begin
            changed = 0;
            make();
            churn();
            $display("within %0d", changed);
            repeat (4) #1;
            $display("after %0d", w.get() == null);
          end
        endmodule
    )";
    SUBCASE("never") {
        CHECK(run(src, never()).out == "within 0\nafter 0\n");
    }
    SUBCASE("ordinary") {
        // 3000 objects are more than four budgets, so collections run inside churn().
        // Whether one is due at a safepoint afterwards depends on where the last of
        // those fell, so only the first line is fixed here.
        const Result r = run(src, ordinary(1));
        CHECK(r.out.starts_with("within 0\n"));
        CHECK(r.stats.emergency_collections > 0u);
    }
    SUBCASE("stress") {
        const Result r = run(src, stress());
        CHECK(r.out == "within 0\nafter 1\n");
        CHECK(r.stats.emergency_collections >= 3000u);
        CHECK(r.stats.weak_cleared == 1u);
    }
}

// The scheduler edge that finds the wheel idle ends the run, and collects nothing:
// what a collection there cleared would have no time step to be seen in. One time
// step under stress is one collection, at the edge between time 0 and time 1.
TEST_CASE("weak_reference: the edge that ends the run is not a collection") {
    constexpr std::string_view src = "module m; initial #1 $display(\"x\"); endmodule";
    const Result               r   = run(src, stress());
    CHECK(r.out == "x\n");
    CHECK(r.stats.collections == 1u);
}

TEST_CASE("weak_reference: what slang refuses stays refused") {
    constexpr std::string_view of_int = "module m; std::weak_reference #(int) w; initial w = null; endmodule";
    // LRM 8.30.1: "The parameter type T shall be a class type".
    CHECK(refused(of_int, "1800-2023").find("is not a class type") != std::string::npos);
    // The class is new in IEEE 1800-2023, and slang offers it from that version on.
    constexpr std::string_view of_class =
      "module m; class C; endclass std::weak_reference #(C) w; initial w = null; endmodule";
    CHECK(refused(of_class, "1800-2017").find("1800-2017") != std::string::npos);
}
