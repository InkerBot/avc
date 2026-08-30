#ifdef _WIN32

#include "control/DesktopHost.hpp"

#include "app/Daemon.hpp"
#include "control/PresetStore.hpp"
#include "control/Serialization.hpp"
#include "control/UiAssets.hpp"
#include "log/Log.hpp"

#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>

#include <WebView2.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace avc::app {
extern std::atomic<bool> g_running;
}

namespace avc::control {
namespace {

using json = nlohmann::json;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"AvcDesktopWindow";
constexpr wchar_t kAppOrigin[] = L"https://avc.local/";
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
constexpr UINT kRpcReplyMessage = WM_APP + 41;
constexpr UINT_PTR kTelemetryTimer = 1;

std::wstring widen(std::string_view text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

int hexDigit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> percentDecode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size()) return std::nullopt;
        const int high = hexDigit(text[i + 1]);
        const int low = hexDigit(text[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

const char *mimeFor(std::string_view path)
{
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js") || path.ends_with(".mjs")) {
        return "text/javascript; charset=utf-8";
    }
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".json") || path.ends_with(".map")) return "application/json";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
    if (path.ends_with(".webp")) return "image/webp";
    if (path.ends_with(".ico")) return "image/x-icon";
    if (path.ends_with(".woff2")) return "font/woff2";
    if (path.ends_with(".wasm")) return "application/wasm";
    return "application/octet-stream";
}

ComPtr<IStream> streamFor(std::string_view bytes)
{
    const SIZE_T size = std::max<std::size_t>(bytes.size(), 1U);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory == nullptr) return {};
    void *destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        return {};
    }
    if (!bytes.empty()) std::memcpy(destination, bytes.data(), bytes.size());
    GlobalUnlock(memory);

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        return {};
    }
    return stream;
}

std::filesystem::path webViewDataDirectory()
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        return {};
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    path /= L"AVC";
    path /= L"WebView2";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

struct ExtensionFileSelection {
    std::filesystem::path path;
    bool cancelled = false;
    std::string error;
};

ExtensionFileSelection chooseExtensionFile(HWND owner)
{
    ComPtr<IFileOpenDialog> dialog;
    const HRESULT created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&dialog));
    if (FAILED(created) || dialog == nullptr) {
        return {{}, false, "Windows could not open the extension file picker"};
    }

    constexpr COMDLG_FILTERSPEC filters[] = {
        {L"AVC extensions (*.dll)", L"*.dll"},
        {L"All files (*.*)", L"*.*"},
    };
    dialog->SetTitle(L"Choose an AVC extension");
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"dll");
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    }

    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {{}, true, {}};
    if (FAILED(shown)) return {{}, false, "Windows could not select the extension file"};

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || item == nullptr) {
        return {{}, false, "Windows did not return an extension path"};
    }
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return {{}, false, "The selected item does not have a local file path"};
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return {std::move(path), false, {}};
}

struct RpcReply {
    bool ok = true;
    json data = nullptr;
    int status = 200;
    std::string error;
};

RpcReply rpcError(int status, std::string error)
{
    return {false, nullptr, status, std::move(error)};
}

}

class DesktopHost::Impl {
public:
    Impl(app::Daemon &daemon, std::string ui_dir)
        : daemon_(daemon), ui_dir_(std::move(ui_dir)),
          presets_(PresetStore::defaultDirectory())
    {
    }

    ~Impl() { stop(); }

    bool start()
    {
        if (thread_.joinable()) return ready_ok_;
        if (ui_dir_.empty() && ui::empty()) {
            spdlog::error("desktop: the editor was not built into this binary");
            return false;
        }
        if (!ui_dir_.empty()
            && !std::filesystem::is_regular_file(std::filesystem::path(ui_dir_) / L"index.html")) {
            spdlog::error("desktop: --ui-dir '{}' has no index.html", ui_dir_);
            return false;
        }
        thread_ = std::thread([this] { threadMain(); });
        std::unique_lock<std::mutex> lock(ready_mutex_);
        ready_cv_.wait(lock, [this] { return ready_reported_; });
        if (!ready_ok_) {
            lock.unlock();
            if (thread_.joinable()) thread_.join();
        }
        return ready_ok_;
    }

    void stop()
    {
        if (!thread_.joinable()) return;
        if (const HWND window = window_.load(std::memory_order_acquire); window != nullptr) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        thread_.join();
    }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        Impl *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
            self = static_cast<Impl *>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self != nullptr ? self->onWindowMessage(window, message, wparam, lparam)
                               : DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT onWindowMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message) {
        case WM_SIZE:
            resizeWebView();
            return 0;
        case WM_MOVE:
            if (controller_ != nullptr) controller_->NotifyParentWindowPositionChanged();
            return 0;
        case WM_SHOWWINDOW:
            if (controller_ != nullptr) controller_->put_IsVisible(wparam != FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
            const UINT dpi = GetDpiForWindow(window);
            info->ptMinTrackSize.x = MulDiv(900, static_cast<int>(dpi), 96);
            info->ptMinTrackSize.y = MulDiv(600, static_cast<int>(dpi), 96);
            return 0;
        }
        case WM_SETFOCUS:
            if (controller_ != nullptr) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        case WM_TIMER:
            if (wparam == kTelemetryTimer) pushEvents();
            return 0;
        case kRpcReplyMessage:
            drainReplies();
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kTelemetryTimer);
            window_.store(nullptr, std::memory_order_release);
            app::g_running.store(false, std::memory_order_relaxed);
            signalReady(false);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    void threadMain()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com)) {
            spdlog::error("desktop: cannot initialize COM: 0x{:08x}",
                          static_cast<unsigned>(com));
            signalReady(false);
            return;
        }

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.lpfnWndProc = &Impl::windowProc;
        klass.hInstance = instance;
        klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        klass.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        // WebView2 owns the whole client area.  Clipping the child prevents the
        // parent background from painting over the compositor surface.
        klass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        klass.lpszClassName = kWindowClass;
        if (RegisterClassExW(&klass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            spdlog::error("desktop: cannot register the window class");
            signalReady(false);
            CoUninitialize();
            return;
        }

        RECT bounds{0, 0, 1440, 900};
        AdjustWindowRectExForDpi(&bounds, kWindowStyle, FALSE, 0, 96);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        const HWND window = CreateWindowExW(0, kWindowClass, L"AVC", kWindowStyle,
                                             x, y, width, height, nullptr, nullptr, instance, this);
        if (window == nullptr) {
            spdlog::error("desktop: cannot create the main window");
            signalReady(false);
            CoUninitialize();
            return;
        }
        window_.store(window, std::memory_order_release);
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));

        startWebView(window);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        stopRpcWorker();
        if (controller_ != nullptr) controller_->Close();
        webview_.Reset();
        controller_.Reset();
        environment_.Reset();
        CoUninitialize();
    }

    void startWebView(HWND window)
    {
        const std::filesystem::path user_data = webViewDataDirectory();
        const std::wstring user_data_text = user_data.wstring();
        const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, user_data_text.empty() ? nullptr : user_data_text.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, window](HRESULT status, ICoreWebView2Environment *environment) -> HRESULT {
                    if (FAILED(status) || environment == nullptr) {
                        startupError(window, L"Microsoft Edge WebView2 Runtime is required to run AVC.");
                        return S_OK;
                    }
                    environment_ = environment;
                    const HRESULT created = environment_->CreateCoreWebView2Controller(
                        window,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this, window](HRESULT controller_status,
                                           ICoreWebView2Controller *controller) -> HRESULT {
                                if (FAILED(controller_status) || controller == nullptr) {
                                    startupError(window, L"AVC could not create its WebView2 window.");
                                    return S_OK;
                                }
                                controller_ = controller;
                                if (FAILED(controller_->get_CoreWebView2(&webview_))
                                    || webview_ == nullptr) {
                                    startupError(window, L"AVC could not initialize WebView2.");
                                    return S_OK;
                                }
                                configureWebView(window);
                                return S_OK;
                            })
                            .Get());
                    if (FAILED(created)) startupError(window, L"AVC could not start WebView2.");
                    return S_OK;
                })
                .Get());
        if (FAILED(result)) startupError(window, L"Microsoft Edge WebView2 Runtime is unavailable.");
    }

    void startupError(HWND window, const wchar_t *message)
    {
        spdlog::error("desktop: WebView2 initialization failed");
        MessageBoxW(window, message, L"AVC", MB_OK | MB_ICONERROR);
        signalReady(false);
        DestroyWindow(window);
    }

    void configureWebView(HWND window)
    {
        controller_->put_IsVisible(TRUE);
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(controller_.As(&controller2)) && controller2 != nullptr) {
            constexpr COREWEBVIEW2_COLOR background{255, 19, 16, 19};
            controller2->put_DefaultBackgroundColor(background);
        }

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webview_->get_Settings(&settings)) && settings != nullptr) {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
            settings->put_AreDevToolsEnabled(FALSE);
#endif
        }

        webview_->AddWebResourceRequestedFilter(
            L"https://avc.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        webview_->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) {
                    return onResourceRequested(args);
                })
                .Get(),
            &resource_token_);
        webview_->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) {
                    return onWebMessage(args);
                })
                .Get(),
            &message_token_);
        webview_->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) {
                    BOOL success = FALSE;
                    COREWEBVIEW2_WEB_ERROR_STATUS error = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    args->get_IsSuccess(&success);
                    args->get_WebErrorStatus(&error);
                    if (success) {
                        spdlog::debug("desktop: navigation completed");
                    } else {
                        spdlog::error("desktop: navigation failed with WebView2 status {}",
                                      static_cast<int>(error));
                    }
                    return S_OK;
                })
                .Get(),
            &navigation_completed_token_);
        webview_->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) {
                    LPWSTR raw = nullptr;
                    if (SUCCEEDED(args->get_Uri(&raw)) && raw != nullptr) {
                        const std::wstring uri(raw);
                        CoTaskMemFree(raw);
                        if (!uri.starts_with(kAppOrigin)) {
                            args->put_Cancel(TRUE);
                            if (uri.starts_with(L"https://") || uri.starts_with(L"http://")) {
                                ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr,
                                              SW_SHOWNORMAL);
                            }
                        }
                    }
                    return S_OK;
                })
                .Get(),
            &navigation_token_);
        webview_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) {
                    LPWSTR raw = nullptr;
                    if (SUCCEEDED(args->get_Uri(&raw)) && raw != nullptr) {
                        ShellExecuteW(nullptr, L"open", raw, nullptr, nullptr, SW_SHOWNORMAL);
                        CoTaskMemFree(raw);
                    }
                    args->put_Handled(TRUE);
                    return S_OK;
                })
                .Get(),
            &new_window_token_);

        constexpr wchar_t diagnostics[] = LR"JS(
(() => {
  const report = (level, message) => window.chrome.webview.postMessage({
    kind: 'diagnostic', level, message: String(message)
  });
  window.addEventListener('error', event => report('error',
    `${event.message} @ ${event.filename}:${event.lineno}:${event.colno}`));
  window.addEventListener('unhandledrejection', event => report('error',
    event.reason?.stack ?? event.reason ?? 'Unhandled promise rejection'));
  window.addEventListener('DOMContentLoaded', () => report('debug',
    `DOM ready; root=${Boolean(document.getElementById('root'))}`));
  window.setTimeout(() => {
    const root = document.getElementById('root');
    report('debug', `rendered; rootChildren=${root?.childElementCount ?? 0}; ` +
      `bodyBackground=${getComputedStyle(document.body).backgroundColor}`);
  }, 1500);
})()
)JS";
        webview_->AddScriptToExecuteOnDocumentCreated(
            diagnostics,
            Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                [](HRESULT status, LPCWSTR) {
                    if (FAILED(status)) spdlog::warn("desktop: could not install JS diagnostics");
                    return S_OK;
                })
                .Get());

        extension_cursor_ = daemon_.extensionEventCursor();
        startRpcWorker();
        resizeWebView();
        SetTimer(window, kTelemetryTimer, 50, nullptr);
        webview_->Navigate(kAppOrigin);
        ShowWindow(window, SW_SHOW);
        controller_->put_IsVisible(TRUE);
        resizeWebView();
        UpdateWindow(window);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        spdlog::info("desktop: WebView2 editor ready (in-process bridge, no port)");
        signalReady(true);
    }

    HRESULT onWebMessage(ICoreWebView2WebMessageReceivedEventArgs *args)
    {
        LPWSTR raw = nullptr;
        if (FAILED(args->get_WebMessageAsJson(&raw)) || raw == nullptr) return S_OK;
        json request = json::parse(narrow(raw), nullptr, false);
        CoTaskMemFree(raw);
        if (request.is_object() && request.value("kind", std::string{}) == "diagnostic") {
            const std::string message = request.value("message", std::string{});
            if (request.value("level", std::string{}) == "error") {
                spdlog::error("desktop JS: {}", message);
            } else {
                spdlog::debug("desktop JS: {}", message);
            }
            return S_OK;
        }
        if (request.is_discarded() || !request.is_object()
            || request.value("kind", std::string{}) != "request"
            || !request.contains("id") || !request.contains("method")) {
            return S_OK;
        }
        if (request.value("method", std::string{}) == "chooseExtension") {
            spdlog::debug("desktop RPC: chooseExtension");
            const ExtensionFileSelection selection = chooseExtensionFile(
                window_.load(std::memory_order_acquire));
            if (selection.cancelled) {
                postReply(request["id"], {true, nullptr});
                return S_OK;
            }
            if (!selection.error.empty()) {
                postReply(request["id"], rpcError(500, selection.error));
                return S_OK;
            }
            request["method"] = "installExtensionPath";
            request["params"] = {{"path", narrow(selection.path.wstring())}};
        }
        {
            const std::lock_guard<std::mutex> lock(rpc_mutex_);
            rpc_requests_.push_back(std::move(request));
        }
        rpc_cv_.notify_one();
        return S_OK;
    }

    void startRpcWorker()
    {
        rpc_thread_ = std::thread([this] {
            for (;;) {
                json request;
                {
                    std::unique_lock<std::mutex> lock(rpc_mutex_);
                    rpc_cv_.wait(lock, [this] { return stop_rpc_ || !rpc_requests_.empty(); });
                    if (stop_rpc_ && rpc_requests_.empty()) return;
                    request = std::move(rpc_requests_.front());
                    rpc_requests_.pop_front();
                }

                RpcReply reply;
                try {
                    spdlog::debug("desktop RPC: {}", request.value("method", std::string{}));
                    reply = dispatch(request.value("method", std::string{}),
                                     request.value("params", json::object()));
                } catch (const std::exception &error) {
                    reply = rpcError(400, error.what());
                }
                json message{{"kind", "response"},
                             {"id", request["id"]},
                             {"ok", reply.ok}};
                if (reply.ok) {
                    message["data"] = std::move(reply.data);
                } else {
                    message["status"] = reply.status;
                    message["error"] = std::move(reply.error);
                }
                {
                    const std::lock_guard<std::mutex> lock(rpc_mutex_);
                    rpc_replies_.push_back(message.dump());
                }
                if (const HWND window = window_.load(std::memory_order_acquire); window != nullptr) {
                    PostMessageW(window, kRpcReplyMessage, 0, 0);
                }
            }
        });
    }

    void stopRpcWorker()
    {
        {
            const std::lock_guard<std::mutex> lock(rpc_mutex_);
            stop_rpc_ = true;
            rpc_requests_.clear();
        }
        rpc_cv_.notify_all();
        if (rpc_thread_.joinable()) rpc_thread_.join();
    }

    RpcReply dispatch(const std::string &method, const json &params)
    {
        if (method == "descriptors") return {true, daemon_.descriptors()};
        if (method == "graph") return {true, json{{"spec", specJson(daemon_.spec())}}};
        if (method == "presets") {
            return {true, json{{"presets", presets_.list()},
                               {"extensionPresets", daemon_.extensionPresets()}}};
        }
        if (method == "audioDevices") return {true, devicesJson(daemon_.devices())};
        if (method == "applyGraph") {
            if (!params.contains("spec")) return rpcError(400, "missing 'spec'");
            std::string error;
            const auto spec = graph::GraphSpec::parse(params["spec"].dump(), error);
            if (!spec) return rpcError(400, error);
            const app::Daemon::ApplyResult result = daemon_.applyGraph(*spec);
            return result.ok ? RpcReply{} : rpcError(422, result.error);
        }
        if (method == "setParam") {
            if (!params.contains("node") || !params.contains("param")
                || !params.contains("value") || !params["value"].is_number()) {
                return rpcError(400, "expected {node, param, value}");
            }
            if (!daemon_.setParam(params["node"].get<std::string>(),
                                  params["param"].get<std::string>(),
                                  params["value"].get<float>())) {
                return rpcError(503, "the engine is not running");
            }
            return {};
        }
        if (method == "savePreset") {
            std::string error;
            return presets_.save(params.at("name").get<std::string>(), daemon_.spec(), error)
                       ? RpcReply{}
                       : rpcError(400, error);
        }
        if (method == "loadPreset") {
            std::string error;
            const auto spec = presets_.load(params.at("name").get<std::string>(), error);
            if (!spec) return rpcError(404, error);
            const auto result = daemon_.applyGraph(*spec);
            return result.ok ? RpcReply{} : rpcError(422, result.error);
        }
        if (method == "deletePreset") {
            std::string error;
            return presets_.remove(params.at("name").get<std::string>(), error)
                       ? RpcReply{}
                       : rpcError(404, error);
        }
        if (method == "setProfiling") {
            daemon_.setProfiling(params.at("enabled").get<bool>());
            return {};
        }
        if (method == "resetStats") {
            daemon_.resetStats();
            return {};
        }
        if (method == "restartEngine") {
            daemon_.requestRestart();
            return {};
        }
        if (method == "forceRestartEngine") {
            daemon_.requestForceRestart();
            return {};
        }
        if (method == "usbIpDriverStatus") return {true, daemon_.usbIpDriverStatus()};
        if (method == "installUsbIpDriver") {
            std::string error;
            if (!daemon_.installUsbIpDriver(error, true)) return rpcError(400, error);
            return {true, daemon_.usbIpDriverStatus()};
        }
        if (method == "extensions") return {true, daemon_.extensions()};
        if (method == "rescanExtensions") {
            daemon_.rescanExtensions();
            return {true, daemon_.extensions()};
        }
        if (method == "setExtensionEnabled") {
            std::string error;
            if (!daemon_.setExtensionEnabled(params.at("key").get<std::string>(),
                                              params.at("enabled").get<bool>(), error)) {
                return rpcError(404, error);
            }
            return {true, daemon_.extensions()};
        }
        if (method == "saveExtensionSettings") {
            std::string error;
            if (!daemon_.setExtensionSettings(params.at("key").get<std::string>(),
                                               params.value("values", json::object()), error)) {
                return rpcError(400, error);
            }
            return {};
        }
        if (method == "deleteExtension") {
            std::string error;
            if (!daemon_.removeExtension(params.at("key").get<std::string>(), error)) {
                return rpcError(409, error);
            }
            return {true, daemon_.extensions()};
        }
        if (method == "installExtensionPath") {
            const std::string raw_path = params.at("path").get<std::string>();
            const std::wstring wide_path = widen(raw_path);
            if (raw_path.empty() || wide_path.empty()) {
                return rpcError(400, "the selected extension path is invalid");
            }
            std::string error;
            if (!daemon_.installExtensionFromPath(std::filesystem::path(wide_path), error)) {
                return rpcError(400, error);
            }
            return {true, daemon_.extensions()};
        }
        if (method == "loadExtensionPreset") {
            const auto spec = daemon_.extensionPreset(params.at("key").get<std::string>(),
                                                      params.at("name").get<std::string>());
            if (!spec) return rpcError(404, "no such graph");
            const auto result = daemon_.applyGraph(*spec);
            return result.ok ? RpcReply{} : rpcError(422, result.error);
        }
        if (method == "extensionUiManifest") {
            const auto manifest = daemon_.extensionUiManifest(params.at("key").get<std::string>());
            return manifest ? RpcReply{true, *manifest}
                            : rpcError(404, "extension has no editor module");
        }
        if (method == "callExtensionUi") {
            const auto result = daemon_.callExtensionUi(
                params.at("key").get<std::string>(), params.at("method").get<std::string>(),
                params.value("data", json(nullptr)));
            return result.ok ? RpcReply{true, result.data} : rpcError(502, result.error);
        }
        return rpcError(404, "unknown desktop method: " + method);
    }

    void drainReplies()
    {
        std::deque<std::string> replies;
        {
            const std::lock_guard<std::mutex> lock(rpc_mutex_);
            replies.swap(rpc_replies_);
        }
        for (const std::string &reply : replies) postJson(reply);
    }

    void pushEvents()
    {
        if (webview_ == nullptr) return;
        postJson(json{{"kind", "event"}, {"event", "telemetry"},
                      {"data", daemon_.telemetry()}}
                     .dump());
        if (++event_tick_ % 5U != 0) return;
        const json events = daemon_.extensionEventsAfter(extension_cursor_);
        for (const json &event : events) {
            postJson(json{{"kind", "event"}, {"event", "extension"}, {"data", event}}.dump());
        }
    }

    void postJson(const std::string &message)
    {
        if (webview_ != nullptr) webview_->PostWebMessageAsJson(widen(message).c_str());
    }

    void postReply(const json &id, RpcReply reply)
    {
        json message{{"kind", "response"}, {"id", id}, {"ok", reply.ok}};
        if (reply.ok) {
            message["data"] = std::move(reply.data);
        } else {
            message["status"] = reply.status;
            message["error"] = std::move(reply.error);
        }
        postJson(message.dump());
    }

    HRESULT onResourceRequested(ICoreWebView2WebResourceRequestedEventArgs *args)
    {
        ComPtr<ICoreWebView2WebResourceRequest> request;
        LPWSTR raw_uri = nullptr;
        if (FAILED(args->get_Request(&request)) || request == nullptr
            || FAILED(request->get_Uri(&raw_uri)) || raw_uri == nullptr) {
            return S_OK;
        }
        std::string uri = narrow(raw_uri);
        CoTaskMemFree(raw_uri);
        constexpr std::string_view origin = "https://avc.local/";
        if (!uri.starts_with(origin)) return S_OK;
        std::string path = uri.substr(origin.size());
        if (const std::size_t query = path.find_first_of("?#"); query != std::string::npos) {
            path.resize(query);
        }

        std::string bytes;
        std::string mime;
        bool found = false;
        constexpr std::string_view extension_prefix = "__avc/extensions/";
        if (path.starts_with(extension_prefix)) {
            std::string_view rest(path.data() + extension_prefix.size(),
                                  path.size() - extension_prefix.size());
            const std::size_t first = rest.find('/');
            const std::size_t second = first == std::string_view::npos
                                           ? std::string_view::npos
                                           : rest.find('/', first + 1);
            if (first != std::string_view::npos && second != std::string_view::npos) {
                const auto key = percentDecode(rest.substr(0, first));
                const auto digest = percentDecode(rest.substr(first + 1, second - first - 1));
                const auto asset_path = percentDecode(rest.substr(second + 1));
                if (key && digest && asset_path) {
                    if (const auto asset = daemon_.extensionUiAsset(*key, *digest, *asset_path)) {
                        bytes = asset->data;
                        mime = asset->mime_type;
                        found = true;
                    }
                }
            }
        } else if (const auto decoded = percentDecode(path)) {
            found = loadEditorAsset(*decoded, bytes, mime);
        }

        const int status = found ? 200 : 404;
        if (!found) {
            bytes = "Not found\n";
            mime = "text/plain; charset=utf-8";
        }
        spdlog::debug("desktop: resource /{} -> {} ({} bytes, {})", path, status,
                      bytes.size(), mime);
        const std::string headers = "Content-Type: " + mime
                                    + "\r\nCache-Control: no-store\r\n"
                                      "X-Content-Type-Options: nosniff\r\n";
        const ComPtr<IStream> stream = streamFor(bytes);
        ComPtr<ICoreWebView2WebResourceResponse> response;
        if (stream != nullptr
            && SUCCEEDED(environment_->CreateWebResourceResponse(
                stream.Get(), status, found ? L"OK" : L"Not Found", widen(headers).c_str(),
                &response))) {
            args->put_Response(response.Get());
        }
        return S_OK;
    }

    bool loadEditorAsset(std::string path, std::string &bytes, std::string &mime) const
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        while (!path.empty() && path.front() == '/') path.erase(path.begin());
        if (path.empty() || path.back() == '/') path += "index.html";
        const auto unsafe = std::filesystem::path(path);
        if (unsafe.is_absolute() || unsafe.has_root_name()) return false;
        for (const auto &part : unsafe) {
            if (part == "..") return false;
        }

        if (!ui_dir_.empty()) {
            auto read = [&](const std::string &name) -> bool {
                const std::filesystem::path file = std::filesystem::path(ui_dir_) / widen(name);
                std::ifstream input(file, std::ios::binary);
                if (!input) return false;
                bytes.assign(std::istreambuf_iterator<char>(input), {});
                mime = mimeFor(name);
                return true;
            };
            if (read(path)) return true;
            return read("index.html");
        }

        const ui::Asset *asset = ui::find(path);
        if (asset == nullptr) asset = ui::find("index.html");
        if (asset == nullptr) return false;
        bytes.assign(asset->data);
        mime = mimeFor(asset->path);
        return true;
    }

    void resizeWebView()
    {
        const HWND window = window_.load(std::memory_order_acquire);
        if (window == nullptr || controller_ == nullptr) return;
        RECT bounds{};
        GetClientRect(window, &bounds);
        controller_->put_Bounds(bounds);
    }

    void signalReady(bool okay)
    {
        const std::lock_guard<std::mutex> lock(ready_mutex_);
        if (ready_reported_) return;
        ready_ok_ = okay;
        ready_reported_ = true;
        ready_cv_.notify_all();
    }

    app::Daemon &daemon_;
    std::string ui_dir_;
    PresetStore presets_;

    std::thread thread_;
    std::atomic<HWND> window_{nullptr};
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    bool ready_reported_ = false;
    bool ready_ok_ = false;

    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    EventRegistrationToken resource_token_{};
    EventRegistrationToken message_token_{};
    EventRegistrationToken navigation_token_{};
    EventRegistrationToken navigation_completed_token_{};
    EventRegistrationToken new_window_token_{};

    std::thread rpc_thread_;
    std::mutex rpc_mutex_;
    std::condition_variable rpc_cv_;
    std::deque<json> rpc_requests_;
    std::deque<std::string> rpc_replies_;
    bool stop_rpc_ = false;

    std::uint64_t extension_cursor_ = 0;
    unsigned event_tick_ = 0;
};

DesktopHost::DesktopHost(app::Daemon &daemon, std::string ui_dir)
    : impl_(std::make_unique<Impl>(daemon, std::move(ui_dir)))
{
}

DesktopHost::~DesktopHost() = default;

bool DesktopHost::start()
{
    return impl_->start();
}

void DesktopHost::stop()
{
    impl_->stop();
}

}

#endif
