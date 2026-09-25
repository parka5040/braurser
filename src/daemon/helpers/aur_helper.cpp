#include <braurser/daemon/helpers/aur_helper.hpp>

#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/core/text.hpp>
#include <braurser/validate/validate.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace braurser::daemon::helpers {
namespace {

bool clean_path(const std::filesystem::path& path) {
    if (!path.is_absolute() || path == "/" || path.native().size() > 4096 ||
        !std::ranges::all_of(path.native(), [](unsigned char ch) { return ch >= 0x20 && ch != 0x7f; })) return false;
    for (const auto& part : path.relative_path()) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return true;
}

bool clean_text(std::string_view text, std::size_t max) {
    if (text.empty() || text.size() > max) return false;
    return std::ranges::all_of(text, [](unsigned char ch) { return ch >= 0x20 && ch != 0x7f; });
}

bool valid_user(std::string_view user) {
    if (!clean_text(user, 64) || !(std::isalpha(static_cast<unsigned char>(user.front())) || user.front() == '_')) return false;
    return std::ranges::all_of(user, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
    });
}

bool valid_language(std::string_view value) {
    if (!clean_text(value, 64)) return false;
    return std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '.' || ch == '-' || ch == '@';
    });
}

bool valid_makeflags(std::string_view value) {
    return value.size() >= 3 && value.size() <= 5 && value.substr(0, 2) == "-j" &&
           std::ranges::all_of(value.substr(2), [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}

void validate_environment(const HelperEnvironment& env) {
    if (!clean_path(env.home) || !clean_path(env.runtime_dir) || !valid_user(env.user) ||
        !valid_language(env.lang) || (env.display && !clean_text(*env.display, 256)) ||
        (env.wayland_display && !clean_text(*env.wayland_display, 256)) ||
        (env.xauthority && !clean_path(*env.xauthority))) {
        throw std::invalid_argument("invalid helper environment");
    }
}

std::vector<std::pair<std::string, std::string>> base_environment(const HelperEnvironment& env) {
    std::vector<std::pair<std::string, std::string>> entries{
        {"PATH", "/usr/bin"}, {"HOME", env.home.string()}, {"USER", env.user},
        {"LOGNAME", env.user}, {"LANG", env.lang}, {"TERM", "dumb"},
        {"NO_COLOR", "1"}, {"XDG_RUNTIME_DIR", env.runtime_dir.string()}};
    if (env.display) entries.emplace_back("DISPLAY", *env.display);
    if (env.wayland_display) entries.emplace_back("WAYLAND_DISPLAY", *env.wayland_display);
    if (env.xauthority) entries.emplace_back("XAUTHORITY", env.xauthority->string());
    if (env.makeflags && valid_makeflags(*env.makeflags)) entries.emplace_back("MAKEFLAGS", *env.makeflags);
    return entries;
}

std::string_view trim(std::string_view value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

bool valid_version_token(std::string_view token) {
    if (token.empty() || token.size() > 128) return false;
    return std::ranges::all_of(token, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == ':' || ch == '-' || ch == '_' || ch == '+' || ch == '~';
    });
}

std::vector<std::string_view> fields(std::string_view text) {
    std::vector<std::string_view> result;
    while (!text.empty()) {
        const auto next = text.find_first_not_of(" \t");
        if (next == std::string_view::npos) break;
        text.remove_prefix(next);
        const auto end = text.find_first_of(" \t");
        result.push_back(text.substr(0, end));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end);
    }
    return result;
}

bool successful(const core::ProcessResult& result) {
    return !result.signaled && !result.timed_out && !result.out_truncated && !result.err_truncated && result.exit_code == 0;
}

std::expected<std::string, HelperError> parse_version(std::string_view name, const core::ProcessResult& result) {
    if (!successful(result) || result.out.size() > 256 || result.out.find('\0') != std::string::npos) {
        return std::unexpected(HelperError{HelperErrorCode::ProcessFailure, "helper version command failed"});
    }
    const auto words = fields(trim(result.out));
    if (words.size() != 5 || words[0] != name || words[2] != "-" || words[3] != "libalpm" ||
        words[1].size() < 2 || words[1][0] != 'v' || !valid_version_token(words[1].substr(1)) ||
        words[4].size() < 2 || words[4][0] != 'v' || !valid_version_token(words[4].substr(1))) {
        return std::unexpected(HelperError{HelperErrorCode::InvalidOutput, "unrecognized helper version"});
    }
    return std::string(words[1].substr(1));
}

bool supported_version(std::string_view name, std::string_view version) {
    const auto dot = version.find('.');
    if (dot == std::string_view::npos) return false;
    const auto minor_end = version.find('.', dot + 1);
    const auto major = version.substr(0, dot);
    const auto minor = version.substr(dot + 1, minor_end - dot - 1);
    if (major.empty() || minor.empty() || major.size() > 3 || minor.size() > 3 ||
        !std::ranges::all_of(major, [](unsigned char ch) { return std::isdigit(ch); }) ||
        !std::ranges::all_of(minor, [](unsigned char ch) { return std::isdigit(ch); })) return false;
    const int major_number = std::stoi(std::string(major));
    const int minor_number = std::stoi(std::string(minor));
    return (name == "paru" && major_number == 2 && minor_number >= 1) ||
           (name == "yay" && major_number == 13);
}

bool available(const std::filesystem::path& path) {
    struct stat info {};
    return clean_path(path) && stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           access(path.c_str(), X_OK) == 0;
}

}

HelperAdapter::HelperAdapter(std::string_view helper_name, std::filesystem::path binary,
                             HelperEnvironment environment, core::ProcessRunner& runner)
    : helper_name_(helper_name), binary_(std::move(binary)), environment_(std::move(environment)), runner_(runner) {
    validate_environment(environment_);
    if (!clean_path(binary_)) throw std::invalid_argument("invalid helper executable");
}

std::string_view HelperAdapter::name() const noexcept { return helper_name_; }

std::expected<std::string, HelperError> HelperAdapter::version() const {
    auto result = runner_.run(core::ProcessSpec{.executable = binary_, .args = {"--version"},
                                                 .env = base_environment(environment_), .cwd = std::nullopt,
                                                 .timeout = std::chrono::seconds(10),
                                                 .max_stdout = 256, .max_stderr = 1024, .stdin_data = std::nullopt});
    if (!result) return std::unexpected(HelperError{HelperErrorCode::ProcessFailure, "cannot run helper version"});
    return parse_version(helper_name_, *result);
}

std::expected<HelperCapabilities, HelperError> HelperAdapter::capabilities() const {
    auto detected = version();
    if (!detected) return std::unexpected(detected.error());
    if (!supported_version(helper_name_, *detected)) {
        return std::unexpected(HelperError{HelperErrorCode::UnsupportedVersion, "unsupported helper version"});
    }
    return HelperCapabilities{true, true, true, true};
}

std::expected<core::ProcessSpec, HelperError> HelperAdapter::build_install_spec(const CommunityRequest& request) const {
    const auto root = environment_.home / ".cache/braurser/build" / helper_name_;
    const auto job_id = request.job_dir.filename().string();
    if (request.home != environment_.home || !clean_path(request.job_dir) ||
        !clean_path(request.shim) || request.job_dir.parent_path() != root ||
        !br_valid_path_component(job_id.c_str(), job_id.size()) ||
        !std::ranges::all_of(job_id, [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-';
        }) ||
        (request.elevation != "pkexec" && request.elevation != "sudo-askpass") ||
        (request.askpass && !clean_path(*request.askpass)) ||
        (request.elevation == "sudo-askpass" && !request.askpass) ||
        (request.elevation == "pkexec" && request.askpass)) {
        return std::unexpected(HelperError{HelperErrorCode::InvalidInput, "invalid community request"});
    }
    if (auto created = core::ensure_private_dir(request.job_dir); !created) {
        return std::unexpected(HelperError{HelperErrorCode::InvalidInput, "cannot create private job directory"});
    }
    auto args = install_flags(request);
    args.insert(args.end(), {"--noconfirm", "-S", "--needed", "aur/" + std::string(request.name.value())});
    auto env = base_environment(environment_);
    if (request.askpass) env.emplace_back("SUDO_ASKPASS", request.askpass->string());
    env.emplace_back("BRAURSER_ELEVATION", request.elevation);
    env.emplace_back("BRAURSER_JOB_POLICY", "aur-build");
    env.emplace_back("BRAURSER_JOB_DIR", request.job_dir.string());
    return core::ProcessSpec{.executable = binary_, .args = std::move(args), .env = std::move(env),
                             .cwd = std::nullopt, .timeout = std::chrono::hours(4),
                             .max_stdout = 8U * 1024 * 1024, .max_stderr = 8U * 1024 * 1024,
                             .stdin_data = std::nullopt};
}

core::ProcessSpec HelperAdapter::build_updates_spec() const {
    return core::ProcessSpec{.executable = binary_, .args = {"-Qua"}, .env = base_environment(environment_),
                             .cwd = std::nullopt, .timeout = std::chrono::seconds(60),
                             .max_stdout = 1024 * 1024, .max_stderr = 1024 * 1024,
                             .stdin_data = std::nullopt};
}

core::ProcessSpec HelperAdapter::build_print_build_file_spec(const domain::PackageName& package) const {
    return core::ProcessSpec{.executable = binary_, .args = {"-Gp", std::string(package.value())},
                             .env = base_environment(environment_), .cwd = std::nullopt,
                             .timeout = std::chrono::seconds(30),
                             .max_stdout = 256 * 1024, .max_stderr = 64 * 1024,
                             .stdin_data = std::nullopt};
}

std::expected<std::vector<CommunityUpdate>, HelperError> HelperAdapter::parse_updates(const core::ProcessResult& result) const {
    if (result.signaled || result.timed_out || result.out_truncated || result.err_truncated ||
        result.out.size() > 1024 * 1024 || (result.exit_code != 0 &&
        !(result.exit_code == 1 && result.out.empty() && result.err.empty()))) {
        return std::unexpected(HelperError{HelperErrorCode::ProcessFailure, "helper update query failed"});
    }
    std::vector<CommunityUpdate> updates;
    std::unordered_set<std::string> seen;
    for (std::string_view rest = result.out; !rest.empty();) {
        const auto end = rest.find('\n');
        const auto line = trim(rest.substr(0, end));
        if (line.size() > 1024 || line.find('\0') != std::string_view::npos) {
            return std::unexpected(HelperError{HelperErrorCode::InvalidOutput, "invalid helper update line"});
        }
        const auto parts = fields(line);
        const bool aged = helper_name_ == "yay" && parts.size() == 5 &&
                          parts[4].size() >= 3 && parts[4].front() == '[' && parts[4].back() == ']';
        if ((parts.size() != 4 && !aged) || parts[2] != "->" ||
            !valid_version_token(parts[1]) || !valid_version_token(parts[3]) ||
            (aged && !std::ranges::all_of(parts[4].substr(1, parts[4].size() - 2), [](unsigned char ch) {
                return std::isalnum(ch);
            }))) {
            return std::unexpected(HelperError{HelperErrorCode::InvalidOutput, "invalid helper update line"});
        }
        auto package = domain::PackageName::make(parts[0]);
        if (!package || !seen.emplace(std::string(parts[0])).second) {
            return std::unexpected(HelperError{HelperErrorCode::InvalidOutput, "invalid helper package name"});
        }
        updates.push_back({std::move(*package), std::string(parts[1]), std::string(parts[3])});
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end + 1);
    }
    return updates;
}

std::expected<std::string, HelperError> HelperAdapter::parse_build_file(const core::ProcessResult& result) const {
    if (!successful(result)) {
        return std::unexpected(HelperError{HelperErrorCode::ProcessFailure, "helper build file query failed"});
    }
    if (result.out.empty() || result.out.size() > 256 * 1024 || result.out.find('\0') != std::string::npos) {
        return std::unexpected(HelperError{HelperErrorCode::InvalidOutput, "invalid helper build file"});
    }
    return result.out;
}

std::optional<PhaseEvent> HelperAdapter::classify_line(std::string_view raw_line) const {
    const std::string sanitized = core::strip_terminal_controls(raw_line);
    const auto line = trim(sanitized);
    constexpr std::string_view confirmation = ":: Proceed with installation? [Y/n]";
    if (line == confirmation || line == std::string(confirmation) + ":") return std::nullopt;
    if (line.find("[Y/n]") != std::string_view::npos || line.find("[y/N]") != std::string_view::npos ||
        line.find("Enter a number") != std::string_view::npos ||
        line.find("==> Packages to exclude") != std::string_view::npos) return PhaseEvent::UnexpectedPrompt;
    if (line.starts_with("error:") || line.starts_with("==> ERROR:")) return PhaseEvent::Error;
    if (line.starts_with(":: Running post-transaction hooks")) return PhaseEvent::Finishing;
    if (line.starts_with(":: Processing package changes") || line.starts_with("installing ") ||
        line.starts_with("upgrading ") || line.starts_with("==> Installing")) return PhaseEvent::Installing;
    if (line.starts_with("==> Making package") || line.starts_with("==> Starting build()") ||
        line.starts_with("==> Starting package()")) return PhaseEvent::Building;
    if (line.starts_with(":: Retrieving packages") || line.starts_with(":: Downloading PKGBUILDs") ||
        line.starts_with("==> Retrieving sources") || line.find(" downloading...") != std::string_view::npos)
        return PhaseEvent::Downloading;
    if (line.starts_with(":: Resolving dependencies") || line.starts_with("AUR Explicit") ||
        line.starts_with("Sync Dependency")) return PhaseEvent::Resolving;
    return std::nullopt;
}

ParuHelper::ParuHelper(HelperEnvironment environment, core::ProcessRunner& runner, std::filesystem::path binary)
    : HelperAdapter("paru", std::move(binary), std::move(environment), runner) {}

std::vector<std::string> ParuHelper::install_flags(const CommunityRequest& request) const {
    return {"--sudo", request.shim.string(), "--nosudoloop", "--skipreview", "--noprovides", "--clonedir", request.job_dir.string()};
}

YayHelper::YayHelper(HelperEnvironment environment, core::ProcessRunner& runner, std::filesystem::path binary)
    : HelperAdapter("yay", std::move(binary), std::move(environment), runner) {}

std::vector<std::string> YayHelper::install_flags(const CommunityRequest& request) const {
    return {"--sudo", request.shim.string(), "--answerclean", "None", "--answerdiff", "None",
            "--answeredit", "None", "--builddir", request.job_dir.string()};
}

HelperLocator::HelperLocator(std::filesystem::path binary_dir) : binary_dir_(std::move(binary_dir)) {
    if (!clean_path(binary_dir_)) throw std::invalid_argument("invalid helper binary directory");
}

std::expected<std::filesystem::path, HelperError> HelperLocator::locate(std::string_view preference) const {
    if (preference != "auto" && preference != "paru" && preference != "yay") {
        return std::unexpected(HelperError{HelperErrorCode::InvalidInput, "invalid helper preference"});
    }
    if (preference == "auto" || preference == "paru") {
        auto paru = binary_dir_ / "paru";
        if (available(paru)) return paru;
    }
    if (preference == "auto" || preference == "yay") {
        auto yay = binary_dir_ / "yay";
        if (available(yay)) return yay;
    }
    return std::unexpected(HelperError{HelperErrorCode::Unavailable, "requested helper unavailable"});
}

std::expected<std::unique_ptr<AURHelper>, HelperError> HelperLocator::select(
    std::string_view preference, HelperEnvironment environment, core::ProcessRunner& runner) const {
    auto binary = locate(preference);
    if (!binary) return std::unexpected(binary.error());
    if (binary->filename() == "paru") return std::make_unique<ParuHelper>(std::move(environment), runner, *binary);
    return std::make_unique<YayHelper>(std::move(environment), runner, *binary);
}

}
