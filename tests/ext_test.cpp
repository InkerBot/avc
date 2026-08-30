#include "control/ExtensionStore.hpp"
#include "ext/ExtensionLoader.hpp"
#include "graph/GraphCompiler.hpp"
#include "graph/GraphSpec.hpp"
#include "node/NodeRegistry.hpp"
#include "node/PortTypeManifest.hpp"

#include <avc/avc_plugin.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <string>
#include <vector>

namespace {

#ifdef _WIN32
constexpr const char *kExtSuffix = ".dll";
void setEnvironment(const char *key, const std::string &value) { _putenv_s(key, value.c_str()); }
void unsetEnvironment(const char *key) { _putenv_s(key, ""); }
std::uint32_t processId() { return GetCurrentProcessId(); }
#else
constexpr const char *kExtSuffix = ".so";
void setEnvironment(const char *key, const std::string &value) { ::setenv(key, value.c_str(), 1); }
void unsetEnvironment(const char *key) { ::unsetenv(key); }
std::uint32_t processId() { return static_cast<std::uint32_t>(::getpid()); }
#endif

std::string extName(std::string stem) { return stem + kExtSuffix; }

using avc::ext::ExtensionLoader;
using avc::ext::ExtensionRequest;
using avc::ext::LoadedExtension;
using avc::graph::CompiledGraph;
using avc::graph::CompileEnv;
using avc::graph::GraphCompiler;
using avc::graph::GraphSpec;
using avc::graph::SpecNode;
using avc::node::NodeRegistry;
using avc::node::PortTransport;
using avc::node::PortTypeManifest;
using avc::types::Sample;

LoadedExtension loadWith(ExtensionLoader &loader, const char *mode,
                         std::map<std::string, std::string> settings = {})
{
    if (mode != nullptr) {
        setEnvironment("AVC_TEST_PLUGIN_MODE", mode);
    } else {
        unsetEnvironment("AVC_TEST_PLUGIN_MODE");
    }

    ExtensionRequest request;
    request.path = AVC_TEST_PLUGIN_PATH;
    request.settings = std::move(settings);
    loader.loadAll({request}, std::filesystem::temp_directory_path() / "avc-ext-test");

    EXPECT_EQ(loader.results().size(), 1U);
    return loader.results().front();
}

std::vector<Sample> runBlock(CompiledGraph &graph, std::uint32_t nframes, Sample value,
                             std::uint32_t n_in = 1, std::uint32_t n_out = 1)
{
    std::vector<std::vector<Sample>> in(n_in, std::vector<Sample>(nframes, value));
    std::vector<std::vector<Sample>> out(n_out, std::vector<Sample>(nframes, -999.0F));
    std::vector<Sample *> in_ptrs;
    std::vector<Sample *> out_ptrs;
    for (auto &buffer : in) in_ptrs.push_back(buffer.data());
    for (auto &buffer : out) out_ptrs.push_back(buffer.data());

    const avc::audio::ProcessContext ctx{in_ptrs.data(), out_ptrs.data(), n_in, n_out, nframes, 0};
    graph.process(ctx);
    return out.front();
}

GraphSpec chainThrough(const std::string &type, const std::string &domain = {})
{
    GraphSpec spec;
    spec.version = 1;
    spec.nodes.push_back(
        {.id = "mic", .type = "capture", .options = {{"source", "m"}}, .outputs = 1});
    spec.nodes.push_back({.id = "p", .type = type, .domain = domain});
    spec.nodes.push_back(
        {.id = "out", .type = "playback", .options = {{"device", "s"}}, .inputs = 1});
    spec.edges.push_back({{"mic", std::string("out_1")}, {"p", std::string("in")}});
    spec.edges.push_back({{"p", std::string("out")}, {"out", std::string("in_1")}});
    return spec;
}

CompileEnv envFor(const GraphSpec &spec, std::uint32_t quantum = 64)
{
    CompileEnv env;
    env.max_quantum = quantum;
    std::vector<avc::audio::IoRequest> requests;
    std::string error;
    GraphCompiler::ioRequests(spec, requests, error);

    std::uint32_t in_slot = 0;
    std::uint32_t out_slot = 0;
    for (const avc::audio::IoRequest &request : requests) {
        std::uint32_t &next = avc::audio::producesSignal(request.kind) ? in_slot : out_slot;
        for (std::uint32_t c = 0; c < request.channels; ++c) {
            env.io_slots[request.node].push_back(next++);
        }
    }
    return env;
}

TEST(Extension, RegistersItsNodeTypesUnderItsOwnName)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, nullptr);

    ASSERT_TRUE(result.loaded) << result.error;
    EXPECT_EQ(result.id, "testext");
    EXPECT_EQ(result.name, "Test extension");
    EXPECT_EQ(result.abi_version, AVC_ABI_VERSION);

    const auto *desc = NodeRegistry::instance().find("testext.accumulate");
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->extension, "testext");
    EXPECT_EQ(desc->category, "test");
    EXPECT_TRUE(desc->realtime_safe);
    ASSERT_EQ(desc->params.size(), 1U);
    EXPECT_EQ(desc->params[0].name, "offset");

    EXPECT_EQ(NodeRegistry::instance().find("accumulate"), nullptr);
    EXPECT_NE(NodeRegistry::instance().create("testext.accumulate"), nullptr);
}

TEST(Extension, ReportsItsSettingsAndItsGraphs)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, nullptr);

    ASSERT_TRUE(result.loaded) << result.error;
    ASSERT_EQ(result.settings.size(), 1U);
    EXPECT_EQ(result.settings[0].key, "scale");
    EXPECT_EQ(result.settings[0].type, "float");
    EXPECT_EQ(result.settings[0].label, "Scale");
    ASSERT_EQ(result.presets.size(), 1U);
    EXPECT_EQ(result.presets[0].name, "Empty");
    EXPECT_EQ(result.ui_entry, "ui/main.js");
    ASSERT_EQ(result.ui_assets.size(), 1U);
    EXPECT_EQ(result.ui_assets[0].mime_type, "text/javascript");
}

TEST(Extension, DispatchesEditorMessagesThroughTheHost)
{
    nlohmann::json reply;
    ExtensionLoader loader;
    loader.setUiSink([&](const nlohmann::json &message) { reply = message; });
    ASSERT_TRUE(loadWith(loader, nullptr).loaded);

    std::string error;
    ASSERT_TRUE(loader.dispatchUiRequest("testext", 42, "echo", R"({"answer":42})", error))
        << error;
    EXPECT_EQ(reply.value("t", std::string{}), "extension.ui.reply");
    EXPECT_EQ(reply.value("id", 0U), 42U);
    EXPECT_TRUE(reply.value("ok", false));
    EXPECT_EQ(reply["data"]["answer"], 42);
}

TEST(Extension, RefusesAnEditorAssetThatEscapesItsNamespace)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, "bad_ui_path");

    EXPECT_FALSE(result.loaded);
    EXPECT_NE(result.error.find("invalid editor asset"), std::string::npos) << result.error;
    EXPECT_EQ(NodeRegistry::instance().find("baduiext.thing"), nullptr);
}

TEST(Extension, PassesPathOptionsBeforePrepareAndReportsStatus)
{
    ExtensionLoader loader;
    ASSERT_TRUE(loadWith(loader, nullptr).loaded);

    const auto *desc = NodeRegistry::instance().find("testext.configurable");
    ASSERT_NE(desc, nullptr);
    ASSERT_EQ(desc->params.size(), 1U);
    EXPECT_EQ(desc->params[0].type, avc::node::ParamType::Path);
    EXPECT_EQ(desc->params[0].default_text, "/default/model.avcrvc");

    GraphSpec invalid = chainThrough("testext.configurable");
    std::string invalid_error;
    EXPECT_EQ(GraphCompiler::compile(invalid, envFor(invalid), nullptr, invalid_error), nullptr);
    EXPECT_NE(invalid_error.find("model package is not valid"), std::string::npos)
        << invalid_error;

    GraphSpec valid = chainThrough("testext.configurable");
    valid.nodes[1].options["model_path"] = "/valid/model.avcrvc";
    std::string error;
    auto graph = GraphCompiler::compile(valid, envFor(valid), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;
    avc::node::NodeStatus status;
    ASSERT_TRUE(graph->nodeAt(0)->status(status));
    EXPECT_EQ(status.state, avc::node::NodeState::Ready);
    EXPECT_EQ(status.message, "/valid/model.avcrvc");
}

TEST(Extension, ItsNodesRunInACompiledGraph)
{
    ExtensionLoader loader;
    ASSERT_TRUE(loadWith(loader, nullptr).loaded);

    const GraphSpec spec = chainThrough("testext.accumulate");
    std::string error;
    const auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    EXPECT_FLOAT_EQ(runBlock(*graph, 64, 2.0F)[0], 2.0F);
    EXPECT_FLOAT_EQ(runBlock(*graph, 64, 2.0F)[0], 4.0F);
    EXPECT_FLOAT_EQ(runBlock(*graph, 64, 0.5F)[0], 4.5F);
}

TEST(Extension, ASettingReachesItBeforeAnyNodeExists)
{
    ExtensionLoader loader;
    ASSERT_TRUE(loadWith(loader, nullptr, {{"scale", "3"}}).loaded);

    const GraphSpec spec = chainThrough("testext.accumulate");
    std::string error;
    const auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;

    EXPECT_FLOAT_EQ(runBlock(*graph, 64, 2.0F)[0], 6.0F);
}

TEST(Extension, CarriesNodeStateAcrossAGraphSwap)
{
    ExtensionLoader loader;
    ASSERT_TRUE(loadWith(loader, nullptr).loaded);

    const GraphSpec spec = chainThrough("testext.accumulate");
    std::string error;
    const auto first = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(first, nullptr) << error;
    runBlock(*first, 64, 2.0F);
    runBlock(*first, 64, 2.0F);

    const auto second = GraphCompiler::compile(spec, envFor(spec), first.get(), error);
    ASSERT_NE(second, nullptr) << error;
    EXPECT_FLOAT_EQ(runBlock(*second, 64, 1.0F)[0], 5.0F);
}

TEST(Extension, ANodeThatIsNotRealtimeSafeIsRefusedOnTheHotPath)
{
    ExtensionLoader loader;
    ASSERT_TRUE(loadWith(loader, nullptr).loaded);

    const auto *desc = NodeRegistry::instance().find("testext.heavy");
    ASSERT_NE(desc, nullptr);
    EXPECT_FALSE(desc->realtime_safe);
    EXPECT_EQ(desc->recommended_cold_block, 4096U);

    const GraphSpec hot = chainThrough("testext.heavy");
    std::string error;
    EXPECT_EQ(GraphCompiler::compile(hot, envFor(hot), nullptr, error), nullptr);
    EXPECT_NE(error.find("not realtime safe"), std::string::npos) << error;

    GraphSpec cold = chainThrough("testext.heavy", "cold");
    cold.domains["cold"] = {24000, 1};
    std::string cold_error;
    const std::unique_ptr<CompiledGraph> compiled =
        GraphCompiler::compile(cold, envFor(cold), nullptr, cold_error);
    ASSERT_NE(compiled, nullptr) << cold_error;
    ASSERT_EQ(compiled->domains().size(), 2U);
    EXPECT_EQ(compiled->domains()[1].block, 24000U);
}

TEST(Extension, DeclaresPortTypesOfItsOwn)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, nullptr);
    ASSERT_TRUE(result.loaded) << result.error;

    const auto *marker = PortTypeManifest::instance().find("marker");
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->extension, "testext");
    EXPECT_EQ(marker->transport, PortTransport::Value);
    EXPECT_EQ(marker->bytes, sizeof(float));

    const auto *mark = NodeRegistry::instance().find("testext.mark");
    ASSERT_NE(mark, nullptr);
    ASSERT_EQ(mark->outputs.size(), 1U);
    EXPECT_EQ(mark->outputs[0].type, "marker");
    EXPECT_TRUE(mark->inputs[0].type.empty()) << "saying nothing means audio";
}

TEST(Extension, CarriesItsOwnPortTypeBetweenTwoOfItsNodes)
{
    ExtensionLoader loader;
    ASSERT_TRUE(loadWith(loader, nullptr).loaded);

    GraphSpec spec;
    spec.version = 1;
    spec.nodes.push_back(
        {.id = "mic", .type = "capture", .options = {{"source", "m"}}, .outputs = 1});
    spec.nodes.push_back({.id = "mark", .type = "testext.mark"});
    spec.nodes.push_back({.id = "unmark", .type = "testext.unmark"});
    spec.nodes.push_back(
        {.id = "out", .type = "playback", .options = {{"device", "s"}}, .inputs = 1});
    spec.edges.push_back({{"mic", std::string("out_1")}, {"mark", std::string("in")}});
    spec.edges.push_back({{"mark", std::string("out")}, {"unmark", std::string("in")}});
    spec.edges.push_back({{"unmark", std::string("out")}, {"out", std::string("in_1")}});

    std::string error;
    auto graph = GraphCompiler::compile(spec, envFor(spec), nullptr, error);
    ASSERT_NE(graph, nullptr) << error;
    EXPECT_FLOAT_EQ(runBlock(*graph, 64, 2.5F).front(), 2.5F);
}

TEST(Extension, RefusesAWireBetweenTwoOfItsPortTypes)
{
    ExtensionLoader loader;
    ASSERT_TRUE(loadWith(loader, nullptr).loaded);

    GraphSpec spec;
    spec.version = 1;
    spec.nodes.push_back(
        {.id = "mic", .type = "capture", .options = {{"source", "m"}}, .outputs = 1});
    spec.nodes.push_back({.id = "mark", .type = "testext.mark"});
    spec.nodes.push_back({.id = "acc", .type = "testext.accumulate"});
    spec.edges.push_back({{"mic", std::string("out_1")}, {"mark", std::string("in")}});
    spec.edges.push_back({{"mark", std::string("out")}, {"acc", std::string("in")}});

    std::string error;
    EXPECT_EQ(GraphCompiler::compile(spec, envFor(spec), nullptr, error), nullptr);
    EXPECT_NE(error.find("marker"), std::string::npos) << error;
}

TEST(Extension, RefusesOneWhosePortNamesATypeNobodyDeclared)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, "bad_port_type");

    EXPECT_FALSE(result.loaded);
    EXPECT_NE(result.error.find("no_such_type"), std::string::npos) << result.error;
    EXPECT_EQ(NodeRegistry::instance().find("badportext.thing"), nullptr);
}

TEST(Extension, RefusesOneThatRedeclaresATypeDifferently)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, "clashing_port_type");

    EXPECT_FALSE(result.loaded);
    EXPECT_NE(result.error.find("text"), std::string::npos) << result.error;
    EXPECT_EQ(NodeRegistry::instance().find("clashext.thing"), nullptr);

    const auto *text = PortTypeManifest::instance().find("text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->transport, PortTransport::Value) << "the built-in survived the attempt";
}

TEST(Extension, TheSdkTextHelpersRoundTrip)
{
    std::vector<std::byte> block(avc::sdk::kTextPortTypeBytes, std::byte{0});
    EXPECT_TRUE(avc::sdk::readText(block.data()).empty());

    avc::sdk::writeText(block.data(), "hello");
    EXPECT_EQ(avc::sdk::readText(block.data()), "hello");

    avc::sdk::writeText(block.data(), std::string(avc::sdk::kTextPortTypeBytes * 2, 'x'));
    EXPECT_EQ(avc::sdk::readText(block.data()).size(),
              avc::sdk::kTextPortTypeBytes - sizeof(std::uint32_t))
        << "what does not fit is truncated, not written past the end";

    EXPECT_TRUE(avc::sdk::readText(nullptr).empty());
}

TEST(Extension, SegmentedTextFramesRemainPlainTextCompatible)
{
    std::vector<std::byte> block(avc::sdk::kTextPortTypeBytes, std::byte{0});
    avc::sdk::writeTextFrame(block.data(), "streaming", 42, 7, 3, false);

    EXPECT_EQ(avc::sdk::readText(block.data()), "streaming");
    auto frame = avc::sdk::readTextFrame(block.data());
    ASSERT_TRUE(frame.valid);
    EXPECT_TRUE(frame.segmented);
    EXPECT_FALSE(frame.final);
    EXPECT_EQ(frame.stream, 42U);
    EXPECT_EQ(frame.segment, 7U);
    EXPECT_EQ(frame.revision, 3U);

    avc::sdk::writeTextFrame(block.data(), "streaming", 42, 7, 4, true);
    frame = avc::sdk::readTextFrame(block.data());
    EXPECT_TRUE(frame.final);
    EXPECT_EQ(frame.revision, 4U);

    avc::sdk::writeText(block.data(), "plain again");
    frame = avc::sdk::readTextFrame(block.data());
    EXPECT_EQ(frame.value, "plain again");
    EXPECT_FALSE(frame.segmented) << "plain writes invalidate a previous frame footer";
}

TEST(Extension, RefusesOneBuiltAgainstAnotherAbi)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, "bad_abi");

    EXPECT_FALSE(result.loaded);
    EXPECT_NE(result.error.find("ABI"), std::string::npos) << result.error;
    EXPECT_EQ(NodeRegistry::instance().find("testext.accumulate"), nullptr);
}

TEST(Extension, RefusesAnIdThatCouldNotNamespaceAnything)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, "bad_id");

    EXPECT_FALSE(result.loaded);
    EXPECT_NE(result.error.find("Bad.Id!"), std::string::npos) << result.error;
}

TEST(Extension, RegistersNothingWhenOneNodeTypeIsBad)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, "duplicate");

    EXPECT_FALSE(result.loaded);
    EXPECT_NE(result.error.find("same"), std::string::npos) << result.error;
    EXPECT_EQ(NodeRegistry::instance().find("dupext.same"), nullptr);
}

TEST(Extension, AcceptsThatOneCanRefuseToLoad)
{
    ExtensionLoader loader;
    const LoadedExtension result = loadWith(loader, "refuse");

    EXPECT_FALSE(result.loaded);
    EXPECT_FALSE(result.error.empty());
}

TEST(Extension, SaysSoWhenTheFileIsNotThere)
{
    ExtensionLoader loader;
    ExtensionRequest request;
    request.path = "/nonexistent/not-an-extension.so";
    loader.loadAll({request}, std::filesystem::temp_directory_path());

    ASSERT_EQ(loader.results().size(), 1U);
    EXPECT_FALSE(loader.results().front().loaded);
    EXPECT_FALSE(loader.results().front().error.empty());
}

TEST(Extension, OneBadExtensionDoesNotCostTheOthers)
{
    ExtensionLoader loader;
    unsetEnvironment("AVC_TEST_PLUGIN_MODE");

    ExtensionRequest missing;
    missing.path = "/nonexistent/not-an-extension.so";
    ExtensionRequest good;
    good.path = AVC_TEST_PLUGIN_PATH;

    loader.loadAll({missing, good}, std::filesystem::temp_directory_path() / "avc-ext-test");

    ASSERT_EQ(loader.results().size(), 2U);
    EXPECT_FALSE(loader.results()[0].loaded);
    EXPECT_TRUE(loader.results()[1].loaded) << loader.results()[1].error;
    EXPECT_NE(NodeRegistry::instance().find("testext.accumulate"), nullptr);
}

TEST(NodeRegistryTest, RefusesADuplicateTypeAndAnythingAfterSealing)
{
    NodeRegistry &registry = NodeRegistry::instance();

    avc::node::NodeDescriptor descriptor;
    descriptor.type = "made.up";
    descriptor.label = "Made up";
    EXPECT_TRUE(registry.add(descriptor, nullptr));
    EXPECT_FALSE(registry.add(descriptor, nullptr)) << "a type may only be claimed once";

    const auto *first = registry.find("made.up");
    ASSERT_NE(first, nullptr);
    avc::node::NodeDescriptor other;
    other.type = "made.up.too";
    EXPECT_TRUE(registry.add(other, nullptr));
    EXPECT_EQ(registry.find("made.up"), first);

    registry.seal();
    EXPECT_TRUE(registry.sealed());
    avc::node::NodeDescriptor late;
    late.type = "too.late";
    EXPECT_FALSE(registry.add(late, nullptr));
    EXPECT_EQ(registry.find("too.late"), nullptr);
}

using avc::control::ExtensionStore;

class Store : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path()
                / ("avc-store-" + std::to_string(processId()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_ / "cfg");
        std::filesystem::create_directories(root_ / "data");
#ifdef _WIN32
        setEnvironment("APPDATA", (root_ / "cfg").string());
        setEnvironment("LOCALAPPDATA", (root_ / "data").string());
#else
        setEnvironment("XDG_CONFIG_HOME", (root_ / "cfg").string());
        setEnvironment("XDG_DATA_HOME", (root_ / "data").string());
#endif

        extra_ = root_ / "extra";
        std::filesystem::create_directories(extra_);
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    void place(const std::filesystem::path &dir, const std::string &name) const
    {
        std::filesystem::create_directories(dir);
        std::ofstream(dir / name) << "not really a shared object";
    }

    std::filesystem::path root_;
    std::filesystem::path extra_;
};

TEST_F(Store, FindsFilesAndLetsTheFirstDirectoryWin)
{
    place(extra_, extName("one"));
    place(extra_, extName("shared"));
    place(ExtensionStore::userDirectory(), extName("shared"));
    place(ExtensionStore::userDirectory(), extName("two"));
    place(extra_, "notes.txt");

    ExtensionStore store({extra_.string()});
    std::map<std::string, std::string> by_key;
    for (const ExtensionStore::Found &found : store.found()) {
        by_key[found.key] = found.path.string();
    }

    EXPECT_EQ(by_key.size(), 3U) << "one, two and a single shared";
    EXPECT_EQ(by_key.count("notes"), 0U) << "only shared libraries are extensions";
    EXPECT_EQ(by_key["shared"], (extra_ / extName("shared")).string());
}

TEST_F(Store, RemembersWhatWasDecidedAboutOne)
{
    place(extra_, extName("thing"));
    std::string error;
    {
        ExtensionStore store({extra_.string()});
        ASSERT_TRUE(store.setEnabled("thing", false, error)) << error;
        ASSERT_TRUE(store.setSettings("thing", nlohmann::json{{"where", "/models"}}, error))
            << error;
    }

    ExtensionStore reopened({extra_.string()});
    const nlohmann::json described = reopened.describe(nlohmann::json::array());
    ASSERT_EQ(described.size(), 1U);
    EXPECT_FALSE(described[0]["enabled"].get<bool>());
    EXPECT_EQ(described[0]["disabledReason"], "user");
    EXPECT_EQ(described[0]["values"]["where"], "/models");
    EXPECT_EQ(described[0]["state"], "disabled");
}

TEST_F(Store, OnlyTellsTheEngineAboutTheOnesThatAreOn)
{
    place(extra_, extName("on"));
    place(extra_, extName("off"));

    ExtensionStore store({extra_.string()});
    std::string error;
    ASSERT_TRUE(store.setEnabled("off", false, error)) << error;
    ASSERT_TRUE(store.setSettings("on", nlohmann::json{{"k", "v"}}, error)) << error;

    const nlohmann::json manifest = store.engineManifest();
    ASSERT_EQ(manifest["extensions"].size(), 1U) << "a file nobody switched on is not loaded";
    EXPECT_EQ(manifest["extensions"][0]["path"], (extra_ / extName("on")).string());
    EXPECT_EQ(manifest["extensions"][0]["settings"]["k"], "v");
    EXPECT_FALSE(manifest["configRoot"].get<std::string>().empty());
}

TEST_F(Store, AnUploadArrivesSwitchedOff)
{
    ExtensionStore store({extra_.string()});
    std::string error;
    ASSERT_TRUE(store.install(extName("uploaded"), "not really a library", error)) << error;

    EXPECT_TRUE(std::filesystem::exists(ExtensionStore::userDirectory() / extName("uploaded")));
    EXPECT_EQ(store.engineManifest()["extensions"].size(), 0U);

    const nlohmann::json described = store.describe(nlohmann::json::array());
    ASSERT_EQ(described.size(), 1U);
    EXPECT_FALSE(described[0]["enabled"].get<bool>());
}

TEST_F(Store, ASelectedFileIsInstalledWithoutAnUpload)
{
    const std::filesystem::path source = root_ / "selected" / extName("picked");
    place(source.parent_path(), source.filename().string());

    ExtensionStore store({extra_.string()});
    std::string error;
    ASSERT_TRUE(store.installFile(source, error)) << error;

    const std::filesystem::path installed = ExtensionStore::userDirectory() / source.filename();
    EXPECT_TRUE(std::filesystem::exists(installed));
    EXPECT_EQ(store.engineManifest()["extensions"].size(), 0U);

    const nlohmann::json described = store.describe(nlohmann::json::array());
    ASSERT_EQ(described.size(), 1U);
    EXPECT_EQ(described[0]["path"], installed.string());
    EXPECT_FALSE(described[0]["enabled"].get<bool>());
}

TEST_F(Store, RefusesAFilenameThatNamesSomethingElse)
{
    EXPECT_TRUE(ExtensionStore::validFilename(extName("thing")));
    EXPECT_TRUE(ExtensionStore::validFilename(extName("a-b_c.1")));

    EXPECT_FALSE(ExtensionStore::validFilename("../escape.so"));
    EXPECT_FALSE(ExtensionStore::validFilename("dir/thing.so"));
    EXPECT_FALSE(ExtensionStore::validFilename("/abs/thing.so"));
    EXPECT_FALSE(ExtensionStore::validFilename(".hidden.so"));
    EXPECT_FALSE(ExtensionStore::validFilename("thing.so.bak"));
    EXPECT_FALSE(ExtensionStore::validFilename("thing"));
    EXPECT_FALSE(ExtensionStore::validFilename(""));
    EXPECT_FALSE(ExtensionStore::validFilename("thing$(rm -rf).so"));

    ExtensionStore store({extra_.string()});
    std::string error;
    EXPECT_FALSE(store.install("../escape.so", "x", error));
    EXPECT_FALSE(std::filesystem::exists(ExtensionStore::userDirectory().parent_path()
                                         / "escape.so"));
}

TEST_F(Store, DeletingOneTakesTwoDeliberateSteps)
{
    place(extra_, extName("doomed"));
    ExtensionStore store({extra_.string()});
    std::string error;

    EXPECT_FALSE(store.remove("doomed", error)) << "it is still switched on";
    EXPECT_TRUE(std::filesystem::exists(extra_ / extName("doomed")));

    ASSERT_TRUE(store.setEnabled("doomed", false, error)) << error;
    EXPECT_TRUE(store.remove("doomed", error)) << error;
    EXPECT_FALSE(std::filesystem::exists(extra_ / extName("doomed")));
    EXPECT_EQ(store.found().size(), 0U);
}

TEST_F(Store, SwitchesOffWhateverTheEngineDiedInside)
{
    place(extra_, extName("guilty"));
    place(extra_, extName("innocent"));
    ExtensionStore store({extra_.string()});

    EXPECT_TRUE(store.quarantine(extra_ / extName("guilty")));

    const nlohmann::json described = store.describe(nlohmann::json::array());
    for (const nlohmann::json &entry : described) {
        if (entry["key"] == "guilty") {
            EXPECT_FALSE(entry["enabled"].get<bool>());
            EXPECT_EQ(entry["disabledReason"], "crash");
            EXPECT_GT(entry["lastCrash"].get<std::int64_t>(), 0);
        } else {
            EXPECT_TRUE(entry["enabled"].get<bool>()) << "only the one at fault is switched off";
        }
    }

    const nlohmann::json manifest = store.engineManifest();
    ASSERT_EQ(manifest["extensions"].size(), 1U);
    EXPECT_EQ(manifest["extensions"][0]["path"], (extra_ / extName("innocent")).string());

    std::string error;
    ASSERT_TRUE(store.setEnabled("guilty", true, error)) << error;
    EXPECT_EQ(store.describe(nlohmann::json::array())[0]["disabledReason"], "");
}

TEST_F(Store, IgnoresACrashItCannotPin)
{
    place(extra_, extName("thing"));
    ExtensionStore store({extra_.string()});

    EXPECT_FALSE(store.quarantine("/usr/lib/libc.so.6"))
        << "the engine faulting in something that is not an extension blames nobody";
    EXPECT_TRUE(store.describe(nlohmann::json::array())[0]["enabled"].get<bool>());
}

}
