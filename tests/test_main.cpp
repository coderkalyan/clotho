// The single doctest runner translation unit. All other test files just add
// TEST_CASEs and link into this executable.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "Lower.h"
#include "Subprocess.h"
#include "ir/Jit.h"

namespace {

// Single-design mode: lower and simulate one source file, then exit. This is how
// run_design_subprocess isolates a design that ends at $finish; see Subprocess.h
// for why that isolation is required. Lowers the file's *text* rather than going
// through lower_file so the buffer is named "source", matching what the same
// design produces in-process.
int run_design(const char* path) {
    const std::string         text = clotho::test::detail::read_file(path);
    const clotho::LowerResult r    = clotho::lower_text(text);
    if (!r.ok || r.module == nullptr) {
        std::fputs(r.diagnostics.c_str(), stderr);
        return 1;
    }

    return clotho::ir::run_jit(*r.module);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && clotho::test::run_design_flag == argv[1]) {
        return run_design(argv[2]);
    }

    std::error_code ec;
    const auto      self = std::filesystem::absolute(argv[0], ec);
    clotho::test::self_exe = ec ? argv[0] : self.string();

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
