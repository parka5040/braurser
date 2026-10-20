#include <braurser/daemon/core/files.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <utility>
#include <vector>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace braurser::daemon::core {
namespace {

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const { return value_; }
    int release() { return std::exchange(value_, -1); }
    explicit operator bool() const { return value_ >= 0; }

private:
    int value_;
};

FileError failure(int code, std::string message) {
    return FileError{code, std::move(message)};
}

bool safe_part(const std::filesystem::path& part) {
    return !part.empty() && part != "." && part != ".." &&
           part.native().find('\0') == std::string::npos &&
           part.native().find('/') == std::string::npos;
}

std::expected<Fd, FileError> walk_directory(const std::filesystem::path& path,
                                              bool create = false) {
    if (!path.is_absolute()) return std::unexpected(failure(EINVAL, "directory path must be absolute"));
    Fd current(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!current) return std::unexpected(failure(errno, "root directory unavailable"));
    for (const auto& part : path.relative_path()) {
        if (!safe_part(part)) return std::unexpected(failure(EINVAL, "unsafe directory component"));
        const std::string name = part.string();
        if (create && mkdirat(current.get(), name.c_str(), 0700) != 0 && errno != EEXIST) {
            return std::unexpected(failure(errno, "cannot create directory"));
        }
        Fd next(openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!next) return std::unexpected(failure(errno, "directory is unavailable or symbolic"));
        current = std::move(next);
    }
    return current;
}

std::expected<Fd, FileError> parent_directory(const std::filesystem::path& path) {
    if (!path.is_absolute() || !safe_part(path.filename())) {
        return std::unexpected(failure(EINVAL, "invalid file path"));
    }
    return walk_directory(path.parent_path());
}

std::expected<void, FileError> remove_entry(int parent, const std::string& name, unsigned depth) {
    if (depth > 64) return std::unexpected(failure(ELOOP, "directory nesting limit exceeded"));
    struct stat entry {};
    if (fstatat(parent, name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(failure(errno, "cannot inspect entry"));
    }
    if (!S_ISDIR(entry.st_mode)) {
        if (unlinkat(parent, name.c_str(), 0) != 0) return std::unexpected(failure(errno, "cannot remove entry"));
        return {};
    }
    Fd child(openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!child) return std::unexpected(failure(errno, "cannot open subdirectory"));
    Fd duplicate(dup(child.get()));
    if (!duplicate) return std::unexpected(failure(errno, "cannot duplicate subdirectory"));
    DIR* directory = fdopendir(duplicate.get());
    if (!directory) return std::unexpected(failure(errno, "cannot iterate subdirectory"));
    duplicate.release();
    while (true) {
        errno = 0;
        dirent* item = readdir(directory);
        if (!item) {
            const int code = errno;
            closedir(directory);
            if (code != 0) return std::unexpected(failure(code, "cannot iterate subdirectory"));
            break;
        }
        const std::string child_name(item->d_name);
        if (child_name == "." || child_name == "..") continue;
        auto removed = remove_entry(child.get(), child_name, depth + 1);
        if (!removed) {
            closedir(directory);
            return removed;
        }
    }
    if (unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
        return std::unexpected(failure(errno, "cannot remove subdirectory"));
    }
    return {};
}

std::atomic<unsigned long long> temp_sequence{0};

}

std::expected<XdgPaths, FileError> resolve_xdg_paths() {
    const auto* home = std::getenv("HOME");
    auto resolve = [home](const char* key, const char* suffix) -> std::filesystem::path {
        const char* value = std::getenv(key);
        if (value && *value) return value;
        if (suffix && home && *home) return std::filesystem::path(home) / suffix;
        return {};
    };
    XdgPaths paths{resolve("XDG_CACHE_HOME", ".cache"),
                   resolve("XDG_STATE_HOME", ".local/state"),
                   resolve("XDG_CONFIG_HOME", ".config"),
                   resolve("XDG_RUNTIME_DIR", nullptr)};
    if (!paths.cache.is_absolute() || !paths.state.is_absolute() ||
        !paths.config.is_absolute() || !paths.runtime.is_absolute()) {
        return std::unexpected(failure(EINVAL, "XDG paths must be absolute; runtime is required"));
    }
    return paths;
}

std::expected<void, FileError> ensure_private_dir(const std::filesystem::path& path) {
    if (!path.is_absolute() || path == "/") {
        return std::unexpected(failure(EINVAL, "invalid private directory"));
    }
    auto parent = walk_directory(path.parent_path(), true);
    if (!parent) return std::unexpected(parent.error());
    const auto name = path.filename().string();
    if (!safe_part(path.filename())) return std::unexpected(failure(EINVAL, "invalid directory name"));
    if (mkdirat(parent->get(), name.c_str(), 0700) != 0 && errno != EEXIST) {
        return std::unexpected(failure(errno, "cannot create private directory"));
    }
    struct stat info {};
    if (fstatat(parent->get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(failure(errno, "cannot inspect private directory"));
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != getuid()) {
        return std::unexpected(failure(EPERM, "private directory owner or type refused"));
    }
    Fd target(openat(parent->get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!target) return std::unexpected(failure(errno, "private directory is symbolic"));
    if (fchmod(target.get(), 0700) != 0) {
        return std::unexpected(failure(errno, "cannot restrict private directory"));
    }
    return {};
}

std::expected<void, FileError> atomic_write(const std::filesystem::path& path,
                                              std::string_view bytes, mode_t mode) {
    if ((mode & 0077U) != 0 || (mode & ~static_cast<mode_t>(0777)) != 0) {
        return std::unexpected(failure(EINVAL, "unsafe file mode"));
    }
    auto parent = parent_directory(path);
    if (!parent) return std::unexpected(parent.error());
    const std::string filename = path.filename().string();
    struct stat existing {};
    if (fstatat(parent->get(), filename.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existing.st_mode) || existing.st_uid != getuid()) {
            return std::unexpected(failure(EPERM, "existing file type or owner refused"));
        }
    } else if (errno != ENOENT) {
        return std::unexpected(failure(errno, "cannot inspect destination"));
    }
    std::string temporary;
    Fd file;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = ".braurser-" + std::to_string(getpid()) + "-" +
                    std::to_string(temp_sequence.fetch_add(1));
        file = Fd(openat(parent->get(), temporary.c_str(),
                         O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_WRONLY, mode));
        if (file) break;
        if (errno != EEXIST) return std::unexpected(failure(errno, "cannot create temporary file"));
    }
    if (!file) return std::unexpected(failure(EEXIST, "temporary file collision"));
    auto cleanup = [&] { unlinkat(parent->get(), temporary.c_str(), 0); };
    if (fchmod(file.get(), mode) != 0) {
        const int code = errno;
        cleanup();
        return std::unexpected(failure(code, "cannot set file mode"));
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = std::min(bytes.size() - offset, static_cast<std::size_t>(SSIZE_MAX));
        const auto count = write(file.get(), bytes.data() + offset, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int code = count < 0 ? errno : EIO;
            cleanup();
            return std::unexpected(failure(code, "cannot write temporary file"));
        }
        offset += static_cast<std::size_t>(count);
    }
    if (fsync(file.get()) != 0) {
        const int code = errno;
        cleanup();
        return std::unexpected(failure(code, "cannot sync temporary file"));
    }
    if (renameat(parent->get(), temporary.c_str(), parent->get(), filename.c_str()) != 0) {
        const int code = errno;
        cleanup();
        return std::unexpected(failure(code, "cannot replace file"));
    }
    if (fsync(parent->get()) != 0) return std::unexpected(failure(errno, "cannot sync directory"));
    return {};
}

std::expected<std::string, FileError> read_bounded(const std::filesystem::path& path,
                                                     std::size_t max_bytes, bool follow_symlinks) {
    auto parent = parent_directory(path);
    if (!parent) return std::unexpected(parent.error());
    Fd file(openat(parent->get(), path.filename().c_str(),
                   O_RDONLY | O_CLOEXEC | O_NONBLOCK | (follow_symlinks ? 0 : O_NOFOLLOW)));
    if (!file) return std::unexpected(failure(errno, "cannot open input file"));
    struct stat info {};
    if (fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::unexpected(failure(errno == 0 ? EINVAL : errno, "input is not a regular file"));
    }
    if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > max_bytes) {
        return std::unexpected(failure(EFBIG, "input exceeds limit"));
    }
    std::string result;
    std::array<char, 8192> buffer{};
    while (true) {
        const auto count = read(file.get(), buffer.data(),
                                std::min(buffer.size(), max_bytes - result.size() +
                                                           static_cast<std::size_t>(result.size() == max_bytes)));
        if (count == 0) return result;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return std::unexpected(failure(errno, "cannot read input file"));
        if (static_cast<std::size_t>(count) > max_bytes - result.size()) {
            return std::unexpected(failure(EFBIG, "input exceeds limit"));
        }
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

std::expected<std::vector<FileEntry>, FileError> list_regular_files(
    const std::filesystem::path& root, std::size_t max_entries) {
    auto fd = walk_directory(root);
    if (!fd) return std::unexpected(fd.error());
    Fd duplicate(dup(fd->get()));
    if (!duplicate) return std::unexpected(failure(errno, "cannot duplicate directory"));
    DIR* raw = fdopendir(duplicate.get());
    if (!raw) return std::unexpected(failure(errno, "cannot iterate directory"));
    duplicate.release();
    auto directory = std::unique_ptr<DIR, void(*)(DIR*)>(raw, [](DIR* value) { closedir(value); });
    std::vector<FileEntry> result;
    while (true) {
        errno = 0;
        const auto* item = readdir(directory.get());
        if (!item) {
            if (errno != 0) return std::unexpected(failure(errno, "cannot iterate directory"));
            return result;
        }
        const std::string name(item->d_name);
        if (name == "." || name == "..") continue;
        struct stat info{};
        if (fstatat(fd->get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
            return std::unexpected(failure(errno, "cannot inspect cache entry"));
        if (!S_ISREG(info.st_mode)) continue;
        if (result.size() >= max_entries) return std::unexpected(failure(EFBIG, "directory entry limit exceeded"));
        result.push_back({name, info});
    }
}

std::expected<void, FileError> touch_file(const std::filesystem::path& path) {
    auto parent = parent_directory(path);
    if (!parent) return std::unexpected(parent.error());
    Fd file(openat(parent->get(), path.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!file) return std::unexpected(failure(errno, "cannot open cache entry"));
    struct stat info{};
    if (fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode))
        return std::unexpected(failure(EINVAL, "cache entry is not a regular file"));
    if (futimens(file.get(), nullptr) != 0) return std::unexpected(failure(errno, "cannot touch cache entry"));
    return {};
}

std::expected<void, FileError> remove_tree_confined(const std::filesystem::path& root,
                                                      const std::filesystem::path& sub) {
    auto directory = walk_directory(root);
    if (!directory) return std::unexpected(directory.error());
    std::filesystem::path relative;
    if (sub.is_absolute()) {
        relative = sub.lexically_relative(root);
    } else {
        relative = sub;
    }
    if (relative.empty() || relative == "." || relative.is_absolute()) {
        return std::unexpected(failure(EINVAL, "subtree must be within root"));
    }
    std::vector<std::string> parts;
    for (const auto& component : relative) {
        if (!safe_part(component)) return std::unexpected(failure(EINVAL, "subtree escapes root"));
        parts.push_back(component.string());
    }
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        Fd next(openat(directory->get(), parts[index].c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!next) return std::unexpected(failure(errno, "subtree directory unavailable"));
        directory = std::move(next);
    }
    return remove_entry(directory->get(), parts.back(), 0);
}

}
