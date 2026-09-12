#include "avc_rvc/FeatureIndex.hpp"
#include "avc_rvc/ModelManifest.hpp"
#include "avc_rvc/RmvpeFrontend.hpp"

#include <avc/avc_plugin.hpp>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kFeatureRate = 16000;

std::string g_default_model;
int g_provider = 0;
int g_cuda_device = 0;

Ort::Env &ortEnvironment()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "avc-rvc");
    return env;
}

std::vector<float> resample(const float *input, std::size_t input_frames,
                            std::uint32_t input_rate, std::uint32_t output_rate)
{
    if (input == nullptr || input_frames == 0 || input_rate == 0 || output_rate == 0) return {};
    const std::size_t output_frames = std::max<std::size_t>(
        1, (input_frames * static_cast<std::uint64_t>(output_rate) + input_rate / 2) / input_rate);
    std::vector<float> output(output_frames);
    if (input_frames == 1) {
        std::fill(output.begin(), output.end(), input[0]);
        return output;
    }
    const double scale = static_cast<double>(input_frames - 1)
                         / static_cast<double>(std::max<std::size_t>(output_frames - 1, 1));
    for (std::size_t i = 0; i < output_frames; ++i) {
        const double at = static_cast<double>(i) * scale;
        const std::size_t left = std::min<std::size_t>(static_cast<std::size_t>(at), input_frames - 1);
        const std::size_t right = std::min(left + 1, input_frames - 1);
        const float fraction = static_cast<float>(at - static_cast<double>(left));
        output[i] = input[left] + (input[right] - input[left]) * fraction;
    }
    return output;
}

std::int64_t coarsePitch(float f0)
{
    if (f0 <= 0.0F) return 1;
    constexpr float f0_min = 50.0F;
    constexpr float f0_max = 1100.0F;
    const float mel = 1127.0F * std::log1p(f0 / 700.0F);
    const float mel_min = 1127.0F * std::log1p(f0_min / 700.0F);
    const float mel_max = 1127.0F * std::log1p(f0_max / 700.0F);
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::lround((mel - mel_min) * 254.0F / (mel_max - mel_min) + 1.0F)),
        1, 255);
}

class ModelRuntime {
public:
    ModelRuntime(avc::rvc::ModelManifest manifest, std::uint32_t graph_rate, int provider,
                 int cuda_device)
        : manifest_(std::move(manifest)), graph_rate_(graph_rate),
          options_(makeOptions(provider, cuda_device)),
          content_(ortEnvironment(), manifest_.contentvec.c_str(), options_),
          synth_(ortEnvironment(), manifest_.synthesizer.c_str(), options_)
    {
        if (manifest_.synthesizer_frames != 0 && manifest_.content_samples_16k == 0
            && manifest_.synthesizer_frames < 32) {
            throw std::runtime_error(
                "model package uses the obsolete short RVC trace; reimport its .pth");
        }
        if (manifest_.uses_f0) {
            rmvpe_ = std::make_unique<Ort::Session>(ortEnvironment(), manifest_.rmvpe.c_str(), options_);
        }
        if (manifest_.uses_index) {
            std::string error;
            feature_index_ = avc::rvc::FeatureIndex::load(
                manifest_.feature_index, manifest_.content_dim, error);
            if (!feature_index_) throw std::runtime_error(error);
        }
        Ort::AllocatorWithDefaultOptions allocator;
        for (std::size_t i = 0; i < content_.GetInputCount(); ++i) {
            content_inputs_.push_back(content_.GetInputNameAllocated(i, allocator).get());
        }
        if (content_inputs_.empty() || content_inputs_.size() > 2) {
            throw std::runtime_error("ContentVec must expose one audio input and an optional attention mask");
        }
        content_output_ = content_.GetOutputNameAllocated(0, allocator).get();
        synth_output_ = synth_.GetOutputNameAllocated(0, allocator).get();
        for (std::size_t i = 0; i < synth_.GetInputCount(); ++i) {
            synth_inputs_.push_back(synth_.GetInputNameAllocated(i, allocator).get());
        }
        if (synth_inputs_.size() != 6) {
            throw std::runtime_error("synthesizer must expose six RVC inputs");
        }
        if (rmvpe_) {
            rmvpe_input_ = rmvpe_->GetInputNameAllocated(0, allocator).get();
            rmvpe_output_ = rmvpe_->GetOutputNameAllocated(0, allocator).get();
        }
        if (manifest_.content_samples_16k != 0) {
            const std::uint64_t numerator =
                static_cast<std::uint64_t>(manifest_.content_samples_16k) * graph_rate_;
            context_graph_frames_ = static_cast<std::size_t>(
                (numerator + kFeatureRate / 2) / kFeatureRate);
            graph_context_.assign(context_graph_frames_, 0.0F);
        }
    }

    bool degraded() const noexcept { return false; }

    void reset() noexcept
    {
        std::fill(graph_context_.begin(), graph_context_.end(), 0.0F);
    }

    std::vector<float> run(const float *input, std::size_t frames, float pitch_shift,
                           std::int64_t speaker, float index_rate)
    {
        const float *content_input = input;
        std::size_t content_frames = frames;
        if (!graph_context_.empty()) {
            const std::size_t incoming = std::min(frames, graph_context_.size());
            if (incoming < graph_context_.size()) {
                std::memmove(graph_context_.data(), graph_context_.data() + incoming,
                             (graph_context_.size() - incoming) * sizeof(float));
            }
            float *tail = graph_context_.data() + graph_context_.size() - incoming;
            if (input != nullptr) std::copy_n(input + frames - incoming, incoming, tail);
            else std::fill_n(tail, incoming, 0.0F);
            content_input = graph_context_.data();
            content_frames = graph_context_.size();
        }
        std::vector<float> audio16 = resample(content_input, content_frames, graph_rate_, kFeatureRate);
        if (audio16.empty()) return {};
        if (manifest_.content_samples_16k != 0) {
            audio16.resize(manifest_.content_samples_16k, 0.0F);
        }

        Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<Ort::Value> content_values;
        std::vector<std::int64_t> attention_mask;
        content_values.reserve(content_inputs_.size());
        if (content_inputs_.size() == 1) {
            // The established v1 export accepts [batch, channel, samples].
            const std::array<std::int64_t, 3> content_shape{
                1, 1, static_cast<std::int64_t>(audio16.size())};
            content_values.push_back(Ort::Value::CreateTensor<float>(
                memory, audio16.data(), audio16.size(), content_shape.data(), content_shape.size()));
        } else {
            // Wav2Vec2-based v2 exports accept [batch, samples] plus an int64
            // attention mask. The bundled converter pins this signature.
            const std::array<std::int64_t, 2> content_shape{
                1, static_cast<std::int64_t>(audio16.size())};
            content_values.push_back(Ort::Value::CreateTensor<float>(
                memory, audio16.data(), audio16.size(), content_shape.data(), content_shape.size()));
            attention_mask.assign(audio16.size(), 1);
            content_values.push_back(Ort::Value::CreateTensor<std::int64_t>(
                memory, attention_mask.data(), attention_mask.size(),
                content_shape.data(), content_shape.size()));
        }
        std::vector<const char *> content_input_names;
        content_input_names.reserve(content_inputs_.size());
        for (const std::string &name : content_inputs_) content_input_names.push_back(name.c_str());
        const char *content_output_names[]{content_output_.c_str()};
        auto content_outputs = content_.Run(Ort::RunOptions{nullptr}, content_input_names.data(),
                                            content_values.data(), content_values.size(),
                                            content_output_names, 1);
        Ort::Value &content_value = content_outputs.front();
        const std::vector<std::int64_t> shape =
            content_value.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3 || shape[0] != 1) {
            throw std::runtime_error("ContentVec output must have shape [1,C,T] or [1,T,C]");
        }
        const bool channels_first = shape[1] == static_cast<std::int64_t>(manifest_.content_dim);
        const std::size_t source_frames = static_cast<std::size_t>(channels_first ? shape[2] : shape[1]);
        const std::size_t channels = static_cast<std::size_t>(channels_first ? shape[1] : shape[2]);
        if (channels != manifest_.content_dim || source_frames == 0) {
            throw std::runtime_error("ContentVec output does not match manifest content_dim");
        }

        const float *features = content_value.GetTensorData<float>();
        std::vector<float> source_features(source_frames * channels);
        for (std::size_t t = 0; t < source_frames; ++t) {
            for (std::size_t c = 0; c < channels; ++c) {
                source_features[t * channels + c] = channels_first
                                                         ? features[c * source_frames + t]
                                                         : features[t * channels + c];
            }
        }
        if (feature_index_ && index_rate > 0.0F) {
            feature_index_->blend(source_features.data(), source_frames, index_rate);
        }

        const std::size_t natural_frames = source_frames * 2;
        const std::size_t feature_frames = manifest_.synthesizer_frames != 0
                                               ? manifest_.synthesizer_frames
                                               : natural_frames;
        std::vector<float> phone(feature_frames * channels);
        for (std::size_t t = 0; t < feature_frames; ++t) {
            // ContentVec runs at half the RVC phone rate. Preserve that exact
            // repetition when the counts agree; otherwise interpolate into a
            // converter-declared fixed trace size.
            const std::size_t source_t = feature_frames == natural_frames
                                             ? std::min(t / 2, source_frames - 1)
                                             : std::min(t * source_frames / feature_frames,
                                                        source_frames - 1);
            for (std::size_t c = 0; c < channels; ++c) {
                phone[t * channels + c] = source_features[source_t * channels + c];
            }
        }

        std::vector<float> pitchf(feature_frames, 0.0F);
        if (manifest_.uses_f0) {
            avc::rvc::RmvpeMel mel = avc::rvc::RmvpeFrontend::logMel(audio16);
            const std::array<std::int64_t, 3> mel_shape{
                1, static_cast<std::int64_t>(avc::rvc::RmvpeFrontend::kMelBins),
                static_cast<std::int64_t>(mel.padded_frames)};
            Ort::Value mel_value = Ort::Value::CreateTensor<float>(
                memory, mel.values.data(), mel.values.size(), mel_shape.data(), mel_shape.size());
            const char *rmvpe_inputs[]{rmvpe_input_.c_str()};
            const char *rmvpe_outputs[]{rmvpe_output_.c_str()};
            auto f0_outputs = rmvpe_->Run(Ort::RunOptions{nullptr}, rmvpe_inputs, &mel_value, 1,
                                          rmvpe_outputs, 1);
            const std::vector<std::int64_t> f0_shape =
                f0_outputs.front().GetTensorTypeAndShapeInfo().GetShape();
            if (f0_shape.size() != 3 || f0_shape[0] != 1
                || f0_shape[2] != static_cast<std::int64_t>(avc::rvc::RmvpeFrontend::kPitchBins)
                || f0_shape[1] < static_cast<std::int64_t>(mel.frames)) {
                throw std::runtime_error("RMVPE output has an incompatible shape");
            }
            const std::vector<float> decoded = avc::rvc::RmvpeFrontend::decode(
                f0_outputs.front().GetTensorData<float>(), mel.frames);
            const float shift = std::pow(2.0F, pitch_shift / 12.0F);
            for (std::size_t t = 0; t < feature_frames && t < decoded.size(); ++t) {
                if (decoded[t] > 0.0F) pitchf[t] = decoded[t] * shift;
            }
        }
        std::vector<std::int64_t> pitch(feature_frames);
        std::ranges::transform(pitchf, pitch.begin(), [](float f0) { return coarsePitch(f0); });
        // Keep one deterministic stream for the lifetime of the runtime. Resetting
        // it for every inference imprints the same excitation on every cold
        // block, producing a strong block-period component in otherwise steady
        // audio.
        std::vector<float> noise(192 * feature_frames);
        std::ranges::generate(noise, [this] { return normal_(random_); });

        std::int64_t phone_length = static_cast<std::int64_t>(feature_frames);
        const std::array<std::int64_t, 3> phone_shape{
            1, phone_length, static_cast<std::int64_t>(channels)};
        const std::array<std::int64_t, 2> pitch_shape{1, phone_length};
        const std::array<std::int64_t, 3> noise_shape{1, 192, phone_length};
        const std::array<std::int64_t, 1> scalar_shape{1};

        std::vector<Ort::Value> inputs;
        inputs.reserve(6);
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memory, phone.data(), phone.size(), phone_shape.data(), phone_shape.size()));
        inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(
            memory, &phone_length, 1, scalar_shape.data(), scalar_shape.size()));
        inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(
            memory, pitch.data(), pitch.size(), pitch_shape.data(), pitch_shape.size()));
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memory, pitchf.data(), pitchf.size(), pitch_shape.data(), pitch_shape.size()));
        inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(
            memory, &speaker, 1, scalar_shape.data(), scalar_shape.size()));
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memory, noise.data(), noise.size(), noise_shape.data(), noise_shape.size()));

        std::vector<const char *> input_names;
        for (const std::string &name : synth_inputs_) input_names.push_back(name.c_str());
        const char *output_names[]{synth_output_.c_str()};
        auto outputs = synth_.Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(),
                                  inputs.size(), output_names, 1);
        const auto count = outputs.front().GetTensorTypeAndShapeInfo().GetElementCount();
        const float *generated = outputs.front().GetTensorData<float>();
        std::vector<float> converted =
            resample(generated, count, manifest_.model_sample_rate, graph_rate_);
        if (!graph_context_.empty()) {
            if (converted.size() >= frames) {
                return {converted.end() - static_cast<std::ptrdiff_t>(frames), converted.end()};
            }
            std::vector<float> padded(frames, 0.0F);
            std::copy(converted.begin(), converted.end(),
                      padded.end() - static_cast<std::ptrdiff_t>(converted.size()));
            return padded;
        }
        return converted;
    }

private:
    static Ort::SessionOptions makeOptions(int provider, int cuda_device)
    {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#if defined(AVC_RVC_ENABLE_CUDA)
        if (provider == 2) {
            OrtCUDAProviderOptions cuda_options{};
            cuda_options.device_id = cuda_device;
            options.AppendExecutionProvider_CUDA(cuda_options);
        }
#else
        (void)cuda_device;
        if (provider == 2) {
            throw std::runtime_error("this avc-rvc build has no CUDA execution provider");
        }
#endif
        return options;
    }

    avc::rvc::ModelManifest manifest_;
    std::uint32_t graph_rate_ = 0;
    Ort::SessionOptions options_;
    Ort::Session content_;
    Ort::Session synth_;
    std::unique_ptr<Ort::Session> rmvpe_;
    std::unique_ptr<avc::rvc::FeatureIndex> feature_index_;
    std::vector<std::string> content_inputs_;
    std::string content_output_;
    std::vector<std::string> synth_inputs_;
    std::string synth_output_;
    std::string rmvpe_input_;
    std::string rmvpe_output_;
    std::size_t context_graph_frames_ = 0;
    std::vector<float> graph_context_;
    std::mt19937 random_{42};
    std::normal_distribution<float> normal_{0.0F, 1.0F};
};

class RvcNode final : public avc::sdk::Node {
public:
    ~RvcNode() override = default;

    bool prepare(const AvcPrepareInfo &info, std::string &error) override
    {
        if (info.n_inputs != 1 || info.n_outputs != 1) {
            error = "RVC requires exactly one input and one output";
            return false;
        }
        sample_rate_ = info.sample_rate;
        max_quantum_ = info.max_quantum;

        const std::string selected = model_path_.empty() ? g_default_model : model_path_;
        if (selected.empty()) {
            error = "no model_path is configured";
            return false;
        }
        avc::rvc::ModelManifest manifest;
        if (!avc::rvc::ModelManifest::load(selected, manifest, error)) return false;

        runtime_.store(nullptr, std::memory_order_release);
        setState(AVC_NODE_LOADING, 0.05F, "loading " + manifest.name);
        const int provider = g_provider;
        const int cuda_device = g_cuda_device;
        loader_ = std::jthread([this, manifest = std::move(manifest), provider, cuda_device] {
            try {
                bool provider_fallback = false;
                bool cpu_runtime = false;
                std::shared_ptr<ModelRuntime> runtime;
#if defined(AVC_RVC_ENABLE_CUDA)
                if (provider == 0) {
                    try {
                        runtime = std::make_shared<ModelRuntime>(manifest, sample_rate_, 2,
                                                                 cuda_device);
                    } catch (...) {
                        provider_fallback = true;
                        cpu_runtime = true;
                        runtime = std::make_shared<ModelRuntime>(manifest, sample_rate_, 1,
                                                                 cuda_device);
                    }
                } else
#endif
                {
                    cpu_runtime = provider != 2;
                    runtime = std::make_shared<ModelRuntime>(manifest, sample_rate_, provider,
                                                             cuda_device);
                }
                // The rolling model window costs about 300 ms on the reference
                // CPU. A smaller cold-domain period repeatedly misses its
                // output deadline and is heard as gaps/noise, even though the
                // generated waveform itself is valid.
                const std::uint32_t recommended_cpu_block = (sample_rate_ + 1) / 2;
                const bool throughput_risk =
                    cpu_runtime && max_quantum_ < recommended_cpu_block;
                const bool degraded = runtime->degraded() || provider_fallback || throughput_risk;
                runtime_.store(std::move(runtime), std::memory_order_release);
                std::string message = "ready";
                if (throughput_risk) {
                    message = "ready on CPU; RVC domain block is too small, use at least "
                              + std::to_string(recommended_cpu_block) + " frames or CUDA";
                } else if (provider_fallback) {
                    message = "ready on CPU; CUDA initialization failed";
                } else if (cpu_runtime) {
                    message = "ready on CPU";
                }
                setState(degraded ? AVC_NODE_DEGRADED : AVC_NODE_READY, 1.0F,
                         std::move(message));
            } catch (const std::exception &ex) {
                failures_.fetch_add(1, std::memory_order_relaxed);
                setState(AVC_NODE_ERROR, 0.0F, ex.what());
            }
        });
        return true;
    }

    void setParam(std::uint32_t index, float value) noexcept override
    {
        switch (index) {
        case 0: enabled_.store(value >= 0.5F, std::memory_order_relaxed); break;
        case 1: pitch_shift_.store(std::clamp(value, -24.0F, 24.0F), std::memory_order_relaxed); break;
        case 2: wet_.store(std::clamp(value, 0.0F, 1.0F), std::memory_order_relaxed); break;
        case 3: speaker_.store(static_cast<std::int64_t>(std::max(0.0F, value)),
                               std::memory_order_relaxed); break;
        case 4: index_rate_.store(std::clamp(value, 0.0F, 1.0F),
                                  std::memory_order_relaxed); break;
        default: break;
        }
    }

    void setOption(std::uint32_t index, std::string_view value) override
    {
        if (index == 5) model_path_ = std::string(value);
    }

    bool status(AvcNodeStatus &out) const noexcept override
    {
        out.state = static_cast<AvcNodeState>(state_.load(std::memory_order_acquire));
        out.progress = progress_.load(std::memory_order_relaxed);
        out.processed_blocks = processed_.load(std::memory_order_relaxed);
        out.bypassed_blocks = bypassed_.load(std::memory_order_relaxed);
        out.failures = failures_.load(std::memory_order_relaxed);
        const std::lock_guard<std::mutex> lock(message_mutex_);
        std::snprintf(out.message, sizeof(out.message), "%s", message_.c_str());
        return true;
    }

    void process(const AvcProcessCtx &ctx) noexcept override
    {
        float *output = ctx.n_outputs > 0 ? ctx.outputs[0] : nullptr;
        const float *input = ctx.n_inputs > 0 ? ctx.inputs[0] : nullptr;
        if (output == nullptr) return;
        const auto dry = [&] {
            if (input != nullptr) std::copy_n(input, ctx.nframes, output);
            else std::fill_n(output, ctx.nframes, 0.0F);
            bypassed_.fetch_add(1, std::memory_order_relaxed);
        };

        std::shared_ptr<ModelRuntime> runtime = runtime_.load(std::memory_order_acquire);
        if (ctx.discontinuity != 0) {
            if (runtime != nullptr) runtime->reset();
        }
        if (!enabled_.load(std::memory_order_relaxed) || runtime == nullptr) {
            dry();
            return;
        }
        try {
            std::vector<float> converted = runtime->run(
                input, ctx.nframes, pitch_shift_.load(std::memory_order_relaxed),
                speaker_.load(std::memory_order_relaxed),
                index_rate_.load(std::memory_order_relaxed));
            if (converted.empty()) {
                dry();
                return;
            }
            if (converted.size() != ctx.nframes) {
                converted = resample(converted.data(), converted.size(),
                                     static_cast<std::uint32_t>(converted.size()), ctx.nframes);
            }
            const float wet = wet_.load(std::memory_order_relaxed);
            for (std::uint32_t i = 0; i < ctx.nframes; ++i) {
                const float dry_sample = input != nullptr ? input[i] : 0.0F;
                output[i] = dry_sample + (converted[i] - dry_sample) * wet;
            }
            processed_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception &ex) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            setState(AVC_NODE_ERROR, 0.0F, ex.what());
            runtime_.store(nullptr, std::memory_order_release);
            dry();
        } catch (...) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            setState(AVC_NODE_ERROR, 0.0F, "unknown inference failure");
            runtime_.store(nullptr, std::memory_order_release);
            dry();
        }
    }

private:
    void setState(AvcNodeState state, float progress, std::string message) noexcept
    {
        {
            const std::lock_guard<std::mutex> lock(message_mutex_);
            message_ = std::move(message);
        }
        progress_.store(progress, std::memory_order_relaxed);
        state_.store(state, std::memory_order_release);
    }

    std::uint32_t sample_rate_ = 48000;
    std::uint32_t max_quantum_ = 0;
    std::string model_path_;
    std::atomic<bool> enabled_{true};
    std::atomic<float> pitch_shift_{0.0F};
    std::atomic<float> wet_{1.0F};
    std::atomic<std::int64_t> speaker_{0};
    std::atomic<float> index_rate_{0.75F};
    std::atomic<AvcNodeState> state_{AVC_NODE_OFFLINE};
    std::atomic<float> progress_{0.0F};
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> bypassed_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::shared_ptr<ModelRuntime>> runtime_;
    mutable std::mutex message_mutex_;
    std::string message_ = "offline";
    std::jthread loader_;
};

avc::sdk::Plugin plugin("rvc", "RVC voice conversion", "0.1.0");

const char *const kEditorModule = R"RVCJS(
const text = (tag, value, className = '') => {
  const element = document.createElement(tag)
  element.textContent = value
  if (className) element.className = className
  return element
}

async function jsonResponse(response) {
  const body = await response.text()
  let value = null
  try { value = body ? JSON.parse(body) : null } catch { value = null }
  if (!response.ok) throw new Error(value?.error ?? body ?? `HTTP ${response.status}`)
  return value
}

class RvcSettings extends HTMLElement {
  connectedCallback() {
    this.timer = 0
    this.file = null
    this.indexFile = null
    this.state = null
    this.renderShell()
    void this.refresh()
  }

  disconnectedCallback() {
    if (this.timer) clearTimeout(this.timer)
  }

  field(labelText, input, hint = '') {
    const label = document.createElement('label')
    label.className = 'field'
    label.append(text('span', labelText, 'field__name'), input)
    if (hint) label.append(text('span', hint, 'hint'))
    return label
  }

  renderShell() {
    const context = this.avcContext
    const values = context.settings.get()
    const panel = document.createElement('section')
    panel.className = 'panel'
    panel.append(text('h2', 'RVC', 'panel__title'))

    this.defaultModel = document.createElement('input')
    this.defaultModel.value = values.default_model ?? ''
    this.defaultModel.placeholder = '/服务器上的路径/voice.avcrvc'
    panel.append(this.field('默认模型', this.defaultModel, '节点留空时使用。'))

    this.provider = document.createElement('select')
    for (const option of ['auto', 'cpu', 'cuda']) {
      const item = document.createElement('option')
      item.value = option
      item.textContent = option
      item.selected = option === (values.provider ?? 'auto')
      this.provider.append(item)
    }
    panel.append(this.field('执行提供程序', this.provider))

    this.cudaDevice = document.createElement('input')
    this.cudaDevice.type = 'number'
    this.cudaDevice.min = '0'
    this.cudaDevice.max = '15'
    this.cudaDevice.value = values.cuda_device ?? '0'
    panel.append(this.field('CUDA 设备', this.cudaDevice))

    const save = document.createElement('button')
    save.className = 'button button--primary'
    save.textContent = '保存并重启引擎'
    save.addEventListener('click', async () => {
      save.disabled = true
      this.say('保存中…')
      try {
        await context.settings.save({
          ...values,
          default_model: this.defaultModel.value.trim(),
          provider: this.provider.value,
          cuda_device: this.cudaDevice.value,
        })
        this.say('已保存')
      } catch (error) {
        this.say(error instanceof Error ? error.message : String(error), true)
        save.disabled = false
      }
    })
    panel.append(save)

    const divider = document.createElement('div')
    divider.className = 'perf__gap'
    panel.append(divider, text('h3', '导入 .pth', 'perf__heading'))

    this.nameInput = document.createElement('input')
    this.nameInput.placeholder = '模型名称'
    panel.append(this.field('名称', this.nameInput))

    this.checkpointInput = document.createElement('input')
    this.checkpointInput.type = 'file'
    this.checkpointInput.accept = '.pth'
    this.checkpointInput.setAttribute('aria-describedby', 'rvc-import-status')
    this.checkpointInput.addEventListener('change', () => {
      this.file = this.checkpointInput.files?.[0] ?? null
      this.checkpointInput.removeAttribute('aria-invalid')
      if (this.file && !this.nameInput.value) this.nameInput.value = this.file.name.replace(/\.pth$/i, '')
    })
    panel.append(this.field('模型文件（.pth）', this.checkpointInput,
      '仅限 RVC 推理模型。'))

    const chooseIndex = document.createElement('input')
    chooseIndex.type = 'file'
    chooseIndex.accept = '.index'
    chooseIndex.addEventListener('change', () => {
      this.indexFile = chooseIndex.files?.[0] ?? null
    })
    panel.append(this.field('特征索引（可选）', chooseIndex))

    this.importButton = document.createElement('button')
    this.importButton.className = 'button button--small'
    this.importButton.textContent = '转换并导入'
    this.importButton.addEventListener('click', () => void this.upload())
    panel.append(this.importButton)

    this.progressBox = document.createElement('div')
    this.modelsBox = document.createElement('div')
    panel.append(this.progressBox, this.modelsBox)

    this.status = text('p', '', 'hint')
    this.status.id = 'rvc-import-status'
    this.status.setAttribute('role', 'status')
    panel.append(this.status)
    this.replaceChildren(panel)
  }

  say(message, bad = false) {
    this.status.textContent = message
    this.status.className = bad ? 'banner banner--error' : 'hint'
    this.status.setAttribute('role', bad ? 'alert' : 'status')
  }

  async upload() {
    if (!this.file) {
      this.checkpointInput.setAttribute('aria-invalid', 'true')
      this.checkpointInput.focus()
      return this.say('请选择 .pth 模型。', true)
    }
    this.importButton.disabled = true
    this.say(this.indexFile ? '正在上传模型与索引…' : '正在上传模型…')
    try {
      const query = new URLSearchParams({ name: this.nameInput.value.trim() })
      const form = new FormData()
      form.append('checkpoint', this.file, this.file.name)
      if (this.indexFile) form.append('index', this.indexFile, this.indexFile.name)
      this.state = await jsonResponse(await fetch(`/api/rvc/models/import?${query}`, {
        method: 'POST', body: form,
      }))
      this.renderState()
      this.schedule()
    } catch (error) {
      this.say(error instanceof Error ? error.message : String(error), true)
      this.importButton.disabled = false
    }
  }

  async refresh() {
    try {
      this.state = await jsonResponse(await fetch('/api/rvc/models'))
      this.renderState()
      if (this.active()) this.schedule()
    } catch (error) {
      this.say(error instanceof Error ? error.message : String(error), true)
    }
  }

  schedule() {
    if (this.timer) clearTimeout(this.timer)
    this.timer = setTimeout(() => void this.refresh(), 750)
  }

  active() {
    return ['uploading', 'queued', 'converting', 'validating'].includes(this.state?.job?.state)
  }

  renderState() {
    this.importButton.disabled = !this.state?.available || this.active()
    this.progressBox.setAttribute('aria-busy', String(this.active()))
    this.progressBox.replaceChildren()
    const job = this.state?.job
    if (job) {
      const row = document.createElement('div')
      row.className = 'field'
      row.append(text('span', `${job.name} · ${job.message || job.state}`, 'field__name'))
      const progress = document.createElement('progress')
      progress.max = 1
      progress.value = Number(job.progress || 0)
      progress.setAttribute('aria-label', `${job.name} 转换进度`)
      row.append(progress)
      if (this.active()) {
        const cancel = document.createElement('button')
        cancel.className = 'button button--small'
        cancel.textContent = '取消'
        cancel.addEventListener('click', async () => {
          try {
            this.state = await jsonResponse(await fetch(`/api/rvc/models/import/${encodeURIComponent(job.id)}/cancel`, { method: 'POST' }))
            this.renderState()
          } catch (error) { this.say(error instanceof Error ? error.message : String(error), true) }
        })
        row.append(cancel)
      }
      if (job.error) row.append(text('span', job.error, 'banner banner--error'))
      this.progressBox.append(row)
    }

    this.modelsBox.replaceChildren(text('h3', '已导入模型', 'perf__heading'))
    const models = this.state?.models ?? []
    if (!models.length) this.modelsBox.append(text('p', '无模型', 'hint'))
    for (const model of models) {
      const card = document.createElement('div')
      card.className = 'extcard'
      const head = document.createElement('div')
      head.className = 'extcard__head'
      const retrieval = model.indexed ? ` · 索引 ${model.indexVectors} 条向量` : ' · 无索引'
      head.append(text('strong', model.name),
        text('span', `${model.version} · ${model.sampleRate} Hz${retrieval}`, 'hint'))
      const path = text('code', model.path, 'hint')
      const actions = document.createElement('div')
      actions.className = 'extcard__foot'
      const use = document.createElement('button')
      use.className = 'button button--small button--primary'
      use.textContent = '设为默认'
      use.addEventListener('click', () => {
        this.defaultModel.value = model.path
        this.say('已填入；保存后生效。')
      })
      const remove = document.createElement('button')
      remove.className = 'button button--small'
      remove.textContent = '删除'
      remove.addEventListener('click', async () => {
        if (!confirm(`删除“${model.name}”？`)) return
        try {
          this.state = await jsonResponse(await fetch(`/api/rvc/models/${encodeURIComponent(model.key)}`, { method: 'DELETE' }))
          this.renderState()
        } catch (error) { this.say(error instanceof Error ? error.message : String(error), true) }
      })
      actions.append(use, remove)
      card.append(head, path, actions)
      this.modelsBox.append(card)
    }
    if (!this.state?.available) this.say(this.state?.error || '转换器不可用。', true)
    else if (job?.state === 'ready') this.say('转换完成。')
    else if (job?.state === 'failed') this.say(job.error || '转换失败。', true)
  }
}

export function activate(api) {
  if (!customElements.get('avc-rvc-settings')) customElements.define('avc-rvc-settings', RvcSettings)
  api.components.registerSettings('avc-rvc-settings')
}
)RVCJS";

void configure(const char *key, const char *value)
{
    if (key == nullptr || value == nullptr) return;
    if (std::strcmp(key, "default_model") == 0) g_default_model = value;
    else if (std::strcmp(key, "provider") == 0) {
        g_provider = std::strcmp(value, "cuda") == 0 ? 2 : std::strcmp(value, "cpu") == 0 ? 1 : 0;
    } else if (std::strcmp(key, "cuda_device") == 0) {
        g_cuda_device = std::max(0, std::atoi(value));
    }
}

}

AVC_PLUGIN_MAIN(plugin)
{
    if (g_default_model.empty()) {
        g_default_model = plugin.dataDir() + "/models/default.avcrvc";
    }
    plugin.author("avc")
        .describe("RVC inference with offline PTH import.")
        .pathSetting("default_model", g_default_model, "Default model",
                      "Used when a node model path is empty.")
        .enumSetting("provider", {"auto", "cpu", "cuda"}, "auto", "Execution provider",
                      "Auto prefers CUDA, then CPU.")
        .intSetting("cuda_device", 0, 0, 15, "CUDA device")
        .onConfigure(&configure)
        .uiAsset("ui/main.js", kEditorModule, "text/javascript; charset=utf-8")
        .uiEntry("ui/main.js");
    plugin.node<RvcNode>(
        avc::sdk::NodeDesc("voice_conversion", "voice", "RVC voice conversion")
            .in("in")
            .out("out")
            .boolParam("enabled", true)
            .floatParam("pitch_shift", -24.0F, 24.0F, 0.0F, "st")
            .floatParam("wet", 0.0F, 1.0F, 1.0F)
            .floatParam("speaker", 0.0F, 255.0F, 0.0F)
            .floatParam("index_rate", 0.0F, 1.0F, 0.75F)
            .pathParam("model_path", {}, "Empty uses the extension default.")
            .notRealtimeSafe()
            .recommendedColdBlock(24000));
}
