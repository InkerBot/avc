#include "audio/PwVirtualDevices.hpp"

#include "log/Log.hpp"

#include <spa/utils/keys.h>

#include <set>

namespace avc::audio {

PwVirtualDevices::~PwVirtualDevices()
{
    clear();
}

void PwVirtualDevices::destroy(Published &node) noexcept
{
    if (node.proxy != nullptr) {
        spdlog::info("unpublished '{}'", node.name);
        pw_proxy_destroy(node.proxy);
        node.proxy = nullptr;
    }
}

void PwVirtualDevices::clear() noexcept
{
    if (nodes_.empty() || !session_.valid()) {
        nodes_.clear();
        return;
    }
    LoopGuard guard(session_.loop());
    for (Published &node : nodes_) {
        destroy(node);
    }
    nodes_.clear();
}

bool PwVirtualDevices::create(const VirtualDeviceRequest &request, std::string &error)
{
    std::string position;
    for (std::uint32_t c = 0; c < request.channels; ++c) {
        position += (position.empty() ? "" : ",") + channelName(c, request.channels);
    }

    pw_properties *props = pw_properties_new(
        SPA_KEY_FACTORY_NAME, "support.null-audio-sink",
        PW_KEY_NODE_NAME, request.name.c_str(),
        PW_KEY_NODE_DESCRIPTION, request.name.c_str(),
        PW_KEY_MEDIA_CLASS,
        request.kind == IoKind::VirtualMic ? "Audio/Source/Virtual" : "Audio/Sink",
        PW_KEY_NODE_VIRTUAL, "true",
        // The node belongs to this process: when the daemon goes away, so does
        // the device, instead of littering the user's audio system. An engine
        // restart is not the daemon going away, which is exactly why these are
        // here and not there.
        PW_KEY_OBJECT_LINGER, "false",
        "monitor.passthrough", "true",
        nullptr);
    pw_properties_set(props, "audio.position", position.c_str());
    pw_properties_setf(props, PW_KEY_AUDIO_CHANNELS, "%u", request.channels);

    Published node;
    node.owner = request.owner;
    node.name = request.name;
    node.kind = request.kind;
    node.channels = request.channels;
    node.proxy = static_cast<pw_proxy *>(pw_core_create_object(
        session_.core(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
    pw_properties_free(props);

    if (node.proxy == nullptr) {
        error = "could not publish '" + request.name + "'";
        return false;
    }
    spdlog::info("published '{}' ({} ch)", node.name, node.channels);
    nodes_.push_back(std::move(node));
    return true;
}

bool PwVirtualDevices::present(const std::vector<VirtualDeviceRequest> &wanted)
{
    PwRegistry &registry = session_.registry();
    for (const VirtualDeviceRequest &request : wanted) {
        const PwNode *node = registry.findNode(request.name);
        if (node == nullptr) {
            return false;
        }
        const Direction needed =
            producesSignal(request.kind) ? Direction::Output : Direction::Input;
        if (registry.countPorts(node->id, needed) < request.channels) {
            return false;
        }
    }
    return true;
}

bool PwVirtualDevices::sameDevice(const Published &node, const VirtualDeviceRequest &request)
{
    return node.owner == request.owner && node.name == request.name && node.kind == request.kind
           && node.channels == request.channels;
}

bool PwVirtualDevices::ensure(const std::vector<VirtualDeviceRequest> &wanted, std::string &error)
{
    if (!session_.valid()) {
        error = "no connection to the audio server";
        return false;
    }

    std::set<std::string> names;
    for (const VirtualDeviceRequest &request : wanted) {
        if (request.channels < 1 || request.channels > 32) {
            error = "'" + request.owner + "' asks for " + std::to_string(request.channels)
                    + " channels";
            return false;
        }
        if (!names.insert(request.name).second) {
            error = "two nodes both publish '" + request.name + "'";
            return false;
        }
    }
    if (wanted.empty()) {
        return true;
    }

    LoopGuard guard(session_.loop());
    for (const VirtualDeviceRequest &request : wanted) {
        bool have = false;
        for (const Published &node : nodes_) {
            have = have || sameDevice(node, request);
        }
        if (!have && !create(request, error)) {
            return false;
        }
    }

    if (!session_.waitUntil([&] { return present(wanted); }, "the published devices")) {
        error = "the audio server did not finish building the published devices";
        return false;
    }
    return true;
}

void PwVirtualDevices::retain(const std::vector<VirtualDeviceRequest> &wanted) noexcept
{
    if (nodes_.empty() || !session_.valid()) {
        return;
    }

    LoopGuard guard(session_.loop());
    std::vector<Published> kept;
    for (Published &node : nodes_) {
        bool still = false;
        for (const VirtualDeviceRequest &request : wanted) {
            still = still || sameDevice(node, request);
        }
        if (still) {
            kept.push_back(std::move(node));
        } else {
            destroy(node);
        }
    }
    nodes_ = std::move(kept);
}

}
