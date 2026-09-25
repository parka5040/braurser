#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/core/text.hpp>
#include <braurser/validate/validate.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace braurser::daemon::manual {
namespace {

ManualError fail(std::string code) { return {std::move(code), {}, {}, {}}; }

}

std::expected<std::pair<std::string, std::string>, ManualError> render_pkgbuild(
    const Receipt& receipt, std::chrono::sys_seconds date, std::string_view arch) {
    auto parsed = validate_git_url(receipt.url);
    if (!parsed || receipt.commit.size() != 40 ||
        !std::all_of(receipt.commit.begin(), receipt.commit.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        }) || arch.empty() || arch.size() > 64 ||
        !std::all_of(arch.begin(), arch.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_';
        })) return std::unexpected(fail("package-input"));
    auto name = parsed->owner_repo.substr(parsed->owner_repo.find_last_of('/') + 1);
    for (char& ch : name) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9') &&
            ch != '+' && ch != '_' && ch != '.' && ch != '-') ch = '-';
    }
    const auto pkgname = "braurser-git-" + name;
    if (!br_valid_pkgname(pkgname.data(), pkgname.size())) return std::unexpected(fail("package-name"));
    const auto seconds = std::chrono::system_clock::to_time_t(date);
    std::tm tm{};
    if (!gmtime_r(&seconds, &tm)) return std::unexpected(fail("package-date"));
    std::array<char, 32> timestamp{};
    if (std::strftime(timestamp.data(), timestamp.size(), "%Y%m%d%H%M%S", &tm) != 14)
        return std::unexpected(fail("package-date"));
    std::string script = "pkgname=" + pkgname + "\n" +
        "pkgver=" + timestamp.data() + ".g" + receipt.commit.substr(0, 12) + "\n" +
        "pkgrel=1\narch=('" + std::string(arch) + "')\n" +
        "license=('custom:unknown')\nurl='" + receipt.url + "'\n" +
        "source=()\nsha256sums=()\n" +
        "package() {\n"
        "    mkdir -p -- \"$pkgdir/usr/local\"\n"
        "    cp -a -- \"$BRAURSER_STAGE/.\" \"$pkgdir/usr/local/\"\n"
        "}\n";
    return std::pair{pkgname, script};
}

std::expected<std::filesystem::path, ManualError> build_system_package(
    core::ProcessRunner& runner, const std::filesystem::path& stage,
    const std::filesystem::path& home, const std::filesystem::path& job_dir,
    const Receipt& receipt, std::chrono::sys_seconds date) {
    const auto jobs = home / ".cache/braurser/build/manual";
    const auto id = job_dir.lexically_relative(jobs).string();
    if (!home.is_absolute() || !job_dir.is_absolute() || !stage.is_absolute() ||
        job_dir.native().find('\0') != std::string::npos ||
        job_dir.lexically_normal() != job_dir || id.empty() || id.size() > 128 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-';
        }) || std::filesystem::is_symlink(job_dir))
        return std::unexpected(fail("package-path"));
    auto validated = validate_stage(stage, Scope::System);
    if (!validated) return std::unexpected(validated.error());
    bool has_elf = false;
    for (const auto& file : validated->files) {
        const auto path = stage / file.path;
        const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return std::unexpected(fail("package-file"));
        struct stat info {};
        std::array<char, 4> magic{};
        const auto got = read(fd, magic.data(), magic.size());
        const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
        close(fd);
        if (!regular || got < 0) return std::unexpected(fail("package-file"));
        if (got == 4 && std::string_view(magic.data(), magic.size()) == "\x7f" "ELF") has_elf = true;
    }
    utsname machine{};
    if (uname(&machine) != 0) return std::unexpected(fail("package-arch"));
    auto render = render_pkgbuild(receipt, date, has_elf ? machine.machine : "any");
    if (!render) return std::unexpected(render.error());
    const auto root = job_dir / render->first;
    for (const auto& path : {job_dir, root, root / "packages", root / "sources", root / "build"}) {
        if (!core::ensure_private_dir(path)) return std::unexpected(fail("package-directory"));
    }
    if (!core::atomic_write(root / "PKGBUILD", render->second, 0600))
        return std::unexpected(fail("package-template"));
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/makepkg";
    spec.args = {"--nodeps", "--noconfirm", "--noprogressbar"};
    spec.env = {{"PATH", "/usr/bin"}, {"HOME", home.string()}, {"LANG", "C.UTF-8"},
                {"BRAURSER_STAGE", stage.string()}, {"PKGDEST", (root / "packages").string()},
                {"SRCDEST", (root / "sources").string()}, {"BUILDDIR", (root / "build").string()}};
    spec.cwd = root;
    spec.timeout = std::chrono::minutes(30);
    spec.max_stdout = spec.max_stderr = 2 * 1024 * 1024;
    auto output = runner.run(spec);
    if (!output) return std::unexpected(fail("package-build"));
    if (output->exit_code != 0 || output->signaled || output->timed_out ||
        output->out_truncated || output->err_truncated) {
        auto error = fail("package-build");
        auto log = core::redact(output->out + output->err);
        if (log.size() > 4096) log.erase(0, log.size() - 4096);
        error.detail = core::strip_terminal_controls(log);
        return std::unexpected(error);
    }
    std::optional<std::filesystem::path> result;
    for (const auto& item : std::filesystem::directory_iterator(root / "packages")) {
        const auto name = item.path().filename().string();
        if (!item.is_regular_file() || item.is_symlink() ||
            !br_is_pkg_archive_name(name.data(), name.size()) ||
            !name.starts_with(render->first + "-")) continue;
        if (result) return std::unexpected(fail("package-ambiguous"));
        result = item.path();
    }
    if (!result) return std::unexpected(fail("package-missing"));
    return *result;
}

std::expected<SystemRequest, ManualError> system_upgrade_request(const std::filesystem::path& archive) {
    const auto name = archive.filename().string();
    if (!archive.is_absolute() || archive.lexically_normal() != archive ||
        !br_is_pkg_archive_name(name.data(), name.size()) ||
        !name.starts_with("braurser-git-")) return std::unexpected(fail("package-archive"));
    return SystemRequest{{"--upgrade", "--", archive.string()}, archive};
}

std::expected<SystemRequest, ManualError> system_remove_request(std::string_view pkgname) {
    if (!br_valid_pkgname(pkgname.data(), pkgname.size()) ||
        !pkgname.starts_with("braurser-git-")) return std::unexpected(fail("package-name"));
    return SystemRequest{{"--remove", "--nosave", "--recursive", "--", std::string(pkgname)}, {}};
}

}
