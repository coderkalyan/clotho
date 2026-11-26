# Architecture decisions (tier 2)

The maintainer's decisions for tier 2 (`docs/PRINCIPLES.md`, review tiers): the
IR, analyses and the compilation pipeline, memory management, containers and
classes. They are normative, like `docs/design/kernel.md` for tier 1. Code that
disagrees is drift unless the decision here is changed first.

**Not yet** marks a decision the code does not follow today; delete the marker
when it does.

One rule applies throughout: a fact has one source of truth. Two pieces of state
that must be kept in agreement are a design smell, even when an assertion keeps
them in step.

## IR structure

- Control flow is structured regions, each ending in an explicit terminator or
  yield operation that also carries the values the region produces. **Not yet:**
  early exits end a region by convention and joins go through memory.
- A body's instructions are parallel arrays, never heap nodes linked by pointers.
- An instruction has a small fixed payload; larger data lives in per-opcode
  records in one word buffer per body.
- Adding an opcode must never silently miscompile: switches over opcodes are
  exhaustive, with no default arms. **Not yet:** backend scans use default arms
  and one of the two operand-walk APIs is not exhaustive.
- One opcode per distinct operation, in an 8-bit field.
- Mutable variables and sub-objects are places: a root and a chain of selects,
  read and written by explicit loads and stores, with no phis.
- Processes have distinct kinds (combinational, clocked, initial, final) with
  typed sensitivity; a general body is used only where no kind fits. **Not yet:**
  waits are instructions and a sensitivity-list process loops implicitly.
- Bodies, foreign imports and compiler-synthesized deferred bodies have separate
  tables and ids (tentative). **Not yet:** one record type for every callable.
- Every body, processes included, has a frame; design-level variables hold only
  static storage. **Not yet:** process locals and temporaries are design-level
  variables.
- The IR describes the design per definition. **Not yet:** per instance (a
  refactor is in progress).
- Types and constants live in a mutable pool during lowering, frozen read-only
  for code generation. **Not yet:** one append-only pool with one untyped handle
  space.
- The IR is frozen after construction; analyses add side tables, and LLVM's
  optimizer does the simplifying.
- Structural invariants (operands defined before use and in scope, regions
  closed, terminators last, payload matching the opcode) are checked by a
  verifier after each body is sealed, in debug builds, aborting on the first
  failure. **Not yet:** the verifier checks typing only.
- The IR has a machine-readable serialization generated from the opcode
  descriptions; the readable dump is a view of it. **Not yet:** a readable dump
  only.

## Analyses and pipeline

- One whole-design analysis computes every derived fact; the code generator does
  not re-scan the IR. **Not yet:** facts are split between that analysis, scans
  inside the code generator and frontend AST pre-scans.
- Compile-time value reasoning uses purpose-built analyses (the may-z fixpoint, a
  separate evaluator for folding with pinned inputs).
- Identical instances do not arise: one body is lowered per definition. **Not
  yet:** detected after lowering by comparing printed code.
- Compilation streams per definition: each is lowered, emitted and compiled as
  soon as it is complete. **Not yet:** serial up to finished modules.
- Compiled code is cached at the per-definition IR level. **Not yet:** no cache.
- A body's code depends on design-wide facts only through an explicit list of
  inputs that is part of its cache key. **Not yet:** bodies depend on waiter
  fan-out, feature gates, may-finish tests, source text, fusion groups and reuse
  counts.
- Optimization effort is chosen for the whole design for now.
- `build` compiles each unit to its own object in parallel, as `run` does, then
  links them. **Not yet:** it links one module and compiles it serially.
- The frontend alone decides legality and reports refusals; the code generator
  only asserts. **Not yet:** the code generator re-checks and stops at its first
  refusal.
- A code-shape switch is removed once its shape is stable; only shipped shapes
  are tested. **Not yet:** switches remain, some never compared against their
  switched-off shape.
- Settings are one struct: what changes generated code is fixed at build time,
  and a run-time subset is handed to the runtime at start.
- Unit granularity, the contents of the always-rebuilt module and how unit code
  addresses instance state are decided by the per-definition refactor.

## Memory

- Class objects are reclaimed by a purpose-built tracing collector.
- The collector owns class objects only; string and container buffers have one
  owner that frees them, and the collector traces through them.
- Every built-in handle kind (fork capture boxes, semaphores, mailboxes, process
  objects, events, covergroup instances) follows one discipline: a collected
  object, with a finalizer for any native resource it owns. **Not yet:**
  covergroup instances live until the run ends.
- A covergroup instance's results outlive it: its finalizer folds them into the
  type's record, an instance with `option.per_instance` set keeps its own record
  as well (LRM 19.7 requires that one in the report), and an instance with its own
  sampling event stays reachable while that event can fire.
- Collection is generational: an object is old once a collection has marked it.
  That bit is the only generation state; it is kept beside the heap, not in the
  object, and a full collection clears it first.
- Objects do not move, but only what the collector scans may rely on that: an
  object's address is held in object fields, design state, frames and the stack
  and nowhere else, and identity and ordering never use it, so that moving can be
  added. Where a handle must stand for its object outside those places (the bytes
  of an associative-array key, formatted output) it is the object's identity
  number, assigned on first request and kept in a table beside the heap. **Not
  yet:** the runtime's own records of an event (a `->> #d` timer, a `->> @(x)`
  subscription, a dump variable, the table of events that are a control) hold its
  address where the collector reads it but could not update it.
- Suspended frames and the native stack are scanned conservatively until frames
  have explicit stack maps; object fields and design state are scanned precisely.
- A young collection costs what happened since the last collection (allocation,
  logged writes, frames that ran), never the size of the design. **Not yet:**
  each one reads every state root, walks every state-resident container and scans
  every registered frame.
- An object is finalized only if it owns a buffer outside the heap when it dies.
  **Not yet:** every dead object with a string or container property runs a
  finalizer.
- A handle stored through a `ref` argument is recorded by a barrier in the
  callee, which finds the owning object from the address. **Not yet:** the call
  site records the owner, and every collection re-records whatever frames
  reference.
- Any process may run on any worker thread, but two run at the same time only
  when what they may access does not overlap with a write, and a run is
  reproducible. So values and containers need no protection against concurrent
  access, and their buffers stay with their one owner. **Not yet:** one thread
  runs every process.
- An object's metadata is found from its address in constant time: chunks are
  aligned and carry their own metadata, and a table of chunks says whether an
  address is in the heap.
- Free space is reclaimed at the 16-byte granule, with one metadata byte per
  granule.
- An object has no collector header: its flags are in its granule's byte, and
  objects are 16-byte aligned.
- An object too large for a block has its own mapping, found through the same
  table. A dead one's mapping is kept for the next object of its size.
- After a full collection, empty chunks and kept mappings beyond what the last
  cycle allocated go back to the system.
- A young collection runs after a fixed amount of allocation (8 MiB, counting
  the buffers objects own).
- A full collection runs when the old generation has grown by a fixed factor
  since the last one: what a collection has marked and the buffers it owns, never
  the committed heap, which includes the young budget. The factor is a constant of
  the build: a collector tuning value becomes a user setting only once a
  measurement shows it must.
- A collection runs at the next boundary between process slices once the budget
  is spent, and inside an allocation only past an overshoot or at the heap's
  ceiling. **Not yet:** the boundary is the end of a time step.
- The heap may be given a ceiling. Reaching it collects everything and, if that
  is not enough, ends the run with a located fatal that lists live bytes by
  class. **Not yet:** the fatal has no such list.
- Allocation blocks and the buffer of logged objects belong to a worker.
  Suspended frames come from an allocator the collector can walk, each with a
  flag set when it resumes. **Not yet:** one global allocation state, and frames
  that register themselves in a list.
- The metadata is laid out so that stopped workers can mark in parallel; the
  marker itself stays single-threaded until a workload needs more.
- Finalizers run in the collection that finds their object dead.
- `std::weak_reference` is weak by its class, not by a mark on the pointer: the
  class's trace map leaves the referent out, and after marking the collector
  clears the referent of each weak reference it reached whose referent is
  unmarked. Collection timing is then visible to a design, so a collection
  happens at a point that is a function of the run, never of thread timing.
- Weak references are cleared only by a collection at the scheduler's safepoint. A
  collection run from inside an allocation keeps every referent, so a design sees
  a weak reference change only between time steps, and a wait on `get()` is woken
  at the safepoint, before the next time step runs.
- The scheduler edge that finds the wheel idle ends the run and is not a
  safepoint.
- An object's identity number (`weak_reference::get_id`, the order of an
  associative array keyed by a class handle) is assigned on first request and
  kept in a table beside the heap, so objects that never ask pay nothing; the
  pass that clears weak references drops a dead object's entry.
- The collector is checked three ways: each design in the collector's tests runs
  at three collection cadences (never, ordinary, stress) on one code path and its
  output must not change; a heap verifier runs after each collection in
  extra-assertion builds; and a model-based test drives the collector's C
  interface with random graphs, stores and collections against a shadow
  reachability model. A design in which the collector can clear a weak reference
  states its output for each cadence instead.

## Containers and classes

- A `ref` into relocatable storage (a container element) is a fat record holding
  the indirection and any offsets, listed in its container, which repoints its
  records when it relocates; other refs cost nothing. **Not yet:** containers keep
  a relocation counter that refs re-check on access.
- How value-semantics copies are implemented (copy-on-write, eager copies, copy
  elision) is open until data on how designs copy containers is available.
- Queues and dynamic arrays are growable ring buffers; a middle insert or delete
  shifts the shorter side. **Not yet:** dynamic arrays are exact-size and a queue
  edit always shifts the later elements.
- Operations that need no memory management are compiled inline; the rest call
  the runtime. Generated code stays lean, not over-inlined. **Not yet:** every
  container and string operation except element access and `size()` is a runtime
  call.
- One container engine is written once in the runtime; per-element-type behaviour
  is generated code reached by direct calls or inlined (mechanism being
  prototyped). **Not yet:** the runtime interprets a layout descriptor.
- Associative arrays are ordered, cache-friendly B-trees; hash indexing is added
  only if measured to be worth it. **Not yet:** a hash map with a sorted index
  rebuilt after each insert or delete.
- An associative array's map is specialized per key type. **Not yet:** every key
  is serialized to bytes.
- Array methods with a per-element expression (sort with a key, find, unique,
  reductions with a clause) call a runtime routine with a generated per-element
  callback and an environment record. **Not yet:** expanded by the frontend into
  primitive IR.
- Virtual calls dispatch through a per-class method table. A per-call-site
  inline cache is deferred until benchmarked (`docs/TODO.md`).
- A wait on a class property is woken by one notifier per object. **Not yet:**
  hidden per-property notifier fields.
