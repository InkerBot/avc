#pragma once

#include "common.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace avc::rt {

class ValueSlot {
public:
    void reset(std::uint32_t bytes);

    std::uint32_t bytes() const noexcept { return bytes_; }

    std::size_t storageBytes() const noexcept { return data_.size(); }

    // ZFW: HOT PATH
    void write(const std::byte *src) noexcept
    {
        if (AVC_UNLIKELY(bytes_ == 0)) {
            return;
        }
        std::memcpy(block(write_index_), src, bytes_);
        write_index_ = latest_.exchange(write_index_ | kFresh, std::memory_order_acq_rel) & kIndex;
    }

    // ZFW: HOT PATH
    bool read(std::byte *dst) noexcept
    {
        if (AVC_UNLIKELY(bytes_ == 0)
            || (latest_.load(std::memory_order_acquire) & kFresh) == 0) {
            return false;
        }
        read_index_ = latest_.exchange(read_index_, std::memory_order_acq_rel) & kIndex;
        std::memcpy(dst, block(read_index_), bytes_);
        return true;
    }

private:
    static constexpr std::uint32_t kFresh = 0x80000000U;
    static constexpr std::uint32_t kIndex = 0x7FFFFFFFU;

    std::byte *block(std::uint32_t index) noexcept
    {
        return data_.data() + static_cast<std::size_t>(index) * bytes_;
    }

    std::vector<std::byte> data_;
    std::uint32_t bytes_ = 0;

    std::atomic<std::uint32_t> latest_{0};

    std::uint32_t write_index_ = 1;
    std::uint32_t read_index_ = 2;
};

}
