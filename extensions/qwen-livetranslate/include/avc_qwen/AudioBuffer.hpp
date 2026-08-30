#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace avc::qwen {

class PcmSpscBuffer {
public:
    void reset(std::size_t requested_capacity)
    {
        std::size_t capacity = 2;
        while (capacity < requested_capacity) capacity <<= 1U;
        data_.assign(capacity, 0);
        mask_ = capacity - 1;
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

    std::size_t capacity() const noexcept { return data_.size(); }

    std::size_t readable() const noexcept
    {
        return static_cast<std::size_t>(write_.load(std::memory_order_acquire)
                                        - read_.load(std::memory_order_relaxed));
    }

    std::size_t writable() const noexcept { return capacity() - readableForProducer(); }

    std::size_t write(std::span<const std::int16_t> samples) noexcept
    {
        const std::uint64_t write = write_.load(std::memory_order_relaxed);
        const std::size_t room = capacity() - static_cast<std::size_t>(
                                                  write
                                                  - read_.load(std::memory_order_acquire));
        const std::size_t count = std::min(room, samples.size());
        const std::size_t at = static_cast<std::size_t>(write) & mask_;
        const std::size_t first = std::min(count, capacity() - at);
        std::copy_n(samples.data(), first, data_.data() + at);
        if (first < count) std::copy_n(samples.data() + first, count - first, data_.data());
        write_.store(write + count, std::memory_order_release);
        return count;
    }

    bool push(std::int16_t sample) noexcept
    {
        return write(std::span<const std::int16_t>(&sample, 1)) == 1;
    }

    std::size_t read(std::span<std::int16_t> samples) noexcept
    {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::size_t ready = static_cast<std::size_t>(
            write_.load(std::memory_order_acquire) - read);
        const std::size_t count = std::min(ready, samples.size());
        const std::size_t at = static_cast<std::size_t>(read) & mask_;
        const std::size_t first = std::min(count, capacity() - at);
        std::copy_n(data_.data() + at, first, samples.data());
        if (first < count) std::copy_n(data_.data(), count - first, samples.data() + first);
        read_.store(read + count, std::memory_order_release);
        return count;
    }

    bool pop(std::int16_t &sample) noexcept
    {
        return read(std::span<std::int16_t>(&sample, 1)) == 1;
    }

    // Only the consumer may call this method.
    void discardAll() noexcept
    {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::size_t readableForProducer() const noexcept
    {
        return static_cast<std::size_t>(write_.load(std::memory_order_relaxed)
                                        - read_.load(std::memory_order_acquire));
    }

    static constexpr std::size_t kCacheLine = 64;
    alignas(kCacheLine) std::atomic<std::uint64_t> write_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> read_{0};
    std::vector<std::int16_t> data_;
    std::size_t mask_ = 0;
};

}
