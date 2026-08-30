#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace avc::rvc {

struct ModelManifest {
    std::filesystem::path root;
    std::filesystem::path contentvec;
    std::filesystem::path rmvpe;
    std::filesystem::path synthesizer;
    std::filesystem::path feature_index;
    std::uint32_t format_version = 0;
    std::uint32_t content_dim = 0;
    std::uint32_t model_sample_rate = 0;
    // Zero denotes a legacy dynamically-shaped synthesizer. The embedded PTH
    // converter writes the fixed frame count used by its trace.
    std::uint32_t synthesizer_frames = 0;
    // Size of the rolling ContentVec/RMVPE window at 16 kHz. Zero denotes a
    // legacy package which receives only the current graph block.
    std::uint32_t content_samples_16k = 0;
    bool uses_f0 = true;
    bool uses_index = false;
    std::string rvc_version;
    std::string name;

    static bool load(const std::filesystem::path &root, ModelManifest &out, std::string &error);
};

}
