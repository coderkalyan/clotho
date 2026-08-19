# Classes (LRM 8) — initial support

Status: **Phase 1 landed; Phase 2 single inheritance landed.** A flat class works
end to end (declaration, properties, `new` + constructor + default initializers,
field read/write, methods, handle assignment / `null` / identity, pass-by-value).

**Single inheritance** (LRM 8.13) now works: `extends`, `super.new` /
`super.method`, non-virtual member override, upcast (reference-shared object),
multi-level chains, constructor-less intermediate classes, and `extends B(args)`.
The object body uses **prefix layout** — the base's flattened property table is
copied first (`lower_class_type`, `element` = base Ref), so an inherited property
keeps its base-relative field index and an upcast is the same pointer with no
adjustment. A non-virtual method binds statically (slang picks the
`SubroutineSymbol` by the handle's static type). Base construction (LRM 8.15) is
emitted at each **constructor's entry** from `getBaseConstructorCall()`
(`emit_base_construction` / `emit_ctor_call`, mutually recursive so a ctor-less
middle class still constructs its own base); the body's `super.new` is a no-op
marker, order-equivalent because slang requires it to be the first statement, and
this keeps the base-arg scope correct. Whole-chain property defaults apply
base-first at allocation. Covered by `tests/class_test.cpp`; iverilog cannot
resolve inherited methods from a derived method body, so it is not an oracle
here.

**Static members** (LRM 8.9) now work: a `static` property is one slot shared
across every instance (and inherited by derived classes) — persistent
module-signal storage keyed by the property symbol (`declare_static_prop`), not in
the per-object body, initialized once at time zero via PreInit. It is reachable as
`obj.p`, `C::p`, or a bare `p` in a method, all routed to the shared signal
(`value_lvalue` / `lower_member_lvalue` branch on `VariableLifetime::Static`
before the frame/`this` paths). `static` methods reuse the free-subroutine path
(no `this`); `local`/`protected`/`const` carry no runtime obligation. An aggregate
or container initializer (unpacked struct / array, queue, dynamic array,
associative array) routes through `emit_struct_store` into that shared slot, the
same element-wise path a module-level declaration initializer takes; a `string`
initializer stores whole, bypassing `coerce` (whose `Resize` rejects a string
value). The initializer is lowered with `fn_ctx_` and `ctor_this_` cleared: it is
class scope, and LRM 8.11 allows `this` only inside a non-static method, so a
stray non-static reference must fail loudly rather than bind to whichever frame or
object happened to be mid-lowering when first access triggered registration.

**Virtual functions / dynamic dispatch** (LRM 8.20) now work (Phase 3, part 1). A
class is *polymorphic* iff it or a base declares a virtual method; virtual
propagates down, so polymorphism is uniform across a hierarchy. Design:

- **Body header.** `class_body_ty` prepends a `ptr` (the class descriptor, whose
  slots are the vtable); a property's GEP index is then its 0-based declaration
  index + `class_header_words`, applied in the single member-GEP chokepoint
  (`agg_member`) and `emit_class_body_init`. Every class carries the word — see
  Runtime type identity below for why it cannot be conditioned on polymorphism.
- **Slots.** `compute_vtable` (Lower.cpp) keys slots by method **name** per
  hierarchy (`class_vslot_`): base virtuals take 0..k-1, an override — implicit, or
  of a pure prototype — reuses the base slot of the same name, a new virtual
  appends. Name-keying (rather than an override-chain / `getOverride()` identity) is
  what lets a pure virtual — a `MethodPrototypeSymbol`, which slang does *not* link
  to its concrete overrides — share a slot with them. The concrete table maps each
  slot to the most-derived body of that name, registered via
  `Module::set_class_desc`. The backend emits one constant descriptor global per
  class (`class_desc_global`) and stores the address of its slot 0 into the header
  at `ClassNew`.
- **Dispatch.** A virtual call through a handle lowers to the new `CallVirtual` IR
  op carrying `{resolved-method (for signature/return type), slot, receiver class,
  args}`. The backend (`lower_virtual_call_inst`) loads the callee from the
  receiver's vtable and shares all argument marshaling with a direct call
  (`lower_call_common`). A `super.method()` binds statically — detected
  syntactically (the callee name's left-most component is the `super` handle), since
  slang lowers it with a null `thisClass` indistinguishable from a bare in-method
  call. A bare in-method virtual call dispatches through the enclosing `this`.

Unit-tested over override, inherited body, non-virtual method calling a virtual
internally, `super`, and a derived-introduced slot.

**Abstract classes** (`virtual class`, LRM 8.21) with concrete virtual methods
lower normally — a valid handle type and base whose virtuals dispatch through the
base; a direct `new` on one is rejected (slang, plus a defense guard in
`lower_new_class`).

A virtual method returning a **non-scalar** (string / unpacked struct / queue /
dynamic array) dispatches like any virtual call: `lower_call_common` (shared by
direct and virtual calls) already allocates the aggregate return temp inline, and
the string / handle return temps are pre-allocated by `build_str_temps` /
`build_dynarr_temps`, which now recognize `CallVirtual` (via a shared
`call_func_id` helper). The same pass extends every other `Tag::Call` scan
(by-pointer-arg ABI marking, comb-fusion legality, has-call detection) to
`CallVirtual` — a latent correctness gap from part 1, exercised once a virtual
method takes an output/inout signal arg.

**Pure virtual** methods (LRM 8.21) dispatch on the concrete type through an
abstract base handle — with and without arguments, and via a non-virtual method
that calls the pure method internally. Slang models a pure virtual as a
`MethodPrototypeSymbol` (invisible to `membersOfType<SubroutineSymbol>`) whose
associated subroutine is **degenerate** (null `thisVar` / `returnValVar`, empty
`getArguments()` — though `call.subroutine` at a *call site* does carry the real
args and return type), and does **not** link a concrete override to the prototype
via `getOverride()`. Two pieces make it work: name-keyed slots (above) match the
concrete override by name; and an abstract class's un-overridden slot is filled by
a **prototype-built signature stub** (`build_pure_stub`: `this` + formals from the
`MethodPrototypeSymbol`, returns X, never executed). A virtual call reads its
`CallVirtual` signature source from the *receiver's own vtable slot*
(`vtable_for(recv)->slots[slot]`) rather than `function_id(sub)` — so slang's
degenerate pure subroutine is never lowered; on an abstract handle the slot holds
the stub, on a concrete handle a real override. A bare in-method call to a pure
method reaches `this` because `resolve_method_this` treats any virtual callee as an
instance method (a pure method's null `thisVar` would otherwise misread it as a
free function).

**Virtual tasks** (LRM 8.20) dispatch on the concrete type over the task
(coroutine) ABI. A task's vtable slot holds its ramp function pointer (`fn_decls_`
of a task FunctionId is the coroutine ramp), so `compute_vtable` fills it exactly
like a function slot. The new `TaskCallVirtual` op loads that pointer via the
shared `load_vtable_callee` and drives the coroutine through the identical
ramp/resume/`coro.done` handshake as a direct `TaskCall` (`lower_task_call_inst`
handles both tags). Time-consuming virtual tasks (`#delay`) and non-virtual tasks
that virtual-call a sibling work.

One ordering subtlety this exposed (and fixed for the function path too): the
`CallVirtual`/`TaskCallVirtual` signature `func` is sourced from `virtual_sig_id`
— `function_id(sub)` for a concrete method, the prototype stub for a pure one —
**not** the receiver's vtable. `compute_vtable` lowers method bodies (via
`function_id`) *before* it calls `set_vtable`, so a virtual method whose body
virtual-calls a sibling of the same class would read a null vtable mid-computation.
Sourcing the signature independently of the vtable avoids that; the backend still
loads the concrete callee from the object's runtime vtable at dispatch.

**Parameterized classes** (`class C #(...)`, LRM 8.25) need no dedicated
machinery: slang **monomorphizes** each `C#(args)` into a distinct, fully
parameter-substituted `ClassType` (its `genericClass` points back to the generic
definition). Each specialization lowers here as an independent nominal class —
distinct members, methods with their own `FunctionId`s, and static properties with
their own `ClassPropertySymbol`s → own signals — keyed per `ClassType` by
`class_type_cache_`. So the only work was lifting the `genericClass != nullptr`
guard, plus folding an `obj.param` member (a value parameter read via an instance,
LRM 8.5) to the specialization's constant (slang cannot const-fold it because the
receiver is a runtime handle). Cross-module specialization identity inherits the
existing per-`Lowerer` class-cache limitation (two modules using `C#(int)` intern
two nominal Refs) — not new to parameterization.

`build_pure_stub` covers every shape a concrete override can have. A **pure virtual
task** gets the coroutine-shaped stub: slang types a task prototype's return as
`void`, so the existing void body (a bare `ret_void`) is already right, and the only
task-specific part is the `is_task` flag on the `Function` record — which is what
selects the `(state, rng, wheel, <formals>, record)` task ABI in `declare_task`, the
signature a `TaskCallVirtual` on an abstract handle drives the override's ramp
through. A **non-scalar return** reuses `build_function`'s fall-through return shape:
an unpacked struct / fixed array and a container (dynamic array, queue, assoc) `Ret`
their frame lvalue so the backend copies the storage out through the trailing return
out-param, while a string handle and a class handle `Ret` a loaded value. The stub
stays never-executed either way, yielding what an override that never assigns its
return variable would.

**Runtime type identity and `$cast`** (LRM 6.24.2 / 8.16) work. Every class — not
only a polymorphic one — carries a pointer to its **class descriptor** at object
body field 0, and the descriptor carries the type identity a dynamic down-cast
tests:

```
%clotho.classdesc = {
  [16 x i32] display,   ; ancestor id by depth, 0 past this class's own depth
  [N x ptr]  slots      ; <- the object header points HERE, at slot 0
}
```

- **Cohen's display, not an interval numbering.** `display[d]` is the type id of
  this class's ancestor at depth `d`, ending with the class's own id. So
  `$cast` to a class at depth `d` with id `i` is `display[d] == i`: one load off
  the object's descriptor and one compare, both operands constant at the cast
  site. The classic alternative — DFS pre-order numbering of the whole
  inheritance forest, testing `(tid - lo) <u extent` — is one ALU op *cheaper* but
  makes an id a position in a global order: it needs the whole forest before it
  can emit the first descriptor, and inserting one class renumbers everything
  after it. A display entry needs nothing but the class's own ancestor chain, so
  ids are unique and nothing more (`ClassIds` in Lower.cpp, minted on demand) and
  a future granular/incremental compilation has no whole-design barrier to clear.
  Id 0 is reserved for "no class": it is the display padding past a class's own
  depth, and what a zeroed word reads as, so both fail closed.
- **Fixed display width** (`ir::kMaxClassDepth`, 16). The destination's depth is
  the load's index, so the offset must not depend on the *object's* class — which
  means the display cannot be variable length. A deeper chain is refused loudly.
- **The header points at slot 0, not at the descriptor base**, so virtual dispatch
  is unchanged (a load plus a constant-index GEP) and the display sits at constant
  *negative* offsets — the shape the Itanium C++ ABI gives `typeinfo` at
  `vptr[-1]`.
- **Ids are design-wide, keyed on the slang `ClassType`.** Two tops intern one
  class as two nominal Refs and emit two descriptors (see the cache limitation
  under Parameterized classes); comparing ids rather than descriptor addresses is
  what keeps that sound.
- **Only case 2 of LRM 8.16 reaches run time.** A destination that is the source's
  own class or an ancestor of it, and a literal `null` source, always succeed and
  lower to a plain assignment; two unrelated class types always fail and lower to
  a constant 0 (single inheritance is what makes that conclusive). The emitted
  test is branch-free: a null handle selects a static all-zero stand-in object
  rather than faulting, and answers 1 separately.
- **A source that is null at run time succeeds** (assigns null, answers 1). LRM
  8.16 is ambiguous — case 2 wants "an object that is assignment compatible", and
  null is not an object, but the closing sentence forces failure on a null source
  only "when the source and destination types are not cast compatible", a
  qualification that would be redundant if every run-time null failed. iverilog
  does not implement `$cast` at all, so it is no oracle here; Verilator answers 1.
  Recorded at `lower_cast` in Lower.cpp.
- **The task form of a failed cast is a run-time error** (LRM 6.24.2), reported
  through the same `severity` + `finish` pair `$fatal` uses. This closes the same
  gap for an enum destination.

The header is uniform across a hierarchy because it has to be: prefix layout puts
the base's properties first so an upcast is the same pointer at the same offsets,
and whether some *derived* class introduces a virtual method is not a property of
the base being laid out. Conditioning the header on polymorphism gave a plain base
of a polymorphic derived class no header while the derived class had one, so every
inherited property read through the base handle was off by a word (a base
constructor writing its own first property overwrote the vtable pointer). One
consequence of making it uniform: every class now has a descriptor, so every class
has the reserved `$copy` slot, and the shallow copy of a non-virtual hierarchy no
longer slices the source.

Randomization has since landed alongside `$copy`, and by the same mechanism: two
more reserved slots, `$randomize` and `$randops`, hold per-class synthesized bodies
(docs/design/constrained-randomization.md). slang's built-in virtual `randomize` is
still excluded from ordinary dispatch via `MethodFlags::BuiltIn`; the slot is
reserved by name instead. **Interface
classes** (LRM 8.26) are a separate gap, and a DAG rather than a tree: a display
cannot answer "does this class implement interface I". The descriptor grows a
sorted, transitively closed set of implemented interface ids (plus a bloom word
for a one-load reject) beside the display, which is still computable from the
class's own declaration and its ancestors. A `$cast` *from* an interface-class
handle needs nothing new — the object behind it is always a real class, so its
display answers as usual.

**Null-handle dereference** (LRM 8.4) is a located fatal, not a fault. LRM 8.4
calls an access to a non-static member through a null handle illegal, leaves the
result indeterminate, and lets an implementation issue an error. Because a handle
is raw pointer bits, the untrapped outcome was an `inttoptr 0` and a segfault
attributing nothing to the design, so every `HandleDeref` now emits a compare
against zero into a cold block (`emit_null_handle_check`, branch-weighted
1:2^20, `cold noreturn` call, `unreachable`) that calls `rt_null_handle` — a
compiler-style diagnostic with the file position, the source line, and a caret
under the handle expression — and exits nonzero. The frontend records the
*object expression's* span on the `HandleDeref` (`class_member_lvalue`'s
`handle_span`) so `obj.prop` underlines `obj`, not the whole access; an implicit
`this` has no such span and falls back to the property reference. The one
dereference that skips the guard is the one rooted directly at `ClassNew` — the
property default-initializer stores every `new` emits — since that handle is the
address `rt_alloc` just returned.

## Implementation deviations from the scope-out below

Two simplifications versus the original plan; both reduce code and risk while
staying LRM-correct:

- **A class handle is a 2-state 64-bit value (like `chandle`), not a `{ ptr }`
  slot.** `layout(Kind::Class)` reports width 64 / Domain::Two, so a class
  variable / property / argument reuses the *entire* scalar storage, assignment,
  `==`/`null`, and subroutine-ABI machinery with zero new plumbing — handle
  assignment is a plain i64 store, identity a plain `icmp`, pass-by-value the
  scalar-arg path. Only two backend ops are new: `ClassNew` (alloc + power-on the
  body + `ptrtoint`) and `HandleDeref` (`inttoptr` → an `is_agg` lvalue rooted at
  the object body, off which the existing `MemberSelect` addresses a property).
- **~~No reserved type-id / vtable header word in V1.~~** Superseded: every class
  now carries a descriptor pointer at object-body field 0 (see Runtime type
  identity above). Phase 1 shipped without it, and property `k` was LLVM field
  `k`; `class_header_words` is now 1 for every class and the `struct_fields` table
  and logical field path stay 0-based.

The type layer, storage model, and lowering below describe the interned
`Kind::Class` (nominal identity via a never-deduped `fields_id`; recursion via
intern-first `reserve_struct_fields`/`set_struct_fields`), which landed as
designed.

---

# Original scope-out

Every construct below originally hit the `lower_type` fail (`// TODO: string /
class types`) or the class method-call fail (`fail("class method calls are not
supported")`).

Goal: a solid, performant, simple, correct base — not full OOP. The *lowering*
reuses the unpacked-struct aggregate path almost verbatim; the real design work
is the **type layer**, because a class is the first genuine **reference type**
(shared handle, identity, `null`) Clotho has. All existing "handle" types
(string, dynamic array, queue) are value types that merely happen to own heap
buffers — they deep-copy on assignment. A class handle does not.

## 1. Guiding decisions

- **Reference semantics, not value.** `h1 = h2` copies the *pointer* (one store);
  `h1 == h2` / `h == null` is pointer identity; passing a handle to a subroutine
  passes the pointer by value (no copy). This is the one hard divergence from the
  dynarr/queue handle family (`is_handle_ref`, `Lower.cpp:1346`, deep-copies) —
  classes need a parallel predicate with copy-pointer behavior.
- **The object *body* reuses the struct aggregate storage.** An object is a heap
  block whose layout is `agg_storage_ty` over the property table
  (`LlvmBackend.cpp:774`). Field access is an aggregate lvalue **rooted at the
  loaded handle pointer** instead of the inline state base
  (`signal_lvalue`, `LlvmBackend.cpp:7482`, roots `lv.base`/`lv.root_ty`
  generically) — the `MemberSelect` → `agg_path` → `lv_field_ptr` chain is
  untouched. This is the "lowering looks roughly the same as struct" the task
  named, and it is the whole reason class field access is cheap.
- **Methods are functions with an implicit `this`.** A method lowers to a
  `FunctionId` (`Ir.h:50`) with a prepended `this` pointer parameter; a method
  call loads the handle and passes it as `this`. The entire subroutine ABI
  (frames, args, return, tasks-as-coroutines) is reused. Non-virtual → direct
  `call`, inlinable by the existing pipeline.
- **Leak lifetime in V1.** `new` allocates; nothing frees. SV objects are
  garbage-collected (LRM 8.4) and a program cannot observe a free, so leaking is
  *observably correct* — it only grows memory on unbounded runs. Ref-counting /
  GC is a documented follow-up (§7); it is a large, correctness-sensitive piece
  (reference cycles are idiomatic in testbenches) and is not needed for a base.
- **Minimal runtime surface.** Object allocation reuses `rt_alloc`
  (`Runtime.h:633`). Handle assign/compare/null are pure LLVM (store / icmp /
  null constant) — no `rt_class_*` family at all in V1. This is the performance
  win: a class is a pointer plus GEPs.

## 2. Scope and phasing

Each cut keeps a loud frontend guard + TODO (never silently accept).

**Phase 1 — the base (this scope-out).** A single, flat (non-inheriting,
non-virtual) class:

- Declaration; **properties** (data members) of any type the aggregate storage
  already lowers — packed integral, `real`, `string`, nested struct/array, and a
  **handle to a class** (including self-reference, §3).
- **`new`** — allocate the body, power-on init the fields, run the constructor
  body; constructor args are ordinary function args. Default (implicit) `new`.
- **Field read/write** (blocking).
- **Methods** — functions and tasks, `this`, early return, recursion.
- **Handle** assignment (pointer copy), `null` literal and compare, `==`/`!=`
  identity, pass-by-value through the subroutine ABI.

**Phase 2 — single inheritance + statics.** `extends`, `super`, non-virtual
member override; base fields laid out first so an upcast is the same pointer
(prefix layout); `super.new`; `static` members/methods (one shared slot, not
per-object); `local`/`protected`/`const` (mostly free — slang enforces at
elaboration; no runtime effect, see §8).

**Phase 3 — polymorphism (M4).** `virtual` methods + dynamic dispatch (vtable),
`virtual class`/pure-virtual, parameterized classes, `$cast`, randomization. To
avoid re-laying-out the object body when this lands, **reserve a one-word type-id
/ vtable-pointer header at body offset 0 in V1** (cheap; the property aggregate
starts at offset 1-word).

## 3. Type layer (the design work)

### New reference kind

Add `Type::Kind::Class` to `InternPool.h:70`. Fields it must carry beyond the
common set:

- `fields_id` — the **property** table, reusing the existing `Field` machinery
  (`intern_struct_fields` / `struct_fields`, `InternPool.h:303`) verbatim.
  Properties recurse through `lower_type` exactly like struct members.
- a **method table** — a parallel side array (`methods_id`) mapping each method's
  slang `SubroutineSymbol` → its `FunctionId`. Populated when the class is
  lowered (§5).
- a **base-class `Ref`** — null in V1, plumbed now so Phase 2 needs no type-layer
  change.
- `layout()` returns the width-0 handle sentinel (like `String`,
  `InternPool.cpp:165`). The object-body layout is backend-owned
  (`agg_storage_ty` over `fields_id`), never a plane-packed width.

### Nominal identity — the sharp edge

Structs in this pool are effectively structural (deduped by `ItemView`
hash/eq, `InternPool.h:185`). **Classes are nominal**: two classes with identical
properties are distinct types, and — critically — two *empty* classes would
**collide and merge** under structural interning. The `Type` hash/eq must gain a
**nominal discriminator** (a per-class serial, or the slang symbol pointer folded
in) so distinct classes never unify. This is the single most important
type-layer decision and the easiest to get silently wrong (an empty-class
collision would type-pun two unrelated handles).

Cache per slang symbol as structs already do (`struct_type_cache_`,
`Lower.cpp:908`) — a `class_type_cache_` keyed on the `ClassType*`.

### Recursive / self-referential types

`class Node; Node next; endclass` — a property whose type is a handle to the
enclosing class. Because a handle is **just a pointer**, the property only needs
the class `Ref`, not the body layout, so cycles resolve through the handle
indirection with no fixpoint: **intern the class `Ref` first (empty), then fill
the property table.** (Value structs cannot contain themselves; reference
semantics is what makes this trivial — a real advantage of the handle model.)

## 4. Storage model

- **Handle slot** — a bare `ptr` (wrap as `{ ptr }` for GEP/array-of-handle
  uniformity, matching the string/dynarr slot convention, `LlvmBackend.cpp:610`).
  Powers on as `null` (the state global's `zeroinitializer` gives it for free —
  no init call). A class-typed signal / frame local is one pointer.
- **Object body** — a heap block: `[ i64 type-id header ] ++ agg_storage_ty(properties)`.
  The header is reserved-but-unused in V1 (Phase 3 vtable). Field access:
  load the handle → aggregate lvalue with `lv.base = <loaded ptr>`,
  `lv.root_ty = <body struct ty>`, `is_agg = true` — then the existing
  `MemberSelect` path. **No new backend addressing code**, only a new lvalue
  *root* (handle-deref) beside `signal_lvalue`/`local_lvalue`.

## 5. Frontend lowering (reuses the struct path)

1. `lower_type`: drop the class fall-through at `Lower.cpp:963`; build
   `Kind::Class` (intern-first for recursion), walk `membersOfType<ClassPropertySymbol>`
   for the field table and register methods (§5.1).
2. **Member access**: `lower_member_lvalue` (`Lower.cpp:7486`) currently
   `fail`s on a non-`FieldSymbol` member (`:7489`) — extend it: a class
   property emits the same `member_select` as an unpacked-struct field,
   rooted at the handle deref.
3. **Handle ops**: assignment = plain pointer store (a new leaf case, *not*
   `emit_handle_assign`'s deep copy); `== / != / null` = `icmp` against another
   handle or a null constant.
4. `@(h.f)` sensitivity, NBA to a field or a handle, class handles inside
   containers (queue/dynarr/assoc of handles), `$display %p` of a handle —
   **all deferred, guarded loud** (the container case is the existing "non-POD
   element" gap in `dynamic-arrays.md`).

### 5.1 Methods — `this` spliced as formal 0

A method is a `SubroutineSymbol` in the `ClassType` scope. slang hands over the
two threading points, so methods reuse the whole existing subroutine machinery
(`function_id` → `build_function`/`build_task`, `build_call_args`,
`emit_copyin`/`copyout`, return ABI) with **`this` as formal 0**:

- `SubroutineSymbol::thisVar` (`const VariableSymbol*`) — the implicit `this`
  inside the method body (slang synthesizes it; `ClassType` carries one too).
- `CallExpression::thisClass()` (`const Expression*`) — the object handle at the
  call site, null for a free function (the existing guard, `Lower.cpp:8209`).

**Definition** (`build_function` / `build_task`, `Lower.cpp:8131` / `8053`).
Today formals become frame Locals, formal k == Local k. For a method, *before*
the real formals: if `sub.thisVar != nullptr`, `fn_local(*sub.thisVar)` first, so
**Local 0 = `this`**, `param_types[0]` = the class-handle `Ref`,
`arg_dirs[0] = In`. Inside the body, slang lowers both `this.f` and bare `f` as
accesses rooted at `thisVar`; `lvalue_root`/`value_lvalue` already consult
`fn_ctx_->localmap` (`Lower.cpp:7737`), so they find `thisVar` → Local 0 → load
the handle → aggregate lvalue rooted at that loaded pointer (§4). No new access
code — the struct `MemberSelect` path with a different root. `this` alone is
Local 0 loaded (a bare handle value).

**Call** (`lower_user_call` / `lower_task_call`, `Lower.cpp:8205` / `8245`).
Replace the `fail`: when `call.thisClass() != nullptr`, lower that object
expression to its handle and **prepend it as arg 0**, then `build_call_args`
(`:8228`) for the explicit args unchanged, then `b.call(id, {this, ...args})`.
copy-out for output/inout formals, ref aliasing, and return are reused verbatim.

**The one subtlety.** `this` is an `In` pointer passed **by value** (reference
semantics) — it must *not* take the aggregate copy-in (`build_function:8152`,
`emit_fn_agg_copyin`) that deep-copies a struct formal's body. `this` (and any
class-handle argument) routes through the scalar-pointer ABI: the pointer *is*
the value, no body copy. This is exactly why the reference-handle predicate (§1)
must exist — it separates "class handle → copy pointer" from "aggregate → copy
body".

**`new`** is a constructor method (slang models it as a `new` `SubroutineSymbol`
with `thisVar`). The `new` expression lowers to: `rt_alloc(bodysize)` →
power-on the body (`emit_agg_init`) → call the `new` `FunctionId` with the fresh
handle as `this` + ctor args → yield the handle.

**Virtual** (deferred) only swaps the direct `b.call(id, ...)` for a vtable load
indexed off the body's type-id header — the reason to reserve that header word in
V1 (§4).

## 6. Runtime surface

Essentially none. `new` calls `rt_alloc` (exists). No `rt_class_free`,
no `rt_class_assign`, no `rt_class_new` — allocation is one call, everything
else is inline codegen. (Ref-counting in a later phase would add
`rt_class_retain`/`release`; not now.)

## 7. Lifetime (leak) and the follow-up

V1 leaks. Correct-but-unbounded. The two real options later:

- **Ref-counting** — a header refcount, retain on handle store, release on
  overwrite / scope exit. Simple, but **leaks reference cycles** (parent↔child
  handles, common in UVM). Needs a cycle collector to be complete.
- **Tracing GC** — correct for cycles, larger machinery (root scanning over
  state + frames), the eventual answer for UVM-scale runs.

Neither is needed for a base; both are gated behind a note here and in TODO.md.

## 7a. Shallow copy (LRM 8.12)

`dst = new src` allocates a second object and copies `src`'s properties into it.
Frontend-only: `lower_copy_class` composes `ClassNew` + `HandleDeref` +
`MemberSelect` + the existing per-type copy, so there is no new IR op, no runtime
entry point and no backend change.

**Per property, the type already decides.** `emit_class_body_copy` walks the
class's flattened field table (base properties are a prefix, so one flat loop
covers inherited and own members) and hands each leaf to `emit_agg_copy`, the same
dispatcher an aggregate assignment uses. A string / queue / dynamic array /
associative array property is a value type that owns a heap buffer and deep-copies;
a class-handle property is `ValueCat::scalar`, a plain 64-bit store, which is
exactly 8.12's "objects, however, are not copied, only their handles". Static
properties are shared (LRM 8.9) and are not in the body, so they are untouched for
free. The constructor and the declaration initializers deliberately do NOT run
(8.12 step 1) — that is the whole difference from `lower_new_class`.

**The allocated class is the SOURCE's, not the static type's.** `new b` where `b`
is a base-typed handle holding a derived object must copy the derived object, and
the object model carries no runtime type identity — no type id, no size word, and
the vtable is function pointers only — so the size cannot be read from a handle.
The dispatch mechanism that does exist is the vtable, so a polymorphic class gets a
**synthesized copy thunk**, `(src handle) -> handle`, which allocates exactly its
own class and copies its own (flattened) field table. `compute_vtable` reserves a
slot for it under the name `$copy` — in the same name-keyed map as the virtual
methods, so it inherits down the hierarchy and a derived class's own virtuals
number after it — and fills it with that class's thunk. `new src` then becomes a
`CallVirtual` on that slot: the concrete class's thunk runs, and the copy is the
right class with the right vtable. The thunk's LLVM signature is `(i64) -> i64` in
every class, so the dispatch is type-safe by construction.

Every class has a descriptor and so a `$copy` slot (see Runtime type identity
above), so this covers a non-virtual hierarchy too. It did not originally: the
slot was reserved only for a class that already had a vtable, and a non-virtual
hierarchy copied the static type inline, slicing a derived source. That was argued
unobservable because reaching the sliced-off part needs `$cast` — which is exactly
what `$cast` then made observable.

Costs: one small thunk per instantiated class, emitted whether or not the design
copies anything (the slot has to be filled when the descriptor is built, and
whether a copy exists is not known then), and one more pointer per descriptor. A
copy also leaks like any other `new` (§7).

## 8. Deferred, each loud-guarded

Inheritance/`super`/override; `virtual`/dynamic dispatch/`virtual class`/pure
virtual; parameterized classes; `static` members (Phase 2); `$cast`;
`rand`/`randomize`; class handles in containers; NBA to a class field or handle;
handle in a general rvalue context (`%p`, streaming). `local`/`protected`/`const`
are **not** guarded — slang rejects violations at elaboration, so they carry no
runtime obligation and V1 accepts them as-is.

## 9. Risk / effort

- **Type layer: high design, low code.** Nominal identity (empty-class
  collision) and recursive intern-first are the two things to get exactly right;
  the rest is a `Kind` plus two side tables mirroring `fields_id`.
- **Backend: low.** One new lvalue root (handle deref) + reserve the body header.
  Object addressing, init, and field ops are the existing aggregate machinery.
- **Frontend: medium, mostly mechanical.** New branches at three known hook
  points (`lower_type:963`, `lower_member_lvalue:7489`, `lower_user_call:8209`);
  `new`, `this`, handle ops.
- **Runtime: negligible.** `rt_alloc` reuse.

## 10. Oracle

iverilog supports classes (verify Phase 1 field/method/new/null semantics
against it); its class support is thin for inheritance and statics, so those are
pinned to the LRM. Confirm handle-identity vs value semantics explicitly:
`h2 = h1; h1.f = 5;` must read `h2.f == 5` (shared object, LRM 8.6) — the
single most important behavioral test distinguishing reference from the
deep-copy handle types.
