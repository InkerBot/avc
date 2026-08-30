#pragma once

#include "node/Node.hpp"

#include <avc/avc_plugin.h>

#include <memory>

namespace avc::ext {

class PluginNode final : public node::Node {
public:
    static std::unique_ptr<PluginNode> create(const AvcNodeVtable &vtable);

    ~PluginNode() override;

    void prepare(const node::PrepareInfo &info) override;
    bool prepareChecked(const node::PrepareInfo &info, std::string &error) override;
    void inherit(const node::Node &previous) override;
    std::uint32_t latencyFrames() const noexcept override;

    // ZFW: HOT PATH
    void setParam(std::uint32_t index, float value) noexcept override;
    void setOption(std::uint32_t index, std::string_view value) override;
    bool status(node::NodeStatus &out) const noexcept override;

    // ZFW: HOT PATH
    void process(const node::NodeContext &ctx) noexcept override;

private:
    PluginNode(const AvcNodeVtable &vtable, void *self) noexcept
        : vtable_(&vtable), self_(self)
    {
    }

    const AvcNodeVtable *vtable_ = nullptr;
    void *self_ = nullptr;
};

}
