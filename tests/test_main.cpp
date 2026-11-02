// The single doctest runner translation unit. All other test files just add
// TEST_CASEs and link into this executable.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#endif

#include "Lower.h"
#include "Subprocess.h"
#include "Twin.h"
#include "ir/Jit.h"
#include "runtime/Runtime.h"

namespace {

// A child mode's output is compared byte for byte by the parent. cmd.exe's `>` and
// `2>` redirects hand the child text-mode stdout and stderr, which rewrite every \n
// as \r\n -- so the captured output would not match what was actually printed. Put
// both back to binary.
void binary_std_streams() {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

// Single-design mode: lower and simulate one source file, then exit. This is how
// run_design_subprocess runs a design in its own process; see Subprocess.h for
// when that is needed. Lowers the file's *text* rather than going
// through lower_files so the buffer is named "source", matching what the same
// design produces in-process.
int run_design(const char* path) {
    binary_std_streams();
    const std::string         text = clotho::test::detail::read_file(path);
    const clotho::LowerResult r    = clotho::lower_text(text);
    if (!r.ok || r.module == nullptr) {
        std::fputs(r.diagnostic_text().c_str(), stderr);
        return 1;
    }

    // Same resolver the CLI installs, so a runtime diagnostic emitted here
    // carries the file position and source snippet a real run would show —
    // which is the whole payload of a test that asserts on one.
    return clotho::ir::run_jit(*r.module, /*timings=*/nullptr, /*config=*/{}, /*plusargs=*/{},
                               clotho::loc_resolver(r));
}

// Encoding-violation mode: call the report the extra-asserts check reaches, as the
// generated code would, from line 7 column 12 of a file whose text is the file's
// first line. See encoding_check_test.cpp.
int report_encoding_violation(const char* path) {
    binary_std_streams();
    std::string line = clotho::test::detail::read_file(path);
    const rt_null_site site{ .where      = "design.sv:7:12",
                             .line       = line.c_str(),
                             .line_no    = 7,
                             .column     = 12,
                             .end_column = 15,
                             .kind       = RT_NULL_CLASS };
    rt_encoding_violation(&site, 42);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && clotho::test::run_design_flag == argv[1]) {
        return run_design(argv[2]);
    }
    if (argc == 3 && clotho::test::encoding_violation_flag == argv[1]) {
        return report_encoding_violation(argv[2]);
    }

    std::error_code ec;
    const auto      self = std::filesystem::absolute(argv[0], ec);
    clotho::test::self_exe = ec ? argv[0] : self.string();

    // Twin mode is a detector for builds that carry the shared-code check (see
    // Twin.h): every design the suite lowers is lowered again with each top twice.
    // CLOTHO_NO_TWIN=1 turns it off, to time the suite without it.
#ifdef CLOTHO_EXTRA_ASSERTS
    if (std::getenv("CLOTHO_NO_TWIN") == nullptr) {
        clotho::set_twin_check(clotho::test::suite_twin_check);
    }
#endif

    doctest::Context ctx(argc, argv);
    const int        rc = ctx.run();

    const clotho::TwinStats& twin = clotho::twin_stats();
    if (twin.twinned != 0 || !twin.skipped.empty()) {
        std::fprintf(stderr, "twin mode: %llu designs lowered with each top twice and checked\n",
                     static_cast<unsigned long long>(twin.twinned));
        for (const auto& [why, n] : twin.skipped) {
            std::fprintf(stderr, "twin mode: %llu designs skipped: %s\n", static_cast<unsigned long long>(n),
                         why.c_str());
        }
    }
    return rc;
}
