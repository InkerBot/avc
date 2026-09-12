#include "app/Daemon.hpp"

#include "app/Application.hpp"
#include "control/ControlPlane.hpp"
#ifdef _WIN32
#include "control/DesktopHost.hpp"
#include "audio/UsbIpAudio.hpp"
#endif
#include "graph/GraphCompiler.hpp"
#include "log/Log.hpp"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fstream>
#include <system_error>
#include <thread>

namespace avc::app {

extern std::atomic<bool> g_running;

namespace {

using nlohmann::json;

constexpr int kPollMs = 200;

constexpr auto kApplyTimeout = std::chrono::seconds(10);
constexpr auto kUiTimeout = std::chrono::seconds(15);
constexpr std::size_t kMaxUiEvents = 256;
constexpr std::size_t kMaxUiPending = 64;

constexpr auto kRapidFailureWindow = std::chrono::seconds(10);
constexpr int kMaxRapidFailures = 5;

constexpr int kEngineFd = 3;

constexpr auto kStopGrace = std::chrono::milliseconds(2000);

std::optional<std::string> decodeBase64(std::string_view input)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (input.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(input.size() / 4 * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        std::uint32_t bits = 0;
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = input[i + static_cast<std::size_t>(j)];
            if (c == '=') {
                ++padding;
                bits <<= 6U;
            } else {
                const std::size_t at = alphabet.find(c);
                if (at == std::string_view::npos || padding != 0) return std::nullopt;
                bits = (bits << 6U) | static_cast<std::uint32_t>(at);
            }
        }
        out.push_back(static_cast<char>((bits >> 16U) & 0xffU));
        if (padding < 2) out.push_back(static_cast<char>((bits >> 8U) & 0xffU));
        if (padding < 1) out.push_back(static_cast<char>(bits & 0xffU));
    }
    return out;
}

std::string uiDigest(const json &assets)
{
    std::uint64_t hash = 1469598103934665603ULL;
    const auto add = [&hash](std::string_view value) {
        for (const unsigned char byte : value) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
    };
    for (const json &asset : assets) {
        add(asset.value("path", std::string{}));
        add(asset.value("data", std::string{}));
    }
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

#ifndef _WIN32
int highestFd()
{
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY) {
        return 4096;
    }
    return static_cast<int>(limit.rlim_cur > 65536 ? 65536 : limit.rlim_cur);
}
#else
std::wstring utf8ToWide(const std::string &text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring(text.begin(), text.end());
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

std::wstring quoteWindowsArg(const std::wstring &arg)
{
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(c);
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(c);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}
#endif

}

const char *engineStateName(EngineState state) noexcept
{
    switch (state) {
    case EngineState::Starting: return "starting";
    case EngineState::Up:       return "up";
    case EngineState::Down:     return "down";
    }
    return "down";
}

Daemon::Daemon(Options &options)
    : options_(options), published_(session_), state_(control::StateStore::defaultDirectory()),
      store_(options.no_extensions ? std::vector<std::string>{} : options.extension_dirs),
      manifest_path_(state_.directory() / "engine-extensions.json")
{
}

Daemon::~Daemon()
{
    stopEngine();
}

bool Daemon::startEngine()
{
#ifndef _WIN32
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        spdlog::error("socketpair failed: {}", std::strerror(errno));
        return false;
    }

#endif
    // Anything left from an earlier run would be attributed to this one.
    std::error_code ec;
    std::filesystem::remove(state_.crashPath(), ec);

    bool have_manifest = false;
    if (!options_.no_extensions) {
        const std::lock_guard<std::mutex> lock(ext_mutex_);
        have_manifest = store_.writeManifest(manifest_path_);
    }

    std::vector<std::string> storage = options_.argv;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE child_read = nullptr;
    HANDLE parent_write = nullptr;
    HANDLE parent_read = nullptr;
    HANDLE child_write = nullptr;
    if (!CreatePipe(&child_read, &parent_write, &security, 0)
        || !CreatePipe(&parent_read, &child_write, &security, 0)) {
        spdlog::error("CreatePipe failed: {}", GetLastError());
        if (child_read) CloseHandle(child_read);
        if (parent_write) CloseHandle(parent_write);
        if (parent_read) CloseHandle(parent_read);
        if (child_write) CloseHandle(child_write);
        return false;
    }
    SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0);
    storage.push_back("--engine-read-handle="
                      + std::to_string(reinterpret_cast<std::uintptr_t>(child_read)));
    storage.push_back("--engine-write-handle="
                      + std::to_string(reinterpret_cast<std::uintptr_t>(child_write)));
#else
    // Everything the child needs is built here: after fork() in a process with
    // threads in it, only async-signal-safe calls are legal, and allocating is
    // not one of them.
    storage.push_back("--engine-fd=" + std::to_string(kEngineFd));
#endif
    if (have_manifest) {
        storage.push_back("--extension-manifest=" + manifest_path_.string());
    }

#ifdef _WIN32
    wchar_t executable[32768]{};
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable, 32768);
    if (executable_length == 0 || executable_length == 32768) {
        spdlog::error("cannot find the running executable: {}", GetLastError());
        CloseHandle(child_read);
        CloseHandle(child_write);
        CloseHandle(parent_read);
        CloseHandle(parent_write);
        return false;
    }
    std::wstring command = quoteWindowsArg(std::wstring(executable, executable_length));
    for (std::size_t i = 1; i < storage.size(); ++i) {
        command.push_back(L' ');
        command += quoteWindowsArg(utf8ToWide(storage[i]));
    }
    command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, nullptr, &startup, &process);
    CloseHandle(child_read);
    CloseHandle(child_write);
    if (!created) {
        spdlog::error("CreateProcess failed: {}", GetLastError());
        CloseHandle(parent_read);
        CloseHandle(parent_write);
        return false;
    }
    CloseHandle(process.hThread);
    channel_.reset(reinterpret_cast<IpcChannel::NativeHandle>(parent_read),
                   reinterpret_cast<IpcChannel::NativeHandle>(parent_write));
    child_ = process.hProcess;
    child_pid_ = process.dwProcessId;
#else
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (std::string &argument : storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const int last_fd = highestFd();
    const pid_t parent = ::getpid();

    const pid_t pid = ::fork();
    if (pid < 0) {
        spdlog::error("fork failed: {}", std::strerror(errno));
        ::close(pair[0]);
        ::close(pair[1]);
        return false;
    }

    if (pid == 0) {
        if (pair[1] != kEngineFd) {
            ::dup2(pair[1], kEngineFd);
        }
        // The HTTP listening socket is in here, and an engine holding it would
        // keep the port bound after the daemon that answers on it has gone.
        for (int fd = kEngineFd + 1; fd < last_fd; ++fd) {
            ::close(fd);
        }
        if (pair[1] != kEngineFd) {
            ::close(pair[1]);
        }
        ::close(pair[0]);

        // An engine whose daemon has gone is answerable to nobody and publishes
        // nothing; it should not outlive it even by accident.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent) {
            ::_exit(1);
        }

        ::execv("/proc/self/exe", argv.data());
        ::_exit(127);
    }

    ::close(pair[1]);
    channel_.reset(pair[0]);
    child_ = pid;
#endif
    started_at_ = std::chrono::steady_clock::now();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        engine_state_ = EngineState::Starting;
    }
#ifdef _WIN32
    spdlog::info("engine started, pid {}", child_pid_);
#else
    spdlog::info("engine started, pid {}", pid);
#endif
    return true;
}

void Daemon::stopEngine(bool force) noexcept
{
    if (!hasChild()) {
        return;
    }
    if (!force) channel_.send(json{{"t", "shutdown"}});

#ifdef _WIN32
    const DWORD waited = force ? WAIT_TIMEOUT
                               : WaitForSingleObject(static_cast<HANDLE>(child_),
                                                     static_cast<DWORD>(kStopGrace.count()));
    if (waited == WAIT_TIMEOUT) {
        if (force) {
            spdlog::warn("force-killing engine pid {}", child_pid_);
        } else {
            spdlog::warn("engine did not stop on its own; killing pid {}", child_pid_);
        }
        TerminateProcess(static_cast<HANDLE>(child_), 1);
        WaitForSingleObject(static_cast<HANDLE>(child_), INFINITE);
    }
    CloseHandle(static_cast<HANDLE>(child_));
    child_ = nullptr;
    child_pid_ = 0;
#else
    if (!force) {
        const auto deadline = std::chrono::steady_clock::now() + kStopGrace;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (::waitpid(child_, &status, WNOHANG) == child_) {
                child_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (child_ >= 0) {
        if (force) {
            spdlog::warn("force-killing engine pid {}", child_);
        } else {
            spdlog::warn("engine did not stop on its own; killing pid {}", child_);
        }
        ::kill(child_, SIGKILL);
        int status = 0;
        ::waitpid(child_, &status, 0);
        child_ = -1;
    }
#endif

    channel_.close();
    const std::lock_guard<std::mutex> lock(mutex_);
    engine_state_ = EngineState::Down;
    inflight_.active = false;
    if (inflight_.awaited) {
        reply_ready_ = true;
        reply_ok_ = false;
        reply_error_ = "the engine stopped before it answered";
        reply_cv_.notify_all();
    }
    failUiRequests("the engine stopped before it answered");
}

void Daemon::reapEngine() noexcept
{
    if (!hasChild()) {
        return;
    }

#ifdef _WIN32
    DWORD exit_code = STILL_ACTIVE;
    const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(child_),
                                             static_cast<DWORD>(kStopGrace.count()));
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(static_cast<HANDLE>(child_), 1);
        WaitForSingleObject(static_cast<HANDLE>(child_), INFINITE);
    }
    GetExitCodeProcess(static_cast<HANDLE>(child_), &exit_code);
    if (exit_code != 0) {
        spdlog::error("engine exited with status {}", exit_code);
        blameExtension();
    } else {
        spdlog::info("engine exited");
    }
    CloseHandle(static_cast<HANDLE>(child_));
    child_ = nullptr;
    child_pid_ = 0;
#else
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + kStopGrace;
    while (::waitpid(child_, &status, WNOHANG) != child_) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(child_, SIGKILL);
            ::waitpid(child_, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (WIFSIGNALED(status)) {
        spdlog::error("engine died on signal {} ({})", WTERMSIG(status),
                      ::strsignal(WTERMSIG(status)));
        blameExtension();
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        spdlog::error("engine exited with status {}", WEXITSTATUS(status));
    } else {
        spdlog::info("engine exited");
    }

    child_ = -1;
#endif
    const bool rapid = std::chrono::steady_clock::now() - started_at_ < kRapidFailureWindow;
    rapid_failures_ = rapid ? rapid_failures_ + 1 : 0;
    channel_.close();

    const std::lock_guard<std::mutex> lock(mutex_);
    engine_state_ = EngineState::Down;
    inflight_.active = false;
    if (inflight_.awaited) {
        reply_ready_ = true;
        reply_ok_ = false;
        reply_error_ = "the engine died before it answered";
        reply_cv_.notify_all();
    }
    failUiRequests("the engine died before it answered");
}

bool Daemon::givingUp() const noexcept
{
    return rapid_failures_ >= kMaxRapidFailures;
}

void Daemon::blameExtension() noexcept
{
    const std::filesystem::path crash = state_.crashPath();
    std::string culprit;
    {
        std::ifstream file(crash);
        std::getline(file, culprit);
    }
    std::error_code ec;
    std::filesystem::remove(crash, ec);

    if (culprit.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(ext_mutex_);
    store_.quarantine(culprit);
}

void Daemon::dispatch(const nlohmann::json &message)
{
    const std::string type = message.value("t", std::string{});

    if (type == "hello") {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            descriptors_ = message.value("descriptors", json::array());
            extensions_ = message.value("extensions", json::array());
            engine_state_ = EngineState::Up;
        }
        spdlog::info("engine is up ({} node types)", descriptors().size());

        graph::GraphSpec spec;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            spec = spec_;
        }
        if (std::unique_lock<std::mutex> serial(apply_mutex_, std::try_to_lock);
            serial.owns_lock()) {
            std::string error;
            if (!sendGraph(spec, false, error)) {
                spdlog::error("cannot install the graph: {}", error);
            }
        }
    } else if (type == "telemetry") {
        const std::lock_guard<std::mutex> lock(mutex_);
        telemetry_ = message.value("data", json::object());
    } else if (type == "graph.ok") {
        onGraphReply(message, true);
    } else if (type == "graph.err") {
        onGraphReply(message, false);
    } else if (type == "extension.ui.reply") {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = ui_pending_.find(message.value("id", std::uint64_t{0}));
        if (found != ui_pending_.end() && !found->second.done) {
            found->second.done = true;
            found->second.ok = message.value("ok", false);
            found->second.data = message.value("data", json(nullptr));
            found->second.error = message.value("error", std::string{});
            ui_reply_cv_.notify_all();
        }
    } else if (type == "extension.ui.event") {
        const std::lock_guard<std::mutex> lock(mutex_);
        ui_events_.push_back({++next_ui_event_,
                              {{"extension", message.value("extension", std::string{})},
                               {"event", message.value("event", std::string{})},
                               {"data", message.value("data", json(nullptr))}}});
        while (ui_events_.size() > kMaxUiEvents) ui_events_.pop_front();
    } else {
        spdlog::warn("daemon: ignoring unknown message '{}'", type);
    }
}

void Daemon::failUiRequests(std::string_view error)
{
    for (auto &[id, pending] : ui_pending_) {
        (void)id;
        if (!pending.done) {
            pending.done = true;
            pending.ok = false;
            pending.error = error;
        }
    }
    ui_reply_cv_.notify_all();
}

void Daemon::onGraphReply(const nlohmann::json &message, bool ok)
{
    const std::uint64_t id = message.value("id", static_cast<std::uint64_t>(0));
    const std::string error = message.value("error", std::string{});

    const std::lock_guard<std::mutex> lock(mutex_);
    if (!inflight_.active || inflight_.id != id) {
        // A reply to a graph that has already been given up on -- the engine
        // was restarted, or the request timed out. Nothing to do with it.
        if (!ok) {
            spdlog::warn("engine rejected a graph nobody is waiting for: {}", error);
        }
        return;
    }

    if (ok) {
        spec_ = std::move(inflight_.spec);
        live_devices_ = std::move(inflight_.devices);
        devices_to_retire_ = true;
        spec_dirty_ = true;
        graph_error_.clear();
    } else if (inflight_.awaited) {
        spdlog::warn("engine rejected the graph, keeping the running one: {}", error);
    } else {
        // Nobody asked for this one: it is the graph being put back after a
        // restart, and it did not go back. Whoever is looking at the editor has
        // to be told, because their audio has just stopped.
        graph_error_ = error;
        spdlog::error("the engine will not run the current graph: {}", error);
    }

    inflight_.active = false;
    reply_ready_ = true;
    reply_ok_ = ok;
    reply_error_ = error;
    reply_cv_.notify_all();
}

void Daemon::retireDevices()
{
    std::vector<audio::VirtualDeviceRequest> live;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!devices_to_retire_) {
            return;
        }
        devices_to_retire_ = false;
        live = live_devices_;
    }
    published_.retain(live);
}

std::vector<audio::VirtualDeviceRequest>
Daemon::virtualDevicesFor(const graph::GraphSpec &spec) const
{
    std::vector<audio::IoRequest> requests;
    graph::GraphCompiler::virtualDevices(spec, requests);

    std::vector<audio::VirtualDeviceRequest> out;
    out.reserve(requests.size());
    for (const audio::IoRequest &request : requests) {
        out.push_back({request.node, request.target, request.kind, request.channels,
                       options_.format.sample_rate});
    }
    return out;
}

bool Daemon::sendGraph(const graph::GraphSpec &spec, bool awaited, std::string &error)
{
    const std::vector<audio::VirtualDeviceRequest> wanted = virtualDevicesFor(spec);

    // Devices first, and only ever added here: the engine has not accepted this
    // graph yet, and a graph it goes on to reject must not have cost anyone the
    // microphone they were already using.
    if (!published_.ensure(wanted, error)) {
        return false;
    }

    std::uint64_t id = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (engine_state_ == EngineState::Down) {
            error = "the engine is not running";
            return false;
        }
        id = ++next_request_;
        inflight_ = Inflight{true, awaited, id, spec, wanted};
        reply_ready_ = false;
    }

    if (!channel_.send(json{{"t", "graph"},
                            {"id", id},
                            {"spec", json::parse(spec.dump(0), nullptr, false)}})) {
        const std::lock_guard<std::mutex> lock(mutex_);
        inflight_.active = false;
        error = "the engine is not listening";
        return false;
    }
    return true;
}

Daemon::ApplyResult Daemon::applyGraph(const graph::GraphSpec &spec)
{
    const std::lock_guard<std::mutex> serial(apply_mutex_);

    std::string error;
    if (!sendGraph(spec, true, error)) {
        return {false, error};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!reply_cv_.wait_for(lock, kApplyTimeout, [this] { return reply_ready_; })) {
        inflight_.active = false;
        return {false, "the engine did not answer"};
    }
    return {reply_ok_, reply_error_};
}

bool Daemon::setParam(const std::string &node, const std::string &param, float value)
{
    if (!std::isfinite(value)) return false;

    const std::lock_guard<std::mutex> lock(mutex_);
    auto spec_node = std::find_if(spec_.nodes.begin(), spec_.nodes.end(), [&](const auto &entry) {
        return entry.id == node;
    });
    if (spec_node == spec_.nodes.end()) return false;

    const auto descriptor = std::find_if(descriptors_.begin(), descriptors_.end(),
                                         [&](const json &entry) {
                                             return entry.value("type", std::string{})
                                                    == spec_node->type;
                                         });
    if (descriptor == descriptors_.end()) return false;
    const json params = descriptor->value("params", json::array());
    const auto described = std::find_if(params.begin(), params.end(), [&](const json &entry) {
        return entry.value("name", std::string{}) == param;
    });
    if (described == params.end()) return false;
    const float min = described->value("min", value);
    const float max = described->value("max", value);
    if (value < min || value > max) return false;

    if (!channel_.send(json{{"t", "param"}, {"node", node}, {"param", param}, {"value", value}})) {
        return false;
    }

    spec_node->params[param] = value;
    spec_dirty_ = true;
    return true;
}

void Daemon::setProfiling(bool enabled)
{
    channel_.send(json{{"t", "profiling"}, {"enabled", enabled}});
}

void Daemon::resetStats()
{
    channel_.send(json{{"t", "stats.reset"}});
}

void Daemon::requestRestart()
{
    RestartRequest expected = RestartRequest::None;
    (void)restart_requested_.compare_exchange_strong(
        expected, RestartRequest::Graceful, std::memory_order_relaxed);
}

void Daemon::requestForceRestart()
{
    // A forced request must never be downgraded by a simultaneous ordinary
    // restart request from an extension setting change.
    restart_requested_.store(RestartRequest::Force, std::memory_order_relaxed);
}

nlohmann::json Daemon::usbIpDriverStatus() const
{
#ifdef _WIN32
    const audio::UsbIpDriverStatus status = audio::queryUsbIpDriverStatus();
    return json{{"supported", true},
                {"clientPresent", status.client_present},
                {"driverReady", status.driver_ready},
                {"compatible", status.compatible},
                {"installerAvailable", status.installer_available},
                {"version", status.version.empty() ? json(nullptr) : json(status.version)},
                {"error", status.error.empty() ? json(nullptr) : json(status.error)}};
#else
    return json{{"supported", false},
                {"clientPresent", false},
                {"driverReady", false},
                {"compatible", false},
                {"installerAvailable", false},
                {"version", nullptr},
                {"error", nullptr}};
#endif
}

bool Daemon::installUsbIpDriver(std::string &error, bool trusted_local, void *owner_window)
{
    const bool loopback = options_.http_bind == "127.0.0.1" || options_.http_bind == "::1"
                          || options_.http_bind == "localhost";
    if (!trusted_local && !loopback) {
        error = "USB/IP driver installation is only allowed from the local desktop or loopback";
        return false;
    }
#ifdef _WIN32
    if (!audio::installBundledUsbIpDriver(error, owner_window)) return false;
    requestForceRestart();
    return true;
#else
    (void)owner_window;
    error = "USB/IP driver installation is available only on Windows";
    return false;
#endif
}

graph::GraphSpec Daemon::spec() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return spec_;
}

nlohmann::json Daemon::descriptors() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return descriptors_;
}

nlohmann::json Daemon::telemetry() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    // Desktop WebView2 starts consuming events immediately, before the child
    // has necessarily produced its first telemetry frame. Keep the public
    // shape complete during that interval so every transport sees the same
    // stable contract instead of a partial {engine, graphError} object.
    json out{{"cycles", 0},
             {"xruns", 0},
             {"quantum", options_.format.quantum},
             {"sampleRate", options_.format.sample_rate},
             {"blockMs", 1000.0 * options_.format.quantum / options_.format.sample_rate},
             {"ioLatencyMs", 0.0},
             {"captureLatencyMs", 0.0},
             {"playbackLatencyMs", 0.0},
             {"clockSource", "timer"},
             {"graphLatencyFrames", 0},
             {"graphLatencyMs", 0.0},
             {"totalLatencyMs", 0.0},
             {"jitterUs", 0.0},
             {"dspUs", 0.0},
             {"dspUsLast", 0.0},
             {"dspUsAvg", 0.0},
             {"load", 0.0},
             {"loadAvg", 0.0},
             {"profiling", false},
             {"nodeCost", json::object()},
             {"nodeLatencyMs", json::object()},
             {"nodeStatus", json::object()},
             {"realtime", false},
             {"generation", 0},
             {"swaps", 0},
             {"nodes", 0},
             {"bufferSlots", 0},
             {"droppedParams", 0},
             {"domains", json::array()},
             {"outputLatencyMs", json::object()},
             {"meters", json::object()},
             {"scopes", json::object()},
             {"texts", json::object()},
             {"scopeBandsHz", json::array()}};
    if (telemetry_.is_object()) {
        for (const auto &[key, value] : telemetry_.items()) out[key] = value;
    }
    // The engine cannot report whether it is there. Its graph generation is
    // retained from the last telemetry frame while it is restarting.
    out["engine"] = engineStateName(engine_state_);
    out["graphError"] = graph_error_;
    return out;
}

nlohmann::json Daemon::extensions() const
{
    // The engine's account first, then the store's -- never the other way
    // round, so the two locks are only ever taken in one order.
    json reported;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        reported = extensions_;
    }
    const std::lock_guard<std::mutex> lock(ext_mutex_);
    return store_.describe(reported);
}

std::optional<nlohmann::json> Daemon::extensionUiManifest(const std::string &key) const
{
    const json listed = extensions();
    const auto public_entry = std::find_if(listed.begin(), listed.end(), [&](const json &entry) {
        return entry.value("key", std::string{}) == key && entry.value("state", std::string{}) == "loaded";
    });
    if (public_entry == listed.end()) return std::nullopt;

    json reported;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::string path = public_entry->value("path", std::string{});
        const auto found = std::find_if(extensions_.begin(), extensions_.end(), [&](const json &entry) {
            return entry.value("path", std::string{}) == path;
        });
        if (found == extensions_.end()) return std::nullopt;
        reported = *found;
    }
    const json ui = reported.value("ui", json::object());
    const json assets = ui.value("assets", json::array());
    const std::string entry = ui.value("entry", std::string{});
    if (entry.empty()) return std::nullopt;

    json exposed = json::array();
    for (const json &asset : assets) {
        exposed.push_back({{"path", asset.value("path", std::string{})},
                           {"mime", asset.value("mime", std::string{})}});
    }
    return json{{"key", key},
                {"id", reported.value("id", std::string{})},
                {"entry", entry},
                {"digest", uiDigest(assets)},
                {"nodeTypes", reported.value("nodeTypes", json::array())},
                {"assets", std::move(exposed)}};
}

std::optional<Daemon::ExtensionUiAsset>
Daemon::extensionUiAsset(const std::string &key, const std::string &digest,
                         const std::string &path) const
{
    const json listed = extensions();
    const auto public_entry = std::find_if(listed.begin(), listed.end(), [&](const json &entry) {
        return entry.value("key", std::string{}) == key && entry.value("state", std::string{}) == "loaded";
    });
    if (public_entry == listed.end()) return std::nullopt;

    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string wanted_path = public_entry->value("path", std::string{});
    const auto reported = std::find_if(extensions_.begin(), extensions_.end(), [&](const json &entry) {
        return entry.value("path", std::string{}) == wanted_path;
    });
    if (reported == extensions_.end()) return std::nullopt;
    const json assets = reported->value("ui", json::object()).value("assets", json::array());
    if (uiDigest(assets) != digest) return std::nullopt;
    const auto asset = std::find_if(assets.begin(), assets.end(), [&](const json &entry) {
        return entry.value("path", std::string{}) == path;
    });
    if (asset == assets.end()) return std::nullopt;
    const auto bytes = decodeBase64(asset->value("data", std::string{}));
    if (!bytes) return std::nullopt;
    return ExtensionUiAsset{asset->value("mime", std::string{"application/octet-stream"}),
                            *bytes};
}

Daemon::ExtensionUiCallResult Daemon::callExtensionUi(const std::string &key,
                                                      const std::string &method,
                                                      const nlohmann::json &data)
{
    if (method.empty() || method.size() > 96
        || !std::all_of(method.begin(), method.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
           })) {
        return {false, nullptr, "invalid extension method"};
    }
    if (data.dump().size() > 256U * 1024U) {
        return {false, nullptr, "extension message is larger than 256 KiB"};
    }
    const json listed = extensions();
    const auto public_entry = std::find_if(listed.begin(), listed.end(), [&](const json &entry) {
        return entry.value("key", std::string{}) == key && entry.value("state", std::string{}) == "loaded";
    });
    if (public_entry == listed.end()) return {false, nullptr, "extension is not loaded"};
    const std::string extension = public_entry->value("id", std::string{});

    std::uint64_t id = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (engine_state_ != EngineState::Up) return {false, nullptr, "the engine is not running"};
        if (ui_pending_.size() >= kMaxUiPending) return {false, nullptr, "too many extension requests"};
        id = ++next_request_;
        ui_pending_.emplace(id, UiPending{});
    }
    if (!channel_.send({{"t", "extension.ui.request"}, {"id", id}, {"extension", extension},
                        {"method", method}, {"data", data}})) {
        const std::lock_guard<std::mutex> lock(mutex_);
        ui_pending_.erase(id);
        return {false, nullptr, "the engine is not listening"};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!ui_reply_cv_.wait_for(lock, kUiTimeout, [&] {
            const auto found = ui_pending_.find(id);
            return found == ui_pending_.end() || found->second.done;
        })) {
        ui_pending_.erase(id);
        return {false, nullptr, "extension did not answer within 15 seconds"};
    }
    const auto found = ui_pending_.find(id);
    if (found == ui_pending_.end()) return {false, nullptr, "extension request was cancelled"};
    ExtensionUiCallResult result{found->second.ok, std::move(found->second.data),
                                 std::move(found->second.error)};
    ui_pending_.erase(found);
    return result;
}

std::uint64_t Daemon::extensionEventCursor() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return next_ui_event_;
}

nlohmann::json Daemon::extensionEventsAfter(std::uint64_t &cursor) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    for (const UiEvent &event : ui_events_) {
        if (event.sequence > cursor) out.push_back(event.value);
    }
    cursor = next_ui_event_;
    return out;
}

bool Daemon::setExtensionEnabled(const std::string &key, bool enabled, std::string &error)
{
    {
        const std::lock_guard<std::mutex> lock(ext_mutex_);
        if (!store_.setEnabled(key, enabled, error)) {
            return false;
        }
    }
    requestRestart();
    return true;
}

bool Daemon::setExtensionSettings(const std::string &key, const nlohmann::json &values,
                                  std::string &error)
{
    {
        const std::lock_guard<std::mutex> lock(ext_mutex_);
        if (!store_.setSettings(key, values, error)) {
            return false;
        }
    }
    // An extension reads its settings once, when it loads. Restarting is what
    // makes that safe: a setting cannot change underneath a running node.
    requestRestart();
    return true;
}

bool Daemon::installExtension(const std::string &filename, const std::string &bytes,
                              std::string &error, bool trusted_local)
{
    if (!trusted_local && !uploadAllowed()) {
        error = "installing extensions is disabled for this control channel";
        return false;
    }
    const std::lock_guard<std::mutex> lock(ext_mutex_);
    return store_.install(filename, bytes, error);
}

bool Daemon::installExtensionFromPath(const std::filesystem::path &path, std::string &error)
{
    const std::lock_guard<std::mutex> lock(ext_mutex_);
    return store_.installFile(path, error);
}

bool Daemon::removeExtension(const std::string &key, std::string &error)
{
    const std::lock_guard<std::mutex> lock(ext_mutex_);
    return store_.remove(key, error);
}

void Daemon::rescanExtensions()
{
    const std::lock_guard<std::mutex> lock(ext_mutex_);
    store_.rescan();
}

bool Daemon::uploadAllowed() const
{
    if (options_.no_extension_upload) {
        return false;
    }
    // Anyone who can reach this port can already rebuild the audio graph; being
    // able to leave a shared object behind is a good deal worse, so it is kept
    // to the one case where "anyone" means "someone on this machine".
    return options_.http_bind == "127.0.0.1" || options_.http_bind == "::1"
           || options_.http_bind == "localhost";
}

nlohmann::json Daemon::extensionPresets() const
{
    json out = json::array();
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const json &extension : extensions_) {
        if (!extension.value("loaded", false)) {
            continue;
        }
        const std::string key =
            std::filesystem::path(extension.value("path", std::string{})).stem().string();
        for (const json &preset : extension.value("presets", json::array())) {
            out.push_back({{"extension", extension.value("id", std::string{})},
                           {"key", key},
                           {"name", preset.value("name", std::string{})}});
        }
    }
    return out;
}

std::optional<graph::GraphSpec> Daemon::extensionPreset(const std::string &key,
                                                        const std::string &name) const
{
    std::string text;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const json &extension : extensions_) {
            const std::string path = extension.value("path", std::string{});
            if (std::filesystem::path(path).stem().string() != key) {
                continue;
            }
            for (const json &preset : extension.value("presets", json::array())) {
                if (preset.value("name", std::string{}) == name) {
                    text = preset.value("json", std::string{});
                }
            }
        }
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::string error;
    auto spec = graph::GraphSpec::parse(text, error);
    if (!spec) {
        spdlog::warn("extension preset '{}' is not a usable graph: {}", name, error);
    }
    return spec;
}

bool Daemon::resolveTargets()
{
    if (options_.input_target.empty()) {
        options_.input_target = session_.defaultDeviceName(audio::Direction::Input);
    }
    if (options_.output_target.empty()) {
        options_.output_target = session_.defaultDeviceName(audio::Direction::Output);
    }

    const std::vector<audio::DeviceInfo> devices = session_.enumerateDevices();
    if (options_.input_target.empty()) {
        options_.input_target = pickFirstDevice(devices, "Audio/Source", audio::Direction::Output);
    }
    if (options_.output_target.empty()) {
        options_.output_target = pickFirstDevice(devices, "Audio/Sink", audio::Direction::Input);
    }

    // Sending the engine into its own virtual speaker would loop its monitor
    // straight back into the engine's input.
    for (const VirtualDeviceOption &device : options_.virtual_devices) {
        if (options_.output_target == device.name || options_.input_target == device.name) {
            spdlog::error("'{}' is a device this engine publishes; pass an explicit --input/"
                          "--output so it does not feed itself", device.name);
            return false;
        }
    }
    if (options_.input_target.empty() || options_.output_target.empty()) {
        spdlog::error("could not pick a capture/playback device; run with --list and pass "
                      "--input=/--output= explicitly");
        return false;
    }

    spdlog::info("capture  <- {}", options_.input_target);
    spdlog::info("playback -> {}", options_.output_target);
    return true;
}

bool Daemon::chooseInitialGraph()
{
    const std::lock_guard<std::mutex> lock(mutex_);

    if (!options_.graph_path.empty()) {
        std::string error;
        const auto spec = graph::GraphSpec::load(options_.graph_path, error);
        if (!spec) {
            spdlog::error("cannot load {}: {}", options_.graph_path, error);
            return false;
        }
        spec_ = *spec;
        watch_path_ = options_.graph_path;
        std::error_code ec;
        watch_mtime_ = std::filesystem::last_write_time(watch_path_, ec);
        spdlog::info("watching {} for changes", options_.graph_path);
        return true;
    }

    if (!options_.no_restore && state_.exists()) {
        std::string error;
        if (const auto spec = state_.load(error)) {
            spec_ = *spec;
            spdlog::info("restored the graph this engine was last running");
            return true;
        }
        spdlog::warn("cannot read the last graph, starting from the built-in one: {}", error);
    }

    spec_ = defaultSpec(options_, options_.input_target, options_.output_target);
    spdlog::info("built-in graph: {} -> gain {:+.1f} dB -> {} (use --graph for anything else)",
                 options_.input_target, options_.gain_db, options_.output_target);
    return true;
}

void Daemon::pollGraphFile()
{
    if (watch_path_.empty()) {
        return;
    }
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(watch_path_, ec);
    if (ec || stamp == watch_mtime_) {
        return;
    }
    watch_mtime_ = stamp;

    std::string error;
    const auto spec = graph::GraphSpec::load(watch_path_.string(), error);
    if (!spec) {
        spdlog::warn("graph reload failed, keeping the running graph: {}", error);
        return;
    }

    // try_lock, not lock: an HTTP thread holding this is blocked on a reply
    // that only this thread delivers. The file is still there next tick.
    std::unique_lock<std::mutex> serial(apply_mutex_, std::try_to_lock);
    if (!serial.owns_lock()) {
        watch_mtime_ = std::filesystem::file_time_type{};
        return;
    }
    if (!sendGraph(*spec, false, error)) {
        spdlog::warn("graph reload failed, keeping the running graph: {}", error);
    }
}

void Daemon::persistIfDirty()
{
    graph::GraphSpec spec;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!spec_dirty_ || !watch_path_.empty()) {
            // With --graph the file is the truth, and writing a second copy
            // would only give the next start two answers to choose between.
            spec_dirty_ = false;
            return;
        }
        spec_dirty_ = false;
        spec = spec_;
    }
    state_.save(spec);
}

int Daemon::run()
{
    if (!session_.open("avc-daemon")) {
        return 1;
    }
    if (!resolveTargets() || !chooseInitialGraph()) {
        return 1;
    }

#ifdef _WIN32
    if (options_.desktop) {
        desktop_ = std::make_unique<control::DesktopHost>(*this, options_.ui_dir);
        if (!desktop_->start()) {
            return 1;
        }
    }
#endif

    control::ControlConfig config;
    config.bind = options_.http_bind;
    config.port = options_.http_port;
    config.ui_dir = options_.ui_dir;
    if (!options_.no_http) {
        control_ = std::make_unique<control::ControlPlane>(*this, std::move(config));
        if (!control_->start()) {
            return 1;
        }
    }

    bool complained = false;
    while (g_running.load(std::memory_order_relaxed)) {
        if (!hasChild()) {
            if (givingUp()) {
                if (!complained) {
                    spdlog::error("the engine has failed {} times in a row; not starting it "
                                  "again until asked to", rapid_failures_);
                    complained = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            } else if (!startEngine()) {
                ++rapid_failures_;
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            }
        }

        if (hasChild()) {
            json message;
            const IpcChannel::Status status = channel_.receive(message, kPollMs);
            if (status == IpcChannel::Status::Closed || status == IpcChannel::Status::Error) {
                reapEngine();
            } else if (status == IpcChannel::Status::Message) {
                dispatch(message);
                while (channel_.receive(message, 0) == IpcChannel::Status::Message) {
                    dispatch(message);
                }
            }
        }

        const RestartRequest restart =
            restart_requested_.exchange(RestartRequest::None, std::memory_order_relaxed);
        if (restart != RestartRequest::None) {
            // A restart somebody asked for is not a crash loop, whatever came before.
            rapid_failures_ = 0;
            const bool force = restart == RestartRequest::Force;
            spdlog::info("{}restarting the engine", force ? "force-" : "");
            complained = false;
            stopEngine(force);
        }

        retireDevices();
        pollGraphFile();
        persistIfDirty();
    }

    spdlog::info("stopping");
    if (control_ != nullptr) {
        control_->stop();
    }
    stopEngine();
#ifdef _WIN32
    // Stopping the child also releases any desktop extension RPC waiting for
    // an engine reply, so the WebView worker can always join promptly.
    if (desktop_ != nullptr) {
        desktop_->stop();
    }
#endif
    persistIfDirty();
    published_.clear();
    session_.close();
    return 0;
}

}
