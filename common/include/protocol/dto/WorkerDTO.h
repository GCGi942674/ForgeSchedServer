#ifndef FORGESCHED_WORKER_DTO_H
#define FORGESCHED_WORKER_DTO_H

#include "../ProtocolError.h"
#include "nlohmann/json.hpp"
#include <string>
#include <cstdint>

namespace ForgeSched::Protocol::DTO {

struct WorkerRegisterRequest {
    std::string worker_id;
    std::string hostname;
    uint32_t slots{0};
};

struct WorkerHeartbeatRequest {
    std::string worker_id;
};

// Conversion functions
bool fromJson(const nlohmann::json& j, WorkerRegisterRequest& out, ProtocolError& error);
nlohmann::json toJson(const WorkerRegisterRequest& value);

bool fromJson(const nlohmann::json& j, WorkerHeartbeatRequest& out);
nlohmann::json toJson(const WorkerHeartbeatRequest& value);

} // namespace ForgeSched::Protocol::DTO

#endif // FORGESCHED_WORKER_DTO_H
