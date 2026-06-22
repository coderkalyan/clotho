#include "Builder.h"

namespace clotho::ir {

Ir Builder::finish() {
    return Ir(pool_, std::move(tags_), std::move(payloads_), std::move(locs_), std::move(extra_));
}

Index Builder::constant(Ref value, SourceLoc loc) {
    return emit({ Inst::Tag::constant, Inst::Payload{ .ref = value } }, loc);
}

Index Builder::add(Index l, Index r, SourceLoc loc) {
    return emit({ Inst::Tag::add, Inst::Payload{ .binary = { l, r } } }, loc);
}

Index Builder::call(Index callee, std::span<const Index> args, SourceLoc loc) {
    const ExtraSlice slice = addSlice(args);
    const ExtraIndex extra = addExtra(slice);

    return emit({ Inst::Tag::call, Inst::Payload{ .unary_extra = { callee, extra } } }, loc);
}

Index Builder::ret(Index value, SourceLoc loc) {
    return emit({ Inst::Tag::ret, Inst::Payload{ .unary = value } }, loc);
}

} // namespace clotho::ir
