#include "regression/RegressionService.h"
#include "task/RegressionPayload.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <set>

using namespace GCRP;

int main() {
    nlohmann::json tagged={{"spec_version",2},{"case","cases/a/run.tcl"},{"flow","route"},
        {"timeout_seconds",uint64_t(60)},{"flow_config",{{"route_design",1}}},
        {"context",{{"source","scheduled"},{"date","2026-10-12"},{"template","route"},
            {"suite","daily"},{"name","route"},{"batch_id","batch"}}}};
    assert(validRegressionPayload(tagged));tagged["context"]["extra"]="no";
    assert(!validRegressionPayload(tagged));
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
    assert(!RegressionQuery::parse({{"date","2026-02-30"}}));
    assert(!RegressionQuery::parse({{"date","2026-10-12"},{"baseline_date","bad"}}));
    auto dailyAdd=[&](TaskId id,const std::string& day,const std::string& stage,
                      const std::string& path,const std::string& source,const std::string& outcome){
        Task task(id,TaskType::REGRESSION,"device","999");
        task.setPayload({{"case",path},{"flow",stage},{"context",{{"date",day},{"source",source},{"template",stage}}}});
        assert(scheduler.submitTask(std::move(task)));
        complete(id,outcome=="PASS"?TaskStatus::SUCCEEDED:outcome=="TIMEOUT"?TaskStatus::TIMEOUT:TaskStatus::FAILED,{{"case_status",outcome}});
    };
    dailyAdd(20,"2026-10-11","route",case_path,"scheduled","PASS");
    dailyAdd(21,"2026-10-12","route",case_path,"scheduled","FAIL");
    dailyAdd(22,"2026-10-11","place",case_path,"scheduled","FAIL");
    dailyAdd(23,"2026-10-12","place",case_path,"scheduled","PASS");
    dailyAdd(24,"2026-10-12","route",case_path,"manual","PASS");
    dailyAdd(25,"2026-10-12","route","cases/missing/run.tcl","scheduled","PASS");
    dailyAdd(26,"2026-10-12","route","cases/timeout/run.tcl","scheduled","TIMEOUT");
    dailyAdd(27,"2026-10-11","route","cases/timeout/run.tcl","scheduled","PASS");
    const auto dailyFilter=RegressionQuery::parse({{"date","2026-10-12"},{"baseline_date","2026-10-11"},{"limit",2}});
    assert(dailyFilter);auto daily=service.query(*dailyFilter);
    assert(daily["total"]==4&&daily["items"].size()==2&&daily["has_more"]==true);
    assert(daily["counts"]["REGRESSION"]==1&&daily["counts"]["IMPROVEMENT"]==1);
    assert(daily["counts"]["MISSING"]==1&&daily["counts"]["EXCEPTION"]==1);
    assert(daily["stages"]["place"]["changes"]["REGRESSION"]==0);
    auto pageFilter=*dailyFilter;pageFilter.after_key=daily["next_after_key"].get<std::string>();
    auto page=service.query(pageFilter);assert(page["items"].size()==2&&page["has_more"]==false);
    assert(page["counts"]==daily["counts"]);
    auto stageFilter=*dailyFilter;stageFilter.template_name="route";stageFilter.change="REGRESSION";
    auto stage=service.query(stageFilter);assert(stage["total"]==1&&stage["items"][0]["comparison"]["task_id"]==21);
    dailyAdd(28,"2026-10-12","route",case_path,"scheduled","PASS");
    assert(service.query(*dailyFilter)["duplicate_records"]==1);
    assert(service.query(stageFilter)["total"]==0);
    // Production-sized inventory: pages do not truncate the 4,000-case set.
    for(TaskId id=100;id<4100;++id){Task task(id,TaskType::REGRESSION,"device","999");
        task.setPayload({{"case","bulk/"+std::to_string(id)+"/run.tcl"},{"flow","route"},
            {"context",{{"source","scheduled"},{"date","2026-10-12"},{"template","route"}}}});
        assert(scheduler.submitTask(std::move(task)));}
    auto bulk=*dailyFilter;bulk.search="bulk/";bulk.limit=50;std::set<std::string> seen;
    do{auto result=service.query(bulk);assert(result["total"]==4000);
        for(const auto& row:result["items"])assert(seen.insert(row["key"].get<std::string>()).second);
        if(!result["has_more"].get<bool>())break;bulk.after_key=result["next_after_key"].get<std::string>();
    }while(true);assert(seen.size()==4000);
}
