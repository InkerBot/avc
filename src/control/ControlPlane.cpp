#include "control/ControlPlane.hpp"

#include "app/Daemon.hpp"
#include "control/Serialization.hpp"
#include "control/ExtensionStore.hpp"
#include "control/UiAssets.hpp"
#include "log/Log.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/socket.h>
#endif

#include <chrono>
#include <string_view>

namespace avc::control {
namespace {

using nlohmann::json;

constexpr auto kTelemetryInterval = std::chrono::milliseconds(50);

constexpr std::size_t kMaxUploadBytes = 64U * 1024U * 1024U;

void sendJson(httplib::Response &res, const json &body, int status = 200)
{
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void sendError(httplib::Response &res, int status, std::string_view message)
{
    sendJson(res, json{{"error", message}}, status);
}

bool parseBody(const httplib::Request &req, httplib::Response &res, json &out)
{
    out = json::parse(req.body, nullptr, false);
    if (out.is_discarded()) {
        sendError(res, 400, "body is not valid JSON");
        return false;
    }
    return true;
}

const char *mimeFor(std::string_view path)
{
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js") || path.ends_with(".mjs")) return "text/javascript; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

}

ControlPlane::ControlPlane(app::Daemon &daemon, ControlConfig config)
    : server_(std::make_unique<httplib::Server>()), daemon_(daemon),
      config_(std::move(config)), presets_(PresetStore::defaultDirectory()),
      rvc_models_(ExtensionStore::configRoot() / "avc-rvc" / "data")
{
}

ControlPlane::~ControlPlane()
{
    stop();
}

bool ControlPlane::start()
{
    registerRoutes();

    // Reuse on POSIX for quick restarts, exclusive ownership on Windows.
    // cpp-httplib would otherwise add SO_REUSEPORT where available, which lets
    // a second engine bind the same port and take a share of the requests.
    server_->set_socket_options([](auto sock) {
        const int on = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&on),
                   sizeof(on));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    });

    if (!server_->bind_to_port(config_.bind, config_.port)) {
        spdlog::error("control plane cannot bind {}:{} -- is another copy running?", config_.bind,
                      config_.port);
        return false;
    }

    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this] { server_->listen_after_bind(); });

    spdlog::info("control plane on http://{}:{}", config_.bind, config_.port);
    return true;
}

void ControlPlane::stop()
{
    if (!running_.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    server_->stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ControlPlane::registerRoutes()
{
    server_->Get("/api/descriptors", [this](const httplib::Request &, httplib::Response &res) {
        sendJson(res, daemon_.descriptors());
    });

    server_->Get("/api/devices", [this](const httplib::Request &, httplib::Response &res) {
        sendJson(res, devicesJson(daemon_.devices()));
    });

    server_->Get("/api/events", [this](const httplib::Request &, httplib::Response &res) {
        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider(
            "text/event-stream", [this](std::size_t, httplib::DataSink &sink) {
                const std::string frame = "data: " + daemon_.telemetry().dump() + "\n\n";
                if (!sink.write(frame.data(), frame.size())) {
                    return false;
                }
                std::this_thread::sleep_for(kTelemetryInterval);
                return running_.load(std::memory_order_relaxed);
            });
    });

    // Per-node timing is a measurement tool, not a monitor: it is off unless
    // someone is looking, because it costs the audio thread two clock reads a
    // node a block to be on.
    server_->Post("/api/profiling", [this](const httplib::Request &req, httplib::Response &res) {
        json body;
        if (!parseBody(req, res, body)) {
            return;
        }
        if (!body.contains("enabled") || !body["enabled"].is_boolean()) {
            sendError(res, 400, "expected {enabled}");
            return;
        }
        daemon_.setProfiling(body["enabled"].get<bool>());
        res.status = 204;
    });

    server_->Post("/api/stats/reset", [this](const httplib::Request &, httplib::Response &res) {
        daemon_.resetStats();
        res.status = 204;
    });

    // Everything that changes which code the engine is running goes through
    // here: the engine is restarted rather than modified, which is the only way
    // to unload a shared object that is safe by construction.
    server_->Post("/api/engine/restart", [this](const httplib::Request &, httplib::Response &res) {
        daemon_.requestRestart();
        res.status = 204;
    });

    server_->Post("/api/engine/restart/force",
                  [this](const httplib::Request &, httplib::Response &res) {
                      daemon_.requestForceRestart();
                      res.status = 204;
                  });

    server_->Get("/api/usbip/driver",
                 [this](const httplib::Request &, httplib::Response &res) {
                     sendJson(res, daemon_.usbIpDriverStatus());
                 });

    server_->Post("/api/usbip/driver/install",
                  [this](const httplib::Request &, httplib::Response &res) {
                      std::string error;
                      if (!daemon_.installUsbIpDriver(error)) {
                          sendError(res, 400, error);
                          return;
                      }
                      sendJson(res, daemon_.usbIpDriverStatus());
                  });

    registerGraphRoutes();
    registerPresetRoutes();
    registerExtensionRoutes();
    registerRvcRoutes();
    registerUiRoutes();
}

void ControlPlane::registerGraphRoutes()
{
    server_->Get("/api/graph", [this](const httplib::Request &, httplib::Response &res) {
        sendJson(res, json{{"spec", specJson(daemon_.spec())}});
    });

    server_->Put("/api/graph", [this](const httplib::Request &req, httplib::Response &res) {
        json body;
        if (!parseBody(req, res, body)) {
            return;
        }
        if (!body.contains("spec")) {
            sendError(res, 400, "missing 'spec'");
            return;
        }

        std::string error;
        const auto spec = graph::GraphSpec::parse(body["spec"].dump(), error);
        if (!spec) {
            sendError(res, 400, error);
            return;
        }
        const app::Daemon::ApplyResult result = daemon_.applyGraph(*spec);
        if (!result.ok) {
            sendError(res, 422, result.error);
            return;
        }
        res.status = 204;
    });

    server_->Post("/api/params", [this](const httplib::Request &req, httplib::Response &res) {
        json body;
        if (!parseBody(req, res, body)) {
            return;
        }
        const bool shaped = body.contains("node") && body.contains("param")
                            && body.contains("value") && body["value"].is_number();
        if (!shaped) {
            sendError(res, 400, "expected {node, param, value}");
            return;
        }
        const std::string node = body["node"].get<std::string>();
        const std::string param = body["param"].get<std::string>();
        if (!daemon_.setParam(node, param, body["value"].get<float>())) {
            sendError(res, 503, "the engine is not running");
            return;
        }
        res.status = 204;
    });
}

void ControlPlane::registerPresetRoutes()
{
    server_->Get("/api/presets", [this](const httplib::Request &, httplib::Response &res) {
        sendJson(res, json{{"presets", presets_.list()},
                           {"extensionPresets", daemon_.extensionPresets()}});
    });

    server_->Post(R"(/api/presets/ext/([^/]+)/(.+)/load)",
                  [this](const httplib::Request &req, httplib::Response &res) {
                      const auto spec = daemon_.extensionPreset(req.matches[1], req.matches[2]);
                      if (!spec) {
                          sendError(res, 404, "no such graph");
                          return;
                      }
                      const app::Daemon::ApplyResult result = daemon_.applyGraph(*spec);
                      if (!result.ok) {
                          sendError(res, 422, result.error);
                          return;
                      }
                      res.status = 204;
                  });

    server_->Put(R"(/api/presets/([^/]+))", [this](const httplib::Request &req,
                                                   httplib::Response &res) {
        json body;
        if (!parseBody(req, res, body)) {
            return;
        }
        if (!body.contains("spec")) {
            sendError(res, 400, "missing 'spec'");
            return;
        }
        std::string error;
        const auto spec = graph::GraphSpec::parse(body["spec"].dump(), error);
        if (!spec) {
            sendError(res, 400, error);
            return;
        }
        if (!presets_.save(req.matches[1], *spec, error)) {
            sendError(res, 400, error);
            return;
        }
        res.status = 204;
    });

    server_->Post(R"(/api/presets/([^/]+)/load)", [this](const httplib::Request &req,
                                                         httplib::Response &res) {
        std::string error;
        const auto spec = presets_.load(req.matches[1], error);
        if (!spec) {
            sendError(res, 404, error);
            return;
        }
        const app::Daemon::ApplyResult result = daemon_.applyGraph(*spec);
        if (!result.ok) {
            sendError(res, 422, result.error);
            return;
        }
        res.status = 204;
    });

    server_->Delete(R"(/api/presets/([^/]+))", [this](const httplib::Request &req,
                                                      httplib::Response &res) {
        std::string error;
        if (!presets_.remove(req.matches[1], error)) {
            sendError(res, 404, error);
            return;
        }
        res.status = 204;
    });
}

void ControlPlane::registerExtensionRoutes()
{
    server_->Get("/api/extensions", [this](const httplib::Request &, httplib::Response &res) {
        sendJson(res, daemon_.extensions());
    });

    server_->Get(R"(/api/extensions/([^/]+)/ui/manifest)",
                 [this](const httplib::Request &req, httplib::Response &res) {
                     const auto manifest = daemon_.extensionUiManifest(req.matches[1]);
                     if (!manifest) {
                         sendError(res, 404, "extension has no editor module");
                         return;
                     }
                     sendJson(res, *manifest);
                 });

    server_->Get(R"(/api/extensions/([^/]+)/ui/assets/([^/]+)/(.*))",
                 [this](const httplib::Request &req, httplib::Response &res) {
                     const auto asset = daemon_.extensionUiAsset(req.matches[1], req.matches[2],
                                                                 req.matches[3]);
                     if (!asset) {
                         sendError(res, 404, "no such extension editor asset");
                         return;
                     }
                     res.set_header("Cache-Control", "public, max-age=31536000, immutable");
                     res.set_header("X-Content-Type-Options", "nosniff");
                     res.set_content(asset->data, asset->mime_type);
                 });

    server_->Post(R"(/api/extensions/([^/]+)/ui/call)",
                  [this](const httplib::Request &req, httplib::Response &res) {
                      if (req.body.size() > 256U * 1024U) {
                          sendError(res, 413, "extension message is larger than 256 KiB");
                          return;
                      }
                      json body;
                      if (!parseBody(req, res, body)) return;
                      if (!body.is_object() || !body.contains("method")
                          || !body["method"].is_string()) {
                          sendError(res, 400, "expected {method, data}");
                          return;
                      }
                      const auto result = daemon_.callExtensionUi(
                          req.matches[1], body["method"].get<std::string>(),
                          body.value("data", json(nullptr)));
                      if (!result.ok) {
                          sendError(res, 502, result.error);
                          return;
                      }
                      sendJson(res, json{{"data", result.data}});
                  });

    server_->Get("/api/extensions/ui/events",
                 [this](const httplib::Request &, httplib::Response &res) {
                     res.set_header("Cache-Control", "no-store");
                     auto cursor = std::make_shared<std::uint64_t>(daemon_.extensionEventCursor());
                     res.set_chunked_content_provider(
                         "text/event-stream",
                         [this, cursor](std::size_t, httplib::DataSink &sink) {
                             const json events = daemon_.extensionEventsAfter(*cursor);
                             std::string frame;
                             for (const json &event : events) {
                                 frame += "event: extension\ndata: " + event.dump() + "\n\n";
                             }
                             if (frame.empty()) frame = ": keepalive\n\n";
                             if (!sink.write(frame.data(), frame.size())) return false;
                             std::this_thread::sleep_for(std::chrono::milliseconds(250));
                             return running_.load(std::memory_order_relaxed);
                         });
                 });

    server_->Post("/api/extensions/rescan", [this](const httplib::Request &,
                                                   httplib::Response &res) {
        daemon_.rescanExtensions();
        sendJson(res, daemon_.extensions());
    });

    server_->Post(R"(/api/extensions/([^/]+)/(enable|disable))",
                  [this](const httplib::Request &req, httplib::Response &res) {
                      std::string error;
                      if (!daemon_.setExtensionEnabled(req.matches[1],
                                                       req.matches[2] == "enable", error)) {
                          sendError(res, 404, error);
                          return;
                      }
                      sendJson(res, daemon_.extensions());
                  });

    server_->Put(R"(/api/extensions/([^/]+)/settings)",
                 [this](const httplib::Request &req, httplib::Response &res) {
                     json body;
                     if (!parseBody(req, res, body)) {
                         return;
                     }
                     std::string error;
                     if (!daemon_.setExtensionSettings(
                             req.matches[1], body.value("values", json::object()), error)) {
                         sendError(res, 400, error);
                         return;
                     }
                     res.status = 204;
                 });

    server_->Delete(R"(/api/extensions/([^/]+))",
                    [this](const httplib::Request &req, httplib::Response &res) {
                        std::string error;
                        if (!daemon_.removeExtension(req.matches[1], error)) {
                            sendError(res, 409, error);
                            return;
                        }
                        sendJson(res, daemon_.extensions());
                    });

    // Read in pieces with a limit of its own, rather than by raising the
    // server's payload ceiling: that ceiling also guards every other route,
    // and a graph has no business being sixty megabytes.
    server_->Post(
        "/api/extensions/install",
        [this](const httplib::Request &req, httplib::Response &res,
               const httplib::ContentReader &content_reader) {
            const std::string name = req.get_param_value("name");
            std::string bytes;
            bool too_big = false;
            content_reader([&](const char *data, std::size_t length) {
                if (bytes.size() + length > kMaxUploadBytes) {
                    too_big = true;
                    return false;
                }
                bytes.append(data, length);
                return true;
            });

            if (too_big) {
                sendError(res, 413, "an extension may be at most "
                                        + std::to_string(kMaxUploadBytes / (1024 * 1024))
                                        + " MiB");
                return;
            }
            std::string error;
            if (!daemon_.installExtension(name, bytes, error)) {
                sendError(res, 400, error);
                return;
            }
            sendJson(res, daemon_.extensions());
        });
}

void ControlPlane::registerRvcRoutes()
{
    server_->Get("/api/rvc/models", [this](const httplib::Request &, httplib::Response &res) {
        sendJson(res, rvc_models_.describe());
    });

    // A checkpoint is normally around 60 MiB and may be substantially larger.
    // Stream it straight to the import staging directory rather than raising
    // the JSON/body limit or holding model weights in daemon memory.
    server_->Post(
        "/api/rvc/models/import",
        [this](const httplib::Request &req, httplib::Response &res,
               const httplib::ContentReader &content_reader) {
            if (!daemon_.uploadAllowed()) {
                sendError(res, 403, "importing models is only allowed on a loopback address");
                return;
            }
            const std::string name = req.get_param_value("name");
            std::string error;
            std::optional<RvcModelManager::Upload> upload;
            bool accepted = true;
            if (req.is_multipart_form_data()) {
                enum class Part { None, Checkpoint, Index } part = Part::None;
                accepted = content_reader(
                    [&](const httplib::FormData &file) {
                        part = Part::None;
                        if (file.name == "checkpoint" && !upload) {
                            upload = rvc_models_.beginUpload(file.filename, name, error);
                            if (!upload) return false;
                            part = Part::Checkpoint;
                            return true;
                        }
                        if (file.name == "index" && upload && !upload->index_stream) {
                            if (!rvc_models_.beginIndex(*upload, file.filename, error)) return false;
                            part = Part::Index;
                            return true;
                        }
                        error = "multipart import expects one checkpoint followed by at most one index";
                        return false;
                    },
                    [&](const char *data, std::size_t length) {
                        const bool okay = part == Part::Checkpoint
                                              ? rvc_models_.append(*upload, data, length, error)
                                              : part == Part::Index
                                                    ? rvc_models_.appendIndex(*upload, data, length,
                                                                              error)
                                                    : false;
                        if (!okay && error.empty()) error = "unexpected multipart model data";
                        return okay;
                    });
            } else {
                upload = rvc_models_.beginUpload(req.get_param_value("filename"), name, error);
                if (upload) {
                    accepted = content_reader([&](const char *data, std::size_t length) {
                        return rvc_models_.append(*upload, data, length, error);
                    });
                }
            }
            if (!upload) {
                sendError(res, 409, error.empty() ? "checkpoint part is missing" : error);
                return;
            }
            if (!accepted) {
                rvc_models_.abort(std::move(*upload));
                const bool too_big = error.find("512 MiB") != std::string::npos
                                     || error.find("2 GiB") != std::string::npos;
                sendError(res, too_big ? 413 : 400, error);
                return;
            }
            if (!rvc_models_.commit(std::move(*upload), error)) {
                rvc_models_.abort(std::move(*upload));
                sendError(res, error.find("512 MiB") != std::string::npos ? 413 : 400, error);
                return;
            }
            sendJson(res, rvc_models_.describe(), 202);
        });

    server_->Post(R"(/api/rvc/models/import/([^/]+)/cancel)",
                  [this](const httplib::Request &req, httplib::Response &res) {
                      std::string error;
                      if (!rvc_models_.cancel(req.matches[1], error)) {
                          sendError(res, 409, error);
                          return;
                      }
                      sendJson(res, rvc_models_.describe());
                  });

    server_->Delete(R"(/api/rvc/models/([^/]+))",
                    [this](const httplib::Request &req, httplib::Response &res) {
                        std::string error;
                        if (!rvc_models_.removeModel(req.matches[1], error)) {
                            sendError(res, 404, error);
                            return;
                        }
                        sendJson(res, rvc_models_.describe());
                    });
}

void ControlPlane::registerUiRoutes()
{
    if (!config_.ui_dir.empty()) {
        if (!server_->set_mount_point("/", config_.ui_dir)) {
            spdlog::warn("--ui-dir '{}' is not a directory", config_.ui_dir);
        }
        return;
    }

    server_->Get("/(.*)", [](const httplib::Request &req, httplib::Response &res) {
        std::string path = req.matches[1];
        if (path.empty() || path.back() == '/') {
            path += "index.html";
        }

        const ui::Asset *asset = ui::find(path);
        if (asset == nullptr) {
            asset = ui::find("index.html");
        }
        if (asset == nullptr) {
            res.status = 404;
            res.set_content("The editor was not built into this binary. Build ui/ and rebuild, "
                            "or pass --ui-dir=ui/dist.\n",
                            "text/plain");
            return;
        }
        res.set_content(std::string(asset->data), mimeFor(asset->path));
    });
}

}
