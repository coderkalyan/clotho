// Covergroups: coverage model construction and sampling.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Fold one covergroup expression to an integer, in the covergroup's own scope.
// LRM 19.5 lets a bin bound name a covergroup formal or an instance constant,
// which makes the bin SET a per-instance run-time quantity; formals are refused
// (below), so everything that reaches here must fold, and one that does not is
// named rather than approximated.
int64_t Lowerer::cover_const(const Expression& e, const char* what) {
    slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
    slang::ast::EvalContext    ectx(actx);
    const slang::ConstantValue cv = e.eval(ectx);
    if (!cv.isInteger()) {
        // TODO: a non-constant bin bound or coverage option (LRM 19.5.1's
        // `bins good = {[low:high]}` over covergroup formals). It needs the bin
        // table built per instance at construction time -- see §6.2 of the
        // design document -- rather than folded into the static model.
        fail(std::string("a non-constant ") + what + " is not supported");
    }
    const std::optional<int64_t> n = cv.integer().as<int64_t>();
    if (!n.has_value()) {
        fail(std::string("a ") + what + " wider than 64 bits is not supported");
    }
    return *n;
}

// The value space of a coverpoint type.
Lowerer::CoverSpace Lowerer::cover_space_of(const slang::ast::Type& t) {
    const slang::ast::Type& ct = t.getCanonicalType();
    if (ct.isFloating()) {
        // TODO: coverpoints of real expressions (LRM 19.5, 2023). Real bins
        // partition a range into type_option.real_interval-wide intervals with
        // their own inclusive/exclusive naming -- a second bin algebra over
        // floats, not a widening of this one.
        fail("a coverpoint of a real expression is not supported");
    }
    if (!ct.isIntegral()) {
        fail("a coverpoint must be an integral expression");
    }
    CoverSpace s;
    s.width     = ct.getBitWidth();
    s.is_signed = ct.isSigned();
    s.four      = ct.isFourState();
    if (s.width == 0 || s.width > 64) {
        // TODO: a coverpoint wider than 64 bits. The sampled value crosses the
        // runtime ABI as one word, and a bin range is a pair of them.
        fail("a coverpoint wider than 64 bits is not supported");
    }
    if (s.is_signed) {
        s.min = static_cast<uint64_t>(-(int64_t{ 1 } << (s.width - 1)));
        s.max = static_cast<uint64_t>((int64_t{ 1 } << (s.width - 1)) - 1);
    } else {
        s.min = 0;
        s.max = s.width == 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << s.width) - 1);
    }
    s.cardinality = s.width >= 64 ? ~uint64_t{ 0 } : (uint64_t{ 1 } << s.width);
    return s;
}

// Truncate a folded bound to the coverpoint's own width, the way LRM 19.5.7
// requires: `b` is statically cast to the effective type of the coverpoint
// expression. A bound outside the expressible range is clamped by the caller
// (19.5.7's range-intersection rule) rather than wrapping silently.
uint64_t Lowerer::cover_trunc(int64_t v, const CoverSpace& s) {
    if (s.width >= 64) {
        return static_cast<uint64_t>(v);
    }
    const uint64_t mask = (uint64_t{ 1 } << s.width) - 1;
    return static_cast<uint64_t>(v) & mask;
}

// Ordering on the coverpoint's own value space: a signed coverpoint compares
// its bounds as two's-complement, an unsigned one as magnitudes. The runtime
// applies the same rule from rt_cover_point::is_signed.
bool Lowerer::cover_le(uint64_t a, uint64_t b, const CoverSpace& s) {
    return s.is_signed ? static_cast<int64_t>(a) <= static_cast<int64_t>(b) : a <= b;
}

// Render a value for a bin name, per LRM 19.5.1 ("bin names are
// binname[value]"). Signed coverpoints print signed, which is what a user
// reading the report expects of `bit signed [2:0]`.
std::string Lowerer::cover_val_str(uint64_t v, const CoverSpace& s) {
    if (!s.is_signed) {
        return std::to_string(v);
    }
    int64_t sv = static_cast<int64_t>(v);
    if (s.width < 64) {
        const uint64_t sign = uint64_t{ 1 } << (s.width - 1);
        sv =
          (v & sign) != 0 ? static_cast<int64_t>(v | ~((uint64_t{ 1 } << s.width) - 1)) : static_cast<int64_t>(v);
    }
    return std::to_string(sv);
}

// One bin range list, expanded from a bins construct's covergroup_range_list.
// Every element is either a single value or an inclusive range; `$` resolves to
// the coverpoint type's extreme (LRM 19.5.1's "$ represents the maximum value").
std::vector<ir::CoverRange> Lowerer::cover_ranges_of(const slang::ast::CoverageBinSymbol& bin,
                                                     const CoverSpace&                    space) {
    std::vector<ir::CoverRange> out;
    for (const Expression* e : bin.getValues()) {
        if (e == nullptr) {
            continue;
        }
        if (e->kind == ExpressionKind::ValueRange) {
            const auto& vr = e->as<slang::ast::ValueRangeExpression>();
            if (vr.rangeKind != slang::ast::ValueRangeKind::Simple) {
                // TODO: the `+/-` and `+%-` tolerance forms (LRM 11.4.13). They
                // exist for real bins, where a tolerance range is the point; on
                // an integral coverpoint they are legal but vanishingly rare.
                fail("a tolerance range in a coverage bin is not supported");
            }
            // `$` on either end stays an unbounded literal in slang's AST
            // (it is not rewritten to the type's extreme), so resolve it here.
            const bool     lo_unbounded = vr.left().type->isUnbounded();
            const bool     hi_unbounded = vr.right().type->isUnbounded();
            const uint64_t lo = lo_unbounded ? space.min : cover_trunc(cover_const(vr.left(), "bin bound"), space);
            const uint64_t hi = hi_unbounded ? space.max : cover_trunc(cover_const(vr.right(), "bin bound"), space);
            if (!cover_le(lo, hi, space)) {
                // LRM 19.5.7: a range with no expressible value contributes
                // nothing. Dropped rather than refused -- slang has already
                // warned about a reversed range.
                continue;
            }
            out.push_back({ lo, hi });
            continue;
        }
        if (e->type->isUnbounded()) {
            fail("a bare '$' is not a coverage bin value");
        }
        const uint64_t v = cover_trunc(cover_const(*e, "bin value"), space);
        out.push_back({ v, v });
    }
    return out;
}

// How many values a range list holds, saturating: the `[]` and `[N]` forms
// enumerate them, so a range list spanning the whole 64-bit space has to be
// refused rather than expanded.
uint64_t Lowerer::cover_range_count(const std::vector<ir::CoverRange>& rs, const CoverSpace& s) {
    uint64_t n = 0;
    for (const ir::CoverRange& r : rs) {
        const uint64_t span = s.is_signed
                                ? static_cast<uint64_t>(static_cast<int64_t>(r.hi) - static_cast<int64_t>(r.lo))
                                : r.hi - r.lo;
        if (span >= kCoverMaxBins || n > kCoverMaxBins) {
            return ~uint64_t{ 0 };
        }
        n += span + 1;
    }
    return n;
}

// Enumerate a range list into individual values, in specification order, which
// is the order LRM 19.5.1's `[N]` distribution rule walks ("the first B
// specified values are assigned to the first bin").
std::vector<uint64_t> Lowerer::cover_values_of(const std::vector<ir::CoverRange>& rs, const CoverSpace& s) {
    std::vector<uint64_t> vals;
    for (const ir::CoverRange& r : rs) {
        uint64_t v = r.lo;
        while (true) {
            vals.push_back(v);
            if (v == r.hi) {
                break;
            }
            v = s.is_signed ? static_cast<uint64_t>(static_cast<int64_t>(v) + 1) : v + 1;
        }
    }
    return vals;
}

// Expand one `bins` / `ignore_bins` / `illegal_bins` construct into model bins.
void Lowerer::cover_expand_bin(const slang::ast::CoverageBinSymbol& bin,
                               const CoverSpace&                    space,
                               const CoverOpts&                     opts,
                               ir::CoverPoint&                      cp) {
    const std::string name = std::string(bin.name);
    if (bin.isWildcard) {
        // TODO: `wildcard bins` (LRM 19.5.4). The bin becomes a (mask, pattern)
        // pair rather than a range, which the runtime match loop does not carry.
        fail("a wildcard coverage bin is not supported");
    }
    if (!bin.getTransList().empty() || bin.isDefaultSequence) {
        // TODO: transition bins (LRM 19.5.2). A per-bin automaton over the
        // sampled-value stream with state that survives across samples, plus
        // `default sequence`; see §11 P3 of the design document.
        fail("a transition coverage bin is not supported");
    }
    if (bin.getSetCoverageExpr() != nullptr) {
        // TODO: set-expression bins (LRM 19.5.1.2): `bins b = <array expr>`.
        fail("a set-expression coverage bin is not supported");
    }
    if (bin.getWithExpr() != nullptr) {
        // TODO: `bins b[] = cp with (item ...)` (LRM 19.5.1.1). Filtering needs
        // the candidate value enumerated against a predicate at construction.
        fail("a 'with' filter on a coverage bin is not supported");
    }
    if (bin.getCrossSelectExpr() != nullptr) {
        fail("a cross coverage bin is not supported");
    }
    if (bin.getIffExpr() != nullptr) {
        // TODO: a per-bin `iff` guard (LRM 19.5.1). It is a run-time predicate
        // per bin, so it belongs in the generated sample body once the bin
        // identity crosses back from the runtime.
        fail("an 'iff' guard on a coverage bin is not supported");
    }
    const ir::CoverBinKind kind =
      bin.binsKind == slang::ast::CoverageBinSymbol::IgnoreBins    ? ir::CoverBinKind::Ignore
      : bin.binsKind == slang::ast::CoverageBinSymbol::IllegalBins ? ir::CoverBinKind::Illegal
                                                                   : ir::CoverBinKind::Normal;
    if (bin.isDefault) {
        if (bin.isArray) {
            // TODO: `bins others[] = default` (LRM 19.5.1), one bin per
            // uncovered value. The value set is only known at sample time.
            fail("an array of default coverage bins is not supported");
        }
        if (kind != ir::CoverBinKind::Normal) {
            // LRM 19.5: "A default bin cannot be explicitly ignored."
            fail("a default coverage bin cannot be an ignore_bins or illegal_bins");
        }
        cp.bins.push_back({ name, ir::CoverBinKind::Default, {} });
        return;
    }
    std::vector<ir::CoverRange> ranges = cover_ranges_of(bin, space);
    if (!bin.isArray) {
        cp.bins.push_back({ name, kind, std::move(ranges) });
        return;
    }
    const uint64_t nvals = cover_range_count(ranges, space);
    if (nvals > kCoverMaxBins) {
        // The LRM sets no limit, so this is clotho's: `bins b[] = {[0:$]}` on a
        // 32-bit coverpoint asks for four billion named bins.
        fail("a coverage bin array of more than " + std::to_string(kCoverMaxBins) + " bins is not supported");
    }
    const std::vector<uint64_t> vals  = cover_values_of(ranges, space);
    const Expression*           nexpr = bin.getNumberOfBinsExpr();
    if (nexpr == nullptr) {
        // `bins b[]`: one bin per value, named `b[value]` (LRM 19.5.1).
        // Duplicates are retained -- 19.5.1 says so explicitly, and each
        // occurrence gets its own bin.
        for (const uint64_t v : vals) {
            cp.bins.push_back({ name + "[" + cover_val_str(v, space) + "]", kind, { { v, v } } });
        }
        return;
    }
    // `bins b[N]`: N bins named b[0]..b[N-1], with the values distributed
    // B = max(1, floor(nvals / N)) at a time and the remainder in the last
    // occupied bin (LRM 19.5.1's worked example).
    const int64_t n = cover_const(*nexpr, "coverage bin count");
    if (n <= 0) {
        fail("a coverage bin array count must be positive");
    }
    const uint64_t nbins = static_cast<uint64_t>(n);
    if (nbins > kCoverMaxBins) {
        fail("a coverage bin array of more than " + std::to_string(kCoverMaxBins) + " bins is not supported");
    }
    const uint64_t per = std::max<uint64_t>(1, vals.size() / nbins);
    size_t         at  = 0;
    for (uint64_t bi = 0; bi < nbins; ++bi) {
        std::vector<ir::CoverRange> rs;
        // The last bin takes everything left, which is what makes an
        // unevenly-divisible list land its remainder there.
        const size_t take = bi + 1 == nbins ? vals.size() - std::min(vals.size(), at)
                                            : std::min<size_t>(per, vals.size() - std::min(vals.size(), at));
        for (size_t k = 0; k < take; ++k) {
            rs.push_back({ vals[at + k], vals[at + k] });
        }
        at += take;
        // An empty bin is kept: 19.5.1's "some of the bins will be empty" is
        // observable, and 19.11.1 excludes it from the ratio by having no value.
        cp.bins.push_back({ name + "[" + std::to_string(bi) + "]", kind, std::move(rs) });
    }
    (void)opts;
}

// Automatic bins (LRM 19.5.3), created only when the coverpoint declares no
// bins except ignore / illegal ones. N is the enumeration's cardinality for an
// enum coverpoint, else MIN(2^M, auto_bin_max); the 2^M values are distributed
// uniformly with the remainder in the last bin, and names are `auto[value]` or
// `auto[low:high]`.
void Lowerer::cover_auto_bins(const slang::ast::Type& type,
                              const CoverSpace&       space,
                              const CoverOpts&        opts,
                              ir::CoverPoint&         cp) {
    cp.is_auto                 = true;
    const slang::ast::Type& ct = type.getCanonicalType();
    if (ct.isEnum()) {
        const auto& et = ct.as<slang::ast::EnumType>();
        for (const auto& ev : et.values()) {
            const slang::ConstantValue& cv = ev.getValue();
            if (!cv.isInteger()) {
                continue;
            }
            const std::optional<int64_t> v = cv.integer().as<int64_t>();
            if (!v.has_value()) {
                fail("an enum coverpoint value wider than 64 bits is not supported");
            }
            const uint64_t u = cover_trunc(*v, space);
            // For an enum, 19.5.3 names the bin after the named constant.
            cp.bins.push_back({ "auto[" + std::string(ev.name) + "]", ir::CoverBinKind::Normal, { { u, u } } });
        }
        cp.auto_total = static_cast<uint32_t>(cp.bins.size());
        return;
    }
    const uint64_t cardinality = space.cardinality;
    const uint64_t nbins       = std::min<uint64_t>(opts.auto_bin_max, cardinality);
    if (nbins == 0) {
        fail("option.auto_bin_max must be positive");
    }
    if (nbins > kCoverMaxBins) {
        fail("option.auto_bin_max above " + std::to_string(kCoverMaxBins) + " is not supported");
    }
    cp.auto_total      = static_cast<uint32_t>(nbins);
    const uint64_t per = cardinality / nbins; // >= 1, since nbins <= cardinality
    uint64_t       lo  = space.min;
    for (uint64_t bi = 0; bi < nbins; ++bi) {
        // Walk the value space in the coverpoint's own order, so a signed
        // coverpoint's first automatic bin starts at its most negative value.
        const uint64_t    span = bi + 1 == nbins ? cardinality - per * bi : per;
        const uint64_t    hi = space.is_signed
                                 ? static_cast<uint64_t>(static_cast<int64_t>(lo) + static_cast<int64_t>(span) - 1)
                                 : lo + span - 1;
        const std::string nm = span == 1
                                 ? "auto[" + cover_val_str(lo, space) + "]"
                                 : "auto[" + cover_val_str(lo, space) + ":" + cover_val_str(hi, space) + "]";
        cp.bins.push_back({ nm, ir::CoverBinKind::Normal, { { lo, hi } } });
        lo = space.is_signed ? static_cast<uint64_t>(static_cast<int64_t>(hi) + 1) : hi + 1;
    }
}

// Resolve one `option.x = expr` / `type_option.x = expr` setter onto `opts`.
// slang validated the name and rejected a duplicate, so anything here is a
// real member of 19.10's structs; the value is folded, since 19.7 evaluates an
// option assignment when the covergroup is instantiated and a per-instance
// value would have to live in the instance record.
void Lowerer::cover_apply_option(const slang::ast::CoverageOptionSetter& set, CoverOpts& opts) {
    const std::string_view name  = set.getName();
    const Expression&      whole = set.getExpression();
    if (whole.kind != ExpressionKind::Assignment) {
        fail("unsupported coverage option assignment");
    }
    const Expression& rhs    = whole.as<slang::ast::AssignmentExpression>().right();
    const auto        as_u32 = [&](const char* what) {
        const int64_t v = cover_const(rhs, what);
        if (v < 0) {
            fail(std::string("a negative ") + what + " is not legal");
        }
        return static_cast<uint32_t>(v);
    };
    if (name == "weight") {
        opts.weight = as_u32("coverage option 'weight'");
        return;
    }
    if (name == "goal") {
        opts.goal = as_u32("coverage option 'goal'");
        return;
    }
    if (name == "at_least") {
        opts.at_least = as_u32("coverage option 'at_least'");
        return;
    }
    if (name == "auto_bin_max") {
        opts.auto_bin_max = as_u32("coverage option 'auto_bin_max'");
        return;
    }
    if (name == "per_instance") {
        opts.per_instance = as_u32("coverage option 'per_instance'") != 0;
        return;
    }
    // TODO: the remaining options of LRM Table 19-1 / 19-3. `name` and
    // `comment` are report metadata the model has no field for;
    // `detect_overlap` is a bin-pair warning; `cross_*` belong with cross
    // coverage; `strobe`, `merge_instances`, `distribute_first` and
    // `real_interval` each change a semantic this phase does not implement.
    fail("coverage option '" + std::string(name) + "' is not supported");
}

// Build the static model of one covergroup type, or return the index already
// built for it.
uint32_t Lowerer::cover_type_of(const slang::ast::CovergroupType& cgt) {
    if (const auto it = cover_decls_.find(&cgt); it != cover_decls_.end()) {
        return it->second.type_idx;
    }
    if (cgt.getBaseGroup() != nullptr) {
        // TODO: `covergroup extends` (LRM 19.4.1). Inherited members arrive as
        // TransparentMemberSymbol wrappers, coverpoints override by name, and a
        // derived group is a separate type for get_coverage().
        fail("a derived covergroup (LRM 19.4.1) is not supported");
    }
    if (!cgt.getArguments().empty()) {
        // TODO: covergroup formals (LRM 19.3). An `input` formal is a value
        // captured at construction; a `ref` formal tracks the actual, which
        // needs the instance record to carry the storage a coverpoint re-reads.
        fail("covergroup arguments are not supported");
    }
    const slang::ast::CovergroupBodySymbol& body = cgt.getBody();
    const slang::ast::SubroutineSymbol*     samp = nullptr;
    if (const Symbol* s = body.find("sample"); s != nullptr && s->kind == SymbolKind::Subroutine) {
        samp = &s->as<slang::ast::SubroutineSymbol>();
    }
    if (samp != nullptr && !samp->getArguments().empty()) {
        // TODO: `with function sample(args)` (LRM 19.8.1). The formals become
        // the sample body's own, and a coverpoint may name one -- which is the
        // shape a UVM monitor uses to sample values it holds locally.
        fail("a covergroup with a custom sample method (LRM 19.8.1) is not supported");
    }

    ir::CoverType ct;
    ct.name   = cover_group_name(cgt);
    ct.banner = banner_at(cgt.location);
    ct.scope  = instance_path();
    if (!ct.scope.empty()) {
        ct.scope += '.';
    }
    CoverOpts gopts;
    for (const slang::ast::CoverageOptionSetter& set : body.options) {
        cover_apply_option(set, gopts);
    }
    ct.weight       = gopts.weight;
    ct.goal         = gopts.goal;
    ct.per_instance = gopts.per_instance;

    std::vector<const slang::ast::CoverpointSymbol*> points;
    for (const Symbol& m : body.members()) {
        switch (m.kind) {
            case SymbolKind::Coverpoint:
                points.push_back(&m.as<slang::ast::CoverpointSymbol>());
                break;
            case SymbolKind::CoverCross:
                // TODO: `cross` (LRM 19.6). The cartesian product of target bins
                // plus the binsof / intersect / with select algebra; see §11 P2
                // of the design document.
                fail("cross coverage (LRM 19.6) is not supported");
            case SymbolKind::TransparentMember:
                // Only a derived covergroup has these, and that is refused above.
                fail("an inherited covergroup member is not supported");
            default:
                // slang pre-populates the body with `option` / `type_option`
                // properties and the six built-in methods; nothing else is
                // behavioral.
                break;
        }
    }
    if (points.empty()) {
        // LRM 19.11: a covergroup with no coverpoints or crosses has a zero
        // denominator, which is legal and scores 0.0 (or 100.0 at weight 0).
        // Nothing to refuse -- the runtime implements that table.
    }
    for (const slang::ast::CoverpointSymbol* cp : points) {
        ct.points.push_back(cover_point_model(*cp, gopts));
    }

    const uint32_t idx = static_cast<uint32_t>(module_.cover_types.size());
    module_.cover_types.push_back(std::move(ct));
    // Insert BEFORE building the sample body: a coverpoint expression cannot
    // reach this covergroup, but the body build reads cover_decls_ for the
    // index, and an exception from it must not leave a half-built entry that a
    // later `new` of the same type would reuse.
    cover_decls_.emplace(&cgt, CoverDecl{ .type_idx = idx, .sample = ir::FunctionId{} });
    const ir::FunctionId sid  = build_cover_sample(cgt, idx, points);
    cover_decls_[&cgt].sample = sid;
    cover_event_process(cgt, idx, sid);
    return idx;
}

// One coverpoint's model. The coverage expression's own type decides the value
// space (LRM 19.5: the inferred type is the self-determined type of the
// expression, or the declared coverpoint type when one is given).
ir::CoverPoint Lowerer::cover_point_model(const slang::ast::CoverpointSymbol& cp, const CoverOpts& gopts) {
    ir::CoverPoint out;
    out.name   = cp.name.empty() ? "cp" : std::string(cp.name);
    out.banner = banner_at(cp.location);
    if (cp.getIffExpr() != nullptr) {
        // TODO: a coverpoint `iff` guard (LRM 19.5). It is one predicate around
        // the sample of this coverpoint, evaluated live at the sample point --
        // the design document records that decision; what is missing is only
        // the guarded region in the generated body.
        fail("an 'iff' guard on a coverpoint is not supported");
    }
    const CoverSpace space = cover_space_of(cp.getType());
    out.is_signed          = space.is_signed;
    // A coverpoint's options default to the covergroup's for at_least and
    // auto_bin_max (LRM Table 19-2's "default for coverpoints"), but NOT for
    // weight and goal, which Table 19-2 says do not act as defaults.
    CoverOpts opts;
    opts.at_least     = gopts.at_least;
    opts.auto_bin_max = gopts.auto_bin_max;
    for (const slang::ast::CoverageOptionSetter& set : cp.options) {
        cover_apply_option(set, opts);
    }
    out.weight   = opts.weight;
    out.goal     = opts.goal;
    out.at_least = opts.at_least;

    // LRM 19.5.3: automatic bins happen when the coverpoint defines no bins
    // EXCEPT ignore / illegal ones, so those are expanded either way.
    bool any_real_bin = false;
    for (const Symbol& m : cp.members()) {
        if (m.kind != SymbolKind::CoverageBin) {
            continue;
        }
        const auto& bin = m.as<slang::ast::CoverageBinSymbol>();
        cover_expand_bin(bin, space, opts, out);
        if (bin.binsKind == slang::ast::CoverageBinSymbol::Bins && !bin.isDefault) {
            any_real_bin = true;
        }
    }
    if (!any_real_bin) {
        // LRM 19.5: with no explicit data type, "the inferred data type for the
        // coverpoint shall be the self-determined type of the coverpoint
        // expression" -- and for an enum that decides the AUTOMATIC BIN COUNT
        // (19.5.3: N is the cardinality of the enumeration, not 2^M, and each
        // bin is named after its constant).
        //
        // Neither slang-side type says so on its own: the CoverpointSymbol's
        // DeclaredType carries the integral BASE type, and the coverage
        // expression is bound as an rvalue against it, so a conversion node sits
        // on top of the enum-typed reference. So the enumeration comes off the
        // expression with those conversions peeled -- a three-enumerand
        // `bit [1:0]` enum is 3 named bins, never 4 named auto[k].
        const slang::ast::Type& declared = cp.getType();
        const slang::ast::Type* et       = cover_enum_type(cp.getCoverageExpr());
        cover_auto_bins(
          declared.getCanonicalType().isEnum() ? declared : (et != nullptr ? *et : declared), space, opts, out);
    }
    if (out.bins.size() > kCoverMaxBins) {
        fail("a coverpoint of more than " + std::to_string(kCoverMaxBins) + " bins is not supported");
    }
    return out;
}

// The enumeration a coverage expression names, with the implicit conversions
// slang inserts when binding it against the coverpoint's integral base type
// peeled off. Null when the expression is not an enum reference.
const slang::ast::Type* Lowerer::cover_enum_type(const Expression& e) {
    const Expression* cur = &e;
    while (cur->kind == ExpressionKind::Conversion) {
        cur = &cur->as<slang::ast::ConversionExpression>().operand();
    }
    if (cur->type != nullptr && cur->type->getCanonicalType().isEnum()) {
        return cur->type;
    }
    return nullptr;
}

// The covergroup type's reported name. A class-embedded covergroup type is
// ANONYMOUS in slang (CoverSymbols.cpp: the name goes on the implicitly
// declared class property instead), so the property that names it is what the
// user, and the report, calls it.
std::string Lowerer::cover_group_name(const slang::ast::CovergroupType& cgt) {
    if (!cgt.name.empty()) {
        return std::string(cgt.name);
    }
    const slang::ast::Scope* owner = cgt.getParentScope();
    if (owner != nullptr) {
        for (const Symbol& m : owner->members()) {
            if (m.kind != SymbolKind::ClassProperty && m.kind != SymbolKind::Variable) {
                continue;
            }
            const slang::ast::Type& t = m.as<slang::ast::ValueSymbol>().getType();
            if (&t.getCanonicalType() == &cgt) {
                return std::string(m.name);
            }
        }
    }
    return "covergroup";
}

// The generated sample body: `void(u64 handle)`. It evaluates every coverpoint
// expression in the covergroup's declaring context and hands each value to the
// coverage runtime, which owns bin matching. One body per covergroup type, so
// an explicit `cg.sample()` and the coverage-event process share it.
ir::FunctionId Lowerer::build_cover_sample(const slang::ast::CovergroupType&                       cgt,
                                           uint32_t                                                type_idx,
                                           const std::vector<const slang::ast::CoverpointSymbol*>& points) {
    const ir::FunctionId id   = module_.reserve_function(module_.pool.void_type);
    const ir::Ref        word = make_int_type(64, false, false);
    const ir::SourceLoc  l    = to_source_loc(cgt.location);

    FnCtx ctx;
    ctx.name_prefix = "cover.sample.";
    ctx.is_task     = false;
    // Local 0 is the covergroup handle formal. An embedded covergroup also
    // needs the enclosing object, which the instance captured at construction:
    // Local 1 holds it, and `this` inside the body resolves through it, so a
    // coverpoint over a class property reads the object that built the group
    // rather than whatever object happens to be calling sample().
    ctx.local_types.push_back(word);
    const bool embedded =
      cgt.getParentScope() != nullptr && cgt.getParentScope()->asSymbol().kind == SymbolKind::ClassType;
    if (embedded) {
        const auto& owner = cgt.getParentScope()->asSymbol().as<slang::ast::ClassType>();
        ctx.this_class    = lower_class_type(owner);
        ctx.this_local    = static_cast<uint32_t>(ctx.local_types.size());
        ctx.local_types.push_back(ctx.this_class);
        ctx.has_this = true;
    }
    const FnScope fn_scope(*this, &ctx);

    ir::Builder               b(module_);
    std::optional<LowerError> failure;
    try {
        const ir::Index handle = b.local(0, word, l);
        if (embedded) {
            const ir::Index dst   = b.local(fn_ctx_->this_local, fn_ctx_->this_class, l);
            const ir::Index owner = b.cover_query(
              ir::CoverOp::Ctx, { { b.unary(ir::Inst::Tag::Load, handle, l) } }, word, type_idx, 0, l);
            b.binary(ir::Inst::Tag::Store, dst, owner, l);
        }
        for (uint32_t k = 0; k < points.size(); ++k) {
            const slang::ast::CoverpointSymbol& cp = *points[k];
            const Expression&                   ce = cp.getCoverageExpr();
            const CoverSpace                    sp = cover_space_of(cp.getType());
            const ir::Index                     v  = lower_expr(ce, b);
            // The value crosses the ABI as one unsigned word; a signed
            // coverpoint is sign-extended first so the runtime's signed compare
            // sees the same number the design does.
            const ir::Index val = b.resize(v, make_int_type(64, false, sp.is_signed), l);
            // LRM 19.5.3 / 19.5.4: a sampled value carrying x or z matches no
            // bin. One bit is enough to say so -- which bits are unknown does
            // not matter while wildcard bins are refused -- so $isunknown is
            // the whole mask.
            const ir::Index xmask =
              sp.four ? b.resize(b.unary(ir::Inst::Tag::IsUnknown, v, l), word, l) : int_const(0, 64, b, l);
            b.cover_ctl(ir::CoverOp::Hit,
                        { { b.unary(ir::Inst::Tag::Load, handle, l), b.resize(val, word, l), xmask } },
                        type_idx,
                        k,
                        l);
        }
    } catch (const LowerError& e) {
        // The id is already reserved with no rollback, so complete it with a
        // dead body before propagating -- build_action_body's discipline.
        failure = e;
    }
    b.ret_void(l);
    module_.set_function(id,
                         ir::Function{ .name        = "cover.sample." + std::to_string(type_idx),
                                       .return_type = module_.pool.void_type,
                                       .param_types = { word },
                                       .arg_dirs    = { ir::ArgDir::In },
                                       .local_types = std::move(ctx.local_types),
                                       .is_task     = false,
                                       .ir          = b.finish() });
    if (failure) {
        throw *failure;
    }
    return id;
}

// The coverage event (LRM 19.3): `covergroup cg @(posedge clk)`. One
// Activation::Observed process per covergroup TYPE walks that type's instance
// registry and samples each one, rather than one process per instance: an
// Observed body is resumed unconditionally at every settled point, so the cost
// has to be per type (see §5 of the design document).
void Lowerer::cover_event_process(const slang::ast::CovergroupType& cgt, uint32_t type_idx, ir::FunctionId sample) {
    const slang::ast::TimingControl* ev = cgt.getCoverageEvent();
    if (ev == nullptr) {
        // No coverage event: sampling is procedural, through sample().
        return;
    }
    if (cgt.getParentScope() != nullptr && cgt.getParentScope()->asSymbol().kind == SymbolKind::ClassType) {
        // TODO: a coverage event on a class-embedded covergroup (LRM 19.4's
        // `covergroup cov1 @m_z;`). The event is a property of an object that
        // does not exist at elaboration, so it cannot become a static
        // sensitivity; it needs a per-object subscription.
        fail("a coverage event on a covergroup embedded in a class is not supported");
    }
    if (ev->kind == slang::ast::TimingControlKind::BlockEventList) {
        // TODO: `@@(begin blk)` / `@@(end blk)` (LRM 19.3). Nothing in clotho
        // notifies on block entry or exit, so every named block, task, function
        // and method would need a hook.
        fail("a block-event coverage event (@@) is not supported");
    }
    if (ev->kind != slang::ast::TimingControlKind::SignalEvent) {
        // TODO: an event LIST as the coverage event.
        fail("this covergroup coverage event form is not supported");
    }
    const auto& sev = ev->as<slang::ast::SignalEventControl>();
    if (sev.iffCondition != nullptr) {
        // TODO: `iff` on the coverage event. The guard is one predicate around
        // the walk below; refused with the coverpoint-level guard so the whole
        // `iff` surface lands at once.
        fail("an 'iff' guard on a covergroup coverage event is not supported");
    }
    const ir::Signal    clock = event_signal(sev.expr);
    const ir::Edge      edge  = to_edge(sev.edge);
    const ir::SourceLoc l     = to_source_loc(cgt.location);
    const ir::Ref       word  = make_int_type(64, false, false);

    // Per-instance edge state, exactly the SVA monitor's shape: the clock's
    // value at the previous Observed pass. Read LIVE, not from a Preponed
    // shadow, which is what makes two clocking events in one time step sample
    // twice as LRM 19.3 requires.
    const ir::Signal prev = declare_temp(mask_type(1), "cover_clk");
    const TempCell   cur  = make_temp_cell(word, "cover_inst");

    ir::Builder     b(module_);
    const ir::Index clk_now = b.resize(load_signal(clock, b, l), module_.pool.bit_type, l);
    const ir::Index clk_was = load_signal(prev, b, l);
    ir::Index       fired   = ir::Index{};
    switch (edge) {
        case ir::Edge::Positive:
            fired = b.binary(ir::Inst::Tag::Band, clk_now, bit_not(clk_was, b, l), l);
            break;
        case ir::Edge::Negative:
            fired = b.binary(ir::Inst::Tag::Band, clk_was, bit_not(clk_now, b, l), l);
            break;
        case ir::Edge::Any:
            fired = b.binary(ir::Inst::Tag::Ne, clk_now, clk_was, l);
            break;
    }
    b.binary(ir::Inst::Tag::Store, b.signal(prev, l), clk_now, l);

    b.open_region();
    // Walk this type's instances. A registry walk rather than a captured
    // handle: the design may build any number of instances of one covergroup
    // type, and the runtime owns them all.
    const ir::Index cur_lv = cur.lv(b, l);
    const ir::Index first  = b.cover_query(ir::CoverOp::First, {}, word, type_idx, 0, l);
    b.binary(ir::Inst::Tag::Store, cur_lv, first, l);
    b.open_region();
    const ir::Index      h    = b.unary(ir::Inst::Tag::Load, cur.lv(b, l), l);
    const ir::Index      pred = b.binary(ir::Inst::Tag::Ne, h, int_const(0, 64, b, l), l);
    const ir::ExtraSlice cond = b.close_region();
    b.open_region();
    const ir::Index bh = b.unary(ir::Inst::Tag::Load, cur.lv(b, l), l);
    b.call(sample, { { bh } }, l);
    const ir::Index next_lv = cur.lv(b, l);
    const ir::Index next    = b.cover_query(ir::CoverOp::Next, { { bh } }, word, type_idx, 0, l);
    b.binary(ir::Inst::Tag::Store, next_lv, next, l);
    const ir::ExtraSlice body = b.close_region();
    b.open_region();
    const ir::ExtraSlice no_step = b.close_region();
    b.loop(cond, pred, body, no_step, /*bottom_test=*/false, l);
    const ir::ExtraSlice then_region = b.close_region();
    b.open_region();
    const ir::ExtraSlice no_else = b.close_region();
    b.cond(fired, then_region, no_else, l);

    module_.add_process(ir::Process{ .activation = ir::Activation::Observed,
                                     // Not a subscription -- an Observed process is resumed once per settled
                                     // point, never woken by a net event. Kept as the record of which event
                                     // delimits this covergroup's sampling.
                                     .wait_events = { ir::WaitEvent{ .signal = clock, .edge = edge } },
                                     .self_start  = false,
                                     .span        = { l, l },
                                     .ir          = b.finish(),
                                     .scope       = cur_scope_id() });
}

// `covergroup_type ident = new;` (LRM 19.3).
ir::Index Lowerer::lower_new_covergroup(const slang::ast::NewCovergroupExpression& nc, ir::Builder& b) {
    const slang::ast::Type& t = nc.type->getCanonicalType();
    if (!t.isCovergroup()) {
        fail("expected a covergroup type");
    }
    const auto&         cgt = t.as<slang::ast::CovergroupType>();
    const ir::SourceLoc l   = loc(nc);
    if (!nc.arguments.empty()) {
        fail("covergroup arguments are not supported");
    }
    const uint32_t idx  = cover_type_of(cgt);
    const ir::Ref  word = make_int_type(64, false, false);
    // An embedded covergroup captures the object that constructs it, so its
    // coverpoints keep reading that object's properties however sample() is
    // later reached (LRM 19.4 requires the `new` to sit in the enclosing
    // class's constructor, which is what makes `this` available here).
    const bool embedded =
      cgt.getParentScope() != nullptr && cgt.getParentScope()->asSymbol().kind == SymbolKind::ClassType;
    std::vector<ir::Index> args;
    if (embedded) {
        if (fn_ctx_ == nullptr || !fn_ctx_->has_this) {
            fail("an embedded covergroup must be constructed from a method of its class");
        }
        args.push_back(this_handle(b, l));
    }
    return b.cover_query(ir::CoverOp::New, args, word, idx, 0, l);
}

// Whether `sub` is one of a covergroup's, coverpoint's or cross's built-in
// methods (LRM 19.8). Discriminated on the OWNER's kind, not on a name and
// package the way the `std` classes are: a covergroup type lives in user scope.
bool Lowerer::is_cover_method(const slang::ast::SubroutineSymbol& sub) {
    if (!sub.flags.has(slang::ast::MethodFlags::BuiltIn)) {
        return false;
    }
    const slang::ast::Scope* owner = sub.getParentScope();
    if (owner == nullptr) {
        return false;
    }
    const SymbolKind k = owner->asSymbol().kind;
    return k == SymbolKind::CovergroupBody || k == SymbolKind::Coverpoint || k == SymbolKind::CoverCross;
}

// A covergroup / coverpoint built-in method call (LRM 19.8). `recv` is the
// covergroup handle expression; a coverpoint-level call arrives with the
// coverpoint named as the member, which the caller resolved.
ir::Index Lowerer::lower_cover_method(const slang::ast::CallExpression&   call,
                                      const slang::ast::SubroutineSymbol& sub,
                                      ir::Builder&                        b) {
    const ir::SourceLoc      l     = loc(call);
    const slang::ast::Scope* owner = sub.getParentScope();
    if (owner->asSymbol().kind == SymbolKind::CoverCross) {
        fail("cross coverage (LRM 19.6) is not supported");
    }
    // The receiver is the covergroup variable; slang models a coverpoint method
    // as a member of the coverpoint scope reached through it.
    const Expression* recv = cover_receiver(call);
    if (recv == nullptr) {
        fail("a covergroup method needs a covergroup handle receiver");
    }
    const slang::ast::Type& rt = recv->type->getCanonicalType();
    if (!rt.isCovergroup()) {
        // A coverpoint-level call (`cv.x.get_inst_coverage()`); the handle is
        // one hop further out.
        fail("a coverpoint-level coverage method is not supported");
    }
    const auto&     cgt  = rt.as<slang::ast::CovergroupType>();
    const uint32_t  idx  = cover_type_of(cgt);
    const ir::Ref   real = module_.pool.real_type;
    const ir::Index h    = lower_expr(*recv, b);
    const auto      args = call.arguments();
    if (sub.name == "sample") {
        if (!args.empty()) {
            fail("a covergroup with a custom sample method (LRM 19.8.1) is not supported");
        }
        b.call(cover_decls_.at(&cgt).sample, { { h } }, l);
        return ir::Index{};
    }
    if (sub.name == "start" || sub.name == "stop") {
        b.cover_ctl(ir::CoverOp::Start, { { h, int_const(sub.name == "start" ? 1 : 0, 32, b, l) } }, idx, 0, l);
        return ir::Index{};
    }
    if (sub.name == "set_inst_name") {
        if (args.size() != 1) {
            fail("set_inst_name takes one string argument");
        }
        b.cover_ctl(ir::CoverOp::SetName, { { h, lower_expr(*args[0], b) } }, idx, 0, l);
        return ir::Index{};
    }
    const bool type_level = sub.name == "get_coverage";
    if (sub.name != "get_inst_coverage" && !type_level) {
        fail("unsupported coverage method '" + std::string(sub.name) + "'");
    }
    // The optional (ref int covered, ref int total) pair (LRM 19.8): stored
    // through the ordinary assignment path from two more queries, rather than
    // handing pointers to the runtime.
    //
    // slang declares both formals WITH defaults (SVInt(32, 0, true)), so a
    // no-argument call still arrives carrying two argument expressions -- the
    // defaults, which are literals and not assignable. What distinguishes the
    // two forms is therefore whether the actual names storage, not the argument
    // count.
    const bool has_outs =
      args.size() == 2 && args[0]->getSymbolReference() != nullptr && args[1]->getSymbolReference() != nullptr;
    if (has_outs) {
        const ir::CoverOp      cov = type_level ? ir::CoverOp::TypeCovered : ir::CoverOp::InstCovered;
        const ir::CoverOp      tot = type_level ? ir::CoverOp::TypeTotal : ir::CoverOp::InstTotal;
        std::vector<ir::Index> qargs;
        if (!type_level) {
            qargs.push_back(h);
        }
        const ir::Index covered_lv = lower_lvalue(*args[0], b, /*is_write=*/true);
        const ir::Index covered    = b.cover_query(cov, qargs, make_int_type(32, false, true), idx, 0, l);
        b.binary(ir::Inst::Tag::Store, covered_lv, covered, l);
        const ir::Index total_lv = lower_lvalue(*args[1], b, /*is_write=*/true);
        const ir::Index total    = b.cover_query(tot, qargs, make_int_type(32, false, true), idx, 0, l);
        b.binary(ir::Inst::Tag::Store, total_lv, total, l);
    }
    std::vector<ir::Index> qargs;
    if (!type_level) {
        qargs.push_back(h);
    }
    return b.cover_query(
      type_level ? ir::CoverOp::TypeCoverage : ir::CoverOp::InstCoverage, qargs, real, idx, 0, l);
}

// The covergroup handle a built-in coverage method was called on.
const Expression* Lowerer::cover_receiver(const slang::ast::CallExpression& call) {
    if (const Expression* t = call.thisClass(); t != nullptr) {
        return t;
    }
    return nullptr;
}

} // namespace clotho::lower
