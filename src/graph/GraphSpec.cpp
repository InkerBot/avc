#include "graph/GraphSpec.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <cmath>
#include <limits>
#include <sstream>

namespace avc::graph {
namespace {

using nlohmann::json;

bool readString(const json &object, const char *key, std::string &out, std::string &error,
                std::string_view path, bool required = false)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        if (required) error = std::string(path) + "." + key + " is required";
        return !required;
    }
    if (!it->is_string()) {
        error = std::string(path) + "." + key + " must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool readU32(const json &object, const char *key, std::uint32_t &out, std::string &error,
             std::string_view path)
{
    const auto it = object.find(key);
    if (it == object.end()) return true;
    if (!it->is_number_unsigned() || *it > std::numeric_limits<std::uint32_t>::max()) {
        error = std::string(path) + "." + key + " must be an unsigned 32-bit integer";
        return false;
    }
    out = it->get<std::uint32_t>();
    return true;
}

bool parsePort(const json &value, PortRef &out, std::string &error, std::string_view path)
{
    if (value.is_string()) {
        out = value.get<std::string>();
        return true;
    }
    if (value.is_number_unsigned() && value <= std::numeric_limits<std::uint32_t>::max()) {
        out = value.get<std::uint32_t>();
        return true;
    }
    error = std::string(path) + " must be a port name or unsigned port index";
    return false;
}

json dumpPort(const PortRef &port)
{
    if (std::holds_alternative<std::string>(port)) {
        return std::get<std::string>(port);
    }
    return std::get<std::uint32_t>(port);
}

bool parseEndpoint(const json &value, SpecEndpoint &endpoint, std::string &error,
                   std::string_view path)
{
    if (!value.is_object()) {
        error = std::string(path) + " must be an object";
        return false;
    }
    if (!readString(value, "node", endpoint.node, error, path, true)) return false;
    if (endpoint.node.empty()) {
        error = std::string(path) + ".node must not be empty";
        return false;
    }
    const auto port = value.find("port");
    if (port == value.end()) {
        error = std::string(path) + ".port is required";
        return false;
    }
    return parsePort(*port, endpoint.port, error, std::string(path) + ".port");
}

bool parseNode(const json &value, GraphSpec &spec, std::string &error, std::size_t index)
{
    const std::string path = "nodes[" + std::to_string(index) + "]";
    if (!value.is_object()) {
        error = path + " must be an object";
        return false;
    }
    SpecNode node;
    if (!readString(value, "id", node.id, error, path, true)
        || !readString(value, "type", node.type, error, path, true)
        || !readString(value, "domain", node.domain, error, path)
        || !readU32(value, "inputs", node.inputs, error, path)
        || !readU32(value, "outputs", node.outputs, error, path)) return false;
    if (node.id.empty() || node.type.empty()) {
        error = path + ".id and .type must not be empty";
        return false;
    }

    if (value.contains("params") && !value["params"].is_object()) {
        error = path + ".params must be an object";
        return false;
    }
    if (value.contains("params")) {
        for (const auto &[key, param] : value["params"].items()) {
            if (param.is_number()) {
                const float number = param.get<float>();
                if (!std::isfinite(number)) {
                    error = path + ".params." + key + " must be finite";
                    return false;
                }
                node.params[key] = number;
            } else if (param.is_boolean()) {
                node.params[key] = param.get<bool>() ? 1.0F : 0.0F;
            } else if (param.is_string()) {
                node.options[key] = param.get<std::string>();
            } else {
                error = path + ".params." + key + " must be a number, boolean, or string";
                return false;
            }
        }
    }
    if (value.contains("ui")) {
        if (!value["ui"].is_object()) {
            error = path + ".ui must be an object";
            return false;
        }
        for (const auto &[key, target] : {std::pair{"x", &node.ui_x},
                                          std::pair{"y", &node.ui_y}}) {
            const auto it = value["ui"].find(key);
            if (it != value["ui"].end()) {
                if (!it->is_number()) {
                    error = path + ".ui." + key + " must be a number";
                    return false;
                }
                *target = it->get<float>();
                if (!std::isfinite(*target)) {
                    error = path + ".ui." + key + " must be finite";
                    return false;
                }
            }
        }
    }
    spec.nodes.push_back(std::move(node));
    return true;
}

}

std::optional<GraphSpec> GraphSpec::parse(const std::string &text, std::string &error)
{
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "graph is not a JSON object";
        return std::nullopt;
    }

    GraphSpec spec;
    if (const auto version = root.find("version"); version != root.end()) {
        if (!version->is_number_unsigned()) {
            error = "version must be an unsigned integer";
            return std::nullopt;
        }
        spec.version = version->get<std::uint64_t>();
    }

    if (!root.contains("nodes") || !root["nodes"].is_array()) {
        error = "graph has no 'nodes' array";
        return std::nullopt;
    }
    for (std::size_t i = 0; i < root["nodes"].size(); ++i) {
        if (!parseNode(root["nodes"][i], spec, error, i)) return std::nullopt;
    }

    if (root.contains("domains") && root["domains"].is_object()) {
        for (const auto &[name, entry] : root["domains"].items()) {
            if (!entry.is_object()) {
                error = "domains." + name + " must be an object";
                return std::nullopt;
            }
            SpecDomain domain;
            domain.safety = types::kDefaultColdSafety;
            if (!readU32(entry, "block", domain.block, error, "domains." + name)
                || !readU32(entry, "safety", domain.safety, error, "domains." + name)) {
                return std::nullopt;
            }
            spec.domains[name] = domain;
        }
    }

    if (root.contains("edges") && root["edges"].is_array()) {
        for (std::size_t i = 0; i < root["edges"].size(); ++i) {
            const json &edge = root["edges"][i];
            const std::string path = "edges[" + std::to_string(i) + "]";
            if (!edge.is_object()) {
                error = path + " must be an object";
                return std::nullopt;
            }
            if (!edge.contains("from") || !edge.contains("to")) {
                error = path + " is missing 'from' or 'to'";
                return std::nullopt;
            }
            SpecEdge decoded;
            if (!parseEndpoint(edge["from"], decoded.from, error, path + ".from")
                || !parseEndpoint(edge["to"], decoded.to, error, path + ".to")) {
                return std::nullopt;
            }
            spec.edges.push_back(std::move(decoded));
        }
    } else if (root.contains("edges")) {
        error = "edges must be an array";
        return std::nullopt;
    }
    return spec;
}

std::optional<GraphSpec> GraphSpec::load(const std::string &path, std::string &error)
{
    std::ifstream file(path);
    if (!file) {
        error = "cannot open " + path;
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str(), error);
}

std::string GraphSpec::dump(int indent) const
{
    json root;
    root["version"] = version;
    root["nodes"] = json::array();
    root["edges"] = json::array();

    for (const SpecNode &node : nodes) {
        json entry;
        entry["id"] = node.id;
        entry["type"] = node.type;
        if (!node.domain.empty() && node.domain != types::kHotDomain) {
            entry["domain"] = node.domain;
        }
        if (node.inputs > 0) {
            entry["inputs"] = node.inputs;
        }
        if (node.outputs > 0) {
            entry["outputs"] = node.outputs;
        }
        entry["params"] = json::object();
        for (const auto &[key, value] : node.params) {
            entry["params"][key] = value;
        }
        for (const auto &[key, value] : node.options) {
            entry["params"][key] = value;
        }
        entry["ui"] = {{"x", node.ui_x}, {"y", node.ui_y}};
        root["nodes"].push_back(std::move(entry));
    }

    if (!domains.empty()) {
        root["domains"] = json::object();
        for (const auto &[name, domain] : domains) {
            json entry;
            if (domain.block > 0) {
                entry["block"] = domain.block;
            }
            entry["safety"] = domain.safety;
            root["domains"][name] = std::move(entry);
        }
    }

    for (const SpecEdge &edge : edges) {
        root["edges"].push_back({
            {"from", {{"node", edge.from.node}, {"port", dumpPort(edge.from.port)}}},
            {"to", {{"node", edge.to.node}, {"port", dumpPort(edge.to.port)}}},
        });
    }
    return root.dump(indent);
}

}
