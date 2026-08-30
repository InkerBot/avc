#include <avc_qwen/Protocol.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace avc::qwen {
namespace {

using nlohmann::json;

constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

const std::vector<std::string> kLanguages{
    "zh",  "en", "ar",  "de", "fr", "es", "pt", "id", "it", "ko",
    "ru",  "th", "vi",  "ja", "tr", "hi", "ms", "nl", "ur", "nb",
    "sv",  "da", "he",  "fi", "pl", "is", "cs", "fil", "fa", "yue",
    "el",  "af", "ast", "be", "bg", "bn", "bs", "ca", "ceb", "et",
    "gl",  "gu", "hr",  "hu", "jv", "kk", "kn", "ky", "lv", "mk",
    "ml",  "mr", "pa",  "ro", "sk", "sl", "sw", "tg", "az", "uk",
};

const std::unordered_set<std::string_view> kAudioLanguages{
    "zh", "en", "ar", "de", "fr", "es", "pt", "id", "it", "ko",
    "ru", "th", "vi", "ja", "tr", "hi", "ms", "nl", "ur", "nb",
    "sv", "da", "he", "fi", "pl", "is", "cs", "fil", "fa",
};

const std::vector<std::string> kAsrLanguages{
    "auto", "zh", "yue", "en", "ja", "de", "ko", "ru", "fr", "pt",
    "ar", "it", "es", "hi", "id", "th", "tr", "uk", "vi", "cs",
    "da", "fil", "fi", "is", "ms", "no", "pl", "sv",
};

const std::vector<std::string> kTtsLanguages{
    "Auto", "Chinese", "English", "German", "Italian", "Portuguese",
    "Spanish", "Japanese", "Korean", "French", "Russian",
};

std::string base64Encode(std::span<const std::int16_t> samples)
{
    std::string bytes;
    bytes.resize(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(samples[i]);
        bytes[i * 2] = static_cast<char>(value & 0xffU);
        bytes[i * 2 + 1] = static_cast<char>(value >> 8U);
    }

    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = static_cast<unsigned char>(bytes[i]);
        const std::uint32_t b = i + 1 < bytes.size()
                                    ? static_cast<unsigned char>(bytes[i + 1])
                                    : 0;
        const std::uint32_t c = i + 2 < bytes.size()
                                    ? static_cast<unsigned char>(bytes[i + 2])
                                    : 0;
        const std::uint32_t packed = (a << 16U) | (b << 8U) | c;
        out.push_back(kBase64[(packed >> 18U) & 63U]);
        out.push_back(kBase64[(packed >> 12U) & 63U]);
        out.push_back(i + 1 < bytes.size() ? kBase64[(packed >> 6U) & 63U] : '=');
        out.push_back(i + 2 < bytes.size() ? kBase64[packed & 63U] : '=');
    }
    return out;
}

int base64Value(char c) noexcept
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64Decode(std::string_view encoded, std::vector<std::uint8_t> &out)
{
    out.clear();
    out.reserve(encoded.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (char c : encoded) {
        if (c == '=') break;
        const int value = base64Value(c);
        if (value < 0) return false;
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
        }
    }
    return true;
}

std::size_t pcmOffset(const std::vector<std::uint8_t> &bytes) noexcept
{
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return 0;
    }
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::uint32_t size = static_cast<std::uint32_t>(bytes[at + 4])
                                 | (static_cast<std::uint32_t>(bytes[at + 5]) << 8U)
                                 | (static_cast<std::uint32_t>(bytes[at + 6]) << 16U)
                                 | (static_cast<std::uint32_t>(bytes[at + 7]) << 24U);
        if (std::memcmp(bytes.data() + at, "data", 4) == 0) return at + 8;
        if (size > bytes.size() - at - 8) return bytes.size();
        at += 8 + size + (size & 1U);
    }
    return bytes.size();
}

std::vector<std::int16_t> decodeAudio(std::string_view encoded, std::string &error)
{
    std::vector<std::uint8_t> bytes;
    if (!base64Decode(encoded, bytes)) {
        error = "response.audio.delta contains invalid Base64";
        return {};
    }
    const std::size_t offset = pcmOffset(bytes);
    if (offset > bytes.size() || (bytes.size() - offset) % 2 != 0) {
        error = "response.audio.delta contains malformed PCM16";
        return {};
    }
    std::vector<std::int16_t> samples((bytes.size() - offset) / 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::uint16_t value = static_cast<std::uint16_t>(bytes[offset + i * 2])
                                  | (static_cast<std::uint16_t>(bytes[offset + i * 2 + 1])
                                     << 8U);
        samples[i] = static_cast<std::int16_t>(value);
    }
    return samples;
}

std::string textField(const json &event, const char *field)
{
    const auto it = event.find(field);
    return it != event.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool aliyunEndpoint(std::string_view endpoint) noexcept
{
    constexpr std::string_view scheme = "wss://";
    if (!endpoint.starts_with(scheme)) return false;
    const std::size_t host_end = endpoint.find_first_of("/?#", scheme.size());
    const std::string_view authority = endpoint.substr(
        scheme.size(), host_end == std::string_view::npos ? endpoint.size() - scheme.size()
                                                         : host_end - scheme.size());
    const std::size_t colon = authority.find(':');
    const std::string_view host = authority.substr(0, colon);
    return host == "aliyuncs.com" || host.ends_with(".aliyuncs.com");
}

}

const std::vector<std::string> &languages()
{
    return kLanguages;
}

const std::vector<std::string> &sourceLanguages()
{
    static const std::vector<std::string> result = [] {
        std::vector<std::string> values{"auto"};
        values.insert(values.end(), kLanguages.begin(), kLanguages.end());
        return values;
    }();
    return result;
}

const std::vector<std::string> &asrLanguages()
{
    return kAsrLanguages;
}

const std::vector<std::string> &ttsLanguages()
{
    return kTtsLanguages;
}

bool supportsAudioOutput(std::string_view language) noexcept
{
    return kAudioLanguages.contains(language);
}

bool validIdentifier(std::string_view value) noexcept
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

std::string makeEventId()
{
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return "event_avc_"
         + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now).count())
         + "_" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

std::string makeSessionUpdate(const SessionOptions &options, std::string &error)
{
    error.clear();
    if (std::find(kLanguages.begin(), kLanguages.end(), options.target_language)
        == kLanguages.end()) {
        error = "unsupported target language: " + options.target_language;
        return {};
    }
    if (std::find(sourceLanguages().begin(), sourceLanguages().end(), options.source_language)
        == sourceLanguages().end()) {
        error = "unsupported source language: " + options.source_language;
        return {};
    }

    const bool audio = options.audio_output && supportsAudioOutput(options.target_language);
    json session{
        {"modalities", audio ? json::array({"text", "audio"}) : json::array({"text"})},
        {"sample_rate", kInputSampleRate},
        {"input_audio_format", "pcm"},
        {"output_audio_format", "pcm"},
        {"turn_detection",
         {{"type", "server_vad"},
          {"threshold", std::clamp(options.vad_threshold, -1.0F, 1.0F)},
          {"silence_duration_ms", std::clamp(options.silence_duration_ms, 200U, 6000U)}}},
    };

    json transcription{{"model", options.source_transcription
                                     ? json("qwen3-asr-flash-realtime")
                                     : json(nullptr)}};
    if (options.source_language != "auto") {
        transcription["language"] = options.source_language;
    }
    session["input_audio_transcription"] = std::move(transcription);

    json translation{{"language", options.target_language}};
    if (!options.hotwords_json.empty()) {
        json hotwords = json::parse(options.hotwords_json, nullptr, false);
        if (hotwords.is_discarded() || !hotwords.is_object()) {
            error = "hotwords_json must be a JSON object of source-to-target phrases";
            return {};
        }
        for (auto it = hotwords.begin(); it != hotwords.end(); ++it) {
            if (!it.value().is_string()) {
                error = "every hotwords_json value must be a string";
                return {};
            }
        }
        translation["corpus"] = {{"phrases", std::move(hotwords)}};
    }
    session["translation"] = std::move(translation);

    if (audio) {
        const std::string voice = options.voice.empty() ? "Tina" : options.voice;
        if (options.voice_clone == VoiceCloneMode::Once
            || options.voice_clone == VoiceCloneMode::Always) {
            session["voice"] = "default";
            session["enable_voice_clone"] = true;
            session["voice_clone_options"] = {
                {"frequency", options.voice_clone == VoiceCloneMode::Once ? "once" : "always"}};
        } else if (options.voice_clone == VoiceCloneMode::Preset) {
            if (!voice.starts_with("qwen-translate-vc-")) {
                error = "preset voice cloning requires a qwen-translate-vc voice ID";
                return {};
            }
            session["voice"] = voice;
            session["enable_voice_clone"] = true;
            session["voice_clone_options"] = {{"frequency", "never"}};
        } else {
            session["voice"] = voice;
        }
    }

    return json{{"event_id", makeEventId()}, {"type", "session.update"},
                {"session", std::move(session)}}
        .dump();
}

std::string makeAsrSessionUpdate(const AsrSessionOptions &options, std::string &error)
{
    error.clear();
    if (std::find(kAsrLanguages.begin(), kAsrLanguages.end(), options.language)
        == kAsrLanguages.end()) {
        error = "unsupported ASR language: " + options.language;
        return {};
    }

    json transcription = json::object();
    if (options.language != "auto") transcription["language"] = options.language;
    json session{
        {"input_audio_format", "pcm"},
        {"sample_rate", kInputSampleRate},
        {"input_audio_transcription", std::move(transcription)},
        {"turn_detection",
         {{"type", "server_vad"},
          {"threshold", std::clamp(options.vad_threshold, -1.0F, 1.0F)},
          {"silence_duration_ms",
           std::clamp(options.silence_duration_ms, 200U, 6000U)}}},
    };
    return json{{"event_id", makeEventId()}, {"type", "session.update"},
                {"session", std::move(session)}}
        .dump();
}

std::string makeTtsSessionUpdate(const TtsSessionOptions &options, std::string &error)
{
    error.clear();
    if (options.voice.empty()) {
        error = "TTS voice must not be empty";
        return {};
    }
    if (std::find(kTtsLanguages.begin(), kTtsLanguages.end(), options.language)
        == kTtsLanguages.end()) {
        error = "unsupported TTS language: " + options.language;
        return {};
    }

    json session{
        {"voice", options.voice},
        {"mode", "commit"},
        {"language_type", options.language},
        {"response_format", "pcm"},
        {"sample_rate", kOutputSampleRate},
        {"speech_rate", std::clamp(options.speech_rate, 0.5F, 2.0F)},
        {"volume", std::clamp(options.volume, 0U, 100U)},
        {"pitch_rate", std::clamp(options.pitch_rate, 0.5F, 2.0F)},
    };
    if (!options.instructions.empty()) {
        session["instructions"] = options.instructions;
        session["optimize_instructions"] = options.optimize_instructions;
    }
    return json{{"event_id", makeEventId()}, {"type", "session.update"},
                {"session", std::move(session)}}
        .dump();
}

std::string makeAudioAppend(std::span<const std::int16_t> samples)
{
    return json{{"event_id", makeEventId()}, {"type", "input_audio_buffer.append"},
                {"audio", base64Encode(samples)}}
        .dump();
}

std::string makeTextAppend(std::string_view text)
{
    return json{{"event_id", makeEventId()}, {"type", "input_text_buffer.append"},
                {"text", text}}
        .dump();
}

std::string makeTextCommit()
{
    return json{{"event_id", makeEventId()}, {"type", "input_text_buffer.commit"}}
        .dump();
}

std::string makeSessionFinish()
{
    return json{{"event_id", makeEventId()}, {"type", "session.finish"}}.dump();
}

ServerEvent parseServerEvent(std::string_view message)
{
    ServerEvent out;
    const json event = json::parse(message, nullptr, false);
    if (event.is_discarded() || !event.is_object()) {
        out.kind = ServerEventKind::Invalid;
        out.error = "server sent invalid JSON";
        return out;
    }
    const std::string type = textField(event, "type");
    if (type == "session.updated") {
        out.kind = ServerEventKind::SessionUpdated;
    } else if (type == "session.finished") {
        out.kind = ServerEventKind::SessionFinished;
    } else if (type == "response.done") {
        const json response = event.value("response", json::object());
        if (textField(response, "status") == "failed") {
            out.kind = ServerEventKind::Error;
            const json details = response.value("status_details", json::object());
            const json detail = details.value("error", json::object());
            out.error = textField(detail, "message");
            if (out.error.empty()) out.error = "Qwen TTS response failed";
        } else {
            out.kind = ServerEventKind::ResponseDone;
        }
    } else if (type == "error") {
        out.kind = ServerEventKind::Error;
        const json detail = event.value("error", json::object());
        const std::string code = textField(detail, "code");
        out.error = (code.empty() ? std::string{} : code + ": ") + textField(detail, "message");
        if (out.error.empty()) out.error = "Qwen returned an unspecified error";
    } else if (type == "response.audio.delta") {
        out.kind = ServerEventKind::Audio;
        out.audio = decodeAudio(textField(event, "delta"), out.error);
        if (!out.error.empty()) out.kind = ServerEventKind::Invalid;
    } else if (type == "response.audio_transcript.text" || type == "response.text.text") {
        out.kind = ServerEventKind::Translation;
        out.text = textField(event, "text") + textField(event, "stash");
    } else if (type == "response.audio_transcript.done") {
        out.kind = ServerEventKind::TranslationDone;
        out.text = textField(event, "transcript");
    } else if (type == "response.text.done") {
        out.kind = ServerEventKind::TranslationDone;
        out.text = textField(event, "text");
    } else if (type == "conversation.item.input_audio_transcription.text") {
        out.kind = ServerEventKind::SourceTranscript;
        out.text = textField(event, "text") + textField(event, "stash");
    } else if (type == "conversation.item.input_audio_transcription.completed") {
        out.kind = ServerEventKind::SourceTranscriptDone;
        out.text = textField(event, "transcript");
    } else if (type == "conversation.item.input_audio_transcription.failed") {
        out.kind = ServerEventKind::Error;
        const json detail = event.value("error", json::object());
        out.error = "source transcription failed: " + textField(detail, "message");
    }
    return out;
}

std::string buildEndpoint(std::string_view region, std::string_view workspace_id,
                          std::string_view endpoint_override, std::string_view model,
                          std::string &error)
{
    error.clear();
    if (!validIdentifier(model)) {
        error = "model contains characters that are unsafe in a URL";
        return {};
    }
    std::string endpoint;
    if (!endpoint_override.empty()) {
        endpoint = endpoint_override;
    } else if (!workspace_id.empty()) {
        if (!validIdentifier(workspace_id)) {
            error = "workspace_id contains invalid characters";
            return {};
        }
        const std::string domain = region == "ap-southeast-1"
                                     ? "ap-southeast-1.maas.aliyuncs.com"
                                     : "cn-beijing.maas.aliyuncs.com";
        endpoint = "wss://" + std::string(workspace_id) + "." + domain
                 + "/api-ws/v1/realtime";
    } else {
        endpoint = region == "ap-southeast-1"
                     ? "wss://dashscope-intl.aliyuncs.com/api-ws/v1/realtime"
                     : "wss://dashscope.aliyuncs.com/api-ws/v1/realtime";
    }
    if (!endpoint.starts_with("wss://")) {
        error = "the Qwen endpoint must use wss://";
        return {};
    }
    if (!aliyunEndpoint(endpoint)) {
        error = "the Qwen endpoint must be hosted under aliyuncs.com";
        return {};
    }
    if (endpoint.find("model=") == std::string::npos) {
        endpoint += endpoint.find('?') == std::string::npos ? "?model=" : "&model=";
        endpoint += model;
    }
    return endpoint;
}

}
