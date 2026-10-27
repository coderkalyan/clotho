// How a run ends, as the process that started it sees it: the exit status after a
// `$fatal` (LRM 20.10).
//
// The in-process cases read the status run_jit returns, which is what the driver
// hands to exit(). The process case runs `clotho run` for real.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

struct Run {
    std::string out;
    std::string err;
    int         status = -1;
};

std::string drain(std::FILE* f) {
    std::fflush(f);
    std::rewind(f);
    std::string s;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        s.append(buf, n);
    }
    return s;
}

Run run(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* out_tmp = std::tmpfile();
    std::FILE* err_tmp = std::tmpfile();
    REQUIRE(out_tmp != nullptr);
    REQUIRE(err_tmp != nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(fileno(stdout));
    const int saved_err = dup(fileno(stderr));
    dup2(fileno(out_tmp), fileno(stdout));
    dup2(fileno(err_tmp), fileno(stderr));

    Run result;
    result.status = ir::run_jit(*r.module);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    result.out = drain(out_tmp);
    result.err = drain(err_tmp);
    std::fclose(out_tmp);
    std::fclose(err_tmp);
    return result;
}

// A design whose only statement is `body`, in a module with a final block that
// proves whether the finals ran.
std::string with_body(std::string_view body) {
    return "module m;\n"
           "  initial begin\n    " +
           std::string(body) +
           "\n  end\n"
           "  final $display(\"final ran\");\n"
           "endmodule\n";
}

} // namespace

// -- $fatal: LRM 20.10 "terminates the simulation with an error code" --

TEST_CASE("exit status: $fatal ends the run with status 1, whatever its finish number") {
    for (const char* const call : { "$fatal;", "$fatal(0, \"x\");", "$fatal(1, \"x\");", "$fatal(2, \"x\");" }) {
        CAPTURE(call);
        CHECK(run(with_body(call)).status == 1);
    }
}

TEST_CASE("exit status: a $fatal still runs the final blocks, as iverilog does") {
    const Run r = run(with_body("$fatal(1, \"stop\");"));
    CHECK(r.out == "final ran\n");
    CHECK(r.status == 1);
}

TEST_CASE("exit status: only $fatal changes it") {
    for (const char* const body : {
           "$finish;",
           "$finish(0);",
           "$finish(1);",
           "$finish(2);",
           "$stop;",
           "$info(\"i\");",
           "$warning(\"w\");",
           "$error(\"e\");",
           "assert (0);",
           "assert (0) else $error(\"custom\");",
           "$display(\"nothing at all\");",
         }) {
        CAPTURE(body);
        CHECK(run(with_body(body)).status == 0);
    }
}

TEST_CASE("exit status: an earlier $fatal's status does not leak into the next run") {
    CHECK(run(with_body("$fatal;")).status == 1);
    CHECK(run(with_body("$display(\"ok\");")).status == 0);
}

// The simulator's own run-time fatals are reported and ended through the same
// severity + finish pair `$fatal` is, so they carry its error code too.
TEST_CASE("exit status: a failed $cast task, a run-time fatal of the design, ends with status 1") {
    const Run r = run("class A; endclass\n"
                      "class B extends A; endclass\n"
                      "class C extends A; endclass\n"
                      "module m;\n"
                      "  A a; B b; C c;\n"
                      "  initial begin\n"
                      "    c = new;\n"
                      "    a = c;\n"
                      "    $cast(b, a);\n"
                      "    $display(\"unreachable\");\n"
                      "  end\n"
                      "  final $display(\"final ran\");\n"
                      "endmodule\n");
    CHECK(r.err.find("FATAL: source:9: m at 0 s: $cast: the source is not assignment compatible") !=
          std::string::npos);
    CHECK(r.out == "final ran\n");
    CHECK(r.status == 1);
}

// -- `clotho run` as a process --

#if !defined(_WIN32)

#include <sys/wait.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

#include "Subprocess.h"

namespace {

namespace fs = std::filesystem;

// A temp directory holding one test's design, dump and captured streams.
class Scratch {
public:
    Scratch() : dir_(fs::temp_directory_path() / clotho::test::detail::unique_stem()) {
        fs::create_directories(dir_);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    Scratch(const Scratch&)            = delete;
    Scratch& operator=(const Scratch&) = delete;

    fs::path path(std::string_view name) const {
        return dir_ / name;
    }

    fs::path write(std::string_view name, std::string_view text) const {
        const fs::path p = path(name);
        std::ofstream  f(p, std::ios::binary);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        return p;
    }

private:
    fs::path dir_;
};

std::string read(const fs::path& p) {
    return clotho::test::detail::read_file(p);
}

} // namespace

// -- $fatal, through the driver --

namespace {

struct Cli {
    int         code = -1;
    std::string out;
    std::string err;
};

Cli run_cli(const Scratch& s, std::string_view source) {
    const fs::path design = s.write("design.sv", source);
    const fs::path out    = s.path("cli.out");
    const fs::path err    = s.path("cli.err");
    const std::string cmd =
      std::format("\"{}\" run \"{}\" > \"{}\" 2> \"{}\"", CLOTHO_EXE, design.string(), out.string(), err.string());
    const int status = std::system(cmd.c_str());
    return Cli{ WIFEXITED(status) ? WEXITSTATUS(status) : -1, read(out), read(err) };
}

} // namespace

TEST_CASE("exit status: `clotho run` exits 1 after a $fatal and 0 after $finish(2) and $error") {
    const Scratch s;
    const Cli     fatal = run_cli(s, with_body("$fatal(1, \"giving up\");"));
    CHECK(fatal.code == 1);
    CHECK(fatal.out == "final ran\n");
    CHECK(fatal.err.find("FATAL: ") != std::string::npos);

    CHECK(run_cli(s, with_body("$finish(2);")).code == 0);
    CHECK(run_cli(s, with_body("$error(\"e\");")).code == 0);
}

#endif // !defined(_WIN32)
