#include <braurser/daemon/elevation/selector.hpp>

#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::elevation {
namespace {
constexpr std::array<std::string_view, 14> agents{
    "polkit-gnome-authentication-agent-1", "polkit-kde-authentication-agent-1",
    "lxqt-policykit-agent", "mate-polkit", "polkit-mate-authentication-agent-1",
    "xfce-polkit", "hyprpolkitagent", "soteria", "gnome-shell", "cinnamon",
    "budgie-polkit-dialog", "cosmic-osd", "lxpolkit", "ukui-polkit"
};

bool known(std::string_view name) {
    for (auto agent : agents) if (name == agent) return true;
    return false;
}

bool executable(const std::filesystem::path& path) {
    struct stat st{};
    return path.is_absolute() && !stat(path.c_str(), &st) && S_ISREG(st.st_mode) &&
           (st.st_mode & 0111) && access(path.c_str(), X_OK) == 0;
}

std::string bounded_file(const std::filesystem::path& path, size_t limit) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::string result;
    result.resize(limit + 1);
    stream.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<size_t>(stream.gcount()));
    if (result.size() > limit) return {};
    return result;
}

bool uid_matches(const std::string& status, uid_t uid) {
    size_t offset = status.find("Uid:\t");
    if (offset == std::string::npos || (offset && status[offset - 1] != '\n')) return false;
    offset += 5;
    auto end = status.data() + offset;
    uid_t found{};
    auto parsed = std::from_chars(end, status.data() + status.size(), found);
    return parsed.ec == std::errc{} && parsed.ptr != end && found == uid &&
           (parsed.ptr == status.data() + status.size() || *parsed.ptr == '\t');
}

bool same_session(const std::string& environment, const std::string& ours) {
    if (ours.empty()) return true;
    const std::string prefix = "XDG_SESSION_ID=";
    size_t start = 0;
    while (start < environment.size()) {
        size_t end = environment.find('\0', start);
        if (end == std::string::npos) end = environment.size();
        if (environment.compare(start, prefix.size(), prefix) == 0)
            return std::string_view(environment.data() + start + prefix.size(),
                                    end - start - prefix.size()) == ours;
        start = end + 1;
    }
    return true;
}
}

std::string askpass_ui(std::string_view preferred, const std::filesystem::path& libexec) {
    const std::string first = preferred == "qt" ? "qt" : "gtk";
    const std::string second = first == "qt" ? "gtk" : "qt";
    if (executable(libexec / ("braurser-askpass-" + first))) return first;
    if (executable(libexec / ("braurser-askpass-" + second))) return second;
    return first;
}

bool ProcAgentProbe::available(const Environment& environment) const {
    std::error_code error;
    std::filesystem::directory_iterator entries(environment.proc_root, error);
    if (error) return false;
    for (auto it = entries; it != std::filesystem::directory_iterator{}; it.increment(error)) {
        if (error) return false;
        auto pid = it->path().filename().string();
        if (pid.empty()) continue;
        bool digits = true;
        for (char c : pid) if (c < '0' || c > '9') digits = false;
        if (!digits) continue;
        const auto process = it->path();
        if (!uid_matches(bounded_file(process / "status", 16384), environment.uid)) continue;
        auto comm = bounded_file(process / "comm", 256);
        if (!comm.empty() && comm.back() == '\n') comm.pop_back();
        std::error_code link_error;
        auto linked = std::filesystem::read_symlink(process / "exe", link_error);
        if (link_error) continue;
        auto executable_name = linked.filename().string();
        if (!known(executable_name) ||
            (comm != executable_name && comm != executable_name.substr(0, 15))) continue;
        if (same_session(bounded_file(process / "environ", 1048576), environment.session_id)) return true;
    }
    return false;
}

Selection ElevationSelector::select(const Environment& environment) const {
    const char* inherited = std::getenv("BRAURSER_ELEVATION");
    std::string override = !environment.settings_override.empty() ? environment.settings_override :
                           !environment.elevation_override.empty() ? environment.elevation_override :
                           inherited ? inherited : "";
    if (!override.empty() && override != "pkexec" && override != "sudo-askpass")
        return {Method::Unavailable, "unknown elevation override"};
    bool pkexec = executable(environment.pkexec);
    bool sudo = executable(environment.sudo) && executable(environment.askpass);
    if (override == "pkexec" && pkexec) return {Method::Pkexec, "explicit pkexec override"};
    if (override == "sudo-askpass" && sudo) return {Method::SudoAskpass, "explicit sudo-askpass override"};
    if (pkexec && probe_.available(environment)) return {Method::Pkexec, "polkit agent available"};
    if (sudo) return {Method::SudoAskpass, "askpass available"};
    return {Method::Unavailable, "no polkit agent or sudo askpass available; doas alone is unsupported"};
}
}
