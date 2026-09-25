#ifndef BRAURSER_DAEMON_JOBS_PROCESS_HPP
#define BRAURSER_DAEMON_JOBS_PROCESS_HPP

#include <braurser/daemon/core/process.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace braurser::daemon::jobs {

class StreamingProcess {
public:
    using Line = std::function<void(std::string_view)>;
    using Done = std::function<void(const core::ProcessResult&)>;
    virtual ~StreamingProcess() = default;
    /// in: spec has an absolute executable, bounded argv and an explicit environment.
    /// out: streams sanitized, bounded stdout and stderr lines then calls done once.
    virtual bool start(const core::ProcessSpec& spec, Line line, Done done) = 0;
    /// out: sends SIGINT to the entire child process group.
    virtual void interrupt() = 0;
    /// out: sends SIGKILL to the entire child process group.
    virtual void kill_group() = 0;
};

class GioStreamingProcess final : public StreamingProcess {
public:
    GioStreamingProcess();
    ~GioStreamingProcess() override;
    GioStreamingProcess(const GioStreamingProcess&) = delete;
    GioStreamingProcess& operator=(const GioStreamingProcess&) = delete;
    bool start(const core::ProcessSpec& spec, Line line, Done done) override;
    void interrupt() override;
    void kill_group() override;
private:
    struct State;
    std::shared_ptr<State> state_;
};

/// in: text may contain terminal controls, carriage returns and untrusted output.
/// out: bounded printable line without terminal escapes or control characters.
std::string safe_line(std::string_view text);

}

#endif
