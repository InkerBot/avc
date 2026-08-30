#include "dsp/Biquad.hpp"

#include <algorithm>
#include <cmath>

namespace avc::dsp {
namespace {

constexpr float kPi = 3.14159265358979323846F;

BiquadCoeffs normalise(float b0, float b1, float b2, float a0, float a1, float a2) noexcept
{
    const float inv = 1.0F / a0;
    return {b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv};
}

}

BiquadCoeffs design(FilterType type, float freq, float q, float gain_db,
                    std::uint32_t sample_rate) noexcept
{
    const float rate = static_cast<float>(sample_rate > 0 ? sample_rate : 48000U);

    // Well inside the band on both ends: a cutoff at nyquist makes cos(w) == -1
    // and the denominator collapse, and one at zero does the same at the bottom.
    const float w = 2.0F * kPi * std::clamp(freq, 10.0F, rate * 0.49F) / rate;
    const float cos_w = std::cos(w);
    const float sin_w = std::sin(w);
    const float alpha = sin_w / (2.0F * std::max(q, 0.05F));
    const float amp = std::pow(10.0F, gain_db / 40.0F);

    switch (type) {
    case FilterType::LowPass:
        return normalise((1.0F - cos_w) * 0.5F, 1.0F - cos_w, (1.0F - cos_w) * 0.5F,
                         1.0F + alpha, -2.0F * cos_w, 1.0F - alpha);
    case FilterType::HighPass:
        return normalise((1.0F + cos_w) * 0.5F, -(1.0F + cos_w), (1.0F + cos_w) * 0.5F,
                         1.0F + alpha, -2.0F * cos_w, 1.0F - alpha);
    case FilterType::BandPass:
        // Constant peak gain, so a band's own output is comparable to its
        // neighbours' -- the formant shifter depends on that.
        return normalise(alpha, 0.0F, -alpha, 1.0F + alpha, -2.0F * cos_w, 1.0F - alpha);
    case FilterType::Notch:
        return normalise(1.0F, -2.0F * cos_w, 1.0F, 1.0F + alpha, -2.0F * cos_w, 1.0F - alpha);
    case FilterType::Peaking:
        return normalise(1.0F + alpha * amp, -2.0F * cos_w, 1.0F - alpha * amp,
                         1.0F + alpha / amp, -2.0F * cos_w, 1.0F - alpha / amp);
    case FilterType::LowShelf: {
        const float root = 2.0F * std::sqrt(amp) * alpha;
        return normalise(amp * ((amp + 1.0F) - (amp - 1.0F) * cos_w + root),
                         2.0F * amp * ((amp - 1.0F) - (amp + 1.0F) * cos_w),
                         amp * ((amp + 1.0F) - (amp - 1.0F) * cos_w - root),
                         (amp + 1.0F) + (amp - 1.0F) * cos_w + root,
                         -2.0F * ((amp - 1.0F) + (amp + 1.0F) * cos_w),
                         (amp + 1.0F) + (amp - 1.0F) * cos_w - root);
    }
    case FilterType::HighShelf: {
        const float root = 2.0F * std::sqrt(amp) * alpha;
        return normalise(amp * ((amp + 1.0F) + (amp - 1.0F) * cos_w + root),
                         -2.0F * amp * ((amp - 1.0F) + (amp + 1.0F) * cos_w),
                         amp * ((amp + 1.0F) + (amp - 1.0F) * cos_w - root),
                         (amp + 1.0F) - (amp - 1.0F) * cos_w + root,
                         2.0F * ((amp - 1.0F) - (amp + 1.0F) * cos_w),
                         (amp + 1.0F) - (amp - 1.0F) * cos_w - root);
    }
    }
    return {};
}

float logBandCentre(std::uint32_t index, std::uint32_t count, float low_hz, float high_hz) noexcept
{
    if (count <= 1) {
        return low_hz;
    }
    const float t = static_cast<float>(index) / static_cast<float>(count - 1);
    return low_hz * std::pow(high_hz / low_hz, t);
}

void designLogBands(BiquadCoeffs *out, std::uint32_t count, float low_hz, float high_hz, float q,
                    std::uint32_t sample_rate) noexcept
{
    for (std::uint32_t i = 0; i < count; ++i) {
        out[i] = design(FilterType::BandPass, logBandCentre(i, count, low_hz, high_hz), q, 0.0F,
                        sample_rate);
    }
}

}
