#ifndef FORGESCHED_WORKER_SERVER_H
#define FORGESCHED_WORKER_SERVER_H

#include "EchoServer.h"
#include "WorkerSessionHandler.h"
#include "worker/WorkerManager.h"
#include "worker/WorkerConnectionRegistry.h"
#include "worker/NetworkWorkerDispatcher.h"
#include "scheduler/Scheduler.h"
#include "task/TaskService.h"
#include "EventLoop.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include "worker/WorkerSession.h"

namespace ForgeSched {

class WorkerServer {
public:
    WorkerServer();

    ~WorkerServer();
    // Call before transport.run(); this object must outlive the transport.
    void attach(EchoServer& transport);
    SchedulingResult runOnce();

    WorkerManager& getWorkerManager() { return *worker_manager_ptr_; }
    WorkerConnectionRegistry& getWorkerConnectionRegistry() { return *registry_ptr_; }
    Scheduler& getScheduler() { return *scheduler_ptr_; }
    TaskService& getTaskService() { return *task_service_ptr_; }
    Protocol::ProtocolRouter& getRouter() { return *router_ptr_; }

private:
    std::unique_ptr<WorkerManager> worker_manager_ptr_;
    std::unique_ptr<WorkerConnectionRegistry> registry_ptr_;
    std::unique_ptr<NetworkWorkerDispatcher> dispatcher_ptr_;
    std::unique_ptr<Scheduler> scheduler_ptr_;
    std::unique_ptr<TaskService> task_service_ptr_;
    std::unique_ptr<SchedulingCoordinator> coordinator_ptr_;
    std::unique_ptr<Protocol::ProtocolRouter> router_ptr_;
    std::mutex session_mutex_;
    std::unordered_map<Connection*, std::unique_ptr<WorkerSession>> sessions_;
};

} // namespace ForgeSched

#endif // FORGESCHED_WORKER_SERVER_H
