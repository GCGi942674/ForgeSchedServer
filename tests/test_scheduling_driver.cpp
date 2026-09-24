#include "TestSupport.h"
#include "scheduler/SchedulingDriver.h"
#include "scheduler/SchedulingCoordinator.h"
#include "task/TaskService.h"
#include <condition_variable>
#include <functional>
#include <atomic>
using namespace ForgeSched;

struct Dispatcher : WorkerDispatcher {
    std::function<bool(const Task&)> action;
    std::atomic<int> calls{0};
    bool dispatch(const WorkerId&, const Task& task) override {
        ++calls;
        return action ? action(task) : true;
    }
};
struct Fixture {
    WorkerManager workers;
    Scheduler scheduler{workers};
    Dispatcher dispatcher;
    SchedulingCoordinator coordinator{scheduler, dispatcher};
    SchedulingDriver driver{coordinator};
    TaskService service{scheduler, driver};
    void submit(TaskId id) {
        CHECK(scheduler.submitTask(Task(id, TaskType::REGRESSION, "target", "rev")));
    }
};
int main() { return testMain([] {
    {
        Fixture f;
        auto a = f.service.createTask({TaskType::REGRESSION, "a", "r", 0}); CHECK(a);
        CHECK(f.scheduler.getTask(*a)->getStatus() == TaskStatus::QUEUED);
        CHECK(f.workers.registerWorker("w", "h", 1));
        f.driver.requestSchedule(); // Unit boundary; network registration tested separately.
        CHECK(f.scheduler.getTask(*a)->getStatus() == TaskStatus::ASSIGNED);
        auto b = f.service.createTask({TaskType::REGRESSION, "b", "r", 0}); CHECK(b);
        CHECK(f.scheduler.getTask(*b)->getStatus() == TaskStatus::QUEUED);
        CHECK(f.service.cancelTask(*a));
        CHECK(f.scheduler.getTask(*b)->getStatus() == TaskStatus::ASSIGNED);
        CHECK(f.dispatcher.calls == 2);
        CHECK(f.service.cancelTask(*b));
        CHECK(f.workers.getWorker("w")->getUsedSlots() == 0);
        CHECK(f.scheduler.getTasks().size() == 2);
    }
    {
        Fixture f;
        CHECK(f.workers.registerWorker("one", "h", 1));
        CHECK(f.workers.registerWorker("two", "h", 2));
        for (TaskId id = 1; id <= 4; ++id) f.submit(id);
        f.driver.requestSchedule();
        CHECK(f.dispatcher.calls == 3);
        CHECK(f.scheduler.getTasksByStatus(TaskStatus::ASSIGNED).size() == 3);
        CHECK(f.scheduler.getTasksByStatus(TaskStatus::QUEUED).size() == 1);
        CHECK(f.workers.getWorker("one")->getUsedSlots() == 1);
        CHECK(f.workers.getWorker("two")->getUsedSlots() == 2);
    }
    // Repeated concurrent arrivals also exercise transitions between active and idle.
    {
        Fixture f; CHECK(f.workers.registerWorker("w", "h", 200));
        std::vector<std::thread> callers;
        for (TaskId id = 1; id <= 100; ++id) callers.emplace_back([&, id] {
            f.submit(id); f.driver.requestSchedule();
        });
        for (auto& t : callers) t.join();
        CHECK(f.dispatcher.calls == 100);
        CHECK(f.scheduler.getTasksByStatus(TaskStatus::ASSIGNED).size() == 100);
        CHECK(f.workers.getWorker("w")->getUsedSlots() == 100);
    }
    for (bool throwing : {false, true}) {
        Fixture f; CHECK(f.workers.registerWorker("w", "h", 2));
        f.submit(1); f.submit(2);
        f.dispatcher.action = [throwing](const Task& t) {
            if (t.getId() == 1) return true;
            if (throwing) throw std::runtime_error("injected dispatch failure");
            return false;
        };
        f.driver.requestSchedule();
        CHECK(f.dispatcher.calls == 2); // Mixed success/failure must not self-retry.
        CHECK(f.scheduler.getTask(2)->getStatus() == TaskStatus::QUEUED);
        CHECK(f.workers.getWorker("w")->getUsedSlots() == 1);
        f.driver.requestSchedule();
        CHECK(f.dispatcher.calls == 3); // All-failed pass also returns.
        f.dispatcher.action = {};
        f.driver.requestSchedule();
        CHECK(f.dispatcher.calls == 4);
        CHECK(f.scheduler.getTask(2)->getStatus() == TaskStatus::ASSIGNED);
    }
    {
        Fixture f; CHECK(f.workers.registerWorker("w", "h", 2)); f.submit(1);
        f.dispatcher.action = [&](const Task& t) {
            if (t.getId() == 1) {
                f.submit(2);
                f.driver.requestSchedule(); // Same-thread reentry must not deadlock.
            }
            return true;
        };
        f.driver.requestSchedule();
        CHECK(f.dispatcher.calls == 2);
        CHECK(f.scheduler.getTasksByStatus(TaskStatus::ASSIGNED).size() == 2);
    }
    for (bool throwing : {false, true}) {
        Fixture f; CHECK(f.workers.registerWorker("w", "h", 1)); f.submit(1);
        std::mutex mutex; std::condition_variable cv;
        bool entered = false, release = false;
        f.dispatcher.action = [&](const Task&) {
            if (f.dispatcher.calls == 1) {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true; cv.notify_all();
                cv.wait(lock, [&] { return release; });
                if (throwing) throw std::runtime_error("injected dispatch failure");
                return false;
            }
            return true;
        };
        std::exception_ptr error;
        std::thread active([&] {
            try { f.driver.requestSchedule(); } catch (...) { error = std::current_exception(); }
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return entered; });
        }
        // These requests must return while the first dispatch remains blocked.
        std::vector<std::thread> callers;
        for (int n = 0; n < 12; ++n)
            callers.emplace_back([&] { f.driver.requestSchedule(); });
        for (auto& t : callers) t.join();
        {
            std::lock_guard<std::mutex> lock(mutex); release = true;
        }
        cv.notify_all(); active.join();
        if (error) std::rethrow_exception(error);
        CHECK(f.dispatcher.calls == 2); // Pending request survives a failed pass.
        CHECK(f.scheduler.getTask(1)->getStatus() == TaskStatus::ASSIGNED);
        CHECK(f.workers.getWorker("w")->getUsedSlots() == 1);
    }
}); }
