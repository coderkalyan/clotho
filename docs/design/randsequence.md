# Random sequence generation — `randsequence` (LRM 18.17)

Target: the whole `randsequence` construct — productions, alternative rules with
`:=` weights, code blocks, `rs_if_else` / `rs_repeat` / `rs_case` production
statements, `rand join` interleaving, `break` / `return` abort semantics, and the
value passing of 18.17.7 (production arguments, non-void productions, and the
implicit per-rule result variables).

Status: **landed** (P0-P4). Four things came out differently from the plan below
and the text is corrected in place: the abort flag and the result slot are
`output` formals rather than `ref` ones, which removed the entry wrapper the plan
called for (§6); the result travels through a caller temp and is copied into the
rule variable after the call (§10); the rule variables are default-initialized
rather than left at the frame's raw contents (§10); and `rand join` needed two
extra entry points per interleaved production (§11).

P4 then lifted four of the six refusals the first pass left behind — a container
return type, the two things an interleaved production could not hold across a unit
boundary, and a block sharing an automatic with a detached fork. Two refusals
remain, each recorded at its guard as a deliberate deferral rather than an
oversight (§14).

This document owns the production-to-`ir::Function` lowering, the production ABI,
the rule-selection and weight rules, the abort model, the value-passing model, and
the `rand join` algorithm. `docs/design/constrained-randomization.md` owns the RNG
itself and §9 of that document owns random stability; this design consumes both and
changes neither. `docs/design/process-class.md` §7.4 owns thread seeding.

---

## 1. What this is

**In:** everything in LRM 18.17.1 through 18.17.7.

**Out of the language, therefore out of scope:** nothing. `randsequence` has no
optional sub-features.

**Out of *this* design, by delegation:** the RNG (`b.urandom_range`, already built),
random stability (already built, §12), and the constraint solver (unrelated —
18.17 has weights, not constraints).

**Refused, each by name (§14):** two constructs, both deferred deliberately — a
rule weight wider than 32 bits, which `randcase` shares and must change with, and a
`rand join` operand that is itself a `rand join`, which LRM 18.17.5's "to depth of
1" does not describe.

---

## 2. Current state

### 2.1 Before this work

`randsequence` had no arm in `lower_stmt` (`src/Lower.cpp:10751-10917`), so it fell
to the catch-all `default: fail("unsupported statement")` — the only guard in the
file that named no construct. Measured on the LRM 18.17 opening example:

```
$ clotho run rs1.sv
rs1.sv:3:5: error: unsupported statement [unsupported-construct]
```

### 2.2 What slang already gives us

slang parses and fully elaborates `randsequence`. Elaboration is *eager*:
`ElabVisitors.h:400-406` calls `RandSeqProductionSymbol::getRules()` on every
production, so by the time `lower_stmt` runs, every rule, weight expression,
production argument and code-block statement is bound and every randsequence
diagnostic has already been emitted.

| slang gives | where |
|---|---|
| `RandSequenceStatement { firstProduction, productions }` | `include/slang/ast/statements/MiscStatements.h:466-491` |
| `RandSeqProductionSymbol` — a `Symbol` **and** a `Scope`, with `arguments` (real `FormalArgumentSymbol`s built by `SubroutineSymbol::buildArguments`) and `declaredReturnType` (explicitly `void` when absent) | `include/slang/ast/symbols/MemberSymbols.h:400-598`; `source/ast/symbols/MemberSymbols.cpp:1921-1949` |
| `Rule { ruleBlock, prods, weightExpr, randJoinExpr, codeBlock, isRandJoin }` | `MemberSymbols.h:503-516` |
| `ProdKind { Item, CodeBlock, IfElse, Repeat, Case }` — five kinds; `rand join` is a *rule* flag, not a prod kind | `MemberSymbols.h:403` |
| the implicit 18.17.7 result variables, already synthesized as `const` + `CompilerGenerated` `VariableSymbol`s in each rule block, scalar for one occurrence and `[1:N]` `FixedSizeUnpackedArray` for N | `MemberSymbols.cpp:2152-2224`, injected at `BlockSymbols.cpp:309-316` |
| `return` inside a production bound against the *production's* return type, with `MissingReturnValueProd` for a bare `return` in a non-void production | `source/ast/statements/LoopStatements.cpp:25-62` |
| `break` legality inside a production (`StatementFlags::InRandSeq`) | `LoopStatements.cpp:83-90` |

Diagnostics slang already emits, which Clotho does **not** re-check:
`UndeclaredIdentifier`, `NotAProduction`, `TooFew/TooManyArguments` and the whole
`CallExpression::bindArgs` family, `NotBooleanConvertible` (if-condition),
`ExprMustBeIntegral` (weight, repeat count), `RandJoinNotNumeric`,
`RandJoinNotEnough` / `RandJoinProdItem` (parser), `MultipleDefaultCases`,
`CaseStatementEmpty`, `ExpectedRsRule`, `Redefinition` (duplicate production names),
`ReturnNotInSubroutine`, `MissingReturnValueProd`, `IndexOOB` on a result-variable
index.

What slang does **not** check, and Clotho therefore owns: recursion (there is no
production-recursion analysis anywhere in slang), weight non-negativity, zero total
weight, repeat-count non-negativity, `rand join` count range, reachability, and
termination. slang also has no constant-eval path — `RandSequenceStatement::evalImpl`
unconditionally emits `ConstEvalRandValue` and fails.

### 2.3 What was already built and got reused

| piece | where | how it was used |
|---|---|---|
| weighted pick: evaluate N weights once, unsigned wrapping sum at the widest weight's precision, `sum != 0` guard, **one** `urandom_range(sum-1, 0)`, cumulative if/else ladder | `lower_randcase` / `lower_randcase_arm`, `src/Lower.cpp:18794-18870` | copied into `gen_weighted_rules` / `gen_rule_arm` for rule selection (§7) and again, in integer form, for the `rand join` step pick (§11) |
| recursion and mutual recursion | `Module::reserve_function()`, `src/ir/Module.h:773-782`; backend declares every function up front, `src/ir/LlvmBackend.cpp:942-944`, `:5367-5369` | ids for the whole production set are reserved before any body is built |
| the function-shaped body builder | `build_function` `src/Lower.cpp:29054`, `build_task` `:28800`, `FnCtx`, `FnScope`, `fn_local` | `build_production` is modelled on `build_fork_child` (`:28886`), the precedent for a synthetic body with no `SubroutineSymbol` |
| argument marshalling | `build_call_args` | its signature is `(span<const FormalArgumentSymbol* const>, span<const Expression* const>, ...)`, which is **exactly** `RandSeqProductionSymbol::arguments` and `ProdItem::args`. Reused verbatim, no adapter |
| enclosing-frame capture detection | `ForkCaptureDetector`, `src/Lower.cpp:1424-1491` | reused verbatim, driven over the block by an explicit walk (§5) |
| `ref` argument passing | `ir::ArgDir::Ref` | the capture formals |
| output-argument copy-out | `ir::ArgDir::Out`, `make_arg_temp`, `emit_call_copyouts` | the abort flag, the result slot, and the `rand join` step's stop flag |
| automatic-variable default init | `lower_var_decl` / `emit_default_init`, `src/Lower.cpp:15887` | the implicit result variables (§10) |
| random stability (LRM 18.14.1) | `docs/design/constrained-randomization.md` §9 | inherited for free (§12) |

---

## 3. Decision summary

| question | decision | why |
|---|---|---|
| production → what? | one `ir::Function` per `RandSeqProductionSymbol`, `is_task` when the enclosing body is a coroutine | LRM's own `GenQueue` example (18.17.7) is left-recursive; inlining cannot express it. Clotho already supports recursion; verilator inlines and consequently rejects the LRM example (§4) |
| how is the whole randsequence entered? | a direct call to the first production's function | 18.17: the top-level production is `randsequence(name)` or the first declared one |
| production ABI | `prod(<this>, output bit $abort, [output T $result,] formals…, captures…)` | `break` must escape across production calls, which no lexical `Disable` can do. **`output`, not the planned `ref`**: the backend's copy-out epilogue then delivers both at each return, which is also correct for a production that suspends, and the caller's own slot is the callee's actual, so the flag propagates one frame per return with no extra state |
| where does the top-level abort flag live? | a write-only `make_arg_temp` | **the planned entry wrapper is unnecessary.** Nothing above the randsequence statement can be unwound, so the top-level flag is never read, and its being a shared module signal in a process body is harmless |
| `return` in a production body | plain `RetVoid` in that production's own function | 18.17.6: "aborts the generation of the current production", which *is* the function |
| `break` in a production body, no enclosing loop | store 1 into `$abort`, `RetVoid`; every caller tests `$abort` after every production call and returns if set | 18.17.6: terminates the randsequence block |
| `break` inside a loop inside a production body | ordinary `b.break_()` | 18.17.6 defers to 12.8: innermost loop wins |
| rule selection | the `lower_randcase` ladder, one draw | 18.17.1 weights are the same construct as 18.16 |
| single-rule production | no weight evaluation, no draw | draw counts are observable (§12); a weight is meaningless without alternatives (18.17.1) |
| unspecified weight | the integer literal 1, at an integer's 32-bit self-determined precision | 18.17.1's "a weight of 1"; folding it into the common width is load-bearing — at the one-bit minimum two default weights sum to zero and neither alternative is ever generated |
| zero total weight | no rule generated, **no draw consumed** | matches the `randcase` precedent, `src/Lower.cpp:18825-18839` |
| weight wider than 32 bits | loud refusal, same message shape as randcase | the LRM's multi-draw stitching scheme is not built for randcase either |
| result variables (18.17.7) | frame locals of the production, **default-initialized**, written from a caller temp after each call, at a slot index re-derived from `createRuleVariables`' counting order | slang synthesizes the variables but records **no** occurrence→index map. The temp is needed because an `output` actual is a raw slot and an array element is a composed lvalue |
| unwritten result variable | the type default (Table 6-7) | 18.17.7 says "undefined value"; the frame's previous contents is the only alternative and is not reproducible |
| production returning a container | the blocking half of `lower_*_assign`, split out as `emit_container_store` and shared | the deep-copy rules belong in one place; a `return` has an expression and a slot but no `AssignmentExpression` to hand them |
| `rand join` mechanism | two extra entry points on each interleaved production: `plan` (draw the rule, report its unit count) and `step` (generate one numbered unit) | interleaving must advance a sequence one unit at a time; a plain call runs it to completion |
| `rand join` bias expression | evaluate the real once, clamp to [0,1], quantize to 1024ths, then integer weight `w_i = (1024−X)·(Lmax+1−L_i) + X·L_i` | hits all three LRM anchors (X=0 favours short, X=1024 favours long, X=512 exactly uniform) with no libm and no real IR; feeds the existing integer ladder |
| recursion depth cap | none | Clotho caps no other SV recursion; native stack is the limit |
| slang's `createCaseProd` / rule-block defects | fixed upstream-style in `patches/slang-randseq-rule-pairing.patch` | precedent: `patches/slang-class-forward-ref.patch`; working around them in Clotho would bake a copy of slang's syntax pairing into the frontend (§13) |

---

## 4. Semantics reproduced, and the oracle position

**iverilog does not parse `randsequence` at all** (`iverilog -g2012` on the 18.17
opening example: `syntax error` / `Malformed statement` on every production line).
It is not an oracle here.

**verilator 5.050 is a partial oracle.** It lowers a randsequence by fully inlining
the grammar into the enclosing body with a `__VrsBreak` flag and `goto` labels
(read out of its own generated C++), which is why it cannot do recursion and why
several LRM examples fail outright. Measured:

| LRM feature | verilator 5.050 | oracle | Clotho |
|---|---|---|---|
| 18.17 opening example | `add push done` | yes | one of the four legal utterances, checked by membership |
| 18.17.1 weights `add := 3 \| dec := (1+1)` over 10000 trials | `a=5962 d=4038` | yes | `a=6009 d=3991` |
| 18.17.2 if-else | works | yes | matches |
| 18.17.3 case, incl. the multi-label item `1, 2 : DISK` | `0 NETWORK / 1 DISK / 2 DISK / 3 MEMORY` | yes — and this is the exact shape slang mis-paired (§13) | identical |
| 18.17.4 `repeat` | works | yes | matches |
| 18.17.5 `rand join S1 S2` | the six legal interleavings | for the legal **set** only | all six, and nothing else, in 4000 trials |
| 18.17.5 bias `(0.0)` vs `(1.0)` | identical distributions — verilator ignores the expression and re-picks with fixed probability ½ per step | **no** | `(0.0)`: 68% blocked orders; `(1.0)`: 32%; `(0.5)` between |
| 18.17.6 `return` example (flag 0/1/2) | `A B C A B C` / `A B C A` / `A C A C` | yes, and fully determined by the LRM | byte-identical |
| 18.17.6 `break` example | **crashes**: `terminate called after throwing an instance of 'std::out_of_range'` | no | `CMD DATA next` / `next`, which is what 18.17.6's text describes |
| 18.17.7 value passing (both LRM examples) | `%Error-UNSUPPORTED: randsequence production function variable` | no | both work |
| 18.17.7 string production argument | passes one byte (`gen("dec")` prints `d`) | no | full string |
| 18.17.7 `GenQueue` (left-recursive) | cannot compile | no | works |

---

## 5. Lowering model

One `ir::Function` per production. In `lower_randsequence(const
RandSequenceStatement&, ir::Builder&)`:

1. **Reserve first, build second.** `randseq_block` calls `module_.reserve_function()`
   for every symbol in `stmt.productions` — and, for every production named as a
   `rand join` operand, two more for its `plan` and `step` — before any body is
   lowered. This is exactly why `reserve_function` exists (`src/ir/Module.h:773-776`)
   and is what makes mutual recursion between productions free.
2. **Collect captures.** With `fn_ctx_ != nullptr`, `ForkCaptureDetector` is driven
   over the block by `visit_randseq`, which walks each production's argument
   defaults, its `visitExprs`, and every code block's bound statement. The explicit
   walk is not optional: `RandSequenceStatement` has neither `visitExprs` nor
   `visitStmts`, so slang's generic visitor descends into nothing, and
   `RandSeqProductionSymbol::visitExprs` deliberately skips code blocks.
3. **Build each body** with a fresh `FnCtx` under `FnScope`, mirroring
   `build_function`, with `ctx.sym = nullptr` as in `build_fork_child`.
   `ctx.name_prefix` is `"randseq.<prod>."` so a `static` declaration in one
   production's code block cannot collide with another's.
4. **Failure atomicity.** Each builder fills its reserved slot with a dead body
   before rethrowing, and `randseq_block` keeps building the siblings so their slots
   are completed too — the rule stated at `src/Lower.cpp:2465-2470` and the pattern
   at `:28929-28948`.
5. **Emit** the entry call to `stmt.firstProduction`. `firstProduction == nullptr`
   (an empty sequence, or a failed lookup slang already diagnosed) emits nothing.
6. **Memoize per AST node**, keyed on the statement alone: a production takes
   everything it reads as a formal, so its body does not depend on which enclosing
   body generated the block — only the call-site actuals do.

`is_task` is set when the enclosing body is a coroutine (`fn_ctx_ == nullptr`, i.e.
a process body, or `fn_ctx_->is_task`). It is uniform across the whole production
set, because `TaskCall` is legal only inside a coroutine (`src/ir/Ir.h:1685-1689`)
and productions call each other. Inside a plain function, productions are plain
functions and a timing control in a code block is refused by the guards that already
exist for functions — correct, since LRM 13.4 forbids it there anyway. Measured
working: a randsequence in a `task automatic` whose production code block does `#5`
advances time normally.

---

## 6. Production ABI

```
randseq.<name>( [this,] output bit $abort [, output T $result]
                [, <declared formals…>] [, <captures…>] )
```

- **`this`** is spliced exactly as `splice_this` does for a method, when the block
  reads a class member. slang's lookup already walks *through* `RandSeqProduction`
  scopes to find the enclosing method (`source/ast/Lookup.cpp:1011-1016`), so a
  production body can name class members and `this`; `ForkCaptureDetector` reports
  the enclosing `thisVar` for a bare property reference and it is pulled out of the
  capture list and passed by value as formal 0.
- **`$abort`** is the `break` flag. `ArgDir::Out`: the callee writes its own frame
  slot and the backend's copy-out epilogue delivers it into the caller's actual,
  which is the caller's *own* `$abort` slot. So one store propagates one frame per
  return, with no shared state and nothing to allocate per activation. It is
  explicitly cleared at each production's entry, because an output formal is not
  seeded from the caller.
- **`$result`** is present only for a non-void production, also `ArgDir::Out`. A
  production is a *task* in the common case and Clotho tasks return void, so one ABI
  for both shapes is worth more than an IR-level `Ret`.
- **declared formals** come straight from `RandSeqProductionSymbol::arguments` and
  keep their declared directions, so `build_call_args` marshals them unchanged,
  including defaults (LRM 18.17.7's `gen(string s = "done")`).
- **captures** are `ArgDir::Ref` over the enclosing frame's symbols the block reads,
  in `ForkCaptureDetector`'s first-reference order. In a process body the list is
  empty: a module signal resolves directly from any body.

Every production call site is therefore:

```
  <evaluate the ProdItem's arguments>              // build_call_args, left to right
  call/task_call randseq.<target>(…, &$abort, &<result temp>, args…, caps…)
  <copy the result temp into this rule's variable>
  if ($abort) { RetVoid }                          // dropped at the top-level call
```

The `$abort` test after every call is the whole cost of `break`: one load through a
frame slot and a branch marked unlikely.

**At the randsequence statement itself** there is no frame above to unwind to, so
both output actuals are `make_arg_temp` cells nothing reads and no abort test is
emitted. That is what removed the entry wrapper the plan called for.

---

## 7. Rule selection and weights (LRM 18.17.1)

Per production activation:

- **One rule.** No draw, no weight evaluation. `getRules()[0]` is generated directly.
- **N rules.** `gen_weighted_rules` evaluates every rule's `weightExpr` in source
  order, exactly once, each self-determined, defaulting to a literal `1` where
  `weightExpr == nullptr`. Then the `lower_randcase` body verbatim: unsigned wrapping
  sum at the widest weight's precision, `sum != 0` guard, one `urandom_range(sum-1,
  0)`, and a cumulative ladder with the rule body in place of randcase's statement.
  Weights wider than 32 bits get the same loud refusal randcase gives.

The common width folds in **32 for every omitted weight**, not the one-bit minimum:
the implicit weight is the integer literal 1, and at one bit two of them sum to zero,
which the zero-total guard then reads as "generate nothing". That was a real bug
during bring-up — `first : add | dec` produced neither.

LRM: "Weight expressions are evaluated when their enclosing production is selected,
thus allowing weights to change dynamically" — so the evaluation happens inside the
production function, on every activation, not once at build time. Test:
*randsequence weights are re-evaluated per activation*.

A rule's generation order is: its prods left to right, then `Rule::codeBlock` (the
optional block after the weight clause). That order is forced by the LRM's own
`GenQueue` example, whose weight-clause block reads `ITEM`, the result of a
production in the same rule's list.

---

## 8. The five prod kinds

| `ProdKind` | lowering |
|---|---|
| `Item` | evaluate args (`build_call_args`), call, copy `$result` into the rule's slot for this occurrence (§10), test `$abort` |
| `CodeBlock` | `lower_stmt(*cb.block->tryGetStatement(), b)`. Already bound by slang's `createRule`, so `tryGetStatement()` and not `getStatement()`. Its declarations are ordinary `VariableDeclStatement`s and take the existing `declare_storage` path, which is what makes 18.17's "anonymous automatic scope" fall out |
| `IfElse` | `lower_predicate` on the condition once, `b.cond` over two regions, each generating one `ProdItem` |
| `Repeat` | the count into a `make_temp_cell`, then `b.loop` over a region generating the one `ProdItem`. **`loop_depth_` is deliberately not raised** — 18.17.4: "The repeat production statement itself cannot be terminated prematurely. A break statement will terminate the entire randsequence block" |
| `Case` | one common compare type over the selector and every label (as `common_case_type` computes for a procedural case), the selector evaluated once, then a first-match `CaseEq` chain; multi-label items `Bor` their compares. `defaultItem` is the else; absent default with no match generates nothing (18.17.3). `lower_case` itself is not reusable — its arm body is a `Statement`, not a production |

---

## 9. `break` and `return` (LRM 18.17.6)

`return` is nearly free: a production is its own `ir::Function`, and `Ret`/`RetVoid`
is a real terminator that escapes any depth of `Cond`/`Loop`/`Switch` regions
(`src/ir/Ir.h:1669-1676`). `lower_return` gained one branch: inside a production it
stores the value into the `$result` output formal and returns void.

`break` needed two pieces of new frontend state, because Clotho kept **none**
(`Break` was one line, `b.break_(loc(stmt))`, resolved in the backend against
`loop_targets_.back()`):

- `uint32_t loop_depth_` on the lowerer, raised around every user loop
  (`lower_for` / `while` / `do_while` / `forever` / `repeat` / `foreach`) with
  `Restore<>`, and reset to 0 by `FnScope` — a `break` in a body cannot bind to a
  loop outside it.
- `FnCtx::in_randseq` plus the `$abort` slot index.

`lower_break` then reads: if `loop_depth_ == 0` and we are in a production body,
store 1 into `$abort` and `RetVoid`; otherwise `b.break_()` as before. That is
precisely 18.17.6's "terminates the smallest enclosing looping statement; otherwise,
it terminates the randsequence block".

This also closed a latent hazard: the backend's `Break` arm comments "slang
guarantees a break appears only inside a loop" and indexes `loop_targets_.back()`
unconditionally. A randsequence `break` is the counter-example, and reaching that
arm with an empty vector is undefined behaviour.

---

## 10. Value passing (LRM 18.17.7)

slang synthesizes the implicit result variables and puts them in the rule block's
scope. It does **not** record which syntactic occurrence writes which array element,
so `count_rule_items` replays `createRuleVariables`' counting walk
(`MemberSymbols.cpp:2170-2203`) over `rule.prods`:

- `ProdItem` → 1
- `CodeBlock` → 0
- `IfElse` → `ifItem`, then `elseItem`
- `Repeat` → its item counts **1**, not the repeat count. LRM 18.17.7 Example 2's
  `A1 : { cnt = 1; } B repeat(5) C B { … }` has implicit variables `int B[1:2]; int
  C;` — `C` is scalar, every iteration overwrites the one slot, and the readable
  value is the last
- `Case` → each case item once, including `default`, and a multi-label item once,
  not once per label

The case walk goes through the **syntax** (`RsCaseSyntax`, zipped with `rule.prods`),
because a `default` item's position among the standard ones is not recoverable from
`CaseProd` — it is held apart in `defaultItem` while `createRuleVariables` counts it
where it was written.

Per rule, `gen_rule` then declares every `CompilerGenerated` `VariableSymbol` member
of `rule.ruleBlock` through **`lower_var_decl`**, not bare `declare_storage`: the
type default it writes (Table 6-7) is what a slot for a production the rule did not
generate reads back. 18.17.7 calls that value "undefined", and the alternative — the
frame's previous contents — is not reproducible across runs. `if (cond) D(5) else
D(20)` therefore reads `d1=0 d2=20` and `d1=5 d2=0` deterministically.

The value itself travels through a caller temp: an `output` actual is a raw slot the
callee writes through, and a `[1:N]` element is a composed lvalue, not a slot. So
`gen_prod_item` passes `make_arg_temp` and copies it into `result_slot(...)` after
the call — the same temp-then-copy-back shape `build_call_args` uses for every
ordinary output argument. `elem_select` indexes in declared-range coordinates, which
`[1:N]` matches directly.

**The return type is unrestricted.** A scalar, string, enum, real or class handle is
a `Store` with a `coerce`; an unpacked aggregate is `emit_agg_store`; a queue,
dynamic array or associative array is `emit_container_store`, the blocking half of
`lower_dyn_array_assign` / `lower_queue_assign` / `lower_assoc_assign` split out for
exactly this. Splitting rather than re-deriving matters: those functions dispatch on
the RHS *expression* shape (`new[n]`, `'{…}` patterns, a dynarr source into a queue,
a streaming concatenation), and a second partial copy of that table beside the real
one is the thing that rots. The copy back out of the caller temp is place-to-place
and needs no dispatch at all (`emit_result_copy`).

(`emit_handle_member_copy`, the neighbouring DECLARATION-INITIALIZER path, is
deliberately left alone: it treats `new[N]` as construction rather than assignment
and refuses the pattern and conditional sources these accept, so the two are not
interchangeable. Folding them together is a separate change.)

---

## 11. `rand join` (LRM 18.17.5)

The parser guarantees every operand of a `rand join` rule is a plain `RsProdItem`
(`RandJoinProdItem`) and that there are at least two (`RandJoinNotEnough`). "At each
step, the generator interleaves nonterminal symbols to depth of 1" fixes the
interleaved units: for operand `S_i`, they are the top-level prods of the rule `S_i`
selects, plus that rule's trailing weight code block when it has one. Each unit is
generated atomically.

Interleaving means each operand must be advanced one unit at a time, which a single
`randseq.<name>` entry point cannot do — it runs the production to completion. Each
operand production therefore gets two more entry points, built only for productions
actually named as operands:

```
randseq.<name>.plan( [this,] output int $rule, output int $len,
                     <formals…>, <captures…> )
randseq.<name>.step( [this,] output bit $abort [, output T $result],
                     input int $rule, input int $pos, output bit $stop,
                     <formals…>, <captures…> )
```

`plan` performs the production's rule draw — through the same `gen_weighted_rules`,
so an interleaved production consumes the same draws in the same order as a plain
one — and reports the chosen rule and its unit count; `$rule` is `-1` and `$len` is
`0` when the weights summed to zero. `step` is a `Cond` chain on `$rule` and then on
`$pos`; both are small syntactic counts, so a chain costs less than a jump table's
key packing. `$stop` reports that the production executed `return`, which in a step
body ends only that unit — `lower_return` raises the flag so the driver drops the
sequence's remaining units, which is 18.17.6's "aborts the generation of the current
production".

The driver, emitted inline in the rule that owns the `rand join`:

1. Evaluate each operand's arguments **once** into temps and replay them at every
   call — "passing data to a production is similar to a task call" (18.17.7).
2. Call `plan` for every operand, left to right; set `pos_i = 0`.
3. Evaluate the bias expression once (see below) into `X`.
4. `while (any nleft_i > 0)`: with `Lmax = max nleft_i`, form
   `w_i = (1024 − X)·(Lmax + 1 − nleft_i) + X·nleft_i` for eligible operands and 0
   otherwise, run the cumulative ladder over one `urandom_range` draw to pick `i`,
   call `step_i(rule_i, pos_i)`, then `pos_i++` and `nleft_i--` — or `nleft_i = 0`
   when `stop_i` came back set. Test `$abort`.
5. A non-void operand's `$result` is copied into the enclosing rule's variable
   exactly when `stop_i` is set, i.e. when the production actually returned a value.

**The bias weight.** LRM 18.17.5 constrains the expression to a real in [0.0, 1.0]
and pins exactly three points: 0.0 favours the shortest remaining sequence, 1.0 the
longest, and 0.5 "does not prioritize any sequence length"; unspecified means 0.5.
The expression is evaluated once, multiplied by 1024, truncated and clamped to
`[0, 1024]`. The weight above hits all three anchors — at `X = 0` it is proportional
to `Lmax + 1 − L_i`, at `X = 1024` to `L_i`, and at `X = 512` it collapses to the
constant `512·(Lmax + 1)`, i.e. uniform over the eligible operands — is monotone in
`X` between them, and is integer-only, so the pick reuses the same ladder every other
weighted choice here uses. `w_i` is at least 1 for an eligible operand at every `X`,
so the total can only be zero when none is eligible, which the loop condition already
tests; no zero-total guard is needed and no draw is wasted.

Measured over 4000 trials of the LRM example: `(0.0)` puts 68% of outcomes on the two
blocked orders `ABCD` / `CDAB`, `(1.0)` puts 68% on the four interleaved ones, `(0.5)`
sits between. **Do not copy verilator here**: its driver re-picks the current operand
with fixed probability ½ per step and ignores the expression entirely.

**The activation record.** `step` is a function of its own, entered once per unit,
so *nothing a production holds across its units may live in its frame*. Everything
that does is hoisted into the driver's frame — which spans the whole interleaving —
and handed back in as `ArgDir::Ref` formals of both `plan` and `step`:

- **the declared formals.** One cell each, set up once by `bind_rand_join_formal`:
  `input` and `inout` copy the actual in, `output` takes the type default, and a
  `ref` formal aliases the actual itself (its lvalue materialized once here, so an
  index expression inside it is evaluated once). One copy-out per `output` / `inout`
  runs after the last unit, through the ordinary `emit_call_copyouts`. That is what
  "similar to a task call" means for a production whose activation spans an
  interleaving: the copies happen at its ends, not at every unit — and an `input`
  the body mutates keeps the mutation between units, exactly as it would in a plain
  production's single call.
- **the rule variables.** One cell per (rule, variable) over every rule of the
  production, in `rand_join_rule_vars` order, default-initialized once before the
  first unit. `build_production_step` registers each symbol against its ref formal
  with `fn_local`, so `value_lvalue` resolves a reference to one straight to the
  driver's cell and `gen_rule`'s ordinary machinery needs no special case at all.

**The one thing that cannot be hoisted** is an operand that is itself a `rand join`:
its state is not a fixed set of variables but an interleaving cursor per inner
operand, chosen at its own plan time, so the outer `step` would have to be a
resumable driver rather than an indexed one. Refused by name (§14), and 18.17.5's
"interleaves nonterminal symbols to depth of 1" does not describe nesting.

---

## 12. Random stability and the draw contract

Nothing new was needed. `randsequence` draws are `b.urandom_range`, which puts the
enclosing body into `ir_draws_rng` automatically; the per-process xoshiro256++ state,
the fork-child reseed, and the caller's-stream-through-ABI-arg-1 rule all apply
unchanged (`docs/design/constrained-randomization.md` §9,
`src/ir/LlvmBackend.cpp:5148-5153`, `:6091-6097`, `:5859-5861`). LRM 18.14.2 lists
`randsequence` among the constructs whose selections are independent of thread
execution order, and emitting `urandom_range` is exactly what delivers that.

**Draw counts are observable and are part of the contract:**

| situation | draws |
|---|---|
| production with one rule | 0 |
| production with N ≥ 2 rules, total weight ≠ 0 | 1 |
| production with N ≥ 2 rules, total weight == 0 | **0**, and no rule is generated |
| `plan` of an interleaved production | the same as one activation of it |
| `rand join` | 1 per generated unit, on top of each operand's `plan` |
| `if` / `case` / `repeat` production statements | 0 of their own |

**Evaluation order**, per production activation: rule weights (source order) → the
draw → the selected rule's prods left to right → for each `ProdItem`, its arguments
left to right, then the call → `Rule::codeBlock`.

---

## 13. Two slang defects, fixed by patch

Both are in the vendored slang (`CMakeLists.txt`, `v10.0-360-g6933832c9`) and both
silently hand the frontend the wrong AST rather than diagnosing.
`patches/slang-randseq-rule-pairing.patch` fixes both;
`cmake/ApplySlangPatch.cmake` now globs and applies every `patches/slang-*.patch` in
name order, so adding a patch is one file and no build-system edit.

**(a) `createCaseProd` mis-paired case items after a multi-label item.**
`source/ast/symbols/MemberSymbols.cpp:2013-2019` pushed one `ProdItem` copy **per
label expression** while `:2047-2056` consumed **one per item**, so `prodIt`
desynchronized. For the LRM 18.17.3 example `case (device & 7) 0 : NETWORK; 1, 2 :
DISK; default : MEMORY; endcase` the item after the multi-label one picks up the
previous item's production. slang's own tests never exercise the shape: the one
multi-label case in `tests/unittests/ast/SerializerTests.cpp:70-75` is the only
standard item present. verilator gets the example right, so the divergence would
have been Clotho's. Test: *randsequence case production statement (LRM 18.17.3)*.

**(b) The weight-clause code block was created once per prod.**
`source/ast/symbols/BlockSymbols.cpp:257-270` added it *inside* the prods loop, while
`createRule` consumes rule blocks positionally through a single `blockIt`. For
`X : { a = 1; } { b = 2; } := 5 { c = 3; } | Y ;` the creation order was
`[{a}, {c}, {b}, {c}]`, pairing `CodeBlockProd` #2 with `{c = 3;}` and
`Rule::codeBlock` with `{b = 2;}` — silently swapped — while `{c = 3;}` was bound
twice, duplicating any diagnostic inside it. Rules with at most one code-block prod
were unaffected, which is why it went unnoticed.

Also worth an upstream note, not fixed here because nothing in this design depends
on it: `MemberSymbols.cpp:2145` sets `stmtCtx.flags = StatementFlags::InRandSeq` by
**assignment**, clearing `InLoop`/`InForkJoin`. A `continue` in a production code
block nested in a loop is therefore wrongly rejected, and a `return` in a production
inside a `fork` escapes the `ReturnInParallel` check.

---

## 14. The refusal surface

| construct | message | site | why still refused |
|---|---|---|---|
| a rule weight wider than 32 bits | `randsequence production weight wider than 32 bits is not supported` | `gen_weighted_rules` | `$urandom_range` is one 32-bit draw; LRM 18.16 wants several stitched together. `randcase` carries the identical guard under the same clause, so the two must change together — and stitching shifts how many values a selection takes from the process stream, which §12 makes a contract |
| a `rand join` operand that is itself a `rand join` | `a randsequence rand join operand that is itself a rand join is not supported` | `check_rand_join_operand` | every other thing an interleaved production holds across its units is a fixed set of variables the driver hoists (§11); an inner `rand join`'s state is an interleaving cursor per inner operand, chosen at its own plan time, so the outer `step` would have to be a resumable driver rather than an indexed one. 18.17.5 interleaves "to depth of 1", which does not describe nesting |

Both are deferrals recorded at the guard, not gaps found later. Everything else in
18.17 is implemented: a production may return any type including a container, an
interleaved production may pass values within its own rules and take arguments of
any direction, and a block may share an automatic with a detached fork.

---

## 15. Rejected alternatives

**Inline the grammar into the enclosing body (verilator's model).** Rejected: it
cannot express recursion, and the LRM's own 18.17.7 `GenQueue` example is
left-recursive. verilator's `%Error-UNSUPPORTED: randsequence production function
variable` and its inability to compile that example are the direct evidence.

**Lower the whole randsequence to a runtime interpreter over a grammar table.**
Rejected: code blocks are arbitrary SV statements including timing controls and class
method calls, so the "table" would have to hold JIT'd thunks anyway; and it would put
a runtime call on every production step, where the function-per-production model puts
a direct call the LLVM inliner can flatten for the common non-recursive case.

**Use `NamedBlock` + `Disable` for `break`.** Rejected: `Disable` binds *lexically*
within one unit (`src/ir/Ir.h:1710-1714`), and a `break` in a production body has to
unwind through an arbitrary chain of production calls.

**Pass `$abort` and `$result` as `ref` formals, with a synthetic entry wrapper owning
the flag.** This is what the plan called for. Rejected once written: `output` gives
the same propagation for free through the backend's existing copy-out epilogue, the
caller's own slot is the callee's actual, and nothing above the randsequence
statement ever reads the flag — so the wrapper had no work to do.

**Give `randsequence` its own RNG.** Rejected: it would need its own storage, its own
membership in the analysis seed set, and it would perturb `--emit llvm` output for
designs that use none. LRM 18.17 defines no generator of its own; 18.14.2 puts
randsequence on the thread's stream.

**Model non-void productions with `Ret` instead of an output result slot.** Rejected:
productions are tasks whenever the randsequence sits in a coroutine, and Clotho tasks
return void. Two ABIs keyed on whether the enclosing body can suspend is more surface
than one output formal costs.

**Bias `rand join` with `w_i = L_i^(2X−1)`.** The mathematically natural form, and it
hits the same three anchors. Rejected on cost: it needs `pow` on every step of every
interleave, for a distribution the LRM does not pin between the anchors.

**Inline each operand's units into the driver instead of a `step` function.** It
would keep the rule variables alive in the driver's frame, which is what §11's
refusals are about. Rejected: `return` inside an inlined operand would return from
the *caller*, which is the wrong production, and the fix for that is the flag
machinery a separate function already gives.

---

## 16. Phased plan

**P0 — accurate refusal.** *(landed, then superseded by P1.)* The
`case StatementKind::RandSequence:` arm, replacing the anonymous
`fail("unsupported statement")`.

**P1 — the core.** *(landed.)* Production→`ir::Function` with the §6 ABI, ids
reserved for the whole set, rule selection with weights, all five prod kinds,
`break`/`return`, recursion, `this` splicing, enclosing-frame captures, production
arguments. `loop_depth_` and `FnCtx::in_randseq` added to the lowerer.
*Acceptance:* the 18.17 opening example, the 18.17.1 weight ratio, the 18.17.3 case
example including the multi-label item, 18.17.2, 18.17.4, and the 18.17.6 `break`
and `return` examples byte-for-byte. All met.

**P2 — value passing (18.17.7).** *(landed.)* Non-void productions, the `$result`
output formal, the implicit rule variables and the occurrence-index re-derivation,
`lower_return` storing through `$result`. *Acceptance:* both LRM 18.17.7 examples,
`GenQueue`, and the default-on-unwritten-slot rule for the `if/else` shape. All met.

**P3 — `rand join` (18.17.5).** *(landed.)* The `plan` / `step` split, the driver,
and the bias weighting. *Acceptance:* the six legal interleavings of the LRM example
all occur and nothing else does; the bias anchors are measurably distinct; `return`
and `break` inside an interleaved operand behave; three operands work. All met.

**P4 — the remaining refusals.** *(landed.)* P0-P3 left six named refusals; four
were mechanism gaps rather than semantics and are now implemented. A production may
return a container (`emit_container_store`, split out of the three whole-container
assignment paths, §10); an interleaved production may pass values within its own
rules and take arguments of any direction (the activation record, §11); and a block
may share an automatic with a detached fork (the box handle travels by value, §6).
The two that remain are recorded as deliberate deferrals at their guards (§14).
*Acceptance:* queue / dynamic-array / associative-array return types including a
`[1:N]` result array; result variables inside an interleaved operand's rule;
`output`, `inout`, `ref` and mutated-`input` formals on interleaved operands, each
with its copy landing exactly once; a randsequence and a `join_none` child sharing
one automatic. All met.

**Prerequisite, landed first:** `patches/slang-randseq-rule-pairing.patch` (§13).

---

## 17. Verification

`tests/randsequence_test.cpp` — 30 `TEST_CASE`s, registered by one line in the
explicit TU list in `CMakeLists.txt` beside `tests/randcase_test.cpp`;
`doctest_discover_tests` needs no change. The two anonymous-namespace helpers every
random-adjacent TU carries (`run_capture`, `lower_error`) are copied from
`tests/randcase_test.cpp`, per that file's own convention.

Determinism without a `--seed` flag uses the three instruments named in
`docs/design/constrained-randomization.md:660-670`:

1. **Grammars with one outcome.** The 18.17.6 `break` and `return` examples, the
   18.17.2 if-else example, `repeat`, the case example, both value-passing examples,
   and every P4 case are fully determined; their expected stdout is the LRM's own
   text, or arithmetic the bias pins.
2. **Compute the property in the design.** The weight ratios, the interleaving set,
   and the bias anchors run thousands of trials and `$display` a boolean bound.
3. **Membership, not equality**, where the grammar admits several utterances (the
   opening example, the argument-default example): the design checks `inside` a set
   and prints `ok`.

Suite result: **2577 passing, 0 failing** (2547 before this work; 2 skipped),
including `tests/randcase_test.cpp` unchanged — which is what confirms both the
shared weighted-pick refactor into `gen_weighted_rules` and P4's split of the
container-assignment paths left the existing behaviour byte-identical.

Still owed: nothing in 18.17. Both refusals of §14 are covered by a `lower_error`
case asserting the message by substring.

---

## 18. LRM decisions this design records

- **A single-rule production takes no draw and evaluates no weight.** 18.17.1 makes a
  weight meaningful only among alternatives; draw counts are observable, so this is a
  contract, not an optimization.
- **An omitted weight is the integer literal 1, at 32-bit precision**, and folds into
  the common weight width like any other summand. Not cosmetic: at the one-bit
  minimum two omitted weights sum to zero.
- **A production whose rule weights sum to zero generates nothing and consumes no
  draw.** 18.17 does not cover the case; this follows the `randcase` precedent
  (18.16), which the LRM does cover.
- **Weights are unsigned and wrap at the widest weight's self-determined precision.**
  Inherited from the randcase decision; 18.16's "each summand is unsigned" is the
  closest text 18.17.1 has.
- **A `repeat` production statement's synthesized loop is not a `break` target.**
  18.17.4 says so explicitly, and it is why `loop_depth_` is not raised for it.
- **A result slot that was not written reads the type default (Table 6-7).** 18.17.7
  calls it "undefined"; the default is a defined choice and the alternative is
  unreproducible frame contents.
- **A repeated production writes one result slot, not an array.** 18.17.7 Example 2
  declares `int C;` scalar for `repeat(5) C`; the readable value is the last
  iteration's.
- **A `case` production with no match and no default generates nothing.** 18.17.3.
- **A `case` production's labels are compared lazily, first match wins.** 18.17.3 says
  the item expressions are "evaluated and compared in the order in which they are
  given"; a procedural `case` in Clotho stops at the first match and this matches it.
- **`rand join` interleaves a rule's prods and its trailing weight code block.** The
  clause says "nonterminal symbols to depth of 1" and does not classify the code
  block; treating it as the sequence's last unit preserves the one invariant the
  clause does state, that each sequence keeps its own relative order.
- **`rand join` bias uses the linear anchor interpolation of §11.** The LRM pins only
  X = 0.0, 0.5 and 1.0; the form chosen hits all three exactly and is monotone
  between them. verilator's behaviour (bias ignored) is a non-oracle here and is not
  matched.
- **An out-of-range `rand join` bias expression is clamped.** slang only requires a
  numeric type and 18.17.5 gives no behaviour for one outside [0.0, 1.0]; clamping
  keeps the weights non-negative, which the ladder needs.
- **An interleaved production's arguments copy at the ends of its ACTIVATION, not at
  each unit.** 18.17.7 makes passing data to a production "similar to a task call"
  and 18.17.5 says nothing about where a copy-out lands in an interleaving; the
  activation is the natural unit, so an `input` is copied in once and keeps what the
  body did to it between units, and an `output` / `inout` copies back exactly once
  after the last unit — identical to what the same production does when called
  plainly.
- **No recursion-depth cap.** 18.17's own `GenQueue` example recurses; Clotho caps no
  other SV recursion and the native stack is the limit, as it is for a recursive
  `function`.
