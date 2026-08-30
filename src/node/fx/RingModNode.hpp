#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"
#include "rt/SmoothedParam.hpp"

#include <cstdint>

namespace avc::node::fx {

class RingModNode final : public Node {
public:
    static NodeDescriptor descriptor();

    void prepare(const PrepareInfo &info) override;
    void inherit(const Node &previous) override;

    // ZFW: HOT PATH
    void setParam(std::uint32_t index, float value) noexcept override;

    // ZFW: HOT PATH
    void process(const NodeContext &ctx) noexcept override;

private:
    enum Param : std::uint32_t {
        kFreq = 0,
        kMix = 1,
    };

    rt::SmoothedParam freq_;
    float mix_ = 1.0F;
    float phase_ = 0.0F;
    std::uint32_t sample_rate_ = types::kDefaultSampleRate;
};

}
