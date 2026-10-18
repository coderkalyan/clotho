// The `not` property operator.
//
// Clotho lowers this: `not` is phase 4 of docs/design/sva-property-layer.md
// and complements the determinised obligation built by phase 3. Every
// expectation below was derived from the LRM before the implementation
// existed, so it cannot be an encoding of what clotho happens to do; the
// STATUS paragraph below records that `./build/clotho run
// tests/sva/diff_not.sv` now reproduces it.
//
// CLAUSE NUMBERING, verified against the IEEE Std 1800-2017 subclause list for
// 16.12 rather than taken on trust: negation is 16.12.3, NOT 16.12.6 or
// 16.12.7. In this standard 16.12.2 is Sequence property, 16.12.3 is Negation
// property, 16.12.4 Disjunction, 16.12.5 Conjunction, 16.12.6 If-else, 16.12.7
// Implication (`|->`, `|=>`), 16.12.8 Implies and iff. The weak/strong
// disposition of a pending attempt is 16.12.2 for sequence properties and
// 16.12.15 for the strength-bearing operators.
//
// LRM 16.12.3 IN FULL, because the clause is three sentences of semantics
// followed by the exact corner this fixture is built around:
//
//   "A property is a negation if it has the form not property_expr. For each
//    evaluation attempt of the property, there is an evaluation attempt of
//    property_expr. The keyword not states that the evaluation of the property
//    returns the opposite of the evaluation of the underlying property_expr.
//    Thus, if property_expr evaluates to true, then not property_expr evaluates
//    to false; and if property_expr evaluates to false, then not property_expr
//    evaluates to true."
//
//   "The not operator switches the strength of a property. In particular, one
//    should be careful when negating a sequence. For example, consider the
//    following assertion:
//
//        a1: assert property (@clk not a ##1 b);
//
//    Since the sequential property a ##1 b is used in an assertion, it is weak.
//    This means that if clk stops ticking and a holds at the last tick of clk,
//    the weak sequential property a ##1 b will also hold beginning at that
//    tick, and so the assertion a1 will fail. In this case it is more
//    reasonable to use:
//
//        a2: assert property (@clk not strong(a ##1 b));"
//
// THE STANDARD WROTE THIS FIXTURE. `nw` and `ns` below are the LRM's own a1
// and a2, and the paragraph explaining them is the requirement that a `not`
// fixture carry an attempt still pending when the clock stops, in both a weak
// and a strong spelling. Negation SWAPS the end-of-trace disposition, so a
// trace on which every attempt resolves before $finish cannot tell a correct
// `not` from an inverted one: both would score identically.
//
// The default strength that makes a1 weak is 16.12.2, and it is a property of
// the ENCLOSING DIRECTIVE, not of the position:
//
//   "If the strong or weak operator is omitted, then the evaluation of the
//    sequence_expr depends on the assertion statement in which it is used. If
//    the assertion statement is assert property or assume property, then the
//    sequence_expr is evaluated as weak(sequence_expr). Otherwise, the
//    sequence_expr is evaluated as strong(sequence_expr)."
//
// which is why `cn` below spells `weak(...)` out. A `cover property` is one of
// the "otherwise" cases, so a bare `a ##1 b` inside a cover is the STRONG
// sequence property and would not be the same property `nw` asserts. That trap
// is worth naming because it turns the cover into a separate, unrelated
// measurement.
//
// with 16.12.2 also supplying what weak and strong mean on a finite trace:
//
//   "strong(sequence_expr) evaluates to true if, and only if, there is a
//    nonempty match of the sequence_expr. weak(sequence_expr) evaluates to true
//    if, and only if, there is no finite prefix that witnesses inability to
//    match the sequence_expr."
//
// Two axes, and each is visible at exactly one kind of tick:
//
//   COMPLEMENT   whether the verdict is inverted at all. Visible at every tick
//                where the operand resolves: `nw`/`ns` report exactly where the
//                operand's own assertion (`sq`) is silent, and vice versa.
//   STRENGTH     what a pending attempt becomes when the clock stops. Invisible
//                unless an attempt IS pending there, which is what 16.12.3's
//                "if clk stops ticking and a holds at the last tick" arranges.
//                a is high for the last three ticks and b never rises again
//                after cycle 4, so both negated sequences have attempts left
//                open at $finish and dispose of them oppositely.
//
// The scan rule each operand reduces to, per attempt starting at cycle N:
//
//   a ##1 b        a[N] low -> FALSE at N (that prefix witnesses inability);
//                  else b[N+1] high -> TRUE at N+1; b[N+1] low -> FALSE at N+1;
//                  else tick N+1 does not exist -> still pending.
//   a ##[1:3] b    a[N] low -> FALSE at N; else FALSE/TRUE at the first tick in
//                  N+1..N+3 where b is high (TRUE) or, if all three exist and b
//                  is low at each, FALSE at N+3; else still pending.
//
// A pending operand then resolves by strength and only there: weak holds,
// strong fails (16.12.2 above, 16.12.15 for the operator list), and `not`
// inverts whichever it gets.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    a  b
//    0     5    1  0     <- a high with b high NEXT tick: the operand MATCHES
//                           from here, so the negations fail at cycle 1
//    1    15    1  1
//    2    25    0  0     <- a low: the operand is refuted at its own start tick,
//                           so the negations hold and `sq` fails, here
//    3    35    1  0     <- second match: b is high at cycle 4
//    4    45    1  1
//    5    55    1  0     b never rises again from here on, so from cycle 5
//    6    65    0  0     onwards every attempt of the operand either is refuted
//    7    75    1  0     at a definite tick or is left pending
//    8    85    1  0
//    9    95    1  0     <- a HIGH at the last evaluated tick. This is 16.12.3's
//                           corner: one attempt of `a ##1 b` and three of
//                           `a ##[1:3] b` are pending when the clock stops
//   10   105    -  -     $finish executes here, so this tick never evaluates
//
// Derived from the LRM. An attempt starts at every tick, so ten attempts per
// directive, and a verdict belongs to an attempt rather than to a cycle -- the
// repeated times below are multiplicity, not duplication. The per-attempt rule is 16.12.7's worked example, which
// enumerates one evaluation per clock tick and a separate verdict for each
// ("the property fails at clock tick 1. Similarly, there is a failure at clock
// ticks 2, 3, and 4"), together with 16.14.1, which runs an action "once for
// each ... evaluation attempt". The sibling fixtures in this directory cite
// 16.12.1 for it; in IEEE Std 1800-2017 that subclause is Property
// instantiation, so the citation is wrong even though the rule is not.
//
//   nw   not (a ##1 b)          -- the LRM's a1; bare sequence, so weak. The
//        operand is TRUE for attempts 0 and 3 (matching at cycles 1 and 4), so
//        those two fail. It is FALSE for attempts 1, 2, 4, 5, 6, 7 and 8, which
//        hold. Attempt 9 is pending, holds weakly, and the negation of a hold
//        is a failure -- at the end of the trace, where the hold is decided.
//                                             -> FAIL 15, FAIL 45, FAILEND 105
//   ns   not strong(a ##1 b)    -- the LRM's a2. Identical to nw everywhere a
//        verdict is reached mid-trace, because strength changes nothing about a
//        match that happens. Attempt 9's operand demands a nonempty match and
//        the trace has no tick 10 to supply one, so the operand is false and the
//        negation HOLDS.                       -> FAIL 15, FAIL 45, and nothing
//                                                 at the end of trace
//   nwr  not (a ##[1:3] b)      -- same shape over a three-tick window, whose
//        only purpose is end-of-trace MULTIPLICITY: attempts 7, 8 and 9 all run
//        off the end of the trace with b low at every window tick that exists.
//        The operand is TRUE for attempt 0 (b at cycle 1) and attempts 1 and 3
//        (both reaching b at cycle 4); FALSE for 2, 4, 5 and 6.
//                                        -> FAIL 15, FAIL 45 x2, FAILEND 105 x3
//   nsr  not strong(a ##[1:3] b) -- nwr's mid-trace failures and none of its
//        end-of-trace ones.                     -> FAIL 15, FAIL 45 x2
//   sq   a ##1 b                -- nw's operand, asserted directly, so it fails
//        exactly where nw holds: attempts 1 and 2 at cycle 2, attempt 4 at cycle
//        5, attempts 5 and 6 at cycle 6, attempt 7 at cycle 8, attempt 8 at
//        cycle 9. Attempts 0 and 3 match; attempt 9 is pending and holds weakly.
//                        -> FAIL 25 x2, FAIL 55, FAIL 65 x2, FAIL 85, FAIL 95
//   nn   not (not (a ##1 b))    -- THE DOUBLE-NEGATION CONTROL, and it must
//        reproduce `sq` report for report, including the silence at the end of
//        the trace. 16.12.3 says the evaluation "returns the opposite of the
//        evaluation of the underlying property_expr", and the opposite of the
//        opposite is the thing itself; the strength swap is a COROLLARY of that
//        sentence rather than an independent rule, so two swaps restore weak and
//        attempt 9 holds. A complement that is not an involution -- one that
//        loses pending, or that reads "swap the strength" as an extra operation
//        applied once -- makes nn diverge from sq while both remain plausible on
//        their own.        -> FAIL 25 x2, FAIL 55, FAIL 65 x2, FAIL 85, FAIL 95
//
//   expected: 4 reports at 15, 4 at 25, 6 at 45, 2 at 55, 4 at 65, 2 at 85, 2 at
//   95, and 4 after the $finish banner -- 28 in all.
//
// Each of those counts measures something the others do not. The 4 at cycle 1
// is every negated directive at once and pins nothing but the complement's
// polarity: it is where the operand is TRUE and all four negations must fire.
// The 6 at cycle 4 is 1 + 1 + 2 + 2, and the 2s are the window forms reaching
// b at cycle 4 from two different start ticks, so it pins attempt multiplicity
// under a negation -- a monitor that merges concurrent attempts into one state
// reports 4 there. The 4 at the end of trace is 1 from nw and 3 from nwr and
// nothing from the strong pair, so it pins the strength swap. Ten of the sixty
// attempts in this file are still pending when the clock stops -- attempt 9
// for each of nw, ns, sq and nn, and attempts 7, 8 and 9 for each window form
// -- so an implementation that drops pending attempts at $finish scores 0
// there, one that fails them all regardless of strength scores 10, and one
// that omits the swap -- resolving a pending `not p` as p's own disposition
// rather than its opposite -- scores the same 4 ON THE WRONG PAIR: ns and nsr
// report, nw and nwr are silent, and sq and nn are untouched because neither
// depends on the swap. That last one is why this derivation lists the reports
// per directive and not just per time: the total does not move.
//
// STATUS. Clotho lowers every directive here, and `./build/clotho run
// tests/sva/diff_not.sv` reproduces the table above exactly: 28 reports (4 at
// 15, 4 at 25, 6 at 45, 2 at 55, 4 at 65, 2 at 85, 2 at 95 and 4 at the
// $finish banner) and 7 cover hits, per directive as derived. That includes
// the two same-tick pairs of `sq`, which is the per-attempt reporting rule
// (16.14.1) holding for two attempts that fail at one tick, and `nwr`'s 2 at
// cycle 4, which is the same measurement on the counted path, where an
// attempt count per state survives determinisation. diff_implies.sv's `ic`
// is that measurement on a ranged-antecedent implication. The file is a
// by-hand fixture: nothing in the test suite runs it.
//
// THE COVER, and the reason it is not redundant with `sq`. `cn` counts how
// many attempts of nw's property SUCCEED, spelled with the strength 16.12.2
// gives it in an assert made explicit, because a cover is one of the contexts
// where the bare spelling would mean something else. nw's property succeeds
// exactly when its operand is false, so:
//
//   cn   -> COVER cn 7        (attempts 1, 2, 4, 5, 6, 7 and 8)
//
// and that 7 is the same ten attempts nw's 3 reports account for: 3 + 7 = 10.
// THAT SUM IS THE POINT. Negation is what forces the verdict lattice to be
// total -- a pending attempt must become true or false when the clock stops,
// because `not` of pending is pending and something has to be reported -- so
// every attempt of this property is scored exactly once, and an implementation
// that leaves one unscored breaks the sum without necessarily breaking either
// number on its own.
//
// The count is per ATTEMPT, not per hit cycle. The seven hits land on only
// five ticks (two at cycle 2 and two at cycle 6), and LRM 16.14.1 settles
// which reading is right:
//
//   "The pass statement specified in statement_or_null shall be executed ONCE
//    FOR EACH SUCCESSFUL EVALUATION ATTEMPT of the underlying property_spec."
//
// so 7, not 5. An implementation that books one hit per hit cycle scores 5.
// Attempt 9 is NOT a hit: its operand holds weakly, so the negation fails, and
// a failing cover has no report to make (16.14.1 gives a cover a pass statement
// only).
//
// No hit here is a vacuous one, and for `not` the LRM says so directly.
// 16.14.8(d): "An evaluation attempt of a property of the form not
// property_expr is nonvacuous if, and only if, the underlying evaluation
// attempt of property_expr is nonvacuous", and 16.14.8(c): "An evaluation
// attempt of a property of the form weak(sequence_expr) is always nonvacuous."
// So every attempt of `cn` is nonvacuous and none of the seven can land in the
// standard's separate vacuous-success counter. slang's -Wvacuous-cover, if it
// fires here, is syntactic in the same way it is in diff_seventually.sv.
//
// NO `not <boolean>` DIRECTIVE, deliberately. `not b` resolves at its own tick
// and would add a report at every tick where b is high, which pins nothing the
// four negated sequences do not already pin while making every per-time count
// above carry a second contribution. The complement of a boolean is the one
// case of `not` that is hard to get wrong: it has no pending
// state to dispose of, which is the entire difficulty this file measures.
//
// Not vacuous, in the fixture sense: every directive both succeeds and
// reports. nw fails 3 attempts of 10 and holds 7; ns fails 2 and holds 8; nwr
// fails 6 and holds 4; nsr fails 3 and holds 7; sq and nn each fail 7 and pass
// 3, and two of those three passes are real matches rather than end-of-trace
// forgiveness. A monitor that never fires loses all 28 reports and all 7 cover
// hits. One that fires once per attempt at every tick it is alive produces 116
// reports across all ten tick times, and in particular reports at 5, 35 and 75,
// the three evaluated ticks where this derivation is silent. Neither reads as
// agreement with anything above.
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

  nw:  assert property (@(posedge clk) not         (a ##1     b));
  ns:  assert property (@(posedge clk) not strong  (a ##1     b));
  nwr: assert property (@(posedge clk) not         (a ##[1:3] b));
  nsr: assert property (@(posedge clk) not strong  (a ##[1:3] b));

  sq:  assert property (@(posedge clk)             (a ##1     b));
  nn:  assert property (@(posedge clk) not (not    (a ##1     b)));

  cn:  cover  property (@(posedge clk) not weak    (a ##1     b))
         $display("[%0t] COVER cn", $time);

  initial begin
    @(posedge clk);            // cyc0: a=1 b=0
    b <= 1;
    @(posedge clk);            // cyc1: a=1 b=1  the operand matches from cyc0
    a <= 0; b <= 0;
    @(posedge clk);            // cyc2: a=0 b=0  refuted at its own start tick
    a <= 1;
    @(posedge clk);            // cyc3: a=1 b=0
    b <= 1;
    @(posedge clk);            // cyc4: a=1 b=1  second match, reached from cyc1
    b <= 0;                    //                and cyc3 by the window form
    @(posedge clk);            // cyc5: a=1 b=0  b never rises again
    a <= 0;
    @(posedge clk);            // cyc6: a=0 b=0
    a <= 1;
    @(posedge clk);            // cyc7: a=1 b=0  a stays high to the end, so the
    @(posedge clk);            // cyc8: a=1 b=0  window form leaves attempts 7, 8
    @(posedge clk);            // cyc9: a=1 b=0  and 9 pending and `a ##1 b`
                               //                leaves attempt 9 pending
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
