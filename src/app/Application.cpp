#include "app/Application.hpp"

#include "audio/HostAudio.hpp"

#include "app/Daemon.hpp"
#include "app/EngineChild.hpp"
#include "control/ExtensionStore.hpp"
#include "log/Log.hpp"
#include "node/NodeRegistry.hpp"
#include "node/PortTypeManifest.hpp"
#include "node/basic/MeterNode.hpp"
#include "rt/RtThread.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string_view>
#include <thread>
#include <vector>

namespace avc::app {

extern std::atomic<bool> g_running;

namespace {

constexpr std::chrono::milliseconds kPollInterval{100};
constexpr int kPollsPerReport = 20;

bool matchValue(std::string_view arg, std::string_view key, std::string_view &value)
{
    if (arg.rfind(key, 0) != 0 || arg.size() <= key.size() || arg[key.size()] != '=') {
        return false;
    }
    value = arg.substr(key.size() + 1);
    return true;
}

std::uint32_t toU32(std::string_view text)
{
    return static_cast<std::uint32_t>(std::strtoul(std::string(text).c_str(), nullptr, 10));
}

bool parseVirtualDevice(std::string_view text, bool source, VirtualDeviceOption &out)
{
    const std::size_t colon = text.rfind(':');
    out.source = source;
    out.channels = source ? 1 : 2;
    out.name = text;

    if (colon != std::string_view::npos && colon + 1 < text.size()) {
        const std::uint32_t channels = toU32(text.substr(colon + 1));
        if (channels >= 1 && channels <= 32) {
            out.name = text.substr(0, colon);
            out.channels = channels;
        }
    }

    return !out.name.empty();
}

const char *paramTypeName(node::ParamType type)
{
    switch (type) {
    case node::ParamType::Float:  return "float";
    case node::ParamType::Enum:   return "enum";
    case node::ParamType::Bool:   return "bool";
    case node::ParamType::Device: return "device";
    case node::ParamType::Text:   return "text";
    case node::ParamType::Path:   return "path";
    }
    return "?";
}

bool isTextual(node::ParamType type)
{
    return type == node::ParamType::Device || type == node::ParamType::Text
           || type == node::ParamType::Path;
}

}

std::string pickFirstDevice(const std::vector<audio::DeviceInfo> &devices,
                            std::string_view media_class, audio::Direction needed)
{
    for (const audio::DeviceInfo &device : devices) {
        const std::uint32_t ports =
            needed == audio::Direction::Output ? device.output_ports : device.input_ports;
        if (device.media_class == media_class && ports > 0) {
            return device.name;
        }
    }
    return {};
}

graph::GraphSpec defaultSpec(const Options &options, const std::string &input_target,
                             const std::string &output_target)
{
    graph::GraphSpec spec;
    spec.version = 1;

    std::vector<std::pair<std::string, std::uint32_t>> sources;
    spec.nodes.push_back({.id = "capture",
                          .type = "capture",
                          .options = {{"source", input_target}},
                          .outputs = options.in_channels});
    sources.emplace_back("capture", options.in_channels);

    std::vector<std::pair<std::string, std::uint32_t>> sinks;
    spec.nodes.push_back({.id = "playback",
                          .type = "playback",
                          .options = {{"device", output_target}},
                          .inputs = options.out_channels,
                          .ui_x = 900.0F});
    sinks.emplace_back("playback", options.out_channels);

    float y = 0.0F;
    for (const VirtualDeviceOption &device : options.virtual_devices) {
        const std::string id = device.name;
        if (device.source) {
            spec.nodes.push_back({.id = id,
                                  .type = "virtual_mic",
                                  .options = {{"publish_as", device.name}},
                                  .inputs = device.channels,
                                  .ui_x = 900.0F,
                                  .ui_y = y += 140.0F});
            sinks.emplace_back(id, device.channels);
        } else {
            spec.nodes.push_back({.id = id,
                                  .type = "virtual_speaker",
                                  .options = {{"publish_as", device.name}},
                                  .outputs = device.channels,
                                  .ui_y = y += 140.0F});
            sources.emplace_back(id, device.channels);
        }
    }

    std::uint32_t channels = 0;
    for (const auto &[id, count] : sources) {
        channels += count;
    }

    const bool needs_mixer = channels > 1;
    if (needs_mixer) {
        spec.nodes.push_back(
            {.id = "mix", .type = "mixer", .inputs = channels, .ui_x = 260.0F});
    }

    std::uint32_t next = 1;
    for (const auto &[id, count] : sources) {
        for (std::uint32_t c = 0; c < count; ++c) {
            const std::string port = "out_" + std::to_string(c + 1);
            if (needs_mixer) {
                spec.edges.push_back({{id, port}, {"mix", "in_" + std::to_string(next++)}});
            } else {
                spec.edges.push_back({{id, port}, {"gain", std::string("in")}});
            }
        }
    }

    spec.nodes.push_back({.id = "gain",
                          .type = "gain",
                          .params = {{"gain_db", options.gain_db}},
                          .ui_x = 440.0F});
    spec.nodes.push_back({.id = "meter", .type = "meter", .ui_x = 620.0F});
    if (needs_mixer) {
        spec.edges.push_back({{"mix", std::string("out")}, {"gain", std::string("in")}});
    }
    spec.edges.push_back({{"gain", std::string("out")}, {"meter", std::string("in")}});

    for (const auto &[id, count] : sinks) {
        for (std::uint32_t c = 0; c < count; ++c) {
            spec.edges.push_back(
                {{"meter", std::string("out")}, {id, "in_" + std::to_string(c + 1)}});
        }
    }
    return spec;
}

bool parseOptions(int argc, char **argv, Options &out)
{
    for (int i = 0; i < argc; ++i) {
        out.argv.emplace_back(argv[i]);
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string_view value;

        if (arg == "--help" || arg == "-h") {
            out.help = true;
        } else if (arg == "--list") {
            out.list_devices = true;
        } else if (arg == "--list-nodes") {
            out.list_nodes = true;
        } else if (arg == "--force-quantum") {
            out.force_quantum = true;
        } else if (matchValue(arg, "--input", value)) {
            out.input_target = value;
        } else if (matchValue(arg, "--output", value)) {
            out.output_target = value;
        } else if (matchValue(arg, "--graph", value)) {
            out.graph_path = value;
        } else if (matchValue(arg, "--check", value)) {
            out.check_path = value;
        } else if (matchValue(arg, "--virtual-mic", value)) {
            VirtualDeviceOption device;
            if (!parseVirtualDevice(value, true, device)) {
                spdlog::error("--virtual-mic needs a name");
                return false;
            }
            out.virtual_devices.push_back(std::move(device));
        } else if (matchValue(arg, "--virtual-sink", value)) {
            VirtualDeviceOption device;
            if (!parseVirtualDevice(value, false, device)) {
                spdlog::error("--virtual-sink needs a name");
                return false;
            }
            out.virtual_devices.push_back(std::move(device));
        } else if (matchValue(arg, "--log-level", value)) {
            out.log_level = value;
        } else if (matchValue(arg, "--port", value)) {
            out.http_port = static_cast<int>(toU32(value));
            out.no_http = false;
        } else if (matchValue(arg, "--bind", value)) {
            out.http_bind = value;
            out.no_http = false;
        } else if (matchValue(arg, "--ui-dir", value)) {
            out.ui_dir = value;
        } else if (arg == "--http") {
            out.no_http = false;
        } else if (arg == "--no-http") {
            out.no_http = true;
#ifdef _WIN32
        } else if (arg == "--desktop") {
            out.desktop = true;
        } else if (arg == "--no-desktop") {
            out.desktop = false;
#endif
        } else if (arg == "--no-restore") {
            out.no_restore = true;
        } else if (arg == "--list-extensions") {
            out.list_extensions = true;
        } else if (arg == "--no-extensions") {
            out.no_extensions = true;
        } else if (arg == "--no-extension-upload") {
            out.no_extension_upload = true;
        } else if (matchValue(arg, "--extension-dir", value)) {
            out.extension_dirs.emplace_back(value);
        } else if (matchValue(arg, "--extension-manifest", value)) {
            out.extension_manifest = value;
        } else if (matchValue(arg, "--engine-fd", value)) {
            const auto handle = static_cast<std::intptr_t>(std::strtoull(
                std::string(value).c_str(), nullptr, 10));
            out.engine_read_handle = handle;
            out.engine_write_handle = handle;
        } else if (matchValue(arg, "--engine-read-handle", value)) {
            out.engine_read_handle = static_cast<std::intptr_t>(
                std::strtoull(std::string(value).c_str(), nullptr, 10));
        } else if (matchValue(arg, "--engine-write-handle", value)) {
            out.engine_write_handle = static_cast<std::intptr_t>(
                std::strtoull(std::string(value).c_str(), nullptr, 10));
#ifdef _WIN32
        } else if (matchValue(arg, "--usbip-operation", value)) {
            out.usbip_operation = value;
        } else if (matchValue(arg, "--usbip-buses", value)) {
            out.usbip_buses = value;
#endif
        } else if (matchValue(arg, "--in-channels", value)) {
            out.in_channels = toU32(value);
        } else if (matchValue(arg, "--out-channels", value)) {
            out.out_channels = toU32(value);
        } else if (matchValue(arg, "--gain", value)) {
            out.gain_db = std::strtof(std::string(value).c_str(), nullptr);
        } else if (matchValue(arg, "--rate", value)) {
            out.format.sample_rate = toU32(value);
        } else if (matchValue(arg, "--quantum", value)) {
            out.format.quantum = toU32(value);
        } else {
            spdlog::error("unknown argument: {}", arg);
            return false;
        }
    }

    const bool sane = out.format.sample_rate >= 8000 && out.format.quantum >= 16
                      && out.format.quantum <= types::kMaxQuantum && out.in_channels >= 1
                      && out.out_channels >= 1;
    if (!sane) {
        spdlog::error("rate/quantum/channel counts out of range");
        return false;
    }
    return true;
}

void printUsage()
{
    std::puts(
        "avc -- realtime voice engine\n"
        "\n"
        "  --list                 list audio nodes and exit\n"
        "  --list-nodes           list DSP node types and their parameters, then exit\n"
        "  --graph=FILE           graph JSON; reloaded whenever the file changes\n"
        "  --check=FILE           compile a graph, print what it would run, exit\n"
        "  --virtual-mic=NAME[:CH]   publish a microphone other apps can select (default 1 ch)\n"
        "  --virtual-sink=NAME[:CH]  publish a speaker and capture what apps play into it\n"
        "  --input=NODE           capture node.name (default: first Audio/Source)\n"
        "  --output=NODE          playback node.name (default: first Audio/Sink)\n"
        "  --in-channels=N        engine input ports (default 1)\n"
        "  --out-channels=N       engine output ports (default 2)\n"
        "  --gain=DB              gain for the default passthrough graph\n"
        "  --rate=HZ              sample rate (default 48000)\n"
        "  --quantum=FRAMES       block size (default 128 = 2.67 ms)\n"
        "  --force-quantum        force the graph quantum instead of requesting it\n"
#ifdef _WIN32
        "  --desktop              open the native WebView2 editor (Windows default)\n"
        "  --no-desktop           do not open the native editor\n"
#endif
        "  --http                 also enable the optional HTTP control plane\n"
        "  --port=N               HTTP control plane port (implies --http; default 7420)\n"
        "  --bind=ADDR            HTTP bind address (implies --http; default 127.0.0.1)\n"
        "  --ui-dir=DIR           load editor files from here instead of the built-in copy\n"
        "  --no-http              disable the optional HTTP control plane\n"
        "  --no-restore           start from the built-in graph, not the last one that ran\n"
        "  --list-extensions      list the extensions found, and what they offer, then exit\n"
        "  --extension-dir=DIR    look here for extensions first; may be given more than once\n"
        "  --no-extensions        load none of them\n"
        "  --no-extension-upload  refuse to accept a shared object over HTTP\n"
        "  --log-level=LEVEL      trace|debug|info|warn|error (default info)\n");
}

void Application::loadExtensions(ext::ExtensionLoader &loader) const
{
    if (options_.no_extensions) {
        node::NodeRegistry::instance().seal();
        return;
    }

    control::ExtensionStore store(options_.extension_dirs);
    const nlohmann::json manifest = store.engineManifest();

    std::vector<ext::ExtensionRequest> requests;
    const nlohmann::json listed = manifest.value("extensions", nlohmann::json::array());
    for (const nlohmann::json &entry : listed) {
        ext::ExtensionRequest request;
        request.path = entry.value("path", std::string{});
        const nlohmann::json settings = entry.value("settings", nlohmann::json::object());
        for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
            request.settings[it.key()] = it->is_string() ? it->get<std::string>() : it->dump();
        }
        if (!request.path.empty()) {
            requests.push_back(std::move(request));
        }
    }

    loader.loadAll(requests, manifest.value("configRoot", std::string{}));
    node::NodeRegistry::instance().seal();
}

int Application::listExtensions() const
{
    control::ExtensionStore store(options_.extension_dirs);

    std::printf("looking in:\n");
    for (const std::filesystem::path &dir : store.directories()) {
        std::printf("  %s\n", dir.string().c_str());
    }
    if (store.found().empty()) {
        std::printf("nothing there.\n");
        return 0;
    }

    ext::ExtensionLoader loader;
    loadExtensions(loader);

    std::map<std::string, const ext::LoadedExtension *> by_path;
    for (const ext::LoadedExtension &loaded : loader.results()) {
        by_path[loaded.path] = &loaded;
    }

    std::printf("\n");
    for (const control::ExtensionStore::Found &entry : store.found()) {
        const auto loaded = by_path.find(entry.path.string());
        if (loaded == by_path.end()) {
            std::printf("%-20s disabled  %s\n", entry.key.c_str(), entry.path.string().c_str());
            continue;
        }
        const ext::LoadedExtension &result = *loaded->second;
        if (!result.loaded) {
            std::printf("%-20s FAILED    %s\n%*s%s\n", entry.key.c_str(),
                        entry.path.string().c_str(),
                        22, "", result.error.c_str());
            continue;
        }
        std::printf("%-20s %-9s %s by %s\n", entry.key.c_str(), result.version.c_str(),
                    result.name.c_str(), result.author.empty() ? "-" : result.author.c_str());
        for (const std::string &type : result.node_types) {
            std::printf("%*snode  %s\n", 22, "", type.c_str());
        }
        for (const ext::SettingInfo &setting : result.settings) {
            std::printf("%*sset   %s (%s, default %s)\n", 22, "", setting.key.c_str(),
                        setting.type.c_str(), setting.default_value.c_str());
        }
        for (const ext::PresetInfo &preset : result.presets) {
            std::printf("%*sgraph %s\n", 22, "", preset.name.c_str());
        }
    }
    return 0;
}

int Application::listNodeTypes()
{
    for (const node::NodeDescriptor *desc : node::NodeRegistry::instance().all()) {
        std::printf("%-10s %-8s in:%zu out:%zu%s\n", desc->type.c_str(), desc->category.c_str(),
                    desc->inputs.size(), desc->outputs.size(),
                    desc->dynamic_inputs    ? "  (input count set by the spec)"
                    : desc->dynamic_outputs ? "  (output count set by the spec)"
                                            : "");
        for (const auto &[side, ports] :
             {std::pair{"in", &desc->inputs}, std::pair{"out", &desc->outputs}}) {
            for (const node::PortDescriptor &port : *ports) {
                if (!port.type.empty() && port.type != node::kAudioPortType) {
                    std::printf("           %s %-10s carries %s\n", side, port.name.c_str(),
                                port.type.c_str());
                }
            }
        }
        for (const node::ParamDescriptor &param : desc->params) {
            if (isTextual(param.type)) {
                std::printf("           %-12s %-6s %s\n", param.name.c_str(),
                            paramTypeName(param.type),
                            param.device_role.empty()
                                ? "a name other applications will see"
                                : ("something to " + param.device_role).c_str());
                continue;
            }
            std::printf("           %-12s %-6s [%g .. %g] default %g %s\n", param.name.c_str(),
                        paramTypeName(param.type), param.min, param.max, param.default_value,
                        param.unit.c_str());
        }
    }
    return 0;
}

int Application::checkGraph() const
{
    std::string error;
    const auto spec = graph::GraphSpec::load(options_.check_path, error);
    if (!spec) {
        spdlog::error("{}", error);
        return 1;
    }

    std::vector<audio::IoRequest> requests;
    if (!graph::GraphCompiler::ioRequests(*spec, requests, error)) {
        spdlog::error("{}", error);
        return 1;
    }

    graph::CompileEnv env;
    env.sample_rate = options_.format.sample_rate;
    env.max_quantum = types::kMaxQuantum;

    // No engine here, so hand out slots in order. It checks everything except
    // whether the devices named actually exist right now.
    std::uint32_t in_slot = 0;
    std::uint32_t out_slot = 0;
    for (const audio::IoRequest &request : requests) {
        std::uint32_t &next = audio::producesSignal(request.kind) ? in_slot : out_slot;
        for (std::uint32_t c = 0; c < request.channels; ++c) {
            env.io_slots[request.node].push_back(next++);
        }
    }
    const auto graph = graph::GraphCompiler::compile(*spec, env, nullptr, error);
    if (graph == nullptr) {
        spdlog::error("{}", error);
        return 1;
    }
    spdlog::info("ok: {} dsp nodes, {} stages, {} buffers ({} KiB), {} engine port group(s)",
                 graph->nodeCount(), graph->stageCount(), graph->bufferSlots(),
                 graph->storageBytes() / 1024, requests.size());
    for (const audio::IoRequest &request : requests) {
        spdlog::info("  {} {} x{}", request.node, request.target, request.channels);
    }

    const double rate = static_cast<double>(env.sample_rate);
    spdlog::info("graph latency: {:.1f} ms ({} samples)", 1000.0 * graph->latencyFrames() / rate,
                 graph->latencyFrames());
    const std::vector<graph::OutputLatency> outputs = graph->latencyByOutput();
    if (outputs.size() > 1) {
        for (const graph::OutputLatency &output : outputs) {
            spdlog::info("  {} waits {:.1f} ms", output.node, 1000.0 * output.frames / rate);
        }
    }
    for (const graph::DomainInfo &domain : graph->domains()) {
        if (!domain.cold) {
            spdlog::info("  domain {} (hot): {} node(s)", domain.name, domain.stages);
            continue;
        }
        spdlog::info("  domain {} (cold): {} node(s), block {} ({:.1f} ms), safety {}, adds {:.1f} ms",
                     domain.name, domain.stages, domain.block, 1000.0 * domain.block / rate,
                     domain.safety, 1000.0 * domain.latency_frames / rate);
    }
    return 0;
}

int Application::listDevices()
{
    audio::HostSession session;
    if (!session.open("avc-list")) {
        return 1;
    }
    for (const audio::DeviceInfo &device : session.enumerateDevices()) {
        std::printf("%6u  %-28s in:%-3u out:%-3u  %s\n", device.id, device.media_class.c_str(),
                    device.input_ports, device.output_ports,
                    device.name.empty() ? device.description.c_str() : device.name.c_str());
    }
    session.close();
    return 0;
}

int Application::run()
{
    if (options_.list_extensions) {
        return listExtensions();
    }
    if (options_.list_nodes || !options_.check_path.empty()) {
        ext::ExtensionLoader loader;
        loadExtensions(loader);
        return options_.list_nodes ? listNodeTypes() : checkGraph();
    }
    if (options_.list_devices) {
        return listDevices();
    }
    if (options_.engine_read_handle >= 0 && options_.engine_write_handle >= 0) {
        return EngineChild(options_, options_.engine_read_handle,
                           options_.engine_write_handle)
            .run();
    }
    return Daemon(options_).run();
}

}
