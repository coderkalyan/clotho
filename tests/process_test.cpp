#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

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

void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

} // namespace

// The `process` class (LRM 9.7). A thread's control block is native storage -- a
// static slot per process, a pooled record per fork child -- and a `process` handle
// is an ordinary class handle onto it. See docs/design/process-class.md.
//
// ORACLE. iverilog 13 cannot parse the class at all (`process p;` is a syntax
// error), so it scores nothing here. Verilator 5.050 implements self / status /
// await / kill and object identity, and those cases were cross-checked against it;
// it does NOT implement suspend / resume (an explicit $error) and does not
// re-parent a finished thread's children, so those cases are pinned from the LRM
// text and say so per case.

// -- process::self() and identity (LRM 9.7) -----------------------------------

// Verilator-scored: `process::self()` twice in one thread compares equal. Clotho
// memoizes one object per control block to get this, because a class-handle `==` is
// a bare pointer compare (docs/design/process-class.md §5.1).
TEST_CASE("process: self() is stable within a thread") {
    const std::string out = run_capture(R"(
        module t;
          process a, b;
          initial begin
            a = process::self();
            b = process::self();
            $display("same=%0d null=%0d", a == b, a == null);
          end
        endmodule
    )");
    CHECK(out == "same=1 null=0\n");
}

// Two different threads get two different handles: identity names the THREAD.
TEST_CASE("process: self() differs between threads") {
    const std::string out = run_capture(R"(
        module t;
          process p1, p2;
          initial begin
            fork
              begin p1 = process::self(); end
              begin p2 = process::self(); end
            join
            $display("distinct=%0d", p1 != p2);
          end
        endmodule
    )");
    CHECK(out == "distinct=1\n");
}

// -- process::status() (LRM 9.7) ----------------------------------------------

// Verilator-scored. RUNNING for the calling thread, WAITING for one parked in a
// blocking statement, FINISHED once it has completed. The state values are slang's
// synthesized enum: FINISHED=0, RUNNING=1, WAITING=2, SUSPENDED=3, KILLED=4.
TEST_CASE("process: status reports RUNNING, WAITING and FINISHED") {
    const std::string out = run_capture(R"(
        module t;
          process me, child;
          initial begin
            me = process::self();
            $display("self=%0d", me.status());
            fork
              begin child = process::self(); #10; end
            join_none
            #1 $display("parked=%0d", child.status());
            #20 $display("done=%0d", child.status());
          end
        endmodule
    )");
    CHECK(out == "self=1\nparked=2\ndone=0\n");
}

// A status enum prints through the ordinary enum-name path: slang's synthesized
// `state` is an ordinary int enum to clotho.
TEST_CASE("process: status().name() renders the enum member") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            process me = process::self();
            $display("%s", me.status().name());
          end
        endmodule
    )");
    // A static-lifetime declaration initializes in the PreInit process (LRM 6.21),
    // so `me` names that thread, which has finished by the time the body runs.
    CHECK(out == "FINISHED\n");
}

// -- process::await() (LRM 9.7) ----------------------------------------------

// Verilator-scored: await blocks until the target completes, and the target's side
// effects are visible when it returns.
TEST_CASE("process: await blocks until the target completes") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          int n = 0;
          initial begin
            fork
              begin child = process::self(); #100 n = 7; end
            join_none
            #1 child.await();
            $display("t=%0t n=%0d status=%0d", $time, n, child.status());
          end
        endmodule
    )");
    CHECK(out == "t=100 n=7 status=0\n");
}

// Awaiting a thread that has already completed returns in the same instant: the
// wait is a loop around a status test, not an unconditional park.
TEST_CASE("process: await on a finished thread returns at once") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          initial begin
            fork
              begin child = process::self(); #5; end
            join
            child.await();
            $display("t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "t=5\n");
}

// Several threads awaiting one target are all woken by its completion: the waiter
// list is unbounded (frame-resident nodes), unlike a static wait bucket.
TEST_CASE("process: two threads await one target") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          initial begin
            fork
              begin child = process::self(); #20; end
            join_none
            #1;
            fork
              begin child.await(); $display("A at %0t", $time); end
              begin child.await(); $display("B at %0t", $time); end
            join
            $display("both done at %0t", $time);
          end
        endmodule
    )");
    // Wake ORDER among threads released by one completion is unspecified (LRM 4.7
    // leaves the order of processes woken by one event arbitrary); the waiter list is
    // push-front, so clotho releases them in reverse link order. Assert the set and
    // the instant, not the order.
    CHECK(out.find("A at 20\n") != std::string::npos);
    CHECK(out.find("B at 20\n") != std::string::npos);
    CHECK(out.find("both done at 20\n") != std::string::npos);
}

// A task runs in its caller's thread (LRM 13.3), so an await inside one parks that
// thread and `process::self()` inside it names the caller, not the activation.
TEST_CASE("process: await and self inside a task name the calling thread") {
    const std::string out = run_capture(R"(
        module t;
          process child, seen;
          task automatic waiter(process p);
            seen = process::self();
            p.await();
          endtask
          initial begin
            process me = null;
            me = process::self();
            fork
              begin child = process::self(); #30; end
            join_none
            #1 waiter(child);
            $display("t=%0t caller=%0d", $time, seen == me);
          end
        endmodule
    )");
    CHECK(out == "t=30 caller=1\n");
}

// -- Refusals (the phases not yet implemented) --------------------------------

// Each unimplemented method refuses by its own name rather than falling into a
// generic message.
TEST_CASE("process: the unimplemented methods refuse by name") {
    check_rejected("module m; initial begin process p; p = process::self(); p.kill(); end endmodule",
                   "process::kill() is not supported yet");
    check_rejected("module m; initial begin process p; p = process::self(); p.suspend(); end endmodule",
                   "process::suspend() is not supported yet");
    check_rejected("module m; initial begin process p; p = process::self(); p.resume(); end endmodule",
                   "process::resume() is not supported yet");
    check_rejected("module m; initial begin process p; p = process::self(); p.srandom(1); end endmodule",
                   "process::srandom() is not supported yet");
    check_rejected("module m; string s; initial begin process p; p = process::self();"
                   " s = p.get_randstate(); end endmodule",
                   "process::get_randstate() is not supported yet");
}

// `process` is abstract in slang, so a user `new` is rejected before clotho sees it
// -- objects exist because threads do.
TEST_CASE("process: cannot be constructed with new") {
    check_rejected("module m; initial begin process p; p = new; end endmodule",
                   "cannot create instance of virtual class 'process'");
}
