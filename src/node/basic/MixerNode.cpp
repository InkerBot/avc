#include "node/basic/MixerNode.hpp"

#include "common.hpp"

#include <cstring>

namespace avc::node::basic {

NodeDescriptor MixerNode::descriptor()
{
    NodeDescriptor d;
    d.type = "mixer";
    d.category = "basic";
    d.label = "Mixer";
    d.inputs = {{"in_1"}, {"in_2"}};
    d.outputs = {{"out"}};
    d.dynamic_inputs = true;
    return d;
}

void MixerNode::prepare(const PrepareInfo & ) {}

void MixerNode::setParam(std::uint32_t , float ) noexcept {}

// ZFW: HOT PATH
void MixerNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    std::uint32_t first = 0;

    while (first < ctx.n_inputs && ctx.inputs[first] == nullptr) {
        ++first;
    }
    if (AVC_UNLIKELY(first == ctx.n_inputs)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }

    std::memcpy(out, ctx.inputs[first], ctx.nframes * sizeof(types::Sample));
    for (std::uint32_t i = first + 1; i < ctx.n_inputs; ++i) {
        const types::Sample *in = ctx.inputs[i];
        if (in == nullptr) {
            continue;
        }
        for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
            out[f] += in[f];
        }
    }
}

}
