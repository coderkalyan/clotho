#include "Lower.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <slang/ast/Compilation.h>
#include <slang/ast/Expression.h>
#include <slang/ast/Statement.h>
#include <slang/ast/SemanticFacts.h>
#include <slang/ast/TimingControl.h>
#include <slang/ast/expressions/AssignmentExpressions.h>
#include <slang/ast/expressions/ConversionExpression.h>
#include <slang/ast/expressions/LiteralExpressions.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/expressions/OperatorExpressions.h>
#include <slang/ast/expressions/Operator.h>
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

ir::SignalInfo::Dir dirOf(slang::ast::ArgumentDirection d) {
    switch (d) {
        case slang::ast::ArgumentDirection::In:
            return ir::SignalInfo::Dir::Input;
        case slang::ast::ArgumentDirection::Out:
            return ir::SignalInfo::Dir::Output;
        case slang::ast::ArgumentDirection::InOut:
            return ir::SignalInfo::Dir::Inout;
        case slang::ast::ArgumentDirection::Ref:
            return ir::SignalInfo::Dir::Internal;
    }

    return ir::SignalInfo::Dir::Internal;
}

// Walks the elaborated AST of one top module into an ir::Module.
class Lowerer {
public:
    explicit Lowerer(ir::Module& module)
      : module_(module) {}

    void lowerInstance(const InstanceSymbol& top) {
        module_.name = std::string(top.name);
        const InstanceBodySymbol& body = top.body;
        collectSignals(body);

        for (const Symbol& m : body.members()) {
            if (m.kind == SymbolKind::ContinuousAssign) {
                lowerContinuousAssign(m.as<ContinuousAssignSymbol>());
            } else if (m.kind == SymbolKind::ProceduralBlock) {
                lowerProceduralBlock(m.as<ProceduralBlockSymbol>());
            }
        }
    }

private:
    [[noreturn]] void fail(std::string msg) {
        throw LowerError{ std::move(msg) };
    }

    ir::SourceLoc loc(const Expression& e) const {
        return toSourceLoc(e.sourceRange.start());
    }
    ir::SourceLoc loc(const Statement& s) const {
        return toSourceLoc(s.sourceRange.start());
    }

    // -- Types --

    ir::Ref lowerType(const slang::ast::Type& type) {
        const slang::ast::Type& ct = type.getCanonicalType();
        if (ct.isVoid()) {
            return module_.pool.void_type;
        }
        if (!ct.isIntegral()) {
            fail("unsupported non-integral type");
        }

        const uint32_t width    = ct.getBitWidth();
        const bool     is_four  = ct.isFourState();
        const bool     is_signed = ct.isSigned();
        const ir::Ref  element  = is_four ? module_.pool.logic_type : module_.pool.bit_type;

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

    // -- Signals --

    void collectSignals(const InstanceBodySymbol& body) {
        // First pass: record port directions by the symbol they back.
        std::unordered_map<const Symbol*, ir::SignalInfo::Dir> port_dir;
        for (const Symbol& m : body.members()) {
            if (m.kind == SymbolKind::Port) {
                const PortSymbol& p = m.as<PortSymbol>();
                if (p.internalSymbol) {
                    port_dir[p.internalSymbol] = dirOf(p.direction);
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
            info.type        = lowerType(vs.getType());
            const auto found = port_dir.find(&m);
            info.dir         = found != port_dir.end() ? found->second : ir::SignalInfo::Dir::Internal;

            sigmap_[&vs] = module_.addSignal(std::move(info));
        }
    }

    ir::Signal signalOf(const ValueSymbol& sym) {
        const auto it = sigmap_.find(&sym);
        if (it == sigmap_.end()) {
            fail("reference to unknown signal '" + std::string(sym.name) + "'");
        }

        return it->second;
    }

    // -- Processes --

    void lowerContinuousAssign(const ContinuousAssignSymbol& sym) {
        const Expression& e = sym.getAssignment();
        if (e.kind != ExpressionKind::Assignment) {
            fail("malformed continuous assignment");
        }

        ir::Builder b(module_.pool);
        lowerAssign(e.as<AssignmentExpression>(), b);
        module_.addProcess(ir::Process{ .kind = ir::ProcKind::Comb, .ir = b.finish() });
    }

    void lowerProceduralBlock(const ProceduralBlockSymbol& sym) {
        ir::ProcKind kind  = ir::ProcKind::Comb;
        ir::Signal   clock = ir::Signal{ 0 };
        ir::Edge     edge  = ir::Edge::Posedge;

        const Statement& body = sym.getBody();
        if (body.kind == StatementKind::Timed) {
            const TimedStatement&      timed = body.as<TimedStatement>();
            const SignalEventControl*  ev    = findEdgeEvent(timed.timing);
            if (ev != nullptr && ev->edge != slang::ast::EdgeKind::None) {
                kind  = ir::ProcKind::Sequential;
                edge  = ev->edge == slang::ast::EdgeKind::NegEdge ? ir::Edge::Negedge : ir::Edge::Posedge;
                clock = clockSignal(ev->expr);
            }
        }

        ir::Builder b(module_.pool);
        lowerStmt(body, b);
        module_.addProcess(
            ir::Process{ .kind = kind, .clock = clock, .clock_edge = edge, .ir = b.finish() });
    }

    // Find the first edge-triggered signal event in a timing control (descends
    // into event lists, e.g. @(posedge clk or negedge rst)).
    const SignalEventControl* findEdgeEvent(const TimingControl& tc) {
        if (tc.kind == TimingControlKind::SignalEvent) {
            return &tc.as<SignalEventControl>();
        }
        if (tc.kind == TimingControlKind::EventList) {
            for (const TimingControl* e : tc.as<EventListControl>().events) {
                if (const SignalEventControl* found = findEdgeEvent(*e)) {
                    return found;
                }
            }
        }

        return nullptr;
    }

    ir::Signal clockSignal(const Expression& e) {
        if (e.kind == ExpressionKind::NamedValue) {
            return signalOf(e.as<NamedValueExpression>().symbol);
        }
        fail("unsupported clock expression");
    }

    // -- Statements --

    void lowerStmt(const Statement& stmt, ir::Builder& b) {
        switch (stmt.kind) {
            case StatementKind::List:
                for (const Statement* s : stmt.as<StatementList>().list) {
                    lowerStmt(*s, b);
                }
                break;
            case StatementKind::Block:
                lowerStmt(stmt.as<BlockStatement>().body, b);
                break;
            case StatementKind::Timed:
                // Timing already consumed for sensitivity; lower the body.
                lowerStmt(stmt.as<TimedStatement>().stmt, b);
                break;
            case StatementKind::ExpressionStatement: {
                const Expression& e = stmt.as<ExpressionStatement>().expr;
                if (e.kind != ExpressionKind::Assignment) {
                    fail("unsupported expression statement");
                }
                lowerAssign(e.as<AssignmentExpression>(), b);
                break;
            }
            case StatementKind::Conditional:
                lowerConditional(stmt.as<ConditionalStatement>(), b);
                break;
            case StatementKind::Empty:
            case StatementKind::VariableDeclaration:
                // No-op for the PoC: empty statements and local declarations carry
                // nothing to lower.
                break;
            case StatementKind::Case:
                fail("case statements are not supported");
            default:
                fail("unsupported statement");
        }
    }

    void lowerConditional(const ConditionalStatement& c, ir::Builder& b) {
        if (c.conditions.size() != 1) {
            fail("unsupported multi-condition if");
        }
        const ir::Index pred = lowerExpr(*c.conditions[0].expr, b);

        b.openRegion();
        lowerStmt(c.ifTrue, b);
        const ir::ExtraSlice then_region = b.closeRegion();

        b.openRegion();
        if (c.ifFalse != nullptr) {
            lowerStmt(*c.ifFalse, b);
        }
        const ir::ExtraSlice else_region = b.closeRegion();

        b.cond(pred, then_region, else_region, loc(c));
    }

    void lowerAssign(const AssignmentExpression& a, ir::Builder& b) {
        if (a.isCompound()) {
            fail("compound assignment not supported");
        }
        const ir::Index  lhs = lowerLvalue(a.left(), b);
        const ir::Index  rhs = lowerExpr(a.right(), b);
        const ir::Inst::Tag tag = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
        b.binary(tag, lhs, rhs, loc(a));
    }

    // -- Expressions --

    // Lower an expression as an rvalue, returning the SSA value index.
    ir::Index lowerExpr(const Expression& expr, ir::Builder& b) {
        const ir::SourceLoc l = loc(expr);
        switch (expr.kind) {
            case ExpressionKind::IntegerLiteral: {
                const ir::Ref  ty   = lowerType(*expr.type);
                const uint64_t bits = expr.as<IntegerLiteral>().getValue().as<uint64_t>().value_or(0);
                return b.constant(module_.pool.putInt(ty, bits), l);
            }
            case ExpressionKind::NamedValue: {
                const ir::Signal sig = signalOf(expr.as<NamedValueExpression>().symbol);
                return b.unary(ir::Inst::Tag::Load, b.signal(sig, l), l);
            }
            case ExpressionKind::UnaryOp:
                return lowerUnary(expr.as<UnaryExpression>(), b);
            case ExpressionKind::BinaryOp:
                return lowerBinary(expr.as<BinaryExpression>(), b);
            case ExpressionKind::ConditionalOp: {
                const ConditionalExpression& c = expr.as<ConditionalExpression>();
                if (c.conditions.size() != 1) {
                    fail("unsupported multi-condition ?:");
                }
                const ir::Index pred = lowerExpr(*c.conditions[0].expr, b);
                const ir::Index a    = lowerExpr(c.left(), b);
                const ir::Index bb   = lowerExpr(c.right(), b);
                return b.mux(pred, a, bb, l);
            }
            case ExpressionKind::Concatenation: {
                std::vector<ir::Index> parts;
                for (const Expression* op : expr.as<ConcatenationExpression>().operands()) {
                    parts.push_back(lowerExpr(*op, b));
                }
                return b.concat(parts, l);
            }
            case ExpressionKind::Replication: {
                const ReplicationExpression& r     = expr.as<ReplicationExpression>();
                const uint64_t               count = constUInt(r.count());
                return b.repl(lowerExpr(r.concat(), b), static_cast<uint32_t>(count), l);
            }
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es = expr.as<ElementSelectExpression>();
                const ir::Index base = lowerLvalue(es.value(), b);
                const ir::Index idx  = lowerExpr(es.selector(), b);
                return b.unary(ir::Inst::Tag::Load, b.binary(ir::Inst::Tag::BitSelect, base, idx, l), l);
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs = expr.as<RangeSelectExpression>();
                if (rs.getSelectionKind() != RangeSelectionKind::Simple) {
                    fail("only constant [msb:lsb] part-selects are supported");
                }
                const ir::Index base = lowerLvalue(rs.value(), b);
                const auto      msb  = static_cast<uint32_t>(constUInt(rs.left()));
                const auto      lsb  = static_cast<uint32_t>(constUInt(rs.right()));
                return b.unary(ir::Inst::Tag::Load, b.partSelect(base, msb, lsb, l), l);
            }
            case ExpressionKind::Conversion: {
                const ConversionExpression& cv = expr.as<ConversionExpression>();
                return b.resize(lowerExpr(cv.operand(), b), lowerType(*expr.type), l);
            }
            default:
                fail("unsupported expression");
        }
    }

    ir::Index lowerUnary(const UnaryExpression& u, ir::Builder& b) {
        const ir::SourceLoc l  = loc(u);
        const ir::Index     op = lowerExpr(u.operand(), b);
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
                const ir::Index zero = b.constant(module_.pool.putInt(lowerType(*u.type), 0), l);
                return b.binary(T::Sub, zero, op, l);
            }
            default:
                fail("unsupported unary operator");
        }
    }

    ir::Index lowerBinary(const BinaryExpression& bin, ir::Builder& b) {
        const ir::SourceLoc l   = loc(bin);
        const ir::Index     lhs = lowerExpr(bin.left(), b);
        const ir::Index     rhs = lowerExpr(bin.right(), b);
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
    ir::Index lowerLvalue(const Expression& expr, ir::Builder& b) {
        const ir::SourceLoc l = loc(expr);
        switch (expr.kind) {
            case ExpressionKind::NamedValue:
                return b.signal(signalOf(expr.as<NamedValueExpression>().symbol), l);
            case ExpressionKind::ElementSelect: {
                const ElementSelectExpression& es = expr.as<ElementSelectExpression>();
                const ir::Index base = lowerLvalue(es.value(), b);
                const ir::Index idx  = lowerExpr(es.selector(), b);
                return b.binary(ir::Inst::Tag::BitSelect, base, idx, l);
            }
            case ExpressionKind::RangeSelect: {
                const RangeSelectExpression& rs = expr.as<RangeSelectExpression>();
                if (rs.getSelectionKind() != RangeSelectionKind::Simple) {
                    fail("only constant [msb:lsb] part-selects are supported");
                }
                const ir::Index base = lowerLvalue(rs.value(), b);
                const auto      msb  = static_cast<uint32_t>(constUInt(rs.left()));
                const auto      lsb  = static_cast<uint32_t>(constUInt(rs.right()));
                return b.partSelect(base, msb, lsb, l);
            }
            default:
                fail("unsupported assignment target");
        }
    }

    // Read a constant integer expression (literal, possibly behind conversions).
    uint64_t constUInt(const Expression& e) {
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
    std::unordered_map<const ValueSymbol*, ir::Signal> sigmap_;
};

LowerResult lowerCompilation(slang::ast::Compilation& comp) {
    LowerResult result;

    const slang::Diagnostics&    diags = comp.getAllDiagnostics();
    const slang::SourceManager*  sm    = comp.getSourceManager();
    bool                         has_error = false;
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
        Lowerer(*module).lowerInstance(*root.topInstances[0]);
    } catch (const LowerError& e) {
        result.diagnostics += "error: " + e.message + "\n";
        return result;
    }

    result.module = std::move(module);
    result.ok     = true;

    return result;
}

} // namespace

LowerResult lowerFile(std::string_view path) {
    auto tree = slang::syntax::SyntaxTree::fromFile(path);
    if (!tree) {
        LowerResult result;
        result.diagnostics =
            "error: cannot read '" + std::string(path) + "': " + std::string(tree.error().second) + "\n";
        return result;
    }

    slang::ast::Compilation comp;
    comp.addSyntaxTree(*tree);

    return lowerCompilation(comp);
}

LowerResult lowerText(std::string_view source) {
    slang::ast::Compilation comp;
    comp.addSyntaxTree(slang::syntax::SyntaxTree::fromText(source));

    return lowerCompilation(comp);
}

} // namespace clotho
