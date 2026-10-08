#include "TestSupport.h"
#include "scheduler/SchedulingCoordinator.h"
#include <condition_variable>
#include <functional>
using namespace ForgeSched;
struct Dispatcher : WorkerDispatcher {
    std::function<bool(const Task&)> action;
    bool dispatch(const WorkerId&, const Task& task) override { return action(task); }
};
int main() { return testMain([] {
    {
        WorkerManager workers; CHECK(workers.registerWorker("w", "host", 2));
        Scheduler scheduler(workers); Dispatcher dispatcher;
        SchedulingCoordinator coordinator(scheduler, dispatcher);
        CHECK(scheduler.submitTask(Task(1, TaskType::REGRESSION, "a", "r")));
        CHECK(scheduler.submitTask(Task(2, TaskType::REGRESSION, "b", "r")));
        int sent = 0;
        dispatcher.action = [&](const Task& task) {
            ++sent;
            CHECK(task.getId() == 1);
            CHECK(!scheduler.cancelTask(2)); // ASSIGNED cannot be cancelled.
            CHECK(scheduler.rollbackAssignment(2)); // Unsent assignment may still roll back.
            return true;
        };
        auto result = coordinator.runOnce();
        CHECK(sent == 1 && result.dispatched_count == 1);
        CHECK(scheduler.getTask(2)->getStatus() == TaskStatus::QUEUED);
        CHECK(scheduler.cancelTask(2));
        CHECK(workers.getWorker("w")->getUsedSlots() == 1);
    }
    for (int outcome : {0, 1, 2}) {
        WorkerManager workers; CHECK(workers.registerWorker("w", "host", 1));
        Scheduler scheduler(workers); Dispatcher dispatcher;
        SchedulingCoordinator coordinator(scheduler, dispatcher);
        CHECK(scheduler.submitTask(Task(1, TaskType::REGRESSION, "a", "r")));
        std::mutex mutex; std::condition_variable cv;
        bool entered = false, release = false;
        dispatcher.action = [&](const Task&) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true; cv.notify_all();
            cv.wait(lock, [&] { return release; });
            if (outcome == 2) throw std::runtime_error("injected");
            return outcome == 1;
        };
        std::exception_ptr error;
        std::thread thread([&] {
            try { coordinator.runOnce(); } catch (...) { error = std::current_exception(); }
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return entered; });
        }
        const bool cancelled = scheduler.cancelTask(1);
        const bool rolled_back = scheduler.rollbackAssignment(1);
        const auto slots = workers.getWorker("w")->getUsedSlots();
        {
            std::lock_guard<std::mutex> lock(mutex); release = true;
        }
        cv.notify_all(); thread.join();
        if (error) std::rethrow_exception(error);
        CHECK(!cancelled && !rolled_back && slots == 1);
        CHECK(workers.getWorker("w")->getUsedSlots() == (outcome == 1 ? 1u : 0u));
        CHECK(scheduler.cancelTask(1) == (outcome != 1));
        // Only rejected/thrown dispatches return to QUEUED and become cancellable.
        if (outcome == 1) {
            CHECK(scheduler.getTask(1)->getStatus() == TaskStatus::ASSIGNED);
            CHECK(workers.getWorker("w")->getUsedSlots() == 1);
        }
    }
}); }
