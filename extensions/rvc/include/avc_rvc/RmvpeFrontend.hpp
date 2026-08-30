#pragma once

#include <cstddef>
#include <vector>

namespace avc::rvc {

struct RmvpeMel {
    std::vector<float> values;
    std::size_t frames = 0;
    std::size_t padded_frames = 0;
};

class RmvpeFrontend {
public:
    static constexpr std::size_t kMelBins = 128;
    static constexpr std::size_t kPitchBins = 360;

    static RmvpeMel logMel(const std::vector<float> &audio_16k);
    static std::vector<float> decode(const float *probabilities, std::size_t frames,
                                     float threshold = 0.03F);
};

}
