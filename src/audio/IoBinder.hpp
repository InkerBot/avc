#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace avc::audio {

enum class IoKind : std::uint8_t {
    Capture,
    Playback,
    VirtualSpeaker,
    VirtualMic,
};

constexpr bool producesSignal(IoKind kind) noexcept
{
    return kind == IoKind::Capture || kind == IoKind::VirtualSpeaker;
}

constexpr bool isVirtual(IoKind kind) noexcept
{
    return kind == IoKind::VirtualSpeaker || kind == IoKind::VirtualMic;
}

struct IoRequest {
    std::string node;
    IoKind kind = IoKind::Capture;
    std::string target;
    std::uint32_t channels = 1;

    bool exclusive = false;
};

class IoBinder {
public:
    virtual ~IoBinder() = default;

    virtual bool bindIo(const std::vector<IoRequest> &requests,
                        std::map<std::string, std::vector<std::uint32_t>> &slots,
                        std::string &error) = 0;

    virtual void releaseIo(const std::set<std::string> &keep) = 0;
};

}
