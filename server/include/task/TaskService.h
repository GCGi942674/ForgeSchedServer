#ifndef FORGESCHED_TASK_SERVICE_H
#define FORGESCHED_TASK_SERVICE_H

#include "task/Task.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include "task/TaskIdGenerator.h"
#include "scheduler/Scheduler.h"
#include <optional>
#include <string>

namespace ForgeSched {

struct CreateTaskRequest {
    TaskType type;
    std::string target;
    std::string revision;
    int priority{0};
};

class TaskService {
public:
    explicit TaskService(Scheduler& scheduler);

    std::optional<TaskId> createTask(const CreateTaskRequest& request);
    std::optional<Task> getTask(TaskId id) const;
    bool cancelTask(TaskId id);

    TaskService(const TaskService&) = delete;
    TaskService& operator=(const TaskService&) = delete;

private:
    bool isPriorityValid(int priority) const;

    Scheduler& scheduler_;
    TaskIdGenerator id_generator_;
};

} // namespace ForgeSched

#endif // FORGESCHED_TASK_SERVICE_H