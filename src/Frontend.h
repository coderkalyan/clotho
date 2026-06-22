#pragma once

// Front-end entry points. Currently a thin wrapper over slang's parser; the
// place where source-text -> syntax/AST lowering will live.

#include <string_view>

namespace clotho {

struct ParseResult {
    bool parsed      = false; // a syntax tree was produced
    bool diagnostics = false; // parser emitted any diagnostics
};

// Parse a SystemVerilog source fragment, reporting whether it parsed cleanly.
ParseResult parseSource(std::string_view text);

} // namespace clotho
