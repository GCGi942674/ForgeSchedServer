#ifndef FORGESCHED_WORKER_MANAGER_H
#define FORGESCHED_WORKER_MANAGER_H

#include "worker/Worker.h"
#include "worker/WorkerStatus.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include <optional>

namespace ForgeSched {

class WorkerManager {
public:
    bool registerWorker(
        const WorkerId& worker_id,
        const std::string& hostname,
        uint32_t slots
    );

    bool heartbeat(const WorkerId& worker_id);
    bool markOffline(const WorkerId& worker_id);

    bool acquireSlot(const WorkerId& worker_id);
    bool releaseSlot(const WorkerId& worker_id);

    std::optional<Worker> getWorker(const WorkerId& id) const;
    std::vector<Worker> getWorkers() const;
    std::vector<Worker> getOnlineWorkers() const;
    std::vector<Worker> getAvailableWorkers() const;

    void checkTimeouts(std::chrono::seconds timeout);

    WorkerManager() = default;
    ~WorkerManager() = default;

    WorkerManager(const WorkerManager&) = delete;
    WorkerManager& operator=(const WorkerManager&) = delete;

private:
    mutable std::mutex mutex_;
    std::unordered_map<WorkerId, std::unique_ptr<Worker>> workers_;
};

} // namespace ForgeSched

#endif // FORGESCHED_WORKER_MANAGER_H
