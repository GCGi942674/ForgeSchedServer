#ifndef FORGESCHED_MESSAGE_TYPE_H
#define FORGESCHED_MESSAGE_TYPE_H

#include <string>

namespace ForgeSched::Protocol {

enum class MessageType {
    SUBMIT_TASK,
    QUERY_TASK,
    CANCEL_TASK,

    WORKER_REGISTER,
    WORKER_HEARTBEAT,

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
