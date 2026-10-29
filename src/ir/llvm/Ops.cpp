// Operators: 4-state arithmetic, logic, compare, select and concat, real math and net resolution.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// (old & ~mask) | ((ins << off) & mask)
llvm::Value* Backend::merge_bits(llvm::Value* old,
                                 llvm::Value* ins,
                                 llvm::Value* off,
                                 llvm::Value* mask,
                                 llvm::Value* nmask) {
    llvm::Value* shifted = b_.CreateAnd(b_.CreateShl(ins, off), mask);
    return b_.CreateOr(b_.CreateAnd(old, nmask), shifted);
}

// Low `n` bits set: (1 << n) - 1 for n in [0, W]; poison-free (n >= W -> all
// ones). `n` is an i32.
llvm::Value* Backend::low_ones(uint32_t W, llvm::Value* n) {
    llvm::Value* ge   = b_.CreateICmpUGE(n, b_.getInt32(W));
    llvm::Value* safe = cast(b_.CreateSelect(ge, b_.getInt32(0), n), W, false);
    llvm::Value* one  = llvm::ConstantInt::get(int_ty(W), 1);
    llvm::Value* m    = b_.CreateSub(b_.CreateShl(one, safe), one);
    return b_.CreateSelect(ge, ones(W), m);
}

// W-bit mask with bits [lo, hi) set (lo <= hi, both i32 in [0, W]).
llvm::Value* Backend::range_bits(uint32_t W, llvm::Value* lo, llvm::Value* hi) {
    return b_.CreateAnd(low_ones(W, hi), b_.CreateNot(low_ones(W, lo)));
}

// `v` shifted by the signed i32 `s`: left when s >= 0, right by |s| when
// s < 0. Poison-free and saturating — |s| >= W yields 0 (every bit leaves the
// W-bit word), the correct behavior when aligning an out-of-range selection.
llvm::Value* Backend::align_shl(llvm::Value* v, llvm::Value* s, uint32_t W) {
    llvm::Value* neg  = b_.CreateICmpSLT(s, b_.getInt32(0));
    llvm::Value* amt  = b_.CreateSelect(neg, b_.CreateNeg(s), s);
    llvm::Value* ovf  = b_.CreateICmpUGE(amt, b_.getInt32(W));
    llvm::Value* samt = cast(b_.CreateSelect(ovf, b_.getInt32(0), amt), W, false);
    llvm::Value* pick = b_.CreateSelect(neg, b_.CreateLShr(v, samt), b_.CreateShl(v, samt));
    return b_.CreateSelect(ovf, zero(W), pick);
}

// Signed min / max on i64 (for clamping a select's range to its container).
llvm::Value* Backend::smin(llvm::Value* a, llvm::Value* b) {
    return b_.CreateSelect(b_.CreateICmpSLT(a, b), a, b);
}

llvm::Value* Backend::smax(llvm::Value* a, llvm::Value* b) {
    return b_.CreateSelect(b_.CreateICmpSGT(a, b), a, b);
}

// Set `lv.range_mask` for a dynamic bit/part select, per LRM 11.5.1. The
// select covers storage bits [lv.bit_off, lv.bit_off + selw); the in-range
// bits are that range clamped to the container [cbase, cbase + cwidth) (the
// whole slot for a direct select, the inner select's span for a nested one).
// An x/z index (`idx_unknown`) makes every bit out of range.
//
// A nested select (`a[i][j]`) is invalid when any level is, and `lv` still
// holds the enclosing select's state. An enclosing select is always one whole
// element (slang rejects a select after a range select), so an x/z or
// out-of-range `i` leaves nothing of its window in range, and `lv.nest_valid`
// says so in one bit. Without it `j` alone decided, and `a[i][j]` read and
// wrote a bit for an `i` that is x.
//
// Returns the verdict --sanitize=bounds reports on. It is read off the
// UNCLAMPED offsets rather than by counting mask bits, so it is three
// compares and cannot disagree with the mask the access then uses.
Backend::SelectVerdict Backend::set_select_bounds(LValue&      lv,
                                                  llvm::Value* cbase,
                                                  uint32_t     cwidth,
                                                  uint32_t     selw,
                                                  llvm::Value* idx_unknown,
                                                  bool         constant_window) {
    const uint32_t sw    = lv.store_width;
    auto*          i64t  = int_ty(64);
    llvm::Value*   base  = b_.CreateSExt(cbase, i64t);
    llvm::Value*   end   = b_.CreateAdd(base, llvm::ConstantInt::get(i64t, cwidth));
    llvm::Value*   off   = b_.CreateSExt(lv.bit_off, i64t);
    llvm::Value*   top   = b_.CreateAdd(off, llvm::ConstantInt::get(i64t, selw));

    SelectVerdict v;
    v.fully = b_.CreateAnd(b_.CreateAnd(b_.CreateICmpSGE(off, base), b_.CreateICmpSLE(top, end)),
                           b_.CreateNot(idx_unknown));
    // Whether the access is invalid outright: this level's index is x/z, or an
    // enclosing level is invalid. A null `nest_valid` is a direct select, which
    // adds no IR here; `v.fully` already is its validity.
    llvm::Value* bad = idx_unknown;
    if (lv.nest_valid != nullptr) {
        bad           = b_.CreateOr(bad, b_.CreateNot(lv.nest_valid));
        lv.nest_valid = b_.CreateAnd(lv.nest_valid, v.fully);
    } else {
        lv.nest_valid = v.fully;
    }
    // Storage too wide to shift inline: hand the container to the runtime helper
    // (which clamps per bit) instead of building an sw-bit mask, whose own
    // construction is the O(sw^2) legalization this path exists to avoid. A
    // constant window has no variable shift to legalize: its mask is a constant.
    if (wide_dyn_storage(sw) && !constant_window) {
        lv.wide_dyn    = true;
        lv.sel_base    = base;
        lv.sel_cwidth  = cwidth;
        lv.sel_unknown = bad;
        // Some bit landed inside: compares only, no mask to count.
        v.partial = selw == 1 ? nullptr
                              : b_.CreateAnd(b_.CreateNot(v.fully),
                                             b_.CreateAnd(b_.CreateAnd(b_.CreateICmpSLT(off, end),
                                                                       b_.CreateICmpSGT(top, base)),
                                                          b_.CreateNot(bad)));
        return v;
    }
    llvm::Value* lo   = smax(smin(off, end), base);
    llvm::Value* hi   = smax(smin(top, end), base);
    llvm::Value* mask = range_bits(sw, b_.CreateTrunc(lo, int_ty(32)), b_.CreateTrunc(hi, int_ty(32)));
    lv.range_mask       = b_.CreateSelect(bad, zero(sw), mask);
    lv.range_mask_outer = false;

    v.partial =
      selw == 1 ? nullptr
                : b_.CreateAnd(b_.CreateNot(v.fully), b_.CreateICmpNE(lv.range_mask, zero(sw)));
    return v;
}

// A bit / part-select index operand as the i32 that is added to a bit offset.
// LRM 11.5.1 compares the index VALUE, so a narrower operand extends by its own
// signedness, and a wider one is narrowed by saturation: every value an i32
// holds passes, and a larger one (a 64-bit index of 2**32 + 1, which its low 32
// bits would turn into bit 1) becomes INT32_MAX or INT32_MIN, which no container
// reaches. The frontend hands over an operand wider than 32 bits only when a
// 32-bit offset could not hold its arithmetic.
llvm::Value* Backend::select_index(Plane idx, ValType t) {
    llvm::Value* m = must_of(idx);
    if (t.width <= 32) {
        return cast(m, 32, t.is_signed);
    }
    llvm::Value* hi = llvm::ConstantInt::get(m->getType(), INT32_MAX);
    if (!t.is_signed) {
        return cast(b_.CreateSelect(b_.CreateICmpULT(m, hi), m, hi), 32, false);
    }
    return cast(smax(smin(m, hi), llvm::ConstantInt::getSigned(m->getType(), INT32_MIN)), 32, false);
}

// The 0-based position of a fixed-size unpacked array element and whether the
// index is inside the declared range (LRM 7.4.5). The index is normalized in a
// type wide enough that neither its extension nor the subtraction of a bound can
// wrap, so an index of 2**32 + 1 or a signed -1 is out of range instead of
// aliasing a low element; LLVM narrows it again for the usual `int` index.
Backend::ElemPos Backend::fixed_elem_pos(Plane idx, ValType t, uint32_t left, uint32_t right) {
    const bool     ascending = left <= right;
    const uint32_t count     = (ascending ? right - left : left - right) + 1;
    const uint32_t ew        = std::max(64u, t.width + 2);
    llvm::Value*   v         = cast(must_of(idx), ew, t.is_signed);
    llvm::Value*   bound     = llvm::ConstantInt::getSigned(int_ty(ew), static_cast<int32_t>(left));
    llvm::Value*   norm      = ascending ? b_.CreateSub(v, bound) : b_.CreateSub(bound, v);
    // An unsigned compare also rejects a normalized index that went negative.
    return { cast(norm, 32, false), b_.CreateICmpULT(norm, llvm::ConstantInt::get(int_ty(ew), count)) };
}

// `idx < len` for a dynamic array or queue (LRM 7.4.5), the index compared at its
// own width and signedness. A negative index is a huge unsigned one, so a single
// compare covers both ends.
Backend::ElemBelow Backend::elem_index_below(Plane idx, ValType t, llvm::Value* len) {
    llvm::Value* m = must_of(idx);
    if (t.width <= 64) {
        llvm::Value* idx64 = cast(m, 64, t.is_signed);
        return { idx64, b_.CreateICmpULT(idx64, len) };
    }
    // Wider than the length: compare there, so the dropped high bits count.
    return { cast(m, 64, false), b_.CreateICmpULT(m, cast(len, t.width, false)) };
}

// Whether a runtime-offset select into `sw`-bit storage takes the runtime
// word-indexed path rather than shifting the whole value inline.
//
// The inline form asks LLVM for a variable shift on an i(2*sw) SSA value, which
// legalizes into O(sw/64) selects per word: measured JIT time for one dynamic bit
// select is 0.2 s at 4 Kbit, 1.1 s at 8 Kbit, 21 s at 12 Kbit and over half an
// hour at 64 Kbit (ivtest pr722). Below the threshold the inline form is both
// cheaper and better optimizable, so it stays; above it the cost is a call whose
// work is O(selw) and independent of the vector's width. The selection's own
// width is unrestricted: its planes travel as ceil(selw/64)-word buffers, so
// `[i +: 128]` of a 64 Kbit vector is as cheap as `[i]`.
bool Backend::wide_dyn_storage(uint32_t sw) {
    return sw > wide_dyn_select_bits;
}

// Storage-width write mask for a partial store: the range-restricted mask for
// a bounds-checked dynamic select, or the naive shifted mask otherwise.
//
// A wide dynamic select never reaches here: both the blocking store and the NBA
// staging route to rt_wide_ins, which addresses words instead of needing a mask
// this wide -- building one is the O(width^2) legalization that path avoids.
llvm::Value* Backend::store_mask(const LValue& lv, uint32_t sw) {
    if (lv.range_mask != nullptr) {
        if (lv.range_mask_outer) {
            // A constant part-select inside a dynamic select's window writes
            // only its own in-range bits (LRM 11.5.1). place_val is poison-free,
            // which matters when the outer index is out of range.
            return b_.CreateAnd(lv.range_mask, place_val(lv, ones(lv.width), sw));
        }
        return lv.range_mask;
    }
    return b_.CreateShl(cast(ones(lv.width), sw, false), cast(lv.bit_off, sw, false));
}

// Align a select's iW value to its storage bit position (poison-free for a
// bounds-checked select, a plain left shift otherwise).
llvm::Value* Backend::place_val(const LValue& lv, llvm::Value* v_iw, uint32_t sw) {
    llvm::Value* vW = cast(v_iw, sw, false);
    if (lv.range_mask != nullptr) {
        return align_shl(vW, lv.bit_off, sw);
    }
    return b_.CreateShl(vW, cast(lv.bit_off, sw, false));
}

// Merge one plane of a partial store: (old & ~mask) | (aligned ins & mask),
// with the alignment poison-free and the mask range-restricted for a
// bounds-checked dynamic select. `mask` is the shared store_mask(lv, sw).
llvm::Value* Backend::merge_sel(llvm::Value*  old,
                                llvm::Value*  ins_iw,
                                const LValue& lv,
                                uint32_t      sw,
                                llvm::Value*  mask) {
    llvm::Value* placed = b_.CreateAnd(place_val(lv, ins_iw, sw), mask);
    return b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(mask)), placed);
}

bool Backend::is_zero_const(llvm::Value* v) {
    auto* c = llvm::dyn_cast<llvm::ConstantInt>(v);
    return c != nullptr && c->isZero();
}

// Multi-driver net resolution of two drivers (LRM 6.6, no drive strengths):
// the per-bit lattice join with a high-Z operand as identity. A 2-state slot
// (z-elided to must==may, z==0) folds through correctly; the X / Z encodings
// are must=0/may=1 with z=0 (X) or z=1 (Z). Associative and commutative for
// all three reducers, so K drivers fold left one pair at a time. `tag` picks
// the reducer: WireRes (conflict -> X), WandRes (any 0 -> 0), WorRes (any
// 1 -> 1).
Plane Backend::lower_net_res(Inst::Tag tag, Plane a, Plane b) {
    llvm::Value* ma = must_of(a), *aa = may_of(a), *za = z_of(a);
    llvm::Value* mb = must_of(b), *ab = may_of(b), *zb = z_of(b);
    llvm::Value* nza = b_.CreateNot(za); // operand a actively drives (not Z)
    llvm::Value* nzb = b_.CreateNot(zb);
    // Per driver, gated by "actively driving": drives 1 (must), drives 0
    // (~may), drives X (may & ~must). Reduce across the two operands.
    llvm::Value* one  = b_.CreateOr(b_.CreateAnd(nza, ma), b_.CreateAnd(nzb, mb));
    llvm::Value* zero = b_.CreateOr(b_.CreateAnd(nza, b_.CreateNot(aa)),
                                    b_.CreateAnd(nzb, b_.CreateNot(ab)));
    llvm::Value* xdr =
      b_.CreateOr(b_.CreateAnd(nza, b_.CreateAnd(aa, b_.CreateNot(ma))),
                  b_.CreateAnd(nzb, b_.CreateAnd(ab, b_.CreateNot(mb))));
    llvm::Value* allz = b_.CreateAnd(za, zb);
    llvm::Value* rz   = allz;

    llvm::Value* rmust = nullptr;
    llvm::Value* rmay  = nullptr;
    switch (tag) {
        case Inst::Tag::WireRes: {
            // Agreement keeps the value; a 0-vs-1 or any-X conflict is X.
            llvm::Value* conflict = b_.CreateOr(b_.CreateAnd(one, zero), xdr);
            rmust                 = b_.CreateAnd(one, b_.CreateNot(conflict));
            // may = 1 for a resolved 1, X, or Z; only a resolved 0 has may 0.
            rmay = b_.CreateOr(rmust, b_.CreateOr(conflict, rz));
            break;
        }
        case Inst::Tag::WandRes:
            // Wired-AND: any 0 driver forces 0 (may = 0), else X on any X,
            // else 1. So the result is 0 exactly where some driver drove 0.
            rmust = b_.CreateAnd(one, b_.CreateAnd(b_.CreateNot(zero), b_.CreateNot(xdr)));
            rmay  = b_.CreateNot(zero);
            break;
        case Inst::Tag::WorRes:
            // Wired-OR: any 1 driver forces 1 (must = 1), else X on any X,
            // else 0. may = 1 for a resolved 1, X, or Z.
            rmust = one;
            rmay  = b_.CreateOr(one, b_.CreateOr(xdr, rz));
            break;
        default:
            throw BackendError{ "lower_net_res: not a resolution tag" };
    }
    return packed(rmust, rmay, rz);
}

// Drive-strength multi-driver resolution (LRM 6.6.1, 28.11). Strengths are
// compile-time constants, so the whole fold is per-strength-level bitmask
// arithmetic on the driver planes — no per-bit strength storage. Each driver
// contributes to the 0-side (bits it could drive to 0: value 0 or X, i.e.
// ~z & ~must) at its strength0 and to the 1-side (value 1 or X: ~z & may) at
// its strength1. Per level L, OR those contributions across all drivers whose
// strength equals L; then walk levels strongest -> weakest and take the first
// level (per bit) that drives anything: a lone side there wins its value, both
// sides at that same top level is X, no level at all is Z. At all-Strong this
// reduces exactly to WireRes.
Plane Backend::lower_strength_res(Index i) {
    const Inst::Payload& p    = ir_->payload(i);
    const auto           info = ir_->extra_data<StrengthResInfo>(p.extra);
    const auto           vals = ir_->region(info.values);
    const auto           raw  = ir_->extra_raw();
    const auto           s_lo = static_cast<uint32_t>(info.strengths.start);

    const uint32_t W = type_of(vals[0]).width;
    // Per-level (1..4) OR of every driver's 0-side / 1-side contribution.
    // Index 0 (HighZ) is unused — a HighZ-strength driver contributes nothing.
    std::array<llvm::Value*, 5> a0{}, a1{};
    for (int L = 0; L < 5; ++L) {
        a0[L] = zero(W);
        a1[L] = zero(W);
    }
    for (size_t k = 0; k < vals.size(); ++k) {
        const Plane        pl  = value_of(vals[k]);
        llvm::Value* const nz  = b_.CreateNot(z_of(pl)); // actively driving
        llvm::Value* const d0  = b_.CreateAnd(nz, b_.CreateNot(must_of(pl)));
        llvm::Value* const d1  = b_.CreateAnd(nz, may_of(pl));
        const uint16_t     str = static_cast<uint16_t>(raw[s_lo + k]);
        const uint8_t      s0  = static_cast<uint8_t>(str >> 8);
        const uint8_t      s1  = static_cast<uint8_t>(str & 0xff);
        if (s0 > 0) {
            a0[s0] = b_.CreateOr(a0[s0], d0);
        }
        if (s1 > 0) {
            a1[s1] = b_.CreateOr(a1[s1], d1);
        }
    }
    llvm::Value* found = zero(W);
    llvm::Value* r_is1 = zero(W);
    llvm::Value* r_isx = zero(W);
    for (int L = 4; L >= 1; --L) {
        llvm::Value* const active = b_.CreateAnd(b_.CreateOr(a0[L], a1[L]), b_.CreateNot(found));
        llvm::Value* const new1   = b_.CreateAnd(active, b_.CreateAnd(a1[L], b_.CreateNot(a0[L])));
        llvm::Value* const newx   = b_.CreateAnd(active, b_.CreateAnd(a0[L], a1[L]));
        r_is1 = b_.CreateOr(r_is1, new1);
        r_isx = b_.CreateOr(r_isx, newx);
        found = b_.CreateOr(found, active);
    }
    // Bits no driver reached stay Z; the rest are 1 / X / (implicitly) 0.
    llvm::Value* const r_z   = b_.CreateNot(found);
    llvm::Value* const rmust = r_is1;
    llvm::Value* const rmay  = b_.CreateOr(r_is1, b_.CreateOr(r_isx, r_z));
    return packed(rmust, rmay, r_z);
}

// Resistive pull of a resolved tri0/tri1 net (LRM 6.6.4): a still-Z bit
// becomes `pull` (0 or 1), a definitely-driven or X bit is unchanged, and the
// result carries no Z. A Z bit is (must 0, may 1, z 1); pulling to 0 clears
// its may bit, pulling to 1 sets its must bit.
Plane Backend::lower_net_pull(Plane a, uint32_t pull) {
    llvm::Value* must = must_of(a);
    llvm::Value* may  = may_of(a);
    llvm::Value* z    = z_of(a);
    if (pull == 0) {
        return packed(must, b_.CreateAnd(may, b_.CreateNot(z)), zero(width_of(a)));
    }
    return packed(b_.CreateOr(must, z), may, zero(width_of(a)));
}

llvm::Type* Backend::fp_ty(uint32_t width) {
    return width == 64 ? static_cast<llvm::Type*>(b_.getDoubleTy()) : b_.getFloatTy();
}

Plane Backend::real_plane(llvm::Value* fp) {
    return Plane{ .fp = fp };
}

llvm::Value* Backend::fp_of(Plane a) {
    if (a.fp == nullptr) {
        throw BackendError{ "internal: real op on a non-real value" };
    }
    return a.fp;
}

// Bit pattern -> storage planes (must = may = pattern, z = 0).
Plane Backend::real_to_planes(llvm::Value* fp) {
    const uint32_t w   = fp->getType()->isDoubleTy() ? 64 : 32;
    llvm::Value*   pat = b_.CreateBitCast(fp, int_ty(w));
    return packed(pat, pat, zero(w));
}

// Storage planes -> float value (the must half is the bit pattern).
llvm::Value* Backend::planes_to_real(Plane a, uint32_t width) {
    return b_.CreateBitCast(must_of(a), fp_ty(width));
}

Plane Backend::lower_real_arith(Inst::Tag tag, Plane a, Plane b) {
    llvm::Value* x = fp_of(a);
    llvm::Value* y = fp_of(b);
    switch (tag) {
        case Inst::Tag::FAdd:
            return real_plane(b_.CreateFAdd(x, y));
        case Inst::Tag::FSub:
            return real_plane(b_.CreateFSub(x, y));
        case Inst::Tag::FMul:
            return real_plane(b_.CreateFMul(x, y));
        case Inst::Tag::FDiv:
            return real_plane(b_.CreateFDiv(x, y));
        default:
            // FPow: C pow semantics cover the corners LRM 11.4.3 leaves
            // unspecified.
            return real_plane(b_.CreateBinaryIntrinsic(llvm::Intrinsic::pow, x, y));
    }
}

// Real comparison -> definite 1-bit (reals carry no x/z). Ordered
// predicates except FNe, which is unordered so NaN != NaN yields 1,
// matching C.
Plane Backend::lower_real_compare(Inst::Tag tag, Plane a, Plane b) {
    llvm::Value* x = fp_of(a);
    llvm::Value* y = fp_of(b);
    llvm::Value* c = nullptr;
    switch (tag) {
        case Inst::Tag::FEq:
            c = b_.CreateFCmpOEQ(x, y);
            break;
        case Inst::Tag::FNe:
            c = b_.CreateFCmpUNE(x, y);
            break;
        case Inst::Tag::FLt:
            c = b_.CreateFCmpOLT(x, y);
            break;
        case Inst::Tag::FLe:
            c = b_.CreateFCmpOLE(x, y);
            break;
        case Inst::Tag::FGt:
            c = b_.CreateFCmpOGT(x, y);
            break;
        default:
            c = b_.CreateFCmpOGE(x, y);
            break;
    }
    return bit_plane(c, c);
}

// Float -> integral. FtoI rounds half away from zero (LRM 6.12.1, the
// assignment / cast conversion); FtoITrunc truncates toward zero ($rtoi,
// LRM 20.5). Both go through a saturating signed 64-bit intermediate:
// fptosi.sat avoids poison on out-of-range / NaN inputs (behavior the LRM
// leaves undefined), and the final width cast wraps like an integer
// assignment.
Plane Backend::lower_ftoi(Inst::Tag tag, Plane v, ValType to) {
    llvm::Value* f = fp_of(v);
    if (tag == Inst::Tag::FtoI) {
        f = b_.CreateUnaryIntrinsic(llvm::Intrinsic::round, f);
    }
    llvm::Value* wide = b_.CreateIntrinsic(llvm::Intrinsic::fptosi_sat, { int_ty(64), f->getType() }, { f });
    llvm::Value* r    = cast(wide, to.width, true);
    return packed(r, r, zero(to.width));
}

// Mux with real alternatives. Definite condition selects normally; an
// ambiguous (x/z) condition compares the alternatives with == and yields
// the common value, else the real default 0.0 (LRM 11.4.11). NaN arms
// compare unequal, so they fall to 0.0.
Plane Backend::lower_real_mux(Plane cond, Plane a, Plane b) {
    llvm::Value* known = b_.CreateICmpEQ(must_of(cond), may_of(cond));
    llvm::Value* ctrue = b_.CreateICmpNE(must_of(cond), zero(1));
    llvm::Value* x     = fp_of(a);
    llvm::Value* y     = fp_of(b);
    llvm::Value* sel   = b_.CreateSelect(ctrue, x, y);
    llvm::Value* eq    = b_.CreateFCmpOEQ(x, y);
    llvm::Value* amb   = b_.CreateSelect(eq, x, llvm::ConstantFP::get(x->getType(), 0.0));
    return real_plane(b_.CreateSelect(known, sel, amb));
}

// Mux with string alternatives, the exact shape of lower_real_mux one level
// up: a string value is a borrowed rt_string handle pointer, so a definite
// condition selects a pointer and there is nothing to blend. An ambiguous
// (x/z) condition compares the alternatives and yields the common value,
// else "" — the string type's default (LRM Table 7-1). LRM 11.4.11 defines
// its bitwise merge for integral results only; slang's own constant
// evaluator falls back to type->getDefaultValue() the same way
// (ConditionalExpression::evalImpl), and that is what real does with 0.0.
// Note verilator instead takes the false arm here, and iverilog rejects a
// string ternary outright, so neither is an oracle for this case.
llvm::Value* Backend::lower_string_mux(Plane cond, Index a, Index b) {
    llvm::Value* x   = str_of(a);
    llvm::Value* y   = str_of(b);
    llvm::Value* sel = b_.CreateSelect(b_.CreateICmpNE(must_of(cond), zero(1)), x, y);
    // A 2-state condition can never be ambiguous, so it costs one select and
    // no rt_string_cmp call -- the common case (a comparison, a bit, a flag).
    if (cond.two) {
        return sel;
    }
    llvm::Value* known = b_.CreateICmpEQ(must_of(cond), may_of(cond));
    llvm::Value* eq    = b_.CreateICmpEQ(b_.CreateCall(str_cmp_fn(), { x, y }), b_.getInt32(0));
    llvm::Value* amb   = b_.CreateSelect(eq, x, str_empty_global());
    return b_.CreateSelect(known, sel, amb);
}

// A double-typed libm routine, resolved from the host process by the JIT
// (DynamicLibrarySearchGenerator in run_jit).
llvm::Value* Backend::libm_call(const char* name, llvm::Value* x, llvm::Value* y) {
    auto* dbl = b_.getDoubleTy();
    if (y != nullptr) {
        return b_.CreateCall(env_->llmod.getOrInsertFunction(name, dbl, dbl, dbl), { x, y });
    }
    return b_.CreateCall(env_->llmod.getOrInsertFunction(name, dbl, dbl), { x });
}

// rotl64(x, k) = (x << k) | (x >> (64 - k)).
llvm::Value* Backend::rotl64(llvm::Value* x, unsigned k) {
    return b_.CreateOr(b_.CreateShl(x, k), b_.CreateLShr(x, 64 - k));
}

// One xoshiro256++ step against the state at `rng`: load the four state
// words, produce the 64-bit result, advance and store the state. Inlined at
// each $urandom site (no per-draw runtime call). `rng` is the body's own
// rng_slot_ everywhere except the seeding draw a detached body takes from the
// ramping context's state (lower_task).
llvm::Value* Backend::emit_xoshiro_next(llvm::Value* rng) {
    auto* i64ty = b_.getInt64Ty();
    auto* arrty = llvm::ArrayType::get(i64ty, 4);
    llvm::Value* s[4];
    for (int k = 0; k < 4; ++k) {
        auto* ptr =
          b_.CreateInBoundsGEP(arrty, rng, { b_.getInt32(0), b_.getInt32(k) });
        s[k] = b_.CreateLoad(i64ty, ptr);
    }
    // result = rotl(s0 + s3, 23) + s0.
    llvm::Value* result = b_.CreateAdd(rotl64(b_.CreateAdd(s[0], s[3]), 23), s[0]);
    // State update (order matters: reads see already-updated words).
    llvm::Value* t   = b_.CreateShl(s[1], 17);
    llvm::Value* n2  = b_.CreateXor(s[2], s[0]);
    llvm::Value* n3  = b_.CreateXor(s[3], s[1]);
    llvm::Value* n1  = b_.CreateXor(s[1], n2);
    llvm::Value* n0  = b_.CreateXor(s[0], n3);
    n2               = b_.CreateXor(n2, t);
    n3               = rotl64(n3, 45);
    llvm::Value* ns[4] = { n0, n1, n2, n3 };
    for (int k = 0; k < 4; ++k) {
        auto* ptr =
          b_.CreateInBoundsGEP(arrty, rng, { b_.getInt32(0), b_.getInt32(k) });
        b_.CreateStore(ns[k], ptr);
    }
    return result;
}

// $urandom (LRM §18.13.1): the high 32 bits of one xoshiro256++ draw. `rng`
// selects the state to advance and defaults to the body's own; ObjRandom
// passes an object's state instead (LRM §18.14.2). A null default rather than
// `rng_slot_` because a non-static member cannot be a default argument.
Plane Backend::lower_urandom(llvm::Value* rng) {
    llvm::Value* draw = emit_xoshiro_next(rng != nullptr ? rng : rng_slot_);
    llvm::Value* hi   = b_.CreateTrunc(b_.CreateLShr(draw, 32), b_.getInt32Ty());
    return two_val(hi);
}

Plane Backend::lower_real_math_unary(Index i) {
    const auto&  p = ir_->payload(i).op_imm;
    llvm::Value* x = fp_of(value_of(p.op));
    using O = RealMathOp;
    namespace In = llvm::Intrinsic;
    switch (static_cast<O>(p.imm)) {
        // LRM Table 20-4 names the C math function each maps to. The
        // classic routines lower to LLVM intrinsics (constant-foldable);
        // the inverse / hyperbolic ones call libm directly.
        case O::Ln:
            return real_plane(b_.CreateUnaryIntrinsic(In::log, x));
        case O::Log10:
            return real_plane(b_.CreateUnaryIntrinsic(In::log10, x));
        case O::Exp:
            return real_plane(b_.CreateUnaryIntrinsic(In::exp, x));
        case O::Sqrt:
            return real_plane(b_.CreateUnaryIntrinsic(In::sqrt, x));
        case O::Floor:
            return real_plane(b_.CreateUnaryIntrinsic(In::floor, x));
        case O::Ceil:
            return real_plane(b_.CreateUnaryIntrinsic(In::ceil, x));
        case O::Sin:
            return real_plane(b_.CreateUnaryIntrinsic(In::sin, x));
        case O::Cos:
            return real_plane(b_.CreateUnaryIntrinsic(In::cos, x));
        case O::Tan:
            return real_plane(libm_call("tan", x));
        case O::Asin:
            return real_plane(libm_call("asin", x));
        case O::Acos:
            return real_plane(libm_call("acos", x));
        case O::Atan:
            return real_plane(libm_call("atan", x));
        case O::Sinh:
            return real_plane(libm_call("sinh", x));
        case O::Cosh:
            return real_plane(libm_call("cosh", x));
        case O::Tanh:
            return real_plane(libm_call("tanh", x));
        case O::Asinh:
            return real_plane(libm_call("asinh", x));
        case O::Acosh:
            return real_plane(libm_call("acosh", x));
        case O::Atanh:
            return real_plane(libm_call("atanh", x));
        case O::Pow:
        case O::Atan2:
        case O::Hypot:
            break;
    }
    throw BackendError{ "internal: two-operand routine on RealMathUnary" };
}

Plane Backend::lower_real_math_binary(Index i) {
    const auto&  p    = ir_->payload(i).op_extra;
    const auto   info = ir_->extra_data<RealMathBinaryInfo>(p.extra);
    llvm::Value* x    = fp_of(value_of(p.op));
    llvm::Value* y    = fp_of(value_of(info.rhs));
    using O = RealMathOp;
    switch (static_cast<O>(info.fn)) {
        case O::Pow:
            return real_plane(b_.CreateBinaryIntrinsic(llvm::Intrinsic::pow, x, y));
        case O::Atan2:
            return real_plane(libm_call("atan2", x, y));
        case O::Hypot:
            return real_plane(libm_call("hypot", x, y));
        default:
            throw BackendError{ "internal: one-operand routine on RealMathBinary" };
    }
}

Plane Backend::lower_arith(Inst::Tag tag, Plane a, Plane b) {
    const uint32_t w    = width_of(a);

    // 2-state: a single native op on the value; no X rule, no plane duplication.
    // Both operands must be 2-state for this -- a 4-state/2-state mix (LRM
    // 5.7: any 4-state operand makes the result 4-state) still needs the
    // packed-plane path below, which assumes both operands already arrived
    // as i(2W) (xz_mask reads a.mm/b.mm raw, not through must_of/may_of).
    // Promote the narrower side instead of trusting the dispatch on `a`
    // alone -- a mismatched-domain pair here is malformed LLVM IR (an i(2W)
    // vs iW operand pair), not just a wrong answer.
    if (a.two && !b.two) {
        a = to_four(a);
    } else if (!a.two && b.two) {
        b = to_four(b);
    } else if (a.two) {
        switch (tag) {
            case Inst::Tag::Add: return two_val(b_.CreateAdd(a.mm, b.mm));
            case Inst::Tag::Sub: return two_val(b_.CreateSub(a.mm, b.mm));
            default:             return two_val(b_.CreateMul(a.mm, b.mm)); // Mul
        }
    }

    // Width-1 add/sub/mul: index the per-op truth table instead of branching.
    // Add/sub truncate the carry/borrow to a^b; mul is a&b. All three keep the
    // arithmetic X rule (any X operand -> X), which the LUTs bake in.
    if (w == 1) {
        switch (tag) {
            case Inst::Tag::Add:
                return single_bit_lut(ADD1_LUT, a, b);
            case Inst::Tag::Sub:
                return single_bit_lut(SUB1_LUT, a, b);
            default: // Mul (lower_arith only handles add/sub/mul)
                return single_bit_lut(MUL1_LUT, a, b);
        }
    }

    auto* wide = int_ty(2 * w);

    llvm::Value* unknown = b_.CreateICmpNE(xz_mask(a, b, w), zero(w));

    auto* x_bb   = llvm::BasicBlock::Create(ctx_, "arith.x", fn_);
    auto* val_bb = llvm::BasicBlock::Create(ctx_, "arith.val", fn_);
    auto* end_bb = llvm::BasicBlock::Create(ctx_, "arith.end", fn_);

    llvm::BranchInst* br = b_.CreateCondBr(unknown, x_bb, val_bb);
    br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

    // Cold: whole result is X.
    b_.SetInsertPoint(x_bb);
    llvm::Value* x_word = x_mm(w);
    b_.CreateBr(end_bb);

    // Hot: one W-bit op on the value, duplicated into both planes.
    b_.SetInsertPoint(val_bb);
    llvm::Value* am = must_of(a);
    llvm::Value* bm = must_of(b);
    llvm::Value* r  = nullptr;
    switch (tag) {
        case Inst::Tag::Add:
            r = b_.CreateAdd(am, bm);
            break;
        case Inst::Tag::Sub:
            r = b_.CreateSub(am, bm);
            break;
        default:
            r = b_.CreateMul(am, bm);
            break;
    }
    llvm::Value* val_mm = packed(r, r, zero(w)).mm;
    b_.CreateBr(end_bb);

    b_.SetInsertPoint(end_bb);
    llvm::PHINode* mm = b_.CreatePHI(wide, 2);
    mm->addIncoming(x_word, x_bb);
    mm->addIncoming(val_mm, val_bb);
    return { mm, zero(w) };
}

// Divide and modulo. LRM 1800-2023 §11.4.3: integer division truncates toward
// zero, modulo takes the sign of the dividend, and a zero divisor or any X/Z
// bit in either operand makes the whole result X.
Plane Backend::lower_divmod(Inst::Tag tag, Plane a, Plane b, bool sign) {
    const uint32_t w      = width_of(a);
    const bool     is_div = tag == Inst::Tag::Div;

    // 2-state: divide/modulo by zero is x, which collapses to 0 in a 2-state
    // result (verified against iverilog: `int`/`bit` div/mod by 0 -> 0). Guard
    // the divisor to avoid poison, then select 0 on a zero divisor. Signed
    // INT_MIN / -1 is also poison, patched to the wrapped value like the
    // 4-state path.
    //
    // Both operands must be 2-state for this fast path -- a 4-state/2-state
    // mix (LRM 5.7: any 4-state operand makes the result 4-state) still
    // needs the packed-plane path below, which assumes both operands
    // already arrived as i(2W) (xz_mask reads a.mm/b.mm raw). Promote the
    // narrower side instead of trusting the dispatch on `a` alone -- a
    // mismatched-domain pair here is malformed LLVM IR (an i(2W) vs iW
    // operand to the same icmp/select), observed to crash inside LLVM's
    // constant folder rather than just miscompile.
    if (a.two && !b.two) {
        a = to_four(a);
    } else if (!a.two && b.two) {
        b = to_four(b);
    } else if (a.two) {
        llvm::Value* dz = b_.CreateICmpEQ(b.mm, zero(w));
        llvm::Value* r;
        if (!sign) {
            llvm::Value* safe = b_.CreateSelect(dz, ones(w), b.mm);
            r = is_div ? b_.CreateUDiv(a.mm, safe) : b_.CreateURem(a.mm, safe);
        } else {
            llvm::Value* neg1 = b_.CreateICmpEQ(b.mm, llvm::ConstantInt::getAllOnesValue(int_ty(w)));
            llvm::Value* bad  = b_.CreateOr(dz, neg1);
            llvm::Value* safe = b_.CreateSelect(bad, llvm::ConstantInt::get(int_ty(w), 1), b.mm);
            r = is_div ? b_.CreateSelect(neg1, b_.CreateNeg(a.mm), b_.CreateSDiv(a.mm, safe))
                       : b_.CreateSelect(neg1, zero(w), b_.CreateSRem(a.mm, safe));
        }
        return two_val(b_.CreateSelect(dz, zero(w), r));
    }

    // Width-1: index the truth table instead of branching. DIV1/MOD1 bake in
    // the X rule and the divide-by-zero -> X case.
    if (w == 1) {
        return single_bit_lut(is_div ? DIV1_LUT : MOD1_LUT, a, b);
    }

    auto* wide = int_ty(2 * w);

    llvm::Value* am = must_of(a);
    llvm::Value* bm = must_of(b);

    // One cold branch folds the X/Z test and the divide-by-zero test together:
    // both map to an all-X result, so a single not-taken branch is optimal. For
    // 2-state operands the X/Z half is statically zero and instcombine drops it,
    // leaving just `divisor == 0`.
    llvm::Value* unknown = b_.CreateICmpNE(xz_mask(a, b, w), zero(w));
    llvm::Value* divzero = b_.CreateICmpEQ(bm, zero(w));
    llvm::Value* cold    = b_.CreateOr(unknown, divzero);

    auto* x_bb   = llvm::BasicBlock::Create(ctx_, "divmod.x", fn_);
    auto* val_bb = llvm::BasicBlock::Create(ctx_, "divmod.val", fn_);
    auto* end_bb = llvm::BasicBlock::Create(ctx_, "divmod.end", fn_);

    llvm::BranchInst* br = b_.CreateCondBr(cold, x_bb, val_bb);
    br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

    b_.SetInsertPoint(x_bb);
    llvm::Value* x_word = x_mm(w);
    b_.CreateBr(end_bb);

    // Hot: divisor guaranteed nonzero. Unsigned is a direct udiv/urem. Signed
    // sdiv/srem are poison for INT_MIN / -1, so sanitize that divisor to 1 and
    // substitute the wrapped result (INT_MIN for /, 0 for %), which is the
    // two's-complement overflow value at width W.
    b_.SetInsertPoint(val_bb);
    llvm::Value* r = nullptr;
    if (!sign) {
        r = is_div ? b_.CreateUDiv(am, bm) : b_.CreateURem(am, bm);
    }
    else {
        llvm::Value* neg1 = b_.CreateICmpEQ(bm, llvm::ConstantInt::getAllOnesValue(int_ty(w)));
        llvm::Value* safe = b_.CreateSelect(neg1, llvm::ConstantInt::get(int_ty(w), 1), bm);
        if (is_div) {
            r = b_.CreateSelect(neg1, b_.CreateNeg(am), b_.CreateSDiv(am, safe));
        }
        else {
            r = b_.CreateSelect(neg1, zero(w), b_.CreateSRem(am, safe));
        }
    }
    llvm::Value* val_mm = packed(r, r, zero(w)).mm;
    b_.CreateBr(end_bb);

    b_.SetInsertPoint(end_bb);
    llvm::PHINode* mm = b_.CreatePHI(wide, 2);
    mm->addIncoming(x_word, x_bb);
    mm->addIncoming(val_mm, val_bb);
    return { mm, zero(w) };
}

// Power (**). LRM 1800-2023 §11.4.3: the exponent is self-determined (so it may
// be a different width from the base/result), any X/Z bit makes the result X, and
// the positive-exponent table corners (exp 0 -> 1, 0**n -> 0, 1**n -> 1) fall out
// of exponentiation by squaring. Two's-complement multiply is sign-agnostic, so
// the same loop is bit-correct for a signed base at a non-negative exponent.
//
// A negative exponent is reachable only when the exponent type is signed; it
// triggers Table 11-4's negative-op2 corners, computed branchlessly (op1==0 -> X,
// op1==1 -> 1, signed op1==-1 -> parity of op2, else -> 0). base_signed gates the
// op1==-1 case: an unsigned all-ones is a large positive, so it takes the `else`.
Plane Backend::lower_pow(Plane a, Plane b, bool base_signed, bool exp_signed) {
    const uint32_t w  = width_of(a);
    const uint32_t ew = width_of(b);

    // 2-state: reuse the 4-state exponentiation-by-squaring loop on the lifted
    // operands (no X bits, so the loop's cold X block folds away), then project
    // the definite result back to a single iW. Avoids duplicating the delicate
    // loop; a 0**neg -> X result projects to 0 via must_of (LRM 6.3.1).
    if (a.two) {
        return two_val(must_of(lower_pow(to_four(a), to_four(b), base_signed, exp_signed)));
    }

    // Past the a.two return the base is 4-state, but the exponent may still be
    // 2-state (mixed domains: a 4-state base with a 2-state exponent, e.g. the
    // common `x ** 3` where a bare literal is 2-state). The 4-state paths below
    // read the exponent's packed i(2*ew) word, so lift it — otherwise the
    // X-mask's `b.mm >> ew` shifts an iW value by its full width (poison).
    b = to_four(b);

    // Width-1 base and exponent, both unsigned: index the truth table (exp 0 -> 1,
    // 1**n -> 1, 0**(n>0) -> 0, any X -> X). The LUT is unsigned-only; a signed
    // operand falls through to the general path (which handles the -1 corner). A
    // wider exponent stays on the runtime path.
    if (w == 1 && ew == 1 && !base_signed && !exp_signed) {
        return single_bit_lut(POW1_LUT, a, b);
    }

    auto* wide = int_ty(2 * w);
    auto* wty  = int_ty(w);
    auto* ety  = int_ty(ew);

    // X/Z in either operand -> all-X. The operands can differ in width, so each
    // mask is tested at its own width and the two feed one cold branch.
    llvm::Value* xa      = b_.CreateXor(a.mm, b_.CreateLShr(a.mm, w));
    llvm::Value* xb      = b_.CreateXor(b.mm, b_.CreateLShr(b.mm, ew));
    llvm::Value* ua      = b_.CreateICmpNE(b_.CreateTrunc(xa, int_ty(w)), zero(w));
    llvm::Value* ub      = b_.CreateICmpNE(b_.CreateTrunc(xb, int_ty(ew)), zero(ew));
    llvm::Value* unknown = b_.CreateOr(ua, ub);

    auto* x_bb    = llvm::BasicBlock::Create(ctx_, "pow.x", fn_);
    auto* known_bb = llvm::BasicBlock::Create(ctx_, "pow.known", fn_);
    auto* val_bb  = llvm::BasicBlock::Create(ctx_, "pow.val", fn_);
    auto* end_bb  = llvm::BasicBlock::Create(ctx_, "pow.end", fn_);

    llvm::BranchInst* br = b_.CreateCondBr(unknown, x_bb, known_bb);
    br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

    b_.SetInsertPoint(x_bb);
    llvm::Value* x_word = x_mm(w);
    b_.CreateBr(end_bb);

    // Operands known. Seeds are materialized here so they dominate both the loop
    // (non-negative exponent) and the negative-exponent corner block below.
    b_.SetInsertPoint(known_bb);
    llvm::Value* base0 = must_of(a);
    llvm::Value* exp0  = must_of(b);

    // Negative-exponent corners (Table 11-4), reachable only for a signed
    // exponent. Branchless: is0 -> X, is1 or (-1 with even op2) -> 1, -1 with
    // odd op2 -> all-ones (-1), otherwise 0.
    llvm::BasicBlock* neg_bb = nullptr;
    llvm::Value*      neg_mm = nullptr;
    if (exp_signed) {
        neg_bb = llvm::BasicBlock::Create(ctx_, "pow.neg", fn_);
        b_.CreateCondBr(b_.CreateICmpSLT(exp0, zero(ew)), neg_bb, val_bb);

        b_.SetInsertPoint(neg_bb);
        llvm::Value* all_ones = llvm::ConstantInt::getAllOnesValue(wty);
        llvm::Value* is0      = b_.CreateICmpEQ(base0, zero(w));
        llvm::Value* is1      = b_.CreateICmpEQ(base0, llvm::ConstantInt::get(wty, 1));
        llvm::Value* ism1     = base_signed ? b_.CreateICmpEQ(base0, all_ones) : b_.getFalse();
        llvm::Value* odd      = b_.CreateTrunc(exp0, b_.getInt1Ty());
        llvm::Value* one_like = b_.CreateOr(is1, b_.CreateAnd(ism1, b_.CreateNot(odd)));
        llvm::Value* negone   = b_.CreateAnd(ism1, odd);
        llvm::Value* val      = b_.CreateSelect(
          negone, all_ones, b_.CreateSelect(one_like, llvm::ConstantInt::get(wty, 1), zero(w)));
        neg_mm = b_.CreateSelect(is0, x_mm(w), packed(val, val, zero(w)).mm);
        b_.CreateBr(end_bb);

        b_.SetInsertPoint(val_bb);
    } else {
        b_.CreateBr(val_bb);
        b_.SetInsertPoint(val_bb);
    }

    // Hot: exponentiation by squaring at the result width W. Every product wraps
    // modulo 2^W, the correct result for width W; the wide (>64-bit) multiply
    // legalizes to the __multi3 libcall resolved in the JIT. The exponent keeps
    // its self-determined width so wide exponents are not truncated. The loop
    // always runs once (exp 0 -> one no-op iteration leaving result 1), which
    // yields the non-negative table corners directly.
    auto* loop = llvm::BasicBlock::Create(ctx_, "pow.loop", fn_);
    b_.CreateBr(loop);

    b_.SetInsertPoint(loop);
    llvm::PHINode* result = b_.CreatePHI(wty, 2);
    llvm::PHINode* base   = b_.CreatePHI(wty, 2);
    llvm::PHINode* exp    = b_.CreatePHI(ety, 2);
    result->addIncoming(llvm::ConstantInt::get(wty, 1), val_bb);
    base->addIncoming(base0, val_bb);
    exp->addIncoming(exp0, val_bb);

    // Multiply result by base when the low exponent bit is set; always square
    // the base and shift the exponent down.
    llvm::Value* odd      = b_.CreateTrunc(exp, b_.getInt1Ty());
    llvm::Value* product  = b_.CreateMul(result, base);
    llvm::Value* next_res = b_.CreateSelect(odd, product, result);
    llvm::Value* next_bas = b_.CreateMul(base, base);
    llvm::Value* next_exp = b_.CreateLShr(exp, llvm::ConstantInt::get(ety, 1));
    llvm::Value* more     = b_.CreateICmpNE(next_exp, zero(ew));

    // Result mantissa for the loop->end edge: emitted before the terminator so it
    // dominates the predecessor's branch (PHI-incoming requirement).
    llvm::Value* val_mm = packed(next_res, next_res, zero(w)).mm;

    b_.CreateCondBr(more, loop, end_bb);
    result->addIncoming(next_res, loop);
    base->addIncoming(next_bas, loop);
    exp->addIncoming(next_exp, loop);

    b_.SetInsertPoint(end_bb);
    llvm::PHINode* mm = b_.CreatePHI(wide, neg_bb ? 3 : 2);
    mm->addIncoming(x_word, x_bb);
    mm->addIncoming(val_mm, loop);
    if (neg_bb) {
        mm->addIncoming(neg_mm, neg_bb);
    }
    return { mm, zero(w) };
}

// Evaluate a single-bit binary op by indexing its packed truth table: the two
// 2-bit operand codes form idx = (a<<2)|b and the result code sits at bit
// idx*2. One variable shift of the i32 immediate selects it — no unpack, no
// branch. The shift count must match the LUT width, so widen the operands to
// i32 (free in codegen); the result truncs back to the 2-bit plane.
Plane Backend::single_bit_lut(uint32_t lut, Plane a, Plane b) {
    llvm::Value* aw = b_.CreateZExt(a.mm, int_ty(32));
    llvm::Value* bw = b_.CreateZExt(b.mm, int_ty(32));
    llvm::Value* sh = b_.CreateOr(b_.CreateShl(aw, 3), b_.CreateShl(bw, 1));
    llvm::Value* t  = b_.CreateLShr(b_.getInt32(lut), sh);
    return { b_.CreateTrunc(t, int_ty(2)), zero(1) };
}

Plane Backend::lower_bitwise(Inst::Tag tag, Plane a, Plane b) {
    const uint32_t w = width_of(a);

    // 2-state: one native bitwise op; xnor is ~(xor). Both operands must be
    // 2-state for this -- a 4-state/2-state mix (LRM 5.7: any 4-state
    // operand makes the result 4-state) still needs the packed-plane path
    // below, which assumes both operands already arrived as i(2W). Promote
    // the narrower side instead of trusting the dispatch on `a` alone -- a
    // mismatched-domain pair here is malformed LLVM IR, not just a wrong
    // answer.
    if (a.two && !b.two) {
        a = to_four(a);
    } else if (!a.two && b.two) {
        b = to_four(b);
    } else if (a.two) {
        switch (tag) {
            case Inst::Tag::Band:  return two_val(b_.CreateAnd(a.mm, b.mm));
            case Inst::Tag::Bor:   return two_val(b_.CreateOr(a.mm, b.mm));
            case Inst::Tag::Bxor:  return two_val(b_.CreateXor(a.mm, b.mm));
            default:               return two_val(b_.CreateNot(b_.CreateXor(a.mm, b.mm))); // Bxnor
        }
    }
    if (tag == Inst::Tag::Band) {
        return { b_.CreateAnd(a.mm, b.mm), zero(w) };
    }
    if (tag == Inst::Tag::Bor) {
        return { b_.CreateOr(a.mm, b.mm), zero(w) };
    }
    if (tag == Inst::Tag::Bxnor) {
        if (w == 1) {
            return single_bit_lut(BXNOR1_LUT, a, b);
        }
        // Wider xnor = ~(xor): lower_bnot does must' = ~may, may' = ~must over
        // the xor result, which is exactly the negated value with the same X mask.
        return lower_bnot(lower_bitwise(Inst::Tag::Bxor, a, b));
    }
    if (w == 1) {
        return single_bit_lut(XOR1_LUT, a, b);
    }
    // Wider Bxor (non-monotone). When neither operand has an X/Z bit, XOR is
    // monotone like AND/OR — must == may in each operand, so the packed `a ^ b`
    // is already the answer. Real designs hit that case ~all the time per site,
    // so guard the cheap path and fall back to the full unpack only on X. The
    // combined X-mask `u` is computed once for the test and reused by the slow
    // path.
    llvm::Value* u     = xz_mask(a, b, w);
    llvm::Value* known = b_.CreateICmpEQ(u, zero(w));

    auto* fast_bb = llvm::BasicBlock::Create(ctx_, "xor.known", fn_);
    auto* slow_bb = llvm::BasicBlock::Create(ctx_, "xor.x", fn_);
    auto* end_bb  = llvm::BasicBlock::Create(ctx_, "xor.end", fn_);

    llvm::BranchInst* br = b_.CreateCondBr(known, fast_bb, slow_bb);
    br->setMetadata(llvm::LLVMContext::MD_prof, llvm::MDBuilder(ctx_).createBranchWeights(1u << 20, 1));

    // Hot: both operands known -> one packed xor carries both planes.
    b_.SetInsertPoint(fast_bb);
    llvm::Value* fast_mm = b_.CreateXor(a.mm, b.mm);
    b_.CreateBr(end_bb);

    // Cold: at least one X bit. Reuse `u`; value is the must-bit xor, X where u.
    b_.SetInsertPoint(slow_bb);
    llvm::Value* am      = must_of(a);
    llvm::Value* bm      = must_of(b);
    llvm::Value* k       = b_.CreateXor(am, bm);
    llvm::Value* slow_mm = packed(b_.CreateAnd(b_.CreateNot(u), k), b_.CreateOr(u, k), zero(w)).mm;
    b_.CreateBr(end_bb);

    b_.SetInsertPoint(end_bb);
    llvm::PHINode* mm = b_.CreatePHI(int_ty(2 * w), 2);
    mm->addIncoming(fast_mm, fast_bb);
    mm->addIncoming(slow_mm, slow_bb);
    return { mm, zero(w) };
}

Plane Backend::lower_bnot(Plane a) {
    // 2-state: a plain complement.
    if (a.two) {
        return two_val(b_.CreateNot(a.mm));
    }
    // NOT is must' = ~may, may' = ~must: complement both planes and swap them.
    // Complement is one full ~ on the packed value; the swap is a rotate-left
    // by W (the two halves trade places), emitted as a funnel shift. NOT and
    // rotate commute, so order does not matter.
    const uint32_t w       = width_of(a);
    auto*          wide    = int_ty(2 * w);
    llvm::Value*   notted  = b_.CreateNot(a.mm);
    llvm::Value*   amount  = llvm::ConstantInt::get(wide, w);
    llvm::Value*   swapped = b_.CreateIntrinsic(llvm::Intrinsic::fshl, { wide }, { notted, notted, amount });
    return { swapped, zero(w) };
}

Plane Backend::lower_reduce(Inst::Tag tag, Plane a) {
    // Definite 4-state 1-bit result (compute_type); a 2-state operand lifts and
    // the any_unknown terms fold away, leaving the native reduction.
    a = to_four(a);
    const uint32_t w  = width_of(a);
    llvm::Value*   am = must_of(a);
    llvm::Value*   aM = may_of(a);
    if (tag == Inst::Tag::RedXor) {
        llvm::Value* unknown = any_unknown(a);
        llvm::Value* pop     = b_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, am);
        llvm::Value* parity  = b_.CreateTrunc(pop, int_ty(1));
        return bit_plane(b_.CreateAnd(b_.CreateNot(unknown), parity), b_.CreateOr(unknown, parity));
    }
    if (tag == Inst::Tag::RedAnd) {
        // must: all bits definitely 1; may: all bits could be 1.
        return bit_plane(b_.CreateICmpEQ(am, ones(w)), b_.CreateICmpEQ(aM, ones(w)));
    }
    // RedOr: must: any bit definitely 1; may: any bit could be 1.
    return bit_plane(b_.CreateICmpNE(am, zero(w)), b_.CreateICmpNE(aM, zero(w)));
}

// $clog2 (LRM §20.8.1): ceil(log2(operand)), operand unsigned, $clog2(0) == 0,
// -> a 32-bit `integer`. For a known operand of width W, the ceiling equals the
// bit index one past the top set bit of `operand - 1`, i.e. `W - clz(operand-1)`;
// `operand == 0` is special-cased to 0 (it would otherwise wrap to all-ones and
// yield W). Any x/z bit in the operand makes the whole result X, matching
// iverilog (the LRM does not define x/z operands here).
// Ctz / Clz: index of the lowest / highest definite-1 bit of the operand's
// must plane, or the operand width when it has none. x/z bits (must = 0)
// never count as set, matching `bit === 1'b1`. The result is always definite
// (the must plane is known), so no X-propagation.
Plane Backend::lower_bit_index(Inst::Tag tag, Plane a) {
    const uint32_t w   = width_of(a);
    llvm::Value*   val = must_of(a);  // definite-1 bits
    llvm::Value*   idx = nullptr;
    if (tag == Inst::Tag::Ctz) {
        // cttz(val, zero_poison=false) returns w when val == 0 (no set bit).
        idx = b_.CreateIntrinsic(llvm::Intrinsic::cttz, { int_ty(w) }, { val, b_.getFalse() });
    }
    else {
        // Highest set bit index = (w-1) - clz(val); w when val == 0.
        llvm::Value* clz = b_.CreateIntrinsic(llvm::Intrinsic::ctlz, { int_ty(w) }, { val, b_.getFalse() });
        llvm::Value* hb  = b_.CreateSub(llvm::ConstantInt::get(int_ty(w), w - 1), clz);
        idx = b_.CreateSelect(b_.CreateICmpEQ(val, zero(w)), llvm::ConstantInt::get(int_ty(w), w), hb);
    }
    return two_val(cast(idx, 32, false));
}

// $countbits / $countones / $onehot* / $isunknown (LRM 20.9): count the
// operand bits whose 4-state value is selected by `mask` (a CountBitsClass
// OR-mask). Per-bit codes from the planes: definite 1 = must; definite 0 =
// ~may; unknown = may & ~must; z = unknown & z-plane; x = unknown & ~z. The
// selected classes (disjoint per bit) OR together, then popcount. The result
// is definite (2-state) — counting never propagates X.
// 32-bit definite popcount of the operand bits whose 4-state class is in
// `mask` (LRM 20.9).
llvm::Value* Backend::count_bits_pop(Plane a, uint32_t mask) {
    a = to_four(a);
    const uint32_t w       = width_of(a);
    llvm::Value*   m       = must_of(a);
    llvm::Value*   M       = may_of(a);
    llvm::Value*   z       = z_of(a);
    llvm::Value*   unknown = b_.CreateAnd(M, b_.CreateNot(m));
    llvm::Value*   sel     = zero(w);
    if (mask & CountZero) {
        sel = b_.CreateOr(sel, b_.CreateNot(M));
    }
    if (mask & CountOne) {
        sel = b_.CreateOr(sel, m);
    }
    if (mask & CountX) {
        sel = b_.CreateOr(sel, b_.CreateAnd(unknown, b_.CreateNot(z)));
    }
    if (mask & CountZ) {
        sel = b_.CreateOr(sel, b_.CreateAnd(unknown, z));
    }
    llvm::Value* pop = b_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, sel);
    return cast(pop, 32, false);
}

Plane Backend::lower_count_bits(Plane a, uint32_t mask) {
    return two_val(count_bits_pop(a, mask));
}

// $onehot / $onehot0 (LRM 20.9): popcount of the definite-1 bits compared
// against 1. x/z bits are never definite 1, so the result is definite.
Plane Backend::lower_onehot(Plane a, bool allow_zero) {
    llvm::Value* pop = count_bits_pop(a, CountOne);
    llvm::Value* r   = allow_zero ? b_.CreateICmpULE(pop, b_.getInt32(1))
                                  : b_.CreateICmpEQ(pop, b_.getInt32(1));
    return two_val(r);
}

// $isunknown (LRM 20.9): the union of the x and z classes is exactly the
// unknown plane, so this is one reduction, not a popcount-compare.
Plane Backend::lower_isunknown(Plane a) {
    a = to_four(a);
    llvm::Value* unknown = b_.CreateAnd(may_of(a), b_.CreateNot(must_of(a)));
    return two_val(b_.CreateICmpNE(unknown, zero(width_of(a))));
}

Plane Backend::lower_clog2(Plane a) {
    const uint32_t w   = width_of(a);
    llvm::Value*   val = must_of(a); // operand treated as unsigned

    // W - clz(val - 1), computed in a width that holds the count (<= W) and the
    // 32-bit result; ctlz's zero-is-poison flag is false so val == 1 (val-1 == 0)
    // yields clz == W and thus a count of 0.
    const uint32_t cw    = std::max(w, 32u);
    llvm::Value*   vm1   = b_.CreateSub(val, llvm::ConstantInt::get(int_ty(w), 1));
    llvm::Value*   clz   = b_.CreateIntrinsic(llvm::Intrinsic::ctlz, { int_ty(w) }, { vm1, b_.getFalse() });
    llvm::Value*   count = b_.CreateSub(llvm::ConstantInt::get(int_ty(cw), w), cast(clz, cw, false));
    llvm::Value*   is_zero = b_.CreateICmpEQ(val, zero(w));
    llvm::Value*   known   = b_.CreateSelect(is_zero, zero(cw), count);
    llvm::Value*   res32   = cast(known, 32, false);

    // X-propagate: any unknown operand bit -> all-X result (must = 0, may = 1s).
    llvm::Value* unknown = any_unknown(a);
    llvm::Value* must    = b_.CreateSelect(unknown, zero(32), res32);
    llvm::Value* may     = b_.CreateSelect(unknown, ones(32), res32);
    return packed(must, may, zero(32));
}

Plane Backend::lower_shift(Inst::Tag tag, Plane a, Plane amt, bool sign) {
    const uint32_t w  = width_of(a);
    const uint32_t aw = width_of(amt);

    // 2-state value: one native shift with the same out-of-range handling. An
    // x/z in the (self-determined) amount makes the result x, which collapses
    // to 0 in a 2-state result.
    if (a.two) {
        const bool     arith = tag == Inst::Tag::Sra && sign;
        const uint32_t cw    = std::max(aw, w);
        llvm::Value*   av    = must_of(amt);
        llvm::Value*   oob   = b_.CreateICmpUGE(cast(av, cw, false), llvm::ConstantInt::get(int_ty(cw), w));
        llvm::Value*   safe =
          b_.CreateSelect(oob, llvm::ConstantInt::get(int_ty(w), w == 0 ? 0 : w - 1), cast(av, w, false));
        llvm::Value* r;
        switch (tag) {
            case Inst::Tag::Sll: r = b_.CreateShl(a.mm, safe); break;
            case Inst::Tag::Srl: r = b_.CreateLShr(a.mm, safe); break;
            default:             r = arith ? b_.CreateAShr(a.mm, safe) : b_.CreateLShr(a.mm, safe); break;
        }
        // Logical shifts (and unsigned >>>) zero on out-of-range; arithmetic
        // >>> already yields sign fill from the clamp to w-1.
        if (!arith) {
            r = b_.CreateSelect(oob, zero(w), r);
        }
        return two_val(b_.CreateSelect(any_unknown(amt), zero(w), r));
    }

    llvm::Value*   am = must_of(a);
    llvm::Value*   aM = may_of(a);

    // Detect a shift amount >= width at the amount's untruncated width. LLVM
    // shift ops are poison once the amount reaches the operand width (LRM
    // §11.4.10 instead defines the result), and truncating the amount to `w`
    // first would alias multiples of 2**w down to 0. Compare in a width that
    // holds both the amount and `w`.
    const uint32_t cw  = std::max(aw, w);
    llvm::Value*   oob = b_.CreateICmpUGE(cast(must_of(amt), cw, false),
                                          llvm::ConstantInt::get(int_ty(cw), w));

    // Clamp to w-1 so every LLVM shift below is well-defined; the truncated
    // amount is exact whenever the shift is in range (a value < w fits in w
    // bits), and the out-of-range result is patched in afterwards.
    llvm::Value* sh   = cast(must_of(amt), w, false);
    llvm::Value* safe = b_.CreateSelect(oob, llvm::ConstantInt::get(int_ty(w), w == 0 ? 0 : w - 1), sh);

    const bool   arith = tag == Inst::Tag::Sra && sign;
    llvm::Value* must;
    llvm::Value* may;
    llvm::Value* z;
    switch (tag) {
        case Inst::Tag::Sll:
            must = b_.CreateShl(am, safe);
            may  = b_.CreateShl(aM, safe);
            z    = b_.CreateShl(a.z, safe);
            break;
        case Inst::Tag::Srl:
            must = b_.CreateLShr(am, safe);
            may  = b_.CreateLShr(aM, safe);
            z    = b_.CreateLShr(a.z, safe);
            break;
        default: // Sra: arithmetic (signed) replicates the sign and its
                 // unknown-ness; unsigned `>>>` is logical.
            must = arith ? b_.CreateAShr(am, safe) : b_.CreateLShr(am, safe);
            may  = arith ? b_.CreateAShr(aM, safe) : b_.CreateLShr(aM, safe);
            z    = arith ? b_.CreateAShr(a.z, safe) : b_.CreateLShr(a.z, safe);
            break;
    }

    // Out-of-range: logical shifts (and unsigned `>>>`) produce 0. The
    // arithmetic `>>>` already yields all sign bits from the clamp to w-1.
    if (!arith) {
        must = b_.CreateSelect(oob, zero(w), must);
        may  = b_.CreateSelect(oob, zero(w), may);
        z    = b_.CreateSelect(oob, zero(w), z);
    }

    llvm::Value* unknown = any_unknown(amt);
    return packed(b_.CreateSelect(unknown, zero(w), must),
                  b_.CreateSelect(unknown, ones(w), may),
                  b_.CreateSelect(unknown, zero(w), z));
}

// Whole-aggregate `==` (LRM 11.2.2): recursively AND-reduce per-leaf
// logical equality over the member walk. Any definite member mismatch is
// 0, else X if any member is unknown, else 1 — the Land reduction carries
// LRM 11.4.5 X-propagation exactly (docs/LRM_NOTES.md, note 13).
Plane Backend::lower_agg_eq(const LValue& lhs, const LValue& rhs, Ref type) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        const auto fields = mod_.pool.struct_fields(t.fields_id);
        Plane      acc{};
        for (uint32_t k = 0; k < fields.size(); ++k) {
            const Plane e = lower_agg_eq(agg_member(lhs, type, k), agg_member(rhs, type, k),
                                         fields[k].type);
            acc = k == 0 ? e : lower_logical(Inst::Tag::Land, acc, e);
        }
        return acc;
    }
    if (t.kind == Type::Kind::UnpackedArray) {
        // Element-wise compare AND-reduced with Land (LRM 11.4.5 X-propagation,
        // same reduction as the struct case). Position order is irrelevant to
        // the reduction, so walk 0..count-1.
        const uint32_t count = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
        Plane          acc{};
        for (uint32_t k = 0; k < count; ++k) {
            const Plane e = lower_agg_eq(agg_elem(lhs, type, k), agg_elem(rhs, type, k), t.element);
            acc = k == 0 ? e : lower_logical(Inst::Tag::Land, acc, e);
        }
        return acc;
    }
    if (t.kind == Type::Kind::String) {
        llvm::Value* c = b_.CreateCall(
          str_cmp_fn(), { lv_string_ptr(lhs, StringUse::Read), lv_string_ptr(rhs, StringUse::Read) });
        llvm::Value* bit = b_.CreateICmpEQ(c, b_.getInt32(0));
        return bit_plane(bit, bit);
    }
    if (t.kind == Type::Kind::Queue || t.kind == Type::Kind::DynArray) {
        const DynElem e = dynarr_elem(type);
        if (e.is_agg || e.is_handle || e.width > 64) {
            // The runtime comparator reads one element word from a flat
            // descriptor, which an aggregate / nested-container element does not
            // have and a wider-than-a-word element does not fit, so the walk
            // happens here: sizes must match, then each element pair compares with
            // this same recursion (which is also what makes a queue of queues
            // work).
            return emit_container_eq_loop(lhs, rhs, type, e);
        }
        // A queue / dynamic-array leaf of scalar elements compares element-wise
        // at runtime with §11.4.5 x-propagation; the runtime tristate (0 unequal,
        // 1 equal, 2 ambiguous) maps to a 1-bit 4-state plane (may = ts != 0,
        // must = ts == 1, so 2 -> must=0/may=1 = x).
        const ContainerFmt cf = container_fmt(type);
        llvm::Value*       ts   = b_.CreateCall(container_eq_fn(),
                                                { lv_handle_ptr(lhs, HandleUse::Read),
                                                  lv_handle_ptr(rhs, HandleUse::Read),
                                                  b_.getInt32(cf.is_queue),
                                                  b_.getInt32(cf.kind),
                                                  b_.getInt32(cf.width),
                                                  b_.getInt32(cf.is_signed),
                                                  b_.getInt32(cf.four),
                                                  b_.getInt32(cf.has_z),
                                                  b_.getInt32(cf.z_off),
                                                  b_.getInt64(cf.stride) });
        llvm::Value* may  = b_.CreateICmpNE(ts, b_.getInt8(0));
        llvm::Value* must = b_.CreateICmpEQ(ts, b_.getInt8(1));
        return bit_plane(must, may);
    }
    if (t.kind == Type::Kind::Assoc) {
        // LRM 11.4.5 / 7.9 (P4a): equal iff the same set of (key, value)
        // pairs. Reuses the P3 assoc-key-leaf canonical serializer (count ||
        // sorted (key, value) pairs) on each whole operand -- the same
        // property that makes that encoding "canonical" (two equal-content
        // maps produce identical bytes) is exactly what a byte compare here
        // needs. Bit-exact, so this answers === directly; the frontend
        // refuses plain == for a 4-state-capable value element, where the
        // X-propagating answer can differ from a flat byte compare.
        //
        // Both spans build into the SAME rt_keybuf_* region back to back, so
        // only the FINAL span (after both are written) is read -- an
        // intervening rt_keybuf_append can reallocate the buffer, which
        // would dangle a pointer captured mid-build; the byte offsets
        // (`mark0`/`mid`/`end`, plain u32s) stay valid regardless.
        llvm::Value* mark0     = b_.CreateCall(keybuf_begin_fn(), {});
        llvm::Value* xz_slot   = entry_alloca(b_.getInt1Ty(), "aeq.anyx");
        b_.CreateStore(b_.getInt1(false), xz_slot);
        emit_variable_agg_key_bytes(type, lhs, xz_slot);
        llvm::Value* mid = b_.CreateCall(keybuf_begin_fn(), {});
        emit_variable_agg_key_bytes(type, rhs, xz_slot);
        llvm::Value* end = b_.CreateCall(keybuf_begin_fn(), {});
        llvm::Value* ptr_slot = entry_alloca(raw_ptr_ty(), "aeq.ptr");
        llvm::Value* len_slot = entry_alloca(b_.getInt32Ty(), "aeq.len");
        b_.CreateCall(keybuf_span_fn(), { mark0, ptr_slot, len_slot });
        llvm::Value* aptr = b_.CreateLoad(raw_ptr_ty(), ptr_slot);
        llvm::Value* alen = b_.CreateSub(mid, mark0);
        llvm::Value* blen = b_.CreateSub(end, mid);
        llvm::Value* bptr = b_.CreateInBoundsGEP(b_.getInt8Ty(), aptr, alen);
        llvm::Value* eqb  = b_.CreateCall(key_span_eq_fn(), { aptr, alen, bptr, blen });
        b_.CreateCall(keybuf_end_fn(), { mark0 });
        llvm::Value* bit = b_.CreateICmpNE(eqb, b_.getInt8(0));
        return bit_plane(bit, bit);
    }
    const ValType vt = from_ref(type);
    // A leaf reached through a deferred associative-array element (`a[k] == x`
    // over an aggregate value) resolves its map slot here -- lower_agg_eq
    // reads leaves inline, bypassing the Load dispatch. An absent key reads
    // the leaf default (dyn_elem_load's out-of-range path).
    Plane pa = lhs.is_assoc ? dyn_elem_load(assoc_elem_lvalue(lhs, /*for_write=*/false))
                            : lower_load(lhs);
    Plane pb = rhs.is_assoc ? dyn_elem_load(assoc_elem_lvalue(rhs, /*for_write=*/false))
                            : lower_load(rhs);
    if (vt.is_real) {
        // A real leaf needs the FLOAT compare. `lower_compare` is the integer
        // one: handed a real plane it read the `double` bit pattern as an
        // integer image and walked planes that a real plane does not carry,
        // which segfaulted outright on `struct { real r; string s; }`. Reals
        // carry no x/z, so FEq's definite 1-bit result needs none of
        // lower_compare's unknown-plane machinery; this is the same helper the
        // scalar `==` reaches through Inst::Tag::FEq.
        return lower_real_compare(Inst::Tag::FEq, real_plane(planes_to_real(pa, vt.width)),
                                  real_plane(planes_to_real(pb, vt.width)));
    }
    return lower_compare(Inst::Tag::Eq, pa, pb, vt.is_signed);
}

Plane Backend::lower_compare(Inst::Tag tag, Plane a, Plane b, bool sign) {
    // The result is always a definite 4-state bit (compute_type), so lift a
    // 2-state operand into the packed encoding; with no x/z the any_unknown
    // guards below fold to a single native icmp.
    a = to_four(a);
    b = to_four(b);

    // Equality keeps a branchless mask form (and a width-1 LUT). LRM 11.4.5:
    // the result is X only if unknown bits make the relation AMBIGUOUS. A bit
    // position where both operands are known and differ decides it outright
    // (== is 0, != is 1) no matter what the x/z bits are; only "no definite
    // difference but some x/z bit" yields X. Unknown bits are exactly
    // must != may (z included), so with 2-state operands `unk` is the zero
    // constant and everything below folds to a single native icmp.
    if (tag == Inst::Tag::Eq || tag == Inst::Tag::Ne) {
        if (width_of(a) == 1) {
            // One bit position: any x/z operand bit is ambiguous, which is
            // exactly the LUT's "either X -> X" rule.
            return single_bit_lut(tag == Inst::Tag::Eq ? EQ1_LUT : NE1_LUT, a, b);
        }
        const uint32_t w    = width_of(a);
        llvm::Value*   am   = must_of(a);
        llvm::Value*   bm   = must_of(b);
        llvm::Value*   unk  = b_.CreateOr(b_.CreateXor(am, may_of(a)), b_.CreateXor(bm, may_of(b)));
        llvm::Value*   diff = b_.CreateICmpNE(b_.CreateAnd(b_.CreateXor(am, bm), b_.CreateNot(unk)), zero(w));
        llvm::Value*   ambiguous = b_.CreateAnd(b_.CreateNot(diff), b_.CreateICmpNE(unk, zero(w)));
        // diff -> definite 0/1; else ambiguous -> X; else all bits known
        // equal -> definite 1/0.
        if (tag == Inst::Tag::Eq) {
            llvm::Value* eq = b_.CreateNot(diff);
            return bit_plane(b_.CreateAnd(eq, b_.CreateNot(ambiguous)), eq);
        }
        return bit_plane(diff, b_.CreateOr(diff, ambiguous));
    }

    // Ordered relationals: any x/z yields X, exactly like arithmetic, so guard
    // on it. The hot block is one native compare on the data (must) bits; the
    // cold block is the all-X word (must 0, may 1).
    //
    // NOTE: Unlike ==/!=, this pessimism is normative, not a shortcut. LRM
    // 11.4.4: "If either operand of a relational operator contains an
    // unknown (x) or high-impedance (z) value, then the result shall be
    // 1'bx" — even when known high bits alone would decide the relation
    // (iverilog agrees). Do not "sharpen" this path.
    llvm::CmpInst::Predicate pred;
    switch (tag) {
        case Inst::Tag::Lt:
            pred = sign ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_ULT;
            break;
        case Inst::Tag::Le:
            pred = sign ? llvm::CmpInst::ICMP_SLE : llvm::CmpInst::ICMP_ULE;
            break;
        case Inst::Tag::Gt:
            pred = sign ? llvm::CmpInst::ICMP_SGT : llvm::CmpInst::ICMP_UGT;
            break;
        default:
            pred = sign ? llvm::CmpInst::ICMP_SGE : llvm::CmpInst::ICMP_UGE;
            break;
    }
    llvm::Value* unknown = b_.CreateOr(any_unknown(a), any_unknown(b));
    return branchy_bit(
      unknown, [&] { return llvm::ConstantInt::get(int_ty(2), 0b10); },
      [&] { return b_.CreateICmp(pred, must_of(a), must_of(b)); });
}

// -- Case-equality (SV `case`/`casez`/`casex`) -> definite 1-bit. --
//
// Unlike `lower_compare`, the result is always known (must == may): the case
// statement compares 4-state planes bit-exactly rather than propagating X.
//   CaseEq  (===): every bit of must/may/z must be equal.
//   CasezEq:  z bits in either operand are don't-care (mask = z_a | z_b).
//   CasexEq:  x and z bits in either operand are don't-care
//             (mask = unknown_a | unknown_b, since both x and z have must!=may).
Plane Backend::lower_case_compare(Inst::Tag tag, Plane a, Plane b) {
    const uint32_t w = std::max(width_of(a), width_of(b));
    // The result is a definite 4-state bit; lift operands to the packed form the
    // masking below needs (z / may planes materialized). 2-state operands carry
    // no x/z, so the case forms collapse to a plain == after folding.
    a = cast_plane(to_four(a), w, false);
    b = cast_plane(to_four(b), w, false);

    llvm::Value* eq;
    if (tag == Inst::Tag::CaseEq) {
        // === : bit-exact equality. The packed mm word carries both the must
        // and may planes, so a single icmp on the word covers them at once;
        // the z plane is compared alongside. One icmp per plane, no unpack.
        eq = b_.CreateAnd(b_.CreateICmpEQ(a.mm, b.mm), b_.CreateICmpEQ(a.z, b.z));
    } else {
        // casez/casex mask out don't-care bits, so they need the planes split.
        llvm::Value* am   = must_of(a);
        llvm::Value* aM   = may_of(a);
        llvm::Value* bm   = must_of(b);
        llvm::Value* bM   = may_of(b);
        llvm::Value* dc   = tag == Inst::Tag::CasezEq
                              ? b_.CreateOr(a.z, b.z)
                              : b_.CreateOr(b_.CreateXor(am, aM), b_.CreateXor(bm, bM));
        llvm::Value* keep = b_.CreateNot(dc);
        eq                = b_.CreateAnd(
          b_.CreateICmpEQ(b_.CreateAnd(am, keep), b_.CreateAnd(bm, keep)),
          b_.CreateICmpEQ(b_.CreateAnd(aM, keep), b_.CreateAnd(bM, keep)));
    }
    return bit_plane(eq, eq);
}

// Wildcard equality `==?`. LRM 1800-2023 §11.4.6: x/z in the RIGHT operand are
// wildcards (that bit position is not compared); the remaining positions are
// compared "as for the logical equality" operator, i.e. with 11.4.5's
// ambiguity rule. Masking both operands to the kept (non-wildcard) bits and
// running `==` gives exactly that: the right is known on kept bits so only a
// left x/z can make a kept position unknown, and a definite mismatch on any
// kept position still decides the result.
// `inside` (LRM 11.4.13): OR-reduce the wildcard-eq singular members and
// the >=/<= range members. lower_logical carries the LRM 11.4.5
// X-propagation of the reduction.
Plane Backend::lower_inside_set(Index i) {
    const auto& p    = ir_->payload(i).op_extra;
    const auto  info = ir_->extra_data<InsideSetInfo>(p.extra);
    const Plane sel  = value_of(p.op);
    const bool  sign = type_of(p.op).is_signed;

    std::optional<Plane> acc;
    const auto add = [&](Plane pred) {
        acc = acc ? lower_logical(Inst::Tag::Lor, *acc, pred) : pred;
    };
    for (const Index v : ir_->region(info.values)) {
        add(lower_wildeq(sel, value_of(v)));
    }
    const std::span<const Index> rngs = ir_->region(info.ranges);
    for (size_t k = 0; k + 1 < rngs.size(); k += 2) {
        const Plane ge = lower_compare(Inst::Tag::Ge, sel, value_of(rngs[k]), sign);
        const Plane le = lower_compare(Inst::Tag::Le, sel, value_of(rngs[k + 1]), sign);
        add(lower_logical(Inst::Tag::Land, ge, le));
    }
    return *acc;
}

Plane Backend::lower_wildeq(Plane a, Plane b) {
    const uint32_t w     = std::max(width_of(a), width_of(b));
    // Definite 4-state bit result; lift operands. 2-state operands have no
    // wildcard (x/z) bits, so ==? folds to a plain ==.
    a                    = cast_plane(to_four(a), w, false);
    b                    = cast_plane(to_four(b), w, false);
    auto*          wide  = int_ty(2 * w);
    llvm::Value*   unkb  = b_.CreateXor(must_of(b), may_of(b)); // x/z bits of b
    llvm::Value*   keep  = b_.CreateNot(unkb);
    llvm::Value*   keepW = b_.CreateOr(b_.CreateZExt(keep, wide),
                                       b_.CreateShl(b_.CreateZExt(keep, wide), w));
    Plane ma = { b_.CreateAnd(a.mm, keepW), b_.CreateAnd(a.z, keep) };
    Plane mb = { b_.CreateAnd(b.mm, keepW), b_.CreateAnd(b.z, keep) };
    return lower_compare(Inst::Tag::Eq, ma, mb, false);
}

Backend::Truth Backend::truth_of(Plane a) {
    const uint32_t w = width_of(a);
    llvm::Value*   t = b_.CreateICmpNE(must_of(a), zero(w));
    llvm::Value*   f = b_.CreateICmpEQ(may_of(a), zero(w));
    return { t, f };
}

Plane Backend::lower_logical(Inst::Tag tag, Plane a, Plane b) {
    // Definite 4-state 1-bit result; lift operands (self-determined, so either
    // may be 2-state) into the packed encoding. With no x/z the truth of each
    // operand is just "nonzero" and the body folds to a native boolean op.
    a = to_four(a);
    b = to_four(b);

    // At width 1 the truth of each operand is the bit itself, so &&/|| are the
    // monotone bitwise &/| — one packed op, no truth reduction. It also degrades
    // straight to a plain &/| once the may plane is elided, unlike a LUT.
    if (width_of(a) == 1 && width_of(b) == 1) {
        llvm::Value* mm =
          tag == Inst::Tag::Land ? b_.CreateAnd(a.mm, b.mm) : b_.CreateOr(a.mm, b.mm);
        return { mm, zero(1) };
    }

    // Multi-bit: with no x/z each operand's truth is just "nonzero", so &&/||
    // are a plain boolean on the data bits. An x/z is rarer and can still be
    // dominated (0 && x = 0), so the cold block keeps the full 3-valued logic.
    const uint32_t wa      = width_of(a);
    const uint32_t wb      = width_of(b);
    llvm::Value*   unknown = b_.CreateOr(any_unknown(a), any_unknown(b));
    return branchy_bit(
      unknown,
      [&] {
          const Truth ta = truth_of(a);
          const Truth tb = truth_of(b);
          return tag == Inst::Tag::Land
                   ? bit_plane(b_.CreateAnd(ta.t, tb.t), b_.CreateNot(b_.CreateOr(ta.f, tb.f))).mm
                   : bit_plane(b_.CreateOr(ta.t, tb.t), b_.CreateNot(b_.CreateAnd(ta.f, tb.f))).mm;
      },
      [&] {
          llvm::Value* at = b_.CreateICmpNE(must_of(a), zero(wa));
          llvm::Value* bt = b_.CreateICmpNE(must_of(b), zero(wb));
          return tag == Inst::Tag::Land ? b_.CreateAnd(at, bt) : b_.CreateOr(at, bt);
      });
}

Plane Backend::lower_lnot(Plane a) {
    // Definite 4-state 1-bit result; lift a 2-state operand (!a folds to a==0).
    a = to_four(a);
    // At width 1, !a is the same operation as ~a (negate the single truth bit),
    // so reuse the bnot plane swap+complement — cheaper than a truth pair, and
    // already optimal (one not + one rotate), so no LUT is warranted.
    if (width_of(a) == 1) {
        return lower_bnot(a);
    }
    const Truth t = truth_of(a);
    // true -> 0, false -> 1, x -> x: must = was-false, may = not was-true.
    return bit_plane(t.f, b_.CreateNot(t.t));
}

// Logical implication a -> b (LRM 11.4.7). Reached only on the
// not-short-circuited path -- lower_short_circuit already skips lowering
// b entirely when a is definitely false (LRM 11.3.5), so by the time this
// runs both operands are real values needing only the combine. Direct
// truth table of (!a || b) without materializing an intermediate Lnot:
// definitely true when a is definitely false OR b is definitely true
// (either half of the OR is settled); definitely false only when a is
// definitely true AND b is definitely false; x otherwise.
Plane Backend::lower_limplies(Plane a, Plane b) {
    a = to_four(a);
    b = to_four(b);
    const Truth ta = truth_of(a);
    const Truth tb = truth_of(b);
    llvm::Value* t = b_.CreateOr(ta.f, tb.t);
    llvm::Value* f = b_.CreateAnd(ta.t, tb.f);
    return bit_plane(t, b_.CreateNot(f));
}

// Logical equivalence a <-> b (LRM 11.4.7). Unlike && / || / ->, LRM
// 11.3.5 does not list <-> as short-circuiting, so the frontend always
// lowers both operands (lower_binary) before this runs. No domination
// shortcut exists either: definitely true when both operands share the
// same definite truth, definitely false when both are definite but
// differ, x whenever either operand is itself ambiguous (an ambiguous
// operand's two possible resolutions give different equivalence answers).
Plane Backend::lower_lequiv(Plane a, Plane b) {
    a = to_four(a);
    b = to_four(b);
    const Truth ta = truth_of(a);
    const Truth tb = truth_of(b);
    llvm::Value* t = b_.CreateOr(b_.CreateAnd(ta.t, tb.t), b_.CreateAnd(ta.f, tb.f));
    llvm::Value* f = b_.CreateOr(b_.CreateAnd(ta.t, tb.f), b_.CreateAnd(ta.f, tb.t));
    return bit_plane(t, b_.CreateNot(f));
}

Plane Backend::lower_select(Plane cond, Plane a, Plane b) {
    // 2-state arms: a native select. slang unifies the two arms to a common
    // type, so both are 2-state here. A 4-state condition may be x (LRM
    // 1800-2023 11.4.11: bitwise merge, x where the arms disagree); in a
    // 2-state result that x collapses to 0, which is exactly `a & b`.
    if (a.two && b.two) {
        llvm::Value* ct  = b_.CreateICmpNE(must_of(cond), zero(1));
        llvm::Value* sel = b_.CreateSelect(ct, a.mm, b.mm);
        if (cond.two) {
            return two_val(sel);
        }
        llvm::Value* known = b_.CreateICmpEQ(must_of(cond), may_of(cond));
        return two_val(b_.CreateSelect(known, sel, b_.CreateAnd(a.mm, b.mm)));
    }
    // Defensive: a lone 2-state arm is lifted so the packed body's direct plane
    // access is uniform.
    a = to_four(a);
    b = to_four(b);

    const uint32_t w          = width_of(a);
    llvm::Value*   cond_known = b_.CreateICmpEQ(must_of(cond), may_of(cond));
    llvm::Value*   cond_true  = b_.CreateICmpNE(must_of(cond), zero(1));

    llvm::Value* sel_mm = b_.CreateSelect(cond_true, a.mm, b.mm);
    llvm::Value* sel_z  = b_.CreateSelect(cond_true, a.z, b.z);

    // Unknown condition: interval hull per bit. Low half (must) ANDs, high half
    // (may) ORs; a low-bit mask keeps each half's result.
    llvm::Value* lowmask = llvm::ConstantInt::get(ctx_, llvm::APInt::getLowBitsSet(2 * w, w));
    llvm::Value* merge   = b_.CreateOr(b_.CreateAnd(b_.CreateAnd(a.mm, b.mm), lowmask),
                                       b_.CreateAnd(b_.CreateOr(a.mm, b.mm), b_.CreateNot(lowmask)));

    return { b_.CreateSelect(cond_known, sel_mm, merge), b_.CreateSelect(cond_known, sel_z, zero(w)) };
}

Plane Backend::lower_concat(Index i) {
    const uint32_t       w = type_of(i).width;
    std::array<Index, 4> scratch;
    const auto           parts = ir_->operands(i, scratch);

    // 2-state result: every part is 2-state (a concat is 4-state if any part
    // is). Assemble the single iW word directly.
    if (!type_of(i).four_state) {
        llvm::Value* acc   = zero(w);
        bool         first = true;
        for (Index part : parts) {
            const uint32_t pw = type_of(part).width;
            // Skip a zero-width part ({0{x}}): it contributes no bits and would
            // make the next part shift by the full result width (poison).
            if (pw == 0) {
                continue;
            }
            if (!first) {
                acc = b_.CreateShl(acc, pw);
            }
            acc   = b_.CreateOr(acc, cast(must_of(value_of(part)), w, false));
            first = false;
        }
        return two_val(acc);
    }

    llvm::Value* must = zero(w);
    llvm::Value* may  = zero(w);
    llvm::Value* z    = zero(w);
    // The first part is OR'd in without a preceding shift: a shift by the
    // full result width (which happens when the concat has a single operand,
    // pw == w) is poison in LLVM, and shifting the still-zero accumulator is
    // a no-op anyway. A 2-state part contributes must == may and a zero z
    // plane (z_of), so it packs into the 4-state result unchanged.
    bool first = true;
    for (Index part : parts) {
        const uint32_t pw = type_of(part).width;
        // Skip a zero-width part ({0{x}}): it contributes no bits and would
        // make the next part shift by the full result width (poison).
        if (pw == 0) {
            continue;
        }
        const Plane pv = value_of(part);
        if (!first) {
            must = b_.CreateShl(must, pw);
            may  = b_.CreateShl(may, pw);
            z    = b_.CreateShl(z, pw);
        }
        must  = b_.CreateOr(must, cast(must_of(pv), w, false));
        may   = b_.CreateOr(may, cast(may_of(pv), w, false));
        z     = b_.CreateOr(z, cast(z_of(pv), w, false));
        first = false;
    }
    return packed(must, may, z);
}

// {<<S{x}}: reverse the order of the operand's S-bit blocks, sliced from
// the LSB (a final partial block keeps its own width; block 0 lands at the
// MSB). Applied to each plane independently — a pure bit permutation.
llvm::Value* Backend::reverse_blocks(llvm::Value* v, uint32_t w, uint32_t slice) {
    const uint32_t n     = (w + slice - 1) / slice;
    llvm::Value*   acc   = zero(w);
    bool           first = true;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t lo = i * slice;
        const uint32_t bw = std::min<uint32_t>((i + 1) * slice, w) - lo;
        llvm::Value*   blk =
          b_.CreateAnd(b_.CreateLShr(v, lo),
                       llvm::ConstantInt::get(ctx_, llvm::APInt::getLowBitsSet(w, bw)));
        // Skip the shift before the first block: for a single block bw == w,
        // and a shift by the full width is poison in LLVM.
        if (!first) {
            acc = b_.CreateShl(acc, bw);
        }
        acc   = b_.CreateOr(acc, blk);
        first = false;
    }
    return acc;
}

Plane Backend::lower_stream_reverse(Plane v, uint32_t slice) {
    const uint32_t w = width_of(v);
    if (v.two) {
        return two_val(reverse_blocks(v.mm, w, slice));
    }
    return packed(reverse_blocks(must_of(v), w, slice), reverse_blocks(may_of(v), w, slice),
                  reverse_blocks(z_of(v), w, slice));
}

Plane Backend::lower_repl(Plane v, uint32_t part_width, uint32_t count) {
    // TODO: a zero replication count ({0{x}}) should yield a 0-width value;
    // here w becomes 0 and int_ty(0) is widened to i1, so it leaks one bit.
    const uint32_t w    = part_width * count;

    // 2-state: replicate the single word.
    if (v.two) {
        llvm::Value* p   = cast(v.mm, w, false);
        llvm::Value* acc = zero(w);
        for (uint32_t c = 0; c < count; ++c) {
            if (c != 0) {
                acc = b_.CreateShl(acc, part_width);
            }
            acc = b_.CreateOr(acc, p);
        }
        return two_val(acc);
    }

    llvm::Value*   pm   = cast(must_of(v), w, false);
    llvm::Value*   pM   = cast(may_of(v), w, false);
    llvm::Value*   pz   = cast(z_of(v), w, false);
    llvm::Value*   must = zero(w);
    llvm::Value*   may  = zero(w);
    llvm::Value*   z    = zero(w);
    // Skip the shift on the first copy: for a single replication ({1{x}})
    // part_width == w, and a shift by the full width is poison in LLVM.
    for (uint32_t c = 0; c < count; ++c) {
        if (c != 0) {
            must = b_.CreateShl(must, part_width);
            may  = b_.CreateShl(may, part_width);
            z    = b_.CreateShl(z, part_width);
        }
        must = b_.CreateOr(must, pm);
        may  = b_.CreateOr(may, pM);
        z    = b_.CreateOr(z, pz);
    }
    return packed(must, may, z);
}

} // namespace clotho::ir::backend
