#pragma once

#include "audio/AudioBackend.hpp"
#include "ext/ExtensionLoader.hpp"
#include "graph/GraphSpec.hpp"
#include "types/Audio.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace avc::app {

struct VirtualDeviceOption {
    std::string name;
    std::uint32_t channels = 1;
    bool source = true;
};

struct Options {
    std::vector<std::string> argv;

    std::string input_target;
    std::string output_target;
    std::string graph_path;
    std::string check_path;
    std::string log_level = "info";
    std::string http_bind = "127.0.0.1";
    std::string ui_dir;
    int http_port = 7420;
#ifdef _WIN32
    bool no_http = true;
    bool desktop = true;
#else
    bool no_http = false;
#endif

    std::intptr_t engine_read_handle = -1;
    std::intptr_t engine_write_handle = -1;
    std::string extension_manifest;

    std::vector<std::string> extension_dirs;
    bool no_extensions = false;
    bool no_extension_upload = false;
    bool list_extensions = false;

    bool no_restore = false;
    std::uint32_t in_channels = 1;
    std::uint32_t out_channels = 2;
    float gain_db = 0.0F;
    types::AudioFormat format{};
    bool force_quantum = false;
    std::vector<VirtualDeviceOption> virtual_devices;
    bool list_devices = false;
    bool list_nodes = false;
    bool help = false;
#ifdef _WIN32
    std::string usbip_operation;
    std::string usbip_buses;
#endif
};

bool parseOptions(int argc, char **argv, Options &out);
void printUsage();

std::string pickFirstDevice(const std::vector<audio::DeviceInfo> &devices,
                            std::string_view media_class, audio::Direction needed);

graph::GraphSpec defaultSpec(const Options &options, const std::string &input_target,
                             const std::string &output_target);

class Application {
public:
    explicit Application(Options options) : options_(std::move(options)) {}

    int run();

private:
    static int listDevices();
    static int listNodeTypes();
    int listExtensions() const;
    int checkGraph() const;

    void loadExtensions(ext::ExtensionLoader &loader) const;

    Options options_;
};

}
