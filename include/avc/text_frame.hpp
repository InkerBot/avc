#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace avc::text {

inline constexpr std::uint32_t kBlockBytes = 4096;

// Text blocks keep the original length-prefixed UTF-8 layout. Segmented producers
// may use the otherwise-unused tail of the block for this footer, so old readers
// still see an ordinary string and old writers remain valid.
inline constexpr std::size_t kFooterBytes = 40;
inline constexpr std::uint64_t kFooterMagic = 0x3154585443564155ULL; // "UAVCTXT1"
inline constexpr std::uint32_t kFinalFlag = 1U;
inline constexpr std::size_t kPlainPayloadBytes =
    kBlockBytes - sizeof(std::uint32_t);
inline constexpr std::size_t kSegmentedPayloadBytes =
    kPlainPayloadBytes - kFooterBytes;

struct Frame {
    std::string_view value;
    std::uint64_t stream = 0;
    std::uint64_t segment = 0;
    std::uint64_t revision = 0;
    bool valid = false;
    bool segmented = false;
    bool final = false;
};

namespace detail {

inline const std::byte *footer(const void *block) noexcept
{
    return static_cast<const std::byte *>(block) + kBlockBytes - kFooterBytes;
}

inline std::byte *footer(void *block) noexcept
{
    return static_cast<std::byte *>(block) + kBlockBytes - kFooterBytes;
}

template <typename T>
inline T load(const std::byte *at) noexcept
{
    T value{};
    std::memcpy(&value, at, sizeof(value));
    return value;
}

template <typename T>
inline void store(std::byte *at, T value) noexcept
{
    std::memcpy(at, &value, sizeof(value));
}

}

inline Frame readFrame(const void *block) noexcept
{
    if (block == nullptr) return {};

    std::uint32_t length = 0;
    std::memcpy(&length, block, sizeof(length));
    if (length > kPlainPayloadBytes) return {};

    Frame frame;
    frame.value = {static_cast<const char *>(block) + sizeof(length), length};
    frame.valid = true;

    if (length > kSegmentedPayloadBytes) return frame;
    const std::byte *footer = detail::footer(block);
    const std::uint64_t magic = detail::load<std::uint64_t>(footer);
    const std::uint32_t footer_length = detail::load<std::uint32_t>(footer + 32);
    if (magic != kFooterMagic || footer_length != length) return frame;

    frame.stream = detail::load<std::uint64_t>(footer + 8);
    frame.segment = detail::load<std::uint64_t>(footer + 16);
    frame.revision = detail::load<std::uint64_t>(footer + 24);
    frame.segmented = frame.stream != 0 && frame.segment != 0;
    frame.final = frame.segmented
                  && (detail::load<std::uint32_t>(footer + 36) & kFinalFlag) != 0;
    return frame;
}

inline void writePlain(void *block, std::string_view value) noexcept
{
    if (block == nullptr) return;
    const auto length = static_cast<std::uint32_t>(
        value.size() < kPlainPayloadBytes ? value.size() : kPlainPayloadBytes);
    std::memcpy(block, &length, sizeof(length));
    if (length > 0) {
        std::memcpy(static_cast<char *>(block) + sizeof(length), value.data(), length);
    }

    // Invalidate metadata left by a previous segmented value when the plain text
    // does not itself occupy the footer bytes.
    if (length <= kSegmentedPayloadBytes) {
        detail::store(detail::footer(block), std::uint64_t{0});
    }
}

inline void writeFrame(void *block, std::string_view value, std::uint64_t stream,
                       std::uint64_t segment, std::uint64_t revision,
                       bool final) noexcept
{
    if (block == nullptr) return;
    const auto length = static_cast<std::uint32_t>(
        value.size() < kSegmentedPayloadBytes ? value.size() : kSegmentedPayloadBytes);
    std::memcpy(block, &length, sizeof(length));
    if (length > 0) {
        std::memcpy(static_cast<char *>(block) + sizeof(length), value.data(), length);
    }

    std::byte *footer = detail::footer(block);
    detail::store(footer, kFooterMagic);
    detail::store(footer + 8, stream);
    detail::store(footer + 16, segment);
    detail::store(footer + 24, revision);
    detail::store(footer + 32, length);
    detail::store(footer + 36, final ? kFinalFlag : std::uint32_t{0});
}

}
