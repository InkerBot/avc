#include "app/Application.hpp"
#include "log/Log.hpp"

#include <atomic>
#include <csignal>

#ifdef _WIN32
#include "audio/UsbIpAudio.hpp"

#include <windows.h>
#endif

namespace avc::app {

std::atomic<bool> g_running{true};

}

namespace {

#ifdef _WIN32
BOOL WINAPI onConsoleEvent(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT
        || event == CTRL_SHUTDOWN_EVENT) {
        avc::app::g_running.store(false, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}
#else
extern "C" void onSignal(int )
{
    avc::app::g_running.store(false, std::memory_order_relaxed);
}
#endif

void installSignalHandlers()
{
#ifdef _WIN32
    SetConsoleCtrlHandler(&onConsoleEvent, TRUE);
#else
    struct sigaction action{};
    action.sa_handler = &onSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif
}

#ifdef _WIN32
void hidePrivateDesktopConsole()
{
    // A console-subsystem binary keeps command-line diagnostics working when
    // launched from a terminal. Explorer gives it a brand-new console of its
    // own; hide only that private window so normal desktop launches do not
    // leave a terminal beside the editor.
    DWORD processes[2]{};
    if (GetConsoleWindow() != nullptr && GetConsoleProcessList(processes, 2) == 1) {
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    }
}
#endif

}

int main(int argc, char **argv)
{
    avc::app::Options options;
    avc::log::init("info");

    if (!avc::app::parseOptions(argc, argv, options)) {
        avc::app::printUsage();
        return 2;
    }
    if (options.help) {
        avc::app::printUsage();
        return 0;
    }
#ifdef _WIN32
    if (!options.usbip_operation.empty()) {
        return avc::audio::runUsbIpDeviceHelper(options.usbip_operation, options.usbip_buses);
    }
    if (options.desktop) {
        hidePrivateDesktopConsole();
    }
#endif

    avc::log::init(options.log_level);
    installSignalHandlers();

    avc::app::Application app(std::move(options));
    return app.run();
}
