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
# runtime is not part of that loop and -O0 there would distort every profile. What
# Debug guarantees everywhere is that NDEBUG is unset, so every assert() is live in
# both columns.
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

# `build`: writes the generated code (LLVM IR, or Clotho's own IR) and no
# executable, so nothing outside work on Clotho itself consumes it yet. Offered
# like `synth`: only the CLI wiring is conditional, and the emission paths behind
# it stay built and tested in every configuration.
option(CLOTHO_BUILD_COMMAND "Expose the `build` command, which writes generated code rather than an executable"
       ${CLOTHO_WIP_DEFAULT})

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
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
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

  add_custom_command(
    OUTPUT ${_libsva_lib} ${_libsva_include}/libsva/sva.h
    COMMAND ${ZIG_EXECUTABLE} build --prefix ${_libsva_prefix}
            -Doptimize=${_libsva_optimize}
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
  src/runtime/Runtime.cpp
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
# debugger. It deliberately stops short of RelWithDebInfo's NDEBUG -- the reason to
# optimize this target is that -O0 distorts profiles and slows simulation, and
# neither argument says anything about disabling checks. Debug is precisely where
# you want Wheel.cpp's `deadline must be in the future` to fire, since scheduling an
# event into the past is the bug it exists to catch. Release and RelWithDebInfo
# already supply the right flags, so the genex leaves them alone.
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
# A machine with ROCm installed has a second LLVM under /opt/rocm, and
# find_package picks it over the system one. That copy is built without RTTI, so
# LlvmBackend.cpp emits references to `typeinfo for llvm::CallbackVH` that nothing
# provides -- and the only symptom is an inscrutable link failure two minutes into
# the build, long after configure said everything was fine.
#
# Fail here instead, naming the fix. Clotho needs RTTI: LlvmBackend derives from
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
  src/Lower.cpp
  src/Sva.cpp
)
target_include_directories(clotho PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
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
if(CLOTHO_BUILD_COMMAND)
  target_compile_definitions(clotho_cli PRIVATE CLOTHO_BUILD_COMMAND)
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

# Single doctest executable aggregating every tests/*.cpp.
enable_testing()
set(CLOTHO_TEST_SOURCES
  tests/test_main.cpp
  tests/sva_test.cpp
  tests/coverage_test.cpp
  tests/ir_test.cpp
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

# An object from `build --emit obj`, linked by this build's own toolchain against
# clotho_runtime and run: the object is only useful if every platform's linker
# takes it, which no in-process test can show. The design prints its line only
# when its own checks pass, so the output is the whole verdict.
if(CLOTHO_BUILD_COMMAND)
  set(aot_object "${CMAKE_CURRENT_BINARY_DIR}/aot_design${CMAKE_CXX_OUTPUT_EXTENSION}")
  add_custom_command(
    OUTPUT "${aot_object}"
    COMMAND clotho_cli build --emit obj -o "${aot_object}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/design.sv"
    DEPENDS clotho_cli tests/aot/design.sv
    VERBATIM)
  set_source_files_properties("${aot_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_executable(clotho_aot_test tests/aot/main.cpp "${aot_object}")
  target_link_libraries(clotho_aot_test PRIVATE clotho_runtime)
  # Built with the suite, so building clotho_test is enough to run all of ctest.
  add_dependencies(clotho_test clotho_aot_test)
  add_test(NAME aot COMMAND clotho_aot_test +n=12345)
  set_tests_properties(aot PROPERTIES PASS_REGULAR_EXPRESSION
                                      "aot: ok 12345 000006e3924924924924924924924924")
endif()
