#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <mutex>
#include <string>

namespace avc::app {

class IpcChannel {
public:
#ifdef _WIN32
    using NativeHandle = std::intptr_t;
    static constexpr NativeHandle kInvalidHandle = 0;
#else
    using NativeHandle = int;
    static constexpr NativeHandle kInvalidHandle = -1;
#endif

    enum class Status {
        Message,
        Timeout,
        Closed,
        Error,
    };

    IpcChannel() = default;
    explicit IpcChannel(NativeHandle handle) noexcept : read_(handle), write_(handle) {}
    IpcChannel(NativeHandle read_handle, NativeHandle write_handle) noexcept
        : read_(read_handle), write_(write_handle)
    {
    }
    ~IpcChannel();

    IpcChannel(const IpcChannel &) = delete;
    IpcChannel &operator=(const IpcChannel &) = delete;

    bool send(const nlohmann::json &message);

    Status receive(nlohmann::json &out, int timeout_ms);

    NativeHandle readHandle() const noexcept { return read_; }
    NativeHandle writeHandle() const noexcept { return write_; }
    bool open() const noexcept
    {
        return read_ != kInvalidHandle && write_ != kInvalidHandle;
    }

    void reset(NativeHandle handle) noexcept { reset(handle, handle); }
    void reset(NativeHandle read_handle, NativeHandle write_handle) noexcept;
    void close() noexcept;

private:
    bool takeLine(std::string &line);

    NativeHandle read_ = kInvalidHandle;
    NativeHandle write_ = kInvalidHandle;
    std::string buffer_;
    std::mutex write_mutex_;
};

}
