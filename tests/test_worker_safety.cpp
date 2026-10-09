#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
#include <iostream>
extern char** environ;
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    char option[] = "-B";
    char* args[] = {argv[1], option, argv[2], nullptr};
    pid_t pid;
    if (posix_spawn(&pid, argv[1], nullptr, nullptr, args, environ)) return 1;
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return 1;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        std::cerr << "Worker safety regression failed\n";
        return 1;
    }
    return 0;
}
