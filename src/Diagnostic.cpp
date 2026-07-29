#include "Diagnostic.h"

#include "SourceContext.h"

namespace clotho {

namespace {

std::string_view severity_name(Severity s) {
    switch (s) {
        case Severity::Note:
            return "note";
        case Severity::Warning:
            return "warning";
        case Severity::Error:
            return "error";
    }

    return "error";
}

// Append one item plus its notes. `depth` indents nested notes so a chain of
// "declared here" follow-ups reads as belonging to its parent.
void render_one(const Diagnostic& d, const SourceContext* context, unsigned depth, std::string& out) {
    // The producing engine already formatted this one (slang, with a source
    // snippet and caret). Pass it through rather than producing something worse.
    if (!d.rendered.empty()) {
        out += d.rendered;
        if (out.back() != '\n') {
            out += '\n';
        }

        return;
    }

    out.append(static_cast<size_t>(depth) * 2, ' ');
    if (context != nullptr && d.span.valid()) {
        out += context->render(d.span.start);
        out += ": ";
    }
    out += severity_name(d.severity);
    out += ": ";
    out += d.message;
    if (!d.code.empty()) {
        out += " [";
        out += d.code;
        out += "]";
    }
    out += '\n';

    for (const Diagnostic& note : d.notes) {
        render_one(note, context, depth + 1, out);
    }
}

} // namespace

std::string render(const std::vector<Diagnostic>& diags, const SourceContext* context) {
    std::string out;
    for (const Diagnostic& d : diags) {
        render_one(d, context, 0, out);
    }

    return out;
}

bool has_error(const std::vector<Diagnostic>& diags) {
    for (const Diagnostic& d : diags) {
        if (d.severity == Severity::Error) {
            return true;
        }
    }

    return false;
}

size_t count_of(const std::vector<Diagnostic>& diags, Severity severity) {
    size_t n = 0;
    for (const Diagnostic& d : diags) {
        n += static_cast<size_t>(d.severity == severity);
    }

    return n;
}

} // namespace clotho
