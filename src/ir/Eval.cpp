#include "Eval.h"

#include <variant>

namespace clotho::ir {

namespace {

// Bitwise AND of partial values. A known 0 on either side makes the result bit
// known regardless of the other -- that absorption is the point of tracking a
// mask per bit rather than a single known/unknown flag for the whole value.
EvalValue band_of(const EvalValue& a, const EvalValue& b) {
    const uint32_t w      = a.width;
    const uint64_t zero_a = a.mask & ~a.bits; // bits known to be 0 in a
    const uint64_t zero_b = b.mask & ~b.bits;

    return { (a.bits & b.bits) & EvalValue::width_mask(w),
             (zero_a | zero_b | (a.mask & b.mask)) & EvalValue::width_mask(w),
             w };
}

// Dual of band_of: a known 1 on either side determines the result bit.
EvalValue bor_of(const EvalValue& a, const EvalValue& b) {
    const uint32_t w     = a.width;
    const uint64_t one_a = a.mask & a.bits;
    const uint64_t one_b = b.mask & b.bits;

    return { (a.bits | b.bits) & EvalValue::width_mask(w),
             (one_a | one_b | (a.mask & b.mask)) & EvalValue::width_mask(w),
             w };
}

// XOR has no absorbing value, so a bit is known only when both inputs are.
EvalValue bxor_of(const EvalValue& a, const EvalValue& b) {
    const uint32_t w    = a.width;
    const uint64_t both = a.mask & b.mask;

    return { (a.bits ^ b.bits) & both, both & EvalValue::width_mask(w), w };
}

Tri truth_of(const EvalValue& v) {
    if ((v.mask & v.bits) != 0) {
        return Tri::True; // a definitely-1 bit dominates (Table 11-17, row 1)
    }
    if (v.fully_known()) {
        return Tri::False; // every bit known and none set
    }

    return Tri::Unknown;
}

EvalValue from_tri(Tri t) {
    switch (t) {
        case Tri::True:
            return EvalValue::known(1, 1);
        case Tri::False:
            return EvalValue::known(0, 1);
        case Tri::Unknown:
            break;
    }

    return EvalValue::unknown(1);
}

} // namespace

Eval::Eval(const Module& module, const Ir& ir, Signal pinned, uint64_t pinned_bits)
  : module_(module)
  , ir_(ir)
  , pins_{ Pin{ pinned, pinned_bits } }
  , memo_(ir.inst_count())
  , done_(ir.inst_count(), 0) {}

Eval::Eval(const Module& module, const Ir& ir, std::span<const Pin> pins)
  : module_(module)
  , ir_(ir)
  , pins_(pins.begin(), pins.end())
  , memo_(ir.inst_count())
  , done_(ir.inst_count(), 0) {}

// Decode an interned constant into partial bits. A 4-state value's canonical
// bytes are [val plane | xz plane]; any bit with its xz bit set is x or z, which
// this reports as unknown rather than trying to model -- the consumers all want a
// 2-state answer and an x bit genuinely is not one.
EvalValue Eval::constant_value(Ref ref) const {
    const auto& item = module_.pool.get(ref);
    const auto* val  = std::get_if<Value>(&item);
    if (val == nullptr) {
        return EvalValue::unknown(0);
    }
    const Layout lay = module_.pool.layout(val->type);
    if (lay.width == 0 || lay.width > 64) {
        return EvalValue::unknown(lay.width);
    }
    const std::span<const uint8_t> bytes = module_.pool.bytes(ref);
    const size_t                   plane = lay.domain == Domain::Four ? bytes.size() / 2 : bytes.size();

    uint64_t bits = 0;
    uint64_t xz   = 0;
    for (size_t k = 0; k < plane && k < 8; ++k) {
        bits |= static_cast<uint64_t>(bytes[k]) << (8 * k);
        if (lay.domain == Domain::Four) {
            xz |= static_cast<uint64_t>(bytes[plane + k]) << (8 * k);
        }
    }
    const uint64_t m = EvalValue::width_mask(lay.width);

    return { bits & ~xz & m, ~xz & m, lay.width };
}

EvalValue Eval::value(Index i) {
    const auto raw = static_cast<uint32_t>(i);
    if (raw >= done_.size()) {
        return EvalValue::unknown(0);
    }
    if (done_[raw] == 0) {
        done_[raw] = 1; // set first: a malformed self-reference yields unknown, not a hang
        memo_[raw] = compute(i);
    }

    return memo_[raw];
}

Tri Eval::truth(Index i) {
    return truth_of(value(i));
}

EvalValue Eval::compute(Index i) {
    const Inst::Payload& p     = ir_.payload(i);
    const uint32_t       width = 0; // filled per-arm where it matters

    switch (ir_.tag(i)) {
        case Inst::Tag::Constant:
            return constant_value(p.ref);

        // A signal read is where folding stops. The pinned signal is the one thing
        // known; everything else is state this evaluator deliberately cannot see.
        case Inst::Tag::Load: {
            const Index src = p.unary;
            if (ir_.tag(src) != Inst::Tag::Signal) {
                return EvalValue::unknown(0);
            }
            const Signal s   = ir_.payload(src).signal;
            const Layout lay = module_.pool.layout(module_.signal(s).type);
            if (lay.width == 0 || lay.width > 64) {
                return EvalValue::unknown(lay.width);
            }
            // Linear over the pins: there is one for the clock at most, plus one
            // per async trigger, so the list is two or three entries.
            for (const Pin& p : pins_) {
                if (p.signal == s) {
                    return EvalValue::known(p.bits, lay.width);
                }
            }

            return EvalValue::unknown(lay.width);
        }

        case Inst::Tag::Bnot: {
            const EvalValue a = value(p.unary);

            return { ~a.bits & a.mask, a.mask, a.width };
        }
        case Inst::Tag::Band:
            return band_of(value(p.binary.l), value(p.binary.r));
        case Inst::Tag::Bor:
            return bor_of(value(p.binary.l), value(p.binary.r));
        case Inst::Tag::Bxor:
            return bxor_of(value(p.binary.l), value(p.binary.r));

        case Inst::Tag::RedOr:
            return from_tri(truth_of(value(p.unary)));
        case Inst::Tag::RedAnd: {
            const EvalValue a = value(p.unary);
            if ((a.mask & ~a.bits) != 0) {
                return EvalValue::known(0, 1); // a definitely-0 bit dominates
            }

            return a.fully_known() ? EvalValue::known(1, 1) : EvalValue::unknown(1);
        }
        case Inst::Tag::Lnot: {
            switch (truth_of(value(p.unary))) {
                case Tri::True:
                    return EvalValue::known(0, 1);
                case Tri::False:
                    return EvalValue::known(1, 1);
                case Tri::Unknown:
                    break;
            }

            return EvalValue::unknown(1);
        }
        // Logical AND/OR absorb: one definitely-false operand settles `&&`
        // whatever the other is, which is what makes `rst && en` fold to `en`
        // when rst is pinned high.
        case Inst::Tag::Land: {
            const Tri l = truth_of(value(p.binary.l));
            const Tri r = truth_of(value(p.binary.r));
            if (l == Tri::False || r == Tri::False) {
                return EvalValue::known(0, 1);
            }

            return (l == Tri::True && r == Tri::True) ? EvalValue::known(1, 1) : EvalValue::unknown(1);
        }
        case Inst::Tag::Lor: {
            const Tri l = truth_of(value(p.binary.l));
            const Tri r = truth_of(value(p.binary.r));
            if (l == Tri::True || r == Tri::True) {
                return EvalValue::known(1, 1);
            }

            return (l == Tri::False && r == Tri::False) ? EvalValue::known(0, 1) : EvalValue::unknown(1);
        }

        // Equality: a disagreeing bit that both sides know settles it as false
        // even when the rest is unknown. Only a fully-known pair can settle true.
        case Inst::Tag::Eq:
        case Inst::Tag::Ne:
        case Inst::Tag::CaseEq: {
            const EvalValue a     = value(p.binary.l);
            const EvalValue b     = value(p.binary.r);
            const bool      is_ne = ir_.tag(i) == Inst::Tag::Ne;
            const uint64_t  both  = a.mask & b.mask;
            if (((a.bits ^ b.bits) & both) != 0) {
                return EvalValue::known(is_ne ? 1 : 0, 1);
            }
            if (a.fully_known() && b.fully_known() && a.width == b.width) {
                return EvalValue::known(is_ne ? 0 : 1, 1);
            }

            return EvalValue::unknown(1);
        }

        // A conversion keeps the low bits it has and knows nothing above them; a
        // zero-extension of a fully known value is known, any other widening
        // leaves the new bits unknown.
        case Inst::Tag::ZExt:
        case Inst::Tag::SExt:
        case Inst::Tag::Trunc:
        case Inst::Tag::ToFour:
        case Inst::Tag::ToTwo: {
            const EvalValue a  = value(p.op_type.op);
            const Layout    to = module_.pool.layout(p.op_type.type);
            if (to.width == 0 || to.width > 64) {
                return EvalValue::unknown(to.width);
            }
            const uint64_t keep = EvalValue::width_mask(to.width < a.width ? to.width : a.width);
            const uint64_t m    = EvalValue::width_mask(to.width);
            uint64_t       mask = a.mask & keep;
            if (to.width > a.width && ir_.tag(i) == Inst::Tag::ZExt && a.fully_known()) {
                mask = m; // zero-extension of a known value is known
            }

            return { a.bits & mask, mask & m, to.width };
        }

        // The eager ternary. Same rule as the lazy select below, and it needs to
        // be here because a `?:` written in a reset guard is ordinary source --
        // and because materialize() renders a lazy select AS a ternary, so
        // without this a rendered design classifies differently from the one it
        // was rendered from.
        case Inst::Tag::Select: {
            const auto info = ir_.extra_data<Select>(p.op_extra.extra);
            switch (truth_of(value(p.op_extra.op))) {
                case Tri::True:
                    return value(info.a);
                case Tri::False:
                    return value(info.b);
                case Tri::Unknown:
                    break;
            }
            const Tri ta = truth_of(value(info.a));
            if (ta != Tri::Unknown && ta == truth_of(value(info.b))) {
                return ta == Tri::True ? EvalValue::known(1, 1) : EvalValue::known(0, 1);
            }

            return EvalValue::unknown(1);
        }

        // Short-circuit `&&` / `||`. A known predicate takes its arm; an unknown
        // one still folds when both arms AGREE, which is the same absorption Bor
        // and Lor do one level down. `rst || rst2` with rst2 pinned is exactly
        // this: the taken arm computes `rst | 1` and the other is the literal 1,
        // so the result is 1 without knowing rst. Without it the classic
        // multi-reset idiom cannot be folded at all.
        case Inst::Tag::SelectLazy: {
            const auto info = ir_.extra_data<SelectLazy>(p.op_extra.extra);
            switch (truth_of(value(p.op_extra.op))) {
                case Tri::True:
                    return value(info.then_val);
                case Tri::False:
                    return value(info.else_val);
                case Tri::Unknown:
                    break;
            }
            const EvalValue t = value(info.then_val);
            const EvalValue e = value(info.else_val);
            // Only the truth matters to every consumer of a lazy select, so
            // agreement is compared on the 1-bit verdict rather than the bits.
            const Tri tt = truth_of(t);
            if (tt != Tri::Unknown && tt == truth_of(e)) {
                return tt == Tri::True ? EvalValue::known(1, 1) : EvalValue::known(0, 1);
            }

            return EvalValue::unknown(1);
        }

        // Everything else is unknown, and that is sound: every consumer reads
        // unknown as "could not prove". A new opcode therefore degrades this
        // analysis rather than breaking it, which is the opposite of the
        // exhaustive tables in Ir.h and is the right default HERE specifically.
        default:
            break;
    }
    (void)width;

    return EvalValue::unknown(0);
}

Tri truth_with_pin(const Module& m, const Ir& ir, Index i, Signal pinned, uint64_t bits) {
    Eval e(m, ir, pinned, bits);

    return e.truth(i);
}

} // namespace clotho::ir
