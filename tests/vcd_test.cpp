#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// A fresh dump path per run so cases (which ctest may run in parallel processes)
// never collide.
std::string unique_path() {
    // generic_string(): the result is spliced into a $dumpfile() string
    // literal, where a Windows path's backslashes would read as escapes.
    return (std::filesystem::temp_directory_path() / (test::detail::unique_stem() + ".vcd"))
      .generic_string();
}

// JIT-run a design and return the VCD it wrote. The source must open its dump with
// $dumpfile("OUT.vcd"); the token is rewritten to a unique path. stdout ($display)
// is discarded so a design's prints do not pollute the test log.
std::string run_vcd(std::string source) {
    const std::string path = unique_path();
    const auto        tok  = source.find("OUT.vcd");
    REQUIRE(tok != std::string::npos);
    source.replace(tok, std::string_view("OUT.vcd").size(), path);

    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    std::FILE* null = std::fopen("/dev/null", "w");
    if (null != nullptr) {
        dup2(fileno(null), fileno(stdout));
    }

    ir::run_jit(*r.module);

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    if (null != nullptr) {
        std::fclose(null);
    }

    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::remove(path.c_str());
    return ss.str();
}

// The value-change body: everything after $enddefinitions.
std::string body_of(const std::string& vcd) {
    const auto pos = vcd.find("$enddefinitions");
    REQUIRE(pos != std::string::npos);
    return vcd.substr(vcd.find('\n', pos) + 1);
}

bool contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("vcd: scalar + vector value changes, with no-change suppression") {
    // A clock and a counter. Registration order (clk, count, next) fixes the
    // identifier codes !/"/#. $finish at #38 lands between edges, so the trace ends
    // cleanly at the #35 posedge with no same-time race at the boundary.
    const std::string src = R"(
        module top;
          reg clk = 0;
          reg [3:0] count = 0;
          wire [3:0] next = count + 1;
          always #5 clk = ~clk;
          always @(posedge clk) count <= next;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #38 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));

    // The full trace is deterministic. Note the compressed vectors (b100, not
    // b0100) and that #10/#20/#30 carry only the clock edge — count/next did not
    // change, so they are not re-emitted (LRM §21.7: one settled value per time).
    const std::string expected =
      "#0\n0!\nb0 \"\nb1 #\n"
      "#5\n1!\nb1 \"\nb10 #\n"
      "#10\n0!\n"
      "#15\n1!\nb10 \"\nb11 #\n"
      "#20\n0!\n"
      "#25\n1!\nb11 \"\nb100 #\n"
      "#30\n0!\n"
      "#35\n1!\nb100 \"\nb101 #\n";
    CHECK(body == expected);
}

TEST_CASE("vcd: header declares scopes, vars, and a timescale") {
    const std::string src = R"(
        `timescale 1ns/1ps
        module top;
          reg a = 0;
          reg [7:0] bus = 0;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #1 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    CHECK(contains(vcd, "$timescale\n\t1ns\n$end"));
    CHECK(contains(vcd, "$scope module top $end"));
    CHECK(contains(vcd, "$var reg 1 ! a $end"));
    CHECK(contains(vcd, "$var reg 8 \" bus $end"));
    CHECK(contains(vcd, "$upscope $end"));
    CHECK(contains(vcd, "$enddefinitions $end"));
}

TEST_CASE("vcd: x and z render per bit") {
    // r is an uninitialized reg (x); w is an undriven 4-state net (z, LRM 6.6).
    const std::string src = R"(
        module top;
          reg [1:0] r;
          wire [1:0] w;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #1 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    // 2-bit all-x compresses to "x", all-z to "z".
    CHECK(contains(body, "bx "));
    CHECK(contains(body, "bz "));
}

TEST_CASE("vcd: nested instance hierarchy nests $scope/$upscope") {
    const std::string src = R"(
        module sub(input wire clk, output reg [2:0] q);
          initial q = 0;
          always @(posedge clk) q <= q + 1;
        endmodule
        module top;
          reg clk = 0;
          wire [2:0] q;
          sub u1(.clk(clk), .q(q));
          always #5 clk = ~clk;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #12 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    // top scope contains a nested u1 scope, each closed by its own $upscope.
    const auto top    = vcd.find("$scope module top $end");
    const auto u1     = vcd.find("$scope module u1 $end");
    const auto enddef = vcd.find("$enddefinitions");
    REQUIRE(top != std::string::npos);
    REQUIRE(u1 != std::string::npos);
    CHECK(top < u1);
    CHECK(u1 < enddef);
    CHECK(contains(vcd, "$var reg 3 "));   // u1.q, a 3-bit reg
}

TEST_CASE("vcd: $dumpoff writes an all-x checkpoint, $dumpon resumes") {
    const std::string src = R"(
        module top;
          reg [1:0] r;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 r = 2'b10;
            #5 $dumpoff;
            #5 $dumpon;
            #5 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    // #0 x, #5 the assignment, #10 the dumpoff all-x checkpoint, #15 the dumpon
    // re-dump of the current value.
    const std::string expected = "#0\nbx !\n#5\nb10 !\n#10\nbx !\n#15\nb10 !\n";
    CHECK(body == expected);
}

TEST_CASE("vcd: unpacked-array memory dumps one $var per element") {
    const std::string src = R"(
        module top;
          reg [7:0] mem [0:3];
          integer i;
          initial begin
            for (i = 0; i < 4; i = i + 1) mem[i] = 0;
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 mem[2] = 8'hAB;
            #5 $finish;
          end
        endmodule
    )";
    const std::string vcd  = run_vcd(src);
    const std::string body = body_of(vcd);
    // Each element is its own $var named mem[<addr>] over the declared [0:3] range.
    CHECK(contains(vcd, "mem[0] $end"));
    CHECK(contains(vcd, "mem[1] $end"));
    CHECK(contains(vcd, "mem[2] $end"));
    CHECK(contains(vcd, "mem[3] $end"));
    // Only mem[2] changes at #5, to 0xAB; the other elements are not re-emitted.
    CHECK(contains(body, "#5\nb10101011 "));
}

TEST_CASE("vcd: nonblocking array-element writes are dumped per element") {
    // mem[idx] <= ... each cycle: the touched element must re-emit on its write
    // cycle only (array writes are instrumented, not always-scanned).
    const std::string src = R"(
        module top;
          reg clk = 0;
          reg [7:0] mem [0:3];
          reg [1:0] idx = 0;
          integer i;
          initial for (i = 0; i < 4; i = i + 1) mem[i] = 0;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            mem[idx] <= mem[idx] + 8'h11;
            idx <= idx + 1;
          end
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #45 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    // mem[0] is written at the first posedge (#5) -> 0x11; mem[1] at #15, etc.
    CHECK(contains(body, "#5\n"));
    CHECK(contains(body, "b10001 "));   // 0x11 on the written element
    // Count 0x11 (b10001) emissions: one per element as each is written once.
    size_t n = 0;
    for (size_t p = body.find("b10001 "); p != std::string::npos; p = body.find("b10001 ", p + 1)) {
        ++n;
    }
    CHECK(n == 4);   // mem[0..3] each written once over the run
}

TEST_CASE("vcd: descending array range names elements by declared address") {
    const std::string src = R"(
        module top;
          reg [3:0] m [3:1];
          initial begin
            m[3] = 0; m[2] = 0; m[1] = 0;
            $dumpfile("OUT.vcd");
            $dumpvars;
            #1 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    CHECK(contains(vcd, "m[3] $end"));
    CHECK(contains(vcd, "m[2] $end"));
    CHECK(contains(vcd, "m[1] $end"));
}

TEST_CASE("vcd: signals wider than 64 bits dump as full multi-word vectors") {
    const std::string src = R"(
        module top;
          reg [95:0] w = 0;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 w = 96'hDEAD_BEEF_1234_5678_9ABC_DEF0;
            #5 $finish;
          end
        endmodule
    )";
    const std::string vcd  = run_vcd(src);
    const std::string body = body_of(vcd);
    CHECK(contains(vcd, "$var reg 96 "));   // declared at full width in the header
    // 0xDEADBEEF123456789ABCDEF0 as 96 binary bits, MSB first, no leading-zero loss
    // (the MSB is 1, so the full width prints).
    CHECK(contains(body, "b110111101010110110111110111011110001001000110100010101100111100"
                         "010011010101111001101111011110000 "));
}

TEST_CASE("vcd: wide four-state value renders x then the settled bits") {
    const std::string src = R"(
        module top;
          reg [70:0] w;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 w = 71'h5A;
            #5 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    CHECK(contains(body, "#0\nbx "));         // uninitialized 71-bit reg is all-x
    CHECK(contains(body, "#5\nb1011010 "));   // 0x5A, leading zeros dropped
}

TEST_CASE("vcd: high-activity trace stays ordered through the writer thread") {
    // Enough instants to exceed the writer's handoff threshold many times over, so
    // the background writer processes multiple buffers. A race would corrupt the
    // record order; assert the time markers come out strictly increasing and the
    // final value is correct.
    const std::string src = R"(
        module top;
          reg [15:0] a = 0;
          always #1 a = a + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #2000 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));

    long prev  = -1;
    long times = 0;
    for (size_t i = 0; i < body.size();) {
        if (body[i] == '#') {
            const long t = std::strtol(body.c_str() + i + 1, nullptr, 10);
            CHECK(t > prev);   // strictly increasing: no reordering across the thread
            prev = t;
            ++times;
        }
        i = body.find('\n', i);
        if (i == std::string::npos) {
            break;
        }
        ++i;
    }
    // One instant per tick 0..2000: the always block fires at t=2000 (a -> 2000)
    // before $finish, and that final value is dumped at #2000 — matches iverilog 13
    // (both emit 2001 markers ending b11111010000). $finish does not suppress the
    // current instant's postponed-region dump.
    CHECK(times == 2001);
    CHECK(contains(body, "b11111001111 !"));    // a == 1999 at the penultimate instant
    CHECK(contains(body, "#2000\nb11111010000 !")); // a == 2000 dumped at the finish instant
}

TEST_CASE("vcd: dirty set re-emits only signals that actually change") {
    // `moving` toggles every posedge (an NBA target); `still` is written the same
    // value every posedge, and `idle` is never written. All three appear at #0
    // (the initial dump); afterwards only `moving` should re-appear -- the NBA
    // commit touches `still` too, but the flush diff suppresses the no-change.
    const std::string src = R"(
        module top;
          reg clk = 0;
          reg [3:0] moving = 0;
          reg [3:0] still  = 7;
          reg [3:0] idle   = 3;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            moving <= moving + 1;
            still  <= 7;
          end
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #28 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));

    // The initial dump at #0 carries all three; count later re-emissions.
    const auto count = [&](std::string_view id) {
        size_t n = 0;
        for (size_t p = body.find(id); p != std::string::npos; p = body.find(id, p + 1)) {
            ++n;
        }
        return n;
    };
    // Identify each var's id from the header is overkill; instead check that only
    // one 4-bit value line pattern recurs. `moving` reaches 1,2 over #28; `still`
    // and `idle` never change after #0. Concretely: exactly one #0 block, and after
    // it only `moving`'s edges produce vector lines.
    CHECK(count("#0") == 1);
    // moving = 1 at first posedge (#5), 2 at #15, 3 at #25 -> b1, b10, b11 appear.
    CHECK(contains(body, "b1 "));
    CHECK(contains(body, "b10 "));
    CHECK(contains(body, "b11 "));
    // `still` holds 7 (b111) and `idle` holds 3 (b11) only at #0; b111 must not
    // recur (still never changes value).
    CHECK(count("b111 ") == 1);
}

TEST_CASE("vcd: $dumpvars(0, top) matches the bare whole-design form") {
    const std::string bare = R"(
        module top;
          reg [3:0] count = 0;
          always #5 count = count + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #18 $finish;
          end
        endmodule
    )";
    const std::string scoped = R"(
        module top;
          reg [3:0] count = 0;
          always #5 count = count + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars(0, top);
            #18 $finish;
          end
        endmodule
    )";
    CHECK(body_of(run_vcd(bare)) == body_of(run_vcd(scoped)));
}

TEST_CASE("vcd: dump opened from a task traces the whole run") {
    // $dumpfile / $dumpvars are legal in a task a process calls. The design-wide
    // "does this design dump?" scan used to look at process bodies only, so the
    // per-instant flush was never emitted and the file held just the final
    // instant. Compare against the same setup written inline.
    const std::string in_task = R"(
        module top;
          reg [3:0] count = 0;
          always #5 count = count + 1;
          task automatic open_dump;
            $dumpfile("OUT.vcd");
            $dumpvars(0, top);
          endtask
          initial begin
            open_dump();
            #18 $finish;
          end
        endmodule
    )";
    const std::string inline_ = R"(
        module top;
          reg [3:0] count = 0;
          always #5 count = count + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars(0, top);
            #18 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(in_task));
    CHECK(body == body_of(run_vcd(inline_)));
    CHECK(body == "#0\nb0 !\n#5\nb1 !\n#10\nb10 !\n#15\nb11 !\n");
}

TEST_CASE("vcd: reals dump as $var real with decimal values") {
    // A real/shortreal has no bit representation in the file: it declares as
    // `$var real` and each change is `r<decimal> <id>` (LRM §21.7.2.1). The
    // decimals are the shortest round-tripping form, so 1/3 keeps all 16 digits a
    // double needs and -0.125 stays exact.
    const std::string src = R"(
        module top;
          real r;
          shortreal s;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            r = 1.5;
            s = 2.5;
            #5 r = -0.125;
            #5 r = 1.0 / 3.0;
            #5 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    // The size field is meaningless for a real, and is 64 for both widths.
    CHECK(contains(vcd, "$var real 64 ! r $end"));
    CHECK(contains(vcd, "$var real 64 \" s $end"));
    CHECK(body_of(vcd)
          == "#0\nr1.5 !\nr2.5 \"\n"
             "#5\nr-0.125 !\n"
             "#10\nr0.3333333333333333 !\n");
}

TEST_CASE("vcd: a real powers on at zero and checkpoints as NaN under $dumpoff") {
    // A real has no x, so the all-x $dumpoff checkpoint has no bit pattern to
    // write; NaN is the value it cannot otherwise hold (and iverilog's spelling).
    const std::string src = R"(
        module top;
          real r;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 r = 1.5;
            #5 $dumpoff;
            #5 $dumpon;
            #5 $finish;
          end
        endmodule
    )";
    CHECK(body_of(run_vcd(src)) == "#0\nr0 !\n#5\nr1.5 !\n#10\nrNaN !\n#15\nr1.5 !\n");
}

TEST_CASE("vcd: real memories and nonblocking real targets are dumped per change") {
    // Reals ride the ordinary dumped-signal paths: one $var per array element, and
    // the NBA commit touch marks a real target dirty like any other scalar.
    const std::string src = R"(
        module top;
          reg clk = 0;
          real mem [0:1];
          real q;
          always #5 clk = ~clk;
          always @(posedge clk) q <= 2.5;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            mem[0] = 0.5;
            mem[1] = -1.0;
            #8 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    CHECK(contains(vcd, "$var real 64 \" mem[0] $end"));
    CHECK(contains(vcd, "$var real 64 # mem[1] $end"));
    CHECK(body_of(vcd) == "#0\n0!\nr0.5 \"\nr-1 #\nr0 $\n#5\n1!\nr2.5 $\n");
}

TEST_CASE("vcd: $dumplimit stops the dump at an instant boundary and says so") {
    // LRM §21.7.1.5: dumping stops when the file reaches the limit and a comment
    // records it. The cap is tested at instant boundaries, so the file holds whole
    // instants only -- byte-identical to the uncapped dump up to the cut. Once cut,
    // nothing resumes it: the trailing $dumpon / $dumpall add nothing.
    const std::string capped_src = R"(
        module top;
          reg [7:0] a = 0;
          always #5 a = a + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumplimit(300);
            $dumpvars;
            #500 $dumpon;
            #5 $dumpall;
            #5 $finish;
          end
        endmodule
    )";
    const std::string uncapped_src = R"(
        module top;
          reg [7:0] a = 0;
          always #5 a = a + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #500 $dumpon;
            #5 $dumpall;
            #5 $finish;
          end
        endmodule
    )";
    const std::string capped   = run_vcd(capped_src);
    const std::string uncapped = run_vcd(uncapped_src);

    const std::string_view note = "$comment Dump file limit (300 bytes) reached. $end\n";
    REQUIRE(capped.ends_with(note));
    const std::string kept = capped.substr(0, capped.size() - note.size());
    CHECK(uncapped.starts_with(kept));      // whole instants, identical content
    CHECK(kept.size() >= 300);              // cut at the first boundary past the cap
    CHECK(kept.size() < uncapped.size());   // and the run really was truncated
}

TEST_CASE("vcd: $dumplimit(0) leaves the dump uncapped") {
    const std::string src = R"(
        module top;
          reg [7:0] a = 0;
          always #5 a = a + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumplimit(0);
            $dumpvars;
            #500 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    CHECK(!contains(vcd, "$comment"));
    CHECK(contains(body_of(vcd), "#495\n"));
}

TEST_CASE("vcd: a named event declares $var event and pulses at each trigger") {
    // An event has occurrences, not a value: the declaration is `$var event 1`,
    // and each trigger instant carries the bare pulse `1<id>` with no complement.
    const std::string src = R"(
        module top;
          event e;
          reg [1:0] r;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            r = 2'b10;
            #5 -> e;
            #5 -> e;
            r = 2'b01;
            #5 $finish;
          end
        endmodule
    )";
    const std::string vcd  = run_vcd(src);
    const std::string body = body_of(vcd);
    CHECK(contains(vcd, "$var event 1 ! e $end"));
    // #0 states r only -- an event has no power-on value to dump, so the initial
    // $dumpvars must not pulse it.
    CHECK(body == "#0\nb10 \"\n#5\n1!\n#10\n1!\nb1 \"\n");
}

TEST_CASE("vcd: every trigger form reaches the dump, once per instant") {
    // `-> e` (blocking), `->> e` (NBA region) and `->> #d e` (delayed) all converge
    // on the same stamp-and-service path, so all three must pulse; two triggers of
    // one event in a single instant collapse to the one pulse VCD can express.
    const std::string src = R"(
        module top;
          event a, b, c;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 ->> a;
            #5 ->> #3 b;
            #10 -> c; -> c;
            #5 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    // a at #5, b at #10+3, c once at #20. Verified against iverilog 13, which
    // writes the same three pulses at the same three instants.
    CHECK(body == "#5\n1!\n#13\n1\"\n#20\n1#\n");
}

TEST_CASE("vcd: a forced dump does not invent an event trigger") {
    // $dumpall / $dumpon re-state current values, and an event has none. A trigger
    // missed while dumping was off must not surface as a pulse at the $dumpon
    // instant either -- that would report an occurrence at a time it did not occur.
    // (iverilog gets both of these wrong: it pulses every event at time 0, at
    // $dumpall, and again at $dumpon.)
    const std::string src = R"(
        module top;
          event e;
          reg r;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            r = 0;
            #5 $dumpall;
            #5 $dumpoff;
            #2 -> e;
            #3 $dumpon;
            #5 -> e;
            #5 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    // #0 r=0; #5 $dumpall re-states r alone; #10 the $dumpoff checkpoint makes r
    // unknown (the event is skipped, it has nothing to make unknown); #15 $dumpon
    // re-states r; #20 the real trigger.
    CHECK(body == "#0\n0\"\n#5\n0\"\n#10\nx\"\n#15\n0\"\n#20\n1!\n");
}

TEST_CASE("vcd: an event triggering at the dumpvars instant still pulses") {
    // The never-triggered sentinel is UINT64_MAX, so a trigger at tick 0 is a real
    // change against it -- the initial dump must carry the pulse rather than lose
    // it to the shadow starting at zero.
    const std::string src = R"(
        module top;
          event e;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            -> e;
            #5 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    CHECK(body == "#0\n1!\n");
}

TEST_CASE("vcd: events in submodules dump in their own scope") {
    const std::string src = R"(
        module sub;
          event se;
          initial #7 -> se;
        endmodule
        module top;
          event e;
          sub u();
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 -> e;
            #10 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    CHECK(contains(vcd, "$scope module top $end\n$var event 1 ! e $end\n$scope module u $end\n"
                        "$var event 1 \" se $end\n$upscope $end\n$upscope $end\n"));
    CHECK(body_of(vcd) == "#5\n1!\n#7\n1\"\n");
}

TEST_CASE("vcd: a checkpoint with nothing to checkpoint writes no time marker") {
    // $dumpoff makes every dumped variable unknown, but an event has no value to
    // make unknown. When events are all there is, the checkpoint must write
    // nothing at all rather than a bare `#tick` with no changes under it.
    const std::string src = R"(
        module top;
          event e;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 -> e;
            #5 $dumpoff;
            #5 $finish;
          end
        endmodule
    )";
    CHECK(body_of(run_vcd(src)) == "#5\n1!\n");
}
