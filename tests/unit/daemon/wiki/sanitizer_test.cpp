#include <braurser/daemon/wiki/sanitizer.hpp>
#include <braurser/daemon/core/files.hpp>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <chrono>
#include <string>

namespace braurser::daemon::wiki {
namespace {

std::string fixture(std::string_view file, std::string_view folder = "wiki") {
    auto content = core::read_bounded(std::filesystem::path(FIXTURES_ROOT) / folder / file, 2 * 1024 * 1024);
    EXPECT_TRUE(content.has_value());
    return content.value_or("");
}

std::string all_text(const Document& doc) {
    std::string text;
    for (const auto& block : doc.blocks) {
        std::visit([&](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Heading> || std::is_same_v<T, Code>) text += b.text;
            else if constexpr (std::is_same_v<T, List>) {
                for (const auto& item : b.items) text += inline_text(item);
            } else text += inline_text(b.inlines);
            text += ' ';
        }, block);
    }
    return text;
}

TEST(Sanitizer, RecordedFirefoxDropsMetaAndComments) {
    auto page = nlohmann::json::parse(fixture("firefox.section0.json"));
    auto doc = sanitize(page.at("parse").at("text").get<std::string>());
    ASSERT_EQ(doc.blocks.size(), 1U);
    const auto* para = std::get_if<Paragraph>(&doc.blocks.front());
    ASSERT_NE(para, nullptr);
    EXPECT_NE(inline_text(para->inlines).find("Firefox is a popular"), std::string::npos);
    EXPECT_EQ(all_text(doc).find("Related articles"), std::string::npos);
    EXPECT_EQ(all_text(doc).find("NewPP"), std::string::npos);
}

TEST(Sanitizer, RecordedPacmanHasSemanticBoxesAndSections) {
    auto page = nlohmann::json::parse(fixture("pacman.full.json"));
    const auto doc = sanitize(page.at("parse").at("text").get<std::string>());
    std::size_t notes = 0, warnings = 0;
    for (const auto& block : doc.blocks) {
        notes += std::holds_alternative<Note>(block);
        warnings += std::holds_alternative<Warning>(block);
    }
    EXPECT_EQ(notes, 16U);
    EXPECT_EQ(warnings, 11U);
    EXPECT_FALSE(doc.sections.empty());
    EXPECT_EQ(all_text(doc).find("NewPP"), std::string::npos);
}

TEST(Sanitizer, HostileMarkupBecomesSafeTextAndHttpsLinksOnly) {
    const auto doc = sanitize(fixture("hostile.html"));
    const auto text = all_text(doc);
    for (const auto forbidden : {"secret", "steal", "navbox", "footnote", "edit"})
        EXPECT_EQ(text.find(forbidden), std::string::npos) << forbidden;
    EXPECT_NE(text.find("A — B"), std::string::npos);
    std::size_t warnings = 0;
    bool relative_resolved = false;
    for (const auto& block : doc.blocks) {
        warnings += std::holds_alternative<Warning>(block);
        if (const auto* p = std::get_if<Paragraph>(&block)) {
            for (const auto& inline_value : p->inlines) {
                if (const auto* link = std::get_if<Link>(&inline_value)) {
                    EXPECT_TRUE(link->href.starts_with("https://"));
                    relative_resolved |= link->href == "https://wiki.archlinux.org/title/Firefox/Privacy";
                    EXPECT_NE(link->label, "unsafe");
                    EXPECT_NE(link->label, "data link");
                    EXPECT_NE(link->label, "protocol link");
                }
            }
        }
    }
    EXPECT_EQ(warnings, 1U);
    EXPECT_TRUE(relative_resolved);
    EXPECT_NE(text.find("grandchild"), std::string::npos);
}

TEST(Sanitizer, ManyStyledSiblingsAreLinear) {
    std::string html = "<p>";
    for (int i = 0; i < 30000; ++i) html += "<b>x</b><i>x</i>";
    html += "</p>";
    const auto started = std::chrono::steady_clock::now();
    const auto doc = sanitize(html);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_EQ(doc.blocks.size(), 1U);
    EXPECT_EQ(inline_text(std::get<Paragraph>(doc.blocks.front()).inlines), std::string(60000, 'x'));
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST(Sanitizer, RejectsDeepNestingBeforeBuildingTree) {
    std::string html;
    for (int i = 0; i < 100000; ++i) html += "<div>";
    html += "<p>hidden</p>";
    for (int i = 0; i < 100000; ++i) html += "</div>";
    EXPECT_TRUE(sanitize(html).blocks.empty());
}

TEST(Sanitizer, NestingScanSkipsVoidQuotedAndRawMarkup) {
    std::string html = "<!DOCTYPE html><!--";
    for (int i = 0; i < 1500; ++i) html += "<div>";
    html += "--><ScRiPt>";
    for (int i = 0; i < 1500; ++i) html += "<div>";
    html += "</sCrIpT><STYLE>";
    for (int i = 0; i < 1500; ++i) html += "<div>";
    html += "</style><div data-value='> <div>'><p>visible</p></div>";
    for (int i = 0; i < 1500; ++i) html += "<br><img><input><hr/><wbr>";
    EXPECT_NE(all_text(sanitize(html)).find("visible"), std::string::npos);
}

TEST(Sanitizer, MalformedAndOversizedInputsBounded) {
    const auto malformed = sanitize(fixture("malformed.html"));
    EXPECT_NE(all_text(malformed).find("second"), std::string::npos);
    EXPECT_EQ(all_text(malformed).find("secret noscript"), std::string::npos);
    const auto oversized = sanitize("<p>" + std::string(130000, 'z') + "</p>");
    ASSERT_FALSE(oversized.blocks.empty());
    EXPECT_LE(inline_text(std::get<Paragraph>(oversized.blocks.front()).inlines).size(), max_block_text);
    EXPECT_TRUE(sanitize(std::string(2 * 1024 * 1024 + 1, 'x')).blocks.empty());
    const auto large_list = sanitize("<ul><li>" + std::string(130000, 'z') +
                                     "<ul><li>child</li></ul></li></ul>");
    ASSERT_FALSE(large_list.blocks.empty());
    const auto* list = std::get_if<List>(&large_list.blocks.front());
    ASSERT_NE(list, nullptr);
    std::size_t length = 0;
    for (const auto& item : list->items) length += inline_text(item).size();
    EXPECT_LE(length, max_block_text);
    std::string nested;
    for (int i = 0; i < 120; ++i) nested += "<ul><li>deep";
    for (int i = 0; i < 120; ++i) nested += "</li></ul>";
    EXPECT_LE(sanitize(nested).blocks.size(), max_blocks);
    const auto fixture_doc = sanitize(fixture("hostile-deep.html"));
    ASSERT_FALSE(fixture_doc.blocks.empty());
    EXPECT_LE(inline_text(std::get<Paragraph>(fixture_doc.blocks.front()).inlines).size(), max_block_text);
    EXPECT_LE(fixture_doc.blocks.size(), max_blocks);
}

}
}
