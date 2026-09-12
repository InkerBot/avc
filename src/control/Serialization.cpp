#include "control/Serialization.hpp"

#include "node/NodeRegistry.hpp"
#include "node/PortTypeManifest.hpp"
#include "rt/RtThread.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <set>

namespace avc::control {
namespace {

using nlohmann::json;

std::string base64(std::string_view input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const std::uint32_t a = static_cast<unsigned char>(input[i]);
        const std::uint32_t b = i + 1 < input.size()
                                  ? static_cast<unsigned char>(input[i + 1]) : 0;
        const std::uint32_t c = i + 2 < input.size()
                                  ? static_cast<unsigned char>(input[i + 2]) : 0;
        const std::uint32_t bits = (a << 16U) | (b << 8U) | c;
        out.push_back(alphabet[(bits >> 18U) & 63U]);
        out.push_back(alphabet[(bits >> 12U) & 63U]);
        out.push_back(i + 1 < input.size() ? alphabet[(bits >> 6U) & 63U] : '=');
        out.push_back(i + 2 < input.size() ? alphabet[bits & 63U] : '=');
    }
    return out;
}

std::string assetDigest(const json &assets)
{
    std::uint64_t hash = 1469598103934665603ULL;
    const auto add = [&hash](std::string_view value) {
        for (const unsigned char byte : value) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
    };
    for (const json &asset : assets) {
        add(asset.value("path", std::string{}));
        add(asset.value("data", std::string{}));
    }
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

const char *paramTypeName(node::ParamType type)
{
    switch (type) {
    case node::ParamType::Float: return "float";
    case node::ParamType::Enum:  return "enum";
    case node::ParamType::Bool:   return "bool";
    case node::ParamType::Device: return "device";
    case node::ParamType::Text:   return "text";
    case node::ParamType::Path:   return "path";
    }
    return "float";
}

const char *nodeKindName(node::NodeKind kind)
{
    switch (kind) {
    case node::NodeKind::Capture:        return "capture";
    case node::NodeKind::Playback:       return "playback";
    case node::NodeKind::VirtualSpeaker: return "virtual_speaker";
    case node::NodeKind::VirtualMic:     return "virtual_mic";
    case node::NodeKind::Dsp:            return "dsp";
    }
    return "dsp";
}

const char *nodeStateName(node::NodeState state)
{
    switch (state) {
    case node::NodeState::Offline:  return "offline";
    case node::NodeState::Loading:  return "loading";
    case node::NodeState::Ready:    return "ready";
    case node::NodeState::Degraded: return "degraded";
    case node::NodeState::Error:    return "error";
    }
    return "offline";
}

json portsJson(const std::vector<node::PortDescriptor> &ports)
{
    json out = json::array();
    for (const node::PortDescriptor &port : ports) {
        out.push_back({{"name", port.name},
                       {"type", port.type.empty() ? node::kAudioPortType : port.type.c_str()}});
    }
    return out;
}

json paramsJson(const std::vector<node::ParamDescriptor> &params)
{
    json out = json::array();
    for (const node::ParamDescriptor &param : params) {
        json entry{
            {"name", param.name},
            {"type", paramTypeName(param.type)},
            {"min", param.min},
            {"max", param.max},
            {"default", param.type == node::ParamType::Text || param.type == node::ParamType::Path
                            ? json(param.default_text)
                            : json(param.default_value)},
            {"unit", param.unit},
            {"curve", param.curve == node::ParamCurve::Logarithmic ? "log" : "linear"},
        };
        if (!param.values.empty()) {
            entry["values"] = param.values;
        }
        if (!param.device_role.empty()) {
            entry["deviceRole"] = param.device_role;
        }
        if (!param.description.empty()) {
            entry["description"] = param.description;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

float toDb(float linear)
{
    return linear > 1e-6F ? 20.0F * std::log10(linear) : -120.0F;
}

}

nlohmann::json descriptorsJson()
{
    json out = json::array();
    for (const node::NodeDescriptor *desc : node::NodeRegistry::instance().all()) {
        out.push_back({
            {"type", desc->type},
            {"category", desc->category},
            {"label", desc->label},
            {"kind", nodeKindName(desc->kind)},
            {"inputs", portsJson(desc->inputs)},
            {"outputs", portsJson(desc->outputs)},
            {"params", paramsJson(desc->params)},
            {"dynamicInputs", desc->dynamic_inputs},
            {"dynamicOutputs", desc->dynamic_outputs},
            {"latencyFrames", desc->latency_frames},
            {"extension", desc->extension},
            {"realtimeSafe", desc->realtime_safe},
            {"recommendedColdBlock", desc->recommended_cold_block},
        });
    }
    return out;
}

nlohmann::json devicesJson(const std::vector<audio::DeviceInfo> &devices)
{
    json out = json::array();
    for (const audio::DeviceInfo &device : devices) {
        out.push_back({
            {"id", device.id},
            {"name", device.name},
            {"description", device.description},
            {"mediaClass", device.media_class},
            {"application", device.application},
            {"mediaName", device.media_name},
            {"inputPorts", device.input_ports},
            {"outputPorts", device.output_ports},
        });
    }
    return out;
}

nlohmann::json extensionsJson(const std::vector<ext::LoadedExtension> &extensions)
{
    json out = json::array();
    for (const ext::LoadedExtension &extension : extensions) {
        json settings = json::array();
        for (const ext::SettingInfo &setting : extension.settings) {
            settings.push_back({
                {"key", setting.key},
                {"label", setting.label},
                {"description", setting.description},
                {"type", setting.type},
                {"min", setting.min},
                {"max", setting.max},
                {"default", setting.default_value},
                {"values", setting.values},
            });
        }

        json presets = json::array();
        for (const ext::PresetInfo &preset : extension.presets) {
            presets.push_back({{"name", preset.name}, {"json", preset.json}});
        }

        json ui_assets = json::array();
        for (const ext::UiAssetInfo &asset : extension.ui_assets) {
            ui_assets.push_back({{"path", asset.path},
                                 {"mime", asset.mime_type},
                                 {"data", base64(asset.data)}});
        }

        out.push_back({
            {"path", extension.path},
            {"loaded", extension.loaded},
            {"error", extension.error},
            {"id", extension.id},
            {"name", extension.name},
            {"version", extension.version},
            {"author", extension.author},
            {"description", extension.description},
            {"abiVersion", extension.abi_version},
            {"nodeTypes", extension.node_types},
            {"settings", std::move(settings)},
            {"presets", std::move(presets)},
            {"ui", {{"entry", extension.ui_entry},
                    {"digest", assetDigest(ui_assets)},
                    {"messages", !extension.ui_entry.empty()},
                    {"assets", std::move(ui_assets)}}},
        });
    }
    return out;
}

nlohmann::json specJson(const graph::GraphSpec &spec)
{
    return json::parse(spec.dump(0), nullptr, false);
}

nlohmann::json telemetryJson(const audio::BackendStats &backend, const graph::GraphStats &graph,
                             const std::vector<graph::MeterReading> &meters,
                             const std::vector<graph::ScopeReading> &scopes,
                             const std::vector<graph::TextReading> &texts)
{
    const std::uint32_t quantum =
        backend.actual_quantum > 0 ? backend.actual_quantum : backend.quantum;
    const std::uint32_t rate = backend.actual_rate > 0 ? backend.actual_rate : backend.sample_rate;
    const double budget_ns = rate > 0 ? 1e9 * static_cast<double>(quantum) / rate : 0.0;

    json meter_json = json::object();
    for (const graph::MeterReading &meter : meters) {
        meter_json[meter.node] = {{"peak", toDb(meter.peak)}, {"rms", toDb(meter.rms)}};
    }

    json scope_json = json::object();
    for (const graph::ScopeReading &scope : scopes) {
        scope_json[scope.node] = {{"wave", scope.wave}, {"bands", scope.bands}};
    }

    json text_json = json::object();
    for (const graph::TextReading &text : texts) {
        text_json[text.node] = {
            {"text", text.text},
            {"stream", text.segmented ? std::to_string(text.stream) : std::string{}},
            {"segment", text.segment},
            {"revision", text.revision},
            {"final", text.final},
        };
    }

    json cost_json = json::object();
    json node_latency_json = json::object();
    for (const graph::NodeCost &cost : graph.node_costs) {
        if (cost.ns > 0) {
            cost_json[cost.node] = static_cast<double>(cost.ns) / 1000.0;
        }
        if (cost.latency_frames > 0 && rate > 0) {
            node_latency_json[cost.node] =
                1000.0 * static_cast<double>(cost.latency_frames) / rate;
        }
    }

    json output_latency_json = json::object();
    for (const graph::OutputLatency &output : graph.output_latency) {
        output_latency_json[output.node] =
            rate > 0 ? 1000.0 * static_cast<double>(output.frames) / rate : 0.0;
    }

    json node_status_json = json::object();
    for (const graph::NodeStatusInfo &entry : graph.node_status) {
        node_status_json[entry.node] = {
            {"state", nodeStateName(entry.status.state)},
            {"progress", entry.status.progress},
            {"processedBlocks", entry.status.processed_blocks},
            {"bypassedBlocks", entry.status.bypassed_blocks},
            {"failures", entry.status.failures},
            {"message", entry.status.message},
        };
    }

    json domain_json = json::array();
    for (const graph::DomainInfo &domain : graph.domains) {
        const std::uint32_t block = domain.cold ? domain.block : quantum;
        const double period_ns = rate > 0 ? 1e9 * static_cast<double>(block) / rate : 0.0;
        domain_json.push_back({
            {"name", domain.name},
            {"cold", domain.cold},
            {"block", block},
            {"blockMs", rate > 0 ? 1000.0 * static_cast<double>(block) / rate : 0.0},
            {"safety", domain.safety},
            {"latencyFrames", domain.latency_frames},
            {"latencyMs",
             rate > 0 ? 1000.0 * static_cast<double>(domain.latency_frames) / rate : 0.0},
            {"stages", domain.stages},
            {"passes", domain.passes},
            {"underruns", domain.underruns},
            {"overruns", domain.overruns},
            {"dspUsAvg", static_cast<double>(domain.ns_avg) / 1000.0},
            {"dspUs", static_cast<double>(domain.ns_max) / 1000.0},
            {"loadAvg", period_ns > 0 ? 100.0 * static_cast<double>(domain.ns_avg) / period_ns : 0.0},
            {"load", period_ns > 0 ? 100.0 * static_cast<double>(domain.ns_max) / period_ns : 0.0},
        });
    }

    const double block_ms = budget_ns / 1e6;
    const double graph_ms =
        rate > 0 ? 1000.0 * static_cast<double>(graph.latency_frames) / rate : 0.0;
    const double fallback_io_ms = block_ms * 2.0;
    const double capture_ms = backend.io_latency_known && rate > 0
                                  ? 1000.0 * backend.input_latency_frames / rate
                                  : block_ms;
    const double playback_ms = backend.io_latency_known && rate > 0
                                   ? 1000.0 * backend.output_latency_frames / rate
                                   : block_ms;
    const double io_ms = backend.io_latency_known ? capture_ms + playback_ms : fallback_io_ms;

    return json{
        {"cycles", backend.cycles},
        {"xruns", backend.xruns},
        {"quantum", quantum},
        {"sampleRate", rate},
        {"blockMs", block_ms},
        {"graphLatencyFrames", graph.latency_frames},
        {"graphLatencyMs", graph_ms},
        {"ioLatencyMs", io_ms},
        {"captureLatencyMs", capture_ms},
        {"playbackLatencyMs", playback_ms},
        {"totalLatencyMs", io_ms + graph_ms},
        {"clockSource", backend.device_driven ? "device" : "timer"},
        {"jitterUs", static_cast<double>(backend.wakeup_jitter_ns_max) / 1000.0},
        {"dspUs", static_cast<double>(backend.process_ns_max) / 1000.0},
        {"dspUsLast", static_cast<double>(backend.process_ns_last) / 1000.0},
        {"dspUsAvg", static_cast<double>(backend.process_ns_avg) / 1000.0},
        {"load", budget_ns > 0 ? 100.0 * backend.process_ns_max / budget_ns : 0.0},
        {"loadAvg", budget_ns > 0 ? 100.0 * backend.process_ns_avg / budget_ns : 0.0},
        {"profiling", graph.profiling},
        {"nodeCost", std::move(cost_json)},
        {"nodeLatencyMs", std::move(node_latency_json)},
        {"nodeStatus", std::move(node_status_json)},
        // Masked, because PipeWire sets SCHED_RESET_ON_FORK alongside the policy
        // and the raw value is not comparable on its own.
        {"realtime", rt::isRealtime({backend.sched_policy, backend.sched_priority,
                                     backend.audio_tid})},
        {"generation", graph.generation},
        {"swaps", graph.swaps},
        {"nodes", graph.nodes},
        {"bufferSlots", graph.buffer_slots},
        {"domains", std::move(domain_json)},
        {"outputLatencyMs", std::move(output_latency_json)},
        {"droppedParams", graph.dropped_params},
        {"meters", std::move(meter_json)},
        {"scopes", std::move(scope_json)},
        {"texts", std::move(text_json)},
        {"scopeBandsHz", graph::GraphHost::scopeBandsHz()},
    };
}

}
