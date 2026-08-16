// The `implies` and `iff` property operators.
//
// Clotho does NOT lower `implies` or `iff` yet. Both are phase 4 of
// docs/design/sva-property-layer.md, and both reduce to `not` plus `or`
// (16.12.8 against 16.12.3 and 16.12.4), which is why they land with the
// complement or not at all; at the time this file was written Sva.cpp refused
// them. That ordering is the whole point: every expectation below was derived
// from the LRM before the implementation existed, so it cannot be an encoding
// of what clotho happens to do. Until they land, `./build/clotho run
// tests/sva/diff_implies.sv` must reach a loud refusal naming the operator the
// source wrote -- `implies` or `iff`, not `or`, even if the lowering is a
// desugaring into `not` and `or`.
//
// CLAUSE NUMBERING, verified against the IEEE Std 1800-2017 subclause list for
// 16.12 rather than taken on trust: implies and iff are 16.12.8. They are NOT
// 16.12.7, which is Implication -- the `|->` / `|=>` operators, a different
// construct that this file scores as the control. 16.12.3 is Negation, 16.12.4
// Disjunction, 16.12.5 Conjunction, 16.12.6 If-else. The weak/strong
// disposition of a pending attempt is 16.12.2 for sequence properties and
// 16.12.15 for the strength-bearing operators.
//
// LRM 16.12.8 IN FULL. The clause is four sentences and it is entirely
// propositional -- there is no timing in it at all, which is the source of
// every subtlety below:
//
//   "A property is an implies if it has the following form:
//        property_expr1 implies property_expr2
//    A property of this form evaluates to true if, and only if, either
//    property_expr1 evaluates to false or property_expr2 evaluates to true.
//    A property is an iff if it has the following form:
//        property_expr1 iff property_expr2
//    A property of this form evaluates to true if, and only if, either both
//    property_expr1 evaluates to false and property_expr2 evaluates to false or
//    both property_expr1 evaluates to true and property_expr2 evaluate to
//    true."
//
// IMPLIES IS NOT `|->`, AND THAT IS THE FIRST THING THIS FIXTURE MEASURES.
// 16.12.8 takes a PROPERTY on the left and combines the two operands' verdicts
// over ONE evaluation attempt, so both operands start at the same tick: the
// consequent is sampled at the ATTEMPT'S START TICK. 16.12.7 takes a SEQUENCE
// on the left and starts an evaluation of the consequent at each match point
// of the antecedent:
//
//   "The implication construct specifies that the checking of a property is
//    performed conditionally on the match of a sequential antecedent"
//
// so its consequent is sampled at the antecedent's END POINT. `ic` below is
// `implies` rewritten with `|->` over the same antecedent and the same
// consequent, and the two disagree on two of the ten attempts in OPPOSITE
// directions -- see the derivation. A monitor that lowers `implies` the way it
// lowers `|->` produces ic's stream for im, which loses a report at cycle 1,
// gains one at cycle 4, and loses both end-of-trace reports.
//
// THE STRENGTH AXIS, and where the pending attempts come from. The antecedent
// is a bare sequence, so 16.12.2 makes it weak in an assert:
//
//   "If the strong or weak operator is omitted, then the evaluation of the
//    sequence_expr depends on the assertion statement in which it is used. If
//    the assertion statement is assert property or assume property, then the
//    sequence_expr is evaluated as weak(sequence_expr). Otherwise, the
//    sequence_expr is evaluated as strong(sequence_expr)."
//
//   "strong(sequence_expr) evaluates to true if, and only if, there is a
//    nonempty match of the sequence_expr. weak(sequence_expr) evaluates to true
//    if, and only if, there is no finite prefix that witnesses inability to
//    match the sequence_expr."
//
// An antecedent still running when the clock stops therefore holds weakly and
// fails strongly, and 16.12.8 propagates that opposite way through each
// operator. `implies` is monotone in its consequent and ANTIMONOTONE in its
// antecedent -- it is `(not p1) or p2` (16.12.3, 16.12.4) -- so the strong
// spelling is the FORGIVING one there. `iff` is monotone in neither: for a
// pending antecedent the weak and the strong spellings demand OPPOSITE values
// of the right operand, so the two fail on DISJOINT sets of attempts. That is
// the sharpest thing in this file and it is why both spellings of both
// operators are asserted here rather than one of each.
//
// The scan rule the antecedent reduces to, per attempt starting at cycle N,
// writing A for its verdict:
//
//   a ##[1:3] b    a[N] low -> A FALSE at N (that prefix witnesses inability to
//                  match); else A TRUE at the first tick in N+1..N+3 where b is
//                  high; else, if all three of those ticks exist and b is low at
//                  each, A FALSE at N+3; else A is still PENDING when the clock
//                  stops -- weakly TRUE, strongly FALSE.
//
// and then, per 16.12.8 with the consequent sampled at tick N:
//
//   A implies c    c[N] high -> TRUE at N, whatever A does. Otherwise the
//                  verdict is the complement of A, decided when A is.
//   A iff c        always decided when A is: FAIL if A differs from c[N].
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N). a and b
// carry diff_not.sv's waveform, so the antecedent's decision ticks can be read
// against that file's table; c is this file's own:
//
//   cyc    t    a  b  c
//    0     5    1  0  0   <- antecedent matches at cycle 1, and c is LOW here
//                            and HIGH there: the implies/|-> split
//    1    15    1  1  1   <- antecedent matches at cycle 4 from here, and c is
//                            HIGH here and LOW there: the split, other way round
//    2    25    0  0  1   <- a low: antecedent FALSE at its own start tick, with
//                            c high, so `iff` fails and `implies` holds
//    3    35    1  0  0
//    4    45    1  1  0   <- b high: the antecedent's second and last match tick
//    5    55    1  0  1   b never rises again from here on
//    6    65    0  0  0
//    7    75    1  0  0   <- a high with b low for the rest of the trace: the
//    8    85    1  0  0      windows from cycles 7, 8 and 9 all run off the end,
//    9    95    1  0  1      so three attempts are PENDING at $finish, with c
//                            low, low and high respectively
//   10   105    -  -  -   $finish executes here, so this tick never evaluates
//
// Derived from the LRM. An attempt starts at every tick, so ten attempts per
// directive, and a verdict belongs to an attempt rather than to a cycle --
// repeated times below are multiplicity, not duplication. The per-attempt rule is 16.12.7's worked example, which
// enumerates one evaluation per clock tick and a separate verdict for each
// ("the property fails at clock tick 1. Similarly, there is a failure at clock
// ticks 2, 3, and 4"), together with 16.14.1, which runs an action "once for
// each ... evaluation attempt"; the 16.12.1 the sibling fixtures cite for this
// is Property instantiation in IEEE Std 1800-2017. A is TRUE for attempts 0 (at
// cycle 1), 1 and 3 (both at cycle 4); FALSE for 2 (at its own cycle), 6 (cycle
// 6), 4 (cycle 7) and 5 (cycle 8); and PENDING for 7, 8 and 9.
//
//   im   (a ##[1:3] b) implies c        -- weak antecedent. Attempts 1, 2, 5 and
//        9 have c high at their own start tick and succeed THERE, before the
//        antecedent is decided and without needing any further tick to exist --
//        a shape `|->` cannot produce. Attempts 0 and 3 have c low and a TRUE
//        antecedent, so they fail, at cycles 1 and 4. Attempts 4 and 6 have c low
//        and a FALSE antecedent, so they hold. Attempts 7 and 8 have c low and a
//        pending antecedent, which holds weakly, so they fail at the end of the
//        trace.                            -> FAIL 15, FAIL 45, FAILEND 105 x2
//   ims  strong(a ##[1:3] b) implies c   -- identical to im everywhere a verdict
//        is reached mid-trace, because strength changes nothing about a match
//        that happens. Attempts 7 and 8 demand a nonempty match the trace has no
//        ticks left to supply, so their antecedents are FALSE and the implication
//        HOLDS: the strong spelling is the forgiving one.
//                                          -> FAIL 15, FAIL 45, and nothing at
//                                             the end of trace
//   ii   (a ##[1:3] b) iff c             -- weak antecedent. Fails wherever A
//        differs from c: attempt 0 (A true, c low) at cycle 1; attempt 2 (A false
//        at its own tick, c high) at cycle 2; attempt 3 (A true, c low) at cycle
//        4; attempt 5 (A false at cycle 8, c high) at cycle 8. Attempts 1, 4 and
//        6 agree and pass. Attempts 7 and 8 have weakly TRUE antecedents against
//        c low, so they fail at the end of the trace; attempt 9 has c high and
//        agrees.        -> FAIL 15, FAIL 25, FAIL 45, FAIL 85, FAILEND 105 x2
//   iis  strong(a ##[1:3] b) iff c       -- the same four mid-trace failures, and
//        THE OTHER END-OF-TRACE ATTEMPT. Attempts 7 and 8 now have FALSE
//        antecedents against c low and agree; attempt 9 has a FALSE antecedent
//        against c HIGH and fails.
//                       -> FAIL 15, FAIL 25, FAIL 45, FAIL 85, FAILEND 105 x1
//   ic   (a ##[1:3] b) |-> c             -- THE CONTROL: 16.12.7 instead of
//        16.12.8, same antecedent, same consequent. It samples c at the
//        antecedent's end point, so attempt 0 checks c at cycle 1 (high, passes)
//        where im checked cycle 0 and failed, and attempt 1 checks c at cycle 4
//        (low, fails) where im had already succeeded at cycle 1. Attempt 3 checks
//        cycle 4 and fails, agreeing with im by coincidence of c. Attempts 2, 4,
//        5 and 6 have no match point and hold vacuously; attempts 7, 8 and 9 have
//        no match point in the ticks that exist, so no obligation was ever
//        raised and the implication holds with NO end-of-trace report at all.
//                                                          -> FAIL 45 x2
//
//   expected: 4 reports at 15, 2 at 25, 6 at 45, 2 at 85, and 5 after the
//   $finish banner -- 19 in all.
//
// b is high at cycles 1 and 4, three ticks apart, and no width-3 window holds
// two ticks that are three apart, so every attempt of this antecedent has AT
// MOST ONE match point. The multiple-match axis is therefore deliberately
// absent from this file -- it is what diff_multimatch.sv scores -- and `ic` is
// a clean control for where the consequent is sampled and nothing else.
//
// The five counts measure five different things. The 4 at cycle 1 is every
// 16.12.8 directive at once, where a TRUE antecedent meets a low consequent,
// and pins the propositional table's false row. The 2 at cycle 2 is the `iff`
// pair alone: it is the only place a FALSE antecedent reports, which is
// exactly what separates `iff` from `implies` -- an implementation that lowers
// `iff` as `implies` reports 0 there. The 6 at cycle 4 is 1 + 1 + 1 + 1 + 2,
// and ic's share of 2 is two attempts reaching the same end point from
// different start ticks, so it pins attempt multiplicity. The 5 at the end of
// trace is the strength swap, split 2 + 0 + 2 + 1 + 0 across the five
// directives, and the ii versus iis split is DISJOINT IN ATTEMPTS: {7, 8}
// weakly, {9} strongly, over the same three pending antecedents. An
// implementation that drops pending attempts at $finish scores 0 there; one
// that fails every attempt left unresolved there scores 13 -- 2 + 2 + 3 + 3
// for the four 16.12.8 directives, plus 3 for ic, whose antecedent is still
// running from cycles 7, 8 and 9 with no match point; and one that resolves a
// pending antecedent without inverting it -- the omitted strength swap --
// produces the RIGHT TOTAL of 5 with every report on the wrong directive: im
// 0, ims 2, ii 1 (attempt 9), iis 2 (attempts 7 and 8). That last one is why
// this derivation lists reports per directive and not just per time.
//
// A PRE-EXISTING CLOTHO DISAGREEMENT, FOUND BY WRITING THIS FIXTURE, and it is
// in `ic` -- the one directive here that already lowers. ic's 2 at cycle 4 is
// two DISTINCT attempts (1 and 3) whose antecedents match at the same end
// point. Clotho today reports ONE. Reduced:
//
//   q: assert property (@(posedge clk) (a ##[1:3] b) |-> c);
//   // a high at cycles 0, 1, 2; b high at cycle 3; c low there
//   // three attempts, three failing obligations, all at cycle 3 -> 1 report
//
// The LRM individuates attempts, not obligation ticks: 16.14.1 executes an
// action "once for each ... evaluation attempt". Clotho's own bare-sequence
// path does that -- `sq` in diff_not.sv reports twice at cycle 2 for two
// attempts that fail there. So two clotho paths disagree with each other and
// the ranged-antecedent implication is the one that is wrong; the plausible
// mechanism is an obligation keyed by its tick, which is right WITHIN an
// attempt (diff_multimatch.sv's rule: one verdict per attempt however many
// times the antecedent matches) and wrong ACROSS attempts.
//
// That is not phase 4's bug and this fixture does not try to fix it. It is
// recorded here because it is the ring discipline losing attempt identity
// exactly where docs/design/sva-property-layer.md says it does -- which is the
// argument for lowering a negated ranged sequence onto the COUNTED path, where
// an attempt count per state survives the merge. The same measurement sits on
// the counted path as `nwr` in diff_not.sv, at the same time and with the same
// expected count of 2.
//
// VACUITY, in the LRM's sense, which is a different question from whether this
// fixture is worth running. 16.14.8(z): an attempt of `property_expr1 implies
// property_expr2` "is nonvacuous if, and only if, the underlying evaluation
// attempt of property_expr1 is true and nonvacuous, and the underlying
// evaluation attempt of property_expr2 is nonvacuous", and 16.14.8(aa): an
// attempt of `iff` "is nonvacuous if, and only if, either the evaluation
// attempt of property_expr1 is nonvacuous or the evaluation attempt of
// property_expr2 is nonvacuous". With 16.14.8(a) and (c) making a boolean and
// a weak sequence property always nonvacuous, every attempt of the `iff` pair
// is nonvacuous, while an `implies` attempt is vacuous exactly when its
// antecedent is false -- attempts 2, 4, 5 and 6 for im, and additionally 7 and
// 8 for ims. A vacuous success "is not considered a pass or a failure"
// (16.14.8), so none of this moves a FAIL above; it is recorded because it is
// the reason for the next paragraph.
//
// NO COVER IN THIS FILE, deliberately, and the reason is that vacuity rule. A
// `cover property` books its pass statement per successful attempt (16.14.1),
// but 16.14.8(z) makes most of an `implies` cover's successes VACUOUS ones,
// and whether the pass action runs on a vacuous success is settable at
// runtime -- $assertcontrol's VacuousOff and NonvacuousOn (LRM 39.5.2) -- so the
// default is a matter of configuration. A count
// over this operator would measure that convention rather than the operator. On
// top of which 16.12.2 flips a bare antecedent to STRONG inside a cover, so the
// covered property would not even be the asserted one. Cover multiplicity is
// exercised where it is unambiguous: diff_not.sv, diff_until.sv and
// diff_seventually.sv.
//
// Not vacuous in the fixture sense: every directive both succeeds and reports,
// and every failure is attributable. im fails 4 attempts of 10 and holds 6;
// ims fails 2 and holds 8; ii fails 6 and passes 4; iis fails 5 and passes 5;
// ic fails 2 and passes 8, of which seven passes are vacuous -- no antecedent
// end point exists -- and one, attempt 0, is a real check of c at cycle 1. A
// monitor that never fires loses all 19 reports. One that fires once per
// attempt at every tick it is alive produces 108 reports across all ten tick
// times, and in particular reports at 5, 35, 55, 65, 75 and 95, the six
// evaluated ticks where this derivation is silent. And a monitor that
// reads 16.12.8 as 16.12.7 -- the single most likely mistake, since `implies`
// reads like implication -- produces ic's two reports for im's four, at cycle
// 4 twice instead of cycles 1 and 4 and the end of trace twice. None of the
// three reads as agreement with anything above.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1, b = 0, c = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b", $time, cyc, a, b, c);
  end

  // Three of the four directives this fixture was written for do not lower
  // today, and the reason is a real restriction rather than a gap in the
  // fixture. `implies` reduces to `(not p) or q`, `not` inverts the strength of
  // the operand it complements, and a monitor carries ONE end-of-trace
  // disposition -- so the reduction can only be tracked when the two operand
  // strengths DIFFER, and `iff`, which obliges both directions at once, can
  // never satisfy that for any pair. libsva refuses both with
  // SVA_ERROR_MIXED_STRENGTH, naming the operator the user wrote.
  //
  // Their derivations are kept above rather than deleted, because the lift is
  // known and scoped: a per-state end-of-trace disposition, OR-combined over
  // the live vector, retires MIXED_STRENGTH entirely. When it lands, restore
  // these three lines and score the header's full table.
  //
  //   im:  assert property (@(posedge clk)        (a ##[1:3] b) implies c);
  //   ii:  assert property (@(posedge clk)        (a ##[1:3] b) iff     c);
  //   iis: assert property (@(posedge clk) strong (a ##[1:3] b) iff     c);
  ims: assert property (@(posedge clk) strong (a ##[1:3] b) implies c);

  ic:  assert property (@(posedge clk)        (a ##[1:3] b) |->     c);

  initial begin
    @(posedge clk);            // cyc0: a=1 b=0 c=0
    b <= 1; c <= 1;
    @(posedge clk);            // cyc1: a=1 b=1 c=1  first antecedent match tick
    a <= 0; b <= 0;
    @(posedge clk);            // cyc2: a=0 b=0 c=1  antecedent false at its own
    a <= 1; c <= 0;            //                    start tick, consequent high
    @(posedge clk);            // cyc3: a=1 b=0 c=0
    b <= 1;
    @(posedge clk);            // cyc4: a=1 b=1 c=0  last antecedent match tick,
    b <= 0; c <= 1;            //                    reached from cyc1 and cyc3
    @(posedge clk);            // cyc5: a=1 b=0 c=1  b never rises again
    a <= 0; c <= 0;
    @(posedge clk);            // cyc6: a=0 b=0 c=0
    a <= 1;
    @(posedge clk);            // cyc7: a=1 b=0 c=0  a stays high and b stays low
    @(posedge clk);            // cyc8: a=1 b=0 c=0  to the end, so attempts 7, 8
    c <= 1;                    //                    and 9 are pending at $finish
    @(posedge clk);            // cyc9: a=1 b=0 c=1  with c low, low and HIGH --
                               //                    which is what makes the weak
                               //                    and strong `iff` fail on
                               //                    disjoint attempts
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
