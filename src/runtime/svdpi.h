#ifndef CLOTHO_SVDPI_H
#define CLOTHO_SVDPI_H

// DPI-C helper API (IEEE 1800-2023 Annex H) — the subset Clotho's runtime
// implements: the canonical value types and the packed-vector bit/part-select
// accessors. Scope (svGetScope/svSetScope), open arrays, and the rest of svdpi
// are not yet provided; calling one is an unresolved-symbol error at JIT link
// time. Any conforming svdpi.h is equally usable — these signatures are standardized.
//
// Compile DPI C against this header (or any svdpi.h) and pass the object with
// `--dpi-lib dpi.o` (a `.o`/`.a` is JIT-linked, so the accessors resolve to the
// Clotho runtime). The accessors operate purely on the canonical layout, which is
// exactly what Clotho marshals for `bit[N]` / `logic[N]` DPI arguments.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Scalar logic values (svLogic).
#define sv_0 0
#define sv_1 1
#define sv_z 2
#define sv_x 3

typedef uint8_t  svScalar;
typedef svScalar svBit;   // 0 / 1
typedef svScalar svLogic; // 0 / 1 / 2 (Z) / 3 (X)

// 4-state packed-array chunk: aval = value-or-X bit, bval = unknown (X or Z) bit.
// Per bit: 0=(0,0) 1=(1,0) Z=(0,1) X=(1,1). Aliased to VPI's vecval so it does not
// conflict with a vpi_user.h that defines the same struct.
#ifndef VPI_VECVAL
#define VPI_VECVAL
typedef struct t_vpi_vecval {
    uint32_t aval;
    uint32_t bval;
} s_vpi_vecval, *p_vpi_vecval;
#endif
typedef s_vpi_vecval svLogicVecVal;
typedef uint32_t     svBitVecVal; // 2-state packed-array chunk

typedef void* svScope;
typedef void* svOpenArrayHandle;

// The implemented DPI standard version.
const char* svDpiVersion(void);

// Scope (LRM 35.5.3). Clotho is single-instance: one scope carrying the design's
// name and a per-key user-data map. svGetScope returns the active scope (of the
// context import currently running); svSetScope swaps it (returns the previous).
svScope     svGetScope(void);
svScope     svSetScope(const svScope scope);
const char* svGetNameFromScope(const svScope scope);
svScope     svGetScopeFromName(const char* scopeName);
int         svPutUserData(const svScope scope, void* userKey, void* userData);
void*       svGetUserData(const svScope scope, void* userKey);

// Bit-select: read/write bit `i` of a packed vector.
svBit   svGetBitselBit(const svBitVecVal* s, int i);
svLogic svGetBitselLogic(const svLogicVecVal* s, int i);
void    svPutBitselBit(svBitVecVal* d, int i, svBit s);
void    svPutBitselLogic(svLogicVecVal* d, int i, svLogic s);

// Part-select: a `w`-bit slice starting at bit `i`. Get copies s[i +: w] into the
// low `w` bits of *d; put writes the low `w` bits of `s` (one word, w <= 32) into
// *d at bit `i`.
void svGetPartselBit(svBitVecVal* d, const svBitVecVal* s, int i, int w);
void svGetPartselLogic(svLogicVecVal* d, const svLogicVecVal* s, int i, int w);
void svPutPartselBit(svBitVecVal* d, const svBitVecVal s, int i, int w);
void svPutPartselLogic(svLogicVecVal* d, const svLogicVecVal s, int i, int w);

#ifdef __cplusplus
}
#endif

#endif // CLOTHO_SVDPI_H
