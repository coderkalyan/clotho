// Concurrent assertions (LRM 16): the libsva boundary.
//
// The front-end walks slang's AssertionExpr tree into an SVA IR through libsva's
// C API, lowers it to an automaton pair, and copies the tables out into an
// ir::AssertionMonitor. This header is the only place libsva's header is
// included, so the dependency is contained and `ir::AssertionMonitor` stays plain
// data that clotho_ir_core and the backend can consume without it.
//
// See docs/design/concurrent-assertions.md.

#pragma once

#include "Diagnostic.h"
#include "ir/Module.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace slang::ast {
class AssertionExpr;
class Expression;
class TimingControl;
} // namespace slang::ast

namespace clotho::sva {

// Whether this build has the temporal engine at all. Concurrent assertions fail
// loudly either way when they cannot be lowered; this only distinguishes "not
// implemented yet" from "not compiled in".
bool available();

// One boolean leaf the front-end must evaluate. `expr` is the SystemVerilog
// expression, already in the polarity the guard needs: negation-normal-form
// lowering pushes `!` down to the leaves, so a negated leaf arrives here as the
// negation of its own expression rather than as a flag. See
// concurrent-assertions.md 5.3.
struct Atom {
    const slang::ast::Expression* expr = nullptr;
    // Non-null only for a `case` property item guard (LRM 16.12.16), where the
    // atom is `expr === match` rather than `expr` read as a condition. slang
    // binds the case expression and each item label as separate expressions and
    // never builds the comparison between them, so there is no single Expression
    // to point at and the pair has to travel. `===` and not `==` because 16.12.16
    // defers to the 12.5 case rules, which compare x and z bit for bit.
    const slang::ast::Expression* match = nullptr;
    // True when the atom should be evaluated negated. NNF means the caller builds
    // the negation itself; this records which so the caller can emit it.
    bool negated = false;
};

// What the front-end asks for.
struct Request {
    // The bound property expression, with a clocking wrapper at its top and the
    // disable-iff wrapper already peeled off by the caller.
    const slang::ast::AssertionExpr* property = nullptr;
    // The disable-iff condition, or null when the assertion never disables. Like
    // `clock` it may have been hoisted out of a named property's declaration --
    // LRM 16.12 puts the clause at the top of a property_spec, and a declaration
    // body is one -- in which case the DisableIff node stays where the user wrote
    // it and the walk has to be told which one it may step over. Every other one
    // is a nesting refusal.
    const slang::ast::Expression* disable = nullptr;
    // The assertion's clocking event. A clock written inside a named sequence or
    // property stays in the tree (LRM 16.13.1 lets it flow out to the whole
    // assertion, but only the caller can decide it did), so the walk needs to
    // know which event it may skip. Every other one is a multiclock refusal.
    const slang::ast::TimingControl* clock = nullptr;
    ir::AssertionKind kind = ir::AssertionKind::Assert;
};

// What came back. On failure, `error` holds a message and `blame` the
// sub-expression to point a diagnostic at.
struct Result {
    bool ok = false;
    std::string error;
    // The AssertionExpr (or Expression) that caused the failure, for a source
    // span. Null when the failure is not attributable to one node.
    const slang::ast::AssertionExpr* blame = nullptr;

    // Filled on success. `atoms` is indexed by the atom handle the tables refer
    // to, so the caller emits one 1-bit evaluation per entry, in order.
    std::vector<Atom> atoms;
    // Roots are boolean combinations of atoms. Each entry is a tree the caller
    // walks to emit the per-cycle root evaluation.
    struct RootNode {
        enum class Op : uint8_t { Atom, Const, And, Or, Not } op = Op::Atom;
        // Atom: index into `atoms`. Const: 0 or 1. And/Or: indices into `nodes`.
        // Not: `lhs` is the operand index.
        uint32_t lhs = 0;
        uint32_t rhs = 0;
    };
    // Flattened root expression forest; `root_index[i]` is the entry in `nodes`
    // for root i. Operands always point backwards.
    std::vector<RootNode> nodes;
    std::vector<uint32_t> root_index;

    // Everything the backend needs, minus the parts only the caller can fill
    // (clock, label, span, banner, and `roots` once it has emitted them).
    ir::AssertionMonitor monitor;
};

// Build and lower one concurrent assertion. Never throws; a failure is reported
// through `Result::error` so the caller can turn it into a diagnostic with a
// source location.
Result lower(const Request& request);

} // namespace clotho::sva
