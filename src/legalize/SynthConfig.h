#pragma once

// SynthConfig: the knobs for the synthesis path, the way SimConfig is the knobs for
// simulation. One field per decision that is genuinely contested; `{}` is the
// sensible thing.

#include <cstdint>
#include <string_view>

namespace clotho::legalize {

// Which language the writer must emit.
//
// Not a formatting preference: 2005 has no `logic`, no wildcard equality and no
// `break`, so the target changes what can be spelled and what has to be declined.
enum class Target : uint8_t {
    // IEEE 1364-2005. The widest downstream reach.
    V2005,
    // IEEE 1800. Everything the IR can express.
    SV2017,
};

// How much of the instance tree the output keeps.
enum class Hierarchy : uint8_t {
    // One module, hierarchical paths mangled into identifiers. Fast, correct, and
    // unable to carry a constraint file -- the test oracle rather than the product.
    Flat,
    // One module per instance, port maps reconstructed from the copy processes the
    // frontend leaves behind, instance and signal names exactly as written. What a
    // downstream flow can actually consume, because SDC and physical scripts key on
    // hierarchical paths. Definitions are per-instance until they are deduplicated
    // by content, so a module instantiated twice is emitted twice.
    Instances,
};

struct SynthConfig {
    Target    target    = Target::SV2017;
    Hierarchy hierarchy = Hierarchy::Flat;

    // Rebuild each written signal's mux tree from its guarded list and write out
    // the arms the source left implicit. Requires a Result; without one the writer
    // translates 1:1 whatever this says.
    bool materialize_mux_trees = true;

    // Module name to emit. Empty renders `m`.
    std::string_view name;

    uint32_t indent_width = 2;
};

} // namespace clotho::legalize
