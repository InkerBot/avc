#pragma once

#include "types/Audio.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace avc::node {

struct NodeContext {
    const types::Sample *const *inputs = nullptr;
    types::Sample *const *outputs = nullptr;
    std::uint32_t n_inputs = 0;
    std::uint32_t n_outputs = 0;
    std::uint32_t nframes = 0;

    const void *const *in_blocks = nullptr;
    void *const *out_blocks = nullptr;

    std::uint64_t start_frame = 0;
    bool discontinuity = false;
};

struct PrepareInfo {
    std::uint32_t sample_rate = types::kDefaultSampleRate;
    std::uint32_t max_quantum = types::kDefaultQuantum;
    std::uint32_t n_inputs = 0;
    std::uint32_t n_outputs = 0;
};

enum class NodeState : std::uint8_t { Offline, Loading, Ready, Degraded, Error };

struct NodeStatus {
    NodeState state = NodeState::Offline;
    float progress = 0.0F;
    std::uint64_t processed_blocks = 0;
    std::uint64_t bypassed_blocks = 0;
    std::uint64_t failures = 0;
    std::string message;
};

class Node {
public:
    Node() = default;
    virtual ~Node() = default;

    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    virtual void prepare(const PrepareInfo &info) = 0;

    virtual bool prepareChecked(const PrepareInfo &info, std::string &error)
    {
        (void)error;
        prepare(info);
        return true;
    }

    virtual void inherit(const Node &previous) { (void)previous; }

    virtual std::uint32_t latencyFrames() const noexcept { return 0; }

    // ZFW: HOT PATH
    virtual void setParam(std::uint32_t index, float value) noexcept = 0;

    virtual void setOption(std::uint32_t index, std::string_view value)
    {
        (void)index;
        (void)value;
    }

    virtual bool status(NodeStatus &out) const noexcept
    {
        (void)out;
        return false;
    }

    // ZFW: HOT PATH
    virtual void process(const NodeContext &ctx) noexcept = 0;
};

}
