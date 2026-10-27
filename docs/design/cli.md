# CLI: the `clotho` driver surface

Status: **landed**. Replaces the flat option list (`--emit=run|ir|llvm|opt|check`
plus a bag of `--no-*` toggles) with six commands over a single option
namespace shared with slang. `src/main.cpp` is the implementation; this document
is the rationale and the migration record.

## Shape

```
clotho <command> [options] <source>... [+plusarg...]

commands:
  build   compile the design and write the requested artifact
  run     compile the design and simulate it
  lint    analyse the design and report diagnostics, writing nothing
  synth   write the design back out as synthesizable SystemVerilog (Debug builds)
  lsp     serve the Language Server Protocol over stdin/stdout (CLOTHO_LSP)
  dap     serve the Debug Adapter Protocol over stdin/stdout (CLOTHO_DAP)
```

The command names how far down the pipeline the run goes, so the first three
nest: `lint` is `build` without an artifact, and `run` is `build` plus execution.
The option sets nest the same way.

`synth` branches rather than nests. It shares the front end and stops before the
LLVM backend entirely -- `clotho_legalize` links no LLVM and no slang, and the
simulator does not link `clotho_legalize` -- so it is a peer of `build`, not a
stage of it. It is the only command whose artifact is source rather than
generated code. It is unfinished, so only a Debug build has it (CMake option
`CLOTHO_SYNTH`, on by default for Debug and off otherwise): a Release build
rejects `synth` as an unknown command and registers none of the `[synth]`
options below.

The command word is required and must be `argv[1]`:
`clotho design.sv` is a usage error, not an implicit `run`, because implicit
modes are how the old surface drifted.

`lsp` and `dap` are servers, not pipeline stages, and take **no arguments at
all** -- not even a source file. An editor supplies the language server's file
set, and the debug adapter's design arrives in the launch request's `args`,
which are a `run` command line passed through to the design process verbatim
(`docs/design/dap.md` §4.4). Accepting `-D` / `-y` / `-f` here and ignoring
them would be the silent acceptance PRINCIPLES.md forbids, so both refuse every
argument except `--help`, `--help-all` and `--version` (`src/main.cpp`).

Both are unfinished too, so they are opt-in like `synth`: CMake options
`CLOTHO_LSP` and `CLOTHO_DAP`, on by default for Debug and off otherwise. Unlike
`synth`, where only the CLI wiring is conditional, the whole server is: with an
option off, its library (`clotho_lsp`, `clotho_dap`), the framing library they
share (`clotho_transport`, built when either is on), glaze (fetched and linked
only when either is on) and its test file (`tests/lsp_test.cpp` and
`tests/lsp_tokens_test.cpp`, `tests/dap_test.cpp`) are not built, linked or
registered with CTest. A Release build rejects `lsp` and `dap` as unknown
commands, and they appear in no help, no "expected ..." list and no suggestion.
CI configures with both on so they stay tested.

`clotho --help` lists the commands and the options most designs need,
`clotho <command> --help` the options that command takes, and `clotho --help-all`
every option (see "Help" below). There is no second help and nothing to forward --
see "One parser".

### Options

| option | commands | effect |
|---|---|---|
| `--emit <target>` | build | `llvm` (default, post-coroutine-pipeline), `llvm-raw` (straight from the backend, `llvm.coro.*` intact), `ir` (Clotho process IR). `ir` stops before code generation, so `-O`, `--sanitize`, `--stat`, `--coverage` and the `--gc-*` group have no effect under it |
| `--target <lang>` | synth (Debug builds only) | `sv` (default, IEEE 1800), `2005` (IEEE 1364-2005: `reg`/`wire`, no wildcard equality, no jump statements) |
| `--hierarchy <mode>` | synth (Debug builds only) | `instances` (default) writes one module per instance with names as written; `flat` writes one module with paths mangled into identifiers |
| `--sidecar <path>` | synth (Debug builds only) | Also write the JSON sidecar: the original-to-emitted name map, and each signal's inferred storage, clock and reset |
| `--no-legalize` | synth (Debug builds only) | Translate as written instead of writing out the implied hardware: no mux trees rebuilt, no implicit branch filled in |
| `--full-case` | synth (Debug builds only) | Believe `full_case` / `unique`; reported wherever the claim is what removed a latch |
| `--parallel-case` | synth (Debug builds only) | Believe `parallel_case` / `unique0` |
| `-o, --output <file>` | build, synth | artifact path for every target; `-` (default) is stdout |
| `--time` | build, run, lint, synth | per-phase wall-clock to stderr |
| `-O` | both | `default<O3>` plus aggressive machine codegen |
| `--stat <list>` | both | `dedup`, `dedup-sets`, `comb-fuse`, `finish`, `parallel`. `parallel` reports only under `run` |
| `--sanitize <list>` | both | dynamic bug checks instrumented into the emitted code, reported to stderr and non-fatal: `bounds` (an index outside what it indexes — array element, packed bit/part select, string character, queue method index, pop from an empty queue; x/z index included), `xprov` (xsan, x provenance — not yet implemented, so **Debug builds only**, `CLOTHO_XSAN`; a shipped build rejects the name). Off by default |
| `--coverage <file>` | both | write the design's functional-coverage report here (LRM 19): which of the bins its `covergroup`s declare the run reached, and which it never did. No report by default. The destination is compiled into the artifact — the emitted coverage model names its own — so a `build` and the `run` of what it built must be given the same file |
| `--gc-stress` | both | collect class objects at every opportunity instead of when the design has created enough of them. Slow by design: it turns an object clotho failed to keep into an immediate wrong answer rather than a rare one, so it is for working on clotho itself. **Changes the generated code** (garbage-collection.md), so a `build` and the `run` of what it built must be given the same setting |
| `--gc-stats` | both | collection summary when the design exits: how many collections, how much was reclaimed, how long the design was paused |
| `--gc-nursery <bytes>` | both | bytes of class objects the design may allocate between collections (default `8388608`). A plain integer — no `k`/`m`/`g` suffix, because no option on this surface has one. `0` is refused: it means `--gc-stress`, and `0` already means "no ceiling" for `--gc-heap-max` |
| `--gc-heap-max <bytes>` | both | soft ceiling in bytes: reaching it asks for a collection at the next opportunity, and passing it anyway is reported rather than refused, since a process may allocate without ever suspending (garbage-collection.md §6.1). `0`, the default, sets no ceiling |
| `--gc-full-every <n>` | both | collect the whole heap on every Nth collection instead of only the objects created since the last one; only a whole-heap pass frees objects that already survived a collection. `0`, the default, lets the runtime decide from how fast the heap is growing |
| `--allow-system <const\|full>` | build, run, lint, synth | permit `$system` (LRM 20.17.1); refused by default |
| `--loose-timescale` | build, run, lint, synth | accept a design that mixes elements declaring a time scale with elements that do not (slang's `MissingTimeScale`, LRM 3.14.2.2); the un-annotated ones take `--timescale`'s value, or `1ns/1ns`. Refused by default |
| `-j, --threads <n>` | build, run, lint, synth | slang's own option; sizes its parser **and** the LLVM compile pool |
| `--dpi-lib <path>` | run | DPI-C `.o`/`.a` (JIT-linked) or `.so` (dlopen'd), repeatable |
| `--perf-jitdump` | run | perf jitdump under `$JITDUMPDIR` |
| `--loose-stale-ref` | run | report a `ref` whose actual storage was restructured under it (data-model.md §3) once per site and keep simulating, instead of the located fatal. Fatal by default |
| `-h, --help` / `--help-all` / `--version` | any | help for the command (or for all of them when none is given); every option including all of slang's; versions (`clotho <release> (<git hash>)`, LLVM, slang) |
| `+<text>` | run | plusarg (LRM 21.6), `+` stripped, in order |

Valued options take `--name value` or `--name=value`. Clotho's options sit in the
same namespace as slang's `-D`, `-I`, `-y`,
`-v`, `-f`, `--top`, `-G`, `--libmap`, `--timescale`, `--std`, `-W`, `--error-limit` and
`-j`, which are options this tool has rather than options it forwards. `-G
<name>=<value>` overrides a parameter of a top-level module (integer and string
values both take effect); a name no top-level module has is not reported, so a
typo simulates the design with its defaults. `--libmap <file>` reads library maps,
which assign source files to libraries; `-L` sets the library priority order and a
`config` can name its own.

"both" in the table means `build` and `run`. The options in it that steer code
generation or what the artifact does when it runs -- `-O`, `--stat`, `--sanitize`,
`--coverage` and the `--gc-*` group -- are the *refused* ones elsewhere: `--help-all`
tags them `[build|run]`, and `lint` and `synth`, which compile nothing, reject
them. The rest of the "both" rows mean something to every command that reads
source, and say so in their `commands` column.

Sources always share one compilation unit (LRM 3.12; `Lower.cpp` forces
`singleUnit`), so `` `define `` macros and `$unit` declarations span files.

### One parser

`main.cpp` registers Clotho's options on the slang `Driver`'s own
`CommandLine` (`register_options`, called before `addStandardArgs` so they head
the help), and the Driver parses the whole line. Consequences:

- **One registry.** `register_options` adds each option through a `Registry`, with
  the commands that take it, and the registry feeds the slang parser, `check_scope`
  (which refuses the option under any other command after parsing) and the
  per-command `--help`. A scope is declared in one place, so the help cannot list
  an option a command then rejects.
- **No passthrough hole.** A mistyped option gets slang's error and its "did you
  mean" hint (`--emti=ir` → `did you mean '--emit'?`, `-O3` → `did you mean
  '-O'?`) instead of being silently handed over as a source file or a slang flag.
- **No duplicate knobs.** Clotho's `--jobs` is gone: slang's `-j/--threads`
  sizes the parser and, via `Driver::options.numThreads`, the LLVM compile pool.
- **The library takes the parsed Driver.** `lower_driver(std::unique_ptr<Driver>)`
  replaces the CLI's use of `lower_sources`, which now only builds a Driver for
  callers that have plain argument lists (tests).

Two things still reach argv before slang: the command word, and runtime plusargs,
which slang would reject as unknown `+` options. A `+arg` that is not one of the
three `+` forms slang registers
(`+define+`, `+incdir+`, `+libext+`) is a plusarg; `--` ends plusarg
interpretation and passes everything after it to slang as given — the escape for
a `+` command slang gained through `--cmd-rename`.

### Help

`--help` is built from the same registrations, in three views:

- `clotho --help`: the command list, then the options every command that reads
  source takes (`--time`, `--allow-system`, `--loose-timescale`, `--help`,
  `--help-all`, `--version`) and the common source options below.
- `clotho <command> --help`: usage (`clotho <command> [options] <files>...`, plus
  `[+plusarg...]` for `run`), then only the options that command takes, then the
  common source options. `lsp` and `dap` take no options besides `--help`,
  `--help-all` and `--version`, so they list no source options.
- `clotho --help-all`, or `--help-all` after any command: every option, Clotho's
  tagged with the commands that take them (`[build|run]`; no tag means every
  command that reads source) and slang's all listed, the Verilog-style `+incdir` /
  `+define` / `+libext` spellings included.

The common source options are slang's `-f`, `-F`, `-I`, `-D`, `-U`, `-y`, `-Y`,
`--libmap`, `-L`, `--top`, `-G`, `--timescale`, `--std`, `-j`, `--error-limit` and
`--color-diagnostics` (`kCommonSourceOptions`). That list only names options: what
each takes and says is read from slang's registration, so it cannot drift.
`CommandLine` can print its options but not enumerate them, so the renderer reads
them back out of `getHelpText` rather than keeping a second list. `-W` and
`--diag-json` are left out of the curated lists on purpose: slang registers them
and Clotho accepts them, but `-W` does not change what is reported for a design
and nothing writes the JSON, so only `--help-all` shows them until they work.

Listings are laid out for 100 columns. An option's spellings get at most 32
columns; a longer one puts its description on the next line, and descriptions
wrap. (slang's own listing pads every description to the longest option name,
65 columns, and does not wrap.)

### Diagnostics

Design diagnostics, slang's and Clotho's alike (including `[unsupported-construct]`
refusals), are rendered by one text client configured as the Driver's:
`--color-diagnostics` forces colour, otherwise colour is used when stderr is a
colour-capable terminal (slang's detection; `NO_COLOR` is not consulted).
`--diag-source`, `--diag-column`, `--diag-location`, `--diag-option` and the other
`--diag-*` options apply to all of them. Diagnostics served to editors are never
coloured. When slang's Driver has already printed a failure (a missing source
file, a bad option), Clotho does not add a second `bad-command-line` line; an
invalid `--top` is not followed by `no top-level modules found`. The exit status is
unchanged.

### Errors, not silent no-ops

An option outside its command's set is a usage error naming the right commands
(`--dpi-lib applies to 'run', not 'build'`; `-O applies to 'build' or 'run', not
'lint'`; `plusarg '+foo' applies to 'run'`). The old
surface accepted `--dpi-lib`, `--perf-jitdump`, `--jobs` and plusargs on every
emit path and ignored them, which contradicts PRINCIPLES.md's never-silently-accept
rule.

A knob that would do nothing is the same error. `lint` and `synth` compile
nothing, so `-O`, `--stat`, `--sanitize`, `--coverage` and the `--gc-*` group are
refused there. The one place they are still accepted without effect is
`build --emit=ir`, which stops before code generation. The `--gc-*` group used to be
refused without `--gc`, back when class objects were collected only on request;
the collector is unconditional now (garbage-collection.md §12a), so every one of
them always means something and none of them needs a companion flag.

### Exit status

| code | meaning |
|---|---|
| 0 | success |
| 1 | compile, emission, or JIT-setup failure |
| 2 | usage error |
| other | `run` only: the simulated design's own status |

`run_jit`'s negative setup sentinel is mapped to 1 rather than returned raw (it
used to reach the shell as 255).

### Version

`clotho --version` starts with `clotho <release> (<git hash>)`, then the LLVM and
slang versions and the license notice. The release is CMake's
`project(clotho VERSION ...)` plus `CLOTHO_VERSION_SUFFIX`, a pre-release tag that
is `alpha` until the first final release and empty after it (`0.1.0-alpha`, then
`0.1.0`). The hash is resolved at configure time and is `unknown` outside a git
tree. `clotho lsp` reports the same string in `serverInfo.version` and `clotho dap`
in its console banner, so a report from an editor names the build as precisely as
the command line does.

## What changed, and why

| before | after | reason |
|---|---|---|
| `--emit=run` | `run` | the mode axis was really a command axis |
| `--emit=opt` | `build --emit=llvm` (default) | "opt" read as an optimization level next to `-O`; this is the target you feed `llc` |
| `--emit=llvm` | `build --emit=llvm-raw` | it is the raw pre-pipeline dump; the name now says so |
| `--emit=ir` | `build --emit=ir` | plus `-o` is finally honoured (it was silently ignored) |
| `--emit=check` | folded into `build` | `build` already elaborates, lowers, analyses and reports the same diagnostics before writing; a diagnostics-only entry point is back as `lint`, where lint severity policy belongs (synthesis-and-lsp-scoping.md §3.4) |
| `--no-z-elision`, `--no-fuse`, `--no-comb-fuse`, `--no-thunk`, `--no-dedup`, `--no-split`, `--no-outline`, `--no-parallel-compile` | removed | they gate as-if optimizations, so the only consumer is the differential test that proves output is byte-identical with the gate off. Those tests set the `SimulationConfig` fields directly; an operator has no reason to reach for one. **Any doc mentioning `--no-<x>` means the `SimulationConfig::<x>` field.** |
| `--domain-spec` | removed with the feature | domain-spec.md |
| `CLOTHO_DEDUP_STAT` (+`=2`), `CLOTHO_COMB_FUSE_STAT`, `CLOTHO_FINISH_STAT`, `CLOTHO_PARALLEL_STAT`, `CLOTHO_DOM_DEBUG` | `--stat=<list>` | five undiscoverable env vars, read by `getenv` deep in the backend, became one documented option and a `Stat` bitmask in `SimulationConfig` |
| `SimulationConfig::optimize` | `lower_coro` | it gates the coroutine pipeline, not optimization; `o3` is the optimization field and the pair read as a contradiction |
| header comment + hand-written usage string | one registration in `register_options` | both copies had drifted (neither listed `--no-split`/`--no-outline`; the header also missed `--dpi-lib` and `--emit=check`). `--help` is generated from the same registry that parses, so an option cannot exist undocumented |
| `--help-slang`, `--slang-arg`, clotho's own `--jobs` | removed | artefacts of running two parsers: a second help to see slang's options, an escape hatch to reach them, and a thread knob duplicating `-j`. One `CommandLine` removes the need for all three |
| `--time` labels `frontend (slang)`, `analysis (ir)`, `llvm compile`, `simulation` | `read source`, `analyze`, `compile`, `simulate` | a label should name what the stage does for the design, not which component does it. `tests/perf/perf.py` scrapes them, so its regexes moved with them |

`--emit=llvm` changing meaning (pre- → post-pipeline) is the one silent
behaviour change; docs quoting the old spellings were migrated with this commit.

### Bugs fixed on the way

- **No `--help`, `-h` or `--version`.** slang's `addStandardArgs` does not
  register them (each slang tool adds its own), so they fell through to the
  Driver and died with `unknown command line argument '--help'`.
- **`--jobs=<junk>` aborted the process** — unguarded `std::stoul` threw
  `std::invalid_argument` (rc 134). Numeric parsing is slang's now, and a bad
  value is a normal option error.
- **`+libext+<ext>` was silently stolen from slang.** The plusarg splitter
  excluded only `+define+` and `+incdir+`, but slang also registers `+libext`,
  so `-y lib +libext+.sv` lost the extension and the module went unfound. The
  three `+` forms slang owns now reach it, and `--` covers any tool-specific
  `+cmd` created by `--cmd-ignore` / `--cmd-rename`.
- **`-o` as the last argument leaked into slang** (`unknown command line
  argument '-o'`); a missing operand is now reported against the option.
- **`--time` measured nothing on the emit path** — `emit_llvm` took no
  `Timings*`, so a `build` reported frontend and analysis only.
- **slang's option set was invisible** — it is half of `--help` now.
- **`--jobs` collided conceptually with slang's `-j`** (parse threads). There is
  one option: `-j/--threads` sizes both the parser and the compile pool.

### Deliberate non-changes

- **`--time`'s numbers and `perf.py`'s JSON keys are unchanged.** Only the
  labels were reworded, so previously recorded numbers stay comparable.
- **slang's options keep their slang names** (`-j`, `--top`, `--timescale`, `-W`).
  Registering them is not the same as renaming them; a user who knows slang
  keeps their muscle memory.
- **Single-unit stays forced.** Documented in `--help` instead of exposed;
  multi-unit has no consumer.
- **`-O` keeps its name.** Familiar, and the only optimization level here.

## Conventions for new options

- A semantics-identical gate for differential testing is a `SimulationConfig`
  field, not a flag.
- A counter or report is a `--stat=` value — never a new flag, never an env var.
- A dynamic check that instruments the emitted code and reports at runtime is a
  `--sanitize=` value, not its own flag. It must leave the design's computed
  results identical to an unsanitized run; anything that changes what the
  design computes is a mode, not a sanitizer.
- A semantics-*changing* mode is a valued option (`--x=<mode>`), never `--no-`.
- Anything that only makes sense during simulation belongs to `run`, and using
  it under `build` must be an error.
- A mode that is *compiled into* the artifact belongs to `build` **and** `run`,
  tagged `[build|run]`, and the two must be given the same one: the `--gc-*` group
  changes the generated code, so `run` cannot add one to what a `build` emitted
  without it. `lint`, which compiles nothing, still refuses them.
- A size is a plain integer number of bytes (`--gc-nursery`, `--gc-heap-max`).
  No `k`/`m`/`g` suffix exists on this surface, and the first option that needs
  one adds it to every size option at once rather than to itself.
- A new option is registered in `register_options`, which is also what `--help`
  prints. Never a second parser, never a second help.
- A knob slang already has is slang's: read it off `Driver::options` instead of
  adding a synonym.
- Help text and messages say what an option does for the design, never how
  Clotho implements it. The exception is an option that exists only for working
  on Clotho (`--stat`, `--emit=llvm-raw`, `--perf-jitdump`), which says so.

`--coverage` is the one carve-out from the `--stat=` rule above, and the line is
whose result it is. A `Stat` bit is a *compiler* statistic: `--stat` is
documented as "for working on clotho", and every bit it has (`dedup`,
`dedup-sets`, `comb-fuse`, `finish`, `parallel`) fires at compile time, prints to
stderr and changes nothing about the emitted code. Functional coverage is a
*design* result — what the design's own stimulus reached (LRM 19) — produced
while the design runs and written to a file the operator names, so it gets its
own valued option, tagged `[build|run]` like the other modes an artifact is
compiled with.

## Out of scope

- `--emit=obj` and a per-definition object cache (module-split.md): needs
  TargetMachine emission and a cache-key design, not a CLI change.
- `--x=` / `--x-init=` (x-semantics.md S3): semantics work; the convention above
  fixes only their spelling.
- A `--vcd <file>` override: `$dumpfile` covers it, and the runtime default
  (`dump.vcd`) is the LRM's.
