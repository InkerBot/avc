#pragma once

#include <filesystem>

namespace avc::app {

void installCrashGuard(const std::filesystem::path &crash_file);

}
