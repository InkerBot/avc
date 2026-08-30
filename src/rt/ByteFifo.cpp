#include "rt/ByteFifo.hpp"

#include <algorithm>

namespace avc::rt {
namespace {

std::uint32_t roundUpPow2(std::uint32_t value)
{
    std::uint32_t size = 2;
    while (size < value) {
        size <<= 1U;
    }
    return size;
}

}

void ByteFifo::reset(std::uint32_t bytes)
{
    capacity_ = roundUpPow2(std::max(bytes, 2U));
    mask_ = capacity_ - 1;
    data_.assign(capacity_, std::byte{0});
    write_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
}

void ByteFifo::prefill(std::uint32_t bytes)
{
    const std::uint32_t count = std::min(bytes, writable());
    write_.fetch_add(count, std::memory_order_release);
}

}
