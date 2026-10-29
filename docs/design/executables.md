# Executables: `clotho build`

Status: **landed** for the machine clotho runs on (Linux x86-64 and AArch64
with glibc, macOS, Windows through MinGW-w64). Cross-compilation is not built.

`clotho build design.sv` writes an executable that simulates the design. It
needs nothing installed where it runs beyond the C library, and building it
needs nothing beyond clotho: no compiler, linker or C library development
files. That is the point of it: a CI job builds once and runs the result, or
copies it to any machine like the one it was built on.

```
design.sv --(front end, backend)--> design.o --(lld, in-process)--> executable
                                                ^
                     embedded in clotho: executable runtime, compiler-rt,
                     the C library's startup objects and static parts
```

## One run, two hosts

`run` and an executable must simulate identically, so everything a host does
around the design goes through the runtime:

- `rt_run(config, design_main)` (Runtime.h) arms a run (plusargs, `$system`
  tier, `--loose-stale-ref`, `--stop-time`), calls the design and returns the
  exit status. The JIT and the executable's `main` (ExeMain.cpp) both call it.
- The design's entry is `clotho_main`, not `main`: the executable's `main` is
  the runtime's.
- The backend emits `clotho_design` (`rt_design`) beside it: the tick, the
  `--allow-system` tier the design was compiled under, and the top-level name.
  Those are fixed when the design is compiled, so an executable cannot be asked
  for a different `$system` tier than its code was generated for.
- `--stop-time` is parsed by the runtime (`rt_parse_stop_time`) for both.

The differential test (tests/exe_test.cpp) builds every example and SVA
fixture at both tiers and requires the executable's stdout and exit status to
equal `run`'s, and `run`'s stderr to be what `build` printed while compiling
followed by what the executable printed.

## The executable runtime links only the C library

An executable links `clotho_runtime_exe`: the runtime's own sources, compiled a
second time against libc++'s headers so that no part of a C++ library is left
to link. Shipping a C++ library inside clotho was rejected: libstdc++ and
libgcc would make every clotho release carry GPL object code, and any C++
library would have to be built for every target clotho cross-compiles to,
where libc++'s headers are target-independent.

What a header-only build cannot provide was removed from the runtime source,
for both builds: the VCD writer's thread, mutex and condition variables are
pthreads or Win32 (Os.h); aligned allocation is `posix_memalign` or
`_aligned_malloc`; floating-point `std::to_chars` and `std::from_chars`, which
live in the C++ library's compiled part, are `shortest_double` and
`parse_double` (runtime/Strings.cpp), checked byte-identical to libstdc++'s on 26M
doubles and 6M strings; `std::unordered_map` is ankerl's map; one guarded
static is an atomic.

The second build then sets (ExeConfig.h, CMakeLists.txt): no exceptions or
RTTI; libc++ hardening off, as a release libstdc++ build has none; {fmt}
header-only, without locales, with a `FMT_THROW` that names the failed
expression instead of constructing an exception object. ExeSupport.cpp
supplies the rest: `operator new`/`delete` over `malloc`, libc++'s
`__libcpp_verbose_abort`, and `std::string`'s members that libc++ declares
extern templates, instantiated from libc++'s own list as its `src/string.cpp`
does. The archive's undefined symbols are the C library, libm, pthreads and
compiler-rt's builtins.

## Linking

lld runs inside clotho (`lld::lldMain`, src/link/Link.cpp) with the link line a
compiler driver would build, written out per platform. The files it needs are
embedded in clotho when clotho is built (`#embed`, generated LinkKit.cpp) and
extracted to a temporary directory for the link:

| Platform | Embedded | Taken from the machine at link time |
|---|---|---|
| Linux (glibc) | runtime, compiler-rt builtins and crtbegin/crtend, glibc's `Scrt1.o`, `crti.o`, `crtn.o`, `libc_nonshared.a` | the dynamic linker, `libc.so.6` and `libm.so.6` clotho itself runs with (`dl_iterate_phdr`); a statically linked clotho, which runs with none, takes the dynamic linker from its fixed ELF ABI path and the libraries from the first standard directory (multiarch first) that has them |
| macOS | runtime, compiler-rt builtins | none: a libSystem stub listing exactly the symbols the inputs need, each checked against this machine's libSystem |
| Windows (MinGW-w64) | runtime, compiler-rt builtins, `crt2.o`, crtbegin/crtend, the MinGW-w64 CRT and import libraries | none |

So an executable needs the C library clotho was built against, or a newer one:
on Linux, glibc at least as new as the one that built clotho; on macOS, the
macOS version clotho was built for.

## Processor

`--cpu` picks the processor for `exe` and `obj`. The default is a baseline every
machine of the kind has (`x86-64-v2`, `apple-m1`, `generic` Armv8-A), so an
executable built on a new machine runs on an older CI runner; `native` is this
machine exactly. The JIT keeps compiling for the host it runs on. Objects use
an ordinary TargetMachine, not ORC's: a JIT TargetMachine on x86-64 defaults to
the large code model, whose absolute addresses in `.text` are text relocations
a macOS link refuses.

## DPI-C

`--dpi-lib` objects and archives are linked in. A shared library is linked
dynamically with an rpath to where it is; when the design has `export "DPI-C"`
functions the executable is linked `--export-dynamic`, so the library can call
them, which the JIT cannot offer. Windows has no such case: a DLL resolves its
imports when it is built, so `build` refuses a DLL for a design with exports.

## Source locations

Messages carry source locations as the compiler rendered them: relative to the
directory `build` ran in. Run an executable from that directory, or build from
the one it will run in, for paths that resolve. `--path-prefix-map <dir>=<new>`
shows files under `<dir>` as `<new>` plus the rest of their path instead
(SourceContext::shown_file), so a CI build can name sources the same way
whatever directory the checkout is in: `--path-prefix-map "$PWD="` shows them
relative to the checkout.

## Not built

- Cross-compilation (`--target`): each target needs its executable runtime
  built for it and its C library's startup files; the link itself is the same.
- A clotho built with MSVC rather than MinGW-w64: the executable runtime's
  flags are GCC/clang's.
- A per-definition object cache for incremental builds (module-split.md).
