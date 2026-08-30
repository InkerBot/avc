#pragma once

#include "audio/IoBinder.hpp"

#include <cstdint>
#include <string>

namespace avc::audio {

struct VirtualDeviceRequest {
    std::string owner;
    std::string name;
    IoKind kind = IoKind::VirtualMic;
    std::uint32_t channels = 1;
};

}
