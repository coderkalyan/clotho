#include "Sva.h"

#ifdef CLOTHO_HAVE_SVA

#include <sva.h>

#include <slang/ast/Expression.h>
#include <slang/ast/TimingControl.h>
#include <slang/ast/expressions/AssertionExpr.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/expressions/OperatorExpressions.h>
#include <slang/ast/symbols/ValueSymbol.h>
#include <slang/ast/types/Type.h>

#include <ankerl/unordered_dense.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace clotho::sva {
namespace {

using slang::ast::AssertionExpr;
using slang::ast::AssertionExprKind;
using slang::ast::AssertionInstanceExpression;
using slang::ast::BinaryAssertionOperator;
using slang::ast::Expression;
using slang::ast::ExpressionKind;

// The libsva header this file was compiled against, checked against the archive
// actually linked. libsva used to enforce the pairing with size constants for
// its objects, which could only ever be checked from one side: a header from a
// different build passed compilation and then overran memory at run time. The
// objects are opaque now and a version replaces the sizes, because a version can
// be compared from both sides.
//
// Deliberately not a global constructor. A build mismatch is worth catching the
// first time clotho actually lowers an assertion -- in a run that was going to
// touch libsva anyway -- rather than at load time in every invocation that never
// sees one. The function-local static makes that one relaxed load afterwards.
//
// Not an assert() either: NDEBUG is set in Release, and a release build silently
// reading tables through a mismatched ABI is exactly the failure being removed.
void check_libsva_abi() {
    static const bool ok = [] {
        const uint32_t linked = sva_abi_version();
        if (linked != SVA_ABI_VERSION) {
            std::fprintf(stderr,
                         "clotho: libsva ABI mismatch -- compiled against version %u, "
                         "linked against version %u. The header and the static library "
                         "are from different builds of libsva; rebuild it.\n",
                         static_cast<unsigned>(SVA_ABI_VERSION),
                         static_cast<unsigned>(linked));
            std::abort();
        }
        return true;
    }();
    (void)ok;
}

// libsva reports failures against a node; the front-end reports against source.
// This maps one to the other by remembering, per libsva node, the AssertionExpr
// that produced it.
class Lowering {
public:
    Lowering(Result& out, const slang::ast::TimingControl* clock, const Expression* disable)
        : out_(out), clock_(clock), disable_(disable) {
        ir_ = sva_ir_create();
    }
    ~Lowering() {
        sva_ir_destroy(ir_);
    }
    Lowering(const Lowering&) = delete;
    Lowering& operator=(const Lowering&) = delete;

    // Null when the IR could not be allocated. lower() checks that once, before
    // any builder runs, so no caller below here has to.
    sva_ir* ir() {
        return ir_;
    }

    // Record a failure once; the first one wins so the innermost cause survives.
    std::nullopt_t reject(std::string message, const AssertionExpr* blame) {
        if (out_.error.empty()) {
            out_.error = std::move(message);
            out_.blame = blame;
        }
        return std::nullopt;
    }

    // -- Boolean layer -------------------------------------------------------
    //
    // Negation normal form. `&&` and `||` commute with SystemVerilog's
    // unknown-is-false collapse, so they become libsva structure; `!` does not,
    // so it is pushed to the leaves by De Morgan and a negated leaf becomes an
    // atom over the negated expression. libsva's bnot is never emitted --
    // it assumes two-valued atoms. See concurrent-assertions.md 5.2-5.3.
    std::optional<sva_node> boolean(const Expression& expr, bool negated,
                                    const AssertionExpr* blame) {
        if (expr.kind == ExpressionKind::UnaryOp) {
            const auto& unary = expr.as<slang::ast::UnaryExpression>();
            if (unary.op == slang::ast::UnaryOperator::LogicalNot) {
                return boolean(unary.operand(), !negated, blame);
            }
        } else if (expr.kind == ExpressionKind::BinaryOp) {
            const auto& binary = expr.as<slang::ast::BinaryExpression>();
            const bool is_and = binary.op == slang::ast::BinaryOperator::LogicalAnd;
            const bool is_or = binary.op == slang::ast::BinaryOperator::LogicalOr;
            if (is_and || is_or) {
                const auto lhs = boolean(binary.left(), negated, blame);
                if (!lhs) {
                    return std::nullopt;
                }
                const auto rhs = boolean(binary.right(), negated, blame);
                if (!rhs) {
                    return std::nullopt;
                }
                // De Morgan: a negated conjunction is a disjunction of the
                // negated operands, and vice versa.
                sva_node node = 0;
                const bool want_and = is_and != negated;
                const sva_status status = want_and ? sva_bool_and(ir_, *lhs, *rhs, &node)
                                                   : sva_bool_or(ir_, *lhs, *rhs, &node);
                if (status != SVA_OK) {
                    return reject("out of memory building an assertion guard", blame);
                }
                return node;
            }
        }
        return atom(expr, negated, blame);
    }

    // -- Sequence layer ------------------------------------------------------

    std::optional<sva_node> sequence(const AssertionExpr& expr) {
        switch (expr.kind) {
            case AssertionExprKind::Simple: {
                const auto& simple = expr.as<slang::ast::SimpleAssertionExpr>();
                // A named sequence use, or a formal argument bound to one,
                // arrives as an AssertionInstanceExpression nested here. slang
                // expands it eagerly -- `body` already has the actual arguments
                // substituted (MiscExpressions.h:235) -- so an instantiation is
                // a recursion into that body, not a substitution pass. This is
                // checked before the repetition below, because the operand of a
                // repetition over an instance is the instance's body, not a
                // boolean, and `boolean()` would silently make an atom of it.
                if (const auto* inst = as_instance(simple.expr)) {
                    if (!instance_supported(*inst, expr)) {
                        return std::nullopt;
                    }
                    const auto body = sequence(inst->body);
                    if (!body || !simple.repetition) {
                        return body;
                    }
                    return repeat(*simple.repetition, *body, is_boolean_sequence(inst->body),
                                  expr);
                }
                const auto guard = boolean(simple.expr, false, &expr);
                if (!guard) {
                    return std::nullopt;
                }
                sva_node node = 0;
                if (sva_seq_boolean(ir_, *guard, &node) != SVA_OK) {
                    return reject("out of memory building an assertion sequence", &expr);
                }
                if (simple.repetition) {
                    // The operand is the seq_boolean just built, which is the
                    // shape goto and non-consecutive repetition require.
                    return repeat(*simple.repetition, node, true, expr);
                }
                return node;
            }
            case AssertionExprKind::SequenceConcat:
                return concat(expr.as<slang::ast::SequenceConcatExpr>(), expr);
            case AssertionExprKind::SequenceWithMatch: {
                // `(seq)[*n]`. slang only builds this node for a parenthesized
                // sequence that carries a repetition or match items; a plain
                // `(seq)` is the inner expression itself.
                const auto& with = expr.as<slang::ast::SequenceWithMatchExpr>();
                if (!with.matchItems.empty()) {
                    // TODO: local variables and match items (LRM 16.10) -- the
                    // assignments would have to run on a match, and libsva's
                    // automaton carries no per-attempt data.
                    return reject("sequence match items are not supported yet", &expr);
                }
                const auto seq = sequence(with.expr);
                if (!seq) {
                    return std::nullopt;
                }
                if (!with.repetition) {
                    return seq;
                }
                return repeat(*with.repetition, *seq, is_boolean_sequence(with.expr), expr);
            }
            case AssertionExprKind::Clocking: {
                const auto& wrapper = expr.as<slang::ast::ClockingAssertionExpr>();
                if (!is_assertion_clock(wrapper)) {
                    // TODO: multiclock sequences (LRM 16.13).
                    return reject("multiclock sequences are not supported yet", &expr);
                }
                return sequence(wrapper.expr);
            }
            case AssertionExprKind::Binary: {
                const auto& binary = expr.as<slang::ast::BinaryAssertionExpr>();
                // `throughout` is the one sequence-layer operator whose left
                // operand is a boolean rather than a sequence, so it does not
                // share the shape below.
                if (binary.op == BinaryAssertionOperator::Throughout) {
                    return throughout(binary, expr);
                }
                // The other four take the same two sequences in the same order,
                // so the operator only selects which builder runs.
                decltype(&sva_seq_or) build = nullptr;
                switch (binary.op) {
                    case BinaryAssertionOperator::Or:
                        build = sva_seq_or;
                        break;
                    case BinaryAssertionOperator::And:
                        build = sva_seq_and;
                        break;
                    case BinaryAssertionOperator::Intersect:
                        build = sva_seq_intersect;
                        break;
                    case BinaryAssertionOperator::Within:
                        // `a within b` puts a inside b, so the left operand is
                        // the inner one. libsva builds this from tags it already
                        // has rather than as an operator of its own.
                        build = sva_seq_within;
                        break;
                    default:
                        // Every sequence-layer binary operator of LRM 16.9 is
                        // handled above, so what lands here is a property-layer
                        // one that reached a sequence position through
                        // property()'s fall-through -- `until`, `implies`, or an
                        // implication nested where a sequence belongs.
                        return reject("this sequence operator is not supported yet", &expr);
                }
                // Both operands are sequences, so this recurses through
                // sequence() and never through property(): these are
                // sequence-layer operators, and the identically spelled
                // property-layer `and` and `or` are different nodes slang builds
                // only where a property is allowed.
                const auto lhs = sequence(binary.left);
                if (!lhs) {
                    return std::nullopt;
                }
                const auto rhs = sequence(binary.right);
                if (!rhs) {
                    return std::nullopt;
                }
                sva_node node = 0;
                if (build(ir_, *lhs, *rhs, &node) != SVA_OK) {
                    return reject("out of memory building an assertion sequence", &expr);
                }
                return node;
            }
            case AssertionExprKind::FirstMatch: {
                const auto& first = expr.as<slang::ast::FirstMatchAssertionExpr>();
                if (!first.matchItems.empty()) {
                    // TODO: local variables and match items (LRM 16.10) -- the
                    // assignments would have to run on a match, and libsva's
                    // automaton carries no per-attempt data.
                    return reject("sequence match items are not supported yet", &expr);
                }
                const auto seq = sequence(first.seq);
                if (!seq) {
                    return std::nullopt;
                }
                sva_node node = 0;
                // The one builder here whose failure is not always exhaustion:
                // first_match keeps only the shortest match from each start, so
                // whether a match survives depends on the others, which is not a
                // language operation libsva can do by combining automata. It
                // determinises the operand instead, and that subset construction
                // is the one in this IR whose state count can be exponential in
                // its operand's -- so SVA_ERROR_STATE_LIMIT is a reachable,
                // user-visible outcome and gets the sentence sva_lower uses for
                // it rather than a misleading complaint about memory.
                const sva_status status = sva_seq_first_match(ir_, *seq, &node);
                if (status == SVA_ERROR_STATE_LIMIT) {
                    return reject("`first_match` needs more automaton states than the "
                                  "limit allows, because it determinises its operand",
                                  &expr);
                }
                if (status != SVA_OK) {
                    return reject("out of memory building an assertion sequence", &expr);
                }
                return node;
            }
            default:
                // Every sequence-layer operator of LRM 16.9 is handled above.
                // What lands here is a property-layer node that reached a
                // sequence position -- `not`/`nexttime`/`always`/`eventually`
                // (Unary), `strong`/`weak` (StrongWeak), `accept_on` and its
                // family (Abort), and the case / if-else property forms.
                return reject("this sequence operator is not supported yet", &expr);
        }
    }

    // `exp throughout seq` (LRM 16.9). The left operand is a BOOLEAN and not a
    // sequence -- it is a condition on every cycle the match spans, conjoined
    // into each state of the right operand's automaton -- and sva_seq_throughout
    // asserts that rather than reporting it. slang types the operand as an
    // AssertionExpr all the same, so it is unwrapped back to its Expression and
    // lowered through boolean(), which is also what puts it in negation normal
    // form.
    std::optional<sva_node> throughout(const slang::ast::BinaryAssertionExpr& binary,
                                       const AssertionExpr& blame) {
        // Three shapes are not a boolean: a node that is not Simple at all, a
        // Simple carrying a repetition, and an instance whose body is a
        // sequence. slang refuses all three itself (ThroughoutLhsInvalid) and
        // that is the diagnostic a user normally sees, so this is the backstop
        // that keeps a wrong shape away from a builder which aborts on one.
        const auto* simple = binary.left.kind == AssertionExprKind::Simple
                                 ? &binary.left.as<slang::ast::SimpleAssertionExpr>()
                                 : nullptr;
        if (simple == nullptr || simple->repetition || as_instance(simple->expr) != nullptr) {
            return reject("the left operand of `throughout` must be a boolean expression, "
                          "not a sequence",
                          &blame);
        }
        const auto guard = boolean(simple->expr, false, &blame);
        if (!guard) {
            return std::nullopt;
        }
        const auto seq = sequence(binary.right);
        if (!seq) {
            return std::nullopt;
        }
        sva_node node = 0;
        if (sva_seq_throughout(ir_, *guard, *seq, &node) != SVA_OK) {
            return reject("out of memory building an assertion sequence", &blame);
        }
        return node;
    }

    // `a ##[lo:hi] b ##[lo2:hi2] c ...`. libsva's seq_delay is binary and
    // left-associative, and a leading delay is spelled with an explicit `1'b1`
    // one-cycle sequence on the left, because no tree edge is optional.
    std::optional<sva_node> concat(const slang::ast::SequenceConcatExpr& concat_expr,
                                   const AssertionExpr& blame) {
        std::optional<sva_node> acc;
        for (const auto& element : concat_expr.elements) {
            const auto next = sequence(*element.sequence);
            if (!next) {
                return std::nullopt;
            }
            const uint32_t lo = element.delay.min;
            const uint32_t hi = element.delay.max ? *element.delay.max : SVA_UNBOUNDED;
            if (lo > hi) {
                return reject("delay range lower bound exceeds its upper bound", &blame);
            }
            if (!acc) {
                if (lo == 0 && hi == 0) {
                    // The usual case: the first element carries no delay.
                    acc = next;
                    continue;
                }
                // A leading `##n`: `1'b1 ##[lo:hi] rest`, measured from the
                // attempt start.
                const auto head = one_cycle_true(blame);
                if (!head) {
                    return std::nullopt;
                }
                acc = head;
            }
            sva_node node = 0;
            if (sva_seq_delay(ir_, *acc, lo, hi, *next, &node) != SVA_OK) {
                return reject("out of memory building an assertion delay", &blame);
            }
            acc = node;
        }
        if (!acc) {
            return reject("empty sequence", &blame);
        }
        return acc;
    }

    // `seq[*lo:hi]`, `b[->lo:hi]` and `b[=lo:hi]` (LRM 16.9.2). libsva asserts
    // its preconditions rather than reporting them, so each one is a refusal
    // here: by the time a node exists it is too late. See
    // concurrent-assertions.md 9.1.
    std::optional<sva_node> repeat(const slang::ast::SequenceRepetition& repetition,
                                   sva_node operand, bool operand_is_boolean,
                                   const AssertionExpr& blame) {
        sva_repeat kind = SVA_REPEAT_CONSECUTIVE;
        switch (repetition.kind) {
            case slang::ast::SequenceRepetition::Consecutive:
                kind = SVA_REPEAT_CONSECUTIVE;
                break;
            case slang::ast::SequenceRepetition::GoTo:
                kind = SVA_REPEAT_GOTO;
                break;
            case slang::ast::SequenceRepetition::Nonconsecutive:
                kind = SVA_REPEAT_NONCONSECUTIVE;
                break;
        }

        const uint32_t lo = repetition.range.min;
        const uint32_t hi = repetition.range.max ? *repetition.range.max : SVA_UNBOUNDED;
        if (lo == 0) {
            // `[*0:n]` admits the empty match, which libsva has no node for:
            // eliminating it is a rewrite of whatever *encloses* the repetition,
            // not of the repetition. slang agrees -- checkNondegeneracy() reports
            // AdmitsEmpty -- and refuses the shapes the LRM itself bans, so what
            // reaches here is a legal one clotho cannot yet expand.
            return reject("a repetition with a zero lower bound admits the empty "
                          "sequence, which is not supported yet",
                          &blame);
        }
        if (lo > hi) {
            return reject("repetition range lower bound exceeds its upper bound", &blame);
        }
        if (kind != SVA_REPEAT_CONSECUTIVE && !operand_is_boolean) {
            // Goto and non-consecutive repetition are defined by the cycles in
            // which the operand does *not* hold, which needs its negation, and
            // "not necessarily back to back" has no reading for an operand whose
            // own matches can overlap.
            return reject("goto and non-consecutive repetition of a compound sequence "
                          "is not supported",
                          &blame);
        }
        sva_node node = 0;
        if (sva_seq_repeat(ir_, kind, operand, lo, hi, &node) != SVA_OK) {
            return reject("out of memory building an assertion repetition", &blame);
        }
        return node;
    }

    // Whether this sequence lowers to a single libsva seq_boolean, which is what
    // goto and non-consecutive repetition require of their operand. The question
    // is asked of slang's tree because libsva aborts on a wrong answer instead of
    // returning one.
    static bool is_boolean_sequence(const AssertionExpr& expr) {
        if (expr.kind != AssertionExprKind::Simple) {
            return false;
        }
        const auto& simple = expr.as<slang::ast::SimpleAssertionExpr>();
        if (simple.repetition) {
            // A repeated boolean is a sequence, not a boolean.
            return false;
        }
        if (const auto* inst = as_instance(simple.expr)) {
            // An instance is whatever its expanded body is.
            return is_boolean_sequence(inst->body);
        }
        return true;
    }

    // -- Named sequences and properties (LRM 16.8, 16.12) --------------------

    // An instance of a named sequence or property, or of a formal argument bound
    // to one. Null for anything else, so a caller can fall through to its
    // ordinary handling.
    //
    // A `let` never arrives here: slang expands a let call straight to the
    // substituted Expression (MiscExpressions.cpp:1023-1025) instead of wrapping
    // it, so a let inside an assertion lowers through the ordinary boolean path
    // with no special case at all.
    static const AssertionInstanceExpression* as_instance(const Expression& expr) {
        if (expr.kind != ExpressionKind::AssertionInstance) {
            return nullptr;
        }
        return &expr.as<AssertionInstanceExpression>();
    }

    // Which layer an instance's body sits in. slang types a sequence instance
    // `sequence` and a property instance `property`, and does the same for an
    // untyped formal argument bound to one (MiscExpressions.cpp:1310-1325), so
    // the type is the discriminator and the symbol kind is not.
    static bool is_property_instance(const AssertionInstanceExpression& inst) {
        return inst.type->getCanonicalType().kind == slang::ast::SymbolKind::PropertyType;
    }

    // The two instance shapes clotho refuses. Both are decided before the body
    // is walked, because for a recursive one the body slang hands back is an
    // InvalidAssertionExpr placeholder rather than the declaration
    // (MiscExpressions.cpp:878-884), and walking it would blame the wrong thing.
    bool instance_supported(const AssertionInstanceExpression& inst,
                            const AssertionExpr& blame) {
        if (!inst.localVars.empty()) {
            // TODO: local variables in a sequence or property (LRM 16.10) --
            // every attempt in flight would carry its own copy, and libsva's
            // automaton is a plain state set with no data to attach one to.
            reject("local variables in a sequence or property are not supported yet", &blame);
            return false;
        }
        if (inst.isRecursiveProperty) {
            // TODO: recursive properties (LRM 16.12.4) -- an unbounded unrolling
            // that no finite automaton represents.
            reject("a recursive property is not supported yet", &blame);
            return false;
        }
        return true;
    }

    // -- Clock flow (LRM 16.13.1) --------------------------------------------

    // Whether this nested clocking event is the one that became the assertion's
    // clock. A named sequence or property may declare its own event, and 16.13.1
    // flows it out over the whole assertion when it is the only one there; the
    // caller made that call and passed the event back as Request::clock, so the
    // node here is that same event written where the user wrote it, and walking
    // through it is exactly what the hoist means.
    //
    // Equivalence and not identity, because the same declaration instantiated
    // twice is one clock written twice: slang expands each use into its own
    // bound tree, so two uses of `sequence s; @(posedge clk) a ##1 b; endsequence`
    // arrive as two distinct TimingControl objects naming the same event.
    //
    // Anything that does not match is a genuinely second clock, and this is the
    // one place that decides so -- the caller's choice is only ever safe because
    // every Clocking node in the tree passes through here.
    bool is_assertion_clock(const slang::ast::ClockingAssertionExpr& wrapper) const {
        return clock_ != nullptr && wrapper.clocking.isEquivalentTo(*clock_);
    }

    // Whether this `disable iff` is the one that became the assertion's reset.
    // Same hoist as the clock above: LRM 16.12 allows the clause at the top of a
    // property declaration as well as at the top of the assertion, the caller
    // lifted whichever one is there into Request::disable, and stepping over that
    // node here is exactly what the lift means.
    //
    // Identity and not equivalence, which is where this parts company with
    // is_assertion_clock. A clock is written once and may be instantiated many
    // times, so two structurally equal events are one clock; 16.12 instead says
    // "Nesting of disable iff clauses, explicitly or through property
    // instantiations, is not allowed", so a second clause is a second reset
    // however it is spelled and must not be absorbed by matching the first one's
    // text. slang hands back one bound body per instantiation with the actuals
    // already substituted, so the condition the caller passed back IS the object
    // reached here -- pointer equality is exact rather than approximate.
    bool is_assertion_disable(const slang::ast::DisableIffAssertionExpr& wrapper) const {
        return disable_ != nullptr && &wrapper.condition == disable_;
    }

    // -- Property layer ------------------------------------------------------

    std::optional<sva_node> property(const AssertionExpr& expr,
                                     sva_strength strength = SVA_WEAK) {
        switch (expr.kind) {
            case AssertionExprKind::StrongWeak: {
                const auto& sw = expr.as<slang::ast::StrongWeakAssertionExpr>();
                const sva_strength inner =
                    sw.strength == slang::ast::StrongWeakAssertionExpr::Strong ? SVA_STRONG
                                                                              : SVA_WEAK;
                return property(sw.expr, inner);
            }
            case AssertionExprKind::Simple: {
                // A named property use. Its body is a property, so it has to be
                // lowered at this layer: routing it through the sequence tail
                // below would hand prop_sequence a property operand, and libsva
                // asserts on operand layer rather than reporting it. A sequence
                // instance deliberately falls through instead, because "a
                // sequence used as a property" is exactly what that tail does.
                const auto& simple = expr.as<slang::ast::SimpleAssertionExpr>();
                const auto* inst = simple.repetition ? nullptr : as_instance(simple.expr);
                if (inst == nullptr || !is_property_instance(*inst)) {
                    break;
                }
                if (!instance_supported(*inst, expr)) {
                    return std::nullopt;
                }
                return property(inst->body, strength);
            }
            case AssertionExprKind::Clocking: {
                // `property p; @(posedge clk) a |=> b; endproperty`. The event
                // has to be stepped over at THIS layer: falling through to the
                // sequence tail would take the implication under it into
                // sequence(), where an implication is not an operator at all.
                const auto& wrapper = expr.as<slang::ast::ClockingAssertionExpr>();
                if (!is_assertion_clock(wrapper)) {
                    // TODO: multiclock properties (LRM 16.13).
                    return reject("multiclock sequences are not supported yet", &expr);
                }
                return property(wrapper.expr, strength);
            }
            case AssertionExprKind::DisableIff: {
                // `property p; disable iff (rst) a |=> b; endproperty`, hoisted by
                // lower_concurrent_assertion. Stepped over at THIS layer for the
                // same reason the clocking wrapper above is: the property under it
                // is a property, and the sequence tail could not lower one.
                const auto& wrapper = expr.as<slang::ast::DisableIffAssertionExpr>();
                if (!is_assertion_disable(wrapper)) {
                    // Either a second clause, or one somewhere the caller does not
                    // hoist from -- under `and`, `until`, an abort. Both are LRM
                    // 16.12 violations that slang diagnoses first
                    // (diag::NestedDisableIff); this stays behind it because
                    // absorbing one would reset an operand the user scoped it to,
                    // or the whole assertion the user did not.
                    return reject("only one `disable iff` is allowed, at the top of the "
                                  "assertion or of the named property it instantiates",
                                  &expr);
                }
                return property(wrapper.expr, strength);
            }
            case AssertionExprKind::Binary: {
                const auto& binary = expr.as<slang::ast::BinaryAssertionExpr>();
                switch (binary.op) {
                    case BinaryAssertionOperator::OverlappedImplication:
                    case BinaryAssertionOperator::NonOverlappedImplication:
                        return implication(binary, strength, expr);
                    case BinaryAssertionOperator::OverlappedFollowedBy:
                    case BinaryAssertionOperator::NonOverlappedFollowedBy:
                        return followed_by(binary, strength, expr);
                    case BinaryAssertionOperator::And:
                    case BinaryAssertionOperator::Or:
                        // Spelled with the same token as the sequence-layer
                        // operators of LRM 16.9, and slang builds ONE node kind
                        // for both layers, so the operator alone cannot say which
                        // this is -- the operands do. Two sequences are the
                        // sequence-layer operator and lower to a single automaton
                        // rather than a junction of two, which is both what 16.12
                        // prescribes for that spelling and the cheaper shape, so
                        // that case falls through to the sequence tail.
                        if (!is_property_expr(binary.left) && !is_property_expr(binary.right)) {
                            break;
                        }
                        return junction(binary, strength, expr);
                    case BinaryAssertionOperator::Iff:
                    case BinaryAssertionOperator::Implies:
                        // TODO: `implies` and `iff` (LRM 16.12.6) -- both reduce to
                        // `not` plus `or`, so they land with `not` in phase 4 of
                        // docs/design/sva-property-layer.md. Note this is the
                        // property operator `implies` and not `|->`, which is
                        // handled above.
                        return reject("the `implies` and `iff` property operators are not "
                                      "supported yet: both reduce to `not`, which needs the "
                                      "operand determinised",
                                      &expr);
                    case BinaryAssertionOperator::Until:
                    case BinaryAssertionOperator::SUntil:
                    case BinaryAssertionOperator::UntilWith:
                    case BinaryAssertionOperator::SUntilWith:
                        // TODO: the `until` family (LRM 16.12.7) -- the obligation
                        // it raises has no bounded length, so it needs the
                        // single-state tracking discipline of phase 3.
                        return reject("the `until` property operators are not supported yet: "
                                      "the obligation they raise is unbounded",
                                      &expr);
                    case BinaryAssertionOperator::Intersect:
                    case BinaryAssertionOperator::Throughout:
                    case BinaryAssertionOperator::Within:
                        // Sequence-layer only: a bare sequence used as a property.
                        break;
                }
                break;
            }
            case AssertionExprKind::Unary:
                return shift(expr.as<slang::ast::UnaryAssertionExpr>(), expr);
            case AssertionExprKind::Conditional:
                return conditional(expr.as<slang::ast::ConditionalAssertionExpr>(), strength,
                                   expr);
            case AssertionExprKind::Case:
                return case_select(expr.as<slang::ast::CaseAssertionExpr>(), strength, expr);
            case AssertionExprKind::Abort:
                return abort_on(expr.as<slang::ast::AbortAssertionExpr>(), strength, expr);
            default:
                break;
        }
        // Anything else is a sequence used as a property.
        const auto seq = sequence(expr);
        if (!seq) {
            return std::nullopt;
        }
        sva_node node = 0;
        if (sva_prop_sequence(ir_, strength, *seq, &node) != SVA_OK) {
            return reject("out of memory building an assertion property", &expr);
        }
        return node;
    }

    // Whether this node is a PROPERTY rather than a sequence used as one, which is
    // what decides the layer for the `and`/`or` spelling shared by LRM 16.9 and
    // 16.12. Only the operators that exist at the property layer alone answer
    // true; a concatenation, a repetition or an `intersect` is a sequence however
    // it is used.
    static bool is_property_expr(const AssertionExpr& expr) {
        switch (expr.kind) {
            case AssertionExprKind::Unary:
            case AssertionExprKind::Abort:
            case AssertionExprKind::Conditional:
            case AssertionExprKind::Case:
            case AssertionExprKind::StrongWeak:
            case AssertionExprKind::DisableIff:
                return true;
            case AssertionExprKind::Clocking:
                return is_property_expr(expr.as<slang::ast::ClockingAssertionExpr>().expr);
            case AssertionExprKind::Simple: {
                const auto& simple = expr.as<slang::ast::SimpleAssertionExpr>();
                const auto* inst = simple.repetition ? nullptr : as_instance(simple.expr);
                return inst != nullptr && is_property_instance(*inst);
            }
            case AssertionExprKind::Binary: {
                const auto& binary = expr.as<slang::ast::BinaryAssertionExpr>();
                switch (binary.op) {
                    case BinaryAssertionOperator::And:
                    case BinaryAssertionOperator::Or:
                        // Shared spelling again: this pair is a property only if
                        // one of ITS operands is.
                        return is_property_expr(binary.left) || is_property_expr(binary.right);
                    case BinaryAssertionOperator::Intersect:
                    case BinaryAssertionOperator::Throughout:
                    case BinaryAssertionOperator::Within:
                        return false;
                    case BinaryAssertionOperator::Iff:
                    case BinaryAssertionOperator::Until:
                    case BinaryAssertionOperator::SUntil:
                    case BinaryAssertionOperator::UntilWith:
                    case BinaryAssertionOperator::SUntilWith:
                    case BinaryAssertionOperator::Implies:
                    case BinaryAssertionOperator::OverlappedImplication:
                    case BinaryAssertionOperator::NonOverlappedImplication:
                    case BinaryAssertionOperator::OverlappedFollowedBy:
                    case BinaryAssertionOperator::NonOverlappedFollowedBy:
                        return true;
                }
                return true;
            }
            case AssertionExprKind::Invalid:
            case AssertionExprKind::SequenceConcat:
            case AssertionExprKind::SequenceWithMatch:
            case AssertionExprKind::FirstMatch:
                return false;
        }
        return false;
    }

    // Whether lowering this property yields a monitor with a TRIGGER: an
    // implication, or a shift over one, which libsva moves onto the antecedent
    // rather than the obligation (Glushkov.zig lowerPropShift). A monitor has one
    // trigger and nowhere to put a second, so a junction, a followed-by's
    // consequent and an abort all refuse an operand carrying one. Mirroring the
    // propagation here is what lets the refusal name the offending operand: libsva
    // reports a node, and clotho has no map from a node back to source.
    static bool carries_trigger(const AssertionExpr& expr) {
        switch (expr.kind) {
            case AssertionExprKind::Binary: {
                const auto& binary = expr.as<slang::ast::BinaryAssertionExpr>();
                return binary.op == BinaryAssertionOperator::OverlappedImplication ||
                       binary.op == BinaryAssertionOperator::NonOverlappedImplication;
            }
            case AssertionExprKind::Unary:
                return carries_trigger(expr.as<slang::ast::UnaryAssertionExpr>().expr);
            case AssertionExprKind::StrongWeak:
                return carries_trigger(expr.as<slang::ast::StrongWeakAssertionExpr>().expr);
            case AssertionExprKind::Clocking:
                return carries_trigger(expr.as<slang::ast::ClockingAssertionExpr>().expr);
            case AssertionExprKind::Simple: {
                const auto& simple = expr.as<slang::ast::SimpleAssertionExpr>();
                const auto* inst = simple.repetition ? nullptr : as_instance(simple.expr);
                if (inst == nullptr || !is_property_instance(*inst)) {
                    return false;
                }
                // An instance clotho refuses outright is not walked: for a
                // recursive one slang hands back an InvalidAssertionExpr
                // placeholder rather than the declaration, and answering from it
                // would let this predicate's refusal pre-empt the accurate one
                // instance_supported is about to give.
                if (inst->isRecursiveProperty || !inst->localVars.empty()) {
                    return false;
                }
                return carries_trigger(inst->body);
            }
            default:
                // A junction, a followed-by, an abort and the guarded selection
                // forms all refuse a trigger-carrying operand, so none of them
                // ever produces one either.
                return false;
        }
    }

    // The strength a unary property operator forces on its subtree, or nullopt for
    // `not`, which is not one of them. `strong`/`weak` wrap sequences and not
    // properties (LRM 16.12.2), so nothing above a shift can override this.
    static std::optional<sva_strength> shift_strength(slang::ast::UnaryAssertionOperator op) {
        switch (op) {
            case slang::ast::UnaryAssertionOperator::NextTime:
            case slang::ast::UnaryAssertionOperator::Always:
            case slang::ast::UnaryAssertionOperator::Eventually:
                return SVA_WEAK;
            case slang::ast::UnaryAssertionOperator::SNextTime:
            case slang::ast::UnaryAssertionOperator::SAlways:
            case slang::ast::UnaryAssertionOperator::SEventually:
                return SVA_STRONG;
            case slang::ast::UnaryAssertionOperator::Not:
                return std::nullopt;
        }
        return std::nullopt;
    }

    // The strength libsva will attach to this operand's plan, given the strength
    // clotho threads into it. Mirrors Glushkov's Plan.strength, which comes from
    // the prop_seq_* LEAF rather than from the operators above it, so the
    // innermost `strong`/`weak` spelling wins. Only sva_prop_junction cares: it
    // refuses a pair whose sides disagree, because a pending pair with one weak
    // and one strong side has no single end-of-trace disposition.
    static sva_strength strength_of(const AssertionExpr& expr, sva_strength inherited) {
        switch (expr.kind) {
            case AssertionExprKind::StrongWeak:
                return expr.as<slang::ast::StrongWeakAssertionExpr>().strength ==
                               slang::ast::StrongWeakAssertionExpr::Strong
                           ? SVA_STRONG
                           : SVA_WEAK;
            case AssertionExprKind::Clocking:
                return strength_of(expr.as<slang::ast::ClockingAssertionExpr>().expr, inherited);
            case AssertionExprKind::Simple: {
                const auto& simple = expr.as<slang::ast::SimpleAssertionExpr>();
                const auto* inst = simple.repetition ? nullptr : as_instance(simple.expr);
                // As carries_trigger: an instance clotho refuses outright has no
                // body worth reading.
                if (inst != nullptr && is_property_instance(*inst) &&
                    !inst->isRecursiveProperty && inst->localVars.empty()) {
                    return strength_of(inst->body, inherited);
                }
                return inherited;
            }
            case AssertionExprKind::Unary: {
                const auto& unary = expr.as<slang::ast::UnaryAssertionExpr>();
                const std::optional<sva_strength> own = shift_strength(unary.op);
                return own ? strength_of(unary.expr, *own) : inherited;
            }
            case AssertionExprKind::Abort:
                return strength_of(expr.as<slang::ast::AbortAssertionExpr>().expr, inherited);
            case AssertionExprKind::Conditional:
                // Both arms are built with the same inherited strength, and the
                // guards are booleans, so either arm answers for the pair.
                return strength_of(expr.as<slang::ast::ConditionalAssertionExpr>().ifExpr,
                                   inherited);
            case AssertionExprKind::Case: {
                const auto& select = expr.as<slang::ast::CaseAssertionExpr>();
                if (!select.items.empty()) {
                    return strength_of(*select.items.front().body, inherited);
                }
                return select.defaultCase != nullptr ? strength_of(*select.defaultCase, inherited)
                                                     : inherited;
            }
            case AssertionExprKind::Binary: {
                const auto& binary = expr.as<slang::ast::BinaryAssertionExpr>();
                switch (binary.op) {
                    case BinaryAssertionOperator::OverlappedImplication:
                    case BinaryAssertionOperator::NonOverlappedImplication:
                    case BinaryAssertionOperator::OverlappedFollowedBy:
                    case BinaryAssertionOperator::NonOverlappedFollowedBy:
                        // The antecedent is a sequence; the consequent carries the
                        // strength.
                        return strength_of(binary.right, inherited);
                    default:
                        return strength_of(binary.left, inherited);
                }
            }
            default:
                return inherited;
        }
    }

    // `s |-> p` and `s |=> p` (LRM 16.12.8). The antecedent is a SEQUENCE and
    // becomes the monitor's trigger; the consequent is the obligation.
    std::optional<sva_node> implication(const slang::ast::BinaryAssertionExpr& binary,
                                        sva_strength strength, const AssertionExpr& blame) {
        const auto antecedent = sequence(binary.left);
        if (!antecedent) {
            return std::nullopt;
        }
        const auto consequent = property(binary.right, strength);
        if (!consequent) {
            return std::nullopt;
        }
        // The antecedent becomes the monitor's one trigger, so a consequent that
        // raises an obligation of its own has nowhere to put its own antecedent.
        // libsva reports this too; saying it here is what keeps the diagnostic
        // naming the shape rather than the mechanism.
        //
        // Checked AFTER the consequent is lowered, so a consequent that fails for
        // a deeper reason -- a recursive instance, a second clock -- reports that
        // reason instead: reject() keeps the first message, and the innermost
        // cause is the useful one.
        if (carries_trigger(binary.right)) {
            return reject("the consequent of an implication is itself an implication",
                          &binary.right);
        }
        sva_node node = 0;
        const sva_implication kind =
            binary.op == BinaryAssertionOperator::OverlappedImplication ? SVA_OVERLAPPING
                                                                       : SVA_NONOVERLAPPING;
        if (sva_prop_implies(ir_, kind, *antecedent, *consequent, &node) != SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        return node;
    }

    // `nexttime`, `always [m:n]`, `eventually [m:n]` and the three strong forms
    // (LRM 16.12.10-16.12.13). All six are ONE construction: the operand shifted
    // over a cycle range, read disjunctively for `nexttime`/`eventually` and
    // conjunctively for `always`. 16.12.13 gives the equivalences directly --
    // `eventually [m:n] p` is `weak(##[m:n] p)` and the `s_` form is the same
    // strong -- which is why the operator's own spelling, and not anything above
    // it, sets the strength of its subtree.
    std::optional<sva_node> shift(const slang::ast::UnaryAssertionExpr& unary,
                                  const AssertionExpr& blame) {
        using Op = slang::ast::UnaryAssertionOperator;
        if (unary.op == Op::Not) {
            // TODO: `not` (LRM 16.12.9) -- a complement needs the operand
            // determinised and it swaps the weak/strong disposition, so it lands
            // in phase 4 of docs/design/sva-property-layer.md with `implies` and
            // `iff`.
            return reject("the `not` property operator is not supported yet: a complement "
                          "needs the operand determinised",
                          &blame);
        }
        const std::optional<sva_strength> inner = shift_strength(unary.op);
        const bool conjunctive = unary.op == Op::Always || unary.op == Op::SAlways;
        const bool nexttime = unary.op == Op::NextTime || unary.op == Op::SNextTime;

        // `nexttime p` is `nexttime[1] p` (LRM 16.12.10): one cycle, not zero. For
        // `always` and `eventually` an absent range is the UNBOUNDED form, and so
        // is a range whose upper bound is `$`.
        uint32_t lo = 1;
        uint32_t hi = 1;
        if (unary.range) {
            lo = unary.range->min;
            if (!unary.range->max) {
                return reject(unbounded_message(conjunctive), &blame);
            }
            hi = *unary.range->max;
        } else if (!nexttime) {
            return reject(unbounded_message(conjunctive), &blame);
        }
        if (lo > hi) {
            // slang refuses an inverted range itself; sva_prop_shift asserts on
            // one rather than reporting it, so this is the backstop.
            return reject("property range lower bound exceeds its upper bound", &blame);
        }

        const auto operand = property(unary.expr, *inner);
        if (!operand) {
            return std::nullopt;
        }
        // libsva moves a shift over an implication onto the ANTECEDENT, which is
        // the only correct place for it -- `nexttime[n] (s |-> p)` obliges `p` for
        // exactly those matches of `s` starting n cycles in. A disjunctive RANGE
        // does not survive that move: `eventually [m:n] (s |-> p)` is a
        // disjunction of implications, each vacuously true on its own, and no
        // single trigger says that (Glushkov.zig lowerPropShift). Checked after
        // lowering, so a deeper failure in the operand reports itself first.
        if (lo != hi && !conjunctive && carries_trigger(unary.expr)) {
            return reject("`eventually [m:n]` over an implication is not supported: it is a "
                          "disjunction of implications, each holding vacuously on its own, "
                          "which one trigger cannot express",
                          &blame);
        }
        sva_node node = 0;
        const sva_shift kind = conjunctive ? SVA_SHIFT_EVERY : SVA_SHIFT_SOME;
        if (sva_prop_shift(ir_, kind, lo, hi, *operand, &node) != SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        return node;
    }

    // Unbounded `always` / `s_eventually`. libsva builds these and refuses them in
    // sva_lower, but the message it has for that names the obligation rather than
    // the operator, so clotho refuses first and says which operator it was.
    static std::string unbounded_message(bool conjunctive) {
        // TODO: unbounded `always` / `eventually` and the `until` family (LRM
        // 16.12.11-16.12.13) -- an obligation with no bounded length needs the
        // single-state tracking discipline of phase 3 in
        // docs/design/sva-property-layer.md, because the number of attempts
        // pending at once is not bounded and no fixed pool tracks them.
        return std::string("an unbounded `") + (conjunctive ? "always" : "eventually") +
               "` is not supported yet: the obligation it raises has no bounded length, so "
               "the number of attempts pending at once is unbounded. Give it a bounded range "
               "to lower it today";
    }

    // `s #-# p` and `s #=# p` (LRM 16.12.9). The existential dual of implication:
    // an antecedent that never matches makes an implication hold vacuously and
    // makes this FAIL, which is exactly what makes it the right shape for the
    // guarded selection forms below.
    std::optional<sva_node> followed_by(const slang::ast::BinaryAssertionExpr& binary,
                                        sva_strength strength, const AssertionExpr& blame) {
        const auto antecedent = sequence(binary.left);
        if (!antecedent) {
            return std::nullopt;
        }
        const auto consequent = property(binary.right, strength);
        if (!consequent) {
            return std::nullopt;
        }
        // libsva fuses the antecedent onto the consequent's automaton rather than
        // making a trigger of it, so the consequent's states do not survive as a
        // block and a trigger of its own has nowhere to go. Checked after lowering,
        // so a deeper failure in the consequent reports itself first.
        if (carries_trigger(binary.right)) {
            return reject("the consequent of a followed-by is an implication, which raises an "
                          "obligation of its own, and a monitor has only one trigger",
                          &binary.right);
        }
        sva_node node = 0;
        const sva_implication kind =
            binary.op == BinaryAssertionOperator::OverlappedFollowedBy ? SVA_OVERLAPPING
                                                                       : SVA_NONOVERLAPPING;
        if (sva_prop_followed_by(ir_, kind, *antecedent, *consequent, &node) != SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        return node;
    }

    // Property-layer `and` / `or` (LRM 16.12.5-16.12.6), reached only when an
    // operand is a property; two sequences take the sequence-layer operator.
    std::optional<sva_node> junction(const slang::ast::BinaryAssertionExpr& binary,
                                     sva_strength strength, const AssertionExpr& blame) {
        const auto lhs = property(binary.left, strength);
        if (!lhs) {
            return std::nullopt;
        }
        const auto rhs = property(binary.right, strength);
        if (!rhs) {
            return std::nullopt;
        }
        // A junction combines two obligations and neither side keeps a trigger of
        // its own, so libsva refuses one rather than picking which attempt it
        // belongs to. Both checks run after lowering, so a deeper failure in either
        // operand reports itself first.
        for (const AssertionExpr* side : { &binary.left, &binary.right }) {
            if (carries_trigger(*side)) {
                return reject("an operand of a property `and`/`or` is an implication, which "
                              "raises an obligation of its own, and a monitor has only one "
                              "trigger",
                              side);
            }
        }
        // The pair is pending until both sides resolve, and a pending pair with
        // one weak and one strong side has no single end-of-trace disposition.
        if (strength_of(binary.left, strength) != strength_of(binary.right, strength)) {
            return reject("the two operands of a property `and`/`or` have different strength, "
                          "so the pair has no single end-of-trace disposition",
                          &blame);
        }
        sva_node node = 0;
        const sva_junction kind =
            binary.op == BinaryAssertionOperator::And ? SVA_CONJUNCTION : SVA_DISJUNCTION;
        if (sva_prop_junction(ir_, kind, *lhs, *rhs, &node) != SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        return node;
    }

    // `accept_on` / `reject_on` and their `sync_` forms (LRM 16.12.7), all four
    // one node kind with `action` and `isSync` picking between them.
    //
    // Unlike a junction or a followed-by, this one does NOT refuse a
    // trigger-carrying operand: an abort above an implication is the shape
    // essentially every real use writes, and pushing it below is not an
    // equivalent rewrite -- below the implication the abort only arms once the
    // consequent starts, so it misses the attempt's own cycle. libsva carries the
    // scope instead (sva_monitor_abort_covers_trigger) and refuses the residue it
    // cannot get right, a trigger whose antecedent takes more than one cycle,
    // with SVA_ERROR_UNSUPPORTED_ABORT_SCOPE.
    std::optional<sva_node> abort_on(const slang::ast::AbortAssertionExpr& node,
                                     sva_strength strength, const AssertionExpr& blame) {
        const bool accept = node.action == slang::ast::AbortAssertionExpr::Accept;
        // The plain forms read the condition from live values and the `sync_`
        // ones from the Preponed sample, which is what the kind tells libsva --
        // it cannot check that the caller honours it, so the split is made here
        // and nowhere else.
        const sva_abort kind = accept ? (node.isSync ? SVA_ABORT_SYNC_ACCEPT : SVA_ABORT_ACCEPT)
                                      : (node.isSync ? SVA_ABORT_SYNC_REJECT : SVA_ABORT_REJECT);

        const auto cond = boolean(node.condition, false, &blame);
        if (!cond) {
            return std::nullopt;
        }
        const auto operand = property(node.expr, strength);
        if (!operand) {
            return std::nullopt;
        }
        sva_node out = 0;
        if (sva_prop_abort(ir_, kind, *cond, *operand, &out) != SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        return out;
    }

    // -- Guarded selection (LRM 16.12.16-16.12.17) ---------------------------
    //
    // `if (c) p else q` and the `case` form need no operator of their own, and the
    // shape that expresses them is a disjunction of FOLLOWED-BYS rather than of
    // implications. Exactly one guard holds, so `(c #-# p) or (!c #-# q)` is it:
    // the disjunct whose guard fails has no antecedent match, and a followed-by
    // without a witness fails rather than holding vacuously, so it contributes
    // nothing and the taken branch alone decides the verdict. With implications
    // there instead, the untaken branch would hold vacuously and the disjunction
    // would always pass.

    // One guard boolean as a one-cycle sequence, which is what a followed-by wants
    // on its left.
    std::optional<sva_node> guard_sequence(sva_node guard, const AssertionExpr& blame) {
        sva_node node = 0;
        if (sva_seq_boolean(ir_, guard, &node) != SVA_OK) {
            return reject("out of memory building an assertion sequence", &blame);
        }
        return node;
    }

    // `guard #-# arm`: the arm, obliged from the guard's own cycle.
    std::optional<sva_node> guarded_arm(sva_node guard, const AssertionExpr& arm,
                                        sva_strength strength, const AssertionExpr& blame) {
        const auto antecedent = guard_sequence(guard, blame);
        if (!antecedent) {
            return std::nullopt;
        }
        const auto consequent = property(arm, strength);
        if (!consequent) {
            return std::nullopt;
        }
        // As a written-out followed-by: the arm becomes part of the obligation, so
        // a trigger of its own has nowhere to go. After lowering, so a deeper
        // failure in the arm reports itself first.
        if (carries_trigger(arm)) {
            return reject("a branch of an `if` or `case` property is an implication, which "
                          "raises an obligation of its own, and a monitor has only one trigger",
                          &arm);
        }
        sva_node node = 0;
        if (sva_prop_followed_by(ir_, SVA_OVERLAPPING, *antecedent, *consequent, &node) !=
            SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        return node;
    }

    // `guard #-# 1'b1`: the arm an unwritten `else` stands for. LRM 16.12.16 makes
    // an `if` with no `else` hold when the condition is false, and 16.12.17 does
    // the same for a `case` in which no item matches and no default is given, so
    // both are the true property under the negated guard rather than an absence.
    std::optional<sva_node> guarded_true(sva_node guard, sva_strength strength,
                                         const AssertionExpr& blame) {
        const auto antecedent = guard_sequence(guard, blame);
        if (!antecedent) {
            return std::nullopt;
        }
        const auto body = one_cycle_true(blame);
        if (!body) {
            return std::nullopt;
        }
        sva_node arm = 0;
        if (sva_prop_sequence(ir_, strength, *body, &arm) != SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        sva_node node = 0;
        if (sva_prop_followed_by(ir_, SVA_OVERLAPPING, *antecedent, arm, &node) != SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        return node;
    }

    std::optional<sva_node> disjoin(sva_node lhs, sva_node rhs, const AssertionExpr& blame) {
        sva_node node = 0;
        if (sva_prop_junction(ir_, SVA_DISJUNCTION, lhs, rhs, &node) != SVA_OK) {
            return reject("out of memory building an assertion property", &blame);
        }
        return node;
    }

    // `if (c) p [else q]` (LRM 16.12.16).
    std::optional<sva_node> conditional(const slang::ast::ConditionalAssertionExpr& cond,
                                        sva_strength strength, const AssertionExpr& blame) {
        // Both guards come from boolean(), which puts them in negation normal
        // form, so the negated one is an atom over the negated condition rather
        // than a libsva bnot.
        const auto taken_guard = boolean(cond.condition, false, &blame);
        if (!taken_guard) {
            return std::nullopt;
        }
        const auto taken = guarded_arm(*taken_guard, cond.ifExpr, strength, blame);
        if (!taken) {
            return std::nullopt;
        }
        const auto else_guard = boolean(cond.condition, true, &blame);
        if (!else_guard) {
            return std::nullopt;
        }
        const auto otherwise = cond.elseExpr != nullptr
                                   ? guarded_arm(*else_guard, *cond.elseExpr, strength, blame)
                                   : guarded_true(*else_guard, strength, blame);
        if (!otherwise) {
            return std::nullopt;
        }
        return disjoin(*taken, *otherwise, blame);
    }

    // `case (e) i0: p0; i1: p1; ... [default: pd] endcase` (LRM 16.12.17).
    //
    // The LRM's linear search stops at the first matching item, so an item's guard
    // is its own labels matching AND no earlier item's matching. Duplicate labels
    // across items are legal SystemVerilog, so that priority is not academic.
    std::optional<sva_node> case_select(const slang::ast::CaseAssertionExpr& select,
                                        sva_strength strength, const AssertionExpr& blame) {
        // `earlier` accumulates "no item before this one matched", which is also
        // the default arm's guard once every item has been folded in.
        std::optional<sva_node> earlier;
        std::optional<sva_node> disjunction;

        for (const auto& item : select.items) {
            // This item's own labels: a disjunction of comparisons against the
            // case expression, and its negation a conjunction of the negated ones.
            std::optional<sva_node> matches;
            std::optional<sva_node> misses;
            for (const Expression* label : item.expressions) {
                const auto hit = match_atom(select.expr, *label, false, &blame);
                const auto miss = match_atom(select.expr, *label, true, &blame);
                if (!hit || !miss) {
                    return std::nullopt;
                }
                if (!matches) {
                    matches = hit;
                    misses = miss;
                    continue;
                }
                sva_node any = 0;
                sva_node none = 0;
                if (sva_bool_or(ir_, *matches, *hit, &any) != SVA_OK ||
                    sva_bool_and(ir_, *misses, *miss, &none) != SVA_OK) {
                    return reject("out of memory building an assertion guard", &blame);
                }
                matches = any;
                misses = none;
            }
            if (!matches) {
                // A case item with no labels is not legal SystemVerilog and slang
                // refuses it; nothing to guard on if one ever arrived.
                return reject("a `case` property item has no labels", &blame);
            }

            // The first-match guard: this item's labels, and no earlier item's.
            sva_node guard = *matches;
            if (earlier) {
                if (sva_bool_and(ir_, *earlier, *matches, &guard) != SVA_OK) {
                    return reject("out of memory building an assertion guard", &blame);
                }
            }
            const auto arm = guarded_arm(guard, *item.body, strength, blame);
            if (!arm) {
                return std::nullopt;
            }
            if (!disjunction) {
                disjunction = arm;
            } else {
                const auto joined = disjoin(*disjunction, *arm, blame);
                if (!joined) {
                    return std::nullopt;
                }
                disjunction = joined;
            }

            if (!earlier) {
                earlier = misses;
            } else {
                sva_node none = 0;
                if (sva_bool_and(ir_, *earlier, *misses, &none) != SVA_OK) {
                    return reject("out of memory building an assertion guard", &blame);
                }
                earlier = none;
            }
        }

        if (!earlier || !disjunction) {
            // `case (e) endcase` with no items at all: slang refuses it.
            return reject("a `case` property has no items", &blame);
        }

        // The default arm, or the vacuous truth 16.12.17 gives when no item
        // matches and none is written.
        const auto fallback = select.defaultCase != nullptr
                                  ? guarded_arm(*earlier, *select.defaultCase, strength, blame)
                                  : guarded_true(*earlier, strength, blame);
        if (!fallback) {
            return std::nullopt;
        }
        return disjoin(*disjunction, *fallback, blame);
    }

    // -- Atoms ---------------------------------------------------------------

    // Identity is by (canonical expression, compared-against expression,
    // polarity). A plain signal reference keys on its symbol so the same signal
    // mentioned twice is one atom; anything else keys on node identity.
    // Duplicates cost a wider valuation and a coarser cube, never a verdict. See
    // concurrent-assertions.md 5.5.
    std::optional<sva_node> atom(const Expression& expr, bool negated,
                                 const AssertionExpr* blame) {
        return atom(expr, nullptr, negated, blame);
    }

    // `expr === match` as one atom, for a `case` property item guard. The
    // comparison has no Expression of its own to key on (see sva::Atom), so both
    // halves go into the key.
    std::optional<sva_node> match_atom(const Expression& expr, const Expression& match,
                                       bool negated, const AssertionExpr* blame) {
        return atom(expr, &match, negated, blame);
    }

    std::optional<sva_node> atom(const Expression& expr, const Expression* match, bool negated,
                                 const AssertionExpr* blame) {
        const void* key = &expr;
        if (expr.kind == ExpressionKind::NamedValue) {
            key = &expr.as<slang::ast::NamedValueExpression>().symbol;
        } else if (expr.kind == ExpressionKind::HierarchicalValue) {
            key = &expr.as<slang::ast::HierarchicalValueExpression>().symbol;
        }

        const auto slot = atoms_.try_emplace({ key, match, negated },
                                             static_cast<uint32_t>(out_.atoms.size()));
        if (slot.second) {
            out_.atoms.push_back(Atom{ &expr, match, negated });
        }
        sva_node node = 0;
        if (sva_bool_atom(ir_, slot.first->second, &node) != SVA_OK) {
            return reject("out of memory building an assertion atom", blame);
        }
        return node;
    }

    std::optional<sva_node> one_cycle_true(const AssertionExpr& blame) {
        sva_node always = 0;
        sva_node node = 0;
        if (sva_bool_const(ir_, true, &always) != SVA_OK ||
            sva_seq_boolean(ir_, always, &node) != SVA_OK) {
            return reject("out of memory building an assertion sequence", &blame);
        }
        return node;
    }

private:
    // Owned. libsva allocates it; the destructor hands it back, and that is
    // null-tolerant, so a failed create needs no special case here.
    sva_ir* ir_ = nullptr;
    Result& out_;
    // The assertion's clocking event, so a nested one written where the user
    // wrote it can be told from a second clock. Never null in practice; the
    // caller has already refused an assertion with no clock at all.
    const slang::ast::TimingControl* clock_ = nullptr;
    // The `disable iff` condition the caller hoisted, so the node it came from can
    // be told from a second clause. Null when the assertion never disables.
    const Expression* disable_ = nullptr;
    ankerl::unordered_dense::map<std::tuple<const void*, const void*, bool>, uint32_t> atoms_;
};

// Copy one automaton's tables out of the monitor.
void read_automaton(const sva_monitor* monitor, const sva_automaton* from,
                    ir::AssertionAutomaton& into) {
    into.state_count = sva_automaton_state_count(from);
    into.words = sva_automaton_words(from);
    const uint32_t guards = sva_monitor_guard_count(monitor);

    into.guard_of.assign(sva_automaton_guards(from),
                         sva_automaton_guards(from) + into.state_count);
    into.first.assign(sva_automaton_first(from), sva_automaton_first(from) + into.words);
    into.last.assign(sva_automaton_last(from), sva_automaton_last(from) + into.words);

    into.follow.resize(static_cast<size_t>(into.state_count) * into.words);
    for (uint32_t state = 0; state < into.state_count; state++) {
        const uint64_t* row = sva_automaton_follow(from, state);
        std::memcpy(into.follow.data() + static_cast<size_t>(state) * into.words, row,
                    static_cast<size_t>(into.words) * sizeof(uint64_t));
    }

    into.guard_states.resize(static_cast<size_t>(guards) * into.words);
    for (uint32_t guard = 0; guard < guards; guard++) {
        const uint64_t* row = sva_automaton_guard_states(from, guard);
        std::memcpy(into.guard_states.data() + static_cast<size_t>(guard) * into.words, row,
                    static_cast<size_t>(into.words) * sizeof(uint64_t));
    }
}

// Rebuild the boolean expression of each root as a flat forest the caller can
// walk to emit its per-cycle evaluation. This is the other half of the atom
// contract: libsva chose which boolean nodes are roots, and only clotho knows
// what an atom means.
uint32_t read_root(const sva_ir* ir, sva_node node, Result& out) {
    sva_bool_view view{};
    sva_bool_read(ir, node, &view);

    Result::RootNode built{};
    switch (view.op) {
        case SVA_BOOL_CONST:
            built.op = Result::RootNode::Op::Const;
            built.lhs = view.lhs;
            break;
        case SVA_BOOL_ATOM:
            built.op = Result::RootNode::Op::Atom;
            built.lhs = view.lhs;
            break;
        case SVA_BOOL_NOT:
            built.op = Result::RootNode::Op::Not;
            built.lhs = read_root(ir, view.lhs, out);
            break;
        case SVA_BOOL_AND:
        case SVA_BOOL_OR:
            built.op = view.op == SVA_BOOL_AND ? Result::RootNode::Op::And
                                               : Result::RootNode::Op::Or;
            built.lhs = read_root(ir, view.lhs, out);
            built.rhs = read_root(ir, view.rhs, out);
            break;
    }
    out.nodes.push_back(built);
    return static_cast<uint32_t>(out.nodes.size() - 1);
}

// Peel the wrappers that do not change which operator is outermost: an explicit
// strength, and a named sequence or property, whose body slang has already
// expanded in place.
const AssertionExpr& outermost(const AssertionExpr& expr) {
    if (expr.kind == AssertionExprKind::StrongWeak) {
        return outermost(expr.as<slang::ast::StrongWeakAssertionExpr>().expr);
    }
    if (expr.kind == AssertionExprKind::Simple) {
        const auto& simple = expr.as<slang::ast::SimpleAssertionExpr>();
        if (!simple.repetition) {
            if (const auto* inst = Lowering::as_instance(simple.expr)) {
                return outermost(inst->body);
            }
        }
    }
    return expr;
}

// Whether the consequent of `property` is a sequence `and` at its top level.
//
// An `and` runs to the later of its two ends, so it is bounded only when BOTH
// operands are, while an `intersect` in the same place is bounded as soon as
// either one is -- an intersection's ends coincide, so the bounded side bounds
// the pair. That asymmetry is the whole reason `req |=> (ack ##[1:$] done) and
// rdy` is refused where the same shape under `intersect` is accepted, and no
// user guesses it from "no bounded longest match" alone.
//
// Only a top-level `and` earns the explanation, because there its bound IS the
// obligation's. Deeper, whatever encloses it sets the bound and naming `and`
// would point at the wrong operator.
bool consequent_is_and(const AssertionExpr& property) {
    const AssertionExpr& top = outermost(property);
    if (top.kind != AssertionExprKind::Binary) {
        return false;
    }
    const auto& binary = top.as<slang::ast::BinaryAssertionExpr>();
    if (binary.op != BinaryAssertionOperator::OverlappedImplication &&
        binary.op != BinaryAssertionOperator::NonOverlappedImplication) {
        return false;
    }
    const AssertionExpr& consequent = outermost(binary.right);
    return consequent.kind == AssertionExprKind::Binary &&
           consequent.as<slang::ast::BinaryAssertionExpr>().op == BinaryAssertionOperator::And;
}

} // namespace

bool available() {
    return true;
}

Result lower(const Request& request) {
    check_libsva_abi();

    Result out;
    Lowering lowering(out, request.clock, request.disable);
    if (lowering.ir() == nullptr) {
        out.error = "out of memory building an assertion";
        return out;
    }

    const auto property = lowering.property(*request.property);
    if (!property) {
        return out;
    }

    // The disable-iff condition is a root like any other. LRM 16.15 says it is
    // NOT sampled, so the caller evaluates this one from live values; an
    // assertion that never disables carries a constant false, which costs the
    // backend one fold and the monitor one branch fewer.
    sva_node disable = 0;
    if (request.disable != nullptr) {
        const auto built = lowering.boolean(*request.disable, false, nullptr);
        if (!built) {
            return out;
        }
        disable = *built;
    } else if (sva_bool_const(lowering.ir(), false, &disable) != SVA_OK) {
        out.error = "out of memory building an assertion";
        return out;
    }

    sva_directive directive{};
    directive.id = 0;
    directive.kind = static_cast<sva_kind>(request.kind);
    directive.clock = SVA_CLOCK_NONE;
    directive.reset = disable;
    directive.property = *property;

    uint32_t index = 0;
    if (sva_ir_add(lowering.ir(), &directive, &index) != SVA_OK) {
        out.error = "out of memory building an assertion";
        return out;
    }

    sva_monitor* monitor = nullptr;
    sva_diagnostic reason{};
    const sva_status status =
        sva_lower(&monitor, lowering.ir(), index, nullptr, &reason);
    if (status != SVA_OK) {
        switch (status) {
            case SVA_ERROR_UNBOUNDED_OBLIGATION:
                out.error = "the consequent has no bounded longest match, so the number "
                            "of concurrent obligations is unbounded";
                if (consequent_is_and(*request.property)) {
                    // The one shape whose rule a user cannot infer from the
                    // sentence above; see consequent_is_and.
                    out.error += ": an `and` matches to the later of its two ends, so BOTH "
                                 "operands need a bounded match -- unlike `intersect`, whose "
                                 "ends coincide, where one bounded operand bounds the pair";
                }
                break;
            case SVA_ERROR_UNSUPPORTED_CONSEQUENT:
                // Reached when a property operator's operand raises an obligation
                // of its own. property() pre-checks the shapes it can name --
                // see carries_trigger -- so this is the backstop for the rest.
                out.error = "an operand of this property operator is an implication, which "
                            "raises an obligation of its own, and a monitor has only one "
                            "trigger";
                break;
            case SVA_ERROR_MIXED_STRENGTH:
                out.error = "the two operands of a property `and`/`or` have different "
                            "strength, so the pair has no single end-of-trace disposition";
                break;
            case SVA_ERROR_UNSUPPORTED_ABORT_SCOPE:
                // Two shapes share this code. The second is the residue of the
                // abort-above-an-implication support: with a multi-cycle
                // antecedent, several attempts can sit part-way through it when
                // the condition fires, each owing its own verdict, and the
                // trigger is their union rather than a per-attempt ring -- so
                // the count cannot be recovered. A one-cycle antecedent has no
                // partial match to lose and is exact.
                // TODO: a multi-cycle antecedent under an abort needs the
                // trigger tracked per attempt; see phase 2 of
                // docs/design/sva-property-layer.md.
                out.error = "an `accept_on` / `reject_on` sits below an operator that "
                            "renumbers its operand's states -- a property `and`, an "
                            "`always [m:n]` or a followed-by -- so the states the abort "
                            "names cannot be carried through, or it covers an implication "
                            "whose antecedent takes more than one cycle to match, whose "
                            "part-matched attempts cannot be told apart";
                break;
            case SVA_ERROR_STATE_LIMIT:
                out.error = "the assertion needs more automaton states than the limit allows";
                break;
            case SVA_ERROR_DEPTH_LIMIT:
                out.error = "the assertion allows more concurrent obligations than the "
                            "limit allows";
                break;
            case SVA_ERROR_UNSUPPORTED_REPEAT_OPERAND:
                out.error = "goto or non-consecutive repetition over a compound sequence";
                break;
            default:
                out.error = "the assertion could not be lowered";
                break;
        }
        return out;
    }

    ir::AssertionMonitor& built = out.monitor;
    built.kind = static_cast<ir::AssertionKind>(sva_monitor_kind(monitor));
    built.strength = sva_monitor_strength(monitor) == SVA_STRONG ? ir::AssertionStrength::Strong
                                                                 : ir::AssertionStrength::Weak;
    built.depth = sva_monitor_depth(monitor);
    built.reset_root = sva_monitor_reset(monitor);
    built.overlap = sva_monitor_implication(monitor) == SVA_OVERLAPPING;
    built.has_trigger = sva_monitor_trigger(monitor) != nullptr;

    const uint32_t guards = sva_monitor_guard_count(monitor);
    built.cubes.resize(guards);
    for (uint32_t guard = 0; guard < guards; guard++) {
        uint32_t count = 0;
        const sva_literal* literals = sva_monitor_guard(monitor, guard, &count);
        built.cubes[guard] = ir::AssertionCube{
            .start = static_cast<uint32_t>(built.literals.size()),
            .len = count,
        };
        for (uint32_t i = 0; i < count; i++) {
            built.literals.push_back(ir::AssertionLiteral{
                .root = SVA_LITERAL_ROOT(literals[i]),
                .negated = SVA_LITERAL_NEGATED(literals[i]) != 0,
            });
        }
    }

    read_automaton(monitor, sva_monitor_obligation(monitor), built.obligation);
    if (built.has_trigger) {
        read_automaton(monitor, sva_monitor_trigger(monitor), built.trigger);
    }

    // The scoped aborts. After the obligation, whose `words` sizes each mask.
    const uint32_t aborts = sva_monitor_abort_count(monitor);
    built.aborts.reserve(aborts);
    for (uint32_t i = 0; i < aborts; i++) {
        // Never an SVA_ABORT_SYNC_* value here -- whether the condition is
        // sampled is reported separately, so the effect is just the verdict.
        const bool accept = sva_monitor_abort_effect(monitor, i) == SVA_ABORT_ACCEPT;
        const uint64_t* mask = sva_monitor_abort_mask(monitor, i);
        built.aborts.push_back(ir::AssertionAbort{
            .root = sva_monitor_abort_root(monitor, i),
            .effect = accept ? ir::AssertionAbortEffect::Accept : ir::AssertionAbortEffect::Reject,
            .sampled = sva_monitor_abort_sampled(monitor, i),
            .resolves_attempt = sva_monitor_abort_resolves_attempt(monitor, i),
            .covers_trigger = sva_monitor_abort_covers_trigger(monitor, i),
            .mask = std::vector<uint64_t>(mask, mask + built.obligation.words),
        });
    }

    // Root expressions, in the monitor's order. The caller emits one 1-bit
    // evaluation per entry and packs the results into the valuation word.
    const uint32_t roots = sva_monitor_root_count(monitor);
    const sva_node* root_nodes = sva_monitor_roots(monitor);
    out.root_index.reserve(roots);
    for (uint32_t root = 0; root < roots; root++) {
        out.root_index.push_back(read_root(lowering.ir(), root_nodes[root], out));
    }

    sva_monitor_destroy(monitor);
    out.ok = true;
    return out;
}

} // namespace clotho::sva

#else // !CLOTHO_HAVE_SVA

namespace clotho::sva {

bool available() {
    return false;
}

Result lower(const Request&) {
    Result out;
    out.error = "this build was configured without concurrent-assertion support "
                "(-DCLOTHO_SVA=OFF)";
    return out;
}

} // namespace clotho::sva

#endif
