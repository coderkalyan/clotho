#include <cassert>

#include "Builder.h"

namespace clotho::ir {

namespace {

// Zero the whole union before setting one narrow (4-byte) member: designated
// initialization alone leaves the tail bytes indeterminate, making the payload
// column non-byte-stable for hashing / future serialization. The 8-byte
// members (scale and the two-field structs) fill the union exactly, so their
// designated initialization stays as is.
template<class M>
Inst::Payload pay(M Inst::Payload::* member, M value) {
    Inst::Payload p{ .scale = 0 };
    p.*member = value;

    return p;
}

} // namespace

Ir Builder::finish() {
    // Whatever remains in scratch_ is the root region.
    const ExtraSlice root = add_slice(scratch_);

    return Ir(
      pool_, std::move(tags_), std::move(payloads_), std::move(locs_), std::move(ends_), std::move(extra_), root);
}

void Builder::open_region() {
    region_starts_.push_back(scratch_.size());
}

ExtraSlice Builder::close_region() {
    const size_t start = region_starts_.back();
    region_starts_.pop_back();
    const std::span<const Index> body{ scratch_.data() + start, scratch_.size() - start };
    const ExtraSlice             slice = add_slice(body);
    scratch_.resize(start);

    return slice;
}

void Builder::rollback(Mark m) {
    // Regions nest within one member's emission, so an abandoned run can only
    // have left regions OPEN, never closed one that was open at the mark. If that
    // is violated the mark is not a prefix of the current state and truncating
    // would splice unrelated instructions into a live region.
    assert(region_starts_.size() >= m.regions);
    assert(tags_.size() >= m.insts);
    assert(extra_.size() >= m.extra);
    assert(scratch_.size() >= m.scratch);
    // The instruction columns are parallel, so a mark taken from one is a mark
    // for all three.
    assert(payloads_.size() == tags_.size());
    assert(locs_.size() == tags_.size());

    region_starts_.resize(m.regions);
    tags_.resize(m.insts);
    payloads_.resize(m.insts);
    locs_.resize(m.insts);
    // The abandoned run's extents would otherwise point at instructions the
    // next emission reuses those indices for.
    std::erase_if(ends_, [&](const auto& kv) { return kv.first >= m.insts; });
    extra_.resize(m.extra);
    scratch_.resize(m.scratch);
}

Index Builder::constant(Ref value, SourceLoc loc) {
    return emit({ Inst::Tag::Constant, pay(&Inst::Payload::ref, value) }, loc);
}

Index Builder::arg(uint32_t position, Ref type, SourceLoc loc) {
    return emit({ Inst::Tag::Arg, Inst::Payload{ .arg = { position, type } } }, loc);
}

Index Builder::signal(Signal sig, SourceLoc loc) {
    return emit({ Inst::Tag::Signal, pay(&Inst::Payload::signal, sig) }, loc);
}

Index Builder::local(uint32_t index, Ref type, SourceLoc loc) {
    return emit({ Inst::Tag::Local, Inst::Payload{ .local = { index, type } } }, loc);
}

Index Builder::event_new(SourceLoc loc) {
    return emit({ Inst::Tag::EventNew, Inst::Payload{} }, loc);
}

Index Builder::event_trigger(Index handle, SourceLoc loc) {
    return emit({ Inst::Tag::EventTrigger, pay(&Inst::Payload::unary, handle) }, loc);
}

Index Builder::event_triggered(Index handle, SourceLoc loc) {
    return emit({ Inst::Tag::EventTriggered, pay(&Inst::Payload::unary, handle) }, loc);
}

Index Builder::event_trigger_nb(Index handle, SourceLoc loc) {
    return emit({ Inst::Tag::EventTriggerNb, pay(&Inst::Payload::unary, handle) }, loc);
}

Index Builder::event_trigger_nb_event(Index handle, Signal sig, uint32_t edge, uint32_t count,
                                      SourceLoc loc) {
    const ExtraIndex extra = add_extra(EventDetachInfo{ sig, edge, count });
    return emit({ Inst::Tag::EventTriggerNbEvent, Inst::Payload{ .op_extra = { handle, extra } } }, loc);
}

Index Builder::event_trigger_nb_delayed(Index handle, Index delay, SourceLoc loc) {
    return emit({ Inst::Tag::EventTriggerNbDelayed, Inst::Payload{ .binary = { delay, handle } } }, loc);
}

Index Builder::unary(Inst::Tag tag, Index op, SourceLoc loc) {
    return emit({ tag, pay(&Inst::Payload::unary, op) }, loc);
}

Index Builder::binary(Inst::Tag tag, Index l, Index r, SourceLoc loc) {
    return emit({ tag, Inst::Payload{ .binary = { l, r } } }, loc);
}

Index Builder::store_nb_delay(Index lvalue, Index value, Index delay, SourceLoc loc) {
    const ExtraIndex extra = add_extra(StoreNbDelayInfo{ value, delay });

    return emit({ Inst::Tag::StoreNbDelay, Inst::Payload{ .op_extra = { lvalue, extra } } }, loc);
}

Index Builder::store_inertial_delay(Index lvalue, Index value, Index delay, SourceLoc loc) {
    const ExtraIndex extra = add_extra(StoreNbDelayInfo{ value, delay });

    return emit({ Inst::Tag::StoreInertialDelay, Inst::Payload{ .op_extra = { lvalue, extra } } }, loc);
}

Index Builder::store_nb_event(Index lvalue, Index value, Index count, Signal trig, uint32_t edge, SourceLoc loc) {
    const ExtraIndex extra = add_extra(StoreNbEventInfo{ value, count, trig, edge });

    return emit({ Inst::Tag::StoreNbEvent, Inst::Payload{ .op_extra = { lvalue, extra } } }, loc);
}

Index Builder::resize(Index op, Ref type, SourceLoc loc) {
    return emit({ Inst::Tag::Resize, Inst::Payload{ .op_type = { op, type } } }, loc);
}

Index Builder::float_cvt(Inst::Tag tag, Index op, Ref type, SourceLoc loc) {
    return emit({ tag, Inst::Payload{ .op_type = { op, type } } }, loc);
}

Index Builder::real_math(RealMathOp op, std::span<const Index> args, SourceLoc loc) {
    if (args.size() == 2) {
        const ExtraIndex extra = add_extra(RealMathBinaryInfo{ args[1], static_cast<uint32_t>(op) });

        return emit({ Inst::Tag::RealMathBinary, Inst::Payload{ .op_extra = { args[0], extra } } }, loc);
    }
    return emit({ Inst::Tag::RealMathUnary, Inst::Payload{ .op_imm = { args[0], static_cast<uint32_t>(op) } } }, loc);
}

Index Builder::repl(Index value, uint32_t count, SourceLoc loc) {
    return emit({ Inst::Tag::Repl, Inst::Payload{ .repl = { value, count } } }, loc);
}

Index Builder::stream_reverse(Index op, uint32_t slice, SourceLoc loc) {
    return emit({ Inst::Tag::StreamReverse, Inst::Payload{ .op_imm = { op, slice } } }, loc);
}

Index Builder::net_pull(Index op, uint32_t pull, SourceLoc loc) {
    return emit({ Inst::Tag::NetPull, Inst::Payload{ .op_imm = { op, pull } } }, loc);
}

Index Builder::strength_res(std::span<const Index> slots, std::span<const uint16_t> strengths, SourceLoc loc) {
    std::vector<uint32_t> packed(strengths.begin(), strengths.end());
    const StrengthResInfo info{ .values = add_slice(slots), .strengths = add_u32_slice(packed) };
    const ExtraIndex      extra = add_extra(info);

    return emit({ Inst::Tag::StrengthRes, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::select(Index cond, Index a, Index b, SourceLoc loc) {
    const ExtraIndex extra = add_extra(Select{ a, b });

    return emit({ Inst::Tag::Select, Inst::Payload{ .op_extra = { cond, extra } } }, loc);
}

Index Builder::concat(std::span<const Index> parts, SourceLoc loc) {
    const ExtraSlice slice = add_slice(parts);
    const ExtraIndex extra = add_extra(slice);

    return emit({ Inst::Tag::Concat, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::part_select(Index base, uint32_t msb, uint32_t lsb, SourceLoc loc) {
    const ExtraIndex extra = add_extra(PartSelect{ msb, lsb });

    return emit({ Inst::Tag::PartSelect, Inst::Payload{ .op_extra = { base, extra } } }, loc);
}

Index Builder::dyn_part_select(Index base, Index offset, uint32_t width, SourceLoc loc) {
    const ExtraIndex extra = add_extra(DynPartSelect{ offset, width });

    return emit({ Inst::Tag::DynPartSelect, Inst::Payload{ .op_extra = { base, extra } } }, loc);
}

Index Builder::elem_select(Index base, Index index, Ref array_type, SourceLoc loc, bool for_write) {
    const ExtraIndex extra = add_extra(ElemSelect{ index, array_type, for_write ? 1u : 0u });

    return emit({ Inst::Tag::ElemSelect, Inst::Payload{ .op_extra = { base, extra } } }, loc);
}

Index Builder::arr_slice(Index base, Ref slice_type, uint32_t first_pos, SourceLoc loc) {
    const ExtraIndex extra = add_extra(ArrSlice{ slice_type, first_pos });

    return emit({ Inst::Tag::ArrSlice, Inst::Payload{ .op_extra = { base, extra } } }, loc);
}

Index Builder::member_select(Index base, uint32_t field_index, Ref struct_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(MemberSelect{ field_index, struct_type });

    return emit({ Inst::Tag::MemberSelect, Inst::Payload{ .op_extra = { base, extra } } }, loc);
}

Index Builder::iface_ref(uint32_t scope, SourceLoc loc) {
    const ExtraIndex extra = add_extra(IfaceInst{ scope });

    return emit({ Inst::Tag::IfaceRef, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::iface_signal(Index handle, Signal rep, SourceLoc loc) {
    const ExtraIndex extra = add_extra(IfaceMember{ rep });

    return emit({ Inst::Tag::IfaceSignal, Inst::Payload{ .op_extra = { handle, extra } } }, loc);
}

Index Builder::class_new(Ref class_type, SourceLoc loc) {
    return emit({ Inst::Tag::ClassNew, Inst::Payload{ .ref = class_type } }, loc);
}

Index Builder::handle_deref(Index handle, Ref class_type, SourceLoc loc) {
    return emit({ Inst::Tag::HandleDeref, Inst::Payload{ .op_type = { handle, class_type } } }, loc);
}

Index Builder::class_is_a(Index handle, Ref class_type, SourceLoc loc) {
    return emit({ Inst::Tag::ClassIsA, Inst::Payload{ .op_type = { handle, class_type } } }, loc);
}

Index Builder::agg_copy(Index dst, Index src, Ref type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(AggCopyInfo{ src, type });

    return emit({ Inst::Tag::AggCopy, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::agg_eq(Index lhs, Index rhs, Ref type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(AggEqInfo{ rhs, type });

    return emit({ Inst::Tag::AggEq, Inst::Payload{ .op_extra = { lhs, extra } } }, loc);
}

Index Builder::dyn_array_new(Index arr, Index size, Index init, bool has_init, Ref array_type, SourceLoc loc) {
    if (has_init) {
        const ExtraIndex extra = add_extra(DynArrayNewInitInfo{ size, init, array_type });

        return emit({ Inst::Tag::DynArrayNewInit, Inst::Payload{ .op_extra = { arr, extra } } }, loc);
    }
    const ExtraIndex extra = add_extra(DynArrayArg{ size, array_type });

    return emit({ Inst::Tag::DynArrayNew, Inst::Payload{ .op_extra = { arr, extra } } }, loc);
}

Index Builder::dyn_array_size(Index arr, Ref array_type, SourceLoc loc) {
    return emit({ Inst::Tag::DynArraySize, Inst::Payload{ .op_type = { arr, array_type } } }, loc);
}

Index Builder::dyn_array_delete(Index arr, Ref array_type, SourceLoc loc) {
    return emit({ Inst::Tag::DynArrayDelete, Inst::Payload{ .op_type = { arr, array_type } } }, loc);
}

Index Builder::dyn_array_copy(Index dst, Index src, Ref array_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(DynArrayArg{ src, array_type });

    return emit({ Inst::Tag::DynArrayCopy, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::dyn_array_reverse(Index arr, Ref array_type, SourceLoc loc) {
    return emit({ Inst::Tag::DynArrayReverse, Inst::Payload{ .op_type = { arr, array_type } } }, loc);
}

Index Builder::dyn_array_sort(Index arr, bool reversed, Ref array_type, SourceLoc loc) {
    const Inst::Tag tag = reversed ? Inst::Tag::DynArrayRsort : Inst::Tag::DynArraySort;
    return emit({ tag, Inst::Payload{ .op_type = { arr, array_type } } }, loc);
}

Index Builder::dyn_array_shuffle(Index arr, Ref array_type, SourceLoc loc) {
    return emit({ Inst::Tag::DynArrayShuffle, Inst::Payload{ .op_type = { arr, array_type } } }, loc);
}

Index Builder::queue_size(Index q, Ref queue_type, SourceLoc loc) {
    return emit({ Inst::Tag::QueueSize, Inst::Payload{ .op_type = { q, queue_type } } }, loc);
}

Index Builder::queue_push(Index q, Index elem, bool front, Ref queue_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(QueueArg{ elem, queue_type });

    return emit(
      { front ? Inst::Tag::QueuePushFront : Inst::Tag::QueuePushBack, Inst::Payload{ .op_extra = { q, extra } } }, loc);
}

Index Builder::queue_pop(Index q, bool front, Ref queue_type, SourceLoc loc) {
    return emit(
      { front ? Inst::Tag::QueuePopFront : Inst::Tag::QueuePopBack, Inst::Payload{ .op_type = { q, queue_type } } },
      loc);
}

Index Builder::queue_insert(Index q, Index idx, Index elem, Ref queue_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(QueueInsertInfo{ idx, elem, queue_type });

    return emit({ Inst::Tag::QueueInsert, Inst::Payload{ .op_extra = { q, extra } } }, loc);
}

Index Builder::queue_delete(Index q, Index idx, bool has_idx, Ref queue_type, SourceLoc loc) {
    if (!has_idx) {
        return emit({ Inst::Tag::QueueDeleteAll, Inst::Payload{ .op_type = { q, queue_type } } }, loc);
    }
    const ExtraIndex extra = add_extra(QueueArg{ idx, queue_type });

    return emit({ Inst::Tag::QueueDeleteIdx, Inst::Payload{ .op_extra = { q, extra } } }, loc);
}

Index Builder::queue_copy(Index dst, Index src, Ref queue_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(QueueArg{ src, queue_type });

    return emit({ Inst::Tag::QueueCopy, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::handle_copy_nb(Index dst, Index src, Ref queue_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(QueueArg{ src, queue_type });

    return emit({ Inst::Tag::HandleCopyNb, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::queue_copy_dynarr(Index dst, Index src, Ref queue_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(QueueArg{ src, queue_type });

    return emit({ Inst::Tag::QueueCopyDyn, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::queue_append_range(Index dst, Index src, Index lo, Index hi, Ref queue_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(QueueSliceInfo{ src, lo, hi, queue_type });

    return emit({ Inst::Tag::QueueAppendRange, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::queue_reverse(Index q, Ref queue_type, SourceLoc loc) {
    return emit({ Inst::Tag::QueueReverse, Inst::Payload{ .op_type = { q, queue_type } } }, loc);
}

Index Builder::queue_sort(Index q, bool reversed, Ref queue_type, SourceLoc loc) {
    const Inst::Tag tag = reversed ? Inst::Tag::QueueRsort : Inst::Tag::QueueSort;
    return emit({ tag, Inst::Payload{ .op_type = { q, queue_type } } }, loc);
}

Index Builder::queue_shuffle(Index q, Ref queue_type, SourceLoc loc) {
    return emit({ Inst::Tag::QueueShuffle, Inst::Payload{ .op_type = { q, queue_type } } }, loc);
}

Index Builder::assoc_select(Index a, Index key, Ref assoc_type, SourceLoc loc, bool for_write) {
    const ExtraIndex extra = add_extra(AssocArg{ key, assoc_type, for_write ? 1u : 0u });

    return emit({ Inst::Tag::AssocSelect, Inst::Payload{ .op_extra = { a, extra } } }, loc);
}

Index Builder::assoc_num(Index a, Ref assoc_type, SourceLoc loc) {
    return emit({ Inst::Tag::AssocNum, Inst::Payload{ .op_type = { a, assoc_type } } }, loc);
}

Index Builder::assoc_exists(Index a, Index key, Ref assoc_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(AssocArg{ key, assoc_type });

    return emit({ Inst::Tag::AssocExists, Inst::Payload{ .op_extra = { a, extra } } }, loc);
}

Index Builder::assoc_delete(Index a, Index key, bool has_key, Ref assoc_type, SourceLoc loc) {
    if (!has_key) {
        return emit({ Inst::Tag::AssocDeleteAll, Inst::Payload{ .op_type = { a, assoc_type } } }, loc);
    }
    const ExtraIndex extra = add_extra(AssocArg{ key, assoc_type });

    return emit({ Inst::Tag::AssocDeleteKey, Inst::Payload{ .op_extra = { a, extra } } }, loc);
}

Index Builder::assoc_default(Index a, Ref assoc_type, SourceLoc loc) {
    return emit({ Inst::Tag::AssocDefault, Inst::Payload{ .op_type = { a, assoc_type } } }, loc);
}

Index Builder::assoc_reset(Index a, Ref assoc_type, SourceLoc loc) {
    return emit({ Inst::Tag::AssocReset, Inst::Payload{ .op_type = { a, assoc_type } } }, loc);
}

Index Builder::assoc_copy(Index dst, Index src, Ref assoc_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(AssocArg{ src, assoc_type });

    return emit({ Inst::Tag::AssocCopy, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::assoc_traverse(Index a, Index key, AssocStep which, Ref assoc_type, SourceLoc loc) {
    Inst::Tag tag = Inst::Tag::AssocFirst;
    switch (which) {
        case AssocStep::First:
            tag = Inst::Tag::AssocFirst;
            break;
        case AssocStep::Last:
            tag = Inst::Tag::AssocLast;
            break;
        case AssocStep::Next:
            tag = Inst::Tag::AssocNext;
            break;
        case AssocStep::Prev:
            tag = Inst::Tag::AssocPrev;
            break;
    }
    const ExtraIndex extra = add_extra(AssocArg{ key, assoc_type });

    return emit({ tag, Inst::Payload{ .op_extra = { a, extra } } }, loc);
}

Index Builder::cond(Index condition, ExtraSlice then_region, ExtraSlice else_region, SourceLoc loc, bool unlikely) {
    const ExtraIndex extra = add_extra(Regions{ then_region, else_region, unlikely ? 1u : 0u });

    return emit({ Inst::Tag::Cond, Inst::Payload{ .op_extra = { condition, extra } } }, loc);
}

Index Builder::switch_(Index                           selector,
                       std::span<const SwitchArmBuild> arms,
                       ExtraSlice                      default_region,
                       uint32_t                        sel_width,
                       uint32_t                        sel_four,
                       uint32_t                        pragmas,
                       SourceLoc                       loc) {
    // Lay the arm records after their keys so every ExtraSlice is an absolute
    // offset: each arm's keys (u64 -> two words) first, then the SwitchArm
    // records contiguously, then the SwitchInfo referencing the record run.
    std::vector<SwitchArm> records;
    records.reserve(arms.size());
    for (const SwitchArmBuild& arm : arms) {
        const auto keys_start = static_cast<ExtraIndex>(extra_.size());
        for (uint64_t k : arm.keys) {
            extra_.push_back(static_cast<uint32_t>(k));
            extra_.push_back(static_cast<uint32_t>(k >> 32));
        }
        const auto keys_end = static_cast<ExtraIndex>(extra_.size());
        records.push_back(SwitchArm{ ExtraSlice{ keys_start, keys_end }, arm.body });
    }

    const auto arms_start = static_cast<ExtraIndex>(extra_.size());
    for (const SwitchArm& rec : records) {
        add_extra(rec);
    }
    const auto arms_end = static_cast<ExtraIndex>(extra_.size());

    const ExtraIndex extra =
      add_extra(SwitchInfo{ ExtraSlice{ arms_start, arms_end }, default_region, sel_width, sel_four, pragmas });

    return emit({ Inst::Tag::Switch, Inst::Payload{ .op_extra = { selector, extra } } }, loc);
}

Index Builder::select_lazy(Index      pred,
                           ExtraSlice then_region,
                           Index      then_val,
                           ExtraSlice else_region,
                           Index      else_val,
                           SourceLoc  loc) {
    const ExtraIndex extra = add_extra(SelectLazy{ then_region, then_val, else_region, else_val });

    return emit({ Inst::Tag::SelectLazy, Inst::Payload{ .op_extra = { pred, extra } } }, loc);
}

Index Builder::loop(ExtraSlice cond_region,
                    Index      pred,
                    ExtraSlice body_region,
                    ExtraSlice step_region,
                    bool       bottom_test,
                    SourceLoc  loc) {
    const ExtraIndex extra = add_extra(LoopInfo{ cond_region, pred, body_region, step_region, bottom_test ? 1u : 0u });

    return emit({ Inst::Tag::Loop, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::break_(SourceLoc loc) {
    return emit({ Inst::Tag::Break, pay(&Inst::Payload::none, 0u) }, loc);
}

Index Builder::ret_void(SourceLoc loc) {
    return emit({ Inst::Tag::RetVoid, pay(&Inst::Payload::none, 0u) }, loc);
}

Index Builder::continue_(SourceLoc loc) {
    return emit({ Inst::Tag::Continue, pay(&Inst::Payload::none, 0u) }, loc);
}

Index Builder::named_block(ExtraSlice body_region, uint32_t label, bool remote, SourceLoc loc) {
    const ExtraIndex extra = add_extra(BlockInfo{ body_region, label, remote ? 1u : 0u });

    return emit({ Inst::Tag::NamedBlock, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::disable(uint32_t label, SourceLoc loc) {
    return emit({ Inst::Tag::Disable, pay(&Inst::Payload::label, label) }, loc);
}

Index Builder::disable_remote(uint32_t target, SourceLoc loc) {
    return emit({ Inst::Tag::DisableRemote, pay(&Inst::Payload::label, target) }, loc);
}

Index Builder::defer_flush(uint32_t target, SourceLoc loc) {
    return emit({ Inst::Tag::DeferFlush, pay(&Inst::Payload::label, target) }, loc);
}

Index Builder::disable_abort(SourceLoc loc) {
    return emit({ Inst::Tag::DisableAbort, Inst::Payload{ .none = 0 } }, loc);
}

Index Builder::str_putc(Index str, Index index, Index ch, SourceLoc loc) {
    const ExtraIndex extra = add_extra(StrPutcInfo{ index, ch });

    return emit({ Inst::Tag::StrPutc, Inst::Payload{ .op_extra = { str, extra } } }, loc);
}

Index Builder::str_substr(Index str, Index i, Index j, SourceLoc loc) {
    const ExtraIndex extra = add_extra(StrSubstrInfo{ i, j });

    return emit({ Inst::Tag::StrSubstr, Inst::Payload{ .op_extra = { str, extra } } }, loc);
}

Index Builder::str_to_packed(Index str, Ref type, SourceLoc loc) {
    return emit({ Inst::Tag::StrToPacked, Inst::Payload{ .op_type = { str, type } } }, loc);
}

Index Builder::enum_name(Index val, std::span<const EnumNameRow> members, bool fmt_fallback,
                         SourceLoc loc) {
    // Pack each member's value + name bytes first, remembering where they landed,
    // then pack the entry records contiguously so they form one ExtraSlice. The
    // byte payloads and the records share the extra buffer but never overlap.
    struct Packed {
        ExtraIndex val_at;
        uint32_t   val_len;
        ExtraIndex name_at;
        uint32_t   name_len;
    };
    std::vector<Packed> packed;
    packed.reserve(members.size());
    for (const EnumNameRow& m : members) {
        const ExtraIndex vat =
          add_bytes(std::string_view(reinterpret_cast<const char*>(m.value.data()), m.value.size()));
        const ExtraIndex nat = add_bytes(m.name);
        packed.push_back({ vat, static_cast<uint32_t>(m.value.size()), nat, static_cast<uint32_t>(m.name.size()) });
    }

    const auto start = static_cast<ExtraIndex>(extra_.size());
    for (const Packed& p : packed) {
        add_extra(EnumNameEntry{ p.val_at, p.val_len, p.name_at, p.name_len });
    }
    const auto end = static_cast<ExtraIndex>(extra_.size());

    const ExtraIndex extra =
      add_extra(EnumNameInfo{ ExtraSlice{ start, end }, fmt_fallback ? 1u : 0u });

    return emit({ Inst::Tag::EnumName, Inst::Payload{ .op_extra = { val, extra } } }, loc);
}

Index Builder::tagged_check(Index                             tag,
                            uint32_t                          expected,
                            bool                              is_write,
                            std::string_view                  member,
                            std::span<const std::string_view> names,
                            SourceLoc                         loc) {
    // Same two-pass shape as enum_name: pack every name's bytes first, then the
    // fixed-size records, so the records form one contiguous ExtraSlice.
    const ExtraIndex member_at = add_bytes(member);

    std::vector<std::pair<ExtraIndex, uint32_t>> packed;
    packed.reserve(names.size());
    for (const std::string_view n : names) {
        packed.emplace_back(add_bytes(n), static_cast<uint32_t>(n.size()));
    }

    const auto start = static_cast<ExtraIndex>(extra_.size());
    for (const auto& [at, len] : packed) {
        add_extra(TaggedNameEntry{ at, len });
    }
    const auto end = static_cast<ExtraIndex>(extra_.size());

    const ExtraIndex extra = add_extra(TaggedCheckInfo{ expected,
                                                        is_write ? 1u : 0u,
                                                        member_at,
                                                        static_cast<uint32_t>(member.size()),
                                                        ExtraSlice{ start, end } });

    return emit({ Inst::Tag::TaggedCheck, Inst::Payload{ .op_extra = { tag, extra } } }, loc);
}

Index Builder::tagged_fmt_p(Index                         value,
                            uint32_t                      width,
                            uint32_t                      tag_bits,
                            std::span<const TaggedFmtRow> members,
                            SourceLoc                     loc) {
    // Names first, then the fixed-size records, so the records form one slice.
    std::vector<std::pair<ExtraIndex, uint32_t>> packed;
    packed.reserve(members.size());
    for (const TaggedFmtRow& m : members) {
        packed.emplace_back(add_bytes(m.name), static_cast<uint32_t>(m.name.size()));
    }

    const auto start = static_cast<ExtraIndex>(extra_.size());
    for (size_t k = 0; k < members.size(); ++k) {
        add_extra(TaggedFmtEntry{ packed[k].first, packed[k].second, members[k].width,
                                  members[k].is_signed ? 1u : 0u });
    }
    const auto end = static_cast<ExtraIndex>(extra_.size());

    const ExtraIndex extra = add_extra(TaggedFmtInfo{ width, tag_bits, ExtraSlice{ start, end } });

    return emit({ Inst::Tag::TaggedFmtP, Inst::Payload{ .op_extra = { value, extra } } }, loc);
}

Index Builder::count_bits(Index op, uint32_t mask, SourceLoc loc) {
    const ExtraIndex extra = add_extra(CountBitsInfo{ mask });

    return emit({ Inst::Tag::CountBits, Inst::Payload{ .op_extra = { op, extra } } }, loc);
}

Index Builder::inside_set(Index sel, std::span<const Index> values, std::span<const Index> ranges, SourceLoc loc) {
    const ExtraSlice vals  = add_slice(values);
    const ExtraSlice rngs  = add_slice(ranges);
    const ExtraIndex extra = add_extra(InsideSetInfo{ vals, rngs });

    return emit({ Inst::Tag::InsideSet, Inst::Payload{ .op_extra = { sel, extra } } }, loc);
}

Index Builder::wait(std::span<const WaitEdge> events, SourceLoc loc) {
    // Pack the WaitEdge records back-to-back, then a WaitInfo pointing at the run.
    const auto start = static_cast<uint32_t>(extra_.size());
    for (const WaitEdge& e : events) {
        constexpr size_t n     = sizeof(WaitEdge) / sizeof(uint32_t);
        const auto       words = std::bit_cast<std::array<uint32_t, n>>(e);
        extra_.insert(extra_.end(), words.begin(), words.end());
    }
    const auto       end = static_cast<uint32_t>(extra_.size());
    const ExtraIndex info =
      add_extra(WaitInfo{ ExtraSlice{ static_cast<ExtraIndex>(start), static_cast<ExtraIndex>(end) } });

    return emit({ Inst::Tag::Wait, pay(&Inst::Payload::extra, info) }, loc);
}

Index Builder::call(FunctionId func, std::span<const Index> args, SourceLoc loc) {
    const ExtraSlice slice = add_slice(args);
    const ExtraIndex extra = add_extra(CallInfo{ func, slice });

    return emit({ Inst::Tag::Call, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::call_virtual(FunctionId func, uint32_t slot, Ref class_ref, std::span<const Index> args, SourceLoc loc) {
    const ExtraSlice slice = add_slice(args);
    const ExtraIndex extra = add_extra(VirtualCallInfo{ func, slot, class_ref, slice });

    return emit({ Inst::Tag::CallVirtual, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::task_call(FunctionId func, std::span<const Index> args, SourceLoc loc) {
    const ExtraSlice slice = add_slice(args);
    const ExtraIndex extra = add_extra(TaskCallInfo{ func, slice });

    return emit({ Inst::Tag::TaskCall, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::task_call_virtual(FunctionId             func,
                                 uint32_t               slot,
                                 Ref                    class_ref,
                                 std::span<const Index> args,
                                 SourceLoc              loc) {
    const ExtraSlice slice = add_slice(args);
    const ExtraIndex extra = add_extra(VirtualCallInfo{ func, slot, class_ref, slice });

    return emit({ Inst::Tag::TaskCallVirtual, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::fork(std::span<const FunctionId> children,
                    std::span<const Index>      captures,
                    ForkKind                    kind,
                    SourceLoc                   loc) {
    // FunctionId is a u32 enum; store the child ids in the extra arena as raw u32s
    // (the same encoding add_slice uses for Index operands).
    const auto start = static_cast<ExtraIndex>(extra_.size());
    for (const FunctionId c : children) {
        extra_.push_back(static_cast<uint32_t>(c));
    }
    const ExtraSlice slice = { start, static_cast<ExtraIndex>(extra_.size()) };
    // Appended after the children, so the two runs never overlap.
    const ExtraSlice args  = add_slice(captures);
    const ExtraIndex extra = add_extra(ForkInfo{ slice, args, kind });

    return emit({ Inst::Tag::Fork, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::proc_query(ProcOp op, std::span<const Index> args, Ref type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(ProcCtlInfo{ add_slice(args), type, op });
    return emit({ Inst::Tag::ProcQuery, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::proc_ctl(ProcOp op, std::span<const Index> args, SourceLoc loc) {
    const ExtraIndex extra = add_extra(ProcCtlInfo{ add_slice(args), Ref::Invalid, op });
    return emit({ Inst::Tag::ProcCtl, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::proc_wait(ProcOp op, std::span<const Index> args, SourceLoc loc) {
    const ExtraIndex extra = add_extra(ProcCtlInfo{ add_slice(args), Ref::Invalid, op });
    return emit({ Inst::Tag::ProcWait, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::sync_wait(uint32_t site, std::span<const Index> args, SourceLoc loc) {
    const ExtraIndex extra = add_extra(SyncInfo{ add_slice(args), site, SyncOp::Park });
    return emit({ Inst::Tag::SyncWait, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::sync_ctl(SyncOp op, uint32_t site, std::span<const Index> args, SourceLoc loc) {
    const ExtraIndex extra = add_extra(SyncInfo{ add_slice(args), site, op });
    return emit({ Inst::Tag::SyncCtl, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::display(std::string_view fmt, bool newline, std::span<const Index> args, SourceLoc loc) {
    // $display / $write: no descriptor operand; the backend targets standard output.
    return emit({ Inst::Tag::Display, pay(&Inst::Payload::extra, display_info(fmt, newline, args)) }, loc);
}

Index Builder::fdisplay(Index fd, std::string_view fmt, bool newline, std::span<const Index> args, SourceLoc loc) {
    // $fdisplay / $fwrite: `fd` selects the destination descriptor.
    return emit({ Inst::Tag::FDisplay, Inst::Payload{ .op_extra = { fd, display_info(fmt, newline, args) } } }, loc);
}

Index Builder::strobe_pend(FunctionId func, SourceLoc loc) {
    return emit({ Inst::Tag::StrobePend, pay(&Inst::Payload::extra, add_extra(StrobePendInfo{ func })) }, loc);
}

Index Builder::reactive_pend(FunctionId func, SourceLoc loc) {
    return emit({ Inst::Tag::ReactivePend, pay(&Inst::Payload::extra, add_extra(StrobePendInfo{ func })) }, loc);
}

Index Builder::expect_wait(uint32_t monitor, SourceLoc loc) {
    return emit({ Inst::Tag::ExpectWait, pay(&Inst::Payload::extra, add_extra(ExpectInfo{ monitor })) }, loc);
}

Index Builder::expect_wake(Index resolved, uint32_t monitor, SourceLoc loc) {
    return emit({ Inst::Tag::ExpectWake, Inst::Payload{ .op_imm = { resolved, monitor } } }, loc);
}

Index Builder::monitor_set(FunctionId func, SourceLoc loc) {
    return emit({ Inst::Tag::MonitorSet, pay(&Inst::Payload::extra, add_extra(StrobePendInfo{ func })) }, loc);
}

Index Builder::monitor_emit(Index fd, std::string_view fmt, std::span<const Index> args, SourceLoc loc) {
    return emit(
      { Inst::Tag::MonitorEmit, Inst::Payload{ .op_extra = { fd, display_info(fmt, /*newline=*/true, args) } } }, loc);
}

Index Builder::monitor_enable(bool on, SourceLoc loc) {
    return emit({ on ? Inst::Tag::MonitorOn : Inst::Tag::MonitorOff, Inst::Payload{} }, loc);
}

ExtraIndex Builder::display_info(std::string_view fmt, bool newline, std::span<const Index> args) {
    // Order in the extra buffer: format bytes, then the arg-index slice, then the
    // DisplayInfo record pointing at both. None of the three regions overlap.
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);

    return add_extra(DisplayInfo{ fmt_at, static_cast<uint32_t>(fmt.size()), newline ? 1u : 0u, slice });
}

Index Builder::fopen_fd(Index name, Index mode, SourceLoc loc) {
    return emit({ Inst::Tag::FopenFd, Inst::Payload{ .binary = { name, mode } } }, loc);
}

Index Builder::fopen_mcd(Index name, SourceLoc loc) {
    return emit({ Inst::Tag::FopenMcd, pay(&Inst::Payload::unary, name) }, loc);
}

Index Builder::fclose(Index handle, SourceLoc loc) {
    return emit({ Inst::Tag::Fclose, pay(&Inst::Payload::unary, handle) }, loc);
}

Index Builder::fseek(Index fd, Index offset, Index operation, SourceLoc loc) {
    const ExtraIndex extra = add_extra(FseekInfo{ offset, operation });

    return emit({ Inst::Tag::Fseek, Inst::Payload{ .op_extra = { fd, extra } } }, loc);
}

Index Builder::fgets(Index dst, Index fd, SourceLoc loc) {
    return emit({ Inst::Tag::Fgets, Inst::Payload{ .binary = { dst, fd } } }, loc);
}

Index Builder::sformat(Index dst, std::string_view fmt, std::span<const Index> args, SourceLoc loc) {
    // Order in the extra buffer: format bytes, then the arg-index slice, then the
    // SformatInfo record pointing at both.
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);
    const ExtraIndex extra  = add_extra(SformatInfo{ dst, fmt_at, static_cast<uint32_t>(fmt.size()), slice });

    return emit({ Inst::Tag::Sformat, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::sformatf(std::string_view fmt, std::span<const Index> args, SourceLoc loc) {
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);
    const ExtraIndex extra  = add_extra(SformatfInfo{ fmt_at, static_cast<uint32_t>(fmt.size()), slice });

    return emit({ Inst::Tag::Sformatf, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::container_fmt_p(Index handle, Ref container_type, SourceLoc loc) {
    return emit({ Inst::Tag::ContainerFmtP, Inst::Payload{ .op_type = { handle, container_type } } }, loc);
}

Index Builder::ferror(Index dst, Index fd, SourceLoc loc) {
    return emit({ Inst::Tag::Ferror, Inst::Payload{ .binary = { dst, fd } } }, loc);
}

Index Builder::system(Index cmd, uint32_t has_arg, SourceLoc loc) {
    return emit({ Inst::Tag::System, Inst::Payload{ .op_imm = { cmd, has_arg } } }, loc);
}

Index Builder::fread_word(Index dst, Index fd, SourceLoc loc) {
    return emit({ Inst::Tag::FreadWord, Inst::Payload{ .binary = { dst, fd } } }, loc);
}

Index Builder::fread_mem(Index     fd,
                         Signal    mem,
                         Index     start,
                         Index     count,
                         bool      has_start,
                         bool      has_count,
                         SourceLoc loc) {
    const ExtraIndex extra = add_extra(FreadMemInfo{ mem, start, count, has_start ? 1u : 0u, has_count ? 1u : 0u });

    return emit({ Inst::Tag::FreadMem, Inst::Payload{ .op_extra = { fd, extra } } }, loc);
}

Index Builder::scanf(Index source, bool is_string, std::string_view fmt, std::span<const Index> args, SourceLoc loc) {
    // Order in the extra buffer: format bytes, then the output-lvalue slice, then
    // the ScanfInfo record pointing at both.
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);
    const ExtraIndex extra  = add_extra(ScanfInfo{ fmt_at, static_cast<uint32_t>(fmt.size()), source, slice });

    return emit({ is_string ? Inst::Tag::Sscanf : Inst::Tag::Fscanf, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::finish(std::string_view banner, uint32_t arg, SourceLoc loc) {
    const ExtraIndex banner_at = add_bytes(banner);
    const ExtraIndex extra     = add_extra(FinishInfo{ banner_at, static_cast<uint32_t>(banner.size()), arg });
    return emit({ Inst::Tag::Finish, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::case_violation(std::string_view banner, uint32_t kind, SourceLoc loc) {
    const ExtraIndex banner_at = add_bytes(banner);
    const ExtraIndex extra     = add_extra(CaseViolInfo{ banner_at, static_cast<uint32_t>(banner.size()), kind });
    return emit({ Inst::Tag::CaseViol, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::severity(uint32_t               level,
                        std::string_view       banner,
                        std::string_view       fmt,
                        std::span<const Index> args,
                        SourceLoc              loc,
                        uint32_t               defer) {
    // Order in the extra buffer: banner bytes, format bytes, then the arg-index
    // slice, then the SeverityInfo record pointing at all three.
    const ExtraIndex banner_at = add_bytes(banner);
    const ExtraIndex fmt_at    = add_bytes(fmt);
    const ExtraSlice slice     = add_slice(args);
    const ExtraIndex extra     = add_extra(SeverityInfo{ banner_at,
                                                         static_cast<uint32_t>(banner.size()),
                                                         fmt_at,
                                                         static_cast<uint32_t>(fmt.size()),
                                                         level,
                                                         defer,
                                                         slice });
    return emit({ Inst::Tag::Severity, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::fflush(Index handle, SourceLoc loc) {
    return emit({ Inst::Tag::Fflush, pay(&Inst::Payload::unary, handle) }, loc);
}

Index Builder::read_mem(Signal    target,
                        Index     filename,
                        Index     start,
                        Index     finish,
                        bool      hex,
                        bool      has_start,
                        bool      has_finish,
                        SourceLoc loc) {
    const ExtraIndex extra =
      add_extra(ReadMemInfo{ target, start, finish, hex ? 1u : 0u, has_start ? 1u : 0u, has_finish ? 1u : 0u });

    return emit({ Inst::Tag::ReadMem, Inst::Payload{ .op_extra = { filename, extra } } }, loc);
}

Index Builder::timeformat(Index units, Index precision, std::string_view suffix, Index min_width, SourceLoc loc) {
    // Order in the extra buffer: suffix bytes, then the TimeFormatInfo record
    // pointing at them. The three operands are plain SSA indices in the record.
    const ExtraIndex suffix_at = add_bytes(suffix);
    const ExtraIndex extra =
      add_extra(TimeFormatInfo{ units, precision, min_width, suffix_at, static_cast<uint32_t>(suffix.size()) });

    return emit({ Inst::Tag::TimeFormat, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::dump_file(Index filename, SourceLoc loc) {
    return emit({ Inst::Tag::DumpFile, pay(&Inst::Payload::unary, filename) }, loc);
}

Index Builder::dump_vars(SourceLoc loc) {
    return emit({ Inst::Tag::DumpVars, pay(&Inst::Payload::none, 0u) }, loc);
}

Index Builder::dump_limit(Index bytes, SourceLoc loc) {
    return emit({ Inst::Tag::DumpLimit, pay(&Inst::Payload::unary, bytes) }, loc);
}

Index Builder::dump_control(Inst::Tag tag, SourceLoc loc) {
    return emit({ tag, pay(&Inst::Payload::none, 0u) }, loc);
}

Index Builder::sim_time(uint64_t scale, SourceLoc loc) {
    return emit({ Inst::Tag::SimTime, Inst::Payload{ .scale = scale } }, loc);
}

Index Builder::random(SourceLoc loc) {
    return emit({ Inst::Tag::Random, pay(&Inst::Payload::none, 0u) }, loc);
}

Index Builder::dist_call(DistOp op, std::span<const Index> args, SourceLoc loc) {
    if (args.size() == 3) {
        const ExtraIndex extra = add_extra(DistBinaryInfo{ args[1], args[2], static_cast<uint32_t>(op) });
        return emit({ Inst::Tag::DistBinary, Inst::Payload{ .op_extra = { args[0], extra } } }, loc);
    }
    const ExtraIndex extra = add_extra(DistUnaryInfo{ args[1], static_cast<uint32_t>(op) });
    return emit({ Inst::Tag::DistUnary, Inst::Payload{ .op_extra = { args[0], extra } } }, loc);
}

Index Builder::urandom(SourceLoc loc) {
    return emit({ Inst::Tag::Urandom, pay(&Inst::Payload::none, 0u) }, loc);
}

Index Builder::urandom_seeded(Index seed, SourceLoc loc) {
    return emit({ Inst::Tag::UrandomSeeded, pay(&Inst::Payload::unary, seed) }, loc);
}

Index Builder::urandom_range(Index maxval, Index minval, SourceLoc loc) {
    return emit({ Inst::Tag::UrandomRange, Inst::Payload{ .binary = { maxval, minval } } }, loc);
}

Index Builder::obj_random(Index rng_lv, SourceLoc loc) {
    return emit({ Inst::Tag::ObjRandom, pay(&Inst::Payload::unary, rng_lv) }, loc);
}

Index Builder::obj_rand_seed(Index rng_lv, Index seed, SourceLoc loc) {
    return emit({ Inst::Tag::ObjRandSeed, Inst::Payload{ .binary = { rng_lv, seed } } }, loc);
}

Index Builder::randc_next(Index seen_lv, Index rng_lv, uint32_t nvals, SourceLoc loc) {
    const ExtraIndex extra = add_extra(RandcInfo{ rng_lv, nvals });
    return emit({ Inst::Tag::RandcNext, Inst::Payload{ .op_extra = { seen_lv, extra } } }, loc);
}

Index Builder::test_plusargs(std::string_view str, SourceLoc loc) {
    const ExtraIndex str_at = add_bytes(str);
    const ExtraIndex extra  = add_extra(TestPlusArgsInfo{ str_at, static_cast<uint32_t>(str.size()) });
    return emit({ Inst::Tag::TestPlusArgs, pay(&Inst::Payload::extra, extra) }, loc);
}

Index Builder::value_plusargs(Index dst, std::string_view prefix, char conv, SourceLoc loc) {
    const ExtraIndex prefix_at = add_bytes(prefix);
    const ExtraIndex extra =
      add_extra(ValuePlusArgsInfo{ prefix_at, static_cast<uint32_t>(prefix.size()), static_cast<uint32_t>(conv) });
    return emit({ Inst::Tag::ValuePlusArgs, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

} // namespace clotho::ir
