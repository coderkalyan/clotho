# clotho

A SystemVerilog simulator that JIT-compiles the design to native code.

clotho reads SystemVerilog, elaborates it with [slang](https://sv-lang.com/),
lowers it through its own IR to LLVM IR, compiles that with the LLVM ORC JIT,
and runs the result against an event-driven scheduler. There is no interpreter
and no generated C++ to build: `clotho run design.sv` goes from source to a
running simulation in one process.

Targeting IEEE 1800-2023 (the LRM) in full. Not there yet — `docs/TODO.md`
lists what is still missing, by milestone. Anything clotho does not
implement fails loudly; it never silently accepts a construct it cannot
correctly simulate.

## Status

Working today: Verilog-2005 RTL, the SystemVerilog data model (packed and
unpacked aggregates, dynamic arrays, queues, associative arrays, strings,
classes), processes and fine-grained timing control, interfaces, packages,
generate, gates and net resolution, immediate and concurrent assertions,
DPI-C import and export, VCD output, and language-server and debug-adapter
front ends. See `docs/TODO.md` for what is missing and `docs/design/` for how
the interesting parts work.

## Building

Requirements: a C++23 compiler (clang is what we test), CMake 3.20+, Ninja,
LLVM 22 development libraries, and Python 3 (slang's build needs an
interpreter). slang, doctest, unordered\_dense and glaze are fetched by CMake
at configure time.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target clotho_cli
./build/clotho run examples/initial.v
```

Concurrent assertions are compiled by [libsva](https://github.com/coderkalyan/libsva),
a separate Zig library, so a full build also needs Zig 0.16+ and a libsva
checkout beside this one (or `-DCLOTHO_LIBSVA_DIR=<path>`). Build without it
with `-DCLOTHO_SVA=OFF`; everything except `assert property` and its relatives
still works.

The default build type is Debug, which builds the frontend unoptimized with
every assertion live; use `RelWithDebInfo` or `Release` for anything you intend
to measure. CI covers Linux x64/arm64, macOS arm64/x64 and Windows x64 under
MSYS2 CLANG64; Windows arm64 is advisory.

## Using it

```
clotho build   compile the design and write the generated code, without running it
clotho run     compile the design and simulate it
clotho lint    check the design and report problems, without compiling or running
clotho lsp     language server over stdio
clotho dap     debug adapter over stdio
```

## Testing

```sh
cmake --build build --target clotho_test
ctest --test-dir build -j 8                        # the whole suite, in parallel
./build/clotho_test -sf='*/class_test.cpp'         # one file
./build/clotho_test -tc='*virtual*'                # cases by name
```

The suite is doctest-based and lives in `tests/`. `tests/perf/` is a benchmark
harness (`perf.py`) that measures compile and simulation time per case, gates
regressions against a baseline measured on the same machine, and can measure
the same cases under iverilog and Verilator for context.

## Contributing

Issues and pull requests are welcome; see `CONTRIBUTING.md`. Contributions
require agreeing to the Clotho Contributor License Agreement.

## License

Copyright (C) 2026 Kalyan Sriram.

Clotho is licensed under the GNU General Public License, version 3 only
(`GPL-3.0-only`, see `LICENSE.txt`). The runtime in `src/runtime/`, which is
linked into every simulation, is licensed under the Mozilla Public License 2.0
(`MPL-2.0`, see `src/runtime/LICENSE`), so a design simulated or compiled by
Clotho can be distributed under any terms its authors choose. A distributed
binary that contains the runtime must tell its recipients where to get the
runtime's source (MPL-2.0 section 3.2); for an unmodified runtime, a link to the
Clotho release is enough. Such a binary also carries the third-party notices the
runtime's dependencies require; see `THIRD_PARTY_NOTICES.md`.

The license covers Clotho itself. Designs and testbenches you run with Clotho,
and the output Clotho produces from them, are yours.

The patches to slang in `patches/` are licensed under the MIT License (see
`patches/LICENSE`), the same terms as slang itself, so they can go upstream.

Third-party components and their licenses are listed in
`THIRD_PARTY_NOTICES.md`.

These terms apply to every revision in this repository's history, including
revisions that predate the license files, with two exceptions. Third-party files
listed in `THIRD_PARTY_NOTICES.md`, or whose own headers state other terms, keep
their own licenses. `src/runtime/` is MPL-2.0 from the commit "runtime: license
src/runtime under MPL-2.0" onward; in earlier revisions it is GPL-3.0-only like
the rest of Clotho.
