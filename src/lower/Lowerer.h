// Internal to the frontend: the Lowerer class and the AST helpers its files share.
// Only src/lower/*.cpp include this.

#pragma once

#include "Lower.h"

#include <algorithm>
#include <any>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <slang/analysis/AnalysisManager.h>
#include <slang/analysis/AnalyzedAssertion.h>
#include <slang/analysis/AnalyzedProcedure.h>
#include <slang/ast/ASTContext.h>
#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Compilation.h>
#include <slang/ast/EvalContext.h>
#include <slang/ast/Expression.h>
#include <slang/ast/Patterns.h>
#include <slang/ast/Scope.h>
#include <slang/ast/SemanticFacts.h>
#include <slang/ast/Statement.h>
#include <slang/ast/TimingControl.h>
#include <slang/ast/expressions/AssertionExpr.h>
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
#include <slang/ast/symbols/AttributeSymbol.h>
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
#include <slang/ast/types/TypePrinter.h>
#include <slang/diagnostics/AnalysisDiags.h>
#include <slang/diagnostics/CompilationDiags.h>
#include <slang/diagnostics/DeclarationsDiags.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/LookupDiags.h>
#include <slang/diagnostics/SysFuncsDiags.h>
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/driver/Driver.h>
#include <slang/numeric/Time.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <slang/util/Function.h>
#include <slang/util/SmallVector.h>

#include "SourceMap.h"
#include "Sva.h"
#include "ir/Builder.h"
#include "ir/TypeModel.h"
#include "runtime/Format.h"

namespace clotho::lower {

using namespace slang::ast;

// Raised when lowering meets a construct Clotho does not implement. Caught at the
// top level and turned into a Diagnostic.
//
// The location travels as a span, not as text prepended to `message`: a
// structured consumer needs the range, and a renderer can add the "file:line:col:"
// prefix itself. `message` is therefore the bare complaint, with no location in it.
struct LowerError {
    std::string    message;
    ir::SourceSpan span;
};

const ValueSymbol& canonical_value_symbol(const ValueSymbol& sym);

uint64_t tsv_to_fs(slang::TimeScaleValue v);

bool delay_is_real(const TimingControl* tc);

// Walks a procedural block body for any real (`#1.5`-style) delay: a `#delay`
// statement or an intra-assignment delay whose amount is floating. Existence-only
// (stops mattering once `found` is set), so it never needs to descend further,
// but a plain visitor is simplest.
struct RealDelayFinder : slang::ast::ASTVisitor<RealDelayFinder, VisitFlags::AllGood> {
    bool found = false;

    void handle(const slang::ast::TimedStatement& s);

    void handle(const slang::ast::AssignmentExpression& a);
};

bool subroutine_has_real_delay(const SubroutineSymbol& sub);

// -- Waited-on class properties (LRM 9.4.2 / 9.4.3) --------------------------
//
// A class property is heap storage inside an object, not a signal, so it owns none
// of the per-signal wait machinery an `@` or a `wait` normally parks on. The three
// shapes below therefore all failed: `@p` in a method and `@(obj.p)` reported the
// property as an unknown signal, and `wait(p)` -- whose sensitivity is the read set
// of its condition, which contains no signal at all -- parked on an EMPTY event
// list and hung forever with no diagnostic.
//
// The fix gives such a property hidden `event`-typed sibling properties (its change
// notifiers, ir::ChangeNotify) and makes a store to it classify its own old -> new
// transition, exactly as a blocking store to a signal already does. These two
// resolvers decide which properties get them; the design-wide pre-pass and the
// wait lowering MUST agree, so both call these and nothing else.

const slang::ast::ClassPropertySymbol* instance_class_prop(const Expression& e);

bool is_notified_container(const slang::ast::Type& ty);

// What a store to a property of this type can classify, and therefore which change
// notifiers the property may carry (LRM 9.4.2: "any change in the value").
enum class PropNotify : uint8_t {
    // Neither an old -> new word nor a mutation surface: an unpacked struct or
    // union, a FIXED unpacked array. Refused loudly in both constructs.
    None,
    // The value IS one stored word, so a store compares old against new through
    // the same emit_edge_facts a signal store uses. Every integral type; the three
    // handle kinds whose value IS the handle (a class or interface-class handle,
    // LRM 8.4; a chandle, 6.14; a virtual interface, 25.9 -- each admitting only
    // assignment, `==`/`!=` and `null`, so identity is exactly what a condition
    // over one tests); and real / shortreal, whose IEEE-754 pattern is one 2-state
    // word (LRM 6.12: no x/z, hence nothing to classify beyond equality).
    Word,
    // The value is the string's CONTENT (LRM 6.16), not the rt_string handle, so
    // the store compares with rt_string_cmp. Two distinct handles over the same
    // characters are the same value and must fire nothing.
    String,
};

PropNotify prop_notify_kind(const slang::ast::Type& ty);

bool prop_has_edges(const slang::ast::Type& ty);

const slang::ast::ClassPropertySymbol* waited_class_prop(const Expression& e);

const slang::ast::ClassPropertySymbol* waited_container_prop(const Expression& e);

const slang::ast::ClassPropertySymbol* level_waited_prop(const Expression& e);

// Every class-property reference in an expression, whether or not it is notifiable.
// `wait` needs both halves: the notifiable ones become sensitivity, and the mere
// PRESENCE of a non-notifiable one is what turns an otherwise empty sensitivity
// from a silent permanent park into a loud failure (lower_wait_stmt).
struct ClassPropReadScan : slang::ast::ASTVisitor<ClassPropReadScan, VisitFlags::AllGood> {
    // Notifiable property references, in source order, deduped by expression node.
    std::vector<const Expression*> notifiable;
    // Any non-static class property was referenced, notifiable or not.
    bool any_prop = false;
    // A CONTAINER property was referenced. `wait` parks on its mutation notifier;
    // `@` refuses, because a mutation is not a change in value (LRM 9.4.2).
    bool any_container = false;
    // Event-typed property references, in source order. `wait` parks on the event
    // OBJECT the property names -- the thing `-> e` fires -- not on a notifier;
    // prop_notify_kind mints none for an event on purpose.
    std::vector<const Expression*> event_props;

    void take(const Expression& e);

    void handle(const slang::ast::NamedValueExpression& e);
    void handle(const slang::ast::HierarchicalValueExpression& e);
    void handle(const MemberAccessExpression& ma);
};

bool box_can_notify(const slang::ast::Type& ty);

// Every value symbol a `wait` condition names. The design-wide pre-pass uses it to
// decide which capture boxes are minted WITH a notifier; a symbol that turns out to
// be a signal or a class property is simply never asked about again.
struct WaitLocalScan : slang::ast::ASTVisitor<WaitLocalScan, VisitFlags::AllGood> {
    ankerl::unordered_dense::set<const slang::ast::ValueSymbol*>* out = nullptr;

    void handle(const slang::ast::NamedValueExpression& e);
    void handle(const slang::ast::HierarchicalValueExpression& e);
};

// Which (class property, edge) pairs the whole design waits on. Design-wide because
// a class's object body must carry the notifiers BEFORE it is laid out, and
// lower_class_type runs on demand from the first reference under any top -- which
// can be lexically far from, or in another top than, the `@` that needs one.
// Modelled on EventMergeScan (the events_merge design-wide fact) in `lower`.
struct WaitedProps {
    struct Waited {
        // Bitmask over ir::Edge of the edges waited on.
        uint8_t edges = 0;
        // A container property: exactly one Any notifier, fired on mutation
        // rather than on a classified transition.
        bool container = false;
    };
    ankerl::unordered_dense::map<const slang::ast::ClassPropertySymbol*, Waited> props;
    // Every value symbol some `wait` condition in the design names. Consulted when a
    // capture box is minted (box_one_capture / install_fork_decl_box): a box whose
    // symbol is here carries a change notifier so the wait can be woken. A superset
    // of the symbols that end up boxed -- signals and class properties land here too
    // and are simply never asked about.
    ankerl::unordered_dense::set<const slang::ast::ValueSymbol*> locals;

    bool waits_local(const slang::ast::ValueSymbol& vs) const;

    void add(const slang::ast::ClassPropertySymbol& prop, ir::Edge edge);

    void add_container(const slang::ast::ClassPropertySymbol& prop);

    bool contains(const slang::ast::ClassPropertySymbol& prop) const;

    bool has(const slang::ast::ClassPropertySymbol& prop, ir::Edge edge) const;

    bool is_container(const slang::ast::ClassPropertySymbol& prop) const;
};

// The pre-pass itself: every `@`/`wait` in the design, contributing the edges its
// operand's property needs. Runs once over the whole AST before any top is lowered.
struct WaitPropScan : slang::ast::ASTVisitor<WaitPropScan, VisitFlags::AllGood> {
    WaitedProps* out = nullptr;

    void handle(const slang::ast::SignalEventControl& ev);

    void handle(const slang::ast::WaitStatement& w);
};

void collect_class_types(const Scope& scope, std::vector<const ClassType*>& out);

bool scope_has_own_real_delay(const Scope& scope);

// Wheel-tick size (fs) for an instance subtree: the GCD (== minimum, since all
// timescale values are powers of ten) of every module's time *unit*, plus the
// time *precision* of every module that contains a real delay. Also reports
// whether any design unit set a timescale explicitly; the caller treats the
// result as unitless unless `any_explicit` is set, so a design with no directive
// keeps raw-tick time (LRM §3.14.2.3). Mirrors lower_instance's recursion over
// each scope's *effective* timescale (slang resolves inheritance / the default).
//
// Only a module with a real delay folds in its precision. With only integer
// delays every event is an exact multiple of its module's unit, so the precision
// terms never enter and the tick collapses to the minimum unit — identical to
// pre-real-delay behavior, keeping deadline magnitudes near the wheel's near-
// window (one revolution) instead of inflating them by unit/precision and
// spilling every timer into the O(log n) overflow heap. A real delay rounds to
// its module's precision (LRM 22.7), so folding that precision into the GCD makes
// the tick divide every real event time exactly while penalizing only designs
// (and only the modules) that actually use fractional delays.
uint64_t design_tick_fs(const InstanceSymbol& inst, bool& any_explicit);

uint64_t scope_min_tick_fs(const Scope& scope, bool& any_explicit);

uint64_t design_tick_fs(const InstanceSymbol& inst, bool& any_explicit);

uint64_t design_min_precision_fs(const InstanceSymbol& inst);

bool scope_declares_subroutine(const Scope& scope);

uint64_t global_scope_tick_fs(const Scope& scope, bool& any_explicit);

int32_t fs_to_unit_exponent(uint64_t fs);

ir::SignalInfo::Direction direction_of(slang::ast::ArgumentDirection d);

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
// (genvars, enum members) carry no sensitivity and are skipped, as are
// parameters — a parameter is not a variable (9.2.2.2, 9.4.2.2), and one that a
// runtime index forced into storage (Lowerer::const_table_signal) is in `sigmap`
// but still never changes.
struct SensitivityCollector : ASTVisitor<SensitivityCollector, VisitFlags::AllGood> {
    using SigMap = ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal>;

    const SigMap&                          sigmap;
    SensitivityMode                        mode;
    std::vector<ir::Signal>                reads;   // in first-seen order
    ankerl::unordered_dense::set<uint32_t> seen;    // dedup for `reads`
    ankerl::unordered_dense::set<uint32_t> written; // assignment targets / locals
    // User functions already descended into, so a function called from several
    // sites (or a recursive one) is walked at most once.
    ankerl::unordered_dense::set<const SubroutineSymbol*> entered;

    // Creates a STATIC class property's module-signal storage and returns it
    // (Lowerer::declare_static_prop). That storage is minted LAZILY -- at the first
    // reference, or eagerly only for a property that has an initializer -- so a
    // sensitivity walk resolving through `sigmap` alone finds nothing for a
    // property this walk is the first to reach, and drops the read SILENTLY. For a
    // `wait` that is an empty event list, i.e. a permanent park (LRM 9.4.3) whose
    // existence depends on the order the enclosing bodies happen to be lowered in.
    // Resolving here rather than in a pre-pass is what keeps the two exactly in
    // step: the pre-pass would have to re-derive this walk's descent rules.
    slang::function_ref<ir::Signal(const slang::ast::ClassPropertySymbol&)> declare_static;

    SensitivityCollector(const SigMap&                                                          m,
                         SensitivityMode                                                        mode,
                         slang::function_ref<ir::Signal(const slang::ast::ClassPropertySymbol&)> declare_static = {})
      : sigmap(m)
      , mode(mode)
      , declare_static(declare_static) {}

    std::optional<ir::Signal> lookup(const ValueSymbol& sym);

    void record_read(const ValueSymbol& sym);

    void mark_written(const ValueSymbol& sym);

    void record_write(const Expression& lhs);

    void handle(const NamedValueExpression& nv);

    void handle(const HierarchicalValueExpression& hv);

    void handle(const AssignmentExpression& a);

    void handle(const TimedStatement& t);

    void handle(const BlockStatement& blk);

    void handle(const UnaryExpression& u);

    void handle(const VariableDeclStatement& d);

    void handle(const ForLoopStatement& f);

    void handle(const CallExpression& c);
};

// Whether an expression may be evaluated even when its value turns out not to
// be needed. The one consumer is the `?:` strategy choice: LRM 11.4.11 forbids
// evaluating the unchosen arm of a DEFINITE condition, but for a speculation-safe
// arm the extra evaluation is unobservable and keeps the branchless packed select.
//
// This is a WHITELIST, and the polarity is the whole point. It used to be a
// blacklist of three AST node kinds (a call, an embedded assignment, an
// increment / decrement), which fails OPEN: every construct clotho lowers to a
// trapping runtime check silently became a wrong answer at this one site. Five
// such traps exist and not one of them is a call, an assignment or an increment --
// a null class-handle deref (`(h == null) ? -1 : h.x` fataled), a null
// virtual-interface deref (Ir.h's IfaceSignal), a read of a non-active
// tagged-union member (LRM 11.9, rt_tagged_access), a read of a stale `ref`
// formal (LRM 13.5.2, rt_ref_stale), and `new src`'s virtual dispatch through a
// possibly-null source. A whitelist fails CLOSED instead: an unlisted kind merely
// takes the branching path, which is correct for every expression by construction
// (lower_lazy_conditional is the only LRM-exact path). So the cost of being
// conservative is one SelectLazy, and the cost of being wrong is a fatal on legal
// code.
//
// Deliberately SAFE, because each has an LRM-defined value and no trap: an
// out-of-range index into a fixed / dynamic array or queue (x, or the element
// default -- LValue::dyn_inrange), a read of a missing associative-array key (the
// element default), integer divide / modulo by zero (all-x -- lower_divmod guards
// the divisor), real divide by zero (IEEE inf / NaN), and a string index out of
// range (0).
//
// The ambiguous-condition case is unaffected either way: LRM 11.4.11 Table 11-20
// requires BOTH arms there, and lower_lazy_conditional's x region evaluates both
// exactly as the eager select did.
struct SpeculationScan : ASTVisitor<SpeculationScan, VisitFlags::Expressions> {
    bool unsafe = false;

    // The generic hook. slang's ASTVisitor routes a node with no specific overload
    // through a templated handler, which is exactly what a whitelist needs: a
    // future ExpressionKind is unsafe until someone lists it. Children are visited
    // only after this node itself is cleared.
    template<typename T>
    void handle(const T& node) {
        if (unsafe) {
            return;
        }
        if constexpr (std::is_base_of_v<slang::ast::Expression, T>) {
            if (!node_safe(node)) {
                unsafe = true;
                return;
            }
        }
        visitDefault(node);
    }

    static bool node_safe(const slang::ast::Expression& e);

    static bool is_ref_formal(const slang::ast::Symbol& sym);

    static bool member_base_safe(const MemberAccessExpression& ma);
};

bool is_speculation_safe(const slang::ast::Expression& e);

// Whether an expression calls a user subroutine -- a function, a task or a class
// method, anything but a system function -- anywhere in its tree. The call's
// callee body is not part of the expression, so it is not walked.
struct UserCallScan : ASTVisitor<UserCallScan, VisitFlags::Expressions> {
    bool found = false;

    void handle(const CallExpression& call) {
        if (!call.isSystemCall()) {
            found = true;
            return;
        }
        visitDefault(call);
    }
};

bool has_user_call(const slang::ast::Expression& e);

// The module instance a scope depends on (its body, or the root for a scope outside
// every instance and package), or null for a package or `$unit` scope. See Scans.cpp.
const slang::ast::Scope* enclosing_instance_scope(const slang::ast::Scope* s);

// Whether the implicit return variable of a STATIC function (LRM 13.4.1, 13.4.2)
// has to be persistent storage. It keeps its value from call to call, but that is
// only observable if some call can see the value an earlier one left: the body
// reads it, or returns it, on a path that has not overwritten it first, or the
// function can be re-entered through its own body, where a nested call's value is
// what the outer one finds. A function that overwrites it on every path before
// any read, and cannot re-enter, behaves the same with a fresh cell per call, which
// costs nothing. Anything the analysis does not model counts as observable.
bool static_return_needs_storage(const slang::ast::SubroutineSymbol& sub);

// Detects whether an expression reads any symbol in a given set (used to find
// frame-local / formal-argument references in an event operand).
struct LocalRefDetector : ASTVisitor<LocalRefDetector, VisitFlags::AllGood> {
    const ankerl::unordered_dense::map<const slang::ast::ValueSymbol*, uint32_t>* locals;
    bool                                                                          found = false;

    void check(const slang::ast::ValueSymbol& sym);
    void handle(const NamedValueExpression& nv);
    void handle(const HierarchicalValueExpression& hv);
};

// Every value symbol an expression references. Used by BoxCandidateScan below, which
// needs the references of a fork child before any storage exists to filter them
// against.
struct ValueRefScan : ASTVisitor<ValueRefScan, VisitFlags::AllGood> {
    ankerl::unordered_dense::set<const slang::ast::ValueSymbol*>* out;
    // Set when a bare non-static class property is referenced, i.e. a read through the
    // enclosing method's implicit `this`. Optional: only BoxCandidateScan cares.
    bool* implicit_this = nullptr;

    void handle(const VariableDeclStatement& d);

    void handle(const CallExpression& call);

    void handle(const NamedValueExpression& nv);
    void handle(const HierarchicalValueExpression& hv);

    void take(const slang::ast::ValueSymbol& sym);
};

// Whether `blk` is a fork of any join kind. A LABEL changes neither this nor the
// shape below: `fork : guard a; b; join` still carries its children as the body's
// top-level statements, so keying on blockKind is what keeps a labelled fork -- the
// shape uvm_objection.svh:818 uses -- on the same path as a bare one.
inline bool is_fork_block(const BlockStatement& blk) {
    return blk.blockKind == StatementBlockKind::JoinAll || blk.blockKind == StatementBlockKind::JoinAny ||
           blk.blockKind == StatementBlockKind::JoinNone;
}

// A fork body's top-level statements, in source order: the members of its
// StatementList, or the single statement, or nothing at all for an empty body.
// A callback rather than a returned vector because every caller walks it once.
template <typename F>
inline void for_each_fork_stmt(const BlockStatement& blk, F&& fn) {
    if (blk.body.kind == StatementKind::List) {
        for (const Statement* s : blk.body.as<StatementList>().list) {
            fn(*s);
        }
    } else if (blk.body.kind != StatementKind::Empty) {
        fn(blk.body);
    }
}

// Symbols a fork's children reference anywhere in a subroutine body, which is the set
// whose frame locals have to be boxed (docs/design/fork-join.md V4). Run before the
// frame is laid out, because a boxed local is boxed for every access in the activation
// -- including accesses lowered before the `fork` is reached.
//
// Deliberately an over-approximation: it collects every reference, not just the ones
// that turn out to be this frame's locals, so it needs no notion of storage class at a
// point where storage does not exist yet. A module signal or a static in the set is
// never a frame local, so it never boxes.
//
// EVERY fork kind, `join` included. The storage a capture needs is not a property of
// the join kind -- it is a property of the capture, and the fat `ArgDir::Ref` ABI could
// carry only a packed integral or a dynamic array, which is why `join` refused a
// string / real / unpacked / queue / handle local outright. `join`'s "the parent
// outlives every child" also has one real counterexample: `kill` on a joining parent
// (docs/design/process-class.md §11), which left a child holding a pointer into a freed
// coroutine frame. One storage rule for all three kinds retires both problems, and
// costs nothing in a design whose forks capture nothing -- which includes every fork in
// a PROCESS body, where the parent's variables are module signals.
struct BoxCandidateScan : ASTVisitor<BoxCandidateScan, VisitFlags::AllGood> {
    ankerl::unordered_dense::set<const slang::ast::ValueSymbol*> candidates;
    // A child reads a class property of the enclosing method through the implicit
    // `this`. The handle it needs is the method's own `this` automatic, which is not a
    // ValueSymbol this scan can name (it hangs off the SubroutineSymbol), so it travels
    // as a flag and install_capture_boxes resolves it against `sub.thisVar`.
    bool implicit_this = false;

    // The declared variables of every fork body walked here -- the fork's OWN
    // block_item_declarations, which are not per-activation captures. Subtracted by
    // install_capture_boxes.
    ankerl::unordered_dense::set<const slang::ast::ValueSymbol*> fork_scope_decls;

    void handle(const BlockStatement& blk);
};

// Collects what a `fork` child body reads or writes out of the ENCLOSING
// subroutine's frame (LRM 9.3.2 with 13.4.2): a child is lowered into its own
// coroutine with its own frame, so every such symbol has to be passed in as a
// ref formal (lower_fork / build_fork_child). Deduped and kept in first-reference
// order, because that order IS the child's formal order.
//
// A bare property reference inside a class method captures the enclosing method's
// `this`, and nothing else: value_lvalue re-roots such a reference at the `this`
// automatic, so that automatic is the frame symbol the child needs. `this` is a real
// slang ValueSymbol (SubroutineSymbol::thisVar) living in localmap like any other
// automatic, so it is captured by exactly the same machinery -- an explicit `this.p`
// already arrived that way.
struct ForkCaptureDetector : ASTVisitor<ForkCaptureDetector, VisitFlags::AllGood> {
    const ankerl::unordered_dense::map<const slang::ast::ValueSymbol*, uint32_t>* locals;
    // Boxed captures are NOT in `locals`: a boxed declared local has no frame slot of
    // its own, precisely so nothing initializes the variable through a slot typed as
    // the box. They are still this frame's, so they still have to be captured.
    const ankerl::unordered_dense::set<const slang::ast::ValueSymbol*>* boxed = nullptr;
    // The enclosing method's `this` automatic, when there is one.
    const slang::ast::ValueSymbol*                                      this_var = nullptr;
    std::vector<const slang::ast::ValueSymbol*>                         captured;

    void check(const slang::ast::ValueSymbol& sym);
    void handle(const NamedValueExpression& nv);
    void handle(const HierarchicalValueExpression& hv);

    void handle(const VariableDeclStatement& d);

    void handle(const CallExpression& call);
};

// Detects everything an assertion action block may not contain, for
// Lowerer::require_reactive_safe and Lowerer::require_finish_inlinable. File
// scope rather than a local struct: the statement whitelist below has to be a
// member template, and a local class may not have one ([temp.mem]).
struct ReactiveSafetyChecker : ASTVisitor<ReactiveSafetyChecker, VisitFlags::AllGood> {
    // An action block is lowered TWICE, into two contexts that can run different
    // things, so this walk runs once per copy and is told which one it is looking
    // at. That split is the shape of the whole check: suspending used to
    // disqualify a construct outright, and now disqualifies it in one copy only.
    //
    //  * `Pended` is the copy a verdict hands to the Reactive region as its own
    //    coroutine (build_action_body, emit_action_pends). It MAY suspend. A
    //    parked coroutine records the region it parked from, and every wake path
    //    -- a static subscription, the timer wheel, a join record -- pushes the
    //    handle onto the ring that handle names rather than onto the Active one
    //    (emit_wake_push, src/ir/llvm/Scheduler.cpp), so a body that blocks on an
    //    event comes back in Reactive. LRM 24.3.1 asks for exactly that, and asks
    //    it of the THREAD rather than of the wake site -- said there of the region
    //    set's other inhabitant, a program block, which clotho already runs this
    //    way; 4.4.2.6 makes an action block a thread of the same region set, and
    //    4.4.2.7 is the special case of it for `#0`.
    //
    //  * `Inlined` is the end-of-trace copy of the FAIL action, and only a STRONG
    //    property has one: LRM 16.12.2 makes an attempt the trace ended
    //    underneath a failure, and Final is past the last time slot's Reactive
    //    drain, so emit_monitor_finish lowers the action straight into a Final
    //    process instead of pending a body nothing would resume. That process is
    //    resumed exactly once and then destroyed (emit_finals_run in
    //    ir/llvm/Scheduler.cpp), and no later region exists to bring it back -- so a
    //    suspension there drops everything after it without a word, which is the
    //    one outcome PRINCIPLES.md forbids outright.
    //
    // A nonblocking assignment and a nonblocking event trigger are the two
    // constructs that survive into the inlined copy while doing nothing there.
    // Both are correct rather than dropped: the region they would commit in is
    // past, and LRM 9.2.3 gives a `final` block the same deal, so an NBA cannot
    // commit there either.
    enum class Copy : uint8_t { Pended, Inlined };

    // Which of the two copies this walk is checking.
    Copy copy = Copy::Pended;
    // The first offending construct, empty while the walk stays clean.
    std::string reason;

    // Why a construct cannot appear in the copy being walked. Every rejection
    // names one, because "not supported" alone does not tell the reader what is
    // missing and therefore what would have to be built.

    // A suspension in the inlined copy. The remedy is spelled out because the
    // cause is not where the user is looking: the action block is fine, and it is
    // the property's STRENGTH that gives it a second home in Final.
    static constexpr std::string_view kFinalCopy =
      "a strong property makes an attempt still pending at the end of the trace a failure (LRM "
      "16.12.2), and that failure runs its action inside a `final` process, which is resumed "
      "exactly once and then destroyed -- so everything after the suspension would be dropped "
      "silently. Weaken the property or take the suspension out of the action block";

    void reject(std::string_view what, std::string_view why);

    void suspends(std::string_view what);

    // Templated on the concrete leaf type rather than taking a plain
    // `const Statement&`: slang's visitDefault only descends from a final class,
    // so a base-typed parameter could not continue the walk.
    template<std::derived_from<Statement> T>
    void handle(const T& s) {
        switch (s.kind) {
            // Zero-time and side-effect-local: control flow, blocking assignment,
            // and the display / severity calls an action block is written with.
            case StatementKind::Empty:
            case StatementKind::List:
            case StatementKind::Conditional:
            case StatementKind::Case:
            case StatementKind::PatternCase:
            case StatementKind::ExpressionStatement:
            case StatementKind::VariableDeclaration:
            case StatementKind::Break:
            case StatementKind::Continue:
            case StatementKind::ForLoop:
            case StatementKind::RepeatLoop:
            case StatementKind::ForeachLoop:
            case StatementKind::WhileLoop:
            case StatementKind::DoWhileLoop:
            case StatementKind::ProceduralAssign:
            case StatementKind::ProceduralDeassign:
                break;
            case StatementKind::Block:
                // A fork is a suspension and nothing more. Not the region: the join
                // record parks the parent as a handle and its wake carries that
                // handle's region, and a child is resumed inline at the spawn
                // (lower_fork_inst), so a child never goes through a ring at all.
                // And no longer the frame either: an action block is lowered as a
                // task body, and a child now reaches the enclosing body's locals
                // through ref formals (lower_fork). What is left is the inlined
                // copy's missing second resume, which is what suspends() names.
                //
                // `join_none` is the exception: it never parks the parent, which
                // walks straight past the fork, so it is not a suspension at all.
                // Its children DO go through a ring (they are pushed rather than
                // resumed inline), but each carries its own home region, which is
                // exactly what the parked-handle tag is for.
                if (const auto bk = s.template as<BlockStatement>().blockKind;
                    bk != StatementBlockKind::Sequential && bk != StatementBlockKind::JoinNone) {
                    suspends("a fork");
                }
                break;
            case StatementKind::ImmediateAssertion:
                if (s.template as<ImmediateAssertionStatement>().isDeferred) {
                    reject("a deferred immediate assertion",
                           "it matures in the Observed region, which reactive code is already "
                           "past, so the report would never be flushed");
                }
                break;
            case StatementKind::Timed:
                // Both flavours park the body as a handle, so both wakes know to
                // come back into Reactive: an event control (`@(posedge clk)`, `@e`,
                // `@(a or b)`) through an ordinary signal subscription, a delay
                // through the wheel. LRM 24.3.1 says the region travels with the
                // THREAD rather than with the wake site, which is what this relies
                // on; 4.4.2.7 is the special case of it for `#0`.
                suspends(s.template as<TimedStatement>().timing.kind == TimingControlKind::Delay
                           ? "a delay"
                           : "an event control");
                break;
            case StatementKind::Wait:
                // `wait (cond)` lowers to a level-sensitive park on every signal the
                // condition reads (lower_wait_stmt) -- subscriptions like any other,
                // so the wake knows where to come back to. An already-true condition
                // does not suspend at all, but this cannot tell which, and the
                // pended copy allows both.
                suspends("a wait");
                break;
            case StatementKind::WaitOrder:
                // `wait_order` desugars to a park per stage (lower_wait_order), so
                // it suspends exactly like a `wait` and the pended copy allows it.
                suspends("a wait_order");
                break;
            case StatementKind::WaitFork:
                // Lowerable everywhere else (LRM 9.6.1), but not here, and for the
                // identity reason `disable fork` is refused below: an action block is
                // pended as a task body and runs on the MONITOR process's control
                // block, so the descendants it would wait for are the monitor's.
                reject("a wait fork",
                       "the descendants it would wait for are the monitor process's, not the "
                       "assertion's: an action block runs on the monitor's thread");
                break;
            case StatementKind::ForeverLoop:
                // Still refused, and still not a region matter -- but the old reason
                // (a reactive body is resumed once) is no longer why. A body may now
                // be resumed as often as it suspends; what it may not do is never
                // finish. With no timing control inside it, the drain that resumed
                // it never gets control back at all; with one, the activation never
                // completes, so its coroutine frame is never released -- and there
                // is one activation per verdict that pends it.
                reject("a forever loop",
                       "it never returns, so the region drain that resumed it never regains "
                       "control and the body's frame is never released");
                break;
            case StatementKind::EventTrigger:
                // Allowed, both spellings. Neither suspends the process that writes
                // it, so neither needs a region of its own: `-> e` fires now and its
                // waiters land on the rings their own handles name, and `->> e`
                // stages a pending trigger that Re-NBA fires (re_nba.commit) with
                // the drain re-settling the Active set afterwards, so a waiter woken
                // by either still runs inside this time slot.
                break;
            case StatementKind::Disable:
                // Not the region, and no longer a form problem either: a `disable`
                // suspends nothing, so no wake is involved, and the remote form does
                // not care where it runs -- it marks the activations on a target's
                // list and pushes their handles. Both copies of an action block are
                // ordinary bodies to it (the pended one a task body, the inlined one a
                // Final process), and a block the action block itself opens is on the
                // same list footing as one in a task. lower_disable still refuses what
                // it refuses everywhere else.
                break;
            case StatementKind::DisableFork:
                // Lowerable everywhere else (LRM 9.6.3, docs/design/process-class.md
                // §8), but not here, and the reason is identity rather than region: an
                // action block is not a thread of its own -- it is pended as a task
                // body and runs on the monitor process's control block -- so the
                // descendants it would end are the monitor's, which is not what the
                // statement means.
                reject("a disable fork",
                       "the descendants it would end are the monitor process's, not the "
                       "assertion's: an action block runs on the monitor's thread");
                break;
            default:
                // The whitelist's floor, and what it now asks. Suspending is no
                // longer disqualifying, so "clotho cannot prove it is zero-time" is
                // no longer the requirement: what is left is that every suspension
                // the body reaches must park somewhere a reactive region brings it
                // back from, and that the inlined copy reaches none at all. An
                // unrecognized statement kind may fail either, and "the walk did not
                // recognize it" is no evidence that it does not.
                if (copy == Copy::Inlined) {
                    reject("a statement clotho cannot prove suspension-free", kFinalCopy);
                } else {
                    reject("this statement",
                           "clotho cannot tell whether it suspends, and a suspension is correct in "
                           "reactive code only where the wake that ends it names the region to come "
                           "back into");
                }
                break;
        }
        visitDefault(s);
    }

    void handle(const AssignmentExpression& a);

    void handle(const CallExpression& c);
};

// The verdict of LRM 16.14.6 clock inference, per concurrent assertion: which
// event control slang picked as the assertion's clock. Both sides are SYNTAX
// nodes, not AST nodes, and that is the whole point of the table.
//
// slang analyses an instance body once and skips every instance whose body is an
// exact duplicate of it (InstanceSymbol::getCanonicalBody), while clotho lowers
// each instance's own body -- distinct Statement and ValueSymbol objects per
// instance. So an AST-keyed table would miss for every duplicate, and its AST
// *values* would name the canonical instance's `clk` rather than this one's. The
// syntax is shared by construction, so keying and answering in syntax makes the
// verdict transferable, and the Lowerer re-resolves it against the procedure it
// is actually lowering (see lower_concurrent_assertion).
//
// A null value means "two elaborations of this same source disagreed": the
// verdict is then unusable and the assertion is refused rather than guessed at.
//
// Filled by lower_compilation from the analysis pass that already runs for the
// dataflow diagnostics, and read rather than recomputed: inference is a
// whole-procedure dataflow question -- 16.14.6 requires that no term of the
// candidate event appear anywhere else in the procedure -- so the
// statement-local walk a Lowerer does cannot answer it.
using InferredClockMap =
  ankerl::unordered_dense::map<const slang::syntax::SyntaxNode*, const slang::syntax::SyntaxNode*>;

// Design-wide class type ids (LRM 8.16). One id per slang ClassType, minted on
// demand: a `$cast` compares ids, so the same class reached through two Lowerers
// -- which intern it as two nominal Refs (see class_type_cache_) -- must still
// answer with one id. Nothing orders or packs the ids, so minting one needs no
// view of the design beyond the class itself.
struct ClassIds {
    ankerl::unordered_dense::map<const slang::ast::ClassType*, uint32_t> ids;
    // 0 is reserved: it is the display padding past a class's own depth and the
    // reading a null handle's stand-in object gives, both of which must match no
    // class at all.
    uint32_t next = 1;
};

// Design-wide interface-class registry (LRM 8.26). Two numbering spaces, both
// minted on demand and for the same reason ClassIds is: an entry is a bit or an
// index and nothing more, so minting one needs no view of the design and no
// whole-program numbering pass.
//
//  * `ids` -- one dense id per interface ClassType. It is the bit this interface
//    occupies in every implementing class's descriptor bitmap, which is what a
//    `$cast` to an interface type tests. Dense from 0 (unlike a class type id,
//    which reserves 0 for "no class") because a bitmap has no padding entry.
//  * `slots` -- one dense index per interface method NAME. Name keying is LRM
//    8.26.6.1: one implementation satisfies every same-named prototype of every
//    interface a class implements, and two prototypes of one name with different
//    signatures make the class illegal. Two UNRELATED classes may still hold
//    different signatures at one index; that is safe because a call site takes its
//    signature from the interface's own prototype and the receiver is guaranteed
//    to implement that interface.
//
// Shared by every Lowerer, like ClassIds: one interface has one id and one method
// name has one index across the design, even where two tops intern the interface
// as two nominal Refs.
struct IfaceIds {
    ankerl::unordered_dense::map<const slang::ast::ClassType*, uint32_t> ids;
    ankerl::unordered_dense::map<std::string_view, uint32_t>             slots;
};

// Design-wide registry backing virtual interfaces (LRM 25.9).
//
// slang resolves `vif.sig` against a PLACEHOLDER instance body it caches once per
// (interface definition, parameter values) -- never against a real elaborated
// instance -- so the member symbol a vif access hands over is in no `sigmap_`.
// `insts` records every real interface instance that was lowered, with its scope
// and its own members by name; a placeholder is matched to one of them by
// `InstanceBodySymbol::hasSameType`, which is exactly the LRM's type identity
// (same definition, all parameter values equal). That REPRESENTATIVE supplies the
// def-local field index; the handle supplies the instance.
//
// Design-wide, and shared by every Lowerer, because the interface is elaborated
// under one top while the class method that dereferences the handle may be
// lowered by another (a package-scope class body lowers on the shared Lowerer).
// One clockvar of an interface's clocking block, as reachable through a handle:
// the sampled storage a read yields and the two slots a synchronous drive writes
// (LRM 14.16). All three are ordinary per-instance signals in the interface's
// scope, so the handle addresses them like any other member.
struct VifClockVar {
    ir::Signal sampled{ 0 };
    ir::Signal drive_val{ 0 };
    ir::Signal drive_pend{ 0 };
    bool       drives = false;
};

struct VifInstance {
    const slang::ast::InstanceBodySymbol* body  = nullptr;
    uint32_t                              scope = 0;
    // Body-level signals by name, plus each clocking block's event under the
    // block's own name. Keys are OWNED strings because a clockvar's key is
    // qualified ("cb.d"): an unqualified one would collide with a body-level `d`
    // and silently hand back the live signal instead of the sampled one.
    ankerl::unordered_dense::map<std::string, ir::Signal>  members;
    ankerl::unordered_dense::map<std::string, VifClockVar> clockvars;
    // The same body-level members by SYMBOL. A subroutine of this interface lowered
    // for handle dispatch resolves its signal references through sigmap_, which
    // belongs to whichever Lowerer does the lowering -- possibly not the one that
    // elaborated this instance. Seeding from here is what makes that work.
    ankerl::unordered_dense::map<const slang::ast::ValueSymbol*, ir::Signal> syms;
};

struct VifDefs {
    std::vector<VifInstance> insts;
    // Placeholder bodies named by a `virtual` declaration somewhere in the design.
    std::vector<const slang::ast::InstanceBodySymbol*> wanted;
};

// Walks the elaborated AST of one top module into an ir::Module.
class Lowerer {
public:
    // `tick_fs` is the femtoseconds-per-wheel-tick (design-wide smallest time
    // unit), or 0 when no design unit set a timescale (unitless ticks).
    // `sources` shows the file names the simulation's banners print.
    Lowerer(ir::Module&          module,
            const SourceContext& sources,
            uint64_t             tick_fs,
            ir::SystemAccess     allow_system = ir::SystemAccess::Off)
      : module_(module)
      , tick_fs_(tick_fs)
      , allow_system_(allow_system)
      , sources_(sources) {}

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

    void lower_instance(const InstanceSymbol& inst,
                        std::string_view      prefix,
                        Pass                  pass,
                        ir::InstanceScope*    node,
                        std::string_view      leaf_name);

    void collect_iface_members(const slang::ast::Scope& scope, VifInstance& rec, bool top_level);

    static std::vector<uint32_t> member_ordinal_path(const slang::ast::Symbol& m,
                                                     const slang::ast::Scope&  body);

    static const slang::ast::Symbol* symbol_at_path(const slang::ast::Scope&     body,
                                                    const std::vector<uint32_t>& path);

    void register_iface_instance(const slang::ast::InstanceBodySymbol& body, uint32_t scope);

    std::optional<VifClockVar> vif_clockvar(const slang::ast::ClockVarSymbol&     cv,
                                            const slang::ast::InstanceBodySymbol* named,
                                            std::string_view                      what);

    ir::Index vif_null_place(const slang::ast::Type& type, ir::Builder& b, ir::SourceLoc l);

    uint32_t iface_scope_of(const InstanceSymbol& inst);

    static const slang::ast::InstanceBodySymbol* vif_named_body(const Expression& handle);

    std::optional<ir::Signal> vif_member_signal(const slang::ast::Symbol&             member,
                                                const slang::ast::InstanceBodySymbol* named,
                                                std::string_view                      what);

    void lower_top(const InstanceSymbol& top, std::string_view prefix);

    void lower_globals(const Compilation& comp, const RootSymbol& root);

    void finalize_globals();

    void init_static_class_props(const Scope& scope);

    // The same for the package / `$unit` generics `top` specializes with a class one
    // of its modules declares: those belong to the module (is_global_scope), so the
    // globals pass skips them and this one runs them on the top that owns them.
    void init_local_class_specializations(const InstanceSymbol& top);

    void init_static_props_of(const std::vector<const ClassType*>& classes);

    const ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal>& sigmap() const {
        return sigmap_;
    }

    void seed_signals(const ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal>& seed);

    // Point this top's Lowerer at the shared globals Lowerer (the one
    // lower_globals ran on), so a package/$unit-scope subroutine call can
    // delegate its lowering there instead of duplicating it per top (see
    // function_id's is_global_subroutine check). `g` must outlive every call
    // made through this Lowerer.
    void set_globals(Lowerer* g) {
        globals_ = g;
    }

    // Point this Lowerer at the inference verdicts the analysis pass produced
    // (LRM 16.14.6). Null when the pass did not run -- lowering then only accepts
    // an explicit clocking event, which is a refusal rather than a wrong answer.
    // `clocks` must outlive every call made through this Lowerer.
    void set_inferred_clocks(const InferredClockMap* clocks) {
        inferred_clocks_ = clocks;
    }

    // Whether `--allow-port-coercion` is given (LRM 23.3.3.1).
    void set_allow_port_coercion(bool allow) {
        allow_port_coercion_ = allow;
    }

    // Point this Lowerer at the design-wide class id table. `ids` must outlive
    // every call made through this Lowerer.
    // Design-wide: some assignment can retarget an event variable, so no event's
    // handle is provably fixed. A static sensitivity subscription is issued once
    // from the seed loop and can never re-resolve, so with merging in the design an
    // event sensitivity list must be left in the body instead, where it re-reads
    // the handle on every pass (LRM 15.5.5: a re-armed wait uses the CURRENT
    // object; only an already-parked waiter stays where it is).
    void set_events_merge(bool v) {
        events_merge_ = v;
    }

    // Point this Lowerer at the design-wide waited-on-property table (WaitPropScan).
    // Shared for the same reason ClassIds is, and consulted at exactly one place:
    // lower_class_type, laying out a class's change notifiers.
    void set_waited_props(const WaitedProps* wp) {
        waited_props_ = wp;
    }

    void set_class_ids(ClassIds* ids) {
        class_ids_ = ids;
    }

    // Point this Lowerer at the design-wide interface-class registry (LRM 8.26).
    // Shared for the same reason ClassIds is: one interface must get one bit and
    // one interface method name one index, whichever Lowerer sees it first.
    void set_iface_ids(IfaceIds* ids) {
        iface_ids_ = ids;
    }

    // Point this Lowerer at the design-wide virtual interface registry. `defs`
    // must outlive every call made through this Lowerer.
    void set_vif_defs(VifDefs* defs) {
        vif_defs_ = defs;
    }

    // Diagnostics for members this Lowerer recovered past, and whether it did so
    // at all. Drained by lower_compilation once the walk finishes.
    [[nodiscard]] std::span<const Diagnostic> recovered_diags() const {
        return diags_;
    }
    [[nodiscard]] bool poisoned() const {
        return poisoned_;
    }

    // Whether this Lowerer lowered a $dumpvars. A whole-design dump has to be
    // able to represent every declared signal, and that is only answerable once
    // every top is lowered, so lower_compilation collects this and runs the
    // per-signal check itself (guard_vcd_representable).
    [[nodiscard]] bool dumps_vcd() const {
        return dumps_vcd_;
    }

private:
    void lower_members(const Scope&       scope,
                       const std::string& self_prefix,
                       uint64_t           self_mul,
                       Pass               pass,
                       ir::InstanceScope& node,
                       size_t&            child_cursor);

    void lower_generate_block(const GenerateBlockSymbol& block,
                              const std::string&         block_prefix,
                              uint64_t                   self_mul,
                              Pass                       pass,
                              ir::InstanceScope&         node,
                              size_t&                    child_cursor);

    void lower_instance_array(const InstanceArraySymbol& arr,
                              const std::string&         self_prefix,
                              const Scope&               parent_scope,
                              uint64_t                   self_mul,
                              Pass                       pass,
                              ir::InstanceScope&         node,
                              size_t&                    child_cursor);

    static std::string instance_array_element_name(const InstanceSymbol& elem);

    static std::string gen_entry_name(const GenerateBlockArraySymbol& array, const GenerateBlockSymbol& entry);

    // Source extent of the construct currently being lowered, tracked at the
    // statement / expression / member-declaration dispatch boundaries so an
    // unsupported-construct guard can point the user at the offending construct
    // without every one of the ~290 `fail` sites threading a location by hand.
    // A range rather than a point so a diagnostic can highlight the whole
    // construct; `cur_loc()` is the start, for the runtime banner helpers.
    slang::SourceRange cur_range_{};

    [[nodiscard]] slang::SourceLocation cur_loc() const {
        return cur_range_.start();
    }

    void report(Severity sev, std::string code, std::string msg, ir::SourceSpan span);

    [[noreturn]] void fail(std::string msg);

    // Run one behavioural member's lowering, and on a guard failure record the
    // diagnostic and carry on with the next member instead of abandoning the
    // whole design. This is what turns "one error per invocation" into "every
    // error in the file", which is the difference between a compiler and an
    // editor backend.
    //
    // Safe here, and NOT safe in general, for three specific reasons:
    //
    //  * No walk state to restore. `prefix_`, `scope_`, `cur_pass_` and
    //    `prec_over_tick_` are written only by the structural cases
    //    (lower_instance / lower_generate_block / the Instance arm), never by a
    //    behavioural member, and `cur_range_` is reset at the top of every
    //    iteration of the member loop. So an abandoned member leaves the walk
    //    exactly where it was.
    //
    //  * No partial process. A procedural block, continuous assign or gate
    //    builds into an `ir::Builder` local to its own lowering function and
    //    appends the process only at the very end, so a throw discards the
    //    partial body wholesale.
    //
    //  * Except for `preinit_`, which every declaration initializer and every
    //    lazily-emitted static class property initializer SHARES across the
    //    scope. That one is rolled back explicitly, because leaving a
    //    half-emitted expression in it would at best emit dead instructions and
    //    at worst leave a region open — and Builder::finish() folds an unclosed
    //    region silently into the root body rather than complaining.
    //
    // Structural members are deliberately excluded by the caller: they recurse
    // and advance `child_cursor`, so recovering mid-recursion would misalign the
    // InstanceScope tree and mis-attribute every later member. That is silently
    // wrong analysis, strictly worse than the hard failure they keep.
    template<typename Fn>
    void recover(Fn&& fn) {
        const bool                             had_preinit = preinit_.has_value();
        const std::optional<ir::Builder::Mark> pre = had_preinit ? std::optional{ preinit_->mark() } : std::nullopt;
        try {
            fn();
        } catch (const LowerError& e) {
            if (had_preinit) {
                preinit_->rollback(*pre);
            } else if (preinit_.has_value()) {
                // The member created it and then failed, so everything in it is
                // that member's partial emission.
                preinit_.reset();
            }
            // The throw skipped the Block case's pops, so entries for the blocks
            // it was inside are still stacked. A stale entry would extend a later
            // %m and, worse, make a later `disable` believe it is inside a block
            // it is not (lower_disable searches this stack).
            named_blocks_.clear();
            diags_.push_back(Diagnostic{ .severity = Severity::Error,
                                         .origin   = DiagnosticOrigin::Unsupported,
                                         .code     = "unsupported-construct",
                                         .message  = e.message,
                                         .span     = e.span });
            poisoned_ = true;
        }
    }

    ir::SourceLoc loc(const Expression& e) const;
    ir::SourceLoc loc(const Statement& s) const;

    ir::Index lower_index(const Expression& e, ir::Builder& b);

    static ir::SourceSpan span_of(const Symbol& sym);

    std::string finish_banner(const Expression& call, std::string_view task) const;

    std::string banner_at(slang::SourceLocation sl) const;

    std::string severity_banner(slang::SourceLocation sl) const;

    std::string case_banner(const CaseStatement& c) const;

    // -- Types --

    void guard_agg_key_leaves(const slang::ast::Type& kt, ir::Ref kr);

    bool key_type_is_variable(ir::Ref type) const;

    bool key_type_has_class_handle(ir::Ref type) const;

    void guard_key_no_handle_in_variable(ir::Ref key_ty);

    void guard_container_element(const slang::ast::Type& et, ir::Ref er, std::string_view what);

    ir::Ref lower_type(const slang::ast::Type& type);

    // One `rand` / `randc` property of a class, as the generated body sees it.
    struct RandProp {
        const slang::ast::ClassPropertySymbol* sym = nullptr;
        // The property's own field index in the object body.
        uint32_t field = 0;
        // Its bit in the object's rand_mode mask (LRM 18.8).
        uint32_t ordinal = 0;
        // `randc` (LRM 18.6): cyclic, and `randc_field` is the body field index of
        // its 256-bit already-used bitmap.
        bool     cyclic      = false;
        uint32_t randc_field = 0;
        uint32_t randc_vals  = 0;
    };
    // A class's rand block: the appended body fields plus the flattened rand-property
    // table of its whole inheritance chain. `present` is false for a class that
    // declares no rand property anywhere in its chain -- such a class carries no
    // block at all and its synthesized bodies are the trivial ones.
    struct ClassRand {
        bool                  present    = false;
        uint32_t              rng_field  = 0;
        uint32_t              mode_field = 0;
        std::vector<RandProp> props;
        // Non-empty when the chain carries a randomization surface step 1 cannot
        // honour (a constraint block, a rand property of an undrawable type): the
        // reason, verbatim. Such a class lowers -- eager static initialization
        // (LRM 6.21) lowers every factory-registered type whether or not anything
        // randomizes it, and UVM's register layer carries constraints in classes
        // most designs never randomize -- but its generated $randomize body is a
        // run-time fatal carrying this message, never a silent unconstrained draw.
        // The body is the one point every randomize() of the class must reach
        // (dispatch is virtual), so a base-typed handle is caught too.
        std::string unsupported;
    };

    void bind_class_type(const slang::ast::ClassType& clt, ir::Ref ref);

    ir::Ref lower_class_type(const slang::ast::ClassType& clt);

    uint32_t reserve_change_notifiers(const slang::ast::ClassPropertySymbol& prop,
                                      ir::Ref                                class_ref,
                                      uint32_t                               prop_field,
                                      uint32_t                               next);

    void emit_change_notifier_fields(const slang::ast::ClassPropertySymbol& prop,
                                     std::vector<ir::FieldRow>&             fields,
                                     std::deque<std::string>&               names);

    RandProp admit_rand_prop(const slang::ast::ClassPropertySymbol& prop, uint32_t field, uint32_t ordinal);

    static const char* rand_type_unsupported(const slang::ast::Type& ty);

    ir::Ref rng_state_type();

    static bool has_constraint_blocks(const slang::ast::ClassType& clt);

    ir::Ref lower_process_class(const slang::ast::ClassType& clt);

    ir::Ref lower_semaphore_class(const slang::ast::ClassType& clt);

    static bool is_sync_method(const slang::ast::SubroutineSymbol& sub);

    static bool is_sync_class(const slang::ast::ClassType& clt);

    ir::Ref lower_mailbox_class(const slang::ast::ClassType& clt);

    static bool is_process_method(const slang::ast::SubroutineSymbol& sub);

    static bool is_user_virtual(const slang::ast::SubroutineSymbol& m);

    static const slang::ast::ClassType* base_class_of(const slang::ast::ClassType& clt);

    uint32_t class_type_id(const slang::ast::ClassType& clt);

    uint32_t iface_id(const slang::ast::ClassType& clt);

    uint32_t iface_slot(std::string_view name);

    std::vector<const slang::ast::ClassType*> iface_closure(const slang::ast::ClassType& clt);

    const slang::ast::MethodPrototypeSymbol* find_pure_proto(const slang::ast::ClassType& clt,
                                                             std::string_view             name);

    // A class's method NAME -> vtable slot map (LRM 8.20). Declared here, ahead of
    // compute_vtable and ensure_vtable, because it names ensure_vtable's return type
    // and a member alias is not visible in a signature above its own declaration.
    using VtableSlots = ankerl::unordered_dense::map<std::string_view, uint32_t>;

    void compute_vtable(const slang::ast::ClassType& clt, ir::Ref class_ref);

    std::pair<std::vector<ir::FunctionId>, std::vector<uint32_t>> build_iface_table(
      const slang::ast::ClassType&       clt,
      const VtableSlots&                 slots,
      const std::vector<ir::FunctionId>& table);

    ir::FunctionId iface_unreachable_id();

    ir::FunctionId pure_stub_id(const slang::ast::MethodPrototypeSymbol& proto);

    void build_pure_stub(const slang::ast::MethodPrototypeSymbol& proto, ir::FunctionId id);

    const VtableSlots& ensure_vtable(const slang::ast::ClassType& clt);

    uint32_t virtual_slot_of(const slang::ast::SubroutineSymbol& sub);

    static const slang::ast::ClassType& declaring_class(const slang::ast::SubroutineSymbol& sub);

    ir::FunctionId virtual_sig_id(const slang::ast::SubroutineSymbol& sub);

    static bool is_super_call(const slang::ast::CallExpression& call);

    static bool is_unpacked_array(const slang::ast::Expression& e);

    static bool is_unpacked_array_type(const slang::ast::Type& t);

    static bool is_unpacked_struct(const slang::ast::Type& t);

    static bool is_agg_abi_type(const slang::ast::Type& t);

    static bool is_aggregate(const slang::ast::Type& t);

    static bool is_unpacked_container(const slang::ast::Type& t);

    // Already-lowered nonblocking intra-assignment timing (LRM 9.4.5), threaded
    // beside the store tag through every decomposition walker. A target that
    // decomposes into N leaves -- `{a,b} <= #5 x`, a streaming or
    // assignment-pattern lvalue, a whole unpacked struct or array -- then arms N
    // deferred records against ONE sampled RHS and ONE evaluation of the delay /
    // repeat count, which is what 9.4.5's "the right-hand expression shall be
    // evaluated before the delay" requires of the decomposed form too.
    //
    // A plain pointer, not a leaf-emitter callback: every ordinary assignment in
    // the design walks this same code, so the cost of the feature has to be one
    // null test per leaf rather than an indirect call. Null is the ordinary path,
    // and is what every caller but the nonblocking intra-assignment one passes.
    struct IntraNbTiming {
        // `<= #d` form: the scaled delay in wheel ticks.
        ir::Index delay{};
        // `<= #d` form with a delay that may be zero at run time: the `delay == 0`
        // predicate that routes the store to the current instant's NBA region as a
        // plain `<=` instead (LRM 10.4.2, 4.9.4). Empty when the delay is a known
        // nonzero constant.
        std::optional<ir::Index> zero{};
        // `<= @e` / `<= repeat(N) @e` form: the countdown, sampled now.
        ir::Index count{};
        // `repeat(N) @e` with a runtime SIGNED or real count: the `N <= 0`
        // predicate that routes the store to the current instant instead (LRM
        // 9.4.5). Empty when the decision folded at lower time, or when the count
        // is unsigned and so has no non-positive value to test for.
        std::optional<ir::Index> nonpos{};
        // The (signal, edge) triggers the event form waits on, packed once into the
        // builder's extra buffer so every leaf of a decomposing target shares one
        // list. Several triggers means `@(a or b)`: the first to bring the countdown
        // to zero matures the store.
        ir::ExtraSlice trigs{};
        bool           is_event = false;
    };

    void store_leaf(ir::Inst::Tag        tag,
                    const IntraNbTiming* t,
                    ir::Index            lv,
                    ir::Index            v,
                    ir::Builder&         b,
                    ir::SourceLoc        l);

    void emit_struct_store(ir::Index            lhs_base,
                           ir::Ref              sty,
                           const Expression&    rhs,
                           ir::Inst::Tag        tag,
                           ir::Builder&         b,
                           ir::SourceLoc        l,
                           const IntraNbTiming* nbt = nullptr);

    static bool struct_has_field_defaults(const slang::ast::Type& t);

    void emit_struct_field_defaults(ir::Index               lhs,
                                    ir::Ref                 sty,
                                    const slang::ast::Type& sv_type,
                                    ir::Builder&            b,
                                    ir::SourceLoc           l);

    static bool aggregate_has_field_defaults(const slang::ast::Type& t);

    void emit_var_field_defaults(const VariableSymbol& vs, ir::Index lv, ir::Builder& b, ir::SourceLoc l);

    void emit_string_literal_array_store(ir::Index        lhs,
                                         ir::Ref          type,
                                         const ir::Type&  t,
                                         std::string_view text,
                                         ir::Inst::Tag        tag,
                                         ir::Builder&         b,
                                         ir::SourceLoc        l,
                                         const IntraNbTiming* nbt = nullptr);

    void emit_agg_store(ir::Index            lhs,
                        ir::Ref              type,
                        const Expression&    rhs,
                        ir::Inst::Tag        tag,
                        ir::Builder&         b,
                        ir::SourceLoc        l,
                        const IntraNbTiming* nbt = nullptr);

    const ConditionalExpression* as_agg_conditional(const Expression& rhs, ir::Ref type) const;

    bool conditions_may_be_ambiguous(std::span<const ConditionalExpression::Condition> conds);

    void emit_agg_conditional(ir::Index                    lhs,
                              ir::Ref                      type,
                              const ConditionalExpression& c,
                              ir::Inst::Tag                tag,
                              ir::Builder&                 b,
                              ir::SourceLoc                l);

    void emit_agg_cond_ambiguous(ir::Index                    lhs,
                                 ir::Ref                      type,
                                 const slang::ast::Type&      sv_type,
                                 const ConditionalExpression& c,
                                 ir::Inst::Tag                tag,
                                 ir::Builder&                 b,
                                 ir::SourceLoc                l);

    void emit_agg_cond_elementwise(ir::Index               lhs,
                                   ir::Ref                 type,
                                   const slang::ast::Type& sv_type,
                                   ir::Index               ta,
                                   ir::Index               tb,
                                   ir::Inst::Tag           tag,
                                   ir::Builder&            b,
                                   ir::SourceLoc           l);

    ir::Index agg_cond_elem_match(ir::Index a, ir::Index other, ir::Ref ety, ir::Builder& b, ir::SourceLoc l);

    void emit_table7_1_default(ir::Index               lv,
                               ir::Ref                 ty,
                               const slang::ast::Type& sv_type,
                               ir::Inst::Tag           tag,
                               ir::Builder&            b,
                               ir::SourceLoc           l);

    void emit_handle_literal_init(ir::Index                          lhs,
                                  ir::Ref                            type,
                                  const ir::Type&                    t,
                                  std::span<const Expression* const> elems,
                                  uint32_t                           reps,
                                  ir::Inst::Tag                      tag,
                                  ir::Builder&                       b,
                                  ir::SourceLoc                      l);

    void emit_assoc_literal_init(ir::Index         lhs,
                                 ir::Ref           type,
                                 const Expression& rhs,
                                 ir::Inst::Tag     tag,
                                 ir::Builder&      b,
                                 ir::SourceLoc     l);

    ir::Index lower_assoc_value(const Expression&       e,
                                const slang::ast::Type& elem,
                                ir::Builder&            b,
                                ir::SourceLoc           l);

    void emit_agg_copy(ir::Index            lhs,
                       ir::Index            rhs,
                       ir::Ref              type,
                       ir::Inst::Tag        tag,
                       ir::Builder&         b,
                       ir::SourceLoc        l,
                       const IntraNbTiming* nbt = nullptr);

    void emit_handle_member_copy(ir::Index         lhs,
                                 const Expression& rhs,
                                 ir::Ref           type,
                                 ir::Inst::Tag     tag,
                                 ir::Builder&      b,
                                 ir::SourceLoc     l);

    void emit_port_agg_copy(ir::Index dst, ir::Index src, ir::Ref ty, ir::Builder& b, ir::SourceLoc l);

    static bool is_whole_array_lvalue(const Expression& e);

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
    ValueCat value_cat(ir::Ref type) const;

    bool is_leaf_ref(ir::Ref type) const;

    bool contains_struct_ref(ir::Ref type) const;

    bool is_agg_abi_ref(ir::Ref type) const;

    bool is_trivially_copyable(ir::Ref type) const;

    static const Expression& unwrap_lvalue_elem(const Expression& e);

    static const slang::ast::AssignmentPatternExpressionBase* as_assignment_pattern(const Expression& e);

    ir::Index lower_packed_pattern(const Expression& expr, ir::Builder& b);

    ir::Index lower_struct_eq(const Expression& lhs, const Expression& rhs, ir::Builder& b, ir::SourceLoc l);

    void guard_agg_eq_members(ir::Ref type);

    bool case_eq_matches_logical(ir::Ref type) const;

    bool assoc_value_may_carry_xz(ir::Ref type) const;

    static const Expression& unwrap_conversion(const Expression& e);

    static bool is_struct_lvalue_shape(const Expression& e);

    static bool is_dyn_array(const slang::ast::Expression& e);

    static bool is_queue(const slang::ast::Expression& e);

    static bool is_assoc(const slang::ast::Expression& e);

    static bool is_assoc_element_select(const slang::ast::Expression& e);

    static bool is_string_expr(const slang::ast::Expression& e);

    static bool is_string_nba_place(const Expression& e);

    bool is_string_ref(ir::Ref ty) const;

    bool is_dyn_array_ref(ir::Ref ty) const;

    bool is_handle_ref(ir::Ref ty) const;

    static bool is_handle_expr(const slang::ast::Expression& e);

    ir::Ref string_value_ref(std::string_view text);

    ir::Ref make_int_type(uint32_t width, bool is_four, bool is_signed);

    // Whether an expression slang can fold is allowed to be folded, which depends
    // on what the site does with it.
    //
    // Required: the grammar demands a constant expression here (a part-select
    // bound, a replication count, a bin bound, a `$past` tick count, ...), so
    // slang has already diagnosed anything else, and a constant function call is
    // legal in it. LRM 13.4.3: "The execution of a constant function call has no
    // effect on the initial values of the variables used either at simulation
    // time or among multiple invocations of a function at elaboration time", so
    // slang's evaluation, every call on fresh variables, is the right one.
    //
    // Runtime: an ordinary run-time expression the lowering tries to specialize
    // when it happens to fold (a case label, a delay, a selector, a gate input).
    // A call to a user subroutine in it has to run each time the expression is
    // evaluated: it may read and write static variables, including its own
    // implicit return variable (LRM 13.4.2), and write module state. slang's
    // evaluator gives every call fresh variables and ignores the side effects,
    // so such an expression is never folded, and the site takes its run-time
    // path.
    enum class Fold : uint8_t { Required, Runtime };

    // Which operand's signedness picks the fill bits of an integral WIDENING.
    // The LRM gives two different answers and the IR's Resize instruction
    // implements the second one: it extends according to the signedness of its
    // TARGET type.
    enum class Extend : uint8_t {
        // LRM 11.8.3 (assignment): "extend the size of the right-hand side,
        // performing sign extension if, and only if, the type of the right-hand
        // side is signed" — the SOURCE decides. LRM 6.24.1 puts a cast under the
        // same rule: a cast "shall return the value that a variable of the
        // casting type would hold after being assigned the expression".
        Source,
        // LRM 11.8.2, last step: a context-determined operand "shall be
        // converted to the propagated type and size... sign-extended only if the
        // propagated type is signed" — the TARGET decides. The two rules really
        // do disagree, in both directions: with `logic [7:0] u; logic signed
        // [3:0] s = -1`, `u + s` zero-extends s (16), while `u = s` sign-extends
        // it (255). Both iverilog 13 and Verilator 5.050 agree on both.
        Target,
    };

    ir::Index coerce(ir::Index               v,
                     const slang::ast::Type& from,
                     ir::Ref                 to,
                     ir::Builder&            b,
                     ir::SourceLoc           l,
                     Extend                  ext = Extend::Source);

    ir::Index extend_by_source_sign(ir::Index               v,
                                    const slang::ast::Type& from,
                                    ir::Ref                 to,
                                    ir::Builder&            b,
                                    ir::SourceLoc           l);

    ir::Index narrow_to_declared(ir::Index v, const slang::ast::Type& declared, ir::Builder& b, ir::SourceLoc l);

    ir::Ref lower_int_literal(const IntegerLiteral& lit);

    ir::Ref svint_to_ref(const slang::SVInt& v, ir::Ref ty);

    ir::Ref lower_constant(const slang::ConstantValue& cv, const slang::ast::Type& type);

    // Refuses a defparam that gives an implicitly typed parameter a value of
    // another type (see the NOTE at the definition), recording the parameter in
    // refused_params_.
    void check_defparam_type(const slang::ast::DefParamSymbol& d);

    ir::Ref lower_param_constant(const slang::ast::ParameterSymbol& p, const slang::ast::Type& type);

    void append_svint_bytes(const slang::SVInt& src, ir::Ref ty, std::vector<uint8_t>& out);

    void append_const_image(const slang::ConstantValue& cv, ir::Ref ty, std::vector<uint8_t>& out);

    bool has_heap_leaf(ir::Ref ty);

    bool is_scalar_element(ir::Ref ty);

    void emit_const_seed(ir::Index                   lhs,
                         ir::Ref                     ty,
                         const slang::ast::Type&     sv_type,
                         const slang::ConstantValue& cv,
                         ir::Builder&                b,
                         ir::SourceLoc               l);

    // -- Signals --

    void register_global_scope_signals(const Scope& scope);

    void init_global_scope_signals(const Scope& scope);

    void build_inout_aliases(const InstanceSymbol& inst);

    bool needs_port_coercion(const PortConnection& pc, const PortSymbol& p);

    bool net_is_read(const ValueSymbol& sym);

    bool port_driven_inside(const PortSymbol& p);

    bool other_driver_of(const Expression& own_target);

    void scan_net_uses();

    void collect_signals(const InstanceBodySymbol& body);

    void collect_scope_signals(const Scope&                                                                  scope,
                               const std::string&                                                            prefix,
                               const ankerl::unordered_dense::map<const Symbol*, ir::SignalInfo::Direction>* port_dir);

    ir::Signal register_signal(ir::SignalInfo info);

    ir::Signal signal_of(const ValueSymbol& sym);

    ir::Signal declare_local(const ValueSymbol& vs);

    static bool is_const_table_symbol(const ValueSymbol& s);

    ir::Signal const_table_signal(const ValueSymbol& vs);

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

    static ir::Inst::Tag net_resolve_tag(NetType::NetKind kind);

    static std::optional<int> net_pull_value(NetType::NetKind kind);

    static uint8_t net_pull_strength(NetType::NetKind kind);

    // A driven region of a net: an unpacked-array element band crossed with a
    // packed bit range within one element. A packed (non-array) net uses the
    // single element band [0, 0]; a whole-element or whole-net driver uses the
    // full bit range [0, UINT32_MAX].
    struct NetRegion {
        uint64_t elem_lo, elem_hi; // linearized unpacked-element ordinals
        uint32_t bit_lo, bit_hi;   // packed bit range within one element
        bool     operator==(const NetRegion&) const = default;
    };

    static constexpr NetRegion kWholeNet{ 0, UINT64_MAX, 0, UINT32_MAX };

    void record_net_driver_region(const ValueSymbol& raw, const NetRegion& reg);

    void record_net_driver_whole(const ValueSymbol& sym);

    void force_net_resolved(ir::Signal net);

    static const ValueSymbol* lvalue_root(const Expression& e);

    static bool is_elaboration_constant(const ValueSymbol& s);

    std::optional<ir::Index> try_lower_constant_select(const Expression& expr, ir::Builder& b, ir::SourceLoc l);

    ir::Index select_base_lvalue(const Expression& base, ir::Builder& b, ir::SourceLoc l);

    void record_net_driver(const Expression& lhs);

    NetRegion net_region_of(const Expression& lhs, const ValueSymbol& root);

    std::optional<std::pair<uint32_t, uint32_t>> packed_bit_range(const Expression& step);

    // -- Multi-driver net resolution (LRM 6.6.1, wire/tri, no strengths) --
    //
    // A net that Classify found to have >= 2 overlapping drivers is "resolved":
    // Emit routes each of its drivers to a private full-width slot signal
    // (alloc_driver_slot) instead of storing the net directly, and
    // emit_wire_resolvers folds all the slots through WireRes into the net at the
    // end of lowering. A single-driver net is never in resolved_nets_ and keeps
    // the plain-store fast path untouched. V1 supports whole-net drivers only.

    static bool is_whole_net_lvalue(const Expression& e);

    std::optional<ir::Signal> resolved_net_of(const Expression& lhs);

    static uint8_t strength_level(const std::optional<slang::ast::DriveStrength>& s);

    static uint16_t pack_strength(
      const std::pair<std::optional<slang::ast::DriveStrength>, std::optional<slang::ast::DriveStrength>>& ds);

    ir::Signal alloc_driver_slot(ir::Signal    net,
                                 ir::SourceLoc l,
                                 uint16_t      strength       = 0x0303,
                                 bool          program_driver = false);

    ir::Ref def_constant_ref(uint32_t width, bool one);

    ir::Ref z_constant_ref(uint32_t width);

    ir::Index build_driver_slot_value(ir::Signal        net,
                                      const Expression& left,
                                      ir::Index         driven,
                                      ir::Builder&      b,
                                      ir::SourceLoc     l);

    void reject_concat_resolved_driver(const Expression& lhs);

    void store_driver_slot(const Expression& left,
                           ir::Signal        slot,
                           ir::Index         val,
                           const Expression* delay_expr,
                           ir::Builder&      b,
                           ir::SourceLoc     l);

    void emit_resolved_driver(ir::Signal                  net,
                              const AssignmentExpression& a,
                              const Expression&           left,
                              ir::Builder&                b,
                              ir::SourceLoc               l,
                              uint16_t                    strength   = 0x0303,
                              const Expression*           delay_expr = nullptr);

    void emit_wire_resolvers();

    // -- MOS switch strength (LRM 28.13, 28.14) --
    //
    // A MOS switch passes the strength of its data input to its output, and a
    // resistive one reduces it. Clotho has no strength on a net's value, so every
    // switch drives strong. That is exact whenever no other driver or pull
    // competes with the switch's strength, or the strengths involved order the same
    // as strong does; check_mos_switch_strength refuses the rest. Classify records
    // what it needs (note_weak_driver, note_mos_switch); the verdict runs in Emit,
    // once Classify has seen every driver of the design.
    struct MosSwitch {
        // The net the switch drives, and the net its data input reads (empty for a
        // constant, a variable or an expression: those drive strong).
        ir::Signal                out;
        std::optional<ir::Signal> data;
        // The data input names a net in a shape the switch lowering reads as an
        // expression (an element of a net array): the strength it would pass is
        // then unknown.
        bool data_opaque;
        bool resistive;
        // Drivers the switch makes on `out`: 1, or 2 for a cmos (its two halves).
        uint32_t drivers;
    };
    // Per-switch and per-net results of the strength propagation over every
    // switch Classify recorded; built on first use by mos_strength().
    struct MosStrength {
        // Possible strength levels (a bit set, see Nets.cpp) of each switch's output,
        // parallel to mos_switches_.
        std::vector<uint8_t> levels;
        // Output net -> indices of the switches driving it.
        ankerl::unordered_dense::map<uint32_t, std::vector<uint32_t>> by_out;
    };

    void note_weak_driver(const Expression& lhs);

    std::optional<MosSwitch> mos_switch_info(const PrimitiveInstanceSymbol& gi);

    void note_mos_switch(const PrimitiveInstanceSymbol& gi);

    const MosStrength& mos_strength();

    void check_mos_switch_strength(const PrimitiveInstanceSymbol& gi, const Expression& out_left);

    void store_every_element(ir::Index lv, ir::Ref ty, ir::Index val, ir::Builder& b, ir::SourceLoc l);

    // -- Processes --

    void lower_continuous_assign(const ContinuousAssignSymbol& sym);

    std::vector<ir::WaitEvent> continuous_events(const Expression& e);

    void add_continuous_process(std::vector<ir::WaitEvent> wait_events,
                                ir::SourceLoc              l,
                                ir::Builder&               b,
                                bool                       reactive_home = false);

    // -- Procedural continuous assignment: force / release (LRM 10.6.2) --
    //
    // Whole-scalar variables (integral or real) and nets. Each forceable signal
    // carries a hidden 2-state i32 `force_site` signal (0 = not forced, else the
    // compile-time id of the active textual force site). Ordinary procedural /
    // continuous writes to the variable are gated `if (force_site === 0)`; a
    // `force` sets the site and writes unconditionally, plus arms a continuous
    // re-eval process that re-applies its RHS whenever the RHS changes (LRM's
    // continuous relationship); `release` clears the site and the value holds. Per
    // textual force site ids give last-force-wins: a superseded site's re-eval
    // process sees `force_site !== its id` and stops writing. Nets, bit/part/
    // element-selects, aggregates, and `assign`/`deassign` are guarded loud below.
    struct ForceInfo {
        ir::Signal site{ 0 };     // force_site, always allocated
        uint32_t   next_site = 0; // ids handed to textual force sites, 1-based
        // Second override layer: the procedural continuous `assign` statement (LRM
        // 10.6.1), priority below force and above ordinary procedural writes.
        // Present only when the variable is an `assign` target somewhere.
        bool       has_assign = false;
        ir::Signal assign_site{ 0 };
        uint32_t   next_assign = 0;
    };
    ankerl::unordered_dense::map<uint32_t, ForceInfo> forceable_;

    // One `force` of a net (LRM 10.6.2), whole or a constant bit/part-select. A net
    // has no procedural writers to gate; instead each force site contributes a
    // range [lo, hi] whose bits the net's resolver overlays with `val` while
    // `active` != 0 (emit_wire_resolvers). `val` is exactly range-wide. Per-site
    // (not a single net force_val) so disjoint partial forces coexist and each
    // releases independently.
    struct NetForceSite {
        ir::Signal active{ 0 }; // 2-state i32, 0 = inactive
        ir::Signal val{ 0 };    // (hi - lo + 1)-wide forced value
        uint32_t   lo = 0, hi = 0;
    };
    // Net handle -> its force sites (in registration order; the resolver blends
    // them in that order, exact for disjoint ranges).
    ankerl::unordered_dense::map<uint32_t, std::vector<NetForceSite>> net_force_sites_;
    // Net handle -> the zero-formal function that re-folds the net's drivers and
    // overlays its force sites. The resolver process calls it, and so does a
    // `release` of the net, so the net takes its driven value at once (LRM
    // 10.6.2) instead of when the resolver process next runs. A plain function,
    // not a task, because a release may sit in a function. Reserved on first
    // use, filled by emit_wire_resolvers.
    ankerl::unordered_dense::map<uint32_t, ir::FunctionId> net_resolver_fn_;

    ir::FunctionId net_resolver_fn(uint32_t net);
    // A `force` statement -> (net handle, index into net_force_sites_) so the Emit
    // lowering reuses the site the Collect scan created.
    ankerl::unordered_dense::map<const void*, std::pair<uint32_t, uint32_t>> stmt_force_site_;

    const ValueSymbol* force_target(const Expression& lhs, bool* is_net);

    const ValueSymbol* force_target_var(const Expression& lhs);

    ForceInfo* force_info_of(const ValueSymbol& sym);

    void scan_force_targets(const Statement& body);

    ir::Signal new_pca_state(const char* tag, const ValueSymbol& sym);

    void register_pca_site(const ProceduralAssignStatement& s);

    void register_net_force_site(const ProceduralAssignStatement& s, const Expression& lhs, const ValueSymbol& root);

    void register_pca_target(const Expression& lhs, bool is_force);

    std::vector<ir::WaitEvent> with_force_wake(std::vector<ir::WaitEvent> ev, const Expression& lhs);

    ir::Index site_const(uint32_t id, ir::Builder& b, ir::SourceLoc l);

    ir::Index site_is(ir::Signal site, uint32_t id, ir::Builder& b, ir::SourceLoc l);

    ir::Index not_forced(const ForceInfo& fi, bool also_assign, ir::Builder& b, ir::SourceLoc l);

    ir::Index gated_store(const AssignmentExpression& a, const ForceInfo& fi, ir::Builder& b);

    ForceInfo* pca_info_of_lvalue(const Expression& lhs);

    void store_or_gate(const ForceInfo*     fi,
                       ir::Inst::Tag        tag,
                       ir::Index            lv,
                       ir::Index            v,
                       ir::Builder&         b,
                       ir::SourceLoc        l,
                       const IntraNbTiming* nbt = nullptr);

    bool is_externally_driven_port(const ValueSymbol& sym);

    const ValueSymbol* pca_target_var(const Expression& lhs, const char* verb);

    // Build a continuous re-eval process that stores `a.right()` into `v` while
    // `pred_fn(rb)` holds, sensitive to the RHS reads plus `extra` wait events.
    template<typename PredFn>
    void add_pca_reeval(const AssignmentExpression& a,
                        ir::Signal                  v,
                        ir::SourceLoc               l,
                        std::vector<ir::WaitEvent>  extra,
                        PredFn                      pred_fn) {
        ir::Builder     rb(module_.pool);
        const ir::Index pred = pred_fn(rb);
        rb.open_region();
        const ir::Index dst = rb.signal(v, l);
        const ir::Index rhs = lower_expr(a.right(), rb);
        rb.binary(ir::Inst::Tag::Store, dst, rhs, l);
        const ir::ExtraSlice then_r = rb.close_region();
        rb.open_region();
        const ir::ExtraSlice else_r = rb.close_region();
        rb.cond(pred, then_r, else_r, l);
        std::vector<ir::WaitEvent> ev = continuous_events(a.right());
        ev.insert(ev.end(), extra.begin(), extra.end());
        add_continuous_process(std::move(ev), l, rb);
    }

    void lower_proc_assign(const ProceduralAssignStatement& s, ir::Builder& b);

    void lower_proc_deassign(const ProceduralDeassignStatement& s, ir::Builder& b);

    void release_net(const Expression& lv, const ValueSymbol& root, ir::Builder& b, ir::SourceLoc l);

    void lower_net_initializer(const NetSymbol& ns);

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
    struct GateTerm {
        ir::Signal sig = ir::Signal{ 0 };
        uint32_t   bit = 0;
    };
    struct GateInput {
        ir::Signal sig        = ir::Signal{ 0 };
        uint32_t   bit        = 0;
        uint8_t    const_code = 0;
        bool       is_const   = false;
    };

    // A three-state gate (bufif/notif, LRM 28.6). `active_high` is the control
    // level that enables the driver (1 for bufif1/notif1, 0 for bufif0/notif0);
    // `invert` distinguishes notif (inverts the data) from bufif.
    struct TriGate {
        bool active_high = false;
        bool invert      = false;
    };

    static std::optional<TriGate> tristate_gate_of(std::string_view name);

    static std::optional<TriGate> mos_switch_of(std::string_view name);

    static std::optional<GateOp> gate_op_of(std::string_view name);

    static ir::Inst::Tag gate_fold_tag(GateOp op);

    static bool gate_inverted(GateOp op);

    static const Expression& unwrap_conversions(const Expression& e);

    std::optional<ir::Signal> gate_select_base(const Expression& value);

    std::optional<uint32_t> gate_select_bit(const Expression& value, const Expression& index, Fold mode);

    // `mode` is how a bit-select index is folded: Required for an output terminal
    // (a net_lvalue takes a constant_select, LRM A.8.5), Runtime for an input,
    // which is any expression. A part-select bound is always Required.
    std::optional<GateTerm> gate_static_term(const Expression& e, Fold mode);

    GateInput lower_gate_input(const Expression& e, std::string_view inst_name, uint32_t input_index);

    GateTerm lower_gate_output(const Expression& conn);

    void lower_pull_source(const PrimitiveInstanceSymbol& gi, bool up);

    void emit_pull_driver(const ValueSymbol& net, bool up, uint8_t level, ir::SourceLoc l, bool program_driver);

    void lower_unconnected_drive(const PortSymbol& p, bool up, bool program_driver);

    void lower_cmos_switch(const PrimitiveInstanceSymbol& gi);

    void lower_gate(const PrimitiveInstanceSymbol& gi);

    ir::Index gate_bit_lvalue(ir::Signal sig, uint32_t bit, ir::Builder& b, ir::SourceLoc l);

    ir::Index gate_output_value(const Expression& left, ir::Index val, ir::Builder& b, ir::SourceLoc l);

    ir::Index gate_input_value(const GateInput& in, ir::Builder& b, ir::SourceLoc l, bool pass_z = false);

    void collect_gate_sensitivity(std::span<const GateInput> inputs, std::vector<ir::WaitEvent>& wait_events);

    ir::Index gate_folded_value(GateOp                      op,
                                std::span<const GateInput>  inputs,
                                std::vector<ir::WaitEvent>& wait_events,
                                ir::Builder&                b,
                                ir::SourceLoc               l);

    ir::Index gate_tristate_value(TriGate                     tg,
                                  const GateInput&            data,
                                  const GateInput&            ctrl,
                                  std::vector<ir::WaitEvent>& wait_events,
                                  ir::Builder&                b,
                                  ir::SourceLoc               l,
                                  bool                        pass_z = false);

    void emit_gate_as_process(GateTerm                   output,
                              ir::Index                  val,
                              std::vector<ir::WaitEvent> wait_events,
                              ir::Builder&               b,
                              ir::SourceLoc              l,
                              const Expression*          delay_expr = nullptr);

    void emit_gate_resolved_driver(const Expression&          left,
                                   ir::Signal                 net,
                                   ir::Index                  val,
                                   std::vector<ir::WaitEvent> wait_events,
                                   ir::Builder&               b,
                                   ir::SourceLoc              l,
                                   uint16_t                   strength   = 0x0303,
                                   const Expression*          delay_expr = nullptr);

    void wire_ports(const InstanceSymbol& child);

    void collect_expr_reads(const Expression& expr, std::vector<ir::WaitEvent>& out);

    ir::Index lower_port_output(const AssignmentExpression& a, ir::Index port_value, ir::Builder& b);

    static const Statement& unwrap_blocks(const Statement& stmt);

    static const Symbol* outermost_named_block(const Statement& body);

    void lower_procedural_block(const ProceduralBlockSymbol& sym);

    static bool is_event_control(const TimingControl& tc);

    const Expression* collect_wait_events(const TimingControl& tc, std::vector<ir::WaitEvent>& out);

    void collect_wait_events_impl(const TimingControl&        tc,
                                  std::vector<ir::WaitEvent>& out,
                                  const Expression*&          iff,
                                  uint32_t&                   n_events);

    void collect_inferred_sensitivity(const Statement& body, SensitivityMode mode, std::vector<ir::WaitEvent>& out);

    static ir::Edge to_edge(slang::ast::EdgeKind edge);

    void push_prop_wait_events(const SignalEventControl&              ev,
                               const slang::ast::ClassPropertySymbol& prop,
                               std::vector<ir::WaitEvent>&            out);

    void refuse_container_event_control(const SignalEventControl& ev);

    ir::Index prop_notifier_handle(const Expression&                      e,
                                   const slang::ast::ClassPropertySymbol& prop,
                                   ir::Edge                               edge,
                                   ir::Builder&                           b);

    // True when a `@(...)` on this expression needs a runtime event HANDLE rather
    // than a signal: it is event-typed but is not a plain reference to a declared
    // event variable. `@(ev[i])`, `@(obj.e)`, `@(s.e)` and `@(<formal>)` are the
    // cases; each names an object that no signal identifies.
    bool events_merge_ = false;

    bool event_wait_needs_handle(const Expression& e);

    bool sensitivity_needs_handle(const TimingControl& tc);

    // Set while an INTRA-BODY wait is being collected: the builder its handle
    // expressions lower into. Null while a process's static sensitivity is
    // collected, which is what makes event_signal refuse a frame-held event there.
    ir::Builder* wait_builder_ = nullptr;
    // Set when a listed event was a member of a never-bound virtual interface:
    // the located LRM 25.9 run-time fatal just emitted replaces the wait, so an
    // empty event list is complete rather than malformed (lower_event_wait).
    bool wait_dead_ = false;

    static bool reads_vif(const Expression& e);

    static std::vector<const MemberAccessExpression*> vif_member_reads(const Expression& e);

    ir::Signal event_signal(const Expression& e);

    ir::Signal synth_event_net(const Expression& e);

    uint32_t anon_local(ir::Ref type);

    void lower_vif_expr_wait(const SignalEventControl& ev, ir::Builder& b, ir::SourceLoc l);

    void lower_vif_clocking_drive(const MemberAccessExpression& ma, const AssignmentExpression& a,
                                  ir::Builder& b);

    void lower_event_wait(const TimingControl& tc, ir::Builder& b, ir::SourceLoc l);

    void lower_wait_order(const slang::ast::WaitOrderStatement& s, ir::Builder& b);

    void emit_guarded_body(const Statement& body, const Expression& guard, ir::Builder& b, ir::SourceLoc l);

    void emit_guarded_wait(const std::vector<ir::WaitEdge>& edges,
                           const Expression&                guard,
                           ir::Builder&                     b,
                           ir::SourceLoc                    l);

    void lower_intra_timing(const TimingControl& tc, ir::Builder& b, ir::SourceLoc l);

    ir::Signal trigger_target(const Expression& target);

    ir::Index event_handle(const Expression& e, ir::Builder& b);

    uint32_t repeat_count_const(const Expression& e);

    void lower_event_trigger(const EventTriggerStatement& ev, ir::Builder& b);

    const Expression* triggered_event(const Expression& e);

    static std::string_view sequence_method(const CallExpression& call);

    // NOTE: `wait(...)` is lowered by the general `lower_wait_stmt` below (from
    // the wait(expr) feature). It subsumes `wait(e.triggered)`: the read set of
    // the condition includes the event, so `-> e` wakes the wait and the
    // re-tested `.triggered` (stamped this instant) ends it.

    // -- Statements --

    void lower_stmt(const Statement& stmt, ir::Builder& b);

    void lower_disable(const slang::ast::DisableStatement& s, ir::Builder& b);

    void lower_fork(const BlockStatement& blk, ir::Builder& b);

    ir::Index lower_predicate(const Expression& cond, ir::Builder& b);

    ir::Index lower_loop_cond(const Expression& cond, ir::Builder& b);

    bool chain_has_final_else(const ConditionalStatement& c) const;

    void emit_deferred_action(const Statement*      action,
                              uint32_t              defer_mode,
                              bool                  is_fail,
                              slang::SourceLocation site,
                              ir::Builder&          b);

    void lower_deferred_assertion(const ImmediateAssertionStatement& a, ir::Builder& b);

    static const TimingControl* local_event_with_syntax(const TimingControl*             local,
                                                        const slang::syntax::SyntaxNode* want);

    static const TimingControl* body_clock(const AssertionExpr& expr);

    static const Expression* body_disable(const AssertionExpr& expr);

    // -- Assertion control (LRM 20.12) ---------------------------------------
    //
    // `$assertoff` and its family name assertions Emit has not reached yet: a
    // `$assertoff(0, sub)` in the top's initial block covers monitors from an
    // instance whose members lower later in the very same pass, and
    // Module::monitors only fills in as Emit goes. So the addressing table
    // cannot be built from the monitor registry. Collect builds it instead --
    // that walk already visits every procedural block to find `force` targets
    // (scan_force_targets), so enumerating the assertions rides along for free,
    // and it settles the one question that has to be answered before any
    // monitor state is allocated: whether this design calls a control task at
    // all. A design that calls none allocates nothing here and emits exactly
    // the signals it emitted before any of this existed.
    //
    // What a control task writes is PER-ASSERTION state and not a global flag,
    // because 20.12 addresses assertions individually: `$asserton` on one scope
    // must not undo an `$assertoff` aimed at another.

    // The control state of one assertion: five 1-bit signals, all in NEGATIVE
    // logic, so every one of them powers up to 0 and 0 IS the LRM's power-up
    // state -- checking, reporting, unlocked. An active-high enable would have
    // to be preset to 1, and the monitor's own state deliberately has no
    // initializer machinery behind it ("All 2-state, so all power up to 0").
    struct MonitorControl {
        // Stop SEEDING new attempts (control types Off and Kill). This gates
        // the seed and nothing else: 20.12 is explicit that attempts already
        // executing are unaffected, so the advance, the verdicts, the state
        // stores and the reports must never read it.
        ir::Signal off{ 0 };
        // Abandon everything in flight (Kill). Held as a REQUEST rather than as
        // a level: the task runs in an arbitrary time slot and the monitor acts
        // only at its own clocking edge, so the request has to survive until
        // that edge -- where it joins `disable iff` in the reset term and is
        // cleared. Same latch, and for the same reason, as
        // MonitorState::abort_seen. Reusing the reset is what makes a kill
        // exactly the abandonment LRM 16.15 already specifies, rather than a
        // second clear that could drift from it.
        ir::Signal kill{ 0 };
        // Pass / fail action suppression (PassOn, PassOff, FailOn, FailOff).
        // Separate bits from `off` because they gate the ACTION BLOCK alone:
        // the automaton has already stepped by the time they are read, so a
        // suppressed report leaves a cover tally advancing behind it.
        ir::Signal poff{ 0 };
        ir::Signal foff{ 0 };
        // Lock / Unlock: while set, no control task may change any of the four
        // above. Every control write is therefore a select on this bit, and the
        // bit itself is the one thing written unconditionally -- Unlock has to
        // be able to escape a lock it is standing inside.
        ir::Signal lock{ 0 };
    };

    // One concurrent assertion, as the Collect walk found it.
    struct AssertSite {
        // Identity, for the Emit-side lookup. Two instances of one module get
        // their own InstanceBodySymbol and therefore their own bound Statement,
        // so `stmt` alone would do; the scope is carried anyway because it is
        // what a wrong answer here would silently alias, and it costs 4 bytes.
        const slang::ast::ConcurrentAssertionStatement* stmt  = nullptr;
        uint32_t                                        scope = ir::Process::no_scope;
        // Which directive it is, for $assertcontrol's directive_type filter
        // (LRM Table 20-7). Read from the AST because the monitor that would
        // otherwise carry it does not exist yet.
        slang::ast::AssertionKind kind = slang::ast::AssertionKind::Assert;
        // The procedural block the assertion sits in, and the innermost named
        // block between the two. Together they answer "does this target name
        // this assertion": a label makes a StatementBlockSymbol, which is what
        // `$assertoff(0, top.ap)` resolves to, and containment in a generate or
        // named block is found by walking up from the procedural block.
        const Symbol* owner = nullptr;
        const Symbol* label = nullptr;
        // Where the control state is allocated: the instance's signal-name
        // prefix, and a location to hang the hidden signals off.
        std::string    prefix;
        ir::SourceLoc  loc{};
        MonitorControl ctl;
    };

    // LRM Table 20-5's control_type codes. The nine named tasks are DEFINED by
    // 20.12 as $assertcontrol with one of these and a fixed assertion_type of
    // 15, so they lower through the same path rather than duplicating it.
    enum class AssertCtl : int32_t {
        Lock         = 1,
        Unlock       = 2,
        On           = 3,
        Off          = 4,
        Kill         = 5,
        PassOn       = 6,
        PassOff      = 7,
        FailOn       = 8,
        FailOff      = 9,
        NonvacuousOn = 10,
        VacuousOff   = 11,
    };

    static int32_t named_control_code(std::string_view name);

    static bool is_assert_control_task(std::string_view name);

    void scan_assert_control(const ProceduralBlockSymbol& blk);

    void scan_disable_targets(const ProceduralBlockSymbol& blk);

    bool hard_park(const Statement& body, bool forked_children_are_occupants = false);

    bool subroutine_parks(const SubroutineSymbol& sub);

    bool design_virtual_forks(slang::ast::Compilation& comp);

    void record_assert_site(const slang::ast::ConcurrentAssertionStatement& s,
                            const Symbol*                                  owner,
                            const Symbol*                                  label);

    static ir::InstanceScope* find_scope_node(ir::InstanceScope& root, uint32_t id);

    void alloc_assert_control();

    const MonitorControl* control_for(const slang::ast::ConcurrentAssertionStatement& a);

    static uint32_t control_depth(const AssertSite& site, const Symbol& target);

    static bool targets_cover(const AssertSite& site, const std::vector<const Symbol*>& targets, int32_t levels);

    static int32_t directive_bit(slang::ast::AssertionKind kind);

    bool expect_seen() const;

    int32_t const_control_arg(const Expression& e, const char* what);

    void store_control_bit(ir::Signal sig, bool set, ir::Builder& b, ir::SourceLoc l);

    void store_control_locked(const MonitorControl& ctl, ir::Signal sig, bool set, ir::Builder& b, ir::SourceLoc l);

    void emit_control_write(const MonitorControl& ctl, AssertCtl code, ir::Builder& b, ir::SourceLoc l);

    void lower_assert_control(std::string_view name, const CallExpression& call, ir::Builder& b);

    // -- Concurrent assertions (LRM 16) --------------------------------------
    //
    // `assert property (@(posedge clk) disable iff (rst) a |=> b);`
    //
    // slang hands the whole thing over as one bound AssertionExpr with the
    // clocking event outermost and the disable condition just inside it (see
    // AssertionExpr::bind), so both wrappers are peeled here before the temporal
    // part goes to libsva. A module-item assertion arrives the same way: slang
    // wraps it in a ProceduralBlockSymbol(Always, isFromAssertion), so it reaches
    // this through lower_procedural_block -> lower_forever -> lower_stmt.
    //
    // What comes back is an ir::AssertionMonitor: two automata over interned
    // guard cubes, plus the obligation tracking a property needs. See
    // docs/design/concurrent-assertions.md.

    // The two hidden signals an `expect`'s STATEMENT needs from its monitor (LRM
    // 16.17), and the only part of MonitorState that leaves emit_monitor: the
    // monitor's state is allocated in there, while the statement that stores the
    // request is emitted by the caller into the executing process. Declared up here
    // rather than beside MonitorState because it appears in emit_expect_wait's
    // signature, which an in-class declaration cannot forward-reference.
    //
    // Both Signal{0} for every other directive, which is also what makes a misuse
    // loud rather than silent: storing into signal 0 is not a quiet no-op, it is a
    // write to a real signal, so the emptiness is checked where it is read.
    struct ExpectPorts {
        ir::Signal req{ 0 };
        ir::Signal busy{ 0 };
    };

    void lower_concurrent_assertion(const slang::ast::ConcurrentAssertionStatement& a, ir::Builder& b);

    void expect_shape_check(const ir::AssertionMonitor& monitor, const Expression* disable);

    void emit_expect_wait(uint32_t                                        id,
                          const ExpectPorts&                             ports,
                          const slang::ast::ConcurrentAssertionStatement& a,
                          ir::Builder&                                    b);

    // The action blocks of one concurrent assertion (LRM 16.14), as slang
    // statements. Either may be absent. An absent fail action still reports: the
    // default failure message is the implicit fail action, so it takes the same
    // Reactive-region path rather than being emitted inline in the monitor.
    struct AssertionActions {
        const Statement* pass = nullptr;
        const Statement* fail = nullptr;
    };

    void require_reactive_safe(const Statement& action);

    void require_finish_inlinable(const Statement& action);

    ir::FunctionId build_action_body(const Statement& body, std::string_view name);

    ir::FunctionId build_default_fail_body(const ir::AssertionMonitor& mon);

    // -- The monitor step, as ordinary IR ------------------------------------
    //
    // libsva's per-cycle protocol (../libsva/src/Monitor.zig:27-47, with a
    // compiling reference implementation at ../libsva/test/backend.c) walks the
    // live states of an automaton. A code generator cannot: there is no run-time
    // state set to iterate, only constants. Every fold below is therefore
    // transposed onto something that IS compile-time -- the states, or the
    // interned guards -- so that each contributes a constant mask selected by one
    // bit. That transposition is the whole of concurrent-assertions.md 7.2.
    //
    // A mask over N automaton states is one `bit [N-1:0]` value. Clotho's values
    // are arbitrary-precision, so AssertionAutomaton::words is a transport detail
    // of libsva's C API and a row collapses to a single constant here.

    // The hidden state one monitor carries between cycles. All 2-state, so all
    // power up to 0 -- no attempt pending, no trigger in flight.
    struct MonitorState {
        // Trigger state mask. Only with an antecedent.
        ir::Signal trigger{ 0 };
        // Whether the trigger matched last cycle, for `|=>`. Only with one.
        ir::Signal fired{ 0 };
        // One obligation attempt per slot, `depth` of them.
        std::vector<ir::Signal> pending;
        // Which slots hold a live attempt: the ring's carrier for
        // ir::AssertionVerdict::Pending. NOT derivable from `pending != 0`: an
        // attempt seeded into a cycle whose first letter does not hold is empty
        // immediately, and that is a Fails rather than an absent attempt. Deriving
        // occupancy from emptiness loses every such failure -- measured, see
        // ../libsva/test/backend.c:405-411. This is why the lattice has three
        // members and not two.
        ir::Signal occupied{ 0 };
        // One count per obligation state, under AssertionTracking::Counted, and
        // empty under the ring. `pending` and `occupied` are the ring's and are
        // empty here instead: the two disciplines never coexist in one monitor.
        // Entry `s` is absent (Signal{0}) for a state that cannot carry an attempt
        // into the next cycle -- see counted_carries.
        std::vector<ir::Signal> counts;
        // Per-attempt identity, which gives an attempt one verdict
        // (docs/LRM_NOTES.md, note 23), `attempt_ring_size` bits wide and
        // all absent when that is 0. `tid[s]` is the set of attempts whose antecedent
        // threads sit in trigger state `s`, one bit per live attempt; `oid[k]` is the
        // set of attempts that seeded obligation slot `k`, which is a SET and not one
        // id because attempts whose antecedent matches end in the same cycle impose
        // the same obligation and share the slot -- while still being owed a verdict
        // each; `firedid` carries a `|=>` match's attempts over to the cycle they
        // seed in; `idnow` is the rotating one-hot id the attempt starting this cycle
        // takes.
        std::vector<ir::Signal> tid;
        std::vector<ir::Signal> oid;
        ir::Signal              firedid{ 0 };
        ir::Signal              idnow{ 0 };
        // Attempts every obligation of which has Held so far, still waiting for the
        // attempt to be unable to raise another one. Not a fourth lattice member:
        // the ATTEMPT is still Pending here, and this records that nothing of it has
        // Failed yet. A pass is only the attempt's once nothing of it is left
        // running, and that can be a later cycle than the one its last obligation
        // matched in.
        ir::Signal passing{ 0 };
        // Match tally, for `cover property`.
        ir::Signal covered{ 0 };
        // -- `expect` only (LRM 16.17), both absent for every other directive. --
        //
        // The pending REQUEST: a process executing the statement sets it, and the
        // next clocking edge consumes it as the seed. This is what makes an
        // `expect` a one-shot without a second mechanism -- a bare sequence starts
        // an attempt every cycle, and gating the seed is exactly how $assertoff
        // stops that (see the seed in emit_monitor), so the request rides the same
        // gate from the other side. One bit and not a count: several processes
        // reaching the statement before the same edge are one cohort, because
        // attempts starting on one edge see one signal history and so owe one
        // verdict.
        //
        // Cleared only on an edge that really seeds. A request the disable
        // condition or an $assertoff held back is still owed an attempt, and
        // clearing it there would park the process forever.
        ir::Signal expect_req{ 0 };
        // Whether an attempt of this expect is in flight: set by the edge that
        // seeds one, cleared by the one that resolves it. Read by the STATEMENT,
        // not by the step -- it is what makes a second cohort's arrival refusable
        // (see lower_concurrent_assertion), which is what keeps the waiter queue
        // exactly the cohort of the next attempt to resolve.
        ir::Signal expect_busy{ 0 };
        // One per ASYNCHRONOUS abort, empty entry for a `sync_` one: whether its
        // condition has been seen high since the last clocking edge. An
        // asynchronous abort is read live and LRM 16.12.7 evaluates it at the
        // granularity of the simulation time step, so it can go high and fall
        // again strictly BETWEEN two ticks (`abx` in tests/sva/diff_abort.sv) --
        // which the step would otherwise never see, because the monitor runs
        // every time slot but only acts on an edge. Latching makes the condition
        // a property of the whole cycle rather than of its final instant.
        std::vector<ir::Signal> abort_seen;
        // The clock's value at the previous Observed evaluation. An Observed
        // process is resumed once per time slot rather than woken by a net event,
        // so the monitor detects its own clocking edge and needs one slot of
        // clock history to do it.
        ir::Signal clkprev{ 0 };
        // LRM 20.12 assertion control, or null when the design calls no control
        // task. Not allocated here like the rest: a control task can name an
        // assertion whose monitor Emit has not built yet, so the signals are
        // laid down before Emit starts and this only points at them
        // (alloc_assert_control).
        const MonitorControl* ctl = nullptr;
    };

    ir::Signal new_monitor_state(std::string name, ir::Ref type, ir::SourceLoc l);

    ir::Ref mask_type(uint32_t width);

    MonitorState alloc_monitor_state(const ir::AssertionMonitor& mon, uint32_t id, const MonitorControl* ctl);

    static bool row_is_zero(std::span<const uint64_t> row);

    static std::span<const uint64_t> table_row(const std::vector<uint64_t>& table, uint32_t index, uint32_t words);

    static bool rows_intersect(std::span<const uint64_t> a, std::span<const uint64_t> b);

    static uint32_t attempt_ring_size(const ir::AssertionMonitor& mon);

    static bool root_reads_live(const ir::AssertionMonitor& mon, uint32_t root);

    static std::vector<uint64_t> abort_arming_row(const ir::AssertionAutomaton& au, const ir::AssertionAbort& abort);

    ir::Ref mask_const(std::span<const uint64_t> row, uint32_t states);

    ir::Index zero_mask(uint32_t width, ir::Builder& b, ir::SourceLoc l);

    ir::Index slot_mask(uint32_t bit, uint32_t width, ir::Builder& b, ir::SourceLoc l);

    ir::Index load_signal(ir::Signal sig, ir::Builder& b, ir::SourceLoc l);

    ir::Index bit_not(ir::Index v, ir::Builder& b, ir::SourceLoc l);

    ir::Index mask_any(ir::Index mask, ir::Builder& b, ir::SourceLoc l);

    ir::Index mask_bit(ir::Index mask, uint32_t bit, uint32_t width, ir::Builder& b, ir::SourceLoc l);

    ir::Index mask_popcount(ir::Index mask, uint32_t width, ir::Builder& b, ir::SourceLoc l);

    ir::Index int_const(uint64_t v, uint32_t width, ir::Builder& b, ir::SourceLoc l);

    ir::Index rotl1(ir::Index v, uint32_t width, ir::Builder& b, ir::SourceLoc l);

    void store_gated(ir::Signal    sig,
                     ir::Index     value,
                     ir::Index     active,
                     ir::Index     edge,
                     uint32_t      width,
                     ir::Builder&  b,
                     ir::SourceLoc l);

    ir::Index next_attempt_id(const MonitorState& st,
                              uint32_t            ring,
                              ir::Index           active,
                              ir::Index           edge,
                              ir::Builder&        b,
                              ir::SourceLoc       l);

    std::vector<ir::Index> advance_attempt_ids(const ir::AssertionMonitor& mon,
                                               const MonitorState&         st,
                                               uint32_t                    ring,
                                               ir::Index                   cur_t,
                                               ir::Index                   next,
                                               ir::Index                   id_now,
                                               ir::Index&                  fired,
                                               ir::Builder&                b,
                                               ir::SourceLoc               l);

    ir::Index automaton_advance(const ir::AssertionAutomaton& au,
                                ir::Index                     current,
                                ir::Index                     satisfied,
                                ir::Builder&                  b,
                                ir::SourceLoc                 l);

    ir::Index automaton_satisfy(const ir::AssertionMonitor&   mon,
                                const ir::AssertionAutomaton& au,
                                std::span<const ir::Index>    roots,
                                ir::Builder&                  b,
                                ir::SourceLoc                 l);

    // -- The verdict lattice -------------------------------------------------
    //
    // ir::AssertionVerdict named as the three values the step computes for one
    // attempt, and the single place the three are defined. Before `not` the lattice
    // was implicit and spread over four sites: the two masks taken here, the
    // occupancy bit that says an attempt exists at all, and the end-of-trace flush
    // where Pending finally resolves. Negation is what makes that untenable --
    // `not` maps Holds to Fails and Fails to Holds but Pending to Pending, so the
    // third member has to be a value rather than the absence of the other two.
    //
    // Ring and counted realise it differently and cannot share a type: a ring slot
    // holds one attempt, so each member is a 1-bit value, while a counted state
    // stands for however many attempts are sitting in it and each member is a
    // count. They agree on the lattice, not on its width -- see the counted
    // verdicts in emit_counted_step, which are `matches`, `failures` and the
    // surviving per-state counts in that order.
    struct AttemptVerdict {
        ir::Index holds{};
        ir::Index fails{};
        ir::Index pending{};
    };

    AttemptVerdict ring_verdict(ir::Index held, ir::Index states, ir::Index last, ir::Builder& b, ir::SourceLoc l);

    // -- The counted discipline ----------------------------------------------
    //
    // What an unbounded obligation needs, and what the ring cannot give it: no
    // bound on the obligation's longest match means no pool can be sized. The
    // obligation arrives DETERMINISED instead, so an attempt occupies exactly one
    // state, two attempts in one state have the same future, and the tracker
    // carries a COUNT per state rather than a state vector per attempt.
    //
    // The reference implementation is ../libsva/test/merge.zig's Subsets.run,
    // measured exhaustively against per-attempt ground truth over every 6- and
    // 8-cycle trace. Everything below is that algorithm with its live subset map
    // replaced by compile-time state slots.

    // Width of an attempt counter. See alloc_monitor_state for why 64 and why no
    // saturating add.
    static constexpr uint32_t counter_bits = 64;

    static bool counted_carries(const ir::AssertionAutomaton& au, uint32_t s);

    static bool row_has(const std::vector<uint64_t>& row, uint32_t s);

    static bool row_has_at(const std::vector<uint64_t>& table, size_t base, uint32_t s);

    // Run `body` exactly `count` times.
    //
    // The ring path unrolls its per-attempt work because `depth` is a compile-time
    // constant. A counted monitor's verdicts are a run-time COUNT instead: every
    // attempt waiting in one state resolves in the cycle that state resolves, and
    // how many were waiting is not known until the trace runs. So this is a real
    // loop -- the one part of the counted path that is not straight-line, and only
    // ever around reporting. The automaton step itself stays branch-free.
    template<typename Body>
    void repeat_verdict(ir::Index count, const Body& body, ir::Builder& b, ir::SourceLoc l) {
        const ir::Ref  ty  = mask_type(counter_bits);
        const TempCell rem = make_temp_cell(ty, "sva_verdicts");
        b.binary(ir::Inst::Tag::Store, rem.lv(b, l), count, l);

        b.open_region();
        const ir::Index      cur  = b.unary(ir::Inst::Tag::Load, rem.lv(b, l), l);
        const ir::Index      pred = b.binary(ir::Inst::Tag::Ne, cur, zero_mask(counter_bits, b, l), l);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        body();
        const ir::ExtraSlice body_region = b.close_region();

        b.open_region();
        const ir::Index step_cur = b.unary(ir::Inst::Tag::Load, rem.lv(b, l), l);
        const ir::Index dst      = rem.lv(b, l);
        const ir::Index next     = b.binary(ir::Inst::Tag::Sub, step_cur, int_const(1, counter_bits, b, l), l);
        b.binary(ir::Inst::Tag::Store, dst, next, l);
        const ir::ExtraSlice step = b.close_region();

        b.loop(cond, pred, body_region, step, /*bottom_test=*/false, l);
    }

    void emit_counted_pends(ir::Index         count,
                            ir::FunctionId    body,
                            ir::Index         active,
                            const ir::Signal* filter,
                            ir::Builder&      b,
                            ir::SourceLoc     l);

    // Redirects assertion-expression operands to their Preponed shadows for the
    // extent of one atom. Scoped rather than a bare flag because lower_predicate
    // can fail() out of the middle of an expression.
    struct SampledScope {
        Lowerer& owner;
        bool     saved;

        SampledScope(Lowerer& lowerer, bool sampled)
          : owner(lowerer)
          , saved(lowerer.sva_sampled_) {
            owner.sva_sampled_ = sampled;
        }
        ~SampledScope() {
            owner.sva_sampled_ = saved;
        }
        SampledScope(const SampledScope&)            = delete;
        SampledScope& operator=(const SampledScope&) = delete;
    };

    // The $past history of one monitor: per sampled variable, a ring of that
    // variable's earlier sampled values.
    //
    // LRM 16.9.3 measures history in CLOCK TICKS -- a value change function
    // compares the sampled value against the one "from the most recent strictly
    // prior time step in which the clocking event occurred" -- while the Preponed
    // shadow is rewritten every TIME STEP. So the ring cannot live in the
    // sampling process, which has no idea where the ticks are; it is shifted by
    // the monitor, which runs per tick and already detects its own edge.
    //
    // Per MONITOR rather than per variable, which is correctness and not
    // convenience: $past(v) under `@(posedge a)` and under `@(posedge b)` name
    // different values, so a single shared ring per variable could answer at most
    // one of them. It also makes the read/shift order self-evident, because both
    // end up in one straight-line process body.
    struct SvaHistory {
        // One ring of earlier sampled values: entries[k] is the sample from k + 1
        // ADVANCES of this ring ago, so $past(v, n, ...) reads entries[n - 1] and
        // the length is the depth. Entries are created on demand, so it ends up as
        // the largest n the source asks of this ring -- a compile-time constant,
        // because a non-constant n is refused before one is ever requested.
        struct Ring {
            ir::Signal live{};
            // The $past gating expression deciding which ticks this ring advances
            // on (LRM 16.9.3), or null for the ungated ring. Part of the ring's
            // identity rather than a decoration on it: $past(v) and $past(v, 1, g)
            // count different ticks, so one ring per variable could answer at most
            // one of them.
            const Expression* gate = nullptr;
            // `gate` as a definite bit, evaluated once where the ring is created
            // and read again by the shift at the end of the body. Left zero and
            // unread on an ungated ring.
            ir::Index               gate_value{};
            std::vector<ir::Signal> entries;
        };
        // In first-use order, so the emitted shift is a function of the source
        // rather than of a hash order. Found by a linear scan: a monitor holds as
        // many rings as the (variable, gate) pairs its sampled value functions
        // name, which is a handful.
        std::vector<Ring> rings;
        // Name prefix for the ring signals, and the location to blame them on.
        std::string   base;
        ir::SourceLoc loc;
    };

    // Publishes the monitor's rings for the extent of its root evaluation. Scoped
    // for the same reason SampledScope is: eval_atom can fail() out of the middle
    // of an expression.
    struct HistoryScope {
        Lowerer&    owner;
        SvaHistory* saved;

        HistoryScope(Lowerer& lowerer, SvaHistory* hist)
          : owner(lowerer)
          , saved(lowerer.sva_history_) {
            owner.sva_history_ = hist;
        }
        ~HistoryScope() {
            owner.sva_history_ = saved;
        }
        HistoryScope(const HistoryScope&)            = delete;
        HistoryScope& operator=(const HistoryScope&) = delete;
    };

    static bool is_sampled_value_fn(std::string_view name);

    static bool is_pure_bitvec_sysfn(std::string_view name);

    ir::Signal sva_history_slot(ir::Signal live, const Expression* gate, uint32_t n, ir::Builder& b);

    void store_when(ir::Signal sig, ir::Index value, ir::Index when, ir::Builder& b, ir::SourceLoc l);

    void emit_history_shift(const SvaHistory& hist, ir::Index edge, ir::Builder& b, ir::SourceLoc l);

    ir::Index def_one(ir::Index v, ir::Builder& b, ir::SourceLoc l);
    ir::Index def_zero(ir::Index v, ir::Builder& b, ir::SourceLoc l);

    ir::Index edge_step(ir::Index old_v, ir::Index new_v, bool rising, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_sampled_value(std::string_view name, const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    void reject_unsampled_call(const Expression& expr);

    ir::Index eval_atom(const sva::Atom& atom, bool sampled, ir::Builder& b);

    ir::Index eval_case_match(const sva::Atom& atom, ir::Builder& b, ir::SourceLoc l);

    ir::Index eval_root(const sva::Result& lowered, uint32_t node, bool sampled, ir::Builder& b, ir::SourceLoc l);

    ExpectPorts emit_monitor(const sva::Result&          lowered,
                            const ir::AssertionMonitor& mon,
                            uint32_t                    id,
                            const AssertionActions&     act,
                            const MonitorControl*       ctl);

    void emit_counted_step(const ir::AssertionMonitor&   mon,
                           const MonitorState&           st,
                           // For expect_wake, which names the monitor whose waiters it
                           // drains. Unused by every other directive.
                           uint32_t                      id,
                           const AssertionActions&       act,
                           bool                          verdicts,
                           std::optional<ir::FunctionId> pass_body,
                           std::optional<ir::FunctionId> fail_body,
                           std::span<const ir::Index>    roots,
                           std::span<const ir::Index>    abort_cond,
                           ir::Index                     sat_o,
                           ir::Index                     active,
                           ir::Index                     edge,
                           SvaHistory&                   hist,
                           ir::Builder&                  b,
                           ir::SourceLoc                 l);

    void emit_counted_finish(const ir::AssertionMonitor& mon,
                             const MonitorState&         st,
                             const AssertionActions&     act,
                             ir::Builder&                b,
                             ir::SourceLoc               l);

    void emit_action_pends(const std::vector<ir::Index>& verdict,
                           ir::FunctionId                body,
                           ir::Index                     active,
                           bool                          unlikely,
                           const ir::Signal*             filter,
                           ir::Builder&                  b,
                           ir::SourceLoc                 l);

    ir::Index bare_seed_gate(const ir::AssertionMonitor& mon,
                             const MonitorState&         st,
                             ir::Builder&                b,
                             ir::SourceLoc               l);

    void emit_expect_step(const MonitorState& st,
                          uint32_t            id,
                          ir::Index           seeding,
                          ir::Index           active,
                          ir::Index           verdict,
                          ir::Builder&        b,
                          ir::SourceLoc       l);

    ir::Index any_verdict(const std::vector<ir::Index>& pass,
                          const std::vector<ir::Index>& fail,
                          ir::Builder&                  b,
                          ir::SourceLoc                 l);

    void emit_monitor_finish(const ir::AssertionMonitor& mon, const MonitorState& st, const AssertionActions& act);

    void lower_immediate_assertion(const ImmediateAssertionStatement& a, ir::Builder& b);

    void lower_conditional(const ConditionalStatement& c, ir::Builder& b);

    // One arm of a collected if / else-if chain: the arm's cond_predicate and the
    // statement it guards. The predicate is a span because `&&&` makes a
    // cond_predicate a list of AND-ed conditions, each either a plain boolean
    // expression or an `expr matches pattern` (LRM 12.4 / 12.6.2).
    struct IfArm {
        std::span<const ConditionalStatement::Condition> conds;
        const Statement*                                 body;
    };

    void lower_unique_if(const ConditionalStatement& c, ir::Builder& b);

    void dispatch_if_arms(const std::vector<IfArm>&     arms,
                          size_t                        k,
                          const std::vector<ir::Index>& preds,
                          const Statement*              final_else,
                          bool                          no_match_report,
                          const std::string&            banner,
                          ir::SourceLoc                 rloc,
                          ir::Builder&                  b);

    void lower_if_chain(const ConditionalStatement& c,
                        bool                        report_no_match,
                        const std::string&          banner,
                        ir::SourceLoc               rloc,
                        ir::Builder&                b);

    ir::Index true_const(ir::Builder& b, ir::SourceLoc l);

    void lower_var_decl(const VariableSymbol& vs, ir::Builder& b, bool force_frame = false);

    void emit_default_init(ir::Index lv, ir::Ref ty, ir::Builder& b, ir::SourceLoc l);

    void lower_var_initializer(const VariableSymbol& vs);

    const Expression* declaration_initializer(const VariableSymbol& vs);

    void finalize_preinit();

    void finalize_sva_history();

    ir::Signal sva_shadow_of(ir::Signal live);

    ir::Signal sampled_shadow_of(ir::Signal live, std::string_view what);

    void sva_sample_roots(const sva::Result& lowered);

    void finalize_preponed();

    // -- Clocking blocks (LRM 14) --
    //
    // A clocking block is a process, not just a declaration: LRM 14.7 makes it
    // "both a declaration and an instance of that declaration ... one copy is
    // created for each instance of the block containing the declaration (like an
    // always procedure)".
    //
    // That process does two things at every clocking event, in this order: update
    // the sampled value of each input clockvar, then trigger the event named by the
    // block (LRM 14.13, 14.10). Both belong to the Observed region (LRM 4.4.2.5),
    // and putting them there is what makes the rest fall out with no new scheduler
    // machinery:
    //
    //  * An Observed body's stores are ordinary design writes, and the time-slot
    //    loop re-settles the active set after the Observed run (LlvmBackend's
    //    emit_active_run), so a process waiting on the block or on one of its
    //    clockvars runs in the first Active region after the Observed region --
    //    exactly what LRM 14.10 specifies for a clocking block event. `@(cb)`,
    //    `@(cb.sig)` and `@(posedge cb.sig)` are therefore ordinary waits on
    //    ordinary signals.
    //  * 14.13's ordering ("update its sampled values before triggering the
    //    clocking block event") is source order inside one body.
    //  * An input skew of `#1step` is "the signal's last value immediately before
    //    the corresponding clock edge" (14.4), which LRM 4.4.2.1 equates to a
    //    Preponed sample of this time step -- the shadow the assertion sampler
    //    already maintains. An explicit `#0` skew is sampled in Observed, which is
    //    where 14.4 puts it.
    //  * Re-entering Observed within one time slot must not resample a `#0` input
    //    (14.13). It cannot: the clock latch is advanced on the first pass, so the
    //    second sees no edge.
    //
    // A clockvar is its own storage, NOT an alias of the signal it samples -- the
    // opposite of a modport port, which canonical_value_symbol collapses onto the
    // underlying signal. Reading a clockvar yields the value sampled at the most
    // recent clocking event, which differs from the live signal for most of every
    // cycle. The storage is registered under the ClockVarSymbol in sigmap_, so
    // every read of `cb.sig` resolves through the ordinary signal_of path and
    // expression lowering needs no clocking-specific code at all.
    struct ClockVarState {
        const ClockVarSymbol* sym = nullptr;
        // The sampled value `cb.sig` reads, for an `input` or `inout` clockvar.
        // Zero for an `output`, which LRM 14.3 makes it illegal to read.
        ir::Signal sampled{ 0 };
        // Set for an explicit `#0` input skew, which samples the live signal in the
        // Observed region; cleared for `#1step` (the LRM 14.4 default), which
        // samples the Preponed shadow of the step before.
        bool zero_skew = false;
        // A synchronous drive (LRM 14.16) of an `output` or `inout` clockvar
        // executes without blocking and defers its drive to the clocking event, so
        // the drive statement writes the value it evaluated into `drive_val` and
        // raises `drive_pend`; the block's Re-NBA body consumes both. Zero for an
        // `input`.
        ir::Signal drive_val{ 0 };
        ir::Signal drive_pend{ 0 };
        // Whether this clockvar can be driven at all.
        bool drives = false;
    };

    struct ClockingBlockState {
        // The clocking block event of LRM 14.10, as a hidden one-bit signal the
        // Observed body toggles. A toggle rather than a named event because every
        // waiter is level-sensitive (`@(cb)` carries no edge -- slang rejects one
        // with ClockingBlockEventEdge), and one toggle per clocking event wakes
        // exactly the processes an event trigger would, through the wait queues
        // that already exist.
        ir::Signal event{ 0 };
        // One-slot history of the clocking signal, so the Observed body can detect
        // its own edge: it is resumed once per settled instant, never woken.
        ir::Signal clkprev{ 0 };
        // Set by the Observed body when the clocking event fired in this time slot
        // and cleared by the Re-NBA body, which is what makes a drive land in the
        // Re-NBA region OF THAT slot (LRM 14.16) rather than in every slot. Sticky
        // across a second Observed pass of the same slot, which sees no edge. Zero
        // when the block has no drivable clockvar.
        ir::Signal fired{ 0 };
        // Clockvars in declaration order.
        std::vector<ClockVarState> vars;
        // Whether any clockvar is drivable, i.e. whether the block needs `fired`
        // and a Re-NBA process at all.
        bool has_drives = false;
    };

    const SignalEventControl& clocking_event(const ClockingBlockSymbol& cb);

    static bool is_program_signal(const ValueSymbol& raw);

    static const ValueSymbol* program_clock_signal(const Expression& e);

    bool zero_input_skew(const ClockingBlockSymbol& cb, const ClockVarSymbol& cv);

    void check_output_skew(const ClockingBlockSymbol& cb, const ClockVarSymbol& cv);

    void collect_clocking_block(const ClockingBlockSymbol& cb);

    void lower_clocking_block(const ClockingBlockSymbol& cb);

    void lower_clocking_drives(const ClockingBlockSymbol& cb);

    const ClockingBlockState& default_clocking_state();

    void lower_cycle_delay(const CycleDelayControl& cd, ir::Builder& b, ir::SourceLoc l);

    const ClockingBlockSymbol& global_clocking();

    const ClockingBlockSymbol* global_clock_call(const Expression& e);

    const TimingControl& clocking_event_of(const TimingControl& tc);

    void lower_clocking_drive(const ClockVarSymbol& cv, const AssignmentExpression& a, ir::Builder& b);

    const ClockingBlockState& clocking_state(const ClockingBlockSymbol& cb);

    void lower_for(const ForLoopStatement& f, ir::Builder& b);

    void lower_while(const WhileLoopStatement& w, ir::Builder& b);

    void lower_do_while(const DoWhileLoopStatement& d, ir::Builder& b);

    void lower_wait_stmt(const WaitStatement& w, ir::Builder& b);

    void lower_forever(const Statement& body, ir::Builder& b);

    void lower_repeat(const RepeatLoopStatement& r, ir::Builder& b);

    void lower_foreach(const ForeachLoopStatement& f, ir::Builder& b);

    ir::Ref foreach_dim_ref(const ForeachLoopStatement& f, size_t dim);

    ir::Index foreach_base_lv(const ForeachLoopStatement& f, size_t dim, ir::Builder& b, ir::SourceLoc l);

    void lower_foreach_dim(const ForeachLoopStatement& f, size_t dim, ir::Builder& b);

    void lower_foreach_runtime_dim(const ForeachLoopStatement& f,
                                   size_t                      dim,
                                   ir::Ref                     ty,
                                   ir::Index                   iter_lv,
                                   ir::Index                   len,
                                   ir::Builder&                b,
                                   ir::SourceLoc               l);

    void lower_foreach_assoc_dim(const ForeachLoopStatement& f,
                                 size_t                      dim,
                                 ir::Ref /*ty*/,
                                 ir::Index     iter_lv,
                                 ir::Builder&  b,
                                 ir::SourceLoc l);

    // Storage for a hidden compiler temporary: a loop counter, a once-evaluated
    // bound, an iteration flag. Nothing in the source names one and nothing
    // outside the construct that made it can read it, so its only requirement is
    // one cell per *activation*.
    //
    // A descriptor rather than a ready-made lvalue Index because a loop's cond /
    // body / step are three separate instruction regions and each needs its own
    // leaf; `lv` re-emits one wherever it is wanted.
    struct TempCell {
        ir::Ref    type     = ir::Ref::Invalid;
        bool       is_local = false;
        uint32_t   local    = 0;
        ir::Signal sig{};

        ir::Index lv(ir::Builder& b, ir::SourceLoc l) const {
            return is_local ? b.local(local, type, l) : b.signal(sig, l);
        }
    };

    TempCell make_temp_cell(ir::Ref type, std::string_view role);

    ir::Signal declare_temp(ir::Ref type, std::string_view role);

    void lower_for_step(const Expression& e, ir::Builder& b);

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

    uint32_t case_pragma_bits(const CaseStatement& c) const;

    void check_case_pragmas(const CaseStatement& c);

    CaseCompareType common_case_type(const CaseStatement& c);

    static bool is_unbounded_bound(const Expression& e);

    CaseCompareType common_inside_type(const CaseStatement& c);

    ir::Index lower_inside_label_pred(const Expression& label, ir::Index sel, ir::Ref ty, ir::Builder& b);

    ir::Index lower_inside_item_pred(const CaseStatement::ItemGroup& item, ir::Index sel, ir::Ref ty, ir::Builder& b);

    void lower_case_inside_item(const CaseStatement&                  c,
                                size_t                                k,
                                ir::Index                             sel,
                                ir::Ref                               ty,
                                uint32_t                              width,
                                uint32_t                              key_width,
                                const std::vector<std::vector<char>>& keep,
                                const slang::ast::ASTContext&         astCtx,
                                bool                                  report_viol,
                                ir::Builder&                          b);

    void lower_case_inside_unique(const CaseStatement&          c,
                                  ir::Index                     sel,
                                  ir::Ref                       ty,
                                  uint32_t                      width,
                                  uint32_t                      key_width,
                                  const slang::ast::ASTContext& astCtx,
                                  bool                          no_match_report,
                                  ir::Builder&                  b);

    void lower_case_inside(const CaseStatement& c, ir::Builder& b);

    static bool has_real_case_operand(const CaseStatement& c);

    ir::Index real_case_label_pred(const Expression& label, ir::Index sel, ir::Ref rty, ir::Builder& b);

    ir::Index real_case_item_pred(const CaseStatement::ItemGroup& item, ir::Index sel, ir::Ref rty, ir::Builder& b);

    ir::Index string_case_label_pred(const Expression& label, ir::Index sel, ir::Builder& b);

    ir::Index string_case_item_pred(const CaseStatement::ItemGroup& item, ir::Index sel, ir::Builder& b);

    void lower_case_string_item(const CaseStatement& c, size_t k, ir::Index sel, bool report_viol, ir::Builder& b);

    void lower_case_string(const CaseStatement& c, ir::Builder& b);

    void lower_case_real_item(const CaseStatement& c,
                              size_t               k,
                              ir::Index            sel,
                              ir::Ref              rty,
                              bool                 report_viol,
                              ir::Builder&         b);

    void lower_case_real(const CaseStatement& c, ir::Builder& b);

    std::vector<std::vector<char>> case_dedup(const CaseStatement&          c,
                                              uint32_t                      width,
                                              const slang::ast::ASTContext& astCtx);

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

    std::optional<SwitchRun> find_switch_run(const CaseStatement&                  c,
                                             size_t                                k,
                                             uint32_t                              width,
                                             uint32_t                              key_width,
                                             const std::vector<std::vector<char>>* keep,
                                             const slang::ast::ASTContext&         astCtx);

    std::vector<ir::Builder::SwitchArmBuild> lower_switch_arm_bodies(const std::vector<SwitchArmPlan>& plan,
                                                                     ir::Builder&                      b);

    // A one-hot reverse-case plan: the base packed value and, per arm, the IR
    // bit position it selects and the body to run. `descending` picks Clz
    // (highest-bit priority) over Ctz (lowest-bit).
    struct OneHotPlan {
        const Expression*                                  base;
        std::vector<std::pair<uint64_t, const Statement*>> arms; // (IR bit, body)
        bool                                               descending;
        uint32_t                                           base_width;
    };

    std::optional<OneHotPlan> build_onehot_plan(const CaseStatement& c);

    void emit_onehot(const CaseStatement& c, const OneHotPlan& plan, ir::Builder& b);

    void lower_case(const CaseStatement& c, ir::Builder& b);

    void lower_case_unique(const CaseStatement&          c,
                           ir::Index                     sel,
                           ir::Inst::Tag                 cmp,
                           ir::Ref                       ty,
                           uint32_t                      width,
                           uint32_t                      key_width,
                           const slang::ast::ASTContext& astCtx,
                           bool                          no_match_report,
                           ir::Builder&                  b);

    void emit_overlap_check(const std::vector<ir::Index>& preds,
                            const std::string&            banner,
                            ir::SourceLoc                 rloc,
                            ir::Builder&                  b);

    void dispatch_preds(const CaseStatement&          c,
                        size_t                        k,
                        ir::Index                     sel,
                        uint32_t                      width,
                        uint32_t                      key_width,
                        const slang::ast::ASTContext& astCtx,
                        const std::vector<ir::Index>& preds,
                        bool                          no_match_report,
                        ir::Builder&                  b,
                        bool                          allow_switch = true);

    void lower_case_item(const CaseStatement&                  c,
                         size_t                                k,
                         ir::Index                             sel,
                         ir::Inst::Tag                         cmp,
                         ir::Ref                               ty,
                         uint32_t                              width,
                         uint32_t                              key_width,
                         const std::vector<std::vector<char>>& keep,
                         const slang::ast::ASTContext&         astCtx,
                         bool                                  report_viol,
                         ir::Builder&                          b);

    void lower_randcase(const slang::ast::RandCaseStatement& rc, ir::Builder& b);

    void lower_randcase_arm(const slang::ast::RandCaseStatement& rc,
                            size_t                               k,
                            ir::Index                            acc,
                            ir::Index                            draw,
                            const std::vector<ir::Index>&        weights,
                            ir::Ref                              u32,
                            ir::Builder&                         b);

    // -- randsequence (LRM 18.17) --
    //
    // Each production becomes its own ir::Function; the grammar is NOT inlined,
    // because LRM 18.17.7's own GenQueue example is left-recursive and a recursive
    // production is the clause's stated way to loop. Every production of one
    // randsequence shares one signature, so any may call any other
    // (docs/design/randsequence.md §6):
    //
    //   randseq.<name>( <this>, output bit $abort, <output T $result>,
    //                   <declared formals>, <captures...> )
    //
    // `$abort` carries `break` (18.17.6) up the call chain: the production that
    // raises it returns at once, and every caller tests it after every production
    // call and returns in turn, so the whole block unwinds to the statement after
    // `endsequence`. `$result` carries a non-void production's `return expr`
    // (18.17.7) into the rule variable of the rule that generated it. Both are
    // `output` formals, so the backend's own copy-out epilogue delivers them and a
    // production that consumes time (a task) needs nothing extra. `return` itself
    // is just the production function's RetVoid, which is exactly 18.17.6's
    // "aborts the generation of the current production". `<captures>` are the
    // enclosing subroutine frame's symbols the block reads, passed by reference; a
    // process body has no frame and the list is empty, since a module signal
    // resolves directly from any body.
    using RsProd     = RandSeqProductionSymbol;
    using RsProdBase = RandSeqProductionSymbol::ProdBase;
    using RsProdItem = RandSeqProductionSymbol::ProdItem;
    using RsProdKind = RandSeqProductionSymbol::ProdKind;

    // Declared in full further down, next to the function lowering and the call
    // marshalling that fill them; named here because the randsequence helpers below
    // take them by reference.
    struct FnCtx;
    struct CopyOutArg;
    struct OpenBlock;

    struct RandSeqCtx {
        // Production -> the ir::Function it lowered to.
        ankerl::unordered_dense::map<const RsProd*, ir::FunctionId> ids;
        // Enclosing-frame symbols every production takes as a trailing formal, in
        // first-reference order -- that order IS the formal order. An ordinary frame
        // symbol goes by reference; one already living in a GC box (a detaching
        // fork captured it too) goes as its box HANDLE by value, exactly as a fork
        // child takes it.
        struct Capture {
            const ValueSymbol* sym;
            bool               boxed;
        };
        std::vector<Capture> captures;
        // The enclosing method's `this` automatic, spliced as formal 0 when the
        // block reads a class member.
        const ValueSymbol* this_var   = nullptr;
        ir::Ref            this_class = ir::Ref::Invalid;
        // Productions are coroutines exactly when the randsequence sits in one.
        bool is_task = false;
        // A production named as a `rand join` operand (LRM 18.17.5) additionally
        // gets two entry points, because interleaving has to advance a sequence one
        // depth-1 unit at a time and a plain call runs it to completion: `plan`
        // performs its rule draw and reports the chosen rule and that rule's unit
        // count, `step` generates one numbered unit of one numbered rule.
        struct Steps {
            ir::FunctionId plan;
            ir::FunctionId step;
        };
        ankerl::unordered_dense::map<const RsProd*, Steps> steps;
    };

    // Per-rule bookkeeping for LRM 18.17.7's implicit result variables.
    struct RandSeqRuleVars {
        // slang's synthesized variable for each non-void production the rule names.
        ankerl::unordered_dense::map<std::string_view, const VariableSymbol*> byname;
        // 1-based index of each syntactic production reference among the references
        // to the same production within this rule.
        ankerl::unordered_dense::map<const RsProdItem*, uint32_t> occ;
        // How many times each production is named -- one means the variable is a
        // scalar, more means it is a `[1:N]` array.
        ankerl::unordered_dense::map<const RsProd*, uint32_t> total;
    };

    // Walk everything a randsequence block can read. slang's generic ASTVisitor
    // stops dead at RandSequenceStatement (it has neither visitExprs nor
    // visitStmts) and RandSeqProductionSymbol::visitExprs deliberately skips code
    // blocks, so the walk has to be spelled out.
    template<typename V>
    static void visit_randseq(const RandSequenceStatement& rs, V& v) {
        for (const RsProd* p : rs.productions) {
            if (p == nullptr) {
                continue;
            }
            for (const FormalArgumentSymbol* fa : p->arguments) {
                if (const Expression* dv = fa->getDefaultValue()) {
                    dv->visit(v);
                }
            }
            p->visitExprs(v);
            for (const RsProd::Rule& rule : p->getRules()) {
                for (const RsProdBase* prod : rule.prods) {
                    if (prod->kind != RsProdKind::CodeBlock) {
                        continue;
                    }
                    if (const Statement* s = prod->as<RsProd::CodeBlockProd>().block->tryGetStatement()) {
                        s->visit(v);
                    }
                }
                if (rule.codeBlock) {
                    if (const Statement* s = rule.codeBlock->block->tryGetStatement()) {
                        s->visit(v);
                    }
                }
            }
        }
    }

    void lower_randsequence(const RandSequenceStatement& rs, ir::Builder& b);

    void declare_capture_formals(const RandSeqCtx&        rs,
                                 FnCtx&                   ctx,
                                 std::vector<ir::Ref>&    param_types,
                                 std::vector<ir::ArgDir>& arg_dirs);

    void push_capture_args(const RandSeqCtx& rs, std::vector<ir::Index>& args, ir::Builder& b, ir::SourceLoc l);

    const RandSeqCtx& randseq_block(const RandSequenceStatement& rs);

    void check_rand_join_operand(const RsProd& p);

    static uint32_t rand_join_units(const RsProd::Rule& rule);

    void build_production(const RsProd& p, const RandSeqCtx& rs, ir::FunctionId id);

    void build_production_plan(const RsProd& p, const RandSeqCtx& rs, ir::FunctionId id);

    void build_production_step(const RsProd& p, const RandSeqCtx& rs, ir::FunctionId id);

    void gen_step_rules(const RandSeqCtx&             rs,
                        std::span<const RsProd::Rule> rules,
                        size_t                        r,
                        ir::Index                     rule_val,
                        ir::Index                     pos_lv,
                        ir::Ref                       i32,
                        ir::Builder&                  b);

    void gen_step_units(const RandSeqCtx&   rs,
                        const RsProd::Rule& rule,
                        uint32_t            u,
                        ir::Index           pos_lv,
                        ir::Ref             i32,
                        ir::Builder&        b);

    void gen_production(const RsProd& p, const RandSeqCtx& rs, ir::Builder& b);

    // LRM 18.17.1's weighted choice among a production's alternative rules, with
    // `emit(k, b)` producing whatever the k-th rule means to the caller: generating
    // it (gen_production) or just recording its index (build_production_plan, which
    // must draw at the same point so an interleaved production's stream matches a
    // plain one's).
    //
    // Same weight domain, sum precision and zero-total rule as randcase (18.16):
    // unsigned summands at the widest weight's self-determined precision, one draw
    // over the total, and no draw at all when the total is zero -- there is then no
    // rule to pick, and drawing would shift the stream.
    template<typename F>
    void gen_weighted_rules(const RsProd& p, std::span<const RsProd::Rule> rules, ir::Builder& b, F&& emit) {
        const ir::SourceLoc l = to_source_loc(p.location);

        // An omitted weight is the integer literal 1 (LRM 18.17.1), whose
        // self-determined precision is an integer's -- 32 bits. It has to fold into
        // the common width like any other summand: at the literal 1-bit minimum,
        // two default weights would sum to 0 and the production would generate
        // nothing.
        uint32_t width = 1;
        for (const RsProd::Rule& r : rules) {
            width = std::max(width, r.weightExpr != nullptr ? r.weightExpr->type->getBitWidth() : 32u);
        }
        if (width > 32) {
            // TODO: a wider sum needs the LRM's several-draw scheme, stitching
            // multiple $urandom_range results into one number ("This can result in
            // multiple calls to $urandom_range() to handle numbers greater than 32
            // bits", LRM 18.16, whose weight rules 18.17.1 shares).
            //
            // Deliberately shared with randcase's identical guard rather than lifted
            // here alone: the two constructs weigh alternatives by the same clause,
            // so they must agree, and stitching changes how many values a selection
            // draws from the process stream -- a documented contract
            // (docs/design/randsequence.md §12), not an implementation detail. One
            // change, both sites, one recorded stream shift.
            fail("randsequence production weight wider than 32 bits is not supported");
        }
        const ir::Ref          wty = make_int_type(width, false, false);
        std::vector<ir::Index> weights;
        weights.reserve(rules.size());
        for (const RsProd::Rule& r : rules) {
            // "If no weight is specified, a production shall use a weight of 1"
            // (18.17.1). A written weight is evaluated on every activation, in rule
            // order, which is what lets it track design state.
            weights.push_back(r.weightExpr != nullptr ? b.resize(lower_expr(*r.weightExpr, b), wty, loc(*r.weightExpr))
                                                      : b.constant(module_.pool.put_int(wty, 1), l));
        }
        ir::Index sum = weights[0];
        for (size_t k = 1; k < weights.size(); k++) {
            sum = b.binary(ir::Inst::Tag::Add, sum, weights[k], l);
        }

        const ir::Ref   u32  = make_int_type(32, false, false);
        const ir::Index zero = b.constant(module_.pool.put_int(wty, 0), l);
        b.open_region();
        const ir::Index wide = b.resize(sum, u32, l);
        const ir::Index one  = b.constant(module_.pool.put_int(u32, 1), l);
        const ir::Index top  = b.binary(ir::Inst::Tag::Sub, wide, one, l);
        const ir::Index low  = b.constant(module_.pool.put_int(u32, 0), l);
        const ir::Index draw = b.urandom_range(top, low, l);
        gen_rule_arm(rules, 0, zero, draw, weights, u32, b, emit);
        const ir::ExtraSlice pick_region = b.close_region();
        b.open_region();
        const ir::ExtraSlice none_region = b.close_region();
        b.cond(b.binary(ir::Inst::Tag::Ne, sum, zero, l), pick_region, none_region, l);
    }

    // One arm of the rule-selection ladder, shaped exactly like randcase's.
    template<typename F>
    void gen_rule_arm(std::span<const RsProd::Rule> rules,
                      size_t                        k,
                      ir::Index                     acc,
                      ir::Index                     draw,
                      const std::vector<ir::Index>& weights,
                      ir::Ref                       u32,
                      ir::Builder&                  b,
                      F&                            emit) {
        if (k == weights.size()) {
            return;
        }
        const ir::SourceLoc l    = to_source_loc(rules[k].ruleBlock->location);
        const ir::Index     next = b.binary(ir::Inst::Tag::Add, acc, weights[k], l);
        const ir::Index     pred = b.binary(ir::Inst::Tag::Lt, draw, b.resize(next, u32, l), l);

        b.open_region();
        emit(k, b);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        gen_rule_arm(rules, k + 1, next, draw, weights, u32, b, emit);
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(pred, then_region, else_region, l);
    }

    void gen_rule(const RsProd::Rule& rule, const RandSeqCtx& rs, ir::Builder& b);

    RandSeqRuleVars rule_vars(const RsProd::Rule& rule, bool declare, ir::Builder& b);

    void gen_rand_join(const RsProd::Rule& rule, const RandSeqCtx& rs, const RandSeqRuleVars& vars, ir::Builder& b);

    ir::Index bind_rand_join_formal(const FormalArgumentSymbol& fa,
                                    const Expression&           actual_expr,
                                    std::vector<CopyOutArg>&    outs,
                                    ir::Builder&                b,
                                    ir::SourceLoc               l);

    static void rand_join_rule_vars(const RsProd& p, std::vector<const VariableSymbol*>& out);

    // The `rand join` length-bias expression, quantized to kRandJoinScale-ths and
    // clamped: LRM 18.17.5 constrains it to [0.0, 1.0] and defaults it to 0.5.
    static constexpr int64_t kRandJoinScale = 1024;

    ir::Index rand_join_bias(const RsProd::Rule& rule, ir::Ref i32, ir::Builder& b, ir::SourceLoc l);

    // One arm of the interleaving pick: advance operand k by a single unit.
    template<typename OpVec>
    void gen_rand_join_arm(const OpVec&                  ops,
                           size_t                        k,
                           ir::Index                     acc,
                           ir::Index                     draw,
                           const std::vector<ir::Index>& weights,
                           ir::Ref                       u32,
                           const RandSeqCtx&             rs,
                           const RandSeqRuleVars&        vars,
                           ir::Builder&                  b,
                           ir::SourceLoc                 l) {
        if (k == ops.size()) {
            return;
        }
        const ir::Index next = b.binary(ir::Inst::Tag::Add, acc, weights[k], l);
        const ir::Index pred = b.binary(ir::Inst::Tag::Lt, draw, next, l);

        b.open_region();
        gen_rand_join_step(ops[k], rs, vars, b, l);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        gen_rand_join_arm(ops, k + 1, next, draw, weights, u32, rs, vars, b, l);
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(pred, then_region, else_region, l);
    }

    template<typename Op>
    void gen_rand_join_step(const Op&              op,
                            const RandSeqCtx&      rs,
                            const RandSeqRuleVars& vars,
                            ir::Builder&           b,
                            ir::SourceLoc          l) {
        const ir::Ref i32    = make_int_type(32, false, true);
        const auto    kconst = [&](int64_t v) { return b.constant(module_.pool.put_int(i32, v), l); };

        const RandSeqCtx::Steps& st = rs.steps.at(op.prod);
        std::vector<ir::Index>   args;
        if (rs.this_var != nullptr) {
            args.push_back(this_handle(b, l));
        }
        args.push_back(abort_lvalue(b, l));
        if (op.result) {
            args.push_back(op.result->lv(b, l));
        }
        args.push_back(b.unary(ir::Inst::Tag::Load, op.rule_idx.lv(b, l), l));
        args.push_back(b.unary(ir::Inst::Tag::Load, op.pos.lv(b, l), l));
        args.push_back(op.stop.lv(b, l));
        args.insert(args.end(), op.formals.begin(), op.formals.end());
        args.insert(args.end(), op.rulevars.begin(), op.rulevars.end());
        push_capture_args(rs, args, b, l);
        if (rs.is_task) {
            b.task_call(st.step, args, l);
        } else {
            b.call(st.step, args, l);
        }

        // Advance this sequence: one unit consumed, unless the production aborted
        // (LRM 18.17.6's `return`), in which case its remaining units are dropped
        // and the other sequences carry on.
        const ir::Index pos_lv = op.pos.lv(b, l);
        const ir::Index cur    = b.unary(ir::Inst::Tag::Load, op.pos.lv(b, l), l);
        const ir::Index one    = kconst(1);
        const ir::Index next   = b.binary(ir::Inst::Tag::Add, cur, one, l);
        b.binary(ir::Inst::Tag::Store, pos_lv, next, l);
        const ir::Index left     = b.unary(ir::Inst::Tag::Load, op.nleft.lv(b, l), l);
        const ir::Index dec      = b.binary(ir::Inst::Tag::Sub, left, kconst(1), l);
        const ir::Index nleft_lv = op.nleft.lv(b, l);
        const ir::Index stopped  = b.unary(ir::Inst::Tag::RedOr, b.unary(ir::Inst::Tag::Load, op.stop.lv(b, l), l), l);
        const ir::Index zero     = kconst(0);
        const ir::Index nleft    = b.select(stopped, zero, dec, l);
        b.binary(ir::Inst::Tag::Store, nleft_lv, nleft, l);
        if (op.result) {
            // Only a production that actually returned a value wrote the slot, so
            // the rule variable is written exactly then; otherwise it keeps the type
            // default lower_var_decl gave it (LRM 18.17.7's "undefined value").
            b.open_region();
            const ir::Index place = result_slot(*op.item, *op.prod, vars, b, l);
            b.binary(ir::Inst::Tag::Store, place, b.unary(ir::Inst::Tag::Load, op.result->lv(b, l), l), l);
            const ir::ExtraSlice then_region = b.close_region();
            b.open_region();
            const ir::ExtraSlice else_region = b.close_region();
            b.cond(b.unary(ir::Inst::Tag::RedOr, b.unary(ir::Inst::Tag::Load, op.stop.lv(b, l), l), l),
                   then_region,
                   else_region,
                   l);
        }
        emit_abort_check(b, l);
    }

    static void count_rule_items(const RsProd::Rule& rule, RandSeqRuleVars& vars);

    void gen_prod(const RsProdBase& prod, const RandSeqCtx& rs, const RandSeqRuleVars& vars, ir::Builder& b);

    void gen_code_block(const RsProd::CodeBlockProd& cb, ir::Builder& b);

    void gen_case_prod(const RsProd::CaseProd& cp, const RandSeqCtx& rs, const RandSeqRuleVars& vars, ir::Builder& b);

    void gen_case_arm(const RsProd::CaseProd& cp,
                      size_t                  k,
                      ir::Index               sel,
                      ir::Ref                 ty,
                      const RandSeqCtx&       rs,
                      const RandSeqRuleVars&  vars,
                      ir::Builder&            b);

    void gen_prod_item(const RsProdItem& item, const RandSeqCtx& rs, const RandSeqRuleVars& vars, ir::Builder& b);

    void emit_result_copy(ir::Index place, ir::Index temp, ir::Ref ty, ir::Builder& b, ir::SourceLoc l);

    ir::Index abort_lvalue(ir::Builder& b, ir::SourceLoc l);

    void emit_abort_check(ir::Builder& b, ir::SourceLoc l);

    ir::Index result_slot(const RsProdItem&      item,
                          const RsProd&          target,
                          const RandSeqRuleVars& vars,
                          ir::Builder&           b,
                          ir::SourceLoc          l);

    // -- System tasks --

    ir::Index lower_system_call(const CallExpression& call, ir::Builder& b);

    void lower_call(const CallExpression& call, ir::Builder& b);

    void lower_dumpvars(const CallExpression& call, ir::Builder& b);

    void lower_readmem(const CallExpression& call, bool hex, ir::Builder& b);

    void lower_timeformat(const CallExpression& call, ir::Builder& b);

    void lower_printtimescale(const CallExpression& call, ir::Builder& b);

    ir::Index scaled_delay(const Expression& amount, ir::Builder& b);

    ir::Index saturating_ticks(ir::Index     scaled,
                               uint64_t      tick_mul,
                               ir::Builder&  b,
                               ir::SourceLoc l,
                               bool          is_signed = false);

    ir::Index lower_string_arg(const Expression& expr, ir::Builder& b);

    std::vector<char> scan_conversion_chars(std::string_view fmt);

    ir::Index lower_scanf(const CallExpression& call, bool is_sscanf, ir::Builder& b, ir::SourceLoc l);

    // An enum member in declaration order: its constant value and source name.
    struct EnumMember {
        slang::SVInt     value;
        std::string_view name;
    };

    std::vector<EnumMember> enum_members(const slang::ast::EnumType& et);

    // An enum's name table for `enum_name`: each member's value as its must-plane
    // bytes plus its source name. The value buffers must outlive the builder call,
    // so they travel with the rows; `valbufs` is reserved up front so the spans in
    // `rows` never dangle behind a reallocation.
    struct EnumNameTable {
        std::vector<std::vector<uint8_t>>     valbufs;
        std::vector<ir::Builder::EnumNameRow> rows;
    };

    EnumNameTable enum_name_table(const slang::ast::EnumType& et);

    ir::Index lower_enum_method(std::string_view            name,
                                const slang::ast::EnumType& et,
                                const CallExpression&       call,
                                ir::Builder&                b,
                                ir::SourceLoc               l);

    std::string format_typename(const slang::ast::Type& type);

    uint32_t count_control_class(const Expression& ctrl);

    ir::Index lower_bit_query(std::string_view      name,
                              const CallExpression& call,
                              ir::Builder&          b,
                              const ir::SourceLoc   l);

    void emit_cast_fatal(const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    void emit_cast_fatal_unless(ir::Index valid, const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_cast(const CallExpression& call, ir::Builder& b, bool task_form);

    ir::Index lower_sysfunc(const CallExpression& call, ir::Builder& b);

    ir::Index lower_string_method(std::string_view name, const CallExpression& call, ir::Builder& b);

    std::string instance_path() const;

    std::string hier_name() const;

    std::string lib_cell_name() const;

    char check_specifier(const clotho::FormatSpec& spec);

    void append_format_literal(std::string& fmt, std::string_view text, std::vector<char>& pending);

    void expand_p_arg(const Expression& arg, std::string& frag, std::vector<ir::Index>& args, ir::Builder& b);

    static const Scope& union_scope(const slang::ast::Type& ct);

    ir::Index packed_slice(ir::Index v, uint32_t lsb, ir::Ref fty, ir::Builder& b, ir::SourceLoc l);

    bool expand_p_packed(ir::Index               v,
                         const slang::ast::Type& sty,
                         std::string&            frag,
                         std::vector<ir::Index>& args,
                         ir::Builder&            b,
                         ir::SourceLoc           l);

    static std::optional<std::string_view> handle_p_name(const slang::ast::Type& ct);

    ir::Index handle_p_render(ir::Index v, std::string_view name, ir::Builder& b, ir::SourceLoc l);

    void expand_p_scalar(ir::Index               v,
                         const slang::ast::Type& sty,
                         std::string&            frag,
                         std::vector<ir::Index>& args,
                         ir::Builder&            b,
                         ir::SourceLoc           l);

    void expand_p_lvalue(ir::Index               lv,
                         ir::Ref                 ty,
                         const slang::ast::Type* sty,
                         std::string&            frag,
                         std::vector<ir::Index>& args,
                         ir::Builder&            b,
                         ir::SourceLoc           l);

    ir::Index build_container_p_string(ir::Index               lv,
                                       ir::Ref                 ty,
                                       const ir::Type&         t,
                                       const slang::ast::Type* sty,
                                       ir::Builder&            b,
                                       ir::SourceLoc           l);

    ir::Index build_assoc_p_string(ir::Index               lv,
                                   ir::Ref                 ty,
                                   const ir::Type&         t,
                                   const slang::ast::Type* sty,
                                   ir::Builder&            b,
                                   ir::SourceLoc           l);

    static std::pair<size_t, size_t> find_p_spec(std::string_view fmt);

    void build_format(std::span<const Expression* const> format_args,
                      std::string&                       fmt,
                      std::vector<ir::Index>&            args,
                      ir::Builder&                       b,
                      char                               default_conv = 'd',
                      std::string_view                   lead_text    = {});

    static char display_radix(std::string_view name, std::string_view base);

    void emit_severity(ir::Severity                       level,
                       const std::string&                 banner,
                       std::span<const Expression* const> msg_args,
                       const CallExpression&              call,
                       ir::Builder&                       b);

    void lower_severity_task(const CallExpression& call, std::string_view name, ir::Builder& b);

    void lower_display(const CallExpression& call, bool newline, ir::Builder& b, char default_conv);

    void lower_strobe(const CallExpression& call, ir::Builder& b, char default_conv, bool fd_arg);

    void lower_monitor(const CallExpression& call, ir::Builder& b, char default_conv, bool fd_arg);

    void lower_fdisplay(const CallExpression& call, bool newline, ir::Builder& b, char default_conv);

    std::optional<std::string> const_format_text(const Expression& e);

    static std::string_view value_type_noun(const slang::ast::Type& t);

    ir::Index lower_dynamic_format(const Expression&                  fmt_expr,
                                   std::span<const Expression* const> value_args,
                                   size_t                             first_pos,
                                   std::vector<ir::Index>&            args,
                                   ir::Builder&                       b,
                                   std::string_view                   who);

    void lower_sformat(const CallExpression& call, bool is_sformat, ir::Builder& b, char default_conv = 'd');

    ir::Index lower_sformatf(const CallExpression& call, ir::Builder& b);

    ir::Index scale_to_ticks(const Expression& arg, ir::Index v, ir::Builder& b);

    static bool intra_nb_target_path(const Expression& e);

    static bool intra_nb_target_supported(const Expression& e);

    static bool intra_nb_target_decomposes(const Expression& e);

    std::optional<IntraNbTiming> lower_intra_nb_event(const AssignmentExpression& a, ir::Builder& b);

    std::optional<IntraNbTiming> lower_intra_nb_timing(const AssignmentExpression& a, ir::Builder& b);

    bool intra_nb_delay_is_const_zero(const Expression& delay_expr);

    IntraNbTiming lower_intra_nb_delay(const Expression& delay_expr, ir::Builder& b);

    void check_subroutine_nba_target(const Expression& lhs);

    ir::Index lower_assign(const AssignmentExpression& a, ir::Builder& b);

    void emit_dyn_array_new_expr(ir::Index                             dst,
                                 const slang::ast::NewArrayExpression& na,
                                 ir::Ref                               aty,
                                 ir::Builder&                          b,
                                 ir::SourceLoc                         l);

    void emit_fixed_array_into_dynarr(ir::Index     dst,
                                      const Expression& src,
                                      ir::Ref       dst_ty,
                                      uint32_t      n,
                                      ir::Builder&  b,
                                      ir::SourceLoc l);

    void emit_fixed_array_into_queue(ir::Index         dst,
                                     const Expression& src,
                                     ir::Ref           queue_ty,
                                     ir::Builder&       b,
                                     ir::SourceLoc      l);

    ir::Index lower_dyn_array_assign(const AssignmentExpression& a, ir::Builder& b);

    void emit_container_store(ir::Index dst, ir::Ref ty, const Expression& rhs, ir::Builder& b, ir::SourceLoc l);

    void emit_dyn_array_store(ir::Index dst, ir::Ref aty, const Expression& rhs_expr, ir::Builder& b, ir::SourceLoc l);

    void emit_dyn_pattern_fill(ir::Index                                          dst,
                               ir::Ref                                            aty,
                               const slang::ast::AssignmentPatternExpressionBase& pat,
                               ir::Builder&                                       b,
                               ir::SourceLoc                                      l);

    ir::Index lower_dyn_array_method(std::string_view      name,
                                     const CallExpression& call,
                                     ir::Builder&          b,
                                     ir::SourceLoc         l);

    void guard_ordering_sort(std::string_view name, const CallExpression& call);

    bool sort_uses_iter_path(const CallExpression& call);

    // Emitted state for one keyed sort. A struct so the emit helpers take a
    // context rather than a dozen cells: the loops are the meat and read better
    // without the parameter noise (TigerStyle).
    struct SortIter {
        ir::Index arr{};
        ir::Index ord{}; // the permutation being sorted: dynamic array of int
        ir::Index buf{}; // merge scratch, same shape
        ir::Ref   aty  = ir::Ref::Invalid;
        ir::Ref   oty  = ir::Ref::Invalid;
        ir::Ref   i32  = ir::Ref::Invalid;
        ir::Ref   i64  = ir::Ref::Invalid;
        ir::Ref   bit1 = ir::Ref::Invalid;
        // Element count, merge run width, run start, run midpoint / end, the two
        // run cursors and the output cursor. 64-bit so `width + width` cannot
        // overflow at the last doubling, which a 32-bit counter can.
        TempCell n, w, lo, mid, hi, li, rj, out;
        // The two element positions a comparison binds, and which side won.
        TempCell                       pos_a, pos_b, take;
        // Holds the right-hand key across the left-hand key's evaluation. See
        // sort_right_wins for why a cell and not an IR value.
        TempCell                       key_hold;
        const Expression*              iter_expr = nullptr;
        const slang::ast::ValueSymbol* iter_var  = nullptr;
        bool                           agg_elem  = false;
        bool                           key_real  = false;
        // A 4-state key, hence one that can be unknown and needs the total order
        // sort_right_wins defines. Decided at lower time, so a 2-state key emits
        // exactly the compare it did before.
        bool                           key_four  = false;
        bool                           desc      = false; // rsort
    };

    ir::Index iconst(ir::Ref ty, uint64_t v, ir::Builder& b, ir::SourceLoc l);

    // How an array-method receiver's iteration positions map to the indices the
    // LRM reports for them. Position `p` (0 .. count-1, walked in the order
    // 7.12.1 defines as leftmost-indexed element first) holds the element whose
    // declared index is `first + step * p`.
    //
    // A queue or dynamic array is 0-based ascending, so `first == 0 && step == 1`
    // and every expression built from the map collapses back to the position
    // itself -- which is why one path serves every receiver instead of two. A
    // fixed-size unpacked array carries its declared range: `int q[3:7]` puts
    // index 3 at position 0, and the DESCENDING `int q[7:3]` puts index 7 there,
    // because 7.12.1's first element is "closest to the leftmost indexed
    // element" -- the left bound, whichever side of the range it is.
    struct DeclIndexMap {
        int32_t first = 0;
        int32_t step  = 1;
        // Element count. Only meaningful (and only known at lower time) when the
        // receiver is fixed-size; the others query their handle.
        uint32_t fixed_count = 0;
        bool     fixed       = false;
    };

    static DeclIndexMap decl_index_map(const Expression& recv);

    ir::Index decl_index_at(const DeclIndexMap& m, ir::Index pos, ir::Ref i32, ir::Builder& b, ir::SourceLoc l);

    void sort_bind(const SortIter& s, const TempCell& pos, ir::Builder& b, ir::SourceLoc l);

    ir::Index sort_key(const SortIter& s, const TempCell& pos, ir::Builder& b, ir::SourceLoc l);

    ir::Index sort_right_wins(const SortIter& s, ir::Builder& b, ir::SourceLoc l);

    void emit_merge_step(const SortIter& s, ir::Builder& b, ir::SourceLoc l);

    void emit_merge_pass(const SortIter& s, ir::Builder& b, ir::SourceLoc l);

    void emit_sort_permute(const SortIter& s, bool queue, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_array_sort_iter(std::string_view      name,
                                    const CallExpression& call,
                                    ir::Index             arr,
                                    ir::Ref               cty,
                                    bool                  queue,
                                    const DeclIndexMap&   map,
                                    ir::Builder&          b,
                                    ir::SourceLoc         l);

    void emit_sort_body(const SortIter& s, bool queue, ir::Builder& b, ir::SourceLoc l);

    static bool is_locator_method(std::string_view name);

    static bool is_reduction_method(std::string_view name);

    static bool is_ordering_method(std::string_view name);

    std::optional<ir::Index> lower_container_method(const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_fixed_array_method(std::string_view      name,
                                       const CallExpression& call,
                                       ir::Builder&          b,
                                       ir::SourceLoc         l);

    ir::Index lower_fixed_array_order(std::string_view      name,
                                      const CallExpression& call,
                                      ir::Index             arr,
                                      ir::Builder&          b,
                                      ir::SourceLoc         l);

    void emit_fixed_mirror_copy(ir::Index           arr,
                                ir::Ref             aty,
                                ir::Index           mirror,
                                ir::Ref             mty,
                                const DeclIndexMap& map,
                                bool                to_mirror,
                                ir::Builder&        b,
                                ir::SourceLoc       l);

    ir::Index canonical_elem_ptr(ir::Index elem, ir::Ref er, ir::Builder& b, ir::SourceLoc l);

    // An active `with` iterator clause (LRM 7.12 / 7.12.4): the element variable
    // slang created for the clause, and the cell holding the index that the
    // clause's `item.index` reads. A stack, so a nested clause resolves to the
    // innermost binding, and so the inner dedup scan of unique() can retarget the
    // index without disturbing the outer one.
    struct IterBinding {
        const slang::ast::ValueSymbol* var = nullptr;
        TempCell                       index;
        // The receiver's declared-index map, so `item.index` reports the declared
        // index (LRM 7.12.4) and not the 0-based iteration position the cell holds.
        DeclIndexMap map;
    };
    std::vector<IterBinding> iter_stack_;

    std::optional<ir::Index> lower_iterator_index(const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_array_iter_method(std::string_view      name,
                                      const CallExpression& call,
                                      ir::Index             arr,
                                      ir::Builder&          b,
                                      ir::SourceLoc         l);

    ir::Index var_elem_select(const ElementSelectExpression& es, ir::Index base, ir::Builder& b,
                              ir::SourceLoc l, bool is_write);

    ir::Index lower_assoc_key(const Expression& key, ir::Ref assoc_ty, ir::Builder& b, ir::SourceLoc l);

    ir::Index var_assoc_select(const ElementSelectExpression& es, ir::Index base, ir::Builder& b,
                               ir::SourceLoc l, bool is_write);

    ir::Index materialize_queue_rvalue(const Expression& expr, ir::Builder& b);

    void append_queue_pieces(ir::Index dst, const Expression& e, ir::Ref qty, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_queue_assign(const AssignmentExpression& a, ir::Builder& b);

    static bool is_plain_handle_place(const Expression& e);

    void emit_queue_store(ir::Index dst, ir::Ref qty, const Expression& rhs_expr, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_assoc_assign(const AssignmentExpression& a, ir::Builder& b);

    bool is_assoc_source_place(const Expression& e);

    void emit_assoc_store(ir::Index dst, ir::Ref aty, const Expression& rhs_expr, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_container_element_value(const Expression& val,
                                            ir::Ref           container_ty,
                                            ir::Builder&      b,
                                            ir::SourceLoc     l);

    static bool mutates_container(std::string_view name);

    ir::Index lower_queue_method(std::string_view name, const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_assoc_method(std::string_view name, const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_compound_rhs(const AssignmentExpression& a, ir::Index lv, ir::Builder& b);

    ir::Index lower_compound_assign(const AssignmentExpression& a, ir::Builder& b);

    void store_concat(const ConcatenationExpression& cc,
                      ir::Index                      rhs,
                      ir::Inst::Tag                  tag,
                      ir::Builder&                   b,
                      ir::SourceLoc                  l,
                      const IntraNbTiming*           nbt = nullptr);

    void store_concat_ops(std::span<const Expression* const> ops,
                          ir::Index                          rhs,
                          ir::Inst::Tag                      tag,
                          ir::Builder&                       b,
                          ir::SourceLoc                      l,
                          const IntraNbTiming*               nbt = nullptr);

    [[noreturn]] void fail_keyed_pattern_lvalue();

    ir::Index lower_pattern_assign(const slang::ast::AssignmentPatternExpressionBase& pat,
                                   const Expression&                                  rhs_expr,
                                   ir::Inst::Tag                                      tag,
                                   ir::Builder&                                       b,
                                   ir::SourceLoc                                      l,
                                   const IntraNbTiming*                               nbt = nullptr);

    void store_pattern(const slang::ast::AssignmentPatternExpressionBase& pat,
                       ir::Index                                          rhs_addr,
                       ir::Ref                                            rhs_ty,
                       ir::Inst::Tag                                      tag,
                       ir::Builder&                                       b,
                       ir::SourceLoc                                      l,
                       const IntraNbTiming*                               nbt = nullptr);

    void store_pattern_elem(const Expression&    member,
                            ir::Index            rhs_elem_addr,
                            ir::Ref              elem_ty,
                            ir::Inst::Tag        tag,
                            ir::Builder&         b,
                            ir::SourceLoc        l,
                            const IntraNbTiming* nbt = nullptr);

    // -- Expressions --

    bool is_agg_bitstream_operand(const Expression& e) const;

    ir::Index serialize_agg_lvalue(ir::Index lv, ir::Ref ty, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_streaming_concat(const StreamingConcatenationExpression& sc,
                                     ir::Ref                                 target_ty,
                                     ir::Builder&                            b,
                                     ir::SourceLoc                           l);

    // -- Streaming concatenation with a dynamically sized operand, or into a
    // string / queue / dynamic-array destination (LRM 11.4.14) --
    //
    // A fixed-size stream is one packed value, so lower_streaming_concat above
    // builds it (block reversal included) and it arrives below as a single Fixed
    // chunk. An operand that is a queue, dynamic array or string has no lower-time
    // width at all -- neither does the stream then -- so those operands stay
    // separate and the destination consumes them in order: bytes for a string,
    // elements for a container. Every chunk contributes a whole number of
    // destination granules; a stream that would split one is refused rather than
    // silently misaligned.
    struct StreamChunk {
        enum class Kind : uint8_t { Fixed, Elems, Str };
        Kind kind = Kind::Fixed;
        // Fixed: the packed value. Elems: the container's handle lvalue. Str: the
        // string value.
        ir::Index value{};
        // Elems: the interned queue / dynamic-array type of `value`.
        ir::Ref ty = ir::Ref::Invalid;
        // Fixed: the value's bit width. Elems: one element's bit width, 0 when the
        // element is itself a string.
        uint32_t width       = 0;
        bool     is_queue    = false;
        bool     elem_is_str = false;
    };

    uint32_t stream_granule_width(const ir::Type& ct);

    void collect_stream_chunks(const StreamingConcatenationExpression& sc,
                               std::vector<StreamChunk>&               out,
                               ir::Builder&                            b,
                               ir::SourceLoc                           l,
                               bool                                    dyn_slice_ok = false);

    void collect_agg_stream_chunks(ir::Index                 lv,
                                   ir::Ref                    ty,
                                   std::vector<StreamChunk>&  out,
                                   ir::Builder&                b,
                                   ir::SourceLoc                l);

    // Loop over the live elements of an Elems chunk, calling `body(elem_lvalue)`
    // inside the loop's body region. The bound is snapshotted once, like every
    // other runtime-length walk here (LRM 12.7.3 leaves a mid-loop resize
    // undefined), and each region re-emits its own leaves (see TempCell).
    template<typename Body>
    void emit_stream_elem_loop(const StreamChunk& c, ir::Builder& b, ir::SourceLoc l, Body body) {
        using T                   = ir::Inst::Tag;
        const ir::Ref   i32       = make_int_type(32, false, true);
        const TempCell  idx       = make_temp_cell(i32, "stream_i");
        const TempCell  bound     = make_temp_cell(i32, "stream_n");
        const ir::Index size      = c.is_queue ? b.queue_size(c.value, c.ty, l) : b.dyn_array_size(c.value, c.ty, l);
        const ir::Index bound_lv  = bound.lv(b, l);
        const ir::Index bound_val = b.resize(size, i32, l);
        b.binary(T::Store, bound_lv, bound_val, l);
        const ir::Index idx_lv   = idx.lv(b, l);
        const ir::Index idx_init = b.constant(module_.pool.put_int(i32, 0), l);
        b.binary(T::Store, idx_lv, idx_init, l);

        b.open_region();
        const ir::Index      cur  = b.unary(T::Load, idx.lv(b, l), l);
        const ir::Index      lim  = b.unary(T::Load, bound.lv(b, l), l);
        const ir::Index      pred = b.binary(T::Lt, cur, lim, l);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        body(b.elem_select(c.value, b.unary(T::Load, idx.lv(b, l), l), c.ty, l));
        const ir::ExtraSlice body_r = b.close_region();

        b.open_region();
        const ir::Index slot = idx.lv(b, l);
        const ir::Index sc   = b.unary(T::Load, idx.lv(b, l), l);
        const ir::Index one  = b.constant(module_.pool.put_int(i32, 1), l);
        const ir::Index next = b.binary(T::Add, sc, one, l);
        b.binary(T::Store, slot, next, l);
        const ir::ExtraSlice step = b.close_region();

        b.loop(cond, pred, body_r, step, /*bottom_test*/ false, l);
    }

    // Loop over the bytes of a string value, calling `body(byte)` with an 8-bit
    // value inside the body region (LRM 11.4.14: a string contributes 8*len bits,
    // byte 0 first).
    template<typename Body>
    void emit_stream_byte_loop(ir::Index str, ir::Builder& b, ir::SourceLoc l, Body body) {
        using T                   = ir::Inst::Tag;
        const ir::Ref   i32       = make_int_type(32, false, true);
        const ir::Ref   byte_ty   = make_int_type(8, false, false);
        const TempCell  idx       = make_temp_cell(i32, "stream_bi");
        const TempCell  bound     = make_temp_cell(i32, "stream_bn");
        const ir::Index bound_lv  = bound.lv(b, l);
        const ir::Index bound_val = b.resize(b.unary(T::StrLen, str, l), i32, l);
        b.binary(T::Store, bound_lv, bound_val, l);
        const ir::Index idx_lv   = idx.lv(b, l);
        const ir::Index idx_init = b.constant(module_.pool.put_int(i32, 0), l);
        b.binary(T::Store, idx_lv, idx_init, l);

        b.open_region();
        const ir::Index      cur  = b.unary(T::Load, idx.lv(b, l), l);
        const ir::Index      lim  = b.unary(T::Load, bound.lv(b, l), l);
        const ir::Index      pred = b.binary(T::Lt, cur, lim, l);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        body(b.resize(b.binary(T::StrGetc, str, b.unary(T::Load, idx.lv(b, l), l), l), byte_ty, l));
        const ir::ExtraSlice body_r = b.close_region();

        b.open_region();
        const ir::Index slot = idx.lv(b, l);
        const ir::Index sc   = b.unary(T::Load, idx.lv(b, l), l);
        const ir::Index one  = b.constant(module_.pool.put_int(i32, 1), l);
        const ir::Index next = b.binary(T::Add, sc, one, l);
        b.binary(T::Store, slot, next, l);
        const ir::ExtraSlice step = b.close_region();

        b.loop(cond, pred, body_r, step, /*bottom_test*/ false, l);
    }

    void check_stream_byte_multiple(uint32_t w);

    ir::Index stream_to_string(std::span<const StreamChunk> chunks, ir::Builder& b, ir::SourceLoc l);

    // Feed the stream to a destination that takes `g`-bit granules, calling
    // `emit(value)` with each one in stream order (most significant first within
    // a chunk).
    template<typename Emit>
    void walk_stream_granules(std::span<const StreamChunk> chunks,
                              uint32_t                     g,
                              ir::Ref                      gty,
                              ir::Builder&                 b,
                              ir::SourceLoc                l,
                              Emit                         emit) {
        using T          = ir::Inst::Tag;
        const auto split = [&](ir::Index v, uint32_t w) {
            if (w % g != 0) {
                // TODO: a stream whose operand does not divide into whole
                // destination elements. Realigning across a granule boundary needs
                // a bit-level buffer, not this shift-and-resize walk.
                fail("streaming a " + std::to_string(w) + "-bit operand into " + std::to_string(g) +
                     "-bit elements is not supported (not a whole number of elements)");
            }
            for (uint32_t k = 0; k < w / g; ++k) {
                const uint32_t lsb  = w - (k + 1) * g;
                ir::Index      part = v;
                if (lsb != 0) {
                    const ir::Index sh =
                      b.constant(module_.pool.put_int(make_int_type(32, false, false), lsb), l);
                    part = b.binary(T::Srl, v, sh, l);
                }
                emit(b.resize(part, gty, l));
            }
        };
        for (const StreamChunk& c : chunks) {
            switch (c.kind) {
                case StreamChunk::Kind::Fixed:
                    split(c.value, c.width);
                    break;
                case StreamChunk::Kind::Str:
                    // A string is 8 bits/char (LRM 11.4.14); split() generalizes
                    // this correctly for any g dividing 8 evenly (1/2/4/8 -- the
                    // g==8 case reduces to exactly the old single emit, since
                    // split's own loop then runs once with no shift) and refuses
                    // g that does not (e.g. 3), with the same message every other
                    // granule mismatch in this function uses.
                    emit_stream_byte_loop(c.value, b, l, [&](ir::Index byte) { split(byte, 8); });
                    break;
                case StreamChunk::Kind::Elems:
                    if (c.elem_is_str) {
                        emit_stream_elem_loop(c, b, l, [&](ir::Index elem) {
                            emit_stream_byte_loop(
                              b.unary(T::Load, elem, l), b, l, [&](ir::Index byte) { split(byte, 8); });
                        });
                    } else {
                        emit_stream_elem_loop(
                          c, b, l, [&](ir::Index elem) { split(b.unary(T::Load, elem, l), c.width); });
                    }
                    break;
            }
        }
    }

    ir::Index stream_granule_count(std::span<const StreamChunk> chunks,
                                   uint32_t                     g,
                                   ir::Builder&                 b,
                                   ir::SourceLoc                l);

    void stream_fill_queue(ir::Index                    dst,
                           std::span<const StreamChunk> chunks,
                           ir::Ref                      qty,
                           ir::Builder&                 b,
                           ir::SourceLoc                l);

    void stream_fill_dynarr(ir::Index                    dst,
                            std::span<const StreamChunk> chunks,
                            ir::Ref                      aty,
                            ir::Builder&                 b,
                            ir::SourceLoc                l);

    ir::Index stream_reorder_pos(ir::Index     p,
                                 ir::Index     w,
                                 ir::Index     nfull,
                                 ir::Index     leftover,
                                 uint32_t      slice,
                                 ir::Builder&  b,
                                 ir::SourceLoc l);

    void check_stream_fits_fixed(ir::Index                               w,
                                 uint32_t                                tw,
                                 const StreamingConcatenationExpression& sc,
                                 ir::Builder&                            b,
                                 ir::SourceLoc                           l);

    ir::Index stream_to_fixed(std::span<const StreamChunk>            chunks,
                              const StreamingConcatenationExpression& sc,
                              ir::Ref                                 target_ty,
                              ir::Builder&                            b,
                              ir::SourceLoc                           l);

    // -- Streaming concatenation as an assignment target: unpack (LRM 11.4.14.3) --
    //
    // slang leaves the RHS self-determined here (AssignmentExpression::fromComponents
    // returns before convertAssignment for a streaming target), so the source keeps
    // its own width: bits are consumed from its LEFT and any low bits the
    // destinations do not cover are discarded, which is legal precisely because a
    // streaming RHS is refused below.
    //
    // What is unconsumed, as the walk hands bits out: `pos` is one past the highest
    // bit not yet taken, counting from the trimmed source's LSB, so a destination of
    // width w takes [pos-1 : pos-w].
    struct StreamUnpack {
        ir::Index src{};
        uint32_t  pos       = 0;
        // Bits reserved for the FIRST dynamically sized destination; LRM 11.4.14.3
        // is greedy, so every later one is left empty.
        uint32_t dyn_bits  = 0;
        bool     dyn_taken = false;
    };

    ir::Index take_stream_bits(StreamUnpack& u, uint32_t w, ir::Ref ty, ir::Builder& b, ir::SourceLoc l);

    std::optional<uint32_t> first_dynamic_dest_granule(const StreamingConcatenationExpression& sc);

    void unpack_stream_dest(const Expression&    dest,
                            StreamUnpack&        u,
                            ir::Inst::Tag        tag,
                            ir::Builder&         b,
                            ir::SourceLoc        l,
                            const IntraNbTiming* nbt = nullptr);

    void unpack_stream_agg(ir::Index            lv,
                           ir::Ref              ty,
                           StreamUnpack&        u,
                           ir::Inst::Tag        tag,
                           ir::Builder&         b,
                           ir::SourceLoc        l,
                           const IntraNbTiming* nbt = nullptr);

    void unpack_stream_dynamic_dest(const Expression& dest,
                                    uint32_t          bits,
                                    StreamUnpack&     u,
                                    ir::Builder&      b,
                                    ir::SourceLoc     l);

    void store_stream_target(const StreamingConcatenationExpression& sc,
                             const Expression&                       rhs_expr,
                             ir::Index                               rhs,
                             ir::Inst::Tag                           tag,
                             ir::Builder&                            b,
                             ir::SourceLoc                           l,
                             const IntraNbTiming*                    nbt = nullptr);

    ir::Index lower_expr(const Expression& expr, ir::Builder& b);

    ir::Index lower_unary(const UnaryExpression& u, ir::Builder& b);

    ir::Index lower_short_circuit(const BinaryExpression& bin, ir::Builder& b);

    ir::Index lower_lazy_conditional(const ConditionalExpression& c, ir::Index pred, ir::Builder& b);

    ir::Index real_zero(const Expression& operand, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_real_binary(const BinaryExpression& bin, ir::Index lhs, ir::Index rhs, ir::Builder& b);

    ir::Index lower_binary(const BinaryExpression& bin, ir::Builder& b);

    ir::Index inside_leaf_eq(ir::Index               sel,
                             ir::Index               val,
                             const slang::ast::Type& vt,
                             ir::Ref                 ty,
                             bool                    real,
                             ir::Builder&            b,
                             ir::SourceLoc           l);

    ir::Index inside_fixed_array_pred(ir::Index               base,
                                      const slang::ast::Type& arr,
                                      ir::Index               sel,
                                      ir::Ref                 ty,
                                      bool                    real,
                                      ir::Builder&            b,
                                      ir::SourceLoc           l);

    ir::Index inside_container_pred(const Expression& m,
                                    ir::Index         sel,
                                    ir::Ref           ty,
                                    bool              real,
                                    ir::Builder&      b,
                                    ir::SourceLoc     l);

    ir::Index lower_inside(const InsideExpression& e, ir::Builder& b);

    ir::Index lower_range_select(const RangeSelectExpression& rs, ir::Index base, ir::Builder& b, ir::SourceLoc l);

    static bool is_indexed_array_slice(const Expression& e);

    static bool is_dynarr_slice(const Expression& e);

    static bool is_positional_array_slice(const Expression& e);

    void guard_indexed_array_slice(const RangeSelectExpression& rs);

    ir::Index elem_coord_const(const ir::Type& t, uint32_t pos, ir::Builder& b, ir::SourceLoc l);

    void emit_indexed_slice_copy(const RangeSelectExpression& rs,
                                 ir::Index                    other,
                                 bool                         into_slice,
                                 ir::Inst::Tag                tag,
                                 ir::Builder&                 b,
                                 ir::SourceLoc                l);

    ir::Index materialize_indexed_slice(const RangeSelectExpression& rs, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_array_operand(const Expression& e, ir::Builder& b);

    ir::Index array_rvalue_lvalue(const Expression& src, ir::Ref ty, ir::Builder& b, ir::SourceLoc l);

    static bool is_array_lvalue_shape(const Expression& e);

    bool array_copy_may_overlap(const Expression& dst, const Expression& src);

    ir::Index array_copy_source(const Expression& dst,
                                const Expression& src,
                                ir::Ref           ty,
                                ir::Builder&      b,
                                ir::SourceLoc     l);

    ir::Index lower_packed_element_lvalue(const ElementSelectExpression& es,
                                          ir::Index                      base,
                                          ir::Builder&                   b,
                                          ir::SourceLoc                  l);

    uint32_t union_tag_bits(const slang::ast::Type& ct);

    ir::Index lower_tagged_union_set(const slang::ast::TaggedUnionExpression& tu, ir::Builder& b);

    void check_union_tag(const MemberAccessExpression& ma,
                         const FieldSymbol&            field,
                         ir::Index                     base,
                         bool                          is_write,
                         ir::Builder&                  b,
                         ir::SourceLoc                 l);

    ir::Index lower_pattern(const slang::ast::Pattern& pat,
                            ir::Index                  subject,
                            const slang::ast::Type&    sty,
                            ir::Inst::Tag              cmp,
                            ir::Builder&               b,
                            ir::SourceLoc              l);

    static ir::Inst::Tag pattern_cmp(CaseStatementCondition cond);

    // AND-reduce the predicates of a cond_predicate's conditions (LRM 12.4 /
    // 12.6): each condition is either a plain boolean expression or an
    // `expr matches pattern`. Pattern variables bound by an earlier condition are
    // visible to later conditions (the `&&&` filter form) and the guarded body,
    // so bindings are emitted left to right into the current region.
    // Templated over the Condition type: ConditionalStatement::Condition and
    // ConditionalExpression::Condition are distinct nested structs with the same
    // { expr, pattern } shape (the `if` and `?:` forms), so one body serves both.
    template<typename Cond>
    ir::Index lower_conditions(std::span<const Cond> conds, ir::Builder& b, ir::SourceLoc l) {
        ir::Index acc  = ir::Index{};
        bool      have = false;
        for (const auto& cond : conds) {
            ir::Index p;
            if (cond.pattern != nullptr) {
                p = lower_pattern(*cond.pattern,
                                  lower_expr(*cond.expr, b),
                                  cond.expr->type->getCanonicalType(),
                                  ir::Inst::Tag::CaseEq,
                                  b,
                                  l);
            } else {
                p = lower_predicate(*cond.expr, b);
            }
            acc  = have ? b.binary(ir::Inst::Tag::Land, acc, p, l) : p;
            have = true;
        }
        return acc;
    }

    void lower_pattern_case(const slang::ast::PatternCaseStatement& c, ir::Builder& b);

    ir::Index lower_pattern_item_pred(const slang::ast::PatternCaseStatement&            c,
                                      const slang::ast::PatternCaseStatement::ItemGroup& item,
                                      ir::Index                                          subject,
                                      ir::Builder&                                       b,
                                      ir::SourceLoc                                      l);

    void lower_pattern_case_item(const slang::ast::PatternCaseStatement& c,
                                 size_t                                  k,
                                 ir::Index                               subject,
                                 ir::Builder&                            b);

    void lower_pattern_case_checked(const slang::ast::PatternCaseStatement& c, ir::Index subject, ir::Builder& b);

    void dispatch_pattern_preds(const slang::ast::PatternCaseStatement& c,
                                size_t                                  k,
                                const std::vector<ir::Index>&           preds,
                                bool                                    no_match_report,
                                const std::string&                      banner,
                                ir::SourceLoc                           rloc,
                                ir::Builder&                            b);

    ir::Index lower_member_lvalue(const MemberAccessExpression& ma, ir::Builder& b, ir::SourceLoc l,
                                  bool is_write = false);

    ir::Index lower_lvalue(const Expression& expr, ir::Builder& b, bool is_write = false);

    uint64_t const_uint(const Expression& e, std::string_view what);

    // The one place Clotho asks slang to evaluate an expression. A bad value when
    // `e` is not constant, or when `mode` forbids folding it. The overload without
    // a context evaluates against the current scope; a caller folding many
    // expressions in one scope passes its own.
    slang::ConstantValue fold_const(const Expression& e, Fold mode);

    slang::ConstantValue fold_const(const Expression& e, Fold mode, const slang::ast::ASTContext& astCtx);

    std::optional<int32_t> try_const_int(const Expression& e, Fold mode = Fold::Runtime);

    // -- Packed-select index normalization --
    //
    // The IR models every packed type as [width-1:0] (make_int_type), so a
    // select index written against the declared range ([0:7], [15:8],
    // [-1:-8], ...) must be translated to a zero-based LSB offset before
    // BitSelect / PartSelect / DynPartSelect (LRM 11.5): a little-endian
    // range [l:r] maps index i to i - r, a big-endian range to r - i.

    static slang::ConstantRange packed_range_of(const Expression& value);

    ir::Index lower_packed_offset(const Expression&    idx,
                                  slang::ConstantRange range,
                                  int32_t              bias,
                                  ir::Builder&         b,
                                  ir::SourceLoc        l,
                                  uint32_t             stride = 1);

    // The same offset for an index whose arithmetic a 32-bit offset could wrap.
    ir::Index lower_wide_packed_offset(const Expression& idx,
                                       bool              descending,
                                       int64_t           adj,
                                       ir::Builder&      b,
                                       uint32_t          stride);

    ir::Index packed_bit_index(const ElementSelectExpression& es, ir::Builder& b, ir::SourceLoc l);

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
        // The return slot's Local index / type (valid iff has_ret_local): the
        // frame cell a `return` hands the caller its value through. For an
        // automatic function it is also the implicit return variable (LRM 13.4.1).
        // For a static one the implicit variable is ret_static instead and this
        // cell is only scratch, made on first use (ret_var_ref).
        uint32_t ret_local       = 0;
        ir::Ref  ret_type        = ir::Ref::Invalid;
        bool     has_ret_local   = false;
        bool     ret_local_ready = false;
        // The implicit return variable of a STATIC function: persistent
        // module-signal storage shared by every call (LRM 13.4.2). Unset for an
        // automatic function.
        std::optional<ir::Signal> ret_static;
        // Name qualifier (e.g. "f.") prepended to a static local's signal name so
        // same-named statics in different functions stay distinct.
        std::string name_prefix;
        // %m of a synthetic body that is not a scope of its own -- a fork child, a
        // $strobe or $monitor print, an assertion action block, a randsequence
        // production -- is that of the statement that created it (LRM 21.2.1.5:
        // the scope that invokes the task). Unset for a subroutine, whose own
        // name is part of its %m.
        std::optional<std::string> invoking_scope;
        // True while lowering a task body (a coroutine): unlike a function, timing
        // controls and $time are allowed.
        bool is_task = false;
        // The subroutine this body belongs to, or null for a synthetic body (a fork
        // child, an action block, a print thunk). Read by lower_disable to spot LRM
        // 9.6.2 example 4: a task naming ITSELF as the disable target.
        const SubroutineSymbol* sym = nullptr;
        // Class method context (valid iff has_this): `this` is spliced as frame
        // Local 0, `this_class` is its Kind::Class handle type. A bare property
        // reference inside the body roots at this handle (value_lvalue -> this_handle).
        bool     has_this   = false;
        uint32_t this_local = 0;
        ir::Ref  this_class = ir::Ref::Invalid;
        // slang's own symbol for `this` (SubroutineSymbol::thisVar). It is an ordinary
        // automatic of this frame, which is what lets a fork child capture it; every
        // reader of the handle goes through this_handle so a BOXED `this` is read out
        // of its box rather than out of Local 0.
        const ValueSymbol* this_var = nullptr;
        // Captured automatics that live in a GC box rather than in the frame, so a
        // detached fork child can share them after this activation is gone (LRM 9.3.2
        // makes an enclosing automatic ONE variable; docs/design/fork-join.md V4).
        // Consulted by value_lvalue BEFORE localmap: a boxed symbol keeps its localmap
        // entry so ForkCaptureDetector still sees it as this frame's, but every access
        // goes through the box.
        struct Boxed {
            uint32_t local; // frame Local holding the box handle
            ir::Ref  cls;   // the box's one-field synthetic class
        };
        ankerl::unordered_dense::map<const ValueSymbol*, Boxed> boxed;
        // Set while lowering a randsequence production body (LRM 18.17). The two
        // synthesized `output` formals: `$abort`, which a `break` with no enclosing
        // loop raises and every caller tests (18.17.6), and `$result`, which a
        // non-void production's `return expr` writes (18.17.7). See
        // docs/design/randsequence.md §6.
        bool     in_randseq         = false;
        uint32_t randseq_abort      = 0;
        bool     randseq_has_result = false;
        uint32_t randseq_result     = 0;
        ir::Ref  randseq_result_ty  = ir::Ref::Invalid;
        // Inside a `rand join` operand's `step` body only (LRM 18.17.5): the output
        // formal that reports "this production returned", so the driver stops
        // advancing it. `return` in a plain production body just ends the function;
        // in a step body it also has to end the production's remaining units.
        bool     randseq_has_stop = false;
        uint32_t randseq_stop     = 0;
    };

    // Inner storage type -> its capture-box class (box_class_type). Keyed
    // separately for the notified variant: a box whose symbol some `wait` reads
    // carries a hidden `$chg.$capture` event field (its change notifier), and the
    // two layouts must not share a Ref. The field names are not legal SV
    // identifiers, so they cannot collide with a property.
    static constexpr std::string_view                          kCaptureBoxField = "$capture";
    static constexpr std::string_view                          kCaptureBoxChg   = "$chg.$capture";
    ankerl::unordered_dense::map<ir::Ref, ir::Ref>             box_class_;
    ankerl::unordered_dense::map<ir::Ref, ir::Ref>             box_class_notified_;
    ankerl::unordered_dense::set<uint32_t>                     notified_box_refs_;

    // The per-entry box of a fork-scope declaration in a PROCESS body, where there is
    // no frame to hold the handle (an initial/always block's own variables are design
    // state, not frame locals -- docs/design/garbage-collection.md 5.4). Keyed by the
    // declared symbol, which belongs to exactly one fork block, so entries never
    // collide; in a subroutine the home is a frame slot and FnCtx::boxed carries it
    // instead. Consulted by value_lvalue after the frame path, which is why a fork
    // CHILD -- which has a frame of its own and receives the handle as a formal --
    // resolves through its own `boxed` entry rather than through this map.
    struct ProcForkBox {
        ir::Signal sig; // design-state signal holding the box handle
        ir::Ref    cls; // the box's one-field synthetic class
    };
    ankerl::unordered_dense::map<const ValueSymbol*, ProcForkBox> proc_fork_boxes_;

    ir::Ref box_class_type(ir::Ref inner);

    ir::Ref box_class_type_notified(ir::Ref inner, bool container);

    ir::Ref box_class_impl(ir::Ref inner, bool notified, bool container);

    ir::Ref box_class_for(const ValueSymbol& vs, ir::Ref inner);

    bool box_is_notified(ir::Ref cls) const;

    ir::Index box_field_lvalue(ir::Index handle, ir::Ref cls, ir::Builder& b, ir::SourceLoc l);

    ir::Index boxed_lvalue(const FnCtx::Boxed& bx, ir::Builder& b, ir::SourceLoc l);

    bool is_boxed_symbol(const ValueSymbol& vs) const;

    ir::Index boxed_notifier_handle(const ValueSymbol& vs, ir::Builder& b, ir::SourceLoc l);

    ir::Index this_handle(ir::Builder& b, ir::SourceLoc l);

    void install_capture_boxes(const SubroutineSymbol& sub, const Statement& body, ir::Builder& b);

    void box_one_capture(const ValueSymbol& vs, bool copy_in, ir::Builder& b, ir::SourceLoc l);

    void install_fork_decl_box(const VariableSymbol& vs, ir::Builder& b, ir::SourceLoc l);

    ir::Index capture_handle(const ValueSymbol& vs, ir::Builder& b, ir::SourceLoc l);

    std::vector<const ValueSymbol*> body_automatics(const SubroutineSymbol& sub, const Statement& body);

    // An object handle paired with the class type that selects its layout, for
    // the two scoped implicit-`this` overrides (ctor_this_ / default_arg_this_,
    // declared with the rest of the state at the bottom of the class). Defined
    // here, ahead of the member-function signatures that name it.
    struct CtorThis {
        ir::Index handle;
        ir::Ref   cls;
    };

    // The queue handle / type `$` resolves against while lowering the bounds of
    // a queue range-select (`q[a:$]`, `q[$:a]`, `q[$-1:$]` -- LRM 7.10.4/11.5.1).
    // `$` there stands for the queue's last valid index (size - 1), a RUNTIME
    // value, unlike a fixed/packed array's `$` (its declared high bound, a
    // constant folded earlier by slang). Nullopt outside such a slice's bounds;
    // scoped by Restore, never assigned bare.
    struct QueueDollarCtx {
        ir::Index src;
        ir::Ref   qty;
    };

    // Restore a saved piece of Lowerer state when the scope exits — on the throwing
    // path too. A lowering failure is an exception that `recover` catches per member
    // and then keeps going, so a restore written after a call that can fail is
    // skipped exactly when it matters: `fn_ctx_` would keep pointing at build_task's
    // dead stack frame, and the next member's value_lvalue would search its localmap
    // and segfault (ivtest pr224a). Anything saved across a fail-capable call belongs
    // in one of these.
    template <typename T>
    class Restore {
    public:
        // Save `slot` and install `next` for the scope's lifetime.
        Restore(T& slot, T next) : slot_(slot), saved_(std::move(slot)) { slot_ = std::move(next); }
        // Save `slot` for callees that overwrite it in place (the time scale).
        explicit Restore(T& slot) : slot_(slot), saved_(slot) {}
        ~Restore() { slot_ = std::move(saved_); }
        Restore(const Restore&)            = delete;
        Restore& operator=(const Restore&) = delete;

    private:
        T& slot_;
        T  saved_;
    };

    // One entry of the named-block stack `disable` resolves against (declared in
    // full further down, next to the statement lowering that pushes it).
    struct OpenBlock;

    // The four pieces of state every subroutine / action / fork-child body swaps:
    // its own frame context, a fresh named-block stack (a `disable` inside the body
    // may only name blocks the body itself opened), no procedure root — a body is
    // built at its first call site, whose enclosing procedure is not its own, so
    // the 16.4.4 outermost-scope question has no answer here — and a loop nesting
    // depth of zero, since a `break` in this body cannot bind to a loop outside it.
    class FnScope {
    public:
        FnScope(Lowerer& l, FnCtx* ctx)
          : ctx_(l.fn_ctx_, ctx)
          , blocks_(l.named_blocks_, {})
          , root_(l.proc_root_block_, nullptr)
          , depth_(l.loop_depth_, 0) {}

    private:
        Restore<FnCtx*>                 ctx_;
        Restore<std::vector<OpenBlock>> blocks_;
        Restore<const Symbol*>          root_;
        Restore<uint32_t>               depth_;
    };

    uint32_t fn_local(const ValueSymbol& vs);

    ir::Signal declare_static_local(const ValueSymbol& vs);

    ir::Signal declare_static_return(const SubroutineSymbol& sub);

    ir::Signal declare_static_prop(const slang::ast::ClassPropertySymbol& prop);

    bool is_static_local(const VariableSymbol& vs, bool force_frame) const;

    void declare_storage(const VariableSymbol& vs, bool force_frame);

    ir::Index value_lvalue(const ValueSymbol& vs, ir::Builder& b, ir::SourceLoc l);

    ir::Index agg_return_operand(const Expression& src, ir::Builder& b, ir::SourceLoc l);

    void lower_break(const Statement& stmt, ir::Builder& b);

    void lower_return(const ReturnStatement& r, ir::Builder& b);

    static bool is_global_subroutine(const SubroutineSymbol& sub);

    static bool is_global_scope(const Scope* scope);

    static std::optional<ir::DpiCType> dpi_ctype(const slang::ast::Type& type);

    void build_dpi_import(const SubroutineSymbol& sub, ir::FunctionId id);

    void build_dpi_open_array_arg(const slang::ast::DPIOpenArrayType& oa,
                                  ir::ArgDir                          dir,
                                  std::vector<ir::Ref>&               param_types,
                                  std::vector<ir::ArgDir>&            arg_dirs,
                                  std::vector<ir::DpiCType>&          arg_cts,
                                  std::vector<ir::DpiCType>&          arg_elem_cts);

    static const InstanceSymbol* owning_top(const Scope* scope);

    static const InstanceSymbol* owning_top(const SubroutineSymbol& sub);

    // Collect `export "DPI-C"` directives (LRM 35.5 / 35.8): for each, force-lower
    // the SV subroutine and record a DpiExport so the backend emits its C-callable
    // thunk. slang resolves the export -> subroutine binding and the c_identifier
    // (Compilation::getDPIExports). Scope: functions AND tasks; input/output/inout
    // scalar / chandle / real / packed-vector args; input string args;
    // scalar/chandle/real/void return. LRM 35.8 puts a task under exactly 35.7's
    // argument rules, so every guard below applies to one unchanged. Deferred
    // (loud-fail): string return (returned-char* lifetime), output/inout string,
    // open-array/unpacked args. Public: called by lower_compilation.
    //
    // getDPIExports is compilation-wide, but every top has its own Lowerer that only
    // knows its own symbols, and instance binding is baked into the lowered body (the
    // backend addresses each signal absolutely off that instance's own state global).
    // So an export must be collected by the Lowerer of the top containing it: `top`
    // admits this top's module-scope exports, and `collect_globals` — true on exactly
    // one call — admits the $unit/package exports, which have no instance multiplicity
    // (LRM 6.8) and lower once through the shared globals Lowerer function_id
    // delegates to. `top` is nullable: a design with no elaborated top module at all
    // (e.g. an interface-only file bound into a package, LRM 23.4 -- a real shape,
    // not a degenerate one: uvm-examples/apb_protocol_monitor pairs uvm_pkg.sv with a
    // single top-less interface) still has $unit/package exports to collect, and the
    // caller passes null with collect_globals=true exactly once for it, since there is
    // no "first top" to piggyback the one-time collection on. A null top can never
    // equal a module-scope export's real owning instance, so the module-scope branch
    // below just skips every such export via the same `continue` an ordinary
    // wrong-top mismatch already takes -- correctly, since a design with no top module
    // has no module-scope subroutine any export could belong to either.
public:
    void collect_dpi_exports(const slang::ast::Compilation& comp, const InstanceSymbol* top,
                             bool collect_globals);

private:
    void apply_time_scale(const Scope& scope);

    ir::FunctionId vif_function_id(const SubroutineSymbol& ph_sub);

    ir::FunctionId function_id(const SubroutineSymbol& sub);

    void build_reserved_body(const SubroutineSymbol& sub, ir::FunctionId id);

    bool has_pending_bodies() const;

    void drain_pending_bodies();

    static bool is_vif_call(const CallExpression& call);

    static ir::ArgDir to_arg_dir(slang::ast::ArgumentDirection dir);

    std::optional<ir::Signal> splice_iface_ctx(std::vector<ir::Ref>&    param_types,
                                               std::vector<ir::ArgDir>& arg_dirs);

    static bool is_iface_body_subroutine(const SubroutineSymbol& sub);

    bool vif_ctx_arg_wanted(const SubroutineSymbol& sub) const;

    std::optional<ir::Index> vif_ctx_arg(const SubroutineSymbol& sub, ir::Builder& b, ir::SourceLoc l);

    void splice_this(const SubroutineSymbol&  sub,
                     std::vector<ir::Ref>&    param_types,
                     std::vector<ir::ArgDir>& arg_dirs);

    ir::Index class_member_lvalue(ir::Index                 handle_val,
                                  const slang::ast::Symbol& prop,
                                  ir::Ref                   class_ref,
                                  ir::Builder&              b,
                                  ir::SourceLoc             l,
                                  ir::SourceSpan            handle_span = {});

    bool stub_eligible(const SubroutineSymbol& sub) const;

    void emit_unsupported_body_stub(const SubroutineSymbol& sub,
                                    const LowerError&       e,
                                    ir::Builder&            b,
                                    ir::Builder::Mark       body_mark);

    void build_task(const SubroutineSymbol& sub, ir::FunctionId id);

    ir::FunctionId build_fork_child(const Statement&                    body,
                                    std::span<const ValueSymbol* const> captures,
                                    ir::ForkKind                        kind,
                                    const std::vector<OpenBlock>&       enclosing);

    ir::FunctionId build_deferred_body(std::span<const Expression* const> args,
                                       char                               default_conv,
                                       ir::SourceLoc                      l,
                                       bool                               fd_arg,
                                       bool                               monitor);

    // The frame cell a `return expr` stages an aggregate in and hands the caller.
    ir::Index ret_var_ref(FnCtx& ctx, ir::SourceLoc l, ir::Builder& b);

    // The function's implicit return variable (LRM 13.4.1): what a bare `return;`
    // and the end of the body hand the caller.
    ir::Index implicit_return_ref(FnCtx& ctx, ir::SourceLoc l, ir::Builder& b);

    void build_function(const SubroutineSymbol& sub, ir::FunctionId id);

    ir::Index lower_new_class(const slang::ast::NewClassExpression& nc, ir::Builder& b);

    void emit_object_rng_seed(const slang::ast::ClassType& clt,
                              ir::Index                    handle,
                              ir::Ref                      class_ref,
                              ir::Builder&                 b,
                              ir::SourceLoc                l);

    ir::Index lower_copy_class(const slang::ast::CopyClassExpression& cc, ir::Builder& b);

    void emit_class_body_copy(ir::Index dst, ir::Index src, ir::Ref class_ref, ir::Builder& b, ir::SourceLoc l);

    ir::FunctionId copy_thunk_id(const slang::ast::ClassType& clt, ir::Ref class_ref);

    void build_copy_thunk(const slang::ast::ClassType& clt, ir::Ref class_ref, ir::FunctionId id);

    // -- Constrained randomization, step 1 (docs/design/constrained-randomization.md) --

    ir::FunctionId rand_body_id(const slang::ast::ClassType&                                              clt,
                                ir::Ref                                                                   class_ref,
                                ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::FunctionId>& map);

    void drain_rand_bodies();

    const SubroutineSymbol* find_rand_hook(const slang::ast::ClassType& clt, std::string_view name);

    void emit_rand_hook(const slang::ast::ClassType& clt,
                        std::string_view             name,
                        ir::Index                    self,
                        ir::Builder&                 b,
                        ir::SourceLoc                l);

    void build_randomize_body(const slang::ast::ClassType& clt, ir::Ref class_ref, ir::FunctionId id);

    // A source of one 32-bit draw. The object's own RNG inside a generated
    // `$randomize` (LRM 18.14.2), the calling thread's for `std::randomize`.
    using RandDraw = std::function<ir::Index()>;

    void emit_rand_assign(const RandProp&  p,
                          const ClassRand& rd,
                          ir::Index        body,
                          ir::Ref          class_ref,
                          ir::Builder&     b,
                          ir::SourceLoc    l);

    void emit_rand_value(ir::Index               lv,
                         const slang::ast::Type& ty,
                         const RandDraw&         draw,
                         ir::Builder&            b,
                         ir::SourceLoc           l);

    void emit_rand_recurse(ir::Index lv, const slang::ast::ClassType& pointee, ir::Builder& b, ir::SourceLoc l);

    ir::Index emit_rand_scalar(const slang::ast::Type& ty,
                               ir::Ref                 target,
                               const RandDraw&         draw,
                               ir::Builder&            b,
                               ir::SourceLoc           l);

    ir::Index emit_rand_enum(const slang::ast::EnumType& et,
                             ir::Ref                     target,
                             ir::Index                   draw,
                             ir::Builder&                b,
                             ir::SourceLoc               l);

    void build_randops_body(const slang::ast::ClassType& clt, ir::Ref class_ref, ir::FunctionId id);

    void emit_ctor_call(const slang::ast::ClassType&       clt,
                        const SubroutineSymbol*            ctor,
                        std::span<const Expression* const> actuals,
                        ir::Index                          handle,
                        ir::Builder&                       b,
                        ir::SourceLoc                      l);

    void emit_base_construction(const slang::ast::ClassType& clt, ir::Index handle, ir::Builder& b, ir::SourceLoc l);

    void emit_class_property_inits(const slang::ast::ClassType& clt,
                                   ir::Index                    handle,
                                   ir::Ref                      class_ref,
                                   ir::Builder&                 b);

    std::optional<ir::Index> lower_process_method(const CallExpression&    call,
                                                 const SubroutineSymbol&  sub,
                                                 ir::Builder&             b);

    std::optional<ir::Index> lower_sync_method(const CallExpression&   call,
                                               const SubroutineSymbol& sub,
                                               ir::Builder&            b);

    std::optional<ir::Index> lower_mailbox_method(const CallExpression&   call,
                                                  const SubroutineSymbol& sub,
                                                  ir::Builder&            b);

    void emit_mbx_message_arrived(ir::Index get_head, ir::Index get_tail, ir::Index peek_head,
                                  ir::Index peek_tail, ir::Builder& b, ir::SourceLoc l);

    // The park / retest / leave loop every blocking synchronization method shares:
    // take the fast path when `fast_ok` (nobody queued and the resource available),
    // otherwise park on the queue and re-test on each wake until this thread is the
    // head and `ready()` holds, then leave the queue -- handing off to the new head,
    // since one `put` can satisfy several waiters. The node stays linked across an
    // unsatisfied retest, which is what preserves arrival order.
    template <typename Ready>
    void emit_sync_park_loop(uint32_t                      site,
                             std::span<const ir::Index, 2> qwords_,
                             ir::Index                     queue_empty,
                             Ready&&                       ready,
                             bool                          require_head,
                             ir::Builder&                  b,
                             ir::SourceLoc                 l) {
        const std::array<ir::Index, 2> qw{ qwords_[0], qwords_[1] };
        const ir::Index fast = b.binary(ir::Inst::Tag::Land, queue_empty, ready(), l);
        b.open_region();
        {
            b.open_region();
            const ir::Index      pred  = true_const(b, l);
            const ir::ExtraSlice lcond = b.close_region();
            b.open_region();
            {
                const ir::Index head = b.sync_wait(site, qw, l);
                // A consuming waiter proceeds only as its queue's head, which is
                // what preserves LRM 15.4.5 / 15.3.3 arrival order. A `peek` waiter
                // consumes nothing, so 15.4.7 unblocks it whenever a message is
                // there, whatever its position.
                const ir::Index mine =
                  require_head ? b.binary(ir::Inst::Tag::Land, head, ready(), l) : ready();
                b.open_region();
                b.break_(l);
                const ir::ExtraSlice then_r = b.close_region();
                b.open_region();
                const ir::ExtraSlice else_r = b.close_region();
                b.cond(mine, then_r, else_r, l);
            }
            const ir::ExtraSlice body = b.close_region();
            b.open_region();
            const ir::ExtraSlice step = b.close_region();
            b.loop(lcond, pred, body, step, /*bottom_test=*/false, l);
            b.sync_ctl(ir::SyncOp::Leave, site, qw, l);
        }
        const ir::ExtraSlice slow = b.close_region();
        b.open_region();
        const ir::ExtraSlice fast_r = b.close_region();
        b.cond(fast, fast_r, slow, l);
    }

    std::optional<ir::Index> lower_user_call(const CallExpression& call, ir::Builder& b);

    void lower_task_call(const CallExpression&    call,
                         const SubroutineSymbol&  sub,
                         ir::Builder&             b,
                         std::optional<ir::Index> this_val = std::nullopt);

    ir::Ref virtual_recv_class(const CallExpression& call);

    uint32_t iface_dispatch_index(const slang::ast::SubroutineSymbol& sub);

    std::optional<ir::Index> resolve_method_this(const CallExpression&   call,
                                                 const SubroutineSymbol& sub,
                                                 ir::Builder&            b);

    std::optional<CtorThis> default_arg_recv(const SubroutineSymbol&  sub,
                                             std::optional<ir::Index> this_val);

    std::pair<ir::Index, const slang::ast::ClassType*> implicit_this(ir::Builder& b, ir::SourceLoc l);

    const slang::ast::ClassType* class_of_ref(ir::Ref ref);

    ir::Index lower_randomize_call(const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_scope_randomize(const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    ir::Index lower_rand_mode_call(const CallExpression& call, ir::Builder& b, ir::SourceLoc l);

    uint32_t rand_slot_of(const slang::ast::ClassType& clt, std::string_view slot);

    ir::FunctionId rand_slot_sig(const slang::ast::ClassType&                                                     clt,
                                 const ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::FunctionId>& map);

    ir::FunctionId iface_rand_sig(ir::Ref iface_ref, bool ops);

    ir::Index emit_randops_dispatch(const slang::ast::ClassType& clt,
                                    ir::Index                    self,
                                    ir::Index                    op,
                                    ir::Index                    arg,
                                    ir::Builder&                 b,
                                    ir::SourceLoc                l);

    ir::Index emit_randomize_dispatch(const slang::ast::ClassType& clt,
                                      ir::Index                    self,
                                      uint64_t                     mask,
                                      ir::Builder&                 b,
                                      ir::SourceLoc                l);

    std::optional<ir::Index> lower_builtin_class_method(const CallExpression&   call,
                                                        const SubroutineSymbol& sub,
                                                        ir::Builder&            b);
    // -- Functional coverage (LRM 19, docs/design/functional-coverage.md) -------
    //
    // A covergroup type lowers to a 64-bit 2-state handle, not a class: an
    // instance is runtime-owned storage that outlives the design, because LRM
    // 19.11.3 aggregates type coverage over instances the design has dropped, so
    // it must not be a collected object. Everything the frontend can decide -- the
    // coverpoints, the bin value sets, the resolved options -- becomes static model
    // data (ir::CoverType) the backend emits beside the design; everything that
    // depends on run-time values is a call into src/runtime/Coverage.h.
    //
    // The model is built lazily at the first `new` of a covergroup type rather than
    // from the declaration, because that is where the lowering context is right:
    // coverpoint expressions name signals of the enclosing instance or properties
    // of the enclosing object, and `new` for an embedded covergroup is required to
    // sit in the enclosing class's constructor (LRM 19.4).

    // What one lowered covergroup declaration owns.
    struct CoverDecl {
        // Index into module_.cover_types.
        uint32_t type_idx = 0;
        // The generated sample body: `void(u64 handle)`, which evaluates every
        // coverpoint expression and hands each value to the coverage runtime.
        ir::FunctionId sample{};
    };

    // A covergroup's own instance / type options, as far as this phase resolves
    // them. LRM 19.7 Table 19-1 for the instance set, 19.7.1 Table 19-3 for the
    // type set; the two share `weight` and `goal`, and a type option is not an
    // independent knob here because merge_instances is refused (19.11.3).
    struct CoverOpts {
        uint32_t weight       = 1;
        uint32_t goal         = 100;
        uint32_t at_least     = 1;
        uint32_t auto_bin_max = 64; // LRM Table 19-1's default
        bool     per_instance = false;
    };

    // The value space a coverpoint's bins live in: what `$` means, and how many
    // automatic bins 19.5.3 would create.
    struct CoverSpace {
        uint32_t width     = 0;
        bool     is_signed = false;
        bool     four      = false;
        uint64_t min       = 0;
        uint64_t max       = 0;
        // 2^width, saturated at UINT64_MAX for a 64-bit coverpoint.
        uint64_t cardinality = 0;
    };

    int64_t cover_const(const Expression& e, const char* what);

    CoverSpace cover_space_of(const slang::ast::Type& t);

    static uint64_t cover_trunc(int64_t v, const CoverSpace& s);

    static bool cover_le(uint64_t a, uint64_t b, const CoverSpace& s);

    static std::string cover_val_str(uint64_t v, const CoverSpace& s);

    std::vector<ir::CoverRange> cover_ranges_of(const slang::ast::CoverageBinSymbol& bin, const CoverSpace& space);

    static uint64_t cover_range_count(const std::vector<ir::CoverRange>& rs, const CoverSpace& s);

    static std::vector<uint64_t> cover_values_of(const std::vector<ir::CoverRange>& rs, const CoverSpace& s);

    void cover_expand_bin(const slang::ast::CoverageBinSymbol& bin,
                          const CoverSpace&                    space,
                          const CoverOpts&                     opts,
                          ir::CoverPoint&                      cp);

    void cover_auto_bins(const slang::ast::Type& type,
                         const CoverSpace&       space,
                         const CoverOpts&        opts,
                         ir::CoverPoint&         cp);

    void cover_apply_option(const slang::ast::CoverageOptionSetter& set, CoverOpts& opts);

    uint32_t cover_type_of(const slang::ast::CovergroupType& cgt);

    ir::CoverPoint cover_point_model(const slang::ast::CoverpointSymbol& cp, const CoverOpts& gopts);

    static const slang::ast::Type* cover_enum_type(const Expression& e);

    std::string cover_group_name(const slang::ast::CovergroupType& cgt);

    ir::FunctionId build_cover_sample(const slang::ast::CovergroupType&                       cgt,
                                      uint32_t                                                type_idx,
                                      const std::vector<const slang::ast::CoverpointSymbol*>& points);

    void cover_event_process(const slang::ast::CovergroupType& cgt, uint32_t type_idx, ir::FunctionId sample);

    ir::Index lower_new_covergroup(const slang::ast::NewCovergroupExpression& nc, ir::Builder& b);

    static bool is_cover_method(const slang::ast::SubroutineSymbol& sub);

    ir::Index lower_cover_method(const slang::ast::CallExpression&   call,
                                 const slang::ast::SubroutineSymbol& sub,
                                 ir::Builder&                        b);

    static const Expression* cover_receiver(const slang::ast::CallExpression& call);

    // Cap on the bins one coverpoint may hold. The LRM sets no limit; enumerating
    // `bins b[] = {[0:$]}` on a 32-bit coverpoint would otherwise try to name four
    // billion of them.
    static constexpr uint64_t kCoverMaxBins = 1u << 16;

    // One output / inout argument's copy-back record: the caller temp holding the
    // formal's value, the actual lvalue to write at completion, and the types to
    // coerce across (formal -> actual).
    struct CopyOutArg {
        ir::Index               temp;
        const Expression*       actual;
        ir::Ref                 actual_ty;
        const slang::ast::Type* formal_ty;
        // A whole-array output / inout formal passed via a canonical temp: the
        // copy-back is a layout-converting element-wise emit_agg_copy (temp ->
        // actual), not a scalar plane store. `agg_ty` is the temp's aggregate type.
        bool    is_agg = false;
        ir::Ref agg_ty = ir::Ref::Invalid;
    };

    std::vector<ir::Index> build_call_args(std::span<const FormalArgumentSymbol* const> formals,
                                           std::span<const Expression* const>           actuals,
                                           std::vector<CopyOutArg>&                     outs,
                                           ir::Builder&                                 b,
                                           ir::SourceLoc                                l,
                                           const std::optional<CtorThis>& recv = std::nullopt);

    ir::Index agg_input_actual(const Expression& src, ir::Ref pty, ir::Builder& b, ir::SourceLoc l);

    void emit_call_copyouts(const std::vector<CopyOutArg>& outs, ir::Builder& b, ir::SourceLoc l);

    bool is_whole_ref_actual(const Expression& a);

    ir::Index lower_ref_actual(const Expression& actual, ir::Builder& b);

    ir::Index make_arg_temp(ir::Ref type, ir::Builder& b, ir::SourceLoc l);

    ir::Index make_canonical_agg_temp(ir::Ref type, ir::Builder& b, ir::SourceLoc l);

    ir::Module& module_;
    // Scope tree of the top instance this Lowerer walks, handed to the module in
    // lower_top. scope_stack_ tracks the path from the top to the instance
    // currently being lowered; register_signal records into its back().
    ir::InstanceScope               top_scope_;
    std::vector<ir::InstanceScope*> scope_stack_;

    // register_signal's own inverse: Signal -> InstanceScope::id of the scope
    // that declared it. Absent for a package / $unit-scope signal (registered
    // by the standalone globals Lowerer, whose scope_stack_ is always empty --
    // see register_signal). sampled_shadow_of uses this to attach a shadow
    // beside the live signal it shadows, wherever that actually is, rather
    // than wherever the asserting scope happens to be.
    ankerl::unordered_dense::map<uint32_t, uint32_t> signal_scope_;

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
    // inout / ref port collapse (LRM 23.3.3): a child port's internal net/var
    // symbol aliased to the parent connection's signal (shared storage, no copy
    // process). Populated at Collect before the child's signals register, so
    // collect_scope_signals maps the internal symbol to the parent signal instead
    // of allocating fresh storage. Both sides then read/write one signal and
    // multi-driver resolution folds competing drivers.
    ankerl::unordered_dense::map<const Symbol*, ir::Signal> inout_alias_;
    // The variable output ports (LRM 23.2.2.2) whose declaration carries an
    // initializer, by the variable each declares. slang keeps that initializer on
    // the PortSymbol and not on the variable, so declaration_initializer finds it
    // here. Filled by collect_signals for every instance body.
    ankerl::unordered_dense::map<const Symbol*, const PortSymbol*> port_initializers_;
    // Continuous-driver coverage per net signal: the closed regions already
    // driven, checked for overlap by record_net_driver_region (NetRegion defined
    // in the net-driver-tracking section).
    ankerl::unordered_dense::map<uint32_t, std::vector<NetRegion>> net_driven_;
    // Which lowering pass is running; the driver-producing members lower in both
    // Classify (record driver regions) and Emit (store / route), and branch on
    // this. Meaningful only inside lower_members and the driver functions.
    Pass cur_pass_ = Pass::Emit;
    // -- Assertion control (LRM 20.12), all filled by the Collect walk --
    //
    // Every concurrent assertion in this top, in the order Collect met them.
    // Populated whether or not the design controls anything, because whether it
    // does is only known once the whole walk has run; alloc_assert_control is
    // what turns the table into signals, and only then.
    std::vector<AssertSite> sites_;
    // Assertion statement -> indices into sites_. A vector because one bound
    // Statement could in principle serve two instances; the entry that matches
    // cur_scope_id() is this instance's.
    ankerl::unordered_dense::map<const slang::ast::ConcurrentAssertionStatement*, std::vector<uint32_t>> site_of_;
    // Whether any control task was seen. False means no control state exists,
    // and every monitor emits exactly what it emitted before 20.12 support.
    bool assert_control_used_ = false;
    // Whether the design contains an immediate / deferred assertion. A control
    // task selecting those types refuses rather than silently leaving them
    // running (see lower_assert_control).
    bool immediate_assertion_seen_ = false;
    // -- `disable` targets (LRM 9.6.2), filled by the Collect walk --
    //
    // Named block -> its ir::BlockInfo id. Membership is what makes the Block
    // case emit a NamedBlock region at all, so it has to be known before Emit
    // reaches the block, which is before the `disable` nested inside it. slang
    // gives every elaborated instance its own StatementBlockSymbol, so an id
    // already names one block in one instance (verified: two instances of one
    // module get L0 and L1) — the remote form needs no separate instance
    // dimension.
    ankerl::unordered_dense::map<const Symbol*, uint32_t> disable_targets_;
    // The subset some `disable` names from OUTSIDE the target — a sibling block or
    // another process. Only these carry the occupant bookkeeping the remote form
    // needs, so a V1-shaped design pays exactly what it did before.
    ankerl::unordered_dense::set<const Symbol*> disable_remote_;
    // The subset whose process also holds a deferred or procedural concurrent
    // assertion, so disabling it as the outermost scope has to drop that
    // procedure's pending reports (LRM 16.4.4 / 16.14.6.4).
    ankerl::unordered_dense::set<const Symbol*> disable_flushes_;
    // The subset that IS its own procedure's outermost scope, which is the only
    // place 16.4.4's flush applies. Recorded per target, not per procedure, so a
    // remote `disable` can answer the question about a procedure it is not in.
    ankerl::unordered_dense::set<const Symbol*> disable_roots_;
    // The subset that cannot have a single live occupant: a block inside a `fork`
    // child, or one declared in a subroutine body. Each activation is its own
    // occupant with its own frame, so the remote form refuses these.
    ankerl::unordered_dense::set<const Symbol*> disable_multi_;
    // Named blocks that enclose a `fork`, anywhere in their body. The forked children
    // are activity "enabled within" the block (LRM 9.6.2), so each child is an
    // activation of every enclosing targeted block (build_fork_child), and a `disable`
    // of one has to walk the list even from inside a single-activation process body.
    ankerl::unordered_dense::set<const Symbol*> disable_forked_;
    // Named blocks whose body calls a subroutine that forks. The children are not
    // lexically inside the block, so a `disable` cannot end them; refused.
    ankerl::unordered_dense::set<const Symbol*> disable_fork_via_call_;
    // Whether any user virtual method of the design forks (design_virtual_forks);
    // computed on first use.
    std::optional<bool> design_virtual_forks_;
    // Named blocks whose body can park in a way the abort cannot claim (a `#0` or
    // runtime-valued delay, a time-consuming task call, a fork, an `expect`); see
    // hard_park. The remote form refuses one, the lexical form does not care -- it
    // never leaves a park behind.
    ankerl::unordered_dense::set<const Symbol*> disable_hard_;
    // Memoized "calling this can suspend the caller", see subroutine_parks.
    ankerl::unordered_dense::map<const SubroutineSymbol*, bool> sub_parks_;
    // The named block that is the current procedure's outermost scope, or null;
    // see outermost_named_block. Set per procedural block, read by lower_disable.
    const Symbol* proc_root_block_ = nullptr;
    // Net signals Classify found to have >= 2 overlapping continuous drivers, so
    // Emit routes every driver to a slot and emit_wire_resolvers folds them.
    ankerl::unordered_dense::set<uint32_t> resolved_nets_;
    // Nets some driver can drive weaker than strong (an `assign`, net declaration
    // assignment or gate with an explicit weak or pull drive strength, a pullup /
    // pulldown), recorded by the Classify pass for check_mos_switch_strength.
    ankerl::unordered_dense::set<uint32_t> weak_driven_nets_;
    // Every MOS switch Classify saw, in source order, and the propagation result
    // over them (reset whenever one is added).
    std::vector<MosSwitch>     mos_switches_;
    std::optional<MosStrength> mos_strength_;
    // The subset of resolved_nets_ with at least one PROGRAM-side driver: a
    // driver lowered while in_program_ (a program's hierarchical `assign`, a
    // net declaration assignment, a `force`) or a wire_ports port copy for a
    // program child. LRM 24.3.2 puts "the driving and resolution of nets on the
    // other side of a program port connection" in the reactive region set, so
    // emit_wire_resolvers gives such a net a reactive-home resolver. Recorded
    // rather than derived because the resolver is built after every pass, long
    // after the flag that answers the question has been restored.
    ankerl::unordered_dense::set<uint32_t> program_driven_nets_;
    // Per resolved net: its per-driver slot signals (fold order), the scope id
    // the resolver process is attributed to, and a representative source loc.
    ankerl::unordered_dense::map<uint32_t, std::vector<ir::Signal>> resolved_slots_;
    // Per-net packed (strength0<<8 | strength1) for each driver slot, parallel to
    // resolved_slots_. A net any of whose slots is non-default (not 0x0303 / strong)
    // resolves through the strength-weighted StrengthRes fold instead of WireRes.
    ankerl::unordered_dense::map<uint32_t, std::vector<uint16_t>> resolved_strengths_;
    ankerl::unordered_dense::map<uint32_t, uint32_t>              resolved_scope_;
    ankerl::unordered_dense::map<uint32_t, ir::SourceLoc>         resolved_loc_;
    // Per resolved net: which reducer folds its slots (WireRes / WandRes /
    // WorRes), from the net kind (net_resolve_tag).
    ankerl::unordered_dense::map<uint32_t, ir::Inst::Tag> net_resolve_op_;
    // tri0 / tri1 nets: the resistive pull applied after the fold, and the home
    // scope / loc for the resolver (so even a driverless net emits one).
    struct PullInfo {
        int           value;
        uint32_t      scope;
        ir::SourceLoc loc;
        // Strength level of the pull (LRM 28.11): Pull (2) for tri0/tri1, Supply
        // (4) for supply0/supply1. A Supply pull always folds as a strength driver
        // (it must beat a Strong driver); a Pull pull only when some driver carries
        // an explicit strength (else the cheaper strengthless NetPull suffices).
        uint8_t strength;
    };
    ankerl::unordered_dense::map<uint32_t, PullInfo> net_pull_;
    // Nets declared with a plain net delay (`wire #5 w;`, no net_decl_assignment
    // — LRM 28.16), keyed by signal id: the single delay-value expression,
    // lowered lazily by emit_wire_resolvers once every driver has registered
    // its slot. Distinct from a delay embedded in a net-declaration ASSIGNMENT
    // (`wire #5 w = expr;`), which LRM 10.3.3 treats as that one driver's own
    // continuous-assign delay instead and lowers through lower_net_initializer.
    ankerl::unordered_dense::map<uint32_t, const Expression*> net_delay_;
    // Hierarchical name prefix of the instance currently being lowered (e.g.
    // "u_alu.u_sub."); empty for the top. Prepended to every signal name created
    // while this instance's body is processed so flattened signals stay unique.
    std::string prefix_;
    // Generate-block name segments (loop[i]/cond-block, dot-terminated)
    // accumulated since the enclosing instance's scope node, which folds no node
    // of its own. Forms the leading part of a child instance's scope-node name so
    // two same-named instances in distinct generate entries get distinct anchors
    // (@clotho.inst.<hier>). Reset entering an instance, restored on exit.
    std::string node_rel_;
    // True when flattened paths omit the single top instance's name (the
    // single-top case); %m and $printtimescale re-prepend module_.name.
    bool top_elided_ = false;
    // True while lowering the members of a `program` instance (LRM 24), so an
    // `initial` inside one becomes a Reactive process. Saved/restored per
    // instance in lower_instance.
    bool in_program_ = false;
    // The named begin blocks enclosing the statement currently being lowered,
    // outermost first. They extend the %m hierarchical name (LRM §21.2.1.5), and
    // a `disable` resolves its target against them (LRM 9.6.2 — a lexically
    // enclosing target is the form V1 lowers, see docs/design/disable.md).
    // Saved/cleared around subroutine body lowering: a body is built at its first
    // call site, whose block nesting is not the body's — which is also what makes
    // "the target encloses me in the same unit" decidable from this stack alone.
    struct OpenBlock {
        const slang::ast::StatementBlockSymbol* sym;
        // ir::BlockInfo label when some `disable` targets this block, else
        // kNoBlockLabel: an untargeted block emits no instruction at all.
        uint32_t label;
        // True for a frame of the PARENT body that a fork child re-pushes so its
        // own `disable` can still find the target (build_fork_child). The child is
        // an activation of that block but not lexically inside a region of it, so
        // it leaves by aborting itself, and %m does not repeat the parent's name
        // (the child's invoking scope already carries it).
        bool inherited = false;
    };
    static constexpr uint32_t kNoBlockLabel = UINT32_MAX;
    std::vector<OpenBlock>    named_blocks_;
    // How many user loops enclose the statement being lowered, within this body
    // (FnScope resets it: a `break` cannot bind to a loop outside its own body).
    // Read only by lower_break, to tell LRM 12.8's "innermost enclosing loop" from
    // 18.17.6's "otherwise, it terminates the randsequence block".
    uint32_t loop_depth_ = 0;
    // The lowered function set of each randsequence statement (LRM 18.17). Keyed on
    // the statement: a production takes everything it reads as a formal, so its
    // body does not depend on which enclosing body generated the block.
    ankerl::unordered_dense::map<const RandSequenceStatement*, RandSeqCtx> randseq_;
    // Femtoseconds per wheel tick (design-wide smallest time unit); 0 == unitless.
    uint64_t tick_fs_ = 0;
    // $system shell-exec access tier (LRM §20.17.1). Off rejects $system at
    // compile time; Const requires a constant-folding command string; Full
    // allows a runtime-computed command. See lower_call / the $system branch.
    ir::SystemAccess allow_system_ = ir::SystemAccess::Off;
    const SourceContext& sources_;
    // Time-unit multiplier of the scope whose code is currently being lowered: a
    // delay is scaled up by this into ticks; $time is scaled (rounded) down by
    // it. Always 1 when unitless. Saved / restored around child-instance
    // recursion and around a subroutine body, whose scope is the one that
    // DECLARES it rather than the instance that calls it (LRM 3.14.2.3).
    uint64_t time_mul_ = 1;
    // Time-precision multiplier of that same scope: precision_fs / tick_fs. A
    // real delay rounds to the scope's precision then scales up by this into
    // ticks (time_mul_ == unit/precision * prec_over_tick_). Always 1 when
    // unitless. Saved / restored alongside time_mul_.
    uint64_t prec_over_tick_ = 1;
    // Monotonic counter naming hidden compiler temporaries backed by a module
    // signal (a synthesized event net, a force site, a gate input, and the loop
    // cells of a process, which has no frame to put them in) so their flattened
    // signal names stay unique.
    uint32_t temp_id_ = 0;
    // Subroutine -> its lowered FunctionId, so each function is lowered once and
    // shared across call sites (and a recursive call resolves its own id).
    // Package/$unit-scope subroutines never populate this map directly on a
    // per-top Lowerer — function_id delegates those to globals_ instead, so a
    // package function called from multiple top instances still gets exactly
    // one FunctionId and one copy of any static local's storage.
    ankerl::unordered_dense::map<const SubroutineSymbol*, ir::FunctionId> fnmap_;
    // Covergroup type -> its model index and generated sample body (LRM 19).
    // Keyed on the CovergroupType symbol, so one declaration lowered once per
    // enclosing instance gets one entry per instance -- which is what makes the
    // model's coverpoint expressions belong to the right instance's signals.
    ankerl::unordered_dense::map<const slang::ast::CovergroupType*, CoverDecl> cover_decls_;
    // Bodies reserved by compute_vtable but not yet lowered, and the flag that says
    // to reserve rather than build. Set only while a vtable is being filled, which
    // is the one place a body build hangs off lowering a TYPE; every other
    // function_id call site is a statement or an expression, already a stable point.
    // Drained at a phase boundary by drain_pending_bodies.
    bool                                                                     defer_bodies_ = false;
    std::vector<std::pair<const SubroutineSymbol*, ir::FunctionId>>          body_pending_;
    // Body-build failure policy (see drain_pending_bodies): true while building
    // vtable-forced bodies, where an unsupported construct in a CLASS METHOD
    // compiles to a warning plus a run-time fatal stub instead of refusing the
    // design. False on the direct-call path, whose compile diagnostics stand.
    bool stub_unsupported_bodies_ = false;
    // Parameters a refused defparam targets (check_defparam_type, Collect pass):
    // their value is not of their type, so a reference to one is refused too.
    ankerl::unordered_dense::set<const slang::ast::ParameterSymbol*> refused_params_;
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
    // Design-wide waited-on class properties (WaitPropScan), or null on a standalone
    // Lowerer with no pre-pass -- which then lays out no notifier, so `@p` / `wait(p)`
    // fails loudly rather than parking on nothing.
    const WaitedProps* waited_props_ = nullptr;
    // (waited-on class property, ir::Edge) -> the object-body field index of its
    // hidden change-notifier event property (ir::ChangeNotify). Filled by
    // lower_class_type at the same time as class_prop_index_, and read by the wait
    // lowering to build `@(obj.<notifier>)`.
    ankerl::unordered_dense::map<const slang::ast::Symbol*, std::array<uint32_t, 3>> prop_notify_;
    // Virtual dispatch (LRM 8.20/8.21). class_vslot_: a class -> its method NAME ->
    // vtable slot map (inheriting the base's numbering, extended with
    // the class's own virtuals). Name-keyed so a pure virtual (a
    // MethodPrototypeSymbol) shares a slot with its concrete overrides. pure_stub_:
    // a pure prototype -> the FunctionId of its never-executed signature stub. Both
    // filled by compute_vtable / pure_stub_id.
    ankerl::unordered_dense::map<const slang::ast::ClassType*, VtableSlots>                class_vslot_;
    ankerl::unordered_dense::map<const slang::ast::MethodPrototypeSymbol*, ir::FunctionId> pure_stub_;
    // The reserved vtable slot name of a class's shallow-copy thunk (LRM 8.12). Not
    // a legal method name, so it cannot collide with a virtual method's slot.
    static constexpr std::string_view kCopySlot = "$copy";
    // Interface Ref -> the never-executed signature stub a randomization dispatch
    // through a handle of that interface uses (iface_rand_sig).
    ankerl::unordered_dense::map<ir::Ref, ir::FunctionId> iface_randomize_sig_;
    ankerl::unordered_dense::map<ir::Ref, ir::FunctionId> iface_randops_sig_;
    // Polymorphic class -> the FunctionId of its shallow-copy thunk (copy_thunk_id).
    ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::FunctionId> copy_thunk_;
    // Constrained randomization, step 1 (docs/design/constrained-randomization.md).
    // The reserved vtable slots of a class's synthesized randomization entry points.
    // Neither is a legal method name, so neither can collide with a virtual method.
    // Both are reserved for EVERY class, so the slot number of each is one constant
    // per hierarchy exactly as kCopySlot's is.
    static constexpr std::string_view kRandomizeSlot = "$randomize";
    static constexpr std::string_view kRandOpsSlot   = "$randops";
    // `$randops` opcodes. `arg` is the seed for Srandom, the on/off bit for ModeAll,
    // `ordinal << 1 | on` for ModeOne and the ordinal for ModeQuery.
    static constexpr uint64_t kRandOpSrandom   = 0;
    static constexpr uint64_t kRandOpModeAll   = 1;
    static constexpr uint64_t kRandOpModeOne   = 2;
    static constexpr uint64_t kRandOpModeQuery = 3;
    ankerl::unordered_dense::map<const slang::ast::ClassType*, ClassRand> class_rand_;
    // A rand property -> its rand ordinal. Design-wide like class_prop_index_ and
    // stable for the same reason: prefix layout gives an inherited property the same
    // ordinal in the base and in every class derived from it.
    ankerl::unordered_dense::map<const slang::ast::Symbol*, uint32_t> rand_ordinal_;
    // Class -> the FunctionIds of its two synthesized bodies. The ids are reserved by
    // compute_vtable and the bodies are built later, by drain_rand_bodies: a body
    // dispatches into the classes its `rand` handles point at, and with a cycle
    // (`rand A next`, or a mutual pair) one of those has no vtable numbering yet at
    // the point its holder is being laid out.
    ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::FunctionId> randomize_fn_;
    ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::FunctionId> randops_fn_;
    std::vector<std::pair<const slang::ast::ClassType*, ir::Ref>>              rand_pending_;
    // Depth of the lower_class_type recursion; the outermost frame drains the list.
    uint32_t class_depth_ = 0;
    // The built-in `process` class (LRM 9.7): the name of the hidden body field
    // holding the thread control-block pointer. Not a legal SV identifier, so it
    // cannot collide with a property name.
    static constexpr std::string_view kProcessCtlField = "$pcb";
    // The built-in `semaphore` class (LRM 15.3): the synthetic body's fields --
    // the key count and the two words of its FIFO wait queue. None is a legal SV
    // identifier, so none can collide with a property name.
    static constexpr std::string_view kSemKeysField = "$keys";
    static constexpr std::string_view kSemHeadField = "$getq.head";
    static constexpr std::string_view kSemTailField = "$getq.tail";
    static constexpr uint32_t         kSemKeys      = 0;
    static constexpr uint32_t         kSemQHead     = 1;
    static constexpr uint32_t         kSemQTail     = 2;
    // The built-in `mailbox #(T)` class (LRM 15.4): the bound, the message FIFO,
    // and the three wait queues -- consuming `get` waiters, space-waiting `put`
    // waiters, and non-consuming `peek` waiters, which are woken as a set.
    static constexpr std::string_view kMbxBoundField    = "$bound";
    static constexpr std::string_view kMbxFifoField     = "$fifo";
    static constexpr std::string_view kMbxGetHeadField  = "$getq.head";
    static constexpr std::string_view kMbxGetTailField  = "$getq.tail";
    static constexpr std::string_view kMbxPutHeadField  = "$putq.head";
    static constexpr std::string_view kMbxPutTailField  = "$putq.tail";
    static constexpr std::string_view kMbxPeekHeadField = "$peekq.head";
    static constexpr std::string_view kMbxPeekTailField = "$peekq.tail";
    static constexpr uint32_t         kMbxBound         = 0;
    static constexpr uint32_t         kMbxFifo          = 1;
    static constexpr uint32_t         kMbxGetHead       = 2;
    static constexpr uint32_t         kMbxGetTail       = 3;
    static constexpr uint32_t         kMbxPutHead       = 4;
    static constexpr uint32_t         kMbxPutTail       = 5;
    static constexpr uint32_t         kMbxPeekHead      = 6;
    static constexpr uint32_t         kMbxPeekTail      = 7;
    // Wait-site ids for the SyncWait / SyncCtl pairs this Lowerer emits. Unique
    // design-wide, which is stricter than the backend needs (it keys one node per
    // site per body) and costs nothing.
    uint32_t sync_site_seq_ = 0;
    // Design-wide class type ids (LRM 8.16). Owned here so a standalone Lowerer
    // works; lower_compilation points every Lowerer at one shared table instead.
    // A subroutine reached through a virtual interface handle -> its
    // ctx-parameterized body (vif_function_id). Keyed on the PLACEHOLDER symbol, so
    // it never collides with fnmap_'s per-instance entries.
    ankerl::unordered_dense::map<const SubroutineSymbol*, ir::FunctionId> vif_fnmap_;
    // Set while such a body is being built: the interface member whose definition
    // the body is compiled against (see splice_iface_ctx).
    std::optional<ir::Signal> vif_build_ctx_;
    // Set while lowering such a body: a bare call to a sibling subroutine of the
    // same interface passes the context on (vif_ctx_arg).
    bool in_vif_body_ = false;
    ClassIds  own_class_ids_;
    ClassIds* class_ids_ = &own_class_ids_;
    // Interface classes (LRM 8.26). Owned so a standalone Lowerer works;
    // lower_compilation shares one registry across every top, like ClassIds.
    IfaceIds  own_iface_ids_;
    IfaceIds* iface_ids_ = &own_iface_ids_;
    // Design-wide virtual interface registry (see VifDefs). Owned here so a
    // standalone Lowerer works; lower_compilation shares one across every top.
    VifDefs  own_vif_defs_;
    VifDefs* vif_defs_ = &own_vif_defs_;
    // The shared Lowerer that owns package/$unit-scope subroutine bodies (see
    // set_globals / function_id). Null on the shared Lowerer itself and on any
    // Lowerer built without one (e.g. tests that lower a single module with no
    // packages in play).
    Lowerer* globals_ = nullptr;
    // Inference verdicts for concurrent assertions (see set_inferred_clocks).
    // Null when nothing populated it.
    const InferredClockMap* inferred_clocks_ = nullptr;
    // Port coercion (LRM 23.3.3.1); see set_allow_port_coercion.
    bool allow_port_coercion_ = false;
    // The ports build_inout_aliases coerced to inout: wire_ports copies nothing for them.
    ankerl::unordered_dense::set<const PortSymbol*> coerced_ports_;
    // Every write target in this top's hierarchy, by the value symbol it roots at, and
    // the instance body each one is written in.
    ankerl::unordered_dense::map<const slang::ast::ValueSymbol*, std::vector<const Expression*>> writes_;
    ankerl::unordered_dense::map<const Expression*, const slang::ast::InstanceBodySymbol*>       write_in_;
    // Every value symbol this top's hierarchy reads somewhere (net_is_read), built on
    // the first question and valid for the top lower_top was given.
    const InstanceSymbol*                                        top_inst_      = nullptr;
    bool                                                         reads_scanned_ = false;
    ankerl::unordered_dense::set<const slang::ast::ValueSymbol*> read_syms_;
    // The event control of the procedure currently being lowered, or null when it
    // has none. Set by lower_procedural_block; read by lower_concurrent_assertion
    // to resolve an inferred clock inside the instance being lowered.
    const TimingControl* proc_timing_ = nullptr;
    // The function currently being lowered, or null when lowering a process. Drives
    // local-vs-signal name resolution and `return` handling.
    FnCtx* fn_ctx_ = nullptr;
    // The LHS lvalue of the compound assignment currently being lowered, read by
    // the LValueReferenceExpression slang embeds in its rewritten RHS. Null
    // outside a compound assignment.
    std::optional<ir::Index> compound_lv_;
    // The value an output port's connection assigns, read by the EmptyArgument
    // slang leaves for it in the connection's right-hand side (lower_port_output).
    // Nullopt outside one; scoped by Restore, never assigned bare.
    std::optional<ir::Index> port_value_;
    // The queue `$` resolves against inside a queue range-select's bounds (see
    // QueueDollarCtx). Nullopt outside one; scoped by Restore, never assigned
    // bare.
    std::optional<QueueDollarCtx> queue_dollar_;
    // The object under construction while a non-static property's DECLARATION
    // initializer is lowered, plus the allocated class type that selects its vtable
    // layout. LRM 8.7 runs those initializers as part of construction ("each
    // property defined in the class shall be initialized to its explicit default
    // value"), so `this` is in scope there and a bare `f()` in an initializer is an
    // ordinary method call. emit_class_property_inits lowers them at the `new` site
    // though, which has no method frame to carry `this` — and whose frame `this`, if
    // it has one, belongs to a DIFFERENT object. Nullopt outside a property
    // initializer; scoped by Restore, never assigned bare.
    // (CtorThis is defined next to class Restore, ahead of the member-function
    // signatures that name it.)
    std::optional<CtorThis> ctor_this_;
    // The receiver context a DEFAULT argument expression evaluates against. LRM
    // 13.5.3 binds a default in the scope of the subroutine DECLARATION, so an
    // implicit `this` inside one — a bare method call or property reference of the
    // callee's class — names the CALL's receiver, never the caller's own `this`.
    // slang materializes an omitted actual as the formal's cached default
    // expression (CallExpression::bindArgs), which build_call_args detects by
    // pointer identity and scopes this override around lowering exactly that
    // actual. Without it the caller's `this` was dispatched with the callee
    // class's slot number: past the end of a smaller vtable that is the easyUVM
    // begin_event.trigger() SIGSEGV, and inside a same-shape one a silent read of
    // the wrong object. Nullopt outside a default-argument expression; scoped by
    // Restore, never assigned bare. Checked ahead of ctor_this_ everywhere: a
    // default is the innermost context, and the object under construction is a
    // different object whenever the receiver was spelled out.
    std::optional<CtorThis> default_arg_this_;
    // Monotonic counter naming anonymous output / inout task-argument temps created
    // in a process body (where temps are module signals, not frame locals).
    uint32_t task_temp_seq_ = 0;
    // Accumulates static variable declaration initializers (LRM 6.8) across the
    // whole instance hierarchy into one run-once pre-init process, finalized at the
    // end of the top instance. Lazily engaged: absent when no initializer exists.
    std::optional<ir::Builder> preinit_;
    // Live signal -> its Preponed shadow. LRM 16.5: assertion expressions read
    // sampled values, and what is sampled is the VARIABLE, not the expression.
    ankerl::unordered_dense::map<uint32_t, ir::Signal> sva_shadows_;
    // In creation order, so the sampling process copies them deterministically.
    std::vector<std::pair<ir::Signal, ir::Signal>> sva_shadow_pairs_;
    // Each clocking block (LRM 14) of the design, by its symbol. Filled in
    // Collect (storage, which must exist before any process reads it) and read in
    // Emit (the sample-and-trigger process, and `@(cb)`).
    ankerl::unordered_dense::map<const Symbol*, ClockingBlockState> clocking_;
    // The one-bit per-step latches the Preponed process clears: a clocking block's
    // "my event occurred in this time step" (LRM 14.11's `##0`, and the maturation
    // window of a synchronous drive).
    std::vector<ir::Signal> preponed_clears_;
    // Set while lowering one sampled assertion atom, so value_lvalue resolves
    // through sva_shadow_of. Scoped by SampledScope, never assigned bare.
    bool sva_sampled_ = false;
    // The $past history rings of the monitor being emitted right now, or null
    // outside one. Non-null is what makes a sampled value function legal at all:
    // LRM 16.9.3 defines history per CLOCK TICK, so these functions only mean
    // something where a clocking event delimits cycles. Scoped by HistoryScope.
    SvaHistory* sva_history_ = nullptr;
    // Every history ring entry paired with the variable it shadows, in creation
    // order, so finalize_sva_history can seed each with that variable's
    // time-zero value.
    std::vector<std::pair<ir::Signal, ir::Signal>> sva_history_seeds_;
    // Diagnostics from members that failed and were recovered past, in the order
    // the walk hit them. Drained by lower_compilation.
    std::vector<Diagnostic> diags_;
    // Set once any member was abandoned. The module is still returned -- it is
    // analysable, and `clotho lint` still wants it -- but it is missing
    // behaviour, so it MUST NOT reach codegen. lower_compilation turns this into
    // LowerResult::ok == false, which is the gate.
    bool poisoned_ = false;
    // Set when $dumpvars is lowered: the design asks for a whole-design VCD dump.
    bool dumps_vcd_ = false;
};

} // namespace clotho::lower
