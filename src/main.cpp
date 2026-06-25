#include <iostream>
#include <print>
#include <string_view>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "ir/Printer.h"

// Thin driver: take a SystemVerilog file, run it through the lowering pipeline,
// then JIT-compile and run it (default), or emit the process IR / LLVM IR for
// debugging. All compiler logic lives in libclotho / libclotho_ir.
//
// Usage: clotho [--emit=run|ir|llvm|opt] [--time] [-o <out>] <file.sv>
// `llvm` emits the backend's raw IR before the coroutine-lowering pipeline runs
// (coro intrinsics intact) — for manual review; `opt` emits it after the passes.
int main(int argc, char** argv) {
    std::string_view emit = "run";
    std::string_view out;
    std::string_view input;
    bool             time = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--emit=")) {
            emit = arg.substr(7);
        } else if (arg == "--time") {
            time = true;
        } else if (arg == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (!arg.starts_with("-")) {
            input = arg;
        } else {
            std::println(stderr, "unknown option: {}", arg);
            return 2;
        }
    }

    if (input.empty() || (emit != "run" && emit != "ir" && emit != "llvm" && emit != "opt")) {
        std::println(stderr, "usage: {} [--emit=run|ir|llvm|opt] [--time] [-o <out>] <file.sv>", argv[0]);
        return 2;
    }

    // Phase timings reported to stderr when --time is set (stdout stays clean for
    // the design's own output).
    clotho::Timings  timings;
    clotho::Timings* tp = time ? &timings : nullptr;
    const auto       report = [&] {
        if (!time) {
            return;
        }
        std::println(stderr, "timings (ms):");
        std::println(stderr, "  {:<16}{:>12.3f}", "frontend (slang)", timings.frontend_ms);
        std::println(stderr, "  {:<16}{:>12.3f}", "analysis (ir)", timings.analysis_ms);
        // llvm compile and simulation are only measured on the run path.
        if (emit == "run") {
            std::println(stderr, "  {:<16}{:>12.3f}", "llvm compile", timings.llvm_ms);
            std::println(stderr, "  {:<16}{:>12.3f}", "simulation", timings.sim_ms);
        }
        const double total =
          timings.frontend_ms + timings.analysis_ms + timings.llvm_ms + timings.sim_ms;
        std::println(stderr, "  {:<16}{:>12.3f}", "total", total);
    };

    const clotho::LowerResult result = clotho::lower_file(input, tp);
    if (!result.diagnostics.empty()) {
        std::print(stderr, "{}", result.diagnostics);
    }
    if (!result.ok) {
        return 1;
    }

    if (emit == "run") {
        const int rc = clotho::ir::run_jit(*result.module, tp);
        report();
        return rc;
    }

    if (emit == "ir") {
        clotho::ir::print_module(*result.module, std::cout);
        report();
        return 0;
    }

    // emit == "llvm" (raw backend output) or "opt" (after the coroutine passes).
    const bool optimize = emit == "opt";
    bool       ok       = false;
    if (out.empty()) {
        ok = clotho::ir::emit_llvm(*result.module, llvm::outs(), optimize);
    } else {
        std::error_code      ec;
        llvm::raw_fd_ostream os(out, ec);
        if (ec) {
            std::println(stderr, "cannot open '{}': {}", out, ec.message());
            return 1;
        }
        ok = clotho::ir::emit_llvm(*result.module, os, optimize);
    }
    report();
    return ok ? 0 : 1;
}
