// Lowering entry points: drive slang, run the Lowerer, check and report.

#include "lower/Lowerer.h"

#include <slang/diagnostics/AllDiags.h>

namespace clotho {

namespace {

using namespace lower;

// Map slang's severity scale onto Clotho's. slang's Ignored never reaches here (an
// ignored diagnostic is not reported at all) and its Fatal collapses onto Error:
// Clotho has no "stop the world" tier, since a lowering guard that cannot continue
// throws instead.
Severity to_severity(slang::DiagnosticSeverity s) {
    switch (s) {
        case slang::DiagnosticSeverity::Ignored:
        case slang::DiagnosticSeverity::Note:
            return Severity::Note;
        case slang::DiagnosticSeverity::Warning:
            return Severity::Warning;
        case slang::DiagnosticSeverity::Error:
        case slang::DiagnosticSeverity::Fatal:
            return Severity::Error;
    }

    return Severity::Error;
}

// Normalize a slang diagnostic (and its note chain) into Clotho's shape. The
// caller attaches `rendered` — slang's own pretty text — separately, because that
// only exists once the engine has actually issued the item.
Diagnostic from_slang(const slang::DiagnosticEngine& engine, const slang::Diagnostic& d) {
    Diagnostic out;
    out.origin   = DiagnosticOrigin::Frontend;
    out.severity = to_severity(engine.getSeverity(d.code, d.location));
    out.code     = std::string(engine.getOptionName(d.code));
    out.message  = engine.formatMessage(d);
    // Prefer the primary highlighted range; fall back to a point at the reported
    // location for a diagnostic that carries no range.
    if (d.ranges.empty()) {
        const ir::SourceLoc at = to_source_loc(d.location);
        out.span               = ir::SourceSpan{ at, at };
    } else {
        out.span = to_source_span(d.ranges.front());
    }
    for (const slang::Diagnostic& note : d.notes) {
        Diagnostic n = from_slang(engine, note);
        // A note's own mapped severity is not meaningful — it is context for its
        // parent, so it always presents as a note.
        n.severity = Severity::Note;
        out.notes.push_back(std::move(n));
    }

    return out;
}

// Map slang's default severities onto Clotho's, in two clearly separate parts.
//
// THE FLOOR comes first and is not policy. Each promotion here exists because
// Clotho's own representation cannot express the alternative, so relaxing one
// would not produce a more permissive mode -- it would produce a miscompile that
// silently drops a driver, a declaration or a method body. No policy may move
// these, which is precisely why they are not reachable from the enum.
//
// THE POLICY follows, and holds only what genuinely differs by semantics. Today
// that is the inferred-latch / inferred-comb pair: legal and faithfully
// simulatable, so a warning under Sim, and the thing a synthesis run most wants
// to fail on. `AlwaysWithoutTimingControl` looks like it belongs there but does
// not -- downgrading it is Clotho's reading of LRM 9.2.2.1 (a deadlock, not an
// error) backed by lowering the construct faithfully, so it holds under every
// semantics.
//
// `policy` is passed rather than threaded from the CLI because there is one
// value: the selector arrives with the second one, and adding Synth is then an
// enumerator, an arm below, and one parameter -- not surgery on this function.
// `partial` says the compilation is known to be missing modules it instantiates,
// which makes one specific family of findings unsound; see below.
//
// NOT here, and deliberately: a width warning for an explicitly-sized literal
// whose declared width differs from the assignment target but whose VALUE still
// fits. `wire [2:0] foo = 4'b0000;` is silent in clotho and a WIDTHTRUNC in
// verilator. slang recomputes the right-hand width with constants sized to the
// minimum needed to represent them (ConversionExpression.cpp:729-737), so the
// 4-bit literal has an effective width of 1 and nothing fires. A precise check
// is available -- `IntegerLiteral::isDeclaredUnsized` separates `4'b0000` from
// the `logic [3:0] a = 1;` idiom slang's comment is protecting, and verilator
// stays quiet on the unsized form too -- and it was still declined: no
// information is lost by the conversion, and one fewer place where clotho and
// slang disagree about what the same source means is worth more than the
// finding. Revisit only with evidence of a real bug it would have caught.
void apply_severity_policy(slang::DiagnosticEngine& engine, DiagnosticPolicy policy, bool partial) {
    // ---- Invariant across policies: representation limits, one compliance
    // ---- reading, and one noise judgment. None of these vary by semantics. ----

    // LRM 6.5 / 10.3.2 both use normative "shall be an error" for a variable
    // driven by more than one continuous assignment, or by a mix of continuous
    // and procedural assignments -- slang defaults these to warnings. Clotho's
    // signal model has one driver per variable slot with no multi-driver
    // resolution, so keeping the default severity would mean one of the
    // conflicting drivers is silently dropped instead of the design rejected.
    engine.setSeverity(slang::diag::MixedVarAssigns, slang::DiagnosticSeverity::Error);
    engine.setSeverity(slang::diag::MultipleContAssigns, slang::DiagnosticSeverity::Error);
    // A name redeclared in the same scope: slang also defaults this to a warning
    // and silently keeps one declaration. Clotho's symbol table has no "last one
    // wins" rule, so promote rather than silently favoring whichever declaration
    // slang's lookup resolves to.
    engine.setSeverity(slang::diag::Redefinition, slang::DiagnosticSeverity::Error);
    engine.setSeverity(slang::diag::RedefinitionDifferentType, slang::DiagnosticSeverity::Error);
    // An out-of-block member declaration (extern method / prototype) whose
    // implementation is never found in the parent scope: slang defaults this to a
    // warning and elaborates the design without the body. Clotho would then lower
    // a call to a member with no lowered implementation.
    engine.setSeverity(slang::diag::MemberImplNotFound, slang::DiagnosticSeverity::Error);
    // slang rejects a timing-control-free `always` as a hard error, but LRM
    // 9.2.2.1 only describes it as a simulation deadlock, and Clotho lowers it
    // faithfully (see lower_procedural_block). A compliance decision, not a
    // preference, so it does not vary by policy.
    engine.setSeverity(slang::diag::AlwaysWithoutTimingControl, slang::DiagnosticSeverity::Warning);

    // Constructs the LRM makes illegal -- "shall be an error", "shall not", or no
    // derivation from the Annex A grammar -- that slang accepts with a warning for
    // compatibility with other tools. Clotho rejects source the LRM rejects.
    // Most are errors in slang's own driver without --compat, but Clotho issues
    // diagnostics through its own engine, which starts from slang's defaults.
    for (const slang::DiagCode code : {
           slang::diag::EnumRangeLiteral,              // 6.19.2
           slang::diag::EmptyUdpPort,                  // A.5.4
           slang::diag::LifetimeForPrototype,          // A.2.6
           slang::diag::InvalidPulseStyle,             // 30.7.4.1
           slang::diag::ImplicitNamedPortTypeMismatch, // 23.3.2.3
           slang::diag::SplitDistWeightOp,             // A.1.10
           slang::diag::DPIPureTask,                   // 35.5.1.3
           slang::diag::SpecifyPathConditionExpr,      // 30.4.4.1
           slang::diag::SolveBeforeDisallowed,         // A.1.10
           slang::diag::DynamicNotProcedural,          // 6.21
           slang::diag::NonstandardStringConcat,       // 6.16
           slang::diag::MisplacedTrailingSeparator,    // A.1.3
           slang::diag::QualifiersOnOutOfBlock,        // 8.24
           slang::diag::InitializerRequired,           // 6.3.2
           slang::diag::PackageImportInClass,          // A.10
           slang::diag::ForeachCallExpr,               // 12.7.3
           slang::diag::NonstandardHierarchicalCross,  // A.2.11
           slang::diag::NonstandardInside,             // A.8.3
           slang::diag::BareAssociativePattern,        // A.6.7.1
           slang::diag::DigitsLeadingUnderscore,       // 5.7.1
           slang::diag::RefArgAutomaticFunc,           // 13.5.2
           slang::diag::NonstandardConstraintBlock,    // A.1.10
           slang::diag::FormatEmptyArg,                // 21.2.1.1
           slang::diag::ClockVarTargetAssign,          // 14.16.2
           slang::diag::CrossIdentInBinsof,            // 19.6
           slang::diag::RefArgForkJoin,                // 9.3.2
           slang::diag::CannotIndexScalar,             // 11.5.1
           slang::diag::VirtualArgNameMismatch,        // 8.20
           slang::diag::ParameterDoesNotExist,         // 23.10.2.2
         }) {
        engine.setSeverity(code, slang::DiagnosticSeverity::Error);
    }
    // NOTE: SeqNoMatch stays a warning although LRM 16.12.22 forbids a degenerate
    // sequence as a property: slang reports it for sequences that can match, such
    // as `(a ##[1:$] b) intersect (c ##[1:2] d)`, whose lengths meet at 2 and 3.
    // NOTE: Three more stay warnings, as common idioms Clotho simulates as
    // written. StaticInitializerMustBeExplicit (6.21): `int i = 0;` in an initial
    // block. MultipleAlwaysAssigns (9.2.2.2): an FPGA power-on value set by an
    // initial block for an always_ff variable. UnsizedInConcat (11.4.12): `{'0, x}`,
    // whose `'0` is one bit wide in a self-determined context (5.7.1).

    // Legal source slang elaborates differently from the LRM, warning as it does.
    // Accepting it would simulate something other than what was written.
    // TODO: Each is a docs/TODO.md entry.
    // LRM 23.10.1: of several defparams on one parameter the last wins; slang
    // keeps the first.
    engine.setSeverity(slang::diag::DuplicateDefparam, slang::DiagnosticSeverity::Error);
    // LRM 34: slang drops a protected envelope's contents.
    engine.setSeverity(slang::diag::ProtectedEnvelope, slang::DiagnosticSeverity::Error);

    // The unused-parameter pair, silenced. CheckUnused is on for every run, and
    // on real parameterised RTL this one finding drowns the rest: measured on
    // hazard3, 718 of 722 unused findings are unused parameters, against three
    // genuinely dead nets/variables. That is not a defect rate -- threading a
    // config parameter set through every module and using the subset each needs
    // IS the idiom -- so the check reports a pattern rather than a problem, at a
    // volume that buries the three findings that matter. Not a semantic question,
    // hence here rather than in the policy below. NOTE: `-W` options do not
    // reach this engine (see main.cpp), so it cannot be re-enabled per design.
    engine.setSeverity(slang::diag::UnusedParameter, slang::DiagnosticSeverity::Ignored);
    engine.setSeverity(slang::diag::UnusedPackageParameter, slang::DiagnosticSeverity::Ignored);

    // ---- A partial design: one buffer, its dependencies absent. ----
    //
    // With CompilationFlags::IgnoreUnknownModules the instances of absent
    // modules survive as UninstantiatedDefSymbol, and slang binds their port
    // connections as ordinary expressions. So a net wired to a missing
    // submodule reads as USED -- which is sound, the connection really is a use
    // -- but never as DRIVEN, because nothing tells slang which of those ports
    // is an output.
    //
    // That splits the unused family cleanly in two, and only one half survives.
    // Measured on hazard3_core.v opened alone: 98 undriven-net and 10
    // undriven-port, every one of them an artifact of the other 20 files not
    // being open, against zero false unused-net. Reporting them would be the
    // server inventing 108 problems; the cost of silencing them is a genuinely
    // undriven net in a self-contained file going unreported, which is the
    // right direction to err (§7 of docs/design/lsp.md).
    if (partial) {
        engine.setSeverity(slang::diag::UndrivenNet, slang::DiagnosticSeverity::Ignored);
        engine.setSeverity(slang::diag::UndrivenPort, slang::DiagnosticSeverity::Ignored);
        engine.setSeverity(slang::diag::UnassignedVariable, slang::DiagnosticSeverity::Ignored);
    }

    // ---- Policy: what the chosen semantics judges differently. ----

    switch (policy) {
        case DiagnosticPolicy::Sim:
            // LRM 9.2.2.2 / 9.2.2.3 recommend tools warn when a comb / latch
            // procedure's behavior does not match its declared kind. slang keeps
            // both in its off-by-default "extra" group, so enable them -- as
            // warnings, because an inferred latch simulates exactly as written.
            engine.setSeverity(slang::diag::InferredLatch, slang::DiagnosticSeverity::Warning);
            engine.setSeverity(slang::diag::InferredComb, slang::DiagnosticSeverity::Warning);
            break;
    }
}

// Why a signal's type has no VCD representation, or nullptr when it has one.
//
// VCD's $var forms carry a scalar, a vector, a `real` and an `event` (LRM 21.7.2)
// -- everything with a fixed-width value image. A string, a heap container
// (queue / dynamic array / associative array), an unpacked struct/union and a
// class handle have none: `layout()` reports width 0 for all but the handle (64,
// the pointer), so a $var written from the layout alone declares a 1-bit signal
// aliased to the container's ring pointer, dumps a heap address, or -- for an
// aggregate, whose storage is a recursive mirror with no leaf slot type -- reads a
// null slot pointer and crashes. All three were silent before this check.
//
// TODO: an unpacked struct / array of structs could dump as one $var per leaf.
// A container has no fixed-width form at all: its size changes during the run,
// which VCD cannot express.
const char* vcd_unrepresentable(const ir::InternPool& pool, ir::Ref type) {
    ir::Ref leaf = type;
    while (std::get<ir::Type>(pool.get(leaf)).kind == ir::Type::Kind::UnpackedArray) {
        leaf = std::get<ir::Type>(pool.get(leaf)).element;
    }
    const ir::Type& t = std::get<ir::Type>(pool.get(leaf));
    switch (t.kind) {
        case ir::Type::Kind::String:
            return "string";
        case ir::Type::Kind::Queue:
            return "queue";
        case ir::Type::Kind::DynArray:
            return "dynamic array";
        case ir::Type::Kind::Assoc:
            return "associative array";
        case ir::Type::Kind::Class:
            return "class handle";
        case ir::Type::Kind::Struct:
            return t.is_packed ? nullptr : "unpacked struct";
        default:
            return nullptr;
    }
}

// Refuse a whole-design $dumpvars that would have to dump a signal with no VCD
// form. Walks exactly the set the backend registers (each top's scope tree, in
// order), so it neither misses a dumped signal nor rejects one that is never
// dumped -- a package-scope variable belongs to no instance scope, for instance.
//
// A compiler-synthesized signal (SourceLoc{0}: a gate input net, a materialized
// container temp) is skipped rather than reported: it is not a declared variable,
// the backend skips it too, and naming it in a diagnostic would point at no
// source.
void check_vcd_scope(const ir::Module& m, const ir::InstanceScope& sc, std::vector<Diagnostic>& out) {
    for (const ir::Signal sig : sc.signals) {
        const ir::SignalInfo& info = m.signal(sig);
        if (info.name.empty() || info.is_event || info.loc == ir::SourceLoc{ 0 }) {
            continue;
        }
        const char* what = vcd_unrepresentable(m.pool, info.type);
        if (what == nullptr) {
            continue;
        }
        out.push_back(
          Diagnostic{ .severity = Severity::Error,
                      .origin   = DiagnosticOrigin::Unsupported,
                      .code     = "unsupported-construct",
                      .message  = "$dumpvars cannot dump '" + info.name + "': " + what + " has no VCD representation",
                      .span     = ir::SourceSpan{ info.loc, info.loc } });
    }
    for (const ir::InstanceScope& child : sc.children) {
        check_vcd_scope(m, child, out);
    }
}

// LRM 13.4.4: a `fork … join_none` inside a FUNCTION schedules processes that can
// only become active after that function has returned, and the LRM allows the call
// "provided that the thread calling the function is created by an initial
// procedure, always procedure, or fork block from one of those procedures and in a
// context in which a side effect is allowed", requiring an error at compile or run
// time otherwise. Its own example is the illegal shape: a static-variable
// initializer calling such a function, with the same verdict for a continuous
// assignment.
//
// The offending pair is (spawning body, calling context) and neither half can see
// the other -- a function is lowered once and knows nothing of its callers -- so it
// is answered here, once, over the whole lowered call graph. A fork in a PROCESS
// body needs no check: slang refuses one in `always_comb` / `always_latch` and
// there is no way to write one in a continuous assignment or in a
// compiler-synthesized body.
//
// "Spawning" covers tasks too, not just functions: a `final` block may enable a
// task, and a `join_none` inside one has children the drain will never reach.
struct SpawnGraph {
    // Whether this body contains a fork or calls something that does.
    std::vector<uint8_t> spawns;
    // The fork site the body reaches, for the report.
    std::vector<ir::SourceLoc> site;

    [[nodiscard]] bool any() const {
        return std::ranges::any_of(spawns, [](uint8_t s) { return s != 0; });
    }
};

// Every function this body calls or enables. Fork children are deliberately NOT
// followed: the body that forks is already marked by the Fork itself, and a child
// is a thread of its own whose legality follows its parent's.
void spawn_callees(const ir::Ir& ir, std::vector<ir::FunctionId>& out) {
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        const auto                i = static_cast<ir::Index>(k);
        const ir::Inst::Payload& p = ir.payload(i);
        switch (ir.tag(i)) {
            case ir::Inst::Tag::Call:
                out.push_back(ir.extra_data<ir::CallInfo>(p.extra).func);
                break;
            case ir::Inst::Tag::TaskCall:
                out.push_back(ir.extra_data<ir::TaskCallInfo>(p.extra).func);
                break;
            case ir::Inst::Tag::CallVirtual:
            case ir::Inst::Tag::TaskCallVirtual:
                // The statically named target only -- the same bound the backend's
                // own reachability walks document.
                out.push_back(ir.extra_data<ir::VirtualCallInfo>(p.extra).func);
                break;
            default:
                break;
        }
    }
}

SpawnGraph build_spawn_graph(const ir::Module& m) {
    const size_t nfn = m.function_count();
    SpawnGraph   g{ std::vector<uint8_t>(nfn, 0), std::vector<ir::SourceLoc>(nfn, ir::SourceLoc{ 0 }) };
    for (uint32_t f = 0; f < nfn; ++f) {
        const ir::Ir& ir = m.function(static_cast<ir::FunctionId>(f)).ir;
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto i = static_cast<ir::Index>(k);
            if (ir.tag(i) == ir::Inst::Tag::Fork) {
                g.spawns[f] = 1;
                g.site[f]   = ir.loc(i);
                break;
            }
        }
    }
    if (!g.any()) {
        return g; // No fork anywhere: the common case pays one scan and stops.
    }
    // Relax over the call edges. A DAG converges in at most as many rounds as it
    // has functions; a cycle (recursion) needs no more, since the flag only ever
    // goes from 0 to 1.
    std::vector<ir::FunctionId> callees;
    bool                        changed = true;
    while (changed) {
        changed = false;
        for (uint32_t f = 0; f < nfn; ++f) {
            if (g.spawns[f] != 0) {
                continue;
            }
            callees.clear();
            spawn_callees(m.function(static_cast<ir::FunctionId>(f)).ir, callees);
            for (const ir::FunctionId c : callees) {
                const auto ci = static_cast<uint32_t>(c);
                if (ci < nfn && g.spawns[ci] != 0) {
                    g.spawns[f] = 1;
                    g.site[f]   = g.site[ci];
                    changed     = true;
                    break;
                }
            }
        }
    }
    return g;
}

// The first fork site `ir` reaches, directly or through a call, or SourceLoc{0}.
ir::SourceLoc spawn_site_of(const SpawnGraph& g, const ir::Ir& ir) {
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        const auto i = static_cast<ir::Index>(k);
        if (ir.tag(i) == ir::Inst::Tag::Fork) {
            return ir.loc(i);
        }
    }
    std::vector<ir::FunctionId> callees;
    spawn_callees(ir, callees);
    for (const ir::FunctionId c : callees) {
        const auto ci = static_cast<uint32_t>(c);
        if (ci < g.spawns.size() && g.spawns[ci] != 0) {
            return g.site[ci];
        }
    }
    return ir::SourceLoc{ 0 };
}

// Why this process is not a thread LRM 13.4.4 lets spawn, or null when it is one.
const char* non_spawn_root(const ir::Process& p) {
    if (p.spawn_root) {
        return nullptr;
    }
    switch (p.activation) {
        case ir::Activation::PreInit:
            return "a static variable initializer";
        case ir::Activation::Preponed:
            return "an assertion sampling process";
        case ir::Activation::Observed:
            return "a property evaluation process";
        case ir::Activation::ReNba:
            return "a clocking block drive";
        case ir::Activation::Final:
            return "a `final` block";
        default:
            // Every remaining shape is a continuous driver: an `assign`, a
            // net-declaration assignment, a gate, a port copy, a wire resolver.
            return "a continuous assignment";
    }
}

void report_bad_spawn(std::string_view where, ir::SourceLoc site, ir::SourceSpan root,
                      std::vector<Diagnostic>& out) {
    Diagnostic d{ .severity = Severity::Error,
                  .origin   = DiagnosticOrigin::Rejected,
                  .code     = "illegal-construct",
                  .message  = "a function that spawns a background process (fork..join_none, LRM "
                              "13.4.4) is called from " +
                              std::string(where) +
                              ", which is not a thread of an initial or always procedure: the "
                              "spawned process could not become active",
                  .span     = ir::SourceSpan{ site, site } };
    if (root.valid()) {
        d.notes.push_back(Diagnostic{ .severity = Severity::Note,
                                      .origin   = DiagnosticOrigin::Rejected,
                                      .code     = "illegal-construct",
                                      .message  = "called from here",
                                      .span     = root });
    }
    out.push_back(std::move(d));
}

// Report every call of a spawning body from a context LRM 13.4.4 does not allow.
void check_spawn_roots(const ir::Module& m, std::vector<Diagnostic>& out) {
    const SpawnGraph g = build_spawn_graph(m);
    if (!g.any()) {
        return;
    }
    for (const ir::Process& p : m.processes()) {
        const char* where = non_spawn_root(p);
        if (where == nullptr) {
            continue;
        }
        const ir::SourceLoc site = spawn_site_of(g, p.ir);
        if (site != ir::SourceLoc{ 0 }) {
            report_bad_spawn(where, site, p.span, out);
        }
    }
    // Bodies that are not processes and have no calling thread at all. A $strobe /
    // $monitor body runs in Postponed, where LRM 4.4.2.9 forbids a value change; a
    // DPI export is entered from native C. An action block (ReactivePend) is NOT
    // here: it is reactive code of the assertion's own thread and may spawn.
    const auto deferred_bodies = [&](const ir::Ir& ir) {
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto  i   = static_cast<ir::Index>(k);
            const auto  tag = ir.tag(i);
            if (tag != ir::Inst::Tag::StrobePend && tag != ir::Inst::Tag::MonitorSet) {
                continue;
            }
            const auto f = static_cast<uint32_t>(
              ir.extra_data<ir::StrobePendInfo>(ir.payload(i).extra).func);
            if (f < g.spawns.size() && g.spawns[f] != 0) {
                report_bad_spawn("a $strobe / $monitor body, which runs in the Postponed region",
                                 g.site[f], ir::SourceSpan{ ir.loc(i), ir.loc(i) }, out);
            }
        }
    };
    for (const ir::Process& p : m.processes()) {
        deferred_bodies(p.ir);
    }
    for (uint32_t f = 0; f < m.function_count(); ++f) {
        deferred_bodies(m.function(static_cast<ir::FunctionId>(f)).ir);
    }
    for (const ir::DpiExport& ex : m.dpi_exports) {
        const auto f = static_cast<uint32_t>(ex.func);
        if (f < g.spawns.size() && g.spawns[f] != 0) {
            report_bad_spawn("a DPI export, entered from native C with no calling thread", g.site[f],
                             m.function(ex.func).span, out);
        }
    }
}

// The body of lower_compilation. `diag_engine` is null exactly when the
// compilation has no source manager. Otherwise `client` is attached to it and
// renders each slang item.
LowerResult lower_design(slang::ast::Compilation&                            comp,
                         std::shared_ptr<SourceContext>                      ctx,
                         slang::DiagnosticEngine*                            diag_engine,
                         const std::shared_ptr<slang::TextDiagnosticClient>& client,
                         Timings*                                            timings,
                         ir::SystemAccess                                    allow_system,
                         bool                                                partial,
                         bool                                                allow_port_coercion) {
    // (allow_port_coercion reaches each Lowerer below.)
    LowerResult result;
    // Attached first so EVERY return path below carries it: `sources` is documented
    // as never null, and there are a dozen early returns.
    if (ctx->empty()) {
        ctx->borrow_source_manager(comp.getSourceManager());
    }
    result.sources = std::move(ctx);

    // Front end: getAllDiagnostics forces full elaboration; getRoot resolves the
    // design hierarchy. Both are slang work.
    const auto                  t_fe      = std::chrono::steady_clock::now();
    const slang::Diagnostics&   diags     = comp.getAllDiagnostics();
    bool                        has_error = false;
    // The analysis pass runs inside the diagnostics block below, but one thing it
    // produces -- the LRM 16.14.6 clock inference verdicts -- is read much later,
    // by the assertion lowering, so the table it fills is declared out here. The
    // AnalysisManager itself is NOT: the table holds only syntax nodes, which the
    // SyntaxTree owns, so nothing in it outlives the manager's arena and the
    // analysis results are still freed as soon as their diagnostics are harvested.
    InferredClockMap inferred_clocks;
    // One id per class for the whole design, shared by every Lowerer below (see
    // ClassIds).
    ClassIds class_ids;
    // Design-wide interface-class registry (LRM 8.26), shared for the same reason:
    // one interface must get one bitmap bit and one interface method name one table
    // index across the whole design, whichever top's Lowerer sees it first.
    IfaceIds iface_ids;
    // Design-wide virtual interface registry, shared by every Lowerer below.
    VifDefs vif_defs;
    if (diag_engine != nullptr) {
        slang::DiagnosticEngine& engine = *diag_engine;
        apply_severity_policy(engine, DiagnosticPolicy::Sim, partial);
        // slang's "$cast will always fail" warning is WRONG in the presence of
        // interface classes. Its condition is "both are class types and neither is
        // assignment compatible with the other" (SystemTasks.cpp:622-625), which is
        // exactly LRM 8.26.5's legal diagonal cast between two interface handles that
        // a common implementer satisfies -- `$cast(get_ref, put_ref)` in the LRM's own
        // example, which clotho lowers to the run-time implements test. Reporting it
        // would tell the user to delete correct code. Dropped when either operand type
        // is an interface class; every other DynamicCastConst is still reported.
        const auto wrong_iface_cast_warning = [](const slang::Diagnostic& d) {
            if (d.code != slang::diag::DynamicCastConst) {
                return false;
            }
            for (const slang::Diagnostic::Arg& a : d.args) {
                const auto* custom = std::get_if<slang::Diagnostic::CustomArgType>(&a);
                if (custom == nullptr) {
                    continue;
                }
                const auto* const* ty = std::any_cast<const slang::ast::Type*>(&custom->second);
                if (ty == nullptr) {
                    continue;
                }
                const slang::ast::Type& canon = (*ty)->getCanonicalType();
                if (canon.isClass() && canon.as<slang::ast::ClassType>().isInterface) {
                    return true;
                }
            }
            return false;
        };
        // `--top` naming something that is not a module makes slang also report
        // that the design has no top at all. That is the same mistake said twice,
        // and the second sentence only buries the one that names the culprit.
        const bool invalid_top = std::ranges::any_of(
          diags, [](const slang::Diagnostic& d) { return d.code == slang::diag::InvalidTopModule; });
        // Issue each diagnostic on its own so the text client's buffer holds
        // exactly one item's pretty rendering (snippet + caret) when we harvest it.
        // A diagnostic mapped to Ignored produces no report, hence the empty check.
        const auto harvest = [&](const slang::Diagnostic& d) {
            if (wrong_iface_cast_warning(d)) {
                return;
            }
            if (invalid_top && d.code == slang::diag::NoTopModules) {
                return;
            }
            client->clear();
            engine.issue(d);
            if (client->empty()) {
                return;
            }
            Diagnostic out = from_slang(engine, d);
            out.rendered   = client->getString();
            result.diagnostics.push_back(std::move(out));
        };
        for (const slang::Diagnostic& d : diags) {
            harvest(d);
        }
        // Post-elaboration analysis (dataflow): inferred-latch/comb checks,
        // blocking-task-in-comb errors, and multi-driver checks (9.2.2.2). Only
        // run on an error-free design — analysis assumes a valid AST.
        //
        // CheckUnused / CheckShadow are on unconditionally, as warnings. They are
        // deliberately NOT gated on the command or the severity policy: an unused
        // signal is unused whether you are simulating or synthesising, so it is
        // not a semantic question, and a finding that only appears under `lint`
        // is a finding most runs never see. Both are cheap -- the analysis walk
        // already happens for the dataflow checks above. The unused-PARAMETER
        // pair is silenced in apply_severity_policy; see the reason there.
        if (engine.getNumErrors() == 0) {
            comp.freeze();
            slang::analysis::AnalysisOptions aopts;
            aopts.flags = slang::analysis::AnalysisFlags::CheckUnused | slang::analysis::AnalysisFlags::CheckShadow;
            slang::analysis::AnalysisManager analysis(aopts);
            // Clock inference (LRM 16.14.6) runs as a side effect of analysing
            // each assertion, and its result is reachable only through the
            // AnalyzedAssertion slang builds for it -- so it is captured as the
            // pass runs rather than queried afterwards, which would mean
            // rediscovering each assertion's containing symbol to key the lookup.
            // A listener may be called concurrently, but only when the manager was
            // given a thread pool; this one was not, so every callback lands on
            // this thread and the map needs no lock.
            //
            // Both sides are recorded as syntax, and the manager's own AST is not
            // retained: see InferredClockMap for why an AST-keyed table would be
            // both incomplete and wrong across duplicated instance bodies. Two
            // elaborations of one source that disagree poison the entry (null), so
            // the ambiguity surfaces as a refusal instead of a coin flip.
            analysis.addListener([&](const slang::analysis::AnalyzedAssertion& aa) {
                const auto* stmt = std::get_if<const ConcurrentAssertionStatement*>(&aa.astNode);
                if (stmt == nullptr || (*stmt)->syntax == nullptr || aa.procedure == nullptr) {
                    return;
                }
                const TimingControl* clk = aa.procedure->getInferredClock();
                if (clk == nullptr || clk->syntax == nullptr) {
                    return;
                }
                const auto [slot, fresh] = inferred_clocks.try_emplace((*stmt)->syntax, clk->syntax);
                if (!fresh && slot->second != clk->syntax) {
                    slot->second = nullptr;
                }
            });
            analysis.analyze(comp);
            for (const slang::Diagnostic& d : analysis.getDiagnostics()) {
                // LRM 23.3.3.1 port coercion: slang says "coerced to 'inout'", which Clotho does
                // not do, and flags every net with a port driver beside another driver.
                // Lowerer::needs_port_coercion decides, and says it in its own words.
                if (d.code == slang::diag::InputPortCoercion || d.code == slang::diag::OutputPortCoercion) {
                    continue;
                }
                harvest(d);
            }
            comp.unfreeze();
        }
        has_error = engine.getNumErrors() > 0;
    }
    if (has_error) {
        if (timings != nullptr) {
            timings->frontend_ms += ms_since(t_fe);
        }
        return result;
    }

    const RootSymbol& root = comp.getRoot();
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_fe);
    }

    // A compilation unit with no top-level module/interface/program instance
    // (e.g. a package-only file) is not an elaboration error -- LRM
    // elaboration (§23.3) resolves whatever top instances exist; nothing
    // obligates a compilation unit to declare any. It simply has nothing to
    // simulate: globals (package / $unit-scope declarations) still lower
    // below so --emit=ir can show them and package-scope errors are still
    // caught, but the loop over root.topInstances runs zero times and the
    // resulting module carries no processes or per-instance signals.

    // Analysis: walk the elaborated AST into clotho IR. The wheel tick is the
    // design's smallest time unit; if no design unit set a timescale, time stays
    // unitless (one tick == one time unit) and nothing is scaled.
    //
    // Multiple top-level modules are each implicitly instantiated once and
    // elaborated into the one flat design (LRM 3.11, 23.3.1): independent name
    // hierarchies sharing one global timeline, with no port connections between
    // them. They flatten into a single Module — signals share the state buffer
    // but never connect. Precision/unit are the minimum across all tops (the
    // global time precision, LRM 3.14.2), so the wheel tick is computed over the
    // whole set, not a single root.
    const auto t_an         = std::chrono::steady_clock::now();
    auto       module       = std::make_unique<ir::Module>();
    bool       any_explicit = false;
    uint64_t   prec_fs      = 0;
    uint64_t   min_prec_fs  = 0;
    for (const InstanceSymbol* top : root.topInstances) {
        const uint64_t p = design_tick_fs(*top, any_explicit);
        if (p != 0 && (prec_fs == 0 || p < prec_fs)) {
            prec_fs = p;
        }
        const uint64_t mp = design_min_precision_fs(*top);
        if (mp != 0 && (min_prec_fs == 0 || mp < min_prec_fs)) {
            min_prec_fs = mp;
        }
    }
    // Packages and `$unit` sit outside the instance tree but declare subroutines
    // the design calls, and those bodies scale by their OWN timescale, so their
    // units join the GCD too (global_scope_tick_fs).
    const auto fold_global = [&](const Scope& scope) {
        const uint64_t p = global_scope_tick_fs(scope, any_explicit);
        if (p != 0 && (prec_fs == 0 || p < prec_fs)) {
            prec_fs = p;
        }
        if (const auto ts = scope.getTimeScale(); ts && scope_declares_subroutine(scope)) {
            const uint64_t mp = tsv_to_fs(ts->precision);
            if (min_prec_fs == 0 || mp < min_prec_fs) {
                min_prec_fs = mp;
            }
        }
    };
    for (const PackageSymbol* pkg : comp.getPackages()) {
        fold_global(*pkg);
    }
    for (const CompilationUnitSymbol* unit : root.compilationUnits) {
        fold_global(*unit);
    }
    const uint64_t tick_fs = any_explicit ? prec_fs : 0;

    // %t needs the real tick size (fs). In unitless mode (no `timescale anywhere)
    // a tick is one second, matching the seconds base the LRM and iverilog use
    // when no scale is set. The default %t time unit is the smallest design
    // precision (LRM Table 20-3), falling back to the tick when unitless.
    constexpr uint64_t fs_per_second = 1'000'000'000'000'000ULL;
    module->tick_fs                  = prec_fs == 0 ? fs_per_second : prec_fs;
    module->timeformat_units         = fs_to_unit_exponent(min_prec_fs == 0 ? module->tick_fs : min_prec_fs);

    // A single top keeps the bare (prefix-less) signal paths the rest of the
    // pipeline expects. With several tops, each top's instance name (== its
    // module name, LRM 23.3.1) prefixes its flattened paths so they stay unique.
    // Each top gets a fresh Lowerer: their bodies share no symbols (no inter-top
    // ports), so per-Lowerer instance/hierarchy state needs not — and must not —
    // be shared. Package / $unit-scope variables are the one exception (LRM 6.8:
    // no instance multiplicity, so one signal must serve every top that
    // references it): a dedicated Lowerer registers them once, up front, and its
    // symbol->signal entries seed every top's own map below.
    const bool single = root.topInstances.size() == 1;
    if (!root.topInstances.empty()) {
        module->top_name = std::string(root.topInstances[0]->name); // DPI svScope name
        if (!single) {
            module->name = std::string(root.topInstances[0]->name);
        }
    }
    bool poisoned  = false;
    bool dumps_vcd = false;
    // Design-wide: can any event variable be retargeted (LRM 15.5.5)? If not, every
    // event names one object for the run and a sensitivity list may be hoisted to a
    // static subscription, which is the fast path. If so, event sensitivity has to
    // stay in the body so each pass re-reads the handle -- a static subscription is
    // issued once from the seed loop and can never move. A `ref` / `output` event
    // formal counts: the callee can retarget the caller's variable.
    struct EventMergeScan : slang::ast::ASTVisitor<EventMergeScan, slang::ast::VisitFlags::AllGood> {
        bool found = false;
        void handle(const slang::ast::AssignmentExpression& a) {
            found = found || a.left().type->isEvent();
            visitDefault(a);
        }
        void handle(const slang::ast::FormalArgumentSymbol& fa) {
            found = found || (fa.direction != slang::ast::ArgumentDirection::In &&
                              fa.getType().getCanonicalType().isEvent());
            visitDefault(fa);
        }
    };
    EventMergeScan merge_scan;
    root.visit(merge_scan);
    const bool events_merge = merge_scan.found;
    // Design-wide: which class properties does some `@` or `wait` in the design wait
    // on, and with which edges (LRM 9.4.2 / 9.4.3)? Each one's object body carries a
    // hidden change-notifier event per waited edge, and the answer has to be complete
    // BEFORE the first class is laid out -- lower_class_type runs on demand from the
    // first reference anywhere under any top, which can precede the `@` that needs
    // the notifier. A design that waits on no property gets an empty table and every
    // class body, every store and every descriptor stays byte-identical.
    WaitedProps  waited_props;
    WaitPropScan wait_prop_scan;
    wait_prop_scan.out = &waited_props;
    root.visit(wait_prop_scan);
    try {
        Lowerer globals(*module, *result.sources, tick_fs, allow_system);
        globals.set_events_merge(events_merge);
        globals.set_class_ids(&class_ids);
        globals.set_iface_ids(&iface_ids);
        globals.set_vif_defs(&vif_defs);
        globals.set_waited_props(&waited_props);
        globals.lower_globals(comp, root);

        for (const InstanceSymbol* top : root.topInstances) {
            const std::string prefix = single ? std::string() : std::string(top->name) + ".";
            Lowerer           lw(*module, *result.sources, tick_fs, allow_system);
            lw.set_allow_port_coercion(allow_port_coercion);
            lw.seed_signals(globals.sigmap());
            lw.set_globals(&globals);
            lw.set_class_ids(&class_ids);
            lw.set_iface_ids(&iface_ids);
            lw.set_vif_defs(&vif_defs);
            lw.set_events_merge(events_merge);
            lw.set_waited_props(&waited_props);
            lw.set_inferred_clocks(&inferred_clocks);
            lw.lower_top(*top, prefix);
            // Exported functions resolve in the scope that owns them: this top's
            // module-scope functions through `lw` (its own lowered bodies address its
            // own instance state), $unit/package ones through the shared `globals`
            // delegation in function_id — those have no instance multiplicity, so
            // only the first top admits them.
            lw.collect_dpi_exports(comp, top, /*collect_globals=*/top == root.topInstances.front());
            const std::span<const Diagnostic> recovered = lw.recovered_diags();
            result.diagnostics.insert(result.diagnostics.end(), recovered.begin(), recovered.end());
            poisoned  = poisoned || lw.poisoned();
            dumps_vcd = dumps_vcd || lw.dumps_vcd();
        }
        if (root.topInstances.empty()) {
            // No top module at all (an interface-only file bound into a package,
            // e.g. uvm-examples/apb_protocol_monitor): the loop above never runs, so
            // nothing would otherwise collect the design's $unit/package DPI exports
            // -- collect_dpi_exports's own "only the first top admits globals" rule
            // has no top to piggyback on. globals.function_id needs no delegation
            // here (globals_ is never set on the globals Lowerer itself), so this
            // reaches build_reserved_body directly, exactly as the per-top delegation
            // path above does.
            globals.collect_dpi_exports(comp, nullptr, /*collect_globals=*/true);
        }
        // Any package/$unit class-method body still queued (a design with no top
        // instance, or a body the last top's own drain queued) builds now, and the
        // stub warnings it may recover with land before the diagnostics are
        // collected just below.
        globals.finalize_globals();
        const std::span<const Diagnostic> global_diags = globals.recovered_diags();
        result.diagnostics.insert(result.diagnostics.end(), global_diags.begin(), global_diags.end());
        poisoned  = poisoned || globals.poisoned();
        dumps_vcd = dumps_vcd || globals.dumps_vcd();
        // Which interface instances a virtual interface handle can name: every
        // elaborated instance whose body has the same type (definition plus
        // parameter values, LRM 25.9) as some `virtual` declaration's placeholder.
        // The set is design-wide and only complete now, after every top: an
        // instance under one top can be handed to a class the next top constructs.
        for (const VifInstance& inst : vif_defs.insts) {
            const bool wanted = std::ranges::any_of(
              vif_defs.wanted, [&](const slang::ast::InstanceBodySymbol* w) {
                  return inst.body->hasSameType(*w);
              });
            if (wanted) {
                module->vif_scopes.push_back(inst.scope);
            }
        }
        // Interface classes (LRM 8.26): the two design-wide extents every
        // descriptor's interface regions are sized to. Only complete now, after
        // every top -- one top can declare an interface a class under the next one
        // implements -- and both stay zero for a design that declares none, which is
        // what keeps such a design's descriptors bit-identical (§9).
        module->iface_count      = static_cast<uint32_t>(iface_ids.ids.size());
        module->iface_slot_count = static_cast<uint32_t>(iface_ids.slots.size());
    } catch (const LowerError& e) {
        // A structural failure (an instance, generate block or instance array),
        // which the member walk deliberately does not recover from because
        // continuing would misalign the scope tree. No module is produced.
        result.diagnostics.push_back(Diagnostic{ .severity = Severity::Error,
                                                 .origin   = DiagnosticOrigin::Unsupported,
                                                 .code     = "unsupported-construct",
                                                 .message  = e.message,
                                                 .span     = e.span });
        return result;
    }
    // The dump has to represent every declared signal, and the answer needs the
    // whole design, so it is checked here rather than at the $dumpvars call.
    if (dumps_vcd) {
        const size_t before = result.diagnostics.size();
        for (const ir::InstanceScope& top : module->tops()) {
            check_vcd_scope(*module, top, result.diagnostics);
        }
        poisoned = poisoned || result.diagnostics.size() != before;
    }
    // Same reason, one level up: which callers may reach a function that spawns a
    // background process is a whole-call-graph question (LRM 13.4.4).
    {
        const size_t before = result.diagnostics.size();
        check_spawn_roots(*module, result.diagnostics);
        poisoned = poisoned || result.diagnostics.size() != before;
    }
    if (timings != nullptr) {
        timings->analysis_ms += ms_since(t_an);
    }

    // The module is handed back even when poisoned: it is missing the behaviour of
    // every member that failed, so it must never be simulated or emitted, but it
    // IS analysable and that is exactly what `clotho lint` consumes. `ok` is the
    // gate, and it is the only thing standing between a poisoned module and
    // codegen — see the `ok` checks in main() (src/main.cpp).
    result.module = std::move(module);
    result.ok     = !poisoned;

    return result;
}

// How the frontend's text client is configured for one run. A default style is
// what an editor or a test wants: plain text, every field on.
struct DiagStyle {
    // The client that renders every item; null makes a plain one. It must be
    // fresh: slang binds a client to one engine for good, and the Driver's own
    // client already belongs to the Driver's engine.
    std::shared_ptr<slang::TextDiagnosticClient> client;
    // Whether a Clotho item names its code, as slang names a warning's option
    // (`--diag-option`).
    bool show_code = true;
};

// Whether the Driver's text client colours what it formats. The decision is
// slang's (`--color-diagnostics`, else a terminal on stderr), made inside the
// Driver and held in a client that cannot be asked, so ask by formatting one
// item and looking for an escape.
bool driver_colours(slang::TextDiagnosticClient& driver_client) {
    const slang::Diagnostic   original(slang::DiagCode{}, slang::SourceLocation::NoLocation);
    slang::ReportedDiagnostic probe(original);
    probe.location         = slang::SourceLocation::NoLocation;
    probe.formattedMessage = "probe";
    probe.severity         = slang::DiagnosticSeverity::Error;
    driver_client.clear();
    driver_client.report(probe);
    const bool coloured = driver_client.getString().contains("\x1b[");
    driver_client.clear();

    return coloured;
}

// A client set up as the Driver's is: colour, then every `--diag-*` option, with
// slang's own defaults where the option is unset. Mirrors what
// slang::driver::Driver::processOptions applies to its client.
DiagStyle style_of(slang::driver::Driver& dr) {
    const slang::driver::Driver::Options& o = dr.options;

    DiagStyle style;
    style.client = std::make_shared<slang::TextDiagnosticClient>();
    slang::TextDiagnosticClient& c = *style.client;
    c.showColors(driver_colours(*dr.textDiagClient));
    c.showColumn(o.diagColumn.value_or(true));
    c.setColumnUnit(o.diagColumnUnit.value_or(slang::ColumnUnit::Display));
    c.showLocation(o.diagLocation.value_or(true));
    c.showSourceLine(o.diagSourceLine.value_or(true));
    c.showOptionName(o.diagOptionName.value_or(true));
    c.showIncludeStack(o.diagIncludeStack.value_or(true));
    c.showMacroExpansion(o.diagMacroExpansion.value_or(true));
    c.showAbsPaths(o.diagAbsPaths.value_or(false));
    c.showHierarchyInstance(o.diagHierarchy.value_or(slang::ShowHierarchyPathOption::Auto));
    style.show_code = o.diagOptionName.value_or(true);

    return style;
}

slang::DiagnosticSeverity to_slang_severity(Severity s) {
    switch (s) {
        case Severity::Note:
            return slang::DiagnosticSeverity::Note;
        case Severity::Warning:
            return slang::DiagnosticSeverity::Warning;
        case Severity::Error:
            return slang::DiagnosticSeverity::Error;
    }

    return slang::DiagnosticSeverity::Error;
}

// Render one Clotho-origin item, then its notes, through the same text client
// slang's own items use, so a refusal gets the same location, source line and
// caret, and the same colour and `--diag-*` settings, as a slang error.
void render_through_client(const Diagnostic&           d,
                           const slang::SourceManager& sm,
                           slang::TextDiagnosticClient& client,
                           bool                         show_code,
                           std::string&                 out) {
    std::string message = d.message;
    if (show_code && !d.code.empty()) {
        message += " [" + d.code + "]";
    }
    // A location inside a macro expansion has no line of its own; slang's text
    // client wants the spelling site, as its engine passes for its own items.
    slang::SourceLocation          at = slang::SourceLocation::NoLocation;
    std::vector<slang::SourceRange> ranges;
    if (d.span.valid()) {
        at = sm.getFullyOriginalLoc(to_slang_location(d.span.start));
        // The IR span's end is SourceLoc{0} for a bare point; a one-character
        // range would underline nothing the caret does not already show.
        if (d.span.end != ir::SourceLoc{ 0 } && d.span.end != d.span.start) {
            const slang::SourceLocation end = sm.getFullyOriginalLoc(to_slang_location(d.span.end));
            if (end.buffer() == at.buffer() && end.offset() > at.offset()) {
                ranges.emplace_back(at, end);
            }
        }
    }

    const slang::Diagnostic     original(slang::DiagCode{}, at);
    slang::ReportedDiagnostic   reported(original);
    reported.location         = at;
    reported.ranges           = ranges;
    reported.formattedMessage = message;
    reported.severity         = to_slang_severity(d.severity);
    client.clear();
    client.report(reported);
    out += client.getString();
    client.clear();

    for (const Diagnostic& note : d.notes) {
        render_through_client(note, sm, client, show_code, out);
    }
}

LowerResult lower_compilation(slang::ast::Compilation&       comp,
                              std::shared_ptr<SourceContext> ctx,
                              Timings*                       timings,
                              ir::SystemAccess               allow_system,
                              bool                           partial             = false,
                              DiagStyle                      style               = {},
                              bool                           allow_port_coercion = false) {
    const slang::SourceManager* const sm = comp.getSourceManager();
    if (sm == nullptr) {
        return lower_design(
          comp, std::move(ctx), nullptr, nullptr, timings, allow_system, partial, allow_port_coercion);
    }

    slang::DiagnosticEngine engine(*sm);
    if (style.client == nullptr) {
        style.client = std::make_shared<slang::TextDiagnosticClient>();
    }
    engine.addClient(style.client);
    LowerResult result =
      lower_design(comp, std::move(ctx), &engine, style.client, timings, allow_system, partial, allow_port_coercion);

    // slang's own items arrive rendered (lower_design harvests them one by one).
    // What is left is Clotho's: lowering guards, post-lowering checks and the
    // driver notices. Items the driver already printed are not rendered again.
    for (Diagnostic& d : result.diagnostics) {
        if (d.rendered.empty() && !d.reported) {
            render_through_client(d, *sm, *style.client, style.show_code, d.rendered);
        }
    }

    return result;
}

} // namespace

LowerResult lower_files(std::span<const std::string_view> paths,
                        Timings*                          timings,
                        ir::SystemAccess                  allow_system,
                        bool                              allow_port_coercion) {
    const auto t_parse = std::chrono::steady_clock::now();

    // One SourceManager shared across every tree so the Compilation's diagnostics
    // resolve file/line locations for all files. It is heap-allocated
    // and handed to the SourceContext so it OUTLIVES this call: an ir::SourceLoc is
    // only decodable against the manager that minted it, and this used to be a
    // stack local, which is why locations were unresolvable after lowering.
    auto                  ctx    = std::make_shared<SourceContext>();
    auto                  sm     = std::make_unique<slang::SourceManager>();
    slang::SourceManager& sm_ref = *sm;
    ctx->adopt_source_manager(std::move(sm));

    // `comp` stays on the stack: it holds the syntax trees, which reference the
    // manager, so it must die BEFORE the context that owns the manager. Returning
    // from this function destroys it first, which is the order we need.
    slang::ast::Compilation comp;
    for (const std::string_view path : paths) {
        auto tree = slang::syntax::SyntaxTree::fromFile(path, sm_ref);
        if (!tree) {
            LowerResult result;
            result.sources = ctx;
            result.diagnostics.push_back(
              Diagnostic{ .severity = Severity::Error,
                          .origin   = DiagnosticOrigin::Driver,
                          .code     = "cannot-read-source",
                          .message  = "cannot read '" + std::string(path) + "': " + std::string(tree.error().second) });
            return result;
        }
        comp.addSyntaxTree(*tree);
    }
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_parse);
    }

    return lower_compilation(comp, std::move(ctx), timings, allow_system, false, {}, allow_port_coercion);
}

LowerResult lower_driver(std::unique_ptr<slang::driver::Driver> driver,
                         Timings*                               timings,
                         ir::SystemAccess                       allow_system,
                         PathPrefixMap                          path_prefix_map,
                         bool                                   allow_port_coercion) {
    const auto t_parse = std::chrono::steady_clock::now();

    // The Driver owns source loading (incl. `-f` command files, `-y`/`-v`
    // libraries, globbing), the SourceManager, and the option Bag; we consume the
    // Compilation it produces and keep Clotho's own diagnostic rendering in
    // lower_compilation. Both the Driver (which owns the
    // SourceManager) and the Compilation are handed to the SourceContext so
    // locations stay resolvable after this call returns.
    auto ctx = std::make_shared<SourceContext>();
    ctx->set_path_prefix_map(std::move(path_prefix_map));
    slang::driver::Driver& dr  = *driver;

    LowerResult result;
    if (!dr.processOptions()) {
        // The Driver printed the specific option / missing-file errors itself, so
        // this notice carries the failure without saying it a second time.
        ctx->adopt_driver(std::move(driver));
        result.sources = ctx;
        result.diagnostics.push_back(Diagnostic{ .severity = Severity::Error,
                                                 .origin   = DiagnosticOrigin::Driver,
                                                 .code     = "bad-command-line",
                                                 .message  = "command-line or source-loading error",
                                                 .reported = true });
        return result;
    }

    // Merge every source file into one compilation unit so `` `define `` macros
    // and `$unit` declarations are shared across files (LRM 3.12). Set before
    // parseAllSources, which is where the SourceLoader consumes it.
    dr.options.singleUnit = true;
    if (!dr.parseAllSources()) {
        // Likewise: parseAllSources prints each load error before it fails.
        ctx->adopt_driver(std::move(driver));
        result.sources = ctx;
        result.diagnostics.push_back(Diagnostic{ .severity = Severity::Error,
                                                 .origin   = DiagnosticOrigin::Driver,
                                                 .code     = "parse-failed",
                                                 .message  = "failed to parse one or more source files",
                                                 .reported = true });
        return result;
    }

    std::unique_ptr<slang::ast::Compilation> comp = dr.createCompilation();
    slang::ast::Compilation&                 cr   = *comp;
    // Order matters: the context destroys the Compilation before the Driver, and
    // the Driver (hence its SourceManager) last. adopt_driver also points the
    // context's resolver at the Driver's manager.
    ctx->adopt_driver(std::move(driver));
    ctx->adopt_compilation(std::move(comp));
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_parse);
    }

    // The design's diagnostics are rendered the way the Driver renders its own, so
    // colour and `--diag-source`, `--diag-column`, `--diag-location`, ... apply to
    // them alike.
    return lower_compilation(
      cr, std::move(ctx), timings, allow_system, /*partial=*/false, style_of(dr), allow_port_coercion);
}

LowerResult lower_sources(std::span<const std::string_view> args,
                          Timings*                          timings,
                          ir::SystemAccess                  allow_system,
                          bool                              allow_port_coercion) {
    auto                   driver = std::make_unique<slang::driver::Driver>();
    slang::driver::Driver& dr     = *driver;
    dr.addStandardArgs();
    // The Driver colours when stderr is a terminal. This entry point takes plain
    // argument lists (tests, tools) and must give plain text unless the list asks
    // for colour with `--color-diagnostics`, which parseCommandLine applies after.
    dr.textDiagClient->showColors(false);

    // Driver's parseCommandLine treats argv[0] as the program name and needs
    // null-terminated C strings; copy the views and prepend a placeholder.
    std::vector<std::string> owned(args.begin(), args.end());
    std::vector<const char*> argv;
    argv.reserve(owned.size() + 1);
    argv.push_back("clotho");
    for (const std::string& a : owned) {
        argv.push_back(a.c_str());
    }

    if (!dr.parseCommandLine(static_cast<int>(argv.size()), argv.data())) {
        // The Driver prints the specific CLI errors itself; report the failure the
        // same way lower_driver reports a bad option set.
        auto        ctx = std::make_shared<SourceContext>();
        LowerResult result;
        ctx->adopt_driver(std::move(driver));
        result.sources = ctx;
        result.diagnostics.push_back(Diagnostic{ .severity = Severity::Error,
                                                 .origin   = DiagnosticOrigin::Driver,
                                                 .code     = "bad-command-line",
                                                 .message  = "command-line or source-loading error",
                                                 .reported = true });
        return result;
    }
    return lower_driver(std::move(driver), timings, allow_system, {}, allow_port_coercion);
}

LowerResult lower_text(std::string_view source,
                       Timings*         timings,
                       ir::SystemAccess allow_system,
                       bool             allow_port_coercion) {
    const auto t_parse = std::chrono::steady_clock::now();

    // `SyntaxTree::fromText` uses slang's process-wide default SourceManager, so
    // there is nothing to own here — the context borrows it. The Compilation is
    // still adopted so its syntax trees outlive the call alongside it.
    auto                     ctx  = std::make_shared<SourceContext>();
    auto                     comp = std::make_unique<slang::ast::Compilation>();
    slang::ast::Compilation& cr   = *comp;
    cr.addSyntaxTree(slang::syntax::SyntaxTree::fromText(source));
    ctx->borrow_source_manager(cr.getSourceManager());
    ctx->adopt_compilation(std::move(comp));
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_parse);
    }

    return lower_compilation(cr, std::move(ctx), timings, allow_system, false, {}, allow_port_coercion);
}

LowerResult lower_buffer(std::string_view path,
                         std::string_view text,
                         Timings*         timings,
                         ir::SystemAccess allow_system) {
    const auto t_parse = std::chrono::steady_clock::now();

    // A manager per call, owned by the context. slang's SourceManager is
    // append-only -- assignBuffer throws on a path it has already seen and there
    // is no removeBuffer -- so a manager reused across revisions would either
    // throw on the second keystroke or grow without bound. Rebuilding it costs
    // the file cache, which is worth nothing here: there is exactly one buffer.
    auto  sm     = std::make_unique<slang::SourceManager>();
    auto  buffer = sm->assignText(path, text);
    auto  ctx    = std::make_shared<SourceContext>();
    auto& sm_ref = *sm;

    // IgnoreUnknownModules only. NOT CompilationFlags::LintMode, despite the
    // name and despite it looking like the flag for exactly this: LintMode
    // makes Scope::isUninstantiated() return true for EVERY scope
    // (slang Scope.cpp:150-153), and slang skips diagnostics in uninstantiated
    // code -- which silently deletes the whole unused/unassigned family, the
    // findings an editor most wants. Measured: with LintMode, `wire [2:0] foo`
    // assigned and never read reported nothing at all.
    slang::ast::CompilationOptions copts;
    copts.flags |= slang::ast::CompilationFlags::IgnoreUnknownModules;
    slang::Bag options;
    options.set(copts);

    auto                     comp = std::make_unique<slang::ast::Compilation>(options);
    slang::ast::Compilation& cr   = *comp;
    cr.addSyntaxTree(slang::syntax::SyntaxTree::fromBuffer(buffer, sm_ref, options));

    // Manager first, then the compilation: the context destroys them in reverse
    // declaration order, and the syntax trees point into the manager's buffers.
    ctx->adopt_source_manager(std::move(sm));
    ctx->adopt_compilation(std::move(comp));
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_parse);
    }

    return lower_compilation(cr, std::move(ctx), timings, allow_system, /*partial=*/true);
}

ir::LocResolver loc_resolver(const LowerResult& result) {
    return [&result](ir::SourceSpan span) {
        const SourcePosition pos = result.sources->resolve(span.start);
        // The underline needs an end on the SAME line to have a width; a span
        // that runs onto the next line degrades to a bare caret.
        const SourcePosition end = result.sources->resolve(span.end);
        return ir::SourceText{
            .where      = result.sources->render(span.start),
            .line       = result.sources->line_text(span.start),
            .line_no    = pos.line,
            .column     = pos.column,
            .end_column = end.valid && end.line == pos.line ? end.column : pos.column,
        };
    };
}

} // namespace clotho
