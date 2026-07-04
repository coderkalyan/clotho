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
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/numeric/Time.h>
#include <slang/syntax/SyntaxTree.h>

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

// Infers the implicit sensitivity of a combinational process body. `reads`
// holds every signal read; `written` holds every assignment target. The two
// exclusion rules differ by construct: `always_comb`/`always_latch` subtract
// signals also written (LRM 9.2.2.2.1 exception b), while `always @*` keeps
// them — 9.4.2.2 excludes only identifiers appearing solely as lvalues, so a
// read+written signal stays in the list and an external writer re-triggers
// the block. Index/part-select expressions on a write target are themselves
// reads in both schemes (11.6, 9.2.2.2, 9.4.2.2).
//
// The AST is walked rather than the lowered IR so reads are seen at the source
// level, before any temporaries the IR introduces. Symbols not in `sigmap`
// (parameters, genvars, other constants) carry no sensitivity and are skipped.
struct SensitivityCollector : ASTVisitor<SensitivityCollector, true, true> {
    using SigMap = ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal>;

    const SigMap&                          sigmap;
    std::vector<ir::Signal>                reads;    // in first-seen order
    ankerl::unordered_dense::set<uint32_t> seen;     // dedup for `reads`
    ankerl::unordered_dense::set<uint32_t> written;  // assignment targets / locals
    // User functions already descended into, so a function called from several
    // sites (or a recursive one) is walked at most once.
    ankerl::unordered_dense::set<const SubroutineSymbol*> entered;

    explicit SensitivityCollector(const SigMap& m) : sigmap(m) {}

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
        record_write(a.left());
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

    // A called function's reads count toward the caller's sensitivity: a function
    // may reference module signals directly, not only through its arguments. Visit
    // the actual-argument expressions, then descend once into the callee body. The
    // callee's formals / locals are not module signals (not in sigmap), so they are
    // ignored; module signals it reads / writes are recorded as usual.
    void handle(const CallExpression& c) {
        for (const Expression* arg : c.arguments()) {
            arg->visit(*this);
        }
        if (c.isSystemCall()) {
            return;
        }
        const SubroutineSymbol* sub = std::get<0>(c.subroutine);
        if (sub != nullptr && entered.insert(sub).second) {
            sub->getBody().visit(*this);
        }
    }
};

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
                case SymbolKind::InstanceArray:
                    // TODO: instance arrays (foo u[N](...)) — needs per-element
                    // elaboration and bit-sliced port connections.
                    fail("instance arrays are not supported");
                    break;
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
    std::vector<ir::WaitEvent> continuous_events(const Expression& e) {
        std::vector<ir::WaitEvent> events;
        SensitivityCollector       c(sigmap_);
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
                collect_inferred_sensitivity(*expr, sens, /*exclude_written=*/false);
            }
            // A port connection is a continuous copy: evaluate at time zero so the
            // child sees the parent's declaration-initialized value even when the
            // parent signal never changes afterward (LRM 23.3.3).
            add_continuous_process(std::move(sens), l, b);
        }
    }

    void lower_procedural_block(const ProceduralBlockSymbol& sym) {
        ir::Activation             activation = ir::Activation::Always;
        std::vector<ir::WaitEvent> wait_events;

        if (sym.procedureKind == ProceduralBlockKind::Initial) {
            activation = ir::Activation::Initial;
        } else if (sym.procedureKind == ProceduralBlockKind::Final) {
            // TODO: `final` blocks run once at simulation end ($finish), LRM 9.2.3.
            // Lowering needs an end-of-sim hook; without it the block would be
            // mis-scheduled as an always (a sensitivity-less infinite loop).
            fail("final block is not supported");
        }

        const Statement* body = &sym.getBody();

        // `always_comb` / `always_latch` infer sensitivity from the body's reads
        // minus its writes (9.2.2.2.1, legal because of the single-writer rule).
        // `always @*` carries an ImplicitEvent timing wrapping the body and keeps
        // read+written signals (9.4.2.2): other processes may write them.
        const bool comb = sym.procedureKind == ProceduralBlockKind::AlwaysComb ||
                          sym.procedureKind == ProceduralBlockKind::AlwaysLatch;
        bool infer = comb;

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
            collect_inferred_sensitivity(*body, wait_events, /*exclude_written=*/comb);
        }
        // always_comb / always_latch evaluate once at time zero (LRM 9.2.2.2), so
        // they settle from declaration-initialized values. An `always @*` or an
        // explicit `@(...)` list is a plain sensitivity list that runs only when a
        // listed signal changes, matching Verilog, so it is left non-self-starting.
        const bool self_start = sym.procedureKind == ProceduralBlockKind::AlwaysComb ||
                                sym.procedureKind == ProceduralBlockKind::AlwaysLatch;
        module_.add_process(ir::Process{ .activation  = activation,
                                         .wait_events = std::move(wait_events),
                                         .self_start  = self_start,
                                         .loc         = to_source_loc(sym.location),
                                         .ir          = b.finish() });
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
                out.push_back(ir::WaitEvent{ .signal = event_signal(ev.expr), .edge = to_edge(ev.edge) });
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

    // Build the inferred sensitivity of an AST node — a combinational process
    // body (@* / always_comb) or a port-connection source expression — and
    // append every signal read as a level-sensitive (any-change) wait.
    // `exclude_written` selects the always_comb rule (9.2.2.2.1 b: drop signals
    // the node also writes); `always @*` passes false per 9.4.2.2.
    template <typename Node>
    void collect_inferred_sensitivity(const Node& node, std::vector<ir::WaitEvent>& out,
                                      bool exclude_written) {
        SensitivityCollector c(sigmap_);
        node.visit(c);
        for (const ir::Signal s : c.reads) {
            if (exclude_written && c.written.contains(static_cast<uint32_t>(s))) {
                continue;
            }
            if (is_string_ref(module_.signal(s).type)) {
                // TODO: string signals in inferred sensitivity — string
                // stores skip wait-queue servicing, so the process would
                // never re-trigger.
                fail("string in inferred sensitivity is not supported");
            }
            out.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
        }
    }

    // Map a slang edge kind to the IR edge. A level-sensitive entry (no edge) and
    // a both-edge entry both record as Any (any value change).
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

    // A loop condition keeps its native width (the backend's definite-1 rule
    // normalizes multi-bit and X/Z), except a string, which must lower to its
    // 1-bit truth value.
    ir::Index lower_loop_cond(const Expression& cond, ir::Builder& b) {
        return is_string_expr(cond) ? lower_predicate(cond, b) : lower_expr(cond, b);
    }

    void lower_conditional(const ConditionalStatement& c, ir::Builder& b) {
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
            lower_stmt(*c.ifFalse, b);
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

        b.loop(cond, pred, body, step, loc(f));
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

        b.loop(cond, pred, body, step, loc(w));
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

        b.loop(cond, pred, body_region, step, loc(body));
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

        b.loop(cond, pred, body, step, l);
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
            const UnaryExpression& u = e.as<UnaryExpression>();
            using U                  = slang::ast::UnaryOperator;
            bool inc;
            switch (u.op) {
                case U::Preincrement:
                case U::Postincrement:
                    inc = true;
                    break;
                case U::Predecrement:
                case U::Postdecrement:
                    inc = false;
                    break;
                default:
                    fail("unsupported for-step expression");
            }
            // A step is a statement: the loop variable's value before/after the
            // bump is discarded, so pre and post forms lower identically.
            const ir::SourceLoc l   = loc(e);
            const ir::Index     lv  = lower_lvalue(u.operand(), b);
            const ir::Index     cur = b.unary(ir::Inst::Tag::Load, lv, l);
            if (u.operand().type->isFloating()) {
                // ++ / -- on a real add / subtract 1.0 (LRM 11.4.2).
                const ir::Index     lv   = lower_lvalue(u.operand(), b);
                const ir::Index     cur  = b.unary(ir::Inst::Tag::Load, lv, l);
                const ir::Index     onef =
                  b.constant(module_.pool.put_real(lower_type(*u.operand().type), 1.0), l);
                const bool          inc = u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Postincrement;
                const ir::Index     nv =
                  b.binary(inc ? ir::Inst::Tag::FAdd : ir::Inst::Tag::FSub, cur, onef, l);
                b.binary(ir::Inst::Tag::Store, lv, nv, l);
                return;
            }
            const ir::Index     one = b.constant(module_.pool.put_int(lower_type(*u.operand().type), 1), l);
            const ir::Index     nv  = b.binary(inc ? ir::Inst::Tag::Add : ir::Inst::Tag::Sub, cur, one, l);
            b.binary(ir::Inst::Tag::Store, lv, nv, l);
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
        // NOTE: unique / unique0 / priority (c.check) only govern violation
        // reporting, a runtime assertion; the first-match execution below is
        // conforming for all of them.
        // TODO: emit unique / priority violation reports.

        const ir::Ref   ty  = common_case_type(c);
        // The selector is evaluated exactly once, up front (12.5).
        const ir::Index sel = b.resize(lower_expr(c.expr, b), ty, loc(c.expr));

        lower_case_item(c, 0, sel, cmp, ty, b);
    }

    // Emit the if-else chain for items[k..] into the current region: item k's
    // match test guards its body, with the rest of the chain (eventually the
    // default) as the else branch. Items are tested in textual order and the
    // first match wins; labels for items not yet reached are evaluated lazily,
    // inside the else regions, so they run only until a match is found.
    void lower_case_item(const CaseStatement& c, size_t k, ir::Index sel, ir::Inst::Tag cmp, ir::Ref ty,
                         ir::Builder& b) {
        if (k == c.items.size()) {
            if (c.defaultCase != nullptr) {
                lower_stmt(*c.defaultCase, b);
            }
            return;
        }

        const CaseStatement::ItemGroup& item = c.items[k];
        if (item.expressions.empty()) {
            lower_case_item(c, k + 1, sel, cmp, ty, b);
            return;
        }

        // Match predicate: OR of (selector <cmp> label) over the group's labels.
        ir::Index pred{};
        bool      have = false;
        for (const Expression* e : item.expressions) {
            const ir::Index val = b.resize(lower_expr(*e, b), ty, loc(*e));
            const ir::Index one = b.binary(cmp, sel, val, loc(*e));
            pred                = have ? b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e)) : one;
            have                = true;
        }

        b.open_region();
        lower_stmt(*item.stmt, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        lower_case_item(c, k + 1, sel, cmp, ty, b);
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
            // suspend-forever equals exit and the two are treated identically.
            // TODO: the optional diagnostic-level argument (0/1/2) controlling
            // exit-message verbosity is parsed by slang but dropped here.
            b.finish(loc(call));
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
        const Expression& mem = *args[1];
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

        if (fn_ctx_ != nullptr && !fn_ctx_->is_task && (name == "$time" || name == "$stime")) {
            // A function is a plain callable with no wheel pointer in its ABI, so
            // it cannot read the simulation clock. Reject rather than emit a
            // SimTime that would dereference a stale wheel. A task is a coroutine
            // and does carry the wheel, so $time is allowed there.
            // TODO: thread the wheel into the function ABI to support this.
            fail("$time / $stime inside a function is not supported");
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
            // back. The V2005 reference stream does not map cleanly onto
            // mt19937's 624-word state, and the IR has no inout-arg writeback
            // path yet, so the seeded form is rejected rather than silently
            // ignoring the seed.
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
            const Expression& target = *args[0];
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

        // TODO: $realtime (LRM §20.3.3) returns a real scaled to the module time
        // unit; blocked on real (floating-point) value support in the IR.
        if (name == "$realtime") {
            fail("$realtime is not supported (no real-type support)");
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
        // TODO: atoi / atohex / atooct / atobin / atoreal and the itoa family
        // (LRM 6.16.9-6.16.10) — need the numeric conversion runtime entries.
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
                            // TODO: %t with a real (realtime) argument — needs the
                            // real tick-scaling path (lands with $realtime).
                            fail("%t with a real argument is not supported");
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
        const ir::Index     w   = b.resize(v, u64, l);
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

    void lower_assign(const AssignmentExpression& a, ir::Builder& b) {
        if (a.isCompound()) {
            fail("compound assignment not supported");
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
            b.str_putc(str, idx, lower_expr(a.right(), b), loc(a));
            return;
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
            return;
        }
        b.binary(tag, lower_lvalue(a.left(), b), rhs, loc(a));
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
                if (is_unpacked_array(expr)) {
                    // TODO: reading a whole unpacked array as a value (array copy,
                    // passing a memory by value). Only element reads are supported.
                    fail("whole unpacked array read is not supported");
                }
                return b.unary(ir::Inst::Tag::Load, value_lvalue(sym, b, l), l);
            }
            case ExpressionKind::UnaryOp:
                return lower_unary(expr.as<UnaryExpression>(), b);
            case ExpressionKind::BinaryOp:
                return lower_binary(expr.as<BinaryExpression>(), b);
            case ExpressionKind::ConditionalOp: {
                const ConditionalExpression& c = expr.as<ConditionalExpression>();
                if (is_string_expr(expr)) {
                    // TODO: ?: over strings (needs a string-aware mux; lower to a
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
                const ir::Index pred = lower_predicate(*c.conditions[0].expr, b);
                const ir::Index a    = lower_expr(c.left(), b);
                const ir::Index bb   = lower_expr(c.right(), b);
                return b.mux(pred, a, bb, l);
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
            default:
                fail("unsupported expression kind=" + std::string(slang::ast::toString(expr.kind)));
        }
    }

    ir::Index lower_unary(const UnaryExpression& u, ir::Builder& b) {
        const ir::SourceLoc l  = loc(u);
        const ir::Index     op = lower_expr(u.operand(), b);
        using U                = slang::ast::UnaryOperator;
        using T                = ir::Inst::Tag;
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
        const ir::SourceLoc l   = loc(bin);
        const ir::Index     lhs = lower_expr(bin.left(), b);
        const ir::Index     rhs = lower_expr(bin.right(), b);
        using B                 = slang::ast::BinaryOperator;
        using T                 = ir::Inst::Tag;

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
            case B::LogicalAnd:
                return b.binary(T::Land, lhs, rhs, l);
            case B::LogicalOr:
                return b.binary(T::Lor, lhs, rhs, l);
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
        // little-endian range, `+:` on a big-endian one).
        //
        // TODO: an out-of-range or X/Z base is not bounds-checked. LRM 11.5.1: an
        // out-of-range read yields X and such a write is a no-op. This inherits the
        // dynamic BitSelect gap and shifts by the raw value.
        const auto    width = static_cast<uint32_t>(const_uint(rs.right()));
        const bool    down  = rs.getSelectionKind() == RangeSelectionKind::IndexedDown;
        const int32_t bias  = down == range.isLittleEndian() ? static_cast<int32_t>(width) - 1 : 0;
        const ir::Index off = lower_packed_offset(rs.left(), range, bias, b, l);
        return b.dyn_part_select(base, off, width, l);
    }

    // Lower an expression as an lvalue, returning the lvalue index (no Load).
    ir::Index lower_lvalue(const Expression& expr, ir::Builder& b) {
        const ir::SourceLoc l = loc(expr);
        switch (expr.kind) {
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
        if (range.isLittleEndian()) {
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
    if (sm != nullptr && !diags.empty()) {
        slang::DiagnosticEngine engine(*sm);
        auto                    client = std::make_shared<slang::TextDiagnosticClient>();
        engine.addClient(client);
        for (const slang::Diagnostic& d : diags) {
            engine.issue(d);
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

LowerResult lower_file(std::string_view path, Timings* timings) {
    const auto t_parse = std::chrono::steady_clock::now();
    auto       tree    = slang::syntax::SyntaxTree::fromFile(path);
    if (!tree) {
        LowerResult result;
        result.diagnostics =
          "error: cannot read '" + std::string(path) + "': " + std::string(tree.error().second) + "\n";
        return result;
    }

    slang::ast::Compilation comp;
    comp.addSyntaxTree(*tree);
    if (timings != nullptr) {
        timings->frontend_ms += ms_since(t_parse);
    }

    return lower_compilation(comp, timings);
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
