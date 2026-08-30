#pragma once

#include "ext/Library.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace avc::ext {

struct ExtensionRequest {
    std::string path;
    std::map<std::string, std::string> settings;
};

struct SettingInfo {
    std::string key;
    std::string label;
    std::string description;
    std::string type;
    double min = 0.0;
    double max = 0.0;
    std::string default_value;
    std::vector<std::string> values;
};

struct PresetInfo {
    std::string name;
    std::string json;
};

struct UiAssetInfo {
    std::string path;
    std::string mime_type;
    std::string data;
};

struct LoadedExtension {
    std::string path;
    bool loaded = false;
    std::string error;

    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::uint32_t abi_version = 0;

    std::vector<std::string> node_types;
    std::vector<SettingInfo> settings;
    std::vector<PresetInfo> presets;
    std::string ui_entry;
    std::vector<UiAssetInfo> ui_assets;
};

class ExtensionLoader {
public:
    ExtensionLoader() = default;

    ExtensionLoader(const ExtensionLoader &) = delete;
    ExtensionLoader &operator=(const ExtensionLoader &) = delete;

    void loadAll(const std::vector<ExtensionRequest> &requests,
                 const std::filesystem::path &config_root);

    const std::vector<LoadedExtension> &results() const noexcept { return results_; }

    void setUiSink(Library::UiSink sink) { ui_sink_ = std::move(sink); }

    bool dispatchUiRequest(const std::string &extension, std::uint64_t request_id,
                           const std::string &method, const std::string &json,
                           std::string &error) const;

private:
    void loadOne(const ExtensionRequest &request, const std::filesystem::path &config_root,
                 LoadedExtension &out);

    bool registerPortTypes(const AvcPlugin &plugin, LoadedExtension &out, std::string &error);

    bool registerNodes(const AvcPlugin &plugin, LoadedExtension &out, std::string &error);

    std::vector<std::unique_ptr<Library>> libraries_;
    std::vector<LoadedExtension> results_;
    std::map<std::string, const AvcPlugin *> ui_plugins_;
    Library::UiSink ui_sink_;
};

}
