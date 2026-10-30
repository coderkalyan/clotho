// The runtime's queue engine as LLVM bitcode, linked into the modules that call it.

#pragma once

#include <cstddef>
#include <span>

#include <llvm/IR/Module.h>

namespace clotho::ir {

// The bitcode image of src/runtime/Queue.cpp, compiled by clang when clotho is
// built and embedded in it (CMakeLists.txt, the generated RuntimeBitcodeImage.cpp).
std::span<const unsigned char> runtime_bitcode_image();

// Link into `m` the definitions this bitcode has of the rt_queue_* functions `m`
// declares, as internal functions, so the optimizer inlines them into the design and
// folds the element descriptor each call passes. Returns whether anything was
// linked; a module that calls none of them is not touched, and does not even parse
// the image.
//
// A bitcode that cannot be read or linked is a broken build, not bad input, so
// it stops the process.
bool link_runtime_bitcode(llvm::Module& m);

} // namespace clotho::ir
