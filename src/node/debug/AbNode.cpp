#include "node/debug/AbNode.hpp"

#include "common.hpp"

#include <cmath>
#include <cstring>

namespace avc::node::debug {
namespace {

constexpr float kHalfPi = 1.57079632679489661923F;

}

NodeDescriptor AbNode::descriptor()
{
    NodeDescriptor d;
    d.type = "ab";
    d.category = "debug";
    d.label = "A/B compare";
    d.inputs = {{"a"}, {"b"}};
    d.outputs = {{"out"}};
    d.params = {
        {"select", ParamType::Float, 0.0F, 1.0F, 0.0F, "", ParamCurve::Linear, {},
         "0 is A, 1 is B, and anything between is both. Smoothed, so flipping it "
         "while listening does not click.", ""},
    };
    return d;
}

void AbNode::prepare(const PrepareInfo & )
{
    select_.configure(0.08F);
    select_.snap(select_.target());
}

void AbNode::inherit(const Node &previous)
{
    const auto &other = static_cast<const AbNode &>(previous);
    select_.snap(other.select_.current());
}

// ZFW: HOT PATH
void AbNode::setParam(std::uint32_t index, float value) noexcept
{
    if (index == kSelect) {
        select_.setTarget(value);
    }
}

// ZFW: HOT PATH
void AbNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *a = ctx.inputs[0];
    const types::Sample *b = ctx.n_inputs > 1 ? ctx.inputs[1] : nullptr;

    const float t = select_.nextBlock() * kHalfPi;
    const float gain_a = std::cos(t);
    const float gain_b = std::sin(t);

    if (AVC_UNLIKELY(a == nullptr && b == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }
    if (a == nullptr) {
        for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
            out[f] = b[f] * gain_b;
        }
        return;
    }
    if (b == nullptr) {
        for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
            out[f] = a[f] * gain_a;
        }
        return;
    }
    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        out[f] = a[f] * gain_a + b[f] * gain_b;
    }
}

}
