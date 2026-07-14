#include "Lower.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <slang/ast/ASTContext.h>
#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Compilation.h>
#include <slang/ast/EvalContext.h>
#include <slang/ast/Expression.h>
#include <slang/ast/SemanticFacts.h>
#include <slang/ast/Statement.h>
#include <slang/ast/TimingControl.h>
#include <slang/ast/expressions/AssignmentExpressions.h>
#include <slang/ast/expressions/CallExpression.h>
#include <slang/ast/expressions/ConversionExpression.h>
#include <slang/ast/expressions/LiteralExpressions.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/expressions/Operator.h>
#include <slang/ast/expressions/OperatorExpressions.h>
#include <slang/ast/expressions/SelectExpressions.h>
#include <slang/ast/statements/ConditionalStatements.h>
#include <slang/ast/statements/LoopStatements.h>
#include <slang/ast/statements/MiscStatements.h>
#include <slang/ast/symbols/BlockSymbols.h>
#include <slang/ast/symbols/ClassSymbols.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/MemberSymbols.h>
#include <slang/ast/symbols/ParameterSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/ast/symbols/SubroutineSymbols.h>
#include <slang/ast/symbols/ValueSymbol.h>
#include <slang/ast/symbols/VariableSymbols.h>
#include <slang/ast/types/AllTypes.h>
#include <slang/ast/types/NetType.h>
#include <slang/ast/types/Type.h>
#include <slang/ast/Scope.h>
#include <slang/analysis/AnalysisManager.h>
#include <slang/diagnostics/AnalysisDiags.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/LookupDiags.h>
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/driver/Driver.h>
#include <slang/numeric/Time.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <slang/util/SmallVector.h>

#include "SourceMap.h"
#include "ir/Builder.h"
#include "runtime/Format.h"

namespace clotho {

namespace {

using namespace slang::ast;

// Raised when lowering meets a construct outside the proof-of-concept's scope.
// Caught at the top level and turned into a diagnostic.
struct LowerError {
    std::string message;
};

// Resolve an interface modport-port proxy to the interface instance's own
// storage symbol. A modport (LRM 25.5) exposes each interface signal through a
// distinct ModportPortSymbol whose `internalSymbol` is the underlying interface
// VariableSymbol/NetSymbol; a plain interface-port (`m(bus w); w.sig`) or direct
// hierarchical reference to the same signal already binds to that underlying
// symbol. Keying every reference on the canonical symbol makes interface storage
// shared for free — a modport read, a plain-port read, and a direct hierarchical
// read all map to one ir::Signal, with no cross-boundary copy process. The loop
// handles a modport re-exported through a nested interface's modport.
const ValueSymbol& canonical_value_symbol(const ValueSymbol& sym) {
    const ValueSymbol* cur = &sym;
    while (cur->kind == SymbolKind::ModportPort) {
        const ModportPortSymbol& mp   = cur->as<ModportPortSymbol>();
        const ValueSymbol*       next = nullptr;
        if (mp.internalSymbol != nullptr) {
            next = mp.internalSymbol->as_if<ValueSymbol>();
        } else if (const Expression* conn = mp.getConnectionExpr()) {
            // A modport expression port (`.name(expr)`, LRM 25.5.3): the port
            // maps to an explicit expression, not a direct internal symbol. A
            // plain rename (`.clean(raw)`) wraps a single whole signal that
            // shares storage, so follow it; a structural expression
            // (part-select / concat) maps to a bit sub-range with no single
            // canonical symbol and is rejected loudly in signal_of.
            if (conn->kind == ExpressionKind::NamedValue ||
                conn->kind == ExpressionKind::HierarchicalValue) {
                next = &conn->as<ValueExpressionBase>().symbol;
            }
        }
        if (next == nullptr) {
            break;
        }
        cur = next;
    }
    return *cur;
}

// Femtoseconds in one unit of a time-scale value. Magnitude is 1/10/100 and the
// unit is a power-of-ten fraction of a second, so the result is a power of ten.
uint64_t tsv_to_fs(slang::TimeScaleValue v) {
    uint64_t fs_per_unit = 1;
    switch (v.unit) {
        case slang::TimeUnit::Femtoseconds:
            fs_per_unit = 1ULL;
            break;
        case slang::TimeUnit::Picoseconds:
            fs_per_unit = 1'000ULL;
            break;
        case slang::TimeUnit::Nanoseconds:
            fs_per_unit = 1'000'000ULL;
            break;
        case slang::TimeUnit::Microseconds:
            fs_per_unit = 1'000'000'000ULL;
            break;
        case slang::TimeUnit::Milliseconds:
            fs_per_unit = 1'000'000'000'000ULL;
            break;
        case slang::TimeUnit::Seconds:
            fs_per_unit = 1'000'000'000'000'000ULL;
            break;
    }

    return static_cast<uint64_t>(v.magnitude) * fs_per_unit;
}

// Wheel-tick size (fs) for an instance subtree: the smallest time *unit* across
// the design, plus whether any design unit set a timescale explicitly. Mirrors
// lower_instance's recursion over each scope's *effective* timescale (slang
// resolves inheritance / the default); the caller treats the result as unitless
// unless `any_explicit` is set, so a design with no directive keeps raw-tick time
// (LRM §3.14.2.3).
//
// The tick is the minimum unit, NOT the minimum precision: with only integer
// delays, every delay (constant or runtime `#expr`) is an exact multiple of its
// module's unit, so no event can land finer than the smallest unit. Precision
// matters only for rounding fractional (real) delays, which are unsupported.
// Choosing the unit keeps deadline magnitudes near the timing wheel's
// near-window (one revolution) instead of inflating them by unit/precision and
// spilling every timer into the O(log n) overflow heap. Since all timescale
// values are powers of ten, the minimum unit equals the GCD of all units, so it
// also exactly represents every event time.
// TODO: when `real` delays land, a fractional delay rounds to its module
// precision, so the tick must drop to the minimum precision (or the GCD that
// includes precisions) to stay exact.
uint64_t design_tick_fs(const InstanceSymbol& inst, bool& any_explicit);

// Smallest child time unit (fs) reachable from a scope, descending into nested
// instances and through generate blocks/arrays (whose instances share the
// enclosing instance's timescale but may instantiate further modules).
// Uninstantiated generate branches contribute nothing.
uint64_t scope_min_tick_fs(const Scope& scope, bool& any_explicit) {
    uint64_t   min_fs = 0;
    const auto consider = [&](uint64_t child) {
        if (child != 0 && (min_fs == 0 || child < min_fs)) {
            min_fs = child;
        }
    };
    for (const Symbol& m : scope.members()) {
        switch (m.kind) {
            case SymbolKind::Instance:
                consider(design_tick_fs(m.as<InstanceSymbol>(), any_explicit));
                break;
            case SymbolKind::GenerateBlock: {
                const auto& gb = m.as<GenerateBlockSymbol>();
                if (!gb.isUninstantiated) {
                    consider(scope_min_tick_fs(gb, any_explicit));
                }
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (const GenerateBlockSymbol* e : m.as<GenerateBlockArraySymbol>().entries) {
                    if (!e->isUninstantiated) {
                        consider(scope_min_tick_fs(*e, any_explicit));
                    }
                }
                break;
            default:
                break;
        }
    }

    return min_fs;
}

uint64_t design_tick_fs(const InstanceSymbol& inst, bool& any_explicit) {
    uint64_t min_fs = 0;
    if (inst.getDefinition().timeScale.has_value()) {
        any_explicit = true;
    }
    if (const auto ts = inst.body.getTimeScale()) {
        min_fs = tsv_to_fs(ts->base);
    }
    const uint64_t child = scope_min_tick_fs(inst.body, any_explicit);
    if (child != 0 && (min_fs == 0 || child < min_fs)) {
        min_fs = child;
    }

    return min_fs;
}

// Smallest time *precision* (fs) across the design's effective timescales. This
// is the default %t time unit (LRM Table 20-3), distinct from the wheel tick,
// which tracks the smallest *unit*. Returns 0 if no scope has a timescale.
uint64_t design_min_precision_fs(const InstanceSymbol& inst) {
    uint64_t min_fs = 0;
    if (const auto ts = inst.body.getTimeScale()) {
        min_fs = tsv_to_fs(ts->precision);
    }
    for (const Symbol& m : inst.body.members()) {
        if (m.kind == SymbolKind::Instance) {
            const uint64_t child = design_min_precision_fs(m.as<InstanceSymbol>());
            if (child != 0 && (min_fs == 0 || child < min_fs)) {
                min_fs = child;
            }
        }
    }

    return min_fs;
}

// Time-unit exponent (10^n seconds) of a power-of-ten femtosecond magnitude. A
// timescale value is always a power of ten, so the base-10 log is exact;
// fs = 10^(n+15) s, so the unit exponent is log10(fs) - 15.
int32_t fs_to_unit_exponent(uint64_t fs) {
    return static_cast<int32_t>(std::lround(std::log10(static_cast<double>(fs)))) - 15;
}

ir::SignalInfo::Direction direction_of(slang::ast::ArgumentDirection d) {
    switch (d) {
        case slang::ast::ArgumentDirection::In:
            return ir::SignalInfo::Direction::Input;
        case slang::ast::ArgumentDirection::Out:
            return ir::SignalInfo::Direction::Output;
        case slang::ast::ArgumentDirection::InOut:
            return ir::SignalInfo::Direction::Inout;
        case slang::ast::ArgumentDirection::Ref:
            return ir::SignalInfo::Direction::Internal;
    }

    return ir::SignalInfo::Direction::Internal;
}

// Which inferred-sensitivity rule set a SensitivityCollector walk applies. The
// LRM defines two that differ in how subroutine calls contribute (9.2.2.2.2):
//
// - Comb (`always_comb` / `always_latch`, 9.2.2.2.1): the bodies of called
//   functions contribute their reads and writes; the bodies of called tasks
//   contribute nothing (only the task's argument expressions do).
// - ImplicitEvent (`always @*`, 9.4.2.2, and continuous assigns / port copies):
//   no called subroutine body contributes anything; only the argument
//   expressions are sensitive. 9.2.2.2.2 makes the function-body contribution an
//   always_comb-only rule ("always_comb is sensitive to changes to variables
//   read within a function called within the procedure, whereas always @* is
//   sensitive to changes only to the arguments of such a function"); a
//   continuous assign responds to "an operand in the right-hand expression"
//   (10.3.2), and a function-internal read is not an operand of that expression.
enum class SensitivityMode : uint8_t {
    Comb,
    ImplicitEvent,
};

// Collects the reads and writes that build a process's inferred sensitivity
// (an `always_comb`, `always_latch`, `always @*`, or continuous assign): the
// signals the body reads to compute a result, and the ones it writes. Whether
// written signals are subtracted from the read set is decided at the use site:
// always_comb excludes them (9.2.2.2.1 (b)), while `always @*` keeps a signal
// that is read anywhere outside an lvalue base even if also written (9.4.2.2:
// only identifiers appearing *solely* as an assignment's left-hand side are
// excluded). Index/part-select expressions on a write target are themselves
// reads and are always kept (11.6, 9.2.2.2).
//
// The AST is walked rather than the lowered IR so reads are seen at the source
// level, before any temporaries the IR introduces. Symbols not in `sigmap`
// (parameters, genvars, other constants) carry no sensitivity and are skipped.
struct SensitivityCollector : ASTVisitor<SensitivityCollector, VisitFlags::AllGood> {
    using SigMap = ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal>;

    const SigMap&                          sigmap;
    SensitivityMode                        mode;
    std::vector<ir::Signal>                reads;    // in first-seen order
    ankerl::unordered_dense::set<uint32_t> seen;     // dedup for `reads`
    ankerl::unordered_dense::set<uint32_t> written;  // assignment targets / locals
    // User functions already descended into, so a function called from several
    // sites (or a recursive one) is walked at most once.
    ankerl::unordered_dense::set<const SubroutineSymbol*> entered;

    SensitivityCollector(const SigMap& m, SensitivityMode mode) : sigmap(m), mode(mode) {}

    void record_read(const ValueSymbol& sym) {
        const auto it = sigmap.find(&canonical_value_symbol(sym));
        if (it == sigmap.end()) {
            return;
        }
        if (seen.insert(static_cast<uint32_t>(it->second)).second) {
            reads.push_back(it->second);
        }
    }

    void mark_written(const ValueSymbol& sym) {
        const auto it = sigmap.find(&canonical_value_symbol(sym));
        if (it != sigmap.end()) {
            written.insert(static_cast<uint32_t>(it->second));
        }
    }

    // Record the write target(s) of an lvalue: the base name is written, but any
    // selector expression on it (an index or a non-constant part-select bound) is
    // a read and is descended into.
    void record_write(const Expression& lhs) {
        switch (lhs.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
                mark_written(lhs.as<ValueExpressionBase>().symbol);
                break;
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es = lhs.as<ElementSelectExpression>();
                record_write(es.value());
                es.selector().visit(*this);
                break;
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs = lhs.as<RangeSelectExpression>();
                record_write(rs.value());
                rs.left().visit(*this);
                rs.right().visit(*this);
                break;
            }
            case ExpressionKind::Concatenation:
                for (const Expression* op : lhs.as<ConcatenationExpression>().operands()) {
                    record_write(*op);
                }
                break;
            case ExpressionKind::Conversion:
                record_write(lhs.as<ConversionExpression>().operand());
                break;
            default:
                // An unrecognized lvalue shape: descend conservatively so any
                // reads inside it are still picked up.
                lhs.visit(*this);
                break;
        }
    }

    void handle(const NamedValueExpression& nv) { record_read(nv.symbol); }

    // A hierarchical read (`@* … = top.u1.sig`, LRM 23.6) makes the process
    // sensitive to the target instance's signal, same as a local read.
    void handle(const HierarchicalValueExpression& hv) { record_read(hv.symbol); }

    void handle(const AssignmentExpression& a) {
        a.right().visit(*this); // RHS: pure reads
        if (a.isCompound()) {
            // `a += b` reads its target too. In Comb mode the write below
            // subtracts it back out; in ImplicitEvent mode it stays sensitive.
            a.left().visit(*this);
        }
        record_write(a.left());
    }

    // Identifiers that appear only in a timing-control (wait / event / delay)
    // expression carry no sensitivity (9.2.2.2.1 (c), 9.4.2.2): skip the timing
    // control and walk only the controlled statement. Reachable only from
    // `always @*` (Comb procedures reject timing controls at elaboration).
    void handle(const TimedStatement& t) { t.stmt.visit(*this); }

    // A `fork` block's children are independent concurrent processes (LRM 9.3.2)
    // with their own sensitivity — their reads never wake the parent process, so
    // do not descend into them. A plain sequential block descends as usual.
    void handle(const BlockStatement& blk) {
        if (blk.blockKind == slang::ast::StatementBlockKind::Sequential) {
            visitDefault(blk);
        }
    }

    void handle(const UnaryExpression& u) {
        switch (u.op) {
            case UnaryOperator::Preincrement:
            case UnaryOperator::Postincrement:
            case UnaryOperator::Predecrement:
            case UnaryOperator::Postdecrement:
                // A ++/-- target is both read and written, so it drops out.
                record_write(u.operand());
                break;
            default:
                break;
        }
        visitDefault(u);
    }

    void handle(const VariableDeclStatement& d) {
        mark_written(d.symbol); // a block-local temporary, not an input
        visitDefault(d);
    }

    void handle(const ForLoopStatement& f) {
        for (const VariableSymbol* v : f.loopVars) {
            mark_written(*v);
        }
        visitDefault(f);
    }

    // A called subroutine's argument expressions are always sensitive. Its body
    // contributes only for a *function* under Comb rules (9.2.2.2.1): a function
    // may reference module signals directly, not only through its arguments, so
    // descend once into the callee body — the callee's formals / locals are not
    // module signals (not in sigmap), so they are ignored, while module signals
    // it reads / writes are recorded as usual. Task bodies never contribute
    // (9.2.2.2.1: "the contents of the tasks do not add anything"), and under
    // ImplicitEvent (@*) rules no callee body does (9.2.2.2.2).
    void handle(const CallExpression& c) {
        for (const Expression* arg : c.arguments()) {
            arg->visit(*this);
        }
        if (c.isSystemCall() || mode != SensitivityMode::Comb) {
            return;
        }
        const SubroutineSymbol* sub = std::get<0>(c.subroutine);
        if (sub != nullptr && sub->subroutineKind == SubroutineKind::Function &&
            entered.insert(sub).second) {
            sub->getBody().visit(*this);
        }
    }
};

// Whether evaluating an expression is observable beyond its value: any call
// (a user function body may display or write module state; system calls like
// $display always are), an embedded assignment, or an increment / decrement.
// Decides whether a ?: arm may be evaluated eagerly — LRM 11.4.11 forbids
// evaluating the unchosen arm of a definite condition, but for a pure arm the
// extra evaluation is unobservable and keeps the branchless packed select.
struct EffectDetector : ASTVisitor<EffectDetector, VisitFlags::AllGood> {
    bool found = false;

    void handle(const CallExpression&) { found = true; }
    void handle(const AssignmentExpression&) { found = true; }
    void handle(const UnaryExpression& u) {
        switch (u.op) {
            case UnaryOperator::Preincrement:
            case UnaryOperator::Postincrement:
            case UnaryOperator::Predecrement:
            case UnaryOperator::Postdecrement:
                found = true;
                return;
            default:
                visitDefault(u);
        }
    }
};

bool has_observable_effect(const slang::ast::Expression& e) {
    EffectDetector d;
    e.visit(d);
    return d.found;
}

// Walks the elaborated AST of one top module into an ir::Module.
class Lowerer {
public:
    // `tick_fs` is the femtoseconds-per-wheel-tick (design-wide smallest time
    // unit), or 0 when no design unit set a timescale (unitless ticks).
    Lowerer(ir::Module& module, uint64_t tick_fs)
      : module_(module)
      , tick_fs_(tick_fs) {}

    // Lowering runs the instance hierarchy twice. Signal registration must
    // precede body lowering across the *whole* design, not just up the tree:
    // a hierarchical reference (`top.u1.sig`, LRM 23.6) may point downward or
    // sideways to a signal whose instance is lowered later, and signal_of needs
    // it in sigmap_ already. So Collect walks the tree registering every signal
    // and building the scope tree; Emit walks it again lowering processes. The
    // two passes traverse identically, so per-scope signal order (and thus the
    // backend's state layout) is unchanged from a single interleaved pass.
    // Collect registers signals and builds the scope tree. Classify walks every
    // continuous driver (assign / net init / gate output / port copy) to detect
    // which nets gain multiple overlapping drivers (resolved_nets_), so Emit can
    // route those drivers to per-driver slots before it stores. Emit lowers the
    // behavioral members. All three traverse identically; only Collect creates
    // signals, so per-scope signal order (and the backend state layout) is
    // unchanged. A single-driver design does no work in Classify beyond the
    // driver-region bookkeeping it already did in Emit before.
    enum class Pass : uint8_t { Collect, Classify, Emit };

    // Flatten one elaborated instance into the module under `prefix` (empty for
    // the top, "<inst>." per level below). In Collect: open this instance's
    // hierarchy scope, register its signals, recurse to collect children. In
    // Emit: `node` is this instance's already-built scope; lower its processes
    // and recurse. Repeated instantiations disambiguate for free — slang gives
    // each instance distinct ValueSymbols, so sigmap_ (keyed by symbol) maps
    // each to its own signal.
    void lower_instance(const InstanceSymbol& inst, std::string_view prefix, Pass pass,
                        ir::InstanceScope* node) {
        if (prefix.empty() && pass == Pass::Collect) {
            module_.name = std::string(inst.name);
        }
        const std::string         self_prefix(prefix);
        const InstanceBodySymbol& body = inst.body;

        // Collect builds the scope node; Emit reuses the node Collect built.
        // A parent's children vector only grows during Collect (before this
        // child pops), so stacked pointers stay stable across that recursion;
        // by Emit the tree is frozen, so a child-node pointer is stable too.
        ir::InstanceScope* self = node;
        if (pass == Pass::Collect) {
            ir::InstanceScope fresh;
            fresh.id       = module_.alloc_scope_id();
            fresh.name     = std::string(inst.name);
            fresh.def_name = std::string(inst.getDefinition().name);
            if (scope_stack_.empty()) {
                top_scope_ = std::move(fresh);
                self       = &top_scope_;
            } else {
                scope_stack_.back()->children.push_back(std::move(fresh));
                self = &scope_stack_.back()->children.back();
            }
        }
        scope_stack_.push_back(self);

        // This instance's delay / $time multiplier: its time unit divided by the
        // wheel tick (the design's finest precision). Both are powers of ten and
        // the unit is never finer than the precision, so the ratio is an exact
        // integer (LRM §3.14). Stays 1 in unitless mode. Saved here so the
        // post-recursion restore below (with prefix_ / scope_) returns it to this
        // level after a child runs.
        if (tick_fs_ != 0) {
            const auto ts = body.getTimeScale();
            time_mul_     = ts ? tsv_to_fs(ts->base) / tick_fs_ : 1;
        }
        const uint64_t self_mul = time_mul_;

        prefix_ = self_prefix;
        scope_  = &body;
        if (pass == Pass::Collect) {
            collect_signals(body);
        }
        size_t child_cursor = 0;
        lower_members(body, self_prefix, self_mul, pass, *self, child_cursor);

        scope_stack_.pop_back();
    }

    // Lower one top instance and its whole hierarchy, then emit the accumulated
    // declaration initializers as this Lowerer's pre-init process. finalize_preinit
    // runs here, per top, rather than inside lower_instance: a multi-top design
    // gives every top a "<name>." prefix, so gating on an empty prefix would drop
    // all initializers (each top gets its own Lowerer, so this covers each one).
    void lower_top(const InstanceSymbol& top, std::string_view prefix) {
        top_elided_ = prefix.empty();
        lower_instance(top, prefix, Pass::Collect, nullptr);
        // Classify reuses the scope tree Collect built (Emit has not yet moved
        // it out); it only reads node pointers to walk children in order.
        lower_instance(top, prefix, Pass::Classify, &top_scope_);
        lower_instance(top, prefix, Pass::Emit, &top_scope_);
        emit_wire_resolvers();
        finalize_preinit();
        module_.add_top(std::move(top_scope_));
    }

    // Register the persistent state a package or the `$unit` compilation-unit
    // scope declares (LRM 6.8): a package/$unit variable has no instance
    // multiplicity, so it gets exactly one signal, shared by every top instance
    // that references it. Must run once, on a Lowerer of its own, before any
    // top's own Lowerer starts (see lower_compilation) so `sigmap()` can seed
    // every top's map with these symbols; its own declaration initializers land
    // in this Lowerer's PreInit process, so a package variable initializes
    // exactly once regardless of how many tops import it.
    void lower_globals(const Compilation& comp, const RootSymbol& root) {
        for (const PackageSymbol* pkg : comp.getPackages()) {
            collect_global_scope_signals(*pkg);
        }
        for (const CompilationUnitSymbol* unit : root.compilationUnits) {
            collect_global_scope_signals(*unit);
        }
        finalize_preinit();
    }

    const ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal>& sigmap() const {
        return sigmap_;
    }

    // Seed this Lowerer's symbol->signal map with globally-shared entries
    // (package / $unit variables) collected by lower_globals, so references to
    // them resolve through the ordinary signal_of path with no special casing.
    void seed_signals(const ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal>& seed) {
        for (const auto& [sym, sig] : seed) {
            sigmap_[sym] = sig;
        }
    }

    // Point this top's Lowerer at the shared globals Lowerer (the one
    // lower_globals ran on), so a package/$unit-scope subroutine call can
    // delegate its lowering there instead of duplicating it per top (see
    // function_id's is_global_subroutine check). `g` must outlive every call
    // made through this Lowerer.
    void set_globals(Lowerer* g) {
        globals_ = g;
    }

private:
    // Lower the non-signal members of a scope (an instance body or a generate
    // block) into processes: continuous assigns, procedural blocks, child
    // instances, and nested generate constructs. `self_prefix` is the
    // hierarchical name prefix of `scope`; `self_mul` is the enclosing instance's
    // time multiplier, restored after a child instance recurses. Signals are
    // already registered by collect_signals, which walked the same generate tree.
    void lower_members(const Scope& scope, const std::string& self_prefix, uint64_t self_mul,
                       Pass pass, ir::InstanceScope& node, size_t& child_cursor) {
        // prefix_ / scope_ hold this level's values on entry to each iteration:
        // set by the caller and restored after any case that descends and changes
        // them, so signal-creating process lowering always sees this level. In
        // Collect only the structural cases (instances / generate constructs) do
        // work — signals were registered by collect_signals; behavioral members
        // lower in Emit. `child_cursor` walks this scope's node.children in the
        // order Collect pushed them, so Emit hands each recursion its own node.
        const bool emit = pass == Pass::Emit;
        // Continuous drivers (assign / net init / gate / port copy) run in both
        // Classify (record driver regions, detect multi-driver nets) and Emit
        // (store, routing resolved nets to slots); behavioral-only members
        // (procedural blocks, variable initializers) run in Emit alone.
        const bool driver_pass = pass != Pass::Collect;
        cur_pass_               = pass;
        for (const Symbol& m : scope.members()) {
            switch (m.kind) {
                case SymbolKind::ContinuousAssign:
                    if (driver_pass) lower_continuous_assign(m.as<ContinuousAssignSymbol>());
                    break;
                case SymbolKind::ProceduralBlock:
                    if (emit) lower_procedural_block(m.as<ProceduralBlockSymbol>());
                    break;
                case SymbolKind::Variable:
                    // Storage was registered by collect_signals; here only a
                    // declaration initializer (LRM 6.8) needs lowering, into the
                    // pre-init process.
                    if (emit) lower_var_initializer(m.as<VariableSymbol>());
                    break;
                case SymbolKind::Net:
                    // Storage was registered by collect_signals; a net declaration
                    // assignment (`wire w = expr`) is a continuous assignment.
                    if (driver_pass) lower_net_initializer(m.as<NetSymbol>());
                    break;
                case SymbolKind::Instance: {
                    const InstanceSymbol& child = m.as<InstanceSymbol>();
                    // Emit wires ports after the child recurses: a connection's
                    // parent-side expression resolves against signals in this
                    // (parent) body. Recursion leaves prefix_ at the child level,
                    // so restore it before wiring.
                    ir::InstanceScope* child_node =
                      driver_pass ? &node.children[child_cursor++] : nullptr;
                    lower_instance(child, self_prefix + std::string(child.name) + ".", pass,
                                   child_node);
                    prefix_   = self_prefix;
                    scope_    = &scope;
                    time_mul_ = self_mul;
                    if (driver_pass) wire_ports(child);
                    break;
                }
                case SymbolKind::GenerateBlock: {
                    // A conditional/bare generate block elaborated to a single
                    // scope. An unmatched if/case branch is uninstantiated and
                    // contributes nothing (its body is not part of the design).
                    const auto& gb = m.as<GenerateBlockSymbol>();
                    if (!gb.isUninstantiated) {
                        lower_generate_block(gb, self_prefix + gb.getExternalName() + ".", self_mul,
                                             pass, node, child_cursor);
                        prefix_ = self_prefix;
                        scope_  = &scope;
                    }
                    break;
                }
                case SymbolKind::GenerateBlockArray: {
                    // A loop generate: one entry block per genvar iteration, named
                    // <array>[<index>] so flattened signal paths stay unique.
                    const auto& ga = m.as<GenerateBlockArraySymbol>();
                    for (const GenerateBlockSymbol* e : ga.entries) {
                        if (e->isUninstantiated) {
                            continue;
                        }
                        lower_generate_block(*e, self_prefix + gen_entry_name(ga, *e) + ".",
                                             self_mul, pass, node, child_cursor);
                    }
                    prefix_ = self_prefix;
                    scope_  = &scope;
                    break;
                }
                case SymbolKind::PrimitiveInstance:
                    if (driver_pass) lower_gate(m.as<PrimitiveInstanceSymbol>());
                    break;
                case SymbolKind::InstanceArray: {
                    // An array of instances — gate primitives (LRM 28.3.6) or
                    // modules/interfaces (LRM 23.3.6). Slang has already
                    // elaborated one leaf symbol per element with per-instance
                    // bit-sliced connections, so this only needs to walk to
                    // the leaves and lower each like its scalar counterpart.
                    lower_instance_array(m.as<InstanceArraySymbol>(), self_prefix, scope, self_mul,
                                         pass, node, child_cursor);
                    prefix_   = self_prefix;
                    scope_    = &scope;
                    time_mul_ = self_mul;
                    break;
                }
                // Known-inert members: elaboration artifacts and declarations
                // that bear no simulation behavior of their own. Ports are wired
                // by wire_ports; parameters/types/genvars are resolved by slang;
                // subroutines lower on demand at call sites; statement blocks
                // lower with their enclosing procedure; elab system tasks were
                // already issued as elaboration diagnostics.
                case SymbolKind::Port:
                // Interface members with no simulation behavior of their own.
                // An interface body is walked when lower_instance recurses into
                // the interface instance; a Modport is a named view (LRM 25.5),
                // an InterfacePort/ModportPort is a reference proxy resolved at
                // use via canonical_value_symbol. Their signals are ordinary
                // Variable/Net members handled above and by collect_signals.
                case SymbolKind::Modport:
                case SymbolKind::InterfacePort:
                case SymbolKind::ModportPort:
                case SymbolKind::Parameter:
                case SymbolKind::TypeParameter:
                case SymbolKind::TypeAlias:
                // A class declaration is a type, not a per-instance member: its
                // methods and object body lower on demand when a variable of the
                // class is declared / used (lower_class_type).
                case SymbolKind::ClassType:
                case SymbolKind::GenericClassDef:
                case SymbolKind::TransparentMember:
                case SymbolKind::Genvar:
                case SymbolKind::Subroutine:
                case SymbolKind::StatementBlock:
                case SymbolKind::WildcardImport:
                case SymbolKind::ExplicitImport:
                case SymbolKind::EmptyMember:
                case SymbolKind::ElabSystemTask:
                case SymbolKind::Specparam:
                case SymbolKind::DefParam:
                    break;
                default:
                    // Anything else (gate primitives, specify blocks, checkers,
                    // nettype aliases, ...) may carry behavior; refusing loudly
                    // beats silently dropping it.
                    fail("unsupported module member kind=" + std::string(toString(m.kind)));
            }
        }
    }

    // Lower one generate block's members under `block_prefix`. Genvar values are
    // implicit localparams in the block scope, so const folding and parameter
    // reads resolve them once scope_ points at the block.
    void lower_generate_block(const GenerateBlockSymbol& block, const std::string& block_prefix,
                              uint64_t self_mul, Pass pass, ir::InstanceScope& node,
                              size_t& child_cursor) {
        // A generate block gets no scope node of its own: its signals fold into
        // the enclosing instance's scope (with prefixed names), and any instances
        // it contains become children of that instance's node. So `node` /
        // `child_cursor` thread straight through.
        prefix_ = block_prefix;
        scope_  = &block;
        lower_members(block, block_prefix, self_mul, pass, node, child_cursor);
    }

    // Recurse through one (possibly multidimensional) array of instances —
    // gate primitives (LRM 28.3.6) or modules/interfaces (LRM 23.3.6). Slang
    // builds a nested InstanceArraySymbol per extra unpacked dimension (`foo
    // u[3][4] (...)`), each ultimately bottoming out at a leaf symbol that
    // already carries its own bit-sliced port/terminal connections — this
    // only walks to those leaves and lowers each like its scalar counterpart.
    // A module/interface leaf flattens under <array>[<index>], mirroring
    // generate-loop arrays (gen_entry_name); a gate leaf needs no hierarchy
    // bookkeeping of its own.
    void lower_instance_array(const InstanceArraySymbol& arr, const std::string& self_prefix,
                              const Scope& parent_scope, uint64_t self_mul, Pass pass,
                              ir::InstanceScope& node, size_t& child_cursor) {
        const bool driver_pass = pass != Pass::Collect;
        for (const Symbol* elem : arr.elements) {
            switch (elem->kind) {
                case SymbolKind::InstanceArray:
                    lower_instance_array(elem->as<InstanceArraySymbol>(), self_prefix, parent_scope,
                                        self_mul, pass, node, child_cursor);
                    break;
                case SymbolKind::PrimitiveInstance:
                    if (driver_pass) lower_gate(elem->as<PrimitiveInstanceSymbol>());
                    break;
                case SymbolKind::Instance: {
                    const InstanceSymbol& child = elem->as<InstanceSymbol>();
                    ir::InstanceScope*    child_node =
                      driver_pass ? &node.children[child_cursor++] : nullptr;
                    lower_instance(child, self_prefix + instance_array_element_name(child) + ".",
                                   pass, child_node);
                    prefix_   = self_prefix;
                    scope_    = &parent_scope;
                    time_mul_ = self_mul;
                    if (driver_pass) wire_ports(child);
                    break;
                }
                default:
                    // A checker instance array: checkers aren't lowered at
                    // all today, so fail the same way a lone one would.
                    fail("unsupported instance array element kind=" + std::string(toString(elem->kind)));
            }
        }
    }

    // Hierarchical-path segment for one instance-array element: <array>[<i0>]
    // [<i1>].... Mirrors slang's own hierarchical-path formatting (slang's
    // Symbol::getHierarchicalPath): arrayPath holds one canonical (always
    // zero-based) index per dimension, and adding each dimension's lower
    // bound recovers the source-code index regardless of whether that
    // dimension's range counts up or down.
    static std::string instance_array_element_name(const InstanceSymbol& elem) {
        std::string name(elem.getArrayName());
        slang::SmallVector<slang::ConstantRange, 4> dims;
        elem.getArrayDimensions(dims);
        SLANG_ASSERT(dims.size() == elem.arrayPath.size());
        for (size_t i = 0; i < dims.size(); i++) {
            name += '[';
            name += std::to_string(int32_t(elem.arrayPath[i]) + dims[i].lower());
            name += ']';
        }
        return name;
    }

    // Hierarchical-path segment for one loop-generate entry: <array>[<index>].
    // The entry's own name is empty; the genvar value (its arrayIndex) supplies
    // the subscript.
    static std::string gen_entry_name(const GenerateBlockArraySymbol& array,
                                      const GenerateBlockSymbol&      entry) {
        std::string name = array.getExternalName();
        if (entry.arrayIndex != nullptr) {
            name += '[';
            name += entry.arrayIndex->toString(slang::LiteralBase::Decimal, /*includeBase=*/false);
            name += ']';
        }
        return name;
    }

    [[noreturn]] void fail(std::string msg) {
        throw LowerError{ std::move(msg) };
    }

    ir::SourceLoc loc(const Expression& e) const {
        return to_source_loc(e.sourceRange.start());
    }
    ir::SourceLoc loc(const Statement& s) const {
        return to_source_loc(s.sourceRange.start());
    }

    // The compile-time banner prefix "<file>:<line>: <task>" for a $finish /
    // $stop (LRM §20.2). The location is resolved here (the backend is
    // slang-free); the runtime appends " called at <time>".
    std::string finish_banner(const Expression& call, std::string_view task) const {
        const slang::SourceManager* sm = scope_->getCompilation().getSourceManager();
        const slang::SourceLocation sl = call.sourceRange.start();
        return std::string(sm->getFileName(sl)) + ":" +
               std::to_string(sm->getLineNumber(sl)) + ": " + std::string(task);
    }

    // Compile-time "<file>:<line>: " banner prefix at a source location. The
    // runtime appends the descriptive message and the simulation time
    // (rt_case_violation).
    std::string banner_at(slang::SourceLocation sl) const {
        const slang::SourceManager* sm = scope_->getCompilation().getSourceManager();
        return std::string(sm->getFileName(sl)) + ":" +
               std::to_string(sm->getLineNumber(sl)) + ": ";
    }

    // Banner for a unique/priority case or if violation, located at the statement.
    std::string case_banner(const CaseStatement& c) const { return banner_at(c.sourceRange.start()); }

    // -- Types --

    ir::Ref lower_type(const slang::ast::Type& type) {
        const slang::ast::Type& ct = type.getCanonicalType();
        if (ct.isVoid()) {
            return module_.pool.void_type;
        }
        if (const auto* arr = ct.as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
            // Fixed-size unpacked array (memory). Recurse on the element (a nested
            // unpacked array gives a multidimensional memory; otherwise an integral
            // element). Bounds come straight from the declared range.
            const slang::ConstantRange r = arr->range;
            if (r.left < 0 || r.right < 0) {
                // TODO: negative element-index bounds (e.g. arr[-1:1]). The ir Type
                // stores bounds as uint32; supporting them needs signed normalization
                // in the backend element-select. Rare in RTL memories.
                fail("unpacked array with negative index bounds is not supported");
            }
            if (arr->elementType.getCanonicalType().isEvent()) {
                // An event lowers to a 1-bit placeholder whose trigger state is a
                // separate static per-event timestamp, so an event under an array
                // dimension would declare fine but have no trigger/wait machinery
                // (every use already fails; the declaration must too).
                // TODO: event arrays (need per-element trigger state, LRM 15.5).
                fail("unpacked array of events is not supported");
            }
            ir::Type t;
            t.kind    = ir::Type::Kind::UnpackedArray;
            t.element = lower_type(arr->elementType);
            t.left    = static_cast<uint32_t>(r.left);
            t.right   = static_cast<uint32_t>(r.right);
            return module_.pool.put(ir::Item{ t });
        }
        if (const auto* da = ct.as_if<slang::ast::DynamicArrayType>()) {
            // Dynamic array (LRM 7.5): a heap-backed rt_dynarr handle. Phase 1
            // restricts the element to a POD scalar (integral or real) — a single
            // per-element slot; string / aggregate / nested variable-size elements
            // need per-element deinit/copy and are a documented follow-up.
            const slang::ast::Type& et = da->elementType.getCanonicalType();
            if (!et.isIntegral() && !et.isFloating() && !et.isString()) {
                // TODO: dynamic array of struct / array / nested dynamic-array /
                // queue / associative element. The rt_elem_ops runtime lifecycle
                // already supports nested containers; the remaining work is the
                // element-select lvalue plumbing (handle-element OOB semantics)
                // and, for aggregates, whole-element rvalue staging.
                fail("dynamic array element type is not supported (only integral / "
                     "real / string elements)");
            }
            ir::Type t;
            t.kind    = ir::Type::Kind::DynArray;
            t.element = lower_type(da->elementType);

            return module_.pool.put(ir::Item{ t });
        }
        if (const auto* qt = ct.as_if<slang::ast::QueueType>()) {
            // Queue (LRM 7.10): a heap-backed rt_queue ring handle. Phase-1
            // restricts the element to a POD scalar (integral or real), same as
            // dynamic arrays — managed elements need per-element lifecycle.
            const slang::ast::Type& et = qt->elementType.getCanonicalType();
            if (!et.isIntegral() && !et.isFloating() && !et.isString()) {
                // TODO: queue of struct / array / nested dynamic-array / queue /
                // associative element. The rt_elem_ops runtime lifecycle already
                // supports nested containers; the remaining work is the
                // element-select lvalue plumbing (handle-element OOB semantics)
                // and, for aggregates, whole-element rvalue staging.
                fail("queue element type is not supported (only integral / real / "
                     "string elements)");
            }
            ir::Type t;
            t.kind    = ir::Type::Kind::Queue;
            t.element = lower_type(qt->elementType);
            // maxBound is the `[$:N]` max index (0 == unbounded); stored in left.
            t.left = qt->maxBound;

            return module_.pool.put(ir::Item{ t });
        }
        if (ct.isUnpackedArray()) {
            // TODO: associative arrays (variable-size, need heap-backed runtime
            // storage). Dynamic arrays and queues are handled above.
            fail("associative arrays are not supported");
        }
        if (ct.isString()) {
            return module_.pool.string_type;
        }
        if (ct.isFloating()) {
            // realtime is synonymous with real (LRM 6.12); both canonicalize to
            // the pool's real type.
            const auto& ft = ct.as<slang::ast::FloatingType>();
            return ft.floatKind == slang::ast::FloatingType::ShortReal ? module_.pool.shortreal_type
                                                                       : module_.pool.real_type;
        }
        if (ct.isStruct()) {
            // Checked ahead of the isIntegral() fallback below: PackedStructType
            // derives IntegralType in slang (LRM 7.2.1 - a packed struct is a
            // single bit-vector value), so without this branch a packed struct
            // would silently fall through to make_int_type and lose its field
            // identity (member access / assignment patterns need it; whole-
            // object declare/assign works either way since the storage is the
            // same bit-vector shape).
            if (const auto cached = struct_type_cache_.find(&ct); cached != struct_type_cache_.end()) {
                return cached->second;
            }

            const bool   is_packed = !ct.isUnpackedStruct();
            const Scope& body      = is_packed ? static_cast<const Scope&>(ct.as<slang::ast::PackedStructType>())
                                               : static_cast<const Scope&>(ct.as<slang::ast::UnpackedStructType>());
            std::vector<ir::FieldRow> fields;
            for (const auto& f : body.membersOfType<slang::ast::FieldSymbol>()) {
                if (!is_packed && f.getType().getCanonicalType().isUnpackedUnion()) {
                    // Unpacked unions are not lowered (packed union member access
                    // is also guarded). Every other member type — scalar, string,
                    // real, unpacked array, nested unpacked struct, dynamic
                    // array, queue — is backed by the recursive aggregate
                    // storage model.
                    fail("unpacked union member is not supported");
                }
                if (!is_packed && f.getType().getCanonicalType().isEvent()) {
                    // Same as the array-of-event guard above: the member would
                    // declare as an inert 1-bit placeholder with no trigger state.
                    // TODO: event struct members (LRM 15.5).
                    fail("event struct member is not supported");
                }
                // Packed member: `bitOffset` is the field's bit position. Unpacked
                // member: addressed by declaration index (`fieldIndex`), no flat
                // offset — store the index so the field table is self-describing.
                fields.push_back({ f.name, lower_type(f.getType()),
                                   is_packed ? static_cast<uint32_t>(f.bitOffset) : f.fieldIndex });
            }

            ir::Type t;
            t.kind      = ir::Type::Kind::Struct;
            t.is_signed = is_packed && ct.isSigned();
            t.is_packed = is_packed;
            // Packed: the whole-value scalar domain / width (shares PackedArray
            // layout; a packed struct with any 4-state member is wholly 4-state
            // per LRM 7.2.1, so the single domain is exact). Unpacked: element /
            // bounds are meaningless — each member's domain lives in its own
            // field-table type, so no flattened whole-struct domain exists; the
            // element stays void so no consumer can mistake it for one.
            t.element   = is_packed
                            ? (ct.isFourState() ? module_.pool.logic_type : module_.pool.bit_type)
                            : module_.pool.void_type;
            t.left      = is_packed ? ct.getBitWidth() - 1 : 0;
            t.right     = 0;
            t.fields_id = module_.pool.intern_struct_fields(std::move(fields));

            const ir::Ref ref = module_.pool.put(ir::Item{ t });
            struct_type_cache_.emplace(&ct, ref);

            return ref;
        }
        if (ct.isPackedUnion()) {
            // A packed union (LRM 7.3.1) is a single bit-vector value whose members
            // all overlay the least-significant bits (every member is the same
            // width). Like a packed struct it derives IntegralType in slang, so this
            // branch must sit ahead of the isIntegral() fallback or the union loses
            // its field identity (member access needs it). Reuses Kind::Struct with
            // is_packed=true: every field sits at bit_offset 0, so the packed
            // member-select path (a constant PartSelect) produces correct
            // reads/writes with no backend changes. Unpacked unions are a separate,
            // larger change (overlapping aggregate storage) and stay loud-failed.
            const auto& ut = ct.as<slang::ast::PackedUnionType>();
            if (ut.isTagged) {
                // TODO: tagged unions (LRM 7.3.2) carry a member tag and per-member
                // validity — no flat bit-vector overlay. Not lowered.
                fail("tagged union is not supported");
            }
            if (const auto cached = struct_type_cache_.find(&ct); cached != struct_type_cache_.end()) {
                return cached->second;
            }

            std::vector<ir::FieldRow> fields;
            for (const auto& f : static_cast<const Scope&>(ut).membersOfType<slang::ast::FieldSymbol>()) {
                // Every packed-union member overlays the LSB (bit_offset 0), so a
                // narrower member is a low part-select of the union value.
                fields.push_back({ f.name, lower_type(f.getType()), 0 });
            }

            ir::Type t;
            t.kind      = ir::Type::Kind::Struct;
            t.is_signed = ct.isSigned();
            t.is_packed = true;
            t.element   = ct.isFourState() ? module_.pool.logic_type : module_.pool.bit_type;
            t.left      = ct.getBitWidth() - 1;
            t.right     = 0;
            t.fields_id = module_.pool.intern_struct_fields(std::move(fields));

            const ir::Ref ref = module_.pool.put(ir::Item{ t });
            struct_type_cache_.emplace(&ct, ref);

            return ref;
        }
        if (ct.isUnpackedUnion()) {
            // TODO: unpacked unions (LRM 7.3) need overlapping aggregate storage —
            // a byte blob every member reinterprets — which the sequential-field
            // agg_storage_ty mirror does not model. A separate change; guarded
            // loud here (and at the member-access site) so an unpacked union never
            // lowers by accident.
            fail("unpacked union is not supported");
        }
        if (ct.isEvent()) {
            // Named event (LRM 15.5): no data value. Modeled as a 1-bit logic
            // placeholder signal whose only role is to own wait queues; the
            // trigger state lives in a separate per-event timestamp. See
            // docs/design/named-events.md.
            return module_.pool.logic_type;
        }
        if (ct.isCHandle()) {
            // chandle (LRM 6.14): an opaque pointer-width handle. Lowered as a
            // 2-state 64-bit value — the only legal operations are null /
            // assignment / equality (slang rejects arithmetic), all integer ops on
            // the handle bits. Enables DPI chandle args (marshaled to/from void*).
            return make_int_type(64, false, false);
        }
        if (ct.isClass()) {
            return lower_class_type(ct.as<slang::ast::ClassType>());
        }
        if (ct.isVirtualInterface()) {
            // TODO: virtual interface (LRM 25.9) — a runtime handle to an
            // interface instance. Needs a handle slot plus either static
            // single-binding resolution or a runtime-indexable subscribe path
            // for `@(vif.sig)`. See docs/design/interfaces.md (deferred).
            fail("virtual interface is not supported");
        }
        if (!ct.isIntegral()) {
            // TODO: string / class types.
            fail("unsupported non-integral type");
        }

        return make_int_type(ct.getBitWidth(), ct.isFourState(), ct.isSigned());
    }

    // Lower a class type (LRM 8) to a Kind::Class reference type. A class variable
    // holds a handle (a pointer, stored as a 2-state 64-bit value); the object body
    // layout is the property table, reused from the struct Field machinery but
    // NOMINAL (each class gets a fresh, never-deduped fields_id, so two structurally
    // identical classes — even two empty ones — never unify). Cached per ClassType
    // symbol; the class Ref is interned BEFORE its properties are lowered, so a
    // self-referential property (`class Node; Node next; endclass`) resolves through
    // the handle indirection with no fixpoint. See docs/design/classes.md.
    ir::Ref lower_class_type(const slang::ast::ClassType& clt) {
        if (const auto cached = class_type_cache_.find(&clt); cached != class_type_cache_.end()) {
            return cached->second;
        }
        // Phase 2/3 features (docs/design/classes.md §2): guarded loud.
        if (clt.getBaseClass() != nullptr) {
            // TODO: single inheritance (`extends`), `super`, override (Phase 2).
            fail("class inheritance is not supported");
        }
        if (clt.isAbstract || clt.isInterface) {
            // TODO: virtual / interface classes and dynamic dispatch (Phase 3).
            fail("virtual / interface classes are not supported");
        }
        if (clt.genericClass != nullptr) {
            // TODO: parameterized classes (Phase 3).
            fail("parameterized classes are not supported");
        }
        // Intern-first: reserve the field slot and build the class Ref before
        // lowering the properties, so a self-handle resolves via the cache.
        const uint32_t fid = module_.pool.reserve_struct_fields();
        ir::Type       t;
        t.kind      = ir::Type::Kind::Class;
        t.fields_id = fid;
        const ir::Ref ref = module_.pool.put(ir::Item{ t });
        class_type_cache_.emplace(&clt, ref);

        std::vector<ir::FieldRow> fields;
        uint32_t                  idx = 0;
        for (const auto& prop : clt.membersOfType<slang::ast::ClassPropertySymbol>()) {
            if (prop.lifetime == slang::ast::VariableLifetime::Static) {
                // TODO: static class members (one shared slot, Phase 2).
                fail("static class properties are not supported");
            }
            if (prop.randMode != slang::ast::RandMode::None) {
                // TODO: rand / randc properties and randomize() (Phase 3).
                fail("rand / randc class properties are not supported");
            }
            if (prop.getType().getCanonicalType().isEvent()) {
                // TODO: event class properties (LRM 15.5) — the property would
                // declare as an inert placeholder with no trigger state.
                fail("event class property is not supported");
            }
            class_prop_index_.emplace(&prop, idx);
            fields.push_back({ prop.name, lower_type(prop.getType()), idx });
            ++idx;
        }
        module_.pool.set_struct_fields(fid, std::move(fields));

        return ref;
    }

    // True if an expression's type is a fixed-size unpacked array (a memory).
    static bool is_unpacked_array(const slang::ast::Expression& e) {
        return e.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() != nullptr;
    }

    // True if a type is an unpacked struct (aggregate storage; whole-object ops
    // decompose field-wise, distinct from a packed struct's flat bit-vector).
    static bool is_unpacked_struct(const slang::ast::Type& t) {
        return t.getCanonicalType().isUnpackedStruct();
    }

    // True if a type contains an unpacked struct at any nesting depth (a bare
    // struct or an unpacked array whose leaf is one): it is backed by the
    // recursive aggregate model, so whole-object assignment / initialization
    // routes through the aggregate copy path, not the flat scalar/array path.
    static bool is_aggregate(const slang::ast::Type& t) {
        const slang::ast::Type& ct = t.getCanonicalType();
        if (ct.isUnpackedStruct()) {
            return true;
        }
        if (const auto* arr = ct.as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
            // A fixed array over a queue / dynamic-array element also takes the
            // recursive aggregate model (its cells are heap handles needing
            // per-cell lifecycle), mirroring the backend's needs_agg_storage.
            const slang::ast::Type& et = arr->elementType.getCanonicalType();
            if (et.as_if<slang::ast::QueueType>() != nullptr ||
                et.as_if<slang::ast::DynamicArrayType>() != nullptr) {
                return true;
            }
            return is_aggregate(arr->elementType);
        }
        return false;
    }

    // True if a type is a fixed-size unpacked array, dynamic array, or queue: the
    // remaining unpacked container shapes (scalar-element, so not is_aggregate)
    // whose declaration initializer must route through the same element-wise /
    // handle-aware emit_agg_store path as a struct-bearing aggregate, rather than
    // the plain scalar Store fallback (which mis-lowers a `'{...}` or bare `{...}`
    // literal, or a copy from another handle variable, into a raw bit-blast over
    // the container's handle/flat storage).
    static bool is_unpacked_container(const slang::ast::Type& t) {
        const slang::ast::Type& ct = t.getCanonicalType();
        return ct.as_if<slang::ast::FixedSizeUnpackedArrayType>() != nullptr ||
               ct.as_if<slang::ast::DynamicArrayType>() != nullptr ||
               ct.as_if<slang::ast::QueueType>() != nullptr;
    }

    // Store a whole unpacked struct: one blocking store per member (the aggregate
    // has no flat value, so a whole-object copy decomposes field-wise). `lhs_base`
    // is the struct's lvalue; `sty` its interned Kind::Struct type. The RHS is
    // either a `'{...}` assignment pattern (per-field element expressions, already
    // in declaration order and type-converted by slang) or another struct lvalue
    // (member-by-member copy). Members are scalar (nested unpacked struct members
    // are guarded at lower_type), so each store is a plain scalar store.
    void emit_struct_store(ir::Index lhs_base, ir::Ref sty, const Expression& rhs,
                           ir::Inst::Tag tag, ir::Builder& b, ir::SourceLoc l) {
        emit_agg_store(lhs_base, sty, rhs, tag, b, l);
    }

    static bool struct_has_field_defaults(const slang::ast::Type& t) {
        const slang::ast::Type& ct = t.getCanonicalType();
        if (!ct.isUnpackedStruct()) {
            return false;
        }
        for (const auto& f : ct.as<slang::ast::UnpackedStructType>().membersOfType<slang::ast::FieldSymbol>()) {
            if (f.getInitializer() != nullptr || struct_has_field_defaults(f.getType())) {
                return true;
            }
        }
        return false;
    }

    // Apply an unpacked struct type's field-level default initializers (see
    // struct_has_field_defaults) to `lhs`, for a variable declared with no
    // initializer of its own: one store per field that declares a default, in
    // declaration order, recursing into a nested struct field that has one of
    // its own. A field with no default (direct or nested) is left alone --
    // its storage already carries the plain power-on value (X / 0) the
    // backend's state initializer set, same as any other uninitialized
    // scalar. `sv_type` is the same struct as `sty`'s interned Kind::Struct
    // (needed alongside it: field initializer expressions and their real
    // types live on the slang AST, not the IR field table).
    void emit_struct_field_defaults(ir::Index lhs, ir::Ref sty, const slang::ast::Type& sv_type,
                                    ir::Builder& b, ir::SourceLoc l) {
        const auto& ct = sv_type.getCanonicalType().as<slang::ast::UnpackedStructType>();
        uint32_t idx = 0;
        for (const auto& f : ct.membersOfType<slang::ast::FieldSymbol>()) {
            if (const Expression* init = f.getInitializer()) {
                const ir::Index fld_lv = b.member_select(lhs, idx, sty, l);
                const ir::Index rhs    = lower_expr(*init, b);
                if (f.getType().getCanonicalType().isString()) {
                    b.binary(ir::Inst::Tag::Store, fld_lv, rhs, l);
                } else {
                    b.binary(ir::Inst::Tag::Store, fld_lv,
                             coerce(rhs, *init->type, lower_type(f.getType()), b, l), l);
                }
            } else if (struct_has_field_defaults(f.getType())) {
                emit_struct_field_defaults(b.member_select(lhs, idx, sty, l), lower_type(f.getType()),
                                           f.getType(), b, l);
            }
            ++idx;
        }
    }

    // True if a type is, or (through unpacked-array nesting) contains, an
    // unpacked struct with a field-level default (struct_has_field_defaults).
    static bool aggregate_has_field_defaults(const slang::ast::Type& t) {
        const slang::ast::Type& ct = t.getCanonicalType();
        if (ct.isUnpackedStruct()) {
            return struct_has_field_defaults(ct);
        }
        if (const auto* arr = ct.as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
            return aggregate_has_field_defaults(arr->elementType);
        }
        return false;
    }

    // Apply a struct-typed variable's field-level default initializers when
    // the variable itself has none (see struct_has_field_defaults /
    // emit_struct_field_defaults). An array of such a struct is a documented
    // follow-up: loud-guarded, not a silent no-op, since silently skipping it
    // would leave every element at its plain power-on value with the field
    // defaults dropped -- the exact bug this function exists to fix.
    void emit_var_field_defaults(const VariableSymbol& vs, ir::Index lv, ir::Builder& b, ir::SourceLoc l) {
        if (!aggregate_has_field_defaults(vs.getType())) {
            return;
        }
        if (is_unpacked_struct(vs.getType())) {
            emit_struct_field_defaults(lv, lower_type(vs.getType()), vs.getType(), b, l);
            return;
        }
        // TODO: an array of a struct with field-level defaults, and no array
        // initializer of its own -- each element needs the same per-field
        // defaulting emit_struct_field_defaults does for a bare struct.
        fail("an array of a struct with field-level default initializers, and no "
             "array initializer of its own, is not supported");
    }

    // Store a string literal into a fixed-size unpacked array of byte-shaped
    // elements (LRM 5.9: "A string literal can be assigned to an unpacked
    // array of bytes. If the size differs, it is left justified."). Character
    // 0 lands at the array's declared left bound, one element per character,
    // walking toward the right bound; a shorter string zero-pads the
    // remaining (rightmost) elements, a longer string's excess tail is
    // dropped. Unlike a `string`-typed object, an array element is not
    // string-terminated, so an embedded NUL byte is kept, not stripped.
    void emit_string_literal_array_store(ir::Index lhs, ir::Ref type, const ir::Type& t,
                                         std::string_view text, ir::Inst::Tag tag, ir::Builder& b,
                                         ir::SourceLoc l) {
        const uint32_t array_len =
            (t.left > t.right ? t.left - t.right : t.right - t.left) + 1;
        for (uint32_t k = 0; k < array_len; ++k) {
            const int32_t coord = t.left <= t.right ? static_cast<int32_t>(t.left + k)
                                                     : static_cast<int32_t>(t.left - k);
            const uint8_t   byte_val = k < text.size() ? static_cast<uint8_t>(text[k]) : 0;
            const ir::Index idx      = b.constant(
              module_.pool.put_int(make_int_type(32, false, true),
                                    static_cast<uint64_t>(static_cast<int64_t>(coord))),
              l);
            const ir::Index val = b.constant(module_.pool.put_int(t.element, byte_val), l);
            b.binary(tag, b.elem_select(lhs, idx, type, l), val, l);
        }
    }

    // Store an aggregate whole-object value into `lhs` (an aggregate lvalue of
    // type `type`), decomposing to per-leaf stores. RHS is a `'{...}` pattern
    // (per-field elements, recursed for aggregate members), a leaf value, or
    // another aggregate lvalue (member-by-member copy). A nonblocking store
    // samples each leaf's RHS now and commits per-leaf, which is whole-object NBA
    // semantics. Array-typed members are guarded (element-wise unroll is a
    // follow-up), so nesting is struct-through-struct here.
    void emit_agg_store(ir::Index lhs, ir::Ref type, const Expression& rhs,
                        ir::Inst::Tag tag, ir::Builder& b, ir::SourceLoc l) {
        const ir::Type& t   = std::get<ir::Type>(module_.pool.get(type));
        const auto*     pat = as_assignment_pattern(rhs);
        // A bare `{...}` concatenation literal targeting a queue / dynamic array /
        // fixed unpacked array is an unpacked-array concatenation (LRM 11.4.14),
        // not a packed bit-concat — slang already resolved the expression's own
        // type to the unpacked target, so it is positional like a `'{...}`
        // pattern and only differs in the element-span accessor.
        const bool is_concat_literal =
            pat == nullptr && rhs.kind == ExpressionKind::Concatenation && rhs.type->isUnpackedArray();
        if (pat != nullptr || is_concat_literal) {
            const auto elems =
                pat != nullptr ? pat->elements() : rhs.as<ConcatenationExpression>().operands();
            const uint32_t reps =
                pat != nullptr && rhs.kind == ExpressionKind::ReplicatedAssignmentPattern
                    ? static_cast<uint32_t>(
                          const_uint(rhs.as<ReplicatedAssignmentPatternExpression>().count()))
                    : 1;
            if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
                if (pat == nullptr) {
                    fail("unpacked concatenation literal into a struct is not supported");
                }
                // Re-fetch each row: lowering an element expression can intern a
                // new struct's field table, which invalidates a held
                // struct_fields span (see InternPool::struct_fields).
                const auto     nfields = module_.pool.struct_fields(t.fields_id).size();
                const uint32_t nelems  = static_cast<uint32_t>(elems.size());
                // A replicated pattern (`'{N{...}}`) stores only its per-repeat
                // item list in `elements()` (nelems < nfields) -- cycle through it
                // field-by-field, same as the array case's `elems[k % n]`. A
                // positional/structured pattern already has nelems == nfields, so
                // the modulo is a no-op there.
                for (uint32_t i = 0; i < nfields; ++i) {
                    const ir::Field f = module_.pool.struct_fields(t.fields_id)[i];
                    emit_agg_store(b.member_select(lhs, i, type, l), f.type, *elems[i % nelems], tag, b, l);
                }
                return;
            }
            if (t.kind == ir::Type::Kind::UnpackedArray) {
                const bool     keyed = pat != nullptr && rhs.kind == ExpressionKind::StructuredAssignmentPattern;
                const uint32_t n     = static_cast<uint32_t>(elems.size());
                const uint32_t lo    = std::min(t.left, t.right);
                for (uint32_t k = 0; k < n * reps; ++k) {
                    const int32_t coord =
                        keyed ? static_cast<int32_t>(lo + k)
                              : (t.left <= t.right ? static_cast<int32_t>(t.left + k)
                                                   : static_cast<int32_t>(t.left - k));
                    const ir::Index idx =
                        b.constant(module_.pool.put_int(make_int_type(32, false, true),
                                                        static_cast<uint64_t>(static_cast<int64_t>(coord))),
                                   l);
                    emit_agg_store(b.elem_select(lhs, idx, type, l), t.element, *elems[k % n], tag, b, l);
                }
                return;
            }
            if (t.kind == ir::Type::Kind::DynArray || t.kind == ir::Type::Kind::Queue) {
                emit_handle_literal_init(lhs, type, t, elems, reps, tag, b, l);
                return;
            }
            fail("'{...} assignment pattern over this aggregate is not supported");
        }
        if (value_cat(type) == ValueCat::handle) {
            // A queue / dynamic-array member: deep-copy from another handle
            // lvalue. A pattern / concat / slice source needs an element-wise
            // build (same follow-up as the top-level queue assignment).
            emit_handle_member_copy(lhs, rhs, type, tag, b, l);
            return;
        }
        if (is_leaf_ref(type)) {
            b.binary(tag, lhs, lower_expr(rhs, b), l);
            return;
        }
        // Aggregate RHS: another struct/array lvalue, or an aggregate-returning
        // call (the callee writes into a caller temp — the Call's lvalue — which
        // this copies into the target).
        const Expression& src = unwrap_conversion(rhs);
        if (t.kind == ir::Type::Kind::UnpackedArray && src.kind == ExpressionKind::StringLiteral) {
            if (!is_leaf_ref(t.element) || module_.pool.is_float(t.element) ||
                module_.pool.layout(t.element).width != 8) {
                // TODO: string literal assigned to an unpacked array whose element
                // isn't an 8-bit byte -- LRM 5.9 only defines this for "an
                // unpacked array of bytes".
                fail("string literal assignment to an unpacked array of non-byte "
                     "elements is not supported");
            }
            emit_string_literal_array_store(lhs, type, t, src.as<StringLiteral>().getValue(), tag, b, l);
            return;
        }
        if (src.kind == ExpressionKind::Call) {
            const std::optional<ir::Index> cv = lower_user_call(src.as<CallExpression>(), b);
            if (!cv) {
                fail("aggregate assignment from a void call");
            }
            if (tag == ir::Inst::Tag::Store && is_trivially_copyable(type)) {
                b.agg_copy(lhs, *cv, type, l);
            } else {
                emit_agg_copy(lhs, *cv, type, tag, b, l);
            }
            return;
        }
        if (!is_struct_lvalue_shape(src)) {
            fail("unsupported unpacked struct rvalue (only '{...} patterns, struct "
                 "variables, and function results are supported)");
        }
        const ir::Index src_lv = lower_lvalue(src, b);
        // A blocking copy of a trivially copyable aggregate is a single memcpy
        // of the storage (the byte image is the value). A heap-owning leaf
        // (string / dynamic array / queue) must copy per-leaf (its stores
        // deep-copy); a nonblocking copy always decomposes per-leaf (it defers).
        if (tag == ir::Inst::Tag::Store && is_trivially_copyable(type)) {
            b.agg_copy(lhs, src_lv, type, l);
            return;
        }
        emit_agg_copy(lhs, src_lv, type, tag, b, l);
    }

    // Build a dynamic-array or queue handle from a flat positional element list
    // (an assignment-pattern or bare unpacked-concatenation literal, LRM 7.5 /
    // 7.10 / 11.4.14). A dynamic array is allocated to the element count and
    // stored element-wise; a queue has no bulk-allocate op, so it is cleared
    // (whole-handle assignment replaces, not appends) then rebuilt with
    // push_back. `reps` is the `'{N{...}}` replicated-pattern multiplier (1 for
    // a positional/keyed pattern or a bare concatenation, which has no repeat
    // form of its own).
    void emit_handle_literal_init(ir::Index lhs, ir::Ref type, const ir::Type& t,
                                  std::span<const Expression* const> elems, uint32_t reps,
                                  ir::Inst::Tag tag, ir::Builder& b, ir::SourceLoc l) {
        if (tag != ir::Inst::Tag::Store) {
            // TODO: nonblocking '{...}/concatenation literal into a queue or
            // dynamic array (needs a handle-aware NBA staging path — same gap as
            // the sibling top-level nonblocking dynamic-array/queue guards).
            fail("nonblocking assignment of a literal into a queue or dynamic array is not supported");
        }
        const uint32_t n     = static_cast<uint32_t>(elems.size());
        const uint32_t count = n * reps;
        if (t.kind == ir::Type::Kind::DynArray) {
            const ir::Index size =
                b.constant(module_.pool.put_int(make_int_type(32, false, false), count), l);
            b.dyn_array_new(lhs, size, lhs, /*has_init*/ false, type, l);
            for (uint32_t k = 0; k < count; ++k) {
                const ir::Index idx =
                    b.constant(module_.pool.put_int(make_int_type(32, false, true), k), l);
                b.binary(ir::Inst::Tag::Store, b.elem_select(lhs, idx, type, l),
                        lower_expr(*elems[k % n], b), l);
            }
            return;
        }
        b.queue_delete(lhs, lhs, /*has_idx*/ false, type, l);
        for (uint32_t k = 0; k < count; ++k) {
            b.queue_push(lhs, lower_expr(*elems[k % n], b), /*front*/ false, type, l);
        }
    }

    // Per-leaf copy of one aggregate lvalue into another (both of `type`),
    // recursing structs and (for a blocking copy) unrolling arrays. Reached only
    // for a string-containing aggregate or a nonblocking copy; a nonblocking
    // whole-array copy is guarded (element-wise deferral is a follow-up).
    void emit_agg_copy(ir::Index lhs, ir::Index rhs, ir::Ref type, ir::Inst::Tag tag,
                       ir::Builder& b, ir::SourceLoc l) {
        const ir::Type& t = std::get<ir::Type>(module_.pool.get(type));
        if (t.kind == ir::Type::Kind::UnpackedArray) {
            if (tag != ir::Inst::Tag::Store) {
                fail("nonblocking whole-object copy of an unpacked-array aggregate is not "
                     "supported");
            }
            const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
            for (uint32_t e = 0; e < n; ++e) {
                const int32_t   coord = t.left <= t.right ? static_cast<int32_t>(t.left + e)
                                                          : static_cast<int32_t>(t.left - e);
                const ir::Index idx =
                  b.constant(module_.pool.put_int(make_int_type(32, false, true),
                                                  static_cast<uint64_t>(static_cast<int64_t>(coord))),
                             l);
                emit_agg_copy(b.elem_select(lhs, idx, type, l), b.elem_select(rhs, idx, type, l),
                              t.element, tag, b, l);
            }
            return;
        }
        if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
            const auto fields = module_.pool.struct_fields(t.fields_id);
            for (uint32_t i = 0; i < fields.size(); ++i) {
                emit_agg_copy(b.member_select(lhs, i, type, l), b.member_select(rhs, i, type, l),
                              fields[i].type, tag, b, l);
            }
            return;
        }
        if (value_cat(type) == ValueCat::handle) {
            if (tag != ir::Inst::Tag::Store) {
                // TODO: nonblocking copy of a queue / dynamic-array member
                // (needs a handle-aware NBA staging path).
                fail("nonblocking assignment to an aggregate with a queue / "
                     "dynamic array member is not supported");
            }
            if (t.kind == ir::Type::Kind::Queue) {
                b.queue_copy(lhs, rhs, type, l);
            } else {
                b.dyn_array_copy(lhs, rhs, type, l);
            }
            return;
        }
        b.binary(tag, lhs, b.unary(ir::Inst::Tag::Load, rhs, l), l);
    }

    // Deep-copy into a queue / dynamic-array member of an aggregate from an
    // aggregate-store RHS element: the member's slot in the RHS lvalue. Only a
    // handle-shaped source is supported (variable / member / element).
    void emit_handle_member_copy(ir::Index lhs, const Expression& rhs, ir::Ref type,
                                 ir::Inst::Tag tag, ir::Builder& b, ir::SourceLoc l) {
        const ir::Type& t = std::get<ir::Type>(module_.pool.get(type));
        if (tag != ir::Inst::Tag::Store) {
            // TODO: nonblocking copy of a queue / dynamic-array member.
            fail("nonblocking assignment to an aggregate with a queue / dynamic "
                 "array member is not supported");
        }
        const Expression& src = unwrap_conversion(rhs);
        const bool        src_is_handle_lvalue =
            src.kind == ExpressionKind::NamedValue ||
            src.kind == ExpressionKind::HierarchicalValue ||
            src.kind == ExpressionKind::MemberAccess ||
            src.kind == ExpressionKind::ElementSelect;
        if (!src_is_handle_lvalue) {
            // TODO: queue / dynamic-array member init from a '{...} pattern or
            // concatenation needs an element-wise build (dynamic-arrays doc
            // phase 4), same as the top-level queue assignment.
            fail("queue / dynamic array member assignment from a non-variable "
                 "value is not supported");
        }
        const ir::Index src_lv = lower_lvalue(src, b);
        if (t.kind == ir::Type::Kind::Queue) {
            b.queue_copy(lhs, src_lv, type, l);
        } else {
            b.dyn_array_copy(lhs, src_lv, type, l);
        }
    }

    // Whole-object blocking copy of one aggregate lvalue into another, for a port
    // connection or net-declaration continuous copy (same fast/slow split as a
    // blocking `a = b`). A trivially copyable aggregate is one memcpy (AggCopy);
    // a heap-owning leaf forces the per-leaf deep copy.
    void emit_port_agg_copy(ir::Index dst, ir::Index src, ir::Ref ty, ir::Builder& b, ir::SourceLoc l) {
        if (is_trivially_copyable(ty)) {
            b.agg_copy(dst, src, ty, l);
        } else {
            emit_agg_copy(dst, src, ty, ir::Inst::Tag::Store, b, l);
        }
    }

    // Whether an expression is a whole unpacked-array lvalue (a named array, or a
    // member / element that is itself a whole array) — the source of a whole-array
    // memcpy copy, as opposed to a slice / element / pattern.
    static bool is_whole_array_lvalue(const Expression& e) {
        if (e.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() == nullptr) {
            return false;
        }
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::MemberAccess:
            case ExpressionKind::ElementSelect:
                return true;
            default:
                return false;
        }
    }

    // Storage/copy category of an interned IR type, driving whole-object and
    // aggregate-leaf dispatch. Exhaustive over every type kind, so a leaf that
    // needs special copy semantics (a heap-owning handle) can never silently
    // take the plane-store path.
    enum class ValueCat {
        scalar,    // plane / real / class-or-chandle word: copy = plane store
        string,    // rt_string handle: copy = deep string assign
        handle,    // dynamic array / queue handle: copy = deep runtime assign
        aggregate, // unpacked struct / fixed unpacked array: decomposes per leaf
    };
    ValueCat value_cat(ir::Ref type) const {
        const ir::Type& t = std::get<ir::Type>(module_.pool.get(type));
        switch (t.kind) {
            case ir::Type::Kind::String:
                return ValueCat::string;
            case ir::Type::Kind::DynArray:
            case ir::Type::Kind::Queue:
                return ValueCat::handle;
            case ir::Type::Kind::UnpackedArray:
                return ValueCat::aggregate;
            case ir::Type::Kind::Struct:
                return t.is_packed ? ValueCat::scalar : ValueCat::aggregate;
            default:
                return ValueCat::scalar;
        }
    }

    // Whether an interned IR type is a scalar / string / real / handle leaf.
    bool is_leaf_ref(ir::Ref type) const {
        return value_cat(type) != ValueCat::aggregate;
    }

    // Whether an interned IR type contains an unpacked struct at any nesting depth
    // (backed by the recursive aggregate model; passed / returned by pointer).
    bool contains_struct_ref(ir::Ref type) const {
        const ir::Type& t = std::get<ir::Type>(module_.pool.get(type));
        if (t.kind == ir::Type::Kind::UnpackedArray) {
            return contains_struct_ref(t.element);
        }
        return t.kind == ir::Type::Kind::Struct && !t.is_packed;
    }

    // Whether a whole-object copy of the type may be a single memcpy of its
    // storage: no leaf owns a heap buffer (string / dynamic array / queue), so
    // duplicating the byte image duplicates the value. A class-handle leaf IS
    // trivially copyable — the pointer copy is the reference semantics.
    //
    // NOTE: distinct from trivial relocatability, which every value type here
    // has (no self-pointers): the runtime containers memcpy-MOVE elements
    // (ring growth, insert/delete shifts) even for heap-owning element types;
    // only memcpy DUPLICATION is what this predicate gates.
    bool is_trivially_copyable(ir::Ref type) const {
        switch (value_cat(type)) {
            case ValueCat::string:
            case ValueCat::handle:
                return false;
            case ValueCat::scalar:
                return true;
            case ValueCat::aggregate: {
                const ir::Type& t = std::get<ir::Type>(module_.pool.get(type));
                if (t.kind == ir::Type::Kind::UnpackedArray) {
                    return is_trivially_copyable(t.element);
                }
                for (const ir::Field& f : module_.pool.struct_fields(t.fields_id)) {
                    if (!is_trivially_copyable(f.type)) {
                        return false;
                    }
                }
                return true;
            }
        }
        return false;
    }

    // Downcast to an assignment pattern (`'{...}`) base if the expression is one
    // of the three concrete pattern kinds. The base is abstract (no isKind), so
    // it cannot go through `as`/`as_if`; the kind check gates the static_cast.
    static const slang::ast::AssignmentPatternExpressionBase* as_assignment_pattern(const Expression& e) {
        switch (e.kind) {
            case ExpressionKind::SimpleAssignmentPattern:
            case ExpressionKind::StructuredAssignmentPattern:
            case ExpressionKind::ReplicatedAssignmentPattern:
                return static_cast<const slang::ast::AssignmentPatternExpressionBase*>(&e);
            default:
                return nullptr;
        }
    }

    ir::Index lower_packed_pattern(const Expression& expr, ir::Builder& b) {
        const ir::SourceLoc l   = loc(expr);
        const auto*         pat = as_assignment_pattern(expr);
        const auto          es  = pat->elements();
        std::vector<ir::Index> parts;
        parts.reserve(es.size());
        for (const Expression* e : es) {
            parts.push_back(lower_expr(*e, b));
        }
        if (expr.kind == ExpressionKind::StructuredAssignmentPattern &&
            expr.type->isPackedArray()) {
            const slang::ConstantRange r = expr.type->getFixedRange();
            if (r.left > r.right) {
                std::reverse(parts.begin(), parts.end());
            }
        }
        if (expr.kind == ExpressionKind::ReplicatedAssignmentPattern) {
            const uint64_t count =
                const_uint(expr.as<ReplicatedAssignmentPatternExpression>().count());
            return b.repl(b.concat(parts, l), static_cast<uint32_t>(count), l);
        }
        return b.concat(parts, l);
    }

    // 1-bit 4-state equality of two whole unpacked structs: one AggEq node the
    // backend expands into per-leaf `==` results AND-reduced with Land (the
    // reduction carries the LRM 11.4.5 X-propagation). Both operands must be
    // struct lvalues (a '{...} pattern compare is a follow-up); an array member
    // is guarded (element-wise compare is a follow-up).
    ir::Index lower_struct_eq(const Expression& lhs, const Expression& rhs, ir::Builder& b,
                              ir::SourceLoc l) {
        const Expression& le = unwrap_conversion(lhs);
        const Expression& re = unwrap_conversion(rhs);
        if (!is_struct_lvalue_shape(le) || !is_struct_lvalue_shape(re)) {
            fail("unpacked struct equality is only supported between struct variables");
        }
        const ir::Ref sty = lower_type(*lhs.type);
        guard_agg_eq_members(sty);
        return b.agg_eq(lower_lvalue(le, b), lower_lvalue(re, b), sty, l);
    }

    // Reject comparable-aggregate shapes the backend expansion does not cover.
    void guard_agg_eq_members(ir::Ref type) {
        const ir::Type& t = std::get<ir::Type>(module_.pool.get(type));
        if (t.kind == ir::Type::Kind::UnpackedArray) {
            // TODO: equality of an aggregate with an unpacked-array member — an
            // element-wise compare (docs/design/unpacked-struct.md).
            fail("equality of an aggregate with an unpacked-array member is not supported");
        }
        if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
            for (const auto& f : module_.pool.struct_fields(t.fields_id)) {
                guard_agg_eq_members(f.type);
            }
        }
        if (value_cat(type) == ValueCat::handle) {
            // TODO: equality of an aggregate with a queue / dynamic-array member
            // needs a length compare plus an element-wise walk (LRM 7.5/7.10).
            fail("equality of an aggregate with a queue / dynamic array member is "
                 "not supported");
        }
    }

    // Strip an outer identity/struct Conversion wrapper slang may insert on a
    // struct-typed rvalue, exposing the underlying lvalue-shaped expression.
    static const Expression& unwrap_conversion(const Expression& e) {
        const Expression* cur = &e;
        while (cur->kind == ExpressionKind::Conversion) {
            cur = &cur->as<ConversionExpression>().operand();
        }
        return *cur;
    }

    // Whether an expression is an lvalue-shaped struct reference (a member-by-member
    // copy source): a named variable or a select landing on one.
    static bool is_struct_lvalue_shape(const Expression& e) {
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::MemberAccess:
            case ExpressionKind::ElementSelect:
                return true;
            default:
                return false;
        }
    }

    // True if an expression's type is a dynamic array (LRM 7.5).
    static bool is_dyn_array(const slang::ast::Expression& e) {
        return e.type->getCanonicalType().as_if<slang::ast::DynamicArrayType>() != nullptr;
    }

    // True if an expression's type is a queue (LRM 7.10).
    static bool is_queue(const slang::ast::Expression& e) {
        return e.type->getCanonicalType().as_if<slang::ast::QueueType>() != nullptr;
    }

    // True if an expression is of the dynamic `string` type (LRM 6.16).
    static bool is_string_expr(const slang::ast::Expression& e) {
        return e.type->getCanonicalType().isString();
    }

    // True if an interned IR type is the string type, or an unpacked array whose
    // leaf element is.
    bool is_string_ref(ir::Ref ty) const {
        const ir::Type& t = std::get<ir::Type>(module_.pool.get(ty));
        if (t.kind == ir::Type::Kind::UnpackedArray) {
            return is_string_ref(t.element);
        }
        return t.kind == ir::Type::Kind::String;
    }

    // True if an interned IR type is a dynamic array (LRM 7.5).
    bool is_dyn_array_ref(ir::Ref ty) const {
        return std::get<ir::Type>(module_.pool.get(ty)).kind == ir::Type::Kind::DynArray;
    }

    // True if an interned IR type is a heap-handle value type — a dynamic array or
    // queue — which share the by-pointer, deep-copy subroutine ABI.
    bool is_handle_ref(ir::Ref ty) const {
        const ir::Type::Kind k = std::get<ir::Type>(module_.pool.get(ty)).kind;
        return k == ir::Type::Kind::DynArray || k == ir::Type::Kind::Queue;
    }

    // True if an expression is a dynamic array or queue (a heap handle).
    static bool is_handle_expr(const slang::ast::Expression& e) {
        const slang::ast::Type& ct = e.type->getCanonicalType();
        return ct.as_if<slang::ast::DynamicArrayType>() != nullptr ||
               ct.as_if<slang::ast::QueueType>() != nullptr;
    }

    // Intern a string literal's characters as a string constant. Every \0 byte
    // is dropped: a string object never contains one, and literal-to-string
    // assignment strips them (LRM 6.16).
    ir::Ref string_value_ref(std::string_view text) {
        std::string stripped;
        stripped.reserve(text.size());
        for (const char c : text) {
            if (c != '\0') {
                stripped += c;
            }
        }
        return module_.pool.put_value(
          module_.pool.string_type,
          { reinterpret_cast<const uint8_t*>(stripped.data()), stripped.size() });
    }

    // Intern a packed integer type of the given width / state / signedness.
    ir::Ref make_int_type(uint32_t width, bool is_four, bool is_signed) {
        const ir::Ref element = is_four ? module_.pool.logic_type : module_.pool.bit_type;
        if (width == 1 && !is_signed) {
            return element;
        }

        ir::Type vec;
        vec.kind      = ir::Type::Kind::PackedArray;
        vec.is_signed = is_signed;
        vec.element   = element;
        vec.left      = width - 1;
        vec.right     = 0;

        return module_.pool.put(ir::Item{ vec });
    }

    // Resize / convert an already-lowered rvalue of slang type `from` to the
    // interned target `to`. Integral-to-integral resizes; any real endpoint
    // picks the matching float conversion (assignment semantics: real ->
    // integral rounds half away from zero, LRM 6.12.1). Used where the
    // frontend coerces manually (port copies, call arguments, returns) rather
    // than through a slang-inserted Conversion.
    ir::Index coerce(ir::Index v, const slang::ast::Type& from, ir::Ref to, ir::Builder& b, ir::SourceLoc l) {
        const bool from_f = from.isFloating();
        const bool to_f   = module_.pool.is_float(to);
        if (to_f && from_f) {
            return b.float_cvt(ir::Inst::Tag::FResize, v, to, l);
        }
        if (to_f) {
            return b.float_cvt(ir::Inst::Tag::ItoF, v, to, l);
        }
        if (from_f) {
            return b.float_cvt(ir::Inst::Tag::FtoI, v, to, l);
        }
        return b.resize(v, to, l);
    }

    // Narrow/widen an already-loaded rvalue to its expression's own declared
    // type (LRM 6.3.1 domain conversion). A no-op for the ordinary
    // (domain-uniform) case; skipped for string/real, which have no domain
    // to narrow (a packed struct member is always integral, LRM 7.2.1) and
    // whose loaded value a plain Resize instruction rejects outright. Needed
    // because the backend's PartSelect/BitSelect type inference
    // (LlvmBackend.cpp compute_type) inherits the base signal's domain
    // uniformly — correct for an ordinary vector/array, wrong when the base
    // is a struct field whose own declared domain is narrower than its
    // struct's physical storage (LRM 6.11.2 forces the whole struct 4-state
    // if any member is).
    ir::Index narrow_to_declared(ir::Index v, const slang::ast::Type& declared, ir::Builder& b, ir::SourceLoc l) {
        if (declared.isString() || declared.isFloating()) {
            return v;
        }
        return b.resize(v, lower_type(declared), l);
    }

    // Intern an integer literal, preserving x/z bits. Slang stores a 4-state
    // SVInt as [value words][unknown words] with X=(val0,unk1), Z=(val1,unk1) —
    // the opposite val convention from the pool's VPI planes (X=(val1,xz1),
    // Z=(val0,xz1)). The unknown word IS the xz plane; the val plane is
    // value^unknown so the per-bit codes line up. 2-state literals take the fast
    // uint64 path.
    ir::Ref lower_int_literal(const IntegerLiteral& lit) {
        return svint_to_ref(lit.getValue(), lower_type(*lit.type));
    }

    // Convert a slang SVInt into a pooled IR constant of the given type.
    ir::Ref svint_to_ref(const slang::SVInt& v, ir::Ref ty) {
        const uint32_t width = v.getBitWidth();
        // Fast path: a known value that fits a machine word.
        if (!v.hasUnknown() && width <= 64) {
            return module_.pool.put_int(ty, v.as<uint64_t>().value_or(0));
        }

        // General path: pack the raw words into val / xz planes. A wide (>64-bit)
        // value heap-allocates even when fully known (e.g. a packed string literal
        // like a 96-bit "hello world!"), so the fast path above must not swallow it
        // via a 64-bit truncation. getRawPtr lays out value words, followed by the
        // unknown words only when hasUnknown.
        const uint32_t        words = (width + 63) / 64;
        const uint64_t*       raw   = v.getRawPtr();
        const bool            unk   = v.hasUnknown();
        std::vector<uint64_t> planes(static_cast<size_t>(words) * 2, 0);
        for (uint32_t i = 0; i < words; ++i) {
            planes[i]         = unk ? (raw[i] ^ raw[words + i]) : raw[i]; // val plane (VPI)
            planes[words + i] = unk ? raw[words + i] : 0;                 // xz plane (unknown)
        }

        const uint32_t       pb = ir::plane_bytes(width);
        std::vector<uint8_t> bytes(static_cast<size_t>(pb) * 2, 0);
        std::memcpy(bytes.data(), planes.data(), pb);
        std::memcpy(bytes.data() + pb, planes.data() + words, pb);

        return module_.pool.put_value(ty, bytes);
    }

    // Lower a slang compile-time constant (a resolved parameter value or a
    // folded constant-required expression) into a pooled IR constant.
    //
    // TODO: string/struct/array/enum constant parameters (depends on the
    // matching runtime types — see docs/TODO.md Tier 3).
    ir::Ref lower_constant(const slang::ConstantValue& cv, const slang::ast::Type& type) {
        // Real / shortreal parameters (LRM 6.20), including untyped
        // `parameter r = 5.7`, whose type slang resolves to real.
        if (cv.isReal()) {
            return module_.pool.put_real(lower_type(type), static_cast<double>(cv.real()));
        }
        if (cv.isShortReal()) {
            return module_.pool.put_real(lower_type(type), static_cast<double>(cv.shortReal()));
        }
        if (!cv.isInteger()) {
            fail("only integral and real constant parameters are supported");
        }
        return svint_to_ref(cv.integer(), lower_type(type));
    }

    // -- Signals --

    // Register the Variable members of a package or `$unit` scope as signals
    // (see lower_globals). Only `Variable` needs action here: nets can't
    // legally appear at this scope (LRM 26.2 / A.1.2), and every other member
    // kind (typedefs, parameters, functions/tasks, imports, classes, ...)
    // either folds at each reference or lowers lazily at its own call/use site
    // — including the slang-injected `std` package, whose class declarations
    // Clotho does not yet support but which no design can reference without
    // hitting that unsupported-type guard independently. `scope_` is set so
    // an initializer expression (ASTContext-driven constant fold, format-arg
    // eval, ...) has a scope to resolve against, matching every other
    // signal-collection path.
    void collect_global_scope_signals(const Scope& scope) {
        scope_ = &scope;
        for (const Symbol& m : scope.members()) {
            if (m.kind != SymbolKind::Variable) {
                continue;
            }
            const auto& vs = m.as<VariableSymbol>();

            ir::SignalInfo info;
            info.name = std::string(vs.name);
            info.type = lower_type(vs.getType());
            info.dir  = ir::SignalInfo::Direction::Internal;

            sigmap_[&vs] = register_signal(std::move(info));
            lower_var_initializer(vs);
        }
    }

    void collect_signals(const InstanceBodySymbol& body) {
        // First pass: record port directions by the symbol they back. Ports exist
        // only on the instance body, never inside a generate block.
        ankerl::unordered_dense::map<const Symbol*, ir::SignalInfo::Direction> port_dir;
        for (const Symbol& m : body.members()) {
            if (m.kind == SymbolKind::Port) {
                const PortSymbol& p = m.as<PortSymbol>();
                if (p.internalSymbol) {
                    port_dir[p.internalSymbol] = direction_of(p.direction);
                }
            }
        }

        collect_scope_signals(body, prefix_, &port_dir);
    }

    // Register the nets and variables of one scope as signals under `prefix`,
    // recursing through instantiated generate blocks/arrays (their nets carry the
    // block's name in the path). `port_dir`, present only for an instance body,
    // tags port-backed signals with their direction; generate-block signals are
    // always internal. Child instances are handled by lower_instance, not here.
    void collect_scope_signals(const Scope& scope, const std::string& prefix,
                               const ankerl::unordered_dense::map<const Symbol*, ir::SignalInfo::Direction>* port_dir) {
        for (const Symbol& m : scope.members()) {
            switch (m.kind) {
                case SymbolKind::Net:
                case SymbolKind::Variable: {
                    const ValueSymbol& vs = m.as<ValueSymbol>();

                    ir::SignalInfo info;
                    if (m.kind == SymbolKind::Net) {
                        const NetSymbol& net = m.as<NetSymbol>();
                        // Modeled net kinds: wire/tri (+ uwire, single-driver
                        // per slang's MultipleUWireDrivers pass) resolve to X on
                        // conflict; wand/triand and wor/trior carry wired-AND /
                        // wired-OR resolution (net_resolve_tag); tri0/tri1 carry a
                        // resistive pull to 0 / 1 (net_pull_value). All Z-default
                        // when undriven, except tri0/tri1 which pull to 0 / 1.
                        // TODO: trireg/supply0/supply1 carry charge / strength
                        // semantics clotho does not implement.
                        switch (net.netType.netKind) {
                            case NetType::Wire:
                            case NetType::Tri:
                            case NetType::UWire:
                            case NetType::WAnd:
                            case NetType::TriAnd:
                            case NetType::WOr:
                            case NetType::TriOr:
                            case NetType::Tri0:
                            case NetType::Tri1:
                                break;
                            default:
                                fail("unsupported net type (only wire/tri/uwire/wand/wor/tri0/tri1 are modeled)");
                        }
                        if (net.getDelay() != nullptr) {
                            // TODO: net delay (`wire #5 w;`, LRM 28.16) — an
                            // inertial delay on every driver of the net; dropped
                            // today, so propagation would be instant.
                            fail("net delay is not supported");
                        }
                        check_drive_strength(net.getDriveStrength(), "a net declaration");
                        info.is_net = true;
                    }
                    info.name     = prefix + std::string(vs.name);
                    info.type     = lower_type(vs.getType());
                    info.is_event = vs.getType().isEvent();
                    info.dir      = ir::SignalInfo::Direction::Internal;
                    if (port_dir != nullptr) {
                        const auto found = port_dir->find(&m);
                        if (found != port_dir->end()) {
                            info.dir = found->second;
                        }
                    }
                    if (info.dir != ir::SignalInfo::Direction::Internal && info.is_event) {
                        // TODO: event ports (LRM 15.5) — needs cross-boundary
                        // event aliasing (Tier 2, docs/design/named-events.md).
                        fail("event ports are not supported");
                    }
                    if (info.dir != ir::SignalInfo::Direction::Internal && is_string_ref(info.type)) {
                        // TODO: string module ports (needs string-aware port
                        // copies and change propagation).
                        fail("string module ports are not supported");
                    }

                    const ir::Signal sig = register_signal(std::move(info));
                    sigmap_[&vs]         = sig;
                    if (m.kind == SymbolKind::Net) {
                        // A tri0/tri1 net always needs a resolver so its pull can
                        // fill undriven bits — even with zero or one driver. Record
                        // it here (its home scope) so a driverless one still emits.
                        if (const std::optional<int> pull =
                              net_pull_value(m.as<NetSymbol>().netType.netKind)) {
                            net_pull_[static_cast<uint32_t>(sig)] =
                              PullInfo{ *pull, cur_scope_id(), to_source_loc(vs.location) };
                        }
                    }
                    break;
                }
                case SymbolKind::GenerateBlock: {
                    const auto& gb = m.as<GenerateBlockSymbol>();
                    if (!gb.isUninstantiated) {
                        collect_scope_signals(gb, prefix + gb.getExternalName() + ".", nullptr);
                    }
                    break;
                }
                case SymbolKind::GenerateBlockArray:
                    for (const GenerateBlockSymbol* e : m.as<GenerateBlockArraySymbol>().entries) {
                        if (!e->isUninstantiated) {
                            collect_scope_signals(*e, prefix + gen_entry_name(m.as<GenerateBlockArraySymbol>(), *e) + ".", nullptr);
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }

    // Append a signal to the module and record it in the currently open
    // hierarchy scope. Every signal-creating path funnels through here so the
    // scope tree covers the signal table exactly. scope_stack_ is empty only on
    // the standalone Lowerer that registers package/$unit-scope state (globals;
    // see lower_globals) and package-scope function statics reached through it
    // (see function_id's is_global_subroutine delegation) — those signals
    // belong to no top's InstanceScope tree, and the backend already treats an
    // unclaimed signal as a flat root-level field (build_state_types).
    ir::Signal register_signal(ir::SignalInfo info) {
        const ir::Signal sig = module_.add_signal(std::move(info));
        if (!scope_stack_.empty()) {
            scope_stack_.back()->signals.push_back(sig);
        }

        return sig;
    }

    ir::Signal signal_of(const ValueSymbol& sym) {
        const ValueSymbol& csym = canonical_value_symbol(sym);
        if (csym.kind == SymbolKind::ModportPort) {
            // TODO: modport expression port with a structural connection
            // (`.name(sig[hi:lo])` or a concat, LRM 25.5.3) — maps to a bit
            // sub-range, not a whole signal, so it must lower as that
            // sub-lvalue. Plain renames resolve above.
            fail("modport expression port with a non-trivial connection "
                 "expression is not supported");
        }
        const auto it = sigmap_.find(&csym);
        if (it == sigmap_.end()) {
            fail("reference to unknown signal '" + std::string(csym.name) + "'");
        }

        return it->second;
    }

    // Register a procedural local variable (a for-loop control variable or a
    // block-local declaration) as backing signal storage, reusing the signal
    // machinery for its loads/stores. Idempotent.
    //
    // NOTE: this gives the local static, signal-like storage rather than true
    // per-entry automatic lifetime (12.7.1). That is correct for the
    // non-recursive, single-instance processes the simulator models today, and
    // matches how module variables are already handled.
    ir::Signal declare_local(const ValueSymbol& vs) {
        const auto it = sigmap_.find(&vs);
        if (it != sigmap_.end()) {
            return it->second;
        }

        ir::SignalInfo info;
        info.name = prefix_ + std::string(vs.name);
        info.type = lower_type(vs.getType());
        info.dir  = ir::SignalInfo::Direction::Internal;

        const ir::Signal sig = register_signal(std::move(info));
        sigmap_[&vs]         = sig;

        return sig;
    }

    // -- Net driver tracking --
    //
    // Each net's driven regions are recorded so a second overlapping driver
    // marks the net for resolution (resolved_nets_): Emit then routes every
    // whole-net driver to a slot and folds them (see the resolution section).
    // Coverage is tracked as an unpacked-array element band crossed with a
    // packed bit range, so genuinely disjoint drivers stay single-driver:
    // distinct constant elements (assign mem[0]=.., assign mem[1]=..) and
    // disjoint bits (assign w[0]=.., assign w[1]=..) never mark the net
    // resolved. Anything not provably disjoint (a dynamic index, a
    // whole-net/array driver) covers the whole net and so overlaps every other
    // driver. Procedural writes are not continuous drivers and are not tracked.

    // The multi-driver reducer for a net kind: wire/tri/uwire resolve to X on a
    // 0-vs-1 conflict; wand/triand fold wired-AND, wor/trior wired-OR (LRM 6.6).
    // Other kinds never reach here (collect_scope_signals rejects them).
    static ir::Inst::Tag net_resolve_tag(NetType::NetKind kind) {
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
    static std::optional<int> net_pull_value(NetType::NetKind kind) {
        switch (kind) {
            case NetType::Tri0:
                return 0;
            case NetType::Tri1:
                return 1;
            default:
                return std::nullopt;
        }
    }

    // A driven region of a net: an unpacked-array element band crossed with a
    // packed bit range within one element. A packed (non-array) net uses the
    // single element band [0, 0]; a whole-element or whole-net driver uses the
    // full bit range [0, UINT32_MAX].
    struct NetRegion {
        uint64_t elem_lo, elem_hi;  // linearized unpacked-element ordinals
        uint32_t bit_lo, bit_hi;    // packed bit range within one element
        bool     operator==(const NetRegion&) const = default;
    };

    static constexpr NetRegion kWholeNet{ 0, UINT64_MAX, 0, UINT32_MAX };

    // Record that `reg` of a net gained a continuous driver; fail on overlap
    // with a previously recorded driver. Two regions overlap iff their element
    // bands intersect AND their bit ranges intersect. Non-net symbols are
    // ignored (slang already rejects multiple continuous drivers on variables).
    void record_net_driver_region(const ValueSymbol& raw, const NetRegion& reg) {
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
            if (reg.elem_lo <= p.elem_hi && p.elem_lo <= reg.elem_hi &&
                reg.bit_lo <= p.bit_hi && p.bit_lo <= reg.bit_hi) {
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
    void record_net_driver_whole(const ValueSymbol& sym) {
        record_net_driver_region(sym, kWholeNet);
    }

    // The root value symbol of an lvalue select chain, or null for shapes with
    // no single named root.
    static const ValueSymbol* lvalue_root(const Expression& e) {
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
    static bool is_elaboration_constant(const ValueSymbol& s) {
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
    std::optional<ir::Index> try_lower_constant_select(const Expression& expr, ir::Builder& b,
                                                       ir::SourceLoc l) {
        const ValueSymbol* root = lvalue_root(expr);
        if (root == nullptr || !is_elaboration_constant(*root)) {
            return std::nullopt;
        }
        slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
        slang::ast::EvalContext    ectx(actx);
        const slang::ConstantValue cv = expr.eval(ectx);
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
    ir::Index select_base_lvalue(const Expression& base, ir::Builder& b, ir::SourceLoc l) {
        const ValueSymbol* root = lvalue_root(base);
        if (root != nullptr && is_elaboration_constant(*root) &&
            base.type->getCanonicalType().isIntegral()) {
            slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
            slang::ast::EvalContext    ectx(actx);
            const slang::ConstantValue cv = base.eval(ectx);
            if (!cv.bad()) {
                const ir::Index tmp = make_arg_temp(lower_type(*base.type), b, l);
                b.binary(ir::Inst::Tag::Store, tmp, b.constant(lower_constant(cv, *base.type), l), l);
                return tmp;
            }
        }

        return lower_lvalue(base, b);
    }

    // Record the net-driver coverage of one continuous-assignment lvalue. A
    // concatenation drives each operand; any other rooted lvalue drives the
    // region computed by net_region_of.
    void record_net_driver(const Expression& lhs) {
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
    NetRegion net_region_of(const Expression& lhs, const ValueSymbol& root) {
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
        const slang::ast::Type* et = &root.getType().getCanonicalType();
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
        size_t   t       = 0;  // unpacked dims consumed so far
        uint32_t bit_lo  = 0;
        uint32_t bit_hi  = UINT32_MAX;
        bool     bit_set = false;
        for (const Expression* step : steps) {
            if (t < m) {
                if (step->kind != ExpressionKind::ElementSelect) {
                    return kWholeNet;
                }
                const std::optional<int32_t> c =
                  try_const_int(step->as<ElementSelectExpression>().selector());
                if (!c) {
                    return kWholeNet;
                }
                base += static_cast<uint64_t>(dims[t].translateIndex(*c)) * sp[t + 1];
                ++t;
                continue;
            }
            // Trailing packed select on the element. One constant bit/part/member
            // select narrows; a second (or a non-constant) widens to the whole
            // element (rare, stays a conservative reject within the element).
            if (bit_set) {
                bit_lo = 0;
                bit_hi = UINT32_MAX;
                continue;
            }
            if (const auto br = packed_bit_range(*step)) {
                bit_lo = br->first;
                bit_hi = br->second;
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
    std::optional<std::pair<uint32_t, uint32_t>> packed_bit_range(const Expression& step) {
        switch (step.kind) {
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es = step.as<ElementSelectExpression>();
                if (!es.value().type->isIntegral() || es.type->getBitWidth() != 1) {
                    return std::nullopt;
                }
                if (const std::optional<int32_t> c = try_const_int(es.selector())) {
                    const auto bit =
                      static_cast<uint32_t>(packed_range_of(es.value()).translateIndex(*c));
                    return std::make_pair(bit, bit);
                }
                return std::nullopt;
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs = step.as<RangeSelectExpression>();
                if (!rs.value().type->isIntegral() ||
                    rs.getSelectionKind() != RangeSelectionKind::Simple) {
                    return std::nullopt;
                }
                const std::optional<int32_t> left  = try_const_int(rs.left());
                const std::optional<int32_t> right = try_const_int(rs.right());
                if (!left || !right) {
                    return std::nullopt;
                }
                const slang::ConstantRange range = packed_range_of(rs.value());
                const auto a = static_cast<uint32_t>(range.translateIndex(*left));
                const auto b = static_cast<uint32_t>(range.translateIndex(*right));
                return std::make_pair(std::min(a, b), std::max(a, b));
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

    // -- Multi-driver net resolution (LRM 6.6.1, wire/tri, no strengths) --
    //
    // A net that Classify found to have >= 2 overlapping drivers is "resolved":
    // Emit routes each of its drivers to a private full-width slot signal
    // (alloc_driver_slot) instead of storing the net directly, and
    // emit_wire_resolvers folds all the slots through WireRes into the net at the
    // end of lowering. A single-driver net is never in resolved_nets_ and keeps
    // the plain-store fast path untouched. V1 supports whole-net drivers only.

    // Whether an lvalue is a whole-net reference (the entire net, no element /
    // range / member select). A whole-net driver writes every bit of its slot,
    // so V1 resolution needs no per-bit undriven handling. A select of any kind
    // is a partial driver (deferred).
    static bool is_whole_net_lvalue(const Expression& e) {
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
    std::optional<ir::Signal> resolved_net_of(const Expression& lhs) {
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
    ir::Signal alloc_driver_slot(ir::Signal net, ir::SourceLoc l) {
        const auto net_u = static_cast<uint32_t>(net);
        const auto it    = resolved_scope_.find(net_u);
        if (it != resolved_scope_.end() && it->second != cur_scope_id()) {
            // The drivers of this net live in different instance scopes (e.g. a
            // shared interface net driven from two modules): the slots would span
            // scopes and the resolver could not address them uniformly.
            // TODO: cross-scope multiple drivers (LRM 6.6.1) — the slots and
            // resolver would need a shared home scope.
            fail("multiple drivers of a net across different scopes are not supported");
        }
        auto& slots = resolved_slots_[net_u];
        ir::SignalInfo info;
        info.name = std::string(module_.signal(net).name) + "$drv" + std::to_string(slots.size());
        info.type = module_.signal(net).type;
        info.dir  = ir::SignalInfo::Direction::Internal;
        const ir::Signal slot = register_signal(std::move(info));
        slots.push_back(slot);
        resolved_scope_[net_u] = cur_scope_id();
        resolved_loc_[net_u]   = l;
        return slot;
    }

    // An all-Z constant of `width` bits (VPI planes: val = 0, xz = 1 for every
    // bit). Used as the resolution identity in the undriven bits of a partial
    // driver's slot value.
    ir::Ref z_constant_ref(uint32_t width) {
        const uint32_t      pb = (width + 7) / 8;
        std::vector<uint8_t> bytes(2 * static_cast<size_t>(pb), 0); // val plane = 0
        for (uint32_t bit = 0; bit < width; ++bit) {
            bytes[pb + bit / 8] |= static_cast<uint8_t>(1u << (bit % 8)); // xz plane = 1 (Z)
        }
        return module_.pool.put_value(make_int_type(width, /*is_four=*/true, /*is_signed=*/false),
                                      bytes);
    }

    // The full-width value one driver stages into its slot: for a whole-net
    // driver the driven value itself; for a constant packed partial driver
    // (`w[7:4] = ...`), the value placed at its bit range with Z — the
    // resolution identity — in every other bit, so the resolver folds each bit
    // only across the drivers that actually reach it. `driven` is the already-
    // lowered right-hand value. Shapes V2 cannot slot (dynamic index, unpacked
    // element, or a 2-state net that cannot hold the Z background) fail loudly.
    ir::Index build_driver_slot_value(ir::Signal net, const Expression& left, ir::Index driven,
                                      ir::Builder& b, ir::SourceLoc l) {
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
            return b.resize(driven, net_ty, l);
        }
        const NetRegion reg = root != nullptr ? net_region_of(left, *root) : kWholeNet;
        // A constant packed sub-range: one element band [0,0], a bounded bit
        // range strictly inside the net. Anything else (dynamic index, unpacked-
        // array element, whole-net via select) is not a slot-able partial.
        const bool packed_const = reg.elem_lo == 0 && reg.elem_hi == 0 &&
                                  reg.bit_hi != UINT32_MAX && reg.bit_lo <= reg.bit_hi &&
                                  reg.bit_hi < net_w;
        if (!packed_const) {
            // TODO: dynamic-index / unpacked-array-element partial drivers (a
            // runtime-varying region needs a per-activation Z refill or a
            // per-element resolver).
            fail("dynamic or unpacked-array partial driver of a multiply-driven net is not supported");
        }
        if (module_.pool.layout(net_ty).domain != ir::Domain::Four) {
            // TODO: 2-state resolved nets have no Z to mark undriven bits, so a
            // partial driver cannot leave an identity background.
            fail("partial driver of a 2-state multiply-driven net is not supported");
        }
        const uint32_t region_w = reg.bit_hi - reg.bit_lo + 1;
        const ir::Index rhs = b.resize(driven, make_int_type(region_w, true, false), l);
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
    void reject_concat_resolved_driver(const Expression& lhs) {
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

    // Emit one continuous-assign driver of a resolved net: stage its value (whole
    // net, or a constant packed sub-range with a Z background) into this driver's
    // slot, sensitive to the RHS reads.
    void emit_resolved_driver(ir::Signal net, const AssignmentExpression& a, const Expression& left,
                              ir::Builder& b, ir::SourceLoc l) {
        const ir::Index val  = build_driver_slot_value(net, left, lower_expr(a.right(), b), b, l);
        const ir::Signal slot = alloc_driver_slot(net, l);
        b.binary(ir::Inst::Tag::Store, b.signal(slot, l), val, l);
        add_continuous_process(continuous_events(a), l, b);
    }

    // Fold every resolved net's driver slots through WireRes and store the
    // resolved value into the net. One self-starting process per net, sensitive
    // to all its slots; the net is written only here, so its readers wake on the
    // resolved value. Runs once per top after Emit, when every driver has
    // registered its slot.
    void emit_wire_resolvers() {
        // Every net that needs a resolver: the ones with >= 2 (or, for tri0/tri1,
        // >= 1) drivers, plus driverless tri0/tri1 nets that still pull to 0/1.
        std::vector<uint32_t> nets;
        nets.reserve(resolved_slots_.size() + net_pull_.size());
        for (const auto& [net_u, slots] : resolved_slots_) {
            nets.push_back(net_u);
        }
        for (const auto& [net_u, pull] : net_pull_) {
            if (!resolved_slots_.contains(net_u)) {
                nets.push_back(net_u);
            }
        }
        std::sort(nets.begin(), nets.end()); // deterministic process order

        for (const uint32_t net_u : nets) {
            const ir::Signal net = static_cast<ir::Signal>(net_u);
            const auto       si  = resolved_slots_.find(net_u);
            const std::span<const ir::Signal> slots =
              si != resolved_slots_.end() ? std::span<const ir::Signal>(si->second)
                                          : std::span<const ir::Signal>();
            const auto pi = net_pull_.find(net_u);

            const ir::SourceLoc l = !slots.empty() ? resolved_loc_[net_u]
                                    : pi != net_pull_.end() ? pi->second.loc
                                                            : ir::SourceLoc{ 0 };
            const uint32_t scope  = !slots.empty() ? resolved_scope_[net_u]
                                    : pi != net_pull_.end() ? pi->second.scope
                                                            : ir::Process::no_scope;
            const ir::Inst::Tag op =
              net_resolve_op_.contains(net_u) ? net_resolve_op_[net_u] : ir::Inst::Tag::WireRes;

            ir::Builder b(module_.pool);
            // Fold the driver slots (a driverless tri0/tri1 net seeds all-Z, so
            // the pull sees only Z and fills the whole net with its pull value).
            ir::Index acc;
            if (slots.empty()) {
                acc = b.constant(z_constant_ref(module_.pool.layout(module_.signal(net).type).width), l);
            } else {
                acc = b.unary(ir::Inst::Tag::Load, b.signal(slots[0], l), l);
                for (size_t k = 1; k < slots.size(); ++k) {
                    const ir::Index next = b.unary(ir::Inst::Tag::Load, b.signal(slots[k], l), l);
                    acc                  = b.binary(op, acc, next, l);
                }
            }
            if (pi != net_pull_.end()) {
                acc = b.net_pull(acc, static_cast<uint32_t>(pi->second.value), l);
            }
            b.binary(ir::Inst::Tag::Store, b.signal(net, l), acc, l);

            std::vector<ir::WaitEvent> wait_events;
            wait_events.reserve(slots.size());
            for (const ir::Signal s : slots) {
                wait_events.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
            }
            module_.add_process(ir::Process{ .activation  = ir::Activation::Always,
                                             .wait_events = std::move(wait_events),
                                             .self_start  = true,
                                             .loc         = l,
                                             .ir          = b.finish(),
                                             .scope       = scope });
        }
    }

    // Reject an explicit non-default drive strength on a continuous assignment
    // or net declaration. Strengths only affect multi-driver resolution, which
    // Clotho lacks; an explicit (strong0, strong1) equals the default (LRM
    // 10.3.4) and stays accepted.
    // TODO: strength-based resolution (LRM 6.6.1, 28.11-28.12).
    void check_drive_strength(
      const std::pair<std::optional<DriveStrength>, std::optional<DriveStrength>>& strength,
      std::string_view what) {
        const auto non_default = [](const std::optional<DriveStrength>& s) {
            return s.has_value() && *s != DriveStrength::Strong;
        };
        if (non_default(strength.first) || non_default(strength.second)) {
            fail("drive strength on " + std::string(what) + " is not supported");
        }
    }

    // -- Processes --

    void lower_continuous_assign(const ContinuousAssignSymbol& sym) {
        if (sym.getDelay() != nullptr) {
            // `assign #N lhs = rhs;` — the net-delay is dropped today, so the
            // update would land at the wrong time. Reject until the scheduler exists.
            fail("continuous assignment delay (assign #N) is not supported");
        }
        check_drive_strength(sym.getDriveStrength(), "a continuous assignment");
        const Expression& e = sym.getAssignment();
        if (e.kind != ExpressionKind::Assignment) {
            fail("malformed continuous assignment");
        }
        const AssignmentExpression& a    = e.as<AssignmentExpression>();
        const Expression&           left = a.left();
        if (cur_pass_ == Pass::Classify) {
            record_net_driver(left);
            return;
        }

        ir::Builder b(module_.pool);
        if (const std::optional<ir::Signal> net = resolved_net_of(left)) {
            // Multiply-driven net: this driver stages its value into its own
            // slot; the resolver folds every driver's slot (emit_wire_resolvers).
            emit_resolved_driver(*net, a, left, b, to_source_loc(sym.location));
            return;
        }
        // A concatenation lvalue has no single root, so resolved_net_of misses
        // it; reject it loudly if an operand drives a resolved net rather than
        // silently storing past the resolver.
        reject_concat_resolved_driver(left);
        lower_assign(a, b);

        // A continuous assign is a process sensitive to the source elements of
        // its expression (LRM 4.9.1): every signal the RHS reads, plus any read
        // in an LHS selector (`assign mem[i] = x` is sensitive to i).
        add_continuous_process(continuous_events(e), to_source_loc(sym.location), b);
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
    std::vector<ir::WaitEvent> continuous_events(const Expression& e) {
        std::vector<ir::WaitEvent> events;
        SensitivityCollector       c(sigmap_, SensitivityMode::ImplicitEvent);
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
    void add_continuous_process(std::vector<ir::WaitEvent> wait_events, ir::SourceLoc l, ir::Builder& b) {
        module_.add_process(ir::Process{ .activation  = ir::Activation::Always,
                                         .wait_events = std::move(wait_events),
                                         .self_start  = true,
                                         .loc         = l,
                                         .ir          = b.finish(),
                                         .scope       = cur_scope_id() });
    }

    // A net declaration assignment (`wire w = expr;`) is a continuous assignment
    // (LRM 6.7.1 / 10.3.3), NOT a one-shot initialization like a variable's: slang
    // models it as the net's initializer expression rather than a separate
    // ContinuousAssignSymbol, so it is lowered here into the same kind of process a
    // bare `assign` produces. The initializer is already converted to the net type
    // by slang, so the store needs no extra resize.
    void lower_net_initializer(const NetSymbol& ns) {
        const Expression* init = ns.getInitializer();
        if (init == nullptr) {
            return;
        }
        if (cur_pass_ == Pass::Classify) {
            record_net_driver_whole(ns);
            return;
        }
        const bool resolved = resolved_nets_.contains(static_cast<uint32_t>(signal_of(ns)));
        if (is_aggregate(ns.getType())) {
            if (resolved) {
                // TODO: multi-driver resolution of an unpacked-aggregate net —
                // WireRes folds packed planes only.
                fail("multiple drivers on an aggregate net are not supported");
            }
            // A whole unpacked-struct / aggregate net initializer is a whole-object
            // continuous copy (one process on the source's sensitivity), the same
            // as a bare `assign n = ...` on a struct.
            ir::Builder         ab(module_.pool);
            const ir::SourceLoc al = loc(*init);
            emit_agg_store(value_lvalue(ns, ab, al), lower_type(ns.getType()), *init,
                           ir::Inst::Tag::Store, ab, al);
            add_continuous_process(continuous_events(*init), to_source_loc(ns.location), ab);
            return;
        }
        ir::Builder         b(module_.pool);
        const ir::SourceLoc l = loc(*init);
        if (resolved) {
            // A net-declaration assignment is a whole-net driver: stage the
            // initializer into this driver's slot (emit_wire_resolvers folds it).
            const ir::Signal slot = alloc_driver_slot(signal_of(ns), l);
            b.binary(ir::Inst::Tag::Store, b.signal(slot, l),
                     b.resize(lower_expr(*init, b), lower_type(ns.getType()), l), l);
            add_continuous_process(continuous_events(*init), to_source_loc(ns.location), b);
            return;
        }
        b.binary(ir::Inst::Tag::Store, value_lvalue(ns, b, l), lower_expr(*init, b), l);
        add_continuous_process(continuous_events(*init), to_source_loc(ns.location), b);
    }

    // -- Gate primitives (LRM 28.4, 28.5) --
    //
    // Each gate instance lowers to a self-starting continuous-assign process:
    // the same comb-process path that handles `assign` statements. This reuses
    // the existing thunk elision, comb-fusion, and topo-sort infrastructure
    // with no dedicated gate evaluation engine.
    //
    // Non-trivial terminal expressions (not a plain signal reference or constant
    // bit-select) are first lowered to implicit nets via separate continuous
    // assigns (LRM 4.9.6), same as before.

    // Local gate types — not stored in the module; only used during lowering.
    enum class GateOp : uint8_t { And, Nand, Or, Nor, Xor, Xnor, Buf, Not };
    struct GateTerm  { ir::Signal sig = ir::Signal{0}; uint32_t bit = 0; };
    struct GateInput { ir::Signal sig = ir::Signal{0}; uint32_t bit = 0;
                       uint8_t const_code = 0; bool is_const = false; };

    // Map a built-in gate primitive name to its GateOp; nullopt for primitives
    // outside the supported set.
    static std::optional<GateOp> gate_op_of(std::string_view name) {
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
    static ir::Inst::Tag gate_fold_tag(GateOp op) {
        switch (op) {
            case GateOp::And:
            case GateOp::Nand: return ir::Inst::Tag::Band;
            case GateOp::Or:
            case GateOp::Nor:
            case GateOp::Buf:
            case GateOp::Not:  return ir::Inst::Tag::Bor;
            case GateOp::Xor:
            case GateOp::Xnor: return ir::Inst::Tag::Bxor;
        }
        __builtin_unreachable();
    }

    static bool gate_inverted(GateOp op) {
        return op == GateOp::Nand || op == GateOp::Nor ||
               op == GateOp::Not  || op == GateOp::Xnor;
    }

    // Strip conversion wrappers whose operand is still a packed integral value.
    // Gate terminals are bound against 1-bit logic, so a `bit` net or a typedef'd
    // scalar arrives wrapped in a no-op (for our purposes) conversion.
    static const Expression& unwrap_conversions(const Expression& e) {
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
    std::optional<ir::Signal> gate_select_base(const Expression& value) {
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
    std::optional<uint32_t> gate_select_bit(const Expression& value, const Expression& index) {
        const std::optional<int32_t> idx = try_const_int(index);
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
    std::optional<GateTerm> gate_static_term(const Expression& e) {
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
            const auto bit = sig ? gate_select_bit(es.value(), es.selector()) : std::nullopt;
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
            const auto bit = sig ? gate_select_bit(rs.value(), rs.right()) : std::nullopt;
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
    GateInput lower_gate_input(const Expression& e, std::string_view inst_name, uint32_t input_index) {
        // Constant fold first: literals, parameters, and constant expressions
        // (tie cells) melt into the gate at compile time. The terminal is bound
        // as 1-bit logic, so bit 0 of the folded value is the input; x and z both
        // fold to the X plane code (Z reads as X, LRM Table 28-3).
        {
            slang::ast::ASTContext     astCtx(*scope_, slang::ast::LookupLocation::max);
            slang::ast::EvalContext    evalCtx(astCtx);
            const slang::ConstantValue cv = e.eval(evalCtx);
            if (cv.isInteger()) {
                const slang::logic_t bit  = cv.integer()[0];
                const uint8_t        code = bit.isUnknown() ? 0b10 : (bit.value != 0 ? 0b11 : 0b00);

                return GateInput{ .const_code = code, .is_const = true };
            }
        }

        if (const auto term = gate_static_term(e)) {
            return GateInput{ .sig = term->sig, .bit = term->bit };
        }

        // Implicit continuous assign to an implicit net: temp = expr, sensitive
        // to the expression's reads, evaluated at time zero like any continuous
        // assign (mirrors lower_continuous_assign).
        ir::SignalInfo info;
        info.name = prefix_ + "$gate" + std::to_string(temp_id_++) + "." + std::string(inst_name) + ".in" +
                    std::to_string(input_index);
        info.type            = module_.pool.logic_type;
        const ir::Signal tmp = module_.add_signal(std::move(info));

        ir::Builder         b(module_.pool);
        const ir::SourceLoc l = loc(e);
        b.binary(ir::Inst::Tag::Store, b.signal(tmp, l), b.resize(lower_expr(e, b), module_.pool.logic_type, l), l);

        std::vector<ir::WaitEvent> wait_events;
        SensitivityCollector       c(sigmap_, SensitivityMode::ImplicitEvent);
        e.visit(c);
        for (const ir::Signal s : c.reads) {
            wait_events.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
        }
        module_.add_process(ir::Process{ .activation  = ir::Activation::Always,
                                         .wait_events = std::move(wait_events),
                                         .self_start  = true,
                                         .loc         = l,
                                         .ir          = b.finish(),
                                         .scope       = cur_scope_id() });

        return GateInput{ .sig = tmp, .bit = 0 };
    }

    // Lower one gate output terminal. slang wraps each output connection in an
    // AssignmentExpression whose left() is the driven lvalue; only a static 1-bit
    // location on a 4-state signal is accepted (a gate can drive X, which a
    // 2-state slot cannot hold).
    GateTerm lower_gate_output(const Expression& conn) {
        if (conn.kind != ExpressionKind::Assignment) {
            fail("malformed gate output terminal");
        }
        const Expression& lhs = conn.as<AssignmentExpression>().left();
        if (!lhs.type->isFourState()) {
            // TODO: gate outputs on 2-state nets need an X-coercion policy.
            fail("gate output on a 2-state net is not supported");
        }
        const auto term = gate_static_term(lhs);
        if (!term) {
            // TODO: concatenation / part-select / unpacked-element gate output
            // lvalues (rare in netlists).
            fail("unsupported gate output terminal");
        }

        return *term;
    }

    void lower_gate(const PrimitiveInstanceSymbol& gi) {
        const PrimitiveSymbol& prim = gi.primitiveType;
        const auto             op   = gate_op_of(prim.name);
        const bool             supported_kind =
            prim.primitiveKind == PrimitiveSymbol::NInput || prim.primitiveKind == PrimitiveSymbol::NOutput;
        if (!op || !supported_kind) {
            // TODO: tri-state gates (bufif*/notif*), MOS / bidirectional
            // switches, pullup/pulldown (need Z generation and driver strength
            // resolution) and user-defined primitives.
            fail("gate primitive '" + std::string(prim.name) + "' is not supported");
        }
        if (gi.getDelay() != nullptr) {
            // TODO: gate delays (rise/fall pairs, inertial semantics, LRM 28.16).
            fail("gate delay is not supported");
        }
        const auto [s0, s1] = gi.getDriveStrength();
        if ((s0 && *s0 != DriveStrength::Strong) || (s1 && *s1 != DriveStrength::Strong)) {
            // TODO: drive strengths (needs multi-driver resolution). The default
            // (strong0, strong1), written out explicitly, is accepted as-is.
            fail("gate drive strength is not supported");
        }

        const auto conns = gi.getPortConnections();
        if (conns.size() < 2) {
            // slang has already diagnosed a malformed terminal list.
            fail("malformed gate terminal list");
        }

        // NInput: terminal 0 is the output, the rest are inputs. NOutput (buf /
        // not): the last terminal is the input, the rest are outputs — one
        // independent process per output terminal.
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
                }
            }
            return;
        }
        for (const Expression* out : output_span) {
            if (out->kind == ExpressionKind::Assignment &&
                resolved_net_of(out->as<AssignmentExpression>().left())) {
                // TODO: fold gate outputs into WireRes resolution (they already
                // produce a 1-bit logic value, like a continuous assign).
                fail("gate primitive driving a multiply-driven net is not supported");
            }
        }

        std::vector<GateInput> inputs;
        inputs.reserve(input_span.size());
        for (size_t i = 0; i < input_span.size(); ++i) {
            inputs.push_back(lower_gate_input(*input_span[i], gi.name, static_cast<uint32_t>(i)));
        }

        const ir::SourceLoc loc = to_source_loc(gi.location);
        for (const Expression* out : output_span) {
            emit_gate_as_process(*op, lower_gate_output(*out), inputs, loc);
        }
    }

    // Lower one gate output as a continuous-assign process: fold inputs with the
    // gate operation, store to the output bit, subscribe to input signal changes.
    void emit_gate_as_process(GateOp op, GateTerm output,
                              std::span<const GateInput> inputs, ir::SourceLoc l) {
        ir::Builder   b(module_.pool);
        const ir::Ref logic1 = module_.pool.logic_type;

        // Intern a 1-bit logic constant from a gate const_code (must=bit0, may=bit1).
        // Pool VPI encoding: val=may, xz=may&~must (bytes: val-plane then xz-plane).
        auto make_gate_const = [&](uint8_t code) -> ir::Index {
            switch (code & 3) {
                case 0b00: return b.constant(module_.pool.put_int(logic1, 0), l);
                case 0b11: return b.constant(module_.pool.put_int(logic1, 1), l);
                default: {
                    // X (and Z mapped to X, LRM Table 28-3): val=1, xz=1.
                    const uint8_t xbytes[2] = {0x01, 0x01};
                    return b.constant(module_.pool.put_value(logic1, {xbytes, 2}), l);
                }
            }
        };

        // lvalue for bit `bit` of `sig`: direct for a 1-bit signal, BitSelect
        // for a bit of a wider packed signal.
        const ir::Ref u32 = make_int_type(32, false, false);
        auto bit_lv = [&](ir::Signal sig, uint32_t bit) -> ir::Index {
            ir::Index s = b.signal(sig, l);
            if (module_.pool.layout(module_.signal(sig).type).width == 1) {
                return s;
            }
            return b.binary(ir::Inst::Tag::BitSelect, s,
                            b.constant(module_.pool.put_int(u32, bit), l), l);
        };

        // Fold inputs using the gate's binary operation.
        std::optional<ir::Index> acc;
        const ir::Inst::Tag      fold = gate_fold_tag(op);
        for (const GateInput& in : inputs) {
            ir::Index val = in.is_const ? make_gate_const(in.const_code)
                                        : b.unary(ir::Inst::Tag::Load, bit_lv(in.sig, in.bit), l);
            acc = acc ? b.binary(fold, *acc, val, l) : val;
        }
        if (!acc) {
            // 0-input degenerate gate: output X.
            const uint8_t xbytes[2] = {0x01, 0x01};
            acc = b.constant(module_.pool.put_value(logic1, {xbytes, 2}), l);
        }
        if (gate_inverted(op)) {
            acc = b.unary(ir::Inst::Tag::Bnot, *acc, l);
        }

        b.binary(ir::Inst::Tag::Store, bit_lv(output.sig, output.bit),
                 b.resize(*acc, logic1, l), l);

        // Sensitivity: unique non-const input signals, any-change.
        std::vector<ir::WaitEvent>                      wait_events;
        ankerl::unordered_dense::set<uint32_t>          seen;
        for (const GateInput& in : inputs) {
            if (!in.is_const && seen.insert(static_cast<uint32_t>(in.sig)).second) {
                wait_events.push_back(ir::WaitEvent{ .signal = in.sig, .edge = ir::Edge::Any });
            }
        }
        add_continuous_process(std::move(wait_events), l, b);
    }

    // Wire one child instance's ports to the parent. Each connection becomes a
    // copy process across the boundary, carrying inferred sensitivity so the value
    // propagates on change (it must not inherit the empty-wait-list gap of plain
    // continuous assigns). Both sides' signals are already registered: the child's
    // by the just-completed recursion, the parent's by this body's collect_signals.
    //
    // slang models a connection asymmetrically: an output yields a full
    // AssignmentExpression (sink = source, with width conversions baked in), while
    // an input yields the bare source rvalue to drive the child net with.
    void wire_ports(const InstanceSymbol& child) {
        for (const PortConnection* pc : child.getPortConnections()) {
            if (pc->port.kind == SymbolKind::InterfacePort) {
                // An interface port carries no value to copy: the interface
                // instance owns the storage and every reference to it (plain,
                // modport, or hierarchical) resolves to that shared ir::Signal
                // via canonical_value_symbol (LRM 25, docs/design/interfaces.md).
                // Nothing to wire here.
                continue;
            }
            if (pc->port.kind != SymbolKind::Port) {
                // TODO: any other non-value port kind.
                fail("unsupported port connection kind");
            }
            const PortSymbol& p = pc->port.as<PortSymbol>();
            if (p.direction == slang::ast::ArgumentDirection::InOut ||
                p.direction == slang::ast::ArgumentDirection::Ref) {
                // TODO: inout / ref ports — bidirectional, needs net resolution
                // (or net-collapsing) rather than a one-way copy.
                fail("inout/ref port connections are not supported");
            }
            if (p.internalSymbol == nullptr) {
                // TODO: a port with no internal net (e.g. an unnamed expression
                // port) — has no signal storage to copy to/from.
                fail("port without an internal net is not supported");
            }
            const Expression* expr = pc->getExpression();
            if (expr == nullptr) {
                // Unconnected port (LRM 23.3.2.7). An unconnected output discards
                // the child's driven value: nothing in the parent reads it, so emit
                // no copy process. An unconnected input leaves the child's internal
                // net with no driver, so it holds the undriven default from
                // init_state (z for a 4-state net per LRM 6.6, x for a variable,
                // 0 for 2-state). A port carrying an explicit default value
                // arrives here with a non-null expression, so a null expression
                // means there is genuinely nothing to drive.
                continue;
            }

            if (cur_pass_ == Pass::Classify) {
                // The connection is a continuous driver: an output drives the
                // parent-side lvalue, an input drives the whole child net.
                if (expr->kind == ExpressionKind::Assignment) {
                    record_net_driver(expr->as<AssignmentExpression>().left());
                } else {
                    record_net_driver_whole(p.internalSymbol->as<ValueSymbol>());
                }
                continue;
            }

            const ir::Signal           child_sig = signal_of(p.internalSymbol->as<ValueSymbol>());
            const ir::SourceLoc        l         = to_source_loc(p.location);
            ir::Builder                b(module_.pool);
            std::vector<ir::WaitEvent> sens;
            if (expr->kind == ExpressionKind::Assignment) {
                // Output: parent_target = child_net. slang gives an assignment whose
                // LHS is the parent target and whose RHS is an EmptyArgument
                // placeholder for the port value, so read the child net directly and
                // resize it to the target width. Wakes on the child net.
                const AssignmentExpression& a = expr->as<AssignmentExpression>();
                if (const std::optional<ir::Signal> net = resolved_net_of(a.left())) {
                    // The parent net has multiple drivers: stage this child's
                    // output (whole net, or a constant sub-range with a Z
                    // background) into its own slot; the resolver folds them.
                    if (is_aggregate(*a.left().type)) {
                        // TODO: aggregate driver of a multiply-driven net.
                        fail("aggregate driver of a multiply-driven net is not supported");
                    }
                    const ir::Ref   ty        = lower_type(*a.left().type);
                    const ir::Index child_val = coerce(
                      b.unary(ir::Inst::Tag::Load, b.signal(child_sig, l), l), *a.right().type, ty, b, l);
                    const ir::Index  val  = build_driver_slot_value(*net, a.left(), child_val, b, l);
                    const ir::Signal slot = alloc_driver_slot(*net, l);
                    b.binary(ir::Inst::Tag::Store, b.signal(slot, l), val, l);
                    sens.push_back(ir::WaitEvent{ .signal = child_sig, .edge = ir::Edge::Any });
                    add_continuous_process(std::move(sens), l, b);
                    continue;
                }
                if (is_aggregate(*a.left().type)) {
                    // A whole unpacked-struct / aggregate port: the copy is a
                    // per-leaf (or memcpy) whole-object copy, not a scalar store.
                    // Port types are equivalent (LRM 23.3.3), so both sides share
                    // the recursive storage layout.
                    emit_port_agg_copy(lower_lvalue(a.left(), b), b.signal(child_sig, l),
                                       lower_type(*a.left().type), b, l);
                } else {
                    const ir::Ref   ty  = lower_type(*a.left().type);
                    const ir::Index val = b.unary(ir::Inst::Tag::Load, b.signal(child_sig, l), l);
                    b.binary(ir::Inst::Tag::Store, lower_lvalue(a.left(), b),
                             coerce(val, *a.right().type, ty, b, l), l);
                }
                sens.push_back(ir::WaitEvent{ .signal = child_sig, .edge = ir::Edge::Any });
            } else {
                // Input: drive the child net from the source rvalue, resized to the
                // port width; wakes on the source's reads. The copy is a continuous
                // driver of the whole child-side net.
                if (resolved_nets_.contains(static_cast<uint32_t>(child_sig))) {
                    // The child net is driven both by this input copy (parent
                    // scope) and internally (child scope). Cross-scope resolution
                    // is not yet supported: the driver slots would span two scopes.
                    // TODO: cross-scope multi-driver resolution (input port + an
                    // internal driver of the same net).
                    fail("a net driven by both an input port and an internal driver is not supported");
                }
                const ir::Ref ty = lower_type(p.internalSymbol->as<ValueSymbol>().getType());
                if (is_aggregate(p.internalSymbol->as<ValueSymbol>().getType())) {
                    // A whole unpacked-struct / aggregate input port: whole-object
                    // copy of the source into the child net.
                    const Expression& src = unwrap_conversion(*expr);
                    if (!is_struct_lvalue_shape(src)) {
                        // TODO: an aggregate port driven by a `'{...}` pattern / call
                        // (needs a materialized temp, like an aggregate call actual).
                        fail("aggregate port connection must be a variable");
                    }
                    emit_port_agg_copy(b.signal(child_sig, l), lower_lvalue(src, b), ty, b, l);
                } else {
                    b.binary(ir::Inst::Tag::Store, b.signal(child_sig, l),
                             coerce(lower_expr(*expr, b), *expr->type, ty, b, l), l);
                }
                collect_expr_reads(*expr, sens);
            }
            // A port connection is a continuous copy: evaluate at time zero so the
            // child sees the parent's declaration-initialized value even when the
            // parent signal never changes afterward (LRM 23.3.3).
            add_continuous_process(std::move(sens), l, b);
        }
    }

    // Append the sensitivity of an rvalue expression (each signal it reads, as an
    // any-change wait) to `out`. Mirrors collect_inferred_sensitivity for an
    // expression with no writes.
    void collect_expr_reads(const Expression& expr, std::vector<ir::WaitEvent>& out) {
        // A port connection is a continuous copy (LRM 23.3.3), so it follows the
        // continuous-assign sensitivity rule: function-internal reads do not
        // contribute (ImplicitEvent), only the expression's own operands and any
        // call arguments.
        SensitivityCollector c(sigmap_, SensitivityMode::ImplicitEvent);
        expr.visit(c);
        for (const ir::Signal s : c.reads) {
            if (c.written.contains(static_cast<uint32_t>(s))) {
                continue;
            }
            if (is_string_ref(module_.signal(s).type)) {
                // TODO: string signals in inferred sensitivity — string stores
                // skip wait-queue servicing, so the process would never
                // re-trigger.
                fail("string in inferred sensitivity is not supported");
            }
            out.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
        }
    }

    void lower_procedural_block(const ProceduralBlockSymbol& sym) {
        ir::Activation             activation = ir::Activation::Always;
        std::vector<ir::WaitEvent> wait_events;

        if (sym.procedureKind == ProceduralBlockKind::Initial) {
            activation = ir::Activation::Initial;
        } else if (sym.procedureKind == ProceduralBlockKind::Final) {
            // A `final` block runs once at simulation end (LRM 9.2.3). The backend
            // runs it, sensitivity-less, on both exit paths after every other
            // process settles. Lowered like an `initial` body; the zero-time
            // requirement (no timing controls) is checked after lowering below.
            activation = ir::Activation::Final;
        }

        // TODO: no non-sequential-behavior warning for `always_ff`. LRM 9.2.2.4
        // says tools should warn when the body does not represent sequential
        // logic, but slang's analysis library has no such diagnostic (unlike
        // inferred-latch / inferred-comb for the other two kinds), so nothing
        // surfaces it today. slang does enforce the hard 9.2.2.4 rules (exactly
        // one event control, no blocking timing controls).

        const Statement* body = &sym.getBody();

        // `always_comb` / `always_latch` infer sensitivity from the body's reads
        // minus its writes (9.2.2.2.1, legal because of the single-writer rule).
        // `always @*` carries an ImplicitEvent timing wrapping the body, inferred
        // under the subtly different 9.4.2.2 rules (see SensitivityMode).
        const bool comb = sym.procedureKind == ProceduralBlockKind::AlwaysComb ||
                          sym.procedureKind == ProceduralBlockKind::AlwaysLatch;
        bool       infer = comb;

        // A top-level event control on an `always` is the process's sensitivity
        // list: collect its wait events here and lower the statement underneath.
        // On an `initial` the same syntax is a one-shot blocking wait (LRM 9.4):
        // hoisting it as sensitivity would run the statement unblocked at time
        // zero, so it is left in the body and lowers to a Wait through
        // lower_stmt's Timed case, exactly like a begin/end-wrapped body.
        if (activation == ir::Activation::Always && body->kind == StatementKind::Timed) {
            const TimedStatement& timed = body->as<TimedStatement>();
            if (timed.timing.kind == TimingControlKind::ImplicitEvent) {
                infer = true;
                body  = &timed.stmt;
            } else if (is_event_control(timed.timing)) {
                collect_wait_events(timed.timing, wait_events);
                body = &timed.stmt;
            }
        }

        // An `always` is semantically a forever loop around its body (LRM 9.2.2).
        // Clotho models that loop implicitly when the body re-triggers on a
        // sensitivity list (event control above) or the block is combinational
        // (always_comb / always_latch / @*, inferred below). Any other `always` —
        // driven by a `#delay` or an internal `@`, or with no timing at all — has
        // no sensitivity to re-trigger it, so the forever loop must live in its
        // own body. A body with no internal timing control is a legal but
        // pathological zero-delay infinite loop (it hangs at time zero);
        // we lower it faithfully rather than reject it.
        const bool looping = activation == ir::Activation::Always && !infer && wait_events.empty();

        ir::Builder b(module_.pool);
        if (looping) {
            lower_forever(*body, b);
        } else {
            lower_stmt(*body, b);
        }
        // Inferred sensitivity is read from the body after lowering, so any
        // block-local declarations it touched are already registered in sigmap_.
        if (infer) {
            collect_inferred_sensitivity(
              *body, comb ? SensitivityMode::Comb : SensitivityMode::ImplicitEvent, wait_events);
        }
        // always_comb / always_latch evaluate once at time zero (LRM 9.2.2.2), so
        // they settle from declaration-initialized values. An `always @*` or an
        // explicit `@(...)` list is a plain sensitivity list that runs only when a
        // listed signal changes, matching Verilog, so it is left non-self-starting.
        const bool self_start = sym.procedureKind == ProceduralBlockKind::AlwaysComb ||
                                sym.procedureKind == ProceduralBlockKind::AlwaysLatch;
        ir::Ir body_ir = b.finish();
        // A `final` block is zero-time (LRM 9.2.3): it must not suspend. slang
        // rejects a time-consuming statement in a `final` at elaboration, so this
        // guard is defense in depth (never silently accept an
        // unimplemented case) — reached only if that check ever regresses.
        if (activation == ir::Activation::Final) {
            for (size_t i = 0; i < body_ir.inst_count(); ++i) {
                const ir::Inst::Tag t = body_ir.tag(static_cast<ir::Index>(i));
                if (t == ir::Inst::Tag::Delay || t == ir::Inst::Tag::Wait) {
                    fail("timing control in a final block is not legal (LRM 9.2.3)");
                }
            }
        }
        module_.add_process(ir::Process{ .activation  = activation,
                                         .wait_events = std::move(wait_events),
                                         .self_start  = self_start,
                                         .loc         = to_source_loc(sym.location),
                                         .ir          = std::move(body_ir),
                                         .scope       = cur_scope_id() });
    }

    // True if `tc` is an @-style event control (the kinds usable as a process's
    // sensitivity), as opposed to a delay (#) or other timing. Only these are
    // consumed as sensitivity; everything else reaches lower_stmt's Timed case.
    static bool is_event_control(const TimingControl& tc) {
        switch (tc.kind) {
            case TimingControlKind::SignalEvent:
            case TimingControlKind::EventList:
            case TimingControlKind::ImplicitEvent:
                return true;
            default:
                return false;
        }
    }

    // Collect the signal events of a timing control into `out`, descending into
    // event lists (@(posedge clk or a or negedge rst)). Each SignalEventControl
    // contributes one wait event; an implicit (@*) or other control adds nothing.
    void collect_wait_events(const TimingControl& tc, std::vector<ir::WaitEvent>& out) {
        switch (tc.kind) {
            case TimingControlKind::SignalEvent: {
                const SignalEventControl& ev = tc.as<SignalEventControl>();
                // TODO: `@(edge sig iff cond)` (LRM 9.4.2.3) — the wake must be
                // gated on the condition at event time; dropping it fires the
                // process regardless of the guard.
                if (ev.iffCondition != nullptr) {
                    fail("iff qualifier on an event control is not supported");
                }
                const ir::Signal sig = event_signal(ev.expr);
                if (ev.edge == slang::ast::EdgeKind::BothEdges) {
                    // `edge` means posedge-or-negedge (LRM 9.4.2), not any change:
                    // an x<->z transition is neither and must not wake. Subscribe
                    // both edge buckets; the scheduled flag dedups a double wake.
                    out.push_back(ir::WaitEvent{ .signal = sig, .edge = ir::Edge::Positive });
                    out.push_back(ir::WaitEvent{ .signal = sig, .edge = ir::Edge::Negative });
                } else {
                    out.push_back(ir::WaitEvent{ .signal = sig, .edge = to_edge(ev.edge) });
                }
                break;
            }
            case TimingControlKind::EventList:
                for (const TimingControl* e : tc.as<EventListControl>().events) {
                    collect_wait_events(*e, out);
                }
                break;
            default:
                // ImplicitEvent (@*) and the like: no explicit signals to collect.
                break;
        }
    }

    // Build the inferred sensitivity of a combinational process body and append
    // it to `out`, each signal as a level-sensitive (any-change) wait. Under
    // Comb rules (always_comb / always_latch) signals the body writes are
    // subtracted (9.2.2.2.1 (b)); under ImplicitEvent (@*) rules a signal read
    // anywhere stays sensitive even if also written — LRM 9.4.2.2's example
    // makes `tmp1 = a & b; y = tmp1;` sensitive to tmp1, which another process
    // may legally write (@* permits multiple drivers).
    void collect_inferred_sensitivity(const Statement& body, SensitivityMode mode,
                                      std::vector<ir::WaitEvent>& out) {
        SensitivityCollector c(sigmap_, mode);
        body.visit(c);
        for (const ir::Signal s : c.reads) {
            if (mode == SensitivityMode::Comb && c.written.contains(static_cast<uint32_t>(s))) {
                continue;
            }
            if (is_string_ref(module_.signal(s).type)) {
                // TODO: string signals in inferred (@*) sensitivity — string
                // stores skip wait-queue servicing, so the process would
                // never re-trigger.
                fail("string in inferred combinational sensitivity is not supported");
            }
            out.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
        }
    }

    // Map a slang edge kind to the IR edge. Only None (a level wait) reaches the
    // default; BothEdges is expanded to two wait events by collect_wait_events.
    static ir::Edge to_edge(slang::ast::EdgeKind edge) {
        switch (edge) {
            case slang::ast::EdgeKind::PosEdge:
                return ir::Edge::Positive;
            case slang::ast::EdgeKind::NegEdge:
                return ir::Edge::Negative;
            default:
                return ir::Edge::Any;
        }
    }

    ir::Signal event_signal(const Expression& e) {
        if (is_string_expr(e)) {
            // TODO: waiting on a string change (needs string-aware wait-queue
            // servicing; string stores skip edge detection).
            fail("string in an event control is not supported");
        }
        if (e.kind == ExpressionKind::NamedValue || e.kind == ExpressionKind::HierarchicalValue) {
            // Waiting on a hierarchical signal (`@(posedge top.dut.clk)`, LRM 23.6)
            // subscribes to the target instance's signal directly.
            return signal_of(e.as<ValueExpressionBase>().symbol);
        }
        fail("unsupported event expression");
    }

    // Lower an intra-body event control `@(...)` to a Wait instruction. A
    // multi-signal list (@(a or posedge b)) registers the process on every listed
    // (signal, edge) queue at once; it wakes when any of them fires.
    void lower_event_wait(const TimingControl& tc, ir::Builder& b, ir::SourceLoc l) {
        std::vector<ir::WaitEvent> events;
        collect_wait_events(tc, events);
        if (events.empty()) {
            fail("intra-body event wait has no signal events");
        }
        std::vector<ir::WaitEdge> edges;
        edges.reserve(events.size());
        for (const ir::WaitEvent& e : events) {
            edges.push_back(ir::WaitEdge{ e.signal, static_cast<uint32_t>(e.edge) });
        }
        b.wait(edges, l);
    }

    // Emit the suspend for a blocking intra-assignment timing control
    // (`lhs = #d rhs` / `lhs = @e rhs` / `lhs = repeat(N) @e rhs`, LRM 9.4.5).
    // The caller has already sampled the RHS; this blocks the process on the
    // delay/event, after which the caller stores the sampled value. A repeat
    // event control waits for its child event N times (count evaluated once,
    // N <= 0 falls through with no wait). The nonblocking form is not routed
    // here — it needs a detached scheduled update (see lower_assign).
    void lower_intra_timing(const TimingControl& tc, ir::Builder& b, ir::SourceLoc l) {
        switch (tc.kind) {
            case TimingControlKind::Delay:
                b.unary(ir::Inst::Tag::Delay, scaled_delay(tc.as<DelayControl>().expr, b), l);
                return;
            case TimingControlKind::SignalEvent:
            case TimingControlKind::EventList:
                lower_event_wait(tc, b, l);
                return;
            case TimingControlKind::RepeatedEvent: {
                const RepeatedEventControl& rec = tc.as<RepeatedEventControl>();
                if (!is_event_control(rec.event)) {
                    fail("repeat intra-assignment timing requires an event control");
                }
                if (rec.expr.type->isFloating()) {
                    fail("repeat with a real count is not supported");
                }
                const ir::Ref    ty      = lower_type(*rec.expr.type);
                const ir::Signal bound   = declare_temp(ty, "repeat_bound");
                const ir::Signal counter = declare_temp(ty, "repeat_count");
                b.binary(ir::Inst::Tag::Store, b.signal(bound, l),
                         b.resize(lower_expr(rec.expr, b), ty, l), l);
                b.binary(ir::Inst::Tag::Store, b.signal(counter, l),
                         b.constant(module_.pool.put_int(ty, 0), l), l);

                b.open_region();
                const ir::Index cur  = b.unary(ir::Inst::Tag::Load, b.signal(counter, l), l);
                const ir::Index lim  = b.unary(ir::Inst::Tag::Load, b.signal(bound, l), l);
                const ir::Index pred = b.binary(ir::Inst::Tag::Lt, cur, lim, l);
                const ir::ExtraSlice cond = b.close_region();

                b.open_region();
                lower_event_wait(rec.event, b, l);
                const ir::ExtraSlice body = b.close_region();

                b.open_region();
                const ir::Index sc   = b.unary(ir::Inst::Tag::Load, b.signal(counter, l), l);
                const ir::Index one  = b.constant(module_.pool.put_int(ty, 1), l);
                b.binary(ir::Inst::Tag::Store, b.signal(counter, l),
                         b.binary(ir::Inst::Tag::Add, sc, one, l), l);
                const ir::ExtraSlice step = b.close_region();

                b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
                return;
            }
            default:
                fail("unsupported intra-assignment timing control");
        }
    }

    // Resolve the target of `-> e` to the event's synthetic signal, verifying it
    // is an event (defensive — slang types the target as an event already).
    ir::Signal trigger_target(const Expression& target) {
        if (target.kind != ExpressionKind::NamedValue || !target.type->isEvent()) {
            // TODO: triggering an event element of an array / struct member, or an
            // event through a hierarchical / handle expression (Tier 2).
            fail("unsupported event trigger target");
        }
        return signal_of(target.as<NamedValueExpression>().symbol);
    }

    // Lower `-> e` / `->> e` / `->> #d e` (LRM 15.5.2, 15.5.4).
    void lower_event_trigger(const EventTriggerStatement& ev, ir::Builder& b) {
        const ir::Signal sig = trigger_target(ev.target);
        if (ev.timing != nullptr) {
            // Only `->>` carries a timing control (grammar), so a delayed trigger
            // is always nonblocking. A fixed delay `->> #d e` schedules a detached
            // wheel timer; the event-controlled form is not yet lowered.
            if (ev.timing->kind == slang::ast::TimingControlKind::Delay) {
                const Expression& amount = ev.timing->as<slang::ast::DelayControl>().expr;
                b.event_trigger_nb_delayed(sig, scaled_delay(amount, b), loc(ev));
                return;
            }
            // TODO: event-controlled nonblocking trigger (`->> @(x) e`,
            // `->> repeat(n) @(x) e`) — needs a detached one-shot event
            // subscription (docs/design/named-events.md, Tier 2 Form 2).
            fail("event-controlled nonblocking trigger ('->> @(...) e') is not supported");
        }
        if (ev.isNonBlocking) {
            b.event_trigger_nb(sig, loc(ev)); // `->> e`: wake in the NBA region
        } else {
            b.event_trigger(sig, loc(ev)); // `-> e`: wake now
        }
    }

    // If `e` is an `<event>.triggered` system-method call (LRM 15.5.3), return the
    // event operand; else nullptr. slang models the property as a system method
    // whose receiver is arguments()[0].
    const Expression* triggered_event(const Expression& e) {
        if (e.kind != ExpressionKind::Call) {
            return nullptr;
        }
        const CallExpression& call = e.as<CallExpression>();
        if (!call.isSystemCall() || call.getSubroutineName() != "triggered") {
            return nullptr;
        }
        if (call.arguments().empty() || !call.arguments()[0]->type->isEvent()) {
            return nullptr;
        }
        return call.arguments()[0];
    }

    // NOTE: `wait(...)` is lowered by the general `lower_wait_stmt` below (from
    // the wait(expr) feature). It subsumes `wait(e.triggered)`: the read set of
    // the condition includes the event, so `-> e` wakes the wait and the
    // re-tested `.triggered` (stamped this instant) ends it.

    // -- Statements --

    void lower_stmt(const Statement& stmt, ir::Builder& b) {
        switch (stmt.kind) {
            case StatementKind::List:
                for (const Statement* s : stmt.as<StatementList>().list) {
                    lower_stmt(*s, b);
                }
                break;
            case StatementKind::Block: {
                const BlockStatement& blk = stmt.as<BlockStatement>();
                if (blk.blockKind != StatementBlockKind::Sequential) {
                    lower_fork(blk, b);
                    break;
                }
                const bool named = blk.blockSymbol != nullptr && !blk.blockSymbol->name.empty();
                if (named) {
                    named_blocks_.push_back(blk.blockSymbol->name);
                }
                lower_stmt(blk.body, b);
                if (named) {
                    named_blocks_.pop_back();
                }
                break;
            }
            case StatementKind::Timed: {
                // A timing control nested inside a process body. A delay (#n) lowers
                // to a Delay instruction (suspend + reschedule on the timing wheel);
                // an event wait (@(edge sig)) lowers to a Wait instruction (suspend
                // on the signal's wait queue). Either way the body underneath runs
                // once the process resumes. (A process's top-level @(edge)
                // sensitivity is consumed in lower_procedural_block, not here.)
                const TimedStatement& timed = stmt.as<TimedStatement>();
                if (timed.timing.kind == TimingControlKind::Delay) {
                    const Expression& amount = timed.timing.as<DelayControl>().expr;
                    b.unary(ir::Inst::Tag::Delay, scaled_delay(amount, b), loc(stmt));
                } else if (is_event_control(timed.timing)) {
                    lower_event_wait(timed.timing, b, loc(stmt));
                } else {
                    fail("unsupported timing control inside a process body");
                }
                lower_stmt(timed.stmt, b);
                break;
            }
            case StatementKind::ExpressionStatement: {
                const Expression& e = stmt.as<ExpressionStatement>().expr;
                if (e.kind == ExpressionKind::Assignment) {
                    lower_assign(e.as<AssignmentExpression>(), b);
                } else if (e.kind == ExpressionKind::Call) {
                    lower_call(e.as<CallExpression>(), b);
                } else if (e.kind == ExpressionKind::UnaryOp) {
                    // A bare `i++;` / `--i;` statement: bump and discard the value.
                    lower_unary(e.as<UnaryExpression>(), b);
                } else {
                    fail("unsupported expression statement");
                }
                break;
            }
            case StatementKind::Conditional:
                lower_conditional(stmt.as<ConditionalStatement>(), b);
                break;
            case StatementKind::Empty:
                break;
            case StatementKind::VariableDeclaration:
                lower_var_decl(stmt.as<VariableDeclStatement>().symbol, b);
                break;
            case StatementKind::Case:
                lower_case(stmt.as<CaseStatement>(), b);
                break;
            case StatementKind::ForLoop:
                lower_for(stmt.as<ForLoopStatement>(), b);
                break;
            case StatementKind::WhileLoop:
                lower_while(stmt.as<WhileLoopStatement>(), b);
                break;
            case StatementKind::ForeverLoop:
                lower_forever(stmt.as<ForeverLoopStatement>().body, b);
                break;
            case StatementKind::RepeatLoop:
                lower_repeat(stmt.as<RepeatLoopStatement>(), b);
                break;
            case StatementKind::DoWhileLoop:
                lower_do_while(stmt.as<DoWhileLoopStatement>(), b);
                break;
            case StatementKind::ForeachLoop:
                lower_foreach(stmt.as<ForeachLoopStatement>(), b);
                break;
            case StatementKind::ImmediateAssertion:
                lower_immediate_assertion(stmt.as<ImmediateAssertionStatement>(), b);
                break;
            case StatementKind::Break:
                b.break_(loc(stmt));
                break;
            case StatementKind::Continue:
                b.continue_(loc(stmt));
                break;
            case StatementKind::Return:
                lower_return(stmt.as<ReturnStatement>(), b);
                break;
            case StatementKind::EventTrigger:
                lower_event_trigger(stmt.as<EventTriggerStatement>(), b);
                break;
            case StatementKind::Wait:
                lower_wait_stmt(stmt.as<WaitStatement>(), b);
                break;
            case StatementKind::Disable:
            case StatementKind::DisableFork:
                // TODO: disable — unwinding a suspended task coroutine and clearing
                // its wheel / wait-queue registrations is not modeled yet.
                fail("disable is not supported");
            default:
                fail("unsupported statement");
        }
    }

    // Lower `fork … join` (LRM 9.3.2). V1: `join` only, in a process context.
    // Each top-level child statement becomes its own fork-child coroutine; the
    // Fork op spawns them and suspends the parent until all complete.
    void lower_fork(const BlockStatement& blk, ir::Builder& b) {
        if (blk.blockKind != StatementBlockKind::JoinAll) {
            // TODO: join_any / join_none (V2) need a growable active ring — a
            // loop can spawn an unbounded number of concurrently-live children
            // (docs/design/fork-join.md).
            fail("fork..join_any / fork..join_none are not supported");
        }
        if (fn_ctx_ != nullptr) {
            // TODO: fork inside a task/function (V1 restriction). A child would
            // need to alias the enclosing subroutine's frame locals, not the
            // parent-signal path build_fork_child assumes.
            fail("fork..join inside a function or task is not supported");
        }

        // Children are the fork body's top-level statements (a StatementList),
        // or the single statement / empty body.
        std::vector<const Statement*> children;
        if (blk.body.kind == StatementKind::List) {
            for (const Statement* s : blk.body.as<StatementList>().list) {
                children.push_back(s);
            }
        } else if (blk.body.kind != StatementKind::Empty) {
            children.push_back(&blk.body);
        }
        if (children.empty()) {
            return; // `fork join` with no children: a no-op.
        }

        std::vector<ir::FunctionId> ids;
        ids.reserve(children.size());
        for (const Statement* c : children) {
            ids.push_back(build_fork_child(*c));
        }
        b.fork(ids, ir::ForkKind::Join, loc(blk));
    }

    ir::Index lower_predicate(const Expression& cond, ir::Builder& b) {
        const ir::SourceLoc l = loc(cond);
        if (is_string_expr(cond)) {
            const ir::Index len = b.unary(ir::Inst::Tag::StrLen, lower_expr(cond, b), l);
            return b.unary(ir::Inst::Tag::RedOr, len, l);
        }
        const ir::Index pred = lower_expr(cond, b);
        if (cond.type->isFloating()) {
            // A real condition is true iff nonzero — a definite 1-bit result
            // (reals carry no x/z).
            return b.binary(ir::Inst::Tag::FNe, pred, real_zero(cond, b, l), l);
        }
        return b.unary(ir::Inst::Tag::RedOr, pred, l);
    }

    // A loop condition (12.7) has the same truth value as an `if` condition
    // (12.4): true iff any bit is definitely 1. Reduce to a 1-bit predicate with
    // lower_predicate so a multi-bit or real condition is normalized here, not
    // left to the backend (which would emit a width-mismatched compare on the
    // native-width must plane, or route a real plane through must_of).
    ir::Index lower_loop_cond(const Expression& cond, ir::Builder& b) {
        return lower_predicate(cond, b);
    }

    // Whether the if/else-if chain rooted at `c` ends in an explicit `else`
    // (a non-`ConditionalStatement` at the bottom of the ifFalse spine). Used to
    // decide the priority-if no-match report: an explicit else always matches, so
    // a chain with one can never no-match. A `begin ... end`-nested inner `if` is
    // a BlockStatement, so it correctly counts as a final else (LRM 12.4.2).
    bool chain_has_final_else(const ConditionalStatement& c) const {
        const ConditionalStatement* cur = &c;
        while (cur->ifFalse != nullptr) {
            const auto* nested = cur->ifFalse->as_if<ConditionalStatement>();
            if (nested == nullptr) {
                return true;
            }
            cur = nested;
        }
        return false;
    }

    // Immediate assertion (LRM 16.3): assert / assume / cover. A true condition
    // runs the optional pass action. `assert`/`assume` (which check identically
    // for simulation, LRM 16.4.3) additionally run the `else` action on a false
    // condition, or emit the default failure report (error severity) when there
    // is none. `cover` has no fail action — a false condition does nothing; its
    // coverage count/report is deliberately unimplemented.
    void lower_immediate_assertion(const ImmediateAssertionStatement& a, ir::Builder& b) {
        if (a.isDeferred || a.isFinal) {
            // TODO: deferred (assert #0) and final immediate assertions defer the
            // check/report to the Observed/Reactive region, scheduling Clotho
            // lacks.
            fail("deferred / final immediate assertions are not supported");
        }
        using AK              = slang::ast::AssertionKind;
        const bool is_cover   = a.assertionKind == AK::CoverProperty;
        if (a.assertionKind != AK::Assert && a.assertionKind != AK::Assume && !is_cover) {
            // TODO: immediate restrict / expect (LRM 16.3) need a property engine;
            // concurrent cover (CoverSequence) needs SVA.
            fail("only immediate assert / assume / cover are supported");
        }
        const ir::Index pred = lower_predicate(a.cond, b);

        b.open_region();
        if (a.ifTrue != nullptr) {
            lower_stmt(*a.ifTrue, b);
        }
        const ir::ExtraSlice then_region = b.close_region();

        // False branch: cover does nothing; assert/assume run the else action or
        // the default failure report.
        b.open_region();
        if (!is_cover) {
            if (a.ifFalse != nullptr) {
                lower_stmt(*a.ifFalse, b);
            } else {
                b.severity(static_cast<uint32_t>(ir::Severity::Error),
                           banner_at(a.sourceRange.start()), "assertion failed", {}, loc(a));
            }
        }
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(pred, then_region, else_region, loc(a));
    }

    void lower_conditional(const ConditionalStatement& c, ir::Builder& b) {
        // unique / unique0 / priority on `if` add the LRM 12.4.2 violation checks;
        // execution (ordered first-match) is identical to a plain if.
        //   - priority / unique: no-match report when no condition is true and the
        //     chain has no explicit final else (kind 0). unique0 stays silent.
        //   - unique / unique0: overlap report when >1 condition is true (kind 1).
        using Check = slang::ast::UniquePriorityCheck;
        if (c.check == Check::Unique || c.check == Check::Unique0) {
            lower_unique_if(c, b);
            return;
        }
        // priority (or the plain None case, which sets no flag): the lazy chain
        // with a no-match report threaded to its terminal.
        const bool report_no_match = c.check == Check::Priority && !chain_has_final_else(c);
        lower_if_chain(c, report_no_match, report_no_match ? banner_at(c.sourceRange.start()) : std::string{},
                       loc(c), b);
    }

    // One arm of a collected if / else-if chain: the condition and the statement
    // it guards.
    struct IfArm {
        const Expression* cond;
        const Statement*  body;
    };

    // Eager lowering for unique / unique0 `if` (LRM 12.4.2). The else-if spine is
    // collected into arms, every arm predicate is evaluated once up front (the
    // LRM permits any-order eager evaluation for unique/unique0 and requires
    // continuing past the first true condition to detect overlap), and the temps
    // are reused for the overlap check and the first-match dispatch. Mirrors the
    // case path (lower_case_unique) over if arms instead of case items.
    void lower_unique_if(const ConditionalStatement& c, ir::Builder& b) {
        using Check = slang::ast::UniquePriorityCheck;

        // Walk the ifFalse spine, collecting each same-series arm. The spine ends
        // at a null ifFalse (no final else) or a non-continuation statement (a
        // plain else body, a begin-end block, or a nested new-series if) — that
        // trailing statement becomes the final else.
        std::vector<IfArm> arms;
        const Statement*   final_else = nullptr;
        const ConditionalStatement* cur = &c;
        for (;;) {
            if (cur->conditions.size() != 1) {
                fail("unsupported multi-condition if");
            }
            if (cur->conditions[0].pattern != nullptr) {
                fail("pattern matching in conditions is not supported");
            }
            arms.push_back({ &*cur->conditions[0].expr, &cur->ifTrue });
            if (cur->ifFalse == nullptr) {
                break;
            }
            const auto* nested = cur->ifFalse->as_if<ConditionalStatement>();
            if (nested == nullptr || nested->check != Check::None) {
                final_else = cur->ifFalse;
                break;
            }
            cur = nested;
        }

        // unique reports a no-match (no arm true, no final else); unique0 does not.
        const bool no_match_report = c.check == Check::Unique && final_else == nullptr;
        const std::string   banner = banner_at(c.sourceRange.start());
        const ir::SourceLoc rloc   = loc(c);

        std::vector<ir::Index> preds;
        preds.reserve(arms.size());
        for (const IfArm& arm : arms) {
            preds.push_back(lower_predicate(*arm.cond, b));
        }

        emit_overlap_check(preds, banner, rloc, b);
        dispatch_if_arms(arms, 0, preds, final_else, no_match_report, banner, rloc, b);
    }

    // First-match dispatch for a unique/unique0 if, reusing the precomputed
    // predicate temps: arm k's body runs when preds[k] is the first true one. The
    // terminal (no arm true) runs the final else, else emits the no-match report
    // when requested (unique, no final else).
    void dispatch_if_arms(const std::vector<IfArm>& arms, size_t k, const std::vector<ir::Index>& preds,
                          const Statement* final_else, bool no_match_report, const std::string& banner,
                          ir::SourceLoc rloc, ir::Builder& b) {
        if (k == arms.size()) {
            if (final_else != nullptr) {
                lower_stmt(*final_else, b);
            }
            else if (no_match_report) {
                b.case_violation(banner, 0, rloc);
            }
            return;
        }
        b.open_region();
        lower_stmt(*arms[k].body, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        dispatch_if_arms(arms, k + 1, preds, final_else, no_match_report, banner, rloc, b);
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(preds[k], then_region, else_region, loc(*arms[k].body));
    }

    // Lower an if / else-if chain. When `report_no_match` is set (a default-less
    // priority chain), the flag is threaded down the ifFalse spine and the
    // terminal else (reached only when no condition was true) emits a no-match
    // report at the chain's `rloc`. With the flag clear this is exactly the plain
    // nested-cond lowering (the nested ifFalse re-enters lower_conditional, which
    // handles its own check), so non-priority ifs are unaffected.
    void lower_if_chain(const ConditionalStatement& c, bool report_no_match, const std::string& banner,
                        ir::SourceLoc rloc, ir::Builder& b) {
        if (c.conditions.size() != 1) {
            fail("unsupported multi-condition if");
        }
        if (c.conditions[0].pattern != nullptr) {
            // TODO: `if (expr matches pattern)` (LRM 12.6.2) — the pattern must
            // be matched and its variables bound; lowering the bare condition
            // would silently ignore the pattern.
            fail("pattern matching in conditions is not supported");
        }
        const ir::Index pred = lower_predicate(*c.conditions[0].expr, b);

        b.open_region();
        lower_stmt(c.ifTrue, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        if (c.ifFalse != nullptr) {
            const auto* nested = c.ifFalse->as_if<ConditionalStatement>();
            if (report_no_match && nested != nullptr &&
                nested->check == slang::ast::UniquePriorityCheck::None) {
                // Continue the same priority series, carrying the flag to its end.
                lower_if_chain(*nested, report_no_match, banner, rloc, b);
            }
            else {
                lower_stmt(*c.ifFalse, b);
            }
        }
        else if (report_no_match) {
            b.case_violation(banner, 0, rloc);
        }
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(pred, then_region, else_region, loc(c));
    }

    // A definite 1-bit `1`, used as the predicate for an omitted for-condition
    // or a `forever` loop.
    ir::Index true_const(ir::Builder& b, ir::SourceLoc l) {
        return b.constant(module_.pool.put_int(make_int_type(1, false, false), 1), l);
    }

    // A local declaration statement: register the variable's storage (a static
    // local's persistent Signal, a per-call frame Local, or a module Signal in a
    // process) and, if it has an initializer, store it. `force_frame` marks a loop
    // control variable, which is per-call regardless of the function's lifetime.
    void lower_var_decl(const VariableSymbol& vs, ir::Builder& b, bool force_frame = false) {
        declare_storage(vs, force_frame);
        if (const Expression* init = vs.getInitializer()) {
            if (is_static_local(vs, force_frame)) {
                // A static local's initializer runs once at time zero, not on each
                // call; modeling that needs a separate init pass. A static local
                // without an initializer (default 0 / X at time zero) works.
                fail("static function local with an initializer is not supported");
            }
            const ir::SourceLoc l = loc(*init);
            if (is_aggregate(vs.getType()) || is_unpacked_container(vs.getType())) {
                emit_struct_store(value_lvalue(vs, b, l), lower_type(vs.getType()), *init, ir::Inst::Tag::Store, b, l);
            } else {
                b.binary(ir::Inst::Tag::Store, value_lvalue(vs, b, l), lower_expr(*init, b), l);
            }
        } else if (aggregate_has_field_defaults(vs.getType())) {
            // No initializer of its own, but the struct type declares field-level
            // defaults (LRM 7.2 / 8.8). Same "runs once, not per call" hazard as
            // an explicit static-local initializer above: applying the defaults
            // here would re-store them on every call, clobbering a previous
            // call's mutations.
            if (is_static_local(vs, force_frame)) {
                fail("static function local with a struct field-level default initializer is not supported");
            }
            const ir::SourceLoc l = to_source_loc(vs.location);
            emit_var_field_defaults(vs, value_lvalue(vs, b, l), b, l);
        }
    }

    // Apply a module-level (static) variable's declaration initializer (LRM 6.8)
    // as a store in the run-once pre-init process. The store services no wait queue
    // (the backend suppresses servicing for a PreInit process), so initialization
    // generates no events and every Initial / Always process starts from the
    // settled value, before any of them run. A variable with no initializer of
    // its own, and whose struct type has no field-level defaults either, keeps
    // its power-on default (X / 0), set by the backend's init_state.
    void lower_var_initializer(const VariableSymbol& vs) {
        const Expression* init = vs.getInitializer();
        if (init == nullptr) {
            if (aggregate_has_field_defaults(vs.getType())) {
                if (!preinit_) {
                    preinit_.emplace(module_.pool);
                }
                ir::Builder&        b = *preinit_;
                const ir::SourceLoc l = to_source_loc(vs.location);
                emit_var_field_defaults(vs, value_lvalue(vs, b, l), b, l);
            }
            return;
        }
        if (!preinit_) {
            preinit_.emplace(module_.pool);
        }
        ir::Builder&        b = *preinit_;
        const ir::SourceLoc l = loc(*init);
        if (is_aggregate(vs.getType()) || is_unpacked_container(vs.getType())) {
            emit_struct_store(value_lvalue(vs, b, l), lower_type(vs.getType()), *init, ir::Inst::Tag::Store, b, l);
        } else {
            b.binary(ir::Inst::Tag::Store, value_lvalue(vs, b, l), lower_expr(*init, b), l);
        }
    }

    // Emit the accumulated declaration initializers as one run-once PreInit
    // process. Called once after the top instance and all its children are lowered;
    // a no-op when the design has no variable initializers.
    void finalize_preinit() {
        if (!preinit_) {
            return;
        }
        module_.add_process(
          ir::Process{ .activation = ir::Activation::PreInit, .ir = preinit_->finish() });
        preinit_.reset();
    }

    // for-loop. The initialization runs once in the enclosing region; the
    // condition, body, and step become the loop's three regions. An omitted
    // condition is a constant true (12.7.1); an X/Z condition exits (handled in
    // the backend via the definite-1 rule).
    void lower_for(const ForLoopStatement& f, ir::Builder& b) {
        // Init: declared loop vars and assignment initializers are mutually
        // exclusive. Either is lowered in source order so a later init may read
        // an earlier one (12.7.1).
        for (const VariableSymbol* vs : f.loopVars) {
            lower_var_decl(*vs, b, true);
        }
        for (const Expression* init : f.initializers) {
            lower_for_step(*init, b);
        }

        b.open_region();
        const ir::Index pred =
          f.stopExpr != nullptr ? lower_loop_cond(*f.stopExpr, b) : true_const(b, loc(f));
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_stmt(f.body, b);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        for (const Expression* s : f.steps) {
            lower_for_step(*s, b);
        }
        const ir::ExtraSlice step = b.close_region();

        b.loop(cond, pred, body, step, /*bottom_test*/ false, loc(f));
    }

    // while-loop: a for-loop with no init and no step.
    void lower_while(const WhileLoopStatement& w, ir::Builder& b) {
        b.open_region();
        const ir::Index     pred = lower_loop_cond(w.cond, b);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_stmt(w.body, b);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::ExtraSlice step = b.close_region(); // empty

        b.loop(cond, pred, body, step, /*bottom_test*/ false, loc(w));
    }

    // do-while (12.7.1): like while, but the body runs once before the first
    // predicate test. The bottom_test flag makes the backend branch entry ->
    // body; `continue` still lands on the step block, which re-evaluates the
    // condition (empty step -> header).
    void lower_do_while(const DoWhileLoopStatement& d, ir::Builder& b) {
        b.open_region();
        const ir::Index      pred = lower_loop_cond(d.cond, b);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_stmt(d.body, b);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::ExtraSlice step = b.close_region(); // empty

        b.loop(cond, pred, body, step, /*bottom_test*/ true, loc(d));
    }

    void lower_wait_stmt(const WaitStatement& w, ir::Builder& b) {
        const ir::SourceLoc l = loc(w);

        // The wait sensitivity: every signal the condition reads, each as an
        // any-change (level) event. Fails loudly on a string operand (wait
        // queues are not string-aware — see collect_expr_reads).
        std::vector<ir::WaitEvent> reads;
        collect_expr_reads(w.cond, reads);
        std::vector<ir::WaitEdge> edges;
        edges.reserve(reads.size());
        for (const ir::WaitEvent& e : reads) {
            edges.push_back(ir::WaitEdge{ e.signal, static_cast<uint32_t>(e.edge) });
        }

        // forever { if (cond) break; @(reads); }
        b.open_region();
        const ir::Index      lpred = true_const(b, l);
        const ir::ExtraSlice lcond = b.close_region();

        b.open_region();
        {
            const ir::Index c = lower_predicate(w.cond, b);
            b.open_region();
            b.break_(l);
            const ir::ExtraSlice then_r = b.close_region();
            b.open_region();
            const ir::ExtraSlice else_r = b.close_region(); // empty
            b.cond(c, then_r, else_r, l);
            b.wait(edges, l);
        }
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::ExtraSlice step = b.close_region(); // empty

        b.loop(lcond, lpred, body, step, /*bottom_test*/ false, l);

        lower_stmt(w.stmt, b);
    }

    // A forever loop around `body`: a constant-true Loop. Lowers both an
    // explicit `forever` statement's body and the implicit forever wrapping a
    // sensitivity-less `always` body (LRM 9.2.2), which has no separate AST
    // node. The body's own timing control (`#`/`@`) suspends each iteration;
    // without one this is the legal zero-delay infinite loop noted in
    // lower_procedural_block.
    void lower_forever(const Statement& body, ir::Builder& b) {
        b.open_region();
        const ir::Index      pred = true_const(b, loc(body));
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_stmt(body, b);
        const ir::ExtraSlice body_region = b.close_region();

        b.open_region();
        const ir::ExtraSlice step = b.close_region(); // empty

        b.loop(cond, pred, body_region, step, /*bottom_test*/ false, loc(body));
    }

    // repeat-loop (12.7.2): runs the body N times, where N is the count
    // expression evaluated EXACTLY ONCE on entry. Lowers to the generic Loop with
    // a hidden counter and a hidden bound: `for (counter = 0; counter < bound;
    // counter++) body`. The count's own type (width + signedness) carries through
    // so a negative signed count yields zero iterations (0 < negative is false),
    // and an X/Z count yields zero iterations (the Lt predicate is non-definite-1,
    // which the backend's definite-1 loop rule treats as exit).
    void lower_repeat(const RepeatLoopStatement& r, ir::Builder& b) {
        const ir::SourceLoc l   = loc(r);
        if (r.count.type->isFloating()) {
            // LRM 12.7.2: a real repeat count would round; not yet lowered.
            fail("repeat with a real count is not supported");
        }
        const ir::Ref       ty  = lower_type(*r.count.type);

        // Hidden bound holds the once-evaluated count; hidden counter starts at 0.
        // Both live in the enclosing region, like a for-loop's init.
        const ir::Signal bound   = declare_temp(ty, "repeat_bound");
        const ir::Signal counter = declare_temp(ty, "repeat_count");
        b.binary(ir::Inst::Tag::Store, b.signal(bound, l), b.resize(lower_expr(r.count, b), ty, l), l);
        b.binary(ir::Inst::Tag::Store, b.signal(counter, l),
                 b.constant(module_.pool.put_int(ty, 0), l), l);

        b.open_region();
        const ir::Index cur  = b.unary(ir::Inst::Tag::Load, b.signal(counter, l), l);
        const ir::Index lim  = b.unary(ir::Inst::Tag::Load, b.signal(bound, l), l);
        const ir::Index pred = b.binary(ir::Inst::Tag::Lt, cur, lim, l);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_stmt(r.body, b);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::Index step_cur = b.unary(ir::Inst::Tag::Load, b.signal(counter, l), l);
        const ir::Index one      = b.constant(module_.pool.put_int(ty, 1), l);
        const ir::Index next     = b.binary(ir::Inst::Tag::Add, step_cur, one, l);
        b.binary(ir::Inst::Tag::Store, b.signal(counter, l), next, l);
        const ir::ExtraSlice step = b.close_region();

        b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
    }

    // foreach-loop (12.7.3): nested loops over an array's declared dimensions.
    // `loopDims` has one entry per bracketed slot the user wrote, in array-
    // dimension order (leftmost = outermost, matching "outer loops correspond
    // to lower cardinality indices"); a skipped slot (empty loop var) means
    // "no iteration over that dimension" per the LRM -- not a loop that runs
    // once, no loop at all, so it just advances to the next slot in the same
    // region. Dimension bounds come straight from slang (`LoopDim::range`),
    // valid for both packed and unpacked fixed-range dimensions alike, so no
    // separate walk of Clotho's own (unpacked-only) array Type is needed --
    // whatever indexing the body performs on the result is ordinary,
    // pre-existing ElemSelect/BitSelect lowering, untouched by this loop nest.
    void lower_foreach(const ForeachLoopStatement& f, ir::Builder& b) {
        lower_foreach_dim(f, 0, b);
    }

    void lower_foreach_dim(const ForeachLoopStatement& f, size_t dim, ir::Builder& b) {
        if (dim == f.loopDims.size()) {
            lower_stmt(f.body, b);
            return;
        }
        const ForeachLoopStatement::LoopDim& d = f.loopDims[dim];
        if (d.loopVar == nullptr) {
            lower_foreach_dim(f, dim + 1, b);
            return;
        }

        const ir::SourceLoc l = loc(f);
        declare_storage(*d.loopVar, /*force_frame*/ true);
        const ir::Ref   ty      = lower_type(d.loopVar->getType());
        const ir::Index iter_lv = value_lvalue(*d.loopVar, b, l);

        if (!d.range.has_value()) {
            // A runtime-sized dimension (string or dynamic array): snapshot the
            // length once and iterate 0..len-1 (0-based, matching StrGetc and
            // dynamic-array element addressing).
            if (is_string_expr(f.arrayRef)) {
                const ir::Index len = b.unary(ir::Inst::Tag::StrLen, lower_expr(f.arrayRef, b), l);
                lower_foreach_runtime_dim(f, dim, ty, iter_lv, len, b, l);
                return;
            }
            if (is_dyn_array(f.arrayRef)) {
                const ir::Index len =
                  b.dyn_array_size(lower_lvalue(f.arrayRef, b), lower_type(*f.arrayRef.type), l);
                lower_foreach_runtime_dim(f, dim, ty, iter_lv, len, b, l);
                return;
            }
            if (is_queue(f.arrayRef)) {
                const ir::Index len =
                  b.queue_size(lower_lvalue(f.arrayRef, b), lower_type(*f.arrayRef.type), l);
                lower_foreach_runtime_dim(f, dim, ty, iter_lv, len, b, l);
                return;
            }
            // TODO: associative array foreach dimension -- needs ordered key
            // iteration.
            fail("foreach over an associative array dimension is not supported");
        }

        // Fixed-range dimension (packed or unpacked): iterate the declared
        // range directly, ascending or descending as declared (LRM's own
        // example iterates a `[5:1]` dimension 5 down to 1). The range is an
        // elaboration-time constant, so unlike a runtime count there is
        // nothing to snapshot.
        const slang::ConstantRange r         = *d.range;
        const bool                 ascending = r.left <= r.right;
        b.binary(ir::Inst::Tag::Store, iter_lv,
                 b.constant(module_.pool.put_int(ty, static_cast<uint64_t>(static_cast<uint32_t>(r.left))), l),
                 l);

        b.open_region();
        const ir::Index cur = b.unary(ir::Inst::Tag::Load, iter_lv, l);
        const ir::Index lim = b.constant(
          module_.pool.put_int(ty, static_cast<uint64_t>(static_cast<uint32_t>(r.right))), l);
        const ir::Index      pred = b.binary(ascending ? ir::Inst::Tag::Le : ir::Inst::Tag::Ge, cur, lim, l);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_foreach_dim(f, dim + 1, b);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::Index step_cur = b.unary(ir::Inst::Tag::Load, iter_lv, l);
        const ir::Index one      = b.constant(module_.pool.put_int(ty, 1), l);
        const ir::Index next =
          b.binary(ascending ? ir::Inst::Tag::Add : ir::Inst::Tag::Sub, step_cur, one, l);
        b.binary(ir::Inst::Tag::Store, iter_lv, next, l);
        const ir::ExtraSlice step = b.close_region();

        b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
    }

    // A runtime-sized foreach dimension (LRM 12.7.3: a `string` "is treated as a
    // dynamic array of bytes indexed from 0 to N-1"; a dynamic array likewise).
    // The bound `len` is snapshotted once on entry like a repeat-loop's count --
    // resizing the container mid-loop is undefined behavior per the LRM (same
    // clause), so one read is enough. 0-based, matching StrGetc / element
    // addressing.
    void lower_foreach_runtime_dim(const ForeachLoopStatement& f, size_t dim, ir::Ref ty,
                                   ir::Index iter_lv, ir::Index len, ir::Builder& b, ir::SourceLoc l) {
        const ir::Signal bound = declare_temp(ty, "foreach_bound");
        b.binary(ir::Inst::Tag::Store, b.signal(bound, l), b.resize(len, ty, l), l);
        b.binary(ir::Inst::Tag::Store, iter_lv, b.constant(module_.pool.put_int(ty, 0), l), l);

        b.open_region();
        const ir::Index      cur  = b.unary(ir::Inst::Tag::Load, iter_lv, l);
        const ir::Index      lim  = b.unary(ir::Inst::Tag::Load, b.signal(bound, l), l);
        const ir::Index      pred = b.binary(ir::Inst::Tag::Lt, cur, lim, l);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_foreach_dim(f, dim + 1, b);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::Index step_cur = b.unary(ir::Inst::Tag::Load, iter_lv, l);
        const ir::Index one      = b.constant(module_.pool.put_int(ty, 1), l);
        b.binary(ir::Inst::Tag::Store, iter_lv, b.binary(ir::Inst::Tag::Add, step_cur, one, l), l);
        const ir::ExtraSlice step = b.close_region();

        b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
    }

    // Register a hidden compiler temporary as backing signal storage. Unlike
    // declare_local there is no source ValueSymbol, so the name is synthesized.
    //
    // TODO: Back compiler temporaries (and SV automatic locals) with a
    // process-local storage class on the coroutine frame (an alloca) rather than
    // a module-global signal. The signal model gives static lifetime, pollutes
    // the signal table, and runs wait-queue machinery a loop-private counter does
    // not need. Correct today because processes are single-activation and the
    // counter is reinitialized at each loop entry, but wrong for true automatic
    // lifetime and future reentrant/recursive activations. See declare_local NOTE.
    ir::Signal declare_temp(ir::Ref type, std::string_view role) {
        ir::SignalInfo info;
        info.name = prefix_ + "__" + std::string(role) + std::to_string(temp_id_++);
        info.type = type;
        info.dir  = ir::SignalInfo::Direction::Internal;

        return register_signal(std::move(info));
    }

    // A for_initialization assignment or a for_step (12.7.1, A.6.8): a simple
    // assignment, or a pre/post increment/decrement. Compound assignments and
    // function-call steps are not supported yet.
    void lower_for_step(const Expression& e, ir::Builder& b) {
        if (e.kind == ExpressionKind::Assignment) {
            lower_assign(e.as<AssignmentExpression>(), b);
            return;
        }
        if (e.kind == ExpressionKind::UnaryOp) {
            // A step discards the result; ++/-- lower via lower_unary (which
            // rejects any other unary operator, none of which are legal steps).
            lower_unary(e.as<UnaryExpression>(), b);
            return;
        }
        fail("unsupported for-step expression (only assignment and ++/-- are supported)");
    }

    // The width/four-state/signedness a case's selector and labels are
    // compared at, plus the interned type built from them. Threading width/
    // four alongside `ty` lets every downstream user (case_dedup,
    // find_switch_run) share one computation instead of each re-deriving it
    // from `c.items` independently -- required once a sub-range of items
    // (a switch run) must key against the SAME width as the whole statement's
    // `sel`, not a possibly-narrower local recompute.
    struct CaseCompareType {
        ir::Ref  ty;
        uint32_t width;
        bool     four;
    };

    // The width/state/signedness all case_expressions are compared at: the
    // longest of the selector and every item label, four-state if any operand is,
    // signed only if all are (IEEE 1800-2023 12.5).
    CaseCompareType common_case_type(const CaseStatement& c) {
        // TODO: case with a real selector or label. LRM 12.5 defines matching
        // with === semantics, which Table 11-1 leaves undefined for reals
        // (iverilog compares with ==). Loud-fail until the semantics call.
        if (c.expr.type->isFloating()) {
            fail("case on a real selector is not supported");
        }
        for (const CaseStatement::ItemGroup& item : c.items) {
            for (const Expression* e : item.expressions) {
                if (e->type->isFloating()) {
                    fail("case with a real item label is not supported");
                }
            }
        }
        uint32_t width  = c.expr.type->getBitWidth();
        bool     four   = c.expr.type->isFourState();
        bool     signd  = c.expr.type->isSigned();
        for (const CaseStatement::ItemGroup& item : c.items) {
            for (const Expression* e : item.expressions) {
                width = std::max(width, e->type->getBitWidth());
                four  = four || e->type->isFourState();
                signd = signd && e->type->isSigned();
            }
        }

        return { make_int_type(width, four, signd), width, four };
    }

    // Whether a range bound is (possibly conversion-wrapped) the `$` literal.
    // slang's binder wraps an UnboundedLiteral operand in an implicit Conversion
    // to the range's common type, so the literal survives only as the
    // innermost operand -- peel every Conversion layer to find it.
    static bool is_unbounded_bound(const Expression& e) {
        const Expression* cur = &e;
        while (cur->kind == ExpressionKind::Conversion) {
            cur = &cur->as<ConversionExpression>().operand();
        }
        return cur->kind == ExpressionKind::UnboundedLiteral;
    }

    // Common compare type for `case ... inside` (12.5.4). As common_case_type,
    // but a range_list member may be a [lo:hi] ValueRangeExpression (itself
    // typed void) instead of a plain expression, so its bound types fold in
    // place of the range's own type. Tolerance ranges (`+/-`, `+%-`) and `$`
    // bounds guard loudly here (TODO: LRM 11.4.13 defines both, but neither
    // is exercised by ranged case-inside labels yet).
    CaseCompareType common_inside_type(const CaseStatement& c) {
        if (c.expr.type->isFloating()) {
            fail("case on a real selector is not supported");
        }
        uint32_t width = c.expr.type->getBitWidth();
        bool     four  = c.expr.type->isFourState();
        bool     signd = c.expr.type->isSigned();
        auto     fold  = [&](const Expression& e) {
            if (e.type->isFloating()) {
                fail("case with a real item label is not supported");
            }
            width = std::max(width, e.type->getBitWidth());
            four  = four || e.type->isFourState();
            signd = signd && e.type->isSigned();
        };
        for (const CaseStatement::ItemGroup& item : c.items) {
            for (const Expression* e : item.expressions) {
                if (e->kind != ExpressionKind::ValueRange) {
                    fold(*e);
                    continue;
                }
                const auto& vr = e->as<ValueRangeExpression>();
                if (vr.rangeKind != ValueRangeKind::Simple) {
                    // TODO: absolute/relative tolerance ranges (LRM 11.4.13).
                    fail("case ... inside tolerance ranges ('+/-' / '+%-') are not supported");
                }
                if (is_unbounded_bound(vr.left()) || is_unbounded_bound(vr.right())) {
                    // TODO: `$` range bound (lowest/highest value of the selector's type).
                    fail("case ... inside unbounded ('$') range bounds are not supported");
                }
                fold(vr.left());
                fold(vr.right());
            }
        }
        return { make_int_type(width, four, signd), width, four };
    }

    // Predicate for one case-inside range_list member (LRM 11.4.13 / 12.5.4).
    // A plain expression uses wildcard equality (`==?`): the case_expression is
    // the inside operator's definite left operand, so only the label's own x/z
    // bits are wildcards (WildEq already matches this exactly). A [lo:hi] range
    // tests inclusive membership with Ge/Le, ANDed -- an x/z bound or selector
    // makes the comparison ambiguous, which Land propagates as x rather than a
    // match, and b.cond already branches to else on anything but a definite 1.
    ir::Index lower_inside_label_pred(const Expression& label, ir::Index sel, ir::Ref ty, ir::Builder& b) {
        if (label.kind == ExpressionKind::ValueRange) {
            const auto&     vr = label.as<ValueRangeExpression>();
            const ir::Index lo = b.resize(lower_expr(vr.left(), b), ty, loc(vr.left()));
            const ir::Index hi = b.resize(lower_expr(vr.right(), b), ty, loc(vr.right()));
            const ir::Index ge = b.binary(ir::Inst::Tag::Ge, sel, lo, loc(label));
            const ir::Index le = b.binary(ir::Inst::Tag::Le, sel, hi, loc(label));
            return b.binary(ir::Inst::Tag::Land, ge, le, loc(label));
        }
        const ir::Index val = b.resize(lower_expr(label, b), ty, loc(label));
        return b.binary(ir::Inst::Tag::WildEq, sel, val, loc(label));
    }

    // OR of a case-inside item's range_list member predicates (12.5.4: a
    // case_item is matched when the inside operation returns 1'b1 for any of
    // its members). No dedup: 11.4.13 explicitly allows repeated/overlapping
    // members, so unlike case_dedup nothing is ever dropped here.
    ir::Index lower_inside_item_pred(const CaseStatement::ItemGroup& item, ir::Index sel, ir::Ref ty,
                                     ir::Builder& b) {
        ir::Index pred = lower_inside_label_pred(*item.expressions[0], sel, ty, b);
        for (size_t j = 1; j < item.expressions.size(); j++) {
            const Expression* e = item.expressions[j];
            const ir::Index   one = lower_inside_label_pred(*e, sel, ty, b);
            pred                  = b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e));
        }
        return pred;
    }

    // Lazy first-match chain for `case ... inside`, mirroring lower_case_item:
    // item k's range_list predicate guards its body, with the rest of the chain
    // (eventually the default) as the else branch, so labels not yet reached
    // are evaluated lazily.
    // Lazy first-match chain for `case ... inside`'s residual (range/wildcard)
    // items, mirroring lower_case_item -- but first tries find_switch_run at
    // every step, so a run of plain clean-constant singular labels between
    // range items still jump-tables instead of chaining one at a time.
    void lower_case_inside_item(const CaseStatement& c, size_t k, ir::Index sel, ir::Ref ty,
                                uint32_t width, uint32_t key_width,
                                const std::vector<std::vector<char>>& keep,
                                const slang::ast::ASTContext& astCtx, bool report_viol, ir::Builder& b) {
        if (k == c.items.size()) {
            if (c.defaultCase != nullptr) {
                lower_stmt(*c.defaultCase, b);
            }
            else if (report_viol) {
                b.case_violation(case_banner(c), 0, loc(c));
            }
            return;
        }

        if (std::optional<SwitchRun> run = find_switch_run(c, k, width, key_width, &keep, astCtx)) {
            if (run->arms.empty()) {
                lower_case_inside_item(c, k + run->len, sel, ty, width, key_width, keep, astCtx,
                                       report_viol, b);
                return;
            }
            const std::vector<ir::Builder::SwitchArmBuild> arms = lower_switch_arm_bodies(run->arms, b);
            b.open_region();
            lower_case_inside_item(c, k + run->len, sel, ty, width, key_width, keep, astCtx,
                                   report_viol, b);
            const ir::ExtraSlice default_region = b.close_region();
            b.switch_(sel, arms, default_region, loc(c.expr));
            return;
        }

        const ir::Index pred = lower_inside_item_pred(c.items[k], sel, ty, b);

        b.open_region();
        lower_stmt(*c.items[k].stmt, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        lower_case_inside_item(c, k + 1, sel, ty, width, key_width, keep, astCtx, report_viol, b);
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(pred, then_region, else_region, loc(*c.items[k].stmt));
    }

    // Eager overlap-checked lowering for unique/unique0 `case ... inside`
    // (mirrors lower_case_unique): every item's predicate is computed once up
    // front and shared by emit_overlap_check and dispatch_preds.
    void lower_case_inside_unique(const CaseStatement& c, ir::Index sel, ir::Ref ty, uint32_t width,
                                  uint32_t key_width, const slang::ast::ASTContext& astCtx,
                                  bool no_match_report, ir::Builder& b) {
        std::vector<ir::Index> preds;
        preds.reserve(c.items.size());
        for (const CaseStatement::ItemGroup& item : c.items) {
            preds.push_back(lower_inside_item_pred(item, sel, ty, b));
        }
        emit_overlap_check(preds, case_banner(c), loc(c), b);
        dispatch_preds(c, 0, sel, width, key_width, astCtx, preds, no_match_report, b);
    }

    // `case ... inside` (12.5.4): set-membership matching via the inside
    // operator (11.4.13). A run of clean-constant singular labels jump-tables
    // via find_switch_run (shared with plain/casex/casez case); a range_list
    // member or the one-hot idiom's ElementSelect labels never form a run, so
    // those keep the existing predicate chain unchanged.
    void lower_case_inside(const CaseStatement& c, ir::Builder& b) {
        using Check = slang::ast::UniquePriorityCheck;
        const bool no_match_report =
          (c.check == Check::Priority || c.check == Check::Unique) && c.defaultCase == nullptr;
        const bool needs_overlap = c.check == Check::Unique || c.check == Check::Unique0;

        const CaseCompareType   ct        = common_inside_type(c);
        const ir::Ref           ty        = ct.ty;
        const uint32_t          key_width = ct.four ? 2 * ct.width : ct.width;
        const ir::Index         sel       = b.resize(lower_expr(c.expr, b), ty, loc(c.expr));
        slang::ast::ASTContext  astCtx(*scope_, slang::ast::LookupLocation::max);

        if (needs_overlap) {
            lower_case_inside_unique(c, sel, ty, ct.width, key_width, astCtx, no_match_report, b);
            return;
        }
        const std::vector<std::vector<char>> keep = case_dedup(c, ct.width, astCtx);
        lower_case_inside_item(c, 0, sel, ty, ct.width, key_width, keep, astCtx, no_match_report, b);
    }

    // Priority-order case dedup (LRM 12.5). The first matching case_item wins,
    // so a later label that always evaluates identically to an earlier one is
    // dead -- its body can never run. A constant label's value is fixed, so a
    // later exact duplicate (=== at the common compare width) is always dead,
    // immune to any intervening side effect. Handles casez/casex too: an
    // identical label yields an identical match predicate regardless of its
    // wildcard bits.
    //
    // TODO: dedup non-constant duplicate labels (a repeated `k` / `a+b`). Only
    // safe when the label and every label between the two occurrences are
    // side-effect-free -- else an intervening f() or ++ could mutate the
    // variables it reads, or a conforming sim must still run the dropped label's
    // own effect (has_observable_effect + a pure-label memo cleared at any impure
    // label gates it). Dropped for now: rare, and the constant path is what the
    // downstream jump-table lowering needs.
    //
    // Skipped for unique/priority checks: a unique/unique0 overlap is a
    // reportable violation, so duplicate labels must survive for
    // lower_case_unique's predicate sum to detect them.
    //
    // `width` is the statement's single common compare width (from
    // common_case_type/common_inside_type) -- passed in rather than
    // re-derived here so this pass and find_switch_run's per-run key
    // extraction always agree on the same width as `sel`. A range_list
    // member (`case ... inside`, itself typed void) is skipped before eval:
    // it is never treated as a duplicate-constant candidate (a genuinely
    // repeated *range* is a missed dead-code opportunity, not a correctness
    // gap -- see find_switch_run, which never puts a range in a switch key
    // either).
    //
    // Returns keep[k][j] = whether item k's label j survives.
    std::vector<std::vector<char>> case_dedup(const CaseStatement& c, uint32_t width,
                                              const slang::ast::ASTContext& astCtx) {
        std::vector<std::vector<char>> keep(c.items.size());
        for (size_t k = 0; k < c.items.size(); k++) {
            keep[k].assign(c.items[k].expressions.size(), 1);
        }
        if (c.check != slang::ast::UniquePriorityCheck::None) {
            return keep;
        }

        std::vector<slang::SVInt> seen_const;

        for (size_t k = 0; k < c.items.size(); k++) {
            const std::span<const Expression* const> exprs = c.items[k].expressions;
            for (size_t j = 0; j < exprs.size(); j++) {
                const Expression& e = *exprs[j];
                if (e.kind == ExpressionKind::ValueRange) {
                    continue;
                }
                slang::ast::EvalContext    ev(astCtx);
                const slang::ConstantValue cv = e.eval(ev);
                if (!cv.isInteger()) {
                    continue;
                }
                slang::SVInt v = cv.integer();
                if (v.getBitWidth() < width) {
                    v = v.extend(width, e.type->isSigned());
                }
                bool dup = false;
                for (const slang::SVInt& s : seen_const) {
                    if (exactlyEqual(s, v)) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    keep[k][j] = 0;
                }
                else {
                    seen_const.push_back(std::move(v));
                }
            }
        }
        return keep;
    }

    // One arm of a switch-lowered case: the constant clean key values selecting
    // it (already normalized to the common compare width) and the body to run.
    struct SwitchArmPlan {
        std::vector<uint64_t>        keys;
        const slang::ast::Statement* body;
    };

    // A maximal run of consecutive switch-eligible items starting at some
    // index k: `len` items are consumed (the caller continues at k + len),
    // and `arms` (which may be empty -- see find_switch_run) are the surviving
    // switch keys/bodies.
    struct SwitchRun {
        size_t                     len;
        std::vector<SwitchArmPlan> arms;
    };

    // Try to consume a maximal run of switch-eligible items starting at index
    // k (generalizes the old whole-statement-only build_switch_arms to a
    // sub-range, so one range/wildcard/non-constant item no longer disables
    // switch dispatch for the rest of the statement -- mirrors how rustc's
    // match lowering groups a maximal run of SwitchInt-compatible arms and
    // recurses into the "otherwise" branch for the rest).
    //
    // An item is eligible iff every label is a constant with no x/z bits
    // ("clean") -- the backend keys a clean value on its bare bits
    // ({may^must, must}), so this needs the packed key to fit 64 bits, checked
    // once for the whole statement via `key_width` (not per-run: a statement
    // too wide for a switch never forms any run at all, matching the old
    // whole-statement gate). The run stops at the first ineligible item; a
    // ValueRangeExpression label (`case ... inside`) always disqualifies its
    // item, so it always ends a run.
    //
    // Clean-and-constant is NOT enough on its own: casex/casez (CasexEq/
    // CasezEq, see lower_case_compare in LlvmBackend.cpp) wildcard on the
    // SELECTOR's own x/z bits too ("dc = a.z | b.z" etc., operand-symmetric),
    // which no fixed compile-time switch key can express -- an x/z selector
    // must still be able to match a clean casex/casez label, but a switch's
    // densified key makes any x/z selector land outside every case value.
    // WildEq (`case ... inside`'s singular labels) only wildcards the LABEL
    // operand, so a clean Inside label degenerates exactly to plain equality
    // and is switch-safe. So this is gated to Normal/Inside; casex/casez never
    // form a run (every call returns nullopt immediately), leaving their
    // existing chain-only lowering untouched.
    //
    // `keep`, when non-null (plain/priority dispatch), filters labels the
    // single whole-statement case_dedup pass already proved dead -- this
    // function never re-derives its own cross-item dedup in that mode, so a
    // duplicate straddling two different runs (or a run and a residual item)
    // is still resolved consistently by the one shared pass. When null
    // (unique/unique0 dispatch, which skips case_dedup so every item's label
    // survives for the overlap-check sum), a fresh set scoped to just this
    // call dedups keys for THIS run's own switch validity only: a duplicate
    // constant in an EARLIER run or residual item needs no cross-run
    // tracking, because that earlier item is always reached first by the
    // recursion regardless (dispatch skips a claimed key only because it
    // dispatches first, not because of any bookkeeping here) -- the overlap
    // itself is still reported by the separate, unconditional
    // emit_overlap_check pass over every item's own predicate, which this
    // function has no bearing on.
    //
    // Returns nullopt only when the run at k is empty (item k itself isn't
    // eligible). A non-nullopt run may still have empty `arms` (every label
    // in it deduped away, e.g. a run of items that all repeat an earlier
    // item's constant) -- callers must skip switch emission and recurse
    // directly at k + len in that case.
    std::optional<SwitchRun> find_switch_run(const CaseStatement& c, size_t k, uint32_t width,
                                             uint32_t key_width,
                                             const std::vector<std::vector<char>>* keep,
                                             const slang::ast::ASTContext& astCtx) {
        if (key_width == 0 || key_width > 64 ||
            (c.condition != CaseStatementCondition::Normal &&
             c.condition != CaseStatementCondition::Inside)) {
            return std::nullopt;
        }

        std::vector<SwitchArmPlan>             arms;
        ankerl::unordered_dense::set<uint64_t> local_seen;  // only used when keep == nullptr.
        size_t                                 i = k;
        for (; i < c.items.size(); i++) {
            const CaseStatement::ItemGroup& item = c.items[i];
            std::vector<uint64_t>           keys;
            bool                             eligible = true;
            for (size_t j = 0; j < item.expressions.size(); j++) {
                const Expression* e = item.expressions[j];
                if (e->kind == ExpressionKind::ValueRange) {
                    eligible = false;
                    break;
                }
                if (keep != nullptr && !(*keep)[i][j]) {
                    continue;  // already proven dead by the whole-statement case_dedup.
                }
                slang::ast::EvalContext    ev(astCtx);
                const slang::ConstantValue cv = e->eval(ev);
                if (!cv.isInteger() || cv.integer().hasUnknown()) {
                    eligible = false;
                    break;
                }
                slang::SVInt v = cv.integer();
                if (v.getBitWidth() < width) {
                    v = v.extend(width, e->type->isSigned());
                }
                const std::optional<uint64_t> u = v.as<uint64_t>();
                if (!u) {
                    eligible = false;
                    break;
                }
                if (keep != nullptr) {
                    // case_dedup already guarantees this key is unique statement-wide.
                    keys.push_back(*u);
                }
                else if (local_seen.insert(*u).second) {
                    keys.push_back(*u);
                }
            }
            if (!eligible) {
                break;
            }
            if (!keys.empty()) {
                arms.push_back({ std::move(keys), item.stmt });
            }
        }
        if (i == k) {
            return std::nullopt;
        }
        return SwitchRun{ i - k, std::move(arms) };
    }

    // Lower a switch run's surviving arms to region-backed SwitchArmBuilds,
    // shared by every chain flavor (Normal/casex/casez, case-inside,
    // unique/unique0 dispatch) that consumes a find_switch_run result.
    std::vector<ir::Builder::SwitchArmBuild> lower_switch_arm_bodies(
      const std::vector<SwitchArmPlan>& plan, ir::Builder& b) {
        std::vector<ir::Builder::SwitchArmBuild> arms;
        arms.reserve(plan.size());
        for (const SwitchArmPlan& ap : plan) {
            b.open_region();
            lower_stmt(*ap.body, b);
            const ir::ExtraSlice body = b.close_region();
            arms.push_back({ std::span<const uint64_t>(ap.keys), body });
        }
        return arms;
    }

    // A one-hot reverse-case plan: the base packed value and, per arm, the IR
    // bit position it selects and the body to run. `descending` picks Clz
    // (highest-bit priority) over Ctz (lowest-bit).
    struct OneHotPlan {
        const Expression*                                  base;
        std::vector<std::pair<uint64_t, const Statement*>> arms;  // (IR bit, body)
        bool                                               descending;
        uint32_t                                           base_width;
    };

    // Detect the one-hot reverse-case idiom `case (1'b1)` where every arm is a
    // single-bit select `base[i]` of a common packed base with a constant index.
    // First arm in source order whose bit is a definite 1 wins (LRM 12.5). When
    // the arms' IR bit positions run monotonically -- ascending (lower Ctz) or
    // descending (Clz) -- source-order priority equals lowest/highest-set-bit
    // priority for ANY set-bit pattern, so it lowers to
    // `switch (ctz/clz(base)) { i_k -> arm_k }`: a jump table keyed on the
    // winning bit index (LLVM owns the table and permutes the arbitrary case
    // values). x/z bits are not definite 1, so they never match, matching `===`.
    //
    // TODO(unique): `unique`/`unique0 case (1'b1)` asserts one-hot (<= 1 bit
    // set), which makes ANY arm order safe (order is unobservable with one bit)
    // -- accept non-monotonic orders there once the unique/unique0 overlap
    // violation report lands (a TODO shared with the plain switch path).
    // TODO(repack): a non-monotonic plain case is not handled -- source-order
    // priority then differs from bit order under multiple simultaneous set bits.
    // A cheap gather re-packing the selected bits into source order (bit k =
    // base[i_k]) before Ctz would restore it; deferred (O(N) ops/eval, rare).
    std::optional<OneHotPlan> build_onehot_plan(const CaseStatement& c) {
        if (c.condition != CaseStatementCondition::Normal ||
            c.check != slang::ast::UniquePriorityCheck::None) {
            return std::nullopt;
        }
        // Selector must be exactly the 1-bit constant 1.
        {
            slang::ast::ASTContext     ctx(*scope_, slang::ast::LookupLocation::max);
            slang::ast::EvalContext    ev(ctx);
            const slang::ConstantValue cv = c.expr.eval(ev);
            if (!cv.isInteger() || cv.integer().getBitWidth() != 1 || cv.integer().hasUnknown() ||
                cv.integer().as<uint64_t>().value_or(0) != 1) {
                return std::nullopt;
            }
        }

        const slang::ast::ValueSymbol*                     base_sym   = nullptr;
        const Expression*                                  base_expr  = nullptr;
        uint32_t                                           base_width = 0;
        std::vector<std::pair<uint64_t, const Statement*>> arms;
        for (const CaseStatement::ItemGroup& item : c.items) {
            if (item.expressions.size() != 1) {
                return std::nullopt;  // grouped labels: fallback (see TODO(repack)).
            }
            const Expression& e = *item.expressions[0];
            if (e.kind != ExpressionKind::ElementSelect) {
                return std::nullopt;
            }
            const ElementSelectExpression& es = e.as<ElementSelectExpression>();
            if (es.value().kind != ExpressionKind::NamedValue) {
                return std::nullopt;
            }
            const slang::ast::ValueSymbol& sym = es.value().as<NamedValueExpression>().symbol;
            if (base_sym == nullptr) {
                base_sym   = &sym;
                base_expr  = &es.value();
                base_width = es.value().type->getBitWidth();
                if (base_width == 0 || !es.value().type->isIntegral()) {
                    return std::nullopt;
                }
            }
            else if (&sym != base_sym) {
                return std::nullopt;  // arms must share one base signal.
            }
            if (!e.type->isIntegral() || e.type->getBitWidth() != 1) {
                return std::nullopt;
            }
            const std::optional<int32_t> decl = try_const_int(es.selector());
            if (!decl) {
                return std::nullopt;
            }
            const slang::ConstantRange range = packed_range_of(es.value());
            if (!range.containsPoint(*decl)) {
                return std::nullopt;
            }
            arms.push_back({ static_cast<uint64_t>(range.translateIndex(*decl)), item.stmt });
        }
        if (arms.size() < 2) {
            return std::nullopt;
        }

        // Monotonic in IR bit position: ascending -> Ctz, descending -> Clz.
        bool ascending = true, descending = true;
        for (size_t k = 1; k < arms.size(); k++) {
            ascending  = ascending && arms[k].first > arms[k - 1].first;
            descending = descending && arms[k].first < arms[k - 1].first;
        }
        if (!ascending && !descending) {
            return std::nullopt;  // see TODO(unique) / TODO(repack).
        }
        return OneHotPlan{ base_expr, std::move(arms), !ascending, base_width };
    }

    void emit_onehot(const CaseStatement& c, const OneHotPlan& plan, ir::Builder& b) {
        const ir::Index base_val = lower_expr(*plan.base, b);
        ir::Index       selector = base_val;
        // Mask to the selected bits so a set base bit with no arm can't shadow a
        // higher-priority selected bit. Skipped when every base bit is an arm.
        if (plan.arms.size() != plan.base_width) {
            slang::SVInt       mask(plan.base_width, 0, false);
            const slang::SVInt one(plan.base_width, 1, false);
            for (const std::pair<uint64_t, const Statement*>& arm : plan.arms) {
                mask |= one.shl(static_cast<uint32_t>(arm.first));
            }
            const ir::Index mcst =
              b.constant(svint_to_ref(mask, lower_type(*plan.base->type)), loc(*plan.base));
            selector = b.binary(ir::Inst::Tag::Band, base_val, mcst, loc(*plan.base));
        }
        const ir::Index key =
          b.unary(plan.descending ? ir::Inst::Tag::Clz : ir::Inst::Tag::Ctz, selector, loc(c.expr));

        std::vector<std::vector<uint64_t>>       keystore;
        std::vector<ir::Builder::SwitchArmBuild> arms;
        keystore.reserve(plan.arms.size());
        arms.reserve(plan.arms.size());
        for (const std::pair<uint64_t, const Statement*>& arm : plan.arms) {
            b.open_region();
            lower_stmt(*arm.second, b);
            const ir::ExtraSlice reg = b.close_region();
            keystore.push_back({ arm.first });
            arms.push_back({ std::span<const uint64_t>(keystore.back()), reg });
        }
        b.open_region();
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        }
        const ir::ExtraSlice default_region = b.close_region();
        b.switch_(key, arms, default_region, loc(c.expr));
    }

    void lower_case(const CaseStatement& c, ir::Builder& b) {
        if (unwrap_conversion(c.expr).kind == ExpressionKind::TypeReference) {
            // type() case selector (LRM 6.23): a type reference is a compile-time
            // marker with no runtime value, so this resolves entirely at lowering
            // time via Type::isMatching, rather than through the packed-comparison
            // machinery below (which cannot represent a type-ref key). Not routed
            // through slang's own CaseStatement::getKnownBranch: that fold keys off
            // `c.expr`'s own (possibly Conversion-wrapped) ExpressionKind directly,
            // so a plain `case (type(x))` selector -- wrapped here just like any
            // other case selector -- would silently fail its TypeReference check.
            const slang::ast::Type& sel_ty =
                unwrap_conversion(c.expr).as<TypeReferenceExpression>().targetType;
            for (const auto& group : c.items) {
                for (const Expression* item : group.expressions) {
                    const Expression& ue = unwrap_conversion(*item);
                    if (ue.kind != ExpressionKind::TypeReference) {
                        fail("case statement over type() with a non-type() item is not supported");
                    }
                    if (ue.as<TypeReferenceExpression>().targetType.isMatching(sel_ty)) {
                        lower_stmt(*group.stmt, b);
                        return;
                    }
                }
            }
            if (c.defaultCase != nullptr) {
                lower_stmt(*c.defaultCase, b);
            }
            return;
        }
        if (is_string_expr(c.expr)) {
            // TODO: case over strings (labels compare with rt_string_cmp; the
            // common-type/resize machinery below is packed-only).
            fail("case statement over strings is not supported");
        }
        if (c.condition == CaseStatementCondition::Inside) {
            lower_case_inside(c, b);
            return;
        }
        ir::Inst::Tag cmp;
        switch (c.condition) {
            case CaseStatementCondition::Normal:
                cmp = ir::Inst::Tag::CaseEq;
                break;
            case CaseStatementCondition::WildcardJustZ:
                cmp = ir::Inst::Tag::CasezEq;
                break;
            case CaseStatementCondition::WildcardXOrZ:
                cmp = ir::Inst::Tag::CasexEq;
                break;
            default:
                fail("unsupported case condition");  // unreachable: Inside handled above.
        }
        // unique / unique0 / priority (c.check) add the LRM 12.5.3 violation
        // checks; first-match execution is conforming for all of them.
        //   - no-match (priority or unique, no default): kind-0 report. unique0
        //     does NOT report no-match; a default can never no-match.
        //   - overlap (unique / unique0, >1 item matches): kind-1 report,
        //     detected by emit_overlap_check summing every item's own predicate
        //     unconditionally -- independent of how dispatch is structured, so
        //     a switch run's own key dedup (see find_switch_run) never needs to
        //     abandon the switch just because a duplicate constant exists
        //     somewhere in the statement.
        using Check = slang::ast::UniquePriorityCheck;
        const bool no_match_report =
          (c.check == Check::Priority || c.check == Check::Unique) && c.defaultCase == nullptr;
        const bool needs_overlap = c.check == Check::Unique || c.check == Check::Unique0;

        // The one-hot reverse-case idiom `case (1'b1)` over base[i] selects lowers
        // to ctz/clz + jump table (check==None only; unique/priority keep the
        // chain, see build_onehot_plan's TODO(unique)).
        if (std::optional<OneHotPlan> oh = build_onehot_plan(c)) {
            emit_onehot(c, *oh, b);
            return;
        }

        const CaseCompareType ct        = common_case_type(c);
        const ir::Ref         ty        = ct.ty;
        const uint32_t        key_width = ct.four ? 2 * ct.width : ct.width;
        // The selector is evaluated exactly once, up front (12.5 / 12.5.3).
        const ir::Index        sel = b.resize(lower_expr(c.expr, b), ty, loc(c.expr));
        slang::ast::ASTContext astCtx(*scope_, slang::ast::LookupLocation::max);

        // unique / unique0 need the eager overlap check; priority / plain use the
        // lazy first-match chain. Both segment switch-eligible runs internally
        // (find_switch_run), so a whole-statement all-clean-constant case still
        // reproduces one jump table as the degenerate single-run case.
        if (needs_overlap) {
            lower_case_unique(c, sel, cmp, ty, ct.width, key_width, astCtx, no_match_report, b);
            return;
        }
        const std::vector<std::vector<char>> keep = case_dedup(c, ct.width, astCtx);
        lower_case_item(c, 0, sel, cmp, ty, ct.width, key_width, keep, astCtx, no_match_report, b);
    }

    // Eager lowering for unique / unique0 (LRM 12.5.3). Unlike the lazy
    // first-match chain, every case_item predicate is evaluated once up front
    // (the LRM permits any-order eager evaluation for unique/unique0: the
    // case_expression is evaluated once before the items, items may be compared
    // in any order, and side effects "may cause nondeterministic results"). The
    // predicates are held in temps and reused for both the overlap check and the
    // first-match dispatch, so labels are never re-evaluated.
    //
    // Overlap: sum the 1-bit predicates; > 1 match is a uniqueness violation
    // (kind 1). The check is marked unlikely so the report stays off the hot
    // path. First-match dispatch and the no-match report (unique only) reuse the
    // temps and match the plain-case semantics exactly.
    void lower_case_unique(const CaseStatement& c, ir::Index sel, ir::Inst::Tag cmp, ir::Ref ty,
                           uint32_t width, uint32_t key_width, const slang::ast::ASTContext& astCtx,
                           bool no_match_report, ir::Builder& b) {
        const size_t           n = c.items.size();
        std::vector<ir::Index> preds;
        preds.reserve(n);
        for (const CaseStatement::ItemGroup& item : c.items) {
            // A case_item's predicate is the OR of (selector <cmp> label) over its
            // labels; every item has at least one label. No dedup (a duplicate is
            // a reportable overlap, not a dead label).
            ir::Index pred{};
            bool      have = false;
            for (const Expression* e : item.expressions) {
                const ir::Index val = b.resize(lower_expr(*e, b), ty, loc(*e));
                const ir::Index one = b.binary(cmp, sel, val, loc(*e));
                pred                = have ? b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e)) : one;
                have                = true;
            }
            preds.push_back(pred);
        }

        emit_overlap_check(preds, case_banner(c), loc(c), b);
        dispatch_preds(c, 0, sel, width, key_width, astCtx, preds, no_match_report, b);
    }

    // Emit the unique/unique0 overlap check (LRM 12.5.3 / 12.4.2) from precomputed
    // 1-bit predicate temps: sum them and, on an unlikely branch, report a kind-1
    // violation when more than one matched. The predicates are definite 2-state
    // (=== / a reduced if-condition never yields x), so the zero-extended 32-bit
    // sum is exact for any realistic arm count. Shared by the case and if paths.
    void emit_overlap_check(const std::vector<ir::Index>& preds, const std::string& banner,
                            ir::SourceLoc rloc, ir::Builder& b) {
        const ir::Ref sumty = make_int_type(32, false, false);
        ir::Index     sum   = b.resize(preds[0], sumty, rloc);
        for (size_t k = 1; k < preds.size(); k++) {
            sum = b.binary(ir::Inst::Tag::Add, sum, b.resize(preds[k], sumty, rloc), rloc);
        }
        const ir::Index one_c   = b.constant(module_.pool.put_int(sumty, 1), rloc);
        const ir::Index overlap = b.binary(ir::Inst::Tag::Gt, sum, one_c, rloc);

        b.open_region();
        b.case_violation(banner, 1, rloc);
        const ir::ExtraSlice report_region = b.close_region();
        b.open_region();
        const ir::ExtraSlice empty_region = b.close_region();
        b.cond(overlap, report_region, empty_region, rloc, /*unlikely=*/true);
    }

    // First-match dispatch for a unique/unique0 case, reusing the precomputed
    // predicate temps: item k's body runs when preds[k] is the first true one.
    // The terminal else (no item matched) runs the default, else emits the
    // no-match report when the caller requested it (unique, no default).
    //
    // Tries find_switch_run first, same as the plain/priority chain -- but
    // its switch is built fresh from `sel` (a switch case value has to be the
    // raw selector, not an equality-result boolean), never from `preds[k]`.
    // `preds` remains used only by the caller's emit_overlap_check; passing
    // `keep = nullptr` here means find_switch_run dedups a run's own keys
    // locally (unique/unique0 skip the whole-statement case_dedup, since a
    // dropped duplicate there would be a swallowed overlap violation) -- the
    // overlap itself is still reported by the separate, already-computed
    // `preds` sum regardless of which items a run's switch absorbs.
    void dispatch_preds(const CaseStatement& c, size_t k, ir::Index sel, uint32_t width,
                        uint32_t key_width, const slang::ast::ASTContext& astCtx,
                        const std::vector<ir::Index>& preds, bool no_match_report, ir::Builder& b) {
        if (k == c.items.size()) {
            if (c.defaultCase != nullptr) {
                lower_stmt(*c.defaultCase, b);
            }
            else if (no_match_report) {
                b.case_violation(case_banner(c), 0, loc(c));
            }
            return;
        }

        if (std::optional<SwitchRun> run =
              find_switch_run(c, k, width, key_width, /*keep=*/nullptr, astCtx)) {
            if (run->arms.empty()) {
                dispatch_preds(c, k + run->len, sel, width, key_width, astCtx, preds, no_match_report, b);
                return;
            }
            const std::vector<ir::Builder::SwitchArmBuild> arms = lower_switch_arm_bodies(run->arms, b);
            b.open_region();
            dispatch_preds(c, k + run->len, sel, width, key_width, astCtx, preds, no_match_report, b);
            const ir::ExtraSlice default_region = b.close_region();
            b.switch_(sel, arms, default_region, loc(c.expr));
            return;
        }

        b.open_region();
        lower_stmt(*c.items[k].stmt, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        dispatch_preds(c, k + 1, sel, width, key_width, astCtx, preds, no_match_report, b);
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(preds[k], then_region, else_region, loc(*c.items[k].stmt));
    }

    // Emit the if-else chain for items[k..] into the current region: item k's
    // match test guards its body, with the rest of the chain (eventually the
    // default) as the else branch. Items are tested in textual order and the
    // first match wins; labels for items not yet reached are evaluated lazily,
    // inside the else regions, so they run only until a match is found.
    //
    // `report_viol` is set for a default-less priority/unique case: the terminal
    // else (reached only when no item matched) emits a no-match violation report
    // (LRM 12.5.3, kind 0). It is mutually exclusive with a default (a
    // default-less case has no default branch here). This is the lazy chain used
    // for priority / plain checks; unique/unique0 use the eager lower_case_unique.
    //
    // Tries find_switch_run at every step first: a run of clean-constant labels
    // jump-tables (generalizing the old whole-statement-only fast path to a
    // sub-range), with a non-constant/x-z-bearing item (or, for casex/casez,
    // any item at all -- see find_switch_run) falling through to the predicate
    // test below and recursing at k+1 as before.
    void lower_case_item(const CaseStatement& c, size_t k, ir::Index sel, ir::Inst::Tag cmp, ir::Ref ty,
                         uint32_t width, uint32_t key_width,
                         const std::vector<std::vector<char>>& keep,
                         const slang::ast::ASTContext& astCtx, bool report_viol, ir::Builder& b) {
        if (k == c.items.size()) {
            if (c.defaultCase != nullptr) {
                lower_stmt(*c.defaultCase, b);
            }
            else if (report_viol) {
                b.case_violation(case_banner(c), 0, loc(c));
            }
            return;
        }

        if (std::optional<SwitchRun> run = find_switch_run(c, k, width, key_width, &keep, astCtx)) {
            if (run->arms.empty()) {
                lower_case_item(c, k + run->len, sel, cmp, ty, width, key_width, keep, astCtx,
                                report_viol, b);
                return;
            }
            const std::vector<ir::Builder::SwitchArmBuild> arms = lower_switch_arm_bodies(run->arms, b);
            b.open_region();
            lower_case_item(c, k + run->len, sel, cmp, ty, width, key_width, keep, astCtx, report_viol, b);
            const ir::ExtraSlice default_region = b.close_region();
            b.switch_(sel, arms, default_region, loc(c.expr));
            return;
        }

        const CaseStatement::ItemGroup& item = c.items[k];

        // Match predicate: OR of (selector <cmp> label) over the group's labels,
        // skipping labels dropped by case_dedup.
        ir::Index pred{};
        bool      have = false;
        for (size_t j = 0; j < item.expressions.size(); j++) {
            if (!keep[k][j]) {
                continue;
            }
            const Expression* e   = item.expressions[j];
            const ir::Index   val = b.resize(lower_expr(*e, b), ty, loc(*e));
            const ir::Index   one = b.binary(cmp, sel, val, loc(*e));
            pred                  = have ? b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e)) : one;
            have                  = true;
        }
        if (!have) {
            // No surviving label (empty group, or every label deduped away): the
            // body is unreachable, so continue the chain in the current region.
            lower_case_item(c, k + 1, sel, cmp, ty, width, key_width, keep, astCtx, report_viol, b);
            return;
        }

        b.open_region();
        lower_stmt(*item.stmt, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        lower_case_item(c, k + 1, sel, cmp, ty, width, key_width, keep, astCtx, report_viol, b);
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(pred, then_region, else_region, loc(*item.stmt));
    }

    // -- System tasks --

    void lower_call(const CallExpression& call, ir::Builder& b) {
        if (!call.isSystemCall()) {
            // A user subroutine invoked as a statement: a function (void, or a
            // non-void whose result is discarded) or a task enable. Lower for side
            // effects; lower_user_call routes a task to lower_task_call.
            lower_user_call(call, b);
            return;
        }
        const std::string_view name = call.getSubroutineName();
        // A string method used as a statement (s.putc(i, c); the others are
        // legal-but-pointless): lower for its effect, discard the value.
        if (!name.empty() && name[0] != '$' && !call.arguments().empty() &&
            is_string_expr(*call.arguments()[0])) {
            lower_string_method(name, call, b);
            return;
        }
        // A dynamic-array method as a statement (arr.delete(); arr.size() is
        // legal-but-pointless): lower for effect, discard the value.
        if (!name.empty() && name[0] != '$' && !call.arguments().empty() &&
            is_dyn_array(*call.arguments()[0])) {
            lower_dyn_array_method(name, call, b, loc(call));
            return;
        }
        // A queue method as a statement (push/pop/insert/delete): lower for
        // effect, discard any returned element.
        if (!name.empty() && name[0] != '$' && !call.arguments().empty() &&
            is_queue(*call.arguments()[0])) {
            lower_queue_method(name, call, b, loc(call));
            return;
        }
        if (const char rx = display_radix(name, "$display")) {
            lower_display(call, /*newline=*/true, b, rx);
            return;
        }
        if (const char rx = display_radix(name, "$write")) {
            lower_display(call, /*newline=*/false, b, rx);
            return;
        }
        if (const char rx = display_radix(name, "$fdisplay")) {
            lower_fdisplay(call, /*newline=*/true, b, rx);
            return;
        }
        if (const char rx = display_radix(name, "$fwrite")) {
            lower_fdisplay(call, /*newline=*/false, b, rx);
            return;
        }
        if (const char rx = display_radix(name, "$swrite")) {
            lower_sformat(call, /*is_sformat=*/false, b, rx);
            return;
        }
        if (name == "$sformat") {
            lower_sformat(call, /*is_sformat=*/true, b);
            return;
        }
        if (name == "$fclose") {
            if (call.arguments().size() != 1) {
                fail("$fclose requires a single file-descriptor argument");
            }
            b.fclose(lower_expr(*call.arguments()[0], b), loc(call));
            return;
        }
        if (name == "$fgets" || name == "$fgetc" || name == "$ungetc" || name == "$feof" ||
            name == "$ftell" || name == "$fseek" || name == "$rewind" || name == "$ferror" ||
            name == "$fscanf" || name == "$sscanf" || name == "$fread" ||
            name == "$test$plusargs" || name == "$value$plusargs") {
            // File read / positioning / error / scan functions, and plusargs,
            // used in statement position: lower for the side effect (file
            // position, write-back) and drop the result.
            lower_sysfunc(call, b);
            return;
        }
        if (name == "$finish" || name == "$stop") {
            // Both end the simulation and exit. LRM §20.2 defines $stop as
            // suspending the run (vs $finish exiting to the OS) and mandates no
            // interactive mode; Clotho cannot resume a suspended run, so a
            // suspend-forever equals exit and the two are treated identically
            // apart from the banner's task name.
            //
            // The optional 0/1/2 argument selects the exit-message verbosity
            // (LRM Table 20-1); slang guarantees it is a constant, defaulting to
            // 1 when omitted. The runtime prints the banner (time + location) for
            // level >= 1.
            uint32_t level = 1;
            if (!call.arguments().empty()) {
                slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
                slang::ast::EvalContext    ectx(actx);
                const slang::ConstantValue cv = call.arguments()[0]->eval(ectx);
                if (cv.isInteger()) {
                    level = cv.integer().as<uint32_t>().value_or(1);
                }
            }
            b.finish(finish_banner(call, name), level, loc(call));
            return;
        }
        if (name == "$info" || name == "$warning" || name == "$error" || name == "$fatal") {
            lower_severity_task(call, name, b);
            return;
        }
        if (name == "$fflush") {
            // No-argument form flushes every open output buffer; the runtime reads
            // a 0 descriptor as "flush all" (LRM §21.3.6). The single-argument form
            // flushes one MCD / FD.
            ir::Index handle;
            if (call.arguments().empty()) {
                const ir::Ref u32 = make_int_type(32, false, false);
                handle            = b.constant(module_.pool.put_int(u32, 0), loc(call));
            } else {
                handle = lower_expr(*call.arguments()[0], b);
            }
            b.fflush(handle, loc(call));
            return;
        }
        if (name == "$printtimescale") {
            lower_printtimescale(call, b);
            return;
        }
        if (name == "$timeformat") {
            lower_timeformat(call, b);
            return;
        }
        if (name == "$readmemh" || name == "$readmemb") {
            lower_readmem(call, name == "$readmemh", b);
            return;
        }
        if (name == "$dumpfile") {
            // $dumpfile(name) (LRM §21.7.1.1): set the output path.
            if (call.arguments().size() != 1) {
                fail("$dumpfile requires one filename argument");
            }
            b.dump_file(lower_expr(*call.arguments()[0], b), loc(call));
            return;
        }
        if (name == "$dumpvars") {
            lower_dumpvars(call, b);
            return;
        }
        if (name == "$dumpoff") {
            b.dump_control(ir::Inst::Tag::DumpOff, loc(call));
            return;
        }
        if (name == "$dumpon") {
            b.dump_control(ir::Inst::Tag::DumpOn, loc(call));
            return;
        }
        if (name == "$dumpall") {
            b.dump_control(ir::Inst::Tag::DumpAll, loc(call));
            return;
        }
        if (name == "$dumpflush") {
            b.dump_control(ir::Inst::Tag::DumpFlush, loc(call));
            return;
        }
        fail("unsupported system task '" + std::string(name) + "'");
    }

    // $dumpvars (LRM §21.7.1.3). Phase 0 supports only the whole-design form:
    // $dumpvars, $dumpvars(0), or $dumpvars(0, <top>...) whose listed scopes are
    // exactly the design's top-module instances (so the request equals the whole
    // design). A level > 0 limits hierarchy depth and any other scope / variable
    // argument selects a subset; both are deferred (docs/VCD.md, sparse dumping) and
    // loud-fail rather than dumping the wrong set.
    void lower_dumpvars(const CallExpression& call, ir::Builder& b) {
        const auto args = call.arguments();
        if (!args.empty()) {
            const std::optional<int32_t> level = try_const_int(*args[0]);
            if (!level.has_value() || *level != 0) {
                fail("$dumpvars with a nonzero level is not supported (whole-design dump only)");
            }
            if (args.size() > 1) {
                const auto tops = scope_->getCompilation().getRoot().topInstances;
                std::vector<const slang::ast::Symbol*> listed;
                for (size_t k = 1; k < args.size(); ++k) {
                    const auto* as = args[k]->as_if<slang::ast::ArbitrarySymbolExpression>();
                    if (as == nullptr || as->symbol->kind != slang::ast::SymbolKind::Instance) {
                        fail("$dumpvars of a scope/variable subset is not supported (whole-design dump only)");
                    }
                    bool is_top = false;
                    for (const slang::ast::InstanceSymbol* t : tops) {
                        if (t == as->symbol.get()) {
                            is_top = true;
                            break;
                        }
                    }
                    if (!is_top) {
                        fail("$dumpvars of a non-top scope is not supported (whole-design dump only)");
                    }
                    listed.push_back(as->symbol.get());
                }
                // The list must cover every top, else the whole-design dump would be
                // a superset of what was requested.
                for (const slang::ast::InstanceSymbol* top : tops) {
                    bool covered = false;
                    for (const slang::ast::Symbol* l : listed) {
                        if (l == top) {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered) {
                        fail("$dumpvars must list every top module to dump the whole design");
                    }
                }
            }
        }
        b.dump_vars(loc(call));
    }

    // $readmemh / $readmemb (LRM §21.4): load a file into an unpacked-array memory.
    // Forms: (file, mem), (file, mem, start), (file, mem, start, finish). The memory
    // must be a whole unpacked array (a plain name or hierarchical reference); the
    // filename is a string literal, string-typed expression, or integral value
    // holding packed ASCII (LRM §21.4), and the optional address bounds are general
    // runtime values.
    void lower_readmem(const CallExpression& call, bool hex, ir::Builder& b) {
        const auto args = call.arguments();
        if (args.size() < 2 || args.size() > 4) {
            fail("$readmem requires 2 to 4 arguments");
        }
        // slang (post-8.0) binds the memory as a system-task output argument: an
        // AssignmentExpression whose left side is the target lvalue, the same
        // shape a user task's output actual has. Unwrap to the target.
        const Expression* mem_arg = args[1];
        if (mem_arg->kind == ExpressionKind::Assignment) {
            mem_arg = &mem_arg->as<AssignmentExpression>().left();
        }
        const Expression& mem = *mem_arg;
        const auto*       vsb = mem.as_if<slang::ast::ValueExpressionBase>();
        if (vsb == nullptr || mem.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() == nullptr) {
            // TODO: a sliced memory target (mem[a:b]) or a multidimensional slice.
            fail("$readmem target must be a whole unpacked-array memory");
        }
        const ir::Signal target   = signal_of(vsb->symbol);
        const ir::Index  filename = lower_expr(*args[0], b);
        ir::Index        start    = filename; // placeholder; unused unless has_start
        ir::Index        finish   = filename;
        const bool       has_start  = args.size() >= 3;
        const bool       has_finish = args.size() >= 4;
        if (has_start) {
            start = lower_expr(*args[2], b);
        }
        if (has_finish) {
            finish = lower_expr(*args[3], b);
        }
        b.read_mem(target, filename, start, finish, hex, has_start, has_finish, loc(call));
    }

    // $timeformat(units, precision, suffix, min_width): set the %t format for all
    // later %t specifiers (LRM §20.4.3). units / precision / min_width are general
    // runtime rvalues; the suffix must be a string literal.
    void lower_timeformat(const CallExpression& call, ir::Builder& b) {
        const auto args = call.arguments();
        // The no-argument form resets to the defaults; slang accepts it but
        // Clotho's runtime default is already seeded at startup, so it is unused
        // in practice. Require the full four-argument form for now.
        // TODO: the zero-argument $timeformat resets to the LRM default values.
        if (args.size() != 4) {
            fail("$timeformat requires four arguments (units, precision, suffix, min_width)");
        }

        // The suffix must be a literal string: dynamic string values have no
        // runtime representation yet, so only a compile-time literal is passed to
        // the runtime as (ptr, len).
        // TODO: a non-literal suffix (a string variable / expression) needs full
        // dynamic string support, which Clotho lacks.
        const Expression* suffix = args[2];
        while (suffix->kind == ExpressionKind::Conversion) {
            suffix = &suffix->as<ConversionExpression>().operand();
        }
        if (suffix->kind != ExpressionKind::StringLiteral) {
            fail("$timeformat suffix must be a string literal");
        }

        const ir::Index units     = lower_expr(*args[0], b);
        const ir::Index precision = lower_expr(*args[1], b);
        const ir::Index min_width = lower_expr(*args[3], b);
        b.timeformat(units, precision, suffix->as<StringLiteral>().getValue(), min_width, loc(call));
    }

    // $printtimescale [hier]: print "Time scale of (<path>) is <unit> / <prec>"
    // for a scope (LRM §21.3.1.2). With no argument the scope is the calling
    // module; the timescale and path are compile-time constants, so this folds to
    // a literal $display.
    void lower_printtimescale(const CallExpression& call, ir::Builder& b) {
        // TODO: a hierarchical-name argument selects another scope's timescale;
        // resolving it needs cross-hierarchy lookup, which Clotho lacks.
        if (!call.arguments().empty()) {
            fail("$printtimescale with a scope argument is not supported");
        }
        const auto ts = scope_->getTimeScale();
        if (!ts) {
            // TODO: a scope with no timescale prints the simulator default; Clotho
            // has no unitless $printtimescale text yet.
            fail("$printtimescale requires a timescale");
        }
        const std::string text = "Time scale of (" + instance_path() + ") is " + ts->toString();
        b.display(text, /*newline=*/true, {}, loc(call));
    }

    // Lower a #delay amount, scaled from the module's time unit to wheel ticks.
    // The unit multiplier is a compile-time constant; widen to 64 bits and
    // multiply so a large unit (e.g. seconds against fs precision) cannot
    // overflow the amount's declared width. No rounding is needed: an integer
    // delay times the unit is always an exact multiple of the precision (LRM
    // §3.14). Real (fractional) delays, which would round, are not yet supported.
    ir::Index scaled_delay(const Expression& amount, ir::Builder& b) {
        // TODO: fractional (real) delays round to the time precision, which
        // forces the wheel tick down to the smallest precision (LRM 22.7; see
        // the design_tick_fs note).
        if (amount.type->isFloating()) {
            fail("real (fractional) delay values are not supported");
        }
        const ir::Index v = lower_expr(amount, b);
        if (time_mul_ <= 1) {
            return v;
        }
        const ir::SourceLoc l   = loc(amount);
        const ir::Ref       u64 = make_int_type(64, false, false);
        const ir::Index     mul = b.constant(module_.pool.put_int(u64, time_mul_), l);

        return b.binary(ir::Inst::Tag::Mul, b.resize(v, u64, l), mul, l);
    }

    // Lower a string-valued system-task argument (a $fopen filename / mode) to a
    // packed-ASCII vector value. slang types these arguments as the dynamic
    // `string` type, wrapping a string literal in a conversion Clotho has no
    // representation for; strip those conversions to recover the packed byte
    // vector the runtime expects (byte 0 = last character). A genuine dynamic
    // string value has no packed form and is rejected.
    ir::Index lower_string_arg(const Expression& expr, ir::Builder& b) {
        const Expression* e = &expr;
        while (e->kind == ExpressionKind::Conversion && e->type->getCanonicalType().isString()) {
            e = &e->as<ConversionExpression>().operand();
        }
        if (e->type->getCanonicalType().isString()) {
            // TODO: a dynamic-string variable / expression as a filename or mode
            // needs runtime string support, which Clotho lacks.
            fail("$fopen with a dynamic-string argument is not supported");
        }
        return lower_expr(*e, b);
    }

    // Count the non-suppressed conversions in a $fscanf / $sscanf format literal,
    // rejecting unsupported conversion characters. Mirrors the runtime scanner: a
    // leading '*' suppresses (consumes no arg), an optional field width follows,
    // then the conversion char. Blocks %f/%e/%g (no reals) and %v/%m/%t/%u/%z.
    size_t count_scan_conversions(std::string_view fmt) {
        size_t n = 0;
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%') {
                continue;
            }
            ++i;
            if (i >= fmt.size()) {
                break;
            }
            if (fmt[i] == '%') {
                continue;
            }
            bool suppress = false;
            if (fmt[i] == '*') {
                suppress = true;
                ++i;
            }
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                ++i;
            }
            if (i >= fmt.size()) {
                break;
            }
            switch (fmt[i]) {
                case 'd':
                case 'o':
                case 'b':
                case 'h':
                case 'x':
                case 'c':
                case 's':
                    break;
                default:
                    // TODO: %f/%e/%g need real support; %v/%m/%t/%u/%z unimplemented.
                    fail(std::string("$fscanf / $sscanf conversion %") + fmt[i] + " is not supported");
            }
            if (!suppress) {
                ++n;
            }
        }
        return n;
    }

    // Lower $fscanf / $sscanf (LRM §21.3.4): the source is the first argument (an
    // FD for $fscanf, a string for $sscanf), the format is the second (a string
    // literal), and the rest are output lvalues, one per conversion.
    ir::Index lower_scanf(const CallExpression& call, bool is_sscanf, ir::Builder& b, ir::SourceLoc l) {
        const auto args = call.arguments();
        if (args.size() < 2) {
            fail("$fscanf / $sscanf requires a source and a format string");
        }
        const Expression* fmt_e = args[1];
        while (fmt_e->kind == ExpressionKind::Conversion) {
            fmt_e = &fmt_e->as<ConversionExpression>().operand();
        }
        if (fmt_e->kind != ExpressionKind::StringLiteral) {
            // TODO: a runtime (non-literal) format string needs dynamic string support.
            fail("$fscanf / $sscanf format must be a string literal");
        }
        const std::string_view fmt      = fmt_e->as<StringLiteral>().getValue();
        const size_t           want     = count_scan_conversions(fmt);
        const auto             out_args = args.subspan(2);
        if (out_args.size() != want) {
            fail("$fscanf / $sscanf has " + std::to_string(want) + " conversion(s) but " +
                 std::to_string(out_args.size()) + " output argument(s)");
        }
        // The source lowers as a value (FD) or a packed string ($sscanf).
        const ir::Index source = is_sscanf ? lower_string_arg(*args[0], b) : lower_expr(*args[0], b);
        std::vector<ir::Index> outs;
        outs.reserve(out_args.size());
        for (const Expression* a : out_args) {
            outs.push_back(lower_lvalue(*a, b));
        }
        return b.scanf(source, is_sscanf, fmt, outs, l);
    }

    // An enum member in declaration order: its constant value and source name.
    struct EnumMember {
        slang::SVInt     value;
        std::string_view name;
    };

    // Collect an enum type's members in declaration order (LRM 6.19: the order
    // the methods first/last/next/prev/name walk). Every member value is a clean
    // compile-time constant of the base type.
    std::vector<EnumMember> enum_members(const slang::ast::EnumType& et) {
        std::vector<EnumMember> members;
        for (const slang::ast::EnumValueSymbol& ev : et.values()) {
            members.push_back({ ev.getValue().integer(), ev.name });
        }
        return members;
    }

    // Lower an enum method (LRM 6.19.2-6.19.5). The receiver is arguments()[0].
    ir::Index lower_enum_method(std::string_view name, const slang::ast::EnumType& et,
                                const CallExpression& call, ir::Builder& b, ir::SourceLoc l) {
        const std::vector<EnumMember> members = enum_members(et);
        const ir::Ref                 base_ty = lower_type(et);

        // num(): the member count (LRM 6.19.4), an int, independent of the value.
        if (name == "num") {
            return b.constant(module_.pool.put_int(lower_type(*call.type), members.size()), l);
        }
        // first()/last(): the first/last member value (LRM 6.19.5), constant.
        if (name == "first") {
            return b.constant(svint_to_ref(members.front().value, base_ty), l);
        }
        if (name == "last") {
            return b.constant(svint_to_ref(members.back().value, base_ty), l);
        }

        // name() (LRM 6.19.2): the source name of the matching member, or "" if
        // the value is not a member. Delegated to a runtime scan of a per-type
        // member table (a string result rules out a frontend select).
        if (name == "name") {
            // Each member value becomes its must-plane bytes ((width+7)/8), so a
            // base wider than 64 bits round-trips; the runtime byte-compares.
            const uint32_t width  = et.bitWidth;
            const uint32_t vbytes = (width + 7) / 8;
            const uint32_t words  = (width + 63) / 64;
            std::vector<std::vector<uint8_t>>     valbufs;
            std::vector<ir::Builder::EnumNameRow> rows;
            valbufs.reserve(members.size());
            rows.reserve(members.size());
            for (const EnumMember& m : members) {
                const slang::SVInt& v      = m.value;
                const uint64_t*     raw    = v.getRawPtr();
                const bool          unk    = v.hasUnknown();
                const uint32_t      vwords = (static_cast<uint32_t>(v.getBitWidth()) + 63) / 64;
                std::vector<uint64_t> vplane(std::max(words, vwords), 0);
                for (uint32_t i = 0; i < vwords; ++i) {
                    // VPI aval (must plane): value bits, with unknown bits cleared.
                    vplane[i] = unk ? (raw[i] ^ raw[vwords + i]) : raw[i];
                }
                std::vector<uint8_t> vb(vbytes, 0);
                std::memcpy(vb.data(), vplane.data(), vbytes);
                valbufs.push_back(std::move(vb));
                rows.push_back({ std::span<const uint8_t>(valbufs.back()), m.name });
            }
            return b.enum_name(lower_expr(*call.arguments()[0], b), rows, l);
        }

        // next(N)/prev(N) (LRM 6.19.5): the Nth member after/before the current
        // one in declaration order, wrapping. N is an optional constant (default
        // 1); a non-member receiver yields X (matches iverilog; LRM leaves it
        // undefined). Lowered as a branchless packed select chain over the member
        // set — an integer select, so any base width works.
        if (name == "next" || name == "prev") {
            int64_t step = 1;
            if (call.arguments().size() >= 2) {
                slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
                slang::ast::EvalContext    ectx(actx);
                const slang::ConstantValue cv = call.arguments()[1]->eval(ectx);
                if (!cv.isInteger()) {
                    // TODO: a runtime (non-constant) step needs value->index lookup
                    // plus modular index arithmetic; rare, so reject for now.
                    fail("enum next/prev with a non-constant step is not supported");
                }
                step = cv.integer().as<int64_t>().value_or(0);
            }

            const int64_t num   = static_cast<int64_t>(members.size());
            const int64_t delta = (name == "next") ? step : -step;
            const auto    wrap  = [num](int64_t k) { return ((k % num) + num) % num; };

            const ir::Index recv = lower_expr(*call.arguments()[0], b);
            // Default (no member matched): X of the base type (2-state base cannot
            // hold X, so it degrades to 0 via svint_to_ref's must plane).
            ir::Index res = b.constant(
              svint_to_ref(et.isFourState ? slang::SVInt::createFillX(et.bitWidth, et.isSigned)
                                          : slang::SVInt(et.bitWidth, 0, et.isSigned),
                           base_ty),
              l);
            for (size_t i = members.size(); i-- > 0;) {
                const ir::Index memberc =
                  b.constant(svint_to_ref(members[i].value, base_ty), l);
                const ir::Index targetc = b.constant(
                  svint_to_ref(members[static_cast<size_t>(wrap(static_cast<int64_t>(i) + delta))].value,
                               base_ty),
                  l);
                const ir::Index eq = b.binary(ir::Inst::Tag::CaseEq, recv, memberc, l);
                res                = b.select(eq, targetc, res, l);
            }
            return res;
        }

        fail("enum method '" + std::string(name) + "' is not supported");
    }

    std::string format_typename(const slang::ast::Type& type) {
        const slang::ast::Type& t = type.getCanonicalType();

        // Non-default signing suffix (LRM 20.6.1 rule b).
        const auto signing = [](const slang::ast::Type& ty, bool default_signed) -> std::string {
            if (ty.isSigned() == default_signed) {
                return "";
            }
            return ty.isSigned() ? " signed" : " unsigned";
        };
        const auto range_str = [](slang::ConstantRange r) {
            return "[" + std::to_string(r.left) + ":" + std::to_string(r.right) + "]";
        };

        if (const auto* st = t.as_if<slang::ast::ScalarType>()) {
            // Predefined 1-bit types; default unsigned.
            const char* base = st->scalarKind == slang::ast::ScalarType::Bit    ? "bit"
                               : st->scalarKind == slang::ast::ScalarType::Reg ? "reg"
                                                                                : "logic";
            return base + signing(*st, false);
        }
        if (const auto* it = t.as_if<slang::ast::PredefinedIntegerType>()) {
            using K = slang::ast::PredefinedIntegerType::Kind;
            const char* base = it->integerKind == K::Int        ? "int"
                               : it->integerKind == K::Byte     ? "byte"
                               : it->integerKind == K::ShortInt ? "shortint"
                               : it->integerKind == K::LongInt  ? "longint"
                               : it->integerKind == K::Integer  ? "integer"
                                                                : "time";
            return base + signing(*it, slang::ast::PredefinedIntegerType::isDefaultSigned(it->integerKind));
        }
        if (const auto* pa = t.as_if<slang::ast::PackedArrayType>()) {
            // Collect the packed dimensions outermost-first down to the base
            // scalar; the whole vector's signing sits between base and dims.
            std::string             dims;
            const slang::ast::Type* cur = pa;
            while (const auto* p = cur->as_if<slang::ast::PackedArrayType>()) {
                dims += range_str(p->range);
                cur = &p->elementType.getCanonicalType();
            }
            const auto* base = cur->as_if<slang::ast::ScalarType>();
            if (base == nullptr) {
                // A packed array of enum / struct — unsupported base.
                fail("$typename of this packed type is not supported");
            }
            const char* name = base->scalarKind == slang::ast::ScalarType::Bit    ? "bit"
                               : base->scalarKind == slang::ast::ScalarType::Reg ? "reg"
                                                                                  : "logic";
            return name + signing(*pa, false) + dims;
        }
        if (const auto* ua = t.as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
            // Unpacked dimension: a "$" placeholder for the anonymous array name
            // (rule d), then the range (LRM: "logic[7:0]$[0:9]").
            return format_typename(ua->elementType) + "$" + range_str(ua->range);
        }
        if (t.isFloating()) {
            const auto& ft = t.as<slang::ast::FloatingType>();
            return ft.floatKind == slang::ast::FloatingType::ShortReal ? "shortreal"
                   : ft.floatKind == slang::ast::FloatingType::RealTime ? "realtime"
                                                                        : "real";
        }
        if (t.isString()) {
            return "string";
        }
        // enum / struct / union / class / interface / dynamic array / queue /
        // associative array / chandle / event — Clotho does not lower these, so
        // it cannot vouch for their $typename string.
        fail("$typename of this type is not supported");
    }

    // Classify a $countbits control-bit argument (LRM 20.9): a constant 1-bit
    // value naming which 4-state value that control tallies. A non-constant
    // control is rejected (it would need a runtime class-mask select).
    uint32_t count_control_class(const Expression& ctrl) {
        slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
        slang::ast::EvalContext    ectx(actx);
        const slang::ConstantValue cv = ctrl.eval(ectx);
        if (!cv.isInteger()) {
            fail("$countbits control bit must be a constant 0/1/x/z");
        }
        const slang::logic_t bit = cv.integer()[0];
        if (bit.value == slang::logic_t::X_VALUE) {
            return ir::CountX;
        }
        if (bit.value == slang::logic_t::Z_VALUE) {
            return ir::CountZ;
        }
        return bit.value != 0 ? ir::CountOne : ir::CountZero;
    }

    // Runtime lowering for the utility bit-query functions (LRM 20.9; $bits on a
    // dynamic string per 20.6.2) when the operand is not constant. A constant
    // operand folds in slang and never reaches here.
    ir::Index lower_bit_query(std::string_view name, const CallExpression& call, ir::Builder& b,
                              const ir::SourceLoc l) {
        const auto    args = call.arguments();
        const ir::Ref i32  = make_int_type(32, false, true);

        // $bits: the only non-constant operand Clotho stores is a dynamic
        // string, whose bit count is 8 per current character (LRM 6.16, 20.6.2).
        // A fixed-size operand would have const-folded above; any other dynamic
        // type (queue / dynamic array / class handle) is unsupported storage.
        if (name == "$bits") {
            if (args.size() != 1 || !is_string_expr(*args[0])) {
                fail("$bits of a non-constant, non-string operand is not supported");
            }
            const ir::Index len   = b.unary(ir::Inst::Tag::StrLen, lower_expr(*args[0], b), l);
            const ir::Index eight = b.constant(module_.pool.put_int(i32, 8), l);
            return b.resize(b.binary(ir::Inst::Tag::Mul, len, eight, l), lower_type(*call.type), l);
        }

        // The 20.9 counting family. The first argument is the counted value;
        // $countbits additionally carries constant control-bit arguments naming
        // which 4-state values to tally.
        const ir::Index val = lower_expr(*args[0], b);

        // $onehot / $onehot0 / $isunknown carry their qualifier in the tag.
        if (name == "$onehot") {
            return b.resize(b.unary(ir::Inst::Tag::OneHot, val, l), lower_type(*call.type), l);
        }
        if (name == "$onehot0") {
            return b.resize(b.unary(ir::Inst::Tag::OneHot0, val, l), lower_type(*call.type), l);
        }
        if (name == "$isunknown") {
            return b.resize(b.unary(ir::Inst::Tag::IsUnknown, val, l), lower_type(*call.type), l);
        }

        uint32_t mask = ir::CountOne; // $countones counts definite-1 bits
        if (name == "$countbits") {
            mask = 0;
            for (size_t k = 1; k < args.size(); ++k) {
                mask |= count_control_class(*args[k]);
            }
        }
        return b.resize(b.count_bits(val, mask, l), lower_type(*call.type), l);
    }

    // Lower a system function call used as an rvalue (e.g. `$time`). User function
    // calls are not yet supported.
    ir::Index lower_sysfunc(const CallExpression& call, ir::Builder& b) {
        if (!call.isSystemCall()) {
            fail("user function calls are not supported");
        }
        const std::string_view name = call.getSubroutineName();
        const ir::SourceLoc    l    = loc(call);

        // String built-in methods (s.len(), s.substr(i,j), ...) arrive as system
        // calls whose receiver is arguments()[0]; their names carry no '$'.
        if (!name.empty() && name[0] != '$' && !call.arguments().empty() &&
            is_string_expr(*call.arguments()[0])) {
            return lower_string_method(name, call, b);
        }

        // Dynamic-array built-in methods (arr.size(), LRM 7.5) arrive the same
        // way, with the array as arguments()[0].
        if (!name.empty() && name[0] != '$' && !call.arguments().empty() &&
            is_dyn_array(*call.arguments()[0])) {
            return lower_dyn_array_method(name, call, b, l);
        }

        // Queue built-in methods (q.size(), q.pop_front(), LRM 7.10.2) arrive the
        // same way, with the queue as arguments()[0].
        if (!name.empty() && name[0] != '$' && !call.arguments().empty() &&
            is_queue(*call.arguments()[0])) {
            return lower_queue_method(name, call, b, l);
        }

        // Enum methods (LRM 6.19.2-6.19.5) arrive the same way, with an enum-typed
        // receiver as arguments()[0].
        if (!name.empty() && name[0] != '$' && !call.arguments().empty()) {
            const slang::ast::Type& rct = call.arguments()[0]->type->getCanonicalType();
            if (const auto* et = rct.as_if<slang::ast::EnumType>()) {
                return lower_enum_method(name, *et, call, b, l);
            }
        }

        // `e.triggered` (LRM 15.5.3): 1-bit "triggered this time step" property.
        if (const Expression* ev = triggered_event(call)) {
            return b.event_triggered(event_signal(*ev), l);
        }

        if (fn_ctx_ != nullptr && !fn_ctx_->is_task &&
            (name == "$time" || name == "$stime" || name == "$realtime")) {
            // A function is a plain callable with no wheel pointer in its ABI, so
            // it cannot read the simulation clock. Reject rather than emit a
            // SimTime that would dereference a stale wheel. A task is a coroutine
            // and does carry the wheel, so $time is allowed there.
            // TODO: thread the wheel into the function ABI to support this.
            fail("$time / $stime / $realtime inside a function is not supported");
        }

        // $time: 64-bit, scaled to the calling module's time unit (LRM §20.3.1).
        // $stime: same value truncated to the low 32 bits (LRM §20.3.2). The
        // backend divides the wheel tick by time_mul_ (1 when unitless) to scale
        // the finest-precision clock down to the module unit; $stime resizes the
        // result to its 32-bit return type.
        if (name == "$time") {
            return b.sim_time(time_mul_, l);
        }
        if (name == "$stime") {
            return b.resize(b.sim_time(time_mul_, l), lower_type(*call.type), l);
        }

        // The seed operand of a $dist_* (an output arg, slang §hasOutputArgs) is
        // wrapped as Assignment{left = seed, right = EmptyArgument} — the same
        // placeholder shape slang uses for output ports. $random's seed is a
        // plain value arg. Reach the underlying seed lvalue in both cases.
        const auto seed_lvalue_expr = [](const Expression& a) -> const Expression& {
            const Expression& inner =
              a.kind == ExpressionKind::Assignment ? a.as<AssignmentExpression>().left() : a;
            return unwrap_conversion(inner);
        };

        // A $dist_* / seeded-$random result is a packed {seed:32, draw:32} i64.
        // Store the updated seed back to the inout `seed` lvalue and yield the
        // signed draw. Shared by $random(seed) and every $dist_* (LRM §20.15).
        const auto seed_writeback = [&](const Expression& seed_arg, ir::Index packed) -> ir::Index {
            const ir::Ref   i32s    = make_int_type(32, false, true);
            const ir::Index sh32    = b.constant(module_.pool.put_int(make_int_type(64, false, false), 32u), l);
            const ir::Index seed_lv = lower_lvalue(seed_lvalue_expr(seed_arg), b);
            b.binary(ir::Inst::Tag::Store, seed_lv,
                     b.resize(b.binary(ir::Inst::Tag::Srl, packed, sh32, l), i32s, l), l);
            return b.resize(packed, i32s, l);
        };

        // $random (LRM §20.15.1): a 32-bit signed PRNG draw. The plain form
        // returns the next value of the simulator's global random stream; the
        // seeded form `$random(seed)` is rtl_dist_uniform(seed, INT32_MIN,
        // INT32_MAX), lowered through the shared DistCall path so its inout-seed
        // writeback matches $dist_*.
        if (name == "$random") {
            if (call.arguments().empty()) {
                return b.random(l);
            }
            const Expression& seed_arg = *call.arguments()[0];
            const ir::Ref     i32s     = make_int_type(32, false, true);
            const ir::Index   lo = b.constant(module_.pool.put_int(i32s, 0x80000000ull), l);
            const ir::Index   hi = b.constant(module_.pool.put_int(i32s, 0x7fffffffull), l);
            const std::array<ir::Index, 3> args{ lower_expr(seed_lvalue_expr(seed_arg), b), lo, hi };
            return seed_writeback(seed_arg, b.dist_call(ir::DistOp::Uniform, args, l));
        }

        // $dist_* (LRM §20.15.2): the IEEE reference distributions, each with an
        // inout `seed` first argument and 1-2 distribution parameters. The
        // rtl_dist_* algorithms are mandated verbatim, so the stream is
        // byte-identical to any conforming tool for a given seed.
        struct DistSpec {
            const char* nm;
            ir::DistOp  op;
            size_t      nparam;
        };
        static constexpr DistSpec kDists[] = {
            { "$dist_uniform", ir::DistOp::Uniform, 2 },
            { "$dist_normal", ir::DistOp::Normal, 2 },
            { "$dist_exponential", ir::DistOp::Exponential, 1 },
            { "$dist_poisson", ir::DistOp::Poisson, 1 },
            { "$dist_chi_square", ir::DistOp::ChiSquare, 1 },
            { "$dist_t", ir::DistOp::StudentT, 1 },
            { "$dist_erlang", ir::DistOp::Erlang, 2 },
        };
        for (const DistSpec& d : kDists) {
            if (name != d.nm) {
                continue;
            }
            const auto argv = call.arguments();
            if (argv.size() != d.nparam + 1) {
                fail(std::string(d.nm) + " requires a seed and " + std::to_string(d.nparam) +
                     " parameter(s)");
            }
            std::vector<ir::Index> args;
            args.reserve(argv.size());
            // The seed (arg 0) is read as a bare integer lvalue; the distribution
            // parameters keep any value conversion slang inserted.
            args.push_back(lower_expr(seed_lvalue_expr(*argv[0]), b));
            for (size_t k = 1; k < argv.size(); ++k) {
                args.push_back(lower_expr(*argv[k], b));
            }
            return seed_writeback(*argv[0], b.dist_call(d.op, args, l));
        }

        // $urandom / $urandom_range (LRM §18.13): the per-process RNG lives in
        // the process's coroutine frame, so these are only legal in a process
        // context. A function/task body has no frame RNG (threading the caller's
        // RNG through the call ABI is future work — tracked in docs/TODO.md).
        if (name == "$urandom") {
            if (fn_ctx_ != nullptr) {
                fail("$urandom inside a function/task is not supported yet (no per-process "
                     "RNG in the call frame); call it directly from a process");
            }
            if (call.arguments().empty()) {
                return b.urandom(l);
            }
            return b.urandom_seeded(lower_expr(*call.arguments()[0], b), l);
        }
        if (name == "$urandom_range") {
            if (fn_ctx_ != nullptr) {
                fail("$urandom_range inside a function/task is not supported yet (no per-process "
                     "RNG in the call frame); call it directly from a process");
            }
            const auto argv = call.arguments();
            if (argv.empty() || argv.size() > 2) {
                fail("$urandom_range requires one or two arguments (maxval [, minval])");
            }
            const ir::Index maxv = lower_expr(*argv[0], b);
            const ir::Index minv =
              argv.size() == 2 ? lower_expr(*argv[1], b)
                               : b.constant(module_.pool.put_int(make_int_type(32, false, false), 0), l);
            return b.urandom_range(maxv, minv, l);
        }

        // $signed / $unsigned (LRM §20.10): reinterpret the self-determined
        // argument with the opposite signedness, preserving its size and value
        // bits. slang gives `call.type` the argument's width with the new sign;
        // a same-width resize leaves the bits untouched but tags the result type
        // so downstream widening sign- vs zero-extends correctly.
        if (name == "$signed" || name == "$unsigned") {
            if (call.arguments().size() != 1) {
                fail("$signed / $unsigned expects exactly one argument");
            }
            return b.resize(lower_expr(*call.arguments()[0], b), lower_type(*call.type), l);
        }

        // $clog2 (LRM §20.8.1): ceil(log2(arg)) with the argument treated as
        // unsigned and $clog2(0) == 0; the result is a 32-bit integer. slang's
        // constant evaluator implements clog2, so a constant argument (the
        // common `$clog2(PARAM)` / literal usage) folds to a pooled literal here.
        if (name == "$clog2") {
            if (call.arguments().size() != 1) {
                fail("$clog2 expects exactly one argument");
            }
            slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
            slang::ast::EvalContext    ectx(actx);
            const slang::ConstantValue cv = call.eval(ectx);
            if (cv.isInteger()) {
                return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
            }
            // A non-constant argument lowers to a runtime count-leading-zeros
            // (Clog2). The argument is self-determined and treated as unsigned
            // (LRM §20.8.1); its own width bounds the result.
            return b.unary(ir::Inst::Tag::Clog2, lower_expr(*call.arguments()[0], b), l);
        }

        // Utility query / bit-counting system functions (LRM 20.6, 20.9). Each
        // is a compile-time constant for a fixed-size operand: slang's constant
        // evaluator folds $bits / $countones / $countbits / $onehot / $onehot0 /
        // $isunknown to a literal (e.g. $bits queries the operand's type, never
        // its value), so the common constant-arg usage costs nothing at
        // runtime. Only a non-constant (runtime signal / dynamic string)
        // argument reaches the IR lowering.
        if (name == "$bits" || name == "$countones" || name == "$countbits" || name == "$onehot" ||
            name == "$onehot0" || name == "$isunknown") {
            slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
            slang::ast::EvalContext    ectx(actx);
            const slang::ConstantValue cv = call.eval(ectx);
            if (cv.isInteger()) {
                return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
            }
            return lower_bit_query(name, call, b, l);
        }

        // $size (LRM 20.7): const-folds in slang for a fixed-size array; a runtime
        // queue / dynamic array cannot fold and reaches here — it is exactly the
        // collection's .size() (the sole/first dimension is 0..len-1).
        if (name == "$size" && !call.arguments().empty()) {
            slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
            slang::ast::EvalContext    ectx(actx);
            const slang::ConstantValue cv = call.eval(ectx);
            if (cv.isInteger()) {
                return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
            }
            const Expression& arr = *call.arguments()[0];
            if (is_dyn_array(arr)) {
                return b.resize(b.dyn_array_size(lower_lvalue(arr, b), lower_type(*arr.type), l),
                                lower_type(*call.type), l);
            }
            if (is_queue(arr)) {
                return b.resize(b.queue_size(lower_lvalue(arr, b), lower_type(*arr.type), l),
                                lower_type(*call.type), l);
            }
            // TODO: $size(coll, dim) on a multidimensional dynamic collection.
            fail("$size of this operand is not supported");
        }

        // Remaining array query functions (LRM 20.7): $dimensions /
        // $unpacked_dimensions / $left / $right / $low / $high / $increment.
        // Each is a pure type/bounds query, so slang folds it to a 32-bit
        // constant for a fixed-size operand (the only kind these reach here for)
        // — including the optional dimension argument, an out-of-range dimension
        // (folds to 'x), and arbitrary packed/unpacked nesting. A non-constant
        // result is only possible for a dynamically-sized operand, which Clotho
        // does not lower for these queries yet ($size has its own runtime path
        // above).
        if (name == "$dimensions" || name == "$unpacked_dimensions" || name == "$left" ||
            name == "$right" || name == "$low" || name == "$high" || name == "$increment") {
            slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
            slang::ast::EvalContext    ectx(actx);
            const slang::ConstantValue cv = call.eval(ectx);
            if (cv.isInteger()) {
                return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
            }
            fail("array query function on a dynamically-sized operand is not supported");
        }

        // TODO: $typename (LRM 20.6.1). slang folds it to a string, but its type
        // $typename (LRM 20.6.1): the operand's resolved type name as a string
        // constant. Clotho formats the type itself (format_typename) rather than
        // trusting slang's Type::toString, so it emits only names for the type
        // kinds it actually lowers and controls the exact LRM format. The
        // argument's self-determined type is named; the type is resolved during
        // elaboration but never evaluated.
        if (name == "$typename") {
            if (call.arguments().size() != 1) {
                fail("$typename expects exactly one argument");
            }
            return b.constant(string_value_ref(format_typename(*call.arguments()[0]->type)), l);
        }

        // $fopen (LRM §21.3): one argument opens a multichannel descriptor (MCD)
        // for writing; two arguments open a single file descriptor (FD) with the
        // given C stdio mode string. The name / mode lower to packed-ASCII vectors
        // the backend spills to byte buffers, like $readmem's filename.
        if (name == "$fopen") {
            const auto args = call.arguments();
            if (args.empty() || args.size() > 2) {
                fail("$fopen expects one or two arguments");
            }
            const ir::Index fname = lower_string_arg(*args[0], b);
            if (args.size() == 2) {
                return b.fopen_fd(fname, lower_string_arg(*args[1], b), l);
            }
            return b.fopen_mcd(fname, l);
        }

        // $fgetc / $ungetc / $feof / $ftell / $fseek / $rewind (LRM §21.3): file
        // read / positioning functions, each returning a 32-bit integer. Operands
        // (a descriptor, plus a character or offset) are plain integer rvalues in
        // call order.
        if (name == "$fgetc" || name == "$feof" || name == "$ftell" || name == "$rewind" ||
            name == "$ungetc" || name == "$fseek") {
            ir::Inst::Tag tag;
            size_t        want;
            if (name == "$fgetc") {
                tag  = ir::Inst::Tag::Fgetc;
                want = 1;
            } else if (name == "$feof") {
                tag  = ir::Inst::Tag::Feof;
                want = 1;
            } else if (name == "$ftell") {
                tag  = ir::Inst::Tag::Ftell;
                want = 1;
            } else if (name == "$rewind") {
                tag  = ir::Inst::Tag::Rewind;
                want = 1;
            } else if (name == "$ungetc") {
                tag  = ir::Inst::Tag::Ungetc;
                want = 2;
            } else {
                tag  = ir::Inst::Tag::Fseek;
                want = 3;
            }
            const auto args = call.arguments();
            if (args.size() != want) {
                fail(std::string(name) + " expects " + std::to_string(want) + " argument(s)");
            }
            std::vector<ir::Index> vals;
            vals.reserve(args.size());
            for (const Expression* a : args) {
                vals.push_back(lower_expr(*a, b));
            }
            if (want == 1) {
                return b.unary(tag, vals[0], l);
            }
            if (want == 2) {
                return b.binary(tag, vals[0], vals[1], l);
            }
            return b.fseek(vals[0], vals[1], vals[2], l);
        }

        // $fgets(str, fd) (LRM §21.3.4): read a line into the packed-vector lvalue
        // `str`, returning the character count. `str` is an output argument.
        if (name == "$fgets") {
            const auto args = call.arguments();
            if (args.size() != 2) {
                fail("$fgets expects two arguments (str, fd)");
            }
            const ir::Index dst = lower_lvalue(*args[0], b);
            const ir::Index fd  = lower_expr(*args[1], b);
            return b.fgets(dst, fd, l);
        }

        // $ferror(fd, str) (LRM §21.3.6): write the descriptor's error message into
        // the string lvalue `str`, returning the error code.
        if (name == "$ferror") {
            const auto args = call.arguments();
            if (args.size() != 2) {
                fail("$ferror expects two arguments (fd, str)");
            }
            const ir::Index fd  = lower_expr(*args[0], b);
            const ir::Index dst = lower_lvalue(*args[1], b);
            return b.ferror(dst, fd, l);
        }

        // $fscanf(fd, fmt, args) / $sscanf(str, fmt, args) (LRM §21.3.4).
        if (name == "$fscanf" || name == "$sscanf") {
            return lower_scanf(call, name == "$sscanf", b, l);
        }

        // $test$plusargs(str) (LRM §21.6): nonzero iff a command-line plusarg's
        // prefix matches `str`. `str` must be a compile-time string literal for
        // now (a runtime / integral-variable string argument, which the LRM
        // also allows, is a documented gap — matches $fscanf/$sscanf's own
        // literal-format-string requirement above).
        if (name == "$test$plusargs") {
            const auto args = call.arguments();
            if (args.size() != 1) {
                fail("$test$plusargs expects one argument");
            }
            const Expression* e = args[0];
            while (e->kind == ExpressionKind::Conversion) {
                e = &e->as<ConversionExpression>().operand();
            }
            if (e->kind != ExpressionKind::StringLiteral) {
                // TODO: a runtime (non-literal) plusarg-string argument.
                fail("$test$plusargs argument must be a string literal");
            }
            return b.test_plusargs(e->as<StringLiteral>().getValue(), l);
        }

        // $value$plusargs(user_string, var) (LRM §21.6). `user_string` must be
        // a compile-time string literal of the form "plusarg_string%[0-9]*conv"
        // for now (a runtime user_string, which the LRM's own ieee1364_example
        // exercises via a `pstring` variable, is a documented gap). Supported
        // conversions: d/o/b/h/x/s; %e/%f/%g need real support (the same gap
        // as $fscanf/$sscanf's own %f/%e/%g, see count_scan_conversions).
        if (name == "$value$plusargs") {
            const auto args = call.arguments();
            if (args.size() != 2) {
                fail("$value$plusargs expects two arguments (user_string, var)");
            }
            const Expression* e = args[0];
            while (e->kind == ExpressionKind::Conversion) {
                e = &e->as<ConversionExpression>().operand();
            }
            if (e->kind != ExpressionKind::StringLiteral) {
                // TODO: a runtime (non-literal) user_string argument.
                fail("$value$plusargs user_string must be a string literal");
            }
            const std::string_view us  = e->as<StringLiteral>().getValue();
            const size_t            pct = us.rfind('%');
            if (pct == std::string_view::npos) {
                fail("$value$plusargs user_string must end in a %conversion");
            }
            size_t i = pct + 1;
            while (i < us.size() && us[i] >= '0' && us[i] <= '9') {
                ++i; // "leading 0 forms" (LRM §21.6) — no width semantics here.
            }
            if (i >= us.size() || i + 1 != us.size()) {
                fail("$value$plusargs user_string must end in a %[0-9]*<conv>");
            }
            const char conv =
              static_cast<char>(std::tolower(static_cast<unsigned char>(us[i])));
            switch (conv) {
                case 'd':
                case 'o':
                case 'b':
                case 'h':
                case 'x':
                case 's':
                    break;
                default:
                    // TODO: %e/%f/%g need real support.
                    fail(std::string("$value$plusargs conversion %") + us[i] + " is not supported");
            }
            const std::string_view prefix = us.substr(0, pct);
            const ir::Index        dst    = lower_lvalue(*args[1], b);
            return b.value_plusargs(dst, prefix, conv, l);
        }

        // $sformatf(fmt, args) (LRM §21.3.3): format into a new string value.
        if (name == "$sformatf") {
            return lower_sformatf(call, b);
        }

        // $fread(target, fd[, start[, count]]) (LRM §21.3.4): binary read into a
        // single vector or a whole unpacked-array memory. The memory form (like
        // $readmem) takes optional start-address and element-count bounds.
        if (name == "$fread") {
            const auto args = call.arguments();
            if (args.size() < 2 || args.size() > 4) {
                fail("$fread requires 2 to 4 arguments");
            }
            // slang (post-8.0) binds the target as a system-task output argument;
            // unwrap the AssignmentExpression to the lvalue (see lower_lvalue).
            const Expression* target_arg = args[0];
            if (target_arg->kind == ExpressionKind::Assignment) {
                target_arg = &target_arg->as<AssignmentExpression>().left();
            }
            const Expression& target = *target_arg;
            if (is_unpacked_array(target)) {
                const auto* vsb = target.as_if<slang::ast::ValueExpressionBase>();
                if (vsb == nullptr) {
                    // TODO: a sliced memory target (mem[a:b]).
                    fail("$fread memory target must be a whole unpacked-array memory");
                }
                const ir::Signal mem       = signal_of(vsb->symbol);
                const ir::Index  fd        = lower_expr(*args[1], b);
                const bool       has_start = args.size() >= 3;
                const bool       has_count = args.size() >= 4;
                ir::Index        start     = fd; // placeholder unless present
                ir::Index        cnt       = fd;
                if (has_start) {
                    start = lower_expr(*args[2], b);
                }
                if (has_count) {
                    cnt = lower_expr(*args[3], b);
                }
                return b.fread_mem(fd, mem, start, cnt, has_start, has_count, l);
            }
            if (args.size() != 2) {
                fail("$fread into a scalar variable takes exactly two arguments");
            }
            const ir::Index dst = lower_lvalue(target, b);
            const ir::Index fd  = lower_expr(*args[1], b);
            return b.fread_word(dst, fd, l);
        }

        // Real conversion functions (LRM §20.5). Constant contexts never reach
        // here — slang folds them.
        if (name == "$rtoi" || name == "$itor" || name == "$realtobits" || name == "$bitstoreal" ||
            name == "$shortrealtobits" || name == "$bitstoshortreal") {
            if (call.arguments().size() != 1) {
                fail(std::string(name) + " expects exactly one argument");
            }
            // slang has converted the argument to the function's formal type
            // (real / shortreal / 64- or 32-bit vector), so only the result
            // conversion is lowered here.
            const ir::Index v = lower_expr(*call.arguments()[0], b);
            if (name == "$rtoi") {
                // Truncates toward zero — unlike assignment conversion, which
                // rounds (LRM 20.5 vs 6.12.1).
                return b.float_cvt(ir::Inst::Tag::FtoITrunc, v, lower_type(*call.type), l);
            }
            if (name == "$itor") {
                return b.float_cvt(ir::Inst::Tag::ItoF, v, lower_type(*call.type), l);
            }
            // The four bit-pattern reinterpretations.
            return b.float_cvt(ir::Inst::Tag::FBitcast, v, lower_type(*call.type), l);
        }

        // Real math functions (LRM §20.8.2, Table 20-4): each matches the
        // equivalent C math library function. slang converts every argument to
        // real. Constant contexts fold in slang and never reach here.
        {
            static constexpr std::pair<std::string_view, ir::RealMathOp> kMath[] = {
                { "$ln", ir::RealMathOp::Ln },       { "$log10", ir::RealMathOp::Log10 },
                { "$exp", ir::RealMathOp::Exp },     { "$sqrt", ir::RealMathOp::Sqrt },
                { "$pow", ir::RealMathOp::Pow },     { "$floor", ir::RealMathOp::Floor },
                { "$ceil", ir::RealMathOp::Ceil },   { "$sin", ir::RealMathOp::Sin },
                { "$cos", ir::RealMathOp::Cos },     { "$tan", ir::RealMathOp::Tan },
                { "$asin", ir::RealMathOp::Asin },   { "$acos", ir::RealMathOp::Acos },
                { "$atan", ir::RealMathOp::Atan },   { "$atan2", ir::RealMathOp::Atan2 },
                { "$hypot", ir::RealMathOp::Hypot }, { "$sinh", ir::RealMathOp::Sinh },
                { "$cosh", ir::RealMathOp::Cosh },   { "$tanh", ir::RealMathOp::Tanh },
                { "$asinh", ir::RealMathOp::Asinh }, { "$acosh", ir::RealMathOp::Acosh },
                { "$atanh", ir::RealMathOp::Atanh },
            };
            for (const auto& [fname, op] : kMath) {
                if (name != fname) {
                    continue;
                }
                const bool two = op == ir::RealMathOp::Pow || op == ir::RealMathOp::Atan2 ||
                                 op == ir::RealMathOp::Hypot;
                const auto args = call.arguments();
                if (args.size() != (two ? 2u : 1u)) {
                    fail(std::string(name) + " expects " + (two ? "two arguments" : "one argument"));
                }
                std::vector<ir::Index> vals;
                for (const Expression* a : args) {
                    vals.push_back(lower_expr(*a, b));
                }
                return b.real_math(op, vals, l);
            }
        }

        // $realtime (LRM §20.3.3): the raw wheel tick count as a real, divided
        // by the unit multiplier so the fractional part below the module time
        // unit survives ($time's round-to-unit does not apply).
        if (name == "$realtime") {
            const ir::Index ticks = b.float_cvt(ir::Inst::Tag::ItoF, b.sim_time(1, l), module_.pool.real_type, l);
            if (time_mul_ <= 1) {
                return ticks;
            }
            const ir::Index mul =
              b.constant(module_.pool.put_real(module_.pool.real_type, static_cast<double>(time_mul_)), l);
            return b.binary(ir::Inst::Tag::FDiv, ticks, mul, l);
        }
        fail("unsupported system function '" + std::string(name) + "'");
    }

    // Lower a built-in string method call (LRM 6.16.x). slang models these as
    // system subroutines with the receiver as arguments()[0]. Read-only methods
    // take the receiver as an rvalue; putc mutates it in place through its
    // lvalue.
    ir::Index lower_string_method(std::string_view name, const CallExpression& call, ir::Builder& b) {
        const ir::SourceLoc l    = loc(call);
        const auto          args = call.arguments();
        const auto          recv = [&] { return lower_expr(*args[0], b); };
        using T                  = ir::Inst::Tag;

        if (name == "len") {
            return b.unary(T::StrLen, recv(), l);
        }
        if (name == "getc" && args.size() == 2) {
            return b.binary(T::StrGetc, recv(), lower_expr(*args[1], b), l);
        }
        if (name == "toupper") {
            return b.unary(T::StrUpper, recv(), l);
        }
        if (name == "tolower") {
            return b.unary(T::StrLower, recv(), l);
        }
        if (name == "compare" && args.size() == 2) {
            return b.binary(T::StrCmp, recv(), lower_expr(*args[1], b), l);
        }
        if (name == "icompare" && args.size() == 2) {
            return b.binary(T::StrICmp, recv(), lower_expr(*args[1], b), l);
        }
        if (name == "substr" && args.size() == 3) {
            const ir::Index s = recv();
            const ir::Index i = lower_expr(*args[1], b);
            const ir::Index j = lower_expr(*args[2], b);
            return b.str_substr(s, i, j, l);
        }
        if (name == "putc" && args.size() == 3) {
            const ir::Index str = lower_lvalue(*args[0], b);
            const ir::Index idx = lower_expr(*args[1], b);
            return b.str_putc(str, idx, lower_expr(*args[2], b), l);
        }
        if (name == "atoi") {
            return b.unary(T::StrAtoi, recv(), l);
        }
        if (name == "atohex") {
            return b.unary(T::StrAtohex, recv(), l);
        }
        if (name == "atooct") {
            return b.unary(T::StrAtooct, recv(), l);
        }
        if (name == "atobin") {
            return b.unary(T::StrAtobin, recv(), l);
        }
        if (name == "atoreal") {
            return b.unary(T::StrAtoreal, recv(), l);
        }
        if (name == "itoa" && args.size() == 2) {
            const ir::Index str = lower_lvalue(*args[0], b);
            return b.binary(T::StrItoa, str, lower_expr(*args[1], b), l);
        }
        if (name == "hextoa" && args.size() == 2) {
            const ir::Index str = lower_lvalue(*args[0], b);
            return b.binary(T::StrHextoa, str, lower_expr(*args[1], b), l);
        }
        if (name == "octtoa" && args.size() == 2) {
            const ir::Index str = lower_lvalue(*args[0], b);
            return b.binary(T::StrOctoa, str, lower_expr(*args[1], b), l);
        }
        if (name == "bintoa" && args.size() == 2) {
            const ir::Index str = lower_lvalue(*args[0], b);
            return b.binary(T::StrBintoa, str, lower_expr(*args[1], b), l);
        }
        if (name == "realtoa" && args.size() == 2) {
            const ir::Index str = lower_lvalue(*args[0], b);
            return b.binary(T::StrRealtoa, str, lower_expr(*args[1], b), l);
        }
        fail("string method '" + std::string(name) + "' is not supported");
    }

    // Build the combined format string and value-arg list of a $display-family
    // task from `format_args` (the arguments after any leading descriptor). A
    // string-literal argument contributes its text verbatim (its %-specifiers pull
    // from following value args, SV-style); a bare value argument not covered by an
    // earlier specifier gets a default `%0d` (or `%s` for a string object).
    // Hierarchical path of the instance / generate scope currently being lowered
    // ("top.u1.blk"): the flattened prefix, with the elided single-top name
    // restored.
    std::string instance_path() const {
        std::string path = top_elided_ ? module_.name : std::string();
        if (!prefix_.empty()) {
            if (!path.empty()) {
                path += '.';
            }
            path.append(prefix_, 0, prefix_.size() - 1);
        }
        return path;
    }

    // Hierarchical name of the scope invoking the statement currently being
    // lowered (LRM §21.2.1.5 %m): the instance path plus the enclosing
    // subroutine and named begin blocks. Static per lowering site, because
    // instances are flattened per-instance and subroutine bodies are lowered
    // per defining instance.
    std::string hier_name() const {
        std::string path = instance_path();
        if (fn_ctx_ != nullptr && !fn_ctx_->name_prefix.empty()) {
            path += '.';
            path.append(fn_ctx_->name_prefix, 0, fn_ctx_->name_prefix.size() - 1);
        }
        for (const std::string_view nb : named_blocks_) {
            path += '.';
            path += nb;
        }
        return path;
    }

    std::string lib_cell_name() const {
        return "work." + scope_stack_.back()->def_name;
    }

    // Validate one non-%% specifier parsed from a display format literal and
    // return its lowercase conversion char. Everything the engine does not
    // implement fails loudly here, so the runtime renderer only ever sees legal
    // specifiers (LRM §21.2.1.1: an undefined specifier is an error).
    char check_specifier(const clotho::FormatSpec& spec) {
        if (!spec.ok) {
            fail("incomplete format specifier in display format");
        }
        if (spec.width_overflow) {
            fail("format field width is out of range");
        }
        const char conv = static_cast<char>(spec.conv | 0x20);
        switch (conv) {
            case 'd':
            case 'h':
            case 'x':
            case 'o':
            case 'b':
            case 'c':
            case 's':
            case 't':
            case 'm':
            case 'l':
                break;
            case 'u':
            case 'z':
                if (spec.has_width || spec.left_justify) {
                    fail("%u / %z output is unformatted; width and justification do not apply");
                }
                break;
            case 'e':
            case 'f':
            case 'g':
                // Real conversions take the full C formatting specification
                // (LRM §21.2.1.2 Table 21-2), including a precision.
                return conv;
            case 'v':
                // TODO: %v needs net strength modeling (LRM §21.2.1.4).
                fail("%v (net signal strength) is not supported");
            case 'p':
                // TODO: %p needs aggregate rendering (LRM §21.2.1.6).
                fail("%p (assignment pattern) is not supported");
            default:
                fail(std::string("undefined format specifier '%") + spec.conv +
                     "' (LRM 21.2.1.1)");
        }
        if (spec.has_prec) {
            fail("a format precision is only defined for real specifiers (%e / %f / %g)");
        }
        return conv;
    }

    // Copy one string-literal argument into `fmt`, validating every specifier,
    // splicing %m as literal text, and appending each argument-consuming
    // conversion char to `pending`.
    void append_format_literal(std::string& fmt, std::string_view text, std::vector<char>& pending) {
        for (size_t i = 0; i < text.size();) {
            if (text[i] != '%') {
                fmt += text[i++];
                continue;
            }
            const clotho::FormatSpec spec = clotho::parse_format_spec(text.substr(i + 1));
            if (spec.ok && spec.conv == '%') {
                fmt += "%%";
                i += 2;
                continue;
            }
            const char conv = check_specifier(spec);
            if (conv == 'm' || conv == 'l') {
                // %m / %l consume no argument; both are static at lower time
                // (the invoking scope's hierarchical name, or its library.cell),
                // so splice the text in directly (width-padded).
                std::string name = conv == 'm' ? hier_name() : lib_cell_name();
                if (spec.has_width && name.size() < spec.width) {
                    if (spec.left_justify) {
                        name.append(spec.width - name.size(), ' ');
                    } else {
                        name.insert(0, spec.width - name.size(), ' ');
                    }
                }
                fmt += name;
            } else {
                fmt.append(text, i, 1 + spec.len);
                pending.push_back(conv);
            }
            i += 1 + spec.len;
        }
    }

    void build_format(std::span<const Expression* const> format_args, std::string& fmt,
                      std::vector<ir::Index>& args, ir::Builder& b, char default_conv = 'd') {
        // Conversion chars (lowercase) of specifiers seen but not yet matched to
        // a value arg, in order (FIFO). Needed so a `%t` argument can be scaled
        // to ticks and a string object checked against %s.
        std::vector<char> pending;
        size_t            next = 0; // front of `pending`

        for (const Expression* arg : format_args) {
            if (arg->kind == ExpressionKind::EmptyArgument) {
                // An empty argument (adjacent commas) displays one space
                // (LRM §21.2.1).
                fmt += ' ';
                continue;
            }
            // A string-literal argument (SV-style) contributes format text, even
            // when width-converted; unwrap conversions to see the literal. A
            // literal that an earlier specifier is still waiting on is a value
            // argument instead ($display("%s", "hi") prints hi).
            const Expression* lit = arg;
            while (lit->kind == ExpressionKind::Conversion) {
                lit = &lit->as<ConversionExpression>().operand();
            }
            if (lit->kind == ExpressionKind::StringLiteral && next >= pending.size()) {
                append_format_literal(fmt, lit->as<StringLiteral>().getValue(), pending);
            } else {
                const bool str  = is_string_expr(*arg);
                const bool fl   = arg->type->isFloating();
                char       conv = str ? 's' : (fl ? 'g' : default_conv);
                if (next < pending.size()) {
                    conv = pending[next++]; // consumed by an earlier specifier
                } else if (fl) {
                    // Bare real: the "default decimal format" (LRM §21.2.1.1)
                    // is unpinned for reals; %#g (6 significant digits,
                    // trailing zeros kept) matches iverilog byte for byte.
                    fmt += "%#g";
                } else {
                    // Bare value default: the task's radix, auto-sized
                    // (LRM §21.2.1.1); a string object formats as a string.
                    fmt += '%';
                    fmt += str ? 's' : default_conv;
                }
                if (str && conv != 's') {
                    // TODO: %d/%h/... of a string object (would need the packed
                    // reinterpretation of its characters).
                    fail("a string argument to $display/$write requires %s");
                }
                const ir::SourceLoc al = loc(*arg);
                const bool real_conv =
                  conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G';
                ir::Index v = lower_expr(*arg, b);
                if (fl && !real_conv) {
                    switch (conv) {
                        case 'd':
                            // A real under %d prints its rounded integer value
                            // (LRM 6.12.1 conversion; matches iverilog).
                            v = b.float_cvt(ir::Inst::Tag::FtoI, v, make_int_type(64, false, true), al);
                            break;
                        case 't':
                            break; // scale_to_ticks below has the real path
                        default:
                            // %h / %o / %b / %c take integral operands only
                            // (LRM 21.2.1.1).
                            fail(std::string("%") + conv + " with a real argument is not supported");
                    }
                } else if (!fl && real_conv) {
                    // An integral argument under %e/%f/%g converts to real
                    // (LRM 6.12.1) so the runtime formats one representation.
                    v = b.float_cvt(ir::Inst::Tag::ItoF, v, module_.pool.real_type, al);
                }
                if (conv == 't') {
                    v = scale_to_ticks(*arg, v, b);
                }
                args.push_back(v);
            }
        }
        if (next < pending.size()) {
            fail("display format has more format specifiers than arguments (LRM 21.2.1.1)");
        }
    }

    // Match a display-family task name against `base` plus an optional radix
    // suffix ($displayb / $writeh / ...); returns the bare-argument default
    // conversion ('d' for the plain name, the suffix's radix otherwise) or 0
    // when the name does not match (LRM §21.2.1.1).
    static char display_radix(std::string_view name, std::string_view base) {
        if (!name.starts_with(base)) {
            return 0;
        }
        const std::string_view rest = name.substr(base.size());
        if (rest.empty()) {
            return 'd';
        }
        if (rest == "b" || rest == "o" || rest == "h") {
            return rest[0];
        }
        return 0;
    }

    // Lower $display / $write: format the arguments and write to standard output.
    // Build the $display-style message from `msg_args` and emit a severity report
    // (LRM 20.10 / 16.3) at `level`. Non-terminating; $fatal adds a finish after.
    void emit_severity(ir::Severity level, const std::string& banner,
                       std::span<const Expression* const> msg_args, const CallExpression& call,
                       ir::Builder& b) {
        std::string            fmt;
        std::vector<ir::Index> args;
        build_format(msg_args, fmt, args, b);
        b.severity(static_cast<uint32_t>(level), banner, fmt, args, loc(call));
    }

    // Lower a severity system task: $info / $warning / $error / $fatal (LRM
    // 20.10). Each renders an optional $display-style message tail. $fatal's first
    // argument is the $finish verbosity level (a constant 0/1/2), the rest the
    // message; it reports, then terminates via $finish(level).
    void lower_severity_task(const CallExpression& call, std::string_view name, ir::Builder& b) {
        const std::string banner = banner_at(call.sourceRange.start());
        const auto        args   = call.arguments();
        if (name == "$fatal") {
            uint32_t                           level = 1;
            std::span<const Expression* const> msg   = args;
            if (!args.empty()) {
                slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
                slang::ast::EvalContext    ectx(actx);
                const slang::ConstantValue cv = args[0]->eval(ectx);
                if (cv.isInteger()) {
                    level = cv.integer().as<uint32_t>().value_or(1);
                }
                msg = args.subspan(1);
            }
            emit_severity(ir::Severity::Fatal, banner, msg, call, b);
            b.finish(finish_banner(call, "$fatal"), level, loc(call));
            return;
        }
        const ir::Severity sev = name == "$info"      ? ir::Severity::Info
                                 : name == "$warning" ? ir::Severity::Warning
                                                      : ir::Severity::Error;
        emit_severity(sev, banner, args, call, b);
    }

    void lower_display(const CallExpression& call, bool newline, ir::Builder& b, char default_conv) {
        std::string            fmt;
        std::vector<ir::Index> args;
        build_format(call.arguments(), fmt, args, b, default_conv);
        b.display(fmt, newline, args, loc(call));
    }

    // Lower $fdisplay / $fwrite (LRM §21.3.3): the first argument is the
    // destination descriptor (an MCD or FD); the rest form the message.
    void lower_fdisplay(const CallExpression& call, bool newline, ir::Builder& b, char default_conv) {
        const auto args_in = call.arguments();
        if (args_in.empty()) {
            fail("$fdisplay / $fwrite requires a file-descriptor argument");
        }
        const ir::Index       fd = lower_expr(*args_in[0], b);
        std::string            fmt;
        std::vector<ir::Index> args;
        build_format(args_in.subspan(1), fmt, args, b, default_conv);
        b.fdisplay(fd, fmt, newline, args, loc(call));
    }

    // Lower $sformat / $swrite (LRM §21.3.3): render the trailing arguments like
    // $write and store the result into the destination string lvalue (the first
    // argument). $sformat's format (its second argument) must be a string literal;
    // $swrite takes $write-style interleaved literals and values.
    void lower_sformat(const CallExpression& call, bool is_sformat, ir::Builder& b,
                       char default_conv = 'd') {
        const auto args = call.arguments();
        if (args.empty()) {
            fail("$sformat / $swrite requires a destination string argument");
        }
        if (is_sformat) {
            if (args.size() < 2) {
                fail("$sformat requires a format-string argument");
            }
            const Expression* fmt = args[1];
            while (fmt->kind == ExpressionKind::Conversion) {
                fmt = &fmt->as<ConversionExpression>().operand();
            }
            if (fmt->kind != ExpressionKind::StringLiteral) {
                // TODO: a runtime (non-literal) $sformat format string needs
                // dynamic string support, which Clotho lacks.
                fail("$sformat format must be a string literal");
            }
        }
        const ir::Index        dst = lower_lvalue(*args[0], b);
        std::string            fmt;
        std::vector<ir::Index> values;
        build_format(args.subspan(1), fmt, values, b, default_conv);
        b.sformat(dst, fmt, values, loc(call));
    }

    // Lower $sformatf (LRM §21.3.3): render like $sformat but yield a new string
    // value. The format (first argument) must be a string literal, like $sformat.
    ir::Index lower_sformatf(const CallExpression& call, ir::Builder& b) {
        const auto args = call.arguments();
        if (args.empty()) {
            fail("$sformatf requires a format-string argument");
        }
        const Expression* fa = args[0];
        while (fa->kind == ExpressionKind::Conversion) {
            fa = &fa->as<ConversionExpression>().operand();
        }
        if (fa->kind != ExpressionKind::StringLiteral) {
            // TODO: a runtime (non-literal) $sformatf format string needs
            // dynamic format parsing, which Clotho lacks.
            fail("$sformatf format must be a string literal");
        }
        std::string            fmt;
        std::vector<ir::Index> values;
        build_format(args, fmt, values, b);
        return b.sformatf(fmt, values, loc(call));
    }

    // Scale a %t argument from its module's time unit to design ticks: the runtime
    // converts ticks -> fs -> the $timeformat unit independent of the caller's
    // timescale (LRM §20.4.3). A value in module units times this instance's
    // time-unit multiplier yields ticks; the result is widened to 64 bits so the
    // runtime reads the full tick count.
    ir::Index scale_to_ticks(const Expression& arg, ir::Index v, ir::Builder& b) {
        const ir::SourceLoc l   = loc(arg);
        const ir::Ref       u64 = make_int_type(64, false, false);
        if (arg.type->isFloating()) {
            // A real (realtime) argument scales in the real domain, then rounds
            // to the nearest tick (FtoI rounds half away from zero); only
            // sub-tick fraction is lost, which the tick-based %t renderer
            // cannot represent anyway.
            ir::Index scaled = v;
            if (time_mul_ > 1) {
                const ir::Index mul =
                  b.constant(module_.pool.put_real(module_.pool.real_type, static_cast<double>(time_mul_)), l);
                scaled = b.binary(ir::Inst::Tag::FMul, v, mul, l);
            }
            return b.float_cvt(ir::Inst::Tag::FtoI, scaled, u64, l);
        }
        const ir::Index w = b.resize(v, u64, l);
        if (time_mul_ <= 1) {
            return w;
        }
        const ir::Index mul = b.constant(module_.pool.put_int(u64, time_mul_), l);
        return b.binary(ir::Inst::Tag::Mul, w, mul, l);
    }

    // Reject the one nonblocking-assignment target a task cannot stage: a frame
    // local or formal argument (both live in localmap). These have no module-signal
    // storage, and nba_target in the backend only walks to a Signal root, so there
    // is no shadow to defer the write into. A module signal, static local, or
    // unpacked-array element is fine: nba_classify walks task bodies, so a signal
    // gets shadow storage and an array element defers through the global runtime
    // queue exactly as it does from a process.
    void check_task_nba_target(const Expression& lhs) {
        switch (lhs.kind) {
            case ExpressionKind::Concatenation:
                for (const Expression* op : lhs.as<ConcatenationExpression>().operands()) {
                    check_task_nba_target(*op);
                }
                return;
            case ExpressionKind::Conversion:
                check_task_nba_target(lhs.as<ConversionExpression>().operand());
                return;
            case ExpressionKind::ElementSelect:
                check_task_nba_target(lhs.as<ElementSelectExpression>().value());
                return;
            case ExpressionKind::RangeSelect:
                check_task_nba_target(lhs.as<RangeSelectExpression>().value());
                return;
            case ExpressionKind::MemberAccess:
                check_task_nba_target(lhs.as<MemberAccessExpression>().value());
                return;
            case ExpressionKind::NamedValue: {
                const ValueSymbol& vs = lhs.as<NamedValueExpression>().symbol;
                if (fn_ctx_->localmap.contains(&vs)) {
                    // TODO: nonblocking assignment to a task local / formal argument.
                    fail("nonblocking assignment to a task-local variable is not supported");
                }
                return;
            }
            default:
                // TODO: an unrecognized nonblocking lvalue shape inside a task.
                fail("unsupported nonblocking-assignment target in a task");
        }
    }

    // Lower an assignment. Returns the raw stored value (the RHS as written,
    // before LHS-type truncation) so an assignment used as an rvalue (LRM 11.3.1,
    // `y = (x = 5)`) can recover its value; the caller truncates to the LHS type.
    // Statement callers discard the return.
    ir::Index lower_assign(const AssignmentExpression& a, ir::Builder& b) {
        if (a.left().type->isEvent()) {
            // `e2 = e1` (merge) / `e = null` (LRM 15.5.5): event variables are
            // handles; assignment aliases the underlying sync object. Tier 2 —
            // needs event reference semantics (docs/design/named-events.md).
            fail("event assignment is not supported");
        }
        if (a.isNonBlocking() && a.left().kind == ExpressionKind::MemberAccess &&
            a.left().as<MemberAccessExpression>().member.kind == SymbolKind::ClassProperty) {
            // TODO: nonblocking assignment to a class property. The aggregate NBA
            // staging path shadows a signal's storage, but an object body is a heap
            // block with no per-object shadow (docs/design/classes.md §8).
            fail("nonblocking assignment to a class property is not supported");
        }
        if (is_aggregate(*a.left().type)) {
            // Whole unpacked-struct assignment: decompose to per-member stores
            // (blocking or nonblocking). Each member commits independently, which
            // is whole-struct NBA semantics.
            const ir::SourceLoc l = loc(a);
            if (a.isCompound()) {
                fail("compound assignment to an unpacked struct is not supported");
            }
            if (a.timingControl != nullptr) {
                fail("intra-assignment timing control (#/@ on an assignment) is not supported");
            }
            const ir::Inst::Tag tag = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
            emit_struct_store(lower_lvalue(a.left(), b), lower_type(*a.left().type), a.right(), tag, b, l);
            return true_const(b, l);
        }
        if (a.isCompound()) {
            return lower_compound_assign(a, b);
        }
        if (a.timingControl != nullptr) {
            // Intra-assignment timing (LRM 9.4.5): sample the RHS now, suspend on
            // the delay/event, then store the sampled value. The blocking form is a
            // pure reorder over the existing Delay/Wait suspends — the sampled RHS
            // is an SSA value live across the suspend, which CoroSplit spills into
            // the process frame. The nonblocking form (`<= #d`) must not block the
            // process and instead schedules a detached future update; that needs
            // scheduler infrastructure not present yet.
            if (a.isNonBlocking()) {
                // Nonblocking form: sample now, do not block, commit after the
                // timing via a detached scheduled store. Only a whole integral scalar
                // target is lowered — the record is monomorphized per whole signal —
                // so selects / concats / aggregates (needing the masked/array record
                // path) fail loud, a documented follow-up
                // (docs/design/intra-assignment-timing.md).
                const bool whole_scalar =
                    (a.left().kind == ExpressionKind::NamedValue ||
                     a.left().kind == ExpressionKind::HierarchicalValue) &&
                    a.left().type->isIntegral() && !a.left().type->isUnpackedArray();
                if (!whole_scalar) {
                    fail("nonblocking intra-assignment timing is only supported for a whole "
                         "integral scalar target");
                }
                const TimingControl& tc = *a.timingControl;
                if (tc.kind == TimingControlKind::Delay) {
                    const Expression& delay_expr = tc.as<DelayControl>().expr;
                    // A compile-time `#0` nonblocking assignment is identical to a plain
                    // `<= ` — the update lands in the current instant's NBA region — so
                    // lower it as a normal NBA. This keeps update ordering to a signal
                    // uniform with its other NBAs (LRM 10.4.2, statement-execution order:
                    // last written wins) via the one block-commit path, instead of
                    // splitting the same signal across block-commit and the deferred
                    // record path (whose commit order is fixed, not execution order).
                    slang::ast::ASTContext     ectx(*scope_, slang::ast::LookupLocation::max);
                    slang::ast::EvalContext    ev(ectx);
                    const slang::ConstantValue cv = delay_expr.eval(ev);
                    if (cv.isInteger()) {
                        const auto v = cv.integer().as<uint64_t>();
                        if (v && *v == 0) {
                            const ir::Index val0 = lower_expr(a.right(), b);
                            b.binary(ir::Inst::Tag::StoreNb, lower_lvalue(a.left(), b), val0, loc(a));
                            return val0;
                        }
                    }
                    const ir::Index val   = lower_expr(a.right(), b);
                    const ir::Index delay = scaled_delay(delay_expr, b);
                    b.store_nb_delay(lower_lvalue(a.left(), b), val, delay, loc(a));
                    return val;
                }
                // Event / repeat-event form: `x <= @e y` / `x <= repeat(N) @e y`.
                // Sample the value (and N) now; store after N occurrences of the
                // trigger edge. Only a single (signal, edge) trigger is lowered.
                const TimingControl* evt   = &tc;
                ir::Index            count = ir::Index{};
                if (tc.kind == TimingControlKind::RepeatedEvent) {
                    const RepeatedEventControl& rec = tc.as<RepeatedEventControl>();
                    if (rec.expr.type->isFloating()) {
                        fail("repeat with a real count is not supported");
                    }
                    evt   = &rec.event;
                    count = lower_expr(rec.expr, b);
                } else {
                    // Plain `@e` waits for one occurrence.
                    count = b.constant(module_.pool.put_int(make_int_type(32, false, false), 1), loc(a));
                }
                if (!is_event_control(*evt)) {
                    fail("nonblocking intra-assignment requires a delay or event control");
                }
                std::vector<ir::WaitEvent> events;
                collect_wait_events(*evt, events);
                if (events.size() != 1) {
                    // TODO: a multi-signal trigger `@(a or b)` needs the record on
                    // several trigger lists with first-fire-wins removal.
                    fail("nonblocking intra-assignment multi-signal event trigger is not supported");
                }
                const ir::Index val = lower_expr(a.right(), b);
                b.store_nb_event(lower_lvalue(a.left(), b), val, count, events[0].signal,
                                 static_cast<uint32_t>(events[0].edge), loc(a));
                return val;
            }
            const bool string_lhs =
                is_string_expr(a.left()) ||
                (a.left().kind == ExpressionKind::ElementSelect &&
                 is_string_expr(a.left().as<ElementSelectExpression>().value()));
            if (string_lhs || is_unpacked_array(a.left())) {
                // TODO: intra-assignment timing on a string / whole-array target —
                // the string/array store paths evaluate the LHS before the RHS, so
                // reordering them across the suspend needs care.
                fail("intra-assignment timing on a string or array target is not supported");
            }
            const ir::Index rhs = lower_expr(a.right(), b);
            lower_intra_timing(*a.timingControl, b, loc(a));
            if (a.left().kind == ExpressionKind::Concatenation) {
                store_concat(a.left().as<ConcatenationExpression>(), rhs, ir::Inst::Tag::Store, b,
                             loc(a));
                return rhs;
            }
            b.binary(ir::Inst::Tag::Store, lower_lvalue(a.left(), b), rhs, loc(a));
            return rhs;
        }
        if (a.isNonBlocking() && fn_ctx_ != nullptr) {
            if (!fn_ctx_->is_task) {
                // A function consumes no simulation time, so a nonblocking
                // assignment in one has no deferred update region to commit into.
                // TODO: nonblocking assignment inside a function.
                fail("nonblocking assignment in a function is not supported");
            }
            // A task runs in its caller's time step; an NBA stages into the target
            // signal's shadow and commits in that instant's NBA region (the
            // backend's nba_classify scans task bodies). Only module-signal targets
            // have that shadow, so reject the rest.
            check_task_nba_target(a.left());
        }
        if (is_dyn_array(a.left())) {
            if (a.isNonBlocking()) {
                // TODO: nonblocking assignment to a dynamic array (needs a
                // handle-aware NBA staging path; the per-signal shadow is
                // plane-shaped).
                fail("nonblocking assignment to a dynamic array is not supported");
            }
            return lower_dyn_array_assign(a, b);
        }
        if (is_queue(a.left())) {
            if (a.isNonBlocking()) {
                // TODO: nonblocking assignment to a queue (needs a handle-aware
                // NBA staging path; the per-signal shadow is plane-shaped).
                fail("nonblocking assignment to a queue is not supported");
            }
            return lower_queue_assign(a, b);
        }
        if (is_unpacked_array(a.left())) {
            // A whole blocking copy of one trivially copyable unpacked array into
            // another is a single memcpy of the storage (same AggCopy path as an
            // aggregate). A nonblocking copy, a `'{...}` pattern, or a string array
            // (heap-owning elements) still decompose / are unsupported.
            const ir::Ref     ty  = lower_type(*a.left().type);
            const Expression& src = unwrap_conversion(a.right());
            if (!a.isNonBlocking() && is_trivially_copyable(ty) && is_whole_array_lvalue(src)) {
                b.agg_copy(lower_lvalue(a.left(), b), lower_lvalue(src, b), ty, loc(a));
                return true_const(b, loc(a));
            }
            if (as_assignment_pattern(src)) {
                // `mem = '{...}`: element-wise store of the (slang-ordered) pattern
                // into a scalar-element unpacked array (`is_aggregate` routes an
                // array of aggregates through emit_struct_store instead).
                if (a.isNonBlocking()) {
                    // TODO: nonblocking `'{...}` to an unpacked array — per-leaf NBA
                    // into array elements needs the deferred array-record path.
                    fail("nonblocking '{...} assignment to an unpacked array is not supported");
                }
                emit_agg_store(lower_lvalue(a.left(), b), ty, src, ir::Inst::Tag::Store, b, loc(a));
                return true_const(b, loc(a));
            }
            if (src.kind == ExpressionKind::StringLiteral) {
                // `mem = "literal"` (LRM 5.9): same element-wise store as the
                // '{...} pattern case above.
                if (a.isNonBlocking()) {
                    // TODO: nonblocking string-literal assignment to an unpacked
                    // array — per-leaf NBA into array elements needs the
                    // deferred array-record path.
                    fail("nonblocking string literal assignment to an unpacked array is not supported");
                }
                emit_agg_store(lower_lvalue(a.left(), b), ty, src, ir::Inst::Tag::Store, b, loc(a));
                return true_const(b, loc(a));
            }
            // TODO: nonblocking whole-array copy, string arrays — element-wise
            // copy / aggregate lowering.
            fail("whole unpacked array assignment is not supported");
        }
        // s[i] = c writes one character in place; out-of-range or a zero byte is
        // a no-op (LRM 6.16.2). The LHS element is byte-typed, so detect it via
        // the selected base.
        if (a.left().kind == ExpressionKind::ElementSelect &&
            is_string_expr(a.left().as<ElementSelectExpression>().value())) {
            if (a.isNonBlocking()) {
                // TODO: nonblocking assignment to strings (needs a string-aware
                // NBA staging path).
                fail("nonblocking assignment to a string is not supported");
            }
            const ElementSelectExpression& es  = a.left().as<ElementSelectExpression>();
            const ir::Index                str = lower_lvalue(es.value(), b);
            const ir::Index                idx = lower_expr(es.selector(), b);
            const ir::Index                chr = lower_expr(a.right(), b);
            b.str_putc(str, idx, chr, loc(a));
            return chr;
        }
        if (is_string_expr(a.left()) && a.isNonBlocking()) {
            // TODO: nonblocking assignment to strings (needs a string-aware NBA
            // staging path; the per-signal shadow is plane-shaped).
            fail("nonblocking assignment to a string is not supported");
        }
        if (a.isNonBlocking() && a.left().kind == ExpressionKind::ElementSelect &&
            is_queue(a.left().as<ElementSelectExpression>().value())) {
            // TODO: nonblocking assignment to a queue element (needs a
            // handle-aware NBA staging path; the per-signal shadow is
            // plane-shaped and cannot reach the heap ring buffer).
            fail("nonblocking assignment to a queue element is not supported");
        }
        // StoreNb gets correct two-phase NBA semantics in the backend: the RHS is
        // sampled here in the active region and committed in the NBA update region.
        // Intra-assignment timing (`<= #5`) is still rejected above, so the deferred
        // update always lands in the current instant's NBA region.
        const ir::Inst::Tag tag = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
        const ir::Index     rhs = lower_expr(a.right(), b);
        if (a.left().kind == ExpressionKind::Concatenation) {
            store_concat(a.left().as<ConcatenationExpression>(), rhs, tag, b, loc(a));
            return rhs;
        }
        b.binary(tag, lower_lvalue(a.left(), b), rhs, loc(a));
        return rhs;
    }

    // Whole dynamic-array assignment (LRM 7.5): `arr = new[n]` / `arr = new[n](src)`
    // (allocate + optional element copy) or `arr = other` (deep copy). Element
    // writes (`arr[i] = x`) are ordinary Stores over an ElemSelect and do not
    // reach here.
    ir::Index lower_dyn_array_assign(const AssignmentExpression& a, ir::Builder& b) {
        const ir::SourceLoc l   = loc(a);
        const ir::Index     dst = lower_lvalue(a.left(), b);
        const ir::Ref       aty = lower_type(*a.left().type);
        if (a.right().kind == ExpressionKind::NewArray) {
            const auto&     na   = a.right().as<slang::ast::NewArrayExpression>();
            const ir::Index size = lower_expr(na.sizeExpr(), b);
            if (const Expression* init = na.initExpr()) {
                b.dyn_array_new(dst, size, lower_lvalue(*init, b), /*has_init*/ true, aty, l);
            } else {
                b.dyn_array_new(dst, size, dst, /*has_init*/ false, aty, l);
            }
            return dst;
        }
        // arr = other: deep copy the source handle (value semantics).
        b.dyn_array_copy(dst, lower_lvalue(a.right(), b), aty, l);
        return dst;
    }

    // A dynamic-array built-in method used as an rvalue or statement (LRM 7.5).
    // The receiver arrives as arguments()[0], like a string method.
    ir::Index lower_dyn_array_method(std::string_view name, const CallExpression& call, ir::Builder& b,
                                     ir::SourceLoc l) {
        const ir::Index arr = lower_lvalue(*call.arguments()[0], b);
        const ir::Ref   aty = lower_type(*call.arguments()[0]->type);
        if (name == "size") {
            return b.resize(b.dyn_array_size(arr, aty, l), lower_type(*call.type), l);
        }
        if (name == "delete") {
            return b.dyn_array_delete(arr, aty, l);
        }
        if (name == "reverse") {
            return b.dyn_array_reverse(arr, aty, l);
        }
        if (name == "sort" || name == "rsort") {
            guard_ordering_sort(name, call);
            return b.dyn_array_sort(arr, name == "rsort", aty, l);
        }
        if (name == "shuffle") {
            guard_shuffle();
            return b.dyn_array_shuffle(arr, aty, l);
        }
        // TODO: sort/rsort `with` clause, 4-state / wide sort, and find/min/max/
        // unique/sum reduction & locator methods (need iterator-clause lowering).
        fail("dynamic array method '" + std::string(name) + "' is not supported");
    }

    // Loud guard for shuffle() (LRM 7.12.2): the per-process RNG lives in the
    // process coroutine frame (LRM 18.13), so it is only legal directly in a
    // process — a function/task body has no frame RNG (same restriction as
    // $urandom; threading the caller RNG through the call ABI is future work).
    void guard_shuffle() {
        if (fn_ctx_ != nullptr) {
            fail("shuffle inside a function/task is not supported yet (no per-process RNG "
                 "in the call frame); call it directly from a process");
        }
    }

    // Loud guard for the array ordering methods sort()/rsort() (LRM 7.12.2): the
    // V1 comparator handles a 2-state integral (width <= 64) or real element and
    // no `with` clause. 4-state ordering (x/z placement is LRM-unspecified), wide
    // elements, and a keyed `with` comparator are documented follow-ups.
    void guard_ordering_sort(std::string_view name, const CallExpression& call) {
        const auto& sci = std::get<CallExpression::SystemCallInfo>(call.subroutine);
        if (sci.getIteratorInfo().first != nullptr) {
            fail("'" + std::string(name) + "' with a `with` clause is not supported");
        }
        const slang::ast::Type* et = call.arguments()[0]->type->getArrayElementType();
        const slang::ast::Type& ec = et->getCanonicalType();
        const bool ok = ec.isFloating() ||
                        (ec.isIntegral() && !ec.isFourState() && ec.getBitWidth() <= 64);
        if (!ok) {
            fail("'" + std::string(name) +
                 "' is only supported for a 2-state integral (width <= 64) or real element");
        }
    }

    // Element address for a variable-size array (dynamic array or queue) index
    // over the handle lvalue `base`. Handles the queue `$` selector (q[$] is the
    // last element, index size-1; size-1 == -1 selects the OOB default on an
    // empty queue).
    ir::Index var_elem_select(const ElementSelectExpression& es, ir::Index base, ir::Builder& b,
                              ir::SourceLoc l) {
        const ir::Ref arr_ty = lower_type(*es.value().type);
        if (is_queue(es.value()) && es.selector().type->isUnbounded()) {
            const ir::Ref   i32  = make_int_type(32, false, true);
            const ir::Index size = b.queue_size(base, arr_ty, l);
            const ir::Index idx =
              b.binary(ir::Inst::Tag::Sub, size, b.constant(module_.pool.put_int(i32, 1), l), l);
            return b.elem_select(base, idx, arr_ty, l);
        }
        return b.elem_select(base, lower_expr(es.selector(), b), arr_ty, l);
    }

    // Whole-queue assignment (LRM 7.10): `q = other` deep-copies the source
    // handle. Element writes (`q[i] = x`) are ordinary Stores over an ElemSelect
    // and do not reach here. Non-queue sources (concatenation / assignment
    // pattern) are a documented follow-up.
    ir::Index lower_queue_assign(const AssignmentExpression& a, ir::Builder& b) {
        const ir::SourceLoc l   = loc(a);
        const ir::Index     dst = lower_lvalue(a.left(), b);
        const ir::Ref       qty = lower_type(*a.left().type);
        // A plain queue variable or a queue-returning call is a handle copy (both
        // have a handle lvalue). A queue-typed concatenation ({q, e}), assignment
        // pattern ('{...}), or slice is a value expression, not an lvalue, and
        // needs an element-wise build.
        const bool src_is_var = is_queue(a.right()) &&
                                (a.right().kind == ExpressionKind::NamedValue ||
                                 a.right().kind == ExpressionKind::HierarchicalValue ||
                                 a.right().kind == ExpressionKind::Call);
        if (!src_is_var) {
            // TODO: queue init from a concatenation ({q, e}), assignment pattern
            // ('{...}), slice, or unpacked-array source — needs an element-wise
            // build, not a handle copy (see docs/design/dynamic-arrays.md phase 4).
            fail("queue assignment from a non-variable value is not supported");
        }
        b.queue_copy(dst, lower_lvalue(a.right(), b), qty, l);
        return dst;
    }

    // A queue built-in method used as an rvalue or statement (LRM 7.10.2). The
    // receiver arrives as arguments()[0], like a string / dynamic-array method.
    ir::Index lower_queue_method(std::string_view name, const CallExpression& call, ir::Builder& b,
                                 ir::SourceLoc l) {
        const auto&     args = call.arguments();
        const ir::Index q    = lower_lvalue(*args[0], b);
        const ir::Ref   qty  = lower_type(*args[0]->type);
        if (name == "size") {
            return b.resize(b.queue_size(q, qty, l), lower_type(*call.type), l);
        }
        if (name == "push_back" || name == "push_front") {
            return b.queue_push(q, lower_expr(*args[1], b), name == "push_front", qty, l);
        }
        if (name == "pop_back" || name == "pop_front") {
            return b.queue_pop(q, name == "pop_front", qty, l);
        }
        if (name == "insert") {
            return b.queue_insert(q, lower_expr(*args[1], b), lower_expr(*args[2], b), qty, l);
        }
        if (name == "delete") {
            // q.delete() clears all; q.delete(i) removes one element.
            if (args.size() > 1) {
                return b.queue_delete(q, lower_expr(*args[1], b), /*has_idx*/ true, qty, l);
            }
            return b.queue_delete(q, q, /*has_idx*/ false, qty, l);
        }
        if (name == "reverse") {
            return b.queue_reverse(q, qty, l);
        }
        if (name == "sort" || name == "rsort") {
            guard_ordering_sort(name, call);
            return b.queue_sort(q, name == "rsort", qty, l);
        }
        if (name == "shuffle") {
            guard_shuffle();
            return b.queue_shuffle(q, qty, l);
        }
        // TODO: sort/rsort `with` clause, 4-state / wide sort, and find/min/max/
        // unique/sum reduction & locator methods (need iterator-clause lowering).
        fail("queue method '" + std::string(name) + "' is not supported");
    }

    // Compound assignment `lhs op= rhs` (LRM 11.4.1, always blocking). slang has
    // rewritten a.right() into `BinaryExpression(LValueReference, rhs, op)` with
    // width/sign/conversion already resolved, so lowering it after binding the
    // LHS lvalue yields `op(load(lhs), rhs)`. The LHS lvalue is evaluated exactly
    // once and reused for the embedded LValueReference (single-evaluation, so
    // `mem[i++] += 1` bumps i once).
    ir::Index lower_compound_assign(const AssignmentExpression& a, ir::Builder& b) {
        const ir::SourceLoc l = loc(a);
        if (a.left().kind == ExpressionKind::Concatenation) {
            // TODO: compound assignment to a concatenation lvalue ({a,b} += x).
            // Needs the LValueReference to read the concat and store_concat to
            // scatter the result.
            fail("compound assignment to a concatenation is not supported");
        }
        if (is_string_expr(a.left()) ||
            (a.left().kind == ExpressionKind::ElementSelect &&
             is_string_expr(a.left().as<ElementSelectExpression>().value()))) {
            // TODO: compound assignment to a string / string element (the lvalue
            // path below is packed-only; a string char write uses str_putc).
            fail("compound assignment to a string is not supported");
        }
        const ir::Index                lv    = lower_lvalue(a.left(), b);
        const std::optional<ir::Index> saved = compound_lv_;
        compound_lv_                         = lv;
        const ir::Index rhs                  = lower_expr(a.right(), b);
        compound_lv_                         = saved;
        b.binary(ir::Inst::Tag::Store, lv, rhs, l);
        return rhs;
    }

    // Distribute a value across a concatenation lvalue, `{a, b, c} = rhs` (LRM
    // §11.4.12). The leftmost operand takes the most-significant slice; operand
    // widths partition the RHS bits. Lowered to one store per operand: the slice
    // is `rhs >> offset` truncated to the operand width via resize (the upper bits
    // are discarded by the narrowing store). The RHS is sampled once by the caller,
    // so a nonblocking concat assignment keeps single-evaluation semantics.
    void store_concat(const ConcatenationExpression& cc, ir::Index rhs, ir::Inst::Tag tag,
                      ir::Builder& b, ir::SourceLoc l) {
        const std::span<const Expression* const> ops = cc.operands();
        uint32_t                                  offset = 0;
        for (size_t k = ops.size(); k-- > 0;) {
            const Expression& op = *ops[k];
            if (is_string_expr(op)) {
                fail("string operand in an lvalue concatenation is not supported");
            }
            ir::Index chunk = rhs;
            if (offset != 0) {
                const ir::Index sh =
                    b.constant(module_.pool.put_int(make_int_type(32, false, false), offset), l);
                chunk = b.binary(ir::Inst::Tag::Srl, rhs, sh, l);
            }
            const ir::Index val = b.resize(chunk, lower_type(*op.type), l);
            if (op.kind == ExpressionKind::Concatenation) {
                store_concat(op.as<ConcatenationExpression>(), val, tag, b, l);
            } else {
                b.binary(tag, lower_lvalue(op, b), val, l);
            }
            offset += op.type->getBitWidth();
        }
    }

    // -- Expressions --

    // Lower a fixed-width RHS streaming concatenation (LRM §11.4.14).
    // `>>` (sliceSize==0): plain left-to-right concat, no reorder.
    // `<<` (sliceSize>0): concatenate, then reverse the order of sliceSize-bit
    // blocks (rightmost block becomes the MSB of the result).
    ir::Index lower_streaming_concat(const StreamingConcatenationExpression& sc,
                                     ir::Ref target_ty, ir::Builder& b, ir::SourceLoc l) {
        if (!sc.isFixedSize()) {
            fail("streaming concatenation with dynamically-sized operands is not supported");
        }
        for (const auto& stream : sc.streams()) {
            if (stream.withExpr) {
                fail("streaming concatenation `with` clause is not supported");
            }
        }

        // Build the generic stream: concatenate operands left-to-right.
        // A nested streaming concat has void type (slang creates it with getVoidType()),
        // so detect it and supply the concrete target type derived from bitstream width.
        const bool is_four_outer = module_.pool.layout(target_ty).domain == ir::Domain::Four;
        std::vector<ir::Index> parts;
        for (const auto& stream : sc.streams()) {
            const Expression& operand = *stream.operand;
            if (operand.kind == ExpressionKind::Streaming) {
                const auto& inner = operand.as<StreamingConcatenationExpression>();
                const ir::Ref inner_ty = make_int_type(
                    static_cast<uint32_t>(inner.getBitstreamWidth()), is_four_outer, false);
                parts.push_back(lower_streaming_concat(inner, inner_ty, b, l));
            } else {
                parts.push_back(lower_expr(operand, b));
            }
        }
        ir::Index raw = parts.size() == 1 ? parts[0] : b.concat(parts, l);

        const uint64_t W = sc.getBitstreamWidth();
        const uint64_t S = sc.getSliceSize();

        // sliceSize > 0: '<<' — reverse the order of the stream's S-bit blocks
        // (block i covers bits [min((i+1)*S,W)-1 : i*S]; block 0, the
        // rightmost, lands at the MSB). One StreamReverse node; the backend
        // owns the per-plane block permutation.
        if (S > 0) {
            raw = b.stream_reverse(raw, static_cast<uint32_t>(S), l);
        }

        // Widen if the target is wider than the stream (LRM §11.4.14: zero-fill
        // on the right, i.e. stream occupies the MSBs of the result).
        const uint32_t target_w = module_.pool.layout(target_ty).width;
        if (target_w > static_cast<uint32_t>(W)) {
            const uint32_t pad       = target_w - static_cast<uint32_t>(W);
            const bool     is_four   = module_.pool.layout(target_ty).domain == ir::Domain::Four;
            const ir::Ref  pad_ty    = make_int_type(pad, is_four, false);
            const ir::Index zero_pad = b.constant(module_.pool.put_int(pad_ty, 0), l);
            std::vector<ir::Index> widened = {raw, zero_pad};
            raw = b.concat(widened, l);
        }

        return b.resize(raw, target_ty, l);
    }

    // Lower an expression as an rvalue, returning the SSA value index.
    ir::Index lower_expr(const Expression& expr, ir::Builder& b) {
        const ir::SourceLoc l = loc(expr);
        switch (expr.kind) {
            case ExpressionKind::IntegerLiteral:
                return b.constant(lower_int_literal(expr.as<IntegerLiteral>()), l);
            case ExpressionKind::NullLiteral:
                // `null` for a chandle (LRM 6.14) is the empty handle, value 0.
                // slang types the literal as NullType (not chandle), so lower a
                // 64-bit 0 in the chandle representation directly; a surrounding
                // conversion to chandle (also i64) is a width-preserving no-op.
                // (Class / event null handles are separate and not lowered here.)
                return b.constant(module_.pool.put_int(make_int_type(64, false, false), 0), l);
            case ExpressionKind::UnbasedUnsizedIntegerLiteral:
                // '0/'1/'x/'z (LRM 5.7.1): the single bit fills the whole
                // context-determined width. slang has already propagated the
                // type and getValue() returns the SVInt sized+filled per the
                // table (setAllOnes / createFillX / createFillZ).
                return b.constant(
                    svint_to_ref(expr.as<UnbasedUnsizedIntegerLiteral>().getValue(), lower_type(*expr.type)), l);
            case ExpressionKind::RealLiteral:
                // An IEEE-754 double (LRM 5.7.2); a shortreal literal arrives as
                // a cast around this, handled by the Conversion case.
                return b.constant(module_.pool.put_real(lower_type(*expr.type), expr.as<RealLiteral>().getValue()),
                                  l);
            case ExpressionKind::TimeLiteral:
                // A realtime value slang has already scaled to the scope's time
                // unit (LRM 5.8); precision rounding applies only in delay
                // contexts, which reject reals for now.
                return b.constant(module_.pool.put_real(lower_type(*expr.type), expr.as<TimeLiteral>().getValue()),
                                  l);
            case ExpressionKind::StringLiteral: {
                // In a string-typed context the literal is a string constant with
                // its \0 bytes dropped (LRM 6.16).
                if (is_string_expr(expr)) {
                    return b.constant(string_value_ref(expr.as<StringLiteral>().getValue()), l);
                }
                // Otherwise a string literal is a packed array of 8-bit ASCII bytes
                // (LRM 5.9); as an integral rvalue it is that packed value. slang
                // folds it to an SVInt of the literal's 8*N-bit type; the
                // surrounding conversion resizes it to the assignment context.
                slang::ast::ASTContext  actx(*scope_, slang::ast::LookupLocation::max);
                slang::ast::EvalContext ectx(actx);
                const slang::ConstantValue cv = expr.eval(ectx);
                if (!cv.isInteger()) {
                    fail("non-integral string literal");
                }
                return b.constant(svint_to_ref(cv.integer(), lower_type(*expr.type)), l);
            }
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                // A hierarchical reference (`top.u1.sig`, LRM 23.6) resolves to the
                // same ValueSymbol a local name would; slang's whole-design
                // elaboration binds it to the target instance's symbol, so once all
                // signals are collected (the two-pass lowering below) it flows
                // through the ordinary signal_of path.
                const ValueSymbol& sym = expr.as<ValueExpressionBase>().symbol;
                if (expr.type->isEvent()) {
                    // A named event has no data value; the legal forms (`@(e)`,
                    // `-> e`, `e.triggered`) resolve the event symbol directly and
                    // never reach this value path. This catches the Tier-2 handle
                    // forms — `e2 = e1` merge, `e == null` — which need event
                    // reference semantics (docs/design/named-events.md).
                    fail("event used as a value is not supported (event assignment / "
                         "comparison / null)");
                }
                if (sym.kind == SymbolKind::Parameter) {
                    const ParameterSymbol& p = sym.as<ParameterSymbol>();
                    return b.constant(lower_constant(p.getValue(), p.getType()), l);
                }
                if (sym.kind == SymbolKind::EnumValue) {
                    // An enum member (LRM 6.19) is a named compile-time constant of
                    // the enum's base type; slang computes its value (auto-increment
                    // or explicit). The enum type itself lowers as its base integer
                    // (lower_type), so the member is just that base-typed constant.
                    const EnumValueSymbol& ev = sym.as<EnumValueSymbol>();
                    return b.constant(lower_constant(ev.getValue(), ev.getType()), l);
                }
                if (is_unpacked_array(expr)) {
                    // TODO: reading a whole unpacked array as a value (array copy,
                    // passing a memory by value). Only element reads are supported.
                    fail("whole unpacked array read is not supported");
                }
                if (is_aggregate(*expr.type)) {
                    // A whole unpacked struct has no flat value: reads are only
                    // supported where they decompose field-wise (assignment,
                    // equality). A general rvalue context ($display, a by-value
                    // argument, a concat operand) would need a by-value struct ABI.
                    fail("unpacked struct as a whole-value rvalue is not supported here");
                }
                return b.unary(ir::Inst::Tag::Load, value_lvalue(sym, b, l), l);
            }
            case ExpressionKind::LValueReference: {
                // slang rewrites a compound assignment's RHS into
                // `BinaryExpression(LValueReference, rhs, op)`; the reference
                // stands for the LHS already lowered once by lower_compound_assign
                // (LRM 11.4.1 single-evaluation).
                if (!compound_lv_.has_value()) {
                    fail("LValueReference outside a compound assignment");
                }
                const ir::Index v = b.unary(ir::Inst::Tag::Load, *compound_lv_, l);
                // A no-op for every lvalue shape except a struct member
                // narrower than its struct's own physical domain (see the
                // MemberAccess case above) — cheap enough to apply generically
                // rather than special-case the one shape that needs it.
                return narrow_to_declared(v, *expr.type, b, l);
            }
            case ExpressionKind::UnaryOp:
                return lower_unary(expr.as<UnaryExpression>(), b);
            case ExpressionKind::BinaryOp:
                return lower_binary(expr.as<BinaryExpression>(), b);
            case ExpressionKind::Inside:
                return lower_inside(expr.as<InsideExpression>(), b);
            case ExpressionKind::ConditionalOp: {
                const ConditionalExpression& c = expr.as<ConditionalExpression>();
                if (is_string_expr(expr)) {
                    // TODO: ?: over strings (needs a string-aware select; lower to a
                    // Cond with a string temp).
                    fail("conditional operator (?:) over strings is not supported");
                }
                if (c.conditions.size() != 1) {
                    fail("unsupported multi-condition ?:");
                }
                if (c.conditions[0].pattern != nullptr) {
                    // TODO: `expr matches pattern ? a : b` (LRM 12.6.2) — same
                    // gap as lower_conditional's pattern guard.
                    fail("pattern matching in conditions is not supported");
                }
                // An arm whose evaluation is observable must not run unless
                // chosen (LRM 11.4.11: a definite condition evaluates only its
                // arm; an ambiguous one evaluates both and blends). Pure arms
                // keep the branchless packed select — evaluating the unchosen
                // arm is then unobservable.
                if (has_observable_effect(c.left()) || has_observable_effect(c.right())) {
                    return lower_lazy_conditional(c, b);
                }
                const ir::Index pred = lower_predicate(*c.conditions[0].expr, b);
                const ir::Index a    = lower_expr(c.left(), b);
                const ir::Index bb   = lower_expr(c.right(), b);
                return b.select(pred, a, bb, l);
            }
            case ExpressionKind::Concatenation: {
                std::vector<ir::Index> parts;
                for (const Expression* op : expr.as<ConcatenationExpression>().operands()) {
                    parts.push_back(lower_expr(*op, b));
                }
                return b.concat(parts, l);
            }
            case ExpressionKind::Replication: {
                const ReplicationExpression& r     = expr.as<ReplicationExpression>();
                const uint64_t               count = const_uint(r.count());
                return b.repl(lower_expr(r.concat(), b), static_cast<uint32_t>(count), l);
            }
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es = expr.as<ElementSelectExpression>();
                if (is_string_expr(es.value())) {
                    // s[i] reads the byte at i, 0 when out of range — identical to
                    // getc (LRM 6.16.3).
                    const ir::Index str = b.unary(ir::Inst::Tag::Load, lower_lvalue(es.value(), b), l);
                    return b.binary(ir::Inst::Tag::StrGetc, str, lower_expr(es.selector(), b), l);
                }
                if (auto c = try_lower_constant_select(expr, b, l)) {
                    return *c;
                }
                const ir::Index base = select_base_lvalue(es.value(), b, l);
                const ir::Index sel =
                    is_unpacked_array(es.value()) || is_dyn_array(es.value()) || is_queue(es.value())
                        ? var_elem_select(es, base, b, l)
                        : lower_packed_element_lvalue(es, base, b, l);
                const ir::Index v = b.unary(ir::Inst::Tag::Load, sel, l);
                // See narrow_to_declared / the MemberAccess case below: a
                // no-op except when `es` selects into a struct field
                // narrower-domain than its struct's own physical storage
                // (e.g. `x.b[3]` where `b` is 2-state inside an otherwise
                // 4-state struct `x`).
                return narrow_to_declared(v, *expr.type, b, l);
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs   = expr.as<RangeSelectExpression>();
                if (auto c = try_lower_constant_select(expr, b, l)) {
                    return *c;
                }
                const ir::Index              base = select_base_lvalue(rs.value(), b, l);
                const ir::Index v = b.unary(ir::Inst::Tag::Load, lower_range_select(rs, base, b, l), l);
                // See narrow_to_declared (e.g. `x.b[7:4]`).
                return narrow_to_declared(v, *expr.type, b, l);
            }
            case ExpressionKind::MemberAccess: {
                // The backend's PartSelect/BitSelect type inference
                // (LlvmBackend.cpp compute_type) inherits the BASE signal's
                // domain, since an ordinary vector/array is domain-uniform
                // throughout. A struct is the one aggregate where a field's
                // own declared domain can be narrower than the whole (mixed)
                // struct's forced-4-state storage (LRM 6.11.2) — so a 2-state
                // field (or a further bit/range-select into one, above) read
                // off 4-state storage must be explicitly narrowed back to its
                // declared type here (LRM 6.3.1 conversion), or downstream
                // consumers that trust the declared type (e.g. an arithmetic
                // operand) see a width/domain mismatch. A no-op when the
                // field's own domain already matches the struct's (a packed
                // struct member is always integral, LRM 7.2.1, so
                // narrow_to_declared's string/real skip never triggers here —
                // used anyway for uniformity with its ElementSelect/RangeSelect
                // siblings above).
                if (auto c = try_lower_constant_select(expr, b, l)) {
                    return *c;
                }
                const MemberAccessExpression& ma = expr.as<MemberAccessExpression>();
                const ir::Index v = b.unary(ir::Inst::Tag::Load, lower_member_lvalue(ma, b, l), l);
                return narrow_to_declared(v, *ma.type, b, l);
            }
            case ExpressionKind::Conversion: {
                const ConversionExpression& cv       = expr.as<ConversionExpression>();
                const bool                  to_str   = is_string_expr(expr);
                const bool                  from_str = is_string_expr(cv.operand());
                if (to_str && from_str) {
                    return lower_expr(cv.operand(), b); // identity
                }
                if (to_str) {
                    // A literal folds at compile time (its \0 bytes dropped);
                    // anything else is the runtime integral -> string cast
                    // (bytes MSB-first, zero bytes removed, LRM 6.16).
                    if (cv.operand().kind == ExpressionKind::StringLiteral) {
                        return b.constant(
                          string_value_ref(cv.operand().as<StringLiteral>().getValue()), l);
                    }
                    if (!cv.operand().type->isIntegral()) {
                        fail("unsupported conversion to string");
                    }
                    return b.unary(ir::Inst::Tag::StrFromPacked, lower_expr(cv.operand(), b), l);
                }
                if (from_str) {
                    // string -> packed integral: characters pack MSB-first,
                    // right-justified to the target width (LRM 6.16).
                    if (!expr.type->isIntegral()) {
                        fail("unsupported conversion from string");
                    }
                    return b.str_to_packed(lower_expr(cv.operand(), b), lower_type(*expr.type), l);
                }
                if (cv.conversionKind == ConversionKind::StreamingConcat) {
                    return lower_streaming_concat(
                        cv.operand().as<StreamingConcatenationExpression>(),
                        lower_type(*expr.type), b, l);
                }
                if (is_handle_expr(cv.operand()) && !expr.type->getCanonicalType().isUnpackedArray()) {
                    // TODO: bit-stream cast from a dynamic array / queue to a
                    // packed type (LRM 6.24.2) -- needs an element-wise pack of
                    // the runtime handle's heap contents, not a plain integer
                    // coerce/resize (which silently produced the wrong value
                    // for a variable operand, and crashed for a call operand's
                    // temporary return handle).
                    fail("bit-stream cast from a dynamic array or queue to a packed type is not supported");
                }
                return coerce(lower_expr(cv.operand(), b), *cv.operand().type, lower_type(*expr.type), b, l);
            }
            case ExpressionKind::Call: {
                const CallExpression& call = expr.as<CallExpression>();
                if (call.isSystemCall()) {
                    return lower_sysfunc(call, b);
                }
                const std::optional<ir::Index> v = lower_user_call(call, b);
                if (!v) {
                    fail("void function call used as an expression");
                }
                return *v;
            }
            case ExpressionKind::NewClass:
                return lower_new_class(expr.as<slang::ast::NewClassExpression>(), b);
            case ExpressionKind::Assignment: {
                // Assignment used as an rvalue (`y = (x += 1)`, `y = (x = 5)`). Its
                // value is the LHS after the store, truncated to the LHS type (LRM
                // 11.3.1) — not the raw RHS. A string target yields the assigned
                // handle unchanged.
                const AssignmentExpression& a = expr.as<AssignmentExpression>();
                const ir::Index             v = lower_assign(a, b);
                return is_string_expr(a.left()) ? v : b.resize(v, lower_type(*a.left().type), l);
            }
            case ExpressionKind::SimpleAssignmentPattern:
            case ExpressionKind::StructuredAssignmentPattern:
            case ExpressionKind::ReplicatedAssignmentPattern:
                return lower_packed_pattern(expr, b);
            default:
                fail("unsupported expression kind=" + std::string(slang::ast::toString(expr.kind)));
        }
    }

    ir::Index lower_unary(const UnaryExpression& u, ir::Builder& b) {
        const ir::SourceLoc l = loc(u);
        using U               = slang::ast::UnaryOperator;
        using T               = ir::Inst::Tag;
        // Increment/decrement (LRM 11.4.2): `i = i +/- 1`, LHS evaluated once.
        // Handled before lowering the operand as an rvalue so the lvalue's
        // selectors are not evaluated twice. Pre returns the new value, post the
        // old; slang has already resolved the operand's width/type.
        if (u.op == U::Preincrement || u.op == U::Postincrement ||
            u.op == U::Predecrement || u.op == U::Postdecrement) {
            const bool inc = u.op == U::Preincrement || u.op == U::Postincrement;
            const bool pre = u.op == U::Preincrement || u.op == U::Predecrement;
            const ir::Index lv  = lower_lvalue(u.operand(), b);
            const ir::Index cur = b.unary(T::Load, lv, l);
            if (u.operand().type->isFloating()) {
                // ++ / -- on a real add / subtract 1.0 (LRM 11.4.2).
                const ir::Index one =
                  b.constant(module_.pool.put_real(lower_type(*u.operand().type), 1.0), l);
                const ir::Index nv = b.binary(inc ? T::FAdd : T::FSub, cur, one, l);
                b.binary(T::Store, lv, nv, l);
                return pre ? nv : cur;
            }
            const ir::Index one = b.constant(module_.pool.put_int(lower_type(*u.operand().type), 1), l);
            const ir::Index nv  = b.binary(inc ? T::Add : T::Sub, cur, one, l);
            b.binary(T::Store, lv, nv, l);
            return pre ? nv : cur;
        }
        const ir::Index op = lower_expr(u.operand(), b);
        if (u.operand().type->isFloating()) {
            switch (u.op) {
                case U::Plus:
                    return op;
                case U::Minus:
                    return b.unary(T::FNeg, op, l);
                case U::LogicalNot:
                    // !r is r == 0.0: a definite 1-bit result (LRM 11.4.7 with
                    // real operands has no ambiguity — reals carry no x/z).
                    return b.binary(T::FEq, op, real_zero(u.operand(), b, l), l);
                default:
                    // Bitwise / reduction operators are illegal on reals (LRM
                    // Table 11-1); slang rejects them upstream.
                    fail("unsupported unary operator on a real operand");
            }
        }
        switch (u.op) {
            case U::BitwiseNot:
                return b.unary(T::Bnot, op, l);
            case U::LogicalNot:
                if (is_string_expr(u.operand())) {
                    fail("logical negation of a string is not legal (LRM Table 11-1)");
                }
                return b.unary(T::Lnot, op, l);
            case U::BitwiseAnd:
                return b.unary(T::RedAnd, op, l);
            case U::BitwiseOr:
                return b.unary(T::RedOr, op, l);
            case U::BitwiseXor:
                return b.unary(T::RedXor, op, l);
            case U::BitwiseNand:
                return b.unary(T::Bnot, b.unary(T::RedAnd, op, l), l);
            case U::BitwiseNor:
                return b.unary(T::Bnot, b.unary(T::RedOr, op, l), l);
            case U::BitwiseXnor:
                return b.unary(T::Bnot, b.unary(T::RedXor, op, l), l);
            case U::Plus:
                return op;
            case U::Minus: {
                const ir::Index zero = b.constant(module_.pool.put_int(lower_type(*u.type), 0), l);
                return b.binary(T::Sub, zero, op, l);
            }
            default:
                fail("unsupported unary operator");
        }
    }

    // Short-circuit `&&` / `||` (LRM 11.3.5, 11.4.7). The left operand is always
    // evaluated; the right operand is evaluated only in the Select's then-branch,
    // taken when the left has not already fixed the result. A definite left
    // operand short-circuits (`&&` on a false left -> 0, `||` on a true left ->
    // 1); an x/z left is ambiguous, so the right operand is still evaluated and
    // the full 3-valued Land/Lor computes the merged result.
    ir::Index lower_short_circuit(const BinaryExpression& bin, ir::Builder& b) {
        using T                = ir::Inst::Tag;
        const ir::SourceLoc l  = loc(bin);
        const bool          is_and = bin.op == slang::ast::BinaryOperator::LogicalAnd;
        const ir::Ref       bit  = make_int_type(1, true, false);

        // A real operand reduces to its own definite truth bit (!= 0.0, reals
        // carry no x/z) before the integer machinery below (LRM 11.4.7).
        ir::Index lhs = lower_expr(bin.left(), b);
        if (bin.left().type->isFloating()) {
            lhs = b.binary(T::FNe, lhs, real_zero(bin.left(), b, l), l);
        }

        // Predicate is a definite 2-state bit: 1 iff the right operand must still
        // be evaluated. `&&` skips when the left is logically false (all bits a
        // definite 0); `||` skips when the left is logically true (its reduction
        // or is a definite 1). Case-equality (===) yields a definite bit even for
        // an x/z left, so the backend can branch on it directly.
        const ir::Ref lhs_ty = bin.left().type->isFloating() ? bit : lower_type(*bin.left().type);
        ir::Index     pred;
        if (is_and) {
            const ir::Index zero = b.constant(module_.pool.put_int(lhs_ty, 0), l);
            pred                 = b.unary(T::Lnot, b.binary(T::CaseEq, lhs, zero, l), l);
        } else {
            const ir::Index ror = b.unary(T::RedOr, lhs, l);
            const ir::Index one = b.constant(module_.pool.put_int(bit, 1), l);
            pred                = b.unary(T::Lnot, b.binary(T::CaseEq, ror, one, l), l);
        }

        b.open_region();
        ir::Index rhs = lower_expr(bin.right(), b);
        if (bin.right().type->isFloating()) {
            rhs = b.binary(T::FNe, rhs, real_zero(bin.right(), b, l), l);
        }
        const ir::Index      combined    = b.binary(is_and ? T::Land : T::Lor, lhs, rhs, l);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        const ir::Index     dom         = b.constant(module_.pool.put_int(bit, is_and ? 0 : 1), l);
        const ir::ExtraSlice else_region = b.close_region();

        return b.select_lazy(pred, then_region, combined, else_region, dom, l);
    }

    // Lazy `?:` for an observable arm (LRM 11.4.11): a definite-true condition
    // evaluates only the first expression, definite-false only the second, and
    // an ambiguous (x/z) condition evaluates BOTH and blends them bit by bit
    // (Table 11-20 — exactly what an eager Select with an unknown condition
    // computes). Built as two nested SelectLazy: the outer branches on
    // definite-true, its else branches on definite-false, and the innermost
    // else region re-lowers both arms eagerly. Case-equality on the 1-bit
    // predicate yields the definite branch bits.
    ir::Index lower_lazy_conditional(const ConditionalExpression& c, ir::Builder& b) {
        using T               = ir::Inst::Tag;
        const ir::SourceLoc l = loc(c);
        const ir::Ref       bit = make_int_type(1, true, false);

        const ir::Index pred    = lower_predicate(*c.conditions[0].expr, b);
        const ir::Index one     = b.constant(module_.pool.put_int(bit, 1), l);
        const ir::Index zero    = b.constant(module_.pool.put_int(bit, 0), l);
        const ir::Index is_true = b.binary(T::CaseEq, pred, one, l);

        b.open_region();
        const ir::Index      tv       = lower_expr(c.left(), b);
        const ir::ExtraSlice t_region = b.close_region();

        b.open_region();
        const ir::Index is_false = b.binary(T::CaseEq, pred, zero, l);

        b.open_region();
        const ir::Index      fv       = lower_expr(c.right(), b);
        const ir::ExtraSlice f_region = b.close_region();

        b.open_region();
        const ir::Index      xa       = lower_expr(c.left(), b);
        const ir::Index      xb       = lower_expr(c.right(), b);
        const ir::Index      blend    = b.select(pred, xa, xb, l);
        const ir::ExtraSlice x_region = b.close_region();

        const ir::Index      inner       = b.select_lazy(is_false, f_region, fv, x_region, blend, l);
        const ir::ExtraSlice else_region = b.close_region();

        return b.select_lazy(is_true, t_region, tv, else_region, inner, l);
    }

    // A 0.0 constant of the operand's float type, for predicates.
    ir::Index real_zero(const Expression& operand, ir::Builder& b, ir::SourceLoc l) {
        return b.constant(module_.pool.put_real(lower_type(*operand.type), 0.0), l);
    }

    // Binary operator with real operands. slang has inserted all LRM 11.3.1 /
    // 11.8.2 conversions, so both operands share one float type here (except a
    // logical operand, which converts to its own truth value first).
    ir::Index lower_real_binary(const BinaryExpression& bin, ir::Index lhs, ir::Index rhs, ir::Builder& b) {
        const ir::SourceLoc l = loc(bin);
        using B               = slang::ast::BinaryOperator;
        using T               = ir::Inst::Tag;
        switch (bin.op) {
            case B::Add:
                return b.binary(T::FAdd, lhs, rhs, l);
            case B::Subtract:
                return b.binary(T::FSub, lhs, rhs, l);
            case B::Multiply:
                return b.binary(T::FMul, lhs, rhs, l);
            case B::Divide:
                return b.binary(T::FDiv, lhs, rhs, l);
            case B::Power:
                return b.binary(T::FPow, lhs, rhs, l);
            case B::Equality:
                return b.binary(T::FEq, lhs, rhs, l);
            case B::Inequality:
                return b.binary(T::FNe, lhs, rhs, l);
            case B::GreaterThan:
                return b.binary(T::FGt, lhs, rhs, l);
            case B::GreaterThanEqual:
                return b.binary(T::FGe, lhs, rhs, l);
            case B::LessThan:
                return b.binary(T::FLt, lhs, rhs, l);
            case B::LessThanEqual:
                return b.binary(T::FLe, lhs, rhs, l);
            case B::LogicalAnd:
            case B::LogicalOr: {
                // Each real operand reduces to its own truth value (!= 0.0,
                // definite); an integral operand keeps its planes and the
                // backend's Land/Lor X rules apply to it alone.
                const ir::Index lp = bin.left().type->isFloating()
                                       ? b.binary(T::FNe, lhs, real_zero(bin.left(), b, l), l)
                                       : lhs;
                const ir::Index rp = bin.right().type->isFloating()
                                       ? b.binary(T::FNe, rhs, real_zero(bin.right(), b, l), l)
                                       : rhs;
                return b.binary(bin.op == B::LogicalAnd ? T::Land : T::Lor, lp, rp, l);
            }
            default:
                // %, bitwise, shifts, ===, ==? are illegal on reals (LRM Table
                // 11-1); slang rejects them upstream.
                fail("unsupported binary operator on real operands");
        }
    }

    ir::Index lower_binary(const BinaryExpression& bin, ir::Builder& b) {
        using B = slang::ast::BinaryOperator;
        using T = ir::Inst::Tag;

        // Short-circuit logical operators: the right operand must not be
        // evaluated when the left already decides the result, so its side
        // effects (a function call) do not occur (LRM 11.3.5, 11.4.7). Handled
        // before the eager operand lowering below, which always evaluates both.
        if (bin.op == B::LogicalAnd || bin.op == B::LogicalOr) {
            return lower_short_circuit(bin, b);
        }

        // Unpacked-struct equality (LRM 7.2, 11.4.5): a whole-struct value cannot
        // be loaded, so `==` / `!=` reduce to a per-member comparison. The members'
        // 4-state Eq results fold with Land (X propagates: any definite mismatch
        // is 0, else X if any member is X, else 1); `!=` negates that reduction.
        // slang wraps each operand in a bitstream Conversion, so the struct type
        // is on the unwrapped operand, not `bin.left().type`.
        if ((bin.op == B::Equality || bin.op == B::Inequality) &&
            is_unpacked_struct(*unwrap_conversion(bin.left()).type)) {
            const ir::SourceLoc l   = loc(bin);
            const ir::Index     acc = lower_struct_eq(unwrap_conversion(bin.left()),
                                                      unwrap_conversion(bin.right()), b, l);
            return bin.op == B::Equality ? acc : b.unary(ir::Inst::Tag::Lnot, acc, l);
        }

        if (bin.left().kind == ExpressionKind::TypeReference ||
            bin.right().kind == ExpressionKind::TypeReference) {
            // type(a) == type(b) (LRM 6.23): a type reference is a compile-time
            // marker, not a value -- slang restricts this expression kind to
            // exactly this position (an equality/case-equality comparison with
            // another type reference) and to a case-statement selector/item
            // (handled separately in lower_case), so both operands are
            // guaranteed TypeReference here; mirrors slang's own constant fold
            // in BinaryExpression::evalImpl.
            if (bin.left().kind != ExpressionKind::TypeReference ||
                bin.right().kind != ExpressionKind::TypeReference ||
                (bin.op != B::Equality && bin.op != B::Inequality && bin.op != B::CaseEquality &&
                 bin.op != B::CaseInequality)) {
                fail("type() is only supported in an equality comparison with another type()");
            }
            const bool matches = bin.left().as<TypeReferenceExpression>().targetType.isMatching(
                bin.right().as<TypeReferenceExpression>().targetType);
            const bool result  = (bin.op == B::Equality || bin.op == B::CaseEquality) ? matches : !matches;
            return b.constant(module_.pool.put_int(make_int_type(1, false, false), result ? 1 : 0), loc(bin));
        }

        const ir::SourceLoc l   = loc(bin);
        const ir::Index     lhs = lower_expr(bin.left(), b);
        const ir::Index     rhs = lower_expr(bin.right(), b);

        if (is_string_expr(bin.left()) || is_string_expr(bin.right())) {
            // slang's implicit conversions make both operands string here. Only
            // comparisons apply to strings; ===/!== degenerate to ==/!= since a
            // string carries no X/Z (LRM 6.16.9).
            switch (bin.op) {
                case B::Equality:
                case B::CaseEquality:
                    return b.binary(T::Eq, lhs, rhs, l);
                case B::Inequality:
                case B::CaseInequality:
                    return b.binary(T::Ne, lhs, rhs, l);
                case B::LessThan:
                    return b.binary(T::Lt, lhs, rhs, l);
                case B::LessThanEqual:
                    return b.binary(T::Le, lhs, rhs, l);
                case B::GreaterThan:
                    return b.binary(T::Gt, lhs, rhs, l);
                case B::GreaterThanEqual:
                    return b.binary(T::Ge, lhs, rhs, l);
                default:
                    fail("unsupported binary operator on strings");
            }
        }
        if (bin.left().type->isFloating() || bin.right().type->isFloating()) {
            return lower_real_binary(bin, lhs, rhs, b);
        }

        switch (bin.op) {
            case B::Add:
                return b.binary(T::Add, lhs, rhs, l);
            case B::Subtract:
                return b.binary(T::Sub, lhs, rhs, l);
            case B::Multiply:
                return b.binary(T::Mul, lhs, rhs, l);
            case B::Divide:
                return b.binary(T::Div, lhs, rhs, l);
            case B::Mod:
                return b.binary(T::Mod, lhs, rhs, l);
            case B::Power:
                return b.binary(T::Pow, lhs, rhs, l);
            case B::BinaryAnd:
                return b.binary(T::Band, lhs, rhs, l);
            case B::BinaryOr:
                return b.binary(T::Bor, lhs, rhs, l);
            case B::BinaryXor:
                return b.binary(T::Bxor, lhs, rhs, l);
            case B::BinaryXnor:
                return b.binary(T::Bxnor, lhs, rhs, l);
            case B::Equality:
                return b.binary(T::Eq, lhs, rhs, l);
            case B::Inequality:
                return b.binary(T::Ne, lhs, rhs, l);
            case B::CaseEquality:
                return b.binary(T::CaseEq, lhs, rhs, l);
            case B::CaseInequality:
                // !== is the strict inequality: logical negation of the always-known
                // === result.
                return b.unary(T::Lnot, b.binary(T::CaseEq, lhs, rhs, l), l);
            case B::WildcardEquality:
                return b.binary(T::WildEq, lhs, rhs, l);
            case B::WildcardInequality:
                // !=? is the logical negation of ==? (X-propagating, so Lnot keeps
                // x as x).
                return b.unary(T::Lnot, b.binary(T::WildEq, lhs, rhs, l), l);
            case B::GreaterThanEqual:
                return b.binary(T::Ge, lhs, rhs, l);
            case B::GreaterThan:
                return b.binary(T::Gt, lhs, rhs, l);
            case B::LessThanEqual:
                return b.binary(T::Le, lhs, rhs, l);
            case B::LessThan:
                return b.binary(T::Lt, lhs, rhs, l);
            case B::LogicalShiftLeft:
            case B::ArithmeticShiftLeft:
                return b.binary(T::Sll, lhs, rhs, l);
            case B::LogicalShiftRight:
                return b.binary(T::Srl, lhs, rhs, l);
            case B::ArithmeticShiftRight:
                return b.binary(T::Sra, lhs, rhs, l);
            default:
                fail("unsupported binary operator");
        }
    }

    // The `inside` set-membership operator (LRM 11.4.13): `sel inside { m0, m1, … }`
    // is 1'b1 if any member matches, 1'bx if some comparison is x with no definite
    // match, else 1'b0 — exactly a logical-OR reduction of the per-member
    // predicates. slang has already widened the left operand and every member (and
    // range bound) to a common comparison type (bindMembershipExpressions ->
    // contextDetermined), so the compares are width-matched.
    ir::Index lower_inside(const InsideExpression& e, ir::Builder& b) {
        const ir::SourceLoc l = loc(e);
        if (is_string_expr(e.left())) {
            // TODO: string-typed `inside` — members would need string comparison,
            // not the integral WildEq / relational ops used here.
            fail("inside operator over string operands is not supported");
        }
        if (e.left().type->isFloating()) {
            // TODO: real `inside` — members need the float compare ops (FEq / FGe /
            // FLe), not the integral WildEq / Ge / Le used here (LRM 11.4.13; reals
            // carry no x/z, so it reduces to == / >= && <=).
            fail("inside operator over real operands is not supported");
        }
        // One InsideSet node carries the whole set; member operands are still
        // evaluated in source order. A `[lo:hi]` range tests
        // `sel >= lo && sel <= hi`; a singular member uses ==? wildcard
        // equality, where only the member's own x/z bits are wildcards
        // (LRM 11.4.13), which is exactly WildEq's right-operand-wildcards
        // semantics.
        const ir::Ref          ty  = lower_type(*e.left().type);
        const ir::Index        sel = lower_expr(e.left(), b);
        std::vector<ir::Index> values;
        std::vector<ir::Index> ranges;
        for (const Expression* m : e.rangeList()) {
            if (m->kind == ExpressionKind::ValueRange) {
                const auto& vr = m->as<ValueRangeExpression>();
                if (vr.rangeKind != ValueRangeKind::Simple) {
                    // TODO: tolerance ranges (`[c +/- d]`, `[c +%- d]`, LRM 11.4.13).
                    fail("inside operator tolerance ranges are not supported");
                }
                if (vr.left().type->isUnbounded() || vr.right().type->isUnbounded()) {
                    // TODO: unbounded `$` range bounds (`[lo:$]` / `[$:hi]`).
                    fail("inside operator unbounded ($) range bounds are not supported");
                }
                ranges.push_back(b.resize(lower_expr(vr.left(), b), ty, loc(vr.left())));
                ranges.push_back(b.resize(lower_expr(vr.right(), b), ty, loc(vr.right())));
                continue;
            }
            if (m->type->getCanonicalType().isUnpackedArray()) {
                // TODO: an unpacked-array set member (`sel inside {arr}`) matches any
                // element (LRM 11.4.13 array unwrap); needs runtime element iteration.
                fail("inside operator with an unpacked-array set member is not supported");
            }
            values.push_back(b.resize(lower_expr(*m, b), ty, loc(*m)));
        }
        return b.inside_set(sel, values, ranges, l);
    }

    // Build the lvalue for a part-select over an already-lowered base lvalue: a
    // constant `[msb:lsb]` slice, or a runtime-base indexed slice `[off +: w]` /
    // `[off -: w]` (width constant, base a runtime value). Shared by the rvalue and
    // lvalue paths so a read and a write of the same shape lower identically.
    ir::Index lower_range_select(const RangeSelectExpression& rs, ir::Index base, ir::Builder& b, ir::SourceLoc l) {
        if (is_queue(rs.value()) || is_dyn_array(rs.value())) {
            // TODO: queue / dynamic-array slice `q[a:b]` (LRM 7.10.4) yields a new
            // queue; needs an element-range copy into a fresh handle, not a packed
            // part-select.
            fail("slice of a queue / dynamic array is not supported");
        }
        if (is_unpacked_array(rs.value())) {
            // TODO: fixed-size unpacked array slice (LRM 7.4.5 `busB = busA[7:6]`,
            // "a slice name of an unpacked array is an unpacked array"). Element
            // storage is address-indexed, not a flat bit-vector, so the packed
            // part-select math below is wrong for it -- guard loudly rather than
            // let a slice comparison (`busA[7:6] == other`) silently fall through
            // to a bogus bit-level compare (only caught here because neither
            // operand is a *plain* whole-array reference -- that path is guarded
            // separately, "whole unpacked array read is not supported" -- so two
            // sliced operands slipped past every existing check).
            fail("slice of a fixed-size unpacked array is not supported");
        }
        // A range select on a multi-dim packed array picks whole W-wide elements
        // (LRM 7.4.6): both bounds and the width scale by the element width W.
        // W is 1 for an ordinary vector, leaving the bit-select math unchanged.
        const auto* pa = rs.value().type->getCanonicalType().as_if<slang::ast::PackedArrayType>();
        const uint32_t w = pa != nullptr ? static_cast<uint32_t>(pa->elementType.getBitWidth()) : 1;
        const slang::ConstantRange range = packed_range_of(rs.value());
        if (rs.getSelectionKind() == RangeSelectionKind::Simple) {
            const std::optional<int32_t> left  = try_const_int(rs.left());
            const std::optional<int32_t> right = try_const_int(rs.right());
            if (!left || !right) {
                fail("expected constant part-select bounds");
            }
            // translateIndex puts the left bound at the higher LSB offset for
            // either endianness (slang has already checked the select's
            // direction matches the declared range). Scaled by W: the top
            // element's high bit is `tl*W + W-1`, the bottom element's low bit
            // is `tr*W`.
            const auto msb = static_cast<uint32_t>(range.translateIndex(*left)) * w + (w - 1);
            const auto lsb = static_cast<uint32_t>(range.translateIndex(*right)) * w;
            return b.part_select(base, msb, lsb, l);
        }
        // Indexed part-select: the element count is a constant (LRM 11.5.1) and
        // the base is a runtime value in declared-range coordinates. The IR
        // carries the low-bit LSB offset: the translated base element offset
        // (minus count-1 when the selection extends away from the LSB end — `-:`
        // on a little-endian range, `+:` on a big-endian one) scaled by W. An
        // out-of-range or X/Z base is bounds-checked in the backend per LRM
        // 11.5.1 (out-of-range read -> X, write -> no-op; a fully-out-of-range
        // W-wide window fills X / no-ops as a whole).
        const auto    count = static_cast<uint32_t>(const_uint(rs.right()));
        const bool    down  = rs.getSelectionKind() == RangeSelectionKind::IndexedDown;
        const int32_t bias  = down == range.isDescending() ? static_cast<int32_t>(count) - 1 : 0;
        const ir::Index off = lower_packed_offset(rs.left(), range, bias, b, l, w);
        return b.dyn_part_select(base, off, count * w, l);
    }

    // Lower an element select on a packed value to its lvalue index. A 1-bit
    // element (ordinary vector `v[i]`) is a BitSelect. A wide element (a
    // multi-dim packed array `a[i]`, LRM 7.4.5) selects the whole W-wide element
    // k at flat bits [k*W .. k*W+W-1], so it is a W-wide part-select whose low
    // bit is the normalized element offset scaled by W. Nested selects compose
    // for free: the result type slang assigns to `a[i]` carries the inner range,
    // which a further select indexes the same way (part-select-of-part-select).
    ir::Index lower_packed_element_lvalue(const ElementSelectExpression& es, ir::Index base,
                                          ir::Builder& b, ir::SourceLoc l) {
        const auto w = static_cast<uint32_t>(es.type->getBitWidth());
        if (w == 1) {
            return b.binary(ir::Inst::Tag::BitSelect, base, packed_bit_index(es, b, l), l);
        }
        const slang::ConstantRange range = packed_range_of(es.value());
        if (const std::optional<int32_t> c = try_const_int(es.selector())) {
            const auto lsb = static_cast<uint32_t>(range.translateIndex(*c)) * w;
            return b.part_select(base, lsb + w - 1, lsb, l);
        }
        const ir::Index off = lower_packed_offset(es.selector(), range, 0, b, l, w);
        return b.dyn_part_select(base, off, w, l);
    }

    // Struct/union member select (LRM 7.2.1 / 7.3): a packed struct or union is
    // a flat bit-vector, so `x.field` is exactly a constant part-select at the
    // field's declared bit offset (0 for every union member — all members
    // overlay the LSB) — the same IR shape (PartSelect over the base lvalue)
    // as a plain `vec[msb:lsb]`, so it reuses the backend's existing
    // partial-write / NBA-shadow machinery with no backend changes. Nested
    // struct member access (`x.outer.inner`) composes for free: the base
    // lvalue is itself a part-select, and part-select-of-a-part-select
    // already works (the same shape a multi-dimensional packed select uses).
    ir::Index lower_member_lvalue(const MemberAccessExpression& ma, ir::Builder& b, ir::SourceLoc l) {
        if (const auto* prop = ma.member.as_if<slang::ast::ClassPropertySymbol>()) {
            // `obj.prop` (LRM 8.4): load the object handle (a value), then a
            // handle-deref + field select rooted at the object body. The base
            // handle is an rvalue (the pointer), not the slot's lvalue.
            const ir::Index handle = lower_expr(ma.value(), b);
            return class_member_lvalue(handle, *prop, lower_type(*ma.value().type), b, l);
        }
        const auto* field = ma.member.as_if<FieldSymbol>();
        if (field == nullptr) {
            // TODO: non-field member access (interface modports, ...) — not lowered.
            fail("unsupported member access");
        }
        if (ma.value().type->isUnpackedUnion()) {
            // TODO: unpacked unions need overlapping aggregate storage (a byte
            // blob every member reinterprets), which the sequential-field
            // agg_storage_ty mirror does not model. A separate change.
            fail("unpacked union member access is not supported");
        }
        // A packed union member sits at bit_offset 0 (all members overlay the LSB),
        // so it flows to the packed part-select path below unchanged.
        const ir::Index base = lower_lvalue(ma.value(), b);
        // An unpacked struct is an aggregate: the member is addressed by field
        // index (MemberSelect -> struct GEP), not a bit range. A packed struct is
        // a flat bit-vector, so the member is a constant part-select.
        if (ma.value().type->getCanonicalType().isUnpackedStruct()) {
            return b.member_select(base, field->fieldIndex, lower_type(*ma.value().type), l);
        }
        const auto lsb = static_cast<uint32_t>(field->bitOffset);
        const auto msb = lsb + static_cast<uint32_t>(ma.type->getBitWidth()) - 1;
        return b.part_select(base, msb, lsb, l);
    }

    // Lower an expression as an lvalue, returning the lvalue index (no Load).
    ir::Index lower_lvalue(const Expression& expr, ir::Builder& b) {
        const ir::SourceLoc l = loc(expr);
        switch (expr.kind) {
            case ExpressionKind::Assignment:
                // slang (post-8.0) binds a system-task output argument ($fgets,
                // $sformat, $*scanf, ... destinations) as an AssignmentExpression
                // whose left side is the target lvalue, the same shape a user
                // task's output actual has. Unwrap to the target.
                return lower_lvalue(expr.as<AssignmentExpression>().left(), b);
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
                // A hierarchical lvalue (`top.u1.sig = …`, LRM 23.6) writes the
                // target instance's signal; the resolved symbol goes through the
                // ordinary signal path (selects below recurse to this base).
                return value_lvalue(expr.as<ValueExpressionBase>().symbol, b, l);
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es   = expr.as<ElementSelectExpression>();
                const ir::Index                base = lower_lvalue(es.value(), b);
                if (is_unpacked_array(es.value()) || is_dyn_array(es.value()) || is_queue(es.value())) {
                    return var_elem_select(es, base, b, l);
                }
                return lower_packed_element_lvalue(es, base, b, l);
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs   = expr.as<RangeSelectExpression>();
                const ir::Index              base = lower_lvalue(rs.value(), b);
                return lower_range_select(rs, base, b, l);
            }
            case ExpressionKind::MemberAccess:
                return lower_member_lvalue(expr.as<MemberAccessExpression>(), b, l);
            case ExpressionKind::Call:
                // A dynamic-array / queue-returning call is an lvalue: its result
                // handle (allocated by the backend) is read by an assignment /
                // .size() / element access / foreach. Others are not lvalues.
                if (is_handle_expr(expr)) {
                    const std::optional<ir::Index> v = lower_user_call(expr.as<CallExpression>(), b);
                    if (!v) {
                        fail("handle-returning call used as an lvalue produced no value");
                    }
                    return *v;
                }
                fail("unsupported assignment target kind=Call");
            default:
                fail("unsupported assignment target kind=" + std::string(slang::ast::toString(expr.kind)));
        }
    }

    // Evaluate a constant integer expression (a literal, a parameter reference,
    // or any constant-foldable expression over them, e.g. `W-1` or `W*2`).
    // Folded with slang against the current instance scope so parameterized
    // widths, replication counts and part-select bounds resolve to a value.
    uint64_t const_uint(const Expression& e) {
        slang::ast::ASTContext  astCtx(*scope_, slang::ast::LookupLocation::max);
        slang::ast::EvalContext evalCtx(astCtx);
        const slang::ConstantValue cv = e.eval(evalCtx);
        if (!cv.isInteger()) {
            fail("expected a constant integer");
        }
        return cv.integer().as<uint64_t>().value_or(0);
    }

    // Constant-fold an integer expression against the current scope; nullopt
    // when it is non-constant (a runtime index) or holds X/Z bits.
    std::optional<int32_t> try_const_int(const Expression& e) {
        slang::ast::ASTContext  astCtx(*scope_, slang::ast::LookupLocation::max);
        slang::ast::EvalContext evalCtx(astCtx);
        const slang::ConstantValue cv = e.eval(evalCtx);
        if (!cv.isInteger()) {
            return std::nullopt;
        }
        return cv.integer().as<int32_t>();
    }

    // -- Packed-select index normalization --
    //
    // The IR models every packed type as [width-1:0] (make_int_type), so a
    // select index written against the declared range ([0:7], [15:8],
    // [-1:-8], ...) must be translated to a zero-based LSB offset before
    // BitSelect / PartSelect / DynPartSelect (LRM 11.5): a little-endian
    // range [l:r] maps index i to i - r, a big-endian range to r - i.

    // Declared range of the packed value a select indexes into. Taken from
    // slang's type so nested selects use whatever coordinates slang assigned
    // to the inner select's result.
    static slang::ConstantRange packed_range_of(const Expression& value) {
        return value.type->getCanonicalType().getFixedRange();
    }

    // Lower a packed-select index to its 32-bit zero-based LSB offset, minus
    // `bias` (0 for a bit select; width-1 when an indexed part-select's low
    // bit sits below the translated base), then scaled by `stride` (the element
    // bit width for a multi-dim packed array element/range select, 1 otherwise).
    // A constant index folds to an immediate. A runtime index is widened by its
    // own signedness (so a signed index into a negative-bound range resolves
    // correctly) and keeps the source's 4-state-ness so an X/Z index still
    // poisons the offset arithmetic instead of being silently coerced to a known
    // value. The stride multiply is built in `off_ty` so it shares the offset's
    // domain (a 4-state offset is physically wider than a 2-state constant, so a
    // mismatched-domain Mul operand would be an invalid instruction).
    ir::Index lower_packed_offset(const Expression& idx, slang::ConstantRange range, int32_t bias,
                                  ir::Builder& b, ir::SourceLoc l, uint32_t stride = 1) {
        if (const std::optional<int32_t> c = try_const_int(idx)) {
            const auto off = static_cast<uint32_t>((range.translateIndex(*c) - bias) * static_cast<int32_t>(stride));
            return b.constant(module_.pool.put_int(make_int_type(32, false, false), off), l);
        }
        const ir::Ref   off_ty = make_int_type(32, idx.type->isFourState(), idx.type->isSigned());
        const ir::Index v      = b.resize(lower_expr(idx, b), off_ty, l);
        ir::Index       off;
        if (range.isDescending()) {
            const int32_t sub = range.right + bias; // off = idx - right - bias
            off = sub == 0 ? v
                           : b.binary(ir::Inst::Tag::Sub, v,
                                      b.constant(module_.pool.put_int(off_ty, static_cast<uint32_t>(sub)), l), l);
        } else {
            // Big-endian: off = (right - bias) - idx.
            off = b.binary(ir::Inst::Tag::Sub,
                           b.constant(module_.pool.put_int(off_ty, static_cast<uint32_t>(range.right - bias)), l),
                           v, l);
        }
        if (stride == 1) {
            return off;
        }
        return b.binary(ir::Inst::Tag::Mul, off,
                        b.constant(module_.pool.put_int(off_ty, stride), l), l);
    }

    // Normalized bit offset for a 1-bit element select on a packed value.
    // Wide (multi-dim packed array) elements go through
    // lower_packed_element_lvalue, which part-selects instead of bit-selecting;
    // this is only ever reached for a 1-bit result.
    ir::Index packed_bit_index(const ElementSelectExpression& es, ir::Builder& b, ir::SourceLoc l) {
        return lower_packed_offset(es.selector(), packed_range_of(es.value()), 0, b, l);
    }

    // -- Functions --
    //
    // A function lowers to its own ir::Function (a real callable), not inlined.
    // Its body references parameters via Arg (copied into a Local at entry so the
    // body may mutate them), function-frame storage via Local, and module signals
    // via Signal; every path ends in a Ret. The backend emits one LLVM function
    // per ir::Function and calls it at each `call` site, so recursion and
    // `automatic` lifetime come from ordinary stack frames.

    // Build-time state for the function currently being lowered. Local indices are
    // handed out densely as formals / the return variable / body locals are seen;
    // `local_types` (parallel to the indices) becomes the Function's frame layout.
    struct FnCtx {
        // Value symbol -> frame Local index (automatic-lifetime locals only).
        ankerl::unordered_dense::map<const ValueSymbol*, uint32_t> localmap;
        // Storage type per Local index.
        std::vector<ir::Ref> local_types;
        // The return variable's Local index / type (valid iff has_ret_local).
        uint32_t ret_local     = 0;
        ir::Ref  ret_type      = ir::Ref::Invalid;
        bool     has_ret_local = false;
        // Name qualifier (e.g. "f.") prepended to a static local's signal name so
        // same-named statics in different functions stay distinct.
        std::string name_prefix;
        // True while lowering a task body (a coroutine): unlike a function, timing
        // controls and $time are allowed.
        bool is_task = false;
        // Class method context (valid iff has_this): `this` is spliced as frame
        // Local 0, `this_class` is its Kind::Class handle type. A bare property
        // reference inside the body roots at this Local (value_lvalue).
        bool     has_this   = false;
        uint32_t this_local = 0;
        ir::Ref  this_class = ir::Ref::Invalid;
    };

    // Get (creating if needed) the frame Local index for a function-local symbol.
    uint32_t fn_local(const ValueSymbol& vs) {
        const auto it = fn_ctx_->localmap.find(&vs);
        if (it != fn_ctx_->localmap.end()) {
            return it->second;
        }
        const auto idx = static_cast<uint32_t>(fn_ctx_->local_types.size());
        fn_ctx_->local_types.push_back(lower_type(vs.getType()));
        fn_ctx_->localmap[&vs] = idx;

        return idx;
    }

    // A static-lifetime function local: it persists across calls (LRM 13.4.2), so
    // it gets persistent module-signal storage, initialized once at time zero —
    // not the per-call frame. Named with the function qualifier to avoid clashes.
    ir::Signal declare_static_local(const ValueSymbol& vs) {
        const auto it = sigmap_.find(&vs);
        if (it != sigmap_.end()) {
            return it->second;
        }
        ir::SignalInfo info;
        info.name = prefix_ + fn_ctx_->name_prefix + std::string(vs.name);
        info.type = lower_type(vs.getType());
        info.dir  = ir::SignalInfo::Direction::Internal;
        const ir::Signal sig = register_signal(std::move(info));
        sigmap_[&vs]         = sig;

        return sig;
    }

    // True if `vs` is a static-lifetime function local (persistent storage). A
    // loop control variable is always treated as per-call (force_frame), since its
    // value is reset by the loop initialization on every entry regardless.
    bool is_static_local(const VariableSymbol& vs, bool force_frame) const {
        return fn_ctx_ != nullptr && !force_frame && vs.lifetime == slang::ast::VariableLifetime::Static;
    }

    // Ensure storage exists for a procedural local declaration / loop variable.
    // Outside a function: a module Signal. Inside a function: a persistent Signal
    // for a static local, else a per-call frame Local.
    void declare_storage(const VariableSymbol& vs, bool force_frame) {
        if (fn_ctx_ != nullptr && vs.getType().getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
            // TODO: unpacked arrays as function-frame locals. The frame layout has no
            // per-element array storage yet; only module-level memories are supported.
            fail("unpacked array as a function local is not supported");
        }
        if (fn_ctx_ == nullptr) {
            declare_local(vs);
        } else if (is_static_local(vs, force_frame)) {
            declare_static_local(vs);
        } else {
            fn_local(vs);
        }
    }

    // The lvalue leaf for a value-symbol reference: a function-frame Local if the
    // symbol is one of the current function's locals, else a module Signal (which
    // a function may also reference directly). signal_of fails loudly if unknown.
    ir::Index value_lvalue(const ValueSymbol& vs, ir::Builder& b, ir::SourceLoc l) {
        if (fn_ctx_ != nullptr) {
            const auto it = fn_ctx_->localmap.find(&vs);
            if (it != fn_ctx_->localmap.end()) {
                return b.local(it->second, fn_ctx_->local_types[it->second], l);
            }
            if (fn_ctx_->has_this) {
                if (const auto* prop = vs.as_if<slang::ast::ClassPropertySymbol>()) {
                    // A bare property reference inside a method (implicit `this`):
                    // load the `this` handle (Local 0) and select the property.
                    const ir::Index handle = b.unary(
                      ir::Inst::Tag::Load, b.local(fn_ctx_->this_local, fn_ctx_->this_class, l), l);
                    return class_member_lvalue(handle, *prop, fn_ctx_->this_class, b, l);
                }
            }
        }

        return b.signal(signal_of(vs), l);
    }

    void lower_return(const ReturnStatement& r, ir::Builder& b) {
        if (fn_ctx_ == nullptr) {
            fail("return statement outside of a function");
        }
        const ir::SourceLoc l = loc(r);
        if (r.expr != nullptr) {
            if (!fn_ctx_->has_ret_local) {
                fail("return with a value in a void function");
            }
            // An aggregate result's Ret operand is the returned object's lvalue
            // (the backend copies its storage out through the return out-param).
            if (contains_struct_ref(fn_ctx_->ret_type)) {
                const Expression& src = unwrap_conversion(*r.expr);
                if (!is_struct_lvalue_shape(src)) {
                    // TODO: return of a `'{...}` pattern / call result — materialize
                    // into a temp first.
                    fail("aggregate return of a non-lvalue expression is not supported");
                }
                b.unary(ir::Inst::Tag::Ret, lower_lvalue(src, b), l);
                return;
            }
            // A dynamic-array result is returned as its handle lvalue; the backend
            // deep-copies it into the caller's result handle (LRM 13.4 value return).
            if (is_handle_ref(fn_ctx_->ret_type)) {
                b.unary(ir::Inst::Tag::Ret, lower_lvalue(*r.expr, b), l);
                return;
            }
            // A string result needs no width resize (slang already converted).
            const ir::Index v = lower_expr(*r.expr, b);
            b.unary(ir::Inst::Tag::Ret,
                    is_string_ref(fn_ctx_->ret_type)
                      ? v
                      : coerce(v, *r.expr->type, fn_ctx_->ret_type, b, l),
                    l);
        } else if (fn_ctx_->has_ret_local) {
            // `return;` in a non-void function yields the current function-var value:
            // an aggregate / dynamic array yields its lvalue (copied out by the
            // backend), else the loaded value.
            if (contains_struct_ref(fn_ctx_->ret_type)) {
                b.unary(ir::Inst::Tag::Ret, b.local(fn_ctx_->ret_local, fn_ctx_->ret_type, l), l);
                return;
            }
            const ir::Index rv = b.local(fn_ctx_->ret_local, fn_ctx_->ret_type, l);
            b.unary(ir::Inst::Tag::Ret,
                    is_handle_ref(fn_ctx_->ret_type) ? rv : b.unary(ir::Inst::Tag::Load, rv, l), l);
        } else {
            b.ret_void(l);
        }
    }

    // True for a subroutine declared directly in a package or `$unit` scope
    // (LRM 6.8's "no instance multiplicity" applies to these exactly as it
    // does to a variable): walks up from the subroutine's declaring scope,
    // stopping at the first Package/CompilationUnit (global) or InstanceBody
    // (module-scope, possibly through an intervening generate block).
    static bool is_global_subroutine(const SubroutineSymbol& sub) {
        const Scope* s = sub.getParentScope();
        while (s != nullptr) {
            switch (s->asSymbol().kind) {
                case SymbolKind::Package:
                case SymbolKind::CompilationUnit:
                    return true;
                case SymbolKind::InstanceBody:
                    return false;
                default:
                    s = s->asSymbol().getParentScope();
            }
        }
        return false;
    }

    // Resolve a subroutine (function or task) to a FunctionId, lowering its body
    // on first reference. The id is reserved before the body is built so a
    // recursive call resolves it. A package/$unit-scope subroutine has no
    // instance multiplicity (LRM 6.8): delegate its whole lowering to the
    // shared globals_ Lowerer instead of duplicating it (and any static
    // local's storage) per top instance.
    // The DPI-C C type of a formal / return type (LRM 35.5.6): a by-value scalar
    // for the small named types, else the by-pointer chunk-array form (BitVec /
    // LogicVec) for a packed vector. nullopt for a type Clotho does not marshal
    // (string, chandle, open array, unpacked aggregate) — the caller loud-fails
    // those. Never returns Void.
    static std::optional<ir::DpiCType> dpi_ctype(const slang::ast::Type& type) {
        const slang::ast::Type& ct = type.getCanonicalType();
        if (ct.isFloating()) {
            return ct.as<slang::ast::FloatingType>().floatKind == slang::ast::FloatingType::ShortReal
                     ? ir::DpiCType::Shortreal
                     : ir::DpiCType::Real;
        }
        if (ct.isPredefinedInteger()) {
            switch (ct.as<slang::ast::PredefinedIntegerType>().integerKind) {
                case slang::ast::PredefinedIntegerType::Byte:
                    return ir::DpiCType::Byte;
                case slang::ast::PredefinedIntegerType::ShortInt:
                    return ir::DpiCType::Shortint;
                case slang::ast::PredefinedIntegerType::Int:
                    return ir::DpiCType::Int;
                case slang::ast::PredefinedIntegerType::LongInt:
                    return ir::DpiCType::Longint;
                default:
                    // `integer` (4-state 32) / `time` (2-state 64): the vec form.
                    return ct.isFourState() ? ir::DpiCType::LogicVec : ir::DpiCType::BitVec;
            }
        }
        if (ct.isScalar()) {
            return ct.isFourState() ? ir::DpiCType::LogicScalar : ir::DpiCType::BitScalar;
        }
        if (ct.isCHandle()) {
            return ir::DpiCType::Chandle; // opaque void*
        }
        if (ct.isString()) {
            return ir::DpiCType::String; // char*
        }
        if (ct.isIntegral()) {
            // A packed vector / packed struct / enum: the svBitVecVal /
            // svLogicVecVal chunk-array form (slang has already checked it is a
            // legal DPI arg type via isValidForDPIArg).
            return ct.isFourState() ? ir::DpiCType::LogicVec : ir::DpiCType::BitVec;
        }
        return std::nullopt; // chandle, string, unpacked array / struct, ...
    }

    // Lower a DPI-C import (LRM 35) into the reserved slot. There is no IR body:
    // the backend emits an external C declaration keyed on the linkage name and
    // marshals each scalar argument at the call site (lower_dpi_call). PoC scope:
    // import functions, `input` arguments, scalar-mapped types only — everything
    // else loud-fails with a TODO (see docs/design/dpi-c.md).
    void build_dpi_import(const SubroutineSymbol& sub, ir::FunctionId id) {
        if (sub.subroutineKind == SubroutineKind::Task) {
            // TODO: DPI import tasks (LRM 35.5.5) — can consume time via disable;
            // the native boundary is coroutine-shaped. Import functions only.
            fail("DPI import tasks are not supported");
        }
        // `context` (LRM 35.5.3) is accepted and lowered identically to a plain
        // import: its purpose is to let the C call exported SV functions (and
        // svScope APIs), which Clotho's export thunks already service against the
        // single flat design-state global — no scope tracking needed for a single
        // instance. (The svScope C API — svGetScope/svSetScope — is not provided;
        // C that calls it gets an unresolved-symbol error, the deferred piece.)

        // A DPI import has no body, so slang leaves returnValVar null even for a
        // non-void function; decide void-ness from the declared return type.
        ir::DpiCType ret_ct = ir::DpiCType::Void;
        ir::Ref      ret_ty = module_.pool.void_type;
        if (!sub.getReturnType().isVoid()) {
            const auto c = dpi_ctype(sub.getReturnType());
            if (!c.has_value() || *c == ir::DpiCType::BitVec || *c == ir::DpiCType::LogicVec) {
                // A DPI return must be a "small value" (LRM 35.5.5): slang already
                // rejects a packed-array return, so a vec here is defensive. string
                // / chandle returns are the deferred marshal.
                fail("DPI return type is not a supported scalar type (only "
                     "byte/shortint/int/longint/real/shortreal/bit/logic)");
            }
            ret_ct = *c;
            ret_ty = lower_type(sub.getReturnType());
        }

        std::vector<ir::Ref>      param_types;
        std::vector<ir::ArgDir>   arg_dirs;
        std::vector<ir::DpiCType> arg_cts;
        std::vector<ir::DpiCType> arg_elem_cts;
        for (const FormalArgumentSymbol* fa : sub.getArguments()) {
            if (fa->direction == slang::ast::ArgumentDirection::Ref) {
                // DPI-C has no `ref` formals (slang rejects DPIRefArg); guard
                // defensively in case a future slang change lets one through.
                fail("DPI ref arguments are not supported");
            }
            // Open array (LRM 35.5.6.2): an unsized `[]` formal arrives as an
            // svOpenArrayHandle. The element's own DpiCType is recorded separately;
            // the bounds/storage come from the actual at each call.
            if (const auto* oa =
                    fa->getType().getCanonicalType().as_if<slang::ast::DPIOpenArrayType>()) {
                // input / output / inout all supported: the C reads (input/inout) and
                // writes (output/inout) elements through svGet/PutArrElem. The backend
                // marshals in before the call and back out after (or, for a named-
                // scalar zero-copy element, the C writes the design array directly).
                build_dpi_open_array_arg(*oa, to_arg_dir(fa->direction), param_types,
                                         arg_dirs, arg_cts, arg_elem_cts);
                continue;
            }
            const auto c = dpi_ctype(fa->getType());
            if (!c.has_value()) {
                // multi-dim open array / unpacked aggregate — deferred.
                fail("DPI argument type is not supported (scalar, packed vector, "
                     "chandle, string, 1-D open array only; not "
                     "multi-dim-open-array/unpacked)");
            }
            if (*c == ir::DpiCType::String && fa->direction != slang::ast::ArgumentDirection::In) {
                // TODO: output / inout string. The C writes an unbounded NUL-
                // terminated char* back and DPI gives no size contract, so there is
                // no safe fixed buffer — deferred (see docs/design/dpi-c.md).
                fail("DPI output/inout string arguments are not supported");
            }
            // input / output / inout: the direction drives the C ABI (by value vs
            // by pointer) in declare_dpi_import / lower_dpi_call. build_call_args
            // already makes the caller temp + CopyOutArg for a non-In direction.
            param_types.push_back(lower_type(fa->getType()));
            arg_dirs.push_back(to_arg_dir(fa->direction));
            arg_cts.push_back(*c);
            arg_elem_cts.push_back(ir::DpiCType::Void);
        }

        // The C linkage name is the import's c_identifier, or the SV name when
        // omitted (LRM 35.5.4). slang does not store it on the symbol — read it
        // off the syntax node.
        std::string cid(sub.name);
        if (const auto* syn = sub.getSyntax()) {
            if (const auto* d = syn->as_if<slang::syntax::DPIImportSyntax>()) {
                if (d->c_identifier) {
                    cid = std::string(d->c_identifier.valueText());
                }
            }
        }

        // Emit a trivial void Ret so the (never-lowered) Ir is well-formed for the
        // whole-function scans in the backend (nba shadow, ptr-arg signals).
        ir::Builder         b(module_.pool);
        const ir::SourceLoc el = to_source_loc(sub.location);
        b.ret_void(el);

        // Mirror param_types into local_types (formal k == Local k), the invariant
        // every whole-function analysis assumes (e.g. compute_may_z joins param_z
        // into local_z). The backend never materializes these — lower_function
        // early-returns for a DPI import — so they are phantom slots.
        std::vector<ir::Ref> local_types = param_types;

        module_.set_function(id, ir::Function{ .name           = std::string(sub.name),
                                               .return_type    = ret_ty,
                                               .param_types    = std::move(param_types),
                                               .arg_dirs       = std::move(arg_dirs),
                                               .local_types    = std::move(local_types),
                                               .is_dpi         = true,
                                               .is_pure        = sub.flags.has(MethodFlags::Pure),
                                               .extern_name    = std::move(cid),
                                               .dpi_arg_ctypes = std::move(arg_cts),
                                               .dpi_ret_ctype  = ret_ct,
                                               .dpi_arg_elem_ctypes = std::move(arg_elem_cts),
                                               .ir             = b.finish() });
    }

    // Classify a 1-D DPI open-array formal (LRM 35.5.6.2). Records DpiCType::OpenArray
    // in arg_cts and the element's own DpiCType in arg_elem_cts; param_types gets the
    // element's lowered type (a phantom slot — the backend reads the actual's array
    // type at each call, but the whole-function scans want a benign non-handle Ref).
    // 1-D, with a marshalable element (named scalar / packed bit / packed logic), in
    // any direction; everything else loud-fails.
    void build_dpi_open_array_arg(const slang::ast::DPIOpenArrayType& oa, ir::ArgDir dir,
                                  std::vector<ir::Ref>&      param_types,
                                  std::vector<ir::ArgDir>&   arg_dirs,
                                  std::vector<ir::DpiCType>& arg_cts,
                                  std::vector<ir::DpiCType>& arg_elem_cts) {
        const slang::ast::Type& elem = oa.elementType.getCanonicalType();
        if (elem.isUnpackedArray() || elem.getCanonicalType().as_if<slang::ast::DPIOpenArrayType>()) {
            // TODO: multi-dimensional open array (element itself an array). Deferred —
            // the descriptor + marshal are 1-D only.
            fail("DPI multi-dimensional open-array arguments are not supported");
        }
        const auto ec = dpi_ctype(elem);
        // V1 elements: the named integers (Byte/Shortint/Int/Longint — zero-copy,
        // Clotho's narrow element storage is already a contiguous C array) and packed
        // bit/logic vectors (marshalled via rt_dpi_pack_arr_*). Scalar bit/logic, real,
        // string, chandle elements are deferred (docs/design/dpi-c.md).
        const bool ok = ec.has_value() &&
                        (*ec == ir::DpiCType::Byte || *ec == ir::DpiCType::Shortint ||
                         *ec == ir::DpiCType::Int || *ec == ir::DpiCType::Longint ||
                         *ec == ir::DpiCType::BitVec || *ec == ir::DpiCType::LogicVec);
        if (!ok) {
            fail("DPI open-array element type is not supported (byte/shortint/int/"
                 "longint or packed bit/logic vector only)");
        }
        // Any element width is supported: <= 64 bits marshals through the
        // autovectorized C runtime (1 word for <= 32, 2 for 33..64), wider elements
        // through inline LLVM IR (native iW/i2W ops) at the call site.
        param_types.push_back(lower_type(elem));
        arg_dirs.push_back(dir);
        arg_cts.push_back(ir::DpiCType::OpenArray);
        arg_elem_cts.push_back(*ec);
    }

    // Collect `export "DPI-C"` directives (LRM 35.5): for each, force-lower the SV
    // function and record a DpiExport so the backend emits its C-callable thunk.
    // slang resolves the export -> subroutine binding and the c_identifier
    // (Compilation::getDPIExports). Scope: functions; input/output/inout scalar /
    // chandle / real / packed-vector args; input string args; scalar/chandle/real/
    // void return. Deferred (loud-fail): string return (returned-char* lifetime),
    // output/inout string, open-array/unpacked args, tasks. Public: called by
    // lower_compilation.
public:
    void collect_dpi_exports(const slang::ast::Compilation& comp) {
        for (const auto& ex : comp.getDPIExports()) {
            const SubroutineSymbol& sub = *ex.subroutine;
            if (sub.subroutineKind == SubroutineKind::Task) {
                // TODO: exported tasks — need the timing wheel + coroutine driving
                // from C (docs/design/dpi-c.md).
                fail("DPI export tasks are not supported");
            }
            ir::DpiCType ret_ct = ir::DpiCType::Void;
            if (!sub.getReturnType().isVoid()) {
                const auto c = dpi_ctype(sub.getReturnType());
                if (!c.has_value() || *c == ir::DpiCType::BitVec ||
                    *c == ir::DpiCType::LogicVec || *c == ir::DpiCType::String) {
                    // slang rejects a packed-array return; a string return is deferred
                    // (the returned char* would dangle past the thunk).
                    fail("DPI export return type is not supported "
                         "(scalar/chandle/real; string return is deferred)");
                }
                ret_ct = *c;
            }
            std::vector<ir::DpiCType> arg_cts;
            std::vector<ir::ArgDir>   arg_dirs;
            for (const FormalArgumentSymbol* fa : sub.getArguments()) {
                if (fa->direction == slang::ast::ArgumentDirection::Ref) {
                    fail("DPI export ref arguments are not supported");
                }
                const auto c = dpi_ctype(fa->getType());
                if (!c.has_value()) {
                    fail("DPI export argument type is not supported "
                         "(open-array / unpacked aggregate)");
                }
                if (*c == ir::DpiCType::String &&
                    fa->direction != slang::ast::ArgumentDirection::In) {
                    // TODO: output / inout string (unbounded C-written char*).
                    fail("DPI export output/inout string arguments are not supported");
                }
                arg_cts.push_back(*c);
                arg_dirs.push_back(to_arg_dir(fa->direction));
            }
            const ir::FunctionId id = function_id(sub); // force-lower the SV function
            module_.dpi_exports.push_back(ir::DpiExport{
              id, std::string(ex.cIdentifier), std::move(arg_cts), std::move(arg_dirs), ret_ct });
        }
    }

private:
    ir::FunctionId function_id(const SubroutineSymbol& sub) {
        if (globals_ != nullptr && this != globals_ && is_global_subroutine(sub)) {
            const Scope* saved_scope = globals_->scope_;
            globals_->scope_         = sub.getParentScope();
            const ir::FunctionId id  = globals_->function_id(sub);
            globals_->scope_         = saved_scope;
            return id;
        }

        const auto cached = fnmap_.find(&sub);
        if (cached != fnmap_.end()) {
            return cached->second;
        }
        if (sub.flags.has(MethodFlags::DPIImport)) {
            const ir::FunctionId id = module_.reserve_function();
            fnmap_[&sub]            = id;
            build_dpi_import(sub, id);
            return id;
        }
        const bool is_task = sub.subroutineKind == SubroutineKind::Task;
        for (const FormalArgumentSymbol* fa : sub.getArguments()) {
            const auto dir = fa->direction;
            if (dir == slang::ast::ArgumentDirection::Ref) {
                // ref / const ref (LRM 13.5.2): the formal aliases the actual's live
                // storage through a passed pointer (see build_function / build_task
                // and the backend ref ABI). Supported for packed integral formals
                // (scalar / packed vector) and dynamic arrays (the handle pointer is
                // aliased directly).
                const auto& ct = fa->getType().getCanonicalType();
                if (!ct.isIntegral() && ct.as_if<slang::ast::DynamicArrayType>() == nullptr) {
                    // TODO: ref of unpacked array / struct / string / real — needs a
                    // handle pass or offset plumbing, not the packed-slot pointer ABI.
                    fail("ref arguments are only supported for packed integral types "
                         "and dynamic arrays");
                }
                continue;
            }
            if (dir != slang::ast::ArgumentDirection::In &&
                fa->getType().getCanonicalType().isString()) {
                // TODO: output / inout string formals (the copy-out path passes
                // plane pointers, not string handles).
                fail("output/inout string arguments are not supported");
            }
        }

        const ir::FunctionId id = module_.reserve_function();
        fnmap_[&sub]            = id;
        if (is_task) {
            build_task(sub, id);
        } else {
            build_function(sub, id);
        }

        return id;
    }

    // Map a slang argument direction to the IR direction.
    static ir::ArgDir to_arg_dir(slang::ast::ArgumentDirection dir) {
        switch (dir) {
            case slang::ast::ArgumentDirection::In:
                return ir::ArgDir::In;
            case slang::ast::ArgumentDirection::Out:
                return ir::ArgDir::Out;
            case slang::ast::ArgumentDirection::InOut:
                return ir::ArgDir::Inout;
            case slang::ast::ArgumentDirection::Ref:
                return ir::ArgDir::Ref;
            default:
                return ir::ArgDir::In;
        }
    }

    // Splice the implicit `this` handle of a class method (LRM 8.11) as frame Local
    // 0. Called at the start of build_function / build_task before the real formals,
    // so `this` occupies Local 0 and every real formal / return var shifts up. A
    // free subroutine (thisVar == nullptr) is a no-op. `this` is an In pointer passed
    // by value (reference semantics): it must NOT take the aggregate copy-in, and
    // does not — a class handle is a scalar i64, routed through the scalar-arg ABI.
    void splice_this(const SubroutineSymbol& sub, std::vector<ir::Ref>& param_types,
                     std::vector<ir::ArgDir>& arg_dirs) {
        if (sub.thisVar == nullptr) {
            return;
        }
        const ir::Ref hty = lower_type(sub.thisVar->getType());
        param_types.push_back(hty);
        arg_dirs.push_back(ir::ArgDir::In);
        fn_ctx_->this_local = fn_local(*sub.thisVar);
        fn_ctx_->has_this   = true;
        fn_ctx_->this_class = hty;
    }

    // The lvalue of a class property `prop` reached through an object handle value
    // `handle_val` (the loaded i64 pointer bits of a class of type `class_ref`):
    // dereference the handle to the object body, then a MemberSelect on the
    // property's field index. Shared by member access (`obj.prop`) and a bare
    // property reference inside a method (rooted at `this`).
    ir::Index class_member_lvalue(ir::Index handle_val, const slang::ast::Symbol& prop,
                                  ir::Ref class_ref, ir::Builder& b, ir::SourceLoc l) {
        const ir::Index body = b.handle_deref(handle_val, class_ref, l);
        return b.member_select(body, class_prop_index_.at(&prop), class_ref, l);
    }

    // Lower one task body into the reserved slot. Like build_function, but the task
    // is void and becomes a coroutine: the backend emits the parameter copy-in /
    // copy-out (output / inout pass by pointer) and the completion handshake, so
    // here we only allocate a frame Local per formal (index k == formal position)
    // and lower the body. Every path ends in a void Ret, which the backend turns
    // into the copy-out + caller-resume epilogue.
    void build_task(const SubroutineSymbol& sub, ir::FunctionId id) {
        FnCtx ctx;
        ctx.name_prefix = std::string(sub.name) + ".";
        ctx.is_task     = true;
        FnCtx* saved    = fn_ctx_;
        fn_ctx_         = &ctx;
        std::vector<std::string_view> saved_blocks;
        saved_blocks.swap(named_blocks_);
        ir::Builder          b(module_.pool);
        std::vector<ir::Ref> param_types;
        std::vector<ir::ArgDir> arg_dirs;

        // A class method (LRM 8.11): splice the implicit `this` handle as formal 0,
        // before the real formals, so they (and the return var) keep formal k ==
        // Local k with `this` at Local 0.
        splice_this(sub, param_types, arg_dirs);

        // Allocate the formals' frame Locals first so formal k maps to Local k (the
        // backend's prologue / epilogue rely on this). No Arg copy is emitted: the
        // backend copies parameters in (and output / inout back out) per direction.
        for (const FormalArgumentSymbol* fa : sub.getArguments()) {
            param_types.push_back(lower_type(fa->getType()));
            arg_dirs.push_back(to_arg_dir(fa->direction));
            fn_local(*fa);
        }

        lower_stmt(sub.getBody(), b);

        // Fall-through return: the void Ret drives the backend's copy-out epilogue.
        const ir::SourceLoc el = to_source_loc(sub.location);
        b.ret_void(el);

        module_.set_function(id, ir::Function{ .name        = std::string(sub.name),
                                               .return_type = module_.pool.void_type,
                                               .param_types = std::move(param_types),
                                               .arg_dirs    = std::move(arg_dirs),
                                               .local_types = std::move(ctx.local_types),
                                               .is_task     = true,
                                               .ir          = b.finish() });
        named_blocks_.swap(saved_blocks);
        fn_ctx_ = saved;
    }

    // Lower one `fork` child statement (LRM 9.3.2) into a synthetic argument-less
    // fork-child task: a coroutine spawned at the fork, whose completion decrements
    // the shared join record. Mirrors build_task, but takes a bare Statement (no
    // formals) and runs in the *parent's* scope context (scope_ / prefix_ /
    // time_mul_ unchanged) so the child's references to parent variables resolve to
    // the parent's signals, while the child's own declared locals get per-spawn
    // frame slots (value_lvalue: localmap first, then signal_of). Returns the
    // reserved FunctionId.
    ir::FunctionId build_fork_child(const Statement& body) {
        const ir::FunctionId id = module_.reserve_function();

        FnCtx ctx;
        ctx.name_prefix = "fork.child.";
        ctx.is_task     = true;
        FnCtx* saved    = fn_ctx_;
        fn_ctx_         = &ctx;
        std::vector<std::string_view> saved_blocks;
        saved_blocks.swap(named_blocks_);

        ir::Builder b(module_.pool);
        lower_stmt(body, b);
        const ir::SourceLoc el = loc(body);
        b.ret_void(el);

        module_.set_function(id, ir::Function{ .name          = "fork.child",
                                               .return_type   = module_.pool.void_type,
                                               .param_types   = {},
                                               .arg_dirs      = {},
                                               .local_types   = std::move(ctx.local_types),
                                               .is_task       = true,
                                               .is_fork_child = true,
                                               .ir            = b.finish() });
        named_blocks_.swap(saved_blocks);
        fn_ctx_ = saved;
        return id;
    }

    // Lower one function body into the reserved slot. (Ir has no default ctor and
    // is not assignable, so every field is built locally and the Function is
    // constructed once, at the end.)
    void build_function(const SubroutineSymbol& sub, ir::FunctionId id) {
        const bool    is_void     = sub.returnValVar == nullptr;
        const ir::Ref return_type = is_void ? module_.pool.void_type : lower_type(sub.returnValVar->getType());

        FnCtx            ctx;
        ctx.name_prefix        = std::string(sub.name) + ".";
        FnCtx*           saved = fn_ctx_;
        fn_ctx_                = &ctx;
        std::vector<std::string_view> saved_blocks;
        saved_blocks.swap(named_blocks_);
        ir::Builder      b(module_.pool);
        std::vector<ir::Ref> param_types;
        std::vector<ir::ArgDir> arg_dirs;

        // A class method (LRM 8.11): splice the implicit `this` handle as formal 0.
        splice_this(sub, param_types, arg_dirs);

        // Allocate a frame Local per formal (formal k == Local k), like build_task.
        // The backend copies parameters into these Locals at entry (emit_copyin) and
        // copies output / inout formals back at each return (emit_copyout); a ref
        // formal's Local is redirected to the passed actual pointer. No IR-level Arg
        // copy is emitted for any direction. An aggregate formal is passed by pointer
        // and copied into its Local by the backend's aggregate copy-in.
        for (const FormalArgumentSymbol* fa : sub.getArguments()) {
            if (is_aggregate(fa->getType())) {
                if (fa->direction != slang::ast::ArgumentDirection::In) {
                    // TODO: aggregate output / inout function arg (copy-out).
                    fail("aggregate output / inout function argument is not supported");
                }
                if (!is_trivially_copyable(lower_type(fa->getType()))) {
                    // TODO: by-value copy of a string-containing aggregate (needs a
                    // per-leaf deep copy of the storage, not a memcpy).
                    fail("by-value pass of a string-containing aggregate is not supported");
                }
            }
            param_types.push_back(lower_type(fa->getType()));
            arg_dirs.push_back(to_arg_dir(fa->direction));
            fn_local(*fa);
        }

        if (!is_void) {
            if (is_aggregate(sub.returnValVar->getType()) && !is_trivially_copyable(return_type)) {
                // TODO: by-value return of a string-containing aggregate.
                fail("by-value return of a string-containing aggregate is not supported");
            }
            ctx.has_ret_local = true;
            ctx.ret_type      = return_type;
            ctx.ret_local     = fn_local(*sub.returnValVar);
        }

        lower_stmt(sub.getBody(), b);

        // Fall-through return: yields the function variable (X if never assigned).
        // An aggregate result's Ret operand is its lvalue (the backend copies its
        // storage out through the trailing return out-param), not a loaded value.
        const ir::SourceLoc el = to_source_loc(sub.location);
        if (is_void) {
            b.ret_void(el);
        } else if (is_aggregate(sub.returnValVar->getType())) {
            b.unary(ir::Inst::Tag::Ret, b.local(ctx.ret_local, ctx.ret_type, el), el);
        } else {
            const ir::Index rv = b.local(ctx.ret_local, ctx.ret_type, el);
            b.unary(ir::Inst::Tag::Ret, b.unary(ir::Inst::Tag::Load, rv, el), el);
        }

        module_.set_function(id, ir::Function{ .name        = std::string(sub.name),
                                               .return_type = return_type,
                                               .param_types = std::move(param_types),
                                               .arg_dirs    = std::move(arg_dirs),
                                               .local_types = std::move(ctx.local_types),
                                               .ir          = b.finish() });
        named_blocks_.swap(saved_blocks);
        fn_ctx_ = saved;
    }

    // Lower a class `new` expression (LRM 8.7): allocate + power-on the object body
    // (ClassNew yields the handle), then run the constructor with the fresh handle
    // spliced as `this`. Yields the object handle (a 2-state 64-bit value). A
    // default (implicit) constructor with no initializers has no constructorCall
    // and only the power-on defaults apply.
    ir::Index lower_new_class(const slang::ast::NewClassExpression& nc, ir::Builder& b) {
        const ir::SourceLoc l = loc(nc);
        if (nc.isSuperClass) {
            // TODO: super.new (chained base construction, Phase 2).
            fail("super.new is not supported");
        }
        const auto&     clt       = nc.type->getCanonicalType().as<slang::ast::ClassType>();
        const ir::Ref   class_ref = lower_type(*nc.type);
        const ir::Index handle    = b.class_new(class_ref, l);
        // Non-static property default initializers run at construction, in
        // declaration order, BEFORE the constructor body (LRM 8.8). slang attaches
        // them to the property (not the constructor body), and a class with no
        // explicit constructor has no constructorCall at all, so apply them here.
        emit_class_property_inits(clt, handle, class_ref, b);
        if (const Expression* ctor = nc.constructorCall()) {
            const CallExpression&   cc   = ctor->as<CallExpression>();
            const SubroutineSymbol& sub  = *std::get<0>(cc.subroutine);
            const ir::FunctionId    id   = function_id(sub); // splices `this` as formal 0
            const auto              formals = sub.getArguments();
            const auto              actuals = cc.arguments();
            if (formals.size() != actuals.size()) {
                fail("constructor argument count mismatch");
            }
            std::vector<CopyOutArg> outs;
            std::vector<ir::Index>  args = build_call_args(formals, actuals, outs, b, l);
            args.insert(args.begin(), handle);
            b.call(id, args, l); // the constructor is void
            emit_call_copyouts(outs, b, l);
        }
        return handle;
    }

    // Apply a class's non-static property default initializers to a freshly-new'd
    // object `handle` (LRM 8.8): a store per property that has an initializer, in
    // declaration order. Lowered in the enclosing context (the `new` site), so an
    // initializer that references `this` or another property fails loudly via
    // signal_of — a documented V1 gap (most initializers are constants / params).
    void emit_class_property_inits(const slang::ast::ClassType& clt, ir::Index handle,
                                   ir::Ref class_ref, ir::Builder& b) {
        for (const auto& prop : clt.membersOfType<slang::ast::ClassPropertySymbol>()) {
            const Expression* init = prop.getInitializer();
            if (init == nullptr) {
                continue;
            }
            const ir::SourceLoc il = loc(*init);
            const ir::Index     lv = class_member_lvalue(handle, prop, class_ref, b, il);
            if (prop.getType().getCanonicalType().isString()) {
                b.binary(ir::Inst::Tag::Store, lv, lower_expr(*init, b), il);
            } else {
                b.binary(ir::Inst::Tag::Store, lv,
                         coerce(lower_expr(*init, b), *init->type, lower_type(prop.getType()), b, il), il);
            }
        }
    }

    // Lower a user function call: the actuals (resized to the parameter types)
    // become a `call`. Returns the result value, or nullopt for a void function.
    std::optional<ir::Index> lower_user_call(const CallExpression& call, ir::Builder& b) {
        if (call.subroutine.index() != 0) {
            fail("system call used where a user subroutine was expected");
        }
        const SubroutineSymbol& sub = *std::get<0>(call.subroutine);
        // A class method (LRM 8.11): resolve the `this` object handle, spliced as
        // arg 0 (the callee splices it as formal 0). An explicit receiver
        // (`obj.method()`) is call.thisClass(); a bare in-method call uses `this`.
        const std::optional<ir::Index> this_val = resolve_method_this(call, sub, b);
        if (sub.subroutineKind == SubroutineKind::Task) {
            // A task is a statement, never a value; it blocks the caller until done.
            lower_task_call(call, sub, b, this_val);
            return std::nullopt;
        }
        const ir::FunctionId id = function_id(sub); // validates + lowers (lazily)

        const auto formals = sub.getArguments();
        const auto actuals = call.arguments();
        if (formals.size() != actuals.size()) {
            fail("function argument count mismatch");
        }
        const ir::SourceLoc     l = loc(call);
        std::vector<CopyOutArg> outs;
        std::vector<ir::Index>  args = build_call_args(formals, actuals, outs, b, l);
        if (this_val) {
            args.insert(args.begin(), *this_val);
        }
        const ir::Index         c = b.call(id, args, l);
        // Output / inout formals copy back to their actuals after the call (a
        // function is zero-time, so this is the same instant as the call).
        emit_call_copyouts(outs, b, l);

        // A DPI import has no returnValVar even when non-void, so key the result
        // on the declared return type (void == no value). Uses the slang type, not
        // the lowered Function record, which is not yet set during a recursive
        // call (build_function sets the slot only after lowering its body).
        return sub.getReturnType().isVoid() ? std::nullopt : std::optional<ir::Index>(c);
    }

    // Lower a task enable (LRM 13.3). The backend drives the task coroutine and
    // blocks the caller until it completes. Argument passing (13.5) is shared with
    // function calls: build_call_args prepares each operand and records output /
    // inout copy-backs, emit_call_copyouts applies them after the call.
    void lower_task_call(const CallExpression& call, const SubroutineSymbol& sub, ir::Builder& b,
                         std::optional<ir::Index> this_val = std::nullopt) {
        const ir::FunctionId id      = function_id(sub); // validates + lowers (lazily)
        const auto           formals = sub.getArguments();
        const auto           actuals = call.arguments();
        if (formals.size() != actuals.size()) {
            fail("task argument count mismatch");
        }
        for (const FormalArgumentSymbol* fa : formals) {
            if (contains_struct_ref(lower_type(fa->getType()))) {
                // TODO: aggregate task argument (by-pointer copy-in, plus copy-out
                // for output / inout). The task coroutine ABI does not yet marshal
                // aggregates; only functions do.
                fail("unpacked struct / aggregate task argument is not supported");
            }
        }
        const ir::SourceLoc     l = loc(call);
        std::vector<CopyOutArg> outs;
        std::vector<ir::Index>  args = build_call_args(formals, actuals, outs, b, l);
        if (this_val) {
            args.insert(args.begin(), *this_val);
        }
        b.task_call(id, args, l);
        emit_call_copyouts(outs, b, l);
    }

    // Resolve the `this` object handle for a possible method call (LRM 8.11). An
    // explicit receiver (`obj.method()`) is call.thisClass(); a bare in-method call
    // (`method()`) uses the enclosing method's `this` Local. Returns nullopt for a
    // free subroutine (no thisVar).
    std::optional<ir::Index> resolve_method_this(const CallExpression& call,
                                                 const SubroutineSymbol& sub, ir::Builder& b) {
        if (call.thisClass() != nullptr) {
            return lower_expr(*call.thisClass(), b);
        }
        if (sub.thisVar == nullptr) {
            return std::nullopt;
        }
        if (fn_ctx_ == nullptr || !fn_ctx_->has_this) {
            fail("class method call without an object handle is not supported");
        }
        const ir::SourceLoc l = loc(call);
        return b.unary(ir::Inst::Tag::Load,
                       b.local(fn_ctx_->this_local, fn_ctx_->this_class, l), l);
    }

    // One output / inout argument's copy-back record: the caller temp holding the
    // formal's value, the actual lvalue to write at completion, and the types to
    // coerce across (formal -> actual).
    struct CopyOutArg {
        ir::Index               temp;
        const Expression*       actual;
        ir::Ref                 actual_ty;
        const slang::ast::Type* formal_ty;
    };

    // Build the call-argument operands for a user function / task (LRM 13.5): an
    // input passes a value resized to the formal; a ref passes the actual's lvalue;
    // an output / inout uses a fresh caller temp (copied in from the actual for
    // inout) the backend passes by pointer. Each output / inout is recorded in
    // `outs` for copy-out after the call. Shared by function and task calls — the
    // copy-in / copy-out semantics are identical and delicate to correctness.
    std::vector<ir::Index> build_call_args(std::span<const FormalArgumentSymbol* const> formals,
                                           std::span<const Expression* const> actuals,
                                           std::vector<CopyOutArg>& outs, ir::Builder& b,
                                           ir::SourceLoc l) {
        std::vector<ir::Index> args;
        args.reserve(formals.size());
        for (size_t i = 0; i < formals.size(); ++i) {
            const auto dir = formals[i]->direction;
            // A DPI open-array formal (LRM 35.5.6.2): pass the actual array's lvalue
            // leaf. The backend builds the svOpenArray descriptor from the actual's
            // array type at the call site. lower_type would fail on DPIOpenArrayType,
            // so intercept before it. Input-only (guarded in build_dpi_open_array_arg).
            if (formals[i]->getType().getCanonicalType().as_if<slang::ast::DPIOpenArrayType>()) {
                const Expression* actual = actuals[i];
                if (actual->kind == ExpressionKind::Assignment) {
                    actual = &actual->as<AssignmentExpression>().left();
                }
                args.push_back(lower_lvalue(unwrap_conversion(*actual), b));
                continue;
            }
            const ir::Ref pty = lower_type(formals[i]->getType());
            if (contains_struct_ref(pty)) {
                // An aggregate actual passes by pointer (its lvalue); the callee
                // copies it in for value semantics. Only In reaches here: the
                // callee guards aggregate output / inout, and aggregate task
                // arguments are guarded before build_call_args (lower_task_call).
                if (!is_trivially_copyable(pty)) {
                    fail("by-value pass of a string-containing aggregate is not supported");
                }
                const Expression& src = unwrap_conversion(*actuals[i]);
                if (!is_struct_lvalue_shape(src)) {
                    // TODO: a `'{...}` / call aggregate actual — materialize a temp.
                    fail("aggregate argument must be a variable");
                }
                args.push_back(lower_lvalue(src, b));
                continue;
            }
            // A dynamic-array actual passes one handle lvalue for every direction:
            // the callee deep-copies it in (input / inout), copies its frame handle
            // back out (output / inout), or aliases it (ref). No caller temp — the
            // actual is always a whole handle. slang wraps an output / inout actual
            // in an AssignmentExpression; unwrap to the bound lvalue.
            if (is_handle_ref(pty)) {
                const Expression* actual = actuals[i];
                if (actual->kind == ExpressionKind::Assignment) {
                    actual = &actual->as<AssignmentExpression>().left();
                }
                args.push_back(lower_lvalue(*actual, b));
                continue;
            }
            if (dir == slang::ast::ArgumentDirection::Ref) {
                args.push_back(lower_ref_actual(*actuals[i], b));
                continue;
            }
            if (dir == slang::ast::ArgumentDirection::In) {
                // A string actual passes as-is (no width to resize to); the callee's
                // copy-in gives value semantics.
                args.push_back(is_string_ref(pty)
                                 ? lower_expr(*actuals[i], b)
                                 : coerce(lower_expr(*actuals[i], b), *actuals[i]->type, pty, b, l));
                continue;
            }
            // slang models an output / inout actual as an AssignmentExpression whose
            // left side is the caller lvalue; unwrap it to the bound expression.
            const Expression* actual = actuals[i];
            if (actual->kind == ExpressionKind::Assignment) {
                actual = &actual->as<AssignmentExpression>().left();
            }
            const ir::Index temp = make_arg_temp(pty, b, l);
            if (dir == slang::ast::ArgumentDirection::InOut) {
                // Copy-in: temp = (parameter type) actual.
                b.binary(ir::Inst::Tag::Store, temp, coerce(lower_expr(*actual, b), *actual->type, pty, b, l), l);
            }
            args.push_back(temp);
            outs.push_back(CopyOutArg{ temp, actual, lower_type(*actual->type), &formals[i]->getType() });
        }
        return args;
    }

    // Copy each output / inout temp back to its actual after the call: actual =
    // (actual type) temp. Reuses the normal store path so width conversion and
    // complex actual lvalues (bit / part selects) work.
    void emit_call_copyouts(const std::vector<CopyOutArg>& outs, ir::Builder& b, ir::SourceLoc l) {
        for (const CopyOutArg& oc : outs) {
            const ir::Index dst = lower_lvalue(*oc.actual, b);
            const ir::Index tv  = b.unary(ir::Inst::Tag::Load, oc.temp, l);
            b.binary(ir::Inst::Tag::Store, dst, coerce(tv, *oc.formal_ty, oc.actual_ty, b, l), l);
        }
    }

    // A legal ref actual (LRM 13.5.2) Clotho supports: a whole variable, or a whole
    // element of an unpacked array (`arr[i]`, possibly nested). Each is one complete
    // `{ i(2W), iW }` slot, so the backend passes its slot pointer. slang already
    // rejects a packed bit/part-select as a ref actual ("only variables, class
    // properties, and members of unpacked structs and arrays are allowed"); an
    // unpacked-struct member needs struct support, still unimplemented.
    bool is_whole_ref_actual(const Expression& a) {
        switch (a.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
                return true;
            case ExpressionKind::ElementSelect: {
                // Only an unpacked-array element is a whole slot; a select of a
                // packed value is a bit/part-select (a partial ref, unsupported).
                const auto& es = a.as<ElementSelectExpression>();
                return is_unpacked_array(es.value()) && is_whole_ref_actual(es.value());
            }
            default:
                return false;
        }
    }

    // Lower a ref actual (LRM 13.5.2) to its lvalue leaf: the backend passes one
    // pointer to the actual's slot (a scalar's slot, or one array-element slot) and
    // the callee aliases it live.
    ir::Index lower_ref_actual(const Expression& actual, ir::Builder& b) {
        const Expression* a = &actual;
        // slang may wrap a ref actual in an AssignmentExpression (as for out/inout);
        // unwrap to the bound lvalue.
        if (a->kind == ExpressionKind::Assignment) {
            a = &a->as<AssignmentExpression>().left();
        }
        if (!is_whole_ref_actual(*a)) {
            // TODO: ref actual that is a packed bit/part-select or unpacked-struct
            // member (needs sub-slot offset plumbing / struct support).
            fail("ref argument actual must be a whole variable or unpacked-array element");
        }
        return lower_lvalue(*a, b);
    }

    // A fresh, anonymous storage cell for an output / inout call argument, returned
    // as an lvalue leaf. Inside a function / task caller it is a per-call frame
    // Local; in a process it is a module Signal (a process has no frame). The same
    // lvalue Index is reused for copy-in, the call operand, and copy-out.
    ir::Index make_arg_temp(ir::Ref type, ir::Builder& b, ir::SourceLoc l) {
        if (fn_ctx_ != nullptr) {
            const auto idx = static_cast<uint32_t>(fn_ctx_->local_types.size());
            fn_ctx_->local_types.push_back(type);
            return b.local(idx, type, l);
        }
        ir::SignalInfo info;
        info.name = prefix_ + "__taskarg" + std::to_string(task_temp_seq_++);
        info.type = type;
        info.dir  = ir::SignalInfo::Direction::Internal;
        return b.signal(register_signal(std::move(info)), l);
    }

    ir::Module& module_;
    // Scope tree of the top instance this Lowerer walks, handed to the module in
    // lower_top. scope_stack_ tracks the path from the top to the instance
    // currently being lowered; register_signal records into its back().
    ir::InstanceScope               top_scope_;
    std::vector<ir::InstanceScope*> scope_stack_;

    // Scope id of the instance currently being lowered, for Process::scope;
    // no_scope outside any instance (the package/$unit Lowerer).
    uint32_t cur_scope_id() const {
        return scope_stack_.empty() ? ir::Process::no_scope : scope_stack_.back()->id;
    }
    // Scope of the instance body currently being lowered; used to fold constant
    // expressions (parameter references and arithmetic over them).
    const slang::ast::Scope* scope_ = nullptr;
    // Maps an elaborated value symbol to its module Signal handle.
    ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal> sigmap_;
    // Continuous-driver coverage per net signal: the closed regions already
    // driven, checked for overlap by record_net_driver_region (NetRegion defined
    // in the net-driver-tracking section).
    ankerl::unordered_dense::map<uint32_t, std::vector<NetRegion>> net_driven_;
    // Which lowering pass is running; the driver-producing members lower in both
    // Classify (record driver regions) and Emit (store / route), and branch on
    // this. Meaningful only inside lower_members and the driver functions.
    Pass cur_pass_ = Pass::Emit;
    // Net signals Classify found to have >= 2 overlapping continuous drivers, so
    // Emit routes every driver to a slot and emit_wire_resolvers folds them.
    ankerl::unordered_dense::set<uint32_t> resolved_nets_;
    // Per resolved net: its per-driver slot signals (fold order), the scope id
    // the resolver process is attributed to, and a representative source loc.
    ankerl::unordered_dense::map<uint32_t, std::vector<ir::Signal>> resolved_slots_;
    ankerl::unordered_dense::map<uint32_t, uint32_t>                resolved_scope_;
    ankerl::unordered_dense::map<uint32_t, ir::SourceLoc>           resolved_loc_;
    // Per resolved net: which reducer folds its slots (WireRes / WandRes /
    // WorRes), from the net kind (net_resolve_tag).
    ankerl::unordered_dense::map<uint32_t, ir::Inst::Tag>           net_resolve_op_;
    // tri0 / tri1 nets: the resistive pull applied after the fold, and the home
    // scope / loc for the resolver (so even a driverless net emits one).
    struct PullInfo {
        int          value;
        uint32_t     scope;
        ir::SourceLoc loc;
    };
    ankerl::unordered_dense::map<uint32_t, PullInfo> net_pull_;
    // Hierarchical name prefix of the instance currently being lowered (e.g.
    // "u_alu.u_sub."); empty for the top. Prepended to every signal name created
    // while this instance's body is processed so flattened signals stay unique.
    std::string prefix_;
    // True when flattened paths omit the single top instance's name (the
    // single-top case); %m and $printtimescale re-prepend module_.name.
    bool top_elided_ = false;
    // Names of the named begin blocks enclosing the statement currently being
    // lowered, outermost first; they extend the %m hierarchical name (LRM
    // §21.2.1.5). Saved/cleared around subroutine body lowering: a body is
    // built at its first call site, whose block nesting is not the body's.
    std::vector<std::string_view> named_blocks_;
    // Femtoseconds per wheel tick (design-wide smallest time unit); 0 == unitless.
    uint64_t tick_fs_ = 0;
    // Time-unit multiplier of the instance currently being lowered: a delay is
    // scaled up by this into ticks; $time is scaled (rounded) down by it. Always
    // 1 when unitless. Saved / restored around child-instance recursion.
    uint64_t time_mul_ = 1;
    // Monotonic counter naming hidden compiler temporaries (e.g. a repeat loop's
    // counter / bound) so their flattened signal names stay unique.
    uint32_t temp_id_ = 0;
    // Subroutine -> its lowered FunctionId, so each function is lowered once and
    // shared across call sites (and a recursive call resolves its own id).
    // Package/$unit-scope subroutines never populate this map directly on a
    // per-top Lowerer — function_id delegates those to globals_ instead, so a
    // package function called from multiple top instances still gets exactly
    // one FunctionId and one copy of any static local's storage.
    ankerl::unordered_dense::map<const SubroutineSymbol*, ir::FunctionId> fnmap_;
    // Canonical slang struct/union type -> its lowered ir::Ref, so a type used by
    // many variables (e.g. a bus of the same struct) interns its field table
    // once. The pool's own structural dedup can't do this: two independently
    // built field lists get different `fields_id`s and would never compare
    // equal, defeating InternPool::put's content-based dedup.
    ankerl::unordered_dense::map<const slang::ast::Type*, ir::Ref> struct_type_cache_;
    // Canonical slang ClassType -> its lowered Kind::Class ir::Ref. Separate from
    // struct_type_cache_ because a class is nominal (not deduped) and interned
    // before its properties, breaking self-reference cycles (see lower_class_type).
    ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::Ref> class_type_cache_;
    // Class property symbol -> its declaration-order field index in the owning
    // class's property table (used to lower a member access to a MemberSelect).
    ankerl::unordered_dense::map<const slang::ast::Symbol*, uint32_t> class_prop_index_;
    // The shared Lowerer that owns package/$unit-scope subroutine bodies (see
    // set_globals / function_id). Null on the shared Lowerer itself and on any
    // Lowerer built without one (e.g. tests that lower a single module with no
    // packages in play).
    Lowerer* globals_ = nullptr;
    // The function currently being lowered, or null when lowering a process. Drives
    // local-vs-signal name resolution and `return` handling.
    FnCtx* fn_ctx_ = nullptr;
    // The LHS lvalue of the compound assignment currently being lowered, read by
    // the LValueReferenceExpression slang embeds in its rewritten RHS. Null
    // outside a compound assignment.
    std::optional<ir::Index> compound_lv_;
    // Monotonic counter naming anonymous output / inout task-argument temps created
    // in a process body (where temps are module signals, not frame locals).
    uint32_t task_temp_seq_ = 0;
    // Accumulates static variable declaration initializers (LRM 6.8) across the
    // whole instance hierarchy into one run-once pre-init process, finalized at the
    // end of the top instance. Lazily engaged: absent when no initializer exists.
    std::optional<ir::Builder> preinit_;
};

LowerResult lower_compilation(slang::ast::Compilation& comp, Timings* timings) {
    LowerResult result;

    // Front end: getAllDiagnostics forces full elaboration; getRoot resolves the
    // design hierarchy. Both are slang work.
    const auto                  t_fe      = std::chrono::steady_clock::now();
    const slang::Diagnostics&   diags     = comp.getAllDiagnostics();
    const slang::SourceManager* sm        = comp.getSourceManager();
    bool                        has_error = false;
    if (sm != nullptr) {
        slang::DiagnosticEngine engine(*sm);
        auto                    client = std::make_shared<slang::TextDiagnosticClient>();
        engine.addClient(client);
        // LRM 9.2.2.2 / 9.2.2.3 recommend tools warn when a comb / latch
        // procedure's behavior does not match its declared kind. slang keeps
        // both diagnostics in its off-by-default "extra" group, so enable them.
        engine.setSeverity(slang::diag::InferredLatch, slang::DiagnosticSeverity::Warning);
        engine.setSeverity(slang::diag::InferredComb, slang::DiagnosticSeverity::Warning);
        // slang rejects a timing-control-free `always` as a hard error, but the
        // LRM (9.2.2.1) only describes it as a simulation deadlock: clotho
        // deliberately lowers it faithfully (see lower_procedural_block), so
        // downgrade to a warning.
        engine.setSeverity(slang::diag::AlwaysWithoutTimingControl,
                           slang::DiagnosticSeverity::Warning);
        // LRM 6.5 / 10.3.2 both use normative "shall be an error" for a
        // variable driven by more than one continuous assignment, or by a mix
        // of continuous and procedural assignments -- slang defaults these to
        // warnings. Clotho's signal model has one driver per variable slot
        // with no multi-driver resolution, so silently keeping the default
        // severity would mean one of the conflicting drivers is silently
        // dropped instead of the design being rejected.
        engine.setSeverity(slang::diag::MixedVarAssigns, slang::DiagnosticSeverity::Error);
        engine.setSeverity(slang::diag::MultipleContAssigns, slang::DiagnosticSeverity::Error);
        // A name redeclared in the same scope: slang also defaults this to a
        // warning and silently keeps one declaration. Clotho's symbol table
        // has no "last one wins" rule, so promote to an error rather than
        // silently favoring whichever declaration slang's lookup resolves to.
        engine.setSeverity(slang::diag::Redefinition, slang::DiagnosticSeverity::Error);
        engine.setSeverity(slang::diag::RedefinitionDifferentType, slang::DiagnosticSeverity::Error);
        for (const slang::Diagnostic& d : diags) {
            engine.issue(d);
        }
        // Post-elaboration analysis (dataflow): inferred-latch/comb checks,
        // blocking-task-in-comb errors, and multi-driver checks (9.2.2.2). Only
        // run on an error-free design — analysis assumes a valid AST. Default
        // AnalysisOptions deliberately leave the lint-style CheckUnused /
        // CheckShadow passes off and match strict LRM driver rules.
        if (engine.getNumErrors() == 0) {
            comp.freeze();
            slang::analysis::AnalysisManager analysis;
            analysis.analyze(comp);
            engine.issue(analysis.getDiagnostics());
            comp.unfreeze();
        }
        result.diagnostics = client->getString();
        has_error          = engine.getNumErrors() > 0;
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
    if (!single && !comp.getDPIExports().empty()) {
        // A context export runs in a specific instance's scope (svScope, LRM
        // 35.5.3); with several tops there is no single design-state root to bind
        // the thunk to. Deferred with the rest of svScope.
        result.diagnostics += "error: DPI export in a multi-top design is not supported\n";
        return result;
    }
    try {
        Lowerer globals(*module, tick_fs);
        globals.lower_globals(comp, root);

        for (const InstanceSymbol* top : root.topInstances) {
            const std::string prefix = single ? std::string() : std::string(top->name) + ".";
            Lowerer lw(*module, tick_fs);
            lw.seed_signals(globals.sigmap());
            lw.set_globals(&globals);
            lw.lower_top(*top, prefix);
            // Exported functions (single-top only, guarded above) resolve in this
            // top's scope — module-scope functions via `lw`, $unit/package via the
            // shared `globals` delegation in function_id.
            if (single) {
                lw.collect_dpi_exports(comp);
            }
        }
    } catch (const LowerError& e) {
        result.diagnostics += "error: " + e.message + "\n";
        return result;
    }
    if (timings != nullptr) {
        timings->analysis_ms += ms_since(t_an);
    }

    result.module = std::move(module);
    result.ok     = true;

    return result;
}

} // namespace

LowerResult lower_files(std::span<const std::string_view> paths, Timings* timings) {
    const auto t_parse = std::chrono::steady_clock::now();

    // One SourceManager shared across every tree so the Compilation's
    // diagnostics resolve file/line locations for all files. It
    // outlives every tree and the compilation use below.
    slang::SourceManager    sm;
    slang::ast::Compilation comp;
    for (const std::string_view path : paths) {
        auto tree = slang::syntax::SyntaxTree::fromFile(path, sm);
        if (!tree) {
            LowerResult result;
            result.diagnostics = "error: cannot read '" + std::string(path) +
                                 "': " + std::string(tree.error().second) + "\n";
            return result;
        }
        comp.addSyntaxTree(*tree);
    }
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_parse);
    }

    return lower_compilation(comp, timings);
}

LowerResult lower_file(std::string_view path, Timings* timings) {
    return lower_files({&path, 1}, timings);
}

LowerResult lower_sources(std::span<const std::string_view> args, Timings* timings) {
    const auto t_parse = std::chrono::steady_clock::now();

    // slang's Driver owns arg parsing, source loading (incl. `-f` command files,
    // `-y`/`-v` libraries, globbing), the SourceManager, and the option Bag; we
    // consume the Compilation it produces and keep Clotho's own diagnostic
    // rendering in lower_compilation. The Driver must outlive the
    // Compilation, so it lives for the whole call; the lowered ir::Module holds
    // no slang pointers, so it may outlive both.
    slang::driver::Driver driver;
    driver.addStandardArgs();

    // Driver's parseCommandLine treats argv[0] as the program name and needs
    // null-terminated C strings; copy the views and prepend a placeholder.
    std::vector<std::string> owned(args.begin(), args.end());
    std::vector<const char*> argv;
    argv.reserve(owned.size() + 1);
    argv.push_back("clotho");
    for (const std::string& a : owned) {
        argv.push_back(a.c_str());
    }

    LowerResult result;
    if (!driver.parseCommandLine(static_cast<int>(argv.size()), argv.data()) ||
        !driver.processOptions()) {
        // Driver prints the specific CLI / missing-file errors to stderr itself.
        result.diagnostics = "error: command-line or source-loading error\n";
        return result;
    }

    // Merge every source file into one compilation unit so `` `define `` macros
    // and `$unit` declarations are shared across files (LRM 3.12). Set before
    // parseAllSources, which is where the SourceLoader consumes it.
    driver.options.singleUnit = true;
    if (!driver.parseAllSources()) {
        result.diagnostics = "error: failed to parse one or more source files\n";
        return result;
    }

    std::unique_ptr<slang::ast::Compilation> comp = driver.createCompilation();
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_parse);
    }

    return lower_compilation(*comp, timings);
}

LowerResult lower_text(std::string_view source, Timings* timings) {
    const auto t_parse = std::chrono::steady_clock::now();
    slang::ast::Compilation comp;
    comp.addSyntaxTree(slang::syntax::SyntaxTree::fromText(source));
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_parse);
    }

    return lower_compilation(comp, timings);
}

} // namespace clotho
