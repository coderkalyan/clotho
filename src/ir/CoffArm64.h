#pragma once

// A pre-link rewrite of ARM64 COFF objects for the JIT, working around an LLVM 22
// RuntimeDyld defect. Windows on Arm is the only host whose JIT objects are ARM64
// COFF, and RuntimeDyld is the only linker LLVM has for them.

#include <memory>

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

namespace clotho::ir {

// Returns `obj` rewritten so that LLVM 22's RuntimeDyld links its
// IMAGE_REL_ARM64_PAGEOFFSET_12L relocations correctly, or unchanged when it is not
// an ARM64 COFF object or the LLVM in use links them correctly already.
//
// The addend of such a relocation is implicit: it is the LDR/STR imm12 field, which
// counts access-size units. RuntimeDyldCOFFAArch64 reads the field as bytes and then
// scales the resolved offset down when it writes the field back, so a load of
// `sym+0x430` as an 8-byte value reads `sym+0x80`. This stores the addend in bytes,
// which is what that reader expects and what it then scales correctly. An object
// MC produces encodes only the low 12 bits of the addend there, so the byte value
// always fits the field; one that does not is an error.
llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> fix_coff_arm64_ldr_addends(std::unique_ptr<llvm::MemoryBuffer> obj);

} // namespace clotho::ir
