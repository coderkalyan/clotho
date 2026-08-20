# CLI: the `clotho` driver surface

Status: **landed**. Replaces the flat option list (`--emit=run|ir|llvm|opt|check`
plus a bag of `--no-*` toggles) with four commands over a single option
namespace shared with slang. `src/main.cpp` is the implementation; this document
is the rationale and the migration record.

## Shape

```
clotho <command> [options] <source>... [+plusarg...]

commands:
  build   compile the design and write the requested artifact
  run     compile the design and simulate it
  lint    analyse the design and report diagnostics, writing nothing
  synth   write the design back out as synthesizable SystemVerilog
  lsp     serve the Language Server Protocol over stdin/stdout
  dap     serve the Debug Adapter Protocol over stdin/stdout
```

The command names how far down the pipeline the run goes, so the first three
nest: `lint` is `build` without an artifact, and `run` is `build` plus execution.
The option sets nest the same way.

`synth` branches rather than nests. It shares the front end and stops before the
LLVM backend entirely -- `clotho_legalize` links no LLVM and no slang, and the
simulator does not link `clotho_legalize` -- so it is a peer of `build`, not a
stage of it. It is the only command whose artifact is source rather than
generated code. It is required and must be `argv[1]`:
`clotho design.sv` is a usage error, not an implicit `run`, because implicit
modes are how the old surface drifted.

`lsp` and `dap` are servers, not pipeline stages, and take **no arguments at
all** -- not even a source file. An editor supplies the language server's file
set, and the debug adapter's design arrives in the launch request's `args`,
which are a `run` command line passed through to the design process verbatim
(`docs/design/dap.md` §4.4). Accepting `-D` / `-y` / `-f` here and ignoring
them would be the silent acceptance the project forbids, so both refuse every
argument except `--help` and `--version` (`src/main.cpp`).

`clotho --help` prints one option list: Clotho's options first, then slang's.
There is no second help and nothing to forward — see "One parser" below.

### Options

| option | commands | effect |
|---|---|---|
| `--emit <target>` | build | `llvm` (default, post-coroutine-pipeline), `llvm-raw` (straight from the backend, `llvm.coro.*` intact), `ir` (Clotho process IR) |
| `--target <lang>` | synth | `sv` (default, IEEE 1800), `2005` (IEEE 1364-2005: `reg`/`wire`, no wildcard equality, no jump statements) |
| `--hierarchy <mode>` | synth | `instances` (default) writes one module per instance with names as written; `flat` writes one module with paths mangled into identifiers |
| `--sidecar <path>` | synth | Also write the JSON sidecar: the original-to-emitted name map, and each signal's inferred storage, clock and reset |
| `--no-legalize` | synth | Translate as written instead of writing out the implied hardware: no mux trees rebuilt, no implicit branch filled in |
| `--full-case` | synth | Believe `full_case` / `unique`; reported wherever the claim is what removed a latch |
| `--parallel-case` | synth | Believe `parallel_case` / `unique0` |
| `-o, --output <file>` | build | artifact path for every target; `-` (default) is stdout |
| `--time` | both | per-phase wall-clock to stderr |
| `-O` | both | `default<O3>` plus aggressive machine codegen |
| `--stat <list>` | both | `dedup`, `dedup-sets`, `comb-fuse`, `finish`, `parallel` |
| `--sanitize <list>` | both | dynamic bug checks instrumented into the emitted code, reported to stderr and non-fatal: `bounds` (an index outside what it indexes — array element, packed bit/part select, string character, queue method index, pop from an empty queue; x/z index included), `xprov` (xsan, x provenance — not yet implemented, so **Debug builds only**, `CLOTHO_XSAN`; a shipped build rejects the name). Off by default |
| `--gc[=<mode>]` | both | collect class objects instead of leaking them: `off` (default), `on`, `stress` (collect at every opportunity, so a missed root fails immediately; for working on clotho). Bare `--gc` means `on`; the mode attaches with `=` only (`--gc on` is a usage error, see the conventions below). **Changes the generated code** (garbage-collection.md), so a `build` and the `run` of what it built must be given the same mode |
| `--gc-stats` | both | collection summary when the design exits: how many collections, how much was reclaimed, how long the design was paused. Needs `--gc` |
| `--gc-nursery <bytes>` | both | bytes of class objects the design may allocate between collections (default `8388608`). A plain integer — no `k`/`m`/`g` suffix, because no option on this surface has one. `0` is refused: it means `--gc=stress`, and `0` already means "no ceiling" for `--gc-heap-max`. Needs `--gc` |
| `--gc-heap-max <bytes>` | both | soft ceiling in bytes: reaching it asks for a collection at the next opportunity, and passing it anyway is reported rather than refused, since a process may allocate without ever suspending (garbage-collection.md §6.1). `0`, the default, sets no ceiling. Needs `--gc` |
| `--allow-system <const\|full>` | both | permit `$system` (LRM 20.17.1); refused by default |
| `--loose-timescale` | both | accept a design that mixes elements declaring a time scale with elements that do not (slang's `MissingTimeScale`, LRM 3.14.2.2); the un-annotated ones take `--timescale`'s value, or `1ns/1ns`. Refused by default |
| `-j, --threads <n>` | both | slang's own option; sizes its parser **and** the LLVM compile pool |
| `--dpi-lib <path>` | run | DPI-C `.o`/`.a` (JIT-linked) or `.so` (dlopen'd), repeatable |
| `--perf-jitdump` | run | perf jitdump under `$JITDUMPDIR` |
| `--loose-stale-ref` | run | report a `ref` whose actual storage was restructured under it (data-model.md §3) once per site and keep simulating, instead of the located fatal. Fatal by default |
| `-h, --help` / `--version` | any | help; versions (clotho git hash, LLVM, slang) |
| `+<text>` | run | plusarg (LRM 21.6), `+` stripped, in order |

Valued options take `--name value` or `--name=value`, with one exception:
`--gc`, whose value may be omitted, takes it only as `--gc=<mode>`, and `--gc on`
is a usage error rather than either of its two readings (see the conventions
below). Clotho's options sit in the same namespace as slang's `-D`, `-I`, `-y`,
`-v`, `-f`, `--top`, `-G`, `--timescale`, `--std`, `-W`, `--error-limit` and
`-j`, which are options this tool has rather than options it forwards.

"both" in the table means `build` and `run`. The `--gc` group is the only such
set that is *refused* elsewhere, because it is the only one that means something
specific to compiling and running rather than nothing at all: `--help` tags it
`[build|run]`, and `lint`, which compiles nothing, rejects it.

Sources always share one compilation unit (LRM 3.12; `Lower.cpp` forces
`singleUnit`), so `` `define `` macros and `$unit` declarations span files.

### One parser

`main.cpp` registers Clotho's options on the slang `Driver`'s own
`CommandLine` (`register_options`, called before `addStandardArgs` so they head
the help), and the Driver parses the whole line. Consequences:

- **One `--help`**, generated by `cmdLine.getHelpText`, listing both sets. A
  `[build]` / `[run]` tag in a description marks a command-specific option and
  `[build|run]` one two commands share, and `check_scope` enforces it after
  parsing.
- **No passthrough hole.** A mistyped option gets slang's error and its "did you
  mean" hint (`--emti=ir` → `did you mean '--emit'?`, `-O3` → `did you mean
  '-O'?`) instead of being silently handed over as a source file or a slang flag.
- **No duplicate knobs.** Clotho's `--jobs` is gone: slang's `-j/--threads`
  sizes the parser and, via `Driver::options.numThreads`, the LLVM compile pool.
- **The library takes the parsed Driver.** `lower_driver(std::unique_ptr<Driver>)`
  replaces the CLI's use of `lower_sources`, which now only builds a Driver for
  callers that have plain argument lists (tests).

Three things still reach argv before slang: the command word; runtime plusargs,
which slang would reject as unknown `+` options; and a bare `--gc`, rewritten to
`--gc=on` because slang's `CommandLine` has no optional-value option (every
non-boolean one demands a value, and a boolean one would reject `stress`). The
rewrite feeds the same registered option, so there is still one parser and one
help. A `+arg` that is not one of the three `+` forms slang registers
(`+define+`, `+incdir+`, `+libext+`) is a plusarg; `--` ends plusarg
interpretation and passes everything after it to slang as given — the escape for
a `+` command slang gained through `--cmd-rename`.

### Errors, not silent no-ops

An option outside its command's set is a usage error naming the right command
(`-o applies to 'build', not 'run'`; `plusarg '+foo' applies to 'run'`). The old
surface accepted `--dpi-lib`, `--perf-jitdump`, `--jobs` and plusargs on every
emit path and ignored them, which contradicts the project's never-silently-accept
rule.

A knob that would do nothing is the same error. `--gc-stats`, `--gc-nursery` and
`--gc-heap-max` without `--gc` (or with `--gc=off`) are refused rather than
accepted and dropped, and `--gc on` is refused rather than read as either
`--gc=on` plus a source file named `on` or as a value the parser cannot take.

### Exit status

| code | meaning |
|---|---|
| 0 | success |
| 1 | compile, emission, or JIT-setup failure |
| 2 | usage error |
| other | `run` only: the simulated design's own status |

`run_jit`'s negative setup sentinel is mapped to 1 rather than returned raw (it
used to reach the shell as 255).

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
  tagged `[build|run]`, and the two must be given the same one: `--gc` changes
  the generated code, so `run` cannot add it to what a `build` emitted without
  it. `lint`, which compiles nothing, still refuses it.
- An option whose value may be omitted takes that value with `=` only (`--gc`,
  `--gc=stress`). slang's `CommandLine` has no optional-value option, so the
  bare form is rewritten to the valued one before the Driver parses; the space
  form is a usage error, since it would otherwise swallow the next source file.
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

## Out of scope

- `--emit=obj` and a per-definition object cache (module-split.md): needs
  TargetMachine emission and a cache-key design, not a CLI change.
- `--x=` / `--x-init=` (x-semantics.md S3): semantics work; the convention above
  fixes only their spelling.
- A `--vcd <file>` override: `$dumpfile` covers it, and the runtime default
  (`dump.vcd`) is the LRM's.
