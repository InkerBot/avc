#pragma once

#include "audio/AudioBackend.hpp"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace avc::audio {

struct PwNode {
    std::uint32_t id = 0;
    std::string name;
    std::string description;
    std::string media_class;

    std::string application;
    std::string media_name;
};

struct PwLink {
    std::uint32_t id = 0;
    std::uint32_t output_node = 0;
    std::uint32_t input_node = 0;
};

struct PwPort {
    std::uint32_t id = 0;
    std::uint32_t node_id = 0;
    std::uint32_t index = 0;
    std::string name;
    std::string channel;
    Direction direction = Direction::Input;
};

class PwRegistry {
public:
    PwRegistry() = default;
    ~PwRegistry();

    PwRegistry(const PwRegistry &) = delete;
    PwRegistry &operator=(const PwRegistry &) = delete;

    bool attach(pw_core *core);
    void detach() noexcept;

    void setChangeSignal(pw_thread_loop *loop) noexcept { signal_loop_ = loop; }

    void setLinkObserver(std::function<void(const PwLink &)> observer)
    {
        link_observer_ = std::move(observer);
    }

    const PwNode *findNodeById(std::uint32_t id) const noexcept;
    std::vector<PwLink> links() const;

    void destroyGlobal(std::uint32_t id) noexcept;

    const std::string &defaultSource() const noexcept { return default_source_; }
    const std::string &defaultSink() const noexcept { return default_sink_; }

    const PwNode *findNode(std::string_view name) const noexcept;
    std::vector<PwNode> nodes() const;

    std::vector<PwPort> portsOf(std::uint32_t node_id, Direction direction) const;

    std::uint32_t countPorts(std::uint32_t node_id, Direction direction) const;

private:
    static void onGlobal(void *data, std::uint32_t id, std::uint32_t permissions,
                         const char *type, std::uint32_t version, const spa_dict *props);
    static void onGlobalRemove(void *data, std::uint32_t id);

    static int onMetadataProperty(void *data, std::uint32_t subject, const char *key,
                                  const char *type, const char *value);

    void addNode(std::uint32_t id, const spa_dict *props);
    void addPort(std::uint32_t id, const spa_dict *props);
    void addLink(std::uint32_t id, const spa_dict *props);
    void bindDefaultMetadata(std::uint32_t id, const spa_dict *props);

    pw_thread_loop *signal_loop_ = nullptr;
    pw_registry *registry_ = nullptr;
    spa_hook listener_{};
    bool listening_ = false;

    pw_metadata *metadata_ = nullptr;
    spa_hook metadata_listener_{};
    bool metadata_listening_ = false;
    std::string default_source_;
    std::string default_sink_;

    std::unordered_map<std::uint32_t, PwNode> nodes_;
    std::unordered_map<std::uint32_t, PwPort> ports_;
    std::unordered_map<std::uint32_t, PwLink> links_;
    std::function<void(const PwLink &)> link_observer_;
};

}
