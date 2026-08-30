#include "node/debug/TextNode.hpp"

#include <avc/text_frame.hpp>

#include <cstring>
#include <string_view>
#include <utility>

namespace avc::node::debug {
namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

bool continuation(unsigned char byte) noexcept
{
    return (byte & 0xC0U) == 0x80U;
}

std::string validUtf8(std::string_view input)
{
    std::string out;
    out.reserve(input.size());

    for (std::size_t i = 0; i < input.size();) {
        const auto first = static_cast<unsigned char>(input[i]);
        std::size_t count = 0;
        bool valid = false;

        if (first <= 0x7FU) {
            count = 1;
            valid = true;
        } else if (first >= 0xC2U && first <= 0xDFU && i + 1 < input.size()) {
            count = 2;
            valid = continuation(static_cast<unsigned char>(input[i + 1]));
        } else if (first >= 0xE0U && first <= 0xEFU && i + 2 < input.size()) {
            const auto second = static_cast<unsigned char>(input[i + 1]);
            const auto third = static_cast<unsigned char>(input[i + 2]);
            count = 3;
            valid = continuation(second) && continuation(third)
                    && (first != 0xE0U || second >= 0xA0U)
                    && (first != 0xEDU || second <= 0x9FU);
        } else if (first >= 0xF0U && first <= 0xF4U && i + 3 < input.size()) {
            const auto second = static_cast<unsigned char>(input[i + 1]);
            count = 4;
            valid = continuation(second)
                    && continuation(static_cast<unsigned char>(input[i + 2]))
                    && continuation(static_cast<unsigned char>(input[i + 3]))
                    && (first != 0xF0U || second >= 0x90U)
                    && (first != 0xF4U || second <= 0x8FU);
        }

        if (!valid) {
            out.append(kReplacement);
            ++i;
            continue;
        }
        out.append(input.data() + i, count);
        i += count;
    }
    return out;
}

}

NodeDescriptor TextNode::descriptor()
{
    NodeDescriptor d;
    d.type = "text";
    d.category = "debug";
    d.label = "Text";
    d.inputs = {{"in", kTextPortType}};
    return d;
}

void TextNode::prepare(const PrepareInfo &)
{
    static_assert(kTextPortTypeBytes == text::kBlockBytes);
    latest_.reset(kTextPortTypeBytes);
    building_.fill(std::byte{0});
    length_ = 0;
    stream_ = 0;
    segment_ = 0;
    revision_ = 0;
    segmented_ = false;
    final_ = false;
    seen_ = false;
}

// ZFW: HOT PATH
void TextNode::setParam(std::uint32_t, float) noexcept
{
}

// ZFW: HOT PATH
void TextNode::process(const NodeContext &ctx) noexcept
{
    const void *block = ctx.n_inputs > 0 && ctx.in_blocks != nullptr
                            ? ctx.in_blocks[0]
                            : nullptr;
    text::Frame frame = text::readFrame(block);
    if (!frame.valid) {
        frame = {};
        frame.valid = true;
    }
    const auto length = static_cast<std::uint32_t>(frame.value.size());

    const std::byte *previous = building_.data() + sizeof(length);
    if (seen_ && length == length_
        && (length == 0 || std::memcmp(previous, frame.value.data(), length) == 0)
        && stream_ == frame.stream && segment_ == frame.segment
        && revision_ == frame.revision && segmented_ == frame.segmented
        && final_ == frame.final) {
        return;
    }

    if (frame.segmented) {
        text::writeFrame(building_.data(), frame.value, frame.stream, frame.segment,
                         frame.revision, frame.final);
    } else {
        text::writePlain(building_.data(), frame.value);
    }
    length_ = length;
    stream_ = frame.stream;
    segment_ = frame.segment;
    revision_ = frame.revision;
    segmented_ = frame.segmented;
    final_ = frame.final;
    seen_ = true;
    latest_.write(building_.data());
}

bool TextNode::takeText(TextSnapshot &out)
{
    std::array<std::byte, kTextPortTypeBytes> snapshot{};
    if (!latest_.read(snapshot.data())) {
        return false;
    }

    const text::Frame frame = text::readFrame(snapshot.data());
    out.text = frame.valid ? validUtf8(frame.value) : std::string{};
    out.stream = frame.stream;
    out.segment = frame.segment;
    out.revision = frame.revision;
    out.segmented = frame.segmented;
    out.final = frame.final;
    return true;
}

bool TextNode::takeText(std::string &out)
{
    TextSnapshot snapshot;
    if (!takeText(snapshot)) return false;
    out = std::move(snapshot.text);
    return true;
}

}
