#pragma once

#include <avc/avc_plugin.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace avc::ext {

class Library {
public:
    using UiSink = std::function<void(const nlohmann::json &)>;
    Library();
    ~Library();

    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;

    bool open(const std::filesystem::path &path,
              const std::map<std::string, std::string> &settings,
              const std::filesystem::path &config_dir,
              const std::filesystem::path &data_dir, UiSink ui_sink,
              std::string &error);

    void setId(std::string id);

    const AvcPlugin *plugin() const noexcept { return plugin_; }
    const std::filesystem::path &path() const noexcept { return path_; }

    /* Public only so the C callback thunks in Library.cpp can name it. It is
     * still an implementation detail and no instance escapes Library. */
    struct HostState;

private:
    std::filesystem::path path_;
    std::unique_ptr<HostState> host_state_;
    std::string config_dir_;
    std::string data_dir_;
    AvcHostApi host_{};
    void *handle_ = nullptr;
    const AvcPlugin *plugin_ = nullptr;
};

}
