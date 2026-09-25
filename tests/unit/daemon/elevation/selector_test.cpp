#include <braurser/daemon/elevation/selector.hpp>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::elevation {
namespace {
class TempTree {
public:
    TempTree() {
        auto name = (std::filesystem::temp_directory_path() / "elevation-XXXXXX").string();
        std::vector<char> buffer(name.begin(), name.end());
        buffer.push_back(0);
        if (mkdtemp(buffer.data())) root = buffer.data();
    }
    ~TempTree() { if (!root.empty()) std::filesystem::remove_all(root); }
    std::filesystem::path root;
};
void agent(const std::filesystem::path& root, std::string name, uid_t uid, std::string session) {
    auto process = root / "proc" / "321";
    std::filesystem::create_directories(process);
    std::ofstream(process / "status") << "Name:\tagent\nUid:\t" << uid << '\t' << uid << '\t' << uid << '\t' << uid << '\n';
    std::ofstream(process / "comm") << name << '\n';
    std::ofstream out(process / "environ", std::ios::binary);
    out << "XDG_SESSION_ID=" << session << '\0';
    std::filesystem::create_symlink("/usr/bin/" + name, process / "exe");
}
Environment setup(const TempTree& tree) {
    Environment e;
    e.proc_root = tree.root / "proc";
    e.uid = getuid();
    e.session_id = "42";
    e.pkexec = tree.root / "pkexec";
    e.sudo = tree.root / "sudo";
    e.askpass = tree.root / "askpass";
    for (auto file : {e.pkexec, e.sudo, e.askpass}) {
        std::ofstream(file) << "stub";
        chmod(file.c_str(), 0700);
    }
    return e;
}
}
TEST(ElevationSelector, RecognizesSameUidAndSession) {
    TempTree t;
    auto e = setup(t);
    agent(t.root, "polkit-gnome-authentication-agent-1", e.uid, "42");
    ProcAgentProbe probe;
    EXPECT_EQ(ElevationSelector(probe).select(e).method, Method::Pkexec);
}
TEST(ElevationSelector, RejectsOtherSessionAndUid) {
    TempTree t;
    auto e = setup(t);
    agent(t.root, "gnome-shell", e.uid, "another");
    ProcAgentProbe probe;
    EXPECT_EQ(ElevationSelector(probe).select(e).method, Method::SudoAskpass);
    std::filesystem::remove_all(e.proc_root);
    agent(t.root, "gnome-shell", e.uid + 1, "42");
    EXPECT_EQ(ElevationSelector(probe).select(e).method, Method::SudoAskpass);
}
TEST(ElevationSelector, ExcludesPolicykitHelperAndUnknownNames) {
    TempTree t;
    auto e = setup(t);
    agent(t.root, "polkit-agent-helper-1", e.uid, "42");
    ProcAgentProbe probe;
    EXPECT_EQ(ElevationSelector(probe).select(e).method, Method::SudoAskpass);
}
TEST(ElevationSelector, IgnoresSpoofedProcessName) {
    TempTree t;
    auto e = setup(t);
    agent(t.root, "gnome-shell", e.uid, "42");
    std::filesystem::remove(e.proc_root / "321" / "exe");
    std::filesystem::create_symlink("/usr/bin/unrelated", e.proc_root / "321" / "exe");
    ProcAgentProbe probe;
    EXPECT_EQ(ElevationSelector(probe).select(e).method, Method::SudoAskpass);
}
TEST(ElevationSelector, OverrideWinsAndUnavailableIsExplicit) {
    TempTree t;
    auto e = setup(t);
    ProcAgentProbe probe;
    e.settings_override = "pkexec";
    EXPECT_EQ(ElevationSelector(probe).select(e).method, Method::Pkexec);
    e.settings_override = "other";
    EXPECT_EQ(ElevationSelector(probe).select(e).method, Method::Unavailable);
    e.settings_override.clear();
    std::filesystem::remove(e.sudo);
    EXPECT_EQ(ElevationSelector(probe).select(e).method, Method::Unavailable);
}
}
