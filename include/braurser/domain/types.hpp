#ifndef BRAURSER_DOMAIN_TYPES_HPP
#define BRAURSER_DOMAIN_TYPES_HPP

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace braurser::domain {

enum class DomainError { InvalidPackageName, InvalidRepoName };

/// in: error is a defined DomainError.
/// out: stable descriptive identifier.
std::string_view to_string_view(DomainError error);

class PackageName {
public:
    /// in: name is untrusted text.
    /// out: a validated package name or an error.
    static std::expected<PackageName, DomainError> make(std::string_view name);

    /// out: the validated name.
    std::string_view value() const noexcept { return value_; }

    friend bool operator==(const PackageName&, const PackageName&) = default;

private:
    explicit PackageName(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

class RepoName {
public:
    /// in: name is untrusted text.
    /// out: a validated repository name or an error.
    static std::expected<RepoName, DomainError> make(std::string_view name);

    /// out: the validated name.
    std::string_view value() const noexcept { return value_; }

    friend bool operator==(const RepoName&, const RepoName&) = default;

private:
    explicit RepoName(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

enum class SourceKind { Official, Community, Git, Outside };

struct PackageId {
    SourceKind source;
    std::optional<RepoName> repo;
    PackageName name;
    friend bool operator==(const PackageId&, const PackageId&) = default;
};

enum class InstallReason { Explicit, Dependency };

}

#endif
