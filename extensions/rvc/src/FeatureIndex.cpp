#include "avc_rvc/FeatureIndex.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace avc::rvc {
namespace {

constexpr std::array<char, 8> kMagic{'A', 'V', 'C', 'R', 'I', 'D', 'X', '\0'};
constexpr std::size_t kHeaderBytes = 72;
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kMetricL2 = 1;
constexpr std::uint32_t kMaxProbeCount = 32;
constexpr std::size_t kNeighbours = 8;

template <typename T>
T readScalar(const std::byte *bytes, std::size_t offset)
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

bool rangeFits(std::uint64_t offset, std::uint64_t count, std::uint64_t item_size,
               std::uint64_t file_size)
{
    return offset <= file_size && item_size != 0 && count <= (file_size - offset) / item_size;
}

float squaredDistance(const float *left, const float *right, std::size_t size)
{
    float sum = 0.0F;
    for (std::size_t i = 0; i < size; ++i) {
        const float difference = left[i] - right[i];
        sum += difference * difference;
    }
    return sum;
}

}

FeatureIndex::~FeatureIndex()
{
#ifdef _WIN32
    if (mapping_ != nullptr) UnmapViewOfFile(mapping_);
    if (mapping_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(mapping_handle_));
    if (file_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(file_handle_));
#else
    if (mapping_ != nullptr) ::munmap(mapping_, mapping_size_);
    if (fd_ >= 0) ::close(fd_);
#endif
}

std::unique_ptr<FeatureIndex> FeatureIndex::load(const std::filesystem::path &path,
                                                 std::uint32_t expected_dimension,
                                                 std::string &error)
{
    if constexpr (std::endian::native != std::endian::little) {
        error = "RVC feature indexes require a little-endian host";
        return nullptr;
    }

    auto index = std::unique_ptr<FeatureIndex>(new FeatureIndex);
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open feature index: " + path.string();
        return nullptr;
    }
    index->file_handle_ = file;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < static_cast<LONGLONG>(kHeaderBytes)
        || static_cast<unsigned long long>(size.QuadPart)
               > (std::numeric_limits<std::size_t>::max)()) {
        error = "feature index is truncated or too large";
        return nullptr;
    }
    index->mapping_size_ = static_cast<std::size_t>(size.QuadPart);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        error = "cannot create feature index file mapping";
        return nullptr;
    }
    index->mapping_handle_ = mapping;
    index->mapping_ = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (index->mapping_ == nullptr) {
        error = "cannot memory-map feature index";
        return nullptr;
    }
#else
    index->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (index->fd_ < 0) {
        error = "cannot open feature index: " + path.string();
        return nullptr;
    }
    struct stat status {};
    if (::fstat(index->fd_, &status) != 0 || status.st_size < static_cast<off_t>(kHeaderBytes)) {
        error = "feature index is truncated";
        return nullptr;
    }
    index->mapping_size_ = static_cast<std::size_t>(status.st_size);
    index->mapping_ = ::mmap(nullptr, index->mapping_size_, PROT_READ, MAP_PRIVATE, index->fd_, 0);
    if (index->mapping_ == MAP_FAILED) {
        index->mapping_ = nullptr;
        error = "cannot memory-map feature index";
        return nullptr;
    }
#endif

    const auto *bytes = static_cast<const std::byte *>(index->mapping_);
    if (std::memcmp(bytes, kMagic.data(), kMagic.size()) != 0) {
        error = "feature index has an unknown signature";
        return nullptr;
    }
    const std::uint32_t version = readScalar<std::uint32_t>(bytes, 8);
    const std::uint32_t metric = readScalar<std::uint32_t>(bytes, 12);
    index->dimension_ = readScalar<std::uint32_t>(bytes, 16);
    index->list_count_ = readScalar<std::uint32_t>(bytes, 20);
    index->probe_count_ = readScalar<std::uint32_t>(bytes, 24);
    index->vector_count_ = readScalar<std::uint64_t>(bytes, 32);
    const std::uint64_t centroids_offset = readScalar<std::uint64_t>(bytes, 40);
    const std::uint64_t offsets_offset = readScalar<std::uint64_t>(bytes, 48);
    const std::uint64_t vectors_offset = readScalar<std::uint64_t>(bytes, 56);
    const std::uint64_t declared_size = readScalar<std::uint64_t>(bytes, 64);

    if (version != kFormatVersion || metric != kMetricL2) {
        error = "feature index version or distance metric is unsupported";
        return nullptr;
    }
    if (index->dimension_ != expected_dimension || index->dimension_ == 0
        || index->list_count_ == 0 || index->list_count_ > 1000000
        || index->probe_count_ == 0 || index->probe_count_ > kMaxProbeCount
        || index->probe_count_ > index->list_count_ || index->vector_count_ == 0
        || declared_size != index->mapping_size_) {
        error = "feature index metadata is invalid or does not match the model";
        return nullptr;
    }
    if (!rangeFits(centroids_offset,
                   static_cast<std::uint64_t>(index->list_count_) * index->dimension_,
                   sizeof(float), declared_size)
        || !rangeFits(offsets_offset, static_cast<std::uint64_t>(index->list_count_) + 1,
                      sizeof(std::uint64_t), declared_size)
        || !rangeFits(vectors_offset, index->vector_count_,
                      static_cast<std::uint64_t>(index->dimension_) * sizeof(float), declared_size)
        || centroids_offset % alignof(float) != 0
        || offsets_offset % alignof(std::uint64_t) != 0
        || vectors_offset % alignof(float) != 0) {
        error = "feature index tables exceed the file bounds";
        return nullptr;
    }

    index->centroids_ = reinterpret_cast<const float *>(bytes + centroids_offset);
    index->list_offsets_ = reinterpret_cast<const std::uint64_t *>(bytes + offsets_offset);
    index->vectors_ = reinterpret_cast<const float *>(bytes + vectors_offset);
    if (index->list_offsets_[0] != 0
        || index->list_offsets_[index->list_count_] != index->vector_count_) {
        error = "feature index list offsets are invalid";
        return nullptr;
    }
    for (std::uint32_t i = 0; i < index->list_count_; ++i) {
        if (index->list_offsets_[i] > index->list_offsets_[i + 1]) {
            error = "feature index list offsets are not monotonic";
            return nullptr;
        }
    }
    return index;
}

void FeatureIndex::blend(float *features, std::size_t rows, float rate) const
{
    rate = std::clamp(rate, 0.0F, 1.0F);
    if (features == nullptr || rows == 0 || rate <= 0.0F) return;

    struct Candidate {
        float distance = std::numeric_limits<float>::infinity();
        const float *vector = nullptr;
    };

    const std::size_t probes = std::min<std::size_t>(probe_count_, list_count_);
    for (std::size_t row = 0; row < rows; ++row) {
        float *query = features + row * dimension_;
        std::vector<std::pair<float, std::uint32_t>> coarse;
        coarse.reserve(probes);
        for (std::uint32_t list = 0; list < list_count_; ++list) {
            const float distance = squaredDistance(query, centroids_ + list * dimension_, dimension_);
            if (!std::isfinite(distance)) continue;
            if (coarse.size() < probes) {
                coarse.emplace_back(distance, list);
                if (coarse.size() == probes) std::ranges::make_heap(coarse);
            } else if (distance < coarse.front().first) {
                std::ranges::pop_heap(coarse);
                coarse.back() = {distance, list};
                std::ranges::push_heap(coarse);
            }
        }

        std::array<Candidate, kNeighbours> nearest{};
        std::size_t found = 0;
        for (const auto &[unused, list] : coarse) {
            (void)unused;
            for (std::uint64_t at = list_offsets_[list]; at < list_offsets_[list + 1]; ++at) {
                const float *candidate = vectors_ + at * dimension_;
                const float distance = squaredDistance(query, candidate, dimension_);
                if (!std::isfinite(distance)) continue;
                if (found < nearest.size()) {
                    nearest[found++] = {distance, candidate};
                    if (found == nearest.size()) {
                        std::ranges::make_heap(nearest, {}, &Candidate::distance);
                    }
                } else if (distance < nearest.front().distance) {
                    std::ranges::pop_heap(nearest, {}, &Candidate::distance);
                    nearest.back() = {distance, candidate};
                    std::ranges::push_heap(nearest, {}, &Candidate::distance);
                }
            }
        }
        if (found == 0) continue;

        std::vector<double> mixed(dimension_, 0.0);
        double weight_sum = 0.0;
        for (std::size_t i = 0; i < found; ++i) {
            // Faiss IndexIVFFlat returns squared L2. Upstream RVC squares its
            // reciprocal once more before normalization.
            const double reciprocal = 1.0 / std::max<double>(nearest[i].distance, 1e-12);
            const double weight = reciprocal * reciprocal;
            weight_sum += weight;
            for (std::size_t channel = 0; channel < dimension_; ++channel) {
                mixed[channel] += static_cast<double>(nearest[i].vector[channel]) * weight;
            }
        }
        if (!std::isfinite(weight_sum) || weight_sum <= 0.0) continue;
        for (std::size_t channel = 0; channel < dimension_; ++channel) {
            const float retrieved = static_cast<float>(mixed[channel] / weight_sum);
            query[channel] += (retrieved - query[channel]) * rate;
        }
    }
}

}
