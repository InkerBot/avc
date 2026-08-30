#include "rt/ValueSlot.hpp"

namespace avc::rt {

void ValueSlot::reset(std::uint32_t bytes)
{
    bytes_ = bytes;
    data_.assign(static_cast<std::size_t>(bytes) * 3, std::byte{0});
    // Block 0 is published and already taken, so the first read finds nothing
    // new and the consumer keeps whatever it started with -- zeroes.
    latest_.store(0, std::memory_order_relaxed);
    write_index_ = 1;
    read_index_ = 2;
}

}
