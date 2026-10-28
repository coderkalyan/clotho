#include "link/Link.h"

#include <string>
#include <vector>

#include <lld/Common/Driver.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#if defined(__linux__)
#include <elf.h>
#include <link.h>
LLD_HAS_DRIVER(elf)
#elif defined(__APPLE__)
#include <dlfcn.h>

#include <algorithm>
#include <set>

#include <llvm/Object/Archive.h>
#include <llvm/Object/ObjectFile.h>
LLD_HAS_DRIVER(macho)
#elif defined(_WIN32)
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(mingw)
#endif

namespace clotho::link {

namespace {

std::string kit_path(const std::string& scratch, std::string_view name) {
    llvm::SmallString<256> path(scratch);
    llvm::sys::path::append(path, name);
    return std::string(path);
}

// Write every embedded file into `scratch`. False, with the reason on stderr,
// if one cannot be written.
bool extract_kit(const std::string& scratch) {
    for (size_t i = 0; i < kKitFileCount; ++i) {
        const KitFile&       file = kKitFiles[i];
        const std::string    path = kit_path(scratch, file.name);
        std::error_code      ec;
        llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            llvm::errs() << "clotho: cannot write '" << path << "': " << ec.message() << "\n";
            return false;
        }
        os.write(reinterpret_cast<const char*>(file.data), file.size);
    }
    return true;
}

bool is_shared_library(std::string_view lib) {
    return lib.ends_with(".so") || lib.find(".so.") != std::string_view::npos || lib.ends_with(".dylib");
}

// The directory a shared DPI library is in, as an absolute path: where the
// executable looks for it when it runs.
std::string library_dir(const std::string& lib) {
    llvm::SmallString<256> path(lib);
    llvm::sys::fs::make_absolute(path);
    return std::string(llvm::sys::path::parent_path(path));
}

bool run_lld(const std::vector<std::string>& args, const lld::DriverDef& driver) {
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    const lld::Result result = lld::lldMain(argv, llvm::outs(), llvm::errs(), { driver });
    return result.retCode == 0;
}

#if defined(__linux__)

// The dynamic linker and C library this process runs with. An executable is
// linked against the same ones, so it runs wherever clotho itself does.
struct SystemLibs {
    std::string interp;
    std::string libc;
    std::string libm;
};

int collect_system_libs(dl_phdr_info* info, size_t, void* data) {
    auto*                  libs = static_cast<SystemLibs*>(data);
    const std::string_view name = info->dlpi_name;
    if (name.empty()) {
        // The main program, whose PT_INTERP names the dynamic linker.
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
            if (info->dlpi_phdr[i].p_type == PT_INTERP) {
                libs->interp = reinterpret_cast<const char*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            }
        }
    } else if (name.ends_with("/libc.so.6")) {
        libs->libc = name;
    } else if (name.ends_with("/libm.so.6")) {
        libs->libm = name;
    }
    return 0;
}

// For what this process does not have loaded, as a statically linked clotho has
// none of them: the dynamic linker at the path the architecture's ELF ABI fixes,
// and libc.so.6 and libm.so.6 from the first standard library directory that
// has them, multiarch first. A library of the wrong architecture is lld's to
// refuse.
void find_system_libs_on_disk(SystemLibs& libs) {
#if defined(__x86_64__)
    constexpr const char* interp    = "/lib64/ld-linux-x86-64.so.2";
    constexpr const char* multiarch = "x86_64-linux-gnu";
#elif defined(__aarch64__)
    constexpr const char* interp    = "/lib/ld-linux-aarch64.so.1";
    constexpr const char* multiarch = "aarch64-linux-gnu";
#endif
    if (libs.interp.empty() && llvm::sys::fs::exists(interp)) {
        libs.interp = interp;
    }
    const std::string dirs[] = { std::string("/lib/") + multiarch,
                                 std::string("/usr/lib/") + multiarch,
                                 "/lib64",
                                 "/usr/lib64",
                                 "/lib",
                                 "/usr/lib" };
    for (const std::string& dir : dirs) {
        if (libs.libc.empty() && llvm::sys::fs::exists(dir + "/libc.so.6")) {
            libs.libc = dir + "/libc.so.6";
        }
        if (libs.libm.empty() && llvm::sys::fs::exists(dir + "/libm.so.6")) {
            libs.libm = dir + "/libm.so.6";
        }
    }
}

bool link_host(const std::string&           object,
               std::span<const std::string> dpi_libs,
               bool                         exports_dpi,
               const std::string&           output,
               const std::string&           scratch) {
#if defined(__x86_64__)
    constexpr const char* emulation = "elf_x86_64";
#elif defined(__aarch64__)
    constexpr const char* emulation = "aarch64linux";
#else
#error "clotho links executables for x86-64 and AArch64 Linux only"
#endif
    SystemLibs libs;
    dl_iterate_phdr(collect_system_libs, &libs);
    find_system_libs_on_disk(libs);
    if (libs.interp.empty() || libs.libc.empty() || libs.libm.empty()) {
        llvm::errs() << "clotho: cannot find the dynamic linker, libc.so.6 or libm.so.6 to link an executable "
                        "against, loaded in clotho or installed where glibc puts them\n";
        return false;
    }

    std::vector<std::string> args = {
        "ld.lld",
        "-m",
        emulation,
        "--hash-style=gnu",
        "--build-id",
        "--eh-frame-hdr",
        "-z",
        "relro",
        "-pie",
        "-dynamic-linker",
        libs.interp,
        "-o",
        output,
        kit_path(scratch, "Scrt1.o"),
        kit_path(scratch, "crti.o"),
        kit_path(scratch, "crtbegin.o"),
        object,
    };
    for (const std::string& lib : dpi_libs) {
        if (is_shared_library(lib)) {
            args.push_back("-rpath");
            args.push_back(library_dir(lib));
        }
        args.push_back(lib);
    }
    if (exports_dpi) {
        // A shared DPI library binds its calls into the design at load time,
        // against the executable's dynamic symbols.
        args.push_back("--export-dynamic");
    }
    for (const std::string& file : {
           kit_path(scratch, "runtime.a"),
           std::string("--push-state"),
           std::string("--as-needed"),
           libs.libm,
           std::string("--pop-state"),
           libs.libc,
           kit_path(scratch, "libc_nonshared.a"),
           kit_path(scratch, "builtins.a"),
           kit_path(scratch, "crtend.o"),
           kit_path(scratch, "crtn.o"),
         }) {
        args.push_back(file);
    }
    return run_lld(args, { lld::Gnu, &lld::elf::link });
}

#elif defined(__APPLE__)

// Add the symbols `path` defines and leaves undefined.
void collect_symbols(const std::string& path, std::set<std::string>& defined, std::set<std::string>& undefined) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer) {
        return;
    }
    const auto add = [&](const llvm::object::ObjectFile& object) {
        for (const llvm::object::SymbolRef& sym : object.symbols()) {
            auto flags = sym.getFlags();
            auto name  = sym.getName();
            if (!flags || !name || (*flags & llvm::object::SymbolRef::SF_Global) == 0) {
                llvm::consumeError(flags.takeError());
                llvm::consumeError(name.takeError());
                continue;
            }
            ((*flags & llvm::object::SymbolRef::SF_Undefined) != 0 ? undefined : defined).insert(name->str());
        }
    };
    auto binary = llvm::object::createBinary((*buffer)->getMemBufferRef());
    if (!binary) {
        llvm::consumeError(binary.takeError());
        return;
    }
    if (auto* archive = llvm::dyn_cast<llvm::object::Archive>(binary->get())) {
        llvm::Error err = llvm::Error::success();
        for (const auto& child : archive->children(err)) {
            auto member = child.getAsBinary();
            if (!member) {
                llvm::consumeError(member.takeError());
                continue;
            }
            if (auto* object = llvm::dyn_cast<llvm::object::ObjectFile>(member->get())) {
                add(*object);
            }
        }
        llvm::consumeError(std::move(err));
    } else if (auto* object = llvm::dyn_cast<llvm::object::ObjectFile>(binary->get())) {
        add(*object);
    }
}

// Whether libSystem, as this machine has it, defines the C symbol `name`.
bool in_libsystem(const std::string& name) {
    if (!name.starts_with("_")) {
        return false;
    }
    void*   address = dlsym(RTLD_DEFAULT, name.c_str() + 1);
    Dl_info info;
    if (address == nullptr || dladdr(address, &info) == 0 || info.dli_fname == nullptr) {
        return false;
    }
    const std::string_view image = info.dli_fname;
    return image.starts_with("/usr/lib/system/") || image.starts_with("/usr/lib/libSystem");
}

// A text stub for libSystem listing exactly the symbols the inputs need from it,
// written instead of taken from an SDK: the executable binds them to
// /usr/lib/libSystem.B.dylib, which re-exports them from its parts. A symbol
// that is in neither the inputs nor libSystem stays undefined, for lld to
// report.
bool write_libsystem_stub(const std::string& path, const char* arch, const std::vector<std::string>& inputs) {
    std::set<std::string> defined;
    std::set<std::string> undefined;
    for (const std::string& input : inputs) {
        collect_symbols(input, defined, undefined);
    }
    std::string symbols = "dyld_stub_binder";
    for (const std::string& name : undefined) {
        if (!defined.contains(name) && in_libsystem(name)) {
            symbols += ", ";
            symbols += name;
        }
    }
    std::error_code      ec;
    llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_Text);
    if (ec) {
        llvm::errs() << "clotho: cannot write '" << path << "': " << ec.message() << "\n";
        return false;
    }
    os << "--- !tapi-tbd\n"
       << "tbd-version: 4\n"
       << "targets: [ " << arch << "-macos ]\n"
       << "install-name: '/usr/lib/libSystem.B.dylib'\n"
       << "exports:\n"
       << "  - targets: [ " << arch << "-macos ]\n"
       << "    symbols: [ " << symbols << " ]\n"
       << "...\n";
    return true;
}

bool link_host(const std::string&           object,
               std::span<const std::string> dpi_libs,
               bool                         exports_dpi,
               const std::string&           output,
               const std::string&           scratch) {
#if defined(__aarch64__)
    constexpr const char* arch = "arm64";
#else
    constexpr const char* arch = "x86_64";
#endif
    // The oldest macOS clotho itself runs on, which the executable then needs.
    const unsigned    min     = __ENVIRONMENT_OS_VERSION_MIN_REQUIRED__;
    const std::string version = std::to_string(min / 10000) + "." + std::to_string(min / 100 % 100);

    std::vector<std::string> inputs = { object };
    for (const std::string& lib : dpi_libs) {
        inputs.push_back(lib);
    }
    inputs.push_back(kit_path(scratch, "runtime.a"));
    inputs.push_back(kit_path(scratch, "builtins.a"));
    const std::string libsystem = kit_path(scratch, "libSystem.tbd");
    if (!write_libsystem_stub(libsystem, arch, inputs)) {
        return false;
    }

    std::vector<std::string> args = { "ld64.lld", "-arch", arch,  "-platform_version", "macos", version,
                                      version,    "-o",    output };
    for (const std::string& lib : dpi_libs) {
        if (is_shared_library(lib)) {
            args.push_back("-rpath");
            args.push_back(library_dir(lib));
        }
    }
    if (exports_dpi) {
        args.push_back("-export_dynamic");
    }
    args.insert(args.end(), inputs.begin(), inputs.end());
    args.push_back(libsystem);
    return run_lld(args, { lld::Darwin, &lld::macho::link });
}

#elif defined(_WIN32)

bool link_host(const std::string&           object,
               std::span<const std::string> dpi_libs,
               bool                         exports_dpi,
               const std::string&           output,
               const std::string&           scratch) {
#if defined(__aarch64__)
    constexpr const char* emulation = "arm64pe";
#else
    constexpr const char* emulation = "i386pep";
#endif
    std::vector<std::string> args = {
        "ld.lld",
        "-m",
        emulation,
        "-Bdynamic",
        "-o",
        output,
        kit_path(scratch, "crt2.o"),
        kit_path(scratch, "crtbegin.o"),
        object,
    };
    for (const std::string& lib : dpi_libs) {
        // A DLL resolves its imports when it is built, so it cannot call a
        // function the executable defines; an import library stands in for one.
        const bool dll = lib.ends_with(".dll") || lib.ends_with(".dll.a");
        if (dll && exports_dpi) {
            llvm::errs() << "clotho: '" << lib
                         << "' is a DLL, but this design has export \"DPI-C\" functions, which a DLL cannot "
                            "call; give --dpi-lib an object or a static archive\n";
            return false;
        }
        if (lib.ends_with(".dll")) {
            llvm::errs() << "clotho: '" << lib << "' is a DLL; give --dpi-lib its import library (.dll.a)\n";
            return false;
        }
        args.push_back(lib);
    }
    for (const char* name : { "runtime.a",
                              "libmingw32.a",
                              "builtins.a",
                              "libmoldname.a",
                              "libmingwex.a",
                              "libmsvcrt.a",
                              "libadvapi32.a",
                              "libshell32.a",
                              "libuser32.a",
                              "libkernel32.a",
                              "libpsapi.a",
                              "crtend.o" }) {
        args.push_back(kit_path(scratch, name));
    }
    return run_lld(args, { lld::MinGW, &lld::mingw::link });
}

#endif

} // namespace

bool link_executable(const std::string&           object,
                     std::span<const std::string> dpi_libs,
                     bool                         exports_dpi,
                     const std::string&           output,
                     const std::string&           scratch) {
    return extract_kit(scratch) && link_host(object, dpi_libs, exports_dpi, output, scratch);
}

} // namespace clotho::link
