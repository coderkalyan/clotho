# Interfaces & modports (LRM 25)

Target: SystemVerilog interfaces — interface instantiation, interface ports,
modport views, and signals / tasks / functions declared in interfaces. TODO.md
M3 item ("Interfaces & modports"); all forms loud-fail today at
`wire_ports` (`Lower.cpp`, `"interface/modport port connections are not
supported"`).

Status: **Phase 1 in progress** — interface instances, plain interface ports,
modport-restricted ports, interface subroutines, and interface arrays. Virtual
interfaces, `bind`, generic interface ports, and modport `export` are deferred
(captured below).

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

## Deferred (loud-fail, tracked)

- **Multi-driver interface nets** — a true bidirectional bus net driven
  continuously from both sides hits `record_net_driver_region`'s multi-driver
  `fail` (no net resolution, shared with plain `inout` ports). Single-driver
  interface signals (the common UVM-style `logic` driven from one side) work.
- **Virtual interfaces** (`virtual interface`, `VirtualInterfaceType`, LRM 25.9)
  — a runtime handle to an interface instance; needs a handle slot pointing at
  the interface instance's state sub-struct plus either static single-binding
  resolution or a runtime-indexable subscribe path for `@(vif.sig)` (the event
  engine is statically compiled per signal — the real blocker). M4 (UVM);
  guarded loudly in `lower_type` (`"virtual interface is not supported"`).
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
interfaces → verilator (iverilog's virtual-interface support is weak).
