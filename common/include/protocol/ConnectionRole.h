#pragma once
#include "protocol/MessageType.h"
namespace ForgeSched::Protocol {
enum class ConnectionRole { UNKNOWN, CLIENT, WORKER };

constexpr bool isClientRequest(MessageType type) {
    return type == MessageType::SUBMIT_TASK || type == MessageType::QUERY_TASK ||
           type == MessageType::CANCEL_TASK;
}
constexpr bool isWorkerRequest(MessageType type) {
    return type == MessageType::WORKER_REGISTER || type == MessageType::WORKER_HEARTBEAT ||
           type == MessageType::TASK_START || type == MessageType::TASK_RESULT;
}
// Single inbound permission policy. Roles are protocol separation, not authentication.
constexpr bool isAllowed(ConnectionRole role, MessageType type) {
    switch (role) {
        case ConnectionRole::UNKNOWN:
            return isClientRequest(type) || type == MessageType::WORKER_REGISTER;
        case ConnectionRole::CLIENT: return isClientRequest(type);
        case ConnectionRole::WORKER: return isWorkerRequest(type);
    }
    return false;
}
} // namespace ForgeSched::Protocol
