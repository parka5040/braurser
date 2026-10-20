#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/core/process.hpp>
#include <gtest/gtest.h>
#include <git2.h>
#include <algorithm>
#include <memory>
#include <unistd.h>

namespace braurser::daemon::manual {
namespace {
class BuildPins : public ::testing::Test {
protected:
    std::filesystem::path root;
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "braurser-pins-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
        ASSERT_GE(git_libgit2_init(), 0);
    }
    void TearDown() override { if (!root.empty()) std::filesystem::remove_all(root); }
    std::string commit(std::vector<std::tuple<std::string, std::string, git_filemode_t>> files) {
        git_repository* raw_repo = nullptr;
        const auto bare = root / "source.git";
        if (git_repository_open_bare(&raw_repo, bare.c_str()) != 0 && git_repository_init(&raw_repo, bare.c_str(), 1) != 0) return {};
        std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw_repo, git_repository_free);
        git_treebuilder* raw_builder = nullptr;
        if (git_treebuilder_new(&raw_builder, repo.get(), nullptr) != 0) return {};
        std::unique_ptr<git_treebuilder, decltype(&git_treebuilder_free)> builder(raw_builder, git_treebuilder_free);
        files.emplace_back("PKGBUILD", "pkgname=widget\npkgver=1\npkgrel=1\n", GIT_FILEMODE_BLOB);
        files.emplace_back(".SRCINFO", "pkgbase = widget\n\tpkgver = 1\n\tpkgrel = 1\n\tarch = any\npkgname = widget\n", GIT_FILEMODE_BLOB);
        for (const auto& [name, bytes, mode] : files) {
            git_oid oid{};
            if ((mode == GIT_FILEMODE_COMMIT ? git_oid_fromstr(&oid, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") :
                 git_blob_create_frombuffer(&oid, repo.get(), bytes.data(), bytes.size())) != 0 ||
                git_treebuilder_insert(nullptr, builder.get(), name.c_str(), &oid, mode) != 0) return {};
        }
        git_oid tree_id{};
        if (git_treebuilder_write(&tree_id, builder.get()) != 0) return {};
        git_tree* raw_tree = nullptr;
        if (git_tree_lookup(&raw_tree, repo.get(), &tree_id) != 0) return {};
        std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
        git_signature* raw_signature = nullptr;
        if (git_signature_new(&raw_signature, "Test", "test@example.org", 0, 0) != 0) return {};
        std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(raw_signature, git_signature_free);
        git_oid id{};
        if (git_commit_create(&id, repo.get(), nullptr, signature.get(), signature.get(), nullptr, "pin", tree.get(), 0, nullptr) != 0) return {};
        git_reference* reference = nullptr;
        if (git_reference_create(&reference, repo.get(), "refs/heads/main", &id, 1, nullptr) != 0) return {};
        git_reference_free(reference);
        if (git_repository_set_head(repo.get(), "refs/heads/main") != 0) return {};
        std::array<char, GIT_OID_MAX_HEXSIZE + 1> hex{};
        git_oid_tostr(hex.data(), hex.size(), &id);
        return hex.data();
    }
};

class UploadPack final : public GitHttp {
public:
    explicit UploadPack(std::filesystem::path repo) : repo_(std::move(repo)) {}
    std::expected<GitReply, ManualError> request(std::string_view, std::string_view body, bool post) override {
        core::ProcessSpec spec;
        spec.executable = "/usr/bin/git-upload-pack";
        spec.args = {"--stateless-rpc"};
        if (!post) spec.args.push_back("--advertise-refs");
        spec.args.push_back(repo_.string());
        spec.env = {{"PATH", "/usr/bin"}};
        spec.stdin_data = std::string(body);
        spec.max_stdout = 16 * 1024 * 1024;
        auto result = runner_.run(spec);
        if (!result || result->exit_code != 0) return std::unexpected(ManualError{"upload", {}, {}, {}});
        auto spool = GitReply::Body::create(repo_.parent_path());
        if (!spool) return std::unexpected(spool.error());
        auto stored = spool->append(post ? result->out : "001e# service=git-upload-pack\n0000" + result->out);
        if (!stored) return std::unexpected(stored.error());
        return GitReply{200, std::move(*spool), {}};
    }
private:
    std::filesystem::path repo_;
    core::PosixProcessRunner runner_;
};

TEST_F(BuildPins, AllAuxiliaryBytesModesAndBinaryDigestsArePinned) {
    const auto id = commit({{"widget.install", "post_install() { true; }\n", GIT_FILEMODE_BLOB_EXECUTABLE},
                            {"binary", std::string("a\0b", 3), GIT_FILEMODE_BLOB},
                            {"large", std::string(300 * 1024, 'x'), GIT_FILEMODE_BLOB},
                            {"link", "widget.install", GIT_FILEMODE_LINK}});
    ASSERT_EQ(id.size(), 40U);
    const auto pin = read_build_tree(root / "source.git", id);
    ASSERT_TRUE(pin) << pin.error().code;
    EXPECT_EQ(pin->commit, id);
    EXPECT_EQ(pin->tree.size(), 40U);
    ASSERT_EQ(pin->files.size(), 6U);
    const auto large = std::ranges::find_if(pin->files, [](const auto& file) { return file.path == "large"; });
    ASSERT_NE(large, pin->files.end());
    ASSERT_TRUE(large->text);
    EXPECT_EQ(large->text->size(), 300U * 1024);
    const auto binary = std::ranges::find_if(pin->files, [](const auto& file) { return file.path == "binary"; });
    ASSERT_NE(binary, pin->files.end());
    EXPECT_EQ(binary->size, 3U);
    EXPECT_FALSE(binary->text);
    EXPECT_EQ(binary->sha256, "59b271ae1bbcb1d31d41929817f4b16fb439eb4f31520b5ad1d5ce98920a7138");
    ASSERT_TRUE(core::ensure_private_dir(root / "materialized"));
    EXPECT_TRUE(materialize_commit(root / "source.git", id, root / "materialized"));
    EXPECT_FALSE(read_build_tree(root / "source.git", std::string(40, 'f')));
}

TEST_F(BuildPins, RejectsEscapingSymlinksAndBlobBudgets) {
    auto id = commit({{"escape", "../outside", GIT_FILEMODE_LINK}});
    ASSERT_EQ(id.size(), 40U);
    EXPECT_FALSE(read_build_tree(root / "source.git", id));
    id = commit({{"vendor", "", GIT_FILEMODE_COMMIT}});
    ASSERT_EQ(id.size(), 40U);
    EXPECT_FALSE(read_build_tree(root / "source.git", id));
    id = commit({{"one", "two", GIT_FILEMODE_LINK}, {"two", "one", GIT_FILEMODE_LINK}});
    ASSERT_EQ(id.size(), 40U);
    EXPECT_FALSE(read_build_tree(root / "source.git", id));
    id = commit({{"large", std::string(4 * 1024 * 1024, 'x'), GIT_FILEMODE_BLOB}});
    ASSERT_EQ(id.size(), 40U);
    EXPECT_FALSE(read_build_tree(root / "source.git", id));
    std::vector<std::tuple<std::string, std::string, git_filemode_t>> files;
    for (int index = 0; index < 255; ++index) files.emplace_back("file" + std::to_string(index), "x", GIT_FILEMODE_BLOB);
    id = commit(std::move(files));
    ASSERT_EQ(id.size(), 40U);
    EXPECT_FALSE(read_build_tree(root / "source.git", id));
}

TEST_F(BuildPins, RejectsUnreviewableTextButAllowsLargeBinary) {
    auto id = commit({{"build.sh", std::string(500 * 1024, 'x'), GIT_FILEMODE_BLOB_EXECUTABLE}});
    ASSERT_EQ(id.size(), 40U);
    auto tree = read_build_tree(root / "source.git", id);
    ASSERT_FALSE(tree);
    EXPECT_EQ(tree.error().code, "build-text-too-large");
    UploadPack http(root / "source.git");
    const auto fetched = fetch_build_tree(http, "widget", root / "cache");
    ASSERT_FALSE(fetched);
    EXPECT_EQ(fetched.error().code, "build-text-too-large");
    id = commit({{"build.sh", std::string(448 * 1024, 'x'), GIT_FILEMODE_BLOB_EXECUTABLE},
                 {"image", std::string(500 * 1024, '\0'), GIT_FILEMODE_BLOB}});
    tree = read_build_tree(root / "source.git", id);
    ASSERT_TRUE(tree) << tree.error().code;
    const auto binary = std::ranges::find_if(tree->files, [](const auto& file) { return file.path == "image"; });
    ASSERT_NE(binary, tree->files.end());
    EXPECT_FALSE(binary->text);
    EXPECT_EQ(binary->size, 500U * 1024);
}

TEST_F(BuildPins, ReadsAndRefreshesPackedPins) {
    const auto id = commit({{"patch", "reviewed", GIT_FILEMODE_BLOB}});
    ASSERT_TRUE(read_build_tree(root / "source.git", id));
    git_repository* raw_repo = nullptr;
    ASSERT_EQ(git_repository_open_bare(&raw_repo, (root / "source.git").c_str()), 0);
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw_repo, git_repository_free);
    git_refdb* raw_refdb = nullptr;
    ASSERT_EQ(git_repository_refdb(&raw_refdb, repo.get()), 0);
    std::unique_ptr<git_refdb, decltype(&git_refdb_free)> refdb(raw_refdb, git_refdb_free);
    ASSERT_EQ(git_refdb_compress(refdb.get()), 0);
    const auto tree = read_build_tree(root / "source.git", id);
    ASSERT_TRUE(tree) << tree.error().code;
}

TEST_F(BuildPins, PrunesExpiredPinsButRetainsLivePinsAndRefreshesAccess) {
    const auto old = commit({{"patch", "old", GIT_FILEMODE_BLOB}});
    ASSERT_TRUE(read_build_tree(root / "source.git", old));
    const auto retained = commit({{"patch", "retained", GIT_FILEMODE_BLOB}});
    ASSERT_TRUE(read_build_tree(root / "source.git", retained));
    const auto fresh = commit({{"patch", "fresh", GIT_FILEMODE_BLOB}});
    ASSERT_TRUE(read_build_tree(root / "source.git", fresh));
    const auto parent = root / "cache/braurser/manual/aur-widget";
    ASSERT_TRUE(core::ensure_private_dir(parent));
    const auto bare = parent / "repo.git";
    std::filesystem::copy(root / "source.git", bare, std::filesystem::copy_options::recursive);
    const auto expired = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 15);
    const auto ref = [&](const auto& id) { return bare / "refs/braurser/pins" / id; };
    for (const auto& id : {old, retained, fresh}) {
        std::filesystem::last_write_time(ref(id), expired);
        std::filesystem::last_write_time(bare / "logs/refs/braurser/pins" / id, expired);
    }
    ASSERT_TRUE(read_build_tree(bare, fresh));
    const std::vector<std::string> keep{retained};
    ASSERT_TRUE(prune_pins(root / "cache", std::chrono::hours(24 * 14), keep));
    EXPECT_FALSE(std::filesystem::exists(ref(old)));
    EXPECT_TRUE(std::filesystem::exists(ref(retained)));
    EXPECT_TRUE(std::filesystem::exists(ref(fresh)));
    EXPECT_GT(std::filesystem::last_write_time(bare / "logs/refs/braurser/pins" / fresh), expired);
    EXPECT_FALSE(prune_pins("relative", std::chrono::hours(24 * 14), {}));
}

TEST_F(BuildPins, EvictsLeastRecentlyUsedWholeReposWithoutFollowingSymlinks) {
    const auto id = commit({{"patch", "reviewed", GIT_FILEMODE_BLOB}});
    ASSERT_TRUE(read_build_tree(root / "source.git", id));
    const auto cache = root / "cache";
    const auto manual = cache / "braurser/manual";
    ASSERT_TRUE(core::ensure_private_dir(manual));
    for (const auto* name : {"aur-old", "aur-new"}) {
        const auto parent = manual / name;
        ASSERT_TRUE(core::ensure_private_dir(parent));
        const auto bare = parent / "repo.git";
        std::filesystem::copy(root / "source.git", bare, std::filesystem::copy_options::recursive);
        ASSERT_TRUE(core::atomic_write(bare / "padding", "x"));
        std::filesystem::resize_file(bare / "padding", 130ULL * 1024 * 1024);
        const auto age = std::filesystem::file_time_type::clock::now() - std::chrono::hours(std::string_view(name) == "aur-old" ? 48 : 24);
        std::filesystem::last_write_time(bare / "refs/braurser/pins" / id, age);
        std::filesystem::last_write_time(bare / "logs/refs/braurser/pins" / id, age);
        std::filesystem::last_write_time(bare, age);
    }
    std::filesystem::create_directory_symlink(root / "source.git", manual / "aur-escape");
    const std::vector<std::string> keep{id};
    ASSERT_TRUE(prune_pins(cache, std::chrono::hours(24 * 14), keep));
    EXPECT_TRUE(std::filesystem::exists(manual / "aur-old"));
    EXPECT_TRUE(std::filesystem::exists(manual / "aur-new"));
    ASSERT_TRUE(prune_pins(cache, std::chrono::hours(24 * 14), {}));
    EXPECT_FALSE(std::filesystem::exists(manual / "aur-old"));
    EXPECT_TRUE(std::filesystem::exists(manual / "aur-new"));
    EXPECT_TRUE(std::filesystem::is_symlink(manual / "aur-escape"));
    EXPECT_TRUE(std::filesystem::exists(root / "source.git"));
}

TEST_F(BuildPins, GuardedRefetchKeepsPreviouslyReviewedOrphanCommitMaterializable) {
    const auto first = commit({{"patch", "reviewed", GIT_FILEMODE_BLOB}});
    UploadPack http(root / "source.git");
    auto pin = fetch_build_tree(http, "widget", root / "cache");
    ASSERT_TRUE(pin) << pin.error().code;
    EXPECT_EQ(pin->commit, first);
    const auto second = commit({{"patch", "changed", GIT_FILEMODE_BLOB}});
    pin = fetch_build_tree(http, "widget", root / "cache");
    ASSERT_TRUE(pin) << pin.error().code;
    EXPECT_EQ(pin->commit, second);
    const auto bare = root / "cache/braurser/manual/aur-widget/repo.git";
    auto old = read_build_tree(bare, first);
    ASSERT_TRUE(old);
    ASSERT_TRUE(core::ensure_private_dir(root / "old"));
    EXPECT_TRUE(materialize_commit(bare, first, root / "old"));
    const auto bytes = core::read_bounded(root / "old/patch", 1024);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(*bytes, "reviewed");
    EXPECT_FALSE(fetch_build_tree(http, "../widget", root / "cache"));
    EXPECT_TRUE(fetch_build_tree(http, "widget_cli++", root / "cache"));
}
}
}
