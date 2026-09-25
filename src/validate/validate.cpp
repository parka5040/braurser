#include <braurser/validate/validate.hpp>

namespace {

bool lower(char c) { return c >= 'a' && c <= 'z'; }
bool upper(char c) { return c >= 'A' && c <= 'Z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }

uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32U - n)); }

void transform(br_sha256_ctx* ctx, const uint8_t* block) {
    constexpr uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t w[64]{};
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = (uint32_t{block[4U * i]} << 24U) | (uint32_t{block[4U * i + 1U]} << 16U)
             | (uint32_t{block[4U * i + 2U]} << 8U) | uint32_t{block[4U * i + 3U]};
    }
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t a = w[i - 15U];
        const uint32_t b = w[i - 2U];
        w[i] = w[i - 16U] + (rotate(a, 7U) ^ rotate(a, 18U) ^ (a >> 3U)) + w[i - 7U]
             + (rotate(b, 17U) ^ rotate(b, 19U) ^ (b >> 10U));
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t s1 = rotate(e, 6U) ^ rotate(e, 11U) ^ rotate(e, 25U);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + choose + k[i] + w[i];
        const uint32_t s0 = rotate(a, 2U) ^ rotate(a, 13U) ^ rotate(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

unsigned hex_value(char c) {
    if (digit(c)) return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a') + 10U;
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A') + 10U;
    return 256U;
}

}

extern "C" {

bool br_valid_pkgname(const char* s, size_t n) {
    if (!s || n == 0 || n > 255) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!(lower(c) || digit(c) || c == '@' || c == '_' || c == '+' ||
              (i != 0 && (c == '.' || c == '-')))) return false;
    }
    return true;
}

bool br_valid_repo_name(const char* s, size_t n) {
    if (!s || n == 0 || n > 64 || s[0] == '-' || s[0] == '.') return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!(lower(c) || upper(c) || digit(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool br_split_target(const char* s, size_t n, size_t* slash_pos) {
    if (!s || !slash_pos || n == 0 || n > 320) return false;
    size_t slash = n;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '/') {
            if (slash != n) return false;
            slash = i;
        }
    }
    if (slash == n) {
        if (!br_valid_pkgname(s, n)) return false;
    } else if (!br_valid_repo_name(s, slash) || !br_valid_pkgname(s + slash + 1, n - slash - 1)) {
        return false;
    }
    *slash_pos = slash;
    return true;
}

bool br_valid_path_component(const char* s, size_t n) {
    if (!s || n == 0 || n > 255 || (n == 1 && s[0] == '.') ||
        (n == 2 && s[0] == '.' && s[1] == '.')) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '/' || c < 32 || c == 127) return false;
    }
    return true;
}

bool br_is_pkg_archive_name(const char* s, size_t n) {
    if (!br_valid_path_component(s, n)) return false;
    constexpr const char* endings[] = {".pkg.tar.zst", ".pkg.tar.xz", ".pkg.tar.gz", ".pkg.tar.bz2"};
    for (const char* ending : endings) {
        size_t len = 0;
        while (ending[len]) ++len;
        if (n <= len) continue;
        size_t i = 0;
        while (i < len && s[n - len + i] == ending[i]) ++i;
        if (i == len) return true;
    }
    return false;
}

void br_sha256_init(br_sha256_ctx* ctx) {
    constexpr uint32_t initial[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    for (unsigned i = 0; i < 8; ++i) ctx->state[i] = initial[i];
    ctx->bytes = 0;
    for (unsigned i = 0; i < 64; ++i) ctx->buffer[i] = 0;
}

void br_sha256_update(br_sha256_ctx* ctx, const uint8_t* data, size_t n) {
    size_t used = static_cast<size_t>(ctx->bytes % 64U);
    ctx->bytes += static_cast<uint64_t>(n);
    for (size_t i = 0; i < n; ++i) {
        ctx->buffer[used++] = data[i];
        if (used == 64) {
            transform(ctx, ctx->buffer);
            used = 0;
        }
    }
}

void br_sha256_final(br_sha256_ctx* ctx, uint8_t out[32]) {
    const uint64_t bits = ctx->bytes * 8U;
    uint8_t padding[128]{};
    padding[0] = 0x80U;
    const size_t used = static_cast<size_t>(ctx->bytes % 64U);
    br_sha256_update(ctx, padding, used < 56 ? 56 - used : 120 - used);
    uint8_t length[8];
    for (unsigned i = 0; i < 8; ++i) length[7U - i] = static_cast<uint8_t>(bits >> (8U * i));
    br_sha256_update(ctx, length, 8);
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            out[4U * i + j] = static_cast<uint8_t>(ctx->state[i] >> (24U - j * 8U));
        }
    }
}

void br_hex32(const uint8_t in[32], char out[65]) {
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        out[2U * i] = digits[in[i] >> 4U];
        out[2U * i + 1U] = digits[in[i] & 15U];
    }
    out[64] = '\0';
}

bool br_digest_equal_hex(const char* a, const char* b) {
    if (!a || !b) return false;
    unsigned difference = 0;
    bool a_end = false, b_end = false;
    for (unsigned i = 0; i < 64; ++i) {
        const char ac = a_end ? '\0' : a[i];
        const char bc = b_end ? '\0' : b[i];
        a_end = a_end || ac == '\0';
        b_end = b_end || bc == '\0';
        const unsigned av = hex_value(ac), bv = hex_value(bc);
        difference |= (av ^ bv) | (av >> 8U) | (bv >> 8U);
    }
    difference |= static_cast<unsigned>(!a_end && a[64] != '\0');
    difference |= static_cast<unsigned>(!b_end && b[64] != '\0');
    return difference == 0;
}

}
