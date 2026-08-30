#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"
#include "rt/SmoothedParam.hpp"

namespace avc::node::basic {

class GainNode final : public Node {
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
        kGainDb = 0,
        kMute = 1,
    };

    rt::SmoothedParam gain_;
    float mute_ = 0.0F;
};

}
