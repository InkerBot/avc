#include "audio/WasapiSession.hpp"

#include "log/Log.hpp"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#include <miniaudio.h>

#include <algorithm>
#include <cstring>

namespace avc::audio {
namespace {

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

}

struct WasapiSession::Impl {
    ma_context context{};
    bool open = false;
};

WasapiSession::WasapiSession() : impl_(std::make_unique<Impl>()) {}
WasapiSession::~WasapiSession() { close(); }

bool WasapiSession::open(const char * )
{
    if (impl_->open) return true;
    const ma_backend backend = ma_backend_wasapi;
    ma_context_config config = ma_context_config_init();
    const ma_result result = ma_context_init(&backend, 1, &config, &impl_->context);
    if (result != MA_SUCCESS) {
        spdlog::error("cannot initialize WASAPI: {}", ma_result_description(result));
        return false;
    }
    impl_->open = true;
    return true;
}

void WasapiSession::close() noexcept
{
    if (!impl_->open) return;
    ma_context_uninit(&impl_->context);
    impl_->open = false;
}

bool WasapiSession::valid() const noexcept { return impl_->open; }

std::vector<DeviceInfo> WasapiSession::enumerateDevices()
{
    std::vector<DeviceInfo> out;
    if (!impl_->open) return out;

    ma_device_info *playback = nullptr;
    ma_device_info *capture = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    const ma_result result = ma_context_get_devices(&impl_->context, &playback, &playback_count,
                                                     &capture, &capture_count);
    if (result != MA_SUCCESS) {
        spdlog::error("cannot enumerate WASAPI devices: {}", ma_result_description(result));
        return out;
    }

    std::uint32_t next_id = 1;
    for (ma_uint32 i = 0; i < capture_count; ++i) {
        DeviceInfo info;
        info.id = next_id++;
        info.name = idString(capture[i].id);
        info.description = capture[i].name;
        info.media_class = "Audio/Source";
        info.output_ports = std::max<ma_uint32>(1, capture[i].nativeDataFormatCount > 0
                                                      ? capture[i].nativeDataFormats[0].channels
                                                      : 1);
        out.push_back(std::move(info));
    }
    for (ma_uint32 i = 0; i < playback_count; ++i) {
        DeviceInfo info;
        info.id = next_id++;
        info.name = idString(playback[i].id);
        info.description = playback[i].name;
        info.media_class = "Audio/Sink";
        info.input_ports = std::max<ma_uint32>(1, playback[i].nativeDataFormatCount > 0
                                                     ? playback[i].nativeDataFormats[0].channels
                                                     : 2);
        out.push_back(std::move(info));
    }
    return out;
}

std::string WasapiSession::defaultDeviceName(Direction direction)
{
    if (!impl_->open) return {};
    ma_device_info *playback = nullptr;
    ma_device_info *capture = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    if (ma_context_get_devices(&impl_->context, &playback, &playback_count, &capture,
                               &capture_count) != MA_SUCCESS) {
        return {};
    }
    ma_device_info *list = direction == Direction::Input ? capture : playback;
    const ma_uint32 count = direction == Direction::Input ? capture_count : playback_count;
    for (ma_uint32 i = 0; i < count; ++i) {
        if (list[i].isDefault) return idString(list[i].id);
    }
    return count > 0 ? idString(list[0].id) : std::string{};
}

}
