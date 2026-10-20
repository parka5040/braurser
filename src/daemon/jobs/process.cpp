#include <braurser/daemon/jobs/process.hpp>
#include <braurser/daemon/core/text.hpp>
#include <glibmm/main.h>
#include <glib.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace braurser::daemon::jobs {
namespace {
constexpr std::size_t max_line = 4096;
constexpr std::size_t max_stream = 8 * 1024 * 1024;

bool valid_spec(const core::ProcessSpec& spec) {
    if (!spec.executable.is_absolute() || spec.executable.native().size() > 4096 ||
        spec.executable.native().find('\0') != std::string::npos ||
        spec.args.size() > 512 || spec.env.size() > 64 || spec.timeout.count() <= 0 ||
        spec.timeout > std::chrono::hours(4) || spec.stdin_data ||
        spec.max_stdout == 0 || spec.max_stdout > max_stream ||
        spec.max_stderr == 0 || spec.max_stderr > max_stream) return false;
    struct stat info{};
    if (stat(spec.executable.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        access(spec.executable.c_str(), X_OK) != 0) return false;
    for (const auto& value : spec.args)
        if (value.size() > 4096 || value.find('\0') != std::string::npos) return false;
    std::set<std::string> keys;
    for (const auto& [key, value] : spec.env)
        if (key.empty() || key.size() > 128 || !keys.insert(key).second ||
            !((key.front() >= 'A' && key.front() <= 'Z') ||
              (key.front() >= 'a' && key.front() <= 'z') || key.front() == '_') ||
            !std::ranges::all_of(key, [](unsigned char ch) {
                return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                       (ch >= '0' && ch <= '9') || ch == '_';
            }) ||
            value.size() > 4096 || value.find('\0') != std::string::npos) return false;
    return !spec.cwd || (spec.cwd->is_absolute() && spec.cwd->native().size() <= 4096 &&
                         spec.cwd->native().find('\0') == std::string::npos);
}
}

std::string safe_line(std::string_view text) {
    auto stripped = core::strip_terminal_controls(text.substr(0, max_line));
    auto folded = stripped;
    std::ranges::transform(folded, folded.begin(), [](unsigned char ch) { return std::tolower(ch); });
    for (const auto key : {"authorization:", "password:", "password=", "passwd:",
                           "passwd=", "secret:", "secret=", "token:", "token="})
        if (folded.find(key) != std::string::npos) return "[REDACTED]";
    auto redacted = core::redact(stripped);
    auto limited = core::bounded_line(redacted, max_line);
    gchar* valid = g_utf8_make_valid(limited.c_str(), static_cast<gssize>(limited.size()));
    std::string result(valid);
    g_free(valid);
    return result;
}

struct GioStreamingProcess::State {
    Line line;
    Done done;
    std::array<std::string, 2> pending;
    std::array<std::size_t, 2> received{};
    std::array<std::size_t, 2> limits{};
    std::array<bool, 2> closed{};
    std::array<int, 2> fds{-1, -1};
    std::array<sigc::connection, 2> watches;
    sigc::connection timeout;
    sigc::connection grace;
    sigc::connection drain;
    core::ProcessResult result;
    pid_t group{};
    bool started{};
    bool exited{};
    bool finished{};

    ~State() { close_pipes(); }
    void close_pipes() {
        for (auto& fd : fds) {
            if (fd >= 0) ::close(fd);
            fd = -1;
        }
    }
    void flush(std::size_t index) {
        if (!pending[index].empty()) {
            line(safe_line(pending[index]));
            pending[index].clear();
        }
    }
    void feed(std::size_t index, std::string_view bytes) {
        for (char ch : bytes) {
            if (ch == '\n' || ch == '\r') {
                flush(index);
            } else if (pending[index].size() < max_line) {
                pending[index].push_back(ch);
            }
        }
    }
    void truncate_output() {
        for (std::size_t index = 0; index < closed.size(); ++index) {
            watches[index].disconnect();
            if (!closed[index]) {
                if (index == 0) result.out_truncated = true;
                else result.err_truncated = true;
                flush(index);
                closed[index] = true;
            }
        }
        close_pipes();
        finish();
    }
    void finish() {
        if (finished || !exited || !closed[0] || !closed[1]) return;
        finished = true;
        group = 0;
        timeout.disconnect();
        grace.disconnect();
        drain.disconnect();
        for (auto& watch : watches) watch.disconnect();
        close_pipes();
        auto callback = std::move(done);
        if (callback) callback(result);
    }
    bool read_pipe(std::size_t index, int fd) {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = ::read(fd, buffer.data(), buffer.size());
            if (count > 0) {
                const auto length = static_cast<std::size_t>(count);
                if (length > limits[index] - received[index]) {
                    if (index == 0) result.out_truncated = true;
                    else result.err_truncated = true;
                    kill_group();
                    truncate_output();
                    return false;
                }
                received[index] += length;
                feed(index, std::string_view(buffer.data(), length));
            } else if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) {
                flush(index);
                closed[index] = true;
                finish();
                return false;
            } else if (count < 0 && errno == EAGAIN) return true;
        }
    }
    void signal(int number) {
        if (group > 1 && !finished) ::kill(-group, number);
    }
    void kill_group() { signal(SIGKILL); }
};

GioStreamingProcess::GioStreamingProcess() : state_(std::make_shared<State>()) {}
GioStreamingProcess::~GioStreamingProcess() {
    if (!state_) return;
    state_->done = {};
    state_->kill_group();
    for (auto& watch : state_->watches) watch.disconnect();
    state_->timeout.disconnect();
    state_->grace.disconnect();
    state_->drain.disconnect();
    state_->close_pipes();
    state_->finished = true;
}

bool GioStreamingProcess::start(const core::ProcessSpec& spec, Line line, Done done) {
    if (!valid_spec(spec) || !line || !done || state_->started) return false;
    std::vector<std::string> environment;
    environment.reserve(spec.env.size());
    for (const auto& [key, value] : spec.env) environment.push_back(key + "=" + value);
    std::vector<const gchar*> envp;
    envp.reserve(environment.size() + 1);
    for (const auto& item : environment) envp.push_back(item.c_str());
    envp.push_back(nullptr);
    const auto executable = spec.executable.string();
    std::vector<const gchar*> argv{executable.c_str()};
    for (const auto& argument : spec.args) argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    const auto directory = spec.cwd ? spec.cwd->string() : std::string{};
    GPid pid{};
    gint out = -1;
    gint err = -1;
    GError* error = nullptr;
    const auto flags = static_cast<GSpawnFlags>(G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CLOEXEC_PIPES);
    if (!g_spawn_async_with_pipes_and_fds(spec.cwd ? directory.c_str() : nullptr, argv.data(), envp.data(), flags,
            [](gpointer) { if (::setpgid(0, 0) != 0) _exit(127); }, nullptr,
            -1, -1, -1, nullptr, nullptr, 0, &pid, nullptr, &out, &err, &error)) {
        if (error) g_error_free(error);
        return false;
    }
    state_->started = true;
    state_->group = pid;
    state_->fds = {out, err};
    state_->limits = {spec.max_stdout, spec.max_stderr};
    state_->line = std::move(line);
    state_->done = std::move(done);
    Glib::signal_child_watch().connect([state = state_](GPid child, int status) {
        state->exited = true;
        if (WIFEXITED(status)) state->result.exit_code = WEXITSTATUS(status);
        else {
            state->result.signaled = true;
            if (WIFSIGNALED(status)) state->result.term_signal = WTERMSIG(status);
        }
        g_spawn_close_pid(child);
        state->finish();
        if (!state->finished) {
            state->drain = Glib::signal_timeout().connect([state] {
                state->truncate_output();
                return false;
            }, 10000);
        }
    }, pid);
    try {
        for (std::size_t index = 0; index < 2; ++index) {
            const int fd = state_->fds[index];
            const int current = fcntl(fd, F_GETFL);
            if (current < 0 || fcntl(fd, F_SETFL, current | O_NONBLOCK) != 0) {
                state_->done = {};
                state_->kill_group();
                return false;
            }
            state_->watches[index] = Glib::signal_io().connect(
                [state = state_, index, fd](Glib::IOCondition) {
                    return state->read_pipe(index, fd);
                }, fd, Glib::IOCondition::IO_IN | Glib::IOCondition::IO_HUP | Glib::IOCondition::IO_ERR);
        }
        state_->timeout = Glib::signal_timeout().connect([state = state_] {
            state->result.timed_out = true;
            state->signal(SIGTERM);
            state->grace = Glib::signal_timeout().connect([state] {
                state->kill_group();
                state->truncate_output();
                return false;
            }, 2000);
            return false;
        }, static_cast<unsigned>(spec.timeout.count()));
        return true;
    } catch (const Glib::Error&) {
        state_->done = {};
        state_->kill_group();
        return false;
    }
}

void GioStreamingProcess::interrupt() { state_->signal(SIGINT); }
void GioStreamingProcess::kill_group() { state_->kill_group(); }

}
