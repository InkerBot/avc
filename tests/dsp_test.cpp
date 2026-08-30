#include "dsp/Biquad.hpp"
#include "node/NodeRegistry.hpp"
#include "node/debug/AbNode.hpp"
#include "node/debug/ScopeNode.hpp"
#include "node/debug/SignalNode.hpp"
#include "node/debug/TextNode.hpp"
#include "node/dyn/CompressorNode.hpp"
#include "node/dyn/GateNode.hpp"
#include "node/eq/BiquadNode.hpp"
#include "node/fx/RingModNode.hpp"
#include "node/voice/FormantNode.hpp"
#include "node/voice/PitchNode.hpp"

#include <avc/text_frame.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using avc::node::Node;
using avc::node::NodeContext;
using avc::node::NodeDescriptor;
using avc::node::PrepareInfo;
using avc::types::Sample;

constexpr std::uint32_t kRate = 48000;
constexpr float kPi = 3.14159265358979323846F;

class Rig {
public:
    explicit Rig(std::unique_ptr<Node> node, NodeDescriptor descriptor, std::uint32_t quantum = 128)
        : node_(std::move(node)), desc_(std::move(descriptor)), quantum_(quantum),
          in_(quantum, 0.0F), out_(quantum, 0.0F)
    {
        for (std::size_t i = 0; i < desc_.params.size(); ++i) {
            node_->setParam(static_cast<std::uint32_t>(i), desc_.params[i].default_value);
        }
    }

    Rig &set(const char *name, float value)
    {
        const int index = desc_.indexOfParam(name);
        EXPECT_GE(index, 0) << "no parameter '" << name << "' on " << desc_.type;
        if (index >= 0) {
            node_->setParam(static_cast<std::uint32_t>(index), value);
        }
        return *this;
    }

    Rig &prepare()
    {
        node_->prepare({kRate, quantum_, 1, 1});
        return *this;
    }

    void run()
    {
        const Sample *in_ptr = in_.data();
        Sample *out_ptr = out_.data();
        const NodeContext ctx{&in_ptr, &out_ptr, 1, 1, quantum_};
        node_->process(ctx);
    }

    std::vector<float> stream(const std::vector<float> &signal)
    {
        std::vector<float> result;
        result.reserve(signal.size());
        for (std::size_t at = 0; at + quantum_ <= signal.size(); at += quantum_) {
            std::copy(signal.begin() + static_cast<std::ptrdiff_t>(at),
                      signal.begin() + static_cast<std::ptrdiff_t>(at + quantum_), in_.begin());
            run();
            result.insert(result.end(), out_.begin(), out_.end());
        }
        return result;
    }

    std::vector<Sample> &in() { return in_; }
    const std::vector<Sample> &out() const { return out_; }
    Node &node() { return *node_; }

private:
    std::unique_ptr<Node> node_;
    NodeDescriptor desc_;
    std::uint32_t quantum_;
    std::vector<Sample> in_;
    std::vector<Sample> out_;
};

template <typename T>
Rig rigFor()
{
    return Rig(std::make_unique<T>(), T::descriptor());
}

std::vector<float> sine(float hz, std::size_t frames, float amplitude = 0.5F)
{
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = amplitude * std::sin(2.0F * kPi * hz * static_cast<float>(i) / kRate);
    }
    return out;
}

float crossingRate(const std::vector<float> &signal)
{
    const std::size_t from = signal.size() / 2;
    std::size_t crossings = 0;
    for (std::size_t i = from + 1; i < signal.size(); ++i) {
        if ((signal[i - 1] < 0.0F) != (signal[i] < 0.0F)) {
            ++crossings;
        }
    }
    const float seconds = static_cast<float>(signal.size() - from - 1) / kRate;
    return static_cast<float>(crossings) / (2.0F * seconds);
}

float peak(const std::vector<float> &signal, std::size_t from = 0)
{
    float worst = 0.0F;
    for (std::size_t i = from; i < signal.size(); ++i) {
        worst = std::fmax(worst, std::fabs(signal[i]));
    }
    return worst;
}

TEST(Biquad, LowPassPassesDcAndStopsNyquist)
{
    avc::dsp::Biquad filter;
    filter.setCoeffs(avc::dsp::design(avc::dsp::FilterType::LowPass, 1000.0F, 0.707F, 0.0F, kRate));

    float dc = 0.0F;
    for (int i = 0; i < 2000; ++i) {
        dc = filter.tick(1.0F);
    }
    EXPECT_NEAR(dc, 1.0F, 1e-3F);

    filter.reset();
    float alternating = 0.0F;
    for (int i = 0; i < 2000; ++i) {
        alternating = filter.tick(i % 2 == 0 ? 1.0F : -1.0F);
    }
    EXPECT_LT(std::fabs(alternating), 1e-3F);
}

TEST(Biquad, PeakingAtZeroGainIsTransparent)
{
    avc::dsp::Biquad filter;
    filter.setCoeffs(avc::dsp::design(avc::dsp::FilterType::Peaking, 1000.0F, 2.0F, 0.0F, kRate));

    const std::vector<float> input = sine(700.0F, 512);
    for (float x : input) {
        EXPECT_NEAR(filter.tick(x), x, 1e-5F);
    }
}

TEST(Biquad, DesignSurvivesFrequenciesOutsideTheBand)
{
    for (float hz : {0.0F, 1.0F, 24000.0F, 96000.0F}) {
        avc::dsp::Biquad filter;
        filter.setCoeffs(avc::dsp::design(avc::dsp::FilterType::LowPass, hz, 0.0F, 0.0F, kRate));
        for (int i = 0; i < 256; ++i) {
            EXPECT_TRUE(std::isfinite(filter.tick(i % 2 == 0 ? 1.0F : -1.0F)));
        }
    }
}

TEST(PitchNode, UnisonIsTransparent)
{
    Rig rig = rigFor<avc::node::voice::PitchNode>();
    rig.prepare();

    const std::vector<float> input = sine(220.0F, 128 * 8);
    const std::vector<float> output = rig.stream(input);

    ASSERT_EQ(output.size(), input.size());
    for (std::size_t i = 0; i < output.size(); ++i) {
        EXPECT_NEAR(output[i], input[i], 1e-6F) << "at " << i;
    }
}

TEST(PitchNode, LatencyIsHalfAGrainOnlyWhileShifting)
{
    Rig rig = rigFor<avc::node::voice::PitchNode>();
    rig.set("grain_ms", 40.0F).prepare();
    EXPECT_EQ(rig.node().latencyFrames(), 0U);

    rig.set("semitones", 7.0F);
    EXPECT_NEAR(static_cast<float>(rig.node().latencyFrames()), 0.020F * kRate, 2.0F);

    rig.set("grain_ms", 80.0F);
    EXPECT_NEAR(static_cast<float>(rig.node().latencyFrames()), 0.040F * kRate, 2.0F);

    rig.set("semitones", 0.0F);
    EXPECT_EQ(rig.node().latencyFrames(), 0U);
}

TEST(PitchNode, AnOctaveUpDoublesTheFrequency)
{
    Rig rig = rigFor<avc::node::voice::PitchNode>();
    rig.set("semitones", 12.0F).prepare();

    const std::vector<float> output = rig.stream(sine(300.0F, kRate));
    EXPECT_NEAR(crossingRate(output), 600.0F, 30.0F);
}

TEST(PitchNode, AnOctaveDownHalvesTheFrequency)
{
    Rig rig = rigFor<avc::node::voice::PitchNode>();
    rig.set("semitones", -12.0F).prepare();

    const std::vector<float> output = rig.stream(sine(600.0F, kRate));
    EXPECT_NEAR(crossingRate(output), 300.0F, 30.0F);
}

TEST(PitchNode, ShiftingDoesNotClick)
{
    Rig rig = rigFor<avc::node::voice::PitchNode>();
    rig.set("semitones", 5.0F).set("grain_ms", 30.0F).prepare();

    const std::vector<float> output = rig.stream(sine(200.0F, kRate / 2, 0.5F));
    EXPECT_LT(peak(output, output.size() / 4), 0.75F);
}

TEST(FormantNode, NoShiftIsTransparent)
{
    Rig rig = rigFor<avc::node::voice::FormantNode>();
    rig.prepare();

    const std::vector<float> input = sine(440.0F, 128 * 40);
    const std::vector<float> output = rig.stream(input);

    ASSERT_EQ(output.size(), input.size());
    for (std::size_t i = 0; i < output.size(); ++i) {
        EXPECT_NEAR(output[i], input[i], 1e-4F) << "at " << i;
    }
}

TEST(FormantNode, AddsNoLatency)
{
    Rig rig = rigFor<avc::node::voice::FormantNode>();
    rig.prepare();
    EXPECT_EQ(rig.node().latencyFrames(), 0U);
}

TEST(FormantNode, ShiftingLeavesThePitchAlone)
{
    Rig rig = rigFor<avc::node::voice::FormantNode>();
    rig.set("shift", 6.0F).prepare();

    const std::vector<float> output = rig.stream(sine(300.0F, kRate));
    EXPECT_NEAR(crossingRate(output), 300.0F, 5.0F);
    EXPECT_TRUE(std::isfinite(peak(output)));
}

TEST(FormantNode, SilenceStaysSilent)
{
    Rig rig = rigFor<avc::node::voice::FormantNode>();
    rig.set("shift", -9.0F).prepare();

    const std::vector<float> output = rig.stream(std::vector<float>(128 * 20, 0.0F));
    EXPECT_EQ(peak(output), 0.0F);
}

TEST(GateNode, PassesSpeechAndShutsOnRoomNoise)
{
    Rig rig = rigFor<avc::node::dyn::GateNode>();
    rig.set("threshold_db", -30.0F).set("hold_ms", 0.0F).set("release_ms", 5.0F).prepare();

    const std::vector<float> loud = rig.stream(sine(200.0F, 128 * 40, 0.5F));
    EXPECT_GT(peak(loud, loud.size() / 2), 0.45F);

    const std::vector<float> quiet = rig.stream(sine(200.0F, 128 * 200, 0.001F));
    EXPECT_LT(peak(quiet, quiet.size() * 3 / 4), 1e-4F);
}

TEST(GateNode, HoldKeepsItOpenThroughAGap)
{
    Rig rig = rigFor<avc::node::dyn::GateNode>();
    rig.set("threshold_db", -20.0F).set("hold_ms", 200.0F).prepare();

    rig.stream(sine(200.0F, 128 * 40, 0.5F));

    std::vector<float> gap(128 * 8, 0.0F);
    for (std::size_t i = 0; i < gap.size(); ++i) {
        gap[i] = 0.3F * std::sin(2.0F * kPi * 200.0F * static_cast<float>(i) / kRate);
        if (i > 128) {
            gap[i] *= 1e-4F;
        }
    }
    const std::vector<float> output = rig.stream(gap);
    EXPECT_GT(peak(output, 128) / 1e-4F, 0.2F);
}

TEST(CompressorNode, RatioOfOneIsTransparent)
{
    Rig rig = rigFor<avc::node::dyn::CompressorNode>();
    rig.set("ratio", 1.0F).set("threshold_db", -40.0F).prepare();

    const std::vector<float> input = sine(440.0F, 128 * 20);
    const std::vector<float> output = rig.stream(input);
    for (std::size_t i = 0; i < output.size(); ++i) {
        EXPECT_NEAR(output[i], input[i], 1e-5F) << "at " << i;
    }
}

TEST(CompressorNode, PullsALoudSignalTowardsTheThreshold)
{
    Rig rig = rigFor<avc::node::dyn::CompressorNode>();
    rig.set("threshold_db", -20.0F)
        .set("ratio", 4.0F)
        .set("knee_db", 0.0F)
        .set("attack_ms", 1.0F)
        .set("release_ms", 50.0F)
        .prepare();

    const std::vector<float> output = rig.stream(sine(440.0F, kRate / 2, 1.0F));
    const float out_db = 20.0F * std::log10(peak(output, output.size() / 2));
    EXPECT_NEAR(out_db, -15.0F, 1.5F);
    EXPECT_LT(rig.node().latencyFrames(), 1U);
}

TEST(CompressorNode, ReportsWhatItIsDoing)
{
    auto node = std::make_unique<avc::node::dyn::CompressorNode>();
    auto *compressor = node.get();
    Rig rig(std::move(node), avc::node::dyn::CompressorNode::descriptor());
    rig.set("threshold_db", -30.0F).set("ratio", 8.0F).prepare();

    rig.stream(sine(440.0F, kRate / 4, 1.0F));
    EXPECT_LT(compressor->reductionDb(), -10.0F);
}

TEST(RingModNode, DryMixIsTransparent)
{
    Rig rig = rigFor<avc::node::fx::RingModNode>();
    rig.set("mix", 0.0F).prepare();

    const std::vector<float> input = sine(440.0F, 128 * 10);
    const std::vector<float> output = rig.stream(input);
    for (std::size_t i = 0; i < output.size(); ++i) {
        EXPECT_NEAR(output[i], input[i], 1e-6F) << "at " << i;
    }
}

TEST(RingModNode, MovesEnergyOffTheCarrier)
{
    Rig rig = rigFor<avc::node::fx::RingModNode>();
    rig.set("freq", 100.0F).set("mix", 1.0F).prepare();

    const std::vector<float> output = rig.stream(sine(400.0F, kRate / 2));
    EXPECT_GT(std::fabs(crossingRate(output) - 400.0F), 20.0F);
}

TEST(SignalNode, GeneratesWithNoInputAtAll)
{
    auto node = std::make_unique<avc::node::debug::SignalNode>();
    const NodeDescriptor desc = avc::node::debug::SignalNode::descriptor();
    EXPECT_TRUE(desc.inputs.empty());

    for (std::size_t i = 0; i < desc.params.size(); ++i) {
        node->setParam(static_cast<std::uint32_t>(i), desc.params[i].default_value);
    }
    node->setParam(static_cast<std::uint32_t>(desc.indexOfParam("level_db")), 0.0F);
    node->prepare({kRate, 128, 0, 1});

    std::vector<Sample> out(128, 0.0F);
    Sample *out_ptr = out.data();
    for (int block = 0; block < 20; ++block) {
        node->process({nullptr, &out_ptr, 0, 1, 128});
    }
    EXPECT_GT(peak(out), 0.5F);
}

TEST(SignalNode, SineComesOutAtTheFrequencyAsked)
{
    Rig rig = rigFor<avc::node::debug::SignalNode>();
    rig.set("freq", 500.0F).set("level_db", 0.0F).prepare();

    std::vector<float> result;
    for (std::uint32_t block = 0; block < kRate / 128; ++block) {
        rig.run();
        result.insert(result.end(), rig.out().begin(), rig.out().end());
    }
    EXPECT_NEAR(crossingRate(result), 500.0F, 5.0F);
}

TEST(SignalNode, SilenceIsSilent)
{
    Rig rig = rigFor<avc::node::debug::SignalNode>();
    rig.set("waveform", 5.0F).set("level_db", 0.0F).prepare();
    rig.run();
    EXPECT_EQ(peak(std::vector<float>(rig.out().begin(), rig.out().end())), 0.0F);
}

TEST(ScopeNode, PassesAudioThroughUntouched)
{
    Rig rig = rigFor<avc::node::debug::ScopeNode>();
    rig.prepare();

    const std::vector<float> input = sine(300.0F, 128 * 20);
    const std::vector<float> output = rig.stream(input);
    for (std::size_t i = 0; i < output.size(); ++i) {
        EXPECT_EQ(output[i], input[i]) << "at " << i;
    }
}

TEST(ScopeNode, PublishesAWholeWindowAtATime)
{
    auto node = std::make_unique<avc::node::debug::ScopeNode>();
    auto *scope = node.get();
    Rig rig(std::move(node), avc::node::debug::ScopeNode::descriptor());
    rig.set("window_ms", 20.0F).prepare();

    avc::node::debug::ScopeFrame frame;
    EXPECT_FALSE(scope->takeFrame(frame));

    rig.stream(sine(440.0F, 128 * 40, 0.5F));
    ASSERT_TRUE(scope->takeFrame(frame));

    EXPECT_EQ(frame.wave.size(), avc::node::debug::kScopePoints);
    EXPECT_NEAR(peak({frame.wave.begin(), frame.wave.end()}), 0.5F, 0.02F);

    std::size_t loudest = 0;
    for (std::size_t b = 1; b < frame.bands.size(); ++b) {
        if (frame.bands[b] > frame.bands[loudest]) {
            loudest = b;
        }
    }
    const float centre = avc::node::debug::ScopeNode::bandCentre(
        static_cast<std::uint32_t>(loudest));
    EXPECT_GT(centre, 250.0F);
    EXPECT_LT(centre, 750.0F);
}

TEST(ScopeNode, DropsFramesRatherThanQueueingThemUp)
{
    auto node = std::make_unique<avc::node::debug::ScopeNode>();
    auto *scope = node.get();
    Rig rig(std::move(node), avc::node::debug::ScopeNode::descriptor());
    rig.set("window_ms", 5.0F).prepare();

    rig.stream(sine(440.0F, kRate, 0.5F));

    avc::node::debug::ScopeFrame frame;
    EXPECT_TRUE(scope->takeFrame(frame));
    EXPECT_FALSE(scope->takeFrame(frame)) << "takeFrame must drain, not hand back a backlog";
}

TEST(TextNode, IsATextSink)
{
    const NodeDescriptor descriptor = avc::node::debug::TextNode::descriptor();
    ASSERT_EQ(descriptor.inputs.size(), 1U);
    EXPECT_EQ(descriptor.inputs[0].type, avc::node::kTextPortType);
    EXPECT_TRUE(descriptor.outputs.empty());
}

TEST(TextNode, PublishesTheNewestStreamingSnapshot)
{
    avc::node::debug::TextNode node;
    node.prepare({kRate, 128, 1, 0});

    std::array<std::byte, avc::node::kTextPortTypeBytes> block{};
    const void *input = block.data();
    const NodeContext context{nullptr, nullptr, 1, 0, 128, &input, nullptr};
    const auto publish = [&](std::string_view value) {
        const auto length = static_cast<std::uint32_t>(value.size());
        std::memcpy(block.data(), &length, sizeof(length));
        std::memcpy(block.data() + sizeof(length), value.data(), value.size());
        node.process(context);
    };

    publish("H");
    publish("Hello");
    publish("Hello, \xE4\xB8\x96\xE7\x95\x8C");

    std::string text;
    ASSERT_TRUE(node.takeText(text));
    EXPECT_EQ(text, "Hello, \xE4\xB8\x96\xE7\x95\x8C");
    EXPECT_FALSE(node.takeText(text));

    publish("Hello, \xE4\xB8\x96\xE7\x95\x8C");
    EXPECT_FALSE(node.takeText(text)) << "an unchanged value is not another stream update";

    publish("");
    ASSERT_TRUE(node.takeText(text));
    EXPECT_TRUE(text.empty());
}

TEST(TextNode, MakesATruncatedUtf8SnapshotSafeForJson)
{
    avc::node::debug::TextNode node;
    node.prepare({kRate, 128, 1, 0});

    std::array<std::byte, avc::node::kTextPortTypeBytes> block{};
    const std::uint32_t length = 2;
    std::memcpy(block.data(), &length, sizeof(length));
    block[sizeof(length)] = std::byte{0xE4};
    block[sizeof(length) + 1] = std::byte{0xB8};
    const void *input = block.data();
    node.process({nullptr, nullptr, 1, 0, 128, &input, nullptr});

    std::string text;
    ASSERT_TRUE(node.takeText(text));
    EXPECT_EQ(text, "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(TextNode, PublishesFinalMetadataEvenWhenTheTextDoesNotChange)
{
    avc::node::debug::TextNode node;
    node.prepare({kRate, 128, 1, 0});

    std::array<std::byte, avc::node::kTextPortTypeBytes> block{};
    const void *input = block.data();
    const NodeContext context{nullptr, nullptr, 1, 0, 128, &input, nullptr};

    avc::text::writeFrame(block.data(), "hello", 9, 2, 1, false);
    node.process(context);
    avc::node::debug::TextSnapshot snapshot;
    ASSERT_TRUE(node.takeText(snapshot));
    EXPECT_TRUE(snapshot.segmented);
    EXPECT_FALSE(snapshot.final);
    EXPECT_EQ(snapshot.stream, 9U);
    EXPECT_EQ(snapshot.segment, 2U);

    avc::text::writeFrame(block.data(), "hello", 9, 2, 2, true);
    node.process(context);
    ASSERT_TRUE(node.takeText(snapshot));
    EXPECT_EQ(snapshot.text, "hello");
    EXPECT_TRUE(snapshot.final);
    EXPECT_EQ(snapshot.revision, 2U);
}

TEST(AbNode, SelectsEitherSide)
{
    auto node = std::make_unique<avc::node::debug::AbNode>();
    auto *ab = node.get();
    const NodeDescriptor desc = avc::node::debug::AbNode::descriptor();
    ab->setParam(0, 0.0F);
    ab->prepare({kRate, 128, 2, 1});

    std::vector<Sample> a(128, 1.0F);
    std::vector<Sample> b(128, -1.0F);
    std::vector<Sample> out(128, 0.0F);
    const Sample *in_ptrs[2] = {a.data(), b.data()};
    Sample *out_ptr = out.data();

    for (int block = 0; block < 200; ++block) {
        ab->process({in_ptrs, &out_ptr, 2, 1, 128});
    }
    EXPECT_NEAR(out[0], 1.0F, 1e-3F);

    ab->setParam(0, 1.0F);
    for (int block = 0; block < 200; ++block) {
        ab->process({in_ptrs, &out_ptr, 2, 1, 128});
    }
    EXPECT_NEAR(out[0], -1.0F, 1e-3F);
    EXPECT_EQ(desc.inputs.size(), 2U);
}

TEST(AbNode, HoldsPowerThroughTheCrossfade)
{
    auto node = std::make_unique<avc::node::debug::AbNode>();
    auto *ab = node.get();
    ab->setParam(0, 0.5F);
    ab->prepare({kRate, 128, 2, 1});

    std::vector<Sample> a = sine(375.0F, 128, 1.0F);
    std::vector<Sample> b = sine(1500.0F, 128, 1.0F);
    std::vector<Sample> out(128, 0.0F);
    const Sample *in_ptrs[2] = {a.data(), b.data()};
    Sample *out_ptr = out.data();
    ab->process({in_ptrs, &out_ptr, 2, 1, 128});

    double sum_in = 0.0;
    double sum_out = 0.0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        sum_in += static_cast<double>(a[i]) * a[i] + static_cast<double>(b[i]) * b[i];
        sum_out += static_cast<double>(out[i]) * out[i];
    }
    EXPECT_NEAR(sum_out / (sum_in * 0.5), 1.0, 0.05);
}

TEST(EveryNode, ToleratesAnUnconnectedInput)
{
    for (const NodeDescriptor *desc : avc::node::NodeRegistry::instance().all()) {
        std::unique_ptr<Node> node = avc::node::NodeRegistry::instance().create(desc->type);
        if (node == nullptr || desc->outputs.empty()) {
            continue;
        }
        node->prepare({kRate, 128, 1, 1});

        std::vector<Sample> out(128, -999.0F);
        const Sample *in_ptr = nullptr;
        Sample *out_ptr = out.data();
        node->process({&in_ptr, &out_ptr, 1, 1, 128});

        for (Sample value : out) {
            EXPECT_EQ(value, 0.0F) << desc->type << " left an unconnected input unwritten";
        }
    }
}

TEST(EveryNode, SurvivesEveryParameterExtreme)
{
    const std::vector<float> input = sine(440.0F, 128);

    for (const NodeDescriptor *desc : avc::node::NodeRegistry::instance().all()) {
        for (std::size_t p = 0; p < desc->params.size(); ++p) {
            for (float value : {desc->params[p].min, desc->params[p].max}) {
                std::unique_ptr<Node> node = avc::node::NodeRegistry::instance().create(desc->type);
                if (node == nullptr) {
                    continue;
                }
                for (std::size_t i = 0; i < desc->params.size(); ++i) {
                    node->setParam(static_cast<std::uint32_t>(i), desc->params[i].default_value);
                }
                node->setParam(static_cast<std::uint32_t>(p), value);
                node->prepare({kRate, 128, 1, 1});

                std::vector<Sample> out(128, 0.0F);
                const Sample *in_ptr = input.data();
                Sample *out_ptr = out.data();
                for (int block = 0; block < 40; ++block) {
                    node->process({&in_ptr, &out_ptr, 1, 1, 128});
                }
                for (Sample sample : out) {
                    ASSERT_TRUE(std::isfinite(sample))
                        << desc->type << '.' << desc->params[p].name << " = " << value;
                }
            }
        }
    }
}

}
