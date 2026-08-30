#pragma once

#include "dsp/Biquad.hpp"
#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"

#include <array>
#include <cstdint>

namespace avc::node::voice {

class FormantNode final : public Node {
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
        kShift = 0,
        kMix = 1,
    };

    static constexpr std::uint32_t kBands = 20;
    static constexpr float kLowHz = 110.0F;
    static constexpr float kHighHz = 7040.0F;

    // ZFW: HOT PATH
    void updateGains() noexcept;

    std::array<dsp::Biquad, kBands> bands_{};
    std::array<float, kBands> env_{};
    std::array<float, kBands> gain_{};

    float shift_ = 0.0F;
    float mix_ = 1.0F;
    float env_coeff_ = 0.0F;
    float bands_per_semitone_ = 0.0F;
};

}
