#pragma once

#include "graph/GraphSpec.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace avc::control {

class StateStore {
public:
    static std::filesystem::path defaultDirectory();

    explicit StateStore(std::filesystem::path directory);

    void save(const graph::GraphSpec &spec) const;

    std::optional<graph::GraphSpec> load(std::string &error) const;
    bool exists() const;

    std::filesystem::path crashPath() const;

    const std::filesystem::path &directory() const noexcept { return directory_; }

private:
    std::filesystem::path graphPath() const;

    std::filesystem::path directory_;
};

}
