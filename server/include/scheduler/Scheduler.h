#ifndef FORGESCHED_SCHEDULER_H
#define FORGESCHED_SCHEDULER_H

#include "task/Task.h"
#include "task/TaskStatus.h"
#include "worker/WorkerManager.h"
#include "worker/Worker.h"
#include <unordered_map>
#include <vector>
#include <optional>
#include <queue>
#include <mutex>
#include <cstdint>

namespace ForgeSched {

struct TaskAssignment {
    TaskId task_id;
    WorkerId worker_id;
};

class Scheduler {
public:
    explicit Scheduler(WorkerManager& worker_manager);

    bool submitTask(Task task);
    std::vector<TaskAssignment> schedule();
    bool completeTask(TaskId task_id, TaskStatus final_status);
    bool cancelTask(TaskId task_id);

    bool markTaskStarted(TaskId task_id, const WorkerId& worker_id);
    bool rollbackAssignment(TaskId task_id);

    std::optional<Task> getTask(TaskId id) const;
    std::vector<Task> getTasks() const;
    std::vector<Task> getTasksByStatus(TaskStatus status) const;

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

private:
    struct QueuedTaskInfo {
        TaskId task_id;
        int priority;
        uint64_t sequence;

        bool operator>(const QueuedTaskInfo& other) const {
            if (priority != other.priority) {
                return priority < other.priority;
            }
            return sequence > other.sequence;
        }
    };

    WorkerManager& worker_manager_;
    std::unordered_map<TaskId, Task> tasks_;
    std::priority_queue<QueuedTaskInfo, std::vector<QueuedTaskInfo>, std::greater<QueuedTaskInfo>> task_queue_;
    uint64_t next_sequence_;
    mutable std::mutex mutex_;

    bool isValidFinalStatus(TaskStatus status) const;

};

} // namespace ForgeSched

#endif // FORGESCHED_SCHEDULER_H
