#include "node/voice/FormantNode.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avc::node::voice {
namespace {

constexpr float kBandQ = 3.5F;

constexpr float kMaxGainDb = 24.0F;

constexpr float kEnvelopeMs = 20.0F;

constexpr float kTotalSemitones = 72.0F;

}

NodeDescriptor FormantNode::descriptor()
{
    NodeDescriptor d;
    d.type = "formant";
    d.category = "voice";
    d.label = "Formant shift";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    d.params = {
        {"shift", ParamType::Float, -12.0F, 12.0F, 0.0F, "st", ParamCurve::Linear, {},
         "Moves the vocal tract, not the pitch. Up sounds smaller and younger, "
         "down sounds larger. Pair it with a pitch node to change who is speaking "
         "rather than how fast the tape is running.", ""},
        {"mix", ParamType::Float, 0.0F, 1.0F, 1.0F, "", ParamCurve::Linear, {},
         "How much of the correction to apply.", ""},
    };
    return d;
}

void FormantNode::prepare(const PrepareInfo &info)
{
    const float rate = static_cast<float>(info.sample_rate);
    const float step = kTotalSemitones / static_cast<float>(kBands - 1);
    bands_per_semitone_ = 1.0F / step;

    for (std::uint32_t j = 0; j < kBands; ++j) {
        const float centre = kLowHz * std::pow(2.0F, step * static_cast<float>(j) / 12.0F);
        bands_[j].setCoeffs(dsp::design(dsp::FilterType::BandPass, centre, kBandQ, 0.0F,
                                        info.sample_rate));
        bands_[j].reset();
        env_[j] = 0.0F;
        gain_[j] = 1.0F;
    }

    env_coeff_ = 1.0F - std::exp(-1.0F / (rate * kEnvelopeMs * 0.001F));
}

void FormantNode::inherit(const Node &previous)
{
    // Envelopes take 20 ms to refill and the gains ramp from wherever they are.
    // Losing them across a swap is a short but audible smear on a held vowel.
    const auto &other = static_cast<const FormantNode &>(previous);
    env_ = other.env_;
    gain_ = other.gain_;
}

// ZFW: HOT PATH
void FormantNode::setParam(std::uint32_t index, float value) noexcept
{
    if (index == kShift) {
        shift_ = value;
    } else if (index == kMix) {
        mix_ = value;
    }
}

// ZFW: HOT PATH
void FormantNode::updateGains() noexcept
{
    const float max_gain = std::pow(10.0F, kMaxGainDb / 20.0F);
    const float min_gain = 1.0F / max_gain;
    const float delta = shift_ * bands_per_semitone_;
    const float last = static_cast<float>(kBands - 1);

    for (std::uint32_t j = 0; j < kBands; ++j) {
        const float source = std::clamp(static_cast<float>(j) - delta, 0.0F, last);
        const auto lower = static_cast<std::uint32_t>(source);
        const std::uint32_t upper = lower + 1 < kBands ? lower + 1 : lower;
        const float frac = source - static_cast<float>(lower);
        const float wanted = env_[lower] + frac * (env_[upper] - env_[lower]);

        // The floor is what keeps a silent band from being multiplied up into
        // whatever noise it happens to hold. It is added to both sides, so that
        // a band asked to carry its own level lands on exactly 1 however quiet
        // it is -- which is what makes a shift of zero bit-for-bit transparent
        // rather than nearly transparent.
        const float target = std::clamp((wanted + 1e-5F) / (env_[j] + 1e-5F), min_gain, max_gain);
        gain_[j] += (target - gain_[j]) * 0.25F;
    }
}

// ZFW: HOT PATH
void FormantNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];

    if (AVC_UNLIKELY(in == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }

    updateGains();

    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        const float x = in[f];
        float correction = 0.0F;

        for (std::uint32_t j = 0; j < kBands; ++j) {
            const float band = bands_[j].tick(x);
            env_[j] += (std::fabs(band) - env_[j]) * env_coeff_;
            correction += band * (gain_[j] - 1.0F);
        }
        out[f] = x + correction * mix_;
    }
}

}
