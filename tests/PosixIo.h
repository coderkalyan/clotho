#pragma once

// The stdout-capture idiom the test files share — dup the real stdout, point
// fileno(stdout) at a temp file with dup2, run, then dup2 back and close — is
// POSIX, declared in <unistd.h>. The MSVC runtime declares the same four names
// in <io.h> (as deprecated aliases of the _-prefixed spellings), so the only
// portability work is picking the right header.

#if defined(_WIN32)
// Keeps <io.h> from deprecating the POSIX spellings of names it declares.
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#include <io.h>
#else
#include <unistd.h>
#endif
