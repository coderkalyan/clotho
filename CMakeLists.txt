cmake_minimum_required(VERSION 3.20)
project(clotho VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake's own default is an EMPTY build type: no -O, no -g, no NDEBUG. That is
# slower than Debug and less debuggable than RelWithDebInfo, and it is silently not
# what anyone meant, so pick one and say which.
#
# Debug, because a bare `cmake` is someone about to work on clotho, and Debug is the
# build for that: every assert() live, frames that have not been inlined away, and
# unfinished work (the `synth` command) reachable without extra flags.
#
# The full matrix, since the three types mean different things per target:
#
#   toplevel          frontend                  clotho_runtime
#   Debug             Debug      (-O0 -g)       -O2 -g  (optimized, asserts ON)
#   RelWithDebInfo    RelWithDebInfo            RelWithDebInfo
#   Release           Release, stripped         Release, stripped
#
# Debug is for iterating on the compiler, so only the frontend is unoptimized; the
# runtime is not part of that loop and -O0 there would distort every profile. Every
# build type keeps assert() live (below); Debug also turns on
# CLOTHO_EXTRA_ASSERTS.
#
# The cost of that default, measured on hazard3 (21 files, 1418 processes):
#
#   full test suite         17.7 s -> 19.7 s   (1.1x)
#   clotho lint             0.146 s -> 0.521 s (3.6x)
#   clotho build --emit=ir  0.107 s -> 0.425 s (4.0x)
#
# The suite barely moves because it is LLVM-bound, and LLVM is a prebuilt shared
# library at its own optimization level -- how clotho is compiled does not touch it.
# The frontend alone is 3.6-4x slower, which is the number to remember when working
# on `lint` latency or the language server: pass -DCMAKE_BUILD_TYPE=RelWithDebInfo
# for that, and for any performance measurement. CI and tests/perf/perf.py already
# pin Release explicitly, so neither is affected by this default.
#
# Not covered by any of the above, and easy to conflate with it: the JIT-compiled
# DESIGN's optimization level is a run-time decision (`-O` on the command line), and
# LLVM is prebuilt and linked at whatever level it was packaged with.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No CMAKE_BUILD_TYPE set; defaulting to Debug")
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Every build type keeps assert() live, shipped builds included: a violated
# invariant stops the program and reports where (docs/PRINCIPLES.md), and an
# assertion compiled out of the binary users run cannot. Measured against
# -DNDEBUG (clang 22, -O3): every tests/perf case and a UVM compile moved by less
# than run-to-run noise; a microbenchmark that is mostly garbage collection runs
# 1.35x the instructions, from the collector's per-object field assertions. Checks
# too slow to ship -- heap lookups and whole-structure verifiers such as the GC's
# heap verify, 2.3x on that microbenchmark -- are behind CLOTHO_EXTRA_ASSERTS
# instead. CMake's defaults put NDEBUG in the flags of every non-Debug type, so it
# is removed there; a normal variable shadows the cache entry for this project and
# every dependency it fetches.
foreach(lang C CXX)
  foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
    string(REGEX REPLACE "[-/]DNDEBUG" "" CMAKE_${lang}_FLAGS_${config} "${CMAKE_${lang}_FLAGS_${config}}")
  endforeach()
endforeach()

# Checks too slow for a shipped build. On by default in Debug; CI also runs the
# suite with them on.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(clotho_extra_asserts_default ON)
else()
  set(clotho_extra_asserts_default OFF)
endif()
option(CLOTHO_EXTRA_ASSERTS "Build in checks too slow for a shipped build (GC heap verify)"
       ${clotho_extra_asserts_default})
if(CLOTHO_EXTRA_ASSERTS)
  add_compile_definitions(CLOTHO_EXTRA_ASSERTS)
endif()

# Release ships stripped: CMake's Release already omits -g, and this drops the
# symbol table too. Profiling or debugging clotho itself is what RelWithDebInfo
# is for.
#
# This is the host binary only. The JIT'd design's optimization level is a run-time
# decision (`-O` on the command line), and LLVM is a prebuilt library at whatever
# level it was packaged with; neither is affected by any of this.
if(NOT MSVC)
  add_link_options($<$<CONFIG:Release>:-s>)
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

# Work in progress, Debug-only. An unfinished command or check is offered only by
# a Debug build; Debug is the default, so it is reachable by default, and neither
# shipped configuration exposes something that is not ready.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CLOTHO_WIP_DEFAULT ON)
else()
  set(CLOTHO_WIP_DEFAULT OFF)
endif()

# `synth`: only the CLI wiring is conditional. clotho_legalize is always built and
# always linked into clotho_test, so the analysis and the writer are covered by
# the full suite in every configuration -- what the others skip is argument
# plumbing.
option(CLOTHO_SYNTH "Expose the unfinished `synth` command in the CLI" ${CLOTHO_WIP_DEFAULT})

# `lsp` and `dap`: unfinished servers, offered the same way as `synth`, except
# that the whole server is conditional, not just its CLI wiring. Off, the server
# libraries and the JSON library behind them (glaze, and the notices it carries)
# are neither built nor linked, and their tests are not part of the suite.
option(CLOTHO_LSP "Build and expose the unfinished `lsp` command (language server)" ${CLOTHO_WIP_DEFAULT})
option(CLOTHO_DAP "Build and expose the unfinished `dap` command (debug adapter)" ${CLOTHO_WIP_DEFAULT})

# `--sanitize=xprov`: the flag is parsed and carried in SimulationConfig, but no
# backend pass reads the bit yet, so it instruments nothing. An operator who
# reaches for a sanitizer and gets silence is worse served than one told the check
# does not exist. As with `synth`, the `Sanitizer::XProv` bit and everything
# behind it stay unconditional; what the shipped configurations drop is the CLI
# wiring.
option(CLOTHO_XSAN "Expose the unfinished `--sanitize=xprov` check in the CLI" ${CLOTHO_WIP_DEFAULT})
if(CLOTHO_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "CLOTHO_SANITIZE requires a GCC/Clang driver")
  endif()
  # UBSan only prints and continues by default, which leaves the exit status of a
  # test that hit undefined behavior unchanged; make every finding stop the run.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
endif()

include(FetchContent)

# System LLVM. Used by the backend for native codegen. This install ships LLVM
# as a single shared library, so prefer the `LLVM` imported target; fall back
# to per-component static libs if only those are present.
# NO_SYSTEM_ENVIRONMENT_PATH: do not infer LLVM's location from PATH.
#
# CMake otherwise searches the parent of every PATH entry, so a machine with ROCm
# installed (/opt/rocm/bin on PATH) finds /opt/rocm/llvm before the system LLVM.
# That copy is built without RTTI and cannot link clotho_ir. Which LLVM to use is
# not something PATH should decide -- pass -DLLVM_DIR=... to choose deliberately.
find_package(LLVM REQUIRED CONFIG NO_SYSTEM_ENVIRONMENT_PATH)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
if(NOT LLVM_VERSION_MAJOR EQUAL 22)
  message(FATAL_ERROR "Clotho requires LLVM 22, found ${LLVM_PACKAGE_VERSION}")
endif()
if(TARGET LLVM)
  set(LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(LLVM_LIBS core support orcjit jitlink native bitwriter bitreader
                                  linker)
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
  # Every patches/slang-*.patch is applied, in name order:
  #
  # slang-class-forward-ref: LRM 8.6 (class methods), 18.5 (constraint blocks),
  # and 19.4/19.5 (a covergroup's coverpoint/cross/iff/event expressions
  # embedded in a class) all give a class's own members unordered visibility to
  # those constructs -- unlike SV's general declare-before-use default (LRM
  # 3.13/6.21) -- but slang's scope-chain walk-up (Lookup.cpp's
  # unqualifiedImpl) unconditionally anchored the search to its own declaration
  # position, so a method, constraint block, or coverpoint that textually
  # preceded the class member it read was misdiagnosed as "used before its
  # declaration". See docs/design/classes.md.
  #
  # slang-input-port-net-kind: LRM 23.2.2.3 says an input or inout port that
  # omits the port kind "shall default to a net of default net type", and its own
  # mh1/mh2/mh15 examples spell that out for an explicit data type (`inout
  # integer x` is `inout wire integer x`). slang applied the OUTPUT rule to
  # inputs -- a data type means a variable -- deliberately, because LRM 6.7.1
  # admits only 4-state types for a net and `input int i` would otherwise be
  # rejected as a net of an illegal type. The patch keeps that escape hatch
  # and follows the LRM for the rest, in both the ANSI and non-ANSI builders: a
  # net only where the syntax alone proves one is legal, so `logic`/`reg`/
  # `integer`/`time` with no unpacked dimension and a usable `default_nettype.
  # Without it `input logic [7:0] d` read x undriven instead of z, and a non-ANSI
  # `inout logic [7:0] d;` was rejected outright as a variable port with an inout
  # direction. Carries its own slang unit test. See
  # docs/design/program-blocks.md §11.
  #
  # slang-randseq-rule-pairing: two randsequence elaboration defects, both of
  # which silently hand the frontend the wrong AST rather than diagnosing.
  # createCaseProd pushed one ProdItem per case LABEL but consumed one per case
  # ITEM, so every rs_case item after a multi-label one picked up the previous
  # item's production; and the rule block symbol created the weight clause's
  # code block once per prod, which bound its statements N times and
  # desynchronized createRule's positional pairing for a rule with two or more
  # code-block prods. See docs/design/randsequence.md §13.
  #
  # slang-enum-cast-context: LRM 6.19.4 says "Casting to an enum type shall cause
  # a conversion of the expression to its base type", and 6.24.1 that a static
  # cast returns "the value that a variable of the casting type would hold after
  # being assigned the expression". So the operand is sized by the cast's base
  # type, as in any assignment. slang only pushed the target's width into the
  # operand when the operand was assignment compatible with the target, which an
  # enum never is, so `e_t'('hx)` (40-bit base) was `00xxxxxxxx`, `e_t'('1)` was 1
  # and `e_t'(x + 2'd1)` lost its carry. This is upstream slang commit 15f633ba6
  # verbatim; it must apply before slang-unsized-xz-extend, which extends the
  # same operand, and it sorts first. Drop it on the first bump past that commit.
  #
  # slang-unsized-xz-extend: LRM 5.7.1 says an unsized unsigned literal whose
  # high-order bit is x or z "shall be extended to the size of the expression
  # containing the literal constant", with x or z. slang only zero-extended, so
  # `reg [63:0] r = 'bx` held 32 x bits under 32 zeros, in the AST Clotho lowers
  # and in every value slang folds (parameters, enum members, constant
  # functions, case labels). This is upstream slang commit 0f89ca18 verbatim
  # (IntegerLiteral::propagateType); drop it on the first bump past that commit.
  PATCH_COMMAND  ${CMAKE_COMMAND} "-DPATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/patches"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplySlangPatch.cmake"
)
FetchContent_MakeAvailable(slang)
# slang builds with hidden visibility; clotho does not, and both instantiate
# type_info for slang types (clotho inspects slang's diagnostic arguments). On
# Apple arm64 the two visibilities compare type_info differently, so the
# std::any_cast in slang's own diagnostic formatter threw bad_any_cast there.
# Clotho links slang statically, so default visibility exports nothing extra.
set_target_properties(slang_slang PROPERTIES CXX_VISIBILITY_PRESET default VISIBILITY_INLINES_HIDDEN OFF)

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

# JSON, for the language server's JSON-RPC. Header-only, C++23 compile-time
# reflection: an LSP structure is an ordinary aggregate and the wire format
# comes from its member names, so there is no schema to keep in sync by hand --
# which is the failure mode a hand-rolled or macro-driven parser has in a
# protocol this wide. Chosen over nlohmann for speed (a semantic-tokens response
# is a very large array) and over simdjson because its on-demand parser is slow
# exactly when object keys arrive out of order, which is normal for LSP clients.
if(CLOTHO_LSP OR CLOTHO_DAP)
  FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        be4481f4b106fc82f0b7bc85f6a92202f8c0dd59 # v8.0.0
  )
  FetchContent_MakeAvailable(glaze)
endif()

# libsva: the concurrent-assertion temporal engine — a Zig static library plus a
# C header (https://github.com/coderkalyan/libsva). It builds the SVA IR, lowers
# it to automata, and hands back tables; see docs/design/concurrent-assertions.md.
#
# This is the first dependency clotho builds with an external tool rather than
# fetching sources for CMake to compile, so it does not follow the FetchContent
# pattern above. It is also a FRONT-END-ONLY dependency: the IR is built,
# lowered, and discarded during lowering, and no libsva code runs in a simulated
# process, so only the `clotho` target links it.
option(CLOTHO_SVA "Concurrent-assertion support (requires Zig and libsva)" ON)
set(CLOTHO_LIBSVA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libsva"
    CACHE PATH "libsva source tree, built in place with `zig build`")

if(CLOTHO_SVA)
  find_program(ZIG_EXECUTABLE zig REQUIRED DOC "Zig 0.16+, used to build libsva")
  if(NOT EXISTS "${CLOTHO_LIBSVA_DIR}/build.zig")
    message(FATAL_ERROR
      "CLOTHO_SVA is ON but no libsva at ${CLOTHO_LIBSVA_DIR}.\n"
      "Point -DCLOTHO_LIBSVA_DIR=<path> at a checkout, or configure with "
      "-DCLOTHO_SVA=OFF to build without concurrent assertions.")
  endif()

  # Keep libsva's assertions live in every clotho build type. They enforce the IR
  # contract (operand layers, range well-formedness, dense atoms), and a violation
  # is a clotho bug that must abort rather than lower to a wrong monitor — so
  # ReleaseFast is deliberately never selected here.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_libsva_optimize Debug)
  else()
    set(_libsva_optimize ReleaseSafe)
  endif()

  set(_libsva_prefix ${CMAKE_CURRENT_BINARY_DIR}/libsva)
  # Zig names a static library sva.lib on Windows, whatever the ABI.
  if(WIN32)
    set(_libsva_lib ${_libsva_prefix}/lib/sva.lib)
  else()
    set(_libsva_lib ${_libsva_prefix}/lib/libsva.a)
  endif()
  # Clotho includes the header as <libsva/sva.h>. A bare <sva.h> resolves to
  # src/Sva.h on a case-insensitive filesystem (macOS, Windows), because src/
  # precedes this directory on the include path.
  set(_libsva_include ${_libsva_prefix}/inc)
  file(GLOB _libsva_sources "${CLOTHO_LIBSVA_DIR}/src/*.zig")

  # -Dcpu=baseline: Zig otherwise targets the build machine's CPU, and the
  # compiler_rt it bundles into libsva supplies clotho's memcpy and memset, so a
  # binary built on a machine with newer vector extensions died with SIGILL at
  # startup on an older one.
  add_custom_command(
    OUTPUT ${_libsva_lib} ${_libsva_include}/libsva/sva.h
    COMMAND ${ZIG_EXECUTABLE} build --prefix ${_libsva_prefix}
            -Doptimize=${_libsva_optimize} -Dcpu=baseline
    COMMAND ${CMAKE_COMMAND} -E copy ${_libsva_prefix}/include/sva.h
            ${_libsva_include}/libsva/sva.h
    WORKING_DIRECTORY ${CLOTHO_LIBSVA_DIR}
    DEPENDS ${_libsva_sources} ${CLOTHO_LIBSVA_DIR}/build.zig
            ${CLOTHO_LIBSVA_DIR}/include/sva.h
    COMMENT "Building libsva (${_libsva_optimize}) with zig"
    VERBATIM
  )
  add_custom_target(libsva_build DEPENDS ${_libsva_lib})

  # The header is generated, so its directory must exist at configure time for
  # INTERFACE_INCLUDE_DIRECTORIES to validate.
  file(MAKE_DIRECTORY ${_libsva_include}/libsva)
  add_library(libsva INTERFACE)
  target_link_libraries(libsva INTERFACE ${_libsva_lib})
  if(WIN32)
    # Zig's standard library calls the native API directly (NtCreateFile,
    # RtlQueryPerformanceCounter, ...), which only ntdll exports.
    target_link_libraries(libsva INTERFACE ntdll)
  endif()
  target_include_directories(libsva SYSTEM INTERFACE ${_libsva_include})
endif()

# Simulation runtime: native support routines the JIT'd design calls into. No
# LLVM and no slang — plain native code, so it stays linkable on its own. The
# timing wheel is part of this surface: the runtime exposes it behind a C ABI.
add_library(clotho_runtime STATIC
  src/runtime/Containers.cpp
  src/runtime/Format.cpp
  src/runtime/Io.cpp
  src/runtime/Random.cpp
  src/runtime/Reports.cpp
  src/runtime/Scheduler.cpp
  src/runtime/Strings.cpp
  src/runtime/Values.cpp
  src/runtime/Vcd.cpp
  src/runtime/Gc.cpp
  src/runtime/Svdpi.cpp
  src/runtime/Coverage.cpp
  src/runtime/Wheel.cpp
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
# A Debug build builds the runtime as RelWithDebInfo, not as Debug.
#
# Debug exists for fast iteration on the COMPILER. The runtime is not part of that
# loop: it is stable native code the JIT'd design calls into on every event, and at
# -O0 its trivial leaf functions (unwrap, WheelImpl accessors) stop inlining, so the
# timing wheel reads as pure call overhead -- every simulation slows and every
# `perf` profile lies about where the time went.
#
# So Debug gets -O2 -g here: optimized like RelWithDebInfo and still walkable in a
# debugger, with every check still live. Release and RelWithDebInfo already supply
# the right flags, so the genex leaves them alone.
target_compile_options(clotho_runtime PRIVATE
  $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${MSVC}>>>:-O2>
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
  src/ir/Typing.cpp
  src/ir/Verify.cpp
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
  src/ir/llvm/Aggregates.cpp
  src/ir/llvm/Analyses.cpp
  src/ir/llvm/AssocArrays.cpp
  src/ir/llvm/Backend.cpp
  src/ir/llvm/Containers.cpp
  src/ir/llvm/Dedup.cpp
  src/ir/llvm/Dpi.cpp
  src/ir/llvm/Events.cpp
  src/ir/llvm/GcEmit.cpp
  src/ir/llvm/Inst.cpp
  src/ir/llvm/LlvmBackend.cpp
  src/ir/llvm/Ops.cpp
  src/ir/llvm/Processes.cpp
  src/ir/llvm/RuntimeAbi.cpp
  src/ir/llvm/Sanitizers.cpp
  src/ir/llvm/Scheduler.cpp
  src/ir/llvm/StateLayout.cpp
  src/ir/llvm/Stores.cpp
  src/ir/llvm/Subroutines.cpp
  src/ir/llvm/SystemTasks.cpp
  src/ir/llvm/Vcd.cpp
  src/ir/Jit.cpp
)
target_include_directories(clotho_ir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${LLVM_INCLUDE_DIRS})
# Every src/ir/llvm/ file opens with the internal Backend.h, which pulls in most of
# LLVM; precompiling it once spares each file parsing LLVM again.
target_precompile_headers(clotho_ir PRIVATE src/ir/llvm/Backend.h)
set_source_files_properties(src/ir/Jit.cpp TARGET_DIRECTORY clotho_ir PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# A machine with ROCm installed has a second LLVM under /opt/rocm, and
# find_package picks it over the system one. That copy is built without RTTI, so
# the backend emits references to `typeinfo for llvm::CallbackVH` that nothing
# provides -- and the only symptom is an inscrutable link failure two minutes into
# the build, long after configure said everything was fine.
#
# Fail here instead, naming the fix. Clotho needs RTTI: the backend derives from
# LLVM types whose typeinfo has to come from the LLVM libraries.
if(DEFINED LLVM_ENABLE_RTTI AND NOT LLVM_ENABLE_RTTI)
  message(FATAL_ERROR
    "the LLVM found at ${LLVM_DIR} was built without RTTI, which cannot link clotho_ir.\n"
    "  Point at an RTTI-enabled LLVM, e.g.:\n"
    "    cmake -S . -B build -DLLVM_DIR=/usr/lib/cmake/llvm")
endif()

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
  src/lower/Assertions.cpp
  src/lower/Assign.cpp
  src/lower/BuiltinClasses.cpp
  src/lower/Classes.cpp
  src/lower/Containers.cpp
  src/lower/Coverage.cpp
  src/lower/Expr.cpp
  src/lower/Hierarchy.cpp
  src/lower/Lower.cpp
  src/lower/Lowerer.cpp
  src/lower/Nets.cpp
  src/lower/Random.cpp
  src/lower/Scans.cpp
  src/lower/Sort.cpp
  src/lower/Statements.cpp
  src/lower/Subroutines.cpp
  src/lower/SvaMonitor.cpp
  src/lower/SystemTasks.cpp
  src/lower/Timing.cpp
  src/lower/Types.cpp
  src/Sva.cpp
)
target_include_directories(clotho PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Every src/lower/ file opens with the internal Lowerer.h, which pulls in most of
# slang's AST; precompiling it once spares each file parsing slang again.
target_precompile_headers(clotho PRIVATE src/lower/Lowerer.h)
set_source_files_properties(src/Diagnostic.cpp src/SourceContext.cpp src/SourceMap.cpp src/Sva.cpp
  TARGET_DIRECTORY clotho PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# ccache caches a file compiled against a Clang PCH only when the PCH carries no
# timestamp (https://ccache.dev/manual/4.7.4.html#_precompiled_headers); CI sets
# the matching CCACHE_SLOPPINESS.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(clotho_ir PRIVATE "SHELL:-Xclang -fno-pch-timestamp")
  target_compile_options(clotho PRIVATE "SHELL:-Xclang -fno-pch-timestamp")
endif()
target_link_libraries(clotho PUBLIC clotho_ir_core slang::slang)
target_link_libraries(clotho PRIVATE clotho_warnings)

# Concurrent assertions: the front-end builds an SVA IR through libsva's C API,
# lowers it to a monitor, and reads the tables back into ir::AssertionMonitor. The
# dependency stops here — clotho_ir consumes only that POD, and nothing links
# libsva into the runtime or the JIT.
if(CLOTHO_SVA)
  add_dependencies(clotho libsva_build)
  target_link_libraries(clotho PRIVATE libsva)
  target_compile_definitions(clotho PRIVATE CLOTHO_HAVE_SVA=1)
endif()

if(CLOTHO_LSP OR CLOTHO_DAP)
  # The base-protocol framing shared by the language server and the debug
  # adapter: `Content-Length` over raw descriptors, and nothing else. Its own
  # target rather than a file inside one of them, because both consume it and
  # neither should link through the other -- clotho_lsp must stay LLVM-free
  # (below) and clotho_dap cannot be.
  add_library(clotho_transport STATIC src/transport/Transport.cpp)
  target_include_directories(clotho_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(clotho_transport PRIVATE clotho_warnings)
  find_package(Threads REQUIRED)
endif()

if(CLOTHO_LSP)
  # Language server: JSON-RPC over stdio, the transport threads and the request
  # dispatcher. Links the frontend and nothing below it, so like clotho_ir_core
  # this target's job is partly to make the build fail if the server ever reaches
  # for LLVM: the process an editor keeps open must stay cheap to start, and the
  # worker split in docs/design/lsp.md 4.3 depends on the router being able to
  # outlive a simulation that crashed.
  #
  # glaze is PRIVATE: no LSP header names a JSON type, so nothing that links this
  # pays for its templates.
  add_library(clotho_lsp STATIC
    src/lsp/LineIndex.cpp
    src/lsp/Tokens.cpp
    src/lsp/Server.cpp
  )
  target_include_directories(clotho_lsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(clotho_lsp PUBLIC clotho clotho_transport)
  target_link_libraries(clotho_lsp PRIVATE clotho_warnings glaze::glaze)
  target_link_libraries(clotho_lsp PUBLIC Threads::Threads)
endif()

if(CLOTHO_DAP)
  # Debug adapter: DAP over stdio, and the design's own process (docs/design/dap.md).
  #
  # Separate from clotho_lsp, and it has to be. A debugger executes the design,
  # so this target reaches the JIT and therefore LLVM -- which is exactly what
  # the invariant above exists to forbid for the server an editor keeps open. It
  # links clotho_ir today only for that dependency's sake; the adapter itself
  # runs the design out of process, so the code here calls nothing in it yet
  # (docs/design/dap.md 4.4). glaze stays PRIVATE for the same reason it is
  # there: no DAP header names a JSON type.
  add_library(clotho_dap STATIC
    src/dap/Server.cpp
    src/dap/Design.cpp
  )
  target_include_directories(clotho_dap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(clotho_dap PUBLIC clotho clotho_transport Threads::Threads)
  target_link_libraries(clotho_dap PRIVATE clotho_warnings glaze::glaze)
endif()

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
if(CLOTHO_SYNTH)
  target_link_libraries(clotho_cli PRIVATE clotho_legalize)
  target_compile_definitions(clotho_cli PRIVATE CLOTHO_SYNTH)
endif()
# Without a command there is nothing to link: the shipped simulator contains no
# legalization code and no language server or debug adapter.
if(CLOTHO_LSP)
  target_link_libraries(clotho_cli PRIVATE clotho_lsp)
  target_compile_definitions(clotho_cli PRIVATE CLOTHO_LSP)
endif()
if(CLOTHO_DAP)
  target_link_libraries(clotho_cli PRIVATE clotho_dap)
  target_compile_definitions(clotho_cli PRIVATE CLOTHO_DAP)
endif()
if(CLOTHO_XSAN)
  target_compile_definitions(clotho_cli PRIVATE CLOTHO_XSAN)
endif()

# `clotho --version`. The release is the project version plus a pre-release
# suffix, which a final release leaves empty. The git hash is resolved at
# configure time; a source tree without git (a release tarball) reports
# "unknown" rather than failing.
set(CLOTHO_VERSION_SUFFIX "alpha")
set(CLOTHO_VERSION "${PROJECT_VERSION}")
if(CLOTHO_VERSION_SUFFIX)
  string(APPEND CLOTHO_VERSION "-${CLOTHO_VERSION_SUFFIX}")
endif()
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
  CLOTHO_VERSION="${CLOTHO_VERSION}"
  CLOTHO_GIT_HASH="${CLOTHO_GIT_HASH}"
  CLOTHO_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
)

# The runtime an executable links (docs/design/executables.md): the same sources
# as clotho_runtime plus the executable's main, compiled against libc++'s headers
# so that nothing is left for a C++ library to supply -- no exceptions, no RTTI,
# no extern templates, no hardening checks, {fmt} header-only without locales,
# and operator new/delete from ExeSupport.cpp. An executable links this, the C
# library and compiler-rt, and no C++ library.
find_path(CLOTHO_LIBCXX_INCLUDE_DIR __config
  PATHS ${LLVM_INCLUDE_DIRS} "${LLVM_LIBRARY_DIRS}/../include" /usr/include
  PATH_SUFFIXES c++/v1
  NO_DEFAULT_PATH)
if(NOT CLOTHO_LIBCXX_INCLUDE_DIR)
  message(FATAL_ERROR "libc++'s headers (c++/v1/__config) were not found beside LLVM or in "
                      "/usr/include; install them (Debian/Ubuntu: libc++-${LLVM_VERSION_MAJOR}-dev), or "
                      "pass -DCLOTHO_LIBCXX_INCLUDE_DIR=<dir>/c++/v1")
endif()
# __config_site is per target, beside __config or in a multiarch <triple>/c++/v1.
file(GLOB clotho_libcxx_config_sites "${CLOTHO_LIBCXX_INCLUDE_DIR}/__config_site"
     "${CLOTHO_LIBCXX_INCLUDE_DIR}/../../*/c++/v1/__config_site")
if(NOT clotho_libcxx_config_sites)
  message(FATAL_ERROR "libc++'s __config_site was not found near ${CLOTHO_LIBCXX_INCLUDE_DIR}")
endif()
list(GET clotho_libcxx_config_sites 0 clotho_libcxx_config_site)
get_filename_component(clotho_libcxx_config_dir "${clotho_libcxx_config_site}" DIRECTORY)
message(STATUS "libc++ headers for executables: ${CLOTHO_LIBCXX_INCLUDE_DIR} (${clotho_libcxx_config_dir})")

# The text `<executable> --licenses` prints, as a byte array. A Windows
# executable links the MinGW-w64 runtime statically, whose ZPL-2.1 parts require
# its notices in a binary; its notice file says so itself.
set(clotho_exe_notices
  src/runtime/exe-notices.txt src/runtime/LICENSE LICENSES/unordered_dense.txt LICENSES/fmt.txt)
if(WIN32)
  list(APPEND clotho_exe_notices LICENSES/MinGW-w64-runtime.txt)
endif()
set(clotho_licenses_text "")
foreach(notice IN LISTS clotho_exe_notices)
  file(READ "${notice}" text)
  string(APPEND clotho_licenses_text "${text}\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${notice}")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/exe-licenses.txt" "${clotho_licenses_text}")
file(READ "${CMAKE_CURRENT_BINARY_DIR}/exe-licenses.txt" clotho_licenses_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," clotho_licenses_hex "${clotho_licenses_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ExeLicenses.inc"
     "static const unsigned char kLicenses[] = {${clotho_licenses_hex}};\n")

add_library(clotho_runtime_exe STATIC
  src/runtime/Containers.cpp
  src/runtime/Format.cpp
  src/runtime/Io.cpp
  src/runtime/Random.cpp
  src/runtime/Reports.cpp
  src/runtime/Scheduler.cpp
  src/runtime/Strings.cpp
  src/runtime/Values.cpp
  src/runtime/Vcd.cpp
  src/runtime/Gc.cpp
  src/runtime/Svdpi.cpp
  src/runtime/Coverage.cpp
  src/runtime/Wheel.cpp
  src/runtime/ExeMain.cpp
  src/runtime/ExeSupport.cpp
)
target_include_directories(clotho_runtime_exe SYSTEM PRIVATE
  $<TARGET_PROPERTY:fmt::fmt,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:unordered_dense::unordered_dense,INTERFACE_INCLUDE_DIRECTORIES>)
target_include_directories(clotho_runtime_exe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# libc++'s directories as flags, not include directories: CMake drops an include
# directory that is one of the compiler's own, which libc++'s is for a clang that
# uses it by default, and -nostdinc++ has removed it from the search.
target_compile_options(clotho_runtime_exe PRIVATE -nostdinc++ "SHELL:-isystem \"${clotho_libcxx_config_dir}\""
                                                  "SHELL:-isystem \"${CLOTHO_LIBCXX_INCLUDE_DIR}\""
                                                  -fno-exceptions -fno-rtti -include
                                                  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/ExeConfig.h")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC flags a size libc++'s allocator computes but never passes on.
  target_compile_options(clotho_runtime_exe PRIVATE -Wno-alloc-size-larger-than)
endif()
target_compile_definitions(clotho_runtime_exe PRIVATE
  CLOTHO_VERSION="${CLOTHO_VERSION}"
  CLOTHO_GIT_HASH="${CLOTHO_GIT_HASH}"
  CLOTHO_LICENSES_INC="${CMAKE_CURRENT_BINARY_DIR}/ExeLicenses.inc")
target_link_libraries(clotho_runtime_exe PRIVATE clotho_warnings)

# What `clotho build` links an executable with (docs/design/executables.md): lld,
# run in-process, and the files every executable needs, embedded in clotho so
# that building one needs no compiler, linker or C library development files.
# The files are this build's own: its executable runtime, compiler-rt's
# builtins, and the C library's startup objects and static parts, found where
# the C compiler driver would find them.
find_package(LLD REQUIRED CONFIG HINTS "${LLVM_DIR}/../lld" "${LLVM_LIBRARY_DIRS}/cmake/lld")
find_program(CLOTHO_CLANG NAMES clang-${LLVM_VERSION_MAJOR} clang HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH)
if(NOT CLOTHO_CLANG)
  message(FATAL_ERROR "clang was not found in ${LLVM_TOOLS_BINARY_DIR}; it locates compiler-rt's builtins")
endif()
execute_process(COMMAND "${CLOTHO_CLANG}" --rtlib=compiler-rt -print-libgcc-file-name
                OUTPUT_VARIABLE clotho_builtins OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT EXISTS "${clotho_builtins}")
  message(FATAL_ERROR "compiler-rt's builtins were not found at '${clotho_builtins}'; install them "
                      "(Debian/Ubuntu: libclang-rt-${LLVM_VERSION_MAJOR}-dev)")
endif()

# `name=path` for one embedded file, found where the C compiler driver finds it.
function(clotho_kit_file out file)
  execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-file-name=${file}
                  OUTPUT_VARIABLE path OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT IS_ABSOLUTE "${path}" OR NOT EXISTS "${path}")
    message(FATAL_ERROR "${file}, which every executable links, was not found by ${CMAKE_C_COMPILER}")
  endif()
  set(${out} "${file}=${path}" PARENT_SCOPE)
endfunction()

set(clotho_kit "runtime.a=$<TARGET_FILE:clotho_runtime_exe>" "builtins.a=${clotho_builtins}")
if(WIN32)
  foreach(file crt2.o crtbegin.o crtend.o libmingw32.a libmingwex.a libmoldname.a libmsvcrt.a libadvapi32.a
               libshell32.a libuser32.a libkernel32.a libpsapi.a)
    clotho_kit_file(entry ${file})
    list(APPEND clotho_kit "${entry}")
  endforeach()
elseif(NOT APPLE)
  # compiler-rt's crtbegin/crtend sit beside its builtins, with the same suffix.
  get_filename_component(clotho_builtins_dir "${clotho_builtins}" DIRECTORY)
  get_filename_component(clotho_builtins_name "${clotho_builtins}" NAME)
  string(REGEX REPLACE "^libclang_rt\\.builtins(.*)\\.a$" "\\1" clotho_rt_suffix "${clotho_builtins_name}")
  foreach(part crtbegin crtend)
    set(path "${clotho_builtins_dir}/clang_rt.${part}${clotho_rt_suffix}.o")
    if(NOT EXISTS "${path}")
      message(FATAL_ERROR "compiler-rt's ${part} was not found at ${path}")
    endif()
    list(APPEND clotho_kit "${part}.o=${path}")
  endforeach()
  foreach(file Scrt1.o crti.o crtn.o libc_nonshared.a)
    clotho_kit_file(entry ${file})
    list(APPEND clotho_kit "${entry}")
  endforeach()
endif()

set(clotho_kit_source "#include \"link/Link.h\"\n\nnamespace clotho::link {\nnamespace {\n")
set(clotho_kit_table "")
set(clotho_kit_paths "")
set(index 0)
foreach(entry IN LISTS clotho_kit)
  string(FIND "${entry}" "=" eq)
  string(SUBSTRING "${entry}" 0 ${eq} name)
  math(EXPR start "${eq} + 1")
  string(SUBSTRING "${entry}" ${start} -1 path)
  string(APPEND clotho_kit_source "const unsigned char kFile${index}[] = {\n#embed \"${path}\"\n};\n")
  string(APPEND clotho_kit_table "    { \"${name}\", kFile${index}, sizeof(kFile${index}) },\n")
  if(NOT name STREQUAL "runtime.a")
    list(APPEND clotho_kit_paths "${path}")
  endif()
  math(EXPR index "${index} + 1")
endforeach()
string(APPEND clotho_kit_source "} // namespace\n\nconst KitFile kKitFiles[] = {\n${clotho_kit_table}};\n"
       "const size_t kKitFileCount = ${index};\n\n} // namespace clotho::link\n")
# Copied from the generated template on every change to an embedded file, so the
# table recompiles with it: #embed is not a dependency the build tool sees.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/LinkKit.cpp.in" CONTENT "${clotho_kit_source}")
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/LinkKit.cpp"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/LinkKit.cpp.in"
          "${CMAKE_CURRENT_BINARY_DIR}/LinkKit.cpp"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/LinkKit.cpp.in" clotho_runtime_exe ${clotho_kit_paths}
  VERBATIM)

add_library(clotho_link STATIC src/link/Link.cpp "${CMAKE_CURRENT_BINARY_DIR}/LinkKit.cpp")
target_include_directories(clotho_link PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(clotho_link SYSTEM PRIVATE ${LLD_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
# #embed is C23; GCC takes it in C++ silently, clang as an extension.
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/LinkKit.cpp" PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-c23-extensions>")
if(APPLE)
  target_link_libraries(clotho_link PRIVATE lldMachO)
elseif(WIN32)
  target_link_libraries(clotho_link PRIVATE lldMinGW lldCOFF)
else()
  target_link_libraries(clotho_link PRIVATE lldELF)
endif()
target_link_libraries(clotho_link PRIVATE lldCommon ${LLVM_LIBS} clotho_warnings)
target_link_libraries(clotho_cli PRIVATE clotho_link)

# Single doctest executable aggregating every tests/*.cpp.
enable_testing()
set(CLOTHO_TEST_SOURCES
  tests/test_main.cpp
  tests/sva_test.cpp
  tests/coverage_test.cpp
  tests/ir_test.cpp
  tests/verify_test.cpp
  tests/operands_test.cpp
  tests/sourcemap_test.cpp
  tests/lower_test.cpp
  tests/program_test.cpp
  tests/clocking_test.cpp
  tests/case_test.cpp
  tests/function_test.cpp
  tests/task_test.cpp
  tests/intra_assign_test.cpp
  tests/continuous_assign_delay_test.cpp
  tests/net_delay_test.cpp
  tests/string_test.cpp
  tests/dpi_test.cpp
  tests/display_test.cpp
  tests/strobe_test.cpp
  tests/loop_test.cpp
  tests/generate_test.cpp
  tests/instance_array_test.cpp
  tests/packed_range_test.cpp
  tests/const_table_test.cpp
  tests/z_elision_test.cpp
  tests/analysis_test.cpp
  tests/recovery_test.cpp
  tests/case_pragma_test.cpp
  tests/eval_test.cpp
  tests/legalize_test.cpp
  tests/perf_legalize_test.cpp
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
  tests/region_test.cpp
  tests/enum_test.cpp
  tests/sysfunc_test.cpp
  tests/randcase_test.cpp
  tests/randsequence_test.cpp
  tests/randomize_test.cpp
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
  tests/process_test.cpp
  tests/sync_test.cpp
  tests/severity_test.cpp
  tests/exit_status_test.cpp
  tests/stop_time_test.cpp
  tests/event_test.cpp
  tests/data_model_test.cpp
  tests/dynamic_array_test.cpp
  tests/memory_bounds_test.cpp
  tests/queue_test.cpp
  tests/associative_array_test.cpp
  tests/array_ordering_test.cpp
  tests/array_locator_test.cpp
  tests/class_test.cpp
  tests/gc_test.cpp
  tests/unbased_test.cpp
  tests/preprocess_test.cpp
  tests/interface_test.cpp
  tests/concat_select_test.cpp
  tests/wide_select_test.cpp
  tests/force_release_test.cpp
  tests/system_test.cpp
  tests/diagnostic_render_test.cpp
  tests/cli_test.cpp
  tests/exe_test.cpp
  tests/todo_test.cpp
)
# The server tests exist only where the servers do.
if(CLOTHO_LSP)
  list(APPEND CLOTHO_TEST_SOURCES tests/lsp_test.cpp tests/lsp_tokens_test.cpp)
endif()
if(CLOTHO_DAP)
  list(APPEND CLOTHO_TEST_SOURCES tests/dap_test.cpp)
endif()
add_executable(clotho_test ${CLOTHO_TEST_SOURCES})
target_link_libraries(clotho_test PRIVATE clotho clotho_ir clotho_legalize doctest::doctest clotho_warnings)
if(CLOTHO_LSP)
  target_link_libraries(clotho_test PRIVATE clotho_lsp glaze::glaze)
endif()
if(CLOTHO_DAP)
  target_link_libraries(clotho_test PRIVATE clotho_dap glaze::glaze)
endif()
# The reset corpus is 28 readable .sv files rather than embedded strings: they are
# a reference to be read and extended, and they are the same files run against
# yosys.
target_compile_definitions(clotho_test PRIVATE CLOTHO_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")

# The language-server and debug-adapter tests drive the real `clotho lsp` and
# `clotho dap` processes over stdio, because framing, session choreography and
# exit status are only observable from outside them. That makes the CLI a build
# input of the test binary, not just a sibling.
target_compile_definitions(clotho_test PRIVATE CLOTHO_EXE="$<TARGET_FILE:clotho_cli>")
add_dependencies(clotho_test clotho_cli)

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

# Register the suite with CTest as one entry per test file, for `ctest -j`. One
# entry per case (doctest_discover_tests) starts the test binary ~2700 times, and
# that startup outweighed the cases themselves. The data-model matrix is a third
# of the suite, so each of its columns is an entry of its own; its summary case
# re-walks every column and runs only in a plain `clotho_test` invocation.
#
# The file filter is anchored on the directory separator, in both spellings,
# because doctest wildcards cannot otherwise tell case_test.cpp from
# randcase_test.cpp. Commas separate alternatives in a doctest filter, which is
# why the matrix columns whose names contain one are matched by wildcard.
foreach(source IN LISTS CLOTHO_TEST_SOURCES)
  get_filename_component(stem "${source}" NAME_WE)
  if(stem STREQUAL "test_main")
    continue()
  endif()
  set(file_filter "-sf=*/${stem}.cpp,*\\${stem}.cpp")
  if(stem STREQUAL "data_model_test")
    add_test(NAME ${stem} COMMAND clotho_test --duration=true ${file_filter}
                                  "-tce=data model matrix*")
  else()
    add_test(NAME ${stem} COMMAND clotho_test --duration=true ${file_filter})
  endif()
endforeach()
foreach(column IN ITEMS "bare element" "fixed unpacked array" "unpacked struct member"
                        "dynamic array element" "queue element" "*int key" "*string key"
                        "class property")
  string(REGEX REPLACE "[^a-z]+" "_" name "${column}")
  string(REGEX REPLACE "^_" "" name "${name}")
  add_test(NAME data_model_matrix_${name}
           COMMAND clotho_test --duration=true "-tc=data model matrix: ${column}")
endforeach()

# Executables written by `clotho build`, linked by its in-process lld, and run:
# what no in-process test can show is that every platform's link works and the
# result runs. Each design prints its line only when its own checks pass, so the
# output is the whole verdict.

# Build `design` into ${name}, with `--dpi-lib` for each DPI library (a path,
# which may be a generator expression), after the DEPENDS targets.
function(clotho_exe_test name design)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "DPI;DEPENDS")
  set(exe "${CMAKE_CURRENT_BINARY_DIR}/${name}${CMAKE_EXECUTABLE_SUFFIX}")
  set(dpi "")
  foreach(lib IN LISTS arg_DPI)
    list(APPEND dpi --dpi-lib "${lib}")
  endforeach()
  add_custom_command(
    OUTPUT "${exe}"
    COMMAND clotho_cli build ${dpi} -o "${exe}" "${CMAKE_CURRENT_SOURCE_DIR}/${design}"
    DEPENDS clotho_cli "${design}" ${arg_DEPENDS}
    VERBATIM)
  add_custom_target(${name}_build DEPENDS "${exe}")
  # Built with the suite, so building clotho_test is enough to run all of ctest.
  add_dependencies(clotho_test ${name}_build)
endfunction()

clotho_exe_test(exe tests/exe/design.sv)
add_test(NAME exe COMMAND "${CMAKE_CURRENT_BINARY_DIR}/exe${CMAKE_EXECUTABLE_SUFFIX}" +n=12345)
set_tests_properties(exe PROPERTIES PASS_REGULAR_EXPRESSION "exe: ok 12345 000006e3924924924924924924924924")
# What an executable says it contains: the MinGW-w64 runtime's notices too on
# Windows, which links it in.
add_test(NAME exe_licenses COMMAND "${CMAKE_CURRENT_BINARY_DIR}/exe${CMAKE_EXECUTABLE_SUFFIX}" --licenses)
if(WIN32)
  set_tests_properties(exe_licenses PROPERTIES PASS_REGULAR_EXPRESSION "MinGW-w64 runtime licensing")
else()
  set_tests_properties(exe_licenses PROPERTIES PASS_REGULAR_EXPRESSION "Mozilla Public License Version 2.0")
endif()

add_library(clotho_exe_dpi_object OBJECT tests/exe/dpi.c)
set_target_properties(clotho_exe_dpi_object PROPERTIES POSITION_INDEPENDENT_CODE ON)
clotho_exe_test(exe_dpi_object tests/exe/dpi.sv DPI $<TARGET_OBJECTS:clotho_exe_dpi_object>
                DEPENDS clotho_exe_dpi_object)
add_test(NAME exe_dpi_object COMMAND "${CMAKE_CURRENT_BINARY_DIR}/exe_dpi_object${CMAKE_EXECUTABLE_SUFFIX}")
set_tests_properties(exe_dpi_object PROPERTIES PASS_REGULAR_EXPRESSION "dpi: ok")

# A DLL cannot call back into the executable, so Windows has no shared case.
if(NOT WIN32)
  add_library(clotho_exe_dpi_shared SHARED tests/exe/dpi.c)
  if(APPLE)
    # sv_twice is the executable's, found when the library is loaded.
    target_link_options(clotho_exe_dpi_shared PRIVATE "LINKER:-undefined,dynamic_lookup")
  endif()
  clotho_exe_test(exe_dpi_shared tests/exe/dpi.sv DPI $<TARGET_FILE:clotho_exe_dpi_shared>
                  DEPENDS clotho_exe_dpi_shared)
  add_test(NAME exe_dpi_shared COMMAND "${CMAKE_CURRENT_BINARY_DIR}/exe_dpi_shared${CMAKE_EXECUTABLE_SUFFIX}")
  set_tests_properties(exe_dpi_shared PROPERTIES PASS_REGULAR_EXPRESSION "dpi: ok")
endif()

# What `clotho build` links into an executable is not sanitized: the executable
# carries no sanitizer runtime (docs/design/executables.md), so instrumented code
# there could neither link nor load. The compiler that builds it is sanitized.
if(CLOTHO_SANITIZE)
  foreach(target clotho_runtime_exe clotho_exe_dpi_object clotho_exe_dpi_shared)
    if(TARGET ${target})
      target_compile_options(${target} PRIVATE -fno-sanitize=all)
      target_link_options(${target} PRIVATE -fno-sanitize=all)
    endif()
  endforeach()
endif()
