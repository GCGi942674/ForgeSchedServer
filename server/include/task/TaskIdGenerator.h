#ifndef FORGESCHED_TASK_ID_GENERATOR_H
#define FORGESCHED_TASK_ID_GENERATOR_H

#include <cstdint>
#include <atomic>

namespace ForgeSched {

using TaskId = uint64_t;

class TaskIdGenerator {
public:
    TaskIdGenerator() = default;
    ~TaskIdGenerator() = default;

    TaskIdGenerator(const TaskIdGenerator&) = delete;
    TaskIdGenerator& operator=(const TaskIdGenerator&) = delete;

    TaskId next();

    TaskIdGenerator(TaskIdGenerator&&) = delete;
    TaskIdGenerator& operator=(TaskIdGenerator&&) = delete;

private:
    std::atomic<TaskId> next_id_{1};
};

} // namespace ForgeSched

#endif // FORGESCHED_TASK_ID_GENERATOR_H