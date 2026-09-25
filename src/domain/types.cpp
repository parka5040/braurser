#include <braurser/domain/types.hpp>
#include <braurser/validate/validate.hpp>

namespace braurser::domain {

std::string_view to_string_view(DomainError error) {
    switch (error) {
    case DomainError::InvalidPackageName: return "invalid-package-name";
    case DomainError::InvalidRepoName: return "invalid-repo-name";
    }
    return "unknown-domain-error";
}

std::expected<PackageName, DomainError> PackageName::make(std::string_view name) {
    if (!br_valid_pkgname(name.data(), name.size())) return std::unexpected(DomainError::InvalidPackageName);
    return PackageName(std::string(name));
}

std::expected<RepoName, DomainError> RepoName::make(std::string_view name) {
    if (!br_valid_repo_name(name.data(), name.size())) return std::unexpected(DomainError::InvalidRepoName);
    return RepoName(std::string(name));
}

}
