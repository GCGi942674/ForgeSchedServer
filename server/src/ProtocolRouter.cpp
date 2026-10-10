#include "protocol/ProtocolRouter.h"
#include "protocol/ProtocolVersion.h"
#include "task/Task.h"
#include "task/TaskService.h"
#include "task/TaskStatus.h"
#include "task/TaskType.h"
#include "scheduler/SchedulingDriver.h"
#include "Logging.h"
#include <chrono>

namespace {
nlohmann::json executionSummary(const std::string& message, ForgeSched::TaskStatus status) noexcept {
    if (message.empty() || message.size() > 8192) return nullptr;
    try {
        const auto source = nlohmann::json::parse(message, nullptr, false);
        if (!source.is_object()) return nullptr;
        const auto status_it = source.find("status");
        if (status_it != source.end() &&
            (!status_it->is_string() || status_it->get<std::string>() != ForgeSched::toString(status)))
            return nullptr;
        nlohmann::json summary = nlohmann::json::object();
        for (const char* key : {"reason", "case_status", "case_reason"}) {
            const auto item = source.find(key);
            if (item != source.end() && item->is_string()) {
                const auto value = item->get<std::string>();
                if (value.size() <= 256) summary[key] = value;
            }
        }
        for (const char* key : {"case_ret_code", "raw_exit_code", "exit_code", "pid"}) {
            const auto item = source.find(key);
            if (item == source.end()) continue;
            if (item->is_number_unsigned()) {
                const auto value = item->get<uint64_t>();
                if (value <= 1000000000) summary[key] = value;
            } else if (item->is_number_integer()) {
                const auto value = item->get<int64_t>();
                if (value >= -1000000000 && value <= 1000000000) summary[key] = value;
            }
        }
        return summary.empty() ? nlohmann::json(nullptr) : summary;
    } catch (...) {
        return nullptr;
    }
}
}

namespace ForgeSched::Protocol {

ProtocolRouter::ProtocolRouter(
    TaskService& task_service,
    Scheduler& scheduler,
    WorkerManager& worker_manager,
    SchedulingDriver& scheduling_driver
)
    : task_service_(task_service)
    , scheduler_(scheduler)
    , worker_manager_(worker_manager)
    , scheduling_driver_(scheduling_driver)
{
}

ResponseCode ProtocolRouter::toErrorCode(TaskStatus status) const {
    switch (status) {
        case TaskStatus::SUCCEEDED:
        case TaskStatus::FAILED:
        case TaskStatus::TIMEOUT:
            return ResponseCode::OK;
        default:
            return ResponseCode::INVALID_STATE;
    }
}

ProtocolMessage ProtocolRouter::handle(const ProtocolMessage& request) {
    if (request.type == ForgeSched::Protocol::MessageType::RESPONSE) {
        LOG_WARN(LogModule::NETWORK, "Received RESPONSE as incoming request");
        return buildResponse(request, ResponseCode::INVALID_REQUEST, "unexpected response message");
    }

    ForgeSched::Protocol::ProtocolError error;
    ProtocolMessage response;

    switch (request.type) {
        case ForgeSched::Protocol::MessageType::SUBMIT_TASK: {
            ForgeSched::Protocol::DTO::SubmitTaskRequest submit_req;
            if (!ForgeSched::Protocol::DTO::fromJson(request.data, submit_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid submit task request");
            }

            ForgeSched::CreateTaskRequest create_req;
            create_req.type = submit_req.task_type;
            create_req.target = submit_req.target;
            create_req.revision = submit_req.revision;
            create_req.priority = submit_req.priority;
            create_req.payload = submit_req.payload;

            auto result = task_service_.createTask(create_req);
            if (!result) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid task parameters");
            }

            nlohmann::json data;
            data["task_id"] = *result;
            return buildResponse(request, ResponseCode::OK, "ok", data);
        }

        case ForgeSched::Protocol::MessageType::QUERY_TASK: {
            ForgeSched::Protocol::DTO::QueryTaskRequest query_req;
            if (!ForgeSched::Protocol::DTO::fromJson(request.data, query_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid query task request");
            }

            auto task = task_service_.getTask(query_req.task_id);
            if (!task) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            }

            return buildResponse(request, ResponseCode::OK, "ok", taskToJson(*task));
        }

        case ForgeSched::Protocol::MessageType::CANCEL_TASK: {
            ForgeSched::Protocol::DTO::CancelTaskRequest cancel_req;
            if (!ForgeSched::Protocol::DTO::fromJson(request.data, cancel_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid cancel task request");
            }

            if (!task_service_.getTask(cancel_req.task_id)) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            }
            bool success = task_service_.cancelTask(cancel_req.task_id);
            if (!success) {
                return buildResponse(request, ResponseCode::INVALID_STATE,
                    "only PENDING/QUEUED tasks can be cancelled; remote cancellation is not supported");
            }

            return buildResponse(request, ResponseCode::OK, "ok");
        }

        case ForgeSched::Protocol::MessageType::WORKER_REGISTER: {
            ForgeSched::Protocol::DTO::WorkerRegisterRequest register_req;
            if (!ForgeSched::Protocol::DTO::fromJson(request.data, register_req, error)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid worker register request");
            }

            bool success = worker_manager_.registerWorker(
                register_req.worker_id,
                register_req.hostname,
                register_req.slots
            );
            if (!success) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid worker registration");
            }

            return buildResponse(request, ResponseCode::OK, "ok");
        }

        case ForgeSched::Protocol::MessageType::WORKER_HEARTBEAT: {
            ForgeSched::Protocol::DTO::WorkerHeartbeatRequest heartbeat_req;
            if (!ForgeSched::Protocol::DTO::fromJson(request.data, heartbeat_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid heartbeat request");
            }

            bool success = worker_manager_.heartbeat(heartbeat_req.worker_id);
            if (!success) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "worker not found");
            }

            return buildResponse(request, ResponseCode::OK, "ok");
        }

        case ForgeSched::Protocol::MessageType::TASK_START: {
            ForgeSched::Protocol::DTO::TaskStartRequest start_req;
            if (!ForgeSched::Protocol::DTO::fromJson(request.data, start_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid task start request");
            }

            auto task = task_service_.getTask(start_req.task_id);
            if (!task) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            }

            if (task->getStatus() != TaskStatus::ASSIGNED) {
                LOG_WARN(LogModule::NETWORK, "TASK_START rejected: task not in ASSIGNED state");
                return buildResponse(request, ResponseCode::INVALID_STATE, "task not ready to start");
            }

            if (task->getWorkerId() != start_req.worker_id) {
                LOG_ERROR(LogModule::NETWORK, "TASK_START rejected: worker_id mismatch");
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "worker_id mismatch");
            }

            bool success = scheduler_.markTaskStarted(start_req.task_id, start_req.worker_id);
            if (!success) {
                return buildResponse(request, ResponseCode::INVALID_STATE, "failed to start task");
            }

            return buildResponse(request, ResponseCode::OK, "ok");
        }

        case ForgeSched::Protocol::MessageType::TASK_RESULT: {
            ForgeSched::Protocol::DTO::TaskResultRequest result_req;
            if (!ForgeSched::Protocol::DTO::fromJson(request.data, result_req, error)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid task result request");
            }

            if (result_req.status != TaskStatus::SUCCEEDED &&
                result_req.status != TaskStatus::FAILED &&
                result_req.status != TaskStatus::TIMEOUT) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid task result status");
            }

            auto task = task_service_.getTask(result_req.task_id);
            if (!task) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            }

            if (task->getWorkerId() != result_req.worker_id) {
                LOG_ERROR(LogModule::NETWORK, "TASK_RESULT rejected: worker_id mismatch");
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "worker_id mismatch");
            }

            auto summary = executionSummary(result_req.message, result_req.status);
            if (!summary.is_null() && task->getStatus() == result_req.status &&
                task->getExecutionSummary() == summary) {
                return buildResponse(request, ResponseCode::OK, "ok");
            }
            if (task->getStatus() != TaskStatus::RUNNING) {
                LOG_WARN(LogModule::NETWORK, "TASK_RESULT rejected: task not in RUNNING state");
               return buildResponse(request, ResponseCode::INVALID_STATE, "task not running");
            }
            bool success = scheduler_.completeTask(result_req.task_id, result_req.status, std::move(summary));
            if (!success) {
                return buildResponse(request, ResponseCode::INTERNAL_ERROR, "failed to complete task");
            }

            scheduling_driver_.requestSchedule();

            return buildResponse(request, ResponseCode::OK, "ok");
        }

        default:
            return buildResponse(request, ResponseCode::INVALID_REQUEST, "unknown message type");
    }
}

ProtocolMessage ProtocolRouter::buildResponse(
    const ProtocolMessage& request,
    ResponseCode code,
    const std::string& message,
    const nlohmann::json& data
) const {
    ProtocolMessage response;
    response.version = ForgeSched::Protocol::PROTOCOL_VERSION;
    response.type = ForgeSched::Protocol::MessageType::RESPONSE;
    response.request_id = request.request_id;

    ForgeSched::Protocol::DTO::Response dto;
    dto.code = static_cast<int>(code);
    dto.message = message;
    dto.data = data;

    response.data = ForgeSched::Protocol::DTO::toJson(dto);
    return response;
}

nlohmann::json ProtocolRouter::taskToJson(const Task& task) const {
    nlohmann::json j;
    j["task_id"] = task.getId();
    j["task_type"] = toString(task.getType());
    j["status"] = toString(task.getStatus());
    j["target"] = task.getTarget();
    j["revision"] = task.getRevision();
    j["worker_id"] = task.getWorkerId();
    j["priority"] = task.getPriority();
    j["payload"] = task.getPayload();
    if (!task.getExecutionSummary().is_null()) j["execution"] = task.getExecutionSummary();
    j["retry_count"] = task.getRetryCount();

    auto created = task.getCreatedAt();
    auto started = task.getStartedAt();
    auto finished = task.getFinishedAt();

    if (created.time_since_epoch().count() > 0) {
        j["created_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            created.time_since_epoch()
        ).count();
    }

    if (started.time_since_epoch().count() > 0) {
        j["started_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            started.time_since_epoch()
        ).count();
    }

    if (finished.time_since_epoch().count() > 0) {
        j["finished_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            finished.time_since_epoch()
        ).count();
    }

    return j;
}

} // namespace ForgeSched::Protocol
