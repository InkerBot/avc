#include "audio/WasapiVirtualDevices.hpp"

namespace avc::audio {

bool WasapiVirtualDevices::ensure(const std::vector<VirtualDeviceRequest> &wanted,
                                  std::string &error)
{
    return usbip_.publish(wanted, error);
}

void WasapiVirtualDevices::retain(const std::vector<VirtualDeviceRequest> &wanted) noexcept
{
    usbip_.retain(wanted);
}

void WasapiVirtualDevices::clear() noexcept
{
    usbip_.clear();
}

}
