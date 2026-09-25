#include <braurser/validate/validate.hpp>
#include <gtest/gtest.h>
#include <array>
#include <string>
#include <string_view>

namespace {

using Check = bool (*)(const char*, size_t);

void check_table(Check fn, std::initializer_list<std::pair<std::string_view, bool>> table) {
    for (const auto& [text, expected] : table) {
        EXPECT_EQ(fn(text.data(), text.size()), expected) << std::string(text);
    }
}

TEST(Validate, PackageNames) {
    check_table(br_valid_pkgname, {{"a", true}, {"0", true}, {"@", true}, {"_", true}, {"+", true},
                                   {"abc.def-1+@_", true}, {"", false}, {"-a", false}, {".a", false},
                                   {"A", false}, {"aZ", false}, {"a b", false}, {"a/b", false},
                                   {"\ta", false}, {"a\nb", false}, {std::string_view("a\0b", 3), false}});
    EXPECT_TRUE(br_valid_pkgname(std::string(255, 'z').data(), 255));
    EXPECT_FALSE(br_valid_pkgname(std::string(256, 'z').data(), 256));
    EXPECT_FALSE(br_valid_pkgname(nullptr, 1));
    for (int byte = 0; byte < 256; ++byte) {
        const char c = static_cast<char>(byte);
        const bool accepted = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                              c == '@' || c == '_' || c == '+' || c == '.' || c == '-';
        const std::string text{'a', c};
        EXPECT_EQ(br_valid_pkgname(text.data(), text.size()), accepted) << byte;
        const std::string first(1, c);
        EXPECT_EQ(br_valid_pkgname(first.data(), first.size()),
                  (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '@' || c == '_' || c == '+') << byte;
    }
}

TEST(Validate, Repositories) {
    check_table(br_valid_repo_name, {{"core", true}, {"COrE.12-_", true}, {"", false},
                                     {".", false}, {"-name", false}, {"a/b", false},
                                     {"@name", false}, {std::string_view("a\0b", 3), false}, {"a b", false}});
    EXPECT_TRUE(br_valid_repo_name(std::string(64, 'X').data(), 64));
    EXPECT_FALSE(br_valid_repo_name(std::string(65, 'X').data(), 65));
    EXPECT_FALSE(br_valid_repo_name(nullptr, 1));
    for (int byte = 0; byte < 256; ++byte) {
        const char c = static_cast<char>(byte);
        const bool accepted = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        const std::string text{'a', c};
        EXPECT_EQ(br_valid_repo_name(text.data(), text.size()), accepted) << byte;
        const std::string first(1, c);
        EXPECT_EQ(br_valid_repo_name(first.data(), first.size()),
                  accepted && c != '.' && c != '-') << byte;
    }
}

TEST(Validate, SplitTargets) {
    for (const auto& [text, valid, slash] : std::initializer_list<std::tuple<std::string_view, bool, size_t>>{
             {"foo", true, 3}, {"Core/foo", true, 4}, {"a/@foo", true, 1}, {"", false, 0},
             {"/foo", false, 0}, {"repo/", false, 0}, {"foo/bar/baz", false, 0},
             {"repo/Foo", false, 0}, {".repo/foo", false, 0}, {std::string_view("repo/a\0b", 8), false, 0}}) {
        size_t actual = 999;
        EXPECT_EQ(br_split_target(text.data(), text.size(), &actual), valid) << std::string(text);
        if (valid) EXPECT_EQ(actual, slash);
        else EXPECT_EQ(actual, 999);
    }
    EXPECT_FALSE(br_split_target("a", 1, nullptr));
    EXPECT_FALSE(br_split_target(nullptr, 1, nullptr));
    const std::string largest = std::string(64, 'r') + "/" + std::string(255, 'p');
    size_t slash = 0;
    EXPECT_TRUE(br_split_target(largest.data(), largest.size(), &slash));
    EXPECT_EQ(slash, 64);
    EXPECT_FALSE(br_split_target((largest + "p").data(), largest.size() + 1, &slash));
}

TEST(Validate, PathComponents) {
    check_table(br_valid_path_component, {{"pkg", true}, {".hidden", true}, {"a b", true},
                                           {"", false}, {".", false}, {"..", false},
                                           {"a/b", false}, {std::string_view("a\0b", 3), false}, {"a\tb", false}});
    EXPECT_TRUE(br_valid_path_component(std::string(255, 'x').data(), 255));
    EXPECT_FALSE(br_valid_path_component(std::string(256, 'x').data(), 256));
    for (int byte = 0; byte < 256; ++byte) {
        const char c = static_cast<char>(byte);
        const std::string text{'x', c};
        EXPECT_EQ(br_valid_path_component(text.data(), text.size()),
                  byte >= 32 && byte != 127 && c != '/') << byte;
    }
}

TEST(Validate, Archives) {
    check_table(br_is_pkg_archive_name, {{"x.pkg.tar.zst", true}, {"x.pkg.tar.xz", true},
                                           {"x.pkg.tar.gz", true}, {"x.pkg.tar.bz2", true},
                                           {".pkg.tar.xz", false}, {"x.pkg.tar.ZST", false},
                                           {"x.pkg.tar.zst.sig", false}, {"../x.pkg.tar.xz", false},
                                           {"x/a.pkg.tar.gz", false}, {"x\0.pkg.tar.gz", false}});
    EXPECT_TRUE(br_is_pkg_archive_name((std::string(243, 'x') + ".pkg.tar.zst").data(), 255));
    EXPECT_FALSE(br_is_pkg_archive_name((std::string(244, 'x') + ".pkg.tar.zst").data(), 256));
}

std::string digest(std::string_view text, size_t chunk) {
    br_sha256_ctx ctx{};
    br_sha256_init(&ctx);
    for (size_t i = 0; i < text.size(); i += chunk) {
        const size_t length = std::min(chunk, text.size() - i);
        br_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(text.data() + i), length);
    }
    br_sha256_update(&ctx, nullptr, 0);
    uint8_t bytes[32];
    br_sha256_final(&ctx, bytes);
    char hex[65];
    br_hex32(bytes, hex);
    return hex;
}

TEST(Validate, Sha256NistVectors) {
    constexpr std::string_view long_message =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    for (const auto& [message, expected] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
             {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
             {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
             {long_message, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"}}) {
        for (const size_t chunk : {1U, 7U, 64U, 128U}) EXPECT_EQ(digest(message, chunk), expected);
    }
    const std::string million(1'000'000, 'a');
    EXPECT_EQ(digest(million, 4096), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Validate, HexComparison) {
    const std::string text = digest("abc", 1);
    std::string uppercase = text;
    for (char& c : uppercase) if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    EXPECT_TRUE(br_digest_equal_hex(text.c_str(), uppercase.c_str()));
    uppercase[1] = uppercase[1] == '0' ? '1' : '0';
    EXPECT_FALSE(br_digest_equal_hex(text.c_str(), uppercase.c_str()));
    uppercase = text;
    uppercase[1] = 'g';
    EXPECT_FALSE(br_digest_equal_hex(text.c_str(), uppercase.c_str()));
    EXPECT_FALSE(br_digest_equal_hex("", text.c_str()));
    EXPECT_FALSE(br_digest_equal_hex((text + "0").c_str(), text.c_str()));
    EXPECT_FALSE(br_digest_equal_hex(nullptr, text.c_str()));
}

}
