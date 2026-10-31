# Modular compilation

Status: steps 1 and 2a/2b/2c are on this branch (2d, lowering a specialization
once, is not), and step 3, the backend per unit, is under way: layout, records,
groups, functions and tasks are lowered once per unit (3a-3c), each unit has
its own LLVM module named by the unit with the instance glue in root, and the
gate test passes (3e; see each below for what is left). The frontend's output
is a `Design` (src/ir/Design.h) with one unit per specialization wherever its
instances lower identically, which `flatten` expands back into the flat module
the backend consumes (`clotho run --stat=units` reports what is still split:
none on rocket-chip, nor on a design whose instances print `%m`, report through
assertions and build covergroups). The frontend still lowers every instance, and
the whole-design analyses still decide the layout; `module-split.md` describes
the LLVM-level partition. An instance's own path is read through `inst_path`,
the address of a borrowed constant string held in the instance's record; it
costs a body nothing until a report fires.

Goal: compile an SV design as a composition of small units, so compilation can
be incremental, cached and parallel. The aim is an architecture whose unit
boundaries follow SV semantics, not a compile-time speedup of the current
pipeline. The speedup follows from the boundaries.

## Where the pipeline is today

Every stage up to and including LLVM IR construction is one whole-design pass:

- slang parses with `singleUnit` and elaborates the whole design.
- Lowering walks every elaborated **instance** into one flat `ir::Module`. A
  definition instantiated N times is lowered N times; `getCanonicalBody` is not
  used. Signal, process, function and scope ids are positions in design-wide
  tables and appear verbatim in IR bodies.
- `analyze()` runs whole-design fixpoints (may-z, undriven, pointer arguments)
  whose results decide state layout.
- The backend builds every LLVM module in one serial pass: the groups and the
  functions and tasks of an instance go into the module of its unit, and the
  code of no instance (a package's, `$unit`'s), NBA commit functions,
  initializers and the seed loop stay in root.
- Nothing is cached. `clotho build` links every module back into one.

The partition is drawn after every whole-design decision, so no stage above
LLVM can be incremental, and the LLVM units are not a function of their own
source: a reader added in one module changes the wake code of another, and
global ids leak into unit code as immediates.

## Granularity

What decides a module body's meaning is its definition, the definitions it
imports (packages, classes, `$unit`), its parameter values, the
specializations of the interfaces on its interface ports, and whatever
elaboration injects (`bind`, `defparam` and configuration targets). Elaboration
computes exactly these (LRM 3.12, 23.10.4); an instance's name, where its ports
lead and who references it are context.

That tuple is slang's `InstanceCacheKey` (definition, parameter values, type
parameters, interface port keys). slang refuses to share a body in exactly the
cases where context leaks into it: targets of `defparam`, `bind` or a
configuration, and bodies with upward hierarchical names.

| Level | Role |
|---|---|
| File | Parsing only. |
| Specialization (slang canonical body), package, `$unit`, class specialization | **Unit**: the grain of lowering, code, caching and parallelism. |
| Instance | Data (state storage) and bindings. Never code. |
| Design | Link: hierarchy, bindings, global numbering, scheduling plan. |

Rejected grains:

- Files carry no semantics; macros cross files within a compilation unit.
- An unparameterized definition compiled like a template: type parameters and
  generate constructs change structure, so there is no single body to compile.
- Instances: code grows with instance count, every copy is a separate cache
  entry, and N copies of one body cost instruction-cache pressure at run time.
- Processes: a process's code is loads, stores and wakeups against its unit's
  layout (field offsets and slot shapes over every signal the unit declares,
  scheduling-flag and wait-queue slots, the wake code of every sibling that
  reads what it writes, calls to the unit's functions). An edit to a module
  usually changes that layout, so a per-process boundary buys no reuse, and
  making each process independent means one relocation per signal access.
  Splitting a large unit into several LLVM modules for codegen parallelism stays
  available underneath the unit.

No stage compiles per instance. Everything that differs between instances of one
specialization is data: the instance name (`%m`, VCD scopes, report banners),
bindings, RNG seeds, and covergroup and assertion instance records.

## The boundary

LRM 23.3.3 splits every cross-unit edge into one of two kinds.

**Value flow.** Input and output ports: "each port connection shall be a
continuous assignment of source to sink". The assignment is a process in the
parent's unit, reading or writing the child's port. Value flow composes through
the scheduler.

**Name binding.** A ref port is "treated as hierarchical references to the
variable to which it is connected"; an inout is a bidirectional connection;
connections of user-defined nettypes on both sides "shall be merged into a
single simulated net"; an interface port is connected to an interface instance.
Hierarchical references, package items and `$unit` items are bindings too. A
unit imports a binding by name; the link step resolves it per instance. Binding
composes through the linker.

## Invariants

1. A unit's IR, and later its object code, is a pure function of its key. No
   design-global index appears in a unit's IR; global numbering exists only at
   link time.
2. Whole-design work reads unit summaries and the hierarchy, never unit bodies.
3. A fact a unit's context imposes on it (an outside writer, a `force` from
   another unit, a waiter on an exported signal) is either data the unit's code
   reads at run time, or an entry in a small explicit summary that is part of
   the unit's codegen key. Facts are joined over every instance of the unit:
   code is shared, so it must be correct for all of them.
4. A specialization's layout depends only on the specialization and on the port
   facts in its summary. Virtual interfaces and body sharing both rely on this.

## IR shape

`ir::Design` (src/ir/Design.h) is the frontend's output:

```
Design
  pool       InternPool        shared by every unit until per-unit pools land
  units      deque<Unit>
  roots      vector<Root>      top-level instances, and the static instance of
                               every package, $unit and class unit
Unit
  kind, def_name
  signals    UnitSignal{SignalInfo, import, scoped}  local definitions and extern declarations
  functions  UnitFunction{Function, name, import}    ditto, with the name others import by
  processes  vector<Process>
  imports    vector<Import>
  inst_refs  vector<InstanceRef>   instances named by IfaceRef
  children   vector<ChildSlot>     the instances this body declares
  bound_ports                      ports the instantiating parent binds
  disable_targets                  UnitDisableTarget{is_task, flush_proc, name, import}
  cover_types                      UnitCoverType{CoverType, name, import}
  monitors, class descriptors, DPI exports
```

### Handles

`Signal` and `FunctionId` stay `u32` handles in instruction payloads, but they
index the unit's own tables. A handle whose entry carries an import is an
**extern declaration**, like an LLVM external global: its `SignalInfo` /
`Function` gives the expected shape and the import says where the definition
lives. Instruction bodies therefore keep their shape; only the table behind the
handle changes.

Every other cross-table id in a body is unit-local too: assertion monitors,
covergroup types (importable: an instance builds a package's type), `disable` targets (importable, because a remote `disable` can
name a block in another instance), and the interface instances `IfaceRef` names
(an index into `inst_refs` instead of a global scope id). `Ir::rebased` moves a
body between handle spaces through a `HandleMapper`, one entry point per
`HandleFamily`; its switch lists every tag, so a new tag carrying a handle
cannot be missed. A synchronization wait site is not a handle: it identifies a
park within its own body only.

A unit signal that is not `scoped` is storage the compiler adds to the unit
without declaring it there, such as a constant table: it is not listed in the
instance's scope, so the VCD dump does not show it.

### Imports

An import names its target by **unit-relative name**, never by the target
unit's local handle, so a unit's IR does not change when an unrelated part of
another unit does, and units can be lowered in any order. A unit's members, by
name, are what it defines plus its bound ports; its own extern declarations are
not members. A name two members share cannot be imported.

| Import | Target |
|---|---|
| `Port(k)` | Whatever the instantiating parent binds to this unit's bound port `k`: a signal for an inout, ref or merged-nettype port, or for a contribution to one (below). |
| `Member(ref, name)` | The member `name` of the instance `ref` reaches: a child's port read by a port copy, a downward or upward hierarchical reference, a package item, a package's covergroup type, a hierarchical task call. |
| `Representative(unit, name)` | The member `name` of any instance of `unit`. Supplies layout only, for a member reached through a virtual interface handle. |

An `InstanceRef` is a base followed by a path of child slots. The base is the
importing instance itself or an ancestor `n` levels up (`Up(n)`), a root
(`Root(i)`: a top, a package or a class unit), or the instance bound to one of
the unit's interface ports (`Port(k)`).

A `ChildSlot` records the child's unit, its instance name and the binding for
each of the child's bound ports, as a handle in the **parent's** space (a
signal, or an `inst_refs` entry for an interface port). Bindings belong to the
parent unit, not to an instance, so the instance tree is fully determined by the
roots and the units' child slots; an `Instance` adds only its name.

Upward references are relative (`Up(n)`), which is what lets a body that
contains one be shared once its imports are keyed on what they resolve to.
Until then such bodies, like the other bodies slang will not share, get a unit
of their own.

A `ChildSlot` also records how many of the parent's processes precede the child
(`process_pos`), so flatten emits processes in the body's source order rather
than all of a parent's before its children's.

### Contributions

A driver through an inout port stores into a **contribution**: a bound port the
driving unit declares for that driver (`io$drv0`, `io$drv1`, ... per port, in
body order), so the body is the same whether the net has one driver or many.
Contributions are part of the unit's interface, like the port itself. The store
is full width (a partial driver carries z in the bits it does not drive), and a
contribution overlaps every other driver of its net, so a net with one is
resolved as soon as it has a second.

The instantiating parent binds each contribution. A parent that reaches the
net through a port of its own re-exports the contribution as one of its own, so
nothing names a grandchild's. Otherwise the parent is the net's owner (or
reaches it by a hierarchical name) and binds it to the net itself when this is
the net's only driver, so the store lands in the net with no run-time cost, and
to a fresh driver slot of the net's owner when the net resolves; the resolver
is the owner's too. The owner's IR therefore depends on its own drivers and its
children's interfaces, never on a descendant's body. `flatten` resolves the
chain of `Port` imports to the net or the slot, so the flat module is the one
a direct store or a slot owned by the driver gave.

### Net types across ports

LRM 23.3.3.7 lets the nets on both sides of a port "take on the same type", and
Table 23-1 gives which. How Clotho reads it depends on the direction.

**Inout.** The port merges the two nets (both symbols share one signal), and the
merged **simulated net** has the type the table gives for the pair (the
external net's unless the table says internal) and the dominating net's delay.
The simulated net is a fact of the net's owner, the symbol that declared the
signal: its resolution function, the pull of a tri0/tri1 net, the strength of a
supply net and its delay are all decided there, once every port is met
(`finalize_sim_nets`), and no driver's own net type is consulted. A child never
reads it, because its drivers store into contributions the owner binds, so
instances of one specialization stay one unit however differently typed the nets
they connect to are.

`NetType::getSimulatedNetType` is the table; it agrees with all 81 cells of
Table 23-1, type and `warn` flag, as printed in the LRM. The clause leaves some
readings open, decided in `collapse_port_net`:

- A net reaches a leaf through a chain of ports, and each is one pairwise
  collapse whose external net is the net the ports above it have made: the
  table is applied outside-in with the type so far as the external type. Ports
  are met in elaboration order, an instance's before its children's and
  siblings in declaration order.
- A pair where neither type dominates (wand against wor, tri0 against tri1, a
  trireg against a wand) goes to "external" and is a `warn` cell, so a result
  that depends on the order is always reported. slang reports each port
  connection whose declared types make a `warn` cell (`net-inconsistent`); a
  pair that is one only for the type the net has taken on is reported by Clotho
  (`net-type-conflict`). iverilog resolves such conflicts between siblings in
  favor of the later port, which the LRM does not require.
- A uwire never reaches an inout collapse (slang rejects it on either side). An
  interconnect net merges with the other net's type (23.3.3.7.1), but Clotho
  models none, so one that declares a signal fails at its declaration.
- A simulated net of a type Clotho does not model (a trireg that dominates)
  fails loudly, and so does a net delay on a collapsed net that dominates its
  owner, which would need the declaring module's time unit.

**Input and output.** 23.3.3 makes the port "a continuous assignment from source
to sink", and that is what Clotho lowers: the sink net keeps its own type and
resolves its drivers, the port copy among them, as that type says. 23.3.3.7 only
calls merging the two nets "permissible", so a design that connects dissimilar
nets through an input or output port is conforming without it, and where the LRM
allows both behaviors Clotho takes the cheaper. What the LRM attaches to not
merging is warnings: Table 23-1's `warn` cells are slang's, for a port of any
direction, and 23.3.3.6 asks for one when a uwire is on either side of a port
whose nets are not merged (`uwire-port-not-merged`).

## Inbound facts and summaries

Each unit gets a summary computed from its body alone: per port and exported
signal, whether it is read, written, waited on, forced or may carry z (and how
z propagates from input to output ports); the combinational dependencies from
input to output ports; the runtime features it uses; and its calls to imports.
The link step solves the cross-unit fixpoints over the instance graph using only
summaries, and hands each unit the joined facts its instances impose on it.

Decisions that today change IR in other modules at lowering time move to
codegen, keyed on those facts: `forceable_` writer gating, nets resolved with
drivers outside the unit, `events_merge_`, `WaitedProps` notifier fields in
class layouts, and `assert_control_used_`. The IR then stays a function of the
unit, which is also what the no-IR-rewriting rule asks
(synthesis-and-lsp-scoping.md §2).

## Code generation

- Unit code is instance-relative: it addresses storage as instance base plus
  offset, never through per-instance symbols, singleton units included, so a
  unit's code does not change when it gains a second instance. The
  constant-anchor form measured faster under FastISel on picorv32
  (module-split.md); recovering that belongs to a later whole-design `-O` tier.
- No inlining across units. Processes fuse within a unit, and a unit's group is
  one function taking the instance's record; the groups of several instances
  that wait on the same events are the rows -- (unit function, instance
  record) pairs -- of one schedule group, run by one dispatcher, with no
  member-count caps. Merged cross-instance bodies can return in a flattening,
  LTO-style `-O` mode without changing units.
- Function and task signatures are uniform: the process-control and home
  parameters are always present and `ref` epoch handling never depends on
  callers, so no other unit's code changes a unit's signatures.
- Root holds link glue only: instance storage and records, binding tables, the
  scheduling plan and global numbering as tables.

### The instance record

Every instance of a unit, a static unit's single instance included, has a
record the unit's code receives as its first argument. It holds what differs
between instances:

- bases of the instance's state, NBA shadow and scheduling flags;
- bases of its ids in design-wide spaces (VCD variables, NBA blocks and
  buffers, triggers, RNG seeds), so code adds a unit-local ordinal;
- its hierarchical name (`%m`, banners, covergroup scope), held as a borrowed
  string handle: `inst_path` reads it at no cost and nothing owns the value, so
  consumers read it in place (a display argument, a banner piece the runtime
  joins when it reports, a concatenation part) and copy only where they need a
  string of their own;
- its deferred-assertion generation counters, one per process it owns: a group
  bumps its counter at each activation and tags the reports it queues with it
  (LRM 16.4.2), so the counter is a field of the instance, not a global of the
  group;
- the design-wide index of each process it owns (its control block, its random
  stream's seed) and the index of its program instance (LRM 24.3, `$exit`);
- the flat ids of the `disable` targets and covergroup types it declares, which a
  unit's code names by position (a hierarchical `disable` of another instance's
  block, or a covergroup of one, still names a symbol of that instance);
- one binding per extern declaration: the address of an imported signal and the
  record of the instance that owns it, and the record of an imported function's
  instance (landed); the record of an instance an interface port or `inst_refs`
  entry names (not yet);
- descriptors of wake buckets the instance's signals own (base, subscriber
  count and trigger bits), and the trigger masks of the guarded groups it runs.

Today's per-definition `@clotho.inst.<hier>.ctx` (StateLayout.cpp) is the
seed of it; the `abs_id` / `idval` relocations (gone: ids are record fields) and constant anchors in unit
code go.

## Staging

1. **IR containers and the flatten adapter.** `Design`, `Unit`, imports and
   child slots, plus `flatten(Design) -> Module`, which expands units into
   instances and reproduces the flat module the backend consumes today. The
   adapter is per-instance only on the backend side, as the backend already is;
   in step 3 it stops copying bodies and keeps only the instance expansion.
2. **Lowering per specialization**, in four sub-steps that each keep the suite
   and the perf corpus simulating identically:
   - **2a, partition (landed).** The Lowerer records which unit owns what
     (`UnitOwners`), and `partition` (src/ir/Partition.h) splits the flat module
     it lowers into units: one per instance, one per package and `$unit`, and a
     glue unit for what belongs to no unit yet. Every reference across units
     becomes an import by name. `flatten(partition(m))` reproduces `m` up to
     numbering and process order within a body.
   - **2b, merge (landed).** partition gives the instances of one
     specialization (slang's canonical body, `note_specialization`) one unit
     wherever their whole subtrees lowered to identical unit IR, and keeps the
     rest apart, so the merge is correct by construction whatever leaks remain.
     `--stat=units` reports the specializations still split and their first
     difference. On rocket-chip, 558 instances become 266 units with none
     split; the leaks below show up in constructs RTL like it does not use.
   - **2c, instance-independent bodies.** Remove the leaks below until no
     specialization stays split. Landed: a subroutine's static locals and a
     class's static properties belong to the instance whose body declares them
     (`Lowerer::home_of`), whichever instance's code reaches them first
     (`a.f()` from the parent), named under that instance; their declaration
     initializers close into a PreInit process of it rather than of the
     caller. A body reached across tops keeps them with the caller. An interface
     instance declares the statics of its subroutines with its other signals,
     and runs their initializers in its own PreInit segment, whether or not
     anything called them there: a virtual interface handle runs one instance's
     body against any instance's record, so the instances of one interface
     have to agree on layout and on start values (`declare_interface_statics`).
   - **2d, lower once.** The Lowerer lowers each specialization's canonical
     body only; per-instance lowering and the comparison go.
3. **Backend per unit.** Code is a function of (unit, inbound facts, config);
   root becomes link glue. Gate: an edit to one definition changes only that
   unit's object bytes and root's, checked by compiling a design twice with one
   definition edited and comparing per-module bitcode. Sub-steps, each keeping
   the suite and corpus simulating identically and measured for speed:
   - **3a, one layout per unit.** flatten records which unit instance every
     flat signal, process and function came from (`Module::unit_instances`).
     Slot facts (may-z and the other z-storage causes, NBA staging flags) are
     joined over a unit's instances, so all of them share one state and shadow
     type; power-on values stay per instance. A unit's storage the scope does
     not list (driver slots, constant tables, assertion state) moves from the
     root struct into the instance's.
   - **3b, instance records.** Every unit instance gets a record; unit code
     addresses its own storage, ids and name through it and other units'
     storage and instances through its bindings. Landed so far: every
     instance scope has a record, and a fusion group addresses its own
     storage, shadow, sched flags, wait buckets, dump-var index and NBA block
     ids through it (block ids at the record's field for the function's
     def-local process ordinal). A signal outside the instance (another
     instance's, a package's) is reached through a binding: two record fields
     per extern declaration of the unit (`InstanceScope::externs`, in the unit's
     order), the address of the signal's storage and the record of the instance
     that owns it, filled at startup. A write through one wakes the owner's
     readers from the owner's record (buckets, trigger bits, shadow, dump-var
     index), exactly as an access through a virtual interface handle does, so
     what it needs of the owner is data and not a symbol. Two declarations of
     one instance can name one signal (an inout port and the contribution a
     child drives through it); the first stands for both, so instances whose
     declarations alias differently do not share code. The ids of its signals in
     design-wide spaces (array, aggregate, container, string and intra NBA
     buffers, `<= @e` triggers) are record fields at def-local ordinals, filled
     at startup, and the sets they index are joined over a unit's instances;
     the dump-var base was a field already. A signal of no instance keeps its
     immediates. The name (`inst_path`) is a record field, the address of the
     instance's borrowed path handle (a constant of root), so code of a unit
     reads its instance's name and bakes none.
   - **3c, code once per unit.** Each unit's processes and functions are
     lowered once from the unit's IR, taking the record, into the unit's module.
     Fusion runs within a unit; groups spanning instances become dispatch
     table rows. `share_twin_groups` and outlining go. Landed: fusion groups
     are formed per unit (`compute_fusion_groups`,
     ir/llvm/Groups.cpp). A group is a list of def-local process ordinals,
     the same in every instance and never mixing instances, decided on the
     unit's processes and facts joined over its instances, never on an
     instance's context. A thunk group is a function taking its instance's
     record, a row; the rows of the groups that wait on the same (signal,
     edge) set, in any instances, share one subscription as a schedule group,
     whose entry (a dispatcher in root, or in the unit module for one row)
     owns the flag, runs the rows and re-arms. Each group is lowered once, from
     the first instance whose imports are alike (`code_key`: which bindings
     alias, and the owner and position of what each names, except an event
     port, which is only its declaration), and every other
     instance gets a row of that function with its own record, plus NBA blocks
     of its own signals; a coroutine ramp takes the record as its first argument
     too. `merge_twin_bodies`, `share_twin_groups` and
     `SimulationConfig::outline` are gone. A lowering that bakes something of the
     instance it ran for (a symbol of its storage or wait buckets, a call of a
     function that is its instance's own, a disable target or covergroup type of
     another instance) taints itself and each
     instance lowers its own; `--stat=unit-code` reports unit groups, functions
     emitted and what stopped sharing. Comb fusion stays within one instance
     (combchain regresses; accepted until fusion across instances is revisited).

     A function or task of an instance takes that instance's record as its
     first argument and addresses the instance through it, as a group does:
     its own storage, shadow, ids, wake buckets and dump-var index from the
     record, another instance's signals through bindings. One that belongs to
     no instance (a package's, `$unit`'s, a virtual interface body) takes the
     root state, which nothing reads, and addresses absolutely. A direct call
     passes the callee's record: the caller's own when it is the same
     instance, else the one its function binding holds (one record field per
     extern function declaration of the unit, `InstanceScope::fn_externs`,
     filled at startup like a signal binding), so a call into another instance
     names a function and never a record. A virtual call cannot name it, so a
     vtable entry of an instance's method is a root thunk of the same signature
     that does, and a `$strobe` / reactive body, a fork child, an exported
     subroutine and the `$monitor` body (`rt_monitor_set` takes it with the
     ramp) carry theirs. A subroutine that stages a nonblocking write registers its
     blocks like a group: the first block id is a record field at the subroutine's
     ordinal in its unit, and the instances that share it register the same blocks
     for their own signals (a signal of another instance through the binding that
     names it).

     The functions and tasks of a unit are lowered once too, into the unit's
     module (`lower_subroutines`, ir/llvm/Families.cpp). The ones of the
     instances of a unit that lower alike are a family: the first to be lowered
     is its function, and the others are that function, each called with its
     own instance's record. What decides a family is its key
     (`subroutine_key`): the unit and the function's ordinal in it
     (`InstanceScope::fn_locals`), the code key of the instance (above, and
     which function bindings alias), what the design says of the function
     rather than of its body (the home region its parks answer to, the random
     stream it owns, the program a `$exit` in it ends), and the functions it
     calls as declared, each with whether it may finish. Callers are lowered
     after their callees (the order of the call graph's strongly connected
     components), so a key names its callees as they were settled: instances
     whose calls land in one function match, instances whose callee was lowered
     apart do not. A cycle of subroutines (a strongly connected component of the
     call graph) is one family: its key is its members' keys in the unit's order,
     each naming the members it calls by their place in the cycle, and the cycle is
     lowered once, all of it or none (a taint in any member keeps every member
     per instance); a cycle with a member of another instance is not shared. A group's key
     names the functions its processes call the same way (`group_key`), which
     is what lets a group that calls a function of its instance, or of the
     instance a binding names, share like any other; a call of a function that
     stayed one instance's own taints the group. A function that is not
     shared is lowered as it was, into its unit's module like every other, and
     `--stat=unit-code` counts the subroutines of multi-instance units, the
     functions emitted for them and what kept each per instance: a
     disable target or covergroup of another instance, a call of such a
     subroutine. The instance a process, a control block, a random
     stream or a `$exit` names is a record field (process indices, the program
     instance) read by the instance's own code, so none of them splits a family; a
     `$exit`'s relation to the subroutine's own instance is in its key. Measured:
     rocket-chip has no function or task in a multi-instance unit (0 of 0; its
     1021 groups of multi-instance units are as many functions as before), so
     the suite's twin designs are what covers shared subroutines; the perf
     corpus runs the same instructions as before, picorv32 1.2% fewer, since
     its testbench's tasks now sit in the module of the code that calls them
     and LLVM inlines them there.

     An event object is instance data reached through its variable: a declared
     event powers on holding the handle of its own object (a slot of
     `clotho.events`, numbered design-wide in signal order, handles being
     design-wide), so code reads the slot from the variable's storage -- the
     instance's own through its record, an event port's through its binding --
     and indexes `clotho.events` with it, as it already did for an event that is
     reassigned. A variable no store reaches (`event_mono_`) is the declared
     object for the whole run, which is what lets waits link without reading the
     handle a second time and unlink without recording the owner; the fact is
     joined over a unit's instances, and over what each instance binds an event
     port to, since the code is shared. The object's VCD index is a field of the
     object and a trigger id of an event is its slot, so neither is code. The
     dump of a reassigned event names its declared object by number, and so does
     every other `$dumpvars` registration: `$dumpvars` in a body is a call of one
     function of root (`vcd_register_fn`) that walks the design's scope tree, the
     link glue that owns the design-wide numbering, so no unit's code names a
     dumped signal of any instance. An event of no instance (a package's) is one
     object for everyone and keeps its constant address.

     The taint list is a claim that the lowering of one instance is the lowering
     of all, and the frontend's equivalent (partition merging) compares, so the
     backend is checked too: `SimulationConfig::verify_shared_code`, on with
     `CLOTHO_EXTRA_ASSERTS` (Debug builds and CI; the suite's build passes it,
     so the whole suite runs through it) and off otherwise,
     lowers every group, function and task again for each instance that shares
     it, into a scratch module (a subroutine into a declaration of its own),
     and compares it with the shared code. What is compared is
     the printed code with the function's name, the names and contents of
     private constants, the names and bodies of struct types and the numbering
     of attribute groups normalized (`CodeText`, ir/llvm/CodeText.h), and for a
     group the NBA blocks and the record fills the sharing instance gets
     against the ones its own lowering registers. A difference, or an own
     lowering that taints, fails the build and names the unit, the group or
     subroutine, both instances and the first line that differs; the emitted
     modules are the same with the check on and off. A difference is a bake the
     taint list missed: move the fact into the instance record, or taint the
     lowering if it is genuinely per-instance (and, for a subroutine, a fact
     of how the design uses it belongs in its key).
     It found one miss: `$finish` emptied the active ring only when an earlier
     body had already made it, so two instances lowered either side of that
     body were different code (and a `$finish` in a function, lowered before
     any body, left what it had woken running). `--stat=unit-code` reports how
     many groups and subroutines were checked and what it cost.
   - **3d, wake and NBA as data.** A writer reads its wake buckets' base and
     capacity from the record, a subscription carries its trigger bits, the
     ring mask is a root global, NBA and trigger ids are record base plus
     unit-local ordinal, and block commit functions live in the writer's unit.
     Facts other units impose (a signal is waited on, forced, may carry z)
     stay joined per unit and part of its codegen key until step 4. Landed so
     far: an owner's scan reads its buckets' subscriber count and trigger bits
     from the record, and trigger colours (design-global, first-fit) are data
     wherever unit code needs them, never an immediate: a guarded group's
     member and group masks are record fields filled at startup, a writer of
     another instance's signal reads that instance's record, a signal of no
     instance has a trigger global per bucket, and the NBA meta table carries
     each committed signal's bits. What a park or self-start stores into the
     scheduled flag is every trigger bit whether or not the design guards a
     group, so no unit's code depends on that.
  - **3e, root as link glue (partly landed).** Landed: one LLVM module per
    Design unit, named by the unit's key (`InstanceScope::unit_key`: the
    definition name, plus a body hash when several units share it) and holding
    only that unit's code under unit-key symbols; instance storage, records, the
    per-instance trampoline of a one-row schedule group (the entry itself is
    unit code taking the record), dispatchers, the shared queues and the wheel's
    trampolines are defined in root whichever unit asked first; a unit's
    declarations of root globals carry no size. Gate (tests/modular_gate_test.cpp,
    `ir::emit_llvm_modules`, `clotho build --emit llvm-units`): compiling a
    design of several modules with several instances each, and a package,
    twice with one definition's body edited changes that unit's module and no
    other unit's; every unit module names no instance. Still open: NBA block
    commit functions live in root (block ids are already record base plus
    ordinal; the commit function would take a block descriptor and sit in the
    writer's unit), the seed loop and record fills are root code rather than
    tables built from the instance table, and flatten still copies bodies per
    instance. What still makes a unit module depend on outside it: lowerings the
    taint list keeps per instance (they name `clotho.inst.<hier>.*` globals),
    `clotho.expect<monitor>` queue names, and the intern pool's type and class
    numbers (step 5).
4. **Summaries and the link step** replace the whole-design `analyze()`.
5. **Caches and parallelism.** Per-unit intern pools (cross-unit types match by
   structure, nominal types by unit and name), content keys, an object cache,
   an IR unit cache, parallel lowering and codegen, and per-unit objects handed
   straight to lld by `clotho build`.

## Known leaks step 2 removes

Each of these makes lowered IR depend on something outside the unit:

- Signal, process, function, scope, monitor, cover-type and `disable` target
  ids minted in design-wide tables (partition renumbers them per unit).
- A package or `$unit` class lowered once per Lowerer that reaches it, so its
  synthesized functions exist once per top and take an ordinal in their import
  name (`own_function`).
- Resolvers with no scope for nets driven from several scopes other than through
  inout ports (hierarchical drivers, interface nets). A Preponed process belongs
  to the scope that holds the variable it samples (its shadow's scope), so an
  instance's sampling state is its own; only a package variable's shadow is
  copied by a process of the top, which is never shared.
- A top's own PreInit processes hold what is no child's: what Collect and
  Classify append (constant-table seeds, a function static of the instance
  being lowered that a continuous driver reaches first) and what follows the
  walk (the statics of the top's own classes, `$past` seeds). They store into
  children's state by import; a root is never shared, so no specialization
  splits on it, but the top's unit depends on its children's internals. Every
  other declaration initializer is a PreInit process of the instance that
  declares it, closed at each child boundary so the processes run in
  elaboration order.
- An instance's name in `%m` strings, severity banners, `$printtimescale` and
  covergroup instance names is read through `inst_path` (flat bridge: resolved
  from `Process::scope` / `Function::scope`; a covergroup takes it as an operand
  of `CoverOp::New` with the text after it on the op, so `CoverType` carries no
  scope; a declaration initializer reads it like any other body), hidden
  temporaries are numbered per name prefix, and assertion state is named by an
  instance-relative ordinal. `inst_path` is a borrowed string, a read-only
  handle over constant bytes whose address is a field of the instance's record,
  so evaluating it takes no temporary and no init or release, and a severity report or a
  covergroup name keeps it apart from the text around it (`SeverityInfo`:
  banner, path, tail; `CoverInfo::text`) for the runtime to join when the line
  is issued or the name is made, so a report that does not fire costs its body
  nothing. Still text: virtual-interface bodies and `$printtimescale(inst)`. A
  covergroup type belongs to the unit that declares it and instances build it
  through an import, so a package's type built by two instances no longer
  aborts partition. Still lowered once per Lowerer that reaches it (a second top
  gets an ordinal in its import name, like a package class), and the
  coverage-event process of a package type stays with the first instance that
  builds it, which splits that specialization.
- Whole-top facts that change other modules' IR: `forceable_`, resolved nets,
  `events_merge_`, `WaitedProps`, `assert_control_used_`, and which blocks are
  `disable` targets. The last is joined over the instances of a specialization
  by `join_disable_targets` (a block some `disable` names from outside its
  instance is a target in every instance, each with its own target id), for the
  top being lowered only; it moves to a codegen-time summary with the others.
- Design-wide registries: class ids, interface-class ids and slot numbering,
  the virtual interface registry, and the time-precision GCD baked into every
  delay and `$time` as a multiplier.

Two questions step 2 must answer before a unit holding either is instantiated
twice (flatten refuses both today, by assertion):

- A class declared in a module body. slang gives twin instances one canonical
  body and so one class type, where today each instance lowers its own class
  with its own descriptor and static properties. Which reading the LRM requires
  is to be checked against its text before the class moves into the unit.
- An `export "DPI-C"` in a module body: one C symbol per instance would collide.
