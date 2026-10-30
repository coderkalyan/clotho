#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors randcase_test.cpp).
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

// Confirm lowering fails loudly for an unsupported construct.
std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostic_text();
}

} // namespace

// The LRM 18.17 opening example. The grammar admits exactly four utterances, so
// the check is membership, not a fixed string: `first` and `second` each pick at
// random. verilator 5.050 agrees on the shape.
TEST_CASE("randsequence generates a grammar-driven stream (LRM 18.17)") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                for (int k = 0; k < 400; k++) begin
                    s = "";
                    randsequence( main )
                        main   : first second done ;
                        first  : add | dec ;
                        second : pop | push ;
                        done   : { s = {s, "."}; } ;
                        add    : { s = {s, "a"}; } ;
                        dec    : { s = {s, "d"}; } ;
                        pop    : { s = {s, "o"}; } ;
                        push   : { s = {s, "u"}; } ;
                    endsequence
                    if (!(s inside {"ao.", "au.", "do.", "du."})) begin
                        $display("bad %s", s);
                        $finish;
                    end
                end
                $display("ok");
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ok\n");
}

// LRM 18.17: "If unspecified, the first production becomes the top-level
// production." Only `b` is generated; `a` is never entered.
TEST_CASE("randsequence with no named top production starts at the first (LRM 18.17)") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                randsequence()
                    b : { $display("b"); } ;
                    a : { $display("a"); } ;
                endsequence
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "b\n");
}

// LRM 18.17.1: `add := 3 | dec := (1 + 1)` gives 60% / 40%. Bounds rather than
// exact counts; a 10000-trial window is far tighter than the +/-5% asserted.
TEST_CASE("randsequence rule weights set the probabilities (LRM 18.17.1)") {
    constexpr std::string_view src = R"(
        module m;
            int a, d;
            initial begin
                for (int i = 0; i < 10000; i++) begin
                    randsequence( first )
                        first : add := 3 | dec := (1 + 1) ;
                        add : { a++; };
                        dec : { d++; };
                    endsequence
                end
                $display("%0d %0d", a + d, (a > 5700 && a < 6300) && (d > 3700 && d < 4300));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10000 1\n");
}

// LRM 18.17.1: "If only some weights are specified, the unspecified weights shall
// use a weight of 1." Two unweighted alternatives are therefore 50/50 -- and the
// implicit 1 has to carry an integer's precision, or the two would sum to zero at
// the one-bit minimum and neither alternative would ever be generated.
TEST_CASE("randsequence unspecified weights default to 1 (LRM 18.17.1)") {
    constexpr std::string_view src = R"(
        module m;
            int a, d;
            initial begin
                for (int i = 0; i < 10000; i++) begin
                    randsequence( first )
                        first : add | dec ;
                        add : { a++; };
                        dec : { d++; };
                    endsequence
                end
                $display("%0d %0d", a + d, (a > 4700 && a < 5300));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10000 1\n");
}

// LRM 18.17.1: "Weight expressions are evaluated when their enclosing production
// is selected, thus allowing weights to change dynamically." The weight flips with
// design state between the two activations, so each takes a different branch with
// certainty.
TEST_CASE("randsequence weights are re-evaluated per activation (LRM 18.17.1)") {
    constexpr std::string_view src = R"(
        module m;
            int pick;
            initial begin
                for (pick = 0; pick < 2; pick++) begin
                    randsequence( sel )
                        sel : lo := (pick == 0) | hi := (pick == 1) ;
                        lo : { $display("lo"); };
                        hi : { $display("hi"); };
                    endsequence
                end
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "lo\nhi\n");
}

// LRM 18.17.2's own example, driven to both arms by the state its code blocks
// update.
TEST_CASE("randsequence if-else production statement (LRM 18.17.2)") {
    constexpr std::string_view src = R"(
        module m;
            int depth;
            initial begin
                for (int k = 0; k < 4; k++) begin
                    randsequence()
                        PP_OP : if ( depth < 2 ) PUSH else POP ;
                        PUSH : { ++depth; $display("push %0d", depth); };
                        POP  : { --depth; $display("pop %0d", depth); };
                    endsequence
                end
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "push 1\npush 2\npop 1\npush 2\n");
}

// LRM 18.17.3's own example, including the multi-label item `1, 2 : DISK`. That
// shape is exactly what slang's createCaseProd mis-paired before
// patches/slang-randseq-rule-pairing.patch: `2` selected DISK's neighbour.
TEST_CASE("randsequence case production statement (LRM 18.17.3)") {
    constexpr std::string_view src = R"(
        module m;
            int device;
            initial begin
                for (device = 0; device < 4; device++) begin
                    randsequence()
                        SELECT : case ( device & 7 )
                            0       : NETWORK ;
                            1, 2    : DISK ;
                            default : MEMORY ;
                        endcase ;
                        NETWORK : { $display("%0d NETWORK", device); };
                        DISK    : { $display("%0d DISK", device); };
                        MEMORY  : { $display("%0d MEMORY", device); };
                    endsequence
                end
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 NETWORK\n1 DISK\n2 DISK\n3 MEMORY\n");
}

// LRM 18.17.3: a case production with no matching item and no default generates
// nothing at all.
TEST_CASE("randsequence case production with no match generates nothing (LRM 18.17.3)") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                randsequence()
                    TOP : SELECT TAIL ;
                    SELECT : case ( 9 ) 0 : A ; 1 : A ; endcase ;
                    A : { $display("A"); };
                    TAIL : { $display("tail"); };
                endsequence
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "tail\n");
}

// LRM 18.17.4: the count is evaluated once and the production generated that many
// times.
TEST_CASE("randsequence repeat production statement (LRM 18.17.4)") {
    constexpr std::string_view src = R"(
        module m;
            int n, calls;
            initial begin
                randsequence()
                    PUSH_OPER : repeat(bound()) PUSH ;
                    PUSH : { n++; };
                endsequence
                $display("%0d %0d", n, calls);
                $finish;
            end
            function automatic int bound(); calls++; return 4; endfunction
        endmodule
    )";
    CHECK(run_capture(src) == "4 1\n");
}

// LRM 18.17.6's `return` example, verbatim, with its stated output for each value
// of `flag`. Fully determined by the grammar -- no draw is involved -- so this is
// an exact-string check; verilator 5.050 produces the same three lines.
TEST_CASE("randsequence return aborts the current production (LRM 18.17.6)") {
    constexpr std::string_view src = R"(
        module m;
            int flag;
            initial begin
                for (flag = 0; flag < 3; flag++) begin
                    $write("%0d ==>", flag);
                    randsequence()
                        TOP : P1 P2 ;
                        P1 : A B C ;
                        P2 : A { if( flag == 1 ) return; } B C ;
                        A  : { $write( " A" ); } ;
                        B  : { if( flag == 2 ) return; $write( " B" ); } ;
                        C  : { $write( " C" ); } ;
                    endsequence
                    $display("");
                end
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 ==> A B C A B C\n1 ==> A B C A\n2 ==> A C A C\n");
}

// LRM 18.17.6's `break` example: COMMAND is not generated, DATA is not generated
// either -- the jump leaves the whole randsequence -- and execution resumes at the
// statement after `endsequence`. verilator 5.050 crashes on this example
// (std::out_of_range), so the LRM text is the only oracle.
TEST_CASE("randsequence break terminates the whole block (LRM 18.17.6)") {
    constexpr std::string_view src = R"(
        module m;
            int fifo_length, max_length;
            initial begin
                max_length = 4;
                for (fifo_length = 0; fifo_length < 6; fifo_length += 5) begin
                    $write("%0d:", fifo_length);
                    randsequence()
                        WRITE : SETUP DATA ;
                        SETUP : { if( fifo_length >= max_length ) break; } COMMAND ;
                        COMMAND : { $write(" CMD"); } ;
                        DATA  : { $write(" DATA"); } ;
                    endsequence
                    $display(" next");
                end
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0: CMD DATA next\n5: next\n");
}

// LRM 18.17.6 defers to 12.8 for a `break` inside a loop: it ends the loop, not
// the randsequence, so the production after it is still generated.
TEST_CASE("randsequence break inside a loop binds to the loop (LRM 18.17.6 / 12.8)") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                randsequence( A )
                    A : { for (int i = 0; i < 4; i++) begin
                              if (i == 2) break;
                              $display("i=%0d", i);
                          end } B ;
                    B : { $display("B"); } ;
                endsequence
                $display("after");
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "i=0\ni=1\nB\nafter\n");
}

// LRM 18.17.4: "The repeat production statement itself cannot be terminated
// prematurely. A break statement will terminate the entire randsequence block."
// The synthesized repeat loop is therefore not a break target.
TEST_CASE("randsequence break inside a repeat ends the block (LRM 18.17.4)") {
    constexpr std::string_view src = R"(
        module m;
            int n;
            initial begin
                randsequence()
                    TOP : repeat(4) STEP TAIL ;
                    STEP : { n++; $display("n=%0d", n); if (n == 2) break; } ;
                    TAIL : { $display("tail"); } ;
                endsequence
                $display("after");
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=1\nn=2\nafter\n");
}

// LRM 18.17.7's first example: `gen` takes a string with a default, and five
// productions call it. verilator 5.050 passes only the first byte of the string,
// so the LRM text is the oracle here.
TEST_CASE("randsequence production arguments and defaults (LRM 18.17.7)") {
    constexpr std::string_view src = R"(
        module m;
            string log;
            initial begin
                for (int k = 0; k < 200; k++) begin
                    log = "";
                    randsequence( main )
                        main : first second gen ;
                        first : add | dec ;
                        second : pop | push ;
                        add : gen("add") ;
                        dec : gen("dec") ;
                        pop : gen("pop") ;
                        push : gen("push") ;
                        gen( string s = "done" ) : { log = {log, s, ";"}; } ;
                    endsequence
                    if (!(log inside {"add;pop;done;", "add;push;done;",
                                      "dec;pop;done;", "dec;push;done;"})) begin
                        $display("bad %s", log);
                        $finish;
                    end
                end
                $display("ok");
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ok\n");
}

// LRM 18.17.7: a production named once in a rule gets a scalar result variable,
// one named N times gets `[1:N]` indexed in syntactic order of appearance.
TEST_CASE("randsequence non-void productions return values (LRM 18.17.7)") {
    constexpr std::string_view src = R"(
        module m;
            int c;
            initial begin
                randsequence( TOP )
                    void TOP : one two ;
                    void one : C { $display("one %0d", C); } ;
                    void two : C C C { $display("two %0d %0d %0d", C[1], C[2], C[3]); } ;
                    int C : { c++; return c; } ;
                endsequence
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "one 1\ntwo 2 3 4\n");
}

// LRM 18.17.7 Example 2: `repeat(5) C` declares a SCALAR `int C`, so every
// iteration writes the one slot and the readable value is the last; and only one
// arm of an if-else writes its element, the other keeping its default (18.17.7
// calls a slot for a production that was not generated "undefined").
TEST_CASE("randsequence result slots follow syntactic appearance (LRM 18.17.7)") {
    constexpr std::string_view src = R"(
        module m;
            int cnt; bit cond;
            initial begin
                for (int k = 0; k < 2; k++) begin
                    cond = k[0];
                    randsequence( A )
                        void A  : A1 A2 ;
                        void A1 : { cnt = 1; } repeat(5) C
                                  { $display("c=%0d", C); } ;
                        void A2 : if (cond) D(5) else D(20)
                                  { $display("d1=%0d d2=%0d", D[1], D[2]); } ;
                        int C   : { cnt = cnt + 1; return cnt; } ;
                        int D (int prm) : { return prm; } ;
                    endsequence
                end
                $finish;
            end
        endmodule
    )";
    // `{ cnt = 1; }` reruns each activation, so both give c=6: the scalar slot holds
    // the fifth (last) iteration's value, not five separate ones.
    CHECK(run_capture(src) == "c=6\nd1=0 d2=20\nc=6\nd1=5 d2=0\n");
}

// LRM 18.17: "a recursive production will cause looping." This is the clause's own
// GenQueue example, left-recursive through `LIST : LIST ITEM := 8`. Productions
// lower to real ir::Functions precisely so this works; verilator, which inlines
// the grammar, cannot compile it at all.
TEST_CASE("randsequence supports recursive productions (LRM 18.17.7 GenQueue)") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int GenQueue(int low, int high, ref int q[$]);
                randsequence()
                    TOP  : BOUND(low) LIST BOUND(high) ;
                    LIST : LIST ITEM := 8 { q = { q, ITEM }; }
                         | ITEM := 2 { q = { q, ITEM }; }
                         ;
                    int ITEM : { return $urandom_range( low, high ); } ;
                    BOUND(int b) : { q = { q, b }; } ;
                endsequence
                return q.size();
            endfunction
            int q[$];
            initial begin
                int n = GenQueue(1, 9, q);
                int ok = 1;
                foreach (q[i]) if (q[i] < 1 || q[i] > 9) ok = 0;
                $display("%0d %0d %0d %0d", n >= 3, q[0], q[q.size()-1], ok);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1 9 1\n");
}

// A production body may consume time when the randsequence sits in a coroutine:
// productions are then lowered as tasks, not as plain functions.
TEST_CASE("randsequence production code block may consume time") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t();
                randsequence( S )
                    S : GAP GAP ;
                    GAP : { #5 $display("t=%0t", $time); } ;
                endsequence
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture(src) == "t=5\nt=10\n");
}

// A randsequence inside a class method: LRM 18.17 makes the production a scope of
// its own, but slang's lookup walks through it, so a bare property reference is
// still `this`-relative.
TEST_CASE("randsequence inside a class method reaches this") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                int n;
                function void go();
                    randsequence( A )
                        A : B B ;
                        B : { n++; $display("n=%0d", n); } ;
                    endsequence
                endfunction
            endclass
            initial begin
                C c = new;
                c.go();
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=1\nn=2\n");
}

// A code block reads and writes the enclosing subroutine's automatics, which a
// production takes as ref formals -- there is no frame to reach otherwise.
TEST_CASE("randsequence code blocks reach the enclosing frame") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int f(int seed);
                int acc = seed;
                randsequence( A )
                    A : B B B ;
                    B : { acc = acc * 2 + 1; } ;
                endsequence
                return acc;
            endfunction
            initial begin
                $display("%0d %0d", f(0), f(1));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "7 15\n");
}

// LRM 18.17.5's own example. The six interleavings that preserve each sequence's
// internal order are exactly the legal outcomes, and every one of them occurs.
TEST_CASE("randsequence rand join produces every order-preserving interleaving (LRM 18.17.5)") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            int    seen[string];
            initial begin
                for (int k = 0; k < 4000; k++) begin
                    s = "";
                    randsequence( TOP )
                        TOP : rand join S1 S2 ;
                        S1 : A B ;
                        S2 : C D ;
                        A : { s = {s, "A"}; }; B : { s = {s, "B"}; };
                        C : { s = {s, "C"}; }; D : { s = {s, "D"}; };
                    endsequence
                    seen[s] = seen[s] + 1;
                end
                $display("%0d %0d %0d %0d %0d %0d %0d",
                         seen.num(),
                         seen["ABCD"] > 100, seen["ACBD"] > 100, seen["ACDB"] > 100,
                         seen["CABD"] > 100, seen["CADB"] > 100, seen["CDAB"] > 100);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "6 1 1 1 1 1 1\n");
}

// LRM 18.17.5 pins the bias expression at three points: 0.0 favours the shortest
// remaining sequence -- which keeps whichever was picked first running to the end,
// giving A B C D and C D A B -- and 1.0 favours the longest, which is what
// produces the four genuinely interleaved orders. verilator 5.050 ignores the
// expression entirely (0.0 and 1.0 measure identical), so the clause is the oracle.
TEST_CASE("randsequence rand join length bias (LRM 18.17.5)") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            int    lo, hi;
            initial begin
                for (int k = 0; k < 4000; k++) begin
                    s = "";
                    randsequence( TOP )
                        TOP : rand join (0.0) S1 S2 ;
                        S1 : A B ; S2 : C D ;
                        A : { s = {s,"A"}; }; B : { s = {s,"B"}; };
                        C : { s = {s,"C"}; }; D : { s = {s,"D"}; };
                    endsequence
                    if (s == "ABCD" || s == "CDAB") lo++;
                    s = "";
                    randsequence( TOP )
                        TOP : rand join (1.0) S1 S2 ;
                        S1 : A B ; S2 : C D ;
                        A : { s = {s,"A"}; }; B : { s = {s,"B"}; };
                        C : { s = {s,"C"}; }; D : { s = {s,"D"}; };
                    endsequence
                    if (s == "ABCD" || s == "CDAB") hi++;
                end
                $display("%0d %0d", lo > 2400, hi < 1600);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1\n");
}

// A `return` in an interleaved production drops that sequence's remaining units
// and lets the others carry on; a `break` still ends the whole block. Bias 0.0
// makes the pick deterministic once a sequence is under way.
TEST_CASE("randsequence rand join honours return and break (LRM 18.17.5 / 18.17.6)") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            int    flag;
            initial begin
                for (flag = 0; flag < 2; flag++) begin
                    s = "";
                    randsequence( TOP )
                        TOP : rand join (0.0) S1 S2 ;
                        S1 : A { if (flag == 1) return; } C ;
                        S2 : D ;
                        A : { s = {s,"A"}; };
                        C : { s = {s,"C"}; };
                        D : { s = {s,"D"}; };
                    endsequence
                    $display("%0d %0d", flag, s.len());
                end
                s = "";
                randsequence( T2 )
                    T2 : rand join (0.0) P Q ;
                    P : { s = {s,"P"}; } { break; } { s = {s,"!"}; } ;
                    Q : { s = {s,"Q"}; } ;
                endsequence
                $display("%s", s);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 3\n1 2\nQP\n");
}

// Value passing survives interleaving: each operand's result reaches the rule
// variable of the rule that joined them.
TEST_CASE("randsequence rand join operands may return values (LRM 18.17.5 / 18.17.7)") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                randsequence( T )
                    void T : rand join (1.0) V W := 1 { $display("%0d %0d", V, W); } ;
                    int V : { return 11; } ;
                    int W : { return 22; } ;
                endsequence
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "11 22\n");
}

// A weight sum wider than 32 bits would need the LRM's several-draw stitching
// scheme (18.16, whose weight rules 18.17.1 shares); guarded loud, exactly as
// randcase guards its own.
TEST_CASE("randsequence with a 64-bit weight fails loudly") {
    CHECK(lower_error("module m; longint w; initial begin w = 5;\n"
                      " randsequence(A) A : B := w | C := 1 ; B : {} ; C : {} ; endsequence end\n"
                      "endmodule")
            .find("weight wider than 32 bits") != std::string::npos);
}

// LRM 18.17.7 over 18.17.5: an interleaved production's rule holds its implicit
// result variables from the unit that writes one to the unit that reads it, with
// the other sequence's units in between. They live in the driver's frame and reach
// `step` by reference, since `step` is entered once per unit.
TEST_CASE("randsequence rand join operands may pass values within their own rules") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                randsequence( T )
                    T : rand join (1.0) P Q ;
                    P : V V { s = {s, $sformatf("P%0d%0d ", V[1], V[2])}; } ;
                    Q : W { s = {s, $sformatf("Q%0d ", W)}; } ;
                    int V : { return 3; } ;
                    int W : { return 9; } ;
                endsequence
                $display("%s", s);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "P33 Q9 \n");
}

// An interleaved production's arguments are evaluated once, before its first unit,
// and its activation spans the whole interleaving (LRM 18.17.7's "similar to a task
// call"): an `input` keeps whatever the body did to it between units, an `output` /
// `inout` copies back exactly once at the end, and a `ref` aliases the actual
// throughout. Bias 0.0 keeps a picked sequence running, which makes the order --
// and so the arithmetic -- deterministic.
TEST_CASE("randsequence rand join operand arguments span the whole activation") {
    constexpr std::string_view src = R"(
        module m;
            typedef int q_t[$];
            string s;
            int    got, seen, io = 5, base = 10;
            q_t    src = {1, 2, 3};
            initial begin
                randsequence( T )
                    T : rand join (0.0) P(got) Q(seen) ;
                    P(output int o) : { o = 41; } { o = o + 1; } ;
                    Q(ref int r)    : { r = 7; } { r = r * 3; } ;
                endsequence
                randsequence( U )
                    U : rand join (0.0) R(io) S ;
                    R(inout int v) : { v = v + 1; } { v = v * 10; } ;
                    S : { } ;
                endsequence
                randsequence( V2 )
                    V2 : rand join (0.0) A(base) B(src) ;
                    A(int v)  : { v = v + 1; } { s = {s, $sformatf("A%0d ", v)}; } ;
                    B(q_t z)  : { z = {z, 99}; } { s = {s, $sformatf("B%0d ", z.size())}; } ;
                endsequence
                $display("%0d %0d %0d %s%0d %0d", got, seen, io, s, base, src.size());
                $finish;
            end
        endmodule
    )";
    // out=42, ref=21, inout=(5+1)*10; the input copies keep their mutations between
    // units (A11, B4) without either escaping to the actual (base=10, size=3).
    CHECK(run_capture(src) == "42 21 60 A11 B4 10 3\n");
}

// LRM 18.17.7 puts no restriction on a production's return type. A container result
// reaches its rule variable through the same whole-container deep copy an ordinary
// assignment applies.
TEST_CASE("randsequence productions may return containers (LRM 18.17.7)") {
    constexpr std::string_view src = R"(
        module m;
            typedef int q_t[$];
            typedef int d_t[];
            typedef int a_t[string];
            int n;
            initial begin
                randsequence( T )
                    void T : P D A { $display("%p %p %p", P, D, A); } ;
                    q_t P : { q_t z = {1, 2, 3}; return z; } ;
                    d_t D : { d_t z = new[2]; z[0] = 7; z[1] = 8; return z; } ;
                    a_t A : { a_t z; z["k"] = 9; return z; } ;
                endsequence
                randsequence( U )
                    void U : Q Q { $display("%p %p", Q[1], Q[2]); } ;
                    q_t Q : { q_t z; n++; z = {n, n * 10}; return z; } ;
                endsequence
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "'{1, 2, 3} '{7, 8} '{\"k\":9}\n'{1, 10} '{2, 20}\n");
}

// A local a DETACHING fork also captured lives in a GC box rather than in the
// frame (docs/design/fork-join.md V4), so the production takes the box HANDLE by
// value -- a first-class GC root -- and both the randsequence and the fork child
// see LRM 9.3.2's one variable.
TEST_CASE("randsequence shares a boxed automatic with a detached fork") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t();
                automatic int acc = 0;
                fork
                    begin #10 $display("fork %0d", acc); end
                join_none
                randsequence( A )
                    A : B B B ;
                    B : { acc = acc * 2 + 1; } ;
                endsequence
                $display("seq %0d", acc);
                #20;
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture(src) == "seq 7\nfork 7\n");
}

// The one interleaving shape that is deferred rather than built: an operand's own
// state would be an inner interleaving cursor chosen at its own plan time, not a
// fixed set of variables the driver can hoist. LRM 18.17.5 interleaves "to depth of
// 1", which does not describe nesting.
TEST_CASE("a nested randsequence rand join operand is refused by name") {
    CHECK(lower_error("module m; initial begin randsequence(T)\n"
                      " T : rand join P Q ; P : rand join R S ; Q : {} ; R : {} ; S : {} ;\n"
                      "endsequence end endmodule")
            .find("operand that is itself a rand join") != std::string::npos);
}

// A randsequence and its code blocks are anonymous scopes (LRM 18.17), so %m in a
// production names the scope the randsequence sits in. The productions are
// synthesized tasks, whose names once leaked as `top.randseq.first`. Neither
// Icarus Verilog 13 (no randsequence) nor Verilator 5.050 (`top.__Vrs0_first`)
// is an oracle here.
TEST_CASE("%m in a production code block names the enclosing scope") {
    CHECK(run_capture(R"(
        module top;
          initial begin : blk
            randsequence (main)
              main : first second;
              first : { $display("first %m"); };
              second : { $display("second %m"); };
            endsequence
          end
        endmodule
    )") == "first top.blk\nsecond top.blk\n");
}
