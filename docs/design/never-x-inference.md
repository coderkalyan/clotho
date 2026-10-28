# Never-X inference (2-state lowering for 4-state RTL)

Extend the landed 2-state path (`two-state.md`) to `logic`/`reg` designs by
**statically proving a 4-state signal can never carry X**, then lowering it as
2-state (narrow storage, native ops, `two`-valued loads). The may-plane analog
of z-elision (`ZElision.h`). Default-safe: only ever fires on a *proof* of
never-X, so it changes performance, never results.

Status: design / scoping. Not started. Read §6 (the uninitialized-register
crux) and §8 (honest coverage) before committing — the sound-and-automatic
version is coverage-limited on sequential RTL, and closing that gap is the hard
part.

---

## 1. Why

`bit`/sized-int designs already lower native, but most RTL is `logic`/`reg`
(4-state) and pays the full must/may cost — the duplicated may plane in storage
and the X-guard branching in every op (the `xor.known`/`xor.x` split we measured
costing ~10% on a single comb xor; more on arithmetic + register-file storage).
X-optimism (force everything 2-state, X/Z→0) would buy it cheaply but is a
gated semantic deviation and off the table. Never-X inference buys the same win
*soundly* wherever the design provably cannot produce X.

## 2. Soundness contract

Lowering a signal 2-state drops its may/z planes; if X or Z ever actually
reaches it, the value is silently read as its must bits (X→0) — a wrong answer,
not a crash. So the analysis must be **conservative**: any doubt ⇒ stay 4-state.

- A signal is **narrow-eligible** iff `!may_x[sig] && !may_z[sig]` (never-X *and*
  never-Z — z-elision already computes the latter).
- **Gate**: a `never_x` field in `SimulationConfig` disables it (a differential
  gate for the tests, not a CLI flag); the standing correctness test is that
  never-X-on and never-X-off produce **byte-identical** output on the whole
  corpus + the combinational fuzzer + picorv32. They must, because 2-state and
  4-state agree exactly when no X/Z occurs, and never-X only fires when none
  can. Any diff is an unsoundness bug (a mis-proven signal).

## 3. The analysis (mirror z-elision)

`compute_may_x(const Module&) -> std::vector<bool>`, structured exactly like
`compute_may_z` (`ZElision.cpp`): a monotone false→true bit per storage node
(module signals + each function's formals / frame locals / return), seeded by
X-sources, propagated bottom-up per instruction (flat store is in creation
order, operands precede users), stores fold the rhs bit into the lhs root, swept
to fixpoint so loop back-edges and cross-process flow ride the storage bits.
Reuse the `Analysis`/`FnState` scaffold verbatim; only the seed set (§4) and one
transfer rule (§5) differ. `may_x` and `may_z` can share one two-bit sweep.

## 4. X-source seeds (the taxonomy)

X enters through more constructs than Z. Seeds (any of these makes the value
may-x):

1. **Explicit X**: interned constants with an x bit (`4'bx01`, params folded from
   one); x-assignments. (Mirror `constant_has_z`, testing `xz & val` for the x
   half instead of `xz & ~val`.)
2. **Uninitialized variables** — *the seed z-elision does not have.* A `reg`/
   `logic` variable with no time-0 defined value holds X until first written
   (LRM: variable default is x, vs a net's z). A read that can observe that X is
   an x-source. This is temporal, not pure dataflow — see §6.
3. **Div / mod**: `a/0`, `a%0` → x (LRM 11.4.3). Conservatively seed every
   div/mod result may-x unless the divisor is provably nonzero (hard; v1 does
   not attempt it). Note the 2-state div path returns 0 on divide-by-zero, so a
   mis-lowered div would diverge — this seed is load-bearing.
4. **Out-of-range / x-indexed selects**: a bit/part-select whose index may be
   out of range or may be X reads x (LRM 11.5.1). v1: dynamic (variable-index)
   selects seed may-x; constant in-range selects propagate the operand.
5. **May-z operands in x-absorbing ops**: z read by any arithmetic/logic/compare
   op becomes x. Since `may_z` is already computed, fold it in: a may-z operand
   into such an op ⇒ may-x result. (This is why narrow-eligibility needs both.)
6. **Contention**: a multi-driver net resolves to x on conflict. v1: any signal
   with >1 structural driver seeds may-x (single-driver is the common case).
7. **External inputs**: top-level input ports are may-x (an external agent may
   drive x) unless the whole design is closed and the driver is proven never-x.

Any *new* X-producing construct (gate primitives, force/release, resolved net
kinds) MUST seed here or the analysis is unsound — same discipline as
`ZElision.cpp`'s seed comment.

## 5. Transfer function

Every value-preserving op (bitwise, arith, shift, concat, reduction, logical,
compare, mux) is never-x iff **all** its operands are never-x — standard forward
propagation, identical in shape to may-z's. The differences from may-z are the
*sources* in §4 (div/mod, oob-select, uninitialized) that inject x where z would
not appear; z-absorbing ops that clear z do **not** clear x.

## 6. The uninitialized-register crux (why this is hard)

Seed #2 is what limits coverage. Two shapes:

- **Combinational signals** (continuous assign / `always_comb`, fully assigned,
  no latch): recomputed from inputs every activation and settled at time-0
  (self-start), so they never observe a stale/uninit value of their own. They
  are never-x **iff their inputs are never-x** — pure dataflow, no temporal
  reasoning. These fall out of the fixpoint for free. (A comb *latch* — not
  fully assigned — holds state ⇒ treat like a register.)
- **Sequential registers** (`always @(posedge clk) q <= …`): X at time 0 until
  the first clocked write. A read before that observes X. So a register is
  never-x only if it cannot be read-while-X:
  - **v1 (sound, cheap)**: never-x iff it has a **time-0 initializer**
    (`logic q = 0`) *and* every driver is never-x. No initializer ⇒ may-x, and
    the x taints its entire fanout. This covers constants, params, initialized
    regs, and all comb fed by them — but **not** the uninitialized-until-reset
    registers that dominate real cores.
  - **v2 (sound, hard) — reset-before-read**: prove a register is assigned a
    *defined* value on reset before any functional read can observe X. Requires
    identifying the reset signal + reset branch, proving the reset dominates the
    first observing read in sim time (a temporal/reachability argument across
    the testbench, not just the DUT), and proving the reset value is defined.
    This is where the real sequential-RTL coverage lives and where the risk and
    effort concentrate. It may not be worth it vs a pragma
    (`/* clotho never_x */`) or accepting the v1 ceiling.

## 7. Integration with the backend

Small, because the 2-state machinery already exists:

- **Storage** (`build_state_layout`, ~`src/ir/llvm/`): today
  `slot.four_state = (domain == Four)`. Change to
  `slot.four_state = (domain == Four) && (may_x[sig] || may_z[sig])`, so a
  proven-clean `logic` slot becomes narrow (`{iW}`). The store into it already
  applies the must-plane projection; never-x guarantees must == may, so it is
  exact. Frame locals / NBA targets mirror.
- **Value** (loads, `type_of`): a load of a narrowed signal must produce a
  `two_val` (`Plane.two = true`) and report `ValType.four_state = false`. The
  existing per-instruction `four_state` recompute + operator `two`-propagation
  then carry it downstream natively with no further change — the same dispatch
  that makes `bit` fast.
- **Ports / ABI**: narrowing is per storage node; a narrowed signal crossing the
  (still-packed) call ABI lifts via `to_four` at the boundary as today.
- The `never_x` gate threads through `SimulationConfig` like `fuse` / `thunk`
  (config-only; the CLI carries no bisect flags).

No new runtime, no ABI change. The pass is one analysis file
(`compute_may_x`, ideally fused into `ZElision.cpp` as a two-bit sweep) plus the
one-line storage-eligibility change and the load `two`-tagging.

## 8. Coverage / value — honest

- **Comb-heavy / fully-initialized designs**: high coverage, the full 2-state
  win on the combinational cone (storage halved, native ops). This is the strong
  case.
- **Sequential-heavy RTL with uninitialized registers (picorv32, most cores)**:
  v1 coverage is low — the register file and control regs seed may-x, and their
  fanout stays 4-state. The win is modest until v2 (reset analysis) lands, and
  v2 is the hard, high-risk part. Do not expect a picorv32 number from v1.
- Net: v1 is sound, cheap, and default-on-able, but its win is design-dependent
  and probably small on the sequential cores we most want to speed up. Measure
  coverage (fraction of signals narrowed) on the target designs before assuming
  a win.

## 9. Effort / risk / phasing

| phase | scope | effort | risk |
|---|---|---|---|
| v1 | `compute_may_x` (seeds §4, transfer §5, initializer-based reg rule §6-v1) + storage/load integration + `--no-never-x` + differential/fuzzer/iverilog gauntlet | ~1 week | med — soundness is the whole game; a missed seed is a silent miscompile, so the seed audit + byte-identical differential are mandatory |
| v2 | reset-before-read analysis for uninitialized registers | ~2–4 weeks | high — temporal proof across DUT+testbench, reset identification; may be better served by a pragma |

Risk is asymmetric: a bug is a *silent wrong answer*, not a crash. The
differential (never-x-on vs `--no-never-x`, byte-identical on corpus + fuzzer +
picorv32) is the non-negotiable safety net, and every seed needs an adversarial
test that an X placed there does taint (i.e., the signal is *not* narrowed).

## 10. Recommendation

1. Build **v1** behind the `never_x` config gate, fused into the z-elision sweep. First
   deliverable is not speed but a **coverage report** (`--stat=never-x`: fraction of
   signals narrowed) on comb-heavy designs vs picorv32 — that number decides
   whether v2 is worth it.
2. If v1 coverage on the target (sequential) designs is low — the likely outcome
   — the win requires **v2 reset analysis**, whose cost/risk should be weighed
   against a `never_x` pragma (designer asserts it, we verify against the 4-state
   build in CI). A pragma gets most of v2's coverage at a fraction of the risk.
3. Sequence after the explicit-2-state residuals are closed (cheap) and
   independently of the scheduling work; never-X is orthogonal to clock-wave.
