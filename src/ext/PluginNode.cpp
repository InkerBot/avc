#include "ext/PluginNode.hpp"

#include <algorithm>

namespace avc::ext {

std::unique_ptr<PluginNode> PluginNode::create(const AvcNodeVtable &vtable)
{
    if (vtable.create == nullptr || vtable.destroy == nullptr || vtable.process == nullptr
        || vtable.set_param == nullptr) {
        return nullptr;
    }
    void *self = vtable.create();
    if (self == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<PluginNode>(new PluginNode(vtable, self));
}

PluginNode::~PluginNode()
{
    if (self_ != nullptr) {
        vtable_->destroy(self_);
    }
}

void PluginNode::prepare(const node::PrepareInfo &info)
{
    std::string ignored;
    (void)prepareChecked(info, ignored);
}

bool PluginNode::prepareChecked(const node::PrepareInfo &info, std::string &error)
{
    if (vtable_->prepare == nullptr) {
        return true;
    }
    const AvcPrepareInfo out{info.sample_rate, info.max_quantum, info.n_inputs, info.n_outputs};
    char message[512]{};
    if (vtable_->prepare(self_, &out, message, sizeof(message)) != AVC_RESULT_OK) {
        error = message[0] != '\0' ? message : "extension node prepare failed";
        return false;
    }
    return true;
}

void PluginNode::inherit(const node::Node &previous)
{
    if (vtable_->inherit == nullptr) {
        return;
    }
    const auto *other = dynamic_cast<const PluginNode *>(&previous);
    if (other == nullptr || other->vtable_ != vtable_ || other->self_ == nullptr) {
        return;
    }
    vtable_->inherit(self_, other->self_);
}

std::uint32_t PluginNode::latencyFrames() const noexcept
{
    return vtable_->latency_frames != nullptr ? vtable_->latency_frames(self_) : 0;
}

// ZFW: HOT PATH
void PluginNode::setParam(std::uint32_t index, float value) noexcept
{
    vtable_->set_param(self_, index, value);
}

void PluginNode::setOption(std::uint32_t index, std::string_view value)
{
    if (vtable_->set_option != nullptr) {
        const std::string text(value);
        vtable_->set_option(self_, index, text.c_str());
    }
}

bool PluginNode::status(node::NodeStatus &out) const noexcept
{
    if (vtable_->get_status == nullptr) return false;
    AvcNodeStatus status{};
    status.struct_size = sizeof(status);
    if (vtable_->get_status(self_, &status) != AVC_RESULT_OK) return false;
    switch (status.state) {
    case AVC_NODE_LOADING:  out.state = node::NodeState::Loading; break;
    case AVC_NODE_READY:    out.state = node::NodeState::Ready; break;
    case AVC_NODE_DEGRADED: out.state = node::NodeState::Degraded; break;
    case AVC_NODE_ERROR:    out.state = node::NodeState::Error; break;
    case AVC_NODE_OFFLINE:
    default:                out.state = node::NodeState::Offline; break;
    }
    out.progress = status.progress;
    out.processed_blocks = status.processed_blocks;
    out.bypassed_blocks = status.bypassed_blocks;
    out.failures = status.failures;
    const char *end = std::find(status.message, status.message + sizeof(status.message), '\0');
    out.message.assign(status.message, static_cast<std::size_t>(end - status.message));
    return true;
}

// ZFW: HOT PATH
void PluginNode::process(const node::NodeContext &ctx) noexcept
{
    const AvcProcessCtx out{ctx.inputs,      ctx.outputs,      ctx.n_inputs,
                           ctx.n_outputs,   ctx.nframes,       ctx.in_blocks,
                           ctx.out_blocks,  ctx.start_frame,   ctx.discontinuity ? 1U : 0U};
    vtable_->process(self_, &out);
}

}
