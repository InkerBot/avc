

#ifndef AVC_PLUGIN_HPP
#define AVC_PLUGIN_HPP

#include "avc_plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace avc::sdk {

inline constexpr const char *kAudioPortType = "audio";

inline constexpr const char *kTextPortType = "text";
inline constexpr std::uint32_t kTextPortTypeBytes = 4096;

inline std::string_view readText(const void *block) noexcept
{
    if (block == nullptr) {
        return {};
    }
    std::uint32_t length = 0;
    std::memcpy(&length, block, sizeof(length));
    if (length > kTextPortTypeBytes - sizeof(length)) {
        return {};
    }
    return {static_cast<const char *>(block) + sizeof(length), length};
}

inline void writeText(void *block, std::string_view text) noexcept
{
    if (block == nullptr) {
        return;
    }
    const auto room = static_cast<std::uint32_t>(kTextPortTypeBytes - sizeof(std::uint32_t));
    const auto length = static_cast<std::uint32_t>(text.size() < room ? text.size() : room);
    std::memcpy(block, &length, sizeof(length));
    std::memcpy(static_cast<char *>(block) + sizeof(length), text.data(), length);
}

class Node {
public:
    Node() = default;
    virtual ~Node() = default;

    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    virtual void prepare(const AvcPrepareInfo &info) { (void)info; }

    virtual bool prepare(const AvcPrepareInfo &info, std::string &error)
    {
        (void)error;
        prepare(info);
        return true;
    }

    virtual void inherit(const Node &previous) { (void)previous; }

    virtual std::uint32_t latencyFrames() const { return 0; }

    virtual void setParam(std::uint32_t index, float value) noexcept = 0;
    virtual void setOption(std::uint32_t index, std::string_view value)
    {
        (void)index;
        (void)value;
    }

    virtual bool status(AvcNodeStatus &out) const noexcept
    {
        (void)out;
        return false;
    }
    virtual void process(const AvcProcessCtx &ctx) noexcept = 0;
};

namespace detail {

template <class T>
AvcNodeVtable vtableFor()
{
    AvcNodeVtable vtable{};
    vtable.create = []() -> void * {
        // A constructor that throws must not unwind through C.
        try {
            return static_cast<Node *>(new T());
        } catch (...) {
            return nullptr;
        }
    };
    vtable.destroy = [](void *self) { delete static_cast<Node *>(self); };
    vtable.prepare = [](void *self, const AvcPrepareInfo *info, char *error,
                        std::uint32_t error_capacity) -> AvcResult {
        const auto writeError = [error, error_capacity](const char *message) {
            if (error == nullptr || error_capacity == 0) return;
            std::snprintf(error, error_capacity, "%s", message != nullptr ? message : "error");
        };
        try {
            std::string reason;
            if (!static_cast<Node *>(self)->prepare(*info, reason)) {
                writeError(reason.empty() ? "extension node prepare failed" : reason.c_str());
                return AVC_RESULT_ERROR;
            }
            return AVC_RESULT_OK;
        } catch (const std::exception &ex) {
            writeError(ex.what());
        } catch (...) {
            writeError("extension node threw while preparing");
        }
        return AVC_RESULT_ERROR;
    };
    vtable.inherit = [](void *self, const void *previous) {
        try {
            static_cast<Node *>(self)->inherit(*static_cast<const Node *>(previous));
        } catch (...) {
        }
    };
    vtable.latency_frames = [](const void *self) -> std::uint32_t {
        return static_cast<const Node *>(self)->latencyFrames();
    };
    vtable.set_param = [](void *self, std::uint32_t index, float value) {
        static_cast<Node *>(self)->setParam(index, value);
    };
    vtable.set_option = [](void *self, std::uint32_t index, const char *value) {
        try {
            static_cast<Node *>(self)->setOption(index, value != nullptr ? value : "");
        } catch (...) {
        }
    };
    vtable.process = [](void *self, const AvcProcessCtx *ctx) {
        static_cast<Node *>(self)->process(*ctx);
    };
    vtable.get_status = [](const void *self, AvcNodeStatus *status) -> AvcResult {
        if (status == nullptr || status->struct_size < sizeof(AvcNodeStatus)) {
            return AVC_RESULT_ERROR;
        }
        try {
            return static_cast<const Node *>(self)->status(*status) ? AVC_RESULT_OK
                                                                    : AVC_RESULT_ERROR;
        } catch (...) {
            return AVC_RESULT_ERROR;
        }
    };
    return vtable;
}

}

class NodeDesc {
public:
    NodeDesc(std::string type, std::string category, std::string label)
        : type_(std::move(type)), category_(std::move(category)), label_(std::move(label))
    {
    }

    NodeDesc &in(std::string name, std::string type = {})
    {
        inputs_.push_back({std::move(name), std::move(type)});
        return *this;
    }

    NodeDesc &out(std::string name, std::string type = {})
    {
        outputs_.push_back({std::move(name), std::move(type)});
        return *this;
    }

    NodeDesc &dynamicInputs()
    {
        dynamic_inputs_ = true;
        return *this;
    }

    NodeDesc &dynamicOutputs()
    {
        dynamic_outputs_ = true;
        return *this;
    }

    NodeDesc &floatParam(std::string name, float min, float max, float default_value,
                         std::string unit = {}, AvcParamCurve curve = AVC_CURVE_LINEAR,
                         std::string description = {})
    {
        Param param;
        param.name = std::move(name);
        param.type = AVC_PARAM_FLOAT;
        param.min = min;
        param.max = max;
        param.default_value = default_value;
        param.unit = std::move(unit);
        param.curve = curve;
        param.description = std::move(description);
        params_.push_back(std::move(param));
        return *this;
    }

    NodeDesc &boolParam(std::string name, bool default_value = false,
                        std::string description = {})
    {
        Param param;
        param.name = std::move(name);
        param.type = AVC_PARAM_BOOL;
        param.min = 0.0F;
        param.max = 1.0F;
        param.default_value = default_value ? 1.0F : 0.0F;
        param.description = std::move(description);
        params_.push_back(std::move(param));
        return *this;
    }

    NodeDesc &enumParam(std::string name, std::vector<std::string> values,
                        std::uint32_t default_index = 0, std::string description = {})
    {
        Param param;
        param.name = std::move(name);
        param.type = AVC_PARAM_ENUM;
        param.min = 0.0F;
        param.max = values.empty() ? 0.0F : static_cast<float>(values.size() - 1);
        param.default_value = static_cast<float>(default_index);
        param.values = std::move(values);
        param.description = std::move(description);
        params_.push_back(std::move(param));
        return *this;
    }

    NodeDesc &textParam(std::string name, std::string default_value = {},
                        std::string description = {})
    {
        return stringParam(std::move(name), AVC_PARAM_TEXT, std::move(default_value),
                           std::move(description));
    }

    NodeDesc &pathParam(std::string name, std::string default_value = {},
                        std::string description = {})
    {
        return stringParam(std::move(name), AVC_PARAM_PATH, std::move(default_value),
                           std::move(description));
    }

    NodeDesc &latency(std::uint32_t frames)
    {
        latency_frames_ = frames;
        return *this;
    }

    NodeDesc &notRealtimeSafe()
    {
        realtime_safe_ = false;
        return *this;
    }

    NodeDesc &recommendedColdBlock(std::uint32_t frames)
    {
        recommended_cold_block_ = frames;
        return *this;
    }

private:
    friend class Plugin;

    struct Port {
        std::string name;
        std::string type;
    };

    struct Param {
        std::string name;
        AvcParamType type = AVC_PARAM_FLOAT;
        float min = 0.0F;
        float max = 1.0F;
        float default_value = 0.0F;
        std::string unit;
        AvcParamCurve curve = AVC_CURVE_LINEAR;
        std::vector<std::string> values;
        std::string description;
        std::string default_text;
    };

    NodeDesc &stringParam(std::string name, AvcParamType type, std::string default_value,
                          std::string description)
    {
        Param param;
        param.name = std::move(name);
        param.type = type;
        param.default_text = std::move(default_value);
        param.description = std::move(description);
        params_.push_back(std::move(param));
        return *this;
    }

    void freeze();

    static const char *orNull(const std::string &text)
    {
        return text.empty() ? nullptr : text.c_str();
    }

    std::string type_;
    std::string category_;
    std::string label_;
    std::vector<Port> inputs_;
    std::vector<Port> outputs_;
    std::vector<Param> params_;
    bool dynamic_inputs_ = false;
    bool dynamic_outputs_ = false;
    std::uint32_t latency_frames_ = 0;
    bool realtime_safe_ = true;
    std::uint32_t recommended_cold_block_ = 0;

    // The frozen C view. Held here so its address is the descriptor's address.
    std::vector<AvcPortDesc> c_inputs_;
    std::vector<AvcPortDesc> c_outputs_;
    std::vector<AvcParamDesc> c_params_;
    std::vector<std::vector<const char *>> c_values_;
    AvcNodeDesc c_desc_{};
};

inline void NodeDesc::freeze()
{
    c_inputs_.clear();
    for (const Port &port : inputs_) {
        c_inputs_.push_back(AvcPortDesc{port.name.c_str(), orNull(port.type)});
    }
    c_outputs_.clear();
    for (const Port &port : outputs_) {
        c_outputs_.push_back(AvcPortDesc{port.name.c_str(), orNull(port.type)});
    }

    // Built first and in full, because a vector that grows later would move the
    // arrays the descriptors point into.
    c_values_.clear();
    c_values_.reserve(params_.size());
    for (const Param &param : params_) {
        std::vector<const char *> labels;
        for (const std::string &value : param.values) {
            labels.push_back(value.c_str());
        }
        labels.push_back(nullptr);
        c_values_.push_back(std::move(labels));
    }

    c_params_.clear();
    for (std::size_t i = 0; i < params_.size(); ++i) {
        const Param &param = params_[i];
        AvcParamDesc out{};
        out.name = param.name.c_str();
        out.type = param.type;
        out.min = param.min;
        out.max = param.max;
        out.default_value = param.default_value;
        out.unit = orNull(param.unit);
        out.curve = param.curve;
        out.values = param.values.empty() ? nullptr : c_values_[i].data();
        out.description = orNull(param.description);
        out.default_text = orNull(param.default_text);
        c_params_.push_back(out);
    }

    c_desc_ = AvcNodeDesc{};
    c_desc_.type = type_.c_str();
    c_desc_.category = category_.c_str();
    c_desc_.label = label_.c_str();
    c_desc_.inputs = c_inputs_.empty() ? nullptr : c_inputs_.data();
    c_desc_.input_count = static_cast<std::uint32_t>(c_inputs_.size());
    c_desc_.outputs = c_outputs_.empty() ? nullptr : c_outputs_.data();
    c_desc_.output_count = static_cast<std::uint32_t>(c_outputs_.size());
    c_desc_.params = c_params_.empty() ? nullptr : c_params_.data();
    c_desc_.param_count = static_cast<std::uint32_t>(c_params_.size());
    c_desc_.dynamic_inputs = dynamic_inputs_ ? 1U : 0U;
    c_desc_.dynamic_outputs = dynamic_outputs_ ? 1U : 0U;
    c_desc_.latency_frames = latency_frames_;
    c_desc_.realtime_safe = realtime_safe_ ? 1U : 0U;
    c_desc_.recommended_cold_block = recommended_cold_block_;
}

class Plugin {
public:
    Plugin(std::string id, std::string name, std::string version)
        : id_(std::move(id)), name_(std::move(name)), version_(std::move(version))
    {
    }

    Plugin(const Plugin &) = delete;
    Plugin &operator=(const Plugin &) = delete;

    Plugin &author(std::string who)
    {
        author_ = std::move(who);
        return *this;
    }

    Plugin &describe(std::string what)
    {
        description_ = std::move(what);
        return *this;
    }

    Plugin &portType(std::string name, AvcPortTransport transport, std::uint32_t bytes,
                     std::string label = {})
    {
        PortType entry;
        entry.name = std::move(name);
        entry.label = std::move(label);
        entry.transport = transport;
        entry.bytes = bytes;
        port_types_.push_back(std::move(entry));
        return *this;
    }

    template <class T>
    Plugin &node(NodeDesc desc)
    {
        static_assert(std::is_base_of_v<Node, T>, "a node type must derive from avc::sdk::Node");
        nodes_.push_back({std::move(desc), detail::vtableFor<T>()});
        return *this;
    }

    Plugin &boolSetting(std::string key, bool default_value, std::string label = {},
                        std::string description = {})
    {
        return setting(std::move(key), AVC_SETTING_BOOL, default_value ? "1" : "0",
                       std::move(label), std::move(description), 0.0, 1.0, {});
    }

    Plugin &intSetting(std::string key, long default_value, double min, double max,
                       std::string label = {}, std::string description = {})
    {
        return setting(std::move(key), AVC_SETTING_INT, std::to_string(default_value),
                       std::move(label), std::move(description), min, max, {});
    }

    Plugin &floatSetting(std::string key, double default_value, double min, double max,
                         std::string label = {}, std::string description = {})
    {
        return setting(std::move(key), AVC_SETTING_FLOAT, std::to_string(default_value),
                       std::move(label), std::move(description), min, max, {});
    }

    Plugin &textSetting(std::string key, std::string default_value, std::string label = {},
                        std::string description = {})
    {
        return setting(std::move(key), AVC_SETTING_TEXT, std::move(default_value),
                       std::move(label), std::move(description), 0.0, 0.0, {});
    }

    Plugin &pathSetting(std::string key, std::string default_value, std::string label = {},
                        std::string description = {})
    {
        return setting(std::move(key), AVC_SETTING_PATH, std::move(default_value),
                       std::move(label), std::move(description), 0.0, 0.0, {});
    }

    Plugin &enumSetting(std::string key, std::vector<std::string> values,
                        std::string default_value, std::string label = {},
                        std::string description = {})
    {
        return setting(std::move(key), AVC_SETTING_ENUM, std::move(default_value),
                       std::move(label), std::move(description), 0.0, 0.0, std::move(values));
    }

    Plugin &preset(std::string name, std::string json)
    {
        presets_.push_back({std::move(name), std::move(json)});
        return *this;
    }

    Plugin &uiAsset(std::string path, std::string data,
                    std::string mime_type = "application/octet-stream")
    {
        ui_assets_.push_back({std::move(path), std::move(mime_type), std::move(data)});
        return *this;
    }

    Plugin &uiEntry(std::string path)
    {
        ui_entry_ = std::move(path);
        return *this;
    }

    Plugin &onUiRequest(void (*handler)(const AvcUiRequest *request))
    {
        handle_ui_request_ = handler;
        return *this;
    }

    bool replyUi(std::uint64_t request_id, std::string_view json = "null") const
    {
        if (host_ == nullptr || host_->ui_reply == nullptr) return false;
        host_->ui_reply(host_->context, request_id, AVC_RESULT_OK, json.data(),
                        static_cast<std::uint32_t>(json.size()), nullptr);
        return true;
    }

    bool failUi(std::uint64_t request_id, std::string_view error) const
    {
        if (host_ == nullptr || host_->ui_reply == nullptr) return false;
        const std::string message(error);
        host_->ui_reply(host_->context, request_id, AVC_RESULT_ERROR, nullptr, 0,
                        message.c_str());
        return true;
    }

    bool emitUi(std::string_view event, std::string_view json = "null") const
    {
        if (host_ == nullptr || host_->ui_emit == nullptr) return false;
        const std::string name(event);
        host_->ui_emit(host_->context, name.c_str(), json.data(),
                       static_cast<std::uint32_t>(json.size()));
        return true;
    }

    Plugin &onConfigure(void (*handler)(const char *key, const char *value))
    {
        configure_ = handler;
        return *this;
    }

    Plugin &onShutdown(void (*handler)())
    {
        shutdown_ = handler;
        return *this;
    }

    const AvcHostApi *host() const noexcept { return host_; }

    void log(AvcLogLevel level, const std::string &message) const
    {
        if (host_ != nullptr && host_->log != nullptr) {
            host_->log(host_->context, level, message.c_str());
        }
    }

    std::string setting(const std::string &key, const std::string &fallback = {}) const
    {
        if (host_ == nullptr || host_->setting == nullptr) {
            return fallback;
        }
        const char *value = host_->setting(host_->context, key.c_str());
        return value != nullptr ? std::string(value) : fallback;
    }

    std::string configDir() const
    {
        return host_ != nullptr && host_->config_dir != nullptr ? host_->config_dir : std::string{};
    }

    std::string dataDir() const
    {
        return host_ != nullptr && host_->data_dir != nullptr ? host_->data_dir : std::string{};
    }

    const AvcPlugin *manifest();

    void bind(const AvcHostApi *host) noexcept { host_ = host; }

private:
    struct NodeEntry {
        NodeDesc desc;
        AvcNodeVtable vtable;
    };

    struct Setting {
        std::string key;
        std::string label;
        std::string description;
        AvcSettingType type = AVC_SETTING_TEXT;
        double min = 0.0;
        double max = 0.0;
        std::string default_value;
        std::vector<std::string> values;
    };

    struct Preset {
        std::string name;
        std::string json;
    };

    struct PortType {
        std::string name;
        std::string label;
        AvcPortTransport transport = AVC_PORT_STREAM;
        std::uint32_t bytes = 0;
    };

    struct UiAsset {
        std::string path;
        std::string mime_type;
        std::string data;
    };

    Plugin &setting(std::string key, AvcSettingType type, std::string default_value,
                    std::string label, std::string description, double min, double max,
                    std::vector<std::string> values)
    {
        Setting entry;
        entry.key = std::move(key);
        entry.label = std::move(label);
        entry.description = std::move(description);
        entry.type = type;
        entry.min = min;
        entry.max = max;
        entry.default_value = std::move(default_value);
        entry.values = std::move(values);
        settings_.push_back(std::move(entry));
        return *this;
    }

    static const char *orNull(const std::string &text)
    {
        return text.empty() ? nullptr : text.c_str();
    }

    std::string id_;
    std::string name_;
    std::string version_;
    std::string author_;
    std::string description_;

    std::vector<NodeEntry> nodes_;
    std::vector<PortType> port_types_;
    std::vector<Setting> settings_;
    std::vector<Preset> presets_;
    std::vector<UiAsset> ui_assets_;
    std::string ui_entry_;
    void (*configure_)(const char *, const char *) = nullptr;
    void (*shutdown_)() = nullptr;
    void (*handle_ui_request_)(const AvcUiRequest *) = nullptr;

    const AvcHostApi *host_ = nullptr;

    std::vector<AvcNodeType> c_nodes_;
    std::vector<AvcPortTypeDesc> c_port_types_;
    std::vector<AvcSettingDesc> c_settings_;
    std::vector<std::vector<const char *>> c_setting_values_;
    std::vector<AvcPresetDesc> c_presets_;
    std::vector<AvcUiAssetDesc> c_ui_assets_;
    AvcPlugin c_plugin_{};
};

inline const AvcPlugin *Plugin::manifest()
{
    c_nodes_.clear();
    c_nodes_.reserve(nodes_.size());
    for (NodeEntry &entry : nodes_) {
        entry.desc.freeze();
        c_nodes_.push_back(AvcNodeType{entry.desc.c_desc_, entry.vtable});
    }

    c_port_types_.clear();
    c_port_types_.reserve(port_types_.size());
    for (const PortType &entry : port_types_) {
        c_port_types_.push_back(AvcPortTypeDesc{entry.name.c_str(), orNull(entry.label),
                                                entry.transport, entry.bytes});
    }

    c_setting_values_.clear();
    c_setting_values_.reserve(settings_.size());
    for (const Setting &entry : settings_) {
        std::vector<const char *> labels;
        for (const std::string &value : entry.values) {
            labels.push_back(value.c_str());
        }
        labels.push_back(nullptr);
        c_setting_values_.push_back(std::move(labels));
    }

    c_settings_.clear();
    for (std::size_t i = 0; i < settings_.size(); ++i) {
        const Setting &entry = settings_[i];
        AvcSettingDesc out{};
        out.key = entry.key.c_str();
        out.label = orNull(entry.label);
        out.description = orNull(entry.description);
        out.type = entry.type;
        out.min = entry.min;
        out.max = entry.max;
        out.default_value = entry.default_value.c_str();
        out.values = entry.values.empty() ? nullptr : c_setting_values_[i].data();
        c_settings_.push_back(out);
    }

    c_presets_.clear();
    for (const Preset &entry : presets_) {
        c_presets_.push_back(AvcPresetDesc{entry.name.c_str(), entry.json.c_str()});
    }

    c_ui_assets_.clear();
    c_ui_assets_.reserve(ui_assets_.size());
    for (const UiAsset &entry : ui_assets_) {
        c_ui_assets_.push_back(AvcUiAssetDesc{sizeof(AvcUiAssetDesc), entry.path.c_str(),
                                              entry.mime_type.c_str(), entry.data.data(),
                                              static_cast<std::uint32_t>(entry.data.size())});
    }

    c_plugin_ = AvcPlugin{};
    c_plugin_.abi_version = AVC_ABI_VERSION;
    c_plugin_.struct_size = sizeof(AvcPlugin);
    c_plugin_.id = id_.c_str();
    c_plugin_.name = name_.c_str();
    c_plugin_.version = version_.c_str();
    c_plugin_.author = orNull(author_);
    c_plugin_.description = orNull(description_);
    c_plugin_.nodes = c_nodes_.empty() ? nullptr : c_nodes_.data();
    c_plugin_.node_count = static_cast<std::uint32_t>(c_nodes_.size());
    c_plugin_.port_types = c_port_types_.empty() ? nullptr : c_port_types_.data();
    c_plugin_.port_type_count = static_cast<std::uint32_t>(c_port_types_.size());
    c_plugin_.settings = c_settings_.empty() ? nullptr : c_settings_.data();
    c_plugin_.setting_count = static_cast<std::uint32_t>(c_settings_.size());
    c_plugin_.presets = c_presets_.empty() ? nullptr : c_presets_.data();
    c_plugin_.preset_count = static_cast<std::uint32_t>(c_presets_.size());
    c_plugin_.ui_assets = c_ui_assets_.empty() ? nullptr : c_ui_assets_.data();
    c_plugin_.ui_asset_count = static_cast<std::uint32_t>(c_ui_assets_.size());
    c_plugin_.ui_entry = orNull(ui_entry_);
    c_plugin_.handle_ui_request = handle_ui_request_;
    c_plugin_.configure = configure_;
    c_plugin_.shutdown = shutdown_;
    return &c_plugin_;
}

namespace detail {

template <class Build>
const AvcPlugin *init(Plugin &plugin, const AvcHostApi *host, Build build)
{
    if (host == nullptr || host->abi_version != AVC_ABI_VERSION
        || host->struct_size < sizeof(AvcHostApi)) {
        return nullptr;
    }
    plugin.bind(host);
    try {
        build();
    } catch (...) {
        plugin.log(AVC_LOG_ERROR, "the extension threw while registering itself");
        return nullptr;
    }
    return plugin.manifest();
}

}
}

#define AVC_PLUGIN_MAIN(plugin)                                                   \
    static void avcPluginBuild();                                                 \
    extern "C" AVC_PLUGIN_EXPORT const AvcPlugin *avc_plugin_init(                \
        const AvcHostApi *host)                                                   \
    {                                                                             \
        return ::avc::sdk::detail::init((plugin), host, &avcPluginBuild);         \
    }                                                                             \
    static void avcPluginBuild()

#endif 
