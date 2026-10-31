// The runtime's queue engine as LLVM bitcode, linked into the modules that call it.

#pragma once

#include <cstddef>
#include <span>

#include <llvm/IR/Module.h>

namespace clotho::ir {

// The bitcode image of src/runtime/Queue.cpp, compiled by clang when clotho is
// built and embedded in it (CMakeLists.txt, the generated RuntimeBitcodeImage.cpp).
// Empty when this build has none: the toolchain that builds clotho could not make
// it, or CLOTHO_RT_BITCODE was turned off. Designs then call the runtime's own
// rt_queue_* functions, which are the same code and take the same descriptors.
std::span<const unsigned char> runtime_bitcode_image();

// Whether this build has the runtime bitcode to link.
inline bool runtime_bitcode_available() {
    return !runtime_bitcode_image().empty();
}

// Link into `m` the engine functions it calls, as internal functions, so the
// optimizer sees them with the element descriptor each call passes as a constant:
//
//   - push and pop are marked always-inline, so every call site gets the ring
//     arithmetic and the element kernel's direct call;
//   - insert, delete and append_range (loops that move elements) are copied once per
//     element descriptor (once per stride for plain bytes), with the descriptor folded
//     in, and left out of line: the stride is then a constant in every loop, and a
//     design with many call sites or many element types pays for a few copies, not
//     one per site or per type;
//   - growing the ring and dropping a bounded queue's overflow stay calls into the
//     runtime.
//
// Only the calls that may run more than once are served. The backend marks a call cold
// (Backend::queue_engine_call) when it is outside every loop of a process that starts
// once and ends; it runs at most once, inlining or copying the engine there is compile
// time that never pays for itself, and it stays a call of the runtime's function.
//
// Returns whether anything was linked. A module with no call that may run more than
// once is not touched, and does not even parse the image, and neither does a build
// without one. An image that cannot be read or linked is a broken build, not bad
// input, so it stops the process.
bool link_runtime_bitcode(llvm::Module& m);

} // namespace clotho::ir
