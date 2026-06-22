#include <print>

#include "Frontend.h"

// Thin driver: all compiler logic lives in libclotho.
int main() {
    const auto result = clotho::parseSource(R"(
        module top;
            logic a, b, y;
            assign y = a & b;
        endmodule
    )");

    std::println("parsed: {}, diagnostics: {}", result.parsed, result.diagnostics ? "present" : "none");
    return result.diagnostics ? 1 : 0;
}
