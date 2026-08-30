#include "node/voice/PitchNode.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avc::node::voice {
namespace {

constexpr float kPi = 3.14159265358979323846F;

constexpr float kMaxGrainMs = 120.0F;

constexpr float kUnisonSemitones = 0.05F;

}

NodeDescriptor PitchNode::descriptor()
{
    NodeDescriptor d;
    d.type = "pitch";
    d.category = "voice";
    d.label = "Pitch shift";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    d.params = {
        {"semitones", ParamType::Float, -24.0F, 24.0F, 0.0F, "st", ParamCurve::Linear, {},
         "How far to move the pitch. 12 is an octave. The formants move with it, "
         "so add a formant node to keep the voice sounding like a voice.", ""},
        {"grain_ms", ParamType::Float, 10.0F, kMaxGrainMs, 40.0F, "ms", ParamCurve::Logarithmic, {},
         "Longer grains sound smoother and cost half their length in latency; "
         "shorter grains are quicker and burble.", ""},
        {"mix", ParamType::Float, 0.0F, 1.0F, 1.0F, "", ParamCurve::Linear, {},
         "Dry to shifted. Below 1 the original pitch is still in the output.", ""},
    };
    d.latency_frames = static_cast<std::uint32_t>(types::kDefaultSampleRate * 0.040F / 2.0F);
    return d;
}

void PitchNode::prepare(const PrepareInfo &info)
{
    sample_rate_ = info.sample_rate;

    const std::size_t capacity =
        static_cast<std::size_t>(kMaxGrainMs * 0.001F * static_cast<float>(sample_rate_)) + 4;
    buffer_.assign(capacity, 0.0F);
    write_ = 0;

    grain_ = grain_ms_ * 0.001F * static_cast<float>(sample_rate_);
    distance_ = grain_ * 0.5F;
    engaged_.configure(0.05F);
    engaged_.snap(std::fabs(semitones_) >= kUnisonSemitones ? 1.0F : 0.0F);
    republishLatency();
}

void PitchNode::inherit(const Node &previous)
{
    // The delay line itself is not worth carrying over -- it is a few tens of
    // milliseconds and the swap is inaudible either way -- but the engage ramp
    // is: without it a swap while shifting would re-open the shifter from dry.
    const auto &other = static_cast<const PitchNode &>(previous);
    engaged_.snap(other.engaged_.current());
}

void PitchNode::republishLatency() noexcept
{
    const float grain = grain_ms_ * 0.001F * static_cast<float>(sample_rate_);
    const bool engaged = std::fabs(semitones_) >= kUnisonSemitones;
    latency_.store(engaged ? static_cast<std::uint32_t>(grain * 0.5F) : 0U,
                   std::memory_order_relaxed);
}

// ZFW: HOT PATH
void PitchNode::setParam(std::uint32_t index, float value) noexcept
{
    switch (index) {
    case kSemitones:
        semitones_ = value;
        ratio_ = std::pow(2.0F, value / 12.0F);
        break;
    case kGrainMs:
        grain_ms_ = value;
        break;
    case kMix:
        mix_ = value;
        break;
    default:
        return;
    }
    republishLatency();
}

// ZFW: HOT PATH
float PitchNode::readAt(float delay) const noexcept
{
    // The wrap is integer arithmetic on purpose. Done in float it reads
    // `write_ - delay`, adds the buffer size back when that goes negative, and
    // for a delay small enough -- which is exactly where the second head sits
    // as it wraps -- `size - delay` rounds straight back up to `size`, one
    // index past the end of the buffer. Splitting the delay first leaves the
    // fraction to the interpolation and the wrap to indices that cannot round.
    const float whole = std::floor(delay);
    const float frac = delay - whole;

    const std::uint32_t size = static_cast<std::uint32_t>(buffer_.size());
    std::uint32_t near = write_ + size - static_cast<std::uint32_t>(whole);
    if (near >= size) {
        near -= size;
    }
    const std::uint32_t far = near == 0 ? size - 1 : near - 1;
    return buffer_[near] + frac * (buffer_[far] - buffer_[near]);
}

// ZFW: HOT PATH
void PitchNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];

    if (AVC_UNLIKELY(in == nullptr || buffer_.empty())) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }

    // Grain length is a slider, so it is re-read per block and the heads are
    // pulled back inside the new window rather than being allowed to run off
    // into a stale part of the buffer.
    grain_ = std::clamp(grain_ms_ * 0.001F * static_cast<float>(sample_rate_), 1.0F,
                        static_cast<float>(buffer_.size() - 2));
    if (distance_ >= grain_) {
        distance_ = std::fmod(distance_, grain_);
    }

    engaged_.setTarget(std::fabs(semitones_) >= kUnisonSemitones ? 1.0F : 0.0F);
    const float wet = engaged_.nextBlock() * mix_;
    const float dry = 1.0F - wet;
    const float step = 1.0F - ratio_;
    const float phase_scale = kPi / grain_;

    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        const float x = in[f];
        buffer_[write_] = x;

        const float phase = distance_ * phase_scale;
        float far = distance_ + grain_ * 0.5F;
        if (far >= grain_) {
            far -= grain_;
        }
        const float shifted =
            readAt(distance_) * std::sin(phase) + readAt(far) * std::fabs(std::cos(phase));

        out[f] = x * dry + shifted * wet;

        distance_ += step;
        if (distance_ < 0.0F) {
            distance_ += grain_;
        } else if (distance_ >= grain_) {
            distance_ -= grain_;
        }
        write_ = write_ + 1 == buffer_.size() ? 0 : write_ + 1;
    }
}

}
