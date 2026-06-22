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

add_executable(clotho src/main.cpp)
target_link_libraries(clotho PRIVATE slang::slang)
