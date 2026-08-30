#include <avc_qwen/AudioBuffer.hpp>
#include <avc_qwen/AudioResampler.hpp>
#include <avc_qwen/Protocol.hpp>

#ifdef AVC_QWEN_PLUGIN_PATH
#include <avc/avc_plugin.hpp>

#include "ext/Library.hpp"
#endif

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

TEST(QwenAudioBuffer, WrapsWithoutReordering)
{
    avc::qwen::PcmSpscBuffer buffer;
    buffer.reset(4);
    const std::array<std::int16_t, 3> first{1, 2, 3};
    EXPECT_EQ(buffer.write(first), 3U);

    std::array<std::int16_t, 2> taken{};
    EXPECT_EQ(buffer.read(taken), 2U);
    EXPECT_EQ(taken, (std::array<std::int16_t, 2>{1, 2}));

    const std::array<std::int16_t, 3> second{4, 5, 6};
    EXPECT_EQ(buffer.write(second), 3U);
    std::array<std::int16_t, 4> all{};
    EXPECT_EQ(buffer.read(all), 4U);
    EXPECT_EQ(all, (std::array<std::int16_t, 4>{3, 4, 5, 6}));
}

TEST(QwenResampler, ProducesExpectedCommonRateCounts)
{
    avc::qwen::FloatToPcm16Resampler down;
    down.reset(48000, 16000);
    std::array<float, 480> input{};
    input.fill(0.5F);
    std::array<std::int16_t, 164> output{};
    const std::size_t count = down.process(input, output);
    EXPECT_EQ(count, 160U);
    ASSERT_GT(count, 0U);
    EXPECT_GT(output[count - 1], 0);

    avc::qwen::Pcm16ToFloatResampler up;
    up.reset(24000, 48000);
    std::array<std::int16_t, 3> source{0, 32767, -32768};
    std::size_t at = 0;
    std::array<float, 4> expanded{};
    for (float &sample : expanded) {
        EXPECT_TRUE(up.pull(
            [&](std::int16_t &value) {
                if (at == source.size()) return false;
                value = source[at++];
                return true;
            },
            sample));
    }
    EXPECT_NEAR(expanded[0], 0.0F, 1e-5F);
    EXPECT_NEAR(expanded[1], 0.5F, 1e-3F);
    EXPECT_NEAR(expanded[2], 1.0F, 1e-3F);
    EXPECT_NEAR(expanded[3], 0.0F, 1e-3F);
}

TEST(QwenProtocol, BuildsDocumentedSessionShape)
{
    avc::qwen::SessionOptions options;
    options.target_language = "zh";
    options.source_language = "en";
    options.voice = "Tina";
    options.hotwords_json = R"({"AVC":"AVC"})";
    std::string error;
    const json event = json::parse(avc::qwen::makeSessionUpdate(options, error));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(event["type"], "session.update");
    EXPECT_EQ(event["session"]["sample_rate"], 16000);
    EXPECT_EQ(event["session"]["modalities"], json::array({"text", "audio"}));
    EXPECT_EQ(event["session"]["input_audio_transcription"]["language"], "en");
    EXPECT_EQ(event["session"]["translation"]["language"], "zh");
    EXPECT_EQ(event["session"]["translation"]["corpus"]["phrases"]["AVC"], "AVC");
}

TEST(QwenProtocol, BuildsAsrSessionShape)
{
    avc::qwen::AsrSessionOptions options;
    options.language = "yue";
    options.vad_threshold = 0.0F;
    options.silence_duration_ms = 400;
    std::string error;
    const json event = json::parse(avc::qwen::makeAsrSessionUpdate(options, error));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(event["type"], "session.update");
    EXPECT_EQ(event["session"]["input_audio_format"], "pcm");
    EXPECT_EQ(event["session"]["sample_rate"], 16000);
    EXPECT_EQ(event["session"]["input_audio_transcription"]["language"], "yue");
    EXPECT_EQ(event["session"]["turn_detection"]["type"], "server_vad");
}

TEST(QwenProtocol, BuildsTtsCommitEvents)
{
    avc::qwen::TtsSessionOptions options;
    options.voice = "Cherry";
    options.language = "Chinese";
    options.speech_rate = 1.25F;
    options.volume = 80;
    std::string error;
    const json update = json::parse(avc::qwen::makeTtsSessionUpdate(options, error));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(update["session"]["mode"], "commit");
    EXPECT_EQ(update["session"]["response_format"], "pcm");
    EXPECT_EQ(update["session"]["sample_rate"], 24000);
    EXPECT_EQ(update["session"]["language_type"], "Chinese");

    const json append = json::parse(avc::qwen::makeTextAppend("你好"));
    EXPECT_EQ(append["type"], "input_text_buffer.append");
    EXPECT_EQ(append["text"], "你好");
    EXPECT_EQ(json::parse(avc::qwen::makeTextCommit())["type"],
              "input_text_buffer.commit");
}

TEST(QwenProtocol, ParsesTtsCompletion)
{
    auto event = avc::qwen::parseServerEvent(
        R"({"type":"response.done","response":{"status":"completed"}})");
    EXPECT_EQ(event.kind, avc::qwen::ServerEventKind::ResponseDone);

    event = avc::qwen::parseServerEvent(
        R"({"type":"response.done","response":{"status":"failed",)"
        R"("status_details":{"error":{"message":"bad voice"}}}})");
    EXPECT_EQ(event.kind, avc::qwen::ServerEventKind::Error);
    EXPECT_EQ(event.error, "bad voice");
}

TEST(QwenProtocol, FallsBackToTextForTextOnlyLanguage)
{
    avc::qwen::SessionOptions options;
    options.target_language = "yue";
    options.audio_output = true;
    std::string error;
    const json event = json::parse(avc::qwen::makeSessionUpdate(options, error));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(event["session"]["modalities"], json::array({"text"}));
    EXPECT_FALSE(event["session"].contains("voice"));
}

TEST(QwenProtocol, RoundTripsPcmThroughAudioEvents)
{
    const std::array<std::int16_t, 4> samples{0, 1, -1, 32767};
    const json append = json::parse(avc::qwen::makeAudioAppend(samples));
    const json response{{"type", "response.audio.delta"}, {"delta", append["audio"]}};
    const avc::qwen::ServerEvent event = avc::qwen::parseServerEvent(response.dump());
    EXPECT_EQ(event.kind, avc::qwen::ServerEventKind::Audio);
    EXPECT_EQ(event.audio, (std::vector<std::int16_t>(samples.begin(), samples.end())));
}

TEST(QwenProtocol, ParsesIncrementalAndFinalText)
{
    auto event = avc::qwen::parseServerEvent(
        R"({"type":"response.audio_transcript.text","text":"你","stash":"好"})");
    EXPECT_EQ(event.kind, avc::qwen::ServerEventKind::Translation);
    EXPECT_EQ(event.text, "你好");

    event = avc::qwen::parseServerEvent(
        R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"hello"})");
    EXPECT_EQ(event.kind, avc::qwen::ServerEventKind::SourceTranscriptDone);
    EXPECT_EQ(event.text, "hello");
}

TEST(QwenProtocol, BuildsOnlySecureAliyunEndpoints)
{
    std::string error;
    EXPECT_EQ(avc::qwen::buildEndpoint("cn-beijing", "llm-abc", {},
                                       "qwen3.5-livetranslate-flash-realtime", error),
              "wss://llm-abc.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime?model="
              "qwen3.5-livetranslate-flash-realtime");
    EXPECT_TRUE(error.empty());

    EXPECT_TRUE(avc::qwen::buildEndpoint("cn-beijing", {}, "ws://example.com/realtime",
                                         "qwen", error)
                    .empty());
    EXPECT_FALSE(error.empty());
}

#ifdef AVC_QWEN_PLUGIN_PATH
TEST(QwenExtension, RegistersSpeechNodesSettingsAndValidPresets)
{
    avc::ext::Library library;
    std::string error;
    const std::filesystem::path temporary = std::filesystem::temp_directory_path();
    ASSERT_TRUE(library.open(AVC_QWEN_PLUGIN_PATH, {}, temporary, temporary, {}, error))
        << error;
    const AvcPlugin *plugin = library.plugin();
    ASSERT_NE(plugin, nullptr);
    EXPECT_STREQ(plugin->id, "qwen-livetranslate");
    EXPECT_STREQ(plugin->version, "1.1.0");

    std::set<std::string> node_types;
    for (std::uint32_t i = 0; i < plugin->node_count; ++i) {
        node_types.emplace(plugin->nodes[i].desc.type);
    }
    EXPECT_EQ(node_types,
              (std::set<std::string>{"interpreter", "stt", "tts"}));

    const auto find_node = [plugin](std::string_view type) -> const AvcNodeDesc * {
        for (std::uint32_t i = 0; i < plugin->node_count; ++i) {
            if (type == plugin->nodes[i].desc.type) return &plugin->nodes[i].desc;
        }
        return nullptr;
    };
    const AvcNodeDesc *stt = find_node("stt");
    ASSERT_NE(stt, nullptr);
    ASSERT_EQ(stt->input_count, 1U);
    ASSERT_EQ(stt->output_count, 1U);
    EXPECT_STREQ(stt->inputs[0].name, "audio");
    EXPECT_STREQ(stt->outputs[0].name, "transcript");
    EXPECT_STREQ(stt->outputs[0].type, avc::sdk::kTextPortType);

    const AvcNodeDesc *tts = find_node("tts");
    ASSERT_NE(tts, nullptr);
    ASSERT_EQ(tts->input_count, 1U);
    ASSERT_EQ(tts->output_count, 1U);
    EXPECT_STREQ(tts->inputs[0].name, "text");
    EXPECT_STREQ(tts->inputs[0].type, avc::sdk::kTextPortType);
    EXPECT_STREQ(tts->outputs[0].name, "audio");

    std::set<std::string> setting_keys;
    for (std::uint32_t i = 0; i < plugin->setting_count; ++i) {
        setting_keys.emplace(plugin->settings[i].key);
    }
    EXPECT_TRUE(setting_keys.contains("asr_model"));
    EXPECT_TRUE(setting_keys.contains("tts_model"));
    EXPECT_TRUE(setting_keys.contains("default_tts_voice"));

    ASSERT_EQ(plugin->preset_count, 2U);
    for (std::uint32_t i = 0; i < plugin->preset_count; ++i) {
        const json graph = json::parse(plugin->presets[i].json, nullptr, false);
        EXPECT_FALSE(graph.is_discarded()) << plugin->presets[i].name;
    }
}
#endif

}
