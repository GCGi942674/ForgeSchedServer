#include "ControlTestSupport.h"
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <filesystem>
extern char** environ;
struct Child {
    pid_t pid{-1};
    ~Child() { if (pid > 0) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); } }
};
struct Result { int code; std::string output; };
Result run(const std::string& executable, std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), executable);
    std::vector<char*> argv;
    for (auto& arg : arguments) argv.push_back(arg.data());
    argv.push_back(nullptr);
    int pipes[2]; CHECK(::pipe2(pipes, O_CLOEXEC) == 0);
    Fd input(pipes[0]), output(pipes[1]);
    posix_spawn_file_actions_t actions;
    CHECK(::posix_spawn_file_actions_init(&actions) == 0);
    struct Actions { posix_spawn_file_actions_t* p; ~Actions() { ::posix_spawn_file_actions_destroy(p); } } cleanup{&actions};
    CHECK(::posix_spawn_file_actions_adddup2(&actions, output.value, STDOUT_FILENO) == 0);
    CHECK(::posix_spawn_file_actions_addclose(&actions, input.value) == 0);
    CHECK(::posix_spawn_file_actions_addclose(&actions, output.value) == 0);
    Child child;
    CHECK(::posix_spawn(&child.pid, executable.c_str(), &actions, nullptr, argv.data(), environ) == 0);
    output.reset();
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (true) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        pollfd p{input.value, POLLIN, 0};
        auto ready = ::poll(&p, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        CHECK(ready >= 0);
        if (!ready) continue;
        char bytes[4096]; auto n = ::read(input.value, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR) continue;
        CHECK(n >= 0);
        if (!n) break;
        text.append(bytes, static_cast<size_t>(n));
        CHECK(text.size() <= 1024 * 1024);
    }
    int status;
    eventually([&] {
        auto n = ::waitpid(child.pid, &status, WNOHANG);
        CHECK(n >= 0); return n == child.pid;
    });
    child.pid = -1;
    CHECK(WIFEXITED(status));
    return {WEXITSTATUS(status), text};
}
int main(int argc, char** argv) { return testMain([&] {
    CHECK(argc == 2);
    const auto executable = std::filesystem::absolute(argv[1]).string();
    ControlTestServer host;
    auto invoke = [&](std::vector<std::string> args) {
        args.insert(args.begin(), {"--host", "127.0.0.1", "--port", std::to_string(host.port)});
        return run(executable, std::move(args));
    };
    CHECK(run(executable, {"--help"}).code == 0);
    for (auto args : std::vector<std::vector<std::string>>{
        {}, {"query", "0"}, {"query", "-1"}, {"query", "18446744073709551616"},
        {"query", "1junk"}, {"submit", "--target", "x"}, {"query", "1", "--priority", "2"},
        {"submit", "--target", "x", "--revision", "r", "--priority", "101"},
        {"--host", "not-an-ip", "query", "1"}, {"--port", "70000", "query", "1"},
        {"--timeout-ms", "0", "query", "1"}, {"--port", "1", "--port", "2", "query", "1"}})
        CHECK(run(executable, std::move(args)).code == 64);
    auto submitted = invoke({"submit", "--target", "cli case", "--revision", "r 1", "--priority", "-3"});
    CHECK(submitted.code == 0);
    auto value = nlohmann::json::parse(submitted.output);
    const auto id = value["result"]["task_id"].get<uint64_t>();
    auto queried = invoke({"query", std::to_string(id)});
    CHECK(queried.code == 0);
    CHECK(nlohmann::json::parse(queried.output)["result"]["status"] == "QUEUED");
    CHECK(invoke({"cancel", std::to_string(id)}).code == 0);
    CHECK(nlohmann::json::parse(invoke({"query", std::to_string(id)}).output)["result"]["status"] == "CANCELLED");
    CHECK(invoke({"cancel", std::to_string(id)}).code == 2);
    CHECK(invoke({"query", "999999"}).code == 2);
    // The executable itself participates in the complete network execution flow.
    auto worker = connectTo(host.port);
    response(worker.value, envelope("worker_register", {{"worker_id", "cli-worker"}, {"hostname", "h"}, {"slots", 1u}}), true);
    submitted = invoke({"submit", "--target", "cli executed", "--revision", "r2"});
    CHECK(submitted.code == 0);
    const auto run_id = nlohmann::json::parse(submitted.output)["result"]["task_id"].get<uint64_t>();
    CHECK(receiveJson(worker.value)["data"]["task_id"] == run_id);
    response(worker.value, envelope("task_start", {{"task_id", run_id}, {"worker_id", "cli-worker"}}), true);
    response(worker.value, envelope("task_result", {{"task_id", run_id}, {"worker_id", "cli-worker"}, {"status", "SUCCEEDED"}}), true);
    CHECK(nlohmann::json::parse(invoke({"query", std::to_string(run_id)}).output)["result"]["status"] == "SUCCEEDED");
    // Bound-but-not-listening socket guarantees a refused connection without a port race.
    Fd unavailable(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)); CHECK(unavailable.value >= 0);
    auto addr = address(0);
    CHECK(::bind(unavailable.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t length = sizeof(addr);
    CHECK(::getsockname(unavailable.value, reinterpret_cast<sockaddr*>(&addr), &length) == 0);
    CHECK(run(executable, {"--port", std::to_string(ntohs(addr.sin_port)), "--timeout-ms", "100", "query", "1"}).code == 1);
    host.stop(); CHECK(host.result);
}); }
