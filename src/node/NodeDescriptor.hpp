#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace avc::node {

enum class NodeKind : std::uint8_t {
    Dsp,
    Capture,
    Playback,
    VirtualSpeaker,
    VirtualMic,
};

constexpr bool producesSignal(NodeKind kind) noexcept
{
    return kind == NodeKind::Capture || kind == NodeKind::VirtualSpeaker;
}

constexpr bool isIo(NodeKind kind) noexcept
{
    return kind != NodeKind::Dsp;
}

enum class ParamType : std::uint8_t {
    Float,
    Enum,
    Bool,
    Device,
    Text,
    Path,
};

enum class ParamCurve : std::uint8_t {
    Linear,
    Logarithmic,
};

struct ParamDescriptor {
    std::string name;
    ParamType type = ParamType::Float;
    float min = 0.0F;
    float max = 1.0F;
    float default_value = 0.0F;
    std::string unit;
    ParamCurve curve = ParamCurve::Linear;
    std::vector<std::string> values;

    std::string description;

    std::string device_role;

    std::string default_text;
};

struct PortDescriptor {
    std::string name;

    std::string type;
};

struct NodeDescriptor {
    std::string type;
    std::string category;
    std::string label;
    NodeKind kind = NodeKind::Dsp;
    std::vector<PortDescriptor> inputs;
    std::vector<PortDescriptor> outputs;
    std::vector<ParamDescriptor> params;

    bool dynamic_inputs = false;
    bool dynamic_outputs = false;

    std::uint32_t latency_frames = 0;

    std::string extension;

    bool realtime_safe = true;

    std::uint32_t recommended_cold_block = 0;

    int indexOfParam(std::string_view name) const noexcept;
};

}
