#include <braurser/daemon/jobs/process.hpp>
#include <braurser/daemon/core/text.hpp>
#include <giomm/subprocess.h>
#include <giomm/subprocesslauncher.h>
#include <giomm/unixinputstream.h>
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
    Glib::RefPtr<Gio::Subprocess> child;
    Line line;
    Done done;
    std::array<std::string, 2> pending;
    std::array<std::size_t, 2> received{};
    std::array<std::size_t, 2> limits{};
    std::array<bool, 2> closed{};
    std::array<sigc::connection, 2> watches;
    sigc::connection timeout;
    sigc::connection grace;
    core::ProcessResult result;
    pid_t group{};
    bool exited{};
    bool finished{};

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
    void finish() {
        if (finished || !exited || !closed[0] || !closed[1]) return;
        finished = true;
        group = 0;
        timeout.disconnect();
        grace.disconnect();
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
                    return true;
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
}

bool GioStreamingProcess::start(const core::ProcessSpec& spec, Line line, Done done) {
    if (!valid_spec(spec) || !line || !done || state_->child) return false;
    try {
        auto launcher = Gio::SubprocessLauncher::create(
            Gio::Subprocess::Flags::STDOUT_PIPE | Gio::Subprocess::Flags::STDERR_PIPE);
        std::vector<std::string> env;
        env.reserve(spec.env.size());
        for (const auto& [key, value] : spec.env) env.push_back(key + "=" + value);
        launcher->set_environ(env);
        launcher->set_child_setup([] { if (::setpgid(0, 0) != 0) _exit(127); });
        if (spec.cwd) launcher->set_cwd(spec.cwd->string());
        std::vector<std::string> argv{spec.executable.string()};
        argv.insert(argv.end(), spec.args.begin(), spec.args.end());
        auto child = launcher->spawn(argv);
        auto stdout_stream = std::dynamic_pointer_cast<Gio::UnixInputStream>(child->get_stdout_pipe());
        auto stderr_stream = std::dynamic_pointer_cast<Gio::UnixInputStream>(child->get_stderr_pipe());
        if (!stdout_stream || !stderr_stream) {
            child->force_exit();
            return false;
        }
        const auto text = std::string(child->get_identifier());
        pid_t group{};
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), group);
        if (error != std::errc{} || end != text.data() + text.size() || group <= 1) {
            child->force_exit();
            return false;
        }
        state_->child = std::move(child);
        state_->group = group;
        state_->limits = {spec.max_stdout, spec.max_stderr};
        state_->line = std::move(line);
        state_->done = std::move(done);
        const std::array streams{stdout_stream, stderr_stream};
        for (std::size_t index = 0; index < 2; ++index) {
            const int fd = streams[index]->get_fd();
            const int flags = fcntl(fd, F_GETFL);
            if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                state_->kill_group();
                return false;
            }
            state_->watches[index] = Glib::signal_io().connect(
                [state = state_, index, fd](Glib::IOCondition) {
                    return state->read_pipe(index, fd);
                }, fd, Glib::IOCondition::IO_IN | Glib::IOCondition::IO_HUP | Glib::IOCondition::IO_ERR);
        }
        state_->child->wait_async([state = state_](const Glib::RefPtr<Gio::AsyncResult>& result) {
            try { state->child->wait_finish(result); }
            catch (const Glib::Error&) {}
            state->exited = true;
            if (state->child->get_if_exited()) state->result.exit_code = state->child->get_exit_status();
            else if (state->child->get_if_signaled()) {
                state->result.signaled = true;
                state->result.term_signal = state->child->get_term_sig();
            } else state->result.signaled = true;
            state->finish();
        });
        state_->timeout = Glib::signal_timeout().connect([state = state_] {
            state->result.timed_out = true;
            state->signal(SIGTERM);
            state->grace = Glib::signal_timeout().connect([state] {
                state->kill_group();
                return false;
            }, 2000);
            return false;
        }, static_cast<unsigned>(spec.timeout.count()));
        return true;
    } catch (const Glib::Error&) {
        state_->kill_group();
        return false;
    }
}

void GioStreamingProcess::interrupt() { state_->signal(SIGINT); }
void GioStreamingProcess::kill_group() { state_->kill_group(); }

}
