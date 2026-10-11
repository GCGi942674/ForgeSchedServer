#include "WorkerServer.h"
#include "Logging.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include "scheduler/SchedulingDriver.h"
#include "config/Config.h"

using namespace ForgeSched::Protocol;

namespace ForgeSched {

WorkerServer::WorkerServer(bool persistent)
{
    if (persistent)
        store_ptr_ = std::make_unique<StateStore>(
            Config::instance().getString("database.path", ""),
            Config::instance().getInt("database.busy_timeout_ms", 5000));
    worker_manager_ptr_ = std::make_unique<WorkerManager>();
    registry_ptr_ = std::make_unique<WorkerConnectionRegistry>();
    dispatcher_ptr_ = std::make_unique<NetworkWorkerDispatcher>(*registry_ptr_);
    scheduler_ptr_ = std::make_unique<Scheduler>(*worker_manager_ptr_);
    if (store_ptr_) scheduler_ptr_->restoreFrom(*store_ptr_);
    coordinator_ptr_ = std::make_unique<SchedulingCoordinator>(*scheduler_ptr_, *dispatcher_ptr_);
    scheduling_driver_ptr_ = std::make_unique<SchedulingDriver>(*coordinator_ptr_);
    task_service_ptr_ = std::make_unique<TaskService>(*scheduler_ptr_, *scheduling_driver_ptr_);
    if (store_ptr_) task_service_ptr_->setNextTaskId(scheduler_ptr_->nextTaskId());
    router_ptr_ = std::make_unique<Protocol::ProtocolRouter>(*task_service_ptr_, *scheduler_ptr_, *worker_manager_ptr_, *scheduling_driver_ptr_);

    LOG_INFO(LogModule::SERVER, "WorkerServer initialized");
    if (persistent) {
        const int timeout = Config::instance().getInt("server.worker_timeout_seconds", 30);
        const int retries = Config::instance().getInt("server.recovery_max_retries", 1);
        if (timeout < 2 || timeout > 3600 || retries < 0 || retries > 100)
            throw std::runtime_error("invalid worker recovery configuration");
        liveness_thread_ = std::thread([this, timeout] {
            std::unique_lock<std::mutex> wait_lock(liveness_mutex_);
            while (!liveness_cv_.wait_for(wait_lock, std::chrono::seconds(1), [this] { return stopping_; })) {
                wait_lock.unlock();
                try {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    worker_manager_ptr_->checkTimeouts(std::chrono::seconds(timeout));
                } catch (const std::exception& error) {
                    LOG_ERROR(LogModule::WORKER, std::string("liveness check failed: ") + error.what());
                }
                wait_lock.lock();
            }
        });
    }
}

WorkerServer::~WorkerServer() {
    {
        std::lock_guard<std::mutex> lock(liveness_mutex_);
        stopping_ = true;
    }
    liveness_cv_.notify_all();
    if (liveness_thread_.joinable()) liveness_thread_.join();
}

void WorkerServer::attach(EchoServer& transport) {
    transport.setConnectionCallbacks(
        [this](const std::shared_ptr<Connection>& conn) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto session = std::make_unique<ConnectionSession>(conn,
                *worker_manager_ptr_, *registry_ptr_, *router_ptr_, *scheduling_driver_ptr_);
            sessions_.emplace(conn.get(), std::move(session));
        },
        [this](const std::shared_ptr<Connection>& conn, const std::string& message) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto it = sessions_.find(conn.get());
            if (it != sessions_.end()) it->second->onMessage(message);
        },
        [this](const std::shared_ptr<Connection>& conn) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto it = sessions_.find(conn.get());
            if (it == sessions_.end()) return;
            it->second->handleClose();
            sessions_.erase(it);
        });
}

SchedulingResult WorkerServer::runOnce() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return coordinator_ptr_->runOnce();
}

} // namespace ForgeSched
