#ifndef BRAURSER_PACMAN_HELPER_REQUEST_HPP
#define BRAURSER_PACMAN_HELPER_REQUEST_HPP

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct br_request {
    char operation;
    size_t count;
    const char* targets[512];
    int asdeps;
    int needed;
    int noconfirm;
    int color;
    int progress;
    char transaction[65];
} br_request;

typedef struct br_archive_batch {
    dev_t devices[64];
    ino_t inodes[64];
    size_t count;
    uint64_t total;
} br_archive_batch;

typedef struct br_archive_provider {
    int (*open_directory)(int parent, const char* name, void* context);
    int (*inspect_directory)(int fd, uid_t caller, void* context);
    int (*open_file)(int parent, const char* name, void* context);
    int (*inspect_file)(int fd, uid_t caller, void* context);
    void* context;
} br_archive_provider;

typedef struct br_database_provider {
    int (*stat_directory)(int fd, struct stat* metadata, void* context);
    void* context;
} br_database_provider;

typedef enum br_sync_action {
    BR_SYNC_INVALID = -1,
    BR_SYNC_SKIP = 0,
    BR_SYNC_COPY = 1,
    BR_SYNC_OVERWRITE = 2,
    BR_SYNC_DELETE = 3
} br_sync_action;

#ifdef __cplusplus
extern "C" {
#endif
int br_trusted_directory(const struct stat* metadata);
int br_open_trusted_directory(int parent, const char* name, const br_database_provider* provider);
int br_open_database_path(const char* path, const br_database_provider* provider);
br_sync_action br_sync_entry_action(const char* name, mode_t mode, int staged_present);
/// out: true only for a canonical allowed request; out borrows target strings from argv.
int br_parse_request(int argc, const char* const* argv, br_request* out);
/// out: argv holds the final shape B command for dbpath, without refresh or digest; returns argc or zero.
size_t br_build_upgrade_argv(const br_request* req, const char* dbpath, const char** argv, size_t capacity);
/// out: argv holds the shape B print command for dbpath; returns argc or zero.
size_t br_build_print_argv(const br_request* req, const char* dbpath, const char** argv, size_t capacity);
/// in: raw contains 32 bytes; hex has space for 65 bytes.
/// out: hex contains lowercase hexadecimal and a terminator.
void br_transaction_hex(const uint8_t raw[32], char hex[65]);
/// in: a and b each contain 65 readable bytes.
/// out: true only for identical lowercase 64-digit digests; all 64 digits are compared.
int br_transaction_equal(const char a[65], const char b[65]);
/// out: true only for a bounded absolute database path without parent components or controls.
int br_valid_dbpath(const char* path);
/// out: adds a unique source within the 64-file, 8 GiB aggregate limit or returns zero.
int br_add_archive(br_archive_batch* batch, dev_t device, ino_t inode, uint64_t bytes);
/// out: true only when available blocks cover total bytes plus a 1 GiB reserve without overflow.
int br_stage_space(uint64_t total, uint64_t blocks, uint64_t block_size);
/// in: paths has 64 entries; staged counts successfully formatted staging paths.
/// out: unlinks only the staged paths, bounded to the array capacity.
void br_clean_archives(char paths[64][4096], size_t staged);
/// out: true only for an unambiguous non-root caller uid.
int br_caller_uid(const char* pkexec_uid, const char* sudo_uid, uid_t* out);
/// out: true only for a caller-owned archive in the caller's private build tree.
int br_validate_archive_path(const char* path, const char* home, uid_t caller,
                             const br_archive_provider* provider);
/// out: an open validated archive descriptor, or -1 on rejection.
int br_open_archive(const char* path, const char* home, uid_t caller,
                    const br_archive_provider* provider);
#ifdef __cplusplus
}
#endif

#endif
