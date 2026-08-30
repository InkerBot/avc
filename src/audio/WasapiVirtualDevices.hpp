#pragma once

#include "audio/VirtualDevices.hpp"
#include "audio/WasapiSession.hpp"

#include <string>
#include <vector>

namespace avc::audio {

class WasapiVirtualDevices {
public:
    explicit WasapiVirtualDevices(WasapiSession &) noexcept {}

    bool ensure(const std::vector<VirtualDeviceRequest> &wanted, std::string &error);
    void retain(const std::vector<VirtualDeviceRequest> &) noexcept {}
    void clear() noexcept {}
};

}
