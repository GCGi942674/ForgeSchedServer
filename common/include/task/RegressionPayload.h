#pragma once
#include "nlohmann/json.hpp"
#include <string>
namespace ForgeSched {
inline bool validRegressionPayload(const nlohmann::json& p) {
    if (!p.is_object() || p.dump().size() > 4096) return false;
    if (p.empty()) return true; // Legacy demo tasks.
    if (p.size() != 4 || !p.contains("spec_version") || !p.contains("case") ||
        !p.contains("flow") || !p.contains("timeout_seconds")) return false;
    if (!p["spec_version"].is_number_integer() || p["spec_version"] != 1 ||
        !p["case"].is_string() || !p["flow"].is_string() ||
        !p["timeout_seconds"].is_number_unsigned()) return false;
    const std::string path = p["case"].get<std::string>();
    const std::string flow = p["flow"].get<std::string>();
    if (path.empty() || path.size() > 512 || path.front() == '/' ||
        path.back() == '/' || path.find('\\') != std::string::npos ||
        path.find("//") != std::string::npos ||
        path.size() < 9 || path.substr(path.size()-8) != "/run.tcl")
        return false;
    size_t pos = 0;
    while (pos < path.size()) {
        const auto end = path.find('/', pos);
        const auto part = path.substr(pos, end == std::string::npos ? end : end-pos);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char ch : part)
            if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
                !(ch >= '0' && ch <= '9') && ch != '_' && ch != '-' && ch != '.') return false;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    if (flow.empty() || flow.size() > 64) return false;
    for (unsigned char ch : flow)
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
            !(ch >= '0' && ch <= '9') && ch != '_' && ch != '-') return false;
    const auto seconds = p["timeout_seconds"].get<uint64_t>();
    return seconds >= 1 && seconds <= 86400;
}
}
