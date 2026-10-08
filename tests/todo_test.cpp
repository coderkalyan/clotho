#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

// docs/TODO.md lists gaps only (see its header). Prose rules alone did not hold:
// the file drifted into a changelog three times, because each edit appended a
// little and nothing looked at the whole. This test holds the shape instead.
// It checks only what can be checked mechanically: entry size, the two marks,
// grouping, and the stale-by-construction content the header forbids.

namespace {

constexpr std::size_t entry_lines_max = 3;
constexpr std::size_t line_columns_max = 100;

struct Line {
    std::uint32_t number;
    std::string text;
};

std::vector<Line> read_todo() {
    std::ifstream file(CLOTHO_TEST_DIR "/../docs/TODO.md");
    REQUIRE(file.good());
    std::vector<Line> lines;
    std::string text;
    std::uint32_t number = 0;
    while (std::getline(file, text)) {
        number += 1;
        lines.push_back({number, text});
    }
    REQUIRE(!lines.empty());
    return lines;
}

bool is_heading(const std::string& text) { return text.starts_with("#"); }

// An entry's bullet: `- [m] ` at column 0, or `  - [m] ` for a child.
bool is_bullet(const std::string& text, std::size_t& indent, char& mark) {
    std::size_t at = text.find_first_not_of(' ');
    if (at == std::string::npos) return false;
    if (text.compare(at, 3, "- [") != 0) return false;
    if (text.size() < at + 6) return false;
    if (text.compare(at + 4, 2, "] ") != 0) return false;
    indent = at;
    mark = text[at + 3];
    return true;
}

void check_content(const Line& line, std::vector<std::string>& out) {
    static const std::regex source_line(
        R"([A-Za-z0-9_./-]+\.(cpp|h|hpp|sv|svh|v|zig|py|md|txt):[0-9])");
    static const std::regex history(R"(\b(landed|used to|was fixed|no longer|now works)\b)",
                                    std::regex::icase);
    static const std::regex phase_label(R"(\b(T[1-3]|V[1-5]b?|R-[a-d]|P[0-5])\b)");
    auto where = "docs/TODO.md:" + std::to_string(line.number) + ": ";
    if (line.text.size() > line_columns_max) out.push_back(where + "line exceeds 100 columns");
    if (std::regex_search(line.text, source_line)) out.push_back(where + "source line number");
    if (std::regex_search(line.text, history)) out.push_back(where + "history, not a gap");
    if (std::regex_search(line.text, phase_label)) out.push_back(where + "internal phase label");
}

// Walks every line after the header (the first `##` heading) and checks entry
// shape: marks, size, grouping. Prose outside an entry is a violation, since
// that is where changelog text accumulates.
std::vector<std::string> check_todo(const std::vector<Line>& lines) {
    std::vector<std::string> out;
    std::size_t index = 0;
    while (index < lines.size() && !lines[index].text.starts_with("## ")) index += 1;
    REQUIRE(index < lines.size());

    std::uint32_t entry_start = 0;
    std::size_t entry_lines = 0;
    std::uint32_t group_start = 0;
    std::size_t group_children = 0;
    bool in_entry = false;
    bool in_group = false;
    auto where = [](std::uint32_t number) {
        return "docs/TODO.md:" + std::to_string(number) + ": ";
    };
    auto close_group = [&] {
        if (in_group && group_children == 0) {
            out.push_back(where(group_start) + "[~] group has no [ ] children");
        }
        in_group = false;
    };

    for (; index < lines.size(); index += 1) {
        const Line& line = lines[index];
        std::size_t indent = 0;
        char mark = 0;
        if (line.text.empty() || is_heading(line.text)) {
            if (is_heading(line.text)) close_group();
            in_entry = false;
            continue;
        }
        check_content(line, out);
        if (is_bullet(line.text, indent, mark)) {
            if (mark != ' ' && mark != '~') out.push_back(where(line.number) + "mark must be [ ] or [~]");
            if (indent == 0) {
                close_group();
                in_group = mark == '~';
                group_start = line.number;
                group_children = 0;
            } else if (!in_group || mark != ' ') {
                out.push_back(where(line.number) + "a child must be [ ] under a [~] group");
            } else {
                group_children += 1;
            }
            in_entry = true;
            entry_start = line.number;
            entry_lines = 1;
            continue;
        }
        if (!in_entry) {
            out.push_back(where(line.number) + "prose outside an entry");
            continue;
        }
        entry_lines += 1;
        if (entry_lines == entry_lines_max + 1) {
            out.push_back(where(entry_start) + "entry exceeds three lines");
        }
    }
    close_group();
    return out;
}

} // namespace

TEST_CASE("docs/TODO.md lists gaps in the required shape") {
    std::vector<Line> lines = read_todo();
    std::vector<std::string> violations = check_todo(lines);
    for (const std::string& violation : violations) FAIL_CHECK(violation);
    CHECK(violations.empty());
}
