#ifndef BRAURSER_DAEMON_CORE_PROCESS_HPP
#define BRAURSER_DAEMON_CORE_PROCESS_HPP

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace braurser::daemon::core {

struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
    std::optional<std::filesystem::path> cwd;
    std::chrono::milliseconds timeout{30000};
    std::size_t max_stdout{1024 * 1024};
    std::size_t max_stderr{1024 * 1024};
    std::optional<std::string> stdin_data;
};

struct ProcessResult {
    int exit_code{-1};
    bool signaled{false};
    int term_signal{0};
    bool timed_out{false};
    std::string out;
    std::string err;
    bool out_truncated{false};
    bool err_truncated{false};
};

struct ProcessError {
    int code{};
    std::string message;
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    /// in: spec contains an absolute executable and a bounded timeout.
    /// out: returns the exit status and captured output, including on timeout.
    /// err: returns a process error if validation, setup or spawning fails.
    virtual std::expected<ProcessResult, ProcessError> run(const ProcessSpec& spec) = 0;
};

class PosixProcessRunner final : public ProcessRunner {
public:
    std::expected<ProcessResult, ProcessError> run(const ProcessSpec& spec) override;
};

}

#endif
