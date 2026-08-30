#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace avc::rvc {

class FeatureIndex {
public:
    ~FeatureIndex();

    FeatureIndex(const FeatureIndex &) = delete;
    FeatureIndex &operator=(const FeatureIndex &) = delete;

    static std::unique_ptr<FeatureIndex> load(const std::filesystem::path &path,
                                               std::uint32_t expected_dimension,
                                               std::string &error);

    std::uint32_t dimension() const noexcept { return dimension_; }
    std::uint32_t listCount() const noexcept { return list_count_; }
    std::uint64_t vectorCount() const noexcept { return vector_count_; }

    void blend(float *features, std::size_t rows, float rate) const;

private:
    FeatureIndex() = default;

#ifdef _WIN32
    void *file_handle_ = nullptr;
    void *mapping_handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    void *mapping_ = nullptr;
    std::size_t mapping_size_ = 0;
    std::uint32_t dimension_ = 0;
    std::uint32_t list_count_ = 0;
    std::uint32_t probe_count_ = 0;
    std::uint64_t vector_count_ = 0;
    const float *centroids_ = nullptr;
    const std::uint64_t *list_offsets_ = nullptr;
    const float *vectors_ = nullptr;
};

}
