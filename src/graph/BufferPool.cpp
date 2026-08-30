#include "graph/BufferPool.hpp"

namespace avc::graph {

std::uint32_t BufferPool::acquire()
{
    if (free_.empty()) {
        return next_++;
    }
    const std::uint32_t slot = free_.back();
    free_.pop_back();
    return slot;
}

void BufferPool::release(std::uint32_t slot)
{
    free_.push_back(slot);
}

}
