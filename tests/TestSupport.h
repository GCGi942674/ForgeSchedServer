#pragma once
#include <stdexcept>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>
#include <cerrno>
#include <utility>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "protocol/Message_codec.h"
#include "nlohmann/json.hpp"

// Unlike assert, checks and their side effects also run in Release builds.
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #expr); } while (false)
struct Fd {
    int value{-1};
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    void reset() { if (value >= 0) ::close(std::exchange(value, -1)); }
};
template<class F> void eventually(F condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
inline sockaddr_in address(int port) {
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(port);
    return addr;
}
inline int freePort() {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)); CHECK(fd.value >= 0);
    auto addr = address(0); CHECK(::bind(fd.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(::getsockname(fd.value, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    return ntohs(addr.sin_port);
}
inline Fd connectTo(int port) {
    int connected = -1;
    eventually([&] {
        Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)); CHECK(fd.value >= 0);
        auto addr = address(port);
        if (::connect(fd.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        timeval timeout{3, 0};
        CHECK(::setsockopt(fd.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        CHECK(::setsockopt(fd.value, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
        connected = std::exchange(fd.value, -1); return true;
    });
    return Fd(connected);
}
inline void sendBytes(int fd, const char* data, size_t size) {
    while (size) {
        auto n = ::send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0); data += n; size -= n;
    }
}
inline void receiveBytes(int fd, char* data, size_t size) {
    while (size) {
        auto n = ::recv(fd, data, size, 0);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0); data += n; size -= n;
    }
}
inline nlohmann::json envelope(const std::string& type, nlohmann::json data, uint64_t id = 7) {
    return {{"version", 1u}, {"type", type}, {"request_id", id}, {"data", std::move(data)}};
}
inline void sendJson(int fd, const nlohmann::json& j) {
    auto bytes = MessageCodec::encode(j.dump()); sendBytes(fd, bytes.data(), bytes.size());
}
inline nlohmann::json receiveJson(int fd) {
    uint32_t length; receiveBytes(fd, reinterpret_cast<char*>(&length), sizeof(length));
    length = ntohl(length); CHECK(length <= MessageCodec::kMaxBodyLenght);
    std::string body(length, '\0'); receiveBytes(fd, body.data(), body.size());
    return nlohmann::json::parse(body);
}
inline void response(int fd, const nlohmann::json& request, bool ok) {
    sendJson(fd, request); auto reply = receiveJson(fd);
    CHECK(reply.at("type") == "response");
    CHECK(reply.at("request_id") == request.at("request_id"));
    CHECK((reply.at("data").at("code") == 0) == ok);
}
template<class F> int testMain(F body) {
    try { body(); std::cout << "PASS\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
