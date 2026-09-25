#include <braurser/daemon/updates/updates.hpp>
#include <braurser/daemon/core/files.hpp>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstdlib>
#include <optional>
#include <set>
#include <sstream>
#include <unistd.h>

namespace braurser::daemon::updates {
namespace {
struct Identity { uid_t uid{}; int parent{}; };
std::optional<int> number(std::string_view value) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < 0) return {};
    return result;
}
std::optional<Identity> identity(std::string_view status) {
    std::optional<int> uid;
    std::optional<int> parent;
    for (std::size_t start = 0; start < status.size();) {
        const auto end = status.find('\n', start);
        const auto line = status.substr(start, end == status.npos ? end : end - start);
        if (line.starts_with("Uid:\t") || line.starts_with("Uid: ")) {
            const auto first = line.find_first_not_of(" \t", 4);
            const auto last = line.find_first_of(" \t", first);
            if (first != line.npos) uid = number(line.substr(first, last - first));
        }
        if (line.starts_with("PPid:\t") || line.starts_with("PPid: ")) {
            const auto first = line.find_first_not_of(" \t", 5);
            if (first != line.npos) parent = number(line.substr(first));
        }
        if (end == status.npos) break;
        start = end + 1;
    }
    if (!uid || !parent) return {};
    return Identity{static_cast<uid_t>(*uid), *parent};
}
bool has_session_id(std::string_view env) {
    for (std::size_t start = 0; start < env.size();) {
        const auto end = env.find('\0', start);
        const auto item = env.substr(start, end == env.npos ? end : end - start);
        if (item.starts_with("XDG_SESSION_ID=") && item.size() > 15) return true;
        if (end == env.npos) break;
        start = end + 1;
    }
    return false;
}
std::optional<std::pair<int, int>> stat_session(std::string_view stat) {
    const auto end = stat.rfind(") ");
    if (end == stat.npos || end + 2 >= stat.size()) return {};
    std::istringstream fields(std::string(stat.substr(end + 2)));
    char state{};
    int ppid{}, pgrp{}, session{}, tty{};
    if (!(fields >> state >> ppid >> pgrp >> session >> tty)) return {};
    return std::pair{session, tty};
}
std::string normalize(std::string_view path) {
    if (path.ends_with(" (deleted)")) path.remove_suffix(10);
    if (path.empty() || path.size() > 4096 || path.front() != '/' ||
        path.find('\0') != path.npos || path.find("/../") != path.npos) return {};
    return std::string(path);
}
std::set<std::string> mapped_files(std::string_view maps) {
    std::set<std::string> result;
    for (std::size_t start = 0; start < maps.size();) {
        const auto end = maps.find('\n', start);
        const auto line = maps.substr(start, end == maps.npos ? end : end - start);
        std::size_t pos = 0;
        for (int field = 0; field < 5; ++field) {
            pos = line.find_first_not_of(" \t", pos);
            if (pos == line.npos) break;
            pos = line.find_first_of(" \t", pos);
            if (pos == line.npos) break;
        }
        if (pos != line.npos) {
            const auto path_start = line.find_first_not_of(" \t", pos);
            if (path_start != line.npos) {
                auto path = normalize(line.substr(path_start));
                if (!path.empty()) result.insert(std::move(path));
            }
        }
        if (end == maps.npos) break;
        start = end + 1;
    }
    return result;
}
}
RunningConsumerScanner::RunningConsumerScanner(std::filesystem::path proc_root) : proc_root_(std::move(proc_root)) {}
ConsumerScan RunningConsumerScanner::scan() const {
    ConsumerScan report;
    report.coverage_complete = true;
    std::error_code ec;
    std::filesystem::directory_iterator entries(proc_root_, ec);
    if (ec) { report.coverage_complete = false; ++report.unreadable_processes; return report; }
    std::size_t count = 0;
    for (const auto end = std::filesystem::directory_iterator{}; entries != end; entries.increment(ec)) {
        if (ec) break;
        const auto pid = number(entries->path().filename().string());
        if (!pid || *pid == 0) continue;
        if (++count > 100000) { ++report.unreadable_processes; report.coverage_complete = false; break; }
        const auto dir = proc_root_ / std::to_string(*pid);
        auto status = core::read_bounded(dir / "status", 64 * 1024);
        if (!status) { ++report.unreadable_processes; continue; }
        auto owner = identity(*status);
        if (!owner) { ++report.unreadable_processes; continue; }
        if (owner->uid != getuid()) continue;
        auto comm = core::read_bounded(dir / "comm", 256);
        auto maps = core::read_bounded(dir / "maps", 4 * 1024 * 1024);
        std::error_code link_error;
        auto exe = std::filesystem::read_symlink(dir / "exe", link_error);
        if (!comm || !maps || link_error || comm->empty() || comm->size() > 256 ||
            !comm->ends_with('\n') || normalize(exe.string()).empty()) {
            ++report.unreadable_processes;
            continue;
        }
        comm->pop_back();
        if (comm->empty() || !std::ranges::all_of(*comm, [](unsigned char c) {
                return c >= 0x20 && c != 0x7f;
            })) {
            ++report.unreadable_processes;
            continue;
        }
        bool session_app = false;
        bool reached_init = false;
        bool parent_unreadable = false;
        int cursor = *pid;
        int parent = owner->parent;
        std::set<int> visited;
        for (unsigned depth = 0; depth < 64; ++depth) {
            if (cursor == 1) { reached_init = true; break; }
            if (parent <= 0 || !visited.insert(cursor).second) { parent_unreadable = true; break; }
            const auto parent_dir = proc_root_ / std::to_string(cursor);
            auto stat = core::read_bounded(parent_dir / "stat", 4096);
            auto env = core::read_bounded(parent_dir / "environ", 64 * 1024);
            if (!stat || !env) { parent_unreadable = true; break; }
            auto session = stat_session(*stat);
            if (!session) { parent_unreadable = true; break; }
            if ((session->first == cursor && session->second != 0) || has_session_id(*env)) {
                session_app = true;
                break;
            }
            if (parent == 1) { reached_init = true; break; }
            cursor = parent;
            auto ancestor = core::read_bounded(proc_root_ / std::to_string(cursor) / "status", 64 * 1024);
            if (!ancestor) { parent_unreadable = true; break; }
            auto parsed = identity(*ancestor);
            if (!parsed) { parent_unreadable = true; break; }
            parent = parsed->parent;
        }
        if (!reached_init && !session_app) parent_unreadable = true;
        if (parent_unreadable) ++report.unreadable_processes;
        const bool service = reached_init && !session_app && !parent_unreadable;
        RunningConsumer consumer{*pid, *comm, service};
        auto paths = mapped_files(*maps);
        paths.insert(normalize(exe.string()));
        for (const auto& path : paths) if (!path.empty()) report.files[path].insert(consumer);
    }
    if (ec) ++report.unreadable_processes;
    report.coverage_complete = report.unreadable_processes == 0;
    return report;
}
}
