#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a design that ends via $finish and return its stdout. The simulation
// gets a process of its own (Subprocess.h explains why $finish cannot share
// the runner's); lowering here as well is what reports a broken source.
// Every wait test drives a free-running stimulus process that only stops at
// $finish, so this (not a plain drain) is the right harness.
std::string run_capture_finish(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    return test::run_design_subprocess(source);
}

std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    return r.ok ? std::string{} : r.diagnostic_text();
}

} // namespace

// IEEE 1800-2023 9.4.3: a `wait` whose condition is already true does not
// suspend — it falls through in the same time step. Verified against iverilog 13
// (which prints the second line at t=0).
TEST_CASE("wait on an already-true condition does not suspend") {
    constexpr std::string_view src = R"(
        module m;
            logic g = 1;
            initial begin
                $display("before %0t", $time);
                wait(g);
                $display("after %0t", $time);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "before 0\nafter 0\n");
}

// LRM 9.4.3: a false condition suspends until it becomes true, then the body
// runs. The process must resume at the exact time the last read changed it true.
TEST_CASE("wait suspends until the condition rises") {
    constexpr std::string_view src = R"(
        module m;
            logic g = 0;
            initial begin
                wait(g);
                $display("woke %0t", $time);
                $finish;
            end
            initial #25 g = 1;
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 25\n");
}

// LRM 9.4.3 truth is the `if` truth (12.4): X is false. A wait on an X value
// must keep blocking until a definite 1 — it must NOT fall through on X. This is
// the case a naive `while (!cond)` lowering gets wrong (RedOr of a 4-state cond
// is X, not a must-plane 0). iverilog resumes only at the 1.
TEST_CASE("wait treats X as false and blocks until a definite 1") {
    constexpr std::string_view src = R"(
        module m;
            logic g;
            initial begin
                wait(g);
                $display("woke %0t g=%b", $time, g);
                $finish;
            end
            initial begin
                #10 g = 1'bx;
                #10 g = 1;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 20 g=1\n");
}

// A multi-signal condition suspends on an any-change wait of every read, and
// re-evaluates the whole expression on each wake.
TEST_CASE("wait on a compound condition wakes when the whole expression is true") {
    constexpr std::string_view src = R"(
        module m;
            logic a = 0, b = 0;
            initial begin
                wait(a && b);
                $display("both %0t", $time);
                $finish;
            end
            initial begin
                #10 a = 1;   // a&&b still false
                #10 b = 1;   // now true
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "both 20\n");
}

// A wait inside a loop must re-arm every iteration — a producer/consumer
// handshake. Verified byte-identical to iverilog 13.
TEST_CASE("wait re-arms across loop iterations (handshake)") {
    constexpr std::string_view src = R"(
        module m;
            logic req = 0;
            int i;
            initial begin
                for (i = 0; i < 3; i++) begin
                    wait(req);
                    $display("got %0d %0t", i, $time);
                    wait(!req);
                end
                $finish;
            end
            initial forever begin
                #10 req = 1;
                #10 req = 0;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "got 0 10\ngot 1 30\ngot 2 50\n");
}

// LRM 9.4.3 with a constant-false condition: the process blocks permanently
// (iverilog warns "will block permanently"). Simulation continues — other
// processes run and $finish still fires. The unreachable body must never print.
TEST_CASE("wait on a constant-false condition parks the process forever") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                $display("before");
                wait(0);
                $display("UNREACHABLE");
            end
            initial #100 begin
                $display("watchdog %0t", $time);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "before\nwatchdog 100\n");
}

// A string condition would need a string-aware wait queue (string stores skip
// wait-queue servicing), so it must fail loudly, not silently never re-trigger.
TEST_CASE("wait on a string condition fails loudly") {
    constexpr std::string_view src = R"(
        module m;
            string s = "";
            initial begin
                wait(s);
                $display("x");
            end
        endmodule
    )";
    const std::string err = lower_error(src);
    CHECK(err.find("string") != std::string::npos);
}

// -- wait on a class property (LRM 9.4.3) ------------------------------------
//
// This is the shape that failed WORST before change notifiers landed. A `wait`
// lowers to `forever { if (cond) break; @(read set of cond); }`, and the read
// set is built from SIGNALS. A class property is heap storage inside an object
// and is not a signal, so the read set came back empty, the `@()` had no
// sensitivity, and the process parked on something nothing could ever trigger:
// a permanent hang with no diagnostic at any stage. Verilator 5.050 prints
// `woke at 1`; iverilog 13 at least fails loudly on the same root cause
// ("Empty NexusSet from wait expression").

// The bare form: `wait(p)` on the enclosing `this`.
TEST_CASE("wait on a bare class property resumes when it changes") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                bit on;
                task w(); wait(on); $display("woke %0t", $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w(); join_none
                #1 c.on = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 1\n");
}

// Through a handle, from outside the class: the uvm_phase_hopper.svh:498 shape
// (`wait (phase.m_premature_end)`). The object expression is lowered INSIDE the
// loop body, so it is re-read on every pass.
TEST_CASE("wait on a class property through a handle resumes when it changes") {
    constexpr std::string_view src = R"(
        module m;
            class C; bit on; endclass
            C c;
            initial begin
                c = new;
                fork begin wait(c.on); $display("woke %0t", $time); end join_none
                #1 c.on = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 1\n");
}

// A condition mixing a class property with an ordinary module signal takes its
// sensitivity from both, and re-tests the whole expression on each wake. Only
// the second change makes it true, so the resume time is what proves both
// wake sources are live. (Verilator 5.050 aborts on this design with "Null
// pointer dereferenced" -- it resolves the handle too early, the same class of
// bug clotho had for `@(c.on)`.)
TEST_CASE("wait on a class property and a module signal wakes from either") {
    constexpr std::string_view src = R"(
        module m;
            class C; bit on; endclass
            C c;
            logic g = 0;
            initial begin
                c = new;
                fork begin wait(c.on && g); $display("mix %0t", $time); end join_none
                #1 c.on = 1;
                #1 g = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "mix 2\n");
}

// Properties of TWO different objects in one condition: two notifiers, two
// object handles, one wait.
TEST_CASE("wait on properties of two objects wakes when both are set") {
    constexpr std::string_view src = R"(
        module m;
            class C; bit on; endclass
            C x, y;
            initial begin
                x = new; y = new;
                fork begin wait(x.on && y.on); $display("two %0t", $time); end join_none
                #1 x.on = 1;
                #1 y.on = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "two 2\n");
}

// The object need not be a variable: an associative-array element keyed by a
// class handle works, which is the uvm_objection.svh shape inside a `wait`.
TEST_CASE("wait on a property of an assoc-array element resumes when it changes") {
    constexpr std::string_view src = R"(
        module m;
            class K; int id; endclass
            class E; bit raised; endclass
            E m_events[K];
            K k;
            initial begin
                k = new;
                m_events[k] = new;
                fork begin wait(m_events[k].raised); $display("woke %0t", $time); end join_none
                #1 m_events[k].raised = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 1\n");
}

// A property whose type carries neither an old -> new WORD to classify nor a
// mutation surface gets no notifier, so there is nothing to park on. It must be
// REFUSED: lowering it anyway is what produced the silent permanent park this
// whole feature exists to remove. `wait(0)` is unaffected -- it reads no property
// at all and keeps the documented LRM 9.4.3 behaviour (the case above).
//
// An unpacked struct is what is left after the scalar rung: comparing one needs a
// walk over its leaves, not a word compare, which is a different mechanism.
TEST_CASE("wait on a class property with no change notifier fails loudly") {
    const std::string err = lower_error(R"(
        module m;
            typedef struct { int a; int b; } pair_t;
            class C;
                pair_t p;
                task w(); wait(p.a == 1); $display("x"); endtask
            endclass
            C c;
            initial begin c = new; c.w(); end
        endmodule
    )");
    CHECK(err.find("no change notifier") != std::string::npos);
}

// -- wait on a CONTAINER class property (LRM 9.4.3) --------------------------
//
// The UVM blocker: uvm_queue#(T)::wait_until_not_empty (uvm_queue.svh:218) is
// `wait(queue.size() > 0)` on a `protected T queue[$]`. The value the condition
// reads is the container's CONTENTS, which no store classifies -- `q.size()` is
// computed, not stored -- so the notifier fires on MUTATION instead.
//
// That is sound only because `wait` is level-sensitive: it re-tests and parks
// again, so a fire that leaves the condition false costs one re-test. Every case
// below asserts the resume TIME, not merely that it resumed, because a mutation
// path whose notify is missing does not fail loudly -- it hangs, or resumes at
// whatever later mutation happens to fire. `@` over the same expression is
// refused (see the last case): a mutation is not a change in value.

// The UVM shape itself, parameterized exactly as uvm_queue is.
TEST_CASE("wait on a queue class property resumes when a producer pushes") {
    constexpr std::string_view src = R"(
        module m;
            class mock_queue #(type T = int);
                protected T queue[$];
                virtual task wait_until_not_empty();
                    wait(queue.size() > 0);
                endtask
                virtual function void push_back(T v);
                    queue.push_back(v);
                endfunction
            endclass
            mock_queue #(int) q;
            initial begin
                q = new;
                fork begin q.wait_until_not_empty(); $display("not empty %0t", $time); end join_none
                #10 q.push_back(7);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "not empty 10\n");
}

// One case per mutation op of LRM 7.10 / 7.5 / 7.8, each timed so that removing
// that op's notify hangs the design rather than shifting a print. They share one
// design because the interesting content is the op, not the scaffolding: each
// waiter is forked before its own mutation and must resume in the step that
// mutation lands in.
TEST_CASE("every queue mutation op wakes a waiter on the property") {
    constexpr std::string_view src = R"(
        module m;
            class C; int q[$]; endclass
            C a, b, c, d, e, f, g;
            int x;
            initial begin
                a = new;
                fork begin wait(a.q.size() > 0); $display("push_back %0t", $time); end join_none
                #1 a.q.push_back(1);

                b = new;
                fork begin wait(b.q.size() > 0); $display("push_front %0t", $time); end join_none
                #1 b.q.push_front(1);

                c = new; c.q.push_back(1); c.q.push_back(2);
                fork begin wait(c.q.size() < 2); $display("pop_front %0t", $time); end join_none
                #1 x = c.q.pop_front();

                d = new; d.q.push_back(1); d.q.push_back(2);
                fork begin wait(d.q.size() < 2); $display("pop_back %0t", $time); end join_none
                #1 x = d.q.pop_back();

                e = new;
                fork begin wait(e.q.size() > 0); $display("insert %0t", $time); end join_none
                #1 e.q.insert(0, 7);

                f = new; f.q.push_back(1); f.q.push_back(2);
                fork begin wait(f.q.size() < 2); $display("delete_idx %0t", $time); end join_none
                #1 f.q.delete(0);

                g = new; g.q.push_back(1);
                fork begin wait(g.q.size() == 0); $display("delete_all %0t", $time); end join_none
                #1 g.q.delete();
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) ==
          "push_back 1\npush_front 2\npop_front 3\npop_back 4\ninsert 5\ndelete_idx 6\ndelete_all 7\n");
}

// An element write is a mutation of the property even though it changes no
// handle: the notifier travels onto the element place so the store fires it.
// `q.sort()` is here too, as the reordering half of the surface -- it changes
// q[0] while leaving q.size() alone, so only an element-reading waiter sees it.
TEST_CASE("a queue element write and a reorder wake a waiter on the element") {
    constexpr std::string_view src = R"(
        module m;
            class C; int q[$]; endclass
            C a, b;
            initial begin
                a = new; a.q.push_back(0);
                fork begin wait(a.q[0] == 5); $display("elem %0t", $time); end join_none
                #1 a.q[0] = 5;

                b = new; b.q.push_back(2); b.q.push_back(1);
                fork begin wait(b.q[0] == 1); $display("sort %0t", $time); end join_none
                #1 b.q.sort();
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "elem 1\nsort 2\n");
}

// A whole-container assignment replaces the contents wholesale (LRM 7.6).
TEST_CASE("assigning a whole queue into a class property wakes its waiter") {
    constexpr std::string_view src = R"(
        module m;
            class C; int q[$]; endclass
            C c;
            int src_q[$];
            initial begin
                c = new;
                src_q.push_back(9);
                fork begin wait(c.q.size() > 0); $display("assign %0t", $time); end join_none
                #1 c.q = src_q;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "assign 1\n");
}

// Dynamic array (LRM 7.5): `new[]`, an element write, and `delete`.
TEST_CASE("dynamic-array class property mutations wake their waiters") {
    constexpr std::string_view src = R"(
        module m;
            class C; int d[]; endclass
            C a, b, c;
            initial begin
                a = new;
                fork begin wait(a.d.size() > 0); $display("new %0t", $time); end join_none
                #1 a.d = new[3];

                b = new; b.d = new[2];
                fork begin wait(b.d[1] == 9); $display("elem %0t", $time); end join_none
                #1 b.d[1] = 9;

                c = new; c.d = new[2];
                fork begin wait(c.d.size() == 0); $display("delete %0t", $time); end join_none
                #1 c.d.delete();
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "new 1\nelem 2\ndelete 3\n");
}

// Associative array (LRM 7.8): an element write CREATES an entry, so it must fire
// even when the stored value equals what an absent key would have read back --
// `a.num()` changed. `delete(key)` is the other half.
TEST_CASE("associative-array class property mutations wake their waiters") {
    constexpr std::string_view src = R"(
        module m;
            class C; int a[int]; endclass
            C x, y;
            initial begin
                x = new;
                fork begin wait(x.a.num() > 0); $display("write %0t", $time); end join_none
                #1 x.a[3] = 0;

                y = new; y.a[1] = 1;
                fork begin wait(y.a.num() == 0); $display("delete %0t", $time); end join_none
                #1 y.a.delete(1);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "write 1\ndelete 2\n");
}

// A `ref` formal is a raw aliased pointer the callee mutates and never copies
// back, so the notify rides the call's own after-write bracket and lands at the
// RETURN. One fire per call rather than one per mutation inside it, which a level
// wait absorbs.
TEST_CASE("a queue class property mutated through a ref argument wakes its waiter") {
    constexpr std::string_view src = R"(
        module m;
            class C; int q[$]; endclass
            function automatic void fill(ref int z[$]); z.push_back(1); endfunction
            C c;
            initial begin
                c = new;
                fork begin wait(c.q.size() > 0); $display("ref %0t", $time); end join_none
                #1 fill(c.q);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "ref 1\n");
}

// A base-class method writes the inherited property at its BASE-relative field
// index, so this only works while the notifier sits immediately after its
// property and the base's flattened table stays a prefix of the derived class's.
TEST_CASE("a base-class method mutating an inherited queue wakes a derived waiter") {
    constexpr std::string_view src = R"(
        module m;
            class B;
                int q[$];
                int pad;
                function void add(int v); q.push_back(v); endfunction
            endclass
            class D extends B;
                int extra;
                task w(); wait(q.size() > 0); $display("base %0t", $time); endtask
            endclass
            D d;
            initial begin
                d = new;
                fork d.w(); join_none
                #1 d.add(3);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "base 1\n");
}

// A container property and an integral one in the same condition: two notifiers
// of two different kinds on one wait. Only the second change makes it true, so
// the resume time is what proves both are live.
TEST_CASE("wait on a queue property and an integral property wakes from either") {
    constexpr std::string_view src = R"(
        module m;
            class C; int q[$]; bit on; endclass
            C c;
            initial begin
                c = new;
                fork begin wait(c.q.size() > 0 && c.on); $display("mix %0t", $time); end join_none
                #1 c.q.push_back(1);
                #1 c.on = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "mix 2\n");
}

// Containers of two different objects in one condition: two notifiers, two object
// handles, one wait.
TEST_CASE("wait on the queues of two objects resumes when both are non-empty") {
    constexpr std::string_view src = R"(
        module m;
            class C; int q[$]; endclass
            C x, y;
            initial begin
                x = new; y = new;
                fork begin wait(x.q.size() > 0 && y.q.size() > 0); $display("two %0t", $time); end join_none
                #1 x.q.push_back(1);
                #1 y.q.push_back(1);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "two 2\n");
}

// The negative space, which is what a fire-on-mutation notifier has to get right:
// a mutation that leaves the condition FALSE must leave the process parked. A
// push then a pop is two fires and no satisfaction; a delete of an unrelated key
// is one more. Only the last line prints.
TEST_CASE("a mutation that leaves the condition false leaves the waiter parked") {
    constexpr std::string_view src = R"(
        module m;
            class C; int q[$]; int a[int]; endclass
            C c, k;
            int x;
            initial begin
                c = new;
                fork begin wait(c.q.size() > 1); $display("QUEUE %0t", $time); end join_none
                k = new; k.a[1] = 1;
                fork begin wait(k.a.exists(7)); $display("KEY %0t", $time); end join_none
                #1 c.q.push_back(1);
                #1 x = c.q.pop_back();
                #1 k.a.delete(1);
                #1 $display("still parked %0t", $time);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "still parked 4\n");
}

// A condition over a NESTED container roots at the outer property, so mutating an
// inner element notifies through the outer notifier. Nothing extra is needed: the
// element select of a mutating receiver is itself a for-write select.
TEST_CASE("wait on the size of a nested queue wakes on an inner push") {
    constexpr std::string_view src = R"(
        module m;
            class C; int qq[$][$]; endclass
            C c;
            initial begin
                c = new;
                c.qq.push_back('{});
                fork begin wait(c.qq[0].size() > 0); $display("nested %0t", $time); end join_none
                #1 c.qq[0].push_back(5);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "nested 1\n");
}

// LRM 9.4.2: a non-edge implicit event fires on any CHANGE IN THE VALUE of the
// expression. A mutation notifier is a different predicate -- a push followed by
// a pop is two mutations and no change -- so handing `@` the container notifier
// would be an observable WRONG wake, not the harmless extra one `wait` absorbs.
// Refused rather than silently downgraded.
TEST_CASE("an event control on a container class property fails loudly") {
    const std::string err = lower_error(R"(
        module m;
            class C; int q[$]; endclass
            C c;
            initial begin
                c = new;
                fork begin @(c.q.size()); $display("x"); end join_none
                #1 c.q.push_back(1);
                #1 $finish;
            end
        endmodule
    )");
    CHECK(err.find("event control on a container class property") != std::string::npos);
}

// A FIXED unpacked array is not in the notified set: its extent never changes, so
// it has no mutation op, and the element store that would have to fire the
// notifier descends through agg_member, which clears it. Refused, not hung.
TEST_CASE("wait on a fixed unpacked-array class property fails loudly") {
    const std::string err = lower_error(R"(
        module m;
            class C;
                int a[4];
                task w(); wait(a[0] == 1); $display("x"); endtask
            endclass
            C c;
            initial begin c = new; c.w(); end
        endmodule
    )");
    CHECK(err.find("no change notifier") != std::string::npos);
}

// -- wait on a SCALAR class property (LRM 9.4.3) -----------------------------
//
// The third rung: every property type whose value is one thing a single store can
// compare. A class / chandle / virtual interface handle (the value IS the handle,
// so a change is an identity change and `== null` is exactly what the condition
// tests), and real / shortreal (a 2-state IEEE-754 pattern, LRM 6.12 -- no x/z,
// so equality is the whole classification). A string is here too but compares
// CONTENT, not the handle; see its own cases below.
//
// Each case asserts the resume TIME rather than merely that it resumed, because a
// missing notify does not fail loudly -- it hangs, or resumes at whatever later
// store happens to fire.

// The UVM blocker itself: uvm_sequence_base.svh:430 is `wait (m_killing_process ==
// null)` on a class-handle property, inside a fork, with the handle cleared by
// another process. This is the shape the whole rung exists for.
TEST_CASE("wait on a class-handle property resumes when the handle is cleared") {
    constexpr std::string_view src = R"(
        module m;
            class process_mock; int id; endclass
            class sequence_base;
                protected process_mock m_killing_process;
                virtual task wait_for_kill();
                    wait (m_killing_process == null);
                endtask
                virtual function void arm();  m_killing_process = new; endfunction
                virtual function void done(); m_killing_process = null; endfunction
            endclass
            sequence_base s;
            initial begin
                s = new;
                s.arm();
                fork begin s.wait_for_kill(); $display("killed %0t", $time); end join_none
                #10 s.done();
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "killed 10\n");
}

// The same shape the other way round: parking until a handle becomes NON-null,
// which is the `wait (h != null)` half of the same idiom.
TEST_CASE("wait on a class-handle property resumes when the handle is assigned") {
    constexpr std::string_view src = R"(
        module m;
            class T; int x; endclass
            class C; T h; endclass
            C c;
            initial begin
                c = new;
                fork begin wait (c.h != null); $display("set %0t", $time); end join_none
                #4 c.h = new;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "set 4\n");
}

// A real property. LRM 6.12 gives a real no x/z, so there are no edge semantics to
// classify and the notifier is the any-change one; the compare is over the stored
// 64-bit IEEE-754 pattern, exactly as a real SIGNAL store compares.
TEST_CASE("wait on a real class property resumes when the value changes") {
    constexpr std::string_view src = R"(
        module m;
            class C; real r; endclass
            C c;
            initial begin
                c = new;
                fork begin wait (c.r > 2.0); $display("r %0t", $time); end join_none
                #2 c.r = 1.0;
                #2 c.r = 2.5;
                #1 $finish;
            end
        endmodule
    )";
    // The store at t=2 fires the notifier and the level wait re-tests false; the
    // one at t=4 is what satisfies it.
    CHECK(run_capture_finish(src) == "r 4\n");
}

TEST_CASE("wait on a shortreal class property resumes when the value changes") {
    constexpr std::string_view src = R"(
        module m;
            class C; shortreal q; endclass
            C c;
            initial begin
                c = new;
                fork begin wait (c.q == 1.5); $display("q %0t", $time); end join_none
                #3 c.q = 1.5;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "q 3\n");
}

// A string property compares CONTENT (LRM 6.16 / 9.4.2 "any change in the
// value"), through rt_string_cmp, because `obj.s = t` replaces the rt_string's
// buffer -- comparing handles would call every write a change.
TEST_CASE("wait on a string class property resumes when the content changes") {
    constexpr std::string_view src = R"(
        module m;
            class C; string s; endclass
            C c;
            initial begin
                c = new;
                fork begin wait (c.s == "go"); $display("s %0t", $time); end join_none
                #2 c.s = "wait";
                #3 c.s = "go";
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "s 5\n");
}

// A virtual interface handle property (LRM 25.9): assignment, `==`/`!=` and `null`
// are all the type admits, and all of them read the handle word this compares.
TEST_CASE("wait on a virtual interface class property resumes when it is bound") {
    constexpr std::string_view src = R"(
        interface vif_if; logic clk; endinterface
        module m;
            vif_if bi();
            class C; virtual vif_if v; endclass
            C c;
            initial begin
                c = new;
                fork begin wait (c.v != null); $display("v %0t", $time); end join_none
                #6 c.v = bi;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "v 6\n");
}

// A chandle property (LRM 6.14). SystemVerilog gives no way to MAKE a non-null
// chandle without DPI, so what is asserted here is the half that is reachable:
// the property is notifiable (it lowers rather than being refused), an
// already-true condition falls through per LRM 9.4.3, and a null -> null store is
// not a change and does not satisfy a waiter parked on one. The DPI-fed
// transition is covered by the layout case in class_test.
TEST_CASE("wait on a chandle class property is notifiable and sees no false change") {
    constexpr std::string_view src = R"(
        module m;
            class C; chandle ch; int n; endclass
            C c;
            initial begin
                c = new;
                wait (c.ch == null);
                $display("null at %0t", $time);
                fork begin wait (c.ch != null); $display("BAD %0t", $time); end join_none
                #1 c.ch = c.ch;
                #1 c.n = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "null at 0\n");
}

// The notifiers live in the OBJECT, so two instances of one class do not share
// them: a store to one must leave the other's waiter parked. Asserted per type
// because each one reaches the notifier through its own compare.
TEST_CASE("a scalar property store wakes only the written object's waiter") {
    constexpr std::string_view src = R"(
        module m;
            class C; string s; real r; C h; endclass
            C a, b;
            initial begin
                a = new; b = new;
                fork begin wait (a.s == "x"); $display("as %0t", $time); end join_none
                fork begin wait (b.s == "x"); $display("bs %0t", $time); end join_none
                fork begin wait (a.r == 1.0); $display("ar %0t", $time); end join_none
                fork begin wait (b.r == 1.0); $display("br %0t", $time); end join_none
                #1 a.s = "x";
                #1 a.r = 1.0;
                #1 b.s = "x";
                #1 b.r = 1.0;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "as 1\nar 2\nbs 3\nbr 4\n");
}

// Through a `ref` formal, which is a raw aliased pointer the callee writes and
// never copies back, so the notify can only land at the call's return -- the
// existing service_snapshot / service_after_write bracket, which a string reaches
// too. And from a base-class method, which writes the inherited property at its
// BASE-relative field index, so this is also the prefix-layout invariant working.
TEST_CASE("a scalar property written through a ref formal or a base method wakes its waiter") {
    constexpr std::string_view src = R"(
        module m;
            function void set_s(ref string r); r = "done"; endfunction
            class D; int id; endclass
            class B;
                D      h;
                string s;
                function void hit(); h = new; endfunction
            endclass
            class E extends B; int extra; endclass
            E e, f;
            initial begin
                e = new; f = new;
                fork begin wait (e.h != null); $display("base %0t", $time); end join_none
                fork begin wait (f.s == "done"); $display("ref %0t", $time); end join_none
                #1 e.hit();
                #1 set_s(f.s);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "base 1\nref 2\n");
}
