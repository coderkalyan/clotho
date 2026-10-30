#pragma once

// IR verifier: checks a lowered Module against the typing rules of
// docs/design/kernel.md ("Values"). The language decides domains, signedness
// and extension through slang's legalized AST and the IR carries the result, so
// an instruction whose operands disagree is a frontend error, not something the
// backend should repair. Today the backend does repair several of these (it
// promotes a 2-state operand next to a 4-state one, reads signedness from the
// left operand only, extends a Resize by the TARGET's sign); each rule below
// names the instructions that rely on that, so a violation count is the number
// of sites the repair is still load-bearing for.
//
// This is a measurement and a guard, not a gate: verify() only reports. The
// types it checks are the ones the backend derives (Typing.h), so a rule is
// violated exactly when the backend would have to patch or would emit an
// ill-typed operation.
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
    // Both operands have the same domain. Backend patch: lower_arith,
    // lower_divmod and lower_bitwise promote the 2-state side (to_four);
    // lower_compare, lower_case_compare and lower_wildeq lift both sides.
    OperandDomain,
    // Ordered compares, Div, Mod and the InsideSet ranges: both operands have the
    // same signedness (the opcode picks one reading for both).
    OperandSign,
    // The signedness an opcode asserts (UDiv/SDiv, ULt/SLt, Sra, UItoF/SItoF, Pow's
    // and InsideSet's flags) differs from its operand's type. A check on the
    // frontend's choice of opcode, in force while the IR's types still carry a sign.
    OpSign,
    // Add Sub Mul Band Bor Bxor Bxnor: operand signedness agrees, because the
    // result's signedness is the LEFT operand's (compute_type).
    ResultSign,
    // Div Mod Pow over a 2-state left operand. slang types these results 4-state
    // (they can be x) and every source-level one arrives so; a 2-state one is
    // arithmetic the frontend synthesized, and is what the backend's separate
    // 2-state divide / modulo path serves (kernel.md, "Operators"). A record.
    ResultDomain,
    // Sll Srl Sra: a 4-state amount makes the LRM result 4-state, but the IR
    // result is the shifted operand's type. Also: the shifted operand or the
    // amount is not an integer.
    ShiftResult,
    // -- Selection --
    // Select / SelectLazy arms agree in width, domain and signedness. Backend
    // patch: lower_select lifts a lone 2-state arm, lower_select_lazy
    // normalizes both arms to the result domain; compute_type takes arm `a`.
    SelectArmWidth,
    SelectArmDomain,
    SelectArmSign,
    // Cond / Select / SelectLazy / Loop predicates are 1 bit wide: the backend
    // compares the value's must plane against a 1-bit zero.
    PredicateWidth,
    // -- Stores --
    // The stored value's width equals the lvalue's. The frontend context-sizes
    // the right-hand side with a Resize, so the backend stores planes directly.
    StoreWidth,
    // The stored value's domain differs from the lvalue's. Backend patch:
    // lower_store projects a 4-state value to a narrow slot and lifts a 2-state
    // one into the packed word; lower_store_nb and the array / aggregate NBA
    // paths lift likewise. Recorded separately from StoreWidth.
    StoreDomain,
    // -- Resize: records, not errors --
    // A Resize whose source and target signedness differ. The extension fill is
    // chosen by the TARGET's signedness (convert(..., to.is_signed)); the LRM
    // picks the source's for an assignment (11.8.3) and the target's for an
    // operand (11.8.2), so this is where the rule is implicit today.
    ResizeSignWiden,
    ResizeSignNarrow,
    // Same width, signedness differs: a retag, no extension.
    ResizeSignRetag,
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
    return rule == Rule::ResizeSignWiden || rule == Rule::ResizeSignNarrow || rule == Rule::ResizeSignRetag ||
           rule == Rule::StoreDomain || rule == Rule::ResultDomain;
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
    // actual and the formal; for a Resize the source and the target; for
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

} // namespace clotho::ir
