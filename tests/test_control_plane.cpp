#include "ControlTestSupport.h"
#include "ForgeClient.h"
#include "protocol/ConnectionRole.h"
#include "task/TaskLimits.h"
#include <set>
using namespace ForgeSched;
using namespace ForgeSched::Protocol;
int main() { return testMain([] {
    const MessageType types[] = {MessageType::SUBMIT_TASK, MessageType::QUERY_TASK,
        MessageType::CANCEL_TASK, MessageType::WORKER_REGISTER, MessageType::WORKER_HEARTBEAT,
        MessageType::TASK_START, MessageType::TASK_RESULT, MessageType::TASK_ASSIGN,
        MessageType::RESPONSE, MessageType::UNKNOWN};
    for (size_t i = 0; i < 10; ++i) {
        CHECK(isAllowed(ConnectionRole::CLIENT, types[i]) == (i < 3));
        CHECK(isAllowed(ConnectionRole::WORKER, types[i]) == (i >= 3 && i <= 6));
        CHECK(isAllowed(ConnectionRole::UNKNOWN, types[i]) == (i <= 3));
    }
    ControlTestServer host;
    ForgeClient api("127.0.0.1", host.port);
    auto client = connectTo(host.port);
    auto malformed = MessageCodec::encode("{");
    sendBytes(client.value, malformed.data(), malformed.size());
    CHECK(receiveJson(client.value)["data"]["code"] == 1);
    response(client.value, envelope("worker_heartbeat", {{"worker_id", "w"}}), false);
    response(client.value, envelope("worker_register", {{"worker_id", "w"}, {"hostname", "h"}, {"slots", 0u}}), false);
    // Unknown type does not establish a role.
    sendJson(client.value, envelope("query_regression", nlohmann::json::object()));
    CHECK(receiveJson(client.value)["data"]["code"] == 1);
    auto submit = envelope("submit_task",
        {{"task_type", "REGRESSION"}, {"target", "network-case"}, {"revision", "r1"}, {"priority", 10}}, 11);
    auto frame = MessageCodec::encode(submit.dump());
    sendBytes(client.value, frame.data(), 2);
    sendBytes(client.value, frame.data()+2, frame.size()-2);
    auto reply = receiveJson(client.value);
    CHECK(reply["request_id"] == 11 && reply["data"]["code"] == 0);
    const auto id = reply["data"]["result"]["task_id"].get<uint64_t>();
    CHECK(api.query(id).data["result"]["status"] == "QUEUED");
    for (auto type : {"worker_register", "worker_heartbeat", "task_start", "task_result", "task_assign", "response"}) {
        auto bad = envelope(type, {{"worker_id", "phantom"}, {"hostname", "h"}, {"slots", 1u},
            {"task_id", id}, {"status", "SUCCEEDED"}});
        CHECK(exchangeJson(client.value, bad)["code"] == 1);
    }
    CHECK(host.worker.getWorkerManager().getWorkers().empty());
    auto worker = connectTo(host.port);
    // A malformed envelope must not prevent a later valid worker registration.
    sendBytes(worker.value, malformed.data(), malformed.size());
    CHECK(receiveJson(worker.value)["data"]["code"] == 1);
    auto registration = envelope("worker_register", {{"worker_id", "w"}, {"hostname", "h"}, {"slots", 1u}});
    response(worker.value, registration, true);
    auto assignment = receiveJson(worker.value);
    CHECK(assignment["type"] == "task_assign" && assignment["data"]["task_id"] == id);
    CHECK(assignment["data"]["target"] == "network-case");
    CHECK(api.cancel(id).data["code"] == 3);
    CHECK(api.query(id).data["result"]["status"] == "ASSIGNED");
    CHECK(host.worker.getWorkerManager().getWorker("w")->getUsedSlots() == 1);
    for (auto type : {"submit_task", "query_task", "cancel_task", "task_assign", "response"}) {
        auto bad = envelope(type, {{"task_id", id}, {"task_type", "REGRESSION"}, {"target", "x"}, {"revision", "r"}});
        CHECK(exchangeJson(worker.value, bad)["code"] == 1);
    }
    response(worker.value, registration, true); // Idempotent registration retains WORKER.
    response(worker.value, envelope("task_start", {{"task_id", id}, {"worker_id", "w"}}), true);
    CHECK(api.query(id).data["result"]["status"] == "RUNNING");
    CHECK(api.cancel(id).data["code"] == 3);
    CHECK(api.query(id).data["result"]["status"] == "RUNNING");
    CHECK(host.worker.getWorkerManager().getWorker("w")->getUsedSlots() == 1);
    auto finish = envelope("task_result", {{"task_id", id}, {"worker_id", "w"},
        {"status", "SUCCEEDED"}, {"message", R"({"status":"SUCCEEDED","reason":"pass","exit_code":0,"pid":123,"secret":"do-not-expose"})"}});
    response(worker.value, finish, true);
    CHECK(api.query(id).data["result"]["status"] == "SUCCEEDED");
    CHECK(api.query(id).data["result"]["execution"]["reason"] == "pass");
    CHECK(api.query(id).data["result"]["execution"]["exit_code"] == 0);
    CHECK(!api.query(id).data["result"]["execution"].contains("secret"));
    // Multiple requests in one write preserve framing and request correlation.
    auto first = MessageCodec::encode(envelope("query_task", {{"task_id", id}}, 101).dump());
    auto second = MessageCodec::encode(envelope("query_task", {{"task_id", id}}, 102).dump());
    first.insert(first.end(), second.begin(), second.end());
    sendBytes(client.value, first.data(), first.size());
    for (uint64_t expected : {101u, 102u}) {
        auto query = receiveJson(client.value);
        CHECK(query["request_id"] == expected);
        CHECK(query["data"]["result"]["status"] == "SUCCEEDED");
    }
    response(worker.value, finish, true);
    auto conflicting = finish;
    conflicting["data"]["message"] = R"({"status":"SUCCEEDED","reason":"different"})";
    response(worker.value, conflicting, false);
    CHECK(api.query(id).data["result"]["execution"]["reason"] == "pass");
    CHECK(api.cancel(id).data["code"] == 3);
    CHECK(api.query(999999).data["code"] == 2);
    CHECK(api.cancel(999999).data["code"] == 2);
    // Submit while a worker is online, then cancel only the queued task.
    const auto active = api.submit("active", "r").data["result"]["task_id"].get<uint64_t>();
    CHECK(receiveJson(worker.value)["data"]["task_id"] == active);
    const auto queued = api.submit("queued", "r").data["result"]["task_id"].get<uint64_t>();
    CHECK(api.query(queued).data["result"]["status"] == "QUEUED");
    CHECK(api.cancel(queued).data["code"] == 0);
    CHECK(api.query(queued).data["result"]["status"] == "CANCELLED");
    CHECK(api.cancel(queued).data["code"] == 3);
    response(worker.value, envelope("task_start", {{"task_id", active}, {"worker_id", "w"}}), true);
    response(worker.value, envelope("task_result", {{"task_id", active}, {"worker_id", "w"},
        {"status", "FAILED"}, {"message", std::string(9000, 'x')}}), true);
    CHECK(api.query(active).data["result"]["status"] == "FAILED");
    CHECK(!api.query(active).data["result"].contains("execution"));
    CHECK(host.worker.getWorkerManager().getWorker("w")->getUsedSlots() == 0);
    worker.reset();
    eventually([&] { return !host.worker.getWorkerConnectionRegistry().hasConnection("w"); });
    // Size rejection precedes task creation; accepted metadata survives worst-case escaping.
    const auto count = host.worker.getScheduler().getTasks().size();
    CHECK(api.submit(std::string(kMaxTaskMetadataBytes, 'x'), "r").data["code"] == 1);
    CHECK(host.worker.getScheduler().getTasks().size() == count);
    const std::string target(kMaxTaskMetadataBytes-1, '\x01');
    auto big = api.submit(target, "r");
    CHECK(big.data["code"] == 0);
    const auto big_id = big.data["result"]["task_id"].get<uint64_t>();
    CHECK(api.query(big_id).data["result"]["target"] == target);
    CHECK(api.cancel(big_id).data["code"] == 0);
    auto oversized_worker = connectTo(host.port);
    response(oversized_worker.value, envelope("worker_register",
        {{"worker_id", std::string(kMaxWorkerIdBytes+1, 'w')}, {"hostname", "h"}, {"slots", 1u}}), false);
    CHECK(exchangeJson(oversized_worker.value, envelope("query_task", {{"task_id", id}}))["code"] == 0);
    // Concurrent external submissions use unique IDs; no fixture creates Tasks internally.
    std::vector<uint64_t> ids(12);
    std::vector<std::exception_ptr> errors(12);
    std::vector<std::thread> callers;
    for (size_t i = 0; i < ids.size(); ++i) callers.emplace_back([&, i] {
        try {
            ForgeClient c("127.0.0.1", host.port);
            auto result = c.submit("parallel", "r");
            CHECK(result.data["code"] == 0);
            ids[i] = result.data["result"]["task_id"].get<uint64_t>();
        } catch (...) { errors[i] = std::current_exception(); }
    });
    for (auto& t : callers) t.join();
    for (auto error : errors) if (error) std::rethrow_exception(error);
    CHECK(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());
    host.stop(); CHECK(host.result);
}); }
