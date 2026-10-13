#include "worker/WorkerManager.h"
#include "task/TaskLimits.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include "Logging.h"

namespace ForgeSched {

bool WorkerManager::registerWorker(
    const WorkerId& worker_id,
    const std::string& hostname,
    uint32_t slots,
    const std::string& machine_id,
    const std::string& slot_id
) {
    if (worker_id.empty() || worker_id.size() > kMaxWorkerIdBytes ||
        hostname.size() > kMaxWorkerHostnameBytes) {
        LOG_ERROR(LogModule::WORKER, "Invalid worker registration: empty or oversized identity");
        return false;
    }

    if (hostname.empty()) {
        LOG_ERROR(LogModule::WORKER, "Invalid worker registration: empty hostname");
        return false;
    }

    if (slots == 0 || (!machine_id.empty() && machine_id.size() > 64) ||
        (!slot_id.empty() && slot_id.size() > 64)) {
        LOG_ERROR(LogModule::WORKER, "Invalid worker registration: slots must be > 0");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(worker_id);
    const auto machine = machine_id.empty() ? hostname : machine_id;
    const auto slot = slot_id.empty() ? worker_id : slot_id;
    for (const auto& entry : workers_) {
        if (entry.first != worker_id && entry.second->getMachineId() == machine &&
            entry.second->getSlotId() == slot) return false;
    }
    if (it != workers_.end()) {
        if (it->second->hasExplicitIdentity() &&
            (it->second->getMachineId() != machine || it->second->getSlotId() != slot))
            return false;
        // Re-registration: update existing worker
        it->second->hostname_ = hostname;
        it->second->setIdentity(machine, slot, !machine_id.empty() && !slot_id.empty());
        it->second->setTotalSlots(slots);
        it->second->setOnline();
        it->second->updateHeartbeat();
        ++generation_;
        LOG_INFO(LogModule::WORKER, "Worker re-registered: " + worker_id + " (hostname=" + hostname + ", slots=" + std::to_string(slots) + ")");
        return true;
    }

    // New registration
    auto worker = std::make_unique<Worker>(worker_id, hostname, slots);
    worker->setIdentity(machine, slot, !machine_id.empty() && !slot_id.empty());
    worker->setOnline();
    worker->updateHeartbeat();
    workers_[worker_id] = std::move(worker);
    ++generation_;
    LOG_INFO(LogModule::WORKER, "New worker registered: " + worker_id + " (hostname=" + hostname + ", slots=" + std::to_string(slots) + ")");
    return true;
}

bool WorkerManager::markDraining(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return false;
    it->second->setDraining();
    ++generation_;
    return true;
}

bool WorkerManager::markOffline(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return false;
    it->second->setOffline();
    ++generation_;
    return true;
}

bool WorkerManager::heartbeat(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return false;
    }

    it->second->updateHeartbeat();
    ++generation_;
    // Heartbeats from a draining slot retain DRAINING until its connection closes.
    LOG_DEBUG(LogModule::WORKER, "Worker heartbeat: " + worker_id);
    return true;
}

bool WorkerManager::acquireSlot(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return false;
    }

    if (it->second->getStatus() != WorkerStatus::ONLINE) {
        return false;
    }

    const bool changed = it->second->acquireSlot();
    if (changed) ++generation_;
    return changed;
}

bool WorkerManager::releaseSlot(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return false;
    }

    const bool changed = it->second->releaseSlot();
    if (changed) ++generation_;
    return changed;
}

std::optional<Worker> WorkerManager::getWorker(const WorkerId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(id);
    if (it == workers_.end()) {
        return std::nullopt;
    }

    return *it->second;
}

std::vector<Worker> WorkerManager::getWorkers() const {
    return snapshot().second;
}

std::pair<uint64_t, std::vector<Worker>> WorkerManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Worker> result;
    result.reserve(workers_.size());

    for (const auto& [id, worker_ptr] : workers_) {
        result.push_back(*worker_ptr);
    }

    return {generation_, std::move(result)};
}

std::vector<Worker> WorkerManager::getOnlineWorkers() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Worker> result;

    for (const auto& [id, worker_ptr] : workers_) {
        if (worker_ptr->getStatus() == WorkerStatus::ONLINE) {
            result.push_back(*worker_ptr);
        }
    }

    return result;
}

std::vector<Worker> WorkerManager::getAvailableWorkers() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Worker> result;

    for (const auto& [id, worker_ptr] : workers_) {
        if (worker_ptr->getStatus() == WorkerStatus::ONLINE && worker_ptr->hasFreeSlot()) {
            result.push_back(*worker_ptr);
        }
    }

    return result;
}

void WorkerManager::checkTimeouts(std::chrono::seconds timeout) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    int timed_out_count = 0;

    for (auto& [id, worker_ptr] : workers_) {
        if (worker_ptr->getStatus() != WorkerStatus::ONLINE) {
            continue;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - worker_ptr->getLastHeartbeat()
        );

        if (elapsed > timeout) {
            worker_ptr->setOffline();
            ++generation_;
            LOG_WARN(LogModule::WORKER, "Worker heartbeat timeout: " + id + " (" + std::to_string(elapsed.count()) + "s ago)");
            timed_out_count++;
        }
    }

    if (timed_out_count > 0) {
        LOG_INFO(LogModule::WORKER, "Marked " + std::to_string(timed_out_count) + " workers as offline due to heartbeat timeout");
    }
}

} // namespace ForgeSched
