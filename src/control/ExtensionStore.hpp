#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace avc::control {

class ExtensionStore {
public:
    struct Record {
        bool enabled = true;
        std::string disabled_reason;
        std::int64_t last_crash = 0;
        std::map<std::string, std::string> settings;
    };

    struct Found {
        std::string key;
        std::filesystem::path path;
    };

    static std::filesystem::path defaultConfigPath();

    static std::filesystem::path userDirectory();

    static std::filesystem::path configRoot();

    explicit ExtensionStore(std::vector<std::string> extra);

    void rescan();

    const std::vector<Found> &found() const noexcept { return found_; }
    const std::vector<std::filesystem::path> &directories() const noexcept { return dirs_; }

    nlohmann::json engineManifest() const;

    bool writeManifest(const std::filesystem::path &path) const;

    nlohmann::json describe(const nlohmann::json &reported) const;

    bool setEnabled(const std::string &key, bool enabled, std::string &error);
    bool setSettings(const std::string &key, const nlohmann::json &values, std::string &error);

    bool remove(const std::string &key, std::string &error);

    bool install(const std::string &filename, const std::string &bytes, std::string &error);
    bool installFile(const std::filesystem::path &source, std::string &error);

    bool quarantine(const std::filesystem::path &path);

    static bool validFilename(const std::string &name);

private:
    void load();
    void save() const;
    const Record *recordFor(const std::string &key) const;
    const Found *findFile(const std::string &key) const;

    std::vector<std::string> extra_;
    std::vector<std::filesystem::path> dirs_;
    std::vector<Found> found_;
    std::map<std::string, Record> records_;
    std::filesystem::path config_path_;
};

}
