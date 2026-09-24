#include "protocol/dto/TaskDTO.h"
#include "protocol/ProtocolError.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include <unordered_map>

namespace ForgeSched::Protocol::DTO {

static TaskType taskTypeFromString(const std::string& str) {
    static const std::unordered_map<std::string, TaskType> map = {
        {"REGRESSION", TaskType::REGRESSION},
        {"UNKNOWN", TaskType::UNKNOWN}
    };
    auto it = map.find(str);
    return (it != map.end()) ? it->second : TaskType::UNKNOWN;
}

static std::string taskTypeToString(TaskType type) {
    return toString(type);
}

static TaskStatus taskStatusFromString(const std::string& str) {
    static const std::unordered_map<std::string, TaskStatus> map = {
        {"PENDING", TaskStatus::PENDING},
        {"QUEUED", TaskStatus::QUEUED},
        {"RUNNING", TaskStatus::RUNNING},
        {"SUCCEEDED", TaskStatus::SUCCEEDED},
        {"FAILED", TaskStatus::FAILED},
        {"TIMEOUT", TaskStatus::TIMEOUT},
        {"CANCELLED", TaskStatus::CANCELLED}
    };
    auto it = map.find(str);
    return (it != map.end()) ? it->second : TaskStatus::PENDING;
}

static std::string taskStatusToString(TaskStatus status) {
    return toString(status);
}

// SubmitTaskRequest
bool fromJson(const nlohmann::json& j, SubmitTaskRequest& out) {
    if (!j.contains("task_type") || !j["task_type"].is_string()) {
        return false;
    }
    out.task_type = taskTypeFromString(j["task_type"].get<std::string>());

    if (!j.contains("target") || !j["target"].is_string()) {
        return false;
    }
    out.target = j["target"].get<std::string>();
    if (out.target.empty()) {
        return false;
    }

    if (!j.contains("revision") || !j["revision"].is_string()) {
        return false;
    }
    out.revision = j["revision"].get<std::string>();
    if (out.revision.empty()) {
        return false;
    }

    if (j.contains("priority")) {
        if (!j["priority"].is_number_integer() || j["priority"] < -100 || j["priority"] > 100)
            return false;
        out.priority = j["priority"].get<int>();
    }

    return out.task_type != TaskType::UNKNOWN;
}

nlohmann::json toJson(const SubmitTaskRequest& value) {
    nlohmann::json j;
    j["task_type"] = taskTypeToString(value.task_type);
    j["target"] = value.target;
    j["revision"] = value.revision;
    j["priority"] = value.priority;
    return j;
}

// QueryTaskRequest
bool fromJson(const nlohmann::json& j, QueryTaskRequest& out) {
    if (!j.contains("task_id") || !j["task_id"].is_number_unsigned()) {
        return false;
    }
    out.task_id = j["task_id"].get<TaskId>();
    return out.task_id > 0;
}

nlohmann::json toJson(const QueryTaskRequest& value) {
    nlohmann::json j;
    j["task_id"] = value.task_id;
    return j;
}

// CancelTaskRequest
bool fromJson(const nlohmann::json& j, CancelTaskRequest& out) {
    if (!j.contains("task_id") || !j["task_id"].is_number_unsigned()) {
        return false;
    }
    out.task_id = j["task_id"].get<TaskId>();
    return out.task_id > 0;
}

nlohmann::json toJson(const CancelTaskRequest& value) {
    nlohmann::json j;
    j["task_id"] = value.task_id;
    return j;
}

// TaskStartRequest
bool fromJson(const nlohmann::json& j, TaskStartRequest& out) {
    if (!j.contains("task_id") || !j["task_id"].is_number_unsigned()) {
        return false;
    }
    out.task_id = j["task_id"].get<TaskId>();

    if (!j.contains("worker_id") || !j["worker_id"].is_string()) {
        return false;
    }
    out.worker_id = j["worker_id"].get<std::string>();

    return true;
}

nlohmann::json toJson(const TaskStartRequest& value) {
    nlohmann::json j;
    j["task_id"] = value.task_id;
    j["worker_id"] = value.worker_id;
    return j;
}

// TaskResultRequest
bool fromJson(const nlohmann::json& j, TaskResultRequest& out, ProtocolError& error) {
    if (!j.contains("task_id") || !j["task_id"].is_number_unsigned()) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: task_id";
        return false;
    }
    out.task_id = j["task_id"].get<TaskId>();

    if (!j.contains("worker_id") || !j["worker_id"].is_string()) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: worker_id";
        return false;
    }
    out.worker_id = j["worker_id"].get<std::string>();

    if (!j.contains("status") || !j["status"].is_string()) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: status";
        return false;
    }

    std::string status_str = j["status"].get<std::string>();
    TaskStatus status = taskStatusFromString(status_str);

    // For TaskResult, only allow SUCCEEDED, FAILED, TIMEOUT
    if (status != TaskStatus::SUCCEEDED &&
        status != TaskStatus::FAILED &&
        status != TaskStatus::TIMEOUT) {
        error.code = ProtocolErrorCode::INVALID_FIELD_VALUE;
        error.message = "Invalid status for TASK_RESULT: " + status_str + " (must be SUCCEEDED, FAILED, or TIMEOUT)";
        return false;
    }
    out.status = status;

    // message is optional
    if (j.contains("message") && j["message"].is_string()) {
        out.message = j["message"].get<std::string>();
    }

    return true;
}

nlohmann::json toJson(const TaskResultRequest& value) {
    nlohmann::json j;
    j["task_id"] = value.task_id;
    j["worker_id"] = value.worker_id;
    j["status"] = taskStatusToString(value.status);
    j["message"] = value.message;
    return j;
}

// TaskAssignRequest
bool fromJson(const nlohmann::json& j, TaskAssignRequest& out) {
    if (!j.contains("task_id") || !j["task_id"].is_number_unsigned()) {
        return false;
    }
    out.task_id = j["task_id"].get<TaskId>();
    if (out.task_id == 0) {
        return false;
    }

    if (!j.contains("task_type") || !j["task_type"].is_string()) {
        return false;
    }
    out.task_type = taskTypeFromString(j["task_type"].get<std::string>());
    if (out.task_type == TaskType::UNKNOWN) {
        return false;
    }

    if (!j.contains("target") || !j["target"].is_string()) {
        return false;
    }
    out.target = j["target"].get<std::string>();
    if (out.target.empty()) {
        return false;
    }

    if (!j.contains("revision") || !j["revision"].is_string()) {
        return false;
    }
    out.revision = j["revision"].get<std::string>();
    if (out.revision.empty()) {
        return false;
    }

    if (j.contains("priority")) {
        if (!j["priority"].is_number_integer() || j["priority"] < -100 || j["priority"] > 100)
            return false;
        out.priority = j["priority"].get<int>();
    }

    return true;
}

nlohmann::json toJson(const TaskAssignRequest& value) {
    nlohmann::json j;
    j["task_id"] = value.task_id;
    j["task_type"] = taskTypeToString(value.task_type);
    j["target"] = value.target;
    j["revision"] = value.revision;
    j["priority"] = value.priority;
    return j;
}

} // namespace ForgeSched::Protocol::DTO
