#ifndef FORGESCHED_LOG_MACROS_H
#define FORGESCHED_LOG_MACROS_H

#include "Logger.h"

#define LOG_DEBUG(module, message) \
    ForgeSched::Logger::instance().log(ForgeSched::LogLevel::DEBUG, module, __FILE__, __LINE__, message)

#define LOG_INFO(module, message) \
    ForgeSched::Logger::instance().log(ForgeSched::LogLevel::INFO, module, __FILE__, __LINE__, message)

#define LOG_WARN(module, message) \
    ForgeSched::Logger::instance().log(ForgeSched::LogLevel::WARN, module, __FILE__, __LINE__, message)

#define LOG_ERROR(module, message) \
    ForgeSched::Logger::instance().log(ForgeSched::LogLevel::ERROR, module, __FILE__, __LINE__, message)

#define LOG_FATAL(module, message) \
    ForgeSched::Logger::instance().log(ForgeSched::LogLevel::FATAL, module, __FILE__, __LINE__, message)

#endif // FORGESCHED_LOG_MACROS_H
