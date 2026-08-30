#pragma once

#include "graph/GraphSpec.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace avc::control {

class PresetStore {
public:
    static std::filesystem::path defaultDirectory();

    explicit PresetStore(std::filesystem::path directory);

    std::vector<std::string> list() const;
    bool save(const std::string &name, const graph::GraphSpec &spec, std::string &error);
    std::optional<graph::GraphSpec> load(const std::string &name, std::string &error) const;
    bool remove(const std::string &name, std::string &error);

    static bool validName(const std::string &name);

private:
    std::filesystem::path pathOf(const std::string &name) const;

    std::filesystem::path directory_;
};

}
