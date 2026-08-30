#pragma once

#include "dsp/Biquad.hpp"
#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"

#include <cstdint>

namespace avc::node::eq {

class BiquadNode final : public Node {
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
        kType = 0,
        kFreq = 1,
        kQ = 2,
        kGainDb = 3,
    };

    // ZFW: HOT PATH
    void redesign() noexcept;

    dsp::Biquad filter_;
    dsp::FilterType type_ = dsp::FilterType::Peaking;
    float freq_ = 1000.0F;
    float q_ = 0.707F;
    float gain_db_ = 0.0F;
    bool dirty_ = true;
    std::uint32_t sample_rate_ = types::kDefaultSampleRate;
};

}
