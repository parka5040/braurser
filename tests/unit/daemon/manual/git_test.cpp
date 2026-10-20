#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/process.hpp>

#include <git2.h>
#include <gtest/gtest.h>
#include <cstdint>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <tuple>
#include <unistd.h>

namespace {
using namespace braurser::daemon::manual;

class Temp {
public:
    Temp() {
        auto pattern = (std::filesystem::temp_directory_path() / "braurser-git-XXXXXX").string();
        auto* dir = mkdtemp(pattern.data());
        if (dir) path = dir;
    }
    ~Temp() { if (!path.empty()) std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

class FakeHttp final : public GitHttp {
public:
    GitReply reply;
    std::string requested;
    std::expected<GitReply, ManualError> request(std::string_view url, std::string_view, bool) override {
        requested = url;
        return std::move(reply);
    }
};

class FakeUploadPack final : public GitHttp {
public:
    explicit FakeUploadPack(std::filesystem::path repo) : repo_(std::move(repo)) {}
    std::size_t largest_response{};
    std::expected<GitReply, ManualError> request(std::string_view, std::string_view body, bool post) override {
        braurser::daemon::core::ProcessSpec spec;
        spec.executable = "/usr/bin/git-upload-pack";
        spec.args = {"--stateless-rpc"};
        if (!post) spec.args.push_back("--advertise-refs");
        spec.args.push_back(repo_.string());
        spec.env = {{"PATH", "/usr/bin"}, {"HOME", repo_.parent_path().string()}};
        spec.stdin_data = std::string(body);
        spec.max_stdout = 64 * 1024 * 1024;
        spec.max_stderr = 4096;
        auto result = runner_.run(spec);
        if (!result || result->exit_code != 0)
            return std::unexpected(ManualError{"fixture-upload-pack", result ? result->err : "spawn", {}, {}});
        auto spool = GitReply::Body::create(repo_.parent_path());
        if (!spool) return std::unexpected(spool.error());
        auto stored = spool->append(post ? result->out : "001e# service=git-upload-pack\n0000" + result->out);
        if (!stored) return std::unexpected(stored.error());
        largest_response = std::max(largest_response, spool->size());
        return GitReply{200, std::move(*spool), {}};
    }
private:
    std::filesystem::path repo_;
    braurser::daemon::core::PosixProcessRunner runner_;
};

std::string build_repo(const std::filesystem::path& path, std::string_view script,
                       std::string_view makefile = {}, std::string_view link = {}, bool gitlink = false,
                       std::string_view pkgbuild = {}, std::size_t files = 0) {
    if (git_libgit2_init() < 0) return {};
    git_repository* raw = nullptr;
    if (git_repository_open_bare(&raw, path.c_str()) != 0 &&
        git_repository_init(&raw, path.c_str(), 1) != 0) return {};
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw, git_repository_free);
    git_treebuilder* builder = nullptr;
    if (git_treebuilder_new(&builder, repo.get(), nullptr) != 0) return {};
    std::unique_ptr<git_treebuilder, decltype(&git_treebuilder_free)> tree(builder, git_treebuilder_free);
    auto insert = [&](const char* name, std::string_view bytes, git_filemode_t mode) {
        git_oid oid{};
        if (git_blob_create_frombuffer(&oid, repo.get(), bytes.data(), bytes.size()) != 0) return -1;
        return git_treebuilder_insert(nullptr, tree.get(), name, &oid, mode);
    };
    for (std::size_t index = 0; index < files; ++index) {
        const auto name = "file-" + std::to_string(index);
        if (insert(name.c_str(), "data", GIT_FILEMODE_BLOB) != 0) return {};
    }
    if (!script.empty() && insert("install.sh", script, GIT_FILEMODE_BLOB) != 0) return {};
    if (!makefile.empty() && insert("Makefile", makefile, GIT_FILEMODE_BLOB) != 0) return {};
    if (!pkgbuild.empty() && insert("PKGBUILD", pkgbuild, GIT_FILEMODE_BLOB) != 0) return {};
    if (!link.empty() && insert("link", link, GIT_FILEMODE_LINK) != 0) return {};
    if (gitlink) {
        git_oid oid{};
        if (git_oid_fromstr(&oid, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") != 0 ||
            git_treebuilder_insert(nullptr, tree.get(), "vendor", &oid, GIT_FILEMODE_COMMIT) != 0) return {};
    }
    git_oid tree_oid{};
    if (git_treebuilder_write(&tree_oid, tree.get()) != 0) return {};
    git_tree* raw_tree = nullptr;
    if (git_tree_lookup(&raw_tree, repo.get(), &tree_oid) != 0) return {};
    std::unique_ptr<git_tree, decltype(&git_tree_free)> tree_object(raw_tree, git_tree_free);
    git_signature* raw_signature = nullptr;
    if (git_signature_new(&raw_signature, "Test", "test@example.org", 0, 0) != 0) return {};
    std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(raw_signature, git_signature_free);
    git_oid commit{};
    git_oid parent_oid{};
    git_commit* raw_parent = nullptr;
    if (git_reference_name_to_id(&parent_oid, repo.get(), "HEAD") == 0 &&
        git_commit_lookup(&raw_parent, repo.get(), &parent_oid) != 0) return {};
    std::unique_ptr<git_commit, decltype(&git_commit_free)> parent(raw_parent, git_commit_free);
    const git_commit* parents[]{parent.get()};
    if (git_commit_create(&commit, repo.get(), "HEAD", signature.get(), signature.get(),
                          nullptr, "test", tree_object.get(), parent ? 1U : 0U, parents) != 0) return {};
    char hex[GIT_OID_HEXSZ + 1]{};
    git_oid_tostr(hex, sizeof(hex), &commit);
    return hex;
}

TEST(GitSpool, ReadsExactBytesAcrossChunkBoundaries) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto spool = GitReply::Body::create(temp.path);
    ASSERT_TRUE(spool) << spool.error().code;
    std::string bytes(3 * GitReply::Body::kReadChunk + 17, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<char>(index % 251);
    ASSERT_TRUE(spool->append(std::string_view(bytes).substr(0, 65535)));
    ASSERT_TRUE(spool->append(std::string_view(bytes).substr(65535)));
    FakeHttp http;
    http.reply = {200, std::move(*spool), {}};
    GuardedGitTransport transport(http);
    auto reader = transport.exchange("https://example.org/repo");
    ASSERT_TRUE(reader) << reader.error().code;
    EXPECT_EQ(reader->size(), bytes.size());
    std::array<char, 2 * GitReply::Body::kReadChunk> buffer{};
    std::string received;
    while (received.size() < reader->size()) {
        auto count = reader->read(buffer, received.size());
        ASSERT_TRUE(count) << count.error().code;
        ASSERT_GT(*count, 0U);
        EXPECT_LE(*count, GitReply::Body::kReadChunk);
        received.append(buffer.data(), *count);
    }
    EXPECT_EQ(received, bytes);
    auto eof = reader->read(buffer, received.size());
    ASSERT_TRUE(eof);
    EXPECT_EQ(*eof, 0U);
    auto crossing = reader->read(std::span<char>(buffer).first(9), 65533);
    ASSERT_TRUE(crossing);
    EXPECT_EQ(std::string_view(buffer.data(), *crossing), std::string_view(bytes).substr(65533, 9));
    EXPECT_FALSE(reader->read(buffer, bytes.size() + 1));
    EXPECT_TRUE(std::filesystem::is_empty(temp.path / "braurser/manual/spool"));
}

TEST(GitSpool, IsPrivateAnonymousCloseOnExecAndMoveOwned) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    int descriptor = -1;
    {
        auto spool = GitReply::Body::create(temp.path);
        ASSERT_TRUE(spool);
        descriptor = spool->native_handle();
        struct stat file_info{};
        ASSERT_EQ(fstat(descriptor, &file_info), 0);
        EXPECT_EQ(file_info.st_nlink, 0U);
        EXPECT_EQ(file_info.st_mode & 0777U, 0600U);
        EXPECT_EQ(fcntl(descriptor, F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
        struct stat directory_info{};
        ASSERT_EQ(stat((temp.path / "braurser/manual/spool").c_str(), &directory_info), 0);
        EXPECT_EQ(directory_info.st_mode & 0777U, 0700U);
        ASSERT_TRUE(spool->append("response"));
        GitReply::Body moved(std::move(*spool));
        EXPECT_EQ(spool->native_handle(), -1);
        EXPECT_EQ(spool->size(), 0U);
        auto other = GitReply::Body::create(temp.path);
        ASSERT_TRUE(other);
        const auto previous = other->native_handle();
        *other = std::move(moved);
        EXPECT_EQ(fcntl(previous, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
        EXPECT_EQ(other->native_handle(), descriptor);
        EXPECT_EQ(other->size(), 8U);
    }
    EXPECT_EQ(fcntl(descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_TRUE(std::filesystem::is_empty(temp.path / "braurser/manual/spool"));
}

TEST(GitSpool, RejectsOversizedResponseAndClosesSpool) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto spool = GitReply::Body::create(temp.path, GitReply::Body::kReadChunk + 1);
    ASSERT_TRUE(spool);
    const auto descriptor = spool->native_handle();
    ASSERT_TRUE(spool->append(std::string(GitReply::Body::kReadChunk, 'x')));
    ASSERT_TRUE(spool->append("x"));
    auto overflow = spool->append("x");
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, "git-size");
    EXPECT_EQ(spool->native_handle(), -1);
    EXPECT_EQ(fcntl(descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_TRUE(std::filesystem::is_empty(temp.path / "braurser/manual/spool"));
}

TEST(GitSpool, RejectsTruncatedResponseAndUnsafeDirectory) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto spool = GitReply::Body::create(temp.path);
    ASSERT_TRUE(spool);
    ASSERT_TRUE(spool->append("response"));
    ASSERT_EQ(ftruncate(spool->native_handle(), 2), 0);
    std::array<char, 8> buffer{};
    auto read = spool->read(buffer, 0);
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, "git-spool-read");
    std::filesystem::create_directory_symlink(temp.path, temp.path / "braurser/manual/unsafe");
    EXPECT_FALSE(GitReply::Body::create(temp.path / "braurser/manual/unsafe"));
    EXPECT_FALSE(GitReply::Body::create("relative"));
}

TEST(GitSpool, EnforcesInjectedHttpAggregateResponseBudget) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto spool = GitReply::Body::create(temp.path);
    ASSERT_TRUE(spool);
    ASSERT_TRUE(spool->append("response"));
    const auto descriptor = spool->native_handle();
    FakeHttp http;
    http.reply = {200, std::move(*spool), {}};
    auto result = http.request_bounded("https://example.org/repo", {}, false, 7);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "git-size");
    EXPECT_EQ(fcntl(descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_TRUE(std::filesystem::is_empty(temp.path / "braurser/manual/spool"));
}

TEST(GitFetch, StreamsLargePackThroughInjectedSmartHttp) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    std::string script(240 * 1024, ' ');
    std::uint32_t state = 1;
    for (auto& ch : script) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        ch = static_cast<char>(32U + state % 95U);
    }
    const auto commit = build_repo(temp.path / "source.git", script);
    ASSERT_EQ(commit.size(), 40U);
    FakeUploadPack http(temp.path / "source.git");
    auto result = fetch_preview(http, "https://example.org/repo", temp.path / "cache", "large-pack");
    ASSERT_TRUE(result) << result.error().code << " " << result.error().detail;
    EXPECT_GT(http.largest_response, GitReply::Body::kReadChunk);
    EXPECT_EQ(result->commit, commit);
    EXPECT_EQ(result->selected_script_text, script);
    EXPECT_TRUE(std::filesystem::is_empty(temp.path / "braurser/manual/spool"));
}

TEST(GitPreview, LargeTokenAndTreeSetsKeepSortedUniqueReferences) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    std::string script;
    for (std::size_t index = 0; index < 100000; ++index) script += "x ";
    script += "./file-9999 file-0 /file-9999 'install.sh' file-0 missing\n";
    const auto repo = temp.path / "repo.git";
    ASSERT_FALSE(build_repo(repo, script, {}, {}, false, {}, 10000).empty());
    auto url = validate_git_url("https://example.org/owner/repo");
    ASSERT_TRUE(url);
    auto preview = preview_repository(repo, "HEAD", *url, "large-preview");
    ASSERT_TRUE(preview) << preview.error().code;
    EXPECT_EQ(preview->referenced_files, (std::vector<std::string>{"file-0", "file-9999", "install.sh"}));
}

TEST(GitUrl, RejectsUnsafeTargets) {
    for (auto text : {"http://example.org/repo", "https://a:b@example.org/repo",
                      "https://example.org:444/repo", "https://127.0.0.1/repo",
                      "https://10.0.0.1/repo", "https://172.16.0.1/repo",
                      "https://192.168.0.1/repo", "https://169.254.1.1/repo",
                      "https://100.64.0.1/repo", "https://224.0.0.1/repo",
                      "https://[::1]/repo", "https://[fd00::1]/repo",
                      "https://[fe80::1]/repo", "https://[ff02::1]/repo",
                      "https://[2001:db8::1]/repo", "https://[::ffff:10.1.2.3]/repo",
                      "https://0.0.0.0/repo", "https://198.51.100.1/repo",
                      "https://example.org/a/../repo",
                      "https://example.org/%2e%2e/repo", "https://example.org/repo?x=1"}) {
        EXPECT_FALSE(validate_git_url(text).has_value()) << text;
    }
    std::string nul_url = "https://example.org/repo";
    nul_url.push_back('\0');
    nul_url += ".git";
    EXPECT_FALSE(validate_git_url(nul_url).has_value());
    auto url = validate_git_url("https://example.org/owner/app.git");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->owner_repo, "owner/app");
}

TEST(GitGuard, RefusesPrivateConnectionsBeforeTls) {
    CurlGitHttp curl;
    GuardedGitTransport transport(curl);
    EXPECT_EQ(transport.exchange("https://127.0.0.1:443/repo").error().code, "git-address");
    EXPECT_EQ(transport.exchange("https://[::1]/repo").error().code, "git-address");
}

TEST(GitGuard, BlocksLocalhostAfterDnsBeforeTls) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    CurlGitHttp http(temp.path);
    GuardedGitTransport transport(http);
    auto result = transport.exchange("https://localhost/repo");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "git-connect");
    EXPECT_TRUE(std::filesystem::is_empty(temp.path / "braurser/manual/spool"));
}

TEST(GitRedirect, RequiresExplicitCrossHostApproval) {
    FakeHttp http;
    http.reply = {302, {}, "https://other.example/repo/info/refs?service=git-upload-pack"};
    GuardedGitTransport transport(http);
    auto result = transport.exchange("https://example.org/repo");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "RedirectRequired");
    EXPECT_EQ(result.error().from_host, "example.org");
    EXPECT_EQ(result.error().to_host, "other.example");
}

TEST(GitRedirect, FetchSurfacesGuardedSmartHttpRedirect) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    FakeHttp http;
    http.reply = {302, {}, "https://other.example/repo/info/refs?service=git-upload-pack"};
    auto fetched = fetch_preview(http, "https://example.org/repo", temp.path, "redirect-test");
    ASSERT_FALSE(fetched.has_value());
    EXPECT_EQ(fetched.error().code, "RedirectRequired");
    EXPECT_EQ(fetched.error().to_host, "other.example");
    EXPECT_EQ(http.requested, "https://example.org/repo/info/refs?service=git-upload-pack");
}

TEST(GitFetch, FetchesThroughInjectedSmartHttpWithoutNetwork) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto commit = build_repo(temp.path / "source.git", "printf test > \"$PREFIX/bin/demo\"\n");
    ASSERT_EQ(commit.size(), 40U);
    FakeUploadPack http(temp.path / "source.git");
    auto result = fetch_preview(http, "https://example.org/demo.git", temp.path / "cache", "fetch-test");
    ASSERT_TRUE(result.has_value()) << result.error().code << " " << result.error().detail;
    EXPECT_EQ(result->commit, commit);
    EXPECT_EQ(result->selected_script_text, "printf test > \"$PREFIX/bin/demo\"\n");
}

TEST(GitFetch, PinsAndValidatesHelperBuildFileWithoutNetwork) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    constexpr std::string_view build = "pkgname=paru\npkgver=1\n";
    const auto commit = build_repo(temp.path / "helper.git", {}, {}, {}, false, build);
    ASSERT_EQ(commit.size(), 40U);
    FakeUploadPack http(temp.path / "helper.git");
    auto checked = fetch_build_repository(http, "https://aur.archlinux.org/paru.git",
                                          temp.path / "cache", "setup-paru");
    ASSERT_TRUE(checked) << checked.error().code << " " << checked.error().detail;
    EXPECT_EQ(checked->commit, commit);
    EXPECT_EQ(checked->pkgbuild, build);
    const std::string invalid(1, static_cast<char>(0xff));
    ASSERT_EQ(build_repo(temp.path / "helper.git", {}, {}, {}, false, invalid).size(), 40U);
    auto refused = fetch_build_repository(http, "https://aur.archlinux.org/paru.git",
                                          temp.path / "cache", "setup-paru");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "build-file");
}

TEST(GitUpdate, DiffsChangedEntryPointWithoutNetwork) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    const auto original = build_repo(temp.path / "source.git", "printf old > \"$PREFIX/bin/demo\"\n");
    ASSERT_EQ(original.size(), 40U);
    FakeUploadPack http(temp.path / "source.git");
    auto cached = fetch_preview(http, "https://example.org/demo.git", temp.path / "cache", "update-test");
    ASSERT_TRUE(cached.has_value()) << cached.error().code;
    const auto next = build_repo(temp.path / "source.git", "printf new > \"$PREFIX/bin/demo\"\n");
    ASSERT_EQ(next.size(), 40U);
    Receipt receipt;
    receipt.id = "update-test";
    receipt.url = "https://example.org/demo.git";
    receipt.commit = original;
    receipt.entry_point = "install.sh";
    auto update = check_update(http, receipt, temp.path / "cache");
    ASSERT_TRUE(update.has_value()) << update.error().code << " " << update.error().detail;
    ASSERT_TRUE(update->preview.has_value());
    EXPECT_EQ(update->preview->commit, next);
    EXPECT_NE(update->entry_point_diff.find("+printf new"), std::string::npos);
}

TEST(GitPreview, DetectsBothEntryPointsAndReadsScript) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    const auto commit = build_repo(temp.path / "repo.git", "install -m755 demo $PREFIX/bin/demo\n",
                                   "install:\n\ttrue\n");
    ASSERT_EQ(commit.size(), 40U);
    auto url = validate_git_url("https://example.org/demo.git");
    ASSERT_TRUE(url.has_value());
    auto undecided = preview_repository(temp.path / "repo.git", "HEAD", *url, "preview-test");
    ASSERT_TRUE(undecided.has_value());
    EXPECT_EQ(undecided->entry_points.size(), 2U);
    EXPECT_TRUE(undecided->selected_script_text.empty());
    auto selected = preview_repository(temp.path / "repo.git", "HEAD", *url,
                                       "preview-test", EntryKind::Script);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->commit, commit);
    EXPECT_EQ(selected->selected_script_text, "install -m755 demo $PREFIX/bin/demo\n");
    std::filesystem::create_directory(temp.path / "src");
    auto materialized = materialize_commit(temp.path / "repo.git", commit, temp.path / "src");
    ASSERT_TRUE(materialized.has_value()) << materialized.error().code;
    EXPECT_TRUE(std::filesystem::is_regular_file(temp.path / "src/install.sh"));
}

TEST(GitPreview, DetectsMakefileOnlyAndRejectsUnsupportedTree) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto url = validate_git_url("https://example.org/repo.git");
    ASSERT_TRUE(url.has_value());
    auto make_commit = build_repo(temp.path / "make.git", {}, "build:\n\ttrue\ninstall:\n\ttrue\n");
    ASSERT_EQ(make_commit.size(), 40U);
    auto make_preview = preview_repository(temp.path / "make.git", "HEAD", *url, "make-test");
    ASSERT_TRUE(make_preview.has_value()) << make_preview.error().code;
    ASSERT_EQ(make_preview->entry_points.size(), 1U);
    EXPECT_EQ(make_preview->entry_points.front().kind, EntryKind::MakeInstall);
    EXPECT_EQ(make_preview->selected_script_text, "build:\n\ttrue\ninstall:\n\ttrue\n");
    auto none_commit = build_repo(temp.path / "none.git", {}, "build:\n\ttrue\n");
    ASSERT_EQ(none_commit.size(), 40U);
    auto none = preview_repository(temp.path / "none.git", "HEAD", *url, "none-test");
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code, "Unsupported");
}

using RawEntry = std::tuple<std::string, std::string, git_oid>;

std::string commit_raw_tree(git_repository* repo, const std::vector<RawEntry>& entries) {
    git_odb* raw_odb = nullptr;
    if (git_repository_odb(&raw_odb, repo) != 0) return {};
    std::unique_ptr<git_odb, decltype(&git_odb_free)> odb(raw_odb, git_odb_free);
    std::string data;
    for (const auto& [mode, name, oid] : entries) {
        data += mode + " " + name;
        data.push_back('\0');
        data.append(reinterpret_cast<const char*>(oid.id), GIT_OID_SHA1_SIZE);
    }
    git_oid tree_oid{};
    if (git_odb_write(&tree_oid, odb.get(), data.data(), data.size(), GIT_OBJECT_TREE) != 0) return {};
    git_tree* raw_tree = nullptr;
    if (git_tree_lookup(&raw_tree, repo, &tree_oid) != 0) return {};
    std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
    git_signature* raw_signature = nullptr;
    if (git_signature_new(&raw_signature, "Test", "test@example.org", 0, 0) != 0) return {};
    std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(raw_signature, git_signature_free);
    git_oid commit{};
    if (git_commit_create(&commit, repo, "HEAD", signature.get(), signature.get(), nullptr, "test",
                          tree.get(), 0, nullptr) != 0) return {};
    char hex[GIT_OID_HEXSZ + 1]{};
    git_oid_tostr(hex, sizeof(hex), &commit);
    return hex;
}

std::string stored_zlib(std::string_view data) {
    std::string out{"\x78\x01\x01", 3};
    const auto length = static_cast<std::uint16_t>(data.size());
    const auto inverse = static_cast<std::uint16_t>(~length);
    for (const auto value : {length, inverse}) {
        out.push_back(static_cast<char>(value & 0xffU));
        out.push_back(static_cast<char>(value >> 8U));
    }
    out.append(data);
    std::uint32_t a = 1, b = 0;
    for (const char ch : data) {
        a = (a + static_cast<unsigned char>(ch)) % 65521U;
        b = (b + a) % 65521U;
    }
    const std::uint32_t adler = (b << 16U) | a;
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((adler >> static_cast<unsigned>(shift)) & 0xffU));
    return out;
}

using Repository = std::unique_ptr<git_repository, decltype(&git_repository_free)>;

Repository init_bare(const std::filesystem::path& path) {
    git_repository* raw = nullptr;
    if (git_libgit2_init() < 0 || git_repository_init(&raw, path.c_str(), 1) != 0) raw = nullptr;
    return Repository(raw, git_repository_free);
}

git_oid blob(git_repository* repo, std::string_view bytes) {
    git_oid oid{};
    EXPECT_EQ(git_blob_create_frombuffer(&oid, repo, bytes.data(), bytes.size()), 0);
    return oid;
}

TEST(GitTree, RejectsDotGitEntriesInAnyCase) {
    for (auto name : {".git", ".GiT"}) {
        Temp temp;
        ASSERT_FALSE(temp.path.empty());
        auto repo = init_bare(temp.path / "repo.git");
        ASSERT_TRUE(repo);
        const auto config = blob(repo.get(), "[core]\n\tfsmonitor = touch /tmp/owned\n");
        const auto script = blob(repo.get(), "true\n");
        const auto commit = commit_raw_tree(repo.get(), {{"100644", name, config}, {"100644", "install.sh", script}});
        ASSERT_EQ(commit.size(), 40U) << name;
        std::filesystem::create_directory(temp.path / "src");
        auto result = materialize_commit(temp.path / "repo.git", commit, temp.path / "src");
        ASSERT_FALSE(result.has_value()) << name;
        EXPECT_EQ(result.error().code, "tree-path");
        EXPECT_TRUE(std::filesystem::is_empty(temp.path / "src"));
        auto url = validate_git_url("https://example.org/demo.git");
        ASSERT_TRUE(url.has_value());
        auto preview = preview_repository(temp.path / "repo.git", "HEAD", *url, "dotgit-test");
        ASSERT_FALSE(preview.has_value());
        EXPECT_EQ(preview.error().code, "tree-path");
    }
}

TEST(GitTree, ChecksDeclaredBlobSizeBeforeInflating) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto repo = init_bare(temp.path / "repo.git");
    ASSERT_TRUE(repo);
    constexpr std::string_view hex = "0123456789abcdef0123456789abcdef01234567";
    const auto object = temp.path / "repo.git/objects" / std::string(hex.substr(0, 2)) / std::string(hex.substr(2));
    std::filesystem::create_directories(object.parent_path());
    const std::string header("blob 1099511627776\0abc", 22);
    std::ofstream(object, std::ios::binary) << stored_zlib(header);
    git_oid huge{};
    ASSERT_EQ(git_oid_fromstr(&huge, std::string(hex).c_str()), 0);
    const auto script = blob(repo.get(), "true\n");
    const auto commit = commit_raw_tree(repo.get(), {{"100644", "huge.bin", huge}, {"100644", "install.sh", script}});
    ASSERT_EQ(commit.size(), 40U);
    auto url = validate_git_url("https://example.org/demo.git");
    ASSERT_TRUE(url.has_value());
    auto preview = preview_repository(temp.path / "repo.git", "HEAD", *url, "bomb-test", EntryKind::Script);
    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, "tree-size");
    std::filesystem::create_directory(temp.path / "src");
    auto materialized = materialize_commit(temp.path / "repo.git", commit, temp.path / "src");
    ASSERT_FALSE(materialized.has_value());
    EXPECT_EQ(materialized.error().code, "tree-size");
}

TEST(GitPreview, RefusesOversizedScriptFromDeclaredSize) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    const auto commit = build_repo(temp.path / "repo.git", std::string(256 * 1024 + 1, 'x'));
    ASSERT_EQ(commit.size(), 40U);
    auto url = validate_git_url("https://example.org/demo.git");
    ASSERT_TRUE(url.has_value());
    auto preview = preview_repository(temp.path / "repo.git", "HEAD", *url, "size-test", EntryKind::Script);
    ASSERT_FALSE(preview.has_value());
    EXPECT_EQ(preview.error().code, "script-size");
}

TEST(GitFetch, RejectsUnsafeRefspecsBeforeNetwork) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    FakeHttp http;
    http.reply = {200, {}, {}};
    for (auto revision : {"refs/heads/a:refs/heads/b", "refs/tags/*", "refs/heads/", "refs/heads/../x",
                          "refs/tags/a b", "refs/heads//x", "a:b", "*"}) {
        auto result = fetch_preview(http, "https://example.org/demo.git", temp.path / "cache", "ref-test", revision);
        ASSERT_FALSE(result.has_value()) << revision;
        EXPECT_EQ(result.error().code, "fetch-ref") << revision;
    }
    EXPECT_TRUE(http.requested.empty());
}

class RecordingHttp final : public GitHttp {
public:
    explicit RecordingHttp(GitHttp& inner) : inner_(inner) {}
    std::vector<std::string> urls;
    std::expected<GitReply, ManualError> request(std::string_view url, std::string_view body, bool post) override {
        urls.emplace_back(url);
        return inner_.request(url, body, post);
    }
private:
    GitHttp& inner_;
};

TEST(GitFetch, IgnoresUrlRewritesFromGitConfig) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    const auto commit = build_repo(temp.path / "source.git", "true\n");
    ASSERT_EQ(commit.size(), 40U);
    const auto bare = temp.path / "cache/braurser/manual/rewrite-test/repo.git";
    std::filesystem::create_directories(bare.parent_path());
    {
        auto repo = init_bare(bare);
        ASSERT_TRUE(repo);
        git_config* raw = nullptr;
        ASSERT_EQ(git_repository_config(&raw, repo.get()), 0);
        std::unique_ptr<git_config, decltype(&git_config_free)> config(raw, git_config_free);
        ASSERT_EQ(git_config_set_string(config.get(), "url.https://rewritten.example/.insteadOf",
                                        "https://example.org/"), 0);
    }
    FakeUploadPack upload(temp.path / "source.git");
    RecordingHttp http(upload);
    auto result = fetch_preview(http, "https://example.org/demo.git", temp.path / "cache", "rewrite-test");
    ASSERT_TRUE(result.has_value()) << result.error().code << " " << result.error().detail;
    EXPECT_EQ(result->commit, commit);
    ASSERT_FALSE(http.urls.empty());
    for (const auto& url : http.urls) EXPECT_TRUE(url.starts_with("https://example.org/")) << url;
}

TEST(GitTree, RejectsEscapingSymlinkAndGitlink) {
    for (auto [link, gitlink] : {std::pair{"../../outside", false}, std::pair{"", true}}) {
        Temp temp;
        auto commit = build_repo(temp.path / "repo.git", "true\n", {}, link, gitlink);
        ASSERT_EQ(commit.size(), 40U);
        std::filesystem::create_directory(temp.path / "src");
        auto result = materialize_commit(temp.path / "repo.git", commit, temp.path / "src");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, gitlink ? "submodules-unsupported" : "tree-symlink");
        EXPECT_TRUE(std::filesystem::is_empty(temp.path / "src"));
    }
}

}
