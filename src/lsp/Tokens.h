#pragma once

// Semantic tokens: what each identifier in a buffer actually resolved to.
//
// The classification runs FORWARD -- walk the elaborated AST, take each
// symbol's and each expression's name token, paint it. That is worth stating
// because the reverse direction, offset -> symbol, is the reverse index that
// docs/design/lsp.md section 6 calls the largest line item in the project and
// that gates hover and go-to-definition. Colouring does not need it and does
// not wait for it.
//
// Only identifiers are emitted. Keywords, comments, literals and operators are
// left to the client's own highlighter: a client that sets
// `augmentsSyntaxTokens` (nvim does, and ships a treesitter SystemVerilog
// grammar) already colours those, and section 8.12.6 forbids the server
// replacing them. What the server adds is the part no grammar can know --
// whether an identifier is a type, an enum member, a parameter or a package,
// and whether it resolved to anything at all.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "SourceContext.h"
#include "lsp/LineIndex.h"

namespace clotho::lsp {

// The legend sent at `initialize`. A token's `tokenType` field is an INDEX into
// this array, so the order is wire format: append only, never reorder.
[[nodiscard]] std::span<const std::string_view> token_type_legend();

// Likewise, but a token's `tokenModifiers` field is a BITSET over these, so
// there is a hard ceiling of 32 (docs/design/lsp.md 8.12.3).
[[nodiscard]] std::span<const std::string_view> token_modifier_legend();

// Every semantic token in `path`, already in the protocol's delta encoding:
// five integers per token, sorted, each position relative to the one before.
//
// `lines` must be built over the same text the compilation was elaborated from.
// Locations outside `path` -- an included header, or a macro's expansion buffer
// -- are dropped rather than mapped, because they name a document the client
// did not ask about and, for the macro case, an offset that is not in any file.
[[nodiscard]] std::vector<uint32_t> semantic_tokens(const SourceContext& sources,
                                                    std::string_view     path,
                                                    const LineIndex&     lines);

} // namespace clotho::lsp
