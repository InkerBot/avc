#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace avc::qwen {

inline constexpr std::uint32_t kInputSampleRate = 16000;
inline constexpr std::uint32_t kOutputSampleRate = 24000;
inline constexpr std::size_t kInputChunkSamples = 1600;

enum class VoiceCloneMode : std::uint8_t {
    Off,
    Once,
    Always,
    Preset,
};

struct SessionOptions {
    std::string target_language = "en";
    std::string source_language = "auto";
    std::string voice = "Tina";
    VoiceCloneMode voice_clone = VoiceCloneMode::Off;
    bool audio_output = true;
    bool source_transcription = true;
    float vad_threshold = 0.2F;
    std::uint32_t silence_duration_ms = 1000;
    std::string hotwords_json;
};

struct AsrSessionOptions {
    std::string language = "auto";
    float vad_threshold = 0.0F;
    std::uint32_t silence_duration_ms = 400;
};

struct TtsSessionOptions {
    std::string voice = "Cherry";
    std::string language = "Auto";
    float speech_rate = 1.0F;
    std::uint32_t volume = 50;
    float pitch_rate = 1.0F;
    std::string instructions;
    bool optimize_instructions = false;
};

enum class ServerEventKind : std::uint8_t {
    Ignored,
    Invalid,
    Error,
    SessionUpdated,
    SessionFinished,
    ResponseDone,
    Audio,
    Translation,
    TranslationDone,
    SourceTranscript,
    SourceTranscriptDone,
};

struct ServerEvent {
    ServerEventKind kind = ServerEventKind::Ignored;
    std::vector<std::int16_t> audio;
    std::string text;
    std::string error;
};

const std::vector<std::string> &languages();
const std::vector<std::string> &sourceLanguages();
const std::vector<std::string> &asrLanguages();
const std::vector<std::string> &ttsLanguages();
bool supportsAudioOutput(std::string_view language) noexcept;
bool validIdentifier(std::string_view value) noexcept;

std::string makeEventId();
std::string makeSessionUpdate(const SessionOptions &options, std::string &error);
std::string makeAsrSessionUpdate(const AsrSessionOptions &options, std::string &error);
std::string makeTtsSessionUpdate(const TtsSessionOptions &options, std::string &error);
std::string makeAudioAppend(std::span<const std::int16_t> samples);
std::string makeTextAppend(std::string_view text);
std::string makeTextCommit();
std::string makeSessionFinish();
ServerEvent parseServerEvent(std::string_view message);

std::string buildEndpoint(std::string_view region, std::string_view workspace_id,
                          std::string_view endpoint_override, std::string_view model,
                          std::string &error);

}
