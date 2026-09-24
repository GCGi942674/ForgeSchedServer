#include "protocol/dto/Response.h"

namespace ForgeSched::Protocol::DTO {

bool fromJson(const nlohmann::json& j, Response& out) {
    if (j.contains("code") && j["code"].is_number_integer()) {
        out.code = j["code"].get<int>();
    }

    if (j.contains("message") && j["message"].is_string()) {
        out.message = j["message"].get<std::string>();
    }

    if (j.contains("result") && j["result"].is_object()) {
        out.data = j["result"];
    } else if (j.contains("data") && j["data"].is_object()) {
        out.data = j["data"];
    }

    return true;
}

nlohmann::json toJson(const Response& value) {
    nlohmann::json j;
    j["code"] = value.code;
    j["message"] = value.message;
    j["result"] = value.data;
    return j;
}

} // namespace ForgeSched::Protocol::DTO