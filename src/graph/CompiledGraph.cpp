#include "graph/CompiledGraph.hpp"

#include "common.hpp"
#include "rt/Clock.hpp"

#include <algorithm>
#include <cstring>

namespace avc::graph {
namespace {

const std::string kEmpty;

}

CompiledGraph::~CompiledGraph()
{
    stopWorkers();
}

node::Node *CompiledGraph::nodeAt(std::size_t index) const noexcept
{
    return index < nodes_.size() ? nodes_[index].get() : nullptr;
}

const std::string &CompiledGraph::nodeIdAt(std::size_t index) const noexcept
{
    return index < node_ids_.size() ? node_ids_[index] : kEmpty;
}

const std::string &CompiledGraph::nodeTypeAt(std::size_t index) const noexcept
{
    return index < node_types_.size() ? node_types_[index] : kEmpty;
}

int CompiledGraph::indexOfNode(std::string_view id) const noexcept
{
    for (std::size_t i = 0; i < node_ids_.size(); ++i) {
        if (node_ids_[i] == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ZFW: HOT PATH
void CompiledGraph::applyParam(std::uint32_t node_index, std::uint32_t param_index,
                               float value) noexcept
{
    if (AVC_UNLIKELY(node_index >= nodes_.size())) {
        return;
    }
    const std::uint32_t domain = node_domains_[node_index];
    if (AVC_LIKELY(domain == 0)) {
        nodes_[node_index]->setParam(param_index, value);
        return;
    }
    // A full queue means hundreds of moves piled up on one knob between two
    // cold passes. Dropping is right: the next one carries the value anyway.
    if (!domains_[domain]->params.push({node_index, param_index, value})) {
        dropped_params_.fetch_add(1, std::memory_order_relaxed);
    }
}

// ZFW: HOT PATH
void CompiledGraph::drainParams(Domain &domain) noexcept
{
    ParamPost post;
    while (domain.params.pop(post)) {
        nodes_[post.node]->setParam(post.param, post.value);
    }
}

std::size_t CompiledGraph::stageNodeIndex(std::size_t index) const noexcept
{
    return index < order_.size() ? order_[index].node_index : 0;
}

std::uint32_t CompiledGraph::stageLatencyFrames(std::size_t index) const noexcept
{
    return index < order_.size() ? order_[index].node->latencyFrames() : 0;
}

std::uint64_t CompiledGraph::stageNs(std::size_t index) const noexcept
{
    return index < stage_ns_.size() ? stage_ns_[index].load(std::memory_order_relaxed) : 0;
}

void CompiledGraph::resetProfile() noexcept
{
    for (std::atomic<std::uint64_t> &slot : stage_ns_) {
        slot.store(0, std::memory_order_relaxed);
    }
    for (std::unique_ptr<Domain> &domain : domains_) {
        domain->ns_max.store(0, std::memory_order_relaxed);
    }
}

void CompiledGraph::startWorkers()
{
    for (std::uint32_t i = 1; i < domains_.size(); ++i) {
        Domain &domain = *domains_[i];
        // A domain wired to nothing can never affect what anyone hears, and
        // with no timeline to pace it would spin a core solid. Leave it parked.
        if (domain.in_crossings.empty() && domain.out_crossings.empty()) {
            continue;
        }
        // A quarter of the block period. Late by a quarter block still lands
        // inside the prefill, and nothing is asking the audio thread to wake
        // anyone: the timeline ring is the only thing between the two threads.
        const std::uint64_t period_us =
            1000000ULL * domain.block / std::max(sample_rate_, 1U);
        domain.worker.start(this, i, static_cast<std::uint32_t>(period_us / 4 + 1));
    }
}

void CompiledGraph::stopWorkers() noexcept
{
    for (std::unique_ptr<Domain> &domain : domains_) {
        domain->worker.stop();
    }
}

std::size_t CompiledGraph::storageBytes() const noexcept
{
    std::size_t bytes = 0;
    for (const std::unique_ptr<Domain> &domain : domains_) {
        for (const TypeStore &store : domain->stores) {
            bytes += store.storage.size() + store.states.size() * sizeof(StreamState);
        }
    }
    for (const std::unique_ptr<Crossing> &crossing : crossings_) {
        bytes += crossing->timeline.storageBytes() + crossing->value.storageBytes();
    }
    return bytes;
}

std::vector<DomainInfo> CompiledGraph::domains() const
{
    std::vector<DomainInfo> out;
    out.reserve(domains_.size());
    for (const std::unique_ptr<Domain> &domain : domains_) {
        DomainInfo info;
        info.name = domain->name;
        info.cold = domain->cold;
        info.block = domain->block;
        info.safety = domain->safety;
        info.latency_frames = domain->cold ? domain->block * (1 + domain->safety) : 0;
        info.stages = domain->exec.size();
        info.passes = domain->passes.load(std::memory_order_relaxed);
        info.ns_avg = domain->ns_avg.load(std::memory_order_relaxed);
        info.ns_max = domain->ns_max.load(std::memory_order_relaxed);
        out.push_back(std::move(info));
    }

    // A crossing always has a cold end -- two hot stages need no ring between
    // them -- so its misses are reported against that end. Preferring the
    // source is what puts "this domain was late" on the domain that was late.
    for (const std::unique_ptr<Crossing> &crossing : crossings_) {
        const std::uint32_t at =
            crossing->src_domain != 0 ? crossing->src_domain : crossing->dst_domain;
        out[at].underruns += crossing->underruns.load(std::memory_order_relaxed);
        out[at].overruns += crossing->overruns.load(std::memory_order_relaxed);
    }
    return out;
}

std::uint32_t CompiledGraph::latencyThrough(const PredRef &pred) const noexcept
{
    return pred.extra
           + (pred.stage >= 0 ? latency_scratch_[static_cast<std::size_t>(pred.stage)] : 0);
}

void CompiledGraph::walkLatencies()
{
    std::fill(latency_scratch_.begin(), latency_scratch_.end(), 0U);

    for (std::size_t s = 0; s < order_.size(); ++s) {
        const Stage &stage = order_[s];
        std::uint32_t upstream = 0;
        for (std::uint32_t k = 0; k < stage.pred_count; ++k) {
            upstream = std::max(upstream, latencyThrough(stage_preds_[stage.pred_offset + k]));
        }
        latency_scratch_[s] = upstream + stage.node->latencyFrames();
    }
}

std::uint32_t CompiledGraph::latencyFrames()
{
    if (order_.empty()) {
        return 0;
    }
    walkLatencies();

    std::uint32_t worst = 0;
    for (const PredRef &pred : output_stages_) {
        worst = std::max(worst, latencyThrough(pred));
    }
    return worst;
}

std::vector<OutputLatency> CompiledGraph::latencyByOutput()
{
    std::vector<OutputLatency> out;
    if (order_.empty() && output_stages_.empty()) {
        return out;
    }
    walkLatencies();

    for (std::size_t i = 0; i < output_stages_.size(); ++i) {
        const std::uint32_t frames = latencyThrough(output_stages_[i]);
        const std::string &node = output_nodes_[i];

        const auto found = std::find_if(out.begin(), out.end(),
                                        [&node](const OutputLatency &e) { return e.node == node; });
        if (found == out.end()) {
            out.push_back({node, frames});
        } else {
            found->frames = std::max(found->frames, frames);
        }
    }
    return out;
}

// ZFW: HOT PATH
void CompiledGraph::copyIn(const audio::ProcessContext &ctx, std::uint32_t nframes) noexcept
{
    const std::size_t bytes = static_cast<std::size_t>(nframes) * sizeof(types::Sample);
    for (const IoBinding &binding : in_bindings_) {
        types::Sample *dst = binding.buffer;
        const types::Sample *src =
            binding.endpoint < ctx.n_inputs ? ctx.inputs[binding.endpoint] : nullptr;
        if (AVC_LIKELY(src != nullptr)) {
            std::memcpy(dst, src, bytes);
        } else {
            std::memset(dst, 0, bytes);
        }
        *binding.state = {};
    }
}

// ZFW: HOT PATH
void CompiledGraph::copyOut(const audio::ProcessContext &ctx, std::uint32_t nframes) noexcept
{
    for (const OutputBinding &binding : out_bindings_) {
        types::Sample *dst =
            binding.endpoint < ctx.n_outputs ? ctx.outputs[binding.endpoint] : nullptr;
        if (AVC_UNLIKELY(dst == nullptr)) {
            continue;
        }

        const types::Sample *first = output_sources_[binding.source_offset];
        const float first_gain = output_gains_[binding.source_offset];
        for (std::uint32_t frame = 0; frame < nframes; ++frame) {
            dst[frame] = first[frame] * first_gain;
        }
        for (std::uint32_t source = 1; source < binding.source_count; ++source) {
            const std::size_t at = binding.source_offset + source;
            const types::Sample *input = output_sources_[at];
            const float gain = output_gains_[at];
            for (std::uint32_t frame = 0; frame < nframes; ++frame) {
                dst[frame] += input[frame] * gain;
            }
        }
    }
    const std::size_t bytes = static_cast<std::size_t>(nframes) * sizeof(types::Sample);
    for (std::uint32_t endpoint : silent_outputs_) {
        types::Sample *dst = endpoint < ctx.n_outputs ? ctx.outputs[endpoint] : nullptr;
        if (dst != nullptr) {
            std::memset(dst, 0, bytes);
        }
    }
}

// ZFW: HOT PATH
void CompiledGraph::readCrossings(const Domain &domain, std::uint32_t nframes,
                                  std::uint64_t start_frame) noexcept
{
    for (std::uint32_t index : domain.in_crossings) {
        Crossing &crossing = *crossings_[index];
        if (AVC_LIKELY(crossing.streaming)) {
            const rt::TimelineRead read =
                crossing.timeline.read(crossing.dst, start_frame, nframes);
            *crossing.dst_state = {read.exact, read.discontinuity};
            if (AVC_UNLIKELY(!read.exact)) {
                crossing.underruns.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        // Nothing published since the last look is not a miss and is not
        // counted as one: the destination keeps the value it already has.
        crossing.value.read(crossing.dst);
        *crossing.dst_state = {};
    }
}

// ZFW: HOT PATH
void CompiledGraph::writeCrossings(const Domain &domain, std::uint32_t nframes,
                                   std::uint64_t start_frame) noexcept
{
    for (std::uint32_t index : domain.out_crossings) {
        Crossing &crossing = *crossings_[index];
        if (AVC_LIKELY(crossing.streaming)) {
            const std::uint64_t output_start = start_frame + crossing.latency;
            const StreamState state = *crossing.src_state;
            if (!state.exact) {
                // A gap advances the producer horizon without pretending that
                // synthetic silence is a valid payload. Every downstream cold
                // domain therefore keeps propagating the missing interval.
                crossing.timeline.publishGap(output_start, nframes);
                continue;
            }
            if (AVC_UNLIKELY(!crossing.timeline.write(
                    crossing.src, output_start, nframes, state.discontinuity))) {
                crossing.overruns.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        crossing.value.write(crossing.src);
    }
}

// ZFW: HOT PATH
void CompiledGraph::mixOne(const MixBinding &mix, std::uint32_t nframes) noexcept
{
    const types::Sample *first = mix_sources_[mix.source_offset];
    const float first_gain = mix_gains_[mix.source_offset];
    for (std::uint32_t frame = 0; frame < nframes; ++frame) {
        mix.output[frame] = first[frame] * first_gain;
    }

    StreamState state = *mix_source_states_[mix.source_offset];
    for (std::uint32_t source = 1; source < mix.source_count; ++source) {
        const std::size_t at = mix.source_offset + source;
        const types::Sample *input = mix_sources_[at];
        const float gain = mix_gains_[at];
        for (std::uint32_t frame = 0; frame < nframes; ++frame) {
            mix.output[frame] += input[frame] * gain;
        }
        const StreamState input_state = *mix_source_states_[at];
        state.exact = state.exact && input_state.exact;
        state.discontinuity = state.discontinuity || input_state.discontinuity;
    }
    *mix.state = state;
}

// ZFW: HOT PATH
void CompiledGraph::runOne(const Stage &stage, std::uint32_t nframes,
                           std::uint64_t start_frame, bool discontinuity) noexcept
{
    for (std::uint32_t i = 0; i < stage.mix_count; ++i) {
        mixOne(mix_bindings_[stage.mix_offset + i], nframes);
    }

    StreamState state{true, discontinuity};
    for (std::uint32_t i = 0; i < stage.n_inputs; ++i) {
        const StreamState *input = input_states_[stage.input_offset + i];
        if (input != nullptr) {
            state.exact = state.exact && input->exact;
            state.discontinuity = state.discontinuity || input->discontinuity;
        }
    }
    const node::NodeContext node_ctx{
        input_ptrs_.data() + stage.input_offset,
        output_ptrs_.data() + stage.output_offset,
        stage.n_inputs,
        stage.n_outputs,
        nframes,
        in_block_ptrs_.data() + stage.input_offset,
        out_block_ptrs_.data() + stage.output_offset,
        start_frame,
        state.discontinuity,
    };
    stage.node->process(node_ctx);
    for (std::uint32_t i = 0; i < stage.n_outputs; ++i) {
        *output_states_[stage.output_offset + i] = state;
    }
}

// ZFW: HOT PATH
void CompiledGraph::runStages(const Domain &domain, std::uint32_t nframes,
                              std::uint64_t start_frame, bool discontinuity) noexcept
{
    for (std::uint32_t index : domain.exec) {
        runOne(order_[index], nframes, start_frame, discontinuity);
    }
}

// ZFW: HOT PATH
void CompiledGraph::runStagesProfiled(const Domain &domain, std::uint32_t nframes,
                                      std::uint64_t start_frame,
                                      bool discontinuity) noexcept
{
    std::uint64_t mark = rt::monoNs();

    for (std::uint32_t index : domain.exec) {
        runOne(order_[index], nframes, start_frame, discontinuity);

        const std::uint64_t now = rt::monoNs();
        // Smoothed over about eight blocks. A single block is mostly cache
        // weather; what the panel should show is what a node usually costs.
        const std::uint64_t previous = stage_ns_[index].load(std::memory_order_relaxed);
        stage_ns_[index].store(previous - (previous >> 3) + ((now - mark) >> 3),
                               std::memory_order_relaxed);
        mark = now;
    }
}

// ZFW: HOT PATH
void CompiledGraph::process(const audio::ProcessContext &ctx) noexcept
{
    if (AVC_UNLIKELY(domains_.empty())) {
        return;
    }
    Domain &hot = *domains_[0];

    // The graph is compiled for a ceiling, not for the current quantum, so a
    // driver that raises the block size mid-run cannot walk off the buffers.
    const std::uint32_t nframes = ctx.nframes < max_quantum_ ? ctx.nframes : max_quantum_;
    const std::uint64_t start_frame = hot_frame_;

    copyIn(ctx, nframes);
    readCrossings(hot, nframes, start_frame);
    if (AVC_UNLIKELY(profiling_.load(std::memory_order_relaxed))) {
        runStagesProfiled(hot, nframes, start_frame, false);
    } else {
        runStages(hot, nframes, start_frame, false);
    }
    writeCrossings(hot, nframes, start_frame);
    copyOut(ctx, nframes);

    // The graph is compiled for max_quantum_. If the driver renegotiates a
    // larger block, never expose stale samples from the tail we cannot process.
    if (AVC_UNLIKELY(nframes < ctx.nframes)) {
        const std::uint32_t missing_frames = ctx.nframes - nframes;
        const std::uint64_t missing_start = start_frame + nframes;
        const std::size_t tail = static_cast<std::size_t>(ctx.nframes - nframes)
                               * sizeof(types::Sample);
        for (std::uint32_t i = 0; i < ctx.n_outputs; ++i) {
            if (ctx.outputs[i] != nullptr) {
                std::memset(ctx.outputs[i] + nframes, 0, tail);
            }
        }
        for (std::uint32_t index : hot.in_crossings) {
            Crossing &crossing = *crossings_[index];
            if (crossing.streaming) {
                crossing.timeline.expire(missing_start, missing_frames);
                crossing.underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
        for (std::uint32_t index : hot.out_crossings) {
            Crossing &crossing = *crossings_[index];
            if (crossing.streaming) {
                crossing.timeline.publishGap(missing_start + crossing.latency,
                                             missing_frames);
                crossing.overruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    hot_frame_ += ctx.nframes;
}

bool CompiledGraph::coldReady(const Domain &domain) const noexcept
{
    bool has_stream_input = false;
    for (std::uint32_t index : domain.in_crossings) {
        const Crossing &crossing = *crossings_[index];
        // A value is never what a pass is waiting for: there is always one, and
        // it is whatever was published last. Only a stream gates a pass.
        if (crossing.streaming
            && !crossing.timeline.canResolve(domain.next_frame, domain.block)) {
            return false;
        }
        has_stream_input = has_stream_input || crossing.streaming;
    }
    // A generator has no input horizon to pace it. Let its output capacity do
    // that job; input-driven domains deliberately never wait for stale output.
    if (!has_stream_input) {
        for (std::uint32_t index : domain.out_crossings) {
            const Crossing &crossing = *crossings_[index];
            if (crossing.streaming
                && !crossing.timeline.canWrite(domain.next_frame + crossing.latency,
                                                domain.block)) {
                return false;
            }
        }
    }
    return true;
}

void CompiledGraph::skipExpired(Domain &domain) noexcept
{
    const std::uint64_t previous = domain.next_frame;
    std::uint64_t fresh = previous;
    for (std::uint32_t index : domain.out_crossings) {
        const Crossing &crossing = *crossings_[index];
        if (!crossing.streaming) continue;
        const std::uint64_t requested = crossing.timeline.requestedUntil();
        if (requested <= crossing.latency) continue;
        const std::uint64_t relative = requested - crossing.latency;
        // Only skip blocks whose entire output interval is expired. A block
        // that overlaps the consumer's future can still satisfy a differently
        // aligned downstream block, so rounding up here creates an avoidable
        // hole when domains use different block sizes.
        const std::uint64_t candidate = (relative / domain.block) * domain.block;
        fresh = std::max(fresh, candidate);
    }
    if (fresh > previous) {
        // Demand propagates upstream across any number of cold domains. Merely
        // waiting in coldReady() would leave the previous domain unaware that
        // all of its old output has already expired.
        for (std::uint32_t index : domain.in_crossings) {
            Crossing &crossing = *crossings_[index];
            if (crossing.streaming) {
                crossing.timeline.expireUntil(fresh);
                crossing.underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
        domain.pending_discontinuity = true;
    }
    domain.next_frame = fresh;
}

bool CompiledGraph::runColdPass(std::uint32_t index) noexcept
{
    if (AVC_UNLIKELY(index == 0 || index >= domains_.size())) {
        return false;
    }
    Domain &domain = *domains_[index];
    const std::uint64_t start = rt::monoNs();

    // A live result whose destination interval was already requested has no
    // value. Jump to the first block that can still overlap the consumer's
    // future instead of spending CPU preserving stale speech.
    skipExpired(domain);

    // A domain with a stream on it is paced by the published timeline. One
    // with nothing but values on it has no such thing to wait for, so its block
    // is the only definition of its rate that is left.
    if (AVC_UNLIKELY(!domain.paced) && start < domain.next_pass_ns) {
        return false;
    }
    if (!coldReady(domain)) {
        return false;
    }
    if (AVC_UNLIKELY(!domain.paced)) {
        domain.next_pass_ns =
            start + 1000000000ULL * domain.block / std::max(sample_rate_, 1U);
    }
    drainParams(domain);
    readCrossings(domain, domain.block, domain.next_frame);
    const bool discontinuity = domain.pending_discontinuity;
    domain.pending_discontinuity = false;
    if (AVC_UNLIKELY(profiling_.load(std::memory_order_relaxed))) {
        runStagesProfiled(domain, domain.block, domain.next_frame, discontinuity);
    } else {
        runStages(domain, domain.block, domain.next_frame, discontinuity);
    }
    // Each node sees only the discontinuities on its own dependency chain, and
    // each crossing publishes the validity of its own producer's buffer.
    writeCrossings(domain, domain.block, domain.next_frame);
    domain.next_frame += domain.block;

    const std::uint64_t took = rt::monoNs() - start;
    const std::uint64_t previous = domain.ns_avg.load(std::memory_order_relaxed);
    domain.ns_avg.store(previous - (previous >> 3) + (took >> 3), std::memory_order_relaxed);
    if (took > domain.ns_max.load(std::memory_order_relaxed)) {
        domain.ns_max.store(took, std::memory_order_relaxed);
    }
    domain.passes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}
