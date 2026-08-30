#pragma once

#include "audio/AudioBackend.hpp"
#include "audio/IoBinder.hpp"

#include <memory>

namespace avc::audio {

class WasapiBackend final : public AudioBackend, public IoBinder {
public:
    WasapiBackend();
    ~WasapiBackend() override;

    void setForceQuantum(bool force) noexcept;
    void setNodeName(std::string name);

    bool open() override;
    void close() noexcept override;
    std::vector<DeviceInfo> enumerateDevices() override;
    std::string defaultDeviceName(Direction direction) override;
    bool start(const types::AudioFormat &format, Processor *processor) override;
    void stop() noexcept override;
    BackendStats stats() const noexcept override;
    void resetPeaks() noexcept override;

    bool bindIo(const std::vector<IoRequest> &requests,
                std::map<std::string, std::vector<std::uint32_t>> &slots,
                std::string &error) override;
    void releaseIo(const std::set<std::string> &keep) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
