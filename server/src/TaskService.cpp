#include "task/TaskService.h"
#include "task/Task.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include "task/TaskLimits.h"
#include "task/RegressionPayload.h"
#include "config/Config.h"
#include "scheduler/SchedulingDriver.h"
#include "Logging.h"
#include <filesystem>
#include <fstream>

namespace ForgeSched {


bool TaskService::isPriorityValid(int priority) const {
    return priority >= -100 && priority <= 100;
}

TaskService::TaskService(Scheduler& scheduler, SchedulingDriver& scheduling_driver)
    : scheduler_(scheduler)
    , scheduling_driver_(scheduling_driver)
{
}

std::optional<TaskId> TaskService::createTask(const CreateTaskRequest& request) {
    if (request.target.size() > kMaxTaskMetadataBytes ||
        request.revision.size() > kMaxTaskMetadataBytes - request.target.size()) {
        LOG_WARN(LogModule::TASK, "Task metadata exceeds size limit");
        return std::nullopt;
    }
    if (!validRegressionPayload(request.payload)) return std::nullopt;
    nlohmann::json payload = request.payload;
    if (!payload.empty() && payload["spec_version"] == 1) {
        const auto directory = Config::instance().getString("server.templates_dir", "");
        if (!directory.empty()) {
            const std::filesystem::path file =
                std::filesystem::path(directory) / (payload["flow"].get<std::string>() + ".json");
            std::ifstream stream(file);
            if (!stream) {
                LOG_WARN(LogModule::TASK, "Missing server flow template: " + file.string());
                return std::nullopt;
            }
            const auto source = nlohmann::json::parse(stream, nullptr, false);
            if (!source.is_object() || !source.contains("flow_config") ||
                !validFlowConfig(source["flow_config"])) return std::nullopt;
            payload["spec_version"] = 2;
            payload["flow_config"] = source["flow_config"];
            if (!validRegressionPayload(payload)) return std::nullopt;
        }
    }
    if (request.type == TaskType::UNKNOWN) {
        LOG_WARN(LogModule::TASK, "Invalid create request: unknown task type");
        return std::nullopt;
    }

    if (request.target.empty()) {
        LOG_WARN(LogModule::TASK, "Invalid create request: empty target");
        return std::nullopt;
    }

    if (request.revision.empty()) {
        LOG_WARN(LogModule::TASK, "Invalid create request: empty revision");
        return std::nullopt;
    }

    if (!isPriorityValid(request.priority)) {
        LOG_WARN(LogModule::TASK, "Invalid create request: priority out of range");
        return std::nullopt;
    }

    TaskId id = id_generator_.next();

    Task task(id, request.type, request.target, request.revision);
    task.setPriority(request.priority);
    task.setPayload(std::move(payload));

    if (!scheduler_.submitTask(std::move(task))) {
        LOG_ERROR(LogModule::TASK, "Generated task could not be submitted to Scheduler");
        return std::nullopt;
    }

    LOG_INFO(LogModule::TASK, "task=" + std::to_string(id) + " created type=" + toString(request.type) + " target=" + request.target + " revision=" + request.revision);

    scheduling_driver_.requestSchedule();

    return id;
}

std::optional<Task> TaskService::getTask(TaskId id) const {
    return scheduler_.getTask(id);
}

bool TaskService::cancelTask(TaskId id) {
    if (scheduler_.cancelTask(id)) {
        scheduling_driver_.requestSchedule();
        return true;
    }
    return false;
}

} // namespace ForgeSched
