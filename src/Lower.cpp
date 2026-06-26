#include "Lower.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <slang/ast/ASTVisitor.h>
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
#include <slang/ast/symbols/SubroutineSymbols.h>
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

// Infers the @* sensitivity of a combinational process body (an `always_comb`,
// `always_latch`, or `always @*`): the signals it reads to compute a result,
// minus the ones it writes. A signal that is both read and written is an
// internal intermediate and is left out; index/part-select expressions on a
// write target are themselves reads and are kept (11.6, 9.2.2.2).
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
    explicit Lowerer(ir::Module& module)
      : module_(module) {}

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
        prefix_                        = self_prefix;
        collect_signals(body);

        // prefix_ holds self_prefix on entry to each iteration: it is set above and
        // restored after the Instance case (the only one that descends and changes
        // it), so signal-creating process lowering always sees this level's prefix.
        for (const Symbol& m : body.members()) {
            switch (m.kind) {
                case SymbolKind::ContinuousAssign:
                    lower_continuous_assign(m.as<ContinuousAssignSymbol>());
                    break;
                case SymbolKind::ProceduralBlock:
                    lower_procedural_block(m.as<ProceduralBlockSymbol>());
                    break;
                case SymbolKind::Instance: {
                    const InstanceSymbol& child = m.as<InstanceSymbol>();
                    // Recurse first (collects child signals), then wire ports: a
                    // connection's parent-side expression resolves against signals
                    // already registered in this (parent) body. Recursion leaves
                    // prefix_ at the child level, so restore it before wiring.
                    lower_instance(child, self_prefix + std::string(child.name) + ".");
                    prefix_ = self_prefix;
                    wire_ports(child);
                    break;
                }
                case SymbolKind::InstanceArray:
                    // TODO: instance arrays (foo u[N](...)) — needs per-element
                    // elaboration and bit-sliced port connections.
                    fail("instance arrays are not supported");
                    break;
                default:
                    break;
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
            info.name        = prefix_ + std::string(vs.name);
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
        info.name = prefix_ + std::string(vs.name);
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

        // A continuous assign is a process sensitive to the source elements of
        // its expression (LRM 4.9.1): every signal the RHS reads, plus any read
        // in an LHS selector (`assign mem[i] = x` is sensitive to i). Unlike an
        // always_comb, the written net is NOT subtracted: a net that is both
        // driven and read is real combinational feedback (`assign y = a & y`)
        // and must stay sensitive. An empty list (constant RHS) yields a
        // run-once process, which is the required time-zero evaluation.
        std::vector<ir::WaitEvent> wait_events;
        SensitivityCollector       c(sigmap_);
        e.visit(c);
        for (const ir::Signal s : c.reads) {
            wait_events.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
        }

        module_.add_process(ir::Process{ .activation  = ir::Activation::Always,
                                         .wait_events = std::move(wait_events),
                                         .loc         = to_source_loc(sym.location),
                                         .ir          = b.finish() });
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
                // TODO: unconnected port — should default to its initializer / z.
                fail("unconnected module port is not supported");
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
                const AssignmentExpression& a   = expr->as<AssignmentExpression>();
                const ir::Ref               ty  = lower_type(*a.left().type);
                const ir::Index             val = b.unary(ir::Inst::Tag::Load, b.signal(child_sig, l), l);
                b.binary(ir::Inst::Tag::Store, lower_lvalue(a.left(), b), b.resize(val, ty, l), l);
                sens.push_back(ir::WaitEvent{ .signal = child_sig, .edge = ir::Edge::Any });
            } else {
                // Input: drive the child net from the source rvalue, resized to the
                // port width; wakes on the source's reads.
                const ir::Ref ty = lower_type(p.internalSymbol->as<ValueSymbol>().getType());
                b.binary(ir::Inst::Tag::Store, b.signal(child_sig, l), b.resize(lower_expr(*expr, b), ty, l), l);
                collect_expr_reads(*expr, sens);
            }
            module_.add_process(ir::Process{ .activation  = ir::Activation::Always,
                                             .wait_events = std::move(sens),
                                             .loc         = l,
                                             .ir          = b.finish() });
        }
    }

    // Append the sensitivity of an rvalue expression (each signal it reads, as an
    // any-change wait) to `out`. Mirrors collect_inferred_sensitivity for an
    // expression with no writes.
    void collect_expr_reads(const Expression& expr, std::vector<ir::WaitEvent>& out) {
        SensitivityCollector c(sigmap_);
        expr.visit(c);
        for (const ir::Signal s : c.reads) {
            if (!c.written.contains(static_cast<uint32_t>(s))) {
                out.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
            }
        }
    }

    void lower_procedural_block(const ProceduralBlockSymbol& sym) {
        ir::Activation             activation = ir::Activation::Always;
        std::vector<ir::WaitEvent> wait_events;

        if (sym.procedureKind == ProceduralBlockKind::Initial) {
            activation = ir::Activation::Initial;
        }

        const Statement* body = &sym.getBody();

        // `always_comb` / `always_latch` have no written sensitivity at all; their
        // event list is inferred from the body's reads (9.2.2.2). `always @*`
        // carries an ImplicitEvent timing wrapping the body, inferred the same way.
        bool infer = sym.procedureKind == ProceduralBlockKind::AlwaysComb ||
                     sym.procedureKind == ProceduralBlockKind::AlwaysLatch;

        // A top-level event control is the process's sensitivity: collect its wait
        // events here and lower the statement underneath. A non-event top-level
        // timing (e.g. `always #5 ...`) is NOT sensitivity — it falls through to
        // lower_stmt, whose Timed case rejects it loudly.
        if (body->kind == StatementKind::Timed) {
            const TimedStatement& timed = body->as<TimedStatement>();
            if (timed.timing.kind == TimingControlKind::ImplicitEvent) {
                infer = true;
                body  = &timed.stmt;
            } else if (is_event_control(timed.timing)) {
                collect_wait_events(timed.timing, wait_events);
                body = &timed.stmt;
            }
        }

        ir::Builder b(module_.pool);
        lower_stmt(*body, b);
        // Inferred sensitivity is read from the body after lowering, so any
        // block-local declarations it touched are already registered in sigmap_.
        if (infer) {
            collect_inferred_sensitivity(*body, wait_events);
        }
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

    // Build the inferred (@*) sensitivity of a combinational process body and
    // append it to `out`: every signal read to compute a result, minus those the
    // body writes, each entered as a level-sensitive (any-change) wait.
    void collect_inferred_sensitivity(const Statement& body, std::vector<ir::WaitEvent>& out) {
        SensitivityCollector c(sigmap_);
        body.visit(c);
        for (const ir::Signal s : c.reads) {
            if (!c.written.contains(static_cast<uint32_t>(s))) {
                out.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
            }
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
            case StatementKind::Return:
                lower_return(stmt.as<ReturnStatement>(), b);
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
            // A user function invoked as a statement: void, or a non-void function
            // whose result is discarded. Lower the call for its side effects (a
            // function may write module signals). A task is rejected in lower_user_call.
            lower_user_call(call, b);
            return;
        }
        const std::string_view name = call.getSubroutineName();
        if (name == "$display" || name == "$write") {
            lower_display(call, name == "$display", b);
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
        if (a.isNonBlocking() && fn_ctx_ != nullptr) {
            // A function consumes no time, so a nonblocking assignment in one has no
            // deferred region to commit into. slang permits it in limited cases; we
            // do not model it.
            fail("nonblocking assignment in a function is not supported");
        }
        const ir::Index lhs = lower_lvalue(a.left(), b);
        const ir::Index rhs = lower_expr(a.right(), b);
        // StoreNb gets correct two-phase NBA semantics in the backend: the RHS is
        // sampled here in the active region and committed in the NBA update region.
        // Intra-assignment timing (`<= #5`) is still rejected above, so the deferred
        // update always lands in the current instant's NBA region.
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
            case ExpressionKind::NamedValue:
                return b.unary(ir::Inst::Tag::Load, value_lvalue(expr.as<NamedValueExpression>().symbol, b, l), l);
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
            case ExpressionKind::Call: {
                const CallExpression& call = expr.as<CallExpression>();
                if (call.isSystemCall()) {
                    // System functions ($signed, $bits, $time, ...) need bespoke
                    // lowering; none is implemented yet.
                    fail("system function '" + std::string(call.getSubroutineName()) + "' is not supported");
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
                return value_lvalue(expr.as<NamedValueExpression>().symbol, b, l);
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
                fail("unsupported assignment target kind=" + std::string(slang::ast::toString(expr.kind)));
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
        const ir::Signal sig = module_.add_signal(std::move(info));
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
            b.unary(ir::Inst::Tag::Ret, b.resize(lower_expr(*r.expr, b), fn_ctx_->ret_type, l), l);
        } else if (fn_ctx_->has_ret_local) {
            // `return;` in a non-void function yields the current function-var value.
            const ir::Index rv = b.local(fn_ctx_->ret_local, fn_ctx_->ret_type, l);
            b.unary(ir::Inst::Tag::Ret, b.unary(ir::Inst::Tag::Load, rv, l), l);
        } else {
            b.unary(ir::Inst::Tag::Ret, void_ret_operand(b, l), l);
        }
    }

    // Resolve a subroutine to a FunctionId, lowering its body on first reference.
    // The id is reserved before the body is built so a recursive call resolves it.
    ir::FunctionId function_id(const SubroutineSymbol& sub) {
        const auto cached = fnmap_.find(&sub);
        if (cached != fnmap_.end()) {
            return cached->second;
        }
        if (sub.subroutineKind != SubroutineKind::Function) {
            // TODO: tasks — they can consume time and need the scheduler.
            fail("task enable is not supported (functions only)");
        }
        if (sub.flags.has(MethodFlags::DPIImport)) {
            // TODO: DPI imports — call out to native code via the runtime.
            fail("DPI imported functions are not supported");
        }
        for (const FormalArgumentSymbol* fa : sub.getArguments()) {
            if (fa->direction != slang::ast::ArgumentDirection::In) {
                // TODO: output / inout / ref arguments — copy-out / aliasing.
                fail("functions with output/inout/ref arguments are not supported");
            }
        }

        const ir::FunctionId id = module_.reserve_function();
        fnmap_[&sub]            = id;
        build_function(sub, id);

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
        const ir::FunctionId    id  = function_id(sub); // validates + lowers (lazily)

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
            args.push_back(b.resize(lower_expr(*actuals[i], b), pty, l));
        }
        const ir::Index c = b.call(id, args, l);

        return sub.returnValVar == nullptr ? std::nullopt : std::optional<ir::Index>(c);
    }

    ir::Module& module_;
    // Maps an elaborated value symbol to its module Signal handle.
    ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal> sigmap_;
    // Hierarchical name prefix of the instance currently being lowered (e.g.
    // "u_alu.u_sub."); empty for the top. Prepended to every signal name created
    // while this instance's body is processed so flattened signals stay unique.
    std::string prefix_;
    // Subroutine -> its lowered FunctionId, so each function is lowered once and
    // shared across call sites (and a recursive call resolves its own id).
    ankerl::unordered_dense::map<const SubroutineSymbol*, ir::FunctionId> fnmap_;
    // The function currently being lowered, or null when lowering a process. Drives
    // local-vs-signal name resolution and `return` handling.
    FnCtx* fn_ctx_ = nullptr;
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
    if (root.topInstances.size() > 1) {
        // TODO: multiple roots — elaborate each into the one flat module (their
        // signals share the state buffer but never connect).
        result.diagnostics += "error: multiple top-level modules are not supported\n";
        return result;
    }

    // Analysis: walk the elaborated AST into clotho IR.
    const auto t_an   = std::chrono::steady_clock::now();
    auto       module = std::make_unique<ir::Module>();
    try {
        Lowerer(*module).lower_instance(*root.topInstances[0], "");
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
