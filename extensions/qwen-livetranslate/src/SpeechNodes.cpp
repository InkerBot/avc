#include <avc/avc_plugin.hpp>

#include <avc_qwen/AudioBuffer.hpp>
#include <avc_qwen/AudioResampler.hpp>
#include <avc_qwen/Protocol.hpp>

#include "QwenShared.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using avc::qwen::detail::g_config;
using avc::qwen::detail::g_curl_ready;

template <std::size_t Capacity>
class FixedSnapshot {
public:
    void publish(std::string_view value, std::uint64_t stream = 0,
                 std::uint64_t segment = 0, std::uint64_t revision = 0,
                 bool final = false) noexcept
    {
        sequence_.fetch_add(1, std::memory_order_acq_rel);
        const std::size_t count = std::min(value.size(), Capacity - 1);
        for (std::size_t i = 0; i < count; ++i) {
            data_[i].store(value[i], std::memory_order_relaxed);
        }
        data_[count].store('\0', std::memory_order_relaxed);
        size_.store(static_cast<std::uint32_t>(count), std::memory_order_relaxed);
        stream_.store(stream, std::memory_order_relaxed);
        segment_.store(segment, std::memory_order_relaxed);
        revision_.store(revision, std::memory_order_relaxed);
        final_.store(final, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }

    std::size_t read(char *destination, std::size_t capacity,
                     std::uint64_t *stream = nullptr,
                     std::uint64_t *segment = nullptr,
                     std::uint64_t *revision = nullptr,
                     bool *final = nullptr) const noexcept
    {
        if (destination == nullptr || capacity == 0) return 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const std::uint64_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0) continue;
            const std::size_t count = std::min<std::size_t>(
                size_.load(std::memory_order_relaxed), capacity - 1);
            for (std::size_t i = 0; i < count; ++i) {
                destination[i] = data_[i].load(std::memory_order_relaxed);
            }
            const std::uint64_t read_stream = stream_.load(std::memory_order_relaxed);
            const std::uint64_t read_segment = segment_.load(std::memory_order_relaxed);
            const std::uint64_t read_revision = revision_.load(std::memory_order_relaxed);
            const bool read_final = final_.load(std::memory_order_relaxed);
            const std::uint64_t after = sequence_.load(std::memory_order_acquire);
            if (before == after && (after & 1U) == 0) {
                destination[count] = '\0';
                if (stream != nullptr) *stream = read_stream;
                if (segment != nullptr) *segment = read_segment;
                if (revision != nullptr) *revision = read_revision;
                if (final != nullptr) *final = read_final;
                return count;
            }
        }
        destination[0] = '\0';
        return 0;
    }

private:
    mutable std::atomic<std::uint64_t> sequence_{0};
    std::array<std::atomic<char>, Capacity> data_{};
    std::atomic<std::uint32_t> size_{0};
    std::atomic<std::uint64_t> stream_{0};
    std::atomic<std::uint64_t> segment_{0};
    std::atomic<std::uint64_t> revision_{0};
    std::atomic<bool> final_{false};
};

std::uint64_t makeTextStreamId()
{
    std::uint64_t value = 1469598103934665603ULL;
    for (const unsigned char byte : avc::qwen::makeEventId()) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value == 0 ? 1 : value;
}

class CurlHandle {
public:
    CurlHandle() : value_(curl_easy_init()) {}
    ~CurlHandle()
    {
        if (value_ != nullptr) curl_easy_cleanup(value_);
    }
    CurlHandle(const CurlHandle &) = delete;
    CurlHandle &operator=(const CurlHandle &) = delete;
    CURL *get() const noexcept { return value_; }

private:
    CURL *value_ = nullptr;
};

class CurlHeaders {
public:
    ~CurlHeaders()
    {
        if (value_ != nullptr) curl_slist_free_all(value_);
    }
    bool add(const std::string &header)
    {
        curl_slist *next = curl_slist_append(value_, header.c_str());
        if (next == nullptr) return false;
        value_ = next;
        return true;
    }
    curl_slist *get() const noexcept { return value_; }

private:
    curl_slist *value_ = nullptr;
};

class WebSocketConnection {
public:
    bool connect(const std::string &endpoint, const std::string &api_key,
                 std::string_view user_agent, std::string &error)
    {
        if (handle_.get() == nullptr) {
            error = "curl_easy_init failed";
            return false;
        }
        if (!headers_.add("Authorization: Bearer " + api_key)) {
            error = "could not allocate the authorization header";
            return false;
        }
        if (!g_config.workspace_id.empty()) {
            if (!avc::qwen::validIdentifier(g_config.workspace_id)) {
                error = "workspace_id contains invalid characters";
                return false;
            }
            if (!headers_.add("X-DashScope-WorkSpace: " + g_config.workspace_id)) {
                error = "could not allocate the workspace header";
                return false;
            }
        }

        CURL *curl = handle_.get();
        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_.get());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        user_agent_.assign(user_agent);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_.c_str());
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_.data());

        const CURLcode result = curl_easy_perform(curl);
        if (result == CURLE_OK) return true;
        error = curl_error_[0] != '\0' ? curl_error_.data() : curl_easy_strerror(result);
        return false;
    }

    bool send(const std::string &payload, std::string &error)
    {
        std::size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (offset < payload.size()) {
            std::size_t sent = 0;
            const CURLcode result = curl_ws_send(handle_.get(), payload.data() + offset,
                                                 payload.size() - offset, &sent, 0,
                                                 CURLWS_TEXT);
            offset += sent;
            if (result == CURLE_OK) continue;
            if (result == CURLE_AGAIN && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            error = std::string("WebSocket send failed: ") + curl_easy_strerror(result);
            return false;
        }
        return true;
    }

    template <class Handler>
    bool receiveAvailable(std::string &frame, Handler &&handler, std::string &error)
    {
        std::array<char, 65536> buffer{};
        while (true) {
            std::size_t received = 0;
            const curl_ws_frame *meta = nullptr;
            const CURLcode result = curl_ws_recv(handle_.get(), buffer.data(), buffer.size(),
                                                 &received, &meta);
            if (result == CURLE_AGAIN) return true;
            if (result != CURLE_OK) {
                error = std::string("WebSocket receive failed: ")
                      + curl_easy_strerror(result);
                return false;
            }
            if (meta == nullptr) {
                error = "WebSocket receive returned no frame metadata";
                return false;
            }
            if ((meta->flags & CURLWS_CLOSE) != 0) {
                error = "Qwen closed the WebSocket";
                return false;
            }
            if ((meta->flags & (CURLWS_TEXT | CURLWS_CONT)) == 0) continue;
            if (frame.size() + received > 2U * 1024U * 1024U) {
                error = "Qwen sent an unexpectedly large event";
                return false;
            }
            frame.append(buffer.data(), received);
            if (meta->bytesleft != 0 || (meta->flags & CURLWS_CONT) != 0) continue;

            const avc::qwen::ServerEvent event = avc::qwen::parseServerEvent(frame);
            frame.clear();
            if (!handler(event, error)) return false;
        }
    }

private:
    CurlHandle handle_;
    CurlHeaders headers_;
    std::array<char, CURL_ERROR_SIZE> curl_error_{};
    std::string user_agent_;
};

class TextSpscQueue {
public:
    void reset() noexcept
    {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

    bool push(std::string_view text) noexcept
    {
        const std::uint64_t write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) >= kSlots) return false;
        Slot &slot = slots_[static_cast<std::size_t>(write) & (kSlots - 1)];
        const std::size_t count = std::min(text.size(), slot.data.size());
        if (count > 0) std::memcpy(slot.data.data(), text.data(), count);
        slot.size = static_cast<std::uint32_t>(count);
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool pop(std::string &text)
    {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        if (write_.load(std::memory_order_acquire) == read) return false;
        const Slot &slot = slots_[static_cast<std::size_t>(read) & (kSlots - 1)];
        text.assign(slot.data.data(), slot.size);
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    void discardAll() noexcept
    {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    static constexpr std::size_t kSlots = 16;
    struct Slot {
        std::array<char, avc::text::kPlainPayloadBytes> data{};
        std::uint32_t size = 0;
    };
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    std::array<Slot, kSlots> slots_{};
};

class QwenAsrNode final : public avc::sdk::Node {
public:
    enum Param : std::uint32_t {
        kEnabled,
        kLanguage,
        kVadThreshold,
        kSilenceDuration,
    };

    QwenAsrNode() : text_stream_(makeTextStreamId())
    {
        status_message_.publish("Waiting for the audio engine");
    }

    ~QwenAsrNode() override
    {
        stop_.store(true, std::memory_order_release);
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool prepare(const AvcPrepareInfo &info, std::string &error) override
    {
        if (info.sample_rate < 8000 || info.sample_rate > 384000) {
            error = "unsupported engine sample rate";
            return false;
        }
        sample_rate_ = info.sample_rate;
        input_resampler_.reset(sample_rate_, avc::qwen::kInputSampleRate);
        input_audio_.reset(avc::qwen::kInputSampleRate * 16U);
        const std::size_t converted =
            (static_cast<std::size_t>(info.max_quantum) * avc::qwen::kInputSampleRate
             + sample_rate_ - 1)
                / sample_rate_
            + 4;
        converted_input_.assign(converted, 0);
        silent_input_.assign(info.max_quantum, 0.0F);

        endpoint_ = avc::qwen::buildEndpoint(g_config.region, g_config.workspace_id,
                                             g_config.asr_endpoint, g_config.asr_model,
                                             error);
        if (endpoint_.empty()) return false;
        std::string validation_error;
        if (avc::qwen::makeAsrSessionUpdate(sessionOptions(), validation_error).empty()) {
            error = validation_error;
            return false;
        }

        stop_.store(false, std::memory_order_release);
        worker_ = std::thread([this] { workerMain(); });
        return true;
    }

    void setParam(std::uint32_t index, float value) noexcept override
    {
        switch (index) {
        case kEnabled: enabled_.store(value >= 0.5F, std::memory_order_release); break;
        case kLanguage:
            language_.store(static_cast<std::uint32_t>(std::lround(value)),
                            std::memory_order_release);
            break;
        case kVadThreshold: vad_threshold_.store(value, std::memory_order_release); break;
        case kSilenceDuration:
            silence_ms_.store(static_cast<std::uint32_t>(std::lround(value)),
                              std::memory_order_release);
            break;
        default: return;
        }
        revision_.fetch_add(1, std::memory_order_acq_rel);
        wake_.notify_all();
    }

    bool status(AvcNodeStatus &out) const noexcept override
    {
        out.state = state_.load(std::memory_order_relaxed);
        out.progress = out.state == AVC_NODE_READY ? 1.0F : 0.0F;
        out.processed_blocks = processed_chunks_.load(std::memory_order_relaxed);
        out.bypassed_blocks = dropped_chunks_.load(std::memory_order_relaxed);
        out.failures = failures_.load(std::memory_order_relaxed);
        status_message_.read(out.message, sizeof(out.message));
        return true;
    }

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        if (ctx.discontinuity != 0) {
            input_resampler_.reset(sample_rate_, avc::qwen::kInputSampleRate);
            input_epoch_.fetch_add(1, std::memory_order_acq_rel);
        }

        if (enabled_.load(std::memory_order_acquire)
            && configured_.load(std::memory_order_acquire)) {
            const float *input = ctx.n_inputs > 0 ? ctx.inputs[0] : nullptr;
            const std::span<const float> source(
                input != nullptr ? input : silent_input_.data(), ctx.nframes);
            const std::size_t count = input_resampler_.process(source, converted_input_);
            const std::size_t written = input_audio_.write(
                std::span<const std::int16_t>(converted_input_.data(), count));
            if (written != count) dropped_chunks_.fetch_add(1, std::memory_order_relaxed);
        }

        std::uint64_t stream = 0;
        std::uint64_t segment = 0;
        std::uint64_t revision = 0;
        bool final = false;
        const std::size_t size = transcript_.read(text_cache_.data(), text_cache_.size(),
                                                  &stream, &segment, &revision, &final);
        if (ctx.n_outputs > 0) {
            const std::string_view value{text_cache_.data(), size};
            if (stream != 0) {
                avc::sdk::writeTextFrame(ctx.out_blocks[0], value, stream, segment,
                                         revision, final);
            } else {
                avc::sdk::writeText(ctx.out_blocks[0], value);
            }
        }
    }

private:
    avc::qwen::AsrSessionOptions sessionOptions() const
    {
        avc::qwen::AsrSessionOptions options;
        const std::size_t index = std::min<std::size_t>(
            language_.load(std::memory_order_acquire),
            avc::qwen::asrLanguages().size() - 1);
        options.language = avc::qwen::asrLanguages()[index];
        options.vad_threshold = vad_threshold_.load(std::memory_order_acquire);
        options.silence_duration_ms = silence_ms_.load(std::memory_order_acquire);
        return options;
    }

    void setState(AvcNodeState state, std::string_view message) noexcept
    {
        state_.store(state, std::memory_order_release);
        status_message_.publish(message);
    }

    void beginTextSession() noexcept
    {
        if (text_revision_ != 0) {
            ++text_segment_;
            text_revision_ = 0;
        }
    }

    void publishText(std::string_view value, bool final) noexcept
    {
        transcript_.publish(value, text_stream_, text_segment_, ++text_revision_, final);
        if (final) {
            ++text_segment_;
            text_revision_ = 0;
        }
    }

    bool runSession(std::uint64_t session_revision, std::string &error)
    {
        WebSocketConnection connection;
        setState(AVC_NODE_LOADING, "Connecting to Qwen speech recognition");
        if (!connection.connect(endpoint_, g_config.api_key, "avc-qwen-asr/1.0", error)) {
            return false;
        }

        std::string protocol_error;
        const std::string update =
            avc::qwen::makeAsrSessionUpdate(sessionOptions(), protocol_error);
        if (update.empty()) {
            error = protocol_error;
            return false;
        }
        if (!connection.send(update, error)) return false;
        setState(AVC_NODE_LOADING, "Connected; waiting for ASR session.updated");

        std::array<std::int16_t, avc::qwen::kInputChunkSamples> input_chunk{};
        std::string frame;
        frame.reserve(65536);
        bool finishing = false;
        bool finished = false;
        auto finish_deadline = std::chrono::steady_clock::time_point::max();
        std::uint64_t seen_input_epoch = input_epoch_.load(std::memory_order_acquire);

        while (true) {
            const bool received = connection.receiveAvailable(
                frame,
                [this, &finished](const avc::qwen::ServerEvent &event,
                                  std::string &handler_error) {
                    switch (event.kind) {
                    case avc::qwen::ServerEventKind::SessionUpdated:
                        beginTextSession();
                        configured_.store(true, std::memory_order_release);
                        setState(AVC_NODE_READY, "Connected to Qwen speech recognition");
                        break;
                    case avc::qwen::ServerEventKind::SessionFinished:
                        finished = true;
                        break;
                    case avc::qwen::ServerEventKind::SourceTranscript:
                        publishText(event.text, false);
                        break;
                    case avc::qwen::ServerEventKind::SourceTranscriptDone:
                        publishText(event.text, true);
                        break;
                    case avc::qwen::ServerEventKind::Error:
                    case avc::qwen::ServerEventKind::Invalid:
                        handler_error = event.error;
                        return false;
                    default: break;
                    }
                    return true;
                },
                error);
            if (!received) return false;
            if (finished) return true;

            const bool should_finish = stop_.load(std::memory_order_acquire)
                                    || !enabled_.load(std::memory_order_acquire)
                                    || revision_.load(std::memory_order_acquire)
                                           != session_revision;
            if (should_finish && !finishing) {
                configured_.store(false, std::memory_order_release);
                if (!connection.send(avc::qwen::makeSessionFinish(), error)) return false;
                finishing = true;
                finish_deadline = std::chrono::steady_clock::now() + 5s;
            }
            if (finishing) {
                if (std::chrono::steady_clock::now() >= finish_deadline) return true;
                std::this_thread::sleep_for(5ms);
                continue;
            }

            const std::uint64_t input_epoch = input_epoch_.load(std::memory_order_acquire);
            if (input_epoch != seen_input_epoch) {
                seen_input_epoch = input_epoch;
                input_audio_.discardAll();
            }
            if (configured_.load(std::memory_order_acquire)) {
                for (int sent = 0; sent < 8
                                   && input_audio_.readable() >= input_chunk.size();
                     ++sent) {
                    if (input_audio_.read(input_chunk) != input_chunk.size()) break;
                    if (!connection.send(avc::qwen::makeAudioAppend(input_chunk), error)) {
                        return false;
                    }
                    processed_chunks_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_.wait_for(lock, 5ms, [this, session_revision] {
                return stop_.load(std::memory_order_acquire)
                    || !enabled_.load(std::memory_order_acquire)
                    || revision_.load(std::memory_order_acquire) != session_revision;
            });
        }
    }

    void workerMain()
    {
        unsigned backoff_seconds = 1;
        while (!stop_.load(std::memory_order_acquire)) {
            if (!enabled_.load(std::memory_order_acquire)) {
                configured_.store(false, std::memory_order_release);
                setState(AVC_NODE_OFFLINE, "Qwen speech recognition is disabled");
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_.wait_for(lock, 250ms, [this] {
                    return stop_.load(std::memory_order_acquire)
                        || enabled_.load(std::memory_order_acquire);
                });
                continue;
            }
            if (!g_curl_ready) {
                setState(AVC_NODE_ERROR, "libcurl initialization failed");
                failures_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (g_config.api_key.empty()) {
                setState(AVC_NODE_ERROR, "Configure the API key in Qwen extension settings");
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_.wait_for(lock, 1s, [this] {
                    return stop_.load(std::memory_order_acquire)
                        || !enabled_.load(std::memory_order_acquire);
                });
                continue;
            }

            const std::uint64_t session_revision = revision_.load(std::memory_order_acquire);
            std::string error;
            const bool clean = runSession(session_revision, error);
            configured_.store(false, std::memory_order_release);
            input_audio_.discardAll();

            if (stop_.load(std::memory_order_acquire)) break;
            if (!enabled_.load(std::memory_order_acquire)
                || revision_.load(std::memory_order_acquire) != session_revision) {
                backoff_seconds = 1;
                continue;
            }
            if (clean) {
                backoff_seconds = 1;
                continue;
            }
            failures_.fetch_add(1, std::memory_order_relaxed);
            setState(AVC_NODE_DEGRADED,
                     error.empty() ? "Qwen ASR connection ended; reconnecting" : error);
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_.wait_for(lock, std::chrono::seconds(backoff_seconds),
                           [this, session_revision] {
                               return stop_.load(std::memory_order_acquire)
                                   || !enabled_.load(std::memory_order_acquire)
                                   || revision_.load(std::memory_order_acquire)
                                          != session_revision;
                           });
            backoff_seconds = std::min(backoff_seconds * 2U, 30U);
        }
        configured_.store(false, std::memory_order_release);
        setState(AVC_NODE_OFFLINE, "Qwen speech recognition stopped");
    }

    std::uint32_t sample_rate_ = 48000;
    std::string endpoint_;
    std::atomic<bool> enabled_{true};
    std::atomic<std::uint32_t> language_{0};
    std::atomic<float> vad_threshold_{0.0F};
    std::atomic<std::uint32_t> silence_ms_{400};

    avc::qwen::PcmSpscBuffer input_audio_;
    avc::qwen::FloatToPcm16Resampler input_resampler_;
    std::vector<std::int16_t> converted_input_;
    std::vector<float> silent_input_;

    const std::uint64_t text_stream_;
    std::uint64_t text_segment_ = 1;
    std::uint64_t text_revision_ = 0;
    FixedSnapshot<avc::sdk::kTextPortTypeBytes - sizeof(std::uint32_t)> transcript_;
    FixedSnapshot<256> status_message_;
    std::array<char, avc::sdk::kTextPortTypeBytes - sizeof(std::uint32_t)> text_cache_{};

    std::thread worker_;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> configured_{false};
    std::atomic<std::uint64_t> revision_{0};
    std::atomic<std::uint64_t> input_epoch_{0};
    std::atomic<AvcNodeState> state_{AVC_NODE_OFFLINE};
    std::atomic<std::uint64_t> processed_chunks_{0};
    std::atomic<std::uint64_t> dropped_chunks_{0};
    std::atomic<std::uint64_t> failures_{0};
};

class QwenTtsNode final : public avc::sdk::Node {
public:
    enum Param : std::uint32_t {
        kEnabled,
        kLanguage,
        kSpeechRate,
        kVolume,
        kPitchRate,
        kOptimizeInstructions,
        kJitter,
        kVoice,
        kInstructions,
    };

    QwenTtsNode()
    {
        status_message_.publish("Waiting for the audio engine");
    }

    ~QwenTtsNode() override
    {
        stop_.store(true, std::memory_order_release);
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool prepare(const AvcPrepareInfo &info, std::string &error) override
    {
        if (info.sample_rate < 8000 || info.sample_rate > 384000) {
            error = "unsupported engine sample rate";
            return false;
        }
        sample_rate_ = info.sample_rate;
        output_resampler_.reset(avc::qwen::kOutputSampleRate, sample_rate_);
        output_audio_.reset(avc::qwen::kOutputSampleRate * 32U);
        text_queue_.reset();
        seen_text_ = false;
        previous_size_ = 0;

        endpoint_ = avc::qwen::buildEndpoint(g_config.region, g_config.workspace_id,
                                             g_config.tts_endpoint, g_config.tts_model,
                                             error);
        if (endpoint_.empty()) return false;
        std::string validation_error;
        if (avc::qwen::makeTtsSessionUpdate(sessionOptions(), validation_error).empty()) {
            error = validation_error;
            return false;
        }

        stop_.store(false, std::memory_order_release);
        worker_ = std::thread([this] { workerMain(); });
        return true;
    }

    std::uint32_t latencyFrames() const override
    {
        const float jitter = jitter_ms_.load(std::memory_order_relaxed);
        return static_cast<std::uint32_t>(
            std::lround((600.0F + jitter) * static_cast<float>(sample_rate_) / 1000.0F));
    }

    void setParam(std::uint32_t index, float value) noexcept override
    {
        bool reconnect = true;
        switch (index) {
        case kEnabled: enabled_.store(value >= 0.5F, std::memory_order_release); break;
        case kLanguage:
            language_.store(static_cast<std::uint32_t>(std::lround(value)),
                            std::memory_order_release);
            break;
        case kSpeechRate: speech_rate_.store(value, std::memory_order_release); break;
        case kVolume:
            volume_.store(static_cast<std::uint32_t>(std::lround(value)),
                          std::memory_order_release);
            break;
        case kPitchRate: pitch_rate_.store(value, std::memory_order_release); break;
        case kOptimizeInstructions:
            optimize_instructions_.store(value >= 0.5F, std::memory_order_release);
            break;
        case kJitter:
            jitter_ms_.store(value, std::memory_order_release);
            reconnect = false;
            break;
        default: return;
        }
        if (reconnect) {
            revision_.fetch_add(1, std::memory_order_acq_rel);
            output_epoch_.fetch_add(1, std::memory_order_acq_rel);
            wake_.notify_all();
        }
    }

    void setOption(std::uint32_t index, std::string_view value) override
    {
        {
            const std::lock_guard<std::mutex> lock(options_mutex_);
            if (index == kVoice) voice_.assign(value);
            else if (index == kInstructions) instructions_.assign(value);
            else return;
        }
        revision_.fetch_add(1, std::memory_order_acq_rel);
        output_epoch_.fetch_add(1, std::memory_order_acq_rel);
        wake_.notify_all();
    }

    bool status(AvcNodeStatus &out) const noexcept override
    {
        out.state = state_.load(std::memory_order_relaxed);
        out.progress = out.state == AVC_NODE_READY ? 1.0F : 0.0F;
        out.processed_blocks = submitted_texts_.load(std::memory_order_relaxed);
        out.bypassed_blocks = dropped_texts_.load(std::memory_order_relaxed);
        out.failures = failures_.load(std::memory_order_relaxed);
        status_message_.read(out.message, sizeof(out.message));
        return true;
    }

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        if (ctx.discontinuity != 0) {
            output_epoch_.fetch_add(1, std::memory_order_acq_rel);
        }

        const void *input_block = ctx.n_inputs > 0 && ctx.in_blocks != nullptr
                                      ? ctx.in_blocks[0]
                                      : nullptr;
        const avc::sdk::TextFrame frame = avc::sdk::readTextFrame(input_block);
        if (frame.valid) acceptText(frame);

        float *output = ctx.n_outputs > 0 ? ctx.outputs[0] : nullptr;
        if (output == nullptr) return;
        if (!enabled_.load(std::memory_order_acquire)) {
            std::memset(output, 0, ctx.nframes * sizeof(float));
            return;
        }
        renderAudio(output, ctx.nframes);
    }

private:
    void acceptText(const avc::sdk::TextFrame &frame) noexcept
    {
        bool changed = !seen_text_ || frame.segmented != previous_segmented_;
        if (!changed && frame.segmented) {
            changed = frame.stream != previous_stream_ || frame.segment != previous_segment_
                   || frame.revision != previous_revision_ || frame.final != previous_final_;
        } else if (!changed) {
            changed = frame.value.size() != previous_size_
                   || (previous_size_ > 0
                       && std::memcmp(previous_text_.data(), frame.value.data(),
                                      previous_size_)
                              != 0);
        }
        if (!changed) return;

        seen_text_ = true;
        previous_segmented_ = frame.segmented;
        previous_stream_ = frame.stream;
        previous_segment_ = frame.segment;
        previous_revision_ = frame.revision;
        previous_final_ = frame.final;
        previous_size_ = std::min(frame.value.size(), previous_text_.size());
        if (previous_size_ > 0) {
            std::memcpy(previous_text_.data(), frame.value.data(), previous_size_);
        }

        if (!enabled_.load(std::memory_order_acquire) || frame.value.empty()) return;
        // Streaming text is revised in place. Synthesizing only its final revision
        // prevents repeated or corrected words from being spoken multiple times.
        if (frame.segmented && !frame.final) return;
        if (!text_queue_.push(frame.value)) {
            dropped_texts_.fetch_add(1, std::memory_order_relaxed);
            failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    avc::qwen::TtsSessionOptions sessionOptions() const
    {
        avc::qwen::TtsSessionOptions options;
        const std::size_t index = std::min<std::size_t>(
            language_.load(std::memory_order_acquire),
            avc::qwen::ttsLanguages().size() - 1);
        options.language = avc::qwen::ttsLanguages()[index];
        options.speech_rate = speech_rate_.load(std::memory_order_acquire);
        options.volume = volume_.load(std::memory_order_acquire);
        options.pitch_rate = pitch_rate_.load(std::memory_order_acquire);
        options.optimize_instructions =
            optimize_instructions_.load(std::memory_order_acquire);
        {
            const std::lock_guard<std::mutex> lock(options_mutex_);
            options.voice = voice_.empty() ? g_config.default_tts_voice : voice_;
            options.instructions = instructions_;
        }
        return options;
    }

    void renderAudio(float *output, std::uint32_t frames) noexcept
    {
        const std::uint64_t epoch = output_epoch_.load(std::memory_order_acquire);
        if (epoch != seen_output_epoch_) {
            seen_output_epoch_ = epoch;
            output_audio_.discardAll();
            output_resampler_.reset(avc::qwen::kOutputSampleRate, sample_rate_);
            playing_ = false;
            flush_audio_.store(false, std::memory_order_release);
        }

        const std::size_t jitter = static_cast<std::size_t>(
            std::max(0.0F, jitter_ms_.load(std::memory_order_relaxed))
            * static_cast<float>(avc::qwen::kOutputSampleRate) / 1000.0F);
        const std::size_t ready = output_audio_.readable();
        if (!playing_ && (ready >= std::max<std::size_t>(jitter, 2)
                          || (flush_audio_.load(std::memory_order_acquire) && ready >= 2))) {
            output_resampler_.reset(avc::qwen::kOutputSampleRate, sample_rate_);
            playing_ = true;
            flush_audio_.store(false, std::memory_order_release);
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            float sample = 0.0F;
            if (playing_) {
                const bool sample_ready = output_resampler_.pull(
                    [this](std::int16_t &value) { return output_audio_.pop(value); }, sample);
                if (!sample_ready) {
                    playing_ = false;
                    sample = 0.0F;
                }
            }
            output[i] = sample;
        }
    }

    void setState(AvcNodeState state, std::string_view message) noexcept
    {
        state_.store(state, std::memory_order_release);
        status_message_.publish(message);
    }

    bool runSession(std::uint64_t session_revision, std::string &error)
    {
        WebSocketConnection connection;
        setState(AVC_NODE_LOADING, "Connecting to Qwen speech synthesis");
        if (!connection.connect(endpoint_, g_config.api_key, "avc-qwen-tts/1.0", error)) {
            return false;
        }

        std::string protocol_error;
        const std::string update =
            avc::qwen::makeTtsSessionUpdate(sessionOptions(), protocol_error);
        if (update.empty()) {
            error = protocol_error;
            return false;
        }
        if (!connection.send(update, error)) return false;
        setState(AVC_NODE_LOADING, "Connected; waiting for TTS session.updated");

        std::string frame;
        frame.reserve(65536);
        std::string text;
        bool in_flight = false;
        bool finishing = false;
        bool finished = false;
        auto finish_deadline = std::chrono::steady_clock::time_point::max();

        while (true) {
            const bool received = connection.receiveAvailable(
                frame,
                [this, &finished, &in_flight](const avc::qwen::ServerEvent &event,
                                              std::string &handler_error) {
                    switch (event.kind) {
                    case avc::qwen::ServerEventKind::SessionUpdated:
                        configured_.store(true, std::memory_order_release);
                        setState(AVC_NODE_READY, "Connected to Qwen speech synthesis");
                        break;
                    case avc::qwen::ServerEventKind::SessionFinished:
                        finished = true;
                        break;
                    case avc::qwen::ServerEventKind::ResponseDone:
                        in_flight = false;
                        flush_audio_.store(true, std::memory_order_release);
                        break;
                    case avc::qwen::ServerEventKind::Audio: {
                        const std::size_t written = output_audio_.write(event.audio);
                        if (written != event.audio.size()) {
                            failures_.fetch_add(1, std::memory_order_relaxed);
                            setState(AVC_NODE_DEGRADED, "TTS audio jitter buffer overflow");
                        }
                        break;
                    }
                    case avc::qwen::ServerEventKind::Error:
                    case avc::qwen::ServerEventKind::Invalid:
                        handler_error = event.error;
                        return false;
                    default: break;
                    }
                    return true;
                },
                error);
            if (!received) return false;
            if (finished) return true;

            const bool should_finish = stop_.load(std::memory_order_acquire)
                                    || !enabled_.load(std::memory_order_acquire)
                                    || revision_.load(std::memory_order_acquire)
                                           != session_revision;
            if (should_finish && !finishing) {
                configured_.store(false, std::memory_order_release);
                if (!connection.send(avc::qwen::makeSessionFinish(), error)) return false;
                finishing = true;
                finish_deadline = std::chrono::steady_clock::now() + 5s;
            }
            if (finishing) {
                if (std::chrono::steady_clock::now() >= finish_deadline) return true;
                std::this_thread::sleep_for(5ms);
                continue;
            }

            if (configured_.load(std::memory_order_acquire) && !in_flight
                && text_queue_.pop(text)) {
                if (!text.empty()) {
                    if (!connection.send(avc::qwen::makeTextAppend(text), error)
                        || !connection.send(avc::qwen::makeTextCommit(), error)) {
                        return false;
                    }
                    in_flight = true;
                    submitted_texts_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_.wait_for(lock, 5ms, [this, session_revision] {
                return stop_.load(std::memory_order_acquire)
                    || !enabled_.load(std::memory_order_acquire)
                    || revision_.load(std::memory_order_acquire) != session_revision;
            });
        }
    }

    void workerMain()
    {
        unsigned backoff_seconds = 1;
        while (!stop_.load(std::memory_order_acquire)) {
            if (!enabled_.load(std::memory_order_acquire)) {
                configured_.store(false, std::memory_order_release);
                text_queue_.discardAll();
                setState(AVC_NODE_OFFLINE, "Qwen speech synthesis is disabled");
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_.wait_for(lock, 250ms, [this] {
                    return stop_.load(std::memory_order_acquire)
                        || enabled_.load(std::memory_order_acquire);
                });
                continue;
            }
            if (!g_curl_ready) {
                setState(AVC_NODE_ERROR, "libcurl initialization failed");
                failures_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (g_config.api_key.empty()) {
                setState(AVC_NODE_ERROR, "Configure the API key in Qwen extension settings");
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_.wait_for(lock, 1s, [this] {
                    return stop_.load(std::memory_order_acquire)
                        || !enabled_.load(std::memory_order_acquire);
                });
                continue;
            }

            const std::uint64_t session_revision = revision_.load(std::memory_order_acquire);
            std::string error;
            const bool clean = runSession(session_revision, error);
            configured_.store(false, std::memory_order_release);
            output_epoch_.fetch_add(1, std::memory_order_acq_rel);

            if (stop_.load(std::memory_order_acquire)) break;
            if (!enabled_.load(std::memory_order_acquire)
                || revision_.load(std::memory_order_acquire) != session_revision) {
                backoff_seconds = 1;
                continue;
            }
            if (clean) {
                backoff_seconds = 1;
                continue;
            }
            failures_.fetch_add(1, std::memory_order_relaxed);
            setState(AVC_NODE_DEGRADED,
                     error.empty() ? "Qwen TTS connection ended; reconnecting" : error);
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_.wait_for(lock, std::chrono::seconds(backoff_seconds),
                           [this, session_revision] {
                               return stop_.load(std::memory_order_acquire)
                                   || !enabled_.load(std::memory_order_acquire)
                                   || revision_.load(std::memory_order_acquire)
                                          != session_revision;
                           });
            backoff_seconds = std::min(backoff_seconds * 2U, 30U);
        }
        configured_.store(false, std::memory_order_release);
        setState(AVC_NODE_OFFLINE, "Qwen speech synthesis stopped");
    }

    std::uint32_t sample_rate_ = 48000;
    std::string endpoint_;
    std::atomic<bool> enabled_{true};
    std::atomic<std::uint32_t> language_{0};
    std::atomic<float> speech_rate_{1.0F};
    std::atomic<std::uint32_t> volume_{50};
    std::atomic<float> pitch_rate_{1.0F};
    std::atomic<bool> optimize_instructions_{false};
    std::atomic<float> jitter_ms_{160.0F};

    mutable std::mutex options_mutex_;
    std::string voice_;
    std::string instructions_;

    TextSpscQueue text_queue_;
    avc::qwen::PcmSpscBuffer output_audio_;
    avc::qwen::Pcm16ToFloatResampler output_resampler_;
    bool playing_ = false;

    std::array<char, avc::text::kPlainPayloadBytes> previous_text_{};
    std::size_t previous_size_ = 0;
    std::uint64_t previous_stream_ = 0;
    std::uint64_t previous_segment_ = 0;
    std::uint64_t previous_revision_ = 0;
    bool previous_segmented_ = false;
    bool previous_final_ = false;
    bool seen_text_ = false;

    FixedSnapshot<256> status_message_;
    std::thread worker_;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> configured_{false};
    std::atomic<std::uint64_t> revision_{0};
    std::atomic<std::uint64_t> output_epoch_{0};
    std::atomic<bool> flush_audio_{false};
    std::uint64_t seen_output_epoch_ = 0;
    std::atomic<AvcNodeState> state_{AVC_NODE_OFFLINE};
    std::atomic<std::uint64_t> submitted_texts_{0};
    std::atomic<std::uint64_t> dropped_texts_{0};
    std::atomic<std::uint64_t> failures_{0};
};

}

void avc::qwen::detail::registerSpeechNodes(avc::sdk::Plugin &plugin)
{
    plugin.node<QwenAsrNode>(
        avc::sdk::NodeDesc("stt", "speech", "Qwen 语音转文本")
            .in("audio")
            .out("transcript", avc::sdk::kTextPortType)
            .boolParam("enabled", true)
            .enumParam("language", avc::qwen::asrLanguages(), 0,
                       "auto = 自动检测。")
            .floatParam("vad_threshold", -1.0F, 1.0F, 0.0F, {}, AVC_CURVE_LINEAR,
                         "VAD 灵敏度。")
            .floatParam("silence_duration_ms", 200.0F, 6000.0F, 400.0F, "ms",
                         AVC_CURVE_LINEAR, "结束语段所需的静音。"));

    plugin.node<QwenTtsNode>(
        avc::sdk::NodeDesc("tts", "speech", "Qwen 文本转语音")
            .in("text", avc::sdk::kTextPortType)
            .out("audio")
            .boolParam("enabled", true)
            .enumParam("language", avc::qwen::ttsLanguages(), 0,
                        "Auto = 自动检测。")
            .floatParam("speech_rate", 0.5F, 2.0F, 1.0F)
            .floatParam("volume", 0.0F, 100.0F, 50.0F)
            .floatParam("pitch_rate", 0.5F, 2.0F, 1.0F)
            .boolParam("optimize_instructions", false,
                       "为 Qwen3-TTS-Instruct 模型优化指令。")
            .floatParam("jitter_ms", 40.0F, 2000.0F, 160.0F, "ms",
                         AVC_CURVE_LINEAR, "播放前的网络抖动缓冲。")
            .textParam("voice", {}, "音色 ID；留空使用扩展默认值。")
            .textParam("instructions", {},
                        "仅 Qwen3-TTS-Instruct 支持。"));
}
