#pragma once

#include "audio/IoBinder.hpp"
#include "audio/VirtualDevices.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace avc::audio {

struct UsbIpDriverStatus {
    bool client_present = false;
    bool driver_ready = false;
    bool compatible = false;
    bool installer_available = false;
    std::string version;
    std::string error;
};

// Reports the installed client/driver separately: usbip.exe can remain on disk
// after a failed driver installation. The bundled installer is available only
// in Windows x64 builds configured with AVC_BUNDLE_USBIP_DRIVER_INSTALLER.
UsbIpDriverStatus queryUsbIpDriverStatus();
bool installBundledUsbIpDriver(std::string &error, void *owner_window = nullptr);

class UsbIpAudioDevice {
public:
    struct Impl;

    ~UsbIpAudioDevice();

    UsbIpAudioDevice(const UsbIpAudioDevice &) = delete;
    UsbIpAudioDevice &operator=(const UsbIpAudioDevice &) = delete;

    void pullPlayback(std::uint32_t channel, float *output, std::size_t frames) noexcept;
    void pushCapture(std::uint32_t channel, const float *input, std::size_t frames) noexcept;

    std::uint32_t channels() const noexcept;

    explicit UsbIpAudioDevice(std::unique_ptr<Impl> impl) noexcept;

private:
    friend class UsbIpAudioManager;
    std::unique_ptr<Impl> impl_;
};

class UsbIpAudioManager {
public:
    explicit UsbIpAudioManager(bool publisher = false);
    ~UsbIpAudioManager();

    UsbIpAudioManager(const UsbIpAudioManager &) = delete;
    UsbIpAudioManager &operator=(const UsbIpAudioManager &) = delete;

    bool ensure(const std::vector<IoRequest> &requests, std::uint32_t sample_rate,
                std::string &error);
    std::shared_ptr<UsbIpAudioDevice> find(const IoRequest &request) const;

    // Daemon-side: owns the USB/IP server and visible Windows devices. New
    // devices are attached before a graph swap; obsolete devices are detached
    // only after the engine accepted that graph.
    bool publish(const std::vector<VirtualDeviceRequest> &wanted, std::string &error);
    void retain(const std::vector<VirtualDeviceRequest> &wanted) noexcept;
    void clear() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Hidden, elevated child mode used to perform all usbip.exe detach/attach
// operations under one UAC consent prompt.
int runUsbIpDeviceHelper(std::string_view operation, std::string_view buses);

}
