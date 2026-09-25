#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/core/text.hpp>
#include <braurser/validate/validate.hpp>

#include <libxml/parser.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::manual {
namespace {

ManualError fail(std::string code, std::string detail = {}) {
    return {std::move(code), std::move(detail), {}, {}};
}

std::string hex_digest(br_sha256_ctx& context) {
    std::array<std::uint8_t, 32> bytes{};
    br_sha256_final(&context, bytes.data());
    std::array<char, 65> hex{};
    br_hex32(bytes.data(), hex.data());
    return hex.data();
}

std::string digest(std::string_view data) {
    br_sha256_ctx context{};
    br_sha256_init(&context);
    br_sha256_update(&context, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return hex_digest(context);
}

std::expected<std::string, ManualError> file_digest(const std::filesystem::path& path, std::uint64_t size) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return std::unexpected(fail("stage-open"));
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) != size) {
        close(fd);
        return std::unexpected(fail("stage-changed"));
    }
    br_sha256_ctx context{};
    br_sha256_init(&context);
    std::uint64_t read_bytes = 0;
    std::array<std::uint8_t, 65536> bytes{};
    while (true) {
        const auto length = read(fd, bytes.data(), bytes.size());
        if (length < 0 && errno == EINTR) continue;
        if (length < 0 || static_cast<std::uint64_t>(length) > size - read_bytes) {
            close(fd);
            return std::unexpected(fail("stage-read"));
        }
        if (length == 0) break;
        br_sha256_update(&context, bytes.data(), static_cast<std::size_t>(length));
        read_bytes += static_cast<std::uint64_t>(length);
    }
    close(fd);
    if (read_bytes != size) return std::unexpected(fail("stage-changed"));
    return hex_digest(context);
}

bool within(std::filesystem::path path) {
    if (path.empty() || path.is_absolute()) return false;
    int depth = 0;
    for (const auto& part : path) {
        if (part == "..") { if (--depth < 0) return false; }
        else if (part != "." && !part.empty()) ++depth;
    }
    return depth > 0;
}

bool stage_link(const std::filesystem::path& root, const std::filesystem::path& item,
                const std::filesystem::path& target) {
    if (target.empty() || target.is_absolute() || target.native().find('\0') != std::string::npos)
        return false;
    auto relative = (item.lexically_relative(root).parent_path() / target).lexically_normal();
    if (!within(relative)) return false;
    std::error_code error;
    const auto resolved = std::filesystem::canonical(root / relative, error);
    return !error && within(resolved.lexically_relative(root));
}

}

std::expected<StageManifest, ManualError> validate_stage(
    const std::filesystem::path& stage, Scope scope) {
    if ((scope != Scope::User && scope != Scope::System) ||
        !stage.is_absolute() || std::filesystem::is_symlink(stage) ||
        !std::filesystem::is_directory(stage)) return std::unexpected(fail("stage-root"));
    StageManifest manifest;
    std::vector<std::string> refused;
    std::uint64_t total = 0;
    std::size_t count = 0;
    const std::set<std::string> allowed{"bin", "lib", "lib64", "libexec", "share", "include", "etc"};
    std::error_code ec;
    std::filesystem::recursive_directory_iterator iterator(stage, std::filesystem::directory_options::none, ec);
    if (ec) return std::unexpected(fail("stage-read"));
    for (; iterator != std::filesystem::recursive_directory_iterator{}; iterator.increment(ec)) {
        if (ec) return std::unexpected(fail("stage-read"));
        const auto& item = *iterator;
        const auto relative = item.path().lexically_relative(stage);
        const auto name = relative.generic_string();
        if (++count > 50000 || name.size() > 4096 || !within(relative))
            return std::unexpected(fail("stage-limit"));
        auto top = relative.begin()->string();
        if (!allowed.contains(top) || (scope == Scope::User && top == "etc")) refused.push_back(name);
        struct stat info {};
        if (lstat(item.path().c_str(), &info) != 0) return std::unexpected(fail("stage-stat"));
        if ((info.st_mode & 07000) != 0 || info.st_uid != getuid()) refused.push_back(name);
        if (S_ISLNK(info.st_mode)) {
            auto target = std::filesystem::read_symlink(item.path(), ec);
            if (ec || !stage_link(stage, item.path(), target)) refused.push_back(name);
            else {
                auto content = target.generic_string();
                manifest.files.push_back({name, digest(content), 0120000U});
            }
        } else if (S_ISDIR(info.st_mode)) {
            if (iterator.depth() > 64) return std::unexpected(fail("stage-depth"));
        } else if (S_ISREG(info.st_mode)) {
            if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > 2ULL * 1024 * 1024 * 1024 - total)
                return std::unexpected(fail("stage-limit"));
            total += static_cast<std::uint64_t>(info.st_size);
            auto hash = file_digest(item.path(), static_cast<std::uint64_t>(info.st_size));
            if (!hash) return std::unexpected(hash.error());
            manifest.files.push_back({name, std::move(*hash), static_cast<unsigned>(info.st_mode & 0777)});
            if (name.starts_with("share/applications/") && name.ends_with(".desktop")) {
                auto data = core::read_bounded(item.path(), 65536);
                if (!data) { refused.push_back(name); continue; }
                auto entry = catalog::parse_desktop_entry(*data, "C.UTF-8");
                if (!entry) refused.push_back(name);
                else {
                    manifest.launchers.push_back(relative.filename().string());
                    if (access("/usr/bin/desktop-file-validate", X_OK) == 0) {
                        core::PosixProcessRunner runner;
                        core::ProcessSpec spec;
                        spec.executable = "/usr/bin/desktop-file-validate";
                        spec.args = {item.path().string()};
                        spec.env = {{"PATH", "/usr/bin"}, {"LANG", "C.UTF-8"}};
                        spec.timeout = std::chrono::seconds(5);
                        spec.max_stdout = spec.max_stderr = 4096;
                        auto checked = runner.run(spec);
                        if (!checked || checked->exit_code != 0 || checked->timed_out || checked->signaled ||
                            checked->out_truncated || checked->err_truncated) refused.push_back(name);
                    }
                }
            }
            if (name.starts_with("share/mime/packages/") && name.ends_with(".xml")) {
                auto data = core::read_bounded(item.path(), 16 * 1024 * 1024);
                if (!data) { refused.push_back(name); continue; }
                xmlParserCtxtPtr context = xmlNewParserCtxt();
                if (!context) return std::unexpected(fail("xml-init"));
                xmlCtxtUseOptions(context, XML_PARSE_NONET);
                xmlDocPtr document = xmlCtxtReadMemory(context, data->data(),
                    static_cast<int>(data->size()), nullptr, nullptr, XML_PARSE_NONET);
                if (!document || document->intSubset || document->extSubset) refused.push_back(name);
                if (document) xmlFreeDoc(document);
                xmlFreeParserCtxt(context);
            }
        } else refused.push_back(name);
        if (refused.size() > 20) break;
    }
    if (ec) return std::unexpected(fail("stage-read"));
    if (!refused.empty()) {
        std::string detail;
        for (const auto& path : refused) detail += path + "\n";
        return std::unexpected(fail("stage-refused", detail));
    }
    if (manifest.files.empty()) return std::unexpected(fail("nothing-installed"));
    std::ranges::sort(manifest.files, {}, &StagedFile::path);
    return manifest;
}

std::expected<std::string, ManualError> run_staging(
    core::ProcessRunner& runner, const std::filesystem::path& source,
    const std::filesystem::path& stage, const std::filesystem::path& home,
    EntryKind kind, Scope scope, std::string_view makeflags) {
    if ((kind != EntryKind::Script && kind != EntryKind::MakeInstall) ||
        (scope != Scope::User && scope != Scope::System) ||
        !source.is_absolute() || !stage.is_absolute() || !home.is_absolute() || home == "/" ||
        source.native().find('\0') != std::string::npos ||
        stage.native().find('\0') != std::string::npos ||
        home.native().find('\0') != std::string::npos ||
        !std::filesystem::is_directory(source) || std::filesystem::is_symlink(source) ||
        !std::filesystem::is_directory(stage) || std::filesystem::is_symlink(stage) ||
        !std::filesystem::is_empty(stage)) return std::unexpected(fail("staging-input"));
    if (!makeflags.empty()) {
        if (makeflags.size() < 3 || makeflags.size() > 5 || !makeflags.starts_with("-j") ||
            !std::all_of(makeflags.begin() + 2, makeflags.end(), [](unsigned char ch) { return std::isdigit(ch); }))
            return std::unexpected(fail("makeflags"));
    }
    std::filesystem::path prefix = scope == Scope::System ? "/usr/local" : home / ".local";
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/bwrap";
    spec.args = {"--unshare-all", "--die-with-parent", "--new-session",
                 "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin",
                 "--symlink", "usr/lib", "/lib", "--symlink", "usr/lib", "/lib64",
                 "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"};
    if (std::filesystem::exists("/etc/ld.so.cache"))
        spec.args.insert(spec.args.end(), {"--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache"});
    if (scope == Scope::User) {
        spec.args.insert(spec.args.end(), {"--tmpfs", home.string(),
                                            "--dir", prefix.string(),
                                            "--bind", stage.string(), prefix.string()});
    } else spec.args.insert(spec.args.end(), {"--bind", stage.string(), "/usr/local"});
    spec.args.insert(spec.args.end(), {"--ro-bind", source.string(), "/build/src",
                                        "--chdir", "/build/src", "--clearenv",
                                        "--setenv", "PATH", "/usr/bin",
                                        "--setenv", "HOME", scope == Scope::User ? home.string() : "/tmp",
                                        "--setenv", "LANG", "C.UTF-8",
                                        "--setenv", "PREFIX", prefix.string()});
    if (!makeflags.empty()) spec.args.insert(spec.args.end(), {"--setenv", "MAKEFLAGS", std::string(makeflags)});
    spec.args.push_back("--");
    if (kind == EntryKind::Script) spec.args.insert(spec.args.end(), {"/usr/bin/bash", "./install.sh"});
    else spec.args.insert(spec.args.end(), {"/usr/bin/make", "install", "PREFIX=" + prefix.string()});
    spec.timeout = std::chrono::minutes(30);
    spec.max_stdout = spec.max_stderr = 2 * 1024 * 1024;
    auto result = runner.run(spec);
    if (!result) return std::unexpected(fail("staging-process"));
    std::string log = core::strip_terminal_controls(core::redact(result->out + result->err));
    if (log.size() > 4 * 1024 * 1024) log.resize(4 * 1024 * 1024);
    if (result->exit_code != 0 || result->signaled || result->timed_out ||
        result->out_truncated || result->err_truncated) {
        if (log.size() > 4096) log.erase(0, log.size() - 4096);
        return std::unexpected(fail("staging-failed", log));
    }
    auto checked = validate_stage(stage, scope);
    if (!checked) return std::unexpected(checked.error());
    return log;
}

}
