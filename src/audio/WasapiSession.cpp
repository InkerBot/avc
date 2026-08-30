#include "audio/WasapiSession.hpp"

#include "log/Log.hpp"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#include <miniaudio.h>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>
#include <string_view>

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

std::string utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                              out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring windowTitle(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    if (copied <= 0) return {};
    title.resize(static_cast<std::size_t>(copied));
    return title;
}

std::wstring processImage(DWORD process_id)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) return {};
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) size = 0;
    CloseHandle(process);
    path.resize(size);
    return path;
}

struct WindowSources {
    std::vector<DeviceInfo> *devices = nullptr;
    std::set<DWORD> seen;
    std::uint32_t *next_id = nullptr;
};

BOOL CALLBACK enumerateWindow(HWND window, LPARAM parameter)
{
    auto &sources = *reinterpret_cast<WindowSources *>(parameter);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;

    const std::wstring title = windowTitle(window);
    if (title.empty()) return TRUE;

    DWORD process_id = 0;
    (void)GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0 || process_id == GetCurrentProcessId()
        || !sources.seen.insert(process_id).second)
        return TRUE;

    const std::wstring image = processImage(process_id);
    if (image.empty()) return TRUE;

    DeviceInfo info;
    info.id = (*sources.next_id)++;
    info.name = "wasapi-process:" + std::to_string(process_id);
    info.description = utf8(title);
    info.media_class = "Stream/Output/Audio";
    info.application = utf8(std::filesystem::path(image).filename().wstring());
    info.media_name = info.description;
    info.output_ports = 2;
    sources.devices->push_back(std::move(info));
    return TRUE;
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
        // A render endpoint can also be used as a capture source through WASAPI
        // loopback. Keeping one stable device name lets the graph use the same
        // endpoint for playback and for capturing what is playing on it.
        info.output_ports = info.input_ports;
        out.push_back(std::move(info));
    }


    WindowSources sources{&out, {}, &next_id};
    (void)EnumWindows(&enumerateWindow, reinterpret_cast<LPARAM>(&sources));
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
