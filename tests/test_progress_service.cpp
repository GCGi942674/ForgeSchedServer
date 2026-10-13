#include "progress/ProgressService.h"
#include "persistence/StateStore.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

using namespace ForgeSched;

int main() {
    WorkerManager workers;
    Scheduler scheduler(workers);
    assert(workers.registerWorker("slot1", "host", 1));
    TaskId next_id = 1;
    auto add = [&](const std::string& revision, const std::string& path) {
        const TaskId id = next_id++;
        Task task(id, TaskType::REGRESSION, "project-a", revision);
        task.setPayload({{"case", path}, {"flow", "route"}});
        assert(scheduler.submitTask(std::move(task)));
        return id;
    };
    auto finish = [&](TaskId id, TaskStatus status, const std::string& case_status) {
        const auto assignments = scheduler.schedule();
        assert(assignments.size() == 1 && assignments[0].task_id == id);
        assert(scheduler.markTaskStarted(id, "slot1"));
        assert(scheduler.completeTask(id, status, {{"case_status", case_status}}));
    };
    for (const auto& path : {"a/run.tcl", "d/run.tcl", "e/run.tcl"}) {
        const auto id = add("18925", path);
        finish(id, TaskStatus::SUCCEEDED, "PASS");
    }
    auto a = add("18926", "a/run.tcl");
    finish(a, TaskStatus::FAILED, "FAIL");
    auto d = add("18926", "d/run.tcl");
    finish(d, TaskStatus::FAILED, "FAIL");
    auto e = add("18926", "e/run.tcl");
    finish(e, TaskStatus::FAILED, "FAIL");
    e = add("18926", "e/run.tcl");
    finish(e, TaskStatus::SUCCEEDED, "PASS");
    auto b = add("18926", "b/run.tcl");
    finish(b, TaskStatus::TIMEOUT, "TIMEOUT");
    a = add("18927", "a/run.tcl");
    finish(a, TaskStatus::SUCCEEDED, "PASS");
    b = add("18927", "b/run.tcl");
    finish(b, TaskStatus::FAILED, "FAIL");
    add("18927", "d/run.tcl"); // A newer unfinished attempt is not a FAIL.
    Task other(next_id++, TaskType::REGRESSION, "project-b", "18927");
    other.setPayload({{"case", "a/run.tcl"}, {"flow", "route"}});
    assert(scheduler.submitTask(std::move(other)));

    assert(!ProgressQuery::parse({{"target", "project-a"}, {"limit", 21}}));
    assert(!ProgressQuery::parse({{"target", "project-a"}, {"before_revision", "latest"}}));
    assert(!RollbackQuery::parse({{"target", "project-a"},
        {"from_revision", "18927"}, {"to_revision", "18926"}}));
    ProgressService service(scheduler);
    const auto filter = *ProgressQuery::parse({{"target", "project-a"}, {"limit", 1}});
    const auto newest = service.queryProgress(filter);
    assert(newest["total_revisions"] == 3 && newest["has_more"] == true);
    assert(newest["revisions"][0]["revision"] == "18927");
    assert(newest["revisions"][0]["counts"]["ACTIVE"] == 1);
    assert(newest["revisions"][0]["rollback_count"] == 0);
    assert(newest["revisions"][0]["fixed_count"] == 1);
    auto previous = filter;
    previous.before_revision = newest["next_before_revision"].get<std::string>();
    const auto middle = service.queryProgress(previous);
    assert(middle["revisions"][0]["revision"] == "18926");
    assert(middle["revisions"][0]["rollback_count"] == 2);
    assert(middle["revisions"][0]["counts"]["PASS"] == 1);
    assert(middle["revisions"][0]["counts"]["TIMEOUT"] == 1);
    assert(middle["revisions"][0]["new_cases"] == 1);
    const auto rollback_filter = *RollbackQuery::parse({{"target", "project-a"},
        {"from_revision", "18925"}, {"to_revision", "18926"}, {"limit", 1}});
    const auto first = service.queryRollbacks(rollback_filter);
    assert(first["total"] == 2 && first["items"].size() == 1 && first["has_more"] == true);
    auto continuation = rollback_filter;
    continuation.after_task_id = first["next_after_task_id"].get<TaskId>();
    const auto second = service.queryRollbacks(continuation);
    assert(second["total"] == 2 && second["items"].size() == 1 && second["has_more"] == false);
    const auto later = *RollbackQuery::parse({{"target", "project-a"},
        {"from_revision", "18926"}, {"to_revision", "18927"}});
    assert(service.queryRollbacks(later)["total"] == 0);

    char directory[] = "/tmp/forge-progress-XXXXXX";
    assert(::mkdtemp(directory));
    struct Cleanup {
        const char* path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    StateStore store(std::string(directory) + "/state.db");
    WorkerManager durable_workers;
    Scheduler durable(durable_workers);
    durable.restoreFrom(store);
    assert(durable_workers.registerWorker("durable-slot", "host", 1));
    Task persisted(1, TaskType::REGRESSION, "project-persisted", "19000");
    persisted.setPayload({{"case", "persisted/run.tcl"}, {"flow", "route"}});
    assert(durable.submitTask(std::move(persisted)));
    const auto assigned = durable.schedule();
    assert(assigned.size() == 1);
    assert(durable.markTaskStarted(1, "durable-slot"));
    assert(durable.completeTask(1, TaskStatus::SUCCEEDED, {{"case_status", "PASS"}}));
    WorkerManager restored_workers;
    Scheduler restored(restored_workers);
    restored.restoreFrom(store);
    const auto restored_view = ProgressService(restored).queryProgress(
        *ProgressQuery::parse({{"target", "project-persisted"}}));
    assert(restored_view["revisions"][0]["counts"]["PASS"] == 1);
}
