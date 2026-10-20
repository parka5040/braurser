#include <braurser/daemon/updates/updates.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <set>
#include <sstream>
#include <unistd.h>

namespace braurser::daemon::updates {
namespace {
class Fd {
public:
    explicit Fd(int value) : value_(value) {}
    ~Fd() { if (value_ >= 0) close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (value_ >= 0) close(value_); value_ = std::exchange(other.value_, -1); }
        return *this;
    }
    int get() const { return value_; }
private:
    int value_;
};
struct Identity { uid_t uid{}; int parent{}; };
struct ProcessStat {
    int parent{};
    int session{};
    int tty{};
    std::uint64_t start{};
    friend bool operator==(const ProcessStat&, const ProcessStat&) = default;
};
std::optional<std::string> read_at(int dir, const char* name, std::size_t limit) {
    const Fd fd(openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (fd.get() < 0) return {};
    std::string result;
    std::array<char, 8192> buffer{};
    while (true) {
        const auto amount = read(fd.get(), buffer.data(), buffer.size());
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0 || static_cast<std::size_t>(amount) > limit - result.size()) return {};
        if (amount == 0) return result;
        result.append(buffer.data(), static_cast<std::size_t>(amount));
    }
}
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
std::optional<ProcessStat> process_stat(std::string_view stat) {
    const auto end = stat.rfind(") ");
    if (end == stat.npos || end + 2 >= stat.size()) return {};
    std::istringstream fields(std::string(stat.substr(end + 2)));
    char state{};
    int group{};
    ProcessStat result;
    if (!(fields >> state >> result.parent >> group >> result.session >> result.tty)) return {};
    std::string ignored;
    for (unsigned field = 8; field < 22; ++field) if (!(fields >> ignored)) return {};
    std::string start;
    if (!(fields >> start)) return {};
    const auto [last, error] = std::from_chars(start.data(), start.data() + start.size(), result.start);
    if (error != std::errc{} || last != start.data() + start.size() || result.parent < 0) return {};
    return result;
}
std::optional<ProcessStat> stat_at(int dir) {
    const auto text = read_at(dir, "stat", 4096);
    return text ? process_stat(*text) : std::nullopt;
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
std::optional<bool> is_service(int dir, int pid, const ProcessStat& initial,
    const std::filesystem::path& root) {
    int cursor = pid;
    Fd current(fcntl(dir, F_DUPFD_CLOEXEC, 0));
    if (current.get() < 0) return {};
    std::set<int> visited;
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (!visited.insert(cursor).second) return {};
        const auto before = stat_at(current.get());
        const auto status = read_at(current.get(), "status", 64 * 1024);
        const auto owner = status ? identity(*status) : std::nullopt;
        const auto env = read_at(current.get(), "environ", 64 * 1024);
        const auto after = stat_at(current.get());
        if (!before || !owner || !env || !after || *before != *after ||
            owner->parent != before->parent || (cursor == pid && *before != initial)) return {};
        if ((before->session == cursor && before->tty != 0) || has_session_id(*env)) return false;
        if (cursor == 1 || before->parent == 1) return true;
        if (before->parent <= 0) return {};
        cursor = before->parent;
        Fd ancestor(open((root / std::to_string(cursor)).c_str(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (ancestor.get() < 0) return {};
        current = std::move(ancestor);
    }
    return {};
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
    std::size_t associations = 0;
    for (const auto end = std::filesystem::directory_iterator{}; entries != end; entries.increment(ec)) {
        if (ec) break;
        const auto pid = number(entries->path().filename().string());
        if (!pid || *pid == 0) continue;
        if (++count > 100000) { ++report.unreadable_processes; break; }
        const Fd dir(open((proc_root_ / std::to_string(*pid)).c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        const auto before = stat_at(dir.get());
        auto status = read_at(dir.get(), "status", 64 * 1024);
        auto owner = status ? identity(*status) : std::nullopt;
        if (dir.get() < 0 || !before || !owner || before->parent != owner->parent) {
            ++report.unreadable_processes;
            continue;
        }
        if (owner->uid != getuid()) {
            const auto after = stat_at(dir.get());
            if (!after || *before != *after) ++report.unreadable_processes;
            continue;
        }
        auto comm = read_at(dir.get(), "comm", 256);
        auto maps = read_at(dir.get(), "maps", 4 * 1024 * 1024);
        std::array<char, 4097> link{};
        const auto length = readlinkat(dir.get(), "exe", link.data(), link.size());
        const auto exe = length > 0 && length < static_cast<ssize_t>(link.size()) ?
            normalize(std::string_view(link.data(), static_cast<std::size_t>(length))) : std::string{};
        if (!comm || !maps || comm->empty() || !comm->ends_with('\n') || exe.empty()) {
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
        const auto service = is_service(dir.get(), *pid, *before, proc_root_);
        const auto after = stat_at(dir.get());
        if (!service || !after || *before != *after) {
            ++report.unreadable_processes;
            continue;
        }
        auto paths = mapped_files(*maps);
        paths.insert(exe);
        bool exhausted = false;
        for (const auto& path : paths) {
            if (associations >= kMaxConsumerAssociations) { exhausted = true; break; }
            ++associations;
            report.files[path].service |= *service;
        }
        if (exhausted) { ++report.unreadable_processes; break; }
    }
    if (ec) ++report.unreadable_processes;
    report.coverage_complete = report.unreadable_processes == 0;
    return report;
}
}
