// The predicate behind the encoding assertions (docs/design/kernel.md, Values), kept
// apart from Backend so a test can fold it over constants.

#pragma once

#include <llvm/IR/IRBuilder.h>

namespace clotho::ir::backend {

// i1: whether a 4-state value breaks the encoding's invariants. `mm` is the packed
// i(2W) must|may word, `z` the iW z plane. The invariants are
//   must & ~may == 0                 a definite 1 is a possible 1
//   z & ~(may & ~must) == 0          z is only ever set on an unknown bit
// Bits above the width cannot exist in an LLVM integer of exactly the width, so the
// third invariant of the encoding holds by construction in generated code.
inline llvm::Value* encoding_violated(llvm::IRBuilder<>& b, llvm::Value* mm, llvm::Value* z) {
    llvm::Type*    iw      = z->getType();
    const unsigned w       = iw->getIntegerBitWidth();
    llvm::Value*   must    = b.CreateTrunc(mm, iw);
    llvm::Value*   may     = b.CreateTrunc(b.CreateLShr(mm, w), iw);
    llvm::Value*   unknown = b.CreateAnd(may, b.CreateNot(must));
    llvm::Value*   bad     = b.CreateOr(b.CreateAnd(must, b.CreateNot(may)),
                                        b.CreateAnd(z, b.CreateNot(unknown)));
    return b.CreateICmpNE(bad, llvm::ConstantInt::get(iw, 0));
}

} // namespace clotho::ir::backend
