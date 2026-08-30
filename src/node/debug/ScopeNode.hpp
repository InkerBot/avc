#pragma once

#include "dsp/Biquad.hpp"
#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"
#include "rt/SpscRing.hpp"

#include <array>
#include <cstdint>

namespace avc::node::debug {

inline constexpr std::uint32_t kScopePoints = 128;
inline constexpr std::uint32_t kScopeBands = 24;

struct ScopeFrame {
    std::array<float, kScopePoints> wave{};
    std::array<float, kScopeBands> bands{};
};

class ScopeNode final : public Node {
public:
    static NodeDescriptor descriptor();

    void prepare(const PrepareInfo &info) override;

    // ZFW: HOT PATH
    void setParam(std::uint32_t index, float value) noexcept override;

    // ZFW: HOT PATH
    void process(const NodeContext &ctx) noexcept override;

    bool takeFrame(ScopeFrame &out) noexcept;

    static float bandCentre(std::uint32_t index) noexcept;

private:
    enum Param : std::uint32_t {
        kWindowMs = 0,
    };

    static constexpr float kLowHz = 40.0F;
    static constexpr float kHighHz = 16000.0F;

    static constexpr float kBandQ = 2.5F;

    std::array<dsp::Biquad, kScopeBands> bands_{};
    std::array<float, kScopeBands> energy_{};
    ScopeFrame building_{};

    rt::SpscRing<ScopeFrame, 4> frames_;

    float bucket_peak_ = 0.0F;
    std::uint32_t bucket_fill_ = 0;
    std::uint32_t point_ = 0;
    std::uint32_t bucket_samples_ = 8;
    float window_ms_ = 40.0F;
    std::uint32_t sample_rate_ = types::kDefaultSampleRate;
};

}
