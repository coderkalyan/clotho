#include "Materialize.h"

#include <algorithm>
#include <cctype>

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
    Renderer(const std::vector<std::string>& sig_names,
             const std::vector<std::string>& fn_names,
             Target                          target)
      : sig_(sig_names)
      , fn_(fn_names)
      , target_(target) {}

    [[nodiscard]] Target target() const {
        return target_;
    }

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
            case ir::Inst::Tag::UDiv:
            case ir::Inst::Tag::SDiv:
                return binary(out, " / ", kMulDiv, ir, p, prec);
            case ir::Inst::Tag::UMod:
            case ir::Inst::Tag::SMod:
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
            case ir::Inst::Tag::ULt:
            case ir::Inst::Tag::SLt:
                return binary(out, " < ", kRel, ir, p, prec);
            case ir::Inst::Tag::ULe:
            case ir::Inst::Tag::SLe:
                return binary(out, " <= ", kRel, ir, p, prec);
            case ir::Inst::Tag::UGt:
            case ir::Inst::Tag::SGt:
                return binary(out, " > ", kRel, ir, p, prec);
            case ir::Inst::Tag::UGe:
            case ir::Inst::Tag::SGe:
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
                if (target_ == Target::V2005) {
                    return wild_2005(out, ir, p, prec);
                }
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
    // Record a decline made by the statement printer rather than the expression
    // printer, so the emitted comment names something.
    void emit_task_name(std::string& out, ir::FunctionId f) {
        out += fn_[static_cast<uint32_t>(f)];
    }
    void decline(ir::Inst::Tag t) {
        blocker_ = t;
        ok_      = false;
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

    // Wildcard equality for Verilog-2005, which has no `==?`.
    //
    // A casez pattern is a constant whose z bits ARE the mask, so `a ==? pat` is
    // exactly `(a & known) === (pat & known)`. Both literals are emitted directly.
    // A non-constant right side has no mask to extract and is declined rather than
    // approximated.
    void wild_2005(std::string& out, const ir::Ir& ir, const ir::Inst::Payload& p, int prec) {
        ir::Index rhs = p.binary.r;
        while (ir.tag(rhs) == ir::Inst::Tag::Resize) {
            rhs = ir.payload(rhs).unary;
        }
        if (ir.tag(rhs) != ir::Inst::Tag::Constant) {
            decline(ir::Inst::Tag::CasezEq);
            return;
        }
        const auto&    v      = std::get<ir::Value>(ir.pool().get(ir.payload(rhs).ref));
        const auto     all    = ir.pool().bytes(ir.payload(rhs).ref);
        const auto     layout = ir.pool().layout(v.type);
        const auto     planes = static_cast<size_t>(layout.domain);
        const size_t   plane  = planes != 0 ? all.size() / planes : all.size();
        const uint32_t w      = layout.width;
        if (w == 0 || w > 64 || plane == 0) {
            decline(ir::Inst::Tag::CasezEq);
            return;
        }
        uint64_t value = 0;
        uint64_t known = 0;
        for (uint32_t k = 0; k < w; ++k) {
            const uint64_t bit = uint64_t{ 1 } << k;
            const bool     xz  = planes > 1 && ((all[plane + k / 8] >> (k % 8)) & 1u) != 0;
            if (!xz) {
                known |= bit;
                if (((all[k / 8] >> (k % 8)) & 1u) != 0) {
                    value |= bit;
                }
            }
        }

        const bool paren = kEq < prec;
        if (paren) {
            out += '(';
        }
        out += '(';
        emit(out, ir, p.binary.l, kBand);
        out += " & ";
        put_uint(out, w);
        out += "'h";
        put_hex(out, known);
        out += ") === ";
        put_uint(out, w);
        out += "'h";
        put_hex(out, value & known);
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

    const std::vector<std::string>& sig_;
    const std::vector<std::string>& fn_;
    Target                          target_  = Target::SV2017;
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
// The arms of a switch, as a view over the flat extra run. Contiguous by
// construction (Builder::switch_ lays keys first, then the record run).
std::span<const ir::SwitchArm> arms_of(const ir::Ir& ir, const ir::SwitchInfo& info) {
    const std::span<const ir::Index> raw    = ir.region(info.arms);
    constexpr size_t                 stride = sizeof(ir::SwitchArm) / sizeof(uint32_t);
    const auto*                      base   = reinterpret_cast<const ir::SwitchArm*>(raw.data());

    return { base, raw.size() / stride };
}

// --- hierarchical emission -------------------------------------------------

// One connection on an instantiation: the child's port and whatever the parent
// wired to it.
struct PortConn {
    ir::Signal    port{};       // the child-scope signal that is the formal
    ir::Index     actual{};     // an expression in the parent, or the lvalue it drives
    const ir::Ir* ir = nullptr; // the copy process's IR, which `actual` indexes
};

// A process reduced to a single unguarded signal-to-signal copy, which is how the
// frontend represents a port connection: `u1.d = a` for an input, `mid = u1.q` for
// an output. Recognising these is what turns a flattened design back into a
// hierarchy -- there is no port map in the IR, only these.
struct Copy {
    bool       valid = false;
    ir::Index  lhs{}; // the store's lvalue expression
    ir::Index  rhs{}; // the store's value expression
    ir::Signal lhs_sig{};
    ir::Signal rhs_sig{};
    bool       rhs_is_signal = false;
};

Copy as_copy(const ir::Ir& ir) {
    Copy      c;
    ir::Index store{};
    size_t    stores = 0;
    for (uint32_t i = 0; i < ir.inst_count(); ++i) {
        const auto ix = static_cast<ir::Index>(i);
        const auto t  = ir.tag(ix);
        if (t == ir::Inst::Tag::Store || t == ir::Inst::Tag::StoreNb) {
            ++stores;
            store = ix;
        } else if (t == ir::Inst::Tag::Cond || t == ir::Inst::Tag::Switch || t == ir::Inst::Tag::Loop) {
            return c; // guarded: not a plain connection
        }
    }
    if (stores != 1) {
        return c;
    }
    const ir::LvalueRoot lv = ir::lvalue_root(ir, ir.payload(store).binary.l);
    if (ir.tag(lv.root) != ir::Inst::Tag::Signal) {
        return c;
    }
    c.lhs     = ir.payload(store).binary.l;
    c.rhs     = ir.payload(store).binary.r;
    c.lhs_sig = ir.payload(lv.root).signal;

    ir::Index r = c.rhs;
    while (ir.tag(r) == ir::Inst::Tag::Resize || ir.tag(r) == ir::Inst::Tag::Load) {
        r = ir.payload(r).unary;
    }
    if (ir.tag(r) == ir::Inst::Tag::Signal) {
        c.rhs_is_signal = true;
        c.rhs_sig       = ir.payload(r).signal;
    }
    c.valid = true;

    return c;
}

// Print a region tree as statements, verbatim.
//
// This is the 1:1 path, and it needs no analysis at all: the region tree ALREADY IS
// the statement tree, so `cond` prints as if/else, `switch` as case, `loop` as while
// and stores as assignments. It is strictly simpler than the legalized renderer,
// which has to rebuild a tree out of guarded lists first.
//
// Used for function bodies (which never had an analysis) and, when no Result is
// supplied, for processes.
bool emit_stmts(std::string& os, Renderer& render, const ir::Ir& ir, std::span<const ir::Index> body, int ind) {
    for (const ir::Index i : body) {
        const ir::Inst::Payload& p = ir.payload(i);
        switch (ir.tag(i)) {
            case ir::Inst::Tag::Store:
            case ir::Inst::Tag::StoreNb: {
                indent(os, ind);
                render.emit(os, ir, p.binary.l, kPrimary);
                // The assignment kind is the opcode's, not the context's: a process
                // may mix blocking and nonblocking, and a function only ever blocks.
                os += ir.tag(i) == ir::Inst::Tag::StoreNb ? " <= " : " = ";
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

            // 2005 has no jump statements at all -- sv2v exists partly to remove
            // them -- so the target decides whether these can be spelled.
            case ir::Inst::Tag::Break:
            case ir::Inst::Tag::Continue:
                if (render.target() == Target::V2005) {
                    render.decline(ir.tag(i));
                    return false;
                }
                indent(os, ind);
                os += ir.tag(i) == ir::Inst::Tag::Break ? "break;\n" : "continue;\n";
                break;

            case ir::Inst::Tag::Cond: {
                const auto                       r     = ir.extra_data<ir::Regions>(p.op_extra.extra);
                const std::span<const ir::Index> then_ = ir.region(r.then_);
                const std::span<const ir::Index> else_ = ir.region(r.else_);
                indent(os, ind);
                os += "if (";
                render.emit(os, ir, p.op_extra.op, kLowest);
                os += ") begin\n";
                if (!render.ok() || !emit_stmts(os, render, ir, then_, ind + 1)) {
                    return false;
                }
                indent(os, ind);
                // An absent else stays absent. This is the 1:1 path: filling the arm
                // in is the legalized renderer's job and would change what the
                // source means here.
                if (else_.empty()) {
                    os += "end\n";
                    break;
                }
                os += "end else begin\n";
                if (!emit_stmts(os, render, ir, else_, ind + 1)) {
                    return false;
                }
                indent(os, ind);
                os += "end\n";
                break;
            }

            case ir::Inst::Tag::Switch: {
                const auto info = ir.extra_data<ir::SwitchInfo>(p.op_extra.extra);
                indent(os, ind);
                os += "case (";
                render.emit(os, ir, p.op_extra.op, kLowest);
                os += ")\n";
                if (!render.ok()) {
                    return false;
                }
                for (const ir::SwitchArm& a : arms_of(ir, info)) {
                    const std::span<const ir::Index> keys = ir.region(a.keys);
                    indent(os, ind + 1);
                    for (size_t k = 0; k + 1 < keys.size(); k += 2) {
                        if (k != 0) {
                            os += ", ";
                        }
                        // Sized to the selector, or unsized when the frontend
                        // declined to state a width (the one-hot lowering): `0'h0`
                        // is not legal source.
                        if (info.sel_width != 0) {
                            put_uint(os, info.sel_width);
                        }
                        os += "'h";
                        put_hex(os, static_cast<uint64_t>(keys[k]) | (static_cast<uint64_t>(keys[k + 1]) << 32));
                    }
                    os += ": begin\n";
                    if (!emit_stmts(os, render, ir, ir.region(a.body), ind + 2)) {
                        return false;
                    }
                    indent(os, ind + 1);
                    os += "end\n";
                }
                const std::span<const ir::Index> dflt = ir.region(info.default_);
                if (!dflt.empty()) {
                    indent(os, ind + 1);
                    os += "default: begin\n";
                    if (!emit_stmts(os, render, ir, dflt, ind + 2)) {
                        return false;
                    }
                    indent(os, ind + 1);
                    os += "end\n";
                }
                indent(os, ind);
                os += "endcase\n";
                break;
            }

            case ir::Inst::Tag::Loop: {
                const auto info = ir.extra_data<ir::LoopInfo>(p.extra);
                // The predicate is computed by the cond region. When that region is
                // only value ops the whole test is one expression and this prints as
                // a `while`; a cond region that ASSIGNS cannot be spelled that way,
                // and is declined rather than approximated.
                for (const ir::Index c : ir.region(info.cond)) {
                    const ir::Inst::Tag t = ir.tag(c);
                    if (t == ir::Inst::Tag::Store || t == ir::Inst::Tag::StoreNb) {
                        render.decline(ir::Inst::Tag::Loop);
                        return false;
                    }
                }
                if (info.bottom_test != 0) {
                    render.decline(ir::Inst::Tag::Loop); // do-while: not 2005, and rare
                    return false;
                }
                indent(os, ind);
                os += "while (";
                render.emit(os, ir, info.pred, kLowest);
                os += ") begin\n";
                if (!render.ok() || !emit_stmts(os, render, ir, ir.region(info.body), ind + 1)) {
                    return false;
                }
                // The step runs at the end of every iteration, which is what `for`
                // hides and `while` has to spell.
                if (!emit_stmts(os, render, ir, ir.region(info.step), ind + 1)) {
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

            // A function call is a VALUE that happens to sit at statement level in
            // the instruction stream; whoever consumes it renders it inline. It is
            // only reachable here because a call's legality is contextual, so the
            // static tag table marks it unsynthesizable and the default arm below
            // would decline it.
            case ir::Inst::Tag::Call:
                break;

            // A task call is a statement. FnLegality only admits a task that writes
            // no module signal, so the call is a no-op -- but it is emitted anyway,
            // because this path claims to be 1:1 and dropping a statement is not.
            case ir::Inst::Tag::TaskCall: {
                const auto info = ir.extra_data<ir::TaskCallInfo>(p.extra);
                indent(os, ind);
                render.emit_task_name(os, info.func);
                os += ";\n";
                break;
            }
            default:
                // Pure value instructions are rendered inline by their users.
                if (!ir::tag_info(ir.tag(i)).synthesizable) {
                    render.decline(ir.tag(i));
                    return false;
                }
                break;
        }
    }

    return true;
}

// Append a type's unpacked dimensions, outermost first, as declared.
//
// An unpacked array keeps its dimensions in the emitted declaration
// (`logic [31:0] m [0:3];`), because the renderer emits an element select with the
// DECLARED index -- `m[5]` for `int m[7:4]`. Declaring from the layout width alone
// (which reports the LEAF width for an array) flattened every memory to a vector
// and silently turned each element select into a bit select.
void unpacked_dims(const ir::InternPool& pool, ir::Ref type, std::string& os) {
    for (ir::Ref t = type;;) {
        const ir::Type& ty = std::get<ir::Type>(pool.get(t));
        if (ty.kind != ir::Type::Kind::UnpackedArray) {
            return;
        }
        os += " [";
        put_uint(os, ty.left);
        os += ':';
        put_uint(os, ty.right);
        os += ']';
        t = ty.element;
    }
}

// One `<kind> [W-1:0] <name> <dims>;` declaration, or nothing when the type has no
// SystemVerilog form (see unsynthesizable_type). `kind` carries its own trailing
// space; the caller has already emitted any indentation.
bool emit_decl(std::string&          os,
               const ir::InternPool& pool,
               ir::Ref               type,
               std::string_view      kind,
               std::string_view      name) {
    if (unsynthesizable_type(pool, type) != nullptr) {
        return false;
    }
    os += kind;
    if (const uint32_t w = pool.layout(type).width; w > 1) {
        os += '[';
        put_uint(os, w - 1);
        os += ":0] ";
    }
    os += name;
    unpacked_dims(pool, type, os);
    os += ";\n";

    return true;
}

// The refusal line that replaces a declaration with no SystemVerilog form. Named
// rather than dropped: an omitted signal reads as a missing declaration, which is
// indistinguishable from a writer bug.
void emit_decl_refusal(std::string& os, const ir::InternPool& pool, ir::Ref type, std::string_view name) {
    os += "  // ";
    os += name;
    os += ": no synthesizable type (";
    os += unsynthesizable_type(pool, type);
    os += ")\n";
}

// A function's SystemVerilog declaration, or nothing when its body is not
// renderable. Formals and locals share one naming rule (`l<i>`), so the expression
// renderer needs no notion of which frame it is in.
std::optional<std::string> emit_fn(const ir::Module& m, Renderer& render, ir::FunctionId id, std::string_view name) {
    const ir::Function& f = m.function(id);
    std::string         os;
    const auto          width = [&](ir::Ref t) { return m.pool.layout(t).width; };
    // A frame slot with no hardware form (a string temp, a container local) has no
    // declaration to write. Refusing the whole function is right rather than
    // conservative: its body necessarily touches the slot, and a `logic [W-1:0]`
    // written from a width-0 layout underflows to `[4294967295:0]`.
    for (const ir::Ref t : f.local_types) {
        if (unsynthesizable_type(m.pool, t) != nullptr) {
            return std::nullopt;
        }
    }
    if (f.return_type != ir::Ref::Invalid && unsynthesizable_type(m.pool, f.return_type) != nullptr) {
        return std::nullopt;
    }
    // A task has no return type, so it declares as a task. Both take their formals
    // the same way, and both name frame slots `l<i>`. Tasks are emitted rather than
    // skipped because the 1:1 path calls them, and a call to a task that was never
    // declared does not re-lower.
    const bool is_task = f.is_task || f.return_type == ir::Ref::Invalid;
    if (is_task) {
        os += "  task automatic ";
    } else {
        os += render.target() == Target::V2005 ? "  function automatic [" : "  function automatic logic [";
        put_uint(os, width(f.return_type) - 1);
        os += ":0] ";
    }
    os += name;
    os += '(';
    for (size_t a = 0; a < f.param_types.size(); ++a) {
        if (a != 0) {
            os += ", ";
        }
        os += render.target() == Target::V2005 ? "input [" : "input logic [";
        put_uint(os, width(f.param_types[a]) - 1);
        os += ":0] l";
        put_uint(os, a);
        unpacked_dims(m.pool, f.param_types[a], os);
    }
    os += ");\n";
    for (size_t l = f.param_types.size(); l < f.local_types.size(); ++l) {
        os += render.target() == Target::V2005 ? "    reg [" : "    logic [";
        put_uint(os, width(f.local_types[l]) - 1);
        os += ":0] l";
        put_uint(os, l);
        unpacked_dims(m.pool, f.local_types[l], os);
        os += ";\n";
    }
    if (!emit_stmts(os, render, f.ir, f.ir.root(), 2)) {
        return std::nullopt;
    }
    os += is_task ? "  endtask\n" : "  endfunction\n";

    return os;
}

} // namespace

void emit_process(std::string&                    os,
                  const ir::Module&               module,
                  const Result*                   result,
                  size_t                          k,
                  const std::vector<std::string>& sig_names,
                  Renderer&                       render,
                  std::vector<Atom>&              pool);

// Names inside one module: the LEAF, not the flattened path.
//
// This is the point of hierarchical emission. A flat render calls the signal
// `hazard3_cpu_1port_core_alu_op_a_rev`, and every SDC constraint, physical script
// and area report that referenced `core.alu.op_a_rev` stops matching. Per scope the
// leaf is what the source wrote, so the names come back exactly -- except where two
// signals in one scope genuinely share a leaf (unrolled loop variables do), which
// takes an index suffix.
// A name as an identifier, escaped when it is not a legal simple one.
//
// Clotho folds a generate block into the instance's NAME, so hazard3 has instances
// literally called `fast_branchcmp.branchcmp_u`. Mangling the dot to an underscore
// renames the instance and breaks every constraint that referenced it; Verilog's
// escaped identifier carries the name through untouched, trailing space and all.
// It does not restore the generate STRUCTURE -- that is folded away at lowering and
// is frontend work -- but it does keep the path.
std::string ident(std::string_view name) {
    bool simple = name.size() > 0 && (name[0] == '_' || std::isalpha(static_cast<unsigned char>(name[0])) != 0);
    for (const char c : name) {
        simple = simple && (c == '_' || c == '$' || std::isalnum(static_cast<unsigned char>(c)) != 0);
    }
    if (simple) {
        return std::string(name);
    }

    return "\\" + std::string(name) + " ";
}

std::string leaf_of(std::string_view name) {
    const size_t dot = name.rfind('.');

    return std::string(dot == std::string_view::npos ? name : name.substr(dot + 1));
}

struct ScopeNames {
    // Per-signal emitted name, indexed by ir::Signal.
    std::vector<std::string> sig;
    // Per-scope emitted module name, indexed by scope id.
    std::vector<std::string> mod;
};

void collect_scope_names(const ir::Module& m, const ir::InstanceScope& sc, std::string_view path, ScopeNames& out) {
    // The module NAME is per-instance and synthetic, so it stays mangled; the
    // instance name is what downstream keys on and is emitted verbatim below.
    std::string mine = path.empty() ? mangle(sc.name) : std::string(path) + "__" + mangle(sc.name);
    if (out.mod.size() <= sc.id) {
        out.mod.resize(sc.id + 1);
    }
    out.mod[sc.id] = mine;

    ankerl::unordered_dense::set<std::string> taken;
    for (const ir::Signal g : sc.signals) {
        std::string n = ident(leaf_of(m.signal(g).name));
        if (!taken.insert(n).second) {
            n += "__" + std::to_string(static_cast<uint32_t>(g));
            taken.insert(n);
        }
        out.sig[static_cast<uint32_t>(g)] = std::move(n);
    }
    for (const ir::InstanceScope& ch : sc.children) {
        collect_scope_names(m, ch, mine, out);
    }
}

void emit_module_body(std::string&                    os,
                      const ir::Module&               m,
                      const Result*                   result,
                      const ir::InstanceScope&        sc,
                      const ScopeNames&               names,
                      const std::vector<std::string>& fn_names,
                      const std::vector<bool>&        written,
                      SynthConfig                     cfg,
                      Renderer&                       render,
                      std::vector<Atom>&              pool) {
    const auto fn_name_of = [&](size_t f) -> std::string_view { return fn_names[f]; };
    // Child scope ids, for deciding whether a copy crosses into an instance.
    ankerl::unordered_dense::map<uint32_t, const ir::InstanceScope*> child_of_signal;
    for (const ir::InstanceScope& ch : sc.children) {
        for (const ir::Signal g : ch.signals) {
            child_of_signal[static_cast<uint32_t>(g)] = &ch;
        }
    }

    // Split this scope's processes into port connections and real logic.
    ankerl::unordered_dense::map<uint32_t, std::vector<PortConn>> conns;
    std::vector<size_t>                                           logic;
    for (size_t k = 0; k < m.processes().size(); ++k) {
        const ir::Process& proc = m.processes()[k];
        if (proc.scope != sc.id) {
            continue;
        }
        const Copy c = as_copy(proc.ir);
        if (c.valid) {
            const auto lhs_child = child_of_signal.find(static_cast<uint32_t>(c.lhs_sig));
            if (lhs_child != child_of_signal.end()) {
                // parent expression drives a child input
                conns[lhs_child->second->id].push_back({ c.lhs_sig, c.rhs, &proc.ir });
                continue;
            }
            if (c.rhs_is_signal) {
                const auto rhs_child = child_of_signal.find(static_cast<uint32_t>(c.rhs_sig));
                if (rhs_child != child_of_signal.end()) {
                    // child output drives a parent lvalue
                    conns[rhs_child->second->id].push_back({ c.rhs_sig, c.lhs, &proc.ir });
                    continue;
                }
            }
        }
        logic.push_back(k);
    }

    // Header: this scope's own ports, in declaration order. A port whose type has
    // no SystemVerilog form is left OUT of the header and named in a comment under
    // it -- the alternative is `input logic q` for a queue, which is a lie the
    // reader cannot see. Its connections are dropped with it (below), and any
    // process that touched it was already refused by the legality gate.
    os += "module ";
    os += names.mod[sc.id];
    std::string refused;
    bool        any_port = false;
    for (const ir::Signal g : sc.signals) {
        if (m.signal(g).dir == ir::SignalInfo::Direction::Internal) {
            continue;
        }
        if (unsynthesizable_type(m.pool, m.signal(g).type) != nullptr) {
            emit_decl_refusal(refused, m.pool, m.signal(g).type, names.sig[static_cast<uint32_t>(g)]);
            continue;
        }
        os += any_port ? ", " : "(";
        any_port         = true;
        const auto     d = m.signal(g).dir;
        const uint32_t w = m.pool.layout(m.signal(g).type).width;
        os += d == ir::SignalInfo::Direction::Input    ? "input "
              : d == ir::SignalInfo::Direction::Output ? "output "
                                                       : "inout ";
        // 2005 has no `logic`, and a port assigned procedurally must be declared
        // `reg` -- a bare `output` defaults to a wire and cannot be driven from an
        // always block.
        if (cfg.target == Target::V2005) {
            if (d != ir::SignalInfo::Direction::Input && written[static_cast<uint32_t>(g)]) {
                os += "reg ";
            }
        } else {
            os += "logic ";
        }
        if (w > 1) {
            os += '[';
            put_uint(os, w - 1);
            os += ":0] ";
        }
        os += names.sig[static_cast<uint32_t>(g)];
        unpacked_dims(m.pool, m.signal(g).type, os);
    }
    os += any_port ? ");\n" : ";\n";
    os += refused;

    for (const ir::Signal g : sc.signals) {
        if (m.signal(g).dir != ir::SignalInfo::Direction::Internal) {
            continue;
        }
        const std::string_view kind = cfg.target == Target::V2005 ? "  reg " : "  logic ";
        if (!emit_decl(os, m.pool, m.signal(g).type, kind, names.sig[static_cast<uint32_t>(g)])) {
            emit_decl_refusal(os, m.pool, m.signal(g).type, names.sig[static_cast<uint32_t>(g)]);
        }
    }

    // Child instantiations, with the port map put back together.
    for (const ir::InstanceScope& ch : sc.children) {
        os += "  ";
        os += names.mod[ch.id];
        os += ' ';
        os += ident(ch.name);
        os += " (";
        bool       first = true;
        const auto it    = conns.find(ch.id);
        if (it != conns.end()) {
            for (const PortConn& pc : it->second) {
                // The port was left out of the child's header, so there is nothing
                // to connect to.
                if (unsynthesizable_type(m.pool, m.signal(pc.port).type) != nullptr) {
                    continue;
                }
                if (!first) {
                    os += ", ";
                }
                first = false;
                os += '.';
                os += names.sig[static_cast<uint32_t>(pc.port)];
                os += '(';
                render.emit(os, *pc.ir, pc.actual, kLowest);
                os += ')';
            }
        }
        os += ");\n";
    }
    // Functions this scope actually calls. The IR has one design-wide table, so a
    // module gets only the operators its own processes reach -- emitting all of
    // them everywhere would be correct and unreadable.
    ankerl::unordered_dense::set<uint32_t> called;
    for (const size_t k : logic) {
        const ir::Ir& pir = m.processes()[k].ir;
        for (uint32_t i = 0; i < pir.inst_count(); ++i) {
            const auto ix = static_cast<ir::Index>(i);
            if (pir.tag(ix) == ir::Inst::Tag::Call) {
                called.insert(static_cast<uint32_t>(pir.extra_data<ir::CallInfo>(pir.payload(ix).extra).func));
            } else if (pir.tag(ix) == ir::Inst::Tag::TaskCall) {
                called.insert(static_cast<uint32_t>(pir.extra_data<ir::TaskCallInfo>(pir.payload(ix).extra).func));
            }
        }
    }
    // A called function may itself call others.
    for (bool grew = true; grew;) {
        grew = false;
        for (const uint32_t f : std::vector<uint32_t>(called.begin(), called.end())) {
            const ir::Ir& fir = m.function(static_cast<ir::FunctionId>(f)).ir;
            for (uint32_t i = 0; i < fir.inst_count(); ++i) {
                const auto ix = static_cast<ir::Index>(i);
                if (fir.tag(ix) != ir::Inst::Tag::Call) {
                    continue;
                }
                grew = called.insert(static_cast<uint32_t>(fir.extra_data<ir::CallInfo>(fir.payload(ix).extra).func))
                         .second ||
                       grew;
            }
        }
    }
    for (size_t f = 0; f < m.function_count(); ++f) {
        if (!called.contains(static_cast<uint32_t>(f))) {
            continue;
        }
        if (const std::optional<std::string> text = emit_fn(m, render, static_cast<ir::FunctionId>(f), fn_name_of(f))) {
            os += *text;
        }
    }

    for (const size_t k : logic) {
        emit_process(os, m, result, k, names.sig, render, pool);
    }
    os += "endmodule\n\n";
}

// Children first, then the parent.
void emit_scope_tree(std::string&                    os,
                     const ir::Module&               m,
                     const Result*                   result,
                     const ir::InstanceScope&        sc,
                     const ScopeNames&               names,
                     const std::vector<std::string>& fn_names,
                     const std::vector<bool>&        written,
                     SynthConfig                     cfg,
                     Renderer&                       render,
                     std::vector<Atom>&              pool) {
    for (const ir::InstanceScope& ch : sc.children) {
        emit_scope_tree(os, m, result, ch, names, fn_names, written, cfg, render, pool);
    }
    emit_module_body(os, m, result, sc, names, fn_names, written, cfg, render, pool);
}

// Emit one process: its sensitivity list and body.
//
// Shared by the flat and hierarchical writers, and by both renderers -- the
// difference is only which statement source is walked, the region tree or the
// guarded lists.
void emit_process(std::string&                    os,
                  const ir::Module&               module,
                  const Result*                   result,
                  size_t                          k,
                  const std::vector<std::string>& sig_names,
                  Renderer&                       render,
                  std::vector<Atom>&              pool) {
    const ProcessResult* pr   = result != nullptr ? &result->processes[k] : nullptr;
    const ir::Process&   proc = module.processes()[k];
    if (pr != nullptr && !pr->synthesizable) {
        os += "  // process ";
        put_uint(os, k);
        os += ": not synthesizable\n";
        return;
    }
    if (pr != nullptr && pr->signals.empty()) {
        return;
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

    // No Result: print the region tree and stop. Nothing is inferred, no arm is
    // filled in, and the output means exactly what the input meant.
    if (pr == nullptr) {
        const size_t mark = os.size();
        render.clear();
        if (!emit_stmts(os, render, proc.ir, proc.ir.root(), 2) || !render.ok()) {
            os.resize(mark);
            os += "    // process ";
            put_uint(os, k);
            os += ": not renderable (opcode ";
            put_uint(os, static_cast<uint32_t>(render.blocker()));
            os += ")\n";
        }
        os += "  end\n";
        return;
    }

    for (const SignalStores& e : pr->signals) {
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
            guard_atoms(result->guards, st.guard, pool);
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

// Every emitted name, computed ONCE.
//
// The sidecar's whole value is that `original -> emitted` is true, so it cannot
// recompute names alongside the writer and hope they agree. Both call this.
EmittedNames emitted_names(const ir::Module& module, SynthConfig cfg) {
    EmittedNames out;
    const auto   signals = module.signals();

    std::vector<std::string> raw_fns;
    raw_fns.reserve(module.function_count());
    for (size_t i = 0; i < module.function_count(); ++i) {
        raw_fns.push_back(module.function(static_cast<ir::FunctionId>(i)).name);
    }
    out.fn = unique_names(raw_fns);

    if (cfg.hierarchy == Hierarchy::Instances) {
        ScopeNames sn;
        sn.sig.assign(signals.size(), std::string{});
        for (const ir::InstanceScope& t : module.tops()) {
            collect_scope_names(module, t, {}, sn);
        }
        out.sig = std::move(sn.sig);
        out.mod = std::move(sn.mod);
        out.scope.assign(out.mod.size(), std::string{});
        // The full instance path, which is the key downstream keys on.
        const auto walk = [&](auto&& self, const ir::InstanceScope& sc, const std::string& pre) -> void {
            const std::string here = pre.empty() ? sc.name : pre + "." + sc.name;
            if (out.scope.size() <= sc.id) {
                out.scope.resize(sc.id + 1);
            }
            out.scope[sc.id] = here;
            for (const ir::InstanceScope& ch : sc.children) {
                self(self, ch, here);
            }
        };
        for (const ir::InstanceScope& t : module.tops()) {
            walk(walk, t, {});
        }

        return out;
    }

    std::vector<std::string> raw_sigs;
    raw_sigs.reserve(signals.size());
    for (const ir::SignalInfo& s : signals) {
        raw_sigs.push_back(s.name);
    }
    out.sig = unique_names(raw_sigs);

    return out;
}

std::string materialize(const ir::Module& module, const Result* result, SynthConfig cfg) {
    std::string os;
    // Grown by doubling, a 3.4 MB result copies several megabytes of text for
    // nothing. The estimate only has to be the right order: signals dominate the
    // declarations and processes dominate the body.
    os.reserve(4096 + 64 * module.signals().size() + 512 * module.processes().size());
    const auto signals = module.signals();

    const EmittedNames              names_all = emitted_names(module, cfg);
    const std::vector<std::string>& sig_names = names_all.sig;
    const std::vector<std::string>& fn_names  = names_all.fn;
    Renderer                        render(sig_names, fn_names, cfg.target);
    std::vector<Atom>               pool; // flat guard paths, reused per signal

    // Which signals any process writes -- the reg/wire question for the 2005 target.
    std::vector<bool> written(signals.size(), false);
    for (const ir::Process& proc : module.processes()) {
        for (uint32_t i = 0; i < proc.ir.inst_count(); ++i) {
            const auto ix = static_cast<ir::Index>(i);
            if (proc.ir.tag(ix) != ir::Inst::Tag::Store && proc.ir.tag(ix) != ir::Inst::Tag::StoreNb) {
                continue;
            }
            const ir::LvalueRoot lv = ir::lvalue_root(proc.ir, proc.ir.payload(ix).binary.l);
            if (proc.ir.tag(lv.root) == ir::Inst::Tag::Signal) {
                written[static_cast<uint32_t>(proc.ir.payload(lv.root).signal)] = true;
            }
        }
    }

    // Hierarchical: one module per instance, names exactly as written, port maps
    // rebuilt from the copy processes. Children are emitted before their parents so
    // the file reads bottom-up; SystemVerilog does not require it, but a reader does.
    if (cfg.hierarchy == Hierarchy::Instances) {
        ScopeNames names;
        names.sig = names_all.sig;
        names.mod = names_all.mod;
        Renderer          hr(names.sig, fn_names, cfg.target);
        std::vector<Atom> hpool;
        for (const ir::InstanceScope& t : module.tops()) {
            emit_scope_tree(os, module, result, t, names, fn_names, written, cfg, hr, hpool);
        }

        return os;
    }

    // No ports. A flattened whole-design render has no meaningful port boundary --
    // an inner module's `input` is driven by its parent, so emitting it as a
    // top-level input would be illegal source. Classification is about process
    // structure, not connectivity, so every signal becomes an internal `logic` and
    // an original input simply has no driver.
    os += "module m;\n";
    for (size_t i = 0; i < signals.size(); ++i) {
        // 2005 has no `logic`: a procedurally assigned signal is a reg, anything
        // else is a wire. Every continuous assign lowered to a process, so "written
        // by some process" is the whole test.
        const std::string_view kind = cfg.target == Target::V2005 ? (written[i] ? "  reg " : "  wire ") : "  logic ";
        if (!emit_decl(os, module.pool, signals[i].type, kind, sig_names[i])) {
            emit_decl_refusal(os, module.pool, signals[i].type, sig_names[i]);
        }
    }

    // Every function that renders. FnLegality has already refused any that writes
    // a module signal, so a rendered one is a self-contained operator.
    for (size_t i = 0; i < module.function_count(); ++i) {
        const auto id = static_cast<ir::FunctionId>(i);
        if (const std::optional<std::string> text = emit_fn(module, render, id, fn_names[i])) {
            os += *text;
        }
    }

    for (size_t k = 0; k < module.processes().size(); ++k) {
        emit_process(os, module, result, k, sig_names, render, pool);
    }
    os += "endmodule\n";

    return os;
}

namespace {

void json_str(std::string& os, std::string_view v) {
    os += '"';
    for (const char c : v) {
        switch (c) {
            case '"':
                os += "\\\"";
                break;
            case '\\':
                os += "\\\\";
                break;
            case '\n':
                os += "\\n";
                break;
            default:
                os += c;
        }
    }
    os += '"';
}

void json_field(std::string& os, std::string_view k, std::string_view v, bool comma = true) {
    json_str(os, k);
    os += ':';
    json_str(os, v);
    if (comma) {
        os += ',';
    }
}

std::string_view kind_name(StorageKind k) {
    switch (k) {
        case StorageKind::Comb:
            return "comb";
        case StorageKind::Latch:
            return "latch";
        case StorageKind::Flop:
            return "flop";
        case StorageKind::AsyncLoad:
            return "async_load";
        case StorageKind::Unclassified:
            return "unclassified";
    }

    return "unclassified";
}

std::string_view finding_name(Finding f) {
    switch (f) {
        case Finding::UnsynthesizableOp:
            return "unsynthesizable_op";
        case Finding::Suspends:
            return "suspends";
        case Finding::Call:
            return "call";
        case Finding::Fork:
            return "fork";
        case Finding::AmbiguousClock:
            return "ambiguous_clock";
        case Finding::AsyncLoad:
            return "async_load";
        case Finding::FullCaseXSelector:
            return "full_case_x_selector";
        case Finding::FullCasePragmaUnsound:
            return "full_case_pragma_unsound";
        case Finding::UnmodelledJump:
            return "unmodelled_jump";
    }

    return "unknown";
}

} // namespace

std::string sidecar(const ir::Module& module, const Result* result, SynthConfig cfg) {
    const EmittedNames names   = emitted_names(module, cfg);
    const auto         signals = module.signals();

    // Which scope each signal belongs to, so the map can carry the instance path.
    std::vector<uint32_t> scope_of(signals.size(), ~0u);
    const auto            walk = [&](auto&& self, const ir::InstanceScope& sc) -> void {
        for (const ir::Signal g : sc.signals) {
            scope_of[static_cast<uint32_t>(g)] = sc.id;
        }
        for (const ir::InstanceScope& ch : sc.children) {
            self(self, ch);
        }
    };
    for (const ir::InstanceScope& t : module.tops()) {
        walk(walk, t);
    }

    // Classification by signal, when there is one.
    std::vector<const SignalStores*> cls(signals.size(), nullptr);
    if (result != nullptr) {
        for (const ProcessResult& pr : result->processes) {
            for (const SignalStores& e : pr.signals) {
                cls[static_cast<uint32_t>(e.signal)] = &e;
            }
        }
    }

    std::string os;
    os.reserve(256 + 128 * signals.size());
    os += "{\"version\":1,";
    json_field(os, "hierarchy", cfg.hierarchy == Hierarchy::Instances ? "instances" : "flat");
    json_field(os, "target", cfg.target == Target::V2005 ? "v2005" : "sv2017");

    os += "\"instances\":[";
    for (size_t i = 0, n = 0; i < names.scope.size(); ++i) {
        if (names.scope[i].empty()) {
            continue;
        }
        os += n++ != 0 ? ",{" : "{";
        json_field(os, "path", names.scope[i]);
        json_str(os, "module");
        os += ':';
        json_str(os, i < names.mod.size() ? names.mod[i] : std::string_view{});
        os += '}';
    }
    os += "],\"signals\":[";

    for (size_t i = 0, n = 0; i < signals.size(); ++i) {
        os += n++ != 0 ? ",{" : "{";
        // The original hierarchical path, and what it became. Bidirectional: the
        // emitted name is unique within its scope, the path is unique globally.
        json_field(os, "path", signals[i].name);
        json_field(os, "emitted", names.sig[i]);
        const uint32_t sc = scope_of[i];
        if (sc != ~0u && sc < names.scope.size()) {
            json_field(os, "scope", names.scope[sc]);
        }
        const SignalStores* e = cls[i];
        if (e == nullptr) {
            os.pop_back(); // trailing comma
            os += '}';
            continue;
        }
        json_field(os, "kind", kind_name(e->kind));
        if (e->kind == StorageKind::Flop || e->kind == StorageKind::AsyncLoad) {
            json_field(os, "clock", module.signal(e->decomp.clock).name);
            json_field(os, "clock_edge", e->decomp.clock_edge == ir::Edge::Negative ? "negedge" : "posedge");
            os += "\"async\":[";
            for (size_t g = 0; g < e->decomp.async.size(); ++g) {
                const AsyncGroup& ag = e->decomp.async[g];
                os += g != 0 ? ",{" : "{";
                json_field(os, "trigger", module.signal(ag.trigger).name);
                json_field(os, "edge", ag.edge == ir::Edge::Negative ? "negedge" : "posedge");
                os += "\"entries\":";
                put_uint(os, ag.entries.size());
                os += '}';
            }
            os += "],";
            os += "\"async_reset\":";
            os += e->async_reset ? "true," : "false,";
        }
        os += "\"always_enabled\":";
        os += e->decomp.always_enabled ? "true" : "false";
        os += '}';
    }
    os += ']';

    if (result != nullptr) {
        os += ",\"findings\":[";
        for (size_t i = 0; i < result->notes.size(); ++i) {
            const Note& nt = result->notes[i];
            os += i != 0 ? ",{" : "{";
            json_field(os, "kind", finding_name(nt.kind));
            os += "\"process\":";
            put_uint(os, nt.process);
            const auto sg = static_cast<uint32_t>(nt.signal);
            if (sg < signals.size()) {
                os += ',';
                json_field(os, "signal", signals[sg].name, /*comma=*/false);
            }
            os += '}';
        }
        os += ']';
    }
    os += "}\n";

    return os;
}

} // namespace clotho::legalize
