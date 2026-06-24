#include "Runtime.h"

#include <print>

extern "C" void rt_hello() {
    std::print("hello from the clotho runtime\n");
}
