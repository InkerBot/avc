#include "node/basic/SplitterNode.hpp"

#include "common.hpp"

#include <cstring>

namespace avc::node::basic {

NodeDescriptor SplitterNode::descriptor()
{
    NodeDescriptor d;
    d.type = "splitter";
    d.category = "basic";
    d.label = "Splitter";
    d.inputs = {{"in"}};
    d.outputs = {{"out_1"}, {"out_2"}};
    d.dynamic_outputs = true;
    return d;
}

void SplitterNode::prepare(const PrepareInfo & ) {}

void SplitterNode::setParam(std::uint32_t , float ) noexcept {}

// ZFW: HOT PATH
void SplitterNode::process(const NodeContext &ctx) noexcept
{
    const types::Sample *in = ctx.inputs[0];
    const std::size_t bytes = ctx.nframes * sizeof(types::Sample);

    for (std::uint32_t i = 0; i < ctx.n_outputs; ++i) {
        if (AVC_LIKELY(in != nullptr)) {
            std::memcpy(ctx.outputs[i], in, bytes);
        } else {
            std::memset(ctx.outputs[i], 0, bytes);
        }
    }
}

}
