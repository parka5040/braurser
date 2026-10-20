#include <braurser/presenter/rpc_client.hpp>
#include <braurser/protocol/frame.hpp>
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <deque>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include <sys/un.h>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace braurser::presenter {
namespace {
class TimeoutRpc final : public Rpc {
public:
    std::chrono::milliseconds timeout{};
    void request(std::string_view, nlohmann::json, Callback, std::chrono::milliseconds value) override {
        timeout = value;
    }
    Subscription subscribe(std::string_view, std::function<void(const nlohmann::json&)>) override { return {}; }
};

class MainLoop final : public Dispatcher {
public:
    void post(std::function<void()> callback) override { std::lock_guard lock(mutex_); callbacks_.push_back(std::move(callback)); }
    bool until(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            drain(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drain(); return done();
    }
    std::size_t queued() { std::lock_guard lock(mutex_); return callbacks_.size(); }
    void drain() {
        std::deque<std::function<void()>> callbacks;
        { std::lock_guard lock(mutex_); callbacks.swap(callbacks_); }
        for (auto& callback : callbacks) { callback(); }
    }
private:
    std::mutex mutex_;
    std::deque<std::function<void()>> callbacks_;
};
class Pairs final : public SocketConnector {
public:
    ~Pairs() override { for (const auto socket : sockets_) { ::close(socket); } }
    int add() {
        std::array<int, 2> sockets{};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) != 0) { throw std::runtime_error("socketpair failed"); }
        std::lock_guard lock(mutex_); sockets_.push_back(sockets[0]); return sockets[1];
    }
    std::expected<int, int> connect() override {
        std::lock_guard lock(mutex_); ++attempts;
        if (sockets_.empty()) { return std::unexpected(ENOENT); }
        const int socket = sockets_.front(); sockets_.pop_front(); return socket;
    }
    std::atomic<unsigned> attempts = 0;
private:
    std::mutex mutex_;
    std::deque<int> sockets_;
};
nlohmann::json payload(std::string_view method, std::string_view suffix) {
    std::ifstream input(std::string(BRAURSER_FIXTURES) + "/" + std::string(method) + "." + std::string(suffix) + ".json");
    return nlohmann::json::parse(input);
}
class FakeServer {
public:
    enum class Mode { Reply, Silent, Oversized, Drop };
    FakeServer(int socket, Mode mode = Mode::Reply) : socket_(socket), worker_([this, mode](std::stop_token stop) {
        protocol::FrameDecoder decoder;
        while (!stop.stop_requested()) {
            pollfd descriptor{socket_, POLLIN, 0};
            if (::poll(&descriptor, 1, 10) <= 0) { continue; }
            std::array<std::byte, 8192> input{};
            const auto count = ::recv(socket_, input.data(), input.size(), 0);
            if (count <= 0) { disconnected.store(true); break; }
            decoder.feed(std::span(input.data(), static_cast<std::size_t>(count)));
            while (true) {
                auto frame = decoder.next(); if (!frame || !*frame) { break; }
                auto message = protocol::parse_message(**frame); if (!message) { break; }
                const auto* request = std::get_if<protocol::Request>(&*message); if (!request) { continue; }
                ++received;
                if (mode == Mode::Silent) { continue; }
                if (mode == Mode::Drop) { ::shutdown(socket_, SHUT_RDWR); return; }
                if (mode == Mode::Oversized) { send_bytes(std::string("\x00\x10\x00\x01", 4)); continue; }
                send_bytes(protocol::encode_frame(protocol::serialize_message(protocol::Response{request->id, payload(request->method, "result")})));
            }
        }
    }) {}
    ~FakeServer() { worker_.request_stop(); worker_.join(); ::close(socket_); }
    void send_bytes(const std::string& bytes) {
        std::lock_guard lock(write_mutex_);
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::send(socket_, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (count <= 0) { return; }
            offset += static_cast<std::size_t>(count);
        }
    }
    std::atomic<unsigned> received = 0;
    std::atomic_bool disconnected = false;
private:
    int socket_;
    std::mutex write_mutex_;
    std::jthread worker_;
};
RpcOptions options() { RpcOptions value; value.launch_daemon = false; value.initial_backoff = std::chrono::milliseconds(10); return value; }
}
TEST(RpcClient, PlanningAndMutationCallsAllowThePlanningDeadline) {
    TimeoutRpc rpc;
    const auto check = [&]<class Params>() {
        using Descriptor = Method<Params>;
        const auto params = payload(Descriptor::name, "params").template get<Params>();
        rpc.call<Descriptor>(params, [](auto) {});
        constexpr bool planning = Descriptor::name == Method<protocol::PlanParams>::name ||
            Descriptor::name == Method<protocol::UpdatesPlanParams>::name ||
            Descriptor::name == Method<protocol::InstallParams>::name ||
            Descriptor::name == Method<protocol::RemoveParams>::name ||
            Descriptor::name == Method<protocol::UpgradeSystemParams>::name ||
            Descriptor::name == Method<protocol::UpgradeCommunityParams>::name ||
            Descriptor::name == Method<protocol::ManualInstallParams>::name ||
            Descriptor::name == Method<protocol::ManualUpgradeParams>::name ||
            Descriptor::name == Method<protocol::ManualRemoveParams>::name ||
            Descriptor::name == Method<protocol::SetupHelperParams>::name;
        EXPECT_EQ(rpc.timeout, std::chrono::seconds(planning ? 200 : 30)) << Descriptor::name;
    };
#define BRAURSER_CHECK_TIMEOUT(name, params, result) check.operator()<protocol::params>();
    BRAURSER_PROTOCOL_METHODS(BRAURSER_CHECK_TIMEOUT)
#undef BRAURSER_CHECK_TIMEOUT
    rpc.call<Method<protocol::PlanParams>>({.kind = "install", .packageId = "target"}, [](auto) {},
        std::chrono::seconds(5));
    EXPECT_EQ(rpc.timeout, std::chrono::seconds(5));
}

TEST(RpcClient, AllGoldenMethodsUseRealFramingAndDispatcher) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>(); FakeServer server(pairs->add()); RpcClient client(loop, options(), pairs);
    const auto main_thread = std::this_thread::get_id(); std::size_t completed = 0;
    for (const auto& method : protocol::kMethods) {
        client.request(method.name, payload(method.name, "params"), [&, method](Reply<nlohmann::json> reply) {
            EXPECT_EQ(std::this_thread::get_id(), main_thread); ASSERT_TRUE(reply) << method.name;
            EXPECT_EQ(*reply, payload(method.name, "result")); ++completed;
        });
    }
    ASSERT_TRUE(loop.until([&] { return completed == protocol::kMethods.size(); }));
    EXPECT_EQ(server.received.load(), protocol::kMethods.size());
}
TEST(RpcClient, TypedHelperAndNotificationSubscription) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>(); FakeServer server(pairs->add()); RpcClient client(loop, options(), pairs);
    bool completed = false;
    client.call<Method<protocol::HelloParams>>({protocol::kApiVersion}, [&](Reply<protocol::HelloResult> reply) { ASSERT_TRUE(reply); EXPECT_EQ(reply->apiVersion, protocol::kApiVersion); completed = true; });
    ASSERT_TRUE(loop.until([&] { return completed; }));
    std::size_t events = 0;
    auto subscription = client.listen<protocol::CatalogChangedNotification>([&](const auto& event) { EXPECT_EQ(event.revision, 1U); ++events; });
    const auto frame = protocol::encode_frame(protocol::serialize_message(protocol::Notification{std::string(Event<protocol::CatalogChangedNotification>::name), payload(Event<protocol::CatalogChangedNotification>::name, "params")}));
    server.send_bytes(frame); ASSERT_TRUE(loop.until([&] { return events == 1; }));
    subscription.reset(); server.send_bytes(frame); std::this_thread::sleep_for(std::chrono::milliseconds(30)); loop.drain(); EXPECT_EQ(events, 1U);
}
TEST(RpcClient, RequestTimeoutDoesNotReplayTransaction) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>(); FakeServer server(pairs->add(), FakeServer::Mode::Silent); RpcClient client(loop, options(), pairs);
    bool expired = false;
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_FALSE(reply); expired = true; }, std::chrono::milliseconds(40));
    ASSERT_TRUE(loop.until([&] { return expired; })); EXPECT_EQ(server.received.load(), 1U);
}
TEST(RpcClient, OversizeFrameFailsClosed) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>(); FakeServer server(pairs->add(), FakeServer::Mode::Oversized); RpcClient client(loop, options(), pairs);
    bool failed = false;
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_FALSE(reply); failed = true; });
    ASSERT_TRUE(loop.until([&] { return failed; }));
}
TEST(RpcClient, ReconnectsWithoutReplayingSentRequests) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>(); FakeServer first(pairs->add(), FakeServer::Mode::Drop); FakeServer second(pairs->add()); RpcClient client(loop, options(), pairs);
    bool failed = false;
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_FALSE(reply); failed = true; });
    ASSERT_TRUE(loop.until([&] { return failed; }));
    bool completed = false;
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_TRUE(reply); completed = true; });
    ASSERT_TRUE(loop.until([&] { return completed; })); EXPECT_GE(pairs->attempts.load(), 2U); EXPECT_EQ(second.received.load(), 1U);
}
TEST(RpcClient, RejectsSymlinkAndConnectsToOwnedSocket) {
    auto directory_template = (std::filesystem::temp_directory_path() / "braurser-rpc-XXXXXX").string();
    ASSERT_NE(::mkdtemp(directory_template.data()), nullptr);
    const std::filesystem::path directory(directory_template);
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(directory, error); }
    } cleanup{directory};
    const auto target = (directory / "core.sock").string();
    ASSERT_LT(target.size(), sizeof(sockaddr_un::sun_path));
    const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0 && (errno == EPERM || errno == EACCES)) { GTEST_SKIP() << "OS policy forbids filesystem sockets; socketpair transport tests still run"; }
    ASSERT_GE(listener, 0) << errno;
    struct Close { int descriptor; ~Close() { ::close(descriptor); } } close{listener};
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    std::ranges::copy(target, address.sun_path);
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);
    const auto link = directory / "linked.sock"; std::filesystem::create_symlink(target, link);
    MainLoop loop; auto configuration = options(); configuration.socket_path = link.string();
    RpcClient client(loop, configuration); bool failed = false;
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_FALSE(reply); failed = true; }, std::chrono::milliseconds(40));
    ASSERT_TRUE(loop.until([&] { return failed; }));
    const int accepted = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    EXPECT_EQ(accepted, -1);
    if (accepted >= 0) { ::close(accepted); }
    configuration.socket_path = target;
    RpcClient owned(loop, configuration); bool completed = false;
    owned.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_TRUE(reply); completed = true; });
    pollfd ready{listener, POLLIN, 0}; ASSERT_EQ(::poll(&ready, 1, 1000), 1);
    const int connected = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC); ASSERT_GE(connected, 0);
    FakeServer server(connected);
    EXPECT_TRUE(loop.until([&] { return completed; }));
}
TEST(RpcClient, FinishedJobsDoNotExhaustTheEventTracker) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>(); FakeServer server(pairs->add()); RpcClient client(loop, options(), pairs);
    bool connected = false;
    auto watch = client.connection_changed([&](bool value) { if (value) { connected = true; } });
    ASSERT_TRUE(loop.until([&] { return connected; }));
    std::size_t states = 0;
    auto subscription = client.listen<protocol::JobStateNotification>([&](const auto&) { ++states; });
    std::string frames;
    for (int index = 0; index < 1100; ++index) {
        frames += protocol::encode_frame(protocol::serialize_message(protocol::Notification{std::string(Event<protocol::JobStateNotification>::name),
            protocol::JobStateNotification{"job-" + std::to_string(index), 1, {"succeeded"}}}));
        if (index % 100 == 99) {
            server.send_bytes(frames); frames.clear();
            ASSERT_TRUE(loop.until([&] { return states == static_cast<std::size_t>(index + 1); }));
        }
    }
    ASSERT_TRUE(loop.until([&] { return states == 1100; }));
    bool completed = false;
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_TRUE(reply); completed = true; });
    ASSERT_TRUE(loop.until([&] { return completed; }));
    EXPECT_EQ(pairs->attempts.load(), 1U);
}
TEST(RpcClient, ReconnectStartsFreshEventSequences) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>();
    auto first = std::make_unique<FakeServer>(pairs->add()); FakeServer second(pairs->add());
    RpcClient client(loop, options(), pairs);
    std::vector<std::uint64_t> sequences;
    auto subscription = client.listen<protocol::JobLogNotification>([&](const auto& event) { sequences.push_back(event.sequence); });
    const auto log = [](std::uint64_t sequence) {
        return protocol::encode_frame(protocol::serialize_message(protocol::Notification{std::string(Event<protocol::JobLogNotification>::name),
            protocol::JobLogNotification{"job", sequence, "line"}}));
    };
    first->send_bytes(log(5));
    ASSERT_TRUE(loop.until([&] { return sequences.size() == 1; }));
    first.reset();
    ASSERT_TRUE(loop.until([&] { return pairs->attempts.load() >= 2; }));
    second.send_bytes(log(1));
    ASSERT_TRUE(loop.until([&] { return sequences.size() == 2; }));
    EXPECT_EQ(sequences, (std::vector<std::uint64_t>{5, 1}));
    bool completed = false;
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_TRUE(reply); completed = true; });
    ASSERT_TRUE(loop.until([&] { return completed; }));
}
TEST(RpcClient, NotificationBacklogIsBoundedAndReconnects) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>();
    FakeServer first(pairs->add()); FakeServer second(pairs->add()); RpcClient client(loop, options(), pairs);
    bool connected = false; bool disconnected = false;
    auto watch = client.connection_changed([&](bool value) { if (value) { connected = true; } else { disconnected = true; } });
    ASSERT_TRUE(loop.until([&] { return connected; }));
    std::size_t delivered = 0;
    auto subscription = client.listen<protocol::CatalogChangedNotification>([&](const auto&) { ++delivered; });
    const auto frame = protocol::encode_frame(protocol::serialize_message(protocol::Notification{
        std::string(Event<protocol::CatalogChangedNotification>::name), protocol::CatalogChangedNotification{1}}));
    std::string frames;
    for (int index = 0; index < 1100; ++index) { frames += frame; }
    first.send_bytes(frames);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!first.disconnected.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    EXPECT_TRUE(first.disconnected.load()); EXPECT_LE(loop.queued(), 1024U);
    ASSERT_TRUE(loop.until([&] { return disconnected && pairs->attempts.load() >= 2; }));
    EXPECT_LE(delivered, 1024U); EXPECT_GT(delivered, 0U);
    bool completed = false;
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { EXPECT_TRUE(reply); completed = true; });
    ASSERT_TRUE(loop.until([&] { return completed; }));
    const auto before = delivered; second.send_bytes(frame);
    EXPECT_TRUE(loop.until([&] { return delivered == before + 1; }));
}
TEST(RpcClient, NotificationPayloadBytesAreBounded) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>(); FakeServer server(pairs->add()); RpcClient client(loop, options(), pairs);
    bool connected = false;
    auto watch = client.connection_changed([&](bool value) { if (value) { connected = true; } });
    ASSERT_TRUE(loop.until([&] { return connected; }));
    std::size_t delivered = 0;
    auto subscription = client.listen<protocol::JobQuestionNotification>([&](const auto&) { ++delivered; });
    auto event = payload("job.question", "params");
    event["question"] = {{"question_id", "question"}, {"token", "token"}, {"expires_at", "2026-09-25T00:00:00Z"},
        {"kind", "build_review"}, {"refs", nlohmann::json::array()}, {"pkgbuild", std::string(256 * 1024, 'x')}};
    for (std::uint64_t sequence = 1; sequence <= 140; ++sequence) {
        event["sequence"] = sequence;
        server.send_bytes(protocol::encode_frame(protocol::serialize_message(protocol::Notification{
            std::string(Event<protocol::JobQuestionNotification>::name), event})));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server.disconnected.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    EXPECT_TRUE(server.disconnected.load()); EXPECT_LT(loop.queued(), 140U);
    loop.drain(); EXPECT_GT(delivered, 0U); EXPECT_LE(delivered, 127U);
}
TEST(RpcClient, BoundsQueueAndRejectsUnknownMethods) {
    MainLoop loop; auto pairs = std::make_shared<Pairs>(); auto configuration = options(); configuration.queue_limit = 1;
    RpcClient client(loop, configuration, pairs); bool full = false; bool unknown = false;
    client.call<Method<protocol::StatusParams>>({}, [](auto) {});
    client.call<Method<protocol::StatusParams>>({}, [&](Reply<protocol::StatusResult> reply) { ASSERT_FALSE(reply); EXPECT_EQ(reply.error().code, protocol::kBusy); full = true; });
    client.request("unknown.method", nlohmann::json::object(), [&](Reply<nlohmann::json> reply) { ASSERT_FALSE(reply); unknown = true; });
    EXPECT_TRUE(loop.until([&] { return full && unknown; }));
}
}
