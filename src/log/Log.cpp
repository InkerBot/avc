#include "log/Log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <string>

namespace avc::log {

void init(std::string_view level)
{
    // Callable more than once: the level is known only after argv is parsed, but
    // parse errors already need a logger.
    static std::shared_ptr<spdlog::logger> logger;
    if (!logger) {
        logger = spdlog::stderr_color_mt("avc");
        logger->set_pattern("%H:%M:%S.%e %^%-5l%$ %v");
        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::warn);
    }
    logger->set_level(spdlog::level::from_str(std::string(level)));
}

}
