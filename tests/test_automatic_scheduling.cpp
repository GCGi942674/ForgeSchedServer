#include "TestSupport.h"
#include "WorkerServer.h"
#include <sys/eventfd.h>
using namespace ForgeSched;

// Real framed TCP messages: no manual driver/coordinator calls.
int main() { return testMain([] {
    Fd signal(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)); CHECK(signal.value >= 0);
    WorkerServer worker; EchoHandler handler;
    int port = freePort();
    EchoServer server(port, handler, signal.value, 2, 2);
    worker.attach(server);
    auto a = worker.getTaskService().createTask({TaskType::REGRESSION, "a", "r", 0});
    auto b = worker.getTaskService().createTask({TaskType::REGRESSION, "b", "r", 0});
    CHECK(a && b);
    bool result = false;
    std::thread thread([&] { result = server.run(); });
    auto stop = [&] {
        if (thread.joinable()) {
            uint64_t one = 1;
            ssize_t written;
            do { written = ::write(signal.value, &one, sizeof(one)); }
            while (written < 0 && errno == EINTR);
            if (written != sizeof(one)) std::terminate();
            thread.join();
        }
    };
    try {
        auto socket = connectTo(port);
        response(socket.value, envelope("worker_register",
            {{"worker_id", "w"}, {"hostname", "h"}, {"slots", 1u}}), true);
        auto assignment = receiveJson(socket.value);
        CHECK(assignment["type"] == "task_assign");
        CHECK(assignment["data"]["task_id"] == *a);
        CHECK(worker.getTaskService().getTask(*b)->getStatus() == TaskStatus::QUEUED);
        CHECK(worker.getWorkerManager().getWorker("w")->getUsedSlots() == 1);
        response(socket.value, envelope("task_start", {{"task_id", *a}, {"worker_id", "w"}}), true);
        sendJson(socket.value, envelope("task_result",
            {{"task_id", *a}, {"worker_id", "w"}, {"status", "SUCCEEDED"}}, 42));
        // Router may enqueue assignment before the result acknowledgement.
        bool got_assignment = false, got_response = false;
        for (int n = 0; n < 2; ++n) {
            auto message = receiveJson(socket.value);
            if (message["type"] == "task_assign") {
                CHECK(!got_assignment && message["data"]["task_id"] == *b);
                got_assignment = true;
            } else {
                CHECK(!got_response && message["type"] == "response");
                CHECK(message["request_id"] == 42 && message["data"]["code"] == 0);
                got_response = true;
            }
        }
        CHECK(got_assignment && got_response);
        CHECK(worker.getTaskService().getTask(*a)->getStatus() == TaskStatus::SUCCEEDED);
        CHECK(worker.getTaskService().getTask(*b)->getStatus() == TaskStatus::ASSIGNED);
        CHECK(worker.getWorkerManager().getWorker("w")->getUsedSlots() == 1);
        response(socket.value, envelope("task_start", {{"task_id", *b}, {"worker_id", "w"}}), true);
        response(socket.value, envelope("task_result",
            {{"task_id", *b}, {"worker_id", "w"}, {"status", "FAILED"}}), true);
        CHECK(worker.getWorkerManager().getWorker("w")->getUsedSlots() == 0);
        stop(); CHECK(result);
    } catch (...) { stop(); throw; }
}); }
