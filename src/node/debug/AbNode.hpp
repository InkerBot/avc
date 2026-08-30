#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"
#include "rt/SmoothedParam.hpp"

namespace avc::node::debug {

class AbNode final : public Node {
public:
    static NodeDescriptor descriptor();

    void prepare(const PrepareInfo &info) override;
    void inherit(const Node &previous) override;

    // ZFW: HOT PATH
    void setParam(std::uint32_t index, float value) noexcept override;

    // ZFW: HOT PATH
    void process(const NodeContext &ctx) noexcept override;

private:
    enum Param : std::uint32_t {
        kSelect = 0,
    };

    rt::SmoothedParam select_;
};

}
