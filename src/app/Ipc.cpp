#include "app/Ipc.hpp"

#include "log/Log.hpp"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <limits>
#include <thread>

namespace avc::app {
namespace {

constexpr std::size_t kMaxLine = 16U * 1024U * 1024U;

constexpr std::size_t kChunk = 16384;

}

IpcChannel::~IpcChannel()
{
    close();
}

void IpcChannel::close() noexcept
{
    reset(kInvalidHandle, kInvalidHandle);
}

void IpcChannel::reset(NativeHandle read_handle, NativeHandle write_handle) noexcept
{
    const std::lock_guard<std::mutex> lock(write_mutex_);
#ifdef _WIN32
    if (read_ != kInvalidHandle) {
        CloseHandle(reinterpret_cast<HANDLE>(read_));
    }
    if (write_ != kInvalidHandle && write_ != read_) {
        CloseHandle(reinterpret_cast<HANDLE>(write_));
    }
#else
    if (read_ != kInvalidHandle) {
        ::close(read_);
    }
    if (write_ != kInvalidHandle && write_ != read_) {
        ::close(write_);
    }
#endif
    read_ = read_handle;
    write_ = write_handle;
    buffer_.clear();
}

bool IpcChannel::send(const nlohmann::json &message)
{
    std::string line = message.dump();
    line.push_back('\n');

    const std::lock_guard<std::mutex> lock(write_mutex_);
    if (!open()) {
        return false;
    }
    std::size_t sent = 0;
    while (sent < line.size()) {
#ifdef _WIN32
        DWORD written = 0;
        const DWORD wanted = static_cast<DWORD>(
            std::min<std::size_t>(line.size() - sent, std::numeric_limits<DWORD>::max()));
        if (!WriteFile(reinterpret_cast<HANDLE>(write_), line.data() + sent, wanted, &written,
                       nullptr)
            || written == 0) {
            return false;
        }
        sent += written;
#else
        // MSG_NOSIGNAL rather than ignoring SIGPIPE process-wide: a peer that
        // died must be an error this call returns, not a signal that takes the
        // whole process with it.
        const ssize_t n =
            ::send(write_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
#endif
    }
    return true;
}

bool IpcChannel::takeLine(std::string &line)
{
    const std::size_t end = buffer_.find('\n');
    if (end == std::string::npos) {
        return false;
    }
    line.assign(buffer_, 0, end);
    buffer_.erase(0, end + 1);
    return true;
}

IpcChannel::Status IpcChannel::receive(nlohmann::json &out, int timeout_ms)
{
    if (!open()) {
        return Status::Closed;
    }

    while (true) {
        std::string line;
        if (takeLine(line)) {
            out = nlohmann::json::parse(line, nullptr, false);
            if (out.is_discarded()) {
                // One unparseable line is not worth tearing the channel down
                // for; the next one is probably fine.
                spdlog::warn("ipc: dropping a message that is not valid JSON");
                continue;
            }
            return Status::Message;
        }

#ifdef _WIN32
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(std::max(timeout_ms, 0));
        DWORD available = 0;
        while (!PeekNamedPipe(reinterpret_cast<HANDLE>(read_), nullptr, 0, nullptr, &available,
                              nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                return Status::Closed;
            }
            return Status::Error;
        }
        while (available == 0) {
            if (timeout_ms == 0 || std::chrono::steady_clock::now() >= deadline) {
                return Status::Timeout;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!PeekNamedPipe(reinterpret_cast<HANDLE>(read_), nullptr, 0, nullptr, &available,
                               nullptr)) {
                const DWORD error = GetLastError();
                return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED
                           ? Status::Closed
                           : Status::Error;
            }
        }
        char chunk[kChunk];
        DWORD n = 0;
        if (!ReadFile(reinterpret_cast<HANDLE>(read_), chunk,
                      static_cast<DWORD>(std::min<std::size_t>(sizeof(chunk), available)), &n,
                      nullptr)) {
            const DWORD error = GetLastError();
            return error == ERROR_BROKEN_PIPE ? Status::Closed : Status::Error;
        }
        if (n == 0) return Status::Closed;
#else
        pollfd watch{};
        watch.fd = read_;
        watch.events = POLLIN;
        const int ready = ::poll(&watch, 1, timeout_ms);
        if (ready == 0) {
            return Status::Timeout;
        }
        if (ready < 0) {
            // A signal is how shutdown reaches a blocked read. Report it as a
            // timeout so the caller gets to re-check whether it is still
            // supposed to be running.
            return errno == EINTR ? Status::Timeout : Status::Error;
        }

        char chunk[kChunk];
        const ssize_t n = ::read(read_, chunk, sizeof(chunk));
        if (n == 0) {
            return Status::Closed;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return Status::Timeout;
            }
            spdlog::error("ipc: read failed: {}", std::strerror(errno));
            return Status::Error;
        }
#endif

        if (buffer_.size() + static_cast<std::size_t>(n) > kMaxLine) {
            spdlog::error("ipc: peer sent more than {} bytes without a newline", kMaxLine);
            return Status::Error;
        }
        buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

}
