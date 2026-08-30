#include "audio/PipeWireBackend.hpp"

#include "common.hpp"
#include "log/Log.hpp"
#include "rt/Clock.hpp"
#include "rt/RtThread.hpp"

#include <spa/utils/keys.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <set>

namespace avc::audio {
namespace {

using rt::monoNs;

constexpr const char *kDspFormat = "32 bit float mono audio";

constexpr const char *kGroup = "avc";

constexpr const char *kHardwareIn = "hw-in";
constexpr const char *kHardwareOut = "hw-out";

}

PipeWireBackend::~PipeWireBackend()
{
    close();
}

bool PipeWireBackend::open()
{
    if (!session_.open("avc-pw")) {
        return false;
    }
    LoopGuard guard(session_.loop());
    session_.registry().setLinkObserver([this](const PwLink &link) { onLinkAppeared(link); });
    return true;
}

void PipeWireBackend::close() noexcept
{
    stop();
    session_.close();
}

std::vector<DeviceInfo> PipeWireBackend::enumerateDevices()
{
    return session_.enumerateDevices();
}

std::string PipeWireBackend::defaultDeviceName(Direction direction)
{
    return session_.defaultDeviceName(direction);
}

bool PipeWireBackend::createEngine(const types::AudioFormat &format)
{
    static const pw_filter_events filter_events = {
        .version = PW_VERSION_FILTER_EVENTS,
        .state_changed = &PipeWireBackend::onFilterState,
        .process = &PipeWireBackend::onFilterProcess,
    };

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_MEDIA_CLASS, "Audio/Filter",
        PW_KEY_APP_NAME, "avc",
        PW_KEY_NODE_NAME, engine_name_.c_str(),
        PW_KEY_NODE_DESCRIPTION, "AVC Voice Engine",
        // Same group as the devices the daemon publishes: they are wired to
        // each other, so the session manager keeps them on one driver at one
        // rate and quantum.
        PW_KEY_NODE_GROUP, kGroup,
        PW_KEY_NODE_LINK_GROUP, kGroup,
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        PW_KEY_NODE_WANT_DRIVER, "true",
        nullptr);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", format.quantum, format.sample_rate);
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", format.sample_rate);
    if (force_quantum_) {
        pw_properties_setf(props, PW_KEY_NODE_FORCE_QUANTUM, "%u", format.quantum);
        pw_properties_set(props, PW_KEY_NODE_LOCK_QUANTUM, "true");
    }

    engine_ = std::make_unique<FilterState>();
    engine_->self = this;
    engine_->filter = pw_filter_new(session_.core(), engine_name_.c_str(), props);
    if (engine_->filter == nullptr) {
        spdlog::error("pw_filter_new failed");
        return false;
    }

    spa_zero(engine_->listener);
    pw_filter_add_listener(engine_->filter, &engine_->listener, &filter_events, engine_.get());
    engine_->listening = true;
    return true;
}

std::vector<PipeWireBackend::PortEntry> &PipeWireBackend::tableFor(IoKind kind) noexcept
{
    return producesSignal(kind) ? input_slots_ : output_slots_;
}

Direction PipeWireBackend::directionFor(IoKind kind) noexcept
{
    return producesSignal(kind) ? Direction::Input : Direction::Output;
}

std::uint32_t PipeWireBackend::findSlot(const std::vector<PortEntry> &table,
                                        const std::string &owner,
                                        std::uint32_t channel) const noexcept
{
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        if (table[i].data != nullptr && table[i].owner == owner && table[i].channel == channel) {
            return i;
        }
    }
    return kNoSlot;
}

std::uint32_t PipeWireBackend::openPort(std::vector<PortEntry> &table, const IoRequest &request,
                                        std::uint32_t channel, Direction direction)
{
    PortEntry entry;
    entry.owner = request.node;
    entry.target = request.target;
    entry.channel = channel;
    entry.name = request.node + "_" + std::to_string(channel);

    entry.data = pw_filter_add_port(
        engine_->filter,
        direction == Direction::Input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(std::uint32_t),
        pw_properties_new(PW_KEY_FORMAT_DSP, kDspFormat, PW_KEY_PORT_NAME, entry.name.c_str(),
                          PW_KEY_AUDIO_CHANNEL, channelName(channel, request.channels).c_str(),
                          nullptr),
        nullptr, 0);
    if (entry.data == nullptr) {
        spdlog::error("pw_filter_add_port failed for '{}'", entry.name);
        return kNoSlot;
    }

    // A freed slot is reused before the table grows, so slot numbers stay small
    // and, more importantly, never shift under a graph that is still running.
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        if (table[i].data == nullptr) {
            table[i] = std::move(entry);
            return i;
        }
    }
    table.push_back(std::move(entry));
    return static_cast<std::uint32_t>(table.size() - 1);
}

void PipeWireBackend::publishTable()
{
    auto table = std::make_unique<PortTable>();
    for (const PortEntry &entry : input_slots_) {
        table->inputs.push_back(entry.data);
    }
    for (const PortEntry &entry : output_slots_) {
        table->outputs.push_back(entry.data);
    }
    table_.store(std::move(table));

    for (int attempt = 0; attempt < 100 && table_.pendingRetired() > 0; ++attempt) {
        if (table_.collect() > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool PipeWireBackend::portsPresent(const std::vector<IoRequest> &requests) const
{
    for (const IoRequest &request : requests) {
        const std::vector<PortEntry> &table =
            producesSignal(request.kind) ? input_slots_ : output_slots_;
        const std::vector<PwPort> ours =
            session_.registry().portsOf(engine_->node_id, directionFor(request.kind));

        for (std::uint32_t c = 0; c < request.channels; ++c) {
            const std::uint32_t slot = findSlot(table, request.node, c);
            if (slot == kNoSlot) {
                return false;
            }
            bool seen = false;
            for (const PwPort &port : ours) {
                seen = seen || port.name == table[slot].name;
            }
            if (!seen) {
                return false;
            }
        }
        if (isVirtual(request.kind)) {
            const PwNode *peer = session_.registry().findNode(request.target);
            const Direction needed =
                producesSignal(request.kind) ? Direction::Output : Direction::Input;
            if (peer == nullptr
                || session_.registry().countPorts(peer->id, needed) < request.channels) {
                return false;
            }
        }
    }
    return true;
}

bool PipeWireBackend::wireRequest(const IoRequest &request,
                                  const std::vector<std::uint32_t> &slots)
{
    const PwNode *peer = session_.registry().findNode(request.target);
    if (peer == nullptr) {
        spdlog::warn("'{}' is not there, leaving '{}' unconnected", request.target, request.node);
        return false;
    }

    const bool produces = producesSignal(request.kind);
    // Named locals: portsOf() returns by value, and a pointer into the
    // temporary a range-for iterates over dangles the moment the loop ends.
    const std::vector<PwPort> our_ports =
        session_.registry().portsOf(engine_->node_id, directionFor(request.kind));
    const std::vector<PwPort> peer_ports =
        session_.registry().portsOf(peer->id, produces ? Direction::Output : Direction::Input);
    if (peer_ports.empty()) {
        spdlog::warn("'{}' has no ports to wire '{}' to", request.target, request.node);
        return false;
    }

    const std::vector<PortEntry> &table = produces ? input_slots_ : output_slots_;
    bool all = true;
    for (std::uint32_t c = 0; c < slots.size(); ++c) {
        const PwPort *ours = nullptr;
        for (const PwPort &port : our_ports) {
            if (port.name == table[slots[c]].name) {
                ours = &port;
            }
        }
        if (ours == nullptr) {
            all = false;
            continue;
        }
        const PwPort &theirs = peer_ports[c % peer_ports.size()];
        all = (produces ? createLink(theirs.node_id, theirs.id, engine_->node_id, ours->id,
                                     request.node)
                        : createLink(engine_->node_id, ours->id, theirs.node_id, theirs.id,
                                     request.node))
              && all;
    }
    return all;
}

void PipeWireBackend::onLinkAppeared(const PwLink &link)
{
    if (exclusive_targets_.empty() || engine_ == nullptr
        || link.output_node == engine_->node_id) {
        return;
    }
    const PwNode *sink = session_.registry().findNodeById(link.input_node);
    if (sink == nullptr || exclusive_targets_.count(sink->name) == 0) {
        return;
    }

    const PwNode *source = session_.registry().findNodeById(link.output_node);
    spdlog::info("'{}' is exclusive to this graph: disconnected '{}'", sink->name,
                 source != nullptr ? source->name : "something");
    session_.registry().destroyGlobal(link.id);
}

void PipeWireBackend::enforceExclusive()
{
    if (exclusive_targets_.empty()) {
        return;
    }
    for (const PwLink &link : session_.registry().links()) {
        onLinkAppeared(link);
    }
}

bool PipeWireBackend::bindIo(const std::vector<IoRequest> &given,
                             std::map<std::string, std::vector<std::uint32_t>> &slots,
                             std::string &error)
{
    if (!started_) {
        error = "the engine is not running";
        return false;
    }

    const std::lock_guard<std::mutex> lock(control_mutex_);
    LoopGuard guard(session_.loop());
    slots.clear();

    // "@default_source" / "@default_sink" follow whatever the desktop's sound
    // settings point at, so a graph can be written once and still land on the
    // right device on another machine.
    std::vector<IoRequest> requests = given;
    for (IoRequest &request : requests) {
        if (isVirtual(request.kind) || request.target.empty() || request.target[0] != '@') {
            continue;
        }
        const std::string resolved = request.target == "@default_source"
                                         ? session_.registry().defaultSource()
                                         : session_.registry().defaultSink();
        if (resolved.empty()) {
            error = "'" + request.node + "' follows " + request.target
                    + ", and the audio system has not named one";
            return false;
        }
        request.target = resolved;
    }

    // Virtual devices are the daemon's, not ours: it publishes them before it
    // hands us the graph, and we only find them by name and link to them. That
    // is what lets this process be restarted without the applications that
    // selected one noticing.
    std::set<std::string> published;
    for (const IoRequest &request : requests) {
        if (request.channels < 1 || request.channels > 32) {
            error = "'" + request.node + "' asks for " + std::to_string(request.channels)
                    + " channels";
            return false;
        }
        if (isVirtual(request.kind) && !published.insert(request.target).second) {
            error = "two nodes both publish '" + request.target + "'";
            return false;
        }
    }
    for (const IoRequest &request : requests) {
        if (!isVirtual(request.kind)
            && (published.count(request.target) > 0 || request.target == engine_name_)) {
            error = "'" + request.node + "' points at '" + request.target
                    + "', which is this engine; that would feed it its own output";
            return false;
        }
    }

    // Only nodes whose wiring actually moved get relinked. Re-creating a link
    // that already exists is refused by the server, and the noise would hide
    // the failures that matter.
    std::set<std::string> to_wire;

    for (const IoRequest &request : requests) {
        std::vector<PortEntry> &table = tableFor(request.kind);
        std::vector<std::uint32_t> assigned;
        bool rewire = false;

        for (std::uint32_t c = 0; c < request.channels; ++c) {
            std::uint32_t slot = findSlot(table, request.node, c);
            if (slot == kNoSlot) {
                if (input_slots_.size() + output_slots_.size() >= kMaxSlots) {
                    error = "that would take the engine past " + std::to_string(kMaxSlots)
                            + " ports";
                    return false;
                }
                slot = openPort(table, request, c, directionFor(request.kind));
                rewire = true;
            }
            if (slot == kNoSlot) {
                error = "the audio server refused a port for '" + request.node + "'";
                return false;
            }
            // A node that keeps its identity but points somewhere else is a
            // relink, not a rebuild: the port stays, only the wire moves.
            rewire = rewire || table[slot].target != request.target;
            table[slot].target = request.target;
            assigned.push_back(slot);
        }

        for (PortEntry &entry : table) {
            if (entry.data != nullptr && entry.owner == request.node
                && entry.channel >= request.channels) {
                rewire = true;
            }
        }
        slots[request.node] = std::move(assigned);
        if (rewire) {
            destroyLinks(request.node);
            to_wire.insert(request.node);
        }
    }

    exclusive_targets_.clear();
    for (const IoRequest &request : requests) {
        if (request.exclusive && request.kind == IoKind::Playback) {
            exclusive_targets_.insert(request.target);
        }
    }

    publishTable();
    if (!session_.waitUntil([&] { return portsPresent(requests); }, "the graph's ports")) {
        error = "the graph's ports did not all turn up -- either the audio server "
                "did not finish opening ours, or a device the daemon publishes is "
                "not there";
        return false;
    }
    for (const IoRequest &request : requests) {
        if (to_wire.count(request.node) > 0) {
            wireRequest(request, slots[request.node]);
        }
    }
    session_.roundtrip();
    enforceExclusive();
    session_.roundtrip();
    return true;
}

void PipeWireBackend::releaseIo(const std::set<std::string> &keep)
{
    const std::lock_guard<std::mutex> lock(control_mutex_);
    LoopGuard guard(session_.loop());

    std::vector<void *> doomed;
    for (std::vector<PortEntry> *table : {&input_slots_, &output_slots_}) {
        for (PortEntry &entry : *table) {
            if (entry.data == nullptr || keep.count(entry.owner) > 0) {
                continue;
            }
            destroyLinks(entry.owner);
            doomed.push_back(entry.data);
            entry = PortEntry{};
        }
    }
    while (!input_slots_.empty() && input_slots_.back().data == nullptr) {
        input_slots_.pop_back();
    }
    while (!output_slots_.empty() && output_slots_.back().data == nullptr) {
        output_slots_.pop_back();
    }

    if (doomed.empty()) {
        return;
    }
    publishTable();
    for (void *port : doomed) {
        pw_filter_remove_port(port);
    }
    session_.roundtrip();
}

bool PipeWireBackend::start(const types::AudioFormat &format, Processor *processor)
{
    if (started_ || processor == nullptr || !session_.valid()) {
        return false;
    }

    format_ = format;
    processor_ = processor;

    LoopGuard guard(session_.loop());
    if (!createEngine(format)) {
        return false;
    }
    publishTable();
    if (pw_filter_connect(engine_->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        spdlog::error("pw_filter_connect failed");
        return false;
    }

    started_ = true;
    return session_.waitUntil(
        [this] {
            engine_->node_id = pw_filter_get_node_id(engine_->filter);
            return engine_->node_id != PW_ID_ANY && engine_->node_id != 0;
        },
        "the engine node");
}

std::uint32_t PipeWireBackend::engineNodeId() const noexcept
{
    return engine_ != nullptr ? engine_->node_id : 0;
}

void PipeWireBackend::stop() noexcept
{
    if (!started_) {
        return;
    }
    started_ = false;

    LoopGuard guard(session_.loop());
    destroyLinks();

    if (engine_ != nullptr) {
        if (engine_->listening) {
            spa_hook_remove(&engine_->listener);
        }
        if (engine_->filter != nullptr) {
            pw_filter_destroy(engine_->filter);
        }
        engine_.reset();
    }
    processor_ = nullptr;
    table_.clear();
    input_slots_.clear();
    output_slots_.clear();
    exclusive_targets_.clear();
}

bool PipeWireBackend::createLink(std::uint32_t out_node, std::uint32_t out_port,
                                 std::uint32_t in_node, std::uint32_t in_port, std::string owner)
{
    pw_properties *props = pw_properties_new(PW_KEY_OBJECT_LINGER, "false", nullptr);
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", out_node);
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out_port);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", in_node);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", in_port);

    auto *proxy = static_cast<pw_proxy *>(pw_core_create_object(
        session_.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
    pw_properties_free(props);

    if (proxy == nullptr) {
        spdlog::error("link-factory failed for {}:{} -> {}:{}", out_node, out_port, in_node,
                      in_port);
        return false;
    }
    links_.push_back({proxy, std::move(owner)});
    return true;
}

void PipeWireBackend::destroyLinks(const std::string &owner) noexcept
{
    auto it = links_.begin();
    while (it != links_.end()) {
        if (it->owner != owner) {
            ++it;
            continue;
        }
        pw_proxy_destroy(it->proxy);
        it = links_.erase(it);
    }
}

void PipeWireBackend::destroyLinks() noexcept
{
    for (const OwnedLink &link : links_) {
        pw_proxy_destroy(link.proxy);
    }
    links_.clear();
}

void PipeWireBackend::onFilterState(void * , pw_filter_state old_state,
                                    pw_filter_state state, const char *error)
{
    spdlog::debug("engine state {} -> {}{}{}", pw_filter_state_as_string(old_state),
                  pw_filter_state_as_string(state), error != nullptr ? ": " : "",
                  error != nullptr ? error : "");
}

void PipeWireBackend::onFilterProcess(void *data, spa_io_position *position)
{
    static_cast<FilterState *>(data)->self->processEngine(position);
}

void PipeWireBackend::firstCycle() noexcept
{
    rt::enableFlushToZero();
    const rt::SchedInfo info = rt::currentSchedInfo();
    sched_policy_.store(info.policy, std::memory_order_relaxed);
    sched_priority_.store(info.priority, std::memory_order_relaxed);
    audio_tid_.store(info.tid, std::memory_order_relaxed);
}

// ZFW: HOT PATH
void PipeWireBackend::processEngine(spa_io_position *position) noexcept
{
    const std::uint64_t cycle = cycles_.load(std::memory_order_relaxed);
    if (AVC_UNLIKELY(cycle == 0)) {
        firstCycle();
    }

    const std::uint32_t nframes = position->clock.duration;
    const std::uint32_t rate = position->clock.rate.denom;
    actual_quantum_.store(nframes, std::memory_order_relaxed);
    actual_rate_.store(rate, std::memory_order_relaxed);

    const std::uint64_t started = monoNs();

    if (AVC_LIKELY(last_wakeup_ns_ != 0 && rate > 0)) {
        const auto nominal =
            static_cast<std::int64_t>(1000000000ULL * nframes / rate);
        const auto actual = static_cast<std::int64_t>(started - last_wakeup_ns_);
        const auto deviation = static_cast<std::uint64_t>(std::abs(actual - nominal));
        if (AVC_UNLIKELY(deviation > wakeup_jitter_max_.load(std::memory_order_relaxed))) {
            wakeup_jitter_max_.store(deviation, std::memory_order_relaxed);
        }
    }
    last_wakeup_ns_ = started;

    auto guard = table_.enter();
    const PortTable *table = guard.get();
    if (AVC_UNLIKELY(table == nullptr)) {
        return;
    }

    // A free slot holds a null handle, and so does a port nobody is connected
    // to. Both reach the processor as a null buffer, which it must tolerate.
    for (std::size_t k = 0; k < table->inputs.size(); ++k) {
        in_bufs_[k] = table->inputs[k] == nullptr
                          ? nullptr
                          : static_cast<types::Sample *>(
                                pw_filter_get_dsp_buffer(table->inputs[k], nframes));
    }
    for (std::size_t k = 0; k < table->outputs.size(); ++k) {
        out_bufs_[k] = table->outputs[k] == nullptr
                           ? nullptr
                           : static_cast<types::Sample *>(
                                 pw_filter_get_dsp_buffer(table->outputs[k], nframes));
    }

    // An output port no graph node writes to would otherwise replay whatever
    // the buffer last held. Clearing up front makes an unclaimed port silent by
    // construction instead of by every processor remembering to do it.
    for (std::size_t k = 0; k < table->outputs.size(); ++k) {
        if (out_bufs_[k] != nullptr) {
            std::memset(out_bufs_[k], 0, nframes * sizeof(types::Sample));
        }
    }

    const ProcessContext ctx{
        in_bufs_.data(), out_bufs_.data(),
        static_cast<std::uint32_t>(table->inputs.size()),
        static_cast<std::uint32_t>(table->outputs.size()),
        nframes, cycle,
    };
    processor_->process(ctx);

    const std::uint64_t elapsed = monoNs() - started;
    process_ns_last_.store(elapsed, std::memory_order_relaxed);
    if (AVC_UNLIKELY(elapsed > process_ns_max_.load(std::memory_order_relaxed))) {
        process_ns_max_.store(elapsed, std::memory_order_relaxed);
    }
    const std::uint64_t average = process_ns_avg_.load(std::memory_order_relaxed);
    process_ns_avg_.store(average - (average >> 4) + (elapsed >> 4), std::memory_order_relaxed);

    const bool recovering = (position->clock.flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER) != 0;
    if (AVC_UNLIKELY(recovering && !in_xrun_)) {
        xruns_.fetch_add(1, std::memory_order_relaxed);
    }
    in_xrun_ = recovering;

    cycles_.store(cycle + 1, std::memory_order_relaxed);
}

BackendStats PipeWireBackend::stats() const noexcept
{
    BackendStats out;
    out.cycles = cycles_.load(std::memory_order_relaxed);
    out.xruns = xruns_.load(std::memory_order_relaxed);
    out.process_ns_last = process_ns_last_.load(std::memory_order_relaxed);
    out.process_ns_max = process_ns_max_.load(std::memory_order_relaxed);
    out.process_ns_avg = process_ns_avg_.load(std::memory_order_relaxed);
    out.wakeup_jitter_ns_max = wakeup_jitter_max_.load(std::memory_order_relaxed);
    out.quantum = format_.quantum;
    out.sample_rate = format_.sample_rate;
    out.actual_quantum = actual_quantum_.load(std::memory_order_relaxed);
    out.actual_rate = actual_rate_.load(std::memory_order_relaxed);
    out.sched_policy = sched_policy_.load(std::memory_order_relaxed);
    out.sched_priority = sched_priority_.load(std::memory_order_relaxed);
    out.audio_tid = audio_tid_.load(std::memory_order_relaxed);

    return out;
}

void PipeWireBackend::resetPeaks() noexcept
{
    process_ns_max_.store(0, std::memory_order_relaxed);
    wakeup_jitter_max_.store(0, std::memory_order_relaxed);
}

}
