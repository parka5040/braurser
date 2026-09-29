#include <braurser/gtk/kit.hpp>
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>

namespace braurser::gtk::kit {
namespace {
std::string render(std::string text, std::string href = {}) {
    const std::array<TextRun, 1> runs{{{std::move(text), false, false, false, std::move(href)}}};
    return build_markup(runs);
}

TEST(DocumentMarkup, EscapesEveryTextCharacter) {
    EXPECT_EQ(render("< > & \" ' <b> </span>"), "&lt; &gt; &amp; &quot; &apos; &lt;b&gt; &lt;/span&gt;");
}

TEST(DocumentMarkup, UsesOnlyFixedStyleTags) {
    const std::array<TextRun, 1> runs{{{"<span foreground='red'>", true, true, true, {}}}};
    EXPECT_EQ(build_markup(runs), "<b><i><tt>&lt;span foreground=&apos;red&apos;&gt;</tt></i></b>");
}

TEST(DocumentMarkup, ReplacesInvalidUtf8AndXmlControls) {
    const std::string invalid("a\xFF\xC0<b>\0\x01z", 9);
    const auto result = render(invalid);
    EXPECT_TRUE(g_utf8_validate(result.data(), static_cast<gssize>(result.size()), nullptr));
    EXPECT_EQ(result, "a\xEF\xBF\xBD\xEF\xBF\xBD&lt;b&gt;\xEF\xBF\xBD\xEF\xBF\xBDz");
}

TEST(DocumentMarkup, PreservesValidUnicodeAndWhitespace) {
    EXPECT_EQ(render("日本語 — مرحبا\n\t"), "日本語 — مرحبا\n\t");
}

TEST(DocumentMarkup, DemotesUnsafeLinks) {
    for (const auto* href : {"javascript:alert(1)", "data:text/html,bad", "http://example.org", "file:///etc/passwd",
                             "//example.org", "https://", "https:///bad", "https://user:password@example.org",
                             "https://example.org\n/bad", "https://example.org\\bad"}) {
        EXPECT_FALSE(allowed_link(href)) << href;
        EXPECT_EQ(render("<link>", href), "&lt;link&gt;") << href;
    }
}

TEST(DocumentMarkup, KeepsHttpsAndEscapesAttributes) {
    EXPECT_EQ(render("Wiki & help", "https://example.org/a?x=1&y=\"quoted\""),
              "<a href=\"https://example.org/a?x=1&amp;y=&quot;quoted&quot;\">Wiki &amp; help</a>");
}

TEST(DocumentMarkup, BoundsExternalInput) {
    EXPECT_THROW(render(std::string(65537, 'x')), std::length_error);
    EXPECT_FALSE(allowed_link("https://example.org/" + std::string(4096, 'x')));
    EXPECT_THROW(build_markup(std::vector<TextRun>(1025)), std::length_error);
    std::vector<TextRun> runs(17, TextRun{std::string(65536, 'x'), false, false, false, {}});
    EXPECT_THROW(build_markup(runs), std::length_error);
}

TEST(DocumentMarkup, DoesNotInterpretClosingTagsOrUriQuotes) {
    EXPECT_EQ(render("</a><b>bad</b>", "https://example.org/?x=' onclick='bad"),
              "&lt;/a&gt;&lt;b&gt;bad&lt;/b&gt;");
    EXPECT_EQ(build_markup({}), "");
}

TEST(CodeText, AcceptsLargeReviewTextAndBoundsIt) {
    const std::string large(200 * 1024, 'x');
    EXPECT_EQ(safe_code(large), large);
    EXPECT_EQ(safe_code(std::string(262144, 'y')).size(), 262144U);
    EXPECT_THROW(safe_code(std::string(262145, 'x')), std::length_error);
    EXPECT_THROW(safe_text(std::string(65537, 'x')), std::length_error);
}

TEST(CodeText, ShowsBidiAndInvisibleFormatCharacters) {
    EXPECT_EQ(safe_code("a\u202Eb\u2066c\u2069d\u200Be\uFEFFf\u061Cg"),
              "a\uFFFDb\uFFFDc\uFFFDd\uFFFDe\uFFFDf\uFFFDg");
    EXPECT_EQ(safe_code(std::string("x\0y\x1b", 4)), "x\uFFFDy\uFFFD");
    EXPECT_EQ(safe_code("if [ \"$a\" ]; then\n\techo \u05E9\u05DC\u05D5\u05DD \u0645\u0631\u062D\u0628\u0627\r\n"),
              "if [ \"$a\" ]; then\n\techo \u05E9\u05DC\u05D5\u05DD \u0645\u0631\u062D\u0628\u0627\r\n");
    EXPECT_EQ(safe_text("a\u202Eb"), "a\u202Eb");
}
}
}
