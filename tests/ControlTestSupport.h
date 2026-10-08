#pragma once
#include "TestSupport.h"
#include "WorkerServer.h"
#include <sys/eventfd.h>
struct ControlTestServer {
    Fd signal{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    ForgeSched::WorkerServer worker;
    EchoHandler handler;
    int port{freePort()};
    EchoServer server{port, handler, signal.value, 2, 2};
    bool result{false};
    std::thread thread;
    ControlTestServer() {
        CHECK(signal.value >= 0);
        worker.attach(server);
        thread = std::thread([this] { result = server.run(); });
    }
    void stop() {
        if (!thread.joinable()) return;
        uint64_t one = 1;
        ssize_t n;
        do { n = ::write(signal.value, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
        if (n != sizeof(one)) std::terminate();
        thread.join();
    }
    ~ControlTestServer() { stop(); }
};
inline nlohmann::json exchangeJson(int fd, const nlohmann::json& request) {
    sendJson(fd, request);
    auto reply = receiveJson(fd);
    CHECK(reply["type"] == "response" && reply["request_id"] == request["request_id"]);
    return reply["data"];
}
