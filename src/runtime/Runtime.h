#pragma once

// Clotho simulation runtime: native support routines linked into the JIT
// environment and called from JIT'd designs. Deliberately has no dependency on
// LLVM or the rest of the compiler — it is plain native code the design calls
// into. The interface is C so the codegen can reference it by a stable symbol.

extern "C" {

// Minimal smoke-test entry: prints a banner. Emitted as a call at the top of a
// design's `main()` to prove runtime linkage works end to end.
void rt_hello();

} // extern "C"
