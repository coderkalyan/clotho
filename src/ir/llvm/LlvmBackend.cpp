// LLVM backend entry points: build the modules, run the LLVM pipeline, emit IR.

#include "ir/llvm/Backend.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/ThreadPool.h>
#include <llvm/Support/Threading.h>

#include "ir/Verify.h"
#include "ir/llvm/RuntimeBitcode.h"

namespace clotho::ir {

namespace {

using namespace backend;

// Print a backend refusal the way every other Clotho diagnostic prints:
// `file:line:col: error: <message> [<code>]`, exactly the shape
// Diagnostic.cpp's render produces for the frontend's own guards.
//
// Three properties this buys, all of which the old `llvm backend error: <text>`
// line lacked. A LOCATION, from the span lower_region attached, so the user can
// find the construct. A stable CODE, so a refusal is filterable and waivable
// like any other item. And no stage name: which pass refused is Clotho's
// business, and "llvm backend" says nothing to someone whose `$value$plusargs`
// was rejected.
//
// The code follows the message, because the message is the classification the
// user reads. A guard whose text opens with "internal:" reports a broken backend
// invariant -- a Clotho bug, not a language feature -- and every such guard in
// this file is spelled that way; everything else is a legal construct the
// backend does not lower, which is the frontend's own `unsupported-construct`.
//
// NOTE: this cannot build a clotho::Diagnostic and hand it back. Diagnostic.h
// sits ABOVE the IR layer (it includes ir/Ir.h) and clotho_ir must not depend on
// the frontend -- see the clotho_ir_core / clotho split in CMakeLists.txt. The
// rendering is duplicated rather than the layering inverted; the two are pinned
// together by tests/plusargs_test.cpp.
void report_backend_error(const BackendError& e, const LocResolver& resolve) {
    const std::string_view code =
      e.message.starts_with("internal:") ? "internal-error" : "unsupported-construct";
    SourceText at;
    if (e.span.valid() && resolve) {
        at = resolve(e.span);
        llvm::errs() << at.where << ": ";
    }
    llvm::errs() << "error: " << e.message << " [" << code << "]\n";
    // The source line and a caret under it, as the frontend's own errors draw them.
    // The padding keeps the line's tabs, so the caret lines up whatever the
    // terminal's tab width is.
    if (!at.line.empty() && at.column >= 1 && at.column <= at.line.size() + 1) {
        std::string pad;
        for (uint32_t i = 0; i + 1 < at.column; ++i) {
            pad += at.line[i] == '\t' ? '\t' : ' ';
        }
        const uint32_t width = at.end_column > at.column ? at.end_column - at.column : 1;
        llvm::errs() << at.line << '\n' << pad << '^' << std::string(width - 1, '~') << '\n';
    }
}

// Clamp every access-alignment CLAIM the pointer cannot prove down to the 8
// bytes the data model does guarantee.
//
// IRBuilder's default load/store claim is the value type's ABI alignment -- 16
// for the i128-and-wider plane of a 4-state 33..64-bit (or any >=128-bit) slot
// -- and the aggregate-copy memcpys claim the aggregate's ABI alignment the
// same way. On x86-64 a claim of 16 makes codegen select the ALIGNED vector
// move (vmovdqa, #GP on a misaligned address; the wide-compare PTEST lowering
// is where it first bit). The claim is a lie through a class handle: a
// collected object's handle sits RT_GC_HEADER_BYTES past a 16-aligned cell
// (Gc.h:54-59), so it is 8 mod 16 permanently, and a field the DataLayout
// placed at a 16-aligned offset lands on an 8-mod-16 address.
//
// The proof rule: a claim getKnownAlignment can re-derive from the pointer is
// kept -- an alloca (its own align), a global plus constant offset (the
// align-64 state global's fields), an argument or call carrying an align
// attribute. Everything else -- a GC object body (never 16-aligned, by
// construction), a malloc'd container payload, a ref formal -- is clamped to
// 8, the floor every storage base and every slot layout does guarantee (GC
// handle 8, malloc >= 16, rt_alloc 64; i64-and-wider leaves sit at 8-aligned
// offsets). The later pipeline only re-raises an alignment it can re-prove the
// same way, so the clamp survives instcombine. Cost: an unprovable-but-true 16
// (a malloc'd payload) loses the aligned-vector encoding; on this
// microarchitecture the unaligned encodings are penalty-free on aligned data.
void clamp_unproven_align(llvm::Module& m) {
    const llvm::DataLayout& dl = m.getDataLayout();
    const llvm::Align       floor{ 8 };
    const auto              proven = [&](llvm::Value* ptr, llvm::Instruction* at,
                            llvm::Align claimed) {
        return llvm::getKnownAlignment(ptr, dl, at) >= claimed;
    };
    for (llvm::Function& f : m) {
        for (llvm::BasicBlock& bb : f) {
            for (llvm::Instruction& inst : bb) {
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                    if (ld->getAlign() > floor &&
                        !proven(ld->getPointerOperand(), ld, ld->getAlign())) {
                        ld->setAlignment(floor);
                    }
                } else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                    if (st->getAlign() > floor &&
                        !proven(st->getPointerOperand(), st, st->getAlign())) {
                        st->setAlignment(floor);
                    }
                } else if (auto* mi = llvm::dyn_cast<llvm::MemIntrinsic>(&inst)) {
                    // The aggregate-copy memcpys (emit_agg_storage_copy /
                    // emit_agg_deep_copy) claim the aggregate's ABI alignment on
                    // both ends; codegen expands a small constant-size memcpy
                    // inline with the same aligned vector moves.
                    const llvm::MaybeAlign dst = mi->getDestAlign();
                    if (dst && *dst > floor && !proven(mi->getRawDest(), mi, *dst)) {
                        mi->setDestAlignment(floor);
                    }
                    if (auto* mt = llvm::dyn_cast<llvm::MemTransferInst>(mi)) {
                        const llvm::MaybeAlign src = mt->getSourceAlign();
                        if (src && *src > floor && !proven(mt->getRawSource(), mt, *src)) {
                            mt->setSourceAlignment(floor);
                        }
                    }
                }
            }
        }
    }
}

// Tier-0 instruction selection (FastISel) folds an address computation into the
// load or store that uses it only inside one basic block. An address EarlyCSE
// computed once for several blocks is a value live across them: it holds a
// register through all of them or is spilled where it is made and reloaded in
// each block that uses it, where a constant displacement on the base would have
// cost nothing. A GEP with constant indices is free to compute where it is used, so
// give each block that uses one its own copy and let the access fold it. The
// base is then what each block reloads, once, however many fields it touches --
// the whole difference between code addressed through an instance record
// (everything one displacement off the record, which the entry of a group holds
// in one argument) and code addressed through absolute symbols, which never
// needed a register.
struct RematAddressPass : llvm::PassInfoMixin<RematAddressPass> {
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager& /*fam*/) {
        llvm::SmallVector<llvm::GetElementPtrInst*, 32> geps;
        for (llvm::BasicBlock& bb : f) {
            for (llvm::Instruction& inst : bb) {
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst); gep != nullptr && gep->hasAllConstantIndices()) {
                    geps.push_back(gep);
                }
            }
        }
        bool changed = false;
        for (llvm::GetElementPtrInst* gep : geps) {
            // The uses outside the GEP's own block, a copy per block. A phi use is
            // read on the edge, not in its block, and keeps the original.
            llvm::SmallVector<llvm::Use*, 8> outside;
            for (llvm::Use& use : gep->uses()) {
                auto* user = llvm::cast<llvm::Instruction>(use.getUser());
                if (user->getParent() != gep->getParent() && !llvm::isa<llvm::PHINode>(user)) {
                    outside.push_back(&use);
                }
            }
            llvm::SmallDenseMap<llvm::BasicBlock*, llvm::Instruction*, 4> copy;
            for (llvm::Use* use : outside) {
                llvm::BasicBlock* const bb = llvm::cast<llvm::Instruction>(use->getUser())->getParent();
                llvm::Instruction*&     c  = copy[bb];
                if (c == nullptr) {
                    c = gep->clone();
                    c->insertBefore(bb->getFirstInsertionPt());
                }
                use->set(c);
                changed = true;
            }
            if (gep->use_empty()) {
                gep->eraseFromParent();
            }
        }
        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }
};

// Tier-0 code generation lays blocks out in the order the function holds them,
// which is the order the backend created them in: a block's successor is rarely
// the next one, so a branch is a conditional jump and a jump, and a hot path is
// a chain of them. Put the blocks in reverse postorder of a walk that follows a
// terminator's last successor first, so the first successor of a branch (the
// "then" of a `br i1` the backend builds, the body of a loop test) is the block
// that follows it and a straight-line sequence of blocks falls through.
struct BlockLayoutPass : llvm::PassInfoMixin<BlockLayoutPass> {
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager& /*fam*/) {
        if (f.size() < 3) {
            return llvm::PreservedAnalyses::all();
        }
        llvm::SmallVector<llvm::BasicBlock*, 32>                         post;
        llvm::SmallPtrSet<llvm::BasicBlock*, 32>                         seen;
        llvm::SmallVector<std::pair<llvm::BasicBlock*, unsigned>, 32> stack;
        stack.emplace_back(&f.getEntryBlock(), 0);
        seen.insert(&f.getEntryBlock());
        while (!stack.empty()) {
            auto& [bb, next] = stack.back();
            const llvm::Instruction* term = bb->getTerminator();
            const unsigned           n    = term->getNumSuccessors();
            if (next < n) {
                // The last successor first, so the first is finished last and comes
                // first in reverse postorder.
                llvm::BasicBlock* succ = term->getSuccessor(n - 1 - next++);
                if (seen.insert(succ).second) {
                    stack.emplace_back(succ, 0);
                }
            } else {
                post.push_back(bb);
                stack.pop_back();
            }
        }
        // Reverse postorder, entry first; blocks the walk did not reach (none but a
        // dead block the optimizer left) keep their place after the others.
        llvm::BasicBlock* prev = &f.getEntryBlock();
        for (auto it = post.rbegin() + 1; it != post.rend(); ++it) {
            (*it)->moveAfter(prev);
            prev = *it;
        }
        return llvm::PreservedAnalyses::all();
    }
};

} // namespace

// Lower the coroutine intrinsics emitted for each process and clean up the
// generated code (tier-0 pipeline: fast compile, no loop/GVN passes). CoroSplit
// (with CoroEarly/CoroCleanup) is mandatory: without it the llvm.coro.*
// intrinsics survive to codegen and the module won't compile. The pre-split
// sroa/early-cse/simplifycfg/instcombine run inside the CGSCC nest (after
// inlining, before split) so promoted values never reach the coroutine frame;
// the post-split function passes clean the resume functions, which do not exist
// earlier. instcombine is what folds the z-elision zero-plane traffic and the
// plane-extraction trunc/lshr chains; instsimplify is not enough (measured:
// no sim-time win). no-verify-fixpoint: LLVM 22
// errors if instcombine's single iteration does not converge on our
// non-canonical IR. Returns false (reason on errs()) only if the pipeline
// string is rejected. Exposed (not file-local) so Jit.cpp can run it per
// module after a bitcode context migration (parallel compilation).
bool optimize_module(llvm::Module& m, bool o3) {
    // The runtime's own functions the module calls come in first, so the pipeline
    // inlines them and folds what they are called with. A queue that is only pushed
    // and popped inline can leave its descriptor, and the kernels it points at, with
    // no reader, which tier 0 does not sweep on its own, so it ends with a global DCE
    // exactly when something was linked.
    const bool linked = link_runtime_bitcode(m);

    llvm::PassBuilder         pb;
    llvm::LoopAnalysisManager     lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager    cgam;
    llvm::ModuleAnalysisManager   mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    llvm::ModulePassManager mpm;
    if (o3) {
        // Aggressive tier: LLVM's full default<O3> pipeline, which contains
        // the mandatory coro passes (CoroEarly / CoroSplit in the CGSCC nest /
        // CoroCleanup) plus the loop/GVN/vectorizer stack tier 0 omits.
        mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    }
    else {
        if (auto err = pb.parsePassPipeline(mpm,
                                            std::string("coro-early,"
                                            "cgscc(inline,function(sroa,early-cse,simplifycfg,instcombine<no-verify-fixpoint>),coro-split),"
                                            "function(simplifycfg,instcombine<no-verify-fixpoint>,dce),"
                                            "coro-cleanup") + (linked ? ",globaldce" : ""))) {
            llvm::errs() << "llvm backend: bad coroutine pipeline: " << llvm::toString(std::move(err)) << "\n";
            return false;
        }
        // Last, so nothing merges the copies again.
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(RematAddressPass()));
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(BlockLayoutPass()));
    }
    mpm.run(m, mam);
    return true;
}

bool select_codegen_tier(bool o3) {
    // NOTE: LLVM picks the register-allocation pipeline from the target machine's
    // optimization level alone; the only way to have tier 0's -O0 code generation
    // (FastISel) feed the greedy allocator is LLVM's own command-line options,
    // which are process-global. They are set as an `llc` command line would set
    // them, and every one is reset to LLVM's default under -O, so a process that
    // compiles at both tiers in turn gets each tier exactly. Two compilations at
    // different tiers must not overlap in one process.
    //
    // The greedy allocator and the coalescer cut the instructions hazard3 executes
    // by about a quarter for about a tenth more compile time (measurements in the
    // commit that added this). The other passes the optimized allocation pipeline
    // brings in -- machine scheduling, LICM and copy propagation -- cost compile
    // time and bought no instructions, so they stay off.
    static constexpr std::pair<llvm::StringLiteral, llvm::StringLiteral> kTier0[] = {
        { "optimize-regalloc", "true" },
        { "enable-misched", "false" },
        { "disable-machine-licm", "true" },
        { "disable-copyprop", "true" },
    };
    const auto& options = llvm::cl::getRegisteredOptions();
    for (const auto& [name, value] : kTier0) {
        const auto it = options.find(name);
        if (it == options.end()) {
            llvm::errs() << "llvm backend: this LLVM has no code generation option '" << name << "'\n";
            return false;
        }
        llvm::cl::Option* const option = it->second;
        option->reset();
        if (!o3 && option->addOccurrence(0, name, value)) {
            llvm::errs() << "llvm backend: cannot set code generation option '" << name << "' to " << value << "\n";
            return false;
        }
    }
    return true;
}

llvm::Expected<llvm::orc::JITTargetMachineBuilder> host_target() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });
    return llvm::orc::JITTargetMachineBuilder::detectHost();
}

BuiltDesign build_modules_unoptimized(const Module& m, llvm::LLVMContext& ctx,
                                      const llvm::TargetMachine& target, const SimulationConfig& config,
                                      const LocResolver& resolve) {
    // Every route to code generation passes here, the hand-built modules of the
    // backend tests included: the backend asserts the invariants the verifier's rules
    // state and repairs none of them.
    assert_well_typed(m, resolve);

    // Not the design's name: a unit module can be named like the design.
    auto root = std::make_unique<llvm::Module>("clotho.root", ctx);

    // Pin the modules to the target's triple and DataLayout before any IR is
    // built: the state/frame struct layouts, the NBA meta table's baked byte
    // offsets, and load/store alignments are all DataLayout-derived, so they
    // must be computed against the layout codegen will actually use. Unit
    // modules copy the root's triple/DataLayout at creation (unit_env).
    root->setTargetTriple(target.getTargetTriple());
    root->setDataLayout(target.createDataLayout());

    BuiltDesign built;
    try {
        Backend be(m, ctx, *root, config, resolve);
        be.run();
        built.modules = be.take_unit_modules();
    } catch (const BackendError& e) {
        report_backend_error(e, resolve);
        return {};
    }
    built.modules.insert(built.modules.begin(), std::move(root));

    // Correct the over-claimed access alignments before anything downstream
    // (verification, the coro pipeline, codegen) can act on them.
    for (const auto& mod : built.modules) {
        clamp_unproven_align(*mod);
    }

    // Verify every freshly-built (pre-split) module. Coro intrinsics are still
    // intact — the caller decides whether/how to run optimize_module (build_modules
    // does so inline below; run_jit defers it to migrate each module into its own
    // LLVMContext first, see Jit.cpp).
    for (const auto& mod : built.modules) {
        if (llvm::verifyModule(*mod, &llvm::errs())) {
            return {};
        }
    }
    return built;
}

BuiltDesign build_modules(const Module& m, llvm::LLVMContext& ctx, const llvm::TargetMachine& target,
                          const SimulationConfig& config, const LocResolver& resolve) {
    auto built = build_modules_unoptimized(m, ctx, target, config, resolve);
    if (built.modules.empty()) {
        return {};
    }
    if (!config.lower_coro) {
        return built;
    }
    // Lower the coroutines per module and verify again — the second pass
    // guards against malformed coro lowering.
    for (const auto& mod : built.modules) {
        if (!optimize_module(*mod, config.o3)) {
            return {};
        }
        if (llvm::verifyModule(*mod, &llvm::errs())) {
            return {};
        }
    }
    return built;
}

// A built module written to bitcode, under the identifier it goes by.
struct ModuleBitcode {
    llvm::SmallVector<char, 0> bytes;
    std::string                name;
};

// Every module of `built`, in order, as bitcode; `built` is left empty. A
// read-only traversal of the context the modules share, so it is done on one
// thread: nothing is written into the context's uniquing tables, but concurrent
// readers of one LLVMContext are not worth the risk.
static std::vector<ModuleBitcode> serialize_modules(BuiltDesign& built) {
    std::vector<ModuleBitcode> bitcode(built.modules.size());
    for (size_t i = 0; i < built.modules.size(); ++i) {
        llvm::raw_svector_ostream bits(bitcode[i].bytes);
        llvm::WriteBitcodeToFile(*built.modules[i], bits);
        bitcode[i].name = built.modules[i]->getModuleIdentifier();
    }
    built.modules.clear();
    return bitcode;
}

// The built modules, each moved by a bitcode round trip into `shared` -- or, with
// none, into a context of its own, kept in `owned` -- so a module reloaded alone
// names its types as its own text says, where modules still sharing the building
// context suffix a type name another module holds.
static bool reload_modules(BuiltDesign& built, llvm::LLVMContext* shared,
                           std::vector<std::unique_ptr<llvm::LLVMContext>>& owned,
                           std::vector<std::unique_ptr<llvm::Module>>&      out) {
    const std::vector<ModuleBitcode> bitcode = serialize_modules(built);
    for (const ModuleBitcode& unit : bitcode) {
        const llvm::MemoryBufferRef buf(llvm::StringRef(unit.bytes.data(), unit.bytes.size()), unit.name);
        llvm::LLVMContext*          ctx = shared;
        if (ctx == nullptr) {
            ctx = owned.emplace_back(std::make_unique<llvm::LLVMContext>()).get();
        }
        auto parsed = llvm::parseBitcodeFile(buf, *ctx);
        if (!parsed) {
            llvm::errs() << "llvm backend: module reload failed: " << llvm::toString(parsed.takeError()) << "\n";
            return false;
        }
        out.push_back(std::move(*parsed));
    }
    return true;
}

llvm::Error for_each_optimized_module(BuiltDesign&                       built,
                                      std::unique_ptr<llvm::LLVMContext> build_ctx,
                                      const SimulationConfig&            config,
                                      ModuleConsumer                     consume) {
    const bool stat = stat_on(config.stats, Stat::Parallel);

    // Each buffer below is reparsed into its own fresh LLVMContext, so optimize_module,
    // verification and `consume` for different modules run concurrently instead of
    // serializing on one shared context's lock. The modules and the context that
    // holds them go now: only the bitcode is needed, and a large design's IR is
    // many times its bitcode.
    const auto                       t_serialize = std::chrono::steady_clock::now();
    const std::vector<ModuleBitcode> bitcode     = serialize_modules(built);
    build_ctx.reset();
    if (stat) {
        fprintf(stderr, "PARALLEL_STAT serialize: %.1f ms\n", ms_since(t_serialize));
    }

    const auto          t_parallel = std::chrono::steady_clock::now();
    std::vector<double> stat_ms(stat ? bitcode.size() : 0);
    std::mutex          err_mu;
    std::string         first_err;
    std::atomic<bool>   failed = false;
    const auto          fail   = [&](std::string message) {
        const std::lock_guard<std::mutex> lock(err_mu);
        if (first_err.empty()) {
            first_err = std::move(message);
        }
        failed = true;
    };

    {
        llvm::DefaultThreadPool pool(llvm::hardware_concurrency(config.compile_jobs));
        for (size_t i = 0; i < bitcode.size(); ++i) {
            pool.async([&, i] {
                if (failed) {
                    return;
                }
                const auto t0 = std::chrono::steady_clock::now();
                OwnedModule unit;
                unit.ctx = std::make_unique<llvm::LLVMContext>();
                const llvm::MemoryBufferRef buf(
                  llvm::StringRef(bitcode[i].bytes.data(), bitcode[i].bytes.size()), bitcode[i].name);
                auto parsed = llvm::parseBitcodeFile(buf, *unit.ctx);
                if (!parsed) {
                    fail("module '" + bitcode[i].name + "' reload failed: " + llvm::toString(parsed.takeError()));
                    return;
                }
                unit.module = std::move(*parsed);
                if (!optimize_module(*unit.module, config.o3)) {
                    fail("module '" + bitcode[i].name + "' optimize failed");
                    return;
                }
                if (llvm::verifyModule(*unit.module, &llvm::errs())) {
                    fail("module '" + bitcode[i].name + "' failed verification after optimize");
                    return;
                }
                if (auto err = consume(i, std::move(unit))) {
                    fail(llvm::toString(std::move(err)));
                    return;
                }
                if (stat) {
                    stat_ms[i] = ms_since(t0);
                }
            });
        }
        pool.wait();
    }

    if (stat) {
        fprintf(stderr, "PARALLEL_STAT parallel section wall: %.1f ms\n", ms_since(t_parallel));
        double sum = 0.0, max_ms = 0.0;
        size_t max_i = 0;
        for (size_t i = 0; i < stat_ms.size(); ++i) {
            sum += stat_ms[i];
            if (stat_ms[i] > max_ms) {
                max_ms = stat_ms[i];
                max_i  = i;
            }
        }
        // max_ms is the longest single module's optimize+verify+consume time -- a
        // hard wall-time floor no thread count can beat. sum/max_ms is the speedup
        // ceiling (vs a hypothetical fully-serial run of just this phase); compare
        // against the measured speedup to see how much headroom load imbalance (a
        // few big modules dominating) is costing.
        fprintf(stderr,
                "PARALLEL_STAT modules=%zu jobs=%u sum_ms=%.1f max_ms=%.1f (module %zu) ceiling=%.2fx\n",
                stat_ms.size(),
                llvm::hardware_concurrency(config.compile_jobs).compute_thread_count(),
                sum,
                max_ms,
                max_i,
                sum / std::max(max_ms, 1e-6));
        for (size_t i = 0; i < stat_ms.size(); ++i) {
            fprintf(stderr, "  module %zu: %.1f ms (bitcode %zu B)\n", i, stat_ms[i], bitcode[i].bytes.size());
        }
    }
    if (!first_err.empty()) {
        return llvm::createStringError(llvm::inconvertibleErrorCode(), first_err);
    }
    return llvm::Error::success();
}

SharedCodeCheck check_shared_code(const Module& m, const LocResolver& resolve) {
    assert_well_typed(m, resolve);
    auto host = host_target();
    if (!host) {
        return { SharedCodeCheck::Outcome::Refused, "cannot detect host target" };
    }
    auto tm = host->createTargetMachine();
    if (!tm) {
        llvm::consumeError(tm.takeError());
        return { SharedCodeCheck::Outcome::Refused, "cannot create the target machine" };
    }
    // Declared before the backend so they outlive it.
    llvm::LLVMContext ctx;
    llvm::Module      root("clotho.root", ctx);
    root.setTargetTriple((*tm)->getTargetTriple());
    root.setDataLayout((*tm)->createDataLayout());
    SimulationConfig config;
    config.verify_shared_code = true;
    try {
        Backend be(m, ctx, root, config, resolve);
        be.run();
    } catch (const BackendError& e) {
        if (!e.message.starts_with("internal: the code shared by")) {
            return { SharedCodeCheck::Outcome::Refused, e.message };
        }
        std::string where;
        if (e.span.valid() && resolve) {
            where = resolve(e.span).where + ": ";
        }
        return { SharedCodeCheck::Outcome::Mismatch, where + "error: " + e.message };
    }
    return {};
}

bool emit_llvm(const Module& m, llvm::raw_ostream& os, const SimulationConfig& config,
               Timings* timings, const LocResolver& resolve) {
    const auto t_llvm = std::chrono::steady_clock::now();
    auto       host   = host_target();
    if (!host) {
        llvm::errs() << "llvm backend: cannot detect host target: " << llvm::toString(host.takeError())
                     << "\n";
        return false;
    }
    auto tm = host->createTargetMachine();
    if (!tm) {
        llvm::errs() << "llvm backend: " << llvm::toString(tm.takeError()) << "\n";
        return false;
    }
    llvm::LLVMContext ctx;
    auto              built = build_modules(m, ctx, **tm, config, resolve);
    if (built.modules.empty()) {
        return false;
    }
    // One linked module: the unit modules are separate
    // llvm::Modules, and printing each in turn gives a file llvm-as rejects at the
    // second `; ModuleID`.
    //
    // The modules are first reloaded into a fresh context (a bitcode round trip,
    // as run_jit does). Linked inside the context that built them, the IR mover
    // finds each unit's named state types still held by the source modules and
    // gives the linked copies no name -- `%0 = type { %1, %1 }` for what was
    // `%alu.state` -- which is the readable part of this output. In a fresh
    // context the names are free.
    llvm::LLVMContext                               linked_ctx;
    std::vector<std::unique_ptr<llvm::LLVMContext>> no_owned;
    std::vector<std::unique_ptr<llvm::Module>>      reloaded;
    if (!reload_modules(built, &linked_ctx, no_owned, reloaded)) {
        return false;
    }
    std::unique_ptr<llvm::Module> root = std::move(reloaded.front());
    for (size_t i = 1; i < reloaded.size(); ++i) {
        // Reports its own diagnostic through the context's handler.
        llvm::Linker linker(*root);
        if (linker.linkInModule(std::move(reloaded[i]))) {
            return false;
        }
    }
    root->print(os, nullptr);
    if (timings != nullptr) {
        timings->llvm_ms += ms_since(t_llvm);
    }
    return true;
}

bool emit_llvm_modules(const Module& m, std::vector<ModuleIr>& out, const SimulationConfig& config,
                       const LocResolver& resolve) {
    auto host = host_target();
    if (!host) {
        llvm::errs() << "llvm backend: cannot detect host target: " << llvm::toString(host.takeError())
                     << "\n";
        return false;
    }
    auto tm = host->createTargetMachine();
    if (!tm) {
        llvm::errs() << "llvm backend: " << llvm::toString(tm.takeError()) << "\n";
        return false;
    }
    llvm::LLVMContext ctx;
    auto              built = build_modules(m, ctx, **tm, config, resolve);
    if (built.modules.empty()) {
        return false;
    }
    std::vector<std::unique_ptr<llvm::LLVMContext>> owned;
    std::vector<std::unique_ptr<llvm::Module>>      reloaded;
    if (!reload_modules(built, nullptr, owned, reloaded)) {
        return false;
    }
    for (const auto& mod : reloaded) {
        ModuleIr& entry = out.emplace_back();
        entry.name      = mod->getModuleIdentifier();
        llvm::raw_string_ostream os(entry.ir);
        mod->print(os, nullptr);
    }
    return true;
}

// The machine objects are generated for. Not target.createTargetMachine(): that
// asks for a JIT target machine, and on x86-64 a JIT one defaults to the large code
// model, whose absolute 64-bit addresses in the code are text relocations a macOS
// link refuses. Only the triple, CPU and features carry over; the code model and
// target options are the platform's defaults for ordinary compiled code. One per
// thread: a TargetMachine is not meant to be shared.
static llvm::Expected<std::unique_ptr<llvm::TargetMachine>>
object_machine(const llvm::orc::JITTargetMachineBuilder& target, bool o3) {
    std::string error;
    const auto* llvm_target = llvm::TargetRegistry::lookupTarget(target.getTargetTriple(), error);
    if (llvm_target == nullptr) {
        return llvm::createStringError(llvm::inconvertibleErrorCode(), error);
    }
    std::unique_ptr<llvm::TargetMachine> tm(
      llvm_target->createTargetMachine(target.getTargetTriple(),
                                       target.getCPU(),
                                       target.getFeatures().getString(),
                                       llvm::TargetOptions(),
                                       llvm::Reloc::PIC_,
                                       std::nullopt,
                                       o3 ? llvm::CodeGenOptLevel::Aggressive : llvm::CodeGenOptLevel::None));
    if (tm == nullptr) {
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "cannot generate code for " + target.getTargetTriple().str());
    }
    return tm;
}

// The object code of `module`, generated into memory and written out by the caller.
static llvm::Expected<llvm::SmallVector<char, 0>> generate_object(llvm::Module& module, llvm::TargetMachine& tm) {
    llvm::SmallVector<char, 0> object;
    llvm::raw_svector_ostream  object_os(object);
    llvm::legacy::PassManager  pm;
    if (tm.addPassesToEmitFile(pm, object_os, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "target " + tm.getTargetTriple().str() + " cannot write an object file");
    }
    pm.run(module);
    return object;
}

bool emit_objects(const Module&                             m,
                  const std::string&                        dir,
                  std::vector<std::string>&                 objects,
                  const llvm::orc::JITTargetMachineBuilder& target,
                  const SimulationConfig&                   config,
                  Timings*                                  timings,
                  const LocResolver&                        resolve) {
    assert(config.lower_coro && "an object needs the coroutines lowered");
    const auto t_llvm = std::chrono::steady_clock::now();
    if (!select_codegen_tier(config.o3)) {
        return false;
    }
    auto layout = object_machine(target, config.o3);
    if (!layout) {
        llvm::errs() << "llvm backend: " << llvm::toString(layout.takeError()) << "\n";
        return false;
    }
    auto ctx   = std::make_unique<llvm::LLVMContext>();
    auto built = build_modules_unoptimized(m, *ctx, **layout, config, resolve);
    if (built.modules.empty()) {
        return false;
    }

    objects.clear();
    for (size_t i = 0; i < built.modules.size(); ++i) {
        llvm::SmallString<256> path(dir);
        llvm::sys::path::append(path, "design." + std::to_string(i) + ".o");
        objects.emplace_back(path);
    }
    if (auto err = for_each_optimized_module(built, std::move(ctx), config, [&](size_t i, OwnedModule&& unit) -> llvm::Error {
            auto tm = object_machine(target, config.o3);
            if (!tm) {
                return tm.takeError();
            }
            auto object = generate_object(*unit.module, **tm);
            if (!object) {
                return object.takeError();
            }
            std::error_code      ec;
            llvm::raw_fd_ostream file(objects[i], ec, llvm::sys::fs::OF_None);
            if (ec) {
                return llvm::createStringError(ec, "cannot open '" + objects[i] + "': " + ec.message());
            }
            file.write(object->data(), object->size());
            file.close();
            if (file.has_error()) {
                ec = file.error();
                file.clear_error();
                return llvm::createStringError(ec, "cannot write '" + objects[i] + "': " + ec.message());
            }
            return llvm::Error::success();
        })) {
        llvm::errs() << "llvm backend: " << llvm::toString(std::move(err)) << "\n";
        return false;
    }
    if (timings != nullptr) {
        timings->llvm_ms += ms_since(t_llvm);
    }
    return true;
}

} // namespace clotho::ir
