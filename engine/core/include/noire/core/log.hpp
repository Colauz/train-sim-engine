#pragma once

// Façade de journalisation. Le back-end (spdlog) reste confiné dans log.cpp :
// le reste du moteur ne dépend que de cet en-tête, et le formatage passe par
// std::format (C++20). Changer de back-end n'impacte aucun code appelant.

#include <format>
#include <string_view>
#include <utility>

namespace noire::log {

enum class Level { Trace, Debug, Info, Warn, Error, Critical };

// Cycle de vie + point d'entrée bas niveau (implémentés dans log.cpp).
void init();
void shutdown();
void log_message(Level level, std::string_view message);

// Surcouche ergonomique et type-safe, sans exposer le back-end.
template <typename... Args>
void write(Level level, std::format_string<Args...> fmt, Args&&... args) {
    log_message(level, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void trace(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Trace, fmt, std::forward<Args>(args)...);
}
template <typename... Args> void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, fmt, std::forward<Args>(args)...);
}
template <typename... Args> void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, fmt, std::forward<Args>(args)...);
}
template <typename... Args> void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, fmt, std::forward<Args>(args)...);
}
template <typename... Args> void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, fmt, std::forward<Args>(args)...);
}

}  // namespace noire::log
