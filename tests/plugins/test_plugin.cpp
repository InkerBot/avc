
#include <avc/avc_plugin.hpp>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

float g_scale = 1.0F;

class Accumulator final : public avc::sdk::Node {
public:
    void inherit(const avc::sdk::Node &previous) override
    {
        total_ = static_cast<const Accumulator &>(previous).total_;
    }

    void setParam(std::uint32_t index, float value) noexcept override
    {
        if (index == 0) {
            offset_ = value;
        }
    }

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        float *out = ctx.outputs[0];
        const float *in = ctx.inputs[0];
        total_ += in != nullptr ? in[0] : 0.0F;
        const float value = (total_ + offset_) * g_scale;
        for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
            out[f] = value;
        }
    }

private:
    float total_ = 0.0F;
    float offset_ = 0.0F;
};

class Heavy final : public avc::sdk::Node {
public:
    void setParam(std::uint32_t index, float value) noexcept override
    {
        if (index == 0) {
            explode_ = value >= 0.5F;
        }
    }

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        if (explode_) {
            volatile int *nowhere = nullptr;
            *nowhere = 1;
        }
        for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
            ctx.outputs[0][f] = ctx.inputs[0] != nullptr ? ctx.inputs[0][f] : 0.0F;
        }
    }

private:
    bool explode_ = false;
};

class Configurable final : public avc::sdk::Node {
public:
    bool prepare(const AvcPrepareInfo &, std::string &error) override
    {
        if (path_ != "/valid/model.avcrvc") {
            error = "model package is not valid";
            return false;
        }
        ready_ = true;
        return true;
    }

    void setOption(std::uint32_t index, std::string_view value) override
    {
        if (index == 0) path_ = value;
    }

    void setParam(std::uint32_t, float) noexcept override {}

    bool status(AvcNodeStatus &out) const noexcept override
    {
        out.state = ready_ ? AVC_NODE_READY : AVC_NODE_OFFLINE;
        out.progress = ready_ ? 1.0F : 0.0F;
        std::snprintf(out.message, sizeof(out.message), "%s", path_.c_str());
        return true;
    }

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
            ctx.outputs[0][f] = ctx.inputs[0] != nullptr ? ctx.inputs[0][f] : 0.0F;
        }
    }

private:
    std::string path_;
    bool ready_ = false;
};

constexpr const char *kMarker = "marker";

class Mark final : public avc::sdk::Node {
public:
    void setParam(std::uint32_t, float) noexcept override {}

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        const float value = ctx.inputs[0] != nullptr ? ctx.inputs[0][0] : 0.0F;
        std::memcpy(ctx.out_blocks[0], &value, sizeof(value));
    }
};

class Unmark final : public avc::sdk::Node {
public:
    void setParam(std::uint32_t, float) noexcept override {}

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        float value = 0.0F;
        if (ctx.in_blocks[0] != nullptr) {
            std::memcpy(&value, ctx.in_blocks[0], sizeof(value));
        }
        for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
            ctx.outputs[0][f] = value;
        }
    }
};

const char *const kPreset = R"({"version":1,"nodes":[],"edges":[]})";

std::string mode()
{
    const char *value = std::getenv("AVC_TEST_PLUGIN_MODE");
    return value != nullptr ? value : "";
}

void onConfigure(const char *key, const char *value)
{
    if (key != nullptr && value != nullptr && std::strcmp(key, "scale") == 0) {
        g_scale = std::strtof(value, nullptr);
    }
}

avc::sdk::Plugin &good();

void onUiRequest(const AvcUiRequest *request)
{
    if (request == nullptr) return;
    const std::string_view payload(request->json != nullptr ? request->json : "",
                                   request->json_size);
    good().replyUi(request->request_id, payload);
}

avc::sdk::Plugin &good()
{
    static avc::sdk::Plugin plugin("testext", "Test extension", "0.1");
    return plugin;
}

void buildGood()
{
    good().author("avc").describe("Loaded by the extension tests.").onConfigure(&onConfigure);
    good().floatSetting("scale", 1.0, 0.0, 10.0, "Scale", "Multiplies everything.");
    good().node<Accumulator>(avc::sdk::NodeDesc("accumulate", "test", "Accumulate")
                                 .in("in")
                                 .out("out")
                                 .floatParam("offset", -10.0F, 10.0F, 0.0F));
    good().node<Heavy>(avc::sdk::NodeDesc("heavy", "test", "Heavy")
                           .in("in")
                           .out("out")
                           .boolParam("crash", false, "Fault on the next block.")
                           .notRealtimeSafe()
                           .recommendedColdBlock(4096));
    good().node<Configurable>(
        avc::sdk::NodeDesc("configurable", "test", "Configurable")
            .in("in")
            .out("out")
            .pathParam("model_path", "/default/model.avcrvc", "A test model package."));
    good().portType(kMarker, AVC_PORT_VALUE, sizeof(float), "Marker");
    good().node<Mark>(avc::sdk::NodeDesc("mark", "test", "Mark").in("in").out("out", kMarker));
    good().node<Unmark>(
        avc::sdk::NodeDesc("unmark", "test", "Unmark").in("in", kMarker).out("out"));
    good().preset("Empty", kPreset);
    good().uiAsset("ui/main.js", "export function activate() {}", "text/javascript")
        .uiEntry("ui/main.js")
        .onUiRequest(&onUiRequest);
}

avc::sdk::Plugin &badId()
{
    static avc::sdk::Plugin plugin("Bad.Id!", "Bad id", "0.1");
    return plugin;
}

void buildBadId()
{
    badId().node<Heavy>(avc::sdk::NodeDesc("thing", "test", "Thing").in("in").out("out"));
}

avc::sdk::Plugin &duplicate()
{
    static avc::sdk::Plugin plugin("dupext", "Duplicate", "0.1");
    return plugin;
}

void buildDuplicate()
{
    duplicate().node<Heavy>(avc::sdk::NodeDesc("same", "test", "One").in("in").out("out"));
    duplicate().node<Heavy>(avc::sdk::NodeDesc("same", "test", "Two").in("in").out("out"));
}

avc::sdk::Plugin &badPortType()
{
    static avc::sdk::Plugin plugin("badportext", "Bad port type", "0.1");
    return plugin;
}

void buildBadPortType()
{
    badPortType().node<Heavy>(
        avc::sdk::NodeDesc("thing", "test", "Thing").in("in").out("out", "no_such_type"));
}

avc::sdk::Plugin &clashingPortType()
{
    static avc::sdk::Plugin plugin("clashext", "Clashing port type", "0.1");
    return plugin;
}

void buildClashingPortType()
{
    clashingPortType().portType(avc::sdk::kTextPortType, AVC_PORT_STREAM, 16);
    clashingPortType().node<Heavy>(avc::sdk::NodeDesc("thing", "test", "Thing").in("in").out("out"));
}

avc::sdk::Plugin &badUi()
{
    static avc::sdk::Plugin plugin("baduiext", "Bad UI", "0.1");
    return plugin;
}

void buildBadUi()
{
    badUi().node<Heavy>(avc::sdk::NodeDesc("thing", "test", "Thing").in("in").out("out"));
    badUi().uiAsset("../main.js", "export function activate() {}", "text/javascript")
        .uiEntry("../main.js");
}

}

extern "C" AVC_PLUGIN_EXPORT const AvcPlugin *avc_plugin_init(const AvcHostApi *host)
{
    const std::string which = mode();

    if (which == "refuse") {
        if (host != nullptr && host->log != nullptr) {
            host->log(host->context, AVC_LOG_ERROR, "refusing to load, on purpose");
        }
        return nullptr;
    }

    if (which == "bad_abi") {
        static AvcPlugin plugin{};
        plugin.abi_version = AVC_ABI_VERSION + 1000U;
        plugin.id = "testext";
        plugin.name = "From the future";
        plugin.version = "0.1";
        return &plugin;
    }

    if (which == "bad_id") {
        return avc::sdk::detail::init(badId(), host, &buildBadId);
    }
    if (which == "duplicate") {
        return avc::sdk::detail::init(duplicate(), host, &buildDuplicate);
    }
    if (which == "bad_port_type") {
        return avc::sdk::detail::init(badPortType(), host, &buildBadPortType);
    }
    if (which == "clashing_port_type") {
        return avc::sdk::detail::init(clashingPortType(), host, &buildClashingPortType);
    }
    if (which == "bad_ui_path") {
        return avc::sdk::detail::init(badUi(), host, &buildBadUi);
    }

    return avc::sdk::detail::init(good(), host, &buildGood);
}
