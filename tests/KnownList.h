#pragma once

// The format of the suite's known lists (tests/config-matrix-known.txt and
// tests/trace-check-known.txt): one entry per line,
//
//   <category> "<doctest test case name>" <reason, to end of line>
//
// This is the format of .github/scripts/sv-tests-known.txt, but for the quotes a doctest
// name needs because it contains spaces. A line that is empty or starts with # is a
// comment. A list names its own categories.

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace clotho::test::known_list {

struct Entry {
    std::string category;
    std::string reason;
    size_t      line = 0;
};

// Parse a known list. Throws, naming the line, on anything it does not recognise: a typo
// must not silently stop covering a test.
inline std::map<std::string, Entry> parse(std::string_view text, std::string_view file,
                                          std::initializer_list<std::string_view> categories) {
    std::map<std::string, Entry> entries;
    size_t                       line_no = 0;
    for (size_t at = 0; at <= text.size();) {
        const size_t           nl   = std::min(text.find('\n', at), text.size());
        const std::string_view line = text.substr(at, nl - at);
        at                          = nl + 1;
        ++line_no;

        const auto fail = [&](const std::string& why) {
            throw std::runtime_error(std::string(file) + ":" + std::to_string(line_no) + ": " + why);
        };
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos || line[first] == '#') {
            continue;
        }
        std::string_view rest     = line.substr(first);
        const size_t     cat_end  = rest.find_first_of(" \t");
        const std::string category(rest.substr(0, cat_end));
        if (std::find(categories.begin(), categories.end(), category) == categories.end()) {
            std::string names;
            for (const std::string_view c : categories) {
                names += (names.empty() ? "" : " or ") + std::string(c);
            }
            fail("category '" + category + "' is not " + names);
        }
        rest = cat_end == std::string_view::npos ? std::string_view{} : rest.substr(cat_end);
        rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
        if (rest.empty() || rest.front() != '"') {
            fail("the test case name must follow the category, in double quotes");
        }
        const size_t close = rest.find('"', 1);
        if (close == std::string_view::npos) {
            fail("the test case name has no closing quote");
        }
        const std::string name(rest.substr(1, close - 1));
        rest.remove_prefix(close + 1);
        rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ' || rest.back() == '\t')) {
            rest.remove_suffix(1);
        }
        if (name.empty()) {
            fail("the test case name is empty");
        }
        if (rest.empty()) {
            fail("an entry needs a reason");
        }
        const auto [it, fresh] = entries.try_emplace(name, Entry{ category, std::string(rest), line_no });
        if (!fresh) {
            fail("\"" + name + "\" is already listed at line " + std::to_string(it->second.line));
        }
    }
    return entries;
}

} // namespace clotho::test::known_list
