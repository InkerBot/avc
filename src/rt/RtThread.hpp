#pragma once

#include <cstdint>
#include <string>

namespace avc::rt {

struct SchedInfo {
    int policy = -1;
    int priority = 0;
    std::int32_t tid = 0;
};

SchedInfo currentSchedInfo() noexcept;

bool isRealtime(const SchedInfo &info) noexcept;

std::string describe(const SchedInfo &info);

void enableFlushToZero() noexcept;

}
