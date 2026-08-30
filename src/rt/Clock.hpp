#pragma once

#include <cstdint>
#include <ctime>

namespace avc::rt {

// ZFW: HOT PATH
inline std::uint64_t monoNs() noexcept
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL
           + static_cast<std::uint64_t>(ts.tv_nsec);
}

}
