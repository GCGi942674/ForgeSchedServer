#ifndef FORGESCHED_TASK_H
#define FORGESCHED_TASK_H

#include "TaskStatus.h"
#include "TaskType.h"
#include <string>
#include <chrono>
#include <cstdint>

namespace ForgeSched {

using TaskId = uint64_t;

class Task {
public:
    Task(TaskId id, TaskType type, const std::string& target, const std::string& revision);

    TaskId getId() const { return id_; }
    TaskType getType() const { return type_; }
    TaskStatus getStatus() const { return status_; }

        const std::string& getTarget() const { return target_; }
    const std::string& getRevision() const { return revision_; }
    const std::string& getWorkerId() const { return worker_id_; }

    int getPriority() const { return priority_; }
    int getRetryCount() const { return retry_count_; }

    const auto& getCreatedAt() const { return created_at_; }
    const auto& getStartedAt() const { return started_at_; }
    const auto& getFinishedAt() const { return finished_at_; }

        bool canTransitionTo(TaskStatus next) const;
    bool transitionTo(TaskStatus next);

    void setWorkerId(const std::string& worker_id) { worker_id_ = worker_id; }
    void clearWorkerId() { worker_id_.clear(); }
    void setPriority(int priority) { priority_ = priority; }
    void incrementRetryCount() { retry_count_++; }

    Task(const Task&) = default;
    Task& operator=(const Task&) = default;
    Task(Task&&) = default;
        Task& operator=(Task&&) = default;

private:
    TaskId id_;
    TaskType type_;
    TaskStatus status_;

    std::string target_;
    std::string revision_;
    std::string worker_id_;

    int priority_;
    int retry_count_;

    std::chrono::system_clock::time_point created_at_;
    std::chrono::system_clock::time_point started_at_;
        std::chrono::system_clock::time_point finished_at_;
};

} // namespace ForgeSched

#endif // FORGESCHED_TASK_H