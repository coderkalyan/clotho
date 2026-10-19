// The debug adapter, driven as a real process over pipes.
//
// Nothing here links the adapter. The protocol is the contract -- byte framing,
// `seq` monotonicity, response identity, which capabilities are claimed, and
// what the design's output and exit status come back as -- and every one of
// those is only observable from outside the process.
//
// The session is event-driven in a way the language server's is not: a launch
// produces `process`, an unbounded run of `output`, `exited` and `terminated`,
// none of them solicited. So the harness is deadline-driven rather than
// bounded by a message count, which is what the LSP suite's `response(id)`
// does and what would give out here on the first chatty testbench.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "Subprocess.h"

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

// NOTE: Named rather than anonymous, for the reason in src/lsp/Server.cpp: glaze
// reflects these types, which clang rejects for a type without linkage.
namespace dap_test {

constexpr glz::opts kRead{ .error_on_unknown_keys = false };

// One struct for both directions: a response and an event differ by `type`,
// and a test that has to know which one it asked for before it can parse it
// cannot assert on an unsolicited message at all. `body` and `arguments` own
// their bytes, because the buffer they came from is reused.
struct Message {
    int64_t       seq = 0;
    std::string   type;
    std::string   command;
    std::string   event;
    int64_t       request_seq = 0;
    bool          success     = false;
    std::string   message;
    glz::raw_json body;
};

// glaze reflects over a type's members by name, and a type declared inside a
// function has no linkage for it to reach -- so the payload types tests parse
// live out here.
struct OutputBody {
    std::string category;
    std::string output;
};

std::string frame(std::string_view body) {
    return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

} // namespace dap_test

using namespace dap_test;

#if !defined(_WIN32)

namespace {

// posix_spawn rather than the shell: the test needs both ends of the child's
// stdio. Not fork + exec: a forked child of the multithreaded test runner runs
// inherited fork handlers before it can exec, which crashed it on macOS.
class Session {
public:
    Session() {
        int to_child[2]   = { -1, -1 };
        int from_child[2] = { -1, -1 };
        REQUIRE(::pipe(to_child) == 0);
        REQUIRE(::pipe(from_child) == 0);

        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        ::posix_spawn_file_actions_addclose(&actions, to_child[0]);
        ::posix_spawn_file_actions_addclose(&actions, to_child[1]);
        ::posix_spawn_file_actions_addclose(&actions, from_child[0]);
        ::posix_spawn_file_actions_addclose(&actions, from_child[1]);
        char      arg0[] = "clotho";
        char      arg1[] = "dap";
        char*     argv[] = { arg0, arg1, nullptr };
        const int rc     = ::posix_spawn(&pid_, CLOTHO_EXE, &actions, nullptr, argv, environ);
        ::posix_spawn_file_actions_destroy(&actions);
        REQUIRE(rc == 0);
        ::close(to_child[0]);
        ::close(from_child[1]);
        in_  = to_child[1];
        out_ = from_child[0];
    }

    ~Session() {
        if (in_ >= 0) {
            ::close(in_);
        }
        if (out_ >= 0) {
            ::close(out_);
        }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // Sends a request and returns the sequence number it was given, so the
    // caller can match the response without tracking the counter itself.
    int64_t request(std::string_view command, std::string_view arguments = "{}") {
        const int64_t seq = ++client_seq_;
        write(frame(
          std::format(R"({{"seq":{},"type":"request","command":"{}","arguments":{}}})", seq, command, arguments)));
        return seq;
    }

    void write(std::string_view text) {
        size_t off = 0;
        while (off < text.size()) {
            const auto n = ::write(in_, text.data() + off, text.size() - off);
            REQUIRE(n > 0);
            off += static_cast<size_t>(n);
        }
    }

    // Next message, or nullopt if none arrives within `timeout`.
    std::optional<Message> recv(std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (std::optional<Message> message = take()) {
                return message;
            }
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                return std::nullopt;
            }
            pollfd    fd{ .fd = out_, .events = POLLIN, .revents = 0 };
            const int ready =
              ::poll(&fd, 1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count()));
            if (ready <= 0) {
                return std::nullopt;
            }
            char       chunk[8192];
            const auto n = ::read(out_, chunk, sizeof chunk);
            if (n <= 0) {
                return std::nullopt;
            }
            buffer_.append(chunk, static_cast<size_t>(n));
        }
    }

    // The response to `seq`, keeping every event seen on the way so a test can
    // assert on both. Deadline-driven: a design that prints a thousand lines
    // would exhaust any fixed message budget.
    std::optional<Message> response(int64_t seq, std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                return std::nullopt;
            }
            std::optional<Message> message = recv(std::chrono::duration_cast<std::chrono::milliseconds>(left));
            if (!message.has_value()) {
                return std::nullopt;
            }
            if (message->type == "response" && message->request_seq == seq) {
                return message;
            }
            seen_.push_back(*std::move(message));
        }
    }

    // The next event named `name`, from what has already been seen or from the
    // stream.
    std::optional<Message> await(std::string_view name, std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        for (size_t i = 0; i < seen_.size(); ++i) {
            if (seen_[i].type == "event" && seen_[i].event == name) {
                Message found = seen_[i];
                seen_.erase(seen_.begin() + static_cast<ptrdiff_t>(i));
                return found;
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                return std::nullopt;
            }
            std::optional<Message> message = recv(std::chrono::duration_cast<std::chrono::milliseconds>(left));
            if (!message.has_value()) {
                return std::nullopt;
            }
            if (message->type == "event" && message->event == name) {
                return message;
            }
            seen_.push_back(*std::move(message));
        }
    }

    // Everything the design wrote to `category`, in order, including whatever
    // was buffered while waiting for something else.
    std::string output_text(std::string_view category) const {
        std::string text;
        for (const Message& message : seen_) {
            if (message.type != "event" || message.event != "output") {
                continue;
            }
            OutputBody parsed;
            if (glz::read<kRead>(parsed, message.body.str)) {
                continue;
            }
            if (parsed.category == category) {
                text += parsed.output;
            }
        }
        return text;
    }

    // Drives the handshake through to the point where a launch is legal.
    void initialize() {
        const int64_t                seq = request("initialize", R"({"clientID":"clotho-test","adapterID":"clotho"})");
        const std::optional<Message> reply = response(seq);
        REQUIRE(reply.has_value());
        REQUIRE(reply->success);
        REQUIRE(await("initialized").has_value());
    }

    int wait_for_exit() {
        ::close(in_);
        in_        = -1;
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

private:
    std::optional<Message> take() {
        const size_t end = buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            return std::nullopt;
        }
        const size_t mark = buffer_.find("Content-Length: ");
        REQUIRE(mark != std::string::npos);
        const size_t length = std::stoul(buffer_.substr(mark + 16));
        if (buffer_.size() < end + 4 + length) {
            return std::nullopt;
        }

        Message                message;
        const std::string_view body(buffer_.data() + end + 4, length);
        REQUIRE_MESSAGE(!glz::read<kRead>(message, body), "unreadable message: ", body);
        buffer_.erase(0, end + 4 + length);
        return message;
    }

    pid_t                pid_        = -1;
    int                  in_         = -1;
    int                  out_        = -1;
    int64_t              client_seq_ = 0;
    std::string          buffer_;
    std::vector<Message> seen_;
};

// A design on disk, because the adapter launches `clotho run` against real
// paths rather than against a buffer the client sent.
class Design {
public:
    explicit Design(std::string_view source)
      : path_(std::filesystem::temp_directory_path() / (clotho::test::detail::unique_stem() + ".sv")) {
        std::ofstream out(path_, std::ios::binary);
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
    }

    ~Design() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    Design(const Design&)            = delete;
    Design& operator=(const Design&) = delete;

    // As a JSON array, ready to be the launch request's `args`.
    std::string args() const {
        std::string escaped;
        REQUIRE(!glz::write_json(path_.string(), escaped));
        return std::format("[{}]", escaped);
    }

private:
    std::filesystem::path path_;
};

constexpr std::string_view kHello = R"(
module top;
  initial begin
    $display("hello from the design");
    $finish;
  end
endmodule
)";

} // namespace

TEST_CASE("dap initialize reports only the capabilities it has") {
    Session       session;
    const int64_t seq   = session.request("initialize", R"({"clientID":"clotho-test"})");
    const auto    reply = session.response(seq);

    REQUIRE(reply.has_value());
    CHECK(reply->type == "response");
    CHECK(reply->command == "initialize");
    CHECK(reply->success);
    // Claimed, because they are implemented.
    CHECK(reply->body.str.find("\"supportsConfigurationDoneRequest\":true") != std::string::npos);
    CHECK(reply->body.str.find("\"supportsTerminateRequest\":true") != std::string::npos);
    // Absent, because they are not. A capability reported false is the same as
    // one omitted, but a capability reported TRUE that does not work is how a
    // client ends up showing a Pause button that does nothing.
    CHECK(reply->body.str.find("supportsStepBack") == std::string::npos);
    CHECK(reply->body.str.find("supportsDataBreakpoints") == std::string::npos);
    CHECK(reply->body.str.find("exceptionBreakpointFilters") == std::string::npos);
}

TEST_CASE("dap sends initialized after the initialize response, not before") {
    Session       session;
    const int64_t seq = session.request("initialize", "{}");

    // The first message must be the response: a client that is still waiting
    // for it will not have started configuring, and an `initialized` event
    // that arrives first is one it may drop.
    const auto first = session.recv();
    REQUIRE(first.has_value());
    CHECK(first->type == "response");
    CHECK(first->request_seq == seq);

    const auto second = session.recv();
    REQUIRE(second.has_value());
    CHECK(second->type == "event");
    CHECK(second->event == "initialized");
}

TEST_CASE("dap refuses everything before initialize") {
    Session       session;
    const int64_t seq   = session.request("threads");
    const auto    reply = session.response(seq);

    REQUIRE(reply.has_value());
    CHECK_FALSE(reply->success);
    CHECK(reply->message.find("not been initialized") != std::string::npos);
}

TEST_CASE("dap stamps a monotonic seq on every message") {
    Session session;
    session.initialize();
    const int64_t seq = session.request("configurationDone");
    REQUIRE(session.response(seq).has_value());

    // Three messages so far in this direction: the initialize response, the
    // initialized event, the configurationDone response. Their seqs must be
    // 1, 2, 3 -- the protocol requires the count, and a client that matches a
    // cancel against it requires the order.
    Session       fresh;
    const int64_t first = fresh.request("initialize", "{}");
    const auto    a     = fresh.recv();
    const auto    b     = fresh.recv();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->seq == 1);
    CHECK(b->seq == 2);
    CHECK(a->request_seq == first);
}

TEST_CASE("dap runs a design and reports its output and exit status") {
    const Design design(kHello);
    Session      session;
    session.initialize();

    const int64_t launch = session.request("launch", std::format(R"({{"args":{},"noDebug":false}})", design.args()));
    REQUIRE(session.response(launch).has_value());
    const int64_t configured = session.request("configurationDone");
    REQUIRE(session.response(configured).has_value());

    // The design is a child of the adapter, and DAP has an event that says so.
    const auto process = session.await("process");
    REQUIRE(process.has_value());
    CHECK(process->body.str.find("\"startMethod\":\"launch\"") != std::string::npos);

    const auto exited = session.await("exited");
    REQUIRE(exited.has_value());
    CHECK(exited->body.str.find("\"exitCode\":0") != std::string::npos);
    REQUIRE(session.await("terminated").has_value());

    // $display goes to the design's stdout, which is a pipe the adapter owns,
    // so it arrives as an event rather than on the protocol stream. That
    // separation is the whole reason the design runs out of process.
    CHECK(session.output_text("stdout").find("hello from the design") != std::string::npos);
}

// The launch `cwd` is where the design resolves relative paths, so a file that
// exists only there must be found.
TEST_CASE("dap runs the design in the launch cwd") {
    const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / clotho::test::detail::unique_stem();
    std::filesystem::create_directory(dir);
    std::ofstream(dir / "dap_cwd_probe.txt") << "probe\n";
    const Design design(R"(
module top;
  integer fd;
  initial begin
    fd = $fopen("dap_cwd_probe.txt", "r");
    if (fd != 0) $display("probe found");
    else $display("probe missing");
    $finish;
  end
endmodule
)");
    std::string cwd;
    REQUIRE(!glz::write_json(dir.string(), cwd));
    Session session;
    session.initialize();

    const int64_t launch =
      session.request("launch", std::format(R"({{"args":{},"cwd":{},"noDebug":false}})", design.args(), cwd));
    REQUIRE(session.response(launch).has_value());
    const int64_t configured = session.request("configurationDone");
    REQUIRE(session.response(configured).has_value());
    REQUIRE(session.await("terminated").has_value());
    CHECK(session.output_text("stdout").find("probe found") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("dap reports a design that fails to compile rather than pretending it ran") {
    const Design design("module top; this is not systemverilog endmodule\n");
    Session      session;
    session.initialize();

    const int64_t launch = session.request("launch", std::format(R"({{"args":{}}})", design.args()));
    REQUIRE(session.response(launch).has_value());
    REQUIRE(session.response(session.request("configurationDone")).has_value());

    const auto exited = session.await("exited");
    REQUIRE(exited.has_value());
    CHECK(exited->body.str.find("\"exitCode\":0") == std::string::npos);
    CHECK_FALSE(session.output_text("stderr").empty());
}

TEST_CASE("dap refuses a launch it cannot honour, by name") {
    Session session;
    session.initialize();

    SUBCASE("no args") {
        const auto reply = session.response(session.request("launch", "{}"));
        REQUIRE(reply.has_value());
        CHECK_FALSE(reply->success);
        CHECK(reply->message.find("args") != std::string::npos);
    }
    SUBCASE("stopOnEntry needs a stop point") {
        const auto reply = session.response(session.request("launch", R"({"args":["a.sv"],"stopOnEntry":true})"));
        REQUIRE(reply.has_value());
        CHECK_FALSE(reply->success);
        CHECK(reply->message.find("stopOnEntry") != std::string::npos);
    }
}

TEST_CASE("dap refuses an unimplemented request by name and says which stage has it") {
    Session session;
    session.initialize();

    // Each of these has a home in docs/design/dap.md 6, and the point of the
    // test is that the refusal says so: "not built yet" and "broken" are
    // different answers and the user is entitled to know which one they got.
    struct Expect {
        std::string_view command;
        std::string_view mentions;
    };
    const Expect expects[] = {
        { "stackTrace", "D2" },   { "scopes", "D2" },     { "variables", "D2" },          { "continue", "D1" },
        { "next", "D3" },         { "pause", "D3" },      { "setDataBreakpoints", "D4" }, { "evaluate", "D5" },
        { "restart", "restart" }, { "attach", "attach" },
    };
    for (const Expect& expect : expects) {
        const auto reply = session.response(session.request(expect.command));
        REQUIRE_MESSAGE(reply.has_value(), "no response to ", expect.command);
        CHECK_FALSE(reply->success);
        CHECK_MESSAGE(reply->message.find(expect.mentions) != std::string::npos, expect.command, ": ", reply->message);
    }
}

TEST_CASE("dap answers an unknown command instead of going quiet") {
    Session session;
    session.initialize();
    const auto reply = session.response(session.request("noSuchCommand"));

    REQUIRE(reply.has_value());
    CHECK_FALSE(reply->success);
    CHECK(reply->command == "noSuchCommand");
    CHECK(reply->message.find("not a command") != std::string::npos);
}

TEST_CASE("dap marks source breakpoints inert rather than failing the request") {
    Session session;
    session.initialize();

    // Per breakpoint, not per file: the client keeps the markers and shows the
    // reason on each, instead of raising one error about the whole document.
    const auto reply = session.response(
      session.request("setBreakpoints", R"({"source":{"path":"/tmp/top.sv"},"breakpoints":[{"line":3},{"line":7}]})"));

    REQUIRE(reply.has_value());
    CHECK(reply->success);
    CHECK(reply->body.str.find("\"verified\":false") != std::string::npos);
    CHECK(reply->body.str.find("D3") != std::string::npos);
}

TEST_CASE("dap exits cleanly on disconnect and kills the design with it") {
    // A design that never ends on its own: the adapter must not wait for it.
    const Design design(R"(
module top;
  reg clk = 0;
  always #1 clk = ~clk;
  initial $display("running");
endmodule
)");
    Session      session;
    session.initialize();
    REQUIRE(session.response(session.request("launch", std::format(R"({{"args":{}}})", design.args()))).has_value());
    REQUIRE(session.response(session.request("configurationDone")).has_value());
    REQUIRE(session.await("process").has_value());

    REQUIRE(session.response(session.request("disconnect", R"({"terminateDebuggee":true})")).has_value());
    CHECK(session.wait_for_exit() == 0);
}

TEST_CASE("dap exits nonzero when the client vanishes") {
    Session session;
    session.initialize();
    // No disconnect: closing stdin is the client dying, and the adapter is
    // expected to notice and say it was not a clean end.
    CHECK(session.wait_for_exit() == 1);
}

#endif // !_WIN32

TEST_CASE("dap takes no arguments on the command line") {
    // The design arrives in the launch request, so an argument here is a
    // mistake worth reporting rather than a flag to ignore.
    const std::string cmd = std::format("\"{}\" dap --top foo < /dev/null > /dev/null 2>&1", CLOTHO_EXE);
#if defined(_WIN32)
    (void)cmd;
#else
    const int status = std::system(cmd.c_str());
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 2);
#endif
}
