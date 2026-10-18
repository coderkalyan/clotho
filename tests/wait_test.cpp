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

// -- wait on SIGNAL-resident storage: static class properties and module-level
//    containers (LRM 9.4.3) ---------------------------------------------------
//
// A static class property and a module-level container live in module-signal
// storage, not in an object, so neither has the hidden per-object notifier the
// cases above park on. Their waiters park on the SIGNAL's own any-change bucket,
// and two independent defects each left that bucket dead:
//
//   1. The sensitivity walk resolved reads through sigmap_ alone, while a static
//      class property's signal is minted LAZILY at its first VALUE access
//      (declare_static_prop) -- eagerly only with an initializer, and for a
//      module-scope class only after every process body has lowered. A `wait`
//      whose condition was the property's first-lowered reference collected an
//      EMPTY event list: a silent permanent park that appeared and disappeared
//      with method declaration order. The walk now declares the signal itself.
//
//   2. A container mutation op (push_back, an assoc element write, new[n], ...)
//      is not a Store, so the store-side service bracket never saw it, and the
//      property notifier is null for a signal -- no wake at all. The mutation
//      sites now fire the signal's any-change bucket unconditionally, exactly as
//      the property notifier fires: `wait` re-tests, so an extra fire costs one
//      re-test while a missed one is a permanent park.
//
// This is what held every UVM phase open: uvm_objection.svh:805 parks the
// background drain process on `wait (m_scheduled_list.size() != 0)` -- a `local
// static uvm_objection_context_object m_scheduled_list[$]`, initializer-less, so
// its first-lowered reference was the wait itself -- and drop_objection's
// push_back (:707) was an unserviced mutation besides. The drain never ran,
// all_dropped never fired, and every test sat at the 9200s default timeout with
// report_phase never executing. Each case asserts the resume TIME: neither
// defect fails loudly, both hang.

// The uvm_objection shape itself: an initializer-less static queue property, the
// waiter a static task whose wait is the property's first-lowered reference, the
// producer a plain method call ten time units later.
TEST_CASE("wait on a static queue class property resumes when a producer pushes") {
    constexpr std::string_view src = R"(
        module m;
            class objection_mock;
                static int scheduled[$];
                static task background();
                    wait (scheduled.size() != 0);
                    $display("drained %0d at %0t", scheduled.pop_front(), $time);
                endtask
                static function void schedule(int v);
                    scheduled.push_back(v);
                endfunction
            endclass
            initial begin
                fork objection_mock::background(); join_none
                #10 objection_mock::schedule(7);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "drained 7 at 10\n");
}

// Defect 1 in isolation, on a SCALAR: the waiting task is declared before the
// writer, so its wait is lowered -- and its sensitivity collected -- before any
// value access has minted the property's signal. The initializer does not save
// it: a module-scope class's eager pass runs only after every process body.
// Declaring the methods in the opposite order used to hide the hang, which is
// exactly why both orders are pinned here.
TEST_CASE("wait on a static scalar class property is order-independent") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                static int m_val = 0;
                task w_first();  wait (m_val == 5); $display("first %0t", $time); endtask
                function void bump(); m_val = 5; endfunction
            endclass
            class D;
                static int m_val = 0;
                function void bump(); m_val = 5; endfunction
                task w_last();   wait (m_val == 5); $display("last %0t", $time);  endtask
            endclass
            C c;  D d;
            initial begin
                c = new; d = new;
                fork c.w_first(); join_none
                fork d.w_last();  join_none
                #10 c.bump();
                #10 d.bump();
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "first 10\nlast 20\n");
}

// Defect 2 in isolation: the queue is a module-level signal (declared, in
// sigmap_ from Collect), so only the mutation-service half is in play. One
// waiter per mutation-op family, timed so a missing notify hangs rather than
// shifting a print. Verified against Verilator 5.050 (iverilog's class support
// cannot host the static shapes; the module-level ones match it too).
TEST_CASE("module-level container mutations wake a waiter on the signal") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int a[int];
            int d[];
            initial begin
                fork begin wait (q.size() != 0); $display("push %0t", $time);   end join_none
                fork begin wait (a.size() != 0); $display("assoc %0t", $time);  end join_none
                fork begin wait (d.size() != 0); $display("dynnew %0t", $time); end join_none
                #1 q.push_back(1);
                #1 a[3] = 7;
                #1 d = new[2];
                fork begin wait (q.size() == 0); $display("delete %0t", $time); end join_none
                #1 q.delete();
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "push 1\nassoc 2\ndynnew 3\ndelete 4\n");
}

// A `ref` formal bound to a signal container is a raw aliased pointer the callee
// mutates without a copy-back, so the notify can only land at the call's return
// -- the same after-return bracket a waited-on container PROPERTY uses, now also
// taken when the actual is a signal (or a signal-map element).
TEST_CASE("a signal container mutated through a ref formal wakes its waiter") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int a[int];
            task automatic fill_q(ref int qq[$]);   qq.push_back(1); endtask
            task automatic fill_a(ref int aa[int]); aa[5] = 9;       endtask
            initial begin
                fork begin wait (q.size() != 0); $display("refq %0t", $time); end join_none
                fork begin wait (a.size() != 0); $display("refa %0t", $time); end join_none
                #10 fill_q(q);
                #10 fill_a(a);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "refq 10\nrefa 20\n");
}

// Defect 1's inferred-sensitivity siblings: an `always @*` body and a continuous
// assign RHS reading a static class property collected the same empty read set,
// so the process ran once at time zero and never re-fired. The walk's lazy
// declaration covers every SensitivityCollector user -- `wait`, `@*` /
// always_comb, a continuous assign, an SVA sample -- because they all resolve
// through the same lookup. The assign half matches Verilator 5.050; the `@*`
// half is LRM 9.4.2.2 ("sensitive to every operand"; a static property is a
// variable with module-storage semantics, 6.21/8.9) -- Verilator omits class
// statics from `@*` sensitivity and does not re-fire there.
TEST_CASE("inferred sensitivity re-fires when a static class property changes") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                static int m_val = 0;
                function void bump(); m_val = 5; endfunction
            endclass
            int y, z;
            always @* begin
                y = C::m_val;
                if (y == 5) $display("comb %0t", $time);
            end
            assign z = C::m_val;
            C c;
            initial begin
                c = new;
                #10 c.bump();
                #1 $display("assign z=%0d at %0t", z, $time);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "comb 10\nassign z=5 at 11\n");
}

// -- Waits on fork-captured automatics (LRM 9.3.2 over 9.4.3) -----------------
//
// A fork-captured automatic is ONE variable shared by parent and children
// (9.3.2), and it lives in a GC capture box -- a one-field synthetic class --
// not in a signal (docs/design/fork-join.md V4). A `wait` reading one therefore
// collected no signal at all and parked on an empty event list: a silent
// permanent park, exactly the shape "lower: waits on static class properties and signal-resident containers" fixed for static properties. This
// is the defect that hung the UVM sequencer/driver rendezvous: the driver's
// m_safe_select_item parks on `wait (select_process != null)` where
// select_process is a task automatic a join_none watchdog writes
// (uvm_sequencer_param_base.svh:535), so `finish_item` never returned.
//
// The fix mints the box WITH a hidden change-notifier event (`$chg.$capture`)
// whenever some `wait` in the design reads the symbol, registered on the same
// store-path machinery a waited class property uses, and the wait parks on it.
// Each test asserts the rendezvous COMPLETES at the exact simulated time.

// The minimal easyUVM shape: the parent waits, a join_none child writes.
TEST_CASE("wait on a fork-captured automatic wakes when the child writes it") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                int flag = 0;
                fork begin #5 flag = 7; end join_none
                wait (flag != 0);
                $display("woke %0d at %0t", flag, $time);
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 7 at 5\n");
}

// The literal UVM watchdog shape (uvm_sequencer_param_base::m_safe_select_item):
// a process-typed automatic the child fills with process::self(); the parent's
// wait must see the write in the SAME time step (the child runs when the parent
// parks, LRM 9.3.2), so the wake is at 0.
TEST_CASE("wait on a captured process handle sees the child's self() at time zero") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                process select_process;
                fork
                    begin
                        select_process = process::self();
                        #100; // stays alive past the wait
                    end
                join_none
                wait (select_process != null);
                $display("granted at %0t", $time);
                select_process.kill();
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "granted at 0\n");
}

// The inverse direction: the CHILD waits, the parent writes after the fork.
TEST_CASE("a fork child's wait on a captured automatic wakes on the parent's write") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                int flag = 0;
                fork begin wait (flag != 0); $display("child woke %0d at %0t", flag, $time); end join_none
                #7 flag = 5;
                #1; // let the child report before returning
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "child woke 5 at 7\n");
}

// Sibling children of one fork: one waits, the other writes.
TEST_CASE("a fork child's wait wakes on a sibling child's write") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                int flag = 0;
                fork
                    begin #6 flag = 2; end
                    begin wait (flag == 2); $display("sibling woke at %0t", $time); end
                join
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "sibling woke at 6\n");
}

// A fork-scope declaration in a PROCESS body boxes per entry
// (install_fork_decl_box, design-state home): same hole, same fix.
TEST_CASE("wait on a fork-scope declaration wakes across sibling children") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                fork
                    automatic int k = 0;
                    begin #5 k = 3; end
                    begin wait (k != 0); $display("woke k=%0d at %0t", k, $time); end
                join
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke k=3 at 5\n");
}

// A captured CONTAINER takes the mutation-fired notifier flavour, like a waited
// container property: `q.size()` is computed, not stored, so the push itself
// must fire.
TEST_CASE("wait on a captured queue wakes when the child pushes") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                int q[$];
                fork begin #9 q.push_back(42); end join_none
                wait (q.size() != 0);
                $display("woke size=%0d head=%0d at %0t", q.size(), q[0], $time);
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke size=1 head=42 at 9\n");
}

// A captured string's value is its CONTENT (LRM 6.16): the store compares
// content, so a same-content rewrite fires nothing and a real change wakes.
TEST_CASE("wait on a captured string wakes when the content changes") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                string s = "";
                fork begin #4 s = "go"; end join_none
                wait (s == "go");
                $display("woke %s at %0t", s, $time);
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke go at 4\n");
}

// join_any detaches like join_none for the un-joined remainder; the box and its
// notifier outlive the parent's continuation.
TEST_CASE("wait in a join_any child wakes on the sibling's write") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                int flag = 0;
                fork
                    begin wait (flag != 0); $display("woke at %0t", $time); end
                    begin #3 flag = 1; #1; end
                join_any
            endtask
            initial begin t(); #10 $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke at 3\n");
}

// A write through a `ref` formal reaches the box by the aliased-pointer bracket,
// so the notify lands at the call's return -- the same after-return arm a
// signal / property actual takes.
TEST_CASE("a captured automatic written through a ref formal wakes its waiter") {
    constexpr std::string_view src = R"(
        module m;
            task automatic bump(ref int x); #5 x = 9; endtask
            task automatic t;
                int flag = 0;
                fork begin bump(flag); end join_none
                wait (flag == 9);
                $display("woke %0d at %0t", flag, $time);
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 9 at 5\n");
}

// A compound condition over two captured locals: the first write wakes and
// re-tests false (LRM 9.4.3), the second completes it.
TEST_CASE("wait over two captured automatics re-tests until both are written") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                int a = 0; int b = 0;
                fork
                    begin #3 a = 1; end
                    begin #8 b = 1; end
                join_none
                wait (a == 1 && b == 1);
                $display("woke at %0t", $time);
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke at 8\n");
}

// A captured automatic of a type with no change notifier (an unpacked struct)
// is refused loudly, like the class-property arm: a silent permanent park is
// the defect this whole section exists to prevent.
TEST_CASE("wait on a captured unpacked struct fails loudly") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int a; } st;
            task automatic t;
                st x;
                fork begin #2 x.a = 1; end join_none
                wait (x.a != 0);
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(lower_error(src).find("fork-captured automatic 'x' of type m.st is not supported") != std::string::npos);
}

// A captured event has no notifier for `.triggered` either; the refusal names the
// event rather than describing aggregate types it is not.
TEST_CASE("wait on a captured event's triggered fails loudly") {
    constexpr std::string_view src = R"(
        module m;
            task automatic t;
                event e;
                fork begin #1 -> e; end join_none
                wait (e.triggered);
            endtask
            initial begin t(); $finish; end
        endmodule
    )";
    CHECK(lower_error(src).find("fork-captured automatic 'e' of type event is not supported") !=
          std::string::npos);
}
