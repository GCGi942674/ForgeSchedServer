#include "task/TaskService.h"
#include "task/Task.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include "Logging.h"

namespace ForgeSched {

TaskService::TaskService(Scheduler& scheduler)
    : scheduler_(scheduler)
{
}

bool TaskService::isPriorityValid(int priority) const {
    return priority >= -100 && priority <= 100;
}

std::optional<TaskId> TaskService::createTask(const CreateTaskRequest& request) {
    if (request.type == TaskType::UNKNOWN) {
        LOG_WARN(LogModule::TASK, "Invalid create request: unknown task type");
        return std::nullopt;
    }

    if (request.target.empty()) {
        LOG_WARN(LogModule::TASK, "Invalid create request: empty target");
        return std::nullopt;
    }

    if (request.revision.empty()) {
        LOG_WARN(LogModule::TASK, "Invalid create request: empty revision");
        return std::nullopt;
    }

    if (!isPriorityValid(request.priority)) {
        LOG_WARN(LogModule::TASK, "Invalid create request: priority out of range");
        return std::nullopt;
    }

    TaskId id = id_generator_.next();

    Task task(id, request.type, request.target, request.revision);
    task.setPriority(request.priority);

    if (!scheduler_.submitTask(std::move(task))) {
        LOG_ERROR(LogModule::TASK, "Generated task could not be submitted to Scheduler");
        return std::nullopt;
    }

    LOG_INFO(LogModule::TASK, "task=" + std::to_string(id) + " created type=" + toString(request.type) + " target=" + request.target + " revision=" + request.revision);

    return id;
}

std::optional<Task> TaskService::getTask(TaskId id) const {
    return scheduler_.getTask(id);
}

bool TaskService::cancelTask(TaskId id) {
    return scheduler_.cancelTask(id);
}

} // namespace ForgeSched