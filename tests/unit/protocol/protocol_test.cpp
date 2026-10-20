#include <braurser/protocol/dto.hpp>
#include <braurser/protocol/frame.hpp>
#include <braurser/protocol/methods.hpp>
#include <braurser/protocol/rpc.hpp>
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace {

thread_local bool count_allocations = false;
thread_local std::size_t allocation_count = 0;
thread_local std::size_t large_allocation_count = 0;

}

[[gnu::noinline]] void* operator new(std::size_t size) {
    if (count_allocations) {
        ++allocation_count;
        if (size >= 256 * 1024) ++large_allocation_count;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}

[[gnu::noinline]] void* operator new[](std::size_t size) { return ::operator new(size); }
[[gnu::noinline]] void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (const std::bad_alloc&) { return nullptr; }
}
[[gnu::noinline]] void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (const std::bad_alloc&) { return nullptr; }
}
[[gnu::noinline]] void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete[](void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace braurser::protocol {
namespace {

TEST(Protocol, LargeBuildFilesValidationAllocations) {
    const nlohmann::json payload{{"packageBase", "demo"},
        {"files", {{{"name", "PKGBUILD"}, {"text", std::string(448 * 1024, 'x')}, {"kind", "text"},
                    {"size", 448 * 1024}, {"sha256", std::string(64, 'a')}}}}, {"truncated", false}};
    allocation_count = 0;
    large_allocation_count = 0;
    count_allocations = true;
    const bool accepted = accepts<BuildFilesResult>(payload);
    count_allocations = false;
    const auto allocations = allocation_count;
    const auto copies = large_allocation_count;
    EXPECT_TRUE(accepted);
    EXPECT_EQ(allocations, 0U);
    EXPECT_EQ(copies, 0U);
    std::printf("BuildFilesResult validation: %zu allocations, %zu payload-sized copies\n", allocations, copies);
    for (const auto& descriptor : kMethods) {
        if (descriptor.result_type != "BuildFilesResult") continue;
        allocation_count = 0;
        count_allocations = true;
        const bool result = descriptor.validate_result(payload);
        count_allocations = false;
        const auto descriptor_allocations = allocation_count;
        EXPECT_TRUE(result);
        EXPECT_EQ(descriptor_allocations, 0U);
    }
}

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

template <class T>
bool parser_accepts(const nlohmann::json& value) {
    try {
        static_cast<void>(value.get<T>());
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

template <class T>
void check_validation(std::string_view name, std::string_view suffix) {
    const auto original = fixture(name, suffix);
    EXPECT_TRUE(accepts<T>(original));
    auto unknown = original;
    unknown["unexpected"] = false;
    EXPECT_FALSE(accepts<T>(unknown));
    auto over_limit = original;
    for (unsigned index = 0; index <= 256; ++index) over_limit["field" + std::to_string(index)] = false;
    EXPECT_FALSE(accepts<T>(over_limit));
    for (auto field = original.begin(); field != original.end(); ++field) {
        SCOPED_TRACE(field.key());
        auto missing = original;
        missing.erase(field.key());
        EXPECT_EQ(accepts<T>(missing), parser_accepts<T>(missing));
        const std::array<nlohmann::json, 12> replacements{{
            nullptr, true, -1, 0, 201, 1.5, "unexpected", std::string(4097, 'x'),
            nlohmann::json::array(), nlohmann::json::object(),
            nlohmann::json::array_t(257, nullptr), std::string("bad\x01", 4)
        }};
        for (const auto& replacement : replacements) {
            auto changed = original;
            changed[field.key()] = replacement;
            EXPECT_EQ(accepts<T>(changed), parser_accepts<T>(changed));
        }
    }
}

TEST(Protocol, EveryDescriptorRejectsUnknownFieldsAndLimitsAndMatchesParsing) {
#define CHECK_VALIDATION(name, params, result) \
    { SCOPED_TRACE(name); check_validation<params>(name, "params"); check_validation<result>(name, "result"); }
    BRAURSER_PROTOCOL_METHODS(CHECK_VALIDATION)
#undef CHECK_VALIDATION
#define CHECK_VALIDATION(name, params) \
    { SCOPED_TRACE(name); check_validation<params>(name, "params"); }
    BRAURSER_PROTOCOL_NOTIFICATIONS(CHECK_VALIDATION)
#undef CHECK_VALIDATION
}

TEST(Protocol, LargeBuildReviewSerializationOnlyAllocatesTheWireString) {
    BuildReviewQuestion review;
    review.pkgbuild = std::string(256 * 1024, 'x');
    large_allocation_count = 0;
    count_allocations = true;
    const nlohmann::json encoded = review;
    count_allocations = false;
    const auto copies = large_allocation_count;
    EXPECT_EQ(copies, 1U);
    EXPECT_EQ(encoded.at("pkgbuild").get_ref<const std::string&>().size(), 256 * 1024U);
    review.pkgbuild = std::string(256 * 1024 + 1, 'x');
    EXPECT_THROW(static_cast<void>(nlohmann::json(review)), std::invalid_argument);
}

TEST(Protocol, TaggedValidationDoesNotCopyPayloadOrLeakEnvelopeKeys) {
    auto payload = fixture("job.question", "params");
    payload["question"] = {{"question_id", "question"}, {"token", "token"},
        {"expires_at", "2026-09-25T00:00:00Z"}, {"kind", "build_review"},
        {"refs", nlohmann::json::array()}, {"pkgbuild", std::string(256 * 1024, 'x')}};
    allocation_count = 0;
    count_allocations = true;
    const bool accepted = accepts<JobQuestionNotification>(payload);
    count_allocations = false;
    const auto allocations = allocation_count;
    EXPECT_TRUE(accepted);
    EXPECT_EQ(allocations, 0U);
    large_allocation_count = 0;
    count_allocations = true;
    const auto parsed = payload.get<JobQuestionNotification>();
    count_allocations = false;
    const auto copies = large_allocation_count;
    EXPECT_EQ(copies, 1U);
    EXPECT_EQ(std::get<BuildReviewQuestion>(parsed.question.choice.value).pkgbuild->size(), 256 * 1024U);
    payload["question"]["unexpected"] = true;
    EXPECT_FALSE(accepts<JobQuestionNotification>(payload));
    payload["question"].erase("unexpected");
    payload["question"]["pkgbuild"] = std::string(256 * 1024 + 1, 'x');
    EXPECT_FALSE(accepts<JobQuestionNotification>(payload));
    EXPECT_FALSE(accepts<BuildReviewQuestion>({{"refs", nlohmann::json::array()}, {"kind", "build_review"}}));
    EXPECT_FALSE(accepts<Inline>({{"kind", "text"}, {"s", "text"}, {"question_id", "q"}}));
    EXPECT_FALSE(accepts<QuestionChoice>(payload.at("question")));
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

TEST(Protocol, BuildPinsAndPinnedFileRequestsValidateExactly) {
    for (const auto length : {40U, 64U}) {
        const BuildPin pin{"widget", {"widget", "widget-libs"}, std::string(length, 'a')};
        const nlohmann::json wire = pin;
        EXPECT_EQ(wire.get<BuildPin>().commit, pin.commit);
        EXPECT_TRUE(accepts<BuildFilesParams>(nlohmann::json(BuildFilesParams{"widget", pin.commit})));
        auto plan = fixture("jobs.plan", "result");
        plan["build_pins"] = nlohmann::json::array({wire});
        EXPECT_TRUE(accepts<TransactionPlan>(plan));
        plan["build_pins"] = nlohmann::json::array();
        for (unsigned index = 0; index < 17; ++index)
            plan["build_pins"].push_back(BuildPin{"widget" + std::to_string(index), {"widget"}, pin.commit});
        EXPECT_FALSE(accepts<TransactionPlan>(plan));
    }
    for (const auto& commit : {std::string(39, 'a'), std::string(40, 'A'), std::string(64, 'g')}) {
        const nlohmann::json wire{{"pkgbase", "widget"}, {"pkgnames", {"widget"}}, {"commit", commit}};
        EXPECT_FALSE(accepts<BuildPin>(wire));
        EXPECT_FALSE(accepts<BuildFilesParams>(nlohmann::json{{"packageId", "widget"}, {"commit", commit}}));
    }
    nlohmann::json wire{{"pkgbase", "../widget"}, {"pkgnames", {"widget"}}, {"commit", std::string(40, 'a')}};
    EXPECT_FALSE(accepts<BuildPin>(wire));
    wire["pkgbase"] = "widget";
    wire["pkgnames"] = {"bad/name"};
    EXPECT_FALSE(accepts<BuildPin>(wire));
    EXPECT_TRUE(accepts<BuildFilesParams>(nlohmann::json{{"packageId", "widget"}}));
}

TEST(Protocol, BuildFileKindsPathsAndBudgetsFailClosed) {
    const BuildFilesParams params{"widget", std::string(40, 'a'), "sub/build.sh"};
    const nlohmann::json request = params;
    EXPECT_EQ(request.get<BuildFilesParams>().path, params.path);
    for (const auto* path : {"", "/absolute", "../escape", "sub/../escape", "sub//file", "sub/", "sub\\file"}) {
        auto invalid = request;
        invalid["path"] = path;
        EXPECT_FALSE(accepts<BuildFilesParams>(invalid)) << path;
    }
    auto invalid = request;
    invalid.erase("commit");
    EXPECT_FALSE(accepts<BuildFilesParams>(invalid));
    for (const auto* kind : {"text", "binary", "omitted"}) {
        const BuildFile file{"sub/build.sh", std::string_view(kind) == "text" ? "x" : "", kind, 1, std::string(64, 'a')};
        const nlohmann::json wire = file;
        EXPECT_EQ(wire.get<BuildFile>().kind, kind);
        EXPECT_TRUE(accepts<BuildFile>(wire));
        for (const auto* field : {"kind", "size", "sha256"}) {
            auto missing = wire; missing.erase(field);
            EXPECT_FALSE(accepts<BuildFile>(missing));
        }
    }
    auto file = nlohmann::json(BuildFile{"build.sh", "x", "text", 1, std::string(64, 'a')});
    for (const auto* digest : {"", "abcd", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg"}) {
        auto bad = file; bad["sha256"] = digest; EXPECT_FALSE(accepts<BuildFile>(bad));
    }
    auto bad = file; bad["kind"] = "placeholder"; EXPECT_FALSE(accepts<BuildFile>(bad));
    bad = file; bad["name"] = "../build.sh"; EXPECT_FALSE(accepts<BuildFile>(bad));
    bad = file; bad["size"] = -1; EXPECT_FALSE(accepts<BuildFile>(bad));
    bad = file; bad["size"] = 2; EXPECT_FALSE(accepts<BuildFile>(bad));
    bad = file; bad["kind"] = "binary"; EXPECT_FALSE(accepts<BuildFile>(bad));
    bad = file; bad["size"] = 448 * 1024 + 1; bad["text"] = std::string(448 * 1024 + 1, 'x');
    EXPECT_FALSE(accepts<BuildFile>(bad));
    EXPECT_THROW(validate_wire(nlohmann::json{{"text", std::string(512 * 1024, 'x')}}), std::invalid_argument);
    const nlohmann::json duplicate{{"packageBase", "widget"}, {"files", {file, file}}, {"truncated", false}};
    EXPECT_FALSE(accepts<BuildFilesResult>(duplicate));
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

TEST(Protocol, EnvelopePayloadsSurviveParsedDomDestruction) {
    const Request request{std::string("request"), "catalog.search", fixture("catalog.search", "params")};
    auto parsed = parse_message(serialize_message(request));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::get<Request>(*parsed).params, request.params);
    const Notification notification{"job.log", JobLogNotification{"job", 1, std::string(4000, 'x')}};
    parsed = parse_message(serialize_message(notification));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::get<Notification>(*parsed).params, notification.params);
    nlohmann::json payload = nlohmann::json::array();
    for (int i = 0; i < 64; ++i) payload.push_back({{"text", std::string(4000, 'x')}, {"index", i}});
    parsed = parse_message(serialize_message(Response{std::string("response"), payload}));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::get<nlohmann::json>(std::get<Response>(*parsed).outcome), payload);
}

TEST(Protocol, SerializationRevalidatesNodeAndStringBudgets) {
    const Response excessive{1, nlohmann::json{{"body", std::string(512 * 1024 + 1, 'x')}}};
    EXPECT_THROW(serialize_message(excessive), std::invalid_argument);
    const Request unknown{1, "unknown.method", nlohmann::json::object()};
    EXPECT_THROW(serialize_message(unknown), std::invalid_argument);
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
