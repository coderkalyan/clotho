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

Index Builder::cond(Index condition, ExtraSlice then_region, ExtraSlice else_region, SourceLoc loc) {
    const ExtraIndex extra = add_extra(Regions{ then_region, else_region });

    return emit({ Inst::Tag::Cond, Inst::Payload{ .op_extra = { condition, extra } } }, loc);
}

Index Builder::loop(ExtraSlice cond_region, Index pred, ExtraSlice body_region, ExtraSlice step_region,
                    SourceLoc loc) {
    const ExtraIndex extra = add_extra(LoopInfo{ cond_region, pred, body_region, step_region });

    return emit({ Inst::Tag::Loop, Inst::Payload{ .extra = extra } }, loc);
}

Index Builder::wait(Signal sig, uint32_t edge, SourceLoc loc) {
    return emit({ Inst::Tag::Wait, Inst::Payload{ .wait = { sig, edge } } }, loc);
}

Index Builder::display(std::string_view fmt, bool newline, std::span<const Index> args, SourceLoc loc) {
    // Order in the extra buffer: format bytes, then the arg-index slice, then the
    // DisplayInfo record pointing at both. None of the three regions overlap.
    const ExtraIndex fmt_at = add_bytes(fmt);
    const ExtraSlice slice  = add_slice(args);
    const ExtraIndex extra =
      add_extra(DisplayInfo{ fmt_at, static_cast<uint32_t>(fmt.size()), newline ? 1u : 0u, slice });

    return emit({ Inst::Tag::Display, Inst::Payload{ .display = extra } }, loc);
}

} // namespace clotho::ir
