#include "avc_rvc/FeatureIndex.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

template <typename T>
void write(std::ofstream &out, const T &value)
{
    out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

std::filesystem::path makeIndex()
{
    const auto path = std::filesystem::temp_directory_path() / "avc-rvc-feature-index-test.avcidx";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::array<char, 8> magic{'A', 'V', 'C', 'R', 'I', 'D', 'X', '\0'};
    out.write(magic.data(), magic.size());
    const std::uint32_t version = 1, metric = 1, dimension = 2, lists = 2, probes = 1,
                        reserved = 0;
    const std::uint64_t vectors = 3, centroids_offset = 72, offsets_offset = 88,
                        vectors_offset = 112, file_size = 136;
    for (const auto value : {version, metric, dimension, lists, probes, reserved}) write(out, value);
    for (const auto value : {vectors, centroids_offset, offsets_offset, vectors_offset, file_size})
        write(out, value);
    const std::array<float, 4> centroids{0.0F, 0.0F, 10.0F, 10.0F};
    out.write(reinterpret_cast<const char *>(centroids.data()), sizeof(centroids));
    const std::array<std::uint64_t, 3> offsets{0, 2, 3};
    out.write(reinterpret_cast<const char *>(offsets.data()), sizeof(offsets));
    const std::array<float, 6> values{1.0F, 0.0F, 0.0F, 1.0F, 10.0F, 10.0F};
    out.write(reinterpret_cast<const char *>(values.data()), sizeof(values));
    return path;
}

TEST(RvcFeatureIndex, LoadsSearchesAndBlendsAnIvfFlatStore)
{
#ifdef _WIN32
    GTEST_SKIP() << "RVC feature indexes currently support Linux only";
#endif
    const auto path = makeIndex();
    std::string error;
    auto index = avc::rvc::FeatureIndex::load(path, 2, error);
    ASSERT_NE(index, nullptr) << error;
    EXPECT_EQ(index->dimension(), 2U);
    EXPECT_EQ(index->listCount(), 2U);
    EXPECT_EQ(index->vectorCount(), 3U);

    std::array<float, 2> query{0.9F, 0.1F};
    index->blend(query.data(), 1, 1.0F);
    EXPECT_GT(query[0], 0.99F);
    EXPECT_LT(query[1], 0.01F);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(RvcFeatureIndex, RejectsAModelDimensionMismatch)
{
#ifdef _WIN32
    GTEST_SKIP() << "RVC feature indexes currently support Linux only";
#endif
    const auto path = makeIndex();
    std::string error;
    EXPECT_EQ(avc::rvc::FeatureIndex::load(path, 768, error), nullptr);
    EXPECT_NE(error.find("does not match"), std::string::npos) << error;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

}
