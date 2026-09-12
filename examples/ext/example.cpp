
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
const translations = {
  'zh-CN': {
    name: '示例效果',
    category: { fx: '效果' },
    settings: {
      max_rate_hz: {
        name: '最高速率',
        description: '>20 Hz 逐渐变为环形调制。',
      },
    },
    nodes: {
      tremolo: {
        label: '颤音',
        ports: { in: '输入', out: '输出' },
        params: {
          rate_hz: { name: '速率', description: '每秒起伏次数。' },
          depth: { name: '深度', description: '起伏幅度。' },
        },
      },
      level: { label: '电平', ports: { in: '输入', out: '输出', said: '读数' } },
    },
    presets: { 'Tremolo mic': '麦克风颤音' },
    ui: {
      title: '示例设置',
      fastestRate: '最高速率（Hz）',
      save: '保存并重启引擎',
      saving: '保存中…',
      saved: '已保存',
    },
  },
  'en-US': {
    name: 'Example effects',
    category: { fx: 'Effects' },
    settings: {
      max_rate_hz: {
        name: 'Maximum rate',
        description: '>20 Hz gradually becomes ring modulation.',
      },
    },
    nodes: {
      tremolo: {
        label: 'Tremolo',
        ports: { in: 'Input', out: 'Output' },
        params: {
          rate_hz: { name: 'Rate', description: 'Sweeps per second.' },
          depth: { name: 'Depth', description: 'Sweep amount.' },
        },
      },
      level: { label: 'Level', ports: { in: 'Input', out: 'Output', said: 'Reading' } },
    },
    presets: { 'Tremolo mic': 'Tremolo microphone' },
    ui: {
      title: 'Example settings',
      fastestRate: 'Maximum rate (Hz)',
      save: 'Save and restart engine',
      saving: 'Saving…',
      saved: 'Saved',
    },
  },
}

class ExampleSettings extends HTMLElement {
  connectedCallback() {
    this.stopLanguage = this.avcContext.i18n.onLanguageChanged(() => this.render())
    this.render()
  }

  disconnectedCallback() {
    this.stopLanguage?.()
  }

  render() {
    const context = this.avcContext
    const t = (key, options) => context.i18n.t(key, options)
    const values = context.settings.get()
    const section = document.createElement('section')
    section.className = 'panel'
    const title = document.createElement('h2')
    title.className = 'panel__title'
    title.textContent = t('ui.title')
    const label = document.createElement('label')
    label.className = 'field'
    const name = document.createElement('span')
    name.className = 'field__name'
    name.textContent = t('ui.fastestRate')
    const input = document.createElement('input')
    input.type = 'number'
    input.min = '1'
    input.max = '200'
    input.value = values.max_rate_hz ?? '20'
    label.append(name, input)
    const save = document.createElement('button')
    save.className = 'button button--primary'
    save.textContent = t('ui.save')
    const status = document.createElement('span')
    status.setAttribute('role', 'status')
    status.className = 'hint'
    save.addEventListener('click', async () => {
      save.disabled = true
      status.textContent = t('ui.saving')
      try {
        await context.settings.save({ ...values, max_rate_hz: input.value })
        status.textContent = t('ui.saved')
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

export function activate(api) {
  api.i18n.addResources(translations)
  if (!customElements.get('avc-example-settings'))
    customElements.define('avc-example-settings', ExampleSettings)
  api.components.registerSettings('avc-example-settings')
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
    plugin.author("avc").describe("Tremolo extension ABI example.");

    plugin.floatSetting("max_rate_hz", 20.0, 1.0, 200.0, "Fastest rate",
                        ">20 Hz gradually becomes ring modulation.");

    const float max_rate = std::strtof(plugin.setting("max_rate_hz", "20").c_str(), nullptr);

    plugin.node<Tremolo>(
        avc::sdk::NodeDesc("tremolo", "fx", "Tremolo")
            .in("in")
            .out("out")
            .floatParam("rate_hz", 0.1F, max_rate > 0.1F ? max_rate : 20.0F, 5.0F, "Hz",
                        AVC_CURVE_LOG, "Sweeps per second.")
            .floatParam("depth", 0.0F, 1.0F, 0.5F, "", AVC_CURVE_LINEAR,
                        "Sweep amount."));

    plugin.node<Level>(avc::sdk::NodeDesc("level", "fx", "Level")
                           .in("in")
                           .out("out")
                           .out("said", avc::sdk::kTextPortType));

    plugin.preset("Tremolo mic", kDemoGraph);
    plugin.uiAsset("ui/main.js", kEditorModule, "text/javascript; charset=utf-8")
        .uiEntry("ui/main.js")
        .onUiRequest(&onUiRequest);
}
