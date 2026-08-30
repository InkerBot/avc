#include "control/PresetStore.hpp"

#include "log/Log.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace avc::control {
namespace {

constexpr std::size_t kMaxNameLength = 64;

}

std::filesystem::path PresetStore::defaultDirectory()
{
#ifdef _WIN32
    if (const char *appdata = std::getenv("APPDATA"); appdata != nullptr && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "avc" / "presets";
    }
    return std::filesystem::path(".") / "avc-presets";
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "avc" / "presets";
    }
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : ".") / ".config" / "avc" / "presets";
#endif
}

PresetStore::PresetStore(std::filesystem::path directory) : directory_(std::move(directory))
{
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        spdlog::warn("cannot create preset directory {}: {}", directory_.string(), ec.message());
    }
}

bool PresetStore::validName(const std::string &name)
{
    if (name.empty() || name.size() > kMaxNameLength) {
        return false;
    }
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '-' || c == '_' || c == ' ';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::filesystem::path PresetStore::pathOf(const std::string &name) const
{
    return directory_ / (name + ".json");
}

std::vector<std::string> PresetStore::list() const
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(directory_, ec)) {
        if (entry.path().extension() == ".json") {
            names.push_back(entry.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool PresetStore::save(const std::string &name, const graph::GraphSpec &spec, std::string &error)
{
    if (!validName(name)) {
        error = "invalid preset name";
        return false;
    }
    std::ofstream file(pathOf(name));
    if (!file) {
        error = "cannot write " + pathOf(name).string();
        return false;
    }
    file << spec.dump() << '\n';
    return true;
}

std::optional<graph::GraphSpec> PresetStore::load(const std::string &name,
                                                  std::string &error) const
{
    if (!validName(name)) {
        error = "invalid preset name";
        return std::nullopt;
    }
    return graph::GraphSpec::load(pathOf(name).string(), error);
}

bool PresetStore::remove(const std::string &name, std::string &error)
{
    if (!validName(name)) {
        error = "invalid preset name";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::remove(pathOf(name), ec)) {
        error = "no such preset";
        return false;
    }
    return true;
}

}
