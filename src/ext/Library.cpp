#include "ext/Library.hpp"

#include "log/Log.hpp"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstring>
#include <map>

namespace avc::ext {

struct Library::HostState {
    std::string id;
    std::map<std::string, std::string> settings;
    UiSink ui_sink;
};

namespace {

constexpr const char *kEntryPoint = "avc_plugin_init";
constexpr std::uint32_t kMaxUiMessageBytes = 256U * 1024U;

void hostLog(void *context, AvcLogLevel level, const char *message)
{
    const auto *state = static_cast<const Library::HostState *>(context);
    const std::string id = state != nullptr && !state->id.empty() ? state->id : "extension";
    const std::string text = "[" + id + "] " + (message != nullptr ? message : "");
    switch (level) {
    case AVC_LOG_TRACE: spdlog::trace("{}", text); break;
    case AVC_LOG_DEBUG: spdlog::debug("{}", text); break;
    case AVC_LOG_INFO:  spdlog::info("{}", text); break;
    case AVC_LOG_WARN:  spdlog::warn("{}", text); break;
    case AVC_LOG_ERROR: spdlog::error("{}", text); break;
    }
}

const char *hostSetting(void *context, const char *key)
{
    const auto *state = static_cast<const Library::HostState *>(context);
    if (state == nullptr || key == nullptr) return nullptr;
    const auto found = state->settings.find(key);
    return found != state->settings.end() ? found->second.c_str() : nullptr;
}

void hostUiReply(void *context, std::uint64_t request_id, AvcResult result,
                 const char *text, std::uint32_t text_size, const char *error)
{
    const auto *state = static_cast<const Library::HostState *>(context);
    if (state == nullptr || !state->ui_sink) return;

    nlohmann::json payload = nullptr;
    bool ok = result == AVC_RESULT_OK;
    std::string reason = error != nullptr ? error : "extension request failed";
    if (text_size > kMaxUiMessageBytes) {
        ok = false;
        reason = "extension response is larger than 256 KiB";
    }
    if (ok) {
        payload = nlohmann::json::parse(text != nullptr ? std::string(text, text_size) : "null",
                                       nullptr, false);
        if (payload.is_discarded()) {
            ok = false;
            reason = "extension returned invalid JSON";
            payload = nullptr;
        }
    }
    state->ui_sink({{"t", "extension.ui.reply"},
                    {"id", request_id},
                    {"extension", state->id},
                    {"ok", ok},
                    {"data", std::move(payload)},
                    {"error", ok ? std::string{} : reason}});
}

void hostUiEmit(void *context, const char *event, const char *text, std::uint32_t text_size)
{
    const auto *state = static_cast<const Library::HostState *>(context);
    if (state == nullptr || !state->ui_sink || event == nullptr) return;
    const std::string_view name(event);
    if (name.empty() || name.size() > 96 || text_size > kMaxUiMessageBytes
        || !std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
           })) {
        hostLog(context, AVC_LOG_WARN, "ignored an invalid UI event");
        return;
    }
    nlohmann::json payload = nlohmann::json::parse(
        text != nullptr ? std::string(text, text_size) : "null", nullptr, false);
    if (payload.is_discarded()) {
        hostLog(context, AVC_LOG_WARN, "ignored a UI event with invalid JSON");
        return;
    }
    state->ui_sink({{"t", "extension.ui.event"},
                    {"extension", state->id},
                    {"event", event},
                    {"data", std::move(payload)}});
}

#ifdef _WIN32
std::string windowsError()
{
    char *message = nullptr;
    const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER
                                          | FORMAT_MESSAGE_FROM_SYSTEM
                                          | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, GetLastError(), 0,
                                      reinterpret_cast<char *>(&message), 0, nullptr);
    std::string out = size > 0 && message != nullptr ? std::string(message, size)
                                                     : "Windows error " + std::to_string(GetLastError());
    if (message != nullptr) LocalFree(message);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
    return out;
}
#endif

}

Library::Library() = default;

Library::~Library()
{
    if (plugin_ != nullptr && plugin_->shutdown != nullptr) {
        plugin_->shutdown();
    }
    // dlclose could invalidate code while an old graph is still retiring on the audio thread.
}

bool Library::open(const std::filesystem::path &path,
                   const std::map<std::string, std::string> &settings,
                   const std::filesystem::path &config_dir,
                   const std::filesystem::path &data_dir, UiSink ui_sink,
                   std::string &error)
{
    path_ = path;
    host_state_ = std::make_unique<HostState>();
    host_state_->id = path.stem().string();
    host_state_->settings = settings;
    host_state_->ui_sink = std::move(ui_sink);
    config_dir_ = config_dir.string();
    data_dir_ = data_dir.string();
    host_ = {};
    host_.abi_version = AVC_ABI_VERSION;
    host_.struct_size = sizeof(AvcHostApi);
    host_.context = host_state_.get();
    host_.log = &hostLog;
    host_.setting = &hostSetting;
    host_.config_dir = config_dir_.c_str();
    host_.data_dir = data_dir_.c_str();
    host_.ui_reply = &hostUiReply;
    host_.ui_emit = &hostUiEmit;

    // RTLD_LOCAL so an extension's symbols cannot be bound to by anything else,
    // including another extension. RTLD_NOW so a missing symbol is a load
    // failure with a message rather than a crash the first time that code runs.
#ifdef _WIN32
    const std::filesystem::path dependency_dir = path.parent_path() / L"lib";
    std::error_code dependency_ec;
    if (std::filesystem::is_directory(dependency_dir, dependency_ec)) {
        // Keep the directory registered for the process lifetime. Execution
        // providers may delay-load their own DLLs after this function returns.
        (void)AddDllDirectory(dependency_dir.c_str());
    }
    handle_ = LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                                 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (handle_ == nullptr && GetLastError() == ERROR_INVALID_PARAMETER)
        handle_ = LoadLibraryW(path.c_str());
    if (handle_ == nullptr) {
        error = windowsError();
        return false;
    }
    using Entry = const AvcPlugin *(*)(const AvcHostApi *);
    Entry entry = nullptr;
    const FARPROC symbol = GetProcAddress(static_cast<HMODULE>(handle_), kEntryPoint);
    static_assert(sizeof(entry) == sizeof(symbol));
    std::memcpy(&entry, &symbol, sizeof(entry));
#else
    handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        const char *reason = ::dlerror();
        error = reason != nullptr ? reason : "cannot open it";
        return false;
    }

    ::dlerror();
    auto entry = reinterpret_cast<const AvcPlugin *(*)(const AvcHostApi *)>(
        ::dlsym(handle_, kEntryPoint));
#endif
    if (entry == nullptr) {
        error = std::string("it exports no ") + kEntryPoint
                + "(); it is a shared library, but not an avc extension";
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        ::dlclose(handle_);
#endif
        handle_ = nullptr;
        return false;
    }

    const AvcPlugin *plugin = entry(&host_);
    if (plugin == nullptr) {
        error = "it refused to load; see the log for what it said";
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        ::dlclose(handle_);
#endif
        handle_ = nullptr;
        return false;
    }

    if (plugin->abi_version != AVC_ABI_VERSION) {
        error = "it was built against extension ABI " + std::to_string(plugin->abi_version)
                + ", and this engine speaks " + std::to_string(AVC_ABI_VERSION)
                + "; it has to be rebuilt";
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        ::dlclose(handle_);
#endif
        handle_ = nullptr;
        return false;
    }
    if (plugin->struct_size < sizeof(AvcPlugin)) {
        error = "it returned a truncated extension manifest";
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        ::dlclose(handle_);
#endif
        handle_ = nullptr;
        return false;
    }

    plugin_ = plugin;
    return true;
}

void Library::setId(std::string id)
{
    if (host_state_ != nullptr) host_state_->id = std::move(id);
}

}
