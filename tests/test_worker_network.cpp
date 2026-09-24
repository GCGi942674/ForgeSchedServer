#include "TestSupport.h"
#include "WorkerServer.h"
#include <sys/eventfd.h>
using namespace ForgeSched;
struct RunningServer {
    Fd signal{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    WorkerServer worker; EchoHandler handler;
    int port{freePort()};
    EchoServer server{port, handler, signal.value, 2, 2};
    bool result{false}; std::thread thread;
    RunningServer() {
        CHECK(signal.value >= 0); worker.attach(server);
        thread = std::thread([this] { result = server.run(); });
    }
    void stop() {
        if (thread.joinable()) { uint64_t one = 1; ::write(signal.value, &one, sizeof(one)); thread.join(); }
    }
    ~RunningServer() { stop(); }
};
int main() { return testMain([] {
    // Registry owns weak references; stale handles must not unbind replacements.
    {
        WorkerConnectionRegistry registry;
        auto first = std::make_shared<Connection>(nullptr, -1);
        auto second = std::make_shared<Connection>(nullptr, -1);
        CHECK(!registry.bind("", first)); CHECK(!registry.bind("w", nullptr));
        auto old = registry.bindWithHandle("w", first); CHECK(old);
        auto current = registry.bindWithHandle("w", second); CHECK(current && *current != *old);
        bool called = false;
        CHECK(!registry.unbind("w", *old, [&] { called = true; })); CHECK(!called);
        CHECK(registry.getConnection("w") == second);
        second->setState(Connection::ConnState::Disconnected);
        NetworkWorkerDispatcher dispatcher(registry);
        Task t(1, TaskType::REGRESSION, "x", "r"); CHECK(!dispatcher.dispatch("w", t));
        second.reset(); CHECK(!registry.hasConnection("w")); CHECK(!registry.isCurrent("w", *current));
        CHECK(!dispatcher.dispatch("w", t));
        CHECK(registry.unbind("w", *current));
    }
    RunningServer host;
    // Create before starting network mutations; subsequent checks use locked snapshots.
    auto task = host.worker.getTaskService().createTask({TaskType::REGRESSION, "x", "r", 0}); CHECK(task);
    auto a = connectTo(host.port);
    auto heartbeat = envelope("worker_heartbeat", {{"worker_id", "w"}});
    response(a.value, heartbeat, false);
    auto reg = envelope("worker_register", {{"worker_id", "w"}, {"hostname", "h"}, {"slots", 1u}});
    // Fragment a real registration frame across writes.
    auto packet = MessageCodec::encode(reg.dump());
    sendBytes(a.value, packet.data(), 2); sendBytes(a.value, packet.data()+2, packet.size()-2);
    CHECK(receiveJson(a.value)["data"]["code"] == 0);
    response(a.value, reg, true);
    auto impostor = reg; impostor["data"]["worker_id"] = "phantom";
    response(a.value, impostor, false); CHECK(!host.worker.getWorkerManager().getWorker("phantom"));
    response(a.value, envelope("submit_task", {{"task_type", "REGRESSION"}, {"target", "x"}, {"revision", "r"}}), false);
    auto bad = MessageCodec::encode("{"); sendBytes(a.value, bad.data(), bad.size());
    CHECK(receiveJson(a.value)["data"]["code"] != 0);
    auto b = connectTo(host.port); response(b.value, reg, true);
    response(a.value, heartbeat, false); a.reset();
    response(b.value, heartbeat, true);
    CHECK(host.worker.getWorkerManager().getWorker("w")->getStatus() == WorkerStatus::ONLINE);
    CHECK(host.worker.runOnce().dispatched_count == 1);
    auto assigned = receiveJson(b.value);
    CHECK(assigned["type"] == "task_assign" && assigned["data"]["task_id"] == *task);
    CHECK(host.worker.getWorkerManager().getWorker("w")->getUsedSlots() == 1);
    auto start = envelope("task_start", {{"task_id", *task}, {"worker_id", "w"}});
    auto finish = envelope("task_result", {{"task_id", *task}, {"worker_id", "w"}, {"status", "SUCCEEDED"}});
    response(b.value, finish, false);
    auto wrong = start; wrong["data"]["worker_id"] = "other"; response(b.value, wrong, false);
    // Disconnect while ASSIGNED: reservation stays until explicit completion/cancellation.
    b.reset();
    eventually([&] { return host.worker.getWorkerManager().getWorker("w")->getStatus() == WorkerStatus::OFFLINE; });
    CHECK(host.worker.getWorkerManager().getWorker("w")->getUsedSlots() == 1);
    auto c = connectTo(host.port); response(c.value, reg, true);
    response(c.value, start, true); response(c.value, start, false);
    CHECK(host.worker.getWorkerManager().getWorker("w")->getUsedSlots() == 1);
    c.reset();
    eventually([&] { return host.worker.getWorkerManager().getWorker("w")->getStatus() == WorkerStatus::OFFLINE; });
    CHECK(host.worker.getTaskService().getTask(*task)->getStatus() == TaskStatus::RUNNING);
    auto d = connectTo(host.port); response(d.value, reg, true);
    response(d.value, finish, true); response(d.value, finish, false);
    CHECK(host.worker.getWorkerManager().getWorker("w")->getUsedSlots() == 0);
    sendJson(d.value, heartbeat); CHECK(::shutdown(d.value, SHUT_WR) == 0);
    CHECK(receiveJson(d.value)["data"]["code"] == 0); d.reset();
    eventually([&] { return !host.worker.getWorkerConnectionRegistry().hasConnection("w"); });
    Task unsent(999, TaskType::REGRESSION, "x", "r");
    NetworkWorkerDispatcher dispatcher(host.worker.getWorkerConnectionRegistry());
    CHECK(!dispatcher.dispatch("w", unsent));
    auto live = connectTo(host.port); response(live.value, reg, true);
    host.stop(); CHECK(host.result);
}); }
