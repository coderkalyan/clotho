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
#include <slang/ast/statements/MiscStatements.h>
#include <slang/ast/symbols/BlockSymbols.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/MemberSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/ast/symbols/ValueSymbol.h>
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
        module_.add_process(
          ir::Process{ .kind = ir::ProcessKind::Comb, .loc = to_source_loc(sym.location), .ir = b.finish() });
    }

    void lower_procedural_block(const ProceduralBlockSymbol& sym) {
        ir::ProcessKind kind  = ir::ProcessKind::Comb;
        ir::Signal      clock = ir::Signal{ 0 };
        ir::Edge        edge  = ir::Edge::Posedge;

        if (sym.procedureKind == ProceduralBlockKind::Initial) {
            kind = ir::ProcessKind::Initial;
        }

        const Statement* body = &sym.getBody();

        // A top-level event control is the process's sensitivity: consume it here
        // and lower the statement underneath. An edge event (@(posedge/negedge
        // clk)) makes the process sequential; a level or implicit list (@*,
        // @(a or b)) leaves it combinational. A non-event top-level timing (e.g.
        // `always #5 ...`) is NOT sensitivity — it falls through to lower_stmt,
        // whose Timed case rejects it loudly.
        // TODO: honor the full sensitivity list — only the edge clock is modeled.
        // Level-sensitive lists are treated as always-evaluated combinational
        // logic, and the per-signal trigger set is otherwise ignored.
        if (body->kind == StatementKind::Timed) {
            const TimedStatement& timed = body->as<TimedStatement>();
            if (is_event_control(timed.timing)) {
                const SignalEventControl* ev = find_edge_event(timed.timing);
                if (ev != nullptr && ev->edge != slang::ast::EdgeKind::None) {
                    kind  = ir::ProcessKind::Sequential;
                    edge  = ev->edge == slang::ast::EdgeKind::NegEdge ? ir::Edge::Negedge : ir::Edge::Posedge;
                    clock = clock_signal(ev->expr);
                }
                body = &timed.stmt;
            }
        }

        ir::Builder b(module_.pool);
        lower_stmt(*body, b);
        module_.add_process(ir::Process{
          .kind = kind, .clock = clock, .clock_edge = edge, .loc = to_source_loc(sym.location), .ir = b.finish() });
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

    // Find the first edge-triggered signal event in a timing control (descends
    // into event lists, e.g. @(posedge clk or negedge rst)).
    const SignalEventControl* find_edge_event(const TimingControl& tc) {
        if (tc.kind == TimingControlKind::SignalEvent) {
            return &tc.as<SignalEventControl>();
        }
        if (tc.kind == TimingControlKind::EventList) {
            for (const TimingControl* e : tc.as<EventListControl>().events) {
                if (const SignalEventControl* found = find_edge_event(*e)) {
                    return found;
                }
            }
        }

        return nullptr;
    }

    ir::Signal clock_signal(const Expression& e) {
        if (e.kind == ExpressionKind::NamedValue) {
            return signal_of(e.as<NamedValueExpression>().symbol);
        }
        fail("unsupported clock expression");
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
            case StatementKind::Timed:
                // A timing control reached here is nested inside a process body — a
                // delay (#10), an intra-body event wait (@(posedge clk)), or
                // wait(expr). None can suspend yet: there is no scheduler to resume
                // on, so running the body immediately would silently violate timing
                // semantics. Reject it loudly. (A process's top-level @(edge)
                // sensitivity is consumed in lower_procedural_block and never lands
                // here.)
                // TODO: lower delays / event waits to a suspend + reschedule on the
                // timing wheel (src/Wheel.h) once the sim loop exists.
                fail("delay / event control / wait inside a process body is not "
                     "supported yet (no scheduler)");
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
            case StatementKind::VariableDeclaration:
                // No-op for the PoC: empty statements and local declarations carry
                // nothing to lower.
                break;
            case StatementKind::Case:
                lower_case(stmt.as<CaseStatement>(), b);
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

LowerResult lower_compilation(slang::ast::Compilation& comp) {
    LowerResult result;

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
        return result;
    }

    const RootSymbol& root = comp.getRoot();
    if (root.topInstances.empty()) {
        result.diagnostics += "error: no top-level module found\n";
        return result;
    }

    auto module = std::make_unique<ir::Module>();
    try {
        Lowerer(*module).lower_instance(*root.topInstances[0]);
    } catch (const LowerError& e) {
        result.diagnostics += "error: " + e.message + "\n";
        return result;
    }

    result.module = std::move(module);
    result.ok     = true;

    return result;
}

} // namespace

LowerResult lower_file(std::string_view path) {
    auto tree = slang::syntax::SyntaxTree::fromFile(path);
    if (!tree) {
        LowerResult result;
        result.diagnostics =
          "error: cannot read '" + std::string(path) + "': " + std::string(tree.error().second) + "\n";
        return result;
    }

    slang::ast::Compilation comp;
    comp.addSyntaxTree(*tree);

    return lower_compilation(comp);
}

LowerResult lower_text(std::string_view source) {
    slang::ast::Compilation comp;
    comp.addSyntaxTree(slang::syntax::SyntaxTree::fromText(source));

    return lower_compilation(comp);
}

} // namespace clotho
