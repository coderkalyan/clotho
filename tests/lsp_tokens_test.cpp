// The two pieces of the semantic-token path that are worth testing directly
// rather than through the protocol: the position arithmetic, and what the AST
// walk decides each identifier is.
//
// Both are pure functions of a buffer, so a subprocess would add nothing. The
// protocol shape -- legend, resultId, delta -- is covered in lsp_test.cpp,
// where it can only be observed from outside.

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "lsp/LineIndex.h"
#include "lsp/Tokens.h"

using namespace clotho;
using clotho::lsp::Encoding;
using clotho::lsp::LineIndex;

namespace {

// A token decoded back out of the wire form, with the source text it covers.
// Decoding rather than asserting on raw integers is the point: it proves the
// relative encoding round-trips, which is the part that silently paints the
// wrong words if the deltas are wrong.
struct Decoded {
    uint32_t    line;
    uint32_t    character;
    std::string text;
    std::string type;
    std::string modifiers;
};

std::vector<Decoded> decode(std::string_view source, Encoding encoding = Encoding::Utf8) {
    const LowerResult           lowered = lower_buffer("/work/tok.sv", source);
    const LineIndex             lines(source, encoding);
    const std::vector<uint32_t> data = lsp::semantic_tokens(*lowered.sources, "/work/tok.sv", lines);
    REQUIRE(data.size() % 5 == 0);

    // Split the buffer so a token can be read back off its own line.
    std::vector<std::string_view> text_lines;
    size_t                        start = 0;
    for (size_t i = 0; i <= source.size(); ++i) {
        if (i == source.size() || source[i] == '\n') {
            text_lines.push_back(source.substr(start, i - start));
            start = i + 1;
        }
    }

    const auto types     = lsp::token_type_legend();
    const auto modifiers = lsp::token_modifier_legend();

    std::vector<Decoded> out;
    uint32_t             line      = 0;
    uint32_t             character = 0;
    for (size_t i = 0; i < data.size(); i += 5) {
        const uint32_t delta_line = data[i];
        line += delta_line;
        character = delta_line != 0 ? data[i + 1] : character + data[i + 1];

        REQUIRE(line < text_lines.size());
        REQUIRE(data[i + 3] < types.size());

        std::string mods;
        for (size_t bit = 0; bit < modifiers.size(); ++bit) {
            if ((data[i + 4] & (1u << bit)) != 0) {
                mods += mods.empty() ? "" : ",";
                mods += modifiers[bit];
            }
        }
        out.push_back(Decoded{ .line      = line,
                               .character = character,
                               .text      = std::string(text_lines[line].substr(character, data[i + 2])),
                               .type      = std::string(types[data[i + 3]]),
                               .modifiers = std::move(mods) });
    }
    return out;
}

const Decoded* find(const std::vector<Decoded>& tokens, std::string_view text, uint32_t line) {
    for (const Decoded& token : tokens) {
        if (token.text == text && token.line == line) {
            return &token;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("LineIndex maps offsets to lines") {
    const std::string text = "aaa\nbb\n\ncccc";
    const LineIndex   index(text, Encoding::Utf8);

    CHECK(index.point(0).line == 0);
    CHECK(index.point(2).character == 2);
    CHECK(index.point(4).line == 1); // first char of "bb"
    CHECK(index.point(7).line == 2); // the empty line
    CHECK(index.point(8).line == 3);
    CHECK(index.point(8).character == 0);
    // Past the end clamps rather than reading out of bounds.
    CHECK(index.point(9999).line == 3);
    CHECK(index.point(9999).character == 4);
}

TEST_CASE("LineIndex counts code units, not bytes or characters") {
    // U+00E9 is two UTF-8 bytes and one UTF-16 unit; U+1F600 is four bytes and
    // two units. Counting characters gets the first right and the second wrong,
    // which is why the surrogate case has to be tested separately.
    const std::string text = "x\u00e9\U0001F600y";

    SUBCASE("utf-8 counts bytes") {
        const LineIndex index(text, Encoding::Utf8);
        CHECK(index.point(static_cast<uint32_t>(text.size())).character == 8);
        CHECK(index.length_between(0, static_cast<uint32_t>(text.size())) == 8);
    }
    SUBCASE("utf-16 counts units, astral characters as two") {
        const LineIndex index(text, Encoding::Utf16);
        // x=1, é=1, emoji=2, y=1.
        CHECK(index.point(static_cast<uint32_t>(text.size())).character == 5);
        CHECK(index.length_between(0, static_cast<uint32_t>(text.size())) == 5);
    }
}

TEST_CASE("semantic tokens name what each identifier resolved to") {
    const std::vector<Decoded> tokens = decode(R"(package pkg;
  localparam int WIDTH = 8;
endpackage

module dut (input logic clk, output logic [7:0] q);
  typedef enum logic [1:0] { IDLE, BUSY } state_t;
  state_t st;
  logic [7:0] acc;
  function automatic logic [7:0] bump(input logic [7:0] v);
    return v + 1;
  endfunction
  always_ff @(posedge clk) begin
    acc <= bump(acc);
    $display("%0d", acc);
  end
  assign q = acc;
endmodule
)");

    REQUIRE(!tokens.empty());

    const auto typed = [&](std::string_view text, uint32_t line) {
        const Decoded* token = find(tokens, text, line);
        REQUIRE_MESSAGE(token != nullptr, "no token for ", text);
        return *token;
    };

    // A package is a namespace, which is the standard type whose existing theme
    // colour is semantically right (docs/design/lsp.md 8.12.3).
    CHECK(typed("pkg", 0).type == "namespace");
    CHECK(typed("dut", 4).type == "module");

    // A parameter is a variable the theme already renders as a constant.
    CHECK(typed("WIDTH", 1).type == "variable");
    CHECK(typed("WIDTH", 1).modifiers == "declaration,readonly");

    // A typedef is coloured by what it ALIASES, so an enum typedef reads as an
    // enum rather than as an anonymous "type".
    CHECK(typed("state_t", 5).type == "enum");
    // ... including where it is used as a type, which exists only in syntax.
    CHECK(typed("state_t", 6).type == "enum");
    CHECK(typed("state_t", 6).modifiers.empty());

    // Enum members are declared inside the type, which the AST walk does not
    // descend into on its own.
    CHECK(typed("IDLE", 5).type == "enumMember");
    CHECK(typed("BUSY", 5).type == "enumMember");

    CHECK(typed("bump", 8).type == "function");
    CHECK(typed("bump", 8).modifiers == "declaration");
    CHECK(typed("v", 8).type == "parameter");
    // A use carries no `declaration`.
    CHECK(typed("bump", 12).modifiers.empty());

    // A system task is a function the language provides.
    CHECK(typed("$display", 13).type == "function");
    CHECK(typed("$display", 13).modifiers == "defaultLibrary");
}

TEST_CASE("semantic tokens are sorted and never overlap") {
    // The encoding is relative, so an out-of-order token silently shifts every
    // one after it, and an overlap is illegal for a client that did not opt in.
    const std::vector<Decoded> tokens = decode(R"(module m (input logic a, input logic b, output logic y);
  logic t1, t2;
  assign t1 = a & b;
  assign t2 = t1 | a;
  assign y = t2 ^ b;
endmodule
)");

    REQUIRE(tokens.size() > 5);
    for (size_t i = 1; i < tokens.size(); ++i) {
        const Decoded& prev = tokens[i - 1];
        const Decoded& next = tokens[i];
        CHECK(std::pair(prev.line, prev.character) < std::pair(next.line, next.character));
        if (prev.line == next.line) {
            CHECK(prev.character + prev.text.size() <= next.character);
        }
    }
}

TEST_CASE("semantic tokens skip what a macro expanded to") {
    // Expanded tokens carry locations in slang's expansion buffer, whose offsets
    // are positions in no file at all. Emitting them puts tokens at arbitrary
    // points of the document, so the test is that every token still covers the
    // text it claims to.
    const std::vector<Decoded> tokens = decode(R"(`define DECL(n) logic n
module m (input logic clk);
  `DECL(alpha);
  `DECL(beta);
  logic plain;
  assign plain = alpha;
endmodule
)");

    for (const Decoded& token : tokens) {
        CHECK_FALSE(token.text.empty());
    }
    // The unexpanded declaration is still there and still resolves.
    CHECK(find(tokens, "plain", 4) != nullptr);
}

TEST_CASE("semantic tokens ignore a file the request did not name") {
    const LowerResult lowered = lower_buffer("/work/tok.sv", "module m; logic a; endmodule");
    const std::string source  = "module m; logic a; endmodule";
    const LineIndex   lines(source, Encoding::Utf8);

    // Same compilation, a path it does not contain: nothing to paint.
    CHECK(lsp::semantic_tokens(*lowered.sources, "/work/other.sv", lines).empty());
}
