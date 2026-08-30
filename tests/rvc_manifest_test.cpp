#include "avc_rvc/ModelManifest.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

class RvcManifestTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path()
                / ("avc-rvc-manifest-" + std::to_string(counter_++));
        std::filesystem::create_directories(root_);
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    void touch(const char *name) { std::ofstream(root_ / name).put('\0'); }

    static inline unsigned counter_ = 0;
    std::filesystem::path root_;
};

TEST_F(RvcManifestTest, LoadsACompleteV2Package)
{
    touch("contentvec.onnx");
    touch("rmvpe.onnx");
    touch("synthesizer.onnx");
    touch("feature_index.avcidx");
    std::ofstream(root_ / "manifest.json")
        << R"({"format_version":1,"name":"voice","rvc_version":"v2","content_dim":768,"model_sample_rate":40000,"synthesizer_frames":50,"content_samples_16k":8192,"uses_f0":true,"feature_index":"feature_index.avcidx"})";

    avc::rvc::ModelManifest manifest;
    std::string error;
    ASSERT_TRUE(avc::rvc::ModelManifest::load(root_, manifest, error)) << error;
    EXPECT_EQ(manifest.content_dim, 768U);
    EXPECT_EQ(manifest.model_sample_rate, 40000U);
    EXPECT_EQ(manifest.synthesizer_frames, 50U);
    EXPECT_EQ(manifest.content_samples_16k, 8192U);
    EXPECT_TRUE(manifest.uses_f0);
    EXPECT_TRUE(manifest.uses_index);
    EXPECT_EQ(manifest.feature_index, root_ / "feature_index.avcidx");
}

TEST_F(RvcManifestTest, RejectsAMissingRequiredModel)
{
    touch("contentvec.onnx");
    std::ofstream(root_ / "manifest.json")
        << R"({"format_version":1,"content_dim":256,"model_sample_rate":32000,"uses_f0":false})";

    avc::rvc::ModelManifest manifest;
    std::string error;
    EXPECT_FALSE(avc::rvc::ModelManifest::load(root_, manifest, error));
    EXPECT_NE(error.find("synthesizer"), std::string::npos) << error;
}

TEST_F(RvcManifestTest, RejectsUnknownFormatAndDimensions)
{
    std::ofstream(root_ / "manifest.json")
        << R"({"format_version":2,"content_dim":123,"model_sample_rate":40000})";
    avc::rvc::ModelManifest manifest;
    std::string error;
    EXPECT_FALSE(avc::rvc::ModelManifest::load(root_, manifest, error));
    EXPECT_NE(error.find("format_version"), std::string::npos) << error;
}

TEST_F(RvcManifestTest, RejectsANonTextFeatureIndexPath)
{
    touch("contentvec.onnx");
    touch("rmvpe.onnx");
    touch("synthesizer.onnx");
    std::ofstream(root_ / "manifest.json")
        << R"({"format_version":1,"content_dim":768,"model_sample_rate":40000,"uses_f0":true,"feature_index":42})";

    avc::rvc::ModelManifest manifest;
    std::string error;
    EXPECT_FALSE(avc::rvc::ModelManifest::load(root_, manifest, error));
    EXPECT_NE(error.find("feature_index"), std::string::npos) << error;
}

TEST_F(RvcManifestTest, RejectsAnUnboundedContentWindow)
{
    touch("contentvec.onnx");
    touch("synthesizer.onnx");
    std::ofstream(root_ / "manifest.json")
        << R"({"format_version":1,"content_dim":768,"model_sample_rate":40000,"content_samples_16k":262145,"uses_f0":false})";

    avc::rvc::ModelManifest manifest;
    std::string error;
    EXPECT_FALSE(avc::rvc::ModelManifest::load(root_, manifest, error));
    EXPECT_NE(error.find("content_samples_16k"), std::string::npos) << error;
}

TEST_F(RvcManifestTest, RejectsTheObsoleteEightFrameTrace)
{
    touch("contentvec.onnx");
    touch("rmvpe.onnx");
    touch("synthesizer.onnx");
    std::ofstream(root_ / "manifest.json")
        << R"({"format_version":1,"content_dim":768,"model_sample_rate":40000,"synthesizer_frames":8,"uses_f0":true})";

    avc::rvc::ModelManifest manifest;
    std::string error;
    EXPECT_FALSE(avc::rvc::ModelManifest::load(root_, manifest, error));
    EXPECT_NE(error.find("obsolete short RVC trace"), std::string::npos) << error;
}

}
