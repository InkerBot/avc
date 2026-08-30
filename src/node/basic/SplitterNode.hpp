#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"

namespace avc::node::basic {

class SplitterNode final : public Node {
public:
    static NodeDescriptor descriptor();

    void prepare(const PrepareInfo &info) override;
    void setParam(std::uint32_t index, float value) noexcept override;

    // ZFW: HOT PATH
    void process(const NodeContext &ctx) noexcept override;
};

}
