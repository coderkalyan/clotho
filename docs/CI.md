# Continuous integration

`.github/workflows/ci.yml` builds clotho and runs the correctness suite
(`clotho_test`, 1781 cases) plus a CLI smoke run on six targets. The
performance cases under `tests/perf/` are deliberately excluded — they are
measurement, not pass/fail.

## Triggers

The macOS and Windows runners are slower than the Linux ones and scarcer, so
the matrix is tiered by trigger — a short signal on every push, the full sweep
where a platform regression would otherwise escape:

| trigger | platforms |
|---|---|
| push to a feature branch, pull request | linux-x64 |
| push to `main`, push to a `ci/*` branch, nightly (07:00 UTC), `workflow_dispatch` | all six, plus linux-x64-sanitize |

`ci/*` exists because GitHub only offers `workflow_dispatch` for workflows that
already exist on the default branch — without it there is no way to exercise the
slower platforms while iterating on CI itself.

## Configurations

Every job builds Release, which keeps `assert()` live as shipped builds do.
linux-x64 adds `CLOTHO_EXTRA_ASSERTS`, the checks too slow to ship, so every
push runs them; the other platforms test the shipped configuration.
linux-x64-sanitize adds `CLOTHO_SANITIZE` (ASan and UBSan) on top of the
extra asserts.

## sv-tests

Nightly, and on `workflow_dispatch` with `sv_tests` set, the `sv-tests` job runs
the linux-x64 binary over [sv-tests](https://github.com/chipsalliance/sv-tests)
with the clotho runner from the `coderkalyan/sv-tests` fork, pinned by
`SV_TESTS_REF`. `.github/scripts/sv-tests-summary.py` classifies each test as
pass, self-check (sv-tests counts a pass but the design printed `FAILED`, as
ivtest designs do when their own checks fail), crash, timeout, ran (exit 0 but
failed: a wrong result or an accepted invalid design) or rejected, and fails
the job on any crash. The report and
logs are uploaded as the `sv-tests-report` artifact.

## The LLVM dependency

LLVM is never built in CI. A source build is tens of minutes and the pin moves
at most twice a year, so each platform takes the cheapest prebuilt that ships
both the CMake package and the development libraries:

| platform | source of LLVM 22 |
|---|---|
| linux-x64, linux-arm64 | `apt.llvm.org` `llvm-22-dev` (the repo publishes amd64 and arm64) |
| macos-arm64, macos-x64 | Homebrew `llvm@22` |
| windows-x64, windows-arm64 | MSYS2 `CLANG64` / `CLANGARM64`, whose `llvm` package is 22.1.8 |

Windows deliberately does *not* use LLVM's own release tarball. The only
prebuilt LLVM 22 development libraries LLVM ships for Windows are MSVC-ABI,
and every Windows-specific workaround this build needed came from that: a DIA
SDK path baked into `LLVMDebugInfoPDB`'s exported target (pointing at the
Visual Studio version on LLVM's build machine, not the runner's), an
expected-lite/`std::expected` divergence in the MSVC STL, mimalloc colliding
with the CRT's allocator symbols, compiler-rt's builtins going unlinked, and
an import library clobbering a static archive of the same name. MSYS2 packages
the same LLVM 22.1.8 as a MinGW-w64/UCRT build, so none of those arise — and
it is a ~600 MB pacman install rather than an 862 MB download plus a 3.5 GB
extraction.

What is cached is `ccache` and the FetchContent sources — slang is pinned to a
SHA, so its clone cannot be shallow, and it dominates cold-build time far more
than LLVM acquisition does. Caches live in GitHub's own Actions cache service,
scoped per ref: a branch restores from its own ref or the default branch, never
from a sibling, so a new branch cold-builds once until `main` has a cache.

If `apt.llvm.org` or Homebrew ever drift off major 22, the fallback is a manual
workflow that builds LLVM once per platform and publishes the result as a
release asset on this repository, which CI then pulls. That is strictly more
robust and strictly more setup; it is not worth it until the drift happens.

## Toolchains

- **Linux: clang, not GCC.** Ubuntu 24.04 caps out at GCC 14, which rejects the
  implicit capture of an enclosing-class member inside a deducing-`this` lambda
  (`ir/llvm/Analyses.cpp`'s `close_fn`) — a GCC 14 defect fixed in 15, and no
  GitHub-hosted image carries 15. `clang-22` comes from the same apt repository
  as the LLVM being linked. Restoring a GCC build (valuable for a compiler
  project — a second front end catches different mistakes) needs either a
  GCC 15 image or a workaround at that call site.
- **macOS: Homebrew LLVM's clang and its libc++**, with
  `-D_LIBCPP_DISABLE_AVAILABILITY`. libc++ annotates declarations whose
  implementation lives in the shared library with the macOS version that first
  shipped them, and floating-point `std::from_chars` (which `src/runtime/` uses
  to parse reals) is marked macOS 26. Those annotations describe Apple's system
  libc++; we link Homebrew's, which has the symbol.
- **Windows: MSYS2's clang, with no MSVC anywhere.** MSVC proper cannot build
  clotho at all — the runtime's wide arithmetic uses `unsigned __int128`, which
  only GCC and Clang provide — and the MSVC *ABI* brought the whole class of
  problems described above. The MSVC-only workarounds that remain in
  `CMakeLists.txt` are scoped to `if(MSVC)` so they cost nothing on this
  toolchain, but they document what a clang-cl build would still need.

Each matrix entry carries its own shell, applied as the job's
`defaults.run.shell`, so Windows steps run inside the MSYS2 environment and
everything else in plain bash. It has to be a job default rather than a
per-step `shell:` — that field cannot see the `matrix` context, and a workflow
that tries is rejected outright with no jobs scheduled at all.

## Platform status

| target | result |
|---|---|
| linux-x64, linux-arm64 | 1206/1206 |
| macos-arm64, macos-x64 | 1206/1206 |
| windows-x64 | 1206/1206 |
| windows-arm64 | 1203/1206 |

Only windows-arm64 is advisory (`continue-on-error`); every other target gates.

**windows-arm64's three failures** are value-correctness, not crashes:
`interface_test:95` and `packed_range_test:226` produce `X` where a definite
value is expected, and `string_test:415` prints the *third* `$display`
argument's value for the first. That last one looks like argument marshalling
rather than arithmetic, and Windows on ARM64 differs from AAPCS64 in how
variadic and floating-point arguments are passed -- the obvious next thread,
and unrelated to the relocation bug below.

**The crash that used to halt it after 338 cases** was a JITLink defect, worked
around in the backend. Every design using `assign #d`, a gate delay, or
`<= #d` segfaulted the moment the delay fired. The wheel has four schedule
sites and they split perfectly: process `#delay` and delayed event triggers
pass a JIT-emitted function as the expiry callback and work, while the
intra-assignment and inertial records passed a *native* runtime function
(`rt_intra_cb` / `rt_inertial_cb`) and were exactly the two that crashed. The
callback is stored into the timer node, so its address must be materialized
into memory -- a different relocation from calling the same symbol, which is
why JIT'd code calls `rt_fwrite` all day on the same host. AArch64 COFF gets
that wrong; x86-64 COFF and AArch64 ELF, both older combinations, do not.
Routing the two native callbacks through JIT-local forwarders puts all four
sites on the footing that already works. Worth reporting upstream with the
reduced case rather than leaving as folklore.

What it took to get Windows this far, in case any of it recurs:

- `longjmp` had to go entirely (see the $finish note below); that alone moved
  x64 from 7 cases to a complete run.
- CRLF, from *three* independent sources: cmd.exe's `>` redirect giving the
  re-executed child a text-mode stdout; clotho's own `$fopen` opening `"w"`
  rather than `"wb"`; and two C++ test fixtures doing the same.
- `/tmp` hardcoded in three tests, once in C++, once as a raw `char path[]`,
  and once inside a SystemVerilog source string.
- `ENABLE_EXPORTS` exports nothing from a PE executable, so DPI-C imports
  defined in the test TUs were invisible to the JIT: `-Wl,--export-all-symbols`.
- `__udivti3` / `__umodti3` / `___chkstk_ms` had to be named explicitly. A
  dynamically linked host finds them via libgcc_s in the process symbol table;
  MinGW links libgcc statically and clotho's native code never divides a
  128-bit integer, so nothing pulled them in to be exported.
- `$system`'s return encoding is platform-defined.

## A hazard the harness works around

A design that only ends at `$finish` is simulated in a **child process**
(`tests/Subprocess.h`), not in the test runner. `$finish` longjmps out of the
middle of the simulation, abandoning live coroutine frames and leaving the
runtime's globals part-way through whatever they were doing; enough of those in
one process and the heap is corrupted, and the suite segfaults inside the
allocator during a later, unrelated LLVM compile.

The isolation is load-bearing, but it is a workaround. The underlying defect —
**a `$finish`'d simulation leaves the process unfit to run another** — is
reachable by any embedder that runs two simulations, and the shorter capture
helpers still run `$finish` designs in-process, which is where linux-arm64's
recurring SIGSEGV comes from.

Both of this file's open problems reduce to `$finish` longjmping out of live
JIT'd coroutine frames: on Windows the SEH unwind cannot walk them at all, and
everywhere else the abandoned frames and half-updated runtime globals corrupt
the heap for later work. Making `$finish` cooperative — set a flag, let the
simulation loop return normally, tear the runtime down in order — fixes the
Windows blocker, the arm64 flake, and the need for a subprocess in the test
harness, all at once. That is the single highest-value follow-up here.
