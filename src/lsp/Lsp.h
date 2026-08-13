#pragma once

// `clotho lsp`: the language server's entry point.

#include <string_view>

namespace clotho::lsp {

// Serves the LSP over stdin/stdout until the client sends `exit` or
// disconnects, then ends the process with the exit status the protocol
// prescribes (0 after a clean shutdown, 1 otherwise).
//
// Does not return. The reader thread spends its life parked in a blocking read
// on stdin, and no portable call wakes a thread out of that -- so rather than
// pretend to unwind past it, the server finishes flushing its output and ends
// the process. There is nothing to unwind: the server owns no files, no
// external processes and no state that outlives it.
//
// `version` is what the client is told in `serverInfo`.
[[noreturn]] void serve(std::string_view version);

} // namespace clotho::lsp
