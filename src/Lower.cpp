#include "Lower.h"

#include <algorithm>
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
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/numeric/Time.h>
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
        const auto it = sigmap.find(&sym);
        if (it == sigmap.end()) {
            return;
        }
        if (seen.insert(static_cast<uint32_t>(it->second)).second) {
            reads.push_back(it->second);
        }
    }

    void mark_written(const ValueSymbol& sym) {
        const auto it = sigmap.find(&sym);
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
                mark_written(lhs.as<NamedValueExpression>().symbol);
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

    // Flatten one elaborated instance into the module: register its signals under
    // `prefix`, lower its processes, then recurse into child instances and wire
    // their ports. `prefix` is empty for the top instance and "<inst>." per level
    // below it. Each instance has its own slang body with distinct ValueSymbols,
    // so sigmap_ (keyed by symbol) disambiguates repeated instantiations for free.
    void lower_instance(const InstanceSymbol& inst, std::string_view prefix) {
        if (prefix.empty()) {
            module_.name = std::string(inst.name);
        }
        const std::string         self_prefix(prefix);
        const InstanceBodySymbol& body = inst.body;

        // Open this instance's hierarchy scope: signals registered while it is
        // on top of the stack (ports, nets/vars, generate-block signals, and
        // compiler temps) are recorded as declared here. A parent's children
        // vector only grows after this child pops, so the stacked pointers stay
        // stable across the recursion.
        ir::InstanceScope node;
        node.name     = std::string(inst.name);
        node.def_name = std::string(inst.getDefinition().name);
        ir::InstanceScope* self = nullptr;
        if (scope_stack_.empty()) {
            top_scope_ = std::move(node);
            self       = &top_scope_;
        } else {
            scope_stack_.back()->children.push_back(std::move(node));
            self = &scope_stack_.back()->children.back();
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
        collect_signals(body);
        lower_members(body, self_prefix, self_mul);

        scope_stack_.pop_back();
    }

    // Lower one top instance and its whole hierarchy, then emit the accumulated
    // declaration initializers as this Lowerer's pre-init process. finalize_preinit
    // runs here, per top, rather than inside lower_instance: a multi-top design
    // gives every top a "<name>." prefix, so gating on an empty prefix would drop
    // all initializers (each top gets its own Lowerer, so this covers each one).
    void lower_top(const InstanceSymbol& top, std::string_view prefix) {
        top_elided_ = prefix.empty();
        lower_instance(top, prefix);
        finalize_preinit();
        module_.add_top(std::move(top_scope_));
    }

private:
    // Lower the non-signal members of a scope (an instance body or a generate
    // block) into processes: continuous assigns, procedural blocks, child
    // instances, and nested generate constructs. `self_prefix` is the
    // hierarchical name prefix of `scope`; `self_mul` is the enclosing instance's
    // time multiplier, restored after a child instance recurses. Signals are
    // already registered by collect_signals, which walked the same generate tree.
    void lower_members(const Scope& scope, const std::string& self_prefix, uint64_t self_mul) {
        // prefix_ / scope_ hold this level's values on entry to each iteration:
        // set by the caller and restored after any case that descends and changes
        // them, so signal-creating process lowering always sees this level.
        for (const Symbol& m : scope.members()) {
            switch (m.kind) {
                case SymbolKind::ContinuousAssign:
                    lower_continuous_assign(m.as<ContinuousAssignSymbol>());
                    break;
                case SymbolKind::ProceduralBlock:
                    lower_procedural_block(m.as<ProceduralBlockSymbol>());
                    break;
                case SymbolKind::Variable:
                    // Storage was registered by collect_signals; here only a
                    // declaration initializer (LRM 6.8) needs lowering, into the
                    // pre-init process.
                    lower_var_initializer(m.as<VariableSymbol>());
                    break;
                case SymbolKind::Net:
                    // Storage was registered by collect_signals; a net declaration
                    // assignment (`wire w = expr`) is a continuous assignment.
                    lower_net_initializer(m.as<NetSymbol>());
                    break;
                case SymbolKind::Instance: {
                    const InstanceSymbol& child = m.as<InstanceSymbol>();
                    // Recurse first (collects child signals), then wire ports: a
                    // connection's parent-side expression resolves against signals
                    // already registered in this (parent) body. Recursion leaves
                    // prefix_ at the child level, so restore it before wiring.
                    lower_instance(child, self_prefix + std::string(child.name) + ".");
                    prefix_   = self_prefix;
                    scope_    = &scope;
                    time_mul_ = self_mul;
                    wire_ports(child);
                    break;
                }
                case SymbolKind::GenerateBlock: {
                    // A conditional/bare generate block elaborated to a single
                    // scope. An unmatched if/case branch is uninstantiated and
                    // contributes nothing (its body is not part of the design).
                    const auto& gb = m.as<GenerateBlockSymbol>();
                    if (!gb.isUninstantiated) {
                        lower_generate_block(gb, self_prefix + gb.getExternalName() + ".", self_mul);
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
                                             self_mul);
                    }
                    prefix_ = self_prefix;
                    scope_  = &scope;
                    break;
                }
                case SymbolKind::PrimitiveInstance:
                    lower_gate(m.as<PrimitiveInstanceSymbol>());
                    break;
                case SymbolKind::InstanceArray: {
                    // An array of instances — gate primitives (LRM 28.3.6) or
                    // modules/interfaces (LRM 23.3.6). Slang has already
                    // elaborated one leaf symbol per element with per-instance
                    // bit-sliced connections, so this only needs to walk to
                    // the leaves and lower each like its scalar counterpart.
                    lower_instance_array(m.as<InstanceArraySymbol>(), self_prefix, scope, self_mul);
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
                case SymbolKind::Parameter:
                case SymbolKind::TypeParameter:
                case SymbolKind::TypeAlias:
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
                              uint64_t self_mul) {
        prefix_ = block_prefix;
        scope_  = &block;
        lower_members(block, block_prefix, self_mul);
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
                              const Scope& parent_scope, uint64_t self_mul) {
        for (const Symbol* elem : arr.elements) {
            switch (elem->kind) {
                case SymbolKind::InstanceArray:
                    lower_instance_array(elem->as<InstanceArraySymbol>(), self_prefix, parent_scope,
                                        self_mul);
                    break;
                case SymbolKind::PrimitiveInstance:
                    lower_gate(elem->as<PrimitiveInstanceSymbol>());
                    break;
                case SymbolKind::Instance: {
                    const InstanceSymbol& child = elem->as<InstanceSymbol>();
                    lower_instance(child, self_prefix + instance_array_element_name(child) + ".");
                    prefix_   = self_prefix;
                    scope_    = &parent_scope;
                    time_mul_ = self_mul;
                    wire_ports(child);
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
            ir::Type t;
            t.kind    = ir::Type::Kind::UnpackedArray;
            t.element = lower_type(arr->elementType);
            t.left    = static_cast<uint32_t>(r.left);
            t.right   = static_cast<uint32_t>(r.right);

            return module_.pool.put(ir::Item{ t });
        }
        if (ct.isUnpackedArray()) {
            // TODO: dynamic arrays, queues, associative arrays (variable-size,
            // need heap-backed runtime storage).
            fail("dynamic / queue / associative arrays are not supported");
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
        if (!ct.isIntegral()) {
            // TODO: string / chandle / event / class types.
            fail("unsupported non-integral type");
        }

        return make_int_type(ct.getBitWidth(), ct.isFourState(), ct.isSigned());
    }

    // True if an expression's type is a fixed-size unpacked array (a memory).
    static bool is_unpacked_array(const slang::ast::Expression& e) {
        return e.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() != nullptr;
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
                        // Only single-driver, z-defaulting net kinds are modeled.
                        // uwire (LRM 6.6.7) is complete, not a partial stub: its
                        // single-driver mandate is enforced by slang's analysis
                        // pass (MultipleUWireDrivers), which runs before lowering.
                        // TODO: wand/wor/tri0/tri1/trireg/supply0/supply1 carry
                        // resolution, pull, or charge semantics (and different
                        // undriven defaults) clotho does not implement.
                        switch (net.netType.netKind) {
                            case NetType::Wire:
                            case NetType::Tri:
                            case NetType::UWire:
                                break;
                            default:
                                fail("unsupported net type (only wire/tri/uwire are modeled)");
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
                    info.name = prefix + std::string(vs.name);
                    info.type = lower_type(vs.getType());
                    info.dir  = ir::SignalInfo::Direction::Internal;
                    if (port_dir != nullptr) {
                        const auto found = port_dir->find(&m);
                        if (found != port_dir->end()) {
                            info.dir = found->second;
                        }
                    }
                    if (info.dir != ir::SignalInfo::Direction::Internal && is_string_ref(info.type)) {
                        // TODO: string module ports (needs string-aware port
                        // copies and change propagation).
                        fail("string module ports are not supported");
                    }

                    sigmap_[&vs] = register_signal(std::move(info));
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
    // scope tree covers the signal table exactly.
    ir::Signal register_signal(ir::SignalInfo info) {
        const ir::Signal sig = module_.add_signal(std::move(info));
        scope_stack_.back()->signals.push_back(sig);

        return sig;
    }

    ir::Signal signal_of(const ValueSymbol& sym) {
        const auto it = sigmap_.find(&sym);
        if (it == sigmap_.end()) {
            fail("reference to unknown signal '" + std::string(sym.name) + "'");
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
    // Clotho has no net resolution: every continuous driver is an independent
    // process and the last write wins, instead of resolving conflicting values
    // to x per LRM 6.6.1 Table 6-2. Until resolution exists, a net whose bits
    // gain a second continuous/structural driver (continuous assign, output
    // port connection, input port copy, net declaration assignment) must fail
    // loudly. Procedural writes are not continuous drivers and are not tracked;
    // non-overlapping constant selects (assign w[0]=..., assign w[1]=...) are
    // legal single-driver-per-bit and stay accepted.
    // TODO: multiple-driver net resolution (LRM 6.6.1); then delete this guard.

    // Record that bits [lo, hi] of a net gained a continuous driver; fail on
    // overlap with a previously recorded driver. Non-net symbols are ignored
    // (slang already rejects multiple continuous drivers on variables).
    void record_net_driver_range(const ValueSymbol& sym, uint32_t lo, uint32_t hi) {
        if (sym.kind != SymbolKind::Net) {
            return;
        }
        auto& ranges = net_driven_[static_cast<uint32_t>(signal_of(sym))];
        for (const auto& [plo, phi] : ranges) {
            if (lo <= phi && plo <= hi) {
                fail("multiple drivers on net '" + std::string(sym.name) +
                     "' are not supported (no resolution)");
            }
        }
        ranges.emplace_back(lo, hi);
    }

    // The root value symbol of an lvalue select chain, or null for shapes with
    // no single named root.
    static const ValueSymbol* lvalue_root(const Expression& e) {
        switch (e.kind) {
            case ExpressionKind::NamedValue:
                return &e.as<NamedValueExpression>().symbol;
            case ExpressionKind::ElementSelect:
                return lvalue_root(e.as<ElementSelectExpression>().value());
            case ExpressionKind::RangeSelect:
                return lvalue_root(e.as<RangeSelectExpression>().value());
            case ExpressionKind::Conversion:
                return lvalue_root(e.as<ConversionExpression>().operand());
            default:
                return nullptr;
        }
    }

    // Record the net-driver coverage of one continuous-assignment lvalue. A
    // constant bit/part-select directly on the net drives an exact bit range;
    // any dynamic or nested select conservatively drives the whole net
    // ([0, UINT32_MAX], which overlaps every other driver).
    void record_net_driver(const Expression& lhs) {
        switch (lhs.kind) {
            case ExpressionKind::NamedValue:
                record_net_driver_range(lhs.as<NamedValueExpression>().symbol, 0, UINT32_MAX);
                break;
            case ExpressionKind::Concatenation:
                for (const Expression* op : lhs.as<ConcatenationExpression>().operands()) {
                    record_net_driver(*op);
                }
                break;
            case ExpressionKind::Conversion:
                record_net_driver(lhs.as<ConversionExpression>().operand());
                break;
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es   = lhs.as<ElementSelectExpression>();
                const ValueSymbol*             root = lvalue_root(es.value());
                if (root == nullptr) {
                    break;
                }
                if (es.value().kind == ExpressionKind::NamedValue &&
                    es.value().type->isIntegral() && es.type->getBitWidth() == 1) {
                    if (const std::optional<int32_t> c = try_const_int(es.selector())) {
                        const auto bit =
                          static_cast<uint32_t>(packed_range_of(es.value()).translateIndex(*c));
                        record_net_driver_range(*root, bit, bit);
                        break;
                    }
                }
                record_net_driver_range(*root, 0, UINT32_MAX);
                break;
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs   = lhs.as<RangeSelectExpression>();
                const ValueSymbol*           root = lvalue_root(rs.value());
                if (root == nullptr) {
                    break;
                }
                if (rs.value().kind == ExpressionKind::NamedValue &&
                    rs.value().type->isIntegral() &&
                    rs.getSelectionKind() == RangeSelectionKind::Simple) {
                    const std::optional<int32_t> left  = try_const_int(rs.left());
                    const std::optional<int32_t> right = try_const_int(rs.right());
                    if (left && right) {
                        const slang::ConstantRange range = packed_range_of(rs.value());
                        const auto a = static_cast<uint32_t>(range.translateIndex(*left));
                        const auto b = static_cast<uint32_t>(range.translateIndex(*right));
                        record_net_driver_range(*root, std::min(a, b), std::max(a, b));
                        break;
                    }
                }
                // A dynamic (or indexed +:/-:) select: conservatively the whole net.
                record_net_driver_range(*root, 0, UINT32_MAX);
                break;
            }
            default:
                // Other lvalue shapes are rejected by lower_lvalue.
                break;
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
        record_net_driver(e.as<AssignmentExpression>().left());

        ir::Builder b(module_.pool);
        lower_assign(e.as<AssignmentExpression>(), b);

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
                                         .ir          = b.finish() });
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
        record_net_driver_range(ns, 0, UINT32_MAX);
        ir::Builder         b(module_.pool);
        const ir::SourceLoc l = loc(*init);
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
                                         .ir          = b.finish() });

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
            if (pc->port.kind != SymbolKind::Port) {
                // TODO: interface / modport ports — need virtual-interface plumbing.
                fail("interface/modport port connections are not supported");
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
                // The connection is a continuous driver of the parent-side net.
                record_net_driver(a.left());
                const ir::Ref   ty  = lower_type(*a.left().type);
                const ir::Index val = b.unary(ir::Inst::Tag::Load, b.signal(child_sig, l), l);
                b.binary(ir::Inst::Tag::Store, lower_lvalue(a.left(), b),
                         coerce(val, *a.right().type, ty, b, l), l);
                sens.push_back(ir::WaitEvent{ .signal = child_sig, .edge = ir::Edge::Any });
            } else {
                // Input: drive the child net from the source rvalue, resized to the
                // port width; wakes on the source's reads. The copy is a continuous
                // driver of the whole child-side net.
                record_net_driver_range(p.internalSymbol->as<ValueSymbol>(), 0, UINT32_MAX);
                const ir::Ref ty = lower_type(p.internalSymbol->as<ValueSymbol>().getType());
                b.binary(ir::Inst::Tag::Store, b.signal(child_sig, l),
                         coerce(lower_expr(*expr, b), *expr->type, ty, b, l), l);
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
                                         .ir          = std::move(body_ir) });
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
        if (e.kind == ExpressionKind::NamedValue) {
            return signal_of(e.as<NamedValueExpression>().symbol);
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
                // TODO: fork..join / join_any / join_none (LRM 9.3.2) — needs
                // child process spawning and join synchronization in the
                // scheduler. Lowering the body sequentially would silently
                // serialize concurrent branches.
                if (blk.blockKind != StatementBlockKind::Sequential) {
                    fail("fork..join blocks are not supported");
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
            case StatementKind::Break:
                b.break_(loc(stmt));
                break;
            case StatementKind::Continue:
                b.continue_(loc(stmt));
                break;
            case StatementKind::Return:
                lower_return(stmt.as<ReturnStatement>(), b);
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
            b.binary(ir::Inst::Tag::Store, value_lvalue(vs, b, l), lower_expr(*init, b), l);
        }
    }

    // Apply a module-level (static) variable's declaration initializer (LRM 6.8)
    // as a store in the run-once pre-init process. The store services no wait queue
    // (the backend suppresses servicing for a PreInit process), so initialization
    // generates no events and every Initial / Always process starts from the
    // settled value, before any of them run. A variable with no initializer keeps
    // its power-on default (X / 0), set by the backend's init_state.
    void lower_var_initializer(const VariableSymbol& vs) {
        const Expression* init = vs.getInitializer();
        if (init == nullptr) {
            return;
        }
        if (!preinit_) {
            preinit_.emplace(module_.pool);
        }
        ir::Builder&        b = *preinit_;
        const ir::SourceLoc l = loc(*init);
        b.binary(ir::Inst::Tag::Store, value_lvalue(vs, b, l), lower_expr(*init, b), l);
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
            if (!is_string_expr(f.arrayRef)) {
                // TODO: dynamic array / queue / associative array foreach
                // dimension -- needs a runtime-sized bound, not modeled
                // (dynamic/queue/associative arrays fail at Lower.cpp:665).
                fail("foreach over a dynamically-sized array dimension is not supported");
            }
            lower_foreach_string_dim(f, dim, ty, iter_lv, b, l);
            return;
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

    // A `string` foreach dimension (LRM 12.7.3: "a string ... is treated as a
    // dynamic array of bytes indexed from 0 to N-1"). The bound is the
    // runtime length, snapshotted once on entry like a repeat-loop's count --
    // resizing the string mid-loop is undefined behavior per the LRM (same
    // clause), so one read is enough. 0-based, matching StrGetc addressing.
    void lower_foreach_string_dim(const ForeachLoopStatement& f, size_t dim, ir::Ref ty,
                                   ir::Index iter_lv, ir::Builder& b, ir::SourceLoc l) {
        const ir::Signal bound = declare_temp(ty, "foreach_bound");
        const ir::Index  len   = b.unary(ir::Inst::Tag::StrLen, lower_expr(f.arrayRef, b), l);
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

    // The width/state/signedness all case_expressions are compared at: the
    // longest of the selector and every item label, four-state if any operand is,
    // signed only if all are (IEEE 1800-2023 12.5).
    ir::Ref common_case_type(const CaseStatement& c) {
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

        return make_int_type(width, four, signd);
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
    // Returns keep[k][j] = whether item k's label j survives.
    std::vector<std::vector<char>> case_dedup(const CaseStatement& c) {
        std::vector<std::vector<char>> keep(c.items.size());
        for (size_t k = 0; k < c.items.size(); k++) {
            keep[k].assign(c.items[k].expressions.size(), 1);
        }
        if (c.check != slang::ast::UniquePriorityCheck::None) {
            return keep;
        }

        // Common compare width: longest of the selector and all labels (12.5).
        uint32_t width = c.expr.type->getBitWidth();
        for (const CaseStatement::ItemGroup& item : c.items) {
            for (const Expression* e : item.expressions) {
                width = std::max(width, e->type->getBitWidth());
            }
        }

        std::vector<slang::SVInt> seen_const;
        slang::ast::ASTContext    astCtx(*scope_, slang::ast::LookupLocation::max);

        for (size_t k = 0; k < c.items.size(); k++) {
            const std::span<const Expression* const> exprs = c.items[k].expressions;
            for (size_t j = 0; j < exprs.size(); j++) {
                const Expression&          e  = *exprs[j];
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

    // Decide whether a Normal case can lower to a switch, and if so build its
    // arms. Eligible when every label is a constant with no x/z bits ("clean")
    // and the packed key fits 64 bits. The backend keys a clean value on its
    // bare bits ({may^must, must}); an x/z-bearing label would need the z field
    // and per-bit packing, so it falls back to the if-else chain (rare).
    //
    // Dedup happens here in one pass (LRM 12.5 first-match: a later label
    // duplicating an earlier key is dead, so it is dropped), giving the switch
    // unique keys without a second label evaluation. Returns nullopt (-> chain)
    // on the first non-constant or x/z label, or when the key is too wide.
    //
    // `had_dup` (when non-null) reports whether a duplicate constant key was
    // dropped. For a unique/unique0 case a dropped duplicate is a compile-time
    // overlap violation, so the caller must route those to the chain (which emits
    // the runtime overlap report) rather than the switch, which would silently
    // merge the keys.
    std::optional<std::vector<SwitchArmPlan>> build_switch_arms(const CaseStatement& c,
                                                                bool* had_dup = nullptr) {
        uint32_t width = c.expr.type->getBitWidth();
        bool     four  = c.expr.type->isFourState();
        for (const CaseStatement::ItemGroup& item : c.items) {
            for (const Expression* e : item.expressions) {
                width = std::max(width, e->type->getBitWidth());
                four  = four || e->type->isFourState();
            }
        }
        const uint32_t key_width = four ? 2 * width : width;
        if (key_width == 0 || key_width > 64) {
            return std::nullopt;
        }

        slang::ast::ASTContext                  astCtx(*scope_, slang::ast::LookupLocation::max);
        std::vector<SwitchArmPlan>              arms;
        ankerl::unordered_dense::set<uint64_t>  seen;
        for (const CaseStatement::ItemGroup& item : c.items) {
            std::vector<uint64_t> keys;
            for (const Expression* e : item.expressions) {
                slang::ast::EvalContext    ev(astCtx);
                const slang::ConstantValue cv = e->eval(ev);
                if (!cv.isInteger() || cv.integer().hasUnknown()) {
                    return std::nullopt;  // non-constant or x/z label -> chain.
                }
                slang::SVInt v = cv.integer();
                if (v.getBitWidth() < width) {
                    v = v.extend(width, e->type->isSigned());
                }
                const std::optional<uint64_t> u = v.as<uint64_t>();
                if (!u) {
                    return std::nullopt;
                }
                if (seen.insert(*u).second) {  // first occurrence wins; drop dups.
                    keys.push_back(*u);
                }
                else if (had_dup != nullptr) {
                    *had_dup = true;
                }
            }
            if (!keys.empty()) {
                arms.push_back({ std::move(keys), item.stmt });
            }
        }
        if (arms.empty()) {
            return std::nullopt;
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
        if (is_string_expr(c.expr)) {
            // TODO: case over strings (labels compare with rt_string_cmp; the
            // common-type/resize machinery below is packed-only).
            fail("case statement over strings is not supported");
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
                fail("case ... inside is not supported");
        }
        // unique / unique0 / priority (c.check) add the LRM 12.5.3 violation
        // checks; first-match execution is conforming for all of them.
        //   - no-match (priority or unique, no default): kind-0 report. unique0
        //     does NOT report no-match; a default can never no-match.
        //   - overlap (unique / unique0, >1 item matches): kind-1 report. Distinct
        //     constant labels can never overlap, so those keep the fast paths.
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

        const ir::Ref   ty  = common_case_type(c);
        // The selector is evaluated exactly once, up front (12.5 / 12.5.3).
        const ir::Index sel = b.resize(lower_expr(c.expr, b), ty, loc(c.expr));

        // Constant, wildcard-free case -> multiway switch (jump table). Distinct
        // constant labels can never overlap at runtime, so this fast path stays
        // valid for unique/unique0; a dropped duplicate key would be a
        // compile-time overlap, so had_dup routes those to the chain (which emits
        // the runtime overlap report). A default-less priority/unique case reports
        // no-match from the switch's own default region.
        if (c.condition == CaseStatementCondition::Normal) {
            bool                                      had_dup = false;
            std::optional<std::vector<SwitchArmPlan>> plan    = build_switch_arms(c, &had_dup);
            if (plan && !(needs_overlap && had_dup)) {
                std::vector<ir::Builder::SwitchArmBuild> arms;
                arms.reserve(plan->size());
                for (const SwitchArmPlan& ap : *plan) {
                    b.open_region();
                    lower_stmt(*ap.body, b);
                    const ir::ExtraSlice body = b.close_region();
                    arms.push_back({ std::span<const uint64_t>(ap.keys), body });
                }
                b.open_region();
                if (c.defaultCase != nullptr) {
                    lower_stmt(*c.defaultCase, b);
                }
                else if (no_match_report) {
                    b.case_violation(case_banner(c), 0, loc(c));
                }
                const ir::ExtraSlice default_region = b.close_region();
                b.switch_(sel, arms, default_region, loc(c.expr));
                return;
            }
        }

        // unique / unique0 need the eager overlap check; priority / plain use the
        // lazy first-match chain.
        if (needs_overlap) {
            lower_case_unique(c, sel, cmp, ty, no_match_report, b);
            return;
        }
        const std::vector<std::vector<char>> keep = case_dedup(c);
        lower_case_item(c, 0, sel, cmp, ty, keep, no_match_report, b);
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
        dispatch_preds(c, 0, preds, no_match_report, b);
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
    void dispatch_preds(const CaseStatement& c, size_t k, const std::vector<ir::Index>& preds,
                        bool no_match_report, ir::Builder& b) {
        if (k == c.items.size()) {
            if (c.defaultCase != nullptr) {
                lower_stmt(*c.defaultCase, b);
            }
            else if (no_match_report) {
                b.case_violation(case_banner(c), 0, loc(c));
            }
            return;
        }
        b.open_region();
        lower_stmt(*c.items[k].stmt, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        dispatch_preds(c, k + 1, preds, no_match_report, b);
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
    void lower_case_item(const CaseStatement& c, size_t k, ir::Index sel, ir::Inst::Tag cmp, ir::Ref ty,
                         const std::vector<std::vector<char>>& keep, bool report_viol, ir::Builder& b) {
        if (k == c.items.size()) {
            if (c.defaultCase != nullptr) {
                lower_stmt(*c.defaultCase, b);
            }
            else if (report_viol) {
                b.case_violation(case_banner(c), 0, loc(c));
            }
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
            lower_case_item(c, k + 1, sel, cmp, ty, keep, report_viol, b);
            return;
        }

        b.open_region();
        lower_stmt(*item.stmt, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        lower_case_item(c, k + 1, sel, cmp, ty, keep, report_viol, b);
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
            name == "$fscanf" || name == "$sscanf" || name == "$fread") {
            // File read / positioning / error / scan functions used in statement
            // position: lower for the side effect (file position, write-back) and
            // drop the result.
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
        fail("unsupported system task '" + std::string(name) + "'");
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
            if (et.bitWidth > 64) {
                // TODO: the table key packs the value into u64; a wider base needs
                // multi-word keys.
                fail("enum name() with a base type wider than 64 bits is not supported");
            }
            std::vector<std::pair<uint64_t, std::string_view>> table;
            table.reserve(members.size());
            for (const EnumMember& m : members) {
                uint64_t key = m.value.as<uint64_t>().value_or(0);
                if (et.bitWidth < 64) {
                    key &= (static_cast<uint64_t>(1) << et.bitWidth) - 1;
                }
                table.push_back({ key, m.name });
            }
            return b.enum_name(lower_expr(*call.arguments()[0], b), table, l);
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

        uint32_t mask = ir::CountOne; // $countones / $onehot* count definite-1 bits
        if (name == "$isunknown") {
            mask = ir::CountX | ir::CountZ;
        }
        else if (name == "$countbits") {
            mask = 0;
            for (size_t k = 1; k < args.size(); ++k) {
                mask |= count_control_class(*args[k]);
            }
        }

        const ir::Index count = b.count_bits(val, mask, l);

        if (name == "$countones" || name == "$countbits") {
            return b.resize(count, lower_type(*call.type), l);
        }
        // $onehot -> count == 1; $onehot0 -> count <= 1; $isunknown -> count != 0.
        ir::Inst::Tag   op  = ir::Inst::Tag::Ne;
        uint64_t        rhs = 0;
        if (name == "$onehot") {
            op  = ir::Inst::Tag::Eq;
            rhs = 1;
        }
        else if (name == "$onehot0") {
            op  = ir::Inst::Tag::Le;
            rhs = 1;
        }
        return b.resize(b.binary(op, count, b.constant(module_.pool.put_int(i32, rhs), l), l),
                        lower_type(*call.type), l);
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

        // Enum methods (LRM 6.19.2-6.19.5) arrive the same way, with an enum-typed
        // receiver as arguments()[0].
        if (!name.empty() && name[0] != '$' && !call.arguments().empty()) {
            const slang::ast::Type& rct = call.arguments()[0]->type->getCanonicalType();
            if (const auto* et = rct.as_if<slang::ast::EnumType>()) {
                return lower_enum_method(name, *et, call, b, l);
            }
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

        // $random (LRM §20.15.1): a 32-bit signed PRNG draw. The plain form
        // returns the next value of the simulator's global random stream.
        if (name == "$random") {
            // TODO: the seeded form `$random(seed)` (LRM §20.15.1) threads PRNG
            // state through an inout `seed` integer — it must read the seed
            // lvalue, advance a per-seed stream, and write the updated state
            // back. The reference PRNG (rt_random) already carries its whole
            // state in one 32-bit word, so the stream maps cleanly; the blocker
            // is only the IR's missing inout-arg writeback path, so the seeded
            // form is rejected rather than silently ignoring the seed.
            if (!call.arguments().empty()) {
                fail("$random with a seed argument is not supported");
            }
            return b.random(l);
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
            const ir::Index fname    = lower_string_arg(*args[0], b);
            const bool      has_mode = args.size() == 2;
            const ir::Index mode     = has_mode ? lower_string_arg(*args[1], b) : fname;
            return b.fopen(fname, mode, has_mode, l);
        }

        // $fgetc / $ungetc / $feof / $ftell / $fseek / $rewind (LRM §21.3): file
        // read / positioning functions, each returning a 32-bit integer. Operands
        // (a descriptor, plus a character or offset) are plain integer rvalues in
        // call order.
        if (name == "$fgetc" || name == "$feof" || name == "$ftell" || name == "$rewind" ||
            name == "$ungetc" || name == "$fseek") {
            ir::FileOp op;
            size_t     want;
            if (name == "$fgetc") {
                op   = ir::FileOp::Fgetc;
                want = 1;
            } else if (name == "$feof") {
                op   = ir::FileOp::Feof;
                want = 1;
            } else if (name == "$ftell") {
                op   = ir::FileOp::Ftell;
                want = 1;
            } else if (name == "$rewind") {
                op   = ir::FileOp::Rewind;
                want = 1;
            } else if (name == "$ungetc") {
                op   = ir::FileOp::Ungetc;
                want = 2;
            } else {
                op   = ir::FileOp::Fseek;
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
            return b.file_value_op(op, vals, l);
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
            case 'l':
                // TODO: %l needs library/config info (LRM §21.2.1.1, Clause 33).
                fail("%l (library binding) is not supported");
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
            if (conv == 'm') {
                // %m consumes no argument; the invoking scope is static at lower
                // time, so splice its name (width-padded) into the text.
                std::string name = hier_name();
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
        if (a.isCompound()) {
            return lower_compound_assign(a, b);
        }
        if (a.timingControl != nullptr) {
            // Intra-assignment timing: `x = #5 y;` / `x = @(posedge clk) y;`. The
            // delay/event is dropped today, so the update would land immediately at
            // the wrong time. Reject until the scheduler exists.
            fail("intra-assignment timing control (#/@ on an assignment) is not supported");
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
        if (is_unpacked_array(a.left())) {
            // TODO: whole-array assignment (mem1 = mem2, mem = '{...}). Needs an
            // element-wise copy / aggregate lowering; only element assignment is
            // supported today.
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

        // sliceSize > 0: '<<' — reverse order of S-bit blocks sliced from the
        // right. Block i covers bits [min((i+1)*S,W)-1 : i*S]; reversed output
        // places block 0 (rightmost) in the MSB position (first concat arg).
        // Extract each block via logical-right-shift + resize (PartSelect is
        // lvalue-only in the backend and cannot be used on intermediate values).
        if (S > 0) {
            const uint64_t N = (W + S - 1) / S;
            std::vector<ir::Index> blocks;
            blocks.reserve(N);
            const bool is_four = module_.pool.layout(target_ty).domain == ir::Domain::Four;
            for (uint64_t i = 0; i < N; ++i) {
                const uint32_t lo    = static_cast<uint32_t>(i * S);
                const uint32_t bw    = static_cast<uint32_t>(std::min((i + 1) * S, W) - lo);
                const ir::Ref  sh_ty = make_int_type(static_cast<uint32_t>(W), false, false);
                const ir::Index sh   = b.constant(module_.pool.put_int(sh_ty, lo), l);
                const ir::Index srl  = b.binary(ir::Inst::Tag::Srl, raw, sh, l);
                blocks.push_back(b.resize(srl, make_int_type(bw, is_four, false), l));
            }
            raw = b.concat(blocks, l);
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
            case ExpressionKind::NamedValue: {
                const ValueSymbol& sym = expr.as<NamedValueExpression>().symbol;
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
                return b.unary(ir::Inst::Tag::Load, value_lvalue(sym, b, l), l);
            }
            case ExpressionKind::LValueReference:
                // slang rewrites a compound assignment's RHS into
                // `BinaryExpression(LValueReference, rhs, op)`; the reference
                // stands for the LHS already lowered once by lower_compound_assign
                // (LRM 11.4.1 single-evaluation).
                if (!compound_lv_.has_value()) {
                    fail("LValueReference outside a compound assignment");
                }
                return b.unary(ir::Inst::Tag::Load, *compound_lv_, l);
            case ExpressionKind::UnaryOp:
                return lower_unary(expr.as<UnaryExpression>(), b);
            case ExpressionKind::BinaryOp:
                return lower_binary(expr.as<BinaryExpression>(), b);
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
                const ir::Index base = lower_lvalue(es.value(), b);
                const ir::Index sel =
                    is_unpacked_array(es.value())
                        ? b.elem_select(base, lower_expr(es.selector(), b), lower_type(*es.value().type), l)
                        : b.binary(ir::Inst::Tag::BitSelect, base, packed_bit_index(es, b, l), l);
                return b.unary(ir::Inst::Tag::Load, sel, l);
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs   = expr.as<RangeSelectExpression>();
                const ir::Index              base = lower_lvalue(rs.value(), b);
                return b.unary(ir::Inst::Tag::Load, lower_range_select(rs, base, b, l), l);
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
            case ExpressionKind::Assignment: {
                // Assignment used as an rvalue (`y = (x += 1)`, `y = (x = 5)`). Its
                // value is the LHS after the store, truncated to the LHS type (LRM
                // 11.3.1) — not the raw RHS. A string target yields the assigned
                // handle unchanged.
                const AssignmentExpression& a = expr.as<AssignmentExpression>();
                const ir::Index             v = lower_assign(a, b);
                return is_string_expr(a.left()) ? v : b.resize(v, lower_type(*a.left().type), l);
            }
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

    // Build the lvalue for a part-select over an already-lowered base lvalue: a
    // constant `[msb:lsb]` slice, or a runtime-base indexed slice `[off +: w]` /
    // `[off -: w]` (width constant, base a runtime value). Shared by the rvalue and
    // lvalue paths so a read and a write of the same shape lower identically.
    ir::Index lower_range_select(const RangeSelectExpression& rs, ir::Index base, ir::Builder& b, ir::SourceLoc l) {
        // TODO: range select on a multi-dim packed array selects whole
        // elements, so both bounds and the width scale by the element width.
        const auto* pa = rs.value().type->getCanonicalType().as_if<slang::ast::PackedArrayType>();
        if (pa != nullptr && pa->elementType.getBitWidth() != 1) {
            fail("range select on a multi-dimensional packed array is not supported");
        }
        const slang::ConstantRange range = packed_range_of(rs.value());
        if (rs.getSelectionKind() == RangeSelectionKind::Simple) {
            const std::optional<int32_t> left  = try_const_int(rs.left());
            const std::optional<int32_t> right = try_const_int(rs.right());
            if (!left || !right) {
                fail("expected constant part-select bounds");
            }
            // translateIndex puts the left bound at the higher LSB offset for
            // either endianness (slang has already checked the select's
            // direction matches the declared range).
            const auto msb = static_cast<uint32_t>(range.translateIndex(*left));
            const auto lsb = static_cast<uint32_t>(range.translateIndex(*right));
            return b.part_select(base, msb, lsb, l);
        }
        // Indexed part-select: the width is a constant (LRM 11.5.1) and the base
        // is a runtime value in declared-range coordinates. The IR carries the
        // low-bit LSB offset: that is the translated base offset, minus width-1
        // when the selection extends away from the LSB end (`-:` on a
        // little-endian range, `+:` on a big-endian one). An out-of-range or X/Z
        // base is bounds-checked in the backend per LRM 11.5.1 (out-of-range read
        // -> X, write -> no-op; partial selects handled bit-by-bit).
        const auto    width = static_cast<uint32_t>(const_uint(rs.right()));
        const bool    down  = rs.getSelectionKind() == RangeSelectionKind::IndexedDown;
        const int32_t bias  = down == range.isDescending() ? static_cast<int32_t>(width) - 1 : 0;
        const ir::Index off = lower_packed_offset(rs.left(), range, bias, b, l);
        return b.dyn_part_select(base, off, width, l);
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
                return value_lvalue(expr.as<NamedValueExpression>().symbol, b, l);
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es   = expr.as<ElementSelectExpression>();
                const ir::Index                base = lower_lvalue(es.value(), b);
                if (is_unpacked_array(es.value())) {
                    return b.elem_select(base, lower_expr(es.selector(), b), lower_type(*es.value().type), l);
                }
                return b.binary(ir::Inst::Tag::BitSelect, base, packed_bit_index(es, b, l), l);
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs   = expr.as<RangeSelectExpression>();
                const ir::Index              base = lower_lvalue(rs.value(), b);
                return lower_range_select(rs, base, b, l);
            }
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
    // bit sits below the translated base). A constant index folds to an
    // immediate. A runtime index is widened by its own signedness (so a
    // signed index into a negative-bound range resolves correctly) and keeps
    // the source's 4-state-ness so an X/Z index still poisons the offset
    // arithmetic instead of being silently coerced to a known value.
    ir::Index lower_packed_offset(const Expression& idx, slang::ConstantRange range, int32_t bias,
                                  ir::Builder& b, ir::SourceLoc l) {
        if (const std::optional<int32_t> c = try_const_int(idx)) {
            const auto off = static_cast<uint32_t>(range.translateIndex(*c) - bias);
            return b.constant(module_.pool.put_int(make_int_type(32, false, false), off), l);
        }
        const ir::Ref   off_ty = make_int_type(32, idx.type->isFourState(), idx.type->isSigned());
        const ir::Index v      = b.resize(lower_expr(idx, b), off_ty, l);
        if (range.isDescending()) {
            const int32_t sub = range.right + bias; // off = idx - right - bias
            if (sub == 0) {
                return v;
            }
            const ir::Index c = b.constant(module_.pool.put_int(off_ty, static_cast<uint32_t>(sub)), l);
            return b.binary(ir::Inst::Tag::Sub, v, c, l);
        }
        // Big-endian: off = (right - bias) - idx.
        const ir::Index c =
            b.constant(module_.pool.put_int(off_ty, static_cast<uint32_t>(range.right - bias)), l);
        return b.binary(ir::Inst::Tag::Sub, c, v, l);
    }

    // Normalized bit offset for an element select on a packed value.
    ir::Index packed_bit_index(const ElementSelectExpression& es, ir::Builder& b, ir::SourceLoc l) {
        // TODO: element select on a multi-dim packed array selects a whole
        // multi-bit element; BitSelect models a 1-bit result, so the offset
        // would also need scaling by the element width.
        if (es.type->getBitWidth() != 1) {
            fail("element select on a multi-dimensional packed array is not supported");
        }
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
        }

        return b.signal(signal_of(vs), l);
    }

    // A 1-bit zero, used as the (ignored) operand of a void function's Ret. Ret
    // always carries an operand; the backend drops it for a void return.
    ir::Index void_ret_operand(ir::Builder& b, ir::SourceLoc l) {
        return b.constant(module_.pool.put_int(make_int_type(1, false, false), 0), l);
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
            // A string result needs no width resize (slang already converted).
            const ir::Index v = lower_expr(*r.expr, b);
            b.unary(ir::Inst::Tag::Ret,
                    is_string_ref(fn_ctx_->ret_type)
                      ? v
                      : coerce(v, *r.expr->type, fn_ctx_->ret_type, b, l),
                    l);
        } else if (fn_ctx_->has_ret_local) {
            // `return;` in a non-void function yields the current function-var value.
            const ir::Index rv = b.local(fn_ctx_->ret_local, fn_ctx_->ret_type, l);
            b.unary(ir::Inst::Tag::Ret, b.unary(ir::Inst::Tag::Load, rv, l), l);
        } else {
            b.unary(ir::Inst::Tag::Ret, void_ret_operand(b, l), l);
        }
    }

    // Resolve a subroutine (function or task) to a FunctionId, lowering its body
    // on first reference. The id is reserved before the body is built so a
    // recursive call resolves it.
    ir::FunctionId function_id(const SubroutineSymbol& sub) {
        const auto cached = fnmap_.find(&sub);
        if (cached != fnmap_.end()) {
            return cached->second;
        }
        if (sub.flags.has(MethodFlags::DPIImport)) {
            // TODO: DPI imports — call out to native code via the runtime.
            fail("DPI imported subroutines are not supported");
        }
        const bool is_task = sub.subroutineKind == SubroutineKind::Task;
        for (const FormalArgumentSymbol* fa : sub.getArguments()) {
            const auto dir = fa->direction;
            if (dir == slang::ast::ArgumentDirection::Ref) {
                // TODO: ref arguments — live aliasing of the actual through the body.
                fail("subroutines with ref arguments are not supported");
            }
            if (!is_task && dir != slang::ast::ArgumentDirection::In) {
                // A function copies no arguments back; only tasks carry out/inout.
                // TODO: output / inout arguments on functions.
                fail("functions with output/inout/ref arguments are not supported");
            }
            if (is_task && dir != slang::ast::ArgumentDirection::In &&
                fa->getType().getCanonicalType().isString()) {
                // TODO: output / inout string task formals (the copy-out path
                // passes plane pointers, not string handles).
                fail("output/inout string task arguments are not supported");
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

    // Map a slang argument direction to the IR direction (ref is rejected upstream).
    static ir::ArgDir to_arg_dir(slang::ast::ArgumentDirection dir) {
        switch (dir) {
            case slang::ast::ArgumentDirection::In:
                return ir::ArgDir::In;
            case slang::ast::ArgumentDirection::Out:
                return ir::ArgDir::Out;
            case slang::ast::ArgumentDirection::InOut:
                return ir::ArgDir::Inout;
            default:
                return ir::ArgDir::In;
        }
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
        b.unary(ir::Inst::Tag::Ret, void_ret_operand(b, el), el);

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

        // Parameters: copy each Arg into the formal's Local at entry, so the body
        // reads / writes the formal as ordinary frame storage.
        for (const FormalArgumentSymbol* fa : sub.getArguments()) {
            const ir::Ref       pty = lower_type(fa->getType());
            const ir::SourceLoc fl  = to_source_loc(fa->location);
            const auto          pos = static_cast<uint32_t>(param_types.size());
            param_types.push_back(pty);
            const uint32_t idx = fn_local(*fa);
            b.binary(ir::Inst::Tag::Store, b.local(idx, pty, fl), b.arg(pos, pty, fl), fl);
        }

        if (!is_void) {
            ctx.has_ret_local = true;
            ctx.ret_type      = return_type;
            ctx.ret_local     = fn_local(*sub.returnValVar);
        }

        lower_stmt(sub.getBody(), b);

        // Fall-through return: yields the function variable (X if never assigned).
        const ir::SourceLoc el = to_source_loc(sub.location);
        if (is_void) {
            b.unary(ir::Inst::Tag::Ret, void_ret_operand(b, el), el);
        } else {
            const ir::Index rv = b.local(ctx.ret_local, ctx.ret_type, el);
            b.unary(ir::Inst::Tag::Ret, b.unary(ir::Inst::Tag::Load, rv, el), el);
        }

        module_.set_function(id, ir::Function{ .name        = std::string(sub.name),
                                               .return_type = return_type,
                                               .param_types = std::move(param_types),
                                               .local_types = std::move(ctx.local_types),
                                               .ir          = b.finish() });
        named_blocks_.swap(saved_blocks);
        fn_ctx_ = saved;
    }

    // Lower a user function call: the actuals (resized to the parameter types)
    // become a `call`. Returns the result value, or nullopt for a void function.
    std::optional<ir::Index> lower_user_call(const CallExpression& call, ir::Builder& b) {
        if (call.subroutine.index() != 0) {
            fail("system call used where a user subroutine was expected");
        }
        if (call.thisClass() != nullptr) {
            // TODO: class methods — need object handles.
            fail("class method calls are not supported");
        }
        const SubroutineSymbol& sub = *std::get<0>(call.subroutine);
        if (sub.subroutineKind == SubroutineKind::Task) {
            // A task is a statement, never a value; it blocks the caller until done.
            lower_task_call(call, sub, b);
            return std::nullopt;
        }
        const ir::FunctionId id = function_id(sub); // validates + lowers (lazily)

        const auto formals = sub.getArguments();
        const auto actuals = call.arguments();
        if (formals.size() != actuals.size()) {
            fail("function argument count mismatch");
        }
        const ir::SourceLoc    l = loc(call);
        std::vector<ir::Index> args;
        args.reserve(actuals.size());
        for (size_t i = 0; i < formals.size(); ++i) {
            const ir::Ref pty = lower_type(formals[i]->getType());
            // A string actual passes as-is (no width to resize to); the callee's
            // copy-in gives value semantics.
            args.push_back(is_string_ref(pty)
                             ? lower_expr(*actuals[i], b)
                             : coerce(lower_expr(*actuals[i], b), *actuals[i]->type, pty, b, l));
        }
        const ir::Index c = b.call(id, args, l);

        return sub.returnValVar == nullptr ? std::nullopt : std::optional<ir::Index>(c);
    }

    // Lower a task enable (LRM 13.3). The backend drives the task coroutine and
    // blocks the caller until it completes. Argument passing (13.5): each input is
    // a value resized to the parameter type; each output / inout uses a caller temp
    // that the backend passes to the task by pointer. We copy an inout's actual into
    // its temp before the call and copy every output / inout temp back to its actual
    // after — both as ordinary assignments, so width conversion and complex actual
    // lvalues (bit / part selects) reuse the normal store path.
    void lower_task_call(const CallExpression& call, const SubroutineSymbol& sub, ir::Builder& b) {
        const ir::FunctionId id      = function_id(sub); // validates + lowers (lazily)
        const auto           formals = sub.getArguments();
        const auto           actuals = call.arguments();
        if (formals.size() != actuals.size()) {
            fail("task argument count mismatch");
        }
        const ir::SourceLoc l = loc(call);

        // One temp + copy-back record per output / inout argument.
        struct OutArg {
            ir::Index         temp;      // the caller temp lvalue (parameter width)
            const Expression* actual;    // the call-site lvalue to copy back into
            ir::Ref           actual_ty; // the actual's type (target of copy-out)
            const slang::ast::Type* formal_ty; // the formal's type (source of copy-out)
        };
        std::vector<OutArg>    outs;
        std::vector<ir::Index> args;
        args.reserve(formals.size());
        for (size_t i = 0; i < formals.size(); ++i) {
            const ir::Ref pty = lower_type(formals[i]->getType());
            if (formals[i]->direction == slang::ast::ArgumentDirection::In) {
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
            const ir::Index temp = make_task_temp(pty, b, l);
            if (formals[i]->direction == slang::ast::ArgumentDirection::InOut) {
                // Copy-in: temp = (parameter type) actual.
                b.binary(ir::Inst::Tag::Store, temp, coerce(lower_expr(*actual, b), *actual->type, pty, b, l), l);
            }
            args.push_back(temp);
            outs.push_back(OutArg{ temp, actual, lower_type(*actual->type), &formals[i]->getType() });
        }

        b.task_call(id, args, l);

        // Copy-out at completion: actual = (actual type) temp.
        for (const OutArg& oc : outs) {
            const ir::Index dst = lower_lvalue(*oc.actual, b);
            const ir::Index tv  = b.unary(ir::Inst::Tag::Load, oc.temp, l);
            b.binary(ir::Inst::Tag::Store, dst, coerce(tv, *oc.formal_ty, oc.actual_ty, b, l), l);
        }
    }

    // A fresh, anonymous storage cell for an output / inout task argument, returned
    // as an lvalue leaf. Inside a task caller it is a per-call frame Local; in a
    // process it is a module Signal (a process has no frame). The same lvalue Index
    // is reused for copy-in, the task_call operand, and copy-out.
    ir::Index make_task_temp(ir::Ref type, ir::Builder& b, ir::SourceLoc l) {
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
    // Scope of the instance body currently being lowered; used to fold constant
    // expressions (parameter references and arithmetic over them).
    const slang::ast::Scope* scope_ = nullptr;
    // Maps an elaborated value symbol to its module Signal handle.
    ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal> sigmap_;
    // Continuous-driver coverage per net signal: the closed bit ranges [lo, hi]
    // already driven, checked for overlap by record_net_driver_range.
    ankerl::unordered_dense::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> net_driven_;
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
    ankerl::unordered_dense::map<const SubroutineSymbol*, ir::FunctionId> fnmap_;
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
    if (root.topInstances.empty()) {
        result.diagnostics += "error: no top-level module found\n";
        return result;
    }

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
    // ports), so per-Lowerer maps need not — and must not — be shared.
    const bool single = root.topInstances.size() == 1;
    if (!single) {
        module->name = std::string(root.topInstances[0]->name);
    }
    try {
        for (const InstanceSymbol* top : root.topInstances) {
            const std::string prefix = single ? std::string() : std::string(top->name) + ".";
            Lowerer(*module, tick_fs).lower_top(*top, prefix);
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
