#ifndef FORGESCHED_TASK_DTO_H
#define FORGESCHED_TASK_DTO_H

#include "../ProtocolError.h"
#include "task/Task.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include "nlohmann/json.hpp"
#include <string>

namespace ForgeSched::Protocol::DTO {

struct SubmitTaskRequest {
    TaskType task_type{TaskType::UNKNOWN};
    std::string target;
    std::string revision;
    int priority{0};
};

struct QueryTaskRequest {
    TaskId task_id;
};

struct CancelTaskRequest {
    TaskId task_id;
};

struct TaskStartRequest {
    TaskId task_id;
    std::string worker_id;
};

struct TaskResultRequest {
    TaskId task_id;
    std::string worker_id;
    TaskStatus status;
    std::string message;
};

struct TaskAssignRequest {
    TaskId task_id{0};
    TaskType task_type{TaskType::UNKNOWN};
    std::string target;
    std::string revision;
    int priority{0};
};

// Conversion functions
bool fromJson(const nlohmann::json& j, SubmitTaskRequest& out);
nlohmann::json toJson(const SubmitTaskRequest& value);

bool fromJson(const nlohmann::json& j, QueryTaskRequest& out);
nlohmann::json toJson(const QueryTaskRequest& value);

bool fromJson(const nlohmann::json& j, CancelTaskRequest& out);
nlohmann::json toJson(const CancelTaskRequest& value);

bool fromJson(const nlohmann::json& j, TaskStartRequest& out);
nlohmann::json toJson(const TaskStartRequest& value);

bool fromJson(const nlohmann::json& j, TaskResultRequest& out, ProtocolError& error);
nlohmann::json toJson(const TaskResultRequest& value);

bool fromJson(const nlohmann::json& j, TaskAssignRequest& out);
nlohmann::json toJson(const TaskAssignRequest& value);

} // namespace ForgeSched::Protocol::DTO

#endif // FORGESCHED_TASK_DTO_H
