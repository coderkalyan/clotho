// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Run-time error reports: severity tasks, bounds, null, reference and allocation checks.

#include "RuntimeInternal.h"

using namespace rt_internal;

namespace {

// Shared tail of the two DPI export-task fatals: both are reached from inside a
// native C frame the simulator cannot unwind, so neither can hand a settled state
// back the way rt_finish_coop does. Same reasoning as rt_oscillation_abort --
// report, close the dump while the design state is still live, exit nonzero.
[[noreturn]] void dpi_export_task_fatal(const char* name, const char* what) {
    std::fflush(stdout);
    std::string line = "FATAL: exported task '";
    line += name;
    line += "' ";
    line += what;
    line += " at ";
    append_finish_time(line, rt_diag_now());
    line += '\n';
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
    rt_vcd_close();
    rt_finishing = 1;
    std::exit(1);
}

} // namespace

extern "C" void rt_dpi_export_task_suspended(const char* name) {
    // The task's coroutine came back from its first resume not done, so it parked
    // on a delay / event control / `wait`. Its caller is a C frame on the
    // simulation stack, and resuming that frame later needs the baton-pool stack
    // handoff of DPI-task phase T2 (docs/design/dpi-c.md). Until it lands, only
    // the zero-time case is supported and this one is refused loudly -- LRM
    // 35.5.1.5 blesses the suspending case, so a wrong answer here would be a
    // silent deviation, not a corner.
    dpi_export_task_fatal(name,
                          "suspended on a timing control, which needs the suspendable C "
                          "frame of DPI-task phase T2 (not implemented)");
}

extern "C" void rt_dpi_export_task_no_task_frame(const char* name) {
    // No imported-TASK frame is live on the way in (clotho.dpi.in_import_task is
    // 0), so the innermost DPI frame is an imported FUNCTION -- or the C was not
    // reached through a DPI import at all. LRM 35.8 calls the first case never
    // legal, on the same ground SystemVerilog forbids a function performing a task
    // enable; the second has no calling thread whose time and region the task
    // could run in, so it is refused on the same line.
    dpi_export_task_fatal(name,
                          "was called from C with no imported DPI task frame live (LRM 35.8: "
                          "it is never legal to call an exported task from within an imported "
                          "function)");
}

extern "C" void rt_observed_case_violation(const char* banner, uint64_t now, int32_t kind) {
    g_observed_case.push_back({ banner, now, kind });
}

extern "C" void rt_observed_flush() {
    if (g_observed_case.empty()) {
        return;
    }
    // A simulator diagnostic (not design output), so it goes to stderr, like
    // rt_finish's banner. The message content is not LRM-mandated; the two forms
    // below name the two violations a unique / priority if or case can produce.
    // Flush stdout first so buffered $display text is committed before the
    // warning interleaves.
    std::fflush(stdout);
    for (const ObservedCaseViol& r : g_observed_case) {
        std::string line = r.banner;
        line += r.kind == 0 ? "no condition is true in the unique/priority if/case at time "
                            : "if/case statement is not unique (overlapping items) at time ";
        append_finish_time(line, r.now);
        line += '\n';
        std::fputs(line.c_str(), stderr);
    }
    std::fflush(stderr);
    g_observed_case.clear();
}
namespace {

// ir::Severity::Fatal, the one level that changes how the run ends.
constexpr int32_t kSeverityFatal = 3;

} // namespace
namespace rt_internal {

std::vector<ObservedCaseViol>                    g_observed_case;
ankerl::unordered_dense::map<uint64_t, uint32_t> g_bounds_hits;
ankerl::unordered_dense::set<const void*>        g_ref_stale_seen;

// The line of one severity report (LRM 20.10), newline included:
// "<LABEL>: <banner> at <time>[: <message>]". LRM 20.10 requires the report to
// carry the call's location, the hierarchical name of the scope it is made in and
// the simulation time; the first two arrive in `banner`, resolved when the design
// was lowered. The time is the current one, written as the $finish banner writes
// it. The layout is not LRM-mandated (docs/LRM_NOTES.md, note 28).
std::string severity_line(int32_t level, const char* banner, const char* fmt, uint32_t argc, const rt_arg* argv) {
    static const char* const labels[] = { "INFO", "WARNING", "ERROR", "FATAL" };
    std::string line = labels[level >= 0 && level < 4 ? level : 2];
    line += ": ";
    if (banner[0] != '\0') {
        line += banner;
        line += ' ';
    }
    line += "at ";
    append_finish_time(line, rt_diag_now());
    // A bare `$error;` has no message, and a separator with nothing after it
    // reads as a truncated line.
    const size_t separator = line.size();
    line += ": ";
    render(line, fmt, argc, argv);
    if (line.size() == separator + 2) {
        line.resize(separator);
    }
    line += '\n';
    return line;
}

} // namespace rt_internal

extern "C" void rt_severity(int32_t level, const char* banner, const char* fmt, uint32_t argc,
                            const rt_arg* argv) {
    // $info / $warning / $error / $fatal (LRM 20.10) or an immediate-assertion
    // default failure (LRM 16.3). A simulator diagnostic, not design output, so
    // it goes to stderr like the $finish / case-violation banners; flush stdout
    // first so buffered $display text is committed before the report interleaves.
    io_ensure_init();
    std::fflush(stdout);
    if (level == kSeverityFatal) {
        // LRM 20.10: $fatal "terminates the simulation with an error code". The
        // termination itself is the $finish that follows this report; the code is
        // the run's exit status.
        g_fatal_seen = true;
    }
    const std::string line = severity_line(level, banner, fmt, argc, argv);
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}
namespace {

// ---- --sanitize=bounds reports ----

// Reports emitted per access site before the site goes quiet. An out-of-range
// index in an always block re-fires on every activation, and an unbounded
// stream of identical lines buries both the first report and the design's own
// output. Not configurable yet; a --sanitize option for it can come when
// something needs it.
constexpr uint32_t bounds_report_cap = 8;

} // namespace
namespace rt_internal {

bool diag_color() {
    int8_t on = g_diag_color.load(std::memory_order_relaxed);
    if (on < 0) {
#if defined(_WIN32)
        const bool tty = _isatty(_fileno(stderr)) != 0;
#else
        const bool tty = isatty(fileno(stderr)) != 0;
#endif
        const char* term = std::getenv("TERM");
        on = tty && std::getenv("NO_COLOR") == nullptr && (term == nullptr || std::strcmp(term, "dumb") != 0) ? 1 : 0;
        g_diag_color.store(on, std::memory_order_relaxed);
    }
    return on != 0;
}

// The source line a marker row will be measured against. A tab is one column to
// slang, so it must occupy one column here too or the caret drifts. Rendering it
// as a single space keeps the arithmetic exact at the cost of the source's own
// indentation width.
std::string snippet_text(const char* src) {
    std::string text = src;
    std::replace(text.begin(), text.end(), '\t', ' ');
    return text;
}

// The "<n> | <source>" row and, under it, the marker row carrying `marks`. The
// row is built as one padded string so two marks cannot fight over spacing;
// later marks overwrite earlier ones where they overlap. The text row puts a
// space after the bar, so source column N is offset N in the marker row.
void print_snippet(const std::string& text, int32_t line_no,
                   std::initializer_list<SnippetMark> marks, const fmt::text_style& loc,
                   const fmt::text_style& caret) {
    const std::string number = fmt::to_string(line_no);
    const std::string gutter(number.size(), ' ');
    fmt::print(stderr, loc, "{} |", number);
    fmt::print(stderr, " {}", text);
    fmt::print(stderr, "\n");
    std::string row;
    for (const SnippetMark& m : marks) {
        if (m.column <= 0 || static_cast<size_t>(m.column) > text.size() + 1) {
            continue;
        }
        const size_t at = static_cast<size_t>(m.column);
        const size_t n  = std::min(m.width, text.size() + 1 - at);
        if (row.size() < at + n) {
            row.resize(at + n, ' ');
        }
        row.replace(at, n, n, m.ch);
    }
    if (row.empty()) {
        return;
    }
    fmt::print(stderr, loc, "{} |", gutter);
    fmt::print(stderr, caret, "{}", row);
    fmt::print(stderr, "\n");
}

} // namespace rt_internal

extern "C" void rt_bounds_report(const rt_bounds_site* site, int64_t index, int64_t extent,
                                 uint32_t flags, uint64_t now) {
    const bool     write   = (flags & 1u) != 0;
    const bool     unknown = (flags & 2u) != 0;
    const bool     partial = (flags & 4u) != 0;
    const uint64_t key     = reinterpret_cast<uint64_t>(site) * 2 + (write ? 1 : 0);
    uint32_t&      hits    = g_bounds_hits[key];
    if (hits >= bounds_report_cap) {
        return;
    }
    ++hits;
    // A simulator diagnostic, not design output, so it goes to stderr like the
    // severity and case-violation reports; flush stdout first so buffered
    // $display text is committed before the warning interleaves.
    io_ensure_init();
    std::fflush(stdout);

    // Laid out like a modern compiler diagnostic: what went wrong, where, the
    // offending line with a caret under the access, then the context that fits
    // in none of those. The array's name and the simulation time are notes
    // rather than headline because the snippet already shows the access.
    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::yellow));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::yellow));

    std::string when;
    append_finish_time(when, now);
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break
    // leaves the style live across it.
    fmt::print(stderr, label, "warning:");
    // What was indexed, and how it was bounded. A runtime-sized object reports
    // the size it actually had; a declared one reports the range as written.
    // An x/z index is invalid by LRM 7.4.5 / Table 7-1 with no numeric value to
    // report, so it prints as what it is rather than as its must-plane bits.
    const char* noun = "index out of bounds";
    switch (site->what) {
        case RT_BOUNDS_BIT: noun = "bit select out of bounds"; break;
        case RT_BOUNDS_PART: noun = partial ? "part select partly out of bounds"
                                            : "part select out of bounds"; break;
        case RT_BOUNDS_CHAR: noun = "string index out of bounds"; break;
        case RT_BOUNDS_QUEUE_OP: noun = "queue index out of bounds"; break;
        case RT_BOUNDS_EMPTY_POP: noun = "pop from an empty queue"; break;
        case RT_BOUNDS_ASSOC_KEY: noun = "associative array key is invalid (x or z)"; break;
        default: break;
    }
    if (site->what == RT_BOUNDS_EMPTY_POP || site->what == RT_BOUNDS_ASSOC_KEY) {
        // No index and no bound to name: x/z makes ANY key invalid, not just
        // one outside some range (LRM 7.8.4 / 7.8.5), so the headline is the
        // whole finding, same as an empty-queue pop.
        fmt::print(stderr, head, " {}", noun);
    } else {
        const std::string idx   = unknown ? std::string("x") : fmt::to_string(index);
        const std::string subj  = site->what == RT_BOUNDS_PART
                                    ? fmt::format("index {} +: {}", idx, site->width)
                                    : fmt::format("index {}", idx);
        const char*       unit  = site->what == RT_BOUNDS_CHAR ? "length"
                                : (site->what == RT_BOUNDS_BIT || site->what == RT_BOUNDS_PART)
                                  ? "width"
                                  : "size";
        const std::string bound = extent >= 0 ? fmt::format("{} {}", unit, extent)
                                              : fmt::format("range [{}:{}]", site->left, site->right);
        fmt::print(stderr, head, " {}: {}, {}", noun, subj, bound);
    }
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        // The marker row: `^^^` under the index expression, `---` under the
        // array it indexes when both landed on this line.
        SnippetMark       acc{ 0, 0, '-' };
        if (site->acc_line_no == site->line_no) {
            size_t n = mark_width(site->acc_column, site->acc_end_column);
            if (n == 1) {
                // No extent recorded for the indexed expression. Fall back to
                // its name, and only where the source really spells it at that
                // column -- a hierarchical `name` is not what the statement
                // wrote, and the leaf may be renamed by a `` `define ``.
                const char*       dot  = std::strrchr(site->name, '.');
                const std::string leaf = dot != nullptr ? dot + 1 : site->name;
                const size_t      at   = static_cast<size_t>(site->acc_column) - 1;
                if (site->acc_column > 0 && at + leaf.size() <= text.size() &&
                    text.compare(at, leaf.size(), leaf) == 0) {
                    n = leaf.size();
                }
            }
            acc = SnippetMark{ site->acc_column, n, '-' };
        }
        print_snippet(text, site->line_no,
                      { acc,
                        SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " {} '{}' at {}", write ? "write to" : "read of", site->name, when);
    fmt::print(stderr, "\n");
    if (hits == bounds_report_cap) {
        fmt::print(stderr, tag, "  note:");
        fmt::print(stderr, body, " further reports from this access are suppressed");
        fmt::print(stderr, "\n");
    }
    std::fflush(stderr);
}

// ---- Null class-handle dereference (LRM 8.4) ----

extern "C" void rt_null_handle(const rt_null_site* site, uint64_t now) {
    // Reached only from the guard the backend plants ahead of every class-handle
    // dereference and every virtual dispatch. LRM 8.4 leaves the result of an
    // access through a null handle indeterminate and permits an implementation
    // error; the untrapped alternative is an inttoptr to address zero and a
    // segfault carrying nothing that points back at the design. Laid out like the
    // bounds report -- what went wrong, where, the offending line under a caret,
    // then the notes -- except that this one is fatal.
    io_ensure_init();
    std::fflush(stdout);

    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));

    std::string when;
    append_finish_time(when, now);
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break leaves
    // the style live across it.
    const bool iface = site->kind == RT_NULL_IFACE;
    fmt::print(stderr, label, "fatal:");
    if (iface) {
        fmt::print(stderr, head, " use of a null virtual interface");
    } else {
        fmt::print(stderr, head, " dereference of a null class handle");
    }
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    if (iface) {
        fmt::print(stderr, body,
                   " the handle names no interface instance at {} -- it was never assigned one, "
                   "or was assigned null",
                   when);
    } else {
        fmt::print(stderr, body,
                   " the handle holds no object at {} -- it was never assigned one, or was "
                   "assigned null",
                   when);
    }
    fmt::print(stderr, "\n");
    fmt::print(stderr, tag, "  note:");
    if (iface) {
        fmt::print(stderr, body, " assign an elaborated interface instance to the handle before "
                                 "reaching a member through it (LRM 25.9)");
    } else {
        fmt::print(stderr, body, " construct the object with `new` before reaching a member "
                                 "through it (LRM 8.4)");
    }
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_oscillation_abort: there is no defined state to hand
    // back to the host mid-instant, and std::exit keeps the nonzero status that
    // distinguishes this from a clean run.
    std::exit(1);
}

// The design's epoch-to-name table, registered once before it runs.
const rt_container_name* g_ref_names       = nullptr;
uint64_t                 g_ref_names_count = 0;

extern "C" void rt_ref_names(const rt_container_name* table, uint64_t count) {
    g_ref_names       = table;
    g_ref_names_count = count;
}

// Declared name of the container whose relocation epoch lives at `epoch`, or null
// when it has no statically known address -- a container inside a class body or
// inside another container's buffer. Naming nothing beats naming the wrong one:
// for `outer[0][0]` the epoch is the INNER queue's, so reporting the outer
// container's name would point at a place that did not move.
//
// Linear: reached at most once per run, from a report that is about to print.
const char* ref_container_name(const void* epoch) {
    for (uint64_t i = 0; i < g_ref_names_count; ++i) {
        if (g_ref_names[i].epoch == epoch) {
            return g_ref_names[i].name;
        }
    }
    return nullptr;
}

extern "C" void rt_ref_stale(const rt_ref_site* site, const void* epoch, uint64_t expect,
                             uint64_t actual, uint64_t now) {
    // Reached only from the guard the backend plants ahead of every dereference
    // through a `ref` formal, and only in the `actual > expect` direction -- the
    // guard routes the impossible one to rt_ref_epoch_regress instead. The
    // container was restructured while the ref was live, so the interior pointer
    // no longer names the actual and LRM 13.5.2's "the formal refers to the
    // actual" has stopped holding. Same layout as the null-handle fatal, except
    // that --loose-stale-ref downgrades this one to a once-per-site warning.
    const bool warn = g_ref_stale_warn != 0;
    if (warn && !g_ref_stale_seen.insert(site).second) {
        return;
    }
    io_ensure_init();
    std::fflush(stdout);

    const fmt::terminal_color accent =
        warn ? fmt::terminal_color::yellow : fmt::terminal_color::red;
    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(accent));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(accent));

    std::string when;
    append_finish_time(when, now);
    // The container is named when its handle has a fixed address; otherwise it is
    // described rather than guessed at (see ref_container_name).
    const char* const named = ref_container_name(epoch);
    const std::string what  = named != nullptr ? fmt::format("'{}'", named)
                                               : std::string("its container");
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break leaves
    // the style live across it.
    fmt::print(stderr, label, "{}", warn ? "warning:" : "fatal:");
    fmt::print(stderr, head,
               " ref '{}' is stale: {} was restructured (epoch {}, ref formed at epoch {})",
               site->formal, what, actual, expect);
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " a push, insert, delete, resize, reorder or whole-value assignment "
                             "to {} by {} moved the element '{}' was bound to",
               what, when, site->formal);
    fmt::print(stderr, "\n");
    if (warn) {
        fmt::print(stderr, tag, "  note:");
        fmt::print(stderr, body, " --loose-stale-ref: proceeding through the stale pointer anyway, "
                                 "and further reports from this dereference are suppressed");
        fmt::print(stderr, "\n");
        std::fflush(stderr);
        return;
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " do not restructure {} while the ref is live, or pass the element "
                             "by value instead (LRM 13.5.2)",
               what);
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_null_handle: there is no defined state to hand back to
    // the host mid-instant, and std::exit keeps the nonzero status that
    // distinguishes this from a clean run.
    std::exit(1);
}

extern "C" void rt_ref_epoch_regress(const rt_ref_site* site, const void* epoch,
                                     uint64_t expect, uint64_t actual, uint64_t now) {
    // The guard reached this because the container's epoch is BELOW the one the
    // ref recorded. Every bump is a ++ and no path resets the field, so a
    // container's epoch can only ever climb: no design can produce this. It means
    // the epoch plumbing itself is wrong -- a missing bump, a handle whose epoch
    // was copied rather than advanced, or a wrong value captured when the ref was
    // formed. --loose-stale-ref deliberately does not apply, because the invariant
    // being violated is Clotho's and not the design's.
    io_ensure_init();
    std::fflush(stdout);

    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));

    std::string when;
    append_finish_time(when, now);
    const char* const named = ref_container_name(epoch);
    const std::string what =
      named != nullptr ? fmt::format("'{}'", named) : std::string("its container");
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break leaves
    // the style live across it.
    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " internal error: the relocation epoch of {} ran backwards", what);
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " {} is at epoch {} but ref '{}' was formed at epoch {}, at {}",
               what, actual, site->formal, expect, when);
    fmt::print(stderr, "\n");
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " this is a bug in Clotho's epoch plumbing, not an error in the "
                             "design -- please report it with the source line above "
                             "(docs/design/data-model.md section 3)");
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_null_handle: there is no defined state to hand back to
    // the host mid-instant, and std::exit keeps the nonzero status that
    // distinguishes this from a clean run.
    std::exit(1);
}

extern "C" void rt_tagged_access(const rt_tagged_site* site, uint64_t tag, uint32_t tag_unknown,
                                 uint64_t now) {
    // Reached only from the guard the backend plants ahead of every tagged-union
    // member read and member assignment. LRM 11.9 makes such an access "completely
    // type-checked" and says an inconsistent one "results in a run-time error";
    // the untrapped alternative is reinterpreting one member's bits as another's,
    // exactly what LRM 7.3.2 says a tagged union exists to prevent. Same layout as
    // the null-handle fatal.
    io_ensure_init();
    std::fflush(stdout);

    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag_s = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));

    // The tag names itself only when it is defined and in range; an x/z tag (an
    // uninitialized tagged union, LRM 11.9) or a value past the member table
    // names no member at all.
    std::string held;
    if (tag_unknown != 0) {
        held = "undefined (x)";
    } else if (tag < site->count) {
        held = fmt::format("`{}`", std::string_view(site->names[tag], site->name_lens[tag]));
    } else {
        held = fmt::format("no member (tag {})", tag);
    }

    std::string when;
    append_finish_time(when, now);
    const char* const verb = site->is_write != 0 ? "assignment to" : "read of";
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break leaves
    // the style live across it.
    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " {} tagged-union member `{}`, which the current tag does not select",
               verb, site->member);
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag_s, "  note:");
    fmt::print(stderr, body, " the tag selects {} at {}", held, when);
    fmt::print(stderr, "\n");
    fmt::print(stderr, tag_s, "  note:");
    fmt::print(stderr, body, " a tagged union's tag and value change only together, through a whole-value "
                             "`tagged` expression -- assign `tagged {} (...)` before reaching this member "
                             "(LRM 11.9)",
               site->member);
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_null_handle: there is no defined state to hand back to
    // the host mid-instant, and std::exit keeps the nonzero status that
    // distinguishes this from a clean run.
    std::exit(1);
}
namespace {

// The noun a headline names the failing object by. Callers say "this <noun>"
// rather than "a <noun>": one phrasing serves "queue" and "associative array"
// alike, and it pairs with the caret already pointing at the statement.
const char* site_noun(const rt_alloc_site* site) {
    return site != nullptr && site->what != nullptr ? site->what : "value";
}

} // namespace
namespace rt_internal {

// Shared body of every heap-failure fatal: the compiler-diagnostic block
// (headline, location, source line under a caret) plus the notes, then exit.
// The caller composes the whole headline, so each failure leads with what is
// wrong with the size rather than with the machinery that noticed.
[[noreturn]] void alloc_fatal(const rt_alloc_site* site, uint64_t now, const std::string& headline) {
    io_ensure_init();
    std::fflush(stdout);

    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));

    const char* where = site != nullptr && site->where != nullptr ? site->where : "<unknown>";

    std::string when;
    append_finish_time(when, now);
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset past the line break leaks the style.
    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " {}", headline);
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", where);
    fmt::print(stderr, "\n");
    if (site != nullptr && site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " at {}", when);
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_oscillation_abort: no defined state to hand back, and
    // a nonzero status distinguishes this from a clean run.
    std::exit(1);
}

} // namespace rt_internal
namespace {

// Render a byte count the way a person reads one: exact, plus a rounded binary
// magnitude once it stops being legible on its own.
std::string bytes_text(uint64_t n) {
    static const char* const unit[] = { "bytes", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
    if (n < 1024) {
        return fmt::format("{} bytes", n);
    }
    double      v = static_cast<double>(n);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < sizeof(unit) / sizeof(unit[0])) {
        v /= 1024.0;
        ++u;
    }
    return fmt::format("{} bytes ({:.1f} {})", n, v, unit[u]);
}

} // namespace
namespace rt_internal {

// Declared with the format renderer, defined here because it reports in exactly
// the shape alloc_fatal already builds: a headline, the statement's location and
// source caret, and the simulation time. Nothing about that block is specific to
// allocation -- only its callers were, until LRM §21.3.3's runtime format string
// gave the renderer design errors of its own to report.
[[noreturn]] void dynfmt_fatal(const rt_fmt_dyn* dyn, const std::string& what) {
    alloc_fatal(dyn != nullptr ? dyn->site : nullptr, rt_diag_now(), what);
}

} // namespace rt_internal

extern "C" uint64_t rt_diag_now(void) {
    return g_diag_wheel != nullptr ? rt_wheel_now(g_diag_wheel) : 0;
}
namespace {

// The body behind both public entry points. `zeroed` picks calloc, whose large
// requests come back as untouched zero pages; the diagnostics are identical
// because the failure modes are.
void* alloc_bytes(uint64_t size, const rt_alloc_site* site, uint64_t now, bool zeroed) {
    if (size == 0) {
        // Every caller treats a null buffer with a zero length as the empty
        // container, and malloc(0) may hand back a pointer that must still be
        // freed. Normalizing here keeps the "null means empty" invariant.
        return nullptr;
    }
    // size_t is the only ceiling: a 32-bit host cannot address a >4 GiB buffer
    // however much the design asks for, and saying so beats a truncating cast.
    if (size > static_cast<uint64_t>(SIZE_MAX)) {
        alloc_fatal(site, now,
                    fmt::format("size for this {} exceeds what this machine can address: {}",
                                site_noun(site), bytes_text(size)));
    }
    const size_t bytes = static_cast<size_t>(size);
    void*        p     = zeroed ? std::calloc(bytes, 1) : std::malloc(bytes);
    if (p == nullptr) {
        alloc_fatal(site, now,
                    fmt::format("out of memory for this {}: the system refused {}",
                                site_noun(site), bytes_text(size)));
    }
    return p;
}

} // namespace

extern "C" void* rt_alloc_checked(uint64_t size, const rt_alloc_site* site, uint64_t now) {
    return alloc_bytes(size, site, now, /*zeroed=*/false);
}

extern "C" uint64_t rt_alloc_elems_size(uint64_t count, uint64_t stride, const rt_alloc_site* site,
                                        uint64_t now) {
    uint64_t size = 0;
    if (__builtin_mul_overflow(count, stride, &size)) {
        alloc_fatal(site, now,
                    fmt::format("size for this {} overflows: {} elements of {} bytes exceeds the "
                                "largest object this machine can address",
                                site_noun(site), count, stride));
    }
    return size;
}

extern "C" void* rt_alloc_elems_checked(uint64_t count, uint64_t stride, const rt_alloc_site* site,
                                        uint64_t now) {
    return rt_alloc_checked(rt_alloc_elems_size(count, stride, site, now), site, now);
}

extern "C" void* rt_zalloc_elems_checked(uint64_t count, uint64_t stride,
                                         const rt_alloc_site* site, uint64_t now) {
    return alloc_bytes(rt_alloc_elems_size(count, stride, site, now), site, now,
                       /*zeroed=*/true);
}

extern "C" void rt_alloc_negative(int64_t n, const rt_alloc_site* site, uint64_t now) {
    alloc_fatal(site, now,
                fmt::format("invalid negative size for this {}: {}", site_noun(site), n));
}

// A runtime replication multiplier with an illegal value (see Runtime.h). Two
// headlines, one shape: the negative one names the value the design computed,
// the x/z one cannot (an unknown has no value to print).
extern "C" void rt_string_repl_invalid(int64_t count, uint32_t unknown, const rt_alloc_site* site,
                                       uint64_t now) {
    if (unknown != 0) {
        alloc_fatal(site, now,
                    fmt::format("invalid x/z multiplier for this {} (LRM 11.4.12.1 requires a "
                                "non-x, non-z multiplier)",
                                site_noun(site)));
    }
    alloc_fatal(site, now,
                fmt::format("invalid negative multiplier for this {}: {} (LRM 11.4.12.1 requires "
                            "a non-negative multiplier)",
                            site_noun(site), count));
}
