#include "control/StateStore.hpp"

#include "log/Log.hpp"

#include <cstdlib>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace avc::control {

std::filesystem::path StateStore::defaultDirectory()
{
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA"); local != nullptr && local[0] != '\0') {
        return std::filesystem::path(local) / "avc" / "state";
    }
    if (const char *appdata = std::getenv("APPDATA"); appdata != nullptr && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "avc" / "state";
    }
    return std::filesystem::path(".") / "avc-state";
#else
    if (const char *xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "avc";
    }
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : ".") / ".local" / "state" / "avc";
#endif
}

StateStore::StateStore(std::filesystem::path directory) : directory_(std::move(directory))
{
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        spdlog::warn("cannot create state directory {}: {}", directory_.string(), ec.message());
    }
}

std::filesystem::path StateStore::graphPath() const
{
    return directory_ / "graph.json";
}

std::filesystem::path StateStore::crashPath() const
{
    return directory_ / "crash";
}

bool StateStore::exists() const
{
    std::error_code ec;
    return std::filesystem::exists(graphPath(), ec);
}

void StateStore::save(const graph::GraphSpec &spec) const
{
    // Written aside and renamed, because this file is what a restart depends
    // on: a half-written one would turn "the engine crashed" into "the engine
    // crashed and lost the graph".
    const std::filesystem::path target = graphPath();
    const std::filesystem::path staging = target.string() + ".new";

    {
        std::ofstream file(staging, std::ios::trunc);
        if (!file) {
            spdlog::warn("cannot write {}", staging.string());
            return;
        }
        file << spec.dump() << '\n';
        if (!file) {
            spdlog::warn("cannot write {}", staging.string());
            return;
        }
    }

    std::error_code ec;
#ifdef _WIN32
    if (!MoveFileExW(staging.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }
#else
    std::filesystem::rename(staging, target, ec);
#endif
    if (ec) {
        spdlog::warn("cannot replace {}: {}", target.string(), ec.message());
        std::filesystem::remove(staging, ec);
    }
}

std::optional<graph::GraphSpec> StateStore::load(std::string &error) const
{
    return graph::GraphSpec::load(graphPath().string(), error);
}

}
