# DPI-C — direct C function and task imports

**Status:** implemented. **Import**: `import "DPI-C" [pure] [context] function` over
scalar, packed-vector, `chandle`, and `string` types, `input`/`output`/`inout` args,
scalar/chandle/string return, plus `import "DPI-C" [context] task` (LRM 35.8) over
the same formals. **Export**: `export "DPI-C" function` **and** `export "DPI-C" task`
(C calls into SV) — `input`/`output`/`inout` scalar/chandle/real/vec args,
`input` string, scalar/chandle/real/void return, any number of tops. `--dpi-lib`
accepts a `.o`/`.a` (JIT-linked, needed for export) or a `.so` (dlopen, import only —
refused outright when the design exports).
**DPI tasks are phase T1 — the zero-time case.** Both directions work end to end
(byte-identical to verilator, see "DPI tasks" below); an exported task that
actually *suspends* is a located runtime fatal, not a wrong answer, because
parking the native C frame is phase T2. `pure` on an imported task is refused
permanently (LRM 35.5.1.3).
1-D **open arrays** (`[]` formals, LRM 35.5.6.2) are supported in all directions
(`input`/`output`/`inout`): named-scalar elements are zero-copy (Clotho's storage is
the C array), packed `bit`/`logic` elements marshal through autovectorized
width-specialized C loops (≤ 64 bits) or inline LLVM IR (wider).
**svdpi.h runtime**: the packed-vector bit/part-select accessors
(`svGet/PutBitselBit/Logic`, `svGet/PutPartselBit/Logic`, `svDpiVersion`), the
per-top scope API (`svGetScope`/`svSetScope`/`svGetNameFromScope`/
`svGetScopeFromName`/`svPutUserData`/`svGetUserData`), and the open-array accessors
(`svSize`/`svLeft`/`svRight`/`svLow`/`svHigh`/`svIncrement`/`svGetArrElemPtr1`/
`svGet/PutBit|LogicArrElem1VecVal`/…) are implemented (`src/runtime/svdpi.h`). Still
**loud-fails** / unresolved with a TODO: import `output`/`inout` string, multi-dim /
dynamic open arrays, open-array export args, a **suspending** exported task (T2) and
the LRM 35.9 **disable protocol** (T3) — both **scoped in full** under "DPI tasks"
below — export string return + `output`/`inout` string, one
c_identifier exported from several scopes (blocked upstream in slang), `svScope`
handles for non-top instances, and `svGetCallerInfo` / the disable API.

This doc is written design-first: the early sections capture the original
scalar-first plan and its rationale, and the **Directions — phasing** and **Phase
3** sections below record what actually landed (Phase 1 scalar in + return, Phase
2 scalar out/inout, Phase 3 packed vectors). Where they differ, the phasing
sections are authoritative.

DPI-C (LRM 35) lets SystemVerilog call a native C function directly:

```systemverilog
import "DPI-C" function int add(input int a, input int b);
initial $display("%0d", add(2, 3));   // 5, computed in C
```

```
clotho run --dpi-lib libmodel.so design.sv   // add() resolved from the .so
```

## Why this scope, and why it is the performance floor

The DPI canonical ABI splits sharply by argument type:

- **Scalar-mapped types** (LRM Table 35-1: `byte`/`shortint`/`int`/`longint`,
  `real`/`shortreal`, `chandle`, scalar `bit`/`logic`) map to native C scalars
  passed **by value**. Clotho's internal 2-state narrow value *is* the C scalar —
  the boundary is a register move (`trunc`/`zext`/`bitcast`), no scratch buffer,
  no pointer indirection, no runtime helper.
- **Everything wider** (packed vector arrays, open arrays, strings) maps to
  `svBitVecVal*` / `svLogicVecVal*` / `char*` — chunked 32-bit arrays passed by
  pointer, requiring a de-interleave marshal (`bval = must^may`,
  `aval = may & ~z`, then rechunk to 32-bit words) into a scratch buffer on every
  call.

The scalar path is both the common case for reference-model DPI and the only
zero-overhead one. Nailing it first fixes the architecture at its fastest point:
a DPI scalar call must lower to a direct `call @cid(...)` with C-native parameter
types, never routed through the wide marshal. The wide path is a separate,
clearly-bounded follow-up that the scalar path never pays for.

Subtlety that decides the boundary: the SV *type kind* selects the path, not the
width. `int` is scalar-fast; `bit [31:0]` (a packed array, same 32 bits) is
`svBitVecVal*` slow. The guard keys on the type kind.

## Starting point (historical)

At design time a DPI import failed loudly in `function_id`
(`fail("DPI imported subroutines are not supported")`); `build_dpi_import` now
replaces that stub. The implementation reused the ordinary user-subroutine call
path: `lower_user_call`
(`lower/Subroutines.cpp`) → `build_call_args` (per-direction marshal) → `b.call`
→ `emit_call_copyouts` (output/inout copy-back). The runtime-call
primitive is `rt_fn` (`ir/llvm/RuntimeAbi.cpp`, `getOrInsertFunction` + `CreateCall`).
JIT symbol resolution injects `rt_*` by absolute address and already adds one
`DynamicLibrarySearchGenerator` for libgcc builtins (`Jit.cpp:248-254`).

## What slang gives us

A DPI import is an ordinary `ast::SubroutineSymbol` flagged
`MethodFlags::DPIImport` (plus `DPIContext` / `Pure`). slang already type-checks
DPI legality (`Type::isValidForDPIArg`/`isValidForDPIReturn`) and emits the
diagnostics; we consume them, not re-validate.

**One gap:** the C linkage name (`import "DPI-C" foo = function int bar(...)`) is
not stored on the symbol. Recover it from the syntax, mirroring slang's own rule
(empty ⇒ SV name):

```cpp
std::string_view cid = sub.name;
if (auto* s = sub.getSyntax())
    if (auto* d = s->as_if<syntax::DPIImportSyntax>())
        if (d->c_identifier)
            cid = d->c_identifier.valueText();
```

## Scalar type map (the entire PoC surface)

| SV type | C type | LLVM param | Clotho → C (call) | C → Clotho (return) |
|---|---|---|---|---|
| `byte` | `char` | `i8` | `must_of` (trunc if wider) | zext to i8 slot |
| `shortint` | `short` | `i16` | `must_of` | — |
| `int` | `int` | `i32` | `must_of` | — |
| `longint` | `long long` | `i64` | `must_of` | — |
| `real` | `double` | `double` | `Plane.fp` (bitcast slot) | to fp |
| `shortreal` | `float` | `float` | `Plane.fp` | to fp |
| `bit` (scalar) | `svBit` = `unsigned char` | `i8` | zext `i1` must | `& 1` → i1 |
| `logic` (scalar) | `svLogic` = `unsigned char` | `i8` | plane → 0/1/2/3 | 0/1/2/3 → plane |
| `chandle` | `void*` | `ptr` | `inttoptr` i64 handle | `ptrtoint` → i64 |
| `string` (input/return) | `char*` | `ptr` | `rt_string_data` (zero-copy) | `rt_dpi_str_out` (copy-in) |

`chandle` and `string` details are in their own sections below.

2-state types (`byte`…`longint`, `bit`) take `must_of` (the value plane; for a
narrow 2-state slot `mm` *is* the value) and truncate/extend to the C width —
essentially free.

**The one non-trivial coercion — scalar `logic` ↔ `svLogic`.** svdpi.h encodes
`sv_0=0, sv_1=1, sv_z=2, sv_x=3`, which is **not** Clotho's plane order. Clotho→C:
`z ? 2 : (may ? (must ? 1 : 3) : 0)`. C→Clotho: a 2-bit switch back to
`{must, may, z}`. A handful of selects, per scalar `logic` arg only.

## IR & ABI design

Reuse the existing `call` node and `FunctionId`. A DPI import reserves a
`FunctionId` whose `Function` carries:

- `extern_name` — the C linkage name (`cid` above).
- `is_dpi = true`, `is_pure` (from `MethodFlags::Pure`).
- `param_types` / `arg_dirs` as usual, plus the per-param + return **C scalar
  descriptor** (which of the table rows) so the backend knows the native type and
  coercion without re-deriving from the SV type.
- **empty IR body** — there is nothing to lower; the body is native.

Frontend (`function_id`, DPI branch replacing the stub): validate every arg and
the return are scalar-mapped types (loud-fail otherwise, see guards), build the
`Function` record, cache the `FunctionId`. `lower_user_call` is **unchanged** —
`build_call_args` produces the arg operands coerced to the formal SV type exactly
as for an internal call; `b.call(id, args)` emits the same node.

Backend, keyed on `is_dpi`:

- **`declare_function`** — emit the external C signature (`getOrInsertFunction`
  with the native param/return types from the descriptor), **no** leading
  `ptr state`, no plane-pair expansion. Attach `readnone` + `willreturn` when
  `is_pure` so `-O` can CSE / hoist / DCE the call.
- **`lower_call_inst`** — for a DPI callee, convert each argument `Plane` to its C
  scalar (table above), `CreateCall` the external symbol directly, convert the C
  return back to a result `Plane`. No `svVecVal`, no scratch, no runtime shim.

### Directions — phasing

- **Phase 1 (landed): `input` args + scalar return.** Pure by-value fast path, no
  pointers, no scratch — the architectural centerpiece; benchmarked at ~1.8 ns per
  call (tier-0), below a native integer add.
- **Phase 2 (landed): scalar `output`/`inout`.** The C ABI passes these by pointer
  (`int*`). The frontend is unchanged — `build_call_args` already makes a caller
  temp and records a `CopyOutArg` for any non-`In` direction, callee-agnostic; only
  `build_dpi_import` had to *allow* the directions (record them in `arg_dirs`,
  classify the scalar type as before). The backend does the boundary:
  - `declare_dpi_import` emits a `ptr` param for an `output`/`inout` arg (pointee is
    the native C scalar), the by-value scalar for `input`.
  - `lower_dpi_call` allocates the C scratch via **`entry_alloca`** (hoisted to the
    function/coroutine entry, so a DPI call in a loop does *not* grow the stack per
    iteration). For `inout` it copies the caller temp in (`lower_load` → `dpi_arg_to_c`
    → store scratch). It passes the scratch pointer, and after the call loads the
    scratch, converts (`dpi_c_to_plane`), and `lower_store`s it back into the caller
    temp — which `emit_call_copyouts` (frontend IR) then copies to the actual with
    the usual width/lvalue coercion. Bounded, scalar-only; reuses the exact
    load/store-through-lvalue helpers the normal call ABI uses.

### Phase 3 (landed): packed-vector marshalling

`bit[N]`/`logic[N]` wider than a scalar map to `svBitVecVal*` / `svLogicVecVal*` —
32-bit chunk arrays passed by pointer, in **every** direction (a vec is never
by-value). A vec is **never a return type** — slang rejects a packed-array DPI
return (LRM 35.5.5 "small value"), so this is args-only. As landed:

- **`svBitVecVal` (2-state) is inline, no helper.** The value plane spilled to a
  32-bit-chunk buffer *is* the canonical layout, so `dpi_vec_copyin` just stores
  `must_of(pl)` (zero-extended) and `dpi_vec_copyout` loads it back and truncates.
- **`svLogicVecVal` (4-state) uses the runtime helper.** The backend spills
  must/may/z to word buffers and calls `rt_dpi_pack_logicvec` (which computes
  `aval = may & ~z`, `bval = must ^ may` and interleaves into `{aval,bval}` pairs);
  `rt_dpi_unpack_logicvec` reverses it. All chunk/endianness/partial-top-chunk
  logic is in that one `-O2` C function.
- Every scratch is `entry_alloca`-hoisted (no per-iteration stack growth in a
  loop). The `output`/`inout` writeback reuses the Phase 2 `CopyOutArg` temp path,
  and the z-elision seeding from Phase 2 (`local_z[k]=1` for 4-state DPI params)
  already covers a `logic[N]` output producing Z.
- Covered by the vec matrix: `bit[63:0]` in/out/inout, `logic[7:0]`
  output carrying `01xz01xz` (all four states), `logic[7:0]` echo preserving x/z,
  and multi-chunk / non-32-aligned widths (`bit[99:0]`, `logic[71:0]`,
  `logic[47:0]`). Automated tests stay on libc scalars (no `.so`); the vec matrix
  is exercised at the CLI.

The original design rationale (why a runtime helper, not inline IR):

The gap: Clotho internal 4-state = `mm` (i2W: must low, may high) + separate `z`
(iW); DPI `svLogicVecVal` = `{aval,bval}` 32-bit pairs. Exact bijection:

```
clotho -> svLogicVecVal:  aval = may & ~z ,  bval = must ^ may
svLogicVecVal -> clotho:  must = aval & ~bval ,  may = aval | bval ,  z = bval & ~aval
2-state <-> svBitVecVal:  the value (must) plane chunked 32 bits at a time
```

Do the plane-split + word-chunk in `runtime/` (`rt_dpi_pack_{bit,logic}vec` /
`rt_dpi_unpack_*`), **not** inline IR. Rationale: (1) compile speed / IR footprint —
inline is O(width/32) instructions per arg at FastISel; a helper is one `call` + a
few plane-spill stores regardless of width; (2) correctness containment — the
chunk interleave, endianness, and partial-top-chunk masking live in one `-O2` C
file, testable in isolation; (3) reuse the `$display` pattern — `build_rt_args`
already spills wide planes to a stack word-buffer and hands the runtime pointers;
the DPI marshal is the same shape (spill planes → alloca svVecVal scratch (via
`entry_alloca`) → `rt_dpi_pack_*` → pass pointer → for out/return `rt_dpi_unpack_*`
back into a plane buffer → load into the result `Plane`).

Keep the scalar path inline (unchanged) — only vectors touch the helper, so the
~1.8 ns scalar floor is not regressed. A ≤32-bit (single-chunk) inline fast-path is
a *documented follow-up*, added only if a benchmark shows a hot narrow-vector DPI
call — start helper-only (one code path).

Deferred beyond Phase 3: `string` (`char*` ↔ `rt_string`), open arrays
(`svOpenArrayHandle` — a descriptor + the `svGetArrElemPtr` accessor API, its own
task), and `context`/`svScope`/`export` (reverse thunks so C can call back into
SV — orthogonal to marshalling).

## chandle (landed)

A `chandle` (LRM 6.14) is an opaque C pointer — the canonical DPI pattern for a
stateful reference model: C `new`s an object, hands back the pointer, SV holds it
and passes it back to the C methods. The only legal SV operations are `null`,
assignment, and equality (`==`/`!=`); slang rejects everything else.

**Representation — a 2-state `i64`, no new pool kind.** Every legal chandle op is
an integer op on the handle bits: `null` is the constant 0, assignment is a copy,
equality is an integer compare. Since slang enforces the restriction, clotho never
sees arithmetic on a chandle, so the minimal representation is exact:

- `lower_type`: `isCHandle()` → `make_int_type(64, false, false)`.
- `lower_expr`: `ExpressionKind::NullLiteral` → a 0 constant of the expression's
  (chandle) type. (This also lights up `h == null` and `h = null` for free — they
  are ordinary integer compare / copy over the handle bits. Class/event `null`
  handles are separate; those types are not lowered and stay guarded.)

This deliberately uses no `Type::Kind::CHandle`: a distinct kind would force every
`Type::Kind` switch (layout, slot type, display) to grow a case for a type whose
only observable behavior is that of a 64-bit 2-state value. "Simplest logic that
is LRM-correct" (PRINCIPLES.md) wins here — slang is the guard.

**DPI marshal — `DpiCType::Chandle` ↔ C `void*` (`ptr`).** A chandle is a *scalar*
(not a vec), so it rides the existing scalar path in every direction with two
one-line conversions:

- `dpi_c_llvm_ty(Chandle)` = `ptr`.
- `dpi_arg_to_c(Chandle, pl)` = `inttoptr(must_of(pl), ptr)` (i64 handle → `void*`).
- `dpi_c_to_plane(Chandle, r)` = `two_val(ptrtoint(r, i64))` (`void*` → i64 handle).

Directions fall out of Phase 1/2: `input` passes the `void*` by value; `output`/
`inout` use the Phase 2 scratch — a `ptr` slot the C writes through (`void**`);
`return` is `ptrtoint` of the C result. A chandle **is** a valid DPI return "small
value" (unlike a packed vector), so the return guard admits it.

## string (landed: input arg + return)

`string` ↔ C `char*` (`input` → `const char*`). Unlike a packed vector, `string`
is a valid DPI return "small value", so `import "DPI-C" function string f()` works.
It is neither a scalar nor a vec — it is the `rt_string` handle — so it gets a
`DpiCType::String` and its own arm in `lower_dpi_call`, always passed as `ptr`.

**Enabled by the `rt_string` NUL invariant** (`byte[len] == 0` — see the SSO block
in `runtime/RuntimeInternal.h`). Because `rt_string_data()` is now a valid C string:

- **Input string is zero-copy**: `argv.push_back(rt_string_data(str_of(a)))` — pass
  the pointer directly, no arena, no copy. The C reads it during the call and must
  not retain it (DPI `input` forbids that anyway).
- **Return string copies out**: the C returns a NUL-terminated `char*` (C-owned);
  `rt_dpi_str_out(dst, p)` does `rt_string_assign_bytes(dst, p, strlen(p))` into the
  call's result handle (`str_of(i)`, allocated by `build_str_temps`).

**`output`/`inout` string is deferred** — loud-fails. The C writes an unbounded
NUL-terminated `char*` back and DPI provides no size contract, so there is no safe
fixed buffer clotho can pass (a fixed max = silent truncation/overflow). Revisit
only with a concrete tool-matched convention. (Input, and the copy-*in* half of an
inout, would be free; it is the copy-*out* that has no safe shape.)

## JIT symbol resolution

One repeatable CLI flag, `--dpi-lib <path>`, dispatched by **file format** to one
of two ORC loaders. The split is not import-vs-export — it is *sealed shared
object* vs *relocatable object*, which differ in their linking model:

- **`.so` (a fully-linked shared object) → `DynamicLibrarySearchGenerator::Load`**
  (`dlopen`). The OS dynamic linker resolves the `.so`'s own undefined symbols
  against the *process* at load time, before ORC sees it; the ORC generator only
  reads symbols *out* of it (`dlsym`) on a JIT lookup. **One-way: import only.**
  ```cpp
  auto lib = llvm::orc::DynamicLibrarySearchGenerator::Load(
      path.c_str(), (*jit)->getDataLayout().getGlobalPrefix());
  jd.addGenerator(std::move(*lib));   // Orc/ExecutionUtils.h, already included
  ```
- **`.o` / `.a` (a relocatable object) → `LLJIT::addObjectFile(jd, buf)`.** ORC's
  *own* linker resolves the object's undefined symbols against the JITDylib — so
  the C's `import` calls bind to the design and the C's `export` calls bind to the
  JIT-compiled export thunks (§ export). **Two-way: import *and* export, in one
  link image.**
  ```cpp
  auto buf = llvm::MemoryBuffer::getFile(path);
  (*jit)->addObjectFile(jd, std::move(*buf));
  ```

**The `.o` path is the canonical one** — strictly more capable (both directions)
and simpler to produce (`clang -c dpi.c`, not `-shared`). `.so` is demoted to an
import-only convenience for a prebuilt/closed-source library or one with heavy
transitive `.so` deps (which the dynamic linker resolves for free; a `.o`'s
externals must instead resolve through clotho's generators — fine for libc/libm
via `GetForCurrentProcess`). A design with a non-empty `dpi_exports` that was given a
`.so` fails loudly before the library is loaded (`run_jit`, `src/ir/Jit.cpp`): a
sealed shared object cannot be linked into the JITDylib, so its `export` calls could
never reach a thunk.

An imported symbol that resolves to nothing surfaces as an ORC `lookup("clotho_main")`
failure at JIT time; wrap it to a loud, named "unresolved DPI import `<cid>`" error
rather than a raw ORC message. No layer/mangling changes; both loaders feed the
one main JITDylib. (Resolving DPI symbols from clotho's *own* process symbols is
deliberately not supported — it would need `-rdynamic`/`ENABLE_EXPORTS`.)

## export "DPI-C" — landed (V1)

`export "DPI-C" function foo;` lets native C **call into** a SystemVerilog
function. The SV function is ordinary (it has an IR body and lowers normally);
export adds a C-callable entry point named by its c_identifier.

**Landed:** functions, any number of tops (module-scope or `$unit`-scope). Args:
`input`/`output`/`inout` scalar / `chandle` / real / packed-vector, plus `input`
string. Return: scalar / chandle / real / void. `context` imports accepted (they
lower like a plain import — see below). The thunk marshal reuses the import helpers
in reverse: input scalar via `dpi_c_to_plane`, input vec via `dpi_vec_copyout`,
input string builds an `rt_string` via `rt_dpi_str_out`; `output`/`inout` pass the
SV `(mm_ptr, z_ptr)` cell ABI and copy back after the call (`dpi_arg_to_c` /
`dpi_vec_copyin`). Verified against **verilator** (a working export oracle):
`output` (two results), `inout`, `input`/`output` bit-vector, `input` string, and
real — byte-identical (`result=105`, `q=3 r=2`, `bvset=aabbccdd11223344`, …). One
caveat: **verilator is not a 4-state oracle** — for an `output logic[N]` carrying
x/z it drops the unknowns (`bval=0`), so clotho's correct svLogicVecVal encoding
(the same `dpi_vec_copyin` the import path uses) diverges there; that path is
covered by the import marshal's own tests rather than by verilator. Deferred:
string return
(returned-`char*` lifetime), `output`/`inout` string, and one c_identifier exported
from more than one scope. Exported **tasks** were deferred out of V1 and have since
landed for the zero-time case — see "DPI tasks" below.

**Why `context` is accepted, not required.** LRM 35.5.3: C may only call an
exported function from within a `context` import, which establishes the SV scope
(verilator enforces this — a plain import that calls an export aborts "scope wasn't
set"). Clotho bakes the instance into the lowered function at lowering time — its
body GEPs that instance's own `@clotho.inst.<hier>.state` global, a link-time
constant — so the thunk needs nothing at run time to find "which instance." Clotho
therefore *accepts* `context` (lowering it identically to a plain import) but never
*needs* it to route a call. The svScope C API is provided (`svGetScope` /
`svSetScope` / `svGetNameFromScope` / `svGetScopeFromName` / `svPutUserData` /
`svGetUserData`, exposed as JIT absolute symbols in `src/ir/Jit.cpp`), with one scope
registered per top; a handle for a nested instance is the deferred piece.

**How multi-top export routes.** `getDPIExports()` is compilation-wide, but each top
is lowered by its own `Lowerer`, so `collect_dpi_exports` keeps only the entries it
owns: it walks the exported subroutine's scopes outward through
`InstanceBodySymbol::parentInstance` to the root instance and takes the entry iff
that root is the top being lowered. A `$unit`/package export has no instance
multiplicity (LRM 6.8) and is admitted by exactly one top, lowering once through the
shared `globals` Lowerer that `function_id` delegates to. Collecting one c_identifier
twice would make `llvm::Function::Create` silently rename the second thunk
`<name>.1` and bind C to an arbitrary instance, so the collector `fail`s on a
duplicate `extern_name`.

**A design with NO top module** (an interface bound into a package, no `module`
anywhere — a real shape, not a degenerate one: uvm-examples/apb_protocol_monitor
and uvm_alu_config_analysis, both `sv-tests` UVM fixtures pairing `uvm_pkg.sv` with
a top-less interface) used to drop its `$unit`/package exports silently: "exactly
one top admits them" had no top to be that one, so `uvm_pkg.sv`'s
`export "DPI-C" function m__uvm_report_dpi` — the report-forwarding hook
`uvm_dpi.o`'s own C calls back into — never collected, and the JIT failed at
`--dpi-lib` link time with `Symbols not found: [ m__uvm_report_dpi ]`, not a
compile-time diagnostic (`lower_top`'s per-top loop simply never ran, so nothing
was there to refuse). `collect_dpi_exports` now takes a nullable top; the caller
collects globals once against `nullptr` when `root.topInstances` is empty, which
`function_id` needs no special-casing for (a null top can only ever match a
module-scope export, which a topless design has none of by construction, so it
takes the same "wrong top" `continue` an ordinary mismatch already does).

The rest of this section is the original scoping; it matches what landed.

### Part 1 — symbol resolution: needs the relocatable-object loader

Export needs the two-way linking model, so it needs the **`.o` path** described in
the JIT symbol resolution section above — nothing new. The reason a `.so` cannot do
export: `Load` → `dlopen(RTLD_LAZY|RTLD_GLOBAL)` binds the `.so`'s undefined symbols
against the **process** symbol table at load time, never the ORC JITDylib, so the
C's undefined `foo` can never reach a JIT-compiled thunk (which lives in ORC-managed
anonymous mmap). `addObjectFile` instead makes that `foo` an ordinary ORC external,
resolved through the *same* JITDylib lookup that binds `rt_*` and libgcc — so it
binds to the export thunk, and the C's own `import` calls bind to the design, both
in one link image. That is exactly what LRM 35 export linkage assumes (static,
name-based, single image).

So there is **no export-specific loader**: `--dpi-lib` already dispatches a `.o`
through `addObjectFile` (RTDyld supports it today — no JITLink switch). Export only
adds a guard, which `run_jit` applies before loading a `.so`: a design with exports
plus a `.so` fails loudly, since a sealed shared object cannot be linked into the
JITDylib. The whole symbol-resolution cost of export is that guard plus the thunk
emission below.

### Part 2 — the reverse thunk (medium, mostly reuse)

- **Frontend**: walk `comp.getDPIExports()` → `{subroutine, cIdentifier, syntax}`
  (slang gives the export→SV-subroutine binding and the c_identifier directly —
  no syntax-digging as imports needed). Record an export request per entry; the SV
  function lowers as a normal `ir::Function`.
- **Backend thunk** (all new, but small): emit `<c_ret> <cid>(<c_args>)` with
  **external** linkage, signature from `dpi_c_llvm_ty` per formal + return. Body:
  fetch the state pointer via `llmod_.getNamedGlobal("clotho.state")` (the single
  flat design-state global — signal addressing is absolute from the root, so there
  is **no** "which instance" problem for one instance), marshal each C arg → plane
  (**reuse `dpi_c_to_plane` / `dpi_vec_copyout`**), `call @<mod>.fn.<name>(state,
  rng, planes...)` (a plain SV function needs only `(state, rng, args)` — no
  wheel/coroutine). `rng` is `@clotho.rng`, the module-wide xoshiro256++ state
  `main` seeds at startup: C drove this call, so there is no calling process whose
  stream a `$urandom` inside the exported function could continue (LRM 18.13).
  Then extract the `{mm,z}` return, marshal plane → C (**reuse `dpi_arg_to_c` /
  `dpi_vec_copyin`**). The scalar/vec/chandle marshal helpers reverse directly.
- Incremental pieces: `output`/`inout` formals (thunk allocas caller temps, passes
  their `(mm_ptr,z_ptr)`, copies back to the C pointers post-call) and string
  return (SV `rt_string` → C `char*`, reverse of `rt_dpi_str_out`).

### Deferred (mirror the import guards)

- **Exported tasks** — deferred out of V1; **T1 (zero-time) has since landed**, see
  "DPI tasks" below. (The reason recorded here originally — "a task needs the timing
  wheel, a `main`-local alloca, not a global" — was already stale when it was
  written: the wheel is `@clotho.wheel`, a module global, and the export-task thunk
  simply loads it.)
- **One c_identifier exported from several scopes** (LRM 35.5.3) — `svSetScope`
  selecting *which instance* a shared C name reaches. Absolute addressing means every
  distinct c_identifier already binds to its own instance with no scope lookup, so
  what is left is the shared-name case, and slang drops those duplicates before
  `getDPIExports()` is visible. Blocked upstream; guard.
- Open arrays / unpacked aggregates.

### Caveat

`@clotho.state`'s array/memory fills and the `shadow_base` store happen at runtime
in `main` (not in the global's initializer). A C call to an export **before
simulation starts** would see uninitialized arrays. Normal DPI calls exports from
*within* an import during a running sim, so this is typically fine — document it,
and consider a "sim started" guard.

### Sizing

Symbol resolution: **small** (the `.o` loader already exists per the unified
`--dpi-lib` dispatch; export adds only the "gave a `.so`, need a `.o`" guard).
Reverse thunk + scalar/vec/chandle marshal: **medium**, dominated by reuse.
`context`/`svScope` are the genuinely deferred pieces here; exported tasks were
scoped separately below and were not the large item that line once claimed — their
zero-time case landed as an extra arm in the same thunk emitter. A
functions-only, scalar+vec+chandle+string-return export V1 is medium and mostly
mechanical — the scary part (resolution) is solved.

## DPI tasks — import and export (T1 landed; T2 and T3 scoped)

**Phase T1 has landed**: both guards are gone, and an import task whose C calls a
zero-time exported task works end to end, byte-identical to verilator (measured
below). What replaced them is a pair of *runtime* refusals, because the two
remaining gaps are dynamic properties of a call chain, not static properties of a
declaration: an exported task that suspends
(`rt_dpi_export_task_suspended`, T2) and an exported task entered with no
imported-task frame live (`rt_dpi_export_task_no_task_frame`, LRM 35.8). One
static refusal is new and permanent: `pure` on an imported task
(`build_dpi_import`, LRM 35.5.1.3). The rest of this section is the original
scoping; **What T1 actually shipped** below records where the implementation
diverged from it.

### The two directions are one feature

LRM 35.8 makes an exported task unreachable without an imported task:

> It is never legal to call an exported task from within an imported function. This
> semantics is identical to native SystemVerilog semantics, in which it is illegal
> for a function to perform a task enable. It is legal for an imported task to call
> an exported task only if the imported task is declared with the `context`
> property.

C code only ever runs inside an import call, so lifting the export guard alone
would produce an entry point with no legal caller. The two guards lift together,
in one phase plan.

### What the LRM adds beyond the function case

**1. A synthetic `int` return in C, both directions (35.8).** "The return value of an
exported task is an `int` value that indicates if a disable is active or not on the
current execution thread. Similarly, imported tasks return an `int` value that is
used to indicate that the imported task has acknowledged a disable." An SV task has
no return type; this `int` is the disable protocol's channel and is never visible to
SV — "Calls to import tasks in SystemVerilog code are indistinguishable from calls
to native SystemVerilog tasks."

**2. The disable protocol (35.9).** Item a) — "When an exported task returns due to a
disable, it shall return a value of 1. Otherwise, it shall return 0." — is the
simulator's obligation. Items b) (import task returns 1 on disable), c) (import
function calls `svAckDisabledState()` before returning on disable) and d) (no
further export calls once disabled) are the C's, and the clause obliges the
simulator to *check* them: "simulators shall implement checks to verify that item
b), item c), and item d) are correctly followed by imported tasks and functions. If
any protocol item is not correctly followed, a fatal simulation error is issued."
`svIsDisabledState()` and `svAckDisabledState()` are part of the API and clotho
provides neither today (a C call to one hits an unresolved symbol).

**3. Reentrancy (35.5.1.5) — the load-bearing clause.**

> A call to an imported task can result in the suspension of the currently executing
> thread. This occurs when an imported task calls an exported task, and the exported
> task executes a delay control, event control, or `wait` statement. Thus it is
> possible for an imported task's C code to be simultaneously active in multiple
> execution threads.

That last sentence is the whole difficulty and it decides the mechanism (below).

**4. `pure` is illegal on an imported task** (35.5.1.3: "An imported task can never
be declared `pure`"). slang only *warns* (`DPIPureTask`), so clotho refuses.

**5. Nothing requires an exported task to consume time.** 35.5.1.1's NOTE says
imported tasks *can* consume time; the exported-task clause never says they must.
A task-shaped DPI hook that completes instantly is legal, common (it is a task
because the C API wanted the `int` channel, or because the SV body writes an
`output`), and is a complete, useful feature on its own — which is what makes the
phasing below honest rather than a stub.

### What already exists (most of this is plumbing)

- **A time-consuming SV task is already a standalone driveable coroutine**, not a
  body inlined into its caller. `declare_task` (`ir/llvm/Subroutines.cpp`) emits
  `(state, rng, wheel, [proc], [home], args…, record)`, returns a coroutine handle,
  and calls `setPresplitCoroutine()`. An export thunk therefore has something
  callable to point at; nothing about the task ABI has to change.
- **The call protocol the thunk needs is already written.**
  `lower_task_call_inst` (`:26077`) ramps the callee, `coro_resume`s it, tests
  `coro_done`, and takes one of two arms: *sync* (`:26256`) destroy the frame and
  continue inline; *async* (`:26264`) store the caller's parked handle into the
  callee's `record.parent` and suspend the caller. An export-task thunk is that
  same split with a different answer to "who is the caller".
- **The wheel is already a module global**, and so is every other piece of
  scheduler state. `build_wheel_global` (`:14825`) emits `@clotho.wheel`; `main`
  only `rt_wheel_init`s it (`:8091-8092`); the comment at `:14823` gives the reason
  ("a function's ABI carries state and rng but no wheel, and every event op needs
  the current tick"). The active / inactive / reactive / re-inactive rings and their
  head/tail indices are internal-linkage globals (`ensure_active_queue` and
  siblings, `:11991-12186`), and `@clotho.state` is a global (`:4441`). **Nothing
  needs promoting** — the "main-local alloca" premise of the original deferral was
  never true.
- **C driving JIT code that resumes a coroutine already happens.**
  `rt_sim_run(state, wheel, drain, finals)` (`runtime/Scheduler.cpp`) is genuine C
  calling JIT function pointers, and `sim_finals` resumes `final`-block coroutines.
  It is a precedent for the *shape* only: a `final` block cannot suspend (LRM
  9.2.3), so that edge never advances time.
- **The GC never scans a machine stack.** Roots are the static root tables, the
  intrusive registry of heap CoroSplit frames, and explicitly registered ranges
  (`push_frame_roots` / `push_range_roots`, `Gc.cpp:1809-1826`; `scan_conservative`
  at `:1781` only ever walks those). A C frame holds no GC roots wherever its stack
  lives, so the suspension mechanism below is GC-neutral by construction and needs
  no stack registration.
- **`src/` contains no `thread_local`** (zero grep hits), which the mechanism choice
  below leans on.

### Three genuinely new pieces

**(1) The synthetic `int`.** Both directions grow a C return no SV type produces.
`DpiCType` needs no new member — it is an `int` — but the descriptor must record
"return present in C, absent in SV" so `lower_dpi_call` does not try to bind it to a
result `Plane`, and so the export thunk's return is generated rather than marshalled.

**(2) Caller identity.** The export *function* thunk passes `null` for `proc` and `0`
for `home` (`:25584-25592`), with the comment that C drove the call so there is no
calling process, no thread whose region to inherit, and `process::self()` reports a
located fatal. For a *task* that is false: the caller is the process that entered
the import task, and `process::self()`, the region a `#0` inside the exported task
lands in, and any `disable` targeting the chain all belong to it. Fix: two globals
(`@clotho.dpi.caller_proc`, `@clotho.dpi.caller_home`) written at the import-*task*
call site and read by the export thunk. They save/restore around the call, because
import tasks nest (an exported task may itself call another import task). This also
buys the 35.8 legality check for free: `caller_proc == null` inside an export-*task*
thunk means the live C frame belongs to an imported *function* (or to no DPI frame
at all), which is exactly the case the clause calls never legal → located fatal
naming both subroutines.

*Superseded in the implementation*: it landed as **four** globals, and the 35.8
check does not read `caller_proc` — see "What T1 actually shipped" below.

**(3) Suspension across the C frame.** The rest of this section.

### Suspension: the mechanism, and why the cheap one is wrong

**Rejected — a nested drain from the thunk** (running the region loop recursively
until the exported task's coroutine completes). It is the tempting shape, because
the drain reads only globals and would re-enter cleanly. It is wrong for two
independent reasons.

- *Completion order is not stack order.* Process A enters an import task whose C
  calls an exported task that waits `#20`; the nested drain runs and, inside it,
  process B enters the same (or another) import task whose C calls an exported task
  that waits `#5`. B's drain frame now sits **above** A's. At `t+5` B's task
  completes and B's nested drain returns — fine. At `t+20` A's task completes and
  A's C frame must resume, but A's drain frame is below B's on the machine stack and
  cannot return until B's does; if B's C is still parked, it never will. Stack
  nesting forces LIFO completion, and LRM 35.5.1.5 explicitly blesses the
  non-LIFO case ("simultaneously active in multiple execution threads").
- *`$finish` assumes an all-JIT stack.* The cooperative finish sets the
  `rt_finishing` global (`runtime/Scheduler.cpp`) and every JIT frame in the
  statically-computed may-finish closure tests it after a call
  (`emit_finish_check`, `ir/llvm/SystemTasks.cpp`), returning through its own
  epilogue. A nested drain multiplies the number of C frames between the `$finish`
  site and the real drain, and none of them tests the flag.

**Accepted — one machine stack per in-flight import-task frame.** The C frame becomes
suspendable because it owns a stack that can be parked. Topology, with the drain on
the simulation stack:

1. Process P (a JIT coroutine, resumed by the drain) reaches its import-task call
   site and calls `rt_dpi_task_enter(thunk, argpack)`. That takes a worker from a
   pool, hands it the baton, and blocks the simulation stack.
2. The worker runs the C. The C calls exported task `t`; `t`'s thunk — now running
   on the worker — ramps `t`'s coroutine with `(state, rng, wheel, caller_proc,
   caller_home, args…, record)` and `coro_resume`s it, exactly as
   `lower_task_call_inst` does.
3. `coro_done` → the thunk destroys the frame, marshals `output`/`inout` args back,
   and returns 0 (or 1 per 35.9(a)). The C continues on the worker. This is the
   whole of phase T1.
4. Not done → the thunk records a **DPI resume ticket** in `t`'s `record.parent` in
   place of a coroutine handle, hands the baton back, and the worker blocks.
   `rt_dpi_task_enter` returns `SUSPENDED` on the simulation stack, and P takes
   `lower_task_call_inst`'s async arm verbatim: park P's own handle, suspend, let
   the drain carry on. No nesting, no LIFO constraint.
5. `t`'s coroutine eventually completes and, per the existing convention, resumes
   `record.parent`. That epilogue now branches on the parent's tag: a coroutine
   handle → `coro_resume` as today; a DPI ticket → `rt_dpi_task_resume(idx)`, which
   hands the baton to the worker and blocks the simulation stack until the worker
   either parks again in another exported task or returns from the import — at
   which point `rt_dpi_task_leave` pushes P's handle onto the ready ring, releases
   the worker, and returns. The drain continues from an ordinary JIT frame.

`record.parent` already carries a tag: `parked_handle` packs the handle with its
home region, so a "this parent is DPI worker *i*" encoding is a spare tag value,
not a new field.

**Which stack primitive.** Two candidates:

- **Pooled OS threads with a strict baton** (one mutex + one condvar per worker;
  exactly one of {simulation stack, one worker} runnable at any instant). Portable,
  no per-architecture assembly, clean under sanitizers and the DAP debugger, and
  safe against the runtime's globals precisely because the baton serialises — the
  simulator stays single-threaded in the only sense that matters. No TLS hazard:
  `src/` has no `thread_local`. GC-neutral: the collector never scans a machine
  stack. Cost: one pooled thread creation on first use (~30 µs) and two futex
  handoffs per switch (~2–5 µs).
- **A hand-rolled context switch** (x86-64 / aarch64 `swapcontext`-shaped assembly,
  ~20 ns per switch; `ucontext`'s own `swapcontext` is disqualified by its
  `sigprocmask` syscall). Faster, but it is new per-architecture assembly in a
  project that has none, plus unwinder / sanitizer / debugger exposure.

**Choose the baton pool.** A switch happens only when simulation time advances
*inside* a DPI call, which by construction cannot be a hot loop: each switch is
paired with an event-wheel round trip and a full region pass, both orders of
magnitude more expensive than the handoff. The assembly path buys nothing
measurable here and costs a permanent portability surface, so it is recorded as a
follow-up gated on a measurement, not taken up front. This is an implementation
choice, not a semantic one — LRM compliance is identical either way.

**`$finish` reached inside an exported task.** The thunk tests `rt_finishing` after
the resume and returns to C immediately; the C between that return and the import's
own return still runs, and the SV import call site's `emit_finish_check` then
unwinds normally. That residual C execution is unavoidable for any simulator that
cannot unwind a foreign frame, and is worth one sentence in the manual rather than a
mechanism. Teardown gets one new obligation: a worker still parked at
`sim_teardown` means a C frame's state is unrecoverable, so report it loudly
(naming the import) instead of leaking a blocked thread.

### Phases

- **T1 — tasks, zero-time. LANDED.** Both guards lifted; `pure` on an import task
  refused; the synthetic `int` generated on both sides and ignored; caller-identity
  globals landed; the export thunk drives the task coroutine and takes only the sync
  arm. An exported task that *does* suspend is a **located fatal** naming the task —
  loud, never silently wrong, per PRINCIPLES.md. This is the phase that covers
  task-shaped hooks completing instantly. See "What T1 actually shipped".
- **T2 — real suspension.** The baton pool, the DPI ticket in `record.parent`, the
  async arm at the import-task call site, `rt_dpi_task_enter/resume/leave`, and
  teardown's parked-worker check. Removes T1's fatal, and converts the import-task
  call site from a plain `Call` into a real task call site.
- **T3 — the disable protocol (35.9).** Export returns 1 on a disable-terminated
  task; the import task's `int` return is read instead of discarded;
  `svIsDisabledState` / `svAckDisabledState`; the simulator-side checks the
  clause mandates for items b), c) and d), each a fatal. Rides the existing disable
  machinery (`design_uses_disable_`, `emit_disable_poll`, `ir/llvm/Processes.cpp`,
  `docs/design/disable.md`), which already polls at every suspending tag.
  `svGetCallerInfo` belongs here too.

### What T1 actually shipped

Three places diverge from the scoping above; each was decided against the clause
text and is commented at the site that implements it.

**The import-task call site stayed a plain `Call`.** The plan said it would lower at
a *task* call site "so the site is suspendable, is inside the `compute_may_finish`
closure, and a call from a function body is rejected by the existing
task-in-function rule". Only the middle clause turned out to be load-bearing:

- *Suspendable* is exactly what T1 does not need — nothing on this path can consume
  time, since an exported task that suspends is refused. A task ramp, record slot
  and suspend arm would all be dead weight the optimizer cannot remove (the callee
  is an opaque external).
- *Task-in-function* is inherited for free either way: slang rejects the enable
  itself ("cannot invoke a task from within a function"), before lowering, measured.
- *may-finish* was real, and needed its own fix rather than the call-site change.
  `compute_may_finish` seeds from a subroutine's IR, and a DPI import has none, so
  an import had **no callees at all** — yet its C can call back into any export
  whose SV body reaches `$finish`. `lower_call_inst`'s DPI branch also returned
  before `emit_finish_check`. Both fixed: the closure gains an edge from every DPI
  import to every `DpiExport`, and the DPI call site emits the check. Exact whenever
  no export finishes; conservative otherwise. This was a latent hole on the export-
  *function* path too — an import function calling an exported function that
  `$finish`es had the same reachability — so the fix is not task-specific.

**Caller identity is four globals, not two.** `clotho.dpi.caller_proc` (ptr) and
`clotho.dpi.caller_home` (i8) are gated exactly as `declare_task` gates those
parameters. Two more were needed:

- `clotho.dpi.caller_rng` (ptr). The export-*function* thunk passes the module-wide
  `@clotho.rng` with the rationale that "C drove this call, so there is no calling
  process whose stream a `$urandom` could continue (LRM 18.13)". For a *task* that
  rationale inverts: there **is** a calling thread, the task runs on it, and a
  `$urandom` inside it must continue that thread's sequence. Passing the module-wide
  stream would have been a silent wrong answer, not a loud gap.
- `clotho.dpi.in_import_task` (i8), and it is the one emitted **unconditionally**
  once the design has DPI tasks. The scoping proposed deriving 35.8's legality check
  from `caller_proc == null`, but `caller_proc` only exists when
  `design_uses_process_` — which is false for most designs — and 35.8's rule does
  not depend on whether the design happens to mention `process`. A dedicated flag
  makes the check independent of an ABI-gating decision unrelated to legality.

All four save/restore around the import-task call, because import tasks nest. That
is verified: import task → exported task → *another* import task → another exported
task returns correctly, and after a legal import-task call returns, the flag is back
to 0 so a subsequent call from an import *function* still fatals.

**`record_slot()` is not reusable here.** It anchors its alloca after the current
coroutine's `coro_begin_`; a C thunk is not a coroutine, so the thunk uses a plain
`entry_alloca` of the same `{ ptr parent }` shape, stored null before the ramp.

### Refusal surface after T1 (each loud, each with a TODO)

An exported task that suspends (T2, `rt_dpi_export_task_suspended`); an exported
task with no imported-task frame live (LRM 35.8,
`rt_dpi_export_task_no_task_frame`); the disable protocol and
`svIsDisabledState`/`svAckDisabledState` (T3); `pure` on an imported task (LRM
35.5.1.3, permanent); everything the export *function* path already refuses
(string return, `output`/`inout` string, open-array formals, one c_identifier from
several scopes) — a task inherits every one of those, since 35.7 says exported tasks
share the function argument rules, and the inheritance is tested rather than
assumed; and a `.so` given to a design that exports, which the existing `run_jit`
guard already covers.

One of those inherited refusals is enforced upstream rather than by clotho, and T1
re-measured it for tasks: **one c_identifier exported from two scopes** is collapsed
by slang before `getDPIExports()` is visible, so exactly one thunk is emitted and
clotho's own duplicate-`extern_name` `fail` in `collect_dpi_exports` is unreachable.
It stays as the invariant's statement, and a test pins the upstream behaviour so a
slang change surfaces rather than silently binding C to an arbitrary instance.

### Oracle — measured, not assumed

- **iverilog is not an oracle for any of this**: iverilog 13 still cannot parse
  `import "DPI-C"` / `export "DPI-C"` at all ("Invalid module item", re-measured).
- **Verilator 5.050 is an oracle for T1 only**, and T1 is now checked against it
  **byte-identically** (measured, below). Two divergences, both re-measured:
  1. It declares an exported task `extern void sv_note(int v, int* doubled)` —
     *void*, not `int` — so it does not implement 35.8's return value and cannot
     oracle T3. The two runs therefore share one `.sv` byte for byte, and the `.c`
     differs only in that one prototype (clotho's reads the `int`, verilator's
     cannot). Clotho returns 0 there, which is 35.8's "no disable is active".
  2. It **refuses a timing control inside an exported task outright** —
     `%Error-UNSUPPORTED: Timing controls inside DPI-exported tasks` (re-measured
     on the same `susp.sv` clotho fatals on) — so it cannot oracle T2 at all.
     Notably both tools refuse the same program; clotho refuses it at run time with
     a located fatal, verilator at compile time.
  Verilator's `--binary` flow also needs the shim wrapped in `extern "C"`: it
  compiles a `.c` with `g++`, so a plain C file fails to link (measured:
  `undefined reference to c_driver`).
- T2 and T3 are therefore derived from the clause text, the way the SVA layer's
  `diff_not.sv` expectation and the LRM 19.11 coverage percentages were.

#### The measured T1 cross-check

A `context` import task (`c_driver`) whose C calls a zero-time exported task
(`sv_note`, with an `output` formal) three times, plus an exported *function*
(`get_base`) reading a design signal, from an `initial` that has already advanced
to `t=5`. `clotho run --dpi-lib rt.o rt.sv` and `verilator --binary --timing`
produce the same eight lines, `diff`-clean:

```
[c]   c_driver n=3 base=100
[sv]  sv_note v=1 doubled=2 base=100 t=5
[c]   sv_note(1) -> doubled=2 disabled=0
[sv]  sv_note v=2 doubled=4 base=100 t=5
[c]   sv_note(2) -> doubled=4 disabled=0
[sv]  sv_note v=3 doubled=6 base=100 t=5
[c]   sv_note(3) -> doubled=6 disabled=0
[sv]  back from c_driver at t=5
```

This covers, in one run: the C→SV task round trip through a JIT-linked `.o`, an
`output` formal marshalled back across the C boundary, a design-signal read from
inside an exported task, the caller's time observed inside it (`t=5`, not 0),
interleaving of the C's own stdout with the SV body's `$display`, and an exported
function and task coexisting.

The three clotho-only cases, none of which verilator can run:

```
# an exported task that suspends (T2)
FATAL: exported task 'sv_wait' suspended on a timing control, which needs the
suspendable C frame of DPI-task phase T2 (not implemented) at 1 s

# an exported task called from an imported FUNCTION (LRM 35.8)
FATAL: exported task 'sv_note2' was called from C with no imported DPI task frame
live (LRM 35.8: it is never legal to call an exported task from within an imported
function) at 0 s
```

```
# nesting: import task -> exported task -> another import task -> another exported task
[c]   outer_c -> sv_outer
[sv]  sv_outer v=5 t=2
[c]   inner_c -> sv_deep
[sv]  sv_deep v=50
[c]   inner_c done
[sv]  sv_outer after inner_c
[c]   outer_c got r=6
[sv]  done t=2
```

`$finish` inside an exported task also unwinds correctly now (it did not before the
`compute_may_finish` fix above): the SV statement after the import call does not
run. The C between the exported task's return and the import's own return still
does — unavoidable for any simulator that cannot unwind a foreign frame, as the
`$finish` discussion above notes.

### Test plan

- **T1 automated (`tests/dpi_test.cpp`, landed).** The two guard subcases inverted:
  the import one became `pure`-refusal plus the inherited task-in-function
  rejection, the export one became three inherited-refusal subcases
  (`output` string, `inout` string, unpacked aggregate). Added: an import task
  resolving against libc `puts` that lowers, runs and prints; the emitted
  `declare i32 @t(i32)` C prototype; an exported task that lowers and runs
  SV-side; the export-task thunk's shape (i32 return, task coroutine callee, the
  `done`/`suspended`/`no_frame` arms, both fatal symbols, `ret i32 0`); the upstream
  slang collapse of one c_identifier from two scopes; and a regression check that an
  exported *function*'s thunk gained none of the task machinery. The C-calls-into-SV
  round trip needs a `.o` through `addObjectFile`, which the doctest harness does
  not wire, so it stays the CLI cross-check above.
- **T1 fatals** are CLI-checked (both transcripts above); the thunk test pins that
  each fatal is reachable from the emitted IR.
- T2: the non-LIFO case that kills the nested drain — two processes, two import-task
  frames, exported tasks waiting `#20` and `#5`, both must return in delay order and
  the SV side must observe the two import calls returning in that same order. Derived
  from 35.5.1.5; neither iverilog nor Verilator can oracle it.
- T2: `$time` inside an exported task equals the caller's time on entry and the
  advanced time after the wait; `process::self()` inside one is the calling process.

### Sizing

T1 was **small-to-medium** and almost entirely reuse, as estimated: two guards
lifted, the export thunk gained `lower_task_call_inst`'s sync arm, and the
wheel/state/ring globals it needs already existed. The estimate was wrong on one
clause — the import-task call site is *not* a task call site (see "What T1 actually
shipped") — and it missed two things that were not task-specific at all: the
`compute_may_finish` edge from a DPI import to every export, and the missing
`emit_finish_check` on the DPI call path. T2 is **medium** and self-contained: one
runtime file for the baton pool, one tag value in `record.parent`, one branch in the
task epilogue, plus converting the import call site. T3 is **medium** and is mostly
bookkeeping over the existing disable machinery.

## svdpi.h runtime (Phase A landed: packed-vector accessors)

`svdpi.h` (LRM Annex H) is the standard C header/API real DPI code is written
against — the canonical types (`svBit`/`svLogic`/`svBitVecVal`/`svLogicVecVal`,
`svScope`, `svOpenArrayHandle`) plus runtime functions the *simulator* provides.
Clotho already produces the exact **layout**; what was missing is the **functions**
DPI C calls. DPI C that pokes the raw layout worked; DPI C that `#include`s
`svdpi.h` and calls `svGetBits`/`svGetScope`/… did not link. This is the gap to
running real third-party DPI code.

**Landed — the packed-vector bit/part-select accessors** (`src/runtime/Svdpi.cpp`,
header `src/runtime/svdpi.h`): `svGetBitselBit`/`svGetBitselLogic`,
`svPutBitselBit`/`svPutBitselLogic`, `svGetPartselBit`/`svGetPartselLogic`,
`svPutPartselBit`/`svPutPartselLogic`, `svDpiVersion`. They are **pure C over the
canonical layout** (no compiler/IR coupling) — a bit-select is one shift+mask; a
part-select loops the bit-selects — so this is a standalone library. Exposed to
the JIT as absolute symbols in `Jit.cpp` (like `rt_*`); a DPI `.o` resolves them
against the JITDylib. Ship `src/runtime/svdpi.h` for users to `-I` (any standard
`svdpi.h` is equally usable — the signatures are standard). Verified vs verilator
(shares its own svdpi) on the non-x/z paths (`svGetBitselLogic`, `svPutBitselBit`,
`svGetPartselBit`); the x/z encoding is unit-tested in `runtime_test.cpp` and
matches the logic-vec marshal (verilator can't parse a `z` literal).

**Landed — scope (one per top).** `svGetScope`, `svSetScope`, `svGetNameFromScope`,
`svGetScopeFromName`, `svPutUserData`/`svGetUserData`. Each top-level instance gets a
runtime scope object holding its hierarchical name and a per-key C user-data map;
`emit_main` emits one `rt_svdpi_scope_register(name, len, index)` call per
`Module::tops()` entry when the design uses DPI, and `index == 0` clears the table so
a second simulation in one process starts with no stale user data. The table is a
`std::deque` because a `svScope` handle *is* the object's address, so registering a
further scope must not move the handles already handed out. `svGetScope` returns the
active scope, defaulting to the first top; `svSetScope` swaps it (a context import may
save/restore it around an export call); `svGetScopeFromName` linear-searches the
table. Verified vs verilator (user-data counter, save/restore, non-empty name all
match; the name is each tool's own top — clotho `top`, verilator `vtop`); unit-tested
in `runtime_test.cpp`. A handle for a nested (non-top) instance is still deferred.

**Landed — open arrays (1-D, all directions, LRM 35.5.6.2).** An unsized `[]` formal
arrives as an `svOpenArrayHandle` (an `rt_svopen` descriptor: `data`, `left`,
`right`, `nelem`, `elem_bytes`, `elem_width`, `is_logic`) the JIT builds per call
from the actual's array type (`build_dpi_open_array_desc` in `ir/llvm/Dpi.cpp`).
Accessors landed: `svDimensions`/`svSize`/`svLeft`/`svRight`/`svLow`/`svHigh`/
`svIncrement`/`svSizeOfArray`, `svGetArrayPtr`/`svGetArrElemPtr1`, and the per-
element `svGet/PutBitArrElem1VecVal`/`svGet/PutLogicArrElem1VecVal`.

Two data paths:

- **Zero-copy** for a named-scalar (`byte`/`shortint`/`int`/`longint`) element:
  Clotho's narrow `[N × iW]` element storage *is* a contiguous C `T[N]`, so the
  descriptor points straight at the design array — no marshal, no copy. The C reads
  and (output/inout) writes it in place.
- **Marshal in inline LLVM IR** for a packed `bit`/`logic` element, at any width
  (`emit_dpi_oa_pack_ir` / `emit_dpi_oa_unpack_ir` in `ir/llvm/Dpi.cpp`). Because the
  actual is a fixed unpacked array, `N` is a compile-time constant, so this is a
  constant-trip loop that loads the native `iW` / `i2W` slot fields directly (no
  memcpy, no stride math) and lets LLVM legalize every op: `must`/`may` are
  `trunc i2W` / `lshr i2W, W` + `trunc`, the reconstruction a single `shl i2W, W` —
  no unaligned funnel shift to hand-roll at *any* width, and the z field is a plain
  struct-field access gated on the slot's real `has_z` (a may-z-elided `{ i2W }` slot
  simply has no field-1 access emitted). This unifies the marshal into one mechanism
  at all widths — see "Why one IR marshal, not a C fast path" below.

**Directions.** Input marshals/points-in before the call. Output/inout: a marshalled
element unpacks the C-written scratch back into the design storage after the call;
a zero-copy element has the C write the design array directly. Documented gap: a
zero-copy output write lands in storage with no copy-back, so it does not trigger
wait/NBA scheduling on the array.

**Actuals.** A fixed unpacked array (above) or a **dynamic array** (LRM 7.5). A
dynamic-array actual (`build_dpi_dynarray_desc`) reads the runtime `{ data, len }`
handle: `nelem`/`right` come from `len`, and a named-scalar element is zero-copy —
the heap buffer is already a contiguous C `T[len]`, read and (output/inout) written
in place; the view is fixed-size so it never resizes. An empty / unallocated array
(`len == 0`) yields `nelem == 0` and an empty `svLow..svHigh` span (right == -1), so
a C loop reads nothing rather than dereferencing the null buffer. A dynamic array
with a *packed* element (needs a
runtime-sized marshal scratch, not the compile-time-`N` inline buffer) is a loud
follow-up. A **queue** actual is not reachable: slang rejects `int q[$]` against an
`int a[]` open-array formal as a type mismatch, so queue → open-array is blocked
upstream, not merely deferred here.

**Why one IR marshal, not a C fast path.** An earlier version kept a
width-specialized C runtime marshal (`rt_dpi_pack_arr_*`, autovectorized at the
runtime's pinned `-O2`) for `≤ 64`-bit elements and used IR only for wider ones. It
was dropped: the C path's only edge is autovectorization at the default `-O0` JIT
tier, and that edge is thin — DPI marshalling is an FFI boundary, not a per-cycle
inner loop, and the access is a *strided* gather (element slots) → contiguous store,
which SIMD handles poorly anyway; under the `-O` tier the inline IR is optimized
*and* inlined (no opaque call), so C is strictly worse there. Against that: two
implementations of the same bit-exact transform are a standing correctness hazard.
Collapsing to the IR marshal at all widths removes the second implementation, the C
runtime functions, and their unit tests, for a marshal cost that only matters in a
workload DPI rarely produces.

Verified vs verilator (byte-identical) and end-to-end in `tests/dpi_test.cpp`:
zero-copy `int` (in + output `i*i` fill), `logic[7:0]` x/z over *descending* bounds,
`bit[15:0]`, inout `logic[7:0]` increment, `bit[63:0]`, `bit[127:0]`, and inout
`logic[99:0]` (a non-32-aligned wide width, exercising the unaligned `may`
extract/insert). The dynamic-array actual is covered at the CLI (runtime size / sum
/ output fill) — verilator rejects a dynamic-array open-array actual outright, so it
is not an oracle here, and the empty-array case is clotho-only.

**Deferred:** multi-dimensional open arrays (recursive descriptor + `svGetArrElemPtr2/3`
+ dim-indexed accessors), a *packed-element* dynamic-array actual (runtime-sized
marshal scratch), open-array *export* args (needs open-array formals in the general
SV function ABI), the disable API, and `svGetCallerInfo`. A queue open-array actual
is blocked in slang (type mismatch), not deferrable here.

## Restrictions — loud-fail with a TODO (per PRINCIPLES.md)

Supported: **import** — scalar types, packed vectors, `chandle`, `string` (input
arg + return), `input`/`output`/`inout`, `pure`, `context`, **functions and tasks**.
**export** — **functions and tasks**,
`input`/`output`/`inout` scalar/chandle/real/vec args, `input` string args,
scalar/chandle/real/void return, any number of tops. Still loud-fail (guards in
`build_dpi_import` / `collect_dpi_exports`, or the two runtime fatals), each with a
TODO:

- Import `output`/`inout` **string** (the unbounded copy-out buffer hazard — see
  the string section) and any non-open-array unpacked aggregate arg/return. **1-D
  open arrays are supported in all directions** (named-scalar zero-copy / packed-vec
  marshal at any width, above); still loud-fail: multi-dim, dynamic-array / queue
  actuals.
- Export string return (returned-`char*` lifetime), export `output`/`inout` string,
  and one c_identifier exported from more than one scope (`svScope`, LRM 35.5.3, would
  pick the instance; blocked upstream — slang drops the duplicate cId entries).
  An exported **task** inherits every one of these (LRM 35.8 puts a task under
  35.7's argument rules), and the inheritance is tested, not assumed.
- The svScope C API is provided with one scope per top; a handle naming a nested
  instance is not available.
- `pure` on an imported **task** (LRM 35.5.1.3, "An imported task can never be
  declared `pure`") — a permanent refusal, not a phase. slang only warns.
- An exported task that **suspends** (executes a delay, event control or `wait`):
  a located runtime fatal, `rt_dpi_export_task_suspended`. Parking the native C
  frame is phase **T2** — see "DPI tasks" above.
- An exported task entered with **no imported-task frame live** — from inside an
  imported function, or from C no DPI import reached: a located runtime fatal,
  `rt_dpi_export_task_no_task_frame`. LRM 35.8 makes the first case never legal;
  the second has no calling thread to borrow. This is a permanent refusal too.
- The LRM 35.9 **disable protocol**: an exported task always returns 0, an imported
  task's `int` return is discarded, and `svIsDisabledState` / `svAckDisabledState`
  are not provided. Phase **T3**, together with `svGetCallerInfo`.

## Oracle

**iverilog 13 does not parse `import "DPI-C"` at all** ("Invalid module item"),
so it is *not* an oracle for DPI — the earlier assumption was wrong. Verilator
implements DPI-C and is the oracle for export (below), but its `--binary` flow did
not link a plain `.c` shim cleanly here, so the import matrix is checked against
clotho's own expected output at the CLI rather than against iverilog or Verilator.
Cover `real`, `byte` signedness, and the `logic` x/z encoding
(`svLogic` 0/1/2=Z/3=X) specifically — those are the non-identity coercions.

Checked at the CLI (clotho `--dpi-lib`), **byte-identical to the expected
`$display` output** on: `int add`, `real` (`rmul`), `byte` (`byte_neg` → -5),
`shortint`, `longint` (negative, 64-bit), `shortreal`, scalar `bit`, scalar
`logic` carrying X (`isx→1`), Z (`code→2`), and 0/1, and a `pure` import.

## Verification

- Automated (`tests/dpi_test.cpp`, in the doctest suite): `int`/`longint`/`real`
  round-trips via **libc/libm** imports (`abs`/`llabs`/`sqrt`/`pow`/`frexp`), a
  `chandle` round-trip via `malloc`/`free` (returns/takes `void*`; `null`/`==`/`!=`
  exercised), and an **input string** via `strlen` (zero-copy `const char*`, empty
  and non-empty) — all resolve through run_jit's existing process-symbol generator,
  so the tests need no `.so` build. `pure` import runs correctly. Guard rejections
  (`output` string, `pure` task, the inherited export-task refusals) confirmed to
  loud-fail via `lower_text`.
  Full suite byte-identical — DPI is additive, no default path changed.
- An **import task** is automated the same way: `puts` is the one libc symbol whose
  shape matches a task import exactly — it takes the `input string` the marshal
  already handles zero-copy and returns the `int` LRM 35.8 wants and SV never sees —
  so the whole T1 import path (i32 C prototype, discarded return, plain call site)
  runs in-suite with no `--dpi-lib`.
- Return string (C returns `char*`), plus the byte/logic/vec/chandle matrix and
  the `--dpi-lib .so` path, are covered by the CLI matrix, since libc has no
  deterministic `char*`-returning function to probe with.
- **Export** is the CLI-vs-**verilator** cross-check (`result=105` on both): a
  `context` import's C calls an exported SV function through one JIT-linked `.o`,
  covering the round-trip, a design-signal read, real, and void. The **exported
  task** cross-check is the eight-line transcript under "DPI tasks" above, also
  byte-identical to verilator. Automated tests cover the export guards, that an
  export directive lowers + runs, and the export-task thunk's emitted shape; the
  C-calls-into-SV round-trip needs a `.o` (addObjectFile), which the doctest harness
  does not wire.
- The byte/shortint/bit/logic + svLogic x/z paths and the `--dpi-lib .so` path
  are covered by the CLI matrix above (not automated, since libc has no clean
  byte/logic-returning functions and building a test `.so` in-suite is fragile).

## Performance notes

- Scalar DPI call = direct `call @cid` with native types; the only instructions
  are the C-width coercions (mostly `trunc`/`zext`, none for `int`/`longint`).
- `pure` → `readnone`+`willreturn` lets the optimizer treat the import like a
  math builtin: hoist out of loops, CSE repeated calls, DCE unused results.
  **Verified**: `square_pure(6) + square_pure(6)` under `-O` emits **one**
  `call @square_pure` (CSE); the same import without `pure` emits two.
- Symbol address is bound by the ORC generator at JIT-link time, so the call is a
  direct relocated call (inlinable target if LTO'd into the .so's own inlining —
  not us), not a registry/indirect dispatch.
