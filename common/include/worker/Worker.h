#ifndef GCRP_WORKER_H
#define GCRP_WORKER_H

#include "WorkerStatus.h"
#include <string>
#include <cstdint>
#include <chrono>

namespace GCRP {

class WorkerManager;

using WorkerId = std::string;

class Worker {
public:
    Worker(const WorkerId& id, const std::string& hostname, uint32_t total_slots);

    WorkerId getId() const { return id_; }
    const std::string& getHostname() const { return hostname_; }
    const std::string& getMachineId() const { return machine_id_; }
    const std::string& getSlotId() const { return slot_id_; }
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
    void setDraining() { status_ = WorkerStatus::DRAINING; }
    void setIdentity(std::string machine, std::string slot, bool explicit_identity = false) {
        machine_id_ = std::move(machine);
        slot_id_ = std::move(slot);
        explicit_identity_ = explicit_identity;
    }
    bool hasExplicitIdentity() const { return explicit_identity_; }
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
    std::string machine_id_;
    std::string slot_id_;
    bool explicit_identity_{false};
    WorkerStatus status_;

    uint32_t total_slots_;
    uint32_t used_slots_;

    std::chrono::system_clock::time_point registered_at_;
    std::chrono::system_clock::time_point last_heartbeat_;
};

} // namespace GCRP

#endif // GCRP_WORKER_H
