#pragma once

#include <cstdint>

namespace avc::dsp {

struct BiquadCoeffs {
    float b0 = 1.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;
};

enum class FilterType : std::uint8_t {
    LowPass = 0,
    HighPass,
    BandPass,
    Notch,
    Peaking,
    LowShelf,
    HighShelf,
};

BiquadCoeffs design(FilterType type, float freq, float q, float gain_db,
                    std::uint32_t sample_rate) noexcept;

void designLogBands(BiquadCoeffs *out, std::uint32_t count, float low_hz, float high_hz, float q,
                    std::uint32_t sample_rate) noexcept;

float logBandCentre(std::uint32_t index, std::uint32_t count, float low_hz,
                    float high_hz) noexcept;

class Biquad {
public:
    void setCoeffs(const BiquadCoeffs &coeffs) noexcept { c_ = coeffs; }
    void reset() noexcept
    {
        z1_ = 0.0F;
        z2_ = 0.0F;
    }

    // ZFW: HOT PATH
    float tick(float x) noexcept
    {
        const float y = c_.b0 * x + z1_;
        z1_ = c_.b1 * x - c_.a1 * y + z2_;
        z2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }

private:
    BiquadCoeffs c_;
    float z1_ = 0.0F;
    float z2_ = 0.0F;
};

}
