// Nets: multi-driver resolution, continuous assignment, force/release, PCA and gate primitives.

#include "lower/Lowerer.h"

#include <bit>
#include <cassert>

namespace clotho::lower {

namespace {

// Strength levels the MOS check tracks, as one bit each, low to high: the
// charge-like small and medium exist only as results of the reduction in LRM
// Table 28-8, the others are the drive strengths of 28.11. A strength level here
// is the strength a switch's data input arrives with.
constexpr uint8_t kSmall    = 1 << 0;
constexpr uint8_t kMedium   = 1 << 1;
constexpr uint8_t kWeak     = 1 << 2;
constexpr uint8_t kPull     = 1 << 3;
constexpr uint8_t kStrong   = 1 << 4;
constexpr uint8_t kSupply   = 1 << 5;
constexpr uint8_t kAnyLevel = kSmall | kMedium | kWeak | kPull | kStrong | kSupply;

// What nmos, pmos and cmos do to a strength (LRM 28.13): pass it on, except that
// supply becomes strong.
uint8_t mos_pass_levels(uint8_t in) {
    return static_cast<uint8_t>((in & ~kSupply) | ((in & kSupply) != 0 ? kStrong : 0));
}

// What the resistive switches do (LRM 28.14, Table 28-8): supply and strong become
// pull, pull weak, weak medium, medium and small small.
uint8_t mos_reduce_levels(uint8_t in) {
    uint8_t out = 0;
    out |= (in & (kSupply | kStrong)) != 0 ? kPull : 0;
    out |= (in & kPull) != 0 ? kWeak : 0;
    out |= (in & kWeak) != 0 ? kMedium : 0;
    out |= (in & (kMedium | kSmall)) != 0 ? kSmall : 0;

    return out;
}

// Whether a packed drive strength pair can drive a value weaker than strong: a side
// that drives (not highz) at weak or pull. Supply is reduced to strong by a MOS
// switch (LRM 28.13), so it is not weak here.
bool strength_may_be_weak(uint16_t packed) {
    const auto weak = [](uint32_t level) { return level == 1 || level == 2; };

    return weak(packed & 0xFF) || weak(packed >> 8);
}

} // namespace

// The multi-driver reducer for a net kind: wire/tri/uwire resolve to X on a
// 0-vs-1 conflict; wand/triand fold wired-AND, wor/trior wired-OR (LRM 6.6).
// Other kinds never reach here (collect_scope_signals rejects them).
ir::Inst::Tag Lowerer::net_resolve_tag(NetType::NetKind kind) {
    switch (kind) {
        case NetType::WAnd:
        case NetType::TriAnd:
            return ir::Inst::Tag::WandRes;
        case NetType::WOr:
        case NetType::TriOr:
            return ir::Inst::Tag::WorRes;
        default:
            // wire / tri / uwire, and tri0 / tri1 (whose drivers resolve as
            // wire before the pull fills undriven bits, net_pull_value).
            return ir::Inst::Tag::WireRes;
    }
}

// The resistive-pull value of a tri0 / tri1 net (LRM 6.6.4), or nullopt for a
// net kind with no pull.
std::optional<int> Lowerer::net_pull_value(NetType::NetKind kind) {
    switch (kind) {
        case NetType::Tri0:
        case NetType::Supply0:
            return 0;
        case NetType::Tri1:
        case NetType::Supply1:
            return 1;
        default:
            return std::nullopt;
    }
}

// Strength level of a net kind's built-in pull: Supply (4) for supply0/1,
// Pull (2) for tri0/1. Only meaningful when net_pull_value has a value.
uint8_t Lowerer::net_pull_strength(NetType::NetKind kind) {
    return (kind == NetType::Supply0 || kind == NetType::Supply1) ? 4 : 2;
}

// Record that `reg` of a net gained a continuous driver; fail on overlap
// with a previously recorded driver. Two regions overlap iff their element
// bands intersect AND their bit ranges intersect. Non-net symbols are
// ignored (slang already rejects multiple continuous drivers on variables).
void Lowerer::record_net_driver_region(const ValueSymbol& raw, const NetRegion& reg) {
    // Canonicalize first: a net driven through an interface modport output
    // (`assign p.sig = …`) arrives as a ModportPort proxy (kind != Net) that
    // would otherwise bypass driver tracking — the underlying interface net
    // still needs multi-driver detection.
    const ValueSymbol& sym = canonical_value_symbol(raw);
    if (sym.kind != SymbolKind::Net) {
        return;
    }
    const auto       net  = static_cast<uint32_t>(signal_of(sym));
    const NetSymbol& nsym = sym.as<NetSymbol>();
    // A tri0/tri1 net needs a resolver from its very first driver: a single
    // tri-state driver still pulls to 0/1 whenever it is off. Other kinds
    // resolve only once a second driver overlaps an existing one.
    const bool has_pull = net_pull_value(nsym.netType.netKind).has_value();
    auto&      regions  = net_driven_[net];
    bool       resolve  = has_pull;
    for (const NetRegion& p : regions) {
        if (reg.overlaps(p)) {
            // Emit routes every driver of a resolved net to its own slot and
            // folds them through the net-kind's reducer (wire -> X on
            // conflict, wand/wor -> wired-AND/OR), then applies any pull.
            resolve = true;
            break;
        }
    }
    if (resolve && resolved_nets_.insert(net).second) {
        net_resolve_op_[net] = net_resolve_tag(nsym.netType.netKind);
    }
    regions.push_back(reg);
}

// A whole-net driver (whole named net, dynamic select, port copy, net-decl
// init): covers every element and every bit, so it conflicts with any other
// driver of the same net.
void Lowerer::record_net_driver_whole(const ValueSymbol& sym) {
    record_net_driver_region(sym, kWholeNet);
}

// Force a net to resolve through a resolver process even with a single driver
// (a pullup/pulldown is a lone permanent driver whose value must still fold by
// strength against any other driver). Mirrors the tri0/tri1 has_pull path.
void Lowerer::force_net_resolved(ir::Signal net) {
    const auto net_u = static_cast<uint32_t>(net);
    if (resolved_nets_.insert(net_u).second) {
        net_resolve_op_[net_u] = ir::Inst::Tag::WireRes;
    }
}

// The root value symbol of an lvalue select chain, or null for shapes with
// no single named root.
const ValueSymbol* Lowerer::lvalue_root(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
            return &e.as<ValueExpressionBase>().symbol;
        case ExpressionKind::ElementSelect:
            return lvalue_root(e.as<ElementSelectExpression>().value());
        case ExpressionKind::RangeSelect:
            return lvalue_root(e.as<RangeSelectExpression>().value());
        case ExpressionKind::MemberAccess:
            return lvalue_root(e.as<MemberAccessExpression>().value());
        case ExpressionKind::Conversion:
            return lvalue_root(e.as<ConversionExpression>().operand());
        default:
            return nullptr;
    }
}

// An elaboration-time constant (parameter/localparam, enum member, genvar,
// specparam) has a value but no signal storage. A whole reference folds at
// the NamedValue case; a bit/part/element-select of one does not root at a
// signal, so it must be folded rather than routed through signal_of.
bool Lowerer::is_elaboration_constant(const ValueSymbol& s) {
    switch (s.kind) {
        case SymbolKind::Parameter:
        case SymbolKind::EnumValue:
        case SymbolKind::Genvar:
        case SymbolKind::Specparam:
            return true;
        default:
            return false;
    }
}

// Fold a select whose base is an elaboration constant (e.g. `P[W-1:0]`,
// LRM 11.2.1) into a constant. Returns nullopt when the base is signal-
// backed (normal path) or when the select itself is not constant — e.g. a
// runtime index into a constant array, which stays an unsupported gap
// rather than a hard eval failure here.
std::optional<ir::Index> Lowerer::try_lower_constant_select(const Expression& expr, ir::Builder& b, ir::SourceLoc l) {
    const ValueSymbol* root = lvalue_root(expr);
    if (root == nullptr || !is_elaboration_constant(*root)) {
        return std::nullopt;
    }
    const slang::ConstantValue cv = fold_const(expr, Fold::Runtime);
    if (cv.bad()) {
        return std::nullopt;
    }

    return b.constant(lower_constant(cv, *expr.type), l);
}

// Provide a select's base as an lvalue. A signal-backed base lowers the
// usual way; a packed elaboration-constant base (a parameter with no
// storage, indexed by a runtime value — e.g. `PMP_HARDWIRED[i]` with a
// procedural loop variable in hazard3_pmp.v) is spilled into a fresh temp
// cell so the runtime bit/part-select has something to read. A constant
// select of such a base is already folded upstream by
// try_lower_constant_select, so this path is the runtime-index case only.
ir::Index Lowerer::select_base_lvalue(const Expression& base, ir::Builder& b, ir::SourceLoc l) {
    const ValueSymbol* root = lvalue_root(base);
    if (root != nullptr && is_elaboration_constant(*root) && base.type->getCanonicalType().isIntegral()) {
        const slang::ConstantValue cv = fold_const(base, Fold::Runtime);
        if (!cv.bad()) {
            const ir::Index tmp = make_arg_temp(lower_type(*base.type), b, l);
            b.binary(ir::Inst::Tag::Store, tmp, b.constant(lower_constant(cv, *base.type), l), l);
            return tmp;
        }
    }

    // A packed concatenation / replication has no lvalue root to address, so
    // a bit/part-select of it (`{a,b}[7:0]`, `{2{a}}[i]`) is spilled into a
    // fresh temp cell for the runtime select to read — same shape as the
    // elaboration-constant spill above. A fully-constant select folds
    // upstream (try_lower_constant_select), so this is the runtime case only.
    if ((base.kind == ExpressionKind::Concatenation || base.kind == ExpressionKind::Replication) &&
        base.type->getCanonicalType().isIntegral()) {
        const ir::Index tmp = make_arg_temp(lower_type(*base.type), b, l);
        b.binary(ir::Inst::Tag::Store, tmp, lower_expr(base, b), l);
        return tmp;
    }

    return lower_lvalue(base, b);
}

// Record the net-driver coverage of one continuous-assignment lvalue. A
// concatenation drives each operand; any other rooted lvalue drives the
// region computed by net_region_of.
void Lowerer::record_net_driver(const Expression& lhs) {
    switch (lhs.kind) {
        case ExpressionKind::Concatenation:
            for (const Expression* op : lhs.as<ConcatenationExpression>().operands()) {
                record_net_driver(*op);
            }
            return;
        case ExpressionKind::Conversion:
            record_net_driver(lhs.as<ConversionExpression>().operand());
            return;
        default:
            break;
    }
    const ValueSymbol* root = lvalue_root(lhs);
    if (root == nullptr || root->kind != SymbolKind::Net) {
        // Non-net or shapes rejected by lower_lvalue: nothing to track.
        return;
    }
    record_net_driver_region(*root, net_region_of(lhs, *root));
}

// Compute the driven region of a rooted continuous-assign lvalue: an
// unpacked-array element band crossed with a packed bit range within one
// element. Constant element-selects consume the outer unpacked dimensions
// (so distinct constant indices are disjoint, and a partial select spans
// the remaining dimensions); a trailing constant bit/part/member select
// narrows the bit range. Any dynamic index or unsupported shape falls back
// to the whole net, which conflicts with every other driver.
Lowerer::NetRegion Lowerer::net_region_of(const Expression& lhs, const ValueSymbol& root) {
    // Collect select steps leaf-first, then reverse to root-first — the
    // order that consumes outer unpacked dimensions before inner packed bits
    // (`mem[i][bit]` nests the unpacked select below the packed one).
    slang::SmallVector<const Expression*, 4> steps;
    for (const Expression* cur = &lhs;;) {
        if (cur->kind == ExpressionKind::ElementSelect) {
            steps.push_back(cur);
            cur = &cur->as<ElementSelectExpression>().value();
        } else if (cur->kind == ExpressionKind::RangeSelect) {
            steps.push_back(cur);
            cur = &cur->as<RangeSelectExpression>().value();
        } else if (cur->kind == ExpressionKind::MemberAccess) {
            steps.push_back(cur);
            cur = &cur->as<MemberAccessExpression>().value();
        } else if (cur->kind == ExpressionKind::Conversion) {
            cur = &cur->as<ConversionExpression>().operand();
        } else {
            break;
        }
    }
    std::reverse(steps.begin(), steps.end());

    // Unpacked dimensions of the net, outermost first.
    slang::SmallVector<slang::ConstantRange, 4> dims;
    const slang::ast::Type*                     et = &root.getType().getCanonicalType();
    while (const auto* arr = et->as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
        dims.push_back(arr->range);
        et = &arr->elementType.getCanonicalType();
    }
    const size_t m = dims.size();

    // Suffix products: sp[k] = product of dim widths k..m-1, sp[m] = 1. Dim
    // k index p contributes p * sp[k+1] to the ordinal; a select consuming
    // t dims leaves an sp[t]-element band.
    std::vector<uint64_t> sp(m + 1, 1);
    for (size_t k = m; k-- > 0;) {
        sp[k] = sp[k + 1] * static_cast<uint64_t>(dims[k].width());
    }

    uint64_t base    = 0;
    size_t   t       = 0; // unpacked dims consumed so far
    uint32_t bit_lo  = 0;
    uint32_t bit_hi  = UINT32_MAX;
    bool     bit_set = false;
    for (const Expression* step : steps) {
        if (t < m) {
            if (step->kind != ExpressionKind::ElementSelect) {
                return kWholeNet;
            }
            const std::optional<int32_t> c =
              try_const_int(step->as<ElementSelectExpression>().selector(), Fold::Required);
            if (!c) {
                return kWholeNet;
            }
            base += static_cast<uint64_t>(dims[t].translateIndex(*c)) * sp[t + 1];
            ++t;
            continue;
        }
        // Trailing packed select(s) on the element. Constant selects compose:
        // each narrows within the sub-vector the previous select isolated
        // (`v[level][bit]` -> bit `level*W + i`), so the range accumulates
        // relative to the current window base (bit_lo). A non-constant select
        // cannot pin a static range and widens to the whole element (a
        // conservative reject downstream via packed_const).
        if (const auto br = packed_bit_range(*step)) {
            const uint32_t base_bit = bit_set ? bit_lo : 0;
            bit_lo                  = base_bit + br->first;
            bit_hi                  = base_bit + br->second;
        } else {
            bit_lo = 0;
            bit_hi = UINT32_MAX;
        }
        bit_set = true;
    }

    return NetRegion{ base, base + sp[t] - 1, bit_lo, bit_hi };
}

// Constant packed bit range of a trailing select on an element: a 1-bit
// element-select -> one bit; a simple part-select -> [min, max]; a struct
// member -> its declared field offset span (LRM 7.2.1). nullopt for a
// dynamic or unsupported shape (the caller then covers the whole element).
std::optional<std::pair<uint32_t, uint32_t>> Lowerer::packed_bit_range(const Expression& step) {
    switch (step.kind) {
        case ExpressionKind::ElementSelect: {
            const ElementSelectExpression& es = step.as<ElementSelectExpression>();
            // A constant element select of a packed vector (a 1-bit select) or
            // of a multi-dimensional packed array (a whole W-bit sub-vector at
            // stride W). `es.type` is the selected element's width — 1 for a
            // bit select, W for a sub-vector — so ordinal * W is its low bit.
            if (!es.value().type->isIntegral()) {
                return std::nullopt;
            }
            if (const std::optional<int32_t> c = try_const_int(es.selector(), Fold::Required)) {
                const auto w   = static_cast<uint32_t>(es.type->getBitWidth());
                const auto ord = static_cast<uint32_t>(packed_range_of(es.value()).translateIndex(*c));
                return std::make_pair(ord * w, ord * w + w - 1);
            }
            return std::nullopt;
        }
        case ExpressionKind::RangeSelect: {
            const RangeSelectExpression& rs = step.as<RangeSelectExpression>();
            if (!rs.value().type->isIntegral()) {
                return std::nullopt;
            }
            // Element bit width (W): >1 for a multi-dim packed array slice, 1
            // for an ordinary vector. Bit offsets scale by W, mirroring
            // lower_range_select.
            const auto*    pa = rs.value().type->getCanonicalType().as_if<slang::ast::PackedArrayType>();
            const uint32_t w  = pa != nullptr ? static_cast<uint32_t>(pa->elementType.getBitWidth()) : 1;
            const slang::ConstantRange range = packed_range_of(rs.value());
            if (rs.getSelectionKind() == RangeSelectionKind::Simple) {
                const std::optional<int32_t> left  = try_const_int(rs.left(), Fold::Required);
                const std::optional<int32_t> right = try_const_int(rs.right(), Fold::Required);
                if (!left || !right) {
                    return std::nullopt;
                }
                const auto a = static_cast<uint32_t>(range.translateIndex(*left)) * w;
                const auto b = static_cast<uint32_t>(range.translateIndex(*right)) * w;
                return std::make_pair(std::min(a, b), std::max(a, b) + w - 1);
            }
            // Indexed part-select (`+:` / `-:`, LRM 11.5.1) with a constant
            // base is a static bit range: the base element's offset (biased by
            // count-1 when the window extends away from the LSB end) scaled by
            // W, spanning count*W bits. A non-constant base is not slot-able.
            const std::optional<int32_t> base = try_const_int(rs.left(), Fold::Required);
            if (!base) {
                return std::nullopt;
            }
            const auto    count = static_cast<uint32_t>(
              const_uint(rs.right(), "an indexed part-select width (LRM 11.5.1)"));
            const bool    down  = rs.getSelectionKind() == RangeSelectionKind::IndexedDown;
            const int32_t bias  = down == range.isDescending() ? static_cast<int32_t>(count) - 1 : 0;
            const auto    lo    = static_cast<uint32_t>(range.translateIndex(*base) - bias) * w;
            return std::make_pair(lo, lo + count * w - 1);
        }
        case ExpressionKind::MemberAccess: {
            const MemberAccessExpression& ma    = step.as<MemberAccessExpression>();
            const auto*                   field = ma.member.as_if<FieldSymbol>();
            if (field == nullptr) {
                return std::nullopt;
            }
            const auto lsb = static_cast<uint32_t>(field->bitOffset);
            const auto msb = lsb + static_cast<uint32_t>(ma.type->getBitWidth()) - 1;
            return std::make_pair(lsb, msb);
        }
        default:
            return std::nullopt;
    }
}

// Whether an lvalue is a whole-net reference (the entire net, no element /
// range / member select). A whole-net driver writes every bit of its slot,
// so V1 resolution needs no per-bit undriven handling. A select of any kind
// is a partial driver (deferred).
bool Lowerer::is_whole_net_lvalue(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
            return true;
        case ExpressionKind::Conversion:
            return is_whole_net_lvalue(e.as<ConversionExpression>().operand());
        default:
            return false;
    }
}

// The resolved net an lvalue roots at, or nullopt when the net has a single
// driver (the common case) or the lvalue does not root at a net.
std::optional<ir::Signal> Lowerer::resolved_net_of(const Expression& lhs) {
    const ValueSymbol* root = lvalue_root(lhs);
    if (root == nullptr) {
        return std::nullopt;
    }
    const ValueSymbol& sym = canonical_value_symbol(*root);
    if (sym.kind != SymbolKind::Net) {
        return std::nullopt;
    }
    const auto net = signal_of(sym);
    if (!resolved_nets_.contains(static_cast<uint32_t>(net))) {
        return std::nullopt;
    }
    return net;
}

// Allocate a fresh full-width driver slot for one driver of resolved `net`
// and register it (in the net's own scope) for the resolver to fold. Slots
// power up like any signal; a tri-state driver writes its own Z, and a
// driver that always writes the whole net leaves no undriven bits.
// Allocate a fresh full-width slot for one driver of `net` (slot type == net
// type, so a partial driver's slot still spans the whole net with z in its
// undriven bits). Registered in the net's scope; cross-scope drivers fail.
// Drive-strength level (LRM 28.11), ordered HighZ < Weak < Pull < Strong <
// Supply; a default (unspecified) driver is Strong. Only these definite drive
// strengths are modeled (charge strengths large/medium/small are trireg, not
// supported). A strength is a compile-time constant threaded to the resolver.
uint8_t Lowerer::strength_level(const std::optional<slang::ast::DriveStrength>& s) {
    if (!s.has_value()) {
        return 3; // default Strong
    }
    switch (*s) {
        case slang::ast::DriveStrength::Supply:
            return 4;
        case slang::ast::DriveStrength::Strong:
            return 3;
        case slang::ast::DriveStrength::Pull:
            return 2;
        case slang::ast::DriveStrength::Weak:
            return 1;
        case slang::ast::DriveStrength::HighZ:
            return 0;
    }
    return 3;
}

// Pack a driver's (strength0, strength1) pair into one u16 for storage parallel
// to its slot; high byte = strength0 (0-value drive), low byte = strength1.
uint16_t Lowerer::pack_strength(
  const std::pair<std::optional<slang::ast::DriveStrength>, std::optional<slang::ast::DriveStrength>>& ds) {
    return static_cast<uint16_t>((strength_level(ds.first) << 8) | strength_level(ds.second));
}

// `program_driver` is for the caller that knows the answer without
// `in_program_`: a program's PORT copy is emitted by wire_ports while the
// PARENT instance's members lower, so the flag there is the parent's answer
// -- false, since a program can only be instantiated in a design scope. The
// same split, for the same reason, as add_continuous_process.
ir::Signal Lowerer::alloc_driver_slot(ir::Signal net, ir::SourceLoc l, uint16_t strength, bool program_driver) {
    const auto net_u        = static_cast<uint32_t>(net);
    const auto existing     = resolved_scope_.find(net_u);
    const bool first_driver = existing == resolved_scope_.end();
    // Every driver of a resolved net passes through here, so this is the one
    // place that can see "some driver of this net is program-side" without
    // each caller remembering to say so (LRM 24.3.2; emit_wire_resolvers
    // turns it into a reactive-home resolver).
    if (program_driver || in_program_) {
        program_driven_nets_.insert(net_u);
    }
    // The drivers of this net can live in different instance scopes (e.g. a
    // shared interface net whose header port is bound from the parent AND
    // separately driven by a bind-inserted module reaching it through an
    // upward hierarchical reference). Every driver SLOT is a design-wide
    // ir::Signal regardless of which scope discovers it (register_signal
    // only records it under scope_stack_ for VCD grouping of that ONE slot;
    // the slot's storage and every load/store of it address it by its flat
    // signal id, unaffected by scope) -- so the fold in emit_wire_resolvers
    // needs nothing new to reach a cross-scope slot. What genuinely has no
    // single right answer is which ONE scope the RESOLVER process (the
    // thing that reads every slot and writes the net) should be attributed
    // to for VCD grouping, once it folds slots contributed by more than one
    // scope: no_scope is the existing, already-supported answer for exactly
    // this shape (a process belonging to no single instance, like a per-top
    // PreInit pass) rather than an arbitrary pick of one driver's scope.
    const bool cross_scope =
      !first_driver && existing->second != ir::Process::no_scope && existing->second != cur_scope_id();
    auto&          slots = resolved_slots_[net_u];
    ir::SignalInfo info;
    info.name             = std::string(module_.signal(net).name) + "$drv" + std::to_string(slots.size());
    info.type             = module_.signal(net).type;
    info.dir              = ir::SignalInfo::Direction::Internal;
    const ir::Signal slot = register_signal(std::move(info));
    slots.push_back(slot);
    resolved_strengths_[net_u].push_back(strength); // parallel to slots
    if (cross_scope) {
        resolved_scope_[net_u] = ir::Process::no_scope;
    } else if (first_driver) {
        resolved_scope_[net_u] = cur_scope_id();
    } // else: same scope as before, or already no_scope -- leave it.
    resolved_loc_[net_u] = l;
    return slot;
}

// A definite (2-state, z-free) all-0 or all-1 constant of `width` bits, in the
// 4-state plane encoding. Used to inject a tri0/tri1 pull as a synthetic
// Pull-strength driver into the strength fold.
ir::Ref Lowerer::def_constant_ref(uint32_t width, bool one) {
    slang::SVInt v(width, 0, /*isSigned=*/false);
    if (one) {
        v.setAllOnes();
    }
    return svint_to_ref(v, make_int_type(width, /*is_four=*/true));
}

// An all-Z constant of `width` bits. Used as the resolution identity in the
// undriven bits of a partial driver's slot value.
ir::Ref Lowerer::z_constant_ref(uint32_t width) {
    return svint_to_ref(slang::SVInt::createFillZ(width, /*isSigned=*/false), make_int_type(width, /*is_four=*/true));
}

// The full-width value one driver stages into its slot: for a whole-net
// driver the driven value itself; for a constant packed partial driver
// (`w[7:4] = ...`), the value placed at its bit range with Z — the
// resolution identity — in every other bit, so the resolver folds each bit
// only across the drivers that actually reach it. `driven` is the already-
// lowered right-hand value. Shapes V2 cannot slot (dynamic index, unpacked
// element) fail loudly.
ir::Index Lowerer::build_driver_slot_value(ir::Signal        net,
                                           const Expression& left,
                                           ir::Index         driven,
                                           ir::Builder&      b,
                                           ir::SourceLoc     l) {
    const ValueSymbol* root = lvalue_root(left);
    if (root != nullptr && !root->getType().getCanonicalType().isIntegral()) {
        // A packed vector or packed struct is integral and folds directly;
        // an unpacked array / unpacked struct net has no single packed plane.
        // TODO: per-element slots + a per-element resolver for unpacked nets.
        fail("unpacked-array or unpacked-struct multiply-driven net is not supported");
    }
    const ir::Ref  net_ty = module_.signal(net).type;
    const uint32_t net_w  = module_.pool.layout(net_ty).width;
    if (is_whole_net_lvalue(left)) {
        return b.convert(driven, net_ty, root != nullptr && root->getType().isSigned(), l);
    }
    const NetRegion reg = root != nullptr ? net_region_of(left, *root) : kWholeNet;
    // A constant packed sub-range: one element band [0,0], a bounded bit
    // range strictly inside the net. Anything else (dynamic index, unpacked-
    // array element, whole-net via select) is not a slot-able partial.
    const bool packed_const = reg.elem_lo == 0 && reg.elem_hi == 0 && reg.bit_hi != UINT32_MAX &&
                              reg.bit_lo <= reg.bit_hi && reg.bit_hi < net_w;
    if (!packed_const) {
        // TODO: dynamic-index / unpacked-array-element partial drivers (a
        // runtime-varying region needs a per-activation Z refill or a
        // per-element resolver).
        fail("dynamic or unpacked-array partial driver of a multiply-driven net is not supported");
    }
    if (module_.pool.layout(net_ty).domain != ir::Domain::Four) {
        // Not a gap: LRM 6.7.1 restricts a built-in net type's data type to a
        // 4-state integral type (or an unpacked aggregate of such), so every
        // net's storage can hold the Z background this builds — slang rejects
        // `wire bit [7:0] w;` outright (InvalidNetType), and a user-defined
        // nettype, the only way to declare a 2-state net (6.6.7 b), is refused
        // in collect_scope_signals. A 2-state MEMBER of a 4-state packed net
        // (`struct packed { bit [3:0] lo; logic [3:0] hi; }`) is a partial
        // driver of a 4-state net and takes the path below.
        fail("internal: resolved net storage is not 4-state");
    }
    const uint32_t  region_w = reg.bit_hi - reg.bit_lo + 1;
    const ir::Index rhs      = b.convert(driven, make_int_type(region_w, true), false, l);
    // Concatenation is MSB-first: { Z above the range, the value, Z below }.
    std::vector<ir::Index> parts;
    if (reg.bit_hi < net_w - 1) {
        parts.push_back(b.constant(z_constant_ref(net_w - 1 - reg.bit_hi), l));
    }
    parts.push_back(rhs);
    if (reg.bit_lo > 0) {
        parts.push_back(b.constant(z_constant_ref(reg.bit_lo), l));
    }
    return parts.size() == 1 ? parts.front() : b.concat(parts, l);
}

// Reject a concatenation (or conversion-wrapped) lvalue any of whose operands
// drives a resolved net. Such an operand cannot route to a per-driver slot in
// V1 (a concat driver spans several nets at once), so storing it directly
// would race the resolver — fail loudly instead.
void Lowerer::reject_concat_resolved_driver(const Expression& lhs) {
    switch (lhs.kind) {
        case ExpressionKind::Concatenation:
            for (const Expression* op : lhs.as<ConcatenationExpression>().operands()) {
                reject_concat_resolved_driver(*op);
            }
            return;
        case ExpressionKind::Conversion:
            reject_concat_resolved_driver(lhs.as<ConversionExpression>().operand());
            return;
        default:
            if (resolved_net_of(lhs)) {
                // TODO: split a concatenation driver into per-net slot stores.
                fail("concatenation driver of a multiply-driven net is not supported");
            }
    }
}

// Stage one driver's full-width value into its slot, honouring a per-driver
// net delay. LRM 10.3.3 (`assign #d`) and LRM 28.16 (gate `#d`) put the delay
// on the DRIVER, ahead of resolution: the driver's own contribution
// transitions inertially and emit_wire_resolvers re-folds when any slot
// changes. Delaying the resolved net instead would smear every driver's
// timing together, so the delay always rides this slot store.
void Lowerer::store_driver_slot(const Expression& left,
                                ir::Signal        slot,
                                ir::Index         val,
                                const Expression* delay_expr,
                                ir::Builder&      b,
                                ir::SourceLoc     l) {
    const ir::Index lv = b.signal(slot, l);
    if (delay_expr == nullptr) {
        b.binary(ir::Inst::Tag::Store, lv, val, l);
        return;
    }
    if (!is_whole_net_lvalue(left)) {
        // A partial driver's slot carries a Z background in the bits it does
        // not drive, but a slot powers up all-x and a delayed driver does not
        // write it until `now + d` — so the undriven bits would fold as x for
        // the whole initial window instead of z. iverilog (`assign #5 v[1:0]`
        // beside `assign v[3:2]`) shows `v=11xx` there, which needs a Z-seeded
        // slot power-up value Clotho has no representation for.
        // TODO: delayed partial driver of a multiply-driven net — seed the
        // slot's power-up value with the Z background.
        fail("delay on a partial driver of a multiply-driven (resolved) net is not supported");
    }
    b.store_inertial_delay(lv, val, scaled_delay(*delay_expr, b), l);
}

// Emit one continuous-assign driver of a resolved net: stage its value (whole
// net, or a constant packed sub-range with a Z background) into this driver's
// slot, sensitive to the RHS reads. A non-null `delay_expr` (`assign #d`, LRM
// 10.3.3) makes that slot store inertial, so this driver alone is delayed.
void Lowerer::emit_resolved_driver(ir::Signal                  net,
                                   const AssignmentExpression& a,
                                   const Expression&           left,
                                   ir::Builder&                b,
                                   ir::SourceLoc               l,
                                   uint16_t                    strength,
                                   const Expression*           delay_expr) {
    const ir::Index  val  = build_driver_slot_value(net, left, lower_expr(a.right(), b), b, l);
    const ir::Signal slot = alloc_driver_slot(net, l, strength);
    store_driver_slot(left, slot, val, delay_expr, b, l);
    add_continuous_process(continuous_events(a), l, b);
}

// Store `val` into `lv` of type `ty`: one store for a packed value, and for a fixed
// unpacked array a loop per dimension over its declared range (ascending or
// descending as declared) that stores into every leaf element. `val` is the value
// of one leaf element. A loop, not a store per element, so the code does not grow
// with the array: a 64K-element array took seconds to lower unrolled.
void Lowerer::store_every_element(ir::Index lv, ir::Ref ty, ir::Index val, ir::Builder& b, ir::SourceLoc l) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(ty));
    if (t.kind != ir::Type::Kind::UnpackedArray) {
        b.binary(ir::Inst::Tag::Store, lv, val, l);
        return;
    }
    const ir::Ref  i32       = make_int_type(32, false);
    const bool     ascending = t.left <= t.right;
    const TempCell iter      = make_temp_cell(i32, "net_fill");
    b.binary(ir::Inst::Tag::Store, iter.lv(b, l), iconst(i32, t.left, b, l), l);

    b.open_region();
    const ir::Index cur = b.unary(ir::Inst::Tag::Load, iter.lv(b, l), l);
    const ir::Index pred =
      b.binary(ascending ? ir::Inst::Tag::SLe : ir::Inst::Tag::SGe, cur, iconst(i32, t.right, b, l), l);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    const ir::Index el = b.elem_select(lv, b.unary(ir::Inst::Tag::Load, iter.lv(b, l), l), ty, l, /*for_write=*/true);
    store_every_element(el, t.element, val, b, l);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index step_cur = b.unary(ir::Inst::Tag::Load, iter.lv(b, l), l);
    const ir::Index next =
      b.binary(ascending ? ir::Inst::Tag::Add : ir::Inst::Tag::Sub, step_cur, iconst(i32, 1, b, l), l);
    b.binary(ir::Inst::Tag::Store, iter.lv(b, l), next, l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test=*/false, l);
}

// Fold every resolved net's driver slots through WireRes and store the
// resolved value into the net. One self-starting process per net, sensitive
// to all its slots; the net is written only here, so its readers wake on the
// resolved value. Runs once per top after Emit, when every driver has
// registered its slot.
void Lowerer::emit_wire_resolvers() {
    // Every net that needs a resolver: the ones with >= 2 (or, for tri0/tri1,
    // >= 1) drivers, plus driverless tri0/tri1 nets that still pull to 0/1.
    std::vector<uint32_t> nets;
    nets.reserve(resolved_slots_.size() + net_pull_.size() + net_force_sites_.size());
    for (const auto& [net_u, slots] : resolved_slots_) {
        nets.push_back(net_u);
    }
    for (const auto& [net_u, pull] : net_pull_) {
        if (!resolved_slots_.contains(net_u)) {
            nets.push_back(net_u);
        }
    }
    // A forced net needs a resolver to inject its override even when it is
    // otherwise single-driver or undriven (an undriven forced net reverts to z
    // on release).
    for (const auto& [net_u, sites] : net_force_sites_) {
        if (!resolved_slots_.contains(net_u) && !net_pull_.contains(net_u)) {
            nets.push_back(net_u);
        }
    }
    std::sort(nets.begin(), nets.end()); // deterministic process order

    for (const uint32_t net_u : nets) {
        const ir::Signal                  net = static_cast<ir::Signal>(net_u);
        const auto                        si  = resolved_slots_.find(net_u);
        const std::span<const ir::Signal> slots =
          si != resolved_slots_.end() ? std::span<const ir::Signal>(si->second) : std::span<const ir::Signal>();
        const auto pi = net_pull_.find(net_u);

        const ir::SourceLoc l     = !slots.empty()          ? resolved_loc_[net_u]
                                    : pi != net_pull_.end() ? pi->second.loc
                                                            : ir::SourceLoc{ 0 };
        const uint32_t      scope = !slots.empty()          ? resolved_scope_[net_u]
                                    : pi != net_pull_.end() ? pi->second.scope
                                                            : ir::Process::no_scope;
        const ir::Inst::Tag op = net_resolve_op_.contains(net_u) ? net_resolve_op_[net_u] : ir::Inst::Tag::WireRes;

        // A net resolves through the strength-weighted fold when any driver
        // has a non-default (not Strong/Strong) drive strength; otherwise the
        // cheaper strengthless pairwise fold (WireRes / WandRes / WorRes).
        const auto                      st_it     = resolved_strengths_.find(net_u);
        const std::span<const uint16_t> strengths = st_it != resolved_strengths_.end()
                                                      ? std::span<const uint16_t>(st_it->second)
                                                      : std::span<const uint16_t>();
        const bool                      has_strength =
          std::any_of(strengths.begin(), strengths.end(), [](uint16_t s) { return s != 0x0303; });
        // A Supply pull (supply0/supply1) must beat a Strong driver, so it always
        // folds as a strength driver; a Pull pull (tri0/tri1) only when a real
        // driver carries an explicit strength (else the strengthless NetPull is
        // equivalent and cheaper).
        const bool supply_pull  = pi != net_pull_.end() && pi->second.strength >= 4;
        const bool use_strength = has_strength || supply_pull;

        const uint32_t net_w = module_.pool.layout(module_.signal(net).type).width;

        ir::Builder b(module_);
        // Fold the driver slots (a driverless tri0/tri1 net seeds all-Z, so
        // the pull sees only Z and fills the whole net with its pull value).
        ir::Index acc;
        bool      pull_injected = false;
        if (slots.empty()) {
            acc = b.constant(z_constant_ref(net_w), l);
        } else if (use_strength) {
            if (op != ir::Inst::Tag::WireRes) {
                // Wired-AND / wired-OR nets combine drivers by value, not by
                // strength; mixing the two resolution models is not defined here.
                // TODO: drive strengths on wand/wor nets (LRM 6.6.1-6.6.3).
                fail("drive strengths on a wand/wor net are not supported");
            }
            std::vector<ir::Index> slot_vals;
            std::vector<uint16_t>  str_vec(strengths.begin(), strengths.end());
            slot_vals.reserve(slots.size() + 1);
            for (const ir::Signal s : slots) {
                slot_vals.push_back(b.unary(ir::Inst::Tag::Load, b.signal(s, l), l));
            }
            if (pi != net_pull_.end()) {
                // A tri0/tri1 (Pull) or supply0/supply1 (Supply) pull is a driver
                // of 0 / 1 at its strength, so it must compete in the strength
                // fold — a weaker driver loses to it — not just fill leftover Z as
                // net_pull does. Inject it as a synthetic driver and skip the
                // post-fold pull.
                const bool    one = pi->second.value != 0;
                const uint8_t str = pi->second.strength;
                slot_vals.push_back(b.constant(def_constant_ref(net_w, one), l));
                str_vec.push_back(one ? static_cast<uint16_t>(str)        // (highz0, strN)
                                      : static_cast<uint16_t>(str << 8)); // (strN, highz1)
                pull_injected = true;
            }
            acc = b.strength_res(slot_vals, str_vec, l);
        } else {
            acc = b.unary(ir::Inst::Tag::Load, b.signal(slots[0], l), l);
            for (size_t k = 1; k < slots.size(); ++k) {
                const ir::Index next = b.unary(ir::Inst::Tag::Load, b.signal(slots[k], l), l);
                acc                  = b.binary(op, acc, next, l);
            }
        }
        if (pi != net_pull_.end() && !pull_injected) {
            acc = b.net_pull(acc, static_cast<uint32_t>(pi->second.value), l);
        }
        // Store the resolved driver fold under every active force site (LRM 10.6.2).
        // Net force writes at each site's range [lo, hi], so unforced bits keep the
        // fold and z is preserved (a positioned store, not a bitwise mask). A released
        // site (active === 0) is skipped, so re-running the resolver reverts those
        // bits to the fold. Each site's active/val join the sensitivity so force apply
        // and release re-run this process.
        //
        // A forced net takes its value in ONE store. A store wakes the net's waiters
        // from the transition it makes, so storing the fold and then the forced bits
        // over it showed them the fold for an instant: an edge and a change the force
        // forbids, since under a force the drivers change nothing a reader can see.
        // The overlay is built in a frame cell of the resolver function instead.
        const auto                          fs_it = net_force_sites_.find(net_u);
        const std::span<const NetForceSite> sites = fs_it != net_force_sites_.end()
                                                      ? std::span<const NetForceSite>(fs_it->second)
                                                      : std::span<const NetForceSite>();
        const ir::Ref                       net_ty = module_.signal(net).type;
        // The net's value as the overlay builds it, a fresh lvalue per use.
        const auto cell = [&] { return b.local(0, net_ty, l); };
        // A net delay (LRM 28.16, `wire #5 w;`) delays the resolved fold itself,
        // not any one driver's slot -- "the time it takes from any driver on the
        // net changing value to the time when the net value is updated". A force
        // is not a driver but an override of all of them (LRM 10.6.2), and it is
        // an active update event of its own (LRM 4.9.2), so it takes effect at
        // once and the net delay does not touch it. On a net that is also forced
        // the delayed fold therefore goes into a signal of its own, and the net is
        // that signal with the active sites laid over it (NetDelayShadow): a driver
        // change that is on its way lands under an active force, and a release
        // shows what the drivers have delivered. Stored into the net itself, the
        // delayed fold landed over the force, and the inertial store compared its
        // fold with the forced value, so a release changed nothing.
        const auto nd_it   = net_delay_.find(net_u);
        const auto sh_it   = net_delay_shadow_.find(net_u);
        const bool delayed = nd_it != net_delay_.end();
        const bool forced  = !sites.empty();
        assert((delayed && forced) == (sh_it != net_delay_shadow_.end()) && "a delayed forced net has its shadow");
        ir::Index value = acc; // what the force sites are laid over
        if (delayed && forced) {
            const NetDelayShadow& sh    = sh_it->second;
            const ir::Index       dst   = b.signal(sh.delayed, l);
            const ir::Index       ticks = scaled_delay(*nd_it->second, b);
            b.binary(ir::Inst::Tag::Store, b.signal(sh.fold, l), acc, l);
            b.store_inertial_delay(dst, acc, ticks, l);
            value = b.unary(ir::Inst::Tag::Load, b.signal(sh.delayed, l), l);
        } else if (delayed) {
            const ir::Index dst   = b.signal(net, l);
            const ir::Index ticks = scaled_delay(*nd_it->second, b);
            b.store_inertial_delay(dst, acc, ticks, l);
        }
        if (forced) {
            b.binary(ir::Inst::Tag::Store, cell(), value, l);
            for (const NetForceSite& site : sites) {
                const ir::Index part =
                  (site.lo == 0 && site.hi == net_w - 1) ? cell() : b.part_select(cell(), site.hi, site.lo, l);
                b.open_region();
                b.binary(ir::Inst::Tag::Store, part, b.unary(ir::Inst::Tag::Load, b.signal(site.val, l), l), l);
                const ir::ExtraSlice then_r = b.close_region();
                b.open_region();
                const ir::ExtraSlice else_r = b.close_region();
                b.cond(site_is(site.active, 1, b, l), then_r, else_r, l);
            }
            const ir::Index result = b.unary(ir::Inst::Tag::Load, cell(), l);
            b.binary(ir::Inst::Tag::Store, b.signal(net, l), result, l);
        } else if (!delayed) {
            // An unpacked array net reaches here only when nothing drives it (a
            // driver's slot is refused for one), and the pull, or the z, of a
            // driverless net lands in every element alike.
            store_every_element(b.signal(net, l), net_ty, acc, b, l);
        }

        std::vector<ir::WaitEvent> wait_events;
        wait_events.reserve(slots.size() + sites.size() * 2 + 1);
        for (const ir::Signal s : slots) {
            wait_events.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
        }
        for (const NetForceSite& site : sites) {
            wait_events.push_back(ir::WaitEvent{ .signal = site.active, .edge = ir::Edge::Any });
            wait_events.push_back(ir::WaitEvent{ .signal = site.val, .edge = ir::Edge::Any });
        }
        if (delayed && forced) {
            // The delayed fold landing changes the net, which is built from it.
            wait_events.push_back(ir::WaitEvent{ .signal = sh_it->second.delayed, .edge = ir::Edge::Any });
        }
        // LRM 24.3.2: "the driving and resolution of nets on the other side of
        // a program port connection also occurs in the reactive region set."
        // One program-side driver therefore makes the WHOLE resolver
        // reactive-home, and that is a real semantic choice, not a local one:
        // a net has exactly one resolution region, so the net's DESIGN-side
        // drivers now resolve in Reactive too. A design cascade that runs
        // through this net consequently sorts after the reactive code of the
        // same slot, and its design readers wake in the next Active iteration
        // -- which is precisely the ordering the clause's own example asks for
        // on the program-driven edge, now applied to every edge of the net.
        // The clause does not distinguish which driver changed; it puts the
        // resolution of such a net in the reactive region set, full stop.
        //
        // The alternative would be per-driver resolution regions -- two
        // resolvers folding one net, an Active one and a Reactive one -- which
        // no sentence of the LRM asks for, and which has no defined answer for
        // how the two partial folds combine into the net's single value.
        const bool reactive_home = program_driven_nets_.contains(net_u);
        ir::Ir     body;
        if (sites.empty()) {
            body = b.finish();
        } else {
            // A forceable net's fold lives in a plain function so that `release`
            // can run it too (release_net), from any procedural context a release
            // may appear in, a function included (LRM 13.4.4: a function may hold
            // any statement that does not block, and the fold never blocks). The
            // process just calls it.
            b.ret_void(l);
            const ir::FunctionId fid = net_resolver_fn(net_u);
            module_.set_function(fid,
                                 ir::Function{ .name        = module_.signal(net).name + "$resolve",
                                               .return_type = module_.pool.void_type,
                                               .param_types = {},
                                               .arg_dirs    = {},
                                               .local_types = std::vector<ir::Ref>{ net_ty },
                                               .is_task     = false,
                                               .ir          = b.finish() });
            ir::Builder pb(module_);
            pb.call(fid, {}, l);
            body = pb.finish();
        }
        module_.add_process(ir::Process{ .activation    = ir::Activation::Always,
                                         .wait_events   = std::move(wait_events),
                                         .self_start    = true,
                                         .reactive_home = reactive_home,
                                         .span          = ir::SourceSpan{ l, l },
                                         .ir            = std::move(body),
                                         .scope         = scope });
    }
}

ir::FunctionId Lowerer::net_resolver_fn(uint32_t net) {
    const auto [it, fresh] = net_resolver_fn_.try_emplace(net);
    if (fresh) {
        it->second = module_.reserve_function(module_.pool.void_type);
    }
    return it->second;
}

void Lowerer::lower_continuous_assign(const ContinuousAssignSymbol& sym) {
    const uint16_t    strength = pack_strength(sym.getDriveStrength());
    const Expression& e        = sym.getAssignment();
    if (e.kind != ExpressionKind::Assignment) {
        fail("malformed continuous assignment");
    }
    const AssignmentExpression& a    = e.as<AssignmentExpression>();
    const Expression&           left = a.left();

    // `assign #d lhs = rhs;` carries an inertial net delay (LRM 10.3.3). Only a
    // single `#d` DelayControl is lowered; a rise/fall/turn-off triple selects
    // a per-transition delay Clotho has no representation for.
    const TimingControl* delay = sym.getDelay();
    if (delay != nullptr && delay->kind != TimingControlKind::Delay) {
        // TODO: continuous-assign rise/fall/turn-off (vector-net transition)
        // delays, LRM 10.3.3 / 28.16.
        fail("continuous assignment rise/fall/turn-off delays (assign #(a,b,c)) are "
             "not supported");
    }

    if (cur_pass_ == Pass::Classify) {
        record_net_driver(left);
        if (strength_may_be_weak(strength)) {
            note_weak_driver(left);
        }
        return;
    }

    ir::Builder         b(module_);
    const ir::SourceLoc l = to_source_loc(sym.location);
    if (const std::optional<ir::Signal> net = resolved_net_of(left)) {
        // Multiply-driven net: this driver stages its value into its own
        // slot; the resolver folds every driver's slot (emit_wire_resolvers).
        // An `assign #d` here is a PER-DRIVER inertial delay (LRM 10.3.3) — it
        // delays this driver's contribution, and the resolver re-runs when the
        // slot lands — so it rides the slot store, never the resolved net.
        // Inside a program the slot store is reactive-home like every other
        // continuous construct (LRM 24.3.1), and alloc_driver_slot records the
        // net through `in_program_` so emit_wire_resolvers folds it from a
        // reactive-home resolver as well (LRM 24.3.2). What that costs the
        // net's design-side drivers is stated at that fold.
        emit_resolved_driver(
          *net, a, left, b, l, strength, delay != nullptr ? &delay->as<DelayControl>().expr : nullptr);
        return;
    }
    // A concatenation lvalue has no single root, so resolved_net_of misses
    // it; reject it loudly if an operand drives a resolved net rather than
    // silently storing past the resolver.
    reject_concat_resolved_driver(left);

    if (delay != nullptr) {
        // Only a whole integral scalar / packed net lvalue is lowered — the
        // deferred record is monomorphized per whole signal (like `x <= #d`),
        // so a select / concat / aggregate target has no record shape.
        // TODO: inertial delay on a bit/part-select, concat, or aggregate net.
        const bool whole_scalar =
          (left.kind == ExpressionKind::NamedValue || left.kind == ExpressionKind::HierarchicalValue) &&
          left.type->isIntegral() && !left.type->isUnpackedArray() && !is_aggregate(*left.type);
        if (!whole_scalar) {
            fail("continuous assignment delay is only supported for a whole integral "
                 "scalar target");
        }
        if (const ValueSymbol* root = lvalue_root(left); root != nullptr && force_info_of(*root) != nullptr) {
            // TODO: a delayed continuous assignment onto a force-target variable. The
            // delayed store commits from the timing wheel with no force gate, so it
            // lands over an active force (LRM 10.6.2: the force overrides a continuous
            // assignment until released), and what a release does with the delay is
            // not settled: 10.6.2 has it "reestablish that assignment and schedule a
            // reevaluation in the continuous assignment's scheduling region", 10.3.3
            // delays every update of the assignment, and iverilog 13 takes the driver's
            // value at once. A net with a delayed driver is unaffected: its driver
            // stores into a slot the resolver folds under the force.
            fail("continuous assignment delay on a force-target variable is not supported");
        }
        const ir::Index rhs   = lower_expr(a.right(), b);
        const ir::Index ticks = scaled_delay(delay->as<DelayControl>().expr, b);
        b.store_inertial_delay(lower_lvalue(left, b), rhs, ticks, l);
        add_continuous_process(continuous_events(e), l, b);
        return;
    }

    lower_assign(a, b);

    // A continuous assign is a process sensitive to the source elements of
    // its expression (LRM 4.9.1): every signal the RHS reads, plus any read
    // in an LHS selector (`assign mem[i] = x` is sensitive to i). A forced
    // variable's driver also wakes on force_site so release reverts to it.
    add_continuous_process(with_force_wake(continuous_events(e), left), l, b);
}

// The wait events of a continuous assignment: every signal its expression
// reads, as an any-change wait (LRM 4.9.1). Unlike inferred (@*)
// sensitivity, a written signal is NOT subtracted: a net that is both
// driven and read is real combinational feedback (`assign y = a & y`) and
// must stay sensitive. An empty list (constant RHS) yields a run-once
// process, which is the required time-zero evaluation.
//
// A continuous assign is sensitive only to the operands of its RHS
// expression (10.3.2), so a called function's argument expressions are
// sensitive but signals the function reads internally are not — the
// function-body contribution is an always_comb-only rule (9.2.2.2.2).
// ImplicitEvent mode enforces that. Verified vs iverilog: with
// `assign y = f(x)` where f reads module signal g, changing g alone does
// not re-evaluate y.
std::vector<ir::WaitEvent> Lowerer::continuous_events(const Expression& e) {
    std::vector<ir::WaitEvent> events;
    const auto                 declare_static = [this](const slang::ast::ClassPropertySymbol& p) {
        return declare_static_prop(p);
    };
    SensitivityCollector c(sigmap_, SensitivityMode::ImplicitEvent, declare_static);
    e.visit(c);
    for (const ir::Signal s : c.reads) {
        events.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
    }
    return events;
}

// Create the always-active, self-starting process every continuous
// construct (continuous assign, net declaration assignment, port-connection
// copy) lowers to: it evaluates once at time zero so it settles from
// declaration-initialized values, then re-evaluates whenever a wait event
// fires (LRM 4.9.1, 23.3.3).
//
// Inside a `program` the same construct belongs to the reactive region set
// instead (LRM 24.3.1: "The continuous assignment assign tclk = clk; would
// also be scheduled in the Reactive region"), which is what `in_program_`
// contributes here. The parameter is for the caller that knows the answer
// without that flag: a program's PORT copy is emitted by wire_ports while the
// PARENT instance's members lower, so `in_program_` there is the parent's
// answer -- false, since a program can only be instantiated in a design scope
// -- rather than the program's.
void Lowerer::add_continuous_process(std::vector<ir::WaitEvent> wait_events,
                                     ir::SourceLoc              l,
                                     ir::Builder&               b,
                                     bool                       reactive_home) {
    module_.add_process(ir::Process{ .activation    = ir::Activation::Always,
                                     .wait_events   = std::move(wait_events),
                                     .self_start    = true,
                                     .reactive_home = reactive_home || in_program_,
                                     .span          = ir::SourceSpan{ l, l },
                                     .ir            = b.finish(),
                                     .scope         = cur_scope_id() });
}

// The whole-scalar force/release target an lvalue names (variable or net), or
// null for a shape Phase 1 does not handle (a select, a concat, an aggregate, a
// string). Integral and real/shortreal scalars are handled — the force
// machinery is type-generic (the value store is a plain lower_expr + Store; the
// force_site state is always integral). A real net is illegal (slang rejects),
// so a real target is always a variable. Sets `*is_net` when non-null.
const ValueSymbol* Lowerer::force_target(const Expression& lhs, bool* is_net) {
    if (lhs.kind != ExpressionKind::NamedValue && lhs.kind != ExpressionKind::HierarchicalValue) {
        return nullptr;
    }
    const slang::ast::Type& t = lhs.type->getCanonicalType();
    if (!t.isIntegral() && !t.isFloating()) {
        return nullptr; // string / aggregate / unpacked / event / class
    }
    const ValueSymbol& sym = lhs.as<ValueExpressionBase>().symbol;
    if (is_net != nullptr) {
        *is_net = canonical_value_symbol(sym).kind == SymbolKind::Net;
    }
    return &sym;
}

// The whole-scalar VARIABLE an lvalue names (integral or real), or null for a
// net / any other shape. Used by the `assign` layer (variables only) and the gate.
const ValueSymbol* Lowerer::force_target_var(const Expression& lhs) {
    bool               is_net = false;
    const ValueSymbol* sym    = force_target(lhs, &is_net);
    return (sym != nullptr && !is_net) ? sym : nullptr;
}

// The ForceInfo for a signal already registered forceable, or null.
Lowerer::ForceInfo* Lowerer::force_info_of(const ValueSymbol& sym) {
    const auto sit = sigmap_.find(&canonical_value_symbol(sym));
    if (sit == sigmap_.end()) {
        return nullptr;
    }
    const auto fit = forceable_.find(static_cast<uint32_t>(sit->second));
    return fit == forceable_.end() ? nullptr : &fit->second;
}

// Collect-pass scan: register the hidden per-target state for every `force` /
// `assign` site in a procedural block, so writers/resolvers lowered later in
// Emit see the target as forceable.
void Lowerer::scan_force_targets(const Statement& body) {
    struct Scan : ASTVisitor<Scan, VisitFlags::AllGood> {
        Lowerer* self;
        void     handle(const ProceduralAssignStatement& s) {
            if (s.assignment.kind == ExpressionKind::Assignment) {
                self->register_pca_site(s);
            }
            visitDefault(s);
        }
    };
    Scan v;
    v.self = this;
    body.visit(v);
}

// A hidden 2-state i32 state signal (`force_site` / `assign_site` / net force
// `active`) that powers up to 0.
ir::Signal Lowerer::new_pca_state(const char* tag, const ValueSymbol& sym) {
    ir::SignalInfo info;
    info.name = prefix_ + tag + "." + std::string(sym.name) + "." + std::to_string(temp_id_++);
    info.type = make_int_type(32, false);
    return register_signal(std::move(info));
}

// Route one force/assign statement to the variable or net registration path.
void Lowerer::register_pca_site(const ProceduralAssignStatement& s) {
    const Expression&  lhs    = s.assignment.as<AssignmentExpression>().left();
    const ValueSymbol* root   = lvalue_root(lhs);
    const bool         is_net = root != nullptr && canonical_value_symbol(*root).kind == SymbolKind::Net;
    if (is_net) {
        if (s.isForce) {
            register_net_force_site(s, lhs, *root); // whole or const select
        }
        return; // `assign` on a net is illegal (slang rejects)
    }
    register_pca_target(lhs, s.isForce); // whole-scalar variable
}

// Register one net `force` site (whole net or a constant bit/part-select of it).
// Allocates the site's `active` + range-wide `val` signals and marks the net for
// resolution (emit_wire_resolvers overlays every active site). A dynamic /
// non-constant select, or a concat, is left unregistered and guarded at lowering.
void Lowerer::register_net_force_site(const ProceduralAssignStatement& s,
                                      const Expression&                lhs,
                                      const ValueSymbol&               root) {
    const auto sit = sigmap_.find(&canonical_value_symbol(root));
    if (sit == sigmap_.end()) {
        return; // hierarchical / not-yet-collected: guarded loud
    }
    if (!lhs.type->getCanonicalType().isIntegral()) {
        // A whole unpacked array or struct net: its layout width is one element's, so
        // a site over it would force element 0 only. Left unregistered, lower_proc_assign
        // refuses it.
        // TODO: force / release of an unpacked-aggregate net.
        return;
    }
    const auto     net_u = static_cast<uint32_t>(sit->second);
    const uint32_t net_w = module_.pool.layout(module_.signal(sit->second).type).width;

    uint32_t lo = 0, hi = net_w - 1;
    if (lhs.kind != ExpressionKind::NamedValue && lhs.kind != ExpressionKind::HierarchicalValue) {
        const auto r = packed_bit_range(lhs);
        if (!r) {
            return; // dynamic / unsupported select: guarded loud at lowering
        }
        lo = r->first;
        hi = r->second;
    }
    NetForceSite site;
    site.active = new_pca_state("$force_active", root);
    site.lo     = lo;
    site.hi     = hi;
    ir::SignalInfo vinfo;
    vinfo.name = prefix_ + "$force_val." + std::string(root.name) + "." + std::to_string(temp_id_++);
    vinfo.type = make_int_type(hi - lo + 1, /*four_state=*/true);
    site.val   = register_signal(std::move(vinfo));

    auto&          sites = net_force_sites_[net_u];
    const uint32_t idx   = static_cast<uint32_t>(sites.size());
    sites.push_back(site);
    stmt_force_site_[&s] = { net_u, idx };
    if (net_delay_.contains(net_u)) {
        const auto [shadow, fresh] = net_delay_shadow_.try_emplace(net_u);
        if (fresh) {
            const std::string net_name = module_.signal(sit->second).name;
            const ir::Ref     net_ty   = module_.signal(sit->second).type;
            const auto        hidden   = [&](const char* suffix) {
                ir::SignalInfo info;
                info.name = net_name + suffix;
                info.type = net_ty;
                info.dir  = ir::SignalInfo::Direction::Internal;
                return register_signal(std::move(info));
            };
            shadow->second.delayed = hidden("$delayed");
            shadow->second.fold    = hidden("$fold");
        }
    }
    if (resolved_nets_.insert(net_u).second) {
        net_resolve_op_[net_u] = net_resolve_tag(canonical_value_symbol(root).as<NetSymbol>().netType.netKind);
    }
    if (in_program_) {
        // A `force` from program code drives this net as surely as a
        // continuous assignment does -- the resolver overlays the site on the
        // fold (LRM 10.6.2) -- and the site's `active` / `val` are written by
        // the program's Reactive thread. Without this the resolver would
        // re-fold them in the next Active, a region later than LRM 24.3.2
        // allows, which is the same defect the driver slots would have.
        // A `force` is the only procedural continuous assignment that can
        // reach a net: `assign` on a net is illegal and slang rejects it
        // (register_pca_site), so no other PCA path needs this.
        program_driven_nets_.insert(net_u);
    }
}

// Register a whole-scalar-VARIABLE `force` (is_force) or `assign` (!is_force)
// target. force_site is allocated on first registration; assign_site is added
// when the variable is an `assign` target.
void Lowerer::register_pca_target(const Expression& lhs, bool is_force) {
    const ValueSymbol* sym = force_target_var(lhs);
    if (sym == nullptr) {
        return; // unsupported shape (net handled elsewhere): guarded loud
    }
    const auto sit = sigmap_.find(&canonical_value_symbol(*sym));
    if (sit == sigmap_.end()) {
        return; // not registered in this scope (hierarchical): guarded loud
    }
    const auto [it, inserted] = forceable_.try_emplace(static_cast<uint32_t>(sit->second));
    ForceInfo& fi             = it->second;
    if (inserted) {
        fi.site = new_pca_state("$force_site", *sym);
    }
    if (!is_force && !fi.has_assign) {
        fi.has_assign  = true;
        fi.assign_site = new_pca_state("$assign_site", *sym);
    }
}

// A continuous driver (module-level `assign`) of a forced variable must also
// wake on `force_site` so that `release` (force_site -> 0) re-evaluates it and
// reverts the variable to the driver's current value (LRM 10.6.2 reestablish).
// Appends that wait event when `lhs`'s root is forceable; a no-op otherwise.
std::vector<ir::WaitEvent> Lowerer::with_force_wake(std::vector<ir::WaitEvent> ev, const Expression& lhs) {
    if (const ValueSymbol* r = lvalue_root(lhs)) {
        if (ForceInfo* fi = force_info_of(*r)) {
            ev.push_back(ir::WaitEvent{ .signal = fi->site, .edge = ir::Edge::Any });
        }
    }
    return ev;
}

// Constant of the force_site signal's type (2-state i32).
ir::Index Lowerer::site_const(uint32_t id, ir::Builder& b, ir::SourceLoc l) {
    return b.constant(module_.pool.put_int(make_int_type(32, false), id), l);
}

// `force_site === id` (CaseEq never yields x; the site is 2-state anyway).
ir::Index Lowerer::site_is(ir::Signal site, uint32_t id, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index cur  = b.unary(ir::Inst::Tag::Load, b.signal(site, l), l);
    const ir::Index want = site_const(id, b, l);
    return b.binary(ir::Inst::Tag::CaseEq, cur, want, l);
}

// `not forced` and, if an assign layer exists, `not assigned` — the predicate
// that lets an ordinary write / the assign-layer driver through. force > assign
// > ordinary (LRM 10.6): ordinary is gated on both, the assign driver on force.
ir::Index Lowerer::not_forced(const ForceInfo& fi, bool also_assign, ir::Builder& b, ir::SourceLoc l) {
    ir::Index pred = site_is(fi.site, 0, b, l);
    if (also_assign && fi.has_assign) {
        pred = b.binary(ir::Inst::Tag::Land, pred, site_is(fi.assign_site, 0, b, l), l);
    }
    return pred;
}

// An ordinary blocking/nonblocking write to a forced/assigned variable, of any
// lvalue shape rooted at it: the whole variable, a bit/part/element-select of
// it, or a packed struct/union member. LRM 10.6.2 suppresses the *update*, not
// the statement, so the lvalue and the RHS are evaluated (their side effects
// run) and only the store is gated on "neither a force nor an assign active".
// A select's store is narrowing over live storage that already holds the forced
// value, so dropping it leaves exactly the forced value in place — the same
// shape-agnostic gate the whole-variable write uses.
ir::Index Lowerer::gated_store(const AssignmentExpression& a, const ForceInfo& fi, ir::Builder& b) {
    const ir::SourceLoc l   = loc(a);
    const ir::Inst::Tag tag = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
    ir::Index           lv  = ir::Index{};
    ir::Index           rhs = ir::Index{};
    if (a.isCompound()) {
        // `x op= e` (LRM 11.4.1, always blocking): bind the lvalue first, then
        // lower slang's `op(LValueReference, e)` through it — the read sees the
        // forced value (it is what storage holds), and only the write-back is
        // gated, so a forced target keeps its value. Evaluation order matches
        // lower_compound_assign so a side-effecting index behaves identically.
        lv  = lower_lvalue(a.left(), b, /*is_write=*/true);
        rhs = lower_compound_rhs(a, lv, b);
    } else {
        rhs = lower_expr(a.right(), b);
        lv  = lower_lvalue(a.left(), b, /*is_write=*/true);
    }
    store_or_gate(&fi, tag, lv, rhs, b, l);
    return rhs;
}

// The force / `assign` site state governing writes to an lvalue, keyed on its
// root: null when that root is not a procedural-continuous-assignment target
// anywhere in the design, i.e. when the write needs no gate at all.
Lowerer::ForceInfo* Lowerer::pca_info_of_lvalue(const Expression& lhs) {
    const ValueSymbol* root = lvalue_root(lhs);
    return root == nullptr ? nullptr : force_info_of(*root);
}

// The update half of an ordinary procedural write: a plain store when the
// target is no force/assign target (`fi` null), else a store dropped while an
// override owns the target. LRM 10.6.2 suppresses the update alone — whatever
// the statement already evaluated (RHS, lvalue selectors, the read of a
// read-modify-write) stands, and the override's value survives untouched.
void Lowerer::store_or_gate(const ForceInfo*     fi,
                            ir::Inst::Tag        tag,
                            ir::Index            lv,
                            ir::Index            v,
                            ir::Builder&         b,
                            ir::SourceLoc        l,
                            const IntraNbTiming* nbt) {
    if (fi == nullptr) {
        store_leaf(tag, nbt, lv, v, b, l);
        return;
    }
    if (nbt != nullptr) {
        // TODO: the same gap lower_assign's force-target guard names, reached
        // through a DECOMPOSING target instead of a whole one -- a concatenation
        // or streaming operand whose own root is a force target, which
        // lvalue_root of the statement's lvalue cannot see. LRM 10.6.2 decides
        // the suppression when the deferred update commits, not when it is
        // armed, and store_nb_delay / store_nb_event carry no predicate.
        fail("intra-assignment timing on a write to a force-target variable "
             "is not supported");
    }
    const ir::Index pred = not_forced(*fi, /*also_assign=*/true, b, l);
    b.open_region();
    b.binary(tag, lv, v, l);
    const ir::ExtraSlice then_r = b.close_region();
    b.open_region();
    const ir::ExtraSlice else_r = b.close_region();
    b.cond(pred, then_r, else_r, l);
}

// Common target resolution for force / assign / release / deassign: the whole
// integral variable, with the shape and port guards. Fails loud on any shape
// Phase 1 does not handle. `verb` names the statement in the diagnostic.
// An input/inout-port variable is driven from outside via a port-copy
// continuous process that is not force_site-aware, so release/deassign could
// not reestablish the external driver (LRM 10.6). Internal vars and output
// ports (driven inside, by a gated assign / procedural write) are handled.
// TODO: force-wake port-copy drivers to lift this.
bool Lowerer::is_externally_driven_port(const ValueSymbol& sym) {
    const ir::SignalInfo::Direction dir = module_.signals()[static_cast<uint32_t>(signal_of(sym))].dir;
    return dir == ir::SignalInfo::Direction::Input || dir == ir::SignalInfo::Direction::Inout;
}

const ValueSymbol* Lowerer::pca_target_var(const Expression& lhs, const char* verb) {
    const ValueSymbol* sym = force_target_var(lhs);
    if (sym == nullptr) {
        fail(std::string(verb) + " is only supported on a whole integral variable "
                                 "(LRM 10.6); nets, selects, and aggregates are not yet supported");
    }
    if (is_externally_driven_port(*sym)) {
        fail(std::string(verb) + " on an input or inout port variable is not supported");
    }
    return sym;
}

void Lowerer::lower_proc_assign(const ProceduralAssignStatement& s, ir::Builder& b) {
    if (s.assignment.kind != ExpressionKind::Assignment) {
        fail("malformed force/assign statement");
    }
    const AssignmentExpression& a = s.assignment.as<AssignmentExpression>();
    const ir::SourceLoc         l = loc(s);

    if (s.isForce) {
        bool               is_net = false;
        const ValueSymbol* sym    = force_target(a.left(), &is_net);
        if (sym == nullptr && !stmt_force_site_.contains(&s)) {
            fail("force is only supported on a whole variable/net or a constant "
                 "bit/part-select of a net (LRM 10.6.2); a select of a variable, "
                 "a concatenation, and an aggregate are not yet supported");
        }
        if (is_net || stmt_force_site_.contains(&s)) {
            // Net force (LRM 10.6.2), whole or a constant bit/part-select: set the
            // site's `active` flag and range-wide `val`; the net's resolver
            // overlays those bits while active, and reverts on release
            // (emit_wire_resolvers). Track the RHS into `val` while active.
            const auto it = stmt_force_site_.find(&s);
            if (it == stmt_force_site_.end()) {
                fail("force on a dynamic or non-constant net bit/part-select is not "
                     "supported (only a constant select, LRM 10.6.2)");
            }
            const auto [net_u, idx]   = it->second;
            const NetForceSite& site  = net_force_sites_[net_u][idx];
            const auto          net   = static_cast<ir::Signal>(net_u);
            const uint32_t      net_w = module_.pool.layout(module_.signal(net).type).width;
            const ir::Index     rhs   = lower_expr(a.right(), b);
            const ir::Index     flag  = b.signal(site.active, l);
            const ir::Index     on    = site_const(1, b, l);
            b.binary(ir::Inst::Tag::Store, flag, on, l);
            b.binary(ir::Inst::Tag::Store, b.signal(site.val, l), rhs, l);
            // Eager positioned apply so a same-instant read (`force n=v; if
            // (n===v)`) sees it — the resolver would otherwise update n one delta
            // later. The resolver re-affirms the same value when it runs.
            const ir::Index net_lv = (site.lo == 0 && site.hi == net_w - 1)
                                       ? b.signal(net, l)
                                       : b.part_select(b.signal(net, l), site.hi, site.lo, l);
            b.binary(ir::Inst::Tag::Store, net_lv, rhs, l);
            add_pca_reeval(a, site.val, l, {}, [&](ir::Builder& rb) { return site_is(site.active, 1, rb, l); });
            return;
        }
        ForceInfo* fip = force_info_of(*sym);
        if (fip == nullptr) {
            // The Collect scan registers every in-scope target; a cross-scope
            // (hierarchical) target works when its instance was elaborated before
            // the force was scanned, else it is not yet in sigmap here.
            fail("force on a target not elaborated before the force statement is "
                 "not supported (declare the instance before the forcing block)");
        }
        if (is_externally_driven_port(*sym)) {
            fail("force on an input or inout port variable is not supported");
        }
        // Variable force: highest priority. Apply now unconditionally, then track
        // the RHS while this site stays the active force (last-force-wins).
        ForceInfo&       fi      = *fip;
        const uint32_t   id      = ++fi.next_site;
        const ir::Signal v       = signal_of(*sym);
        const ir::Index  site_lv = b.signal(fi.site, l);
        const ir::Index  id_val  = site_const(id, b, l);
        b.binary(ir::Inst::Tag::Store, site_lv, id_val, l);
        const ir::Index dst = b.signal(v, l);
        const ir::Index rhs = lower_expr(a.right(), b);
        b.binary(ir::Inst::Tag::Store, dst, rhs, l);
        add_pca_reeval(a, v, l, {}, [&](ir::Builder& rb) { return site_is(fi.site, id, rb, l); });
        return;
    }

    // Procedural continuous `assign` (LRM 10.6.1) — variables only.
    const ValueSymbol* sym = pca_target_var(a.left(), "procedural continuous assign");
    ForceInfo*         fip = force_info_of(*sym);
    if (fip == nullptr) {
        fail("procedural continuous assign on a hierarchical or cross-scope target "
             "is not supported");
    }
    ForceInfo&       fi = *fip;
    const ir::Signal v  = signal_of(*sym);

    // Procedural continuous `assign` (LRM 10.6.1): overrides ordinary procedural
    // writes, is itself overridden by force. assign_site was allocated by the
    // scan. Apply now while not force-overridden, then track the RHS; the
    // re-eval also wakes on force_site so `release` reestablishes this assign.
    const uint32_t  id      = ++fi.next_assign;
    const ir::Index site_lv = b.signal(fi.assign_site, l);
    const ir::Index id_val  = site_const(id, b, l);
    b.binary(ir::Inst::Tag::Store, site_lv, id_val, l);
    {
        const ir::Index pred = not_forced(fi, /*also_assign=*/false, b, l);
        b.open_region();
        const ir::Index dst = b.signal(v, l);
        const ir::Index rhs = lower_expr(a.right(), b);
        b.binary(ir::Inst::Tag::Store, dst, rhs, l);
        const ir::ExtraSlice then_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice else_r = b.close_region();
        b.cond(pred, then_r, else_r, l);
    }
    add_pca_reeval(a, v, l, { ir::WaitEvent{ .signal = fi.site, .edge = ir::Edge::Any } }, [&](ir::Builder& rb) {
        const ir::Index assigned = site_is(fi.assign_site, id, rb, l);
        const ir::Index unforced = not_forced(fi, /*also_assign=*/false, rb, l);
        return rb.binary(ir::Inst::Tag::Land, assigned, unforced, l);
    });
}

void Lowerer::lower_proc_deassign(const ProceduralDeassignStatement& s, ir::Builder& b) {
    const Expression&   lv     = s.lvalue;
    const ValueSymbol*  root   = lvalue_root(lv);
    const bool          is_net = root != nullptr && canonical_value_symbol(*root).kind == SymbolKind::Net;
    const ir::SourceLoc l      = loc(s);

    if (is_net) {
        if (!s.isRelease) {
            fail("deassign of a net is not legal (deassign applies to a variable's "
                 "procedural continuous assignment, LRM 10.6.1)");
        }
        release_net(lv, *root, b, l);
        return;
    }

    // Variable release / deassign.
    const ValueSymbol* sym = force_target_var(lv);
    if (sym == nullptr) {
        fail(std::string(s.isRelease ? "release" : "deassign") +
             " is only supported on a whole variable/net or a constant bit/part-"
             "select of a net (LRM 10.6)");
    }
    ForceInfo* fi = force_info_of(*sym);
    if (fi == nullptr) {
        // release/deassign of a variable never forced/assigned is a legal no-op
        // (LRM 10.6): no site state to clear, and the value already holds.
        return;
    }
    if (s.isRelease) {
        // Clear the force. Storing force_site services its wait queue and wakes
        // everything gated on it: a variable's active assign / continuous driver
        // reestablishes, otherwise the value holds (LRM 10.6.2).
        const ir::Index site_lv = b.signal(fi->site, l);
        const ir::Index none    = site_const(0, b, l);
        b.binary(ir::Inst::Tag::Store, site_lv, none, l);
    } else if (fi->has_assign) {
        // deassign (LRM 10.6.1): end the procedural continuous assign; the value
        // holds. A never-assigned variable is a no-op (no assign_site).
        const ir::Index site_lv = b.signal(fi->assign_site, l);
        const ir::Index none    = site_const(0, b, l);
        b.binary(ir::Inst::Tag::Store, site_lv, none, l);
    }
}

// `release net` / `release net[hi:lo]` (LRM 10.6.2): clear every active force
// site of the net whose range is covered by the release range; the resolver
// reverts those bits to the driver value. Whole-net release covers all sites.
void Lowerer::release_net(const Expression& lv, const ValueSymbol& root, ir::Builder& b, ir::SourceLoc l) {
    const auto sit = sigmap_.find(&canonical_value_symbol(root));
    if (sit == sigmap_.end()) {
        return; // never registered (hierarchical): a release with no force is a no-op
    }
    const auto fs = net_force_sites_.find(static_cast<uint32_t>(sit->second));
    if (fs == net_force_sites_.end()) {
        return; // net never forced: no-op
    }
    const uint32_t net_w = module_.pool.layout(module_.signal(sit->second).type).width;
    uint32_t       rlo = 0, rhi = net_w - 1;
    if (lv.kind != ExpressionKind::NamedValue && lv.kind != ExpressionKind::HierarchicalValue) {
        const auto r = packed_bit_range(lv);
        if (!r) {
            fail("release of a dynamic or non-constant net bit/part-select is not "
                 "supported (only a constant select, LRM 10.6.2)");
        }
        rlo = r->first;
        rhi = r->second;
    }
    // A force site is one `force` statement's whole range, active as a unit. A
    // release of part of an ACTIVE site (LRM 10.6.2: the released bits take their
    // drivers' value, the rest stay forced) has no representation, so it stops the
    // run with a diagnostic instead of releasing all of it, or none. Whether the
    // site is active is a run-time fact: a release that only overlaps a site not
    // forced at that moment (ivtest force_release_wire_pv forces [2:1] after it has
    // released [3:2]) is exact.
    // TODO: per-bit release of a wider force site (needs per-segment active flags
    // fixed at Collect, from every release range of the net).
    for (const NetForceSite& site : fs->second) {
        if (site.lo <= rhi && rlo <= site.hi && !(site.lo >= rlo && site.hi <= rhi)) {
            const std::string banner = banner_at(lv.sourceRange.start());
            b.open_region();
            b.severity(static_cast<uint32_t>(ir::Severity::Fatal),
                       severity_banner(lv.sourceRange.start()),
                       "release of part of an active force is not supported (LRM 10.6.2)",
                       {},
                       l);
            b.finish(banner + "release", 1, l);
            const ir::ExtraSlice then_r = b.close_region();
            b.open_region();
            const ir::ExtraSlice else_r = b.close_region();
            b.cond(site_is(site.active, 1, b, l), then_r, else_r, l, /*unlikely=*/true);
        }
    }
    // A net with a net delay (LRM 28.16) answers a release with what the drivers have
    // delivered through it, which is the fold unless a driver change is still on its
    // way. Then the LRM does not say what the freed bits take. 10.6.2: "the net shall
    // immediately be assigned the value determined by the drivers of the net", and
    // the drivers have changed. 10.3.3: "any value change that is to be applied to
    // [the net] by some other statement shall be delayed ... before it takes effect",
    // and the change has not taken effect. Neither yields to the other in the text, so
    // the run stops here instead of picking one. The two answers differ exactly when
    // the site's bits of the delayed fold and of the fold do.
    // TODO: release of a net while its net delay holds a driver change (needs the
    // LRM question above settled; the freed bits then take `fold` or `delayed`).
    if (const auto sh = net_delay_shadow_.find(static_cast<uint32_t>(sit->second)); sh != net_delay_shadow_.end()) {
        for (const NetForceSite& site : fs->second) {
            if (site.lo < rlo || site.hi > rhi) {
                continue;
            }
            const auto bits = [&](ir::Signal sig) {
                const ir::Index part = (site.lo == 0 && site.hi == net_w - 1)
                                         ? b.signal(sig, l)
                                         : b.part_select(b.signal(sig, l), site.hi, site.lo, l);
                return b.unary(ir::Inst::Tag::Load, part, l);
            };
            const ir::Index   delayed = bits(sh->second.delayed);
            const ir::Index   fold    = bits(sh->second.fold);
            const ir::Index   same    = b.binary(ir::Inst::Tag::CaseEq, delayed, fold, l);
            const ir::Index   moving  = b.unary(ir::Inst::Tag::Lnot, same, l);
            const ir::Index   pending = b.binary(ir::Inst::Tag::Land, site_is(site.active, 1, b, l), moving, l);
            const std::string banner  = banner_at(lv.sourceRange.start());
            b.open_region();
            b.severity(static_cast<uint32_t>(ir::Severity::Fatal),
                       severity_banner(lv.sourceRange.start()),
                       "release of a net while its net delay holds a driver change is not supported "
                       "(LRM 10.6.2, 10.3.3)",
                       {},
                       l);
            b.finish(banner + "release", 1, l);
            const ir::ExtraSlice then_r = b.close_region();
            b.open_region();
            const ir::ExtraSlice else_r = b.close_region();
            b.cond(pending, then_r, else_r, l, /*unlikely=*/true);
        }
    }
    bool released = false;
    for (const NetForceSite& site : fs->second) {
        if (site.lo >= rlo && site.hi <= rhi) {
            const ir::Index flag = b.signal(site.active, l);
            const ir::Index off  = site_const(0, b, l);
            b.binary(ir::Inst::Tag::Store, flag, off, l);
            released = true;
        }
    }
    if (released) {
        // LRM 10.6.2: "when released, the net shall immediately be assigned the
        // value determined by the drivers of the net". Run the resolver's fold
        // now rather than leave it to the process the store above only wakes.
        b.call(net_resolver_fn(static_cast<uint32_t>(sit->second)), {}, l);
    }
}

// A net declaration assignment (`wire w = expr;`) is a continuous assignment
// (LRM 6.7.1 / 10.3.3), NOT a one-shot initialization like a variable's: slang
// models it as the net's initializer expression rather than a separate
// ContinuousAssignSymbol, so it is lowered here into the same kind of process a
// bare `assign` produces. The initializer is already converted to the net type
// by slang, so the store needs no extra resize.
void Lowerer::lower_net_initializer(const NetSymbol& ns) {
    const Expression* init = ns.getInitializer();
    if (init == nullptr) {
        return;
    }
    if (cur_pass_ == Pass::Classify) {
        record_net_driver_whole(ns);
        if (strength_may_be_weak(pack_strength(ns.getDriveStrength()))) {
            weak_driven_nets_.insert(static_cast<uint32_t>(signal_of(ns)));
        }
        return;
    }
    const bool resolved = resolved_nets_.contains(static_cast<uint32_t>(signal_of(ns)));
    if (is_aggregate(ns.getType())) {
        if (resolved) {
            // TODO: multi-driver resolution of an unpacked-aggregate net —
            // WireRes folds packed planes only.
            fail("multiple drivers on an aggregate net are not supported");
        }
        if (ns.getDelay() != nullptr) {
            // TODO: net declaration assignment delay on an unpacked
            // aggregate — emit_agg_store has no inertial-delay counterpart
            // (that needs a leaf-wise replay, like the intra-assignment
            // aggregate commit does for `<= #d`).
            fail("a delay on an aggregate net declaration assignment is not supported");
        }
        // A whole unpacked-struct / aggregate net initializer is a whole-object
        // continuous copy (one process on the source's sensitivity), the same
        // as a bare `assign n = ...` on a struct.
        ir::Builder         ab(module_);
        const ir::SourceLoc al = loc(*init);
        emit_agg_store(value_lvalue(ns, ab, al), lower_type(ns.getType()), *init, ir::Inst::Tag::Store, ab, al);
        add_continuous_process(continuous_events(*init), to_source_loc(ns.location), ab);
        return;
    }
    // `wire #d w = expr;` carries a per-driver continuous-assign delay (LRM
    // 10.3.3's last paragraph: embedded in the declaration, so it belongs to
    // THIS one driver and is not added to any other driver's delay), lowered
    // exactly like a bare `assign #d`. The LHS is always the whole net here —
    // a net_decl_assignment forbids any indexing (LRM 10.3.1) — so unlike
    // lower_continuous_assign there is no "whole scalar" shape to check.
    const TimingControl* delay = ns.getDelay();
    if (delay != nullptr && delay->kind != TimingControlKind::Delay) {
        // TODO: net declaration assignment rise/fall/turn-off delays
        // (`wire #(a,b,c) w = ...;`) — same gap as `assign #(a,b,c)`.
        fail("net declaration assignment rise/fall/turn-off delays (wire #(a,b,c) w = ...) "
             "are not supported");
    }
    ir::Builder         b(module_);
    const ir::SourceLoc l = loc(*init);
    if (resolved) {
        // A net-declaration assignment is a whole-net driver: stage the
        // initializer into this driver's slot (emit_wire_resolvers folds it).
        const ir::Signal slot = alloc_driver_slot(signal_of(ns), l, pack_strength(ns.getDriveStrength()));
        const ir::Index  val  = b.convert(lower_expr(*init, b), lower_type(ns.getType()), ns.getType().isSigned(), l);
        if (delay != nullptr) {
            const ir::Index dst   = b.signal(slot, l);
            const ir::Index ticks = scaled_delay(delay->as<DelayControl>().expr, b);
            b.store_inertial_delay(dst, val, ticks, l);
        } else {
            b.binary(ir::Inst::Tag::Store, b.signal(slot, l), val, l);
        }
        add_continuous_process(continuous_events(*init), to_source_loc(ns.location), b);
        return;
    }
    if (delay != nullptr) {
        const ir::Index dst   = value_lvalue(ns, b, l);
        const ir::Index rhs   = lower_expr(*init, b);
        const ir::Index ticks = scaled_delay(delay->as<DelayControl>().expr, b);
        b.store_inertial_delay(dst, rhs, ticks, l);
        add_continuous_process(continuous_events(*init), to_source_loc(ns.location), b);
        return;
    }
    const ir::Index dst = value_lvalue(ns, b, l);
    const ir::Index rhs = lower_expr(*init, b);
    b.binary(ir::Inst::Tag::Store, dst, rhs, l);
    add_continuous_process(continuous_events(*init), to_source_loc(ns.location), b);
}

// Map a built-in three-state gate name to its TriGate; nullopt otherwise.
std::optional<Lowerer::TriGate> Lowerer::tristate_gate_of(std::string_view name) {
    if (name == "bufif1") {
        return TriGate{ .active_high = true, .invert = false };
    }
    if (name == "bufif0") {
        return TriGate{ .active_high = false, .invert = false };
    }
    if (name == "notif1") {
        return TriGate{ .active_high = true, .invert = true };
    }
    if (name == "notif0") {
        return TriGate{ .active_high = false, .invert = true };
    }

    return std::nullopt;
}

// Map a built-in MOS switch name (LRM 28.7, Table 28-6) to the TriGate that
// describes it; nullopt otherwise. A MOS switch behaves exactly as the
// matching bufif gate except that a z data input passes through as z rather
// than reading as x: a pass device never manufactures an x from a floating
// input. No MOS form inverts. cmos/rcmos are absent here because LRM 28.9
// defines them as an nmos and a pmos sharing one output, which is two
// drivers, not one (lower_cmos_switch).
std::optional<Lowerer::TriGate> Lowerer::mos_switch_of(std::string_view name) {
    if (name == "nmos" || name == "rnmos") {
        return TriGate{ .active_high = true, .invert = false };
    }
    if (name == "pmos" || name == "rpmos") {
        return TriGate{ .active_high = false, .invert = false };
    }

    return std::nullopt;
}

// Map a built-in gate primitive name to its GateOp; nullopt for primitives
// outside the supported set.
std::optional<Lowerer::GateOp> Lowerer::gate_op_of(std::string_view name) {
    if (name == "and") {
        return GateOp::And;
    }
    if (name == "nand") {
        return GateOp::Nand;
    }
    if (name == "or") {
        return GateOp::Or;
    }
    if (name == "nor") {
        return GateOp::Nor;
    }
    if (name == "xor") {
        return GateOp::Xor;
    }
    if (name == "xnor") {
        return GateOp::Xnor;
    }
    if (name == "buf") {
        return GateOp::Buf;
    }
    if (name == "not") {
        return GateOp::Not;
    }

    return std::nullopt;
}

// Fold IR op for each gate type (XNOR folds with XOR, then inverts).
ir::Inst::Tag Lowerer::gate_fold_tag(GateOp op) {
    switch (op) {
        case GateOp::And:
        case GateOp::Nand:
            return ir::Inst::Tag::Band;
        case GateOp::Or:
        case GateOp::Nor:
        case GateOp::Buf:
        case GateOp::Not:
            return ir::Inst::Tag::Bor;
        case GateOp::Xor:
        case GateOp::Xnor:
            return ir::Inst::Tag::Bxor;
    }
    __builtin_unreachable();
}

bool Lowerer::gate_inverted(GateOp op) {
    return op == GateOp::Nand || op == GateOp::Nor || op == GateOp::Not || op == GateOp::Xnor;
}

// Strip conversion wrappers whose operand is still a packed integral value.
// Gate terminals are bound against 1-bit logic, so a `bit` net or a typedef'd
// scalar arrives wrapped in a no-op (for our purposes) conversion.
const Expression& Lowerer::unwrap_conversions(const Expression& e) {
    const Expression* cur = &e;
    while (cur->kind == ExpressionKind::Conversion) {
        const Expression& op = cur->as<ConversionExpression>().operand();
        if (!op.type->isIntegral()) {
            break;
        }
        cur = &op;
    }

    return *cur;
}

// Resolve a select base to its packed module signal, if it is a (possibly
// conversion-wrapped) plain signal reference. Only a width-preserving wrapper
// keeps bit positions aligned with the underlying signal's storage; gate
// instance arrays slice through such a converted view of the port expression.
std::optional<ir::Signal> Lowerer::gate_select_base(const Expression& value) {
    const Expression& val = unwrap_conversions(value);
    if (val.kind != ExpressionKind::NamedValue ||
        val.type->getCanonicalType().getBitWidth() != value.type->getCanonicalType().getBitWidth()) {
        return std::nullopt;
    }
    const ValueSymbol& sym = val.as<NamedValueExpression>().symbol;
    if (sym.kind == SymbolKind::Parameter || !sigmap_.contains(&sym)) {
        return std::nullopt;
    }

    return signal_of(sym);
}

// Normalize a declared index into `value`'s packed range to an LSB-relative
// bit position (translateIndex maps both descending and ascending ranges to
// distance from the LSB end), so non-zero-based and ascending ranges resolve
// correctly.
std::optional<uint32_t> Lowerer::gate_select_bit(const Expression& value, const Expression& index, Fold mode) {
    const std::optional<int32_t> idx = try_const_int(index, mode);
    if (!idx) {
        return std::nullopt;
    }
    const slang::ConstantRange r = value.type->getCanonicalType().getFixedRange();
    if (!r.containsPoint(*idx)) {
        return std::nullopt;
    }

    return static_cast<uint32_t>(r.translateIndex(*idx));
}

// Resolve a terminal expression to a static (signal, bit) location if it is a
// plain scalar reference, a constant bit-select, or a constant 1-bit
// part-select of a packed signal (gate instance arrays produce the latter).
std::optional<Lowerer::GateTerm> Lowerer::gate_static_term(const Expression& e, Fold mode) {
    const Expression& base = unwrap_conversions(e);
    if (base.kind == ExpressionKind::NamedValue) {
        const ValueSymbol& sym = base.as<NamedValueExpression>().symbol;
        if (sym.kind == SymbolKind::Parameter || !sigmap_.contains(&sym)) {
            return std::nullopt;
        }
        if (base.type->getCanonicalType().getBitWidth() != 1) {
            return std::nullopt;
        }

        return GateTerm{ .sig = signal_of(sym), .bit = 0 };
    }
    if (base.kind == ExpressionKind::ElementSelect) {
        const ElementSelectExpression& es = base.as<ElementSelectExpression>();
        if (is_unpacked_array(es.value())) {
            return std::nullopt;
        }
        const auto sig = gate_select_base(es.value());
        const auto bit = sig ? gate_select_bit(es.value(), es.selector(), mode) : std::nullopt;
        if (!bit) {
            return std::nullopt;
        }

        return GateTerm{ .sig = *sig, .bit = *bit };
    }
    if (base.kind == ExpressionKind::RangeSelect) {
        const RangeSelectExpression& rs = base.as<RangeSelectExpression>();
        if (rs.getSelectionKind() != RangeSelectionKind::Simple ||
            base.type->getCanonicalType().getBitWidth() != 1 || is_unpacked_array(rs.value())) {
            return std::nullopt;
        }
        const auto sig = gate_select_base(rs.value());
        const auto bit = sig ? gate_select_bit(rs.value(), rs.right(), Fold::Required) : std::nullopt;
        if (!bit) {
            return std::nullopt;
        }

        return GateTerm{ .sig = *sig, .bit = *bit };
    }

    return std::nullopt;
}

// Lower one gate input terminal to a GateInput: a compile-time constant plane
// code, a static 1-bit location, or (the general case, LRM 4.9.6) a synthesized
// temp net continuously assigned from the expression.
Lowerer::GateInput Lowerer::lower_gate_input(const Expression& e, std::string_view inst_name, uint32_t input_index) {
    // Constant fold first: literals, parameters, and constant expressions
    // (tie cells) melt into the gate at compile time. The terminal is bound
    // as 1-bit logic, so bit 0 of the folded value is the input. x and z need
    // distinct codes: every gate but a MOS switch reads z as x (LRM Table
    // 28-3), while a MOS pass switch propagates it as z (LRM Table 28-6), so
    // gate_input_value resolves the code per caller.
    {
        const slang::ConstantValue cv = fold_const(e, Fold::Runtime);
        if (cv.isInteger()) {
            const slang::logic_t bit = cv.integer()[0];
            // logic_t::operator== is a 4-state compare (x == z yields x), so
            // z has to be identified by its exact encoding.
            const uint8_t code = bit.value == slang::logic_t::Z_VALUE ? 0b01
                                 : bit.isUnknown()                    ? 0b10
                                 : bit.value != 0                     ? 0b11
                                                                      : 0b00;

            return GateInput{ .const_code = code, .is_const = true };
        }
    }

    if (const auto term = gate_static_term(e, Fold::Runtime)) {
        return GateInput{ .sig = term->sig, .bit = term->bit };
    }

    // Implicit continuous assign to an implicit net: temp = expr, sensitive
    // to the expression's reads, evaluated at time zero like any continuous
    // assign (mirrors lower_continuous_assign).
    ir::SignalInfo info;
    info.name            = prefix_ + "$gate" + std::to_string(temp_id_++) + "." + std::string(inst_name) + ".in" +
                           std::to_string(input_index);
    info.type            = module_.pool.logic_type;
    const ir::Signal tmp = module_.add_signal(std::move(info));

    ir::Builder         b(module_);
    const ir::SourceLoc l   = loc(e);
    const ir::Index     dst = b.signal(tmp, l);
    const ir::Index     rhs = b.convert(lower_expr(e, b), module_.pool.logic_type, false, l);
    b.binary(ir::Inst::Tag::Store, dst, rhs, l);

    std::vector<ir::WaitEvent> wait_events;
    const auto                 declare_static = [this](const slang::ast::ClassPropertySymbol& p) {
        return declare_static_prop(p);
    };
    SensitivityCollector c(sigmap_, SensitivityMode::ImplicitEvent, declare_static);
    e.visit(c);
    for (const ir::Signal s : c.reads) {
        wait_events.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
    }
    module_.add_process(ir::Process{ .activation  = ir::Activation::Always,
                                     .wait_events = std::move(wait_events),
                                     .self_start  = true,
                                     .span        = ir::SourceSpan{ l, l },
                                     .ir          = b.finish(),
                                     .scope       = cur_scope_id() });

    return GateInput{ .sig = tmp, .bit = 0 };
}

// Lower one gate output terminal. slang wraps each output connection in an
// AssignmentExpression whose left() is the driven lvalue; only a static 1-bit
// packed location is accepted (gate_static_term), which rules out every
// non-integral target (a real is 32/64 bits wide, a string 0).
//
// The target's domain does not matter here. A 2-state target is legal: LRM
// 10.3.2 lets one primitive output drive a variable, so `bit q; and (q, a, b);`
// is a lone driver of 2-state storage, and a 2-state member of a 4-state packed
// net (`struct packed { bit [3:0] lo; logic [3:0] hi; }`) is another. Either way
// gate_output_value has already applied LRM 6.11.2's 4-state -> 2-state
// conversion (x and z become 0), so the store carries nothing the target cannot
// hold.
Lowerer::GateTerm Lowerer::lower_gate_output(const Expression& conn) {
    if (conn.kind != ExpressionKind::Assignment) {
        fail("malformed gate output terminal");
    }
    const Expression& lhs  = conn.as<AssignmentExpression>().left();
    const auto        term = gate_static_term(lhs, Fold::Required);
    if (!term) {
        // TODO: concatenation / part-select / unpacked-element gate output
        // lvalues (rare in netlists).
        fail("unsupported gate output terminal");
    }

    return *term;
}

// A pullup (drives constant 1) / pulldown (drives constant 0) source (LRM
// 28.5): a lone permanent driver at pull strength (or an explicit strength),
// lowered as a strength driver slot on its output net so it folds against any
// real driver — a stronger driver wins, else the net holds the pull value.
void Lowerer::lower_pull_source(const PrimitiveInstanceSymbol& gi, bool up) {
    const auto conns = gi.getPortConnections();
    if (conns.empty() || conns[0] == nullptr) {
        fail("malformed pullup/pulldown terminal");
    }
    // slang wraps the output terminal in an AssignmentExpression (lhs = source);
    // the driven net is its left().
    const Expression& out =
      conns[0]->kind == ExpressionKind::Assignment ? conns[0]->as<AssignmentExpression>().left() : *conns[0];
    // A pull source has no single canonical net for a bit/part-select target.
    if (out.kind != ExpressionKind::NamedValue && out.kind != ExpressionKind::HierarchicalValue) {
        // TODO: pullup/pulldown on a bit/part-select or concat target.
        fail("pullup/pulldown on a non-whole-net target is not supported");
    }
    const ValueSymbol& sym = canonical_value_symbol(out.as<ValueExpressionBase>().symbol);
    if (sym.kind != SymbolKind::Net) {
        fail("pullup/pulldown target must be a net");
    }
    // Default pull strength (LRM 28.11) is Pull; an explicit strength overrides
    // the driven side. pullup drives only 1 (highz0), pulldown only 0 (highz1).
    const auto [ds0, ds1] = gi.getDriveStrength();
    const uint8_t driven = up ? (ds1 ? strength_level(ds1) : uint8_t{ 2 }) : (ds0 ? strength_level(ds0) : uint8_t{ 2 });
    emit_pull_driver(sym, up, driven, to_source_loc(gi.location), /*program_driver=*/false);
}

// A permanent driver of all ones (`up`) or all zeros on a whole net at drive
// strength `level`, lowered as a strength driver slot so it folds against any
// real driver: a stronger driver wins, else the net holds the pull value. The
// source a pullup / pulldown primitive (LRM 28.5) is, and an unconnected input
// port under `unconnected_drive (LRM 22.9) is.
void Lowerer::emit_pull_driver(const ValueSymbol& sym, bool up, uint8_t level, ir::SourceLoc l, bool program_driver) {
    if (cur_pass_ == Pass::Classify) {
        record_net_driver_whole(sym);
        force_net_resolved(signal_of(sym));
        if (level != 4) {
            weak_driven_nets_.insert(static_cast<uint32_t>(signal_of(sym)));
        }
        return;
    }
    if (cur_pass_ != Pass::Emit) {
        return;
    }
    const ir::Signal net = signal_of(sym);
    const uint32_t   w   = module_.pool.layout(module_.signal(net).type).width;
    // (highz0, level1) for a pull up, (level0, highz1) for a pull down.
    const uint16_t   strength = up ? static_cast<uint16_t>(level) : static_cast<uint16_t>(level << 8);
    ir::Builder      b(module_);
    const ir::Signal slot = alloc_driver_slot(net, l, strength, program_driver);
    const ir::Index  dst  = b.signal(slot, l);
    const ir::Index  val  = b.constant(def_constant_ref(w, up), l);
    b.binary(ir::Inst::Tag::Store, dst, val, l);
    // A permanent source: self-starting, never re-evaluated (empty sensitivity).
    add_continuous_process({}, l, b, program_driver);
}

// An unconnected input port of a module declared under `unconnected_drive pull0 /
// pull1 (LRM 22.9) is pulled down / up instead of floating: a Pull-strength
// driver of its net, so a driver of the child's own beats it. Only a whole
// packed net port is pulled. LRM 22.9 says nothing of what pulling a variable
// means (23.3.3.2 gives an unconnected input variable its data type's default),
// so a variable, an aggregate and a select port are refused; an event is not
// driven and has nothing to pull.
void Lowerer::lower_unconnected_drive(const PortSymbol& p, bool up, bool program_driver) {
    const ValueSymbol& sym = p.internalSymbol->as<ValueSymbol>();
    if (sym.getType().isEvent()) {
        return;
    }
    if (sym.kind != SymbolKind::Net || p.getInternalExpr() != nullptr || is_aggregate(sym.getType()) ||
        is_unpacked_array_type(sym.getType())) {
        // TODO: `unconnected_drive on an unconnected input variable, aggregate
        // or select port.
        fail("`unconnected_drive on an unconnected input port that is not a whole packed net is not "
             "supported");
    }
    emit_pull_driver(sym, up, /*level=*/2, to_source_loc(p.location), program_driver);
}

// A cmos / rcmos switch (LRM 28.9). The LRM defines it purely by
// decomposition: `cmos (w, data, ncontrol, pcontrol)` is exactly
// `nmos (w, data, ncontrol); pmos (w, data, pcontrol);` — two drivers sharing
// one output net, which the existing net resolver already folds. Both passes
// must agree on that count: Classify records the output twice so the net
// enters resolved_nets_, and Emit stages each half into its own driver slot.
// Were Classify to record only one, the net would never resolve, both halves
// would take the plain-store path and stomp each other last-write-wins.
void Lowerer::lower_cmos_switch(const PrimitiveInstanceSymbol& gi) {
    const auto conns = gi.getPortConnections();
    if (conns.size() != 4 || conns[0] == nullptr || conns[1] == nullptr || conns[2] == nullptr ||
        conns[3] == nullptr || conns[0]->kind != ExpressionKind::Assignment) {
        // slang has already diagnosed a malformed terminal list.
        fail("malformed cmos/rcmos terminal list");
    }
    const Expression& left = conns[0]->as<AssignmentExpression>().left();
    if (cur_pass_ == Pass::Classify) {
        // Twice, deliberately: the two halves overlap on the same region, which
        // is what marks the net resolved.
        record_net_driver(left);
        record_net_driver(left);
        note_mos_switch(gi);
        return;
    }
    if (cur_pass_ != Pass::Emit) {
        return;
    }
    check_mos_switch_strength(gi, left);
    const TimingControl* cmos_delay = gi.getDelay();
    if (cmos_delay != nullptr && cmos_delay->kind != TimingControlKind::Delay) {
        // TODO: cmos/rcmos rise/fall/turn-off delay triples #(a,b,c) (LRM 28.16)
        // — needs the per-transition delay selection Clotho has no
        // representation for. A single #d is lowered onto each half below.
        fail("gate rise/fall/turn-off delays (#(a,b,c)) are not supported");
    }
    const std::optional<ir::Signal> net = resolved_net_of(left);
    if (!net) {
        // Classify records two drivers for every cmos rooted at a net, so a
        // net output is resolved by construction. Anything else (a variable
        // output, an interface modport proxy record_net_driver skips) would
        // silently lose one half, so it fails instead.
        // TODO: cmos/rcmos driving something other than a plain net.
        fail("cmos/rcmos output must be a net");
    }
    const uint16_t      gate_strength = pack_strength(gi.getDriveStrength());
    const ir::SourceLoc l             = to_source_loc(gi.location);
    // Terminals are (output, data, ncontrol, pcontrol).
    const GateInput data  = lower_gate_input(*conns[1], gi.name, 0);
    const GateInput nctrl = lower_gate_input(*conns[2], gi.name, 1);
    const GateInput pctrl = lower_gate_input(*conns[3], gi.name, 2);
    // Each half is an independent driver slot. The n-channel half conducts on
    // ncontrol == 1, the p-channel half on pcontrol == 0; neither inverts, and
    // both pass a z data input through as z. A `cmos #d` delay lands on each
    // half's own slot store: LRM 28.9's decomposition gives the two halves the
    // device's delay, and both fold to the same output timing as one delayed
    // device (confirmed against iverilog 13: `cmos #5` and `nmos #5` + `pmos #5`
    // produce identical waveforms).
    const auto emit_half = [&](TriGate tg, const GateInput& ctrl) {
        ir::Builder                b(module_);
        std::vector<ir::WaitEvent> wait_events;
        const ir::Index            folded = gate_tristate_value(tg, data, ctrl, wait_events, b, l, /*pass_z=*/true);
        const ir::Index            val    = gate_output_value(left, folded, b, l);
        emit_gate_resolved_driver(left,
                                  *net,
                                  val,
                                  std::move(wait_events),
                                  b,
                                  l,
                                  gate_strength,
                                  cmos_delay != nullptr ? &cmos_delay->as<DelayControl>().expr : nullptr);
    };
    emit_half(TriGate{ .active_high = true, .invert = false }, nctrl);
    emit_half(TriGate{ .active_high = false, .invert = false }, pctrl);
}

// Record that `lhs` is driven at a weak strength: every net it names, through
// concatenations and conversions like record_net_driver.
void Lowerer::note_weak_driver(const Expression& lhs) {
    switch (lhs.kind) {
        case ExpressionKind::Concatenation:
            for (const Expression* op : lhs.as<ConcatenationExpression>().operands()) {
                note_weak_driver(*op);
            }
            return;
        case ExpressionKind::Conversion:
            note_weak_driver(lhs.as<ConversionExpression>().operand());
            return;
        default:
            break;
    }
    const ValueSymbol* root = lvalue_root(lhs);
    if (root != nullptr && canonical_value_symbol(*root).kind == SymbolKind::Net) {
        weak_driven_nets_.insert(static_cast<uint32_t>(signal_of(canonical_value_symbol(*root))));
    }
}

// What a MOS switch instance (nmos, pmos, cmos and their resistive forms) drives
// and reads, or nullopt for any other primitive or a terminal that is not a net.
std::optional<Lowerer::MosSwitch> Lowerer::mos_switch_info(const PrimitiveInstanceSymbol& gi) {
    const std::string_view name = gi.primitiveType.name;
    const bool             cmos = name == "cmos" || name == "rcmos";
    if (!cmos && !mos_switch_of(name)) {
        return std::nullopt;
    }
    const auto conns = gi.getPortConnections();
    if (conns.size() < 2 || conns[0] == nullptr || conns[1] == nullptr ||
        conns[0]->kind != ExpressionKind::Assignment) {
        return std::nullopt; // slang has diagnosed the terminal list
    }
    const ValueSymbol* root = lvalue_root(conns[0]->as<AssignmentExpression>().left());
    if (root == nullptr || canonical_value_symbol(*root).kind != SymbolKind::Net) {
        return std::nullopt;
    }
    MosSwitch sw{ .out         = signal_of(canonical_value_symbol(*root)),
                  .data        = std::nullopt,
                  .data_opaque = false,
                  .resistive   = name.starts_with('r'),
                  .drivers     = cmos ? 2u : 1u };
    // A data input that is a net bit passes that net's strength; a constant, a
    // variable or an expression is a strong driver. A net reached some other way
    // (an element of a net array) is lowered as an expression, so its strength is
    // not known here.
    if (const auto term = gate_static_term(*conns[1], Fold::Runtime)) {
        if (module_.signal(term->sig).is_net) {
            sw.data = term->sig;
        }
    } else if (const ValueSymbol* in = lvalue_root(*conns[1]);
               in != nullptr && canonical_value_symbol(*in).kind == SymbolKind::Net) {
        sw.data_opaque = true;
    }

    return sw;
}

void Lowerer::note_mos_switch(const PrimitiveInstanceSymbol& gi) {
    if (const std::optional<MosSwitch> sw = mos_switch_info(gi)) {
        mos_switches_.push_back(*sw);
        mos_strength_.reset();
    }
}

// The strength levels each recorded switch can drive, by propagation to a fixed
// point over the switches (a switch's output feeds the next one's data input).
//
// A net's levels, as seen by a switch reading it:
//  - any level, when some driver of it can be weak (weak_driven_nets_) or it has a
//    tri0 / tri1 pull: the strength then depends on which driver is active;
//  - otherwise strong, when it has a driver that is not a switch (every such
//    driver is strong or supply, which a switch treats alike), a supply pull or
//    no driver at all, plus what each switch driving it can output.
// A switch outputs the pass or the reduction of its input's levels; its input is
// strong when it is not a net.
const Lowerer::MosStrength& Lowerer::mos_strength() {
    if (mos_strength_) {
        return *mos_strength_;
    }
    MosStrength  r;
    const size_t n = mos_switches_.size();
    r.levels.assign(n, 0);
    ankerl::unordered_dense::map<uint32_t, std::vector<uint32_t>> readers; // data net -> switches
    for (uint32_t i = 0; i < n; ++i) {
        r.by_out[static_cast<uint32_t>(mos_switches_[i].out)].push_back(i);
        if (mos_switches_[i].data) {
            readers[static_cast<uint32_t>(*mos_switches_[i].data)].push_back(i);
        }
    }
    const auto net_levels = [&](uint32_t net) -> uint8_t {
        if (weak_driven_nets_.contains(net)) {
            return kAnyLevel;
        }
        const auto pull = net_pull_.find(net);
        if (pull != net_pull_.end() && pull->second.strength < 4) {
            return kAnyLevel;
        }
        uint8_t  levels      = 0;
        uint32_t switch_drvs = 0;
        if (const auto it = r.by_out.find(net); it != r.by_out.end()) {
            for (const uint32_t i : it->second) {
                levels |= r.levels[i];
                switch_drvs += mos_switches_[i].drivers;
            }
        }
        const auto   driven = net_driven_.find(net);
        const size_t total  = driven != net_driven_.end() ? driven->second.size() : 0;
        if (total > switch_drvs || total == 0 || pull != net_pull_.end()) {
            levels |= kStrong;
        }

        return levels;
    };

    // A worklist: a switch is revisited when a switch driving its data net grew.
    // Each switch's level set only gains bits (at most six), so the number of
    // visits is bounded by the switch count plus six per reader edge.
    std::vector<uint32_t> work;
    std::vector<bool>     queued(n, true);
    size_t                edges = 0;
    for (const auto& [net, rs] : readers) {
        edges += rs.size();
    }
    work.reserve(n);
    for (uint32_t i = n; i-- > 0;) {
        work.push_back(i);
    }
    size_t budget = n + 6 * edges + 1;
    while (!work.empty()) {
        assert(budget > 0 && "MOS strength propagation did not converge");
        --budget;
        const uint32_t i = work.back();
        work.pop_back();
        queued[i]           = false;
        const MosSwitch& sw = mos_switches_[i];
        const uint8_t in = sw.data_opaque ? kAnyLevel : sw.data ? net_levels(static_cast<uint32_t>(*sw.data)) : kStrong;
        const uint8_t out = sw.resistive ? mos_reduce_levels(in) : mos_pass_levels(in);
        if ((r.levels[i] | out) == r.levels[i]) {
            continue;
        }
        r.levels[i] |= out;
        if (const auto it = readers.find(static_cast<uint32_t>(sw.out)); it != readers.end()) {
            for (const uint32_t j : it->second) {
                if (!queued[j]) {
                    queued[j] = true;
                    work.push_back(j);
                }
            }
        }
    }
    mos_strength_ = std::move(r);

    return *mos_strength_;
}

// The Emit-time verdict for one MOS switch whose output is `out_left`. Clotho
// drives every switch strong (see the block comment in Lowerer.h), which is exact
// when nothing the switch's strength could be compared with is there:
//  - the output net is not resolved: the switch alone drives it and no pull does;
//  - the switch is the only driver of its net and the net has no weak pull;
//  - every switch on the net outputs exactly strong: it orders against any other
//    driver or pull as strong does;
//  - every driver is a switch outputting the same single level and no weak pull
//    competes: the whole net shifts by one step and orders as it did.
// Anything else is refused instead of resolved wrongly.
// TODO: carry a strength with a net's value (LRM 28.13, 28.14).
void Lowerer::check_mos_switch_strength(const PrimitiveInstanceSymbol& gi, const Expression& out_left) {
    const std::optional<MosSwitch>  sw  = mos_switch_info(gi);
    const std::optional<ir::Signal> net = resolved_net_of(out_left);
    if (!sw || !net) {
        return;
    }
    const MosStrength& st     = mos_strength();
    const auto         net_u  = static_cast<uint32_t>(*net);
    const auto         on_net = st.by_out.find(net_u);
    if (on_net == st.by_out.end()) {
        return;
    }
    uint32_t switch_drvs = 0;
    bool     all_strong  = true;
    bool     uniform     = true;
    for (const uint32_t i : on_net->second) {
        switch_drvs += mos_switches_[i].drivers;
        all_strong = all_strong && st.levels[i] == kStrong;
        uniform    = uniform && st.levels[i] == st.levels[on_net->second.front()] && std::has_single_bit(st.levels[i]);
    }
    const auto   driven    = net_driven_.find(net_u);
    const size_t total     = driven != net_driven_.end() ? driven->second.size() : 0;
    const auto   pull      = net_pull_.find(net_u);
    const bool   weak_pull = pull != net_pull_.end() && pull->second.strength < 4;
    const bool   others    = total > switch_drvs;
    if (all_strong || (!others && !weak_pull && (on_net->second.size() == 1 || uniform))) {
        return;
    }
    fail("MOS switch '" + std::string(gi.primitiveType.name) +
         "' shares its output net with another driver or a pull, and its output strength is not known to order "
         "like strong there (LRM 28.13: it passes its data input's strength; LRM 28.14: a resistive switch "
         "reduces it), which is not supported: Clotho has no strength on a net value");
}

void Lowerer::lower_gate(const PrimitiveInstanceSymbol& gi) {
    const PrimitiveSymbol& prim = gi.primitiveType;
    if (prim.name == "pullup" || prim.name == "pulldown") {
        lower_pull_source(gi, prim.name == "pullup");
        return;
    }
    // LRM 28.9 makes cmos/rcmos two drivers on one net, so it needs its own
    // two-pass path rather than the single-output loop below.
    if (prim.name == "cmos" || prim.name == "rcmos") {
        lower_cmos_switch(gi);
        return;
    }
    const auto op  = gate_op_of(prim.name);
    const auto tri = tristate_gate_of(prim.name);
    const auto mos = mos_switch_of(prim.name);
    const bool supported_kind =
      prim.primitiveKind == PrimitiveSymbol::NInput || prim.primitiveKind == PrimitiveSymbol::NOutput;
    if (!tri && !mos && (!op || !supported_kind)) {
        // TODO: bidirectional pass switches (tran, rtran, tranif0, tranif1,
        // rtranif0, rtranif1 — LRM 28.8) and user-defined primitives.
        // Conditional bidirectional conduction has no representation in the
        // one-pass driver-slot fold. Note that slang classifies only tran,
        // tranif0 and tranif1 as BiDiSwitch — rtran and rtranif0/rtranif1 are
        // Fixed — so this family must be recognized by name, not by kind.
        fail("gate primitive '" + std::string(prim.name) + "' is not supported");
    }
    const TimingControl* gate_delay = gi.getDelay();
    if (gate_delay != nullptr && gate_delay->kind != TimingControlKind::Delay) {
        // TODO: gate rise/fall/turn-off delay triples #(a,b,c) (LRM 28.16) —
        // needs the per-transition delay selection Clotho has no representation
        // for. A single #d delay is lowered below.
        fail("gate rise/fall/turn-off delays (#(a,b,c)) are not supported");
    }
    const uint16_t gate_strength = pack_strength(gi.getDriveStrength());

    const auto conns = gi.getPortConnections();
    if (conns.size() < 2) {
        // slang has already diagnosed a malformed terminal list.
        fail("malformed gate terminal list");
    }

    // NInput: terminal 0 is the output, the rest are inputs. NOutput (buf /
    // not): the last terminal is the input, the rest are outputs — one
    // independent process per output terminal. Fixed tri-state (bufif/notif)
    // and MOS switch (nmos/pmos/rnmos/rpmos): terminal 0 is the output,
    // terminals 1..2 are the data and control inputs.
    const bool n_output    = prim.primitiveKind == PrimitiveSymbol::NOutput;
    const auto input_span  = n_output ? conns.subspan(conns.size() - 1) : conns.subspan(1);
    const auto output_span = n_output ? conns.subspan(0, conns.size() - 1) : conns.subspan(0, 1);

    if (cur_pass_ == Pass::Classify) {
        // Record each output terminal as a continuous driver so a gate that
        // shares a net with another driver is detected (and rejected in Emit
        // until gate outputs join resolution).
        for (const Expression* out : output_span) {
            if (out->kind == ExpressionKind::Assignment) {
                record_net_driver(out->as<AssignmentExpression>().left());
                if (strength_may_be_weak(gate_strength)) {
                    note_weak_driver(out->as<AssignmentExpression>().left());
                }
            }
        }
        if (mos) {
            note_mos_switch(gi);
        }
        return;
    }
    if (mos) {
        check_mos_switch_strength(gi, output_span[0]->as<AssignmentExpression>().left());
    }
    std::vector<GateInput> inputs;
    inputs.reserve(input_span.size());
    for (size_t i = 0; i < input_span.size(); ++i) {
        inputs.push_back(lower_gate_input(*input_span[i], gi.name, static_cast<uint32_t>(i)));
    }

    const ir::SourceLoc loc = to_source_loc(gi.location);
    for (const Expression* out : output_span) {
        // A gate output is always an AssignmentExpression (lower_gate_output
        // enforces this); its left() is the driven lvalue. When that lvalue is
        // a multiply-driven net, stage into a per-driver slot so the net's
        // resolver folds it against the other drivers, else store directly.
        const Expression& left = out->as<AssignmentExpression>().left();

        // Fold the gate value into this output's own builder / process (a
        // tri-state gate drives Z, so it never forces z to x like the
        // combinational fold does).
        ir::Builder                b(module_);
        std::vector<ir::WaitEvent> wait_events;
        const ir::Index            folded =
          mos   ? gate_tristate_value(*mos, inputs[0], inputs[1], wait_events, b, loc, /*pass_z=*/true)
          : tri ? gate_tristate_value(*tri, inputs[0], inputs[1], wait_events, b, loc)
                : gate_folded_value(*op, inputs, wait_events, b, loc);
        const ir::Index val = gate_output_value(left, folded, b, loc);

        if (const std::optional<ir::Signal> net = resolved_net_of(left)) {
            // A gate delay on a driver of a resolved net is PER DRIVER (LRM
            // 28.16): this gate's own contribution transitions inertially and
            // the resolver re-runs when the slot lands, so it rides the slot
            // store rather than the resolved net.
            emit_gate_resolved_driver(left,
                                      *net,
                                      val,
                                      std::move(wait_events),
                                      b,
                                      loc,
                                      gate_strength,
                                      gate_delay != nullptr ? &gate_delay->as<DelayControl>().expr : nullptr);
        } else {
            const GateTerm output = lower_gate_output(*out);
            if (gate_delay != nullptr && module_.pool.layout(module_.signal(output.sig).type).width != 1) {
                // The inertial-delay store targets a whole signal; a gate driving
                // one bit of a wider vector (`and #d (v[i], ...)`) has no whole-
                // signal lvalue. TODO: bit-addressed gate output with a delay.
                fail("gate delay is only supported for a whole scalar output");
            }
            emit_gate_as_process(output,
                                 val,
                                 std::move(wait_events),
                                 b,
                                 loc,
                                 gate_delay != nullptr ? &gate_delay->as<DelayControl>().expr : nullptr);
        }
    }
}

// lvalue for bit `bit` of `sig`: direct for a 1-bit signal, BitSelect for a
// bit of a wider packed signal. Shared by gate input loads and output stores.
ir::Index Lowerer::gate_bit_lvalue(ir::Signal sig, uint32_t bit, ir::Builder& b, ir::SourceLoc l) {
    ir::Index s = b.signal(sig, l);
    if (module_.pool.layout(module_.signal(sig).type).width == 1) {
        return s;
    }
    const ir::Ref u32 = make_int_type(32, false);
    return b.binary(ir::Inst::Tag::BitSelect, s, b.constant(module_.pool.put_int(u32, bit), l), l);
}

// Adapt a gate's folded output to the domain of the terminal it drives. Every
// gate folds its output as 1-bit logic (LRM 28.4 Table 28-1 and friends are
// defined over 0/1/x/z), but the driven terminal may be declared 2-state: a
// variable, which LRM 10.3.2 allows exactly one primitive output to drive
// (`bit q; and (q, a, b);`), or a 2-state member of a 4-state packed net
// (`wire struct packed { bit [3:0] lo; logic [3:0] hi; } s;` driven at
// `s.lo[0]`). LRM 6.11.2 makes the automatic 4-state -> 2-state conversion turn
// x and z into 0; slang inserts that conversion on a continuous assign's
// right-hand side, but a gate's value is synthesized here, so apply it here —
// otherwise `and (s.lo[0], a, b)` would leave an x in the net where the
// equivalent `assign s.lo[0] = a & b;` leaves a 0.
ir::Index Lowerer::gate_output_value(const Expression& left, ir::Index val, ir::Builder& b, ir::SourceLoc l) {
    if (left.type->isFourState()) {
        return val;
    }
    return b.convert(val, make_int_type(1, /*is_four=*/false), false, l);
}

// Materialize one gate input terminal as its 1-bit logic value: a load of the
// input signal bit, or a folded constant. Pool VPI encoding: val=may,
// xz=may&~must (bytes: val-plane then xz-plane); an X const code is (val=1,
// xz=1). `pass_z` selects the MOS pass-switch reading of a constant z
// terminal, which stays z (LRM Table 28-6); every other gate reads z as x
// (LRM Table 28-3).
ir::Index Lowerer::gate_input_value(const GateInput& in, ir::Builder& b, ir::SourceLoc l, bool pass_z) {
    const ir::Ref logic1 = module_.pool.logic_type;
    if (!in.is_const) {
        return b.unary(ir::Inst::Tag::Load, gate_bit_lvalue(in.sig, in.bit, b, l), l);
    }
    switch (in.const_code & 3) {
        case 0b00:
            return b.constant(module_.pool.put_int(logic1, 0), l);
        case 0b11:
            return b.constant(module_.pool.put_int(logic1, 1), l);
        case 0b01:
            if (pass_z) {
                return b.constant(z_constant_ref(1), l);
            }
            break;
        default:
            break;
    }
    const uint8_t xbytes[2] = { 0x01, 0x01 };

    return b.constant(module_.pool.put_value(logic1, { xbytes, 2 }), l);
}

// Collect the any-change sensitivity over a gate's unique non-const input
// signals into `wait_events`.
void Lowerer::collect_gate_sensitivity(std::span<const GateInput> inputs, std::vector<ir::WaitEvent>& wait_events) {
    ankerl::unordered_dense::set<uint32_t> seen;
    for (const GateInput& in : inputs) {
        if (!in.is_const && seen.insert(static_cast<uint32_t>(in.sig)).second) {
            wait_events.push_back(ir::WaitEvent{ .signal = in.sig, .edge = ir::Edge::Any });
        }
    }
}

// Fold the gate inputs with its binary operation into a 1-bit logic value and
// collect the any-change sensitivity over the non-const input signals (into
// `wait_events`). Shared by the direct-store path (emit_gate_as_process) and
// the resolved-net driver path (emit_gate_resolved_driver).
ir::Index Lowerer::gate_folded_value(GateOp                      op,
                                     std::span<const GateInput>  inputs,
                                     std::vector<ir::WaitEvent>& wait_events,
                                     ir::Builder&                b,
                                     ir::SourceLoc               l) {
    const ir::Ref logic1 = module_.pool.logic_type;

    std::optional<ir::Index> acc;
    const ir::Inst::Tag      fold = gate_fold_tag(op);
    for (const GateInput& in : inputs) {
        const ir::Index val = gate_input_value(in, b, l);
        acc                 = acc ? b.binary(fold, *acc, val, l) : val;
    }
    if (!acc) {
        // 0-input degenerate gate: output X.
        const uint8_t xbytes[2] = { 0x01, 0x01 };
        acc                     = b.constant(module_.pool.put_value(logic1, { xbytes, 2 }), l);
    }
    if (gate_inverted(op)) {
        acc = b.unary(ir::Inst::Tag::Bnot, *acc, l);
    } else if (inputs.size() == 1) {
        // A gate primitive never outputs z: a z input is treated as x (LRM
        // Table 28-3). Multi-input folds (Band/Bor/Bxor) and the inverting
        // Bnot already clear the z plane, but a single-input non-inverting
        // buf/and passes the loaded value through unchanged — so its z plane
        // survives and the resolver would mistake the driver for undriven.
        // AND with 1 forces z to x (a no-op the z-elision pass removes when
        // the input provably carries no z).
        acc = b.binary(ir::Inst::Tag::Band, *acc, b.constant(module_.pool.put_int(logic1, 1), l), l);
    }

    collect_gate_sensitivity(inputs, wait_events);
    return b.convert(*acc, logic1, false, l);
}

// Lower a three-state gate (bufif/notif, LRM 28.6, Table 28-5) or a MOS pass
// switch (nmos/pmos/rnmos/rpmos, LRM 28.7, Table 28-6) to its 1-bit output
// value. When the control is at the active level the data passes (buf) or
// inverts (notif) to the output; at the inactive level the output is high-Z;
// an x/z control yields X. The LRM's weak L/H results for an x/z control
// (0-or-z, 1-or-z) collapse to X here: Clotho has no value-strength model,
// matching iverilog on a plain wire (docs/LRM_NOTES.md, note 37).
//
// `pass_z` is the entire difference between Table 28-5 and Table 28-6. A
// bufif reads a z data bit as x (the Band-with-1 / Bnot clears the z plane),
// but a MOS switch is a pass device and never manufactures an x from a
// floating input, so a definite z data bit reaches the output as z for every
// control value. Keeping the z plane and pre-empting on `data === z` gives
// Table 28-6's DATA=z row exactly and leaves every other cell identical.
ir::Index Lowerer::gate_tristate_value(TriGate                     tg,
                                       const GateInput&            data,
                                       const GateInput&            ctrl,
                                       std::vector<ir::WaitEvent>& wait_events,
                                       ir::Builder&                b,
                                       ir::SourceLoc               l,
                                       bool                        pass_z) {
    const ir::Ref   logic1    = module_.pool.logic_type;
    const ir::Index one       = b.constant(module_.pool.put_int(logic1, 1), l);
    const ir::Index zero      = b.constant(module_.pool.put_int(logic1, 0), l);
    const uint8_t   xbytes[2] = { 0x01, 0x01 };
    const ir::Index x_const   = b.constant(module_.pool.put_value(logic1, { xbytes, 2 }), l);
    const ir::Index z_const   = b.constant(z_constant_ref(1), l);

    const ir::Index data_v = gate_input_value(data, b, l, pass_z);
    const ir::Index ctrl_v = gate_input_value(ctrl, b, l);
    // The Band-with-1 exists only to clear the data's z plane; a pass switch
    // keeps that plane instead.
    const ir::Index passed = tg.invert ? b.unary(ir::Inst::Tag::Bnot, data_v, l)
                             : pass_z  ? data_v
                                       : b.binary(ir::Inst::Tag::Band, data_v, one, l);
    // Definite (===) tests: control exactly at the active / inactive level.
    const ir::Index active_lvl   = tg.active_high ? one : zero;
    const ir::Index inactive_lvl = tg.active_high ? zero : one;
    const ir::Index is_active    = b.binary(ir::Inst::Tag::CaseEq, ctrl_v, active_lvl, l);
    const ir::Index is_inactive  = b.binary(ir::Inst::Tag::CaseEq, ctrl_v, inactive_lvl, l);
    // active -> passed data; else inactive -> Z; else (control x/z) -> X.
    const ir::Index inner  = b.select(is_inactive, z_const, x_const, l);
    ir::Index       result = b.select(is_active, passed, inner, l);
    if (pass_z) {
        // A floating data input leaves the output floating for every control
        // value, which is Table 28-6's all-z DATA=z row.
        const ir::Index data_is_z = b.binary(ir::Inst::Tag::CaseEq, data_v, z_const, l);
        result                    = b.select(data_is_z, z_const, result, l);
    }

    collect_gate_sensitivity(std::array<GateInput, 2>{ data, ctrl }, wait_events);
    return b.convert(result, logic1, false, l);
}

// Lower one gate output as a continuous-assign process: store the pre-folded
// 1-bit value `val` to the output bit, subscribing to `wait_events`. A non-null
// `delay_expr` (`and #d (...)`, LRM 28.16) makes the store inertial — the same
// epoch-cancelled deferred record a delayed continuous assign uses (caller has
// already guarded a whole-scalar output).
void Lowerer::emit_gate_as_process(GateTerm                   output,
                                   ir::Index                  val,
                                   std::vector<ir::WaitEvent> wait_events,
                                   ir::Builder&               b,
                                   ir::SourceLoc              l,
                                   const Expression*          delay_expr) {
    const ir::Index lv = gate_bit_lvalue(output.sig, output.bit, b, l);
    if (delay_expr != nullptr) {
        b.store_inertial_delay(lv, val, scaled_delay(*delay_expr, b), l);
    } else {
        b.binary(ir::Inst::Tag::Store, lv, val, l);
    }
    add_continuous_process(std::move(wait_events), l, b);
}

// Lower one gate output that drives a multiply-driven (resolved) net: stage the
// pre-folded 1-bit value `val` into this driver's own slot (whole net, or a
// constant sub-range with a Z background) so the net's resolver folds it
// against the other drivers — exactly like a resolved continuous assign
// (emit_resolved_driver). `left` is the output lvalue; a 2-state one (a member
// of a 4-state packed net) already arrived through gate_output_value, and the
// slot itself is net-typed, hence 4-state (LRM 6.7.1), so it can hold the Z
// background a partial driver needs. A non-null `delay_expr` (`buf #d`, LRM
// 28.16) makes that slot store inertial, delaying this gate's own contribution
// ahead of resolution.
void Lowerer::emit_gate_resolved_driver(const Expression&          left,
                                        ir::Signal                 net,
                                        ir::Index                  val,
                                        std::vector<ir::WaitEvent> wait_events,
                                        ir::Builder&               b,
                                        ir::SourceLoc              l,
                                        uint16_t                   strength,
                                        const Expression*          delay_expr) {
    const ir::Index  slot_val = build_driver_slot_value(net, left, val, b, l);
    const ir::Signal slot     = alloc_driver_slot(net, l, strength);
    store_driver_slot(left, slot, slot_val, delay_expr, b, l);
    add_continuous_process(std::move(wait_events), l, b);
}

} // namespace clotho::lower
