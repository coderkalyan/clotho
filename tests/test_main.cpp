// The single doctest runner translation unit. All other test files just add
// TEST_CASEs and link into this executable.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#else
#include <sys/resource.h>
#endif

#include "ConfigMatrix.h"
#include "Lower.h"
#include "Subprocess.h"
#include "TraceCheck.h"
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
// design produces in-process. CLOTHO_TEST_DPI_LIB, when set, names a relocatable
// object to link into the JIT (`--dpi-lib`): the C side of a design whose C calls
// an export (dpi_test.cpp).
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
    clotho::ir::SimulationConfig config;
    if (const char* const dpi_lib = std::getenv("CLOTHO_TEST_DPI_LIB"); dpi_lib != nullptr) {
        config.dpi_libs.emplace_back(dpi_lib);
    }
    return clotho::ir::run_jit(*r.module, /*timings=*/nullptr, config, /*plusargs=*/{}, clotho::loc_resolver(r));
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

#if !defined(_WIN32)
// A config-matrix child runs designs the suite lowered but may never have run, so
// it is held to limits no ordinary test needs: no core file (a crashed JIT's would
// be gigabytes), a bounded output file (a design that prints forever must not fill
// the disk) and a CPU budget that ends one its parent has lost.
void limit_variant_child() {
    const rlimit no_core{ .rlim_cur = 0, .rlim_max = 0 };
    const rlimit file{ .rlim_cur = 64ull << 20, .rlim_max = 64ull << 20 };
    const rlimit cpu{ .rlim_cur = 600, .rlim_max = 600 };
    setrlimit(RLIMIT_CORE, &no_core);
    setrlimit(RLIMIT_FSIZE, &file);
    setrlimit(RLIMIT_CPU, &cpu);
}
#endif

// Config-matrix child mode (ConfigMatrix.h): argv is the flag, the variant, the
// entry point to lower with (text | files | sources), `$system`'s access tier, the
// port-coercion flag, the stop time in ticks, then the entry's inputs. Lowers the
// design as the hook's caller did, then simulates it once under the variant.
int run_variant(int argc, char** argv) {
    namespace cm = clotho::test::config_matrix;
    binary_std_streams();
#if !defined(_WIN32)
    limit_variant_child();
#endif

    const cm::Variant* const variant = cm::find_variant(argv[2]);
    const std::string_view   entry   = argv[3];
    const std::string_view   system  = argv[4];
    const std::string_view   coerce  = argv[5];
    char*                    end     = nullptr;
    const uint64_t           ticks   = std::strtoull(argv[6], &end, 10);
    if (variant == nullptr || (entry != "text" && entry != "files" && entry != "sources") ||
        (system != "0" && system != "1" && system != "2") || (coerce != "0" && coerce != "1") || *end != '\0') {
        std::fprintf(stderr, "%s: bad arguments\n", argv[1]);
        return 2;
    }
    const auto allow_system = static_cast<clotho::ir::SystemAccess>(system[0] - '0');
    const bool allow_coerce = coerce == "1";
    const std::vector<std::string_view> inputs(argv + 7, argv + argc);

    std::string         text;
    clotho::LowerResult r;
    if (entry == "text") {
        text = clotho::test::detail::read_file(std::string(inputs.front()));
        r    = clotho::lower_text(text, nullptr, allow_system, allow_coerce);
    } else if (entry == "files") {
        r = clotho::lower_files(inputs, nullptr, allow_system, allow_coerce);
    } else {
        r = clotho::lower_sources(inputs, nullptr, allow_system, allow_coerce);
    }
    if (!r.ok || r.module == nullptr) {
        std::fprintf(stderr, "%s\n", std::string(cm::kReplayFailedMarker).c_str());
        std::fputs(r.diagnostic_text().c_str(), stderr);
        return cm::kReplayFailed;
    }

    clotho::ir::SimulationConfig config;
    variant->apply(config);
    if (variant == &cm::kSelfTest) {
        std::puts("selftest: a line only this variant prints");
        std::fflush(stdout);
    }
    config.stop_time_fs = cm::stop_time_fs(ticks, r.module->tick_fs);
    return clotho::ir::run_jit(*r.module, /*timings=*/nullptr, config, /*plusargs=*/{}, clotho::loc_resolver(r));
}

// Tells the config-matrix suite which test case the designs it is handed belong to.
struct ConfigMatrixListener : doctest::IReporter {
    explicit ConfigMatrixListener(const doctest::ContextOptions&) {}

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData& tc) override {
        clotho::test::config_matrix::begin_test(tc.m_name);
        clotho::test::trace_check::begin_test(tc.m_name);
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats& stats) override {
        clotho::test::config_matrix::end_test(stats.testCaseSuccess);
        clotho::test::trace_check::end_test(stats.testCaseSuccess);
    }
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

REGISTER_LISTENER("clotho-config-matrix", 1, ConfigMatrixListener);

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && clotho::test::run_design_flag == argv[1]) {
        return run_design(argv[2]);
    }
    if (argc == 3 && clotho::test::encoding_violation_flag == argv[1]) {
        return report_encoding_violation(argv[2]);
    }
    if (argc >= 8 && clotho::test::run_variant_flag == argv[1]) {
        return run_variant(argc, argv);
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

    // Trace-check mode (TraceCheck.h): every design the suite simulates is checked against the
    // scheduler's rules while it runs. CLOTHO_NO_TRACE_CHECK=1 turns it off (and the backend
    // then emits none of the calls), to time the suite without it.
#ifdef CLOTHO_EXTRA_ASSERTS
    if (std::getenv("CLOTHO_NO_TRACE_CHECK") == nullptr) {
        try {
            clotho::test::trace_check::install();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 2;
        }
    }
#endif

    // Config-matrix mode (ConfigMatrix.h): every design the suite lowers is run under
    // each backend gate in child processes. Opt-in, because it costs a process per
    // variant per design: CLOTHO_CONFIG_MATRIX=1.
    if (const char* matrix = std::getenv("CLOTHO_CONFIG_MATRIX"); matrix != nullptr && matrix[0] != '\0') {
        if (std::string_view(matrix) != "0" && std::string_view(matrix) != "1") {
            std::fprintf(stderr, "CLOTHO_CONFIG_MATRIX must be 0 or 1, not '%s'\n", matrix);
            return 2;
        }
        if (std::string_view(matrix) == "1") {
            try {
                clotho::test::config_matrix::install();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%s\n", e.what());
                return 2;
            }
        }
    }

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
    const int matrix_rc = clotho::test::config_matrix::finish();
    const int trace_rc  = clotho::test::trace_check::finish();
    return rc != 0 ? rc : (matrix_rc != 0 ? matrix_rc : trace_rc);
}
