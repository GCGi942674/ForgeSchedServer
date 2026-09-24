#include "protocol/MessageType.h"
#include <unordered_map>

namespace ForgeSched::Protocol {

std::string toString(MessageType type) {
    switch (type) {
        case MessageType::SUBMIT_TASK:    return "submit_task";
        case MessageType::QUERY_TASK:     return "query_task";
        case MessageType::CANCEL_TASK:    return "cancel_task";
        case MessageType::WORKER_REGISTER: return "worker_register";
        case MessageType::WORKER_HEARTBEAT: return "worker_heartbeat";
        case MessageType::TASK_ASSIGN:    return "task_assign";
        case MessageType::TASK_START:     return "task_start";
        case MessageType::TASK_RESULT:    return "task_result";
        case MessageType::RESPONSE:       return "response";
        default:                          return "unknown";
    }
}

MessageType messageTypeFromString(const std::string& value) {
    static const std::unordered_map<std::string, MessageType> type_map = {
        {"submit_task", MessageType::SUBMIT_TASK},
        {"query_task", MessageType::QUERY_TASK},
        {"cancel_task", MessageType::CANCEL_TASK},
        {"worker_register", MessageType::WORKER_REGISTER},
        {"worker_heartbeat", MessageType::WORKER_HEARTBEAT},
        {"task_assign", MessageType::TASK_ASSIGN},
        {"task_start", MessageType::TASK_START},
        {"task_result", MessageType::TASK_RESULT},
        {"response", MessageType::RESPONSE}
    };

    auto it = type_map.find(value);
    if (it != type_map.end()) {
        return it->second;
    }
    return MessageType::UNKNOWN;
}

} // namespace ForgeSched::Protocol