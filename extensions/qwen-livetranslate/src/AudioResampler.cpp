#include <avc_qwen/AudioResampler.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace avc::qwen {
namespace {

std::int16_t pcm16(float sample) noexcept
{
    const float clipped = std::clamp(sample, -1.0F, 1.0F);
    const long value = std::lround(clipped * (clipped < 0.0F ? 32768.0F : 32767.0F));
    return static_cast<std::int16_t>(
        std::clamp(value, static_cast<long>(std::numeric_limits<std::int16_t>::min()),
                   static_cast<long>(std::numeric_limits<std::int16_t>::max())));
}

}

void FloatToPcm16Resampler::reset(std::uint32_t input_rate,
                                  std::uint32_t output_rate) noexcept
{
    input_rate_ = std::max(input_rate, 1U);
    output_rate_ = std::max(output_rate, 1U);
    source_index_ = 0;
    next_output_numerator_ = 0;
    previous_ = 0.0F;
    filtered_ = 0.0F;
    have_previous_ = false;

    if (input_rate_ > output_rate_) {
        constexpr double kPi = 3.14159265358979323846;
        const double cutoff = 0.45 * static_cast<double>(output_rate_);
        filter_alpha_ = static_cast<float>(
            1.0 - std::exp(-2.0 * kPi * cutoff / static_cast<double>(input_rate_)));
    } else {
        filter_alpha_ = 1.0F;
    }
}

std::size_t FloatToPcm16Resampler::process(std::span<const float> input,
                                           std::span<std::int16_t> output) noexcept
{
    std::size_t produced = 0;
    for (float raw : input) {
        filtered_ += filter_alpha_ * (raw - filtered_);
        const float current = filtered_;

        if (!have_previous_) {
            previous_ = current;
            have_previous_ = true;
            if (next_output_numerator_ == 0 && produced < output.size()) {
                output[produced++] = pcm16(current);
                next_output_numerator_ += input_rate_;
            }
            continue;
        }

        ++source_index_;
        const std::uint64_t interval_start = (source_index_ - 1) * output_rate_;
        const std::uint64_t interval_end = source_index_ * output_rate_;
        while (next_output_numerator_ <= interval_end && produced < output.size()) {
            const double fraction = static_cast<double>(next_output_numerator_ - interval_start)
                                  / static_cast<double>(output_rate_);
            output[produced++] = pcm16(static_cast<float>(
                previous_ + (current - previous_) * fraction));
            next_output_numerator_ += input_rate_;
        }
        previous_ = current;
    }
    return produced;
}

void Pcm16ToFloatResampler::reset(std::uint32_t input_rate,
                                  std::uint32_t output_rate) noexcept
{
    step_ = static_cast<double>(std::max(input_rate, 1U))
          / static_cast<double>(std::max(output_rate, 1U));
    fraction_ = 0.0;
    current_ = 0;
    next_ = 0;
    primed_ = false;
}

}
