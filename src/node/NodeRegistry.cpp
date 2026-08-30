#include "node/NodeRegistry.hpp"

#include "node/PortTypeManifest.hpp"
#include "node/basic/GainNode.hpp"
#include "node/basic/MeterNode.hpp"
#include "node/basic/MixerNode.hpp"
#include "node/basic/SplitterNode.hpp"
#include "node/debug/AbNode.hpp"
#include "node/debug/ScopeNode.hpp"
#include "node/debug/SignalNode.hpp"
#include "node/dyn/CompressorNode.hpp"
#include "node/dyn/GateNode.hpp"
#include "node/eq/BiquadNode.hpp"
#include "node/fx/RingModNode.hpp"
#include "node/voice/FormantNode.hpp"
#include "node/voice/PitchNode.hpp"

#include "log/Log.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace avc::node {
namespace {

ParamDescriptor deviceParam(const char *name, const char *role)
{
    ParamDescriptor p;
    p.name = name;
    p.type = ParamType::Device;
    p.device_role = role;
    return p;
}

ParamDescriptor textParam(const char *name)
{
    ParamDescriptor p;
    p.name = name;
    p.type = ParamType::Text;
    return p;
}

ParamDescriptor exclusiveParam()
{
    ParamDescriptor p;
    p.name = "exclusive";
    p.type = ParamType::Bool;
    p.description = "Disconnect anything else that plays to this device, so everything "
                    "has to come through the graph.";
    return p;
}

NodeDescriptor ioDescriptor(const char *type, const char *label, NodeKind kind,
                            std::uint32_t channels, ParamDescriptor target)
{
    NodeDescriptor d;
    d.type = type;
    d.category = "io";
    d.label = label;
    d.kind = kind;
    d.params = {std::move(target)};
    if (kind == NodeKind::Playback) {
        d.params.push_back(exclusiveParam());
    }

    std::vector<PortDescriptor> ports;
    for (std::uint32_t i = 0; i < channels; ++i) {
        ports.push_back({(producesSignal(kind) ? "out_" : "in_") + std::to_string(i + 1)});
    }
    if (producesSignal(kind)) {
        d.outputs = std::move(ports);
        d.dynamic_outputs = true;
    } else {
        d.inputs = std::move(ports);
        d.dynamic_inputs = true;
    }
    return d;
}

}

int NodeDescriptor::indexOfParam(std::string_view name) const noexcept
{
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void NodeRegistry::seal() noexcept
{
    sealed_ = true;
    PortTypeManifest::instance().seal();
}

NodeRegistry &NodeRegistry::instance()
{
    static NodeRegistry registry;
    return registry;
}

NodeRegistry::NodeRegistry()
{
    registerBuiltins();
}

bool NodeRegistry::add(NodeDescriptor descriptor, Factory factory)
{
    if (sealed_) {
        spdlog::error("node type '{}' arrived after the registry was sealed", descriptor.type);
        return false;
    }
    const std::string type = descriptor.type;
    return entries_.emplace(type, Entry{std::move(descriptor), std::move(factory)}).second;
}

void NodeRegistry::registerBuiltins()
{
    add(ioDescriptor("capture", "Capture", NodeKind::Capture, 2,
                     deviceParam("source", "capture")),
        nullptr);
    add(ioDescriptor("playback", "Playback", NodeKind::Playback, 2,
                     deviceParam("device", "playback")),
        nullptr);
    add(ioDescriptor("virtual_speaker", "Virtual speaker", NodeKind::VirtualSpeaker, 2,
                     textParam("publish_as")),
        nullptr);
    add(ioDescriptor("virtual_mic", "Virtual mic", NodeKind::VirtualMic, 1,
                     textParam("publish_as")),
        nullptr);

    add(basic::GainNode::descriptor(), [] { return std::make_unique<basic::GainNode>(); });
    add(basic::MixerNode::descriptor(), [] { return std::make_unique<basic::MixerNode>(); });
    add(basic::SplitterNode::descriptor(), [] { return std::make_unique<basic::SplitterNode>(); });
    add(basic::MeterNode::descriptor(), [] { return std::make_unique<basic::MeterNode>(); });

    add(voice::PitchNode::descriptor(), [] { return std::make_unique<voice::PitchNode>(); });
    add(voice::FormantNode::descriptor(), [] { return std::make_unique<voice::FormantNode>(); });
    add(eq::BiquadNode::descriptor(), [] { return std::make_unique<eq::BiquadNode>(); });
    add(dyn::GateNode::descriptor(), [] { return std::make_unique<dyn::GateNode>(); });
    add(dyn::CompressorNode::descriptor(),
        [] { return std::make_unique<dyn::CompressorNode>(); });
    add(fx::RingModNode::descriptor(), [] { return std::make_unique<fx::RingModNode>(); });

    add(debug::SignalNode::descriptor(), [] { return std::make_unique<debug::SignalNode>(); });
    add(debug::ScopeNode::descriptor(), [] { return std::make_unique<debug::ScopeNode>(); });
    add(debug::AbNode::descriptor(), [] { return std::make_unique<debug::AbNode>(); });
}

const NodeDescriptor *NodeRegistry::find(std::string_view type) const noexcept
{
    const auto it = entries_.find(std::string(type));
    return it != entries_.end() ? &it->second.descriptor : nullptr;
}

std::unique_ptr<Node> NodeRegistry::create(std::string_view type) const
{
    const auto it = entries_.find(std::string(type));
    if (it == entries_.end() || !it->second.factory) {
        return nullptr;
    }
    return it->second.factory();
}

std::vector<const NodeDescriptor *> NodeRegistry::all() const
{
    std::vector<const NodeDescriptor *> out;
    out.reserve(entries_.size());
    for (const auto &[type, entry] : entries_) {
        out.push_back(&entry.descriptor);
    }
    std::sort(out.begin(), out.end(), [](const NodeDescriptor *a, const NodeDescriptor *b) {
        return a->type < b->type;
    });
    return out;
}

}
