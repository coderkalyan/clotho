#include <doctest/doctest.h>

#include "Frontend.h"

TEST_CASE("parse_source accepts a well-formed module") {
    const auto r = clotho::parse_source("module top; endmodule");
    CHECK(r.parsed);
    CHECK_FALSE(r.diagnostics);
}

TEST_CASE("parse_source flags a malformed module") {
    const auto r = clotho::parse_source("module top"); // missing endmodule
    CHECK(r.parsed);                                   // a (partial) tree is still produced
    CHECK(r.diagnostics);                              // but with diagnostics
}
