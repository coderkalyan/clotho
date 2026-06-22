#include "Frontend.h"

#include <slang/syntax/SyntaxTree.h>

namespace clotho {

ParseResult parseSource(std::string_view text) {
    const auto tree = slang::syntax::SyntaxTree::fromText(text);
    return {
        .parsed      = tree != nullptr,
        .diagnostics = !tree->diagnostics().empty(),
    };
}

} // namespace clotho
