#include "request.hpp"

#include <braurser/validate/validate.hpp>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t child_pid = -1;
void forward_int(int) { if (child_pid > 0) kill(static_cast<pid_t>(child_pid), SIGINT); }

int write_all(int fd, const char* data, size_t size) {
    while (size) {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return 0;
        data += written;
        size -= static_cast<size_t>(written);
    }
    return 1;
}

int system_dir(const char* path) {
    int fd = open(path, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022)) {
        close(fd);
        return -1;
    }
    return fd;
}

int root_dir(int parent, const char* name) {
    if (mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    int fd = openat(parent, name, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077)) {
        close(fd);
        return -1;
    }
    return fd;
}

int open_audit() {
    int var = system_dir("/var");
    if (var < 0) return -1;
    close(var);
    int parent = system_dir("/var/log");
    if (parent < 0) return -1;
    int dir = root_dir(parent, "braurser");
    close(parent);
    if (dir < 0) return -1;
    int fd = openat(dir, "helper.log", O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(dir);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077)) {
        close(fd);
        return -1;
    }
    return fd;
}

int audit_line(int fd, uid_t caller, const br_request& req, const char* const* digests, int status) {
    time_t now = time(nullptr);
    struct tm utc{};
    char stamp[32]{};
    if (!gmtime_r(&now, &utc) || !strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc)) return 0;
    char line[8192]{};
    int used = snprintf(line, sizeof(line), "%s uid=%lu op=%c count=%zu targets=",
                        stamp, static_cast<unsigned long>(caller), req.operation ? req.operation : '-', req.count);
    if (used < 0 || static_cast<size_t>(used) >= sizeof(line)) return 0;
    size_t size = static_cast<size_t>(used);
    size_t targets = 0;
    for (size_t i = 0; i < req.count; ++i) {
        const char* target = req.targets[i];
        if (!target) continue;
        size_t length = strlen(target);
        if (length > 1024 - targets) length = 1024 - targets;
        for (size_t j = 0; j < length; ++j) {
            const unsigned char c = static_cast<unsigned char>(target[j]);
            line[size + j] = c < 32 || c == 127 ? '?' : static_cast<char>(c);
        }
        size += length;
        targets += length;
        if (targets == 1024) break;
        line[size++] = ',';
        ++targets;
    }
    if (digests) {
        for (size_t i = 0; i < req.count; ++i) {
            if (digests[i] && size + 73 < sizeof(line)) {
                int n = snprintf(line + size, sizeof(line) - size, " sha256=%s", digests[i]);
                if (n > 0) size += static_cast<size_t>(n);
            }
        }
    }
    int n = snprintf(line + size, sizeof(line) - size, " status=%d\n", status);
    int ok = n > 0 && static_cast<size_t>(n) < sizeof(line) - size &&
             write_all(fd, line, size + static_cast<size_t>(n));
    return ok;
}

int audit(uid_t caller, const br_request& req, const char* const* digests, int status) {
    int fd = open_audit();
    if (fd < 0) return 0;
    int ok = audit_line(fd, caller, req, digests, status);
    close(fd);
    return ok;
}

int reject(const char* reason, uid_t caller, const br_request& req) {
    audit(caller, req, nullptr, 64);
    dprintf(STDERR_FILENO, "braurser-pacman-helper: rejected %s\n", reason);
    return 64;
}

void clean(const char* dir, char paths[64][4096], size_t count) {
    for (size_t i = 0; i < count; ++i) if (paths[i][0]) unlink(paths[i]);
    if (dir[0]) rmdir(dir);
}

int prepare_stage(char dir[4096]) {
    int var = system_dir("/var");
    if (var < 0) return 0;
    close(var);
    int lib = system_dir("/var/lib");
    if (lib < 0) return 0;
    int base = root_dir(lib, "braurser");
    close(lib);
    if (base < 0) return 0;
    int stage = root_dir(base, "staging");
    close(base);
    if (stage < 0) return 0;
    close(stage);
    memcpy(dir, "/var/lib/braurser/staging/job-XXXXXX", sizeof("/var/lib/braurser/staging/job-XXXXXX"));
    return mkdtemp(dir) != nullptr;
}

int copy_archive(int input, const char* dest, char digest[65]) {
    int output = open(dest, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) return 0;
    br_sha256_ctx hash{};
    br_sha256_init(&hash);
    char buffer[65536];
    uint64_t total = 0;
    int ok = 1;
    for (;;) {
        ssize_t size = read(input, buffer, sizeof(buffer));
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) { ok = 0; break; }
        if (size == 0) break;
        total += static_cast<uint64_t>(size);
        if (total > UINT64_C(4294967296) || !write_all(output, buffer, static_cast<size_t>(size))) {
            ok = 0;
            break;
        }
        br_sha256_update(&hash, reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(size));
    }
    struct stat st{};
    if (fstat(input, &st) || st.st_size < 0 || static_cast<uint64_t>(st.st_size) != total ||
        fsync(output)) ok = 0;
    close(output);
    if (!ok) return 0;
    uint8_t raw[32];
    br_sha256_final(&hash, raw);
    br_hex32(raw, digest);
    return 1;
}

int pacman_trusted() {
    int usr = system_dir("/usr");
    if (usr < 0) return 0;
    close(usr);
    int bin = system_dir("/usr/bin");
    if (bin < 0) return 0;
    close(bin);
    int fd = open("/usr/bin/pacman", O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st{};
    int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 0022) &&
             (st.st_mode & 0111);
    close(fd);
    return ok;
}
}

int main(int argc, char** argv) {
    br_request req{};
    uid_t caller = 0;
    if (geteuid() != 0) return reject("not-root", caller, req);
    if (!br_caller_uid(getenv("PKEXEC_UID"), getenv("SUDO_UID"), &caller)) return reject("caller", caller, req);
    struct passwd pwd{};
    struct passwd* result = nullptr;
    char pwbuf[16384];
    if (getpwuid_r(caller, &pwd, pwbuf, sizeof(pwbuf), &result) || !result || !pwd.pw_dir ||
        pwd.pw_dir[0] != '/' || strlen(pwd.pw_dir) > 2048) return reject("home", caller, req);
    if (!br_parse_request(argc - 1, const_cast<const char* const*>(argv + 1), &req))
        return reject("argv", caller, req);
    if (req.operation == 'F') {
        if (!audit(caller, req, nullptr, 0)) return reject("audit", caller, req);
        return 0;
    }
    if (!pacman_trusted()) return reject("pacman", caller, req);
    char stage[4096]{};
    char copies[64][4096]{};
    char hashes[64][65]{};
    const char* digest_ptrs[64]{};
    char* arguments[532]{};
    arguments[0] = const_cast<char*>("pacman");
    for (int i = 1; i < argc; ++i) arguments[i] = argv[i];
    if (req.operation == 'D') {
        if (!prepare_stage(stage)) return reject("staging", caller, req);
        for (size_t i = 0; i < req.count; ++i) {
            int fd = br_open_archive(req.targets[i], pwd.pw_dir, caller, nullptr);
            if (fd < 0) {
                clean(stage, copies, req.count);
                return reject("archive", caller, req);
            }
            const char* basename = strrchr(req.targets[i], '/');
            int n = snprintf(copies[i], sizeof(copies[i]), "%s/%zu-%s", stage, i, basename + 1);
            if (n < 0 || static_cast<size_t>(n) >= sizeof(copies[i]) ||
                !copy_archive(fd, copies[i], hashes[i])) {
                close(fd);
                clean(stage, copies, req.count);
                return reject("copy", caller, req);
            }
            close(fd);
            digest_ptrs[i] = hashes[i];
            for (int j = 1; j < argc; ++j) if (arguments[j] == req.targets[i]) {
                arguments[j] = copies[i];
                break;
            }
        }
    }
    int log_fd = open_audit();
    if (log_fd < 0) {
        clean(stage, copies, req.count);
        return reject("audit", caller, req);
    }
    if (clearenv() || setenv("PATH", "/usr/bin", 1) || setenv("LC_ALL", "C", 1)) {
        close(log_fd);
        clean(stage, copies, req.count);
        return reject("environment", caller, req);
    }
    char* envp[] = {const_cast<char*>("PATH=/usr/bin"), const_cast<char*>("LC_ALL=C"), nullptr};
    umask(022);
    struct sigaction broken_pipe{};
    broken_pipe.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &broken_pipe, nullptr)) {
        close(log_fd);
        clean(stage, copies, req.count);
        return reject("signals", caller, req);
    }
    pid_t pid = fork();
    if (pid == 0) {
        execve("/usr/bin/pacman", arguments, envp);
        _exit(127);
    }
    if (pid < 0) {
        close(log_fd);
        clean(stage, copies, req.count);
        return reject("fork", caller, req);
    }
    child_pid = pid;
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGTERM, &ignore, nullptr);
    sigaction(SIGHUP, &ignore, nullptr);
    struct sigaction interrupt{};
    interrupt.sa_handler = forward_int;
    sigaction(SIGINT, &interrupt, nullptr);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { status = 127 << 8; break; }
    }
    child_pid = -1;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127;
    int logged = audit_line(log_fd, caller, req, req.operation == 'D' ? digest_ptrs : nullptr, code);
    close(log_fd);
    clean(stage, copies, req.count);
    if (!logged) {
        dprintf(STDERR_FILENO, "braurser-pacman-helper: rejected audit\n");
        return 64;
    }
    return code;
}
