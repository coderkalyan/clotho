#include "Printer.h"

#include <sstream>

namespace clotho::ir {

namespace {

// Render an interned type compactly, e.g. `logic[7:0]`. Free function so both
// the instruction printer and the module signal table can reuse it.
void renderType(const InternPool& pool, Ref type, std::ostream& os) {
    if (type == Ref::Invalid) {
        os << "<?>";
        return;
    }
    const Type& t = std::get<Type>(pool.get(type));
    if (t.is_signed) {
        os << "signed ";
    }
    switch (t.kind) {
        case Type::Kind::Void:
            os << "void";
            break;
        case Type::Kind::Logic:
            os << "logic";
            break;
        case Type::Kind::Bit:
            os << "bit";
            break;
        case Type::Kind::PackedArray:
            renderType(pool, t.element, os);
            os << "[" << t.left << ":" << t.right << "]";
            break;
        case Type::Kind::UnpackedArray:
            renderType(pool, t.element, os);
            os << " [" << t.left << ":" << t.right << "]";
            break;
    }
}

// Short mnemonic for an instruction tag (snake_case, femtoc-style).
const char* tagName(Inst::Tag tag) {
    using T = Inst::Tag;
    switch (tag) {
        case T::Constant:
            return "constant";
        case T::Arg:
            return "arg";
        case T::Signal:
            return "signal";
        case T::BitSelect:
            return "bit_select";
        case T::PartSelect:
            return "part_select";
        case T::Load:
            return "load";
        case T::Store:
            return "store";
        case T::StoreNb:
            return "store_nb";
        case T::Add:
            return "add";
        case T::Sub:
            return "sub";
        case T::Mul:
            return "mul";
        case T::Band:
            return "band";
        case T::Bor:
            return "bor";
        case T::Bxor:
            return "bxor";
        case T::Bnot:
            return "bnot";
        case T::RedAnd:
            return "red_and";
        case T::RedOr:
            return "red_or";
        case T::RedXor:
            return "red_xor";
        case T::Sll:
            return "sll";
        case T::Srl:
            return "srl";
        case T::Sra:
            return "sra";
        case T::Eq:
            return "eq";
        case T::Ne:
            return "ne";
        case T::Lt:
            return "lt";
        case T::Le:
            return "le";
        case T::Gt:
            return "gt";
        case T::Ge:
            return "ge";
        case T::Land:
            return "land";
        case T::Lor:
            return "lor";
        case T::Lnot:
            return "lnot";
        case T::Mux:
            return "mux";
        case T::Concat:
            return "concat";
        case T::Repl:
            return "repl";
        case T::Resize:
            return "resize";
        case T::Cond:
            return "cond";
        case T::Ret:
            return "ret";
    }

    return "?";
}

// Stateful walker: holds the IR and output stream and tracks indentation as it
// descends through region bodies.
class Printer {
public:
    Printer(const Ir& ir, std::ostream& os)
      : ir_(ir)
      , os_(os) {}

    void run() {
        for (Index i : ir_.root()) {
            renderInst(i, 0);
        }
    }

private:
    // Emit `depth` levels of two-space indentation.
    void indent(int depth) {
        for (int i = 0; i < depth; ++i) {
            os_ << "  ";
        }
    }

    // Render `%index`.
    void ref(Index i) {
        os_ << "%" << static_cast<uint32_t>(i);
    }

    // Render an interned type compactly, e.g. `logic[7:0]`.
    void renderType(Ref type) {
        clotho::ir::renderType(ir_.pool, type, os_);
    }

    // Render an interned constant as `<type> 0x<val-plane>`. Only the value
    // plane is shown; a trailing `+xz` flags any unknown bits.
    void renderConstant(Ref value) {
        const Value& v   = std::get<Value>(ir_.pool.get(value));
        const auto   all = ir_.pool.bytes(value);
        renderType(v.type);

        const auto planes      = static_cast<size_t>(ir_.pool.layout(v.type).domain);
        const auto plane_bytes = planes ? all.size() / planes : all.size();
        os_ << " 0x" << std::hex;
        if (plane_bytes == 0) {
            os_ << "0";
        }
        for (size_t k = plane_bytes; k-- > 0;) {
            os_.width(2);
            os_.fill('0');
            os_ << static_cast<unsigned>(all[k]);
        }
        os_ << std::dec;

        bool unknown = false;
        for (size_t k = plane_bytes; k < all.size(); ++k) {
            unknown |= all[k] != 0;
        }
        if (unknown) {
            os_ << "+xz";
        }
    }

    // Render one instruction line (and, for cond, its nested region blocks).
    void renderInst(Index i, int depth) {
        indent(depth);
        ref(i);
        os_ << " = " << tagName(ir_.tag(i)) << "(";

        const Inst::Tag      tag = ir_.tag(i);
        const Inst::Payload& p   = ir_.payload(i);
        switch (payloadKind(tag)) {
            case Inst::PayloadKind::None:
                break;
            case Inst::PayloadKind::Ref:
                renderConstant(p.ref);
                break;
            case Inst::PayloadKind::Signal:
                os_ << "s" << static_cast<uint32_t>(p.signal);
                break;
            case Inst::PayloadKind::Unary:
                ref(p.unary);
                break;
            case Inst::PayloadKind::Binary:
                ref(p.binary.l);
                os_ << ", ";
                ref(p.binary.r);
                break;
            case Inst::PayloadKind::OpType:
                ref(p.op_type.op);
                os_ << " -> ";
                renderType(p.op_type.type);
                break;
            case Inst::PayloadKind::Arg:
                os_ << "#" << p.arg.position << ": ";
                renderType(p.arg.type);
                break;
            case Inst::PayloadKind::Repl:
                ref(p.repl.value);
                os_ << " x" << p.repl.count;
                break;
            case Inst::PayloadKind::Extra: {
                // concat: variadic operands.
                const auto parts = ir_.region(ir_.extraData<ExtraSlice>(p.extra));
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k != 0) {
                        os_ << ", ";
                    }
                    ref(parts[k]);
                }
                break;
            }
            case Inst::PayloadKind::OpExtra:
                renderOpExtra(tag, p, depth);
                return; // cond closes its own line; others fall through below.
        }

        os_ << ")\n";
    }

    // Render the op-plus-extra payloads (mux, part_select, cond).
    void renderOpExtra(Inst::Tag tag, const Inst::Payload& p, int depth) {
        switch (tag) {
            case Inst::Tag::Mux: {
                const Mux m = ir_.extraData<Mux>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << " ? ";
                ref(m.a);
                os_ << " : ";
                ref(m.b);
                os_ << ")\n";
                break;
            }
            case Inst::Tag::PartSelect: {
                const PartSelect ps = ir_.extraData<PartSelect>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << "[" << ps.msb << ":" << ps.lsb << "])\n";
                break;
            }
            case Inst::Tag::Cond: {
                const Regions regions = ir_.extraData<Regions>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << ") {\n";
                renderRegion("then", regions.then_, depth);
                renderRegion("else", regions.else_, depth);
                indent(depth);
                os_ << "}\n";
                break;
            }
            default:
                os_ << ")\n";
                break;
        }
    }

    // Render a labelled region body, indented one level under its op.
    void renderRegion(const char* label, ExtraSlice slice, int depth) {
        indent(depth + 1);
        os_ << label << ":\n";
        for (Index child : ir_.region(slice)) {
            renderInst(child, depth + 2);
        }
    }

    const Ir&     ir_;
    std::ostream& os_;
};

} // namespace

void print(const Ir& ir, std::ostream& os) {
    Printer(ir, os).run();
}

std::string toString(const Ir& ir) {
    std::ostringstream os;
    print(ir, os);

    return os.str();
}

namespace {

const char* dirName(SignalInfo::Dir dir) {
    switch (dir) {
        case SignalInfo::Dir::Internal:
            return "internal";
        case SignalInfo::Dir::Input:
            return "input";
        case SignalInfo::Dir::Output:
            return "output";
        case SignalInfo::Dir::Inout:
            return "inout";
    }

    return "?";
}

} // namespace

void printModule(const Module& m, std::ostream& os) {
    os << "module " << m.name << "\n";

    const auto signals = m.signals();
    for (size_t i = 0; i < signals.size(); ++i) {
        const SignalInfo& s = signals[i];
        os << "  s" << i << ": ";
        renderType(m.pool, s.type, os);
        os << " " << s.name << " (" << dirName(s.dir) << ")\n";
    }

    const auto processes = m.processes();
    for (size_t i = 0; i < processes.size(); ++i) {
        const Process& p = processes[i];
        os << "process[" << i << "] ";
        if (p.kind == ProcKind::Sequential) {
            os << "kind=seq clk=" << (p.clock_edge == Edge::Posedge ? "posedge" : "negedge") << " s"
               << static_cast<uint32_t>(p.clock);
        } else {
            os << "kind=comb";
        }
        os << "\n";
        print(p.ir, os);
    }
}

std::string toString(const Module& m) {
    std::ostringstream os;
    printModule(m, os);

    return os.str();
}

} // namespace clotho::ir
