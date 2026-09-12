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
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace avc::qwen::detail {
ExtensionConfig g_config;
bool g_curl_ready = false;
}

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
    // Event ids already combine a monotonic timestamp and process-local counter.
    // Hashing keeps that identity compact enough for the text-port footer.
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

class QwenLiveTranslateNode final : public avc::sdk::Node {
public:
    enum Param : std::uint32_t {
        kEnabled,
        kTargetLanguage,
        kSourceLanguage,
        kAudioOutput,
        kSourceTranscript,
        kVadThreshold,
        kSilenceDuration,
        kVoiceClone,
        kJitter,
        kVoice,
    };

    QwenLiveTranslateNode()
        : translation_stream_(makeTextStreamId()), source_stream_(makeTextStreamId())
    {
        status_message_.publish("Waiting for the audio engine");
    }

    ~QwenLiveTranslateNode() override
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
        output_resampler_.reset(avc::qwen::kOutputSampleRate, sample_rate_);

        const std::size_t input_capacity = avc::qwen::kInputSampleRate * 16U;
        const std::size_t output_capacity = avc::qwen::kOutputSampleRate * 32U;
        input_audio_.reset(input_capacity);
        output_audio_.reset(output_capacity);
        const std::size_t converted =
            (static_cast<std::size_t>(info.max_quantum) * avc::qwen::kInputSampleRate
             + sample_rate_ - 1)
                / sample_rate_
            + 4;
        converted_input_.assign(converted, 0);
        silent_input_.assign(info.max_quantum, 0.0F);

        endpoint_ = avc::qwen::buildEndpoint(g_config.region, g_config.workspace_id,
                                             g_config.endpoint,
                                             g_config.model, error);
        if (endpoint_.empty()) return false;

        avc::qwen::SessionOptions options = sessionOptions();
        std::string validation_error;
        if (avc::qwen::makeSessionUpdate(options, validation_error).empty()) {
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
            std::lround((2800.0F + jitter) * static_cast<float>(sample_rate_) / 1000.0F));
    }

    void setParam(std::uint32_t index, float value) noexcept override
    {
        bool reconnect = false;
        switch (index) {
        case kEnabled:
            enabled_.store(value >= 0.5F, std::memory_order_release);
            reconnect = true;
            break;
        case kTargetLanguage:
            target_language_.store(static_cast<std::uint32_t>(std::lround(value)),
                                   std::memory_order_release);
            reconnect = true;
            break;
        case kSourceLanguage:
            source_language_.store(static_cast<std::uint32_t>(std::lround(value)),
                                   std::memory_order_release);
            reconnect = true;
            break;
        case kAudioOutput:
            audio_output_.store(value >= 0.5F, std::memory_order_release);
            reconnect = true;
            break;
        case kSourceTranscript:
            source_transcript_.store(value >= 0.5F, std::memory_order_release);
            reconnect = true;
            break;
        case kVadThreshold:
            vad_threshold_.store(value, std::memory_order_release);
            reconnect = true;
            break;
        case kSilenceDuration:
            silence_ms_.store(static_cast<std::uint32_t>(std::lround(value)),
                              std::memory_order_release);
            reconnect = true;
            break;
        case kVoiceClone:
            voice_clone_.store(static_cast<std::uint32_t>(std::lround(value)),
                               std::memory_order_release);
            reconnect = true;
            break;
        case kJitter:
            jitter_ms_.store(value, std::memory_order_release);
            break;
        default: break;
        }
        if (reconnect) {
            revision_.fetch_add(1, std::memory_order_acq_rel);
            wake_.notify_all();
        }
    }

    void setOption(std::uint32_t index, std::string_view value) override
    {
        if (index != kVoice) return;
        {
            const std::lock_guard<std::mutex> lock(options_mutex_);
            voice_.assign(value);
        }
        revision_.fetch_add(1, std::memory_order_acq_rel);
        wake_.notify_all();
    }

    bool status(AvcNodeStatus &out) const noexcept override
    {
        out.state = state_.load(std::memory_order_relaxed);
        out.progress = out.state == AVC_NODE_READY ? 1.0F : 0.0F;
        out.processed_blocks = processed_chunks_.load(std::memory_order_relaxed);
        out.bypassed_blocks = bypassed_blocks_.load(std::memory_order_relaxed);
        out.failures = failures_.load(std::memory_order_relaxed);
        status_message_.read(out.message, sizeof(out.message));
        return true;
    }

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        const bool enabled = enabled_.load(std::memory_order_acquire);
        const bool configured = configured_.load(std::memory_order_acquire);
        const float *input = ctx.n_inputs > 0 ? ctx.inputs[0] : nullptr;
        float *output = ctx.n_outputs > 0 ? ctx.outputs[0] : nullptr;

        if (ctx.discontinuity != 0) {
            input_resampler_.reset(sample_rate_, avc::qwen::kInputSampleRate);
            input_epoch_.fetch_add(1, std::memory_order_acq_rel);
        }

        if (enabled && configured) {
            const std::span<const float> source(
                input != nullptr ? input : silent_input_.data(), ctx.nframes);
            const std::size_t count = input_resampler_.process(source, converted_input_);
            const std::size_t written = input_audio_.write(
                std::span<const std::int16_t>(converted_input_.data(), count));
            if (written != count) failures_.fetch_add(1, std::memory_order_relaxed);
        }

        if (output != nullptr) {
            if (!enabled) {
                if (input != nullptr) {
                    std::memcpy(output, input, ctx.nframes * sizeof(float));
                } else {
                    std::memset(output, 0, ctx.nframes * sizeof(float));
                }
                bypassed_blocks_.fetch_add(1, std::memory_order_relaxed);
            } else {
                renderTranslation(output, ctx.nframes);
            }
        }

        std::uint64_t translation_stream = 0;
        std::uint64_t translation_segment = 0;
        std::uint64_t translation_revision = 0;
        bool translation_final = false;
        translation_size_ = translation_.read(
            translation_cache_.data(), translation_cache_.size(), &translation_stream,
            &translation_segment, &translation_revision, &translation_final);
        std::uint64_t source_stream = 0;
        std::uint64_t source_segment = 0;
        std::uint64_t source_revision = 0;
        bool source_final = false;
        source_size_ = source_text_.read(source_cache_.data(), source_cache_.size(),
                                         &source_stream, &source_segment,
                                         &source_revision, &source_final);
        if (ctx.n_outputs > 1) {
            const std::string_view value{translation_cache_.data(), translation_size_};
            if (translation_stream != 0) {
                avc::sdk::writeTextFrame(ctx.out_blocks[1], value, translation_stream,
                                         translation_segment, translation_revision,
                                         translation_final);
            } else {
                avc::sdk::writeText(ctx.out_blocks[1], value);
            }
        }
        if (ctx.n_outputs > 2) {
            const std::string_view value{source_cache_.data(), source_size_};
            if (source_stream != 0) {
                avc::sdk::writeTextFrame(ctx.out_blocks[2], value, source_stream,
                                         source_segment, source_revision, source_final);
            } else {
                avc::sdk::writeText(ctx.out_blocks[2], value);
            }
        }
    }

private:
    avc::qwen::SessionOptions sessionOptions() const
    {
        avc::qwen::SessionOptions options;
        const auto target = std::min<std::size_t>(
            target_language_.load(std::memory_order_acquire), avc::qwen::languages().size() - 1);
        const auto source = std::min<std::size_t>(
            source_language_.load(std::memory_order_acquire),
            avc::qwen::sourceLanguages().size() - 1);
        options.target_language = avc::qwen::languages()[target];
        options.source_language = avc::qwen::sourceLanguages()[source];
        options.audio_output = audio_output_.load(std::memory_order_acquire);
        options.source_transcription = source_transcript_.load(std::memory_order_acquire);
        options.vad_threshold = vad_threshold_.load(std::memory_order_acquire);
        options.silence_duration_ms = silence_ms_.load(std::memory_order_acquire);
        options.hotwords_json = g_config.hotwords_json;
        options.voice_clone = static_cast<avc::qwen::VoiceCloneMode>(
            std::min(voice_clone_.load(std::memory_order_acquire), 3U));
        {
            const std::lock_guard<std::mutex> lock(options_mutex_);
            options.voice = voice_.empty() ? g_config.default_voice : voice_;
        }
        return options;
    }

    void renderTranslation(float *output, std::uint32_t frames) noexcept
    {
        const std::uint64_t epoch = output_epoch_.load(std::memory_order_acquire);
        if (epoch != seen_output_epoch_) {
            seen_output_epoch_ = epoch;
            output_audio_.discardAll();
            output_resampler_.reset(avc::qwen::kOutputSampleRate, sample_rate_);
            playing_ = false;
        }

        const std::size_t jitter = static_cast<std::size_t>(
            std::max(0.0F, jitter_ms_.load(std::memory_order_relaxed))
            * static_cast<float>(avc::qwen::kOutputSampleRate) / 1000.0F);
        if (!playing_ && output_audio_.readable() >= std::max<std::size_t>(jitter, 2)) {
            output_resampler_.reset(avc::qwen::kOutputSampleRate, sample_rate_);
            playing_ = true;
        }

        bool underrun = false;
        for (std::uint32_t i = 0; i < frames; ++i) {
            float sample = 0.0F;
            if (playing_) {
                const bool ready = output_resampler_.pull(
                    [this](std::int16_t &value) { return output_audio_.pop(value); }, sample);
                if (!ready) {
                    playing_ = false;
                    underrun = true;
                    sample = 0.0F;
                }
            }
            output[i] = sample;
        }
        if (underrun) bypassed_blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    void setState(AvcNodeState state, std::string_view message) noexcept
    {
        state_.store(state, std::memory_order_release);
        status_message_.publish(message);
    }

    void beginTextSession() noexcept
    {
        // A reconnect may abandon a partial utterance. Move the next update to a
        // fresh segment so the UI keeps the interrupted text instead of rewriting it.
        if (translation_revision_ != 0) {
            ++translation_segment_;
            translation_revision_ = 0;
        }
        if (source_revision_ != 0) {
            ++source_segment_;
            source_revision_ = 0;
        }
    }

    void publishTranslation(std::string_view value, bool final) noexcept
    {
        translation_.publish(value, translation_stream_, translation_segment_,
                             ++translation_revision_, final);
        if (final) {
            ++translation_segment_;
            translation_revision_ = 0;
        }
    }

    void publishSource(std::string_view value, bool final) noexcept
    {
        source_text_.publish(value, source_stream_, source_segment_,
                             ++source_revision_, final);
        if (final) {
            ++source_segment_;
            source_revision_ = 0;
        }
    }

    bool sendText(CURL *curl, const std::string &payload, std::string &error)
    {
        std::size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (offset < payload.size()) {
            std::size_t sent = 0;
            const CURLcode result = curl_ws_send(curl, payload.data() + offset,
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
        if (offset != payload.size()) {
            error = "WebSocket send was interrupted";
            return false;
        }
        return true;
    }

    bool receiveAvailable(CURL *curl, std::string &frame, bool &finished,
                          std::string &error)
    {
        std::array<char, 65536> buffer{};
        while (true) {
            std::size_t received = 0;
            const curl_ws_frame *meta = nullptr;
            const CURLcode result = curl_ws_recv(curl, buffer.data(), buffer.size(), &received,
                                                 &meta);
            if (result == CURLE_AGAIN) return true;
            if (result != CURLE_OK) {
                error = std::string("WebSocket receive failed: ") + curl_easy_strerror(result);
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
            switch (event.kind) {
            case avc::qwen::ServerEventKind::SessionUpdated: {
                beginTextSession();
                configured_.store(true, std::memory_order_release);
                const auto options = sessionOptions();
                const bool text_only = options.audio_output
                                    && !avc::qwen::supportsAudioOutput(options.target_language);
                setState(AVC_NODE_READY,
                         text_only ? "Connected; this target language is text-only"
                                   : "Connected to Qwen LiveTranslate");
                break;
            }
            case avc::qwen::ServerEventKind::SessionFinished:
                finished = true;
                return true;
            case avc::qwen::ServerEventKind::Audio: {
                const std::size_t written = output_audio_.write(event.audio);
                if (written != event.audio.size()) {
                    failures_.fetch_add(1, std::memory_order_relaxed);
                    setState(AVC_NODE_DEGRADED, "Translated-audio jitter buffer overflow");
                }
                break;
            }
            case avc::qwen::ServerEventKind::Translation:
                publishTranslation(event.text, false);
                break;
            case avc::qwen::ServerEventKind::TranslationDone:
                publishTranslation(event.text, true);
                break;
            case avc::qwen::ServerEventKind::SourceTranscript:
                publishSource(event.text, false);
                break;
            case avc::qwen::ServerEventKind::SourceTranscriptDone:
                publishSource(event.text, true);
                break;
            case avc::qwen::ServerEventKind::Error:
                if (event.error.starts_with("source transcription failed:")) {
                    failures_.fetch_add(1, std::memory_order_relaxed);
                    setState(AVC_NODE_DEGRADED, event.error);
                    break;
                }
                error = event.error;
                return false;
            case avc::qwen::ServerEventKind::Invalid:
                error = event.error;
                return false;
            case avc::qwen::ServerEventKind::ResponseDone:
            case avc::qwen::ServerEventKind::Ignored: break;
            }
        }
    }

    bool runSession(std::uint64_t session_revision, const std::string &api_key,
                    std::string &error)
    {
        CurlHandle handle;
        if (handle.get() == nullptr) {
            error = "curl_easy_init failed";
            return false;
        }
        CurlHeaders headers;
        if (!headers.add("Authorization: Bearer " + api_key)) {
            error = "could not allocate the authorization header";
            return false;
        }

        std::array<char, CURL_ERROR_SIZE> curl_error{};
        CURL *curl = handle.get();
        curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "avc-qwen-livetranslate/1.0");
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error.data());

        setState(AVC_NODE_LOADING, "Connecting to Qwen LiveTranslate");
        const CURLcode connected = curl_easy_perform(curl);
        if (connected != CURLE_OK) {
            error = curl_error[0] != '\0' ? curl_error.data() : curl_easy_strerror(connected);
            return false;
        }

        std::string protocol_error;
        const std::string update = avc::qwen::makeSessionUpdate(sessionOptions(), protocol_error);
        if (update.empty()) {
            error = protocol_error;
            return false;
        }
        if (!sendText(curl, update, error)) return false;
        setState(AVC_NODE_LOADING, "Connected; waiting for session.updated");

        std::array<std::int16_t, avc::qwen::kInputChunkSamples> input_chunk{};
        std::string frame;
        frame.reserve(65536);
        bool finishing = false;
        bool finished = false;
        auto finish_deadline = std::chrono::steady_clock::time_point::max();
        std::uint64_t seen_input_epoch = input_epoch_.load(std::memory_order_acquire);

        while (true) {
            if (!receiveAvailable(curl, frame, finished, error)) return false;
            if (finished) return true;

            const bool should_finish = stop_.load(std::memory_order_acquire)
                                    || !enabled_.load(std::memory_order_acquire)
                                    || revision_.load(std::memory_order_acquire)
                                           != session_revision;
            if (should_finish && !finishing) {
                configured_.store(false, std::memory_order_release);
                if (!sendText(curl, avc::qwen::makeSessionFinish(), error)) return false;
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
                    if (!sendText(curl, avc::qwen::makeAudioAppend(input_chunk), error)) {
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
                setState(AVC_NODE_OFFLINE, "Qwen translation is disabled");
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
            const bool clean = runSession(session_revision, g_config.api_key, error);
            configured_.store(false, std::memory_order_release);
            output_epoch_.fetch_add(1, std::memory_order_acq_rel);
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
                     error.empty() ? "Qwen connection ended; reconnecting" : error);
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
        setState(AVC_NODE_OFFLINE, "Qwen translation stopped");
    }

    std::uint32_t sample_rate_ = 48000;
    std::string endpoint_;

    std::atomic<bool> enabled_{true};
    std::atomic<std::uint32_t> target_language_{1}; // en
    std::atomic<std::uint32_t> source_language_{0}; // auto
    std::atomic<bool> audio_output_{true};
    std::atomic<bool> source_transcript_{true};
    std::atomic<float> vad_threshold_{0.2F};
    std::atomic<std::uint32_t> silence_ms_{1000};
    std::atomic<std::uint32_t> voice_clone_{0};
    std::atomic<float> jitter_ms_{240.0F};

    mutable std::mutex options_mutex_;
    std::string voice_;

    avc::qwen::PcmSpscBuffer input_audio_;
    avc::qwen::PcmSpscBuffer output_audio_;
    avc::qwen::FloatToPcm16Resampler input_resampler_;
    avc::qwen::Pcm16ToFloatResampler output_resampler_;
    std::vector<std::int16_t> converted_input_;
    std::vector<float> silent_input_;
    bool playing_ = false;

    const std::uint64_t translation_stream_;
    const std::uint64_t source_stream_;
    std::uint64_t translation_segment_ = 1;
    std::uint64_t source_segment_ = 1;
    std::uint64_t translation_revision_ = 0;
    std::uint64_t source_revision_ = 0;
    FixedSnapshot<avc::sdk::kTextPortTypeBytes - sizeof(std::uint32_t)> translation_;
    FixedSnapshot<avc::sdk::kTextPortTypeBytes - sizeof(std::uint32_t)> source_text_;
    FixedSnapshot<256> status_message_;
    std::array<char, avc::sdk::kTextPortTypeBytes - sizeof(std::uint32_t)> translation_cache_{};
    std::array<char, avc::sdk::kTextPortTypeBytes - sizeof(std::uint32_t)> source_cache_{};
    std::size_t translation_size_ = 0;
    std::size_t source_size_ = 0;

    std::thread worker_;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> configured_{false};
    std::atomic<std::uint64_t> revision_{0};
    std::atomic<std::uint64_t> input_epoch_{0};
    std::atomic<std::uint64_t> output_epoch_{0};
    std::uint64_t seen_output_epoch_ = 0;
    std::atomic<AvcNodeState> state_{AVC_NODE_OFFLINE};
    std::atomic<std::uint64_t> processed_chunks_{0};
    std::atomic<std::uint64_t> bypassed_blocks_{0};
    std::atomic<std::uint64_t> failures_{0};
};

avc::sdk::Plugin plugin("qwen-livetranslate", "Qwen 语音", "1.1.0");

void configure(const char *key, const char *value)
{
    if (key == nullptr || value == nullptr) return;
    if (std::strcmp(key, "region") == 0) g_config.region = value;
    else if (std::strcmp(key, "api_key") == 0) g_config.api_key = value;
    else if (std::strcmp(key, "workspace_id") == 0) g_config.workspace_id = value;
    else if (std::strcmp(key, "endpoint") == 0) g_config.endpoint = value;
    else if (std::strcmp(key, "model") == 0) g_config.model = value;
    else if (std::strcmp(key, "default_voice") == 0) g_config.default_voice = value;
    else if (std::strcmp(key, "hotwords_json") == 0) g_config.hotwords_json = value;
    else if (std::strcmp(key, "asr_endpoint") == 0) g_config.asr_endpoint = value;
    else if (std::strcmp(key, "asr_model") == 0) g_config.asr_model = value;
    else if (std::strcmp(key, "tts_endpoint") == 0) g_config.tts_endpoint = value;
    else if (std::strcmp(key, "tts_model") == 0) g_config.tts_model = value;
    else if (std::strcmp(key, "default_tts_voice") == 0) {
        g_config.default_tts_voice = value;
    }
}

void shutdown()
{
    if (g_curl_ready) curl_global_cleanup();
    g_curl_ready = false;
}

const char *const kPreset = R"JSON({
  "version": 1,
  "nodes": [
    { "id": "mic", "type": "capture", "params": { "source": "@default_source" }, "outputs": 1, "ui": { "x": 40, "y": 100 } },
    { "id": "qwen", "type": "qwen-livetranslate.interpreter", "params": { "target_language": 1, "source_language": 0, "audio_output": 1 }, "ui": { "x": 320, "y": 100 } },
    { "id": "speaker", "type": "playback", "params": { "device": "@default_sink" }, "inputs": 1, "ui": { "x": 650, "y": 100 } }
  ],
  "edges": [
    { "from": { "node": "mic", "port": "out_1" }, "to": { "node": "qwen", "port": "in" } },
    { "from": { "node": "qwen", "port": "translated_audio" }, "to": { "node": "speaker", "port": "in_1" } }
  ]
})JSON";

const char *const kSpeechPreset = R"JSON({
  "version": 1,
  "nodes": [
    { "id": "mic", "type": "capture", "params": { "source": "@default_source" }, "outputs": 1, "ui": { "x": 40, "y": 100 } },
    { "id": "stt", "type": "qwen-livetranslate.stt", "params": { "language": 0 }, "ui": { "x": 300, "y": 100 } },
    { "id": "tts", "type": "qwen-livetranslate.tts", "params": { "language": 0 }, "ui": { "x": 560, "y": 100 } },
    { "id": "speaker", "type": "playback", "params": { "device": "@default_sink" }, "inputs": 1, "ui": { "x": 830, "y": 100 } },
    { "id": "text", "type": "text", "ui": { "x": 560, "y": 300 } }
  ],
  "edges": [
    { "from": { "node": "mic", "port": "out_1" }, "to": { "node": "stt", "port": "audio" } },
    { "from": { "node": "stt", "port": "transcript" }, "to": { "node": "tts", "port": "text" } },
    { "from": { "node": "stt", "port": "transcript" }, "to": { "node": "text", "port": "in" } },
    { "from": { "node": "tts", "port": "audio" }, "to": { "node": "speaker", "port": "in_1" } }
  ]
})JSON";

const char *const kSettingsModule = R"QWENJS(
const text = (tag, value, className = '') => {
  const element = document.createElement(tag)
  element.textContent = value
  if (className) element.className = className
  return element
}

class QwenLiveTranslateSettings extends HTMLElement {
  connectedCallback() {
    const context = this.avcContext
    const values = context.settings.get()
    const panel = document.createElement('section')
    panel.className = 'panel'
    panel.append(text('h2', 'Qwen', 'panel__title'))

    const field = (labelText, input, hint = '') => {
      const label = document.createElement('label')
      label.className = 'field'
      label.append(text('span', labelText, 'field__name'), input)
      if (hint) label.append(text('span', hint, 'hint'))
      return label
    }

    this.apiKey = document.createElement('input')
    this.apiKey.type = 'password'
    this.apiKey.autocomplete = 'off'
    this.apiKey.spellcheck = false
    this.apiKey.value = values.api_key ?? ''
    this.apiKey.placeholder = 'sk-...'
    panel.append(field('API Key', this.apiKey, '配置文件中明文保存。'))

    const reveal = document.createElement('label')
    reveal.className = 'field--check'
    const revealInput = document.createElement('input')
    revealInput.type = 'checkbox'
    revealInput.addEventListener('change', () => {
      this.apiKey.type = revealInput.checked ? 'text' : 'password'
    })
    reveal.append(revealInput, text('span', '显示 API Key'))
    panel.append(reveal)

    this.region = document.createElement('select')
    for (const option of ['cn-beijing', 'ap-southeast-1']) {
      const item = document.createElement('option')
      item.value = option
      item.textContent = option
      item.selected = option === (values.region ?? 'cn-beijing')
      this.region.append(item)
    }
    panel.append(field('服务地域', this.region, '需与 API Key 和 Workspace ID 匹配。'))

    this.workspaceId = document.createElement('input')
    this.workspaceId.value = values.workspace_id ?? ''
    this.workspaceId.spellcheck = false
    panel.append(field('业务空间 ID', this.workspaceId, '留空使用兼容域名。'))

    this.endpoint = document.createElement('input')
    this.endpoint.value = values.endpoint ?? ''
    this.endpoint.spellcheck = false
    this.endpoint.placeholder = '自动'
    panel.append(field('传译 WSS', this.endpoint, '仅限 wss://*.aliyuncs.com/…'))

    this.model = document.createElement('input')
    this.model.value = values.model ?? 'qwen3.5-livetranslate-flash-realtime'
    this.model.spellcheck = false
    panel.append(field('传译模型', this.model))

    this.defaultVoice = document.createElement('input')
    this.defaultVoice.value = values.default_voice ?? 'Tina'
    panel.append(field('默认音色', this.defaultVoice, '节点留空时使用。'))

    this.asrEndpoint = document.createElement('input')
    this.asrEndpoint.value = values.asr_endpoint ?? ''
    this.asrEndpoint.spellcheck = false
    this.asrEndpoint.placeholder = '自动'
    panel.append(field('语音识别 WSS', this.asrEndpoint, '仅限 wss://*.aliyuncs.com/…'))

    this.asrModel = document.createElement('input')
    this.asrModel.value = values.asr_model ?? 'qwen3-asr-flash-realtime'
    this.asrModel.spellcheck = false
    panel.append(field('语音识别模型', this.asrModel))

    this.ttsEndpoint = document.createElement('input')
    this.ttsEndpoint.value = values.tts_endpoint ?? ''
    this.ttsEndpoint.spellcheck = false
    this.ttsEndpoint.placeholder = '自动'
    panel.append(field('语音合成 WSS', this.ttsEndpoint, '仅限 wss://*.aliyuncs.com/…'))

    this.ttsModel = document.createElement('input')
    this.ttsModel.value = values.tts_model ?? 'qwen3-tts-flash-realtime'
    this.ttsModel.spellcheck = false
    panel.append(field('语音合成模型', this.ttsModel,
      '也可填写 qwen3-tts-instruct-flash-realtime。'))

    this.defaultTtsVoice = document.createElement('input')
    this.defaultTtsVoice.value = values.default_tts_voice ?? 'Cherry'
    panel.append(field('默认 TTS 音色', this.defaultTtsVoice, '节点留空时使用。'))

    this.hotwords = document.createElement('textarea')
    this.hotwords.value = values.hotwords_json ?? ''
    this.hotwords.rows = 4
    this.hotwords.spellcheck = false
    this.hotwords.placeholder = '{"人工智能":"Artificial Intelligence"}'
    panel.append(field('热词 JSON', this.hotwords))

    const save = document.createElement('button')
    save.className = 'button button--primary'
    save.textContent = '保存并重启引擎'
    const status = text('p', '', 'hint')
    status.setAttribute('role', 'status')
    save.addEventListener('click', async () => {
      save.disabled = true
      status.className = 'hint'
      status.textContent = '保存中…'
      try {
        await context.settings.save({
          api_key: this.apiKey.value.trim(),
          region: this.region.value,
          workspace_id: this.workspaceId.value.trim(),
          endpoint: this.endpoint.value.trim(),
          model: this.model.value.trim(),
          default_voice: this.defaultVoice.value.trim(),
          hotwords_json: this.hotwords.value.trim(),
          asr_endpoint: this.asrEndpoint.value.trim(),
          asr_model: this.asrModel.value.trim(),
          tts_endpoint: this.ttsEndpoint.value.trim(),
          tts_model: this.ttsModel.value.trim(),
          default_tts_voice: this.defaultTtsVoice.value.trim(),
        })
        status.textContent = '已保存，重启中…'
      } catch (error) {
        status.className = 'banner banner--error'
        status.textContent = error instanceof Error ? error.message : String(error)
        save.disabled = false
      }
    })
    panel.append(save, status)
    this.replaceChildren(panel)
  }
}

export function activate(api) {
  if (!customElements.get('avc-qwen-livetranslate-settings')) {
    customElements.define('avc-qwen-livetranslate-settings', QwenLiveTranslateSettings)
  }
  api.components.registerSettings('avc-qwen-livetranslate-settings')
}
)QWENJS";

}

AVC_PLUGIN_MAIN(plugin)
{
    g_config.region = plugin.setting("region", g_config.region);
    g_config.api_key = plugin.setting("api_key", g_config.api_key);
    g_config.workspace_id = plugin.setting("workspace_id", g_config.workspace_id);
    g_config.endpoint = plugin.setting("endpoint", g_config.endpoint);
    g_config.model = plugin.setting("model", g_config.model);
    g_config.default_voice = plugin.setting("default_voice", g_config.default_voice);
    g_config.hotwords_json = plugin.setting("hotwords_json", g_config.hotwords_json);
    g_config.asr_endpoint = plugin.setting("asr_endpoint", g_config.asr_endpoint);
    g_config.asr_model = plugin.setting("asr_model", g_config.asr_model);
    g_config.tts_endpoint = plugin.setting("tts_endpoint", g_config.tts_endpoint);
    g_config.tts_model = plugin.setting("tts_model", g_config.tts_model);
    g_config.default_tts_voice =
        plugin.setting("default_tts_voice", g_config.default_tts_voice);

    g_curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    plugin.author("avc")
        .describe("Qwen 实时语音。")
        .textSetting("api_key", {}, "API Key", "配置文件中明文保存。")
        .enumSetting("region", {"cn-beijing", "ap-southeast-1"}, g_config.region,
                     "服务地域", "需与 API Key 匹配。")
        .textSetting("workspace_id", g_config.workspace_id, "业务空间 ID",
                     "留空使用兼容域名。")
        .textSetting("endpoint", g_config.endpoint, "WSS 地址覆盖",
                     "仅限 wss://*.aliyuncs.com/…")
        .textSetting("model", g_config.model, "模型")
        .textSetting("default_voice", g_config.default_voice, "默认音色",
                     "节点留空时使用。")
        .textSetting("hotwords_json", g_config.hotwords_json, "热词 JSON",
                     R"({"源词":"译词"})")
        .textSetting("asr_endpoint", g_config.asr_endpoint, "语音识别 WSS 地址覆盖",
                     "仅限 wss://*.aliyuncs.com/…")
        .textSetting("asr_model", g_config.asr_model, "语音识别模型")
        .textSetting("tts_endpoint", g_config.tts_endpoint, "语音合成 WSS 地址覆盖",
                     "仅限 wss://*.aliyuncs.com/…")
        .textSetting("tts_model", g_config.tts_model, "语音合成模型")
        .textSetting("default_tts_voice", g_config.default_tts_voice,
                     "默认 TTS 音色", "节点留空时使用。")
        .onConfigure(&configure)
        .onShutdown(&shutdown)
        .preset("Qwen 同声传译（麦克风到扬声器）", kPreset)
        .preset("Qwen 语音识别并重读", kSpeechPreset)
        .uiAsset("ui/main.js", kSettingsModule, "text/javascript; charset=utf-8")
        .uiEntry("ui/main.js");

    plugin.node<QwenLiveTranslateNode>(
        avc::sdk::NodeDesc("interpreter", "translation", "Qwen 同声传译")
            .in("in")
            .out("translated_audio")
            .out("translation", avc::sdk::kTextPortType)
            .out("source_transcript", avc::sdk::kTextPortType)
            .boolParam("enabled", true, "关闭时原声直通。")
            .enumParam("target_language", avc::qwen::languages(), 1,
                       "仅文本语种会关闭音频输出。")
            .enumParam("source_language", avc::qwen::sourceLanguages(), 0,
                       "auto = 自动检测。")
            .boolParam("audio_output", true, "同时生成 24 kHz 翻译语音。")
            .boolParam("source_transcript", true)
            .floatParam("vad_threshold", -1.0F, 1.0F, 0.2F, {}, AVC_CURVE_LINEAR,
                         "VAD 灵敏度。")
            .floatParam("silence_duration_ms", 200.0F, 6000.0F, 1000.0F, "ms",
                         AVC_CURVE_LINEAR, "结束语段所需的静音。")
            .enumParam("voice_clone", {"off", "once", "always", "preset"}, 0,
                       "preset 需要在 voice 中填写已复刻音色 ID。")
            .floatParam("jitter_ms", 40.0F, 2000.0F, 240.0F, "ms", AVC_CURVE_LINEAR,
                         "播放前的网络抖动缓冲。")
            .textParam("voice", {}, "音色 ID；留空使用扩展默认值。"));

    avc::qwen::detail::registerSpeechNodes(plugin);
}
