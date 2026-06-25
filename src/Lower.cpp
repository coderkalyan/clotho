#include "Lower.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <slang/ast/Compilation.h>
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
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/ast/symbols/ValueSymbol.h>
#include <slang/ast/symbols/VariableSymbols.h>
#include <slang/ast/types/Type.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/TextDiagnosticClient.h>
#include <slang/syntax/SyntaxTree.h>

#include "SourceMap.h"
#include "ir/Builder.h"

namespace clotho {

namespace {

using namespace slang::ast;

// Raised when lowering meets a construct outside the proof-of-concept's scope.
// Caught at the top level and turned into a diagnostic.
struct LowerError {
    std::string message;
};

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

// Walks the elaborated AST of one top module into an ir::Module.
class Lowerer {
public:
    explicit Lowerer(ir::Module& module)
      : module_(module) {}

    void lower_instance(const InstanceSymbol& top) {
        module_.name                   = std::string(top.name);
        const InstanceBodySymbol& body = top.body;
        collect_signals(body);

        for (const Symbol& m : body.members()) {
            if (m.kind == SymbolKind::ContinuousAssign) {
                lower_continuous_assign(m.as<ContinuousAssignSymbol>());
            } else if (m.kind == SymbolKind::ProceduralBlock) {
                lower_procedural_block(m.as<ProceduralBlockSymbol>());
            }
        }
    }

private:
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
        if (!ct.isIntegral()) {
            fail("unsupported non-integral type");
        }

        return make_int_type(ct.getBitWidth(), ct.isFourState(), ct.isSigned());
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

    // Intern an integer literal, preserving x/z bits. Slang stores a 4-state
    // SVInt as [value words][unknown words] with X=(val0,unk1), Z=(val1,unk1) —
    // the opposite val convention from the pool's VPI planes (X=(val1,xz1),
    // Z=(val0,xz1)). The unknown word IS the xz plane; the val plane is
    // value^unknown so the per-bit codes line up. 2-state literals take the fast
    // uint64 path.
    ir::Ref lower_int_literal(const IntegerLiteral& lit) {
        const ir::Ref      ty = lower_type(*lit.type);
        const slang::SVInt v  = lit.getValue();
        if (!v.hasUnknown()) {
            return module_.pool.put_int(ty, v.as<uint64_t>().value_or(0));
        }

        const uint32_t        width = v.getBitWidth();
        const uint32_t        words = (width + 63) / 64;
        const uint64_t*       raw   = v.getRawPtr();
        std::vector<uint64_t> planes(static_cast<size_t>(words) * 2);
        for (uint32_t i = 0; i < words; ++i) {
            planes[i]         = raw[i] ^ raw[words + i]; // val plane (VPI)
            planes[words + i] = raw[words + i];          // xz plane (unknown)
        }

        const uint32_t       pb = ir::plane_bytes(width);
        std::vector<uint8_t> bytes(static_cast<size_t>(pb) * 2, 0);
        std::memcpy(bytes.data(), planes.data(), pb);
        std::memcpy(bytes.data() + pb, planes.data() + words, pb);

        return module_.pool.put_value(ty, bytes);
    }

    // -- Signals --

    void collect_signals(const InstanceBodySymbol& body) {
        // First pass: record port directions by the symbol they back.
        ankerl::unordered_dense::map<const Symbol*, ir::SignalInfo::Direction> port_dir;
        for (const Symbol& m : body.members()) {
            if (m.kind == SymbolKind::Port) {
                const PortSymbol& p = m.as<PortSymbol>();
                if (p.internalSymbol) {
                    port_dir[p.internalSymbol] = direction_of(p.direction);
                }
            }
        }

        // Second pass: nets and variables become signals.
        for (const Symbol& m : body.members()) {
            if (m.kind != SymbolKind::Net && m.kind != SymbolKind::Variable) {
                continue;
            }
            const ValueSymbol& vs = m.as<ValueSymbol>();

            ir::SignalInfo info;
            info.name        = std::string(vs.name);
            info.type        = lower_type(vs.getType());
            const auto found = port_dir.find(&m);
            info.dir         = found != port_dir.end() ? found->second : ir::SignalInfo::Direction::Internal;

            sigmap_[&vs] = module_.add_signal(std::move(info));
        }
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
        info.name = std::string(vs.name);
        info.type = lower_type(vs.getType());
        info.dir  = ir::SignalInfo::Direction::Internal;

        const ir::Signal sig = module_.add_signal(std::move(info));
        sigmap_[&vs]         = sig;

        return sig;
    }

    // -- Processes --

    void lower_continuous_assign(const ContinuousAssignSymbol& sym) {
        if (sym.getDelay() != nullptr) {
            // `assign #N lhs = rhs;` — the net-delay is dropped today, so the
            // update would land at the wrong time. Reject until the scheduler exists.
            fail("continuous assignment delay (assign #N) is not supported");
        }
        const Expression& e = sym.getAssignment();
        if (e.kind != ExpressionKind::Assignment) {
            fail("malformed continuous assignment");
        }

        ir::Builder b(module_.pool);
        lower_assign(e.as<AssignmentExpression>(), b);
        // TODO: a continuous assign re-evaluates on any change of its RHS signals;
        // that sensitivity is not yet inferred, so the wait list is left empty.
        module_.add_process(
          ir::Process{ .activation = ir::Activation::Always, .loc = to_source_loc(sym.location), .ir = b.finish() });
    }

    void lower_procedural_block(const ProceduralBlockSymbol& sym) {
        ir::Activation             activation = ir::Activation::Always;
        std::vector<ir::WaitEvent> wait_events;

        if (sym.procedureKind == ProceduralBlockKind::Initial) {
            activation = ir::Activation::Initial;
        }

        const Statement* body = &sym.getBody();

        // A top-level event control is the process's sensitivity: collect its wait
        // events here and lower the statement underneath. A non-event top-level
        // timing (e.g. `always #5 ...`) is NOT sensitivity — it falls through to
        // lower_stmt, whose Timed case rejects it loudly.
        // TODO: implicit (@*) and inferred combinational sensitivity are not yet
        // collected, so those processes get an empty wait list.
        if (body->kind == StatementKind::Timed) {
            const TimedStatement& timed = body->as<TimedStatement>();
            if (is_event_control(timed.timing)) {
                collect_wait_events(timed.timing, wait_events);
                body = &timed.stmt;
            }
        }

        ir::Builder b(module_.pool);
        lower_stmt(*body, b);
        module_.add_process(ir::Process{ .activation  = activation,
                                         .wait_events = std::move(wait_events),
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
        if (e.kind == ExpressionKind::NamedValue) {
            return signal_of(e.as<NamedValueExpression>().symbol);
        }
        fail("unsupported event expression");
    }

    // Lower an intra-body event control `@(...)` to a Wait instruction. Only a
    // single signal event is supported for now; a multi-signal list (@(a or b))
    // or implicit (@*) wait would need the process to register on several queues
    // at once, which the backend cannot yet do safely.
    void lower_event_wait(const TimingControl& tc, ir::Builder& b, ir::SourceLoc l) {
        std::vector<ir::WaitEvent> events;
        collect_wait_events(tc, events);
        if (events.size() != 1) {
            fail("only single-signal intra-body event waits (@(sig)) are supported yet");
        }
        b.wait(events[0].signal, static_cast<uint32_t>(events[0].edge), l);
    }

    // -- Statements --

    void lower_stmt(const Statement& stmt, ir::Builder& b) {
        switch (stmt.kind) {
            case StatementKind::List:
                for (const Statement* s : stmt.as<StatementList>().list) {
                    lower_stmt(*s, b);
                }
                break;
            case StatementKind::Block:
                lower_stmt(stmt.as<BlockStatement>().body, b);
                break;
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
                    b.unary(ir::Inst::Tag::Delay, lower_expr(amount, b), loc(stmt));
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
                lower_forever(stmt.as<ForeverLoopStatement>(), b);
                break;
            default:
                fail("unsupported statement");
        }
    }

    void lower_conditional(const ConditionalStatement& c, ir::Builder& b) {
        if (c.conditions.size() != 1) {
            fail("unsupported multi-condition if");
        }
        const ir::Index pred = lower_expr(*c.conditions[0].expr, b);

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

    // A local declaration statement: register the variable's storage and, if it
    // has an initializer, store it.
    void lower_var_decl(const VariableSymbol& vs, ir::Builder& b) {
        const ir::Signal sig = declare_local(vs);
        if (const Expression* init = vs.getInitializer()) {
            const ir::SourceLoc l = loc(*init);
            b.binary(ir::Inst::Tag::Store, b.signal(sig, l), lower_expr(*init, b), l);
        }
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
            lower_var_decl(*vs, b);
        }
        for (const Expression* init : f.initializers) {
            lower_for_step(*init, b);
        }

        b.open_region();
        const ir::Index pred = f.stopExpr != nullptr ? lower_expr(*f.stopExpr, b) : true_const(b, loc(f));
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
        const ir::Index     pred = lower_expr(w.cond, b);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_stmt(w.body, b);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::ExtraSlice step = b.close_region(); // empty

        b.loop(cond, pred, body, step, loc(w));
    }

    // forever-loop: a while-loop with a constant-true condition.
    void lower_forever(const ForeverLoopStatement& f, ir::Builder& b) {
        b.open_region();
        const ir::Index     pred = true_const(b, loc(f));
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        lower_stmt(f.body, b);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::ExtraSlice step = b.close_region(); // empty

        b.loop(cond, pred, body, step, loc(f));
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
            fail("only system task calls are supported as statements");
        }
        const std::string_view name = call.getSubroutineName();
        if (name == "$display" || name == "$write") {
            lower_display(call, name == "$display", b);
            return;
        }
        fail("unsupported system task '" + std::string(name) + "'");
    }

    // Lower $display / $write into a single format string plus the value args its
    // specifiers consume. A string-literal argument contributes its text verbatim
    // (its %-specifiers pull from following value args, SV-style); a bare value
    // argument not covered by an earlier specifier gets a default `%0d`.
    void lower_display(const CallExpression& call, bool newline, ir::Builder& b) {
        std::string            fmt;
        std::vector<ir::Index> args;
        int                    pending = 0; // unmatched specifiers seen so far

        for (const Expression* arg : call.arguments()) {
            if (arg->kind == ExpressionKind::StringLiteral) {
                const std::string_view text = arg->as<StringLiteral>().getValue();
                fmt += text;
                pending += count_specifiers(text);
            } else {
                args.push_back(lower_expr(*arg, b));
                if (pending > 0) {
                    --pending; // consumed by an earlier format specifier
                } else {
                    fmt += "%0d"; // bare value: default decimal
                }
            }
        }

        b.display(fmt, newline, args, loc(call));
    }

    // Count format specifiers in `s`, treating `%%` as an escaped literal percent.
    static int count_specifiers(std::string_view s) {
        int n = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '%') {
                continue;
            }
            if (i + 1 < s.size() && s[i + 1] == '%') {
                ++i; // skip the escaped percent
                continue;
            }
            ++n;
        }

        return n;
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
        const ir::Index lhs = lower_lvalue(a.left(), b);
        const ir::Index rhs = lower_expr(a.right(), b);
        // TODO(nonblocking): StoreNb is lowered identically to a blocking Store in
        // the backend. Correct NBA semantics need two-phase update — sample every
        // RHS in the active region, then commit in the NBA region — so that e.g.
        // `a <= b; b <= a;` swaps rather than copying.
        const ir::Inst::Tag tag = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
        b.binary(tag, lhs, rhs, loc(a));
    }

    // -- Expressions --

    // Lower an expression as an rvalue, returning the SSA value index.
    ir::Index lower_expr(const Expression& expr, ir::Builder& b) {
        const ir::SourceLoc l = loc(expr);
        switch (expr.kind) {
            case ExpressionKind::IntegerLiteral:
                return b.constant(lower_int_literal(expr.as<IntegerLiteral>()), l);
            case ExpressionKind::NamedValue: {
                const ir::Signal sig = signal_of(expr.as<NamedValueExpression>().symbol);
                return b.unary(ir::Inst::Tag::Load, b.signal(sig, l), l);
            }
            case ExpressionKind::UnaryOp:
                return lower_unary(expr.as<UnaryExpression>(), b);
            case ExpressionKind::BinaryOp:
                return lower_binary(expr.as<BinaryExpression>(), b);
            case ExpressionKind::ConditionalOp: {
                const ConditionalExpression& c = expr.as<ConditionalExpression>();
                if (c.conditions.size() != 1) {
                    fail("unsupported multi-condition ?:");
                }
                const ir::Index pred = lower_expr(*c.conditions[0].expr, b);
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
                const ElementSelectExpression& es   = expr.as<ElementSelectExpression>();
                const ir::Index                base = lower_lvalue(es.value(), b);
                const ir::Index                idx  = lower_expr(es.selector(), b);
                return b.unary(ir::Inst::Tag::Load, b.binary(ir::Inst::Tag::BitSelect, base, idx, l), l);
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs = expr.as<RangeSelectExpression>();
                if (rs.getSelectionKind() != RangeSelectionKind::Simple) {
                    fail("only constant [msb:lsb] part-selects are supported");
                }
                const ir::Index base = lower_lvalue(rs.value(), b);
                const auto      msb  = static_cast<uint32_t>(const_uint(rs.left()));
                const auto      lsb  = static_cast<uint32_t>(const_uint(rs.right()));
                return b.unary(ir::Inst::Tag::Load, b.part_select(base, msb, lsb, l), l);
            }
            case ExpressionKind::Conversion: {
                const ConversionExpression& cv = expr.as<ConversionExpression>();
                return b.resize(lower_expr(cv.operand(), b), lower_type(*expr.type), l);
            }
            default:
                fail("unsupported expression");
        }
    }

    ir::Index lower_unary(const UnaryExpression& u, ir::Builder& b) {
        const ir::SourceLoc l  = loc(u);
        const ir::Index     op = lower_expr(u.operand(), b);
        using U                = slang::ast::UnaryOperator;
        using T                = ir::Inst::Tag;
        switch (u.op) {
            case U::BitwiseNot:
                return b.unary(T::Bnot, op, l);
            case U::LogicalNot:
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

    ir::Index lower_binary(const BinaryExpression& bin, ir::Builder& b) {
        const ir::SourceLoc l   = loc(bin);
        const ir::Index     lhs = lower_expr(bin.left(), b);
        const ir::Index     rhs = lower_expr(bin.right(), b);
        using B                 = slang::ast::BinaryOperator;
        using T                 = ir::Inst::Tag;

        switch (bin.op) {
            case B::Add:
                return b.binary(T::Add, lhs, rhs, l);
            case B::Subtract:
                return b.binary(T::Sub, lhs, rhs, l);
            case B::Multiply:
                return b.binary(T::Mul, lhs, rhs, l);
            case B::BinaryAnd:
                return b.binary(T::Band, lhs, rhs, l);
            case B::BinaryOr:
                return b.binary(T::Bor, lhs, rhs, l);
            case B::BinaryXor:
                return b.binary(T::Bxor, lhs, rhs, l);
            case B::BinaryXnor:
                return b.unary(T::Bnot, b.binary(T::Bxor, lhs, rhs, l), l);
            case B::Equality:
                return b.binary(T::Eq, lhs, rhs, l);
            case B::Inequality:
                return b.binary(T::Ne, lhs, rhs, l);
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

    // Lower an expression as an lvalue, returning the lvalue index (no Load).
    ir::Index lower_lvalue(const Expression& expr, ir::Builder& b) {
        const ir::SourceLoc l = loc(expr);
        switch (expr.kind) {
            case ExpressionKind::NamedValue:
                return b.signal(signal_of(expr.as<NamedValueExpression>().symbol), l);
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es   = expr.as<ElementSelectExpression>();
                const ir::Index                base = lower_lvalue(es.value(), b);
                const ir::Index                idx  = lower_expr(es.selector(), b);
                return b.binary(ir::Inst::Tag::BitSelect, base, idx, l);
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs = expr.as<RangeSelectExpression>();
                if (rs.getSelectionKind() != RangeSelectionKind::Simple) {
                    fail("only constant [msb:lsb] part-selects are supported");
                }
                const ir::Index base = lower_lvalue(rs.value(), b);
                const auto      msb  = static_cast<uint32_t>(const_uint(rs.left()));
                const auto      lsb  = static_cast<uint32_t>(const_uint(rs.right()));
                return b.part_select(base, msb, lsb, l);
            }
            default:
                fail("unsupported assignment target");
        }
    }

    // Read a constant integer expression (literal, possibly behind conversions).
    uint64_t const_uint(const Expression& e) {
        const Expression* p = &e;
        while (p->kind == ExpressionKind::Conversion) {
            p = &p->as<ConversionExpression>().operand();
        }
        if (p->kind == ExpressionKind::IntegerLiteral) {
            return p->as<IntegerLiteral>().getValue().as<uint64_t>().value_or(0);
        }
        fail("expected a constant integer");
    }

    ir::Module& module_;
    // Maps an elaborated value symbol to its module Signal handle.
    ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal> sigmap_;
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

    // Analysis: walk the elaborated AST into clotho IR.
    const auto t_an   = std::chrono::steady_clock::now();
    auto       module = std::make_unique<ir::Module>();
    try {
        Lowerer(*module).lower_instance(*root.topInstances[0]);
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
