#include "graph/GraphCompiler.hpp"

#include "graph/BufferPool.hpp"
#include "node/NodeRegistry.hpp"
#include "node/PortTypeManifest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace avc::graph {
namespace {

using node::NodeDescriptor;
using node::NodeKind;
using node::NodeRegistry;
using node::PortTypeManifest;
using node::PortTypeDescriptor;
using node::PortTransport;

constexpr std::uint32_t kNoSlot = 0xFFFFFFFFU;

struct Edge {
    std::uint32_t from_node = 0;
    std::uint32_t from_port = 0;
    std::uint32_t to_node = 0;
    std::uint32_t to_port = 0;

    std::uint32_t type = node::kAudioPortTypeIndex;
    float gain = 1.0F;
};

struct ForeignOut {
    std::uint32_t consumers = 0;
    std::uint32_t remaining = 0;
    std::uint32_t crossing = 0;
};

struct Vertex {
    const SpecNode *spec = nullptr;
    const NodeDescriptor *desc = nullptr;
    std::vector<std::uint32_t> io_slots;
    std::vector<std::string> in_names;
    std::vector<std::string> out_names;
    std::vector<std::uint32_t> in_types;
    std::vector<std::uint32_t> out_types;
    std::vector<std::vector<int>> in_edges;
    std::vector<std::uint32_t> out_fanout;
    std::vector<std::uint32_t> out_slot;
    std::vector<std::uint32_t> out_remaining;

    std::uint32_t domain = 0;
    std::vector<std::uint32_t> out_local;
    std::vector<std::map<std::uint32_t, ForeignOut>> out_foreign;

    int dsp_index = -1;
    int stage_index = -1;
};

audio::IoKind ioKindOf(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Playback:       return audio::IoKind::Playback;
    case NodeKind::VirtualSpeaker: return audio::IoKind::VirtualSpeaker;
    case NodeKind::VirtualMic:     return audio::IoKind::VirtualMic;
    case NodeKind::Capture:
    case NodeKind::Dsp:            break;
    }
    return audio::IoKind::Capture;
}

std::uint32_t ioChannels(const NodeDescriptor &desc, const SpecNode &node)
{
    if (producesSignal(desc.kind)) {
        return desc.dynamic_outputs && node.outputs > 0
                   ? node.outputs
                   : static_cast<std::uint32_t>(desc.outputs.size());
    }
    return desc.dynamic_inputs && node.inputs > 0
               ? node.inputs
               : static_cast<std::uint32_t>(desc.inputs.size());
}

const char *ioTargetKey(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Capture:  return "source";
    case NodeKind::Playback: return "device";
    default:                 return "publish_as";
    }
}

bool hasStuckSuccessor(const std::vector<std::uint32_t> &successors,
                       const std::vector<bool> &stuck)
{
    for (std::uint32_t next : successors) {
        if (stuck[next]) {
            return true;
        }
    }
    return false;
}

void trimToLoop(std::vector<bool> &stuck,
                const std::vector<std::vector<std::uint32_t>> &successors)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < stuck.size(); ++i) {
            if (!stuck[i] || hasStuckSuccessor(successors[i], stuck)) {
                continue;
            }
            stuck[i] = false;
            changed = true;
        }
    }
}

std::vector<std::string> portNames(const std::vector<node::PortDescriptor> &declared,
                                   std::uint32_t count, const char *prefix)
{
    std::vector<std::string> names;
    names.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        names.push_back(i < declared.size() ? declared[i].name
                                            : std::string(prefix) + "_" + std::to_string(i + 1));
    }
    return names;
}

bool resolvePortTypes(const std::vector<node::PortDescriptor> &declared,
                      const std::vector<std::string> &names, const std::string &node_id,
                      std::vector<std::uint32_t> &out, std::string &error)
{
    static const std::string kNone;
    const PortTypeManifest &manifest = PortTypeManifest::instance();

    out.clear();
    out.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string &type = i < declared.size() ? declared[i].type
                                  : declared.empty()  ? kNone
                                                      : declared.back().type;
        const int index = manifest.indexOf(type);
        if (index < 0) {
            error = "'" + node_id + "' has a port '" + names[i] + "' carrying '" + type
                    + "', which nothing declares";
            return false;
        }
        out.push_back(static_cast<std::uint32_t>(index));
    }
    return true;
}

int lookupPort(const std::vector<std::string> &names, const PortRef &ref)
{
    if (std::holds_alternative<std::uint32_t>(ref)) {
        const std::uint32_t index = std::get<std::uint32_t>(ref);
        return index < names.size() ? static_cast<int>(index) : -1;
    }
    const std::string &name = std::get<std::string>(ref);
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string portRefText(const PortRef &ref)
{
    return std::holds_alternative<std::string>(ref) ? std::get<std::string>(ref)
                                                    : std::to_string(std::get<std::uint32_t>(ref));
}

float edgeGain(float db) noexcept
{
    db = std::clamp(db, -90.0F, 24.0F);
    return db <= -90.0F ? 0.0F : std::pow(10.0F, db / 20.0F);
}

}

class GraphCompiler::Builder {
public:
    Builder(const GraphSpec &spec, const CompileEnv &env, const CompiledGraph *previous)
        : spec_(spec), env_(env), previous_(previous)
    {
    }

    std::unique_ptr<CompiledGraph> build(std::string &error);

private:
    bool resolveNodes(std::string &error);
    bool resolveIo(Vertex &vertex, const SpecNode &spec_node, std::string &error);
    bool resolveEdges(std::string &error);
    bool assignDomains(std::string &error);
    bool topoSort(std::string &error);
    void planCrossings();
    bool instantiate(std::string &error);
    void assignSlots();
    void resolvePointers();

    struct Source {
        std::uint32_t slot = 0;
        std::uint32_t type = node::kAudioPortTypeIndex;
        std::int32_t pred_stage = -1;
        std::uint32_t extra = 0;
    };
    Source sourceOf(std::uint32_t domain, int edge);
    void releaseSource(std::uint32_t domain, int edge);

    void applyParams(node::Node &node, const Vertex &vertex);
    void applyOptions(node::Node &node, const Vertex &vertex);
    void inheritState(node::Node &node, const Vertex &vertex);

    const GraphSpec &spec_;
    const CompileEnv &env_;
    const CompiledGraph *previous_;

    std::vector<Vertex> vertices_;
    std::vector<Edge> edges_;
    std::unordered_map<std::string, std::uint32_t> by_id_;
    std::vector<std::uint32_t> topo_;

    std::vector<std::vector<BufferPool>> pools_;

    BufferPool &poolFor(std::uint32_t domain, std::uint32_t type) noexcept
    {
        return pools_[domain][type];
    }

    struct SlotSpot {
        std::uint32_t domain = 0;
        std::uint32_t type = node::kAudioPortTypeIndex;
        std::int64_t slot = -1;
    };
    std::vector<SlotSpot> stage_in_slots_;
    std::vector<SlotSpot> stage_out_slots_;
    std::vector<SlotSpot> mix_source_slots_;
    std::vector<SlotSpot> mix_output_slots_;
    std::vector<SlotSpot> output_source_slots_;

    std::unique_ptr<CompiledGraph> graph_ = std::make_unique<CompiledGraph>();
};

bool GraphCompiler::Builder::resolveNodes(std::string &error)
{
    const NodeRegistry &registry = NodeRegistry::instance();

    for (const SpecNode &spec_node : spec_.nodes) {
        if (!by_id_.emplace(spec_node.id, static_cast<std::uint32_t>(vertices_.size())).second) {
            error = "duplicate node id '" + spec_node.id + "'";
            return false;
        }

        Vertex vertex;
        vertex.spec = &spec_node;
        vertex.desc = registry.find(spec_node.type);
        if (vertex.desc == nullptr) {
            error = "node '" + spec_node.id + "' has unknown type '" + spec_node.type + "'";
            return false;
        }

        const std::uint32_t n_in =
            vertex.desc->dynamic_inputs && spec_node.inputs > 0
                ? spec_node.inputs
                : static_cast<std::uint32_t>(vertex.desc->inputs.size());
        const std::uint32_t n_out =
            vertex.desc->dynamic_outputs && spec_node.outputs > 0
                ? spec_node.outputs
                : static_cast<std::uint32_t>(vertex.desc->outputs.size());
        vertex.in_names = portNames(vertex.desc->inputs, n_in, "in");
        vertex.out_names = portNames(vertex.desc->outputs, n_out, "out");
        if (!resolvePortTypes(vertex.desc->inputs, vertex.in_names, spec_node.id, vertex.in_types,
                              error)
            || !resolvePortTypes(vertex.desc->outputs, vertex.out_names, spec_node.id,
                                 vertex.out_types, error)) {
            return false;
        }
        vertex.in_edges.resize(n_in);
        vertex.out_fanout.assign(vertex.out_names.size(), 0);
        vertex.out_slot.assign(vertex.out_names.size(), kNoSlot);
        vertex.out_remaining.assign(vertex.out_names.size(), 0);
        vertex.out_local.assign(vertex.out_names.size(), 0);
        vertex.out_foreign.assign(vertex.out_names.size(), {});

        if (isIo(vertex.desc->kind) && !resolveIo(vertex, spec_node, error)) {
            return false;
        }
        vertices_.push_back(std::move(vertex));
    }
    return true;
}

bool GraphCompiler::Builder::resolveIo(Vertex &vertex, const SpecNode &spec_node,
                                       std::string &error)
{
    const std::uint32_t channels = ioChannels(*vertex.desc, spec_node);
    const auto it = env_.io_slots.find(spec_node.id);

    if (it == env_.io_slots.end() || it->second.size() < channels) {
        const auto target = spec_node.options.find(ioTargetKey(vertex.desc->kind));
        error = "'" + spec_node.id + "' has no ports"
                + (target == spec_node.options.end() || target->second.empty()
                       ? "; nothing is selected on it"
                       : " for '" + target->second + "'");
        return false;
    }
    const std::vector<std::uint32_t> &types =
        producesSignal(vertex.desc->kind) ? vertex.out_types : vertex.in_types;
    for (std::size_t port = 0; port < types.size(); ++port) {
        if (types[port] != node::kAudioPortTypeIndex) {
            error = "'" + spec_node.id + "' is an io node, so its ports carry audio, but '"
                    + (producesSignal(vertex.desc->kind) ? vertex.out_names[port]
                                                         : vertex.in_names[port])
                    + "' says it carries '"
                    + PortTypeManifest::instance().at(types[port]).name + "'";
            return false;
        }
    }

    vertex.io_slots.assign(it->second.begin(), it->second.begin() + channels);
    return true;
}

bool GraphCompiler::Builder::resolveEdges(std::string &error)
{
    for (const SpecEdge &spec_edge : spec_.edges) {
        const auto from_it = by_id_.find(spec_edge.from.node);
        const auto to_it = by_id_.find(spec_edge.to.node);
        if (from_it == by_id_.end() || to_it == by_id_.end()) {
            error = "edge references unknown node '"
                    + (from_it == by_id_.end() ? spec_edge.from.node : spec_edge.to.node) + "'";
            return false;
        }

        Vertex &from = vertices_[from_it->second];
        Vertex &to = vertices_[to_it->second];
        const int from_port = lookupPort(from.out_names, spec_edge.from.port);
        const int to_port = lookupPort(to.in_names, spec_edge.to.port);

        if (from_port < 0 || to_port < 0) {
            error = "edge " + spec_edge.from.node + ":" + portRefText(spec_edge.from.port) + " -> "
                    + spec_edge.to.node + ":" + portRefText(spec_edge.to.port)
                    + " references a port that does not exist";
            return false;
        }
        const std::uint32_t from_type = from.out_types[static_cast<std::size_t>(from_port)];
        const std::uint32_t to_type = to.in_types[static_cast<std::size_t>(to_port)];
        if (from_type != to_type) {
            const PortTypeManifest &manifest = PortTypeManifest::instance();
            error = "edge " + spec_edge.from.node + ":" + from.out_names[from_port] + " -> "
                    + spec_edge.to.node + ":" + to.in_names[to_port] + " connects '"
                    + manifest.at(from_type).name + "' to '" + manifest.at(to_type).name
                    + "'; a wire carries one type";
            return false;
        }

        const bool already_connected = !to.in_edges[static_cast<std::size_t>(to_port)].empty();
        if (from_type != node::kAudioPortTypeIndex
            && (already_connected || spec_edge.gain_db != 0.0F)) {
            error = "input " + spec_edge.to.node + ":" + to.in_names[to_port]
                    + " carries '" + PortTypeManifest::instance().at(from_type).name
                    + "'; only audio inputs can mix multiple edges or apply gain";
            return false;
        }

        to.in_edges[to_port].push_back(static_cast<int>(edges_.size()));
        ++from.out_fanout[from_port];
        edges_.push_back({from_it->second, static_cast<std::uint32_t>(from_port), to_it->second,
                          static_cast<std::uint32_t>(to_port), from_type,
                          edgeGain(spec_edge.gain_db)});
    }
    return true;
}

bool GraphCompiler::Builder::assignDomains(std::string &error)
{
    auto hot = std::make_unique<CompiledGraph::Domain>();
    hot->name = types::kHotDomain;
    hot->cold = false;
    hot->block = env_.max_quantum;
    graph_->domains_.push_back(std::move(hot));

    std::unordered_map<std::string, std::uint32_t> by_name;

    for (Vertex &vertex : vertices_) {
        const std::string &name = vertex.spec->domain;
        if (name.empty() || name == types::kHotDomain) {
            if (!vertex.desc->realtime_safe) {
                error = "'" + vertex.spec->id + "' is a '" + vertex.desc->type
                        + "', which says it is not realtime safe; put it on a cold path";
                return false;
            }
            continue;
        }
        if (isIo(vertex.desc->kind)) {
            error = "'" + vertex.spec->id + "' is an io node and cannot run in domain '" + name
                    + "'; the audio system drives its ports";
            return false;
        }

        const auto found = by_name.find(name);
        if (found != by_name.end()) {
            vertex.domain = found->second;
            continue;
        }

        const auto settings = spec_.domains.find(name);
        auto domain = std::make_unique<CompiledGraph::Domain>();
        domain->name = name;
        domain->cold = true;
        domain->block = std::clamp(settings != spec_.domains.end() && settings->second.block > 0
                                       ? settings->second.block
                                       : types::kDefaultColdBlock,
                                   types::kMinColdBlock, types::kMaxColdBlock);
        domain->safety = std::min(settings != spec_.domains.end() ? settings->second.safety
                                                                  : types::kDefaultColdSafety,
                                  types::kMaxColdSafety);

        vertex.domain = static_cast<std::uint32_t>(graph_->domains_.size());
        by_name.emplace(name, vertex.domain);
        graph_->domains_.push_back(std::move(domain));
    }

    pools_.assign(graph_->domains_.size(),
                  std::vector<BufferPool>(PortTypeManifest::instance().size()));
    return true;
}

void GraphCompiler::Builder::planCrossings()
{
    for (const Edge &edge : edges_) {
        Vertex &producer = vertices_[edge.from_node];
        const std::uint32_t consumer_domain = vertices_[edge.to_node].domain;

        if (producer.domain == consumer_domain) {
            ++producer.out_local[edge.from_port];
            continue;
        }
        ++producer.out_foreign[edge.from_port][consumer_domain].consumers;
    }

    const PortTypeManifest &manifest = PortTypeManifest::instance();

    for (Vertex &vertex : vertices_) {
        for (std::size_t port = 0; port < vertex.out_foreign.size(); ++port) {
            for (auto &[destination, foreign] : vertex.out_foreign[port]) {
                const CompiledGraph::Domain &src = *graph_->domains_[vertex.domain];
                const CompiledGraph::Domain &dst = *graph_->domains_[destination];
                const std::uint32_t type = vertex.out_types[port];
                const PortTypeDescriptor &carried = manifest.at(type);

                auto crossing = std::make_unique<Crossing>();
                crossing->type = type;
                crossing->src_domain = vertex.domain;
                crossing->dst_domain = destination;
                crossing->streaming = carried.transport == PortTransport::Stream;

                if (crossing->streaming) {
                    const std::uint32_t prefill = src.cold ? src.block * (1 + src.safety) : 0;
                    crossing->latency = prefill;

                    const std::uint32_t written = src.cold ? src.block : env_.max_quantum;
                    const std::uint32_t taken = dst.cold ? dst.block : env_.max_quantum;
                    crossing->timeline.reset(
                        prefill + written + taken + std::max(written, taken), carried.bytes);
                    crossing->timeline.prefill(0, prefill);
                } else {
                    // A value hop delays no audio: there is nothing to
                    // accumulate and nothing prefilled in front of it, only the
                    // newest answer waiting to be picked up.
                    crossing->value.reset(carried.bytes);
                }

                foreign.remaining = foreign.consumers;
                foreign.crossing = static_cast<std::uint32_t>(graph_->crossings_.size());
                graph_->domains_[vertex.domain]->out_crossings.push_back(foreign.crossing);
                graph_->domains_[destination]->in_crossings.push_back(foreign.crossing);
                graph_->crossings_.push_back(std::move(crossing));
            }
        }
    }

    // A domain with a stream on it is paced by its published timeline. One
    // with nothing but values on it would run as fast as its thread can spin,
    // so it has to be told that its block is its rate.
    for (std::size_t i = 1; i < graph_->domains_.size(); ++i) {
        CompiledGraph::Domain &domain = *graph_->domains_[i];
        const auto streams = [this](std::uint32_t index) {
            return graph_->crossings_[index]->streaming;
        };
        domain.paced = std::any_of(domain.in_crossings.begin(), domain.in_crossings.end(), streams)
                       || std::any_of(domain.out_crossings.begin(), domain.out_crossings.end(),
                                      streams);
    }
}

bool GraphCompiler::Builder::topoSort(std::string &error)
{
    std::vector<std::uint32_t> in_degree(vertices_.size(), 0);
    std::vector<std::vector<std::uint32_t>> successors(vertices_.size());

    for (const Edge &edge : edges_) {
        ++in_degree[edge.to_node];
        successors[edge.from_node].push_back(edge.to_node);
    }

    std::deque<std::uint32_t> ready;
    for (std::uint32_t i = 0; i < vertices_.size(); ++i) {
        if (in_degree[i] == 0) {
            ready.push_back(i);
        }
    }

    topo_.reserve(vertices_.size());
    while (!ready.empty()) {
        const std::uint32_t current = ready.front();
        ready.pop_front();
        topo_.push_back(current);
        for (std::uint32_t next : successors[current]) {
            if (--in_degree[next] == 0) {
                ready.push_back(next);
            }
        }
    }

    if (topo_.size() != vertices_.size()) {
        std::vector<bool> stuck(vertices_.size(), false);
        for (std::uint32_t i = 0; i < vertices_.size(); ++i) {
            stuck[i] = in_degree[i] > 0;
        }
        trimToLoop(stuck, successors);

        std::string names;
        for (std::uint32_t i = 0; i < vertices_.size(); ++i) {
            if (stuck[i]) {
                names += (names.empty() ? "" : " -> ") + vertices_[i].spec->id;
            }
        }
        error = "graph has a feedback loop through: " + names;
        return false;
    }
    return true;
}

void GraphCompiler::Builder::applyParams(node::Node &node, const Vertex &vertex)
{
    for (std::size_t i = 0; i < vertex.desc->params.size(); ++i) {
        const node::ParamDescriptor &param = vertex.desc->params[i];
        if (param.type == node::ParamType::Text || param.type == node::ParamType::Path
            || param.type == node::ParamType::Device) {
            continue;
        }
        const auto it = vertex.spec->params.find(param.name);
        const float raw = it != vertex.spec->params.end() ? it->second : param.default_value;
        node.setParam(static_cast<std::uint32_t>(i), std::clamp(raw, param.min, param.max));
    }
}

void GraphCompiler::Builder::applyOptions(node::Node &node, const Vertex &vertex)
{
    for (std::size_t i = 0; i < vertex.desc->params.size(); ++i) {
        const node::ParamDescriptor &param = vertex.desc->params[i];
        if (param.type != node::ParamType::Text && param.type != node::ParamType::Path
            && param.type != node::ParamType::Device) {
            continue;
        }
        const auto it = vertex.spec->options.find(param.name);
        const std::string &value =
            it != vertex.spec->options.end() ? it->second : param.default_text;
        node.setOption(static_cast<std::uint32_t>(i), value);
    }
}

void GraphCompiler::Builder::inheritState(node::Node &node, const Vertex &vertex)
{
    if (previous_ == nullptr) {
        return;
    }
    const int index = previous_->indexOfNode(vertex.spec->id);
    if (index < 0 || previous_->nodeTypeAt(static_cast<std::size_t>(index)) != vertex.spec->type) {
        return;
    }
    const node::Node *old_node = previous_->nodeAt(static_cast<std::size_t>(index));
    if (old_node != nullptr) {
        node.inherit(*old_node);
    }
}

bool GraphCompiler::Builder::instantiate(std::string &error)
{
    const NodeRegistry &registry = NodeRegistry::instance();

    for (Vertex &vertex : vertices_) {
        if (vertex.desc->kind != NodeKind::Dsp) {
            continue;
        }
        std::unique_ptr<node::Node> node = registry.create(vertex.spec->type);
        if (node == nullptr) {
            error = "node type '" + vertex.spec->type + "' has no implementation";
            return false;
        }

        applyParams(*node, vertex);
        applyOptions(*node, vertex);
        const node::PrepareInfo prepare_info{
            env_.sample_rate, graph_->domains_[vertex.domain]->block,
            static_cast<std::uint32_t>(vertex.in_names.size()),
            static_cast<std::uint32_t>(vertex.out_names.size())};
        std::string prepare_error;
        if (!node->prepareChecked(prepare_info, prepare_error)) {
            error = "node '" + vertex.spec->id + "' could not prepare"
                    + (prepare_error.empty() ? std::string{} : ": " + prepare_error);
            return false;
        }
        inheritState(*node, vertex);

        vertex.dsp_index = static_cast<int>(graph_->nodes_.size());
        graph_->nodes_.push_back(std::move(node));
        graph_->node_ids_.push_back(vertex.spec->id);
        graph_->node_types_.push_back(vertex.spec->type);
        graph_->node_domains_.push_back(vertex.domain);
    }
    return true;
}

GraphCompiler::Builder::Source GraphCompiler::Builder::sourceOf(std::uint32_t domain, int edge)
{
    const Edge &in = edges_[static_cast<std::size_t>(edge)];
    Vertex &producer = vertices_[in.from_node];

    if (producer.domain == domain) {
        return {producer.out_slot[in.from_port], in.type, producer.stage_index, 0};
    }
    const ForeignOut &foreign = producer.out_foreign[in.from_port].at(domain);
    const Crossing &crossing = *graph_->crossings_[foreign.crossing];
    return {crossing.dst_slot, in.type, producer.stage_index, crossing.latency};
}

void GraphCompiler::Builder::releaseSource(std::uint32_t domain, int edge)
{
    const Edge &in = edges_[static_cast<std::size_t>(edge)];
    Vertex &producer = vertices_[in.from_node];

    if (producer.domain == domain) {
        if (--producer.out_remaining[in.from_port] == 0) {
            poolFor(producer.domain, in.type).release(producer.out_slot[in.from_port]);
        }
        return;
    }
    ForeignOut &foreign = producer.out_foreign[in.from_port].at(domain);
    if (--foreign.remaining == 0) {
        poolFor(domain, in.type).release(graph_->crossings_[foreign.crossing]->dst_slot);
    }
}

void GraphCompiler::Builder::assignSlots()
{
    // A crossing lands its samples before any stage of the destination domain
    // runs, exactly like an engine input, so its slot has to be reserved from
    // the top of that domain's pass rather than from wherever in the order its
    // first reader happens to sit.
    for (std::unique_ptr<Crossing> &crossing : graph_->crossings_) {
        crossing->dst_slot = poolFor(crossing->dst_domain, crossing->type).acquire();
    }

    for (std::uint32_t index : topo_) {
        Vertex &vertex = vertices_[index];

        // An output read from another domain is read at the end of this
        // domain's pass, so it stays live for the whole of it: one extra
        // consumer that is never counted down.
        const auto publish = [&](std::size_t port, std::uint32_t slot) {
            vertex.out_slot[port] = slot;
            vertex.out_remaining[port] =
                vertex.out_local[port] + (vertex.out_foreign[port].empty() ? 0U : 1U);
            for (const auto &entry : vertex.out_foreign[port]) {
                graph_->crossings_[entry.second.crossing]->src_slot = slot;
            }
        };

        if (producesSignal(vertex.desc->kind)) {
            BufferPool &pool = poolFor(vertex.domain, node::kAudioPortTypeIndex);
            for (std::size_t port = 0; port < vertex.out_slot.size(); ++port) {
                const std::uint32_t slot = pool.acquire();
                publish(port, slot);
                graph_->in_bindings_.push_back({vertex.io_slots[port], slot, nullptr});
                if (vertex.out_remaining[port] == 0) {
                    pool.release(slot);
                }
            }
            continue;
        }

        if (isIo(vertex.desc->kind)) {
            for (std::size_t port = 0; port < vertex.in_edges.size(); ++port) {
                const std::vector<int> &incoming = vertex.in_edges[port];
                if (incoming.empty()) {
                    // Nothing patched in, but the port still has to be written
                    // or it replays whatever was last in the buffer.
                    graph_->silent_outputs_.push_back(vertex.io_slots[port]);
                    continue;
                }

                CompiledGraph::OutputBinding binding;
                binding.endpoint = vertex.io_slots[port];
                binding.source_offset = static_cast<std::uint32_t>(output_source_slots_.size());
                binding.source_count = static_cast<std::uint32_t>(incoming.size());
                for (int edge : incoming) {
                    // Never released, so every source buffer stays live until
                    // the graph ends and copyOut can sum it into the endpoint.
                    const Source source = sourceOf(vertex.domain, edge);
                    output_source_slots_.push_back(
                        {vertex.domain, source.type, static_cast<std::int64_t>(source.slot)});
                    graph_->output_gains_.push_back(edges_[static_cast<std::size_t>(edge)].gain);
                    graph_->output_stages_.push_back({source.pred_stage, source.extra});
                    graph_->output_nodes_.push_back(vertex.spec->id);
                }
                graph_->out_bindings_.push_back(binding);
            }
            continue;
        }

        // Outputs are acquired before inputs are released, so a node can never
        // be handed the same buffer for reading and writing.
        CompiledGraph::Stage stage;
        stage.node = graph_->nodes_[static_cast<std::size_t>(vertex.dsp_index)].get();
        stage.node_index = static_cast<std::uint32_t>(vertex.dsp_index);
        stage.output_offset = static_cast<std::uint32_t>(stage_out_slots_.size());
        stage.n_outputs = static_cast<std::uint32_t>(vertex.out_slot.size());
        stage.mix_offset = static_cast<std::uint32_t>(graph_->mix_bindings_.size());
        for (std::size_t port = 0; port < vertex.out_slot.size(); ++port) {
            publish(port, poolFor(vertex.domain, vertex.out_types[port]).acquire());
            stage_out_slots_.push_back({vertex.domain, vertex.out_types[port],
                                        vertex.out_slot[port]});
        }

        stage.input_offset = static_cast<std::uint32_t>(stage_in_slots_.size());
        stage.n_inputs = static_cast<std::uint32_t>(vertex.in_edges.size());
        stage.pred_offset = static_cast<std::uint32_t>(graph_->stage_preds_.size());
        std::vector<SlotSpot> temporary_mixes;

        for (std::size_t port = 0; port < vertex.in_edges.size(); ++port) {
            const std::vector<int> &incoming = vertex.in_edges[port];
            if (incoming.empty()) {
                stage_in_slots_.push_back({vertex.domain, vertex.in_types[port], -1});
                graph_->stage_preds_.push_back({-1, 0});
                continue;
            }

            const Edge &only = edges_[static_cast<std::size_t>(incoming.front())];
            if (incoming.size() == 1 && only.gain == 1.0F) {
                const Source source = sourceOf(vertex.domain, incoming.front());
                stage_in_slots_.push_back(
                    {vertex.domain, source.type, static_cast<std::int64_t>(source.slot)});
                graph_->stage_preds_.push_back({source.pred_stage, source.extra});
                continue;
            }

            const std::uint32_t mixed_slot =
                poolFor(vertex.domain, node::kAudioPortTypeIndex).acquire();
            const SlotSpot mixed{vertex.domain, node::kAudioPortTypeIndex,
                                 static_cast<std::int64_t>(mixed_slot)};
            stage_in_slots_.push_back(mixed);
            temporary_mixes.push_back(mixed);

            CompiledGraph::MixBinding binding;
            binding.source_offset = static_cast<std::uint32_t>(mix_source_slots_.size());
            binding.source_count = static_cast<std::uint32_t>(incoming.size());
            for (int edge : incoming) {
                const Source source = sourceOf(vertex.domain, edge);
                mix_source_slots_.push_back(
                    {vertex.domain, source.type, static_cast<std::int64_t>(source.slot)});
                graph_->mix_gains_.push_back(edges_[static_cast<std::size_t>(edge)].gain);
                graph_->stage_preds_.push_back({source.pred_stage, source.extra});
            }
            mix_output_slots_.push_back(mixed);
            graph_->mix_bindings_.push_back(binding);
            ++stage.mix_count;
        }
        stage.pred_count =
            static_cast<std::uint32_t>(graph_->stage_preds_.size()) - stage.pred_offset;

        vertex.stage_index = static_cast<int>(graph_->order_.size());
        graph_->domains_[vertex.domain]->exec.push_back(
            static_cast<std::uint32_t>(graph_->order_.size()));
        graph_->order_.push_back(stage);

        for (const std::vector<int> &incoming : vertex.in_edges) {
            for (int edge : incoming) {
                releaseSource(vertex.domain, edge);
            }
        }
        for (const SlotSpot &mix : temporary_mixes) {
            poolFor(mix.domain, mix.type).release(static_cast<std::uint32_t>(mix.slot));
        }
        for (std::size_t port = 0; port < vertex.out_slot.size(); ++port) {
            if (vertex.out_remaining[port] == 0) {
                poolFor(vertex.domain, vertex.out_types[port]).release(vertex.out_slot[port]);
            }
        }
    }
}

void GraphCompiler::Builder::resolvePointers()
{
    graph_->sample_rate_ = env_.sample_rate;
    graph_->max_quantum_ = env_.max_quantum;
    graph_->generation_ = env_.generation;

    // Every buffer in the graph is allocated here and nowhere else, and nothing
    // is resized afterwards -- which is what makes it safe for everything below
    // to hold a bare pointer into it for the life of the graph.
    const PortTypeManifest &manifest = PortTypeManifest::instance();
    graph_->slot_count_ = 0;
    for (std::size_t i = 0; i < graph_->domains_.size(); ++i) {
        CompiledGraph::Domain &domain = *graph_->domains_[i];
        domain.stores.assign(manifest.size(), CompiledGraph::TypeStore{});
        domain.slot_count = 0;
        for (std::uint32_t type = 0; type < manifest.size(); ++type) {
            CompiledGraph::TypeStore &store = domain.stores[type];
            store.slots = pools_[i][type].highWater();
            store.stride = manifest.at(type).blockBytes(domain.block);
            store.storage.assign(static_cast<std::size_t>(store.slots) * store.stride,
                                 std::byte{0});
            store.states.assign(store.slots, StreamState{});
            domain.slot_count += store.slots;
        }
        graph_->slot_count_ += domain.slot_count;
    }

    // Two views of the same slot table: the audio one a DSP node has always
    // been handed, null where the port carries something else, and the untyped
    // one beside it. Both are resolved here so that neither costs anything per
    // block. An audio store's stride is a whole number of samples and the
    // allocator's blocks are aligned well past four, so the cast is sound.
    const auto audioView = [&](const SlotSpot &spot, std::byte *data) -> types::Sample * {
        return spot.type == node::kAudioPortTypeIndex ? reinterpret_cast<types::Sample *>(data)
                                                      : nullptr;
    };

    graph_->input_ptrs_.reserve(stage_in_slots_.size());
    graph_->in_block_ptrs_.reserve(stage_in_slots_.size());
    graph_->input_states_.reserve(stage_in_slots_.size());
    for (const SlotSpot &spot : stage_in_slots_) {
        std::byte *data =
            spot.slot < 0 ? nullptr
                          : graph_->slotData(spot.domain, spot.type,
                                             static_cast<std::uint32_t>(spot.slot));
        graph_->input_ptrs_.push_back(audioView(spot, data));
        graph_->in_block_ptrs_.push_back(data);
        graph_->input_states_.push_back(
            spot.slot < 0 ? nullptr
                          : &graph_->domains_[spot.domain]->stores[spot.type]
                                 .states[static_cast<std::size_t>(spot.slot)]);
    }
    graph_->output_ptrs_.reserve(stage_out_slots_.size());
    graph_->out_block_ptrs_.reserve(stage_out_slots_.size());
    graph_->output_states_.reserve(stage_out_slots_.size());
    for (const SlotSpot &spot : stage_out_slots_) {
        std::byte *data =
            graph_->slotData(spot.domain, spot.type, static_cast<std::uint32_t>(spot.slot));
        graph_->output_ptrs_.push_back(audioView(spot, data));
        graph_->out_block_ptrs_.push_back(data);
        graph_->output_states_.push_back(
            &graph_->domains_[spot.domain]->stores[spot.type].states[spot.slot]);
    }

    graph_->mix_sources_.reserve(mix_source_slots_.size());
    graph_->mix_source_states_.reserve(mix_source_slots_.size());
    for (const SlotSpot &spot : mix_source_slots_) {
        std::byte *data = graph_->slotData(
            spot.domain, spot.type, static_cast<std::uint32_t>(spot.slot));
        graph_->mix_sources_.push_back(reinterpret_cast<types::Sample *>(data));
        graph_->mix_source_states_.push_back(
            &graph_->domains_[spot.domain]->stores[spot.type]
                 .states[static_cast<std::size_t>(spot.slot)]);
    }
    for (std::size_t i = 0; i < graph_->mix_bindings_.size(); ++i) {
        const SlotSpot &spot = mix_output_slots_[i];
        graph_->mix_bindings_[i].output = reinterpret_cast<types::Sample *>(
            graph_->slotData(spot.domain, spot.type, static_cast<std::uint32_t>(spot.slot)));
        graph_->mix_bindings_[i].state =
            &graph_->domains_[spot.domain]->stores[spot.type]
                 .states[static_cast<std::size_t>(spot.slot)];
    }

    for (CompiledGraph::IoBinding &binding : graph_->in_bindings_) {
        binding.buffer = reinterpret_cast<types::Sample *>(
            graph_->slotData(0, node::kAudioPortTypeIndex, binding.slot));
        binding.state = &graph_->domains_[0]->stores[node::kAudioPortTypeIndex]
                             .states[binding.slot];
    }
    graph_->output_sources_.reserve(output_source_slots_.size());
    for (const SlotSpot &spot : output_source_slots_) {
        graph_->output_sources_.push_back(reinterpret_cast<types::Sample *>(
            graph_->slotData(spot.domain, spot.type, static_cast<std::uint32_t>(spot.slot))));
    }

    for (std::unique_ptr<Crossing> &crossing : graph_->crossings_) {
        crossing->src = graph_->slotData(crossing->src_domain, crossing->type, crossing->src_slot);
        crossing->dst = graph_->slotData(crossing->dst_domain, crossing->type, crossing->dst_slot);
        crossing->src_state = &graph_->domains_[crossing->src_domain]->stores[crossing->type]
                                   .states[crossing->src_slot];
        crossing->dst_state = &graph_->domains_[crossing->dst_domain]->stores[crossing->type]
                                   .states[crossing->dst_slot];
    }

    // Sized here, once, so neither the latency walk on the control thread nor
    // the profiler on the audio thread ever allocates.
    graph_->latency_scratch_.assign(graph_->order_.size(), 0U);
    graph_->stage_ns_ = std::vector<std::atomic<std::uint64_t>>(graph_->order_.size());
}

std::unique_ptr<CompiledGraph> GraphCompiler::Builder::build(std::string &error)
{
    const bool ok = resolveNodes(error) && resolveEdges(error) && assignDomains(error)
                    && topoSort(error) && instantiate(error);
    if (!ok) {
        return nullptr;
    }
    planCrossings();
    assignSlots();
    resolvePointers();
    return std::move(graph_);
}

bool GraphCompiler::ioRequests(const GraphSpec &spec, std::vector<audio::IoRequest> &out,
                               std::string &error)
{
    const NodeRegistry &registry = NodeRegistry::instance();
    out.clear();

    for (const SpecNode &node : spec.nodes) {
        const NodeDescriptor *desc = registry.find(node.type);
        if (desc == nullptr) {
            error = "node '" + node.id + "' has unknown type '" + node.type + "'";
            return false;
        }
        if (!isIo(desc->kind)) {
            continue;
        }

        audio::IoRequest request;
        request.node = node.id;
        request.kind = ioKindOf(desc->kind);
        request.channels = ioChannels(*desc, node);
        const auto target = node.options.find(ioTargetKey(desc->kind));
        if (target == node.options.end() || target->second.empty()) {
            error = "'" + node.id + "' has nothing selected on it";
            return false;
        }
        request.target = target->second;

        const auto exclusive = node.params.find("exclusive");
        request.exclusive = exclusive != node.params.end() && exclusive->second >= 0.5F;
        out.push_back(std::move(request));
    }
    return true;
}

void GraphCompiler::virtualDevices(const GraphSpec &spec,
                                   std::vector<audio::IoRequest> &out)
{
    const NodeRegistry &registry = NodeRegistry::instance();
    out.clear();

    for (const SpecNode &node : spec.nodes) {
        // Unknown types are skipped rather than refused, because this is what
        // the daemon calls and the daemon does not load extensions: a graph
        // full of plugin nodes still has to yield its published devices. Every
        // other kind of validation is the engine's, which does know them.
        const NodeDescriptor *desc = registry.find(node.type);
        if (desc == nullptr || !isIo(desc->kind)) {
            continue;
        }
        const audio::IoKind kind = ioKindOf(desc->kind);
        if (!audio::isVirtual(kind)) {
            continue;
        }

        const auto target = node.options.find(ioTargetKey(desc->kind));
        if (target == node.options.end() || target->second.empty()) {
            continue;
        }

        audio::IoRequest request;
        request.node = node.id;
        request.kind = kind;
        request.channels = ioChannels(*desc, node);
        request.target = target->second;
        out.push_back(std::move(request));
    }
}

std::unique_ptr<CompiledGraph> GraphCompiler::compile(const GraphSpec &spec, const CompileEnv &env,
                                                      const CompiledGraph *previous,
                                                      std::string &error)
{
    error.clear();
    Builder builder(spec, env, previous);
    return builder.build(error);
}

}
