#pragma once

#include "types/Audio.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace avc::node {

enum class PortTransport : std::uint8_t {
    Stream,
    Value,
};

struct PortTypeDescriptor {
    std::string name;
    std::string label;
    PortTransport transport = PortTransport::Stream;

    std::uint32_t bytes = 0;

    std::string extension;

    std::uint32_t blockBytes(std::uint32_t frames) const noexcept
    {
        return transport == PortTransport::Stream ? bytes * frames : bytes;
    }
};

inline constexpr const char *kAudioPortType = "audio";
inline constexpr std::uint32_t kAudioPortTypeIndex = 0;

inline constexpr const char *kTextPortType = "text";
inline constexpr std::uint32_t kTextPortTypeBytes = 4096;

class PortTypeManifest {
public:
    static PortTypeManifest &instance();

    bool add(PortTypeDescriptor descriptor, std::string &error);

    void seal() noexcept { sealed_ = true; }
    bool sealed() const noexcept { return sealed_; }

    int indexOf(std::string_view name) const noexcept;

    const PortTypeDescriptor *find(std::string_view name) const noexcept;

    const PortTypeDescriptor &at(std::uint32_t index) const noexcept { return types_[index]; }

    std::size_t size() const noexcept { return types_.size(); }
    const std::vector<PortTypeDescriptor> &all() const noexcept { return types_; }

private:
    PortTypeManifest();

    std::vector<PortTypeDescriptor> types_;
    bool sealed_ = false;
};

}
