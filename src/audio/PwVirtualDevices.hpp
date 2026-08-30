#pragma once

#include "audio/IoBinder.hpp"
#include "audio/PwSession.hpp"
#include "audio/VirtualDevices.hpp"

#include <pipewire/pipewire.h>

#include <cstdint>
#include <string>
#include <vector>

namespace avc::audio {

class PwVirtualDevices {
public:
    explicit PwVirtualDevices(PwSession &session) noexcept : session_(session) {}
    ~PwVirtualDevices();

    PwVirtualDevices(const PwVirtualDevices &) = delete;
    PwVirtualDevices &operator=(const PwVirtualDevices &) = delete;

    bool ensure(const std::vector<VirtualDeviceRequest> &wanted, std::string &error);

    void retain(const std::vector<VirtualDeviceRequest> &wanted) noexcept;

    void clear() noexcept;

private:
    struct Published {
        std::string owner;
        std::string name;
        IoKind kind = IoKind::VirtualMic;
        std::uint32_t channels = 0;
        pw_proxy *proxy = nullptr;
    };

    static bool sameDevice(const Published &node, const VirtualDeviceRequest &request);

    bool create(const VirtualDeviceRequest &request, std::string &error);
    bool present(const std::vector<VirtualDeviceRequest> &wanted);
    void destroy(Published &node) noexcept;

    PwSession &session_;
    std::vector<Published> nodes_;
};

}
