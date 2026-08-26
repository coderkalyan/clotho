# DPI-C — direct C function imports

**Status:** implemented. **Import**: `import "DPI-C" [pure] [context] function` over
scalar, packed-vector, `chandle`, and `string` types, `input`/`output`/`inout` args,
scalar/chandle/string return. **Export**: `export "DPI-C" function` (C calls
into SV) for functions — `input`/`output`/`inout` scalar/chandle/real/vec args,
`input` string, scalar/chandle/real/void return, any number of tops. `--dpi-lib`
accepts a `.o`/`.a` (JIT-linked, needed for export) or a `.so` (dlopen, import only —
refused outright when the design exports).
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
dynamic open arrays, open-array export args, DPI import/export tasks, export string
return + `output`/`inout` string, one c_identifier exported from several scopes
(blocked upstream in slang), `svScope` handles for non-top instances, and
`svGetCallerInfo` / the disable API.

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
(`Lower.cpp:8043`) → `build_call_args` (`:8117`, per-direction marshal) → `b.call`
→ `emit_call_copyouts` (`:8104`, output/inout copy-back). The runtime-call
primitive is `rt_fn` (`LlvmBackend.cpp:5234`, `getOrInsertFunction` + `CreateCall`).
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
is LRM-correct" wins here — slang is the guard.

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
in `runtime/Runtime.cpp`). Because `rt_string_data()` is now a valid C string:

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

An imported symbol that resolves to nothing surfaces as an ORC `lookup("main")`
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
(returned-`char*` lifetime), `output`/`inout` string, exported tasks, and one
c_identifier exported from more than one scope.

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

- **Exported tasks** — a task needs the timing `wheel` (a `main`-local alloca, not
  a global) and coroutine driving from C. Functions only in V1.
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
`context`/`svScope` and exported tasks are the genuinely large, deferred pieces. A
functions-only, scalar+vec+chandle+string-return export V1 is medium and mostly
mechanical — the scary part (resolution) is solved.

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
from the actual's array type (`build_dpi_open_array_desc` in `LlvmBackend.cpp`).
Accessors landed: `svDimensions`/`svSize`/`svLeft`/`svRight`/`svLow`/`svHigh`/
`svIncrement`/`svSizeOfArray`, `svGetArrayPtr`/`svGetArrElemPtr1`, and the per-
element `svGet/PutBitArrElem1VecVal`/`svGet/PutLogicArrElem1VecVal`.

Two data paths:

- **Zero-copy** for a named-scalar (`byte`/`shortint`/`int`/`longint`) element:
  Clotho's narrow `[N × iW]` element storage *is* a contiguous C `T[N]`, so the
  descriptor points straight at the design array — no marshal, no copy. The C reads
  and (output/inout) writes it in place.
- **Marshal in inline LLVM IR** for a packed `bit`/`logic` element, at any width
  (`emit_dpi_oa_pack_ir` / `emit_dpi_oa_unpack_ir` in `LlvmBackend.cpp`). Because the
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

## Restrictions — loud-fail with a TODO

Supported: **import** — scalar types, packed vectors, `chandle`, `string` (input
arg + return), `input`/`output`/`inout`, `pure`, `context`. **export** — functions,
`input`/`output`/`inout` scalar/chandle/real/vec args, `input` string args,
scalar/chandle/real/void return, any number of tops. Still loud-fail (guards in
`build_dpi_import` / `collect_dpi_exports`), each with a TODO:

- Import `output`/`inout` **string** (the unbounded copy-out buffer hazard — see
  the string section) and any non-open-array unpacked aggregate arg/return. **1-D
  open arrays are supported in all directions** (named-scalar zero-copy / packed-vec
  marshal at any width, above); still loud-fail: multi-dim, dynamic-array / queue
  actuals.
- Export string return (returned-`char*` lifetime), export `output`/`inout` string,
  and one c_identifier exported from more than one scope (`svScope`, LRM 35.5.3, would
  pick the instance; blocked upstream — slang drops the duplicate cId entries).
- The svScope C API is provided with one scope per top; a handle naming a nested
  instance is not available.
- DPI import **and** export **tasks** (`SubroutineKind::Task`) — a task can consume
  time; the coroutine-shaped native boundary (imports) / wheel-from-C (exports) is
  deferred. Functions only.

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
  (`output` string, DPI task, `context`) confirmed to loud-fail via `lower_text`.
  Full suite byte-identical — DPI is additive, no default path changed.
- Return string (C returns `char*`), plus the byte/logic/vec/chandle matrix and
  the `--dpi-lib .so` path, are covered by the CLI matrix, since libc has no
  deterministic `char*`-returning function to probe with.
- **Export** is the CLI-vs-**verilator** cross-check (`result=105` on both): a
  `context` import's C calls an exported SV function through one JIT-linked `.o`,
  covering the round-trip, a design-signal read, real, and void. Automated tests
  cover the export guards and that an export directive lowers + runs; the C-calls-
  into-SV round-trip needs a `.o` (addObjectFile), which the doctest harness does
  not wire.
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
