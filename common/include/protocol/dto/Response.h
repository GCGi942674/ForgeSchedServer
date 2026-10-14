#ifndef GCRP_RESPONSE_H
#define GCRP_RESPONSE_H

#include "nlohmann/json.hpp"
#include <string>

namespace GCRP::Protocol::DTO {

struct Response {
    int code{0};
    std::string message;
    nlohmann::json data;
};

bool fromJson(const nlohmann::json& j, Response& out);
nlohmann::json toJson(const Response& value);

} // namespace GCRP::Protocol::DTO

#endif // GCRP_RESPONSE_H
