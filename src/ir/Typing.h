#pragma once

// Value typing of the process IR: the width, domain and signedness of the value
// each instruction produces. Nothing is stored on an instruction; its type
// follows from its operands' types and the types its payload declares
// (docs/design/kernel.md, "Values").
//
// This is the one derivation of those types. The backend (ir/llvm) lowers with
// it and the IR verifier (Verify.h) checks against it, so a rule the verifier
// holds is exactly the rule the backend relies on.

#include <cstdint>
#include <span>
#include <vector>

#include "InternPool.h"
#include "Ir.h"
#include "Module.h"

namespace clotho::ir {

// The type of the value an instruction produces.
struct ValueType {
    // Packed width in bits. A type with no packed width (void, an unpacked
    // aggregate, a container handle) reports 1; meaningless for a string.
    uint32_t width = 0;
    // 4-state (a meaningful x/z plane) rather than 2-state.
    bool four_state = false;
    bool is_signed  = false;
    // A string: an rt_string handle, never planes.
    bool is_string = false;
    // A real (width 64) or shortreal (width 32); never 4-state.
    bool is_real = false;

    bool operator==(const ValueType&) const = default;
};

static_assert(sizeof(ValueType) == 8, "one ValueType per instruction; keep it one word");

// The value type of a declared type: a constant's type, an Arg's or Local's
// declared type, a call's return type, a Resize target. A real counts as
// signed (LRM 11.8.1).
ValueType value_type_of(const InternPool& pool, Ref type);

// The type of instruction `i` of `ir`, given the types of every instruction
// before it (`done`, indexed by Index; it holds at least `i` entries). `ir` is a
// body of `m`, finished or still being built. A statement, or an lvalue producer
// with no value of its own, gets the placeholder its tag defines; only values
// are ever asked for. An operand that does not precede `i` is malformed IR and
// types as a void placeholder, which the verifier reports.
ValueType compute_type(const Module& m, const IrView& ir, Index i, std::span<const ValueType> done);

// The value type of every instruction of `ir`, indexed by Index. `ir` must be a
// body of `m` (its constants and types live in `m.pool`).
std::vector<ValueType> value_types(const Module& m, const Ir& ir);

} // namespace clotho::ir
