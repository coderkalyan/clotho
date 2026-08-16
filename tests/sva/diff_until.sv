// The four until forms: `until`, `s_until`, `until_with` and `s_until_with`.
//
// Clotho lowers this: all four operators reach the counted tracking discipline
// of docs/design/sva-property-layer.md, phase 3. It did NOT when this file was
// written -- Sva.cpp still refused them with "the obligation they raise is
// unbounded" -- and that ordering is the point: every expectation below was
// derived from the LRM before the implementation existed, so it cannot be an
// encoding of what clotho happens to do.
//
// Run against the implementation afterwards, clotho reproduces the derivation
// exactly and emits nothing it does not have: 12 reports at t=25, 4 at t=55,
// 8 after the $finish banner, and COVER cs 3.
//
// Getting there took one fix, and the cover is what found it. On its first run
// this file also produced four end-of-trace FAILURES from `cs`: the drain that
// disposes of attempts still pending when the clock stops was gated on the
// directive not being a `restrict`, which let a cover through, and a cover has
// no fail statement to run (LRM 16.14.1) so it fell back to the default report.
// The gate is now "strong AND this directive can report a failure", which
// exempts cover and restrict on both the ring and the counted path. The bug was
// latent in the ring path and only became reachable when the unbounded
// operators landed, because those are the ones whose natural spelling is
// strong -- which is the argument for putting a cover in a fixture at all.
//
// LRM 16.12.12, in full, because all four forms are asserted here and the
// clause is the only place the LRM distinguishes them:
//
//   "An until property of the non-overlapping form evaluates to true if
//    property_expr1 evaluates to true at every clock tick beginning with the
//    starting clock tick of the evaluation attempt and continuing until at
//    least one tick before a clock tick where property_expr2 evaluates to true.
//    An until property of one of the overlapping forms evaluates to true if
//    property_expr1 evaluates to true at every clock tick beginning with the
//    starting clock tick of the evaluation attempt and continuing until and
//    including a clock tick at which property_expr2 evaluates to true. An until
//    property of one of the strong forms requires a current or future clock
//    tick exist at which property_expr2 evaluates to true, while an until
//    property of one of the weak forms does not make this requirement. An until
//    property of one of the weak forms evaluates to true if property_expr1
//    evaluates to true at each clock tick, even if property_expr2 never holds."
//
// and, from the worked examples in the same clause, the corner this stimulus is
// built around:
//
//   "If b is true at the starting clock tick of the evaluation attempt, then a
//    need not be true at that clock tick."          (said of p1, `a until b`)
//
// Two axes, and each is visible at exactly one place in a trace:
//
//   OVERLAP    whether the releasing tick must itself satisfy the left operand.
//              Invisible unless some tick has b high and a low -- everywhere
//              else the two forms make the same demands of the same ticks. That
//              tick is cycle 5.
//   STRENGTH   whether b must ever arrive at all. Invisible unless an attempt is
//              still waiting when the clock stops, which is what LRM 16.12.15
//              ("they require ... that the property clock ticks enough time")
//              and 16.12.2 make the whole weak/strong difference. b never rises
//              again after cycle 5, so cycles 6..9 leave four attempts waiting.
//
// The scan rule each form reduces to, per attempt starting at cycle N, is worth
// writing down because the ORDER of the two tests is the overlap difference:
//
//   non-overlapping   at tick i: b high -> hold (a[i] is not looked at);
//                                else a low -> fail at i; else advance.
//   overlapping       at tick i: a low  -> fail at i (b[i] does not rescue it);
//                                else b high -> hold; else advance.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    a  b
//    0     5    1  0
//    1    15    1  0
//    2    25    0  0     <- a low with b low: every waiting attempt fails here
//    3    35    1  1     <- b releases; a is high too, so all four forms hold
//    4    45    1  0
//    5    55    0  1     <- b high WITH a low: the overlap corner. `until`
//                           holds ("a need not be true at that clock tick"),
//                           `until_with` fails.
//    6    65    1  0     the tail: a stays high and b never rises again, so
//    7    75    1  0     every attempt from cycle 6 on is still waiting when
//    8    85    1  0     the clock stops -- weakly they hold, strongly they
//    9    95    1  0     fail. This is the only place strength is observable.
//   10   105    -  -     $finish executes here, so this tick never evaluates
//
// Derived from the LRM. An attempt starts at every tick (16.12.1), so ten
// attempts per directive, and a verdict belongs to an attempt rather than to a
// cycle -- the repeated times below are multiplicity, not duplication:
//
//   u    a until b        -- attempts 0,1,2 are waiting at cycle 2 with b still
//        low, and a is low there.                          -> FAIL 25 x3
//        Attempt 3 is released at its own start tick (b high). Attempts 4 and 5
//        are released at cycle 5: b is high, so a's low there is not looked at.
//        Attempts 6..9 wait forever and hold, weakly.       -> nothing at eot
//   su   a s_until b      -- identical to u everywhere a verdict is reached,
//        because strength changes nothing about a failure that happens. The
//        four attempts u forgives are owed a b that never comes.
//                                              -> FAIL 25 x3, FAILEND 105 x4
//   uw   a until_with b   -- attempts 0,1,2 fail at cycle 2 exactly as u does.
//        Attempt 3 holds at cycle 3 (a and b both high). Attempts 4 and 5 reach
//        cycle 5 and FAIL there: the releasing tick must satisfy a too.
//                                                 -> FAIL 25 x3, FAIL 55 x2
//   suw  a s_until_with b -- uw's failures, plus the same four unreleased
//        attempts.                    -> FAIL 25 x3, FAIL 55 x2, FAILEND 105 x4
//
//   expected: 12 reports at 25, 4 at 55, and 8 at the end of trace -- 24 in all.
//
// The three counts measure three different things and no two of them move
// together. 12 at cycle 2 is four directives times the three attempts alive
// there, so it pins attempt multiplicity: a monitor that merges concurrent
// attempts into one state reports 4. 4 at cycle 5 is the overlapping pair only,
// so it pins the overlap rule: a monitor that reads `until` as `until_with`
// reports 8 there, and one that reads `until_with` as `until` reports 0. 8 at
// the end of trace is the strong pair only, so it pins strength: 0 if pending
// attempts are dropped at $finish, 16 if weak attempts are drained there too.
//
// Not vacuous. Every one of the four directives both succeeds and reports:
// attempts 3, 4 and 5 succeed under `u` and `su` while 0, 1 and 2 fail; attempt
// 3 succeeds under `uw` and `suw` while five fail; and the two weak directives
// additionally hold the four attempts left open at the end of the trace. A
// monitor that never fires loses all 24 reports. One that fires once per
// attempt per tick reports 40 times, at seven tick times where this derivation
// is silent (5, 15, 35, 45, 65, 85, 95) as well as at the three where it is
// not. Neither reads as agreement with anything below.
//
// THE COVER, and why it is on the strong form. `cs` counts how many attempts of
// `a s_until b` SUCCEED, which is a multiplicity rather than a cycle set -- one
// marker line per hit. It is deliberately not on a weak form:
// a weak until attempt that is still waiting when the clock stops evaluates to
// true (16.12.12's last sentence), so a cover on `a until b` would additionally
// be asking when, and whether, an end-of-trace success is booked as a coverage
// hit. That is a question about an end-of-simulation convention, not about the
// operator. A strong until has no such hit: it either succeeds at a definite
// tick or fails, so the count is unambiguous.
//
//   cs   attempts 3, 4 and 5 succeed (at cycles 3, 5 and 5); attempts 0..2 fail
//        at cycle 2 and 6..9 fail at the end of trace.       -> COVER cs 3
//
// Two of those three hits land on the same tick, and the count is what tells
// the two readings of that apart. LRM 16.14.1 settles which is right:
//
//   "The pass statement specified in statement_or_null shall be executed ONCE
//    FOR EACH SUCCESSFUL EVALUATION ATTEMPT of the underlying property_spec."
//
// so 3, not 2 -- per attempt, matching the per-attempt reporting rule clotho
// adopted in "sva: one verdict per attempt" for assertions. An implementation that books one hit per
// hit CYCLE scores 2.
//
// slang warns -Wvacuous-cover here, and the warning is syntactic: it fires for
// every property operator that CAN succeed vacuously, without looking at the
// operands. These cannot. LRM 16.14.8(w) makes an `s_until` attempt nonvacuous
// when there is a clock event at which either operand's own evaluation is
// nonvacuous with the right operand not yet holding, and 16.14.8(a) makes a
// boolean operand's evaluation nonvacuous always -- so every attempt here is
// nonvacuous at its own start tick and no hit is a vacuous one.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1, b = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b", $time, cyc, a, b);
  end

  u:   assert property (@(posedge clk) a until        b);
  su:  assert property (@(posedge clk) a s_until      b);
  uw:  assert property (@(posedge clk) a until_with   b);
  suw: assert property (@(posedge clk) a s_until_with b);

  cs:  cover  property (@(posedge clk) a s_until      b)
         $display("[%0t] COVER cs", $time);

  initial begin
    @(posedge clk);            // cyc0: a=1 b=0
    @(posedge clk);            // cyc1: a=1 b=0
    a <= 0;
    @(posedge clk);            // cyc2: a=0 b=0
    a <= 1; b <= 1;
    @(posedge clk);            // cyc3: a=1 b=1
    b <= 0;
    @(posedge clk);            // cyc4: a=1 b=0
    a <= 0; b <= 1;
    @(posedge clk);            // cyc5: a=0 b=1  the overlap corner
    a <= 1; b <= 0;
    @(posedge clk);            // cyc6  b never rises again, so the four
    @(posedge clk);            // cyc7  attempts starting here are still
    @(posedge clk);            // cyc8  waiting when the clock stops: weakly
    @(posedge clk);            // cyc9  they hold, strongly they fail
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
