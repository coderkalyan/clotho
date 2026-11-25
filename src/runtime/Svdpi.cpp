// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DPI-C helper API (IEEE 1800-2023 Annex H) implementation — the packed-vector
// bit/part-select accessors DPI C calls to manipulate svBitVecVal / svLogicVecVal
// without poking the raw layout. Pure operations on the canonical representation
// (the same layout Clotho marshals for bit[N]/logic[N] DPI args), so this depends
// on nothing in the compiler. Exposed to JIT'd designs / DPI objects as absolute
// symbols in Jit.cpp. See src/runtime/svdpi.h.

#include "svdpi.h"

#include "Runtime.h" // rt_svopen

#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>

#include <ankerl/unordered_dense.h>

// 4-state per-bit encoding (svdpi.h): aval = value-or-X, bval = unknown.
//   svLogic 0 = (a0,b0)   1 = (a1,b0)   Z(2) = (a0,b1)   X(3) = (a1,b1)
// so svLogic == aval_bit | (bval_bit << 1).

// -- svScope (LRM 35.5.3): one scope per DPI scope --
//
// A scope carries the hierarchical name of a top, of an instance or generate block
// that exports a subroutine or declares a `context` import, or of a package or `$unit`
// that does; its position in the registration order, which is what the generated code
// names it by (rt_svdpi_scope_enter); and a per-key C user-data map (svPutUserData /
// svGetUserData). `g_current` is the scope svGetScope returns and svSetScope swaps: a
// call of a context import makes the import's scope current and puts the caller's back
// when it returns, and an export several scopes share reads it to pick its instance.
// main registers the scopes (rt_svdpi_scope_register) before any process runs, and
// `index == 0` clears the table first so a second simulation in the same process starts
// with fresh user data. A svScope handle IS an SvScopeObj*, hence the deque: registering
// a further scope must not move the handles already handed out.

namespace {
struct SvScopeObj {
    std::string                                name;
    uint32_t                                   index = 0;
    ankerl::unordered_dense::map<void*, void*> user_data;
};
std::deque<SvScopeObj> g_scopes;
// The scopes by name; a name two scopes share (a package and an instance both called
// `p`) resolves to the one registered first, which is a top's before any other.
ankerl::unordered_dense::map<std::string_view, uint32_t> g_by_name;
void*                                                    g_current = nullptr;
} // namespace

extern "C" void rt_svdpi_scope_register(const char* name, uint32_t name_len, uint32_t index) {
    if (index == 0) {
        g_scopes.clear();
        g_by_name.clear();
        g_current = nullptr;
    }
    SvScopeObj& scope = g_scopes.emplace_back();
    scope.name.assign(name, name_len);
    scope.index = static_cast<uint32_t>(g_scopes.size() - 1);
    assert(scope.index == index && "scopes are registered in index order");
    // A deque never moves its elements, so a view of a name outlives later registrations.
    g_by_name.try_emplace(scope.name, scope.index);
    if (g_current == nullptr) {
        g_current = &g_scopes.front(); // the first top registered is the default
    }
}

extern "C" void* rt_svdpi_scope_enter(uint32_t index) {
    assert(index < g_scopes.size() && "the backend registers every scope a call enters");
    void* const previous = g_current;
    g_current            = &g_scopes[index];
    return previous;
}

extern "C" void rt_svdpi_scope_leave(void* previous) {
    g_current = previous;
}

extern "C" uint32_t rt_svdpi_scope_index(void) {
    return g_current == nullptr ? UINT32_MAX : static_cast<const SvScopeObj*>(g_current)->index;
}

extern "C" const char* rt_svdpi_scope_name(void) {
    return g_current == nullptr ? nullptr : static_cast<const SvScopeObj*>(g_current)->name.c_str();
}

extern "C" {

svScope svGetScope(void) {
    if (g_scopes.empty()) {
        g_scopes.emplace_back(); // defensive: a design that registered nothing
        g_current = &g_scopes.front();
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
    if (scopeName == nullptr) {
        return nullptr;
    }
    const auto it = g_by_name.find(std::string_view(scopeName));
    return it != g_by_name.end() ? &g_scopes[it->second] : nullptr;
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

// -- Open arrays (LRM 35.5.6.2): 1-D svOpenArrayHandle descriptor accessors --
//
// The element pack/unpack marshal is emitted inline as LLVM IR at the DPI call site
// (emit_dpi_oa_pack_ir / emit_dpi_oa_unpack_ir in the backend), operating on the
// native iW/i2W design slot at any width; the runtime only provides these accessors
// the DPI C calls on the resulting descriptor.

namespace {

// SV index -> linear element position 0..nelem-1 (handles descending [left:right]).
uint32_t open_pos(const rt_svopen* a, int i) {
    return static_cast<uint32_t>(a->left <= a->right ? (i - a->left) : (a->left - i));
}

} // namespace

extern "C" {

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
// An empty array (a 0-length dynamic array / queue) has right == left - 1, which the
// min/max form would misread as a descending range and hand back an inverted
// [high >= low] span — so a `for (i = svLow; i <= svHigh; i++)` C loop would deref a
// null buffer. Return low > high (0 > -1) for the empty case so the loop is empty.
int svLow(const svOpenArrayHandle h, int) {
    const auto* a = static_cast<const rt_svopen*>(h);
    if (a->nelem == 0) return a->left;
    return a->left < a->right ? a->left : a->right;
}
int svHigh(const svOpenArrayHandle h, int) {
    const auto* a = static_cast<const rt_svopen*>(h);
    if (a->nelem == 0) return a->left - 1;
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
// The element vecval copies span every chunk (elem_bytes = nchunks * sizeof(chunk)),
// so a wide (33..64-bit) element's two svVecVal words move together.
void svGetBitArrElem1VecVal(svBitVecVal* value, const svOpenArrayHandle h, int indx1) {
    const auto* a = static_cast<const rt_svopen*>(h);
    std::memcpy(value, svGetArrElemPtr1(h, indx1), a->elem_bytes);
}
void svPutBitArrElem1VecVal(const svOpenArrayHandle h, svBitVecVal* value, int indx1) {
    const auto* a = static_cast<const rt_svopen*>(h);
    std::memcpy(svGetArrElemPtr1(h, indx1), value, a->elem_bytes);
}
void svGetLogicArrElem1VecVal(svLogicVecVal* value, const svOpenArrayHandle h, int indx1) {
    const auto* a = static_cast<const rt_svopen*>(h);
    std::memcpy(value, svGetArrElemPtr1(h, indx1), a->elem_bytes);
}
void svPutLogicArrElem1VecVal(const svOpenArrayHandle h, svLogicVecVal* value, int indx1) {
    const auto* a = static_cast<const rt_svopen*>(h);
    std::memcpy(svGetArrElemPtr1(h, indx1), value, a->elem_bytes);
}

} // extern "C"
