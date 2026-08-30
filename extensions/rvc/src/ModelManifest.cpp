#include "avc_rvc/ModelManifest.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace avc::rvc {
namespace {

bool regular(const std::filesystem::path &path, const char *label, std::string &error)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = std::string(label) + " is missing: " + path.string();
        return false;
    }
    return true;
}

}

bool ModelManifest::load(const std::filesystem::path &root, ModelManifest &out,
                         std::string &error)
{
    const std::filesystem::path manifest_path = root / "manifest.json";
    std::ifstream input(manifest_path);
    if (!input) {
        error = "model manifest is missing: " + manifest_path.string();
        return false;
    }

    nlohmann::json json;
    try {
        input >> json;
    } catch (const std::exception &ex) {
        error = "cannot parse model manifest: " + std::string(ex.what());
        return false;
    }

    ModelManifest parsed;
    parsed.root = root;
    parsed.format_version = json.value("format_version", 0U);
    parsed.content_dim = json.value("content_dim", 0U);
    parsed.model_sample_rate =
        json.value("model_sample_rate", json.value("sample_rate", 0U));
    parsed.synthesizer_frames = json.value("synthesizer_frames", 0U);
    parsed.content_samples_16k = json.value("content_samples_16k", 0U);
    parsed.uses_f0 = json.value("uses_f0", true);
    parsed.uses_index = json.contains("feature_index");
    parsed.rvc_version = json.value("rvc_version", std::string{});
    parsed.name = json.value("name", root.filename().string());

    if (parsed.format_version != 1) {
        error = "unsupported model format_version " + std::to_string(parsed.format_version);
        return false;
    }
    if (parsed.content_dim != 256 && parsed.content_dim != 768) {
        error = "content_dim must be 256 (RVC v1) or 768 (RVC v2)";
        return false;
    }
    if (parsed.model_sample_rate < 16000 || parsed.model_sample_rate > 96000) {
        error = "model_sample_rate must be between 16000 and 96000";
        return false;
    }
    if (parsed.synthesizer_frames > 4096) {
        error = "synthesizer_frames must not exceed 4096";
        return false;
    }
    if (parsed.content_samples_16k > 262144) {
        error = "content_samples_16k must not exceed 262144";
        return false;
    }
    if (parsed.synthesizer_frames != 0 && parsed.content_samples_16k == 0
        && parsed.synthesizer_frames < 32) {
        error = "model package uses the obsolete short RVC trace; reimport its .pth";
        return false;
    }

    parsed.contentvec = root / json.value("contentvec", "contentvec.onnx");
    parsed.rmvpe = root / json.value("rmvpe", "rmvpe.onnx");
    parsed.synthesizer = root / json.value("synthesizer", "synthesizer.onnx");
    if (parsed.uses_index) {
        if (!json["feature_index"].is_string()) {
            error = "feature_index must name a file in the model package";
            return false;
        }
        parsed.feature_index = root / json.value("feature_index", "feature_index.avcidx");
    }
    if (!regular(parsed.contentvec, "ContentVec model", error)
        || (parsed.uses_f0 && !regular(parsed.rmvpe, "RMVPE model", error))
        || (parsed.uses_index && !regular(parsed.feature_index, "feature index", error))
        || !regular(parsed.synthesizer, "synthesizer model", error)) {
        return false;
    }

    out = std::move(parsed);
    return true;
}

}
