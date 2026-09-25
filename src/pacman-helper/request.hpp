#ifndef BRAURSER_PACMAN_HELPER_REQUEST_HPP
#define BRAURSER_PACMAN_HELPER_REQUEST_HPP

#include <stddef.h>
#include <sys/types.h>

typedef struct br_request {
    char operation;
    size_t count;
    const char* targets[512];
    int asdeps;
} br_request;

typedef struct br_archive_provider {
    int (*open_directory)(int parent, const char* name, void* context);
    int (*inspect_directory)(int fd, uid_t caller, void* context);
    int (*open_file)(int parent, const char* name, void* context);
    int (*inspect_file)(int fd, uid_t caller, void* context);
    void* context;
} br_archive_provider;

#ifdef __cplusplus
extern "C" {
#endif
int br_parse_request(int argc, const char* const* argv, br_request* out);
int br_caller_uid(const char* pkexec_uid, const char* sudo_uid, uid_t* out);
int br_validate_archive_path(const char* path, const char* home, uid_t caller,
                             const br_archive_provider* provider);
int br_open_archive(const char* path, const char* home, uid_t caller,
                    const br_archive_provider* provider);
#ifdef __cplusplus
}
#endif

#endif
