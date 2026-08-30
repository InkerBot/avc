#pragma once

#ifdef _WIN32

#include <memory>
#include <string>

namespace avc::app {
class Daemon;
}

namespace avc::control {

// Runs on its own STA thread so the daemon remains free to service
// engine IPC while UI requests are in flight.
class DesktopHost {
public:
    DesktopHost(app::Daemon &daemon, std::string ui_dir);
    ~DesktopHost();

    DesktopHost(const DesktopHost &) = delete;
    DesktopHost &operator=(const DesktopHost &) = delete;

    bool start();
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
