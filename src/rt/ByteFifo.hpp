#pragma once

#include "common.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace avc::rt {

class ByteFifo {
public:
    void reset(std::uint32_t bytes);

    void prefill(std::uint32_t bytes);

    std::uint32_t capacity() const noexcept { return capacity_; }

    // ZFW: HOT PATH -- consumer side
    std::uint32_t readable() const noexcept
    {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
    }

    // ZFW: HOT PATH -- producer side
    std::uint32_t writable() const noexcept
    {
        return capacity_ - (write_.load(std::memory_order_relaxed)
                            - read_.load(std::memory_order_acquire));
    }

    // ZFW: HOT PATH
    bool write(const std::byte *src, std::uint32_t bytes) noexcept
    {
        if (AVC_UNLIKELY(bytes > writable())) {
            return false;
        }
        const std::uint32_t at = write_.load(std::memory_order_relaxed) & mask_;
        const std::uint32_t first = bytes < capacity_ - at ? bytes : capacity_ - at;
        std::memcpy(data_.data() + at, src, first);
        if (first < bytes) {
            std::memcpy(data_.data(), src + first, bytes - first);
        }
        write_.fetch_add(bytes, std::memory_order_release);
        return true;
    }

    // ZFW: HOT PATH
    bool read(std::byte *dst, std::uint32_t bytes) noexcept
    {
        if (AVC_UNLIKELY(bytes > readable())) {
            return false;
        }
        const std::uint32_t at = read_.load(std::memory_order_relaxed) & mask_;
        const std::uint32_t first = bytes < capacity_ - at ? bytes : capacity_ - at;
        std::memcpy(dst, data_.data() + at, first);
        if (first < bytes) {
            std::memcpy(dst + first, data_.data(), bytes - first);
        }
        read_.fetch_add(bytes, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t kCacheLine = 64;

    alignas(kCacheLine) std::atomic<std::uint32_t> write_{0};
    alignas(kCacheLine) std::atomic<std::uint32_t> read_{0};
    std::vector<std::byte> data_;
    std::uint32_t capacity_ = 0;
    std::uint32_t mask_ = 0;
};

}
