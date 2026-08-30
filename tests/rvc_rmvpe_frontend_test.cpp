#include "avc_rvc/RmvpeFrontend.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

TEST(RvcRmvpeFrontend, ProducesThePaddedLogMelShapeExpectedByOnnx)
{
    std::vector<float> silence(8192, 0.0F);
    const avc::rvc::RmvpeMel mel = avc::rvc::RmvpeFrontend::logMel(silence);
    EXPECT_EQ(mel.frames, 52U);
    EXPECT_EQ(mel.padded_frames, 64U);
    ASSERT_EQ(mel.values.size(), 128U * 64U);
    EXPECT_NEAR(mel.values[0], std::log(1e-5F), 1e-5F);
    EXPECT_FLOAT_EQ(mel.values[63], 0.0F);
}

TEST(RvcRmvpeFrontend, AVoicedSaliencePeakDecodesAndAWeakPeakDoesNot)
{
    std::vector<float> probabilities(2 * 360, 0.0F);
    probabilities[100] = 0.9F;
    probabilities[360 + 100] = 0.02F;
    const std::vector<float> f0 = avc::rvc::RmvpeFrontend::decode(probabilities.data(), 2);
    EXPECT_NEAR(f0[0], 100.64F, 0.02F);
    EXPECT_FLOAT_EQ(f0[1], 0.0F);
}

TEST(RvcRmvpeFrontend, SineInputProducesFiniteNonSilentMelEnergy)
{
    std::vector<float> audio(8192);
    for (std::size_t i = 0; i < audio.size(); ++i) {
        audio[i] = 0.1F * std::sin(2.0F * 3.14159265358979323846F * 220.0F
                                  * static_cast<float>(i) / 16000.0F);
    }
    const avc::rvc::RmvpeMel mel = avc::rvc::RmvpeFrontend::logMel(audio);
    EXPECT_TRUE(std::ranges::all_of(mel.values, [](float value) { return std::isfinite(value); }));
    EXPECT_GT(*std::max_element(mel.values.begin(), mel.values.end()), 0.0F);
}
