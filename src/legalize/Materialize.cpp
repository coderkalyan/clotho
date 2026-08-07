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

// SystemVerilog operator precedence, higher binds tighter. Parentheses are emitted
// only where the tree needs them, which matters for more than looks: the previous
// fully-parenthesised output was 3.46 MB for hazard3, and most of it was brackets to
// produce, store and re-parse.
enum : int {
    kLowest  = 0,
    kTernary = 2,
    kLor     = 3,
    kLand    = 4,
    kBor     = 5,
    kBxor    = 6,
    kBand    = 7,
    kEq      = 8,
    kRel     = 9,
    kShift   = 10,
    kAddSub  = 11,
    kMulDiv  = 12,
    kUnary   = 14,
    kPrimary = 15,
};

void put_uint(std::string& out, uint64_t v) {
    char   tmp[24];
    size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n != 0) {
        out += tmp[--n];
    }
}

void put_hex(std::string& out, uint64_t v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    char                  tmp[16];
    size_t                n = 0;
    do {
        tmp[n++] = kDigits[v & 0xFu];
        v >>= 4;
    } while (v != 0);
    while (n != 0) {
        out += tmp[--n];
    }
}

// Emits expressions straight into one buffer.
//
// Nothing here returns a string. The previous version handed back a
// `std::optional<std::string>` per node and formatted every constant through an
// `ostringstream`, which made the printer allocation-bound at 128 MB/s. Failure is
// a flag plus a buffer rollback instead of an optional threaded through forty call
// sites: the caller records the length, emits, and truncates if `ok()` went false.
class Renderer {
public:
    Renderer(const ir::Module& m, const std::vector<std::string>& sig_names, const std::vector<std::string>& fn_names)
      : m_(m)
      , sig_(sig_names)
      , fn_(fn_names) {}

    // Emit `i` at precedence context `prec`; parenthesise only if it binds looser.
    void emit(std::string& out, const ir::Ir& ir, ir::Index i, int prec = kLowest) {
        const ir::Inst::Payload& p = ir.payload(i);
        switch (ir.tag(i)) {
            case ir::Inst::Tag::Constant:
                constant(out, ir, p.ref);
                return;
            case ir::Inst::Tag::Signal:
                out += sig_[static_cast<uint32_t>(p.signal)];
                return;
            case ir::Inst::Tag::Load:
            case ir::Inst::Tag::Resize:
                // A load names its storage and a resize is a width cast; neither is
                // visible in source, and re-lowering reinserts both.
                emit(out, ir, p.unary, prec);
                return;

            // A function frame slot. Formals are locals 0..N-1, so one naming rule
            // covers arguments, the return variable and temporaries.
            case ir::Inst::Tag::Local:
                out += 'l';
                put_uint(out, p.local.index);
                return;
            case ir::Inst::Tag::Arg:
                out += 'l';
                put_uint(out, p.arg.position);
                return;

            // A call is an operator, so it renders as one. The arguments are the
            // CALLER's expressions -- nothing crosses into the callee here, which
            // is why this needs no cross-Ir operand identity.
            case ir::Inst::Tag::Call: {
                const auto info = ir.extra_data<ir::CallInfo>(p.extra);
                out += fn_[static_cast<uint32_t>(info.func)];
                out += '(';
                bool first = true;
                for (const ir::Index a : ir.region(info.args)) {
                    if (!first) {
                        out += ", ";
                    }
                    first = false;
                    emit(out, ir, a, kLowest);
                }
                out += ')';
                return;
            }

            case ir::Inst::Tag::Bnot:
                return unary(out, "~", ir, p.unary, prec);
            case ir::Inst::Tag::Lnot:
                return unary(out, "!", ir, p.unary, prec);
            case ir::Inst::Tag::RedAnd:
                return unary(out, "&", ir, p.unary, prec);
            case ir::Inst::Tag::RedOr:
                return unary(out, "|", ir, p.unary, prec);
            case ir::Inst::Tag::RedXor:
                return unary(out, "^", ir, p.unary, prec);

            case ir::Inst::Tag::Mul:
                return binary(out, " * ", kMulDiv, ir, p, prec);
            case ir::Inst::Tag::Div:
                return binary(out, " / ", kMulDiv, ir, p, prec);
            case ir::Inst::Tag::Mod:
                return binary(out, " % ", kMulDiv, ir, p, prec);
            case ir::Inst::Tag::Add:
                return binary(out, " + ", kAddSub, ir, p, prec);
            case ir::Inst::Tag::Sub:
                return binary(out, " - ", kAddSub, ir, p, prec);
            case ir::Inst::Tag::Sll:
                return binary(out, " << ", kShift, ir, p, prec);
            case ir::Inst::Tag::Srl:
                return binary(out, " >> ", kShift, ir, p, prec);
            case ir::Inst::Tag::Sra:
                return binary(out, " >>> ", kShift, ir, p, prec);
            case ir::Inst::Tag::Lt:
                return binary(out, " < ", kRel, ir, p, prec);
            case ir::Inst::Tag::Le:
                return binary(out, " <= ", kRel, ir, p, prec);
            case ir::Inst::Tag::Gt:
                return binary(out, " > ", kRel, ir, p, prec);
            case ir::Inst::Tag::Ge:
                return binary(out, " >= ", kRel, ir, p, prec);
            case ir::Inst::Tag::Eq:
                return binary(out, " == ", kEq, ir, p, prec);
            case ir::Inst::Tag::Ne:
                return binary(out, " != ", kEq, ir, p, prec);
            case ir::Inst::Tag::CaseEq:
                return binary(out, " === ", kEq, ir, p, prec);
            // Wildcard equality. `casez` lowers to a chain of these, and without
            // them every casez-driven signal loses its arms and re-reads as a latch.
            case ir::Inst::Tag::CasezEq:
            case ir::Inst::Tag::CasexEq:
            case ir::Inst::Tag::WildEq:
                return binary(out, " ==? ", kEq, ir, p, prec);
            case ir::Inst::Tag::Band:
                return binary(out, " & ", kBand, ir, p, prec);
            case ir::Inst::Tag::Bxor:
                return binary(out, " ^ ", kBxor, ir, p, prec);
            case ir::Inst::Tag::Bxnor:
                return binary(out, " ~^ ", kBxor, ir, p, prec);
            case ir::Inst::Tag::Bor:
                return binary(out, " | ", kBor, ir, p, prec);
            case ir::Inst::Tag::Land:
                return binary(out, " && ", kLand, ir, p, prec);
            case ir::Inst::Tag::Lor:
                return binary(out, " || ", kLor, ir, p, prec);

            case ir::Inst::Tag::BitSelect:
                emit(out, ir, p.binary.l, kPrimary);
                out += '[';
                emit(out, ir, p.binary.r, kLowest);
                out += ']';
                return;
            case ir::Inst::Tag::PartSelect: {
                const auto ps = ir.extra_data<ir::PartSelect>(p.op_extra.extra);
                emit(out, ir, p.op_extra.op, kPrimary);
                out += '[';
                put_uint(out, ps.msb);
                out += ':';
                put_uint(out, ps.lsb);
                out += ']';
                return;
            }
            case ir::Inst::Tag::DynPartSelect: {
                const auto dp = ir.extra_data<ir::DynPartSelect>(p.op_extra.extra);
                emit(out, ir, p.op_extra.op, kPrimary);
                out += '[';
                emit(out, ir, dp.offset, kLowest);
                out += " +: ";
                put_uint(out, dp.width);
                out += ']';
                return;
            }
            case ir::Inst::Tag::ElemSelect: {
                const auto es = ir.extra_data<ir::ElemSelect>(p.op_extra.extra);
                emit(out, ir, p.op_extra.op, kPrimary);
                out += '[';
                emit(out, ir, es.index, kLowest);
                out += ']';
                return;
            }

            // `a ? b : c` and the short-circuit operators share a shape: the branch
            // values of a lazy select live in regions, but the VALUES are ordinary
            // expressions.
            case ir::Inst::Tag::Select: {
                const auto sel = ir.extra_data<ir::Select>(p.op_extra.extra);
                return ternary(out, ir, p.op_extra.op, sel.a, sel.b, prec);
            }
            case ir::Inst::Tag::SelectLazy: {
                const auto sel = ir.extra_data<ir::SelectLazy>(p.op_extra.extra);
                return ternary(out, ir, p.op_extra.op, sel.then_val, sel.else_val, prec);
            }

            case ir::Inst::Tag::Concat: {
                out += '{';
                bool first = true;
                ir.for_each_operand(i, [&](ir::Index op) {
                    if (!first) {
                        out += ", ";
                    }
                    first = false;
                    emit(out, ir, op, kLowest);
                });
                out += '}';
                return;
            }
            case ir::Inst::Tag::Repl:
                out += '{';
                put_uint(out, p.repl.count);
                out += '{';
                emit(out, ir, p.repl.value, kLowest);
                out += "}}";
                return;

            default:
                blocker_ = ir.tag(i);
                ok_      = false;
                return;
        }
    }

    [[nodiscard]] bool ok() const {
        return ok_;
    }
    void clear() {
        ok_ = true;
    }
    [[nodiscard]] ir::Inst::Tag blocker() const {
        return blocker_;
    }

private:
    void unary(std::string& out, const char* op, const ir::Ir& ir, ir::Index a, int prec) {
        const bool paren = kUnary < prec;
        if (paren) {
            out += '(';
        }
        out += op;
        // One level TIGHTER than the operator itself: SystemVerilog's grammar takes
        // a primary after a unary operator, so `|!x` does not parse and a nested
        // unary must bracket. Rare enough that the extra pair costs nothing.
        emit(out, ir, a, kUnary + 1);
        if (paren) {
            out += ')';
        }
    }

    void binary(std::string& out, const char* op, int mine, const ir::Ir& ir, const ir::Inst::Payload& p, int prec) {
        const bool paren = mine < prec;
        if (paren) {
            out += '(';
        }
        emit(out, ir, p.binary.l, mine);
        out += op;
        // Right operand one tighter: these are all left-associative, so `a - b - c`
        // keeps its shape and `a - (b - c)` keeps its brackets.
        emit(out, ir, p.binary.r, mine + 1);
        if (paren) {
            out += ')';
        }
    }

    void ternary(std::string& out, const ir::Ir& ir, ir::Index c, ir::Index t, ir::Index e, int prec) {
        const bool paren = kTernary < prec;
        if (paren) {
            out += '(';
        }
        emit(out, ir, c, kTernary + 1);
        out += " ? ";
        emit(out, ir, t, kLowest);
        out += " : ";
        emit(out, ir, e, kTernary);
        if (paren) {
            out += ')';
        }
    }

    // A sized literal.
    //
    // Four-state constants render in BINARY with per-bit x / z, not as `'x`. A casez
    // pattern is a constant whose z bits are the wildcards; collapsing it throws the
    // pattern away and the arm stops matching anything. Planes are [value |
    // unknown]; an unknown bit is x when its value bit is 1, z when 0.
    void constant(std::string& out, const ir::Ir& ir, ir::Ref value) {
        const auto&    v      = std::get<ir::Value>(ir.pool().get(value));
        const auto     all    = ir.pool().bytes(value);
        const auto     layout = ir.pool().layout(v.type);
        const auto     planes = static_cast<size_t>(layout.domain);
        const size_t   plane  = planes != 0 ? all.size() / planes : all.size();
        const uint32_t w      = layout.width;
        if (w == 0 || plane == 0) {
            ok_      = false;
            blocker_ = ir::Inst::Tag::Constant;
            return;
        }

        bool unknown = false;
        for (size_t k = plane; k < all.size(); ++k) {
            unknown = unknown || all[k] != 0;
        }

        const auto bit = [&](uint32_t idx, size_t base) { return (all[base + idx / 8] >> (idx % 8)) & 1u; };
        put_uint(out, w);
        if (unknown) {
            out += "'b";
            for (uint32_t k = w; k-- > 0;) {
                const unsigned xz = bit(k, plane);
                const unsigned vb = bit(k, 0);
                out += xz == 0 ? (vb != 0 ? '1' : '0') : (vb != 0 ? 'x' : 'z');
            }
            return;
        }

        out += "'h";
        bool any = false;
        for (size_t k = plane; k-- > 0;) {
            const unsigned b = all[k];
            if (!any && b == 0 && k != 0) {
                continue; // trim leading zero bytes
            }
            if (any) {
                static constexpr char kDigits[] = "0123456789abcdef";
                out += kDigits[(b >> 4) & 0xFu];
                out += kDigits[b & 0xFu];
            } else {
                put_hex(out, b);
            }
            any = true;
        }
        if (!any) {
            out += '0';
        }
    }

    const ir::Module&               m_;
    const std::vector<std::string>& sig_;
    const std::vector<std::string>& fn_;
    ir::Inst::Tag                   blocker_ = ir::Inst::Tag::Constant;
    bool                            ok_      = true;
};

// The atoms of a guard, root-first. Stored child-to-parent, so this reverses.
// Appends `g`'s atoms to `pool`, root-first. Stored child-to-parent, so this
// reverses only the run it just added.
void guard_atoms(const std::vector<GuardNode>& arena, GuardId g, std::vector<Atom>& pool) {
    const size_t start = pool.size();
    for (GuardId k = g; k != GuardId::None;) {
        const GuardNode& n = arena[static_cast<uint32_t>(k)];
        pool.push_back(n.atom);
        k = n.parent;
    }
    std::reverse(pool.begin() + static_cast<ptrdiff_t>(start), pool.end());
}

// One store, with its guard flattened to a root-first atom path.
struct Item {
    // The guard path as (offset, length) into a flat per-signal pool, not an owned
    // vector. emit_tree partitions items into `yes` / `no` / per-arm groups at every
    // level, so an Item that owns a vector costs a heap allocation per copy per
    // level; hazard3 spent half the printer's time there. 16 bytes and trivially
    // copyable now.
    uint32_t path_off = 0;
    uint32_t path_len = 0;
    // Indices, not rendered strings: the value and the lvalue are emitted straight
    // into the output buffer at the point they appear.
    //
    // The lvalue is not always the bare signal. `q[i] = ...` is a partial write, and
    // rendering it as `q = ...` turns a latch into combinational logic; hazard3's
    // bit-reversal loops are exactly that shape and are how the round trip caught it.
    ir::Index value{};
    ir::Index lvalue{};
};

struct SigStyle {
    std::string_view sig;
    const char*      assign;
    // Edge-triggered: a missing arm can be spelled `q <= q`, which is an ordinary
    // clock enable. Level-sensitive: the same text would be a combinational loop,
    // so the hold stays a comment.
    bool edge;
};

void indent(std::string& os, int depth) {
    for (int k = 0; k < depth; ++k) {
        os += "  ";
    }
}

// Rebuild the decision tree from the guard paths and emit it as nested if/else.
//
// Flattening to a sequence of guarded ifs is what a first version did, and it is
// wrong: `if (g) q<=a;` twice loses the fact that the two arms are exhaustive, so
// every signal re-lowers as held and every reset stops being a reset. The arena is
// a prefix tree already, so the nesting is recoverable exactly.
void emit_tree(std::string&          os,
               Renderer&             render,
               const ir::Ir&         ir,
               std::span<const Atom> pool,
               std::span<const Item> items,
               size_t                depth,
               int                   ind,
               const SigStyle&       st) {
    const auto atom_at = [&](const Item& it, size_t d) -> const Atom& { return pool[it.path_off + d]; };
    // Items whose path is exhausted assign here, in order; later wins, which is
    // the same rule the source had.
    for (const Item& it : items) {
        if (it.path_len == depth) {
            indent(os, ind);
            render.emit(os, ir, it.lvalue, kPrimary);
            os += ' ';
            os += st.assign;
            os += ' ';
            render.emit(os, ir, it.value, kLowest);
            os += ";\n";
        }
    }

    // Group the rest by the predicate at this level, preserving first-seen order.
    std::vector<ir::Index> order;
    for (const Item& it : items) {
        if (it.path_len > depth) {
            const ir::Index p = atom_at(it, depth).pred;
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
            if (it.path_len <= depth || atom_at(it, depth).pred != pred) {
                continue;
            }
            const AtomKind k = atom_at(it, depth).kind;
            is_switch        = is_switch || k == AtomKind::SwitchArm || k == AtomKind::SwitchDefault;
            (k == AtomKind::CondTrue ? yes : no).push_back(it);
        }
        // Emit the predicate once into a scratch buffer: it is needed twice (the
        // `if` and, on failure, nothing at all), and rolling the main buffer back
        // is how failure is reported now.
        std::string pred_text;
        render.clear();
        render.emit(pred_text, ir, pred, kLowest);
        if (!render.ok()) {
            indent(os, ind);
            os += "// ";
            os += st.sig;
            os += ": unrenderable predicate\n";
            continue;
        }

        // A case, rendered as a case. Flattening its arms into if/else chains
        // would work, but a case with a default is complete BY STRUCTURE and that
        // is what the re-analysis reads; dropping the arms (which an earlier
        // version did) turned every case-driven combinational signal into a latch.
        if (is_switch) {
            indent(os, ind);
            os += "case (";
            os += pred_text;
            os += ")\n";
            for (const Item& probe : items) {
                if (probe.path_len <= depth || atom_at(probe, depth).pred != pred ||
                    atom_at(probe, depth).kind != AtomKind::SwitchArm) {
                    continue;
                }
                // One arm per distinct key set, in first-seen order.
                bool already = false;
                for (const Item& earlier : items) {
                    if (&earlier == &probe) {
                        break;
                    }
                    if (earlier.path_len > depth && atom_at(earlier, depth).pred == pred &&
                        atom_at(earlier, depth).kind == AtomKind::SwitchArm &&
                        atom_at(earlier, depth).keys.start == atom_at(probe, depth).keys.start) {
                        already = true;
                        break;
                    }
                }
                if (already) {
                    continue;
                }
                const std::span<const ir::Index> raw = ir.region(atom_at(probe, depth).keys);
                indent(os, ind + 1);
                for (size_t k = 0; k + 1 < raw.size(); k += 2) {
                    const uint64_t key = static_cast<uint64_t>(raw[k]) | (static_cast<uint64_t>(raw[k + 1]) << 32);
                    // SIZED to the selector. An unsized `'h0` re-lowers against a
                    // 32-bit selector type, so a case that was full over 2 bits
                    // stops being full and its signal reads as a latch. Width 0
                    // means the frontend declined to state one (the one-hot path),
                    // and `0'h0` is not legal source, so leave those unsized.
                    const uint32_t w = atom_at(probe, depth).sel_width;
                    if (k != 0) {
                        os += ", ";
                    }
                    if (w != 0) {
                        put_uint(os, w);
                    }
                    os += "'h";
                    put_hex(os, key);
                }
                os += ": begin\n";
                std::vector<Item> arm;
                for (const Item& it : items) {
                    if (it.path_len > depth && atom_at(it, depth).pred == pred &&
                        atom_at(it, depth).kind == AtomKind::SwitchArm &&
                        atom_at(it, depth).keys.start == atom_at(probe, depth).keys.start) {
                        arm.push_back(it);
                    }
                }
                emit_tree(os, render, ir, pool, arm, depth + 1, ind + 2, st);
                indent(os, ind + 1);
                os += "end\n";
            }
            std::vector<Item> dflt;
            for (const Item& it : items) {
                if (it.path_len > depth && atom_at(it, depth).pred == pred &&
                    atom_at(it, depth).kind == AtomKind::SwitchDefault) {
                    dflt.push_back(it);
                }
            }
            // No default in the source means no default here: inventing one would
            // change an incomplete case into a complete one.
            if (!dflt.empty()) {
                indent(os, ind + 1);
                os += "default: begin\n";
                emit_tree(os, render, ir, pool, dflt, depth + 1, ind + 2, st);
                indent(os, ind + 1);
                os += "end\n";
            }
            indent(os, ind);
            os += "endcase\n";
            continue;
        }

        indent(os, ind);
        os += "if (";
        os += pred_text;
        os += ") begin\n";
        emit_tree(os, render, ir, pool, yes, depth + 1, ind + 1, st);
        indent(os, ind);
        os += "end else begin\n";
        if (no.empty()) {
            // The fill-in. This is the whole point of materializing: the arm the
            // source left implicit is written out.
            indent(os, ind + 1);
            if (st.edge) {
                os += st.sig;
                os += ' ';
                os += st.assign;
                os += ' ';
                os += st.sig;
                os += "; // hold\n";
            } else {
                os += "// ";
                os += st.sig;
                os += " holds: latch\n";
            }
        } else {
            emit_tree(os, render, ir, pool, no, depth + 1, ind + 1, st);
        }
        indent(os, ind);
        os += "end\n";
    }
}

// Render a function body structurally.
//
// Unlike a process, a function needs no analysis to render: there is no guarded
// list to rebuild because the region tree IS the statement tree. Assignments land
// on frame slots, `ret` becomes `return`, and nothing here can touch a module
// signal -- FnLegality already refused any function that does.
bool emit_fn_body(std::string& os, Renderer& render, const ir::Ir& ir, std::span<const ir::Index> body, int ind) {
    for (const ir::Index i : body) {
        const ir::Inst::Payload& p = ir.payload(i);
        switch (ir.tag(i)) {
            case ir::Inst::Tag::Store:
            case ir::Inst::Tag::StoreNb: {
                indent(os, ind);
                render.emit(os, ir, p.binary.l, kPrimary);
                os += " = ";
                render.emit(os, ir, p.binary.r, kLowest);
                os += ";\n";
                if (!render.ok()) {
                    return false;
                }
                break;
            }
            case ir::Inst::Tag::Ret: {
                indent(os, ind);
                os += "return ";
                render.emit(os, ir, p.unary, kLowest);
                os += ";\n";
                if (!render.ok()) {
                    return false;
                }
                break;
            }
            case ir::Inst::Tag::RetVoid:
                indent(os, ind);
                os += "return;\n";
                break;
            case ir::Inst::Tag::Cond: {
                const auto r = ir.extra_data<ir::Regions>(p.op_extra.extra);
                indent(os, ind);
                os += "if (";
                render.emit(os, ir, p.op_extra.op, kLowest);
                os += ") begin\n";
                if (!render.ok()) {
                    return false;
                }
                if (!emit_fn_body(os, render, ir, ir.region(r.then_), ind + 1)) {
                    return false;
                }
                indent(os, ind);
                os += "end else begin\n";
                if (!emit_fn_body(os, render, ir, ir.region(r.else_), ind + 1)) {
                    return false;
                }
                indent(os, ind);
                os += "end\n";
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

    std::string os;
    const auto  width = [&](ir::Ref t) { return m.pool.layout(t).width; };
    os += "  function automatic logic [";
    put_uint(os, width(f.return_type) - 1);
    os += ":0] ";
    os += name;
    os += '(';
    for (size_t a = 0; a < f.param_types.size(); ++a) {
        if (a != 0) {
            os += ", ";
        }
        os += "input logic [";
        put_uint(os, width(f.param_types[a]) - 1);
        os += ":0] l";
        put_uint(os, a);
    }
    os += ");\n";
    for (size_t l = f.param_types.size(); l < f.local_types.size(); ++l) {
        os += "    logic [";
        put_uint(os, width(f.local_types[l]) - 1);
        os += ":0] l";
        put_uint(os, l);
        os += ";\n";
    }
    if (!emit_fn_body(os, render, f.ir, f.ir.root(), 2)) {
        return std::nullopt;
    }
    os += "  endfunction\n";

    return os;
}

} // namespace

std::string materialize(const ir::Module& module, const Result& result, MaterializeOptions options) {
    std::string os;
    // Grown by doubling, a 3.4 MB result copies several megabytes of text for
    // nothing. The estimate only has to be the right order: signals dominate the
    // declarations and processes dominate the body.
    os.reserve(4096 + 64 * module.signals().size() + 512 * module.processes().size());
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
    std::vector<Atom>              pool; // flat guard paths, reused per signal

    // No ports. A flattened whole-design render has no meaningful port boundary --
    // an inner module's `input` is driven by its parent, so emitting it as a
    // top-level input would be illegal source. Classification is about process
    // structure, not connectivity, so every signal becomes an internal `logic` and
    // an original input simply has no driver.
    os += "module ";
    os += name;
    os += ";\n";
    for (size_t i = 0; i < signals.size(); ++i) {
        const uint32_t w = module.pool.layout(signals[i].type).width;
        os += "  logic ";
        if (w > 1) {
            os += '[';
            put_uint(os, w - 1);
            os += ":0] ";
        }
        os += sig_names[i];
        os += ";\n";
    }

    // Every function that renders. FnLegality has already refused any that writes
    // a module signal, so a rendered one is a self-contained operator.
    for (size_t i = 0; i < module.function_count(); ++i) {
        const auto id = static_cast<ir::FunctionId>(i);
        if (const std::optional<std::string> text = emit_fn(module, render, id, fn_names[i])) {
            os += *text;
        }
    }

    for (size_t k = 0; k < result.processes.size(); ++k) {
        const ProcessResult& pr   = result.processes[k];
        const ir::Process&   proc = module.processes()[k];
        if (!pr.synthesizable) {
            os += "  // process ";
            put_uint(os, k);
            os += ": not synthesizable\n";
            continue;
        }
        if (pr.signals.empty()) {
            continue;
        }

        // The sensitivity list is the process's own; materializing does not
        // change WHEN a block runs, only what it says while running.
        os += "  always @(";
        bool sep = false;
        for (const ir::WaitEvent& w : proc.wait_events) {
            if (sep) {
                os += " or ";
            }
            switch (w.edge) {
                case ir::Edge::Positive:
                    os += "posedge ";
                    break;
                case ir::Edge::Negative:
                    os += "negedge ";
                    break;
                case ir::Edge::Any:
                    break;
            }
            os += sig_names[static_cast<uint32_t>(w.signal)];
            sep = true;
        }
        if (!sep) {
            os += '*';
        }
        os += ") begin\n";

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
                os += "    // ";
                os += sig;
                os += ": loop body below is emitted once; iteration not rendered\n";
            }

            std::vector<Item> items;
            items.reserve(e.stores.size());
            pool.clear();
            for (const GuardedStore& st : e.stores) {
                const auto off = static_cast<uint32_t>(pool.size());
                guard_atoms(result.guards, st.guard, pool);
                items.push_back(
                  { off, static_cast<uint32_t>(pool.size() - off), st.value, proc.ir.payload(st.store).binary.l });
            }
            const size_t mark = os.size();
            render.clear();
            emit_tree(os, render, proc.ir, pool, items, 0, 2, { sig, nb ? "<=" : "=", edge });
            if (!render.ok()) {
                // Roll the buffer back and say so, rather than leave half a
                // statement behind. This is the whole failure path now: no optional
                // threaded through forty call sites, just a length and a truncate.
                os.resize(mark);
                os += "    // ";
                os += sig;
                os += ": unrenderable operand (opcode ";
                put_uint(os, static_cast<uint32_t>(render.blocker()));
                os += ")\n";
                continue;
            }
        }
        os += "  end\n";
    }
    os += "endmodule\n";

    return os;
}

} // namespace clotho::legalize
