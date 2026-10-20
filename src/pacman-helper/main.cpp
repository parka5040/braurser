#include "request.hpp"

#include <braurser/validate/validate.hpp>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t child_pid = -1;
volatile sig_atomic_t cancel_requested = 0;
void forward_int(int) {
    int saved_errno = errno;
    cancel_requested = 1;
    if (child_pid > 0) kill(static_cast<pid_t>(child_pid), SIGINT);
    errno = saved_errno;
}

int cancellation_pending() {
    sigset_t pending{};
    return cancel_requested || (sigpending(&pending) == 0 && sigismember(&pending, SIGINT) == 1);
}

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

int audit_line(int fd, uid_t caller, const br_request& req, const char* const* digests, int status,
               const char* resolved = nullptr, const char* reason = nullptr) {
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
    if (req.operation == 'B') {
        int n = snprintf(line + size, sizeof(line) - size, " transaction=%s%s%s%s", req.transaction,
                         status == 75 ? " stale" : "", resolved ? " resolved=" : "", resolved ? resolved : "");
        if (n < 0 || static_cast<size_t>(n) >= sizeof(line) - size) return 0;
        size += static_cast<size_t>(n);
    }
    int n = snprintf(line + size, sizeof(line) - size, " status=%d%s%s\n", status,
                     reason ? " reason=" : "", reason ? reason : "");
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

int remove_entries(int dir, int depth) {
    int scan = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* entries = scan >= 0 ? fdopendir(scan) : nullptr;
    if (!entries) {
        if (scan >= 0) close(scan);
        return 0;
    }
    int ok = 1;
    while (dirent* entry = readdir(entries)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st{};
        if (fstatat(dir, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { ok = 0; continue; }
        if (S_ISDIR(st.st_mode) && depth > 0) {
            int child = openat(dir, entry->d_name, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
            if (child < 0) { ok = 0; continue; }
            if (!remove_entries(child, depth - 1)) ok = 0;
            close(child);
            if (unlinkat(dir, entry->d_name, AT_REMOVEDIR)) ok = 0;
        } else if (S_ISDIR(st.st_mode) || unlinkat(dir, entry->d_name, 0)) ok = 0;
    }
    closedir(entries);
    return ok;
}

void remove_stage(const char* dir) {
    if (!dir[0]) return;
    int root = open(dir, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (root >= 0) {
        int db = openat(root, "db", O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        if (db >= 0) {
            int sync = openat(db, "sync", O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
            if (sync >= 0) {
                remove_entries(sync, 1);
                close(sync);
            }
            unlinkat(db, "local", 0);
            unlinkat(db, "db.lck", 0);
            unlinkat(db, "sync", AT_REMOVEDIR);
            close(db);
        }
        unlinkat(root, "db", AT_REMOVEDIR);
        close(root);
    }
    rmdir(dir);
}

void clean(const char* dir, char paths[64][4096], size_t staged) {
    br_clean_archives(paths, staged);
    remove_stage(dir);
}

int prepare_database_stage(const char* dbpath, int db, char dir[4096]) {
    int scan = openat(db, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* entries = scan >= 0 ? fdopendir(scan) : nullptr;
    if (!entries) {
        if (scan >= 0) close(scan);
        return 0;
    }
    while (dirent* entry = readdir(entries)) {
        if (strncmp(entry->d_name, ".braurser-stage-", 16)) continue;
        struct stat st{};
        char stale[4096]{};
        int n = snprintf(stale, sizeof(stale), "%s/%s", dbpath, entry->d_name);
        if (n > 0 && static_cast<size_t>(n) < sizeof(stale) &&
            !fstatat(db, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode) && st.st_uid == 0)
            remove_stage(stale);
    }
    closedir(entries);
    int n = snprintf(dir, 4096, "%s/.braurser-stage-XXXXXX", dbpath);
    if (n < 0 || n >= 4096 || !mkdtemp(dir)) {
        dir[0] = 0;
        return 0;
    }
    int fd = open(dir, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    int ok = fd >= 0 && !fchmod(fd, 0755);
    if (fd >= 0) close(fd);
    return ok;
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

int copy_archive(int input, const char* dest, char digest[65], uint64_t expected) {
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
        if (total > expected || !write_all(output, buffer, static_cast<size_t>(size))) {
            ok = 0;
            break;
        }
        br_sha256_update(&hash, reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(size));
    }
    struct stat st{};
    if (total != expected || fstat(input, &st) || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) != total || fsync(output)) ok = 0;
    close(output);
    if (!ok) return 0;
    uint8_t raw[32];
    br_sha256_final(&hash, raw);
    br_hex32(raw, digest);
    return 1;
}

int run_child(const char* program, const char* const* arguments, size_t limit = 0,
              char* output = nullptr, size_t* length = nullptr, br_sha256_ctx* hash = nullptr) {
    int pipefd[2]{-1, -1};
    if (limit && pipe2(pipefd, O_CLOEXEC)) return 127;
    sigset_t interrupts{}, previous{};
    sigemptyset(&interrupts);
    sigaddset(&interrupts, SIGINT);
    if (sigprocmask(SIG_BLOCK, &interrupts, &previous)) {
        if (limit) { close(pipefd[0]); close(pipefd[1]); }
        return 127;
    }
    if (cancellation_pending()) {
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        if (limit) { close(pipefd[0]); close(pipefd[1]); }
        return 130;
    }
    char* envp[] = {const_cast<char*>("PATH=/usr/bin"), const_cast<char*>("LC_ALL=C"), nullptr};
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction normal{};
        normal.sa_handler = SIG_DFL;
        if (sigaction(SIGTERM, &normal, nullptr) || sigaction(SIGHUP, &normal, nullptr) ||
            sigaction(SIGINT, &normal, nullptr) || sigaction(SIGPIPE, &normal, nullptr) ||
            sigprocmask(SIG_SETMASK, &previous, nullptr)) _exit(127);
        if (limit) {
            close(pipefd[0]);
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
            close(pipefd[1]);
        }
        execve(program, const_cast<char* const*>(arguments), envp);
        _exit(127);
    }
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        if (limit) { close(pipefd[0]); close(pipefd[1]); }
        return 127;
    }
    child_pid = pid;
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    int ok = 1;
    size_t total = 0;
    if (limit) {
        close(pipefd[1]);
        char buffer[65536];
        for (;;) {
            ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n == 0) break;
            if (n < 0 || static_cast<size_t>(n) > limit - total) { ok = 0; break; }
            size_t size = static_cast<size_t>(n);
            if (output) memcpy(output + total, buffer, size);
            if (hash) br_sha256_update(hash, reinterpret_cast<const uint8_t*>(buffer), size);
            total += size;
        }
        close(pipefd[0]);
        if (!ok) kill(pid, SIGKILL);
    }
    siginfo_t info{};
    while (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) < 0) {
        if (errno != EINTR) break;
    }
    sigprocmask(SIG_BLOCK, &interrupts, &previous);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { status = 127 << 8; break; }
    }
    child_pid = -1;
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    if (length) *length = total;
    if (cancel_requested) return 130;
    if (!ok) return 64;
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127;
}

int database_path(char path[4096]) {
    const char* arguments[] = {"pacman-conf", "DBPath", nullptr};
    char output[4097]{};
    size_t size = 0;
    if (run_child("/usr/bin/pacman-conf", arguments, 4096, output, &size) || !size ||
        memchr(output, 0, size)) return 0;
    if (output[size - 1] == '\n') --size;
    if (size > 1 && output[size - 1] == '/') --size;
    if (!size || size >= 4096) return 0;
    output[size] = 0;
    if (!br_valid_dbpath(output)) return 0;
    memcpy(path, output, size + 1);
    return 1;
}

int copy_database(int input, int output, uint64_t expected) {
    char buffer[65536];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(input, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return 0;
        if (!n) break;
        total += static_cast<uint64_t>(n);
        if (total > expected || !write_all(output, buffer, static_cast<size_t>(n))) return 0;
    }
    struct stat st{};
    return total == expected && !fstat(input, &st) && st.st_size >= 0 &&
           static_cast<uint64_t>(st.st_size) == total && !fsync(output);
}

int copy_databases(int source, int destination, int databases_only, int overwrite) {
    int scan = openat(source, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0) return 0;
    DIR* entries = fdopendir(scan);
    if (!entries) { close(scan); return 0; }
    uint64_t total = 0;
    const uint64_t limit = databases_only ? UINT64_C(805306368) : UINT64_C(2147483648);
    const uint64_t file_limit = databases_only ? UINT64_C(536870912) : limit;
    int ok = 1;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(entries);
        if (!entry) { if (errno) ok = 0; break; }
        const char* name = entry->d_name;
        br_sync_action action = br_sync_entry_action(name, S_IFREG, 1);
        if (action == BR_SYNC_SKIP || (databases_only && action != BR_SYNC_OVERWRITE)) continue;
        int input = openat(source, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (input < 0) { ok = 0; break; }
        struct stat st{};
        if (action == BR_SYNC_INVALID || fstat(input, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            static_cast<uint64_t>(st.st_size) > file_limit ||
            static_cast<uint64_t>(st.st_size) > limit - total) {
            close(input);
            ok = 0;
            break;
        }
        total += static_cast<uint64_t>(st.st_size);
        if (overwrite && unlinkat(destination, name, 0) && errno != ENOENT) {
            close(input);
            ok = 0;
            break;
        }
        int output = openat(destination, name,
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (output < 0) { close(input); ok = 0; break; }
        ok = copy_database(input, output, static_cast<uint64_t>(st.st_size));
        close(input);
        close(output);
        if (!ok) { unlinkat(destination, name, 0); break; }
    }
    closedir(entries);
    return ok && !fsync(destination);
}

int stage_database(const char* stage, const char* dbpath, int source, char staged_db[4096]) {
    int n = snprintf(staged_db, 4096, "%s/db", stage);
    if (n < 0 || n >= 4096 || mkdir(staged_db, 0755)) return 0;
    int db = open(staged_db, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (db < 0) return 0;
    char local[4096]{};
    n = snprintf(local, sizeof(local), "%s/local", dbpath);
    int ok = n > 0 && static_cast<size_t>(n) < sizeof(local) &&
             !symlinkat(local, db, "local") && !mkdirat(db, "sync", 0755);
    int destination = ok ? openat(db, "sync", O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC) : -1;
    close(db);
    if (destination < 0) return 0;
    ok = copy_databases(source, destination, 1, 0);
    close(destination);
    return ok;
}

int remove_sync_directory(int db) {
    struct stat st{};
    if (fstatat(db, ".braurser-sync.new", &st, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT;
    if (!br_trusted_directory(&st)) return 0;
    int dir = br_open_trusted_directory(db, ".braurser-sync.new", nullptr);
    if (dir < 0) return 0;
    int scan = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* entries = scan >= 0 ? fdopendir(scan) : nullptr;
    if (!entries) { if (scan >= 0) close(scan); close(dir); return 0; }
    int ok = 1;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(entries);
        if (!entry) { if (errno) ok = 0; break; }
        const char* name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || unlinkat(dir, name, 0)) {
            ok = 0;
            break;
        }
    }
    closedir(entries);
    close(dir);
    return ok && !unlinkat(db, ".braurser-sync.new", AT_REMOVEDIR);
}

int remove_stale_signatures(int source, int destination) {
    int scan = openat(destination, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* entries = scan >= 0 ? fdopendir(scan) : nullptr;
    if (!entries) { if (scan >= 0) close(scan); return 0; }
    int ok = 1;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(entries);
        if (!entry) { if (errno) ok = 0; break; }
        const char* name = entry->d_name;
        if (br_sync_entry_action(name, S_IFREG, 0) != BR_SYNC_DELETE) continue;
        struct stat st{};
        int present = fstatat(source, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
        if ((!present && errno != ENOENT) ||
            (present && !S_ISREG(st.st_mode)) ||
            (br_sync_entry_action(name, S_IFREG, present) == BR_SYNC_DELETE && unlinkat(destination, name, 0))) {
            ok = 0;
            break;
        }
    }
    closedir(entries);
    return ok;
}

int promote_database(const char* staged_db, int db, int live_sync) {
    if (!remove_sync_directory(db) || mkdirat(db, ".braurser-sync.new", 0755)) return 0;
    int destination = openat(db, ".braurser-sync.new", O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    int staged = open(staged_db, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    int source = staged >= 0 ? openat(staged, "sync", O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC) : -1;
    if (staged >= 0) close(staged);
    int ok = destination >= 0 && source >= 0 &&
             copy_databases(live_sync, destination, 0, 0) &&
             copy_databases(source, destination, 1, 1) &&
             remove_stale_signatures(source, destination) && !fsync(destination);
    if (source >= 0) close(source);
    if (destination >= 0) close(destination);
    sigset_t interrupts{}, previous{};
    sigemptyset(&interrupts);
    sigaddset(&interrupts, SIGINT);
    int blocked = sigprocmask(SIG_BLOCK, &interrupts, &previous) == 0;
    if (!blocked || cancellation_pending()) ok = 0;
    if (ok && renameat2(db, ".braurser-sync.new", db, "sync", RENAME_EXCHANGE)) ok = 0;
    if (blocked) sigprocmask(SIG_SETMASK, &previous, nullptr);
    int removed = remove_sync_directory(db);
    int synced = fsync(db) == 0;
    return ok && removed && synced;
}

int system_upgrade(uid_t caller, const br_request& req, int log_fd, char stage[4096]) {
    char dbpath[4096]{}, staged_db[4096]{}, resolved[65]{};
    const char* arguments[532]{};
    const char* reason = "database-stage";
    int code = 64, db = -1, local = -1, sync = -1, locked = 0;
    if (database_path(dbpath)) {
        int pinned = br_open_database_path(dbpath, nullptr);
        if (pinned >= 0) {
            db = openat(pinned, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            close(pinned);
        }
    }
    if (db >= 0) {
        int lock = openat(db, "db.lck", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0000);
        if (lock < 0) reason = errno == EEXIST ? "locked" : "database-lock";
        else {
            locked = 1;
            close(lock);
            local = br_open_trusted_directory(db, "local", nullptr);
            sync = br_open_trusted_directory(db, "sync", nullptr);
            if (local >= 0 && sync >= 0 && prepare_database_stage(dbpath, db, stage) && stage_database(stage, dbpath, sync, staged_db)) {
                const char* refresh[] = {"pacman", "--sync", "--refresh", "--dbpath", staged_db,
                                         "--noconfirm", "--color=never", "--noprogressbar", nullptr};
                code = cancellation_pending() ? 130 : run_child("/usr/bin/pacman", refresh);
                reason = code ? "refresh" : nullptr;
                if (!code) {
                    if (cancellation_pending()) code = 130;
                    else if (!br_build_print_argv(&req, staged_db, arguments, 532)) {
                        code = 64;
                        reason = "print-argv";
                    } else {
                        br_sha256_ctx hash{};
                        br_sha256_init(&hash);
                        code = run_child("/usr/bin/pacman", arguments, 4194304, nullptr, nullptr, &hash);
                        reason = code ? "print" : nullptr;
                        if (!code) {
                            uint8_t raw[32]{};
                            br_sha256_final(&hash, raw);
                            br_transaction_hex(raw, resolved);
                            if (!br_transaction_equal(req.transaction, resolved)) code = 75;
                        }
                    }
                }
                if (!code) {
                    if (cancellation_pending()) code = 130;
                    else if (!br_build_upgrade_argv(&req, staged_db, arguments, 532)) {
                        code = 64;
                        reason = "upgrade-argv";
                    } else {
                        code = run_child("/usr/bin/pacman", arguments);
                        reason = code ? "upgrade" : nullptr;
                    }
                }
                if (!code) {
                    if (cancellation_pending()) code = 130;
                    else if (!promote_database(staged_db, db, sync)) {
                        code = 64;
                        reason = "database-promote";
                    }
                }
            }
        }
    }
    if (code && code != 75 && cancellation_pending()) { code = 130; reason = "cancelled"; }
    if (locked && unlinkat(db, "db.lck", 0)) { code = 64; reason = "database-unlock"; }
    if (sync >= 0) close(sync);
    if (local >= 0) close(local);
    if (db >= 0) close(db);
    int logged = audit_line(log_fd, caller, req, nullptr, code, resolved[0] ? resolved : nullptr, reason);
    if (!logged) dprintf(STDERR_FILENO, "braurser-pacman-helper: rejected audit\n");
    else if (code == 64) dprintf(STDERR_FILENO, "braurser-pacman-helper: rejected %s\n", reason ? reason : "upgrade");
    return logged || code == 75 || code == 130 ? code : 64;
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
    size_t staged = 0;
    char hashes[64][65]{};
    const char* digest_ptrs[64]{};
    const char* arguments[532]{};
    arguments[0] = "pacman";
    for (int i = 1; i < argc; ++i) arguments[i] = argv[i];
    if (req.operation == 'D') {
        if (!prepare_stage(stage)) {
            clean(stage, copies, staged);
            return reject("staging", caller, req);
        }
        int inputs[64];
        uint64_t expected[64]{};
        for (size_t i = 0; i < 64; ++i) inputs[i] = -1;
        br_archive_batch batch{};
        int ok = req.count <= 64;
        for (size_t i = 0; ok && i < req.count; ++i) {
            inputs[i] = br_open_archive(req.targets[i], pwd.pw_dir, caller, nullptr);
            struct stat st{};
            ok = inputs[i] >= 0 && !fstat(inputs[i], &st) && st.st_size >= 0 &&
                 br_add_archive(&batch, st.st_dev, st.st_ino, static_cast<uint64_t>(st.st_size));
            if (ok) expected[i] = static_cast<uint64_t>(st.st_size);
        }
        int dir = open(stage, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        struct statvfs space{};
        ok = ok && dir >= 0 && !fstatvfs(dir, &space) &&
             br_stage_space(batch.total, static_cast<uint64_t>(space.f_bavail),
                            static_cast<uint64_t>(space.f_frsize));
        if (dir >= 0) close(dir);
        const char* failure = "archive-limits";
        for (size_t i = 0; ok && i < req.count; ++i) {
            const char* basename = strrchr(req.targets[i], '/');
            int n = snprintf(copies[i], sizeof(copies[i]), "%s/%zu-%s", stage, i, basename + 1);
            failure = "copy";
            if (n < 0 || static_cast<size_t>(n) >= sizeof(copies[i])) { ok = 0; break; }
            ++staged;
            ok = copy_archive(inputs[i], copies[i], hashes[i], expected[i]);
            close(inputs[i]);
            inputs[i] = -1;
            if (!ok) break;
            digest_ptrs[i] = hashes[i];
            for (int j = 1; j < argc; ++j) if (arguments[j] == req.targets[i]) {
                arguments[j] = copies[i];
                break;
            }
        }
        for (size_t i = 0; i < 64; ++i) if (inputs[i] >= 0) close(inputs[i]);
        if (!ok) {
            clean(stage, copies, staged);
            return reject(failure, caller, req);
        }
    }
    int log_fd = open_audit();
    if (log_fd < 0) {
        clean(stage, copies, staged);
        return reject("audit", caller, req);
    }
    if (clearenv() || setenv("PATH", "/usr/bin", 1) || setenv("LC_ALL", "C", 1)) {
        close(log_fd);
        clean(stage, copies, staged);
        return reject("environment", caller, req);
    }
    umask(022);
    struct sigaction broken_pipe{};
    broken_pipe.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &broken_pipe, nullptr)) {
        close(log_fd);
        clean(stage, copies, staged);
        return reject("signals", caller, req);
    }
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGTERM, &ignore, nullptr);
    sigaction(SIGHUP, &ignore, nullptr);
    struct sigaction interrupt{};
    interrupt.sa_handler = forward_int;
    struct sigaction normal{};
    normal.sa_handler = SIG_DFL;
    if (sigaction(SIGINT, &interrupt, nullptr) || sigaction(SIGCHLD, &normal, nullptr)) {
        close(log_fd);
        clean(stage, copies, staged);
        return reject("signals", caller, req);
    }
    if (req.operation == 'B') {
        int code = system_upgrade(caller, req, log_fd, stage);
        close(log_fd);
        clean(stage, copies, staged);
        return code;
    }
    int code = run_child("/usr/bin/pacman", arguments);
    int logged = audit_line(log_fd, caller, req, req.operation == 'D' ? digest_ptrs : nullptr, code);
    close(log_fd);
    clean(stage, copies, staged);
    if (!logged) {
        dprintf(STDERR_FILENO, "braurser-pacman-helper: rejected audit\n");
        return 64;
    }
    return code;
}
