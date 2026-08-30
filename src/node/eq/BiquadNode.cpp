#include "node/eq/BiquadNode.hpp"

#include "common.hpp"

#include <algorithm>
#include <cstring>

namespace avc::node::eq {

NodeDescriptor BiquadNode::descriptor()
{
    NodeDescriptor d;
    d.type = "biquad";
    d.category = "eq";
    d.label = "Filter";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    d.params = {
        {"type", ParamType::Enum, 0.0F, 6.0F, 4.0F, "", ParamCurve::Linear,
         {"lowpass", "highpass", "bandpass", "notch", "peaking", "lowshelf", "highshelf"},
         "", ""},
        {"freq", ParamType::Float, 20.0F, 18000.0F, 1000.0F, "Hz", ParamCurve::Logarithmic, {},
         "", ""},
        {"q", ParamType::Float, 0.1F, 10.0F, 0.707F, "", ParamCurve::Logarithmic, {},
         "How narrow. High values ring; 0.707 is as flat as a lowpass gets.", ""},
        {"gain_db", ParamType::Float, -24.0F, 24.0F, 0.0F, "dB", ParamCurve::Linear, {},
         "Only the peaking and shelf types use this.", ""},
    };
    return d;
}

void BiquadNode::prepare(const PrepareInfo &info)
{
    sample_rate_ = info.sample_rate;
    filter_.reset();
    dirty_ = true;
    redesign();
}

void BiquadNode::inherit(const Node &previous)
{
    (void)previous;
    // Deliberately nothing. Two state words at 48 kHz decay in well under a
    // millisecond, and copying them across a swap would be carrying the tail of
    // a filter that may no longer have the same coefficients.
}

// ZFW: HOT PATH
void BiquadNode::setParam(std::uint32_t index, float value) noexcept
{
    switch (index) {
    case kType:
        type_ = static_cast<dsp::FilterType>(
            std::clamp(static_cast<int>(value + 0.5F), 0, 6));
        break;
    case kFreq:   freq_ = value; break;
    case kQ:      q_ = value; break;
    case kGainDb: gain_db_ = value; break;
    default:      return;
    }
    dirty_ = true;
}

// ZFW: HOT PATH
void BiquadNode::redesign() noexcept
{
    if (AVC_LIKELY(!dirty_)) {
        return;
    }
    filter_.setCoeffs(dsp::design(type_, freq_, q_, gain_db_, sample_rate_));
    dirty_ = false;
}

// ZFW: HOT PATH
void BiquadNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];

    if (AVC_UNLIKELY(in == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        filter_.reset();
        return;
    }

    redesign();
    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        out[f] = filter_.tick(in[f]);
    }
}

}
