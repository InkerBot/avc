#include "node/debug/SignalNode.hpp"

#include "common.hpp"

#include <cmath>
#include <cstring>

namespace avc::node::debug {
namespace {

constexpr float kTwoPi = 6.28318530717958647692F;

float dbToLinear(float db)
{
    return db <= -90.0F ? 0.0F : std::pow(10.0F, db / 20.0F);
}

}

NodeDescriptor SignalNode::descriptor()
{
    NodeDescriptor d;
    d.type = "signal";
    d.category = "debug";
    d.label = "Signal generator";
    d.outputs = {{"out"}};
    d.params = {
        {"waveform", ParamType::Enum, 0.0F, 5.0F, 0.0F, "", ParamCurve::Linear,
         {"sine", "square", "saw", "noise", "impulse", "silence"},
         "A sine to hear a pitch shift on, noise to see a filter on, an impulse "
         "to measure a delay with.", ""},
        {"freq", ParamType::Float, 20.0F, 8000.0F, 220.0F, "Hz", ParamCurve::Logarithmic, {},
         "Ignored by noise and silence. For impulse it is the repeat rate.", ""},
        {"level_db", ParamType::Float, -90.0F, 0.0F, -12.0F, "dB", ParamCurve::Linear, {},
         "", ""},
    };
    return d;
}

void SignalNode::prepare(const PrepareInfo &info)
{
    sample_rate_ = info.sample_rate;
    level_.configure(0.15F);
    level_.snap(level_.target());
    freq_.configure(0.1F);
    freq_.snap(freq_.target());
    phase_ = 0.0F;
}

void SignalNode::inherit(const Node &previous)
{
    // Carrying the phase over means a graph swap does not put a step in the
    // middle of the tone the swap is being tested with.
    const auto &other = static_cast<const SignalNode &>(previous);
    phase_ = other.phase_;
    rng_ = other.rng_;
    level_.snap(other.level_.current());
    freq_.snap(other.freq_.current());
}

// ZFW: HOT PATH
void SignalNode::setParam(std::uint32_t index, float value) noexcept
{
    switch (index) {
    case kWaveform:
        waveform_ = static_cast<Waveform>(value < 0.0F ? 0 : static_cast<std::uint32_t>(value + 0.5F));
        break;
    case kFreq:    freq_.setTarget(value); break;
    case kLevelDb: level_.setTarget(dbToLinear(value)); break;
    default:       break;
    }
}

// ZFW: HOT PATH
float SignalNode::nextNoise() noexcept
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_) * (2.0F / 4294967296.0F) - 1.0F;
}

// ZFW: HOT PATH
void SignalNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const float level = level_.nextBlock();

    if (AVC_UNLIKELY(waveform_ == kSilence || level == 0.0F)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }

    const float step = freq_.nextBlock() / static_cast<float>(sample_rate_);

    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        float value = 0.0F;
        switch (waveform_) {
        case kSine:    value = std::sin(kTwoPi * phase_); break;
        case kSquare:  value = phase_ < 0.5F ? 1.0F : -1.0F; break;
        case kSaw:     value = 2.0F * phase_ - 1.0F; break;
        case kNoise:   value = nextNoise(); break;
        case kImpulse: value = phase_ < step ? 1.0F : 0.0F; break;
        case kSilence: break;
        }
        out[f] = value * level;

        phase_ += step;
        if (phase_ >= 1.0F) {
            phase_ -= 1.0F;
        }
    }
}

}
