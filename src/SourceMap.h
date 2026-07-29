#pragma once

// Conversions between slang::SourceLocation and the IR's frontend-agnostic
// ir::SourceLoc handle. Lives in the slang-aware layer so clotho_ir stays free
// of any slang dependency.

#include <slang/text/SourceLocation.h>

#include "ir/Ir.h"

namespace clotho {

// Pack a slang source location into the opaque IR handle.
ir::SourceLoc to_source_loc(slang::SourceLocation loc);

// Unpack an IR handle back into a slang source location.
slang::SourceLocation to_slang_location(ir::SourceLoc loc);

// Pack a slang source range into an IR span. slang's SourceRange is
// [start, end) over the raw text; the IR span keeps both endpoints as-is, so a
// consumer that wants an editor selection uses [start, end) directly.
ir::SourceSpan to_source_span(slang::SourceRange range);

} // namespace clotho
