# Associative-array keys: every legal index type

**Status:** scoping. Nothing here is implemented; the phases at the end are the
proposal. The current model is `dynamic-arrays.md` §4a, which this supersedes
once (if) the phases land.

Goal: make every index type LRM 7.8.5 declares legal actually work, instead of
the six-kind subset clotho admits today. The blocking case in the conformance
suite is an unpacked struct with a queue member, which is nearly the LRM's own
example.

## 1. What the LRM requires

7.8: "the index expression is not restricted to integral expressions, but can be
of any type"; "the data type to be used as an index serves as the lookup key and
**imposes an ordering**".

7.8.5, in full, because every design decision below hangs off one of these four
bullets:

- "Declared indices shall have the **equality operator defined** for its type to
  be legal. This includes **all of the dynamically sized types** as legal index
  types. However, real or shortreal data types, or a type containing a real or
  shortreal, shall be an **illegal** index type."
- "An index expression that is or contains **x or z** in any of its elements is
  **invalid**."
- "An index expression that is or contains an **empty value or null** for any of
  its elements does **not** make the index invalid."
- "**If the relational operator is defined** for the index type, the ordering is
  as defined in the preceding clauses. **If not, the relative ordering of any two
  entries can vary, even between successive runs of the same tool.** However, the
  relative ordering shall remain the same within the same simulation run while no
  indices have been added or deleted."

Its example is the shape we currently refuse:

```systemverilog
typedef struct { byte B; int I[*]; } Unpkt;
int array_name [ Unpkt ];          // struct key containing an ASSOC
```

7.8.6 adds the invalid-index behaviour: a read with an x/z index "shall issue a
warning" and return the nonexistent-entry value; a write with one "shall be
ignored, and a warning shall be issued".

**The fourth bullet is the load-bearing one.** No relational operator is defined
for an unpacked struct, an unpacked array, a queue, a dynamic array or an
associative array, so for every one of those key types clotho may order entries
however it likes, provided the order is stable within a run while the key set is
unchanged. Ordering only has to be *meaningful* for the key types where SV
defines `<`: integral (7.8.1, numeric), string (7.8.2 / 6.16.8, lexicographic),
and — per 7.8.3 — a class handle's ordering is explicitly "deterministic but
arbitrary". That frees the encoding for everything hard.

## 2. What exists today

### Frontend — `src/Lower.cpp`

- Admit list, `lower_type`'s `AssociativeArrayType` arm (`Lower.cpp:1907`):
  `kt.isClass() || kt.isIntegral() || kt.isString() || is_agg_abi_type(kt)`.
  Anything else gets one message naming the admitted set.
- `is_agg_abi_type` (`Lower.cpp:2497`) is `isUnpackedStruct() ||
  as_if<FixedSizeUnpackedArrayType>()` — note it **excludes an unpacked union**,
  which `lower_type` is otherwise happy to lower to `Kind::Struct`.
- `guard_agg_key_leaves` (`Lower.cpp:1739-1766`) walks an aggregate key's leaves
  and admits **integral leaves only**. It emits the two messages the conformance
  suite is hitting: `... with a string member is not supported` and `... with a
  queue / dynamic-array member is not supported`. Its `is_float` arm is dead —
  slang rejects a real-bearing index type first (`index type cannot be or contain
  floating point types`).
- The type pool encodes the key on the `Assoc` type: `t.fields_id` = the key type
  ref, `t.right` = the `[*]` wildcard flag (`Lower.cpp:1890-1921`).
- `lower_assoc_key` is the single frontend entry that lowers a key expression;
  two paths bypass it (`emit_assoc_literal_init` at `Lower.cpp:2936`,
  `const_table_signal` at `Lower.cpp:3749`) — latent, see §7.
- The frontend does **nothing** about x/z indices.

### Backend — `src/ir/LlvmBackend.cpp:13396-13697`

A key crosses the ABI as a normalized span `{const u8* ptr, u32 len}` built
inline by the JIT. Four normalizers:

| key kind | encoding | length |
| --- | --- | --- |
| integral / enum / packed / class handle | big-endian `ceil(W/8)` bytes, MSB flipped when signed (so unsigned `memcmp` reproduces signed `<`) | constant per TYPE |
| `[*]` wildcard | 64-bit unsigned big-endian, truncating | constant (8) |
| string | the raw `rt_string` bytes, aliased (no copy) | **variable per VALUE** |
| unpacked struct / fixed unpacked array | leaf-by-leaf depth-first concatenation, no padding | constant per TYPE (`agg_key_bytes`, a pure type walk) |

The span lives in an **entry-block alloca** sized by that compile-time constant;
the string case is the one variable-length span, and it is deliberately not
composable inside an aggregate — which is exactly what `guard_agg_key_leaves`
enforces.

x/z never reaches the span: only the must plane is serialized, and the may/z
planes fold into a separate `i1` validity predicate carried on `dyn_inrange`
(`AssocKeySpan::valid`).

### Runtime — `src/runtime/Runtime.cpp:2050-2432`, `Runtime.h:1133-1213`

**The runtime is already key-kind-agnostic and already variable-length.**

- `AssocMap` (`Runtime.cpp:2122`) is
  `ankerl::unordered_dense::map<std::string, SlotVec>` plus a lazily built
  `std::vector<const std::string*> sorted`.
- Keys are **owned, per-entry `std::string`** — copied from the caller's span on
  insert. There is no key-width field anywhere on `rt_assoc` or `AssocMap`.
- Ordering is `AssocKeyLess` (`Runtime.cpp:2058`): `memcmp` over the common
  prefix, tie-break **shorter-is-less**.
- Equality is `memcmp`; the hash is over the raw span. No per-key-type ops
  descriptor exists (`rt_elem_ops`, `Runtime.h:897`, has copy/deinit/init for
  VALUES but no compare and no hash).
- `assoc_set_span` (`Runtime.cpp:2192`) hands `k->data(), k->size()` back for
  `first`/`last`/`next`/`prev`.

So the map layer needs **no change** for variable-length keys. Every restriction
is in the backend's fixed-size span construction and the frontend guards that
protect it.

### Support matrix, verified

Each row was run through `build-perf/clotho` on `main`:

| index type | today |
| --- | --- |
| integral 2-state / 4-state, enum, packed | works |
| `[*]` wildcard | works |
| string | works |
| class handle | works |
| unpacked struct of integrals (nested structs / fixed arrays included) | works |
| fixed unpacked array of integrals | works |
| unpacked struct or array containing a **string** | refused (`... with a string member`) |
| unpacked struct or array containing a **queue / dynamic array** | refused (`... with a queue / dynamic-array member`) |
| unpacked struct or array containing an **assoc** | refused (same walk) |
| **queue**, **dynamic array**, **assoc** as the key itself | refused (admit list) |
| **chandle**, **event**, **virtual interface** | refused (admit list) |
| **unpacked union** | refused (admit list, via `is_agg_abi_type`) |
| real / real-containing | rejected by slang — correct, 7.8.5 |

## 3. Why the guards exist: prefix ambiguity

Concatenating variable-length leaves is not injective. With today's "raw bytes,
no padding" aggregate layout, a struct `{string a; string b;}` would serialize

```
'{"ab", "c"}   ->  "abc"
'{"a",  "bc"}  ->  "abc"
```

Two different keys, one span: they would collide into one entry. The same
argument applies to any container leaf (element counts differ, bytes do not).
That is the whole reason strings are non-composable today, and it is the single
technical problem this work has to solve.

## 4. Proposed encoding

**Make the span canonical and prefix-free, and only pay for it where a
variable-length component actually appears.**

1. A key type is classified at lowering time as **fixed** (every leaf is
   integral / handle-shaped) or **variable** (some leaf is a string or a
   container). Fixed keys keep today's encoding byte for byte — no ordering
   change, no perf change, no test churn.
2. Inside a variable key, every variable-length component is emitted as
   `u32 length (big-endian) || bytes`. Length-prefixing is injective, which is
   all that is required: no relational operator is defined for an aggregate,
   queue, dynamic array or assoc key, so 7.8.5's fourth bullet lets the resulting
   order be arbitrary as long as it is stable (it is — the bytes are).
3. A **bare string key** keeps the raw, unprefixed encoding it has today, so
   6.16.8 lexicographic ordering survives where the LRM actually demands it. A
   string *inside* an aggregate is length-prefixed. The two never mix in one
   span.
4. A **queue / dynamic-array** component emits `u32 count || element[0] ||
   element[1] || …`, each element recursively normalized. Element order is the
   array's own order, which is its identity (`==` on queues is element-wise,
   11.4.5 / 7.10).
5. An **assoc** component emits `u32 count || (key, value) pairs in the map's
   sorted-key order`. The sorted index already exists (`AssocMap::sorted_keys`),
   which is what makes this canonical: two equal-content maps must produce
   identical bytes, or key equality breaks.
6. **chandle / event / virtual interface** are handle words — the class-handle
   normalizer verbatim, 8 bytes, no new mechanism.
7. `null` / empty stay legal and encode naturally (a null handle is the zero
   word, an empty string or container is `count = 0`), per 7.8.5's third bullet.

Depth and recursion: the encoder is a type-directed walk, so a key type that
contains a container of structs of strings is just recursion. The one bound worth
enforcing is a nesting-depth cap with a loud failure, since a recursive class
handle inside a key would otherwise be unbounded — handles serialize as their
word, so this cannot actually recurse today, but the cap is cheap insurance.

## 5. Machinery this needs

### Runtime: a key scratch buffer

A variable-length span cannot live in an entry-block alloca. Add a per-process
(per-thread if we ever thread) growable scratch:

```
uint32_t rt_keybuf_begin(void);                                  // returns a mark
void     rt_keybuf_append(const void* bytes, uint32_t len);      // appends
void     rt_keybuf_span(uint32_t mark, const uint8_t** p, uint32_t* n);
void     rt_keybuf_end(uint32_t mark);                           // pops to mark
```

Mark/pop rather than a single buffer because key construction **nests**: building
an assoc-bearing key iterates a map whose own keys are spans. The map copies the
span into its own `std::string` on insert (`Runtime.cpp:2215`), so the scratch
never has to outlive the call — that ownership property already holds and is what
makes this safe.

### Backend

- Split the normalizer into fixed-path (today's alloca, unchanged) and
  variable-path (`rt_keybuf_*`).
- Recursive emitters for string / queue / dynarr / assoc components, including
  the sorted-order walk for an assoc component, which needs a runtime iterator
  callable from JIT code (`rt_assoc_first` / `rt_assoc_next` already exist; they
  hand back spans, so the assoc component's encoding is a loop over those).
- **Denormalization is the expensive half.** `first`/`last`/`next`/`prev` write
  the found key back into a user variable. Today that is a reverse walk over
  fixed-width leaves. With variable leaves it becomes a real deserializer, and
  for a container leaf it must *materialize* a fresh queue / dynarr / assoc
  handle and fill it. This is the single biggest piece of new code in the whole
  scope, and it is only needed for traversal — lookup, `exists`, `delete` and
  element access never decode.

### Frontend

- Widen the admit list to every type with `==` defined, minus real-bearing
  (slang already rejects those): add chandle, event, virtual interface, queue,
  dynamic array, assoc, unpacked union.
- Replace `guard_agg_key_leaves`'s integral-only rule with the fixed/variable
  classification, keeping a loud failure only for a leaf with no value identity.
- Route `emit_assoc_literal_init` and `const_table_signal` through
  `lower_assoc_key` (§7).

## 6. Phases

| phase | content | rough size |
| --- | --- | --- |
| **P0** | chandle / event / virtual-interface keys (handle word, class-handle normalizer verbatim); decide unpacked-union keys (does slang define `==`? if not, keep a guard that says so) | small — admit-list edit + tests |
| **P1** | `rt_keybuf_*` + the fixed/variable split + **string leaves** inside aggregate keys, including denormalization for traversal | medium — this is where the mechanism is built and paid for |
| **P2** | **queue / dynamic-array** components, top-level and nested (count-prefixed), including materializing deserialization for traversal | medium |
| **P3** | **assoc** components (canonical sorted-entry serialization), which completes the LRM 7.8.5 example | medium, mostly the nested-iteration case |
| **P4** | the correctness debts in §7 | small each, but P4a is a silent miscompile |

P1 is the gate: P2 and P3 are new emitters on machinery P1 builds. P0 is
independent and could land tomorrow.

## 7. Correctness debts found while scoping

These are pre-existing, independent of the feature, and two of them are worse
than the missing feature:

- **P4a — associative-array `==` / `!=` is silently wrong.** `is_assoc` is absent
  from the aggregate-equality dispatch (`Lower.cpp:19368`), so a whole-assoc
  compare falls through to the scalar binary path. Verified on `main`: two maps
  with different contents report `eq 1 ne 0`. Should be an element-wise compare
  (or, until then, a loud guard).
- **P4b — LRM 7.8.6 is not implemented.** No warning is ever emitted for an
  invalid (x/z) index on read or write, and `integral_key_valid`
  (`LlvmBackend.cpp:13576`) treats a *literal* x/z key as valid because its
  unknown mask is an `llvm::Constant`. Verified: `a[8'hxx] = 7; a[8'hzz] = 8;`
  creates **one** entry (x and z both normalize to the all-zero span) where the
  LRM wants zero entries and two warnings.
- **P4c — two key paths bypass `lower_assoc_key`** (`emit_assoc_literal_init`,
  `const_table_signal`). Currently unreachable for aggregate keys because a
  struct localparam is separately unsupported; it becomes reachable as soon as
  key types widen.
- **P4d — `%p` of an assoc leaf is refused** (`Lower.cpp:16729`), which will read
  as a regression once exotic keys are printable.

## 8. Test and oracle plan

- iverilog is not usable as an oracle here: it does not implement enough of the
  assoc surface (and cannot compile `inside`, for what that is worth), so the
  plan below pins behaviour against the LRM directly.
- Compare **sets, not order**, for every key type without a defined relational
  operator — 7.8.5 explicitly leaves traversal order implementation-defined
  there, so an order-sensitive expectation would be testing nothing real. Assert
  ordering only for integral and string keys.
- Assert the LRM's stability rule directly: traverse, insert nothing, traverse
  again, require identical order.
- Pin the injectivity that motivates the whole encoding: `'{"ab","c"}` and
  `'{"a","bc"}` must be two distinct entries.
- Pin the value-semantics rule: mutate the source container *after* inserting
  with a container-bearing key; the entry must not move (keys are copied values).
- Round-trip every key type through `first`/`next` into a user variable and back
  into a lookup.
- Keep a rejection test for whatever stays guarded.

## 9. Performance

The fixed path is untouched: an integral, enum, packed, handle or all-integral
aggregate key still builds its span in an entry-block alloca with no call. Only
a key type that *contains* a variable-length component pays for `rt_keybuf_*`,
and that cost is a serialization pass proportional to the key's live size, once
per lookup. An assoc-bearing key additionally forces the source map's sorted
index to be built — worth a note in the docs, since it turns an O(1) lookup into
O(n log n) on first use of such a key.

## 10. Open questions

1. ~~Does slang define `==` for an unpacked union?~~ **Answered while scoping:
   yes.** `typedef union { byte a; byte b; } u_t; u_t x, y; x == y` binds and
   runs on `main` today (prints 1), so an unpacked union has the equality
   operator 7.8.5 asks for and belongs in P0's admit list. The only work is that
   `is_agg_abi_type` (`Lower.cpp:2497`) tests `isUnpackedStruct()` and misses it,
   even though `lower_type` already gives a union `Kind::Struct` storage — so the
   leaf walk needs the union's overlapping members thought about (two members at
   offset 0 must not serialize twice).
2. Should an **assoc-bearing key** be legal at all in clotho's first cut? It is
   the LRM's own example, so yes eventually, but it is the only phase that makes
   a lookup depend on another map's sorted index. Splitting it out (P3) keeps
   that decision reversible.
3. Ordering for **string keys inside an aggregate**: this proposal length-prefixes
   them, so the aggregate's order is not lexicographic in that leaf. That is
   legal (no `<` on a struct), but if we ever want "obvious" ordering for
   debugging, an escaping encoding (`00 -> 00 FF`, terminator `00 00`) preserves
   order at the cost of a second pass and up to 2x expansion.
4. Is there a **key-size cap** worth enforcing (a queue key with a million
   elements serializes a million times per lookup)? A loud failure past some
   large bound is cheaper to explain than a mysterious hang.
