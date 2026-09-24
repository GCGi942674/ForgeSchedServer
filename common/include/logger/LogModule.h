#ifndef FORGESCHED_LOG_MODULE_H
#define FORGESCHED_LOG_MODULE_H

#include <string>

namespace ForgeSched {

enum class LogModule {
    SERVER,
    NETWORK,
    TASK,
    SCHEDULER,
    WORKER,
    DATABASE
};

inline std::string toString(LogModule module) {
    switch (module) {
        case LogModule::SERVER:    return "SERVER";
        case LogModule::NETWORK:   return "NETWORK";
        case LogModule::TASK:      return "TASK";
        case LogModule::SCHEDULER: return "SCHEDULER";
                case LogModule::WORKER:    return "WORKER";
        case LogModule::DATABASE:  return "DATABASE";
        default: return "UNKNOWN";
    }
}

} // namespace ForgeSched

#endif // FORGESCHED_LOG_MODULE_H