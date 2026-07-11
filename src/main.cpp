#include <iostream>
#include <print>
#include <string>
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
// Usage: clotho [--emit=run|ir|llvm|opt] [--time] [--no-z-elision] [--no-fuse] [--no-comb-fuse] [--no-thunk] [--domain-spec] [-O] [--perf-jitdump] [-o <out>] [-D<macro>] [-I<dir>] [+define+...] [+incdir+...] <file.sv>... [+plusarg...]
// Multiple source files are compiled together into one design (shared name
// space, LRM 3.12), parsed in the order given.
// `llvm` emits the backend's raw IR before the coroutine-lowering pipeline runs
// (coro intrinsics intact) — for manual review; `opt` emits it after the passes.
// `--no-z-elision` keeps every signal slot's z word (bisection / debug).
int main(int argc, char** argv) {
    std::string_view              emit = "run";
    std::string_view              out;
    std::vector<std::string_view> inputs;
    std::vector<std::string_view> plusargs;
    clotho::FrontendOptions       fe;
    bool                          time = false;
    clotho::ir::SimulationConfig  config;

    // Split a `+define+A=1+B+C` / `+incdir+d1+d2` tail on '+', appending each
    // non-empty field to `out`. Empty fields (e.g. a trailing '+') are skipped.
    const auto split_plus = [](std::string_view tail, std::vector<std::string>& out) {
        for (size_t start = 0; start <= tail.size();) {
            const size_t end   = tail.find('+', start);
            const auto   field = tail.substr(start, end - start);
            if (!field.empty()) {
                out.emplace_back(field);
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    };

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
        } else if (arg == "--no-comb-fuse") {
            config.comb_fuse = false;
        } else if (arg == "--no-thunk") {
            config.thunk = false;
        } else if (arg == "--domain-spec") {
            config.domain_spec = true;
        } else if (arg == "-O") {
            config.o3 = true;
        } else if (arg == "--perf-jitdump") {
            config.perf_jitdump = true;
        } else if (arg == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (arg == "-D" && i + 1 < argc) {
            fe.defines.emplace_back(argv[++i]);
        } else if (arg.starts_with("-D")) {
            fe.defines.emplace_back(arg.substr(2));
        } else if (arg == "-I" && i + 1 < argc) {
            fe.include_dirs.emplace_back(argv[++i]);
        } else if (arg.starts_with("-I")) {
            fe.include_dirs.emplace_back(arg.substr(2));
        } else if (arg.starts_with("+define+")) {
            // `+define+NAME=VALUE+NAME2...` macro predefines (LRM 22.5.1).
            split_plus(arg.substr(8), fe.defines);
        } else if (arg.starts_with("+incdir+")) {
            // `+incdir+dir1+dir2...` `include search dirs (LRM 22.4).
            split_plus(arg.substr(8), fe.include_dirs);
        } else if (arg.starts_with("+")) {
            // A plusarg (LRM §21.6): made available to $test$plusargs /
            // $value$plusargs, in order, with the leading `+` stripped. Not a
            // design source or a Clotho option.
            plusargs.push_back(arg.substr(1));
        } else if (!arg.starts_with("-")) {
            inputs.push_back(arg);
        } else {
            std::println(stderr, "unknown option: {}", arg);
            return 2;
        }
    }

    if (inputs.empty() || (emit != "run" && emit != "ir" && emit != "llvm" && emit != "opt")) {
        std::println(stderr,
                     "usage: {} [--emit=run|ir|llvm|opt] [--time] [--no-z-elision] [--no-fuse] [--no-comb-fuse] [--no-thunk] [--domain-spec] [-O] [--perf-jitdump] [-o <out>] [-D<macro>] [-I<dir>] [+define+...] [+incdir+...] <file.sv>... [+plusarg...]",
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

    const clotho::LowerResult result = clotho::lower_files(inputs, fe, tp);
    if (!result.diagnostics.empty()) {
        std::print(stderr, "{}", result.diagnostics);
    }
    if (!result.ok) {
        return 1;
    }

    if (emit == "run") {
        const int rc = clotho::ir::run_jit(*result.module, tp, config, plusargs);
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
