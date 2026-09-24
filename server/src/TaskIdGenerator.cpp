#include "task/TaskIdGenerator.h"

namespace ForgeSched {

TaskId TaskIdGenerator::next() {
    return next_id_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace ForgeSched
