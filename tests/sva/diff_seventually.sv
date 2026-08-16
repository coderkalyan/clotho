// The unranged `s_eventually`, against a ranged weak twin.
//
// Clotho lowers this: the unranged `s_eventually` reaches the counted tracking
// discipline of docs/design/sva-property-layer.md, phase 3, and the weak twin
// has lowered since phase 1 (sva_prop_shift as SVA_SHIFT_SOME, scored by
// diff_eventuallyrange.sv). It did NOT lower when this file was written --
// Sva.cpp still refused it with "the obligation it raises has no bounded
// length" -- and that ordering is the point: every expectation below was
// derived from the LRM before the implementation existed, so it cannot be an
// encoding of what clotho happens to do.
//
// Run against the implementation afterwards, clotho reproduces the derivation
// exactly and emits nothing it does not have: `we` at t=25, 75, 85 and 95, `se`
// silent for the whole trace and four reports after the $finish banner, and
// COVER ce 6 -- four hits at t=35 and two at t=55, the multiplicity this file
// was built to measure.
//
// `ce` also found a bug on its first run, shared with diff_until.sv's cover and
// written up there: the end-of-trace drain was exempting `restrict` rather than
// every directive that cannot report a failure, so a strong cover left pending
// at $finish emitted four default failure reports. Fixed at the shared gate.
//
// LRM 16.12.13. The unranged form is the STRONG one -- the weak eventually has
// no unranged spelling at all, which is the mirror image of `always`, where the
// unranged form is the weak one and no strong unranged form exists:
//
//   "A property s_eventually property_expr evaluates to true if, and only if,
//    there exists a current or future clock tick at which property_expr
//    evaluates to true."
//   "A property eventually [constant_range] property_expr evaluates to true if,
//    and only if, either there exists a current or future clock tick within the
//    range specified by constant_range at which property_expr evaluates to true
//    or not all the current or future clock ticks within the range specified by
//    constant_range exist. THE RANGE FOR A WEAK EVENTUALLY SHALL BE BOUNDED."
//
// and 16.12.15, which is doing more work here than in any other fixture in this
// directory, because it says outright that this operator has no other failure:
//
//   "a failure of the property s_eventually a on a computation with infinitely
//    many clock ticks cannot be identified at a finite time; if it is violated,
//    the value of a must be false at each of the infinitely many clock ticks."
//
// On a FINITE trace that becomes a sharp claim: an unranged `s_eventually p`
// can fail at exactly one place, the end of the trace, and nowhere else. It has
// no mid-trace verdict to get wrong -- an attempt either finds a high tick and
// succeeds there, or it is still looking when the clock stops and fails. So
// this fixture turns on two things a cycle-set comparison alone would miss:
// WHICH SIDE of the $finish banner a report lands on, and HOW MANY attempts
// each success covers.
//
// The weak twin is `eventually [1:2] p` on the same signal. It cannot be an
// unranged weak eventually because the LRM does not have one, so the pair
// differs in range as well as in strength; what makes it a controlled contrast
// anyway is that the two report in DISJOINT places. Every mid-trace report in
// this file belongs to the weak directive and every end-of-trace report belongs
// to the strong one, and each of those two claims fails loudly if strength is
// implemented as a drain-everything-at-$finish pass.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    a
//    0     5    0
//    1    15    0
//    2    25    0     <- the weak window [1:2] from cycle 0 is ticks 1 and 2,
//                        both low: its only failure that is not near the tail
//    3    35    1     <- first high. Four attempts of `s_eventually` are
//                        waiting on it (from cycles 0, 1, 2 and 3).
//    4    45    0
//    5    55    1     <- second high, ending two more attempts (cycles 4, 5)
//    6    65    0     a is low for the whole tail, so the four attempts from
//    7    75    0     cycles 6..9 never find their high tick and are still
//    8    85    0     looking when the clock stops. Strong, so they fail; and
//    9    95    0     the weak twin's last two attempts hold over the same
//   10   105    -     ticks. $finish executes here; this tick never evaluates.
//
// Derived from the LRM. An attempt starts at every tick (16.12.1), so ten
// attempts per directive, and a verdict belongs to an attempt: repeated times
// below are multiplicity, not duplication.
//
//   se   s_eventually a  -- attempts 0,1,2,3 succeed at cycle 3; attempts 4 and
//        5 succeed at cycle 5; attempts 6,7,8,9 find no high tick in the ticks
//        that exist and are owed one that never comes.  -> FAILEND 105 x4
//        AND NOTHING ELSE. A mid-trace report from `se` is wrong by 16.12.15
//        whatever cycle it lands on.
//   we   eventually [1:2] a -- attempt N needs a high at tick N+1 or N+2 and is
//        decided at the LATER of the two, since until then the operand may
//        still come high. Attempt 0 sees ticks 1 and 2, both low. Attempts 1..4
//        all catch cycle 3 or cycle 5. Attempts 5, 6 and 7 see (6,7), (7,8) and
//        (8,9), every one of them low. Attempt 8's window is ticks 9 and 10:
//        tick 9 exists and is low, tick 10 never evaluates, so "not all the
//        current or future clock ticks within the range exist" and it HOLDS --
//        the low at cycle 9 is exactly what makes that forgiveness visible.
//        Attempt 9 wants ticks 10 and 11, neither of which exists, and holds
//        for the same reason.
//                                     -> FAIL 25, FAIL 75, FAIL 85, FAIL 95
//
//   expected: one report each at 25, 75, 85 and 95, and four at the end of
//   trace -- eight in all, four mid-trace and four at $finish, with no time
//   carrying a report from both directives.
//
// Six attempts are open when the clock stops: `se`'s four and `we`'s two. Four
// of the six report. That 4-of-6 split, over the same tail ticks and the same
// operand, is the weak/strong difference with nothing else varying: an
// implementation that drains every pending attempt at $finish scores 6 there
// and one that drops them all scores 0.
//
// THE COVER, and the reason it is the most interesting line in the file. `ce`
// counts how many attempts of `s_eventually a` SUCCEED. Six do, but they land
// on only two ticks: four at cycle 3 and two at cycle 5.
//
//   ce   -> COVER ce 6
//
// That is exactly the measurement docs/design/sva-property-layer.md section 3
// says a merged state vector gets wrong. A monitor that keeps one bit of "still
// looking" for the whole directive rather than one obligation per attempt
// reaches cycle 3 with a single pending state, books ONE hit there and one at
// cycle 5, and scores COVER ce 2 -- with the right hit CYCLES and the wrong
// multiplicity, which is precisely the failure a cycle-set comparison reads as
// agreement. 6 versus 2 is that experiment.
//
// The count is per ATTEMPT and that is not a convention this directory made up.
// LRM 16.14.1:
//
//   "The pass statement specified in statement_or_null shall be executed ONCE
//    FOR EACH SUCCESSFUL EVALUATION ATTEMPT of the underlying property_spec."
//
// which is the same rule "sva: one verdict per attempt" settled for assertion reports, stated for
// covers by the standard itself. An implementation that books one hit per hit
// CYCLE scores 2, which is the failure this number is here to catch.
//
// slang warns -Wvacuous-cover on this line, and the warning is syntactic: it
// fires for every property operator that CAN succeed vacuously, without looking
// at the operands. This one cannot. LRM 16.14.8(s) makes an `s_eventually`
// attempt nonvacuous when there is a clock event at which the operand's own
// evaluation is nonvacuous and the operand has not held earlier, and 16.14.8(a)
// makes a boolean operand's evaluation nonvacuous always -- so all six hits are
// nonvacuous and none of them lands in the standard's separate "succeeded
// because of vacuity" counter.
//
// Not vacuous, and in the one way that matters for a strong operator: `se` both
// succeeds (six attempts) and fails (four), so it is neither a monitor that
// never fires nor one that always does, and its failures are attributable to
// s_eventually alone -- no other directive in the file reports at the end of
// trace. `we` likewise passes four attempts and fails four. A monitor that never
// fires loses all eight reports and all six cover hits; one that fires once per
// attempt per tick produces 20 reports across all ten tick times instead of
// eight across four, so it disagrees at six times where this derivation is
// silent (5, 15, 35, 45, 55, 65).
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b", $time, cyc, a);
  end

  se: assert property (@(posedge clk) s_eventually       a);
  we: assert property (@(posedge clk) eventually   [1:2] a);

  ce: cover  property (@(posedge clk) s_eventually       a)
        $display("[%0t] COVER ce", $time);

  initial begin
    @(posedge clk);            // cyc0: a=0
    @(posedge clk);            // cyc1: a=0
    @(posedge clk);            // cyc2: a=0
    a <= 1;
    @(posedge clk);            // cyc3: a=1  four attempts of `se` end here
    a <= 0;
    @(posedge clk);            // cyc4: a=0
    a <= 1;
    @(posedge clk);            // cyc5: a=1  two more end here
    a <= 0;
    @(posedge clk);            // cyc6: a=0  a stays low for the rest of the
    @(posedge clk);            // cyc7: a=0  trace, so every attempt from here
    @(posedge clk);            // cyc8: a=0  on is still looking when the clock
    @(posedge clk);            // cyc9: a=0  stops
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
