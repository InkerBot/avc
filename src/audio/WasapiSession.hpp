#pragma once

#include "audio/AudioBackend.hpp"

#include <memory>
#include <string>
#include <vector>

namespace avc::audio {

class WasapiSession {
public:
    WasapiSession();
    ~WasapiSession();

    WasapiSession(const WasapiSession &) = delete;
    WasapiSession &operator=(const WasapiSession &) = delete;

    bool open(const char *name);
    void close() noexcept;
    bool valid() const noexcept;

    std::vector<DeviceInfo> enumerateDevices();
    std::string defaultDeviceName(Direction direction);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
