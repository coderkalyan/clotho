#pragma once

// Structured diagnostics: the frontend's report of what it found, as data rather
// than as pre-rendered text.
//
// Clotho used to hand back one flat `std::string` of already-formatted slang
// output plus, at most, a single "error: ..." line from the first lowering guard
// it tripped. That is fine for a CLI and useless for anything else: a language
// server needs per-item severities and ranges, a synthesis or lint mode needs to
// apply its own severity policy, and a CI report needs to key on a stable code
// rather than grep prose. So lowering emits `Diagnostic` values and rendering
// becomes a separate, optional step (`render`).
//
// This deliberately does NOT wrap slang's own Diagnostic type. Clotho's
// diagnostics come from two places -- slang's elaboration/analysis engine and
// Clotho's own ~290 lowering guards -- and downstream consumers should not have
// to care which. Both are normalized into this shape at the boundary.

#include <cstdint>
#include <string>
#include <vector>

#include "ir/Ir.h"

namespace clotho {

// Resolves an ir::SourceLoc to a file position. Only needed to RENDER a
// diagnostic, never to produce or inspect one, so it stays a forward declaration.
class SourceContext;

// How bad an item is. Ordered, so `>=` works as a filter threshold.
//
// Note this is Clotho's own scale, not slang's: slang distinguishes Ignored /
// Note / Warning / Error / Fatal, and maps its Fatal onto Error here because
// Clotho has no separate "stop immediately" tier -- a lowering guard that
// cannot continue throws, and everything else accumulates.
enum class Severity : uint8_t {
    // Attached context for the item above it. Never appears at top level.
    Note,
    // The design is usable but something is suspect.
    Warning,
    // No usable module will be produced.
    Error,
};

// Where a diagnostic came from. Consumers apply different policy per origin: a
// synthesis or lint mode wants to re-rank slang's warnings without touching
// Clotho's own hard limits, and an editor wants to distinguish "your code is
// wrong" from "this tool does not implement that yet".
enum class DiagnosticOrigin : uint8_t {
    // slang parse / elaboration / dataflow-analysis diagnostic.
    Frontend,
    // A Clotho lowering guard: a legal construct Clotho does not implement.
    // Always paired with a TODO in the source (see PRINCIPLES.md).
    Unsupported,
    // Clotho refuses the construct on purpose (a security gate, an unsupported
    // combination), as opposed to not having got to it yet.
    Rejected,
    // A finding from Clotho's own analysis rather than a limitation of it: the
    // code is legal, implemented, and simulates -- Clotho has just proved
    // something about it the author probably did not intend. Distinct from
    // Frontend because the severity policy for these differs sharply by
    // consumer: a synthesis mode wants a mismatch risk to be an error, a linter
    // a warning, and a simulation run does not care at all.
    Analysis,
    // Something outside the design: an unreadable file, a bad command line.
    Driver,
};

struct Diagnostic {
    Severity         severity = Severity::Error;
    DiagnosticOrigin origin   = DiagnosticOrigin::Frontend;
    // Stable machine-readable identifier for filtering and suppression.
    // slang diagnostics carry their option name where they have one ("width-trunc");
    // Clotho's own carry a hand-assigned kebab-case name ("unsupported-construct").
    // May be empty when the underlying diagnostic has no stable name -- consumers
    // must tolerate that rather than keying on it unconditionally.
    std::string code;
    // Human-readable text. Never contains a location prefix: the location lives
    // in `span`, and rendering adds the prefix. This is what let the old flat
    // string double as both, and it is why it could not be filtered.
    std::string message;
    // Source extent. `span.valid()` is false for an item with no location (a
    // driver-level error, or a design-wide statement).
    ir::SourceSpan span;
    // Attached context: slang's own notes, plus Clotho's "declared here" style
    // follow-ups. Each carries its own span; severity is normally Note.
    std::vector<Diagnostic> notes;
    // The producing engine's own fully-formatted rendering, when it had one.
    //
    // This exists so structuring the diagnostics does not REGRESS the CLI. slang's
    // TextDiagnosticClient emits a source snippet with a caret under the offending
    // token, which is materially better than anything worth hand-rolling here, and
    // throwing it away to gain structure would be a bad trade. So slang-origin
    // items carry their pretty text along with the structured fields, and `render`
    // prefers it. Clotho-origin items leave this empty and `render` formats them.
    //
    // Consumers doing anything other than printing to a terminal should ignore
    // this field entirely and use the structured fields.
    std::string rendered;
    // The producing engine already showed this item to the user, in its own words.
    // The item stays in the list -- it still counts toward `has_error`, so the exit
    // status is unchanged -- but `render` skips it. Today only the driver notices
    // that follow a failure slang's Driver printed itself ("error: 'x.sv': No such
    // file or directory"), which a second line saying "command-line or
    // source-loading error" would only repeat.
    bool reported = false;
};

// Render a list in the compiler-conventional
// "file:line:col: severity: message [code]" form -- one item per line, notes
// indented beneath their parent, and `rendered` passed through verbatim where
// present. `context` may be null, in which case items render without a location.
[[nodiscard]] std::string render(const std::vector<Diagnostic>& diags, const SourceContext* context);

// True when any item would stop a module being produced.
[[nodiscard]] bool has_error(const std::vector<Diagnostic>& diags);

} // namespace clotho
