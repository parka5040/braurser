#ifndef BRAURSER_VALIDATE_VALIDATE_HPP
#define BRAURSER_VALIDATE_VALIDATE_HPP

#include <stddef.h>
#include <stdint.h>

struct br_sha256_ctx {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t buffer[64];
};

extern "C" {

/// in: s points to n bytes; n is at most 255.
/// out: true only for a valid package name.
bool br_valid_pkgname(const char* s, size_t n);

/// in: s points to n bytes; n is at most 64.
/// out: true only for a valid repository name.
bool br_valid_repo_name(const char* s, size_t n);

/// in: s points to n bytes; slash_pos is writable.
/// out: true for a valid name or repo/name; slash_pos is n when unqualified.
bool br_split_target(const char* s, size_t n, size_t* slash_pos);

/// in: s points to n bytes; n is at most 255.
/// out: true only for a safe single path component.
bool br_valid_path_component(const char* s, size_t n);

/// in: s points to n bytes; n is at most 255.
/// out: true only for a supported package archive filename.
bool br_is_pkg_archive_name(const char* s, size_t n);

/// in: ctx is writable.
/// out: ctx is initialized for SHA-256.
void br_sha256_init(br_sha256_ctx* ctx);

/// in: ctx is initialized; data points to n bytes.
/// out: ctx incorporates the bytes.
void br_sha256_update(br_sha256_ctx* ctx, const uint8_t* data, size_t n);

/// in: ctx is initialized; out has room for 32 bytes.
/// out: out contains the SHA-256 digest.
void br_sha256_final(br_sha256_ctx* ctx, uint8_t out[32]);

/// in: in contains 32 bytes; out has room for 65 bytes.
/// out: out contains lowercase hexadecimal and a terminator.
void br_hex32(const uint8_t in[32], char out[65]);

/// in: a and b point to readable NUL-terminated strings of at most 64 hex digits.
/// out: true only if both are valid 64-digit hex digests with equal values.
bool br_digest_equal_hex(const char* a, const char* b);

}

#endif
