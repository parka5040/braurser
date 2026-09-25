#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/validate/validate.hpp>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::manual {
namespace {

ManualError fail(std::string code) { return {std::move(code), {}, {}, {}}; }

class Fd {
public:
    explicit Fd(int handle = -1) : handle_(handle) {}
    ~Fd() { if (handle_ >= 0) close(handle_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (handle_ >= 0) close(handle_); handle_ = std::exchange(other.handle_, -1); }
        return *this;
    }
    int get() const { return handle_; }
    explicit operator bool() const { return handle_ >= 0; }
private:
    int handle_;
};

bool safe_part(const std::filesystem::path& part) {
    auto text = part.string();
    return !text.empty() && text != "." && text != ".." && text.size() <= 255 &&
           text.find('/') == text.npos && text.find('\0') == text.npos;
}

bool safe_relative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.native().size() > 4096) return false;
    for (const auto& part : path) if (!safe_part(part)) return false;
    return true;
}

std::expected<Fd, ManualError> directory(const std::filesystem::path& path, bool create) {
    if (!path.is_absolute() || path == "/") return std::unexpected(fail("install-path"));
    Fd current(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!current) return std::unexpected(fail("install-root"));
    for (const auto& part : path.relative_path()) {
        if (!safe_part(part)) return std::unexpected(fail("install-path"));
        const auto name = part.string();
        if (create && mkdirat(current.get(), name.c_str(), 0700) != 0 && errno != EEXIST)
            return std::unexpected(fail("install-mkdir"));
        Fd next(openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!next) return std::unexpected(fail("install-dir"));
        current = std::move(next);
    }
    return current;
}

std::string sha(std::string_view bytes) {
    br_sha256_ctx ctx{};
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    std::array<std::uint8_t, 32> hash{};
    br_sha256_final(&ctx, hash.data());
    std::array<char, 65> hex{};
    br_hex32(hash.data(), hex.data());
    return hex.data();
}

nlohmann::json encode(const Receipt& receipt) {
    nlohmann::json files = nlohmann::json::array();
    for (const auto& file : receipt.files) files.push_back({{"path", file.path}, {"sha256", file.sha256}, {"mode", file.mode}});
    nlohmann::json backups = nlohmann::json::array();
    for (const auto& [path, backup] : receipt.backups) backups.push_back({{"path", path}, {"backup", backup}});
    nlohmann::json data = { {"id", receipt.id}, {"name", receipt.name}, {"url", receipt.url},
        {"host", receipt.host}, {"commit", receipt.commit}, {"entry_point", receipt.entry_point},
        {"scope", receipt.scope == Scope::User ? "user" : "system"},
        {"installed_at", receipt.installed_at}, {"files", files}, {"backups", backups},
        {"launcher_ids", receipt.launcher_ids} };
    if (!receipt.pkgname.empty()) data["pkgname"] = receipt.pkgname;
    return data;
}

void update_caches(core::ProcessRunner& runner, const std::filesystem::path& prefix) {
    for (const auto& [program, relative] : std::array<std::pair<const char*, const char*>, 3>{{
        {"/usr/bin/update-desktop-database", "share/applications"},
        {"/usr/bin/update-mime-database", "share/mime"},
        {"/usr/bin/gtk-update-icon-cache", "share/icons/hicolor"}}}) {
        if (access(program, X_OK) != 0 || !std::filesystem::is_directory(prefix / relative)) continue;
        core::ProcessSpec spec;
        spec.executable = program;
        if (std::string_view(program).ends_with("gtk-update-icon-cache")) spec.args = {"-f", "-t"};
        spec.args.push_back((prefix / relative).string());
        spec.env = {{"PATH", "/usr/bin"}, {"LANG", "C.UTF-8"}};
        spec.timeout = std::chrono::seconds(30);
        spec.max_stdout = spec.max_stderr = 64 * 1024;
        (void)runner.run(spec);
    }
}

}

std::expected<Receipt, ManualError> install_user(
    core::ProcessRunner& runner, const std::filesystem::path& stage,
    const std::filesystem::path& home, const std::filesystem::path& state,
    Receipt receipt) {
    auto source = validate_git_url(receipt.url);
    if (receipt.scope != Scope::User || !home.is_absolute() || !state.is_absolute() ||
        !safe_part(receipt.id) || receipt.id.size() > 128 ||
        !std::all_of(receipt.id.begin(), receipt.id.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-';
        }) || receipt.name.empty() || receipt.name.size() > 256 ||
        receipt.name.find_first_of("\r\n") != receipt.name.npos ||
        receipt.name.find('\0') != receipt.name.npos ||
        !source || receipt.host != source->host ||
        (receipt.entry_point != "install.sh" && receipt.entry_point != "Makefile") ||
        receipt.commit.size() != 40 || !std::all_of(receipt.commit.begin(), receipt.commit.end(),
            [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
        return std::unexpected(fail("receipt-input"));
    auto manifest = validate_stage(stage, Scope::User);
    if (!manifest) return std::unexpected(manifest.error());
    auto prefix = home / ".local";
    auto receipt_dir = state / "braurser/manual/receipts";
    auto backup_dir = state / "braurser/manual/backups" / receipt.id;
    for (const auto& path : {state / "braurser", state / "braurser/manual",
                            state / "braurser/manual/receipts", state / "braurser/manual/backups", backup_dir}) {
        if (!core::ensure_private_dir(path)) return std::unexpected(fail("receipt-directory"));
    }
    auto receipt_file = receipt_dir / (receipt.id + ".json");
    if (std::filesystem::exists(receipt_file)) return std::unexpected(fail("receipt-exists"));
    std::set<std::string> previously_owned;
    std::uint64_t receipt_bytes = 0;
    std::size_t receipt_count = 0;
    for (const auto& item : std::filesystem::directory_iterator(receipt_dir)) {
        if (++receipt_count > 50000) return std::unexpected(fail("receipt-limit"));
        if (item.path().extension() != ".json") continue;
        auto content = core::read_bounded(item.path(), 8 * 1024 * 1024);
        if (!content || content->size() > 64ULL * 1024 * 1024 - receipt_bytes ||
            !nlohmann::json::accept(*content)) return std::unexpected(fail("receipt-read"));
        receipt_bytes += content->size();
        auto record = nlohmann::json::parse(*content);
        if (!record.is_object() || !record.contains("scope") || !record["scope"].is_string() ||
            !record.contains("files") || !record["files"].is_array())
            return std::unexpected(fail("receipt-read"));
        if (record["scope"] != "user") continue;
        for (const auto& installed : record["files"]) {
            if (!installed.is_object() || !installed.contains("path") || !installed["path"].is_string())
                return std::unexpected(fail("receipt-read"));
            auto path = installed["path"].get<std::string>();
            if (!safe_relative(std::filesystem::path(path))) return std::unexpected(fail("receipt-read"));
            previously_owned.insert(std::move(path));
        }
    }
    auto root = directory(prefix, true);
    if (!root) return std::unexpected(root.error());
    for (const auto& item : std::filesystem::recursive_directory_iterator(stage)) {
        auto path = item.path().lexically_relative(stage);
        if (!safe_relative(path)) return std::unexpected(fail("stage-path"));
        if (item.is_directory()) {
            auto created = directory(prefix / path, true);
            if (!created) return std::unexpected(created.error());
        }
    }
    receipt.files.clear();
    receipt.backups.clear();
    for (const auto& file : manifest->files) {
        auto relative = std::filesystem::path(file.path);
        if (!safe_relative(relative)) return std::unexpected(fail("stage-path"));
        const auto parent = relative.parent_path();
        auto dest_dir = directory(prefix / parent, true);
        auto source_dir = directory(stage / parent, false);
        auto backup_parent = directory(backup_dir / parent, true);
        if (!dest_dir || !source_dir || !backup_parent) return std::unexpected(fail("install-directory"));
        const auto name = relative.filename().string();
        struct stat existing {};
        if (fstatat(dest_dir->get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(existing.st_mode) && !S_ISLNK(existing.st_mode))
                return std::unexpected(fail("install-conflict"));
            if (previously_owned.contains(file.path)) {
                if (unlinkat(dest_dir->get(), name.c_str(), 0) != 0)
                    return std::unexpected(fail("install-replace"));
            } else {
                if (renameat(dest_dir->get(), name.c_str(), backup_parent->get(), name.c_str()) != 0)
                    return std::unexpected(fail("install-backup"));
                receipt.backups.emplace_back(file.path, file.path);
            }
        } else if (errno != ENOENT) return std::unexpected(fail("install-stat"));
        if (file.mode == 0120000U) {
            std::array<char, 4097> buffer{};
            const auto length = readlinkat(source_dir->get(), name.c_str(), buffer.data(), buffer.size());
            if (length <= 0 || length >= static_cast<ssize_t>(buffer.size()) ||
                sha(std::string_view(buffer.data(), static_cast<std::size_t>(length))) != file.sha256 ||
                symlinkat(std::string(buffer.data(), static_cast<std::size_t>(length)).c_str(),
                          dest_dir->get(), name.c_str()) != 0)
                return std::unexpected(fail("install-link"));
            receipt.files.push_back(file);
            continue;
        }
        Fd in(openat(source_dir->get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        Fd out(openat(dest_dir->get(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                      file.mode & 0111U ? 0700 : 0600));
        if (!in || !out) return std::unexpected(fail("install-open"));
        struct stat source_stat {};
        if (fstat(in.get(), &source_stat) != 0 || !S_ISREG(source_stat.st_mode))
            return std::unexpected(fail("install-source"));
        br_sha256_ctx copied{};
        br_sha256_init(&copied);
        std::array<char, 8192> buffer{};
        while (true) {
            auto amount = read(in.get(), buffer.data(), buffer.size());
            if (amount < 0 && errno == EINTR) continue;
            if (amount < 0) return std::unexpected(fail("install-read"));
            if (amount == 0) break;
            br_sha256_update(&copied, reinterpret_cast<const std::uint8_t*>(buffer.data()),
                             static_cast<std::size_t>(amount));
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(amount)) {
                auto written = write(out.get(), buffer.data() + offset,
                                     static_cast<std::size_t>(amount) - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) return std::unexpected(fail("install-write"));
                offset += static_cast<std::size_t>(written);
            }
        }
        std::array<std::uint8_t, 32> copied_hash{};
        br_sha256_final(&copied, copied_hash.data());
        std::array<char, 65> copied_hex{};
        br_hex32(copied_hash.data(), copied_hex.data());
        if (std::string_view(copied_hex.data()) != file.sha256)
            return std::unexpected(fail("install-changed"));
        receipt.files.push_back({file.path, file.sha256, file.mode & 0111U ? 0700U : 0600U});
    }
    receipt.launcher_ids = manifest->launchers;
    receipt.installed_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (!core::atomic_write(receipt_file, encode(receipt).dump(), 0600))
        return std::unexpected(fail("receipt-write"));
    update_caches(runner, prefix);
    return receipt;
}

std::expected<RemovalResult, ManualError> remove_user(
    core::ProcessRunner& runner, const std::filesystem::path& home,
    const std::filesystem::path& state, const Receipt& receipt) {
    if (receipt.scope != Scope::User || !home.is_absolute() || !state.is_absolute() ||
        !safe_part(receipt.id)) return std::unexpected(fail("receipt-input"));
    auto saved = core::read_bounded(state / "braurser/manual/receipts" / (receipt.id + ".json"), 8 * 1024 * 1024);
    if (!saved || !nlohmann::json::accept(*saved) || nlohmann::json::parse(*saved) != encode(receipt))
        return std::unexpected(fail("receipt-mismatch"));
    auto prefix = home / ".local";
    auto backup_root = state / "braurser/manual/backups" / receipt.id;
    RemovalResult result;
    auto restore = [&](const std::string& path, int parent, const std::string& name)
        -> std::expected<void, ManualError> {
        for (const auto& [original, backup] : receipt.backups) {
            if (original != path) continue;
            if (!safe_relative(backup)) return std::unexpected(fail("backup-path"));
            const auto relative = std::filesystem::path(backup);
            auto backup_parent = directory(backup_root / relative.parent_path(), false);
            if (!backup_parent || renameat(backup_parent->get(), relative.filename().c_str(),
                                           parent, name.c_str()) != 0)
                return std::unexpected(fail("backup-restore"));
        }
        return {};
    };
    for (const auto& file : receipt.files) {
        auto relative = std::filesystem::path(file.path);
        if (!safe_relative(relative)) return std::unexpected(fail("receipt-path"));
        auto parent = directory(prefix / relative.parent_path(), false);
        if (!parent) return std::unexpected(parent.error());
        auto name = relative.filename().string();
        if (file.mode == 0120000U) {
            std::array<char, 4097> link{};
            const auto length = readlinkat(parent->get(), name.c_str(), link.data(), link.size());
            if (length < 0 && errno == ENOENT) {
                if (auto restored = restore(file.path, parent->get(), name); !restored)
                    return std::unexpected(restored.error());
                continue;
            }
            if (length <= 0 || length >= static_cast<ssize_t>(link.size()) ||
                sha(std::string_view(link.data(), static_cast<std::size_t>(length))) != file.sha256) {
                result.modified_files.push_back(file.path);
                continue;
            }
            if (unlinkat(parent->get(), name.c_str(), 0) != 0) return std::unexpected(fail("remove-file"));
            if (auto restored = restore(file.path, parent->get(), name); !restored)
                return std::unexpected(restored.error());
            continue;
        }
        Fd existing(openat(parent->get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (!existing) {
            if (errno == ENOENT) {
                if (auto restored = restore(file.path, parent->get(), name); !restored)
                    return std::unexpected(restored.error());
                continue;
            }
            result.modified_files.push_back(file.path);
            continue;
        }
        struct stat info {};
        if (fstat(existing.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
            static_cast<std::uint64_t>(info.st_size) > 2ULL * 1024 * 1024 * 1024) {
            result.modified_files.push_back(file.path);
            continue;
        }
        auto bytes = core::read_bounded(prefix / relative, 2ULL * 1024 * 1024 * 1024);
        if (!bytes || sha(*bytes) != file.sha256) {
            result.modified_files.push_back(file.path);
            continue;
        }
        if (unlinkat(parent->get(), name.c_str(), 0) != 0) return std::unexpected(fail("remove-file"));
        if (auto restored = restore(file.path, parent->get(), name); !restored)
            return std::unexpected(restored.error());
    }
    update_caches(runner, prefix);
    if (!result.modified_files.empty()) return result;
    if (unlink((state / "braurser/manual/receipts" / (receipt.id + ".json")).c_str()) != 0)
        return std::unexpected(fail("receipt-delete"));
    return result;
}

std::expected<void, ManualError> record_system_install(
    const std::filesystem::path& state, const Receipt& receipt) {
    if (!state.is_absolute() || receipt.scope != Scope::System ||
        !safe_part(receipt.id) || receipt.id.size() > 128 ||
        !std::all_of(receipt.id.begin(), receipt.id.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-';
        }) || !br_valid_pkgname(receipt.pkgname.data(), receipt.pkgname.size()) ||
        !receipt.pkgname.starts_with("braurser-git-") || !validate_git_url(receipt.url) ||
        receipt.files.empty()) return std::unexpected(fail("receipt-input"));
    auto dir = state / "braurser/manual/receipts";
    for (const auto& part : {state / "braurser", state / "braurser/manual", dir}) {
        if (!core::ensure_private_dir(part)) return std::unexpected(fail("receipt-directory"));
    }
    const auto path = dir / (receipt.id + ".json");
    if (std::filesystem::exists(path) || !core::atomic_write(path, encode(receipt).dump(), 0600))
        return std::unexpected(fail("receipt-write"));
    return {};
}

std::expected<Receipt, ManualError> upgrade_user(
    core::ProcessRunner& runner, const std::filesystem::path& stage,
    const std::filesystem::path& home, const std::filesystem::path& state,
    const Receipt& previous, Receipt next) {
    if (previous.scope != Scope::User || next.scope != Scope::User ||
        previous.id != next.id || previous.url != next.url ||
        previous.commit == next.commit || !home.is_absolute() || !state.is_absolute())
        return std::unexpected(fail("upgrade-input"));
    auto validated = validate_stage(stage, Scope::User);
    if (!validated) return std::unexpected(validated.error());
    for (const auto& file : previous.files) {
        auto relative = std::filesystem::path(file.path);
        if (!safe_relative(relative)) return std::unexpected(fail("upgrade-path"));
        auto parent = directory(home / ".local" / relative.parent_path(), false);
        if (!parent) return std::unexpected(parent.error());
        const auto name = relative.filename().string();
        struct stat info {};
        if (fstatat(parent->get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            return std::unexpected(fail("upgrade-stat"));
        }
        if (file.mode == 0120000U) {
            std::array<char, 4097> target{};
            auto length = readlinkat(parent->get(), name.c_str(), target.data(), target.size());
            if (length <= 0 || length >= static_cast<ssize_t>(target.size()) ||
                sha(std::string_view(target.data(), static_cast<std::size_t>(length))) != file.sha256)
                return std::unexpected(fail("upgrade-modified"));
        } else {
            auto data = core::read_bounded(home / ".local" / relative, 2ULL * 1024 * 1024 * 1024);
            if (!data || sha(*data) != file.sha256) return std::unexpected(fail("upgrade-modified"));
        }
    }
    auto removed = remove_user(runner, home, state, previous);
    if (!removed || !removed->modified_files.empty()) return std::unexpected(fail("upgrade-remove"));
    return install_user(runner, stage, home, state, std::move(next));
}

}
