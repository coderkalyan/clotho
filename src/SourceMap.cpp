#include "SourceMap.h"

namespace clotho {

namespace {
// slang::SourceLocation packs a 28-bit buffer id and a 36-bit char offset. We
// mirror that layout in the opaque 64-bit handle: [ buffer : 28 | offset : 36 ].
constexpr unsigned offset_bits = 36;
constexpr uint64_t offset_mask = (uint64_t{ 1 } << offset_bits) - 1;
} // namespace

ir::SourceLoc to_source_loc(slang::SourceLocation loc) {
    const uint64_t buffer = loc.buffer().getId();
    const uint64_t offset = loc.offset() & offset_mask;

    return static_cast<ir::SourceLoc>((buffer << offset_bits) | offset);
}

slang::SourceLocation to_slang_location(ir::SourceLoc loc) {
    const auto     bits   = static_cast<uint64_t>(loc);
    const auto     buffer = static_cast<uint32_t>(bits >> offset_bits);
    const uint64_t offset = bits & offset_mask;

    return slang::SourceLocation(slang::BufferID(buffer, ""), offset);
}

} // namespace clotho
