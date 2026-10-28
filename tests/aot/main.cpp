// The `main` a program linking a `build --emit obj` object supplies: hand the
// plusargs to rt_run and return the exit status it reports.

#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/Runtime.h"

extern "C" int32_t clotho_main(void);

int main(int argc, char** argv) {
    std::vector<const char*> plusargs;
    std::vector<uint32_t>    lens;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '+') {
            plusargs.push_back(argv[i] + 1);
            lens.push_back(static_cast<uint32_t>(std::strlen(argv[i] + 1)));
        }
    }
    const rt_run_config config = {
        .plusargs      = plusargs.data(),
        .plusarg_lens  = lens.data(),
        .plusarg_count = static_cast<uint32_t>(plusargs.size()),
    };
    return rt_run(&config, clotho_main);
}
