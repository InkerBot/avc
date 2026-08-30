#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace avc::qwen {

class FloatToPcm16Resampler {
public:
    void reset(std::uint32_t input_rate, std::uint32_t output_rate = 16000) noexcept;
    std::size_t process(std::span<const float> input,
                        std::span<std::int16_t> output) noexcept;

private:
    std::uint32_t input_rate_ = 48000;
    std::uint32_t output_rate_ = 16000;
    std::uint64_t source_index_ = 0;
    std::uint64_t next_output_numerator_ = 0;
    float previous_ = 0.0F;
    float filtered_ = 0.0F;
    float filter_alpha_ = 1.0F;
    bool have_previous_ = false;
};

class Pcm16ToFloatResampler {
public:
    void reset(std::uint32_t input_rate = 24000,
               std::uint32_t output_rate = 48000) noexcept;

    template <class Source>
    bool pull(Source &&source, float &sample) noexcept
    {
        if (!primed_) {
            if (!source(current_) || !source(next_)) return false;
            fraction_ = 0.0;
            primed_ = true;
        }

        sample = static_cast<float>((1.0 - fraction_) * pcm(current_)
                                    + fraction_ * pcm(next_));
        fraction_ += step_;
        while (fraction_ >= 1.0) {
            current_ = next_;
            if (!source(next_)) {
                primed_ = false;
                return true;
            }
            fraction_ -= 1.0;
        }
        return true;
    }

private:
    static float pcm(std::int16_t value) noexcept
    {
        return static_cast<float>(value) / 32768.0F;
    }

    double step_ = 0.5;
    double fraction_ = 0.0;
    std::int16_t current_ = 0;
    std::int16_t next_ = 0;
    bool primed_ = false;
};

}
