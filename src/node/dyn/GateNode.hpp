#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"

#include <cstdint>

namespace avc::node::dyn {

class GateNode final : public Node {
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
        kThresholdDb = 0,
        kAttackMs = 1,
        kHoldMs = 2,
        kReleaseMs = 3,
        kFloorDb = 4,
    };

    float coeffFor(float ms) const noexcept;

    void retime() noexcept;

    float threshold_ = 0.0F;
    float floor_ = 0.0F;
    float attack_ms_ = 2.0F;
    float release_ms_ = 250.0F;
    float hold_ms_ = 120.0F;
    float attack_ = 1.0F;
    float release_ = 1.0F;
    std::uint32_t hold_samples_ = 0;

    float level_ = 0.0F;
    float gain_ = 0.0F;
    std::uint32_t held_ = 0;
    float level_decay_ = 0.0F;
    std::uint32_t sample_rate_ = types::kDefaultSampleRate;
};

}
