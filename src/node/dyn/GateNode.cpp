#include "node/dyn/GateNode.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avc::node::dyn {
namespace {

constexpr float kDetectorMs = 15.0F;

float dbToLinear(float db)
{
    return db <= -90.0F ? 0.0F : std::pow(10.0F, db / 20.0F);
}

}

NodeDescriptor GateNode::descriptor()
{
    NodeDescriptor d;
    d.type = "noise_gate";
    d.category = "dyn";
    d.label = "Noise gate";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    d.params = {
        {"threshold_db", ParamType::Float, -90.0F, 0.0F, -45.0F, "dB", ParamCurve::Linear, {},
         "Anything quieter than this is treated as room noise.", ""},
        {"attack_ms", ParamType::Float, 0.1F, 50.0F, 2.0F, "ms", ParamCurve::Logarithmic, {},
         "How fast the gate opens. Too slow and the first consonant is missing.", ""},
        {"hold_ms", ParamType::Float, 0.0F, 500.0F, 120.0F, "ms", ParamCurve::Linear, {},
         "How long it stays open after the level drops, so it does not chatter "
         "between syllables.", ""},
        {"release_ms", ParamType::Float, 5.0F, 2000.0F, 250.0F, "ms", ParamCurve::Logarithmic, {},
         "How gently it closes once the hold runs out.", ""},
        {"floor_db", ParamType::Float, -90.0F, 0.0F, -90.0F, "dB", ParamCurve::Linear, {},
         "What is left when it is shut. Not silence but -20 dB keeps the room "
         "there, which sounds less like a dropped connection.", ""},
    };
    return d;
}

float GateNode::coeffFor(float ms) const noexcept
{
    const float samples = std::max(ms, 0.01F) * 0.001F * static_cast<float>(sample_rate_);
    return 1.0F - std::exp(-1.0F / std::max(samples, 1.0F));
}

void GateNode::retime() noexcept
{
    attack_ = coeffFor(attack_ms_);
    release_ = coeffFor(release_ms_);
    hold_samples_ =
        static_cast<std::uint32_t>(hold_ms_ * 0.001F * static_cast<float>(sample_rate_));
}

void GateNode::prepare(const PrepareInfo &info)
{
    sample_rate_ = info.sample_rate;
    level_decay_ = coeffFor(kDetectorMs);
    retime();

    gain_ = 1.0F;
    level_ = 0.0F;
    held_ = 0;
}

void GateNode::inherit(const Node &previous)
{
    const auto &other = static_cast<const GateNode &>(previous);
    level_ = other.level_;
    gain_ = other.gain_;
    held_ = other.held_;
}

// ZFW: HOT PATH
void GateNode::setParam(std::uint32_t index, float value) noexcept
{
    switch (index) {
    case kThresholdDb: threshold_ = dbToLinear(value); return;
    case kAttackMs:    attack_ms_ = value; break;
    case kHoldMs:      hold_ms_ = value; break;
    case kReleaseMs:   release_ms_ = value; break;
    case kFloorDb:     floor_ = dbToLinear(value); return;
    default:           return;
    }
    retime();
}

// ZFW: HOT PATH
void GateNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];

    if (AVC_UNLIKELY(in == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }

    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        const float x = in[f];

        const float rectified = std::fabs(x);
        level_ = rectified > level_ ? rectified : level_ + (rectified - level_) * level_decay_;

        float target = floor_;
        if (level_ >= threshold_) {
            held_ = hold_samples_;
            target = 1.0F;
        } else if (held_ > 0) {
            --held_;
            target = 1.0F;
        }

        gain_ += (target - gain_) * (target > gain_ ? attack_ : release_);
        out[f] = x * gain_;
    }
}

}
