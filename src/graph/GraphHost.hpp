#pragma once

#include "audio/AudioBackend.hpp"
#include "graph/CompiledGraph.hpp"
#include "graph/GraphCompiler.hpp"
#include "graph/GraphSpec.hpp"
#include "audio/IoBinder.hpp"
#include "rt/RcuSlot.hpp"
#include "rt/SpscRing.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace avc::graph {

struct ParamMessage {
    std::uint64_t generation = 0;
    std::uint32_t node_index = 0;
    std::uint32_t param_index = 0;
    float value = 0.0F;
};

struct MeterReading {
    std::string node;
    float peak = 0.0F;
    float rms = 0.0F;
};

struct ScopeReading {
    std::string node;
    std::vector<float> wave;
    std::vector<float> bands;
};

struct NodeCost {
    std::string node;
    std::uint64_t ns = 0;
    std::uint32_t latency_frames = 0;
};

struct NodeStatusInfo {
    std::string node;
    node::NodeStatus status;
};

struct GraphStats {
    std::uint64_t generation = 0;
    std::uint64_t swaps = 0;
    std::uint64_t dropped_params = 0;
    std::uint64_t silent_cycles = 0;
    std::size_t nodes = 0;
    std::size_t stages = 0;
    std::uint32_t buffer_slots = 0;
    std::size_t pending_retired = 0;

    std::uint32_t latency_frames = 0;

    std::vector<OutputLatency> output_latency;

    bool profiling = false;
    std::vector<NodeCost> node_costs;
    std::vector<NodeStatusInfo> node_status;

    std::vector<DomainInfo> domains;
};

class GraphHost final : public audio::Processor {
public:
    GraphHost(CompileEnv env, audio::IoBinder *binder) : env_(env), binder_(binder) {}
    ~GraphHost() override = default;

    bool apply(const GraphSpec &spec, std::string &error);

    void poll();

    bool setParam(std::string_view node_id, std::string_view param, float value);

    void shutdown();

    GraphSpec spec() const;

    std::vector<MeterReading> meters();

    std::vector<ScopeReading> scopes();

    static std::vector<float> scopeBandsHz();
    GraphStats stats() const;

    void setProfiling(bool on);
    void resetProfile();

    // ZFW: HOT PATH
    void process(const audio::ProcessContext &ctx) noexcept override;

private:
    // ZFW: HOT PATH
    void drainParams(CompiledGraph &graph) noexcept;
    // ZFW: HOT PATH
    static void silence(const audio::ProcessContext &ctx) noexcept;

    bool applyLocked(const GraphSpec &spec, std::string &error);

    void drainRetired();
    void releaseIoFor(const GraphSpec &spec);

    mutable std::mutex control_mutex_;

    rt::RcuSlot<CompiledGraph> slot_;
    rt::SpscRing<ParamMessage, 1024> params_;

    CompileEnv env_;
    audio::IoBinder *binder_ = nullptr;
    GraphSpec spec_;
    CompiledGraph *current_ = nullptr;

    bool profiling_ = false;

    std::atomic<std::uint64_t> swaps_{0};
    std::atomic<std::uint64_t> dropped_params_{0};
    std::atomic<std::uint64_t> silent_cycles_{0};
};

}
