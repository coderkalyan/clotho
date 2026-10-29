// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// SystemVerilog strings: storage, methods and conversions.

#include "RuntimeInternal.h"

using namespace rt_internal;

namespace {

// Grow s's buffer to hold at least n bytes, preserving contents and length.
// The handle stays valid across the grow, so aliased readers of s (same handle
// pointer) keep seeing the current bytes.
void str_reserve(rt_string* s, uint64_t n, const rt_alloc_site* site) {
    if (!str_is_heap(s)) {
        if (n <= str_inline_cap) {
            return;
        }
        const uint64_t len = str_inline_cap - s->storage[31];
        char*          buf = static_cast<char*>(rt_alloc_checked(n + 1, site, rt_diag_now()));
        std::memcpy(buf, s->storage, len);
        buf[len] = 0; // preserve the NUL at byte[len] in the new buffer
        str_set_heap(s, StrHeap{ buf, len, n });
        return;
    }
    StrHeap h = str_heap(s);
    if (n <= h.cap) {
        return;
    }
    // Geometric growth so repeated append-style reassignment amortizes.
    const uint64_t cap = h.cap * 2 > n ? h.cap * 2 : n;
    char*          buf = static_cast<char*>(rt_alloc_checked(cap + 1, site, rt_diag_now()));
    std::memcpy(buf, h.ptr, h.len);
    buf[h.len] = 0;
    std::free(h.ptr);
    str_set_heap(s, StrHeap{ buf, h.len, cap });
}

// Set the length of s; capacity for n bytes must already be reserved (str_reserve
// / str_assign allocate cap+1, so byte[n] holds the NUL terminator).
void str_set_len(rt_string* s, uint64_t n) {
    if (str_is_heap(s)) {
        StrHeap h = str_heap(s);
        h.ptr[n]  = 0;
        h.len     = n;
        str_set_heap(s, h);
        return;
    }
    s->storage[n]  = 0;
    s->storage[31] = static_cast<unsigned char>(str_inline_cap - n);
}

char str_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

char str_ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// Scratch buffer for building concat/repl results before the alias-safe final
// assign: stack storage for small results, heap beyond.
struct StrScratch {
    char  stack[256];
    char* buf;

    explicit StrScratch(uint64_t n, const rt_alloc_site* site) {
        buf = n <= sizeof(stack) ? stack
                                 : static_cast<char*>(rt_alloc_checked(n, site, rt_diag_now()));
    }
    ~StrScratch() {
        if (buf != stack) {
            std::free(buf);
        }
    }
    StrScratch(const StrScratch&)            = delete;
    StrScratch& operator=(const StrScratch&) = delete;
};

} // namespace

extern "C" void rt_string_init(rt_string* s) {
    s->storage[0]  = 0;              // NUL terminator for the empty string
    s->storage[31] = str_inline_cap; // Inline form, length 0.
}

extern "C" void rt_string_deinit(rt_string* s) {
    if (str_is_heap(s)) {
        std::free(str_heap(s).ptr);
    }
    s->storage[0]  = 0;
    s->storage[31] = str_inline_cap;
}

extern "C" void rt_string_assign(rt_string* dst, const rt_string* src, const rt_alloc_site* site) {
    if (dst == src) {
        return;
    }
    str_assign(dst, str_data(src), str_len(src), site);
}

extern "C" void rt_string_move(rt_string* dst, rt_string* src) {
    if (dst == src) {
        return;
    }
    // Ownership transfer, not a copy: release dst's own buffer, then take src's
    // handle image whole. The 32 bytes encode either form -- inline text or a
    // heap {ptr,len,cap} -- so one memcpy moves the value and, with it, ownership
    // of any buffer. src is re-initialized rather than left aliasing that buffer,
    // so its own later deinit stays correct and the moved value is freed once.
    // No epoch: a string handle has no interior pointer for a ref to hold.
    rt_string_deinit(dst);
    std::memcpy(dst->storage, src->storage, sizeof(src->storage));
    rt_string_init(src);
}

extern "C" void rt_string_assign_bytes(rt_string* dst, const char* p, uint64_t n, const rt_alloc_site* site) {
    str_assign(dst, p, n, site);
}

extern "C" uint64_t rt_string_len(const rt_string* s) {
    return str_len(s);
}

extern "C" const char* rt_string_data(const rt_string* s) {
    return str_data(s);
}

extern "C" void rt_string_concat(rt_string* dst, const rt_string* const* parts, uint32_t n, const rt_alloc_site* site) {
    // Aliasing exists only as identical handle pointers: distinct handles never
    // share a buffer (every assignment deep-copies). Scan for dst among the
    // parts; anything but a prefix occurrence falls back to a scratch build.
    uint64_t total  = 0;
    uint32_t dst_at = n;
    for (uint32_t k = 0; k < n; ++k) {
        total += str_len(parts[k]);
        if (dst_at == n && parts[k] == dst) {
            dst_at = k;
        }
    }
    if (dst_at == n || dst_at == 0) {
        // In-place: dst's current bytes are either unused (dst is not a part)
        // or already the result's prefix (dst is part 0, the s = {s, ...}
        // append). Writes land at offsets >= that prefix, so later parts equal
        // to dst still read their original bytes ({s, s} self-append); dst's
        // stored length is only updated after the copies, so their str_len is
        // the pre-call value too.
        const uint64_t skip = dst_at == 0 ? str_len(dst) : 0;
        str_reserve(dst, total, site);
        char*    out = str_data(dst);
        uint64_t off = skip;
        for (uint32_t k = dst_at == 0 ? 1 : 0; k < n; ++k) {
            const uint64_t len = str_len(parts[k]);
            std::memcpy(out + off, str_data(parts[k]), len);
            off += len;
        }
        str_set_len(dst, total);
        return;
    }
    // dst appears at a later position (s = {c, s}): its bytes sit mid-result,
    // so prefix writes would clobber them. Build in scratch.
    StrScratch scratch(total, site);
    uint64_t   off = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint64_t len = str_len(parts[k]);
        std::memcpy(scratch.buf + off, str_data(parts[k]), len);
        off += len;
    }
    str_assign(dst, scratch.buf, total, site);
}

extern "C" void rt_string_repl(rt_string* dst, const rt_string* src, int64_t count, const rt_alloc_site* site) {
    // A negative multiplier is an illegal value under LRM 11.4.12.1 (zero is the
    // clause's one blessed edge case and falls through to the empty string), and
    // it can only arrive from the RUNTIME-multiplier string deviation — a
    // constant one was refused at elaboration. Checked before the size product,
    // which would otherwise see it zero-extended as a huge element count.
    if (count < 0) {
        rt_string_repl_invalid(count, /*unknown=*/0, site, rt_diag_now());
    }
    const uint64_t n   = static_cast<uint64_t>(count);
    const uint64_t len = str_len(src);
    // `count` is a design-controlled replication factor ({N{s}}), so the byte
    // total is the one product here that a program can push past 2^64. A wrapping
    // one would size the scratch buffer from the wrapped value and then write
    // `count` copies into it.
    const uint64_t total = rt_alloc_elems_size(n, len, site, rt_diag_now());
    StrScratch     scratch(total, site);
    for (uint64_t k = 0; k < n; ++k) {
        std::memcpy(scratch.buf + k * len, str_data(src), len);
    }
    str_assign(dst, scratch.buf, total, site);
}

extern "C" int32_t rt_string_cmp(const rt_string* a, const rt_string* b) {
    const uint64_t la = str_len(a);
    const uint64_t lb = str_len(b);
    const uint64_t n  = la < lb ? la : lb;
    const int      c  = n != 0 ? std::memcmp(str_data(a), str_data(b), n) : 0;
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

extern "C" int32_t rt_string_icmp(const rt_string* a, const rt_string* b) {
    const uint64_t la = str_len(a);
    const uint64_t lb = str_len(b);
    const uint64_t n  = la < lb ? la : lb;
    const char*    pa = str_data(a);
    const char*    pb = str_data(b);
    for (uint64_t k = 0; k < n; ++k) {
        const char ca = str_ascii_lower(pa[k]);
        const char cb = str_ascii_lower(pb[k]);
        if (ca != cb) {
            return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
        }
    }
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

extern "C" void rt_string_substr(rt_string* dst, const rt_string* src, int32_t i, int32_t j,
                                 const rt_alloc_site* site) {
    const uint64_t len = str_len(src);
    if (i < 0 || j < i || static_cast<uint64_t>(j) >= len) {
        str_assign(dst, nullptr, 0, site);
        return;
    }
    str_assign(dst, str_data(src) + i, static_cast<uint64_t>(j) - static_cast<uint64_t>(i) + 1, site);
}

extern "C" void rt_enum_name(rt_string* dst, const uint8_t* val, uint32_t vbytes, uint32_t count,
                             const uint8_t* values, const char* const* names, const uint32_t* lens,
                             uint32_t width, uint32_t fmt_fallback, uint32_t is_signed,
                             const rt_alloc_site* site) {
    for (uint32_t k = 0; k < count; ++k) {
        if (std::memcmp(val, values + static_cast<size_t>(k) * vbytes, vbytes) == 0) {
            str_assign(dst, names[k], lens[k], site);
            return;
        }
    }
    if (fmt_fallback != 0) {
        // LRM 21.2.1.6: "Otherwise the value shall print according to the base type
        // of the enumeration" — under %p that is the ordinary integral leaf, i.e. the
        // base type printed as it would be unformatted: decimal (21.2.1.1), signed
        // when the base type is. Only the must plane reaches here, so an x/z-valued
        // enum prints its must bits.
        clotho::FormatSpec spec{};
        spec.ok        = true;
        spec.conv      = 'd';
        spec.has_width = true; // width 0 => minimal digits (the %0d form)

        // render_arg reads all three planes, so the unknown planes are materialized
        // as zeros rather than left null.
        const uint32_t        words = plane_words(width != 0 ? width : 1);
        std::vector<uint64_t> mv(words, 0);
        const std::vector<uint64_t> zero(words, 0);
        std::memcpy(mv.data(), val, std::min<size_t>(vbytes, words * sizeof(uint64_t)));
        std::string  s;
        const rt_arg a{ width, is_signed != 0 ? 1u : 0u, mv.data(), zero.data(), zero.data() };
        render_arg(s, a, spec);
        str_assign(dst, s.data(), static_cast<uint64_t>(s.size()), site);
        return;
    }
    str_assign(dst, nullptr, 0, site); // No member matched: empty string (LRM 6.19.2).
}

extern "C" void rt_string_toupper(rt_string* dst, const rt_string* src, const rt_alloc_site* site) {
    rt_string_assign(dst, src, site); // No-op when dst == src, then transform in place.
    char*          p = str_data(dst);
    const uint64_t n = str_len(dst);
    for (uint64_t k = 0; k < n; ++k) {
        p[k] = str_ascii_upper(p[k]);
    }
}

extern "C" void rt_string_tolower(rt_string* dst, const rt_string* src, const rt_alloc_site* site) {
    rt_string_assign(dst, src, site);
    char*          p = str_data(dst);
    const uint64_t n = str_len(dst);
    for (uint64_t k = 0; k < n; ++k) {
        p[k] = str_ascii_lower(p[k]);
    }
}

extern "C" void rt_string_putc(rt_string* s, int32_t i, uint32_t c) {
    if (i < 0 || static_cast<uint64_t>(i) >= str_len(s) || (c & 0xffu) == 0) {
        return;
    }
    str_data(s)[i] = static_cast<char>(c & 0xffu);
}

extern "C" uint32_t rt_string_getc(const rt_string* s, int32_t i) {
    if (i < 0 || static_cast<uint64_t>(i) >= str_len(s)) {
        return 0;
    }
    return static_cast<unsigned char>(str_data(s)[i]);
}
namespace {

// Shared digit-scan for the atoi family (LRM §6.16.9): consume leading digits
// (and '_' separators) of `base`, stopping at the first other character or
// end of string. No leading whitespace or sign handling — the clause
// explicitly excludes them ("does not parse the full syntax for integer
// literals"). Verified against iverilog. Underscore
// interleaving rules out std::from_chars here; overflow wraps silently via
// plain unsigned 32-bit accumulation, matching the clause's own truncation
// note.
int32_t str_atoi_base(const rt_string* s, uint32_t base) {
    const char*    p     = str_data(s);
    const uint64_t n     = str_len(s);
    uint32_t       acc   = 0;
    bool           found = false;
    for (uint64_t k = 0; k < n; ++k) {
        const char c = p[k];
        if (c == '_') {
            continue;
        }
        uint32_t v;
        if (c >= '0' && c <= '9') {
            v = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'z') {
            v = static_cast<uint32_t>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'Z') {
            v = static_cast<uint32_t>(c - 'A') + 10;
        } else {
            break;
        }
        if (v >= base) {
            break;
        }
        acc   = acc * base + v;
        found = true;
    }
    return found ? static_cast<int32_t>(acc) : 0;
}

// dst = sign + magnitude of `i` in `base`, lowercase digits (LRM §6.16.11-
// 6.16.14). std::to_chars natively formats a negative signed integer as '-'
// followed by the magnitude in any base, matching iverilog's convention for
// hextoa/octtoa/bintoa (docs/LRM_NOTES.md, note 1) — not a two's-complement bit-pattern
// dump.
void str_toa_base(rt_string* dst, int32_t i, int base, const rt_alloc_site* site) {
    char       buf[36]; // sign + up to 32 binary digits
    const auto res = std::to_chars(buf, buf + sizeof(buf), i, base);
    str_assign(dst, buf, static_cast<uint64_t>(res.ptr - buf), site);
}

} // namespace
namespace rt_internal {

// The shortest decimal that reads back as `d`, laid out as std::to_chars(first,
// last, d) lays it out (C++ [charconv.to.chars]): fixed or scientific
// (`d.ddde+XX`), whichever has fewer characters, a tie going to fixed, and a
// fixed integer printed exactly as %.0f would. Written out because
// floating-point std::to_chars lives in the C++ library's compiled part, which
// an executable's runtime does not link (docs/design/executables.md); fmt's `{}`
// supplies the same shortest digits. Checked byte-identical to std::to_chars on
// 26M doubles: random bit patterns, decimals, powers of two and widened floats.
// Returns the length; `out` holds at least 32 bytes.
size_t shortest_double(char* out, double d) {
    char* p = out;
    if (std::signbit(d)) {
        *p++ = '-';
    }
    if (std::isnan(d) || std::isinf(d)) {
        std::memcpy(p, std::isnan(d) ? "nan" : "inf", 3);
        return static_cast<size_t>(p + 3 - out);
    }
    if (d == 0.0) {
        *p++ = '0';
        return static_cast<size_t>(p - out);
    }
    // fmt's shortest form: "0.00123", "1.5e-07", "123456.0" and so on. Only its
    // digits and where its decimal point falls are used.
    char         rep[40];
    const size_t n = fmt::format_to_n(rep, sizeof(rep) - 1, "{}", std::fabs(d)).size;
    rep[n]         = '\0';
    char digits[32];
    int  nd    = 0;
    int  point = -1;
    int  exp10 = 0;
    for (size_t i = 0; i < n; ++i) {
        if (rep[i] == '.') {
            point = nd;
        } else if (rep[i] == 'e') {
            exp10 = std::atoi(rep + i + 1);
            break;
        } else {
            digits[nd++] = rep[i];
        }
    }
    if (point < 0) {
        point = nd;
    }
    int lead = 0;
    while (digits[lead] == '0') {
        ++lead;
    }
    int last = nd;
    while (digits[last - 1] == '0') {
        --last;
    }
    const char* sig       = digits + lead;
    const int   ns        = last - lead;
    const int   e         = point + exp10 - lead - 1; // the power of ten of sig[0]
    const int   abs_e     = e < 0 ? -e : e;
    const int   sci_len   = 1 + (ns > 1 ? ns : 0) + 2 + (abs_e >= 100 ? 3 : 2);
    const int   fixed_len = e >= ns - 1 ? e + 1 : e >= 0 ? ns + 1 : ns + 1 - e;
    if (fixed_len > sci_len) {
        *p++ = sig[0];
        if (ns > 1) {
            *p++ = '.';
            std::memcpy(p, sig + 1, static_cast<size_t>(ns - 1));
            p += ns - 1;
        }
        *p++ = 'e';
        *p++ = e < 0 ? '-' : '+';
        if (abs_e >= 100) {
            *p++ = static_cast<char>('0' + abs_e / 100);
        }
        *p++ = static_cast<char>('0' + abs_e / 10 % 10);
        *p++ = static_cast<char>('0' + abs_e % 10);
    } else if (e >= ns - 1) {
        p = fmt::format_to(p, "{:.0f}", std::fabs(d));
    } else if (e >= 0) {
        std::memcpy(p, sig, static_cast<size_t>(e + 1));
        p += e + 1;
        *p++ = '.';
        std::memcpy(p, sig + e + 1, static_cast<size_t>(ns - e - 1));
        p += ns - e - 1;
    } else {
        *p++ = '0';
        *p++ = '.';
        std::memset(p, '0', static_cast<size_t>(-e - 1));
        p += -e - 1;
        std::memcpy(p, sig, static_cast<size_t>(ns));
        p += ns;
    }
    return static_cast<size_t>(p - out);
}

} // namespace rt_internal
namespace {

#if defined(_WIN32)
_locale_t g_c_locale = nullptr;
#else
locale_t g_c_locale = nullptr;
#endif

// strtod in the "C" locale, whatever setlocale a DPI library may have called.
double strtod_c(const char* s) {
#if defined(_WIN32)
    if (g_c_locale == nullptr) {
        g_c_locale = _create_locale(LC_ALL, "C");
    }
    return _strtod_l(s, nullptr, g_c_locale);
#else
    if (g_c_locale == nullptr) {
        g_c_locale = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    }
    return strtod_l(s, nullptr, g_c_locale);
#endif
}

// std::from_chars(first, last, value) for a double whose `value` starts at 0.0,
// the contract rt_string_atoreal was written against: the longest prefix that is
// a decimal floating literal ([-]digits[.digits][e[+-]digits], no hex, no
// leading '+' or whitespace) or [-]inf, infinity, nan or nan(chars) in any case,
// rounded correctly; 0.0 when there is no such prefix or its value overflows or
// underflows to zero. Written out for the same reason as shortest_double. The
// conversion is the C library's, given only the accepted prefix: strtod alone
// would read "0x10" as hex, and turn "1e400" into infinity.
double parse_double(const char* first, const char* last) {
    const char* p = first;
    if (p < last && *p == '-') {
        ++p;
    }
    // The length of `word` (lowercase letters) at p, in any case, or 0.
    const auto word = [&](const char* w) -> size_t {
        size_t k = 0;
        while (w[k] != '\0' && p + k < last && (p[k] | 0x20) == w[k]) {
            ++k;
        }
        return w[k] == '\0' ? k : 0;
    };
    const auto  digit = [](char c) { return c >= '0' && c <= '9'; };
    const char* end   = nullptr;
    if (const size_t k = word("infinity"); k != 0) {
        end = p + k;
    } else if (const size_t k = word("inf"); k != 0) {
        end = p + k;
    } else if (const size_t k = word("nan"); k != 0) {
        end = p + k;
        if (end < last && *end == '(') {
            const char* q = end + 1;
            while (q < last && (digit(*q) || ((*q | 0x20) >= 'a' && (*q | 0x20) <= 'z') || *q == '_')) {
                ++q;
            }
            if (q < last && *q == ')') {
                end = q + 1;
            }
        }
    } else {
        const char* q      = p;
        bool        digits = false;
        while (q < last && digit(*q)) {
            ++q;
            digits = true;
        }
        if (q < last && *q == '.') {
            ++q;
            while (q < last && digit(*q)) {
                ++q;
                digits = true;
            }
        }
        if (!digits) {
            return 0.0;
        }
        end = q;
        if (q < last && (*q | 0x20) == 'e') {
            const char* x = q + 1;
            if (x < last && (*x == '+' || *x == '-')) {
                ++x;
            }
            if (x < last && digit(*x)) {
                while (x < last && digit(*x)) {
                    ++x;
                }
                end = x;
            }
        }
    }
    const size_t len = static_cast<size_t>(end - first);
    char         small[64];
    char*        buf = len < sizeof(small) ? small : static_cast<char*>(std::malloc(len + 1));
    std::memcpy(buf, first, len);
    buf[len]           = '\0';
    errno              = 0;
    const double v     = strtod_c(buf);
    const bool   range = errno == ERANGE;
    if (buf != small) {
        std::free(buf);
    }
    // from_chars reports overflow, and underflow to zero, as out of range and
    // leaves `value` alone; a subnormal result is in range.
    return range && (v == 0.0 || std::isinf(v)) ? 0.0 : v;
}

} // namespace

extern "C" int32_t rt_string_atoi(const rt_string* s) {
    return str_atoi_base(s, 10);
}
extern "C" int32_t rt_string_atohex(const rt_string* s) {
    return str_atoi_base(s, 16);
}
extern "C" int32_t rt_string_atooct(const rt_string* s) {
    return str_atoi_base(s, 8);
}
extern "C" int32_t rt_string_atobin(const rt_string* s) {
    return str_atoi_base(s, 2);
}

extern "C" double rt_string_atoreal(const rt_string* s) {
    const char* p   = str_data(s);
    const char* end = p + str_len(s);
    // Unlike the atoi family, atoreal accepts leading whitespace and a '+'
    // sign (verified against iverilog); the real-literal parse accepts neither,
    // so both are skipped by hand first.
    while (p < end && scan_ws(static_cast<unsigned char>(*p))) {
        ++p;
    }
    if (p < end && *p == '+') {
        ++p;
    }
    return parse_double(p, end);
}

extern "C" void rt_string_itoa(rt_string* dst, int32_t i, const rt_alloc_site* site) {
    str_toa_base(dst, i, 10, site);
}
extern "C" void rt_string_hextoa(rt_string* dst, int32_t i, const rt_alloc_site* site) {
    str_toa_base(dst, i, 16, site);
}
extern "C" void rt_string_octtoa(rt_string* dst, int32_t i, const rt_alloc_site* site) {
    str_toa_base(dst, i, 8, site);
}
extern "C" void rt_string_bintoa(rt_string* dst, int32_t i, const rt_alloc_site* site) {
    str_toa_base(dst, i, 2, site);
}

extern "C" void rt_string_realtoa(rt_string* dst, double r, const rt_alloc_site* site) {
    char         buf[32];
    const size_t len = shortest_double(buf, r);
    str_assign(dst, buf, len, site);
}

extern "C" void rt_string_from_packed(rt_string* dst, const uint8_t* val, uint32_t width_bits,
                                      const rt_alloc_site* site) {
    const uint32_t nbytes = (width_bits + 7) / 8;
    StrScratch     scratch(nbytes, site);
    uint64_t       out_len = 0;
    for (uint32_t k = nbytes; k-- > 0;) {
        uint8_t c = val[k];
        if (k == nbytes - 1 && width_bits % 8 != 0) {
            c &= static_cast<uint8_t>((1u << (width_bits % 8)) - 1);
        }
        if (c != 0) {
            scratch.buf[out_len++] = static_cast<char>(c);
        }
    }
    str_assign(dst, scratch.buf, out_len, site);
}

// Append a packed value's bytes to `dst`, most significant byte first: one
// operand of a streaming concatenation whose destination is a string
// (LRM 11.4.14). Same 8-bit-chunk rule as rt_string_from_packed, including the
// dropped 0x00 chunk (LRM 6.16), because a stream reaching a string destination
// converts through exactly that rule -- slang's constant evaluator routes it
// through ConstantValue::convertToStr(), so a folded `localparam string P =
// {>>{...}}` and this runtime append must agree, and they only do if both drop.
extern "C" void rt_string_append_packed(rt_string* dst, const uint8_t* val, uint32_t width_bits,
                                        const rt_alloc_site* site) {
    const uint32_t nbytes = (width_bits + 7) / 8;
    const uint64_t base   = str_len(dst);
    str_reserve(dst, base + nbytes, site);
    char*    out = str_data(dst);
    uint64_t len = base;
    for (uint32_t k = 0; k < nbytes; ++k) {
        uint8_t c = val[nbytes - 1 - k];
        if (k == 0 && width_bits % 8 != 0) {
            c &= static_cast<uint8_t>((1u << (width_bits % 8)) - 1);
        }
        if (c != 0) {
            out[len++] = static_cast<char>(c);
        }
    }
    str_set_len(dst, len);
}

extern "C" void rt_string_to_packed(const rt_string* s, uint8_t* out, uint32_t width_bits) {
    // Buffer sizing mirrors ir::plane_bytes (InternPool.h) — the backend allocas
    // exactly one val plane and loads an iW from it.
    uint32_t buf_bytes = 0;
    if (width_bits <= 8) {
        buf_bytes = 1;
    } else if (width_bits <= 16) {
        buf_bytes = 2;
    } else if (width_bits <= 32) {
        buf_bytes = 4;
    } else if (width_bits <= 64) {
        buf_bytes = 8;
    } else {
        buf_bytes = ((width_bits + 63) / 64) * 8;
    }
    std::memset(out, 0, buf_bytes);

    const uint32_t nbytes = (width_bits + 7) / 8;
    const uint64_t len    = str_len(s);
    const char*    p      = str_data(s);
    const uint32_t take   = len < nbytes ? static_cast<uint32_t>(len) : nbytes;
    for (uint32_t k = 0; k < take; ++k) {
        out[k] = static_cast<uint8_t>(p[len - 1 - k]);
    }
    if (width_bits % 8 != 0) {
        out[nbytes - 1] &= static_cast<uint8_t>((1u << (width_bits % 8)) - 1);
    }
}
