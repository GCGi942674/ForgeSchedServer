#ifndef FORGESCHED_TASK_TYPE_H
#define FORGESCHED_TASK_TYPE_H

#include <string>

namespace ForgeSched {

enum class TaskType {
    REGRESSION,
    UNKNOWN
};

std::string toString(TaskType type);

} // namespace ForgeSched

#endif // FORGESCHED_TASK_TYPE_H
