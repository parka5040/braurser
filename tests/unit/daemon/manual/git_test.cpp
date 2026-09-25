#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/process.hpp>

#include <git2.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
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
        return reply;
    }
};

class FakeUploadPack final : public GitHttp {
public:
    explicit FakeUploadPack(std::filesystem::path repo) : repo_(std::move(repo)) {}
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
        return GitReply{200, post ? result->out : "001e# service=git-upload-pack\n0000" + result->out, {}};
    }
private:
    std::filesystem::path repo_;
    braurser::daemon::core::PosixProcessRunner runner_;
};

std::string build_repo(const std::filesystem::path& path, std::string_view script,
                       std::string_view makefile = {}, std::string_view link = {}, bool gitlink = false,
                       std::string_view pkgbuild = {}) {
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
    CurlGitHttp http;
    GuardedGitTransport transport(http);
    auto result = transport.exchange("https://localhost/repo");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "git-connect");
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
