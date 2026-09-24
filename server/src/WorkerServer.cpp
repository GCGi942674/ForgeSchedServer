#include "WorkerServer.h"
#include "Logging.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"

using namespace ForgeSched::Protocol;

namespace ForgeSched {

WorkerServer::WorkerServer()
{
    worker_manager_ptr_ = std::make_unique<WorkerManager>();
    registry_ptr_ = std::make_unique<WorkerConnectionRegistry>();
    dispatcher_ptr_ = std::make_unique<NetworkWorkerDispatcher>(*registry_ptr_);
    scheduler_ptr_ = std::make_unique<Scheduler>(*worker_manager_ptr_);
    task_service_ptr_ = std::make_unique<TaskService>(*scheduler_ptr_);
    coordinator_ptr_ = std::make_unique<SchedulingCoordinator>(*scheduler_ptr_, *dispatcher_ptr_);
    router_ptr_ = std::make_unique<Protocol::ProtocolRouter>(*task_service_ptr_, *scheduler_ptr_, *worker_manager_ptr_);

    LOG_INFO(LogModule::SERVER, "WorkerServer initialized");
}

WorkerServer::~WorkerServer() {
}

void WorkerServer::attach(EchoServer& transport) {
    transport.setConnectionCallbacks(
        [this](const std::shared_ptr<Connection>& conn) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto session = std::make_unique<WorkerSession>(conn->ownerLoop(),
                *worker_manager_ptr_, *registry_ptr_, *router_ptr_);
            session->setConnection(conn);
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
