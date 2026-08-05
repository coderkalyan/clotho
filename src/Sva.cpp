#include "Sva.h"

#ifdef CLOTHO_HAVE_SVA

#include <sva.h>

#include <slang/ast/Expression.h>
#include <slang/ast/expressions/AssertionExpr.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/expressions/OperatorExpressions.h>
#include <slang/ast/symbols/ValueSymbol.h>
#include <slang/ast/types/Type.h>

#include <ankerl/unordered_dense.h>

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

// libsva reports failures against a node; the front-end reports against source.
// This maps one to the other by remembering, per libsva node, the AssertionExpr
// that produced it.
class Lowering {
public:
    explicit Lowering(Result& out) : out_(out) {
        sva_ir_init(&ir_);
    }
    ~Lowering() {
        sva_ir_deinit(&ir_);
    }
    Lowering(const Lowering&) = delete;
    Lowering& operator=(const Lowering&) = delete;

    sva_ir* ir() {
        return &ir_;
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
                const sva_status status = want_and ? sva_bool_and(&ir_, *lhs, *rhs, &node)
                                                   : sva_bool_or(&ir_, *lhs, *rhs, &node);
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
                if (sva_seq_boolean(&ir_, *guard, &node) != SVA_OK) {
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
            case AssertionExprKind::Clocking:
                // A nested clocking event is a multiclock sequence.
                // TODO: multiclock sequences (LRM 16.13).
                return reject("multiclock sequences are not supported yet", &expr);
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
                if (build(&ir_, *lhs, *rhs, &node) != SVA_OK) {
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
                const sva_status status = sva_seq_first_match(&ir_, *seq, &node);
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
        if (sva_seq_throughout(&ir_, *guard, *seq, &node) != SVA_OK) {
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
            if (sva_seq_delay(&ir_, *acc, lo, hi, *next, &node) != SVA_OK) {
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
        if (sva_seq_repeat(&ir_, kind, operand, lo, hi, &node) != SVA_OK) {
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
            case AssertionExprKind::DisableIff:
                // Only a `disable iff` at the top of the assertion is peeled, by
                // lower_concurrent_assertion, which is the only place that can
                // put one in the sva::Request. LRM 16.12 also allows it at the
                // top of a `property` declaration, where reaching the request
                // means hoisting it back out through the instantiation.
                // TODO: `disable iff` inside a named property declaration.
                return reject("a `disable iff` inside a named property is not supported yet",
                              &expr);
            case AssertionExprKind::Binary: {
                const auto& binary = expr.as<slang::ast::BinaryAssertionExpr>();
                const bool overlap = binary.op == BinaryAssertionOperator::OverlappedImplication;
                const bool nonoverlap =
                    binary.op == BinaryAssertionOperator::NonOverlappedImplication;
                if (!overlap && !nonoverlap) {
                    // A sequence-layer binary operator in a property position is
                    // a bare sequence property; fall through to the sequence
                    // path so `a ##1 b` as a whole property still works.
                    break;
                }
                const auto antecedent = sequence(binary.left);
                if (!antecedent) {
                    return std::nullopt;
                }
                const auto consequent = property(binary.right, strength);
                if (!consequent) {
                    return std::nullopt;
                }
                sva_node node = 0;
                const sva_implication kind = overlap ? SVA_OVERLAPPING : SVA_NONOVERLAPPING;
                if (sva_prop_implies(&ir_, kind, *antecedent, *consequent, &node) != SVA_OK) {
                    return reject("out of memory building an assertion property", &expr);
                }
                return node;
            }
            case AssertionExprKind::Unary:
            case AssertionExprKind::Abort:
            case AssertionExprKind::Conditional:
            case AssertionExprKind::Case:
                // TODO: property operators not / nexttime / always / eventually /
                // until / implies / iff (LRM 16.12), accept_on / reject_on
                // (16.12.7), and the case / if-else property forms -- libsva has
                // no tag for any of them.
                return reject("this property operator is not supported yet", &expr);
            default:
                break;
        }
        // Anything else is a sequence used as a property.
        const auto seq = sequence(expr);
        if (!seq) {
            return std::nullopt;
        }
        sva_node node = 0;
        if (sva_prop_sequence(&ir_, strength, *seq, &node) != SVA_OK) {
            return reject("out of memory building an assertion property", &expr);
        }
        return node;
    }

    // -- Atoms ---------------------------------------------------------------

    // Identity is by (canonical expression, polarity). A plain signal reference
    // keys on its symbol so the same signal mentioned twice is one atom; anything
    // else keys on node identity. Duplicates cost a wider valuation and a coarser
    // cube, never a verdict. See concurrent-assertions.md 5.5.
    std::optional<sva_node> atom(const Expression& expr, bool negated,
                                 const AssertionExpr* blame) {
        const void* key = &expr;
        if (expr.kind == ExpressionKind::NamedValue) {
            key = &expr.as<slang::ast::NamedValueExpression>().symbol;
        } else if (expr.kind == ExpressionKind::HierarchicalValue) {
            key = &expr.as<slang::ast::HierarchicalValueExpression>().symbol;
        }

        const auto slot = atoms_.try_emplace({ key, negated },
                                             static_cast<uint32_t>(out_.atoms.size()));
        if (slot.second) {
            out_.atoms.push_back(Atom{ &expr, negated });
        }
        sva_node node = 0;
        if (sva_bool_atom(&ir_, slot.first->second, &node) != SVA_OK) {
            return reject("out of memory building an assertion atom", blame);
        }
        return node;
    }

    std::optional<sva_node> one_cycle_true(const AssertionExpr& blame) {
        sva_node always = 0;
        sva_node node = 0;
        if (sva_bool_const(&ir_, true, &always) != SVA_OK ||
            sva_seq_boolean(&ir_, always, &node) != SVA_OK) {
            return reject("out of memory building an assertion sequence", &blame);
        }
        return node;
    }

private:
    sva_ir ir_{};
    Result& out_;
    ankerl::unordered_dense::map<std::pair<const void*, bool>, uint32_t> atoms_;
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
    Result out;
    Lowering lowering(out);

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

    sva_monitor monitor{};
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
                out.error = "the consequent of an implication is itself an implication";
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
    built.kind = static_cast<ir::AssertionKind>(sva_monitor_kind(&monitor));
    built.strength = sva_monitor_strength(&monitor) == SVA_STRONG ? ir::AssertionStrength::Strong
                                                                 : ir::AssertionStrength::Weak;
    built.depth = sva_monitor_depth(&monitor);
    built.reset_root = sva_monitor_reset(&monitor);
    built.overlap = sva_monitor_implication(&monitor) == SVA_OVERLAPPING;
    built.has_trigger = sva_monitor_trigger(&monitor) != nullptr;

    const uint32_t guards = sva_monitor_guard_count(&monitor);
    built.cubes.resize(guards);
    for (uint32_t guard = 0; guard < guards; guard++) {
        uint32_t count = 0;
        const sva_literal* literals = sva_monitor_guard(&monitor, guard, &count);
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

    read_automaton(&monitor, sva_monitor_obligation(&monitor), built.obligation);
    if (built.has_trigger) {
        read_automaton(&monitor, sva_monitor_trigger(&monitor), built.trigger);
    }

    // Root expressions, in the monitor's order. The caller emits one 1-bit
    // evaluation per entry and packs the results into the valuation word.
    const uint32_t roots = sva_monitor_root_count(&monitor);
    const sva_node* root_nodes = sva_monitor_roots(&monitor);
    out.root_index.reserve(roots);
    for (uint32_t root = 0; root < roots; root++) {
        out.root_index.push_back(read_root(lowering.ir(), root_nodes[root], out));
    }

    sva_monitor_deinit(&monitor);
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
