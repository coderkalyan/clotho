#include "CoffArm64.h"

#include <cstring>
#include <string>

#include <llvm/BinaryFormat/COFF.h>
#include <llvm/BinaryFormat/Magic.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Object/COFF.h>
#include <llvm/Support/Endian.h>

namespace clotho::ir {

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> fix_coff_arm64_ldr_addends(std::unique_ptr<llvm::MemoryBuffer> obj) {
#if LLVM_VERSION_MAJOR >= 23
    // NOTE: LLVM 23 reads the addend in bytes itself
    // (https://github.com/llvm/llvm-project/commit/811ba37d61f4); rewriting it here
    // too would scale it twice. Delete this file once LLVM 22 is no longer supported.
    return obj;
#else
    if (llvm::identify_magic(obj->getBuffer()) != llvm::file_magic::coff_object) {
        return obj;
    }
    auto coff = llvm::object::COFFObjectFile::create(obj->getMemBufferRef());
    if (!coff) {
        return coff.takeError();
    }
    if ((*coff)->getMachine() != llvm::COFF::IMAGE_FILE_MACHINE_ARM64) {
        return obj;
    }

    // Copied on the first rewrite; most objects have none.
    std::unique_ptr<llvm::WritableMemoryBuffer> out;
    for (const llvm::object::SectionRef& section : (*coff)->sections()) {
        const llvm::object::coff_section* s = (*coff)->getCOFFSection(section);
        for (const llvm::object::coff_relocation& r : (*coff)->getRelocations(s)) {
            if (r.Type != llvm::COFF::IMAGE_REL_ARM64_PAGEOFFSET_12L) {
                continue;
            }
            const uint64_t at = uint64_t{ s->PointerToRawData } + r.VirtualAddress - s->VirtualAddress;
            if (r.VirtualAddress < s->VirtualAddress || at + 4 > obj->getBufferSize()) {
                return llvm::createStringError("ARM64 COFF relocation outside its section's data");
            }
            const char* src = out != nullptr ? out->getBufferStart() : obj->getBufferStart();
            const uint32_t inst = llvm::support::endian::read32le(src + at);
            // The access size, as RuntimeDyldCOFFAArch64's write32AArch64Ldr derives it:
            // bits 31:30, plus 4 for a 128-bit SIMD/FP access.
            uint32_t size = inst >> 30;
            if ((inst & 0x04800000) == 0x04800000) {
                size += 4;
            }
            const uint32_t imm = (inst >> 10) & 0xFFF;
            if (size == 0 || imm == 0) {
                continue;
            }
            const uint64_t bytes = uint64_t{ imm } << size;
            if (bytes > 0xFFF) {
                return llvm::createStringError("ARM64 COFF PAGEOFFSET_12L addend of " + std::to_string(bytes) +
                                               " bytes does not fit the byte-unit field LLVM 22's RuntimeDyld reads");
            }
            if (out == nullptr) {
                out = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(obj->getBufferSize(), obj->getBufferIdentifier());
                std::memcpy(out->getBufferStart(), obj->getBufferStart(), obj->getBufferSize());
            }
            const uint32_t rewritten = (inst & ~(0xFFFu << 10)) | (static_cast<uint32_t>(bytes) << 10);
            llvm::support::endian::write32le(out->getBufferStart() + at, rewritten);
        }
    }
    if (out == nullptr) {
        return obj;
    }
    return std::unique_ptr<llvm::MemoryBuffer>(std::move(out));
#endif
}

} // namespace clotho::ir
