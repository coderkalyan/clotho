# Interfaces & modports (LRM 25)

Target: SystemVerilog interfaces — interface instantiation, interface ports,
modport views, and signals / tasks / functions declared in interfaces. TODO.md
M3 item ("Interfaces & modports"); all forms loud-fail today at
`wire_ports` (`Lower.cpp`, `"interface/modport port connections are not
supported"`).

Status: **Phase 1 landed** — interface instances, plain interface ports,
modport-restricted ports, interface subroutines, and interface arrays.
**Virtual interfaces** (LRM 25.9) landed too, bar a clocking block or a
subroutine reached through a handle; see that section. `bind`, generic interface
ports, and modport `export` are deferred (captured below).

## The enabling insight

An interface instance is, to slang, a plain `InstanceSymbol` (`isInterface()`)
whose `InstanceBodySymbol` holds ordinary `VariableSymbol` / `NetSymbol`
members — structurally identical to a module. Clotho's whole storage/reference
model is keyed on *symbol identity*: `sigmap_` maps `const ValueSymbol*` →
`ir::Signal`, and every read, write, sensitivity entry, and net-driver record
resolves through that map. Two facts, both confirmed empirically against slang
(compiled probes comparing resolved `ValueSymbol` pointers across scopes):

1. A **plain** interface-port reference (`module m(simple_bus w); … w.sig`) and
   a direct hierarchical reference (`busA.sig`) resolve to the **same**
   `VariableSymbol` living in the connected interface instance's body. Same
   pointer, same `sigmap_` key.
2. A **modport-restricted** reference (`module m(simple_bus.master p); … p.sig`)
   resolves to a distinct `ModportPortSymbol` (one per interface instance), but
   that symbol carries `internalSymbol` pointing back at the interface
   instance's underlying `VariableSymbol`.

So interface storage **shares itself** through the existing hierarchical-value
machinery. The entire aliasing strategy is one canonicalization step applied
wherever a `ValueSymbol` is turned into an `ir::Signal`:

```cpp
const ValueSymbol& canonical_value_symbol(const ValueSymbol& sym) {
    const ValueSymbol* cur = &sym;
    while (cur->kind == SymbolKind::ModportPort) {
        const Symbol* inner = cur->as<ModportPortSymbol>().internalSymbol;
        const ValueSymbol* vs = inner ? inner->as_if<ValueSymbol>() : nullptr;
        if (!vs) break;
        cur = vs;
    }
    return *cur;
}
```

A plain-port / hierarchical reference already lands on the interface
`VariableSymbol`, so it passes through unchanged; a modport reference follows
`internalSymbol` to that same symbol. No copy process, no new storage, correct
sharing.

A **modport expression port** (`.name(expr)`, LRM 25.5.3) has a null
`internalSymbol` and instead carries a connection expression. A plain rename
(`.clean(raw)`) wraps a single whole signal — `canonical_value_symbol` follows
`getConnectionExpr()` when it is a `NamedValue`/`HierarchicalValue` and lands on
that shared signal. A structural connection (`.lo(raw[3:0])`, a concat) maps to
a bit *sub-range* with no single canonical symbol; following it would corrupt
the sibling bits, so `signal_of` rejects a still-`ModportPort` symbol loudly
(deferred — needs the port lowered as a sub-lvalue).

## Why the copy-process port model does not apply

A normal module port (`wire_ports`) is a one-way value mirror: parent and child
each own separate `ir::Signal` storage, kept in sync by a generated continuous
copy process, sensitized on the source. That model is fundamentally
uni-directional and duplicative — wrong for an interface, whose signals are
bidirectionally shared (a modport signal is `output` from one module and
`input` to another, both naming *one* storage cell). Interfaces instead reuse
the shared-storage path already used for cross-hierarchy references
(`top.u1.sig`, LRM 23.6): one `ir::Signal` in the interface instance's scope,
every reference resolving to it.

## Phase 1 changes (`src/Lower.cpp`)

The two-pass `Collect` → `Emit` lowering already guarantees every signal is
registered before any body emits, so an interface instance declared anywhere in
an enclosing scope has its signals in `sigmap_` before a consuming module's body
lowers — the same guarantee that makes downward/sideways hierarchical refs work.

1. **`canonical_value_symbol`** free helper (file-scope, near
   `SensitivityCollector`).
2. **`signal_of`** — canonicalize before the `sigmap_` lookup. Covers reads
   (`value_lvalue` → `signal_of`), writes (same), and `record_net_driver_region`'s
   own `signal_of`. This is the primary choke point.
3. **`SensitivityCollector::record_read` / `mark_written`** — canonicalize
   before the `sigmap` lookup. Without this, `always @(posedge p.clk)` through a
   modport would silently lose sensitivity (the `ModportPortSymbol` is not a
   `sigmap` key).
4. **`record_net_driver_region`** — canonicalize before the `kind != Net` early
   return, so a net driven through a modport output (`assign p.sig = …`) still
   participates in multi-driver detection (the raw `ModportPortSymbol` has kind
   `ModportPort`, not `Net`, and would bypass the check).
5. **`wire_ports`** — an interface port (`pc->port.kind == InterfacePort`) has no
   value to copy; storage is shared. Skip it (`continue`) instead of failing.
6. **`lower_members`** — add `Modport`, `InterfacePort`, `ModportPort` to the
   known-inert member list (an interface body is walked when `lower_instance`
   recurses into the interface instance; its `ModportSymbol` members carry no
   simulation behavior of their own). `Subroutine` is already inert (lowered on
   demand at call sites), so `p.f()` through a modport `import` — which slang
   resolves to the interface instance's own `SubroutineSymbol` — needs no extra
   work.

Interface arrays (`simple_bus bus[N]()`) and interface-array element port
connections ride the existing `InstanceArraySymbol` element path unchanged —
each element is a full interface `InstanceSymbol`, keyed on its own
`ValueSymbol`s.

## Virtual interfaces (LRM 25.9)

A virtual interface variable is a 64-bit handle (`Type::Kind::VirtualIface`, a
plane word like a class or event handle) holding **the address of one elaborated
interface instance's runtime context record** — `@clotho.inst.<hier>.ctx`, the
per-instance descriptor the module-deduplication work already builds. Its layout
is per *definition*, so twin instances present identical field constants:

```
  0             ptr  state base            3             i32  first dump-var index
  1             ptr  NBA shadow base       ctx_blk_base  i32  NBA block ids
  2             ptr  sched-flag base       ctx_wait_base ptr  wait-bucket bases
```

That is exactly the set of bases a member access needs, which is why the whole
feature is one substitution rather than a new addressing model: `vif.sig` is the
ordinary signal lvalue with its base taken from the handle instead of from the
owning instance's symbol. `CtxOverride` installs the handle's record for the
duration of one lvalue's access, and every def-relative base — storage, shadow,
wait buckets, dump-var index — then resolves against it. A reused module's own
body already emits that exact code for itself (`storage_ptr`'s `ctx_field(a, 0)`
arm); a virtual interface only chooses the record at run time.

**The representative.** slang resolves `vif.sig` against a PLACEHOLDER instance
body it caches once per (definition, parameter values) — `InstanceSymbol::
createVirtual` — never against a real instance, so the member symbol is in no
`sigmap_`. `VifDefs` records every elaborated interface instance with its scope
and its members by name; a placeholder is matched to one by
`InstanceBodySymbol::hasSameType`, which is exactly the LRM's type identity (same
definition, all parameter values equal). The matched instance's copy of the member
is the **representative**: it supplies every def-local constant (field path, slot
shape, shadow fields, wait slots, dump-var offset) while the handle supplies the
instance. The registry is design-wide and shared by every Lowerer, because the
interface is elaborated under one top while the class method that dereferences the
handle may be lowered by another.

**Shape forcing.** Every signal of an interface a handle can name keeps the
canonical type-derived shape — no narrowing, no z-elision — the rule a `ref`
actual already follows and for the same reason: the access is emitted once per
definition against a base chosen at run time, so it cannot depend on what the
may-z analysis proved about one instance's writers. `Module::vif_scopes` carries
the instance set from the frontend; the backend spreads it by definition name.

**Waiting.** `@(edge vif.sig)` and `wait(vif.x)` park a frame-resident node on the
member's waiter list in the instance the handle names. The head lives in the
definition's own state struct, one per (member, edge) some wait actually names, so
a waiter reaches it the way it reaches the member — instance base plus def-local
field index — and it is shaped like an event object so `emit_event_link` /
`_unlink` / `_wake_list` serve both with no second engine. `service_wait_queue`
walks it under the same edge fact that guards the buckets, so a write from any
side wakes it. This is the piece the per-signal wait buckets could never give:
their identity and their capacity are both compile-time, and neither is knowable
for a target chosen at run time. The intrusive list has no capacity at all.
`WaitEdge::inst` carries the handle, beside the event `handle`.

**What the LRM removes.** 25.9 bans a virtual interface member from continuous
assignments and from sensitivity lists, and bans the variable from ports,
interface items and unions — so there are no driver slots, no net resolution and
no static subscription to build, and slang enforces all of it (`DynamicNotProcedural`,
`VirtualInterfaceIfaceMember`, `VirtualInterfaceUnionMember`). Nets are driven
through a clocking block or an in-interface driver, per the clause.

**Nonblocking writes.** A member that any handle nonblocking-writes leaves the
per-signal block shadow for the **deferred-record** path (`emit_array_nba` /
`emit_array_commit`, generalized from arrays to scalars). A shadow is one slot per
signal and a handle supplies only a representative, so two handles naming twin
instances would stage into one slot and one write would silently never land.
A record is per *write*: it already carries the destination pointer resolved at
statement-execution time, and it gains a `ptr ctx` field so the replay wakes the
bound instance's waiters. The decision is a property of the **member**, not of the
statement — `vif_passes` runs before `nba_classify`, so *every* writer of such a
member, handle or direct, routes to records and execution order still decides the
winner. The cost is confined to members of vif-bound interfaces that anyone
nonblocking-writes; every other signal's layout is untouched.

**Clocking blocks.** `vif.cb.sig` reads the sampled storage of the instance the
handle names: the clockvar's sampled temp lives in that instance's own scope, so it
is one more def-relative base. A synchronous drive (`vif.cb.d <= x`) writes the
block's `drive_pend` / `drive_val` pair in that instance, and that instance's own
Re-NBA process performs the drive — the LRM 14.16.2 last-write-wins rule falls out
because the two handles' drives land in different instances' slots. The vif
registry carries the clockvars by name (`VifClockVar`), because a placeholder's
`ClockingBlockSymbol` members are not in `sigmap_` for the same reason its signals
are not.

**Interface subroutines.** `vif.task()` is lowered **once for the definition**,
with the handle spliced as formal 0 exactly as a class method takes `this`
(`splice_iface_ctx`, `Function::takes_iface_ctx`). The backend reads it out of the
frame Local at entry and installs it as a **body-wide** instance context, so every
signal the body touches — reads, writes, waits, NBA records — resolves against the
caller's instance. `vif_function_id` builds that body from the representative's
`SubroutineSymbol` and caches it on the placeholder's, so the ordinary per-instance
lowering (`fnmap_`) is untouched and still serves direct calls. A bare call to a
sibling subroutine of the same interface passes the context straight on
(`vif_ctx_arg`), because it is a call on the same instance.

The body-wide install is what forced `CtxOverride` to carry an **anchor**: a
hierarchical reference out of the interface (`top.u1.sig`) has a different anchor
and a different `ctx_ty`, and would otherwise GEP the wrong layout off the record.
`def_relative_for` now gates on `owner.anchor == ctx_override_anchor_`, which turns
the hazard into a compile-time invariant.

**An event control on an EXPRESSION over members** (`@(vif.a & vif.b)`) is
desugared rather than synthesized into a net: sample the expression, park on each
member the way `wait` already does, re-evaluate, loop until it changes. A
continuous net would read the handle before it was assigned and freeze onto
whichever instance was bound at time zero.

An `edge` or an `iff` qualifier on such an expression (`@(posedge (vif.a &
vif.b))`, LRM 9.4.2 takes the edge of the LSB) stays guarded loud: the change form
is the one that occurs, and the qualifier would need the sample compared bit-wise
rather than for inequality.

**A member of an interface the design never instantiates.** The handle can only
ever be null, so the access is an unconditional run-time fatal, and the compiler
says so up front with a `virtual-interface-never-bound` warning. A design that
never executes the access is still legal, so this is not a refusal.

**Which scope a member may live in.** A **generate block** of the interface is not
a separate instance: its signals are fields of the instance's own state struct, on
the same anchor, so `vif.g[0].e` is an ordinary member access. It cannot be found
by name in the body's member list, so the representative is located by its
**ordinal path** — the chain of member positions from the body down. Two instances
of one definition with equal parameters elaborate identically (that is what
`hasSameType` asserts), so the same chain reaches the corresponding member of any
of them, and no path name has to be rendered or parsed.

An interface **instantiated inside** the interface (`vif.i.d`) is the opposite
case and is refused loud: `i` is a separate instance with a separate state struct
and a separate context record, and the handle's record says nothing about it.
Resolving the member against the registry by type alone finds the nested
instance's own signal and then reads it off the *outer* instance's record — the
wrong layout off the wrong base. `vif_member_signal` therefore requires the
member's body to be the body the handle's TYPE names, which is what makes the
difference between these two cases decidable. Supporting it needs the context
record to carry its children's records so a nested base can be reached from the
outer one; a handle taken directly to the inner instance (`virtual inner v = o.i;`)
works today and is the way to express it.

**Writes inside a ctx-parameterized body.** A nonblocking write in an interface
subroutine reached through a handle names a bare member, but the instance is the
one formal 0 named, so it is a write through a handle and must take the
deferred-record path like any other. `collect_vif_nba` marks NBA targets inside a
`takes_iface_ctx` body for exactly this reason; without it the write stages into
the representative's block shadow and commits into the wrong instance.

**Events are woken through the object, not the member.** `@(vif.e)` parks on the
event object in the instance the handle names — the same intrusive list `@(ev[i])`
uses — not on the member's signal waiter list, because `-> vif.e` fires the
object. Routing it to the waiter list instead never wakes at all.

## Deferred (loud-fail, tracked)

- **Multi-driver interface nets** — a true bidirectional bus net driven
  continuously from both sides hits `record_net_driver_region`'s multi-driver
  `fail` (no net resolution, shared with plain `inout` ports). Single-driver
  interface signals (the common UVM-style `logic` driven from one side) work.
- **Virtual interfaces** — landed; see the section above for what is left.
- **Structural modport expression ports** (`.name(sig[hi:lo])` / concat) — see
  the shared-storage section; the whole-signal rename form works.
- **Modport `export`** — a method the connecting module must provide back to the
  interface; heavier wiring than `import`. Phase 4.
- **Generic interface ports** (`interface p`, `InterfacePortSymbol::isGeneric`)
  and **`bind`** of an interface — Phase 4.
- **`$typename` of an interface type** — lands with the shared `$typename`
  aggregate-type gap.

## Oracle

iverilog handles basic interfaces / modports — Phase 1 oracle. Virtual
interfaces → verilator (iverilog's virtual-interface support is weak; it has no
interface support at all, so every virtual-interface expectation in
tests/interface_test.cpp is pinned to the LRM).
