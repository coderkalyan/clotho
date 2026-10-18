#include "lsp/LineIndex.h"

#include <algorithm>

namespace clotho::lsp {

namespace {

// Code units spanned by a UTF-8 byte range, in the negotiated encoding.
//
// UTF-16 counts a code unit per BMP character and two for anything above it. In
// UTF-8 those astral characters are exactly the four-byte sequences, whose lead
// byte is 0xF0-0xF7; continuation bytes (0b10xxxxxx) belong to a character
// already counted. Invalid UTF-8 degrades to one unit per non-continuation
// byte, which keeps the arithmetic monotonic rather than correct -- a buffer
// that is not valid UTF-8 has no correct answer, and a monotonic one at least
// cannot produce a range whose end precedes its start.
uint32_t count_units(std::string_view bytes, Encoding encoding) {
    if (encoding == Encoding::Utf8) {
        return static_cast<uint32_t>(bytes.size());
    }
    uint32_t units = 0;
    for (const unsigned char byte : bytes) {
        if ((byte & 0xC0) == 0x80) {
            continue;
        }
        units += byte >= 0xF0 ? 2 : 1;
    }
    return units;
}

} // namespace

std::string_view encoding_name(Encoding encoding) {
    return encoding == Encoding::Utf8 ? "utf-8" : "utf-16";
}

LineIndex::LineIndex(std::string_view text, Encoding encoding)
  : text_(text)
  , encoding_(encoding) {
    // One line start per newline, plus the first. A trailing newline therefore
    // produces a final empty line, which is what an editor shows.
    starts_.reserve(text.size() / 32 + 1);
    starts_.push_back(0);
    for (uint32_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            starts_.push_back(i + 1);
        }
    }
}

LineIndex::Point LineIndex::point(uint32_t offset) const {
    if (starts_.empty()) {
        return {};
    }
    offset = std::min<uint32_t>(offset, static_cast<uint32_t>(text_.size()));

    // upper_bound lands one past the line containing the offset, so the line is
    // its index minus one. starts_ always begins with 0, so it can never be
    // begin().
    const auto     it    = std::ranges::upper_bound(starts_, offset);
    const uint32_t line  = static_cast<uint32_t>(it - starts_.begin()) - 1;
    const uint32_t start = starts_[line];
    return Point{ .line = line, .character = count_units(text_.substr(start, offset - start), encoding_) };
}

uint32_t LineIndex::length_between(uint32_t begin, uint32_t end) const {
    begin = std::min<uint32_t>(begin, static_cast<uint32_t>(text_.size()));
    end   = std::clamp<uint32_t>(end, begin, static_cast<uint32_t>(text_.size()));
    return count_units(text_.substr(begin, end - begin), encoding_);
}

} // namespace clotho::lsp
