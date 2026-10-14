#pragma once

#include "scheduler/Scheduler.h"
#include "nlohmann/json.hpp"
#include <optional>
#include <string>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace GCRP {

struct RegressionQuery {
    std::string revision;
    std::string case_path;
    std::string flow;
    std::string batch_id;
    std::string date, baseline_date, template_name, search, change, after_key;
    size_t limit{20};
    TaskId before_task_id{0};

    static std::optional<RegressionQuery> parse(const nlohmann::json& request);
};

// Read-only view over one-case Tasks. This does not infer a PJTest parent task
// or invent a business result for an infrastructure failure.
class RegressionService {
public:
    explicit RegressionService(const Scheduler& scheduler, bool cache_enabled = false,
                               size_t max_entries = 1000, int ttl_seconds = 5)
        : scheduler_(scheduler), cache_enabled_(cache_enabled),
          max_entries_(max_entries), ttl_seconds_(ttl_seconds) {}
    nlohmann::json query(const RegressionQuery& filter) const;
    nlohmann::json compareDaily(const RegressionQuery& filter) const;
    nlohmann::json cacheStats() const;

private:
    struct CacheEntry {
        nlohmann::json response;
        std::chrono::steady_clock::time_point expires_at;
    };
    const Scheduler& scheduler_;
    const bool cache_enabled_;
    const size_t max_entries_;
    const int ttl_seconds_;
    mutable std::mutex cache_mutex_;
    mutable uint64_t cached_generation_{0};
    mutable std::unordered_map<std::string, CacheEntry> cache_;
    mutable uint64_t hits_{0}, misses_{0}, invalidations_{0};
};

} // namespace GCRP
