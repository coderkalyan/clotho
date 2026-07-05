#pragma once

// Z-plane liveness ("z elision") analysis. Proves, per module signal, that the
// signal can never carry a high-impedance (z) bit, letting the backend drop the
// z word from the signal's state-struct slot and materialize a constant-zero z
// plane at each load instead.

#include <vector>

namespace clotho::ir {

class Module;

// One bit per Signal handle: true when the signal MAY carry a nonzero z plane.
// A false bit is a conservative proof of never-z (monotone may-analysis over
// the whole module, iterated to fixpoint).
//
// The exhaustive z-source seed set lives in ZElision.cpp (constants with z
// bits, $readmem into 4-state memories). Any new z-producing construct (tri
// nets, bufif-style primitives, force/release of z, ...) MUST seed there or
// elision becomes unsound.
std::vector<bool> compute_may_z(const Module& m);

} // namespace clotho::ir
