#ifndef FORGESCHED_RESPONSE_H
#define FORGESCHED_RESPONSE_H

#include "nlohmann/json.hpp"
#include <string>

namespace ForgeSched::Protocol::DTO {

struct Response {
    int code{0};
    std::string message;
    nlohmann::json data;
};

bool fromJson(const nlohmann::json& j, Response& out);
nlohmann::json toJson(const Response& value);

} // namespace ForgeSched::Protocol::DTO

#endif // FORGESCHED_RESPONSE_H
