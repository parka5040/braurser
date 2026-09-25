#ifndef BRAURSER_DAEMON_HELPERS_AUR_HELPER_HPP
#define BRAURSER_DAEMON_HELPERS_AUR_HELPER_HPP

#include <braurser/daemon/core/process.hpp>
#include <braurser/domain/types.hpp>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::helpers {

enum class HelperErrorCode { InvalidInput, ProcessFailure, InvalidOutput, UnsupportedVersion, Unavailable };

struct HelperError {
    HelperErrorCode code;
    std::string message;
};

struct HelperCapabilities {
    bool install_community{};
    bool update_community{};
    bool check_community_updates{};
    bool print_build_file{};
    friend bool operator==(const HelperCapabilities&, const HelperCapabilities&) = default;
};

struct CommunityUpdate {
    domain::PackageName name;
    std::string from;
    std::string to;
    friend bool operator==(const CommunityUpdate&, const CommunityUpdate&) = default;
};

struct HelperEnvironment {
    std::filesystem::path home;
    std::string user;
    std::filesystem::path runtime_dir;
    std::string lang{"C.UTF-8"};
    std::optional<std::string> display;
    std::optional<std::string> wayland_display;
    std::optional<std::filesystem::path> xauthority;
    std::optional<std::string> makeflags;
};

struct CommunityRequest {
    domain::PackageName name;
    std::filesystem::path job_dir;
    std::filesystem::path shim;
    std::string elevation;
    std::optional<std::filesystem::path> askpass;
    std::filesystem::path home;
};

enum class PhaseEvent { Resolving, Downloading, Building, Installing, Finishing, Error, UnexpectedPrompt };

class AURHelper {
public:
    virtual ~AURHelper() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual std::expected<std::string, HelperError> version() const = 0;
    virtual std::expected<HelperCapabilities, HelperError> capabilities() const = 0;
    virtual std::expected<core::ProcessSpec, HelperError> build_install_spec(const CommunityRequest& request) const = 0;
    virtual core::ProcessSpec build_updates_spec() const = 0;
    virtual core::ProcessSpec build_print_build_file_spec(const domain::PackageName& name) const = 0;
    virtual std::expected<std::vector<CommunityUpdate>, HelperError> parse_updates(const core::ProcessResult& result) const = 0;
    virtual std::expected<std::string, HelperError> parse_build_file(const core::ProcessResult& result) const = 0;
    virtual std::optional<PhaseEvent> classify_line(std::string_view raw_line) const = 0;
};

class HelperAdapter : public AURHelper {
public:
    std::string_view name() const noexcept override;
    std::expected<std::string, HelperError> version() const override;
    std::expected<HelperCapabilities, HelperError> capabilities() const override;
    std::expected<core::ProcessSpec, HelperError> build_install_spec(const CommunityRequest& request) const override;
    core::ProcessSpec build_updates_spec() const override;
    core::ProcessSpec build_print_build_file_spec(const domain::PackageName& name) const override;
    std::expected<std::vector<CommunityUpdate>, HelperError> parse_updates(const core::ProcessResult& result) const override;
    std::expected<std::string, HelperError> parse_build_file(const core::ProcessResult& result) const override;
    std::optional<PhaseEvent> classify_line(std::string_view raw_line) const override;

protected:
    HelperAdapter(std::string_view helper_name, std::filesystem::path binary,
                  HelperEnvironment environment, core::ProcessRunner& runner);
    virtual std::vector<std::string> install_flags(const CommunityRequest& request) const = 0;

private:
    std::string helper_name_;
    std::filesystem::path binary_;
    HelperEnvironment environment_;
    core::ProcessRunner& runner_;
};

class ParuHelper final : public HelperAdapter {
public:
    ParuHelper(HelperEnvironment environment, core::ProcessRunner& runner,
               std::filesystem::path binary = "/usr/bin/paru");

private:
    std::vector<std::string> install_flags(const CommunityRequest& request) const override;
};

class YayHelper final : public HelperAdapter {
public:
    YayHelper(HelperEnvironment environment, core::ProcessRunner& runner,
              std::filesystem::path binary = "/usr/bin/yay");

private:
    std::vector<std::string> install_flags(const CommunityRequest& request) const override;
};

class HelperLocator {
public:
    explicit HelperLocator(std::filesystem::path binary_dir = "/usr/bin");
    std::expected<std::filesystem::path, HelperError> locate(std::string_view preference) const;
    std::expected<std::unique_ptr<AURHelper>, HelperError> select(
        std::string_view preference, HelperEnvironment environment, core::ProcessRunner& runner) const;

private:
    std::filesystem::path binary_dir_;
};

}

#endif
