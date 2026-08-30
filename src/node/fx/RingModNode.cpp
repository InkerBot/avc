#include "node/fx/RingModNode.hpp"

#include "common.hpp"

#include <cmath>
#include <cstring>

namespace avc::node::fx {
namespace {

constexpr float kTwoPi = 6.28318530717958647692F;

}

NodeDescriptor RingModNode::descriptor()
{
    NodeDescriptor d;
    d.type = "ringmod";
    d.category = "fx";
    d.label = "Ring modulator";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    d.params = {
        {"freq", ParamType::Float, 10.0F, 2000.0F, 50.0F, "Hz", ParamCurve::Logarithmic, {},
         "Low is a rasp, around 30-80 Hz is the classic robot, high is a bell.", ""},
        {"mix", ParamType::Float, 0.0F, 1.0F, 1.0F, "", ParamCurve::Linear, {},
         "Dry to modulated. Halfway keeps the words intelligible.", ""},
    };
    return d;
}

void RingModNode::prepare(const PrepareInfo &info)
{
    sample_rate_ = info.sample_rate;
    freq_.configure(0.1F);
    freq_.snap(freq_.target());
    phase_ = 0.0F;
}

void RingModNode::inherit(const Node &previous)
{
    // Carrying the phase over is what keeps a graph swap from putting a step in
    // the middle of the carrier.
    const auto &other = static_cast<const RingModNode &>(previous);
    phase_ = other.phase_;
    freq_.snap(other.freq_.current());
}

// ZFW: HOT PATH
void RingModNode::setParam(std::uint32_t index, float value) noexcept
{
    if (index == kFreq) {
        freq_.setTarget(value);
    } else if (index == kMix) {
        mix_ = value;
    }
}

// ZFW: HOT PATH
void RingModNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];

    if (AVC_UNLIKELY(in == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }

    const float step = kTwoPi * freq_.nextBlock() / static_cast<float>(sample_rate_);
    const float dry = 1.0F - mix_;

    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        const float x = in[f];
        out[f] = x * dry + x * std::sin(phase_) * mix_;

        phase_ += step;
        if (phase_ >= kTwoPi) {
            phase_ -= kTwoPi;
        }
    }
}

}
