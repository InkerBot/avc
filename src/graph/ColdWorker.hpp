#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace avc::graph {

class CompiledGraph;

class ColdWorker {
public:
    ColdWorker() = default;
    ~ColdWorker();

    ColdWorker(const ColdWorker &) = delete;
    ColdWorker &operator=(const ColdWorker &) = delete;

    void start(CompiledGraph *graph, std::uint32_t domain, std::uint32_t poll_us);

    void stop() noexcept;

    bool running() const noexcept { return thread_.joinable(); }

private:
    void run();

    CompiledGraph *graph_ = nullptr;
    std::uint32_t domain_ = 0;
    std::uint32_t poll_us_ = 1000;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}
