#ifndef FORGESCHED_TASK_STATUS_H
#define FORGESCHED_TASK_STATUS_H

#include <string>

namespace ForgeSched {

enum class TaskStatus {
    PENDING,
    QUEUED,
    ASSIGNED,
    RUNNING,
    SUCCEEDED,
    FAILED,
    TIMEOUT,
    CANCELLED
};

std::string toString(TaskStatus status);

} // namespace ForgeSched

#endif // FORGESCHED_TASK_STATUS_H
