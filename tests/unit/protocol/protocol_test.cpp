#include <braurser/protocol/dto.hpp>
#include <braurser/protocol/frame.hpp>
#include <braurser/protocol/methods.hpp>
#include <braurser/protocol/rpc.hpp>
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace braurser::protocol {
namespace {

nlohmann::json fixture(std::string_view name, std::string_view suffix) {
    const auto path = std::filesystem::path(PROTOCOL_FIXTURES) /
                      (std::string(name) + "." + std::string(suffix) + ".json");
    std::ifstream file(path);
    if (!file) throw std::runtime_error("missing fixture: " + path.string());
    return nlohmann::json::parse(file);
}

template <class T>
void check_fixture(std::string_view name, std::string_view suffix) {
    const auto original = fixture(name, suffix);
    const auto typed = original.get<T>();
    const nlohmann::json encoded = typed;
    EXPECT_EQ(encoded, original);
}

TEST(Protocol, EveryMethodAndNotificationHasRoundTripFixture) {
#define CHECK_METHOD(name, params, result) \
    { SCOPED_TRACE(name); check_fixture<params>(name, "params"); check_fixture<result>(name, "result"); \
      EXPECT_TRUE(parse_message(serialize_message(Message(Request{std::int64_t{1}, name, fixture(name, "params")})))); \
      EXPECT_TRUE(parse_message(serialize_message(Message(Response{std::int64_t{1}, fixture(name, "result")})))); }
    BRAURSER_PROTOCOL_METHODS(CHECK_METHOD)
#undef CHECK_METHOD
#define CHECK_NOTIFICATION(name, params) \
    { SCOPED_TRACE(name); check_fixture<params>(name, "params"); \
      EXPECT_TRUE(parse_message(serialize_message(Message(Notification{name, fixture(name, "params")})))); }
    BRAURSER_PROTOCOL_NOTIFICATIONS(CHECK_NOTIFICATION)
#undef CHECK_NOTIFICATION
    EXPECT_EQ(kMethods.size(), 37U);
    EXPECT_EQ(kNotifications.size(), 7U);
}

TEST(Protocol, PlansRequireBoundApprovalAndRejectLooseMutationInputs) {
    auto plan = fixture("jobs.plan", "params");
    EXPECT_TRUE(accepts<PlanParams>(plan));
    plan["kind"] = "unexpected";
    EXPECT_FALSE(accepts<PlanParams>(plan));
    plan = fixture("jobs.plan", "params");
    plan.erase("packageId");
    EXPECT_FALSE(accepts<PlanParams>(plan));
    auto mutation = fixture("jobs.install", "params");
    EXPECT_TRUE(accepts<InstallParams>(mutation));
    mutation["approvalId"] = "loose";
    EXPECT_FALSE(accepts<InstallParams>(mutation));
    mutation.erase("approvalId");
    mutation.erase("approvalToken");
    EXPECT_FALSE(accepts<InstallParams>(mutation));
    auto status = fixture("system.status", "result");
    status["catalogState"] = "injected";
    EXPECT_FALSE(accepts<StatusResult>(status));
    auto search = fixture("catalog.search", "params");
    search["sort"] = "popularity";
    EXPECT_FALSE(accepts<SearchParams>(search));
    search["sort"] = "name";
    search["category"] = "Utility";
    EXPECT_TRUE(accepts<SearchParams>(search));
}

TEST(Protocol, ShadowedRepositoriesAndRefreshStateRoundTrip) {
    PackageSummary summary{{{"official"}, "extra", "demo"}, "1", "Description", false};
    summary.alsoIn = std::vector<std::string>{"core"};
    summary.shadowed = true;
    const nlohmann::json summary_wire = summary;
    EXPECT_EQ(summary_wire.at("alsoIn"), nlohmann::json::array({"core"}));
    EXPECT_EQ(summary_wire.at("shadowed"), true);
    EXPECT_EQ(summary_wire.get<PackageSummary>().alsoIn, summary.alsoIn);
    PackageDetails details{summary.package, "1", "Description", {}, {}};
    details.alsoIn = summary.alsoIn;
    details.shadowed = true;
    const nlohmann::json details_wire = details;
    EXPECT_EQ(details_wire.get<PackageDetails>().shadowed, true);
    auto invalid = details_wire;
    invalid["alsoIn"] = nlohmann::json::array({"bad/repo"});
    EXPECT_FALSE(accepts<PackageDetails>(invalid));
    SearchResult search;
    search.catalogState = "refreshing";
    const nlohmann::json search_wire = search;
    EXPECT_EQ(search_wire.get<SearchResult>().catalogState, "refreshing");
    invalid = search_wire;
    invalid["catalogState"] = "injected";
    EXPECT_FALSE(accepts<SearchResult>(invalid));
    BuildReviewQuestion review;
    review.pkgbuild = "pkgname=demo\n";
    const nlohmann::json review_wire = review;
    EXPECT_EQ(review_wire.get<BuildReviewQuestion>().pkgbuild, review.pkgbuild);
}

TEST(Protocol, FrameHandlesPartialFeedsAndConsecutiveMessages) {
    const std::string first = "{\"jsonrpc\":\"2.0\",\"id\":1,\"apiVersion\":1,\"method\":\"system.status\",\"params\":{}}";
    const std::string second = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}";
    const auto frames = encode_frame(first) + encode_frame(second);
    FrameDecoder decoder;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        decoder.feed(std::as_bytes(std::span(frames.data() + i, 1)));
        auto frame = decoder.next();
        ASSERT_TRUE(frame.has_value());
        if (frame->has_value()) {
            EXPECT_EQ(**frame, i < encode_frame(first).size() ? first : second);
        }
    }
    EXPECT_FALSE(decoder.next()->has_value());
}

TEST(Protocol, FrameAcceptsMaximumFrameFollowedByNextFrameInOneFeed) {
    const std::string large = "\"" + std::string(kMaxFrameBytes - 2, 'a') + "\"";
    const std::string small = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}";
    const auto frames = encode_frame(large) + encode_frame(small);
    const auto tail = encode_frame(large);
    FrameDecoder decoder;
    decoder.feed(std::as_bytes(std::span(frames.data(), frames.size())));
    decoder.feed(std::as_bytes(std::span(tail.data(), 4096)));
    auto first = decoder.next();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ(**first, large);
    auto second = decoder.next();
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->has_value());
    EXPECT_EQ(**second, small);
    decoder.feed(std::as_bytes(std::span(tail.data() + 4096, tail.size() - 4096)));
    auto third = decoder.next();
    ASSERT_TRUE(third.has_value());
    ASSERT_TRUE(third->has_value());
    EXPECT_EQ(**third, large);
    EXPECT_FALSE(decoder.next()->has_value());
}

TEST(Protocol, FrameBoundsUndrainedBuffer) {
    const std::string large = "\"" + std::string(kMaxFrameBytes - 2, 'a') + "\"";
    const auto frame = encode_frame(large);
    FrameDecoder decoder;
    for (int i = 0; i < 3; ++i) decoder.feed(std::as_bytes(std::span(frame.data(), frame.size())));
    EXPECT_FALSE(decoder.next().has_value());
}

TEST(Protocol, FrameRejectsOversizeAndZeroBeforeReadingBody) {
    for (const std::array<std::byte, 4> prefix : {
             std::array{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}},
             std::array{std::byte{0}, std::byte{0x10}, std::byte{0}, std::byte{1}}}) {
        FrameDecoder decoder;
        decoder.feed(prefix);
        ASSERT_FALSE(decoder.next().has_value());
    }
    EXPECT_THROW(encode_frame(""), std::invalid_argument);
    EXPECT_THROW(encode_frame(std::string(kMaxFrameBytes + 1, 'x')), std::invalid_argument);
}

TEST(Protocol, EnvelopeDistinguishesRequestResponseAndNotification) {
    const Request request{std::int64_t{42}, "system.hello", fixture("system.hello", "params")};
    auto parsed = parse_message(serialize_message(Message(request)));
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(std::holds_alternative<Request>(*parsed));
    EXPECT_EQ(std::get<Request>(*parsed).id, request.id);
    const Response response{std::string("a"), ErrorObject{kOffline, "error.offline", nlohmann::json::object()}};
    parsed = parse_message(serialize_message(Message(response)));
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(std::holds_alternative<Response>(*parsed));
    const Notification note{"job.state", fixture("job.state", "params")};
    parsed = parse_message(serialize_message(Message(note)));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(std::holds_alternative<Notification>(*parsed));
}

TEST(Protocol, EnvelopeRejectsMalformedOrUntrustedInputs) {
    const auto expect_code = [](std::string_view body, int code) {
        const auto result = parse_message(body);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, code);
    };
    expect_code("{", kParseError);
    expect_code("{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":{}}", kInvalidRequest);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{},\"extra\":1}", kInvalidRequest);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"id\":2,\"result\":{}}", kInvalidRequest);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":18446744073709551615,\"result\":{}}", kInvalidRequest);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"apiVersion\":4294967297,\"method\":\"system.status\",\"params\":{}}", kInvalidRequest);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":{}}", kInvalidRequest);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"system.status\",\"params\":{}}", kInvalidRequest);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"apiVersion\":1,\"method\":\"unknown\",\"params\":{}}", kMethodNotFound);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"apiVersion\":1,\"method\":\"system.status\",\"params\":{\"extra\":0}}", kInvalidParams);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{},\"error\":{}}", kInvalidRequest);
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32001,\"message_key\":\"raw stderr\",\"args\":{}}}", kInvalidRequest);
    std::string nested = "null";
    for (int i = 0; i < 33; ++i) nested = "[" + nested + "]";
    expect_code("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":" + nested + "}", kInvalidRequest);
}

TEST(Protocol, JobEventsHaveIndependentMonotonicSequences) {
    EventSequenceTracker tracker;
    EXPECT_TRUE(tracker.observe("job-a", 1));
    EXPECT_TRUE(tracker.observe("job-b", 1));
    EXPECT_TRUE(tracker.observe("job-a", 3));
    EXPECT_FALSE(tracker.observe("job-a", 2));
    EXPECT_FALSE(tracker.observe("job-b", 1));
    EXPECT_FALSE(tracker.observe("job-a", 0));
    tracker.forget("job-a");
    EXPECT_TRUE(tracker.observe("job-a", 1));
}

TEST(Protocol, TaggedWikiAndQuestionsValidateEveryVariant) {
    const std::array<nlohmann::json, 6> blocks{{
        {{"kind", "heading"}, {"level", 3}, {"text", "Title"}},
        {{"kind", "paragraph"}, {"inlines", {{{"kind", "text"}, {"s", "Some text"}, {"emphasis", false}, {"strong", true}, {"code", false}},
                                             {{"kind", "link"}, {"label", "docs"}, {"href", "https://example.org/docs"}}}}},
        {{"kind", "list"}, {"ordered", true}, {"items", nlohmann::json::array()}},
        {{"kind", "code"}, {"text", "hello"}},
        {{"kind", "note"}, {"inlines", nlohmann::json::array()}},
        {{"kind", "warning"}, {"inlines", nlohmann::json::array()}}
    }};
    for (const auto& block : blocks) EXPECT_EQ(nlohmann::json(block.get<WikiBlock>()), block);
    const std::array<nlohmann::json, 6> choices{{
        {{"kind", "variant"}, {"options", nlohmann::json::array()}, {"urls_differ", false}},
        {{"kind", "provider"}, {"options", nlohmann::json::array()}},
        {{"kind", "build_review"}, {"refs", nlohmann::json::array()}},
        {{"kind", "conflict"}, {"a", {{"source", "official"}, {"name", "sample"}}}, {"b", {{"source", "community"}, {"name", "sample"}}}},
        {{"kind", "news_ack"}, {"items", nlohmann::json::array()}},
        {{"kind", "git_redirect"}, {"from_host", "example.org"}, {"to_host", "example.net"}}
    }};
    for (auto choice : choices) {
        choice["question_id"] = "question";
        choice["token"] = "token";
        choice["expires_at"] = "2026-09-25T00:00:00Z";
        EXPECT_EQ(nlohmann::json(choice.get<Question>()), choice);
    }
    EXPECT_FALSE(accepts<WikiBlock>({{"kind", "heading"}, {"level", 5}, {"text", "bad"}}));
    EXPECT_FALSE(accepts<Question>({{"question_id", "q"}, {"token", "t"}, {"expires_at", "2026-09-25T00:00:00Z"}, {"kind", "unknown"}}));
}

TEST(Protocol, DTORejectsBoundsEnumsPackagesAndInsecureUrls) {
    auto search = fixture("catalog.search", "params");
    search["text"] = std::string(16385, 'x');
    EXPECT_FALSE(accepts<SearchParams>(search));
    search = fixture("catalog.search", "params");
    search["limit"] = 201;
    EXPECT_FALSE(accepts<SearchParams>(search));
    search["limit"] = 20;
    search["filters"]["unexpected"] = nlohmann::json::array();
    for (int i = 0; i < 257; ++i) search["filters"]["unexpected"].push_back("extra");
    EXPECT_FALSE(accepts<SearchParams>(search));
    search = fixture("catalog.search", "params");
    search["filters"]["aurOnly"] = true;
    search["filters"]["repoOnly"] = true;
    EXPECT_FALSE(accepts<SearchParams>(search));
    auto hello = fixture("system.hello", "result");
    hello["capabilities"]["init"] = "malicious";
    EXPECT_FALSE(accepts<HelloResult>(hello));
    hello["capabilities"]["init"] = "openrc systemd";
    EXPECT_FALSE(accepts<HelloResult>(hello));
    auto details = fixture("catalog.get", "result");
    details["details"]["package"]["name"] = "-invalid";
    EXPECT_FALSE(accepts<GetResult>(details));
    auto manual = fixture("manual.preview", "params");
    manual["httpsUrl"] = "http://example.org/repo";
    EXPECT_FALSE(accepts<ManualPreviewParams>(manual));
    manual["httpsUrl"] = "https://user:password@example.org/repo";
    EXPECT_FALSE(accepts<ManualPreviewParams>(manual));
    manual["httpsUrl"] = "https://127.0.0.1/repo";
    EXPECT_FALSE(accepts<ManualPreviewParams>(manual));
    manual["httpsUrl"] = "https://example.org/repo";
    manual["scriptPath"] = "../install.sh";
    EXPECT_FALSE(accepts<ManualPreviewParams>(manual));
    manual["scriptPath"] = "install.sh";
    manual["scope"] = "root";
    EXPECT_FALSE(accepts<ManualPreviewParams>(manual));
    search = fixture("catalog.search", "params");
    search["limit"] = -1;
    EXPECT_FALSE(accepts<SearchParams>(search));
    search["limit"] = 4294967297ULL;
    EXPECT_FALSE(accepts<SearchParams>(search));
    auto settings = fixture("settings.set", "params");
    settings["changedFields"]["theme"] = "untrusted";
    EXPECT_FALSE(accepts<SettingsSetParams>(settings));
    auto wiki = fixture("wiki.get", "result");
    wiki["document"]["blocks"] = {{{"kind", "paragraph"}, {"inlines", {{{"kind", "link"}, {"label", "bad"}, {"href", "http://example.org"}}}}}};
    EXPECT_FALSE(accepts<WikiGetResult>(wiki));
}

}
}
