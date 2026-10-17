// The followed-by operators, in both the overlapped and the nonoverlapped form.
//
// Clotho lowers this: both forms reach sva_prop_followed_by, which fuses the
// antecedent onto the consequent's automaton rather than making a trigger of it,
// so a followed-by carries no trigger where the implication it mirrors does.
// See docs/design/sva-property-layer.md, phase 1 (as built).
//
// The followed-by operators, LRM 16.12.9:
//
//   "For the followed-by property to succeed, the following shall hold:
//      -- From a given start point sequence_expr shall have at least one
//         successful match.
//      -- property_expr shall be successfully evaluated starting from one of
//         the match points of the sequence_expr."
//
// with the overlapped form starting the consequent AT the match end point and
// the nonoverlapped form starting it "the clock tick after the end point of
// the match". The same clause names them "the duals of the implication
// operators": `s #-# p` is `not (s |-> not p)`.
//
// THE HEADLINE DIFFERENCE FROM IMPLICATION, and the thing this fixture exists
// to pin: an antecedent that does not match makes an implication vacuously
// TRUE and makes a followed-by FALSE. `g |-> h` with g low is a pass; `g #-# h`
// with g low is a report. Cycle 6 below is exactly that case, and it is the
// single most likely thing for an implementation to get backwards.
//
// The offset between #-# and #=# is invisible in the middle of a trace, for the
// same reason the nexttime index is (see diff_nexttime.sv): a report is keyed
// by the tick at which the consequent fails, not by the tick the attempt
// started, so shifting the anchor moves WHICH attempt fails and not WHEN. It is
// visible only at the boundary. h is low at cycle 0, and `g #-# h` catches that
// from the cycle-0 attempt while `g #=# h` cannot -- that would need an attempt
// at cycle -1 -- so the report at t=5 belongs to the overlapped form alone.
//
// ---------------------------------------------------------------------------
// A READING THAT LOOKS RIGHT AND IS NOT. Worth keeping, because it is a
// semantic the implementation would otherwise get wrong.
// ---------------------------------------------------------------------------
//
// The tempting derivation has `fq`'s cycle-9 attempt FAIL at the end of trace.
// The reasoning is that 16.12.9 requires the antecedent to have an actual
// match, reinforced by the same clause's `sequence_expr #-# strong(seq1)` being
// equivalent to `strong(sequence_expr ##0 seq1)` -- so the antecedent side
// looks STRONG, and an antecedent left pending when the clock stops looks
// like a failure.
//
// It is wrong, and there is no report at t=105. The correct reading is LRM
// 16.12.15, which enumerates the strength-bearing operators:
//
//   "The property operators s_nexttime, s_always, s_eventually, s_until,
//    s_until_with, and sequence operator strong are strong ..."
//   "The property operators nexttime, always, until, eventually, until_with,
//    and sequence operator weak are weak ..."
//
// `#-#` and `#=#` appear in NEITHER list. Followed-by carries no strength of
// its own; it inherits from its operands. The consequents here are plain
// booleans, and 16.12.2 makes a bare sequence_expr in an `assert property`
// weak, so the whole followed-by is weak and an attempt still pending when the
// trace ends is 16.12.21's "Pending" level -- no bad state seen -- which does
// not report.
//
// The mistake was over-reading the `strong(...)` equivalence: it is stated for
// an EXPLICITLY strong consequent and does not push strength onto the
// antecedent of a followed-by whose consequent is weak. Concretely, for the
// implementation: `#-#`/`#=#` must NOT drain pending attempts at end of trace
// on their own account -- only an explicitly strong operand may do that.
//
// Both halves of the boundary are in the fixture and both are now consistent
// under that one rule: `fq`'s cycle-8 attempt has its CONSEQUENT pushed past
// the end and holds, and its cycle-9 attempt has its ANTECEDENT left unmatched
// and also holds. The absence of both reports is as much a part of what this
// fixture pins as any presence.
//
// ---------------------------------------------------------------------------
//
// Every antecedent below has at most one match point per attempt, deliberately:
// a multi-match antecedent lands on the per-attempt reporting rule, one verdict
// per attempt (docs/LRM_NOTES.md, note 23), and would confuse what this fixture
// pins.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    g  h  k
//    0     5    1  0  1     <- h low at the very first tick
//    1    15    1  1  1
//    2    25    1  1  1
//    3    35    1  0  1     <- h low mid-trace
//    4    45    1  1  1
//    5    55    1  1  0     <- k low mid-trace
//    6    65    0  1  1     <- g low: the antecedent does not match at all
//    7    75    1  1  1
//    8    85    1  1  1
//    9    95    1  1  1     last evaluated tick
//   10   105    1  1  1     $finish executes here, so this tick never evaluates
//
// Derived from the LRM, with the reading argued above folded in:
//
//   fo  g #-# h            -- the antecedent `g` matches at its own tick when
//       g is high, and the consequent is evaluated from that same tick, so
//       this holds exactly when g && h. It fails at cycle 0 (h low), cycle 3
//       (h low) and cycle 6 (g low, no match at all).
//                                            -> FAIL 5, FAIL 35, FAIL 65
//   fn  g #=# h            -- same antecedent, consequent one tick later, so
//       this holds exactly when g at N and h at N+1. h low at cycle 0 is
//       unreachable. h low at cycle 3 is caught by the cycle-2 attempt and
//       reported where h is evaluated. g low at cycle 6 is a non-match, known
//       at cycle 6. The cycle-9 attempt matches its antecedent and pushes a
//       weak consequent past the end of trace, so it holds.
//                                                     -> FAIL 35, FAIL 65
//   fq  (g ##1 h) #=# k    -- a two-tick antecedent, so the consequent starts
//       two ticks after the attempt:
//         cycle 2: g high, h low at cycle 3 -> antecedent cannot match, known
//                  at cycle 3.                                  -> FAIL 35
//         cycle 3: antecedent matches over cycles 3-4, consequent k at cycle
//                  5 is low.                                    -> FAIL 55
//         cycle 6: g low -> antecedent cannot match, known at cycle 6.
//                                                               -> FAIL 65
//         cycle 8: antecedent matches over cycles 8-9, consequent pushed onto
//                  the tick that never evaluates -> weak, holds.
//         cycle 9: antecedent still unmatched when the clock stops -> weak,
//                  holds. (This is the case argued out above.)
//
//   expected: 1 report at 5, 3 at 35, 1 at 55, 3 at 65, and NOTHING at 105 --
//   eight in all. t=5 belongs to `fo` alone and t=55 to `fq` alone, so two of
//   the four times identify a single assertion outright.
//
// Not vacuous: every one of the three assertions both passes and reports. `fo`
// passes at seven of the ten evaluated cycles, `fn` at seven, `fq` at five.
// The two failure MODES are separated too -- t=5, t=35 and t=55 are consequent
// failures, t=65 is an antecedent that never matched, and an implementation
// that reads followed-by as implication loses t=65 while keeping the rest.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          g   = 1, h = 0, k = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d g=%b h=%b k=%b", $time, cyc, g, h, k);
  end

  fo: assert property (@(posedge clk) g #-# h);
  fn: assert property (@(posedge clk) g #=# h);
  fq: assert property (@(posedge clk) (g ##1 h) #=# k);

  initial begin
    @(posedge clk);            // cyc0: g=1 h=0 k=1
    h <= 1;
    @(posedge clk);            // cyc1: h=1
    @(posedge clk);            // cyc2
    h <= 0;
    @(posedge clk);            // cyc3: h=0
    h <= 1;
    @(posedge clk);            // cyc4: h=1
    k <= 0;
    @(posedge clk);            // cyc5: k=0
    k <= 1; g <= 0;
    @(posedge clk);            // cyc6: g=0, so no antecedent matches here
    g <= 1;
    @(posedge clk);            // cyc7  the tail is quiet apart from the two
    @(posedge clk);            // cyc8  fq attempts left open at cycles 8 and
    @(posedge clk);            // cyc9  9, both of which hold
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
