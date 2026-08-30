#include "rt/TimelineFifo.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace avc::rt {
namespace {

std::uint32_t roundUpPow2(std::uint32_t value)
{
    std::uint32_t size = 2;
    while (size < value && size <= (std::numeric_limits<std::uint32_t>::max() >> 1U)) {
        size <<= 1U;
    }
    return size;
}

}

void TimelineFifo::reset(std::uint32_t frames, std::uint32_t frame_bytes)
{
    capacity_ = roundUpPow2(std::max(frames, 2U));
    mask_ = capacity_ - 1;
    frame_bytes_ = frame_bytes;
    data_.assign(static_cast<std::size_t>(capacity_) * frame_bytes_, std::byte{0});
    positions_.assign(capacity_, 0);
    discontinuities_.assign(capacity_, 0);
    write_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
    published_until_.store(0, std::memory_order_relaxed);
    requested_until_.store(0, std::memory_order_relaxed);
}

void TimelineFifo::copyIn(const std::byte *src, std::uint32_t at,
                          std::uint32_t frames) noexcept
{
    const std::uint32_t first = std::min(frames, capacity_ - at);
    const std::size_t first_bytes = static_cast<std::size_t>(first) * frame_bytes_;
    const std::size_t rest_bytes = static_cast<std::size_t>(frames - first) * frame_bytes_;
    std::byte *const first_dst = data_.data() + static_cast<std::size_t>(at) * frame_bytes_;
    if (src != nullptr) {
        std::memcpy(first_dst, src, first_bytes);
        if (rest_bytes > 0) std::memcpy(data_.data(), src + first_bytes, rest_bytes);
    } else {
        std::memset(first_dst, 0, first_bytes);
        if (rest_bytes > 0) std::memset(data_.data(), 0, rest_bytes);
    }
}

void TimelineFifo::copyOut(std::byte *dst, std::uint32_t at,
                           std::uint32_t frames) const noexcept
{
    const std::uint32_t first = std::min(frames, capacity_ - at);
    const std::size_t first_bytes = static_cast<std::size_t>(first) * frame_bytes_;
    const std::size_t rest_bytes = static_cast<std::size_t>(frames - first) * frame_bytes_;
    const std::byte *const first_src =
        data_.data() + static_cast<std::size_t>(at) * frame_bytes_;
    std::memcpy(dst, first_src, first_bytes);
    if (rest_bytes > 0) std::memcpy(dst + first_bytes, data_.data(), rest_bytes);
}

void TimelineFifo::prefill(std::uint64_t start_frame, std::uint32_t frames)
{
    (void)write(nullptr, start_frame, std::min(frames, capacity_), false);
}

bool TimelineFifo::write(const std::byte *src, std::uint64_t start_frame,
                         std::uint32_t frames, bool discontinuity) noexcept
{
    if (AVC_UNLIKELY(frames == 0 || frames > capacity_ || frame_bytes_ == 0)) {
        return false;
    }
    const std::uint64_t end_frame = start_frame + frames;

    // Publish the producer's progress even when the payload cannot be stored.
    // That turns a dropped write into a definite gap instead of a consumer
    // waiting forever for bytes that will never arrive.
    const std::uint64_t previous_horizon = published_until_.load(std::memory_order_relaxed);
    if (AVC_UNLIKELY(start_frame < previous_horizon)) {
        return false;
    }

    if (AVC_UNLIKELY(end_frame <= requested_until_.load(std::memory_order_acquire))) {
        published_until_.store(end_frame, std::memory_order_release);
        return false;
    }

    const std::uint64_t write = write_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_.load(std::memory_order_acquire);
    if (AVC_UNLIKELY(frames > capacity_ - static_cast<std::uint32_t>(write - read))) {
        published_until_.store(end_frame, std::memory_order_release);
        return false;
    }

    const std::uint32_t at = static_cast<std::uint32_t>(write) & mask_;
    copyIn(src, at, frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint32_t slot = (at + i) & mask_;
        positions_[slot] = start_frame + i;
        discontinuities_[slot] = discontinuity ? 1U : 0U;
    }
    write_.store(write + frames, std::memory_order_release);
    published_until_.store(end_frame, std::memory_order_release);
    return true;
}

void TimelineFifo::publishGap(std::uint64_t start_frame, std::uint32_t frames) noexcept
{
    const std::uint64_t horizon = published_until_.load(std::memory_order_relaxed);
    if (start_frame >= horizon) {
        published_until_.store(start_frame + frames, std::memory_order_release);
    }
}

bool TimelineFifo::canResolve(std::uint64_t start_frame, std::uint32_t frames) const noexcept
{
    return published_until_.load(std::memory_order_acquire) >= start_frame + frames;
}

bool TimelineFifo::canWrite(std::uint64_t start_frame, std::uint32_t frames) const noexcept
{
    if (frames == 0 || frames > capacity_
        || start_frame + frames <= requested_until_.load(std::memory_order_acquire)) {
        return false;
    }
    const std::uint64_t write = write_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_.load(std::memory_order_acquire);
    return frames <= capacity_ - static_cast<std::uint32_t>(write - read);
}

void TimelineFifo::discardBefore(std::uint64_t frame, std::uint64_t write) noexcept
{
    std::uint64_t read = read_.load(std::memory_order_relaxed);
    while (read < write && positions_[static_cast<std::uint32_t>(read) & mask_] < frame) {
        ++read;
    }
    read_.store(read, std::memory_order_release);
}

TimelineRead TimelineFifo::read(std::byte *dst, std::uint64_t start_frame,
                                std::uint32_t frames) noexcept
{
    TimelineRead result;
    if (AVC_UNLIKELY(dst == nullptr || frames == 0 || frame_bytes_ == 0)) return result;

    const std::uint64_t end_frame = start_frame + frames;
    requested_until_.store(end_frame, std::memory_order_release);

    std::uint64_t write = write_.load(std::memory_order_acquire);
    discardBefore(start_frame, write);
    std::uint64_t read = read_.load(std::memory_order_relaxed);
    write = write_.load(std::memory_order_acquire);

    const bool enough = write - read >= frames;
    const std::uint32_t at = static_cast<std::uint32_t>(read) & mask_;
    const bool exact = enough && positions_[at] == start_frame
                       && positions_[(at + frames - 1U) & mask_] == end_frame - 1U;
    if (AVC_LIKELY(exact)) {
        copyOut(dst, at, frames);
        for (std::uint32_t i = 0; i < frames; ++i) {
            result.discontinuity = result.discontinuity
                                   || discontinuities_[(at + i) & mask_] != 0;
        }
        read_.store(read + frames, std::memory_order_release);
        result.exact = true;
        return result;
    }

    // The deadline has passed. Consume all stored payload older than its end;
    // a future write for this interval will see requested_until_ and be refused.
    discardBefore(end_frame, write);
    std::memset(dst, 0, static_cast<std::size_t>(frames) * frame_bytes_);
    result.discontinuity = true;
    return result;
}

void TimelineFifo::expire(std::uint64_t start_frame, std::uint32_t frames) noexcept
{
    expireUntil(start_frame + frames);
}

void TimelineFifo::expireUntil(std::uint64_t end_frame) noexcept
{
    requested_until_.store(end_frame, std::memory_order_release);
    const std::uint64_t write = write_.load(std::memory_order_acquire);
    discardBefore(end_frame, write);
}

}
