#include "node/basic/MeterNode.hpp"

#include "common.hpp"

#include <cmath>
#include <cstring>

namespace avc::node::basic {

NodeDescriptor MeterNode::descriptor()
{
    NodeDescriptor d;
    d.type = "meter";
    d.category = "basic";
    d.label = "Meter";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    return d;
}

void MeterNode::prepare(const PrepareInfo & ) {}

void MeterNode::setParam(std::uint32_t , float ) noexcept {}

// ZFW: HOT PATH
void MeterNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];

    if (AVC_UNLIKELY(in == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        rms_.store(0.0F, std::memory_order_relaxed);
        return;
    }

    float peak = 0.0F;
    float sum_sq = 0.0F;
    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        const float sample = in[f];
        out[f] = sample;
        peak = std::fmax(peak, std::fabs(sample));
        sum_sq += sample * sample;
    }

    if (peak > peak_.load(std::memory_order_relaxed)) {
        peak_.store(peak, std::memory_order_relaxed);
    }
    rms_.store(std::sqrt(sum_sq / static_cast<float>(ctx.nframes)), std::memory_order_relaxed);
}

}
