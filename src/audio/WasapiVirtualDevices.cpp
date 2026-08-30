#include "audio/WasapiVirtualDevices.hpp"

namespace avc::audio {

bool WasapiVirtualDevices::ensure(const std::vector<VirtualDeviceRequest> &wanted,
                                  std::string &error)
{
    if (wanted.empty()) return true;
    error = "virtual microphones and speakers on Windows require a signed virtual audio "
            "driver; remove virtual_mic/virtual_speaker nodes from this graph";
    return false;
}

}
