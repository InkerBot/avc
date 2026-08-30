#include "node/debug/ScopeNode.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avc::node::debug {
namespace {

constexpr float kFloorDb = -100.0F;

}

NodeDescriptor ScopeNode::descriptor()
{
    NodeDescriptor d;
    d.type = "scope";
    d.category = "debug";
    d.label = "Scope";
    d.inputs = {{"in"}};
    d.outputs = {{"out"}};
    d.params = {
        {"window_ms", ParamType::Float, 5.0F, 500.0F, 40.0F, "ms", ParamCurve::Logarithmic, {},
         "How much time the waveform covers. Short enough to see a single "
         "pitch period, or long enough to see a syllable.", ""},
    };
    return d;
}

float ScopeNode::bandCentre(std::uint32_t index) noexcept
{
    return dsp::logBandCentre(index, kScopeBands, kLowHz, kHighHz);
}

void ScopeNode::prepare(const PrepareInfo &info)
{
    sample_rate_ = info.sample_rate;

    std::array<dsp::BiquadCoeffs, kScopeBands> coeffs{};
    dsp::designLogBands(coeffs.data(), kScopeBands, kLowHz, kHighHz, kBandQ, info.sample_rate);
    for (std::uint32_t b = 0; b < kScopeBands; ++b) {
        bands_[b].setCoeffs(coeffs[b]);
        bands_[b].reset();
        energy_[b] = 0.0F;
    }

    bucket_peak_ = 0.0F;
    bucket_fill_ = 0;
    point_ = 0;
}

// ZFW: HOT PATH
void ScopeNode::setParam(std::uint32_t index, float value) noexcept
{
    if (index == kWindowMs) {
        window_ms_ = value;
    }
}

bool ScopeNode::takeFrame(ScopeFrame &out) noexcept
{
    bool any = false;
    // Drain rather than take one: the audio thread produces a frame per window
    // and the editor asks at its own rate, so what it wants is the newest, not
    // a queue that grows a display lag.
    while (frames_.pop(out)) {
        any = true;
    }
    return any;
}

// ZFW: HOT PATH
void ScopeNode::process(const NodeContext &ctx) noexcept
{
    types::Sample *out = ctx.outputs[0];
    const types::Sample *in = ctx.inputs[0];

    if (AVC_UNLIKELY(in == nullptr)) {
        std::memset(out, 0, ctx.nframes * sizeof(types::Sample));
        return;
    }

    const float window_samples = std::clamp(window_ms_, 1.0F, 1000.0F) * 0.001F
                                 * static_cast<float>(sample_rate_);
    bucket_samples_ = std::max(1U, static_cast<std::uint32_t>(window_samples / kScopePoints));

    for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
        const float x = in[f];
        out[f] = x;

        // Sum of squares per band over the window: an energy measurement needs
        // no follower and no time constant to argue about, because the window
        // is already the averaging time.
        for (std::uint32_t b = 0; b < kScopeBands; ++b) {
            const float band = bands_[b].tick(x);
            energy_[b] += band * band;
        }

        if (std::fabs(x) > std::fabs(bucket_peak_)) {
            bucket_peak_ = x;
        }
        if (++bucket_fill_ < bucket_samples_) {
            continue;
        }

        building_.wave[point_] = bucket_peak_;
        bucket_peak_ = 0.0F;
        bucket_fill_ = 0;

        if (++point_ < kScopePoints) {
            continue;
        }
        point_ = 0;

        const float samples = static_cast<float>(bucket_samples_ * kScopePoints);
        for (std::uint32_t b = 0; b < kScopeBands; ++b) {
            const float rms = std::sqrt(energy_[b] / samples);
            building_.bands[b] = rms > 1e-5F ? 20.0F * std::log10(rms) : kFloorDb;
            energy_[b] = 0.0F;
        }
        frames_.push(building_);
    }
}

}
