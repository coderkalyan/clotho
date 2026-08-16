// The abort operators. All four of `accept_on`, `reject_on`, `sync_accept_on`
// and `sync_reject_on` (AbortAssertionExpr, `isSync` distinguishing the pairs)
// lower, and this file pins all nine of their reports.
//
// Every assertion below puts the abort ABOVE an implication,
// `accept_on (abx) (trig |=> e)`, which is the shape essentially every real use
// writes and the one that used to be refused a second time one layer down --
// libsva's `lowerAbort` rejected any operand whose plan carried a trigger. It no
// longer does: an abort's scope now records whether it reaches the antecedent,
// and a match ending in a cycle the condition holds in takes the abort's verdict
// instead of seeding an attempt. See docs/design/sva-property-layer.md, phase 2.
//
// PUSHING THE ABORT DOWN TO `trig |=> accept_on (abx) e` IS NOT AN EQUIVALENT
// REWRITE, and this file is what pins that, so do not weaken it into one.
// Below the implication, the abort only arms once the consequent starts -- one
// cycle after the attempt begins, for `|=>`. Above it, the abort covers the
// attempt's own cycle too. That difference is exactly what C2 below is built on:
// `aby` is first sampled high AT a tick where `trig` is also high, so the
// t=75 reports come from an abort firing in the attempt's own cycle, before its
// obligation has been seeded. The pushed-down form loses both of them, and a
// fixture that passes by testing a different property is worse than one that does
// not run.
//
// Every expectation below was derived from the LRM before these operators
// lowered, and used as the acceptance bar for the implementation, so the
// numbers were never fitted to clotho's behaviour.
//
// ---------------------------------------------------------------------------
// WHAT THE LRM ACTUALLY SAYS, because the scoping note in
// docs/design/sva-property-layer.md §6 states it the wrong way round.
// ---------------------------------------------------------------------------
//
// LRM 16.12.14, on the verdicts:
//
//   "For an evaluation of accept_on (expr) property_expr and of
//    sync_accept_on (expr) property_expr ... If during the evaluation, the
//    abort condition becomes true, then the overall evaluation of the property
//    results in TRUE."
//   "For ... reject_on ... and ... sync_reject_on ... the overall evaluation
//    of the property results in FALSE."
//
// and on the timing, which is the part §6 gets backwards:
//
//   "The operators accept_on and reject_on are evaluated at the granularity
//    of the SIMULATION TIME STEP like disable iff, but their abort condition
//    is evaluated USING SAMPLED VALUE as a regular Boolean expression in
//    assertions."
//   "The operators sync_accept_on and sync_reject_on are evaluated at the
//    simulation time step WHEN THE CLOCKING EVENT HAPPENS, unlike disable iff,
//    accept_on, and reject_on. Their abort condition is evaluated using
//    sampled value AS FOR accept_on and reject_on."
//
// BOTH pairs sample. 16.12.14's own list of how accept_on differs from
// disable iff includes "accept_on uses sampled values" as one of the
// DIFFERENCES -- disable iff is the one that reads live values (16.15), not the
// plain abort forms. The async/sync split is purely about WHEN the sampled
// condition is looked at: at every simulation time step, or only at the ones
// carrying a clocking event. §6's claim that "the sync_ forms sample, the plain
// forms do not" is wrong on the LRM. See the note on under-determined corners
// below.
//
// ---------------------------------------------------------------------------
// TWO CORNERS 16.12.14 UNDER-DETERMINES, DELIBERATELY AVOIDED HERE.
// ---------------------------------------------------------------------------
//
// C1. THE TIMESTAMP OF AN ASYNCHRONOUS ABORT. Drive a condition high strictly
//     between two ticks, at t=18, while an attempt is pending, with the
//     underlying property passing so the only possible report comes from the
//     abort. The verdict and the aborted attempt are not in doubt; the time the
//     report carries is, because "evaluated at the granularity of the
//     simulation time step" and "evaluated using sampled value" pull against
//     each other for a pulse that lives between two ticks. Read literally --
//     the sampled value examined at each time step -- the report lands at t=20,
//     the first time step whose PREPONED value is high, which is neither the
//     step in which the condition changed nor the next clock tick. No reading
//     is picked here; the corner is kept out of the fixture.
//
// C2. AN ABORT LANDING ON AN ATTEMPT THAT RESOLVES VACUOUSLY IN THE SAME TICK.
//     When the abort condition is sampled true at a tick where a fresh attempt
//     starts AND that attempt's antecedent does not match, the property is
//     already true at that same tick, and whether a condition that holds there
//     counts as becoming true "during the evaluation" of an evaluation with no
//     duration is exactly what the clause does not say.
//
//     With `trig` HIGH at that tick, so the fresh attempt is a genuine pending
//     obligation rather than an instantaneous one, the question does not arise.
//     That is the shape this fixture uses.
//
// HOW THE CORNERS ARE AVOIDED WITHOUT LOSING COVERAGE:
//
//   * The async/sync distinction is carried by the ACCEPT pair, on `abx`, a
//     pulse living strictly between two ticks. An accept DISCHARGES, so the
//     asynchronous form produces an ABSENCE of a report -- and an absence has
//     no timestamp to be in doubt about. The synchronous form misses the pulse
//     and the underlying failure comes through at an ordinary tick. Presence
//     versus absence at t=45, with C1 never reached.
//   * The REJECT pair is driven by `aby`, which is only ever first sampled high
//     AT a tick, and only at a tick where nothing is pending, so no live-versus
//     -sampled read can move its report (C1) -- and where `trig` is high, so
//     the attempt it aborts is a real obligation (C2).
//   * Consequently `reject_on` and `sync_reject_on` are NOT distinguished from
//     each other here. That distinction is exactly C1 and cannot be tested
//     without landing on it. Both are still pinned individually: each must turn
//     a passing attempt into a report at t=75.
//
// ---------------------------------------------------------------------------
//
// The property under all four wrappers is the same, `trig |=> e`, one pending
// obligation at a time, so report counts stay attributable.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    trig  e   abx  aby
//    0     5     0    1    0    0
//    1    15     1    1    0    0   seeds an obligation on e at cycle 2
//    2    25     0    0    0    0   <- it fails, with no abort anywhere near
//    3    35     1    1    0    0   seeds again, into the abx pulse
//    4    45     0    0    0    0   <- it would fail; abx pulsed at t=38..42
//    5    55     0    1    0    0
//    6    65     0    1    0    0   nothing pending, so aby may rise at t=72
//    7    75     1    1    0    1   <- fresh REAL obligation meets aby
//    8    85     0    1    0    0   e high, so the cycle-7 attempt would pass
//    9    95     1    1    0    0   seeds an obligation left open at $finish
//   10   105     0    1    0    0   $finish here, so this tick never evaluates
//
//   abx  set t=38, cleared t=42. Ticks 3 (35) and 4 (45) both sample it low.
//   aby  set t=72, cleared t=76. Nothing is scheduled between t=72 and the
//        t=75 tick, so that tick is the first step to sample it high, and no
//        reject-wrapped attempt is pending at t=72 to be aborted early.
//
// Derived from the LRM. The bare `trig |=> e` fails from cycle 1 (e low at
// cycle 2) and from cycle 3 (e low at cycle 4), passes from cycle 7 (e high at
// cycle 8), and is left pending from cycle 9, which holds weakly per 16.12.2.
// Then:
//
//   aa  accept_on(abx)      (trig |=> e)
//       cycle 1: no pulse overlaps ticks 1..2                 -> FAIL 25
//       cycle 3: abx is seen between the ticks, so the property is discharged
//                TRUE and the real failure at cycle 4 never surfaces
//                                                             -> no report
//       cycle 9: pending, weak, no abort                      -> no report
//   sa  sync_accept_on(abx) (trig |=> e)
//       cycle 1: as above                                     -> FAIL 25
//       cycle 3: ticks 3 and 4 both sample abx LOW, so the pulse is invisible
//                and the underlying failure stands            -> FAIL 45
//       cycle 9: pending, weak                                -> no report
//   rr  reject_on(aby)      (trig |=> e)
//       cycle 1: no aby                                       -> FAIL 25
//       cycle 3: no aby, underlying failure stands            -> FAIL 45
//       cycle 7: trig high, so this is a real obligation, and aby is sampled
//                true at its start tick. It would otherwise have PASSED at
//                cycle 8, so this report is created by the abort rather than
//                uncovered by it                              -> FAIL 75
//       cycle 9: pending, weak, no aby                        -> no report
//   sr  sync_reject_on(aby) (trig |=> e)
//       identical to rr on this stimulus, by construction: aby is only ever
//       first sampled high at a tick.        -> FAIL 25, FAIL 45, FAIL 75
//
//   expected: 4 reports at 25, 3 at 45, 2 at 75 -- nine in all, and NOTHING at
//   105.
//
// Three things each report set pins:
//
//   t=25   the control where all four wrappers are transparent and the bare
//          property's failure comes through unchanged, so none of them is
//          silently swallowing everything.
//   t=45   the async/sync split, on the accept side: `aa` is silent and `sa`
//          reports, from the same property and the same pulse. This is the
//          only place the two differ, and it needs no judgement about non-tick
//          report times.
//   t=75   reject turning a PASS into a report. The underlying property holds
//          at cycle 8, so neither accept form says anything and both reject
//          forms do -- which is the only way to show reject_on doing something
//          an ordinary failure could not.
//
// Not vacuous, in both directions: t=25 proves no wrapper swallows everything
// and t=75 proves no wrapper passes everything through. The cycle-9 attempt is
// left pending at $finish with no abort in sight and must produce nothing from
// any of the four, which pins that an abort wrapper does not disturb 16.12.2's
// weak end-of-trace verdict.
//
// The nine reports, per directive:
//
//   FAIL 25   aa, sa, rr, sr -- all started 15
//   FAIL 45   sa, rr, sr     -- all started 35   (aa discharged)
//   FAIL 75   rr, sr         -- both started 75
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          trig = 0, e = 1, abx = 0, aby = 0;
  int unsigned cyc  = 0;

  // Both clock edges AND every change of an abort condition are marked,
  // because an asynchronous abort can be seen at a time step carrying no tick
  // and a report there has to be attributable to it rather than to the last
  // tick.
  always @(clk or abx or aby)
    $display("[%0t] clk=%b cyc=%0d trig=%b e=%b abx=%b aby=%b",
             $time, clk, cyc, trig, e, abx, aby);
  always @(posedge clk) cyc <= cyc + 1;

  aa: assert property (@(posedge clk) accept_on      (abx) (trig |=> e));
  sa: assert property (@(posedge clk) sync_accept_on (abx) (trig |=> e));
  rr: assert property (@(posedge clk) reject_on      (aby) (trig |=> e));
  sr: assert property (@(posedge clk) sync_reject_on (aby) (trig |=> e));

  initial begin
    @(posedge clk);            // cyc0
    trig <= 1;
    @(posedge clk);            // cyc1: trig=1, seeds
    trig <= 0; e <= 0;
    @(posedge clk);            // cyc2: e=0, the control failure
    trig <= 1; e <= 1;
    @(posedge clk);            // cyc3: trig=1, seeds into the abx pulse
    trig <= 0; e <= 0;
    @(posedge clk);            // cyc4: e=0
    e <= 1;
    @(posedge clk);            // cyc5
    @(posedge clk);            // cyc6: nothing pending across t=72
    trig <= 1;
    @(posedge clk);            // cyc7: trig=1 AND aby sampled high here
    trig <= 0;
    @(posedge clk);            // cyc8: e=1, so cycle 7 would have passed
    trig <= 1;
    @(posedge clk);            // cyc9: seeds an obligation nothing can settle
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end

  // Absolute times, not clock-relative: the whole point of abx is that it
  // falls between two ticks, and the whole point of aby is that it does not.
  initial begin
    #38 abx = 1;               // between ticks 3 (35) and 4 (45)
    #4  abx = 0;               // t=42
    #30 aby = 1;               // t=72: nothing is scheduled before the t=75
    #4  aby = 0;               // t=76   tick, so the tick samples it first
  end
endmodule
