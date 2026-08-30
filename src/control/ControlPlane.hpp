#pragma once

#include "control/PresetStore.hpp"
#include "control/RvcModelManager.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace avc::app {
class Daemon;
}

namespace avc::control {

struct ControlConfig {
    std::string bind = "127.0.0.1";
    int port = 7420;

    std::string ui_dir;
};

class ControlPlane {
public:
    ControlPlane(app::Daemon &daemon, ControlConfig config);
    ~ControlPlane();

    ControlPlane(const ControlPlane &) = delete;
    ControlPlane &operator=(const ControlPlane &) = delete;

    bool start();
    void stop();

    int port() const noexcept { return config_.port; }

private:
    void registerRoutes();
    void registerGraphRoutes();
    void registerPresetRoutes();
    void registerExtensionRoutes();
    void registerRvcRoutes();
    void registerUiRoutes();

    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    app::Daemon &daemon_;
    ControlConfig config_;
    PresetStore presets_;
    RvcModelManager rvc_models_;
};

}
