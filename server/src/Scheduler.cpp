#include "scheduler/Scheduler.h"
#include "task/TaskStatus.h"
#include "Logging.h"
#include "worker/WorkerDispatcher.h"
#include <type_traits>

namespace ForgeSched {
namespace {
const std::string* batchId(const Task& task) {
    const auto& payload = task.getPayload();
    if (!payload.is_object() || !payload.contains("context") ||
        !payload["context"].is_object()) return nullptr;
    const auto& context = payload["context"];
    if (!context.contains("batch_id") || !context["batch_id"].is_string()) return nullptr;
    return &context["batch_id"].get_ref<const std::string&>();
}

void countStatus(BatchSnapshot& snapshot, TaskStatus status) {
    ++snapshot.total;
    switch (status) {
        case TaskStatus::PENDING: ++snapshot.pending; break;
        case TaskStatus::QUEUED: ++snapshot.queued; break;
        case TaskStatus::ASSIGNED: ++snapshot.assigned; break;
        case TaskStatus::RUNNING: ++snapshot.running; break;
        case TaskStatus::SUCCEEDED: ++snapshot.succeeded; break;
        case TaskStatus::FAILED: ++snapshot.failed; break;
        case TaskStatus::TIMEOUT: ++snapshot.timeout; break;
        case TaskStatus::CANCELLED: ++snapshot.cancelled; break;
    }
}
}

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

    const auto batch = batchId(task);
    if (batch && stopped_batches_.count(*batch)) return false;

    if (tasks_.find(task.getId()) != tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Duplicate TaskId: " + std::to_string(task.getId()));
        return false;
    }

    if (!task.transitionTo(TaskStatus::QUEUED)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition PENDING -> QUEUED failed");
        return false;
    }

    // Reserve before changing task ownership; rollback can then requeue without allocation.
    task_queue_.reserve(tasks_.size() + 1);
    const auto id = task.getId();
    const auto priority = task.getPriority();
    tasks_.emplace(id, std::move(task));
    task_queue_.push({id, priority, next_sequence_++});

    return true;
}

std::vector<TaskAssignment> Scheduler::schedule() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Work on a copy: allocation failure must not consume the real queue.
    auto pending = task_queue_;
    pending.reserve(tasks_.size()); // also reserves allocation-free rollback space
    std::vector<TaskAssignment> assignments;
    std::vector<std::pair<Task*, Task>> changes;
    assignments.reserve(pending.size());
    changes.reserve(pending.size());
    size_t acquired = 0;
    static_assert(std::is_nothrow_move_assignable<Task>::value, "task commit must not throw");
    try {
        while (!pending.empty()) {
            const auto queued = pending.top();
            auto it = tasks_.find(queued.task_id);
            if (it == tasks_.end() || it->second.getStatus() != TaskStatus::QUEUED) {
                pending.pop();
                continue;
            }
            auto workers = worker_manager_.getAvailableWorkers();
            if (workers.empty()) break;
            const Worker* selected = &workers.front();
            for (const auto& worker : workers) {
                if (worker.freeSlots() > selected->freeSlots() ||
                    (worker.freeSlots() == selected->freeSlots() && worker.getId() < selected->getId()))
                    selected = &worker;
            }
            const auto worker_id = selected->getId();
            Task prepared = it->second;
            prepared.setWorkerId(worker_id);
            prepared.transitionTo(TaskStatus::ASSIGNED);
            changes.emplace_back(&it->second, std::move(prepared));
            assignments.push_back({queued.task_id, worker_id});
            if (!worker_manager_.acquireSlot(worker_id)) {
                assignments.pop_back();
                changes.pop_back();
                break;
            }
            ++acquired;
            pending.pop();
        }
    } catch (...) {
        // Strings, vector growth, worker snapshots and queue copies can all fail.
        for (size_t i = 0; i < acquired; ++i)
            worker_manager_.releaseSlot(assignments[i].worker_id);
        throw;
    }
    // All potentially allocating work is complete. No logging after commit.
    for (auto& change : changes) *change.first = std::move(change.second);
    task_queue_.swap(pending);
    return assignments;
}

std::optional<bool> Scheduler::dispatchAssignment(
    const TaskAssignment& assignment, WorkerDispatcher& dispatcher) {
    std::optional<Task> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(assignment.task_id);
        if (it == tasks_.end() || it->second.getStatus() != TaskStatus::ASSIGNED ||
            it->second.getWorkerId() != assignment.worker_id ||
            dispatching_.count(assignment.task_id)) return std::nullopt;
        snapshot = it->second; // allocate before establishing the claim
        dispatching_.insert(assignment.task_id);
    }
    try {
        const bool accepted = dispatcher.dispatch(assignment.worker_id, *snapshot);
        std::lock_guard<std::mutex> lock(mutex_);
        dispatching_.erase(assignment.task_id);
        return accepted;
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatching_.erase(assignment.task_id);
        throw;
    }
}


bool Scheduler::completeTask(TaskId task_id, TaskStatus final_status,
                             nlohmann::json execution_summary) {
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

    if (!task.canTransitionTo(final_status)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition RUNNING -> " + toString(final_status) + " failed for task=" + std::to_string(task_id));
        return false;
    }
    task.setExecutionSummary(execution_summary);
    task.transitionTo(final_status);

    if (!worker_manager_.releaseSlot(task.getWorkerId())) {
        LOG_ERROR(LogModule::SCHEDULER, "Worker slot release failed after task completion for task=" + std::to_string(task_id));
        return false;
    }

    LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " completed status=" + toString(final_status));

    return true;
}

bool Scheduler::cancelTask(TaskId task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_.count(task_id)) return false;

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

        case TaskStatus::ASSIGNED:
        case TaskStatus::RUNNING:
            // No remote cancellation ACK exists yet. Never release a live slot.
            return false;

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

std::optional<BatchSnapshot> Scheduler::queryBatch(const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    BatchSnapshot snapshot;
    snapshot.stop_requested = stopped_batches_.count(batch_id) != 0;
    for (const auto& entry : tasks_)
        if (const auto id = batchId(entry.second); id && *id == batch_id)
            countStatus(snapshot, entry.second.getStatus());
    if (!snapshot.total) return std::nullopt;
    return snapshot;
}

std::optional<BatchSnapshot> Scheduler::cancelBatch(const std::string& batch_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    for (const auto& entry : tasks_)
        if (const auto id = batchId(entry.second); id && *id == batch_id) {
            found = true; break;
        }
    if (!found) return std::nullopt;
    stopped_batches_.insert(batch_id); // Allocate before changing any task state.
    BatchSnapshot snapshot;
    snapshot.stop_requested = true;
    for (auto& entry : tasks_) {
        auto& task = entry.second;
        const auto id = batchId(task);
        if (!id || *id != batch_id) continue;
        if (task.getStatus() == TaskStatus::PENDING || task.getStatus() == TaskStatus::QUEUED) {
            task.transitionTo(TaskStatus::CANCELLED);
            ++snapshot.cancelled_now;
        }
        countStatus(snapshot, task.getStatus());
    }
    return snapshot;
}

bool Scheduler::isBatchStopped(const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_batches_.count(batch_id) != 0;
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
    if (dispatching_.count(task_id)) return false;

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

    const auto batch = batchId(task);
    if (batch && stopped_batches_.count(*batch))
        return task.transitionTo(TaskStatus::CANCELLED);

    if (!task.transitionTo(TaskStatus::QUEUED)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition ASSIGNED -> QUEUED failed for task=" + std::to_string(task_id));
        return false;
    }

    task_queue_.push({task.getId(), task.getPriority(), next_sequence_++});

    // No allocating diagnostics after the state change.

    return true;
}

} // namespace ForgeSched
