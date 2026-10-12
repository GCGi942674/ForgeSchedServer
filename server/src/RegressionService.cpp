#include "regression/RegressionService.h"
#include "task/TaskType.h"
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace ForgeSched {
namespace {
bool readFilter(const nlohmann::json& request, const char* name,
                std::string& value, size_t max_length) {
    if (!request.contains(name)) return true;
    if (!request[name].is_string()) return false;
    value = request[name].get<std::string>();
    return !value.empty() && value.size() <= max_length;
}
std::string field(const nlohmann::json& object, const char* name) {
    if (!object.is_object() || !object.contains(name) || !object[name].is_string()) return {};
    return object[name].get<std::string>();
}
std::string batchOf(const nlohmann::json& payload) {
    if (!payload.is_object() || !payload.contains("context")) return {};
    return field(payload["context"], "batch_id");
}
std::string outcomeOf(const Task& task) {
    switch (task.getStatus()) {
        case TaskStatus::TIMEOUT: return "TIMEOUT";
        case TaskStatus::CANCELLED: return "CANCELLED";
        case TaskStatus::PENDING:
        case TaskStatus::QUEUED:
        case TaskStatus::ASSIGNED:
        case TaskStatus::RUNNING: return "ACTIVE";
        default: break;
    }
    const auto case_status = field(task.getExecutionSummary(), "case_status");
    if (task.getStatus() == TaskStatus::SUCCEEDED && case_status == "PASS") return "PASS";
    if (task.getStatus() == TaskStatus::FAILED) {
        if (case_status == "FAIL") return "FAIL";
        if (case_status == "CRASH") return "CRASH";
        return "ERROR";
    }
    return "UNKNOWN";
}
int64_t millis(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}
bool matches(const Task& task, const RegressionQuery& filter) {
    if (task.getType() != TaskType::REGRESSION ||
        !task.getPayload().is_object() ||
        field(task.getPayload(), "case").empty()) return false;
    if (!filter.revision.empty() && task.getRevision() != filter.revision) return false;
    if (!filter.case_path.empty() && field(task.getPayload(), "case") != filter.case_path) return false;
    if (!filter.flow.empty() && field(task.getPayload(), "flow") != filter.flow) return false;
    if (!filter.batch_id.empty() && batchOf(task.getPayload()) != filter.batch_id) return false;
    return true;
}
nlohmann::json rowOf(const Task& task) {
    const auto& payload = task.getPayload();
    const auto& execution = task.getExecutionSummary();
    nlohmann::json row = {{"task_id", task.getId()}, {"revision", task.getRevision()},
        {"target", task.getTarget()}, {"case", field(payload, "case")},
        {"flow", field(payload, "flow")}, {"batch_id", batchOf(payload)},
        {"status", toString(task.getStatus())}, {"outcome", outcomeOf(task)},
        {"worker_id", task.getWorkerId()}, {"run_id", task.getRunId()},
        {"retry_count", task.getRetryCount()},
        {"created_at_ms", millis(task.getCreatedAt())}};
    if (task.getFinishedAt().time_since_epoch().count() > 0)
        row["finished_at_ms"] = millis(task.getFinishedAt());
    for (const char* key : {"reason", "case_status", "case_reason"}) {
        const auto value = field(execution, key);
        if (!value.empty()) row[key] = value;
    }
    return row;
}
} // namespace

std::optional<RegressionQuery> RegressionQuery::parse(const nlohmann::json& request) {
    if (!request.is_object() || request.size() > 6) return std::nullopt;
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != "revision" && it.key() != "case" && it.key() != "flow" &&
            it.key() != "batch_id" && it.key() != "limit" && it.key() != "before_task_id")
            return std::nullopt;
    RegressionQuery result;
    if (!readFilter(request, "revision", result.revision, 128) ||
        !readFilter(request, "case", result.case_path, 512) ||
        !readFilter(request, "flow", result.flow, 64) ||
        !readFilter(request, "batch_id", result.batch_id, 128) ||
        (result.revision.empty() && result.case_path.empty() && result.batch_id.empty()))
        return std::nullopt;
    if (request.contains("limit")) {
        if (!request["limit"].is_number_integer()) return std::nullopt;
        const auto limit = request["limit"].get<int64_t>();
        if (limit < 1 || limit > 50) return std::nullopt;
        result.limit = static_cast<size_t>(limit);
    }
    if (request.contains("before_task_id")) {
        if (!request["before_task_id"].is_number_integer()) return std::nullopt;
        if (!request["before_task_id"].is_number_unsigned() &&
            request["before_task_id"].get<int64_t>() <= 0) return std::nullopt;
        result.before_task_id = request["before_task_id"].get<TaskId>();
        if (!result.before_task_id) return std::nullopt;
    }
    return result;
}

nlohmann::json RegressionService::query(const RegressionQuery& filter) const {
    auto tasks = scheduler_.getTasks();
    std::sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) {
        return a.getId() > b.getId();
    });
    nlohmann::json counts = {{"PASS", 0}, {"FAIL", 0}, {"TIMEOUT", 0},
        {"CRASH", 0}, {"ERROR", 0}, {"CANCELLED", 0},
        {"ACTIVE", 0}, {"UNKNOWN", 0}};
    nlohmann::json rows = nlohmann::json::array();
    uint64_t total = 0;
    bool has_more = false;
    for (const auto& task : tasks) {
        if (!matches(task, filter)) continue;
        ++total;
        const auto outcome = outcomeOf(task);
        counts[outcome] = counts[outcome].get<uint64_t>() + 1;
        if (filter.before_task_id && task.getId() >= filter.before_task_id) continue;
        if (rows.size() < filter.limit) rows.push_back(rowOf(task));
        else has_more = true;
    }
    nlohmann::json response = {{"filters", {{"revision", filter.revision},
        {"case", filter.case_path}, {"flow", filter.flow}, {"batch_id", filter.batch_id}}},
        {"total", total}, {"counts", counts}, {"items", rows},
        {"has_more", has_more}, {"next_before_task_id", nullptr}};
    if (has_more) response["next_before_task_id"] = rows.back()["task_id"];
    return response;
}

} // namespace ForgeSched
