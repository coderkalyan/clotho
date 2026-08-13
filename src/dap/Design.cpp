#include "dap/Design.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace clotho::dap {

namespace {

// One read(2) worth. The design's output is the payload of an `output` event,
// and an event per 4 KiB of a chatty testbench is already more than a client
// renders; larger chunks are cheaper on both sides.
constexpr size_t kChunk = 16 * 1024;

} // namespace

#if !defined(_WIN32)

namespace {

// Reads whatever is ready on `fd` into `out`. Returns false at EOF or on a
// broken descriptor, which the caller turns into "this stream is finished".
bool drain(int fd, DesignProcess::Stream stream, std::vector<DesignProcess::Chunk>& out) {
    std::array<char, kChunk> buf{};
    for (;;) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
            out.push_back({ .stream = stream, .text = std::string(buf.data(), static_cast<size_t>(n)) });
            // One read per wakeup. A design writing faster than the client
            // renders would otherwise keep this loop from ever returning, and
            // the poll below will say immediately that there is more.
            return true;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

} // namespace

std::string self_exe(std::string_view argv0) {
    std::array<char, 4096> buf{};
#if defined(__APPLE__)
    uint32_t size = static_cast<uint32_t>(buf.size());
    if (::_NSGetExecutablePath(buf.data(), &size) == 0) {
        return std::string(buf.data());
    }
#else
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n > 0) {
        return std::string(buf.data(), static_cast<size_t>(n));
    }
#endif
    // Neither answer is available, so the client's own argv[0] is all there
    // is. It resolves whenever the adapter was started by an absolute path,
    // which is how every editor spawns one.
    return std::string(argv0);
}

DesignProcess::~DesignProcess() {
    if (out_fd_ >= 0) {
        ::close(static_cast<int>(out_fd_));
    }
    if (err_fd_ >= 0) {
        ::close(static_cast<int>(err_fd_));
    }
}

bool DesignProcess::start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd) {
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        error_ = std::format("could not create a pipe for the design's output: {}", std::strerror(errno));
        return false;
    }

    // Built before the fork: everything between fork and exec must be
    // async-signal-safe, and allocating an argv there is not.
    std::vector<char*> argv;
    argv.reserve(args.size() + 3);
    std::string exe_copy = exe;
    std::string run_word = "run";
    argv.push_back(exe_copy.data());
    argv.push_back(run_word.data());
    std::vector<std::string> owned = args;
    for (std::string& arg : owned) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        error_ = std::format("could not start the design: {}", std::strerror(errno));
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return false;
    }
    if (child == 0) {
        // The child never returns from here: any failure is an immediate
        // _exit, because unwinding back into the adapter's own code with two
        // copies of its state would be worse than a lost error message.
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            ::_exit(127);
        }
        ::execv(exe_copy.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    pid_    = child;
    out_fd_ = out_pipe[0];
    err_fd_ = err_pipe[0];
    return true;
}

bool DesignProcess::read(std::vector<Chunk>& out) {
    out.clear();
    while (out_fd_ >= 0 || err_fd_ >= 0) {
        std::array<pollfd, 2> fds{};
        nfds_t                count = 0;
        const int             out_i = out_fd_ >= 0 ? static_cast<int>(count++) : -1;
        if (out_i >= 0) {
            fds[static_cast<size_t>(out_i)] = { .fd = static_cast<int>(out_fd_), .events = POLLIN, .revents = 0 };
        }
        const int err_i = err_fd_ >= 0 ? static_cast<int>(count++) : -1;
        if (err_i >= 0) {
            fds[static_cast<size_t>(err_i)] = { .fd = static_cast<int>(err_fd_), .events = POLLIN, .revents = 0 };
        }

        if (::poll(fds.data(), count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_ = std::format("lost the design's output: {}", std::strerror(errno));
            break;
        }

        const auto service = [&](int index, int64_t& fd, Stream stream) {
            if (index < 0 || fds[static_cast<size_t>(index)].revents == 0) {
                return;
            }
            if (!drain(static_cast<int>(fd), stream, out)) {
                ::close(static_cast<int>(fd));
                fd = -1;
            }
        };
        service(out_i, out_fd_, Stream::Stdout);
        service(err_i, err_fd_, Stream::Stderr);
        if (!out.empty()) {
            return true;
        }
    }

    if (!reaped_) {
        reaped_    = true;
        int status = 0;
        while (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0 && errno == EINTR) {
        }
        if (WIFEXITED(status)) {
            exit_code_ = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_code_ = -WTERMSIG(status);
        }
    }
    return false;
}

void DesignProcess::terminate() {
    if (pid_ > 0 && !reaped_) {
        ::kill(static_cast<pid_t>(pid_), SIGTERM);
    }
}

#else

namespace {

// Windows quoting, per CommandLineToArgvW: a run of backslashes is literal
// unless it precedes a quote, in which case it is doubled.
void append_quoted(std::string& cmd, std::string_view arg) {
    cmd.push_back('"');
    size_t slashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') {
            cmd.append(slashes * 2 + 1, '\\');
        } else {
            cmd.append(slashes, '\\');
        }
        slashes = 0;
        cmd.push_back(c);
    }
    cmd.append(slashes * 2, '\\');
    cmd.push_back('"');
}

std::string last_error() {
    return std::format("windows error {}", static_cast<uint32_t>(::GetLastError()));
}

} // namespace

std::string self_exe(std::string_view argv0) {
    std::array<char, 4096> buf{};
    const DWORD            n = ::GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size()) {
        return std::string(buf.data(), n);
    }
    return std::string(argv0);
}

DesignProcess::~DesignProcess() {
    const auto shut = [](int64_t& h) {
        if (h != -1) {
            ::CloseHandle(reinterpret_cast<HANDLE>(h));
            h = -1;
        }
    };
    shut(out_fd_);
    shut(err_fd_);
    shut(handle_);
}

bool DesignProcess::start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd) {
    SECURITY_ATTRIBUTES sa{ .nLength              = sizeof(SECURITY_ATTRIBUTES),
                            .lpSecurityDescriptor = nullptr,
                            .bInheritHandle       = TRUE };
    HANDLE              out_r = nullptr;
    HANDLE              out_w = nullptr;
    HANDLE              err_r = nullptr;
    HANDLE              err_w = nullptr;
    if (::CreatePipe(&out_r, &out_w, &sa, 0) == 0 || ::CreatePipe(&err_r, &err_w, &sa, 0) == 0) {
        error_ = std::format("could not create a pipe for the design's output: {}", last_error());
        return false;
    }
    ::SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    std::string cmd;
    append_quoted(cmd, exe);
    cmd += " run";
    for (const std::string& arg : args) {
        cmd.push_back(' ');
        append_quoted(cmd, arg);
    }

    STARTUPINFOA startup{ .cb = sizeof(STARTUPINFOA) };
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = out_w;
    startup.hStdError  = err_w;
    PROCESS_INFORMATION info{};
    const BOOL          ok = ::CreateProcessA(exe.c_str(),
                                              cmd.data(),
                                              nullptr,
                                              nullptr,
                                              TRUE,
                                              0,
                                              nullptr,
                                              cwd.empty() ? nullptr : cwd.c_str(),
                                              &startup,
                                              &info);
    ::CloseHandle(out_w);
    ::CloseHandle(err_w);
    if (ok == 0) {
        error_ = std::format("could not start the design: {}", last_error());
        ::CloseHandle(out_r);
        ::CloseHandle(err_r);
        return false;
    }
    ::CloseHandle(info.hThread);
    pid_    = info.dwProcessId;
    handle_ = reinterpret_cast<int64_t>(info.hProcess);
    out_fd_ = reinterpret_cast<int64_t>(out_r);
    err_fd_ = reinterpret_cast<int64_t>(err_r);
    return true;
}

bool DesignProcess::read(std::vector<Chunk>& out) {
    // Blocking ReadFile on two pipes needs two threads or overlapped I/O; a
    // poll of the peek is what keeps this one function and one thread. The
    // interval only bounds latency on a silent design, never throughput.
    out.clear();
    while (out_fd_ != -1 || err_fd_ != -1) {
        bool       any  = false;
        const auto pump = [&](int64_t& h, Stream stream) {
            if (h == -1) {
                return;
            }
            auto* const handle    = reinterpret_cast<HANDLE>(h);
            DWORD       available = 0;
            if (::PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) == 0) {
                ::CloseHandle(handle);
                h = -1;
                return;
            }
            if (available == 0) {
                return;
            }
            std::string text(std::min<size_t>(available, kChunk), '\0');
            DWORD       got = 0;
            if (::ReadFile(handle, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) == 0 || got == 0) {
                ::CloseHandle(handle);
                h = -1;
                return;
            }
            text.resize(got);
            out.push_back({ .stream = stream, .text = std::move(text) });
            any = true;
        };
        pump(out_fd_, Stream::Stdout);
        pump(err_fd_, Stream::Stderr);
        if (any) {
            return true;
        }
        ::Sleep(10);
    }

    if (!reaped_) {
        reaped_            = true;
        auto* const handle = reinterpret_cast<HANDLE>(handle_);
        ::WaitForSingleObject(handle, INFINITE);
        DWORD code = 0;
        if (::GetExitCodeProcess(handle, &code) != 0) {
            exit_code_ = static_cast<int32_t>(code);
        }
    }
    return false;
}

void DesignProcess::terminate() {
    if (handle_ != -1 && !reaped_) {
        ::TerminateProcess(reinterpret_cast<HANDLE>(handle_), 1);
    }
}

#endif

} // namespace clotho::dap
