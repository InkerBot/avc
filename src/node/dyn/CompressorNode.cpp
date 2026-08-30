#include "node/dyn/CompressorNode.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avc::node::dyn {
namespace {

constexpr float kLnToDb = 8.685889638F;
constexpr float kDbToLn = 0.1151292546F;

constexpr float kFloor = 1e-16F;

}

NodeDescriptor CompressorNode::descriptor()
{
    NodeDescriptor d;
    d.type = "compressor";
    d.category = "dyn";
    d.label = "Compressor";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    d.params = {
        {"threshold_db", ParamType::Float, -60.0F, 0.0F, -18.0F, "dB", ParamCurve::Linear, {},
         "Where it starts working.", ""},
        {"ratio", ParamType::Float, 1.0F, 20.0F, 4.0F, ":1", ParamCurve::Logarithmic, {},
         "How much of what goes over the threshold is let through. 4 turns 8 dB "
         "over into 2 dB over.", ""},
        {"knee_db", ParamType::Float, 0.0F, 24.0F, 6.0F, "dB", ParamCurve::Linear, {},
         "How wide a region either side of the threshold to ease into it over. "
         "Zero is an audible corner.", ""},
        {"attack_ms", ParamType::Float, 0.1F, 100.0F, 10.0F, "ms", ParamCurve::Logarithmic, {},
         "How fast it clamps down. Very fast attacks flatten consonants.", ""},
        {"release_ms", ParamType::Float, 5.0F, 2000.0F, 150.0F, "ms", ParamCurve::Logarithmic, {},
         "How fast it lets go. Too fast pumps the room noise up between words.", ""},
        {"makeup_db", ParamType::Float, -12.0F, 24.0F, 0.0F, "dB", ParamCurve::Linear, {},
         "Put back what the compression took off.", ""},
    };
    return d;
}

void CompressorNode::retime() noexcept
{
    const float rate = static_cast<float>(sample_rate_);
    const float attack_samples = std::max(attack_ms_, 0.01F) * 0.001F * rate;
    const float release_samples = std::max(release_ms_, 0.01F) * 0.001F * rate;
    attack_ = 1.0F - std::exp(-1.0F / std::max(attack_samples, 1.0F));
    release_ = 1.0F - std::exp(-1.0F / std::max(release_samples, 1.0F));
}

void CompressorNode::prepare(const PrepareInfo &info)
{
    sample_rate_ = info.sample_rate;
    retime();
    env_db_ = 0.0F;
}

void CompressorNode::inherit(const Node &previous)
{
    // Losing the envelope means the next block is compressed as if the speaker
    // had just started talking, which is a jump in level on a held note.
    const auto &other = static_cast<const CompressorNode &>(previous);
    env_db_ = other.env_db_;
}

// ZFW: HOT PATH
void CompressorNode::setParam(std::uint32_t index, float value) noexcept
{
    switch (index) {
    case kThresholdDb: threshold_ = value; return;
    case kRatio:       slope_ = 1.0F - 1.0F / std::max(value, 1.0F); return;
    case kKneeDb:      knee_ = value; return;
    case kAttackMs:    attack_ms_ = value; break;
    case kReleaseMs:   release_ms_ = value; break;
    case kMakeupDb:    makeup_ = value; return;
    default:           return;
    }
    retime();
}

// ZFW: HOT PATH
void CompressorNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];

    if (AVC_UNLIKELY(in == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        env_db_ = 0.0F;
        reduction_.store(0.0F, std::memory_order_relaxed);
        return;
    }

    const float half_knee = knee_ * 0.5F;

    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        const float x = in[f];
        const float level_db = kLnToDb * 0.5F * std::log(x * x + kFloor);
        const float over = level_db - threshold_;

        float target = 0.0F;
        if (over >= half_knee) {
            target = -slope_ * over;
        } else if (over > -half_knee && knee_ > 0.0F) {
            const float into = over + half_knee;
            target = -slope_ * into * into / (2.0F * knee_);
        }

        env_db_ += (target - env_db_) * (target < env_db_ ? attack_ : release_);
        out[f] = x * std::exp((env_db_ + makeup_) * kDbToLn);
    }

    reduction_.store(env_db_, std::memory_order_relaxed);
}

}
