# Associative-array keys: every legal index type

**Status: landed.** Every LRM 7.8.5 index type works: integral (2-/4-state,
enum, packed), `[*]` wildcard, string, class handle, chandle, event, virtual
interface, unpacked union, unpacked struct / fixed unpacked array, queue,
dynamic array, associative array, and any nesting of these through structs /
fixed arrays to arbitrary depth -- including the LRM's own example
(`struct { byte B; int I[*]; }`). Supersedes `dynamic-arrays.md` §4a, which
described the pre-P0 six-kind subset.

Landed across five commits (P0-P4, docs/TODO.md): P0 admitted chandle / event
/ virtual-interface / unpacked-union keys (all already flat handle-shaped or
is_packed values, no backend change needed); P1 built the variable-length key
mechanism (`rt_keybuf_*`) and string leaves; P2 added queue / dynamic-array
key components; P3 added associative-array key components (completing the
LRM's example); P4 fixed four independent correctness debts found while
scoping (§7).

One scope boundary beyond the original plan: a class handle anywhere inside a
VARIABLE key (a key containing a string / queue / dynamic-array / associative-
array component) is refused loudly (`guard_key_no_handle_in_variable`,
lower/Types.cpp). The collector's key-handle trace is a compile-time STATIC offset
table, correct only for a FIXED key; a variable key's byte offsets depend on
runtime data, so a handle inside one could land at a different offset per
entry. A runtime decode walk the collector could use instead was not built.
A handle in an otherwise-fixed key (P0's bare handle keys, or one nested only
through fixed struct/array members) is unaffected -- its static offset already
works, verified under `--gc-stress` with heavy allocation churn around a live
keyed entry.

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

### Frontend — `src/lower/`

- Admit list, `lower_type`'s `AssociativeArrayType` arm (`lower/Types.cpp`):
  `kt.isClass() || kt.isIntegral() || kt.isString() || is_agg_abi_type(kt)`.
  Anything else gets one message naming the admitted set.
- `is_agg_abi_type` (`lower/Lowerer.cpp`) is `isUnpackedStruct() ||
  as_if<FixedSizeUnpackedArrayType>()` — note it **excludes an unpacked union**,
  which `lower_type` is otherwise happy to lower to `Kind::Struct`.
- `guard_agg_key_leaves` (`lower/Types.cpp`) walks an aggregate key's leaves
  and admits **integral leaves only**. It emits the two messages the conformance
  suite is hitting: `... with a string member is not supported` and `... with a
  queue / dynamic-array member is not supported`. Its `is_float` arm is dead —
  slang rejects a real-bearing index type first (`index type cannot be or contain
  floating point types`).
- The type pool encodes the key on the `Assoc` type: `t.fields_id` = the key type
  ref, `t.right` = the `[*]` wildcard flag (`lower/Types.cpp, lower_type`).
- `lower_assoc_key` is the single frontend entry that lowers a key expression;
  two paths bypass it (`emit_assoc_literal_init` at `lower/Assign.cpp`,
  `const_table_signal` at `lower/Types.cpp`) — latent, see §7.
- The frontend does **nothing** about x/z indices.

### Backend — `src/ir/llvm/AssocArrays.cpp`

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

### Runtime — `src/runtime/Containers.cpp, rt_assoc_*`, `Runtime.h:1133-1213`

**The runtime is already key-kind-agnostic and already variable-length.**

- `AssocMap` (`runtime/Containers.cpp`) is
  `ankerl::unordered_dense::map<std::string, SlotVec>` plus a lazily built
  `std::vector<const std::string*> sorted`.
- Keys are **owned, per-entry `std::string`** — copied from the caller's span on
  insert. There is no key-width field anywhere on `rt_assoc` or `AssocMap`.
- Ordering is `AssocKeyLess` (`runtime/Containers.cpp`): `memcmp` over the common
  prefix, tie-break **shorter-is-less**.
- Equality is `memcmp`; the hash is over the raw span. No per-key-type ops
  descriptor exists (`rt_elem_ops`, `Runtime.h:897`, has copy/deinit/init for
  VALUES but no compare and no hash).
- `assoc_set_span` (`runtime/Containers.cpp`) hands `k->data(), k->size()` back for
  `first`/`last`/`next`/`prev`.

So the map layer needs **no change** for variable-length keys. Every restriction
is in the backend's fixed-size span construction and the frontend guards that
protect it.

### Support matrix, landed

| index type | status |
| --- | --- |
| integral 2-state / 4-state, enum, packed | works |
| `[*]` wildcard | works |
| string | works |
| class handle | works |
| chandle, event, virtual interface (P0) | works |
| unpacked union (P0) | works |
| unpacked struct / fixed unpacked array of integrals | works |
| unpacked struct or array containing a **string** (P1) | works |
| unpacked struct or array containing a **queue / dynamic array** (P2) | works |
| unpacked struct or array containing an **associative array** (P3) | works |
| **queue**, **dynamic array**, **associative array** as the key itself (P2/P3) | works |
| any nesting of the above to arbitrary depth (queue of queues, assoc of structs of strings, ...) | works |
| a class handle combined with a string / queue / dynamic-array / associative-array component | refused (GC-safety scope boundary, see header) |
| a non-lvalue dynamic-array / associative-array key expression (a literal, a slice, an in-place call result) | refused (needs the dynamic-array/assoc twin of `materialize_queue_rvalue`) |
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
span into its own `std::string` on insert
(`runtime/Containers.cpp, rt_assoc_put_ptr`), so the scratch never has to
outlive the call — that ownership property already holds and is what
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

## 6. Phases — all landed

| phase | content | commit |
| --- | --- | --- |
| **P0** | chandle / event / virtual-interface keys (already flat handle words); unpacked-union keys (already flat `is_packed` values, since it lowers identically to a packed union) | `assoc: admit chandle / event / virtual-interface / unpacked-union keys (P0)` |
| **P1** | `rt_keybuf_*` + the fixed/variable split + **string leaves** inside aggregate keys, including denormalization for traversal | `assoc: variable-length key encoding for string leaves inside struct/array keys (P1)` |
| **P2** | **queue / dynamic-array** components, top-level and nested (count-prefixed), including materializing deserialization for traversal | `assoc: queue / dynamic-array key components, nested or bare (P2)` |
| **P3** | **assoc** components (canonical sorted-entry serialization), completing the LRM 7.8.5 example | `assoc: associative-array key components, nested or bare (P3)` |
| **P4** | the four correctness debts below | `assoc: fix silently-wrong whole-array equality (P4a)`, `assoc: implement LRM 7.8.6 invalid-key handling (P4b)`, `assoc: route literal-pattern keys through lower_assoc_key; fix a %p crash (P4c, P4d)` |

P2/P3's runtime loops (materializing a queue/dynarray/assoc from decoded key
bytes, and the sorted-entry walk) also surfaced a latent SSA-dominance bug:
the encoder's unknown-bit accumulator was threaded as a plain SSA reference
across a runtime loop of unbounded trip count, which the LLVM verifier
rejects once a leaf's `any_unknown()` is a genuine runtime value rather than
a compile-time constant (latent for every all-2-state key tested through
P1-P3; surfaced by P4a's 4-state-value regression test). Fixed by making the
accumulator memory-backed (an `i1*` alloca), safe regardless of loop trip
count — folded in with the P4a commit.

## 7. Correctness debts found while scoping — all fixed

These were pre-existing, independent of the feature:

- **P4a — associative-array `==` / `!=` was silently wrong.** `is_assoc` was
  absent from the aggregate-equality dispatch, so a whole-assoc compare fell
  through to the scalar binary path (verified on `main`: two maps with
  different contents reported `eq 1 ne 0`). Fixed: reuses the P3 canonical
  byte serializer on each whole operand and compares bytes -- bit-exact, so
  `===`/`!==` always work; plain `==`/`!=` are refused loudly for a
  4-state-capable value element, where the LRM's X-propagating answer can
  differ from a flat byte compare.
- **P4b — LRM 7.8.6 was not implemented.** No warning was ever emitted for an
  invalid (x/z) index on read or write, and `integral_key_valid` treated a
  *literal* x/z key as valid because its unknown mask is an `llvm::Constant`
  (checking "is a Constant" rather than "is the constant false"), so
  `a[8'hxx] = 7; a[8'hzz] = 8;` created **one** entry instead of the LRM's zero.
  Fixed: the constant-folding bug is corrected, and the mandated warning is
  emitted under `--sanitize=bounds` (`RT_BOUNDS_ASSOC_KEY`), matching how
  every other "shall issue a warning" bounds clause Clotho already implements.
- **P4c — two key paths bypassed `lower_assoc_key`** (`emit_assoc_literal_init`,
  `const_table_signal`). `emit_assoc_literal_init` (a `'{key: value}` literal
  pattern) is fixed, now reachable and exercised by P1-P3's widened key kinds.
  `const_table_signal`'s own bypass (seeding a parameter-typed assoc constant)
  remains unreachable, gated by an independent, still-standing "only integral,
  real, string and null constant parameters" restriction unrelated to this
  feature; documented in place (`lower/Types.cpp`, `emit_const_seed`) for if that
  restriction is ever lifted.
- **P4d — `%p` of an assoc with an unpacked-struct / fixed-array key crashed**
  (a slang assertion, casting the key's unpacked struct type as packed) --
  pre-existing on `main`, found while verifying `%p` for the newly admitted
  key kinds. The key rendering unconditionally used the scalar leaf `%p` path;
  fixed to split on `is_agg_abi_ref` the same way `key_operand()` already does
  for `a[k]` itself. A bare queue / dynamic-array / associative-array key
  still refuses `%p` cleanly (no crash), matching the pre-existing,
  independent "container VALUE element" `%p` limitation.

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
- Keep a rejection test for whatever stays guarded, per PRINCIPLES.md.

## 9. Performance

The fixed path is untouched: an integral, enum, packed, handle or all-integral
aggregate key still builds its span in an entry-block alloca with no call. Only
a key type that *contains* a variable-length component pays for `rt_keybuf_*`,
and that cost is a serialization pass proportional to the key's live size, once
per lookup. An assoc-bearing key additionally forces the source map's sorted
index to be built — worth a note in the docs, since it turns an O(1) lookup into
O(n log n) on first use of such a key.

## 10. Open questions — resolved

1. ~~Does slang define `==` for an unpacked union?~~ **Answered while scoping:
   yes**, and it turned out moot: `lower_type`'s `isUnpackedUnion` arm already
   lowers it identically to a packed union (one flat `is_packed` value, LRM's
   overlay left to Clotho since 7.3 does not fix one for the unpacked case) --
   so it already fell into `assoc_key_info`'s integral-width branch with no
   leaf walk, no overlap handling, and no new backend code at all (P0).
2. Should an **assoc-bearing key** be legal at all in clotho's first cut?
   **Yes** (P3) -- landed as its own phase, on top of the P1 mechanism, as
   planned.
3. Ordering for **string keys inside an aggregate**: **length-prefixed**
   (P1), per the recommendation here -- simple, matches every other variable
   leaf, no `<` guarantee needed (legal per 7.8.5, no relational operator
   defined for an aggregate key). The escaping alternative was not built.
4. Is there a **key-size cap** worth enforcing? **No** -- left unbounded, on
   the same trust-the-user basis as every other unbounded container
   operation in Clotho.
