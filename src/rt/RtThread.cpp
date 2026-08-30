#include "rt/RtThread.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#include <xmmintrin.h>
#include <pmmintrin.h>

namespace avc::rt {

SchedInfo currentSchedInfo() noexcept
{
    SchedInfo info;
#ifdef _WIN32
    info.tid = static_cast<std::int32_t>(GetCurrentThreadId());
    info.priority = GetThreadPriority(GetCurrentThread());
    info.policy = info.priority >= THREAD_PRIORITY_HIGHEST ? 1 : 0;
#else
    info.tid = static_cast<std::int32_t>(gettid());
    info.policy = sched_getscheduler(0);

    sched_param param{};
    if (sched_getparam(0, &param) == 0) {
        info.priority = param.sched_priority;
    }
#endif
    return info;
}

namespace {

int basePolicy(int policy) noexcept
{
#ifdef _WIN32
    return policy;
#else
    return policy & ~SCHED_RESET_ON_FORK;
#endif
}

}

bool isRealtime(const SchedInfo &info) noexcept
{
    const int policy = basePolicy(info.policy);
#ifdef _WIN32
    return policy == 1;
#else
    return policy == SCHED_FIFO || policy == SCHED_RR;
#endif
}

std::string describe(const SchedInfo &info)
{
    const char *policy = "unknown";
#ifdef _WIN32
    policy = basePolicy(info.policy) == 1 ? "MMCSS/time-critical" : "normal";
#else
    switch (basePolicy(info.policy)) {
    case SCHED_OTHER: policy = "SCHED_OTHER"; break;
    case SCHED_FIFO:  policy = "SCHED_FIFO";  break;
    case SCHED_RR:    policy = "SCHED_RR";    break;
    default: break;
    }
#endif

    std::string out = std::string(policy) + " prio " + std::to_string(info.priority);
#ifndef _WIN32
    if ((info.policy & SCHED_RESET_ON_FORK) != 0) {
        out += " (reset-on-fork)";
    }
#endif
    return out + " raw " + std::to_string(info.policy)
           + " tid " + std::to_string(info.tid);
}

void enableFlushToZero() noexcept
{
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

}
