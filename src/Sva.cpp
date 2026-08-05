#include "Sva.h"

#ifdef CLOTHO_HAVE_SVA

#include <sva.h>

#include <slang/ast/Expression.h>
#include <slang/ast/expressions/AssertionExpr.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/expressions/OperatorExpressions.h>
#include <slang/ast/symbols/ValueSymbol.h>

#include <ankerl/unordered_dense.h>

#include <cstring>
#include <utility>

namespace clotho::sva {
namespace {

using slang::ast::AssertionExpr;
using slang::ast::AssertionExprKind;
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
                if (simple.repetition) {
                    // TODO: sequence repetition [*] / [=] / [->] (LRM 16.9.2)
                    // needs the range translated and the goto/nonconsecutive
                    // operand checked against libsva's seq_boolean requirement.
                    return reject("sequence repetition is not supported yet", &expr);
                }
                // A named sequence or property instance arrives as an
                // AssertionInstanceExpression nested here; its body is already
                // expanded, but binding local variables is not modelled.
                if (simple.expr.kind == ExpressionKind::AssertionInstance) {
                    // TODO: named sequence / property instantiation (LRM 16.8,
                    // 16.12) -- walk AssertionInstanceExpression::body once
                    // local variables are rejected explicitly.
                    return reject("named sequences and properties are not supported yet",
                                  &expr);
                }
                const auto guard = boolean(simple.expr, false, &expr);
                if (!guard) {
                    return std::nullopt;
                }
                sva_node node = 0;
                if (sva_seq_boolean(&ir_, *guard, &node) != SVA_OK) {
                    return reject("out of memory building an assertion sequence", &expr);
                }
                return node;
            }
            case AssertionExprKind::SequenceConcat:
                return concat(expr.as<slang::ast::SequenceConcatExpr>(), expr);
            case AssertionExprKind::Clocking:
                // A nested clocking event is a multiclock sequence.
                // TODO: multiclock sequences (LRM 16.13).
                return reject("multiclock sequences are not supported yet", &expr);
            default:
                // TODO: or / and / intersect / throughout / within /
                // first_match / with-match-items (LRM 16.9).
                return reject("this sequence operator is not supported yet", &expr);
        }
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
