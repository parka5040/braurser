#ifndef BRAURSER_DAEMON_JOBS_SRCINFO_HPP
#define BRAURSER_DAEMON_JOBS_SRCINFO_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::jobs {

struct Dependency {
    std::string name;
    std::string comparison;
    std::string version;
};

struct SourcePackage {
    std::string name;
    std::string pkgver;
    std::string pkgrel;
    std::string epoch;
    std::vector<std::string> arch;
    std::vector<std::string> depends;
    std::vector<std::string> makedepends;
    std::vector<std::string> provides;
};

struct BuiltPackage {
    std::string name;
    std::string version;
    std::string arch;
    std::vector<std::string> depends;
};

struct SourceInfo {
    std::string pkgbase;
    std::vector<SourcePackage> packages;
};

/// in: text is a bounded package dependency with an optional version constraint.
/// out: a validated dependency or no value for malformed input.
std::optional<Dependency> parse_dependency(std::string_view text);

/// in: text is unexecuted SRCINFO and architecture is the build machine architecture.
/// out: validated split-package metadata with inherited and architecture-specific fields.
std::optional<SourceInfo> parse_srcinfo(std::string_view text, std::string_view architecture);

/// in: text is bounded, untrusted PKGINFO from a built archive.
/// out: unique package identity and validated runtime dependencies, or no value.
std::optional<BuiltPackage> parse_pkginfo(std::string_view text);

/// in: version is a package or versioned provide version and dependency is validated.
/// out: whether the version satisfies the dependency constraint.
bool satisfies_version(std::string_view version, const Dependency& dependency);

}

#endif
