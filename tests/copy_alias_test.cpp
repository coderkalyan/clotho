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

// A continuous assignment copying one whole signal into another of a unit becomes
// a second name for one storage (docs/design/modular-compilation.md, "Copy
// aliasing"). These cases pin what a design observes through both names, and the
// cases where something other than the copy writes the target, which must keep
// the copy. The expected outputs follow from the LRM clauses each case cites and
// match iverilog 13.

namespace {

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

// JIT-run a design whose source opens its dump with $dumpfile("OUT.vcd") and
// return the dump.
std::string run_vcd(std::string source) {
    const std::string path =
      (std::filesystem::temp_directory_path() / (test::detail::unique_stem() + ".vcd")).generic_string();
    const auto tok = source.find("OUT.vcd");
    REQUIRE(tok != std::string::npos);
    source.replace(tok, std::string_view("OUT.vcd").size(), path);

    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    ir::run_jit(*r.module);

    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::remove(path.c_str());
    return ss.str();
}

} // namespace

TEST_CASE("copy alias: a forced copy target follows its force and release, the source does not") {
    // LRM 10.6.2: a force on a net overrides all its drivers and a release
    // reassigns the driver value at once; a continuous assignment to a variable
    // is re-established by release and re-evaluates in the Active region, hence
    // the #0 before the last print.
    const std::string out = run_capture(R"(
        module child;
          logic a;
          wire  b = a;
          wire  c = b;
          logic v;
          assign v = a;
        endmodule
        module tb;
          child u();
          task show(input string tag);
            $display("%0d %s a=%b b=%b c=%b v=%b", $time, tag, u.a, u.b, u.c, u.v);
          endtask
          initial begin
            u.a = 0;
            #1 show("#1");
            force tb.u.b = 1;
            force tb.u.v = 1;
            #1 show("#1 forced");
            u.a = 1;
            #1 show("#1 forced");
            u.a = 0;
            #1 show("#1 forced");
            release tb.u.b;
            $display("%0d right after release: b=%b", $time, u.b);
            release tb.u.v;
            #0 show("#0 released");
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "1 #1 a=0 b=0 c=0 v=0\n"
                 "2 #1 forced a=0 b=1 c=1 v=1\n"
                 "3 #1 forced a=1 b=1 c=1 v=1\n"
                 "4 #1 forced a=0 b=1 c=1 v=1\n"
                 "4 right after release: b=0\n"
                 "4 #0 released a=0 b=0 c=0 v=0\n");
}

TEST_CASE("copy alias: a copy chain through two port levels, with forces on an internal and an output net") {
    // LRM 23.3.3 (a port is a continuous assignment), 23.6, 10.6.2. Only nets
    // downstream of a forced net change; the forced value never reaches upstream.
    const std::string out = run_capture(R"(
        module leaf(input wire [3:0] li, output wire [3:0] lo);
          wire [3:0] t = li;
          assign lo = t;
        endmodule
        module mid(input wire [3:0] mi, output wire [3:0] mo);
          wire [3:0] m = mi;
          leaf l(.li(m), .lo(mo));
        endmodule
        module tb;
          logic [3:0] src;
          wire  [3:0] dst;
          mid u(.mi(src), .mo(dst));
          task show(input string tag);
            $display("%0d %s src=%h mi=%h m=%h li=%h t=%h lo=%h mo=%h dst=%h", $time, tag, src,
                     tb.u.mi, tb.u.m, tb.u.l.li, tb.u.l.t, tb.u.l.lo, tb.u.mo, dst);
          endtask
          initial begin
            src = 4'h3;
            #1 show("plain");
            src = 4'ha;
            #1 show("plain");
            force tb.u.l.t = 4'hf;
            #1 show("force t");
            src = 4'h5;
            #1 show("force t");
            release tb.u.l.t;
            #1 show("release t");
            force tb.u.l.lo = 4'h0;
            #1 show("force lo");
            src = 4'h6;
            #1 show("force lo");
            release tb.u.l.lo;
            #1 show("release lo");
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "1 plain src=3 mi=3 m=3 li=3 t=3 lo=3 mo=3 dst=3\n"
                 "2 plain src=a mi=a m=a li=a t=a lo=a mo=a dst=a\n"
                 "3 force t src=a mi=a m=a li=a t=f lo=f mo=f dst=f\n"
                 "4 force t src=5 mi=5 m=5 li=5 t=f lo=f mo=f dst=f\n"
                 "5 release t src=5 mi=5 m=5 li=5 t=5 lo=5 mo=5 dst=5\n"
                 "6 force lo src=5 mi=5 m=5 li=5 t=5 lo=0 mo=0 dst=0\n"
                 "7 force lo src=6 mi=6 m=6 li=6 t=6 lo=0 mo=0 dst=0\n"
                 "8 release lo src=6 mi=6 m=6 li=6 t=6 lo=6 mo=6 dst=6\n");
}

TEST_CASE("copy alias: an internal net copied to an output port is read and waited on by both names") {
    // `assign o = x;` makes x a name for the port's storage. The parent reads x
    // and o hierarchically and through the port, and counts changes of u.x after
    // time 0 (at time 0 the count would race the first evaluation, LRM 4.7). An
    // assignment of an equal value is no change (LRM 9.4.2), and any x operand
    // bit makes the sum all x (LRM 11.4.3).
    const std::string out = run_capture(R"(
        module child(input wire [3:0] i, output wire [3:0] o);
          wire [3:0] x;
          assign x = i + 4'd1;
          assign o = x;
        endmodule
        module tb;
          logic [3:0] v;
          wire  [3:0] y;
          int n = 0;
          child u(.i(v), .o(y));
          always @(u.x) if ($time > 0) n++;
          task show;
            $display("%0d x=%h o=%h y=%h n=%0d", $time, u.x, u.o, y, n);
          endtask
          initial begin
            v = 4'd3;
            #1 show;
            v = 4'd7;
            #1 show;
            v = 4'd7;
            #1 show;
            v = 4'b1x00;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "1 x=4 o=4 y=4 n=0\n"
                 "2 x=8 o=8 y=8 n=1\n"
                 "3 x=8 o=8 y=8 n=1\n"
                 "4 x=x o=x y=x n=2\n");
}

TEST_CASE("copy alias: the VCD dump declares each alias on its target's identifier code") {
    // LRM 21.7.2.3: "More than one reference name can be mapped to the same
    // identifier code". b and c copy a, so all three are one variable of the dump,
    // declared under their own names and var types, and each change is one record.
    const std::string vcd = run_vcd(R"(
        module top;
          logic a = 0;
          wire  b = a;
          wire  c = b;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #1 a = 1;
            #1 $finish;
          end
        endmodule
    )");
    CHECK(vcd.find("$var reg 1 ! a $end\n$var wire 1 ! b $end\n$var wire 1 ! c $end\n") != std::string::npos);
    const auto defs = vcd.find("$enddefinitions");
    REQUIRE(defs != std::string::npos);
    CHECK(vcd.substr(vcd.find('\n', defs) + 1) == "#0\n0!\n#1\n1!\n");
}
