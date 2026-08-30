#include "audio/WasapiBackend.hpp"

#include "audio/UsbIpAudio.hpp"
#include "audio/WasapiSession.hpp"
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

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace avc::audio {
namespace {

constexpr std::size_t kMaxSlots = 64;
constexpr std::size_t kEndpointRingFrames = 32768;
constexpr int kUnusedChannel = -1;
constexpr std::string_view kProcessTargetPrefix = "wasapi-process:";
constexpr int kProcessCommandStart = 1;
constexpr int kProcessCommandPause = 2;
constexpr int kProcessCommandStop = 3;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

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

std::optional<DWORD> processIdFromTarget(std::string_view target)
{
    if (!target.starts_with(kProcessTargetPrefix)) return std::nullopt;
    target.remove_prefix(kProcessTargetPrefix.size());
    DWORD process_id = 0;
    const auto [end, error] =
        std::from_chars(target.data(), target.data() + target.size(), process_id);
    if (error != std::errc{} || end != target.data() + target.size() || process_id == 0)
        return std::nullopt;
    return process_id;
}

bool processExists(DWORD process_id)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) return false;
    DWORD exit_code = 0;
    const bool active = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return active;
}

std::string hresultMessage(HRESULT result)
{
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t *>(&message), 0,
        nullptr);
    std::string out;
    if (length > 0 && message != nullptr) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(length),
                                             nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            out.resize(static_cast<std::size_t>(size));
            (void)WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(length),
                                      out.data(), size, nullptr, nullptr);
            while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
        }
        LocalFree(message);
    }
    if (out.empty()) out = "HRESULT 0x" + std::to_string(static_cast<unsigned>(result));
    return out;
}

class ProcessActivationHandler final : public IActivateAudioInterfaceCompletionHandler,
                                       public IAgileObject {
public:
    ProcessActivationHandler() : ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override
    {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        } else if (id == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject *>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE
    ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) override
    {
        HRESULT activation_result = E_UNEXPECTED;
        IUnknown *unknown = nullptr;
        result_ = operation->GetActivateResult(&activation_result, &unknown);
        if (SUCCEEDED(result_)) result_ = activation_result;
        if (SUCCEEDED(result_) && unknown != nullptr)
            result_ = unknown->QueryInterface(__uuidof(IAudioClient),
                                              reinterpret_cast<void **>(&client_));
        if (unknown != nullptr) unknown->Release();
        SetEvent(ready_);
        return S_OK;
    }

    HANDLE readyEvent() const noexcept { return ready_; }
    HRESULT result() const noexcept { return result_; }

    IAudioClient *copyClient() const noexcept
    {
        if (client_ != nullptr) client_->AddRef();
        return client_;
    }

private:
    ~ProcessActivationHandler()
    {
        if (client_ != nullptr) client_->Release();
        if (ready_ != nullptr) CloseHandle(ready_);
    }

    std::atomic<ULONG> references_{1};
    HANDLE ready_ = nullptr;
    HRESULT result_ = E_PENDING;
    IAudioClient *client_ = nullptr;
};

struct SlotRing {
    std::array<float, kEndpointRingFrames> samples{};
    alignas(64) std::atomic<std::uint64_t> read{0};
    alignas(64) std::atomic<std::uint64_t> write{0};
    bool primed = false;
    float last = 0.0F;

    void reset() noexcept
    {
        read.store(0, std::memory_order_relaxed);
        write.store(0, std::memory_order_relaxed);
        primed = false;
        last = 0.0F;
    }

    bool pushPlane(const float *source, std::uint32_t frames) noexcept
    {
        const std::uint64_t current_write = write.load(std::memory_order_relaxed);
        const std::uint64_t current_read = read.load(std::memory_order_acquire);
        const std::uint64_t available = kEndpointRingFrames - (current_write - current_read);
        const std::uint32_t count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, available));
        for (std::uint32_t frame = 0; frame < count; ++frame)
            samples[(current_write + frame) % kEndpointRingFrames] = source[frame];
        write.store(current_write + count, std::memory_order_release);
        return count == frames;
    }

    bool pushInterleaved(const float *source, std::uint32_t frames, std::uint32_t channels,
                         std::uint32_t channel) noexcept
    {
        const std::uint64_t current_write = write.load(std::memory_order_relaxed);
        const std::uint64_t current_read = read.load(std::memory_order_acquire);
        const std::uint64_t available = kEndpointRingFrames - (current_write - current_read);
        const std::uint32_t count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, available));
        for (std::uint32_t frame = 0; frame < count; ++frame) {
            samples[(current_write + frame) % kEndpointRingFrames] =
                source[static_cast<std::size_t>(frame) * channels + channel];
        }
        write.store(current_write + count, std::memory_order_release);
        return count == frames;
    }

    bool pushSilence(std::uint32_t frames) noexcept
    {
        const std::uint64_t current_write = write.load(std::memory_order_relaxed);
        const std::uint64_t current_read = read.load(std::memory_order_acquire);
        const std::uint64_t available = kEndpointRingFrames - (current_write - current_read);
        const std::uint32_t count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, available));
        for (std::uint32_t frame = 0; frame < count; ++frame)
            samples[(current_write + frame) % kEndpointRingFrames] = 0.0F;
        write.store(current_write + count, std::memory_order_release);
        return count == frames;
    }

    // Every WASAPI endpoint owns a clock. A small elastic buffer and a one
    // sample correction outside its center band keeps independent clocks from
    // eventually overflowing or underrunning without changing graph timing.
    void pull(float *destination, std::uint32_t frames, std::uint32_t quantum,
              std::uint32_t minimum_target = 0) noexcept
    {
        std::uint64_t current_read = read.load(std::memory_order_relaxed);
        const std::uint64_t current_write = write.load(std::memory_order_acquire);
        std::uint64_t available = current_write - current_read;
        const std::uint64_t target = std::min<std::uint64_t>(
            kEndpointRingFrames / 2,
            std::max({static_cast<std::uint64_t>(quantum) * 3,
                      static_cast<std::uint64_t>(frames) * 3,
                      static_cast<std::uint64_t>(minimum_target)}));

        if (!primed) {
            if (available < target) {
                std::fill_n(destination, frames, 0.0F);
                return;
            }
            primed = true;
        }
        if (available < frames) {
            std::fill_n(destination, frames, 0.0F);
            primed = false;
            last = 0.0F;
            return;
        }

        const std::uint64_t margin = std::max<std::uint64_t>(quantum, target / 4);
        if (available > target + margin) {
            const std::uint64_t skip = std::min<std::uint64_t>(4, available - target);
            current_read += skip;
            available -= skip;
        }

        std::uint32_t offset = 0;
        std::uint32_t consume = frames;
        if (available + margin < target && frames > 1) {
            destination[0] = last;
            offset = 1;
            consume = frames - 1;
        }
        for (std::uint32_t frame = 0; frame < consume; ++frame) {
            destination[offset + frame] = samples[(current_read + frame) % kEndpointRingFrames];
        }
        if (frames > 0) last = destination[frames - 1];
        read.store(current_read + consume, std::memory_order_release);
    }
};

}

struct WasapiBackend::Impl {
    struct PhysicalEndpoint;

    enum class EndpointMode : std::uint8_t {
        Capture,
        Playback,
        DeviceLoopback,
        ProcessLoopback,
    };

    struct Binding {
        std::string owner;
        std::string target;
        IoKind kind = IoKind::Capture;
        bool exclusive = false;
        std::vector<std::uint32_t> slots;
    };

    struct EndpointSpec {
        IoKind kind = IoKind::Capture;
        std::string target;
        std::uint32_t channels = 0;
        bool exclusive = false;
        EndpointMode mode = EndpointMode::Capture;

        bool operator==(const EndpointSpec &) const = default;
    };

    struct PhysicalEndpoint {
        Impl *owner = nullptr;
        EndpointSpec spec;
        ma_device device{};
        bool open = false;
        bool started = false;
        bool dormant = false;
        std::uint32_t ring_target_frames = 0;
        std::array<float, types::kMaxQuantum> scratch{};
        std::thread process_thread;
        HANDLE process_ready = nullptr;
        HANDLE process_command = nullptr;
        HANDLE process_ack = nullptr;
        HANDLE process_samples = nullptr;
        std::atomic<int> requested_command{0};
        HRESULT process_result = E_PENDING;
        std::string process_error;

        static void dataCallback(ma_device *device, void *output, const void *input,
                                 ma_uint32 frames);
        void process(void *output, const void *input, std::uint32_t frames) noexcept;
        void pushCapture(const float *input, std::uint32_t frames, bool silent) noexcept;
        bool openProcessLoopback(DWORD process_id, std::string &error);
        void processLoop(DWORD process_id) noexcept;
        bool start(std::string &error);
        void pause() noexcept;
        void close() noexcept;
    };

    ma_context context{};
    bool context_open = false;
    bool force_quantum = false;
    std::string node_name = "avc";
    Processor *processor = nullptr;
    types::AudioFormat format{};
    std::mutex control_mutex;
    std::vector<Binding> bindings;
    std::vector<std::unique_ptr<PhysicalEndpoint>> endpoints;
    std::vector<EndpointSpec> endpoint_specs;
    std::array<bool, kMaxSlots> input_used{};
    std::array<bool, kMaxSlots> output_used{};
    std::array<std::atomic<int>, kMaxSlots> input_channel{};
    std::array<std::atomic<int>, kMaxSlots> output_channel{};
    std::array<std::atomic<PhysicalEndpoint *>, kMaxSlots> input_endpoint{};
    std::array<std::atomic<PhysicalEndpoint *>, kMaxSlots> output_endpoint{};
    std::array<std::atomic<UsbIpAudioDevice *>, kMaxSlots> input_usb{};
    std::array<std::atomic<UsbIpAudioDevice *>, kMaxSlots> output_usb{};
    std::array<SlotRing, kMaxSlots> input_rings{};
    std::array<SlotRing, kMaxSlots> output_rings{};
    std::atomic<std::uint32_t> input_high{0};
    std::atomic<std::uint32_t> output_high{0};
    UsbIpAudioManager usbip;

    std::array<std::array<types::Sample, types::kMaxQuantum>, kMaxSlots> input_planes{};
    std::array<std::array<types::Sample, types::kMaxQuantum>, kMaxSlots> output_planes{};
    std::array<types::Sample *, kMaxSlots> input_ptrs{};
    std::array<types::Sample *, kMaxSlots> output_ptrs{};

    std::thread clock_thread;
    HANDLE clock_timer = nullptr;
    HANDLE clock_stop = nullptr;
    std::atomic<bool> clock_running{false};
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

    Impl()
    {
        for (auto &entry : input_channel) entry.store(kUnusedChannel);
        for (auto &entry : output_channel) entry.store(kUnusedChannel);
        for (auto &entry : input_endpoint) entry.store(nullptr);
        for (auto &entry : output_endpoint) entry.store(nullptr);
        for (auto &entry : input_usb) entry.store(nullptr);
        for (auto &entry : output_usb) entry.store(nullptr);
    }

    std::optional<ma_device_id> resolveId(const std::string &target, ma_device_type type)
    {
        if (target.empty() || target == "@default_source" || target == "@default_sink")
            return std::nullopt;
        ma_device_info *playback = nullptr;
        ma_device_info *capture = nullptr;
        ma_uint32 playback_count = 0;
        ma_uint32 capture_count = 0;
        if (ma_context_get_devices(&context, &playback, &playback_count, &capture,
                                   &capture_count) != MA_SUCCESS)
            return std::nullopt;
        ma_device_info *list = type == ma_device_type_capture ? capture : playback;
        const ma_uint32 count = type == ma_device_type_capture ? capture_count : playback_count;
        for (ma_uint32 i = 0; i < count; ++i) {
            if (idString(list[i].id) == target) return list[i].id;
        }
        return std::nullopt;
    }

    std::optional<EndpointMode> endpointMode(const std::string &target, IoKind kind)
    {
        if (!producesSignal(kind)) {
            if (processIdFromTarget(target)) return std::nullopt;
            if (target.empty() || target == "@default_sink"
                || resolveId(target, ma_device_type_playback))
                return EndpointMode::Playback;
            return std::nullopt;
        }

        if (const auto process_id = processIdFromTarget(target)) {
            return EndpointMode::ProcessLoopback;
        }
        if (target.empty() || target == "@default_source"
            || resolveId(target, ma_device_type_capture))
            return EndpointMode::Capture;
        if (target == "@default_sink" || resolveId(target, ma_device_type_playback))
            return EndpointMode::DeviceLoopback;
        return std::nullopt;
    }

    PhysicalEndpoint *findEndpoint(IoKind kind, const std::string &target) noexcept
    {
        for (const auto &endpoint : endpoints) {
            if (endpoint->spec.kind == kind && endpoint->spec.target == target)
                return endpoint.get();
        }
        return nullptr;
    }

    bool openEndpoints(const std::vector<EndpointSpec> &specs, std::string &error)
    {
        endpoints.clear();
        for (const EndpointSpec &spec : specs) {
            auto endpoint = std::make_unique<PhysicalEndpoint>();
            endpoint->owner = this;
            endpoint->spec = spec;
            if (spec.mode == EndpointMode::ProcessLoopback) {
                const auto process_id = processIdFromTarget(spec.target);
                if (!process_id) {
                    error = "invalid Windows process loopback source '" + spec.target + "'";
                    stopEndpoints();
                    return false;
                }
                if (!processExists(*process_id)) {
                    endpoint->open = true;
                    endpoint->dormant = true;
                    spdlog::warn("process loopback source '{}' is no longer running; capture will "
                                 "produce silence until another application is selected",
                                 spec.target);
                } else if (!endpoint->openProcessLoopback(*process_id, error)) {
                    // The application can exit between validation and asynchronous
                    // activation. That is a normal property of process sources, not a
                    // reason to reject the rest of the graph.
                    if (processExists(*process_id)) {
                        stopEndpoints();
                        return false;
                    }
                    error.clear();
                    endpoint->open = true;
                    endpoint->dormant = true;
                    spdlog::warn("process loopback source '{}' exited during activation; it will "
                                 "produce silence until another application is selected",
                                 spec.target);
                }
                endpoints.push_back(std::move(endpoint));
                continue;
            }

            const bool capture = spec.mode != EndpointMode::Playback;
            const ma_device_type type = spec.mode == EndpointMode::DeviceLoopback
                                            ? ma_device_type_loopback
                                        : capture ? ma_device_type_capture
                                                  : ma_device_type_playback;
            ma_device_config config = ma_device_config_init(type);
            config.sampleRate = format.sample_rate;
            config.periodSizeInFrames = format.quantum;
            config.periods = force_quantum ? 2 : 0;
            config.performanceProfile = ma_performance_profile_low_latency;
            config.dataCallback = &PhysicalEndpoint::dataCallback;
            config.pUserData = endpoint.get();
            const auto id = resolveId(
                spec.target, spec.mode == EndpointMode::DeviceLoopback ? ma_device_type_playback
                                                                       : type);
            if (capture) {
                config.capture.format = ma_format_f32;
                config.capture.channels = spec.channels;
                config.capture.shareMode =
                    spec.exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
                if (id) config.capture.pDeviceID = &*id;
            } else {
                config.playback.format = ma_format_f32;
                config.playback.channels = spec.channels;
                config.playback.shareMode =
                    spec.exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
                if (id) config.playback.pDeviceID = &*id;
            }
            const ma_result result = ma_device_init(&context, &config, &endpoint->device);
            if (result != MA_SUCCESS) {
                error = "cannot open WASAPI endpoint '" + spec.target + "'"
                        + (spec.exclusive ? " in exclusive mode: " : ": ")
                        + ma_result_description(result);
                stopEndpoints();
                return false;
            }
            endpoint->open = true;
            endpoints.push_back(std::move(endpoint));
        }
        endpoint_specs = specs;
        return true;
    }

    bool startEndpoints(std::string &error)
    {
        for (const auto &endpoint : endpoints) {
            if (!endpoint->start(error)) {
                stopEndpoints();
                return false;
            }
        }
        return true;
    }

    void pauseEndpoints() noexcept
    {
        for (const auto &endpoint : endpoints) endpoint->pause();
    }

    void stopEndpoints() noexcept
    {
        for (const auto &endpoint : endpoints) endpoint->close();
        endpoints.clear();
        endpoint_specs.clear();
    }

    bool startClock(std::string &error)
    {
        if (clock_running.load(std::memory_order_acquire)) return true;
        clock_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        clock_timer = CreateWaitableTimerExW(nullptr, nullptr,
                                             CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);
        if (clock_timer == nullptr)
            clock_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (clock_stop == nullptr || clock_timer == nullptr) {
            error = "cannot create the Windows engine clock (error "
                    + std::to_string(GetLastError()) + ")";
            if (clock_stop != nullptr) CloseHandle(clock_stop);
            if (clock_timer != nullptr) CloseHandle(clock_timer);
            clock_stop = nullptr;
            clock_timer = nullptr;
            return false;
        }
        last_wakeup = 0;
        clock_running.store(true, std::memory_order_release);
        try {
            clock_thread = std::thread([this] { clockLoop(); });
        } catch (...) {
            clock_running.store(false, std::memory_order_release);
            CloseHandle(clock_stop);
            CloseHandle(clock_timer);
            clock_stop = nullptr;
            clock_timer = nullptr;
            error = "cannot create the Windows engine clock thread";
            return false;
        }
        return true;
    }

    void stopClock() noexcept
    {
        if (!clock_running.exchange(false, std::memory_order_acq_rel)) return;
        if (clock_stop != nullptr) SetEvent(clock_stop);
        if (clock_thread.joinable()) clock_thread.join();
        if (clock_timer != nullptr) {
            CancelWaitableTimer(clock_timer);
            CloseHandle(clock_timer);
            clock_timer = nullptr;
        }
        if (clock_stop != nullptr) {
            CloseHandle(clock_stop);
            clock_stop = nullptr;
        }
    }

    void clockLoop() noexcept
    {
        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const rt::SchedInfo info = rt::currentSchedInfo();
        sched_policy.store(info.policy, std::memory_order_relaxed);
        sched_priority.store(info.priority, std::memory_order_relaxed);
        audio_tid.store(info.tid, std::memory_order_relaxed);
        rt::enableFlushToZero();

        const auto period = std::chrono::nanoseconds(
            std::max<std::uint64_t>(1, 1000000000ULL * format.quantum / format.sample_rate));
        auto deadline = std::chrono::steady_clock::now();
        const HANDLE waits[2] = {clock_stop, clock_timer};
        while (clock_running.load(std::memory_order_acquire)) {
            deadline += period;
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = std::max(std::chrono::nanoseconds(1), deadline - now);
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(
                1, static_cast<LONGLONG>((remaining.count() + 99) / 100));
            if (!SetWaitableTimer(clock_timer, &due, 0, nullptr, nullptr, FALSE)) {
                std::this_thread::sleep_until(deadline);
            } else if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
                break;
            }
            if (!clock_running.load(std::memory_order_acquire)) break;
            process(format.quantum);
            const auto after = std::chrono::steady_clock::now();
            if (after > deadline + period) deadline = after;
        }
        if (mmcss != nullptr) (void)AvRevertMmThreadCharacteristics(mmcss);
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

    void process(std::uint32_t frame_count) noexcept
    {
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

        std::uint32_t offset = 0;
        while (offset < frame_count) {
            const std::uint32_t count =
                std::min<std::uint32_t>(frame_count - offset, types::kMaxQuantum);
            const std::uint32_t in_count = input_high.load(std::memory_order_acquire);
            const std::uint32_t out_count = output_high.load(std::memory_order_acquire);

            for (std::uint32_t slot = 0; slot < in_count; ++slot) {
                UsbIpAudioDevice *usb = input_usb[slot].load(std::memory_order_acquire);
                PhysicalEndpoint *endpoint =
                    input_endpoint[slot].load(std::memory_order_acquire);
                const int channel = input_channel[slot].load(std::memory_order_relaxed);
                auto &plane = input_planes[slot];
                if (usb != nullptr && channel >= 0) {
                    usb->pullPlayback(static_cast<std::uint32_t>(channel), plane.data(), count);
                    input_ptrs[slot] = plane.data();
                } else if (endpoint != nullptr && channel >= 0) {
                    input_rings[slot].pull(plane.data(), count, format.quantum,
                                           endpoint->ring_target_frames);
                    input_ptrs[slot] = plane.data();
                } else {
                    input_ptrs[slot] = nullptr;
                }
            }
            for (std::uint32_t slot = 0; slot < out_count; ++slot) {
                const bool connected = output_usb[slot].load(std::memory_order_acquire) != nullptr
                                       || output_endpoint[slot].load(std::memory_order_acquire)
                                              != nullptr;
                if (connected) {
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

            for (std::uint32_t slot = 0; slot < out_count; ++slot) {
                if (output_endpoint[slot].load(std::memory_order_acquire) != nullptr
                    && !output_rings[slot].pushPlane(output_planes[slot].data(), count))
                    xruns.fetch_add(1, std::memory_order_relaxed);
                UsbIpAudioDevice *usb = output_usb[slot].load(std::memory_order_acquire);
                const int channel = output_channel[slot].load(std::memory_order_relaxed);
                if (usb != nullptr && channel >= 0)
                    usb->pushCapture(static_cast<std::uint32_t>(channel),
                                     output_planes[slot].data(), count);
            }
            cycles.fetch_add(1, std::memory_order_relaxed);
            offset += count;
        }
    }
};

void WasapiBackend::Impl::PhysicalEndpoint::dataCallback(ma_device *device, void *output,
                                                          const void *input, ma_uint32 frames)
{
    static_cast<PhysicalEndpoint *>(device->pUserData)
        ->process(output, input, static_cast<std::uint32_t>(frames));
}

void WasapiBackend::Impl::PhysicalEndpoint::process(void *raw_output, const void *raw_input,
                                                    std::uint32_t frames) noexcept
{
    const bool capture = producesSignal(spec.kind);
    if (capture) {
        const auto *input = static_cast<const float *>(raw_input);
        if (input != nullptr) pushCapture(input, frames, false);
        return;
    }

    auto *output = static_cast<float *>(raw_output);
    if (output == nullptr) return;
    std::fill_n(output, static_cast<std::size_t>(frames) * spec.channels, 0.0F);
    std::uint32_t offset = 0;
    while (offset < frames) {
        const std::uint32_t count =
            std::min<std::uint32_t>(frames - offset, types::kMaxQuantum);
        const std::uint32_t high = owner->output_high.load(std::memory_order_acquire);
        for (std::uint32_t slot = 0; slot < high; ++slot) {
            if (owner->output_endpoint[slot].load(std::memory_order_acquire) != this) continue;
            const int channel = owner->output_channel[slot].load(std::memory_order_relaxed);
            if (channel < 0 || static_cast<std::uint32_t>(channel) >= spec.channels) continue;
            owner->output_rings[slot].pull(scratch.data(), count, owner->format.quantum);
            for (std::uint32_t frame = 0; frame < count; ++frame) {
                output[static_cast<std::size_t>(offset + frame) * spec.channels + channel]
                    += scratch[frame];
            }
        }
        offset += count;
    }
}

void WasapiBackend::Impl::PhysicalEndpoint::pushCapture(const float *input,
                                                        std::uint32_t frames,
                                                        bool silent) noexcept
{
    const std::uint32_t high = owner->input_high.load(std::memory_order_acquire);
    for (std::uint32_t slot = 0; slot < high; ++slot) {
        if (owner->input_endpoint[slot].load(std::memory_order_acquire) != this) continue;
        const int channel = owner->input_channel[slot].load(std::memory_order_relaxed);
        if (channel < 0 || static_cast<std::uint32_t>(channel) >= spec.channels) continue;
        const bool complete =
            silent ? owner->input_rings[slot].pushSilence(frames)
                   : owner->input_rings[slot].pushInterleaved(
                         input, frames, spec.channels, static_cast<std::uint32_t>(channel));
        if (!complete) owner->xruns.fetch_add(1, std::memory_order_relaxed);
    }
}

bool WasapiBackend::Impl::PhysicalEndpoint::openProcessLoopback(DWORD process_id,
                                                                std::string &error)
{
    process_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    process_command = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    process_ack = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    process_samples = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (process_ready == nullptr || process_command == nullptr || process_ack == nullptr
        || process_samples == nullptr) {
        error = "cannot create process loopback synchronization objects";
        close();
        return false;
    }

    try {
        process_thread = std::thread([this, process_id] { processLoop(process_id); });
    } catch (...) {
        error = "cannot create the process loopback capture thread";
        close();
        return false;
    }

    if (WaitForSingleObject(process_ready, 10000) != WAIT_OBJECT_0) {
        error = "timed out while activating Windows process loopback capture";
        requested_command.store(kProcessCommandStop, std::memory_order_release);
        SetEvent(process_command);
        if (process_thread.joinable()) process_thread.join();
        close();
        return false;
    }
    if (FAILED(process_result)) {
        error = process_error.empty() ? hresultMessage(process_result) : process_error;
        if (process_thread.joinable()) process_thread.join();
        close();
        return false;
    }
    open = true;
    return true;
}

void WasapiBackend::Impl::PhysicalEndpoint::processLoop(DWORD process_id) noexcept
{
    spdlog::debug("process loopback {}: worker starting", process_id);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    auto fail = [&](HRESULT result, std::string message) {
        process_result = result;
        process_error = std::move(message);
        SetEvent(process_ready);
        if (uninitialize_com) CoUninitialize();
    };
    if (FAILED(com_result)) {
        fail(com_result, "cannot initialize COM for process loopback: "
                             + hresultMessage(com_result));
        return;
    }

    auto *handler = new (std::nothrow) ProcessActivationHandler();
    if (handler == nullptr || handler->readyEvent() == nullptr) {
        if (handler != nullptr) handler->Release();
        fail(E_OUTOFMEMORY, "cannot allocate the process loopback activation handler");
        return;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activation{};
    activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation.ProcessLoopbackParams.TargetProcessId = process_id;
    activation.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT parameters{};
    parameters.vt = VT_BLOB;
    parameters.blob.cbSize = sizeof(activation);
    parameters.blob.pBlobData = reinterpret_cast<BYTE *>(&activation);

    IActivateAudioInterfaceAsyncOperation *operation = nullptr;
    spdlog::debug("process loopback {}: activating audio interface", process_id);
    HRESULT result = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &parameters, handler,
        &operation);
    spdlog::debug("process loopback {}: activation call returned 0x{:08x}", process_id,
                  static_cast<unsigned>(result));
    if (SUCCEEDED(result)) {
        const DWORD activation_wait = WaitForSingleObject(handler->readyEvent(), 10000);
        spdlog::debug("process loopback {}: activation wait returned {}", process_id,
                      activation_wait);
        if (activation_wait != WAIT_OBJECT_0) result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    if (SUCCEEDED(result)) result = handler->result();
    IAudioClient *client = SUCCEEDED(result) ? handler->copyClient() : nullptr;
    if (operation != nullptr) operation->Release();
    handler->Release();
    if (FAILED(result) || client == nullptr) {
        fail(FAILED(result) ? result : E_NOINTERFACE,
             "cannot activate process loopback (Windows 10 build 20348 or newer is required): "
                 + hresultMessage(FAILED(result) ? result : E_NOINTERFACE));
        return;
    }

    WAVEFORMATEX capture_format{};
    capture_format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    capture_format.nChannels = static_cast<WORD>(spec.channels);
    capture_format.nSamplesPerSec = owner->format.sample_rate;
    capture_format.wBitsPerSample = 32;
    capture_format.nBlockAlign =
        static_cast<WORD>(capture_format.nChannels * capture_format.wBitsPerSample / 8);
    capture_format.nAvgBytesPerSec =
        capture_format.nSamplesPerSec * capture_format.nBlockAlign;
    const DWORD stream_flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                               | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                               | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    spdlog::debug("process loopback {}: initializing audio client", process_id);
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0,
                                &capture_format, nullptr);
    spdlog::debug("process loopback {}: audio client initialization returned 0x{:08x}",
                  process_id, static_cast<unsigned>(result));
    IAudioCaptureClient *capture = nullptr;
    if (SUCCEEDED(result))
        result = client->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void **>(&capture));
    if (SUCCEEDED(result)) result = client->SetEventHandle(process_samples);
    if (FAILED(result)) {
        if (capture != nullptr) capture->Release();
        client->Release();
        fail(result, "cannot initialize process loopback: " + hresultMessage(result));
        return;
    }

    process_result = S_OK;
    process_error.clear();
    // Process loopback uses the audio engine's shared-mode packet cadence, not
    // the graph quantum. Keep 20 ms plus one graph block queued; otherwise small
    // graph blocks can drain the ring between two packet events and produce
    // periodic gaps even though the source is continuously playing. Do not call
    // IAudioClient::GetBufferSize here: some process-loopback clients can block
    // that synchronous query indefinitely during graph startup.
    ring_target_frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        kEndpointRingFrames / 2,
        static_cast<std::uint64_t>(owner->format.sample_rate) / 50
            + owner->format.quantum));
    SetEvent(process_ready);

    bool running = false;
    bool stopping = false;
    const HANDLE waits[2] = {process_command, process_samples};
    while (!stopping) {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            const int command = requested_command.exchange(0, std::memory_order_acq_rel);
            if (command == kProcessCommandStart) {
                process_result = running ? S_OK : client->Start();
                if (SUCCEEDED(process_result)) running = true;
            } else if (command == kProcessCommandPause) {
                process_result = running ? client->Stop() : S_OK;
                if (SUCCEEDED(process_result)) {
                    running = false;
                    process_result = client->Reset();
                }
            } else if (command == kProcessCommandStop) {
                process_result = running ? client->Stop() : S_OK;
                running = false;
                stopping = true;
            }
            SetEvent(process_ack);
            continue;
        }
        if (wait != WAIT_OBJECT_0 + 1 || !running) continue;

        UINT32 frames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&frames)) && frames > 0) {
            BYTE *data = nullptr;
            DWORD flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;
            const HRESULT packet_result = capture->GetBuffer(
                &data, &frames, &flags, &device_position, &qpc_position);
            if (FAILED(packet_result)) {
                owner->xruns.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0)
                owner->xruns.fetch_add(1, std::memory_order_relaxed);
            pushCapture(reinterpret_cast<const float *>(data), frames, silent);
            (void)capture->ReleaseBuffer(frames);
        }
    }

    capture->Release();
    client->Release();
    if (uninitialize_com) CoUninitialize();
}

bool WasapiBackend::Impl::PhysicalEndpoint::start(std::string &error)
{
    if (started) return true;
    if (dormant) {
        started = true;
        return true;
    }
    if (spec.mode == EndpointMode::ProcessLoopback) {
        ResetEvent(process_ack);
        requested_command.store(kProcessCommandStart, std::memory_order_release);
        SetEvent(process_command);
        if (WaitForSingleObject(process_ack, 5000) != WAIT_OBJECT_0
            || FAILED(process_result)) {
            error = "cannot start process loopback '" + spec.target + "': "
                    + (FAILED(process_result) ? hresultMessage(process_result)
                                              : std::string("timed out"));
            return false;
        }
    } else {
        const ma_result result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            error = "cannot start WASAPI endpoint '" + spec.target + "': "
                    + ma_result_description(result);
            return false;
        }
    }
    started = true;
    return true;
}

void WasapiBackend::Impl::PhysicalEndpoint::pause() noexcept
{
    if (!started) return;
    if (dormant) {
        started = false;
        return;
    }
    if (spec.mode == EndpointMode::ProcessLoopback) {
        ResetEvent(process_ack);
        requested_command.store(kProcessCommandPause, std::memory_order_release);
        SetEvent(process_command);
        (void)WaitForSingleObject(process_ack, 5000);
    } else {
        (void)ma_device_stop(&device);
    }
    started = false;
}

void WasapiBackend::Impl::PhysicalEndpoint::close() noexcept
{
    if (spec.mode == EndpointMode::ProcessLoopback) {
        if (process_thread.joinable()) {
            ResetEvent(process_ack);
            requested_command.store(kProcessCommandStop, std::memory_order_release);
            if (process_command != nullptr) SetEvent(process_command);
            (void)WaitForSingleObject(process_ack, 5000);
            process_thread.join();
        }
    } else if (open) {
        if (started) ma_device_stop(&device);
        ma_device_uninit(&device);
    }
    if (process_ready != nullptr) CloseHandle(process_ready);
    if (process_command != nullptr) CloseHandle(process_command);
    if (process_ack != nullptr) CloseHandle(process_ack);
    if (process_samples != nullptr) CloseHandle(process_samples);
    process_ready = nullptr;
    process_command = nullptr;
    process_ack = nullptr;
    process_samples = nullptr;
    started = false;
    open = false;
    dormant = false;
}

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
    impl_->usbip.clear();
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
    if (!impl_->context_open || processor == nullptr || format.sample_rate == 0
        || format.quantum == 0 || format.quantum > types::kMaxQuantum)
        return false;
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->format = format;
    impl_->processor = processor;
    impl_->actual_rate.store(format.sample_rate, std::memory_order_relaxed);
    std::string error;
    if (!impl_->startClock(error)) {
        spdlog::error("{}", error);
        impl_->processor = nullptr;
        return false;
    }
    return true;
}

void WasapiBackend::stop() noexcept
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->stopClock();
    impl_->stopEndpoints();
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
    spdlog::debug("WASAPI graph binding: begin ({} request(s))", requests.size());
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    if (impl_->processor == nullptr) {
        error = "the engine is not running";
        return false;
    }
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
        if (!isVirtual(request.kind)) {
            if (!impl_->endpointMode(request.target, request.kind)) {
                error = "the selected Windows endpoint for '" + request.node
                        + "' is no longer available";
                return false;
            }
        }
    }
    spdlog::debug("WASAPI graph binding: opening USB/IP mappings");
    if (!impl_->usbip.ensure(requests, impl_->format.sample_rate, error)) return false;
    spdlog::debug("WASAPI graph binding: USB/IP mappings ready");

    std::vector<Impl::EndpointSpec> specs;
    for (const IoRequest &request : requests) {
        if (isVirtual(request.kind)) continue;
        auto found = std::ranges::find_if(specs, [&](const Impl::EndpointSpec &spec) {
            return spec.kind == request.kind && spec.target == request.target;
        });
        if (found == specs.end()) {
            const auto mode = impl_->endpointMode(request.target, request.kind);
            if (!mode) {
                error = "the selected Windows endpoint for '" + request.node
                        + "' disappeared while the graph was being prepared";
                return false;
            }
            specs.push_back({request.kind, request.target, request.channels, request.exclusive,
                             *mode});
        } else {
            found->channels = std::max(found->channels, request.channels);
            found->exclusive = found->exclusive || request.exclusive;
        }
    }
    std::ranges::sort(specs, {}, [](const Impl::EndpointSpec &spec) {
        return std::pair{static_cast<int>(spec.kind), spec.target};
    });

    impl_->stopClock();
    // A Windows process-loopback IAudioClient can acknowledge Stop/Reset/Start
    // yet stop delivering capture events afterwards. Recreate those clients on
    // every graph application; ordinary device endpoints remain reusable.
    const bool has_process_loopback = std::ranges::any_of(specs, [](const auto &spec) {
        return spec.mode == Impl::EndpointMode::ProcessLoopback;
    });
    const bool replace_endpoints = specs != impl_->endpoint_specs || has_process_loopback;
    if (replace_endpoints) {
        impl_->stopEndpoints();
        spdlog::debug("WASAPI graph binding: opening {} physical endpoint(s)", specs.size());
        if (!impl_->openEndpoints(specs, error)) {
            std::string ignored;
            (void)impl_->startClock(ignored);
            return false;
        }
        spdlog::debug("WASAPI graph binding: physical endpoints ready");
    } else {
        impl_->pauseEndpoints();
    }

    for (std::size_t slot = 0; slot < kMaxSlots; ++slot) {
        impl_->input_endpoint[slot].store(nullptr, std::memory_order_release);
        impl_->output_endpoint[slot].store(nullptr, std::memory_order_release);
        impl_->input_usb[slot].store(nullptr, std::memory_order_release);
        impl_->output_usb[slot].store(nullptr, std::memory_order_release);
        impl_->input_channel[slot].store(kUnusedChannel, std::memory_order_release);
        impl_->output_channel[slot].store(kUnusedChannel, std::memory_order_release);
        impl_->input_rings[slot].reset();
        impl_->output_rings[slot].reset();
    }

    slots.clear();
    for (const IoRequest &request : requests) {
        auto found = std::ranges::find_if(impl_->bindings, [&](const Impl::Binding &entry) {
            return entry.owner == request.node && entry.kind == request.kind;
        });
        if (found == impl_->bindings.end()) {
            impl_->bindings.push_back(
                {request.node, request.target, request.kind, request.exclusive, {}});
            found = std::prev(impl_->bindings.end());
        }
        found->target = request.target;
        found->exclusive = request.exclusive;
        while (found->slots.size() < request.channels) {
            const std::uint32_t slot = impl_->allocateSlot(request.kind);
            if (slot == std::numeric_limits<std::uint32_t>::max()) {
                error = "the Windows backend supports at most 64 capture and 64 playback ports";
                impl_->stopEndpoints();
                std::string ignored;
                (void)impl_->startClock(ignored);
                return false;
            }
            found->slots.push_back(slot);
        }
        slots[request.node].assign(found->slots.begin(), found->slots.begin() + request.channels);

        UsbIpAudioDevice *usb = nullptr;
        Impl::PhysicalEndpoint *endpoint = nullptr;
        if (isVirtual(request.kind)) {
            const auto device = impl_->usbip.find(request);
            if (!device) {
                error = "the requested USB/IP audio device is not available";
                impl_->stopEndpoints();
                std::string ignored;
                (void)impl_->startClock(ignored);
                return false;
            }
            usb = device.get();
        } else {
            endpoint = impl_->findEndpoint(request.kind, request.target);
            if (endpoint == nullptr) {
                error = "the selected Windows audio endpoint was not opened";
                impl_->stopEndpoints();
                std::string ignored;
                (void)impl_->startClock(ignored);
                return false;
            }
        }

        for (std::uint32_t channel = 0; channel < request.channels; ++channel) {
            const std::uint32_t slot = found->slots[channel];
            auto &channel_map = producesSignal(request.kind) ? impl_->input_channel
                                                             : impl_->output_channel;
            auto &endpoint_map = producesSignal(request.kind) ? impl_->input_endpoint
                                                              : impl_->output_endpoint;
            auto &usb_map = producesSignal(request.kind) ? impl_->input_usb : impl_->output_usb;
            channel_map[slot].store(static_cast<int>(channel), std::memory_order_release);
            endpoint_map[slot].store(endpoint, std::memory_order_release);
            usb_map[slot].store(usb, std::memory_order_release);
        }
    }

    if (!impl_->startEndpoints(error)) {
        std::string ignored;
        (void)impl_->startClock(ignored);
        return false;
    }
    if (!impl_->startClock(error)) {
        impl_->stopEndpoints();
        return false;
    }
    spdlog::debug("WASAPI graph binding: complete");
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
        auto &channel_map = producesSignal(it->kind) ? impl_->input_channel
                                                     : impl_->output_channel;
        auto &endpoint_map = producesSignal(it->kind) ? impl_->input_endpoint
                                                      : impl_->output_endpoint;
        auto &usb_map = producesSignal(it->kind) ? impl_->input_usb : impl_->output_usb;
        for (const std::uint32_t slot : it->slots) {
            endpoint_map[slot].store(nullptr, std::memory_order_release);
            usb_map[slot].store(nullptr, std::memory_order_release);
            channel_map[slot].store(kUnusedChannel, std::memory_order_release);
            used[slot] = false;
        }
        it = impl_->bindings.erase(it);
    }
}

}
