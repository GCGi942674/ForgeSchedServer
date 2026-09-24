#include "TestSupport.h"
#include "scheduler/SchedulingCoordinator.h"
#include "task/TaskService.h"
#include <limits>
#include <algorithm>
#include <set>
using namespace ForgeSched;
struct Dispatcher : WorkerDispatcher {
    int mode{0};
    bool dispatch(const WorkerId&, const Task&) override {
        if (mode == 2) throw std::runtime_error("injected dispatch failure");
        return mode == 1;
    }
};
int main() { return testMain([] {
    // Exhaustive legal/illegal transitions, including absorbing terminal states.
    const TaskStatus states[] = {TaskStatus::PENDING, TaskStatus::QUEUED, TaskStatus::ASSIGNED,
        TaskStatus::RUNNING, TaskStatus::SUCCEEDED, TaskStatus::FAILED, TaskStatus::TIMEOUT, TaskStatus::CANCELLED};
    const bool legal[8][8] = {{0,1,0,0,0,0,0,1}, {0,0,1,0,0,0,0,1},
        {0,1,0,1,0,0,0,1}, {0,0,0,0,1,1,1,1}, {}, {}, {}, {}};
    for (int from=0; from<8; ++from) for (int to=0; to<8; ++to) {
        Task t(1, TaskType::REGRESSION, "target", "rev");
        if (from > 0) CHECK(t.transitionTo(TaskStatus::QUEUED));
        if (from > 1) CHECK(t.transitionTo(TaskStatus::ASSIGNED));
        if (from > 2) CHECK(t.transitionTo(TaskStatus::RUNNING));
        if (from > 3) CHECK(t.transitionTo(states[from]));
        CHECK(t.transitionTo(states[to]) == legal[from][to]);
    }
    WorkerManager workers; Scheduler scheduler(workers); TaskService service(scheduler);
    CHECK(!workers.registerWorker("", "host", 1));
    CHECK(!workers.registerWorker("w", "host", 0));
    CHECK(workers.registerWorker("w", "host", 1));
    CHECK(!service.createTask({TaskType::UNKNOWN, "x", "r", 0}));
    CHECK(!service.createTask({TaskType::REGRESSION, "", "r", 0}));
    CHECK(!service.createTask({TaskType::REGRESSION, "x", "r", 101}));
    auto low = service.createTask({TaskType::REGRESSION, "x", "r", -1}); CHECK(low);
    auto high = service.createTask({TaskType::REGRESSION, "x", "r", 10}); CHECK(high);
    auto equal = service.createTask({TaskType::REGRESSION, "x", "r", 10}); CHECK(equal);
    CHECK(*low != *high && *high != *equal);
    CHECK(workers.getWorker("w")->getUsedSlots() == 0);
    Dispatcher dispatcher; SchedulingCoordinator coordinator(scheduler, dispatcher);
    for (int mode : {0, 2}) {
        dispatcher.mode = mode; auto result = coordinator.runOnce();
        CHECK(result.dispatch_failed_count == 1 && result.rollback_failed_count == 0);
        CHECK(workers.getWorker("w")->getUsedSlots() == 0);
        CHECK(scheduler.getTasksByStatus(TaskStatus::ASSIGNED).empty());
    }
    dispatcher.mode = 1;
    CHECK(coordinator.runOnce().dispatched_count == 1);
    auto assigned = scheduler.getTasksByStatus(TaskStatus::ASSIGNED); CHECK(assigned.size() == 1);
    auto id = assigned.front().getId(); CHECK(id != *low);
    CHECK(!scheduler.markTaskStarted(id, "wrong"));
    CHECK(!scheduler.completeTask(id, TaskStatus::SUCCEEDED));
    CHECK(workers.getWorker("w")->getUsedSlots() == 1);
    CHECK(workers.markOffline("w")); CHECK(scheduler.schedule().empty());
    CHECK(workers.registerWorker("w", "new-host", 1));
    CHECK(workers.getWorker("w")->getUsedSlots() == 1);
    CHECK(workers.getWorker("w")->getHostname() == "new-host");
    CHECK(scheduler.markTaskStarted(id, "w")); CHECK(!scheduler.markTaskStarted(id, "w"));
    CHECK(!scheduler.rollbackAssignment(id));
    CHECK(scheduler.completeTask(id, TaskStatus::SUCCEEDED));
    CHECK(!scheduler.completeTask(id, TaskStatus::SUCCEEDED));
    CHECK(!service.cancelTask(id)); CHECK(workers.getWorker("w")->getUsedSlots() == 0);
    CHECK(!workers.releaseSlot("w"));
    for (auto t : scheduler.getTasksByStatus(TaskStatus::QUEUED)) CHECK(service.cancelTask(t.getId()));
    CHECK(scheduler.schedule().empty());
    // Priority/FIFO independently of rollback (which requeues at the end).
    for (int priority : {0, 20, 20}) CHECK(service.createTask({TaskType::REGRESSION, "x", "r", priority}));
    auto queued = scheduler.getTasksByStatus(TaskStatus::QUEUED);
    std::vector<TaskId> top;
    for (const auto& t : queued) if (t.getPriority() == 20) top.push_back(t.getId());
    std::sort(top.begin(), top.end());
    for (auto expected : top) {
        auto a = scheduler.schedule(); CHECK(a.size() == 1 && a[0].task_id == expected);
        CHECK(service.cancelTask(expected)); CHECK(workers.getWorker("w")->getUsedSlots() == 0);
    }
    auto a = scheduler.schedule(); CHECK(a.size() == 1);
    CHECK(scheduler.markTaskStarted(a[0].task_id, "w"));
    CHECK(service.cancelTask(a[0].task_id)); CHECK(workers.getWorker("w")->getUsedSlots() == 0);
    CHECK(workers.registerWorker("big", "host", std::numeric_limits<uint32_t>::max()));
    CHECK(service.createTask({TaskType::REGRESSION, "x", "r", 0}));
    a = scheduler.schedule(); CHECK(a.size() == 1 && a[0].worker_id == "big");
    CHECK(service.cancelTask(a[0].task_id));
    for (auto final : {TaskStatus::FAILED, TaskStatus::TIMEOUT}) {
        auto next = service.createTask({TaskType::REGRESSION, "x", "r", 0}); CHECK(next);
        auto batch = scheduler.schedule(); CHECK(batch.size() == 1);
        CHECK(scheduler.markTaskStarted(*next, batch[0].worker_id));
        CHECK(scheduler.completeTask(*next, final));
        CHECK(!scheduler.completeTask(*next, final));
        CHECK(workers.getWorker(batch[0].worker_id)->getUsedSlots() == 0);
    }
    TaskIdGenerator generator;
    std::vector<std::vector<TaskId>> generated(4);
    std::vector<std::thread> threads;
    for (size_t i=0; i<generated.size(); ++i) threads.emplace_back([&, i] {
        for (int n=0; n<1000; ++n) generated[i].push_back(generator.next());
    });
    for (auto& thread : threads) thread.join();
    std::set<TaskId> unique;
    for (const auto& batch : generated) unique.insert(batch.begin(), batch.end());
    CHECK(unique.size() == 4000 && *unique.begin() == 1 && *unique.rbegin() == 4000);
}); }
