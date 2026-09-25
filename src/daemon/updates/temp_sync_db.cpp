#include <braurser/daemon/updates/updates.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/validate/validate.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace braurser::daemon::updates {
namespace {
struct Fd {
    int value{-1};
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    explicit Fd(int v) : value(v) {}
};
std::atomic<unsigned long long> counter{0};
bool valid_path(const std::filesystem::path& path) {
    if (!path.is_absolute() || path == "/" || path.native().size() > 4096 ||
        !std::ranges::all_of(path.native(), [](unsigned char c) { return c >= 0x20 && c != 0x7f; })) return false;
    for (const auto& part : path.relative_path())
        if (part.empty() || part == "." || part == "..") return false;
    return true;
}
UpdateError file_error() { return {UpdateErrorCode::FileFailure, "private database setup failed"}; }
std::expected<void, UpdateError> copy_db(int source, int target, const std::string& name, std::uint64_t& total) {
    Fd input(openat(source, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (input.value < 0) return std::unexpected(file_error());
    struct stat info{};
    constexpr std::uint64_t file_limit = 512ULL * 1024 * 1024;
    constexpr std::uint64_t total_limit = 768ULL * 1024 * 1024;
    if (fstat(input.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > file_limit ||
        static_cast<std::uint64_t>(info.st_size) > total_limit - total) return std::unexpected(file_error());
    Fd output(openat(target, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (output.value < 0) return std::unexpected(file_error());
    std::array<char, 16384> bytes{};
    std::uint64_t copied = 0;
    while (true) {
        const auto count = read(input.value, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return std::unexpected(file_error());
        if (count == 0) break;
        if (static_cast<std::uint64_t>(count) > file_limit - copied ||
            static_cast<std::uint64_t>(count) > total_limit - total - copied) return std::unexpected(file_error());
        copied += static_cast<std::uint64_t>(count);
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(count)) {
            const auto written = write(output.value, bytes.data() + offset, static_cast<std::size_t>(count) - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return std::unexpected(file_error());
            offset += static_cast<std::size_t>(written);
        }
    }
    if (copied != static_cast<std::uint64_t>(info.st_size)) return std::unexpected(file_error());
    total += copied;
    return {};
}
std::expected<void, UpdateError> prepare(const std::filesystem::path& real_db, const std::filesystem::path& temp) {
    Fd real(open(real_db.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Fd private_db(open(temp.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (real.value < 0 || private_db.value < 0) return std::unexpected(file_error());
    Fd real_local(openat(real.value, "local", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Fd real_sync(openat(real.value, "sync", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (real_local.value < 0 || real_sync.value < 0 ||
        symlinkat((real_db / "local").c_str(), private_db.value, "local") != 0 ||
        mkdirat(private_db.value, "sync", 0700) != 0) return std::unexpected(file_error());
    Fd sync(openat(private_db.value, "sync", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (sync.value < 0) return std::unexpected(file_error());
    std::error_code ec;
    std::filesystem::directory_iterator it(real_db / "sync", ec);
    if (ec) return std::unexpected(file_error());
    std::size_t files = 0;
    std::uint64_t total = 0;
    for (const auto end = std::filesystem::directory_iterator{}; it != end; it.increment(ec)) {
        if (ec) return std::unexpected(file_error());
        if (++files > 256) return std::unexpected(file_error());
        const auto name = it->path().filename().string();
        if (!name.ends_with(".db")) continue;
        const std::string_view repo(name.data(), name.size() - 3);
        if (!br_valid_repo_name(repo.data(), repo.size())) return std::unexpected(file_error());
        if (auto copied = copy_db(real_sync.value, sync.value, name, total); !copied)
            return std::unexpected(copied.error());
    }
    if (ec || total == 0) return std::unexpected(file_error());
    return {};
}
}

TempSyncDb::TempSyncDb(std::filesystem::path root, std::filesystem::path path)
    : root_(std::move(root)), path_(std::move(path)) {}
TempSyncDb::TempSyncDb(TempSyncDb&& other) noexcept
    : root_(std::move(other.root_)), path_(std::exchange(other.path_, {})) {}
TempSyncDb& TempSyncDb::operator=(TempSyncDb&& other) noexcept {
    if (this != &other) {
        if (!path_.empty()) static_cast<void>(core::remove_tree_confined(root_, path_));
        root_ = std::move(other.root_);
        path_ = std::exchange(other.path_, {});
    }
    return *this;
}
TempSyncDb::~TempSyncDb() {
    if (!path_.empty()) static_cast<void>(core::remove_tree_confined(root_, path_));
}

std::expected<TempSyncDb, UpdateError> TempSyncDb::create(core::ProcessRunner& runner,
    const std::filesystem::path& real_db, const std::filesystem::path& runtime,
    const std::filesystem::path& fakeroot) {
    if (!valid_path(real_db) || !valid_path(runtime) || !valid_path(fakeroot))
        return std::unexpected(UpdateError{UpdateErrorCode::InvalidInput, "invalid temporary database path"});
    struct stat binary{};
    if (stat(fakeroot.c_str(), &binary) != 0 || !S_ISREG(binary.st_mode) || access(fakeroot.c_str(), X_OK) != 0)
        return std::unexpected(UpdateError{UpdateErrorCode::Unavailable, "fakeroot is unavailable; install base-devel"});
    const auto root = runtime / "braurser" / "checkdb";
    if (auto ensured = core::ensure_private_dir(root); !ensured)
        return std::unexpected(file_error());
    Fd parent(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parent.value < 0) return std::unexpected(file_error());
    std::filesystem::path temp;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto name = "db-" + std::to_string(getpid()) + "-" + std::to_string(counter.fetch_add(1));
        if (mkdirat(parent.value, name.c_str(), 0700) == 0) { temp = root / name; break; }
        if (errno != EEXIST) return std::unexpected(file_error());
    }
    if (temp.empty()) return std::unexpected(file_error());
    TempSyncDb result(root, temp);
    if (auto created = prepare(real_db, temp); !created) return std::unexpected(created.error());
    core::ProcessSpec spec;
    spec.executable = fakeroot;
    spec.args = {"--", "/usr/bin/pacman", "-Sy", "--dbpath", temp.string(), "--logfile", "/dev/null", "--disable-sandbox"};
    spec.env = {{"PATH", "/usr/bin"}, {"LC_ALL", "C"}};
    spec.timeout = std::chrono::minutes(5);
    spec.max_stdout = spec.max_stderr = 1024 * 1024;
    auto refreshed = runner.run(spec);
    if (!refreshed || refreshed->exit_code != 0 || refreshed->signaled || refreshed->timed_out ||
        refreshed->out_truncated || refreshed->err_truncated ||
        refreshed->out.size() > spec.max_stdout || refreshed->err.size() > spec.max_stderr)
        return std::unexpected(UpdateError{UpdateErrorCode::ProcessFailure, "private sync database refresh failed"});
    return result;
}

}
