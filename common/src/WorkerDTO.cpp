#include "protocol/dto/WorkerDTO.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include <limits>

namespace ForgeSched::Protocol::DTO {

// WorkerRegisterRequest
bool fromJson(const nlohmann::json& j, WorkerRegisterRequest& out, ProtocolError& error) {
    if (!j.contains("worker_id") || !j["worker_id"].is_string()) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: worker_id";
        return false;
    }
    out.worker_id = j["worker_id"].get<std::string>();
    if (out.worker_id.empty()) {
        error.code = ProtocolErrorCode::INVALID_FIELD_VALUE;
        error.message = "worker_id cannot be empty";
        return false;
    }

    if (!j.contains("hostname") || !j["hostname"].is_string()) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: hostname";
        return false;
    }
    out.hostname = j["hostname"].get<std::string>();
    if (out.hostname.empty()) {
        error.code = ProtocolErrorCode::INVALID_FIELD_VALUE;
        error.message = "hostname cannot be empty";
        return false;
    }

    if (j.contains("slots") && j["slots"].is_number_unsigned()) {
        const auto slots = j["slots"].get<uint64_t>();
        if (slots == 0 || slots > std::numeric_limits<uint32_t>::max()) {
            error.code = ProtocolErrorCode::INVALID_FIELD_VALUE;
            error.message = "slots must be greater than 0";
            return false;
        }
        out.slots = static_cast<uint32_t>(slots);
    } else {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: slots";
        return false;
    }

    return true;
}

nlohmann::json toJson(const WorkerRegisterRequest& value) {
    nlohmann::json j;
    j["worker_id"] = value.worker_id;
    j["hostname"] = value.hostname;
    j["slots"] = value.slots;
    return j;
}

// WorkerHeartbeatRequest
bool fromJson(const nlohmann::json& j, WorkerHeartbeatRequest& out) {
    if (!j.contains("worker_id") || !j["worker_id"].is_string()) {
        return false;
    }
    out.worker_id = j["worker_id"].get<std::string>();
    return !out.worker_id.empty();
}

nlohmann::json toJson(const WorkerHeartbeatRequest& value) {
    nlohmann::json j;
    j["worker_id"] = value.worker_id;
    return j;
}

} // namespace ForgeSched::Protocol::DTO
