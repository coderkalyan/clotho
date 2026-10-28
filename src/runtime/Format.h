// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <string_view>

// Shared $display format-specifier parser (LRM §21.2.1). Both the frontend
// (specifier validation and argument typing in lower/SystemTasks.cpp) and the runtime
// renderer (Runtime.cpp) parse specifiers through this one function, so the
// two sides cannot drift. Grammar accepted: `% [-] [width] [. precision] conv`.
// A leading 0 in the width folds into the numeric width (`%0d` is width 0, the
// LRM's minimal-width form); there is no zero-pad flag. The `-` left-justify
// flag is an extension the LRM does not define (docs/LRM_NOTES.md, note 30).

namespace clotho {

// One parsed specifier. `ok` is false when the text ends before a conversion
// character. `conv` is '%' for the literal `%%` escape.
struct FormatSpec {
    bool     ok;
    bool     left_justify;
    bool     has_width;
    bool     width_overflow;
    bool     has_prec;
    // The width began with a '0'. Integer conversions ignore this, because the 0
    // folds into the width (docs/LRM_NOTES.md, note 30); the real conversions honor it
    // as the C zero-pad flag (LRM §21.2.1.2 Table 21-2 grants %e/%f/%g the full
    // C formatting specification).
    bool     zero_pad;
    // C '#' alternative-form flag, honored by the real conversions only (the
    // frontend emits %#g for a bare real display argument).
    bool     alt;
    char     conv;
    uint32_t width;
    uint32_t prec;
    // Characters consumed after the '%' (flag + digits + conversion char).
    uint32_t len;
};

// Parse the specifier beginning immediately after a '%'.
inline FormatSpec parse_format_spec(std::string_view s) {
    FormatSpec spec{};
    size_t     i = 0;
    if (i >= s.size()) {
        return spec;
    }
    if (s[i] == '%') {
        spec.ok   = true;
        spec.conv = '%';
        spec.len  = 1;
        return spec;
    }
    while (i < s.size() && (s[i] == '-' || s[i] == '#')) {
        if (s[i] == '-') {
            spec.left_justify = true;
        } else {
            spec.alt = true;
        }
        ++i;
    }
    if (i < s.size() && s[i] == '0') {
        spec.zero_pad = true;
    }
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        spec.has_width   = true;
        const uint64_t w = uint64_t{ spec.width } * 10 + static_cast<uint64_t>(s[i] - '0');
        if (w > UINT32_MAX) {
            spec.width_overflow = true;
        } else {
            spec.width = static_cast<uint32_t>(w);
        }
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
        spec.has_prec = true;
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            const uint64_t p = uint64_t{ spec.prec } * 10 + static_cast<uint64_t>(s[i] - '0');
            spec.prec        = p > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(p);
            ++i;
        }
    }
    if (i >= s.size()) {
        return spec;
    }
    spec.conv = s[i];
    spec.len  = static_cast<uint32_t>(i + 1);
    spec.ok   = true;
    return spec;
}

} // namespace clotho
