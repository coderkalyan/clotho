#include "lsp/Tokens.h"

#include <algorithm>
#include <array>
#include <optional>
#include <variant>

#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Compilation.h>
#include <slang/ast/Symbol.h>
#include <slang/ast/expressions/CallExpression.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/SubroutineSymbols.h>
#include <slang/ast/symbols/ValueSymbol.h>
#include <slang/ast/types/AllTypes.h>
#include <slang/ast/types/DeclaredType.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>

namespace clotho::lsp {

namespace {

// Token types. Standard LSP names wherever a theme's existing colour is
// SEMANTICALLY right, per docs/design/lsp.md 8.12.3 -- a package really is a
// namespace and an SV interface really is an interface, so an unmodified theme
// renders both correctly without knowing SystemVerilog exists. Custom names
// only where nothing fits, and those degrade to "no colour" rather than to a
// wrong one.
//
// Order is wire format: a token's type field is an index into kTypes. Append
// only, never reorder.
enum class TokenType : uint32_t {
    Namespace,  // package
    TypeName,   // typedef whose target is neither a struct nor an enum
    Struct,     // struct or union typedef
    Enum,       // enum typedef
    EnumMember, // enum value
    Interface,  // SV interface instance
    Variable,   // variable, net, parameter (the last with `readonly`)
    Parameter,  // subroutine formal argument
    Property,   // class member field
    Function,   // function, task, system task
    Method,     // class method
    Module,     // custom: LSP has no name for a module
    Modport,    // custom
    Count,
};

constexpr std::array<std::string_view, size_t(TokenType::Count)> kTypes{
    "namespace", "type",     "struct",   "enum",   "enumMember", "interface", "variable",
    "parameter", "property", "function", "method", "module",     "modport",
};

// Modifiers are a bitset in one uinteger, so 32 is the hard ceiling. This is
// only the baseline allocation; the design modifiers of 8.12.3 (flop/latch/comb
// from S6, mayX from S5, domain<n>) take later slots, which is why the doc
// reserves them rather than leaving the numbering to chance.
enum class Mod : uint32_t {
    Declaration    = 1u << 0,
    Definition     = 1u << 1,
    Readonly       = 1u << 2,
    DefaultLibrary = 1u << 3,
};

constexpr std::array<std::string_view, 4> kModifiers{
    "declaration",
    "definition",
    "readonly",
    "defaultLibrary",
};

constexpr uint32_t bit(Mod mod) {
    return static_cast<uint32_t>(mod);
}

struct Item {
    uint32_t  offset;
    uint32_t  length;
    TokenType type;
    uint32_t  modifiers;
};

// What a declaration should be coloured as. Nullopt means "nothing an editor
// should recolour": most of slang's symbol kinds are elaboration bookkeeping
// with no identifier of their own in the source.
std::optional<TokenType> type_of(const slang::ast::Symbol& symbol) {
    using namespace slang::ast;
    switch (symbol.kind) {
        case SymbolKind::Package:
            return TokenType::Namespace;
        case SymbolKind::TypeAlias: {
            // Coloured by what it aliases, so `typedef enum {...} state_t` reads
            // as an enum rather than as an anonymous "type".
            const slang::ast::Type& target = symbol.as<TypeAliasType>().targetType.getType().getCanonicalType();
            if (target.isEnum()) {
                return TokenType::Enum;
            }
            if (target.isStruct() || target.isUnpackedStruct() || target.isUnion()) {
                return TokenType::Struct;
            }
            return TokenType::TypeName;
        }
        case SymbolKind::EnumValue:
            return TokenType::EnumMember;
        case SymbolKind::Parameter:
        case SymbolKind::TypeParameter:
        case SymbolKind::Specparam:
        case SymbolKind::Net:
        case SymbolKind::Variable:
            return TokenType::Variable;
        case SymbolKind::FormalArgument:
            return TokenType::Parameter;
        case SymbolKind::ClassProperty:
            return TokenType::Property;
        case SymbolKind::Subroutine:
            return symbol.as<SubroutineSymbol>().thisVar != nullptr ? TokenType::Method : TokenType::Function;
        case SymbolKind::Modport:
            return TokenType::Modport;
        case SymbolKind::Instance:
            return symbol.as<InstanceSymbol>().isInterface() ? TokenType::Interface : TokenType::Module;
        default:
            return std::nullopt;
    }
}

uint32_t modifiers_of(const slang::ast::Symbol& symbol) {
    using namespace slang::ast;
    switch (symbol.kind) {
        case SymbolKind::Parameter:
        case SymbolKind::TypeParameter:
        case SymbolKind::Specparam:
        case SymbolKind::EnumValue:
            // Themes already render `readonly` as a constant, which is exactly
            // what a localparam is.
            return bit(Mod::Readonly);
        default:
            return 0;
    }
}

class Collector : public slang::ast::ASTVisitor<Collector, slang::ast::VisitFlags::AllCanonical> {
public:
    Collector(const slang::SourceManager& sm, slang::BufferID buffer)
      : sm_(sm)
      , buffer_(buffer) {}

    std::vector<Item> take() {
        return std::move(items_);
    }

    template<typename T>
    void handle(const T& node) {
        if constexpr (std::is_base_of_v<slang::ast::Symbol, T>) {
            declare(node);
        } else if constexpr (std::is_base_of_v<slang::ast::Expression, T>) {
            use(node);
        }
        visitDefault(node);
    }

private:
    void declare(const slang::ast::Symbol& symbol) {
        using namespace slang::ast;
        if (!symbol.name.empty()) {
            if (const std::optional<TokenType> type = type_of(symbol)) {
                add(symbol.location,
                    static_cast<uint32_t>(symbol.name.size()),
                    *type,
                    modifiers_of(symbol) | bit(Mod::Declaration));
            }
        }

        // A declaration also paints two things that are not the declared name.
        if (const DeclaredType* declared = symbol.getDeclaredType()) {
            enum_values(declared->getType());
            type_reference(*declared);
        }
        if (symbol.kind == SymbolKind::TypeAlias) {
            enum_values(symbol.as<TypeAliasType>());
        }
    }

    // The members of an enum are declared inside its type, which the AST walk
    // does not descend into -- types are reached through symbols, not visited.
    // Without this `IDLE` is coloured everywhere it is USED and plain where it
    // is declared, which reads as a broken highlighter rather than a missing
    // feature.
    void enum_values(const slang::ast::Type& type) {
        using namespace slang::ast;
        const slang::ast::Type& canonical = type.getCanonicalType();
        if (!canonical.isEnum()) {
            return;
        }
        for (const EnumValueSymbol& value : canonical.as<EnumType>().values()) {
            add(value.location,
                static_cast<uint32_t>(value.name.size()),
                TokenType::EnumMember,
                bit(Mod::Readonly) | bit(Mod::Declaration));
        }
    }

    // The type NAME in `state_t st;`. It is the identifier a reader most wants
    // distinguished from a variable, and it exists only in syntax: the AST
    // records that `st` has an enum type, not that the word `state_t` appears
    // before it. Anonymous and built-in types have no alias to point at and are
    // left to the client's own highlighter, which already knows `logic`.
    void type_reference(const slang::ast::DeclaredType& declared) {
        using namespace slang::ast;
        const slang::syntax::DataTypeSyntax* syntax = declared.getTypeSyntax();
        if (syntax == nullptr || syntax->kind != slang::syntax::SyntaxKind::NamedType) {
            return;
        }
        const slang::ast::Type& type = declared.getType();
        if (type.kind != SymbolKind::TypeAlias) {
            return;
        }
        if (const std::optional<TokenType> painted = type_of(type)) {
            add(syntax->getFirstToken().location(), static_cast<uint32_t>(type.name.size()), *painted, 0);
        }
    }

    void use(const slang::ast::Expression& expr) {
        using namespace slang::ast;
        switch (expr.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                const ValueSymbol& symbol = expr.as<ValueExpressionBase>().symbol;
                if (const std::optional<TokenType> type = type_of(symbol)) {
                    // The name, not the expression's range: a hierarchical
                    // reference's range covers the whole dotted path, and only
                    // its first segment starts here.
                    add(
                      expr.sourceRange.start(), static_cast<uint32_t>(symbol.name.size()), *type, modifiers_of(symbol));
                }
                break;
            }
            case ExpressionKind::Call: {
                const auto&            call = expr.as<CallExpression>();
                const std::string_view name = call.getSubroutineName();
                if (call.isSystemCall()) {
                    // $display and friends: a function the language itself
                    // provides, which is what defaultLibrary means.
                    add(expr.sourceRange.start(),
                        static_cast<uint32_t>(name.size()),
                        TokenType::Function,
                        bit(Mod::DefaultLibrary));
                    break;
                }
                const auto* sub = std::get<const SubroutineSymbol*>(call.subroutine);
                add(expr.sourceRange.start(),
                    static_cast<uint32_t>(name.size()),
                    sub != nullptr && sub->thisVar != nullptr ? TokenType::Method : TokenType::Function,
                    0);
                break;
            }
            default:
                break;
        }
    }

    void add(slang::SourceLocation loc, uint32_t length, TokenType type, uint32_t modifiers) {
        // Two distinct reasons a location is not paintable. A macro location
        // addresses slang's expansion buffer, whose offsets are positions in no
        // file at all -- 38 of 50 tokens in a macro-heavy source, and zero in
        // hazard3_core.v, which is why this cannot be left to be noticed later.
        // A different file buffer is an included header: a real position, in a
        // document the client did not ask about.
        if (length == 0 || !loc.valid() || loc.buffer() != buffer_ || sm_.isMacroLoc(loc)) {
            return;
        }
        items_.push_back(Item{
          .offset = static_cast<uint32_t>(loc.offset()), .length = length, .type = type, .modifiers = modifiers });
    }

    const slang::SourceManager& sm_;
    slang::BufferID             buffer_;
    std::vector<Item>           items_;
};

} // namespace

std::span<const std::string_view> token_type_legend() {
    return kTypes;
}

std::span<const std::string_view> token_modifier_legend() {
    return kModifiers;
}

std::vector<uint32_t> semantic_tokens(const SourceContext& sources, std::string_view path, const LineIndex& lines) {
    const slang::ast::Compilation* comp = sources.compilation();
    if (comp == nullptr) {
        return {};
    }
    const slang::SourceManager* sm = comp->getSourceManager();
    if (sm == nullptr) {
        return {};
    }

    // Which buffer is the document. Everything else is dropped in add().
    slang::BufferID buffer;
    for (const auto& tree : comp->getSyntaxTrees()) {
        const slang::BufferID candidate = tree->root().sourceRange().start().buffer();
        if (sm->getFullPath(candidate).string() == path) {
            buffer = candidate;
            break;
        }
    }
    if (!buffer.valid()) {
        return {};
    }

    Collector collector(*sm, buffer);
    // NoFinalize because elaboration already happened: lower_compilation forced
    // it through getAllDiagnostics and then froze the compilation, and calling
    // getRoot() on a frozen compilation throws.
    comp->getRootNoFinalize().visit(collector);
    std::vector<Item> items = collector.take();

    // Sorted, because the encoding is relative and the client applies it in
    // order. Deduplicated because collisions are real rather than defensive: a
    // declaration and an expression can name the same token, and overlapping
    // tokens are legal only for a client that opted into them.
    std::ranges::sort(items, [](const Item& a, const Item& b) { return a.offset < b.offset; });
    const auto duplicates =
      std::ranges::unique(items, [](const Item& a, const Item& b) { return a.offset == b.offset; });
    items.erase(duplicates.begin(), duplicates.end());

    // Five integers per token, each position relative to the one before. With
    // prev_line and prev_char starting at zero the first token needs no special
    // case: its "delta" from the origin is its absolute position.
    std::vector<uint32_t> out;
    out.reserve(items.size() * 5);
    uint32_t prev_line = 0;
    uint32_t prev_char = 0;
    for (const Item& item : items) {
        const LineIndex::Point start = lines.point(item.offset);
        const LineIndex::Point end   = lines.point(item.offset + item.length);
        if (end.line != start.line) {
            // An identifier cannot span lines, so this is a length that ran off
            // the end of one. Dropping it beats emitting a token that makes the
            // client reject the whole response.
            continue;
        }

        out.push_back(start.line - prev_line);
        out.push_back(start.line == prev_line ? start.character - prev_char : start.character);
        out.push_back(lines.length_between(item.offset, item.offset + item.length));
        out.push_back(static_cast<uint32_t>(item.type));
        out.push_back(item.modifiers);
        prev_line = start.line;
        prev_char = start.character;
    }
    return out;
}

} // namespace clotho::lsp
