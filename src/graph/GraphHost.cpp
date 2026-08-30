#include "graph/GraphHost.hpp"

#include "common.hpp"
#include "log/Log.hpp"
#include "node/NodeRegistry.hpp"
#include "node/basic/MeterNode.hpp"
#include "node/debug/ScopeNode.hpp"
#include "node/debug/TextNode.hpp"

#include <chrono>
#include <cstring>
#include <set>
#include <thread>
#include <utility>

namespace avc::graph {
namespace {

constexpr std::uint32_t kMaxParamsPerBlock = 256;

}

bool GraphHost::apply(const GraphSpec &spec, std::string &error)
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    return applyLocked(spec, error);
}

void GraphHost::drainRetired()
{
    for (int attempt = 0; attempt < 100 && slot_.pendingRetired() > 0; ++attempt) {
        if (slot_.collect() > 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void GraphHost::releaseIoFor(const GraphSpec &spec)
{
    if (binder_ == nullptr) {
        return;
    }
    std::vector<audio::IoRequest> requests;
    std::string ignored;
    GraphCompiler::ioRequests(spec, requests, ignored);

    std::set<std::string> keep;
    for (const audio::IoRequest &request : requests) {
        keep.insert(request.node);
    }
    binder_->releaseIo(keep);
}

bool GraphHost::applyLocked(const GraphSpec &spec, std::string &error)
{
    std::vector<audio::IoRequest> requests;
    if (!GraphCompiler::ioRequests(spec, requests, error)) {
        return false;
    }

    CompileEnv env = env_;
    env.generation = env_.generation + 1;
    if (binder_ != nullptr && !binder_->bindIo(requests, env.io_slots, error)) {
        releaseIoFor(spec_);
        return false;
    }

    std::unique_ptr<CompiledGraph> compiled =
        GraphCompiler::compile(spec, env, current_, error);
    if (compiled == nullptr) {
        releaseIoFor(spec_);
        return false;
    }

    spdlog::info("graph gen {}: {} nodes, {} stages, {} buffers, {} domains, {} crossings",
                 env.generation, compiled->nodeCount(), compiled->stageCount(),
                 compiled->bufferSlots(), compiled->domainCount(), compiled->crossingCount());

    compiled->setProfiling(profiling_);
    // Cold domains start before the swap so their output timelines are already
    // prefilled and turning over when the audio thread first reads one.
    compiled->startWorkers();
    current_ = compiled.get();
    env_.generation = env.generation;
    spec_ = spec;
    slot_.store(std::move(compiled));
    swaps_.fetch_add(1, std::memory_order_relaxed);

    drainRetired();
    releaseIoFor(spec_);
    return true;
}

void GraphHost::poll()
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    slot_.collect();
}

GraphSpec GraphHost::spec() const
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    return spec_;
}

std::vector<MeterReading> GraphHost::meters()
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    std::vector<MeterReading> out;
    if (current_ == nullptr) {
        return out;
    }
    for (std::size_t i = 0; i < current_->nodeCount(); ++i) {
        auto *meter = dynamic_cast<node::basic::MeterNode *>(current_->nodeAt(i));
        if (meter != nullptr) {
            out.push_back({current_->nodeIdAt(i), meter->takePeak(), meter->rms()});
        }
    }
    return out;
}

std::vector<ScopeReading> GraphHost::scopes()
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    std::vector<ScopeReading> out;
    if (current_ == nullptr) {
        return out;
    }
    node::debug::ScopeFrame frame;
    for (std::size_t i = 0; i < current_->nodeCount(); ++i) {
        auto *scope = dynamic_cast<node::debug::ScopeNode *>(current_->nodeAt(i));
        if (scope != nullptr && scope->takeFrame(frame)) {
            out.push_back({current_->nodeIdAt(i),
                           {frame.wave.begin(), frame.wave.end()},
                           {frame.bands.begin(), frame.bands.end()}});
        }
    }
    return out;
}

std::vector<TextReading> GraphHost::texts()
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    std::vector<TextReading> out;
    if (current_ == nullptr) {
        return out;
    }
    node::debug::TextSnapshot snapshot;
    for (std::size_t i = 0; i < current_->nodeCount(); ++i) {
        auto *renderer = dynamic_cast<node::debug::TextNode *>(current_->nodeAt(i));
        if (renderer != nullptr && renderer->takeText(snapshot)) {
            out.push_back({current_->nodeIdAt(i), std::move(snapshot.text),
                           snapshot.stream, snapshot.segment, snapshot.revision,
                           snapshot.segmented, snapshot.final});
        }
    }
    return out;
}

std::vector<float> GraphHost::scopeBandsHz()
{
    std::vector<float> out;
    out.reserve(node::debug::kScopeBands);
    for (std::uint32_t b = 0; b < node::debug::kScopeBands; ++b) {
        out.push_back(node::debug::ScopeNode::bandCentre(b));
    }
    return out;
}

bool GraphHost::setParam(std::string_view node_id, std::string_view param, float value)
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    if (current_ == nullptr) {
        return false;
    }
    const int node_index = current_->indexOfNode(node_id);
    if (node_index < 0) {
        return false;
    }

    const node::NodeDescriptor *desc =
        node::NodeRegistry::instance().find(current_->nodeTypeAt(
            static_cast<std::size_t>(node_index)));
    const int param_index = desc != nullptr ? desc->indexOfParam(param) : -1;
    if (param_index < 0) {
        return false;
    }

    const ParamMessage message{env_.generation, static_cast<std::uint32_t>(node_index),
                               static_cast<std::uint32_t>(param_index), value};
    if (!params_.push(message)) {
        dropped_params_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void GraphHost::shutdown()
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    if (current_ != nullptr) {
        current_->stopWorkers();
    }
    slot_.clear();
    current_ = nullptr;
}

// ZFW: HOT PATH
void GraphHost::silence(const audio::ProcessContext &ctx) noexcept
{
    for (std::uint32_t i = 0; i < ctx.n_outputs; ++i) {
        if (ctx.outputs[i] != nullptr) {
            std::memset(ctx.outputs[i], 0, ctx.nframes * sizeof(types::Sample));
        }
    }
}

// ZFW: HOT PATH
void GraphHost::drainParams(CompiledGraph &graph) noexcept
{
    ParamMessage message;
    for (std::uint32_t i = 0; i < kMaxParamsPerBlock && params_.pop(message); ++i) {
        if (AVC_LIKELY(message.generation == graph.generation())) {
            graph.applyParam(message.node_index, message.param_index, message.value);
        }
    }
}

// ZFW: HOT PATH
void GraphHost::process(const audio::ProcessContext &ctx) noexcept
{
    auto guard = slot_.enter();
    CompiledGraph *graph = guard.get();

    if (AVC_UNLIKELY(graph == nullptr)) {
        silence(ctx);
        silent_cycles_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    drainParams(*graph);
    graph->process(ctx);
}

void GraphHost::setProfiling(bool on)
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    profiling_ = on;
    if (current_ != nullptr) {
        current_->setProfiling(on);
    }
}

void GraphHost::resetProfile()
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    if (current_ != nullptr) {
        current_->resetProfile();
    }
}

GraphStats GraphHost::stats() const
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    GraphStats out;
    out.generation = env_.generation;
    out.swaps = swaps_.load(std::memory_order_relaxed);
    out.dropped_params = dropped_params_.load(std::memory_order_relaxed)
                         + (current_ != nullptr ? current_->droppedParams() : 0);
    out.silent_cycles = silent_cycles_.load(std::memory_order_relaxed);
    out.pending_retired = slot_.pendingRetired();
    out.profiling = profiling_;
    if (current_ != nullptr) {
        out.nodes = current_->nodeCount();
        out.stages = current_->stageCount();
        out.buffer_slots = current_->bufferSlots();
        out.latency_frames = current_->latencyFrames();
        out.domains = current_->domains();
        out.output_latency = current_->latencyByOutput();

        out.node_costs.reserve(current_->stageCount());
        for (std::size_t s = 0; s < current_->stageCount(); ++s) {
            const std::uint32_t delay = current_->stageLatencyFrames(s);
            const std::uint64_t ns = profiling_ ? current_->stageNs(s) : 0;
            if (delay > 0 || ns > 0) {
                out.node_costs.push_back(
                    {current_->nodeIdAt(current_->stageNodeIndex(s)), ns, delay});
            }
        }
        for (std::size_t i = 0; i < current_->nodeCount(); ++i) {
            node::NodeStatus status;
            node::Node *instance = current_->nodeAt(i);
            if (instance != nullptr && instance->status(status)) {
                out.node_status.push_back({current_->nodeIdAt(i), std::move(status)});
            }
        }
    }
    return out;
}

}
