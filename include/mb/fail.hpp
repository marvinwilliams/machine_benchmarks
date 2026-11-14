#pragma once

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace mb {

// Reports a failed system call and aborts. Such failures are not expected during a benchmark and are not recovered
// from.
[[noreturn]] inline void fail(char const* what, int error) {
    std::cerr << "Error: " << what << ": " << std::strerror(error) << std::endl;
    std::abort();
}

}  // namespace mb
