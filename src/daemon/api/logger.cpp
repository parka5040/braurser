#include <braurser/daemon/api/logger.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/core/text.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <format>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::api {
namespace {
constexpr off_t max_log_bytes = 1024 * 1024;

bool safe_name(std::string_view name) {
    return !name.empty() && name.size() <= 48 &&
           std::ranges::all_of(name, [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
           });
}

bool safe_file(int dir, const char* name) {
    struct stat info{};
    if (fstatat(dir, name, &info, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
    return S_ISREG(info.st_mode) && info.st_uid == getuid() && (info.st_mode & 0777) == 0600;
}

void rotate(int dir) {
    for (const char* name : {"daemon.log", "daemon.log.1", "daemon.log.2", "daemon.log.3"})
        if (!safe_file(dir, name)) throw std::runtime_error("unsafe daemon log");
    if (unlinkat(dir, "daemon.log.3", 0) != 0 && errno != ENOENT)
        throw std::runtime_error("cannot rotate daemon log");
    if (renameat(dir, "daemon.log.2", dir, "daemon.log.3") != 0 && errno != ENOENT)
        throw std::runtime_error("cannot rotate daemon log");
    if (renameat(dir, "daemon.log.1", dir, "daemon.log.2") != 0 && errno != ENOENT)
        throw std::runtime_error("cannot rotate daemon log");
    if (renameat(dir, "daemon.log", dir, "daemon.log.1") != 0 && errno != ENOENT)
        throw std::runtime_error("cannot rotate daemon log");
}

std::string safe_value(std::string_view raw) {
    const std::string clean = core::bounded_line(core::redact(raw), 256);
    std::string value;
    value.reserve(clean.size());
    for (const char raw_c : clean) {
        const auto c = static_cast<unsigned char>(raw_c);
        if (c <= 32 || c == 127 || c == '=' || c == '\\' || c == '"') value += '_';
        else value += static_cast<char>(c);
    }
    return value;
}
}

DaemonLog::DaemonLog(const std::filesystem::path& state_home) : directory_(state_home / "braurser") {
    if (!core::ensure_private_dir(directory_)) throw std::runtime_error("private daemon state unavailable");
}

void DaemonLog::write(std::string_view level, std::string_view component, std::string_view event,
                      std::initializer_list<std::pair<std::string_view, std::string_view>> values) {
    if (!safe_name(level) || !safe_name(component) || !safe_name(event) || values.size() > 16)
        throw std::invalid_argument("invalid daemon log event");
    auto line = std::format("{:%FT%TZ} {} {} {}", std::chrono::floor<std::chrono::seconds>(
                            std::chrono::system_clock::now()), level, component, event);
    for (const auto& [key, value] : values) {
        if (!safe_name(key)) throw std::invalid_argument("invalid daemon log key");
        line += ' ';
        line += key;
        line += '=';
        line += safe_value(value);
    }
    line += '\n';
    if (line.size() > 4096) throw std::invalid_argument("daemon log line exceeds limit");
    std::lock_guard lock(mutex_);
    const int dir = open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) throw std::runtime_error("cannot open daemon log directory");
    try {
        if (!safe_file(dir, "daemon.log")) throw std::runtime_error("unsafe daemon log file");
        int fd = openat(dir, "daemon.log", O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat info{};
        if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_uid != getuid() || (info.st_mode & 0777) != 0600) {
            if (fd >= 0) close(fd);
            throw std::runtime_error("cannot open private daemon log");
        }
        if (info.st_size > max_log_bytes - static_cast<off_t>(line.size())) {
            close(fd);
            rotate(dir);
            fd = openat(dir, "daemon.log", O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
                info.st_uid != getuid() || (info.st_mode & 0777) != 0600) {
                if (fd >= 0) close(fd);
                throw std::runtime_error("cannot open private rotated log");
            }
        }
        const auto written = ::write(fd, line.data(), line.size());
        close(fd);
        if (written != static_cast<ssize_t>(line.size())) throw std::runtime_error("daemon log write failed");
    } catch (...) {
        close(dir);
        throw;
    }
    close(dir);
}

}
