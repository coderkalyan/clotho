// Runtime ABI: the rt_* entry points generated code calls and the LLVM mirrors of the runtime's C structs.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

llvm::FunctionCallee Backend::rt_fn(const char* name, llvm::Type* ret, llvm::ArrayRef<llvm::Type*> params) {
    return env_->llmod.getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
}

llvm::Type* Backend::raw_ptr_ty() {
    return llvm::PointerType::getUnqual(ctx_);
}

// -- $display / $write --

// The runtime argument descriptor, mirroring `rt_arg` in runtime/Runtime.h:
//   { i32 width, i32 flags, ptr val, ptr xz, ptr z }
// Each plane is a pointer to a ceil(width/64)-word little-endian buffer
// (built in build_rt_args), so operands of any width are carried in full.
llvm::StructType* Backend::arg_struct_ty() {
    return llvm::StructType::get(
      ctx_, { b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_fwrite(i32 desc, const char* fmt, i32 argc, const rt_arg* argv)
llvm::FunctionCallee Backend::fwrite_fn() {
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_fwrite", b_.getVoidTy(), { i32, raw_ptr_ty(), i32, raw_ptr_ty() });
}

// -- $strobe (LRM 21.2.2) --

// void rt_strobe_pend(ptr handle) — queue one $strobe invocation's ramped
// (not yet resumed) coroutine handle to run in the Postponed region of the
// current time step (LRM 21.2.2).
llvm::FunctionCallee Backend::strobe_pend_fn() {
    return rt_fn("rt_strobe_pend", b_.getVoidTy(), { raw_ptr_ty() });
}

// ptr rt_strobe_pop(void) — pop and return the next queued handle (FIFO —
// same-instant $strobe calls print in call order), or null when empty.
llvm::FunctionCallee Backend::strobe_pop_fn() {
    return rt_fn("rt_strobe_pop", raw_ptr_ty(), {});
}

// -- $monitor (LRM 21.2.2) --

// rt_monitor_emit(i32 desc, ptr fmt, i32 argc, ptr argv) — like rt_fwrite but
// prints only when the rendered output differs from the last (LRM 21.2.2).
llvm::FunctionCallee Backend::monitor_emit_fn() {
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_monitor_emit", b_.getVoidTy(), { i32, raw_ptr_ty(), i32, raw_ptr_ty() });
}

// rt_monitor_set(ptr ramp) — register the monitor body's ramp function pointer
// as the single active monitor (enabled, output-primed reset).
llvm::FunctionCallee Backend::monitor_set_fn() {
    return rt_fn("rt_monitor_set", b_.getVoidTy(), { raw_ptr_ty() });
}

// rt_monitor_enable(i32 on) — $monitoron / $monitoroff.
llvm::FunctionCallee Backend::monitor_enable_fn() {
    return rt_fn("rt_monitor_enable", b_.getVoidTy(), { b_.getInt32Ty() });
}

// ptr rt_monitor_active_ramp(void) — the registered ramp fn ptr if a monitor is
// active and enabled, else null (drives the Postponed-region per-instant run).
llvm::FunctionCallee Backend::monitor_active_ramp_fn() {
    return rt_fn("rt_monitor_active_ramp", raw_ptr_ty(), {});
}

// -- $timeformat / %t --

// void rt_timeformat_init(i64 tick_fs, i32 default_units)
llvm::FunctionCallee Backend::timeformat_init_fn() {
    return rt_fn("rt_timeformat_init", b_.getVoidTy(), { b_.getInt64Ty(), b_.getInt32Ty() });
}

// void rt_timeformat_set(i32 units, i32 precision, const char* suffix, i32 suffix_len, i32 min_width)
llvm::FunctionCallee Backend::timeformat_set_fn() {
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_timeformat_set", b_.getVoidTy(), { i32, i32, raw_ptr_ty(), i32, i32 });
}

// -- Simulation control --

// void rt_finish_coop(ptr banner, i64 now, i32 arg) — the cooperative form.
// Deliberately NOT noreturn: it prints the banner, sets rt_finishing, and
// returns so the caller can leave through its own epilogue and free its
// frame (docs/design/cooperative-finish.md).
llvm::FunctionCallee Backend::finish_coop_fn() {
    return rt_fn("rt_finish_coop", b_.getVoidTy(),
                 { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty() });
}

// void rt_observed_case_violation(ptr banner, i64 now, i32 kind) — queues the
// report for the Observed region (rt_observed_flush emits it). kind selects
// the message (0 = no match, 1 = overlap).
llvm::FunctionCallee Backend::observed_case_violation_fn() {
    return rt_fn("rt_observed_case_violation", b_.getVoidTy(),
                 { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty() });
}

// void rt_observed_flush() — emit + clear every Observed-region report queued
// this instant.
llvm::FunctionCallee Backend::observed_flush_fn() {
    return rt_fn("rt_observed_flush", b_.getVoidTy(), {});
}

// void rt_severity(i32 level, ptr banner, ptr fmt, i32 argc, ptr argv) —
// renders the message and writes the labelled diagnostic to stderr, then
// returns (non-fatal).
llvm::FunctionCallee Backend::severity_fn() {
    return rt_fn("rt_severity", b_.getVoidTy(),
                 { b_.getInt32Ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

// void rt_defer_report(i32 level, i64 is_final, ptr gen, i64 gen_val, ptr
// banner, ptr fmt, i32 argc, ptr argv) — queue a deferred assertion report.
llvm::FunctionCallee Backend::defer_report_fn() {
    return rt_fn("rt_defer_report", b_.getVoidTy(),
                 { b_.getInt32Ty(), b_.getInt64Ty(), raw_ptr_ty(), b_.getInt64Ty(),
                   raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

// void rt_defer_flush_observed() / rt_defer_flush_final() — mature the queued
// observed (#0) / final deferred reports.
llvm::FunctionCallee Backend::defer_flush_observed_fn() {
    return rt_fn("rt_defer_flush_observed", b_.getVoidTy(), {});
}

llvm::FunctionCallee Backend::defer_flush_final_fn() {
    return rt_fn("rt_defer_flush_final", b_.getVoidTy(), {});
}

// The rt_bounds_site record (Runtime.h): { ptr where, ptr line, ptr name,
// i32 line_no, i32 column, i32 end_column, i32 acc_line_no, i32 acc_column,
// i32 acc_end_column, i32 what, i32 left, i32 right, i32 width }. Field
// order and types mirror the C struct exactly, so the two change together.
llvm::StructType* Backend::bounds_site_ty() {
    auto* i32 = b_.getInt32Ty();
    auto* ptr = raw_ptr_ty();
    return llvm::StructType::get(
      ctx_, { ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 });
}

// void rt_bounds_report(ptr site, i64 index, i64 extent, i32 flags, i64 now)
// — writes one warning to stderr and returns; see Runtime.h for the flag
// bits and the extent convention.
llvm::FunctionCallee Backend::bounds_report_fn() {
    auto* i64 = b_.getInt64Ty();
    return rt_fn("rt_bounds_report", b_.getVoidTy(),
                 { raw_ptr_ty(), i64, i64, b_.getInt32Ty(), i64 });
}

// -- Runtime-offset bit / part select of a very wide vector (LRM 11.5.1) --

// void rt_wide_sel(const void* mm, const void* z, u32 width, i64 base, u32 cwidth,
//                  i64 off, u32 n, u32 four, u32 has_z, u32 unknown,
//                  u64* must, u64* may, u64* z_out)
llvm::FunctionCallee Backend::wide_sel_fn() {
    auto* i32 = b_.getInt32Ty();
    auto* i64 = b_.getInt64Ty();
    return rt_fn("rt_wide_sel", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), i32, i64, i32, i64, i32, i32, i32, i32,
                   raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_wide_ins(void* mm, void* z, u32 width, i64 base, u32 cwidth, i64 off,
//                  u32 n, u32 four, u32 has_z, u32 unknown, const u64* must,
//                  const u64* may, const u64* z)
llvm::FunctionCallee Backend::wide_ins_fn() {
    auto* i32 = b_.getInt32Ty();
    auto* i64 = b_.getInt64Ty();
    return rt_fn("rt_wide_ins", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), i32, i64, i32, i64, i32, i32, i32, i32,
                   raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// -- Multiply, divide and modulo of very wide integers --

// void rt_wide_mul / rt_wide_udiv / rt_wide_sdiv / rt_wide_umod / rt_wide_smod(
//     u64* out, const u64* a, const u64* b, u32 width): all five share one shape.
llvm::FunctionCallee Backend::wide_arith_fn(const char* name) {
    return rt_fn(name, b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
}

// -- Null class-handle dereference (LRM 8.4) --

llvm::StructType* Backend::null_site_ty() {
    auto* i32 = b_.getInt32Ty();
    return llvm::StructType::get(ctx_, { raw_ptr_ty(), raw_ptr_ty(), i32, i32, i32, i32 });
}

// void rt_null_handle(ptr site, i64 now) — prints the located fatal and ends
// the simulation; never returns (see Runtime.h).
llvm::FunctionCallee Backend::null_handle_fn() {
    return rt_fn("rt_null_handle", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty() });
}

// -- A `ref` whose actual's container was restructured under it (LRM 13.5.2) --

// The rt_ref_site record (Runtime.h): { ptr where, ptr line, i32 line_no,
// i32 column, i32 end_column, ptr formal, ptr container }. Field order and
// types mirror the C struct exactly, so the two change together.
llvm::StructType* Backend::ref_site_ty() {
    auto* i32 = b_.getInt32Ty();
    return llvm::StructType::get(
      ctx_, { raw_ptr_ty(), raw_ptr_ty(), i32, i32, i32, raw_ptr_ty() });
}

// void rt_ref_stale(ptr site, i64 expect, i64 actual, i64 now) — reports the
// dangling ref; fatal by default, a once-per-site warning under
// --loose-stale-ref, so it CAN return (see Runtime.h).
//
// Declared as touching only memory the design cannot reach (it writes a
// diagnostic and a per-site suppression set, both runtime-private). Without
// that, GVN has to assume the call on the FAILING path clobbers the epoch, and
// refuses to CSE the epoch load of a second dereference in the same block — so
// N accesses through one ref formal paid N loads and N compares instead of one.
llvm::FunctionCallee Backend::ref_stale_fn() {
    auto callee =
      rt_fn("rt_ref_stale", b_.getVoidTy(),
            { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty() });
    if (auto* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
        fn->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());
        fn->setDoesNotThrow();
    }
    return callee;
}

// void rt_ref_epoch_regress(ptr site, ptr epoch, i64 expect, i64 actual, i64 now)
// — an epoch that went backwards is a Clotho bug; aborts unconditionally.
llvm::FunctionCallee Backend::ref_epoch_regress_fn() {
    return rt_fn(
      "rt_ref_epoch_regress", b_.getVoidTy(),
      { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty() });
}

// { ptr epoch, ptr name } -- must match rt_container_name in Runtime.h.
llvm::StructType* Backend::ref_name_ty() {
    return llvm::StructType::get(ctx_, { raw_ptr_ty(), raw_ptr_ty() });
}

// -- Tagged-union member access inconsistent with the current tag (LRM 11.9) --

// The rt_tagged_site record (Runtime.h): { ptr where, ptr line, i32 line_no,
// i32 column, i32 end_column, ptr member, ptr names, ptr name_lens, i32 count,
// i32 is_write }. Field order and types mirror the C struct exactly, so the two
// change together.
llvm::StructType* Backend::tagged_site_ty() {
    auto* i32 = b_.getInt32Ty();
    return llvm::StructType::get(ctx_, { raw_ptr_ty(), raw_ptr_ty(), i32, i32, i32,
                                         raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), i32, i32 });
}

// void rt_tagged_access(ptr site, i64 tag, i32 tag_unknown, i64 now) — prints
// the located fatal and ends the simulation; never returns (see Runtime.h).
llvm::FunctionCallee Backend::tagged_access_fn() {
    return rt_fn("rt_tagged_access", b_.getVoidTy(),
                 { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty(), b_.getInt64Ty() });
}

// -- Checked heap allocation --

// The rt_alloc_site record (Runtime.h): { ptr where, ptr line, ptr what,
// i32 line_no, i32 column, i32 end_column }. Field order and types mirror
// the C struct exactly, so the two change together.
llvm::StructType* Backend::alloc_site_ty() {
    auto* i32 = b_.getInt32Ty();
    return llvm::StructType::get(ctx_,
                                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), i32, i32, i32 });
}

// void rt_sim_run(ptr state, ptr wheel, ptr drain, ptr finals) — wraps the
// drain in the $finish catch (a setjmp target) so a $finish returns here with
// state live, then runs the final blocks (LRM 9.2.3). See runtime/Scheduler.cpp.
llvm::FunctionCallee Backend::sim_run_fn() {
    auto* ptr = raw_ptr_ty();
    return rt_fn("rt_sim_run", b_.getVoidTy(), { ptr, ptr, ptr, ptr });
}

// -- File I/O (LRM 1800-2023 §21.3) --

// i32 rt_fopen_mcd(const char* name, i32 name_len)
llvm::FunctionCallee Backend::fopen_mcd_fn() {
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_fopen_mcd", i32, { raw_ptr_ty(), i32, i32 });
}

// i32 rt_fopen_fd(const char* name, i32 name_len, i32 name_fwd,
//                 const char* mode, i32 mode_len, i32 mode_fwd)
llvm::FunctionCallee Backend::fopen_fd_fn() {
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_fopen_fd", i32, { raw_ptr_ty(), i32, i32, raw_ptr_ty(), i32, i32 });
}

// void rt_fclose(i32 desc)
llvm::FunctionCallee Backend::fclose_fn() {
    return rt_fn("rt_fclose", b_.getVoidTy(), { b_.getInt32Ty() });
}

// i32 rt_system(const char* cmd, i32 cmd_len, i32 cmd_fwd, i32 has_arg)
llvm::FunctionCallee Backend::system_fn() {
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_system", i32, { raw_ptr_ty(), i32, i32, i32 });
}

// void rt_fflush(i32 desc) — flush a descriptor's output buffers (0 => all).
llvm::FunctionCallee Backend::fflush_fn() {
    return rt_fn("rt_fflush", b_.getVoidTy(), { b_.getInt32Ty() });
}

// i32 rt_fgets(i8* mm, i8* z, i32 width, i32 fd, i32 flags)
llvm::FunctionCallee Backend::fgets_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(i32, { ptr, ptr, i32, i32, i32 }, false);
    return env_->llmod.getOrInsertFunction("rt_fgets", fnty);
}

// i32 rt_fgets_str(rt_string* dst, i32 fd, const rt_alloc_site*)
llvm::FunctionCallee Backend::fgets_str_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(i32, { ptr, i32, ptr }, false);
    return env_->llmod.getOrInsertFunction("rt_fgets_str", fnty);
}

// void rt_sformat(i8* mm, i8* z, i32 width, const char* fmt, i32 argc, const rt_arg* argv,
//                 i32 flags, const rt_fmt_dyn* dyn)
llvm::FunctionCallee Backend::sformat_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr, i32, ptr, i32, ptr, i32, ptr }, false);
    return env_->llmod.getOrInsertFunction("rt_sformat", fnty);
}

// void rt_sformat_str(rt_string* dst, const char* fmt, i32 argc, const rt_arg* argv,
//                     const rt_alloc_site*, const rt_fmt_dyn* dyn)
llvm::FunctionCallee Backend::sformat_str_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* fnty =
      llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr, b_.getInt32Ty(), ptr, ptr, ptr }, false);
    return env_->llmod.getOrInsertFunction("rt_sformat_str", fnty);
}

// { i64 time_mul, ptr site } — mirrors rt_fmt_dyn in runtime/Runtime.h.
llvm::StructType* Backend::fmt_dyn_struct_ty() {
    return llvm::StructType::get(ctx_, { b_.getInt64Ty(), raw_ptr_ty() });
}

// i32 rt_ferror(i32 fd, i8* mm, i8* z, i32 width, i32 flags)
llvm::FunctionCallee Backend::ferror_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(i32, { i32, ptr, ptr, i32, i32 }, false);
    return env_->llmod.getOrInsertFunction("rt_ferror", fnty);
}

// i32 rt_ferror_str(i32 fd, rt_string* dst, const rt_alloc_site*)
llvm::FunctionCallee Backend::ferror_str_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(i32, { i32, ptr, ptr }, false);
    return env_->llmod.getOrInsertFunction("rt_ferror_str", fnty);
}

// { ptr mm, ptr z, i32 width, i32 flags, ptr str, ptr site } — mirrors
// rt_scan_arg in runtime/Runtime.h.
llvm::StructType* Backend::scan_arg_struct_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    return llvm::StructType::get(
      ctx_, { ptr, ptr, b_.getInt32Ty(), b_.getInt32Ty(), ptr, ptr });
}

// i32 rt_fscanf(i32 fd, const char* fmt, i32 argc, const rt_scan_arg* args)
llvm::FunctionCallee Backend::fscanf_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(i32, { i32, ptr, i32, ptr }, false);
    return env_->llmod.getOrInsertFunction("rt_fscanf", fnty);
}

// i32 rt_sscanf(const char* buf, i32 len, i32 fwd, const char* fmt, i32 argc,
//               const rt_scan_arg* args)
llvm::FunctionCallee Backend::sscanf_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(i32, { ptr, i32, i32, ptr, i32, ptr }, false);
    return env_->llmod.getOrInsertFunction("rt_sscanf", fnty);
}

// void rt_readmem(const char* path, i32 path_len, i8* mm_base, i8* z_base,
//                 i32 stride, i32 count, i32 width, i64 decl_left, i64 decl_right,
//                 i64 start, i64 finish, i32 flags)
llvm::FunctionCallee Backend::readmem_fn() {
    auto* ptr = raw_ptr_ty();
    auto* i32 = b_.getInt32Ty();
    auto* i64 = b_.getInt64Ty();
    return rt_fn("rt_readmem", b_.getVoidTy(), { ptr, i32, ptr, ptr, i32, i32, i32, i64, i64, i64, i64, i32 });
}

// i32 rt_fread(i32 fd, i8* mm, i8* z, i32 stride, i32 count, i32 width, i32 four,
//              i64 decl_left, i64 decl_right, i64 start, i64 max_count, i32 flags)
llvm::FunctionCallee Backend::fread_fn() {
    auto* ptr = raw_ptr_ty();
    auto* i32 = b_.getInt32Ty();
    auto* i64 = b_.getInt64Ty();
    return rt_fn("rt_fread", i32, { i32, ptr, ptr, i32, i32, i32, i32, i64, i64, i64, i64, i32 });
}

// -- Value-change dump (VCD, LRM 1800-2023 §21.7; design in docs/VCD.md) --

llvm::FunctionCallee Backend::vcd_open_fn() {
    return rt_fn("rt_vcd_open", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty() });
}

// void rt_vcd_begin(rt_wheel*, i64 tick_fs, i32 units)
llvm::FunctionCallee Backend::vcd_begin_fn() {
    return rt_fn("rt_vcd_begin", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::vcd_scope_fn() {
    return rt_fn("rt_vcd_scope", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::vcd_upscope_fn() {
    return rt_fn("rt_vcd_upscope", b_.getVoidTy(), {});
}

// void rt_vcd_var(i8* name, i32 len, i32 width, i32 flags, i8* mm, i8* z)
llvm::FunctionCallee Backend::vcd_var_fn() {
    auto* ptr = raw_ptr_ty();
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_vcd_var", b_.getVoidTy(), { ptr, i32, i32, i32, ptr, ptr });
}

// void rt_vcd_array(i8* name, i32 len, i32 count, i32 width, i32 flags, i8* mm_base,
//                   i8* z_base, i32 stride, i64 left, i64 right)
llvm::FunctionCallee Backend::vcd_array_fn() {
    auto* ptr = raw_ptr_ty();
    auto* i32 = b_.getInt32Ty();
    auto* i64 = b_.getInt64Ty();
    return rt_fn("rt_vcd_array", b_.getVoidTy(), { ptr, i32, i32, i32, i32, ptr, ptr, i32, i64, i64 });
}

llvm::FunctionCallee Backend::vcd_start_fn() {
    return rt_fn("rt_vcd_start", b_.getVoidTy(), {});
}

llvm::FunctionCallee Backend::vcd_limit_fn() {
    return rt_fn("rt_vcd_limit", b_.getVoidTy(), { b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::vcd_touch_fn() {
    return rt_fn("rt_vcd_touch", b_.getVoidTy(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::vcd_flush_fn() {
    return rt_fn("rt_vcd_flush", b_.getVoidTy(), { b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::vcd_close_fn() {
    return rt_fn("rt_vcd_close", b_.getVoidTy(), {});
}

llvm::FunctionCallee Backend::vcd_nullary_fn(const char* name) {
    return rt_fn(name, b_.getVoidTy(), {});
}

llvm::FunctionCallee Backend::vcd_touch_range_fn() {
    return rt_fn("rt_vcd_touch_range", b_.getVoidTy(), { b_.getInt32Ty(), b_.getInt32Ty() });
}

// -- $random --

llvm::FunctionCallee Backend::random_fn() {
    return rt_fn("rt_random", b_.getInt32Ty(), {});
}

// -- Probabilistic distribution functions (LRM §20.15.2) and $random(seed) --

// rt_dist_<op>(i32 seed, i32 params...) -> i64 {seed:32, draw:32}. `nparam`
// is the count of distribution parameters after the seed (1 or 2).
llvm::FunctionCallee Backend::dist_fn(const char* name, unsigned nparam) {
    std::vector<llvm::Type*> params(nparam + 1, b_.getInt32Ty());
    return rt_fn(name, b_.getInt64Ty(), params);
}

// -- $urandom / $urandom_range (LRM §18.13) --

// void rt_urandom_seed(i64* state, i64 seed): splitmix64 expansion of a
// 64-bit selector into the process's xoshiro256++ frame state.
llvm::FunctionCallee Backend::urandom_seed_fn() {
    return rt_fn("rt_urandom_seed", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty() });
}

// -- randc cyclic randomization (LRM §18.6) --

// i32 rt_randc_next(i64* seen, i32 nvals, i64* rng): one cyclic draw for a
// randc property (LRM §18.6), against that property's remaining-value bitmap
// and the object's own RNG state. Both are passed as raw pointers to class
// properties, so the runtime needs no knowledge of the class layout.
llvm::FunctionCallee Backend::randc_next_fn() {
    return rt_fn("rt_randc_next", b_.getInt32Ty(), { raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

// -- Plusargs (LRM §21.6) --

// i32 rt_test_plusargs(const char* s, i32 len)
llvm::FunctionCallee Backend::test_plusargs_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(i32, { ptr, i32 }, false);
    return env_->llmod.getOrInsertFunction("rt_test_plusargs", fnty);
}

// i32 rt_value_plusargs(const char* prefix, i32 prefix_len, i32 conv, const rt_scan_arg* arg)
llvm::FunctionCallee Backend::value_plusargs_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* fnty = llvm::FunctionType::get(i32, { ptr, i32, i32, ptr }, false);
    return env_->llmod.getOrInsertFunction("rt_value_plusargs", fnty);
}

// i32 rt_value_plusargs_str(const char* prefix, i32 prefix_len, rt_string* dst,
//                           const rt_alloc_site*)
llvm::FunctionCallee Backend::value_plusargs_str_fn() {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* fnty = llvm::FunctionType::get(b_.getInt32Ty(), { ptr, b_.getInt32Ty(), ptr, ptr }, false);
    return env_->llmod.getOrInsertFunction("rt_value_plusargs_str", fnty);
}

// -- Deferred nonblocking writes to unpacked-array elements --

// Deferred array-NBA record buffers (rt_nba_arr_*). The runtime owns the
// growable per-array buffers; the backend defines and fills the fixed-stride
// records and emits the monomorphized commit.
llvm::FunctionCallee Backend::nba_arr_slot_fn() {
    return rt_fn("rt_nba_arr_slot", raw_ptr_ty(), { b_.getInt32Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::nba_arr_base_fn() {
    return rt_fn("rt_nba_arr_base", raw_ptr_ty(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::nba_arr_count_fn() {
    return rt_fn("rt_nba_arr_count", b_.getInt32Ty(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::nba_arr_any_fn() {
    return rt_fn("rt_nba_arr_any", b_.getInt32Ty(), {});
}

llvm::FunctionCallee Backend::nba_arr_reset_fn() {
    return rt_fn("rt_nba_arr_reset", b_.getVoidTy(), {});
}

// -- Container element lifecycle descriptors --

// The rt_elem_ops mirror type (Runtime.h:947-958):
// { i32 kind, i32 nleaves, i64 stride, ptr proto, ptr inner, ptr leaves,
//   i32 nhandles, i32 nkeyhandles, ptr handles, ptr keyhandles }.
// Must match the runtime struct exactly.
//
// The last four fields are the collector's (docs/design/garbage-collection.md
// §3.3) and are APPENDED, so every field that predates them keeps its offset and
// this mirror grows in lockstep by growing (Runtime.h:909-911).
llvm::StructType* Backend::elem_ops_ty() {
    if (elem_ops_ty_ == nullptr) {
        elem_ops_ty_ = llvm::StructType::create(
          ctx_,
          { b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt64Ty(), raw_ptr_ty(), raw_ptr_ty(),
            raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty(), raw_ptr_ty() },
          "clotho.elemops");
    }
    return elem_ops_ty_;
}

// The rt_elem_leaf mirror type: { i64 offset, ptr ops }.
llvm::StructType* Backend::elem_leaf_ty() {
    if (elem_leaf_ty_ == nullptr) {
        elem_leaf_ty_ = llvm::StructType::create(ctx_, { b_.getInt64Ty(), raw_ptr_ty() }, "clotho.elemleaf");
    }
    return elem_leaf_ty_;
}

// -- SystemVerilog dynamic arrays --

// The opaque rt_dynarr handle slot type
// (`%clotho.dynarr = { ptr, i64, i64 }`), memoized. Must mirror the runtime
// rt_dynarr layout ({ void* data, u64 len, u64 epoch }).
llvm::StructType* Backend::dynarr_slot_ty() {
    if (dynarr_slot_ty_ == nullptr) {
        dynarr_slot_ty_ = llvm::StructType::create(
          ctx_, { raw_ptr_ty(), b_.getInt64Ty(), b_.getInt64Ty() }, "clotho.dynarr");
    }
    return dynarr_slot_ty_;
}

// -- Dynamic array runtime (rt_dynarr_*, LRM 7.5) --
// void rt_dynarr_new(rt_dynarr*, i64 stride, i64 n, const void* proto)
// void rt_dynarr_new(rt_dynarr*, const rt_elem_ops*, i64 n, const rt_alloc_site*)
llvm::FunctionCallee Backend::dynarr_new_fn() {
    return rt_fn("rt_dynarr_new", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
}

// void rt_dynarr_new_copy(rt_dynarr*, i64 stride, i64 n, const void* proto, const rt_dynarr* src)
// void rt_dynarr_new_copy(rt_dynarr*, const rt_elem_ops*, i64 n, const rt_dynarr* src,
//                         const rt_alloc_site*)
llvm::FunctionCallee Backend::dynarr_new_copy_fn() {
    return rt_fn("rt_dynarr_new_copy", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_dynarr_delete(rt_dynarr*, const rt_elem_ops*)
llvm::FunctionCallee Backend::dynarr_delete_fn() {
    return rt_fn("rt_dynarr_delete", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_dynarr_assign(rt_dynarr*, const rt_dynarr*, const rt_elem_ops*, const rt_alloc_site*)
llvm::FunctionCallee Backend::dynarr_assign_fn() {
    return rt_fn("rt_dynarr_assign", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_dynarr_move(rt_dynarr* dst, rt_dynarr* src, const rt_elem_ops*)
llvm::FunctionCallee Backend::dynarr_move_fn() {
    return rt_fn("rt_dynarr_move", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// -- Array ordering methods (LRM 7.12.2) --

// void rt_dynarr_reverse(rt_dynarr*, const rt_elem_ops*)  (LRM 7.12.2)
llvm::FunctionCallee Backend::dynarr_reverse_fn() {
    return rt_fn("rt_dynarr_reverse", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_dynarr_sort(rt_dynarr*, const rt_elem_ops*, i32 kind, i32 width, i32 rev)
llvm::FunctionCallee Backend::dynarr_sort_fn() {
    return rt_fn("rt_dynarr_sort", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(),
                   b_.getInt32Ty() });
}

// void rt_dynarr_shuffle(rt_dynarr*, const rt_elem_ops*, i64* rng)  (LRM 7.12.2)
llvm::FunctionCallee Backend::dynarr_shuffle_fn() {
    return rt_fn("rt_dynarr_shuffle", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// -- SystemVerilog queues --

// The opaque rt_queue ring handle slot type
// (`%clotho.queue = { ptr, i64, i64, i64, i64 }`), memoized. Must mirror the
// runtime rt_queue layout
// ({ void* base, u64 head, u64 len, u64 cap, u64 epoch }).
llvm::StructType* Backend::queue_slot_ty() {
    if (queue_slot_ty_ == nullptr) {
        queue_slot_ty_ = llvm::StructType::create(ctx_,
                                                  { raw_ptr_ty(), b_.getInt64Ty(),
                                                    b_.getInt64Ty(), b_.getInt64Ty(),
                                                    b_.getInt64Ty() },
                                                  "clotho.queue");
    }
    return queue_slot_ty_;
}

// void rt_dynarr_assign_queue(rt_dynarr*, const rt_queue*, const rt_elem_ops*, const rt_alloc_site*)
llvm::FunctionCallee Backend::dynarr_assign_queue_fn() {
    return rt_fn("rt_dynarr_assign_queue", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// -- Queue runtime (rt_queue_*, LRM 7.10) --
// void rt_queue_push_{back,front}(rt_queue*, const rt_elem_ops*, u64 bound, const void* elem,
//                                 const rt_alloc_site*)
llvm::FunctionCallee Backend::queue_push_fn(bool front) {
    return rt_fn(front ? "rt_queue_push_front" : "rt_queue_push_back", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_queue_pop_{back,front}(rt_queue*, const rt_elem_ops*, void* out)
llvm::FunctionCallee Backend::queue_pop_fn(bool front) {
    return rt_fn(front ? "rt_queue_pop_front" : "rt_queue_pop_back", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_queue_insert(rt_queue*, const rt_elem_ops*, u64 bound, u64 i, const void* elem,
//                      const rt_alloc_site*)
llvm::FunctionCallee Backend::queue_insert_fn() {
    return rt_fn("rt_queue_insert", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), b_.getInt64Ty(), raw_ptr_ty(),
                   raw_ptr_ty() });
}

// void rt_queue_delete_idx(rt_queue*, const rt_elem_ops*, u64 i)
llvm::FunctionCallee Backend::queue_delete_idx_fn() {
    return rt_fn("rt_queue_delete_idx", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
}

// void rt_queue_delete_all(rt_queue*, const rt_elem_ops*)
llvm::FunctionCallee Backend::queue_delete_all_fn() {
    return rt_fn("rt_queue_delete_all", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_queue_assign_dynarr(rt_queue*, const rt_dynarr*, const rt_elem_ops*,
//                             const rt_alloc_site*)
llvm::FunctionCallee Backend::queue_assign_dynarr_fn() {
    return rt_fn("rt_queue_assign_dynarr", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_queue_assign(rt_queue*, const rt_queue*, const rt_elem_ops*, const rt_alloc_site*)
llvm::FunctionCallee Backend::queue_assign_fn() {
    return rt_fn("rt_queue_assign", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_queue_move(rt_queue* dst, rt_queue* src, const rt_elem_ops*)
llvm::FunctionCallee Backend::queue_move_fn() {
    return rt_fn("rt_queue_move", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_queue_append_range(rt_queue* dst, const rt_queue* src,
//                            const rt_elem_ops*, u64 bound, i64 lo, i64 hi,
//                            const rt_alloc_site*)
llvm::FunctionCallee Backend::queue_append_range_fn() {
    return rt_fn("rt_queue_append_range", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), b_.getInt64Ty(),
                   b_.getInt64Ty(), raw_ptr_ty() });
}

// void rt_queue_reverse(rt_queue*, const rt_elem_ops*)  (LRM 7.12.2)
llvm::FunctionCallee Backend::queue_reverse_fn() {
    return rt_fn("rt_queue_reverse", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_queue_sort(rt_queue*, const rt_elem_ops*, i32 kind, i32 width, i32 rev)
llvm::FunctionCallee Backend::queue_sort_fn() {
    return rt_fn("rt_queue_sort", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(),
                   b_.getInt32Ty() });
}

// void rt_queue_shuffle(rt_queue*, const rt_elem_ops*, i64* rng)  (LRM 7.12.2)
llvm::FunctionCallee Backend::queue_shuffle_fn() {
    return rt_fn("rt_queue_shuffle", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_container_format_p(rt_string* out, const void* handle, i32 is_queue,
//   i32 kind, i32 width, i32 is_signed, i32 four_state, i32 has_z, i32 z_off, i64 stride,
//   const rt_alloc_site*)
llvm::FunctionCallee Backend::container_fmt_p_fn() {
    return rt_fn("rt_container_format_p", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty(),
                   b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty(),
                   b_.getInt64Ty(), raw_ptr_ty() });
}

// void rt_tagged_format_p(rt_string* dst, const rt_arg* v, u32 width, u32 tag_bits,
//                         u32 count, const char* const* names, const u32* name_lens,
//                         const u32* widths, const u32* signs, const rt_alloc_site*)
llvm::FunctionCallee Backend::tagged_fmt_p_fn() {
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_tagged_format_p", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), i32, i32, i32, raw_ptr_ty(), raw_ptr_ty(),
                   raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// i8 rt_container_eq(const void* a, const void* b, i32 is_queue, i32 kind, i32 width,
//   i32 is_signed, i32 four_state, i32 has_z, i32 z_off, i64 stride)
llvm::FunctionCallee Backend::container_eq_fn() {
    return rt_fn("rt_container_eq", b_.getInt8Ty(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty(),
                   b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty(),
                   b_.getInt64Ty() });
}

// -- Associative-array key scratch buffer (docs/design/assoc-keys.md P1) --

// -- Variable-length associative-array key scratch (docs/design/
// assoc-keys.md P1). See Runtime.h for the mark/pop contract. --
// u32 rt_keybuf_begin(void)
llvm::FunctionCallee Backend::keybuf_begin_fn() {
    return rt_fn("rt_keybuf_begin", b_.getInt32Ty(), {});
}

// void rt_keybuf_append(const void* bytes, u32 len)
llvm::FunctionCallee Backend::keybuf_append_fn() {
    return rt_fn("rt_keybuf_append", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() });
}

// void rt_keybuf_span(u32 mark, const u8** out_ptr, u32* out_len)
llvm::FunctionCallee Backend::keybuf_span_fn() {
    return rt_fn("rt_keybuf_span", b_.getVoidTy(), { b_.getInt32Ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_keybuf_end(u32 mark)
llvm::FunctionCallee Backend::keybuf_end_fn() {
    return rt_fn("rt_keybuf_end", b_.getVoidTy(), { b_.getInt32Ty() });
}

// u8 rt_key_span_eq(const u8* a, u32 alen, const u8* b, u32 blen)
llvm::FunctionCallee Backend::key_span_eq_fn() {
    return rt_fn("rt_key_span_eq", b_.getInt8Ty(),
                 { raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty(), b_.getInt32Ty() });
}

// -- SystemVerilog associative arrays --

// The opaque rt_assoc handle slot type (`%clotho.assoc = { ptr, i64 }`),
// memoized. Must mirror the runtime rt_assoc layout
// ({ void* impl, u64 epoch }).
llvm::StructType* Backend::assoc_slot_ty() {
    if (assoc_slot_ty_ == nullptr) {
        assoc_slot_ty_ =
          llvm::StructType::create(ctx_, { raw_ptr_ty(), b_.getInt64Ty() }, "clotho.assoc");
    }
    return assoc_slot_ty_;
}

// -- Associative-array runtime entry points (LRM 7.8/7.9). Keys pass as a
// normalized byte span (ptr + i32 len); the value element lifecycle rides the
// rt_elem_ops descriptor. See docs/design/dynamic-arrays.md §6. --
// void* rt_assoc_get_ptr(rt_assoc*, const u8* kptr, u32 klen)
llvm::FunctionCallee Backend::assoc_get_ptr_fn() {
    return rt_fn("rt_assoc_get_ptr", raw_ptr_ty(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
}

// void* rt_assoc_put_ptr(rt_assoc*, const rt_elem_ops*, const u8* kptr, u32 klen,
//                        const rt_alloc_site*)
llvm::FunctionCallee Backend::assoc_put_ptr_fn() {
    return rt_fn("rt_assoc_put_ptr", raw_ptr_ty(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

// i32 rt_assoc_exists(rt_assoc*, const u8* kptr, u32 klen)
llvm::FunctionCallee Backend::assoc_exists_fn() {
    return rt_fn("rt_assoc_exists", b_.getInt32Ty(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
}

// void rt_assoc_delete_key(rt_assoc*, const rt_elem_ops*, const u8* kptr, u32 klen)
llvm::FunctionCallee Backend::assoc_delete_key_fn() {
    return rt_fn("rt_assoc_delete_key", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
}

// void rt_assoc_delete_all(rt_assoc*, const rt_elem_ops*)
llvm::FunctionCallee Backend::assoc_delete_all_fn() {
    return rt_fn("rt_assoc_delete_all", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

// void* rt_assoc_default_ptr(rt_assoc*, const rt_elem_ops*, const rt_alloc_site*)
llvm::FunctionCallee Backend::assoc_default_ptr_fn() {
    return rt_fn("rt_assoc_default_ptr", raw_ptr_ty(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_assoc_reset(rt_assoc*, const rt_elem_ops*)
llvm::FunctionCallee Backend::assoc_reset_fn() {
    return rt_fn("rt_assoc_reset", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

// u64 rt_assoc_num(rt_assoc*)
llvm::FunctionCallee Backend::assoc_num_fn() {
    return rt_fn("rt_assoc_num", b_.getInt64Ty(), { raw_ptr_ty() });
}

// i32 rt_assoc_first/last(rt_assoc*, const u8** out_ptr, u32* out_len)
llvm::FunctionCallee Backend::assoc_end_fn(bool last) {
    return rt_fn(last ? "rt_assoc_last" : "rt_assoc_first", b_.getInt32Ty(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// i32 rt_assoc_next/prev(rt_assoc*, const u8* cur, u32 curlen, const u8** out, u32* outlen)
llvm::FunctionCallee Backend::assoc_step_fn(bool prev) {
    return rt_fn(prev ? "rt_assoc_prev" : "rt_assoc_next", b_.getInt32Ty(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_assoc_assign(rt_assoc* dst, const rt_assoc* src, const rt_elem_ops*,
//                      const rt_alloc_site*)
llvm::FunctionCallee Backend::assoc_assign_fn() {
    return rt_fn("rt_assoc_assign", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_assoc_move(rt_assoc* dst, rt_assoc* src, const rt_elem_ops*)
llvm::FunctionCallee Backend::assoc_move_fn() {
    return rt_fn("rt_assoc_move", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// -- SystemVerilog string objects --

// String runtime (rt_string_*); every ptr is an rt_string handle except
// where noted.
llvm::FunctionCallee Backend::str_init_fn() {
    return rt_fn("rt_string_init", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_deinit_fn() {
    return rt_fn("rt_string_deinit", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_assign_fn() {
    return rt_fn("rt_string_assign", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_string_move(rt_string* dst, rt_string* src)
llvm::FunctionCallee Backend::str_move_fn() {
    return rt_fn("rt_string_move", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_string_assign_bytes(rt_string*, const char* bytes, i64 len, const rt_alloc_site*)
llvm::FunctionCallee Backend::str_assign_bytes_fn() {
    return rt_fn("rt_string_assign_bytes", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_len_fn() {
    return rt_fn("rt_string_len", b_.getInt64Ty(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_data_fn() {
    return rt_fn("rt_string_data", raw_ptr_ty(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_concat_fn() {
    return rt_fn("rt_string_concat", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_repl_fn() {
    return rt_fn("rt_string_repl", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
}

// [[noreturn]] void rt_string_repl_invalid(i64 count, i32 unknown, ptr site, i64 now)
// — the located fatal for a negative or x/z runtime replication multiplier
// (the LRM 11.4.12.1 string deviation; see lower/Expr.cpp's Replication case).
llvm::FunctionCallee Backend::str_repl_invalid_fn() {
    return rt_fn("rt_string_repl_invalid", b_.getVoidTy(),
                 { b_.getInt64Ty(), b_.getInt32Ty(), raw_ptr_ty(), b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::str_cmp_fn() {
    return rt_fn("rt_string_cmp", b_.getInt32Ty(), { raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_icmp_fn() {
    return rt_fn("rt_string_icmp", b_.getInt32Ty(), { raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_substr_fn() {
    return rt_fn("rt_string_substr", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_upper_fn() {
    return rt_fn("rt_string_toupper", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

// void rt_enum_name(rt_string* dst, const u8* val, u32 vbytes, u32 count,
//                   const u8* values, const char* const* names, const u32* lens,
//                   u32 width, u32 fmt_fallback, u32 is_signed, const rt_alloc_site*)
llvm::FunctionCallee Backend::enum_name_fn() {
    auto* i32 = b_.getInt32Ty();
    return rt_fn("rt_enum_name", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), i32, i32, raw_ptr_ty(), raw_ptr_ty(),
                   raw_ptr_ty(), i32, i32, i32, raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_lower_fn() {
    return rt_fn("rt_string_tolower", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_putc_fn() {
    return rt_fn("rt_string_putc", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::str_getc_fn() {
    return rt_fn("rt_string_getc", b_.getInt32Ty(), { raw_ptr_ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::str_from_packed_fn() {
    return rt_fn("rt_string_from_packed", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_append_packed_fn() {
    return rt_fn("rt_string_append_packed", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_to_packed_fn() {
    return rt_fn("rt_string_to_packed", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::str_atoi_fn() {
    return rt_fn("rt_string_atoi", b_.getInt32Ty(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_atohex_fn() {
    return rt_fn("rt_string_atohex", b_.getInt32Ty(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_atooct_fn() {
    return rt_fn("rt_string_atooct", b_.getInt32Ty(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_atobin_fn() {
    return rt_fn("rt_string_atobin", b_.getInt32Ty(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_atoreal_fn() {
    return rt_fn("rt_string_atoreal", b_.getDoubleTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_itoa_fn() {
    return rt_fn("rt_string_itoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_hextoa_fn() {
    return rt_fn("rt_string_hextoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_octtoa_fn() {
    return rt_fn("rt_string_octtoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_bintoa_fn() {
    return rt_fn("rt_string_bintoa", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::str_realtoa_fn() {
    return rt_fn("rt_string_realtoa", b_.getVoidTy(),
                 { raw_ptr_ty(), b_.getDoubleTy(), raw_ptr_ty() });
}

// -- Coroutine frame allocation --

// Coroutine frame allocation, sim-owned: ptr rt_alloc(i64) / void rt_free(ptr).
llvm::FunctionCallee Backend::alloc_fn() {
    return rt_fn("rt_alloc", raw_ptr_ty(), { b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::free_fn() {
    return rt_fn("rt_free", b_.getVoidTy(), { raw_ptr_ty() });
}

// -- Run start (the process-global reset seam) --

llvm::FunctionCallee Backend::event_detach_register_fn() {
    return rt_fn("rt_event_detach_register", b_.getVoidTy(),
                 { b_.getInt32Ty(), b_.getInt64Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::event_detach_fire_fn() {
    return rt_fn("rt_event_detach_fire", b_.getInt32Ty(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::event_detach_ready_fn() {
    return rt_fn("rt_event_detach_ready", b_.getInt64Ty(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::event_new_fn() {
    return rt_fn("rt_event_new", b_.getInt64Ty(), {});
}

llvm::FunctionCallee Backend::event_ptr_fn() {
    return rt_fn("rt_event_ptr", raw_ptr_ty(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::run_init_fn() {
    return rt_fn("rt_run_init", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt32Ty() });
}

// -- Process control blocks (the `process` class, LRM 9.7) --

llvm::FunctionCallee Backend::proc_alloc_fn() {
    return rt_fn("rt_proc_alloc", raw_ptr_ty(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_complete_fn() {
    return rt_fn("rt_proc_complete", raw_ptr_ty(), { raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_mark_kill_fn() {
    return rt_fn("rt_proc_mark_kill", raw_ptr_ty(), { raw_ptr_ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::proc_adopt_fn() {
    return rt_fn("rt_proc_adopt", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_thread_done_fn() {
    return rt_fn("rt_proc_thread_done", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_obj_retain_fn() {
    return rt_fn("rt_proc_obj_retain", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_await_self_fn() {
    return rt_fn("rt_proc_await_self", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::proc_randstate_get_fn() {
    return rt_fn("rt_proc_randstate_get", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_randstate_set_fn() {
    return rt_fn("rt_proc_randstate_set", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
}

// -- Timing wheel --

// Timing wheel (rt_wheel_*); the ptr parameter is the rt_wheel.
llvm::FunctionCallee Backend::wheel_init_fn() {
    return rt_fn("rt_wheel_init", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::wheel_deinit_fn() {
    return rt_fn("rt_wheel_deinit", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::oscillation_abort_fn() {
    return rt_fn("rt_oscillation_abort", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::ring_overflow_fn() {
    return rt_fn("rt_ring_overflow", b_.getVoidTy(), { b_.getInt32Ty(), b_.getInt32Ty() });
}

// The two DPI export-task fatals (LRM 35.8). Both take the exported task's
// c_identifier and never return; the thunk follows each with `unreachable`.
llvm::FunctionCallee Backend::dpi_export_task_suspended_fn() {
    return rt_fn("rt_dpi_export_task_suspended", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::dpi_export_task_no_task_frame_fn() {
    return rt_fn("rt_dpi_export_task_no_task_frame", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::wheel_now_fn() {
    return rt_fn("rt_wheel_now", b_.getInt64Ty(), { raw_ptr_ty() });
}

// void rt_wheel_schedule_rel(rt_wheel*, rt_timer*, i64 delay, rt_timer_fn)
llvm::FunctionCallee Backend::wheel_schedule_rel_fn() {
    return rt_fn("rt_wheel_schedule_rel", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
}

// int rt_wheel_advance(rt_wheel*, void* ctx)
llvm::FunctionCallee Backend::wheel_advance_fn() {
    return rt_fn("rt_wheel_advance", b_.getInt32Ty(), { raw_ptr_ty(), raw_ptr_ty() });
}

// -- Deferred nonblocking intra-assignment stores (LRM 9.4.5) --

// Deferred nonblocking intra-assignment stores (rt_intra_*, LRM 9.4.5).
llvm::FunctionCallee Backend::intra_alloc_fn() {
    return rt_fn("rt_intra_alloc", raw_ptr_ty(), { b_.getInt32Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::intra_cb_fn() {
    return rt_fn("rt_intra_cb", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::intra_ready_base_fn() {
    return rt_fn("rt_intra_ready_base", raw_ptr_ty(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::intra_ready_count_fn() {
    return rt_fn("rt_intra_ready_count", b_.getInt32Ty(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::intra_any_fn() {
    return rt_fn("rt_intra_any", b_.getInt32Ty(), {});
}

llvm::FunctionCallee Backend::intra_release_fn() {
    return rt_fn("rt_intra_release", b_.getVoidTy(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::intra_reset_fn() {
    return rt_fn("rt_intra_reset", b_.getVoidTy(), {});
}

// -- Inertial continuous-assignment delay (`assign #d lhs = rhs`, LRM 10.3.3) --

// Inertial continuous-assignment delay (rt_inertial_*, LRM 10.3.3). The
// decide call takes plain 0/1 flags (not the value planes themselves), so
// it is width-agnostic — the backend does the value compares in IR against
// the pointer rt_inertial_buf returns.
llvm::FunctionCallee Backend::inertial_buf_fn() {
    return rt_fn("rt_inertial_buf", raw_ptr_ty(), { b_.getInt32Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::inertial_decide_fn() {
    auto* i64 = b_.getInt64Ty();
    return rt_fn("rt_inertial_decide", i64, { b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::inertial_cb_fn() {
    return rt_fn("rt_inertial_cb", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

// -- Event-triggered deferred stores (`x <= @e y` / `x <= repeat(N) @e y`) --

llvm::FunctionCallee Backend::intra_event_register_fn() {
    return rt_fn("rt_intra_event_register", b_.getVoidTy(), { b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::intra_event_register_group_fn() {
    return rt_fn("rt_intra_event_register_group", b_.getVoidTy(),
                 { raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::intra_event_fire_fn() {
    return rt_fn("rt_intra_event_fire", b_.getVoidTy(), { b_.getInt32Ty() });
}

// -- DPI-C packed-vector marshalling (LRM 35.5.6) --

llvm::FunctionCallee Backend::dpi_pack_logicvec_fn() {
    auto* p = raw_ptr_ty();
    return rt_fn("rt_dpi_pack_logicvec", b_.getVoidTy(), { p, p, p, b_.getInt32Ty(), p });
}

llvm::FunctionCallee Backend::dpi_unpack_logicvec_fn() {
    auto* p = raw_ptr_ty();
    return rt_fn("rt_dpi_unpack_logicvec", b_.getVoidTy(), { p, b_.getInt32Ty(), p, p, p });
}

// void rt_dpi_str_out(rt_string* dst, const char* p, const rt_alloc_site*) — copy a
// returned C string.
llvm::FunctionCallee Backend::dpi_str_out_fn() {
    auto* p = raw_ptr_ty();
    return rt_fn("rt_dpi_str_out", b_.getVoidTy(), { p, p, p });
}

// -- DPI open arrays (LRM 35.5.6.2) --

// The LLVM struct mirroring the runtime rt_svopen descriptor (Runtime.h):
// { ptr data, i32 left, i32 right, i32 nelem, i32 elem_bytes, i32 elem_width,
// i32 is_logic }. Memoized; must match the C layout exactly.
llvm::StructType* Backend::rt_svopen_ty() {
    if (rt_svopen_ty_ == nullptr) {
        auto* i32 = b_.getInt32Ty();
        rt_svopen_ty_ = llvm::StructType::create(
          ctx_, { raw_ptr_ty(), i32, i32, i32, i32, i32, i32 }, "clotho.svopen");
    }
    return rt_svopen_ty_;
}

// -- Garbage collector struct mirrors and entry points (src/runtime/Gc.h) --

// rt_gc_map (Gc.h:85-92):
// { i64 size, i32 nhandles, i32 nleaves, ptr handles, ptr leaves, ptr finalize }.
llvm::StructType* Backend::gc_map_ty() {
    if (gc_map_ty_ == nullptr) {
        gc_map_ty_ = llvm::StructType::create(
          ctx_,
          { b_.getInt64Ty(), b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty(), raw_ptr_ty(),
            raw_ptr_ty() },
          "clotho.gcmap");
    }
    return gc_map_ty_;
}

// rt_gc_roots (Gc.h:100-105): { ptr base, i32 nhandles, i32 reserved, ptr offsets }.
llvm::StructType* Backend::gc_roots_ty() {
    if (gc_roots_ty_ == nullptr) {
        gc_roots_ty_ = llvm::StructType::create(
          ctx_, { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty() },
          "clotho.gcroots");
    }
    return gc_roots_ty_;
}

// rt_gc_container (Gc.h:249-254): { i64 offset, i32 kind, i32 reserved, ptr ops }.
// `ops` is the container's OWN descriptor (kind 2 / 3 / 5), so ops->kind equals
// `kind` and ops->inner describes the element -- the convention nested container
// elements already use, which is what lets the collector treat a root entry, a
// class-map leaf and a nested element as one shape.
llvm::StructType* Backend::gc_container_ty() {
    if (gc_container_ty_ == nullptr) {
        gc_container_ty_ = llvm::StructType::create(
          ctx_, { b_.getInt64Ty(), b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty() },
          "clotho.gccontainer");
    }
    return gc_container_ty_;
}

// rt_gc_container_table (Gc.h:256-261):
// { ptr base, i32 count, i32 reserved, ptr entries }.
llvm::StructType* Backend::gc_container_table_ty() {
    if (gc_container_table_ty_ == nullptr) {
        gc_container_table_ty_ = llvm::StructType::create(
          ctx_, { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty() },
          "clotho.gccontainers");
    }
    return gc_container_table_ty_;
}

llvm::StructType* Backend::gc_frame_ty() {
    if (gc_frame_ty_ == nullptr) {
        gc_frame_ty_ = llvm::StructType::create(
          ctx_, { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() },
          "clotho.gcframe");
    }
    return gc_frame_ty_;
}

// rt_gc_mutator (Gc.h:163-182): { ptr cursor, ptr limit, i64 budget_left,
// i32 block_index, i32 request, ptr block_base, ptr start_bitmap,
// ptr fin_bitmap, ptr frames_head }.
//
// Mirrored because this one is an ADDRESSING contract, not just an argument
// type, and since P2 it is the widest one in the file: generated code reads AND
// WRITES these fields directly. `request` at every safepoint
// (emit_gc_safepoint); `cursor` / `limit` / `block_base` / `start_bitmap` /
// `fin_bitmap` at every inlined allocation (emit_gc_alloc_fast); `frames_head` at
// every coroutine ramp and cleanup (emit_gc_frame_link / emit_gc_frame_unlink).
// So a field added, widened or reordered ahead of any of them silently moves the
// bytes generated code touches -- and unlike the poll, the allocator's stores
// would corrupt the heap rather than merely misread a flag.
//
// rt_gc_init catches exactly that: gc_mutator_request_offset() and
// gc_mutator_bytes() below are handed over in rt_gc_config and compared against
// the runtime's own offsetof and sizeof (Gc.h:196-203). The size check is what
// makes the other eight offsets trustworthy: this mirror and the C struct are
// both padding-free LP64 layouts, so agreeing on the total is agreeing on every
// field boundary.
llvm::StructType* Backend::gc_mutator_ty() {
    if (gc_mutator_ty_ == nullptr) {
        gc_mutator_ty_ = llvm::StructType::create(
          ctx_,
          { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty(), b_.getInt32Ty(), b_.getInt32Ty(),
            raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() },
          "clotho.gcmutator");
    }
    return gc_mutator_ty_;
}

// rt_gc_config (Gc.h:233-246): { i64 nursery_bytes, i64 heap_bytes_max,
// i32 stress, i32 stats, i32 map_vptr_offset, i32 request_offset,
// i32 mutator_bytes, i32 full_every, i32 reserved }.
//
// `full_every` is the generational policy knob (Gc.h:241-245): one full
// collection per that many collections, 0 leaving the choice to the runtime's
// heap-growth heuristic. It is a config field rather than a mode because a young
// collection reclaims no old garbage, so how often the collector re-derives which
// lines hold old survivors is a tuning question, not a compile-time one.
llvm::StructType* Backend::gc_config_ty() {
    if (gc_config_ty_ == nullptr) {
        gc_config_ty_ = llvm::StructType::create(
          ctx_,
          { b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt32Ty(), b_.getInt32Ty(),
            b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty(),
            b_.getInt32Ty() },
          "clotho.gcconfig");
    }
    return gc_config_ty_;
}

// -- Garbage collection entry points (src/runtime/Gc.h) ----------------------
// rt_fn declares on demand, so each of these lands in the module only if some
// emitter actually calls it: a design that never allocates carries no
// rt_gc_alloc declaration, and one whose every frame is unregistered carries no
// rt_gc_frame_link.
llvm::FunctionCallee Backend::gc_init_fn() {
    return rt_fn("rt_gc_init", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::gc_shutdown_fn() {
    return rt_fn("rt_gc_shutdown", b_.getVoidTy(), {});
}

llvm::FunctionCallee Backend::gc_mutator_fn() {
    return rt_fn("rt_gc_mutator_current", raw_ptr_ty(), {});
}

llvm::FunctionCallee Backend::gc_alloc_fn() {
    return rt_fn("rt_gc_alloc", raw_ptr_ty(), { raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::gc_roots_add_fn() {
    return rt_fn("rt_gc_roots_add", b_.getVoidTy(), { raw_ptr_ty() });
}

// The container-root ABI (Gc.h:263-268). rt_gc_containers_add takes a constant
// rt_gc_container_table for state-resident storage; the add / remove pair takes a
// live rt_dynarr / rt_queue / rt_assoc address for a frame local or a body
// temporary.
llvm::FunctionCallee Backend::gc_containers_add_fn() {
    return rt_fn("rt_gc_containers_add", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::gc_container_add_fn() {
    return rt_fn("rt_gc_container_add", b_.getVoidTy(),
                 { raw_ptr_ty(), b_.getInt32Ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::gc_container_remove_fn() {
    return rt_fn("rt_gc_container_remove", b_.getVoidTy(), { raw_ptr_ty() });
}

// The out-of-line half of the inlined allocation fast path (Gc.h:243-253): the
// current hole cannot satisfy the request, so the runtime refills and allocates,
// and refreshes cursor / limit / block_base / start_bitmap / fin_bitmap. Reached
// only on the cold edge emit_gc_alloc_fast branches to.
llvm::FunctionCallee Backend::gc_alloc_slow_fn() {
    return rt_fn("rt_gc_alloc_slow", raw_ptr_ty(),
                 { raw_ptr_ty(), b_.getInt64Ty(), raw_ptr_ty() });
}

// Only the slow half is ever emitted: generated code polls
// rt_gc_mutator::request inline (emit_gc_safepoint) and calls this exactly on
// the taken edge, so reaching it means a collection is due. rt_gc_safepoint,
// the polling wrapper, stays in the ABI for the runtime's own call sites and
// the tests (Gc.h:235-245) and is no longer named by any emitted module.
llvm::FunctionCallee Backend::gc_safepoint_slow_fn() {
    return rt_fn("rt_gc_safepoint_slow", b_.getVoidTy(), {});
}

// The out-of-line half of the inlined write barrier (Gc.h:98-108). Generated code
// tests `(gcword & (OLD|LOGGED)) == OLD` inline and calls this only on the taken
// edge, so reaching it means an OLD object has just acquired a reference a young
// collection would otherwise never look at. Idempotent -- it sets LOGGED -- so a
// hot object costs the call once per collection cycle, not once per store.
llvm::FunctionCallee Backend::gc_remember_fn() {
    return rt_fn("rt_gc_remember", b_.getVoidTy(), { raw_ptr_ty() });
}

// -- Functional coverage struct mirrors and entry points (src/runtime/Coverage.h) --

llvm::StructType* Backend::cover_range_ty() {
    if (cover_range_ty_ == nullptr) {
        cover_range_ty_ = llvm::StructType::create(ctx_, { b_.getInt64Ty(), b_.getInt64Ty() }, "clotho.coverrange");
    }
    return cover_range_ty_;
}

llvm::StructType* Backend::cover_bin_ty() {
    if (cover_bin_ty_ == nullptr) {
        cover_bin_ty_ = llvm::StructType::create(
          ctx_, { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty(), raw_ptr_ty() }, "clotho.coverbin");
    }
    return cover_bin_ty_;
}

llvm::StructType* Backend::cover_point_ty() {
    if (cover_point_ty_ == nullptr) {
        cover_point_ty_ = llvm::StructType::create(ctx_,
                                                   { raw_ptr_ty(),
                                                     raw_ptr_ty(),
                                                     b_.getInt32Ty(),
                                                     raw_ptr_ty(),
                                                     b_.getInt32Ty(),
                                                     b_.getInt32Ty(),
                                                     b_.getInt32Ty(),
                                                     b_.getInt32Ty(),
                                                     b_.getInt32Ty(),
                                                     b_.getInt32Ty() },
                                                   "clotho.coverpoint");
    }
    return cover_point_ty_;
}

llvm::StructType* Backend::cover_type_ty() {
    if (cover_type_ty_ == nullptr) {
        cover_type_ty_ = llvm::StructType::create(ctx_,
                                                  { raw_ptr_ty(),
                                                    raw_ptr_ty(),
                                                    b_.getInt32Ty(),
                                                    raw_ptr_ty(),
                                                    b_.getInt32Ty(),
                                                    b_.getInt32Ty(),
                                                    b_.getInt32Ty(),
                                                    raw_ptr_ty() },
                                                  "clotho.covertype");
    }
    return cover_type_ty_;
}

llvm::StructType* Backend::cover_config_ty() {
    if (cover_config_ty_ == nullptr) {
        cover_config_ty_ = llvm::StructType::create(ctx_,
                                                    { raw_ptr_ty(),
                                                      b_.getInt32Ty(),
                                                      b_.getInt32Ty(),
                                                      b_.getInt32Ty(),
                                                      b_.getInt32Ty(),
                                                      b_.getInt32Ty(),
                                                      raw_ptr_ty() },
                                                    "clotho.coverconfig");
    }
    return cover_config_ty_;
}

// -- Functional coverage entry points (src/runtime/Coverage.h) --------------
// Declared on demand like the collector's, so a design with no covergroup
// carries none of them and rt_cover_init is never called.
llvm::FunctionCallee Backend::cover_init_fn() {
    return rt_fn("rt_cover_init", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::cover_new_fn() {
    return rt_fn("rt_cover_new", b_.getInt64Ty(), { b_.getInt32Ty(), b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::cover_ctx_fn() {
    return rt_fn("rt_cover_ctx", b_.getInt64Ty(), { b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::cover_first_fn() {
    return rt_fn("rt_cover_first", b_.getInt64Ty(), { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::cover_next_fn() {
    return rt_fn("rt_cover_next", b_.getInt64Ty(), { b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::cover_hit_fn() {
    return rt_fn("rt_cover_hit", b_.getVoidTy(),
                 { b_.getInt64Ty(), b_.getInt32Ty(), b_.getInt64Ty(), b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::cover_start_fn() {
    return rt_fn("rt_cover_start", b_.getVoidTy(), { b_.getInt64Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::cover_set_name_fn() {
    return rt_fn("rt_cover_set_inst_name", b_.getVoidTy(),
                 { b_.getInt64Ty(), raw_ptr_ty(), b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::cover_inst_query_fn(const char* name, llvm::Type* ret) {
    return rt_fn(name, ret, { b_.getInt64Ty() });
}

llvm::FunctionCallee Backend::cover_point_query_fn(const char* name, llvm::Type* ret) {
    return rt_fn(name, ret, { b_.getInt64Ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::cover_type_query_fn(const char* name, llvm::Type* ret) {
    return rt_fn(name, ret, { b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::cover_design_coverage_fn() {
    return rt_fn("rt_cover_design_coverage", b_.getDoubleTy(), {});
}
} // namespace clotho::ir::backend
