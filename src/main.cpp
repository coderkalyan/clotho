#include <iostream>
#include <print>

#include "Lower.h"
#include "ir/Printer.h"

// Thin driver: take a SystemVerilog file, run it through the lowering pipeline,
// and print the resulting process IR. All compiler logic lives in libclotho.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: {} <file.sv>", argv[0]);
        return 2;
    }

    const clotho::LowerResult result = clotho::lowerFile(argv[1]);
    if (!result.diagnostics.empty()) {
        std::print(stderr, "{}", result.diagnostics);
    }
    if (!result.ok) {
        return 1;
    }

    clotho::ir::printModule(*result.module, std::cout);

    return 0;
}
