#pragma once

#include "types/Audio.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace avc::graph {

using PortRef = std::variant<std::string, std::uint32_t>;

struct SpecEndpoint {
    std::string node;
    PortRef port = std::uint32_t{0};
};

struct SpecEdge {
    SpecEndpoint from;
    SpecEndpoint to;
};

struct SpecNode {
    std::string id;
    std::string type;

    std::string domain;

    std::map<std::string, float> params;

    std::map<std::string, std::string> options;

    std::uint32_t inputs = 0;
    std::uint32_t outputs = 0;

    float ui_x = 0.0F;
    float ui_y = 0.0F;
};

struct SpecDomain {
    std::uint32_t block = 0;

    std::uint32_t safety = types::kDefaultColdSafety;
};

struct GraphSpec {
    std::uint64_t version = 0;
    std::vector<SpecNode> nodes;
    std::vector<SpecEdge> edges;

    std::map<std::string, SpecDomain> domains;

    static std::optional<GraphSpec> parse(const std::string &text, std::string &error);
    static std::optional<GraphSpec> load(const std::string &path, std::string &error);

    std::string dump(int indent = 2) const;
};

}
