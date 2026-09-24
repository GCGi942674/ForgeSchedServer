#ifndef FORGESCHED_WORKER_H
#define FORGESCHED_WORKER_H

#include "WorkerStatus.h"
#include <string>
#include <cstdint>
#include <chrono>

namespace ForgeSched {

class WorkerManager;

using WorkerId = std::string;

class Worker {
public:
    Worker(const WorkerId& id, const std::string& hostname, uint32_t total_slots);

    WorkerId getId() const { return id_; }
    const std::string& getHostname() const { return hostname_; }
    WorkerStatus getStatus() const { return status_; }
    uint32_t getTotalSlots() const { return total_slots_; }
    uint32_t getUsedSlots() const { return used_slots_; }

    const auto& getRegisteredAt() const { return registered_at_; }
    const auto& getLastHeartbeat() const { return last_heartbeat_; }

    uint32_t freeSlots() const;
    bool hasFreeSlot() const;

    void setStatus(WorkerStatus status) { status_ = status; }
    void setTotalSlots(uint32_t slots) { total_slots_ = slots; }
    void setOnline() { status_ = WorkerStatus::ONLINE; }
    void setOffline() { status_ = WorkerStatus::OFFLINE; }
    void updateHeartbeat();

    Worker(const Worker&) = default;
    Worker& operator=(const Worker&) = default;
    Worker(Worker&&) = default;
    Worker& operator=(Worker&&) = default;

private:
    friend class WorkerManager;

    bool acquireSlot();
    bool releaseSlot();
    WorkerId id_;
    std::string hostname_;
    WorkerStatus status_;

    uint32_t total_slots_;
    uint32_t used_slots_;

    std::chrono::system_clock::time_point registered_at_;
    std::chrono::system_clock::time_point last_heartbeat_;
};

} // namespace ForgeSched

#endif // FORGESCHED_WORKER_H
