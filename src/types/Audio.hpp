#pragma once

#include <cstdint>

namespace avc::types {

using Sample = float;

inline constexpr std::uint32_t kDefaultSampleRate = 48000;

inline constexpr std::uint32_t kDefaultQuantum = 128;

inline constexpr std::uint32_t kMaxQuantum = 8192;

inline constexpr std::uint32_t kDefaultColdBlock = 1024;
inline constexpr std::uint32_t kMinColdBlock = 64;
// Model domains may deliberately trade about half a second of buffering for
// CPU inference throughput. Hot-device quanta retain their much smaller cap.
inline constexpr std::uint32_t kMaxColdBlock = 32768;

inline constexpr std::uint32_t kDefaultColdSafety = 1;
inline constexpr std::uint32_t kMaxColdSafety = 8;

inline constexpr const char *kHotDomain = "hot";

struct AudioFormat {
    std::uint32_t sample_rate = kDefaultSampleRate;
    std::uint32_t quantum = kDefaultQuantum;
};

constexpr double quantumMs(const AudioFormat &format) noexcept
{
    return 1000.0 * static_cast<double>(format.quantum) / static_cast<double>(format.sample_rate);
}

}
