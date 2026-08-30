#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"
#include "rt/SmoothedParam.hpp"

#include <atomic>
#include <vector>

namespace avc::node::voice {

class PitchNode final : public Node {
public:
    static NodeDescriptor descriptor();

    void prepare(const PrepareInfo &info) override;
    void inherit(const Node &previous) override;

    std::uint32_t latencyFrames() const noexcept override
    {
        return latency_.load(std::memory_order_relaxed);
    }

    // ZFW: HOT PATH
    void setParam(std::uint32_t index, float value) noexcept override;

    // ZFW: HOT PATH
    void process(const NodeContext &ctx) noexcept override;

private:
    enum Param : std::uint32_t {
        kSemitones = 0,
        kGrainMs = 1,
        kMix = 2,
    };

    // ZFW: HOT PATH
    float readAt(float delay) const noexcept;

    void republishLatency() noexcept;

    std::vector<types::Sample> buffer_;
    std::uint32_t write_ = 0;
    float distance_ = 0.0F;

    float ratio_ = 1.0F;
    float grain_ = 0.0F;
    float grain_ms_ = 40.0F;
    float semitones_ = 0.0F;
    float mix_ = 1.0F;
    std::uint32_t sample_rate_ = types::kDefaultSampleRate;

    rt::SmoothedParam engaged_;

    std::atomic<std::uint32_t> latency_{0};
};

}
