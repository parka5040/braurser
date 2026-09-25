#include <braurser/daemon/notifications/service.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/presenter/copy.hpp>
#include <giomm/dbusproxy.h>
#include <glibmm/variant.h>
#include <algorithm>
#include <cerrno>
#include <array>
#include <cstdio>
#include <format>
#include <limits>
#include <set>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;

namespace braurser::daemon::notifications {
namespace {
std::optional<std::string> digest(const protocol::UpdateReport& report) {
    std::set<std::string> ids;
    auto collect = [&ids](const auto& entries, std::string_view source) {
        if (entries.size() > 10000) return false;
        for (const auto& item : entries) {
            if (item.package.name.empty() || item.package.name.size() > 255 || ids.size() >= 10000)
                return false;
            ids.insert(std::string(source) + item.package.name);
        }
        return true;
    };
    if (!collect(report.official, "official:") || !collect(report.community, "community:") ||
        !collect(report.manual, "manual:") || ids.empty()) return std::nullopt;
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& id : ids) {
        for (const char raw : id) { hash ^= static_cast<unsigned char>(raw); hash *= 1099511628211ULL; }
        hash ^= 0;
        hash *= 1099511628211ULL;
    }
    return std::format("{:016x}", hash);
}

std::string day_of(std::chrono::system_clock::time_point time) {
    return std::format("{:%F}", std::chrono::floor<std::chrono::days>(time));
}
}

struct DesktopTransport::Impl {
    explicit Impl(std::filesystem::path executable) : launcher(std::move(executable)) {}
    std::filesystem::path launcher;
    Glib::RefPtr<Gio::DBus::Proxy> proxy;
    sigc::connection signal;
    std::set<std::uint32_t> ids;
};

DesktopTransport::DesktopTransport(std::filesystem::path launcher)
    : impl_(std::make_unique<Impl>(std::move(launcher))) {}
DesktopTransport::~DesktopTransport() { impl_->signal.disconnect(); }

bool DesktopTransport::send(const Payload& payload) {
    if (payload.app_name != "braurser" || payload.icon != "braurser" ||
        payload.actions.size() != 2 || payload.actions[0] != "review" ||
        payload.summary.size() > 512 || payload.body.size() > 2048 ||
        !impl_->launcher.is_absolute()) return false;
    try {
        if (!impl_->proxy) {
            impl_->proxy = Gio::DBus::Proxy::create_for_bus_sync(Gio::DBus::BusType::SESSION,
                "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications", {}, Gio::DBus::ProxyFlags::DO_NOT_AUTO_START |
                Gio::DBus::ProxyFlags::DO_NOT_LOAD_PROPERTIES);
            impl_->signal = impl_->proxy->signal_signal().connect([this](const Glib::ustring&,
                const Glib::ustring& name, Glib::VariantContainerBase args) {
                if (name != "ActionInvoked" || !g_variant_is_of_type(args.gobj(), G_VARIANT_TYPE("(us)"))) return;
                guint32 id{};
                const gchar* action = nullptr;
                g_variant_get(args.gobj(), "(u&s)", &id, &action);
                if (!impl_->ids.contains(id) || !action || std::string_view(action) != "review") return;
                impl_->ids.erase(id);
                const auto command = impl_->launcher.string();
                std::array<char*, 4> arguments{const_cast<char*>(command.c_str()),
                                               const_cast<char*>("--page"), const_cast<char*>("updates"), nullptr};
                pid_t pid{};
                if (posix_spawn(&pid, command.c_str(), nullptr, nullptr, arguments.data(), environ) == 0)
                    g_child_watch_add(pid, [](GPid child, gint, gpointer) {
                        g_spawn_close_pid(child);
                    }, nullptr);
            });
        }
        if (impl_->proxy->get_name_owner().empty()) return false;
        GVariantBuilder actions;
        GVariantBuilder hints;
        g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        for (const auto& action : payload.actions)
            g_variant_builder_add(&actions, "s", action.c_str());
        const auto arguments = Glib::VariantContainerBase(g_variant_new("(susss@as@a{sv}i)",
            payload.app_name.c_str(), 0U, payload.icon.c_str(), payload.summary.c_str(),
            payload.body.c_str(), g_variant_builder_end(&actions), g_variant_builder_end(&hints), -1));
        auto result = impl_->proxy->call_sync("Notify", arguments, 2000);
        if (!g_variant_is_of_type(result.gobj(), G_VARIANT_TYPE("(u)"))) return false;
        guint32 id{};
        g_variant_get(result.gobj(), "(u)", &id);
        if (id == 0) return false;
        if (impl_->ids.size() >= 512) impl_->ids.clear();
        impl_->ids.insert(id);
        return true;
    } catch (const Glib::Error&) { return false; }
}

NotificationService::NotificationService(std::filesystem::path state, NotificationTransport& transport)
    : state_(std::move(state)), transport_(transport) {}

Payload NotificationService::payload(std::uint64_t count) {
    const presenter::Copy copy("en", presenter::MissingCopy::Throw);
    return {"braurser", "braurser", copy.text("notification.updates_title"),
            copy.text("notification.updates_body", {{"count", static_cast<std::int64_t>(
                std::min(count, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))}}),
            {"review", copy.text("notification.updates_action")}};
}

bool NotificationService::notify(const protocol::UpdateReport& report,
                                 std::chrono::system_clock::time_point now) {
    if (!state_.is_absolute()) return false;
    const auto current_hash = digest(report);
    if (!current_hash) return false;
    const auto current_day = day_of(now);
    const auto destination = state_ / "braurser/notifications.json";
    std::set<std::string> hashes;
    struct stat info{};
    if (lstat(destination.c_str(), &info) == 0) {
        if (!S_ISREG(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 0777) != 0600) return false;
        const auto content = core::read_bounded(destination, 16384);
        if (!content) return false;
        const auto parsed = nlohmann::json::parse(*content, nullptr, false);
        if (!parsed.is_object() || parsed.size() != 2 || !parsed.contains("day") ||
            !parsed.at("day").is_string() || !parsed.contains("hashes") ||
            !parsed.at("hashes").is_array() || parsed.at("hashes").size() > 512) return false;
        try {
            const auto day = parsed.at("day").get<std::string>();
            if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
            for (std::size_t i = 0; i < day.size(); ++i)
                if (i != 4 && i != 7 && (day[i] < '0' || day[i] > '9')) return false;
            for (const auto& value : parsed.at("hashes")) {
                if (!value.is_string()) return false;
                const auto hash = value.get<std::string>();
                if (hash.size() != 16 || !std::ranges::all_of(hash, [](char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                })) return false;
                if (day == current_day) hashes.insert(hash);
            }
        } catch (const nlohmann::json::exception&) { return false; }
    } else if (errno != ENOENT) return false;
    if (hashes.contains(*current_hash) || hashes.size() >= 512 ||
        !core::ensure_private_dir(state_ / "braurser")) return false;
    const auto count = report.official.size() + report.community.size() + report.manual.size();
    if (!transport_.send(payload(count))) return false;
    hashes.insert(*current_hash);
    const nlohmann::json next{{"day", current_day}, {"hashes", hashes}};
    if (!core::atomic_write(destination, next.dump(), 0600)) return false;
    return true;
}

}
