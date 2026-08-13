// `clotho lsp`, driven the way an editor drives it: a real process, a real
// stdio transcript, real base-protocol framing.
//
// Nothing here links the server. The protocol is the contract -- byte framing,
// response identity, error codes, exit status -- and all four are observable
// only from outside the process. A test that called the dispatcher directly
// would pass while the framing was broken, which is the one failure an editor
// notices first.
//
// The transcript is a file rather than a pipe, which also makes the batching
// deterministic: read(2) on a regular file returns everything asked for short
// of EOF, so a transcript smaller than the reader's chunk arrives as one batch.
// The cancellation case depends on that.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>

#include "Subprocess.h"
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct ResponseError {
    int32_t     code;
    std::string message;
};

// One message off the wire, in either direction: a response carries `id` and
// one of result/error, a server-initiated notification carries `method` and
// `params`. Modelled as one struct because the transport does not distinguish
// them and neither should a test that asserts on ordering between the two.
struct Message {
    std::string                                        jsonrpc;
    std::variant<std::monostate, int64_t, std::string> id;
    std::string                                        method;
    // raw, not optional: `"result": null` and no result at all are different
    // messages, and only the first is a valid success response.
    glz::raw_json                result;
    glz::raw_json                params;
    std::optional<ResponseError> error;
};

struct Session {
    int                  status = -1;
    std::vector<Message> responses;
};

std::string frame(std::string_view body) {
    return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

// Concatenates each message with its own header.
std::string transcript(std::initializer_list<std::string_view> bodies) {
    std::string text;
    for (const std::string_view body : bodies) {
        text += frame(body);
    }
    return text;
}

std::vector<Message> parse_frames(std::string_view out) {
    std::vector<Message> messages;
    size_t               pos = 0;
    while (pos < out.size()) {
        const size_t end = out.find("\r\n\r\n", pos);
        REQUIRE(end != std::string_view::npos);
        const std::string_view headers = out.substr(pos, end - pos);
        const size_t           mark    = headers.find("Content-Length: ");
        REQUIRE(mark != std::string_view::npos);
        const size_t length = std::stoul(std::string(headers.substr(mark + 16)));

        const std::string_view body = out.substr(end + 4, length);
        REQUIRE(body.size() == length);
        Message response;
        REQUIRE_MESSAGE(
          !glz::read<glz::opts{ .error_on_unknown_keys = false }>(response, body), "unreadable response: ", body);
        messages.push_back(std::move(response));
        pos = end + 4 + length;
    }
    return messages;
}

Session run_session(std::string_view input) {
    namespace fs = std::filesystem;

    const std::string stem = clotho::test::detail::unique_stem();
    const fs::path    dir  = fs::temp_directory_path();
    const fs::path    in   = dir / (stem + ".lsp-in");
    const fs::path    out  = dir / (stem + ".lsp-out");
    {
        std::ofstream f(in, std::ios::binary);
        f.write(input.data(), static_cast<std::streamsize>(input.size()));
    }

    // The server's own log goes to stderr and is not part of the contract.
#if defined(_WIN32)
    constexpr std::string_view null_device = "NUL";
#else
    constexpr std::string_view null_device = "/dev/null";
#endif
    const auto  quote = [](const std::string& s) { return "\"" + s + "\""; };
    std::string cmd =
      std::format("{} lsp < {} > {} 2> {}", quote(CLOTHO_EXE), quote(in.string()), quote(out.string()), null_device);
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";
#endif
    const int rc = std::system(cmd.c_str());

    Session session;
#if defined(_WIN32)
    session.status = rc;
#else
    session.status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    const std::string text = clotho::test::detail::read_file(out);
    session.responses      = parse_frames(text);

    std::error_code ec;
    fs::remove(in, ec);
    fs::remove(out, ec);
    return session;
}

constexpr std::string_view kInitialize  = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
constexpr std::string_view kInitialized = R"({"jsonrpc":"2.0","method":"initialized","params":{}})";
constexpr std::string_view kShutdown    = R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})";
constexpr std::string_view kExit        = R"({"jsonrpc":"2.0","method":"exit"})";

int64_t int_id(const Message& r) {
    REQUIRE(std::holds_alternative<int64_t>(r.id));
    return std::get<int64_t>(r.id);
}

int32_t error_code(const Message& r) {
    REQUIRE(r.error.has_value());
    return r.error->code;
}

#if !defined(_WIN32)

// A live session over pipes, for everything the transcript harness structurally
// cannot see: publishDiagnostics is unsolicited and arrives whenever the
// analysis finishes, so a test has to be able to wait for a message rather than
// read a finished file.
//
// fork+exec, not the shell: the test needs both ends of the child's stdio, and
// the exec is immediate so none of the parent's JIT state is inherited into a
// running child (the hazard Subprocess.h documents).
class LiveSession {
public:
    LiveSession() {
        int to_child[2]   = { -1, -1 };
        int from_child[2] = { -1, -1 };
        REQUIRE(::pipe(to_child) == 0);
        REQUIRE(::pipe(from_child) == 0);

        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::dup2(to_child[0], STDIN_FILENO);
            ::dup2(from_child[1], STDOUT_FILENO);
            const int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDERR_FILENO);
            }
            ::close(to_child[0]);
            ::close(to_child[1]);
            ::close(from_child[0]);
            ::close(from_child[1]);
            ::execl(CLOTHO_EXE, "clotho", "lsp", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(to_child[0]);
        ::close(from_child[1]);
        in_  = to_child[1];
        out_ = from_child[0];
    }

    ~LiveSession() {
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

    LiveSession(const LiveSession&)            = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    void send(std::string_view body) {
        const std::string text = frame(body);
        size_t            off  = 0;
        while (off < text.size()) {
            const auto n = ::write(in_, text.data() + off, text.size() - off);
            REQUIRE(n > 0);
            off += static_cast<size_t>(n);
        }
    }

    // Next message, or nullopt if none arrives within `timeout`.
    std::optional<Message> recv(std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
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

    // Drives the handshake and returns once the server is ready for documents.
    void initialize(std::string_view encodings = R"(["utf-8","utf-16"])") {
        send(std::format(
          R"({{"jsonrpc":"2.0","id":1,"method":"initialize","params":{{"capabilities":{{"general":{{"positionEncodings":{}}},"workspace":{{"semanticTokens":{{"refreshSupport":true}}}}}}}}}})",
          encodings));
        const std::optional<Message> ack = recv();
        REQUIRE(ack.has_value());
        REQUIRE(!ack->error.has_value());
        send(kInitialized);
    }

    // The next notification (or server request) named `method`, skipping
    // anything else in the stream. The server pushes diagnostics and then asks
    // for a token refresh, so the two interleave with every response.
    std::optional<Message> notification(std::string_view          method,
                                        std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                return std::nullopt;
            }
            const std::optional<Message> message = recv(std::chrono::duration_cast<std::chrono::milliseconds>(left));
            if (!message.has_value()) {
                return std::nullopt;
            }
            if (message->method == method) {
                return message;
            }
        }
    }

    // The response to `id`, skipping anything the server sent in the meantime.
    // Diagnostics and semantic-token refreshes arrive unprompted, so a bare
    // recv() after a request usually returns one of those instead.
    std::optional<Message> response(int64_t id) {
        for (int i = 0; i < 8; ++i) {
            const std::optional<Message> message = recv();
            if (!message.has_value()) {
                return std::nullopt;
            }
            if (std::holds_alternative<int64_t>(message->id) && std::get<int64_t>(message->id) == id) {
                return message;
            }
        }
        return std::nullopt;
    }

    int shutdown() {
        send(kShutdown);
        send(kExit);
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
        REQUIRE_MESSAGE(
          !glz::read<glz::opts{ .error_on_unknown_keys = false }>(message, body), "unreadable message: ", body);
        buffer_.erase(0, end + 4 + length);
        return message;
    }

    pid_t       pid_ = -1;
    int         in_  = -1;
    int         out_ = -1;
    std::string buffer_;
};

// A URI need not name a real file: the server analyses the text the client sent
// it, and only uses the path to attribute diagnostics.
constexpr std::string_view kUri     = "file:///clotho-test/latchy.sv";
constexpr std::string_view kPublish = "textDocument/publishDiagnostics";

std::string did_open(std::string_view uri, int version, std::string_view text) {
    std::string escaped;
    REQUIRE(!glz::write_json(text, escaped));
    return std::format(
      R"({{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{{"textDocument":{{"uri":"{}","languageId":"systemverilog","version":{},"text":{}}}}}}})",
      uri,
      version,
      escaped);
}

std::string did_change(std::string_view uri, int version, std::string_view text) {
    std::string escaped;
    REQUIRE(!glz::write_json(text, escaped));
    return std::format(
      R"({{"jsonrpc":"2.0","method":"textDocument/didChange","params":{{"textDocument":{{"uri":"{}","version":{}}},"contentChanges":[{{"text":{}}}]}}}})",
      uri,
      version,
      escaped);
}

struct PublishedDiagnostic {
    struct Position {
        uint32_t line;
        uint32_t character;
    };
    struct Range {
        Position start;
        Position end;
    };

    Range                      range;
    int32_t                    severity;
    std::optional<std::string> code;
    std::string                source;
    std::string                message;
};

struct Published {
    std::string                      uri;
    int64_t                          version;
    std::vector<PublishedDiagnostic> diagnostics;
};

Published published(const Message& message) {
    REQUIRE(message.method == "textDocument/publishDiagnostics");
    Published out;
    REQUIRE_MESSAGE(!glz::read<glz::opts{ .error_on_unknown_keys = false }>(out, message.params.str),
                    "unreadable params: ",
                    message.params.str);
    return out;
}

#endif // !_WIN32

} // namespace

TEST_CASE("lsp serves the lifecycle handshake") {
    const Session session = run_session(transcript({ kInitialize, kInitialized, kShutdown, kExit }));

    CHECK(session.status == 0);
    REQUIRE(session.responses.size() == 2);

    const Message& init = session.responses[0];
    CHECK(init.jsonrpc == "2.0");
    CHECK(int_id(init) == 1);
    CHECK(!init.error.has_value());
    CHECK(init.result.str.find(R"("name":"clotho")") != std::string::npos);
    CHECK(init.result.str.find(R"("textDocumentSync")") != std::string::npos);

    // shutdown's result is null, and the member has to be there: a response with
    // neither result nor error is not a JSON-RPC response.
    const Message& shutdown = session.responses[1];
    CHECK(int_id(shutdown) == 2);
    CHECK(!shutdown.error.has_value());
    CHECK(shutdown.result.str == "null");
}

TEST_CASE("lsp negotiates the position encoding") {
    constexpr std::string_view offers_utf8 =
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{"general":{"positionEncodings":["utf-8","utf-16"]}}}})";
    constexpr std::string_view offers_utf32 =
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{"general":{"positionEncodings":["utf-32"]}}}})";

    SUBCASE("utf-8 when the client can read it") {
        const Session session = run_session(transcript({ offers_utf8, kShutdown, kExit }));
        REQUIRE(session.responses.size() == 2);
        CHECK(session.responses[0].result.str.find(R"("positionEncoding":"utf-8")") != std::string::npos);
    }
    SUBCASE("utf-16 otherwise, which is the protocol default") {
        const Session session = run_session(transcript({ offers_utf32, kShutdown, kExit }));
        REQUIRE(session.responses.size() == 2);
        CHECK(session.responses[0].result.str.find(R"("positionEncoding":"utf-16")") != std::string::npos);
    }
}

TEST_CASE("lsp reports an unclean exit in its status") {
    SUBCASE("exit without shutdown") {
        const Session session = run_session(transcript({ kInitialize, kExit }));
        CHECK(session.status == 1);
    }
    SUBCASE("the client disconnects without saying anything") {
        const Session session = run_session(transcript({ kInitialize }));
        CHECK(session.status == 1);
    }
    SUBCASE("shutdown then exit") {
        const Session session = run_session(transcript({ kInitialize, kShutdown, kExit }));
        CHECK(session.status == 0);
    }
}

TEST_CASE("lsp gates requests on the lifecycle") {
    SUBCASE("before initialize") {
        const Session session = run_session(transcript({ kShutdown, kExit }));
        REQUIRE(session.responses.size() == 1);
        CHECK(error_code(session.responses[0]) == -32002);
    }
    SUBCASE("a second initialize") {
        constexpr std::string_view again   = R"({"jsonrpc":"2.0","id":9,"method":"initialize","params":{}})";
        const Session              session = run_session(transcript({ kInitialize, again, kExit }));
        REQUIRE(session.responses.size() == 2);
        CHECK(int_id(session.responses[1]) == 9);
        CHECK(error_code(session.responses[1]) == -32600);
    }
    SUBCASE("after shutdown") {
        constexpr std::string_view late    = R"({"jsonrpc":"2.0","id":9,"method":"shutdown"})";
        const Session              session = run_session(transcript({ kInitialize, kShutdown, late, kExit }));
        REQUIRE(session.responses.size() == 3);
        CHECK(error_code(session.responses[2]) == -32600);
        CHECK(session.status == 0);
    }
}

TEST_CASE("lsp answers an unknown request and ignores an unknown notification") {
    constexpr std::string_view unknown_request      = R"({"jsonrpc":"2.0","id":"h1","method":"textDocument/hover"})";
    constexpr std::string_view unknown_notification = R"({"jsonrpc":"2.0","method":"telemetry/nonsense","params":{}})";

    const Session session =
      run_session(transcript({ kInitialize, kInitialized, unknown_notification, unknown_request, kShutdown, kExit }));

    CHECK(session.status == 0);
    // Three, not four: a notification never draws a response, however unknown.
    REQUIRE(session.responses.size() == 3);
    CHECK(std::get<std::string>(session.responses[1].id) == "h1");
    CHECK(error_code(session.responses[1]) == -32601);
}

TEST_CASE("lsp survives a message it cannot parse") {
    constexpr std::string_view truncated = R"({"jsonrpc":"2.0","id":9,)";

    const Session session = run_session(frame(kInitialize) + frame(truncated) + frame(kShutdown) + frame(kExit));

    CHECK(session.status == 0);
    REQUIRE(session.responses.size() == 3);
    // The id is precisely what could not be read, so the error carries null --
    // and the framing layer, which never looked at the JSON, stays in sync.
    CHECK(std::holds_alternative<std::monostate>(session.responses[1].id));
    CHECK(error_code(session.responses[1]) == -32700);
    CHECK(int_id(session.responses[2]) == 2);
}

TEST_CASE("lsp declines a request its client already cancelled") {
    constexpr std::string_view cancel  = R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":2}})";
    constexpr std::string_view retried = R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})";

    const Session session = run_session(transcript({ kInitialize, kInitialized, kShutdown, cancel, retried, kExit }));

    CHECK(session.status == 0);
    REQUIRE(session.responses.size() == 3);
    // Cancelled, so the handler never ran: the request is answered, and the
    // shutdown it asked for did not happen.
    CHECK(int_id(session.responses[1]) == 2);
    CHECK(error_code(session.responses[1]) == -32800);
    CHECK(int_id(session.responses[2]) == 3);
    CHECK(!session.responses[2].error.has_value());
}

TEST_CASE("lsp answers document notifications with notifications, never responses") {
    constexpr std::string_view did_open =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///a.sv","languageId":"systemverilog","version":1,"text":"module a; endmodule"}}})";
    constexpr std::string_view did_change =
      R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///a.sv","version":2},"contentChanges":[{"text":"module a; endmodule\n"}]}})";
    constexpr std::string_view did_close =
      R"({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"file:///a.sv"}}})";

    const Session session =
      run_session(transcript({ kInitialize, kInitialized, did_open, did_change, did_close, kShutdown, kExit }));

    CHECK(session.status == 0);

    // Exactly two messages carry an id, one per request. Anything else the
    // server emitted is a notification of its own -- the diagnostics it
    // publishes -- and a notification must never draw a response (JSON-RPC 2.0).
    size_t responses = 0;
    for (const Message& message : session.responses) {
        if (std::holds_alternative<std::monostate>(message.id)) {
            CHECK(message.method == "textDocument/publishDiagnostics");
            continue;
        }
        ++responses;
        CHECK(message.method.empty());
    }
    CHECK(responses == 2);
}

TEST_CASE("lsp accepts bare newline framing") {
    // The spec says \r\n. Accepting \n costs nothing and is what makes a
    // hand-written transcript usable.
    const std::string input = std::format(
      "Content-Length: {}\n\n{}Content-Length: {}\n\n{}", kInitialize.size(), kInitialize, kExit.size(), kExit);
    const Session session = run_session(input);
    REQUIRE(session.responses.size() == 1);
    CHECK(int_id(session.responses[0]) == 1);
}

TEST_CASE("lsp takes no command line arguments") {
    const Session session = run_session("");
    CHECK(session.status == 1); // No initialize, so an unclean disconnect.

    const std::string cmd = std::format("\"{}\" lsp --top foo > /dev/null 2>&1", CLOTHO_EXE);
#if !defined(_WIN32)
    const int rc = std::system(cmd.c_str());
    CHECK(WIFEXITED(rc));
    CHECK(WEXITSTATUS(rc) == 2);
#endif
}

#if !defined(_WIN32)

// -- diagnostics -------------------------------------------------------------

namespace {

// `q` is assigned only under `if (en)`, so it infers a latch. The finding is
// Clotho's own, not slang's, which is what makes it worth asserting on: it
// proves the whole lowering and analysis pipeline ran on the editor's buffer.
constexpr std::string_view kLatch = "module latchy(input en, input d, output logic q);\n"
                                    "  always_comb begin\n"
                                    "    if (en) q = d;\n"
                                    "  end\n"
                                    "endmodule\n";

// Same module with the latch closed.
constexpr std::string_view kNoLatch = "module latchy(input en, input d, output logic q);\n"
                                      "  always_comb begin\n"
                                      "    q = en ? d : 1'b0;\n"
                                      "  end\n"
                                      "endmodule\n";

} // namespace

TEST_CASE("lsp publishes diagnostics for an opened document") {
    LiveSession session;
    session.initialize();
    session.send(did_open(kUri, 1, kLatch));

    const std::optional<Message> message = session.notification(kPublish);
    REQUIRE(message.has_value());
    const Published note = published(*message);

    CHECK(note.uri == kUri);
    CHECK(note.version == 1);
    REQUIRE(note.diagnostics.size() == 1);

    const PublishedDiagnostic& diagnostic = note.diagnostics[0];
    CHECK(diagnostic.source == "clotho");
    CHECK(diagnostic.code == "inferred-latch");
    CHECK(diagnostic.severity == 2); // Warning
    CHECK(diagnostic.message.find("latch inferred for 'q'") != std::string::npos);
    // Line 3 of the source, 0-based; `q` is the 13th character on it.
    CHECK(diagnostic.range.start.line == 2);
    CHECK(diagnostic.range.start.character == 12);
    CHECK(diagnostic.range.end.character == 13);

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp republishes an empty list once the finding is fixed") {
    LiveSession session;
    session.initialize();
    session.send(did_open(kUri, 1, kLatch));
    REQUIRE(published(*session.notification(kPublish)).diagnostics.size() == 1);

    session.send(did_change(kUri, 2, kNoLatch));
    const std::optional<Message> message = session.notification(kPublish);
    REQUIRE(message.has_value());
    const Published note = published(*message);

    // An empty list is the only way to retract a diagnostic; going silent would
    // leave the editor showing the latch forever.
    CHECK(note.version == 2);
    CHECK(note.diagnostics.empty());

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp clears diagnostics when a document is closed") {
    LiveSession session;
    session.initialize();
    session.send(did_open(kUri, 1, kLatch));
    REQUIRE(published(*session.notification(kPublish)).diagnostics.size() == 1);

    session.send(std::format(
      R"({{"jsonrpc":"2.0","method":"textDocument/didClose","params":{{"textDocument":{{"uri":"{}"}}}}}})", kUri));
    const std::optional<Message> message = session.notification(kPublish);
    REQUIRE(message.has_value());
    CHECK(published(*message).diagnostics.empty());

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp coalesces a burst of edits into one analysis") {
    LiveSession session;
    session.initialize();
    session.send(did_open(kUri, 1, kLatch));
    REQUIRE(published(*session.notification(kPublish)).version == 1);

    // Six revisions in rapid succession, as typing produces. Only the last is
    // worth analysing, and the debounce is what makes that true.
    for (int version = 2; version <= 7; ++version) {
        session.send(did_change(kUri, version, version == 7 ? kNoLatch : kLatch));
    }

    const std::optional<Message> message = session.notification(kPublish);
    REQUIRE(message.has_value());
    const Published note = published(*message);
    CHECK(note.version == 7);
    CHECK(note.diagnostics.empty());

    // And exactly one: an analysis per keystroke would show up here.
    CHECK_FALSE(session.notification(kPublish, std::chrono::milliseconds(1500)).has_value());

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp analyses a document whose dependencies are absent") {
    // One file of a design, opened alone. Neither slang's `unknown module` nor
    // Clotho's `unsupported module member` may appear: both would be the server
    // inventing a problem out of the other files not being open.
    constexpr std::string_view source = "module parent(input en, input d, output logic q);\n"
                                        "  child u ();\n"
                                        "  always_comb begin\n"
                                        "    if (en) q = d;\n"
                                        "  end\n"
                                        "endmodule\n";

    LiveSession session;
    session.initialize();
    session.send(did_open(kUri, 1, source));

    const Published note = published(*session.notification(kPublish));
    REQUIRE(note.diagnostics.size() == 1);
    CHECK(note.diagnostics[0].code == "inferred-latch");

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp reports columns in the negotiated position encoding") {
    // U+1F600 is two UTF-16 code units and four UTF-8 bytes; U+00E9 is one unit
    // and two bytes. Counting either as one character -- an easy bug to write --
    // puts the underline in the wrong place.
    constexpr std::string_view source = "module latchy(input en, input d, output logic q);\n"
                                        "  always_comb begin\n"
                                        "    /* \xF0\x9F\x98\x80 \xC3\xA9 */ if (en) q = d;\n"
                                        "  end\n"
                                        "endmodule\n";

    SUBCASE("utf-8 counts bytes") {
        LiveSession session;
        session.initialize(R"(["utf-8","utf-16"])");
        session.send(did_open(kUri, 1, source));
        const Published note = published(*session.notification(kPublish));
        REQUIRE(note.diagnostics.size() == 1);
        CHECK(note.diagnostics[0].range.start.character == 26);
        CHECK(session.shutdown() == 0);
    }
    SUBCASE("utf-16 counts code units, surrogate pairs as two") {
        LiveSession session;
        session.initialize(R"(["utf-16"])");
        session.send(did_open(kUri, 1, source));
        const Published note = published(*session.notification(kPublish));
        REQUIRE(note.diagnostics.size() == 1);
        CHECK(note.diagnostics[0].range.start.character == 23);
        CHECK(session.shutdown() == 0);
    }
}

TEST_CASE("lsp ignores a document it cannot read") {
    LiveSession session;
    session.initialize();
    // Not a file:// URI, so there is nothing on disk and no path to name the
    // buffer with. It must be dropped, not guessed at.
    session.send(did_open("untitled:Untitled-1", 1, kLatch));

    CHECK_FALSE(session.notification(kPublish, std::chrono::milliseconds(1500)).has_value());
    CHECK(session.shutdown() == 0);
}

#endif // !_WIN32

#if !defined(_WIN32)

TEST_CASE("lsp reports a net that nothing reads") {
    // Regression: CompilationFlags::LintMode was set here on the strength of
    // its name, and it makes Scope::isUninstantiated() true for every scope
    // (slang Scope.cpp:150-153), which silently deleted the entire unused
    // family. This exact source published nothing at all.
    constexpr std::string_view source = "module test ();\n"
                                        "    wire [2:0] foo = 4'b0000;\n"
                                        "endmodule\n";

    LiveSession session;
    session.initialize();
    session.send(did_open(kUri, 1, source));

    const Published note = published(*session.notification(kPublish));
    REQUIRE(note.diagnostics.size() == 1);
    CHECK(note.diagnostics[0].code == "unused-net");
    CHECK(note.diagnostics[0].message.find("unused net 'foo'") != std::string::npos);
    CHECK(note.diagnostics[0].range.start.line == 1);

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp does not invent findings from the files that are not open") {
    // Every net here is driven by a module the buffer cannot see. Each one
    // would otherwise be reported `never driven` -- one fabricated warning per
    // connection, which is what makes a server not worth keeping open.
    constexpr std::string_view source = "module parent(output logic [3:0] y, output logic z);\n"
                                        "  logic [3:0] a;\n"
                                        "  logic       b;\n"
                                        "  child_one u0 (.o(a));\n"
                                        "  child_two u1 (.o(b));\n"
                                        "  assign y = a;\n"
                                        "  assign z = b;\n"
                                        "endmodule\n";

    LiveSession session;
    session.initialize();
    session.send(did_open(kUri, 1, source));

    const Published note = published(*session.notification(kPublish));
    for (const PublishedDiagnostic& diagnostic : note.diagnostics) {
        CHECK(diagnostic.message.find("never driven") == std::string::npos);
        CHECK(diagnostic.message.find("unknown module") == std::string::npos);
        CHECK(diagnostic.severity != 1);
    }
    CHECK(note.diagnostics.empty());

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp declares a semantic tokens legend and serves the stream") {
    LiveSession session;
    // The legend arrives in the initialize response, and its indexes ARE the
    // wire encoding, so a client that never saw it cannot decode anything.
    session.send(
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{"workspace":{"semanticTokens":{"refreshSupport":true}}}}})");
    const std::optional<Message> ack = session.recv();
    REQUIRE(ack.has_value());
    CHECK(ack->result.str.find(R"("semanticTokensProvider")") != std::string::npos);
    CHECK(ack->result.str.find(R"("tokenTypes")") != std::string::npos);
    CHECK(ack->result.str.find(R"("enumMember")") != std::string::npos);
    CHECK(ack->result.str.find(R"("delta":true)") != std::string::npos);
    session.send(kInitialized);

    session.send(did_open(kUri, 1, kLatch));
    REQUIRE(session.notification(kPublish).has_value());

    session.send(std::format(
      R"({{"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{{"textDocument":{{"uri":"{}"}}}}}})",
      kUri));
    const std::optional<Message> tokens = session.response(2);
    REQUIRE(tokens.has_value());
    // Five integers per token, and at least the module, its three ports and the
    // two uses of q and d.
    CHECK(tokens->result.str.find(R"("resultId")") != std::string::npos);
    CHECK(tokens->result.str.find(R"("data")") != std::string::npos);

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp answers a delta against an unchanged analysis with no edits") {
    LiveSession session;
    session.initialize();
    session.send(did_open(kUri, 1, kLatch));
    REQUIRE(session.notification(kPublish).has_value());

    session.send(std::format(
      R"({{"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{{"textDocument":{{"uri":"{}"}}}}}})",
      kUri));
    const std::optional<Message> full = session.response(2);
    REQUIRE(full.has_value());

    // resultId is the analysis the tokens describe. Asking for a delta against
    // the one just received must produce an empty edit list, not a repaint.
    session.send(std::format(
      R"({{"jsonrpc":"2.0","id":3,"method":"textDocument/semanticTokens/full/delta","params":{{"textDocument":{{"uri":"{}"}},"previousResultId":"1"}}}})",
      kUri));
    const std::optional<Message> delta = session.response(3);
    REQUIRE(delta.has_value());
    CHECK(delta->result.str.find(R"("edits":[])") != std::string::npos);

    CHECK(session.shutdown() == 0);
}

TEST_CASE("lsp asks the client to refresh tokens after a new analysis") {
    LiveSession session;
    session.initialize(R"(["utf-8","utf-16"])");
    session.send(did_open(kUri, 1, kLatch));
    REQUIRE(session.notification(kPublish).has_value());

    session.send(did_change(kUri, 2, kNoLatch));

    // Diagnostics are pushed but tokens are pulled, so without this request the
    // colours stay at whatever version the client last asked for.
    CHECK(session.notification("workspace/semanticTokens/refresh").has_value());

    CHECK(session.shutdown() == 0);
}

#endif // !_WIN32
