#include "transport/Transport.h"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace clotho::transport {

namespace {

// One read syscall's worth. Sized so an ordinary editor burst -- a didChange
// plus the requests that follow it -- lands in a single read, which is what
// makes the batching in FrameReader effective.
constexpr size_t kChunk = 64 * 1024;

// A header block larger than this is not a slow client, it is a desynchronized
// stream that will never produce a blank line.
constexpr size_t kMaxHeaderBytes = 8 * 1024;

// Refuse absurd bodies rather than trying to allocate them. The largest thing a
// real client sends is a didOpen of a whole file.
constexpr size_t kMaxBodyBytes = 64 * 1024 * 1024;

#if defined(_WIN32)
int read_fd(int fd, char* buf, size_t count) {
    return ::_read(fd, buf, static_cast<unsigned>(count));
}
int write_fd(int fd, const char* buf, size_t count) {
    return ::_write(fd, buf, static_cast<unsigned>(count));
}
#else
ptrdiff_t read_fd(int fd, char* buf, size_t count) {
    return ::read(fd, buf, count);
}
ptrdiff_t write_fd(int fd, const char* buf, size_t count) {
    return ::write(fd, buf, count);
}
#endif

bool equal_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (ca != b[i]) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

FrameReader::Step FrameReader::extract(std::vector<std::string>& out) {
    const std::string_view region = std::string_view(buf_).substr(head_);

    // Headers are scanned line by line rather than by searching for "\r\n\r\n",
    // which costs nothing extra and accepts bare \n as a line ending. The spec
    // says \r\n; every serious client sends \r\n; being strict about it buys
    // nothing and makes a hand-written transcript unusable.
    size_t pos            = 0;
    size_t content_length = 0;
    bool   have_length    = false;
    for (;;) {
        const size_t nl = region.find('\n', pos);
        if (nl == std::string_view::npos) {
            if (region.size() - pos > kMaxHeaderBytes) {
                error_ = "header block with no terminator";
                return Step::Error;
            }
            return Step::Incomplete;
        }
        const std::string_view line = trim(region.substr(pos, nl - pos));
        pos                         = nl + 1;
        if (line.empty()) {
            break;
        }

        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            error_ = std::format("malformed header line '{}'", line);
            return Step::Error;
        }
        if (!equal_ignore_case(trim(line.substr(0, colon)), "content-length")) {
            continue; // Content-Type and any client extension header.
        }
        const std::string_view value = trim(line.substr(colon + 1));
        const auto [end, ec]         = std::from_chars(value.data(), value.data() + value.size(), content_length);
        if (ec != std::errc{} || end != value.data() + value.size()) {
            error_ = std::format("bad Content-Length '{}'", value);
            return Step::Error;
        }
        have_length = true;
    }

    if (!have_length) {
        error_ = "message with no Content-Length";
        return Step::Error;
    }
    if (content_length > kMaxBodyBytes) {
        error_ = std::format("Content-Length {} exceeds the {} byte limit", content_length, kMaxBodyBytes);
        return Step::Error;
    }
    if (region.size() - pos < content_length) {
        return Step::Incomplete;
    }

    out.emplace_back(region.substr(pos, content_length));
    head_ += pos + content_length;
    return Step::Frame;
}

FrameReader::Status FrameReader::read(std::vector<std::string>& out) {
    out.clear();
    for (;;) {
        for (;;) {
            const Step step = extract(out);
            if (step == Step::Frame) {
                continue;
            }
            if (step == Step::Error) {
                return Status::Error;
            }
            break;
        }
        if (!out.empty()) {
            return Status::Ok;
        }

        // Drop what has been consumed before growing, so a long session reuses
        // one allocation instead of ratcheting the buffer up forever.
        if (head_ == buf_.size()) {
            buf_.clear();
            head_ = 0;
            if (buf_.capacity() > 4 * kChunk) {
                buf_.shrink_to_fit();
            }
        } else if (head_ > kChunk) {
            buf_.erase(0, head_);
            head_ = 0;
        }

        const size_t filled = buf_.size();
        buf_.resize(filled + kChunk);
        const auto n = read_fd(fd_, buf_.data() + filled, kChunk);
        if (n < 0) {
            buf_.resize(filled);
            if (errno == EINTR) {
                continue;
            }
            error_ = std::format("read failed: {}", std::strerror(errno));
            return Status::Error;
        }
        buf_.resize(filled + static_cast<size_t>(n));
        if (n == 0) {
            if (buf_.size() != head_) {
                error_ = "stream ended mid-message";
                return Status::Error;
            }
            return Status::Eof;
        }
    }
}

bool FrameWriter::write(std::string_view body) {
    buf_.clear();
    std::format_to(std::back_inserter(buf_), "Content-Length: {}\r\n\r\n", body.size());
    buf_.append(body);

    size_t off = 0;
    while (off < buf_.size()) {
        const auto n = write_fd(fd_, buf_.data() + off, buf_.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

void configure_stdio() {
#if defined(_WIN32)
    ::_setmode(kStdinFd, _O_BINARY);
    ::_setmode(kStdoutFd, _O_BINARY);
#endif
}

} // namespace clotho::transport
