// Discord Rich Presence: see discord_rpc.h. Shown: the game's name, where Link is (the same names the online player list
// uses), single player or online with a player count, and the time played. Never the room, a player, a seed or a path.
#include "discord_rpc.h"
#include "discord_frame.h"

#include "anchor.h"
#include "game_state.h"
#include "rando_flow.h"
#include "ship/cvar.h"
#include "ship_menu.h"
#include <imgui.h>

#define PICOJSON_USE_INT64
#include "picojson.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef BOD_DISCORD_APP_ID
#define BOD_DISCORD_APP_ID ""
#endif

namespace discord {
namespace {

constexpr const char* kGameName = "Boat of Daphnes (Wind Waker PC)";
constexpr const char* kImageKey = "icon";
constexpr int kRetryMs = 15000;    // Discord not running or the pipe closed: try again this often
constexpr int kMinSendMs = 4000;   // Discord allows about 5 updates per 20 s

using Clock = std::chrono::steady_clock;

// ---- the connection: a named pipe (Windows) or a Unix socket (Linux) ----
class Conn {
public:
    ~Conn() { Close(); }
    bool Open(const std::string& path) {
#ifdef _WIN32
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        return h_ != INVALID_HANDLE_VALUE;
#else
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (path.size() >= sizeof a.sun_path) {
            Close();
            return false;
        }
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            Close();
            return false;
        }
        return true;
#endif
    }
    void Close() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE)
            CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
#endif
    }
    bool Write(const std::string& data) {
#ifdef _WIN32
        DWORD n = 0;
        return WriteFile(h_, data.data(), static_cast<DWORD>(data.size()), &n, nullptr) && n == data.size();
#else
        size_t off = 0;
        while (off < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
                return false;
            off += static_cast<size_t>(n);
        }
        return true;
#endif
    }
    // Up to `n` bytes if any arrive within `ms`: the count, 0 for none yet, -1 when the connection is gone.
    int Read(void* buf, size_t n, int ms) {
#ifdef _WIN32
        const auto until = Clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr))
                return -1;
            if (avail > 0) {
                DWORD got = 0;
                if (!ReadFile(h_, buf, static_cast<DWORD>(n), &got, nullptr))
                    return -1;
                return static_cast<int>(got);
            }
            if (Clock::now() >= until)
                return 0;
            Sleep(10);
        }
#else
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, ms);
        if (r < 0)
            return errno == EINTR ? 0 : -1;
        if (r == 0)
            return 0;
        const ssize_t got = ::recv(fd_, buf, n, 0);
        return got > 0 ? static_cast<int>(got) : -1;
#endif
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// The places Discord may listen: BLUEWAKE_DISCORD_PIPE (testing: the only one tried), else discord-ipc-0..9 where the
// client puts them.
std::vector<std::string> Candidates() {
    std::vector<std::string> out;
    if (const char* test = std::getenv("BLUEWAKE_DISCORD_PIPE"); test != nullptr && *test != 0) {
        out.push_back(test);
        return out;
    }
    for (int i = 0; i < 10; ++i) {
#ifdef _WIN32
        out.push_back("\\\\.\\pipe\\discord-ipc-" + std::to_string(i));
#else
        std::vector<std::string> dirs;
        for (const char* v : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
            if (const char* d = std::getenv(v); d != nullptr && *d != 0)
                dirs.push_back(d);
        dirs.push_back("/tmp");
        for (std::string d : dirs) {
            while (d.size() > 1 && d.back() == '/')
                d.pop_back();
            for (const char* sub : {"", "/app/com.discordapp.Discord", "/snap.discord"})
                out.push_back(d + sub + "/discord-ipc-" + std::to_string(i));
        }
#endif
    }
    return out;
}

// ---- the activity ----
struct Activity {
    bool active = false; // false: no activity (cleared)
    bool hold = false;   // Compute only: between scenes, keep what is shown
    std::string details, state;
    long long start = 0;
    bool operator==(const Activity& o) const {
        return active == o.active && details == o.details && state == o.state && start == o.start;
    }
    bool operator!=(const Activity& o) const { return !(*this == o); }
};

std::string g_app_id;
bool g_checked_id;
long long g_start_time;
std::mutex g_mutex;
std::condition_variable g_cv;
Activity g_desired;
bool g_stop;
std::thread g_thread;
std::atomic<bool> g_done{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_reconnect{false};

unsigned long long pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long long>(getpid());
#endif
}

std::string ActivityCommand(const Activity& a) {
    static int nonce;
    picojson::object args;
    args["pid"] = picojson::value(static_cast<int64_t>(pid()));
    if (a.active) {
        picojson::object act, ts, assets;
        if (!a.details.empty())
            act["details"] = picojson::value(a.details);
        if (!a.state.empty())
            act["state"] = picojson::value(a.state);
        if (a.start != 0) {
            ts["start"] = picojson::value(static_cast<int64_t>(a.start));
            act["timestamps"] = picojson::value(ts);
        }
        assets["large_image"] = picojson::value(std::string(kImageKey));
        assets["large_text"] = picojson::value(std::string(kGameName));
        act["assets"] = picojson::value(assets);
        args["activity"] = picojson::value(act);
    }
    picojson::object cmd;
    cmd["cmd"] = picojson::value(std::string("SET_ACTIVITY"));
    cmd["args"] = picojson::value(args);
    cmd["nonce"] = picojson::value(std::to_string(++nonce));
    return picojson::value(cmd).serialize();
}

// One frame from Discord: false when the connection is gone. A ping is answered; text is returned in `json`.
bool ReadFrame(Conn& c, int ms, uint32_t* op, std::string* json, bool* got) {
    *got = false;
    unsigned char h[8];
    size_t have = 0;
    auto until = Clock::now() + std::chrono::milliseconds(ms);
    std::string body;
    uint32_t len = 0;
    bool header = false;
    for (;;) {
        const size_t want = header ? len - body.size() : sizeof h - have;
        if (header && want == 0)
            break;
        unsigned char tmp[4096];
        const int n = c.Read(header ? tmp : h + have, header ? std::min<size_t>(want, sizeof tmp) : want, 100);
        if (n < 0)
            return false;
        if (n > 0) {
            if (header)
                body.append(reinterpret_cast<char*>(tmp), static_cast<size_t>(n));
            else
                have += static_cast<size_t>(n);
            if (!header && have == sizeof h) {
                if (!DecodeHeader(h, op, &len))
                    return false;
                header = true;
                if (len == 0)
                    break;
            }
            until = Clock::now() + std::chrono::milliseconds(2000); // a frame has begun: let the rest arrive
        } else if (Clock::now() >= until) {
            return !header && have == 0; // nothing yet (fine) or a torn frame (gone)
        }
    }
    *json = body;
    *got = true;
    if (*op == kPing)
        return c.Write(EncodeFrame(kPong, body));
    return *op != kClose;
}

bool Handshake(Conn& c) {
    picojson::object hs;
    hs["v"] = picojson::value(static_cast<int64_t>(1));
    hs["client_id"] = picojson::value(g_app_id);
    if (!c.Write(EncodeFrame(kHandshake, picojson::value(hs).serialize())))
        return false;
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < until) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_stop)
                return false;
        }
        uint32_t op;
        std::string json;
        bool got;
        if (!ReadFrame(c, 200, &op, &json, &got))
            return false;
        if (!got)
            continue;
        picojson::value v;
        if (!picojson::parse(v, json).empty() || !v.is<picojson::object>())
            continue;
        const auto& o = v.get<picojson::object>();
        const auto evt = o.find("evt");
        if (evt != o.end() && evt->second.is<std::string>()) {
            if (evt->second.get<std::string>() == "READY")
                return true;
            if (evt->second.get<std::string>() == "ERROR")
                return false;
        }
    }
    return false;
}

void Worker() {
    Conn conn;
    bool connected = false;
    Activity sent;
    bool dirty = false; // a failed or interrupted exchange: send again after connecting
    auto next_try = Clock::now();
    auto last_send = Clock::now() - std::chrono::seconds(60);
    bool logged_wait = false;
    for (;;) {
        Activity want;
        bool stop;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait_for(lock, std::chrono::milliseconds(connected ? 250 : 500));
            want = g_desired;
            stop = g_stop;
        }
        if (g_reconnect.exchange(false)) { // the menu's Reconnect
            if (connected) {
                conn.Close();
                connected = false;
                g_connected = false;
            }
            next_try = Clock::now();
        }
        if (stop) {
            if (connected && (sent.active || dirty)) {
                Activity none;
                conn.Write(EncodeFrame(kFrame, ActivityCommand(none)));
                std::fprintf(stderr, "[discord] activity cleared\n");
            }
            return;
        }
        if (!connected) {
            if (!want.active || Clock::now() < next_try)
                continue;
            bool ok = false;
            for (const std::string& path : Candidates()) {
                if (conn.Open(path)) {
                    if (Handshake(conn)) {
                        ok = true;
                        break;
                    }
                    std::fprintf(stderr, "[discord] %s answered, but not with the handshake's READY\n", path.c_str());
                    conn.Close();
                }
            }
            if (!ok) {
                next_try = Clock::now() + std::chrono::milliseconds(kRetryMs);
                if (!logged_wait)
                    std::fprintf(stderr, "[discord] Discord is not running (trying again every %d s)%s\n", kRetryMs / 1000,
                                 std::getenv("BLUEWAKE_DISCORD_PIPE") != nullptr ? " [test pipe set]" : "");
                logged_wait = true;
                continue;
            }
            logged_wait = false;
            connected = true;
            sent = Activity();
            dirty = true;
            g_connected = true;
            std::fprintf(stderr, "[discord] connected\n");
        }
        // Whatever Discord says (replies to our commands, pings); a closed connection starts the retry loop.
        for (int i = 0; i < 8 && connected; ++i) {
            uint32_t op;
            std::string json;
            bool got;
            if (!ReadFrame(conn, 0, &op, &json, &got)) {
                conn.Close();
                connected = false;
                g_connected = false;
                next_try = Clock::now() + std::chrono::milliseconds(kRetryMs);
                std::fprintf(stderr, "[discord] connection closed (trying again in %d s)\n", kRetryMs / 1000);
            } else if (!got) {
                break;
            }
        }
        if (!connected)
            continue;
        if ((want != sent || dirty) && Clock::now() - last_send >= std::chrono::milliseconds(kMinSendMs)) {
            if (!conn.Write(EncodeFrame(kFrame, ActivityCommand(want)))) {
                conn.Close();
                connected = false;
                g_connected = false;
                next_try = Clock::now() + std::chrono::milliseconds(kRetryMs);
                continue;
            }
            last_send = Clock::now();
            sent = want;
            dirty = false;
            std::fprintf(stderr, "[discord] activity %s\n", want.active ? "set" : "cleared");
        }
    }
}

bool Opt(const char* name) { return CVarGetInteger((std::string("gNetwork.Discord.") + name).c_str(), 1) != 0; }

Activity Compute() {
    Activity a;
    a.active = true;
    a.start = Opt("ShowTime") ? g_start_time : 0;
    const BluewakeLocation where = bluewake_game_location();
    const int players = online::PlayersOnline();
    if (Opt("ShowOnline"))
        a.state = players > 0 ? "Online with " + std::to_string(players) + (players == 1 ? " player" : " players") : "Single player";
    if (Opt("ShowMode") && randoflow::Active())
        a.state += a.state.empty() ? "Randomizer" : " - Randomizer";
    const bool opening = where.scene_proc == BLUEWAKE_SCENE_OPEN; // a new file's opening, before the play scene
    const bool title = where.scene_proc == BLUEWAKE_SCENE_TITLE || std::strcmp(where.stage, "sea_T") == 0;
    const bool file_select = where.scene_proc == BLUEWAKE_SCENE_NAME || std::strcmp(where.stage, "Name") == 0;
    if (title || file_select || opening) {
        if (!Opt("ShowTitle"))
            a.active = false;
        else
            a.details = opening ? "Starting a game" : title ? "On the title screen" : "Choosing a file";
    } else if (where.scene_proc == BLUEWAKE_SCENE_PLAY && where.stage[0] != 0 && where.has_player) {
        if (Opt("ShowLocation"))
            a.details = online::PlaceName(where.stage, where.room);
    } else {
        a.hold = true; // booting or between scenes: keep what is shown
    }
    return a;
}

} // namespace

void Frame() {
    if (!g_checked_id) {
        g_checked_id = true;
        g_start_time = static_cast<long long>(std::time(nullptr));
        const char* env = std::getenv("BLUEWAKE_DISCORD_APP_ID");
        g_app_id = env != nullptr && *env != 0 ? env : BOD_DISCORD_APP_ID;
        if (g_app_id.empty())
            std::fprintf(stderr, "[discord] no application id in this build\n");
    }
    if (g_app_id.empty())
        return;
    // Testing: BLUEWAKE_TEST_DISCORD_OFF_AT=<seconds> turns the setting off that long after the game started, and
    // BLUEWAKE_TEST_DISCORD_ON_AT=<seconds> back on (the menu's switch, which a scenario cannot click).
    for (const char* name : {"BLUEWAKE_TEST_DISCORD_OFF_AT", "BLUEWAKE_TEST_DISCORD_ON_AT"}) {
        static bool done[2];
        const bool off = name[23] == 'F';
        const char* at = std::getenv(name);
        if (at != nullptr && !done[off] && static_cast<long long>(std::time(nullptr)) - g_start_time >= std::atoi(at)) {
            done[off] = true;
            CVarSetInteger("gNetwork.Discord.Enabled", off ? 0 : 1);
            std::fprintf(stderr, "[test] Discord Rich Presence setting %s\n", off ? "off" : "on");
        }
    }
    static Activity last;
    static bool first = true;
    Activity a;
    if (Opt("Enabled"))
        a = Compute();
    if (a.hold && !first)
        return;
    if (a.hold)
        a = Activity();
    if (a == last && !first)
        return;
    first = false;
    last = a;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_desired = a;
    }
    if (!g_thread.joinable() && a.active) {
        std::fprintf(stderr, "[discord] starting (application id set)\n");
        g_thread = std::thread(Worker);
    }
    g_cv.notify_all();
}

void Shutdown() {
    if (!g_thread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_cv.notify_all();
    // The worker clears the activity and returns; Discord not answering must not hold the exit up for long.
    std::thread waiter([] {
        g_thread.join();
        g_done = true;
    });
    for (int i = 0; i < 100 && !g_done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_done)
        waiter.join();
    else
        waiter.detach();
}


void BuildMenu(shipmenu::Menu& m) {
    using shipmenu::WidgetInfo;
    using shipmenu::WidgetType;
    const char* H = "Network";
    const char* S = "Discord";
    m.AddSidebar(H, S, 1);
    auto needs_master = [](WidgetInfo& w) {
        if (!Opt("Enabled"))
            w.disabledReasons.push_back("Discord Rich Presence is off");
    };
    m.AddWidget(H, S, 0, "Rich Presence", WidgetType::SeparatorText);
    m.AddWidget(H, S, 0, "Discord Rich Presence", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Discord.Enabled")
        .DefaultValue(true)
        .Tooltip("Shows in Discord that you are playing, where you are, and whether you're online; never your room or other "
                 "players' names. Needs the Discord app running on this PC. Off: nothing is shown and the status is cleared.");
    m.AddWidget(H, S, 0, "Status", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        const char* text;
        if (g_app_id.empty() && g_checked_id)
            text = "Not available in this build";
        else if (!Opt("Enabled"))
            text = "Off";
        else if (g_connected)
            text = "Connected to Discord";
        else if (g_thread.joinable())
            text = "Discord not running (looking again every 15 s)";
        else
            text = "Waiting for the game";
        ImGui::Text("Status: %s", text);
        const bool usable = Opt("Enabled") && !g_app_id.empty();
        ImGui::BeginDisabled(!usable);
        if (ImGui::Button("Reconnect", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            g_reconnect = true;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Looks for Discord again now, instead of waiting for the next try.");
    });
    m.AddWidget(H, S, 0, "What Is Shown", WidgetType::SeparatorText);
    m.AddWidget(H, S, 0, "Show Location", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Discord.ShowLocation")
        .DefaultValue(true)
        .Tooltip("The island, town or dungeon you are in, with the same names as the online player list. Off: the area is left out.")
        .PreFunc(needs_master);
    m.AddWidget(H, S, 0, "Show Game Mode", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Discord.ShowMode")
        .DefaultValue(true)
        .Tooltip("\" - Randomizer\" after the status when you play the Randomizer.")
        .PreFunc(needs_master);
    m.AddWidget(H, S, 0, "Show Online Status", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Discord.ShowOnline")
        .DefaultValue(true)
        .Tooltip("\"Single player\", or \"Online with N players\" while you are connected to an Anchor room: only the number, never the "
                 "room's name, the players' names or the seed.")
        .PreFunc(needs_master);
    m.AddWidget(H, S, 0, "Show Elapsed Time", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Discord.ShowTime")
        .DefaultValue(true)
        .Tooltip("How long the game has been running, as Discord shows for a game.")
        .PreFunc(needs_master);
    m.AddWidget(H, S, 0, "Show on Title Screen and File Select", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Discord.ShowTitle")
        .DefaultValue(true)
        .Tooltip("\"On the title screen\" and \"Choosing a file\". Off: the status is cleared there until a game starts.")
        .PreFunc(needs_master);
}

} // namespace discord
