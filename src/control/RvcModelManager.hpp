#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace avc::control {

class RvcModelManager {
public:
    struct RuntimePaths {
        std::filesystem::path converter;
        std::filesystem::path contentvec_v1;
        std::filesystem::path contentvec_v2;
        std::filesystem::path rmvpe;
    };

    struct Upload {
        std::string id;
        std::string name;
        std::filesystem::path directory;
        std::filesystem::path input;
        std::filesystem::path index_input;
        std::unique_ptr<std::ofstream> stream;
        std::unique_ptr<std::ofstream> index_stream;
        std::size_t bytes = 0;
        std::size_t index_bytes = 0;
    };

    explicit RvcModelManager(std::filesystem::path data_root,
                             RuntimePaths runtime = defaultRuntimePaths());
    ~RvcModelManager();

    RvcModelManager(const RvcModelManager &) = delete;
    RvcModelManager &operator=(const RvcModelManager &) = delete;

    std::optional<Upload> beginUpload(const std::string &filename,
                                      const std::string &requested_name,
                                      std::string &error);
    bool append(Upload &upload, const char *data, std::size_t size, std::string &error);
    bool beginIndex(Upload &upload, const std::string &filename, std::string &error);
    bool appendIndex(Upload &upload, const char *data, std::size_t size, std::string &error);
    bool commit(Upload &&upload, std::string &error);
    void abort(Upload &&upload) noexcept;

    nlohmann::json describe() const;
    bool cancel(const std::string &id, std::string &error);
    bool removeModel(const std::string &name, std::string &error);

    static RuntimePaths defaultRuntimePaths();
    static bool validSourceFilename(const std::string &filename);
    static bool validIndexFilename(const std::string &filename);
    static std::string safeModelName(const std::string &value);

private:
    enum class State { Uploading, Queued, Converting, Validating, Ready, Failed, Cancelled };

    struct Job {
        std::string id;
        std::string name;
        State state = State::Uploading;
        float progress = 0.0F;
        std::string message;
        std::string error;
        std::filesystem::path path;
    };

    static const char *stateName(State state) noexcept;
    bool runtimeAvailable(std::string &error) const;
    void convert(std::stop_token stop, Upload upload);
    int runConverter(const Upload &upload, const std::filesystem::path &output,
                     std::stop_token stop, std::string &error);
    void updateProgress(float progress, const std::string &message);
    void finish(State state, const std::string &message, const std::string &error,
                const std::filesystem::path &path = {});
    nlohmann::json models() const;

    std::filesystem::path data_root_;
    std::filesystem::path models_root_;
    std::filesystem::path imports_root_;
    RuntimePaths runtime_;

    mutable std::mutex mutex_;
    std::optional<Job> job_;
    std::jthread worker_;
    std::atomic<int> child_pid_{0};
    std::atomic<std::uint64_t> next_id_{1};
};

}
