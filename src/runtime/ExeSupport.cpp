// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The two pieces of the C++ library an executable's runtime still calls, given
// the C library instead (docs/design/executables.md). Linked into executables
// only: clotho itself gets both from its C++ library.

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

namespace {

[[noreturn]] void out_of_memory(std::size_t bytes) {
    std::fprintf(stderr, "out of memory: the system refused %zu bytes\n", bytes);
    std::abort();
}

} // namespace

// The replaceable global allocation functions (C++ [new.delete]) behind the
// runtime's std containers. Built without exceptions, so a failure ends the run
// rather than throwing std::bad_alloc.
void* operator new(std::size_t bytes) {
    void* p = std::malloc(bytes == 0 ? 1 : bytes);
    if (p == nullptr) {
        out_of_memory(bytes);
    }
    return p;
}

void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

// Where libc++'s headers report a broken precondition, or an exception they
// cannot throw without exceptions: ExeConfig.h points _LIBCPP_VERBOSE_ABORT
// here instead of at libc++'s library.
extern "C" void rt_libcxx_abort(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::abort();
}

// std::string's members that libc++'s <string> declares `extern template`, for
// its library to provide. There is no library here, so instantiate them as
// libc++'s own src/string.cpp does, from the same list.
_LIBCPP_BEGIN_NAMESPACE_STD
#define CLOTHO_INSTANTIATE(...) template __VA_ARGS__;
#ifdef _LIBCPP_ABI_STRING_OPTIMIZED_EXTERNAL_INSTANTIATION
_LIBCPP_STRING_UNSTABLE_EXTERN_TEMPLATE_LIST(CLOTHO_INSTANTIATE, char)
#else
_LIBCPP_STRING_V1_EXTERN_TEMPLATE_LIST(CLOTHO_INSTANTIATE, char)
#endif
#undef CLOTHO_INSTANTIATE
_LIBCPP_END_NAMESPACE_STD
