// Functional coverage (LRM 19): the run-time half.
//
// Design: docs/design/functional-coverage.md. The frontend evaluates every bin
// specification at lowering time and emits a static, read-only model of the whole
// design's coverage — types, coverpoints, bins, inclusive value ranges, and the
// resolved options — as LLVM constants (LlvmBackend.cpp, `cover_model_global`).
// This file owns everything that depends on run-time values: instances, hit
// counts, bin matching, the 19.11 arithmetic, and the end-of-run report.
//
// A covergroup instance is NOT a garbage-collected object. It is malloc'd here
// and lives until the process exits, threaded onto its type's registry, because
// LRM 19.11.3 aggregates type coverage over instances the design has already
// dropped. A covergroup handle in the design is the 64-bit address of one
// rt_cover_inst; null is 0, exactly like a class handle.
//
// The model structs below are mirrored field-for-field by the backend. Any change
// is a lockstep two-file edit; rt_cover_init cross-checks the struct sizes the
// backend computes independently.
#ifndef CLOTHO_RUNTIME_COVERAGE_H
#define CLOTHO_RUNTIME_COVERAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -- The static model ---------------------------------------------------------

// Bin kinds, LRM 19.5.1 / 19.5.5 / 19.5.6. `Illegal` is checked before anything
// else at every sample: 19.5.6 gives it precedence over every other bin.
#define RT_COVER_BIN_NORMAL  0u
#define RT_COVER_BIN_IGNORE  1u
#define RT_COVER_BIN_ILLEGAL 2u
// A `default` bin (LRM 19.5): catches values no other bin of the coverpoint
// takes, and is excluded from the coverage computation. Carries no ranges.
#define RT_COVER_BIN_DEFAULT 3u

// One inclusive value range of one bin, in the coverpoint's own value space.
// Already cast, clamped and x/z-filtered by the frontend (LRM 19.5.7), so a
// comparison here is a plain unsigned or signed 64-bit compare depending on
// rt_cover_point::is_signed.
typedef struct rt_cover_range {
    uint64_t lo;
    uint64_t hi;
} rt_cover_range;

// One bin. `name` is the FULL LRM 19.5.1 bin name, rendered by the frontend:
// "lo", "b[7]", "b[3]" for a `[]` array over the value 3, "auto[0:1]". Rendering
// there rather than here is what keeps 19.11.3's merge-by-name comparison a
// string compare with no naming logic in two places.
typedef struct rt_cover_bin {
    const char*           name;
    uint32_t              kind;
    uint32_t              nranges;
    const rt_cover_range* ranges;
} rt_cover_bin;

typedef struct rt_cover_point {
    const char* name;
    // "<file>:<line>: ", for the illegal-bin fatal.
    const char*         banner;
    uint32_t            nbins;
    const rt_cover_bin* bins;
    // Resolved instance options (LRM 19.7). Constant in this phase: a
    // non-constant option expression is refused in the frontend.
    uint32_t weight;
    uint32_t goal;
    uint32_t at_least;
    // Non-zero when the bins were created automatically (LRM 19.5.3), which
    // changes the coverage denominator to MIN(auto_bin_max, 2^M) — auto_total
    // below — per 19.11.1.
    uint32_t is_auto;
    uint32_t auto_total;
    // Coverpoint value signedness, for the range compares.
    uint32_t is_signed;
} rt_cover_point;

typedef struct rt_cover_type {
    // The covergroup type name, or the property name for a class-embedded
    // covergroup (whose type is anonymous in slang).
    const char* name;
    // Declaration site, for the report.
    const char*           banner;
    uint32_t              npoints;
    const rt_cover_point* points;
    uint32_t              weight;
    uint32_t              goal;
    uint32_t              per_instance;
    // A hierarchical prefix for instances created in this scope ("top.u1."), so
    // one covergroup type declared in a module instantiated twice reports two
    // distinguishable instances.
    const char* scope;
} rt_cover_type;

// Emitted once per design as `clotho.cover.config` and handed to rt_cover_init.
typedef struct rt_cover_config {
    const rt_cover_type* types;
    uint32_t             ntypes;
    // sizeof(rt_cover_type) / sizeof(rt_cover_point) / sizeof(rt_cover_bin) /
    // sizeof(rt_cover_range) as the BACKEND computed them. Cross-checked against
    // this file's own sizes so a layout divergence fails loudly instead of
    // reading garbage.
    uint32_t sizeof_type;
    uint32_t sizeof_point;
    uint32_t sizeof_bin;
    uint32_t sizeof_range;
    // Report destination from `--coverage <file>`, or null for no report. The
    // design still samples and `get_coverage()` still answers without one.
    const char* report_path;
} rt_cover_config;

// -- Lifecycle ----------------------------------------------------------------

// Publish the design's coverage model. Called once from main before the
// simulation starts, and again (with the same pointer) on a second in-process
// run, which resets every instance and count.
void rt_cover_init(const rt_cover_config* config);

// Write the report to the configured path, if any. Called from rt_sim_run after
// the `final` blocks, so it runs on both the natural-drain and $finish paths.
void rt_cover_report(void);

// Release every instance. Called from rt_cover_init on a re-run and at process
// teardown; a report must already have been written.
void rt_cover_shutdown(void);

// -- Instances ----------------------------------------------------------------

// `covergroup_type ident = new;` (LRM 19.3). `ctx` is the enclosing object's
// handle for a class-embedded covergroup, so the generated sample body can read
// the class properties its coverpoints name, and 0 otherwise. Returns the
// covergroup handle. Never returns null: an allocation failure is fatal.
uint64_t rt_cover_new(uint32_t type_idx, uint64_t ctx);

// The captured `ctx`, read by the generated sample body.
uint64_t rt_cover_ctx(uint64_t h);

// Registry walk, for the synthesized coverage-event process: every live instance
// of one covergroup type, in creation order. rt_cover_next(0) is 0.
uint64_t rt_cover_first(uint32_t type_idx);
uint64_t rt_cover_next(uint64_t h);

// -- Sampling -----------------------------------------------------------------

// One coverpoint's sampled value (LRM 19.5). `xmask` carries the 4-state unknown
// bits: a value with any x/z matches no bin at all, since automatic bins consider
// only 2-state values (19.5.3) and an explicit 2-state bin compares with `===`.
// A hit on an `illegal_bins` bin is a located run-time fatal (19.5.6).
void rt_cover_hit(uint64_t h, uint32_t point, uint64_t value, uint64_t xmask);

// `start()` / `stop()` (LRM 19.8). While stopped, rt_cover_hit counts nothing.
void rt_cover_start(uint64_t h, int32_t on);

// `set_inst_name(string)` (LRM 19.8). Copies the bytes.
void rt_cover_set_inst_name(uint64_t h, const char* name, uint64_t len);

// -- Queries (LRM 19.8, 19.11) ------------------------------------------------
//
// Each query is split into three entry points rather than one taking `ref int`
// out-parameters, so the frontend stores the two counts through the ordinary
// assignment path instead of passing pointers into the runtime.

// get_inst_coverage(): this instance only, 0.0 .. 100.0.
double  rt_cover_inst_coverage(uint64_t h);
int32_t rt_cover_inst_covered(uint64_t h);
int32_t rt_cover_inst_total(uint64_t h);

// get_coverage(): the covergroup TYPE, over every instance ever created
// (19.11.3, weighted-average form — merge_instances is refused in the frontend).
double  rt_cover_type_coverage(uint32_t type_idx);
int32_t rt_cover_type_covered(uint32_t type_idx);
int32_t rt_cover_type_total(uint32_t type_idx);

// A coverpoint of one instance / of a type: `cv1.x.get_inst_coverage()` and
// `cg::x::get_coverage()`. `point` indexes rt_cover_type::points.
double  rt_cover_point_coverage(uint64_t h, uint32_t point);
int32_t rt_cover_point_covered(uint64_t h, uint32_t point);
int32_t rt_cover_point_total(uint64_t h, uint32_t point);

// $get_coverage() (LRM 19.9): the overall coverage of every covergroup type,
// 100.0 when the design has no covergroup instances or every weight is zero.
double rt_cover_design_coverage(void);

// -- Test oracle --------------------------------------------------------------

// The hit count of one bin of one instance, for in-process tests. -1 when the
// handle, point or bin index is out of range.
int64_t rt_cover_bin_count(uint64_t h, uint32_t point, uint32_t bin);

// The number of instances of a type ever created.
uint32_t rt_cover_ninst(uint32_t type_idx);

#ifdef __cplusplus
}
#endif

#endif // CLOTHO_RUNTIME_COVERAGE_H
