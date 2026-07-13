#include "Printer.h"

#include <sstream>

namespace clotho::ir {

namespace {

// Render an interned type compactly, e.g. `logic[7:0]`. Free function so both
// the instruction printer and the module signal table can reuse it.
void render_type(const InternPool& pool, Ref type, std::ostream& os) {
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
        case Type::Kind::Real:
            os << "real";
            break;
        case Type::Kind::ShortReal:
            os << "shortreal";
            break;
        case Type::Kind::PackedArray:
            render_type(pool, t.element, os);
            os << "[" << t.left << ":" << t.right << "]";
            break;
        case Type::Kind::UnpackedArray:
            render_type(pool, t.element, os);
            os << " [" << t.left << ":" << t.right << "]";
            break;
        case Type::Kind::String:
            os << "string";
            break;
        case Type::Kind::Struct: {
            const auto fields = pool.struct_fields(t.fields_id);
            os << (t.is_packed ? "struct packed{" : "struct{");
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << pool.field_name(fields[i]) << ":";
                render_type(pool, fields[i].type, os);
            }
            os << "}";
            break;
        }
        case Type::Kind::DynArray:
            render_type(pool, t.element, os);
            os << " []";
            break;
        case Type::Kind::Queue:
            render_type(pool, t.element, os);
            os << " [$]";
            break;
        case Type::Kind::Class: {
            const auto fields = pool.struct_fields(t.fields_id);
            os << "class{";
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << pool.field_name(fields[i]) << ":";
                render_type(pool, fields[i].type, os);
            }
            os << "}";
            break;
        }
    }
}

// Short mnemonic for an instruction tag (snake_case, femtoc-style).
const char* tag_name(Inst::Tag tag) {
    using T = Inst::Tag;
    switch (tag) {
        case T::Constant:
            return "constant";
        case T::Arg:
            return "arg";
        case T::Signal:
            return "signal";
        case T::Local:
            return "local";
        case T::BitSelect:
            return "bit_select";
        case T::PartSelect:
            return "part_select";
        case T::ElemSelect:
            return "elem_select";
        case T::MemberSelect:
            return "member_select";
        case T::ClassNew:
            return "class_new";
        case T::HandleDeref:
            return "handle_deref";
        case T::AggCopy:
            return "agg_copy";
        case T::AggEq:
            return "agg_eq";
        case T::Load:
            return "load";
        case T::Store:
            return "store";
        case T::StoreNb:
            return "store_nb";
        case T::DynArrayNew:
            return "dyn_array_new";
        case T::DynArrayNewInit:
            return "dyn_array_new_init";
        case T::DynArraySize:
            return "dyn_array_size";
        case T::DynArrayDelete:
            return "dyn_array_delete";
        case T::DynArrayCopy:
            return "dyn_array_copy";
        case T::QueueSize:
            return "queue_size";
        case T::QueuePushBack:
            return "queue_push_back";
        case T::QueuePushFront:
            return "queue_push_front";
        case T::QueuePopBack:
            return "queue_pop_back";
        case T::QueuePopFront:
            return "queue_pop_front";
        case T::QueueInsert:
            return "queue_insert";
        case T::QueueDeleteIdx:
            return "queue_delete_idx";
        case T::QueueDeleteAll:
            return "queue_delete_all";
        case T::QueueCopy:
            return "queue_copy";
        case T::StoreNbDelay:
            return "store_nb_delay";
        case T::StoreNbEvent:
            return "store_nb_event";
        case T::Add:
            return "add";
        case T::Sub:
            return "sub";
        case T::Mul:
            return "mul";
        case T::Div:
            return "div";
        case T::Mod:
            return "mod";
        case T::Pow:
            return "pow";
        case T::Band:
            return "band";
        case T::Bor:
            return "bor";
        case T::Bxor:
            return "bxor";
        case T::Bxnor:
            return "bxnor";
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
        case T::CaseEq:
            return "case_eq";
        case T::CasezEq:
            return "casez_eq";
        case T::CasexEq:
            return "casex_eq";
        case T::WildEq:
            return "wild_eq";
        case T::InsideSet:
            return "inside_set";
        case T::Land:
            return "land";
        case T::Lor:
            return "lor";
        case T::Lnot:
            return "lnot";
        case T::Select:
            return "select";
        case T::SelectLazy:
            return "select_lazy";
        case T::Concat:
            return "concat";
        case T::Repl:
            return "repl";
        case T::StreamReverse:
            return "stream_reverse";
        case T::Resize:
            return "resize";
        case T::Cond:
            return "cond";
        case T::Switch:
            return "switch";
        case T::Ret:
            return "ret";
        case T::RetVoid:
            return "ret_void";
        case T::Call:
            return "call";
        case T::TaskCall:
            return "task_call";
        case T::Fork:
            return "fork";
        case T::Loop:
            return "loop";
        case T::Delay:
            return "delay";
        case T::Wait:
            return "wait";
        case T::EventTrigger:
            return "event_trigger";
        case T::EventTriggered:
            return "event_triggered";
        case T::EventTriggerNb:
            return "event_trigger_nb";
        case T::EventTriggerNbDelayed:
            return "event_trigger_nb_delayed";
        case T::StrLen:
            return "str_len";
        case T::StrGetc:
            return "str_getc";
        case T::StrPutc:
            return "str_putc";
        case T::StrCmp:
            return "str_cmp";
        case T::StrICmp:
            return "str_icmp";
        case T::StrUpper:
            return "str_upper";
        case T::StrLower:
            return "str_lower";
        case T::StrSubstr:
            return "str_substr";
        case T::EnumName:
            return "enum_name";
        case T::StrFromPacked:
            return "str_from_packed";
        case T::StrToPacked:
            return "str_to_packed";
        case T::StrAtoi:
            return "str_atoi";
        case T::StrAtohex:
            return "str_atohex";
        case T::StrAtooct:
            return "str_atooct";
        case T::StrAtobin:
            return "str_atobin";
        case T::StrAtoreal:
            return "str_atoreal";
        case T::StrItoa:
            return "str_itoa";
        case T::StrHextoa:
            return "str_hextoa";
        case T::StrOctoa:
            return "str_octtoa";
        case T::StrBintoa:
            return "str_bintoa";
        case T::StrRealtoa:
            return "str_realtoa";
        case T::Display:
            return "display";
        case T::FDisplay:
            return "fdisplay";
        case T::Finish:
            return "finish";
        case T::CaseViol:
            return "case_viol";
        case T::Severity:
            return "severity";
        case T::Break:
            return "break";
        case T::Continue:
            return "continue";
        case T::Fflush:
            return "fflush";
        case T::FopenFd:
            return "fopen_fd";
        case T::FopenMcd:
            return "fopen_mcd";
        case T::Fclose:
            return "fclose";
        case T::Fgetc:
            return "fgetc";
        case T::Ungetc:
            return "ungetc";
        case T::Feof:
            return "feof";
        case T::Ftell:
            return "ftell";
        case T::Fseek:
            return "fseek";
        case T::Rewind:
            return "rewind";
        case T::Fgets:
            return "fgets";
        case T::Sformat:
            return "sformat";
        case T::Sformatf:
            return "sformatf";
        case T::Ferror:
            return "ferror";
        case T::Fscanf:
            return "fscanf";
        case T::Sscanf:
            return "sscanf";
        case T::FreadWord:
            return "fread_word";
        case T::FreadMem:
            return "fread_mem";
        case T::DynPartSelect:
            return "dyn_part_select";
        case T::ReadMem:
            return "read_mem";
        case T::DumpFile:
            return "dump_file";
        case T::DumpVars:
            return "dump_vars";
        case T::DumpOff:
            return "dump_off";
        case T::DumpOn:
            return "dump_on";
        case T::DumpAll:
            return "dump_all";
        case T::DumpFlush:
            return "dump_flush";
        case T::SimTime:
            return "sim_time";
        case T::TimeFormat:
            return "timeformat";
        case T::Random:
            return "random";
        case T::DistUnary:
            return "dist_unary";
        case T::DistBinary:
            return "dist_binary";
        case T::Urandom:
            return "urandom";
        case T::UrandomSeeded:
            return "urandom_seeded";
        case T::UrandomRange:
            return "urandom_range";
        case T::Clog2:
            return "clog2";
        case T::CountBits:
            return "count_bits";
        case T::OneHot:
            return "onehot";
        case T::OneHot0:
            return "onehot0";
        case T::IsUnknown:
            return "isunknown";
        case T::TestPlusArgs:
            return "test_plusargs";
        case T::ValuePlusArgs:
            return "value_plusargs";
        case T::Ctz:
            return "ctz";
        case T::Clz:
            return "clz";
        case T::FAdd:
            return "fadd";
        case T::FSub:
            return "fsub";
        case T::FMul:
            return "fmul";
        case T::FDiv:
            return "fdiv";
        case T::FPow:
            return "fpow";
        case T::FNeg:
            return "fneg";
        case T::FEq:
            return "feq";
        case T::FNe:
            return "fne";
        case T::FLt:
            return "flt";
        case T::FLe:
            return "fle";
        case T::FGt:
            return "fgt";
        case T::FGe:
            return "fge";
        case T::ItoF:
            return "itof";
        case T::FtoI:
            return "ftoi";
        case T::FtoITrunc:
            return "ftoi_trunc";
        case T::FBitcast:
            return "fbitcast";
        case T::FResize:
            return "fresize";
        case T::RealMathUnary:
            return "real_math_unary";
        case T::RealMathBinary:
            return "real_math_binary";
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
            render_inst(i, 0);
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
    void render_type(Ref type) {
        clotho::ir::render_type(ir_.pool(), type, os_);
    }

    // Render an interned constant as `<type> 0x<val-plane>`. Only the value
    // plane is shown; a trailing `+xz` flags any unknown bits.
    void render_constant(Ref value) {
        const Value& v   = std::get<Value>(ir_.pool().get(value));
        const auto   all = ir_.pool().bytes(value);
        render_type(v.type);

        // A string constant's bytes are its characters, not plane data.
        if (std::get<Type>(ir_.pool().get(v.type)).kind == Type::Kind::String) {
            os_ << " \"";
            for (const uint8_t c : all) {
                os_ << static_cast<char>(c);
            }
            os_ << '"';
            return;
        }

        const auto planes      = static_cast<size_t>(ir_.pool().layout(v.type).domain);
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

    // Render one instruction line (and, for cond / loop, its nested region blocks).
    void render_inst(Index i, int depth) {
        indent(depth);
        ref(i);
        os_ << " = " << tag_name(ir_.tag(i)) << "(";

        const Inst::Tag      tag = ir_.tag(i);
        const Inst::Payload& p   = ir_.payload(i);
        // loop shares the Extra payload with concat but renders region blocks.
        if (tag == Inst::Tag::Loop) {
            const LoopInfo lp = ir_.extra_data<LoopInfo>(p.extra);
            ref(lp.pred);
            os_ << ") {\n";
            render_region("cond", lp.cond, depth);
            render_region("body", lp.body, depth);
            render_region("step", lp.step, depth);
            indent(depth);
            os_ << "}\n";
            return;
        }
        switch (payload_kind(tag)) {
            case Inst::PayloadKind::None:
                break;
            case Inst::PayloadKind::Scale:
                os_ << "scale=" << p.scale;
                break;
            case Inst::PayloadKind::Ref:
                // ClassNew's ref is the class *type*, not an interned value.
                if (tag == Inst::Tag::ClassNew) {
                    render_type(p.ref);
                } else {
                    render_constant(p.ref);
                }
                break;
            case Inst::PayloadKind::Signal:
                os_ << "s" << static_cast<uint32_t>(p.signal);
                break;
            case Inst::PayloadKind::Wait: {
                const auto events = ir_.wait_edges(p.extra);
                for (size_t k = 0; k < events.size(); ++k) {
                    if (k != 0) {
                        os_ << " or ";
                    }
                    const char* edge = events[k].edge == static_cast<uint32_t>(Edge::Positive)   ? "posedge "
                                       : events[k].edge == static_cast<uint32_t>(Edge::Negative) ? "negedge "
                                                                                                 : "";
                    os_ << "@" << edge << "s" << static_cast<uint32_t>(events[k].signal);
                }
                break;
            }
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
                render_type(p.op_type.type);
                break;
            case Inst::PayloadKind::Arg:
                os_ << "#" << p.arg.position << ": ";
                render_type(p.arg.type);
                break;
            case Inst::PayloadKind::Local:
                os_ << "l" << p.local.index << ": ";
                render_type(p.local.type);
                break;
            case Inst::PayloadKind::Call: {
                const auto ci = ir_.extra_data<CallInfo>(p.extra);
                os_ << "fn" << static_cast<uint32_t>(ci.func);
                for (Index a : ir_.region(ci.args)) {
                    os_ << ", ";
                    ref(a);
                }
                break;
            }
            case Inst::PayloadKind::TaskCall: {
                const auto ti = ir_.extra_data<TaskCallInfo>(p.extra);
                os_ << "task" << static_cast<uint32_t>(ti.func);
                for (Index a : ir_.region(ti.args)) {
                    os_ << ", ";
                    ref(a);
                }
                break;
            }
            case Inst::PayloadKind::Repl:
                ref(p.repl.value);
                os_ << " x" << p.repl.count;
                break;
            case Inst::PayloadKind::Extra: {
                if (tag == Inst::Tag::TestPlusArgs) {
                    const auto info = ir_.extra_data<TestPlusArgsInfo>(p.extra);
                    os_ << '"' << ir_.extra_bytes(info.str, info.str_len) << '"';
                    break;
                }
                // Finish / CaseViol carry a banner-bytes record, not a variadic
                // operand slice — render the banner rather than mis-reading the
                // packed bytes as instruction indices.
                if (tag == Inst::Tag::Finish) {
                    const auto info = ir_.extra_data<FinishInfo>(p.extra);
                    os_ << '"' << ir_.extra_bytes(info.banner, info.banner_len) << "\", " << info.arg;
                    break;
                }
                if (tag == Inst::Tag::CaseViol) {
                    const auto info = ir_.extra_data<CaseViolInfo>(p.extra);
                    os_ << '"' << ir_.extra_bytes(info.banner, info.banner_len) << "\", "
                        << info.kind;
                    break;
                }
                // concat: variadic operands.
                const auto parts = ir_.region(ir_.extra_data<ExtraSlice>(p.extra));
                for (size_t k = 0; k < parts.size(); ++k) {
                    if (k != 0) {
                        os_ << ", ";
                    }
                    ref(parts[k]);
                }
                break;
            }
            case Inst::PayloadKind::Display: {
                const auto info = ir_.extra_data<DisplayInfo>(p.extra);
                os_ << '"' << ir_.extra_bytes(info.fmt, info.fmt_len) << '"';
                for (Index a : ir_.region(info.args)) {
                    os_ << ", ";
                    ref(a);
                }
                break;
            }
            case Inst::PayloadKind::FDisplay: {
                const auto info = ir_.extra_data<DisplayInfo>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << ", \"" << ir_.extra_bytes(info.fmt, info.fmt_len) << '"';
                for (Index a : ir_.region(info.args)) {
                    os_ << ", ";
                    ref(a);
                }
                break;
            }
            case Inst::PayloadKind::Severity: {
                const auto info = ir_.extra_data<SeverityInfo>(p.extra);
                os_ << "level" << info.level << ", \"" << ir_.extra_bytes(info.banner, info.banner_len)
                    << "\", \"" << ir_.extra_bytes(info.fmt, info.fmt_len) << '"';
                for (Index a : ir_.region(info.args)) {
                    os_ << ", ";
                    ref(a);
                }
                break;
            }
            case Inst::PayloadKind::Sformat: {
                const auto info = ir_.extra_data<SformatInfo>(p.extra);
                ref(info.dst);
                os_ << ", \"" << ir_.extra_bytes(info.fmt, info.fmt_len) << '"';
                for (Index a : ir_.region(info.args)) {
                    os_ << ", ";
                    ref(a);
                }
                break;
            }
            case Inst::PayloadKind::Sformatf: {
                const auto info = ir_.extra_data<SformatfInfo>(p.extra);
                os_ << '"' << ir_.extra_bytes(info.fmt, info.fmt_len) << '"';
                for (Index a : ir_.region(info.args)) {
                    os_ << ", ";
                    ref(a);
                }
                break;
            }
            case Inst::PayloadKind::Scanf: {
                const auto info = ir_.extra_data<ScanfInfo>(p.extra);
                ref(info.source);
                os_ << ", \"" << ir_.extra_bytes(info.fmt, info.fmt_len) << '"';
                for (Index a : ir_.region(info.args)) {
                    os_ << ", ";
                    ref(a);
                }
                break;
            }
            case Inst::PayloadKind::OpImm:
                os_ << "op" << p.op_imm.imm << ", ";
                ref(p.op_imm.op);
                break;
            case Inst::PayloadKind::TimeFormat: {
                const auto info = ir_.extra_data<TimeFormatInfo>(p.extra);
                ref(info.units);
                os_ << ", ";
                ref(info.precision);
                os_ << ", ";
                ref(info.min_width);
                os_ << ", \"" << ir_.extra_bytes(info.suffix, info.suffix_len) << '"';
                break;
            }
            case Inst::PayloadKind::OpExtra:
                render_op_extra(tag, p, depth);
                return; // cond closes its own line; others fall through below.
        }

        os_ << ")\n";
    }

    // Render the op-plus-extra payloads (select, part_select, cond).
    void render_op_extra(Inst::Tag tag, const Inst::Payload& p, int depth) {
        switch (tag) {
            case Inst::Tag::Select: {
                const Select m = ir_.extra_data<Select>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << " ? ";
                ref(m.a);
                os_ << " : ";
                ref(m.b);
                os_ << ")\n";
                break;
            }
            case Inst::Tag::PartSelect: {
                const PartSelect ps = ir_.extra_data<PartSelect>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << "[" << ps.msb << ":" << ps.lsb << "])\n";
                break;
            }
            case Inst::Tag::ElemSelect: {
                const ElemSelect es = ir_.extra_data<ElemSelect>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << "[";
                ref(es.index);
                os_ << "])\n";
                break;
            }
            case Inst::Tag::MemberSelect: {
                const MemberSelect ms = ir_.extra_data<MemberSelect>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << "." << ms.field_index << ")\n";
                break;
            }
            case Inst::Tag::AggCopy: {
                const AggCopyInfo info = ir_.extra_data<AggCopyInfo>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << " = ";
                ref(info.src);
                os_ << ")\n";
                break;
            }
            case Inst::Tag::AggEq: {
                const AggEqInfo info = ir_.extra_data<AggEqInfo>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << " == ";
                ref(info.rhs);
                os_ << ")\n";
                break;
            }
            case Inst::Tag::StrPutc: {
                const StrPutcInfo sp = ir_.extra_data<StrPutcInfo>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << "[";
                ref(sp.index);
                os_ << "] = ";
                ref(sp.ch);
                os_ << ")\n";
                break;
            }
            case Inst::Tag::StrSubstr: {
                const StrSubstrInfo ss = ir_.extra_data<StrSubstrInfo>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << ", ";
                ref(ss.i);
                os_ << ", ";
                ref(ss.j);
                os_ << ")\n";
                break;
            }
            case Inst::Tag::Cond: {
                const Regions regions = ir_.extra_data<Regions>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << ") {\n";
                render_region("then", regions.then_, depth);
                render_region("else", regions.else_, depth);
                indent(depth);
                os_ << "}\n";
                break;
            }
            case Inst::Tag::Switch: {
                const SwitchInfo info = ir_.extra_data<SwitchInfo>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << ") {\n";
                const std::span<const Index> arm_words = ir_.region(info.arms);
                constexpr uint32_t           arm_len   = sizeof(SwitchArm) / sizeof(uint32_t);
                for (uint32_t a = 0; a * arm_len < arm_words.size(); ++a) {
                    const auto arm = ir_.extra_data<SwitchArm>(
                      static_cast<ExtraIndex>(static_cast<uint32_t>(info.arms.start) + a * arm_len));
                    indent(depth + 1);
                    const std::span<const Index> kw = ir_.region(arm.keys);
                    for (size_t k = 0; k + 1 < kw.size(); k += 2) {
                        const uint64_t val =
                          static_cast<uint64_t>(kw[k]) | (static_cast<uint64_t>(kw[k + 1]) << 32);
                        os_ << (k == 0 ? "" : ", ") << val;
                    }
                    os_ << ":\n";
                    render_region("do", arm.body, depth + 1);
                }
                render_region("default", info.default_, depth);
                indent(depth);
                os_ << "}\n";
                break;
            }
            case Inst::Tag::SelectLazy: {
                const SelectLazy sel = ir_.extra_data<SelectLazy>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << ") {\n";
                render_region("then", sel.then_, depth);
                indent(depth + 1);
                os_ << "-> ";
                ref(sel.then_val);
                os_ << "\n";
                render_region("else", sel.else_, depth);
                indent(depth + 1);
                os_ << "-> ";
                ref(sel.else_val);
                os_ << "\n";
                indent(depth);
                os_ << "}\n";
                break;
            }
            case Inst::Tag::ValuePlusArgs: {
                const auto info = ir_.extra_data<ValuePlusArgsInfo>(p.op_extra.extra);
                ref(p.op_extra.op);
                os_ << ", \"" << ir_.extra_bytes(info.prefix, info.prefix_len) << "%"
                    << static_cast<char>(info.conv) << "\")\n";
                break;
            }
            default:
                os_ << ")\n";
                break;
        }
    }

    // Render a labelled region body, indented one level under its op.
    void render_region(const char* label, ExtraSlice slice, int depth) {
        indent(depth + 1);
        os_ << label << ":\n";
        for (Index child : ir_.region(slice)) {
            render_inst(child, depth + 2);
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

const char* direction_name(SignalInfo::Direction dir) {
    switch (dir) {
        case SignalInfo::Direction::Internal:
            return "internal";
        case SignalInfo::Direction::Input:
            return "input";
        case SignalInfo::Direction::Output:
            return "output";
        case SignalInfo::Direction::Inout:
            return "inout";
    }

    return "?";
}

} // namespace

void print_module(const Module& m, std::ostream& os) {
    os << "module " << m.name << "\n";

    const auto signals = m.signals();
    for (size_t i = 0; i < signals.size(); ++i) {
        const SignalInfo& s = signals[i];
        os << "  s" << i << ": ";
        render_type(m.pool, s.type, os);
        os << " " << s.name << " (" << direction_name(s.dir) << ")\n";
    }

    const auto processes = m.processes();
    for (size_t i = 0; i < processes.size(); ++i) {
        const Process& p = processes[i];
        os << "process[" << i << "] ";
        switch (p.activation) {
            case Activation::PreInit:
                os << "preinit";
                break;
            case Activation::Initial:
                os << "initial";
                break;
            case Activation::Always:
                os << "always";
                break;
            case Activation::Final:
                os << "final";
                break;
        }
        for (const WaitEvent& w : p.wait_events) {
            os << " @";
            switch (w.edge) {
                case Edge::Positive:
                    os << "posedge ";
                    break;
                case Edge::Negative:
                    os << "negedge ";
                    break;
                case Edge::Any:
                    break;
            }
            os << "s" << static_cast<uint32_t>(w.signal);
        }
        os << "\n";
        print(p.ir, os);
    }
}

std::string toString(const Module& m) {
    std::ostringstream os;
    print_module(m, os);

    return os.str();
}

} // namespace clotho::ir
