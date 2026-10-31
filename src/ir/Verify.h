#pragma once

// IR verifier: checks a lowered Module against the typing rules of
// docs/design/kernel.md ("Values"). The language decides domains, signedness
// and extension through slang's legalized AST and the IR carries the result (a
// type is a width and a domain; the opcode or instruction data says how to read a
// sign), so an instruction whose operands disagree is a frontend error, not
// something the backend repairs.
//
// verify() only reports; the caller decides whether a finding is fatal. The types
// it checks are the ones the backend derives (Typing.h), so a rule is violated
// exactly when the backend would emit an ill-typed operation.
//
// Like the rest of clotho_ir_core, no LLVM and no slang.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Ir.h"
#include "Module.h"
#include "Typing.h"

namespace clotho::ir {

enum class Rule : uint8_t {
    // -- Binary operators --
    // Add Sub Mul Div Mod Pow Band Bor Bxor Bxnor, Eq..Ge, CaseEq family,
    // WildEq, and the InsideSet selector against each member: an operand is a
    // string or a real where an integer is required (or the reverse for a real
    // operator).
    OperandKind,
    // Both operands have the same width. The backend patches CaseEq / CasezEq /
    // CasexEq / WildEq with a max-width cast; every other operator is ill-typed.
    OperandWidth,
    // Both operands have the same domain. The backend reads one domain for the pair
    // and converts neither side, so a mismatch is an IR error it cannot repair.
    OperandDomain,
    // Sll Srl Sra: a 4-state amount makes the LRM result 4-state, but the IR
    // result is the shifted operand's type. Also: the shifted operand or the
    // amount is not an integer.
    ShiftResult,
    // -- Selection --
    // Select / SelectLazy arms agree in width and domain.
    SelectArmWidth,
    SelectArmDomain,
    // Cond / Select / SelectLazy / Loop predicates are 1 bit wide: the backend
    // compares the value's must plane against a 1-bit zero.
    PredicateWidth,
    // -- Stores --
    // The stored value's width equals the lvalue's. The frontend context-sizes
    // the right-hand side with a conversion, so the backend stores planes directly.
    StoreWidth,
    // The stored value's domain differs from the lvalue's. The backend reads the value
    // in the lvalue's domain and converts nothing. Recorded separately from StoreWidth.
    StoreDomain,
    // -- Conversions --
    // A conversion has the shape its opcode names: ZExt / SExt widen and Trunc
    // narrows, in one domain; ToFour / ToTwo change the domain at one width. An
    // operand or a target that is not an integer is reported too.
    ConvertShape,
    // -- Calls --
    // A call passes a different number of operands than the callee has formals.
    CallArity,
    // An actual's width / domain differs from the formal's.
    CallArgWidth,
    CallArgDomain,
    // An Arg's declared type differs from the function's formal at that position.
    ArgType,
    // A Ret's value width / domain differs from the function's return type.
    RetWidth,
    RetDomain,
};

// Rules that record where the backend decides something implicitly, rather than
// naming ill-typed IR.
constexpr bool is_record(Rule rule) {
    return rule == Rule::StoreDomain;
}

// Stable kebab-case name, for reports and for tests.
const char* rule_name(Rule rule);

// Where an instruction lives: one of the module's processes, or one of its
// functions (tasks and fork children included).
struct Site {
    enum class Kind : uint8_t { Process, Function };
    Kind kind = Kind::Process;
    // Index into Module::processes(), or the FunctionId.
    uint32_t id = 0;
};

struct Violation {
    Rule      rule = Rule::OperandKind;
    Inst::Tag tag  = Inst::Tag::Constant;
    // The offending instruction, in the body `site` names.
    Index inst = kNoIndex;
    Site  site;
    // The types the rule compared: for a binary operator the left and right
    // operand; for a store the value and the lvalue; for a call actual the
    // actual and the formal; for a conversion the source and the target; for
    // a select the two arms. Unused entries are default-constructed.
    std::array<ValueType, 2> types;
    // The instruction's own source location; SourceLoc{0} when the frontend
    // recorded none (a synthesized instruction).
    SourceLoc loc{ 0 };
};

// Check every process and function body of `m`.
std::vector<Violation> verify(const Module& m);

// One line, no trailing newline:
//   ir-verify: <rule>: <site> %<inst> <tag>(<types>) [at <file:line:col>]
// `resolve` is optional, as for print_module.
std::string format(const Module& m, const Violation& v, const LocResolver& resolve = {});

// Abort, naming every finding on stderr, if `m` violates a rule. The lowering's
// contract with the backend: the backend asserts the invariants these rules state
// and never repairs them. Compiled out under NDEBUG (every default build type keeps
// assertions, see CMakeLists.txt), where the callers that want the answer anyway
// call verify() themselves.
void assert_well_typed(const Module& m, const LocResolver& resolve = {});

} // namespace clotho::ir
