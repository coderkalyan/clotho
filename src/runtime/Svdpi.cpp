// DPI-C helper API (IEEE 1800-2023 Annex H) implementation — the packed-vector
// bit/part-select accessors DPI C calls to manipulate svBitVecVal / svLogicVecVal
// without poking the raw layout. Pure operations on the canonical representation
// (the same layout Clotho marshals for bit[N]/logic[N] DPI args), so this depends
// on nothing in the compiler. Exposed to JIT'd designs / DPI objects as absolute
// symbols in Jit.cpp. See src/runtime/svdpi.h.

#include "svdpi.h"

#include <cstdint>
#include <string>
#include <unordered_map>

// 4-state per-bit encoding (svdpi.h): aval = value-or-X, bval = unknown.
//   svLogic 0 = (a0,b0)   1 = (a1,b0)   Z(2) = (a0,b1)   X(3) = (a1,b1)
// so svLogic == aval_bit | (bval_bit << 1).

// -- svScope (LRM 35.5.3): single-instance model --
//
// Clotho has one flat design state, so there is exactly one DPI scope. It carries
// the design's hierarchical name and a per-key C user-data map (svPutUserData /
// svGetUserData). `g_current` is the active scope svGetScope returns and svSetScope
// swaps — trivial with a single scope, but a context import may save/restore it
// around export calls. rt_svdpi_scope_init (called from main when the design uses
// DPI) sets the name and resets state per simulation. Multi-instance scope
// (mapping a handle to an instance's state sub-region) is deferred with svScope
// export.

namespace {
struct SvScopeObj {
    std::string                      name;
    std::unordered_map<void*, void*> user_data;
};
SvScopeObj g_scope;
void*      g_current = nullptr;
} // namespace

extern "C" void rt_svdpi_scope_init(const char* name, uint32_t name_len) {
    g_scope.name.assign(name, name_len);
    g_scope.user_data.clear();
    g_current = &g_scope;
}

extern "C" {

svScope svGetScope(void) {
    if (g_current == nullptr) {
        g_current = &g_scope; // defensive: a design that did not call the init
    }
    return g_current;
}

svScope svSetScope(const svScope scope) {
    svScope old = g_current;
    g_current   = scope;
    return old;
}

const char* svGetNameFromScope(const svScope scope) {
    return static_cast<const SvScopeObj*>(scope)->name.c_str();
}

svScope svGetScopeFromName(const char* scopeName) {
    return g_scope.name == scopeName ? &g_scope : nullptr;
}

int svPutUserData(const svScope scope, void* userKey, void* userData) {
    static_cast<SvScopeObj*>(scope)->user_data[userKey] = userData;
    return 0;
}

void* svGetUserData(const svScope scope, void* userKey) {
    auto&      m  = static_cast<SvScopeObj*>(scope)->user_data;
    const auto it = m.find(userKey);
    return it != m.end() ? it->second : nullptr;
}

const char* svDpiVersion(void) {
    return "1800-2023";
}

svBit svGetBitselBit(const svBitVecVal* s, int i) {
    return static_cast<svBit>((s[i >> 5] >> (i & 31)) & 1u);
}

svLogic svGetBitselLogic(const svLogicVecVal* s, int i) {
    const svLogicVecVal& c = s[i >> 5];
    const uint32_t       a = (c.aval >> (i & 31)) & 1u;
    const uint32_t       b = (c.bval >> (i & 31)) & 1u;
    return static_cast<svLogic>(a | (b << 1));
}

void svPutBitselBit(svBitVecVal* d, int i, svBit s) {
    const uint32_t m = 1u << (i & 31);
    if (s & 1u) {
        d[i >> 5] |= m;
    } else {
        d[i >> 5] &= ~m;
    }
}

void svPutBitselLogic(svLogicVecVal* d, int i, svLogic s) {
    svLogicVecVal& c = d[i >> 5];
    const uint32_t m = 1u << (i & 31);
    if (s & 1u) {
        c.aval |= m;
    } else {
        c.aval &= ~m;
    }
    if (s & 2u) {
        c.bval |= m;
    } else {
        c.bval &= ~m;
    }
}

// Part-selects are defined bit-wise via the bit-selects above, so any width and
// any (source, destination) alignment work with one correct implementation. Get
// right-justifies s[i +: w] into *d (bits 0..w-1); put writes the low w bits of
// the one-word source `s` into *d at bit i.

void svGetPartselBit(svBitVecVal* d, const svBitVecVal* s, int i, int w) {
    for (int k = 0; k < w; ++k) {
        svPutBitselBit(d, k, svGetBitselBit(s, i + k));
    }
}

void svGetPartselLogic(svLogicVecVal* d, const svLogicVecVal* s, int i, int w) {
    for (int k = 0; k < w; ++k) {
        svPutBitselLogic(d, k, svGetBitselLogic(s, i + k));
    }
}

void svPutPartselBit(svBitVecVal* d, const svBitVecVal s, int i, int w) {
    for (int k = 0; k < w; ++k) {
        svPutBitselBit(d, i + k, static_cast<svBit>((s >> k) & 1u));
    }
}

void svPutPartselLogic(svLogicVecVal* d, const svLogicVecVal s, int i, int w) {
    for (int k = 0; k < w; ++k) {
        svPutBitselLogic(d, i + k, svGetBitselLogic(&s, k));
    }
}

} // extern "C"
