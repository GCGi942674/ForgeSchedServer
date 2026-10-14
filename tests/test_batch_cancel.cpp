#include "ControlTestSupport.h"
#include "GcrpClient.h"
using namespace GCRP;

int main() { return testMain([] {
    ControlTestServer host;
    auto ready = connectTo(host.port);
    ready.reset();
    GcrpClient client("127.0.0.1", host.port);
    const std::string batch = "daily-18925-test";
    auto submit = [&](const std::string& group, const std::string& name) {
        return client.request(Protocol::MessageType::SUBMIT_TASK,
            {{"task_type", "REGRESSION"}, {"target", name}, {"revision", "18925"},
             {"payload", {{"spec_version", 2}, {"case", "cases/one/run.tcl"},
                 {"flow", "route"}, {"timeout_seconds", 600},
                 {"flow_config", {{"route_design", 1}}},
                 {"context", {{"batch_id", group}}}}}});
    };
    auto first = submit(batch, "first");
    CHECK(first.data["code"] == 0);
    const auto first_id = first.data["result"]["task_id"].get<uint64_t>();
    auto worker = connectTo(host.port);
    response(worker.value, envelope("worker_register",
        {{"worker_id", "slot-1"}, {"hostname", "h"}, {"slots", 1u}}), true);
    auto assignment = receiveJson(worker.value);
    CHECK(assignment["type"] == "task_assign");
    CHECK(assignment["data"]["task_id"] == first_id);
    response(worker.value, envelope("task_start",
        {{"task_id", first_id}, {"worker_id", "slot-1"}}), true);
    CHECK(exchangeJson(worker.value, envelope("cancel_batch", {{"batch_id", batch}}))["code"] == 1);
    CHECK(client.cancelBatch("").data["code"] == 1);
    auto second = submit(batch, "second");
    auto third = submit(batch, "third");
    auto unrelated = submit("other-batch", "unrelated");
    CHECK(second.data["code"] == 0 && third.data["code"] == 0 &&
          unrelated.data["code"] == 0);
    const auto second_id = second.data["result"]["task_id"].get<uint64_t>();
    const auto third_id = third.data["result"]["task_id"].get<uint64_t>();
    const auto unrelated_id = unrelated.data["result"]["task_id"].get<uint64_t>();
    auto cancelled = client.cancelBatch(batch);
    CHECK(cancelled.data["code"] == 0);
    CHECK(cancelled.data["result"]["cancelled_now"] == 2);
    CHECK(cancelled.data["result"]["in_flight"] == 1);
    CHECK(cancelled.data["result"]["complete"] == false);
    CHECK(client.query(second_id).data["result"]["status"] == "CANCELLED");
    CHECK(client.query(third_id).data["result"]["status"] == "CANCELLED");
    CHECK(client.query(unrelated_id).data["result"]["status"] == "QUEUED");
    CHECK(client.query(first_id).data["result"]["status"] == "RUNNING");
    CHECK(host.worker.getWorkerManager().getWorker("slot-1")->getUsedSlots() == 1);
    CHECK(client.cancelBatch(batch).data["result"]["cancelled_now"] == 0);
    CHECK(submit(batch, "late").data["code"] == 3);
    CHECK(client.queryBatch("unknown").data["code"] == 2);
    auto finish_first = envelope("task_result", {{"task_id", first_id},
        {"worker_id", "slot-1"}, {"status", "SUCCEEDED"},
        {"message", R"({"status":"SUCCEEDED","reason":"pass"})"}});
    sendJson(worker.value, finish_first);
    bool got_reply = false, got_assignment = false;
    for (int i = 0; i < 2; ++i) {
        auto message = receiveJson(worker.value);
        if (message["type"] == "response") {
            CHECK(message["request_id"] == finish_first["request_id"]);
            CHECK(message["data"]["code"] == 0);
            got_reply = true;
        } else {
            CHECK(message["type"] == "task_assign");
            CHECK(message["data"]["task_id"] == unrelated_id);
            got_assignment = true;
        }
    }
    CHECK(got_reply && got_assignment);
    auto final = client.queryBatch(batch);
    CHECK(final.data["code"] == 0);
    CHECK(final.data["result"]["complete"] == true);
    CHECK(final.data["result"]["succeeded"] == 1);
    CHECK(final.data["result"]["cancelled"] == 2);
    CHECK(host.worker.getWorkerManager().getWorker("slot-1")->getUsedSlots() == 1);
    response(worker.value, envelope("task_start", {{"task_id", unrelated_id},
        {"worker_id", "slot-1"}}), true);
    response(worker.value, envelope("task_result", {{"task_id", unrelated_id},
        {"worker_id", "slot-1"}, {"status", "SUCCEEDED"}}), true);
    CHECK(host.worker.getWorkerManager().getWorker("slot-1")->getUsedSlots() == 0);
    host.stop(); CHECK(host.result);
}); }
