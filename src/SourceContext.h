#pragma once

// SourceContext: keeps an ir::SourceLoc decodable after lowering returns.
//
// An `ir::SourceLoc` is an opaque (buffer id, char offset) pair that is only
// meaningful against the slang SourceManager that minted it. Every lowering entry
// point used to create that manager as a stack local and destroy it on return, so
// although every IR instruction carried a location, nothing could render one: the
// only caller of `to_slang_location` in the whole tree was a unit test. Locations
// were write-only.
//
// This is the fix. Lowering hands back a SourceContext that owns (or borrows) the
// manager and whatever else has to outlive it, so a diagnostic renderer, an IR
// dump, a synthesis report, or a language server can still resolve a location
// later. It is the piece that makes source fidelity actually usable rather than
// merely present.
//
// Cheap to hold: it is a handful of pointers. Cheap to share: consumers take it by
// `shared_ptr` so a diagnostic list can outlive the LowerResult it came from.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "ir/Ir.h"

namespace slang {
class SourceManager;
namespace ast {
class Compilation;
}
namespace driver {
class Driver;
}
} // namespace slang

namespace clotho {

// A resolved source position. Line and column are 1-based, matching compiler and
// editor convention; the LSP's 0-based positions are the server's conversion.
struct SourcePosition {
    // File name as slang reports it, honoring any `` `line `` directives.
    std::string file;
    uint32_t    line   = 0;
    uint32_t    column = 0;
    // False when the handle carried no location, or named a buffer this context
    // does not know. Consumers MUST check: a compiler-synthesized construct (a
    // wire resolver, a PreInit pass) legitimately has no source position.
    bool valid = false;
};

class SourceContext {
public:
    SourceContext();
    ~SourceContext();
    SourceContext(SourceContext&&) noexcept;
    SourceContext& operator=(SourceContext&&) noexcept;
    SourceContext(const SourceContext&)            = delete;
    SourceContext& operator=(const SourceContext&) = delete;

    // Resolve a handle. `valid` is false when unresolvable; the other fields are
    // then unspecified.
    [[nodiscard]] SourcePosition resolve(ir::SourceLoc loc) const;

    // "file:line:col", or "<unknown>" when unresolvable. The form diagnostics and
    // IR dumps use.
    [[nodiscard]] std::string render(ir::SourceLoc loc) const;

    // Absolute path of the file a location came from, ignoring any `` `line ``
    // directives. Empty when the location does not resolve to a file buffer.
    //
    // Deliberately not part of SourcePosition. `file` there is what a compiler
    // PRINTS, and slang renders that relative to the process's working
    // directory ("../../tmp/a.sv"), which is unusable to a consumer that does
    // not share the cwd -- an editor being the obvious one, since it needs to
    // turn the answer back into a file:// URI.
    [[nodiscard]] std::string full_path(ir::SourceLoc loc) const;

    // The source line the location sits on, verbatim and without its newline;
    // empty when unresolvable. Paired with `SourcePosition::column` this is
    // enough to draw the snippet-and-caret block a compiler diagnostic shows.
    [[nodiscard]] std::string line_text(ir::SourceLoc loc) const;

    // Byte offset of a location within its own buffer, for a consumer that has
    // the buffer text and its own line index. Empty when the location does not
    // resolve to a file buffer.
    //
    // The offset is raw: it ignores `` `line `` directives, which is right for
    // anyone addressing the actual bytes and wrong for anyone printing a
    // location, so this and `resolve` are not interchangeable.
    [[nodiscard]] std::optional<uint32_t> offset(ir::SourceLoc loc) const;

    // The elaborated compilation, when this context adopted one. Null otherwise
    // -- a context built by `lower_driver` before elaboration has none, and a
    // default-constructed one never will.
    //
    // Exposed for consumers that need the AST rather than just locations: a
    // language server colours identifiers by walking symbols forward to their
    // name tokens, which needs the same elaboration lowering already paid for
    // rather than a second one.
    [[nodiscard]] const slang::ast::Compilation* compilation() const;

    // True when nothing was ever adopted, so every resolve fails. A default
    // context is empty; this lets a consumer skip location work entirely.
    [[nodiscard]] bool empty() const;

    // -- Frontend-internal construction. --
    // Each entry point has a different ownership shape (a bare SourceManager, a
    // Driver that owns one, a Compilation created from either), so the pieces are
    // adopted individually. Declaration order in the class fixes destruction
    // order; see the note on the members.
    void adopt_source_manager(std::unique_ptr<slang::SourceManager> sm);
    void adopt_driver(std::unique_ptr<slang::driver::Driver> driver);
    void adopt_compilation(std::unique_ptr<slang::ast::Compilation> comp);
    // Point at a manager this context does not own — used when it is owned by an
    // adopted Driver, or is one of slang's process-wide static managers (as
    // `SyntaxTree::fromText` uses). The referent must outlive the context.
    void borrow_source_manager(const slang::SourceManager* sm);

private:
    // Destruction runs in reverse declaration order, and the dependency chain is
    // Compilation -> syntax trees -> SourceManager, with a Driver owning both a
    // manager and the trees it loaded. So the manager is declared FIRST (destroyed
    // last) and the Compilation LAST (destroyed first). Reordering these members
    // reintroduces a use-after-free on shutdown.
    std::unique_ptr<slang::SourceManager>    owned_sm_;
    std::unique_ptr<slang::driver::Driver>   driver_;
    std::unique_ptr<slang::ast::Compilation> comp_;
    // The manager every resolve goes through: either owned_sm_, or one borrowed
    // from driver_ / a slang static.
    const slang::SourceManager* sm_ = nullptr;
};

} // namespace clotho
