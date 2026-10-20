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
    out->color = color;
    out->progress = progress;
    if (equal(op, "--sync")) {
        if (i < count && equal(parts[i], "--needed")) {
            ++i;
            out->operation = 'A';
            if (i < count && equal(parts[i], "--asdeps")) { out->asdeps = 1; ++i; }
            if (i < count && equal(parts[i], "--noconfirm")) ++i;
        } else if (i + 1 < count && equal(parts[i], "--refresh") && equal(parts[i + 1], "--sysupgrade")) {
            i += 2;
            out->operation = 'B';
            if (i < count && equal(parts[i], "--noconfirm")) { ++i; out->noconfirm = 1; }
            if (i < count && equal(parts[i], "--needed")) { ++i; out->needed = 1; }
            constexpr char prefix[] = "--braurser-transaction=";
            if (i >= count || strncmp(parts[i], prefix, sizeof(prefix) - 1) != 0) return 0;
            const char* digest = parts[i++] + sizeof(prefix) - 1;
            if (strlen(digest) != 64) return 0;
            for (size_t j = 0; j < 64; ++j) {
                if (!((digest[j] >= '0' && digest[j] <= '9') ||
                      (digest[j] >= 'a' && digest[j] <= 'f'))) return 0;
            }
            memcpy(out->transaction, digest, sizeof(out->transaction));
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
    if (out->operation == 'B' && i == count && !out->needed) return 1;
    if (i >= count || !equal(parts[i], "--")) return 0;
    ++i;
    if (out->operation == 'B' && !out->needed) return 0;
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

extern "C" size_t br_build_upgrade_argv(const br_request* req, const char* dbpath,
                                        const char** argv, size_t capacity) {
    if (!req || req->operation != 'B' || req->count > 512 || !br_valid_dbpath(dbpath) ||
        !argv || capacity < req->count + 11) return 0;
    size_t n = 0;
    argv[n++] = "pacman";
    argv[n++] = "--sync";
    argv[n++] = "--sysupgrade";
    argv[n++] = "--noconfirm";
    argv[n++] = "--dbpath";
    argv[n++] = dbpath;
    if (req->color) argv[n++] = "--color=never";
    if (req->progress) argv[n++] = "--noprogressbar";
    if (req->needed) argv[n++] = "--needed";
    if (req->count) argv[n++] = "--";
    for (size_t i = 0; i < req->count; ++i) argv[n++] = req->targets[i];
    argv[n] = nullptr;
    return n;
}

extern "C" size_t br_build_print_argv(const br_request* req, const char* dbpath,
                                      const char** argv, size_t capacity) {
    if (!req || req->operation != 'B' || req->count > 512 || !dbpath || !argv ||
        capacity < req->count + 13) return 0;
    size_t n = 0;
    argv[n++] = "pacman";
    argv[n++] = "--sync";
    argv[n++] = "--sysupgrade";
    argv[n++] = "--print";
    argv[n++] = "--print-format";
    argv[n++] = "%r/%n %v";
    argv[n++] = "--dbpath";
    argv[n++] = dbpath;
    argv[n++] = "--color=never";
    if (req->needed) argv[n++] = "--needed";
    if (req->count) argv[n++] = "--";
    for (size_t i = 0; i < req->count; ++i) argv[n++] = req->targets[i];
    argv[n] = nullptr;
    return n;
}

extern "C" void br_transaction_hex(const uint8_t raw[32], char hex[65]) {
    br_hex32(raw, hex);
}

extern "C" int br_transaction_equal(const char a[65], const char b[65]) {
    unsigned difference = static_cast<unsigned>(a[64]) | static_cast<unsigned>(b[64]);
    for (size_t i = 0; i < 64; ++i) {
        const unsigned char x = static_cast<unsigned char>(a[i]);
        const unsigned char y = static_cast<unsigned char>(b[i]);
        const unsigned valid_x = static_cast<unsigned>((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f'));
        const unsigned valid_y = static_cast<unsigned>((y >= '0' && y <= '9') || (y >= 'a' && y <= 'f'));
        difference |= static_cast<unsigned>(x ^ y) | (valid_x ^ 1U) | (valid_y ^ 1U);
    }
    return difference == 0;
}

extern "C" int br_valid_dbpath(const char* path) {
    if (!path || path[0] != '/') return 0;
    size_t n = strnlen(path, PATH_MAX + 1);
    if (!n || n >= PATH_MAX || has_control(path, n)) return 0;
    const char* cursor = path;
    while (*cursor) {
        while (*cursor == '/') ++cursor;
        const char* end = strchr(cursor, '/');
        if (!end) end = cursor + strlen(cursor);
        if (end - cursor == 2 && cursor[0] == '.' && cursor[1] == '.') return 0;
        cursor = end;
    }
    return 1;
}

extern "C" int br_trusted_directory(const struct stat* metadata) {
    return metadata && S_ISDIR(metadata->st_mode) && metadata->st_uid == 0 &&
           !(metadata->st_mode & 0022);
}

namespace {
int trusted_descriptor(int fd, const br_database_provider* provider) {
    struct stat metadata{};
    if (fd < 0) return -1;
    int result = provider ? provider->stat_directory(fd, &metadata, provider->context) : fstat(fd, &metadata);
    if (result || !br_trusted_directory(&metadata)) { close(fd); return -1; }
    return fd;
}
}

extern "C" int br_open_trusted_directory(int parent, const char* name, const br_database_provider* provider) {
    if (!name || (provider && !provider->stat_directory)) return -1;
    return trusted_descriptor(openat(parent, name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), provider);
}

extern "C" int br_open_database_path(const char* path, const br_database_provider* provider) {
    if (!br_valid_dbpath(path) || (provider && !provider->stat_directory)) return -1;
    int dir = trusted_descriptor(open("/", O_PATH | O_DIRECTORY | O_CLOEXEC), provider);
    if (dir < 0) return -1;
    const char* cursor = path + 1;
    while (*cursor) {
        if (*cursor == '/') { ++cursor; continue; }
        const char* end = strchr(cursor, '/');
        if (!end) end = cursor + strlen(cursor);
        size_t length = static_cast<size_t>(end - cursor);
        char name[NAME_MAX + 1]{};
        if (length > NAME_MAX) { close(dir); return -1; }
        memcpy(name, cursor, length);
        int next = br_open_trusted_directory(dir, name, provider);
        close(dir);
        if (next < 0) return -1;
        dir = next;
        cursor = end;
    }
    return dir;
}

extern "C" br_sync_action br_sync_entry_action(const char* name, mode_t mode, int staged_present) {
    if (!name || (staged_present != 0 && staged_present != 1)) return BR_SYNC_INVALID;
    size_t n = strnlen(name, NAME_MAX + 1);
    if (!n || n > NAME_MAX || strchr(name, '/')) return BR_SYNC_INVALID;
    if (equal(name, ".") || equal(name, "..")) return BR_SYNC_SKIP;
    if (!S_ISREG(mode)) return BR_SYNC_INVALID;
    size_t suffix = 0;
    if (n > 7 && equal(name + n - 7, ".db.sig")) suffix = 7;
    else if (n > 3 && equal(name + n - 3, ".db")) suffix = 3;
    if (!suffix || !br_valid_repo_name(name, n - suffix)) return BR_SYNC_COPY;
    if (staged_present) return BR_SYNC_OVERWRITE;
    return suffix == 7 ? BR_SYNC_DELETE : BR_SYNC_COPY;
}

extern "C" int br_add_archive(br_archive_batch* batch, dev_t device, ino_t inode, uint64_t bytes) {
    constexpr uint64_t limit = UINT64_C(8589934592);
    if (!batch || batch->count >= 64 || batch->total > limit || bytes > UINT64_C(4294967296) ||
        bytes > limit - batch->total) return 0;
    for (size_t i = 0; i < batch->count; ++i) {
        if (batch->devices[i] == device && batch->inodes[i] == inode) return 0;
    }
    batch->devices[batch->count] = device;
    batch->inodes[batch->count++] = inode;
    batch->total += bytes;
    return 1;
}

extern "C" int br_stage_space(uint64_t total, uint64_t blocks, uint64_t block_size) {
    constexpr uint64_t reserve = UINT64_C(1073741824);
    if (!block_size || total > UINT64_MAX - reserve) return 0;
    const uint64_t required = total + reserve;
    const uint64_t whole = required / block_size;
    return blocks >= whole && (required % block_size == 0 || blocks > whole);
}

extern "C" void br_clean_archives(char paths[64][4096], size_t staged) {
    if (staged > 64) staged = 64;
    for (size_t i = 0; i < staged; ++i) if (paths[i][0]) unlink(paths[i]);
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
