// DPI-C helper API (IEEE 1800-2023 Annex H) implementation — the packed-vector
// bit/part-select accessors DPI C calls to manipulate svBitVecVal / svLogicVecVal
// without poking the raw layout. Pure operations on the canonical representation
// (the same layout Clotho marshals for bit[N]/logic[N] DPI args), so this depends
// on nothing in the compiler. Exposed to JIT'd designs / DPI objects as absolute
// symbols in Jit.cpp. See src/runtime/svdpi.h.

#include "svdpi.h"

#include "Runtime.h" // rt_svopen + the array marshal declarations

#include <cstdint>
#include <cstring>
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

// -- Open arrays (LRM 35.5.6.2): 1-D descriptor accessors + element marshal --

namespace {

// SV index -> linear element position 0..nelem-1 (handles descending [left:right]).
uint32_t open_pos(const rt_svopen* a, int i) {
    return static_cast<uint32_t>(a->left <= a->right ? (i - a->left) : (a->left - i));
}

// A W-bit-per-element pack/unpack between Clotho's `[n x slot]` design storage and
// a contiguous svVecVal buffer. Templated on the concrete slot integer types so
// each is a fixed-stride strided-load loop with branch-free arithmetic (the -O2
// runtime autovectorizes it). W <= 32 (one svVecVal word).
// `HasZ` is a template parameter so each instantiation is a straight-line,
// branch-free loop the -O2 runtime autovectorizes. A never-z element (its z word
// elided by may-z analysis, so the slot is `{ i2W }` with no z field) instantiates
// HasZ=false: z is the constant 0 and the z read is compiled out entirely.
template <typename MM, typename Z, bool HasZ>
void pack_logic(const uint8_t* b, uint32_t stride, uint32_t z_off, uint32_t w, uint32_t n,
                svLogicVecVal* o) {
    const uint32_t mask = w >= 32 ? 0xffffffffu : ((1u << w) - 1);
    for (uint32_t k = 0; k < n; ++k) {
        MM mm;
        std::memcpy(&mm, b + static_cast<size_t>(k) * stride, sizeof(MM));
        uint32_t z = 0;
        if constexpr (HasZ) {
            Z zc;
            std::memcpy(&zc, b + static_cast<size_t>(k) * stride + z_off, sizeof(Z));
            z = static_cast<uint32_t>(zc);
        }
        const uint32_t must = static_cast<uint32_t>(mm) & mask;
        const uint32_t may  = static_cast<uint32_t>(static_cast<uint64_t>(mm) >> w) & mask;
        o[k].aval = may & ~z;
        o[k].bval = must ^ may;
    }
}

template <typename MM, typename Z, bool HasZ>
void unpack_logic(const svLogicVecVal* in, uint8_t* b, uint32_t stride, uint32_t z_off, uint32_t w,
                  uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t aval = in[k].aval;
        const uint32_t bval = in[k].bval;
        const MM       mm =
          static_cast<MM>(static_cast<uint64_t>(aval & ~bval) | (static_cast<uint64_t>(aval | bval) << w));
        std::memcpy(b + static_cast<size_t>(k) * stride, &mm, sizeof(MM));
        if constexpr (HasZ) {
            const Z z = static_cast<Z>(bval & ~aval);
            std::memcpy(b + static_cast<size_t>(k) * stride + z_off, &z, sizeof(Z));
        }
    }
}

template <typename V>
void pack_bit(const uint8_t* b, uint32_t stride, uint32_t w, uint32_t n, svBitVecVal* o) {
    const uint32_t mask = w >= 32 ? 0xffffffffu : ((1u << w) - 1);
    for (uint32_t k = 0; k < n; ++k) {
        V v;
        std::memcpy(&v, b + static_cast<size_t>(k) * stride, sizeof(V));
        o[k] = static_cast<uint32_t>(v) & mask;
    }
}

template <typename V>
void unpack_bit(const svBitVecVal* in, uint8_t* b, uint32_t stride, uint32_t w, uint32_t n) {
    const uint32_t mask = w >= 32 ? 0xffffffffu : ((1u << w) - 1);
    for (uint32_t k = 0; k < n; ++k) {
        const V v = static_cast<V>(in[k] & mask);
        std::memcpy(b + static_cast<size_t>(k) * stride, &v, sizeof(V));
    }
}

} // namespace

extern "C" {

void rt_dpi_pack_arr_bit(const void* base, uint32_t stride, uint32_t w, uint32_t n, void* out) {
    const auto* b = static_cast<const uint8_t*>(base);
    auto*       o = static_cast<svBitVecVal*>(out);
    if (w <= 8) {
        pack_bit<uint8_t>(b, stride, w, n, o);
    } else if (w <= 16) {
        pack_bit<uint16_t>(b, stride, w, n, o);
    } else {
        pack_bit<uint32_t>(b, stride, w, n, o);
    }
}

void rt_dpi_unpack_arr_bit(const void* in, uint32_t stride, uint32_t w, uint32_t n, void* base) {
    const auto* i = static_cast<const svBitVecVal*>(in);
    auto*       b = static_cast<uint8_t*>(base);
    if (w <= 8) {
        unpack_bit<uint8_t>(i, b, stride, w, n);
    } else if (w <= 16) {
        unpack_bit<uint16_t>(i, b, stride, w, n);
    } else {
        unpack_bit<uint32_t>(i, b, stride, w, n);
    }
}

void rt_dpi_pack_arr_logic(const void* base, uint32_t stride, uint32_t z_off, uint32_t has_z,
                           uint32_t w, uint32_t n, void* out) {
    const auto* b = static_cast<const uint8_t*>(base);
    auto*       o = static_cast<svLogicVecVal*>(out);
    if (has_z) {
        if (w <= 8)       pack_logic<uint16_t, uint8_t, true>(b, stride, z_off, w, n, o);
        else if (w <= 16) pack_logic<uint32_t, uint16_t, true>(b, stride, z_off, w, n, o);
        else              pack_logic<uint64_t, uint32_t, true>(b, stride, z_off, w, n, o);
    } else {
        if (w <= 8)       pack_logic<uint16_t, uint8_t, false>(b, stride, z_off, w, n, o);
        else if (w <= 16) pack_logic<uint32_t, uint16_t, false>(b, stride, z_off, w, n, o);
        else              pack_logic<uint64_t, uint32_t, false>(b, stride, z_off, w, n, o);
    }
}

void rt_dpi_unpack_arr_logic(const void* in, uint32_t stride, uint32_t z_off, uint32_t has_z,
                             uint32_t w, uint32_t n, void* base) {
    const auto* i = static_cast<const svLogicVecVal*>(in);
    auto*       b = static_cast<uint8_t*>(base);
    if (has_z) {
        if (w <= 8)       unpack_logic<uint16_t, uint8_t, true>(i, b, stride, z_off, w, n);
        else if (w <= 16) unpack_logic<uint32_t, uint16_t, true>(i, b, stride, z_off, w, n);
        else              unpack_logic<uint64_t, uint32_t, true>(i, b, stride, z_off, w, n);
    } else {
        if (w <= 8)       unpack_logic<uint16_t, uint8_t, false>(i, b, stride, z_off, w, n);
        else if (w <= 16) unpack_logic<uint32_t, uint16_t, false>(i, b, stride, z_off, w, n);
        else              unpack_logic<uint64_t, uint32_t, false>(i, b, stride, z_off, w, n);
    }
}

int svDimensions(const svOpenArrayHandle) {
    return 1;
}
int svSize(const svOpenArrayHandle h, int) {
    return static_cast<int>(static_cast<const rt_svopen*>(h)->nelem);
}
int svLeft(const svOpenArrayHandle h, int) {
    return static_cast<const rt_svopen*>(h)->left;
}
int svRight(const svOpenArrayHandle h, int) {
    return static_cast<const rt_svopen*>(h)->right;
}
int svLow(const svOpenArrayHandle h, int) {
    const auto* a = static_cast<const rt_svopen*>(h);
    return a->left < a->right ? a->left : a->right;
}
int svHigh(const svOpenArrayHandle h, int) {
    const auto* a = static_cast<const rt_svopen*>(h);
    return a->left > a->right ? a->left : a->right;
}
int svIncrement(const svOpenArrayHandle h, int) {
    const auto* a = static_cast<const rt_svopen*>(h);
    return a->left >= a->right ? 1 : -1;
}
int svSizeOfArray(const svOpenArrayHandle h) {
    const auto* a = static_cast<const rt_svopen*>(h);
    return static_cast<int>(a->nelem * a->elem_bytes);
}
void* svGetArrayPtr(const svOpenArrayHandle h) {
    return static_cast<const rt_svopen*>(h)->data;
}
void* svGetArrElemPtr1(const svOpenArrayHandle h, int indx1) {
    const auto* a = static_cast<const rt_svopen*>(h);
    return static_cast<char*>(a->data) + static_cast<size_t>(open_pos(a, indx1)) * a->elem_bytes;
}
void svGetBitArrElem1VecVal(svBitVecVal* value, const svOpenArrayHandle h, int indx1) {
    *value = *static_cast<const svBitVecVal*>(svGetArrElemPtr1(h, indx1));
}
void svPutBitArrElem1VecVal(const svOpenArrayHandle h, svBitVecVal* value, int indx1) {
    *static_cast<svBitVecVal*>(svGetArrElemPtr1(h, indx1)) = *value;
}
void svGetLogicArrElem1VecVal(svLogicVecVal* value, const svOpenArrayHandle h, int indx1) {
    *value = *static_cast<const svLogicVecVal*>(svGetArrElemPtr1(h, indx1));
}
void svPutLogicArrElem1VecVal(const svOpenArrayHandle h, svLogicVecVal* value, int indx1) {
    *static_cast<svLogicVecVal*>(svGetArrElemPtr1(h, indx1)) = *value;
}

} // extern "C"
