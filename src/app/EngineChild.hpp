#pragma once

#include "app/Ipc.hpp"
#include "audio/HostAudio.hpp"
#include "ext/ExtensionLoader.hpp"
#include "graph/GraphHost.hpp"
#include "types/Audio.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace avc::app {

struct Options;

class EngineChild {
public:
    EngineChild(const Options &options, IpcChannel::NativeHandle read_handle,
                IpcChannel::NativeHandle write_handle);

    int run();

private:
    void loadExtensions();

    bool startAudio();
    void announce();
    void publishTelemetry();

    void report(const audio::BackendStats &backend, const graph::GraphStats &graph,
                const std::vector<graph::MeterReading> &meters);

    bool dispatch(const nlohmann::json &message);
    void applyGraph(const nlohmann::json &message);

    const Options &options_;
    IpcChannel channel_;
    audio::HostBackend backend_;
    ext::ExtensionLoader extensions_;
    std::unique_ptr<graph::GraphHost> host_;

    int frames_since_report_ = 0;
    bool sched_reported_ = false;
};

}
