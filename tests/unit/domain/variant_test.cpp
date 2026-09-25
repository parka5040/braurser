#include <braurser/domain/variants.hpp>
#include <gtest/gtest.h>
#include <array>

namespace braurser::domain {
namespace {

VariantCandidate item(std::string_view name, SourceKind source, std::string url = {}) {
    return {*PackageName::make(name), source, std::move(url)};
}

}

TEST(Variant, StandardAndBin) {
    const std::array available{item("pkg", SourceKind::Community), item("pkg-bin", SourceKind::Community)};
    for (const auto& requested : {"pkg", "pkg-bin"}) {
        const auto family = resolve_family(*PackageName::make(requested), available);
        ASSERT_TRUE(family);
        EXPECT_EQ(family->base, "pkg");
        ASSERT_EQ(family->options.size(), 2);
        ASSERT_TRUE(family->recommended);
        EXPECT_EQ(family->options[*family->recommended].kind, VariantKind::Bin);
        EXPECT_EQ(family->options[0].candidate.name.value(), "pkg-bin");
    }
}

TEST(Variant, OfficialBeatsCommunityBin) {
    const std::array available{item("foo-bin", SourceKind::Community),
                               item("foo", SourceKind::Official)};
    const auto family = resolve_family(*PackageName::make("foo"), available);
    ASSERT_TRUE(family);
    ASSERT_TRUE(family->recommended);
    EXPECT_EQ(family->options[0].candidate.source, SourceKind::Official);
    EXPECT_EQ(*family->recommended, 0);
}

TEST(Variant, DevelopmentOnlyIsNotRecommended) {
    const std::array available{item("foo-nightly", SourceKind::Community),
                               item("foo-git", SourceKind::Git)};
    const auto family = resolve_family(*PackageName::make("foo-git"), available);
    ASSERT_TRUE(family);
    EXPECT_FALSE(family->recommended);
    EXPECT_TRUE(family->options[0].development);
    EXPECT_EQ(family->options[0].candidate.name.value(), "foo-git");
}

TEST(Variant, SuffixMustBeExactAndSingle) {
    const std::array available{item("foo", SourceKind::Official), item("foobar-bin", SourceKind::Community),
                               item("foo-binary", SourceKind::Community), item("foo-bin-git", SourceKind::Git),
                               item("foo-bin", SourceKind::Community)};
    const auto family = resolve_family(*PackageName::make("foo"), available);
    ASSERT_TRUE(family);
    ASSERT_EQ(family->options.size(), 2);
    EXPECT_EQ(family->base, "foo");
    const auto nested = resolve_family(*PackageName::make("foo-bin-git"), available);
    ASSERT_TRUE(nested);
    EXPECT_EQ(nested->base, "foo-bin");
    EXPECT_EQ(nested->options.size(), 2);
    const std::array singleton{item("foo-binary", SourceKind::Community)};
    EXPECT_FALSE(resolve_family(*PackageName::make("foo"), singleton));
}

TEST(Variant, UrlNormalizationAndDifferences) {
    const std::array same{item("foo", SourceKind::Official, "HTTPS://Example.org/repo/"),
                          item("foo-bin", SourceKind::Community, "https://example.ORG/repo")};
    const auto normalized = resolve_family(*PackageName::make("foo"), same);
    ASSERT_TRUE(normalized);
    EXPECT_FALSE(normalized->urls_differ);
    const std::array different{item("foo", SourceKind::Official, "https://example.org/foo"),
                               item("foo-stable", SourceKind::Community, "https://example.org/bar")};
    const auto family = resolve_family(*PackageName::make("foo"), different);
    ASSERT_TRUE(family);
    EXPECT_TRUE(family->urls_differ);
    const std::array missing{item("foo", SourceKind::Official), item("foo-stable", SourceKind::Community,
                                                                     "https://example.org/")};
    ASSERT_TRUE(resolve_family(*PackageName::make("foo"), missing));
    EXPECT_FALSE(resolve_family(*PackageName::make("foo"), missing)->urls_differ);
    const std::array oversized{item("foo", SourceKind::Official, std::string(2049, 'a')),
                               item("foo-bin", SourceKind::Community, "https://example.org")};
    EXPECT_TRUE(resolve_family(*PackageName::make("foo"), oversized)->urls_differ);
}

}
