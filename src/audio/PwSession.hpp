#pragma once

#include "audio/AudioBackend.hpp"
#include "audio/PwRegistry.hpp"

#include <pipewire/pipewire.h>

#include <functional>
#include <string>
#include <vector>

namespace avc::audio {

class LoopGuard {
public:
    explicit LoopGuard(pw_thread_loop *loop) noexcept : loop_(loop) { pw_thread_loop_lock(loop_); }
    ~LoopGuard() { pw_thread_loop_unlock(loop_); }

    LoopGuard(const LoopGuard &) = delete;
    LoopGuard &operator=(const LoopGuard &) = delete;

private:
    pw_thread_loop *loop_;
};

class PwSession {
public:
    PwSession() = default;
    ~PwSession();

    PwSession(const PwSession &) = delete;
    PwSession &operator=(const PwSession &) = delete;

    bool open(const char *loop_name);
    void close() noexcept;

    bool valid() const noexcept { return loop_ != nullptr; }

    bool roundtrip();

    bool waitUntil(const std::function<bool()> &ready, const char *what);

    std::vector<DeviceInfo> enumerateDevices();
    std::string defaultDeviceName(Direction direction);

    pw_thread_loop *loop() const noexcept { return loop_; }
    pw_core *core() const noexcept { return core_; }
    PwRegistry &registry() noexcept { return registry_; }
    const PwRegistry &registry() const noexcept { return registry_; }

private:
    static void onCoreDone(void *data, std::uint32_t id, int seq);
    static void onCoreError(void *data, std::uint32_t id, int seq, int res, const char *message);

    pw_thread_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;

    spa_hook core_listener_{};
    bool core_listening_ = false;

    PwRegistry registry_;

    int sync_seq_ = 0;
    bool sync_done_ = false;
};

}
