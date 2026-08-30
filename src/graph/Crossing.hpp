#pragma once

#include "rt/TimelineFifo.hpp"
#include "rt/ValueSlot.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace avc::graph {

struct StreamState {
    bool exact = true;
    bool discontinuity = false;
};

struct Crossing {
    rt::TimelineFifo timeline;
    rt::ValueSlot value;
    bool streaming = true;

    std::uint32_t type = 0;

    std::uint32_t src_domain = 0;
    std::uint32_t src_slot = 0;
    std::uint32_t dst_domain = 0;
    std::uint32_t dst_slot = 0;

    const std::byte *src = nullptr;
    std::byte *dst = nullptr;
    const StreamState *src_state = nullptr;
    StreamState *dst_state = nullptr;

    std::uint32_t latency = 0;

    std::atomic<std::uint64_t> underruns{0};
    std::atomic<std::uint64_t> overruns{0};
};

}
