#pragma once

// Config-matrix mode for the suite (docs/VERIFICATION.md, "Detectors"): every design
// a test lowers is run in child processes under the default configuration and under
// each semantics-preserving backend gate flipped on its own, and the test that
// lowered it fails if the output differs. A gate that holds its contract changes
// nothing a design can observe, so for a design whose output the LRM fixes, any
// difference is a bug in the gate. A design whose output the LRM leaves open (LRM
// 4.7) can differ legitimately, and a test that owns one is listed in
// tests/config-matrix-known.txt.
//
// Opt-in, `CLOTHO_CONFIG_MATRIX=1`: each design costs a process per variant. The
// hook is the lowering entry points' (`set_design_check`, Lower.h), as twin mode's is,
// and the child is the test binary re-executed in the hidden mode test_main.cpp
// implements (Subprocess.h), once per run.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

#include "Lower.h"
#include "Subprocess.h"
#include "ir/Ir.h"
#include "ir/Module.h"
#include "ir/SimConfig.h"

namespace clotho::test::config_matrix {

// One configuration a design is run under: the default one with exactly one gate
// flipped (`apply` flips it).
struct Variant {
    std::string_view name;
    void (*apply)(ir::SimulationConfig&);
};

// The default first, then every gate of SimulationConfig that exists to be
// differential-tested (its comments say so: "semantics are identical either way").
// `lower_coro` is forced on by run_jit, `verify_shared_code` is a check and not a
// behavior, and the stats, sanitizers, DPI libraries and `--gc-*` sizing knobs
// change what is reported rather than what runs.
inline constexpr Variant kVariants[] = {
    { "default", [](ir::SimulationConfig&) {} },
    { "fuse=false", [](ir::SimulationConfig& c) { c.fuse = false; } },
    { "comb_fuse=false", [](ir::SimulationConfig& c) { c.comb_fuse = false; } },
    { "thunk=false", [](ir::SimulationConfig& c) { c.thunk = false; } },
    { "regions=false", [](ir::SimulationConfig& c) { c.regions = false; } },
    { "dedup=false", [](ir::SimulationConfig& c) { c.dedup = false; } },
    { "split=false", [](ir::SimulationConfig& c) { c.split = false; } },
    { "z_elision=false", [](ir::SimulationConfig& c) { c.z_elision = false; } },
    { "o3=true", [](ir::SimulationConfig& c) { c.o3 = true; } },
    { "gc_stress=true", [](ir::SimulationConfig& c) { c.gc_stress = true; } },
};

// The variant `selftest=differ` is the check's own test double: it runs the design
// as the default does and prints one line more (test_main.cpp), so a difference
// exists whatever Clotho does. It is in no default run.
inline constexpr Variant kSelfTest = { "selftest=differ", [](ir::SimulationConfig&) {} };

inline const Variant* find_variant(std::string_view name) {
    for (const Variant& v : kVariants) {
        if (v.name == name) {
            return &v;
        }
    }
    return name == kSelfTest.name ? &kSelfTest : nullptr;
}

// Every run ends at this simulation time, counted in ticks of the design's own
// finest time unit (Module::tick_fs), so a free-running design (a clock nobody
// stops) ends where every variant can reach it. A design that ends sooner is
// unaffected.
inline constexpr uint64_t kStopTicks = 10'000'000;

// `ticks` of a design whose tick is `tick_fs` femtoseconds, in femtoseconds, which
// is what SimulationConfig::stop_time_fs counts. Saturates.
inline uint64_t stop_time_fs(uint64_t ticks, uint64_t tick_fs) {
    tick_fs = std::max<uint64_t>(tick_fs, 1);
    return ticks > UINT64_MAX / tick_fs ? UINT64_MAX : ticks * tick_fs;
}

// What a child of test_main.cpp's replay mode exits with, and prints first on
// stderr, when the design does not lower for it. It lowered for the test, so the
// replay is not of the same design and nothing it ran is comparable.
inline constexpr int              kReplayFailed       = 97;
inline constexpr std::string_view kReplayFailedMarker = "config matrix: the replay did not lower the design";

// How a child ended, for a message.
inline std::string describe_status(int status) {
#if defined(_WIN32)
    return "exit " + std::to_string(status);
#else
    if (WIFEXITED(status)) {
        return "exit " + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return "signal " + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")";
    }
    return "status " + std::to_string(status);
#endif
}

namespace impl {

// A line of output for a message: bounded, with control bytes shown as \xNN.
inline std::string quote(std::string_view line) {
    constexpr size_t kMax = 120;
    std::string      out  = "`";
    for (size_t k = 0; k < line.size() && k < kMax; ++k) {
        const auto c = static_cast<unsigned char>(line[k]);
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\x%02x", c);
            out += hex;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += line.size() > kMax ? "...`" : "`";
    return out;
}

inline std::string seconds_text(double seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", seconds);
    return text;
}

} // namespace impl

// Where two outputs first differ, as "line N: default `a`, variant `b`", or empty
// when they are the same bytes.
inline std::string first_difference(std::string_view base, std::string_view other) {
    if (base == other) {
        return {};
    }
    size_t base_at = 0;
    size_t other_at = 0;
    for (size_t line = 1;; ++line) {
        const bool base_end  = base_at >= base.size();
        const bool other_end = other_at >= other.size();
        if (base_end && other_end) {
            return "differs only in its final newline";
        }
        const size_t           base_nl  = base.find('\n', base_at);
        const size_t           other_nl = other.find('\n', other_at);
        const std::string_view base_line =
          base_end ? std::string_view{} : base.substr(base_at, (base_nl == std::string_view::npos ? base.size() : base_nl) - base_at);
        const std::string_view other_line =
          other_end ? std::string_view{}
                    : other.substr(other_at, (other_nl == std::string_view::npos ? other.size() : other_nl) - other_at);
        if (base_end || other_end || base_line != other_line) {
            return "differs at line " + std::to_string(line) + ": default " +
                   (base_end ? std::string("<end of output>") : impl::quote(base_line)) + ", variant " +
                   (other_end ? std::string("<end of output>") : impl::quote(other_line));
        }
        base_at  = base_nl == std::string_view::npos ? base.size() : base_nl + 1;
        other_at = other_nl == std::string_view::npos ? other.size() : other_nl + 1;
    }
}

// What differs between the default run and another one, or empty when nothing the
// design can observe does: exit status, stdout and stderr.
inline std::string difference(const DesignRun& base, const DesignRun& other) {
    std::string out;
    const auto  add = [&out](const std::string& part) {
        out += out.empty() ? part : "; " + part;
    };
    if (base.status != other.status) {
        add("exit status: default " + describe_status(base.status) + ", variant " + describe_status(other.status));
    }
    if (base.out != other.out) {
        add("stdout " + first_difference(base.out, other.out));
    }
    if (base.err != other.err) {
        add("stderr " + first_difference(base.err, other.err));
    }
    return out;
}

namespace impl {

// Instructions whose effect is outside the process, or that read what the child
// does not have: the same design run twice, or by a child with another working
// directory, stdin or command line, is not guaranteed to do the same thing.
// Anything not listed is compared, so a missing entry shows up as a mismatch to
// triage and never as a design that went unchecked.
inline const std::array<std::string_view, 256>& effect_table() {
    static const std::array<std::string_view, 256> table = [] {
        using Tag = ir::Inst::Tag;
        std::array<std::string_view, 256> t{};
        const auto set = [&t](Tag tag, std::string_view why) { t[static_cast<uint8_t>(tag)] = why; };
        set(Tag::FopenFd, "opens a file ($fopen)");
        set(Tag::FopenMcd, "opens a file ($fopen)");
        for (const Tag tag : { Tag::Fclose, Tag::Fgetc, Tag::Ungetc, Tag::Feof, Tag::Ftell, Tag::Fseek, Tag::Rewind,
                               Tag::Fgets, Tag::Fscanf, Tag::FreadWord, Tag::FreadMem, Tag::Ferror }) {
            set(tag, "reads or closes a file descriptor ($fgetc, $fscanf, ...)");
        }
        set(Tag::System, "runs a shell command ($system)");
        set(Tag::TestPlusArgs, "reads plusargs");
        set(Tag::ValuePlusArgs, "reads plusargs");
        for (const Tag tag : { Tag::DumpFile, Tag::DumpVars, Tag::DumpLimit, Tag::DumpControl }) {
            set(tag, "writes a VCD file");
        }
        return t;
    }();
    return table;
}

} // namespace impl

// Why `m` is not run under the variants, or empty when it is. A design that
// does nothing has nothing to compare, and one that reaches outside the process
// (files, a shell, DPI-C, plusargs) is not the same design in a child.
inline std::string skip_reason(const ir::Module& m) {
    if (m.tops().empty()) {
        return "no top-level module";
    }
    if (m.processes().empty()) {
        return "no process";
    }
    const auto& table = impl::effect_table();
    std::string why;
    const auto  scan = [&](const ir::Ir& body) {
        for (uint32_t k = 0; k < body.inst_count() && why.empty(); ++k) {
            why = std::string(table[static_cast<uint8_t>(body.tag(static_cast<ir::Index>(k)))]);
        }
    };
    for (const ir::Process& p : m.processes()) {
        scan(p.ir);
    }
    for (size_t f = 0; f < m.function_count() && why.empty(); ++f) {
        const ir::Function& fn = m.function(static_cast<ir::FunctionId>(f));
        if (fn.is_dpi) {
            why = "imports a DPI-C function";
        } else {
            scan(fn.ir);
        }
    }
    return why;
}

// A test case whose design legitimately or knowingly differs under some variant
// (tests/config-matrix-known.txt).
struct KnownEntry {
    std::string category; // "race" or "bug"
    std::string reason;
    size_t      line = 0;
};

// Parse the known list: `<category> "<test case name>" <reason, to end of line>`.
// This is the format of .github/scripts/sv-tests-known.txt, but for the quotes a
// doctest name needs because it contains spaces. Throws, naming the line, on
// anything it does not recognise: a typo must not silently stop covering a test.
inline std::map<std::string, KnownEntry> parse_known(std::string_view text, std::string_view file = "config-matrix-known.txt") {
    std::map<std::string, KnownEntry> entries;
    size_t                            line_no = 0;
    for (size_t at = 0; at <= text.size();) {
        const size_t           nl   = std::min(text.find('\n', at), text.size());
        const std::string_view line = text.substr(at, nl - at);
        at                          = nl + 1;
        ++line_no;

        const auto fail = [&](const std::string& why) {
            throw std::runtime_error(std::string(file) + ":" + std::to_string(line_no) + ": " + why);
        };
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos || line[first] == '#') {
            continue;
        }
        std::string_view rest     = line.substr(first);
        const size_t     cat_end  = rest.find_first_of(" \t");
        const std::string category(rest.substr(0, cat_end));
        if (category != "race" && category != "bug") {
            fail("category '" + category + "' is not race or bug");
        }
        rest = cat_end == std::string_view::npos ? std::string_view{} : rest.substr(cat_end);
        rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
        if (rest.empty() || rest.front() != '"') {
            fail("the test case name must follow the category, in double quotes");
        }
        const size_t close = rest.find('"', 1);
        if (close == std::string_view::npos) {
            fail("the test case name has no closing quote");
        }
        const std::string name(rest.substr(1, close - 1));
        rest.remove_prefix(close + 1);
        rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ' || rest.back() == '\t')) {
            rest.remove_suffix(1);
        }
        if (name.empty()) {
            fail("the test case name is empty");
        }
        if (rest.empty()) {
            fail("an entry needs a reason");
        }
        const auto [it, fresh] = entries.try_emplace(name, KnownEntry{ category, std::string(rest), line_no });
        if (!fresh) {
            fail("\"" + name + "\" is already listed at line " + std::to_string(it->second.line));
        }
    }
    return entries;
}

struct Options {
    // Variants to run besides the default, by name; empty runs all of them.
    std::vector<std::string> variants;
    // Wall-clock limit of each child.
    std::chrono::milliseconds limit{ 60'000 };
    // Simulation time every run ends at, in ticks of the design's finest time unit.
    uint64_t stop_ticks = kStopTicks;
    // File one line per design is appended to (outcome, test case, detail, seconds);
    // empty writes none. Appends of other processes interleave by line.
    std::string log;
};

// What running one design under the variants found.
struct Mismatch {
    std::string variant;
    std::string what;
    bool        flaky = false; // a second run of the variant agreed with the default
};

struct Outcome {
    std::string           skipped; // why the design was not compared, or empty
    std::vector<Mismatch> mismatches;
    std::string           replay;      // command that reruns the first mismatching variant
    size_t                variants = 0; // variants compared with the default
    double                seconds  = 0; // wall time of all the runs
};

struct Stats {
    uint64_t                        checked  = 0; // designs run under every variant
    uint64_t                        repeated = 0; // designs the process had already checked
    uint64_t                        unlisted = 0; // designs that differ, thrown to the test
    std::map<std::string, uint64_t> known;        // designs that differ and a known entry covers, by category
    std::map<std::string, uint64_t> skipped;      // designs not compared, by reason
    double                          seconds = 0;  // wall time spent in children
};

// The check itself and its bookkeeping. The suite installs one (install below); the
// tests of the check build their own.
class Suite {
public:
    Suite(Options options, std::map<std::string, KnownEntry> known)
        : options_(std::move(options)), known_(std::move(known)) {
        for (const std::string& name : options_.variants) {
            if (find_variant(name) == nullptr || name == "default") {
                throw std::runtime_error("config matrix: '" + name + "' is not a variant");
            }
        }
    }

    // The doctest test case the designs that follow belong to; known entries are
    // keyed by it.
    void begin_test(std::string name) {
        test_ = std::move(name);
    }

    // A known entry whose test passed without any of its designs differing is out of
    // date: the gate was fixed, or the test changed.
    void end_test(bool success) {
        if (success && known_.count(test_) != 0 && differed_.count(test_) == 0) {
            stale_.push_back(test_);
        }
        test_.clear();
    }

    // Run `result`'s design under the variants and throw if it does not behave as
    // under the default configuration, unless the current test is a known entry.
    void check(const LowerResult& result, const LowerInputs& inputs) {
        const std::string key = cache_key(inputs);
        if (!key.empty()) {
            const auto it = cache_.find(key);
            if (it != cache_.end()) {
                report(it->second, /*repeat=*/true);
                return;
            }
        }
        const Outcome outcome = evaluate(result, inputs);
        if (!key.empty()) {
            cache_.emplace(key, outcome);
        }
        report(outcome, /*repeat=*/false);
    }

    const Stats&                    stats() const { return stats_; }
    const std::vector<std::string>& stale() const { return stale_; }
    const std::map<std::string, KnownEntry>& known() const { return known_; }

private:
    // A design that came from text is the same design when the text and the flags
    // are; the others name files, which can change under the same name.
    static std::string cache_key(const LowerInputs& in) {
        if (in.entry != LowerInputs::Entry::Text) {
            return {};
        }
        return std::to_string(static_cast<int>(in.allow_system)) + (in.allow_port_coercion ? "c" : "-") + std::string(in.text);
    }

    // The inputs of the child that replays a design, and the temp file holding its
    // source when it came from text.
    struct Job {
        std::string              entry;
        std::vector<std::string> inputs;
        std::string              system;
        std::string              coercion;
        std::filesystem::path    source;
        bool                     keep = false;

        Job() = default;
        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;
        ~Job() {
            if (!source.empty() && !keep) {
                std::error_code ec;
                std::filesystem::remove(source, ec);
            }
        }
    };

    static void make_job(const LowerInputs& in, Job& job) {
        job.system   = std::to_string(static_cast<int>(in.allow_system));
        job.coercion = in.allow_port_coercion ? "1" : "0";
        switch (in.entry) {
        case LowerInputs::Entry::Text: {
            job.entry  = "text";
            job.source = std::filesystem::temp_directory_path() / (detail::unique_stem() + ".sv");
            std::ofstream f(job.source, std::ios::binary);
            f.write(in.text.data(), static_cast<std::streamsize>(in.text.size()));
            job.inputs.push_back(job.source.string());
            return;
        }
        case LowerInputs::Entry::Files:
            job.entry = "files";
            break;
        case LowerInputs::Entry::Sources:
            job.entry = "sources";
            break;
        }
        for (const std::string_view arg : in.args) {
            job.inputs.emplace_back(arg);
        }
    }

    std::vector<std::string> child_args(const Job& job, std::string_view variant) const {
        std::vector<std::string> args{ std::string(run_variant_flag), std::string(variant), job.entry, job.system,
                                       job.coercion, std::to_string(options_.stop_ticks) };
        args.insert(args.end(), job.inputs.begin(), job.inputs.end());
        return args;
    }

    DesignRun run(const Job& job, std::string_view variant) {
#if defined(_WIN32)
        (void)job;
        (void)variant;
        throw std::runtime_error("config matrix mode needs POSIX process control and does not run on Windows");
#else
        DesignRun r = run_self_bounded(child_args(job, variant), options_.limit);
        stats_.seconds += r.seconds;
        return r;
#endif
    }

    static std::string shell_word(const std::string& s) {
        const bool plain = !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || std::strchr("_./=:+,-", c) != nullptr;
        });
        return plain ? s : "'" + s + "'";
    }

    // A replay of a child fails to lower the design only if the design is not the one
    // the test lowered, which this check cannot compare.
    static bool replay_failed(const DesignRun& r) {
        return design_exit_code(r) == kReplayFailed && r.err.find(kReplayFailedMarker) != std::string::npos;
    }

    Outcome evaluate(const LowerResult& result, const LowerInputs& inputs) {
        Outcome out;
        if (!result.ok || result.module == nullptr) {
            out.skipped = "lowering refused the design";
            return out;
        }
        if (std::string why = skip_reason(*result.module); !why.empty()) {
            out.skipped = std::move(why);
            return out;
        }

        const auto before = stats_.seconds;
        Job        job;
        make_job(inputs, job);

        const DesignRun base = run(job, "default");
        if (base.timed_out) {
            out.skipped = "the default run reached the time limit";
        } else if (replay_failed(base)) {
            out.skipped = "the replay did not lower the design";
        }
        if (!out.skipped.empty()) {
            out.seconds = stats_.seconds - before;
            return out;
        }

        std::vector<std::string_view> names;
        for (const Variant& v : kVariants) {
            names.push_back(v.name);
        }
        if (!options_.variants.empty()) {
            names.assign(options_.variants.begin(), options_.variants.end());
        }
        for (const std::string_view name : names) {
            if (name == "default") {
                continue;
            }
            ++out.variants;
            const DesignRun other = run(job, name);
            std::string     what;
            if (other.timed_out) {
                what = "timed out after " + impl::seconds_text(other.seconds) + " s; the default run took " +
                       impl::seconds_text(base.seconds) + " s";
            } else {
                what = difference(base, other);
            }
            if (!what.empty()) {
                out.mismatches.push_back(Mismatch{ std::string(name), std::move(what), false });
            }
        }

        if (!out.mismatches.empty()) {
            // A default that does not repeat itself is not a baseline; and a variant that
            // agrees with the default the second time is a timing effect, which is
            // reported as one.
            const DesignRun again = run(job, "default");
            if (again.timed_out || !difference(base, again).empty()) {
                out.mismatches.clear();
                out.skipped = "the default run is not deterministic";
            } else {
                for (Mismatch& m : out.mismatches) {
                    const DesignRun rerun = run(job, m.variant);
                    m.flaky               = !rerun.timed_out && difference(base, rerun).empty();
                }
                job.keep = true;
                std::string cmd = shell_word(self_exe);
                for (const std::string& arg : child_args(job, out.mismatches.front().variant)) {
                    cmd += " " + shell_word(arg);
                }
                out.replay = std::move(cmd);
            }
        }
        out.seconds = stats_.seconds - before;
        return out;
    }

    std::string message(const Outcome& o) const {
        std::string m = "config matrix: the design behaves differently under " + std::to_string(o.mismatches.size()) +
                        " of " + std::to_string(o.variants) + " variants than under the default configuration";
        for (const Mismatch& mm : o.mismatches) {
            m += "\n  " + mm.variant + ": " + mm.what;
            if (mm.flaky) {
                m += " (not reproducible: a second run agreed with the default)";
            }
        }
        m += "\n  replay: " + o.replay;
        m += "\n  A race the LRM leaves open (4.7), or a bug not fixed yet, is listed in tests/config-matrix-known.txt";
        return m;
    }

    void log(std::string_view outcome, std::string_view what, const Outcome& o) const {
#if !defined(_WIN32)
        if (options_.log.empty()) {
            return;
        }
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.2f", o.seconds);
        std::string line(outcome);
        line += '\t' + test_ + '\t' + std::string(what) + '\t' + seconds + '\n';
        std::replace(line.begin(), line.end() - 1, '\n', ' ');
        const int fd = open(options_.log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            // One write per line: lines of the suite's processes interleave whole.
            [[maybe_unused]] const ssize_t n = write(fd, line.data(), line.size());
            close(fd);
        }
#else
        (void)outcome;
        (void)what;
        (void)o;
#endif
    }

    void report(const Outcome& o, bool repeat) {
        if (!o.skipped.empty()) {
            ++stats_.skipped[o.skipped];
            log("skip", o.skipped, o);
            return;
        }
        ++(repeat ? stats_.repeated : stats_.checked);
        if (o.mismatches.empty()) {
            log(repeat ? "repeat" : "ok", {}, o);
            return;
        }
        std::string names;
        for (const Mismatch& m : o.mismatches) {
            names += (names.empty() ? "" : ",") + m.variant;
        }
        const auto it = known_.find(test_);
        if (it != known_.end()) {
            differed_.insert(test_);
            ++stats_.known[it->second.category];
            log("known-" + it->second.category, names, o);
            return;
        }
        ++stats_.unlisted;
        log("mismatch", names, o);
        throw std::runtime_error(message(o));
    }

    Options                                options_;
    std::map<std::string, KnownEntry>      known_;
    std::unordered_map<std::string, Outcome> cache_;
    std::set<std::string>                  differed_;
    std::vector<std::string>               stale_;
    std::string                            test_;
    Stats                                  stats_;
};

// ---- The suite's own instance ----

inline std::unique_ptr<Suite>& installed() {
    static std::unique_ptr<Suite> suite;
    return suite;
}

inline DesignCheck hook() {
    return [](const LowerResult& result, const LowerInputs& inputs) { installed()->check(result, inputs); };
}

inline std::string known_file() {
    return std::string(CLOTHO_TEST_DIR) + "/config-matrix-known.txt";
}

// Turn the mode on: read the known list and hook every lowering that follows.
// Throws on a malformed known list or environment variable.
inline void install() {
#if defined(_WIN32)
    throw std::runtime_error("CLOTHO_CONFIG_MATRIX needs POSIX process control and does not run on Windows");
#endif
    Options options;
    if (const char* limit = std::getenv("CLOTHO_CONFIG_MATRIX_TIMEOUT"); limit != nullptr && *limit != '\0') {
        char*        end     = nullptr;
        const double seconds = std::strtod(limit, &end);
        if (*end != '\0' || !(seconds > 0)) {
            throw std::runtime_error("CLOTHO_CONFIG_MATRIX_TIMEOUT must be a number of seconds, not '" + std::string(limit) + "'");
        }
        options.limit = std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
    }
    if (const char* log = std::getenv("CLOTHO_CONFIG_MATRIX_LOG"); log != nullptr) {
        options.log = log;
    }
    const std::string                       path = known_file();
    std::ifstream                           in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("config matrix: cannot read " + path);
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    installed() = std::make_unique<Suite>(std::move(options), parse_known(text, path));
    set_design_check(hook());
}

// Pauses the suite's hook for a scope. A test of the check lowers designs the
// suite itself must not run under the variants, because they differ on purpose.
class Paused {
public:
    Paused() { set_design_check({}); }
    ~Paused() {
        if (installed()) {
            set_design_check(hook());
        }
    }
    Paused(const Paused&)            = delete;
    Paused& operator=(const Paused&) = delete;
};

inline void begin_test(const char* name) {
    if (installed()) {
        installed()->begin_test(name);
    }
}

inline void end_test(bool success) {
    if (installed()) {
        installed()->end_test(success);
    }
}

// Print the mode's summary and return the status the run must end with: non-zero
// when a known entry is out of date.
inline int finish() {
    const Suite* suite = installed().get();
    if (suite == nullptr) {
        return 0;
    }
    const Stats& s = suite->stats();
    std::fprintf(stderr, "config matrix: %llu designs run under %zu variants (%llu more were repeats), %.0f s in children\n",
                 static_cast<unsigned long long>(s.checked), std::size(kVariants) - 1,
                 static_cast<unsigned long long>(s.repeated), s.seconds);
    for (const auto& [why, n] : s.skipped) {
        std::fprintf(stderr, "config matrix: %llu designs skipped: %s\n", static_cast<unsigned long long>(n), why.c_str());
    }
    for (const auto& [category, n] : s.known) {
        std::fprintf(stderr, "config matrix: %llu designs differ and are listed as %s\n", static_cast<unsigned long long>(n),
                     category.c_str());
    }
    if (s.unlisted != 0) {
        std::fprintf(stderr, "config matrix: %llu designs differ and are not listed\n", static_cast<unsigned long long>(s.unlisted));
    }
    for (const std::string& name : suite->stale()) {
        std::fprintf(stderr,
                     "config matrix: tests/config-matrix-known.txt line %zu lists \"%s\", which no longer differs under any "
                     "variant: delete the entry\n",
                     suite->known().at(name).line, name.c_str());
    }
    return suite->stale().empty() ? 0 : 1;
}

} // namespace clotho::test::config_matrix
