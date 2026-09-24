#include "task/Task.h"
#include "task/TaskStatus.h"
#include "task/TaskType.h"

namespace ForgeSched {

std::string toString(TaskStatus status) {
    switch (status) {
        case TaskStatus::PENDING:   return "PENDING";
        case TaskStatus::QUEUED:    return "QUEUED";
        case TaskStatus::ASSIGNED:  return "ASSIGNED";
        case TaskStatus::RUNNING:   return "RUNNING";
        case TaskStatus::SUCCEEDED: return "SUCCEEDED";
        case TaskStatus::FAILED:    return "FAILED";
        case TaskStatus::TIMEOUT:   return "TIMEOUT";
        case TaskStatus::CANCELLED: return "CANCELLED";
        default: return "UNKNOWN";
    }
}

std::string toString(TaskType type) {
    switch (type) {
        case TaskType::REGRESSION: return "REGRESSION";
        case TaskType::UNKNOWN:    return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

Task::Task(TaskId id, TaskType type, const std::string& target, const std::string& revision)
    : id_(id)
    , type_(type)
    , status_(TaskStatus::PENDING)
    , target_(target)
    , revision_(revision)
    , worker_id_()
    , priority_(0)
    , retry_count_(0)
    , created_at_(std::chrono::system_clock::now())
    , started_at_()
    , finished_at_()
{
}

bool Task::canTransitionTo(TaskStatus next) const {
    switch (status_) {
        case TaskStatus::PENDING:
            return next == TaskStatus::QUEUED || next == TaskStatus::CANCELLED;

        case TaskStatus::QUEUED:
            return next == TaskStatus::ASSIGNED || next == TaskStatus::CANCELLED;

        case TaskStatus::ASSIGNED:
            return next == TaskStatus::RUNNING ||
                   next == TaskStatus::QUEUED ||
                   next == TaskStatus::CANCELLED;

        case TaskStatus::RUNNING:
            return next == TaskStatus::SUCCEEDED ||
                   next == TaskStatus::FAILED ||
                   next == TaskStatus::TIMEOUT ||
                   next == TaskStatus::CANCELLED;

        case TaskStatus::SUCCEEDED:
        case TaskStatus::FAILED:
        case TaskStatus::TIMEOUT:
        case TaskStatus::CANCELLED:
            return false;

        default:
            return false;
    }
}

bool Task::transitionTo(TaskStatus next) {
    if (!canTransitionTo(next)) {
        return false;
    }

    status_ = next;

    if (next == TaskStatus::RUNNING && started_at_.time_since_epoch().count() == 0) {
        started_at_ = std::chrono::system_clock::now();
    }

    if ((next == TaskStatus::SUCCEEDED ||
         next == TaskStatus::FAILED ||
         next == TaskStatus::TIMEOUT ||
         next == TaskStatus::CANCELLED) &&
        finished_at_.time_since_epoch().count() == 0) {
        finished_at_ = std::chrono::system_clock::now();
    }

    return true;
}

} // namespace ForgeSched