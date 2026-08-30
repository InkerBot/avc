#include "control/ExtensionStore.hpp"

#include "log/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <set>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace avc::control {
namespace {

using nlohmann::json;

#ifdef _WIN32
constexpr const char *kLibrarySuffix = ".dll";

std::filesystem::path executableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return {};
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}
#else
constexpr const char *kLibrarySuffix = ".so";
constexpr const char *kSystemDirectory = "/usr/lib/avc/extensions";
#endif
constexpr std::size_t kMaxFilenameLength = 128;

#ifndef _WIN32
std::filesystem::path xdgPath(const char *variable, const char *fallback, const char *tail)
{
    if (const char *value = std::getenv(variable); value != nullptr && value[0] != '\0') {
        return std::filesystem::path(value) / tail;
    }
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : ".") / fallback / tail;
}
#endif

std::int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}

std::filesystem::path ExtensionStore::defaultConfigPath()
{
#ifdef _WIN32
    const char *root = std::getenv("APPDATA");
    return std::filesystem::path(root != nullptr ? root : ".") / "avc" / "extensions.json";
#else
    return xdgPath("XDG_CONFIG_HOME", ".config", "avc") / "extensions.json";
#endif
}

std::filesystem::path ExtensionStore::userDirectory()
{
#ifdef _WIN32
    const char *root = std::getenv("LOCALAPPDATA");
    return std::filesystem::path(root != nullptr ? root : ".") / "avc" / "extensions";
#else
    return xdgPath("XDG_DATA_HOME", ".local/share", "avc") / "extensions";
#endif
}

std::filesystem::path ExtensionStore::configRoot()
{
#ifdef _WIN32
    const char *root = std::getenv("LOCALAPPDATA");
    return std::filesystem::path(root != nullptr ? root : ".") / "avc" / "ext";
#else
    return xdgPath("XDG_DATA_HOME", ".local/share", "avc") / "ext";
#endif
}

bool ExtensionStore::validFilename(const std::string &name)
{
    if (name.size() <= std::strlen(kLibrarySuffix) || name.size() > kMaxFilenameLength
        || !name.ends_with(kLibrarySuffix)) {
        return false;
    }
    // No separators and no leading dot: between them that is every way a name
    // could name something other than a file in the directory it is joined to.
    if (name.front() == '.') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
               || c == '.' || c == '-' || c == '_';
    });
}

ExtensionStore::ExtensionStore(std::vector<std::string> extra)
    : extra_(std::move(extra)), config_path_(defaultConfigPath())
{
    std::error_code ec;
    std::filesystem::create_directories(config_path_.parent_path(), ec);
    std::filesystem::create_directories(userDirectory(), ec);
    std::filesystem::create_directories(configRoot(), ec);
    load();
    rescan();
}

void ExtensionStore::load()
{
    records_.clear();
    std::ifstream file(config_path_);
    if (!file) {
        return;
    }
    const json config = json::parse(file, nullptr, false);
    if (config.is_discarded() || !config.is_object()) {
        spdlog::warn("{} is not readable; starting from defaults", config_path_.string());
        return;
    }

    const json extensions = config.value("extensions", json::object());
    for (auto it = extensions.cbegin(); it != extensions.cend(); ++it) {
        Record record;
        record.enabled = it->value("enabled", true);
        record.disabled_reason = it->value("disabledReason", std::string{});
        record.last_crash = it->value("lastCrash", static_cast<std::int64_t>(0));
        const json settings = it->value("settings", json::object());
        for (auto s = settings.cbegin(); s != settings.cend(); ++s) {
            record.settings[s.key()] = s->is_string() ? s->get<std::string>() : s->dump();
        }
        records_[it.key()] = std::move(record);
    }
}

void ExtensionStore::save() const
{
    json extensions = json::object();
    for (const auto &[key, record] : records_) {
        json entry{{"enabled", record.enabled}, {"settings", record.settings}};
        if (!record.disabled_reason.empty()) {
            entry["disabledReason"] = record.disabled_reason;
        }
        if (record.last_crash > 0) {
            entry["lastCrash"] = record.last_crash;
        }
        extensions[key] = std::move(entry);
    }

    // Written aside and renamed: this file is what decides which code the
    // engine loads, and a half-written one is not something to start from.
    const std::filesystem::path staging = config_path_.string() + ".new";
    {
        std::ofstream file(staging, std::ios::trunc);
        if (!file) {
            spdlog::warn("cannot write {}", staging.string());
            return;
        }
        file << json{{"version", 1}, {"extensions", std::move(extensions)}}.dump(2) << '\n';
    }
    std::error_code ec;
#ifdef _WIN32
    if (!MoveFileExW(staging.c_str(), config_path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }
#else
    std::filesystem::rename(staging, config_path_, ec);
#endif
    if (ec) {
        spdlog::warn("cannot replace {}: {}", config_path_.string(), ec.message());
        std::filesystem::remove(staging, ec);
    }
}

void ExtensionStore::rescan()
{
    dirs_.clear();
    for (const std::string &dir : extra_) {
        dirs_.emplace_back(dir);
    }
    dirs_.push_back(userDirectory());
#ifdef _WIN32
    const std::filesystem::path executable_dir = executableDirectory();
    if (!executable_dir.empty()) dirs_.push_back(executable_dir / "extensions");
    if (const char *program_files = std::getenv("ProgramFiles"); program_files != nullptr) {
        dirs_.emplace_back(std::filesystem::path(program_files) / "avc" / "extensions");
    }
#else
    dirs_.emplace_back(kSystemDirectory);
#endif

    // First directory wins, the way PATH does: pointing --extension-dir at a
    // build tree is how you try a new version of something already installed.
    found_.clear();
    std::set<std::string> taken;
    for (const std::filesystem::path &dir : dirs_) {
        std::error_code ec;
        std::vector<std::filesystem::path> entries;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == kLibrarySuffix) {
                entries.push_back(entry.path());
            }
        }
        std::sort(entries.begin(), entries.end());
        for (const std::filesystem::path &path : entries) {
            const std::string key = path.stem().string();
            if (taken.insert(key).second) {
                found_.push_back({key, path});
            }
        }
    }
}

const ExtensionStore::Record *ExtensionStore::recordFor(const std::string &key) const
{
    const auto found = records_.find(key);
    return found != records_.end() ? &found->second : nullptr;
}

const ExtensionStore::Found *ExtensionStore::findFile(const std::string &key) const
{
    for (const Found &entry : found_) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

nlohmann::json ExtensionStore::engineManifest() const
{
    json extensions = json::array();
    for (const Found &entry : found_) {
        const Record *record = recordFor(entry.key);
        // No record means nobody has said anything about it, which for a file
        // somebody put in an extensions directory means yes. An upload writes
        // a record saying no, because dropping a file somewhere and agreeing to
        // run it are not the same act.
        if (record != nullptr && !record->enabled) {
            continue;
        }
        extensions.push_back({
            {"path", entry.path.string()},
            {"settings", record != nullptr ? json(record->settings) : json::object()},
        });
    }
    return json{{"configRoot", configRoot().string()}, {"extensions", std::move(extensions)}};
}

bool ExtensionStore::writeManifest(const std::filesystem::path &path) const
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        spdlog::warn("cannot write the extension list to {}", path.string());
        return false;
    }
    file << engineManifest().dump() << '\n';
    return static_cast<bool>(file);
}

nlohmann::json ExtensionStore::describe(const nlohmann::json &reported) const
{
    std::map<std::string, const json *> by_path;
    if (reported.is_array()) {
        for (const json &entry : reported) {
            by_path[entry.value("path", std::string{})] = &entry;
        }
    }

    json out = json::array();
    for (const Found &entry : found_) {
        const Record *record = recordFor(entry.key);
        const bool enabled = record == nullptr || record->enabled;

        json item{
            {"key", entry.key},
            {"path", entry.path.string()},
            {"enabled", enabled},
            {"disabledReason", record != nullptr ? record->disabled_reason : std::string{}},
            {"lastCrash", record != nullptr ? record->last_crash : 0},
            {"values", record != nullptr ? json(record->settings) : json::object()},
        };

        const auto said = by_path.find(entry.path.string());
        if (said != by_path.end()) {
            const json &engine = *said->second;
            item["state"] = engine.value("loaded", false) ? "loaded" : "failed";
            item["error"] = engine.value("error", std::string{});
            item["id"] = engine.value("id", std::string{});
            item["name"] = engine.value("name", std::string{});
            item["version"] = engine.value("version", std::string{});
            item["author"] = engine.value("author", std::string{});
            item["description"] = engine.value("description", std::string{});
            item["abiVersion"] = engine.value("abiVersion", 0);
            item["nodeTypes"] = engine.value("nodeTypes", json::array());
            item["settings"] = engine.value("settings", json::array());
            item["presets"] = engine.value("presets", json::array());
            item["hasUi"] = !engine.value("ui", json::object())
                                  .value("entry", std::string{}).empty();
            item["uiDigest"] = engine.value("ui", json::object())
                                   .value("digest", std::string{});
        } else {
            item["state"] = enabled ? "pending" : "disabled";
            item["error"] = "";
            item["id"] = "";
            item["name"] = entry.key;
            item["version"] = "";
            item["author"] = "";
            item["description"] = "";
            item["abiVersion"] = 0;
            item["nodeTypes"] = json::array();
            item["settings"] = json::array();
            item["presets"] = json::array();
            item["hasUi"] = false;
            item["uiDigest"] = "";
        }
        out.push_back(std::move(item));
    }
    return out;
}

bool ExtensionStore::setEnabled(const std::string &key, bool enabled, std::string &error)
{
    if (findFile(key) == nullptr) {
        error = "no extension called '" + key + "'";
        return false;
    }
    Record &record = records_[key];
    record.enabled = enabled;
    // Turning it back on clears whatever turned it off, including a crash: the
    // user has seen the reason and decided anyway.
    record.disabled_reason = enabled ? std::string{} : "user";
    save();
    return true;
}

bool ExtensionStore::setSettings(const std::string &key, const nlohmann::json &values,
                                 std::string &error)
{
    if (findFile(key) == nullptr) {
        error = "no extension called '" + key + "'";
        return false;
    }
    if (!values.is_object()) {
        error = "expected {values: {...}}";
        return false;
    }

    Record &record = records_[key];
    record.settings.clear();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        record.settings[it.key()] = it->is_string() ? it->get<std::string>() : it->dump();
    }
    save();
    return true;
}

bool ExtensionStore::remove(const std::string &key, std::string &error)
{
    const Found *entry = findFile(key);
    if (entry == nullptr) {
        error = "no extension called '" + key + "'";
        return false;
    }
    const Record *record = recordFor(key);
    if (record == nullptr || record->enabled) {
        error = "'" + key + "' is still enabled; disable it first";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::remove(entry->path, ec)) {
        error = "cannot delete " + entry->path.string()
                + (ec ? ": " + ec.message() : " (it may not be yours to delete)");
        return false;
    }
    records_.erase(key);
    save();
    rescan();
    return true;
}

bool ExtensionStore::install(const std::string &filename, const std::string &bytes,
                             std::string &error)
{
    if (!validFilename(filename)) {
        error = std::string("a name like 'thing") + kLibrarySuffix
                + "', with letters, digits, '.', '-' and '_'";
        return false;
    }
    if (bytes.empty()) {
        error = "there was nothing in it";
        return false;
    }

    const std::filesystem::path target = userDirectory() / filename;
    std::error_code ec;
    std::filesystem::create_directories(userDirectory(), ec);
    {
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot write " + target.string();
            return false;
        }
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            error = "cannot write " + target.string();
            return false;
        }
    }

    // Arrives switched off. Uploading code and agreeing to run it are two
    // decisions, and this only covers the first.
    Record &record = records_[target.stem().string()];
    record.enabled = false;
    record.disabled_reason = "user";
    save();
    rescan();
    spdlog::info("installed {} (disabled until it is switched on)", target.string());
    return true;
}

bool ExtensionStore::installFile(const std::filesystem::path &source, std::string &error)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        error = "not an extension file: " + source.string();
        return false;
    }

    const std::string filename = source.filename().string();
    if (!validFilename(filename)) {
        error = std::string("choose a file named like 'thing") + kLibrarySuffix
                + "', with letters, digits, '.', '-' and '_'";
        return false;
    }

    const std::filesystem::path target = userDirectory() / filename;
    std::filesystem::create_directories(userDirectory(), ec);
    if (ec) {
        error = "cannot create " + userDirectory().string() + ": " + ec.message();
        return false;
    }

    ec.clear();
    const bool already_managed = std::filesystem::equivalent(source, target, ec);
    if (!already_managed) {
        ec.clear();
        std::filesystem::copy_file(source, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot install " + source.string() + ": " + ec.message();
            return false;
        }
    }

    Record &record = records_[target.stem().string()];
    record.enabled = false;
    record.disabled_reason = "user";
    save();
    rescan();
    spdlog::info("installed {} from {} (disabled until it is switched on)", target.string(),
                 source.string());
    return true;
}

bool ExtensionStore::quarantine(const std::filesystem::path &path)
{
    for (const Found &entry : found_) {
        std::error_code ec;
        if (!std::filesystem::equivalent(entry.path, path, ec)) {
            continue;
        }
        Record &record = records_[entry.key];
        if (!record.enabled) {
            return false;
        }
        record.enabled = false;
        record.disabled_reason = "crash";
        record.last_crash = nowSeconds();
        save();
        spdlog::error("the engine died inside '{}'; it has been disabled", entry.key);
        return true;
    }
    return false;
}

}
