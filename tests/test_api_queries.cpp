#include "TestSupport.h"
#include "protocol/ApiQueries.h"
#include "protocol/ProtocolCodec.h"
#include "protocol/ConnectionRole.h"
#include "protocol/ProtocolRouter.h"
#include "scheduler/SchedulingCoordinator.h"
#include "scheduler/SchedulingDriver.h"
#include "worker/WorkerDispatcher.h"
using namespace GCRP;
using namespace GCRP::Protocol;
struct Dispatcher : WorkerDispatcher {
    bool dispatch(const WorkerId&, const Task&) override { return false; }
};
int main() { return testMain([] {
    WorkerManager workers; Scheduler scheduler(workers); Dispatcher dispatcher;
    SchedulingCoordinator coordinator(scheduler, dispatcher); SchedulingDriver driver(coordinator);
    TaskService service(scheduler, driver); ProtocolRouter router(service, scheduler, workers, driver);
    auto route = [&](const std::string& type, nlohmann::json data) {
        ProtocolMessage request; ProtocolError error;
        CHECK(ProtocolCodec::decode(envelope(type, data).dump(), request, error));
        CHECK(isAllowed(ConnectionRole::CLIENT, request.type));
        CHECK(!isAllowed(ConnectionRole::WORKER, request.type));
        auto response = router.handle(request);
        CHECK(response.request_id == request.request_id);
        return response.data;
    };
    const auto empty = nlohmann::json::object();
    CHECK(route("query_capabilities", empty)["result"]["features"]["remote_cancel"] == false);
    CHECK(route("query_tasks", empty)["result"]["items"].empty());
    for (int i = 0; i < 3; ++i)
        CHECK(route("submit_task", {{"task_type", "REGRESSION"}, {"target", "device"}, {"revision", "123"}})["code"] == 0);
    auto page = route("query_tasks", {{"limit", 1}})["result"];
    CHECK(page["total"] == 3); CHECK(page["has_more"] == true);
    auto next = route("query_tasks", {{"before_task_id", page["next_before_task_id"]}, {"limit", 1}})["result"];
    CHECK(next["items"][0]["task_id"] < page["items"][0]["task_id"]);
    CHECK(next["snapshot_generation"] == page["snapshot_generation"]);
    const auto old_generation = page["snapshot_generation"];
    CHECK(route("cancel_task", {{"task_id", page["items"][0]["task_id"]}})["code"] == 0);
    CHECK(route("query_tasks", empty)["result"]["snapshot_generation"] > old_generation);
    CHECK(route("query_task", {{"task_id", 1u}, {"unexpected", true}})["code"] == 1);
    CHECK(route("query_tasks", empty)["server_instance_id"] == route("query_capabilities", empty)["server_instance_id"]);
    CHECK(route("query_tasks", {{"target", "missing"}})["result"]["total"] == 0);
    for (auto bad : {nlohmann::json{{"limit", 0}}, nlohmann::json{{"limit", 51}},
            nlohmann::json{{"limit", true}}, nlohmann::json{{"before_task_id", -1}},
            nlohmann::json{{"unexpected", 1}}, nlohmann::json{{"status", "invalid"}}}) {
        auto r = route("query_tasks", bad);
        CHECK(r["code"] == 1); CHECK(r["error_reason"] == "INVALID_REQUEST");
    }
    CHECK(workers.registerWorker("host-slot1", "host", 1, "host", "slot1"));
    CHECK(workers.registerWorker("host-slot2", "host", 1, "host", "slot2"));
    CHECK(workers.markOffline("host-slot1")); CHECK(workers.markDraining("host-slot2"));
    page = route("query_workers", {{"limit", 1}})["result"];
    CHECK(page["total"] == 2); CHECK(page["items"][0]["status"] == "OFFLINE");
    next = route("query_workers", {{"after", page["next_after"]}})["result"];
    CHECK(next["items"].size() == 1); CHECK(next["items"][0]["status"] == "DRAINING");
    CHECK(next["next_after"].is_null());
    CHECK(workers.heartbeat("host-slot2"));
    CHECK(route("query_workers", empty)["result"]["snapshot_generation"] > next["snapshot_generation"]);
    CHECK(route("query_workers", {{"machine_id", "other"}})["result"]["total"] == 0);
    CHECK(route("query_workers", {{"after", {{"slot_id", "slot1"}}}})["code"] == 1);
    CHECK(route("query_capabilities", {{"extra", true}})["code"] == 1);
}); }
