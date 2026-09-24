#ifndef FORGESCHED_PROTOCOL_ROUTER_H
#define FORGESCHED_PROTOCOL_ROUTER_H

#include "task/TaskService.h"
#include "task/Task.h"
#include "task/TaskStatus.h"
#include "scheduler/Scheduler.h"
#include "worker/WorkerManager.h"
#include "protocol/ProtocolMessage.h"
#include "protocol/MessageType.h"
#include "protocol/dto/TaskDTO.h"
#include "protocol/dto/WorkerDTO.h"
#include "protocol/dto/Response.h"

namespace ForgeSched::Protocol {

enum class ResponseCode {
    OK = 0,
    INVALID_REQUEST = 1,
    NOT_FOUND = 2,
    INVALID_STATE = 3,
    INTERNAL_ERROR = 4
};

class ProtocolRouter {
public:
    ProtocolRouter(
        TaskService& task_service,
        Scheduler& scheduler,
        WorkerManager& worker_manager
    );

    ProtocolMessage handle(const ProtocolMessage& request);

    ProtocolRouter(const ProtocolRouter&) = delete;
    ProtocolRouter& operator=(const ProtocolRouter&) = delete;

private:
    ResponseCode toErrorCode(TaskStatus status) const;
    ProtocolMessage buildResponse(
        const ProtocolMessage& request,
        ResponseCode code,
        const std::string& message = "",
        const nlohmann::json& data = nlohmann::json{}
    ) const;
    nlohmann::json taskToJson(const Task& task) const;

    TaskService& task_service_;
    Scheduler& scheduler_;
    WorkerManager& worker_manager_;
};

} // namespace ForgeSched::Protocol

#endif // FORGESCHED_PROTOCOL_ROUTER_H