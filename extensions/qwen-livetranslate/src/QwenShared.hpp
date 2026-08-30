#pragma once

#include <string>

namespace avc::sdk {
class Plugin;
}

namespace avc::qwen::detail {

struct ExtensionConfig {
    std::string region = "cn-beijing";
    std::string api_key;
    std::string workspace_id;

    std::string endpoint;
    std::string model = "qwen3.5-livetranslate-flash-realtime";
    std::string default_voice = "Tina";
    std::string hotwords_json;

    std::string asr_endpoint;
    std::string asr_model = "qwen3-asr-flash-realtime";
    std::string tts_endpoint;
    std::string tts_model = "qwen3-tts-flash-realtime";
    std::string default_tts_voice = "Cherry";
};

extern ExtensionConfig g_config;
extern bool g_curl_ready;

void registerSpeechNodes(avc::sdk::Plugin &plugin);

}
