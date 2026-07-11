#include "noire/core/log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace noire::log {
namespace {

spdlog::level::level_enum to_spdlog(Level level) {
    switch (level) {
        case Level::Trace:    return spdlog::level::trace;
        case Level::Debug:    return spdlog::level::debug;
        case Level::Info:     return spdlog::level::info;
        case Level::Warn:     return spdlog::level::warn;
        case Level::Error:    return spdlog::level::err;
        case Level::Critical: return spdlog::level::critical;
    }
    return spdlog::level::info;
}

}  // namespace

void init() {
    auto logger = spdlog::stdout_color_mt("noire");
    logger->set_pattern("[%H:%M:%S.%e] [%^%-8l%$] %v");
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);
}

void shutdown() { spdlog::shutdown(); }

void log_message(Level level, std::string_view message) {
    spdlog::log(to_spdlog(level), "{}", message);
}

}  // namespace noire::log
