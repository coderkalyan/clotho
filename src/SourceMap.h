#pragma once

// Conversions between slang::SourceLocation and the IR's frontend-agnostic
// ir::SourceLoc handle. Lives in the slang-aware layer so clotho_ir stays free
// of any slang dependency.

#include <slang/text/SourceLocation.h>

#include "ir/Ir.h"

namespace clotho {

// Pack a slang source location into the opaque IR handle.
ir::SourceLoc toSourceLoc(slang::SourceLocation loc);

// Unpack an IR handle back into a slang source location.
slang::SourceLocation toSlangLocation(ir::SourceLoc loc);

} // namespace clotho
