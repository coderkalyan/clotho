#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"
#include "ir/SimConfig.h"
#include "runtime/Gc.h"
#include "runtime/Runtime.h"

using namespace clotho;

namespace {

// JIT-run a design and return its stdout (the harness of class_test.cpp and
// comb_fusion_test.cpp, unchanged). In-process rather than through
// Subprocess.h, for two reasons: the child mode test_main.cpp implements takes
// no configuration, so a subprocess run can select neither the stress cadence nor
// a nursery size; and the collector's own counters are read back out of the
// clotho_runtime this executable links, which only works if the design ran here.
//
// Every design in this file therefore ends by running out of events, never at
// $finish. That is not stylistic: $finish longjmps out of live coroutine frames
// (Subprocess.h, cooperative-finish.md:24-28) and a collector holding a frame
// registry across that inherits the corruption directly -- abandoned frames are
// never unlinked (garbage-collection.md §6.3). A $finish design here would be
// testing the hazard, not the collector.
std::string run_capture(std::string_view source, ir::SimulationConfig config) {
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

    std::rewind(tmp);
    std::string out;
    char        buf[4096];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return out;
}

// One Immix block (Gc.h:36). The shipped default is 8 MiB
// (SimConfig.h `gc_nursery_bytes`) and no design in this file allocates a tenth
// of that, so under the default the ordinary cadence would reach every safepoint
// with the budget still unspent, never collect, and every case below would pass
// while testing nothing.
//
// Not smaller: rt_gc_init floors nursery_bytes at one block, so asking for less
// silently gets this anyway. The budget-path case therefore paces its
// expectations off bytes actually allocated, not off the number configured here.
constexpr uint64_t nursery_bytes_small = RT_GC_BLOCK_BYTES;

// Nonzero `gc_full_every` means "one full collection per N collections"
// (Gc.h:241-244), so a value no design in this file can reach selects a
// YOUNG-ONLY run. That is the only configuration in which "a young collection
// kept this alive" is a statement about the young path: under stress young and
// full alternate, so a full collection traces the whole heap a safepoint or two
// after any store and would mask a missing write barrier. The largest design
// below collects a few hundred times.
constexpr uint32_t full_every_never = 1u << 24;

// The two cadences, named because `run_stats(src, expected, true)` says nothing
// about which one it selected. Ordinary is what every run gets: a collection is
// due once the nursery budget is spent. Stress is `--gc-stress`: a collection at
// every safepoint. There is no third choice -- the collector itself is not
// optional (garbage-collection.md §12).
constexpr bool cadence_ordinary = false;
constexpr bool cadence_stress   = true;

ir::SimulationConfig gc_cfg(bool stress, uint64_t nursery_bytes = nursery_bytes_small,
                            uint32_t full_every = 0) {
    ir::SimulationConfig cfg;
    cfg.gc_stress        = stress;
    cfg.gc_nursery_bytes = nursery_bytes;
    cfg.gc_full_every    = full_every;
    return cfg;
}

// The primary oracle for the whole file. LRM 8.4 gives an object no destructor
// and no `delete`, so a program cannot observe reclamation: both cadences must
// print the same bytes, and a collector that changes observable behaviour is a
// bug by definition (garbage-collection.md §13.1). Stress is the cadence that
// bites -- it collects at every safepoint, so a root the collector does not know
// about turns from a rare corruption into a deterministic wrong answer.
//
// Read the strength of this honestly. The reasoning above still holds for the
// two cadences, but its strongest leg is gone: the collector is unconditional,
// so there is no uncollected run left to put on the other side of the
// comparison. This shows that two collection SCHEDULES agree, not that
// collecting agrees with not collecting, and a bug that hits both cadences
// equally -- a mark that misses a root whenever it runs, a finalizer that frees
// a buffer still in use -- prints the same bytes twice and passes here.
//
// What catches that is inside the cases, not in this comparison. §2's `Junk`
// filler hands a wrongly-freed line straight back out overwritten with
// 0x5A5A5A5A, so a read of a collected object sees the pattern rather than
// stale-but-intact bytes; the `string` property on the object under test turns
// the same mistake into an AddressSanitizer heap-use-after-free under
// -DCLOTHO_SANITIZE=ON. Both are absolute -- neither needs a second run to
// disagree with.
void check_both_cadences(std::string_view source, const std::string& expected) {
    CHECK(run_capture(source, gc_cfg(cadence_ordinary)) == expected);
    CHECK(run_capture(source, gc_cfg(cadence_stress)) == expected);
}

// Run one cadence and read the collector's counters back. rt_gc_init zeroes
// rt_gc_stats and rt_gc_shutdown leaves it alone, so what is read here belongs
// to exactly this run, and `objects_live` / `bytes_live` hold what the LAST
// collection computed rather than a teardown recount. Two consequences every
// caller below depends on:
//
//   - a run with no collection reports objects_live == 0, so every reclamation
//     bound is paired with a `collections` assertion; without that pair the
//     bound passes vacuously on a collector that never ran.
//   - `finalized` counts finalizers run by a COLLECTION only; the teardown sweep
//     reports separately and does not touch the struct. That is what separates
//     "this garbage was reclaimed while the design ran" from "the process
//     exited", and it is the whole cycle-collection oracle.
//
// `full_every` selects the collection KIND policy for the run and defaults to 0,
// which is "let the runtime choose"; the generational cases in §7 pass
// full_every_never to get a run with no full collection in it at all.
rt_gc_stats run_stats(std::string_view source, const std::string& expected, bool stress,
                      uint64_t nursery_bytes = nursery_bytes_small, uint32_t full_every = 0) {
    CHECK(run_capture(source, gc_cfg(stress, nursery_bytes, full_every)) == expected);
    rt_gc_stats st = {};
    rt_gc_get_stats(&st);
    return st;
}

} // namespace

// -- 1. Observational identity ------------------------------------------------
//
// There is no differential oracle for a collector (garbage-collection.md §13):
// iverilog cannot check something the LRM makes unobservable. The substitute is
// that the class operations keep working under a heap that is being swept
// underneath them. Each expected string here is the behaviour class_test.cpp
// already pins for the same construct; what these cases add is that it survives
// both collection cadences byte for byte.

// Construction, inheritance, virtual dispatch through a base handle, a handle
// returned out of a subroutine and passed into another, and a down-cast that
// reads the object's own class from field 0 (LRM 8.4/8.20, $cast at
// ir/llvm/Inst.cpp, lower_inst). The GC word sits BELOW the vptr precisely so
// none of this arithmetic moves (Gc.h:41-46) -- if it did, `who()` would
// dispatch through the header and $cast would read the wrong display.
TEST_CASE("gc: identity - construction, inheritance, virtual dispatch, $cast") {
    constexpr std::string_view src = R"(
        module m;
            class Base;
                int    id;
                string kind;
                virtual function string who(); return "base"; endfunction
            endclass
            class Derived extends Base;
                int extra;
                virtual function string who(); return "derived"; endfunction
            endclass
            function automatic Base make(input int id);
                Derived d;
                d = new;
                d.id    = id;
                d.extra = id * 2;
                d.kind  = "made";
                return d;
            endfunction
            function automatic int read_id(input Base b);
                return b.id;
            endfunction
            initial begin
                Base    b;
                Derived d;
                int     r;
                b = make(7);
                $display("%s %0d %0d", b.who(), b.id, read_id(b));
                r = $cast(d, b);
                $display("%0d %0d %s", r, d.extra, d.kind);
            end
        endmodule
    )";
    check_both_cadences(src, "derived 7 7\n1 14 made\n");
}

// Shallow copy (LRM 8.12): value properties are independent in the copy, handle
// properties are SHARED. Both halves are load-bearing for a collector -- the
// copy must be a distinct object with its own header, and the shared `peer` must
// be one object with two referents, not two objects. `new src` allocates without
// running the constructor, which is the allocation path most likely to forget
// the header.
TEST_CASE("gc: identity - shallow copy shares handles and separates values") {
    constexpr std::string_view src = R"(
        module m;
            class Bag;
                int    n;
                string label;
                Bag    peer;
            endclass
            function automatic Bag clone_of(input Bag src);
                Bag c;
                c = new src;
                return c;
            endfunction
            initial begin
                Bag a, b, c;
                a = new; a.n = 1; a.label = "a";
                b = new; b.n = 2; b.label = "b";
                a.peer = b;
                c = clone_of(a);
                c.n      = 99;
                c.label  = "c";
                c.peer.n = 42;
                $display("%0d %s %0d %s %0d %b", a.n, a.label, c.n, c.label, b.n, c.peer == b);
            end
        endmodule
    )";
    check_both_cadences(src, "1 a 99 c 42 1\n");
}

// The same operations with time in them: a blocking task that allocates and
// suspends, an always-process that allocates per edge, and a process that loops
// over both. Every #delay and every edge is a safepoint (garbage-collection.md
// §6.1), so under stress this design is collected several times per iteration
// while handles are live in three different frames at once.
TEST_CASE("gc: identity - allocation across suspensions in a process and a task") {
    constexpr std::string_view src = R"(
        module m;
            class Tick;
                int    seq;
                string tag;
            endclass
            bit  clk;
            Tick last;
            int  seen = 0;
            task automatic hold(input int k);
                Tick t;
                t = new;
                t.seq = k;
                t.tag = "task";
                #3;
                seen = seen + t.seq;
            endtask
            initial begin
                clk = 0;
                for (int i = 0; i < 24; i++) #5 clk = ~clk;
            end
            always @(posedge clk) begin
                last = new;
                last.seq = seen;
                last.tag = "always";
            end
            initial begin
                for (int i = 1; i <= 4; i++) begin
                    hold(i);
                    @(posedge clk);
                end
                $display("%0d %s", seen, last.tag);
            end
        endmodule
    )";
    check_both_cadences(src, "10 always\n");
}

// -- 2. Reachability through every root class ---------------------------------
//
// The use-after-free cases. Each holds exactly one object through a window in
// which the collector runs, reachable through one root class and no other, and
// then reads it. Two shared devices make these fail loudly instead of quietly:
//
//   - a `Junk` class whose body has the SAME shape as the object under test, and
//     a process that allocates and drops it continuously during the window. A
//     line freed by a wrong collection is handed straight back out and
//     overwritten with 0x5A5A5A5A, so the read sees the filler pattern instead
//     of its own stale-but-intact bytes. Without the filler these cases pass on
//     a collector with no roots at all.
//   - a `string` property on the object under test. Its buffer is real malloc'd
//     memory freed by the emitted finalizer (Gc.h:80-84), so under
//     -DCLOTHO_SANITIZE=ON a wrongly-collected object turns the %s into an
//     AddressSanitizer heap-use-after-free rather than a text diff.

// A static class property is ONE slot with module-signal storage
// (declare_static_prop, lower/Types.cpp) and reaches the collector through
// the emitted clotho.gcroots table (garbage-collection.md §5). The object is
// allocated inside a static METHOD and never touched by the process, so no
// coroutine frame ever holds the handle -- the precise root table is the only
// thing that can keep it alive, and a conservative frame hit cannot mask a
// missing table entry.
TEST_CASE("gc: an object reachable only from a static class property survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Registry;
                static Cell slot;
                static function void install(input int a);
                    slot = new;
                    slot.v   = a;
                    slot.tag = "static-rooted";
                endfunction
            endclass
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(posedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            initial begin
                Registry::install(91);
                repeat (6) @(posedge clk);
                $display("%0d %s", Registry::slot.v, Registry::slot.tag);
            end
        endmodule
    )";
    check_both_cadences(src, "91 static-rooted\n");
}

// A module-scope handle lives in the design state buffer (@clotho.state), the
// first row of the root table in garbage-collection.md §5. Same construction as
// the static case: allocated in a plain function, whose locals are on the native
// stack and therefore gone at the region boundary where the collection runs
// (§5.1 -- non-coroutine code needs no registration because none of its frames
// exist there).
TEST_CASE("gc: an object reachable only from module-scope design state survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            Cell held;
            Junk sink;
            function automatic void install(input int a);
                held = new;
                held.v   = a;
                held.tag = "design-state-rooted";
            endfunction
            bit clk;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(posedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            initial begin
                install(55);
                repeat (6) @(posedge clk);
                $display("%0d %s", held.v, held.tag);
            end
        endmodule
    )";
    check_both_cadences(src, "55 design-state-rooted\n");
}

// The conservative frame-root path (garbage-collection.md §5.1-5.2). A suspended
// process has no stack: its locals are in a heap coroutine frame whose layout
// CoroSplit chose, reachable only through the intrusive rt_gc_frame node linked
// at the ramp (Gc.h:107-124). Nothing else designates this object -- no design
// state, no static, no other handle -- so if the registry is not linked, not
// scanned, or unlinked too early, the object is freed while the process sleeps
// and the filler overwrites it.
//
// The local MUST be `automatic` inside an `always` block. A declaration in an
// `initial` block is static in SystemVerilog, and clotho gives it storage in the
// design state buffer, where it is a PRECISE root -- an earlier version of this
// case did exactly that and passed with the frame registry never consulted. The
// pins assertion below is what keeps that from happening again silently.
TEST_CASE("gc: an object reachable only from a process suspended on posedge survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            always @(posedge clk) begin
                automatic Cell only = new;
                only.v   = 4242;
                only.tag = "frame-rooted";
                repeat (3) @(posedge clk);
                $display("%0d %s", only.v, only.tag);
            end
        endmodule
    )";
    // Four completed activations: the body consumes its own trigger plus three
    // more posedges before printing, and the clock has 16.
    const std::string expected = "4242 frame-rooted\n4242 frame-rooted\n"
                                 "4242 frame-rooted\n4242 frame-rooted\n";
    check_both_cadences(src, expected);
    // Negative space: the object is designated by nothing precise, so a run that
    // reports no conservative pin did not reach it through the frame registry and
    // this case has stopped testing what it names.
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
    CHECK(st.conservative_pins > 0);
}

// A blocking task is its own coroutine with its own frame (setPresplitCoroutine
// at ir/llvm/Subroutines.cpp, declare_task), so its locals are a SECOND frame that must be in the
// registry while the caller's frame is suspended in the call. A registry that
// only links process ramps and not task ramps passes the case above and fails
// this one. The suspension is a #delay rather than an edge, covering the other
// half of the quiescent-edge set (F1: after rt_wheel_advance returns).
TEST_CASE("gc: an object reachable only from a suspended blocking task survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            Junk sink;
            task automatic hold(input int a);
                Cell only;
                only = new;
                only.v   = a;
                only.tag = "task-frame-rooted";
                #20;
                $display("%0d %s", only.v, only.tag);
            endtask
            initial begin
                for (int i = 0; i < 30; i++) begin
                    #1;
                    for (int k = 0; k < 64; k++) begin
                        sink = new;
                        sink.v   = 32'h5A5A5A5A;
                        sink.tag = "junk";
                    end
                end
            end
            initial hold(77);
        endmodule
    )";
    check_both_cadences(src, "77 task-frame-rooted\n");
}

// The frame-registration predicate, adversarially. `frame_may_hold_gc_ref`
// (ir/llvm/Analyses.cpp) decides whether a coroutine registers its frame at all: a
// process that provably cannot hold a reference pays nothing, which is what keeps
// the collector off the critical path of class-free RTL now that every run has
// one. That makes the predicate soundness-critical in the worst way -- a missed
// door frees a live object -- so each of its five doors
// (ir/llvm/Analyses.cpp, frame_may_hold_gc_ref) gets a shape here that reaches a handle WITHOUT
// the obvious one (a plainly class-typed declared local assigned from `new`):
//
//   r1  the handle arrives only as a function's return value
//   r2  the handle is only read out of module-scope state into a frame local
//   r3  the handle only ever lives inside an aggregate frame local
//   r4  the handle is only reachable through a `ref` formal's activation
//   r5  the handle is held only by a fork child, which is its own coroutine
//
// Every one is held across a suspension while a filler process recycles freed
// lines and poisons them, so a frame the predicate wrongly skips reads back
// filler rather than its own value.
TEST_CASE("gc: the frame-registration predicate misses no way a frame holds a handle") {
    constexpr std::string_view src = R"(
        module m;
            class Box;
                int a, b, c, d;
                function new(int v); a=v; b=v+1; c=v+2; d=v+3; endfunction
                function int sum(); return a+b+c+d; endfunction
            endclass
            typedef struct { int tag; Box bx; } Wrap;
            bit clk = 0;
            Box shared;
            int r1, r2, r3, r4, r5;
            always #1 clk = ~clk;
            always @(negedge clk) begin
                for (int j = 0; j < 300; j++) begin
                    automatic Box k = new(-1);
                    if (k.a == 12345) $display("unreachable");
                end
            end
            function automatic Box make(int v); return new(v); endfunction
            always @(posedge clk) begin
                automatic Box fromfn = make(10);
                @(posedge clk);
                r1 = fromfn.sum();
            end
            always @(posedge clk) begin
                automatic Box loc = shared;
                @(posedge clk);
                if (loc != null) r2 = loc.sum();
            end
            always @(posedge clk) begin
                automatic Wrap w;
                w.tag = 1;
                w.bx  = new(20);
                @(posedge clk);
                r3 = w.bx.sum();
            end
            task automatic bump(ref int slot, Box keepalive);
                #2;
                slot = keepalive.sum();
            endtask
            always @(posedge clk) begin
                automatic Box t = new(30);
                bump(r4, t);
            end
            always @(posedge clk) begin
                fork
                    begin
                        automatic Box f = new(40);
                        #3;
                        r5 = f.sum();
                    end
                join
            end
            initial begin
                shared = new(50);
                #40;
                $display("%0d %0d %0d %0d %0d", r1, r2, r3, r4, r5);
                $finish;
            end
        endmodule
    )";
    const std::string expected = "46 206 86 126 166\n";
    check_both_cadences(src, expected);
    // As in the posedge case: no precise root designates r1/r3/r4/r5's objects, so
    // a run with no conservative pin did not reach them through a frame at all.
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
    CHECK(st.conservative_pins > 0);
}

// Worklist correctness. 256 links hang off one design-state handle, so marking
// must traverse the whole chain from a single root: the mark phase is an
// explicit bounded worklist and never recursion (TigerStyle,
// garbage-collection.md §13), and a worklist with a fixed cap, a lost
// grey-object push, or a depth limit collects the tail of this chain. The walk
// is bounded at 4096 so a chain corrupted into a ring reports a wrong sum
// instead of hanging the suite (TigerStyle).
TEST_CASE("gc: an object reachable only through a deep handle chain survives") {
    constexpr std::string_view src = R"(
        module m;
            class Link; int v; string tag; Link next; endclass
            class Junk; int v; string tag; endclass
            Link head;
            Junk sink;
            function automatic void build(input int n);
                Link cur;
                for (int i = 0; i < n; i++) begin
                    cur = new;
                    cur.v    = i;
                    cur.tag  = "link";
                    cur.next = head;
                    head     = cur;
                end
            endfunction
            function automatic int walk();
                Link cur;
                int  s = 0;
                cur = head;
                for (int i = 0; i < 4096 && cur != null; i++) begin
                    s   = s + cur.v;
                    cur = cur.next;
                end
                return s;
            endfunction
            bit clk;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(posedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            initial begin
                build(256);
                repeat (6) @(posedge clk);
                $display("%0d", walk());
            end
        endmodule
    )";
    check_both_cadences(src, "32640\n");
}

// A `ref` formal bound to a class property is a raw INTERIOR pointer into the
// object body, carrying clotho.ref.noepoch -- a compile-time-zero epoch that can
// never signal invalidation (F6, ir/llvm/StateLayout.cpp, ref_actual_arg). The task nulls
// the only precise root before suspending, so across the #20 the object is
// designated by nothing but that interior pointer, and keeping it alive is
// exactly rt_gc_find_object's interior resolution (Gc.h:244-250): a validator
// that only accepts object starts frees the object here.
//
// `v` is deliberately NOT the first property. rt_gc_find_object resolves an
// address strictly inside [start, start + size), so a ref to the property at
// offset 0 is bit-identical to the handle and would be resolved by any
// object-start check at all -- the case would look like it covered interior
// resolution while covering nothing. `head` pushes `v` off the object start.
//
// Honest limit: the caller's own frame is scanned conservatively, so if the
// backend spills the base handle while forming the argument, that spill is a
// second (ambiguous) root, and that one IS an object start. The case cannot
// prove which of the two saved the object; it can only fail loudly if neither
// does.
TEST_CASE("gc: an object reachable only through a ref into a class property survives") {
    constexpr std::string_view src = R"(
        module m;
            class Box;  int head; int v; string tag; endclass
            class Junk; int head; int v; string tag; endclass
            Box  boxed;
            Junk sink;
            int  observed = 0;
            function automatic void install(input int a);
                boxed = new;
                boxed.head = 32'h0BADF00D;
                boxed.v    = a;
                boxed.tag  = "ref-rooted";
            endfunction
            task automatic bump(ref int r);
                boxed = null;
                #20;
                r        = r + 1;
                observed = r;
            endtask
            initial begin
                for (int i = 0; i < 30; i++) begin
                    #1;
                    for (int k = 0; k < 64; k++) begin
                        sink = new;
                        sink.head = 32'h5A5A5A5A;
                        sink.v    = 32'h5A5A5A5A;
                        sink.tag  = "junk";
                    end
                end
            end
            initial begin
                install(41);
                bump(boxed.v);
                $display("%0d", observed);
            end
        endmodule
    )";
    check_both_cadences(src, "42\n");
}

// -- 3. Containers of class handles -------------------------------------------
//
// P4's subject (garbage-collection.md §3.3). A container's payload is malloc'd,
// not GC-heap memory, so no root path the collector had before this phase can
// reach the handles inside it: a precise root table describes state-buffer words,
// and a conservative frame scan finds only a pointer to a ring that
// rt_gc_find_object correctly rejects because it is not in a block. Handle-bearing
// containers are therefore walked precisely, from their handle through their
// element descriptor's new GC fields (Runtime.h:909-932).
//
// The question every case below is built around is which containers are ROOTS,
// which is decided purely by where the container's own storage lives
// (Gc.h:234-244):
//
//   design state, static class property  ->  a root, in the static table
//   frame local                          ->  a root while the activation lives,
//                                            rt_gc_container_add / _remove
//   class property                       ->  NOT a root: traced through the
//                                            owning object's rt_gc_map leaves
//
// One clotho-specific fact decides which row a declared local lands in, and it is
// not the `automatic` keyword: declare_local gives every procedural local static,
// signal-like storage (lower/Types.cpp), and a process has no frame at all --
// "frame_slots_.clear(); // a process has no frame; deinit sees only temps"
// (ir/llvm/Subroutines.cpp, lower_group). So a container declared in an `always` block is a
// SignalSlot in the first row, and only a task's or function's locals, which do get
// real frame slots (build_frame_layout, ir/llvm/Subroutines.cpp), take the
// second.
//
// Both directions of getting that wrong are bugs, and they fail differently. A
// root that is missing frees live objects, so those cases run the §2 devices --
// the Junk filler that recycles and poisons freed lines, and a `string` property
// that turns a wrong collection into an ASan heap-use-after-free rather than a
// value diff. A class-property container wrongly registered as a root leaks its
// whole subtree forever instead, which no output can show, so that direction is
// asserted on rt_gc_stats.

// A module-scope `Cell q[$]` puts the rt_queue itself in the design state buffer,
// so its storage is a root -- but the handles live in the malloc'd ring, one
// indirection past everything the §5 root table describes. Nothing else
// designates these four objects: they are allocated and pushed inside a plain
// function, whose locals are on the native stack and therefore gone at the region
// boundary where the collection runs (§5.1). A collector that registers
// state-resident containers but never walks one, or walks only q[0], hands the
// freed line straight back to the filler and reads 0x5A5A5A5A here.
TEST_CASE("gc: objects reachable only from a queue in design state survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            Cell q[$];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                Cell c;
                for (int i = 0; i < 4; i++) begin
                    c = new;
                    c.v   = base + i;
                    c.tag = "queue-root";
                    q.push_back(c);
                end
            endfunction
            function automatic int total();
                int s = 0;
                foreach (q[i]) s = s + q[i].v;
                return s;
            endfunction
            initial begin
                install(11);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s %s", q.size(), total(), q[0].tag, q[3].tag);
            end
        endmodule
    )";
    const std::string expected = "4 50 queue-root queue-root\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// The same shape through a dynamic array, whose payload is a separate malloc'd
// block with its own descriptor (rt_elem_dynarr, Runtime.h:902). A walker written
// for the queue's ring and never wired to rt_dynarr passes the case above and
// frees every object here.
TEST_CASE("gc: objects reachable only from a dynamic array in design state survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            Cell d[];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                Cell c;
                d = new[4];
                for (int i = 0; i < 4; i++) begin
                    c = new;
                    c.v   = base + i;
                    c.tag = "dynarr-root";
                    d[i]  = c;
                end
            endfunction
            function automatic int total();
                int s = 0;
                foreach (d[i]) s = s + d[i].v;
                return s;
            endfunction
            initial begin
                install(21);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s %s", d.size(), total(), d[0].tag, d[3].tag);
            end
        endmodule
    )";
    const std::string expected = "4 90 dynarr-root dynarr-root\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// And through an associative array, which is the one the other two walkers cannot
// be reused for: element payloads are one SlotVec heap block per entry and
// unordered_dense relocates them on rehash (runtime/Containers.cpp, AssocMap), so the walk
// has to go through rt_assoc_visit (Runtime.h:939-941) rather than over a
// contiguous span. A walker that treats the map as an array of stride-sized slots
// reads freed or unrelated memory and marks nothing that is here.
TEST_CASE("gc: objects reachable only from an associative array in design state survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            Cell map[string];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                Cell c;
                for (int i = 0; i < 4; i++) begin
                    c = new;
                    c.v   = base + i;
                    c.tag = "assoc-root";
                    map[{"k", string'(48 + i)}] = c;
                end
            endfunction
            function automatic int total();
                int s = 0;
                foreach (map[k]) s = s + map[k].v;
                return s;
            endfunction
            initial begin
                install(31);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s", map.num(), total(), map["k0"].tag);
            end
        endmodule
    )";
    const std::string expected = "4 130 assoc-root\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// The other root storage class: a STATIC class property, which is its own storage
// block rather than the design state buffer (declare_static_prop,
// lower/Classes.cpp, lower_class_type) and reaches the collector through its own table entry. A
// container-root table emitted only for @clotho.state passes the three cases above
// and frees everything the UVM factory holds, which is exactly this shape.
TEST_CASE("gc: objects reachable only from a container in a static class property survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Factory;
                static Cell pool[string];
                static function void install(input int base);
                    Cell c;
                    for (int i = 0; i < 4; i++) begin
                        c = new;
                        c.v   = base + i;
                        c.tag = "static-assoc-root";
                        pool[{"p", string'(48 + i)}] = c;
                    end
                endfunction
                static function int total();
                    int s = 0;
                    foreach (pool[k]) s = s + pool[k].v;
                    return s;
                endfunction
            endclass
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            initial begin
                Factory::install(41);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s", Factory::pool.num(), Factory::total(),
                         Factory::pool["p0"].tag);
            end
        endmodule
    )";
    const std::string expected = "4 170 static-assoc-root\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// A container declared inside a process body. `automatic` does not put it in a
// coroutine frame -- per the header above it is signal storage, so it belongs to
// the static container table exactly like the module-scope cases. What `automatic`
// does buy is a per-entry re-default at the declaration point, which frees the
// previous ring and leaves an empty queue AT THE SAME ADDRESS.
//
// That is what this case is for. Registration is by address, so it has to hold
// across 16 activations each of which releases and re-fills the ring: a collector
// that dropped the table entry when a ring was freed, or that cached the ring
// pointer instead of re-reading the handle at every collection, frees objects the
// current activation is still holding. The module-scope cases cannot show that --
// their rings are built once.
//
// 32 objects rather than one, deliberately. The handles pass through this frame as
// SSA temporaries on their way into the ring, and a stale spill is a conservative
// root (§5.2), so one or two may be pinned by accident; 32 cannot be, and the sum
// is over all of them. Reading only q[$] would let a spill mask a missing
// registration entirely -- the trap §5.4 documents twice.
TEST_CASE("gc: objects reachable only from a queue local to a suspended process survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic Cell make(input int i);
                Cell c;
                c = new;
                c.v   = i;
                c.tag = "process-local-queue";
                return c;
            endfunction
            always @(posedge clk) begin
                automatic Cell q[$];
                automatic int  s = 0;
                for (int i = 1; i <= 32; i++) q.push_back(make(i));
                repeat (3) @(posedge clk);
                foreach (q[i]) s = s + q[i].v;
                $display("%0d %0d %s %s", q.size(), s, q[0].tag, q[31].tag);
            end
        endmodule
    )";
    // Four completed activations, as in §2's posedge case: the body consumes its
    // own trigger plus three more, and the clock has 16.
    const std::string line     = "32 528 process-local-queue process-local-queue\n";
    const std::string expected = line + line + line + line;
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// A blocking task is its own coroutine (setPresplitCoroutine at
// ir/llvm/Subroutines.cpp, declare_task) and its locals DO get real frame slots, so `Cell q[$]`
// here is the shape rt_gc_container_add / _remove exists for (Gc.h:265-268): the
// frame holds only a pointer to a malloc'd ring, which rt_gc_find_object rejects
// because it is not in a block, so scanning the frame conservatively cannot help
// and the container has to register itself as a root at element init and
// deregister at deinit. Nothing in the static table covers it, and the case above
// goes through that table instead -- so a build with no add/remove pair at all
// passes there and frees all 32 objects here. The suspension is a #delay rather
// than an edge, covering the other half of the quiescent-edge set (F1).
TEST_CASE("gc: objects reachable only from a queue local to a suspended task survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            Junk sink;
            function automatic Cell make(input int i);
                Cell c;
                c = new;
                c.v   = i;
                c.tag = "task-queue-rooted";
                return c;
            endfunction
            task automatic hold(input int base);
                Cell q[$];
                int  s = 0;
                for (int i = 1; i <= 32; i++) q.push_back(make(base + i));
                #20;
                foreach (q[i]) s = s + q[i].v;
                $display("%0d %0d %s %s", q.size(), s, q[0].tag, q[31].tag);
            endtask
            initial begin
                for (int i = 0; i < 30; i++) begin
                    #1;
                    for (int k = 0; k < 64; k++) begin
                        sink = new;
                        sink.v   = 32'h5A5A5A5A;
                        sink.tag = "junk";
                    end
                end
            end
            initial hold(100);
        endmodule
    )";
    const std::string expected = "32 3728 task-queue-rooted task-queue-rooted\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// A container inside a class body is NOT a root (Gc.h:242-244): the queue's
// storage is inside the object, so it is reached by tracing the owning object's
// rt_gc_map leaf table (Gc.h:81-93) and stops being reached the moment that
// object dies. This is the case that requires the leaf table to be emitted for
// real -- P2 shipped `nleaves == 0` with the whole managed-property job left to
// the finalizer, and against that map these eight objects are unreachable from
// anything and get freed while `h` is alive and holding them.
TEST_CASE("gc: objects reachable only from a queue property of a live object survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell;   int v; string tag; endclass
            class Junk;   int v; string tag; endclass
            class Holder; string name; Cell q[$]; endclass
            Holder h;
            bit    clk;
            Junk   sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                Cell c;
                h = new;
                h.name = "holder";
                for (int i = 0; i < 8; i++) begin
                    c = new;
                    c.v   = base + i;
                    c.tag = "class-property-queue";
                    h.q.push_back(c);
                end
            endfunction
            function automatic int total();
                int s = 0;
                foreach (h.q[i]) s = s + h.q[i].v;
                return s;
            endfunction
            initial begin
                install(51);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s %s", h.q.size(), total(), h.name, h.q[7].tag);
            end
        endmodule
    )";
    const std::string expected = "8 436 holder class-property-queue\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// The mirror, and the direction with no observable symptom: when the OWNING object
// dies its container must stop keeping anything alive. 50 holders, each with a
// seven-element queue, all built in a plain function and unreachable the moment it
// returns -- 400 objects that must be reclaimed while the design runs.
//
// What this catches is the tempting shortcut of registering every handle-bearing
// container as a root when it acquires its first handle element: the queue inside
// a dead holder is then a root nothing ever removes, its seven cells are immortal,
// and objects_live climbs to the full 400. It tolerates one collection of lag --
// a holder collected in cycle N may only release its cells in N+1 -- which is
// sound, merely conservative, and is what the 32-object margin is for.
TEST_CASE("gc: the container of a dead object keeps nothing alive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell;   int v; string tag; endclass
            class Holder; string name; Cell q[$]; endclass
            int built = 0;
            function automatic void build_one(input int c);
                Holder h;
                Cell   e;
                h = new;
                h.name = "owner-that-dies";
                for (int i = 0; i < 7; i++) begin
                    e = new;
                    e.v   = c * 8 + i;
                    e.tag = "owned-by-a-dead-holder";
                    h.q.push_back(e);
                end
                built = built + 8;
            endfunction
            initial begin
                for (int c = 0; c < 50; c++) begin
                    build_one(c);
                    #1;
                end
                $display("built %0d", built);
            end
        endmodule
    )";
    const rt_gc_stats st = run_stats(src, "built 400\n", cadence_stress);
    // Negative space first: with no collection every bound below is vacuous.
    CHECK(st.collections >= 25u);
    CHECK(st.objects_allocated >= 400u);
    CHECK(st.finalized >= 350u);
    CHECK(st.objects_live <= 32u);
}

// Ring wraparound, which is the queue walker's one arithmetic obligation: the
// live elements occupy physical slots (head + i) mod cap (runtime/Containers.cpp, q_elem)
// and pop_front advances head rather than shifting (rt_queue_pop_front). The design
// forces the wrap rather than hoping for it: 16 pushes settle cap at 16 with
// head == 0 (growth linearises, q_reserve), 12 pop_fronts move head to
// 12 without dropping capacity, and the next 12 pushes fit without a grow -- so
// they land in physical slots 0..11 while the four survivors sit at 12..15.
//
// Those 12 wrapped elements are the only reference to their objects, and a walker
// that computes base + (head + i) * stride without the modulus does not see one of
// them: it walks off the end of the ring instead. The four unwrapped survivors are
// what keeps that from being a wholly-empty queue, so the case cannot pass by
// walking nothing.
TEST_CASE("gc: the queue walker follows ring wraparound") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            Cell q[$];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void fill_and_wrap();
                Cell c;
                for (int i = 0; i < 16; i++) begin
                    c = new;
                    c.v   = 1000 + i;
                    c.tag = "pre";
                    q.push_back(c);
                end
                for (int i = 0; i < 12; i++) begin
                    c = q.pop_front();
                    c = null;
                end
                for (int i = 0; i < 12; i++) begin
                    c = new;
                    c.v   = i;
                    c.tag = "wrapped";
                    q.push_back(c);
                end
            endfunction
            function automatic int total();
                int s = 0;
                foreach (q[i]) s = s + q[i].v;
                return s;
            endfunction
            initial begin
                fill_and_wrap();
                repeat (6) @(posedge clk);
                $display("%0d %0d %s %s %s", q.size(), total(), q[0].tag, q[4].tag,
                         q[15].tag);
            end
        endmodule
    )";
    // 1012..1015 survive the pops (4054) plus the 12 wrapped 0..11 (66).
    const std::string expected = "16 4120 pre wrapped wrapped\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// An associative array KEYED by a class handle (LRM 7.8.3). The key is a strong
// reference and the collector has to decode it out of the normalized key bytes,
// where the handle's 64 bits were big-endian-encoded (emit_key_bytes,
// ir/llvm/AssocArrays.cpp, emit_agg_key_bytes/normalize_agg_key). This is the ONE-ENTRY case of the key-handle offset
// table: `nkeyhandles == 1` at offset 0 (Runtime.h:937-946). It is the only shape the
// deleted RT_ELEM_FLAG_KEY_HANDLE bit could express, so keeping it here is what pins
// that generalizing one flag into a table did not cost the degenerate case. Here the
// VALUE is a plain int, so the key path is the only reference to any of these four
// objects and a collector that traces element payloads but not keys frees all of them.
//
// The readback is through first / next, so the case also pins that a key surviving
// collection is still a findable key: it is read back into `k` and dereferenced,
// which is a use-after-free on both its body and its `string` if it was freed.
TEST_CASE("gc: an object held only as an associative-array key survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            int  keyed[Cell];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                Cell c;
                for (int i = 0; i < 4; i++) begin
                    c = new;
                    c.v      = base + i;
                    c.tag    = "key-rooted";
                    keyed[c] = (base + i) * 2;
                end
            endfunction
            // Order-independent: 7.8.3 makes handle-key traversal order arbitrary,
            // so this sums over every entry rather than checking a sequence.
            function automatic int walk();
                Cell k;
                int  s = 0;
                if (keyed.first(k)) begin
                    s = s + k.v + keyed[k];
                    while (keyed.next(k)) s = s + k.v + keyed[k];
                end
                return s;
            endfunction
            function automatic string first_tag();
                Cell k;
                if (keyed.first(k)) return k.tag;
                return "none";
            endfunction
            initial begin
                install(61);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s", keyed.num(), walk(), first_tag());
            end
        endmodule
    )";
    const std::string expected = "4 750 key-rooted\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// The third element shape, between a bare handle and a nested container: an
// AGGREGATE element that holds a handle inside its byte image. Its descriptor
// lists that handle by its offset within the image, alongside the string leaf it
// already carried for deep-copy and free (Runtime.h:913-916) -- so this is the one
// case where `nhandles` and `leaves` describe the same element and both have to be
// right. A walker that only recognises a whole-element handle (offset 0, the shape
// of the four cases above) frees all four objects here; one that reads the handle
// at the wrong offset within the image marks `seq` or the rt_string as if it were
// a handle, which the object-start bitmap rejects, so it marks nothing.
TEST_CASE("gc: objects reachable only from a queue of structs holding handles survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            typedef struct { int seq; Cell c; string label; } Wrap;
            Wrap wq[$];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                Wrap w;
                for (int i = 0; i < 4; i++) begin
                    w.seq   = i;
                    w.c     = new;
                    w.c.v   = base + i;
                    w.c.tag = "aggregate-element";
                    w.label = "wrap";
                    wq.push_back(w);
                end
            endfunction
            function automatic int total();
                int s = 0;
                foreach (wq[i]) s = s + wq[i].c.v + wq[i].seq;
                return s;
            endfunction
            initial begin
                install(71);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s %s", wq.size(), total(), wq[0].label, wq[3].c.tag);
            end
        endmodule
    )";
    // 71..74 plus the four seq values.
    const std::string expected = "4 296 wrap aggregate-element\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// Nesting, three container levels deep: a queue of queues of dynamic arrays of
// handles (LRM 7.10 with 7.5). A nested container element carries no handle
// offsets of its own -- its handles are found by recursing through
// rt_elem_ops::inner (Runtime.h:913-919) -- so the 27 objects here are three
// `inner` hops from the design-state root. A depth-1 walk (only the outer queue's
// own element images) or a depth-2 one frees every one of them, which is what
// makes this a test of the walk being an explicit worklist rather than a
// hand-unrolled level or two.
//
// The rows are built in a plain function and pushed by value, so the deep copy in
// the outer queue is the only surviving reference: the local row containers are
// deinit'd at return, which also exercises the removal half of the frame-local
// registration on a container that held handles.
TEST_CASE("gc: objects reachable only through three levels of nested containers survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            Cell nest[$][$][];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void build();
                Cell rowq[$][];
                Cell arr[];
                Cell c;
                for (int i = 0; i < 3; i++) begin
                    rowq.delete();
                    for (int j = 0; j < 3; j++) begin
                        arr = new[3];
                        for (int k = 0; k < 3; k++) begin
                            c = new;
                            c.v    = i * 9 + j * 3 + k;
                            c.tag  = "nested";
                            arr[k] = c;
                        end
                        rowq.push_back(arr);
                    end
                    nest.push_back(rowq);
                end
            endfunction
            function automatic int total();
                int s = 0;
                foreach (nest[i]) begin
                    foreach (nest[i][j]) begin
                        foreach (nest[i][j][k]) s = s + nest[i][j][k].v;
                    end
                end
                return s;
            endfunction
            initial begin
                build();
                repeat (6) @(posedge clk);
                $display("%0d %0d %0d %0d %s", nest.size(), nest[2].size(),
                         nest[2][2].size(), total(), nest[2][2][2].tag);
            end
        endmodule
    )";
    // 27 objects, values 0..26.
    const std::string expected = "3 3 3 351 nested\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// The shape this whole phase exists for: garbage-collection.md §3.3 names the UVM
// component tree as the reason the collector has to walk containers at all, since
// uvm_component holds its children in an associative array of handles. Ten
// components: a root, three agents, two drivers each, every child holding its
// parent back, so the tree is one strongly-connected component whose every edge
// but the first is a class-property associative array.
//
// The survival half. `root` is the only root, and every other object is reached
// only by walking an assoc property of a live object -- rt_gc_map leaves into
// rt_assoc_visit, once per level. The walk also reads each grandchild's `parent`
// name back and adds a decisive 100000 if it does not match, so a collector that
// keeps the children alive but corrupts or misses the back edges is not a pass.
TEST_CASE("gc: a live UVM-shaped tree of assoc children stays reachable") {
    constexpr std::string_view src = R"(
        module m;
            class Junk; int id; string name; endclass
            class Comp;
                int    id;
                string name;
                Comp   parent;
                Comp   children[string];
                function automatic void add(input string n, input Comp c);
                    children[n] = c;
                    c.parent    = this;
                endfunction
            endclass
            Comp root;
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.id   = 32'h5A5A5A5A;
                    sink.name = "junk";
                end
            end
            function automatic Comp mk(input int id, input string name);
                Comp c;
                c = new;
                c.id   = id;
                c.name = name;
                return c;
            endfunction
            function automatic void build_env();
                Comp agent, driver;
                root = mk(1, "root");
                for (int i = 0; i < 3; i++) begin
                    agent = mk(10 + i, {"agent", string'(48 + i)});
                    root.add(agent.name, agent);
                    for (int j = 0; j < 2; j++) begin
                        driver = mk(100 + i * 10 + j, {"driver", string'(48 + j)});
                        agent.add(driver.name, driver);
                    end
                end
            endfunction
            function automatic int walk();
                int s = root.id;
                foreach (root.children[n]) begin
                    s = s + root.children[n].id;
                    foreach (root.children[n].children[mm]) begin
                        s = s + root.children[n].children[mm].id;
                        if (root.children[n].children[mm].parent.name !=
                            root.children[n].name) s = s + 100000;
                    end
                end
                return s;
            endfunction
            initial begin
                build_env();
                repeat (6) @(posedge clk);
                $display("%0d %0d %s", root.children.num(), walk(),
                         root.children["agent2"].name);
            end
        endmodule
    )";
    // 1 + (10+11+12) + (100+101+110+111+120+121).
    const std::string expected = "3 697 agent2\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
}

// The reclamation half, and the gate on M4: drop the root of that tree and the
// whole subtree must go. Nothing here is observable to the design (LRM 8.4), so
// this is asserted on the collector's counters, with `collections` first so the
// bound cannot pass vacuously.
//
// Refcounting cannot reclaim any of it -- verilator#4651, quoted in §1: "Assuming
// each and every component references its children and parent, currently no
// uvm_components will ever be freed" -- and neither can a tracing collector whose
// container-root table wrongly contains `children`, which would make every tree
// ever built immortal. 30 trees of 10, so the bound is two orders of magnitude
// below the 300 allocated.
TEST_CASE("gc: a dropped UVM-shaped tree of assoc children is reclaimed") {
    constexpr std::string_view src = R"(
        module m;
            class Comp;
                int    id;
                string name;
                Comp   parent;
                Comp   children[string];
                function automatic void add(input string n, input Comp c);
                    children[n] = c;
                    c.parent    = this;
                endfunction
            endclass
            int built = 0;
            function automatic Comp mk(input int id, input string name);
                Comp c;
                c = new;
                c.id   = id;
                c.name = name;
                built  = built + 1;
                return c;
            endfunction
            function automatic void build_env();
                Comp root, agent, driver;
                root = mk(1, "root");
                for (int i = 0; i < 3; i++) begin
                    agent = mk(10 + i, {"agent", string'(48 + i)});
                    root.add(agent.name, agent);
                    for (int j = 0; j < 2; j++) begin
                        driver = mk(100 + i * 10 + j, {"driver", string'(48 + j)});
                        agent.add(driver.name, driver);
                    end
                end
            endfunction
            initial begin
                for (int c = 0; c < 30; c++) begin
                    build_env();
                    #1;
                end
                $display("built %0d", built);
            end
        endmodule
    )";
    const rt_gc_stats st = run_stats(src, "built 300\n", cadence_stress);
    CHECK(st.collections >= 15u);
    CHECK(st.objects_allocated >= 300u);
    // Every tree is garbage the instant build_env returns; only the last one can
    // still be live when the final collection runs.
    CHECK(st.finalized >= 260u);
    CHECK(st.objects_live <= 32u);
}

// The key shape P4 refused and this phase describes: an associative array whose KEY
// is an unpacked struct or fixed array CONTAINING a class handle. What replaced the
// one-bit RT_ELEM_FLAG_KEY_HANDLE is an OFFSET TABLE -- `nkeyhandles` entries in
// `keyhandles`, each naming 8 big-endian bytes inside one normalized key span
// (Runtime.h:937-957) -- so a handle at any offset, and several handles per key, are
// describable. Lowering always admitted the shape (a class handle's value_cat is
// `scalar`, lower/Types.cpp) and guard_agg_key_leaves (lower/Types.cpp) still refuses
// only the leaves with no fixed width, so the refusal that used to stand here is
// replaced by survival cases rather than deleted: the alternative to the guard was
// freeing an object whose only reference is inside a key.
//
// There are two independent ways to get the table wrong, and the cases below are
// chosen to separate them:
//
//   - stopping at offset 0, which is every walk a flag bit could describe. The
//     nonzero-offset and several-handles cases fail on it; the bare handle-key case
//     in §3 still passes, which is why that one alone proves nothing here.
//   - reusing the STORAGE offsets. An unpacked struct's storage is an LLVM struct
//     GEP'd by field index (agg_member, ir/llvm/Aggregates.cpp), so a handle member is
//     8-byte aligned and its predecessors are padded; the serialized span is
//     `ceil(W/8)` per leaf with no padding at all (assoc-keys.md §2, agg_key_bytes at
//     ir/llvm/AssocArrays.cpp, agg_key_bytes). The two agree only by accident, and the mixed-width
//     case exists to make them disagree by three bytes.
//
// Every case holds its objects ONLY in key bytes and runs the Junk filler, so an
// object the collector loses is recycled into 0x5A5A5A5A junk with a freed `string`.
// The readback then either sums junk or is an ASan use-after-free on the tag, instead
// of reading its own stale-but-intact value and passing.

// Offset 0, the entry the old flag bit could have described, but reached through the
// aggregate path: the span is 12 bytes, the handle is the first leaf, and the `int i`
// leaf behind it is what a walk that treats the whole span as one handle gets wrong.
TEST_CASE("gc: an object held only inside a struct assoc KEY survives (offset 0)") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            typedef struct { Cell c; int i; } k_t;
            int  keyed[k_t];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                k_t k;
                for (int i = 0; i < 4; i++) begin
                    k.c      = new;
                    k.c.v    = base + i;
                    k.c.tag  = "key-agg-0";
                    k.i      = i;
                    keyed[k] = (base + i) * 2;
                end
                k.c = null;
            endfunction
            // Order-independent: the handle is the FIRST leaf of the span, so 7.8.3's
            // "deterministic but arbitrary" handle ordering decides the traversal
            // order and this sums every entry rather than checking a sequence.
            function automatic int walk();
                k_t k;
                int s = 0;
                if (keyed.first(k)) begin
                    s = s + k.c.v + k.i + keyed[k];
                    while (keyed.next(k)) s = s + k.c.v + k.i + keyed[k];
                end
                return s;
            endfunction
            function automatic string first_tag();
                k_t k;
                if (keyed.first(k)) return k.c.tag;
                return "none";
            endfunction
            initial begin
                install(61);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s", keyed.num(), walk(), first_tag());
            end
        endmodule
    )";
    // Keys 61..64 (250), their `i` leaves 0..3 (6), their values 122..128 (500).
    const std::string expected = "4 756 key-agg-0\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    // Paired with the filler: without allocation churn "a lost object reads back as
    // poison" is a claim about a heap nothing ever recycled, and the survival above
    // could pass on a collector that freed the keys into untouched memory.
    CHECK(st.objects_allocated >= 512u);
}

// The case a flag bit could never express, and the most valuable one in the set: the
// handle is the SECOND leaf, at serialized offset 4 behind one `int`. Three distinct
// bugs land here. A walk that only knows offset 0 marks the `int i` bytes, which the
// object-start bitmap rejects, so it marks nothing. A walk off by one leaf reads the
// wrong 8 bytes for the same result. A walk on STORAGE offsets reads the 8 bytes at
// byte 8 -- the member's padded LLVM offset -- which run 4 past the end of a 12-byte
// span.
//
// The leading `int` also makes the traversal order deterministic (the handle bytes
// are never reached by the memcmp), so unlike the offset-0 case this one can name
// WHICH object it recovered at each end of the map.
TEST_CASE("gc: an object held only in a struct assoc KEY at a NONZERO offset survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            typedef struct { int i; Cell c; } k_t;
            int  keyed[k_t];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                k_t k;
                for (int i = 0; i < 4; i++) begin
                    k.i      = i;
                    k.c      = new;
                    k.c.v    = base + i;
                    k.c.tag  = "key-agg-nz";
                    keyed[k] = (base + i) * 2;
                end
                k.c = null;
            endfunction
            function automatic int walk();
                k_t k;
                int s = 0;
                if (keyed.first(k)) begin
                    s = s + k.c.v + keyed[k];
                    while (keyed.next(k)) s = s + k.c.v + keyed[k];
                end
                return s;
            endfunction
            // The leading int orders the span, so first / last name the i==0 and i==3
            // entries specifically: each end of the table's offset walk is checked by
            // dereferencing the handle it recovered, not by a sum that a single
            // surviving object could carry.
            function automatic int end_v(input bit want_last);
                k_t k;
                if (want_last) begin
                    if (keyed.last(k)) return k.c.v;
                end else if (keyed.first(k)) return k.c.v;
                return -1;
            endfunction
            function automatic string first_tag();
                k_t k;
                if (keyed.first(k)) return k.c.tag;
                return "none";
            endfunction
            initial begin
                install(71);
                repeat (6) @(posedge clk);
                $display("%0d %0d %0d %0d %s", keyed.num(), walk(), end_v(0), end_v(1),
                         first_tag());
            end
        endmodule
    )";
    // Keys 71..74 (290) plus their values 142..148 (580); ends are i==0 and i==3.
    const std::string expected = "4 870 71 74 key-agg-nz\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.objects_allocated >= 512u);
}

// Two handles in ONE key, at serialized offsets 0 and 12 across a 20-byte span. A
// table that stops after its first entry keeps four objects and frees four, so the
// `b` half of every sum reads junk. The second offset is also where the storage
// layout diverges hardest: `Cell b` is at storage offset 16, past the padding after
// the `int i`, so a storage-offset walk reads 4 bytes of `b` and 4 bytes of nothing.
TEST_CASE("gc: several objects held only in the SAME struct assoc KEY survive") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            typedef struct { Cell a; int i; Cell b; } k_t;
            int  keyed[k_t];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                k_t k;
                for (int i = 0; i < 4; i++) begin
                    k.a      = new;
                    k.a.v    = base + i;
                    k.a.tag  = "key-a";
                    k.i      = i;
                    k.b      = new;
                    k.b.v    = base + 100 + i;
                    k.b.tag  = "key-b";
                    keyed[k] = i;
                end
                k.a = null;
                k.b = null;
            endfunction
            // Order-independent again: `a` leads the span (7.8.3). Both handles are
            // dereferenced per entry, so losing either half is a wrong sum or an ASan
            // report, never a silent pass.
            function automatic int walk();
                k_t k;
                int s = 0;
                if (keyed.first(k)) begin
                    s = s + k.a.v + k.b.v + k.i + keyed[k];
                    while (keyed.next(k)) s = s + k.a.v + k.b.v + k.i + keyed[k];
                end
                return s;
            endfunction
            function automatic string tag_of(input bit second);
                k_t k;
                if (!keyed.first(k)) return "none";
                if (second) return k.b.tag;
                return k.a.tag;
            endfunction
            initial begin
                install(81);
                repeat (6) @(posedge clk);
                $display("%0d %0d %s %s", keyed.num(), walk(), tag_of(0), tag_of(1));
            end
        endmodule
    )";
    // a: 81..84 (330); b: 181..184 (730); `i` leaves and values both 0..3 (6 each).
    const std::string expected = "4 1072 key-a key-b\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.objects_allocated >= 512u);
    // Eight key-only objects, not four: a table truncated to one entry would keep
    // exactly half of them, and this is the count that says so.
    CHECK(st.objects_live >= 8u);
}

// Depth-first leaf order, through a fixed unpacked ARRAY and then a nested STRUCT:
// the span is pre[0], pre[1], pre[2], in.c, in.tail, so the handle is at serialized
// offset 3. Nothing about that offset is guessable from the type's shape -- it is
// three `byte` leaves of an array, which the offset walk has to enter and stride
// through at 1 byte per element (agg_key_handle_offsets, ir/llvm/GcEmit.cpp).
// Storage puts `pre` in bytes 0..2 and then aligns `in` to 8, so a storage-offset
// walk starts on byte 8 of a 15-byte span -- inside it, past the handle, and running
// one byte off the end.
TEST_CASE("gc: a handle nested under an array and a struct inside an assoc KEY survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            typedef struct { Cell c; int tail; } in_t;
            typedef struct { byte pre[3]; in_t in; } k_t;
            int  keyed[k_t];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                k_t k;
                for (int i = 0; i < 4; i++) begin
                    k.pre[0]   = i;
                    k.pre[1]   = i + 10;
                    k.pre[2]   = i + 20;
                    k.in.c     = new;
                    k.in.c.v   = base + i;
                    k.in.c.tag = "nested-a";
                    k.in.tail  = i * 2;
                    keyed[k]   = base + i;
                end
                k.in.c = null;
            endfunction
            function automatic int walk();
                k_t k;
                int s = 0;
                if (keyed.first(k)) begin
                    s = s + k.in.c.v + k.in.tail + keyed[k];
                    while (keyed.next(k)) s = s + k.in.c.v + k.in.tail + keyed[k];
                end
                return s;
            endfunction
            // pre[0] leads the span, so the order is deterministic and `first` is the
            // i==0 entry. Reading pre[2] back as well pins that the leaves BEHIND the
            // handle still round-trip through denormalize_agg_key: an offset walk that
            // is right about the handle but wrong about the span length would be
            // marking bytes that belong to a different leaf.
            function automatic int first_pre2();
                k_t k;
                if (keyed.first(k)) return k.pre[2];
                return -1;
            endfunction
            function automatic int first_v();
                k_t k;
                if (keyed.first(k)) return k.in.c.v;
                return -1;
            endfunction
            function automatic string first_tag();
                k_t k;
                if (keyed.first(k)) return k.in.c.tag;
                return "none";
            endfunction
            initial begin
                install(111);
                repeat (6) @(posedge clk);
                $display("%0d %0d %0d %0d %s", keyed.num(), walk(), first_v(),
                         first_pre2(), first_tag());
            end
        endmodule
    )";
    // Keys 111..114 (450), tails 0/2/4/6 (12), values 111..114 (450); first is i==0.
    const std::string expected = "4 912 111 20 nested-a\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.objects_allocated >= 512u);
}

// The other nesting direction: a fixed unpacked array OF HANDLES, inside a nested
// struct, behind an int. The span is lead, in.pair[0], in.pair[1], in.tag_id, so the
// table has two entries at 4 and 12 and the walk has to stride the array by 8
// (agg_key_handle_offsets, ir/llvm/GcEmit.cpp) rather than by the element's
// storage size. Storage would say 8 and 16.
TEST_CASE("gc: an assoc KEY holding a fixed array of handles keeps every element") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            typedef struct { Cell pair[2]; int tag_id; } pr_t;
            typedef struct { int lead; pr_t in; } k_t;
            int  keyed[k_t];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                k_t k;
                for (int i = 0; i < 4; i++) begin
                    k.lead            = i;
                    k.in.pair[0]      = new;
                    k.in.pair[0].v    = base + i;
                    k.in.pair[0].tag  = "pair-a";
                    k.in.pair[1]      = new;
                    k.in.pair[1].v    = base + 50 + i;
                    k.in.pair[1].tag  = "pair-b";
                    k.in.tag_id       = i * 3;
                    keyed[k]          = i;
                end
                k.in.pair[0] = null;
                k.in.pair[1] = null;
            endfunction
            function automatic int walk();
                k_t k;
                int s = 0;
                if (keyed.first(k)) begin
                    s = s + k.in.pair[0].v + k.in.pair[1].v + k.in.tag_id + keyed[k];
                    while (keyed.next(k))
                        s = s + k.in.pair[0].v + k.in.pair[1].v + k.in.tag_id + keyed[k];
                end
                return s;
            endfunction
            function automatic int first_v();
                k_t k;
                if (keyed.first(k)) return k.in.pair[0].v;
                return -1;
            endfunction
            // The SECOND array element specifically: a walk that enters the array but
            // does not stride it emits one offset and frees four objects, all of them
            // pair[1], which is the only leaf this reads.
            function automatic string second_tag();
                k_t k;
                if (keyed.first(k)) return k.in.pair[1].tag;
                return "none";
            endfunction
            initial begin
                install(91);
                repeat (6) @(posedge clk);
                $display("%0d %0d %0d %s", keyed.num(), walk(), first_v(), second_tag());
            end
        endmodule
    )";
    // pair[0]: 91..94 (370); pair[1]: 141..144 (570); tag_ids 0/3/6/9 (18); values 6.
    const std::string expected = "4 964 91 pair-b\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.objects_allocated >= 512u);
    CHECK(st.objects_live >= 8u);
}

// The case that separates a SERIALIZED offset walk from a STORAGE one, and the only
// case in the set that a walk passing all of the above can still fail. Three integral
// leaves of different widths sit ahead of the handle:
//
//   leaf          width   serialized bytes   serialized offset   storage offset
//   byte b          8            1                   0                 0
//   shortint s     16            2                   1                 2
//   bit [12:0] w   13            2                   3                 4
//   Cell c         64            8                   5                 8
//
// `ceil(W/8)` per leaf with no padding (assoc-keys.md §2) puts the handle at 5; the
// LLVM struct layout agg_member GEPs (ir/llvm/Aggregates.cpp) puts the member at 8,
// three bytes later, because `shortint` is 2-aligned and the handle is 8-aligned.
// A walk reusing the storage offsets reads bytes 8..15 of a 13-byte span: the tail of
// the handle plus three bytes off the end, which marks nothing and frees the object.
TEST_CASE("gc: an assoc KEY handle behind mixed-width leaves uses SERIALIZED offsets") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            typedef struct { byte b; shortint s; bit [12:0] w; Cell c; } k_t;
            int  keyed[k_t];
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void install(input int base);
                k_t k;
                for (int i = 0; i < 4; i++) begin
                    k.b      = i;
                    k.s      = 1000 + i;
                    k.w      = 13'h1000 + i;
                    k.c      = new;
                    k.c.v    = base + i;
                    k.c.tag  = "mixed-width";
                    keyed[k] = base + i;
                end
                k.c = null;
            endfunction
            function automatic int walk();
                k_t k;
                int s = 0;
                if (keyed.first(k)) begin
                    s = s + k.c.v + keyed[k];
                    while (keyed.next(k)) s = s + k.c.v + keyed[k];
                end
                return s;
            endfunction
            // `b` leads, so `first` is the i==0 entry. Its narrow leaves are read back
            // too: they are the ones whose serialized widths place the handle, so a
            // wrong `s` or `w` here says the span itself was laid out differently than
            // the offsets in the table assume, which is the same bug seen upstream.
            function automatic int first_of(input int which);
                k_t k;
                if (!keyed.first(k)) return -1;
                if (which == 0) return k.c.v;
                if (which == 1) return k.s;
                return k.w;
            endfunction
            function automatic string first_tag();
                k_t k;
                if (keyed.first(k)) return k.c.tag;
                return "none";
            endfunction
            initial begin
                install(101);
                repeat (6) @(posedge clk);
                $display("%0d %0d %0d %0d %0d %s", keyed.num(), walk(), first_of(0),
                         first_of(1), first_of(2), first_tag());
            end
        endmodule
    )";
    // Keys 101..104 (410) plus values 101..104 (410); first is b==0, s==1000, w==4096.
    const std::string expected = "4 820 101 1000 4096 mixed-width\n";
    check_both_cadences(src, expected);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.objects_allocated >= 512u);
}

// -- 4. Cycles are collected --------------------------------------------------
//
// The case reference counting cannot handle, and the reason §1 rejects it:
// verilator#4651 records exactly this shape leaking forever under intrusive
// refcounting. Asserting the printed output is not enough here -- the design
// cannot see reclamation -- so these two assert on the collector's own counters.
// `finalized` counts only finalizers run by a COLLECTION, so it is a direct
// count of objects reclaimed while the design was running.

// A two-object cycle per iteration, built in a plain FUNCTION so both handles
// die with the native stack frame and the pair is genuinely unreachable at the
// next safepoint. 60 pairs are dropped; a collector that traces from roots
// reclaims all but the newest, a refcounting one reclaims none.
TEST_CASE("gc: a dropped parent-child cycle is reclaimed") {
    constexpr std::string_view src = R"(
        module m;
            class Node; int v; string tag; Node peer; endclass
            int built = 0;
            function automatic void make_cycle(input int i);
                Node a, b;
                a = new; a.v =  i; a.tag = "parent";
                b = new; b.v = -i; b.tag = "child";
                a.peer = b;
                b.peer = a;
                built  = built + 1;
            endfunction
            initial begin
                for (int i = 0; i < 60; i++) begin
                    make_cycle(i);
                    #1;
                end
                $display("built %0d", built);
            end
        endmodule
    )";
    const rt_gc_stats st = run_stats(src, "built 60\n", cadence_stress);
    // Negative space first: with no collection every bound below is vacuous.
    CHECK(st.collections >= 1u);
    CHECK(st.objects_allocated >= 120u);
    // 120 objects built, all garbage the instant make_cycle returns. The margin
    // covers the newest pair (live when the last collection ran) and any pair a
    // stale conservative frame slot still designates (§5.2).
    CHECK(st.finalized >= 100u);
    CHECK(st.objects_live <= 24u);
}

// The UVM shape: a component holds its children and every child holds its
// parent, so the whole subtree is one strongly-connected component with a second
// path to `leaf_b` for good measure ("Assuming each and every component
// references its children and parent, currently no uvm_components will ever be
// freed" -- verilator#4651, quoted in garbage-collection.md §1). Dropping the
// root must reclaim all four, not three.
TEST_CASE("gc: a dropped UVM-shaped component subtree is reclaimed") {
    constexpr std::string_view src = R"(
        module m;
            class Component;
                string    name;
                Component parent;
                Component child_a;
                Component child_b;
            endclass
            int built = 0;
            function automatic void build_env();
                Component root, agent, driver, monitor;
                root    = new; root.name    = "root";
                agent   = new; agent.name   = "agent";
                driver  = new; driver.name  = "driver";
                monitor = new; monitor.name = "monitor";
                root.child_a   = agent;   agent.parent   = root;
                agent.child_a  = driver;  driver.parent  = agent;
                agent.child_b  = monitor; monitor.parent = agent;
                root.child_b   = monitor;
                built = built + 4;
            endfunction
            initial begin
                for (int i = 0; i < 30; i++) begin
                    build_env();
                    #1;
                end
                $display("built %0d", built);
            end
        endmodule
    )";
    const rt_gc_stats st = run_stats(src, "built 120\n", cadence_stress);
    CHECK(st.collections >= 1u);
    CHECK(st.objects_allocated >= 120u);
    CHECK(st.finalized >= 100u);
    CHECK(st.objects_live <= 24u);
}

// -- 5. Steady state ----------------------------------------------------------
//
// The `churn` shape from garbage-collection.md §11: N allocated per cycle, M
// kept live. This is what P1's acceptance criterion means by "reaches
// steady-state RSS instead of growing linearly" (§12), measured on the
// collector's own live count rather than on process RSS -- RSS is not a usable
// in-test signal here, because a sanitizer build's quarantine holds freed
// memory and would report growth for a heap that is being reclaimed correctly.

// 16 allocated per cycle for 200 cycles with a 4-deep shift register of
// survivors. Under stress a collection runs at every cycle, so the live count is
// the shift register plus whatever one cycle's garbage a conservative frame slot
// still designates -- two orders of magnitude below the 3200 allocated. A
// collector that reclaims nothing reports objects_live == objects_allocated.
TEST_CASE("gc: a churn loop with a bounded live set does not grow without bound") {
    constexpr std::string_view src = R"(
        module m;
            class Item; int v; string tag; endclass
            Item live0, live1, live2, live3;
            function automatic void one_cycle(input int c);
                Item fresh;
                for (int i = 0; i < 16; i++) begin
                    fresh = new;
                    fresh.v   = c * 16 + i;
                    fresh.tag = "churn";
                end
                live3 = live2;
                live2 = live1;
                live1 = live0;
                live0 = fresh;
            endfunction
            initial begin
                for (int c = 0; c < 200; c++) begin
                    one_cycle(c);
                    #1;
                end
                $display("%0d %0d %0d %0d", live0.v, live1.v, live2.v, live3.v);
            end
        endmodule
    )";
    const rt_gc_stats st = run_stats(src, "3199 3183 3167 3151\n", cadence_stress);
    CHECK(st.collections >= 100u);
    CHECK(st.objects_allocated >= 3200u);
    // 4 kept live, so 64 is a 16x margin and still 50x below the allocation
    // count. bytes_live is the same statement in the unit §12 phrases it in.
    CHECK(st.objects_live <= 64u);
    CHECK(st.bytes_live <= 64u * 1024u);
}

// The same design under the ordinary cadence, which is what every run gets
// without `--gc-stress`: collection is due only when the nursery budget is spent,
// so the bound is looser by construction -- one budget of garbage may accumulate
// between collections. What this case adds over the stress one is that the BUDGET
// path fires at all. With the 8 MiB default it would not, which is what
// nursery_bytes_small exists for.
TEST_CASE("gc: the nursery budget path bounds the live set") {
    constexpr std::string_view src = R"(
        module m;
            class Item; int v; string tag; endclass
            Item live0, live1, live2, live3;
            function automatic void one_cycle(input int c);
                Item fresh;
                for (int i = 0; i < 16; i++) begin
                    fresh = new;
                    fresh.v   = c * 16 + i;
                    fresh.tag = "churn";
                end
                live3 = live2;
                live2 = live1;
                live1 = live0;
                live0 = fresh;
            endfunction
            initial begin
                for (int c = 0; c < 400; c++) begin
                    one_cycle(c);
                    #1;
                end
                $display("%0d %0d %0d %0d", live0.v, live1.v, live2.v, live3.v);
            end
        endmodule
    )";
    const rt_gc_stats st = run_stats(src, "6399 6383 6367 6351\n", cadence_ordinary);
    CHECK(st.collections >= 3u);
    CHECK(st.objects_allocated >= 6400u);
    // A 32 KiB budget holds well under a thousand of these objects, so a live
    // count near the 6400 allocated means nothing is being reclaimed.
    CHECK(st.objects_live <= 2048u);
}

// -- 6. Finalization frees managed properties ---------------------------------

// The documented second-order leak: because a class body is never freed today,
// every string, queue, dynamic array and associative array INSIDE an object
// leaks its own heap too -- emit_class_body_init (ir/llvm/Aggregates.cpp) has
// no matching emit_agg_deinit call anywhere (garbage-collection.md §0). The fix
// is the per-class finalizer at Gc.h:91, which is that same deinit walk run at
// sweep, and `finalized` counts the objects it ran for.
//
// Each Rec owns two independent managed buffers (a string too long for any
// small-string optimisation, and a 32-element queue), so 800 uncounted objects
// here would be 1600 leaked buffers. The counter is the portable oracle; the
// end-to-end one is LeakSanitizer with the ASAN_OPTIONS=detect_leaks=0 override
// at data_model_test.cpp:701-705 deleted, which is P1's own acceptance criterion
// (§12) and a suite-level change, not something this case can assert.
TEST_CASE("gc: finalizers run for the managed properties of collected objects") {
    constexpr std::string_view src = R"(
        module m;
            class Rec;
                string name;
                int    log[$];
            endclass
            int made = 0;
            function automatic void churn(input int c);
                Rec r;
                for (int i = 0; i < 8; i++) begin
                    r = new;
                    r.name = "a-name-far-too-long-to-live-anywhere-but-its-own-heap-buffer";
                    for (int k = 0; k < 32; k++) r.log.push_back(c * 32 + k);
                    made = made + 1;
                end
            endfunction
            initial begin
                for (int c = 0; c < 100; c++) begin
                    churn(c);
                    #1;
                end
                $display("made %0d", made);
            end
        endmodule
    )";
    const rt_gc_stats st = run_stats(src, "made 800\n", cadence_stress);
    CHECK(st.collections >= 50u);
    CHECK(st.objects_allocated >= 800u);
    // Every Rec is garbage the moment churn() returns; only the final cycle's
    // eight can still be live when the last collection runs.
    CHECK(st.finalized >= 700u);
    CHECK(st.objects_live <= 32u);
}

// -- 7. Generational collection -----------------------------------------------
//
// garbage-collection.md §7. A young collection marks from the roots plus the
// remembered set and treats an OLD object as live WITHOUT tracing into it
// (Gc.h:359-363). That stop is the point of the phase, and it is also the one way
// a collector can lose a live object with every root correctly registered: the
// old -> young edge is invisible to a young mark, and the inlined write barrier
// (Gc.h:98-108) is the only thing that reports one.
//
// Two devices on top of §2's filler and `string` property:
//
//   - `remembered > 0`, in every retention case here. A young collection that
//     reached the object through a root retains it with the barrier deleted, so
//     without this pair the case stops testing what it names -- the same argument
//     as the `conservative_pins` assertion the frame cases carry (:427-432).
//   - `full_every_never`, which pins the collection KINDS. The first collection is
//     unconditionally full (nothing is OLD yet, so it costs what a young one
//     costs, and it is what first derives the old population), so a young-only run
//     reports `full_collections == 1`. Asserting exactly 1 is the load-bearing
//     part: it says no full collection ran after the store under test, and a full
//     one there would trace the owner and mask a missing barrier entirely. That is
//     also why stress is the wrong cadence for a barrier claim -- it
//     alternates young and full by design (the last case below), so the very next
//     collection after any store traces the whole heap.

// The barrier, in the smallest shape that needs it. `owner` is a module-scope
// handle, so it is a precise root scanned at every collection, young or full, and
// it is OLD from the first collection onwards. `attach` then stores a FRESH object
// into its `leaf` property and drops the only other reference, so from that
// instant the fresh object is designated by exactly one word in the entire
// process -- a property slot inside a promoted object -- and a young collection
// that does not consult the remembered set never looks at that word.
//
// The store is inside a FUNCTION for the same reason §2's cases put one there: a
// handle in the calling process's coroutine frame is conservatively pinned
// (§5.2), which would keep the fresh object alive through any number of young
// collections and let this case pass with the barrier deleted. `attach`'s locals
// live on the machine stack of a call that has already returned by the time the
// process next reaches a region boundary, so nothing pins them.
//
// Failure mode without the barrier: the young collection frees the fresh object
// while `owner.leaf` still points at it, the filler is handed the line straight
// back, and the $display prints 1515870810 junk instead of 4242 fresh-into-old.
TEST_CASE("gc: a fresh object stored into a promoted object survives a young collection") {
    constexpr std::string_view src = R"(
        module m;
            class Leaf; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Node; int id; string name; Leaf leaf; endclass
            Node owner;
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 96; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 128; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void attach(input Node o, input int v);
                Leaf f;
                f = new;
                f.v    = v;
                f.tag  = "fresh-into-old";
                o.leaf = f;
                f      = null;
            endfunction
            initial begin
                owner      = new;
                owner.id   = 7;
                owner.name = "long-lived-owner";
                repeat (16) @(posedge clk);
                attach(owner, 4242);
                repeat (16) @(posedge clk);
                $display("%0d %0d %s", owner.id, owner.leaf.v, owner.leaf.tag);
            end
        endmodule
    )";
    const std::string expected = "7 4242 fresh-into-old\n";
    check_both_cadences(src, expected);
    // Young-only: after the mandatory first collection nothing traces `owner`
    // again, so the remembered set is the sole path to the fresh object.
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    // `owner` was already OLD when attach stored into it -- otherwise a young
    // collection would have traced it as an ordinary young object and the
    // retention above would say nothing about the barrier.
    CHECK(yg.promoted > 0u);
    CHECK(yg.remembered > 0u);
    // And under stress, where the barrier runs against a collection at every
    // region boundary rather than one per spent nursery.
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.remembered > 0u);
}

// The same edge through a QUEUE property, which is the shape UVM has: a component
// holding a queue of transactions it was handed. The queue's payload is malloc'd
// rather than GC-heap memory (§3.3), so there is no GC object at the store's
// destination to log at all -- what has to be remembered is the OWNER, and the
// young collection then re-walks its leaf table (Gc.h:122-126) to find the
// element. A barrier keyed on the destination address, which is the natural
// reading of "a store into a heap object", logs nothing here.
//
// Failure mode: the pushed object is reachable only from a malloc'd ring that no
// young root path visits; the next young collection frees it, the filler poisons
// the line, and the element reads back 1515870810 junk.
TEST_CASE("gc: a fresh object pushed into a queue property of a promoted object survives") {
    constexpr std::string_view src = R"(
        module m;
            class Item; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Bag;  string name; Item q[$]; endclass
            Bag  bag;
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 96; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 128; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void stash(input Bag b, input int base);
                Item a;
                for (int i = 0; i < 3; i++) begin
                    a     = new;
                    a.v   = base + i;
                    a.tag = "queued-into-old";
                    b.q.push_back(a);
                end
                a = null;
            endfunction
            function automatic int total();
                int s = 0;
                foreach (bag.q[i]) s = s + bag.q[i].v;
                return s;
            endfunction
            initial begin
                bag      = new;
                bag.name = "long-lived-bag";
                repeat (16) @(posedge clk);
                stash(bag, 300);
                repeat (16) @(posedge clk);
                $display("%0d %0d %s %s", bag.q.size(), total(), bag.name, bag.q[2].tag);
            end
        endmodule
    )";
    const std::string expected = "3 903 long-lived-bag queued-into-old\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    CHECK(yg.promoted > 0u);
    CHECK(yg.remembered > 0u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.remembered > 0u);
}

// And through an ASSOCIATIVE ARRAY property -- the config-DB shape, and the one
// the queue walker cannot be reused for: an entry's payload is its own slot-vector
// block and the map moves those as it grows, so the walk goes through
// rt_assoc_visit (Runtime.h:939-941) rather than over a contiguous span (:793-798).
// Runtime.h:874-877 licenses element relocation by raw memcpy with no descriptor
// involvement at all, which is why the design doc's card-marking sketch preferred
// a range mark over an owner identity (§7). An owner-remembering barrier is sound
// against that only because the owner is re-walked from scratch at the young
// collection; nothing about where the handle was stored is retained.
//
// Failure mode: as the queue case, plus the one specific to growth -- anything
// remembered by destination address rather than by owner aims into a released
// slot block by the time the collection reads it.
TEST_CASE("gc: a fresh object mapped into an assoc property of a promoted object survives") {
    constexpr std::string_view src = R"(
        module m;
            class Item; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Bag;  string name; Item m[int]; endclass
            Bag  bag;
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 96; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 128; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            // Twelve entries rather than one, for two reasons: the map grows
            // several times over that range, so the payloads the collection must
            // reach are not the ones the stores wrote into; and only the FIRST
            // store finds the barrier's inline test true, because it sets LOGGED
            // (Gc.h:98-108). The other eleven objects are reachable only because
            // the one remembered owner is re-walked in full, which is the property
            // an owner-remembering barrier stands or falls on.
            function automatic void stash(input Bag b, input int base);
                Item c;
                for (int k = 0; k < 12; k++) begin
                    c        = new;
                    c.v      = base + k;
                    c.tag    = "mapped-into-old";
                    b.m[k*7] = c;
                end
                c = null;
            endfunction
            // Indexed rather than `foreach`: the keys are known, so this reads
            // every entry the collector had to keep without depending on the
            // iteration order of a map property.
            function automatic int total();
                int s = 0;
                for (int k = 0; k < 12; k++) s = s + bag.m[k*7].v;
                return s;
            endfunction
            initial begin
                bag      = new;
                bag.name = "long-lived-bag";
                repeat (16) @(posedge clk);
                stash(bag, 500);
                repeat (16) @(posedge clk);
                $display("%0d %0d %s %s", bag.m.num(), total(), bag.m[0].tag, bag.m[77].tag);
            end
        endmodule
    )";
    // 12 entries, values 500..511: 12*500 + (0+..+11).
    const std::string expected = "12 6066 mapped-into-old mapped-into-old\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    CHECK(yg.promoted > 0u);
    CHECK(yg.remembered > 0u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.remembered > 0u);
}

// The container shape where the value path proves nothing: an assoc PROPERTY whose
// KEY is a class handle and whose value is a plain int (§3's bare handle-key case is
// the same map in design state). The four fresh objects exist nowhere but in the
// normalized key bytes of entries inside a promoted object, so a young collection
// reaches them only by remembering the owner and then decoding the key-handle offset
// table -- here in its one-entry form, `nkeyhandles == 1` at offset 0
// (Runtime.h:937-946) -- during its leaf walk. A barrier emitted at handle STORES
// alone misses this entirely: no handle is stored here, one is hashed.
//
// Failure mode: all four keys are freed while the map still holds them; the filler
// recycles the lines, and the first/next readback either sums 0x5A5A5A5A values or
// dereferences a freed `string` under ASan.
TEST_CASE("gc: an object held only as an assoc KEY inside a promoted object survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Bag;  string name; int keyed[Cell]; endclass
            Bag  bag;
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 96; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 128; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void stash(input Bag b, input int base);
                Cell c;
                for (int i = 0; i < 4; i++) begin
                    c          = new;
                    c.v        = base + i;
                    c.tag      = "key-into-old";
                    b.keyed[c] = (base + i) * 2;
                end
                c = null;
            endfunction
            // Order-independent, as :1224-1225: 7.8.3 leaves handle-key traversal
            // order arbitrary, so this sums every entry rather than a sequence.
            function automatic int walk();
                Cell k;
                int  s = 0;
                if (bag.keyed.first(k)) begin
                    s = s + k.v + bag.keyed[k];
                    while (bag.keyed.next(k)) s = s + k.v + bag.keyed[k];
                end
                return s;
            endfunction
            function automatic string first_tag();
                Cell k;
                if (bag.keyed.first(k)) return k.tag;
                return "none";
            endfunction
            initial begin
                bag      = new;
                bag.name = "long-lived-bag";
                repeat (16) @(posedge clk);
                stash(bag, 61);
                repeat (16) @(posedge clk);
                $display("%0d %0d %s", bag.keyed.num(), walk(), first_tag());
            end
        endmodule
    )";
    // Keys 61..64 (250) plus their values 122..128 (500).
    const std::string expected = "4 750 key-into-old\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    CHECK(yg.promoted > 0u);
    CHECK(yg.remembered > 0u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.remembered > 0u);
}

// The generational case for the key TABLE rather than for its one-entry form: the
// promoted-owner shape directly above, but the key is an unpacked struct whose handle
// sits at serialized offset 4 instead of a bare handle at 0. Both halves have to work
// at once, and each fails differently. The write barrier has to log `bag` at a store
// that writes no handle anywhere -- one is HASHED into the key span -- which is why
// the barrier hangs off the assoc lvalue's `assoc_key_handle` on the entry-CREATING
// put_ptr path (ir/llvm/AssocArrays.cpp, assoc_elem_lvalue) rather than off any handle store -- and then the
// young collection's re-walk of that single remembered owner has to decode the handle
// back out of the span at the offset the table names. A barrier keyed on handle
// stores never logs the owner; a barrier that fires paired with a walk that reads
// offset 0 (the flag-bit shape) or offset 8 (the member's storage offset) marks
// nothing, because the object-start bitmap rejects the `int i` bytes and the
// past-the-end read alike.
//
// full_every_never is what makes this a statement about the YOUNG path: under a run
// with full collections in it, a full trace reaches the map from the root a safepoint
// or two after the store and would mask both failures at once.
TEST_CASE("gc: a struct assoc KEY holding a handle inside a promoted object survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            typedef struct { int i; Cell c; } k_t;
            class Bag;  string name; int keyed[k_t]; endclass
            Bag  bag;
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 96; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 128; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void stash(input Bag b, input int base);
                k_t k;
                for (int i = 0; i < 4; i++) begin
                    k.i        = i;
                    k.c        = new;
                    k.c.v      = base + i;
                    k.c.tag    = "key-agg-old";
                    b.keyed[k] = (base + i) * 2;
                end
                k.c = null;
            endfunction
            function automatic int walk();
                k_t k;
                int s = 0;
                if (bag.keyed.first(k)) begin
                    s = s + k.c.v + bag.keyed[k];
                    while (bag.keyed.next(k)) s = s + k.c.v + bag.keyed[k];
                end
                return s;
            endfunction
            // The leading int orders the span, so unlike the bare handle-key case
            // above this one can name the object it recovered: `first` is i==0.
            function automatic int first_v();
                k_t k;
                if (bag.keyed.first(k)) return k.c.v;
                return -1;
            endfunction
            function automatic string first_tag();
                k_t k;
                if (bag.keyed.first(k)) return k.c.tag;
                return "none";
            endfunction
            initial begin
                bag      = new;
                bag.name = "long-lived-bag";
                repeat (16) @(posedge clk);
                stash(bag, 61);
                repeat (16) @(posedge clk);
                $display("%0d %0d %0d %s", bag.keyed.num(), walk(), first_v(),
                         first_tag());
            end
        endmodule
    )";
    // Keys 61..64 (250) plus their values 122..128 (500); first is i==0.
    const std::string expected = "4 750 61 key-agg-old\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    CHECK(yg.promoted > 0u);
    // The narrow one: `b.keyed[k] = int` stores no handle and `bag.name` is written
    // once before any promotion, so the only barrier emission that can log this owner
    // is the entry-creating put_ptr's (ir/llvm/AssocArrays.cpp, assoc_elem_lvalue).
    CHECK(yg.remembered > 0u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.remembered > 0u);
}

// The store site that is not a store: `first(k)` WRITES the found key into `k`
// through denormalize_assoc_key, which reaches lower_store directly and never
// passes through the ordinary handle-store path (src/ir/llvm/). So when
// the readback target is a class PROPERTY -- a cursor that is a class member, which
// is what UVM writes -- a class handle lands inside a possibly-promoted object
// through a path the barrier has to be taught about separately.
//
// Isolating that site takes some care, because a barrier is only load-bearing while
// the object it protects is still YOUNG. The source map therefore lives in a
// second, YOUNG Bag built inside the function: storing into it logs nothing (the
// inline test wants OLD), and the map dies with it at return, so after `carry`
// returns the one key object is designated by `bag.cursor` and by nothing else in
// the process. `dst.id = ok` is an int store and emits no barrier, so no other
// store in this design can log `bag` -- which makes `remembered > 0` here the
// narrowest assertion in the section: it names exactly one emission site.
//
// One entry, not four: LRM 7.8.3 leaves handle-key traversal order arbitrary, so
// with several entries the case could not say WHICH object it carried.
//
// `cursor` must stay declared as `Cell`: the member selection keeps the owner only
// for a member whose type transitively reaches a class handle (agg_member,
// ir/llvm/Aggregates.cpp, is_leaf_type), so a cursor "simplified" to an int or a string clears the
// owner and the barrier can never fire however OLD `bag` is. The value check is
// what covers that -- a vacuous version of this case fails on the $display rather
// than passing quietly -- but this is the line not to touch.
//
// Failure mode: the next young collection frees the carried key while
// `bag.cursor` points at it, the filler recycles the line, and the $display reads
// 1515870810 junk instead of 4242 carried-key.
TEST_CASE("gc: a key read back into a property of a promoted object survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Bag;
                string name;
                int    id;
                Cell   cursor;
                int    keyed[Cell];
            endclass
            Bag  bag;
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 96; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 128; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            function automatic void carry(input Bag dst, input int v);
                Bag  src;
                Cell c;
                int  ok;
                src = new;
                src.name = "young-source-that-dies";
                c     = new;
                c.v   = v;
                c.tag = "carried-key";
                src.keyed[c] = v * 2;
                c   = null;
                ok  = src.keyed.first(dst.cursor);
                dst.id = ok;
                src = null;
            endfunction
            initial begin
                bag      = new;
                bag.name = "long-lived-bag";
                repeat (16) @(posedge clk);
                carry(bag, 4242);
                repeat (16) @(posedge clk);
                $display("%0d %0d %s", bag.id, bag.cursor.v, bag.cursor.tag);
            end
        endmodule
    )";
    const std::string expected = "1 4242 carried-key\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    CHECK(yg.promoted > 0u);
    CHECK(yg.remembered > 0u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.remembered > 0u);
}

// The owner not being present at the store site at all. `ref Cell r[$]` bound to
// `bag.q` is a raw interior pointer into the object body (:591-597), and the push
// happens on the far side of a `#20` -- so the collection that ran during the delay
// has already drained the remembered set and cleared LOGGED, and at the push
// nothing syntactically names `bag`.
//
// Two mechanisms can retain the pushed object and this case cannot tell them apart,
// which is why it asserts survival and not `remembered`: the barrier at the push,
// if the owner is recovered from the interior ref; or the conservative scan of the
// suspended task frame, which resolves that interior pointer to the owner
// (rt_gc_find_object) and lets the collector re-log it -- and a collector-side
// re-log is deliberately NOT counted in `remembered` (Gc.cpp:1679-1682), which is
// the counter's whole point. So `remembered > 0` would be asserting the wrong of
// the two. The honest statement is the one at :605-609: the case fails loudly if
// NEITHER mechanism holds.
//
// Failure mode: the fresh Cell is pushed into a malloc'd ring owned by an object
// that no young collection revisits, the next young collection frees it, and the
// readback prints 1515870810 junk.
TEST_CASE("gc: a handle pushed through a ref into a promoted object's queue survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Bag;  string name; Cell q[$]; endclass
            Bag  bag;
            Junk sink;
            int  observed = 0;
            // The push is AFTER the suspension on purpose: before it, the call site
            // and the store sit in one activation and any barrier at either covers
            // both. After it, they are separated by a collection.
            task automatic stash_later(ref Cell r[$], input int v);
                Cell f;
                #20;
                f        = new;
                f.v      = v;
                f.tag    = "pushed-after-suspend";
                r.push_back(f);
                f        = null;
                observed = r.size();
            endtask
            initial begin
                for (int i = 0; i < 80; i++) begin
                    #1;
                    for (int k = 0; k < 128; k++) begin
                        sink = new;
                        sink.v   = 32'h5A5A5A5A;
                        sink.tag = "junk";
                    end
                end
            end
            initial begin
                bag      = new;
                bag.name = "long-lived-bag";
                #20;
                stash_later(bag.q, 4242);
                #20;
                $display("%0d %0d %s", observed, bag.q[0].v, bag.q[0].tag);
            end
        endmodule
    )";
    const std::string expected = "1 4242 pushed-after-suspend\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    // `bag` was promoted before the call, so the push really did cross the old
    // boundary rather than landing in a young object.
    CHECK(yg.promoted > 0u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
}

// The variant of the case above where the frame scan cannot help, which is the one
// that needed new code rather than a new test. `ref Cell slot` bound to `bag.q[3]`
// is an interior pointer into the QUEUE'S malloc'd payload, not into the object
// body: rt_gc_find_object rejects it (it is not in a block at all), so the
// conservative scan of the suspended frame resolves nothing and the owner is never
// re-logged. The barrier at the call site logs `bag`, but the collection that runs
// during the `#20` drains that entry, and the assignment lands after it.
//
// What carries it is the ROOT path, not the barrier: the call registers the OUTER
// container -- `bag.q`, whose handle sits at a fixed address in the object body --
// as a dynamically-scoped container root for the call's duration, and removes it on
// every completion and resume path. The outer one and not the element, because an
// element address moves when the ring grows and the collector would then read freed
// memory: the same wild read the registration exists to remove. The same
// registration covers a `ref` to a NESTED container (`bag.qq[3]`), whose elements
// the collector reaches from the outer handle anyway (Gc.cpp:1266-1271).
//
// `remembered` is deliberately NOT asserted, and asserting it would be worse than
// useless: the barrier does fire at the call site, so the counter moves whether or
// not the registration exists, and the case would keep passing if the registration
// regressed. `container_roots` is not usable either -- Gc.h:424-428 defines it as
// the containers consulted in the LAST collection, and by then the call has
// returned and the registration is gone. So the value check is the oracle here.
//
// Failure mode: the fresh Cell is reachable only from a ring slot inside an object
// no young collection revisits. In a build with assertions the collector's
// old-edge verification catches it as an un-remembered OLD object holding a young
// reference; without them, the filler recycles the line and the readback prints
// 1515870810 junk.
TEST_CASE("gc: a handle assigned through a ref to a queue ELEMENT of a promoted object survives") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Bag;  string name; Cell q[$]; endclass
            Bag  bag;
            Junk sink;
            int  observed = 0;
            function automatic void install();
                Cell ph;
                bag      = new;
                bag.name = "long-lived-bag";
                for (int i = 0; i < 4; i++) begin
                    ph     = new;
                    ph.v   = 100 + i;
                    ph.tag = "placeholder";
                    bag.q.push_back(ph);
                end
            endfunction
            task automatic replace_later(ref Cell slot, input int v);
                Cell f;
                #20;
                f        = new;
                f.v      = v;
                f.tag    = "assigned-after-suspend";
                slot     = f;
                f        = null;
                observed = v;
            endtask
            initial begin
                for (int i = 0; i < 80; i++) begin
                    #1;
                    for (int k = 0; k < 128; k++) begin
                        sink = new;
                        sink.v   = 32'h5A5A5A5A;
                        sink.tag = "junk";
                    end
                end
            end
            initial begin
                install();
                #20;
                replace_later(bag.q[3], 4242);
                #20;
                $display("%0d %0d %s %0d %s", observed, bag.q[3].v, bag.q[3].tag,
                         bag.q.size(), bag.q[0].tag);
            end
        endmodule
    )";
    const std::string expected = "4242 4242 assigned-after-suspend 4 placeholder\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    // The owner and the three surviving placeholders were all promoted before the
    // call, so the assignment really did cross the old boundary.
    CHECK(yg.promoted >= 5u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
}

// Two refs into the SAME container, live at once. The registration above is keyed on
// the outer container handle's ADDRESS, which for a heap container belongs to the
// owner rather than to the activation -- so two ref actuals aiming into `bag.q`
// register the same address twice and both must stay registered until the last
// holder is done with it. A registry that enforces one registration per address is
// correct for a frame-local container, whose address is a per-activation alloca,
// and wrong here: the first removal deregisters a container the other ref is still
// aimed into, and the collector then frees what that ref wrote.
//
// Both refs belong to one suspended activation rather than two forked processes,
// which is the same registry path with less scheduling in the way: two adds before
// the call, two removes at the single completion block.
//
// Failure mode, in the two builds: with assertions live a one-shot registry trips
// its own duplicate-registration assertion at the second add, before any collection
// happens; without them, one of the two fresh objects is freed while a queue slot
// still points at it and the readback prints 1515870810 junk for whichever slot lost.
TEST_CASE("gc: two refs into one container of a promoted object both stay registered") {
    constexpr std::string_view src = R"(
        module m;
            class Cell; int v; string tag; endclass
            class Junk; int v; string tag; endclass
            class Bag;  string name; Cell q[$]; endclass
            Bag  bag;
            Junk sink;
            int  observed = 0;
            function automatic void install();
                Cell ph;
                bag      = new;
                bag.name = "long-lived-bag";
                for (int i = 0; i < 5; i++) begin
                    ph     = new;
                    ph.v   = 100 + i;
                    ph.tag = "placeholder";
                    bag.q.push_back(ph);
                end
            endfunction
            task automatic fill_two(ref Cell a, ref Cell b, input int v);
                Cell f;
                Cell g;
                #20;
                f        = new;
                f.v      = v;
                f.tag    = "first-slot";
                g        = new;
                g.v      = v + 1;
                g.tag    = "second-slot";
                a        = f;
                b        = g;
                f        = null;
                g        = null;
                observed = v;
            endtask
            initial begin
                for (int i = 0; i < 80; i++) begin
                    #1;
                    for (int k = 0; k < 128; k++) begin
                        sink = new;
                        sink.v   = 32'h5A5A5A5A;
                        sink.tag = "junk";
                    end
                end
            end
            initial begin
                install();
                #20;
                fill_two(bag.q[3], bag.q[4], 4242);
                #20;
                $display("%0d %0d %s %0d %s %0d %s", observed, bag.q[3].v, bag.q[3].tag,
                         bag.q[4].v, bag.q[4].tag, bag.q.size(), bag.q[0].tag);
            end
        endmodule
    )";
    const std::string expected =
        "4242 4242 first-slot 4243 second-slot 5 placeholder\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    // The owner plus five placeholders, all live at the first collection.
    CHECK(yg.promoted >= 6u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
}

// The claim the phase exists for (§7, §12 P3 acceptance): a young collection's
// cost is independent of the stable set's size. 500 nodes in a chain hung off one
// module-scope handle, plus eight objects allocated and dropped per cycle for 100
// cycles under stress.
//
// The stable set is a chain and not a design-state queue on purpose. A queue of
// 500 handles is a container ROOT (§3.3), so its elements are visited at every
// collection whether or not they are OLD and stopping at OLD saves one load per
// element: nothing worth measuring. Behind a single root handle the young
// collection examines exactly one word, finds OLD, and never sees the other 499
// nodes -- which is the mechanism §7 calls "young-collection cost independent of
// stable-set size".
//
// What the counters can express, exactly: after a young collection objects_live
// and bytes_live hold what THAT collection marked, and an un-remembered OLD
// object is neither marked nor counted -- not even one a precise root names
// directly. So a young-only run must report a live count of the same order as one
// cycle's churn, and a run of the same design where every collection is full must
// report at least the 500. A regression that lets the young mark walk through an
// OLD object shows up as the first number jumping to >= 500.
//
// What they cannot express: the cost itself. Nothing in rt_gc_stats counts
// objects visited, lines scanned or blocks skipped, so "the young collection did
// not touch the stable set" is asserted through what it counted as live, not
// through work done; `promoted` cannot substitute, because OLD is a one-time
// transition and a young collection that wrongly traced through OLD objects
// reports exactly the same `promoted` as one that stopped at them.
TEST_CASE("gc: a young collection does not mark the stable population") {
    constexpr std::string_view src = R"(
        module m;
            class Node; int v; string tag; Node next; endclass
            Node head;
            Node keep;
            int  made = 0;
            function automatic void build();
                Node n;
                for (int i = 0; i < 500; i++) begin
                    n      = new;
                    n.v    = i;
                    n.tag  = "stable";
                    n.next = head;
                    head   = n;
                end
            endfunction
            function automatic void churn(input int c);
                Node n;
                for (int i = 0; i < 8; i++) begin
                    n     = new;
                    n.v   = c * 8 + i;
                    n.tag = "churn";
                    made  = made + 1;
                end
                keep = n;
            endfunction
            // Bounded and null-guarded rather than `while (n != null)`: a chain
            // broken by a wrong collection must show up as a wrong sum here, not
            // as a null-handle fatal that takes the test binary with it.
            function automatic int walk();
                Node n;
                int  s = 0;
                n = head;
                for (int i = 0; i < 500; i++) begin
                    if (n != null) begin
                        s = s + n.v;
                        n = n.next;
                    end
                end
                return s;
            endfunction
            initial begin
                build();
                for (int c = 0; c < 100; c++) begin
                    churn(c);
                    #1;
                end
                $display("%0d %0d %s %0d", walk(), made, head.tag, keep.v);
            end
        endmodule
    )";
    // 0 + 1 + ... + 499, the 800 churned, and the last churn object's value.
    const std::string expected = "124750 800 stable 799\n";
    check_both_cadences(src, expected);

    // An explicit full_every wins over stress, so this is "collect at every
    // region boundary, and after the first one always young".
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_stress, nursery_bytes_small, full_every_never);
    CHECK(yg.collections >= 50u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections >= 49u);
    // The first (full) collection promoted the whole chain, which is what makes
    // the bound below a statement about OLD objects rather than about young ones.
    CHECK(yg.promoted >= 500u);
    // Eight allocated per cycle with one kept, so the young survivors are single
    // digits plus whatever a frame word still designates. 64 is a wide margin and
    // still an order of magnitude under the 500 a whole-heap mark would report.
    CHECK(yg.objects_live <= 64u);
    CHECK(yg.bytes_live <= 64u * 1024u);

    // The contrast, same design and same allocation: when every collection walks
    // everything, the same counter reports the whole live set. Without this half
    // the bound above would also pass on a collector whose live count is broken.
    const rt_gc_stats fl =
        run_stats(src, expected, cadence_stress, nursery_bytes_small, 1u);
    CHECK(fl.collections >= 50u);
    CHECK(fl.full_collections == fl.collections);
    CHECK(fl.young_collections == 0u);
    CHECK(fl.objects_live >= 500u);
}

// The generational trade, stated as a test: old garbage is NOT reclaimed by a
// young collection, and IS reclaimed by a full one. 200 records with a `string`
// property are built, promoted by the first collection, and then dropped by
// clearing the design-state queue that held them; from that point they are
// unreachable OLD objects and every remaining collection in the young-only run is
// a young one.
//
// `finalized` is the oracle because it counts finalizers run BY A COLLECTION
// (Gc.h:401-404) and only `Rec` has one: `Churn`, the filler, holds no managed
// property, so its class map carries no finalize function and its reclamation
// contributes nothing to this counter. So `finalized == 0` here means precisely
// "no young collection reclaimed any of the 200", and it is paired with
// `collections`, `young_collections` and `promoted` so that it cannot pass on a
// run that never collected or never promoted.
//
// Failure mode in the young-only direction: a collector that sweeps an unmarked
// OLD object -- the second and third hazards named at Gc.h:87-93, the finalizer
// sweep and the large-object sweep -- reclaims live old data in every young
// collection, and this design finalizes 200 objects that are merely unmarked. In
// the full direction, a collector that never clears OLD leaks the old generation
// forever, which no value check can show.
TEST_CASE("gc: old garbage survives young collections and is reclaimed by a full one") {
    constexpr std::string_view src = R"(
        module m;
            class Churn; int a; int b; endclass
            class Rec;   int v; string name; endclass
            Rec   keep[$];
            Churn sink;
            bit   clk;
            int   dropped = 0;
            initial begin
                clk = 0;
                for (int i = 0; i < 160; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 128; i++) begin
                    sink   = new;
                    sink.a = 32'h5A5A5A5A;
                    sink.b = 32'h5A5A5A5A;
                end
            end
            function automatic void build();
                Rec r;
                for (int i = 0; i < 200; i++) begin
                    r      = new;
                    r.v    = i;
                    r.name = "an-old-record-with-a-heap-buffer-of-its-own";
                    keep.push_back(r);
                end
            endfunction
            initial begin
                build();
                repeat (16) @(posedge clk);
                dropped = keep.size();
                keep.delete();
                repeat (48) @(posedge clk);
                $display("dropped %0d", dropped);
            end
        endmodule
    )";
    const std::string expected = "dropped 200\n";
    check_both_cadences(src, expected);

    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    CHECK(yg.promoted >= 200u);
    CHECK(yg.finalized == 0u);

    // The same design with every collection full. The 200 are garbage from the
    // delete() onwards and there are collections either side of it, so all but at
    // most a handful must be finalized; the margin is one collection of lag.
    const rt_gc_stats fl = run_stats(src, expected, cadence_ordinary, nursery_bytes_small, 1u);
    CHECK(fl.collections > 1u);
    CHECK(fl.full_collections == fl.collections);
    CHECK(fl.young_collections == 0u);
    CHECK(fl.promoted >= 200u);
    CHECK(fl.finalized >= 190u);
}

// Promotion is per OBJECT, reclamation is per LINE, and a 128-byte line holds four
// of these objects (Gc.h:37-40): `Tiny` is an 8-byte header plus a vptr and an
// int, 32 bytes once rounded to RT_GC_ALIGN. So the keeper necessarily shares its
// line with objects that die immediately, and four same-shaped neighbours on each
// side make that deterministic rather than lucky -- whichever of the four slots
// the bump allocator hands the keeper, at least one other slot in the same line
// holds a dead Junk.
//
// This is the first hazard at Gc.h:87-91. A young collection does not mark the
// promoted keeper, so its line's mark does not equal the epoch and the line looks
// free by the ordinary rule; only the per-line `line_old` record says otherwise.
// A collector missing it hands the line to the filler, which writes 0x5A5A5A5A
// over the keeper's own field -- 1515870810 instead of 42, in the same line, at
// the same alignment, with no allocator misbehaviour anywhere.
//
// No `string` property here, deliberately: 32 bytes is the whole point of the
// case, and an rt_string is 32 on its own. That costs the ASan half of §2's
// device, so this case's only signal is the value.
TEST_CASE("gc: a promoted object sharing a line with dead young objects survives") {
    constexpr std::string_view src = R"(
        module m;
            class Tiny; int v; endclass
            class Junk; int v; endclass
            Tiny keeper;
            Junk sink;
            bit  clk;
            initial begin
                clk = 0;
                for (int i = 0; i < 96; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 256; i++) begin
                    sink   = new;
                    sink.v = 32'h5A5A5A5A;
                end
            end
            function automatic void place();
                Junk pad;
                for (int i = 0; i < 4; i++) begin
                    pad   = new;
                    pad.v = 32'h5A5A5A5A;
                end
                keeper   = new;
                keeper.v = 42;
                for (int i = 0; i < 4; i++) begin
                    pad   = new;
                    pad.v = 32'h5A5A5A5A;
                end
                pad = null;
            endfunction
            initial begin
                place();
                repeat (24) @(posedge clk);
                $display("%0d", keeper.v);
            end
        endmodule
    )";
    const std::string expected = "42\n";
    check_both_cadences(src, expected);
    const rt_gc_stats yg =
        run_stats(src, expected, cadence_ordinary, nursery_bytes_small, full_every_never);
    CHECK(yg.collections > 1u);
    CHECK(yg.full_collections == 1u);
    CHECK(yg.young_collections > 0u);
    // Without this the case would pass on a collector that never promotes: the
    // keeper would be an ordinary marked young object and its line marked with it.
    CHECK(yg.promoted > 0u);
    // Non-vacuity for the poison device rather than a claim about recycling: the
    // filler allocates 256 objects of the keeper's own size class per negedge, and
    // if it did not run at all no freed line is ever handed back out and the value
    // check cannot fail however the collector behaves.
    CHECK(yg.objects_allocated >= 8u * 256u);
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0u);
    CHECK(st.promoted > 0u);
}

// `--gc-stress` must stay the strongest correctness cadence -- with no uncollected
// run left to compare against, it is the only differential partner the ordinary
// cadence has -- and generational collection splits the collector into two paths
// that can fail independently: a stress cadence that only ever ran one of them
// would leave the other tested by the budget path alone. So stress with no
// explicit `full_every` alternates -- full, young, full, young -- and this case
// pins that, both counters advancing at roughly half the collections rather than
// merely being nonzero.
//
// It also pins the accounting identity Gc.h:408-409 states: young + full is
// exactly `collections`, so no collection is of a third kind and none is counted
// twice. Failure mode: a stress cadence that never selects one kind, or a `reason`
// and `kind` argument transposed at a call site, which would show up here as one
// counter at zero while the sum still held.
TEST_CASE("gc: --gc-stress alternates young and full collections") {
    constexpr std::string_view src = R"(
        module m;
            class Item; int v; string tag; endclass
            Item keep;
            Item owner;
            int  made = 0;
            function automatic void one_cycle(input int c);
                Item fresh;
                for (int i = 0; i < 8; i++) begin
                    fresh     = new;
                    fresh.v   = c * 8 + i;
                    fresh.tag = "churn";
                    made      = made + 1;
                end
                keep = fresh;
            endfunction
            initial begin
                owner     = new;
                owner.v   = 1;
                owner.tag = "stress-owner";
                for (int c = 0; c < 60; c++) begin
                    one_cycle(c);
                    #1;
                end
                $display("%0d %0d %s", made, keep.v, owner.tag);
            end
        endmodule
    )";
    const std::string expected = "480 479 stress-owner\n";
    const rt_gc_stats st       = run_stats(src, expected, cadence_stress);
    CHECK(st.collections >= 30u);
    CHECK(st.young_collections + st.full_collections == st.collections);
    // Alternation, not just "both happened": one kind per collection, so each
    // count is within one of half. The full one rounds up (the first collection).
    CHECK(st.full_collections >= st.collections / 2u);
    CHECK(st.young_collections >= st.collections / 2u - 1u);
    CHECK(st.promoted > 0u);
}

// An object whose only root is an INTERFACE-class handle (LRM 8.26). The declared
// type of a handle says nothing about tracing -- it is a class handle either way,
// and the object's own trace map is reached through its descriptor -- but the
// interface regions of that descriptor sit BELOW the trace-map pointer, so this is
// the case that would break if their placement ever moved the map's offset. The
// `string` property turns a wrongly-freed object into a sanitizer
// heap-use-after-free rather than a stale read (see check_both_cadences).
TEST_CASE("gc: an object rooted only by an interface-class handle survives") {
    constexpr std::string_view src = R"(
        interface class Named;
            pure virtual function int id();
            pure virtual function string label();
        endclass
        class Item implements Named;
            int    v;
            string tag;
            virtual function int id(); return v; endfunction
            virtual function string label(); return tag; endfunction
        endclass
        module m;
            Named held;
            int   made = 0;
            function automatic void churn(input int c);
                Item fresh;
                for (int i = 0; i < 8; i++) begin
                    fresh     = new;
                    fresh.v   = c * 8 + i;
                    fresh.tag = "churn";
                    made      = made + 1;
                end
                // The concrete handle dies here; only the interface-typed root keeps
                // the last object alive.
                held = fresh;
            endfunction
            initial begin
                for (int c = 0; c < 40; c++) begin
                    churn(c);
                    #1;
                end
                $display("%0d %0d %s", made, held.id(), held.label());
            end
        endmodule
    )";
    check_both_cadences(src, "320 319 churn\n");
}

// -- 8. Built-in handle kinds: the `process` object ------------------------------
//
// docs/design/architecture.md, Memory: every built-in handle kind is a collected object
// with a finalizer for any native resource it owns. A `process` object (LRM 9.7) is a
// handle onto a thread's control block, a native record the collector never scans; the
// object is the collected half, and its finalizer drops the reference it holds on the
// block and clears the block's weak memo of it (docs/design/process-class.md §4.2).
//
// The object used to be allocated outside the heap yet typed as a class handle, so the
// first collection that found one in design state, a class property or a container was
// a fatal ("a precise GC root does not name a class object"), and nothing ever released
// a block. Each location below holds the object through a window in which the collector
// runs, with `Junk` recycling and poisoning every line a wrong collection frees.

// One process handle per kind of location a collector walks: design state, a class
// property, a queue element and an associative-array value. Each handle is the only
// reference to a distinct fork child's object, and the child has finished, so the control
// block is kept alive by the object alone: a status that still reads FINISHED afterwards
// shows the object, the block it names and the memo all survived.
TEST_CASE("gc: a process handle held only by design state, a class property, a queue or an assoc survives") {
    constexpr std::string_view src = R"(
        module m;
            class Holder; process p; endclass
            class Junk; int v; string tag; endclass
            process pmod;
            Holder  h;
            process q[$];
            process byname[string];
            Junk    sink;
            bit     clk;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            initial begin
                h = new;
                fork
                    pmod = process::self();
                    h.p = process::self();
                    q.push_back(process::self());
                    byname["k"] = process::self();
                join
                repeat (6) @(posedge clk);
                $display("%0d %0d %0d %0d", pmod.status(), h.p.status(), q[0].status(),
                         byname["k"].status());
                $display("%0d", pmod != h.p && pmod != q[0] && pmod != byname["k"] &&
                                h.p != q[0] && h.p != byname["k"] && q[0] != byname["k"]);
            end
        endmodule
    )";
    check_both_cadences(src, "0 0 0 0\n1\n");
    const rt_gc_stats st = run_stats(src, "0 0 0 0\n1\n", cadence_stress);
    CHECK(st.collections > 0);
}

// A process handle in the automatic local of a blocking task, the conservative frame-root
// path: a process body's own locals are design state, so it takes a task's coroutine frame
// to put the handle where only the frame scan sees it. `self()` must also keep naming the
// same object for as long as a handle exists: the block's memo is weak, so a collection
// that cleared it while `me` was live would make the second `self()` mint a new object,
// and the comparison would read 0.
TEST_CASE("gc: a process handle in a suspended task frame survives and stays the thread's identity") {
    constexpr std::string_view src = R"(
        module m;
            class Junk; int v; string tag; endclass
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            task automatic hold();
                automatic process me = process::self();
                repeat (3) @(posedge clk);
                $display("%0d %0d", me.status(), process::self() == me);
            endtask
            always @(posedge clk) hold();
        endmodule
    )";
    const std::string expected = "1 1\n1 1\n1 1\n1 1\n";
    check_both_cadences(src, expected);
    // Negative space: nothing precise designates the object, so a run that reports no
    // conservative pin did not reach it through the frame registry and this case has
    // stopped testing what it names.
    const rt_gc_stats st = run_stats(src, expected, cadence_stress);
    CHECK(st.collections > 0);
    CHECK(st.conservative_pins > 0);
}

// A handle captured by a fork child that outlives the frame it was declared in. The
// capture lives in a heap box, a class body whose trace map names the handle field, so
// this is the "a class field names a process object" shape with the parent gone.
TEST_CASE("gc: a process handle captured by a fork child outliving its frame survives") {
    constexpr std::string_view src = R"(
        module m;
            class Junk; int v; string tag; endclass
            bit  clk;
            Junk sink;
            initial begin
                clk = 0;
                for (int i = 0; i < 32; i++) #5 clk = ~clk;
            end
            always @(negedge clk) begin
                for (int i = 0; i < 64; i++) begin
                    sink = new;
                    sink.v   = 32'h5A5A5A5A;
                    sink.tag = "junk";
                end
            end
            task automatic spawn();
                automatic process me = process::self();
                fork
                    begin
                        repeat (4) @(posedge clk);
                        $display("box %0d", me.status());
                    end
                join_none
            endtask
            initial spawn();
        endmodule
    )";
    check_both_cadences(src, "box 0\n");
}

// The leak: every fork child that asked for its handle took a control block and an
// object, and neither was ever released (~176 B per child, linear). With the object
// collected, its finalizer returns the block to the pool, so the pool's high-water mark
// tracks the children outstanding between two collections instead of the children ever
// spawned. 12000 children against bounds of ~2000 (a 32 KiB nursery holds ~1000 of these
// objects) and ~500 (stress collects every time step, and 100 children run in each).
TEST_CASE("gc: the control blocks of fork children are released with their process objects") {
    constexpr std::string_view src = R"(
        module m;
            int done;
            initial begin
                for (int i = 0; i < 12000; i++) begin
                    fork
                        begin
                            automatic process p = process::self();
                            automatic int s = p.status();
                            if (s == 1) done++;
                        end
                    join_none
                    if (i % 100 == 99) #1;
                end
                #1;
                $display("done=%0d", done);
            end
        endmodule
    )";
    const rt_gc_stats ordinary = run_stats(src, "done=12000\n", cadence_ordinary);
    CHECK(ordinary.collections >= 5u);
    CHECK(ordinary.objects_allocated >= 12000u);
    CHECK(ordinary.finalized >= 8000u);
    CHECK(rt_proc_pool_blocks() <= 2048u);

    const rt_gc_stats stress = run_stats(src, "done=12000\n", cadence_stress);
    CHECK(stress.finalized >= 11000u);
    CHECK(rt_proc_pool_blocks() <= 512u);
}

// -- 9. Staging images are roots --------------------------------------------------
//
// `s <= #5 t` samples the right-hand side now and commits `s` five time units later
// (LRM 9.4.5). Between the two, the pending record is native memory that no root table
// describes, and the program is free to drop every other reference to what it carries:
// here `t.h = null` leaves the staged handle as the ONLY reference to its object. Each
// design lets collections run while the record is in flight and then reads the object
// back through the committed destination; the `Junk` filler recycles and poisons every
// line a wrong collection frees, and `conservative_pins` proves the record's range is
// what held the object, not a frame word.

// A struct with a handle field: one record per leaf, the handle leaf's byte image in the
// record's payload.
TEST_CASE("gc: a handle staged by a delayed nonblocking struct assignment survives") {
    constexpr std::string_view src = R"(
        module top;
            class H; int x; static function H make(); H h = new; h.x = 42; return h; endfunction endclass
            class Junk; int x; endclass
            typedef struct { int a; H h; } S_t;
            S_t s, t;
            Junk keep;
            initial begin
                t.a = 7;
                t.h = H::make();
                s <= #5 t;
                t.h = null;
                repeat (4) begin
                    for (int i = 0; i < 300; i++) begin keep = new; keep.x = 32'h5A5A5A5A; end
                    #1;
                end
                #3;
                $display("a=%0d h.x=%0d", s.a, s.h.x);
            end
        endmodule
    )";
    check_both_cadences(src, "a=7 h.x=42\n");
    const rt_gc_stats st = run_stats(src, "a=7 h.x=42\n", cadence_stress);
    CHECK(st.collections > 0);
    CHECK(st.conservative_pins > 0);
}

// An unpacked array of handles: one record per element, the handle in the record's value
// word.
TEST_CASE("gc: handles staged by a delayed nonblocking array assignment survive") {
    constexpr std::string_view src = R"(
        module top;
            class H; int x; static function H make(); H h = new; h.x = 42; return h; endfunction endclass
            class Junk; int x; endclass
            H arr[2], src[2];
            Junk keep;
            initial begin
                src[0] = H::make();
                src[1] = H::make();
                src[1].x = 43;
                arr <= #5 src;
                src[0] = null;
                src[1] = null;
                repeat (4) begin
                    for (int i = 0; i < 300; i++) begin keep = new; keep.x = 32'h5A5A5A5A; end
                    #1;
                end
                #3;
                $display("%0d %0d", arr[0].x, arr[1].x);
            end
        endmodule
    )";
    check_both_cadences(src, "42 43\n");
    const rt_gc_stats st = run_stats(src, "42 43\n", cadence_stress);
    CHECK(st.collections > 0);
    CHECK(st.conservative_pins > 0);
}

// The event-controlled form parks the record on a trigger list instead of the wheel; the
// trigger is not met until after the collections, so the record is in flight throughout.
TEST_CASE("gc: handles staged by an event-controlled nonblocking assignment survive") {
    constexpr std::string_view src = R"(
        module top;
            class H; int x; static function H make(); H h = new; h.x = 42; return h; endfunction endclass
            class Junk; int x; endclass
            typedef struct { int a; H h; } S_t;
            S_t s, t;
            H arr[2], src[2];
            Junk keep;
            bit clk;
            initial begin
                clk = 0;
                t.a = 9;
                t.h = H::make();
                src[1] = H::make();
                src[1].x = 44;
                s   <= @(posedge clk) t;
                arr <= @(posedge clk) src;
                t.h = null;
                src[1] = null;
                repeat (4) begin
                    for (int i = 0; i < 300; i++) begin keep = new; keep.x = 32'h5A5A5A5A; end
                    #1;
                end
                clk = 1;
                #1;
                $display("a=%0d h.x=%0d arr1.x=%0d", s.a, s.h.x, arr[1].x);
            end
        endmodule
    )";
    check_both_cadences(src, "a=9 h.x=42 arr1.x=44\n");
    const rt_gc_stats st = run_stats(src, "a=9 h.x=42 arr1.x=44\n", cadence_stress);
    CHECK(st.collections > 0);
    CHECK(st.conservative_pins > 0);
}
