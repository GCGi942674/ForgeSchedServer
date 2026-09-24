#include "scheduler/SchedulingCoordinator.h"
#include "scheduler/Scheduler.h"
#include "task/Task.h"
#include "Logging.h"
#include <stdexcept>

namespace ForgeSched {

SchedulingCoordinator::SchedulingCoordinator(
    Scheduler& scheduler,
    WorkerDispatcher& dispatcher
)
    : scheduler_(scheduler)
    , dispatcher_(dispatcher)
{
}

SchedulingResult SchedulingCoordinator::runOnce() {
    SchedulingResult result;

    auto assignments = scheduler_.schedule();
    result.assigned_count = assignments.size();

    for (const auto& assignment : assignments) {
        auto task = scheduler_.getTask(assignment.task_id);

        if (!task) {
            LOG_ERROR(LogModule::SCHEDULER, "Assigned task not found: " + std::to_string(assignment.task_id));
            continue;
        }

        bool dispatch_success = false;
        bool dispatch_threw = false;

        try {
            dispatch_success = dispatcher_.dispatch(assignment.worker_id, *task);
        } catch (const std::exception& e) {
            dispatch_threw = true;
            LOG_ERROR(LogModule::SCHEDULER, "dispatch exception for task=" + std::to_string(assignment.task_id) + " worker=" + assignment.worker_id + ": " + e.what());
        } catch (...) {
            dispatch_threw = true;
            LOG_ERROR(LogModule::SCHEDULER, "unknown dispatch exception for task=" + std::to_string(assignment.task_id) + " worker=" + assignment.worker_id);
        }

        if (dispatch_success) {
            result.dispatched_count++;
            LOG_INFO(LogModule::SCHEDULER, "task=" + std::to_string(assignment.task_id) + " dispatch accepted worker=" + assignment.worker_id);
            continue;
        }

        if (dispatch_threw) {
            LOG_WARN(LogModule::SCHEDULER, "task=" + std::to_string(assignment.task_id) + " dispatch failed (exception), rolling back");
        }

        result.dispatch_failed_count++;

        if (!scheduler_.rollbackAssignment(assignment.task_id)) {
            result.rollback_failed_count++;
            LOG_ERROR(LogModule::SCHEDULER, "rollback failed after dispatch failure for task=" + std::to_string(assignment.task_id));
        } else {
            LOG_WARN(LogModule::SCHEDULER, "task=" + std::to_string(assignment.task_id) + " dispatch failed worker=" + assignment.worker_id + ", rolling back");
        }
    }

    return result;
}

} // namespace ForgeSched