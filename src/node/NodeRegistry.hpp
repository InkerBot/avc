#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"

#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace avc::node {

class NodeRegistry {
public:
    using Factory = std::function<std::unique_ptr<Node>()>;

    static NodeRegistry &instance();

    bool add(NodeDescriptor descriptor, Factory factory);

    void seal() noexcept;
    bool sealed() const noexcept { return sealed_; }

    const NodeDescriptor *find(std::string_view type) const noexcept;
    std::unique_ptr<Node> create(std::string_view type) const;
    std::vector<const NodeDescriptor *> all() const;

private:
    NodeRegistry();

    void registerBuiltins();

    struct Entry {
        NodeDescriptor descriptor;
        Factory factory;
    };

    std::unordered_map<std::string, Entry> entries_;
    bool sealed_ = false;
};

}
