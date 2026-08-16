// AN ACTION BLOCK THAT SUSPENDS ON SOMETHING OTHER THAN `#0`, and what it proves
// is where it comes BACK: a thread of the reactive region set that parks on a
// delay or on an event control must resume in the REACTIVE region of the slot it
// wakes in, not in that slot's Active region. diff_action_delay.sv is the same
// measurement for `#0` alone, where LRM 4.4.2.7 names the region to come back
// through; this file covers the two wake paths that have nothing to do with `#0`
// -- a timer maturing, and a signal subscription firing -- which are the two an
// implementation is most likely to route through whatever queue its ordinary
// processes use.
//
// SCORED BY FULL OUTPUT, NOT BY REPORT TIME, exactly as diff_action_delay.sv and
// diff_renba.sv are: both directives here carry a custom action block, so there
// is no report stream to normalise. Ordering within a time step IS the
// observable, so every line is stamped with `$time` and tagged: `ACT` for a
// design print in the active region set, `RCT` for a print from inside an action
// block, numbered so a human diff is unambiguous about which statement of which
// action block ran when.
//
// Clotho lowers this. It did NOT when this file was written: its reactive safety
// check (src/Lower.cpp) whitelisted only the suspension-free action block plus two
// cases the region set already handled -- a nonblocking assignment, which commits
// in Re-NBA, and a bare `#0`, which parks in Re-Inactive -- and refused everything
// else because every other wake path pushed the resumed handle onto the ACTIVE
// ring, which is the wrong region by exactly the transposition this file measures.
// That ordering is the whole point: both derivations below predate the
// implementation, so neither can be an encoding of what clotho happens to do.
//
// Run against the implementation afterwards, clotho reproduces the derivation
// EXACTLY: all 17 lines below, in that order, with every `RCT` line after the
// `ACT nba` line of its own slot, pe resuming at t=35 rather than at the t=25 edge
// it failed on, and pt's `#0` landing at t=55 beside the timer resume that
// preceded it. So the composition of 4.4.2.6 and 24.3.1 argued for below is what
// clotho implements, and none of the iverilog transposition recorded further down
// survives in it.
//
// CLAUSE NUMBERING, verified against IEEE Std 1800-2017 itself rather than taken
// on trust: 4.4.2.2 Active, 4.4.2.3 Inactive, 4.4.2.4 NBA, 4.4.2.5 Observed,
// 4.4.2.6 Reactive, 4.4.2.7 Re-Inactive, 4.4.2.8 Re-NBA. Note that Observed is
// 4.4.2.5 and NOT 4.4.2.4, which parts of docs/ get wrong.
//
// LRM 4.4.2.6, in full, because both halves of it are used here:
//
//   "The Reactive region holds the current reactive region set events being
//    evaluated and can be processed in any order."
//
//   "The code specified by blocking assignments in checkers, program blocks and
//    the code in action blocks of concurrent assertions are scheduled in the
//    Reactive region. The Reactive region is the reactive region set dual of the
//    Active region (see 4.4.2.2)."
//
// with LRM 16.14.1 saying the same thing from the assertion's side:
//
//   "The pass and fail statements of an assert statement are executed in the
//    Reactive region."
//
// and 16.14.1 also being the only clause that restricts what an action block may
// contain, which is what makes this fixture legal SystemVerilog rather than a
// probe of undefined behaviour:
//
//   "The action_block shall not include any concurrent assert, assume, or cover
//    statement. The action_block, however, can contain immediate assertion
//    statements."
//
// A delay and an event control are therefore ordinary contents of an action
// block, and neither clause says what happens when one of them SUSPENDS. The LRM
// answers that for the region set as a whole rather than for action blocks
// specifically, in 4.4.2.7 for `#0`:
//
//   "If events are being executed in the reactive region set, an explicit #0
//    delay control requires the process to be suspended and an event to be
//    scheduled into the Re-Inactive region of the current time slot so that the
//    process can be resumed in the next Re-Inactive to Reactive iteration."
//
// and in 24.3.1 for a delay and an event control, where it is said of program
// code -- the region set's other inhabitant -- and said as a property of the
// THREAD rather than of the wake site:
//
//   "Statements and constructs within a program block that are sensitive to
//    changes (e.g., update events) on design signals are scheduled in the
//    Reactive region. Consider a program that contains the statement @(clk) S1;
//    where clk is a design signal. Every transition of signal clk will cause the
//    statement S1 to be scheduled into the Reactive region. ... The standard #
//    delay operator within program blocks schedules process resumption in the
//    Reactive region."
//
//   "Once a program process starts a thread of execution, all subsequent blocking
//    statements in that thread are scheduled in the Reactive region. This includes
//    subroutine code called by the thread, even if the subroutine code is declared
//    in a module, package, or interface. Effectively, a section of sequential code
//    anywhere in the design or testbench inherits the scheduling region of the
//    thread that calls it."
//
// THE DERIVATION IS THE COMPOSITION OF THOSE, and it is worth being explicit
// that it IS a composition, because no single sentence of the LRM says "an
// action block that waits for a posedge resumes in Reactive". 4.4.2.6 makes an
// action block a thread of the reactive region set; 24.3.1 says a thread of that
// region set has its blocking statements -- both flavours, `#` and `@` -- and
// even the subroutines it calls scheduled in the Reactive region, and says it as
// a property the thread carries rather than one the waited-on signal carries.
// Applying that to the other inhabitant of the same region set is the only
// reading under which the region set is closed: the alternative has a wake put a
// reactive thread into the Active region, where 4.4.2.6's "dual of the Active
// region" and 4.4.2.7's "next Re-Inactive to Reactive iteration" would both be
// describing a region a process can leave but not stay in.
//
// TWO WAKE PATHS, one per directive, because they are separately implementable
// and separately wrong:
//
//   pe   an EVENT CONTROL, `@(posedge clk)`. The wake comes from a signal
//        subscription shared with every ordinary process waiting on that edge.
//   pt   a nonzero DELAY, `#10`. The wake comes from a timer that matures at the
//        top of a later time slot, before that slot's Active region has run --
//        which is the case where routing the resumed handle to the active ring
//        looks most natural and is most visibly wrong.
//
// pt then does a `#0` as well, so that the thread's region is checked AFTER a
// timer resume and not only at the point the action block starts: 4.4.2.7's
// Re-Inactive round trip must still land it back in Reactive at the same instant,
// which it can only do if the timer put it in Reactive to begin with.
//
// The two properties are IDENTICAL in shape on purpose. This file does not
// measure a property; a property here is only a way to get one failure at a known
// tick, so both are `antecedent |=> b` with `b` never raised, and what differs
// between the directives is what the action block does after it fails.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N). Every driver
// assignment is nonblocking, so the sampled value of a signal at a tick is also
// the value an active-region print reads at that tick:
//
//   cyc    t    a  x  b   what happens
//    0     5    0  0  0   a and x low: every attempt of both directives is
//                         vacuously true here.
//    1    15    1  0  0   a sampled high: pe's only non-vacuous attempt is armed,
//                         and it needs b at cyc2.
//    2    25    0  0  0   b is low at that obligation tick, so pe FAILS here. Its
//                         action block prints and then waits for a posedge --
//                         which cannot be this one: 16.14.1 runs the action block
//                         in the Reactive region, and clk changed back in the
//                         Active region of this same slot, so the wait starts
//                         after the only edge that could have satisfied it.
//    3    35    0  1  0   x sampled high: pt's only non-vacuous attempt is armed,
//                         and it needs b at cyc4. What ALSO lands here is pe's
//                         RESUMPTION, on the first posedge after the one it began
//                         waiting on at cyc2.
//    4    45    0  0  0   b is low at pt's obligation tick, so pt FAILS here. Its
//                         action block prints and then delays 10.
//    5    55    0  0  0   pt's timer matures at the top of this slot and its
//                         action block RESUMES, then makes a `#0` round trip
//                         through Re-Inactive and prints again at this same time.
//                         LAST EVALUATED TICK.
//    -    56    -  -   $finish.
//
// $finish is at t=56 rather than on a tick, so the last tick's regions all run to
// completion and nothing races the end of the run. The sibling report-time
// fixtures $finish ON an unevaluated tick, which a full-output fixture cannot
// afford: it would make the last edge print and the $finish two active-region
// events in different processes, and 4.4.2.2 leaves those unordered.
//
// Derived from the LRM. 17 lines, in this order:
//
//   [5]  ACT edge cyc=0 a=0 x=0 b=0
//   [5]  ACT nba  cyc=1
//   [15] ACT edge cyc=1 a=1 x=0 b=0
//   [15] ACT nba  cyc=2
//   [25] ACT edge cyc=2 a=0 x=0 b=0
//   [25] ACT nba  cyc=3
//   [25] RCT pe 1 fail, waiting for a posedge
//   [35] ACT edge cyc=3 a=0 x=1 b=0
//   [35] ACT nba  cyc=4
//   [35] RCT pe 2 resumed on the posedge
//   [45] ACT edge cyc=4 a=0 x=0 b=0
//   [45] ACT nba  cyc=5
//   [45] RCT pt 1 fail, delaying 10
//   [55] ACT edge cyc=5 a=0 x=0 b=0
//   [55] ACT nba  cyc=6
//   [55] RCT pt 2 resumed after #10
//   [55] RCT pt 3 resumed after #0
//   ($finish banner)                and NOTHING after it
//
// THAT ORDER IS TOTAL, and arranging for it cost the fixture a third directive.
// 4.4.2.6 says reactive events "can be processed in any order", so any two
// reactive lines at one time stamp are an unordered PAIR and a full-output diff
// cannot score them. So no two reactive threads are ever live at the same instant
// here: pe fails at cyc2 and resumes at cyc3, pt fails at cyc4 and resumes at
// cyc5, and the only instant with two reactive lines is t=55, where both belong
// to one thread and are ordered by that thread's own control flow. A `fork`/`join`
// in an action block is the third wake path an implementation has to
// parameterise -- the join record -- and it is NOT here for exactly this reason:
// its children cannot be given distinct resume instants in one trace without
// putting two reactive threads on one tick. It wants its own fixture.
//
// WHAT EACH LINE MEASURES:
//
//   THE TWO ACT LINES PER TICK are the region ruler, and their own order is fixed
//   rather than chosen: `ACT edge` is the posedge process, which runs in the
//   Active region and reads `cyc` before its own nonblocking increment commits;
//   `ACT nba` is `always @(cyc)`, which cannot be woken until that increment
//   commits in the NBA region (4.4.2.4) and so runs in a later pass of the Active
//   region. `ACT nba` is therefore the LAST active-region line of its tick.
//
//   EVERY `RCT` LINE MUST SORT AFTER BOTH OF THEM, and that is the whole
//   measurement. A wake routed to the active ring instead is not a missing line
//   but a TRANSPOSITION of two lines at one time stamp: a resumption already
//   queued at the top of the slot (pt's timer) or queued by the edge itself (pe's
//   subscription) is processed before the NBA region that creates the `@(cyc)`
//   event, so `RCT pe 2` would print between `ACT edge cyc=3` and `ACT nba
//   cyc=4`, and `RCT pt 2` between `ACT edge cyc=5` and `ACT nba cyc=6`. Such an
//   implementation loses the `#0` line's placement as well, and loses it as a
//   CONSEQUENCE rather than as a second bug: a `#0` executed from a thread that
//   came back through the Active region parks in Inactive (4.4.2.3) rather than
//   Re-Inactive, so `RCT pt 3` would print before `ACT nba cyc=6` too. That is
//   what makes pt's third line worth its space -- it re-measures the region after
//   a wake has already been taken, where diff_action_delay.sv measures it only at
//   the point the action block starts.
//
//   THE TIMES 35 AND 55 are the wake ARITHMETIC, independent of the region: pe
//   resumes on the FIRST posedge after the one it failed on, because the action
//   block runs in Reactive and the edge that failed the assertion is already over
//   there, and pt resumes exactly 10 later. An implementation that satisfies pe's
//   wait with its own edge prints `RCT pe 2` at t=25, which is a different bug
//   from putting it in the wrong region at t=35, and is told apart from it by
//   the time stamp alone.
//
//   EACH `RCT` LINE APPEARS ONCE. Both properties have exactly one non-vacuous
//   attempt, so a directive that reports per tick, or that merges attempts, shows
//   up as a repeated action block rather than as a mis-ordered one. Six ticks are
//   evaluated, so twelve attempts start in this file and ten of them are vacuous
//   (`a` and `x` are each low at every tick but one), which 16.14.1 gives no
//   action block: `else` runs "when the
//   property for the assert statement is evaluated to be false", and a vacuous
//   pass is not that.
//
// DELIBERATELY NOT HERE:
//
//   * A `wait` and a `@(negedge clk)`, which are the same two wake paths reached
//     by different syntax and would add lines without adding a measurement.
//   * A delay whose amount is not a constant. Once the wake carries the region,
//     all three amounts -- literal zero, literal nonzero, and a run-time `#d` --
//     are one case, so a run-time amount adds a line and no measurement: it is the
//     same timer path with the number computed later.
//   * A suspending action block on a STRONG property, which is a genuinely
//     different check rather than a redundant one. A weak directive gets one copy
//     of its action block, the coroutine the verdict pends; a strong one also gets
//     an end-of-trace copy, which is resumed once and destroyed and so may not
//     suspend AT ALL. Both properties here are bare implications in an `assert`,
//     which 16.12.2 makes weak, so this file exercises the pended copy only. The
//     end-of-trace copy wants a fixture whose header can quote 16.12.2 at it.
//   * A nonblocking assignment, which is diff_renba.sv, and a bare `#0`, which is
//     diff_action_delay.sv. This file is the gap between them.
//
// PART OF THIS IS ALREADY CONFIRMED INDEPENDENTLY, and it is worth saying
// which. iverilog -g2012 has no SVA and no reactive region set, but it does have
// the stimulus and the wake arithmetic: run over this module with the two action
// blocks replaced by ordinary processes started where the Reactive region of the
// failing tick is -- pe one time unit after cyc2's edge, which is the LRM's "the
// edge is already over", and pt at the top of cyc4's slot so its `#10` is
// unshifted -- it reproduces every ACT line and every value in the table, and it
// resumes pe at t=35, pt at t=55, and pt's `#0` at t=55. So the waveform, the two
// failing ticks and both wake times are confirmed independently, and what remains
// unconfirmed is exactly the REGION.
//
// That run also demonstrates the wrong-region signature is real rather than
// hypothetical, because it IS the wrong region: having no reactive region set,
// iverilog puts all four resumption lines before the `ACT nba` line of their own
// slot -- `RCT pe 2` before `ACT edge cyc=3`, `RCT pt 2` before `ACT edge cyc=5`,
// `RCT pt 3` between `ACT edge cyc=5` and `ACT nba cyc=6` -- which is the exact
// transposition described above, produced by an implementation doing exactly
// what one that wakes a reactive thread onto the active ring would do.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 0;  // arms pe's attempt at cyc1
  bit          x   = 0;  // arms pt's attempt at cyc3
  bit          b   = 0;  // never raised: that is what makes an armed attempt fail
  int unsigned cyc = 0;

  // The active-region ruler, two lines per tick. The first runs in the Active
  // region and so reads `cyc` before its own nonblocking increment commits; the
  // second cannot run until that increment commits in the NBA region, so it is
  // the LAST active-region line of the tick and every reactive-region line of the
  // same tick must sort after it.
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] ACT edge cyc=%0d a=%b x=%b b=%b", $time, cyc, a, x, b);
  end
  always @(cyc) $display("[%0t] ACT nba  cyc=%0d", $time, cyc);

  // Wake path 1: an event control. The action block runs in the Reactive region
  // of cyc2 (16.14.1), by which time this tick's posedge is over, so the wait is
  // satisfied by the NEXT one -- in the Reactive region of cyc3.
  pe: assert property (@(posedge clk) a |=> b)
      else begin
        $display("[%0t] RCT pe 1 fail, waiting for a posedge", $time);
        @(posedge clk);
        $display("[%0t] RCT pe 2 resumed on the posedge", $time);
      end

  // Wake path 2: a timer, which matures at the top of a later time slot -- before
  // that slot's Active region has run, which is where routing the resumed handle
  // to the active ring looks most natural. The `#0` afterwards checks the thread
  // is STILL reactive once a timer has resumed it: 4.4.2.7's Re-Inactive round
  // trip must land it back in Reactive at the same instant.
  pt: assert property (@(posedge clk) x |=> b)
      else begin
        $display("[%0t] RCT pt 1 fail, delaying 10", $time);
        #10;
        $display("[%0t] RCT pt 2 resumed after #10", $time);
        #0;
        $display("[%0t] RCT pt 3 resumed after #0", $time);
      end

  initial begin
    @(posedge clk);   // cyc0 t=5
    a <= 1;           // sampled high at cyc1: arms pe's attempt, which needs b at cyc2
    @(posedge clk);   // cyc1 t=15
    a <= 0;
    @(posedge clk);   // cyc2 t=25   pe fails here and starts waiting for a posedge
    x <= 1;           // sampled high at cyc3: arms pt's attempt, which needs b at cyc4
    @(posedge clk);   // cyc3 t=35   pe resumes here
    x <= 0;
    @(posedge clk);   // cyc4 t=45   pt fails here and delays 10
    @(posedge clk);   // cyc5 t=55   pt resumes here, twice
    #1;               // off-tick, so nothing races the end of the run
    $finish;          // t=56
  end
endmodule
