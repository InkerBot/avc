#pragma once

#include <spdlog/spdlog.h>

#include <string_view>

namespace avc::log {

void init(std::string_view level);

}

// Realtime threads must never log: spdlog allocates, locks, and may write to a
// file descriptor. Any diagnostic from the audio callback goes through a counter
// that the control thread reads and prints.
