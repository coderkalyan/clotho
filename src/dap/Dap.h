#pragma once

// `clotho dap`: the debug adapter's entry point.

#include <string_view>

namespace clotho::dap {

// Serves the Debug Adapter Protocol over stdin/stdout until the client
// disconnects, then ends the process (0 after a clean disconnect, 1
// otherwise).
//
// Does not return, for the same reason `lsp::serve` does not: the reader
// thread spends its life parked in a blocking read on stdin and no portable
// call wakes a thread out of that. The one thing this owns that the language
// server does not -- the design's child process -- is signalled before the
// adapter ends, and the OS reaps it either way.
//
// `version` is what the client is told in the `initialize` response.
// `argv0` seeds the path the design process is spawned from; the adapter
// prefers the platform's own answer and falls back to this.
[[noreturn]] void serve(std::string_view version, std::string_view argv0);

} // namespace clotho::dap
