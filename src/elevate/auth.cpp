#include "auth.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace braurser::elevate {
int auth_check(const std::string& program, const std::vector<std::string>& args,
               const std::string& job_dir, const std::vector<std::string>& environment) {
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> raw{const_cast<char*>(program.c_str())};
        for (const auto& arg : args) raw.push_back(const_cast<char*>(arg.c_str()));
        raw.push_back(nullptr);
        if (environment.empty()) execv(program.c_str(), raw.data());
        else {
            std::vector<char*> envp;
            for (const auto& item : environment) envp.push_back(const_cast<char*>(item.c_str()));
            envp.push_back(nullptr);
            execve(program.c_str(), raw.data(), envp.data());
        }
        _exit(127);
    }
    int status = 0;
    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) { status = -1; break; }
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    }
    int dir = open(job_dir.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
        int marker = openat(dir, "auth-failed", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (marker >= 0) close(marker);
        close(dir);
    }
    kill(0, SIGTERM);
    return 1;
}
}
