#include "regression/RegressionService.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>

using namespace ForgeSched;

int main() {
    WorkerManager manager;
    Scheduler scheduler(manager);
    assert(manager.registerWorker("worker-1", "host", 1));
    const std::string case_path = "cases/one/run.tcl";
    auto add = [&](TaskId id, const std::string& revision, const std::string& batch) {
        Task task(id, TaskType::REGRESSION, "device", revision);
        task.setPayload({{"spec_version", 2}, {"case", case_path}, {"flow", "route"},
            {"timeout_seconds", 600}, {"flow_config", {{"route_design", 1}}},
            {"context", {{"batch_id", batch}}}});
        assert(scheduler.submitTask(std::move(task)));
    };
    auto complete = [&](TaskId id, TaskStatus status, nlohmann::json execution) {
        auto assigned = scheduler.schedule();
        assert(assigned.size() == 1 && assigned[0].task_id == id);
        assert(scheduler.markTaskStarted(id, assigned[0].worker_id));
        assert(scheduler.completeTask(id, status, std::move(execution)));
    };
    add(1, "18925", "daily-1");
    complete(1, TaskStatus::SUCCEEDED, {{"case_status", "PASS"}, {"reason", "pass"}});
    add(2, "18925", "daily-1");
    complete(2, TaskStatus::FAILED, {{"reason", "missing_result"}});
    add(3, "18925", "daily-1");
    complete(3, TaskStatus::FAILED, {{"case_status", "FAIL"}, {"reason", "business_failed"}});
    add(4, "18926", "daily-2");
    complete(4, TaskStatus::TIMEOUT, {{"case_status", "TIMEOUT"}});
    add(5, "18926", "daily-2");
    complete(5, TaskStatus::FAILED, {{"case_status", "CRASH"}});
    add(6, "18926", "daily-2");
    assert(scheduler.cancelTask(6));
    add(7, "18927", "daily-3");

    assert(!RegressionQuery::parse(nlohmann::json::object()));
    assert(!RegressionQuery::parse({{"flow", "route"}}));
    assert(!RegressionQuery::parse({{"revision", "18925"}, {"limit", 51}}));
    assert(!RegressionQuery::parse({{"case", case_path}, {"extra", 1}}));
    assert(!RegressionQuery::parse({{"case", case_path}, {"before_task_id", 0}}));
    RegressionService service(scheduler);
    const auto filter = RegressionQuery::parse({{"case", case_path}, {"limit", 2}});
    assert(filter);
    const auto first = service.query(*filter);
    assert(first["total"] == 7 && first["has_more"] == true);
    assert(first["counts"]["PASS"] == 1 && first["counts"]["FAIL"] == 1);
    assert(first["counts"]["ERROR"] == 1 && first["counts"]["CRASH"] == 1);
    assert(first["counts"]["TIMEOUT"] == 1 && first["counts"]["CANCELLED"] == 1);
    assert(first["counts"]["ACTIVE"] == 1);
    assert(first["items"].size() == 2 && first["items"][0]["task_id"] == 7);
    auto next = *filter;
    next.before_task_id = first["next_before_task_id"].get<TaskId>();
    const auto second = service.query(next);
    assert(second["items"][0]["task_id"] == 5);
    assert(second["total"] == 7 && second["counts"] == first["counts"]);
    auto revision = RegressionQuery::parse({{"revision", "18925"}, {"batch_id", "daily-1"}});
    assert(revision);
    const auto selected = service.query(*revision);
    assert(selected["total"] == 3 && selected["has_more"] == false);
    assert(selected["items"][0]["outcome"] == "FAIL");

    RegressionService cached(scheduler, true, 2, 60);
    const auto before = cached.query(*filter);
    assert(cached.query(*filter) == before);
    assert(cached.cacheStats()["hits"] == 1);
    add(8, "18927", "daily-3");
    const auto after = cached.query(*filter);
    assert(after["total"] == 8 && after["items"][0]["task_id"] == 8);
    assert(cached.cacheStats()["invalidations"] == 1);
    assert(scheduler.cancelTask(8));
    assert(cached.query(*filter)["counts"]["CANCELLED"] == 2);
    cached.query(*filter);
    complete(7, TaskStatus::SUCCEEDED, {{"case_status", "PASS"}});
    const auto terminal = cached.query(*filter);
    assert(terminal["counts"]["PASS"] == 2);
}
