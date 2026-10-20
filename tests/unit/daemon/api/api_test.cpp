#include <braurser/daemon/api/broadcaster.hpp>
#include "services.hpp"
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/api/logger.hpp>
#include <braurser/daemon/cache/store.hpp>
#include <braurser/daemon/cache/database.hpp>
#include <braurser/daemon/api/registry.hpp>
#include <braurser/daemon/api/server.hpp>
#include <braurser/protocol/frame.hpp>
#include <braurser/protocol/rpc.hpp>
#include <gtest/gtest.h>
#include <git2.h>
#include <giomm/init.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <future>
#include <fstream>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
namespace api = braurser::daemon::api;
namespace protocol = braurser::protocol;

struct Runtime {
    Runtime() {
        auto pattern = (std::filesystem::temp_directory_path() / "braurser-api-XXXXXX").string();
        auto* created = mkdtemp(pattern.data());
        if (created) path = created;
    }
    ~Runtime() { if (!path.empty()) std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

struct DenyPeer final : api::PeerCredentials {
    bool allowed(int, uid_t) const override { return false; }
};

struct RunningServer {
    explicit RunningServer(api::Server& target) : server(target) {
        auto ready = std::make_shared<std::promise<void>>();
        auto started = ready->get_future();
        thread = std::thread([this, ready] {
            Glib::signal_idle().connect([ready] { ready->set_value(); return false; });
            server.run();
        });
        started.wait();
    }
    ~RunningServer() {
        Glib::MainContext::get_default()->invoke([this] { server.stop(); return false; });
        thread.join();
    }
    api::Server& server;
    std::thread thread;
};

int connect_client(const std::filesystem::path& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const auto text = path.string();
    if (text.size() >= sizeof(addr.sun_path)) { close(fd); return -1; }
    std::memcpy(addr.sun_path, text.c_str(), text.size() + 1);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool read_exact(int fd, void* data, std::size_t size) {
    auto* destination = static_cast<char*>(data);
    while (size) {
        pollfd entry{fd, POLLIN, 0};
        if (poll(&entry, 1, 3000) <= 0) return false;
        const auto got = recv(fd, destination, size, 0);
        if (got <= 0) return false;
        destination += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

std::optional<protocol::Message> receive(int fd) {
    std::array<unsigned char, 4> header{};
    if (!read_exact(fd, header.data(), header.size())) return std::nullopt;
    std::size_t count = 0;
    for (const auto byte : header) count = (count << 8) | byte;
    if (count == 0 || count > protocol::kMaxFrameBytes) return std::nullopt;
    std::string bytes(count, '\0');
    if (!read_exact(fd, bytes.data(), bytes.size())) return std::nullopt;
    auto parsed = protocol::parse_message(bytes);
    if (!parsed) return std::nullopt;
    return *parsed;
}

void send_request(int fd, const protocol::Request& request) {
    const auto frame = protocol::encode_frame(protocol::serialize_message(request));
    ASSERT_EQ(send(fd, frame.data(), frame.size(), MSG_NOSIGNAL), static_cast<ssize_t>(frame.size()));
}

void register_hello(api::HandlerRegistry& registry) {
    registry.register_handler<api::HelloParamsMethod>([](const auto&, auto reply) {
        protocol::Capabilities capabilities{{"auto"}, "", {"unavailable"}, {"unknown"}, "arch", {}};
        reply.ok({"0.1.0", protocol::kApiVersion, {"auto"}, capabilities, {{"gtk"}}});
    });
}

bool hello(int fd, std::int64_t id) {
    const auto frame = protocol::encode_frame(protocol::serialize_message(
        protocol::Request{id, std::string(api::HelloParamsMethod::wire_name), protocol::HelloParams{protocol::kApiVersion}}));
    if (send(fd, frame.data(), frame.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(frame.size())) return false;
    const auto reply = receive(fd);
    return reply && std::holds_alternative<protocol::Response>(*reply) &&
           std::holds_alternative<nlohmann::json>(std::get<protocol::Response>(*reply).outcome);
}

bool closed_by_peer(int fd) {
    pollfd entry{fd, POLLIN, 0};
    if (poll(&entry, 1, 3000) <= 0) return false;
    char byte{};
    return recv(fd, &byte, 1, 0) == 0;
}

void send_raw(int fd, const std::string& body) {
    const auto frame = protocol::encode_frame(body);
    ASSERT_EQ(send(fd, frame.data(), frame.size(), MSG_NOSIGNAL), static_cast<ssize_t>(frame.size()));
}

TEST(ApiRegistry, UnknownAndInvalidParameters) {
    api::HandlerRegistry registry;
    std::optional<std::variant<nlohmann::json, protocol::ErrorObject>> answer;
    registry.dispatch({1, "does.not.exist", {}}, [&](auto outcome) { answer = std::move(outcome); });
    ASSERT_TRUE(answer);
    EXPECT_EQ(std::get<protocol::ErrorObject>(*answer).code, protocol::kMethodNotFound);
    registry.register_handler<api::HelloParamsMethod>([](const protocol::HelloParams&, api::Reply<protocol::HelloResult>) {});
    registry.dispatch({1, std::string(api::HelloParamsMethod::wire_name), nlohmann::json{{"apiVersion", 8}}},
                      [&](auto outcome) { answer = std::move(outcome); });
    EXPECT_EQ(std::get<protocol::ErrorObject>(*answer).code, protocol::kInvalidParams);
    EXPECT_EQ(std::get<protocol::ErrorObject>(*answer).message_key, "error.generic_title");
}

TEST(ApiRegistry, AsyncReplyAndExceptionAreBounded) {
    api::HandlerRegistry registry;
    std::optional<api::Reply<protocol::JobIdResult>> deferred;
    registry.register_handler<api::RefreshParamsMethod>([&](const protocol::RefreshParams&, auto reply) {
        deferred = reply;
    });
    std::size_t received = 0;
    registry.dispatch({1, std::string(api::RefreshParamsMethod::wire_name), protocol::RefreshParams{}},
                      [&](auto outcome) { ++received; EXPECT_TRUE(std::holds_alternative<nlohmann::json>(outcome)); });
    EXPECT_EQ(received, 0U);
    ASSERT_TRUE(deferred);
    deferred->ok({"refresh-1"});
    deferred->ok({"refresh-2"});
    EXPECT_EQ(received, 1U);
    registry.register_handler<api::HelloParamsMethod>([](const auto&, auto) { throw std::runtime_error("sensitive details"); });
    registry.dispatch({2, std::string(api::HelloParamsMethod::wire_name), nlohmann::json{{"apiVersion", 1}}},
                      [&](auto outcome) {
                          EXPECT_EQ(std::get<protocol::ErrorObject>(outcome).code, protocol::kInternalError);
                          EXPECT_EQ(std::get<protocol::ErrorObject>(outcome).message_key, "error.generic_title");
                      });
}

TEST(SocketSecurity, RefusesInsecureOrSymbolicRuntime) {
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    ASSERT_EQ(chmod(runtime.path.c_str(), 0777), 0);
    api::Server insecure(registry, runtime.path);
    EXPECT_FALSE(insecure.start());
    ASSERT_EQ(chmod(runtime.path.c_str(), 0700), 0);
    const auto link = runtime.path / "linked";
    ASSERT_EQ(symlink(runtime.path.c_str(), link.c_str()), 0);
    api::Server symbolic(registry, link);
    EXPECT_FALSE(symbolic.start());
}

TEST(SocketSecurity, RejectsUntrustedPeerAndProtectsSocket) {
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    DenyPeer denied;
    Gio::init();
    api::Server server(registry, runtime.path, {}, &denied);
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    const auto path = runtime.path / "braurser/core.sock";
    struct stat info{};
    ASSERT_EQ(lstat(path.c_str(), &info), 0);
    EXPECT_TRUE(S_ISSOCK(info.st_mode));
    EXPECT_EQ(info.st_mode & 0777, 0600);
    api::Server duplicate(registry, runtime.path);
    EXPECT_FALSE(duplicate.start());
    {
    RunningServer running(server);
    int fd = connect_client(path);
    ASSERT_GE(fd, 0);
    pollfd event{fd, POLLIN | POLLHUP, 0};
    ASSERT_GT(poll(&event, 1, 3000), 0);
    char byte{};
    EXPECT_EQ(recv(fd, &byte, 1, 0), 0);
    close(fd);
    }
    EXPECT_NE(lstat(path.c_str(), &info), 0);
}

TEST(DaemonSocket, RoundTripsAndBroadcastsWhileIdle) {
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    registry.register_handler<api::HelloParamsMethod>([](const auto&, auto reply) {
        protocol::Capabilities capabilities{{"auto"}, "", {"unavailable"}, {"unknown"}, "arch", {}};
        reply.ok({"0.1.0", protocol::kApiVersion, {"auto"}, capabilities, {{"gtk"}}});
    });
    registry.register_handler<api::SearchParamsMethod>([](const auto&, auto reply) {
        reply.ok(protocol::SearchResult{{}, {}, false});
    });
    registry.register_handler<api::SettingsSetParamsMethod>([](const auto& params, auto reply) {
        protocol::Settings settings{{"auto"}, {"auto"}, {"daily"}, true, false, true, false, {"system"}};
        if (params.changedFields.notifications) settings.notifications = *params.changedFields.notifications;
        reply.ok(protocol::SettingsSetResult{settings});
    });
    Gio::init();
    api::Server server(registry, runtime.path);
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    api::Broadcaster broadcast(server);
    RunningServer running(server);
    const int fd = connect_client(runtime.path / "braurser/core.sock");
    ASSERT_GE(fd, 0);
    send_request(fd, {1, std::string(api::HelloParamsMethod::wire_name), protocol::HelloParams{protocol::kApiVersion}});
    auto first = receive(fd);
    ASSERT_TRUE(first);
    ASSERT_TRUE(std::holds_alternative<protocol::Response>(*first));
    EXPECT_EQ(std::get<nlohmann::json>(std::get<protocol::Response>(*first).outcome).at("apiVersion"), 1);
    send_request(fd, {2, std::string(api::SearchParamsMethod::wire_name), protocol::SearchParams{"demo", {}, {}, 10}});
    auto second = receive(fd);
    ASSERT_TRUE(second);
    EXPECT_TRUE(std::get<nlohmann::json>(std::get<protocol::Response>(*second).outcome).at("packages").empty());
    protocol::SettingsPatch patch;
    patch.notifications = false;
    send_request(fd, {3, std::string(api::SettingsSetParamsMethod::wire_name), protocol::SettingsSetParams{patch}});
    auto third = receive(fd);
    ASSERT_TRUE(third);
    EXPECT_FALSE(std::get<nlohmann::json>(std::get<protocol::Response>(*third).outcome).at("settings").at("notifications"));
    Glib::MainContext::get_default()->invoke([&] {
        broadcast.emit<api::CatalogChangedNotificationEvent>({8});
        return false;
    });
    auto notification = receive(fd);
    ASSERT_TRUE(notification);
    ASSERT_TRUE(std::holds_alternative<protocol::Notification>(*notification));
    EXPECT_EQ(std::get<protocol::Notification>(*notification).method, api::CatalogChangedNotificationEvent::wire_name);
    close(fd);
}

TEST(DaemonSocket, AliveRejectsUnknownAndDeferredClosingConnections) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    std::atomic<api::ConnectionId> owner{};
    registry.register_handler<api::HelloParamsMethod>([&](const protocol::HelloParams&, api::RequestContext context, auto reply) {
        owner = context.connection;
        protocol::Capabilities capabilities{{"auto"}, "", {"unavailable"}, {"unknown"}, "arch", {}};
        reply.ok({"0.1.0", protocol::kApiVersion, {"auto"}, capabilities, {{"gtk"}}});
    });
    api::Server server(registry, runtime.path);
    EXPECT_FALSE(server.alive(0));
    EXPECT_FALSE(server.alive(999));
    bool disconnected = false;
    std::promise<void> closed;
    server.on_disconnect([&](api::ConnectionId id) {
        EXPECT_EQ(id, owner.load());
        EXPECT_FALSE(server.alive(id));
        disconnected = true;
        closed.set_value();
    });
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const int fd = connect_client(runtime.path / "braurser/core.sock");
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(hello(fd, 1));
    ASSERT_NE(owner.load(), 0U);
    std::promise<void> marked;
    Glib::MainContext::get_default()->invoke([&] {
        EXPECT_TRUE(server.alive(owner.load()));
        EXPECT_FALSE(server.alive(0));
        EXPECT_FALSE(server.alive(owner.load() + 1));
        server.broadcast({std::string(api::CatalogChangedNotificationEvent::wire_name), nlohmann::json::object()});
        EXPECT_FALSE(disconnected);
        EXPECT_FALSE(server.alive(owner.load()));
        marked.set_value();
        return false;
    });
    marked.get_future().wait();
    EXPECT_TRUE(closed_by_peer(fd));
    EXPECT_EQ(closed.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    close(fd);
}

TEST(DaemonSocket, ConnectionIdentitiesAreMonotonicAndNotificationsStayPrivate) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    std::atomic<api::ConnectionId> first{};
    std::atomic<api::ConnectionId> second{};
    std::atomic<api::ConnectionId> disconnected{};
    registry.register_handler<api::HelloParamsMethod>([&](const protocol::HelloParams&, api::RequestContext context, auto reply) {
        if (first.load() == 0) first = context.connection;
        else second = context.connection;
        protocol::Capabilities capabilities{{"auto"}, "", {"unavailable"}, {"unknown"}, "arch", {}};
        reply.ok({"0.1.0", protocol::kApiVersion, {"auto"}, capabilities, {{"gtk"}}});
    });
    api::Server server(registry, runtime.path);
    server.on_disconnect([&](api::ConnectionId id) { disconnected = id; });
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const auto path = runtime.path / "braurser/core.sock";
    int original = connect_client(path);
    ASSERT_GE(original, 0);
    send_request(original, {1, "system.hello", protocol::HelloParams{protocol::kApiVersion}});
    ASSERT_TRUE(receive(original));
    const auto original_id = first.load();
    ASSERT_GT(original_id, 0U);
    close(original);
    for (int attempt = 0; attempt < 200 && disconnected.load() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(disconnected.load(), original_id);
    const int replacement = connect_client(path);
    ASSERT_GE(replacement, 0);
    send_request(replacement, {2, "system.hello", protocol::HelloParams{protocol::kApiVersion}});
    ASSERT_TRUE(receive(replacement));
    ASSERT_GT(second.load(), original_id);
    const auto recipient = second.load();
    const int other = connect_client(path);
    ASSERT_GE(other, 0);
    send_request(other, {3, "system.hello", protocol::HelloParams{protocol::kApiVersion}});
    ASSERT_TRUE(receive(other));
    Glib::MainContext::get_default()->invoke([&] {
        server.emit_to(original_id, protocol::Notification{"job.state", protocol::JobStateNotification{"old", 1, {"failed"}}});
        server.emit_to(recipient, protocol::Notification{"job.state", protocol::JobStateNotification{"owned", 1, {"failed"}}});
        server.broadcast(protocol::Notification{"job.question", nlohmann::json::object()});
        return false;
    });
    auto private_event = receive(replacement);
    ASSERT_TRUE(private_event);
    ASSERT_TRUE(std::holds_alternative<protocol::Notification>(*private_event));
    EXPECT_EQ(std::get<protocol::Notification>(*private_event).params.at("jobId"), "owned");
    pollfd entry{other, POLLIN, 0};
    EXPECT_EQ(poll(&entry, 1, 50), 0);
    close(replacement);
    close(other);
}

TEST(DaemonCatalog, PinnedBuildListingFetchesCompleteLargeFilesAndDisclosesBinary) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    const auto cache = runtime.path / "cache";
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(cache));
    api::HandlerRegistry registry;
    api::Server server(registry, runtime.path);
    api::Broadcaster broadcaster(server);
    braurser::daemon::app::RealServices services({cache, runtime.path / "state", runtime.path / "config", runtime.path}, broadcaster);
    const auto bare = cache / "braurser/manual/aur-widget/repo.git";
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(bare.parent_path()));
    ASSERT_GE(git_libgit2_init(), 0);
    git_repository* raw_repo = nullptr;
    ASSERT_EQ(git_repository_init(&raw_repo, bare.c_str(), 1), 0);
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw_repo, git_repository_free);
    git_treebuilder* raw_builder = nullptr;
    ASSERT_EQ(git_treebuilder_new(&raw_builder, repo.get(), nullptr), 0);
    std::unique_ptr<git_treebuilder, decltype(&git_treebuilder_free)> builder(raw_builder, git_treebuilder_free);
    const std::vector<std::pair<std::string, std::string>> files{
        {"PKGBUILD", "pkgname=widget\n"}, {".SRCINFO", "pkgbase = widget\n"},
        {"build.sh", std::string(300 * 1024, 'x')}, {"large.sh", std::string(448 * 1024, '\n')},
        {"image", std::string("a\0b", 3)}};
    for (const auto& [name, bytes] : files) {
        git_oid oid{};
        ASSERT_EQ(git_blob_create_frombuffer(&oid, repo.get(), bytes.data(), bytes.size()), 0);
        ASSERT_EQ(git_treebuilder_insert(nullptr, builder.get(), name.c_str(), &oid, GIT_FILEMODE_BLOB), 0);
    }
    git_oid tree_id{};
    ASSERT_EQ(git_treebuilder_write(&tree_id, builder.get()), 0);
    git_tree* raw_tree = nullptr;
    ASSERT_EQ(git_tree_lookup(&raw_tree, repo.get(), &tree_id), 0);
    std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
    git_signature* raw_signature = nullptr;
    ASSERT_EQ(git_signature_new(&raw_signature, "Test", "test@example.org", 0, 0), 0);
    std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(raw_signature, git_signature_free);
    git_oid id{};
    ASSERT_EQ(git_commit_create(&id, repo.get(), nullptr, signature.get(), signature.get(), nullptr, "review", tree.get(), 0, nullptr), 0);
    std::array<char, GIT_OID_MAX_HEXSIZE + 1> hex{};
    git_oid_tostr(hex.data(), hex.size(), &id);
    protocol::BuildFilesParams request{"widget", hex.data()};
    const auto listing = services.build_files(request);
    ASSERT_TRUE(listing);
    EXPECT_FALSE(listing->truncated);
    ASSERT_EQ(listing->files.size(), files.size());
    std::size_t omitted = 0;
    for (const auto& file : listing->files) {
        if (file.name == "image") { EXPECT_EQ(file.kind, "binary"); EXPECT_TRUE(file.text.empty()); EXPECT_EQ(file.size, 3U); }
        if (file.kind == "omitted") { ++omitted; EXPECT_TRUE(file.text.empty()); }
    }
    EXPECT_EQ(omitted, 1U);
    for (const auto* path : {"build.sh", "large.sh"}) {
        request.path = path;
        const auto result = services.build_files(request);
        ASSERT_TRUE(result);
        ASSERT_EQ(result->files.size(), 1U);
        EXPECT_EQ(result->files.front().kind, "text");
        EXPECT_EQ(result->files.front().name, path);
        EXPECT_EQ(result->files.front().text.size(), std::string_view(path) == "build.sh" ? 300U * 1024 : 448U * 1024);
        const auto wire = protocol::serialize_message(protocol::Response{1, nlohmann::json(*result)});
        EXPECT_LE(wire.size(), protocol::kMaxFrameBytes);
    }
    request.path = "missing";
    EXPECT_FALSE(services.build_files(request));
}

TEST(DaemonStartup, ConstructsRealServiceContainerAfterGioInitialization) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    auto cache = runtime.path / "cache";
    auto state = runtime.path / "state";
    auto config = runtime.path / "config";
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(cache));
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(state));
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(config));
    api::HandlerRegistry registry;
    api::Server server(registry, runtime.path);
    api::Broadcaster broadcaster(server);
    EXPECT_NO_THROW(([&] {
        braurser::daemon::app::RealServices services({cache, state, config, runtime.path}, broadcaster);
        services.start_monitors();
        api::register_handlers(registry, services);
        for (const auto& method : {"jobs.plan", "jobs.install", "jobs.remove", "jobs.upgradeSystem",
                                   "jobs.upgradeCommunity", "manual.install", "manual.upgrade",
                                   "manual.remove", "setup.helper", "jobs.dequeue"}) {
            nlohmann::json params = method == std::string_view("jobs.plan") ?
                nlohmann::json{{"kind", "update-system"}} : method == std::string_view("jobs.dequeue") ?
                nlohmann::json{{"jobId", "job-1"}} :
                nlohmann::json{{"planId", "plan-1"}, {"approvalToken", "opaque"}};
            registry.dispatch({1, method, params}, [&](auto outcome) {
                EXPECT_EQ(std::get<protocol::ErrorObject>(outcome).code, protocol::kUnsupportedCapability);
            });
        }
    }()));
}

TEST(DaemonCatalog, SearchesOfficialPackagesWithoutOptionalSourcesAndListsFacets) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    for (const auto& name : {"cache", "state", "config"})
        ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(runtime.path / name));
    const auto cache = runtime.path / "cache";
    braurser::daemon::cache::SnapshotBuilder builder(cache);
    ASSERT_TRUE(builder.begin({"core"}));
    braurser::daemon::catalog::RepoPackage repo;
    repo.repo = "core";
    repo.name = "demo";
    repo.base = "demo";
    repo.version = "1.0";
    repo.description = "Sample application";
    repo.groups = {"utilities"};
    repo.download_size = 123;
    ASSERT_TRUE(builder.add(repo));
    braurser::daemon::catalog::InstalledPackage installed;
    installed.name = "demo";
    installed.version = "0.9";
    installed.size = 456;
    ASSERT_TRUE(builder.add(installed));
    braurser::daemon::catalog::AppMetadata app;
    app.pkgname = "demo";
    app.appstream_id = "demo.desktop";
    app.name = "Demo App";
    app.categories = {"Utility"};
    ASSERT_TRUE(builder.add(app));
    ASSERT_TRUE(builder.add(braurser::daemon::catalog::SourceState{"core", "ok", "", "1", "0", 1}));
    ASSERT_TRUE(builder.add(braurser::daemon::catalog::SourceState{"installed", "ok", "", "1", "0", 1}));
    ASSERT_TRUE(builder.add(braurser::daemon::catalog::SourceState{"community", "unavailable", "dump-unavailable", "0", "0", 1}));
    ASSERT_TRUE(builder.add(braurser::daemon::catalog::SourceState{"apps", "ok", "", "1", "0", 1}));
    ASSERT_TRUE(builder.commit());
    api::HandlerRegistry registry;
    api::Server server(registry, runtime.path);
    api::Broadcaster broadcaster(server);
    braurser::daemon::app::RealServices services({cache, runtime.path / "state", runtime.path / "config", runtime.path}, broadcaster);
    protocol::SearchParams params{"demo", {}, {}, 20};
    params.sort = {"name"};
    params.category = "Utility";
    auto page = services.search(params);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->packages.size(), 1U);
    EXPECT_EQ(page->packages[0].installedVersion, "0.9");
    EXPECT_EQ(page->packages[0].installedSize, 456U);
    EXPECT_EQ(page->packages[0].downloadSize, 123U);
    EXPECT_EQ(page->packages[0].appName, "Demo App");
    auto facets = services.facets({});
    ASSERT_TRUE(facets);
    ASSERT_EQ(facets->repos.size(), 1U);
    EXPECT_EQ(facets->repos[0].packageCount, 1U);
    ASSERT_EQ(facets->groups.size(), 1U);
    EXPECT_EQ(facets->groups[0].name, "utilities");
    ASSERT_EQ(facets->categories.size(), 1U);
    EXPECT_EQ(facets->categories[0].id, "Utility");
    std::optional<api::ServiceResult<protocol::GetResult>> background;
    std::jthread worker([&] { background = services.catalog_detail({"core/demo"}); });
    auto detail = services.get({"core/demo"});
    worker.join();
    ASSERT_TRUE(detail);
    ASSERT_TRUE(detail->appMetadata);
    ASSERT_TRUE(background);
    ASSERT_TRUE(*background);
    EXPECT_FALSE((*background)->appMetadata);
    detail->appMetadata.reset();
    EXPECT_EQ(nlohmann::json(**background), nlohmann::json(*detail));
}

TEST(DaemonCatalog, ResolvesShadowedRepositoriesThroughSearchAndGet) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    for (const auto& name : {"cache", "state", "config"})
        ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(runtime.path / name));
    const auto cache = runtime.path / "cache";
    braurser::daemon::cache::SnapshotBuilder builder(cache);
    ASSERT_TRUE(builder.begin({"core", "extra"}));
    braurser::daemon::catalog::RepoPackage package;
    package.repo = "core";
    package.name = "demo";
    package.base = "demo";
    package.version = "2";
    ASSERT_TRUE(builder.add(package));
    package.repo = "extra";
    package.version = "1";
    ASSERT_TRUE(builder.add(package));
    ASSERT_TRUE(builder.commit());
    api::HandlerRegistry registry;
    api::Server server(registry, runtime.path);
    api::Broadcaster broadcaster(server);
    braurser::daemon::app::RealServices services({cache, runtime.path / "state", runtime.path / "config", runtime.path}, broadcaster);
    protocol::SearchParams search{"demo", {}, {}, 20};
    auto page = services.search(search);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->packages.size(), 1U);
    EXPECT_EQ(page->packages.front().package.repo, "core");
    EXPECT_EQ(page->packages.front().alsoIn, (std::vector<std::string>{"extra"}));
    EXPECT_EQ(page->packages.front().shadowed, false);
    search.filters.sourceRepo = "extra";
    page = services.search(search);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->packages.size(), 1U);
    EXPECT_EQ(page->packages.front().package.repo, "extra");
    EXPECT_EQ(page->packages.front().alsoIn, (std::vector<std::string>{"core"}));
    EXPECT_EQ(page->packages.front().shadowed, true);
    auto detail = services.get({"extra/demo"});
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->details.version, "1");
    EXPECT_EQ(detail->details.alsoIn, (std::vector<std::string>{"core"}));
    EXPECT_EQ(detail->details.shadowed, true);
}

TEST(DaemonCatalog, IncompatibleSchemaIsNotSearched) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    for (const auto& name : {"cache", "state", "config"})
        ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(runtime.path / name));
    const auto cache = runtime.path / "cache";
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(cache / "braurser"));
    const auto path = cache / "braurser/catalog.sqlite";
    {
        braurser::daemon::cache::Database incompatible(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        incompatible.execute("PRAGMA user_version=1");
    }
    api::HandlerRegistry registry;
    api::Server server(registry, runtime.path);
    api::Broadcaster broadcaster(server);
    braurser::daemon::app::RealServices services({cache, runtime.path / "state", runtime.path / "config", runtime.path}, broadcaster);
    auto search = services.search(protocol::SearchParams{"demo", {}, {}, 20});
    ASSERT_TRUE(search);
    EXPECT_TRUE(search->packages.empty());
    EXPECT_EQ(search->catalogState, "refreshing");
    EXPECT_FALSE(services.facets({}));
}

TEST(DaemonStorage, RestartPersistsAndCacheClearKeepsCatalog) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    for (const auto& name : {"cache", "state", "config"})
        ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(runtime.path / name));
    api::HandlerRegistry registry;
    api::Server server(registry, runtime.path);
    api::Broadcaster broadcaster(server);
    const auto paths = braurser::daemon::core::XdgPaths{runtime.path / "cache", runtime.path / "state", runtime.path / "config", runtime.path};
    braurser::daemon::app::RealServices services(paths, broadcaster);
    const auto restart_path = paths.state / "braurser/restart.json";
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(restart_path.parent_path()));
    ASSERT_TRUE(braurser::daemon::core::atomic_write(restart_path,
                R"({"level":"reboot","reason_keys":["restart.kernel"],"since":"2026-09-25T00:00:00Z"})"));
    auto restart = services.restart_state({});
    ASSERT_TRUE(restart);
    EXPECT_EQ(restart->level.value, "reboot");
    ASSERT_TRUE(services.restart_acknowledge({}));
    restart = services.restart_state({});
    ASSERT_TRUE(restart);
    EXPECT_EQ(restart->level.value, "none");
    const auto root = paths.cache / "braurser";
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(root / "downloads"));
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(root / "build"));
    ASSERT_TRUE(braurser::daemon::core::atomic_write(root / "downloads/file", "data"));
    ASSERT_TRUE(braurser::daemon::core::atomic_write(root / "catalog.sqlite", "keep"));
    ASSERT_TRUE(braurser::daemon::core::atomic_write(root / "build/job", "keep"));
    auto cleared = services.clear_cache({});
    ASSERT_TRUE(cleared);
    EXPECT_EQ(cleared->freedBytes, 4U);
    EXPECT_FALSE(std::filesystem::exists(root / "downloads"));
    EXPECT_TRUE(std::filesystem::exists(root / "catalog.sqlite"));
    EXPECT_TRUE(std::filesystem::exists(root / "build/job"));
}

TEST(DaemonStorage, LogRedactsAndRotatesPrivately) {
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    ASSERT_TRUE(braurser::daemon::core::ensure_private_dir(runtime.path / "state"));
    api::DaemonLog log(runtime.path / "state");
    log.write("info", "daemon", "startup", {{"test", "password=hunter2 https://person:secret@example.org"}});
    const auto log_path = runtime.path / "state/braurser/daemon.log";
    struct stat info{};
    ASSERT_EQ(lstat(log_path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
    std::ifstream stream(log_path);
    const std::string line(std::istreambuf_iterator<char>{stream}, {});
    EXPECT_EQ(line.find("hunter2"), std::string::npos);
    EXPECT_EQ(line.find("person:secret"), std::string::npos);
    for (unsigned i = 0; i < 5000; ++i) log.write("info", "daemon", "pulse", {{"part", std::string(240, 'x')}});
    EXPECT_TRUE(std::filesystem::exists(log_path.string() + ".1"));
    EXPECT_LE(std::filesystem::file_size(log_path), 1024U * 1024U);
    EXPECT_FALSE(std::filesystem::exists(log_path.string() + ".4"));
}

TEST(DaemonSocket, RepliesToInvalidRequestsWithExtractableIds) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    registry.register_handler<api::StatusParamsMethod>([](const auto&, auto reply) {
        reply.ok(protocol::StatusResult{{"unknown"}, {}, {}, {"unavailable"}});
    });
    api::Server server(registry, runtime.path);
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const int fd = connect_client(runtime.path / "braurser/core.sock");
    ASSERT_GE(fd, 0);
    const std::array<std::pair<std::string, int>, 3> cases{{
        {R"({"jsonrpc":"2.0","id":1,"apiVersion":1,"method":"catalog.refresh","params":{"sources":["repos"]}})", protocol::kInvalidParams},
        {R"({"jsonrpc":"2.0","id":2,"apiVersion":1,"method":"unknown.method","params":{}})", protocol::kMethodNotFound},
        {R"({"jsonrpc":"2.0","id":"abc","method":"system.status","params":{}})", protocol::kInvalidRequest}
    }};
    for (const auto& [body, code] : cases) {
        const auto frame = protocol::encode_frame(body);
        ASSERT_EQ(send(fd, frame.data(), frame.size(), MSG_NOSIGNAL), static_cast<ssize_t>(frame.size()));
        auto result = receive(fd);
        ASSERT_TRUE(result);
        ASSERT_TRUE(std::holds_alternative<protocol::Response>(*result));
        EXPECT_EQ(std::get<protocol::ErrorObject>(std::get<protocol::Response>(*result).outcome).code, code);
    }
    send_request(fd, {4, std::string(api::StatusParamsMethod::wire_name), protocol::StatusParams{}});
    auto valid = receive(fd);
    ASSERT_TRUE(valid);
    EXPECT_EQ(std::get<protocol::Response>(*valid).id, protocol::RequestId(std::int64_t{4}));
    close(fd);
}

TEST(SocketLimits, RejectsOversizedFrameBeforeAllocation) {
    protocol::FrameDecoder decoder;
    const std::array<std::byte, 4> header{std::byte{0x00}, std::byte{0x10}, std::byte{0x00}, std::byte{0x01}};
    decoder.feed(header);
    const auto frame = decoder.next();
    ASSERT_FALSE(frame);
    EXPECT_EQ(frame.error().code, protocol::kInvalidRequest);
}

TEST(SocketLimits, TooManyPendingRequestsReturnBusy) {
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    std::vector<api::Reply<protocol::JobIdResult>> pending;
    registry.register_handler<api::RefreshParamsMethod>([&](const auto&, auto reply) { pending.push_back(reply); });
    Gio::init();
    api::Server server(registry, runtime.path);
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const int fd = connect_client(runtime.path / "braurser/core.sock");
    ASSERT_GE(fd, 0);
    for (int i = 0; i < 65; ++i)
        send_request(fd, {i, std::string(api::RefreshParamsMethod::wire_name), protocol::RefreshParams{}});
    auto busy = receive(fd);
    ASSERT_TRUE(busy);
    EXPECT_EQ(std::get<protocol::ErrorObject>(std::get<protocol::Response>(*busy).outcome).code, protocol::kBusy);
    close(fd);
}

TEST(SocketLimits, RejectedFrameIdExtractionIsBoundedAndPositionIndependent) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    register_hello(registry);
    api::Server server(registry, runtime.path);
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const auto path = runtime.path / "braurser/core.sock";
    const int late = connect_client(path);
    ASSERT_GE(late, 0);
    send_raw(late, R"({"jsonrpc":"2.0","method":"system.status","params":{"x":{"y":[1,[2,{"z":3}]]}},"id":"late"})");
    auto answer = receive(late);
    ASSERT_TRUE(answer);
    ASSERT_TRUE(std::holds_alternative<protocol::Response>(*answer));
    EXPECT_EQ(std::get<protocol::Response>(*answer).id, protocol::RequestId(std::string("late")));
    EXPECT_EQ(std::get<protocol::ErrorObject>(std::get<protocol::Response>(*answer).outcome).code, protocol::kInvalidRequest);
    close(late);
    const int nested = connect_client(path);
    ASSERT_GE(nested, 0);
    std::string deep = R"({"jsonrpc":"2.0","id":7,"params":)";
    deep += std::string(40, '[') + std::string(40, ']') + "}";
    send_raw(nested, deep);
    EXPECT_TRUE(closed_by_peer(nested));
    close(nested);
    const int flood = connect_client(path);
    ASSERT_GE(flood, 0);
    std::string unterminated = R"({"id":1,"params":)";
    unterminated += std::string(protocol::kMaxFrameBytes - unterminated.size(), '[');
    send_raw(flood, unterminated);
    EXPECT_TRUE(closed_by_peer(flood));
    close(flood);
    const int after = connect_client(path);
    ASSERT_GE(after, 0);
    EXPECT_TRUE(hello(after, 1));
    close(after);
}

TEST(SocketLimits, CapsConcurrentClients) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    register_hello(registry);
    api::Server server(registry, runtime.path);
    std::atomic<unsigned> disconnected{};
    server.on_disconnect([&](api::ConnectionId) { ++disconnected; });
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const auto path = runtime.path / "braurser/core.sock";
    std::vector<int> clients;
    for (int index = 0; index < 128; ++index) {
        const int fd = connect_client(path);
        ASSERT_GE(fd, 0);
        clients.push_back(fd);
        ASSERT_TRUE(hello(fd, index + 1)) << index;
    }
    const int extra = connect_client(path);
    ASSERT_GE(extra, 0);
    EXPECT_TRUE(closed_by_peer(extra));
    close(extra);
    close(clients.back());
    clients.pop_back();
    for (int attempt = 0; attempt < 600 && disconnected.load() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(disconnected.load(), 1U);
    const int replacement = connect_client(path);
    ASSERT_GE(replacement, 0);
    EXPECT_TRUE(hello(replacement, 500));
    close(replacement);
    for (const int fd : clients) close(fd);
}

TEST(SocketLimits, NestedBroadcastFromDisconnectDoesNotReuseErasedClients) {
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    register_hello(registry);
    api::Server server(registry, runtime.path);
    const protocol::Notification line{"job.log", protocol::JobLogNotification{"job", 1, std::string(4000, 'x')}};
    bool nested = false;
    std::atomic<unsigned> disconnected{};
    server.on_disconnect([&](api::ConnectionId) {
        ++disconnected;
        if (nested) return;
        nested = true;
        server.broadcast(line);
    });
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const auto path = runtime.path / "braurser/core.sock";
    std::vector<int> clients;
    for (int index = 0; index < 3; ++index) {
        const int fd = connect_client(path);
        ASSERT_GE(fd, 0);
        ASSERT_TRUE(hello(fd, index + 1));
        clients.push_back(fd);
    }
    std::promise<void> flooded;
    Glib::MainContext::get_default()->invoke([&] {
        for (int round = 0; round < 1300; ++round) server.broadcast(line);
        EXPECT_EQ(disconnected.load(), 0U);
        EXPECT_FALSE(nested);
        server.broadcast(line);
        EXPECT_EQ(disconnected.load(), 0U);
        flooded.set_value();
        return false;
    });
    flooded.get_future().wait();
    for (const int fd : clients) {
        EXPECT_TRUE(closed_by_peer(fd));
        close(fd);
    }
    EXPECT_EQ(disconnected.load(), 3U);
    const int after = connect_client(path);
    ASSERT_GE(after, 0);
    EXPECT_TRUE(hello(after, 10));
    close(after);
}

TEST(SocketLimits, SerializationFailureDisconnectsOnlyAfterBroadcastReturns) {
    Gio::init();
    Runtime runtime;
    api::HandlerRegistry registry;
    register_hello(registry);
    api::Server server(registry, runtime.path);
    std::atomic<unsigned> disconnected{};
    server.on_disconnect([&](api::ConnectionId) { ++disconnected; });
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const int fd = connect_client(runtime.path / "braurser/core.sock");
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(hello(fd, 1));
    std::promise<void> broadcasted;
    Glib::MainContext::get_default()->invoke([&] {
        server.broadcast({"unknown.event", nlohmann::json::object()});
        EXPECT_EQ(disconnected.load(), 0U);
        server.broadcast({"job.log", protocol::JobLogNotification{"job", 1, "ignored"}});
        EXPECT_EQ(disconnected.load(), 0U);
        broadcasted.set_value();
        return false;
    });
    broadcasted.get_future().wait();
    EXPECT_TRUE(closed_by_peer(fd));
    EXPECT_EQ(disconnected.load(), 1U);
    close(fd);
}

TEST(SocketLimits, FirstRequestAndPartialFramesHaveDeadlines) {
    Gio::init();
    Runtime runtime;
    api::HandlerRegistry registry;
    register_hello(registry);
    api::Server server(registry, runtime.path, {}, nullptr, nullptr, {200, 80});
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const auto path = runtime.path / "braurser/core.sock";
    const int silent = connect_client(path);
    ASSERT_GE(silent, 0);
    EXPECT_TRUE(closed_by_peer(silent));
    close(silent);
    const int fd = connect_client(path);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(hello(fd, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ASSERT_TRUE(hello(fd, 2));
    const auto frame = protocol::encode_frame(protocol::serialize_message(
        protocol::Request{3, "system.hello", protocol::HelloParams{protocol::kApiVersion}}));
    ASSERT_EQ(send(fd, frame.data(), 2, MSG_NOSIGNAL), 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    ASSERT_EQ(send(fd, frame.data() + 2, 3, MSG_NOSIGNAL), 3);
    EXPECT_TRUE(closed_by_peer(fd));
    close(fd);
}

TEST(SocketLimits, CompletedFramesResetOnlyThePartialFrameDeadline) {
    Gio::init();
    Runtime runtime;
    api::HandlerRegistry registry;
    register_hello(registry);
    api::Server server(registry, runtime.path, {}, nullptr, nullptr, {500, 100});
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const int fd = connect_client(runtime.path / "braurser/core.sock");
    ASSERT_GE(fd, 0);
    const auto frame = protocol::encode_frame(protocol::serialize_message(
        protocol::Request{1, "system.hello", protocol::HelloParams{protocol::kApiVersion}}));
    ASSERT_EQ(send(fd, frame.data(), 5, MSG_NOSIGNAL), 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ASSERT_EQ(send(fd, frame.data() + 5, frame.size() - 5, MSG_NOSIGNAL), static_cast<ssize_t>(frame.size() - 5));
    ASSERT_TRUE(receive(fd));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_TRUE(hello(fd, 2));
    close(fd);
}

TEST(DaemonSocket, PublicBroadcastBacklogsHaveIdenticalFramesPerRecipient) {
    Gio::init();
    Runtime runtime;
    api::HandlerRegistry registry;
    register_hello(registry);
    api::Server server(registry, runtime.path);
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    std::vector<int> clients;
    for (int i = 0; i < 4; ++i) {
        const int fd = connect_client(runtime.path / "braurser/core.sock");
        ASSERT_GE(fd, 0);
        ASSERT_TRUE(hello(fd, i + 1));
        clients.push_back(fd);
    }
    std::promise<void> queued;
    Glib::MainContext::get_default()->invoke([&] {
        for (std::uint64_t sequence = 1; sequence <= 100; ++sequence)
            server.broadcast({"job.log", protocol::JobLogNotification{"job", sequence, std::string(4000, 'x')}});
        queued.set_value();
        return false;
    });
    queued.get_future().wait();
    for (const int fd : clients) {
        for (std::uint64_t sequence = 1; sequence <= 100; ++sequence) {
            const auto message = receive(fd);
            ASSERT_TRUE(message);
            const auto* notification = std::get_if<protocol::Notification>(&*message);
            ASSERT_NE(notification, nullptr);
            EXPECT_EQ(notification->method, "job.log");
            EXPECT_EQ(notification->params, nlohmann::json(protocol::JobLogNotification{"job", sequence, std::string(4000, 'x')}));
        }
        close(fd);
    }
}

TEST(SocketLimits, AcceptRecoversAfterDescriptorExhaustion) {
#if defined(__SANITIZE_ADDRESS__)
    GTEST_SKIP() << "the sanitizer runtime needs spare descriptors";
#endif
    Gio::init();
    Runtime runtime;
    ASSERT_FALSE(runtime.path.empty());
    api::HandlerRegistry registry;
    register_hello(registry);
    api::Server server(registry, runtime.path);
    auto started = server.start();
    if (!started && started.error() == "cannot create listener: 1") GTEST_SKIP() << "AF_UNIX denied by sandbox";
    ASSERT_TRUE(started) << started.error();
    RunningServer running(server);
    const auto path = runtime.path / "braurser/core.sock";
    const int first = connect_client(path);
    ASSERT_GE(first, 0);
    ASSERT_TRUE(hello(first, 1));
    rlimit original{};
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &original), 0);
    const int probe = dup(first);
    ASSERT_GE(probe, 0);
    close(probe);
    rlimit reduced = original;
    reduced.rlim_cur = static_cast<rlim_t>(probe) + 1;
    ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &reduced), 0);
    const int second = connect_client(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    close(first);
    const bool served = second >= 0 && hello(second, 2);
    ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &original), 0);
    EXPECT_GE(second, 0);
    EXPECT_TRUE(served);
    if (second >= 0) close(second);
}

}
