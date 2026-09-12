#pragma once

#include "app/Ipc.hpp"
#include "audio/HostAudio.hpp"
#include "control/ExtensionStore.hpp"
#include "control/StateStore.hpp"
#include "graph/GraphSpec.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace avc::control {
class ControlPlane;
#ifdef _WIN32
class DesktopHost;
#endif
}

namespace avc::app {

struct Options;

enum class EngineState : std::uint8_t {
    Starting,
    Up,
    Down,
};

const char *engineStateName(EngineState state) noexcept;

class Daemon {
public:
    explicit Daemon(Options &options);
    ~Daemon();

    Daemon(const Daemon &) = delete;
    Daemon &operator=(const Daemon &) = delete;

    int run();

    struct ApplyResult {
        bool ok = false;
        std::string error;
    };

    ApplyResult applyGraph(const graph::GraphSpec &spec);

    bool setParam(const std::string &node, const std::string &param, float value);

    void setProfiling(bool enabled);
    void resetStats();

    void requestRestart();
    void requestForceRestart();

    nlohmann::json usbIpDriverStatus() const;
    bool installUsbIpDriver(std::string &error, bool trusted_local = false,
                            void *owner_window = nullptr);

    graph::GraphSpec spec() const;

    nlohmann::json descriptors() const;

    nlohmann::json telemetry() const;

    std::vector<audio::DeviceInfo> devices() { return session_.enumerateDevices(); }

    nlohmann::json extensions() const;

    bool setExtensionEnabled(const std::string &key, bool enabled, std::string &error);
    bool setExtensionSettings(const std::string &key, const nlohmann::json &values,
                              std::string &error);
    bool installExtension(const std::string &filename, const std::string &bytes,
                          std::string &error, bool trusted_local = false);
    bool installExtensionFromPath(const std::filesystem::path &path, std::string &error);
    bool removeExtension(const std::string &key, std::string &error);
    void rescanExtensions();

    bool uploadAllowed() const;

    nlohmann::json extensionPresets() const;

    std::optional<graph::GraphSpec> extensionPreset(const std::string &key,
                                                    const std::string &name) const;

    struct ExtensionUiCallResult {
        bool ok = false;
        nlohmann::json data = nullptr;
        std::string error;
    };

    struct ExtensionUiAsset {
        std::string mime_type;
        std::string data;
    };

    std::optional<nlohmann::json> extensionUiManifest(const std::string &key) const;
    std::optional<ExtensionUiAsset> extensionUiAsset(const std::string &key,
                                                     const std::string &digest,
                                                     const std::string &path) const;
    ExtensionUiCallResult callExtensionUi(const std::string &key, const std::string &method,
                                          const nlohmann::json &data);

    std::uint64_t extensionEventCursor() const;
    nlohmann::json extensionEventsAfter(std::uint64_t &cursor) const;

private:
    enum class RestartRequest : std::uint8_t {
        None,
        Graceful,
        Force,
    };

    bool startEngine();
    void stopEngine(bool force = false) noexcept;
    void reapEngine() noexcept;
    bool hasChild() const noexcept
    {
#ifdef _WIN32
        return child_ != nullptr;
#else
        return child_ >= 0;
#endif
    }

    bool givingUp() const noexcept;

    void blameExtension() noexcept;

    void dispatch(const nlohmann::json &message);
    void onGraphReply(const nlohmann::json &message, bool ok);
    void failUiRequests(std::string_view error);

    bool sendGraph(const graph::GraphSpec &spec, bool awaited, std::string &error);

    void retireDevices();

    bool chooseInitialGraph();
    bool resolveTargets();
    std::vector<audio::VirtualDeviceRequest> virtualDevicesFor(const graph::GraphSpec &spec) const;

    void pollGraphFile();
    void persistIfDirty();

    Options &options_;

    audio::HostSession session_;
    audio::HostVirtualDevices published_;
    control::StateStore state_;

    mutable std::mutex ext_mutex_;
    control::ExtensionStore store_;
    std::filesystem::path manifest_path_;
    std::unique_ptr<control::ControlPlane> control_;
#ifdef _WIN32
    std::unique_ptr<control::DesktopHost> desktop_;
#endif

    IpcChannel channel_;
#ifdef _WIN32
    void *child_ = nullptr;
    std::uint32_t child_pid_ = 0;
#else
    int child_ = -1;
#endif

    std::mutex apply_mutex_;

    struct Inflight {
        bool active = false;
        bool awaited = false;
        std::uint64_t id = 0;
        graph::GraphSpec spec;
        std::vector<audio::VirtualDeviceRequest> devices;
    };

    std::uint64_t next_request_ = 0;

    mutable std::mutex mutex_;
    graph::GraphSpec spec_;
    nlohmann::json descriptors_ = nlohmann::json::array();
    nlohmann::json extensions_ = nlohmann::json::array();
    nlohmann::json telemetry_ = nlohmann::json::object();
    EngineState engine_state_ = EngineState::Down;

    bool spec_dirty_ = false;

    std::string graph_error_;

    bool devices_to_retire_ = false;
    std::vector<audio::VirtualDeviceRequest> live_devices_;

    Inflight inflight_;

    struct UiPending {
        bool done = false;
        bool ok = false;
        nlohmann::json data = nullptr;
        std::string error;
    };
    std::map<std::uint64_t, UiPending> ui_pending_;
    std::condition_variable ui_reply_cv_;

    struct UiEvent {
        std::uint64_t sequence = 0;
        nlohmann::json value;
    };
    std::deque<UiEvent> ui_events_;
    std::uint64_t next_ui_event_ = 0;

    std::condition_variable reply_cv_;
    bool reply_ready_ = false;
    bool reply_ok_ = false;
    std::string reply_error_;

    std::atomic<RestartRequest> restart_requested_{RestartRequest::None};

    std::chrono::steady_clock::time_point started_at_{};
    int rapid_failures_ = 0;

    std::filesystem::path watch_path_;
    std::filesystem::file_time_type watch_mtime_{};
};

}
