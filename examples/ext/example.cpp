
#include <avc/avc_plugin.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr float kTwoPi = 6.28318530718F;

class Tremolo final : public avc::sdk::Node {
public:
    enum Param : std::uint32_t {
        kRate = 0,
        kDepth = 1,
    };

    void prepare(const AvcPrepareInfo &info) override
    {
        sample_rate_ = info.sample_rate > 0 ? static_cast<float>(info.sample_rate) : 48000.0F;
    }

    void inherit(const avc::sdk::Node &previous) override
    {
        phase_ = static_cast<const Tremolo &>(previous).phase_;
    }

    void setParam(std::uint32_t index, float value) noexcept override
    {
        if (index == kRate) {
            rate_ = value;
        } else if (index == kDepth) {
            depth_ = value;
        }
    }

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        float *out = ctx.outputs[0];
        const float *in = ctx.inputs[0];

        if (in == nullptr) {
            std::memset(out, 0, ctx.nframes * sizeof(float));
            return;
        }

        const float step = kTwoPi * rate_ / sample_rate_;
        const float depth = depth_;
        for (std::uint32_t f = 0; f < ctx.nframes; ++f) {
            const float gain = 1.0F - depth * 0.5F * (1.0F - std::cos(phase_));
            out[f] = in[f] * gain;
            phase_ += step;
            if (phase_ >= kTwoPi) {
                phase_ -= kTwoPi;
            }
        }
    }

private:
    float sample_rate_ = 48000.0F;
    float rate_ = 5.0F;
    float depth_ = 0.5F;
    float phase_ = 0.0F;
};

avc::sdk::Plugin plugin("example", "Example effects", "1.0");

void onUiRequest(const AvcUiRequest *request)
{
    if (request == nullptr) return;
    const std::string_view method(request->method != nullptr ? request->method : "");
    if (method == "about") {
        plugin.replyUi(request->request_id,
                       R"({"effect":"tremolo","managedBy":"example extension"})");
    } else {
        plugin.failUi(request->request_id, "unknown example method");
    }
}

const char *const kEditorModule = R"JS(
class ExampleSettings extends HTMLElement {
  connectedCallback() {
    const context = this.avcContext
    const values = context.settings.get()
    const section = document.createElement('section')
    section.className = 'panel'
    const title = document.createElement('h2')
    title.className = 'panel__title'
    title.textContent = 'Example extension settings'
    const label = document.createElement('label')
    label.className = 'field'
    const name = document.createElement('span')
    name.className = 'field__name'
    name.textContent = 'Fastest rate (Hz)'
    const input = document.createElement('input')
    input.type = 'number'
    input.min = '1'
    input.max = '200'
    input.value = values.max_rate_hz ?? '20'
    label.append(name, input)
    const save = document.createElement('button')
    save.className = 'button button--primary'
    save.textContent = 'Save and restart engine'
    const status = document.createElement('span')
    status.setAttribute('role', 'status')
    status.className = 'hint'
    save.addEventListener('click', async () => {
      save.disabled = true
      status.textContent = 'Saving…'
      try {
        await context.settings.save({ ...values, max_rate_hz: input.value })
        status.textContent = 'Saved'
      } catch (error) {
        status.textContent = error instanceof Error ? error.message : String(error)
      } finally {
        save.disabled = false
      }
    })
    section.append(title, label, save, status)
    this.replaceChildren(section)
  }
}

class TremoloInspector extends HTMLElement {
  connectedCallback() {
    const text = document.createElement('p')
    text.className = 'hint'
    text.textContent = `This control is owned by ${this.avcContext.extension.name}.`
    this.replaceChildren(text)
  }
}

export function activate(api) {
  if (!customElements.get('avc-example-settings'))
    customElements.define('avc-example-settings', ExampleSettings)
  if (!customElements.get('avc-example-tremolo'))
    customElements.define('avc-example-tremolo', TremoloInspector)
  api.components.registerSettings('avc-example-settings')
  api.components.registerNodeInspector('example.tremolo', 'avc-example-tremolo')
}
)JS";

const char *const kDemoGraph = R"({
  "version": 1,
  "nodes": [
    { "id": "mic",   "type": "capture",         "params": { "source": "@default_source" }, "outputs": 1 },
    { "id": "trem",  "type": "example.tremolo", "params": { "rate_hz": 4.0, "depth": 0.7 }, "ui": { "x": 300, "y": 80 } },
    { "id": "out",   "type": "virtual_mic",     "params": { "publish_as": "avc_tremolo" }, "inputs": 1, "ui": { "x": 600, "y": 80 } }
  ],
  "edges": [
    { "from": { "node": "mic",  "port": "out_1" }, "to": { "node": "trem", "port": "in" } },
    { "from": { "node": "trem", "port": "out"   }, "to": { "node": "out",  "port": "in_1" } }
  ]
})";

class Level final : public avc::sdk::Node {
public:
    void setParam(std::uint32_t, float) noexcept override {}

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        const float *in = ctx.inputs[0];
        float *out = ctx.outputs[0];
        if (in == nullptr) {
            std::memset(out, 0, ctx.nframes * sizeof(float));
        } else {
            std::memcpy(out, in, ctx.nframes * sizeof(float));
        }

        float peak = 0.0F;
        for (std::uint32_t f = 0; in != nullptr && f < ctx.nframes; ++f) {
            peak = std::fmax(peak, std::fabs(in[f]));
        }

        char said[32];
        std::snprintf(said, sizeof(said), "%.1f dB",
                      peak > 1e-6F ? 20.0F * std::log10(peak) : -120.0F);
        avc::sdk::writeText(ctx.out_blocks[1], said);
    }
};

}

AVC_PLUGIN_MAIN(plugin)
{
    plugin.author("avc").describe("A tremolo, as a worked example of the extension ABI.");

    plugin.floatSetting("max_rate_hz", 20.0, 1.0, 200.0, "Fastest rate",
                        "The top of the rate slider. Past about 20 Hz a tremolo stops "
                        "sounding like one and starts sounding like a ring modulator.");

    const float max_rate = std::strtof(plugin.setting("max_rate_hz", "20").c_str(), nullptr);

    plugin.node<Tremolo>(
        avc::sdk::NodeDesc("tremolo", "fx", "Tremolo")
            .in("in")
            .out("out")
            .floatParam("rate_hz", 0.1F, max_rate > 0.1F ? max_rate : 20.0F, 5.0F, "Hz",
                        AVC_CURVE_LOG, "How often the volume sweeps up and down.")
            .floatParam("depth", 0.0F, 1.0F, 0.5F, "", AVC_CURVE_LINEAR,
                        "How far it ducks at the bottom of the sweep."));

    plugin.node<Level>(avc::sdk::NodeDesc("level", "fx", "Level")
                           .in("in")
                           .out("out")
                           .out("said", avc::sdk::kTextPortType));

    plugin.preset("Tremolo mic", kDemoGraph);
    plugin.uiAsset("ui/main.js", kEditorModule, "text/javascript; charset=utf-8")
        .uiEntry("ui/main.js")
        .onUiRequest(&onUiRequest);
}
