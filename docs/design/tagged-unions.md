# Tagged unions (LRM 7.3.2)

**Status: Phases 1 and 2 landed, and so has the LRM 11.9 access check** (type +
`tagged` setter + tag-checked member r/w + whole-value ops + pattern matching +
`%p`). Phase 3 (non-integral unpacked payloads) and a few pattern corners
remain, gated loud.

Phase 1 (`src/lower/Types.cpp`): `lower_type` builds the flat `{tag, payload}` type
for both packed and unpacked tagged unions; `lower_tagged_union_set` lowers the
`TaggedUnionExpression` setter; `union_tag_bits` computes the discriminant
width. A member write does not touch the tag — the `store_union_tag` helper that
used to retag on a direct `u.m = v`, and its special case in `lower_assign`, are
gone (LRM 11.9, below). Format and gap-bit decisions are pinned under
**Semantics pinned** below.

Phase 2 (pattern matching, LRM 12.6): `lower_pattern` matches a pattern against
a flat subject rvalue (sub-values extracted by shift/resize), returning a 1-bit
predicate and binding `PatternVarSymbol`s (via `declare_storage` + a store, the
foreach-iterator mechanism). Supports all five pattern kinds — Wildcard `.*`,
Variable `.n`, Constant, Tagged, Structure. `lower_pattern_case`
(`StatementKind::PatternCase`) lowers `case matches` as a first-match if/else
chain. `lower_conditions` (templated over the `if` and `?:` `Condition` structs)
AND-reduces a cond_predicate, so `if (expr matches pat)`, `expr matches pat ? a
: b`, and `&&&` filters (an extra AND-ed `Condition`) all work; the `?:` form
computes the condition once up front (LRM 11.4.11) and shares it with the
branchless and lazy select paths, so a pattern var is bound before the arms and
an observable arm still single-evaluates. Neither iverilog nor verilator has
tagged-union support, so this is pinned by tests in `tests/struct_test.cpp`
rather than by an outside implementation.

`unique` / `priority` / `unique0` pattern case (LRM 12.6.1: "The priority and
unique qualifiers can also be used") report the same violations as a plain case
— `lower_pattern_case_checked` evaluates every item predicate once, runs
`emit_overlap_check` (unique/unique0) and the no-match report (priority/unique,
no default), then first-match dispatches. Same detection and wording as the
plain-case path.

`unique` / `unique0 if (expr matches pattern)` was the third `matches` guard
site — the Phase 2 plan below named only two, so this one survived undocumented
and kept rejecting with `pattern matching in conditions is not supported` (a
`&&&` filter failed one line earlier, as an unsupported multi-condition `if`).
It is legal SV: LRM 12.4.2 attaches `unique_priority` to `if (cond_predicate)`
and 12.6.2 makes a pattern a `cond_predicate`. `priority if (... matches ...)`
already worked. Fixed: `lower_unique_if` now collects each arm's whole condition
list and lowers it through `lower_conditions`, the same helper the plain and
priority chains use. Every arm's predicate — pattern bindings and `&&&` filters
included — is evaluated once up front, which 12.4.2 permits for unique/unique0
since overlap detection has to continue past the first true condition; binding a
non-selected arm's pattern variables is harmless because each is a pure
sub-value extraction, read only inside the arm its own predicate guards. Same
discipline as `lower_pattern_case_checked`.

`casez` / `casex ... matches` (LRM 12.6.1): the case keyword's comparison op
threads through `lower_pattern` (`pattern_cmp`) — `CaseEq` for `case`/`if`/`?:`,
`CasezEq`/`CasexEq` for casez/casex — governing every bit compare a pattern
generates (tag + Constant leaves), so the z/x label bits wildcard per the LRM.
Applying the case keyword's own comparison op uniformly — rather than making a
pattern label the one place where casez/casex stops wildcarding — is the point:
one rule for every label the statement compares.

## The LRM 11.9 access check

Every tagged-union member access — read and write — is guarded against the
union's current tag. LRM 11.9: "Members of tagged unions can be read or assigned
using the usual dot notation. Such accesses are completely type-checked, i.e.,
the value read or assigned shall be consistent with the current tag. In general,
this can require a run-time check. An attempt to read or assign a value whose
type is inconsistent with the tag results in a run-time error." Its example block
puts writes under the same rule as reads — "All of the following examples are
legal only if the instruction variable i1 currently has tag Add:", over
`x = i1.Add.reg1;`, `i1.Add = '{19, 4, 3};`, and `i1.Add.reg2 = 4;`.

`check_union_tag` plants the guard from inside `lower_member_lvalue`, the single
funnel every member access passes through, so one call site covers member reads,
member writes, compound `u.m += v`, a concat-embedded `{u.m, ...} = v`, `ref`
arguments, `force`, and `$fread` targets. It reads the tag off the union lvalue
the member select is about to use, so nothing is re-evaluated.

Shape: a new `TaggedCheck` IR node carrying an all-compile-time site record (the
member name, its expected `fieldIndex`, the member-name table in tag order, and
the location), plus a new runtime fatal `rt_tagged_access`
(`src/runtime/Reports.cpp`) modelled on the existing `rt_null_handle` null-handle
fatal. The cost on the hot path is one 4-state equality compare of the tag
against the member's `fieldIndex` and one branch weighted toward the
fall-through, with the report in a cold `noreturn` block. The report is a located
fatal with a caret snippet and two notes — which tag the union actually holds,
and that a tagged union's tag and value change only together through a
whole-value `tagged` expression — and exits nonzero.

**A member write no longer sets the tag.** Only a whole-value `tagged Member(v)`
expression moves it, which is what LRM 7.3.2 asks for: "The tag and value can
only be updated together consistently using a statically type-checked tagged
union expression (see 11.9)."

An x/z tag matches no member and reports; the note reads `the tag selects
undefined (x)`, following 11.9's "An uninitialized variable of tagged union type
shall be undefined. This includes the tag bits."

Elided where no tag value can disagree: a plain (untagged) union, and a
single-member tagged union (zero tag bits, so its one member is always the active
one). Never reached from a `case ... matches` / `if ... matches` arm either —
`lower_pattern` extracts the payload itself by shift/resize and binds a pattern
variable, so a matched arm never goes through a member access at all.

The LRM does not say whether the run-time error stops the simulation; clotho
reports it as a located fatal, the same way it treats every other normative
run-time error.

## `%p` (LRM 21.2.1.6)

21.2.1.6 is specific about tagged unions: "In the case of a tagged union, it
shall print "tag:value" along with the currently valid element." Four structural
`shall` clauses in that paragraph used to be violated silently, because `%p`
dispatched on `is_agg_abi_ref`, a *storage* predicate (`UnpackedArray || (Struct
&& !is_packed)`) — a tagged union, a plain union, a packed struct and an enum are
all `is_packed` / singular, so they fell straight through to a bare `%0d` decimal
with no loud guard. Measured forms now:

| operand | `%p` |
|---|---|
| packed tagged union | `'{Valid:42}`; a void member `'{Invalid}` |
| unpacked tagged union | `'{b:-85}` (`byte`), `'{w:-559038737}` (`int`), `'{none}` |
| packed struct | `'{a:-85, b:42}` |
| plain packed union | `'{a:-85}` |
| enum | `GREEN`; a value matching no member `3` |
| nested in an unpacked struct | `'{o:'{Valid:7}, c:GREEN, p:'{hi:17, lo:34}, n:5}` |
| unpacked array of tagged unions | `'{'{Valid:1}, '{Invalid}}` |
| 4-state tagged union, x tag | `'{x}` |

A tagged union is the one `%p` shape rendered in the runtime (new `TaggedFmtP` IR
node → new `rt_tagged_format_p`), because only the run-time tag says which member
is valid; every other shape is still expanded to literal text at lower time. The
payload renders at the **active member's own width**, not the union's whole
payload region, so the LRM-undefined gap bits never show up, and at the
member's own signedness (the `signs` column of the member table). An x/z or
out-of-range tag has no valid element, so `%p` falls back to rendering the whole
flat value as one decimal leaf — 21.2.1.6 does not cover that case.

The `tag:value` pair is bracketed `'{...}` at lower time: the clause requires the
whole output to be "a legal interpretation of the assignment pattern syntax", which
a bare `tag:value` is not. The bracketed form is also what sv-tests
`chapter-7/unions/tagged/basic.sv` asserts.

A tagged union in a queue / dynamic-array **element** position is rejected loud
(`%p of a queue / dynamic array of tagged unions is not supported`) because the
runtime container descriptor carries no member-name table. New documented gap.

Leaf radix: minimal **decimal** at the leaf's own
signedness (21.2.1.6's "as they would unformatted" plus 21.2.1.1's decimal
default), `%g` for reals, quoted for strings. Only the white space is
implementation-dependent. Nothing in `%p` is left knowingly deviating.

## Still gated loud (follow-ups)

- Non-integral member of an *unpacked* tagged union (`real` / `string` /
  unpacked aggregate / class payload) — the Phase 3 gap.
- Pattern case / structure pattern over an unpacked **aggregate** subject (an
  unpacked struct or array). An unpacked *tagged union* is not caught by that
  guard and does work: it lowers to a flat integral.
- `force` / `release`, DPI, and queue / dynamic-array / associative-array element
  positions, for the *unpacked* tagged-union form only — all of them key on
  slang's `isIntegral()`. The packed form works in each.
- `%p` of a queue / dynamic array of tagged unions.

Open, but not loud: `$cast` into a packed tagged union is a bit cast and is not
tag-checked; VCD dumps a tagged union as one flat anonymous `$var` (LRM 21.7 has no
tagged-union notion). Tracked in docs/TODO.md.

## What SV gives you (the whole surface)

1. **Type decl** — `union tagged { void Invalid; int a; bit[7:0] b; } u;`,
   packed or unpacked. Members may include `void` (no payload); a packed member
   must be integral, unpacked may (LRM) be any type.
2. **Construction / whole-set** — `u = tagged a (3);` or void `u = tagged Invalid;`
   (slang `TaggedUnionExpression`, `ExpressionKind::TaggedUnion`). This is the
   ONLY way to change the active tag.
3. **Member read** — `u.a` (`MemberAccessExpression`). Legal only while the tag
   selects `a`; otherwise a run-time error (LRM 11.9, above).
4. **Member write** — `u.a = 3;` requires the tag to *already* select `a` and
   never moves it (LRM 11.9 / 7.3.2). Same rule as the read, and the same
   run-time check. A plain union differs twice over: it has no tag, and a member
   write there leaves the rest of the value untouched.
5. **Pattern matching** (LRM 12.6) — the payoff for tagged unions:
   - `case (u) matches` … `tagged a .x : …` `tagged Invalid : …` `default:`
     (`PatternCaseStatement`).
   - `if (u matches tagged a .x) …` (`ConditionalStatement`, `condition.pattern`),
     including under `unique` / `unique0` / `priority`.
   - `u matches tagged a .x ? … : …` (`ConditionalExpression`, same field).
   - Optional filter guard `&&& expr` on case items / conditions.
   - Five pattern kinds (`PatternKind`): `Wildcard` (`.*`, always true),
     `Constant` (expr equality), `Variable` (`.x`, binds a `PatternVarSymbol`),
     `Tagged` (tag compare + optional nested pattern on payload), `Structure`
     (per-field nested patterns — for struct-typed payloads/operands).

## slang layout facts (measured from source)

**Packed tagged** (`PackedUnionType`, is an `IntegralType`):
- `bitWidth = tagBits + max(member widths)`, `tagBits = bit_width(nMembers-1)`.
- Tag in the **high** bits `[bitWidth-1 : bitWidth-tagBits]`; payload in low
  bits `[valWidth-1:0]`; bits between are undefined/x
  (`TaggedUnionExpression::evalImpl`, `AllTypes.cpp` `PackedUnionType::fromSyntax`).
- Tag value of member `i` = its `fieldIndex` (declaration order, 0-based).
- Every member `FieldSymbol.bitOffset == 0` (payload overlays LSBs).
- Members integral or `void`.

**Unpacked tagged** (`UnpackedUnionType`, NOT integral): slang models it as an
`SVUnion{activeMember, value}` aggregate — no dictated bit layout, members any
type.

**Pattern eval** (`TaggedPattern::evalImpl`): match ⇔
`activeMember == member.fieldIndex`, then recurse `valuePattern` on the payload;
void member ⇒ succeed with no binding.

## Representation decision

**One representation for both: a flat integral value, tag in high bits, payload
in low bits** — reuses the entire existing non-tagged packed-union path
(`Kind::Struct`, `is_packed`, `is_union`, all field `bit_offset 0`; storage,
NBA, VCD, change-detect, whole-value `==`/`$display`/streaming all free).

- **Packed tagged**: use slang's exact `tagBits` / `getBitWidth()` so the
  bit-vector matches LRM for part-select / streaming / equality.
- **Unpacked tagged**: synthesize `tagBits = bit_width(nMembers-1)`,
  `width = tagBits + maxMemberWidth`, **integral members only** (same
  restriction already enforced for non-tagged unpacked unions at `:1180`).
  Non-integral unpacked-tagged members (real/string/struct payload) loud-fail →
  Phase 3.

This is the user's "tag + payload" idea realized as a bit-vector concat (no LLVM
struct, cheaper, rides existing machinery). Tag = high part-select; member
payload = low part-select. `union_tag_bits` recomputes the discriminant width
from the slang type on demand, which is all the member select, the pattern
matcher and the 11.9 check need to find the tag.

**Withdrawn — not deferred: storing `tagBits` + payload width, or an `is_tagged`
flag, on `ir::Type`.** `ir::Type` deliberately carries no taggedness: past
`lower_type` the flat `{tag, payload}` representation makes a tagged union
indistinguishable from a plain packed union, and that indistinguishability is
exactly what buys the free ride above. The one place that genuinely has to tell
them apart is `%p`, where 21.2.1.6 names tagged unions, plain unions, packed
structs and enums as separate cases — and it gets there by threading the slang
`Type` alongside the lowered `ir::Type` through the `%p` expansion. Same
coverage, nested leaves included, without widening a byte-hashed interned struct
that has no spare padding
(`static_assert(std::has_unique_object_representations_v<Type>)`). No remaining
caller wants the flag.

## Phases

### Phase 1 — type + construct + member r/w (no pattern matching)
Makes tagged unions declarable, assignable, printable, comparable.
- `lower_type`: replace both `fail` guards; compute `tagBits`/payload width;
  build flat union type (packed: slang width; unpacked: synthesized,
  integral-only gate + void handling).
- `TaggedUnionExpression` in `lower_expr`: build
  `(zext(tag_index) << (width-tagBits)) | zext(payload)`; void ⇒ payload 0/x.
- **Member write** `u.m = e`: reuses the packed member-select store unchanged.
  P1 additionally retagged the union to `m.fieldIndex` here; LRM 11.9 forbids
  that, so the branch was removed again (see the 11.9 section).
- **Member read** `u.m`: low part-select (existing MemberSelect path already
  correct).
- **Tag check** on either: `check_union_tag` from `lower_member_lvalue` (11.9).
- Whole-union assign / `==` / `$display %b` / VCD: free via flat path.
- Tests in `tests/struct_test.cpp`.

### Phase 2 — pattern matching (the bulk)
- New `StatementKind::PatternCase` handler: lower to an if/else-if chain over
  items (reuse `lower_if_chain` structure incl. unique/priority no-match report).
- Remove the `matches` guards and route condition/`?:` patterns through the same
  matcher. **Three** sites, not the two originally listed (`:4475` and `:8782`):
  the `unique` / `unique0 if` path is the third and was missed here, which is why
  it stayed rejected after Phase 2 shipped.
- **Matcher** `lower_pattern(pat, matched_value) -> {predicate, bindings}`:
  - `Wildcard` → pred = 1.
  - `Constant` → pred = `matched == expr`.
  - `Variable` → pred = 1, binding `(PatternVarSymbol := matched)`.
  - `Tagged` → pred = `high_tagbits(matched) == fieldIndex`, AND recurse
    `valuePattern` on `low_payload(matched)`; void ⇒ just the tag compare.
  - `Structure` → per-field recurse on struct member selects, AND-reduce.
- **Pattern-var binding**: `PatternVarSymbol` is a `TempVarSymbol` — reuse the
  foreach iterator-var mechanism (`declare_storage(sym, force_frame=true)` +
  `value_lvalue`, `:4802`); store the matched sub-value before running the arm.
- **Filter guard** `&&& expr`: AND into the item/condition predicate after
  bindings are in scope (filter may reference pattern vars).
- Predicate composition: definite/ambiguous (x) tag compares fold through
  existing logical-AND lowering; unmatched arms fall through.

### Phase 3 — deferred (loud-failed until then)
- Non-integral unpacked-tagged members (real / string / unpacked struct / class
  payload) — needs a true tag+aggregate representation, not the flat plane.

## Semantics pinned

- **Accessing a member the tag does not select** (`u.a` when tag≠a), read or
  write: a located run-time fatal per LRM 11.9, and a member write never retags.
- **Middle/undefined bits** of a packed tagged value (between tag and a narrower
  payload), and a `void` member's payload region: x for a 4-state union, 0 for
  2-state, matching slang const-eval so a runtime setter and the same expression
  const-folded produce identical bits. `%b` shows them; `%p` does not, since it
  renders the active member at its own width.
- **No iverilog or verilator reference**: neither has tagged-union support at
  all, so neither can cross-check this
  feature. slang const-eval (`Pattern::eval`,
  `TaggedUnionExpression::evalImpl`) is the reference for values, and where the
  normative text is explicit clotho follows it — the casez/casex label bits and
  the 11.9 check above are both decided that way.

## Effort

- P1: small — mostly `lower_type` + one setter expr + one member-store branch;
  reuses the whole flat-union path. ~half day.
- P2: medium — the matcher + PatternCase + 3 guard sites + var binding; each
  piece is mechanical but there are five pattern kinds and the binding/scoping.
- P3: only if a real design needs non-integral unpacked payloads.
