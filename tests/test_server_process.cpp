#include "TestSupport.h"
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <signal.h>
struct Child {
    pid_t pid{-1};
    ~Child() { if (pid > 0) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); } }
    int wait() {
        int status = 0;
        eventually([&] { auto r = ::waitpid(pid, &status, WNOHANG); CHECK(r >= 0); return r == pid; });
        pid = -1; CHECK(WIFEXITED(status)); return WEXITSTATUS(status);
    }
};
int main(int argc, char** argv) { return testMain([&] {
    CHECK(argc == 2); auto executable = std::filesystem::absolute(argv[1]).string();
    char path[] = "/tmp/forgesched-process-XXXXXX"; CHECK(::mkdtemp(path));
    struct Cleanup { const char* path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{path};
    std::filesystem::create_directory(std::string(path)+"/config");
    auto configure = [&](int port) {
        std::ofstream out(std::string(path)+"/config/forgesched.conf"); CHECK(out.good());
        out << "server.port=" << port << "\nserver.io_threads=2\nserver.worker_threads=2\nlog.level=WARN\nlog.dir=./logs\n";
    };
    auto launch = [&](Child& child) {
        child.pid = ::fork(); CHECK(child.pid >= 0);
        if (child.pid == 0) {
            if (::chdir(path) != 0) ::_exit(126);
            ::execl(executable.c_str(), executable.c_str(), static_cast<char*>(nullptr)); ::_exit(127);
        }
    };
    for (int sig : {SIGTERM, SIGINT}) {
        int port = freePort(); configure(port); Child child; launch(child);
        auto conn = connectTo(port);
        response(conn.value, envelope("worker_register", {{"worker_id", "process"}, {"hostname", "h"}, {"slots", 1u}}), true);
        CHECK(::kill(child.pid, sig) == 0); CHECK(child.wait() == 0);
    }
    configure(70000); Child invalid; launch(invalid); CHECK(invalid.wait() == 1);
    Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)); CHECK(listener.value >= 0);
    auto addr = address(0); CHECK(::bind(listener.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(::listen(listener.value, 1) == 0); socklen_t size = sizeof(addr);
    CHECK(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&addr), &size) == 0);
    configure(ntohs(addr.sin_port)); Child conflict; launch(conflict); CHECK(conflict.wait() == 1);
}); }
