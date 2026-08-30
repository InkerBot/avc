#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"

#include <atomic>
#include <cstdint>

namespace avc::node::dyn {

class CompressorNode final : public Node {
public:
    static NodeDescriptor descriptor();

    void prepare(const PrepareInfo &info) override;
    void inherit(const Node &previous) override;

    // ZFW: HOT PATH
    void setParam(std::uint32_t index, float value) noexcept override;

    // ZFW: HOT PATH
    void process(const NodeContext &ctx) noexcept override;

    float reductionDb() const noexcept { return reduction_.load(std::memory_order_relaxed); }

private:
    enum Param : std::uint32_t {
        kThresholdDb = 0,
        kRatio = 1,
        kKneeDb = 2,
        kAttackMs = 3,
        kReleaseMs = 4,
        kMakeupDb = 5,
    };

    void retime() noexcept;

    float threshold_ = -18.0F;
    float slope_ = 0.75F;
    float knee_ = 6.0F;
    float makeup_ = 0.0F;
    float attack_ms_ = 10.0F;
    float release_ms_ = 150.0F;
    float attack_ = 1.0F;
    float release_ = 1.0F;

    float env_db_ = 0.0F;
    std::atomic<float> reduction_{0.0F};
    std::uint32_t sample_rate_ = types::kDefaultSampleRate;
};

}
