#include "ext/ExtensionLoader.hpp"

#include "ext/PluginNode.hpp"
#include "log/Log.hpp"
#include "node/NodeRegistry.hpp"
#include "node/PortTypeManifest.hpp"

#include <algorithm>
#include <set>
#include <system_error>

namespace avc::ext {
namespace {

constexpr std::uint32_t kMaxUiAssets = 64;
constexpr std::uint32_t kMaxUiAssetBytes = 2U * 1024U * 1024U;
constexpr std::uint32_t kMaxUiBytes = 4U * 1024U * 1024U;

std::string str(const char *text)
{
    return text != nullptr ? std::string(text) : std::string{};
}

std::vector<std::string> labels(const char *const *values)
{
    std::vector<std::string> out;
    for (; values != nullptr && *values != nullptr; ++values) {
        out.emplace_back(*values);
    }
    return out;
}

bool validId(const std::string &id)
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    const char first = id.front();
    if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool validAssetPath(const std::string &path)
{
    if (path.empty() || path.size() > 240 || path.front() == '/' || path.back() == '/'
        || path.find('\\') != std::string::npos || path.find("//") != std::string::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string_view part(path.data() + begin,
                                    (end == std::string::npos ? path.size() : end) - begin);
        if (part.empty() || part == "." || part == "..") return false;
        begin = end == std::string::npos ? path.size() : end + 1;
    }
    return true;
}

bool validType(const std::string &type)
{
    return validId(type);
}

node::ParamType paramTypeOf(AvcParamType type)
{
    switch (type) {
    case AVC_PARAM_ENUM: return node::ParamType::Enum;
    case AVC_PARAM_BOOL: return node::ParamType::Bool;
    case AVC_PARAM_TEXT: return node::ParamType::Text;
    case AVC_PARAM_PATH: return node::ParamType::Path;
    case AVC_PARAM_FLOAT:
    default:             return node::ParamType::Float;
    }
}

const char *settingTypeName(AvcSettingType type)
{
    switch (type) {
    case AVC_SETTING_BOOL:  return "bool";
    case AVC_SETTING_INT:   return "int";
    case AVC_SETTING_FLOAT: return "float";
    case AVC_SETTING_ENUM:  return "enum";
    case AVC_SETTING_PATH:  return "path";
    case AVC_SETTING_TEXT:
    default:                return "text";
    }
}

std::vector<node::PortDescriptor> portsOf(const AvcPortDesc *ports, std::uint32_t count)
{
    std::vector<node::PortDescriptor> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string name = str(ports[i].name);
        out.push_back({name.empty() ? "p" + std::to_string(i + 1) : std::move(name),
                       str(ports[i].type)});
    }
    return out;
}

bool portTypesExist(const AvcNodeDesc &desc, std::string &error)
{
    const node::PortTypeManifest &manifest = node::PortTypeManifest::instance();
    const auto check = [&](const AvcPortDesc *ports, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::string type = str(ports[i].type);
            if (manifest.indexOf(type) < 0) {
                error = "node type '" + str(desc.type) + "' has a port carrying '" + type
                        + "', which nothing declares";
                return false;
            }
        }
        return true;
    };
    return check(desc.inputs, desc.input_count) && check(desc.outputs, desc.output_count);
}

std::vector<node::ParamDescriptor> paramsOf(const AvcParamDesc *params, std::uint32_t count)
{
    std::vector<node::ParamDescriptor> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const AvcParamDesc &in = params[i];
        node::ParamDescriptor param;
        param.name = str(in.name);
        param.type = paramTypeOf(in.type);
        param.min = in.min;
        param.max = in.max;
        param.default_value = in.default_value;
        param.unit = str(in.unit);
        param.curve = in.curve == AVC_CURVE_LOG ? node::ParamCurve::Logarithmic
                                                : node::ParamCurve::Linear;
        param.values = labels(in.values);
        param.description = str(in.description);
        param.default_text = str(in.default_text);
        out.push_back(std::move(param));
    }
    return out;
}

}

bool ExtensionLoader::registerPortTypes(const AvcPlugin &plugin, LoadedExtension &out,
                                        std::string &error)
{
    node::PortTypeManifest &manifest = node::PortTypeManifest::instance();

    for (std::uint32_t i = 0; i < plugin.port_type_count; ++i) {
        const AvcPortTypeDesc &desc = plugin.port_types[i];
        const std::string name = str(desc.name);
        const node::PortTypeDescriptor *have = manifest.find(name);
        if (have == nullptr) {
            continue;
        }
        const bool streaming = desc.transport == AVC_PORT_STREAM;
        if (have->bytes != desc.bytes
            || (have->transport == node::PortTransport::Stream) != streaming) {
            error = "port type '" + name + "' is already declared with a different shape"
                    + (have->extension.empty() ? ", as a built-in"
                                               : ", by '" + have->extension + "'");
            return false;
        }
    }

    for (std::uint32_t i = 0; i < plugin.port_type_count; ++i) {
        const AvcPortTypeDesc &desc = plugin.port_types[i];
        node::PortTypeDescriptor type;
        type.name = str(desc.name);
        type.label = str(desc.label);
        type.transport = desc.transport == AVC_PORT_VALUE ? node::PortTransport::Value
                                                          : node::PortTransport::Stream;
        type.bytes = desc.bytes;
        type.extension = out.id;
        if (!manifest.add(std::move(type), error)) {
            return false;
        }
    }
    return true;
}

bool ExtensionLoader::registerNodes(const AvcPlugin &plugin, LoadedExtension &out,
                                    std::string &error)
{
    const node::NodeRegistry &registry = node::NodeRegistry::instance();

    std::set<std::string> seen;
    for (std::uint32_t i = 0; i < plugin.node_count; ++i) {
        const AvcNodeDesc &desc = plugin.nodes[i].desc;
        const std::string type = str(desc.type);
        if (!validType(type)) {
            error = "node type '" + type + "' is not a usable name (a-z, 0-9, '-' and '_')";
            return false;
        }
        if (!seen.insert(type).second) {
            error = "it offers two node types both called '" + type + "'";
            return false;
        }
        if (registry.find(out.id + "." + type) != nullptr) {
            error = "'" + out.id + "." + type + "' is already taken";
            return false;
        }
        if (plugin.nodes[i].vtable.create == nullptr || plugin.nodes[i].vtable.process == nullptr) {
            error = "node type '" + type + "' has no implementation behind it";
            return false;
        }
        if (!portTypesExist(desc, error)) {
            return false;
        }
    }

    for (std::uint32_t i = 0; i < plugin.node_count; ++i) {
        const AvcNodeType &entry = plugin.nodes[i];
        const AvcNodeDesc &desc = entry.desc;

        node::NodeDescriptor descriptor;
        descriptor.type = out.id + "." + str(desc.type);
        descriptor.category = str(desc.category);
        descriptor.label = str(desc.label);
        if (descriptor.category.empty()) {
            descriptor.category = out.id;
        }
        if (descriptor.label.empty()) {
            descriptor.label = str(desc.type);
        }
        descriptor.kind = node::NodeKind::Dsp;
        descriptor.inputs = portsOf(desc.inputs, desc.input_count);
        descriptor.outputs = portsOf(desc.outputs, desc.output_count);
        descriptor.params = paramsOf(desc.params, desc.param_count);
        descriptor.dynamic_inputs = desc.dynamic_inputs != 0;
        descriptor.dynamic_outputs = desc.dynamic_outputs != 0;
        descriptor.latency_frames = desc.latency_frames;
        descriptor.extension = out.id;
        descriptor.realtime_safe = desc.realtime_safe != 0;
        descriptor.recommended_cold_block = desc.recommended_cold_block;

        const std::string type = descriptor.type;
        const AvcNodeVtable *vtable = &entry.vtable;
        node::NodeRegistry::instance().add(std::move(descriptor),
                                           [vtable] { return PluginNode::create(*vtable); });
        out.node_types.push_back(type);
    }
    return true;
}

void ExtensionLoader::loadOne(const ExtensionRequest &request,
                              const std::filesystem::path &config_root, LoadedExtension &out)
{
    out.path = request.path;

    // Named after the file rather than after the extension id, because the
    // directory has to exist before avc_plugin_init is called and the id is
    // something that call returns.
    const std::filesystem::path config_dir =
        config_root / std::filesystem::path(request.path).stem();
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);

    const std::filesystem::path data_dir = config_dir / "data";
    std::filesystem::create_directories(data_dir, ec);

    auto library = std::make_unique<Library>();
    const bool opened =
        library->open(request.path, request.settings, config_dir, data_dir, ui_sink_, out.error);

    if (!opened) {
        return;
    }

    const AvcPlugin &plugin = *library->plugin();
    out.abi_version = plugin.abi_version;
    out.id = str(plugin.id);
    out.name = str(plugin.name);
    out.version = str(plugin.version);
    out.author = str(plugin.author);
    out.description = str(plugin.description);
    if (out.name.empty()) {
        out.name = out.id;
    }

    if (!validId(out.id)) {
        out.error = "its id '" + out.id
                    + "' is not a usable name (a-z, 0-9, '-' and '_', no dots)";
        return;
    }
    library->setId(out.id);
    for (const LoadedExtension &other : results_) {
        if (other.loaded && other.id == out.id) {
            out.error = "another extension is already loaded as '" + out.id + "' ("
                        + other.path + ")";
            return;
        }
    }

    for (std::uint32_t i = 0; i < plugin.setting_count; ++i) {
        const AvcSettingDesc &in = plugin.settings[i];
        SettingInfo setting;
        setting.key = str(in.key);
        setting.label = str(in.label).empty() ? setting.key : str(in.label);
        setting.description = str(in.description);
        setting.type = settingTypeName(in.type);
        setting.min = in.min;
        setting.max = in.max;
        setting.default_value = str(in.default_value);
        setting.values = labels(in.values);
        out.settings.push_back(std::move(setting));
    }

    for (std::uint32_t i = 0; i < plugin.preset_count; ++i) {
        out.presets.push_back({str(plugin.presets[i].name), str(plugin.presets[i].json)});
    }

    if (plugin.ui_asset_count > kMaxUiAssets) {
        out.error = "it embeds too many editor assets";
        return;
    }
    if (plugin.ui_asset_count != 0 && plugin.ui_assets == nullptr) {
        out.error = "it declares editor assets but provides no asset table";
        return;
    }
    std::set<std::string> asset_paths;
    std::uint64_t ui_bytes = 0;
    for (std::uint32_t i = 0; i < plugin.ui_asset_count; ++i) {
        const AvcUiAssetDesc &asset = plugin.ui_assets[i];
        const std::string path = str(asset.path);
        if (asset.struct_size < sizeof(AvcUiAssetDesc) || !validAssetPath(path)
            || !asset_paths.insert(path).second || asset.data_size > kMaxUiAssetBytes
            || (asset.data == nullptr && asset.data_size != 0)) {
            out.error = "it has an invalid editor asset '" + path + "'";
            return;
        }
        ui_bytes += asset.data_size;
        if (ui_bytes > kMaxUiBytes) {
            out.error = "its editor assets are larger than 4 MiB";
            return;
        }
        UiAssetInfo copy;
        copy.path = path;
        copy.mime_type = str(asset.mime_type);
        if (copy.mime_type.empty()) copy.mime_type = "application/octet-stream";
        if (copy.mime_type.size() > 120 || copy.mime_type.find('\r') != std::string::npos
            || copy.mime_type.find('\n') != std::string::npos) {
            out.error = "editor asset '" + path + "' has an invalid MIME type";
            return;
        }
        if (asset.data_size != 0) {
            copy.data.assign(static_cast<const char *>(asset.data), asset.data_size);
        }
        out.ui_assets.push_back(std::move(copy));
    }
    out.ui_entry = str(plugin.ui_entry);
    if (!out.ui_entry.empty()
        && (!validAssetPath(out.ui_entry) || !asset_paths.contains(out.ui_entry))) {
        out.error = "its editor entry does not name an embedded asset";
        return;
    }
    if (!out.ui_entry.empty()) {
        const auto entry = std::find_if(out.ui_assets.begin(), out.ui_assets.end(),
                                        [&](const UiAssetInfo &asset) {
                                            return asset.path == out.ui_entry;
                                        });
        if (entry == out.ui_assets.end()
            || (entry->mime_type.find("javascript") == std::string::npos
                && entry->mime_type.find("ecmascript") == std::string::npos)) {
            out.error = "its editor entry is not a JavaScript asset";
            return;
        }
    }

    // UI validation happens before either registry is touched. Like node
    // validation, a malformed editor bundle must not leave half an extension
    // behind after the load is reported as failed.
    if (!registerPortTypes(plugin, out, out.error) || !registerNodes(plugin, out, out.error)) {
        return;
    }

    // Every stored setting, once, before any node exists. Settings cannot
    // change while an extension is loaded -- changing one restarts the engine
    // -- so this is the only time an extension is told about them.
    if (plugin.configure != nullptr) {
        for (const auto &[key, value] : request.settings) {
            plugin.configure(key.c_str(), value.c_str());
        }
    }

    out.loaded = true;
    ui_plugins_[out.id] = &plugin;
    libraries_.push_back(std::move(library));
    spdlog::info("extension '{}' {} loaded from {} ({} node type(s))", out.id, out.version,
                 out.path, out.node_types.size());
}

bool ExtensionLoader::dispatchUiRequest(const std::string &extension, std::uint64_t request_id,
                                        const std::string &method, const std::string &json,
                                        std::string &error) const
{
    const auto found = ui_plugins_.find(extension);
    if (found == ui_plugins_.end() || found->second->handle_ui_request == nullptr) {
        error = "extension has no editor message handler";
        return false;
    }
    const AvcUiRequest request{sizeof(AvcUiRequest), request_id, method.c_str(), json.data(),
                               static_cast<std::uint32_t>(json.size())};
    try {
        found->second->handle_ui_request(&request);
    } catch (...) {
        error = "extension threw while handling an editor request";
        return false;
    }
    return true;
}

void ExtensionLoader::loadAll(const std::vector<ExtensionRequest> &requests,
                              const std::filesystem::path &config_root)
{
    for (const ExtensionRequest &request : requests) {
        LoadedExtension result;
        loadOne(request, config_root, result);
        if (!result.loaded) {
            spdlog::error("extension {} did not load: {}", request.path, result.error);
        }
        results_.push_back(std::move(result));
    }
}

}
