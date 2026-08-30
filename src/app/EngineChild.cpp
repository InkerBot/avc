#include "app/EngineChild.hpp"

#include "app/Application.hpp"
#include "app/CrashGuard.hpp"
#include "control/Serialization.hpp"
#include "control/StateStore.hpp"
#include "log/Log.hpp"
#include "node/NodeRegistry.hpp"
#include "rt/RtThread.hpp"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/prctl.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <vector>

namespace avc::app {

extern std::atomic<bool> g_running;

namespace {

using nlohmann::json;

constexpr int kPollMs = 100;

constexpr auto kTelemetryInterval = std::chrono::milliseconds(50);

constexpr int kFramesPerReport = 40;

float linearToDb(float linear)
{
    return linear > 1e-6F ? 20.0F * std::log10(linear) : -120.0F;
}

}

EngineChild::EngineChild(const Options &options, IpcChannel::NativeHandle read_handle,
                         IpcChannel::NativeHandle write_handle)
    : options_(options), channel_(read_handle, write_handle)
{
    extensions_.setUiSink([this](const json &message) { channel_.send(message); });
}

void EngineChild::loadExtensions()
{
    node::NodeRegistry &registry = node::NodeRegistry::instance();
    if (options_.extension_manifest.empty()) {
        registry.seal();
        return;
    }

    std::ifstream file(options_.extension_manifest);
    const json manifest = json::parse(file, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        spdlog::error("cannot read the extension list at {}", options_.extension_manifest);
        registry.seal();
        return;
    }

    // Named locals rather than iterating what value() returns: those are
    // temporaries, and a loop over one reads better than it lives.
    const json listed = manifest.value("extensions", json::array());
    std::vector<ext::ExtensionRequest> requests;
    for (const json &entry : listed) {
        ext::ExtensionRequest request;
        request.path = entry.value("path", std::string{});
        if (request.path.empty()) {
            continue;
        }
        const json settings = entry.value("settings", json::object());
        for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
            request.settings[it.key()] =
                it->is_string() ? it->get<std::string>() : it->dump();
        }
        requests.push_back(std::move(request));
    }

    extensions_.loadAll(requests, manifest.value("configRoot", std::string{}));

    // Nothing may add a node type after this. The audio thread starts next, and
    // from here on the registry is read-only, which is what lets a compile hold
    // a bare pointer to a descriptor.
    registry.seal();
}

bool EngineChild::startAudio()
{
    if (!backend_.open()) {
        return false;
    }

    graph::CompileEnv env;
    env.sample_rate = options_.format.sample_rate;
    env.max_quantum = types::kMaxQuantum;
    host_ = std::make_unique<graph::GraphHost>(env, &backend_);

    backend_.setForceQuantum(options_.force_quantum);
    if (!backend_.start(options_.format, host_.get())) {
        spdlog::error("engine failed to start");
        return false;
    }
    return true;
}

void EngineChild::announce()
{
    channel_.send(json{
        {"t", "hello"},
#ifdef _WIN32
        {"pid", static_cast<std::int64_t>(GetCurrentProcessId())},
#else
        {"pid", static_cast<std::int64_t>(::getpid())},
#endif
        {"descriptors", control::descriptorsJson()},
        {"extensions", control::extensionsJson(extensions_.results())},
    });
}

void EngineChild::publishTelemetry()
{
    // Read once and use twice: meters() clears the peaks it returns, so asking
    // separately for the log would take readings away from the editor.
    const audio::BackendStats backend = backend_.stats();
    const graph::GraphStats graph = host_->stats();
    const std::vector<graph::MeterReading> meters = host_->meters();

    channel_.send(json{
        {"t", "telemetry"},
        {"data", control::telemetryJson(backend, graph, meters, host_->scopes(),
                                        host_->texts())},
    });

    if (++frames_since_report_ >= kFramesPerReport) {
        frames_since_report_ = 0;
        report(backend, graph, meters);
    }
}

void EngineChild::report(const audio::BackendStats &backend, const graph::GraphStats &graph,
                         const std::vector<graph::MeterReading> &meters)
{
    const std::uint32_t quantum =
        backend.actual_quantum > 0 ? backend.actual_quantum : backend.quantum;
    const std::uint32_t rate = backend.actual_rate > 0 ? backend.actual_rate : backend.sample_rate;
    const double budget_ns = 1e9 * static_cast<double>(quantum) / static_cast<double>(rate);
    const double load = 100.0 * static_cast<double>(backend.process_ns_max) / budget_ns;

    if (!sched_reported_ && backend.cycles > 0) {
        const rt::SchedInfo info{backend.sched_policy, backend.sched_priority, backend.audio_tid};
        if (rt::isRealtime(info)) {
            spdlog::info("audio thread: {}", rt::describe(info));
        } else {
            spdlog::warn("audio thread is NOT realtime ({})", rt::describe(info));
#ifdef _WIN32
            spdlog::warn("the WASAPI callback did not receive MMCSS/time-critical scheduling; "
                         "check that the Multimedia Class Scheduler service is running");
#else
            spdlog::warn("the graph will glitch under load. PipeWire falls back to rtkit "
                         "when the session has no RLIMIT_RTPRIO, and that grant does not "
                         "always stick.");
            spdlog::warn("fix: sudo usermod -aG pipewire $USER, then log out and back in "
                         "(/etc/security/limits.d/25-pw-rlimits.conf already grants "
                         "@pipewire rtprio 95)");
#endif
        }
        sched_reported_ = true;
    }

    spdlog::info("cycles {:<8} xrun {:<3} quantum {}@{}  dsp {:.1f}/{:.1f} us ({:.1f}%)  "
                 "gen {} swaps {} nodes {} buffers {}",
                 backend.cycles, backend.xruns, quantum, rate,
                 static_cast<double>(backend.process_ns_max) / 1000.0, budget_ns / 1000.0, load,
                 graph.generation, graph.swaps, graph.nodes, graph.buffer_slots);

    if (graph.dropped_params > 0) {
        spdlog::warn("{} parameter updates dropped: the ring is full", graph.dropped_params);
    }
    for (const graph::MeterReading &meter : meters) {
        spdlog::info("meter '{}': peak {:>6.1f} dBFS   rms {:>6.1f} dBFS", meter.node,
                     linearToDb(meter.peak), linearToDb(meter.rms));
    }
}

void EngineChild::applyGraph(const nlohmann::json &message)
{
    const std::uint64_t id = message.value("id", static_cast<std::uint64_t>(0));

    std::string error;
    const auto spec = graph::GraphSpec::parse(message.value("spec", json::object()).dump(), error);
    if (!spec || !host_->apply(*spec, error)) {
        channel_.send(json{{"t", "graph.err"}, {"id", id}, {"error", error}});
        return;
    }
    channel_.send(json{{"t", "graph.ok"}, {"id", id}});
}

bool EngineChild::dispatch(const nlohmann::json &message)
{
    const std::string type = message.value("t", std::string{});

    if (type == "graph") {
        applyGraph(message);
    } else if (type == "param") {
        host_->setParam(message.value("node", std::string{}),
                        message.value("param", std::string{}), message.value("value", 0.0F));
    } else if (type == "profiling") {
        host_->setProfiling(message.value("enabled", false));
    } else if (type == "stats.reset") {
        backend_.resetPeaks();
        host_->resetProfile();
    } else if (type == "extension.ui.request") {
        const std::uint64_t id = message.value("id", static_cast<std::uint64_t>(0));
        std::string error;
        if (!extensions_.dispatchUiRequest(message.value("extension", std::string{}), id,
                                           message.value("method", std::string{}),
                                           message.value("data", json(nullptr)).dump(), error)) {
            channel_.send({{"t", "extension.ui.reply"},
                           {"id", id},
                           {"extension", message.value("extension", std::string{})},
                           {"ok", false},
                           {"data", nullptr},
                           {"error", error}});
        }
    } else if (type == "shutdown") {
        return false;
    } else {
        spdlog::warn("engine: ignoring unknown message '{}'", type);
    }
    return true;
}

int EngineChild::run()
{
#ifndef _WIN32
    ::prctl(PR_SET_NAME, "avc-engine", 0, 0, 0);
#endif

    // Before anything third-party is mapped in, so that whatever it does next
    // is attributable. The daemon and this process agree on the path because
    // they are the same binary reading the same environment.
    installCrashGuard(control::StateStore::defaultDirectory() / "crash");

    loadExtensions();

    if (!startAudio()) {
        return 1;
    }
    announce();
    spdlog::info("engine running: {} Hz, quantum {} ({:.2f} ms/block)",
                 options_.format.sample_rate, options_.format.quantum,
                 types::quantumMs(options_.format));

    auto last_telemetry = std::chrono::steady_clock::now();
    bool asked_to_stop = false;
    bool received_graph = false;

    while (g_running.load(std::memory_order_relaxed) && !asked_to_stop) {
        json message;
        const IpcChannel::Status status = channel_.receive(message, kPollMs);
        if (status == IpcChannel::Status::Closed || status == IpcChannel::Status::Error) {
            spdlog::info("engine: the daemon closed the channel");
            break;
        }
        if (status == IpcChannel::Status::Message) {
            received_graph = received_graph
                             || message.value("t", std::string{}) == "graph";
            asked_to_stop = !dispatch(message);
            // Whatever else arrived in the same read, before going back to
            // sleep -- a slider drag is a burst, not one message.
            while (!asked_to_stop
                   && channel_.receive(message, 0) == IpcChannel::Status::Message) {
                received_graph = received_graph
                                 || message.value("t", std::string{}) == "graph";
                asked_to_stop = !dispatch(message);
            }
        }

        host_->poll();

        const auto now = std::chrono::steady_clock::now();
        // The daemon publishes virtual devices synchronously after the hello
        // handshake. Sending telemetry before its first graph request can fill
        // the reverse anonymous pipe while the daemon is busy; the subsequent
        // graph write then deadlocks both processes. A graph request is the
        // protocol point at which both sides have entered their receive loops.
        if (received_graph && now - last_telemetry >= kTelemetryInterval) {
            publishTelemetry();
            last_telemetry = now;
        }
    }

    spdlog::info("engine stopping");
    backend_.stop();
    host_->shutdown();
    backend_.close();
    return 0;
}

}
