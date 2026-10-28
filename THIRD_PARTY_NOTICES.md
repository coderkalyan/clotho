# Third-party notices

Clotho is licensed under GPL-3.0-only (`LICENSE.txt`), with these exceptions:

- `src/runtime/` is licensed under MPL-2.0 (`src/runtime/LICENSE`).
- `patches/` is licensed under MIT (`patches/LICENSE`).
- `docs/cla/` is adapted from the Project Harmony contributor agreements
  (HA-CLA-I and HA-CLA-E v1.0) and, like them, is licensed under CC BY 3.0; see
  each file's footer.
- The third-party files under `tests/perf/cases/` listed below keep their own
  licenses.

This file lists third-party code that Clotho builds against, links, or carries
in its source tree. The full text of each license is in `LICENSES/`, copied
verbatim from the upstream project, or for BSL-1.0, SHL-0.51 and BSD-2-Clause
from the SPDX reference text.

## Linked into the `clotho` executable

Fetched at configure time, or found on the host where noted. Clotho's
`CMakeLists.txt` pins slang, unordered_dense and glaze; slang's
`external/CMakeLists.txt` pins {fmt}, boost.regex, mimalloc and toml++. glaze is
fetched and linked only into builds configured with `CLOTHO_LSP` or `CLOTHO_DAP`
(on by default for Debug builds, off otherwise): the language server and debug
adapter are its only users, so a default Release build contains it, and the
three components bundled with it, nowhere.

| Component | License | Text |
|---|---|---|
| [slang](https://github.com/MikePopoloski/slang) | MIT | `LICENSES/slang.txt` |
| slang's bundled `boost_unordered.hpp`, `boost_concurrent.hpp`, `expected.hpp` | BSL-1.0 | `LICENSES/BSL-1.0.txt` |
| slang's bundled `BS_thread_pool.hpp` ([thread-pool](https://github.com/bshoshany/thread-pool)) | MIT | `LICENSES/BS_thread_pool.txt` |
| slang's bundled IEEE 1800 VPI headers (`external/ieee1800/`, constants used by `$sformat`) | MIT, per slang's `REUSE.toml` | `LICENSES/slang.txt` |
| [boost.regex](https://github.com/MikePopoloski/regex) (via slang; host Boost if present) | BSL-1.0 | `LICENSES/BSL-1.0.txt` |
| [toml++](https://github.com/marzer/tomlplusplus) (via slang; host copy if present) | MIT | `LICENSES/tomlplusplus.txt` |
| [{fmt}](https://github.com/fmtlib/fmt) | MIT, with an object-code exception | `LICENSES/fmt.txt` |
| [mimalloc](https://github.com/microsoft/mimalloc) (via slang; host copy if present) | MIT | `LICENSES/mimalloc.txt` |
| [unordered_dense](https://github.com/martinus/unordered_dense) | MIT | `LICENSES/unordered_dense.txt` |
| [glaze](https://github.com/stephenberry/glaze) (only with `CLOTHO_LSP` or `CLOTHO_DAP`) | MIT | `LICENSES/glaze.txt` |
| glaze's bundled [fast_float](https://github.com/fastfloat/fast_float) (only with `CLOTHO_LSP` or `CLOTHO_DAP`) | Apache-2.0, MIT or BSL-1.0; used under MIT | `LICENSES/fast_float.txt` |
| glaze's bundled [zmij](https://github.com/vitaut/zmij) (only with `CLOTHO_LSP` or `CLOTHO_DAP`) | MIT or BSL-1.0; used under BSL-1.0 | `LICENSES/BSL-1.0.txt` |
| glaze's bundled xxh64, Copyright (c) 2015 Daniel Kirchner (only with `CLOTHO_LSP` or `CLOTHO_DAP`) | MIT, notice in `glaze/api/xxh64.hpp` | `LICENSES/glaze.txt` (same MIT terms) |
| [LLVM](https://llvm.org) 22 (host install), including lld, which `clotho build` links with | Apache-2.0 WITH LLVM-exception | `LICENSES/LLVM.txt` |
| [libsva](https://github.com/coderkalyan/libsva), including the [Zig](https://ziglang.org) standard library and compiler-rt it is built with | MIT | `LICENSES/libsva.txt`, `LICENSES/zig.txt` |

## Linked into every simulation

The Clotho runtime (`src/runtime/`, MPL-2.0) uses unordered_dense and {fmt},
so their notices apply to any binary that contains the runtime: `clotho` itself,
and every executable `clotho build` writes. {fmt}'s license waives its notice
requirement for code embedded in object form; unordered_dense's MIT license does
not. An executable prints the runtime's license and both notices for
`--licenses` (`src/runtime/exe-notices.txt`).

`clotho` also embeds, and links into every executable, files from the toolchain
it was built with (docs/design/executables.md):

| Component | License | Notice in an executable |
|---|---|---|
| LLVM's compiler-rt builtins and crtbegin/crtend, and code instantiated from libc++'s headers | Apache-2.0 WITH LLVM-exception (`LICENSES/LLVM.txt`) | none required: the exception waives it for code embedded by compilation |
| glibc's `Scrt1.o`, `crti.o`, `crtn.o` and `libc_nonshared.a` (Linux) | LGPL-2.1-or-later with glibc's exception for these files, which permits linking them into any program without restriction | none required |
| The MinGW-w64 C runtime and import libraries (Windows) | mostly public domain, with parts under ZPL-2.1 and other permissive licenses (`LICENSES/MinGW-w64-runtime.txt`) | the MinGW-w64 runtime's notices, printed by `--licenses` after the others |

## Tests only

| Component | License | Text |
|---|---|---|
| [doctest](https://github.com/doctest/doctest) | MIT | `LICENSES/doctest.txt` |
| [glaze](https://github.com/stephenberry/glaze), for the language-server and debug-adapter tests (only with `CLOTHO_LSP` or `CLOTHO_DAP`) | MIT | `LICENSES/glaze.txt` |

## Vendored in `tests/perf/cases/`

Benchmark inputs, not part of any Clotho binary. Each file keeps its upstream
header; files Clotho modified also carry a notice saying what changed. Core RTL
is downloaded by each case's `fetch.sh` and is not stored in this repository.

| Files | Origin | License |
|---|---|---|
| `coremark_cv32e40p/tb/{cv32e40p_tb_subsystem,dp_ram,mm_ram,tb_top,cv32e40p_random_interrupt_generator}.sv` (modified) | [OpenHW cv32e40p](https://github.com/openhwgroup/cv32e40p) | SHL-0.51 (`LICENSES/SHL-0.51.txt`) |
| `coremark_*/fw/port/core_portme.{c,h}` (modified) | [EEMBC CoreMark](https://github.com/eembc/coremark) `barebones/` port | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |
| `coremark_cv32e40p/fw/bsp/vectors.S` | ETH Zürich and University of Bologna | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |
| `coremark_cv32e40p/fw/bsp/crt0.S`; `coremark_picorv32/fw/bsp/crt0.S` (modified) | SiFive Inc., ETH Zürich and University of Bologna | BSD-2-Clause (FreeBSD) (`LICENSES/BSD-2-Clause.txt`), notice in file |
| `coremark_cv32e40p/fw/bsp/link.ld`; `coremark_{picorv32,hazard3}/fw/bsp/link.ld` (modified) | Free Software Foundation; ETH Zürich and University of Bologna | permissive notice in file |
| `picorv32/testbench.v`, `coremark_picorv32/testbench.v` (modified) | [YosysHQ picorv32](https://github.com/YosysHQ/picorv32) | public domain, notice in file |
| `hazard3/tb_hazard3.sv`, `coremark_hazard3/tb_hazard3.sv` (the core instance and its port connections; the memory and MMIO model follows `tb_memio.cpp` and `tb_constants.h`) | [Hazard3](https://github.com/Wren6991/Hazard3) `test/sim/tb_common/` | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |
| `coremark_cv32e40p/coremark.hex`, `coremark_picorv32/coremark.hex` | compiled from [EEMBC CoreMark](https://github.com/eembc/coremark) (with `ee_printf.c` patched by `fw/build_fw.sh`), the CoreMark port, the SiFive/ETH `crt0.S`, `link.ld` and (cv32e40p) `vectors.S` files above, and Clotho's own `libnano.c` | Apache-2.0 (`LICENSES/Apache-2.0.txt`); BSD-2-Clause for `crt0.S`, Copyright (c) 2017 SiFive Inc. and Copyright (c) 2019 ETH Zürich and University of Bologna (`LICENSES/BSD-2-Clause.txt`); the FSF notice; and GPL-3.0-only for `libnano.c` |
| `coremark_hazard3/coremark.hex` | compiled from [EEMBC CoreMark](https://github.com/eembc/coremark) (with `ee_printf.c` patched by `fw/build_fw.sh`), the CoreMark port and `link.ld` above, and Clotho's own `crt0.S` and `libnano.c` | Apache-2.0 (`LICENSES/Apache-2.0.txt`); the FSF notice; and GPL-3.0-only for Clotho's files |

Source for the `coremark.hex` images: each case's `fw/` directory holds the port,
the board-support files and `build_fw.sh`, and its `fetch.sh` names the exact
upstream commit, eembc/coremark@1f483d5b8316753a742cbf5590caf5bd0a4e4777, whose
sources `build_fw.sh` compiles.

CoreMark® is a registered trademark of EEMBC. The images above are used to
measure simulator speed; they are not CoreMark scores.
