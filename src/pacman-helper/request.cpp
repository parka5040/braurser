#include "request.hpp"

#include <braurser/validate/validate.hpp>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int equal(const char* a, const char* b) { return strcmp(a, b) == 0; }

int has_control(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 32 || c == 127) return 1;
    }
    return 0;
}

int decimal_uid(const char* s, uid_t* value) {
    if (!s || !*s || strnlen(s, 21) > 20) return 0;
    uint64_t n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return 0;
        n = n * 10 + static_cast<unsigned>(*s - '0');
        if (n > static_cast<uint64_t>(static_cast<uid_t>(-1))) return 0;
    }
    if (n == 0) return 0;
    *value = static_cast<uid_t>(n);
    return 1;
}

int directory_default(int parent, const char* name, void*) {
    return openat(parent, name, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
}
int directory_ok(int fd, uid_t caller, void*) {
    struct stat st{};
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode)) return 0;
    if (st.st_uid != 0 && st.st_uid != caller) return 0;
    if ((st.st_mode & 0022) && !((st.st_mode & S_ISVTX) && st.st_uid == 0)) return 0;
    return 1;
}
int file_default(int parent, const char* name, void*) {
    return openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
}
int file_ok(int fd, uid_t caller, void*) {
    struct stat st{};
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == caller &&
           st.st_size >= 0 && static_cast<uint64_t>(st.st_size) <= UINT64_C(4294967296);
}
const br_archive_provider default_provider{directory_default, directory_ok, file_default, file_ok, nullptr};
}

extern "C" int br_caller_uid(const char* pkexec_uid, const char* sudo_uid, uid_t* out) {
    if (!out || (!pkexec_uid && !sudo_uid)) return 0;
    uid_t pk = 0, su = 0;
    if ((pkexec_uid && !decimal_uid(pkexec_uid, &pk)) ||
        (sudo_uid && !decimal_uid(sudo_uid, &su)) ||
        (pkexec_uid && sudo_uid && pk != su)) return 0;
    *out = pkexec_uid ? pk : su;
    return 1;
}

extern "C" int br_parse_request(int argc, const char* const* argv, br_request* out) {
    if (!out || !argv || argc < 1 || argc > 530) return 0;
    *out = {};
    if (argc == 1 && argv[0] && equal(argv[0], "--auth-check")) {
        out->operation = 'F';
        return 1;
    }
    const char* parts[530]{};
    int count = 0, color = 0, progress = 0, separator = -1;
    for (int i = 0; i < argc; ++i) {
        const char* s = argv[i];
        if (!s || !*s || strnlen(s, PATH_MAX + 1) > PATH_MAX) return 0;
        if (separator < 0 && equal(s, "--color=never")) {
            if (color++) return 0;
        } else if (separator < 0 && equal(s, "--noprogressbar")) {
            if (progress++) return 0;
        } else {
            if (separator < 0 && equal(s, "--")) separator = count;
            parts[count++] = s;
        }
    }
    if (!count || !parts[0]) return 0;
    const char* op = parts[0];
    int i = 1;
    int upgrade_needs_targets = 0;
    if (equal(op, "--sync")) {
        if (i < count && equal(parts[i], "--needed")) {
            ++i;
            out->operation = 'A';
            if (i < count && equal(parts[i], "--asdeps")) { out->asdeps = 1; ++i; }
            if (i < count && equal(parts[i], "--noconfirm")) ++i;
        } else if (i + 1 < count && equal(parts[i], "--refresh") && equal(parts[i + 1], "--sysupgrade")) {
            i += 2;
            out->operation = 'B';
            if (i < count && equal(parts[i], "--noconfirm")) ++i;
            if (i < count && equal(parts[i], "--needed")) { ++i; upgrade_needs_targets = 1; }
        } else return 0;
    } else if (equal(op, "--remove")) {
        if (i + 1 >= count || !equal(parts[i], "--nosave") || !equal(parts[i + 1], "--recursive")) return 0;
        i += 2;
        out->operation = 'C';
        if (i < count && equal(parts[i], "--noconfirm")) ++i;
    } else if (equal(op, "--upgrade")) {
        out->operation = 'D';
        if (i < count && equal(parts[i], "--needed")) ++i;
        if (i < count && equal(parts[i], "--noconfirm")) ++i;
    } else if (equal(op, "--database")) {
        out->operation = 'E';
        if (i >= count || (!equal(parts[i], "--asdeps") && !equal(parts[i], "--asexplicit"))) return 0;
        out->asdeps = equal(parts[i], "--asdeps");
        ++i;
        if (i < count && equal(parts[i], "--noconfirm")) ++i;
    } else return 0;
    if (out->operation == 'B' && i == count && !upgrade_needs_targets) return 1;
    if (i >= count || !equal(parts[i], "--")) return 0;
    ++i;
    if (out->operation == 'B' && (!upgrade_needs_targets || i == count)) return 0;
    size_t max = out->operation == 'D' ? 64u : 512u;
    if (i == count || static_cast<size_t>(count - i) > max) return 0;
    for (; i < count; ++i) {
        const char* s = parts[i];
        size_t n = strlen(s);
        if (!n || s[0] == '-' || strstr(s, "://") || equal(s, "--")) return 0;
        if (out->operation == 'D') {
            if (s[0] != '/' || n > PATH_MAX || has_control(s, n)) return 0;
        } else if (out->operation == 'A' || out->operation == 'B') {
            size_t slash = 0;
            if (!br_split_target(s, n, &slash)) return 0;
        } else if (!br_valid_pkgname(s, n)) return 0;
        out->targets[out->count++] = s;
    }
    return 1;
}

extern "C" int br_open_archive(const char* path, const char* home, uid_t caller,
                                  const br_archive_provider* injection) {
    const br_archive_provider* p = injection ? injection : &default_provider;
    if (!path || !home || !p->open_directory || !p->inspect_directory ||
        !p->open_file || !p->inspect_file || caller == 0) return -1;
    size_t h = strnlen(home, PATH_MAX + 1), n = strnlen(path, PATH_MAX + 1);
    if (!h || h >= PATH_MAX || home[0] != '/' || home[h - 1] == '/' ||
        n > PATH_MAX || strncmp(home, path, h) != 0 || path[h] != '/') return -1;
    const char* suffix = path + h;
    const char* prefix = "/.cache/braurser/build/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(suffix, prefix, prefix_len) != 0) return -1;
    const char* sub = suffix + prefix_len;
    const char* slash = strchr(sub, '/');
    if (!slash || !((static_cast<size_t>(slash - sub) == 4 && strncmp(sub, "paru", 4) == 0) ||
                    (static_cast<size_t>(slash - sub) == 3 && strncmp(sub, "yay", 3) == 0) ||
                    (static_cast<size_t>(slash - sub) == 6 && strncmp(sub, "manual", 6) == 0))) return -1;
    const char* job = slash + 1;
    const char* job_end = strchr(job, '/');
    if (!job_end || !br_valid_path_component(job, static_cast<size_t>(job_end - job))) return -1;
    const char* pkg = job_end + 1;
    const char* pkg_end = strchr(pkg, '/');
    if (!pkg_end || !br_valid_path_component(pkg, static_cast<size_t>(pkg_end - pkg))) return -1;
    const char* file = pkg_end + 1;
    if (strchr(file, '/') || !br_is_pkg_archive_name(file, strlen(file))) return -1;
    int dir = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return -1;
    const char* cursor = path + 1;
    while (cursor < pkg_end + 1) {
        const char* end = strchr(cursor, '/');
        if (!end || end > pkg_end) break;
        size_t length = static_cast<size_t>(end - cursor);
        char name[256];
        if (!br_valid_path_component(cursor, length)) { close(dir); return -1; }
        memcpy(name, cursor, length);
        name[length] = 0;
        int next = p->open_directory(dir, name, p->context);
        close(dir);
        if (next < 0) return -1;
        if (!p->inspect_directory(next, caller, p->context)) { close(next); return -1; }
        dir = next;
        cursor = end + 1;
    }
    if (cursor != file) { close(dir); return -1; }
    int fd = p->open_file(dir, file, p->context);
    close(dir);
    if (fd < 0) return -1;
    if (!p->inspect_file(fd, caller, p->context)) { close(fd); return -1; }
    return fd;
}

extern "C" int br_validate_archive_path(const char* path, const char* home, uid_t caller,
                                           const br_archive_provider* provider) {
    int fd = br_open_archive(path, home, caller, provider);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}
