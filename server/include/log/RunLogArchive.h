#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include "nlohmann/json.hpp"

namespace GCRP {
// A bounded, per-run archive. All path components except the configured root
// are validated identities or names from a fixed allowlist.
class RunLogArchive {
public:
    explicit RunLogArchive(std::filesystem::path root);
    static bool validRunId(const std::string& value);
    static bool validName(const std::string& value);
    nlohmann::json append(uint64_t task_id, const std::string& run_id,
                          const std::string& name, uint64_t offset,
                          const std::string& hex, bool complete);
    nlohmann::json list(uint64_t task_id, const std::string& run_id);
    nlohmann::json read(uint64_t task_id, const std::string& run_id,
                        const std::string& name, uint64_t offset, size_t limit);
    size_t pruneOlderThan(int days);
private:
    std::filesystem::path file(uint64_t task_id, const std::string& run_id,
                               const std::string& name) const;
    std::filesystem::path root_;
    std::mutex mutex_;
};
} // namespace GCRP
