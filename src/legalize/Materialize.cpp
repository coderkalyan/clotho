#include "Materialize.h"

#include <algorithm>
#include <optional>
#include <span>
#include <sstream>
#include <vector>

namespace clotho::legalize {

namespace {

// The last component of a flattened hierarchical name. Module signals carry
// dotted paths; a rendered single module wants the leaf.
std::string_view leaf(std::string_view name) {
    const size_t dot = name.rfind('.');

    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

class Renderer {
public:
    explicit Renderer(const ir::Module& m)
      : m_(m) {}

    // An expression, or nothing when an opcode is not covered. Failure is never
    // silent: the caller turns it into a comment naming the opcode, because
    // emitting approximate source would be worse than emitting none.
    std::optional<std::string> expr(const ir::Ir& ir, ir::Index i) {
        const ir::Inst::Payload& p = ir.payload(i);
        switch (ir.tag(i)) {
            case ir::Inst::Tag::Constant:
                return constant(ir, p.ref);
            case ir::Inst::Tag::Signal:
                return std::string(leaf(m_.signal(p.signal).name));
            case ir::Inst::Tag::Load:
            case ir::Inst::Tag::Resize:
                // A load names its storage and a resize is a width cast; neither
                // is visible in source, and re-lowering reinserts both.
                return expr(ir, p.unary);

            case ir::Inst::Tag::Bnot:
                return unary("~", ir, p.unary);
            case ir::Inst::Tag::Lnot:
                return unary("!", ir, p.unary);
            case ir::Inst::Tag::RedAnd:
                return unary("&", ir, p.unary);
            case ir::Inst::Tag::RedOr:
                return unary("|", ir, p.unary);
            case ir::Inst::Tag::RedXor:
                return unary("^", ir, p.unary);

            case ir::Inst::Tag::Add:
                return binary("+", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Sub:
                return binary("-", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Mul:
                return binary("*", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Div:
                return binary("/", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Mod:
                return binary("%", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Band:
                return binary("&", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Bor:
                return binary("|", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Bxor:
                return binary("^", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Bxnor:
                return binary("~^", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Sll:
                return binary("<<", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Srl:
                return binary(">>", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Sra:
                return binary(">>>", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Eq:
                return binary("==", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Ne:
                return binary("!=", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Lt:
                return binary("<", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Le:
                return binary("<=", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Gt:
                return binary(">", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Ge:
                return binary(">=", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::CaseEq:
                return binary("===", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Land:
                return binary("&&", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::Lor:
                return binary("||", ir, p.binary.l, p.binary.r);

            case ir::Inst::Tag::BitSelect:
                return index("", ir, p.binary.l, p.binary.r);
            case ir::Inst::Tag::PartSelect: {
                const auto                 ps   = ir.extra_data<ir::PartSelect>(p.op_extra.extra);
                std::optional<std::string> base = expr(ir, p.op_extra.op);
                if (!base) {
                    return std::nullopt;
                }

                return *base + "[" + std::to_string(ps.msb) + ":" + std::to_string(ps.lsb) + "]";
            }

            // `a ? b : c` and the short-circuit operators share a shape: the
            // branch values of a lazy select live in regions, but the VALUES are
            // ordinary expressions, and any instruction in the region that
            // matters is reachable from them.
            case ir::Inst::Tag::Select: {
                const auto sel = ir.extra_data<ir::Select>(p.op_extra.extra);

                return ternary(ir, p.op_extra.op, sel.a, sel.b);
            }
            case ir::Inst::Tag::SelectLazy: {
                const auto sel = ir.extra_data<ir::SelectLazy>(p.op_extra.extra);

                return ternary(ir, p.op_extra.op, sel.then_val, sel.else_val);
            }

            case ir::Inst::Tag::Concat: {
                std::string out   = "{";
                bool        ok    = true;
                bool        first = true;
                ir.for_each_operand(i, [&](ir::Index op) {
                    if (!ok) {
                        return;
                    }
                    const std::optional<std::string> e = expr(ir, op);
                    if (!e) {
                        ok = false;
                        return;
                    }
                    if (!first) {
                        out += ", ";
                    }
                    first = false;
                    out += *e;
                });
                if (!ok) {
                    return std::nullopt;
                }

                return out + "}";
            }
            case ir::Inst::Tag::Repl: {
                const std::optional<std::string> v = expr(ir, p.repl.value);
                if (!v) {
                    return std::nullopt;
                }

                return "{" + std::to_string(p.repl.count) + "{" + *v + "}}";
            }

            default:
                blocker_ = ir.tag(i);

                return std::nullopt;
        }
    }

    [[nodiscard]] ir::Inst::Tag blocker() const {
        return blocker_;
    }

private:
    std::optional<std::string> unary(const char* op, const ir::Ir& ir, ir::Index a) {
        const std::optional<std::string> e = expr(ir, a);

        // Parenthesized like the binaries: `|!x` does not parse, and precedence is
        // the IR's shape rather than the renderer's to re-derive.
        return e ? std::optional<std::string>("(" + std::string(op) + *e + ")") : std::nullopt;
    }

    std::optional<std::string> binary(const char* op, const ir::Ir& ir, ir::Index l, ir::Index r) {
        const std::optional<std::string> a = expr(ir, l);
        const std::optional<std::string> b = expr(ir, r);
        if (!a || !b) {
            return std::nullopt;
        }

        // Fully parenthesized: precedence is the IR's shape, not the renderer's
        // to re-derive, and a re-lowering must rebuild the same tree.
        return "(" + *a + " " + op + " " + *b + ")";
    }

    std::optional<std::string> index(const char* op, const ir::Ir& ir, ir::Index l, ir::Index r) {
        const std::optional<std::string> a = expr(ir, l);
        const std::optional<std::string> b = expr(ir, r);
        if (!a || !b) {
            return std::nullopt;
        }

        return *a + op + "[" + *b + "]";
    }

    std::optional<std::string> ternary(const ir::Ir& ir, ir::Index c, ir::Index t, ir::Index e) {
        const std::optional<std::string> pc = expr(ir, c);
        const std::optional<std::string> pt = expr(ir, t);
        const std::optional<std::string> pe = expr(ir, e);
        if (!pc || !pt || !pe) {
            return std::nullopt;
        }

        return "(" + *pc + " ? " + *pt + " : " + *pe + ")";
    }

    // A sized literal. X/Z bits render as 'x rather than a value, which is what
    // the reset corpus's `q <= 'x` case needs and is never a silent lie.
    std::optional<std::string> constant(const ir::Ir& ir, ir::Ref value) {
        const auto&  v      = std::get<ir::Value>(ir.pool().get(value));
        const auto   all    = ir.pool().bytes(value);
        const auto   layout = ir.pool().layout(v.type);
        const auto   planes = static_cast<size_t>(layout.domain);
        const size_t plane  = planes ? all.size() / planes : all.size();

        for (size_t k = plane; k < all.size(); ++k) {
            if (all[k] != 0) {
                return "'x"; // any unknown bit: do not invent a value
            }
        }

        std::ostringstream os;
        os << layout.width << "'h";
        bool any = false;
        for (size_t k = plane; k-- > 0;) {
            const unsigned b = all[k];
            if (!any && b == 0 && k != 0) {
                continue; // trim leading zero bytes
            }
            os << std::hex;
            if (any) {
                os.width(2);
                os.fill('0');
            }
            os << b;
            any = true;
        }
        if (!any) {
            os << '0';
        }

        return os.str();
    }

    const ir::Module& m_;
    ir::Inst::Tag     blocker_ = ir::Inst::Tag::Constant;
};

// The atoms of a guard, root-first. Stored child-to-parent, so this reverses.
std::vector<Atom> guard_atoms(const std::vector<GuardNode>& arena, GuardId g) {
    std::vector<Atom> out;
    for (GuardId k = g; k != GuardId::None;) {
        const GuardNode& n = arena[static_cast<uint32_t>(k)];
        out.push_back(n.atom);
        k = n.parent;
    }
    std::reverse(out.begin(), out.end());

    return out;
}

// One store, with its guard flattened to a root-first atom path.
struct Item {
    std::vector<Atom> path;
    std::string       value;
};

struct SigStyle {
    std::string_view sig;
    const char*      assign;
    // Edge-triggered: a missing arm can be spelled `q <= q`, which is an ordinary
    // clock enable. Level-sensitive: the same text would be a combinational loop,
    // so the hold stays a comment.
    bool edge;
};

void indent(std::ostringstream& os, int depth) {
    for (int k = 0; k < depth; ++k) {
        os << "  ";
    }
}

// Rebuild the decision tree from the guard paths and emit it as nested if/else.
//
// Flattening to a sequence of guarded ifs is what a first version did, and it is
// wrong: `if (g) q<=a;` twice loses the fact that the two arms are exhaustive, so
// every signal re-lowers as held and every reset stops being a reset. The arena is
// a prefix tree already, so the nesting is recoverable exactly.
void emit_tree(std::ostringstream&   os,
               Renderer&             render,
               const ir::Ir&         ir,
               std::span<const Item> items,
               size_t                depth,
               int                   ind,
               const SigStyle&       st) {
    // Items whose path is exhausted assign here, in order; later wins, which is
    // the same rule the source had.
    for (const Item& it : items) {
        if (it.path.size() == depth) {
            indent(os, ind);
            os << st.sig << " " << st.assign << " " << it.value << ";\n";
        }
    }

    // Group the rest by the predicate at this level, preserving first-seen order.
    std::vector<ir::Index> order;
    for (const Item& it : items) {
        if (it.path.size() > depth) {
            const ir::Index p = it.path[depth].pred;
            if (std::find(order.begin(), order.end(), p) == order.end()) {
                order.push_back(p);
            }
        }
    }

    for (const ir::Index pred : order) {
        std::vector<Item> yes;
        std::vector<Item> no;
        for (const Item& it : items) {
            if (it.path.size() <= depth || it.path[depth].pred != pred) {
                continue;
            }
            (it.path[depth].kind == AtomKind::CondTrue ? yes : no).push_back(it);
        }
        const std::optional<std::string> p = render.expr(ir, pred);
        if (!p) {
            indent(os, ind);
            os << "// " << st.sig << ": unrenderable predicate\n";
            continue;
        }

        indent(os, ind);
        os << "if (" << *p << ") begin\n";
        emit_tree(os, render, ir, yes, depth + 1, ind + 1, st);
        indent(os, ind);
        os << "end else begin\n";
        if (no.empty()) {
            // The fill-in. This is the whole point of materializing: the arm the
            // source left implicit is written out.
            indent(os, ind + 1);
            if (st.edge) {
                os << st.sig << " " << st.assign << " " << st.sig << "; // hold\n";
            } else {
                os << "// " << st.sig << " holds: latch\n";
            }
        } else {
            emit_tree(os, render, ir, no, depth + 1, ind + 1, st);
        }
        indent(os, ind);
        os << "end\n";
    }
}

} // namespace

std::string materialize(const ir::Module& module, const Result& result, MaterializeOptions options) {
    std::ostringstream     os;
    const std::string_view name = options.name.empty() ? std::string_view("m") : options.name;

    // Ports first, in declaration order, so the rendered module is a drop-in for
    // the original -- which is what makes the round-trip test possible.
    os << "module " << name << "(";
    bool       first   = true;
    const auto signals = module.signals();
    for (const ir::SignalInfo& s : signals) {
        if (s.dir == ir::SignalInfo::Direction::Internal) {
            continue;
        }
        const char*    dir = s.dir == ir::SignalInfo::Direction::Input    ? "input"
                             : s.dir == ir::SignalInfo::Direction::Output ? "output"
                                                                          : "inout";
        const uint32_t w   = module.pool.layout(s.type).width;
        os << (first ? "" : ", ") << dir << " logic ";
        if (w > 1) {
            os << "[" << (w - 1) << ":0] ";
        }
        os << leaf(s.name);
        first = false;
    }
    os << ");\n";

    for (const ir::SignalInfo& s : signals) {
        if (s.dir != ir::SignalInfo::Direction::Internal) {
            continue;
        }
        const uint32_t w = module.pool.layout(s.type).width;
        os << "  logic ";
        if (w > 1) {
            os << "[" << (w - 1) << ":0] ";
        }
        os << leaf(s.name) << ";\n";
    }

    Renderer render(module);
    for (size_t k = 0; k < result.processes.size(); ++k) {
        const ProcessResult& pr   = result.processes[k];
        const ir::Process&   proc = module.processes()[k];
        if (!pr.synthesizable) {
            os << "  // process " << k << ": not synthesizable\n";
            continue;
        }
        if (pr.signals.empty()) {
            continue;
        }

        // The sensitivity list is the process's own; materializing does not
        // change WHEN a block runs, only what it says while running.
        os << "  always @(";
        bool sep = false;
        for (const ir::WaitEvent& w : proc.wait_events) {
            os << (sep ? " or " : "");
            switch (w.edge) {
                case ir::Edge::Positive:
                    os << "posedge ";
                    break;
                case ir::Edge::Negative:
                    os << "negedge ";
                    break;
                case ir::Edge::Any:
                    break;
            }
            os << leaf(module.signal(w.signal).name);
            sep = true;
        }
        if (!sep) {
            os << "*";
        }
        os << ") begin\n";

        for (const SignalStores& e : pr.signals) {
            const std::string_view sig  = leaf(module.signal(e.signal).name);
            const bool             nb   = !e.stores.empty() && e.stores.front().nonblocking;
            const bool             edge = e.kind == StorageKind::Flop || e.kind == StorageKind::AsyncLoad;

            std::vector<Item> items;
            items.reserve(e.stores.size());
            bool ok = true;
            for (const GuardedStore& st : e.stores) {
                const std::optional<std::string> v = render.expr(proc.ir, st.value);
                ok                                 = ok && v.has_value();
                items.push_back({ guard_atoms(result.guards, st.guard), v.value_or("") });
            }
            if (!ok) {
                os << "    // " << sig << ": unrenderable operand (opcode " << static_cast<uint32_t>(render.blocker())
                   << ")\n";
                continue;
            }
            emit_tree(os, render, proc.ir, items, 0, 2, { sig, nb ? "<=" : "=", edge });
        }
        os << "  end\n";
    }
    os << "endmodule\n";

    return os.str();
}

} // namespace clotho::legalize
