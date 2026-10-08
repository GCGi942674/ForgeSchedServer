#include "TestSupport.h"
#include "ForgeClient.h"
#include <functional>
#include <poll.h>
using namespace ForgeSched;
using namespace ForgeSched::Protocol;
struct FakeServer {
    Fd listener{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    int port{0}; std::thread thread; std::exception_ptr error;
    FakeServer(std::function<void(int, const nlohmann::json&)> action) {
        CHECK(listener.value >= 0);
        auto addr = address(0);
        CHECK(::bind(listener.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        CHECK(::listen(listener.value, 1) == 0);
        socklen_t length = sizeof(addr);
        CHECK(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&addr), &length) == 0);
        port = ntohs(addr.sin_port);
        thread = std::thread([this, action] {
            try {
                pollfd readable{listener.value, POLLIN, 0};
                CHECK(::poll(&readable, 1, 3000) == 1);
                Fd socket(::accept4(listener.value, nullptr, nullptr, SOCK_CLOEXEC));
                CHECK(socket.value >= 0);
                timeval timeout{2, 0};
                CHECK(::setsockopt(socket.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
                auto request = receiveJson(socket.value);
                action(socket.value, request);
            } catch (...) { error = std::current_exception(); }
        });
    }
    void finish() { if (thread.joinable()) thread.join(); if (error) std::rethrow_exception(error); }
    ~FakeServer() { if (thread.joinable()) thread.join(); }
};
int main() { return testMain([] {
    for (int mode = 0; mode < 11; ++mode) {
        FakeServer server([mode](int fd, const nlohmann::json& request) {
            auto message = envelope("response", {{"code", 0}, {"message", "ok"}, {"result", {{"status", "QUEUED"}}}},
                request["request_id"].get<uint64_t>());
            if (mode == 1) message["request_id"] = 999u;
            if (mode == 2) message["type"] = "task_assign";
            if (mode == 3) message["data"].erase("code");
            if (mode == 4) message["data"]["code"] = 4294967296ULL;
            if (mode == 5) {
                uint32_t size = htonl(MessageCodec::kMaxBodyLenght+1);
                sendBytes(fd, reinterpret_cast<const char*>(&size), sizeof(size)); return;
            }
            if (mode == 6) {
                uint32_t size = htonl(100);
                sendBytes(fd, reinterpret_cast<const char*>(&size), sizeof(size));
                sendBytes(fd, "{", 1); return; // Truncated frame/EOF.
            }
            if (mode == 7) { std::this_thread::sleep_for(std::chrono::milliseconds(180)); return; }
            if (mode == 8) {
                auto packet = MessageCodec::encode(message.dump());
                for (size_t n = 0; n < 4; ++n) {
                    if (::send(fd, packet.data()+n, 1, MSG_NOSIGNAL) != 1) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(45));
                }
                return; // Drip feed must not extend the transaction deadline.
            }
            if (mode == 9) message["version"] = 2u;
            if (mode == 10) {
                auto packet = MessageCodec::encode("{");
                sendBytes(fd, packet.data(), packet.size()); return;
            }
            auto packet = MessageCodec::encode(message.dump());
            sendBytes(fd, packet.data(), 2); sendBytes(fd, packet.data()+2, packet.size()-2);
        });
        ForgeClient client("127.0.0.1", server.port, mode == 7 || mode == 8 ? 80 : 2000);
        bool rejected = false;
        std::string failure;
        const auto start = std::chrono::steady_clock::now();
        try {
            auto response = client.query(1);
            CHECK(response.data["code"] == 0);
        } catch (const std::runtime_error& e) { rejected = true; failure = e.what(); }
        CHECK(rejected == (mode != 0));
        if (mode == 7 || mode == 8) CHECK(failure == "request deadline exceeded");
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
        server.finish();
    }
    ForgeClient client("127.0.0.1", 1);
    bool rejected = false;
    try { client.request(MessageType::WORKER_REGISTER, nlohmann::json::object()); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}); }
