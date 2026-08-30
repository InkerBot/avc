#pragma once

#include <cstdint>
#include <vector>

namespace avc::graph {

class BufferPool {
public:
    std::uint32_t acquire();
    void release(std::uint32_t slot);

    std::uint32_t highWater() const noexcept { return next_; }

private:
    std::vector<std::uint32_t> free_;
    std::uint32_t next_ = 0;
};

}
