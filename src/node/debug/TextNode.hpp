#pragma once

#include "node/Node.hpp"
#include "node/NodeDescriptor.hpp"
#include "node/PortTypeManifest.hpp"
#include "rt/ValueSlot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace avc::node::debug {

struct TextSnapshot {
    std::string text;
    std::uint64_t stream = 0;
    std::uint64_t segment = 0;
    std::uint64_t revision = 0;
    bool segmented = false;
    bool final = false;
};

class TextNode final : public Node {
public:
    static NodeDescriptor descriptor();

    void prepare(const PrepareInfo &info) override;

    // ZFW: HOT PATH
    void setParam(std::uint32_t index, float value) noexcept override;

    // ZFW: HOT PATH
    void process(const NodeContext &ctx) noexcept override;

    // Returns only when a newer snapshot has arrived. If several streaming
    // updates arrived between editor frames, `out` receives the newest one.
    bool takeText(TextSnapshot &out);
    bool takeText(std::string &out);

private:
    rt::ValueSlot latest_;
    std::array<std::byte, kTextPortTypeBytes> building_{};

    std::uint32_t length_ = 0;
    std::uint64_t stream_ = 0;
    std::uint64_t segment_ = 0;
    std::uint64_t revision_ = 0;
    bool segmented_ = false;
    bool final_ = false;
    bool seen_ = false;
};

}
