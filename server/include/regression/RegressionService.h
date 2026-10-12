#pragma once

#include "scheduler/Scheduler.h"
#include "nlohmann/json.hpp"
#include <optional>
#include <string>

namespace ForgeSched {

struct RegressionQuery {
    std::string revision;
    std::string case_path;
    std::string flow;
    std::string batch_id;
    size_t limit{20};
    TaskId before_task_id{0};

    static std::optional<RegressionQuery> parse(const nlohmann::json& request);
};

// Read-only view over one-case Tasks. This does not infer a PJTest parent task
// or invent a business result for an infrastructure failure.
class RegressionService {
public:
    explicit RegressionService(const Scheduler& scheduler) : scheduler_(scheduler) {}
    nlohmann::json query(const RegressionQuery& filter) const;

private:
    const Scheduler& scheduler_;
};

} // namespace ForgeSched
