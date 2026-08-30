#include "audio/WasapiBackend.hpp"

#include "audio/WasapiSession.hpp"
#include "common.hpp"
#include "log/Log.hpp"
#include "rt/RtThread.hpp"
#include "types/Audio.hpp"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <avrt.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

namespace avc::audio {
namespace {

constexpr std::size_t kMaxSlots = 64;
constexpr int kUnusedChannel = -1;

std::string idString(const ma_device_id &id)
{
    static constexpr char hex[] = "0123456789abcdef";
    const auto *bytes = reinterpret_cast<const unsigned char *>(&id);
    std::string out = "wasapi:";
    out.reserve(7 + sizeof(id) * 2);
    for (std::size_t i = 0; i < sizeof(id); ++i) {
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 15]);
    }
    return out;
}

std::uint64_t nowNs() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void raiseAtomicMax(std::atomic<std::uint64_t> &value, std::uint64_t candidate) noexcept
{
    std::uint64_t old = value.load(std::memory_order_relaxed);
    while (old < candidate
           && !value.compare_exchange_weak(old, candidate, std::memory_order_relaxed)) {
    }
}

}

struct WasapiBackend::Impl {
    struct Binding {
        std::string owner;
        std::string target;
        IoKind kind = IoKind::Capture;
        std::vector<std::uint32_t> slots;
    };

    ma_context context{};
    ma_device device{};
    bool context_open = false;
    bool device_open = false;
    bool device_started = false;
    bool force_quantum = false;
    std::string node_name = "avc";
    Processor *processor = nullptr;
    types::AudioFormat format{};
    std::mutex control_mutex;
    std::vector<Binding> bindings;
    std::array<bool, kMaxSlots> input_used{};
    std::array<bool, kMaxSlots> output_used{};
    std::array<std::atomic<int>, kMaxSlots> input_channel{};
    std::array<std::atomic<int>, kMaxSlots> output_channel{};
    std::atomic<std::uint32_t> input_high{0};
    std::atomic<std::uint32_t> output_high{0};
    std::uint32_t capture_channels = 0;
    std::uint32_t playback_channels = 0;
    std::string capture_target;
    std::string playback_target;

    std::array<std::array<types::Sample, types::kMaxQuantum>, kMaxSlots> input_planes{};
    std::array<std::array<types::Sample, types::kMaxQuantum>, kMaxSlots> output_planes{};
    std::array<types::Sample *, kMaxSlots> input_ptrs{};
    std::array<types::Sample *, kMaxSlots> output_ptrs{};

    std::atomic<std::uint64_t> cycles{0};
    std::atomic<std::uint64_t> xruns{0};
    std::atomic<std::uint64_t> process_last{0};
    std::atomic<std::uint64_t> process_max{0};
    std::atomic<std::uint64_t> process_avg{0};
    std::atomic<std::uint64_t> jitter_max{0};
    std::atomic<std::uint32_t> actual_quantum{0};
    std::atomic<std::uint32_t> actual_rate{0};
    std::atomic<int> sched_policy{-1};
    std::atomic<int> sched_priority{0};
    std::atomic<std::int32_t> audio_tid{0};
    std::uint64_t last_wakeup = 0;
    bool first_cycle = true;

    Impl()
    {
        for (auto &entry : input_channel) entry.store(kUnusedChannel);
        for (auto &entry : output_channel) entry.store(kUnusedChannel);
    }

    static void dataCallback(ma_device *device, void *output, const void *input,
                             ma_uint32 frame_count)
    {
        static_cast<Impl *>(device->pUserData)->process(output, input, frame_count);
    }

    std::optional<ma_device_id> resolveId(const std::string &target, ma_device_type type)
    {
        if (target.empty() || target == "@default_source" || target == "@default_sink") {
            return std::nullopt;
        }
        ma_device_info *playback = nullptr;
        ma_device_info *capture = nullptr;
        ma_uint32 playback_count = 0;
        ma_uint32 capture_count = 0;
        if (ma_context_get_devices(&context, &playback, &playback_count, &capture,
                                   &capture_count) != MA_SUCCESS) {
            return std::nullopt;
        }
        ma_device_info *list = type == ma_device_type_capture ? capture : playback;
        const ma_uint32 count = type == ma_device_type_capture ? capture_count : playback_count;
        for (ma_uint32 i = 0; i < count; ++i) {
            if (idString(list[i].id) == target) return list[i].id;
        }
        return std::nullopt;
    }

    bool targetExists(const std::string &target, ma_device_type type)
    {
        return target.empty() || target == "@default_source" || target == "@default_sink"
               || resolveId(target, type).has_value();
    }

    void uninitDevice() noexcept
    {
        if (!device_open) return;
        if (device_started) ma_device_stop(&device);
        ma_device_uninit(&device);
        device_started = false;
        device_open = false;
    }

    bool initDevice(std::uint32_t capture_count, std::uint32_t playback_count,
                    const std::string &capture_name, const std::string &playback_name,
                    std::string &error)
    {
        uninitDevice();

        // A render callback supplies the clock while no graph is installed.
        if (capture_count == 0 && playback_count == 0) playback_count = 2;
        const ma_device_type type = capture_count > 0 && playback_count > 0
                                        ? ma_device_type_duplex
                                    : capture_count > 0 ? ma_device_type_capture
                                                        : ma_device_type_playback;
        ma_device_config config = ma_device_config_init(type);
        config.sampleRate = format.sample_rate;
        config.periodSizeInFrames = format.quantum;
        config.periods = force_quantum ? 2 : 0;
        config.performanceProfile = ma_performance_profile_low_latency;
        config.dataCallback = &Impl::dataCallback;
        config.pUserData = this;

        const auto capture_id = resolveId(capture_name, ma_device_type_capture);
        const auto playback_id = resolveId(playback_name, ma_device_type_playback);
        if (capture_count > 0) {
            config.capture.format = ma_format_f32;
            config.capture.channels = capture_count;
            config.capture.shareMode = ma_share_mode_shared;
            if (capture_id) config.capture.pDeviceID = &*capture_id;
        }
        if (playback_count > 0) {
            config.playback.format = ma_format_f32;
            config.playback.channels = playback_count;
            config.playback.shareMode = ma_share_mode_shared;
            if (playback_id) config.playback.pDeviceID = &*playback_id;
        }

        ma_result result = ma_device_init(&context, &config, &device);
        if (result != MA_SUCCESS) {
            error = std::string("cannot open the selected WASAPI endpoint: ")
                    + ma_result_description(result);
            return false;
        }
        device_open = true;
        capture_channels = capture_count;
        playback_channels = playback_count;
        capture_target = capture_name;
        playback_target = playback_name;
        actual_rate.store(device.sampleRate, std::memory_order_relaxed);

        result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            error = std::string("cannot start the WASAPI stream: ")
                    + ma_result_description(result);
            uninitDevice();
            return false;
        }
        device_started = true;
        return true;
    }

    std::uint32_t allocateSlot(IoKind kind)
    {
        auto &used = producesSignal(kind) ? input_used : output_used;
        for (std::uint32_t i = 0; i < used.size(); ++i) {
            if (!used[i]) {
                used[i] = true;
                auto &high = producesSignal(kind) ? input_high : output_high;
                high.store(std::max(high.load(std::memory_order_relaxed), i + 1),
                           std::memory_order_release);
                return i;
            }
        }
        return std::numeric_limits<std::uint32_t>::max();
    }

    void process(void *raw_output, const void *raw_input, std::uint32_t frame_count) noexcept
    {
        if (first_cycle) {
            first_cycle = false;
            DWORD task_index = 0;
            (void)AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
            (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            const rt::SchedInfo info = rt::currentSchedInfo();
            sched_policy.store(info.policy, std::memory_order_relaxed);
            sched_priority.store(info.priority, std::memory_order_relaxed);
            audio_tid.store(info.tid, std::memory_order_relaxed);
            rt::enableFlushToZero();
        }

        const std::uint64_t wake = nowNs();
        const std::uint64_t expected = format.sample_rate > 0
                                           ? 1000000000ULL * frame_count / format.sample_rate
                                           : 0;
        if (last_wakeup > 0 && wake > last_wakeup + expected) {
            const std::uint64_t late = wake - last_wakeup - expected;
            raiseAtomicMax(jitter_max, late);
            if (expected > 0 && late > expected / 2) xruns.fetch_add(1);
        }
        last_wakeup = wake;
        actual_quantum.store(frame_count, std::memory_order_relaxed);

        auto *interleaved_out = static_cast<float *>(raw_output);
        const auto *interleaved_in = static_cast<const float *>(raw_input);
        if (interleaved_out != nullptr) {
            std::fill_n(interleaved_out, static_cast<std::size_t>(frame_count) * playback_channels,
                        0.0F);
        }

        std::uint32_t offset = 0;
        while (offset < frame_count) {
            const std::uint32_t count =
                std::min<std::uint32_t>(frame_count - offset, types::kMaxQuantum);
            const std::uint32_t in_count = input_high.load(std::memory_order_acquire);
            const std::uint32_t out_count = output_high.load(std::memory_order_acquire);

            for (std::uint32_t slot = 0; slot < in_count; ++slot) {
                const int channel = input_channel[slot].load(std::memory_order_relaxed);
                auto &plane = input_planes[slot];
                if (channel >= 0 && interleaved_in != nullptr
                    && static_cast<std::uint32_t>(channel) < capture_channels) {
                    for (std::uint32_t f = 0; f < count; ++f) {
                        plane[f] = interleaved_in[(offset + f) * capture_channels + channel];
                    }
                    input_ptrs[slot] = plane.data();
                } else {
                    input_ptrs[slot] = nullptr;
                }
            }
            for (std::uint32_t slot = 0; slot < out_count; ++slot) {
                const int channel = output_channel[slot].load(std::memory_order_relaxed);
                if (channel >= 0) {
                    std::fill_n(output_planes[slot].data(), count, 0.0F);
                    output_ptrs[slot] = output_planes[slot].data();
                } else {
                    output_ptrs[slot] = nullptr;
                }
            }

            const std::uint64_t begin = nowNs();
            if (processor != nullptr) {
                processor->process({input_ptrs.data(), output_ptrs.data(), in_count, out_count,
                                    count, cycles.load(std::memory_order_relaxed)});
            }
            const std::uint64_t elapsed = nowNs() - begin;
            process_last.store(elapsed, std::memory_order_relaxed);
            raiseAtomicMax(process_max, elapsed);
            const std::uint64_t old_avg = process_avg.load(std::memory_order_relaxed);
            process_avg.store(old_avg == 0 ? elapsed : (old_avg * 31 + elapsed) / 32,
                              std::memory_order_relaxed);

            if (interleaved_out != nullptr) {
                for (std::uint32_t slot = 0; slot < out_count; ++slot) {
                    const int channel = output_channel[slot].load(std::memory_order_relaxed);
                    if (channel < 0 || static_cast<std::uint32_t>(channel) >= playback_channels)
                        continue;
                    for (std::uint32_t f = 0; f < count; ++f) {
                        interleaved_out[(offset + f) * playback_channels + channel]
                            += output_planes[slot][f];
                    }
                }
            }
            cycles.fetch_add(1, std::memory_order_relaxed);
            offset += count;
        }
    }
};

WasapiBackend::WasapiBackend() : impl_(std::make_unique<Impl>()) {}
WasapiBackend::~WasapiBackend() { close(); }

void WasapiBackend::setForceQuantum(bool force) noexcept { impl_->force_quantum = force; }
void WasapiBackend::setNodeName(std::string name) { impl_->node_name = std::move(name); }

bool WasapiBackend::open()
{
    if (impl_->context_open) return true;
    const ma_backend backend = ma_backend_wasapi;
    ma_context_config config = ma_context_config_init();
    const ma_result result = ma_context_init(&backend, 1, &config, &impl_->context);
    if (result != MA_SUCCESS) {
        spdlog::error("cannot initialize WASAPI: {}", ma_result_description(result));
        return false;
    }
    impl_->context_open = true;
    return true;
}

void WasapiBackend::close() noexcept
{
    stop();
    if (impl_->context_open) {
        ma_context_uninit(&impl_->context);
        impl_->context_open = false;
    }
}

std::vector<DeviceInfo> WasapiBackend::enumerateDevices()
{
    WasapiSession session;
    if (!session.open("avc-engine-enumerate")) return {};
    return session.enumerateDevices();
}

std::string WasapiBackend::defaultDeviceName(Direction direction)
{
    WasapiSession session;
    if (!session.open("avc-engine-default")) return {};
    return session.defaultDeviceName(direction);
}

bool WasapiBackend::start(const types::AudioFormat &format, Processor *processor)
{
    if (!impl_->context_open || processor == nullptr) return false;
    impl_->format = format;
    impl_->processor = processor;
    std::string error;
    if (!impl_->initDevice(0, 2, {}, {}, error)) {
        spdlog::error("{}", error);
        return false;
    }
    return true;
}

void WasapiBackend::stop() noexcept
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->uninitDevice();
    impl_->processor = nullptr;
}

BackendStats WasapiBackend::stats() const noexcept
{
    BackendStats out;
    out.cycles = impl_->cycles.load(std::memory_order_relaxed);
    out.xruns = impl_->xruns.load(std::memory_order_relaxed);
    out.process_ns_last = impl_->process_last.load(std::memory_order_relaxed);
    out.process_ns_max = impl_->process_max.load(std::memory_order_relaxed);
    out.process_ns_avg = impl_->process_avg.load(std::memory_order_relaxed);
    out.wakeup_jitter_ns_max = impl_->jitter_max.load(std::memory_order_relaxed);
    out.quantum = impl_->format.quantum;
    out.sample_rate = impl_->format.sample_rate;
    out.actual_quantum = impl_->actual_quantum.load(std::memory_order_relaxed);
    out.actual_rate = impl_->actual_rate.load(std::memory_order_relaxed);
    out.sched_policy = impl_->sched_policy.load(std::memory_order_relaxed);
    out.sched_priority = impl_->sched_priority.load(std::memory_order_relaxed);
    out.audio_tid = impl_->audio_tid.load(std::memory_order_relaxed);
    return out;
}

void WasapiBackend::resetPeaks() noexcept
{
    impl_->process_max.store(0, std::memory_order_relaxed);
    impl_->jitter_max.store(0, std::memory_order_relaxed);
}

bool WasapiBackend::bindIo(const std::vector<IoRequest> &requests,
                           std::map<std::string, std::vector<std::uint32_t>> &slots,
                           std::string &error)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    std::string capture_target;
    std::string playback_target;
    std::uint32_t capture_channels = 0;
    std::uint32_t playback_channels = 0;
    for (const IoRequest &request : requests) {
        if (isVirtual(request.kind)) {
            error = "virtual audio nodes on Windows require a signed virtual audio driver";
            return false;
        }
        if (request.kind == IoKind::Playback && request.exclusive) {
            error = "exclusive playback nodes are not supported by the Windows shared-mode backend";
            return false;
        }
        std::string &selected = producesSignal(request.kind) ? capture_target : playback_target;
        if (!selected.empty() && selected != request.target) {
            error = "the Windows backend currently supports one capture endpoint and one playback "
                    "endpoint per graph";
            return false;
        }
        selected = request.target;
        if (producesSignal(request.kind)) {
            capture_channels = std::max(capture_channels, request.channels);
        } else {
            playback_channels = std::max(playback_channels, request.channels);
        }
    }
    if (!impl_->targetExists(capture_target, ma_device_type_capture)) {
        error = "the selected Windows capture endpoint is no longer available";
        return false;
    }
    if (!impl_->targetExists(playback_target, ma_device_type_playback)) {
        error = "the selected Windows playback endpoint is no longer available";
        return false;
    }

    // Preserve slots for nodes that also exist in the running graph. New slots
    // are allocated before old nodes are released, matching the graph swap.
    for (const IoRequest &request : requests) {
        auto found = std::find_if(impl_->bindings.begin(), impl_->bindings.end(),
                                  [&](const Impl::Binding &entry) {
                                      return entry.owner == request.node && entry.kind == request.kind;
                                  });
        if (found == impl_->bindings.end()) {
            impl_->bindings.push_back({request.node, request.target, request.kind, {}});
            found = std::prev(impl_->bindings.end());
        }
        found->target = request.target;
        while (found->slots.size() < request.channels) {
            const std::uint32_t slot = impl_->allocateSlot(request.kind);
            if (slot == std::numeric_limits<std::uint32_t>::max()) {
                error = "the Windows backend supports at most 64 capture and 64 playback ports";
                return false;
            }
            found->slots.push_back(slot);
        }
        slots[request.node].assign(found->slots.begin(), found->slots.begin() + request.channels);
        for (std::uint32_t channel = 0; channel < request.channels; ++channel) {
            auto &map = producesSignal(request.kind) ? impl_->input_channel
                                                     : impl_->output_channel;
            map[found->slots[channel]].store(static_cast<int>(channel), std::memory_order_release);
        }
    }

    if (capture_channels != impl_->capture_channels
        || playback_channels != impl_->playback_channels
        || capture_target != impl_->capture_target || playback_target != impl_->playback_target) {
        if (!impl_->initDevice(capture_channels, playback_channels, capture_target,
                               playback_target, error)) {
            return false;
        }
    }
    return true;
}

void WasapiBackend::releaseIo(const std::set<std::string> &keep)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    for (auto it = impl_->bindings.begin(); it != impl_->bindings.end();) {
        if (keep.contains(it->owner)) {
            ++it;
            continue;
        }
        auto &used = producesSignal(it->kind) ? impl_->input_used : impl_->output_used;
        auto &map = producesSignal(it->kind) ? impl_->input_channel : impl_->output_channel;
        for (const std::uint32_t slot : it->slots) {
            map[slot].store(kUnusedChannel, std::memory_order_release);
            used[slot] = false;
        }
        it = impl_->bindings.erase(it);
    }
}

}
