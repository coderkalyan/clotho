#pragma once

// Byte offset -> LSP `Position`, for a buffer the server already holds.
//
// Two reasons this is its own type rather than a call into slang. The cheap one
// is speed: SourceManager::getColumnNumber walks backward to the start of the
// line on every call, which measures 1.00 us per token against 0.02 us through
// a line table -- irrelevant for the dozen locations a diagnostic list touches,
// and 39x the entire cost of semantic tokens, which touches ten thousand
// (docs/design/lsp.md 8.12.7).
//
// The load-bearing one is that `Position.character` is a UTF-16 code-unit index
// unless the client negotiated otherwise, and getting that conversion wrong
// misplaces every range in a file containing one non-ASCII character. It is the
// classic language-server bug. There is one implementation of it, here.

#include <cstdint>
#include <string_view>
#include <vector>

namespace clotho::lsp {

// What the client counts in `Position.character`. Negotiated once at
// `initialize` and then constant for the session.
enum class Encoding : uint8_t { Utf8, Utf16 };

std::string_view encoding_name(Encoding encoding);

class LineIndex {
public:
    struct Point {
        uint32_t line;
        uint32_t character;
    };

    LineIndex() = default;

    // `text` must outlive the index; nothing is copied.
    LineIndex(std::string_view text, Encoding encoding);

    // Clamps rather than failing: an offset past the end resolves to the end of
    // the buffer. Handing it a location minted for a different buffer is a
    // caller bug, but it must not read out of bounds.
    [[nodiscard]] Point point(uint32_t offset) const;

    // Code units between two byte offsets on the SAME line, in the negotiated
    // encoding. This is a token's `length` field, which LSP measures in code
    // units rather than bytes.
    [[nodiscard]] uint32_t length_between(uint32_t begin, uint32_t end) const;

private:
    std::string_view text_;
    // Byte offset of each line's first character. Always starts with 0, so it is
    // never empty for a constructed index.
    std::vector<uint32_t> starts_;
    Encoding              encoding_ = Encoding::Utf16;
};

} // namespace clotho::lsp
