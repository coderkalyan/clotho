#include "Builder.h"

namespace clotho::ir {

Ir Builder::finish() {
    // Whatever remains in scratch_ is the root region.
    const ExtraSlice root = add_slice(scratch_);

    return Ir(pool_, std::move(tags_), std::move(payloads_), std::move(locs_), std::move(extra_), root);
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

Index Builder::constant(Ref value, SourceLoc loc) {
    return emit({ Inst::Tag::Constant, Inst::Payload{ .ref = value } }, loc);
}

Index Builder::arg(uint32_t position, Ref type, SourceLoc loc) {
    return emit({ Inst::Tag::Arg, Inst::Payload{ .arg = { position, type } } }, loc);
}

Index Builder::signal(Signal sig, SourceLoc loc) {
    return emit({ Inst::Tag::Signal, Inst::Payload{ .signal = sig } }, loc);
}

Index Builder::local(uint32_t index, Ref type, SourceLoc loc) {
    return emit({ Inst::Tag::Local, Inst::Payload{ .local = { index, type } } }, loc);
}

Index Builder::unary(Inst::Tag tag, Index op, SourceLoc loc) {
    return emit({ tag, Inst::Payload{ .unary = op } }, loc);
}

Index Builder::binary(Inst::Tag tag, Index l, Index r, SourceLoc loc) {
    return emit({ tag, Inst::Payload{ .binary = { l, r } } }, loc);
}

Index Builder::resize(Index op, Ref type, SourceLoc loc) {
    return emit({ Inst::Tag::Resize, Inst::Payload{ .op_type = { op, type } } }, loc);
}

Index Builder::repl(Index value, uint32_t count, SourceLoc loc) {
    return emit({ Inst::Tag::Repl, Inst::Payload{ .repl = { value, count } } }, loc);
}

Index Builder::mux(Index cond, Index a, Index b, SourceLoc loc) {
    const ExtraIndex extra = add_extra(Mux{ a, b });

    return emit({ Inst::Tag::Mux, Inst::Payload{ .op_extra = { cond, extra } } }, loc);
}

Index Builder::concat(std::span<const Index> parts, SourceLoc loc) {
    const ExtraSlice slice = add_slice(parts);
    const ExtraIndex extra = add_extra(slice);

    return emit({ Inst::Tag::Concat, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::part_select(Index base, uint32_t msb, uint32_t lsb, SourceLoc loc) {
    const ExtraIndex extra = add_extra(PartSelect{ msb, lsb });

    return emit({ Inst::Tag::PartSelect, Inst::Payload{ .op_extra = { base, extra } } }, loc);
}

Index Builder::dyn_part_select(Index base, Index offset, uint32_t width, SourceLoc loc) {
    const ExtraIndex extra = add_extra(DynPartSelect{ offset, width });

    return emit({ Inst::Tag::DynPartSelect, Inst::Payload{ .op_extra = { base, extra } } }, loc);
}

Index Builder::elem_select(Index base, Index index, Ref array_type, SourceLoc loc) {
    const ExtraIndex extra = add_extra(ElemSelect{ index, array_type });

    return emit({ Inst::Tag::ElemSelect, Inst::Payload{ .op_extra = { base, extra } } }, loc);
}

Index Builder::cond(Index condition, ExtraSlice then_region, ExtraSlice else_region, SourceLoc loc) {
    const ExtraIndex extra = add_extra(Regions{ then_region, else_region });

    return emit({ Inst::Tag::Cond, Inst::Payload{ .op_extra = { condition, extra } } }, loc);
}

Index Builder::loop(ExtraSlice cond_region, Index pred, ExtraSlice body_region, ExtraSlice step_region,
                    bool bottom_test, SourceLoc loc) {
    const ExtraIndex extra =
      add_extra(LoopInfo{ cond_region, pred, body_region, step_region, bottom_test });

    return emit({ Inst::Tag::Loop, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::break_(SourceLoc loc) {
    return emit({ Inst::Tag::Break, Inst::Payload{ .none = 0 } }, loc);
}

Index Builder::continue_(SourceLoc loc) {
    return emit({ Inst::Tag::Continue, Inst::Payload{ .none = 0 } }, loc);
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

Index Builder::wait(std::span<const WaitEdge> events, SourceLoc loc) {
    // Pack the WaitEdge records back-to-back, then a WaitInfo pointing at the run.
    const auto start = static_cast<uint32_t>(extra_.size());
    for (const WaitEdge& e : events) {
        constexpr size_t n     = sizeof(WaitEdge) / sizeof(uint32_t);
        const auto       words = std::bit_cast<std::array<uint32_t, n>>(e);
        extra_.insert(extra_.end(), words.begin(), words.end());
    }
    const auto       end  = static_cast<uint32_t>(extra_.size());
    const ExtraIndex info = add_extra(
      WaitInfo{ ExtraSlice{ static_cast<ExtraIndex>(start), static_cast<ExtraIndex>(end) } });

    return emit({ Inst::Tag::Wait, Inst::Payload{ .wait = info } }, loc);
}

Index Builder::call(FunctionId func, std::span<const Index> args, SourceLoc loc) {
    const ExtraSlice slice = add_slice(args);
    const ExtraIndex extra = add_extra(CallInfo{ func, slice });

    return emit({ Inst::Tag::Call, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::task_call(FunctionId func, std::span<const Index> args, SourceLoc loc) {
    const ExtraSlice slice = add_slice(args);
    const ExtraIndex extra = add_extra(TaskCallInfo{ func, slice });

    return emit({ Inst::Tag::TaskCall, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::display(std::string_view fmt, bool newline, std::span<const Index> args, SourceLoc loc) {
    // $display / $write: no descriptor operand; the backend targets standard output.
    return display_impl(static_cast<Index>(0), false, fmt, newline, args, loc);
}

Index Builder::fdisplay(Index fd, std::string_view fmt, bool newline, std::span<const Index> args,
                        SourceLoc loc) {
    // $fdisplay / $fwrite: `fd` selects the destination descriptor.
    return display_impl(fd, true, fmt, newline, args, loc);
}

Index Builder::display_impl(Index fd, bool has_fd, std::string_view fmt, bool newline,
                            std::span<const Index> args, SourceLoc loc) {
    // Order in the extra buffer: format bytes, then the arg-index slice, then the
    // DisplayInfo record pointing at both. None of the three regions overlap.
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);
    const ExtraIndex extra  = add_extra(DisplayInfo{
      fmt_at, static_cast<uint32_t>(fmt.size()), newline ? 1u : 0u, fd, has_fd ? 1u : 0u, slice });

    return emit({ Inst::Tag::Display, Inst::Payload{ .display = extra } }, loc);
}

Index Builder::fopen(Index name, Index mode, bool has_mode, SourceLoc loc) {
    const ExtraIndex extra = add_extra(FopenInfo{ mode, has_mode ? 1u : 0u });

    return emit({ Inst::Tag::Fopen, Inst::Payload{ .op_extra = { name, extra } } }, loc);
}

Index Builder::fclose(Index handle, SourceLoc loc) {
    return emit({ Inst::Tag::Fclose, Inst::Payload{ .unary = handle } }, loc);
}

Index Builder::file_value_op(FileOp op, std::span<const Index> args, SourceLoc loc) {
    const ExtraSlice slice = add_slice(args);
    const ExtraIndex extra = add_extra(FileOpInfo{ static_cast<uint32_t>(op), slice });

    return emit({ Inst::Tag::FileValueOp, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::fgets(Index dst, Index fd, SourceLoc loc) {
    const ExtraIndex extra = add_extra(FgetsInfo{ fd });

    return emit({ Inst::Tag::Fgets, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::sformat(Index dst, std::string_view fmt, std::span<const Index> args, SourceLoc loc) {
    // Order in the extra buffer: format bytes, then the arg-index slice, then the
    // SformatInfo record pointing at both.
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);
    const ExtraIndex extra = add_extra(SformatInfo{ dst, fmt_at, static_cast<uint32_t>(fmt.size()), slice });

    return emit({ Inst::Tag::Sformat, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::sformatf(std::string_view fmt, std::span<const Index> args, SourceLoc loc) {
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);
    const ExtraIndex extra  = add_extra(SformatfInfo{ fmt_at, static_cast<uint32_t>(fmt.size()), slice });

    return emit({ Inst::Tag::Sformatf, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::ferror(Index dst, Index fd, SourceLoc loc) {
    const ExtraIndex extra = add_extra(FerrorInfo{ fd });

    return emit({ Inst::Tag::Ferror, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::fread_word(Index dst, Index fd, SourceLoc loc) {
    const ExtraIndex extra = add_extra(FreadWordInfo{ fd });

    return emit({ Inst::Tag::FreadWord, Inst::Payload{ .op_extra = { dst, extra } } }, loc);
}

Index Builder::fread_mem(Index fd, Signal mem, Index start, Index count, bool has_start, bool has_count,
                         SourceLoc loc) {
    const ExtraIndex extra =
      add_extra(FreadMemInfo{ mem, start, count, has_start ? 1u : 0u, has_count ? 1u : 0u });

    return emit({ Inst::Tag::FreadMem, Inst::Payload{ .op_extra = { fd, extra } } }, loc);
}

Index Builder::scanf(Index source, bool is_string, std::string_view fmt, std::span<const Index> args,
                     SourceLoc loc) {
    // Order in the extra buffer: format bytes, then the output-lvalue slice, then
    // the ScanfInfo record pointing at both.
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);
    const ExtraIndex extra  = add_extra(
      ScanfInfo{ fmt_at, static_cast<uint32_t>(fmt.size()), source, is_string ? 1u : 0u, slice });

    return emit({ Inst::Tag::Scanf, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::finish(SourceLoc loc) {
    return emit({ Inst::Tag::Finish, Inst::Payload{ .none = 0 } }, loc);
}

Index Builder::fflush(Index handle, SourceLoc loc) {
    return emit({ Inst::Tag::Fflush, Inst::Payload{ .unary = handle } }, loc);
}

Index Builder::read_mem(Signal target, Index filename, Index start, Index finish, bool hex, bool has_start,
                        bool has_finish, SourceLoc loc) {
    const ExtraIndex extra = add_extra(ReadMemInfo{ target, start, finish, hex, has_start, has_finish });

    return emit({ Inst::Tag::ReadMem, Inst::Payload{ .op_extra = { filename, extra } } }, loc);
}

Index Builder::timeformat(Index units, Index precision, std::string_view suffix, Index min_width, SourceLoc loc) {
    // Order in the extra buffer: suffix bytes, then the TimeFormatInfo record
    // pointing at them. The three operands are plain SSA indices in the record.
    const ExtraIndex suffix_at = add_bytes(suffix);
    const ExtraIndex extra     = add_extra(
      TimeFormatInfo{ units, precision, min_width, suffix_at, static_cast<uint32_t>(suffix.size()) });

    return emit({ Inst::Tag::TimeFormat, Inst::Payload{ .timeformat = extra } }, loc);
}

Index Builder::sim_time(uint64_t scale, SourceLoc loc) {
    return emit({ Inst::Tag::SimTime, Inst::Payload{ .scale = scale } }, loc);
}

Index Builder::random(SourceLoc loc) {
    return emit({ Inst::Tag::Random, Inst::Payload{ .none = 0 } }, loc);
}

} // namespace clotho::ir
