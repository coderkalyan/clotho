# Third-party notices

Clotho is licensed under GPL-3.0-only (`LICENSE.txt`), with these exceptions:

- `src/runtime/` is licensed under MPL-2.0 (`src/runtime/LICENSE`).
- `patches/` is licensed under MIT (`patches/LICENSE`).
- `docs/cla/` is adapted from the Project Harmony contributor agreements
  (HA-CLA-I and HA-CLA-E v1.0), licensed under CC BY 3.0; see each file's footer.
- The third-party files under `tests/perf/cases/` listed below keep their own
  licenses.

This file lists third-party code that Clotho builds against, links, or carries
in its source tree. The full text of each license is in `LICENSES/`, copied
verbatim from the upstream project, or for BSL-1.0, SHL-0.51 and BSD-2-Clause
from the SPDX reference text.

## Linked into the `clotho` executable

Fetched at configure time (see `CMakeLists.txt` for the pinned revisions), or
found on the host where noted.

| Component | License | Text |
|---|---|---|
| [slang](https://github.com/MikePopoloski/slang) | MIT | `LICENSES/slang.txt` |
| slang's bundled `boost_unordered.hpp`, `boost_concurrent.hpp`, `expected.hpp` | BSL-1.0 | `LICENSES/BSL-1.0.txt` |
| slang's bundled `BS_thread_pool.hpp` ([thread-pool](https://github.com/bshoshany/thread-pool)) | MIT | `LICENSES/BS_thread_pool.txt` |
| [boost.regex](https://github.com/MikePopoloski/regex) (via slang; host Boost if present) | BSL-1.0 | `LICENSES/BSL-1.0.txt` |
| [toml++](https://github.com/marzer/tomlplusplus) (via slang; host copy if present) | MIT | `LICENSES/tomlplusplus.txt` |
| [{fmt}](https://github.com/fmtlib/fmt) | MIT, with an object-code exception | `LICENSES/fmt.txt` |
| [mimalloc](https://github.com/microsoft/mimalloc) (via slang; host copy if present) | MIT | `LICENSES/mimalloc.txt` |
| [unordered_dense](https://github.com/martinus/unordered_dense) | MIT | `LICENSES/unordered_dense.txt` |
| [glaze](https://github.com/stephenberry/glaze) | MIT | `LICENSES/glaze.txt` |
| glaze's bundled [fast_float](https://github.com/fastfloat/fast_float) | Apache-2.0, MIT or BSL-1.0; used under MIT | `LICENSES/fast_float.txt` |
| [LLVM](https://llvm.org) 22 (host install) | Apache-2.0 WITH LLVM-exception | `LICENSES/LLVM.txt` |
| [libsva](https://github.com/coderkalyan/libsva), including the [Zig](https://ziglang.org) standard library it is built with | MIT | `LICENSES/libsva.txt`, `LICENSES/zig.txt` |

## Linked into every simulation

The Clotho runtime (`src/runtime/`, MPL-2.0) uses unordered_dense and {fmt},
so their notices apply to any binary that contains the runtime. {fmt}'s license
waives its notice requirement for code embedded in object form;
unordered_dense's MIT license does not.

## Tests only

| Component | License | Text |
|---|---|---|
| [doctest](https://github.com/doctest/doctest) | MIT | `LICENSES/doctest.txt` |

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
| `picorv32/testbench.v`, `coremark_picorv32/testbench.v` | [YosysHQ picorv32](https://github.com/YosysHQ/picorv32) | public domain, notice in file |
| `coremark_*/coremark.hex` | compiled from [EEMBC CoreMark](https://github.com/eembc/coremark) with the files above | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |

CoreMark® is a registered trademark of EEMBC. The images above are used to
measure simulator speed; they are not CoreMark scores.
