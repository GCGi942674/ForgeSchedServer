#ifndef FORGESCHED_STATE_STORE_H
#define FORGESCHED_STATE_STORE_H

#include "task/Task.h"
#include <optional>
#include <utility>
#include <string>
#include <vector>

struct sqlite3;

namespace ForgeSched {

struct PersistedTask {
    Task task;
    uint64_t queue_sequence;
};

class StateStore {
public:
    explicit StateStore(const std::string& path, int busy_timeout_ms = 5000);
    ~StateStore();
    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;

    std::vector<PersistedTask> loadTasks() const;
    std::vector<std::string> loadStoppedBatches() const;
    std::optional<std::pair<TaskId, std::string>> findSubmission(const std::string& key) const;
    std::optional<std::string> recoveryDecision(const std::string& run_id,
                                               TaskId task_id, const std::string& worker_id) const;
    // Only completed attempts may accept or expose archived evidence.
    std::optional<std::string> archiveOwner(TaskId task_id, const std::string& run_id) const;
    // All task and batch changes are one durable transaction. Throws on failure.
    void save(const std::vector<Task>& tasks,
              const std::optional<std::string>& stopped_batch = std::nullopt,
              const std::optional<std::string>& run_status_override = std::nullopt,
              const std::optional<std::pair<std::string, std::string>>& submission = std::nullopt,
              const std::optional<std::pair<std::string, std::string>>& recovery = std::nullopt);

private:
    sqlite3* db_{nullptr};
    int lock_fd_{-1};
    void exec(const char* sql) const;
};

} // namespace ForgeSched
#endif
