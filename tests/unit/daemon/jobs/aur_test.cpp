#include <braurser/daemon/jobs/aur.hpp>
#include <braurser/daemon/core/files.hpp>
#include <gtest/gtest.h>
#include <unistd.h>

namespace braurser::daemon::jobs {
namespace {
class AurFixture : public ::testing::Test {
protected:
    std::map<std::string, protocol::GetResult, std::less<>> details;
    std::map<std::string, std::string, std::less<>> bases;
    std::map<std::string, manual::BuildTree, std::less<>> trees;
    std::vector<catalog::LocalRecord> installed;
    std::vector<catalog::RepoPackage> official;
    std::vector<std::string> fetched;
    void package(const std::string& name, std::string dependencies = {}, std::string base = {}) {
        if (base.empty()) base = name;
        protocol::GetResult result;
        result.details.package = {{"community"}, {}, name};
        result.details.version = "1-1";
        details[name] = result;
        bases[name] = base;
        auto& tree = trees[base];
        tree.commit = std::string(40, 'a');
        tree.tree = std::string(40, 'b');
        tree.files = {{"PKGBUILD", 0100644, 0, {}, "pkgver=1\n"}};
        tree.srcinfo = "pkgbase = " + base + "\n\tpkgver = 1\n\tpkgrel = 1\n\tarch = any\n" + dependencies + "pkgname = " + name + "\n";
    }
    Result<Snapshot> resolve(bool updates = false) {
        Snapshot snapshot;
        snapshot.plan.kind = {"install"};
        snapshot.targets = {"app"};
        snapshot.fingerprint = "catalog";
        auto result = resolve_aur(snapshot, "app", "x86_64", installed,
            [&](std::string_view name) -> Result<protocol::GetResult> {
                const auto found = details.find(name);
                if (found == details.end()) return std::unexpected(Error{protocol::kInvalidParams, "error.generic_title"});
                return found->second;
            }, [&](std::string_view name) -> std::optional<std::string> {
                const auto found = bases.find(name);
                return found == bases.end() ? std::nullopt : std::optional(found->second);
            }, [&](std::string_view base) -> Result<manual::BuildTree> {
                fetched.emplace_back(base);
                return trees.at(std::string(base));
            }, [&](const Dependency& dependency) -> std::optional<catalog::RepoPackage> {
                for (const auto& item : official) {
                    if (item.name == dependency.name && satisfies_version(item.version, dependency)) return item;
                    for (const auto& value : item.provides) {
                        const auto provided = parse_dependency(value);
                        if (provided && provided->name == dependency.name &&
                            (dependency.comparison.empty() || satisfies_version(provided->version, dependency))) return item;
                    }
                }
                return std::nullopt;
            }, updates);
        if (!result) return std::unexpected(result.error());
        return snapshot;
    }
};

TEST_F(AurFixture, ResolvesMakeAndTransitiveDependenciesBeforeTarget) {
    package("app", "\tdepends = library>=1\n\tmakedepends = generator\n\tcheckdepends = never-build-this\n");
    package("library", "\tdepends = leaf\n");
    package("generator");
    package("leaf");
    const auto result = resolve();
    ASSERT_TRUE(result);
    ASSERT_EQ(result->aur_pins.size(), 4U);
    EXPECT_EQ(result->aur_pins.back().pkgbase, "app");
    const auto library = std::ranges::find_if(result->aur_pins, [](const auto& pin) { return pin.pkgbase == "library"; });
    const auto leaf = std::ranges::find_if(result->aur_pins, [](const auto& pin) { return pin.pkgbase == "leaf"; });
    EXPECT_LT(leaf - result->aur_pins.begin(), library - result->aur_pins.begin());
    EXPECT_EQ(result->plan.build_pins.size(), 4U);
    EXPECT_EQ(result->plan.build_review_refs.size(), 4U);
    EXPECT_EQ(result->plan.installs.size(), 4U);
    EXPECT_EQ(fetched.size(), 4U);
}

TEST_F(AurFixture, InstalledVersionedProvidesAndOfficialProvidersSatisfyDependencies) {
    package("app", "\tdepends = virtual-runtime>=2\n\tmakedepends = compiler>=3\n");
    catalog::LocalRecord runtime;
    runtime.package.name = "runtime-provider";
    runtime.package.version = "1-1";
    runtime.provides = {"virtual-runtime=2"};
    installed.push_back(runtime);
    catalog::RepoPackage compiler;
    compiler.name = "compiler-provider";
    compiler.repo = "extra";
    compiler.version = "4-1";
    compiler.provides = {"compiler=4"};
    official.push_back(compiler);
    auto result = resolve();
    ASSERT_TRUE(result);
    ASSERT_EQ(result->aur_pins.size(), 1U);
    EXPECT_EQ(result->official_deps, (std::vector<std::string>{"extra/compiler-provider"}));
    EXPECT_EQ(resolve(true).error().code, protocol::kSystemUpdateRequired);
    installed.front().provides = {"virtual-runtime"};
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
    installed.front().provides = {"virtual-runtime=1"};
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
}

TEST_F(AurFixture, SplitPackagesSharePinButKeepEveryNeededName) {
    package("app", "\tdepends = lib-one\n\tdepends = lib-two\n");
    package("lib-one", {}, "libraries");
    package("lib-two", {}, "libraries");
    trees["libraries"].srcinfo += "pkgname = lib-one\n";
    const auto result = resolve();
    ASSERT_TRUE(result);
    ASSERT_EQ(result->aur_pins.size(), 2U);
    EXPECT_EQ(result->aur_pins.front().pkgnames, (std::vector<std::string>{"lib-one", "lib-two"}));
    EXPECT_EQ(result->plan.build_review_refs.size(), 3U);
    EXPECT_EQ(fetched.size(), 2U);
}

TEST_F(AurFixture, UnknownDependenciesConstraintsCyclesAndDepthFailClosed) {
    package("app", "\tdepends = missing\n");
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
    package("missing", "\tdepends = app\n");
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
    package("missing");
    package("app", "\tdepends = missing>2\n");
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
    package("app", "\tdepends = node0\n");
    for (int index = 0; index < 9; ++index)
        package("node" + std::to_string(index), index == 8 ? "" : "\tdepends = node" + std::to_string(index + 1) + "\n");
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
}

TEST_F(AurFixture, SelfAndSplitPackageCyclesFailClosed) {
    package("app", "\tdepends = app\n");
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
    package("app", "\tdepends = sibling\n", "base");
    package("sibling", {}, "base");
    trees["base"].srcinfo += "\tdepends = app\npkgname = app\n\tdepends = sibling\n";
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
}

TEST_F(AurFixture, OfficialDependencyBoundIsEnforced) {
    std::string dependencies;
    for (int index = 0; index < 257; ++index) {
        const auto name = "official" + std::to_string(index);
        protocol::GetResult item;
        item.details.package = {{"official"}, "extra", name};
        item.details.version = "1-1";
        details[name] = item;
        dependencies += std::string(index == 256 ? "\tmakedepends = " : "\tdepends = ") + name + "\n";
    }
    package("app", dependencies);
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
}

TEST_F(AurFixture, BaseDependencyBoundIsEnforced) {
    std::string dependencies;
    for (int index = 0; index < 16; ++index) {
        const auto name = "dep" + std::to_string(index);
        package(name);
        dependencies += "\tdepends = " + name + "\n";
    }
    package("app", dependencies);
    EXPECT_EQ(resolve().error().code, protocol::kUntrustedInput);
}

TEST_F(AurFixture, PlannedVersionComesFromPinnedSourceInfoInsteadOfCatalog) {
    package("app");
    details["app"].details.version = "99-1";
    trees["app"].srcinfo = "pkgbase = app\n\tpkgver = 2.3\n\tpkgrel = 4\n\tepoch = 1\n\tarch = any\npkgname = app\n";
    const auto result = resolve();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->planned_version, "1:2.3-4");
}

TEST_F(AurFixture, DetectsPkgverFunctionAndFingerprintsDynamicVersion) {
    package("app");
    const auto baseline = resolve();
    ASSERT_TRUE(baseline);
    EXPECT_FALSE(baseline->aur_pins.front().dynamic_version);
    for (const auto* text : {"pkgver() {\n}", " \tpkgver ()\t{\n}", "pkgver()\n{\n}", "pkgver\t()  \n{\n}"}) {
        trees["app"].files.front().text = text;
        const auto result = resolve();
        ASSERT_TRUE(result) << text;
        EXPECT_TRUE(result->aur_pins.front().dynamic_version) << text;
        EXPECT_NE(baseline->fingerprint, result->fingerprint);
    }
    for (const auto* text : {"# pkgver() {", "echo pkgver() {", "function pkgver() {", "pkgversion() {",
                             "pkgver=1", "pkgver()suffix {", "pkgver () trailing", "_pkgver() {"}) {
        trees["app"].files.front().text = text;
        const auto result = resolve();
        ASSERT_TRUE(result) << text;
        EXPECT_FALSE(result->aur_pins.front().dynamic_version) << text;
    }
    trees["app"].files.front().text.reset();
    EXPECT_FALSE(resolve());
    trees["app"].files.clear();
    EXPECT_FALSE(resolve());
}

TEST_F(AurFixture, CommitAndAuxiliaryTreeChangesInvalidateFingerprint) {
    package("app");
    const auto first = resolve();
    ASSERT_TRUE(first);
    trees["app"].commit = std::string(40, 'c');
    const auto second = resolve();
    ASSERT_TRUE(second);
    EXPECT_NE(first->fingerprint, second->fingerprint);
    trees["app"].tree = std::string(40, 'd');
    const auto third = resolve();
    ASSERT_TRUE(third);
    EXPECT_NE(second->fingerprint, third->fingerprint);
}

TEST_F(AurFixture, RecordsEveryRuntimeAndBuildConstraintFromEveryParent) {
    package("app", "\tdepends = left\n\tmakedepends = right\n");
    package("left", "\tdepends = library>=1\n\tmakedepends = library<3\n");
    package("right", "\tdepends = library=1-1\n\tmakedepends = library<=2\n");
    package("library");
    trees["library"].files.front().text = "pkgver() {\n}";
    const auto first = resolve();
    ASSERT_TRUE(first);
    const auto library = std::ranges::find_if(first->aur_pins, [](const auto& pin) { return pin.pkgbase == "library"; });
    ASSERT_NE(library, first->aur_pins.end());
    ASSERT_EQ(library->constraints.size(), 4U);
    const auto info = parse_srcinfo(library->srcinfo, "x86_64");
    ASSERT_TRUE(info);
    EXPECT_TRUE(aur_version_matches(*library, info->packages.front(), "1-1"));
    EXPECT_FALSE(aur_version_matches(*library, info->packages.front(), "1-2"));
    EXPECT_FALSE(aur_version_matches(*library, info->packages.front(), "3-1"));
    trees["right"].srcinfo += "\tdepends = library>=1\n";
    const auto second = resolve();
    ASSERT_TRUE(second);
    EXPECT_NE(first->fingerprint, second->fingerprint);
}

TEST_F(AurFixture, InstalledReasonAndProvidesAreBoundIntoAurFingerprint) {
    package("app");
    catalog::LocalRecord local;
    local.package.name = "installed";
    local.package.version = "1-1";
    local.package.reason = domain::InstallReason::Explicit;
    installed.push_back(local);
    const auto first = resolve();
    ASSERT_TRUE(first);
    ASSERT_EQ(first->aur_installed.size(), 1U);
    installed.front().package.reason = domain::InstallReason::Dependency;
    const auto second = resolve();
    ASSERT_TRUE(second);
    EXPECT_NE(first->fingerprint, second->fingerprint);
    installed.front().provides = {"virtual-installed=1"};
    const auto third = resolve();
    ASSERT_TRUE(third);
    EXPECT_NE(second->fingerprint, third->fingerprint);
}

class AurArchiveFixture : public ::testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("braurser-aur-archives-fixture-" + std::to_string(getpid()));
    AurPin pin{"foo", {"foo"}, std::string(40, 'a'), std::string(40, 'b'),
        "pkgbase = foo\n\tpkgver = 1.2\n\tpkgrel = 3\n\tarch = any\npkgname = foo\npkgname = foo-bar\n", true, false};
    void SetUp() override { ASSERT_TRUE(core::ensure_private_dir(root)); }
    void TearDown() override { std::filesystem::remove_all(root); }
    void archive(std::string_view name) { ASSERT_TRUE(core::atomic_write(root / name, "package", 0600)); }
};

TEST_F(AurArchiveFixture, SplitPackagesAndAnyArchitectureMatchExactNames) {
    pin.pkgnames = {"foo", "foo-bar"};
    archive("foo-1.2-3-any.pkg.tar.zst");
    archive("foo-bar-1.2-3-x86_64.pkg.tar.xz");
    archive("foo-1.2-3-aarch64.pkg.tar.zst");
    const auto result = aur_archives(pin, root, "x86_64");
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, (std::vector<std::string>{(root / "foo-1.2-3-any.pkg.tar.zst").string(),
                                                (root / "foo-bar-1.2-3-x86_64.pkg.tar.xz").string()}));
}

TEST_F(AurArchiveFixture, PrefixCollisionDoesNotMatchEvenForDynamicVersions) {
    pin.dynamic_version = true;
    archive("foo-bar-1.2-3-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
    archive("foo-1.3-1-any.pkg.tar.zst");
    EXPECT_TRUE(aur_archives(pin, root, "x86_64"));
}

TEST_F(AurArchiveFixture, DynamicNewerVersionAcceptedButStaticVersionRejected) {
    archive("foo-1.10-1-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
    pin.dynamic_version = true;
    EXPECT_TRUE(aur_archives(pin, root, "x86_64"));
}

TEST_F(AurArchiveFixture, DynamicArchiveMustMeetEqualityUpperAndLowerBounds) {
    pin.dynamic_version = true;
    pin.constraints = {{"foo", {"foo", ">=", "1.2-3"}}, {"foo", {"foo", "<", "2"}}, {"foo", {"foo", "=", "1.3-1"}}};
    const auto wrong = root / "foo-1.4-1-any.pkg.tar.zst";
    archive(wrong.filename().string());
    const auto rejected = aur_archives(pin, root, "x86_64");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message_key, "job.build_mismatch");
    archive("foo-1.3-1-any.pkg.tar.zst");
    const auto accepted = aur_archives(pin, root, "x86_64");
    ASSERT_TRUE(accepted);
    ASSERT_EQ(accepted->size(), 1U);
    EXPECT_EQ(std::filesystem::path(accepted->front()).filename(), "foo-1.3-1-any.pkg.tar.zst");
}

TEST_F(AurArchiveFixture, DynamicOlderVersionRejected) {
    pin.dynamic_version = true;
    archive("foo-1.1-99-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
}

TEST_F(AurArchiveFixture, DynamicEpochComparedBeforePackageVersion) {
    pin.dynamic_version = true;
    pin.srcinfo = "pkgbase = foo\n\tpkgver = 1.2\n\tpkgrel = 3\n\tepoch = 2\n\tarch = any\npkgname = foo\n";
    archive("foo-1:99-1-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
    archive("foo-2:1.3-1-any.pkg.tar.zst");
    EXPECT_TRUE(aur_archives(pin, root, "x86_64"));
}

TEST_F(AurArchiveFixture, EpochlessDynamicArchiveUsesPinnedEpoch) {
    pin.dynamic_version = true;
    pin.srcinfo = "pkgbase = foo\n\tpkgver = 1.2\n\tpkgrel = 3\n\tepoch = 2\n\tarch = any\npkgname = foo\n";
    archive("foo-1.1-99-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
    archive("foo-1.3-1-any.pkg.tar.zst");
    EXPECT_TRUE(aur_archives(pin, root, "x86_64"));
}

TEST_F(AurArchiveFixture, StaticExplicitEpochMustMatchPinnedEpoch) {
    pin.srcinfo = "pkgbase = foo\n\tpkgver = 1.2\n\tpkgrel = 3\n\tepoch = 2\n\tarch = any\npkgname = foo\n";
    archive("foo-1:1.2-3-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
    archive("foo-2:1.2-3-any.pkg.tar.zst");
    EXPECT_TRUE(aur_archives(pin, root, "x86_64"));
}

TEST_F(AurArchiveFixture, MultipleAcceptedDynamicVersionsRejected) {
    pin.dynamic_version = true;
    archive("foo-1.3-1-any.pkg.tar.zst");
    archive("foo-1.4-1-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
}

TEST_F(AurArchiveFixture, SymlinksAndNonRegularArchivesRejected) {
    archive("contents");
    std::filesystem::create_symlink(root / "contents", root / "foo-1.2-3-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
    std::filesystem::remove(root / "foo-1.2-3-any.pkg.tar.zst");
    std::filesystem::create_directory(root / "foo-1.2-3-any.pkg.tar.zst");
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
}

TEST(AurArchives, RequiresExactNeededNameVersionAndUnambiguousRegularArchive) {
    const auto root = std::filesystem::temp_directory_path() / ("braurser-aur-archives-" + std::to_string(getpid()));
    ASSERT_TRUE(core::ensure_private_dir(root));
    AurPin pin{"widget", {"widget"}, std::string(40, 'a'), std::string(40, 'b'),
        "pkgbase = widget\n\tpkgver = 1.2\n\tpkgrel = 3\n\tepoch = 2\n\tarch = any\npkgname = widget\npkgname = widget-docs\n", true};
    ASSERT_TRUE(core::atomic_write(root / "widget-9-1-any.pkg.tar.zst", "old", 0600));
    ASSERT_TRUE(core::atomic_write(root / "widget-docs-1.2-3-any.pkg.tar.zst", "docs", 0600));
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
    const auto archive = root / "widget-1.2-3-any.pkg.tar.zst";
    ASSERT_TRUE(core::atomic_write(archive, "package", 0600));
    const auto result = aur_archives(pin, root, "x86_64");
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, (std::vector<std::string>{archive.string()}));
    ASSERT_TRUE(core::atomic_write(root / "widget-1.2-3-x86_64.pkg.tar.zst", "ambiguous", 0600));
    EXPECT_FALSE(aur_archives(pin, root, "x86_64"));
    std::filesystem::remove_all(root);
}
}
}
