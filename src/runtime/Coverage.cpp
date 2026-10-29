// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Functional coverage (LRM 19): instances, bin matching, the 19.11 arithmetic
// and the end-of-run report. Coverage.h is the contract and states the division
// of labour: the frontend evaluates every bin specification at lowering time and
// publishes a static, read-only model; everything here depends on run-time
// values.
//
// Three properties this file is built around, all of them consequences of the
// clause text rather than choices:
//
//   * An instance is immortal. LRM 19.11.3 aggregates type coverage over the
//     instances the design has already dropped, so an instance is malloc'd,
//     threaded onto its type's registry in creation order, and released only by
//     rt_cover_shutdown. It is not a garbage-collected object.
//   * A sample can increment more than one counter. LRM 19.5.1 keeps duplicate
//     values, so bin matching is a full scan, not a first-match search.
//   * The coverage arithmetic has a zero-denominator table (19.11 a-d, 19.11.1
//     a-d) that is the whole difference between a compliant number and a
//     plausible one. It appears at three levels and is implemented once, in
//     empty_coverage() and Weighted, so the three levels cannot drift.
//
// The report is a deterministic text file, ordered by (type index, instance
// creation order) and never by pointer order, so two runs of one design diff
// cleanly (design §9).
#include "Coverage.h"

#include "Runtime.h"

#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// The one-block instance layout below sizes itself in uint64_t and narrows once
// to size_t. A 32-bit host would need an overflow check there instead; every
// other size assumption in this runtime is already 64-bit (Gc.cpp:2963).
static_assert(sizeof(size_t) >= sizeof(uint64_t), "the runtime assumes a 64-bit host");

// A model string, or a stand-in when the frontend left one null. Every string in
// the model is a compile-time constant, so this only ever fires on a
// half-populated model -- but a report full of "(null)" is a worse diagnostic
// than a named placeholder.
const char* or_else(const char* s, const char* fallback) {
    return s != nullptr && s[0] != '\0' ? s : fallback;
}

// -- Loud failure -------------------------------------------------------------

// A model banner is "<file>:<line>: ", the form a JIT'd message prepends to its
// text. A location line wants the same text without the separator, so trim it.
const char* location(char* out, size_t cap, const char* banner) {
    assert(out != nullptr && cap != 0);
    if (banner == nullptr || banner[0] == '\0') {
        std::snprintf(out, cap, "<unknown>");
        return out;
    }
    size_t n = std::strlen(banner);
    while (n != 0 && (banner[n - 1] == ' ' || banner[n - 1] == ':')) {
        n -= 1;
    }
    std::snprintf(out, cap, "%.*s", static_cast<int>(n), banner);
    return out;
}

// gc_fatal's block (Gc.cpp:436) with the source location it cannot have: a
// coverpoint carries a compile-time banner, so the line gc_fatal fills with
// "<unknown>" names the declaration instead. Everything else is identical --
// headline, one note, simulation time, VCD close, exit(1) -- because the failure
// mode is the same one: no defined state to hand back mid-instant, and a nonzero
// status to distinguish this from a clean run.
[[noreturn]] void cover_fatal(const char* banner, const char* headline, const char* note) {
    assert(headline != nullptr);
    std::fflush(stdout);
    char where[256];
    std::fprintf(stderr, "fatal: %s\n", headline);
    std::fprintf(stderr, "%s\n", location(where, sizeof(where), banner));
    if (note != nullptr) {
        std::fprintf(stderr, "  note: %s\n", note);
    }
    std::fprintf(stderr, "  note: at time %" PRIu64 "\n", rt_diag_now());
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    std::exit(1);
}

// The four sizes the backend computed from its own mirror of the model layouts,
// checked before anything reads the model and in RELEASE as well as debug.
// Generated code hands over pointers into arrays it laid out itself, so a drifted
// mirror makes every bin, range and option read land in the wrong field -- a
// silent wrong-code bug rather than a diagnosable one, which is exactly what a
// located fatal at startup is for (Gc.cpp:2482-2522 makes the same argument for
// the collector's three offsets).
void check_backend_mirror(const rt_cover_config* config) {
    assert(config != nullptr);
    struct Check {
        const char* what;
        uint32_t    backend;
        uint32_t    here;
    };
    const Check checks[] = {
        { "rt_cover_type", config->sizeof_type, static_cast<uint32_t>(sizeof(rt_cover_type)) },
        { "rt_cover_point", config->sizeof_point, static_cast<uint32_t>(sizeof(rt_cover_point)) },
        { "rt_cover_bin", config->sizeof_bin, static_cast<uint32_t>(sizeof(rt_cover_bin)) },
        { "rt_cover_range", config->sizeof_range, static_cast<uint32_t>(sizeof(rt_cover_range)) },
    };
    for (const Check& c : checks) {
        if (c.backend == c.here) {
            continue;
        }
        char note[192];
        std::snprintf(note,
                      sizeof(note),
                      "the backend computed sizeof(%s) = %u; this runtime computes %u",
                      c.what,
                      c.backend,
                      c.here);
        cover_fatal(nullptr, "the coverage model layout disagrees with the backend's mirror of it", note);
    }
}

// -- Owning the model ---------------------------------------------------------
//
// The published model is LLVM constant data the JIT owns, and the JIT is torn
// down when the run ends -- while covergroup instances are immortal, because LRM
// 19.11.3 aggregates instances the design has already dropped. So every entry
// point but rt_cover_report (which runs inside rt_sim_run, before teardown) would
// read freed memory through the caller's pointer. rt_cover_init therefore DEEP
// COPIES the whole tree -- types, coverpoints, bins, ranges and every string --
// into runtime-owned storage and publishes that copy instead.
//
// One block, one free: the model is a tree of fixed-size arrays plus strings, so
// a measure pass followed by a copy pass over the same source replaces per-node
// ownership bookkeeping entirely, and leaves no partially-built tree to unwind if
// the allocation fails.
struct Arena {
    uint8_t* base; // Null while measuring: arena_take then only totals bytes up.
    uint64_t used;
    uint64_t cap;
};

uint64_t align8(uint64_t n) {
    return (n + 7u) & ~static_cast<uint64_t>(7u);
}

// Reserve `bytes`, at 8-byte alignment for a struct array and unaligned for a
// string. Returns null during the measure pass, so one traversal serves both.
//
// An empty reservation is null rather than a zero-length pointer into the block:
// that makes "count == 0 implies pointer == null" hold for every array in the
// copy, which is what the rest of this file already assumes of an empty node --
// and it keeps a zero-length memcpy from being handed a null source, which is
// undefined even at size zero.
void* arena_take(Arena& a, uint64_t bytes, bool aligned) {
    if (bytes == 0) {
        return nullptr;
    }
    if (aligned) {
        a.used = align8(a.used);
    }
    const uint64_t at = a.used;
    a.used += bytes;
    if (a.base == nullptr) {
        return nullptr;
    }
    // The two passes walk the same source, so they must agree to the byte.
    assert(a.used <= a.cap && "the measure pass and the copy pass disagree");
    return a.base + at;
}

// A null source string stays null: or_else() and location() both distinguish
// "the frontend left this out" from "the frontend published an empty string".
const char* arena_str(Arena& a, const char* s) {
    if (s == nullptr) {
        return nullptr;
    }
    const uint64_t n   = std::strlen(s) + 1;
    char*          out = static_cast<char*>(arena_take(a, n, false));
    if (out != nullptr) {
        std::memcpy(out, s, n);
    }
    return out;
}

// Copy the published model into `a`. Run once with a measuring arena to size the
// block, then again with a real one to fill it; the return value is meaningful
// only on the second pass.
//
// Each node is copied by struct assignment and then has its pointer fields
// overwritten, so a new scalar option in the model needs no line here.
const rt_cover_config* copy_model(Arena& a, const rt_cover_config* src) {
    assert(src != nullptr);
    rt_cover_config* cfg = static_cast<rt_cover_config*>(arena_take(a, sizeof(rt_cover_config), true));

    // Effective counts, computed from the source so both passes agree: a nonzero
    // count with a null array is a malformed model, and normalizing it to zero
    // here is what keeps every later walk from dereferencing the null.
    const uint32_t ntypes = src->types != nullptr ? src->ntypes : 0;
    rt_cover_type* types =
      static_cast<rt_cover_type*>(arena_take(a, static_cast<uint64_t>(ntypes) * sizeof(rt_cover_type), true));

    for (uint32_t t = 0; t < ntypes; ++t) {
        const rt_cover_type& st      = src->types[t];
        const uint32_t       npoints = st.points != nullptr ? st.npoints : 0;
        rt_cover_point*      points =
          static_cast<rt_cover_point*>(arena_take(a, static_cast<uint64_t>(npoints) * sizeof(rt_cover_point), true));

        for (uint32_t p = 0; p < npoints; ++p) {
            const rt_cover_point& sp    = st.points[p];
            const uint32_t        nbins = sp.bins != nullptr ? sp.nbins : 0;
            rt_cover_bin*         bins =
              static_cast<rt_cover_bin*>(arena_take(a, static_cast<uint64_t>(nbins) * sizeof(rt_cover_bin), true));

            for (uint32_t b = 0; b < nbins; ++b) {
                const rt_cover_bin& sb      = sp.bins[b];
                const uint32_t      nranges = sb.ranges != nullptr ? sb.nranges : 0;
                rt_cover_range*     ranges  = static_cast<rt_cover_range*>(
                  arena_take(a, static_cast<uint64_t>(nranges) * sizeof(rt_cover_range), true));
                const char* bname = arena_str(a, sb.name);
                if (bins == nullptr) {
                    continue;
                }
                bins[b]         = sb;
                bins[b].name    = bname;
                bins[b].nranges = nranges;
                bins[b].ranges  = ranges;
                if (nranges != 0) {
                    std::memcpy(ranges, sb.ranges, static_cast<size_t>(nranges) * sizeof(rt_cover_range));
                }
            }

            const char* pname   = arena_str(a, sp.name);
            const char* pbanner = arena_str(a, sp.banner);
            if (points == nullptr) {
                continue;
            }
            points[p]        = sp;
            points[p].name   = pname;
            points[p].banner = pbanner;
            points[p].nbins  = nbins;
            points[p].bins   = bins;
        }

        const char* tname   = arena_str(a, st.name);
        const char* tbanner = arena_str(a, st.banner);
        const char* tscope  = arena_str(a, st.scope);
        if (types == nullptr) {
            continue;
        }
        types[t]         = st;
        types[t].name    = tname;
        types[t].banner  = tbanner;
        types[t].scope   = tscope;
        types[t].npoints = npoints;
        types[t].points  = points;
    }

    // Copied too, so the whole configuration is self-contained: the report path
    // is read at end of run, by which time the caller's string may be gone.
    const char* path = arena_str(a, src->report_path);
    if (cfg == nullptr) {
        return nullptr;
    }
    *cfg             = *src;
    cfg->ntypes      = ntypes;
    cfg->types       = types;
    cfg->report_path = path;
    return cfg;
}

// -- State --------------------------------------------------------------------

// One covergroup instance (LRM 19.3). One allocation holds the header, the flat
// counter array and the per-coverpoint base offsets into it: an instance is
// immortal, so three allocations per `new` would only buy three frees at
// shutdown. The counter array is flat rather than per-coverpoint because a sample
// touches exactly one coverpoint's slice and the base offset makes that slice one
// add away.
struct Inst {
    Inst*     next;   // Next instance of the SAME type, in creation order.
    uint64_t  ctx;    // The enclosing object for a class-embedded covergroup.
    char*     name;   // Owned: the default below, or set_inst_name's copy.
    uint64_t* counts; // Trailing array, sum of nbins over the type's coverpoints.
    uint32_t* base;   // Trailing array, npoints wide: each coverpoint's offset.
    uint32_t  type_idx;
    uint32_t  ordinal; // 0-based creation index within the type.
    uint32_t  ncounts;
    uint32_t  npoints;
    // LRM 19.8: start() and stop() gate a covergroup that is ALREADY collecting,
    // so this starts enabled and nothing has to call start() first.
    int32_t started;
};

// Every instance of one covergroup type. The tail pointer is what keeps the list
// in creation order at O(1) per `new`, and creation order is what makes the
// report and 19.11.3's instance walk deterministic.
struct TypeReg {
    Inst*    head;
    Inst*    tail;
    uint32_t ninst;
};

// The published model, ALWAYS the runtime's own deep copy (copy_model) and never
// the caller's pointer, because the caller's lives in JIT memory that dies with
// the run.
const rt_cover_config* g_config = nullptr;
// The single block the copy behind g_config lives in, for the single free.
uint8_t* g_model = nullptr;
// One entry per published type, or null when the design has no covergroup at all.
TypeReg* g_reg = nullptr;
// g_reg's own length, not re-derived from g_config->ntypes at teardown: one
// counter for one array is what keeps a second in-process run from freeing the
// wrong number of lists. The registry owns its length.
uint32_t g_nreg = 0;

Inst* inst_of(uint64_t h) {
    return reinterpret_cast<Inst*>(static_cast<uintptr_t>(h));
}

uint64_t handle_of(const Inst* inst) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(inst));
}

// The instance name the report shows and set_inst_name replaces: the type's
// hierarchical scope prefix and its name, plus the creation ordinal from the
// SECOND instance of the type on. LRM 19.8 leaves instance naming to the tool;
// what a report needs is that two instances of one type are distinguishable and
// that one design names them the same way on every run. The first instance keeps
// the bare name rather than every instance gaining a `_0`, because renaming it
// once a second instance appears would mutate a name set_inst_name may already
// have replaced.
char* default_name(const rt_cover_type& ty, uint32_t ordinal) {
    const char* scope = ty.scope != nullptr ? ty.scope : "";
    const char* name  = or_else(ty.name, "<covergroup>");
    char        ord[24];
    ord[0] = '\0';
    if (ordinal != 0) {
        std::snprintf(ord, sizeof(ord), "_%u", ordinal);
    }
    const size_t n   = std::strlen(scope) + std::strlen(name) + std::strlen(ord) + 1;
    char*        out = static_cast<char*>(std::malloc(n));
    if (out == nullptr) {
        cover_fatal(ty.banner, "out of memory for a covergroup instance name", nullptr);
    }
    std::snprintf(out, n, "%s%s%s", scope, name, ord);
    return out;
}

// -- Bin matching (LRM 19.5) --------------------------------------------------

// Whether the value lies in one inclusive range. The frontend already cast,
// clamped and x/z-filtered both bounds into the coverpoint's own value space
// (Coverage.h:39-42), so this is one compare in the coverpoint's signedness --
// which has to be honoured, or a bin over [-4:-1] on a signed coverpoint matches
// nothing and a bin over [0:$] matches everything.
bool in_range(const rt_cover_range& r, uint64_t value, bool is_signed) {
    if (is_signed) {
        const int64_t lo = static_cast<int64_t>(r.lo);
        const int64_t hi = static_cast<int64_t>(r.hi);
        const int64_t v  = static_cast<int64_t>(value);
        return v >= lo && v <= hi;
    }
    return value >= r.lo && value <= r.hi;
}

bool bin_holds(const rt_cover_bin& bin, uint64_t value, bool is_signed) {
    for (uint32_t i = 0; i < bin.nranges; ++i) {
        if (in_range(bin.ranges[i], value, is_signed)) {
            return true;
        }
    }
    return false;
}

// LRM 19.5.6: "a run-time error is issued" when a sampled value lands in an
// illegal_bins bin. Fatal rather than counted, because the clause makes the value
// itself the error, and located at the coverpoint like every other clotho
// run-time fatal.
[[noreturn]] void illegal_fatal(const rt_cover_type&  ty,
                                const rt_cover_point& pt,
                                const rt_cover_bin&   bin,
                                uint64_t              value) {
    char head[320];
    std::snprintf(head,
                  sizeof(head),
                  "illegal_bins bin '%s' of coverpoint '%s' in covergroup '%s' was hit",
                  or_else(bin.name, "<bin>"),
                  or_else(pt.name, "<coverpoint>"),
                  or_else(ty.name, "<covergroup>"));
    char note[192];
    if (pt.is_signed != 0) {
        std::snprintf(note,
                      sizeof(note),
                      "the sampled value was %" PRId64 "; LRM 19.5.6 makes it a run-time error",
                      static_cast<int64_t>(value));
    } else {
        std::snprintf(
          note, sizeof(note), "the sampled value was %" PRIu64 "; LRM 19.5.6 makes it a run-time error", value);
    }
    cover_fatal(pt.banner, head, note);
}

// -- The 19.11 arithmetic -----------------------------------------------------

// The numerator and denominator of one coverage ratio: 19.8's two `ref int`
// out-counts, and the two terms of the percentage they belong to.
struct Ratio {
    uint32_t covered;
    uint32_t total;
};

// LRM 19.11 (b)-(c) and 19.11.1 (b)-(c), the half of the zero-denominator table
// that says what an item with nothing to measure REPORTS: 0.0 when its weight is
// nonzero, 100.0 when its weight is zero. The other half -- that it contributes
// to neither term of its parent's average -- is Weighted's job. One function
// because the table is quoted identically at every level, and three copies of it
// would be three chances to get the arms backwards.
double empty_coverage(uint32_t weight) {
    return weight != 0 ? 0.0 : 100.0;
}

double ratio_percent(Ratio r, uint32_t weight) {
    if (r.total == 0) {
        return empty_coverage(weight);
    }
    return 100.0 * static_cast<double>(r.covered) / static_cast<double>(r.total);
}

// The weighted average `sum(W_i * C_i) / sum(W_i)` of LRM 19.11 (over
// coverpoints), 19.11.1 (over bins, via Ratio) and 19.11.3 (over instances). An
// item that does not contribute is absent from BOTH sums, which is what "does not
// contribute to the covergroup's coverage" means arithmetically -- as opposed to
// contributing a 0.0, which would drag the average down instead of leaving it
// alone.
struct Weighted {
    double sum = 0.0;
    // Widened, because a design may hold many instances of a type and each
    // contributes a full uint32_t weight.
    uint64_t weight = 0;

    void add(double coverage, uint32_t w) {
        sum += static_cast<double>(w) * coverage;
        weight += w;
    }

    // True when the average has no denominator: either nothing contributed, or
    // everything that did carries weight zero. Both are the zero-denominator case.
    bool empty() const {
        return weight == 0;
    }

    double average() const {
        assert(!empty() && "a weighted average with no weight");
        return sum / static_cast<double>(weight);
    }

    double value(uint32_t own_weight) const {
        return empty() ? empty_coverage(own_weight) : average();
    }
};

// LRM 19.11.1 for one coverpoint of one instance.
Ratio point_ratio(const Inst* inst, uint32_t point) {
    assert(inst != nullptr);
    assert(g_config != nullptr);
    const rt_cover_type& ty = g_config->types[inst->type_idx];
    assert(point < ty.npoints);
    const rt_cover_point& pt    = ty.points[point];
    const uint64_t*       count = inst->counts + inst->base[point];

    Ratio r{ 0, 0 };
    for (uint32_t b = 0; b < pt.nbins; ++b) {
        const rt_cover_bin& bin = pt.bins[b];
        // The four exclusions, all of them explicit in the clause text: an
        // ignore_bins bin (19.5.5), an illegal_bins bin (19.5.6) and the `default`
        // bin (19.5) are excluded from the coverage computation, and a bin left
        // associated with no value or range after 19.5.7's resolution "contributes
        // to neither the numerator nor the denominator" (19.11.1). The empty bin is
        // retained rather than dropped because 19.11.3 matches bins across
        // instances by NAME.
        if (bin.kind != RT_COVER_BIN_NORMAL || bin.nranges == 0) {
            continue;
        }
        r.total += 1;
        // 19.11.1: a bin counts as covered once it has been hit at_least times.
        if (count[b] >= pt.at_least) {
            r.covered += 1;
        }
    }
    // 19.11.1: for automatically created bins the denominator is
    // MIN(auto_bin_max, 2^M) -- the number of bins the coverpoint WOULD have had --
    // not the number the tool kept. That is what makes an auto bin removed by
    // ignore_bins lower the coverage instead of vanishing from it, which is
    // exactly what 19.11.1's `auto[2,3] auto[4] auto[7]` example measures.
    if (pt.is_auto != 0) {
        r.total = pt.auto_total;
    }
    // 19.11.1 (d): the two out-counts of an item with nothing to measure are both
    // zero, whatever the loop above happened to tally.
    if (r.total == 0) {
        r.covered = 0;
    }
    assert(r.covered <= r.total && "more covered bins than the coverage denominator");
    return r;
}

// One instance's answer to get_inst_coverage(), all three parts of it.
struct InstResult {
    double   coverage;    // 19.11's weighted average over the coverpoints.
    Ratio    bins;        // 19.8's two out-counts, aggregated over them.
    bool     contributes; // False in 19.11 (a): stays out of the type average.
    uint32_t weight;      // The covergroup weight, for 19.11.3's average.
};

InstResult inst_result(const Inst* inst) {
    assert(inst != nullptr);
    assert(g_config != nullptr);
    const rt_cover_type& ty = g_config->types[inst->type_idx];

    Weighted acc;
    Ratio    bins{ 0, 0 };
    for (uint32_t p = 0; p < ty.npoints; ++p) {
        const rt_cover_point& pt = ty.points[p];
        const Ratio           r  = point_ratio(inst, p);
        // 19.11.1 (a): a coverpoint with no bins to measure does not contribute to
        // the covergroup's coverage -- so it is absent from the weight sum too.
        if (r.total == 0) {
            continue;
        }
        acc.add(ratio_percent(r, pt.weight), pt.weight);
        bins.covered += r.covered;
        bins.total += r.total;
    }

    InstResult out;
    out.contributes = !acc.empty();
    // 19.11 (b)-(c): with no weight to divide by, the covergroup reports by its
    // own weight rather than by an average it cannot form.
    out.coverage = acc.value(ty.weight);
    out.weight   = ty.weight;
    // 19.11 (d): both out-counts are zero in that case.
    out.bins = out.contributes ? bins : Ratio{ 0, 0 };
    return out;
}

// One type's answer to get_coverage().
struct TypeResult {
    double coverage;
    Ratio  bins;
    bool   contributes;
};

TypeResult type_result(uint32_t type_idx) {
    assert(g_config != nullptr);
    // Both bounds, because the walk below reads the model and the registry in
    // lockstep; rt_cover_init is what makes them the same length.
    assert(type_idx < g_config->ntypes);
    assert(type_idx < g_nreg && "the registry and the model disagree on the type count");
    const rt_cover_type& ty = g_config->types[type_idx];

    // LRM 19.11.3 with merge_instances false: the weighted average of the
    // instances' coverage, each weighted by its covergroup weight. Every instance
    // of one type carries the same resolved option.weight in this phase, so the
    // sum reduces to a plain average -- written in the weighted form anyway,
    // because that is the form the clause states and the form a per-instance
    // weight would need. The union-by-bin-name merge form is refused in the
    // frontend and deliberately absent here.
    Weighted acc;
    Ratio    bins{ 0, 0 };
    for (const Inst* in = g_reg[type_idx].head; in != nullptr; in = in->next) {
        const InstResult r = inst_result(in);
        if (!r.contributes) {
            continue;
        }
        acc.add(r.coverage, r.weight);
        bins.covered += r.bins.covered;
        bins.total += r.bins.total;
    }

    TypeResult out;
    out.contributes = !acc.empty();
    out.coverage    = acc.value(ty.weight);
    out.bins        = out.contributes ? bins : Ratio{ 0, 0 };
    return out;
}

// What the report says about a bin the arithmetic does not count, and why. These
// are the same four exclusions point_ratio applies, named at the one place a
// reader is looking at a hit count and wondering why the ratio ignored it.
const char* bin_note(const rt_cover_bin& bin) {
    switch (bin.kind) {
        case RT_COVER_BIN_IGNORE:
            return "  ignore, excluded";
        case RT_COVER_BIN_ILLEGAL:
            return "  illegal, excluded";
        case RT_COVER_BIN_DEFAULT:
            return "  default, excluded";
        default:
            break;
    }
    return bin.nranges == 0 ? "  empty, excluded" : "";
}

} // namespace

// -- Lifecycle ----------------------------------------------------------------

extern "C" void rt_cover_init(const rt_cover_config* config) {
    assert(config != nullptr && "rt_cover_init needs a model");
    if (config == nullptr) {
        return;
    }
    // Against the CALLER's config, and before a single one of its arrays is read:
    // that the two sides agree on the struct sizes is exactly what makes the copy
    // below a copy rather than a reinterpretation of foreign bytes.
    check_backend_mirror(config);
    // Every instance and count from an earlier run in this process goes away here
    // rather than when that run ended: the test harness runs many designs in one
    // process and reads the oracle after a run returns, so teardown has to leave
    // the counters readable while a fresh run starts from zero. Gc.cpp:2984-2987
    // makes exactly this trade for the collector's statistics.
    rt_cover_shutdown();

    // Measure, then copy. Unconditional -- a design with no covergroup type still
    // publishes a report path, and that string has the same lifetime problem.
    Arena measure{ nullptr, 0, 0 };
    copy_model(measure, config);
    const uint64_t bytes = measure.used;
    assert(bytes >= sizeof(rt_cover_config) && "the config itself is always copied");
    g_model = static_cast<uint8_t*>(std::calloc(1, static_cast<size_t>(bytes)));
    if (g_model == nullptr) {
        char note[192];
        std::snprintf(note, sizeof(note), "%" PRIu64 " bytes for %u covergroup types", bytes, config->ntypes);
        cover_fatal(nullptr, "out of memory for the coverage model", note);
    }
    Arena build{ g_model, 0, bytes };
    g_config = copy_model(build, config);
    assert(build.used == bytes && "the measure pass and the copy pass disagree");
    assert(g_config != nullptr);

    if (g_config->ntypes == 0) {
        return; // A design with no covergroup allocates no registry at all.
    }
    g_reg = static_cast<TypeReg*>(std::calloc(g_config->ntypes, sizeof(TypeReg)));
    if (g_reg == nullptr) {
        char note[192];
        std::snprintf(note, sizeof(note), "%u covergroup types at %zu bytes each", g_config->ntypes, sizeof(TypeReg));
        cover_fatal(nullptr, "out of memory for the coverage instance registry", note);
    }
    g_nreg = g_config->ntypes;
}

extern "C" void rt_cover_report(void) {
    // No --coverage: write nothing. The design still samples and get_coverage()
    // still answers, because both are design-visible semantics that a reporting
    // option must not change (design §9).
    if (g_config == nullptr || g_config->report_path == nullptr || g_config->report_path[0] == '\0') {
        return;
    }
    std::FILE* f = std::fopen(g_config->report_path, "wb");
    if (f == nullptr) {
        // rt_vcd_begin's soft failure (runtime/Vcd.cpp): one diagnostic to
        // stderr and carry on. A report that cannot be written is not a reason to
        // fail a simulation that has already finished running.
        std::fprintf(stderr, "--coverage: cannot open '%s'\n", g_config->report_path);
        return;
    }
    assert(g_config->ntypes == g_nreg && "the registry and the model disagree on the type count");

    char loc[256];
    std::fprintf(f, "clotho functional coverage (LRM 19)\n");
    // Ordered by type index, then by instance creation order -- never by pointer
    // or hash order, so two runs of one design produce byte-identical reports and
    // a diff of them is a diff of the coverage.
    for (uint32_t t = 0; t < g_config->ntypes; ++t) {
        const rt_cover_type& ty = g_config->types[t];
        std::fprintf(
          f, "\ncovergroup %s  (%s)\n", or_else(ty.name, "<covergroup>"), location(loc, sizeof(loc), ty.banner));
        for (const Inst* in = g_reg[t].head; in != nullptr; in = in->next) {
            const InstResult ir = inst_result(in);
            std::fprintf(f,
                         "  instance %s  coverage %6.2f%%  bins %u/%u\n",
                         or_else(in->name, "<instance>"),
                         ir.coverage,
                         ir.bins.covered,
                         ir.bins.total);
            for (uint32_t p = 0; p < ty.npoints; ++p) {
                const rt_cover_point& pt    = ty.points[p];
                const Ratio           r     = point_ratio(in, p);
                const uint64_t*       count = in->counts + in->base[p];
                std::fprintf(f,
                             "    coverpoint %s  coverage %6.2f%%  bins %u/%u\n",
                             or_else(pt.name, "<coverpoint>"),
                             ratio_percent(r, pt.weight),
                             r.covered,
                             r.total);
                for (uint32_t b = 0; b < pt.nbins; ++b) {
                    const rt_cover_bin& bin = pt.bins[b];
                    // The bin name is the frontend's rendering of LRM 19.5.1 /
                    // 19.5.3, printed verbatim: it is load-bearing for 19.11.3's
                    // merge-by-name, not cosmetic.
                    std::fprintf(
                      f, "      bin %-32s %12" PRIu64 "%s\n", or_else(bin.name, "<bin>"), count[b], bin_note(bin));
                }
            }
        }
        const TypeResult tr = type_result(t);
        std::fprintf(f,
                     "  type %s  coverage %6.2f%%  bins %u/%u  instances %u\n",
                     or_else(ty.name, "<covergroup>"),
                     tr.coverage,
                     tr.bins.covered,
                     tr.bins.total,
                     g_reg[t].ninst);
    }
    std::fprintf(f, "\ndesign coverage %6.2f%%\n", rt_cover_design_coverage());
    std::fclose(f);
}

extern "C" void rt_cover_shutdown(void) {
    // g_nreg, not g_config->ntypes, is what sized g_reg: one counter per array
    // keeps the two frees below independent, so neither depends on the other
    // still being published.
    if (g_reg != nullptr) {
        for (uint32_t t = 0; t < g_nreg; ++t) {
            Inst* in = g_reg[t].head;
            while (in != nullptr) {
                // The counter and base arrays are trailing parts of this same
                // allocation, so the name is the only separate one.
                Inst* next = in->next;
                std::free(in->name);
                std::free(in);
                in = next;
            }
        }
        std::free(g_reg);
        g_reg  = nullptr;
        g_nreg = 0;
    }
    // The instances go before the model, because an instance is described by it.
    // Both are the runtime's own storage: the model is copy_model's single block,
    // so one free retires the whole tree.
    std::free(g_model);
    g_model = nullptr;
    // Idempotent, and safe before any rt_cover_init: the harness may tear down a
    // run that never started one.
    g_config = nullptr;
}

// -- Instances ----------------------------------------------------------------

extern "C" uint64_t rt_cover_new(uint32_t type_idx, uint64_t ctx) {
    // This one never degrades gracefully. The `new` site elides the null guard on
    // a fresh handle exactly like ClassNew does, so returning 0 would be a silent
    // wrong-code bug rather than a diagnosable one.
    if (g_config == nullptr || type_idx >= g_config->ntypes) {
        char note[192];
        std::snprintf(note,
                      sizeof(note),
                      "type index %u of %u published types",
                      type_idx,
                      g_config != nullptr ? g_config->ntypes : 0u);
        cover_fatal(nullptr, "a covergroup instance names a type outside the published model", note);
    }
    assert(type_idx < g_nreg && "the registry and the model disagree on the type count");
    const rt_cover_type& ty = g_config->types[type_idx];

    uint64_t ncounts = 0;
    for (uint32_t p = 0; p < ty.npoints; ++p) {
        ncounts += ty.points[p].nbins;
    }
    // The flat counter array is addressed by a uint32_t base offset. LRM 19.5.1's
    // `bins b[] = {[0:$]}` over a 32-bit coverpoint asks for 2^32 bins and the
    // clause sets no limit, so this has to be a loud refusal rather than a wrapped
    // offset; bounding it here is also what keeps the byte arithmetic below inside
    // size_t.
    if (ncounts > 0xFFFFFFFFull) {
        char note[192];
        std::snprintf(
          note, sizeof(note), "covergroup '%s' asks for %" PRIu64 " bins", or_else(ty.name, "<covergroup>"), ncounts);
        cover_fatal(ty.banner, "a covergroup has more bins than the runtime can address", note);
    }

    // One block: the header, then the counters (8-aligned, so they come first),
    // then the base offsets. calloc is what zeroes the counters.
    const uint64_t head_bytes  = (sizeof(Inst) + 7u) & ~static_cast<uint64_t>(7u);
    const uint64_t count_bytes = ncounts * sizeof(uint64_t);
    const uint64_t base_bytes  = static_cast<uint64_t>(ty.npoints) * sizeof(uint32_t);
    void*          raw         = std::calloc(1, static_cast<size_t>(head_bytes + count_bytes + base_bytes));
    if (raw == nullptr) {
        char note[192];
        std::snprintf(note,
                      sizeof(note),
                      "%" PRIu64 " bytes for %" PRIu64 " bins of covergroup '%s'",
                      head_bytes + count_bytes + base_bytes,
                      ncounts,
                      or_else(ty.name, "<covergroup>"));
        cover_fatal(ty.banner, "out of memory for a covergroup instance", note);
    }

    uint8_t* const bytes = static_cast<uint8_t*>(raw);
    Inst* const    inst  = static_cast<Inst*>(raw);
    inst->next           = nullptr;
    inst->ctx            = ctx;
    inst->counts         = ncounts != 0 ? reinterpret_cast<uint64_t*>(bytes + head_bytes) : nullptr;
    inst->base           = ty.npoints != 0 ? reinterpret_cast<uint32_t*>(bytes + head_bytes + count_bytes) : nullptr;
    inst->type_idx       = type_idx;
    inst->ordinal        = g_reg[type_idx].ninst;
    inst->ncounts        = static_cast<uint32_t>(ncounts);
    inst->npoints        = ty.npoints;
    inst->started        = 1; // LRM 19.8: a fresh instance is already collecting.

    uint32_t at = 0;
    for (uint32_t p = 0; p < ty.npoints; ++p) {
        inst->base[p] = at;
        at += ty.points[p].nbins;
    }
    assert(at == inst->ncounts && "the base offsets do not span the counter array");

    inst->name = default_name(ty, inst->ordinal);

    // Append, so the list is in creation order for the report and for 19.11.3's
    // instance walk.
    TypeReg& reg = g_reg[type_idx];
    if (reg.tail != nullptr) {
        reg.tail->next = inst;
    } else {
        reg.head = inst;
    }
    reg.tail = inst;
    reg.ninst += 1;
    return handle_of(inst);
}

extern "C" uint64_t rt_cover_ctx(uint64_t h) {
    const Inst* inst = inst_of(h);
    assert(inst != nullptr && "rt_cover_ctx on a null covergroup handle");
    return inst != nullptr ? inst->ctx : 0;
}

extern "C" uint64_t rt_cover_first(uint32_t type_idx) {
    if (g_config == nullptr || type_idx >= g_config->ntypes) {
        return 0;
    }
    assert(type_idx < g_nreg && "the registry and the model disagree on the type count");
    return handle_of(g_reg[type_idx].head);
}

extern "C" uint64_t rt_cover_next(uint64_t h) {
    const Inst* inst = inst_of(h);
    return inst != nullptr ? handle_of(inst->next) : 0;
}

// -- Sampling -----------------------------------------------------------------

extern "C" void rt_cover_hit(uint64_t h, uint32_t point, uint64_t value, uint64_t xmask) {
    Inst* inst = inst_of(h);
    assert(inst != nullptr && "rt_cover_hit on a null covergroup handle");
    if (inst == nullptr) {
        return;
    }
    // LRM 19.8: stop() suspends collection of coverage information. The caller has
    // already evaluated the coverpoint expression, which is what the clause
    // describes -- the sample happens, the count does not.
    if (inst->started == 0) {
        return;
    }
    // A sampled value carrying any x or z matches NO bin, so it is not even a
    // candidate for an illegal one: automatic bins consider only 2-state values
    // (19.5.3), and an explicit bin compares with `===`, which an unknown fails
    // against every 2-state bound the frontend resolved. Checked before the illegal
    // scan for exactly that reason.
    if (xmask != 0) {
        return;
    }
    assert(g_config != nullptr && "rt_cover_hit before rt_cover_init");
    if (g_config == nullptr) {
        return;
    }
    const rt_cover_type& ty = g_config->types[inst->type_idx];
    assert(point < ty.npoints && "a coverpoint outside the published model");
    if (point >= ty.npoints) {
        return;
    }
    const rt_cover_point& pt        = ty.points[point];
    const bool            is_signed = pt.is_signed != 0;
    uint64_t* const       count     = inst->counts + inst->base[point];

    // Pass 1 -- illegal and ignore, before anything is incremented. LRM 19.5.6
    // gives an illegal_bins bin precedence over EVERY other bin, ignore_bins
    // included ("if the value is specified in both, it shall be flagged as
    // illegal"), so an ignore match must not end the scan: a later illegal bin
    // holding the same value still has to win.
    bool ignored = false;
    for (uint32_t b = 0; b < pt.nbins; ++b) {
        const rt_cover_bin& bin = pt.bins[b];
        if (bin.kind == RT_COVER_BIN_NORMAL || bin.kind == RT_COVER_BIN_DEFAULT) {
            continue;
        }
        if (!bin_holds(bin, value, is_signed)) {
            continue;
        }
        if (bin.kind == RT_COVER_BIN_ILLEGAL) {
            illegal_fatal(ty, pt, bin, value);
        }
        assert(bin.kind == RT_COVER_BIN_IGNORE && "an unknown coverage bin kind");
        ignored = true;
    }
    // LRM 19.5.5: a value in an ignore_bins bin is excluded from coverage
    // entirely. It does not reach the bin it would otherwise have landed in, and it
    // does not fall through to the default bin either.
    if (ignored) {
        return;
    }

    // Pass 2 -- EVERY normal bin holding the value, not the first one. LRM 19.5.1:
    // "Duplicate values are retained; thus the same value can be assigned to
    // multiple bins", so a value that legitimately belongs to two bins increments
    // two counters. A first-match search here is the classic wrong answer.
    bool     hit         = false;
    uint32_t default_bin = pt.nbins;
    for (uint32_t b = 0; b < pt.nbins; ++b) {
        const rt_cover_bin& bin = pt.bins[b];
        if (bin.kind == RT_COVER_BIN_DEFAULT) {
            default_bin = b;
            continue;
        }
        if (bin.kind != RT_COVER_BIN_NORMAL) {
            continue;
        }
        if (bin_holds(bin, value, is_signed)) {
            count[b] += 1;
            hit = true;
        }
    }
    // LRM 19.5: the `default` bin catches the values no other bin of the
    // coverpoint takes. It is counted here and excluded from the arithmetic in
    // point_ratio.
    if (!hit && default_bin < pt.nbins) {
        count[default_bin] += 1;
    }
}

extern "C" void rt_cover_start(uint64_t h, int32_t on) {
    Inst* inst = inst_of(h);
    assert(inst != nullptr && "rt_cover_start on a null covergroup handle");
    if (inst == nullptr) {
        return;
    }
    inst->started = on != 0 ? 1 : 0;
}

extern "C" void rt_cover_set_inst_name(uint64_t h, const char* name, uint64_t len) {
    Inst* inst = inst_of(h);
    assert(inst != nullptr && "rt_cover_set_inst_name on a null covergroup handle");
    if (inst == nullptr) {
        return;
    }
    // The bytes are copied: the caller's string is a design value with its own
    // lifetime, and this name outlives every instance the design drops.
    const size_t n    = name != nullptr ? static_cast<size_t>(len) : 0;
    char*        copy = static_cast<char*>(std::malloc(n + 1));
    if (copy == nullptr) {
        cover_fatal(nullptr, "out of memory for a covergroup instance name", nullptr);
    }
    if (n != 0) {
        std::memcpy(copy, name, n);
    }
    copy[n] = '\0';
    std::free(inst->name);
    inst->name = copy;
}

// -- Queries (LRM 19.8, 19.11) ------------------------------------------------

extern "C" double rt_cover_inst_coverage(uint64_t h) {
    const Inst* inst = inst_of(h);
    assert(inst != nullptr && "rt_cover_inst_coverage on a null covergroup handle");
    if (inst == nullptr || g_config == nullptr) {
        return 0.0;
    }
    return inst_result(inst).coverage;
}

extern "C" int32_t rt_cover_inst_covered(uint64_t h) {
    const Inst* inst = inst_of(h);
    if (inst == nullptr || g_config == nullptr) {
        return 0;
    }
    return static_cast<int32_t>(inst_result(inst).bins.covered);
}

extern "C" int32_t rt_cover_inst_total(uint64_t h) {
    const Inst* inst = inst_of(h);
    if (inst == nullptr || g_config == nullptr) {
        return 0;
    }
    return static_cast<int32_t>(inst_result(inst).bins.total);
}

extern "C" double rt_cover_type_coverage(uint32_t type_idx) {
    if (g_config == nullptr || type_idx >= g_config->ntypes) {
        return 0.0;
    }
    return type_result(type_idx).coverage;
}

extern "C" int32_t rt_cover_type_covered(uint32_t type_idx) {
    if (g_config == nullptr || type_idx >= g_config->ntypes) {
        return 0;
    }
    return static_cast<int32_t>(type_result(type_idx).bins.covered);
}

extern "C" int32_t rt_cover_type_total(uint32_t type_idx) {
    if (g_config == nullptr || type_idx >= g_config->ntypes) {
        return 0;
    }
    return static_cast<int32_t>(type_result(type_idx).bins.total);
}

extern "C" double rt_cover_point_coverage(uint64_t h, uint32_t point) {
    const Inst* inst = inst_of(h);
    if (inst == nullptr || g_config == nullptr) {
        return 0.0;
    }
    const rt_cover_type& ty = g_config->types[inst->type_idx];
    if (point >= ty.npoints) {
        return 0.0;
    }
    return ratio_percent(point_ratio(inst, point), ty.points[point].weight);
}

extern "C" int32_t rt_cover_point_covered(uint64_t h, uint32_t point) {
    const Inst* inst = inst_of(h);
    if (inst == nullptr || g_config == nullptr) {
        return 0;
    }
    if (point >= g_config->types[inst->type_idx].npoints) {
        return 0;
    }
    return static_cast<int32_t>(point_ratio(inst, point).covered);
}

extern "C" int32_t rt_cover_point_total(uint64_t h, uint32_t point) {
    const Inst* inst = inst_of(h);
    if (inst == nullptr || g_config == nullptr) {
        return 0;
    }
    if (point >= g_config->types[inst->type_idx].npoints) {
        return 0;
    }
    return static_cast<int32_t>(point_ratio(inst, point).total);
}

extern "C" double rt_cover_design_coverage(void) {
    if (g_config == nullptr) {
        return 100.0;
    }
    // $get_coverage() (LRM 19.9): the same weighted average one level up, over
    // covergroup TYPES.
    Weighted acc;
    for (uint32_t t = 0; t < g_config->ntypes; ++t) {
        const TypeResult r = type_result(t);
        if (!r.contributes) {
            continue;
        }
        acc.add(r.coverage, g_config->types[t].weight);
    }
    // LRM 19.11's closing sentence: with no covergroup instances at all -- or with
    // every weight zero -- the overall coverage is 100%, not 0%. There is no
    // enclosing item here whose weight could select the other arm of the
    // zero-denominator table, so this is the one level where the answer is fixed.
    if (acc.empty()) {
        return 100.0;
    }
    return acc.average();
}

// -- Test oracle --------------------------------------------------------------

extern "C" int64_t rt_cover_bin_count(uint64_t h, uint32_t point, uint32_t bin) {
    const Inst* inst = inst_of(h);
    if (inst == nullptr || g_config == nullptr) {
        return -1;
    }
    const rt_cover_type& ty = g_config->types[inst->type_idx];
    if (point >= ty.npoints || bin >= ty.points[point].nbins) {
        return -1;
    }
    return static_cast<int64_t>(inst->counts[inst->base[point] + bin]);
}

extern "C" uint32_t rt_cover_ninst(uint32_t type_idx) {
    if (g_config == nullptr || type_idx >= g_config->ntypes) {
        return 0;
    }
    assert(type_idx < g_nreg && "the registry and the model disagree on the type count");
    return g_reg[type_idx].ninst;
}
