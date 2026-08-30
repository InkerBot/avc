#include "audio/PwSession.hpp"

#include "log/Log.hpp"

#include <spa/utils/result.h>

#include <cerrno>
#include <cstring>

namespace avc::audio {
namespace {

constexpr int kSyncTimeoutSec = 3;
constexpr int kWaitSec = 5;

}

PwSession::~PwSession()
{
    close();
}

bool PwSession::open(const char *loop_name)
{
    static const pw_core_events core_events = {
        .version = PW_VERSION_CORE_EVENTS,
        .done = &PwSession::onCoreDone,
        .error = &PwSession::onCoreError,
    };

    pw_init(nullptr, nullptr);

    loop_ = pw_thread_loop_new(loop_name, nullptr);
    if (loop_ == nullptr) {
        spdlog::error("pw_thread_loop_new failed");
        return false;
    }

    {
        LoopGuard guard(loop_);
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (context_ == nullptr) {
            spdlog::error("pw_context_new failed");
            return false;
        }

        core_ = pw_context_connect(context_, nullptr, 0);
        if (core_ == nullptr) {
            spdlog::error("cannot connect to PipeWire: {}", std::strerror(errno));
            return false;
        }

        spa_zero(core_listener_);
        pw_core_add_listener(core_, &core_listener_, &core_events, this);
        core_listening_ = true;

        if (!registry_.attach(core_)) {
            return false;
        }
        registry_.setChangeSignal(loop_);
    }

    if (pw_thread_loop_start(loop_) != 0) {
        spdlog::error("pw_thread_loop_start failed");
        return false;
    }

    LoopGuard guard(loop_);
    // The first pass registers the server's objects, including the metadata
    // this binds to; that metadata's properties -- the user's default devices --
    // only arrive on the pass after it.
    return roundtrip() && roundtrip();
}

void PwSession::close() noexcept
{
    if (loop_ == nullptr) {
        return;
    }

    {
        LoopGuard guard(loop_);
        registry_.detach();
        if (core_listening_) {
            spa_hook_remove(&core_listener_);
            core_listening_ = false;
        }
        if (core_ != nullptr) {
            pw_core_disconnect(core_);
            core_ = nullptr;
        }
        if (context_ != nullptr) {
            pw_context_destroy(context_);
            context_ = nullptr;
        }
    }

    pw_thread_loop_stop(loop_);
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    pw_deinit();
}

void PwSession::onCoreDone(void *data, std::uint32_t id, int seq)
{
    auto *self = static_cast<PwSession *>(data);
    if (id != PW_ID_CORE || seq != self->sync_seq_) {
        return;
    }
    self->sync_done_ = true;
    pw_thread_loop_signal(self->loop_, false);
}

void PwSession::onCoreError(void *data, std::uint32_t id, int seq, int res, const char *message)
{
    auto *self = static_cast<PwSession *>(data);
    spdlog::error("PipeWire error on object {} seq {}: {} ({})", id, seq, message,
                  spa_strerror(res));
    if (id == PW_ID_CORE) {
        self->sync_done_ = true;
        pw_thread_loop_signal(self->loop_, false);
    }
}

bool PwSession::roundtrip()
{
    sync_done_ = false;
    sync_seq_ = pw_core_sync(core_, PW_ID_CORE, 0);

    while (!sync_done_) {
        if (pw_thread_loop_timed_wait(loop_, kSyncTimeoutSec) != 0) {
            spdlog::error("PipeWire sync timed out after {}s", kSyncTimeoutSec);
            return false;
        }
    }
    return true;
}

bool PwSession::waitUntil(const std::function<bool()> &ready, const char *what)
{
    timespec deadline{};
    pw_thread_loop_get_time(loop_, &deadline, kWaitSec * SPA_NSEC_PER_SEC);

    while (true) {
        if (!roundtrip()) {
            return false;
        }
        if (ready()) {
            return true;
        }
        if (pw_thread_loop_timed_wait_full(loop_, &deadline) != 0) {
            spdlog::error("{} did not appear within {}s", what, kWaitSec);
            return false;
        }
    }
}

std::vector<DeviceInfo> PwSession::enumerateDevices()
{
    std::vector<DeviceInfo> out;
    if (loop_ == nullptr) {
        return out;
    }

    LoopGuard guard(loop_);
    for (const PwNode &node : registry_.nodes()) {
        if (node.media_class.rfind("Audio/", 0) != 0 && node.media_class.rfind("Stream/", 0) != 0) {
            continue;
        }
        if (node.name.empty()) {
            continue;
        }
        DeviceInfo info;
        info.id = node.id;
        info.name = node.name;
        info.description = node.description;
        info.media_class = node.media_class;
        info.application = node.application;
        info.media_name = node.media_name;
        info.input_ports = registry_.countPorts(node.id, Direction::Input);
        info.output_ports = registry_.countPorts(node.id, Direction::Output);
        out.push_back(std::move(info));
    }
    return out;
}

std::string PwSession::defaultDeviceName(Direction direction)
{
    if (loop_ == nullptr) {
        return {};
    }
    LoopGuard guard(loop_);
    return direction == Direction::Input ? registry_.defaultSource() : registry_.defaultSink();
}

}
