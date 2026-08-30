#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"
#include "rt/SmoothedParam.hpp"

#include <cstdint>

namespace avc::node::debug {

class SignalNode final : public Node {
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
        kWaveform = 0,
        kFreq = 1,
        kLevelDb = 2,
    };

    enum Waveform : std::uint32_t {
        kSine = 0,
        kSquare,
        kSaw,
        kNoise,
        kImpulse,
        kSilence,
    };

    // ZFW: HOT PATH
    float nextNoise() noexcept;

    rt::SmoothedParam level_;
    rt::SmoothedParam freq_;
    Waveform waveform_ = kSine;
    float phase_ = 0.0F;
    std::uint32_t rng_ = 0x9E3779B9U;
    std::uint32_t sample_rate_ = types::kDefaultSampleRate;
};

}
