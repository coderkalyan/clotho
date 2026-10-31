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
  object, with a finalizer for any native resource it owns. **Not yet:** events
  are never freed.
- The collector's design (heap organization, roots, generations, barriers,
  triggers, safepoints) is open for discussion.

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
