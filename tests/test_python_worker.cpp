#include "ControlTestSupport.h"
#include "ForgeClient.h"
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
extern char** environ;
struct Process {
    pid_t pid{-1};
    ~Process() { if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); } }
};
int main(int argc, char** argv) { return testMain([&] {
    CHECK(argc == 3);
    char path[] = "/tmp/forge-worker-test-XXXXXX"; CHECK(::mkdtemp(path));
    struct Cleanup { const char* p; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); } } cleanup{path};
    for (int mode = 0; mode < 5; ++mode) {
        ControlTestServer server;
        auto ready = connectTo(server.port);
        auto output = std::string(path) + "/" + std::to_string(mode);
        std::vector<std::string> args{argv[1], argv[2], "--port", std::to_string(server.port),
            "--worker-id", "process-worker", "--output", output, "--demo-seconds",
            mode >= 2 ? "20" : "0.1", "--demo-exit-code", mode == 1 ? "7" : "0",
            "--task-timeout", mode == 2 ? "0.2" : "5"};
        std::vector<char*> raw;
        for (auto& a : args) raw.push_back(a.data());
        raw.push_back(nullptr);
        Process child;
        CHECK(::posix_spawn(&child.pid, argv[1], nullptr, nullptr, raw.data(), environ) == 0);
        ForgeSched::ForgeClient client("127.0.0.1", server.port);
        if (mode >= 3) {
            CHECK(client.submit("shutdown", "r").data["code"] == 0);
            int processId = 0;
            eventually([&] {
                if (!std::filesystem::exists(output)) return false;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(output)) {
                    if (entry.path().filename() == "pid") {
                        std::ifstream in(entry.path()); in >> processId;
                    }
                }
                return processId > 0;
            });
            if (mode == 3) CHECK(::kill(child.pid, SIGTERM) == 0);
            else server.stop();
            int status = 0;
            eventually([&] { return ::waitpid(child.pid, &status, WNOHANG) == child.pid; });
            child.pid = -1;
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (mode == 3 ? 0 : 1));
            CHECK(::kill(processId, 0) == -1 && errno == ESRCH);
            continue;
        }
        std::vector<uint64_t> ids;
        for (int i = 0; i < 3; ++i) {
            auto reply = client.submit("not-a-shell-command; exit 99", "r");
            CHECK(reply.data["code"] == 0);
            ids.push_back(reply.data["result"]["task_id"].get<uint64_t>());
        }
        for (auto id : ids) eventually([&] {
            auto result = client.query(id);
            return result.data["result"]["status"] == (mode == 0 ? "SUCCEEDED" : mode == 1 ? "FAILED" : "TIMEOUT");
        });
        int count = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(output)) {
            if (entry.path().filename() != "result.json") continue;
            std::ifstream in(entry.path()); nlohmann::json value; in >> value;
            CHECK(value["exit_code"] == (mode == 0 ? 0 : mode == 1 ? 7 : -9));
            CHECK(value["pid"].get<int>() > 0);
            CHECK(::kill(value["pid"].get<int>(), 0) == -1 && errno == ESRCH);
            for (const auto* log : {"stdout.log", "stderr.log"}) {
                CHECK(std::filesystem::is_regular_file(entry.path().parent_path() / log));
                // A timed-out interpreter may be killed before its first write.
                if (mode != 2) CHECK(std::filesystem::file_size(entry.path().parent_path() / log) > 0);
            }
            ++count;
        }
        CHECK(count == 3);
        CHECK(::kill(child.pid, SIGTERM) == 0);
        int status = 0;
        eventually([&] { return ::waitpid(child.pid, &status, WNOHANG) == child.pid; });
        child.pid = -1;
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}); }
