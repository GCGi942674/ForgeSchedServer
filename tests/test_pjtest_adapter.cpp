#include "ControlTestSupport.h"
#include "config/Config.h"
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
extern char** environ;
struct Child {
    pid_t pid{-1};
    ~Child() { if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); } }
};
nlohmann::json invoke(const std::vector<std::string>& args, int expected = 0) {
    int pipefd[2]; CHECK(::pipe2(pipefd, O_CLOEXEC) == 0);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    std::vector<char*> raw;
    for (auto& item : args) raw.push_back(const_cast<char*>(item.c_str()));
    raw.push_back(nullptr);
    pid_t pid; CHECK(::posix_spawn(&pid, args[0].c_str(), &actions, nullptr, raw.data(), environ) == 0);
    posix_spawn_file_actions_destroy(&actions); ::close(pipefd[1]);
    std::string text; char buf[4096]; ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) text.append(buf, n);
    ::close(pipefd[0]); int status; CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expected);
    return nlohmann::json::parse(text);
}
int main(int argc, char** argv) { return testMain([&] {
    CHECK(argc == 5);
    char path[] = "/tmp/forgesched-pjtest-XXXXXX"; CHECK(::mkdtemp(path));
    struct Cleanup { const char* path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{path};
    invoke({argv[1], argv[4], path}); // fixture prints JSON below
    { const auto settings = std::filesystem::path(path) / "worker.json";
      std::ifstream input(settings); nlohmann::json worker_config; input >> worker_config;
      worker_config["flow_profiles"] = nlohmann::json::object();
      worker_config["require_server_flow"] = true;
      std::ofstream output(settings); output << worker_config.dump(); }
    const auto templates = std::filesystem::path(path) / "templates";
    std::filesystem::create_directories(templates);
    { std::ofstream file(templates / "route.json");
      file << R"({"flow_config":{"original":1,"route_design":1}})"; }
    { std::ofstream file(std::filesystem::path(path) / "server.conf");
      file << "server.templates_dir=" << templates.string() << "\n"; }
    CHECK(ForgeSched::Config::instance().load(std::string(path) + "/server.conf"));
    ControlTestServer host;
    auto client = connectTo(host.port);
    const auto tasks_before = host.worker.getScheduler().getTasks().size();
    for (const auto& payload : {
            nlohmann::json{{"spec_version", 1}, {"case", "../escape/run.tcl"}, {"flow", "route"}, {"timeout_seconds", 1}},
            nlohmann::json{{"spec_version", 1}, {"case", "cases/success/run.tcl"}, {"flow", "route;exit"}, {"timeout_seconds", 1}},
            nlohmann::json{{"spec_version", 1}, {"case", "cases/success/run.tcl"}, {"flow", "route"}, {"timeout_seconds", 0}},
            nlohmann::json{{"command", "exit 0"}}}) {
        const auto answer = exchangeJson(client.value, envelope("submit_task",
            {{"task_type", "REGRESSION"}, {"target", "xcvu9p"}, {"revision", "58231"},
             {"payload", payload}}));
        CHECK(answer["code"] == 1);
    }
    CHECK(host.worker.getScheduler().getTasks().size() == tasks_before);
    Child worker;
    std::vector<std::string> args{argv[1], argv[2], "--worker-id", "pjtest-fixture",
        "--port", std::to_string(host.port), "--output", std::string(path)+"/output",
        "--pjtest-config", std::string(path)+"/worker.json"};
    std::vector<char*> raw;
    for (auto& item : args) raw.push_back(item.data());
    raw.push_back(nullptr);
    CHECK(::posix_spawn(&worker.pid, argv[1], nullptr, nullptr, raw.data(), environ) == 0);
    std::string cli = argv[3], port = std::to_string(host.port);
    for (auto mode : {"success", "business_fail", "process_fail", "timeout", "missing",
                      "invalid", "stale", "wrong_case", "pass_nonzero", "missing_artifact"}) {
        const std::string revision = std::string(mode) == "missing_artifact" ? "99999" : "58231";
        auto response = invoke({cli, "--port", port, "submit", "--target", "xcvu9p",
            "--revision", revision, "--case", std::string("cases/")+mode+"/run.tcl",
            "--flow", "route", "--case-timeout", "1"});
        CHECK(response["code"] == 0);
        auto id = response["result"]["task_id"].get<uint64_t>();
        std::string final;
        if (std::string(mode) == "success" || std::string(mode) == "pass_nonzero") final = "SUCCEEDED";
        else if (std::string(mode) == "timeout") final = "TIMEOUT";
        else final = "FAILED";
        eventually([&] {
            auto q = invoke({cli, "--port", port, "query", std::to_string(id)});
            return q["result"]["status"] == final;
        });
        auto query = invoke({cli, "--port", port, "query", std::to_string(id)});
        CHECK(query["result"]["payload"]["case"] == std::string("cases/")+mode+"/run.tcl");
        CHECK(query["result"]["payload"]["spec_version"] == 2);
        CHECK(query["result"]["payload"]["flow_config"]["route_design"] == 1);
        CHECK(query["result"]["status"] == final);
        if (std::string(mode) == "missing_artifact")
            CHECK(query["result"]["execution"]["reason"].get<std::string>().find(
                "artifact not found") != std::string::npos);
        // Each run records a local result before the Worker sends TASK_RESULT.
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(std::string(path)+"/output")) {
            auto file = entry.path() / std::to_string(id) / "result.json";
            if (!std::filesystem::exists(file)) continue;
            std::ifstream in(file); nlohmann::json record; in >> record;
            CHECK(record["status"] == final);
            CHECK(record["task_id"] == id);
            if (std::string(mode) == "stale") CHECK(record["reason"] == "missing_result");
            if (std::string(mode) == "invalid" || std::string(mode) == "wrong_case")
                CHECK(record["reason"] == "invalid_result");
            if (std::string(mode) == "pass_nonzero")
                CHECK(record["raw_exit_code"] == 7 && record["exit_code"] == 0);
            if (std::string(mode) == "missing_artifact")
                CHECK(record["reason"].get<std::string>().find("artifact not found") != std::string::npos);
            found = true;
        }
        CHECK(found);
    }
    std::ifstream flow(std::string(path)+"/slot/test2/flow_config");
    std::string line; std::getline(flow, line); CHECK(line == "original 1");
    int clean_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(std::string(path)+"/output")) {
        if (entry.path().filename() != ".fixture_clean_calls") continue;
        std::ifstream clean(entry.path());
        while (std::getline(clean, line)) ++clean_count;
    }
    CHECK(clean_count == 18);
    auto cleanup_failure = invoke({cli, "--port", port, "submit", "--target", "xcvu9p",
        "--revision", "58231", "--case", "cases/cleanup_fail/run.tcl", "--flow", "route",
        "--case-timeout", "1"});
    const auto failed_id = cleanup_failure["result"]["task_id"].get<uint64_t>();
    int status; eventually([&] { return ::waitpid(worker.pid, &status, WNOHANG) == worker.pid; });
    worker.pid = -1; CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    CHECK(host.worker.getScheduler().getTask(failed_id)->getStatus() == ForgeSched::TaskStatus::RUNNING);
    auto next = invoke({cli, "--port", port, "submit", "--target", "xcvu9p", "--revision", "58231",
        "--case", "cases/success/run.tcl", "--flow", "route", "--case-timeout", "1"});
    const auto next_id = next["result"]["task_id"].get<uint64_t>();
    CHECK(host.worker.getScheduler().getTask(next_id)->getStatus() == ForgeSched::TaskStatus::QUEUED);
    host.stop(); CHECK(host.result);
}); }
