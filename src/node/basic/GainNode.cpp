#include "node/basic/GainNode.hpp"

#include "common.hpp"

#include <cmath>
#include <cstring>

namespace avc::node::basic {
namespace {

float dbToLinear(float db)
{
    return db <= -90.0F ? 0.0F : std::pow(10.0F, db / 20.0F);
}

}

NodeDescriptor GainNode::descriptor()
{
    NodeDescriptor d;
    d.type = "gain";
    d.category = "basic";
    d.label = "Gain";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    d.params = {
        {"gain_db", ParamType::Float, -90.0F, 24.0F, 0.0F, "dB", ParamCurve::Linear, {}},
        {"mute", ParamType::Bool, 0.0F, 1.0F, 0.0F, "", ParamCurve::Linear, {}},
    };
    return d;
}

void GainNode::prepare(const PrepareInfo & )
{
    gain_.snap(gain_.target());
}

void GainNode::inherit(const Node &previous)
{
    const auto &other = static_cast<const GainNode &>(previous);
    // Keep the ramp where it was so a graph swap mid-fade does not jump.
    gain_.snap(other.gain_.current());
}

// ZFW: HOT PATH
void GainNode::setParam(std::uint32_t index, float value) noexcept
{
    if (index == kGainDb) {
        gain_.setTarget(dbToLinear(value));
    } else if (index == kMute) {
        mute_ = value;
    }
}

// ZFW: HOT PATH
void GainNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];
    const float gain = gain_.nextBlock() * (mute_ >= 0.5F ? 0.0F : 1.0F);

    if (AVC_UNLIKELY(in == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }
    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        out[f] = in[f] * gain;
    }
}

}
