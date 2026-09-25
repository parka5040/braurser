#include <braurser/domain/types.hpp>
#include <gtest/gtest.h>

namespace braurser::domain {

TEST(Domain, StrongNames) {
    const auto valid = PackageName::make("hello-bin");
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->value(), "hello-bin");
    EXPECT_EQ(PackageName::make("Hello").error(), DomainError::InvalidPackageName);
    EXPECT_EQ(PackageName::make(std::string_view("ab\0cd", 5)).error(), DomainError::InvalidPackageName);
    const auto repo = RepoName::make("Core");
    ASSERT_TRUE(repo);
    EXPECT_EQ(repo->value(), "Core");
    EXPECT_EQ(RepoName::make(".core").error(), DomainError::InvalidRepoName);
    const PackageId id{SourceKind::Official, *repo, *valid};
    EXPECT_EQ(id.name.value(), "hello-bin");
    EXPECT_EQ(to_string_view(DomainError::InvalidPackageName), "invalid-package-name");
    EXPECT_EQ(to_string_view(DomainError::InvalidRepoName), "invalid-repo-name");
}

}
