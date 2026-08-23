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

// -- 4-state ordering (LRM 7.12.2 under 11.4.4's unknown rule) ---------------
//
// LRM 11.4.4 makes a relational compare with an unknown operand yield x, which
// is not an ordering at all -- feeding it to a comparison sort is an INCONSISTENT
// comparator, not merely an unspecified result. Clotho therefore defines a total
// order (docs/design/array-methods.md): an element or key carrying ANY x or z bit
// is "unknown", all unknowns are mutually equal, and unknown precedes every
// defined value ascending (so it follows every defined value descending). The
// sort is stable, which is what makes the position of mutually-equal unknowns
// deterministic rather than merely legal.
//
// Oracles: verilator 5.050 accepts these but fabricates a value -- it maps an
// unknown to 0 and sorts it as 0; iverilog 13 does not implement sort() at all.

// With no element unknown, a 4-state element orders exactly as the 2-state one
// of the same shape: the comparator reads the slot's must plane, and a signed
// 4-state element still compares arithmetically.
TEST_CASE("4-state element sorts numerically") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0]        a[$];
            logic signed [7:0] s[$];
            initial begin
                a.push_back(8'd3); a.push_back(8'hff); a.push_back(8'd1); a.push_back(8'h80);
                a.sort();  $write("a:"); foreach (a[i]) $write(" %02x", a[i]); $display;
                a.rsort(); $write("r:"); foreach (a[i]) $write(" %02x", a[i]); $display;
                s.push_back(-8'sd2); s.push_back(8'sd5); s.push_back(-8'sd10); s.push_back(8'sd0);
                s.sort();  $write("s:"); foreach (s[i]) $write(" %0d", s[i]); $display;
                s.rsort(); $write("d:"); foreach (s[i]) $write(" %0d", s[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a: 01 03 80 ff\nr: ff 80 03 01\n"
                              "s: -10 -2 0 5\nd: 5 0 -2 -10\n");
}

// The shape that motivated 4-state ordering, transcribed: Accellera UVM
// `uvm_root.svh:827` sorts a queue of `uvm_cmdline_set_verbosity` HANDLES with
// `m_time_settings.sort() with ( item.offset )`, and that property is declared
// `time offset` (`uvm_cmdline_report.svh:179`) -- a 64-bit FOUR-STATE type, so
// the whole call used to be refused even with no unknown in sight. Two settings
// share an offset, so the tie pins stability at the full 64-bit width, and one
// carries an unknown offset (what an unparsed `+uvm_set_verbosity` leaves
// behind), which now has a defined place instead of a refusal.
TEST_CASE("sort with a `time` key (the UVM m_time_settings shape)") {
    constexpr std::string_view src = R"(
        module m;
            class uvm_cmdline_set_verbosity;
                string id;
                time   offset;
            endclass
            uvm_cmdline_set_verbosity m_time_settings[$];
            initial begin
                uvm_cmdline_set_verbosity s;
                s = new; s.id = "c"; s.offset = 64'd30;          m_time_settings.push_back(s);
                s = new; s.id = "a"; s.offset = 64'd10;          m_time_settings.push_back(s);
                s = new; s.id = "w"; s.offset = 64'hFFFF_FFFF_1; m_time_settings.push_back(s);
                s = new; s.id = "x"; s.offset = 64'bx;           m_time_settings.push_back(s);
                s = new; s.id = "b"; s.offset = 64'd10;          m_time_settings.push_back(s);
                if (m_time_settings.size() > 0) begin
                    m_time_settings.sort() with ( item.offset );
                end
                $write("asc:");
                foreach (m_time_settings[i]) $write(" %s=%0d", m_time_settings[i].id,
                                                               m_time_settings[i].offset);
                $display;
                m_time_settings.rsort() with ( item.offset );
                $write("desc:");
                foreach (m_time_settings[i]) $write(" %s", m_time_settings[i].id);
                $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "asc: x=x a=10 b=10 c=30 w=68719476721\ndesc: w c a b x\n");
}

// Placement: an unknown is below every defined value, so it sorts first ascending
// and last descending. The descending direction is ordered directly rather than
// by reversing an ascending sort -- a reverse would put the unknown first there
// too, and would flip the unknown tie group besides.
TEST_CASE("an unknown sorts first ascending and last descending") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a[$];
            initial begin
                a.push_back(8'd3); a.push_back(8'hxx); a.push_back(8'd1);
                a.sort();  $write("asc:");  foreach (a[i]) $write(" %h", a[i]); $display;
                a.rsort(); $write("desc:"); foreach (a[i]) $write(" %h", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "asc: xx 01 03\ndesc: 03 01 xx\n");
}

// x and z are NOT distinguished: every unknown compares equal to every other one,
// so the three unknown elements here are one tie group and stability is what
// decides their order -- input order, in both directions. An implementation that
// ranked z below x, or that ordered unknowns by their defined bits, would reorder
// them.
TEST_CASE("x and z are the same to the ordering, and stability keeps their order") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a[$];
            initial begin
                a.push_back(8'hzz); a.push_back(8'd7); a.push_back(8'hxx);
                a.push_back(8'hxz); a.push_back(8'd2);
                a.sort();  $write("asc:");  foreach (a[i]) $write(" %h", a[i]); $display;
                a.rsort(); $write("desc:"); foreach (a[i]) $write(" %h", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "asc: zz xx xz 02 07\ndesc: 07 02 zz xx xz\n");
}

// A value with SOME unknown bits is unknown wholesale, not sorted by the bits it
// does know. This is the case an implementation that compared the value (must)
// plane and only checked for an all-unknown word gets wrong: it would order
// `8'b0000_000x` next to `8'b0000_0000` and put `8'b1111_111x` last, instead of
// tying the two partial unknowns and putting both ahead of the defined value.
TEST_CASE("a partly unknown value is unknown wholesale") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] p[$];
            initial begin
                p.push_back(8'b0000_0000);
                p.push_back(8'b1111_111x);
                p.push_back(8'b0000_000x);
                p.sort();  $write("asc:");  foreach (p[i]) $write(" %b", p[i]); $display;
                p.rsort(); $write("desc:"); foreach (p[i]) $write(" %b", p[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "asc: 1111111x 0000000x 00000000\n"
                              "desc: 00000000 1111111x 0000000x\n");
}

// What decides is the COMPARED value's domain, not the container's. A 4-state key
// over a 2-state element hoists the element whose key is unknown; a 2-state key
// over an element that carries x/z in a field the key does not read hoists
// nothing, because no key is ever unknown.
TEST_CASE("4-state ordering follows the key, not the element") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { logic [7:0] v; int k; } rec;
            int         e[$];
            logic [3:0] key[$];
            rec         c[$];
            initial begin
                key.push_back(4'd2); key.push_back(4'bxx01); key.push_back(4'd1);
                e.push_back(0); e.push_back(1); e.push_back(2);
                e.sort() with ( key[item] );
                $write("k4:"); foreach (e[i]) $write(" %0d", e[i]); $display;
                c.push_back('{ v: 8'hxx, k: 2 });
                c.push_back('{ v: 8'h01, k: 1 });
                c.push_back('{ v: 8'hzz, k: 0 });
                c.sort() with ( item.k );
                $write("k2:"); foreach (c[i]) $write(" %h", c[i].v); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "k4: 1 2 0\nk2: zz 01 xx\n");
}

// A 4-state element wider than the 64-bit word the runtime comparator loads goes
// through the IR permutation sort, and needed nothing extra: the unknown test is
// one reduction of the whole unknown plane, at any width. The third value is
// unknown in its LOW half only, so it ties with the all-x one -- the any-bit rule
// holding across the limb boundary.
TEST_CASE("a 4-state element wider than 64 bits") {
    constexpr std::string_view src = R"(
        module m;
            logic [95:0] w[$];
            initial begin
                w.push_back(96'h5);
                w.push_back({96{1'bx}});
                w.push_back({48'd1, 48'bx});
                w.push_back(96'h1);
                w.sort();  $write("asc:");  foreach (w[i]) $write(" %h", w[i]); $display;
                w.rsort(); $write("desc:"); foreach (w[i]) $write(" %h", w[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "asc: xxxxxxxxxxxxxxxxxxxxxxxx 000000000001xxxxxxxxxxxx"
          " 000000000000000000000001 000000000000000000000005\n"
          "desc: 000000000000000000000005 000000000000000000000001"
          " xxxxxxxxxxxxxxxxxxxxxxxx 000000000001xxxxxxxxxxxx\n");
}

// The runtime comparator's plane decode is width arithmetic -- it reads
// `(2W+7)/8` bytes of the slot and masks W bits out of each half -- so the widths
// where that arithmetic changes shape are the ones worth pinning: 1 (both planes
// in one byte), 33 (the halves straddle a 32-bit boundary) and 64 (the mask is
// all ones and the packed word is a full 128 bits, which is `time`'s width).
// Also the only case here on a DYNAMIC ARRAY rather than a queue, since the two
// receivers reach the comparator through different entry points.
TEST_CASE("4-state ordering at the plane-decode width boundaries") {
    constexpr std::string_view src = R"(
        module m;
            logic        w1[$];
            logic [32:0] w33[$];
            logic [63:0] w64[];
            initial begin
                w1.push_back(1'b1); w1.push_back(1'bx); w1.push_back(1'b0); w1.push_back(1'bz);
                w1.sort(); $write("w1:"); foreach (w1[i]) $write(" %b", w1[i]); $display;
                w33.push_back(33'h1_0000_0000); w33.push_back({1'bx, 32'h0}); w33.push_back(33'h1);
                w33.sort(); $write("w33:"); foreach (w33[i]) $write(" %h", w33[i]); $display;
                w64 = new[3];
                w64[0] = 64'hFFFF_FFFF_FFFF_FFFF;
                w64[1] = {63'h0, 1'bz};
                w64[2] = 64'h2;
                w64.sort();  $write("w64:"); foreach (w64[i]) $write(" %h", w64[i]); $display;
                w64.rsort(); $write("r64:"); foreach (w64[i]) $write(" %h", w64[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "w1: x z 0 1\n"
                              "w33: x00000000 000000001 100000000\n"
                              "w64: 000000000000000Z 0000000000000002 ffffffffffffffff\n"
                              "r64: ffffffffffffffff 0000000000000002 000000000000000Z\n");
}

// Every element unknown means every pair a tie, so a stable sort is the identity
// -- in both directions. This is the whole ordering reduced to its degenerate
// case, and it is what makes the result reproducible rather than merely legal.
TEST_CASE("an all-unknown array keeps its order") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a[$];
            initial begin
                a.push_back(8'hxz); a.push_back(8'hzx); a.push_back(8'hx0);
                a.sort();  $write("asc:");  foreach (a[i]) $write(" %h", a[i]); $display;
                a.rsort(); $write("desc:"); foreach (a[i]) $write(" %h", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "asc: xz zx x0\ndesc: xz zx x0\n");
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

// What clotho still cannot order, refused loudly. A non-comparable element or
// key is 7.12.2's own restriction ("the relational operators shall be defined
// for..."), which slang's binder enforces ahead of lowering -- pinned here so
// the pair of them cannot regress into a silent accept. A 4-state element or key
// is not among these: it is ordered, by the total order the cases above pin.
// Neither is a fixed-size receiver, which 7.12.2 admits in as many words
// ("fixed or dynamically sized") and which the cases below order.
TEST_CASE("ordering shapes Clotho still refuses loudly") {
    SUBCASE("a multidimensional element has no relational operators") {
        // The elements of `int a[2][3]` are its sub-arrays, so a bare sort has
        // nothing defined to compare -- 7.12.2 again, on a fixed receiver.
        const LowerResult r = lower_text(R"(
            module m;
                int a[2][3];
                initial begin a[0][0] = 1; a.sort(); end
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("unpacked arrays of comparable values") != std::string::npos);
    }
    SUBCASE("an element with no relational operators") {
        const LowerResult r = lower_text(R"(
            module m;
                typedef struct { int a; int b; } pair;
                pair q[$];
                initial begin q.push_back('{1, 2}); q.sort(); end
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("unpacked arrays of comparable values") != std::string::npos);
    }
    SUBCASE("a `with` clause key with no relational operators") {
        const LowerResult r = lower_text(R"(
            module m;
                typedef struct { int a; } inner;
                typedef struct { inner nest; int k; } outer;
                outer q[$];
                initial q.sort with ( item.nest );
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("unpacked arrays of comparable values") != std::string::npos);
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

// A fixed-size unpacked-array receiver, which 7.12.2 admits explicitly:
// "Array ordering methods reorder the elements of any unpacked array (fixed or
// dynamically sized) except for associative arrays." All four run over a mirror
// dynamic array (lower_fixed_array_order), so a fixed receiver reaches the SAME
// implementation of each method a dynamic array does rather than a second copy
// of any of it -- which the reuse cases further down pin by result, not by
// assertion. The string case is the clause's own reverse example, and the struct
// case is its `struct { byte red, green, blue; } c [512]` keyed sort, which was
// written for a fixed receiver in the first place.
TEST_CASE("ordering methods on a fixed-size unpacked array (LRM 7.12.2)") {
    constexpr std::string_view src = R"(
        module m;
            int a[5];
            string s[3];
            struct { byte red, green, blue; } c [4];
            initial begin
                a[0]=3; a[1]=7; a[2]=1; a[3]=7; a[4]=9;
                a.sort();    $write("sort:");  foreach (a[k]) $write(" %0d", a[k]); $display;
                a.rsort();   $write("rsort:"); foreach (a[k]) $write(" %0d", a[k]); $display;
                a.reverse(); $write("rev:");   foreach (a[k]) $write(" %0d", a[k]); $display;
                s[0]="hello"; s[1]="sad"; s[2]="world";
                s.reverse(); $display("s.rev: %s %s %s", s[0], s[1], s[2]);
                s.sort();    $display("s.sort: %s %s %s", s[0], s[1], s[2]);
                c[0]='{3,1,1}; c[1]='{1,2,2}; c[2]='{4,3,3}; c[3]='{2,4,4};
                c.sort with ( item.red );
                $display("c.red: %0d %0d %0d %0d", c[0].red, c[1].red, c[2].red, c[3].red);
                c.reverse();
                $display("c.rev: %0d %0d %0d %0d", c[0].red, c[1].red, c[2].red, c[3].red);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sort: 1 3 7 7 9\n"
                              "rsort: 9 7 7 3 1\n"
                              "rev: 1 3 7 7 9\n"
                              "s.rev: world sad hello\n"
                              "s.sort: hello sad world\n"
                              "c.red: 1 2 3 4\n"
                              "c.rev: 4 3 2 1\n");
}

// A fixed array is reordered in DECLARED-ADDRESS order, so `sort` puts the
// smallest element at the declared LEFT bound whichever side of the range it is
// on -- index 3 for `int b[3:7]` and index 7 for the descending `int c[7:3]`.
// Each line below reads the array back left bound first, so an implementation
// that mistook the storage order for the index order would print these reversed.
TEST_CASE("fixed-array ordering permutes in declared-address order (LRM 7.12.2)") {
    constexpr std::string_view src = R"(
        module m;
            int b[3:7];
            int c[7:3];
            initial begin
                b[3]=10; b[4]=40; b[5]=20; b[6]=40; b[7]=30;
                b.sort();    $write("b.sort: ");  for (int k=3;k<=7;k++) $write("%0d ", b[k]); $display;
                b.rsort();   $write("b.rsort: "); for (int k=3;k<=7;k++) $write("%0d ", b[k]); $display;
                b.reverse(); $write("b.rev: ");   for (int k=3;k<=7;k++) $write("%0d ", b[k]); $display;
                c[7]=10; c[6]=40; c[5]=20; c[4]=40; c[3]=30;
                c.sort();    $write("c.sort: ");  for (int k=7;k>=3;k--) $write("%0d ", c[k]); $display;
                c.rsort();   $write("c.rsort: "); for (int k=7;k>=3;k--) $write("%0d ", c[k]); $display;
                c.reverse(); $write("c.rev: ");   for (int k=7;k>=3;k--) $write("%0d ", c[k]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "b.sort: 10 20 30 40 40 \n"
                              "b.rsort: 40 40 30 20 10 \n"
                              "b.rev: 10 20 30 40 40 \n"
                              "c.sort: 10 20 30 40 40 \n"
                              "c.rsort: 40 40 30 20 10 \n"
                              "c.rev: 10 20 30 40 40 \n");
}

// That a fixed receiver reuses the existing machinery rather than duplicating
// it, pinned by results that would differ under any second implementation:
//
//  - the 4-state total order (unknown first ascending, last descending) comes
//    out of the runtime comparator sort_run4 -- the bare integral form still
//    takes the runtime fast path, on the mirror;
//  - the keyed forms are STABLE and cost the same number of clause evaluations
//    as the queue receiver ("sort / rsort with a `with` clause are stable" and
//    "a `with` clause key is evaluated per comparison" pin 12/13/15/10/11/14 and
//    calls=8 for a queue; the same numbers here mean the same merge sort);
//  - `item.index` inside a keyed sort is the element's ORIGINAL declared index,
//    so an rsort by it reverses a non-zero-based array.
TEST_CASE("fixed-array ordering reuses the queue / dynamic-array machinery") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int key; int tag; } kv;
            kv          c[3:8];
            logic [7:0] fs[4];
            int         fa[4];
            int         calls;
            function automatic int key(int v); calls++; return v; endfunction
            initial begin
                fs[0]=8'd3; fs[1]=8'hxx; fs[2]=8'd1; fs[3]=8'd2;
                fs.sort();  $display("4state: %0h %0h %0h %0h", fs[0], fs[1], fs[2], fs[3]);
                fs.rsort(); $display("4rsort: %0h %0h %0h %0h", fs[0], fs[1], fs[2], fs[3]);

                c[3]='{1,10}; c[4]='{1,11}; c[5]='{0,12};
                c[6]='{0,13}; c[7]='{1,14}; c[8]='{0,15};
                c.sort  with ( item.key );
                $write("asc: ");  for (int k=3;k<=8;k++) $write("%0d ", c[k].tag); $display;
                c.rsort with ( item.key );
                $write("desc: "); for (int k=3;k<=8;k++) $write("%0d ", c[k].tag); $display;
                c.rsort with ( item.index );
                $write("byidx: "); for (int k=3;k<=8;k++) $write("%0d ", c[k].tag); $display;

                fa[0]=4; fa[1]=5; fa[2]=3; fa[3]=1;
                calls = 0;
                fa.sort with ( key(item) );
                $write("fa:"); foreach (fa[k]) $write(" %0d", fa[k]);
                $display(" calls=%0d", calls);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4state: xx 1 2 3\n"
                              "4rsort: 3 2 1 xx\n"
                              "asc: 12 13 15 10 11 14 \n"
                              "desc: 10 11 14 12 13 15 \n"
                              "byidx: 15 13 12 14 11 10 \n"
                              "fa: 1 3 4 5 calls=8\n");
}

// A fixed-array shuffle draws the SAME RNG stream as a dynamic-array shuffle,
// because it is the same runtime call (rt_dynarr_shuffle) on a mirror of the same
// length. The permutation below is the one "dynamic array + queue shuffle is a
// stable permutation" already pins for `int a[]` with six elements in the first
// process, and the trailing $urandom shows the stream left at the same point --
// so the two receivers cannot silently diverge into two Fisher-Yates loops.
TEST_CASE("fixed-array shuffle draws the dynamic-array RNG stream") {
    constexpr std::string_view fixed_src = R"(
        module m;
            int fa[6];
            initial begin
                for (int k = 0; k < 6; k++) fa[k] = k;
                fa.shuffle();
                $write("perm:"); foreach (fa[k]) $write(" %0d", fa[k]); $display;
                $display("next=%0d", $urandom);
            end
        endmodule
    )";
    constexpr std::string_view dyn_src   = R"(
        module m;
            int da[];
            initial begin
                da = new[6];
                for (int k = 0; k < 6; k++) da[k] = k;
                da.shuffle();
                $write("perm:"); foreach (da[k]) $write(" %0d", da[k]); $display;
                $display("next=%0d", $urandom);
            end
        endmodule
    )";
    const std::string          fixed_out = run_capture(fixed_src);
    CHECK(fixed_out == "perm: 2 3 1 4 0 5\nnext=88327031\n");
    CHECK(fixed_out == run_capture(dyn_src));
}

// A multidimensional fixed array reorders its SUB-ARRAYS, which reverse and
// shuffle can do because they move whole element slots and need no comparison
// (7.12.2 excludes only associative arrays). A bare sort of the same array is
// refused above, for want of relational operators on a sub-array.
TEST_CASE("multidimensional fixed array reverses its sub-arrays (LRM 7.12.2)") {
    constexpr std::string_view src = R"(
        module m;
            int n [2:3][3];
            initial begin
                n[2][0]=1;  n[2][1]=2;  n[2][2]=3;
                n[3][0]=40; n[3][1]=50; n[3][2]=60;
                n.reverse();
                $display("%0d %0d %0d / %0d %0d %0d",
                         n[2][0], n[2][1], n[2][2], n[3][0], n[3][1], n[3][2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "40 50 60 / 1 2 3\n");
}
