#include "control/RvcModelManager.hpp"

#include "log/Log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace avc::control {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxCheckpointBytes = 512U * 1024U * 1024U;
constexpr std::size_t kMaxIndexBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxNameLength = 64;

bool regular(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path envPath(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::filesystem::path(value)
                                                 : std::filesystem::path{};
}

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return value;
}

}

RvcModelManager::RvcModelManager(std::filesystem::path data_root, RuntimePaths runtime)
    : data_root_(std::move(data_root)), models_root_(data_root_ / "models"),
      imports_root_(data_root_ / ".imports"), runtime_(std::move(runtime))
{
    std::error_code ec;
    std::filesystem::create_directories(models_root_, ec);
    std::filesystem::create_directories(imports_root_, ec);
}

RvcModelManager::~RvcModelManager()
{
    if (worker_.joinable()) {
        worker_.request_stop();
#ifndef _WIN32
        const int pid = child_pid_.load(std::memory_order_relaxed);
        if (pid > 0) ::kill(pid, SIGTERM);
#endif
        worker_.join();
    }
}

RvcModelManager::RuntimePaths RvcModelManager::defaultRuntimePaths()
{
    std::filesystem::path root = envPath("AVC_RVC_CONVERTER_ROOT");
    std::error_code ec;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", ec);
#if defined(AVC_RVC_BUILD_CONVERTER_ROOT)
    // Do not let an installed binary accidentally keep using a converter left
    // in the build tree on the packaging machine.
    const std::filesystem::path build_root = AVC_RVC_BUILD_CONVERTER_ROOT;
    const std::filesystem::path build_bin = AVC_RVC_BUILD_BINARY_DIR;
    if (root.empty() && !ec && executable.parent_path() == build_bin
        && regular(build_root / "avc-rvc-convert")) {
        root = build_root;
    }
#endif
#if defined(AVC_RVC_INSTALL_LIBEXECDIR)
    if (root.empty() && !ec) {
        const std::filesystem::path relative =
            executable.parent_path().parent_path() / AVC_RVC_INSTALL_LIBEXECDIR
            / "avc" / "rvc-converter";
        if (regular(relative / "avc-rvc-convert")) root = relative;
    }
#endif
#if defined(AVC_RVC_INSTALLED_CONVERTER_ROOT)
    if (root.empty()) root = AVC_RVC_INSTALLED_CONVERTER_ROOT;
#endif
    return {
        root / "avc-rvc-convert",
        root / "base" / "contentvec-v1.onnx",
        root / "base" / "contentvec-v2.onnx",
        root / "base" / "rmvpe.onnx",
    };
}

bool RvcModelManager::validSourceFilename(const std::string &filename)
{
    if (filename.empty() || filename.size() > 255
        || !std::filesystem::path(filename).extension().string().ends_with(".pth")) {
        return false;
    }
    return filename.find('/') == std::string::npos && filename.find('\\') == std::string::npos
           && filename != ".pth" && filename.front() != '.';
}

bool RvcModelManager::validIndexFilename(const std::string &filename)
{
    return !filename.empty() && filename.size() <= 255
           && std::filesystem::path(filename).extension() == ".index"
           && filename.find('/') == std::string::npos
           && filename.find('\\') == std::string::npos && filename != ".index"
           && filename.front() != '.';
}

std::string RvcModelManager::safeModelName(const std::string &value)
{
    std::string source = std::filesystem::path(value).stem().string();
    std::string out;
    out.reserve(std::min(source.size(), kMaxNameLength));
    for (const unsigned char c : source) {
        if (out.size() >= kMaxNameLength) break;
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                          || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(safe ? static_cast<char>(c) : '_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "voice" : out;
}

const char *RvcModelManager::stateName(State state) noexcept
{
    switch (state) {
    case State::Uploading:  return "uploading";
    case State::Queued:     return "queued";
    case State::Converting: return "converting";
    case State::Validating: return "validating";
    case State::Ready:      return "ready";
    case State::Failed:     return "failed";
    case State::Cancelled:  return "cancelled";
    }
    return "failed";
}

bool RvcModelManager::runtimeAvailable(std::string &error) const
{
    const std::array<std::pair<const char *, const std::filesystem::path *>, 4> required{{
        {"converter", &runtime_.converter},
        {"RVC v1 ContentVec", &runtime_.contentvec_v1},
        {"RVC v2 ContentVec", &runtime_.contentvec_v2},
        {"RMVPE", &runtime_.rmvpe},
    }};
    for (const auto &[label, path] : required) {
        if (!regular(*path)) {
            error = std::string(label) + " is not installed: " + path->string();
            return false;
        }
    }
    return true;
}

std::optional<RvcModelManager::Upload>
RvcModelManager::beginUpload(const std::string &filename, const std::string &requested_name,
                             std::string &error)
{
    if (!validSourceFilename(filename)) {
        error = "expected a plain .pth inference checkpoint filename";
        return std::nullopt;
    }
    if (!runtimeAvailable(error)) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    if (job_ && (job_->state == State::Uploading || job_->state == State::Queued
                 || job_->state == State::Converting || job_->state == State::Validating)) {
        error = "another RVC model import is already running";
        return std::nullopt;
    }

    Upload upload;
    upload.id = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count())
                + "-" + std::to_string(next_id_.fetch_add(1));
    upload.name = safeModelName(requested_name.empty() ? filename : requested_name);
    const std::filesystem::path target = models_root_ / (upload.name + ".avcrvc");
    if (std::filesystem::exists(target)) {
        error = "a model named '" + upload.name + "' already exists";
        return std::nullopt;
    }

    upload.directory = imports_root_ / upload.id;
    upload.input = upload.directory / "source.pth";
    std::error_code ec;
    std::filesystem::create_directories(upload.directory, ec);
    if (ec) {
        error = "cannot create import directory: " + ec.message();
        return std::nullopt;
    }
    upload.stream = std::make_unique<std::ofstream>(upload.input, std::ios::binary | std::ios::trunc);
    if (!*upload.stream) {
        error = "cannot store the uploaded checkpoint";
        std::filesystem::remove_all(upload.directory, ec);
        return std::nullopt;
    }

    job_ = Job{upload.id, upload.name, State::Uploading, 0.02F, "uploading checkpoint"};
    return upload;
}

bool RvcModelManager::append(Upload &upload, const char *data, std::size_t size,
                             std::string &error)
{
    if (!upload.stream || !*upload.stream) {
        error = "the upload is no longer open";
        return false;
    }
    if (size > kMaxCheckpointBytes - std::min(upload.bytes, kMaxCheckpointBytes)) {
        error = "an RVC checkpoint may be at most 512 MiB";
        return false;
    }
    upload.stream->write(data, static_cast<std::streamsize>(size));
    if (!*upload.stream) {
        error = "cannot write the uploaded checkpoint";
        return false;
    }
    upload.bytes += size;
    return true;
}

bool RvcModelManager::beginIndex(Upload &upload, const std::string &filename,
                                 std::string &error)
{
    if (!validIndexFilename(filename)) {
        error = "expected a plain .index feature filename";
        return false;
    }
    if (upload.index_stream) {
        error = "only one feature index may be uploaded";
        return false;
    }
    upload.index_input = upload.directory / "source.index";
    upload.index_stream =
        std::make_unique<std::ofstream>(upload.index_input, std::ios::binary | std::ios::trunc);
    if (!*upload.index_stream) {
        upload.index_stream.reset();
        error = "cannot store the uploaded feature index";
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (job_ && job_->id == upload.id && job_->state == State::Uploading) {
            job_->message = "uploading feature index";
        }
    }
    return true;
}

bool RvcModelManager::appendIndex(Upload &upload, const char *data, std::size_t size,
                                  std::string &error)
{
    if (!upload.index_stream || !*upload.index_stream) {
        error = "the feature-index upload is not open";
        return false;
    }
    if (size > kMaxIndexBytes - std::min(upload.index_bytes, kMaxIndexBytes)) {
        error = "an RVC feature index may be at most 2 GiB";
        return false;
    }
    upload.index_stream->write(data, static_cast<std::streamsize>(size));
    if (!*upload.index_stream) {
        error = "cannot write the uploaded feature index";
        return false;
    }
    upload.index_bytes += size;
    return true;
}

bool RvcModelManager::commit(Upload &&upload, std::string &error)
{
    if (!upload.stream || upload.bytes == 0) {
        error = "the checkpoint is empty";
        abort(std::move(upload));
        return false;
    }
    upload.stream->close();
    upload.stream.reset();
    if (upload.index_stream) {
        if (upload.index_bytes == 0) {
            error = "the feature index is empty";
            abort(std::move(upload));
            return false;
        }
        upload.index_stream->close();
        upload.index_stream.reset();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!job_ || job_->id != upload.id || job_->state != State::Uploading) {
            error = "the upload is no longer current";
            return false;
        }
        job_->state = State::Queued;
        job_->progress = 0.05F;
        job_->message = "queued for conversion";
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, upload = std::move(upload)](std::stop_token stop) mutable {
        convert(stop, std::move(upload));
    });
    return true;
}

void RvcModelManager::abort(Upload &&upload) noexcept
{
    if (upload.stream) upload.stream->close();
    if (upload.index_stream) upload.index_stream->close();
    std::error_code ec;
    std::filesystem::remove_all(upload.directory, ec);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (job_ && job_->id == upload.id && job_->state == State::Uploading) job_.reset();
}

void RvcModelManager::updateProgress(float progress, const std::string &message)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!job_) return;
    job_->progress = std::clamp(progress, job_->progress, 0.95F);
    if (!message.empty()) job_->message = message;
}

void RvcModelManager::finish(State state, const std::string &message, const std::string &error,
                             const std::filesystem::path &path)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!job_) return;
    job_->state = state;
    job_->progress = (state == State::Ready) ? 1.0F : job_->progress;
    job_->message = message;
    job_->error = error;
    job_->path = path;
}

void RvcModelManager::convert(std::stop_token stop, Upload upload)
{
    updateProgress(0.08F, "starting isolated converter");
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (job_) job_->state = State::Converting;
    }

    const std::filesystem::path output = upload.directory / "model.avcrvc";
    std::string error;
    const int code = runConverter(upload, output, stop, error);
    child_pid_.store(0, std::memory_order_relaxed);
    if (stop.stop_requested() || code == 130) {
        finish(State::Cancelled, "conversion cancelled", {});
    } else if (code != 0) {
        if (error.empty()) error = "converter exited with status " + std::to_string(code);
        finish(State::Failed, "conversion failed", trim(error));
    } else {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (job_) job_->state = State::Validating;
        }
        updateProgress(0.96F, "installing model package");
        const std::filesystem::path manifest_path = output / "manifest.json";
        std::ifstream manifest_file(manifest_path);
        const json manifest = json::parse(manifest_file, nullptr, false);
        const bool shaped = manifest.is_object() && manifest.value("format_version", 0) == 1
                            && regular(output / "synthesizer.onnx")
                            && regular(output / "contentvec.onnx") && regular(output / "rmvpe.onnx")
                            && (!manifest.contains("feature_index")
                                || regular(output / manifest.value("feature_index", "")));
        if (!shaped) {
            finish(State::Failed, "conversion failed", "converter produced an invalid model package");
        } else {
            const std::filesystem::path target = models_root_ / (upload.name + ".avcrvc");
            std::error_code ec;
            std::filesystem::rename(output, target, ec);
            if (ec) {
                finish(State::Failed, "conversion failed", "cannot install model: " + ec.message());
            } else {
                finish(State::Ready, "model is ready", {}, target);
                spdlog::info("imported RVC model {}", target.string());
            }
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(upload.directory, ec);
}

int RvcModelManager::runConverter(const Upload &upload, const std::filesystem::path &output,
                                  std::stop_token stop, std::string &error)
{
#ifdef _WIN32
    (void)upload;
    (void)output;
    (void)stop;
    error = "the bundled RVC converter currently supports Linux only";
    return 1;
#else
    int pipefd[2]{-1, -1};
    if (::pipe2(pipefd, O_CLOEXEC) != 0) {
        error = std::string("cannot create converter pipe: ") + std::strerror(errno);
        return 1;
    }

    std::vector<std::string> storage{
        runtime_.converter.string(), "--input", upload.input.string(), "--output", output.string(),
        "--name", upload.name, "--contentvec-v1", runtime_.contentvec_v1.string(),
        "--contentvec-v2", runtime_.contentvec_v2.string(), "--rmvpe", runtime_.rmvpe.string(),
    };
    if (!upload.index_input.empty()) {
        storage.push_back("--index");
        storage.push_back(upload.index_input.string());
    }
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (std::string &item : storage) argv.push_back(item.data());
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("cannot fork converter: ") + std::strerror(errno);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return 1;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        ::chdir(upload.directory.c_str());
        ::setenv("PYTHONNOUSERSITE", "1", 1);
        ::setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
        ::setenv("OMP_NUM_THREADS", "1", 1);
        ::setenv("MKL_NUM_THREADS", "1", 1);
        ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        // Best effort: unprivileged network namespaces are disabled on some
        // distributions. The converter never needs the network either way.
        ::unshare(CLONE_NEWNET);
        const rlimit cpu{600, 600};
        const rlimit files{64, 64};
        const rlimit output_limit{3ULL * 1024ULL * 1024ULL * 1024ULL,
                                  3ULL * 1024ULL * 1024ULL * 1024ULL};
        ::setrlimit(RLIMIT_CPU, &cpu);
        ::setrlimit(RLIMIT_NOFILE, &files);
        ::setrlimit(RLIMIT_FSIZE, &output_limit);
        ::execv(argv[0], argv.data());
        const std::string message = std::string("cannot execute converter: ") + std::strerror(errno) + "\n";
        ::write(STDERR_FILENO, message.data(), message.size());
        _exit(127);
    }

    child_pid_.store(static_cast<int>(pid), std::memory_order_relaxed);
    ::close(pipefd[1]);
    std::string pending;
    std::array<char, 4096> buffer{};
    while (true) {
        if (stop.stop_requested()) ::kill(pid, SIGTERM);
        const ssize_t count = ::read(pipefd[0], buffer.data(), buffer.size());
        if (count > 0) {
            pending.append(buffer.data(), static_cast<std::size_t>(count));
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                const json event = json::parse(line, nullptr, false);
                if (event.is_object() && event.contains("progress")) {
                    updateProgress(event.value("progress", 0.1F), event.value("message", std::string{}));
                } else if (!line.empty()) {
                    error = std::move(line);
                }
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    ::close(pipefd[0]);
    if (!pending.empty()) error = trim(pending);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM) return 130;
    return 1;
#endif
}

nlohmann::json RvcModelManager::models() const
{
    json out = json::array();
    std::error_code ec;
    std::vector<std::filesystem::path> entries;
    for (const auto &entry : std::filesystem::directory_iterator(models_root_, ec)) {
        if (entry.is_directory() && entry.path().extension() == ".avcrvc") entries.push_back(entry.path());
    }
    std::sort(entries.begin(), entries.end());
    for (const std::filesystem::path &path : entries) {
        std::ifstream file(path / "manifest.json");
        const json manifest = json::parse(file, nullptr, false);
        if (!manifest.is_object()) continue;
        const json retrieval = manifest.value("retrieval", json::object());
        const std::uint64_t index_vectors = retrieval.is_object()
                                                ? retrieval.value("vectors", std::uint64_t{0})
                                                : 0;
        out.push_back({
            {"name", manifest.value("name", path.stem().string())},
            {"key", path.stem().string()},
            {"path", path.string()},
            {"version", manifest.value("rvc_version", std::string{})},
            {"sampleRate", manifest.value("model_sample_rate", 0)},
            {"speakers", manifest.value("speakers", 1)},
            {"indexed", manifest.contains("feature_index")},
            {"indexVectors", index_vectors},
        });
    }
    return out;
}

nlohmann::json RvcModelManager::describe() const
{
    std::string runtime_error;
    const bool available = runtimeAvailable(runtime_error);
    json out{{"available", available}, {"error", runtime_error}, {"models", models()}};
    const std::lock_guard<std::mutex> lock(mutex_);
    if (job_) {
        out["job"] = {
            {"id", job_->id}, {"name", job_->name}, {"state", stateName(job_->state)},
            {"progress", job_->progress}, {"message", job_->message}, {"error", job_->error},
            {"path", job_->path.string()},
        };
    } else {
        out["job"] = nullptr;
    }
    return out;
}

bool RvcModelManager::cancel(const std::string &id, std::string &error)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!job_ || job_->id != id) {
            error = "no such RVC import";
            return false;
        }
        if (job_->state != State::Uploading && job_->state != State::Queued
            && job_->state != State::Converting && job_->state != State::Validating) {
            error = "that import has already finished";
            return false;
        }
        job_->message = "cancelling";
    }
    if (worker_.joinable()) worker_.request_stop();
#ifndef _WIN32
    const int pid = child_pid_.load(std::memory_order_relaxed);
    if (pid > 0) ::kill(pid, SIGTERM);
#endif
    return true;
}

bool RvcModelManager::removeModel(const std::string &name, std::string &error)
{
    if (safeModelName(name) != name || name.empty()) {
        error = "invalid RVC model name";
        return false;
    }
    const std::filesystem::path target = models_root_ / (name + ".avcrvc");
    std::error_code ec;
    if (!std::filesystem::is_directory(target, ec)) {
        error = "no RVC model named '" + name + "'";
        return false;
    }
    if (!std::filesystem::remove_all(target, ec) || ec) {
        error = "cannot remove model: " + ec.message();
        return false;
    }
    return true;
}

}
