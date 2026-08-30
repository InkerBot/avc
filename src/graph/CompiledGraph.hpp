#pragma once

#include "audio/AudioBackend.hpp"
#include "graph/ColdWorker.hpp"
#include "graph/Crossing.hpp"
#include "node/Node.hpp"
#include "rt/SpscRing.hpp"
#include "types/Audio.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace avc::graph {

class GraphCompiler;

struct OutputLatency {
    std::string node;
    std::uint32_t frames = 0;
};

struct DomainInfo {
    std::string name;
    bool cold = false;

    std::uint32_t block = 0;
    std::uint32_t safety = 0;

    std::uint32_t latency_frames = 0;

    std::size_t stages = 0;
    std::uint64_t passes = 0;

    std::uint64_t underruns = 0;
    std::uint64_t overruns = 0;

    std::uint64_t ns_avg = 0;
    std::uint64_t ns_max = 0;
};

class CompiledGraph {
public:
    CompiledGraph() = default;
    ~CompiledGraph();

    CompiledGraph(const CompiledGraph &) = delete;
    CompiledGraph &operator=(const CompiledGraph &) = delete;

    // ZFW: HOT PATH
    void process(const audio::ProcessContext &ctx) noexcept;

    // ZFW: HOT PATH
    void applyParam(std::uint32_t node_index, std::uint32_t param_index, float value) noexcept;

    std::uint64_t droppedParams() const noexcept
    {
        return dropped_params_.load(std::memory_order_relaxed);
    }

    bool runColdPass(std::uint32_t domain) noexcept;

    void startWorkers();

    void stopWorkers() noexcept;

    std::uint32_t latencyFrames();

    std::vector<OutputLatency> latencyByOutput();

    void setProfiling(bool on) noexcept { profiling_.store(on, std::memory_order_relaxed); }
    bool profiling() const noexcept { return profiling_.load(std::memory_order_relaxed); }
    void resetProfile() noexcept;

    std::uint64_t stageNs(std::size_t index) const noexcept;
    std::size_t stageNodeIndex(std::size_t index) const noexcept;
    std::uint32_t stageLatencyFrames(std::size_t index) const noexcept;

    std::uint64_t generation() const noexcept { return generation_; }
    std::uint32_t bufferSlots() const noexcept { return slot_count_; }
    std::size_t nodeCount() const noexcept { return nodes_.size(); }
    std::size_t stageCount() const noexcept { return order_.size(); }
    std::size_t domainCount() const noexcept { return domains_.size(); }

    std::size_t storageBytes() const noexcept;
    std::size_t crossingCount() const noexcept { return crossings_.size(); }

    std::vector<DomainInfo> domains() const;

    node::Node *nodeAt(std::size_t index) const noexcept;
    const std::string &nodeIdAt(std::size_t index) const noexcept;
    const std::string &nodeTypeAt(std::size_t index) const noexcept;

    int indexOfNode(std::string_view id) const noexcept;

private:
    friend class GraphCompiler;

    struct Stage {
        node::Node *node = nullptr;
        std::uint32_t input_offset = 0;
        std::uint32_t n_inputs = 0;
        std::uint32_t output_offset = 0;
        std::uint32_t n_outputs = 0;

        std::uint32_t node_index = 0;

        std::uint32_t pred_offset = 0;
        std::uint32_t pred_count = 0;
    };

    struct PredRef {
        std::int32_t stage = -1;
        std::uint32_t extra = 0;
    };

    struct ParamPost {
        std::uint32_t node = 0;
        std::uint32_t param = 0;
        float value = 0.0F;
    };

    struct IoBinding {
        std::uint32_t endpoint = 0;
        std::uint32_t slot = 0;
        types::Sample *buffer = nullptr;
        StreamState *state = nullptr;
    };

    struct TypeStore {
        std::uint32_t stride = 0;
        std::uint32_t slots = 0;
        std::vector<std::byte> storage;
        std::vector<StreamState> states;
    };

    struct Domain {
        std::string name;
        bool cold = false;

        std::uint32_t block = 0;
        std::uint32_t safety = 0;

        std::uint32_t slot_count = 0;

        std::vector<TypeStore> stores;

        bool paced = true;
        std::uint64_t next_pass_ns = 0;

        std::uint64_t next_frame = 0;
        bool pending_discontinuity = false;

        std::vector<std::uint32_t> exec;

        std::vector<std::uint32_t> in_crossings;
        std::vector<std::uint32_t> out_crossings;

        rt::SpscRing<ParamPost, 256> params;

        std::atomic<std::uint64_t> passes{0};
        std::atomic<std::uint64_t> ns_avg{0};
        std::atomic<std::uint64_t> ns_max{0};

        ColdWorker worker;
    };

    // ZFW: HOT PATH
    void runStages(const Domain &domain, std::uint32_t nframes,
                   std::uint64_t start_frame, bool discontinuity) noexcept;
    // ZFW: HOT PATH
    void runStagesProfiled(const Domain &domain, std::uint32_t nframes,
                           std::uint64_t start_frame, bool discontinuity) noexcept;

    // ZFW: HOT PATH
    void runOne(const Stage &stage, std::uint32_t nframes, std::uint64_t start_frame,
                bool discontinuity) noexcept;

    // ZFW: HOT PATH
    void drainParams(Domain &domain) noexcept;

    // ZFW: HOT PATH
    void readCrossings(const Domain &domain, std::uint32_t nframes,
                       std::uint64_t start_frame) noexcept;
    // ZFW: HOT PATH
    void writeCrossings(const Domain &domain, std::uint32_t nframes,
                        std::uint64_t start_frame) noexcept;

    // ZFW: HOT PATH
    void copyIn(const audio::ProcessContext &ctx, std::uint32_t nframes) noexcept;
    // ZFW: HOT PATH
    void copyOut(const audio::ProcessContext &ctx, std::uint32_t nframes) noexcept;

    bool coldReady(const Domain &domain) const noexcept;
    void skipExpired(Domain &domain) noexcept;

    void walkLatencies();
    std::uint32_t latencyThrough(const PredRef &pred) const noexcept;

    std::byte *slotData(std::uint32_t domain, std::uint32_t type, std::uint32_t slot) noexcept
    {
        TypeStore &store = domains_[domain]->stores[type];
        return store.storage.data() + static_cast<std::size_t>(slot) * store.stride;
    }

    std::vector<std::unique_ptr<node::Node>> nodes_;
    std::vector<std::string> node_ids_;
    std::vector<std::string> node_types_;
    std::vector<std::uint32_t> node_domains_;

    std::vector<Stage> order_;
    std::vector<const types::Sample *> input_ptrs_;
    std::vector<types::Sample *> output_ptrs_;
    std::vector<const void *> in_block_ptrs_;
    std::vector<void *> out_block_ptrs_;
    std::vector<const StreamState *> input_states_;
    std::vector<StreamState *> output_states_;

    std::vector<std::unique_ptr<Domain>> domains_;
    std::vector<std::unique_ptr<Crossing>> crossings_;

    std::vector<IoBinding> in_bindings_;
    std::vector<IoBinding> out_bindings_;

    std::vector<PredRef> stage_preds_;
    std::vector<PredRef> output_stages_;
    std::vector<std::string> output_nodes_;

    std::vector<std::uint32_t> latency_scratch_;

    std::vector<std::atomic<std::uint64_t>> stage_ns_;
    std::atomic<bool> profiling_{false};
    std::atomic<std::uint64_t> dropped_params_{0};

    std::vector<std::uint32_t> silent_outputs_;

    std::uint32_t slot_count_ = 0;
    std::uint32_t sample_rate_ = types::kDefaultSampleRate;
    std::uint32_t max_quantum_ = types::kDefaultQuantum;
    std::uint64_t hot_frame_ = 0;
    std::uint64_t generation_ = 0;
};

}
