#include <doctest/doctest.h>

#include <slang/text/SourceLocation.h>

#include "SourceMap.h"

using namespace clotho;

TEST_CASE("slang source location round-trips through the opaque handle") {
    const slang::SourceLocation original(slang::BufferID(7, ""), 123456);
    const ir::SourceLoc         handle = to_source_loc(original);
    const slang::SourceLocation back   = to_slang_location(handle);

    CHECK(back.buffer().getId() == 7);
    CHECK(back.offset() == 123456);
    CHECK(back == original);
}
