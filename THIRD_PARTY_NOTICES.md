# Third-party notices

Clotho itself is licensed under GPL-3.0-only (`LICENSE.txt`), except for the
runtime in `src/runtime/`, which is licensed under MPL-2.0
(`src/runtime/LICENSE`). This file lists third-party code that Clotho builds
against, links, or carries in its source tree. The full text of each license is
in `LICENSES/`, copied verbatim from the upstream project.

## Linked into the `clotho` executable

Fetched at configure time (see `CMakeLists.txt` for the pinned revisions), or
found on the host where noted.

| Component | License | Text |
|---|---|---|
| [slang](https://github.com/MikePopoloski/slang) | MIT | `LICENSES/slang.txt` |
| slang's bundled `boost_unordered.hpp`, `boost_concurrent.hpp`, `expected.hpp` | BSL-1.0 | `LICENSES/BSL-1.0.txt` |
| slang's bundled `BS_thread_pool.hpp` | MIT | `LICENSES/BS_thread_pool.txt` |
| [{fmt}](https://github.com/fmtlib/fmt) | MIT, with an object-code exception | `LICENSES/fmt.txt` |
| [mimalloc](https://github.com/microsoft/mimalloc) (via slang; host copy if present) | MIT | `LICENSES/mimalloc.txt` |
| [unordered_dense](https://github.com/martinus/unordered_dense) | MIT | `LICENSES/unordered_dense.txt` |
| [glaze](https://github.com/stephenberry/glaze) | MIT | `LICENSES/glaze.txt` |
| [LLVM](https://llvm.org) 22 (host install) | Apache-2.0 WITH LLVM-exception | `LICENSES/LLVM.txt` |
| [libsva](https://github.com/coderkalyan/libsva) | MIT | `LICENSES/libsva.txt` |

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
header. Core RTL is downloaded by each case's `fetch.sh` and is not stored in
this repository.

| Files | Origin | License |
|---|---|---|
| `coremark_cv32e40p/tb/{cv32e40p_tb_subsystem,dp_ram,mm_ram,tb_top}.sv` | [OpenHW cv32e40p](https://github.com/openhwgroup/cv32e40p) | SHL-0.51 (`LICENSES/SHL-0.51.txt`) |
| `coremark_cv32e40p/fw/bsp/vectors.S` | ETH Zürich and University of Bologna | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |
| `coremark_cv32e40p/fw/bsp/crt0.S` | SiFive Inc., ETH Zürich and University of Bologna | BSD-2-Clause (FreeBSD), notice in file |
| `coremark_cv32e40p/fw/bsp/link.ld` | Free Software Foundation; ETH Zürich and University of Bologna | permissive notice in file |
| `picorv32/testbench.v`, `coremark_picorv32/testbench.v` | [YosysHQ picorv32](https://github.com/YosysHQ/picorv32) | public domain, notice in file |
| `coremark_*/coremark.hex` | compiled from [EEMBC CoreMark](https://github.com/eembc/coremark) | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |
| `picorv32/firmware/firmware.hex` | picorv32 firmware (public domain), linking the riscv-tests suite (The Regents of the University of California) | public domain; BSD-3-Clause (`LICENSES/riscv-tests.txt`) |

CoreMark® is a registered trademark of EEMBC. The images above are used to
measure simulator speed; they are not CoreMark scores.
