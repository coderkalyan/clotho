#include <iostream>
#include <print>
#include <string_view>
#include <vector>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "ir/Printer.h"

// Thin driver: take a SystemVerilog file, run it through the lowering pipeline,
// then JIT-compile and run it (default), or emit the process IR / LLVM IR for
// debugging. All compiler logic lives in libclotho / libclotho_ir.
//
// Usage: clotho [--emit=run|ir|llvm|opt] [--time] [--no-z-elision] [--no-fuse] [-O] [--perf-jitdump] [-o <out>] <file.sv>...
// Multiple source files are compiled together into one design (shared name
// space, LRM 3.12), parsed in the order given.
// `llvm` emits the backend's raw IR before the coroutine-lowering pipeline runs
// (coro intrinsics intact) — for manual review; `opt` emits it after the passes.
// `--no-z-elision` keeps every signal slot's z word (bisection / debug).
int main(int argc, char** argv) {
    std::string_view              emit = "run";
    std::string_view              out;
    std::vector<std::string_view> inputs;
    bool                          time = false;
    clotho::ir::SimulationConfig  config;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--emit=")) {
            emit = arg.substr(7);
        } else if (arg == "--time") {
            time = true;
        } else if (arg == "--no-z-elision") {
            config.z_elision = false;
        } else if (arg == "--no-fuse") {
            config.fuse = false;
        } else if (arg == "-O") {
            config.o3 = true;
        } else if (arg == "--perf-jitdump") {
            config.perf_jitdump = true;
        } else if (arg == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (!arg.starts_with("-")) {
            inputs.push_back(arg);
        } else {
            std::println(stderr, "unknown option: {}", arg);
            return 2;
        }
    }

    if (inputs.empty() || (emit != "run" && emit != "ir" && emit != "llvm" && emit != "opt")) {
        std::println(stderr,
                     "usage: {} [--emit=run|ir|llvm|opt] [--time] [--no-z-elision] [--no-fuse] [-O] [--perf-jitdump] [-o <out>] <file.sv>...",
                     argv[0]);
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

    const clotho::LowerResult result = clotho::lower_files(inputs, tp);
    if (!result.diagnostics.empty()) {
        std::print(stderr, "{}", result.diagnostics);
    }
    if (!result.ok) {
        return 1;
    }

    if (emit == "run") {
        const int rc = clotho::ir::run_jit(*result.module, tp, config);
        report();
        return rc;
    }

    if (emit == "ir") {
        clotho::ir::print_module(*result.module, std::cout);
        report();
        return 0;
    }

    // emit == "llvm" (raw backend output) or "opt" (after the coroutine passes).
    config.optimize = emit == "opt";
    bool ok         = false;
    if (out.empty()) {
        ok = clotho::ir::emit_llvm(*result.module, llvm::outs(), config);
    } else {
        std::error_code      ec;
        llvm::raw_fd_ostream os(out, ec);
        if (ec) {
            std::println(stderr, "cannot open '{}': {}", out, ec.message());
            return 1;
        }
        ok = clotho::ir::emit_llvm(*result.module, os, config);
    }
    report();
    return ok ? 0 : 1;
}
