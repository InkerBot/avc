#pragma once

#include "audio/VirtualDevices.hpp"
#include "audio/UsbIpAudio.hpp"
#include "audio/WasapiSession.hpp"

#include <string>
#include <vector>

namespace avc::audio {

class WasapiVirtualDevices {
public:
    explicit WasapiVirtualDevices(WasapiSession &) noexcept : usbip_(true) {}

    bool ensure(const std::vector<VirtualDeviceRequest> &wanted, std::string &error);
    void retain(const std::vector<VirtualDeviceRequest> &wanted) noexcept;
    void clear() noexcept;

private:
    UsbIpAudioManager usbip_;
};

}
