#include "auth.hpp"
#include "normalize.hpp"

#include <braurser/validate/validate.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef BRAURSER_HELPER_PATH
#define BRAURSER_HELPER_PATH "/usr/local/libexec/braurser/braurser-pacman-helper"
#endif

namespace {
int reject(const char* reason) {
    std::fprintf(stderr, "braurser-elevate: rejected %s\n", reason);
    return 1;
}

int trusted_askpass(const char* path) {
    if (!path || path[0] != '/' || strnlen(path, 4097) > 4096) return 0;
    int dir = open("/", O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return 0;
    const char* current = path + 1;
    for (;;) {
        const char* slash = std::strchr(current, '/');
        size_t length = slash ? static_cast<size_t>(slash - current) : std::strlen(current);
        if (!br_valid_path_component(current, length)) { close(dir); return 0; }
        char name[256];
        std::memcpy(name, current, length);
        name[length] = 0;
        int fd = openat(dir, name, O_PATH | O_NOFOLLOW | O_CLOEXEC | (slash ? O_DIRECTORY : 0));
        close(dir);
        if (fd < 0) return 0;
        struct stat st{};
        if (fstat(fd, &st) || st.st_uid != 0 || (st.st_mode & 0022) ||
            (slash ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode) || !(st.st_mode & 0111))) {
            close(fd);
            return 0;
        }
        if (!slash) { close(fd); return 1; }
        dir = fd;
        current = slash + 1;
    }
}

bool valid_job(const char* path, const char* home) {
    if (!path || !home || home[0] != '/' || strnlen(path, 4097) > 4096 ||
        strnlen(home, 2049) > 2048 || home[std::strlen(home) - 1] == '/') return false;
    std::string prefix = std::string(home) + "/.cache/braurser/build/";
    if (std::strncmp(path, prefix.c_str(), prefix.size()) != 0) return false;
    const char* helper = path + prefix.size();
    const char* slash = std::strchr(helper, '/');
    if (!slash || !(std::string_view(helper, static_cast<size_t>(slash - helper)) == "paru" ||
                    std::string_view(helper, static_cast<size_t>(slash - helper)) == "yay")) return false;
    const char* job = slash + 1;
    return !std::strchr(job, '/') && br_valid_path_component(job, std::strlen(job));
}

}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 531) return reject("argc");
    if (getuid() == 0 || geteuid() != getuid()) return reject("caller");
    const char* policy = std::getenv("BRAURSER_JOB_POLICY");
    if (!policy || std::strcmp(policy, "aur-build") != 0) return reject("policy");
    struct passwd pwd{};
    struct passwd* entry = nullptr;
    char buffer[16384];
    if (getpwuid_r(getuid(), &pwd, buffer, sizeof(buffer), &entry) || !entry) return reject("caller");
    const char* job = std::getenv("BRAURSER_JOB_DIR");
    if (!valid_job(job, pwd.pw_dir)) return reject("job-dir");
    const char* method = std::getenv("BRAURSER_ELEVATION");
    if (!method) return reject("method");
    bool pkexec = std::strcmp(method, "pkexec") == 0;
    if (!pkexec && std::strcmp(method, "sudo-askpass") != 0) return reject("method");
    std::string askpass;
    std::vector<std::string> env;
    if (!pkexec) {
        const char* ask = std::getenv("SUDO_ASKPASS");
        if (!trusted_askpass(ask)) return reject("askpass");
        askpass = ask;
        env.push_back("SUDO_ASKPASS=" + askpass);
        for (const char* name : {"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY", "LANG"}) {
            const char* value = std::getenv(name);
            if (value && strnlen(value, 4097) <= 4096 && !std::strchr(value, '\n'))
                env.emplace_back(std::string(name) + "=" + value);
        }
        const char* action = std::getenv("BRAURSER_AUTH_ACTION");
        if (action && (std::strcmp(action, "install") == 0 || std::strcmp(action, "remove") == 0 ||
                       std::strcmp(action, "update") == 0 || std::strcmp(action, "git") == 0))
            env.emplace_back(std::string("BRAURSER_AUTH_ACTION=") + action);
        env.emplace_back("PATH=/usr/bin");
    }
    const char* command = pkexec ? "/usr/bin/pkexec" : "/usr/bin/sudo";
    if (access(command, X_OK) != 0) return reject("binary");
    bool probe = argc == 2 && std::strcmp(argv[1], "-v") == 0;
    std::vector<std::string> args;
    if (probe) args = pkexec ? std::vector<std::string>{BRAURSER_HELPER_PATH, "--auth-check"} :
                                std::vector<std::string>{"-A", "--", BRAURSER_HELPER_PATH, "--auth-check"};
    else {
        if (argc < 4 || (std::strcmp(argv[1], "pacman") && std::strcmp(argv[1], "/usr/bin/pacman")))
            return reject("program");
        std::vector<std::string> input;
        for (int i = 1; i < argc; ++i) {
            if (!argv[i] || strnlen(argv[i], 4097) > 4096) return reject("argument");
            input.emplace_back(argv[i]);
        }
        auto normalized = braurser::elevate::normalize(input);
        if (!normalized) return reject(normalized.reason.c_str());
        if (!braurser::elevate::allowed_for_job(normalized, job)) return reject("job-policy");
        if (!pkexec) { args = {"-A", "--", BRAURSER_HELPER_PATH}; }
        else args = {BRAURSER_HELPER_PATH};
        args.insert(args.end(), normalized.args.begin(), normalized.args.end());
    }
    if (probe) return braurser::elevate::auth_check(command, args, job, env);
    std::vector<char*> raw{const_cast<char*>(command)};
    for (auto& arg : args) raw.push_back(arg.data());
    raw.push_back(nullptr);
    if (pkexec) execv(command, raw.data());
    else {
        std::vector<char*> envp;
        for (auto& item : env) envp.push_back(item.data());
        envp.push_back(nullptr);
        execve(command, raw.data(), envp.data());
    }
    return reject("exec");
}
