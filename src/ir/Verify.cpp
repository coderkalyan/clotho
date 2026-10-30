#include "ir/Verify.h"

#include <format>
#include <optional>
#include <variant>

#include "ir/Printer.h"
#include "ir/TypeModel.h"

namespace clotho::ir {

namespace {

using Tag = Inst::Tag;

// Walks one body, appending to `out`. Control flow lives in check_inst; the
// helpers below it only compare types and report.
class Checker {
public:
    Checker(const Module& m, const Ir& ir, Site site, std::vector<Violation>& out)
      : m_(m)
      , ir_(ir)
      , site_(site)
      , types_(value_types(m, ir))
      , out_(out)
      , fn_(site.kind == Site::Kind::Function ? &m.function(static_cast<FunctionId>(site.id)) : nullptr) {}

    void run() {
        for (size_t k = 0; k < ir_.inst_count(); ++k) {
            check_inst(static_cast<Index>(k));
        }
    }

private:
    const Module&           m_;
    const Ir&               ir_;
    const Site              site_;
    std::vector<ValueType>  types_;
    std::vector<Violation>& out_;
    const Function* const   fn_;

    bool valid(Index x) const {
        return x != kNoIndex && static_cast<uint32_t>(x) < types_.size();
    }

    ValueType ty(Index x) const {
        return valid(x) ? types_[static_cast<uint32_t>(x)] : ValueType{};
    }

    void report(Rule rule, Index at, ValueType a = {}, ValueType b = {}) {
        out_.push_back(
          { .rule = rule, .tag = ir_.tag(at), .inst = at, .site = site_, .types = { a, b }, .loc = ir_.loc(at) });
    }

    static bool is_scalar(ValueType t) {
        return !t.is_string && !t.is_real;
    }

    // -- Dispatch --

    void check_inst(Index i) {
        const Inst::Payload& p = ir_.payload(i);
        switch (ir_.tag(i)) {
            case Tag::Add:
            case Tag::Sub:
            case Tag::Mul:
            case Tag::Band:
            case Tag::Bor:
            case Tag::Bxor:
            case Tag::Bxnor:
                check_integer_pair(i, p.binary.l, p.binary.r);
                check_result_sign(i, p.binary.l, p.binary.r);
                return;
            case Tag::UDiv:
            case Tag::SDiv:
            case Tag::UMod:
            case Tag::SMod:
                check_integer_pair(i, p.binary.l, p.binary.r);
                check_sign_pair(i, p.binary.l, p.binary.r);
                check_forced_four_state(i, p.binary.l, p.binary.r);
                check_op_sign(i, p.binary.l, signed_tag(ir_.tag(i)));
                return;
            case Tag::Pow: {
                const auto info = ir_.extra_data<PowInfo>(p.op_extra.extra);
                check_forced_four_state(i, p.op_extra.op, info.exp);
                check_op_sign(i, p.op_extra.op, info.base_signed != 0);
                check_op_sign(i, info.exp, info.exp_signed != 0);
                return;
            }
            case Tag::ULt:
            case Tag::SLt:
            case Tag::ULe:
            case Tag::SLe:
            case Tag::UGt:
            case Tag::SGt:
            case Tag::UGe:
            case Tag::SGe:
                check_compare(i, p.binary.l, p.binary.r, /*ordered=*/true);
                check_op_sign(i, p.binary.l, signed_tag(ir_.tag(i)));
                return;
            case Tag::Eq:
            case Tag::Ne:
                check_compare(i, p.binary.l, p.binary.r, /*ordered=*/false);
                return;
            case Tag::CaseEq:
            case Tag::CasezEq:
            case Tag::CasexEq:
            case Tag::WildEq:
                check_integer_pair(i, p.binary.l, p.binary.r);
                return;
            case Tag::Sll:
            case Tag::Srl:
            case Tag::Sra:
                check_shift(i, p.binary.l, p.binary.r);
                if (ir_.tag(i) == Tag::Sra) {
                    check_op_sign(i, p.binary.l, true);
                }
                return;
            case Tag::UItoF:
            case Tag::SItoF:
                check_op_sign(i, p.op_type.op, signed_tag(ir_.tag(i)));
                return;
            case Tag::FAdd:
            case Tag::FSub:
            case Tag::FMul:
            case Tag::FDiv:
            case Tag::FPow:
            case Tag::FEq:
            case Tag::FNe:
            case Tag::FLt:
            case Tag::FLe:
            case Tag::FGt:
            case Tag::FGe:
                check_real_pair(i, p.binary.l, p.binary.r);
                return;
            case Tag::InsideSet:
                check_inside(i);
                return;
            case Tag::Select:
                check_select(i);
                return;
            case Tag::SelectLazy:
                check_select_lazy(i);
                return;
            case Tag::Cond:
                check_predicate(i, p.op_extra.op);
                return;
            case Tag::Loop:
                check_predicate(i, ir_.extra_data<LoopInfo>(p.extra).pred);
                return;
            case Tag::Store:
            case Tag::StoreNb:
                check_store(i, p.binary.l, p.binary.r);
                return;
            case Tag::StoreNbDelay:
            case Tag::StoreInertialDelay:
                check_store(i, p.op_extra.op, ir_.extra_data<StoreNbDelayInfo>(p.op_extra.extra).value);
                return;
            case Tag::StoreNbEvent:
                check_store(i, p.op_extra.op, ir_.extra_data<StoreNbEventInfo>(p.op_extra.extra).value);
                return;
            case Tag::Resize:
                check_resize(i);
                return;
            case Tag::Call:
                check_call(i, ir_.extra_data<CallInfo>(p.extra).func, ir_.extra_data<CallInfo>(p.extra).args);
                return;
            case Tag::TaskCall:
                check_call(i, ir_.extra_data<TaskCallInfo>(p.extra).func, ir_.extra_data<TaskCallInfo>(p.extra).args);
                return;
            case Tag::CallVirtual:
            case Tag::TaskCallVirtual:
                check_call(
                  i, ir_.extra_data<VirtualCallInfo>(p.extra).func, ir_.extra_data<VirtualCallInfo>(p.extra).args);
                return;
            case Tag::Arg:
                check_arg(i);
                return;
            case Tag::Ret:
                check_ret(i);
                return;
            default:
                return;
        }
    }

    // -- Opcode signedness --

    // The opcodes that read their operands as signed numbers.
    static bool signed_tag(Tag t) {
        return t == Tag::SDiv || t == Tag::SMod || t == Tag::SLt || t == Tag::SLe || t == Tag::SGt || t == Tag::SGe ||
               t == Tag::SItoF || t == Tag::Sra;
    }

    // The signedness an opcode asserts about `operand`, against the operand's type.
    // Types carry a sign until the IR goes signless; until then this is what shows
    // the frontend's choice of opcode agrees with the language's types.
    void check_op_sign(Index i, Index operand, bool asserted_signed) {
        const ValueType a = ty(operand);
        if (is_scalar(a) && a.is_signed != asserted_signed) {
            report(Rule::OpSign, i, a);
        }
    }

    // -- Binary operators --

    // Both operands are integers of one width and one domain.
    void check_integer_pair(Index i, Index l, Index r) {
        const ValueType a = ty(l);
        const ValueType b = ty(r);
        if (!is_scalar(a) || !is_scalar(b)) {
            report(Rule::OperandKind, i, a, b);
            return;
        }
        if (a.width != b.width) {
            report(Rule::OperandWidth, i, a, b);
        }
        if (a.four_state != b.four_state) {
            report(Rule::OperandDomain, i, a, b);
        }
    }

    void check_sign_pair(Index i, Index l, Index r) {
        const ValueType a = ty(l);
        const ValueType b = ty(r);
        if (is_scalar(a) && is_scalar(b) && a.is_signed != b.is_signed) {
            report(Rule::OperandSign, i, a, b);
        }
    }

    void check_result_sign(Index i, Index l, Index r) {
        const ValueType a = ty(l);
        const ValueType b = ty(r);
        if (is_scalar(a) && is_scalar(b) && a.is_signed != b.is_signed) {
            report(Rule::ResultSign, i, a, b);
        }
    }

    // Div / Mod / Pow: the LRM result is 4-state because it can be x.
    void check_forced_four_state(Index i, Index l, Index r) {
        const ValueType a = ty(l);
        if (is_scalar(a) && !a.four_state) {
            report(Rule::ResultDomain, i, a, ty(r));
        }
    }

    // A comparison is either between two strings or between two integers.
    void check_compare(Index i, Index l, Index r, bool ordered) {
        const ValueType a = ty(l);
        const ValueType b = ty(r);
        if (a.is_string || b.is_string) {
            if (a.is_string != b.is_string) {
                report(Rule::OperandKind, i, a, b);
            }
            return;
        }
        check_integer_pair(i, l, r);
        if (ordered) {
            check_sign_pair(i, l, r);
        }
    }

    // The amount is self-determined and may differ in width, domain and sign
    // from the shifted operand; the result is the shifted operand's type, so a
    // 4-state amount over a 2-state operand has no home.
    void check_shift(Index i, Index l, Index r) {
        const ValueType a = ty(l);
        const ValueType b = ty(r);
        if (!is_scalar(a) || !is_scalar(b) || (b.four_state && !a.four_state)) {
            report(Rule::ShiftResult, i, a, b);
        }
    }

    void check_real_pair(Index i, Index l, Index r) {
        const ValueType a = ty(l);
        const ValueType b = ty(r);
        if (!a.is_real || !b.is_real) {
            report(Rule::OperandKind, i, a, b);
        } else if (a.width != b.width) {
            report(Rule::OperandWidth, i, a, b);
        }
    }

    // The selector against every singular member and range bound; ranges also
    // compare in the selector's signedness.
    void check_inside(Index i) {
        const Inst::Payload& p    = ir_.payload(i);
        const auto           info = ir_.extra_data<InsideSetInfo>(p.op_extra.extra);
        for (const Index v : ir_.region(info.values)) {
            check_integer_pair(i, p.op_extra.op, v);
        }
        for (const Index v : ir_.region(info.ranges)) {
            check_integer_pair(i, p.op_extra.op, v);
            check_sign_pair(i, p.op_extra.op, v);
        }
        if (!ir_.region(info.ranges).empty()) {
            check_op_sign(i, p.op_extra.op, info.ranges_signed != 0);
        }
    }

    // -- Selection --

    void check_arms(Index i, Index a_idx, Index b_idx) {
        const ValueType a = ty(a_idx);
        const ValueType b = ty(b_idx);
        if (a.is_string != b.is_string || a.is_real != b.is_real) {
            report(Rule::OperandKind, i, a, b);
            return;
        }
        if (a.is_string) {
            return;
        }
        if (a.width != b.width) {
            report(Rule::SelectArmWidth, i, a, b);
        }
        if (a.four_state != b.four_state) {
            report(Rule::SelectArmDomain, i, a, b);
        }
        // A real is signed whatever its declared type says; the backend's signal
        // slots do not record it, so comparing would flag a real signal read.
        if (!a.is_real && a.is_signed != b.is_signed) {
            report(Rule::SelectArmSign, i, a, b);
        }
    }

    void check_predicate(Index i, Index pred) {
        const ValueType t = ty(pred);
        if (t.width != 1 || !is_scalar(t)) {
            report(Rule::PredicateWidth, i, t);
        }
    }

    void check_select(Index i) {
        const Inst::Payload& p      = ir_.payload(i);
        const auto           select = ir_.extra_data<Select>(p.op_extra.extra);
        check_predicate(i, p.op_extra.op);
        check_arms(i, select.a, select.b);
    }

    void check_select_lazy(Index i) {
        const Inst::Payload& p   = ir_.payload(i);
        const auto           sel = ir_.extra_data<SelectLazy>(p.op_extra.extra);
        check_predicate(i, p.op_extra.op);
        check_arms(i, sel.then_val, sel.else_val);
    }

    // -- Stores --

    // The declared storage type of an lvalue, when it names one place directly.
    std::optional<Ref> declared_type(Index lv) const {
        const Inst::Payload& p          = ir_.payload(lv);
        const auto           element_of = [&](Ref aggregate) { return std::get<Type>(m_.pool.get(aggregate)).element; };
        switch (ir_.tag(lv)) {
            case Tag::Signal:
                return m_.signal(p.signal).type;
            case Tag::IfaceSignal:
                return m_.signal(ir_.extra_data<IfaceMember>(p.op_extra.extra).signal).type;
            case Tag::Local:
                return p.local.type;
            case Tag::ElemSelect:
                return element_of(ir_.extra_data<ElemSelect>(p.op_extra.extra).array_type);
            case Tag::AssocSelect:
                return element_of(ir_.extra_data<AssocArg>(p.op_extra.extra).assoc_type);
            case Tag::MemberSelect: {
                const auto  ms = ir_.extra_data<MemberSelect>(p.op_extra.extra);
                const Type& st = std::get<Type>(m_.pool.get(ms.struct_type));
                return m_.pool.struct_fields(st.fields_id)[ms.field_index].type;
            }
            default:
                return std::nullopt;
        }
    }

    // Whether the lvalue designates one plane-packed integer or real, which is
    // all a Store's width and domain rules speak about.
    bool plane_lvalue(Index lv) const {
        switch (ir_.tag(lv)) {
            case Tag::BitSelect:
            case Tag::PartSelect:
            case Tag::DynPartSelect:
                return true;
            default:
                break;
        }
        const std::optional<Ref> declared = declared_type(lv);
        if (!declared) {
            return false;
        }
        if (types::storage_class(m_.pool, *declared) != types::StorageClass::Plane) {
            return false;
        }
        return is_scalar(value_type_of(m_.pool, *declared)) || m_.pool.is_float(*declared);
    }

    void check_store(Index i, Index lv, Index value) {
        if (!valid(lv) || !valid(value) || !plane_lvalue(lv)) {
            return;
        }
        const ValueType v = ty(value);
        const ValueType l = ty(lv);
        if (v.is_string || v.is_real != l.is_real) {
            return;
        }
        if (v.width != l.width) {
            report(Rule::StoreWidth, i, v, l);
        }
        if (!v.is_real && v.four_state != l.four_state) {
            report(Rule::StoreDomain, i, v, l);
        }
    }

    // -- Resize --

    void check_resize(Index i) {
        const Inst::Payload& p      = ir_.payload(i);
        const ValueType      source = ty(p.op_type.op);
        const ValueType      target = value_type_of(m_.pool, p.op_type.type);
        if (!is_scalar(source) || !is_scalar(target)) {
            report(Rule::OperandKind, i, source, target);
            return;
        }
        if (source.is_signed == target.is_signed) {
            return;
        }
        if (target.width > source.width) {
            report(Rule::ResizeSignWiden, i, source, target);
        } else if (target.width == source.width) {
            report(Rule::ResizeSignRetag, i, source, target);
        } else {
            report(Rule::ResizeSignNarrow, i, source, target);
        }
    }

    // -- Calls, arguments, returns --

    // Whether `formal` is a plane value an actual can be compared against.
    bool plane_formal(Ref formal) const {
        return types::storage_class(m_.pool, formal) == types::StorageClass::Plane;
    }

    void check_actual(Index i, Index actual, Ref formal) {
        const ValueType a = ty(actual);
        const ValueType f = value_type_of(m_.pool, formal);
        if (a.is_string || f.is_string) {
            return;
        }
        if (a.width != f.width || a.is_real != f.is_real) {
            report(Rule::CallArgWidth, i, a, f);
        }
        if (a.four_state != f.four_state) {
            report(Rule::CallArgDomain, i, a, f);
        }
    }

    void check_call(Index i, FunctionId func, ExtraSlice arg_slice) {
        const Function&              f      = m_.function(func);
        const std::span<const Index> actual = ir_.region(arg_slice);
        if (actual.size() != f.param_types.size()) {
            report(Rule::CallArity,
                   i,
                   { .width = static_cast<uint32_t>(actual.size()) },
                   { .width = static_cast<uint32_t>(f.param_types.size()) });
            return;
        }
        for (size_t k = 0; k < actual.size(); ++k) {
            // An open-array formal is a phantom slot: the actual is the array
            // itself and the backend reads its type at the call (Function::
            // param_types in lower/Subroutines.cpp build_dpi_open_array_arg).
            const bool open_array =
              f.is_dpi && k < f.dpi_arg_ctypes.size() && f.dpi_arg_ctypes[k] == DpiCType::OpenArray;
            if (!open_array && valid(actual[k]) && plane_formal(f.param_types[k])) {
                check_actual(i, actual[k], f.param_types[k]);
            }
        }
    }

    void check_arg(Index i) {
        const Inst::Payload& p = ir_.payload(i);
        if (fn_ == nullptr || p.arg.position >= fn_->param_types.size()) {
            return;
        }
        const ValueType declared = value_type_of(m_.pool, p.arg.type);
        const ValueType formal   = value_type_of(m_.pool, fn_->param_types[p.arg.position]);
        if (!(declared == formal)) {
            report(Rule::ArgType, i, declared, formal);
        }
    }

    void check_ret(Index i) {
        if (fn_ == nullptr || !plane_formal(fn_->return_type)) {
            return;
        }
        const Index     value = ir_.payload(i).unary;
        const ValueType v     = ty(value);
        const ValueType r     = value_type_of(m_.pool, fn_->return_type);
        if (v.is_string || r.is_string) {
            return;
        }
        if (v.width != r.width || v.is_real != r.is_real) {
            report(Rule::RetWidth, i, v, r);
        }
        if (v.four_state != r.four_state) {
            report(Rule::RetDomain, i, v, r);
        }
    }
};

std::string describe(ValueType t) {
    if (t.is_string) {
        return "string";
    }
    if (t.is_real) {
        return std::format("real{}", t.width);
    }
    return std::format("{}{}[{}]", t.is_signed ? "signed " : "", t.four_state ? "logic" : "bit", t.width);
}

} // namespace

const char* rule_name(Rule rule) {
    switch (rule) {
        case Rule::OperandKind:
            return "operand-kind";
        case Rule::OperandWidth:
            return "operand-width";
        case Rule::OperandDomain:
            return "operand-domain";
        case Rule::OperandSign:
            return "operand-sign";
        case Rule::OpSign:
            return "op-sign";
        case Rule::ResultSign:
            return "result-sign";
        case Rule::ResultDomain:
            return "result-domain";
        case Rule::ShiftResult:
            return "shift-result";
        case Rule::SelectArmWidth:
            return "select-arm-width";
        case Rule::SelectArmDomain:
            return "select-arm-domain";
        case Rule::SelectArmSign:
            return "select-arm-sign";
        case Rule::PredicateWidth:
            return "predicate-width";
        case Rule::StoreWidth:
            return "store-width";
        case Rule::StoreDomain:
            return "store-domain";
        case Rule::ResizeSignWiden:
            return "resize-sign-widen";
        case Rule::ResizeSignNarrow:
            return "resize-sign-narrow";
        case Rule::ResizeSignRetag:
            return "resize-sign-retag";
        case Rule::CallArity:
            return "call-arity";
        case Rule::CallArgWidth:
            return "call-arg-width";
        case Rule::CallArgDomain:
            return "call-arg-domain";
        case Rule::ArgType:
            return "arg-type";
        case Rule::RetWidth:
            return "ret-width";
        case Rule::RetDomain:
            return "ret-domain";
    }
    return "unknown";
}

std::vector<Violation> verify(const Module& m) {
    std::vector<Violation> out;
    const auto             processes = m.processes();
    for (size_t k = 0; k < processes.size(); ++k) {
        Checker(m, processes[k].ir, { Site::Kind::Process, static_cast<uint32_t>(k) }, out).run();
    }
    for (size_t k = 0; k < m.function_count(); ++k) {
        const Function& f = m.function(static_cast<FunctionId>(k));
        if (!f.is_dpi) {
            Checker(m, f.ir, { Site::Kind::Function, static_cast<uint32_t>(k) }, out).run();
        }
    }
    return out;
}

std::string format(const Module& m, const Violation& v, const LocResolver& resolve) {
    const std::string site = v.site.kind == Site::Kind::Process
                               ? std::format("process {}", v.site.id)
                               : std::format("function {}", m.function(static_cast<FunctionId>(v.site.id)).name);
    std::string       types;
    if (v.rule == Rule::CallArity) {
        types = std::format("{} actuals, {} formals", v.types[0].width, v.types[1].width);
    } else {
        types = describe(v.types[0]);
        if (v.types[1] != ValueType{}) {
            types += ", " + describe(v.types[1]);
        }
    }
    std::string text = std::format(
      "ir-verify: {}: {} %{} {}({})", rule_name(v.rule), site, static_cast<uint32_t>(v.inst), tag_name(v.tag), types);
    if (resolve && v.loc != SourceLoc{ 0 }) {
        text += " at " + resolve(SourceSpan{ v.loc, v.loc }).where;
    }
    return text;
}

} // namespace clotho::ir
