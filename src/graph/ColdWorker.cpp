#include "graph/ColdWorker.hpp"

#include "graph/CompiledGraph.hpp"
#include "rt/RtThread.hpp"

#include <chrono>

namespace avc::graph {

ColdWorker::~ColdWorker()
{
    stop();
}

void ColdWorker::start(CompiledGraph *graph, std::uint32_t domain, std::uint32_t poll_us)
{
    if (thread_.joinable()) {
        return;
    }
    graph_ = graph;
    domain_ = domain;
    poll_us_ = poll_us > 0 ? poll_us : 1;
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
}

void ColdWorker::stop() noexcept
{
    if (!thread_.joinable()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true, std::memory_order_relaxed);
    }
    cv_.notify_all();
    thread_.join();
}

void ColdWorker::run()
{
    // Denormals cost the same here as they do on the audio thread, and a cold
    // domain is exactly where a long reverb tail is going to decay into them.
    rt::enableFlushToZero();

    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    while (true) {
        while (graph_->runColdPass(domain_)) {
            if (stop_.load(std::memory_order_relaxed)) {
                return;
            }
        }
        lock.lock();
        cv_.wait_for(lock, std::chrono::microseconds(poll_us_),
                     [this] { return stop_.load(std::memory_order_relaxed); });
        const bool stopping = stop_.load(std::memory_order_relaxed);
        lock.unlock();
        if (stopping) {
            return;
        }
    }
}

}
