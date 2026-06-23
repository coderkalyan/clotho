#include <iostream>
#include <print>
#include <string_view>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "ir/LlvmBackend.h"
#include "ir/Printer.h"

// Thin driver: take a SystemVerilog file, run it through the lowering pipeline,
// and either print the process IR (default) or emit LLVM IR. All compiler logic
// lives in libclotho / libclotho_ir.
//
// Usage: clotho [--emit=ir|llvm] [-o <out>] <file.sv>
int main(int argc, char** argv) {
    std::string_view emit = "ir";
    std::string_view out;
    std::string_view input;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--emit=")) {
            emit = arg.substr(7);
        } else if (arg == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (!arg.starts_with("-")) {
            input = arg;
        } else {
            std::println(stderr, "unknown option: {}", arg);
            return 2;
        }
    }

    if (input.empty() || (emit != "ir" && emit != "llvm")) {
        std::println(stderr, "usage: {} [--emit=ir|llvm] [-o <out>] <file.sv>", argv[0]);
        return 2;
    }

    const clotho::LowerResult result = clotho::lower_file(input);
    if (!result.diagnostics.empty()) {
        std::print(stderr, "{}", result.diagnostics);
    }
    if (!result.ok) {
        return 1;
    }

    if (emit == "ir") {
        clotho::ir::print_module(*result.module, std::cout);
        return 0;
    }

    // emit == "llvm"
    if (out.empty()) {
        return clotho::ir::emit_llvm(*result.module, llvm::outs()) ? 0 : 1;
    }
    std::error_code      ec;
    llvm::raw_fd_ostream os(out, ec);
    if (ec) {
        std::println(stderr, "cannot open '{}': {}", out, ec.message());
        return 1;
    }
    return clotho::ir::emit_llvm(*result.module, os) ? 0 : 1;
}
