# Interface classes (LRM 8.26)

Target: `interface class` as a declarable type and a handle type, `implements` on a
class or `virtual class` (one or many), `extends` between interface classes
(multiple inheritance, diamonds), pure virtual prototypes dispatched through an
interface-class handle, parameterized interface classes and their `::` type/parameter
access, assignment of a class handle to an interface-class handle, and `$cast` in
every direction that involves one (LRM 8.26.1-8.26.9).

Status: **landed, in the shape scoped below, all three phases.** `interface class`
is a type, `implements` (one or many) works, dispatch through an interface handle
costs exactly what an ordinary virtual call costs, `$cast` works in every direction,
and `randomize()` through an interface handle works. 19 cases in
`tests/class_test.cpp` (the `// -- Interface classes (LRM 8.26) --` section) plus one
in `tests/gc_test.cpp`; the whole suite is green at 2204 cases, and `--emit llvm` is
byte-identical for a class-using design that declares no interface class. The four
places this deviated from the plan are marked **as built** where they appear (§4.3,
§5.2, §6.4, §9). What is still refused, by its own message, is in §8.

The thesis: **this feature needs no new runtime, no new ABI, no new allocation
shape, and no change to how a class handle is stored.** Every part except one
descriptor region and one slot-numbering space already exists and is tested:

- The **class descriptor** hangs off every object body at field 0, and the object
  header points at vtable slot 0, so anything prepended to the descriptor sits at a
  design-wide **constant negative offset** from the handle
  (`src/ir/LlvmBackend.cpp:2213-2239`). That is exactly the shape an interface test
  needs, and it is the shape `$cast`'s ancestor display already uses
  (`LlvmBackend.cpp:15091-15115`; ten green cases at `tests/class_test.cpp:1527-1826`).
- **Name-keyed vtable slots.** `compute_vtable` keys slots by method *name* per
  hierarchy, which is what already lets a `MethodPrototypeSymbol` share a slot with
  its concrete overrides even though slang links them not at all
  (`src/Lower.cpp:3077-3136`). LRM 8.26.6.1 requires a single implementation to
  satisfy every same-named prototype of every implemented interface — name keying
  *is* that rule.
- **Prototype-built stubs.** `build_pure_stub` rebuilds a callable signature from a
  `MethodPrototypeSymbol` alone — function or task, scalar or aggregate return
  (`src/Lower.cpp:3199-3277`). An interface-class prototype is the same symbol kind,
  so it needs no new stub machinery.
- **Reserved descriptor slots by name.** `$copy`, `$randomize` and `$randops`
  already occupy vtable slots keyed by names no SV identifier can spell
  (`src/Lower.cpp:3118-3135`), and every consumer looks them up *by name*
  (`ensure_vtable(clt).at(kCopySlot)`, `src/Lower.cpp:23435`) — so renumbering them
  costs nothing.
- **Specialization identity.** slang monomorphizes `C#(args)` into a distinct fully
  substituted `ClassType`, memoized on the argument tuple
  (`build/_deps/slang-src/source/ast/symbols/ClassSymbols.cpp:1088-1102`), and clotho
  already treats each specialization as an independent nominal class
  (`docs/design/classes.md:124-137`; four green cases at `tests/class_test.cpp:910`,
  `:934`, `:951`, `:971`). A parameterized interface class inherits all of that.

What was built, and it is the list this document predicted: an interface-class `Ref`
that is a handle type with no object (`lower_class_type`'s interface path), one
design-wide numbering space for interface method names plus one for interface ids
(`IfaceIds`), two constant-offset descriptor regions (`class_desc_ty`), one new IR op
(`ClassImplements`), one `uint32_t` on `VirtualCallInfo`, and eight frontend
branches. Measured: +730 / -86 lines of source across `src/Lower.cpp` and
`src/ir/` (Ir.h, Module.h, Builder, Printer, Analysis, LlvmBackend), plus 535 lines
of tests. No runtime change, no ABI change, no allocator change: `src/runtime/` is
untouched.

---

## 1. Semantics (LRM 8.26)

### 1.1 The construct

An interface class contains **only** pure virtual method prototypes, type
declarations, and parameters (LRM 8.26, 8.26.1). No properties, no constraints, no
covergroups, no nested classes, no constructor. It cannot be instantiated (8.26.5).
It may `extends` zero or more interface classes and may not `implements` anything;
a class or `virtual class` may `implements` zero or more interface classes, may not
`extends` one, and may simultaneously extend a class and implement interfaces
(8.26.2).

Four rules carry all the design weight:

1. **`implements` inherits nothing** (8.26.2, 8.26.3). No members, no types, no
   parameters. It is a *requirement* plus a *type relationship*, nothing else. This
   is why the object body, the prefix layout and the trace map need no change at
   all.
2. **The implementation may be inherited** (8.26.2): a virtual method in a base
   class satisfies an interface the derived class declares, and a subclass
   implicitly implements everything its superclass implements. So "does this object
   implement I" is a question about the whole base chain, not one declaration.
3. **One implementation satisfies every same-named prototype** (8.26.6.1), and if
   two prototypes of the same name disagree on signature the class is illegal.
   Method identity across interfaces is therefore *by name*.
4. **A handle of interface type may hold any implementer** (8.26, 8.26.5), and
   `$cast` is legal between interface handles whenever the underlying object
   qualifies (8.26.5) — including the diagonal case `$cast(get_ref, put_ref)` where
   neither static type relates to the other.

Randomization: `randomize()` through an interface handle is legal, `rand_mode` and
`constraint_mode` are **not** (8.26.9). Interface classes carry `pre_randomize` /
`post_randomize` and those never cause name conflicts (8.26.9).

### 1.2 What slang already guarantees

Verified in the vendored tree; clotho may assume all of it and need not re-check.

| guarantee | site |
|---|---|
| an interface class body holds only prototypes, typedefs, parameters (everything else is a parse error) | `source/parsing/Parser_members.cpp:1313-1562` |
| every interface prototype is `MethodFlags::Pure \| Virtual` and bodyless | `source/ast/symbols/SubroutineSymbols.cpp:211-217`, `:1167-1181` |
| `implements` operands are interface classes; interface `extends` operands are interface classes; neither may be a type parameter or an incomplete forward typedef | `source/ast/Lookup.cpp:1494-1522` (`IfaceExtendTypeParam`, `IfaceExtendIncomplete`, `ImplementNonIface`, `ExtendClassFromIface`) |
| a normal class cannot `extends` an interface class | `source/ast/symbols/ClassSymbols.cpp:331-337` (`ExtendIfaceFromClass`) |
| `new` on an interface class is rejected | `source/ast/expressions/AssignmentExpressions.cpp:660-663` (`NewInterfaceClass`) |
| every reachable prototype has an implementation that is present, **virtual**, and signature-identical down to argument names, directions, default-value presence and visibility (no covariant return) | `ClassSymbols.cpp:838-869` + `SubroutineSymbols.cpp:488-596` |
| genuine diamonds are allowed; non-diamond name collisions are rejected | `ClassSymbols.cpp:757-788` (`IfaceNameConflict`) |
| `ifaceHandle = classHandle` is type-checked, **including through base classes** | `source/ast/types/Type.cpp:646-661`, `:741-760` |
| identical specialization arguments yield the identical `ClassType*` | `ClassSymbols.cpp:1088-1102` |

Consequence worth stating plainly: because slang requires the implementing method to
be declared `virtual` (`ClassSymbols.cpp:858-865`, `diag::IfaceMethodNotVirtual`),
`is_user_virtual` (`src/Lower.cpp:3024-3026`) is **already true** for every
interface-implementing method, and each already gets an ordinary name-keyed vtable
slot in its own hierarchy today.

### 1.3 What slang does not do (clotho's obligations)

1. **`getImplementedInterfaces()` is not closed over the base chain.** It is written
   only by `handleImplements`, which runs only when the class has its own
   `implements` clause (`ClassSymbols.cpp:314-319`, `:877-878`), and `findIfaces` is
   applied only to the operand interface (`:820`, `:873`). So for
   `class D extends B;` where `B implements I`, `D.getImplementedInterfaces()` is
   **empty** — the header comment at `ClassSymbols.h:86-87` is wrong in that
   direction. slang gets the right answer anyway because `Type::implements` walks the
   base chain itself at query time (`Type.cpp:741-760`). Clotho must do the same walk
   when building a class's interface set; trusting the comment silently produces a
   `$cast` that answers 0 for a legal down-cast, and that is exactly the bug
   Verilator ships (§14).
2. **`membersOfType<T>()` does not unwrap `TransparentMemberSymbol`**
   (`include/slang/ast/Scope.h:224-227`: `skipToNext` filters on
   `SpecificType::isKind`). Prototypes an interface inherited from its parents are
   transparent members, so enumerating one interface's own `MethodPrototypeSymbol`s
   does **not** see them. The interface closure walk is mandatory, not an
   optimization.
3. **No prototype → implementation link.** `getSubroutine()` fabricates a bodyless
   stub for a pure prototype (`SubroutineSymbols.cpp:1177`) and `getOverride()`
   points at the *parent prototype*, last-writer-wins across a diamond
   (`ClassSymbols.cpp:802-806`). Binding is by name against the implementing class,
   which is what clotho's name-keyed tables already do.
4. **No slot or ordinal API.** Scope order is actively unusable: inherited members
   are prepended one at a time and all carry `indexInScope == 0`
   (`source/ast/Scope.cpp:632-635`, `:918-923`). Any numbering is clotho's own.
5. **No `$cast` checking whatsoever** for class or interface destinations —
   `CastTask::checkArguments` emits only the `DynamicCastConst` *warning*
   (`source/ast/builtins/SystemTasks.cpp:604-629`). All run-time semantics are
   clotho's.
6. **`DynamicCastConst` is wrong for an interface source.** Its always-fails arm is
   `srcType.isClass() && !srcType.isAssignmentCompatible(destType)`
   (`SystemTasks.cpp:622-627`), which fires on LRM 8.26.5's own legal example.
   Measured through clotho today, on `PutImp put_ref; GetImp get_ref;`:
   `warning: $cast from 'PutImp' to 'GetImp' will always fail [-Wdynamic-cast-const]`.
   §6.4 decides what clotho does about it.
7. **No cycle check in `handleImplements`** — unlike `handleExtends`, which has
   `ClassInheritanceCycle` (`ClassSymbols.cpp:345-357`). `interface class A extends A;`
   is not diagnosed, so clotho's closure walk needs a visited set, not faith.
8. **`getDeclaredInterfaces()` is not deduplicated** (`ClassSymbols.cpp:819`,
   `:872`); `getImplementedInterfaces()` is (`SmallSet seenIfaces`, `:733`).
9. **Every interface class still carries eight built-in methods** (`randomize`,
   `pre_randomize`, `post_randomize`, `get_randstate`, `set_randstate`, `srandom`,
   `rand_mode`, `constraint_mode`) and a `thisVar` (`ClassSymbols.cpp:233-258`).
   They are `SubroutineSymbol`s, not prototypes, so filtering on
   `SymbolKind::MethodPrototype` excludes them — and `rand_mode` /
   `constraint_mode` reaching an interface handle is LRM-illegal (8.26.9) but not
   slang-rejected, so it is clotho's guard to write.
10. **`$bits` of an interface class is 0** (`ClassSymbols.cpp:881-885`), and
    `int'(ifaceHandle)` is a `BadConversion`.

---

## 2. Current state — what this work started from

Facts, all established against the code at "backend: drop the always-true has_wheel()" and by running probes
against `build/clotho run`, in the order the design needs them. §13
records what each of them became.

1. **The refusal fires at the interface-typed *entity*, not at the interface
   declaration.** `lower_class_type` is reached from `lower_type`
   (`src/Lower.cpp:2511-2513`) and an interface class satisfies `isClass()`, so the
   guard trips the first time a variable, formal, property, return type or `$cast`
   destination of interface type is lowered. Measured, six probes, every diagnostic
   pointing at the interface-typed *declaration*:
   `p1_basic.sv:11:16: error: interface classes are not supported [unsupported-construct]`
   (`PutImp p = f;`), and likewise for the `$cast`, multi-interface, parameterized,
   partial-implementation and inherited-implementation probes.
2. **`implements` is not guarded at all, and today that is sound.**
   `getImplementedInterfaces` / `getDeclaredInterfaces` appear nowhere in `src/`.
   `lower_class_type` reads only `getBaseClass()` (`:2635-2637`) and
   `compute_vtable` walks only `base_class_of` (`:3098`, `:3141`), so a class with an
   `implements` clause lowers as an ordinary root class. Measured — this design runs
   correctly today and prints `put 7`:
   ```
   interface class PutImp; pure virtual function void put(int a); endclass
   class Fifo implements PutImp;
       virtual function void put(int a); $display("put %0d", a); endfunction
   endclass
   module m; initial begin Fifo f = new; f.put(7); end endmodule
   ```
   It is correct because `implements` inherits nothing (§1.1(1)) and slang already
   forced `put` to be virtual, so the class clotho lowers is the class SV describes.
   The moment the interface *type* is named, the guard fires. So there is no silent
   partial acceptance to repair — but there is also no test pinning either half.
3. **A class handle is a 2-state 64-bit value** (`src/ir/InternPool.cpp:177-182`),
   identical in storage to `chandle`. Assignment is an `i64` store, identity an
   `icmp`, pass-by-value the scalar path, and an upcast is a width-64-to-width-64
   `Resize`, i.e. a no-op (`src/ir/Builder.cpp:139-141`). **No pointer adjustment
   exists anywhere in the class implementation**, which rules out any interface
   design needing an adjusted `this` (§12).
4. **The descriptor is `{ptr map, [16 x i32] display, [N x ptr] slots}`** and the
   object header points at `slots[0]` (`LlvmBackend.cpp:2235-2239`, `:2306-2311`).
   Two offset facts the design must respect, both stated in the source at
   `:2224-2234`: the trace-map pointer is *prepended* so its vptr-relative offset
   (`-72`) is independent of the slot count and is cross-checked against the runtime
   at startup (`gc_map_vptr_offset()` at `:2247-2254`, `rt_gc_map_vptr_offset()` at
   `src/runtime/Gc.cpp:2960-2965`, `gc_fatal` on mismatch at `:2492-2497`); and the
   display must end exactly where slot 0 begins, because `ClassIsA` computes
   `4*class_depth(dest) - 4*kMaxClassDepth` (`LlvmBackend.cpp:15111`).
5. **Vtable slot numbers are per single-inheritance hierarchy, first-come by name**
   (`src/Lower.cpp:3098-3136`), and `VirtualCallInfo::slot` is a compile-time
   `uint32_t` used as a fixed GEP index (`src/ir/Ir.h:451-452`,
   `LlvmBackend.cpp:21666`). Two unrelated classes implementing the same interface
   therefore assign that interface's method **different** slot numbers. This is the
   one genuine design problem (§5).
6. **`class_type_id` mints ids on demand from a design-wide counter starting at 1**
   (`src/Lower.cpp:1039-1046`, `:3039-3057`), and enforces
   `depth < kMaxClassDepth = 16`. Ids are "unique and nothing more — no order, no
   density", deliberately, so minting needs no view of the design.
7. **`lower_cast`'s always-fails arm assumes single inheritance, in writing**
   (`src/Lower.cpp:17040-17049`): *"Single inheritance is what makes this conclusive --
   with two bases a common descendant could satisfy both."* An interface destination
   or source invalidates it (§6.3).
8. **Two whole-design analyses key on the raw slot index across every descriptor** —
   the `ref`-formal epoch ABI union (`src/ir/Analysis.cpp:1724-1755`) and the RNG
   materialization union (`LlvmBackend.cpp:8058-8090`). A second numbering space
   needs its own pass over these, or virtual `ref` formals and `$urandom` state get
   computed against the wrong function set.
9. **`find_pure_proto` walks only the base chain** (`src/Lower.cpp:3062-3074`), and
   `compute_vtable` fails loudly on an unfilled slot in a non-abstract class
   (`:3187-3189`). Both need the interface closure once prototypes can arrive from
   an interface.
10. **`Module::ClassDesc` has no interface state**: `{class_ref, slots, type_id}`
    (`src/ir/Module.h:631-640`), and `Module::class_desc` is a linear scan
    (`:762-769`) already called once per `ClassIsA` site and once per display
    ancestor.
11. **Corrections to `docs/design/classes.md`** found while establishing the above,
    to be fixed when this lands: `set_vtable` (`classes.md:119`) and
    `vtable_for` (`:99`) do not exist — the API is `Module::set_class_desc` /
    `Module::class_desc`; the descriptor sketch at `:151-160` omits the trace-map
    pointer that is now field 0; and `is_handle_ref` is at `Lower.cpp:4235`, not
    `:1346`, and means *container* handle, never class handle.

---

## 3. Decision summary

| # | Move | Why |
|---|---|---|
| 1 | An interface class interns as an ordinary `Kind::Class` `Ref` with an **empty field table**, no descriptor, no vtable, never `ClassNew`'d | LRM 8.26 forbids properties, so the body type is the header word alone and `class_body_ty(iface)`'s field-0 GEP — the only thing dispatch needs from the receiver's static type — is already right |
| 2 | Interface **method names** get a design-wide index (`IfaceSlots`), minted on demand exactly like `ClassIds` | name keying *is* LRM 8.26.6.1; on-demand minting keeps the "no whole-design numbering pass" property (§2.6) |
| 3 | Each implementing class's descriptor grows an **interface-method table** at a design-wide constant negative offset | dispatch stays one load at a constant index — identical cost to an ordinary virtual call, no search, no fat handle |
| 4 | Each class's descriptor grows an **implemented-interface bitmap**, one bit per interface id, width one design-wide constant | the implements test becomes one load and one AND, branch-free, the same shape as the display's one load and one compare |
| 5 | Both regions are emitted **only when the design declares an interface class**, and both go *below* the trace-map pointer | keeps `--emit llvm` byte-identical for every design that does not use the feature, and keeps the map offset at `-72` so the runtime and its startup cross-check are untouched |
| 6 | One new IR op, `ClassImplements`, appended at the end of the tag enum; `VirtualCallInfo` grows one `uint32_t` | mid-enum insertion renumbers tags and breaks `tests/sva_test.cpp`'s `monitor_shape` digests (`process-class.md:679-681`) |
| 7 | `$randomize` / `$randops` also get interface indices | `randomize()` through an interface handle is legal (LRM 8.26.9) and the existing reserved slots are hierarchy-relative, so unreachable from one; the reserved names are already looked up by name, so renumbering is free |
| 8 | A class's interface set is built by walking **its own closure plus every base's**, mirroring `Type::implements` | slang's flattened list stops at the class's own declaration (§1.3(1)) |
| 9 | `$cast` *from* an interface handle to a class needs no new machinery | the object behind it is always a real class, so its display answers as usual |
| 10 | An unreachable interface-table entry holds a shared `rt_iface_unreachable` thunk, not null | a dispatch bug becomes a located fatal instead of a jump to address 0 |

---

## 4. The interface class as a type

### 4.1 The `Ref`

`lower_class_type` (`src/Lower.cpp:2568`) loses the refusal and gains an early
interface path, before the property-chain construction at `:2643`:

- Intern `Kind::Class` with a freshly reserved `fields_id` (nominal identity, never
  deduped — `src/ir/InternPool.h:126-135`), `element = Ref::Invalid` (an interface
  class has no base: slang routes interface `extends` into the *implements* list, so
  `getBaseClass()` is always null for one — `source/parsing/Parser_members.cpp:1160-1166`,
  `ClassSymbols.cpp:331-334`), and an **empty** field table.
- No `compute_vtable`, no `class_type_id`, no descriptor, no `$copy` thunk, no
  randomization bodies. An interface class has no object, so synthesizing a copy
  thunk or a `$randomize` body for it would build code addressing storage that does
  not exist (`copy_thunk_id` at `:23468`, `rand_body_id` at `:23509`).
- Mint an **interface id** from a separate dense counter (`IfaceIds`, 0-based,
  shared across `Lowerer`s the way `ClassIds` is via `set_class_ids` at `:1627-1629`).
  Interface ids index the bitmap of §4.3 and are unrelated to display ids: an
  interface occupies no position in any object's ancestor chain, so it consumes no
  display depth and the `kMaxClassDepth` guard does not apply to it.

Everything else about an interface-typed value follows for free: `layout` reports
the same 64-bit two-state handle (`src/ir/InternPool.cpp:177-182`), so a variable, a
formal, a property, a queue element and a mailbox message of interface type reuse
the entire scalar path; `coerce` routes a class-to-interface assignment to
`b.resize` (`src/Lower.cpp:4285-4298`), which is a no-op between two width-64 refs;
and the GC traces an interface-typed word because it is `Kind::Class`, with the
concrete object's own trace map applying at the other end.

### 4.2 What is refused for an interface class

`new` (slang, `NewInterfaceClass`), property access (no properties exist), and — the
one clotho must add — `rand_mode` / `constraint_mode` through an interface handle,
which LRM 8.26.9 makes illegal but slang accepts because it installs those built-ins
in every class including interface ones (`ClassSymbols.cpp:233-249`). One guard, by
name, at the built-in interception point.

### 4.3 The two new descriptor regions

`class_desc_ty` (`src/ir/LlvmBackend.cpp:2235-2239`) gains two regions, both **only
in a design that declares an interface class**:

```
%clotho.classdesc = {
  [M x ptr]  islots,       ; interface-method table, design-wide index (§5.2)
  [W x i64]  implemented,  ; bitmap of interface ids, W = ceil(K/64)
  ptr        map,          ; rt_gc_map -- still at vptr-72
  [16 x i32] display,      ; ancestor display -- still at vptr-64..vptr-1
  [N x ptr]  slots         ; <- the object header points HERE
}
```

Three properties make this the right placement, and each is load-bearing:

- **The trace-map offset does not move.** `map` still has exactly one pointer and
  one display between it and slot 0, so `gc_map_vptr_offset()` computes `-72` as
  before; only its two hardcoded element indices change (`0`/`2` → `2`/`4`,
  `LlvmBackend.cpp:2250-2251`). The runtime's independent computation
  (`rt_gc_map_vptr_offset()`, `src/runtime/Gc.cpp:2960-2965`) and the startup
  cross-check (`:2492-2497`) are untouched, in both descriptor shapes.
- **`$cast`'s display arithmetic does not move.** The display still ends where slot
  0 begins, which is the invariant `LlvmBackend.cpp:2230-2234` states and
  `ClassIsA` depends on (`:15111`).
- **Both new regions are at design-wide constant negative offsets** —
  `implemented` at `-72-8W`, `islots` at `-72-8W-8M` — because `W` and `M` are
  design-wide constants *by the time descriptors are emitted*. Descriptor globals
  are built in the backend (`class_desc_global`, `:2256-2291`), which runs after the
  whole frontend has finished minting, so both extents are known and uniform. This
  is what buys constant-index dispatch without a whole-design pre-pass in the
  frontend.

`islots` is emitted **only for a class that implements at least one interface**. It
is the deepest region, so omitting it shifts nothing above it, and a class that
implements nothing can never be the dynamic receiver of an interface-dispatched
call — the assignment rules (LRM 8.26, checked by slang) guarantee the object behind
an `I` handle implements `I`. `implemented` is emitted for **every** class in such a
design (all-zero words for a non-implementer), because any handle can be the source
of a `$cast` to an interface type and that load is at a fixed offset.

`ClassDesc` (`src/ir/Module.h:631-653`) grows the two vectors, `set_class_desc`
grows two defaulted parameters, and the interface-typed `Ref` gets no `ClassDesc`
at all. `null_object_global` grows the same two regions, all-zero, so a null handle
in an interface test reads "implements nothing" instead of faulting — the same trick
that makes the display test branch-free.

**As built**, two details the sketch above left open:

- A class's `iface_slots` vector is only as long as the highest index *it* needs, and
  the backend pads it to `M` when emitting (`class_desc_global`). The frontend mints
  indices as it walks, so a class registered early cannot know the final `M`; padding
  at emission is what keeps the offsets uniform without a pre-pass.
- The stand-in object for a null handle became a whole slot-less zero descriptor
  (`clotho.nulldesc`) rather than the bare zero display it is in a design without
  interface classes, because the bitmap now also has to read as zero. The old shape
  is kept verbatim when `iface_count == 0`, which is part of what makes the emitted
  IR byte-identical for a design that does not use the feature.

---

## 5. Dispatch through an interface handle

### 5.1 Why the existing slot index cannot be reused

A call site knows the *interface* statically and the receiver's class not at all.
Slot numbers are allocated per hierarchy, first-come by name
(`src/Lower.cpp:3098-3136`), so `Fifo::put` and `Stack::put` — both implementing
`PutImp::put` — routinely land at different indices. `VirtualCallInfo::slot` is a
compile-time constant GEP index (`src/ir/Ir.h:451-452`,
`LlvmBackend.cpp:21666`), so one number cannot serve both receivers. Something has
to be uniform across *implementers of an interface*, which the per-hierarchy
numbering is not.

### 5.2 The interface-method index

**One design-wide index per interface method *name*.** `IfaceSlots` mints
`std::string_view -> uint32_t` on demand, in encounter order, exactly as `ClassIds`
mints type ids and with the same justification (`src/Lower.cpp:1035-1046`): an index
is unique and nothing more, so minting one needs no view of the design.

Name keying is not a shortcut, it is the LRM rule. 8.26.6.1 requires a single
method to satisfy every same-named prototype of every implemented interface, and
makes the class illegal when their signatures disagree (slang enforces it —
`ClassSymbols.cpp:867-869`). So within one class, all same-named interface
prototypes collapse to one entry by construction. Across *unrelated* classes, two
interfaces may share a name with different signatures (`IntfBaseA::funcBase`
returning `bit`, `IntfBaseB::funcBase` returning `string`, LRM 8.26.6.1's own
example); they then share an index, and each class's entry holds its own body. That
is safe because the call site takes its signature from the *interface's* prototype
and the receiver is guaranteed to implement that interface. Ordinary vtable slots
already mix signatures across unrelated hierarchies at one index for the same
reason.

**Filling a class's `islots`.** For a class `C`:

1. Build `C`'s interface closure: union of `getImplementedInterfaces()` over `C` and
   every base in its chain (§1.3(1)), with a visited set (§1.3(7)).
2. For each interface in the closure, for each of its own
   `membersOfType<MethodPrototypeSymbol>()` — its parents' prototypes arrive via
   their own closure entries, because `membersOfType` does not unwrap transparent
   members (§1.3(2)) — take the method's name, mint or read its index, and fill
   `islots[index]` with the same `FunctionId` the class's ordinary vtable already
   holds for that name (`ensure_vtable(C).at(name)`). A name implemented by an
   inherited virtual resolves through exactly the same lookup, which is how LRM
   8.26.2's inherited-implementation case falls out with no extra code.
3. For a `virtual class` that re-declares a prototype instead of implementing it
   (LRM 8.26.7), the entry takes the prototype stub `build_pure_stub` already
   builds for abstract classes (`src/Lower.cpp:3181-3195`).
4. Every other index in `islots` is unreachable and holds one shared never-executed
   body. Never called if the design is right; diagnosable if it is not.

`$randomize` and `$randops` are minted as interface indices too (decision 7), so
`randomize()` on an interface handle reaches the receiver's synthesized body. Both
are already looked up by name everywhere, so they need no renumbering work.

**As built**, three deviations, all in the same direction — less machinery than
planned:

- The unreachable entry is **not** a new runtime symbol. `iface_unreachable_id()`
  synthesizes one IR function per module (`$iface.unreachable`: a `Fatal` severity
  plus `finish`), cached on the `Module` so several `Lowerer`s share it. No
  `src/runtime/` change, no new JIT-exposed symbol, and the diagnostic is a located
  clotho message rather than a raw trap.
- `randomize()` through an interface handle needed a **signature** as well as an
  index: the reserved slot's `FunctionId` normally comes from the receiver class's
  own synthesized body, and at an interface call site no concrete class is named.
  `iface_rand_sig` builds one never-executed stub per interface Ref per entry point
  (`$iface.randomize` / `$iface.randops`), which is `build_pure_stub`'s trick applied
  to a synthesized method instead of a user one. This was not in the plan; without it
  `h.randomize()` failed with the unrelated "class that is still being laid out"
  message.
- `find_pure_proto` gained the closure walk (it is no longer `static`), which is what
  makes a call through an interface handle to a prototype **inherited** from a parent
  interface find its signature at all.

### 5.3 The call sequence

Frontend, at the two existing virtual-call decision points
(`lower_user_call:24381,24405-24408`, `lower_task_call:24439,24457-24460`): when
`declaring_class(sub).isInterface`, the call is interface-dispatched.
`declaring_class` already returns the interface `ClassType` for a call bound through
an interface handle (`:3322-3328`), and slang hands the call a bodyless stub
subroutine carrying `MethodFlags::Pure` with a non-null `getPrototype()`
(`SubroutineSymbols.cpp:1177`, `Lookup.cpp:1988-1992`) — that triple is the
detection, and `virtual_sig_id` already routes a `Pure` callee to a prototype-built
stub for its signature (`src/Lower.cpp:3337-3345`).

`VirtualCallInfo` (`src/ir/Ir.h:445-458`) grows one field:

```
// Design-wide interface-method index when this call dispatches through an
// interface-class handle; kNoIface otherwise, and then `slot` applies.
uint32_t iface_index;
```

A `uint32_t` keeps the payload trivially copyable, u32-sized and padding-free, which
is what `ExtraPayload` requires; a `bool` would not. `ir::kNoIface = UINT32_MAX`.

Backend: `load_vtable_callee` (`LlvmBackend.cpp:21659-21668`) branches on
`iface_index` and, when set, replaces its constant-index GEP off the vptr with a
constant *negative* offset GEP into `islots`:

```
handle -> null check -> body -> vptr = body[0]
                                     -> callee = load(vptr + (-72 - 8W - 8M + 8*iface_index))
```

Same instruction count as an ordinary virtual dispatch: one load of the vptr, one
load of the callee, both at compile-time constant offsets. The null guard
(`emit_null_handle_check`, `:21662`) is already there and already correct — LRM 8.4
puts a method reached through a null handle in the illegal class, and the vptr load
is the first thing that would fault. `lower_call_common` (`:21684`) is untouched:
nothing in it is class-identity-aware. `TaskCallVirtual` gets the same branch, so a
`pure virtual task` in an interface class dispatches over the existing coroutine ramp
handshake with no new machinery (`docs/design/classes.md:106-114`).

### 5.4 The whole-design unions

The `ref`-formal epoch union (`src/ir/Analysis.cpp:1724-1755`) and the RNG
materialization union (`LlvmBackend.cpp:8058-8090`) both index by raw slot number
across every `ClassDesc`. Interface indices are a **second numbering space**: each
pass needs a parallel loop over `ClassDesc::islots` keyed by `iface_index`, and a
`CallVirtual` with `iface_index != kNoIface` must consult that table rather than the
slot table. Getting this wrong is silent: a virtual `ref` formal gets the wrong
epoch ABI, or `$urandom` inside an interface-dispatched method reads unmaterialized
RNG state.

---

## 6. `implements` testing: assignment and `$cast`

### 6.1 The bitmap test

`ClassImplements` — one new IR op, `{handle, interface Ref}` → 1-bit value, the same
payload shape and value facts as `ClassIsA` (`src/ir/Ir.h:1219-1225`,
`Analysis.cpp:1046-1050`). Codegen mirrors `ClassIsA`
(`LlvmBackend.cpp:15091-15115`) and is branch-free for the same reason:

```
isnull = handle == 0
obj    = select(isnull, clotho.nullobj, inttoptr handle)
vptr   = load obj
word   = load i64 (vptr + (-72 - 8W + 8*(id >> 6)))
hit    = (word & (1 << (id & 63))) != 0
result = hit | isnull
```

Two loads, an AND and a compare, every operand but the vptr a constant at the cast
site. A null handle reads the all-zero bitmap of the stand-in object and answers 1
separately, which is the same LRM 8.16 reading clotho already recorded for the
display test and which LRM 8.26.5 explicitly defers to ("casting from a source
interface class handle that is null is handled in the same manner as ... 8.16").

No bloom word and no sorted id list, unlike the sketch at
`docs/design/classes.md:218-225` and `TODO.md`: interface ids are minted
dense from 0, so a bitmap is exact, smaller for any realistic interface count, and
needs no scan and no fallback path. It is also the only shape whose cost does not
depend on how many interfaces the *object's* class implements.

### 6.2 The four directions

| source (static) | destination (static) | verdict |
|---|---|---|
| class `C` | interface `I` | slang says assignment-compatible when `C` (or a base) implements `I` → static assign, answer 1, no test. Otherwise **runtime `ClassImplements`**: a subclass of `C` may implement `I` |
| interface `I` | class `C` | **runtime `ClassIsA`**, unchanged — the object behind an interface handle is a real class and its display answers as usual |
| interface `I` | interface `J` | `I extends J` (transitively) → static assign; otherwise **runtime `ClassImplements`**, which is LRM 8.26.5's diagonal `$cast(get_ref, put_ref)` |
| literal `null` | either | static assign, answer 1 (existing behaviour: slang types `null` as assignment-compatible with every class handle) |

Plain assignment (not `$cast`) of a class handle to an interface handle needs
nothing: slang has type-checked it through `Type::implements` including the base
chain (`Type.cpp:646-661`, `:741-760`), and `coerce`'s `Resize` is a no-op.

### 6.3 The static-fail arm must not run on an interface operand

`lower_cast` (`src/Lower.cpp:17040-17049`) currently answers a constant 0 whenever
neither static type is assignment-compatible with the other, and says why in the
source: single inheritance makes that conclusive. With an interface on either side
it is not. `$cast(base_ref, iface_ref)` where some `Derived extends Base implements I`
exists is legal and must be able to succeed, and `$cast(get_ref, put_ref)` is LRM
8.26.5's own example. The arm becomes: if either operand's type is an interface
class, fall through to the runtime test chosen by §6.2 instead of folding to 0.

### 6.4 The false `DynamicCastConst` warning

slang warns "will always fail" on exactly the legal diagonal case
(`SystemTasks.cpp:622-627`, measured in §1.3(6)). Two moves, both needed:

1. Report it upstream — the condition is wrong in the presence of interface classes,
   and Verilator has the same bug with worse consequences (§14).
2. Suppress it locally until then.

**As built**, the suppression is finer than the plan's design-wide severity
downgrade, and strictly better for it: the harvest lambda in `lower_compilation`
drops a `DynamicCastConst` **whose own type arguments name an interface class**,
leaving every other always-fails warning in place even in a design that uses
interface classes. slang stores a `const Type*` in the diagnostic's argument list
(`Type.cpp:1397-1400`), so the operand pair is available at harvest time; the
`apply_severity_policy` route was rejected because `DiagnosticEngine::setSeverity`
keys on the diagnostic code alone and would have silenced the correct warnings too.

---

## 7. Parameterized interface classes and `::` access

Nothing new is needed for parameterization itself. slang monomorphizes each
`I#(args)` into a distinct fully substituted `ClassType` memoized on the argument
tuple (`ClassSymbols.cpp:1088-1102`), and clotho already treats every specialization
as an independent nominal class keyed per `ClassType` in `class_type_cache_`
(`docs/design/classes.md:124-137`). Consequences, all of which fall out:

- `PutImp#(int)` and `PutImp#(logic)` are different interfaces, get different
  interface ids, and a class implementing both is two bits in the bitmap. LRM
  8.26.6.3 says exactly this ("each interface class specialization is considered as
  though it is a unique interface class type"), including that there is *no* diamond
  between two specializations of one generic interface.
- A method name shared by two specializations shares one design-wide index, and the
  implementing class must supply one implementation whose signature satisfies both —
  which is the 8.26.6.1 rule slang already enforces, and the name-keyed index
  expresses it without any extra check.
- Parameters and typedefs of an interface class are static and reached through `::`
  (LRM 8.26.3). They are not inherited by an implementing class, so
  `IntfC::t1_t x;` must resolve while a bare `t1_t` must not — both are slang's
  lookup, not clotho's. `I::PARAM` is a constant of the specialization; the existing
  `obj.param` fold that reads a value parameter through a runtime handle
  (`docs/design/classes.md:130-133`) applies unchanged to an interface handle.
- LRM 8.26.4's restrictions (`implements` a type parameter, `extends` a forward
  typedef) are slang's (`Lookup.cpp:1494-1504`).

---

## 8. Refused, each loud — as built

Per PRINCIPLES.md, each of these fails by its own message rather than being
silently accepted. All were measured against the built binary:

- **`rand_mode` through an interface handle** — LRM 8.26.9 makes it illegal and slang
  accepts it (§1.3(9)). Refused by its own message: *"rand_mode through an interface
  class handle is not legal (LRM 8.26.9)"*, and pinned by a test.
  **`constraint_mode`** keeps the pre-existing design-wide *"constraint_mode is not
  supported"* refusal — constraints are unimplemented, so this receiver is not yet a
  distinguishable case; it becomes one when constraint blocks land.
- **`%p` of an interface handle** — no longer a refusal: `%p` of a class handle now
  prints `null` or the handle's own class name, and an interface handle prints its
  DECLARED name (`handle_p_name` takes the static type). Measured: an `I` handle on
  a `C` object prints `I`, a null one `null`. Pinned by a test.
- **`$bits` / bitstream casts of an interface handle** — slang rejects these itself:
  measured *"invalid argument type 'I' to system function"* (`$bits` of a handle) and
  `BadConversion` for `int'(h)`, so no clotho guard is needed
  (`ClassSymbols.cpp:881-885`).
- **1800-2023 nested `interface class` inside a class** — allowed only in that
  language version (`Parser_members.cpp:1559-1562`) and adjacent to the nested-class
  gap already tracked at `TODO.md`. Untouched here.
- **An interface class deeper than the bitmap** — no such limit exists: `W` is
  computed from the design's interface count, not fixed. Recorded so nobody replaces
  it with a constant later. Verified with 70 interfaces (two words) in the suite.

---

## 9. Gating and cost

**Gate.** A design that declares no interface class emits nothing new: the two
descriptor regions are absent, `class_desc_ty` is the exact struct it is today, the
GEP indices in `class_vptr` are unchanged, no `ClassImplements` op exists, and
`--emit llvm` must be byte-identical. **As built** the flag is not a `DesignUses`
member: it is `Module::iface_count`, which the frontend publishes at the end of
`lower_compilation` from the shared `IfaceIds` registry. `DesignUses` records
op-presence facts an IR scan derives; this is a declaration fact already known
exactly once, and the backend needs the *count* anyway to size the regions, so a
separate boolean would have been a second source of truth for the same thing.

Measured: a design exercising classes, single inheritance, virtual dispatch, `$cast`
and `randomize()` but no interface class emits **byte-identical** LLVM before and
after this change (`cmp` on `--emit llvm` output, main worktree vs this one).

**With it set**, per design:

| site | cost |
|---|---|
| every class descriptor | `W = ceil(K/64)` words of bitmap; `K` = interface classes in the design |
| every *implementing* class descriptor | `M` pointers of interface table; `M` = distinct interface method names in the design, plus 2 for `$randomize`/`$randops` |
| an interface-dispatched call | identical to an ordinary virtual call: vptr load + callee load at constant offsets, then the shared marshaling |
| a `$cast` to an interface type | two loads, an AND, an OR — one instruction more than the display test |
| a `$cast` from an interface type | unchanged from today's display test |
| minting | one hash-map probe per interface method name per class, at lowering time only |

Concretely: 60 interface classes and 40 interface method names over 1000 classes is
`1000*8 + 1000*336` ≈ 344 KB of read-only data. The pathological shape is many
interface names times many implementing classes; it is linear in both and it is
data, not code. Nothing in the object body, the allocator, the trace map or the
scheduler changes size.

---

## 10. IR, printing, legalization

- **One new tag, `ClassImplements`, appended at the end of `Inst::Tag`** — inserting
  mid-enum renumbers every later tag and breaks `tests/sva_test.cpp`'s
  `monitor_shape` digests without moving an instruction
  (`docs/design/process-class.md:679-681`). Payload `op_type` (`{handle, iface Ref}`),
  like `ClassIsA`.
- **`Printer`**: one arm for `ClassImplements`, and `CallVirtual` /
  `TaskCallVirtual` print `@ifaceK` instead of `@slotK` when `iface_index` is set.
  Measured on the `$cast` probe: `%4 = call_virtual(vfn9@iface0, %1, %3)` and
  `%10 = class_implements(%9 -> class{})`.
- **`Analysis`**: `ClassImplements` joins `ClassIsA`'s value-facts arm (a definite
  1-bit answer, `val_z = 0`, `Analysis.cpp:214-215`, `:1046-1050`); the two
  slot-keyed unions gain their interface-index parallel (§5.4).
- **`Legalize`**: an interface-dispatched call is already a `Finding::Call` through
  the existing tags (`src/legalize/Legalize.cpp:642-647`); `ClassImplements` is pure
  and synthesizable-neutral in the same way `ClassIsA` is.
- **`Eval`**: no constant-folding arm. Whether an object implements an interface is
  a run-time question about a handle; the statically decidable cases never reach the
  op (§6.2).

---

## 11. Hazards

Ranked by how much damage a naive implementation does.

1. **Trusting `getImplementedInterfaces()` to be closed over the base chain.** It is
   not (§1.3(1)), and the failure is a `$cast` or an assignment that silently
   answers "does not implement" for `class D extends B;` where `B implements I` —
   LRM 8.26's very first example. Verilator ships this bug (§14). The mitigation is
   the explicit base-chain union, and the test is precisely that example.
2. **A non-uniform descriptor layout.** The map pointer's offset, the display's
   adjacency to slot 0, and the two new regions' offsets are all design-wide
   constants that generated code dereferences directly. Insert a region in the wrong
   place and `$cast` reads a function pointer as a display entry, or the GC marks
   garbage. Two of the three failures are loud (the startup cross-check at
   `Gc.cpp:2492-2497`); the display one is silent. Mitigation: both new regions go
   *below* the map pointer, and `gc_map_vptr_offset` keeps computing from the
   `DataLayout` rather than a literal.
3. **Non-deterministic index minting.** If an interface id or a method index is ever
   assigned by iterating a hash map, the emitted LLVM changes run to run and every
   `--emit llvm` comparison becomes noise. Mitigation: mint on first encounter in the
   deterministic lowering walk, exactly as `ClassIds` does; never iterate
   `class_vslot_` or an interface set to assign a number.
4. **The second numbering space escaping the whole-design unions** (§5.4). Silent
   wrong ABI for a virtual `ref` formal, or unmaterialized RNG state, only in designs
   that use both features.
5. **`compute_vtable`'s concrete-class check firing on a legal abstract class.**
   `if (!clt.isAbstract) fail("concrete class has an unimplemented pure virtual method")`
   (`src/Lower.cpp:3187-3189`) is right for the base chain; a `virtual class` that
   re-declares an interface prototype (LRM 8.26.7) must reach the stub path, and a
   *concrete* class can never get there because slang already required the
   implementation. Keep the check, route `find_pure_proto` (`:3062-3074`) through the
   interface closure.
6. **A cyclic interface `extends`.** slang does not check it (§1.3(7)); an unvisited
   closure walk hangs the compiler. A visited set, and a loud failure rather than a
   silent break, because a cycle is not a legal design.
7. **`implements` on a class whose interface method is *also* an ordinary virtual of
   a base.** The same `FunctionId` then occupies both an ordinary slot and an
   interface index. That is correct and costs 8 bytes; the hazard is "fixing" it by
   trying to alias the two numbering spaces, which cannot work because one is
   per-hierarchy and the other design-wide.
8. **Synthesizing per-class bodies for an interface class.** `copy_thunk_id` /
   `rand_body_id` would build code addressing an object that cannot exist. The
   interface path in `lower_class_type` must return before `compute_vtable`, not
   after.

---

## 12. Rejected alternatives

- **Fat interface handles (the Go itab model): an interface-typed value is
  `{itab, object}`, built at the statically known assignment site.** Dispatch would
  be one load with no descriptor region at all. Rejected because a class handle is a
  64-bit scalar everywhere (`InternPool.cpp:177-182`) and that single fact is what
  gives classes free assignment, free `==`, free argument passing, free container
  elements and a one-word trace-map entry (§2.3). A second width for interface-typed
  slots would fork all of it, make handle identity type-dependent, and require a
  two-word trace-map leaf where one word is traceable.
- **Per-(class, interface) itables reached through a sorted interface-id array.** The
  classic C++/Java shape. Rejected because dispatch becomes a search: load the array,
  scan or binary-search for the statically known interface id, then index. It buys
  nothing here — the flat design-wide index is exact, needs no grouping, and its
  extent is known at descriptor-emission time anyway.
- **Folding interface method names into the existing per-hierarchy slot numbering by
  seeding every class's slot map with a design-wide interface region.** Saves the
  separate table. Rejected: the numbering must be identical in a base and its
  derived classes (prefix property), so seeding has to happen before *any* class is
  numbered, which requires a whole-design pre-pass over every interface class before
  the first descriptor — the property `ClassIds` was deliberately designed not to
  need (`src/Lower.cpp:1035-1038`, `classes.md:169-176`) — and it pads every class in
  the design, not only implementers.
- **A sorted id list plus a bloom word for the implements test**, as sketched at
  `classes.md:218-225`. Rejected in favour of the bitmap: ids are minted dense, so
  the bitmap is exact (no fallback scan), branch-free, smaller below ~500 interface
  classes, and independent of how many interfaces the object's class implements.
- **Interval numbering (DFS pre-order) over the interface DAG.** A DAG has no
  interval numbering; this is why the class display cannot answer the question in the
  first place (`classes.md:218-220`).
- **A runtime helper `rt_class_implements(type_id, iface_id)` over a design-wide
  table.** No descriptor change, but a call (or a hash probe) on every cast and no
  way to reach an interface method at all. The descriptor already gives constant-time
  access from a handle; paying a call to avoid two words of data is backwards.
- **Refusing multiple `implements` in V1.** Tempting, and rejected: the parameterized
  producer/consumer shape in LRM 8.26's own examples (`Fifo implements PutImp#(T),
  GetImp#(T)`) is the reason the feature exists, and the bitmap plus the flat index
  cost nothing extra for the multiple case.

---

## 13. Phased plan — as built

All three phases landed on one branch, in this order, each compiling and green on
its own. The plan held; the deviations are the four marked **as built** above.

**P1 — the type and dispatch.** The interface path in `lower_class_type` (§4.1),
`IfaceIds` (both numbering spaces), the `islots` region and the gated descriptor
shape, the `iface_index` field on `VirtualCallInfo`, the two frontend call-site
branches, the `load_vtable_callee` branch, the shared unreachable body, the interface
closure walk, the `find_pure_proto` extension, and the union parallels (§5.4). At the
end of P1: declaration, assignment to an interface handle, and dispatch through one —
including an inherited implementation, a partial implementation in a `virtual class`,
multiple interfaces with a shared method name, an interface extending interfaces, a
diamond, a parameterized interface class, and a `pure virtual task` that consumes
time. Cost, measured: it is the bulk of the change.

**P2 — `$cast` and the implements test.** The `implemented` bitmap, the
`ClassImplements` op and its codegen, `lower_cast`'s arms and the static-arm fix
(§6.2, §6.3), the `null_object_global` extension, the `DynamicCastConst` suppression
(§6.4). As predicted: `ClassIsA` with a different constant offset and an AND.

**P3 — randomization and the illegal built-ins.** `$randomize` / `$randops`
interface indices so `randomize()` works through an interface handle (LRM 8.26.9),
the two signature stubs that turned out to be needed with them (§5.2), and the
`rand_mode` refusal. `constraint_mode` keeps its existing "not supported" refusal —
constraints are unimplemented design-wide, so the LRM-illegality of that particular
receiver is not yet a distinguishable case.

**Cleanup, done here:** the `TODO.md` entry and the `$cast` residue note, and
the stale sketch in `docs/design/classes.md`. The three stale symbol references
(§2.11) are corrected where this document cites them; `classes.md`'s own body still
names `set_vtable` / `vtable_for` and is left for the next change to that file.

---

## 14. Verification

**The oracle situation was measured** (`iverilog 13.0`,
 `Verilator 5.050`, `build/clotho` at "backend: drop the always-true has_wheel()"):

- **iverilog 13 cannot parse interface classes at all.** Every probe:
  `p1_basic.sv:1: syntax error / I give up.` It is not an oracle for any part of
  this feature.
- **Verilator 5.050 implements a large, useful subset**, and scored correctly on:
  dispatch through an interface handle (`put 3` / `done`), a class implementing two
  interfaces with a shared method name (`1 1`), a parameterized interface class
  (`put 9`), and a `virtual class` partial implementation completed by a subclass
  (`1 2`).
- **Verilator gets two LRM-explicit cases wrong**, and both are cases clotho must
  get right:
  1. **An implementation inherited from a base class** — LRM 8.26.2's own example
     (`class ExtClass extends BaseClass implements IntfClass;`, LRM p. 209). Measured:
     `%Error: p7_inherit_impl.sv:3:1: Class 'Ext' implements 'I' but is missing
     implementation for 'f' (IEEE 1800-2023 8.26)`. It rejects a legal design; the
     same base-chain gap described in §11(1).
  2. **`$cast` between interface handles** — LRM 8.26.5's own example
     (`$cast(get_ref, put_ref)`). Measured: a compile-time
     `%Warning-CASTCONST: $cast will always return zero as 'class{}GetImp' is not
     castable from 'class{}PutImp'`, then at run time
     `%Error: p3_cast.sv:16: Null pointer dereferenced / Aborting...`. It compiles a
     legal cast to a guaranteed failure.
- **slang shares the second bug** at the diagnostic level only
  (`DynamicCastConst`, §1.3(6)); it is a warning, and its `$cast` semantics are
  clotho's to define in any case.

**Settlement.** Per PRINCIPLES.md, where iverilog does not cover the construct the
strategy must be named explicitly: **score the Verilator-supported subset against
Verilator 5.050, and pin everything Verilator declines or gets wrong to the LRM
text**, with the LRM clause quoted in the test comment.

| case group | oracle |
|---|---|
| declaration, assignment to an interface handle, dispatch through one | Verilator 5.050 |
| multiple `implements`, shared method name (8.26.6.1) | Verilator 5.050 |
| interface `extends` interface, diamond (8.26.6.3) | Verilator 5.050 |
| parameterized interface classes, specialization identity (8.26.3, 8.26.6.3) | Verilator 5.050 |
| partial implementation in a `virtual class` (8.26.7) | Verilator 5.050 |
| **inherited implementation (8.26.2)** | **LRM 8.26.2 only** — Verilator rejects a legal design |
| **`$cast` between interface handles (8.26.5)** | **LRM 8.26.5 only** — Verilator compiles it to a null dereference |
| `$cast` interface → class, null source, task form | LRM 8.16 + 8.26.5, and clotho's own recorded reading (`classes.md:186-201`) |
| `randomize()` through an interface handle; `rand_mode` refusal (8.26.9) | LRM 8.26.9 only |
| every refusal message | clotho-specific |

A Verilator-scored case must say so in its comment, and an LRM-pinned case must
quote the clause, so a later Verilator release changing behaviour cannot silently
reclassify a pinned case.

**Landed:** 20 cases in `tests/class_test.cpp` under a
`// -- Interface classes (LRM 8.26) --` banner, using that file's own harness
(`run_capture(src)` for an exact-stdout `CHECK`, `fails(src)` for a refusal
substring), plus one in `tests/gc_test.cpp` through `check_both_cadences`. A new TU
was not warranted: these are class cases and they share the harness. One case per row
of the table above, and each carries its oracle status in its comment —
"Cross-checked vs Verilator 5.050" or "**LRM 8.26.x only**" with the reason
Verilator disagrees. The whole suite is green: **2204 cases, 20597 assertions**.

The cases: dispatch through an interface handle; an `implements` clause alone;
two interfaces sharing a method name; `extends` plus dispatch through a parent
interface; a diamond with an inherited parameter; an implementation supplied by a
base class (and by a base's base); partial implementation in a `virtual class`; a
parameterized interface and implementer; typedef and parameter through `::`; a
`pure virtual task` that consumes time; `$cast` between two interface handles;
`$cast` from an interface handle to a class the interface knows nothing about; a
`$cast` miss and a null source; an interface id in the second bitmap word (70
interfaces, so the word index is exercised); handle identity, arguments and
properties; a fork child dispatching a virtual method through a CAPTURED
interface-class handle (the one case that only exists because interface classes and
fork V4's capture boxes landed on separate branches and met at the merge -- see
below); an object rooted only by an interface handle across a GC; `randomize()`
through an interface handle; the `rand_mode` refusal; `%p` of an interface handle.

**Also measured, outside the suite:**

- **Byte-identical `--emit llvm`** for a design exercising classes, inheritance,
  virtual dispatch, `$cast` and `randomize()` with no interface class — `cmp` clean
  against the pre-change binary. This is the gate's only real test, and it was run
  after every phase.
- Every probe under `--gc-stress` as well as the default cadence, byte-identical
  output.
- The IR printer on the `$cast` probe, so the new dispatch and op are inspectable:
  `call_virtual(vfn9@iface0, ...)`, `class_implements(%9 -> class{})`.
- **Sanitizers** (`-DCLOTHO_SANITIZE=ON`, ASan + UBSan, `detect_leaks=1`): the 19
  interface cases plus the GC case are clean and leak **zero bytes** — better than the
  `process` suite's 6096 bytes in 30 allocations, which the same build reports and
  which is what proves leak detection was live. No UBSan finding anywhere in them.
  The full suite under ASan reproduces one pre-existing failure unrelated to this
  work: `tests/data_model_test.cpp`'s matrix spawns child designs and treats a
  nonzero child exit as a finding, and with `detect_leaks=1` a child's own 8192-byte
  teardown leak makes LSan exit 1. The matrix passes with `detect_leaks=0`, and the
  leaking allocation is in the child's runtime, not in anything this change touches.

**Still owed:**

- The legality corner cases from slang's own suite
  (`build/_deps/slang-src/tests/unittests/ast/ClassTests.cpp:1230-1645`) as `fails()`
  cases wherever clotho's message differs from slang's.
- An upstream report for slang's `DynamicCastConst` condition (§6.4) and for
  Verilator's two LRM violations (above).

---

## 15. LRM decisions this design records

- **Name-keyed interface method indices are LRM 8.26.6.1, not an optimization.** A
  single implementation satisfying every same-named prototype is the normative rule;
  two same-named prototypes with different signatures in one class is an error slang
  already reports. Two *unrelated* classes may hold different signatures at one
  index, which is sound because the call site's signature comes from the interface
  and the receiver is guaranteed to implement it.
- **A null source in a `$cast` to an interface type succeeds** (assigns null, answers
  1). LRM 8.26.5 defers null handling to 8.16, and clotho's 8.16 reading is already
  recorded and tested (`classes.md:186-201`): the closing sentence of 8.16 forces
  failure on a null source only "when the source and destination types are not cast
  compatible", a qualification that would be redundant if every run-time null failed.
- **An unrelated static type pair is no longer conclusive.** LRM 8.26 lets a common
  implementer relate two otherwise unrelated types, so the always-fails fold is
  restricted to pairs where neither side is an interface class
  (`src/Lower.cpp:17040-17049` gains that condition, and its comment gains the
  reason).
- **`rand_mode` / `constraint_mode` through an interface handle is refused, not
  guessed.** LRM 8.26.9 says they "shall not be legal"; slang installs the built-ins
  anyway, so clotho refuses by name rather than picking a meaning for a construct the
  LRM forbids.
- **An interface class occupies no display depth.** It is not an ancestor of any
  object: LRM 8.26.2 forbids a class from extending one, so the ancestor chain
  `kMaxClassDepth` bounds is unaffected by how deep an interface DAG goes.
- **`randomize()` through an interface handle is legal and must dispatch to the
  object's own body** (LRM 8.26.9), which is why the two reserved randomization
  names get interface indices rather than a refusal.

## Interaction with fork capture boxes

This feature and fork V4's capture boxes (`docs/design/fork-join.md` V4) were
developed on separate branches and met at the merge, which is exactly where a
cross-feature hole would hide: neither branch's tests could reach the combination,
and the merge was textually clean, so nothing would have flagged it.

They compose with no work, and the reason is that each is expressed in a mechanism
the other already understands. An interface-class handle is an ordinary class handle
at run time (§"Representation" — the type is a bitmap test, not a different
pointer), and a capture box is a synthetic one-field class whose field type is the
captured variable's declared type. So capturing an interface-class-typed automatic
boxes a handle, and reading it back yields a handle that dispatches exactly as it
would have in the enclosing frame. Nothing in `lower_fork` or `box_class_type`
consults a slang type, and nothing in the interface-class path consults storage
class.

What that buys, pinned by `interface class: a fork child dispatches through a
captured handle` in `tests/class_test.cpp`:

- a fork child reaching a class property through the enclosing method's implicit
  `this`, then calling a **virtual** method on it — `this` is a captured automatic
  like any other, so the child re-roots bare property references at the boxed handle;
- a captured `string` property, which the predecessor packed-slot ref ABI could not
  carry at all;
- a **detached** (`join_none`) child dispatching through a captured
  interface-class-typed local *after the method that spawned it has returned*, which
  is the lifetime the box exists for and the case an `ArgDir::Ref` pointer into the
  parent frame could never have served.

Every capture in that design was refused outright before V4, so the interaction is
not a regression risk that was always latent — it is new surface, and it is the
merge's semantic proof rather than a formality.

