#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"

#include <atomic>

namespace avc::node::basic {

class MeterNode final : public Node {
public:
    static NodeDescriptor descriptor();

    void prepare(const PrepareInfo &info) override;
    void setParam(std::uint32_t index, float value) noexcept override;

    // ZFW: HOT PATH
    void process(const NodeContext &ctx) noexcept override;

    float takePeak() noexcept { return peak_.exchange(0.0F, std::memory_order_relaxed); }
    float rms() const noexcept { return rms_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> peak_{0.0F};
    std::atomic<float> rms_{0.0F};
};

}
