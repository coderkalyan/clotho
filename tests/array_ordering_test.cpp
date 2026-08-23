#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors queue_test.cpp / dynamic_array_test.cpp).
// Ordering results (LRM 7.12.2 reverse / sort / rsort) are hand-checkable and,
// where iverilog supports the method, oracle-matched.
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    std::rewind(tmp);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);

    return out;
}

} // namespace

// sort() ascending, rsort() descending, reverse() flips order — dynamic array,
// signed `int` element (LRM 7.12.2). reverse of the rsorted vector returns the
// ascending order.
TEST_CASE("dynamic array sort/rsort/reverse (int)") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            initial begin
                a = new[5];
                a[0]=3; a[1]=1; a[2]=4; a[3]=1; a[4]=5;
                a.sort();
                $write("sort:");  foreach (a[i]) $write(" %0d", a[i]); $display;
                a.rsort();
                $write("rsort:"); foreach (a[i]) $write(" %0d", a[i]); $display;
                a.reverse();
                $write("rev:");   foreach (a[i]) $write(" %0d", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sort: 1 1 3 4 5\nrsort: 5 4 3 1 1\nrev: 1 1 3 4 5\n");
}

// A signed element orders arithmetically: negatives sort below zero (the signed
// comparator, not a raw bit-pattern compare).
TEST_CASE("dynamic array sort is signed for a signed element") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            initial begin
                a = new[4];
                a[0]=-2; a[1]=5; a[2]=-10; a[3]=0;
                a.sort();
                foreach (a[i]) $write("%0d ", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-10 -2 0 5 \n");
}

// An unsigned element orders by magnitude: 8'hFF is the largest, not negative.
TEST_CASE("dynamic array sort is unsigned for an unsigned element") {
    constexpr std::string_view src = R"(
        module m;
            bit [7:0] a[];
            initial begin
                a = new[3];
                a[0]=8'hff; a[1]=8'h01; a[2]=8'h80;
                a.sort();
                foreach (a[i]) $write("%02x ", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "01 80 ff \n");
}

// Real elements order by IEEE value (the real comparator).
TEST_CASE("dynamic array sort (real)") {
    constexpr std::string_view src = R"(
        module m;
            real a[];
            initial begin
                a = new[4];
                a[0]=2.5; a[1]=-1.5; a[2]=0.0; a[3]=10.25;
                a.sort();
                foreach (a[i]) $write("%g ", a[i]); $display;
                a.rsort();
                foreach (a[i]) $write("%g ", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-1.5 0 2.5 10.25 \n10.25 2.5 0 -1.5 \n");
}

// Queue ordering: reverse / sort / rsort operate on the ring's logical order.
TEST_CASE("queue sort/rsort/reverse (int)") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            initial begin
                q.push_back(3); q.push_back(1); q.push_back(4);
                q.push_back(1); q.push_back(5);
                q.sort();
                $write("sort:");  foreach (q[i]) $write(" %0d", q[i]); $display;
                q.rsort();
                $write("rsort:"); foreach (q[i]) $write(" %0d", q[i]); $display;
                q.reverse();
                $write("rev:");   foreach (q[i]) $write(" %0d", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sort: 1 1 3 4 5\nrsort: 5 4 3 1 1\nrev: 1 1 3 4 5\n");
}

// A queue whose head has wrapped (push_front then push_back) still sorts by
// logical order — the ring's head offset is honoured.
TEST_CASE("queue sort after push_front (wrapped head)") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            initial begin
                q.push_back(2); q.push_front(7); q.push_back(1); q.push_front(9);
                q.sort();
                foreach (q[i]) $write("%0d ", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 7 9 \n");
}

// shuffle() produces a permutation (LRM 7.12.2): sorting the shuffled array
// recovers the original, and the specific order is the process RNG's stable
// stream (seed = process index, like $urandom). Both the exact
// permutation and the sort-recovers invariant are pinned.
TEST_CASE("dynamic array + queue shuffle is a stable permutation") {
    constexpr std::string_view src = R"(
        module m;
            int a[]; int q[$]; int i;
            initial begin
                a = new[6]; foreach (a[i]) a[i] = i;
                a.shuffle();
                $write("sh:"); foreach (a[i]) $write(" %0d", a[i]); $display;
                a.sort();
                $write("re:"); foreach (a[i]) $write(" %0d", a[i]); $display;
                for (i = 0; i < 5; i++) q.push_back(i);
                q.shuffle();
                $write("q:"); foreach (q[i]) $write(" %0d", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sh: 2 3 1 4 0 5\nre: 0 1 2 3 4 5\nq: 0 4 1 2 3\n");
}

// shuffle draws from the frame RNG shared with $urandom: the process becomes a
// coroutine and the RNG state persists across activations (each posedge draws a
// fresh permutation) while the multiset is preserved (sum invariant).
TEST_CASE("shuffle in an always block keeps a per-process RNG") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0; int a[]; int i, s;
            initial begin
                a = new[5]; foreach (a[i]) a[i] = i + 1;
                repeat (4) #5 clk = ~clk;
                $finish;
            end
            always @(posedge clk) begin
                a.shuffle();
                s = 0; foreach (a[i]) s += a[i];
                $display("sum=%0d a0=%0d", s, a[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sum=15 a0=5\nsum=15 a0=3\n");
}

// shuffle() draws from the caller's RNG, which the call ABI threads into a
// subroutine body -- so a shuffle inside a task is the same permutation the
// caller would have produced inline (compare against the pinned first
// permutation above, which the same process index seeds).
TEST_CASE("shuffle inside a task uses the caller's RNG") {
    constexpr std::string_view src = R"(
        module m;
            int a[]; int i;
            task automatic mix(ref int v[]); v.shuffle(); endtask
            initial begin
                a = new[6]; foreach (a[i]) a[i] = i;
                mix(a);
                $write("sh:"); foreach (a[i]) $write(" %0d", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sh: 2 3 1 4 0 5\n");
}

// Ordering an empty or single-element container is a no-op (no crash).
TEST_CASE("array ordering on empty / single element is a no-op") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            int q[$];
            initial begin
                a.sort(); a.rsort(); a.reverse();
                q.sort(); q.rsort(); q.reverse();
                a = new[1]; a[0]=42;
                a.sort(); a.reverse();
                q.push_back(7); q.rsort();
                $display("%0d %0d", a[0], q[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "42 7\n");
}

// The `with` clause on sort / rsort (LRM 7.12.2). The clause's own two examples,
// on a queue receiver: `c.sort with (item.red)` and the renamed-iterator
// `c.sort(x) with ({x.blue, x.green})`. The struct-field sort doubles as a
// stability check -- the two red==1 elements keep their original relative order,
// which is why the greens come out 2 then 1.
TEST_CASE("sort with a `with` clause (LRM 7.12.2 examples)") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { byte red, green, blue; } rgb;
            rgb c[$];
            initial begin
                c.push_back('{ red: 3, green: 9, blue: 1 });
                c.push_back('{ red: 1, green: 2, blue: 1 });
                c.push_back('{ red: 2, green: 5, blue: 0 });
                c.push_back('{ red: 1, green: 1, blue: 2 });
                c.sort with ( item.red );
                $write("red:");  foreach (c[i]) $write(" %0d.%0d", c[i].red, c[i].green); $display;
                c.sort( x ) with ( {x.blue, x.green} );
                $write("blgr:"); foreach (c[i]) $write(" %0d.%0d", c[i].blue, c[i].green); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "red: 1.2 1.1 2.5 3.9\nblgr: 0.5 1.2 1.9 2.1\n");
}

// The UVM shape that motivated the clause: a queue of class handles ordered by a
// string a METHOD CALL returns (uvm_phase_hopper.svh:763,
// `succ_q.sort with (item.get_full_name())`). The key is neither the element nor
// a field of it, and evaluating it allocates.
TEST_CASE("sort with a string key returned by a method call") {
    constexpr std::string_view src = R"(
        module m;
            class phase;
                string nm;
                function new(string n); nm = n; endfunction
                function string get_full_name(); return nm; endfunction
            endclass
            phase succ_q[$];
            phase p;
            initial begin
                p = new("uvm.run");     succ_q.push_back(p);
                p = new("uvm.build");   succ_q.push_back(p);
                p = new("uvm.connect"); succ_q.push_back(p);
                succ_q.sort with ( item.get_full_name() );
                foreach (succ_q[i]) $write(" %s", succ_q[i].get_full_name()); $display;
                succ_q.rsort with ( item.get_full_name() );
                foreach (succ_q[i]) $write(" %s", succ_q[i].get_full_name()); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == " uvm.build uvm.connect uvm.run\n uvm.run uvm.connect uvm.build\n");
}

// An int, a real and a string key, each on a queue whose elements carry a
// distinguishing tag, so the assertion sees the permutation and not just the keys.
TEST_CASE("sort with an int / real / string key") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int tag; int i; real r; string s; } rec;
            rec q[$];
            initial begin
                q.push_back('{ tag: 0, i: 3, r: 2.5,  s: "c" });
                q.push_back('{ tag: 1, i: 1, r: -1.5, s: "a" });
                q.push_back('{ tag: 2, i: 2, r: 9.0,  s: "b" });
                q.sort  with ( item.i ); $write("i: ");  foreach (q[k]) $write("%0d", q[k].tag); $display;
                q.sort  with ( item.r ); $write("r: ");  foreach (q[k]) $write("%0d", q[k].tag); $display;
                q.sort  with ( item.s ); $write("s: ");  foreach (q[k]) $write("%0d", q[k].tag); $display;
                q.rsort with ( item.i ); $write("ri:");  foreach (q[k]) $write("%0d", q[k].tag); $display;
                q.rsort with ( item.r ); $write("rr:");  foreach (q[k]) $write("%0d", q[k].tag); $display;
                q.rsort with ( item.s ); $write("rs:");  foreach (q[k]) $write("%0d", q[k].tag); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "i: 120\nr: 102\ns: 120\nri:021\nrr:201\nrs:021\n");
}

// Stability, in BOTH directions, with keys that tie and elements that do not.
// The rsort line is the one that catches an "ascending then reverse_run"
// implementation: reversing an ascending sort reverses each tie GROUP too, so it
// would answer "14 11 10 15 13 12" instead of keeping 10 11 14 / 12 13 15.
TEST_CASE("sort / rsort with a `with` clause are stable") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int key; int tag; } kv;
            kv q[$];
            task automatic fill();
                q.delete();
                q.push_back('{ key: 1, tag: 10 });
                q.push_back('{ key: 1, tag: 11 });
                q.push_back('{ key: 0, tag: 12 });
                q.push_back('{ key: 0, tag: 13 });
                q.push_back('{ key: 1, tag: 14 });
                q.push_back('{ key: 0, tag: 15 });
            endtask
            initial begin
                fill(); q.sort  with ( item.key );
                $write("asc: ");  foreach (q[i]) $write("%0d ", q[i].tag); $display;
                fill(); q.rsort with ( item.key );
                $write("desc: "); foreach (q[i]) $write("%0d ", q[i].tag); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "asc: 12 13 15 10 11 14 \ndesc: 10 11 14 12 13 15 \n");
}

// A bare sort() over a string element, which 7.12.2 admits because `<` / `>` /
// `==` are defined for a string. The clause's own string example uses reverse; the
// sorted and rsorted orders are the extension. iverilog cannot even parse
// `s.sort()` on a string array; verilator agrees with these lines.
TEST_CASE("sort / rsort a string element") {
    constexpr std::string_view src = R"(
        module m;
            string s[] = '{ "hello", "sad", "world" };
            string q[$];
            initial begin
                s.reverse; $write("rev: ");   foreach (s[i]) $write("%s ", s[i]); $display;
                s.sort;    $write("sort: ");  foreach (s[i]) $write("%s ", s[i]); $display;
                s.rsort;   $write("rsort: "); foreach (s[i]) $write("%s ", s[i]); $display;
                q.push_back("b"); q.push_back(""); q.push_back("ab"); q.push_back("b");
                q.sort;    $write("q: '");    foreach (q[i]) $write("%s'", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "rev: world sad hello \nsort: hello sad world \nrsort: world sad hello \nq: ''ab'b'b'\n");
}

// A bare sort() over an element wider than the 64-bit word the runtime's flat
// comparator loads, signed and unsigned. The unsigned case straddles the limb
// boundary (1<<64 must outrank every 64-bit value), and the signed case puts a
// negative value -- all-ones in the top limb -- below every positive one.
TEST_CASE("sort an element wider than 64 bits") {
    constexpr std::string_view src = R"(
        module m;
            bit [127:0]       u[$];
            bit signed [95:0] s[$];
            initial begin
                u.push_back(128'h2);
                u.push_back(128'h1_0000_0000_0000_0000);
                u.push_back(128'h1);
                u.push_back(128'hFFFF_FFFF_FFFF_FFFF);
                u.sort;
                $write("u:"); foreach (u[i]) $write(" %0d", u[i][71:64]); $display;
                s.push_back(96'sd5);
                s.push_back(-96'sd5);
                s.push_back(96'sd0);
                s.sort;
                $write("s:"); foreach (s[i]) $write(" %0d", s[i]); $display;
                s.rsort;
                $write("r:"); foreach (s[i]) $write(" %0d", s[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "u: 0 0 0 1\ns: -5 0 5\nr: 5 0 -5\n");
}

// The key is evaluated once per element per COMPARISON, not once per element into
// a key buffer. This pins that choice: a side-effecting clause observes the
// per-comparison count, so switching to precomputed keys later has to change this
// number deliberately rather than silently.
TEST_CASE("a `with` clause key is evaluated per comparison") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int calls;
            function automatic int key(int v); calls++; return v; endfunction
            initial begin
                q.push_back(4); q.push_back(5); q.push_back(3); q.push_back(1);
                calls = 0;
                q.sort with ( key(item) );
                $write("q:"); foreach (q[i]) $write(" %0d", q[i]);
                $display(" calls=%0d", calls);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "q: 1 3 4 5 calls=8\n");
}

// A key expression that ALLOCATES on every evaluation, over enough elements and
// comparisons to run the collector. The regression this pins is the one a runtime
// comparator calling back into generated code would have had: no raw pointer into
// the element buffer is held across the clause, because every element access
// re-resolves through elem_select. Class objects are collected
// (docs/design/garbage-collection.md) while a container payload is malloc'd
// (Gc.h:333), so neither can move under the sort.
TEST_CASE("sort with an allocating key survives collection") {
    constexpr std::string_view src = R"(
        module m;
            class node;
                int id;
                function new(int i); id = i; endfunction
                function string name(); return $sformatf("n%03d", 400 - id); endfunction
            endclass
            node q[$];
            node n;
            int  i;
            initial begin
                for (i = 0; i < 200; i++) begin
                    n = new(i);
                    q.push_back(n);
                end
                q.sort with ( item.name() );
                $display("%s %s %0d", q[0].name(), q[199].name(), q.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n201 n400 200\n");
}

// Empty and single-element receivers, every keyed form. Nothing can be compared,
// so the clause never runs -- and nothing crashes on a zero-length permutation.
TEST_CASE("keyed ordering on empty / single element is a no-op") {
    constexpr std::string_view src = R"(
        module m;
            int    q[$];
            int    a[];
            string s[];
            int    calls;
            function automatic int key(int v); calls++; return v; endfunction
            initial begin
                calls = 0;
                q.sort with (key(item)); q.rsort with (key(item));
                a.sort with (key(item)); a.rsort with (key(item));
                s.sort; s.rsort;
                q.push_back(7); a = new[1]; a[0] = 9; s = new[1]; s[0] = "z";
                q.sort with (key(item)); q.rsort with (key(item));
                a.sort with (key(item)); a.rsort with (key(item));
                s.sort; s.rsort;
                $display("%0d %0d %s calls=%0d", q[0], a[0], s[0], calls);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "7 9 z calls=0\n");
}

// What 7.12.2 does not let clotho simulate, refused loudly rather than ordered by
// an invented rule. 4-state is the deliberate one: a 4-state `<` yields x, not a
// total order, so there is no defined place for an unknown.
TEST_CASE("ordering shapes Clotho still refuses loudly") {
    SUBCASE("4-state element") {
        const LowerResult r = lower_text(R"(
            module m;
                logic [7:0] q[$];
                initial begin q.push_back(1); q.sort(); end
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("4-state element") != std::string::npos);
        CHECK(r.diagnostic_text().find("ordering of x and z undefined") != std::string::npos);
    }
    SUBCASE("4-state `with` clause key") {
        const LowerResult r = lower_text(R"(
            module m;
                int         q[$];
                logic [3:0] k[$];
                initial begin q.push_back(1); q.sort with ( k[item] ); end
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("4-state `with` clause key") != std::string::npos);
    }
    SUBCASE("4-state enum element") {
        const LowerResult r = lower_text(R"(
            module m;
                typedef enum logic [1:0] { A = 0, B = 1 } e4;
                e4 q[$];
                initial begin q.push_back(B); q.sort(); end
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("ordering of x and z undefined") != std::string::npos);
    }
}

// "Specifying a with clause shall be a compiler error" for reverse() and
// shuffle() (LRM 7.12.2). slang's binder enforces it ahead of lowering, so the
// requirement is met upstream -- pinned here so it cannot regress into clotho
// silently accepting and ignoring a clause.
TEST_CASE("a `with` clause on reverse / shuffle is a compiler error") {
    SUBCASE("reverse") {
        const LowerResult r = lower_text(R"(
            module m;
                int q[$];
                initial q.reverse with ( item );
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("cannot use 'with' expression with 'reverse'") != std::string::npos);
    }
    SUBCASE("shuffle") {
        const LowerResult r = lower_text(R"(
            module m;
                int q[$];
                initial q.shuffle with ( item );
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("cannot use 'with' expression with 'shuffle'") != std::string::npos);
    }
}

// An enum key orders by its base value (an enum is integral, so 7.12.2's
// relational operators are defined for it), and `item.index` inside a clause
// reads the element's ORIGINAL position -- the sort permutes an index vector, so
// the array it indexes has not moved yet.
TEST_CASE("sort with an enum key and with item.index") {
    constexpr std::string_view src = R"(
        module m;
            typedef enum int { LOW = 1, MID = 5, HIGH = 9 } lvl;
            lvl e[$];
            int q[$];
            initial begin
                e.push_back(HIGH); e.push_back(LOW); e.push_back(MID);
                e.sort with ( item );
                $write("e:"); foreach (e[i]) $write(" %0d", e[i]); $display;
                q.push_back(50); q.push_back(20); q.push_back(90);
                q.rsort with ( item.index );
                $write("q:"); foreach (q[i]) $write(" %0d", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "e: 1 5 9\nq: 90 20 50\n");
}

// A key expression that MUTATES the receiver. 7.12.2 is silent, so clotho defines
// it rather than corrupting memory: the element count is snapshotted at entry (the
// rule foreach uses for its bound), so the sort orders exactly the elements that
// were there and anything the clause pushes stays past them. Nothing dangles,
// because every access re-resolves through the handle.
TEST_CASE("a `with` clause that mutates the receiver is bounded, not corrupting") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            function automatic int grow(int v); q.push_back(99); return v; endfunction
            initial begin
                q.push_back(5); q.push_back(3); q.push_back(8); q.push_back(1);
                q.sort with ( grow(item) );
                $write("n=%0d:", q.size()); foreach (q[i]) $write(" %0d", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=14: 1 3 5 8 99 99 99 99 99 99 99 99 99 99\n");
}
