#pragma once

#include "audio/AudioBackend.hpp"
#include "ext/ExtensionLoader.hpp"
#include "graph/GraphHost.hpp"
#include "graph/GraphSpec.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace avc::control {

nlohmann::json descriptorsJson();

nlohmann::json devicesJson(const std::vector<audio::DeviceInfo> &devices);

nlohmann::json extensionsJson(const std::vector<ext::LoadedExtension> &extensions);

nlohmann::json specJson(const graph::GraphSpec &spec);

nlohmann::json telemetryJson(const audio::BackendStats &backend, const graph::GraphStats &graph,
                             const std::vector<graph::MeterReading> &meters,
                             const std::vector<graph::ScopeReading> &scopes,
                             const std::vector<graph::TextReading> &texts);

}
