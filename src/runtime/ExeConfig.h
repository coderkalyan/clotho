// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Included ahead of every source of clotho_runtime_exe (-include, in
// CMakeLists.txt): the settings that keep the executable's runtime from needing
// any part of a C++ library that its headers do not define
// (docs/design/executables.md).

// libc++ checks nothing a release libstdc++ build does not, so clotho's JIT
// runtime and the executable's run the same code.
#define _LIBCPP_HARDENING_MODE _LIBCPP_HARDENING_MODE_NONE

// What libc++'s headers call for a broken precondition, or for an exception
// they cannot throw without exceptions; libc++ lets this macro replace its
// library's __libcpp_verbose_abort. ExeSupport.cpp defines the function.
extern "C" [[noreturn]] void rt_libcxx_abort(const char* format, ...);
#define _LIBCPP_VERBOSE_ABORT(...) ::rt_libcxx_abort(__VA_ARGS__)

// {fmt} compiled in, with no std::locale.
#define FMT_HEADER_ONLY
#define FMT_USE_LOCALE 0

// Without exceptions {fmt} would still construct its exception object to call
// what() on it, and those types' constructors live in the C++ library. Report
// the expression instead; reaching one is a bug in a format call, not input.
#define FMT_THROW(x) ::fmt::assert_fail(__FILE__, __LINE__, #x)
