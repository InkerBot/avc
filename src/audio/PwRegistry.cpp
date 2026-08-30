#include "audio/PwRegistry.hpp"

#include "log/Log.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace avc::audio {
namespace {

const char *dictGet(const spa_dict *props, const char *key)
{
    const char *value = props != nullptr ? spa_dict_lookup(props, key) : nullptr;
    return value != nullptr ? value : "";
}

std::uint32_t dictGetU32(const spa_dict *props, const char *key)
{
    const char *value = dictGet(props, key);
    return value[0] != '\0' ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 0;
}

std::string jsonName(const char *value)
{
    const std::string text(value != nullptr ? value : "");
    const std::size_t key = text.find("\"name\"");
    if (key == std::string::npos) {
        return {};
    }
    const std::size_t colon = text.find(':', key);
    const std::size_t open = colon == std::string::npos ? std::string::npos
                                                        : text.find('"', colon + 1);
    const std::size_t close = open == std::string::npos ? std::string::npos
                                                        : text.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    return text.substr(open + 1, close - open - 1);
}

}

PwRegistry::~PwRegistry()
{
    detach();
}

bool PwRegistry::attach(pw_core *core)
{
    static const pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = &PwRegistry::onGlobal,
        .global_remove = &PwRegistry::onGlobalRemove,
    };

    registry_ = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (registry_ == nullptr) {
        spdlog::error("pw_core_get_registry failed");
        return false;
    }

    spa_zero(listener_);
    pw_registry_add_listener(registry_, &listener_, &registry_events, this);
    listening_ = true;
    return true;
}

void PwRegistry::detach() noexcept
{
    if (metadata_listening_) {
        spa_hook_remove(&metadata_listener_);
        metadata_listening_ = false;
    }
    if (metadata_ != nullptr) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata_));
        metadata_ = nullptr;
    }
    default_source_.clear();
    default_sink_.clear();
    if (listening_) {
        spa_hook_remove(&listener_);
        listening_ = false;
    }
    if (registry_ != nullptr) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry_));
        registry_ = nullptr;
    }
    nodes_.clear();
    ports_.clear();
    links_.clear();
}

void PwRegistry::onGlobal(void *data, std::uint32_t id, std::uint32_t ,
                          const char *type, std::uint32_t , const spa_dict *props)
{
    auto *self = static_cast<PwRegistry *>(data);
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        self->addNode(id, props);
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        self->addPort(id, props);
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        self->addLink(id, props);
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        self->bindDefaultMetadata(id, props);
    }
    if (self->signal_loop_ != nullptr) {
        pw_thread_loop_signal(self->signal_loop_, false);
    }
}

void PwRegistry::onGlobalRemove(void *data, std::uint32_t id)
{
    auto *self = static_cast<PwRegistry *>(data);
    self->nodes_.erase(id);
    self->ports_.erase(id);
    self->links_.erase(id);
    if (self->signal_loop_ != nullptr) {
        pw_thread_loop_signal(self->signal_loop_, false);
    }
}

void PwRegistry::bindDefaultMetadata(std::uint32_t id, const spa_dict *props)
{
    static const pw_metadata_events events = {
        .version = PW_VERSION_METADATA_EVENTS,
        .property = &PwRegistry::onMetadataProperty,
    };

    if (metadata_ != nullptr || std::strcmp(dictGet(props, PW_KEY_METADATA_NAME), "default") != 0) {
        return;
    }
    metadata_ = static_cast<pw_metadata *>(
        pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (metadata_ == nullptr) {
        return;
    }
    spa_zero(metadata_listener_);
    pw_metadata_add_listener(metadata_, &metadata_listener_, &events, this);
    metadata_listening_ = true;
}

int PwRegistry::onMetadataProperty(void *data, std::uint32_t subject, const char *key,
                                   const char * , const char *value)
{
    auto *self = static_cast<PwRegistry *>(data);
    if (subject != PW_ID_CORE || key == nullptr) {
        return 0;
    }
    if (std::strcmp(key, "default.audio.source") == 0) {
        self->default_source_ = jsonName(value);
        spdlog::debug("default source is '{}'", self->default_source_);
    } else if (std::strcmp(key, "default.audio.sink") == 0) {
        self->default_sink_ = jsonName(value);
        spdlog::debug("default sink is '{}'", self->default_sink_);
    }
    return 0;
}

void PwRegistry::addNode(std::uint32_t id, const spa_dict *props)
{
    PwNode node;
    node.id = id;
    node.name = dictGet(props, PW_KEY_NODE_NAME);
    node.description = dictGet(props, PW_KEY_NODE_DESCRIPTION);
    node.media_class = dictGet(props, PW_KEY_MEDIA_CLASS);
    node.application = dictGet(props, PW_KEY_APP_NAME);
    node.media_name = dictGet(props, PW_KEY_MEDIA_NAME);
    nodes_[id] = std::move(node);
}

void PwRegistry::addPort(std::uint32_t id, const spa_dict *props)
{
    PwPort port;
    port.id = id;
    port.node_id = dictGetU32(props, PW_KEY_NODE_ID);
    port.index = dictGetU32(props, PW_KEY_PORT_ID);
    port.name = dictGet(props, PW_KEY_PORT_NAME);
    port.channel = dictGet(props, PW_KEY_AUDIO_CHANNEL);
    port.direction = std::strcmp(dictGet(props, PW_KEY_PORT_DIRECTION), "out") == 0
                         ? Direction::Output
                         : Direction::Input;
    ports_[id] = std::move(port);
}

void PwRegistry::addLink(std::uint32_t id, const spa_dict *props)
{
    PwLink link;
    link.id = id;
    link.output_node = dictGetU32(props, PW_KEY_LINK_OUTPUT_NODE);
    link.input_node = dictGetU32(props, PW_KEY_LINK_INPUT_NODE);
    links_[id] = link;

    if (link_observer_) {
        link_observer_(link);
    }
}

const PwNode *PwRegistry::findNodeById(std::uint32_t id) const noexcept
{
    const auto it = nodes_.find(id);
    return it != nodes_.end() ? &it->second : nullptr;
}

std::vector<PwLink> PwRegistry::links() const
{
    std::vector<PwLink> out;
    out.reserve(links_.size());
    for (const auto &[id, link] : links_) {
        out.push_back(link);
    }
    return out;
}

void PwRegistry::destroyGlobal(std::uint32_t id) noexcept
{
    if (registry_ != nullptr) {
        pw_registry_destroy(registry_, id);
    }
}

const PwNode *PwRegistry::findNode(std::string_view name) const noexcept
{
    for (const auto &[id, node] : nodes_) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

std::vector<PwNode> PwRegistry::nodes() const
{
    std::vector<PwNode> out;
    out.reserve(nodes_.size());
    for (const auto &[id, node] : nodes_) {
        out.push_back(node);
    }
    std::sort(out.begin(), out.end(),
              [](const PwNode &a, const PwNode &b) { return a.id < b.id; });
    return out;
}

std::vector<PwPort> PwRegistry::portsOf(std::uint32_t node_id, Direction direction) const
{
    std::vector<PwPort> out;
    for (const auto &[id, port] : ports_) {
        if (port.node_id == node_id && port.direction == direction) {
            out.push_back(port);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const PwPort &a, const PwPort &b) { return a.index < b.index; });
    return out;
}

std::uint32_t PwRegistry::countPorts(std::uint32_t node_id, Direction direction) const
{
    std::uint32_t count = 0;
    for (const auto &[id, port] : ports_) {
        count += static_cast<std::uint32_t>(port.node_id == node_id && port.direction == direction);
    }
    return count;
}

}
