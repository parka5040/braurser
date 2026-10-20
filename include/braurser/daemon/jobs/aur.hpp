#ifndef BRAURSER_DAEMON_JOBS_AUR_HPP
#define BRAURSER_DAEMON_JOBS_AUR_HPP

#include <braurser/daemon/jobs/jobs.hpp>
#include <braurser/daemon/jobs/srcinfo.hpp>
#include <braurser/daemon/catalog/sources.hpp>
#include <span>

namespace braurser::daemon::jobs {

using AurLookup = std::function<Result<protocol::GetResult>(std::string_view)>;
using AurBase = std::function<std::optional<std::string>(std::string_view)>;
using AurFetch = std::function<Result<manual::BuildTree>(std::string_view)>;
using OfficialLookup = std::function<std::optional<catalog::RepoPackage>(const Dependency&)>;

/// in: target is an exact AUR name; lookups and fetched trees are untrusted inputs.
/// out: bounded dependency-first pins and official dependencies, or a closed refusal.
Result<void> resolve_aur(Snapshot& snapshot, std::string_view target, std::string_view architecture,
                         std::span<const catalog::LocalRecord> installed, const AurLookup& lookup,
                         const AurBase& base_lookup, const AurFetch& fetch, const OfficialLookup& official, bool official_updates);

/// in: pin names only needed split packages and packages is a private archive directory.
/// out: one regular archive per name matching the pin and every incoming constraint; rejects ambiguity.
Result<std::vector<std::string>> aur_archives(const AurPin& pin, const std::filesystem::path& packages,
                                             std::string_view architecture);

struct AurArchive {
    std::string path;
    std::string sha256;
};

/// in: snapshot contains approved dependencies and runner performs bounded read-only queries.
/// out: the complete official transaction is displayed and bound into the fingerprint.
Result<void> plan_aur_official(Snapshot& snapshot, core::ProcessRunner& runner);

/// in: archives are selected for pin; snapshot contains reviewed closure and installed inputs.
/// out: verified PKGINFO and fd-read SHA-256 for every archive, or a closed refusal.
Result<std::vector<AurArchive>> validate_aur_archives(const Snapshot& snapshot, const AurPin& pin,
    std::span<const std::string> archives, std::string_view architecture, core::ProcessRunner& runner);

/// in: archives have previously validated metadata and SHA-256.
/// err: refuses missing or byte-changed archives before privileged installation.
Result<void> recheck_aur_archives(std::span<const AurArchive> archives);

/// in: pin and version describe a built or installed AUR package.
/// out: whether the version matches the source and every incoming constraint.
bool aur_version_matches(const AurPin& pin, const SourcePackage& package, std::string_view version);

/// in: snapshot captures installed reasons before execution.
/// out: dependency-only AUR names excluding every previously explicit package.
std::vector<std::string> aur_dependency_names(const Snapshot& snapshot);

/// in: root, workspace and home are private absolute build paths.
/// out: makepkg argv, PID-contained by bwrap when contained is true; environment and limits unchanged.
core::ProcessSpec aur_build_spec(const std::filesystem::path& root, const std::filesystem::path& workspace,
                                 const std::filesystem::path& home,
                                 bool contained = true);

/// in: workspace is the private job build directory.
/// err: refuses builds when available filesystem space is below four GiB.
Result<void> aur_build_space(const std::filesystem::path& workspace);

/// in: workspace is strictly beneath root and success is the terminal job outcome.
/// out: removes successful workspaces and retains at most three newest failed workspaces.
Result<void> finish_aur_workspace(const std::filesystem::path& root, const std::filesystem::path& workspace, bool success);

}

#endif
