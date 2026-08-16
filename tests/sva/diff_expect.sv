// The `expect` statement: a procedural blocking property, and the region its
// continuation resumes in.
//
// SCORED BY FULL OUTPUT, NOT BY REPORT TIME. Every other fixture in this
// directory measures a set of report cycles, because for a concurrent assertion
// that is the whole observable. An `expect` has a different one: it is a
// PROCEDURAL BLOCKING statement, so what it is observed by is the ORDER and the
// TIME of what its process does afterwards. There is no report set to normalise
// -- each directive here carries a custom action block -- so the file is scored
// the way diff_action_delay.sv and diff_renba.sv are, by comparing full output.
// Every line below is stamped with `$time` and carries a tag and a number: `ACT`
// for a design print in the active region set, `EXP` for the process executing
// the expects, and the number names the exact statement, so a human diff is
// unambiguous about both order and which branch of an action block ran. The
// `$finish` banner matters here only as a POSITION -- which side of it a line
// falls on.
//
// Clotho lowers this. It did NOT when this file was written -- `expect` needed a
// blocking wait whose resume lands in the reactive region set, which is a
// scheduler change rather than a lowering one, and src/Lower.cpp still carried the
// TODO saying so -- and that ordering is the whole point: every expectation below
// EXCEPT the fourth expect was derived from the LRM before the implementation
// existed, so it cannot be an encoding of what clotho happens to do. The one
// exception is labelled where it appears and again in the measurements.
//
// Run against the implementation afterwards, clotho reproduces the derivation
// EXACTLY: all 28 lines below, in that order, with the same time stamps and the
// same branch of every action block, and nothing after the $finish banner. That
// last part is a verdict on the one question the LRM does not settle -- see the
// three dispositions of a pending expect at the end of this header -- and clotho
// picked (a), which this file predicted rather than the LRM settling it.
//
// CLAUSE NUMBERING, verified against IEEE Std 1800-2017 itself rather than taken
// on trust: the Expect statement is 16.17 (16.16 is Clock resolution, 16.18 is
// Clocking blocks and concurrent assertions). The region names this file leans
// on are 4.4.2.5 Observed, 4.4.2.6 Reactive and 4.4.2.7 Re-Inactive; note that
// 4.4.2.4 is the NBA region, NOT Observed, which parts of docs/ get wrong.
//
// LRM 16.17, the three sentences this fixture is built on, verbatim:
//
//   "The expect statement accepts the same syntax used to assert a property. An
//    expect statement causes the executing process to block until the given
//    property succeeds or fails. The statement following the expect is scheduled
//    to execute after processing the Observed region in which the property
//    completes its evaluation. When the property succeeds or fails, the process
//    unblocks, and the property stops being evaluated (i.e., no property
//    evaluation is started until that expect statement is executed again)."
//
//   "When executed, the expect statement starts a single thread of evaluation for
//    the given property on the subsequent clocking event, that is, the first
//    evaluation shall take place on the next clocking event. If the property
//    fails at its clocking event, the optional else clause of the action block is
//    executed. If the property succeeds, the optional pass statement of the
//    action block is executed."
//
//   "The expect statement can appear anywhere a wait statement (see 9.4.3) can
//    appear."
//
// Five separate requirements, and each one is a way an implementation can be
// wrong while looking right:
//
//   BLOCKING     the statement following the expect does not run until the
//                property resolves. Visible only if clock ticks PASS while the
//                process is blocked, so every expect below resolves two clocking
//                events after it executes and the ticks in between print their
//                own lines.
//   REGION       the continuation is scheduled "after processing the Observed
//                region in which the property completes its evaluation" -- so it
//                lands in the reactive region set of THAT time slot (4.4.2.6),
//                after everything the active region set does at that instant, and
//                not in the next time slot. Visible only against a print made in
//                the active region set at the resolving instant, which is what
//                the two ACT lines per tick are for.
//   SINGLE       one thread of evaluation per execution, not one attempt per
//                tick. An `assert property` starts an attempt at every clocking
//                event; an expect starts exactly one and stops evaluating when it
//                resolves. Visible as a COUNT: each action line below appears
//                exactly once.
//   OFFSET       the first evaluation is on the NEXT clocking event, not the
//                current one. Visible in the time of the resolution, which is why
//                the first three expects here execute at an off-edge time.
//   RE-EXECUTION the two requirements above COMPOSE, and the composition is not
//                obvious: an expect re-executed from the continuation of a
//                previous one runs in the Reactive region of the tick that
//                resolved it, so that tick's Observed region is ALREADY
//                PROCESSED, and "the next clocking event" for the new attempt is
//                the FOLLOWING tick rather than the one whose region it is
//                standing in. expect2b measures exactly that, and it is the one
//                case in this file whose expectation was not written blind: see
//                the note on it below.
//
// LATENCY IS NOT THE VERDICT, which is the third of the properties this fixture
// is meant to pin. A passing expect and a failing one differ in WHICH branch of
// the action block runs and in nothing else: both resolve on the second clocking
// event after the statement executed, and both resume in the reactive region set
// of that instant. expect1 and expect2 below are the same property spelled the
// same way over the same signal, executed at the same offset from an edge, and
// the only difference between them is the sampled value they meet.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N). `d` moves by
// nonblocking assignment only, so its sampled value at a tick is also the value
// an active-region print reads at that tick:
//
//   cyc    t    d  z   what the four expect attempts do here
//    -     1    0  0   EXP executes expect1. No attempt yet: 16.17 puts the first
//                      evaluation on the next clocking event.
//    0     5    0  0   expect1's single attempt STARTS. Its property is `##1 d`,
//                      so this tick supplies no value to test.
//    1    15    1  0   d sampled 1 -> expect1 SUCCEEDS. EXP unblocks here, in the
//                      reactive region set of THIS slot.
//    -    16    1  0   EXP executes expect2, again off-edge and again one tick
//                      before the attempt starts.
//    2    25    0  0   expect2's single attempt starts.
//    3    35    0  0   d sampled 0 -> expect2 FAILS. EXP unblocks here AND
//                      executes expect2b here, with no delay in between -- so
//                      expect2b is executed from the Reactive region of a tick
//                      whose Observed region is already behind it.
//    4    45    0  0   expect2b's attempt starts. NOT cyc3: that tick's Observed
//                      region was processed before expect2b existed.
//    5    55    1  0   d sampled 1 -> expect2b SUCCEEDS. EXP unblocks here.
//    -    56    1  0   EXP executes expect3.
//    6    65    1  0   expect3's attempt starts. `##[1:8] z` can be satisfied at
//                      cyc7..cyc14 and refuted only after cyc14.
//    7    75    1  0   z low: no match, attempt still pending.
//    8    85    1  0   z low: still pending. LAST EVALUATED TICK.
//    -    86    -  -   $finish, with expect3 pending and EXP still blocked on it.
//
// `z` is deliberately never raised, and expect3's window is deliberately BOUNDED:
// a pending attempt is wanted, but not at the price of also requiring an
// unbounded property operator, which is a separate measurement (diff_until.sv,
// diff_strongprop.sv). Eight ticks is more than the trace has left, so the
// attempt is neither matched nor refuted when the run ends.
//
// $finish is at t=86 rather than on a tick, so the last tick's regions all run to
// completion and nothing races the end of the run. The sibling fixtures $finish
// ON an unevaluated tick because they only compare report times; a full-output
// fixture cannot afford the unordered pair that makes ($finish in one
// active-region process, the edge print in another, LRM 4.4.2.2: active region
// events "can be processed in any order").
//
// Derived from the LRM, expect2b excepted. 28 lines, in this order:
//
//   [1]  EXP  1 before expect1
//   [5]  ACT edge cyc=0 d=0 z=0
//   [5]  ACT nba  cyc=1
//   [15] ACT edge cyc=1 d=1 z=0
//   [15] ACT nba  cyc=2
//   [15] EXP  2 expect1 PASS action
//   [15] EXP  4 after expect1
//   [16] EXP  5 before expect2
//   [25] ACT edge cyc=2 d=0 z=0
//   [25] ACT nba  cyc=3
//   [35] ACT edge cyc=3 d=0 z=0
//   [35] ACT nba  cyc=4
//   [35] EXP  7 expect2 FAIL action
//   [35] EXP  8 after expect2
//   [35] EXP  9 before expect2b, back to back
//   [45] ACT edge cyc=4 d=0 z=0
//   [45] ACT nba  cyc=5
//   [55] ACT edge cyc=5 d=1 z=0
//   [55] ACT nba  cyc=6
//   [55] EXP 10 expect2b PASS action
//   [55] EXP 12 after expect2b
//   [56] EXP 13 before expect3
//   [65] ACT edge cyc=6 d=1 z=0
//   [65] ACT nba  cyc=7
//   [75] ACT edge cyc=7 d=1 z=0
//   [75] ACT nba  cyc=8
//   [85] ACT edge cyc=8 d=1 z=0
//   [85] ACT nba  cyc=9
//   ($finish banner)                and NOTHING after it
//
// Every part of that carries a measurement:
//
//   THE TWO ACT LINES PER TICK are the region ruler, and their own order is
//   fixed rather than chosen: `ACT edge` is the posedge process, which runs in
//   the Active region and reads `cyc` before its own nonblocking increment
//   commits; `ACT nba` is `always @(cyc)`, which cannot be woken until that
//   increment commits in the NBA region (4.4.2.4) and so runs in a later active
//   region pass. `ACT nba` is therefore the LAST active-region line of the tick,
//   and an expect continuation that is scheduled per 16.17 must sort after it. An
//   implementation that resumes the process through the ordinary active path
//   instead puts `EXP 2` and `EXP 4` before `ACT nba cyc=2`, because a resumption
//   already queued at the top of the slot is processed before the NBA region that
//   creates the `@(cyc)` event -- so the wrong region is a visible transposition
//   of two lines at one time stamp, not a missing line.
//
//   EXP 1 AT t=1 AND EXP 2 AT t=15 are the blocking requirement: a whole clock
//   tick (cyc0) passes between them, printing both of its ACT lines, while EXP
//   has executed nothing. An implementation in which `expect` does not block
//   prints EXP 2 or EXP 4 at t=1.
//
//   EXP 2 vs EXP 3, EXP 7 vs EXP 6 AND EXP 10 vs EXP 11 are the verdicts,
//   measured positively. Both branches of every action block print, so a wrong
//   verdict is a wrong line rather than a missing one: expect1 must print EXP 2
//   and never EXP 3, expect2 must print EXP 7 and never EXP 6, expect2b must
//   print EXP 10 and never EXP 11.
//
//   THE TIMES 15 AND 35 are the OFFSET, and they are the same offset twice: t=1
//   -> t=15 and t=16 -> t=35 are both "the second clocking event after the
//   statement executed". An implementation that starts the attempt at the current
//   or immediately following tick instead resolves expect1 at t=5 and expect2 at
//   t=25; one that starts it a tick late resolves at t=25 and t=45. The LRM
//   sentence that fixes this is 16.17's own worked example: "The sequence will
//   not match if a, b, or c is evaluated to be false at the first, second, or
//   third clocking event occurrence, respectively" -- the first element of the
//   sequence is tested at the FIRST clocking event after the expect, so the `d`
//   behind a leading `##1` is tested at the second.
//
//   THE TIME 55, AND THE FACT THAT IT IS A PASS, are the RE-EXECUTION
//   requirement, and this is the one case in this file whose expectation was
//   written AFTER clotho's lowering landed rather than before it. It is here
//   because it is a corner an implementer demonstrably gets wrong -- it caught
//   the author of the lowering out, in the opposite direction -- and its
//   evidential weight therefore comes from the LRM argument, not from having
//   been blind. The argument, in full: expect2b is executed by the continuation
//   of expect2, which 16.17 schedules "after processing the
//   Observed region in which the property completes its evaluation". So expect2b
//   is executed at t=35 with cyc3's Observed region already behind it, and "the
//   next clocking event" can only be cyc4. Its `##1 d` is therefore tested at
//   cyc5, where d is high, so it PASSES at t=55.
//
//   That gives the error TWO independent witnesses, which is why d is raised
//   again at cyc4 instead of leaving the trace quiet: an implementation that
//   counts cyc3 as still available starts the attempt at cyc3, tests d at cyc4
//   where it is LOW, and prints EXP 11 -- the wrong branch -- at t=45 -- the
//   wrong time. Neither witness can be produced by getting the requirement
//   right, and an implementation cannot land on one of them by accident: EXP 10
//   at t=45 or EXP 11 at t=55 would each be a third, distinct behaviour.
//
//   AND THE BOOKKEEPING ON THAT, because it is easy to double-count. There is a
//   doctest in tests/sva_test.cpp for the same rule in a loop spelling, and it is
//   NOT independent evidence for expect2b: it was written and corrected in the
//   same window and is the same non-blind derivation, one rule in two spellings.
//   The only blind witness for the rule is the iverilog emulation recorded further
//   down, which reaches t=55 without having a reactive region at all, and that is
//   the one worth citing.
//
//   EACH ACTION LINE APPEARS ONCE. That is the SINGLE requirement, and it is the
//   one an implementation that lowers `expect` by reusing its concurrent-assert
//   machinery gets wrong: an attempt per tick would print EXP 2 or EXP 3 at every
//   tick from cyc1 on. 16.17 is explicit that the property "stops being
//   evaluated" once it resolves and that "no property evaluation is started until
//   that expect statement is executed again".
//
//   NOTHING AFTER THE BANNER, and this one is a PREDICTION rather than a reading
//   of the LRM, because the LRM does not settle it. Three dispositions of a
//   pending expect at $finish are defensible and the file distinguishes all
//   three, which is why expect3's action block prints on both branches AND is
//   followed by a line that must never appear:
//
//     (a) nothing, and no EXP 16 -- the run ends, and the pass/fail statement and
//         the statement after the expect are both statements of a process that
//         16.17 can only schedule "after processing the Observed region in which
//         the property completes its evaluation". No such region ever comes, so
//         neither runs. THIS IS THE DERIVATION.
//     (b) EXP 15 then EXP 16 -- the attempt is disposed of as a refutation when
//         the clock stops, and the process is unblocked one last time. This is
//         what strict application of 16.12.2's default strength predicts: "If the
//         strong or weak operator is omitted, then the evaluation of the
//         sequence_expr depends on the assertion statement in which it is used.
//         If the assertion statement is assert property or assume property, then
//         the sequence_expr is evaluated as weak(sequence_expr). Otherwise, the
//         sequence_expr is evaluated as strong(sequence_expr)." An expect is
//         neither of the two named statements, so `##[1:8] z` here is the STRONG
//         sequence property, and a strong property with no match when the trace
//         ends is false.
//     (c) EXP 14 then EXP 16 -- the weak reading, i.e. treating an expect as one
//         of 16.12.2's assert-like statements after all.
//
//   (a) and (b) disagree about whether a verdict with nowhere to run its action
//   still runs it; (b) and (c) disagree only about strength. All three are one
//   line apart in the output. What clotho must NOT do is print EXP 16 without an
//   action line, or print EXP 16 anywhere before the banner: that would mean the
//   process was never really blocked.
//
//   CLOTHO'S (a) IS A CHOICE, NOT AN ACCIDENT: the lowering skips the end-of-trace
//   drain entirely for an expect, so (a) is what it means to do rather than what
//   falls out. One consequence is that the strength question is MOOT for clotho's
//   expect in a way it is not for the LRM: with no end-of-trace disposition
//   consulted at all, whether `##[1:8] z` is weak or strong cannot reach the
//   observable, so settling on (a) is not evidence that 16.12.2's "Otherwise"
//   reading was either adopted or rejected.
//
// DELIBERATELY NOT HERE, each because it would add a second independent
// measurement to a file about blocking and region:
//
//   * An expect in a `program` block, which is 16.17's own example's context.
//     Program support is a separate clotho gap (docs/TODO.md), and the reactive
//     home region of a program `initial` would confound the REGION measurement
//     above: the point here is that a MODULE `initial`, whose home region is
//     Active, still has its continuation scheduled into the reactive region set,
//     and 16.17's "anywhere a wait statement can appear" is what makes that
//     legal.
//   * The automatic-variable case, 16.17's `task automatic wait_for(...)`
//     example. "Because it is a blocking statement, the property can refer to
//     automatic variables" is a capture-per-execution requirement and deserves
//     its own fixture.
//   * Assertion action control (20.12), which 16.17 explicitly extends to an
//     expect's pass and fail statements. Gating the actions of a blocking
//     statement is a third measurement again.
//
// PART OF THIS IS ALREADY CONFIRMED INDEPENDENTLY, and it is worth saying
// which. iverilog -g2012 has no SVA and no reactive region set, but it does have
// the stimulus: run over this module with each expect replaced by the two
// `@(posedge clk)` waits and the `if (d)` that 16.17's offset rule says it is
// equivalent to, it reproduces all 28 lines above -- every ACT line, every value
// in the table, and the times 1, 15, 16, 35, 55, 56 and 86 -- and prints neither
// EXP 14 nor EXP 16. It reproduces expect2b's t=55 PASS too, and for the reason
// the derivation gives: the emulated process is resumed by cyc3's edge, so its
// next two `@(posedge clk)` waits are cyc4 and cyc5 whether or not the tool has
// a reactive region at all. So the waveform, the sampled values, both resolution
// offsets and the blocking are confirmed independently, and what remains
// unconfirmed is exactly the REGION and the disposition of a pending expect at
// $finish.
//
// That run also demonstrates the wrong-region signature is real rather than
// hypothetical: with the continuation resumed by an ordinary event control, EXP 2
// and EXP 4 print between `ACT edge cyc=1` and `ACT nba cyc=2` -- the exact
// transposition described above, produced by iverilog doing exactly what an
// implementation that resumes through the active path would do.
//
// `expect` in a MODULE rather than a program is plainly legal -- 16.17's
// wait-statement sentence, quoted above -- but every example in the clause is in
// a program, and a module `initial` is what makes the REGION measurement above
// possible, so this file deliberately does not wrap itself in one.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          d   = 0;  // expect1 and expect2b meet it high, expect2 meets it low
  bit          z   = 0;  // never raised: expect3's window can never be satisfied
  int unsigned cyc = 0;

  // The active-region ruler, two lines per tick. The first runs in the Active
  // region and so reads `cyc` before its own nonblocking increment commits; the
  // second cannot run until that increment commits in the NBA region, so it is
  // the LAST active-region line of the tick and every reactive-region line of the
  // same tick must sort after it.
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] ACT edge cyc=%0d d=%b z=%b", $time, cyc, d, z);
  end
  always @(cyc) $display("[%0t] ACT nba  cyc=%0d", $time, cyc);

  // The process under test. expect1, expect2 and expect3 execute at an off-edge
  // time, so "the next clocking event" (16.17) is unambiguous; expect2b instead
  // executes at an edge ON PURPOSE, from the continuation of expect2, which is the
  // one place the answer is not obvious. Every action block prints on both
  // branches, so a wrong verdict is a wrong line rather than a missing one.
  initial begin
    #1;
    $display("[%0t] EXP  1 before expect1", $time);
    expect (@(posedge clk) ##1 d) $display("[%0t] EXP  2 expect1 PASS action", $time);
      else                        $display("[%0t] EXP  3 expect1 FAIL action", $time);
    $display("[%0t] EXP  4 after expect1", $time);

    // t=16. Nothing else in the design runs at this instant, so the region this
    // `#1` resumes into is deliberately unobservable HERE -- 16.17 does not say
    // what region a process belongs to once an expect has resumed it, though LRM
    // 24.3.1's inheritance rule answers it for a thread of the reactive region
    // set. expect2b below is where this file does ask, and it asks in the one form
    // whose answer is a time stamp rather than a line order.
    #1;
    $display("[%0t] EXP  5 before expect2", $time);
    expect (@(posedge clk) ##1 d) $display("[%0t] EXP  6 expect2 PASS action", $time);
      else                        $display("[%0t] EXP  7 expect2 FAIL action", $time);
    $display("[%0t] EXP  8 after expect2", $time);

    // NO DELAY HERE, deliberately: expect2b is executed from expect2's own
    // continuation, so it runs at t=35 with cyc3's Observed region already
    // processed. "The next clocking event" is therefore cyc4 and not cyc3, its
    // `##1 d` is tested at cyc5 where d is high, and it passes at t=55. An
    // implementation that counts cyc3 as still available prints the OTHER branch
    // at the OTHER time, t=45.
    $display("[%0t] EXP  9 before expect2b, back to back", $time);
    expect (@(posedge clk) ##1 d) $display("[%0t] EXP 10 expect2b PASS action", $time);
      else                        $display("[%0t] EXP 11 expect2b FAIL action", $time);
    $display("[%0t] EXP 12 after expect2b", $time);

    #1;
    $display("[%0t] EXP 13 before expect3", $time);
    expect (@(posedge clk) ##[1:8] z) $display("[%0t] EXP 14 expect3 PASS action", $time);
      else                            $display("[%0t] EXP 15 expect3 FAIL action", $time);
    // Reached only if a pending expect unblocks its process at the end of the
    // run. It must never appear before the $finish banner: that would mean the
    // expect did not block at all.
    $display("[%0t] EXP 16 after expect3 -- MUST NOT APPEAR", $time);
  end

  initial begin
    @(posedge clk);   // cyc0 t=5
    d <= 1;           // sampled 1 at cyc1, where expect1's `##1 d` is tested
    @(posedge clk);   // cyc1 t=15   expect1 succeeds here
    d <= 0;           // sampled 0 from cyc2 on, so expect2's `##1 d` fails at cyc3
    @(posedge clk);   // cyc2 t=25   expect2's attempt starts here
    @(posedge clk);   // cyc3 t=35   expect2 fails here, and expect2b is executed
    @(posedge clk);   // cyc4 t=45   expect2b's attempt starts here
    d <= 1;           // sampled 1 at cyc5, where expect2b's `##1 d` is tested, and
                      // LOW at cyc4, where a one-tick-early attempt would test it
    @(posedge clk);   // cyc5 t=55   expect2b succeeds here
    @(posedge clk);   // cyc6 t=65   expect3's attempt starts here
    @(posedge clk);   // cyc7 t=75
    @(posedge clk);   // cyc8 t=85   last evaluated tick
    #1;               // off-tick, so nothing races the end of the run
    $finish;          // t=86, with expect3 pending
  end
endmodule
