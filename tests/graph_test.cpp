#include "audio/AudioBackend.hpp"
#include "graph/GraphCompiler.hpp"
#include "graph/GraphHost.hpp"
#include "graph/GraphSpec.hpp"
#include "node/Node.hpp"
#include "node/NodeRegistry.hpp"
#include "node/PortTypeManifest.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using avc::audio::ProcessContext;
using avc::graph::CompileEnv;
using avc::graph::CompiledGraph;
using avc::graph::GraphCompiler;
using avc::graph::GraphHost;
using avc::graph::GraphSpec;
using avc::graph::SpecNode;
using avc::types::Sample;

class Block {
public:
    Block(std::uint32_t n_in, std::uint32_t n_out, std::uint32_t nframes)
        : nframes_(nframes), in_(n_in, std::vector<Sample>(nframes, 0.0F)),
          out_(n_out, std::vector<Sample>(nframes, -999.0F))
    {
        for (auto &buffer : in_) {
            in_ptrs_.push_back(buffer.data());
        }
        for (auto &buffer : out_) {
            out_ptrs_.push_back(buffer.data());
        }
    }

    void fillInput(std::size_t index, Sample value)
    {
        std::fill(in_[index].begin(), in_[index].end(), value);
    }

    void disconnectInput(std::size_t index) { in_ptrs_[index] = nullptr; }

    ProcessContext context(std::uint64_t cycle = 0)
    {
        return ProcessContext{in_ptrs_.data(),
                              out_ptrs_.data(),
                              static_cast<std::uint32_t>(in_ptrs_.size()),
                              static_cast<std::uint32_t>(out_ptrs_.size()),
                              nframes_,
                              cycle};
    }

    Sample output(std::size_t index, std::size_t frame = 0) const { return out_[index][frame]; }

private:
    std::uint32_t nframes_;
    std::vector<std::vector<Sample>> in_;
    std::vector<std::vector<Sample>> out_;
    std::vector<Sample *> in_ptrs_;
    std::vector<Sample *> out_ptrs_;
};

class FakeBinder final : public avc::audio::IoBinder {
public:
    bool bindIo(const std::vector<avc::audio::IoRequest> &requests,
                std::map<std::string, std::vector<std::uint32_t>> &slots,
                std::string &) override
    {
        slots.clear();
        bound = requests;
        std::uint32_t in_slot = 0;
        std::uint32_t out_slot = 0;
        for (const avc::audio::IoRequest &request : requests) {
            std::uint32_t &next = avc::audio::producesSignal(request.kind) ? in_slot : out_slot;
            for (std::uint32_t c = 0; c < request.channels; ++c) {
                slots[request.node].push_back(next++);
            }
        }
        return true;
    }

    void releaseIo(const std::set<std::string> &keep) override { released = keep; }

    std::vector<avc::audio::IoRequest> bound;
    std::set<std::string> released;
};

CompileEnv envFor(const GraphSpec &spec, std::uint32_t quantum = 64)
{
    CompileEnv e;
    e.max_quantum = quantum;

    std::vector<avc::audio::IoRequest> requests;
    std::string error;
    GraphCompiler::ioRequests(spec, requests, error);

    std::uint32_t in_slot = 0;
    std::uint32_t out_slot = 0;
    for (const avc::audio::IoRequest &request : requests) {
        std::uint32_t &next = avc::audio::producesSignal(request.kind) ? in_slot : out_slot;
        for (std::uint32_t c = 0; c < request.channels; ++c) {
            e.io_slots[request.node].push_back(next++);
        }
    }
    return e;
}

SpecNode input(const std::string &id, const std::string &source = "mic", std::uint32_t ch = 1)
{
    return {.id = id, .type = "capture", .options = {{"source", source}}, .outputs = ch};
}

SpecNode output(const std::string &id, const std::string &device = "speakers",
                std::uint32_t ch = 1)
{
    return {.id = id, .type = "playback", .options = {{"device", device}}, .inputs = ch};
}

SpecNode gain(const std::string &id, float db = 0.0F)
{
    return {.id = id, .type = "gain", .params = {{"gain_db", db}}};
}

SpecNode pitch(const std::string &id, float semitones, float grain_ms = 40.0F)
{
    return {.id = id,
            .type = "pitch",
            .params = {{"semitones", semitones}, {"grain_ms", grain_ms}}};
}

SpecNode splitter(const std::string &id, std::uint32_t outputs)
{
    return {.id = id, .type = "splitter", .outputs = outputs};
}

bool isIoType(const std::string &type)
{
    return type == "capture" || type == "playback" || type == "virtual_mic"
           || type == "virtual_speaker";
}

void connect(GraphSpec &spec, const std::string &from, const std::string &from_port,
             const std::string &to, const std::string &to_port, float gain_db = 0.0F)
{
    const auto resolve = [&spec](const std::string &id, const std::string &port) {
        for (const SpecNode &node : spec.nodes) {
            if (node.id == id && isIoType(node.type) && (port == "in" || port == "out")) {
                return port + "_1";
            }
        }
        return port;
    };
    spec.edges.push_back(
        {{from, resolve(from, from_port)}, {to, resolve(to, to_port)}, gain_db});
}

std::unique_ptr<CompiledGraph> compileOf(const GraphSpec &spec, std::string &error)
{
    return GraphCompiler::compile(spec, envFor(spec), nullptr, error);
}

GraphSpec passthrough(float db = 0.0F)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), gain("g", db), output("spk")};
    connect(spec, "mic", "out", "g", "in");
    connect(spec, "g", "out", "spk", "in");
    return spec;
}

std::atomic<int> g_probe_active{0};
std::atomic<int> g_probe_max_active{0};
std::atomic<int> g_probe_arrivals{0};
std::mutex g_probe_mutex;
std::condition_variable g_probe_cv;

class ParallelProbeNode final : public avc::node::Node {
public:
    void prepare(const avc::node::PrepareInfo &) override {}
    void setParam(std::uint32_t, float) noexcept override {}

    void process(const avc::node::NodeContext &ctx) noexcept override
    {
        const int active = g_probe_active.fetch_add(1, std::memory_order_acq_rel) + 1;
        int maximum = g_probe_max_active.load(std::memory_order_relaxed);
        while (maximum < active
               && !g_probe_max_active.compare_exchange_weak(
                   maximum, active, std::memory_order_relaxed)) {
        }

        g_probe_arrivals.fetch_add(1, std::memory_order_release);
        g_probe_cv.notify_all();
        {
            std::unique_lock<std::mutex> lock(g_probe_mutex);
            g_probe_cv.wait_for(lock, std::chrono::milliseconds(500), [] {
                return g_probe_arrivals.load(std::memory_order_acquire) >= 2;
            });
        }

        if (ctx.outputs[0] != nullptr) {
            if (ctx.inputs[0] != nullptr) {
                std::copy_n(ctx.inputs[0], ctx.nframes, ctx.outputs[0]);
            } else {
                std::fill_n(ctx.outputs[0], ctx.nframes, 0.0F);
            }
        }
        g_probe_active.fetch_sub(1, std::memory_order_release);
    }
};

bool registerParallelProbe()
{
    avc::node::NodeDescriptor descriptor;
    descriptor.type = "test_parallel_probe";
    descriptor.category = "test";
    descriptor.label = "Parallel probe";
    descriptor.inputs = {{"in", ""}};
    descriptor.outputs = {{"out", ""}};
    return avc::node::NodeRegistry::instance().add(
        std::move(descriptor), [] { return std::make_unique<ParallelProbeNode>(); });
}

const bool g_parallel_probe_registered = registerParallelProbe();

TEST(GraphCompiler, CompilesAChainAndRunsIt)
{
    std::string error;
    auto graph = compileOf(passthrough(-6.0F), error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->nodeCount(), 1U);
    EXPECT_EQ(graph->stageCount(), 1U);

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    EXPECT_NEAR(block.output(0), std::pow(10.0F, -6.0F / 20.0F), 1e-4F);
}

TEST(GraphCompiler, RunsNodesInDependencyOrder)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), gain("a", 12.0F), gain("b", -12.0F), output("spk")};
    connect(spec, "b", "out", "spk", "in");
    connect(spec, "a", "out", "b", "in");
    connect(spec, "mic", "out", "a", "in");

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 1, 64);
    block.fillInput(0, 0.25F);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    EXPECT_NEAR(block.output(0), 0.25F, 1e-4F);
}

TEST(GraphCompiler, RejectsAFeedbackLoop)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), gain("a"), gain("b"), output("spk")};
    connect(spec, "mic", "out", "a", "in");
    connect(spec, "a", "out", "b", "in");
    connect(spec, "b", "out", "a", "in");
    connect(spec, "b", "out", "spk", "in");

    std::string error;
    EXPECT_EQ(GraphCompiler::compile(spec, envFor(spec), nullptr, error), nullptr);
    EXPECT_NE(error.find("feedback loop"), std::string::npos) << error;
}

TEST(GraphCompiler, RejectsUnknownNodeType)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), SpecNode{.id = "x", .type = "reverb"}, output("spk")};
    std::string error;
    EXPECT_EQ(GraphCompiler::compile(spec, envFor(spec), nullptr, error), nullptr);
    EXPECT_NE(error.find("unknown type"), std::string::npos) << error;
}

TEST(GraphCompiler, RejectsDuplicateNodeId)
{
    GraphSpec spec;
    spec.nodes = {gain("g"), gain("g")};
    std::string error;
    EXPECT_EQ(GraphCompiler::compile(spec, envFor(spec), nullptr, error), nullptr);
    EXPECT_NE(error.find("duplicate node id"), std::string::npos) << error;
}

TEST(GraphCompiler, SumsTwoEdgesIntoOneInput)
{
    GraphSpec spec;
    spec.nodes = {input("a", "mic_a"), input("b", "mic_b"), gain("g"), output("spk")};
    connect(spec, "a", "out", "g", "in", -6.0F);
    connect(spec, "b", "out", "g", "in", 6.0F);
    connect(spec, "g", "out", "spk", "in");

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(2, 1, 64);
    block.fillInput(0, 0.25F);
    block.fillInput(1, 0.5F);
    graph->process(block.context());
    const float expected = 0.25F * std::pow(10.0F, -6.0F / 20.0F)
                           + 0.5F * std::pow(10.0F, 6.0F / 20.0F);
    EXPECT_NEAR(block.output(0), expected, 1e-5F);
}

TEST(GraphCompiler, RejectsAnIoNodeWithNothingSelected)
{
    GraphSpec spec;
    spec.nodes = {SpecNode{.id = "mic", .type = "capture"}, gain("g"), output("spk")};
    connect(spec, "mic", "out", "g", "in");
    connect(spec, "g", "out", "spk", "in");

    std::vector<avc::audio::IoRequest> requests;
    std::string error;
    EXPECT_FALSE(GraphCompiler::ioRequests(spec, requests, error));
    EXPECT_NE(error.find("nothing selected"), std::string::npos) << error;
}

TEST(GraphCompiler, ReadsWhatEachIoNodeIsAttachedTo)
{
    GraphSpec spec;
    spec.nodes = {input("mic", "headset", 1),
                  SpecNode{.id = "game",
                           .type = "virtual_speaker",
                           .options = {{"publish_as", "game_audio"}},
                           .outputs = 2},
                  SpecNode{.id = "cast",
                           .type = "virtual_mic",
                           .options = {{"publish_as", "stream_mic"}},
                           .inputs = 1},
                  output("spk", "hdmi", 2)};

    std::vector<avc::audio::IoRequest> requests;
    std::string error;
    ASSERT_TRUE(GraphCompiler::ioRequests(spec, requests, error)) << error;
    ASSERT_EQ(requests.size(), 4U);

    EXPECT_EQ(requests[0].target, "headset");
    EXPECT_EQ(requests[0].channels, 1U);
    EXPECT_EQ(requests[1].kind, avc::audio::IoKind::VirtualSpeaker);
    EXPECT_EQ(requests[1].target, "game_audio");
    EXPECT_EQ(requests[1].channels, 2U);
    EXPECT_EQ(requests[2].kind, avc::audio::IoKind::VirtualMic);
    EXPECT_EQ(requests[3].kind, avc::audio::IoKind::Playback);
    EXPECT_EQ(requests[3].channels, 2U);
}

TEST(GraphCompiler, TwoNodesMayPointAtTheSameDevice)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), gain("g"), output("a", "spk"), output("b", "spk")};
    connect(spec, "mic", "out", "g", "in");
    connect(spec, "g", "out", "a", "in");
    connect(spec, "g", "out", "b", "in");

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 2, 64);
    block.fillInput(0, 0.5F);
    ProcessContext ctx = block.context();
    graph->process(ctx);
    EXPECT_NEAR(block.output(0), 0.5F, 1e-5F);
    EXPECT_NEAR(block.output(1), 0.5F, 1e-5F);
}

TEST(GraphCompiler, AMultichannelCaptureCarriesEveryChannel)
{
    GraphSpec spec;
    spec.nodes = {input("desktop", "speakers", 2), output("l", "spk_l"), output("r", "spk_r")};
    connect(spec, "desktop", "out_1", "l", "in_1");
    connect(spec, "desktop", "out_2", "r", "in_1");

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(2, 2, 64);
    block.fillInput(0, 0.25F);
    block.fillInput(1, 0.75F);
    ProcessContext ctx = block.context();
    graph->process(ctx);
    EXPECT_NEAR(block.output(0), 0.25F, 1e-5F);
    EXPECT_NEAR(block.output(1), 0.75F, 1e-5F);
}

TEST(GraphCompiler, RejectsAPortThatDoesNotExist)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), gain("g"), output("spk")};
    connect(spec, "mic", "out", "g", "in_7");
    std::string error;
    EXPECT_EQ(GraphCompiler::compile(spec, envFor(spec), nullptr, error), nullptr);
    EXPECT_NE(error.find("does not exist"), std::string::npos) << error;
}

TEST(GraphCompiler, LongChainReusesTwoBuffers)
{
    GraphSpec spec;
    spec.nodes.push_back(input("mic"));
    for (int i = 0; i < 20; ++i) {
        spec.nodes.push_back(gain("g" + std::to_string(i)));
    }
    spec.nodes.push_back(output("spk"));

    connect(spec, "mic", "out", "g0", "in");
    for (int i = 1; i < 20; ++i) {
        connect(spec, "g" + std::to_string(i - 1), "out", "g" + std::to_string(i), "in");
    }
    connect(spec, "g19", "out", "spk", "in");

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->stageCount(), 20U);
    EXPECT_EQ(graph->bufferSlots(), 2U);
}

TEST(GraphCompiler, FanOutDoesNotCopy)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), gain("g", 0.0F), output("l", "spk_l"), output("r", "spk_r")};
    connect(spec, "mic", "out", "g", "in");
    connect(spec, "g", "out", "l", "in");
    connect(spec, "g", "out", "r", "in");

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->stageCount(), 1U);

    Block block(1, 2, 64);
    block.fillInput(0, 0.5F);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    EXPECT_NEAR(block.output(0), 0.5F, 1e-5F);
    EXPECT_NEAR(block.output(1), 0.5F, 1e-5F);
}

TEST(GraphCompiler, OutputSumsEdgesWithIndependentGain)
{
    GraphSpec spec;
    spec.nodes = {input("a", "mic_a"), input("b", "mic_b"), output("spk")};
    connect(spec, "a", "out", "spk", "in", -6.0F);
    connect(spec, "b", "out", "spk", "in", 6.0F);

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(2, 1, 64);
    block.fillInput(0, 0.25F);
    block.fillInput(1, 0.5F);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    const float expected = 0.25F * std::pow(10.0F, -6.0F / 20.0F)
                           + 0.5F * std::pow(10.0F, 6.0F / 20.0F);
    EXPECT_NEAR(block.output(0), expected, 1e-5F);
}

TEST(GraphCompiler, SplitterFeedsEveryOutputIndependently)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), splitter("split", 3), gain("a", 0.0F), gain("b", -6.0F),
                  gain("c", -90.0F), output("x", "spk_x"), output("y", "spk_y"), output("z", "spk_z")};
    connect(spec, "mic", "out", "split", "in");
    connect(spec, "split", "out_1", "a", "in");
    connect(spec, "split", "out_2", "b", "in");
    connect(spec, "split", "out_3", "c", "in");
    connect(spec, "a", "out", "x", "in");
    connect(spec, "b", "out", "y", "in");
    connect(spec, "c", "out", "z", "in");

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 3, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    EXPECT_NEAR(block.output(0), 1.0F, 1e-5F);
    EXPECT_NEAR(block.output(1), std::pow(10.0F, -6.0F / 20.0F), 1e-4F);
    EXPECT_NEAR(block.output(2), 0.0F, 1e-5F);
}

TEST(GraphCompiler, SplitterFallsBackToItsDescriptorPortCount)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), {.id = "split", .type = "splitter"}, output("x", "spk_x"), output("y", "spk_y")};
    connect(spec, "mic", "out", "split", "in");
    connect(spec, "split", "out_1", "x", "in");
    connect(spec, "split", "out_2", "y", "in");

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    connect(spec, "split", "out_3", "x", "in");
    EXPECT_EQ(GraphCompiler::compile(spec, envFor(spec), nullptr, error), nullptr);
    EXPECT_NE(error.find("does not exist"), std::string::npos) << error;
}

TEST(GraphCompiler, TreatsAnUnconnectedBackendPortAsSilence)
{
    std::string error;
    auto graph = compileOf(passthrough(), error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    block.disconnectInput(0);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    EXPECT_FLOAT_EQ(block.output(0), 0.0F);
}

TEST(GraphCompiler, WritesSilenceToAnOutputWithNothingConnected)
{
    GraphSpec spec;
    spec.nodes = {output("spk")};
    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 1, 64);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    EXPECT_FLOAT_EQ(block.output(0), 0.0F);
}

TEST(GraphLatency, AGraphOfNodesThatDoNotDelayHasNone)
{
    std::string error;
    auto graph = compileOf(passthrough(), error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->latencyFrames(), 0U);
}

TEST(GraphLatency, SumsAlongAChain)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), pitch("p1", 5.0F, 40.0F), pitch("p2", 5.0F, 20.0F), output("spk")};
    connect(spec, "mic", "out", "p1", "in");
    connect(spec, "p1", "out", "p2", "in");
    connect(spec, "p2", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;

    const std::uint32_t expected =
        static_cast<std::uint32_t>(avc::types::kDefaultSampleRate * 0.030F);
    EXPECT_NEAR(static_cast<double>(graph->latencyFrames()), expected, 4.0);
}

TEST(GraphLatency, TakesTheWorstPathNotTheSum)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), splitter("split", 2), pitch("slow", 5.0F, 100.0F),
                  pitch("fast", 5.0F, 20.0F), output("spk")};
    connect(spec, "mic", "out", "split", "in");
    connect(spec, "split", "out_1", "slow", "in");
    connect(spec, "split", "out_2", "fast", "in");
    connect(spec, "slow", "out", "spk", "in");
    connect(spec, "fast", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;

    const std::uint32_t expected =
        static_cast<std::uint32_t>(avc::types::kDefaultSampleRate * 0.050F);
    EXPECT_NEAR(static_cast<double>(graph->latencyFrames()), expected, 4.0);
}

TEST(GraphLatency, IgnoresAChainThatReachesNoOutput)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), splitter("split", 2), pitch("dangling", 12.0F, 120.0F),
                  gain("g"), output("spk")};
    connect(spec, "mic", "out", "split", "in");
    connect(spec, "split", "out_1", "g", "in");
    connect(spec, "split", "out_2", "dangling", "in");
    connect(spec, "g", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->latencyFrames(), 0U);
}

TEST(GraphLatency, FollowsAParameterWithoutARecompile)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), pitch("p", 7.0F, 60.0F), output("spk")};
    connect(spec, "mic", "out", "p", "in");
    connect(spec, "p", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_GT(graph->latencyFrames(), 0U);

    const int node = graph->indexOfNode("p");
    ASSERT_GE(node, 0);
    graph->applyParam(static_cast<std::uint32_t>(node), 0, 0.0F);
    EXPECT_EQ(graph->latencyFrames(), 0U);
}

TEST(GraphProfiling, IsOffUntilAskedFor)
{
    std::string error;
    auto graph = compileOf(passthrough(), error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_FALSE(graph->profiling());

    Block block(1, 1, 64);
    ProcessContext ctx = block.context();
    graph->process(ctx);
    EXPECT_EQ(graph->stageNs(0), 0U);
}

TEST(GraphProfiling, TimesEachNodeOnceSwitchedOn)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), gain("g"), pitch("p", 7.0F), output("spk")};
    connect(spec, "mic", "out", "g", "in");
    connect(spec, "g", "out", "p", "in");
    connect(spec, "p", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;

    graph->setProfiling(true);
    Block block(1, 1, 64);
    block.fillInput(0, 0.5F);
    for (int i = 0; i < 200; ++i) {
        ProcessContext ctx = block.context(static_cast<std::uint64_t>(i));
        graph->process(ctx);
    }

    bool measured = false;
    for (std::size_t s = 0; s < graph->stageCount(); ++s) {
        measured = measured || graph->stageNs(s) > 0;
        EXPECT_FALSE(graph->nodeIdAt(graph->stageNodeIndex(s)).empty());
    }
    EXPECT_TRUE(measured);

    graph->resetProfile();
    for (std::size_t s = 0; s < graph->stageCount(); ++s) {
        EXPECT_EQ(graph->stageNs(s), 0U);
    }
}

TEST(GraphHost, KeepsRunningTheOldGraphWhenTheNewOneFails)
{
    FakeBinder binder;
    GraphHost host(CompileEnv{}, &binder);
    std::string error;
    ASSERT_TRUE(host.apply(passthrough(0.0F), error)) << error;

    GraphSpec broken;
    broken.nodes = {input("mic"), SpecNode{.id = "x", .type = "does_not_exist"}};
    EXPECT_FALSE(host.apply(broken, error));

    Block block(1, 1, 64);
    block.fillInput(0, 0.5F);
    ProcessContext ctx = block.context();
    host.process(ctx);
    EXPECT_NEAR(block.output(0), 0.5F, 1e-5F);
}

TEST(GraphHost, CarriesGainStateAcrossASwap)
{
    FakeBinder binder;
    GraphHost host(CompileEnv{}, &binder);
    std::string error;
    ASSERT_TRUE(host.apply(passthrough(0.0F), error)) << error;

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();
    host.process(ctx);
    ASSERT_NEAR(block.output(0), 1.0F, 1e-5F);

    ASSERT_TRUE(host.setParam("g", "gain_db", -90.0F));
    for (int i = 0; i < 3; ++i) {
        host.process(ctx);
    }
    const float mid_ramp = block.output(0);
    EXPECT_LT(mid_ramp, 0.9F);
    EXPECT_GT(mid_ramp, 0.0F);

    ASSERT_TRUE(host.apply(passthrough(0.0F), error)) << error;
    host.process(ctx);
    EXPECT_NEAR(block.output(0), mid_ramp, 0.25F);
    EXPECT_LT(block.output(0), 0.95F);
}

TEST(GraphHost, DropsParameterUpdatesAimedAtAReplacedGraph)
{
    FakeBinder binder;
    GraphHost host(CompileEnv{}, &binder);
    std::string error;
    ASSERT_TRUE(host.apply(passthrough(0.0F), error)) << error;

    ASSERT_TRUE(host.setParam("g", "gain_db", -90.0F));
    ASSERT_TRUE(host.apply(passthrough(0.0F), error)) << error;

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();
    for (int i = 0; i < 40; ++i) {
        host.process(ctx);
    }
    EXPECT_NEAR(block.output(0), 1.0F, 1e-4F);
}

TEST(GraphHost, SwapsBetweenGraphsOfVeryDifferentSize)
{
    FakeBinder binder;
    GraphHost host(CompileEnv{}, &binder);
    std::string error;
    ASSERT_TRUE(host.apply(passthrough(0.0F), error)) << error;
    EXPECT_EQ(host.stats().nodes, 1U);

    GraphSpec big;
    big.nodes.push_back(input("mic"));
    for (int i = 0; i < 12; ++i) {
        big.nodes.push_back(gain("g" + std::to_string(i)));
    }
    big.nodes.push_back(output("l", "spk_l"));
    big.nodes.push_back(output("r", "spk_r"));
    connect(big, "mic", "out", "g0", "in");
    for (int i = 1; i < 12; ++i) {
        connect(big, "g" + std::to_string(i - 1), "out", "g" + std::to_string(i), "in");
    }
    connect(big, "g11", "out", "l", "in");
    connect(big, "g11", "out", "r", "in");

    ASSERT_TRUE(host.apply(big, error)) << error;
    EXPECT_EQ(host.stats().nodes, 12U);
    EXPECT_EQ(host.stats().swaps, 2U);

    Block block(1, 2, 64);
    block.fillInput(0, 0.5F);
    ProcessContext ctx = block.context();
    host.process(ctx);
    EXPECT_NEAR(block.output(0), 0.5F, 1e-4F);
    EXPECT_NEAR(block.output(1), 0.5F, 1e-4F);

    host.poll();
    EXPECT_EQ(host.stats().pending_retired, 0U);
}

SpecNode inDomain(SpecNode node, const std::string &domain = "cold")
{
    node.domain = domain;
    return node;
}

GraphSpec coldPassthrough(std::uint32_t block = 128, std::uint32_t safety = 1, float db = 0.0F)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), inDomain(gain("g", db)), output("spk")};
    spec.domains["cold"] = {block, safety};
    connect(spec, "mic", "out", "g", "in");
    connect(spec, "g", "out", "spk", "in");
    return spec;
}

TEST(ColdDomain, LeavesAGraphThatNeverAskedForOneAlone)
{
    std::string error;
    auto graph = compileOf(passthrough(-6.0F), error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->domainCount(), 1U);
    EXPECT_EQ(graph->crossingCount(), 0U);
    EXPECT_EQ(graph->latencyFrames(), 0U);
}

TEST(ColdDomain, SplitsTheOrderAndPlacesOneCrossingEachWay)
{
    std::string error;
    auto graph = compileOf(coldPassthrough(), error);
    ASSERT_NE(graph, nullptr) << error;

    ASSERT_EQ(graph->domainCount(), 2U);
    EXPECT_EQ(graph->crossingCount(), 2U);

    const std::vector<avc::graph::DomainInfo> domains = graph->domains();
    EXPECT_EQ(domains[0].name, "hot");
    EXPECT_FALSE(domains[0].cold);
    EXPECT_EQ(domains[0].stages, 0U) << "the only dsp node went to the cold side";
    EXPECT_EQ(domains[1].name, "cold");
    EXPECT_TRUE(domains[1].cold);
    EXPECT_EQ(domains[1].block, 128U);
    EXPECT_EQ(domains[1].stages, 1U);
}

TEST(ColdDomain, RefusesToRunAnIoNodeCold)
{
    GraphSpec spec = passthrough();
    spec.nodes[0].domain = "cold";

    std::string error;
    EXPECT_EQ(compileOf(spec, error), nullptr);
    EXPECT_NE(error.find("io node"), std::string::npos) << error;
    EXPECT_NE(error.find("mic"), std::string::npos) << error;
}

TEST(ColdDomain, ReportsTheCrossingAsLatency)
{
    std::string error;
    auto graph = compileOf(coldPassthrough(128, 1), error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->latencyFrames(), 256U);

    auto tighter = compileOf(coldPassthrough(128, 0), error);
    ASSERT_NE(tighter, nullptr) << error;
    EXPECT_EQ(tighter->latencyFrames(), 128U) << "no margin is still one block of prefill";

    auto safer = compileOf(coldPassthrough(256, 2), error);
    ASSERT_NE(safer, nullptr) << error;
    EXPECT_EQ(safer->latencyFrames(), 768U);
}

TEST(ColdDomain, AddsTheCrossingOnTopOfWhatTheNodeItselfDelays)
{
    GraphSpec hot;
    hot.nodes = {input("mic"), pitch("p", 5.0F), output("spk")};
    connect(hot, "mic", "out", "p", "in");
    connect(hot, "p", "out", "spk", "in");

    GraphSpec cold = hot;
    cold.nodes[1].domain = "cold";
    cold.domains["cold"] = {128, 1};

    std::string error;
    auto hot_graph = compileOf(hot, error);
    ASSERT_NE(hot_graph, nullptr) << error;
    auto cold_graph = compileOf(cold, error);
    ASSERT_NE(cold_graph, nullptr) << error;

    EXPECT_GT(hot_graph->latencyFrames(), 0U) << "a pitch shifter delays; the test needs that";
    EXPECT_EQ(cold_graph->latencyFrames(), hot_graph->latencyFrames() + 256U);
}

TEST(ColdDomain, WaitsForAWholeBlock)
{
    std::string error;
    auto graph = compileOf(coldPassthrough(128), error);
    ASSERT_NE(graph, nullptr) << error;

    EXPECT_FALSE(graph->runColdPass(1)) << "nothing has been captured yet";

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();

    graph->process(ctx);
    EXPECT_FALSE(graph->runColdPass(1)) << "half a block is not a block";

    graph->process(ctx);
    EXPECT_TRUE(graph->runColdPass(1));
    EXPECT_FALSE(graph->runColdPass(1)) << "and it does not run twice on one block";
}

TEST(ColdDomain, DelaysTheSignalByExactlyWhatItReports)
{
    std::string error;
    auto graph = compileOf(coldPassthrough(128, 1), error);
    ASSERT_NE(graph, nullptr) << error;
    ASSERT_EQ(graph->latencyFrames(), 256U);

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();

    for (int i = 0; i < 4; ++i) {
        graph->process(ctx);
        EXPECT_EQ(block.output(0), 0.0F) << "block " << i;
        graph->runColdPass(1);
    }

    graph->process(ctx);
    EXPECT_NEAR(block.output(0), 1.0F, 1e-6F);
    EXPECT_NEAR(block.output(0, 63), 1.0F, 1e-6F);
}

TEST(ColdDomain, OneCrossingServesEveryConsumerInTheSameDomain)
{
    GraphSpec spec;
    spec.nodes = {input("mic"),      gain("dry"),      inDomain(gain("a")),
                  inDomain(gain("b")), output("spk")};
    spec.domains["cold"] = {128, 1};
    connect(spec, "mic", "out", "dry", "in");
    connect(spec, "dry", "out", "a", "in");
    connect(spec, "dry", "out", "b", "in");
    connect(spec, "a", "out", "spk", "in");
    connect(spec, "b", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->crossingCount(), 3U);
}

TEST(ColdDomain, LeavesAHotChainInTheSameGraphUndelayed)
{
    GraphSpec spec;
    spec.nodes = {input("mic"),  gain("fast"),          output("spk"),
                  input("mic2"), inDomain(gain("slow")), output("spk2")};
    spec.domains["cold"] = {128, 1};
    connect(spec, "mic", "out", "fast", "in");
    connect(spec, "fast", "out", "spk", "in");
    connect(spec, "mic2", "out", "slow", "in");
    connect(spec, "slow", "out", "spk2", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(2, 2, 64);
    block.fillInput(0, 1.0F);
    block.fillInput(1, 1.0F);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    EXPECT_NEAR(block.output(0), 1.0F, 1e-6F) << "the hot chain is through on the first block";
    EXPECT_EQ(block.output(1), 0.0F) << "the cold one is still filling its prefill";
}

TEST(ColdDomain, KeepsTwoChainsLatenciesApart)
{
    GraphSpec spec;
    spec.nodes = {input("mic"),  gain("fast"),           output("spk"),
                  input("mic2"), inDomain(gain("slow")), output("spk2")};
    spec.domains["cold"] = {128, 1};
    connect(spec, "mic", "out", "fast", "in");
    connect(spec, "fast", "out", "spk", "in");
    connect(spec, "mic2", "out", "slow", "in");
    connect(spec, "slow", "out", "spk2", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->latencyFrames(), 256U) << "the headline is still the worst of them";

    const std::vector<avc::graph::OutputLatency> outputs = graph->latencyByOutput();
    ASSERT_EQ(outputs.size(), 2U);
    std::map<std::string, std::uint32_t> by_node;
    for (const avc::graph::OutputLatency &output : outputs) {
        by_node[output.node] = output.frames;
    }
    EXPECT_EQ(by_node["spk"], 0U) << "the hot chain waits for nothing";
    EXPECT_EQ(by_node["spk2"], 256U);
}

TEST(ColdDomain, RunsIndependentNamedDomainsConcurrently)
{
    ASSERT_TRUE(g_parallel_probe_registered);
    g_probe_active.store(0, std::memory_order_relaxed);
    g_probe_max_active.store(0, std::memory_order_relaxed);
    g_probe_arrivals.store(0, std::memory_order_relaxed);

    GraphSpec spec;
    spec.nodes = {input("a", "mic_a"),
                  inDomain({.id = "probe_a", .type = "test_parallel_probe"}, "left"),
                  output("out_a", "spk_a"), input("b", "mic_b"),
                  inDomain({.id = "probe_b", .type = "test_parallel_probe"}, "right"),
                  output("out_b", "spk_b")};
    spec.domains["left"] = {64, 1};
    spec.domains["right"] = {64, 1};
    connect(spec, "a", "out", "probe_a", "in");
    connect(spec, "probe_a", "out", "out_a", "in");
    connect(spec, "b", "out", "probe_b", "in");
    connect(spec, "probe_b", "out", "out_b", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    graph->startWorkers();

    Block block(2, 2, 64);
    block.fillInput(0, 0.25F);
    block.fillInput(1, 0.75F);
    ProcessContext ctx = block.context();
    graph->process(ctx);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_probe_max_active.load(std::memory_order_acquire) < 2
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    graph->stopWorkers();

    EXPECT_EQ(g_probe_max_active.load(std::memory_order_relaxed), 2)
        << "independent cold domains did not overlap";
    const auto domains = graph->domains();
    ASSERT_EQ(domains.size(), 3U);
    EXPECT_GT(domains[1].passes, 0U);
    EXPECT_GT(domains[2].passes, 0U);
}

TEST(ColdDomain, KeepsAGapOnOnePathOutOfAnotherColdPath)
{
    GraphSpec spec;
    spec.nodes = {input("a", "mic_a"), inDomain(gain("late"), "late_domain"),
                  gain("late_hot"), output("out_a", "spk_a"),
                  input("b", "mic_b"), gain("fresh_hot"),
                  inDomain(gain("fresh"), "fresh_domain"), output("out_b", "spk_b")};
    spec.domains["late_domain"] = {64, 0};
    spec.domains["fresh_domain"] = {64, 0};
    connect(spec, "a", "out", "late", "in");
    connect(spec, "late", "out", "late_hot", "in");
    connect(spec, "late_hot", "out", "out_a", "in");
    connect(spec, "b", "out", "fresh_hot", "in");
    connect(spec, "fresh_hot", "out", "fresh", "in");
    connect(spec, "fresh", "out", "out_b", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    ASSERT_EQ(graph->domainCount(), 3U);

    Block block(2, 2, 64);
    ProcessContext ctx = block.context();
    for (int cycle = 1; cycle <= 3; ++cycle) {
        block.fillInput(0, 9.0F);
        block.fillInput(1, static_cast<float>(cycle));
        graph->process(ctx);
        ASSERT_TRUE(graph->runColdPass(2));
    }

    EXPECT_FLOAT_EQ(block.output(0), 0.0F);
    EXPECT_FLOAT_EQ(block.output(1), 2.0F)
        << "the unrelated missing path tainted the fresh path";
}

TEST(ColdDomain, CountsAnUnderrunRatherThanBlockingTheAudioThread)
{
    std::string error;
    auto graph = compileOf(coldPassthrough(128, 1), error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();

    for (int i = 0; i < 8; ++i) {
        graph->process(ctx);
        EXPECT_EQ(block.output(0), 0.0F) << "block " << i;
    }
    EXPECT_GT(graph->domains()[1].underruns, 0U);
    EXPECT_EQ(graph->domains()[1].passes, 0U);
}

TEST(ColdDomain, DropsExpiredWorkInsteadOfReplayingItAfterAnUnderrun)
{
    std::string error;
    auto graph = compileOf(coldPassthrough(128, 1), error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 1, 64);
    ProcessContext ctx = block.context();
    for (int hot = 0; hot < 6; ++hot) {
        block.fillInput(0, static_cast<float>(hot / 2 + 1));
        graph->process(ctx);
        EXPECT_EQ(block.output(0), 0.0F) << "hot block " << hot;
    }

    ASSERT_TRUE(graph->runColdPass(1));
    block.fillInput(0, 4.0F);
    graph->process(ctx);
    EXPECT_FLOAT_EQ(block.output(0), 2.0F);
    EXPECT_FLOAT_EQ(block.output(0, 63), 2.0F);
}

TEST(ColdDomain, KeepsTheStillUsefulTailOfAPartlyExpiredBlock)
{
    std::string error;
    auto graph = compileOf(coldPassthrough(100, 1), error);
    ASSERT_NE(graph, nullptr) << error;
    ASSERT_EQ(graph->latencyFrames(), 200U);

    Block block(1, 1, 64);
    ProcessContext ctx = block.context();
    for (int cycle = 1; cycle <= 4; ++cycle) {
        block.fillInput(0, static_cast<float>(cycle));
        graph->process(ctx);
    }

    ASSERT_TRUE(graph->runColdPass(1));
    ASSERT_TRUE(graph->runColdPass(1));
    block.fillInput(0, 5.0F);
    graph->process(ctx);

    EXPECT_FLOAT_EQ(block.output(0), 1.0F);
    EXPECT_FLOAT_EQ(block.output(0, 8), 2.0F);
    EXPECT_FLOAT_EQ(block.output(0, 63), 2.0F);
}

TEST(ColdDomain, ComposesSerialDomainsWithDifferentBlockSizes)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), inDomain(gain("a"), "first"),
                  inDomain(gain("b"), "second"), output("spk")};
    spec.domains["first"] = {100, 0};
    spec.domains["second"] = {150, 0};
    connect(spec, "mic", "out", "a", "in");
    connect(spec, "a", "out", "b", "in");
    connect(spec, "b", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    ASSERT_EQ(graph->latencyFrames(), 250U);

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();
    for (int cycle = 0; cycle < 4; ++cycle) {
        graph->process(ctx);
        while (graph->runColdPass(1)) {
        }
        while (graph->runColdPass(2)) {
        }
    }

    EXPECT_FLOAT_EQ(block.output(0), 0.0F);
    EXPECT_FLOAT_EQ(block.output(0, 57), 0.0F);
    EXPECT_FLOAT_EQ(block.output(0, 58), 1.0F);
    EXPECT_FLOAT_EQ(block.output(0, 63), 1.0F);
}

TEST(ColdDomain, PropagatesTheFreshIntervalAcrossMultipleColdDomains)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), inDomain(gain("a"), "first"),
                  inDomain(gain("b"), "second"), output("spk")};
    spec.domains["first"] = {128, 1};
    spec.domains["second"] = {128, 1};
    connect(spec, "mic", "out", "a", "in");
    connect(spec, "a", "out", "b", "in");
    connect(spec, "b", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    ASSERT_EQ(graph->domainCount(), 3U);

    Block block(1, 1, 64);
    ProcessContext ctx = block.context();
    for (int hot = 0; hot < 10; ++hot) {
        block.fillInput(0, static_cast<float>(hot / 2 + 1));
        graph->process(ctx);
    }

    EXPECT_FALSE(graph->runColdPass(2));
    ASSERT_TRUE(graph->runColdPass(1));
    ASSERT_TRUE(graph->runColdPass(2));

    block.fillInput(0, 6.0F);
    graph->process(ctx);
    EXPECT_FLOAT_EQ(block.output(0), 2.0F)
        << "the expired marker 1 must not be replayed through either domain";
}

TEST(ColdDomain, TheWorkerThreadCarriesTheSignalOnItsOwn)
{
    std::string error;
    auto graph = compileOf(coldPassthrough(128, 1), error);
    ASSERT_NE(graph, nullptr) << error;
    graph->startWorkers();

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();

    bool arrived = false;
    for (int i = 0; i < 2000 && !arrived; ++i) {
        graph->process(ctx);
        arrived = block.output(0) > 0.5F;
        std::this_thread::sleep_for(std::chrono::microseconds(1400));
    }
    graph->stopWorkers();

    EXPECT_TRUE(arrived) << "the cold domain never delivered a block";
    EXPECT_GT(graph->domains()[1].passes, 0U);
}

TEST(ColdDomain, AppliesAParameterOnTheThreadThatRunsTheNode)
{
    FakeBinder binder;
    CompileEnv env;
    env.max_quantum = 64;
    GraphHost host(env, &binder);

    std::string error;
    ASSERT_TRUE(host.apply(coldPassthrough(128, 1, 0.0F), error)) << error;
    ASSERT_TRUE(host.setParam("g", "gain_db", -20.0F));

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();

    const float expected = std::pow(10.0F, -20.0F / 20.0F);
    bool settled = false;
    for (int i = 0; i < 4000 && !settled; ++i) {
        host.process(ctx);
        settled = std::fabs(block.output(0) - expected) < 1e-3F;
        std::this_thread::sleep_for(std::chrono::microseconds(1400));
    }
    EXPECT_TRUE(settled) << "the knob never reached the cold node; last was "
                         << block.output(0);
    EXPECT_EQ(host.stats().dropped_params, 0U);

    host.shutdown();
}

TEST(ColdDomain, ParksAWorkerThatIsWiredToNothing)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), gain("g"), output("spk"), inDomain(gain("orphan"))};
    spec.domains["cold"] = {128, 1};
    connect(spec, "mic", "out", "g", "in");
    connect(spec, "g", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->crossingCount(), 0U) << "nothing crosses, so there is nothing to cross";

    graph->startWorkers();
    graph->stopWorkers();
    EXPECT_EQ(graph->domains()[1].passes, 0U);
}

TEST(GraphHost, SwapsBetweenAHotAndAColdVersionOfTheSameChain)
{
    FakeBinder binder;
    CompileEnv env;
    env.max_quantum = 64;
    GraphHost host(env, &binder);

    std::string error;
    ASSERT_TRUE(host.apply(passthrough(-6.0F), error)) << error;
    EXPECT_EQ(host.stats().domains.size(), 1U);
    EXPECT_EQ(host.stats().latency_frames, 0U);

    ASSERT_TRUE(host.apply(coldPassthrough(128, 1, -6.0F), error)) << error;
    ASSERT_EQ(host.stats().domains.size(), 2U);
    EXPECT_TRUE(host.stats().domains[1].cold);
    EXPECT_EQ(host.stats().latency_frames, 256U);

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    ProcessContext ctx = block.context();
    for (int i = 0; i < 40; ++i) {
        host.process(ctx);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    EXPECT_NEAR(block.output(0), std::pow(10.0F, -6.0F / 20.0F), 1e-4F);

    ASSERT_TRUE(host.apply(passthrough(-6.0F), error)) << error;
    EXPECT_EQ(host.stats().domains.size(), 1U);
    host.process(ctx);
    EXPECT_NEAR(block.output(0), std::pow(10.0F, -6.0F / 20.0F), 1e-4F);

    host.shutdown();
}

TEST(GraphSpec, RoundTripsADomainThroughJson)
{
    GraphSpec spec = coldPassthrough(2048, 2);
    std::string error;
    const std::optional<GraphSpec> back = GraphSpec::parse(spec.dump(), error);
    ASSERT_TRUE(back.has_value()) << error;

    EXPECT_EQ(back->nodes[0].domain, "");
    EXPECT_EQ(back->nodes[1].domain, "cold");
    ASSERT_EQ(back->domains.count("cold"), 1U);
    EXPECT_EQ(back->domains.at("cold").block, 2048U);
    EXPECT_EQ(back->domains.at("cold").safety, 2U);
}

std::string_view readText(const void *block)
{
    if (block == nullptr) {
        return {};
    }
    std::uint32_t length = 0;
    std::memcpy(&length, block, sizeof(length));
    return {static_cast<const char *>(block) + sizeof(length), length};
}

void writeText(void *block, std::string_view text)
{
    const auto length = static_cast<std::uint32_t>(text.size());
    std::memcpy(block, &length, sizeof(length));
    std::memcpy(static_cast<char *>(block) + sizeof(length), text.data(), length);
}

class TextRuler final : public avc::node::Node {
public:
    void prepare(const avc::node::PrepareInfo &) override {}
    void setParam(std::uint32_t, float) noexcept override {}

    void process(const avc::node::NodeContext &ctx) noexcept override
    {
        const float level = ctx.inputs[0] != nullptr ? ctx.inputs[0][0] : 0.0F;
        const auto count = static_cast<std::size_t>(std::clamp(level, 0.0F, 32.0F));
        writeText(ctx.out_blocks[0], std::string(count, 'x'));
    }
};

class TextLength final : public avc::node::Node {
public:
    void prepare(const avc::node::PrepareInfo &) override {}
    void setParam(std::uint32_t, float) noexcept override {}

    void process(const avc::node::NodeContext &ctx) noexcept override
    {
        const auto value = static_cast<Sample>(readText(ctx.in_blocks[0]).size());
        std::fill(ctx.outputs[0], ctx.outputs[0] + ctx.nframes, value);
    }
};

class TextEcho final : public avc::node::Node {
public:
    void prepare(const avc::node::PrepareInfo &) override {}
    void setParam(std::uint32_t, float) noexcept override {}

    void process(const avc::node::NodeContext &ctx) noexcept override
    {
        writeText(ctx.out_blocks[0], readText(ctx.in_blocks[0]));
    }
};

bool registerTextNodes()
{
    using avc::node::NodeDescriptor;
    using avc::node::NodeRegistry;

    NodeDescriptor ruler;
    ruler.type = "test_text_ruler";
    ruler.category = "test";
    ruler.label = "Ruler";
    ruler.inputs = {{"in", ""}};
    ruler.outputs = {{"out", "text"}};
    NodeRegistry::instance().add(ruler, [] { return std::make_unique<TextRuler>(); });

    NodeDescriptor length;
    length.type = "test_text_length";
    length.category = "test";
    length.label = "Length";
    length.inputs = {{"in", "text"}};
    length.outputs = {{"out", ""}};
    NodeRegistry::instance().add(length, [] { return std::make_unique<TextLength>(); });

    NodeDescriptor echo;
    echo.type = "test_text_echo";
    echo.category = "test";
    echo.label = "Echo";
    echo.inputs = {{"in", "text"}};
    echo.outputs = {{"out", "text"}};
    NodeRegistry::instance().add(echo, [] { return std::make_unique<TextEcho>(); });

    NodeDescriptor bad;
    bad.type = "test_bad_port";
    bad.category = "test";
    bad.label = "Bad";
    bad.inputs = {{"in", ""}};
    bad.outputs = {{"out", "nonsense"}};
    NodeRegistry::instance().add(bad, [] { return std::make_unique<TextEcho>(); });
    return true;
}

const bool g_text_nodes = registerTextNodes();

SpecNode node(const std::string &id, const std::string &type)
{
    return {.id = id, .type = type};
}

GraphSpec textChain()
{
    GraphSpec spec;
    spec.nodes = {input("mic"), node("ruler", "test_text_ruler"),
                  node("length", "test_text_length"), output("spk")};
    connect(spec, "mic", "out", "ruler", "in");
    connect(spec, "ruler", "out", "length", "in");
    connect(spec, "length", "out", "spk", "in");
    return spec;
}

TEST(PortTypes, RefusesAWireBetweenTwoTypes)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), node("ruler", "test_text_ruler"), gain("g"), output("spk")};
    connect(spec, "mic", "out", "ruler", "in");
    connect(spec, "ruler", "out", "g", "in");
    connect(spec, "g", "out", "spk", "in");

    std::string error;
    EXPECT_EQ(compileOf(spec, error), nullptr);
    EXPECT_NE(error.find("text"), std::string::npos) << error;
    EXPECT_NE(error.find("audio"), std::string::npos) << error;
}

TEST(PortTypes, RefusesAPortCarryingSomethingNothingDeclares)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), node("bad", "test_bad_port")};
    connect(spec, "mic", "out", "bad", "in");

    std::string error;
    EXPECT_EQ(compileOf(spec, error), nullptr);
    EXPECT_NE(error.find("nonsense"), std::string::npos) << error;
}

TEST(PortTypes, RefusesMultipleEdgesOnAValueInput)
{
    GraphSpec spec;
    spec.nodes = {input("a", "a"), input("b", "b"),
                  node("ruler_a", "test_text_ruler"),
                  node("ruler_b", "test_text_ruler"),
                  node("length", "test_text_length"), output("spk")};
    connect(spec, "a", "out", "ruler_a", "in");
    connect(spec, "b", "out", "ruler_b", "in");
    connect(spec, "ruler_a", "out", "length", "in");
    connect(spec, "ruler_b", "out", "length", "in");
    connect(spec, "length", "out", "spk", "in");

    std::string error;
    EXPECT_EQ(compileOf(spec, error), nullptr);
    EXPECT_NE(error.find("only audio inputs"), std::string::npos) << error;
}

TEST(PortTypes, CarriesTextFromOneNodeToTheNext)
{
    std::string error;
    auto graph = compileOf(textChain(), error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 1, 64);
    block.fillInput(0, 3.0F);
    graph->process(block.context());
    EXPECT_FLOAT_EQ(block.output(0), 3.0F) << "three characters of text made the round trip";

    block.fillInput(0, 7.0F);
    graph->process(block.context());
    EXPECT_FLOAT_EQ(block.output(0), 7.0F);
}

TEST(TextNode, ExposesOnlyChangedSnapshotsThroughTheGraphHost)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), node("ruler", "test_text_ruler"),
                  node("caption", "text")};
    connect(spec, "mic", "out", "ruler", "in");
    connect(spec, "ruler", "out", "caption", "in");

    FakeBinder binder;
    GraphHost host(CompileEnv{}, &binder);
    std::string error;
    ASSERT_TRUE(host.apply(spec, error)) << error;

    Block block(1, 0, 64);
    block.fillInput(0, 2.0F);
    host.process(block.context());

    auto readings = host.texts();
    ASSERT_EQ(readings.size(), 1U);
    EXPECT_EQ(readings[0].node, "caption");
    EXPECT_EQ(readings[0].text, "xx");

    host.process(block.context());
    EXPECT_TRUE(host.texts().empty()) << "unchanged snapshots do not bloat telemetry";

    block.fillInput(0, 9.0F);
    host.process(block.context());
    readings = host.texts();
    ASSERT_EQ(readings.size(), 1U);
    EXPECT_EQ(readings[0].text, "xxxxxxxxx");
    host.shutdown();
}

TEST(PortTypes, KeepsTextAndAudioOutOfEachOthersBuffers)
{
    GraphSpec spec;
    spec.nodes = {input("mic"),
                  splitter("split", 2),
                  gain("g", 0.0F),
                  node("ruler", "test_text_ruler"),
                  node("length", "test_text_length"),
                  output("audio_out"),
                  output("text_out")};
    connect(spec, "mic", "out", "split", "in");
    connect(spec, "split", "out_1", "g", "in");
    connect(spec, "g", "out", "audio_out", "in");
    connect(spec, "split", "out_2", "ruler", "in");
    connect(spec, "ruler", "out", "length", "in");
    connect(spec, "length", "out", "text_out", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 2, 64);
    block.fillInput(0, 5.0F);
    graph->process(block.context());
    EXPECT_FLOAT_EQ(block.output(0), 5.0F) << "the audio branch";
    EXPECT_FLOAT_EQ(block.output(1), 5.0F) << "the text branch";
}

TEST(PortTypes, CarriesTextAcrossADomainBoundary)
{
    GraphSpec spec = textChain();
    spec.nodes[1].domain = "cold";
    spec.domains["cold"] = {64, 1};

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    ASSERT_EQ(graph->crossingCount(), 2U) << "audio in, text out";

    Block block(1, 1, 64);
    block.fillInput(0, 4.0F);
    graph->process(block.context());
    ASSERT_TRUE(graph->runColdPass(1)) << "a whole block of audio arrived";

    graph->process(block.context());
    EXPECT_FLOAT_EQ(block.output(0), 4.0F) << "the text the cold pass published";
}

TEST(PortTypes, PacesADomainThatHasNothingButValuesOnIt)
{
    GraphSpec spec;
    spec.nodes = {input("mic"), node("ruler", "test_text_ruler"),
                  node("echo", "test_text_echo"), node("length", "test_text_length"),
                  output("spk")};
    spec.nodes[2].domain = "words";
    spec.domains["words"] = {64, 1};
    connect(spec, "mic", "out", "ruler", "in");
    connect(spec, "ruler", "out", "echo", "in");
    connect(spec, "echo", "out", "length", "in");
    connect(spec, "length", "out", "spk", "in");

    std::string error;
    auto graph = compileOf(spec, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_EQ(graph->latencyFrames(), 0U) << "a text hop delays no audio";

    Block block(1, 1, 64);
    block.fillInput(0, 6.0F);
    graph->process(block.context());

    EXPECT_TRUE(graph->runColdPass(1));
    EXPECT_FALSE(graph->runColdPass(1)) << "one pass per block period, and nothing to wait on";

    graph->process(block.context());
    EXPECT_FLOAT_EQ(block.output(0), 6.0F);
}

TEST(GraphSpec, RoundTripsThroughJson)
{
    GraphSpec original = passthrough(-3.0F);
    original.edges[0].gain_db = -4.5F;
    original.nodes[1].ui_x = 120.0F;
    original.nodes[1].ui_y = 80.0F;
    original.nodes[1].ui_width = 320.0F;
    original.nodes[1].ui_height = 180.0F;
    std::string error;
    const auto parsed = GraphSpec::parse(original.dump(), error);
    ASSERT_TRUE(parsed.has_value()) << error;

    ASSERT_EQ(parsed->nodes.size(), original.nodes.size());
    EXPECT_EQ(parsed->nodes[1].type, "gain");
    EXPECT_NEAR(parsed->nodes[1].params.at("gain_db"), -3.0F, 1e-5F);
    EXPECT_FLOAT_EQ(parsed->nodes[1].ui_x, 120.0F);
    EXPECT_FLOAT_EQ(parsed->nodes[1].ui_y, 80.0F);
    EXPECT_FLOAT_EQ(parsed->nodes[1].ui_width, 320.0F);
    EXPECT_FLOAT_EQ(parsed->nodes[1].ui_height, 180.0F);
    ASSERT_EQ(parsed->edges.size(), original.edges.size());
    EXPECT_FLOAT_EQ(parsed->edges[0].gain_db, -4.5F);

    auto graph = GraphCompiler::compile(*parsed, envFor(*parsed), nullptr, error);
    EXPECT_NE(graph, nullptr) << error;
}

TEST(GraphSpec, PreservesExplicitMixerNodesAndRouteGains)
{
    const std::string graph_json = R"({
        "version": 1,
        "nodes": [
            {"id":"a","type":"capture","outputs":1,"params":{"source":"a"}},
            {"id":"b","type":"capture","outputs":1,"params":{"source":"b"}},
            {"id":"mix","type":"mixer","inputs":2,"params":{}},
            {"id":"spk","type":"playback","inputs":1,"params":{"device":"spk"}}
        ],
        "edges": [
            {"from":{"node":"a","port":"out_1"},"to":{"node":"mix","port":"in_1"},"gain_db":-3},
            {"from":{"node":"b","port":"out_1"},"to":{"node":"mix","port":"in_2"}},
            {"from":{"node":"mix","port":"out"},"to":{"node":"spk","port":"in_1"},"gain_db":2}
        ]
    })";

    std::string error;
    const auto parsed = GraphSpec::parse(graph_json, error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->nodes.size(), 4U);
    ASSERT_EQ(parsed->edges.size(), 3U);
    EXPECT_EQ(parsed->edges[0].from.node, "a");
    EXPECT_EQ(parsed->edges[0].to.node, "mix");
    EXPECT_FLOAT_EQ(parsed->edges[0].gain_db, -3.0F);
    EXPECT_EQ(parsed->edges[1].from.node, "b");
    EXPECT_FLOAT_EQ(parsed->edges[1].gain_db, 0.0F);
    EXPECT_EQ(parsed->edges[2].from.node, "mix");
    EXPECT_EQ(parsed->edges[2].to.node, "spk");
    EXPECT_FLOAT_EQ(parsed->edges[2].gain_db, 2.0F);

    auto graph = compileOf(*parsed, error);
    ASSERT_NE(graph, nullptr) << error;
    Block block(2, 1, 64);
    block.fillInput(0, 2.0F);
    block.fillInput(1, 4.0F);
    graph->process(block.context());
    const float expected = (2.0F * std::pow(10.0F, -3.0F / 20.0F) + 4.0F)
                           * std::pow(10.0F, 2.0F / 20.0F);
    EXPECT_NEAR(block.output(0), expected, 1e-4F);
}

TEST(GraphSpec, ReportsMalformedJson)
{
    std::string error;
    EXPECT_FALSE(GraphSpec::parse("{ not json", error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(GraphSpec, RejectsWrongFieldTypesWithoutThrowing)
{
    for (const std::string text : {
             R"({"version":"one","nodes":[]})",
             R"({"nodes":[{"id":12,"type":"gain"}]})",
             R"({"nodes":[{"id":"g","type":"gain","params":[]}]})",
             R"({"nodes":[{"id":"g","type":"gain","ui":{"width":0}}]})",
             R"({"nodes":[{"id":"g","type":"gain","ui":{"height":"large"}}]})",
             R"({"nodes":[],"edges":[{"from":{"node":"a","port":{}},"to":{}}]})",
             R"({"nodes":[],"edges":[{"from":{"node":"a","port":0},"to":{"node":"b","port":0},"gain_db":"loud"}]})",
             R"({"nodes":[],"domains":{"cold":{"block":-1}}})",
         }) {
        std::string error;
        EXPECT_NO_THROW({ EXPECT_FALSE(GraphSpec::parse(text, error).has_value()) << text; });
        EXPECT_FALSE(error.empty()) << text;
    }
}

TEST(CompiledGraph, SilencesOutputBeyondItsCompiledQuantum)
{
    const GraphSpec spec = passthrough();
    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec, 32), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    Block block(1, 1, 64);
    block.fillInput(0, 1.0F);
    graph->process(block.context());
    EXPECT_NEAR(block.output(0, 31), 1.0F, 1e-5F);
    EXPECT_FLOAT_EQ(block.output(0, 32), 0.0F);
    EXPECT_FLOAT_EQ(block.output(0, 63), 0.0F);
}

}
