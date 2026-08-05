cmake_minimum_required(VERSION 3.20)
project(clotho C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No CMAKE_BUILD_TYPE set; defaulting to RelWithDebInfo")
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Warnings apply to clotho targets only, not fetched dependencies. The flags are
# GCC/Clang spelling; a cl-style driver (MSVC, or clang-cl) takes the /-forms.
# Note clotho cannot be built by MSVC proper regardless — the runtime's wide
# arithmetic uses `unsigned __int128`, which only GCC and Clang provide — so the
# cl-style branch exists for clang-cl.
add_library(clotho_warnings INTERFACE)
if(MSVC)
  target_compile_options(clotho_warnings INTERFACE
    /W4
    # See the -Werror=switch rationale below.
    /we4062
  )
else()
  target_compile_options(clotho_warnings INTERFACE
    -Wall -Wextra
    # Omitted aggregate members value-initialize; flagging them fights the
    # designated-initializer style used throughout.
    -Wno-missing-field-initializers
    # The IR layer's correctness story for adding ops rests on exhaustive
    # switches over Inst::Tag in several consumers; a silently non-exhaustive
    # switch is a miscompile factory, so promote it to an error.
    -Werror=switch
  )
endif()

option(CLOTHO_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(CLOTHO_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "CLOTHO_SANITIZE requires a GCC/Clang driver")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)

# System LLVM. Used by the backend for native codegen. This install ships LLVM
# as a single shared library, so prefer the `LLVM` imported target; fall back
# to per-component static libs if only those are present.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
if(NOT LLVM_VERSION_MAJOR EQUAL 22)
  message(FATAL_ERROR "Clotho requires LLVM 22, found ${LLVM_PACKAGE_VERSION}")
endif()
if(TARGET LLVM)
  set(LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(LLVM_LIBS core support orcjit jitlink native bitwriter bitreader)
endif()

# slang declares fmt with `FIND_PACKAGE_ARGS 12.1`, so FetchContent silently
# prefers a system fmt over the pinned 12.1.0 it would otherwise fetch. That
# makes the build depend on whatever the host distro ships: a host fmt that is
# ABI- or API-divergent from the pin breaks the slang build outright (Arch's
# 12.1.0 does — `fmt::format` is not visible through the headers slang
# includes), and it silently varies the toolchain across CI platforms. Force the
# pinned source build so every host resolves fmt identically.
set(CMAKE_DISABLE_FIND_PACKAGE_fmt ON)

if(WIN32 AND NOT MSVC)
  # $finish longjmps out of the middle of a simulation, back to the setjmp in
  # run_jit, deliberately abandoning the JIT'd coroutine frames it jumps over.
  # Windows' longjmp is SEH-based: it *unwinds*, which needs valid unwind data
  # for every frame in between, and JIT'd code registers none — so the unwind
  # aborts the process instead of returning to the host. mingw-w64 selects the
  # plain non-unwinding `_setjmp(buf, NULL)` when this is defined, which is the
  # semantics clotho wants (and what glibc and Darwin already give it). Applied
  # as a compile definition rather than a #define so it precedes every
  # transitive <setjmp.h>.
  add_compile_definitions(__USE_MINGW_SETJMP_NON_APP_CONTEXT)
endif()

if(WIN32)
  # slang defaults to mimalloc, whose malloc/free/realloc/_msize overrides
  # collide with the Windows CRT's own at link time (lld-link reports every one
  # as a duplicate symbol). It is an allocator speedup, not a dependency of any
  # slang API clotho uses.
  set(SLANG_USE_MIMALLOC OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
  slang
  GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
  # Post-8.0: the v8.0 release tag predates the slang analysis library
  # (AnalysisManager, dataflow diagnostics like inferred-latch), which the
  # frontend runs after elaboration.
  GIT_TAG        6933832c97c39976890e5f12b69245f061d60a9a # v10.0-360-g6933832c9
)
FetchContent_MakeAvailable(slang)

if(WIN32)
  # slang pins its own build to C++20 while clotho is C++23. That is normally
  # harmless, but expected-lite (which backs slang's nonstd::expected) forwards
  # to std::expected exactly when <expected> is available — C++23 on the MSVC
  # STL. So slang compiles SyntaxTree::fromFile returning its own expected type
  # while clotho calls one returning std::expected, and the link fails on the
  # mangled mismatch. Build slang at clotho's standard so both agree.
  set_target_properties(slang_slang PROPERTIES CXX_STANDARD 23)
endif()

# Header-only: SOURCE_SUBDIR none skips doctest's own CMakeLists (its
# cmake_minimum_required predates CMake 4) and just populates the source.
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        ae7a13539fb71f270b87eb2e874fbac80bc8dda2 # v2.4.11
  SOURCE_SUBDIR  none
)
FetchContent_MakeAvailable(doctest)
add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest INTERFACE ${doctest_SOURCE_DIR})

FetchContent_Declare(
  unordered_dense
  GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
  GIT_TAG        231e48c9426bd21c273669e5fdcd042c146975cf # v4.4.0
)
FetchContent_MakeAvailable(unordered_dense)

# Simulation runtime: native support routines the JIT'd design calls into. No
# LLVM and no slang — plain native code, so it stays linkable on its own. The
# timing wheel is part of this surface: the runtime exposes it behind a C ABI.
add_library(clotho_runtime STATIC
  src/runtime/Runtime.cpp
  src/runtime/Svdpi.cpp
  src/Wheel.cpp
)
target_include_directories(clotho_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# unordered_dense is header-only (INTERFACE), so it adds only an include dir:
# the runtime uses ankerl's map for associative-array storage (LRM 7.8/7.9) and
# for sanitizer per-site bookkeeping. fmt is a real link edge, for diagnostic
# formatting (rt_bounds_report); it arrives with slang's FetchContent, so it is
# already in every clotho link.
target_link_libraries(clotho_runtime PRIVATE clotho_warnings unordered_dense::unordered_dense
                                             fmt::fmt)
if(WIN32)
  # $finish(2)'s resource line has no getrusage here; it reads peak working set
  # through GetProcessMemoryInfo (psapi).
  target_link_libraries(clotho_runtime PRIVATE psapi)

  # The runtime's wide arithmetic is `unsigned __int128`, whose division lowers
  # to compiler-rt's __udivti3 / __umodti3. The MSVC CRT has no such routines
  # and clang-cl does not link compiler-rt's builtins by default, so name the
  # archive that ships alongside the compiler. A MinGW-w64 clang links its own
  # builtins and needs none of this.
  if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    get_filename_component(clotho_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB clotho_crt_builtins
         "${clotho_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.builtins-*.lib")
    if(clotho_crt_builtins)
      list(GET clotho_crt_builtins 0 clotho_crt_builtin)
      message(STATUS "compiler-rt builtins: ${clotho_crt_builtin}")
      target_link_libraries(clotho_runtime PUBLIC "${clotho_crt_builtin}")
    else()
      message(WARNING "compiler-rt builtins not found under ${clotho_llvm_bin}/../lib/clang; "
                      "128-bit arithmetic will not link")
    endif()
  endif()
endif()
# Always optimize the runtime, even in a Debug build. It is stable native code
# the JIT'd design calls into on every event; at -O0 its trivial leaf functions
# (unwrap, WheelImpl accessors) stop inlining and the timing wheel reads as pure
# call overhead — distorting `perf` profiles and slowing every simulation. Debug
# is for iterating on the compiler, not the runtime, so pin this target to -O2
# there (asserts stay live — NDEBUG is not forced, so the runtime keeps its
# safety checks). Release / RelWithDebInfo already optimize; the genex leaves
# them untouched.
target_compile_options(clotho_runtime PRIVATE
  $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${MSVC}>>>:-O3>
  $<$<AND:$<CONFIG:Debug>,$<BOOL:${MSVC}>>:/O2>
)

# IR core: the frozen process IR, its builder, intern pool, printer and
# whole-design analyses. Free of LLVM, slang AND the runtime at source level —
# this is the layer a non-JIT consumer (a synthesis emitter, a language server)
# links against without dragging a code generator in. Keeping it a distinct
# target is load bearing, not cosmetic: it makes the build fail if an analysis
# ever reaches for LLVM.
add_library(clotho_ir_core STATIC
  src/ir/InternPool.cpp
  src/ir/Ir.cpp
  src/ir/Builder.cpp
  src/ir/Printer.cpp
  src/ir/Analysis.cpp
  src/ir/Eval.cpp
)
target_include_directories(clotho_ir_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clotho_ir_core PUBLIC unordered_dense::unordered_dense)
target_link_libraries(clotho_ir_core PRIVATE clotho_warnings)

# Simulation code generator: process IR -> LLVM IR -> ORC JIT. The only part of
# the IR layer that needs LLVM, so the only part that links it — and the only
# part that needs the runtime's C ABI layouts (rt_string, rt_queue, ...).
# Process legalization: what hardware each always-block implies. A peer of
# clotho_ir, not a layer inside clotho_ir_core -- the simulator never consumes it,
# so `clotho run` should not link it. Same rule as ir_core: no LLVM, no slang.
add_library(clotho_legalize STATIC
  src/legalize/Legalize.cpp
  src/legalize/Materialize.cpp
)
target_include_directories(clotho_legalize PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clotho_legalize PUBLIC clotho_ir_core)
target_link_libraries(clotho_legalize PRIVATE clotho_warnings)

add_library(clotho_ir STATIC
  src/ir/LlvmBackend.cpp
  src/ir/Jit.cpp
)
target_include_directories(clotho_ir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${LLVM_INCLUDE_DIRS})
target_compile_definitions(clotho_ir PUBLIC ${LLVM_DEFINITIONS})
target_link_libraries(clotho_ir PUBLIC clotho_ir_core clotho_runtime ${LLVM_LIBS})
target_link_libraries(clotho_ir PRIVATE clotho_warnings)

# Front-end: the slang -> IR translation, plus source-location plumbing and
# structured diagnostics. Needs slang and the IR core but NOT the code
# generator, so it links clotho_ir_core rather than clotho_ir. A consumer that
# only wants "source text -> ir::Module + diagnostics" (check mode, a language
# server) links this and never pulls LLVM in.
add_library(clotho STATIC
  src/Diagnostic.cpp
  src/SourceContext.cpp
  src/SourceMap.cpp
  src/Lower.cpp
)
target_include_directories(clotho PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clotho PUBLIC clotho_ir_core slang::slang)
target_link_libraries(clotho PRIVATE clotho_warnings)

# Thin build driver. Needs the code generator explicitly now that `clotho` no
# longer re-exports it.
add_executable(clotho_cli src/main.cpp)
set_target_properties(clotho_cli PROPERTIES OUTPUT_NAME clotho)
if(WIN32)
  # The `clotho` static library archives to clotho.lib, and this executable is
  # named clotho — so the import library the linker writes beside clotho.exe
  # takes the same path and clobbers the archive. The next link then resolves
  # nothing from it (clotho_test fails on exactly the frontend's symbols, while
  # clotho_ir's resolve). Give the archive its own name; nothing outside the
  # build refers to it.
  set_target_properties(clotho PROPERTIES ARCHIVE_OUTPUT_NAME clotho_frontend)
endif()
# `clotho` links clotho_ir_core, NOT the code generator, so the CLI names
# clotho_ir itself — it is the consumer that actually emits and JITs.
target_link_libraries(clotho_cli PRIVATE clotho clotho_ir clotho_warnings)

# `clotho --version`. The git hash is resolved at configure time; a source tree
# without git (a release tarball) reports "unknown" rather than failing.
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE CLOTHO_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT CLOTHO_GIT_HASH)
  set(CLOTHO_GIT_HASH "unknown")
endif()
target_compile_definitions(clotho_cli PRIVATE
  CLOTHO_GIT_HASH="${CLOTHO_GIT_HASH}"
  CLOTHO_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
)

# Single doctest executable aggregating every tests/*.cpp.
enable_testing()
add_executable(clotho_test
  tests/test_main.cpp
  tests/ir_test.cpp
  tests/operands_test.cpp
  tests/sourcemap_test.cpp
  tests/lower_test.cpp
  tests/case_test.cpp
  tests/function_test.cpp
  tests/task_test.cpp
  tests/intra_assign_test.cpp
  tests/continuous_assign_delay_test.cpp
  tests/string_test.cpp
  tests/dpi_test.cpp
  tests/display_test.cpp
  tests/strobe_test.cpp
  tests/loop_test.cpp
  tests/generate_test.cpp
  tests/instance_array_test.cpp
  tests/packed_range_test.cpp
  tests/z_elision_test.cpp
  tests/analysis_test.cpp
  tests/recovery_test.cpp
  tests/case_pragma_test.cpp
  tests/eval_test.cpp
  tests/legalize_test.cpp
  tests/gate_test.cpp
  tests/net_resolution_test.cpp
  tests/llvm_backend_test.cpp
  tests/two_state_test.cpp
  tests/real_test.cpp
  tests/wheel_test.cpp
  tests/runtime_test.cpp
  tests/compound_test.cpp
  tests/final_test.cpp
  tests/comb_fusion_test.cpp
  tests/enum_test.cpp
  tests/sysfunc_test.cpp
  tests/streaming_test.cpp
  tests/uwire_test.cpp
  tests/struct_test.cpp
  tests/assign_pattern_test.cpp
  tests/plusargs_test.cpp
  tests/vcd_test.cpp
  tests/package_test.cpp
  tests/inside_test.cpp
  tests/wait_test.cpp
  tests/xhier_test.cpp
  tests/fork_test.cpp
  tests/severity_test.cpp
  tests/event_test.cpp
  tests/dynamic_array_test.cpp
  tests/memory_bounds_test.cpp
  tests/queue_test.cpp
  tests/associative_array_test.cpp
  tests/array_ordering_test.cpp
  tests/class_test.cpp
  tests/unbased_test.cpp
  tests/preprocess_test.cpp
  tests/interface_test.cpp
  tests/concat_select_test.cpp
  tests/force_release_test.cpp
  tests/system_test.cpp
)
target_link_libraries(clotho_test PRIVATE clotho clotho_ir clotho_legalize doctest::doctest clotho_warnings)
# The reset corpus is 28 readable .sv files rather than embedded strings: they are
# a reference to be read and extended, and they are the same files run against
# yosys.
target_compile_definitions(clotho_test PRIVATE CLOTHO_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")

# Export the test executable's dynamic symbols so DPI-C imports defined in the
# test TUs (extern "C") resolve through the JIT's process-symbol generator
# (dlsym(RTLD_DEFAULT)), the same path libc/libm DPI imports already use.
set_target_properties(clotho_test PROPERTIES ENABLE_EXPORTS ON)
if(WIN32 AND NOT MSVC)
  # ENABLE_EXPORTS alone exports nothing from a PE executable -- there is no
  # equivalent of -rdynamic, and the linker only exports what is explicitly
  # marked. The MinGW linker will export the lot when asked, which is what the
  # JIT's process-symbol lookup needs to find the DPI-C imports the test TUs
  # define.
  target_link_options(clotho_test PRIVATE -Wl,--export-all-symbols)
endif()

# Register each doctest case as its own CTest test for parallelism and
# per-case reporting.
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(clotho_test)
