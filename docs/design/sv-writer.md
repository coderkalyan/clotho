# S7: the SystemVerilog writer

Scoping. No big blockers; two real limitations, both named in §7 and neither fatal
to a useful v1.

## 1. What it is, and what already exists

`legalize::materialize` (M4) is the prototype: it renders a legalized design and its
round trip over picorv32 and hazard3 is exact — 530/530 and 1676/1676 signals
re-classify identically. That proves the *description* is faithful. S7 is that
renderer productised, and the three things it adds are structure retention, a
legalization-free path, and a target language contract.

Measured baseline, hazard3: **27.0 ms for 3.46 MB — 128 MB/s**.

## 2. Two input modes (goal 3)

```cpp
std::string emit(const ir::Module&, const legalize::Result* = nullptr, SynthConfig = {});
```

**Without a `Result` — 1:1 structural translation.** Walk each process's region tree
and print it: `cond` becomes `if/else`, `switch` becomes `case`, `loop` becomes
`for`, stores become assignments. No analysis, no guarded lists, no reordering. This
path is *simpler* than the legalized one and it already exists in miniature —
`emit_fn_body` renders function bodies exactly this way, because a function needs no
analysis to print: **the region tree already is the statement tree.**

**With a `Result` — legalized rendering.** Per written signal, the guarded list is
rebuilt into a mux tree and the implicit arms are filled in (M4's behaviour). This is
where `full_case` collapses a latch, where a missing arm becomes an explicit `else q
<= q`, and where storage classification can annotate the output.

The two share one printer and one expression emitter. The difference is only which
statement source is walked: the region tree, or the guarded lists.

## 3. The printer (goal 1)

The current 128 MB/s is allocation-bound, and the causes are all structural:

| cause | fix |
|---|---|
| `std::optional<std::string>` returned per expression node | `void emit_expr(Out&, ir, Index)` straight into one buffer |
| `std::ostringstream` per **constant** | hand-rolled hex/binary formatter |
| fully parenthesized output | precedence-aware printing |
| failure threaded as `optional` through every node | `can_render(ir, Index)` pre-pass, memoised per index |

Precedence awareness matters twice: `((a & b) | (c & d))` costs bytes to produce,
bytes to store and bytes to re-parse, and hazard3's 3.46 MB is mostly parentheses.
Expect a large size reduction alongside the speed.

**Target: ≥500 MB/s, and a smaller output** — under 7 ms for hazard3. Nothing here is
clever; it is removing allocation from a loop that runs millions of times.

Output goes to a caller-supplied sink (`std::string`, `FILE*`, or a span) so a large
design never needs the whole text resident.

## 4. Structure retention (goal 2)

What survives lowering, checked rather than assumed:

| construct | survives? | how |
|---|---|---|
| instance hierarchy | **yes** | `Module::tops()` → `InstanceScope{name, def_name, signals, children}` |
| process → instance | **yes** | `Process::scope` |
| signal → instance | **yes** | `InstanceScope::signals`, in declaration order |
| port direction | **yes** | `SignalInfo::dir` |
| port connectivity | **yes, indirectly** | each connection lowers to a one-store copy process |
| declaration order | yes | signal table and scope lists are ordered |
| source locations | yes | per-instruction `SourceLoc` + `SourceManager` |
| statement structure | yes | the region tree is the statement tree |
| parameters | **no** | folded at elaboration; `i < W` arrives as `constant(8)` |
| generate blocks | **no** | folded into the parent instance; survive only as `name[3]` substrings |
| definition identity | **no** | a module instantiated twice is lowered twice |

Port connectivity is the pleasant surprise. `child u1(.clk(clk), .d(a), .q(mid))`
lowers to three continuous-assign processes — `u1.clk = clk`, `u1.d = a`,
`mid = u1.q` — so the port map is recoverable by recognising single-store, unguarded
processes whose RHS is one signal load crossing a scope boundary. Reconstruct the
map, elide the processes.

Three hierarchy modes, since the machinery differs sharply:

* **`Flat`** — one module, mangled hierarchical names. What M4 does today, already
  round-trip proven. Fast, correct, and unusable in a real flow (§7.3): SDC,
  physical scripts and UPF all key on hierarchical paths.
* **`Instances`** — one module per instance, port maps reconstructed, names exact.
  Usable downstream. Output is no larger than flat; it is the same content nested.
* **`Deduped`** — instances whose process set and signal types hash identically share
  one definition. Smaller output, and the interim §3.7 recommends until
  definition-level lowering lands.

`Flat` is the fallback and the test oracle; `Instances` is the default worth aiming
at, because a writer whose output cannot carry a constraint file is a demo.

**Name fidelity is a hard requirement, not a nicety** (§7.3). Every rename that
cannot be avoided goes in the sidecar as `original path -> emitted path`,
bidirectional.

## 5. Verilog-2005 (goal 4)

Slang has `LanguageVersion::v1364_2005`, so compliance is verifiable *in process*:
re-parse the emitted text with that version and require zero errors. That becomes an
acceptance test over the corpus and both CPUs, not a claim.

Concrete gaps in what M4 emits today:

| emitted now | 2005 |
|---|---|
| `logic [7:0] x;` | `reg` when procedurally assigned, `wire` when continuously driven |
| `==?` (from `casez`) | not in 2005 — expand to `(a & mask) === (pat & mask)`, or re-emit the `casez` |
| `'x`, `'h0` unsized | sized: `8'bx`, `2'h0` |
| `function automatic logic [7:0] f(...)` | `function [7:0] f;` with separate `input` declarations |
| `always @(a or b)` | fine |
| `x[i +: 8]` | fine — indexed part-select is 2001 |

`reg` vs `wire` is the only one needing analysis, and it is already available: a
signal with a procedural store is `reg`, one driven only by a continuous assign is
`wire`. `SignalInfo::is_net` records the original choice.

`==?` is the interesting one. It appears only because a `casez` lowered to a cond
chain; the masked-`===` expansion is mechanical and exact, since the pattern is a
constant whose z bits *are* the mask.

## 6. `SynthConfig`

Mirrors `SimConfig` — a plain struct of knobs in `src/ir/`, defaulted so `{}` is the
sensible thing, one field per decision that is genuinely contested.

```cpp
struct SynthConfig {
    enum class Target : uint8_t { V2005, SV2017 };
    enum class Hierarchy : uint8_t { Flat, Instances, Deduped };

    Target    target    = Target::SV2017;
    Hierarchy hierarchy = Hierarchy::Instances;

    // Forwarded to legalize when a Result is being produced.
    bool honour_full_case     = false;
    bool honour_parallel_case = false;

    // Fill implicit arms (M4's renderer). Off means 1:1 structural translation
    // even when a Result is supplied.
    bool materialize_mux_trees = true;

    // `original -> emitted` name map plus the classification/mismatch JSON.
    bool emit_sidecar = true;
    // `// file:line` on each process and declaration.
    bool emit_source_locs = false;

    uint32_t indent_width  = 2;
    uint32_t wrap_column   = 100;   // 0 disables wrapping
};
```

## 7. Limitations, stated

**Generate blocks cannot be re-emitted as generates.** §7.3 requirement 2 asks for
this and it is not reachable from the IR: generate identity is folded into the parent
instance at lowering and survives only as a substring of the flattened name. Fixing
it is frontend work (retain generate-block scope nodes), not emitter work. v1 emits
the unrolled instances with their exact original names, which keeps *names* faithful
even though *structure* is not.

**Definitions are duplicated unless hashed.** A module instantiated twice is lowered
twice, so `Instances` emits two definitions. `Deduped` fixes it by content hash; full
fidelity needs §3.7's definition-level lowering, which is deferred.

**Parameters are resolved.** Widths become literal. §7.2 chose this deliberately —
"parameters resolved, widths explicit everywhere" is the normalized subset — but it
does mean the output is not re-parameterizable.

None of the three blocks a v1 that emits legal, name-faithful, downstream-usable
Verilog. They bound how *rolled* the output is, not whether it is correct.

## 8. Milestones

| | | acceptance |
|---|---|---|
| **W1** | Fast printer: buffer sink, no per-node allocation, precedence-aware, hand-rolled literals | ≥500 MB/s on hazard3, output smaller, round trip still exact |
| **W2** | 1:1 structural path (no `Result`) | every process re-lowers to an identical region tree; works on both CPUs |
| **W3** | `SynthConfig` + Verilog-2005 target | emitted text re-parses under `v1364_2005` with zero errors, corpus + both CPUs |
| **W4** | `Instances` hierarchy: port-map reconstruction, exact names | re-lowered design has the same `InstanceScope` tree, same signal names |
| **W5** | Sidecar: name map + classification JSON | every emitted name maps back; classification matches `Result` |
| **W6** | `Deduped` hierarchy by content hash | hazard3 emits one `hazard3_alu`, not one per instance |

W1–W3 are independently shippable and need no new IR. W4 is the first that needs
process classification beyond legalization (recognising port copies). W6 is the only
one that would benefit from §3.7 landing first, and does not require it.

The existing round trip is the regression test throughout: render, re-lower,
re-legalize, require identical classification. It has already caught five renderer
defects the 28-case corpus could not reach, and every milestone above keeps it green.
