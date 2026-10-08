#include "TestSupport.h"
#include "scheduler/SchedulingCoordinator.h"
#include <new>
#include <cstdlib>
using namespace ForgeSched;
// Fail all subsequent allocations on this thread until explicitly disarmed.
// Logger background threads and fixture setup are not affected.
thread_local long allocations_left = -1;
void* operator new(std::size_t size) {
    if (allocations_left == 0) throw std::bad_alloc();
    if (allocations_left > 0) --allocations_left;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
struct Dispatcher : WorkerDispatcher {
    int sends[4]{};
    bool dispatch(const WorkerId&, const Task& t) override { ++sends[t.getId()]; return true; }
};
struct Fixture {
    WorkerManager workers;
    Scheduler scheduler{workers};
    Dispatcher dispatcher;
    SchedulingCoordinator coordinator{scheduler, dispatcher};
    Fixture() {
        CHECK(workers.registerWorker(std::string(80, 'w'), "host", 3));
        for (TaskId id = 1; id <= 3; ++id)
            CHECK(scheduler.submitTask(Task(id, TaskType::REGRESSION, std::string(128, 'x'), "revision")));
    }
};
int main() { return testMain([] {
    int failures = 0;
    for (long n = 0; n < 100; ++n) {
        Fixture f;
        bool threw = false;
        allocations_left = n;
        try { f.scheduler.schedule(); } catch (const std::bad_alloc&) { threw = true; }
        allocations_left = -1;
        if (threw) {
            ++failures;
            CHECK(f.scheduler.getTasksByStatus(TaskStatus::QUEUED).size() == 3);
            CHECK(f.workers.getWorker(std::string(80, 'w'))->getUsedSlots() == 0);
            CHECK(f.scheduler.schedule().size() == 3);
        } else {
            CHECK(f.scheduler.getTasksByStatus(TaskStatus::ASSIGNED).size() == 3);
        }
    }
    CHECK(failures > 10);
    for (long n = 0; n < 140; ++n) {
        Fixture f;
        allocations_left = n;
        try { f.coordinator.runOnce(); } catch (const std::bad_alloc&) {}
        allocations_left = -1;
        size_t accepted = 0;
        for (TaskId id = 1; id <= 3; ++id) {
            CHECK(f.dispatcher.sends[id] <= 1);
            auto task = f.scheduler.getTask(id);
            CHECK(task->getStatus() == (f.dispatcher.sends[id] ? TaskStatus::ASSIGNED : TaskStatus::QUEUED));
            accepted += f.dispatcher.sends[id];
        }
        CHECK(f.workers.getWorker(std::string(80, 'w'))->getUsedSlots() == accepted);
        f.coordinator.runOnce();
        for (TaskId id = 1; id <= 3; ++id) CHECK(f.dispatcher.sends[id] == 1);
        CHECK(f.workers.getWorker(std::string(80, 'w'))->getUsedSlots() == 3);
    }
    // Rollback itself must work even with a completely unavailable allocator.
    Fixture f;
    auto batch = f.scheduler.schedule();
    allocations_left = 0;
    bool rolled_back = f.scheduler.rollbackAssignment(batch.front().task_id);
    allocations_left = -1;
    CHECK(rolled_back);
    CHECK(f.scheduler.schedule().size() == 1);
}); }
