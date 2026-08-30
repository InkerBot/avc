#include "avc_rvc/RmvpeFrontend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>

namespace avc::rvc {
namespace {

constexpr std::size_t kFftSize = 1024;
constexpr std::size_t kSpectrumBins = kFftSize / 2 + 1;
constexpr std::size_t kHop = 160;
constexpr float kSampleRate = 16000.0F;
constexpr float kFMin = 30.0F;
constexpr float kFMax = 8000.0F;
constexpr float kPi = 3.14159265358979323846F;

float hzToMel(float hz) { return 2595.0F * std::log10(1.0F + hz / 700.0F); }
float melToHz(float mel) { return 700.0F * (std::pow(10.0F, mel / 2595.0F) - 1.0F); }

const std::vector<float> &melFilters()
{
    static const std::vector<float> filters = [] {
        std::array<float, RmvpeFrontend::kMelBins + 2> edges{};
        const float mel_min = hzToMel(kFMin);
        const float mel_max = hzToMel(kFMax);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            edges[i] = melToHz(mel_min + (mel_max - mel_min) * static_cast<float>(i)
                                             / static_cast<float>(edges.size() - 1));
        }

        std::vector<float> result(RmvpeFrontend::kMelBins * kSpectrumBins, 0.0F);
        for (std::size_t mel = 0; mel < RmvpeFrontend::kMelBins; ++mel) {
            const float lower = edges[mel];
            const float center = edges[mel + 1];
            const float upper = edges[mel + 2];
            // librosa.filters.mel(..., htk=True) defaults to Slaney area
            // normalization even though the frequency scale itself is HTK.
            const float normalization = 2.0F / (upper - lower);
            for (std::size_t bin = 0; bin < kSpectrumBins; ++bin) {
                const float frequency = kSampleRate * static_cast<float>(bin)
                                        / static_cast<float>(kFftSize);
                const float rising = (frequency - lower) / (center - lower);
                const float falling = (upper - frequency) / (upper - center);
                result[mel * kSpectrumBins + bin] =
                    std::max(0.0F, std::min(rising, falling)) * normalization;
            }
        }
        return result;
    }();
    return filters;
}

void fft(std::array<std::complex<float>, kFftSize> &values)
{
    for (std::size_t i = 1, reversed = 0; i < kFftSize; ++i) {
        std::size_t bit = kFftSize >> 1;
        for (; (reversed & bit) != 0; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (i < reversed) std::swap(values[i], values[reversed]);
    }
    for (std::size_t length = 2; length <= kFftSize; length <<= 1) {
        const float angle = -2.0F * kPi / static_cast<float>(length);
        const std::complex<float> step{std::cos(angle), std::sin(angle)};
        for (std::size_t start = 0; start < kFftSize; start += length) {
            std::complex<float> phase{1.0F, 0.0F};
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const std::complex<float> even = values[start + offset];
                const std::complex<float> odd = values[start + offset + length / 2] * phase;
                values[start + offset] = even + odd;
                values[start + offset + length / 2] = even - odd;
                phase *= step;
            }
        }
    }
}

std::size_t reflectedIndex(std::ptrdiff_t index, std::size_t size)
{
    if (size < 2) return 0;
    const std::ptrdiff_t limit = static_cast<std::ptrdiff_t>(size);
    while (index < 0 || index >= limit) {
        if (index < 0) index = -index;
        if (index >= limit) index = 2 * limit - 2 - index;
    }
    return static_cast<std::size_t>(index);
}

}

RmvpeMel RmvpeFrontend::logMel(const std::vector<float> &audio_16k)
{
    RmvpeMel output;
    if (audio_16k.empty()) return output;
    // torch.stft(center=True) reflect-pads n_fft/2 samples on both sides,
    // yielding floor(samples / hop) + 1 frames.
    output.frames = audio_16k.size() / kHop + 1;
    output.padded_frames = ((output.frames + 31) / 32) * 32;
    output.values.assign(kMelBins * output.padded_frames, 0.0F);

    const std::vector<float> &filters = melFilters();
    std::array<std::complex<float>, kFftSize> spectrum{};
    std::array<float, kSpectrumBins> magnitude{};
    for (std::size_t frame = 0; frame < output.frames; ++frame) {
        const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(frame * kHop)
                                     - static_cast<std::ptrdiff_t>(kFftSize / 2);
        for (std::size_t sample = 0; sample < kFftSize; ++sample) {
            const std::size_t source = reflectedIndex(
                start + static_cast<std::ptrdiff_t>(sample), audio_16k.size());
            // torch.hann_window(1024) is periodic by default.
            const float window = 0.5F - 0.5F * std::cos(
                                                  2.0F * kPi * static_cast<float>(sample)
                                                  / static_cast<float>(kFftSize));
            spectrum[sample] = {audio_16k[source] * window, 0.0F};
        }
        fft(spectrum);
        for (std::size_t bin = 0; bin < kSpectrumBins; ++bin) {
            magnitude[bin] = std::abs(spectrum[bin]);
        }
        for (std::size_t mel = 0; mel < kMelBins; ++mel) {
            double value = 0.0;
            const float *filter = filters.data() + mel * kSpectrumBins;
            for (std::size_t bin = 0; bin < kSpectrumBins; ++bin) {
                value += static_cast<double>(filter[bin]) * magnitude[bin];
            }
            output.values[mel * output.padded_frames + frame] =
                std::log(std::max(static_cast<float>(value), 1e-5F));
        }
    }
    return output;
}

std::vector<float> RmvpeFrontend::decode(const float *probabilities, std::size_t frames,
                                         float threshold)
{
    std::vector<float> f0(frames, 0.0F);
    if (probabilities == nullptr) return f0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float *row = probabilities + frame * kPitchBins;
        const auto maximum = std::max_element(row, row + kPitchBins);
        if (!std::isfinite(*maximum) || *maximum <= threshold) continue;
        const std::size_t center = static_cast<std::size_t>(maximum - row);
        const std::size_t begin = center > 4 ? center - 4 : 0;
        const std::size_t end = std::min(center + 5, kPitchBins);
        double weighted_cents = 0.0;
        double weight = 0.0;
        for (std::size_t bin = begin; bin < end; ++bin) {
            if (!std::isfinite(row[bin]) || row[bin] <= 0.0F) continue;
            const double cents = 20.0 * static_cast<double>(bin) + 1997.3794084376191;
            weighted_cents += static_cast<double>(row[bin]) * cents;
            weight += row[bin];
        }
        if (weight > std::numeric_limits<double>::epsilon()) {
            f0[frame] = static_cast<float>(10.0 * std::pow(2.0, weighted_cents / weight / 1200.0));
        }
    }
    return f0;
}

}
