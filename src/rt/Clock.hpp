#pragma once

#include <chrono>
#include <cstdint>

namespace avc::rt {

// ZFW: HOT PATH
inline std::uint64_t monoNs() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}
