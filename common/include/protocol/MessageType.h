#ifndef FORGESCHED_MESSAGE_TYPE_H
#define FORGESCHED_MESSAGE_TYPE_H

#include <string>

namespace ForgeSched::Protocol {

enum class MessageType {
    SUBMIT_TASK,
    QUERY_TASK,
    QUERY_STATUS,
    QUERY_TASKS,
    QUERY_WORKERS,
    QUERY_CAPABILITIES,
    CANCEL_TASK,
    QUERY_BATCH,
    CANCEL_BATCH,
    QUERY_REGRESSION,
    QUERY_PROGRESS,
    QUERY_ROLLBACK,

    WORKER_REGISTER,
    WORKER_HEARTBEAT,
    WORKER_RECOVER,
    WORKER_DRAIN,

    TASK_ASSIGN,
    TASK_START,
    TASK_RESULT,

    RESPONSE,

    UNKNOWN
};

std::string toString(MessageType type);
MessageType messageTypeFromString(const std::string& value);

} // namespace ForgeSched::Protocol

#endif // FORGESCHED_MESSAGE_TYPE_H
