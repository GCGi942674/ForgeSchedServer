#include "scheduler/Scheduler.h"
#include "task/TaskStatus.h"
#include "Logging.h"

namespace ForgeSched {

Scheduler::Scheduler(WorkerManager& worker_manager)
    : worker_manager_(worker_manager)
    , next_sequence_(0)
{
}

bool Scheduler::submitTask(Task task) {
    if (task.getId() == 0) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot submit task with zero TaskId");
        return false;
    }

    if (task.getStatus() != TaskStatus::PENDING) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot submit task not in PENDING state");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (tasks_.find(task.getId()) != tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Duplicate TaskId: " + std::to_string(task.getId()));
        return false;
    }

    if (!task.transitionTo(TaskStatus::QUEUED)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition PENDING -> QUEUED failed");
        return false;
    }

    task_queue_.push({task.getId(), task.getPriority(), next_sequence_++});
    tasks_.emplace(task.getId(), std::move(task));

    LOG_INFO(LogModule::TASK, "task=" + std::to_string(task.getId()) + " submitted priority=" + std::to_string(task.getPriority()));

    return true;
}

std::vector<TaskAssignment> Scheduler::schedule() {
    std::vector<TaskAssignment> assignments;

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<QueuedTaskInfo> to_process;

    while (!task_queue_.empty()) {
        const auto& queued_info = task_queue_.top();
        TaskId task_id = queued_info.task_id;

        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) {
            task_queue_.pop();
            continue;
        }

        Task& task = it->second;

        if (task.getStatus() != TaskStatus::QUEUED) {
            task_queue_.pop();
            continue;
        }

        to_process.push_back(queued_info);
        task_queue_.pop();
    }

    for (const auto& queued_info : to_process) {
        auto it = tasks_.find(queued_info.task_id);
        if (it == tasks_.end()) {
            continue;
        }

        Task& task = it->second;

        if (task.getStatus() != TaskStatus::QUEUED) {
            continue;
        }

        std::vector<Worker> available_workers = worker_manager_.getAvailableWorkers();

        if (available_workers.empty()) {
            task_queue_.push(queued_info);
            continue;
        }

        std::optional<Worker> selected_worker;
        uint32_t max_free_slots = 0;

        for (const auto& worker : available_workers) {
            uint32_t free = worker.freeSlots();
            if (!selected_worker || free > max_free_slots || (free == max_free_slots && worker.getId() < selected_worker->getId())) {
                max_free_slots = free;
                selected_worker = worker;
            }
        }

        if (!worker_manager_.acquireSlot(selected_worker->getId())) {
            task_queue_.push(queued_info);
            continue;
        }

        task.setWorkerId(selected_worker->getId());

        if (!task.transitionTo(TaskStatus::ASSIGNED)) {
            LOG_ERROR(LogModule::SCHEDULER, "Slot acquired but task transition QUEUED -> ASSIGNED failed for task=" + std::to_string(task.getId()));
            worker_manager_.releaseSlot(selected_worker->getId());
            continue;
        }

        assignments.push_back({task.getId(), selected_worker->getId()});

        LOG_INFO(LogModule::SCHEDULER, "task=" + std::to_string(task.getId()) + " assigned worker=" + selected_worker->getId());
    }

    return assignments;
}

bool Scheduler::completeTask(TaskId task_id, TaskStatus final_status) {
    if (!isValidFinalStatus(final_status)) {
        LOG_WARN(LogModule::SCHEDULER, "Invalid completion status: " + toString(final_status));
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot complete unknown task: " + std::to_string(task_id));
        return false;
    }

    Task& task = it->second;

    if (task.getStatus() != TaskStatus::RUNNING) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot complete task not in RUNNING state: " + std::to_string(task_id));
        return false;
    }

    if (task.getWorkerId().empty()) {
        LOG_ERROR(LogModule::SCHEDULER, "Task in RUNNING state has no worker_id: " + std::to_string(task_id));
        return false;
    }

    if (!task.transitionTo(final_status)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition RUNNING -> " + toString(final_status) + " failed for task=" + std::to_string(task_id));
        return false;
    }

    if (!worker_manager_.releaseSlot(task.getWorkerId())) {
        LOG_ERROR(LogModule::SCHEDULER, "Worker slot release failed after task completion for task=" + std::to_string(task_id));
        return false;
    }

    LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " completed status=" + toString(final_status));

    return true;
}

bool Scheduler::cancelTask(TaskId task_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot cancel unknown task: " + std::to_string(task_id));
        return false;
    }

    Task& task = it->second;

    switch (task.getStatus()) {
        case TaskStatus::QUEUED: {
            if (!task.transitionTo(TaskStatus::CANCELLED)) {
                LOG_ERROR(LogModule::SCHEDULER, "Task transition QUEUED -> CANCELLED failed for task=" + std::to_string(task_id));
                return false;
            }
            LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " cancelled");
            return true;
        }

        case TaskStatus::ASSIGNED: {
            if (task.getWorkerId().empty()) {
                LOG_ERROR(LogModule::SCHEDULER, "Task in ASSIGNED state has no worker_id: " + std::to_string(task_id));
                return false;
            }

            if (!task.transitionTo(TaskStatus::CANCELLED)) {
                LOG_ERROR(LogModule::SCHEDULER, "Task transition ASSIGNED -> CANCELLED failed for task=" + std::to_string(task_id));
                return false;
            }

            if (!worker_manager_.releaseSlot(task.getWorkerId())) {
                LOG_ERROR(LogModule::SCHEDULER, "Worker slot release failed after task cancellation for task=" + std::to_string(task_id));
                return false;
            }

            LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " cancelled");
            return true;
        }

        case TaskStatus::RUNNING: {
            if (task.getWorkerId().empty()) {
                LOG_ERROR(LogModule::SCHEDULER, "Task in RUNNING state has no worker_id: " + std::to_string(task_id));
                return false;
            }

            if (!task.transitionTo(TaskStatus::CANCELLED)) {
                LOG_ERROR(LogModule::SCHEDULER, "Task transition RUNNING -> CANCELLED failed for task=" + std::to_string(task_id));
                return false;
            }

            if (!worker_manager_.releaseSlot(task.getWorkerId())) {
                LOG_ERROR(LogModule::SCHEDULER, "Worker slot release failed after task cancellation for task=" + std::to_string(task_id));
                return false;
            }

            LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " cancelled");
            return true;
        }

        case TaskStatus::PENDING:
            if (!task.transitionTo(TaskStatus::CANCELLED)) {
                LOG_ERROR(LogModule::SCHEDULER, "Task transition PENDING -> CANCELLED failed for task=" + std::to_string(task_id));
                return false;
            }
            LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " cancelled");
            return true;

        case TaskStatus::SUCCEEDED:
        case TaskStatus::FAILED:
        case TaskStatus::TIMEOUT:
        case TaskStatus::CANCELLED:
            LOG_WARN(LogModule::SCHEDULER, "Cannot cancel terminal task: " + std::to_string(task_id));
            return false;

        default:
            LOG_ERROR(LogModule::SCHEDULER, "Unknown task status for cancellation: " + std::to_string(task_id));
            return false;
    }
}

std::optional<Task> Scheduler::getTask(TaskId id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::vector<Task> Scheduler::getTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Task> result;
    result.reserve(tasks_.size());

    for (const auto& [id, task] : tasks_) {
        result.push_back(task);
    }

    return result;
}

std::vector<Task> Scheduler::getTasksByStatus(TaskStatus status) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Task> result;

    for (const auto& [id, task] : tasks_) {
        if (task.getStatus() == status) {
            result.push_back(task);
        }
    }

    return result;
}

bool Scheduler::isValidFinalStatus(TaskStatus status) const {
    return status == TaskStatus::SUCCEEDED ||
           status == TaskStatus::FAILED ||
           status == TaskStatus::TIMEOUT;
}

bool Scheduler::markTaskStarted(TaskId task_id, const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot start unknown task: " + std::to_string(task_id));
        return false;
    }

    Task& task = it->second;

    if (task.getStatus() != TaskStatus::ASSIGNED) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot start task not in ASSIGNED state: " + std::to_string(task_id));
        return false;
    }

    if (task.getWorkerId() != worker_id) {
        LOG_WARN(LogModule::SCHEDULER, "Worker id mismatch for task start: task=" + std::to_string(task_id) + " expected=" + task.getWorkerId() + " got=" + worker_id);
        return false;
    }

    if (!task.transitionTo(TaskStatus::RUNNING)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition ASSIGNED -> RUNNING failed for task=" + std::to_string(task_id));
        return false;
    }

    LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " started");

    return true;
}

bool Scheduler::rollbackAssignment(TaskId task_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot rollback unknown task: " + std::to_string(task_id));
        return false;
    }

    Task& task = it->second;

    if (task.getStatus() != TaskStatus::ASSIGNED) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot rollback task not in ASSIGNED state: " + std::to_string(task_id));
        return false;
    }

    if (task.getWorkerId().empty()) {
        LOG_ERROR(LogModule::SCHEDULER, "Task in ASSIGNED state has no worker_id: " + std::to_string(task_id));
        return false;
    }

    if (!worker_manager_.releaseSlot(task.getWorkerId())) {
        LOG_ERROR(LogModule::SCHEDULER, "Worker slot release failed during rollback for task=" + std::to_string(task_id));
        return false;
    }

    task.clearWorkerId();

    if (!task.transitionTo(TaskStatus::QUEUED)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition ASSIGNED -> QUEUED failed for task=" + std::to_string(task_id));
        return false;
    }

    task_queue_.push({task.getId(), task.getPriority(), next_sequence_++});

    LOG_INFO(LogModule::SCHEDULER, "task=" + std::to_string(task_id) + " rolled back to queue");

    return true;
}

} // namespace ForgeSched
