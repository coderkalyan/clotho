// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The `main` of an executable `clotho build` writes: read the command line, arm
// the run through rt_run and exit with the status it reports (docs/design/
// executables.md). Everything else about the run was fixed when the design was
// compiled, and reaches here as clotho_design. Linked into executables only;
// clotho has its own main.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Runtime.h"

// CLOTHO_LICENSES_INC names the generated array of the notices --licenses
// prints (CMakeLists.txt, clotho_runtime_exe).
#include CLOTHO_LICENSES_INC

extern "C" int32_t         clotho_main(void);
extern "C" const rt_design clotho_design;

namespace {

constexpr const char* kVersion = CLOTHO_VERSION " (" CLOTHO_GIT_HASH ")";

// argv[0] without its directory, for messages.
const char* program_name(const char* argv0) {
    const char* name = argv0;
    for (const char* p = argv0; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

int usage_error(const char* program, const char* message) {
    std::fprintf(stderr, "%s: %s\ntry '%s --help'\n", program, message, program);
    return 2;
}

void print_help(const char* program) {
    std::printf("Usage: %s [options] [+plusarg...]\n"
                "\n"
                "Simulates %s, as compiled by clotho %s.\n"
                "\n"
                "Options:\n"
                "  --stop-time <time>  Stop the simulation at this simulation time, an integer and a unit\n"
                "                      with no space between (fs, ps, ns, us, ms or s), e.g. 100ns or 5us.\n"
                "                      Everything at or before that time runs, then the run ends as if the\n"
                "                      design had called $finish there: final blocks run and dump files are\n"
                "                      flushed. A time finer than the design's time precision is rounded up\n"
                "                      to the next tick\n"
                "  --licenses          Print the licenses of the code this program contains\n"
                "  --version           Print the version of clotho that compiled this program\n"
                "  -h, --help          Print this help\n"
                "\n"
                "An argument starting with '+' goes to the design itself, for $test$plusargs and\n"
                "$value$plusargs to read while it runs (LRM 21.6).\n"
                "\n"
                "Exit status: 0 when the simulation ends, 1 after a $fatal, 2 for a usage error, 130\n"
                "when interrupted.\n",
                program,
                clotho_design.top,
                kVersion);
}

} // namespace

int main(int argc, char** argv) {
    const char* program = program_name(argc > 0 ? argv[0] : "simulation");

    // At most one plusarg per argument.
    auto*    plusargs = static_cast<const char**>(std::malloc(sizeof(const char*) * static_cast<size_t>(argc)));
    auto*    lens     = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * static_cast<size_t>(argc)));
    uint32_t count    = 0;
    bool     has_stop = false;
    uint64_t stop_fs  = 0;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg[0] == '+') {
            plusargs[count] = arg + 1;
            lens[count]     = static_cast<uint32_t>(std::strlen(arg + 1));
            ++count;
            continue;
        }
        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            print_help(program);
            return 0;
        }
        if (std::strcmp(arg, "--version") == 0) {
            std::printf("%s: a simulation of %s, compiled by clotho %s\n", program, clotho_design.top, kVersion);
            return 0;
        }
        if (std::strcmp(arg, "--licenses") == 0) {
            std::fwrite(kLicenses, 1, sizeof(kLicenses), stdout);
            return 0;
        }
        const char* value = nullptr;
        if (std::strcmp(arg, "--stop-time") == 0) {
            if (i + 1 == argc) {
                return usage_error(program, "--stop-time needs a time, e.g. --stop-time 100ns");
            }
            value = argv[++i];
        } else if (std::strncmp(arg, "--stop-time=", 12) == 0) {
            value = arg + 12;
        }
        if (value == nullptr) {
            char message[512];
            std::snprintf(message,
                          sizeof(message),
                          "unknown argument '%s': an option starts with '--', and an argument for the design "
                          "with '+'",
                          arg);
            return usage_error(program, message);
        }
        char error[512];
        if (rt_parse_stop_time(value, std::strlen(value), &stop_fs, error, sizeof(error)) == 0) {
            return usage_error(program, error);
        }
        has_stop = true;
    }

    const rt_run_config config = {
        .plusargs        = plusargs,
        .plusarg_lens    = lens,
        .plusarg_count   = count,
        .system_access   = clotho_design.system_access,
        .loose_stale_ref = 0,
        .has_stop_time   = has_stop ? 1 : 0,
        .stop_fs         = stop_fs,
        .tick_fs         = clotho_design.tick_fs,
    };
    return rt_run(&config, clotho_main);
}
