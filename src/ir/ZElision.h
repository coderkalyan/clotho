#pragma once

// Z-plane liveness ("z elision") analysis. Proves, per module signal, that the
// signal can never carry a high-impedance (z) bit, letting the backend drop the
// z word from the signal's state-struct slot and materialize a constant-zero z
// plane at each load instead.

#include <cstdint>
#include <vector>

namespace clotho::ir {

class Module;

// One bit per Signal handle: true when the signal has no driver anywhere in
// the design — nothing stores to it, no $readmem targets it, and no task
// out/inout copy-out writes it. An undriven 4-state net defaults to z
// (LRM 1800-2023 6.6: "The default initialization value for a net shall be the
// value z"); an undriven variable defaults to x.
std::vector<bool> compute_undriven(const Module& m);

// Per-signal undriven-bit mask for the power-on z default of a PARTIALLY-driven
// packed 4-state net. compute_undriven is whole-signal (a partial store like
// `w[3:0] = ...` marks all of `w` driven), so its undriven bits would power up x
// instead of z (LRM 6.6). This refines that: for a packed integral net that has
// at least one driver but leaves some bits undriven, entry[sig] holds the
// undriven-bit mask (LSB-first words, bit set == undriven == powers on z). The
// entry is empty for every other signal — fully undriven (compute_undriven
// handles it), fully driven, 2-state, unpacked-array, or non-net — so the common
// cases stay on the whole-signal path. Dynamic-index / unpacked-element / whole
// stores mark all bits driven (conservative: those bits keep the x default).
std::vector<std::vector<uint64_t>> compute_partial_undriven(const Module& m);

// One bit per Signal handle: true when the signal MAY carry a nonzero z plane.
// A false bit is a conservative proof of never-z (monotone may-analysis over
// the whole module, iterated to fixpoint).
//
// The exhaustive z-source seed set lives in ZElision.cpp (undriven 4-state
// nets, constants with z bits, $readmem into 4-state memories). Any new
// z-producing construct (bufif-style primitives, resolving/pulled net kinds,
// force/release of z, ...) MUST seed there or elision becomes unsound.
std::vector<bool> compute_may_z(const Module& m);

} // namespace clotho::ir
