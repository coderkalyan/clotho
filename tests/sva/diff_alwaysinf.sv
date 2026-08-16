// The unranged weak `always`, against a bounded strong twin.
//
// Clotho lowers this: the unranged `always` reaches the counted tracking
// discipline of docs/design/sva-property-layer.md, phase 3, and the strong twin
// has lowered since phase 1 (sva_prop_shift). It did NOT lower when this file
// was written -- Sva.cpp still refused it with "the obligation it raises has no
// bounded length" -- and that ordering is the point: every expectation below
// was derived from the LRM before the implementation existed, so it cannot be
// an encoding of what clotho happens to do.
//
// Run against the implementation afterwards, clotho reproduces the derivation
// exactly and with no report the derivation does not have: 3 + 3 at t=25,
// 5 from `wa` and 3 from `sa` at t=75, and 2 after the $finish banner from `sa`
// alone. The 5-versus-3 and the 0-versus-2 below are the two measurements this
// file exists for and both of them landed.
//
// LRM 16.12.11, the unranged weak form and -- as the same clause's list makes
// plain by omission -- the fact that it has no strong twin:
//
//   "A property always property_expr evaluates to true if, and only if,
//    property_expr holds at every current or future clock tick."
//   "A property s_always [constant_range] property_expr evaluates to true if,
//    and only if, all current or future clock ticks specified by constant_range
//    exist and property_expr holds at each of these clock ticks. THE RANGE FOR
//    A STRONG ALWAYS SHALL BE BOUNDED."
//
// with 16.12.15 explaining why the unranged form can only be the weak one:
//
//   "the property always a is a safety property since it is violated only if
//    after finitely many clock ticks there is a clock tick at which a is false,
//    even if there are infinitely many clock ticks in the computation."
//
// THERE IS NO UNBOUNDED STRONG ALWAYS -- the grammar has no unranged s_always
// and the clause forbids the ranged one from being unbounded. So the weak/strong
// pair this fixture needs cannot be same-operator-different-strength; the
// closest legal pairing is the unranged weak form against a BOUNDED strong one,
// and the stimulus below is shaped so that the two differ in exactly two places
// and agree everywhere else:
//
//   REACH        `always a` is answerable to every later tick; `s_always [0:2]
//                a` only to three. A low far enough after an attempt is seen by
//                the first and not the second -- cycle 7, reached from cycle 3.
//   STRENGTH     an attempt that has seen no low when the clock stops holds
//                weakly and fails strongly (16.12.2, 16.12.15). a is high at
//                cycles 8 and 9, so both directives leave the SAME TWO ATTEMPTS
//                open at $finish and dispose of them oppositely.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    a
//    0     5    1
//    1    15    1
//    2    25    0     <- first low. Reached from cycles 0, 1 and 2 -- and by
//                        the [0:2] window from the same three, so both
//                        directives report identically here.
//    3    35    1
//    4    45    1
//    5    55    1     the gap from cycle 2 to cycle 7 is FOUR ticks wide, which
//    6    65    1     is wider than the twin's window on purpose
//    7    75    0     <- second low. `always` reaches it from cycles 3..7 (five
//                        attempts); the [0:2] window only from 5, 6 and 7.
//    8    85    1     a is high for the rest of the trace, so the attempts
//    9    95    1     starting at cycles 8 and 9 are still open at the end
//   10   105    -     $finish executes here, so this tick never evaluates
//
// Derived from the LRM. An attempt starts at every tick (16.12.1), so ten
// attempts per directive, and a verdict belongs to an attempt: repeated times
// below are multiplicity, not duplication.
//
//   wa   always a         -- an attempt fails at the FIRST low at or after its
//        own start tick, and holds at every tick until then. Attempts 0,1,2 all
//        find cycle 2 (attempt 2 at its own start tick, which is what "every
//        current or future clock tick" includes). Attempts 3,4,5,6,7 all find
//        cycle 7. Attempts 8 and 9 find no low at all and are still open when
//        the clock stops; weak, so they hold and report NOTHING.
//                                                  -> FAIL 25 x3, FAIL 75 x5
//   sa   s_always [0:2] a -- an attempt is answerable for ticks N, N+1, N+2 and
//        all three must exist. Attempts 0,1,2 find cycle 2 in window. Attempts
//        3 and 4 see only high ticks and pass at cycles 5 and 6. Attempts 5,6,7
//        find cycle 7. Attempts 8 and 9 want ticks 10, 11 and 12, which never
//        evaluate.                     -> FAIL 25 x3, FAIL 75 x3, FAILEND 105 x2
//
//   expected: 6 reports at 25, 8 at 75, and 2 at the end of trace -- 16 in all.
//
// The two counts that carry the file are 5-versus-3 at cycle 75 and 0-versus-2
// at the end of trace, and neither moves when the other is wrong. A finite
// trace cannot prove a reach is INFINITE -- nothing can -- but it can prove the
// reach is not the twin's: an `always a` clamped to [0:2] scores 3 at cycle 75
// instead of 5, and one clamped to [0:3] scores 4, so the multiplicity there
// measures how far back the obligation is still alive. And an implementation
// that drains pending attempts at $finish regardless of strength turns the 2
// into 4, while one that drops them turns it into 0.
//
// Not vacuous. `wa` fails from eight of its ten attempts and holds two; `sa`
// fails eight and passes two; a is high at eight of the ten evaluated ticks, so
// both directives are silent far more often than they report. A monitor that
// never fires loses all 16 reports. One that fires once per attempt per tick
// produces 20 reports spread over all ten tick times rather than 16 over three,
// so it is distinguishable from correct behaviour at the eight tick times where
// this derivation is silent (5, 15, 35, 45, 55, 65, 85, 95).
//
// NO COVER IN THIS FILE, deliberately. A `cover property (always a)` has no
// finite success point: the property is only decided in the affirmative when
// the clock stops, so its hit count would measure an end-of-simulation
// convention rather than the operator. Cover multiplicity is exercised in
// diff_until.sv and diff_seventually.sv, where a STRONG operator gives every
// hit a definite tick.
//
// The alternative twins were rejected for isolating nothing: `strong(a[*3])` is
// the same obligation in another spelling, and a twin from another operator
// family, such as a ranged `s_eventually`, differs from `always a` in polarity
// and quantifier as well as strength.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b", $time, cyc, a);
  end

  wa: assert property (@(posedge clk) always         a);
  sa: assert property (@(posedge clk) s_always [0:2] a);

  initial begin
    @(posedge clk);            // cyc0: a=1
    @(posedge clk);            // cyc1: a=1
    a <= 0;
    @(posedge clk);            // cyc2: a=0
    a <= 1;
    @(posedge clk);            // cyc3: a=1  the four-tick gap to the next low
    @(posedge clk);            // cyc4  is what separates an unbounded reach
    @(posedge clk);            // cyc5  from the twin's [0:2] window
    @(posedge clk);            // cyc6: a=1
    a <= 0;
    @(posedge clk);            // cyc7: a=0
    a <= 1;
    @(posedge clk);            // cyc8: a=1  a stays high, so these two attempts
    @(posedge clk);            // cyc9: a=1  are open when the clock stops
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
