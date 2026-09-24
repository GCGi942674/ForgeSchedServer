#ifndef FORGESCHED_LOG_LEVEL_H
#define FORGESCHED_LOG_LEVEL_H

#include <string>

namespace ForgeSched {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

inline std::string toString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
                default: return "UNKNOWN";
    }
}

} // namespace ForgeSched

#endif // FORGESCHED_LOG_LEVEL_H