#include <braurser/daemon/core/process.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace braurser::daemon::core {
namespace {

class Fd {
public:
    Fd() = default;
    explicit Fd(int value) : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    int get() const { return value_; }
    int release() { return std::exchange(value_, -1); }
    void reset(int value = -1) {
        if (value_ >= 0) {
            close(value_);
        }
        value_ = value;
    }

private:
    int value_{-1};
};

struct Pipe {
    Fd read;
    Fd write;
};

std::expected<Pipe, ProcessError> make_pipe() {
    int ends[2];
    if (pipe2(ends, O_CLOEXEC | O_NONBLOCK) != 0) {
        return std::unexpected(ProcessError{errno, "pipe2 failed"});
    }
    return Pipe{Fd{ends[0]}, Fd{ends[1]}};
}

bool make_blocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

bool contains_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

std::expected<void, ProcessError> validate(const ProcessSpec& spec) {
    if (!spec.executable.is_absolute() || spec.executable.native().empty() ||
        contains_nul(spec.executable.native())) {
        return std::unexpected(ProcessError{EINVAL, "executable must be an absolute path"});
    }
    struct stat info {};
    if (stat(spec.executable.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        access(spec.executable.c_str(), X_OK) != 0) {
        return std::unexpected(ProcessError{errno == 0 ? EACCES : errno, "executable unavailable"});
    }
    if (spec.timeout.count() <= 0 || spec.timeout > std::chrono::hours(24) ||
        spec.max_stdout > 64U * 1024 * 1024 || spec.max_stderr > 64U * 1024 * 1024 ||
        spec.args.size() > 4096 || spec.env.size() > 4096 ||
        (spec.stdin_data && spec.stdin_data->size() > 64U * 1024 * 1024)) {
        return std::unexpected(ProcessError{EINVAL, "process input exceeds limit"});
    }
    if (spec.cwd) {
        if (!spec.cwd->is_absolute() || contains_nul(spec.cwd->native())) {
            return std::unexpected(ProcessError{EINVAL, "cwd must be absolute"});
        }
        if (stat(spec.cwd->c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
            return std::unexpected(ProcessError{errno == 0 ? ENOTDIR : errno, "cwd unavailable"});
        }
    }
    for (const auto& arg : spec.args) {
        if (contains_nul(arg) || arg.size() > 131072) {
            return std::unexpected(ProcessError{EINVAL, "argument invalid or oversized"});
        }
    }
    for (const auto& [key, value] : spec.env) {
        if (key.empty() || key.size() > 256 || value.size() > 131072 ||
            key.find('=') != std::string::npos || contains_nul(key) || contains_nul(value)) {
            return std::unexpected(ProcessError{EINVAL, "invalid environment entry"});
        }
    }
    return {};
}

void consume(Fd& fd, std::string& output, std::size_t cap, bool& truncated) {
    std::array<char, 8192> buffer{};
    for (int reads = 0; reads < 8 && fd.get() >= 0; ++reads) {
        const ssize_t count = read(fd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            const std::size_t size = static_cast<std::size_t>(count);
            const std::size_t available = cap - output.size();
            const std::size_t stored = std::min(size, available);
            output.append(buffer.data(), stored);
            truncated |= stored != size;
        } else if (count == 0) {
            fd.reset();
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            fd.reset();
        }
    }
}

class SpawnActions {
public:
    SpawnActions() : status_(posix_spawn_file_actions_init(&actions_)) {}
    ~SpawnActions() {
        if (status_ == 0) {
            posix_spawn_file_actions_destroy(&actions_);
        }
    }
    SpawnActions(const SpawnActions&) = delete;
    SpawnActions& operator=(const SpawnActions&) = delete;
    int status() const { return status_; }
    posix_spawn_file_actions_t* get() { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    int status_{};
};

class SpawnAttributes {
public:
    SpawnAttributes() : status_(posix_spawnattr_init(&attr_)) {}
    ~SpawnAttributes() {
        if (status_ == 0) {
            posix_spawnattr_destroy(&attr_);
        }
    }
    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;
    int status() const { return status_; }
    posix_spawnattr_t* get() { return &attr_; }

private:
    posix_spawnattr_t attr_{};
    int status_{};
};

void signal_group(pid_t pid, int signal, bool reaped) {
    if (kill(-pid, signal) == -1 && errno == ESRCH && !reaped) {
        kill(pid, signal);
    }
}

ssize_t write_pipe(int fd, const char* data, std::size_t length) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    sigset_t original;
    if (pthread_sigmask(SIG_BLOCK, &blocked, &original) != 0) return -1;
    sigset_t pending;
    sigpending(&pending);
    const bool previously_pending = sigismember(&pending, SIGPIPE) == 1;
    const auto written = write(fd, data, length);
    const int saved_errno = errno;
    if (written < 0 && saved_errno == EPIPE && !previously_pending) {
        timespec immediate{};
        sigtimedwait(&blocked, nullptr, &immediate);
    }
    pthread_sigmask(SIG_SETMASK, &original, nullptr);
    errno = saved_errno;
    return written;
}

}

std::expected<ProcessResult, ProcessError> PosixProcessRunner::run(const ProcessSpec& spec) {
    if (spec.cancel && spec.cancel->load()) {
        return std::unexpected(ProcessError{ECANCELED, "cancelled"});
    }
    if (auto status = validate(spec); !status) {
        return std::unexpected(status.error());
    }
    auto input = make_pipe();
    if (!input) return std::unexpected(input.error());
    auto output = make_pipe();
    if (!output) return std::unexpected(output.error());
    auto errors = make_pipe();
    if (!errors) return std::unexpected(errors.error());
    if (!make_blocking(input->read.get()) || !make_blocking(output->write.get()) ||
        !make_blocking(errors->write.get())) {
        return std::unexpected(ProcessError{errno, "cannot configure child pipes"});
    }

    SpawnActions actions;
    SpawnAttributes attributes;
    if (actions.status() != 0 || attributes.status() != 0) {
        return std::unexpected(ProcessError{actions.status() != 0 ? actions.status() : attributes.status(),
                                            "spawn setup failed"});
    }
    int status = posix_spawn_file_actions_adddup2(actions.get(), input->read.get(), STDIN_FILENO);
    if (status == 0) status = posix_spawn_file_actions_adddup2(actions.get(), output->write.get(), STDOUT_FILENO);
    if (status == 0) status = posix_spawn_file_actions_adddup2(actions.get(), errors->write.get(), STDERR_FILENO);
    if (status == 0) status = posix_spawn_file_actions_addclosefrom_np(actions.get(), 3);
    if (status == 0 && spec.cwd) status = posix_spawn_file_actions_addchdir_np(actions.get(), spec.cwd->c_str());
    if (status == 0) status = posix_spawnattr_setflags(attributes.get(), POSIX_SPAWN_SETPGROUP);
    if (status == 0) status = posix_spawnattr_setpgroup(attributes.get(), 0);
    if (status != 0) {
        return std::unexpected(ProcessError{status, "spawn setup failed"});
    }

    std::vector<std::string> environment;
    std::vector<char*> envp;
    environment.reserve(spec.env.size());
    envp.reserve(spec.env.size() + 1);
    for (const auto& [key, value] : spec.env) {
        environment.push_back(key + "=" + value);
    }
    for (auto& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<char*> argv;
    argv.reserve(spec.args.size() + 2);
    std::string executable_name = spec.executable.filename().string();
    argv.push_back(executable_name.data());
    for (const auto& arg : spec.args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid{};
    status = posix_spawn(&pid, spec.executable.c_str(), actions.get(), attributes.get(),
                         argv.data(), envp.data());
    if (status != 0) {
        return std::unexpected(ProcessError{status, "posix_spawn failed"});
    }
    input->read.reset();
    output->write.reset();
    errors->write.reset();
    if (!spec.stdin_data || spec.stdin_data->empty()) input->write.reset();

    ProcessResult result;
    std::size_t input_offset = 0;
    bool reaped = false;
    int wait_status{};
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + spec.timeout;
    auto grace_deadline = deadline;
    bool sent_kill = false;
    bool killed_for_output = false;
    while (!reaped || output->read.get() >= 0 || errors->read.get() >= 0) {
        const auto now = std::chrono::steady_clock::now();
        if (!result.timed_out && (now >= deadline || (spec.cancel && spec.cancel->load())) &&
            (!reaped || output->read.get() >= 0 || errors->read.get() >= 0)) {
            result.timed_out = true;
            signal_group(pid, SIGTERM, reaped);
            grace_deadline = now + std::chrono::seconds(2);
            input->write.reset();
        }
        if (result.timed_out && !sent_kill && now >= grace_deadline) {
            signal_group(pid, SIGKILL, reaped);
            sent_kill = true;
            output->read.reset();
            errors->read.reset();
        }
        if (!reaped) {
            pid_t waited;
            do {
                waited = waitpid(pid, &wait_status, WNOHANG);
            } while (waited == -1 && errno == EINTR);
            if (waited == pid) reaped = true;
            if (waited == -1) {
                const int code = errno;
                if (code != ECHILD) {
                    signal_group(pid, SIGKILL, false);
                    while (waitpid(pid, &wait_status, 0) == -1 && errno == EINTR) {}
                }
                return std::unexpected(ProcessError{code, "waitpid failed"});
            }
        }
        std::array<pollfd, 3> descriptors{};
        nfds_t count = 0;
        if (output->read.get() >= 0) descriptors[count++] = {output->read.get(), POLLIN, 0};
        if (errors->read.get() >= 0) descriptors[count++] = {errors->read.get(), POLLIN, 0};
        if (input->write.get() >= 0 && spec.stdin_data && input_offset < spec.stdin_data->size()) {
            descriptors[count++] = {input->write.get(), POLLOUT, 0};
        }
        if (count != 0) {
            int ready;
            do {
                ready = poll(descriptors.data(), count, 20);
            } while (ready < 0 && errno == EINTR);
            if (ready < 0) {
                const int code = errno;
                if (!reaped) {
                    signal_group(pid, SIGKILL, false);
                    while (waitpid(pid, &wait_status, 0) == -1 && errno == EINTR) {}
                }
                return std::unexpected(ProcessError{code, "poll failed"});
            }
            for (nfds_t index = 0; index < count; ++index) {
                if (descriptors[index].fd == output->read.get() && descriptors[index].revents != 0) {
                    consume(output->read, result.out, spec.max_stdout, result.out_truncated);
                } else if (descriptors[index].fd == errors->read.get() && descriptors[index].revents != 0) {
                    consume(errors->read, result.err, spec.max_stderr, result.err_truncated);
                } else if (descriptors[index].fd == input->write.get() && descriptors[index].revents != 0) {
                    const auto remaining = spec.stdin_data->size() - input_offset;
                    const auto amount = std::min(remaining, static_cast<std::size_t>(8192));
                    const ssize_t written = write_pipe(input->write.get(), spec.stdin_data->data() + input_offset, amount);
                    if (written > 0) input_offset += static_cast<std::size_t>(written);
                    else if (written < 0 && errno != EINTR && errno != EAGAIN) input->write.reset();
                    if (input_offset == spec.stdin_data->size()) input->write.reset();
                }
            }
            if ((result.out_truncated || result.err_truncated) && !killed_for_output) {
                signal_group(pid, SIGKILL, reaped);
                killed_for_output = true;
            }
        } else if (!reaped) {
            poll(nullptr, 0, 20);
        }
    }
    if (WIFEXITED(wait_status)) result.exit_code = WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status)) {
        result.signaled = true;
        result.term_signal = WTERMSIG(wait_status);
    }
    return result;
}

}
