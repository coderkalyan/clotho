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

# Warnings apply to clotho targets only, not fetched dependencies.
add_library(clotho_warnings INTERFACE)
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

option(CLOTHO_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(CLOTHO_SANITIZE)
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
  llvm_map_components_to_libnames(LLVM_LIBS core support orcjit jitlink native)
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

# Simulation runtime: native support routines the JIT'd design calls into.
# Deliberately depends on nothing (no LLVM, no slang) — plain native code. The
# timing wheel is part of this surface: the runtime exposes it behind a C ABI.
add_library(clotho_runtime STATIC
  src/runtime/Runtime.cpp
  src/Wheel.cpp
)
target_include_directories(clotho_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clotho_runtime PRIVATE clotho_warnings)
# Always optimize the runtime, even in a Debug build. It is stable native code
# the JIT'd design calls into on every event; at -O0 its trivial leaf functions
# (unwrap, WheelImpl accessors) stop inlining and the timing wheel reads as pure
# call overhead — distorting `perf` profiles and slowing every simulation. Debug
# is for iterating on the compiler, not the runtime, so pin this target to -O2
# there (asserts stay live — NDEBUG is not forced, so the runtime keeps its
# safety checks). Release / RelWithDebInfo already optimize; the genex leaves
# them untouched.
target_compile_options(clotho_runtime PRIVATE $<$<CONFIG:Debug>:-O2>)

# Backend IR + passes. Target-agnostic, no slang dependency: handles the bulk
# of compiler logic short of the slang -> IR translation layer.
add_library(clotho_ir STATIC
  src/ir/InternPool.cpp
  src/ir/Ir.cpp
  src/ir/Builder.cpp
  src/ir/Printer.cpp
  src/ir/ZElision.cpp
  src/ir/LlvmBackend.cpp
  src/ir/Jit.cpp
)
target_include_directories(clotho_ir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${LLVM_INCLUDE_DIRS})
target_compile_definitions(clotho_ir PUBLIC ${LLVM_DEFINITIONS})
target_link_libraries(clotho_ir PUBLIC unordered_dense::unordered_dense clotho_runtime ${LLVM_LIBS})
target_link_libraries(clotho_ir PRIVATE clotho_warnings)

# Front-end: the slang -> IR translation. Pulls in slang and the IR;
# consumers get both transitively.
add_library(clotho STATIC
  src/SourceMap.cpp
  src/Lower.cpp
)
target_include_directories(clotho PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clotho PUBLIC clotho_ir slang::slang)
target_link_libraries(clotho PRIVATE clotho_warnings)

# Thin build driver.
add_executable(clotho_cli src/main.cpp)
set_target_properties(clotho_cli PROPERTIES OUTPUT_NAME clotho)
target_link_libraries(clotho_cli PRIVATE clotho clotho_warnings)

# Single doctest executable aggregating every tests/*.cpp.
enable_testing()
add_executable(clotho_test
  tests/test_main.cpp
  tests/ir_test.cpp
  tests/sourcemap_test.cpp
  tests/lower_test.cpp
  tests/case_test.cpp
  tests/function_test.cpp
  tests/task_test.cpp
  tests/string_test.cpp
  tests/display_test.cpp
  tests/loop_test.cpp
  tests/generate_test.cpp
  tests/instance_array_test.cpp
  tests/packed_range_test.cpp
  tests/z_elision_test.cpp
  tests/llvm_backend_test.cpp
  tests/two_state_test.cpp
  tests/real_test.cpp
  tests/wheel_test.cpp
  tests/runtime_test.cpp
  tests/compound_test.cpp
  tests/final_test.cpp
  tests/comb_fusion_test.cpp
  tests/sysfunc_test.cpp
)
target_link_libraries(clotho_test PRIVATE clotho doctest::doctest clotho_warnings)

# Register each doctest case as its own CTest test for parallelism and
# per-case reporting.
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(clotho_test)
