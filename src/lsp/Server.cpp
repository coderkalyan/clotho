// The language server: framing threads around a single-threaded dispatcher.
//
//   stdin ──► reader thread ──► event queue ──► main loop ──► outgoing queue ──► writer thread ──► stdout
//
// Three threads, one owner. The reader only reads and the writer only writes,
// so neither can be delayed by, or delay, the work; the main loop owns every
// mutable field the server has, which is why nothing in this file takes a lock
// around server state. That shape is the reason for the threads at all: a
// language server's state is small, hot, and touched by every handler, and the
// alternative -- a mutex around it -- is the design that later has to be undone
// (docs/design/lsp.md section 6.7).
//
// Splitting the writer off matters for a second reason that only shows up under
// load: an editor that stops reading will block a write, and if that write
// happens on the dispatch thread the server stops answering anything. Here it
// applies backpressure to a queue instead.
//
// What is deliberately absent: a worker pool. Nothing offloads yet because
// nothing here computes anything, and the shape of an offloaded request depends
// on the immutable snapshot it would carry -- which does not exist until the
// frontend gains a named in-memory buffer entry point (docs/design/lsp.md
// section 1.1 #1). The seam is the event queue: a pool posts completions to it
// exactly the way the reader posts messages, and no handler signature changes.

#include "Lower.h"
#include "lsp/LineIndex.h"
#include "lsp/Lsp.h"
#include "lsp/Tokens.h"
#include "transport/Transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace clotho::lsp {

namespace {

// The framing layer is shared with the debug adapter (src/transport), so it
// lives in its own namespace; these are the only names this file takes from it.
using transport::configure_stdio;
using transport::FrameReader;
using transport::FrameWriter;
using transport::kStdinFd;
using transport::kStdoutFd;

constexpr std::string_view kJsonRpcVersion = "2.0";

// JSON-RPC 2.0's codes plus the LSP reservations this server can produce.
enum class ErrorCode : int32_t {
    ParseError           = -32700,
    InvalidRequest       = -32600,
    MethodNotFound       = -32601,
    InvalidParams        = -32602,
    InternalError        = -32603,
    ServerNotInitialized = -32002,
    RequestCancelled     = -32800,
};

// A request id: an integer, a string, or absent. Absent and JSON `null` both
// land on the monostate, which is what makes one type serve both directions --
// a notification has no id, and a response to a message too malformed to have
// an id must carry `null`.
using Id = std::variant<std::monostate, int64_t, std::string>;

bool is_request(const Id& id) {
    return id.index() != 0;
}

// Clients extend every structure in the protocol, and a server that rejects a
// field it has not heard of is a server that breaks on the next editor release.
constexpr glz::opts kRead{ .error_on_unknown_keys = false };

// -- wire structures ---------------------------------------------------------

struct Envelope {
    std::string_view jsonrpc;
    Id               id;
    std::string_view method;
    // Left unparsed: the dispatcher does not know the parameter type, and the
    // handler that does can read it straight out of the original bytes.
    glz::raw_json_view params;
};

struct ErrorBody {
    int32_t          code;
    std::string_view message;
};

struct ErrorResponse {
    std::string_view jsonrpc = kJsonRpcVersion;
    Id               id;
    ErrorBody        error;
};

template<class Result>
struct ResultResponse {
    std::string_view jsonrpc = kJsonRpcVersion;
    Id               id;
    Result           result;
};

struct ServerInfo {
    std::string_view name;
    std::string_view version;
};

// Only the full-document form is offered: the server discards the text anyway,
// and declaring incremental sync would ask clients to send edits this build has
// no position arithmetic to apply.
struct TextDocumentSyncOptions {
    bool    openClose;
    int32_t change;
};
constexpr int32_t kSyncFull = 1;

struct SemanticTokensFull {
    bool delta = true;
};

struct SemanticTokensLegend {
    std::vector<std::string_view> tokenTypes;
    std::vector<std::string_view> tokenModifiers;
};

struct SemanticTokensOptions {
    SemanticTokensLegend legend;
    // `range` for a fast first paint, `full` so scrolling and the minimap stay
    // consistent, `delta` so a keystroke costs one array edit rather than a
    // repaint of every token (docs/design/lsp.md 8.12.5).
    bool               range = true;
    SemanticTokensFull full;
};

struct ServerCapabilities {
    std::string_view        positionEncoding;
    TextDocumentSyncOptions textDocumentSync;
    SemanticTokensOptions   semanticTokensProvider;
};

struct InitializeResult {
    ServerCapabilities capabilities;
    ServerInfo         serverInfo;
};

struct GeneralClientCapabilities {
    std::optional<std::vector<std::string>> positionEncodings;
};

struct SemanticTokensWorkspaceCapabilities {
    std::optional<bool> refreshSupport;
};

struct WorkspaceClientCapabilities {
    std::optional<SemanticTokensWorkspaceCapabilities> semanticTokens;
};

struct ClientCapabilities {
    std::optional<GeneralClientCapabilities>   general;
    std::optional<WorkspaceClientCapabilities> workspace;
};

struct ClientInfo {
    std::string name;
};

struct InitializeParams {
    // Null when the client was not started by another process.
    std::optional<int64_t>            processId;
    std::optional<ClientInfo>         clientInfo;
    std::optional<ClientCapabilities> capabilities;
};

struct CancelParams {
    Id id;
};

struct TraceParams {
    std::string value;
};

struct TextDocumentItem {
    std::string uri;
    std::string languageId;
    int64_t     version;
    std::string text;
};

struct DidOpenParams {
    TextDocumentItem textDocument;
};

struct VersionedTextDocumentIdentifier {
    std::string uri;
    int64_t     version;
};

// Full-document sync, so `text` is the whole buffer and the range fields a
// client would send under incremental sync are absent by construction.
struct TextDocumentContentChangeEvent {
    std::string text;
};

struct DidChangeParams {
    VersionedTextDocumentIdentifier             textDocument;
    std::vector<TextDocumentContentChangeEvent> contentChanges;
};

struct TextDocumentIdentifier {
    std::string uri;
};

struct DidCloseParams {
    TextDocumentIdentifier textDocument;
};

struct Position {
    uint32_t line;
    uint32_t character;
};

struct Range {
    Position start;
    Position end;
};

struct Location {
    std::string uri;
    Range       range;
};

struct SemanticTokensParams {
    TextDocumentIdentifier textDocument;
};

struct SemanticTokensDeltaParams {
    TextDocumentIdentifier textDocument;
    std::string            previousResultId;
};

struct SemanticTokensRangeParams {
    TextDocumentIdentifier textDocument;
    Range                  range;
};

struct SemanticTokens {
    // Identifies this exact array so the client can ask for a delta against it.
    // The document version, stringified: two requests answered from the same
    // analysis produce the same id, which is what makes the delta meaningful.
    std::string           resultId;
    std::vector<uint32_t> data;
};

// One splice of the previous array. LSP allows several; one is enough here,
// because the cheapest correct diff of two token arrays is the single span
// between their common prefix and common suffix.
struct SemanticTokensEdit {
    uint32_t              start;
    uint32_t              deleteCount;
    std::vector<uint32_t> data;
};

struct SemanticTokensDelta {
    std::string                     resultId;
    std::vector<SemanticTokensEdit> edits;
};

struct RelatedInformation {
    Location    location;
    std::string message;
};

// LSP DiagnosticSeverity.
constexpr int32_t kSeverityError       = 1;
constexpr int32_t kSeverityWarning     = 2;
constexpr int32_t kSeverityInformation = 3;

struct LspDiagnostic {
    Range   range;
    int32_t severity;
    // Clotho's stable kebab-case identifier, absent when the underlying
    // diagnostic has no stable name (`Diagnostic::code` is documented as
    // possibly empty, and an empty string here would render as a bare "[]").
    std::optional<std::string>      code;
    std::string_view                source;
    std::string                     message;
    std::vector<RelatedInformation> relatedInformation;
};

// A request the SERVER sends. The only one it makes: it carries no params and
// its result is null, and the point is entirely the side effect of the client
// re-asking for tokens.
struct RefreshRequest {
    std::string_view jsonrpc = kJsonRpcVersion;
    int64_t          id;
    std::string_view method;
};

struct PublishDiagnosticsParams {
    std::string                uri;
    int64_t                    version;
    std::vector<LspDiagnostic> diagnostics;
};

template<class Params>
struct NotificationMessage {
    std::string_view jsonrpc = kJsonRpcVersion;
    std::string_view method;
    Params           params;
};

// -- file URIs ---------------------------------------------------------------

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Decode a `file://` URI into a filesystem path. Empty for any other scheme --
// the server can only analyse what it can read.
std::string path_from_uri(std::string_view uri) {
    constexpr std::string_view kScheme = "file://";
    if (!uri.starts_with(kScheme)) {
        return {};
    }
    // Skip the authority, which is empty for every URI an editor produces
    // ("file:///path") but is permitted to name a host.
    uri.remove_prefix(kScheme.size());
    const size_t slash = uri.find('/');
    if (slash == std::string_view::npos) {
        return {};
    }
    uri.remove_prefix(slash);

    std::string path;
    path.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int hi = hex_value(uri[i + 1]);
            const int lo = hex_value(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                path.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        path.push_back(uri[i]);
    }

    // "/C:/x" is how a Windows path arrives; the leading slash is URI syntax,
    // not part of the path.
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') {
        path.erase(0, 1);
    }
    return path;
}

// Encode a filesystem path as a `file://` URI. Only used for the location of a
// related note, which may sit in a file the client never named; the open
// document is always published under the exact URI the client sent, never a
// round-tripped one.
std::string uri_from_path(std::string_view path) {
    std::string uri = "file://";
    if (!path.starts_with('/')) {
        uri.push_back('/'); // A Windows path starts at its drive letter.
    }
    for (const unsigned char c : path) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
        if (unreserved) {
            uri.push_back(static_cast<char>(c));
        } else {
            std::format_to(std::back_inserter(uri), "%{:02X}", c);
        }
    }
    return uri;
}

// -- diagnostics -------------------------------------------------------------

// One conversion for the whole server: diagnostics and semantic tokens go
// through the same index, so the UTF-16 arithmetic exists once and is tested
// once. Locations outside the indexed buffer -- an included header -- have no
// position in this document and collapse to its start.
Range range_of(const SourceContext& sources, ir::SourceSpan span, const LineIndex& lines) {
    const auto point = [&](ir::SourceLoc loc) {
        const std::optional<uint32_t> offset = sources.offset(loc);
        if (!offset) {
            return Position{ .line = 0, .character = 0 };
        }
        const LineIndex::Point p = lines.point(*offset);
        return Position{ .line = p.line, .character = p.character };
    };

    if (!span.valid()) {
        // A driver-level error has no location but still has to land somewhere
        // the user will see it.
        return Range{};
    }
    const Position start = point(span.start);
    // Clotho's spans are half-open, which is also what LSP wants, so `end` maps
    // straight across. A span with no end degenerates to an empty range, which
    // the editor renders as a caret.
    return Range{ .start = start, .end = span.end != ir::SourceLoc{ 0 } ? point(span.end) : start };
}

int32_t severity_of(Severity severity) {
    switch (severity) {
        case Severity::Error:
            return kSeverityError;
        case Severity::Warning:
            return kSeverityWarning;
        case Severity::Note:
            return kSeverityInformation;
    }
    return kSeverityInformation;
}

LspDiagnostic to_lsp(const Diagnostic& diagnostic, const SourceContext& sources, const LineIndex& lines) {
    LspDiagnostic out{
        .range    = range_of(sources, diagnostic.span, lines),
        .severity = severity_of(diagnostic.severity),
        .code     = diagnostic.code.empty() ? std::nullopt : std::optional<std::string>(diagnostic.code),
        .source   = "clotho",
        .message  = diagnostic.message,
    };
    // `rendered` is deliberately dropped: it is a terminal snippet with a caret,
    // and Diagnostic.h says a non-terminal consumer must ignore it.
    out.relatedInformation.reserve(diagnostic.notes.size());
    for (const Diagnostic& note : diagnostic.notes) {
        out.relatedInformation.push_back(RelatedInformation{
          .location = { .uri   = uri_from_path(sources.full_path(note.span.start)),
                        .range = range_of(sources, note.span, lines) },
          .message  = note.message,
        });
    }
    return out;
}

// -- plumbing ----------------------------------------------------------------

// An unbounded MPSC queue. Unbounded on purpose: the producers are one reader
// thread, throttled by the client's own write rate; the analysis worker, which
// posts at most one result per job; and the watchdog, which posts once.
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

    enum class Pop : uint8_t { Value, Timeout, Closed };

    // As `pop`, but gives up at `deadline`. The debounce timer is the only
    // caller: the loop has to wake on its own schedule as well as on traffic.
    Pop pop_until(T& out, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_until(lock, deadline, [this] { return !queue_.empty() || closed_; })) {
            return Pop::Timeout;
        }
        if (queue_.empty()) {
            return Pop::Closed;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        return Pop::Value;
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

// What the analysis worker is asked to do. Carries the text by value: the
// document store is the main loop's alone, and a worker holding a reference
// into it would be reading a buffer the next keystroke rewrites.
struct AnalysisJob {
    std::string uri;
    std::string path;
    std::string text;
    int64_t     version  = 0;
    Encoding    encoding = Encoding::Utf16;
};

struct AnalysisResult {
    std::string                uri;
    int64_t                    version = 0;
    std::vector<LspDiagnostic> diagnostics;
    // Already in the protocol's delta encoding. Computed here rather than on
    // request because it needs the same elaborated compilation the diagnostics
    // came from, and elaborating a second time on the dispatch thread to answer
    // a tokens request is the one thing the threading exists to avoid.
    std::vector<uint32_t> tokens;
};

struct Event {
    enum class Kind : uint8_t { Batch, Analysis, Eof, TransportError, ParentGone };

    Kind                            kind = Kind::Batch;
    std::vector<std::string>        bodies;
    std::string                     detail;
    std::unique_ptr<AnalysisResult> analysis;
};

// Where the lifecycle handshake has got to. The gate is Uninitialized: before
// `initialize` the protocol allows nothing through, and after `shutdown` it
// allows only `exit`.
enum class Lifecycle : uint8_t { Uninitialized, Initializing, Ready, ShuttingDown };

bool process_alive(int64_t pid) {
#if defined(_WIN32)
    const HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    const bool alive = ::WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    ::CloseHandle(handle);
    return alive;
#else
    // EPERM means it exists and belongs to someone else, which is still alive.
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

class Server {
public:
    explicit Server(std::string_view version)
      : version_(version) {}

    [[noreturn]] void run();

    // The handler signatures and the handlers themselves are public because the
    // dispatch tables below are namespace-scope constants naming them. State
    // stays private.
    using RequestFn      = void (Server::*)(const Id&, std::string_view);
    using NotificationFn = void (Server::*)(std::string_view);

    void on_initialize(const Id& id, std::string_view params);
    void on_shutdown(const Id& id, std::string_view params);
    void on_initialized(std::string_view params);
    void on_exit(std::string_view params);
    void on_set_trace(std::string_view params);
    void on_did_open(std::string_view params);
    void on_did_change(std::string_view params);
    void on_did_close(std::string_view params);
    void on_semantic_tokens_full(const Id& id, std::string_view params);
    void on_semantic_tokens_delta(const Id& id, std::string_view params);
    void on_semantic_tokens_range(const Id& id, std::string_view params);

private:
    // A message after envelope parsing, before dispatch. Parsing the whole batch
    // up front is what lets a cancellation reach back over the requests that
    // preceded it in the same burst.
    struct Staged {
        Envelope env{};
        bool     parsed    = false;
        bool     cancelled = false;
    };

    // What the client has open. The main loop owns this outright; the analysis
    // worker is handed copies, never references.
    struct Document {
        std::string path;
        std::string text;
        int64_t     version = 0;
    };

    // The last analysis result for a document, kept so a semantic-token request
    // can be answered from the dispatch thread without touching a compilation.
    // `version` is what the tokens describe, which is not necessarily the
    // client's current version -- see answer_tokens.
    struct Cache {
        int64_t               version = 0;
        std::vector<uint32_t> tokens;
    };

    void read_loop();
    void write_loop();
    void analysis_loop();

    void handle_batch(std::vector<std::string>& bodies);
    void apply_cancellations();
    void dispatch(const Staged& staged);
    void dispatch_request(const Id& id, std::string_view method, std::string_view params);
    void dispatch_notification(std::string_view method, std::string_view params);

    void respond_null(const Id& id);

    template<class Result>
    void respond(const Id& id, const Result& result);
    void respond_error(const Id& id, ErrorCode code, std::string_view message);
    void send(std::string&& body);

    void finish(int code);

    template<class Params>
    bool parse_params(std::string_view raw, Params& out, std::string_view method);

    template<class Params>
    void notify(std::string_view method, const Params& params);

    void schedule_analysis(const std::string& uri, std::chrono::steady_clock::duration delay);
    void dispatch_due_analyses();
    std::optional<std::chrono::steady_clock::time_point> next_deadline() const;
    void                                                 publish(AnalysisResult& result);

    void         answer_tokens(const Id& id, const std::string& uri, std::string_view previous_result_id);
    void         answer_tokens_range(const Id& id, const std::string& uri, const Range& range);
    const Cache* cache_for(const std::string& uri) const;
    void         request_refresh();

    std::string_view version_;

    Channel<Event>       events_;
    Channel<std::string> outgoing_;

    Lifecycle state_     = Lifecycle::Uninitialized;
    bool      done_      = false;
    int       exit_code_ = 0;
    bool      trace_     = false;
    Encoding  encoding_  = Encoding::Utf16;
    // Whether the client honours workspace/semanticTokens/refresh. If it does
    // not, its colours simply lag until it re-requests for its own reasons.
    bool tokens_refresh_supported_ = false;
    // Ids for requests this server sends. Distinct from the client's id space,
    // which is why replies to these are matched by id rather than assumed.
    int64_t next_request_id_ = 1;

    std::vector<Staged> batch_;

    Channel<AnalysisJob>                      jobs_;
    std::unordered_map<std::string, Document> documents_;
    // Debounce timers, keyed by URI: when the document last stopped changing
    // for long enough to be worth analysing.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> due_;
    // URIs whose analysis is running right now. A second job for the same
    // document would only queue behind the first and be discarded on arrival,
    // so it is not dispatched until the first comes back.
    std::unordered_set<std::string>        in_flight_;
    std::unordered_map<std::string, Cache> tokens_;
};

// The dispatch tables. Sorted, so lookup is a binary search and the
// static_assert below catches a duplicate or out-of-order entry at build time
// -- worth having now because this table is where every future feature lands.
constexpr std::array<std::pair<std::string_view, Server::RequestFn>, 5> kRequests{ {
  { "initialize", &Server::on_initialize },
  { "shutdown", &Server::on_shutdown },
  { "textDocument/semanticTokens/full", &Server::on_semantic_tokens_full },
  { "textDocument/semanticTokens/full/delta", &Server::on_semantic_tokens_delta },
  { "textDocument/semanticTokens/range", &Server::on_semantic_tokens_range },
} };

constexpr std::array<std::pair<std::string_view, Server::NotificationFn>, 7> kNotifications{ {
  { "$/cancelRequest", nullptr }, // Handled ahead of dispatch; listed so it is not "unknown".
  { "$/setTrace", &Server::on_set_trace },
  { "exit", &Server::on_exit },
  { "initialized", &Server::on_initialized },
  { "textDocument/didChange", &Server::on_did_change },
  { "textDocument/didClose", &Server::on_did_close },
  { "textDocument/didOpen", &Server::on_did_open },
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
static_assert(sorted_by_name(kRequests));
static_assert(sorted_by_name(kNotifications));

template<class Table>
auto find_method(const Table& table, std::string_view name) -> const typename Table::value_type* {
    using Entry   = typename Table::value_type;
    const auto it = std::ranges::lower_bound(table, name, {}, &Entry::first);
    if (it == table.end() || it->first != name) {
        return nullptr;
    }
    return &*it;
}

// stderr is the only place a server may talk out of band: stdout is the
// protocol.
template<class... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    std::println(stderr, "clotho lsp: {}", std::format(fmt, std::forward<Args>(args)...));
}

// -- I/O threads -------------------------------------------------------------

void Server::read_loop() {
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

void Server::write_loop() {
    FrameWriter writer(kStdoutFd);
    std::string body;
    while (outgoing_.pop(body)) {
        if (!writer.write(body)) {
            events_.push(Event{ .kind = Event::Kind::TransportError, .detail = "stdout closed" });
            return;
        }
    }
}

// The one place any real work happens. A single worker, not a pool: analyses
// are per-document and the loop already refuses to run two for the same
// document at once, so extra threads would only contend for the same answer.
// Nothing here can be interrupted -- neither slang nor Clotho's lowering has a
// cancellation point -- so a superseded analysis runs to completion and is
// discarded on arrival. That is the honest cost of not having the worker
// PROCESS of docs/design/lsp.md 4.3, where cancellation is kill(pid).
void Server::analysis_loop() {
    AnalysisJob job;
    while (jobs_.pop(job)) {
        auto result     = std::make_unique<AnalysisResult>();
        result->uri     = job.uri;
        result->version = job.version;

        const LowerResult lowered = lower_buffer(job.path, job.text);
        // One index over the job's own text, shared by both products. Holds a
        // view into `job.text`, which outlives it.
        const LineIndex lines(job.text, job.encoding);

        for (const Diagnostic& diagnostic : lowered.diagnostics) {
            // A diagnostic from an included file belongs to a document this
            // server was not asked about and does not track, so it cannot be
            // published (or, later, cleared) honestly.
            const std::string path = lowered.sources->full_path(diagnostic.span.start);
            if (diagnostic.span.valid() && path != job.path) {
                continue;
            }
            result->diagnostics.push_back(to_lsp(diagnostic, *lowered.sources, lines));
        }
        result->tokens = semantic_tokens(*lowered.sources, job.path, lines);

        events_.push(Event{ .kind = Event::Kind::Analysis, .analysis = std::move(result) });
    }
}

void Server::run() {
    configure_stdio();

    // Detached, and never joined. Both are parked in a blocking syscall for
    // most of their lives, and the process ends below rather than unwinding
    // past them -- so there is nothing to join and nothing to destroy. The
    // writer is the exception: it is joined, because its queue is the only
    // thing standing between a response and the client seeing it.
    std::thread([this] { read_loop(); }).detach();
    std::thread([this] { analysis_loop(); }).detach();
    std::thread writer([this] { write_loop(); });

    while (!done_) {
        // Anything whose debounce has expired goes out before the loop parks
        // again, and the next expiry bounds how long it parks for.
        dispatch_due_analyses();

        Event                        event;
        const auto                   deadline = next_deadline();
        typename Channel<Event>::Pop status =
          deadline ? events_.pop_until(event, *deadline)
                   : (events_.pop(event) ? Channel<Event>::Pop::Value : Channel<Event>::Pop::Closed);
        if (status == Channel<Event>::Pop::Closed) {
            break;
        }
        if (status == Channel<Event>::Pop::Timeout) {
            continue;
        }
        switch (event.kind) {
            case Event::Kind::Batch:
                handle_batch(event.bodies);
                break;
            case Event::Kind::Analysis:
                publish(*event.analysis);
                break;
            case Event::Kind::Eof:
                // The client vanished without saying goodbye. Same verdict as an
                // `exit` in the same position.
                log("client disconnected");
                finish(state_ == Lifecycle::ShuttingDown ? 0 : 1);
                break;
            case Event::Kind::TransportError:
                log("transport error: {}", event.detail);
                finish(1);
                break;
            case Event::Kind::ParentGone:
                log("client process exited");
                finish(1);
                break;
        }
    }

    outgoing_.close();
    writer.join();
    std::_Exit(exit_code_);
}

void Server::finish(int code) {
    exit_code_ = code;
    done_      = true;
}

// -- analysis scheduling -----------------------------------------------------

// How long a document must stop changing before it is worth analysing. Nothing
// about the frontend is incremental, so every keystroke would otherwise start a
// whole-file elaboration whose result the next keystroke invalidates.
constexpr auto kDebounce = std::chrono::milliseconds(300);

void Server::schedule_analysis(const std::string& uri, std::chrono::steady_clock::duration delay) {
    due_[uri] = std::chrono::steady_clock::now() + delay;
}

std::optional<std::chrono::steady_clock::time_point> Server::next_deadline() const {
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (const auto& [uri, when] : due_) {
        if (!earliest || when < *earliest) {
            earliest = when;
        }
    }
    return earliest;
}

void Server::dispatch_due_analyses() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = due_.begin(); it != due_.end();) {
        const auto document = documents_.find(it->first);
        if (document == documents_.end()) {
            it = due_.erase(it); // Closed before its debounce expired.
            continue;
        }
        // Holding the timer rather than queueing behind the running job keeps
        // the worker from doing work that is already known to be superseded.
        if (it->second > now || in_flight_.contains(it->first)) {
            ++it;
            continue;
        }

        jobs_.push(AnalysisJob{ .uri      = it->first,
                                .path     = document->second.path,
                                .text     = document->second.text,
                                .version  = document->second.version,
                                .encoding = encoding_ });
        in_flight_.insert(it->first);
        it = due_.erase(it);
    }
}

void Server::publish(AnalysisResult& result) {
    in_flight_.erase(result.uri);

    const auto document = documents_.find(result.uri);
    if (document == documents_.end()) {
        return; // Closed while the analysis ran; on_did_close already cleared it.
    }
    if (document->second.version != result.version) {
        // The buffer moved on. Nothing could stop the analysis mid-flight, so
        // the answer is simply dropped and the current text re-queued.
        if (trace_) {
            log("discarding diagnostics for {} at version {}", result.uri, result.version);
        }
        schedule_analysis(result.uri, std::chrono::milliseconds(0));
        return;
    }

    tokens_[result.uri] = Cache{ .version = result.version, .tokens = std::move(result.tokens) };

    notify("textDocument/publishDiagnostics",
           PublishDiagnosticsParams{
             .uri = result.uri, .version = result.version, .diagnostics = std::move(result.diagnostics) });

    // Diagnostics are pushed; semantic tokens are pulled, so a newer set is
    // invisible until the client is told to ask again. Without this the colours
    // freeze at whatever version was current the first time the editor asked.
    if (tokens_refresh_supported_) {
        request_refresh();
    }
}

// -- dispatch ----------------------------------------------------------------

void Server::handle_batch(std::vector<std::string>& bodies) {
    // `bodies` outlives this call's parsing, and nothing appends to it, so the
    // string_views the envelopes hold into it stay valid for the whole batch.
    batch_.clear();
    batch_.resize(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) {
        Staged& staged = batch_[i];
        staged.parsed  = !glz::read<kRead>(staged.env, bodies[i]);
    }

    apply_cancellations();

    for (const Staged& staged : batch_) {
        if (done_) {
            break;
        }
        dispatch(staged);
    }
}

void Server::apply_cancellations() {
    for (size_t i = 0; i < batch_.size(); ++i) {
        const Staged& cancel = batch_[i];
        if (!cancel.parsed || cancel.env.method != "$/cancelRequest") {
            continue;
        }
        CancelParams params;
        if (glz::read<kRead>(params, cancel.env.params.str)) {
            log("ignoring $/cancelRequest with unreadable params");
            continue;
        }

        // Only earlier messages: a cancellation names a request the client has
        // already sent. A request still in flight in a worker would be signalled
        // through its cancellation flag here instead -- nothing offloads yet, so
        // the queue is the only place a cancellable request can be.
        bool matched = false;
        for (size_t j = 0; j < i; ++j) {
            Staged& target = batch_[j];
            if (target.parsed && is_request(target.env.id) && target.env.id == params.id) {
                target.cancelled = true;
                matched          = true;
            }
        }
        if (!matched && trace_) {
            log("$/cancelRequest for a request that already completed");
        }
    }
}

void Server::dispatch(const Staged& staged) {
    if (!staged.parsed) {
        // The id is exactly what could not be read, so the response carries
        // null, per JSON-RPC 2.0.
        respond_error(Id{}, ErrorCode::ParseError, "message is not valid JSON");
        return;
    }

    const Envelope& env = staged.env;
    if (env.jsonrpc != kJsonRpcVersion) {
        if (is_request(env.id)) {
            respond_error(env.id, ErrorCode::InvalidRequest, "unsupported jsonrpc version");
        }
        return;
    }
    if (env.method.empty()) {
        // A response to a request this server sent. It sends none.
        log("ignoring a response to a request that was never made");
        return;
    }
    if (trace_) {
        log("<- {}", env.method);
    }

    if (!is_request(env.id)) {
        dispatch_notification(env.method, env.params.str);
        return;
    }
    if (staged.cancelled) {
        respond_error(env.id, ErrorCode::RequestCancelled, "cancelled by the client");
        return;
    }
    dispatch_request(env.id, env.method, env.params.str);
}

void Server::dispatch_request(const Id& id, std::string_view method, std::string_view params) {
    if (state_ == Lifecycle::Uninitialized && method != "initialize") {
        respond_error(id, ErrorCode::ServerNotInitialized, "expected 'initialize' first");
        return;
    }
    if (state_ != Lifecycle::Uninitialized && method == "initialize") {
        respond_error(id, ErrorCode::InvalidRequest, "already initialized");
        return;
    }
    if (state_ == Lifecycle::ShuttingDown) {
        respond_error(id, ErrorCode::InvalidRequest, "server is shutting down");
        return;
    }

    const auto* entry = find_method(kRequests, method);
    if (entry == nullptr) {
        respond_error(id, ErrorCode::MethodNotFound, std::format("no such request '{}'", method));
        return;
    }
    // A handler that offloads its work registers `id` here against a
    // cancellation flag, and clears it when the response is queued.
    (this->*entry->second)(id, params);
}

void Server::dispatch_notification(std::string_view method, std::string_view params) {
    // `exit` is the one notification that means something before the handshake
    // and after it: end, now.
    if (state_ == Lifecycle::Uninitialized && method != "exit") {
        log("dropping '{}' received before initialize", method);
        return;
    }

    const auto* entry = find_method(kNotifications, method);
    if (entry == nullptr) {
        // Notifications get no response, not even an error one (JSON-RPC 2.0).
        if (trace_) {
            log("no handler for notification '{}'", method);
        }
        return;
    }
    if (entry->second == nullptr) {
        return; // $/cancelRequest, already applied.
    }
    (this->*entry->second)(params);
}

// -- responses ---------------------------------------------------------------

void Server::send(std::string&& body) {
    outgoing_.push(std::move(body));
}

template<class Result>
void Server::respond(const Id& id, const Result& result) {
    std::string body;
    if (glz::write_json(ResultResponse<Result>{ .id = id, .result = result }, body)) {
        respond_error(id, ErrorCode::InternalError, "could not serialize the result");
        return;
    }
    send(std::move(body));
}

template<class Params>
void Server::notify(std::string_view method, const Params& params) {
    std::string body;
    if (glz::write_json(NotificationMessage<Params>{ .method = method, .params = params }, body)) {
        log("could not serialize a '{}' notification", method);
        return;
    }
    send(std::move(body));
}

void Server::respond_null(const Id& id) {
    // JSON-RPC requires the member to be present, so this is the one write that
    // must not elide a null.
    constexpr glz::opts kKeepNulls{ .skip_null_members = false };
    std::string         body;
    if (glz::write<kKeepNulls>(ResultResponse<std::monostate>{ .id = id, .result = {} }, body)) {
        respond_error(id, ErrorCode::InternalError, "could not serialize the result");
        return;
    }
    send(std::move(body));
}

void Server::respond_error(const Id& id, ErrorCode code, std::string_view message) {
    const ErrorResponse response{ .id = id, .error = { static_cast<int32_t>(code), message } };
    std::string         body;
    if (glz::write_json(response, body)) {
        // Nothing left to fall back to: the failure is in the encoder itself.
        log("could not serialize an error response");
        return;
    }
    send(std::move(body));
}

template<class Params>
bool Server::parse_params(std::string_view raw, Params& out, std::string_view method) {
    if (raw.empty()) {
        return false;
    }
    if (const auto ec = glz::read<kRead>(out, raw)) {
        log("could not read the params of '{}': {}", method, glz::format_error(ec, raw));
        return false;
    }
    return true;
}

// -- handlers ----------------------------------------------------------------

void Server::on_initialize(const Id& id, std::string_view params) {
    InitializeParams parsed;
    parse_params(params, parsed, "initialize");

    // UTF-8 when the client can read it, because it is the encoding the source
    // is already in and it makes every future span conversion a no-op. UTF-16
    // is the protocol's default and the fallback (docs/design/lsp.md 1.1 #6).
    if (parsed.capabilities && parsed.capabilities->general && parsed.capabilities->general->positionEncodings) {
        const auto& offered = *parsed.capabilities->general->positionEncodings;
        if (std::ranges::find(offered, "utf-8") != offered.end()) {
            encoding_ = Encoding::Utf8;
        }
    }
    if (parsed.capabilities && parsed.capabilities->workspace && parsed.capabilities->workspace->semanticTokens) {
        tokens_refresh_supported_ = parsed.capabilities->workspace->semanticTokens->refreshSupport.value_or(false);
    }

    // A client that dies without saying `exit` would otherwise leave this
    // process running forever, holding its pipes open.
    if (parsed.processId) {
        const int64_t pid = *parsed.processId;
        std::thread([this, pid] {
            while (process_alive(pid)) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            events_.push(Event{ .kind = Event::Kind::ParentGone });
        }).detach();
    }

    log("initialized by {} (position encoding {})",
        parsed.clientInfo ? std::string_view(parsed.clientInfo->name) : std::string_view("an unnamed client"),
        encoding_name(encoding_));

    state_ = Lifecycle::Initializing;
    respond(
      id,
      InitializeResult{
        .capabilities = { .positionEncoding       = encoding_name(encoding_),
                          .textDocumentSync       = { .openClose = true, .change = kSyncFull },
                          .semanticTokensProvider = { .legend = { .tokenTypes     = { token_type_legend().begin(),
                                                                                      token_type_legend().end() },
                                                                  .tokenModifiers = { token_modifier_legend().begin(),
                                                                                      token_modifier_legend()
                                                                                        .end() } } } },
        .serverInfo   = { .name = "clotho", .version = version_ },
      });
}

void Server::on_shutdown(const Id& id, std::string_view) {
    state_ = Lifecycle::ShuttingDown;
    respond_null(id);
}

void Server::on_initialized(std::string_view) {
    state_ = Lifecycle::Ready;
}

void Server::on_exit(std::string_view) {
    // Exiting without a preceding shutdown is an error the client wants to hear
    // about, and the exit status is the only channel left to say it in.
    finish(state_ == Lifecycle::ShuttingDown ? 0 : 1);
}

void Server::on_set_trace(std::string_view params) {
    TraceParams parsed;
    if (!parse_params(params, parsed, "$/setTrace")) {
        return;
    }
    trace_ = parsed.value != "off";
}

void Server::on_did_open(std::string_view params) {
    DidOpenParams parsed;
    if (!parse_params(params, parsed, "textDocument/didOpen")) {
        return;
    }
    const std::string path = path_from_uri(parsed.textDocument.uri);
    if (path.empty()) {
        log("ignoring {}: only file:// documents can be analysed", parsed.textDocument.uri);
        return;
    }

    documents_[parsed.textDocument.uri] =
      Document{ .path = path, .text = std::move(parsed.textDocument.text), .version = parsed.textDocument.version };
    // No debounce on open: the user is looking at the file right now, and there
    // is no edit in progress to wait out.
    schedule_analysis(parsed.textDocument.uri, std::chrono::milliseconds(0));
    log("open {} ({})", parsed.textDocument.uri, parsed.textDocument.languageId);
}

void Server::on_did_change(std::string_view params) {
    DidChangeParams parsed;
    if (!parse_params(params, parsed, "textDocument/didChange")) {
        return;
    }
    const auto document = documents_.find(parsed.textDocument.uri);
    if (document == documents_.end()) {
        log("change for {}, which was never opened", parsed.textDocument.uri);
        return;
    }
    if (parsed.contentChanges.empty()) {
        return;
    }

    // Full sync, which is what the server declared: the last change carries the
    // whole document.
    document->second.text    = std::move(parsed.contentChanges.back().text);
    document->second.version = parsed.textDocument.version;
    schedule_analysis(parsed.textDocument.uri, kDebounce);
}

void Server::on_did_close(std::string_view params) {
    DidCloseParams parsed;
    if (!parse_params(params, parsed, "textDocument/didClose")) {
        return;
    }
    documents_.erase(parsed.textDocument.uri);
    due_.erase(parsed.textDocument.uri);
    tokens_.erase(parsed.textDocument.uri);
    // The client discards diagnostics for a closed file only if the server says
    // so; leaving them behind pins stale entries in its problem list.
    notify("textDocument/publishDiagnostics",
           PublishDiagnosticsParams{ .uri = parsed.textDocument.uri, .version = 0, .diagnostics = {} });
    log("close {}", parsed.textDocument.uri);
}

// -- semantic tokens ---------------------------------------------------------

// A server-initiated request, and the only one this server makes. It carries no
// params and its result is null; the point is entirely the side effect of the
// client re-asking for tokens.
void Server::request_refresh() {
    std::string body;
    if (glz::write_json(RefreshRequest{ .id = next_request_id_++, .method = "workspace/semanticTokens/refresh" },
                        body)) {
        log("could not serialize a semanticTokens refresh");
        return;
    }
    send(std::move(body));
}

const Server::Cache* Server::cache_for(const std::string& uri) const {
    const auto it = tokens_.find(uri);
    return it == tokens_.end() ? nullptr : &it->second;
}

// Answers from the last completed analysis, whatever version that is.
//
// Returning tokens for an older version is not a compromise, it is the design.
// Colours from a stale analysis are still colours for text the user typed, and
// the alternative -- blocking until the current version elaborates -- puts a
// 65 ms parse on the dispatch thread and stalls every other message. When a
// newer analysis lands, `publish` sends `workspace/semanticTokens/refresh` and
// the client asks again. nvim tracks document versions and drops responses it
// has already superseded.
void Server::answer_tokens(const Id& id, const std::string& uri, std::string_view previous_result_id) {
    const Cache* cache = cache_for(uri);
    if (cache == nullptr) {
        // Nothing analysed yet. An empty array is a valid answer and the client
        // will ask again after the refresh; an error would make it stop asking.
        respond(id, SemanticTokens{ .resultId = "0", .data = {} });
        return;
    }

    const std::string result_id = std::to_string(cache->version);
    if (!previous_result_id.empty() && previous_result_id == result_id) {
        // Same analysis the client already has, so nothing changed.
        respond(id, SemanticTokensDelta{ .resultId = result_id, .edits = {} });
        return;
    }
    respond(id, SemanticTokens{ .resultId = result_id, .data = cache->tokens });
}

// `range` exists so opening a large file paints the visible screen before the
// whole token array is encoded. This build sends the full array and lets the
// client crop: the encoding is relative, so a genuine range response has to be
// re-based from the range's first token rather than sliced out of the full one,
// and at 2,865 tokens for 1,595 lines the cropping is not what costs anything.
// Declared and answered correctly, just not yet cheaper than `full`.
void Server::answer_tokens_range(const Id& id, const std::string& uri, const Range&) {
    const Cache* cache = cache_for(uri);
    respond(id,
            SemanticTokens{ .resultId = cache != nullptr ? std::to_string(cache->version) : "0",
                            .data     = cache != nullptr ? cache->tokens : std::vector<uint32_t>{} });
}

void Server::on_semantic_tokens_full(const Id& id, std::string_view params) {
    SemanticTokensParams parsed;
    if (!parse_params(params, parsed, "textDocument/semanticTokens/full")) {
        respond_error(id, ErrorCode::InvalidParams, "expected a textDocument");
        return;
    }
    answer_tokens(id, parsed.textDocument.uri, {});
}

void Server::on_semantic_tokens_delta(const Id& id, std::string_view params) {
    SemanticTokensDeltaParams parsed;
    if (!parse_params(params, parsed, "textDocument/semanticTokens/full/delta")) {
        respond_error(id, ErrorCode::InvalidParams, "expected a textDocument");
        return;
    }
    answer_tokens(id, parsed.textDocument.uri, parsed.previousResultId);
}

void Server::on_semantic_tokens_range(const Id& id, std::string_view params) {
    SemanticTokensRangeParams parsed;
    if (!parse_params(params, parsed, "textDocument/semanticTokens/range")) {
        respond_error(id, ErrorCode::InvalidParams, "expected a textDocument");
        return;
    }
    answer_tokens_range(id, parsed.textDocument.uri, parsed.range);
}

} // namespace

void serve(std::string_view version) {
    Server server(version);
    server.run();
}

} // namespace clotho::lsp
