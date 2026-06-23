cmake_minimum_required(VERSION 3.20)
project(clotho CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
  slang
  GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
  GIT_TAG        v8.0
  GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(slang)

# Header-only: SOURCE_SUBDIR none skips doctest's own CMakeLists (its
# cmake_minimum_required predates CMake 4) and just populates the source.
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  GIT_SHALLOW    ON
  SOURCE_SUBDIR  none
)
FetchContent_MakeAvailable(doctest)
add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest INTERFACE ${doctest_SOURCE_DIR})

FetchContent_Declare(
  unordered_dense
  GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
  GIT_TAG        v4.4.0
  GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(unordered_dense)

# Backend IR + passes. Target-agnostic, no slang dependency: handles the bulk
# of compiler logic short of the slang -> IR translation layer.
add_library(clotho_ir STATIC
  src/ir/InternPool.cpp
  src/ir/Ir.cpp
  src/ir/Builder.cpp
  src/ir/Printer.cpp
)
target_include_directories(clotho_ir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clotho_ir PUBLIC unordered_dense::unordered_dense)

# Front-end and the (future) slang -> IR translation. Pulls in slang and the
# IR; consumers get both transitively.
add_library(clotho STATIC
  src/Frontend.cpp
  src/SourceMap.cpp
  src/Lower.cpp
)
target_include_directories(clotho PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clotho PUBLIC clotho_ir slang::slang)

# Thin build driver.
add_executable(clotho_cli src/main.cpp)
set_target_properties(clotho_cli PROPERTIES OUTPUT_NAME clotho)
target_link_libraries(clotho_cli PRIVATE clotho)

# Single doctest executable aggregating every tests/*.cpp.
enable_testing()
add_executable(clotho_test
  tests/test_main.cpp
  tests/ir_test.cpp
  tests/frontend_test.cpp
  tests/sourcemap_test.cpp
  tests/lower_test.cpp
)
target_link_libraries(clotho_test PRIVATE clotho doctest::doctest)
add_test(NAME clotho_test COMMAND clotho_test)
