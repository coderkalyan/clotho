#pragma once

// Twin mode's check for the suite (docs/VERIFICATION.md, "Shared code"): every
// design a test lowers is lowered again with each top instantiated twice, and the
// backend checks the code the instances of a unit share against each instance's
// own lowering. A mismatch throws, so the test that compiled the design fails.

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "Lower.h"
#include "ir/LlvmBackend.h"

namespace clotho::test {

inline TwinVerdict suite_twin_check(const LowerResult& twin) {
    const ir::SharedCodeCheck check = ir::check_shared_code(*twin.module, loc_resolver(twin));
    if (check.outcome == ir::SharedCodeCheck::Outcome::Mismatch) {
        throw std::runtime_error("twin mode: the design instantiated twice shares code its instances do not "
                                 "lower alike\n" + check.message);
    }
    if (check.outcome == ir::SharedCodeCheck::Outcome::Refused && std::getenv("CLOTHO_TWIN_VERBOSE") != nullptr) {
        std::fprintf(stderr, "twin refused: %s\n", check.message.c_str());
    }
    return check.outcome == ir::SharedCodeCheck::Outcome::Ok ? TwinVerdict::Checked : TwinVerdict::Refused;
}

} // namespace clotho::test
