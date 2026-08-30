#include "app/Application.hpp"
#include "log/Log.hpp"

#include <atomic>
#include <csignal>

#ifdef _WIN32
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

    avc::log::init(options.log_level);
    installSignalHandlers();

    avc::app::Application app(std::move(options));
    return app.run();
}
