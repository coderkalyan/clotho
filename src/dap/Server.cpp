#include "dap/Dap.h"

// The debug adapter's message pump and dispatcher (docs/design/dap.md).
//
// The topology is the language server's, because the base protocol is the same
// one and the reasoning that produced it did not change:
//
//   stdin -> reader thread -> events_ -> main loop -> outgoing_ -> writer thread -> stdout
//                               ^
//                       design reader thread
//
// The main loop owns every mutable field below, so nothing here takes a lock
// around adapter state. The writer is split off so a client that stops reading
// blocks its own thread rather than freezing the dispatcher, and the design
// reader is split off because it spends its life in poll(2) on the child's
// pipes.
//
// What this stage is, exactly: D0 of docs/design/dap.md 5. It launches a design
// and reports what the design says and how it ended. It does not stop, and it
// says so -- every stop-shaped capability is absent and every stop-shaped
// request is refused by name, with the stage that implements it. There is no
// half-working breakpoint here, deliberately (PRINCIPLES.md: never silently
// accept).

#include "dap/Design.h"
#include "transport/Transport.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

namespace clotho::dap {

namespace {

// The framing layer is shared with the language server (src/transport); these
// are the only names this file takes from it.
using transport::configure_stdio;
using transport::FrameReader;
using transport::FrameWriter;
using transport::kStdinFd;
using transport::kStdoutFd;

// Clients extend every structure in the protocol, and an adapter that rejects
// a field it has not heard of is one that breaks on the next editor release.
constexpr glz::opts kRead{ .error_on_unknown_keys = false };

// -- wire structures ---------------------------------------------------------
//
// DAP's envelope is simpler than JSON-RPC's: ids are int32 only, so there is no
// variant to carry, and `type` discriminates instead of the presence of a
// field. `arguments` is left unparsed for the same reason the language server
// leaves `params` unparsed -- the dispatcher does not know the argument type
// and the handler that does can read it out of the original bytes.

struct Request {
    int64_t            seq = 0;
    std::string_view   type;
    std::string_view   command;
    glz::raw_json_view arguments;
};

struct Message {
    int32_t     id = 0;
    std::string format;
};

struct ErrorBody {
    Message error;
};

struct ErrorResponse {
    int64_t          seq         = 0;
    std::string_view type        = "response";
    int64_t          request_seq = 0;
    bool             success     = false;
    std::string_view command;
    std::string      message;
    ErrorBody        body;
};

// Acknowledgement with no body. A separate type rather than an optional field,
// because `"body": null` and no body at all are different messages and the
// second is the one the spec asks for.
struct Ack {
    int64_t          seq         = 0;
    std::string_view type        = "response";
    int64_t          request_seq = 0;
    bool             success     = true;
    std::string_view command;
};

template<class Body>
struct Reply {
    int64_t          seq         = 0;
    std::string_view type        = "response";
    int64_t          request_seq = 0;
    bool             success     = true;
    std::string_view command;
    Body             body;
};

// An event with nothing to say beyond its name (`initialized`, `terminated`).
struct BareEvent {
    int64_t          seq  = 0;
    std::string_view type = "event";
    std::string_view event;
};

template<class Body>
struct EventOf {
    int64_t          seq  = 0;
    std::string_view type = "event";
    std::string_view event;
    Body             body;
};

// Only what is true. Absent is false throughout DAP's capability set, so an
// omitted field is the honest way to say "not yet" -- and every capability
// this stage does not claim has a request that refuses by name below.
struct Capabilities {
    bool supportsConfigurationDoneRequest = true;
    bool supportsTerminateRequest         = true;
    bool supportTerminateDebuggee         = true;
};

struct InitializeArguments {
    std::optional<std::string> clientID;
    std::optional<std::string> clientName;
    std::optional<bool>        linesStartAt1;
    std::optional<bool>        columnsStartAt1;
};

// The launch configuration. `args` is the `clotho run` argument tail, passed
// to the design process verbatim: one vocabulary for the command line and the
// launch config, and no second parser to drift (docs/design/cli.md).
struct LaunchArguments {
    std::optional<std::vector<std::string>> args;
    std::optional<std::string>              cwd;
    std::optional<bool>                     stopOnEntry;
    std::optional<bool>                     noDebug;
};

struct DisconnectArguments {
    std::optional<bool> restart;
    std::optional<bool> terminateDebuggee;
};

struct CancelArguments {
    std::optional<int64_t> requestId;
};

struct OutputBody {
    std::string_view category;
    std::string      output;
};

struct ProcessBody {
    std::string      name;
    int64_t          systemProcessId = 0;
    bool             isLocalProcess  = true;
    std::string_view startMethod     = "launch";
};

struct ExitedBody {
    int32_t exitCode = 0;
};

struct ThreadInfo {
    int64_t     id = 0;
    std::string name;
};

struct ThreadsBody {
    std::vector<ThreadInfo> threads;
};

// A breakpoint the adapter could not arm. DAP models this per breakpoint
// rather than as a failed request, which is the better answer here: the client
// shows the user exactly which markers are inert and why, instead of an error
// dialog about the whole file.
struct BreakpointInfo {
    bool             verified = false;
    std::string_view reason   = "failed";
    std::string_view message;
};

struct BreakpointsBody {
    std::vector<BreakpointInfo> breakpoints;
};

struct SourceBreakpointsArguments {
    std::optional<std::vector<glz::raw_json>> breakpoints;
};

// -- plumbing ----------------------------------------------------------------

// An unbounded MPSC queue. Unbounded on purpose: the producers are one reader
// thread throttled by the client's write rate, and one design reader throttled
// by the design's own output.
template<class T>
class Channel {
public:
    void push(T value) {
        {
            const std::lock_guard lock(mutex_);
            queue_.push_back(std::move(value));
        }
        ready_.notify_one();
    }

    // Blocks. False once the channel is closed and drained.
    bool pop(T& out) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close() {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

private:
    std::mutex              mutex_;
    std::condition_variable ready_;
    std::deque<T>           queue_;
    bool                    closed_ = false;
};

struct Event {
    enum class Kind : uint8_t { Batch, DesignOutput, DesignExited, Eof, TransportError };

    Kind                     kind = Kind::Batch;
    std::vector<std::string> bodies;
    // The message for a TransportError, the text for a DesignOutput.
    std::string detail;
    bool        from_stderr = false;
    int32_t     exit_code   = 0;
};

// Where the handshake has got to. DAP's is not the language server's: the
// adapter sends `initialized`, the client answers with its configuration, and
// only `configurationDone` plus `launch` together mean "start the design".
enum class Lifecycle : uint8_t { Uninitialized, Configuring, Running, Ended };

// No parent-process watchdog, unlike the language server: DAP's `initialize`
// carries no client pid, and it does not need to. A client that dies takes the
// write end of this adapter's stdin with it, so the reader sees EOF -- which is
// the same signal, without a poll or a field that might be absent.

class Adapter {
public:
    Adapter(std::string_view version, std::string_view argv0)
      : version_(version)
      , exe_(self_exe(argv0)) {}

    [[noreturn]] void run();

    // The handlers are public because the dispatch table below is a
    // namespace-scope constant naming them. State stays private.
    using Handler = void (Adapter::*)(const Request&);

    void on_initialize(const Request& request);
    void on_launch(const Request& request);
    void on_configuration_done(const Request& request);
    void on_disconnect(const Request& request);
    void on_terminate(const Request& request);
    void on_threads(const Request& request);
    void on_set_breakpoints(const Request& request);
    void on_cancel(const Request& request);

private:
    void read_loop();
    void write_loop();
    void design_loop();

    void handle_batch(std::vector<std::string>& bodies);
    void dispatch(const Request& request);
    void maybe_start_design();
    void stop_design();
    void finish(int code);

    void    send(std::string body);
    int64_t next_seq();
    void    ack(const Request& request);
    void    fail(const Request& request, std::string message);
    void    refuse(const Request& request, std::string_view stage);
    void    bare_event(std::string_view name);
    void    output(std::string_view category, std::string text);
    template<class Body>
    void reply(const Request& request, const Body& body);
    template<class Body>
    void emit(std::string_view name, const Body& body);
    template<class Args>
    bool parse(const Request& request, Args& out);

    std::string_view version_;
    std::string      exe_;

    Channel<Event>       events_;
    Channel<std::string> outgoing_;

    Lifecycle state_     = Lifecycle::Uninitialized;
    bool      done_      = false;
    int       exit_code_ = 0;
    int64_t   seq_       = 0;
    // The `launch` request is answered as soon as it is understood, but the
    // design does not start until the client has also finished configuring.
    // Either order is legal, so both arrivals are recorded rather than assumed.
    bool                           launched_   = false;
    bool                           configured_ = false;
    std::vector<std::string>       design_args_;
    std::string                    design_cwd_;
    std::unique_ptr<DesignProcess> design_;
    std::vector<Request>           batch_;
};

// The dispatch table. Sorted, so lookup is a binary search and the
// static_assert below catches a duplicate or an out-of-order entry at build
// time -- worth having now, because this table is where every later stage
// lands.
constexpr std::array<std::pair<std::string_view, Adapter::Handler>, 8> kCommands{ {
  { "cancel", &Adapter::on_cancel },
  { "configurationDone", &Adapter::on_configuration_done },
  { "disconnect", &Adapter::on_disconnect },
  { "initialize", &Adapter::on_initialize },
  { "launch", &Adapter::on_launch },
  { "setBreakpoints", &Adapter::on_set_breakpoints },
  { "terminate", &Adapter::on_terminate },
  { "threads", &Adapter::on_threads },
} };

// Commands this adapter will answer, but not yet, each with the stage of
// docs/design/dap.md 6 that implements it. Refusing by name with the reason is
// the difference between "not built" and "broken", and the client shows the
// text. Sorted for the same reason kCommands is.
constexpr std::array<std::pair<std::string_view, std::string_view>, 17> kPlanned{ {
  { "attach", "not planned: there is no running simulation to attach to" },
  { "breakpointLocations", "D3: source breakpoints" },
  { "continue", "D1: stopping at severity and assertion sites" },
  { "dataBreakpointInfo", "D4: signal watchpoints" },
  { "evaluate", "D5: expression evaluation" },
  { "exceptionInfo", "D1: stopping at severity and assertion sites" },
  { "next", "D3: statement stepping" },
  { "pause", "D3: statement stepping" },
  { "restart", "not yet: an adapter serves one run, so restart is a respawn" },
  { "scopes", "D2: the design's state" },
  { "setDataBreakpoints", "D4: signal watchpoints" },
  { "setExceptionBreakpoints", "D1: stopping at severity and assertion sites" },
  { "setFunctionBreakpoints", "D5: task and function frames" },
  { "stackTrace", "D2: the instance hierarchy" },
  { "stepIn", "D3: statement stepping" },
  { "stepOut", "D5: task and function frames" },
  { "variables", "D2: the design's state" },
} };

template<class Table>
constexpr bool sorted_by_name(const Table& table) {
    for (size_t i = 1; i < table.size(); ++i) {
        if (!(table[i - 1].first < table[i].first)) {
            return false;
        }
    }
    return true;
}
static_assert(sorted_by_name(kCommands));
static_assert(sorted_by_name(kPlanned));

template<class Table>
auto find_command(const Table& table, std::string_view name) -> const typename Table::value_type* {
    using Entry   = typename Table::value_type;
    const auto it = std::ranges::lower_bound(table, name, {}, &Entry::first);
    if (it == table.end() || it->first != name) {
        return nullptr;
    }
    return &*it;
}

// stderr is the only place the adapter may talk out of band: stdout is the
// protocol, and the design's own output arrives over a pipe and goes out as an
// `output` event instead.
template<class... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    std::println(stderr, "clotho dap: {}", std::format(fmt, std::forward<Args>(args)...));
}

// -- I/O threads -------------------------------------------------------------

void Adapter::read_loop() {
    FrameReader reader(kStdinFd);
    for (;;) {
        std::vector<std::string> bodies;
        switch (reader.read(bodies)) {
            case FrameReader::Status::Ok:
                events_.push(Event{ .kind = Event::Kind::Batch, .bodies = std::move(bodies) });
                break;
            case FrameReader::Status::Eof:
                events_.push(Event{ .kind = Event::Kind::Eof });
                return;
            case FrameReader::Status::Error:
                events_.push(Event{ .kind = Event::Kind::TransportError, .detail = std::string(reader.error()) });
                return;
        }
    }
}

void Adapter::write_loop() {
    FrameWriter writer(kStdoutFd);
    std::string body;
    while (outgoing_.pop(body)) {
        if (!writer.write(body)) {
            events_.push(Event{ .kind = Event::Kind::TransportError, .detail = "stdout closed" });
            return;
        }
    }
}

// Parked in poll(2) on the design's pipes for its whole life. Runs on its own
// thread rather than in the main loop's wait, because the main loop waits on a
// condition variable and no portable call waits on both.
void Adapter::design_loop() {
    std::vector<DesignProcess::Chunk> chunks;
    while (design_->read(chunks)) {
        for (DesignProcess::Chunk& chunk : chunks) {
            events_.push(Event{ .kind        = Event::Kind::DesignOutput,
                                .detail      = std::move(chunk.text),
                                .from_stderr = chunk.stream == DesignProcess::Stream::Stderr });
        }
    }
    events_.push(Event{ .kind = Event::Kind::DesignExited, .exit_code = design_->exit_code() });
}

void Adapter::run() {
    configure_stdio();

    // Detached, and never joined: it spends its life in a blocking read, and
    // the process ends below rather than unwinding past it. The writer is the
    // exception, because its queue is the only thing between a response and
    // the client seeing it.
    std::thread([this] { read_loop(); }).detach();
    std::thread writer([this] { write_loop(); });

    while (!done_) {
        Event ev;
        if (!events_.pop(ev)) {
            break;
        }
        switch (ev.kind) {
            case Event::Kind::Batch:
                handle_batch(ev.bodies);
                break;
            case Event::Kind::DesignOutput:
                output(ev.from_stderr ? "stderr" : "stdout", std::move(ev.detail));
                break;
            case Event::Kind::DesignExited:
                // Both, in this order: `exited` carries the design's status,
                // `terminated` tells the client the session is over. A client
                // that sees only the second reports no exit code at all.
                emit("exited", ExitedBody{ .exitCode = ev.exit_code });
                bare_event("terminated");
                state_ = Lifecycle::Ended;
                break;
            case Event::Kind::Eof:
                // The client vanished without saying goodbye -- it died, or it
                // closed the pipe. Either way the design has nobody to report
                // to, so it goes with it.
                log("client disconnected");
                stop_design();
                finish(1);
                break;
            case Event::Kind::TransportError:
                log("transport error: {}", ev.detail);
                stop_design();
                finish(1);
                break;
        }
    }

    outgoing_.close();
    writer.join();
    std::_Exit(exit_code_);
}

void Adapter::stop_design() {
    if (design_ != nullptr) {
        design_->terminate();
    }
}

void Adapter::finish(int code) {
    exit_code_ = code;
    done_      = true;
}

// -- dispatch ----------------------------------------------------------------

void Adapter::handle_batch(std::vector<std::string>& bodies) {
    // `bodies` outlives this call's parsing and nothing appends to it, so the
    // string_views the requests hold into it stay valid for the whole batch.
    batch_.clear();
    batch_.resize(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (glz::read<kRead>(batch_[i], bodies[i])) {
            log("could not parse a message; ignoring it");
            batch_[i].command = {};
        }
    }
    for (const Request& request : batch_) {
        if (!request.command.empty()) {
            dispatch(request);
        }
    }
}

void Adapter::dispatch(const Request& request) {
    // `type` is the discriminator, and an adapter is only ever sent requests.
    // A response or an event arriving here means the client is confused, and
    // answering it would confuse it further.
    if (request.type != "request") {
        log("ignoring a message of type '{}'", request.type);
        return;
    }
    if (state_ == Lifecycle::Uninitialized && request.command != "initialize") {
        fail(request, "the adapter has not been initialized");
        return;
    }
    if (const auto* entry = find_command(kCommands, request.command); entry != nullptr) {
        (this->*entry->second)(request);
        return;
    }
    if (const auto* planned = find_command(kPlanned, request.command); planned != nullptr) {
        refuse(request, planned->second);
        return;
    }
    fail(request, std::format("'{}' is not a command this adapter has", request.command));
}

// -- serialization -----------------------------------------------------------

void Adapter::send(std::string body) {
    outgoing_.push(std::move(body));
}

// Allocated here rather than in the writer thread: the writer moves an already
// serialized string, and stamping the sequence number there would mean
// serializing twice. The main loop is the only caller, so the counter needs no
// synchronization.
int64_t Adapter::next_seq() {
    return ++seq_;
}

void Adapter::ack(const Request& request) {
    std::string body;
    if (glz::write_json(Ack{ .seq = next_seq(), .request_seq = request.seq, .command = request.command }, body)) {
        log("could not serialize the response to '{}'", request.command);
        return;
    }
    send(std::move(body));
}

template<class Body>
void Adapter::reply(const Request& request, const Body& payload) {
    std::string body;
    if (glz::write_json(
          Reply<Body>{ .seq = next_seq(), .request_seq = request.seq, .command = request.command, .body = payload },
          body)) {
        log("could not serialize the response to '{}'", request.command);
        return;
    }
    send(std::move(body));
}

void Adapter::fail(const Request& request, std::string message) {
    std::string   body;
    ErrorResponse response{
        .seq = next_seq(), .request_seq = request.seq, .command = request.command, .message = std::move(message)
    };
    response.body.error.format = response.message;
    if (glz::write_json(response, body)) {
        log("could not serialize the error for '{}'", request.command);
        return;
    }
    send(std::move(body));
}

void Adapter::refuse(const Request& request, std::string_view stage) {
    fail(request, std::format("'{}' is not implemented yet ({})", request.command, stage));
}

void Adapter::bare_event(std::string_view name) {
    std::string body;
    if (glz::write_json(BareEvent{ .seq = next_seq(), .event = name }, body)) {
        log("could not serialize the '{}' event", name);
        return;
    }
    send(std::move(body));
}

template<class Body>
void Adapter::emit(std::string_view name, const Body& payload) {
    std::string body;
    if (glz::write_json(EventOf<Body>{ .seq = next_seq(), .event = name, .body = payload }, body)) {
        log("could not serialize the '{}' event", name);
        return;
    }
    send(std::move(body));
}

void Adapter::output(std::string_view category, std::string text) {
    emit("output", OutputBody{ .category = category, .output = std::move(text) });
}

template<class Args>
bool Adapter::parse(const Request& request, Args& out) {
    if (request.arguments.str.empty()) {
        return true;
    }
    if (const auto ec = glz::read<kRead>(out, request.arguments.str)) {
        log("could not read the arguments of '{}': {}", request.command, glz::format_error(ec, request.arguments.str));
        return false;
    }
    return true;
}

// -- handlers ----------------------------------------------------------------

void Adapter::on_initialize(const Request& request) {
    if (state_ != Lifecycle::Uninitialized) {
        fail(request, "the adapter is already initialized");
        return;
    }
    InitializeArguments arguments;
    if (!parse(request, arguments)) {
        fail(request, "could not read the initialize arguments");
        return;
    }
    // Both default to true in the protocol, and every location this stage
    // reports comes from the design's own output rather than from a span, so a
    // client that wants 0-based counting is told the adapter cannot oblige
    // rather than quietly given 1-based numbers later.
    if (arguments.linesStartAt1 == false || arguments.columnsStartAt1 == false) {
        fail(request, "this adapter reports 1-based lines and columns only");
        return;
    }
    log("initialized by {}", arguments.clientName.value_or(arguments.clientID.value_or("an unnamed client")));

    state_ = Lifecycle::Configuring;
    reply(request, Capabilities{});
    // Configuration requests are only legal after this, and the client waits
    // for it, so it must follow the response rather than precede it.
    bare_event("initialized");
}

void Adapter::on_launch(const Request& request) {
    if (state_ != Lifecycle::Configuring) {
        fail(request, "launch must follow initialize and may only be sent once");
        return;
    }
    LaunchArguments arguments;
    if (!parse(request, arguments)) {
        fail(request, "could not read the launch arguments");
        return;
    }
    if (arguments.stopOnEntry == true) {
        fail(request, "'stopOnEntry' needs a stop point, which arrives in D1; launch without it");
        return;
    }
    if (!arguments.args.has_value() || arguments.args->empty()) {
        fail(request, "'args' must carry the `clotho run` arguments for the design, and is empty");
        return;
    }
    design_args_ = *std::move(arguments.args);
    design_cwd_  = arguments.cwd.value_or(std::string{});
    launched_    = true;
    ack(request);
    maybe_start_design();
}

void Adapter::on_configuration_done(const Request& request) {
    configured_ = true;
    ack(request);
    maybe_start_design();
}

// Either order is legal -- a client may finish configuring before or after it
// launches -- so the design starts on whichever of the two arrives second.
void Adapter::maybe_start_design() {
    if (!launched_ || !configured_ || design_ != nullptr) {
        return;
    }
    // The console gets the adapter's identity and the exact command it is
    // about to run, because the first question about any debug session that
    // behaves oddly is which build produced it and what it was actually told.
    std::string banner = std::format("clotho dap {}: run", version_);
    for (const std::string& arg : design_args_) {
        banner += ' ';
        banner += arg;
    }
    output("console", banner + "\n");

    design_ = std::make_unique<DesignProcess>();
    if (!design_->start(exe_, design_args_, design_cwd_)) {
        output("important", std::format("{}\n", design_->error()));
        emit("exited", ExitedBody{ .exitCode = 1 });
        bare_event("terminated");
        design_.reset();
        state_ = Lifecycle::Ended;
        return;
    }
    state_ = Lifecycle::Running;
    emit(
      "process",
      ProcessBody{ .name = exe_, .systemProcessId = design_->pid(), .isLocalProcess = true, .startMethod = "launch" });
    std::thread([this] { design_loop(); }).detach();
}

void Adapter::on_disconnect(const Request& request) {
    DisconnectArguments arguments;
    (void)parse(request, arguments);
    if (arguments.restart == true) {
        fail(request, "restart is not implemented yet (an adapter serves one run; spawn a new one)");
        return;
    }
    // `terminateDebuggee` defaults to killing a launched debuggee, and this
    // adapter cannot detach from one: the design is a child whose pipes are
    // the only reason it has somewhere to write.
    stop_design();
    ack(request);
    finish(0);
}

void Adapter::on_terminate(const Request& request) {
    stop_design();
    ack(request);
}

void Adapter::on_threads(const Request& request) {
    // One thread, because that is what is true at this stage: the design runs
    // as a single unit and nothing here can see inside it. The per-process
    // thread list of docs/design/dap.md 3.1 arrives with D2, and until then
    // inventing entries for processes the adapter cannot inspect would be
    // worse than reporting the one it can.
    ThreadsBody body;
    if (state_ == Lifecycle::Running) {
        body.threads.push_back({ .id = 1, .name = "simulation" });
    }
    reply(request, body);
}

void Adapter::on_set_breakpoints(const Request& request) {
    SourceBreakpointsArguments arguments;
    (void)parse(request, arguments);
    // Answered rather than refused, so the client marks each breakpoint inert
    // with a reason instead of raising one error about the whole file. The
    // markers stay visible and the user is told exactly what they do: nothing.
    BreakpointsBody body;
    const size_t    count = arguments.breakpoints.has_value() ? arguments.breakpoints->size() : 0;
    body.breakpoints.resize(
      count, BreakpointInfo{ .verified = false, .reason = "failed", .message = "source breakpoints arrive in D3" });
    reply(request, body);
}

void Adapter::on_cancel(const Request& request) {
    // Nothing this adapter does outlives the request that asked for it, so
    // there is never anything in flight to cancel. Acknowledged rather than
    // refused: the spec makes `cancel` best-effort, and "there was nothing to
    // stop" is a successful best effort.
    CancelArguments arguments;
    (void)parse(request, arguments);
    ack(request);
}

} // namespace

[[noreturn]] void serve(std::string_view version, std::string_view argv0) {
    Adapter adapter(version, argv0);
    adapter.run();
}

} // namespace clotho::dap
