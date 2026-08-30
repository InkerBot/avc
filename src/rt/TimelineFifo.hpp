#pragma once

#include "common.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace avc::rt {

struct TimelineRead {
    bool exact = false;
    bool discontinuity = false;
};

class TimelineFifo {
public:
    void reset(std::uint32_t frames, std::uint32_t frame_bytes);

    void prefill(std::uint64_t start_frame, std::uint32_t frames);

    bool write(const std::byte *src, std::uint64_t start_frame, std::uint32_t frames,
               bool discontinuity = false) noexcept;

    void publishGap(std::uint64_t start_frame, std::uint32_t frames) noexcept;

    TimelineRead read(std::byte *dst, std::uint64_t start_frame,
                      std::uint32_t frames) noexcept;

    void expire(std::uint64_t start_frame, std::uint32_t frames) noexcept;
    void expireUntil(std::uint64_t end_frame) noexcept;

    bool canResolve(std::uint64_t start_frame, std::uint32_t frames) const noexcept;

    bool canWrite(std::uint64_t start_frame, std::uint32_t frames) const noexcept;

    std::uint32_t capacityFrames() const noexcept { return capacity_; }
    std::uint32_t frameBytes() const noexcept { return frame_bytes_; }
    std::size_t storageBytes() const noexcept
    {
        return data_.size() + positions_.size() * sizeof(std::uint64_t)
               + discontinuities_.size();
    }

    std::uint64_t requestedUntil() const noexcept
    {
        return requested_until_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kCacheLine = 64;

    void copyIn(const std::byte *src, std::uint32_t at, std::uint32_t frames) noexcept;
    void copyOut(std::byte *dst, std::uint32_t at, std::uint32_t frames) const noexcept;
    void discardBefore(std::uint64_t frame, std::uint64_t write) noexcept;

    alignas(kCacheLine) std::atomic<std::uint64_t> write_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> read_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> published_until_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> requested_until_{0};

    std::vector<std::byte> data_;
    std::vector<std::uint64_t> positions_;
    std::vector<std::uint8_t> discontinuities_;
    std::uint32_t capacity_ = 0;
    std::uint32_t mask_ = 0;
    std::uint32_t frame_bytes_ = 0;
};

}
