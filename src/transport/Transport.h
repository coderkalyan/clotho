#pragma once

// The framing layer shared by the LSP and DAP base protocols:
// `Content-Length: N\r\n\r\n<N bytes>`, over raw file descriptors.
//
// Both protocols frame identically, and this layer is why: it knows about byte
// counts and descriptors, and nothing else. Headers other than Content-Length
// are skipped rather than rejected, so DAP's optional Content-Type costs
// nothing.
//
// Raw descriptors rather than iostreams, deliberately. stdout carries the
// protocol and nothing else, so it must not share a buffer with anything that
// might print, must not be reordered by a stream's own flush policy, and must
// not pay for the locale machinery on every message. The same argument applies
// to stdin, plus one more: a reader has to see byte counts exactly as the OS
// delivered them to batch correctly (see FrameReader).
//
// Nothing here knows what JSON is. Framing errors and protocol errors have
// different recovery stories -- a bad frame means the byte stream is
// desynchronized and the connection is over, while a bad message just means one
// error response -- so they are separate layers.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace clotho::transport {

// Reads base-protocol frames from a descriptor.
//
// `read` returns every frame that one `read(2)` completed, not just the first.
// That grouping is load bearing rather than an optimization: an editor that
// sends a request and then abandons it writes the request and its
// `$/cancelRequest` back to back, and only a server that sees them together can
// decline to do the work. Splitting the burst into independently delivered
// messages would throw that away before the dispatcher ever saw it.
class FrameReader {
public:
    enum class Status : uint8_t {
        // At least one frame was decoded into `out`.
        Ok,
        // The peer closed the stream at a frame boundary.
        Eof,
        // The stream is desynchronized or the descriptor failed; see `error`.
        Error,
    };

    explicit FrameReader(int fd)
      : fd_(fd) {}

    // Blocks until at least one frame completes, the peer disconnects, or the
    // stream breaks. Clears `out` first.
    Status read(std::vector<std::string>& out);

    std::string_view error() const {
        return error_;
    }

private:
    enum class Step : uint8_t { Frame, Incomplete, Error };

    Step extract(std::vector<std::string>& out);

    int         fd_;
    std::string buf_;
    // Bytes of `buf_` already handed out. Reclaimed by compaction rather than by
    // erasing on every frame, so a burst of small messages does not memmove the
    // tail once per message.
    size_t      head_ = 0;
    std::string error_;
};

// Writes base-protocol frames to a descriptor, one `write(2)` per message so a
// frame cannot interleave with another thread's.
class FrameWriter {
public:
    explicit FrameWriter(int fd)
      : fd_(fd) {}

    // Blocks until the whole frame is out. False means the peer is gone.
    bool write(std::string_view body);

private:
    int         fd_;
    std::string buf_;
};

// Puts the standard descriptors into binary mode. A no-op outside Windows,
// where the default text mode rewrites \n as \r\n on the way out and would
// corrupt every Content-Length the server sends.
void configure_stdio();

// The descriptors the server speaks on. Named because 0 and 1 appearing bare in
// thread bodies read as magic numbers.
inline constexpr int kStdinFd  = 0;
inline constexpr int kStdoutFd = 1;

} // namespace clotho::transport
