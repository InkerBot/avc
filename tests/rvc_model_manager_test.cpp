#include "control/RvcModelManager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

class RvcModelManagerTest : public ::testing::Test {
protected:
    RvcModelManagerTest()
        : root_(std::filesystem::temp_directory_path()
                / ("avc-rvc-import-" + std::to_string(counter_++)))
    {
        std::filesystem::create_directories(root_);
    }

    ~RvcModelManagerTest() override
    {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::filesystem::path touch(const std::string &name)
    {
        const auto path = root_ / name;
        std::ofstream(path) << "model";
        return path;
    }

    avc::control::RvcModelManager::RuntimePaths fakeRuntime()
    {
        const auto script = root_ / "convert";
        std::ofstream(script)
            << "#!/bin/sh\n"
               "while [ $# -gt 0 ]; do\n"
               "  case \"$1\" in\n"
               "    --output) output=$2; shift 2;;\n"
               "    --name) name=$2; shift 2;;\n"
               "    --contentvec-v1) vec=$2; shift 2;;\n"
               "    --contentvec-v2) shift 2;;\n"
               "    --rmvpe) rmvpe=$2; shift 2;;\n"
               "    --index) index=$2; shift 2;;\n"
               "    *) shift;;\n"
               "  esac\n"
               "done\n"
               "echo '{\"progress\":0.8,\"message\":\"fake export\"}'\n"
               "mkdir -p \"$output\"\n"
               "printf onnx > \"$output/synthesizer.onnx\"\n"
               "ln -s \"$vec\" \"$output/contentvec.onnx\"\n"
               "ln -s \"$rmvpe\" \"$output/rmvpe.onnx\"\n"
               "extra=\n"
               "if [ -n \"$index\" ]; then\n"
               "  printf avcidx > \"$output/feature_index.avcidx\"\n"
               "  extra=',\"feature_index\":\"feature_index.avcidx\",\"retrieval\":{\"vectors\":3}'\n"
               "fi\n"
               "printf '{\"format_version\":1,\"name\":\"%s\",\"rvc_version\":\"v1\",\"model_sample_rate\":40000,\"speakers\":1%s}' \"$name\" \"$extra\" > \"$output/manifest.json\"\n";
        std::filesystem::permissions(
            script,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                | std::filesystem::perms::owner_exec | std::filesystem::perms::group_read
                | std::filesystem::perms::group_exec,
            std::filesystem::perm_options::replace);
        return {script, touch("v1.onnx"), touch("v2.onnx"), touch("rmvpe.onnx")};
    }

    std::filesystem::path root_;
    inline static int counter_ = 0;
};

TEST_F(RvcModelManagerTest, AcceptsOnlyPlainPthFilenames)
{
    using Manager = avc::control::RvcModelManager;
    EXPECT_TRUE(Manager::validSourceFilename("voice.pth"));
    EXPECT_TRUE(Manager::validIndexFilename("voice.index"));
    EXPECT_FALSE(Manager::validSourceFilename("../voice.pth"));
    EXPECT_FALSE(Manager::validSourceFilename("voice.onnx"));
    EXPECT_FALSE(Manager::validSourceFilename(".pth"));
    EXPECT_FALSE(Manager::validIndexFilename("../voice.index"));
    EXPECT_EQ(Manager::safeModelName("A voice (40k).pth"), "A_voice__40k");
}

TEST_F(RvcModelManagerTest, StreamsAnOptionalFeatureIndexIntoTheConvertedPackage)
{
#ifdef _WIN32
    GTEST_SKIP() << "the bundled RVC converter currently supports Linux only";
#endif
    avc::control::RvcModelManager manager(root_ / "data", fakeRuntime());
    std::string error;
    auto upload = manager.beginUpload("source.pth", "indexed", error);
    ASSERT_TRUE(upload.has_value()) << error;
    const std::string checkpoint = "restricted checkpoint bytes";
    ASSERT_TRUE(manager.append(*upload, checkpoint.data(), checkpoint.size(), error)) << error;
    ASSERT_TRUE(manager.beginIndex(*upload, "added_voice.index", error)) << error;
    const std::string index = "faiss index bytes";
    ASSERT_TRUE(manager.appendIndex(*upload, index.data(), index.size(), error)) << error;
    ASSERT_TRUE(manager.commit(std::move(*upload), error)) << error;

    nlohmann::json state;
    for (int i = 0; i < 100; ++i) {
        state = manager.describe();
        if (state["job"].value("state", "") == "ready") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(state["job"]["state"], "ready") << state.dump();
    ASSERT_EQ(state["models"].size(), 1U);
    EXPECT_TRUE(state["models"][0]["indexed"].get<bool>());
    EXPECT_EQ(state["models"][0]["indexVectors"], 3);
}

TEST_F(RvcModelManagerTest, ReportsWhyTheBundledRuntimeIsMissing)
{
    avc::control::RvcModelManager manager(root_ / "data", {});
    const auto state = manager.describe();
    EXPECT_FALSE(state["available"].get<bool>());
    EXPECT_FALSE(state["error"].get<std::string>().empty());
}

TEST_F(RvcModelManagerTest, StreamsConvertsAndAtomicallyPublishesAModel)
{
#ifdef _WIN32
    GTEST_SKIP() << "the bundled RVC converter currently supports Linux only";
#endif
    avc::control::RvcModelManager manager(root_ / "data", fakeRuntime());
    std::string error;
    auto upload = manager.beginUpload("source.pth", "voice", error);
    ASSERT_TRUE(upload.has_value()) << error;
    const std::string checkpoint = "restricted checkpoint bytes";
    ASSERT_TRUE(manager.append(*upload, checkpoint.data(), checkpoint.size(), error)) << error;
    ASSERT_TRUE(manager.commit(std::move(*upload), error)) << error;

    nlohmann::json state;
    for (int i = 0; i < 100; ++i) {
        state = manager.describe();
        if (state["job"].value("state", "") == "ready") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(state["job"]["state"], "ready") << state.dump();
    ASSERT_EQ(state["models"].size(), 1U);
    EXPECT_EQ(state["models"][0]["key"], "voice");
    EXPECT_TRUE(std::filesystem::is_directory(state["models"][0]["path"].get<std::string>()));

    ASSERT_TRUE(manager.removeModel("voice", error)) << error;
    EXPECT_TRUE(manager.describe()["models"].empty());
}

}
