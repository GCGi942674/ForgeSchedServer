#include "protocol/MessageType.h"
#include <unordered_map>

namespace ForgeSched::Protocol {

std::string toString(MessageType type) {
    switch (type) {
        case MessageType::SUBMIT_TASK:    return "submit_task";
        case MessageType::QUERY_TASK:     return "query_task";
        case MessageType::QUERY_STATUS:   return "query_status";
        case MessageType::CANCEL_TASK:    return "cancel_task";
        case MessageType::QUERY_BATCH:    return "query_batch";
        case MessageType::CANCEL_BATCH:   return "cancel_batch";
        case MessageType::WORKER_REGISTER: return "worker_register";
        case MessageType::WORKER_HEARTBEAT: return "worker_heartbeat";
        case MessageType::WORKER_RECOVER: return "worker_recover";
        case MessageType::WORKER_DRAIN: return "worker_drain";
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
        {"query_status", MessageType::QUERY_STATUS},
        {"cancel_task", MessageType::CANCEL_TASK},
        {"query_batch", MessageType::QUERY_BATCH},
        {"cancel_batch", MessageType::CANCEL_BATCH},
        {"worker_register", MessageType::WORKER_REGISTER},
        {"worker_heartbeat", MessageType::WORKER_HEARTBEAT},
        {"worker_recover", MessageType::WORKER_RECOVER},
        {"worker_drain", MessageType::WORKER_DRAIN},
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
