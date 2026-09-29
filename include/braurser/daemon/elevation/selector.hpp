#ifndef BRAURSER_DAEMON_ELEVATION_SELECTOR_HPP
#define BRAURSER_DAEMON_ELEVATION_SELECTOR_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace braurser::daemon::elevation {

enum class Method { Pkexec, SudoAskpass, Unavailable };

struct Environment {
    std::string settings_override;
    std::string elevation_override;
    std::filesystem::path askpass;
    std::string session_id;
    uid_t uid{};
    std::filesystem::path proc_root{"/proc"};
    std::filesystem::path pkexec{"/usr/bin/pkexec"};
    std::filesystem::path sudo{"/usr/bin/sudo"};
};

struct Selection {
    Method method{Method::Unavailable};
    std::string reason;
};

class AgentProbe {
public:
    virtual ~AgentProbe() = default;
    /// in: environment identifies a proc tree, caller uid and session.
    /// out: true only when a recognized agent belongs to the caller.
    virtual bool available(const Environment& environment) const = 0;
};

class ProcAgentProbe final : public AgentProbe {
public:
    bool available(const Environment& environment) const override;
};

/// in: preferred is "gtk" or "qt"; libexec is the absolute private executable directory.
/// out: preferred when its askpass dialog is installed, else the other installed one, else preferred.
std::string askpass_ui(std::string_view preferred, const std::filesystem::path& libexec);

class ElevationSelector {
public:
    explicit ElevationSelector(const AgentProbe& probe) : probe_(probe) {}
    /// in: environment supplies override, executable paths and caller identity.
    /// out: method and reason describe the available authentication route.
    Selection select(const Environment& environment) const;
private:
    const AgentProbe& probe_;
};

}

#endif
