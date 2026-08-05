#include "Materialize.h"

#include <algorithm>

#include <ankerl/unordered_dense.h>
#include <optional>
#include <span>
#include <sstream>
#include <vector>

namespace clotho::legalize {

namespace {

// A flattened design's signals carry full hierarchical paths, and the render is a
// single module, so the path has to become one identifier.
//
// Stripping to the leaf does not work: hazard3's 1805 signals collapse to 777
// leaves. Mangling the whole path is not quite enough either -- 12 names collide
// outright, all of them loop variables duplicated by unrolling (`...alu.i` six
// times) -- so a colliding name takes its signal index as a suffix.
std::string mangle(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        out += (c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? c : '_';
    }
    if (!out.empty() && out[0] >= '0' && out[0] <= '9') {
        out.insert(out.begin(), '_');
    }

    return out;
}

std::vector<std::string> unique_names(const std::vector<std::string>& raw) {
    ankerl::unordered_dense::set<std::string> taken;
    std::vector<std::string>                  out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        std::string n = mangle(raw[i]);
        if (!taken.insert(n).second) {
            n += "__" + std::to_string(i);
            taken.insert(n);
        }
        out.push_back(std::move(n));
    }

    return out;
}

class Renderer {
public:
    Renderer(const ir::Module& m, const std::vector<std::string>& sig_names, const std::vector<std::string>& fn_names)
      : m_(m)
      , sig_(sig_names)
      , fn_(fn_names) {}

    // An expression, or nothing when an opcode is not covered. Failure is never
    // silent: the caller turns it into a comment naming the opcode, because
    // emitting approximate source would be worse than emitting none.
    std::optional<std::string> expr(const ir::Ir& ir, ir::Index i) {
        const ir::Inst::Payload& p = ir.payload(i);
        switch (ir.tag(i)) {
            case ir::Inst::Tag::Constant:
                return constant(ir, p.ref);
            case ir::Inst::Tag::Signal:
                return sig_[static_cast<uint32_t>(p.signal)];
            case ir::Inst::Tag::Load:
            case ir::Inst::Tag::Resize:
                // A load names its storage and a resize is a width cast; neither
                // is visible in source, and re-lowering reinserts both.
                return expr(ir, p.unary);

            // A function frame slot. Formals are locals 0..N-1, so one naming
            // rule covers arguments, the return variable and temporaries.
            case ir::Inst::Tag::Local:
                return "l" + std::to_string(p.local.index);
            case ir::Inst::Tag::Arg:
                return "l" + std::to_string(p.arg.position);

            // A call is an operator, so it renders as one. The arguments are the
            // CALLER's expressions -- nothing crosses into the callee here, which
            // is why this needs no cross-Ir operand identity.
            case ir::Inst::Tag::Call: {
                const auto  info  = ir.extra_data<ir::CallInfo>(p.extra);
                std::string out   = fn_[static_cast<uint32_t>(info.func)] + "(";
                bool        ok    = true;
                bool        first = true;
                for (const ir::Index a : ir.region(info.args)) {
                    const std::optional<std::string> e = expr(ir, a);
                    if (!e) {
                        ok = false;
                        break;
                    }
                    out += (first ? "" : ", ") + *e;
                    first = false;
                }
                if (!ok) {
                    return std::nullopt;
                }

                return out + ")";
            }

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
            // Wildcard equality. `casez` lowers to a chain of these, and without
            // them every casez-driven signal loses its arms and re-reads as a
            // latch -- which is how hazard3's predecode block diverged.
            case ir::Inst::Tag::CasezEq:
            case ir::Inst::Tag::CasexEq:
            case ir::Inst::Tag::WildEq:
                return binary("==?", ir, p.binary.l, p.binary.r);
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
            case ir::Inst::Tag::DynPartSelect: {
                const auto                       dp   = ir.extra_data<ir::DynPartSelect>(p.op_extra.extra);
                const std::optional<std::string> base = expr(ir, p.op_extra.op);
                const std::optional<std::string> off  = expr(ir, dp.offset);
                if (!base || !off) {
                    return std::nullopt;
                }

                return *base + "[" + *off + " +: " + std::to_string(dp.width) + "]";
            }
            case ir::Inst::Tag::ElemSelect: {
                const auto es = ir.extra_data<ir::ElemSelect>(p.op_extra.extra);

                return index("", ir, p.op_extra.op, es.index);
            }

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

    // A sized literal.
    //
    // Four-state constants render in BINARY with per-bit x / z, not as `'x`. A
    // casez pattern is a constant whose z bits are the wildcards; collapsing it to
    // `'x` throws the pattern away and the arm stops matching anything. Planes are
    // [value | unknown]; an unknown bit is x when its value bit is 1, z when 0.
    std::optional<std::string> constant(const ir::Ir& ir, ir::Ref value) {
        const auto&    v      = std::get<ir::Value>(ir.pool().get(value));
        const auto     all    = ir.pool().bytes(value);
        const auto     layout = ir.pool().layout(v.type);
        const auto     planes = static_cast<size_t>(layout.domain);
        const size_t   plane  = planes ? all.size() / planes : all.size();
        const uint32_t w      = layout.width;
        if (w == 0 || plane == 0) {
            return std::nullopt;
        }

        bool unknown = false;
        for (size_t k = plane; k < all.size(); ++k) {
            unknown |= all[k] != 0;
        }

        std::ostringstream os;
        const auto         bit = [&](size_t idx, size_t base) { return (all[base + idx / 8] >> (idx % 8)) & 1u; };
        if (unknown) {
            os << w << "'b";
            for (uint32_t k = w; k-- > 0;) {
                const unsigned xz = bit(k, plane);
                const unsigned vb = bit(k, 0);
                os << (xz == 0 ? (vb ? '1' : '0') : (vb ? 'x' : 'z'));
            }

            return os.str();
        }

        os << w << "'h" << std::hex;
        bool any = false;
        for (size_t k = plane; k-- > 0;) {
            const unsigned b = all[k];
            if (!any && b == 0 && k != 0) {
                continue; // trim leading zero bytes
            }
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

    const ir::Module&               m_;
    const std::vector<std::string>& sig_;
    const std::vector<std::string>& fn_;
    ir::Inst::Tag                   blocker_ = ir::Inst::Tag::Constant;
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
    // The rendered LVALUE, which is not always the bare signal: `q[i] = ...` is a
    // partial write, and rendering it as `q = ...` turns a latch into
    // combinational logic. hazard3's bit-reversal loops are exactly this shape and
    // are how the round trip caught it.
    std::string lvalue;
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
            os << it.lvalue << " " << st.assign << " " << it.value << ";\n";
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
        bool              is_switch = false;
        for (const Item& it : items) {
            if (it.path.size() <= depth || it.path[depth].pred != pred) {
                continue;
            }
            const AtomKind k = it.path[depth].kind;
            is_switch        = is_switch || k == AtomKind::SwitchArm || k == AtomKind::SwitchDefault;
            (k == AtomKind::CondTrue ? yes : no).push_back(it);
        }
        const std::optional<std::string> p = render.expr(ir, pred);
        if (!p) {
            indent(os, ind);
            os << "// " << st.sig << ": unrenderable predicate\n";
            continue;
        }

        // A case, rendered as a case. Flattening its arms into if/else chains
        // would work, but a case with a default is complete BY STRUCTURE and that
        // is what the re-analysis reads; dropping the arms (which an earlier
        // version did) turned every case-driven combinational signal into a latch.
        if (is_switch) {
            indent(os, ind);
            os << "case (" << *p << ")\n";
            for (const Item& probe : items) {
                if (probe.path.size() <= depth || probe.path[depth].pred != pred ||
                    probe.path[depth].kind != AtomKind::SwitchArm) {
                    continue;
                }
                // One arm per distinct key set, in first-seen order.
                bool already = false;
                for (const Item& earlier : items) {
                    if (&earlier == &probe) {
                        break;
                    }
                    if (earlier.path.size() > depth && earlier.path[depth].pred == pred &&
                        earlier.path[depth].kind == AtomKind::SwitchArm &&
                        earlier.path[depth].keys.start == probe.path[depth].keys.start) {
                        already = true;
                        break;
                    }
                }
                if (already) {
                    continue;
                }
                const std::span<const ir::Index> raw = ir.region(probe.path[depth].keys);
                indent(os, ind + 1);
                for (size_t k = 0; k + 1 < raw.size(); k += 2) {
                    const uint64_t     key = static_cast<uint64_t>(raw[k]) | (static_cast<uint64_t>(raw[k + 1]) << 32);
                    std::ostringstream hex;
                    hex << std::hex << key;
                    // SIZED to the selector. An unsized `'h0` re-lowers against a
                    // 32-bit selector type, so a case that was full over 2 bits
                    // stops being full and its signal reads as a latch. Width 0
                    // means the frontend declined to state one (the one-hot path),
                    // and `0'h0` is not legal source, so leave those unsized.
                    const uint32_t w = probe.path[depth].sel_width;
                    os << (k == 0 ? "" : ", ");
                    if (w != 0) {
                        os << w;
                    }
                    os << "'h" << hex.str();
                }
                os << ": begin\n";
                std::vector<Item> arm;
                for (const Item& it : items) {
                    if (it.path.size() > depth && it.path[depth].pred == pred &&
                        it.path[depth].kind == AtomKind::SwitchArm &&
                        it.path[depth].keys.start == probe.path[depth].keys.start) {
                        arm.push_back(it);
                    }
                }
                emit_tree(os, render, ir, arm, depth + 1, ind + 2, st);
                indent(os, ind + 1);
                os << "end\n";
            }
            std::vector<Item> dflt;
            for (const Item& it : items) {
                if (it.path.size() > depth && it.path[depth].pred == pred &&
                    it.path[depth].kind == AtomKind::SwitchDefault) {
                    dflt.push_back(it);
                }
            }
            // No default in the source means no default here: inventing one would
            // change an incomplete case into a complete one.
            if (!dflt.empty()) {
                indent(os, ind + 1);
                os << "default: begin\n";
                emit_tree(os, render, ir, dflt, depth + 1, ind + 2, st);
                indent(os, ind + 1);
                os << "end\n";
            }
            indent(os, ind);
            os << "endcase\n";
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

// Render a function body structurally.
//
// Unlike a process, a function needs no analysis to render: there is no guarded
// list to rebuild because the region tree IS the statement tree. Assignments land
// on frame slots, `ret` becomes `return`, and nothing here can touch a module
// signal -- FnLegality already refused any function that does.
bool emit_fn_body(std::ostringstream&        os,
                  Renderer&                  render,
                  const ir::Ir&              ir,
                  std::span<const ir::Index> body,
                  int                        ind) {
    for (const ir::Index i : body) {
        const ir::Inst::Payload& p = ir.payload(i);
        switch (ir.tag(i)) {
            case ir::Inst::Tag::Store:
            case ir::Inst::Tag::StoreNb: {
                const std::optional<std::string> lv = render.expr(ir, p.binary.l);
                const std::optional<std::string> rv = render.expr(ir, p.binary.r);
                if (!lv || !rv) {
                    return false;
                }
                indent(os, ind);
                os << *lv << " = " << *rv << ";\n";
                break;
            }
            case ir::Inst::Tag::Ret: {
                const std::optional<std::string> v = render.expr(ir, p.unary);
                if (!v) {
                    return false;
                }
                indent(os, ind);
                os << "return " << *v << ";\n";
                break;
            }
            case ir::Inst::Tag::RetVoid:
                indent(os, ind);
                os << "return;\n";
                break;
            case ir::Inst::Tag::Cond: {
                const auto                       r = ir.extra_data<ir::Regions>(p.op_extra.extra);
                const std::optional<std::string> c = render.expr(ir, p.op_extra.op);
                if (!c) {
                    return false;
                }
                indent(os, ind);
                os << "if (" << *c << ") begin\n";
                if (!emit_fn_body(os, render, ir, ir.region(r.then_), ind + 1)) {
                    return false;
                }
                indent(os, ind);
                os << "end else begin\n";
                if (!emit_fn_body(os, render, ir, ir.region(r.else_), ind + 1)) {
                    return false;
                }
                indent(os, ind);
                os << "end\n";
                break;
            }
            // A lazy select's regions feed its branch values, which are rendered
            // inline as a ternary; they need no statement of their own.
            case ir::Inst::Tag::SelectLazy:
                break;
            default:
                // Pure value instructions are rendered inline by their users. A
                // loop or a case inside a function is not covered yet, and saying
                // so beats emitting source that means something else.
                if (!ir::tag_info(ir.tag(i)).synthesizable || ir.tag(i) == ir::Inst::Tag::Loop ||
                    ir.tag(i) == ir::Inst::Tag::Switch) {
                    return false;
                }
                break;
        }
    }

    return true;
}

// A function's SystemVerilog declaration, or nothing when its body is not
// renderable. Formals and locals share one naming rule (`l<i>`), so the expression
// renderer needs no notion of which frame it is in.
std::optional<std::string> emit_fn(const ir::Module& m, Renderer& render, ir::FunctionId id, std::string_view name) {
    const ir::Function& f = m.function(id);
    if (f.is_task || f.return_type == ir::Ref::Invalid) {
        return std::nullopt; // a legal task is a no-op; there is no call to render
    }

    std::ostringstream os;
    const auto         width = [&](ir::Ref t) { return m.pool.layout(t).width; };
    os << "  function automatic logic [" << (width(f.return_type) - 1) << ":0] " << name << "(";
    for (size_t a = 0; a < f.param_types.size(); ++a) {
        os << (a == 0 ? "" : ", ") << "input logic [" << (width(f.param_types[a]) - 1) << ":0] l" << a;
    }
    os << ");\n";
    for (size_t l = f.param_types.size(); l < f.local_types.size(); ++l) {
        os << "    logic [" << (width(f.local_types[l]) - 1) << ":0] l" << l << ";\n";
    }
    if (!emit_fn_body(os, render, f.ir, f.ir.root(), 2)) {
        return std::nullopt;
    }
    os << "  endfunction\n";

    return os.str();
}

} // namespace

std::string materialize(const ir::Module& module, const Result& result, MaterializeOptions options) {
    std::ostringstream     os;
    const std::string_view name    = options.name.empty() ? std::string_view("m") : options.name;
    const auto             signals = module.signals();

    std::vector<std::string> raw_sigs;
    raw_sigs.reserve(signals.size());
    for (const ir::SignalInfo& s : signals) {
        raw_sigs.push_back(s.name);
    }
    std::vector<std::string> raw_fns;
    raw_fns.reserve(module.function_count());
    for (size_t i = 0; i < module.function_count(); ++i) {
        raw_fns.push_back(module.function(static_cast<ir::FunctionId>(i)).name);
    }
    const std::vector<std::string> sig_names = unique_names(raw_sigs);
    const std::vector<std::string> fn_names  = unique_names(raw_fns);
    Renderer                       render(module, sig_names, fn_names);

    // No ports. A flattened whole-design render has no meaningful port boundary --
    // an inner module's `input` is driven by its parent, so emitting it as a
    // top-level input would be illegal source. Classification is about process
    // structure, not connectivity, so every signal becomes an internal `logic` and
    // an original input simply has no driver.
    os << "module " << name << ";\n";
    for (size_t i = 0; i < signals.size(); ++i) {
        const uint32_t w = module.pool.layout(signals[i].type).width;
        os << "  logic ";
        if (w > 1) {
            os << "[" << (w - 1) << ":0] ";
        }
        os << sig_names[i] << ";\n";
    }

    // Every function that renders. FnLegality has already refused any that writes
    // a module signal, so a rendered one is a self-contained operator.
    for (size_t i = 0; i < module.function_count(); ++i) {
        const auto id = static_cast<ir::FunctionId>(i);
        if (const std::optional<std::string> text = emit_fn(module, render, id, fn_names[i])) {
            os << *text;
        }
    }

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
            os << sig_names[static_cast<uint32_t>(w.signal)];
            sep = true;
        }
        if (!sep) {
            os << "*";
        }
        os << ") begin\n";

        for (const SignalStores& e : pr.signals) {
            const std::string_view sig  = sig_names[static_cast<uint32_t>(e.signal)];
            const bool             nb   = !e.stores.empty() && e.stores.front().nonblocking;
            const bool             edge = e.kind == StorageKind::Flop || e.kind == StorageKind::AsyncLoad;

            // A store inside a loop happens once per iteration, and the guard does
            // not record that -- a loop adds no atom. Rendering the body once would
            // silently drop the iteration, which turns hazard3's bit-reversal and
            // shift loops into plain combinational assignments.
            bool looped = false;
            for (const GuardedStore& st : e.stores) {
                looped = looped || st.in_loop;
            }
            if (looped) {
                // Not skipped: these signals are written by more than one process,
                // so omitting them here leaves a partially-driven design that reads
                // WORSE than an approximate one. The body is emitted once, the
                // caveat is inline, and `GuardedStore::in_loop` lets a consumer
                // exclude the signal rather than trust it.
                os << "    // " << sig << ": loop body below is emitted once; iteration not rendered\n";
            }

            std::vector<Item> items;
            items.reserve(e.stores.size());
            bool ok = true;
            for (const GuardedStore& st : e.stores) {
                const std::optional<std::string> v  = render.expr(proc.ir, st.value);
                const std::optional<std::string> lv = render.expr(proc.ir, proc.ir.payload(st.store).binary.l);
                ok                                  = ok && v.has_value() && lv.has_value();
                items.push_back(
                  { guard_atoms(result.guards, st.guard), v.value_or(""), lv.value_or(std::string(sig)) });
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
