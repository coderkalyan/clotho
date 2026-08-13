#pragma once

// The design under debug, as a child process.
//
// A child rather than a thread inside the adapter, for three reasons that all
// point the same way. The design's stdout is its own, so `$display` cannot
// interleave with the protocol on descriptor 1 and the adapter needs no
// descriptor games to keep them apart. A design that aborts -- and three paths
// in the runtime do exit outright (docs/design/dap.md 1.1 blocker 9) -- takes
// only itself down, which matters most for a debugger, because a design under
// debug is broken by definition. And `$finish` is free to leave the process in
// any state, which tests/Subprocess.h records the consequences of when it is
// not.
//
// The child is `clotho run` with the launch request's arguments passed through
// verbatim, so there is exactly one place that knows what a Clotho command
// line means (docs/design/cli.md: never a second parser, never a second help).

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace clotho::dap {

// Absolute path of the running `clotho` binary, so the adapter can spawn
// itself without path discovery (docs/design/lsp.md 11.5). Falls back to
// `argv0` when the platform has no answer.
std::string self_exe(std::string_view argv0);

class DesignProcess {
public:
    enum class Stream : uint8_t { Stdout, Stderr };

    // One read's worth of the child's output, tagged with the stream it came
    // from. Not line-buffered: DAP's `output` event carries arbitrary text and
    // holding a partial line back would delay the last line of a design that
    // ends without a newline.
    struct Chunk {
        Stream      stream = Stream::Stdout;
        std::string text;
    };

    DesignProcess()                                = default;
    DesignProcess(const DesignProcess&)            = delete;
    DesignProcess& operator=(const DesignProcess&) = delete;
    ~DesignProcess();

    // Spawns `exe` with `args` as argv[1..], both standard streams piped, in
    // `cwd` when it is non-empty. False means the child never started and
    // `error()` says why.
    bool start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd);

    // Blocks until the child writes something or both its pipes close. Clears
    // `out` first. False means the child is finished and reaped, after which
    // `exit_code()` is the answer and this must not be called again.
    bool read(std::vector<Chunk>& out);

    // Asks the child to stop (SIGTERM / TerminateProcess). Idempotent, and
    // safe from a thread other than the one in `read`: it only signals. The
    // reader then observes the pipes closing, which is what ends the session.
    void terminate();

    // Valid once `read` has returned false. A child killed by a signal reports
    // the negated signal number, which is what `clotho run` itself would have
    // exited with under a shell.
    int32_t exit_code() const {
        return exit_code_;
    }

    int64_t pid() const {
        return pid_;
    }

    std::string_view error() const {
        return error_;
    }

private:
    // Platform handles, kept as integers so this header names no OS type.
    // POSIX: file descriptors and a pid. Windows: HANDLEs and a process id.
    int64_t pid_       = -1;
    int64_t handle_    = -1;
    int64_t out_fd_    = -1;
    int64_t err_fd_    = -1;
    int32_t exit_code_ = -1;
    // Written by the thread in `read`, read by `terminate` on another, so it
    // is the one field here that is contended: signalling a pid the OS has
    // already handed back is how an unrelated process gets killed.
    std::atomic<bool> reaped_ = false;
    std::string       error_;
};

} // namespace clotho::dap
