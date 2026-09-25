#ifndef BRAURSER_PRESENTER_FIXTURES_HPP
#define BRAURSER_PRESENTER_FIXTURES_HPP

#include <braurser/presenter/models.hpp>

namespace braurser::presenter::fixtures {

struct Data {
    std::vector<protocol::PackageSummary> packages;
    protocol::GetResult official;
    protocol::GetResult community;
    protocol::CommunityInfo community_info;
    protocol::VariantsResult variants;
    protocol::ManualReceipt receipt;
    protocol::ManualPreview preview;
    protocol::LauncherEntry outside;
    protocol::WikiGetResult wiki;
    protocol::UpdateReport updates;
    protocol::JobsGetResult running;
    protocol::Question question;
};

const Data& data();

class FakeServer final : public Rpc {
public:
    enum class Mode { Populated, Empty, Error, Loading, Offline };
    explicit FakeServer(Dispatcher& dispatcher, Mode mode = Mode::Populated);
    ~FakeServer() override;
    void request(std::string_view method, nlohmann::json params, Callback callback, std::chrono::milliseconds timeout) override;
    Subscription subscribe(std::string_view method, std::function<void(const nlohmann::json&)> callback) override;
    void emit_event(std::string_view method, nlohmann::json value);
    void reply(std::string method, nlohmann::json value);
    static protocol::UpdateReport update_report();
    template<class EventType> void notify(const EventType& event) {
        emit_event(Event<EventType>::name, nlohmann::json(event));
    }
    template<class Models> void populate(Models& models) {
        models.shell.refresh();
        models.settings.refresh();
        models.search.submit();
        models.search.facets();
        models.first.refresh();
        models.installed.refresh();
        if (mode_ == Mode::Error || mode_ == Mode::Loading) { models.updates.check(); return; }
        auto report = update_report();
        if (mode_ == Mode::Empty) { report.official.clear(); report.community.clear(); report.manual.clear(); report.news.clear(); report.overall_restart = {"none"}; }
        models.updates.report(report);
        models.settings.capabilities({"paru"}, "paru", false);
        models.settings.apply({});
        const std::vector<std::string> shipped{"krita"};
        models.discover.collection(1, shipped, mode_ == Mode::Empty ? std::vector<std::string>{} : shipped, data().packages);
        models.discover.availability(true, true, mode_ == Mode::Offline);
    }
    std::vector<std::pair<std::string, nlohmann::json>> requests;
private:
    Reply<nlohmann::json> response(std::string_view method, const nlohmann::json& params);
    Dispatcher& dispatcher_;
    Mode mode_;
    std::uint64_t next_ = 1;
    std::map<std::string, nlohmann::json> overrides_;
    struct Listener {
        std::string method;
        std::function<void(const nlohmann::json&)> callback;
        bool active = true;
    };
    std::map<std::uint64_t, std::shared_ptr<Listener>> listeners_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::map<std::string, protocol::TransactionPlan> plans_;
    std::uint64_t revision_ = 0;
    bool dequeued_ = false;
    protocol::RestartStateResult restart_{{"none"}, {}, {}};
    protocol::Settings settings_{{"auto"}, {"auto"}, {"daily"}, true, false, true, false, {"system"}};
};

}

#endif
