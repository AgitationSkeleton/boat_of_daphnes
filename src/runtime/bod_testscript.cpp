// Scenario scripts (testing only): BLUEWAKE_TEST_SCRIPT=<file> is a list of commands the game thread works through,
// one retrace at a time, from the start of the run: load a save through the file select by what the game shows, wait
// for Link to be playable, edit the save (items, equipment, flags, rupees), warp, press buttons, take frame dumps,
// open the menu on a page, read and write guest memory. tools/scenario.py writes the file from a scenario
// (tools/scenarios/*.txt) and runs the game with it; docs/scenarios.md lists the commands.
//
// A command that waits (wait, start, warp, press, ...) holds the ones after it; the others run at once. Edits wait for a
// safe moment first (a play scene with Link in it, no event running, no scene change under way). Times are retraces
// (60 a second, the game runs 30 frames a second on them), counted from the start of the command, so a script is
// the same whatever time the loading takes.
#include "runtime/bod_bridge.h"

#include "../bod/shim/bod_compat.h"

#include "core/cpu.h"
#include "dev_tools.h"
#include "game_state.h"
extern "C" {
#include "bunny_hood.h"
#include "chart_reveal.h"
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" bool bluewake_test_script_menu(char* spec, unsigned size);  // win_settings.cpp asks for the menu requests

namespace devtools {
std::string RunCommand(const std::string& line);  // dev_windows.cpp: the Dev Tools console's commands
}

namespace bod = rt::bod;

namespace {
constexpr u32 kInfo = 0x803C4C08u;       // g_dComIfG_gameInfo
constexpr u32 kCurStage = 0x803C9D3Cu;   // play.mCurStage: name[8], point (s16), room (s8), layer (s8)
constexpr u32 kNextStage = 0x803C9D48u;  // play.mNextStage: as mCurStage, then +0xC enable, +0xD wipe
constexpr u32 kEventMode = 0x803C9EA2u;  // play.mEvtCtrl.mMode: 0 when no event runs
constexpr u32 kOverlap = 0x803F6160u;    // l_fopOvlpM_overlap[0]: a scene change's overlay while one runs
constexpr u32 kPlayerPtr = 0x803CA754u;  // play.mpPlayerPtr[0]
constexpr u32 kSaveTime = kInfo + 0x18 + 0xC;    // dSv_player_status_b_c::mTime (f32, degrees)
constexpr u32 kSelectSave = kInfo + 0x09;        // dSv_player_status_a_c::mSelectItem[3]: inventory slots for X, Y, Z
constexpr u32 kSelectPlay = kInfo + 0x12A0 + 0x4933;  // the play scene's copy: item numbers for X, Y, Z

enum Pad : unsigned {
    kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kZ = 0x0010, kR = 0x0020, kL = 0x0040,
    kA = 0x0100, kB = 0x0200, kX = 0x0400, kY = 0x0800, kStart = 0x1000,
};

struct Command {
    std::string verb;
    std::vector<std::string> args;
    int line = 0;
};

std::string g_menuRequest;  // a menu page asked for, taken by the UI thread (under S.lock)

CPUState s_cpu{};
CPUState* cpu() {
    if (!s_cpu.ram) {
        s_cpu.ram = bod::guestRam();
        s_cpu.ram_size = 0x02000000u;
    }
    return &s_cpu;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

unsigned buttonsOf(const std::string& spec) {
    unsigned mask = 0;
    size_t i = 0;
    while (i <= spec.size()) {
        size_t j = spec.find_first_of("+,", i);
        if (j == std::string::npos) j = spec.size();
        const std::string b = lower(spec.substr(i, j - i));
        i = j + 1;
        if (b.empty()) continue;
        static const struct { const char* name; unsigned bit; } names[] = {
            {"a", kA}, {"b", kB}, {"x", kX}, {"y", kY}, {"start", kStart}, {"z", kZ}, {"r", kR}, {"l", kL},
            {"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight},
        };
        bool found = false;
        for (const auto& n : names)
            if (b == n.name) mask |= n.bit, found = true;
        if (!found) std::fprintf(stderr, "[script] unknown button '%s'\n", b.c_str());
    }
    return mask;
}

// ---- the game's state, as the script asks about it ----
u64 now() { return bod::retraceCount(); }

std::string stageName() {
    char name[9] = {};
    for (u32 i = 0; i < 8; ++i) name[i] = char(mem_read8(cpu(), kCurStage + i));
    return name;
}

// A play scene with Link in it, nothing to wait for: no event, no scene change started or under way.
bool playable(const BluewakeLocation& loc) {
    return loc.scene_proc == BLUEWAKE_SCENE_PLAY && loc.has_player && mem_read8(cpu(), kEventMode) == 0 &&
           mem_read8(cpu(), kNextStage + 0xC) == 0 && mem_read32(cpu(), kOverlap) == 0;
}

// ---- the script ----
struct Script {
    std::vector<Command> cmds;
    size_t at = 0;
    bool began = false;     // the current command has started
    u64 began_at = 0;       // its first retrace
    u64 safe_since = 0;     // the first retrace of the present run of playable retraces (0: not playable now)
    u64 placed_since = 0;   // the same without the no-event condition: Link is in a play scene and no change is under way
    int phase = 0;          // a command's own steps
    int tries = 0;
    u64 phase_at = 0;
    bool finished = false;
    u64 finished_at = 0;
    bool loaded = false;
    // input
    unsigned held = 0;              // hold / release
    int hx = 0, hy = 0, hcx = 0, hcy = 0;
    unsigned pulse = 0;             // a timed press (press / stick / cstick) until pulse_until
    int px = 0, py = 0, pcx = 0, pcy = 0;
    u64 pulse_until = 0;
    // time of day kept
    bool time_hold = false;
    float time_deg = 0;
    // frame dump requests
    std::string shot_path;
    bool shot_pending = false;
    int shots = 0;
    std::string shot_dir;
    std::atomic<bool> snap{false};  // a shutter release asked for (the game thread's dSnap_Execute takes it)
    std::mutex lock;  // the input and the shot request: the frontend reads them on its own thread
} S;

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[script] %s\n", text);
}

void load() {
    const char* path = std::getenv("BLUEWAKE_TEST_SCRIPT");
    if (!path || !*path) return;
    std::ifstream in(path);
    if (!in) {
        log("cannot read %s", path);
        return;
    }
    if (const char* d = std::getenv("BLUEWAKE_TEST_SHOTS")) S.shot_dir = d;
    else if (const char* d2 = std::getenv("SA2_DUMP_DIR")) S.shot_dir = d2;
    else S.shot_dir = ".";
    std::string raw;
    int n = 0;
    while (std::getline(in, raw)) {
        ++n;
        if (const size_t hash = raw.find('#'); hash != std::string::npos) raw.resize(hash);
        std::istringstream words(raw);
        Command c;
        c.line = n;
        if (!(words >> c.verb)) continue;
        c.verb = lower(c.verb);
        for (std::string w; words >> w;) c.args.push_back(w);
        S.cmds.push_back(std::move(c));
    }
    log("%zu commands from %s", S.cmds.size(), path);
}

long num(const std::vector<std::string>& a, size_t i, long fallback) {
    return i < a.size() ? std::strtol(a[i].c_str(), nullptr, 0) : fallback;
}

void setInput(unsigned buttons, int x, int y, int cx, int cy, u64 frames) {
    std::lock_guard g(S.lock);
    S.pulse = buttons, S.px = x, S.py = y, S.pcx = cx, S.pcy = cy;
    S.pulse_until = now() + frames;
}

float readFloat(u32 a) {
    const u32 v = mem_read32(cpu(), a);
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}
void writeFloat(u32 a, float f) {
    u32 v;
    std::memcpy(&v, &f, sizeof v);
    mem_write32(cpu(), a, v);
}

void writeStage(const std::string& stage, int room, int point, int layer, int wipe) {
    CPUState* c = cpu();
    for (u32 i = 0; i < 8; ++i) mem_write8(c, kNextStage + i, i < stage.size() ? u8(stage[i]) : u8(0));
    mem_write16(c, kNextStage + 0x8, u16(point));
    mem_write8(c, kNextStage + 0xA, u8(room));
    mem_write8(c, kNextStage + 0xB, u8(layer));
    mem_write8(c, kNextStage + 0xD, u8(wipe));
    mem_write8(c, kNextStage + 0xC, 1);
}

// An address as the script writes it: 0x803C4C0C, or actor:PROC+OFFSET for a field of the first live actor of that process
// (fpcNm number, as the console's actors list shows: actor:0x88+0x2F4 is Obj_Ajav's sphere hit flag). 0 when there is none.
u32 addressOf(const std::string& text) {
    if (text.compare(0, 6, "actor:") != 0) return u32(std::strtoul(text.c_str(), nullptr, 0));
    const long proc = std::strtol(text.c_str() + 6, nullptr, 0);
    const size_t plus = text.find('+');
    const u32 offset = plus == std::string::npos ? 0u : u32(std::strtoul(text.c_str() + plus + 1, nullptr, 0));
    static std::vector<BluewakeDevActor> list(2048);
    const unsigned count = bluewake_dev_actors(list.data(), unsigned(list.size()));
    for (unsigned i = 0; i < count; ++i)
        if (list[i].proc == proc) return list[i].address + offset;
    return 0;
}

// One command: true when done. `first` is true on its first call.
bool runCommand(const Command& c, bool first, u64 since) {
    const auto& a = c.args;
    const std::string& v = c.verb;
    const BluewakeLocation loc = bluewake_game_location();
    const u64 t = now();
    const u64 elapsed = t - since;
    auto timedOut = [&](u64 limit, const char* what) {
        if (elapsed < limit) return false;
        log("TIMEOUT line %d: %s (after %llu retraces)", c.line, what, (unsigned long long)elapsed);
        return true;
    };
    const bool safe = playable(loc) && S.safe_since && t - S.safe_since >= 20;
    const bool placed = S.placed_since && t - S.placed_since >= 20;  // (a scene's opening event may still run)
    // ---- time ----
    if (v == "wait") {
        // wait N | wait link | wait stage NAME [room] | wait event | wait noevent | wait menu
        if (a.empty()) return true;
        const std::string w = lower(a[0]);
        if (w == "link" || w == "safe") return safe || timedOut(u64(num(a, 1, 3600)), "Link never became playable");
        if (w == "stage") {
            const bool room_ok = a.size() < 3 || loc.room == std::atoi(a[2].c_str());
            return (safe && a.size() > 1 && stageName() == a[1] && room_ok) ||
                   timedOut(3600, ("stage " + (a.size() > 1 ? a[1] : std::string())).c_str());
        }
        if (w == "event") return mem_read8(cpu(), kEventMode) != 0 || timedOut(1800, "no event started");
        if (w == "noevent") return mem_read8(cpu(), kEventMode) == 0 || timedOut(3600, "the event never ended");
        return elapsed >= u64(std::strtol(a[0].c_str(), nullptr, 0));
    }
    if (v == "log") {
        std::string m;
        for (const auto& w : a) m += (m.empty() ? "" : " ") + w;
        log("%s", m.c_str());
        return true;
    }
    if (v == "quit" || v == "end") {
        log("quit at retrace %llu", (unsigned long long)t);
        S.finished = true;
        S.finished_at = t;
        return true;
    }
    // ---- getting into the game ----
    if (v == "start") {
        // start load [slot 0..2] | start new [slot]: the title and the file select, driven by what they show.
        const bool fresh = !a.empty() && lower(a[0]) == "new";
        const int slot = int(num(a, 1, 0));
        if (first) log("start %s, slot %d", fresh ? "new" : "load", slot);
        if (timedOut(fresh ? 12000 : 6000, "the game did not start")) return true;
        const bool event = mem_read8(cpu(), kEventMode) != 0;
        if (loc.scene_proc == BLUEWAKE_SCENE_PLAY && loc.has_player && (!fresh || (!event && safe)))
            return S.safe_since != 0 && (t - S.safe_since >= 60 || !fresh);
        if (elapsed % 600 == 0)
            log("start: scene %04X file select %d/%d slot %d stage %s event %d", loc.scene_proc, loc.fs_main, loc.fs_sub, loc.fs_slot,
                stageName().c_str(), int(event));
        if (t < S.phase_at) return false;  // a press is under way
        auto press = [&](unsigned b, u64 hold, u64 gap) { setInput(b, 0, 0, 0, 0, hold), S.phase_at = t + hold + gap; };
        if (loc.scene_proc == BLUEWAKE_SCENE_NAME && loc.fs_main == 4) {
            if (loc.fs_sub == 0 && loc.fs_slot != slot) {
                press(loc.fs_slot < slot ? kDown : kUp, 8, 20);
                return false;
            }
            press(kA, 8, 70);
            return false;
        }
        if (loc.scene_proc == BLUEWAKE_SCENE_NAME && (loc.fs_main == 6 || loc.fs_main == 7)) {
            // the name entry: START is END, then A confirms
            press((++S.tries & 1) ? kStart : kA, 8, 60);
            return false;
        }
        if (loc.scene_proc == BLUEWAKE_SCENE_PLAY) {
            // (the title screen is a play scene too: the sea backdrop, stage sea_T, with no Link in it)
            if (stageName() == "sea_T") press(kStart, 8, 80);
            else if (event || !loc.has_player) press(kA, 8, 40);  // talk and cutscenes: through
            return false;
        }
        // title, opening movie, logos
        press(kStart, 8, 80);
        return false;
    }
    // ---- everything below changes the game or looks at it ----
    if (v == "warp") {
        // warp STAGE ROOM [POINT [LAYER [WIPE]]]: a scene change as a door makes one. Waits for the scene it leaves to be
        // playable, asks, checks the game took it up (asks again if not) and waits for the new scene.
        if (a.size() < 2) return log("line %d: warp STAGE ROOM [POINT [LAYER]]", c.line), true;
        const int room = int(num(a, 1, 0)), point = int(num(a, 2, 0)), layer = int(num(a, 3, -1)), wipe = int(num(a, 4, 0));
        if (S.phase == 0) {
            if (!safe && !timedOut(3600, "no playable moment to warp from")) return false;
            if (elapsed >= 3600) return true;
            writeStage(a[0], room, point, layer, wipe);
            log("warp %s room %d point %d layer %d (from %s room %d)", a[0].c_str(), room, point, layer, stageName().c_str(), loc.room);
            S.phase = 1, S.phase_at = t, S.tries = 1;
            return false;
        }
        if (S.phase == 1) {  // taken up: the overlay shows or the enable is cleared
            if (mem_read32(cpu(), kOverlap) != 0 || mem_read8(cpu(), kNextStage + 0xC) == 0) return S.phase = 2, false;
            if (t - S.phase_at > 90) {
                if (++S.tries > 4) return log("TIMEOUT line %d: the game ignored the warp", c.line), true;
                writeStage(a[0], room, point, layer, wipe);
                S.phase_at = t;
            }
            return false;
        }
        const bool arrived = placed && stageName() == a[0] && loc.room == room;
        return arrived || timedOut(5400, "the warp did not arrive");
    }
    if (v == "teleport") {
        // teleport STAGE ROOM X Y Z [ANGLE]: Link to a place in a stage (the game's restart record, as Anchor's teleport)
        if (a.size() < 5) return log("line %d: teleport STAGE ROOM X Y Z [ANGLE]", c.line), true;
        if (S.phase == 0) {
            if (!safe && !timedOut(3600, "no playable moment to teleport from")) return false;
            if (elapsed >= 3600) return true;
            const float pos[3] = {std::strtof(a[2].c_str(), nullptr), std::strtof(a[3].c_str(), nullptr), std::strtof(a[4].c_str(), nullptr)};
            bluewake_request_teleport(a[0].c_str(), int(num(a, 1, 0)), pos, int(num(a, 5, 0)));
            S.phase = 1, S.phase_at = t;
            return false;
        }
        if (S.phase == 1) {  // started: the enable shows within a few retraces
            if (mem_read8(cpu(), kNextStage + 0xC) != 0 || mem_read32(cpu(), kOverlap) != 0) S.phase = 2;
            else if (t - S.phase_at > 120) return log("TIMEOUT line %d: the teleport did not start", c.line), true;
            return false;
        }
        return (placed && stageName() == a[0] && loc.room == num(a, 1, 0)) || timedOut(5400, "the teleport did not arrive");
    }
    if (v == "menu") {
        // menu HEADER/PAGE | menu close: the Boat of Daphnes menu, opened on a page (a window the UI thread draws)
        std::lock_guard g(S.lock);
        g_menuRequest = a.empty() ? "close" : a[0];
        for (size_t i = 1; i < a.size(); ++i) g_menuRequest += " " + a[i];
        log("menu %s", g_menuRequest.c_str());
        return true;
    }
    if (v == "shot") {
        // shot [NAME]: a frame dump (the game's picture, as SA2_DUMP_DIR writes them) from the next presented frame
        if (first) {
            std::lock_guard g(S.lock);
            std::string name = a.empty() ? "shot" + std::to_string(++S.shots) : a[0];
            S.shot_path = S.shot_dir + "/" + name + ".rgba";
            S.shot_pending = true;
            log("shot %s", name.c_str());
        }
        std::lock_guard g(S.lock);
        return !S.shot_pending || timedOut(300, "the frame was not dumped");
    }
    if (v == "snap") {
        // snap: the Picto Box's shutter, released directly (what the A button starts) while its viewfinder is up
        S.snap = true;
        return true;
    }
    if (v == "winshot") {  // for the tool: it captures the process's windows when it reads this line
        if (first) log("winshot %s", a.empty() ? "win" : a[0].c_str());
        return elapsed >= 45;
    }
    if (v == "press" || v == "tap") {
        // press BUTTON[+BUTTON] [FRAMES=8]: held for that many retraces, then released
        if (first) setInput(buttonsOf(a.empty() ? "" : a[0]), 0, 0, 0, 0, u64(num(a, 1, 8)));
        return elapsed >= u64(num(a, 1, 8)) + 6;
    }
    if (v == "skip") {
        // skip [TIMEOUT]: A through cutscenes and text until no event runs (for 30 retraces)
        const bool event = mem_read8(cpu(), kEventMode) != 0;
        if (event) S.phase_at = t;
        if (!event && t - S.phase_at >= 30) return true;
        if (timedOut(u64(num(a, 0, 3600)), "the event did not end")) return true;
        if (event && (elapsed % 50) == 0) setInput(kA, 0, 0, 0, 0, 8);
        return false;
    }
    if (v == "hold") {  // hold BUTTON[+BUTTON]: until release
        std::lock_guard g(S.lock);
        S.held |= buttonsOf(a.empty() ? "" : a[0]);
        return true;
    }
    if (v == "release") {  // release [BUTTON | all]
        std::lock_guard g(S.lock);
        S.held = a.empty() || lower(a[0]) == "all" ? 0 : (S.held & ~buttonsOf(a[0]));
        if (a.empty() || lower(a[0]) == "all") S.hx = S.hy = S.hcx = S.hcy = 0;
        return true;
    }
    if (v == "stick" || v == "cstick") {
        // stick X Y [FRAMES] (-128..127; no frames: kept until the next stick): the main stick or the C-stick
        const int x = int(num(a, 0, 0)), y = int(num(a, 1, 0));
        const bool cs = v == "cstick";
        if (a.size() < 3) {
            std::lock_guard g(S.lock);
            (cs ? S.hcx : S.hx) = x, (cs ? S.hcy : S.hy) = y;
            return true;
        }
        if (first) setInput(0, cs ? 0 : x, cs ? 0 : y, cs ? x : 0, cs ? y : 0, u64(num(a, 2, 0)));
        return elapsed >= u64(num(a, 2, 0)) + 2;
    }
    if (v == "peek") {
        // peek ADDRESS[/SIZE] [LABEL]: logs "[script] peek 0x... = 0x..." (size 1, 2 or 4)
        if (a.empty()) return true;
        const u32 addr = addressOf(a[0]);
        const size_t slash = a[0].find('/');
        const int size = slash == std::string::npos ? 4 : std::atoi(a[0].c_str() + slash + 1);
        if (addr == 0) return log("peek %s: no such actor", a[0].c_str()), true;
        const u32 val = size == 1 ? mem_read8(cpu(), addr) : size == 2 ? mem_read16(cpu(), addr) : mem_read32(cpu(), addr);
        log("peek 0x%08X/%d = 0x%X (%u)%s%s", addr, size, val, val, a.size() > 1 ? " " : "", a.size() > 1 ? a[1].c_str() : "");
        return true;
    }
    if (v == "poke") {
        // poke ADDRESS=VALUE[/SIZE]
        for (const auto& w : a) {
            const size_t eq = w.find('=');
            if (eq == std::string::npos) continue;
            const u32 addr = addressOf(w.substr(0, eq));
            if (addr == 0) {
                log("poke %s: no such actor", w.c_str());
                continue;
            }
            const size_t slash = w.find('/', eq);
            const int size = slash == std::string::npos ? 4 : std::atoi(w.c_str() + slash + 1);
            const u32 val = u32(std::strtoull(w.c_str() + eq + 1, nullptr, 0));
            if (size == 1) mem_write8(cpu(), addr, u8(val));
            else if (size == 2) mem_write16(cpu(), addr, u16(val));
            else mem_write32(cpu(), addr, val);
            log("poke 0x%08X/%d = 0x%X", addr, size, val);
        }
        return true;
    }
    if (v == "reveal") {  // reveal 0|1: Reveal Sea Chart
        bluewake_chart_reveal_enable(num(a, 0, 1) != 0);
        log("reveal sea chart %s", num(a, 0, 1) ? "on" : "off");
        return true;
    }
    if (v == "bunny") {  // bunny 0|1: the Bunny Hood feature
        bluewake_bunny_hood_enable(num(a, 0, 1) != 0);
        log("bunny hood feature %s", num(a, 0, 1) ? "on" : "off");
        return true;
    }
    // ---- save edits: wait for a safe moment, then through the Dev Tools console's own commands ----
    static const char* const edits[] = {"give", "equip", "set", "flag", "console", "rupees", "health", "maxhealth", "magic", "bombs",
                                        "arrows", "time", "spawn", "collect"};
    bool edit = false;
    for (const char* e : edits) edit |= v == e;
    if (edit) {
        if (!safe && !timedOut(3600, "no playable moment for a save edit")) return false;
        if (elapsed >= 3600) return true;
        std::string line;
        auto join = [&](size_t from) {
            std::string s;
            for (size_t i = from; i < a.size(); ++i) s += (s.empty() ? "" : " ") + a[i];
            return s;
        };
        if (v == "console") line = join(0);
        else if (v == "rupees" || v == "health" || v == "maxhealth" || v == "magic") line = "set " + v + " " + join(0);
        else if (v == "bombs") line = "give bomb_bag " + join(0);
        else if (v == "arrows") line = "give bow " + join(0);
        else if (v == "time") {
            // time DEGREES [hold]: the time of day (0 to 360, as the Save Editor's slider), kept while "hold" is given
            S.time_deg = std::strtof(a.empty() ? "180" : a[0].c_str(), nullptr);
            S.time_hold = a.size() > 1 && lower(a[1]) == "hold";
            writeFloat(kSaveTime, S.time_deg);
            log("time of day %.0f%s", S.time_deg, S.time_hold ? " (kept)" : "");
            return true;
        } else line = v + " " + join(0);
        const std::string answer = devtools::RunCommand(line);
        log("%s -> %s", line.c_str(), answer.c_str());
        return true;
    }
    log("line %d: unknown command '%s'", c.line, v.c_str());
    return true;
}
}  // namespace

extern "C" bool bluewake_test_script_menu(char* spec, unsigned size) {
    std::lock_guard g(S.lock);
    if (g_menuRequest.empty()) return false;
    std::snprintf(spec, size, "%s", g_menuRequest.c_str());
    g_menuRequest.clear();
    return true;
}

namespace rt::bod {
void testScriptRetrace() {
    static bool inited;
    if (!inited) {
        inited = true;
        load();
    }
    if (S.cmds.empty()) return;
    const u64 t = now();
    const BluewakeLocation loc = bluewake_game_location();
    if (playable(loc)) {
        if (!S.safe_since) S.safe_since = t;
    } else {
        S.safe_since = 0;
    }
    if (loc.scene_proc == BLUEWAKE_SCENE_PLAY && loc.has_player && mem_read8(cpu(), kNextStage + 0xC) == 0 && mem_read32(cpu(), kOverlap) == 0) {
        if (!S.placed_since) S.placed_since = t;
    } else {
        S.placed_since = 0;
    }
    if (S.time_hold) writeFloat(kSaveTime, S.time_deg);
    if (S.finished) {
        if (t >= S.finished_at + 45) bod::requestStop();  // (a dump asked for last is written first)
        return;
    }
    // a command may finish and the next begin in the same retrace; the ones that only wait stop the loop
    for (int guard = 0; guard < 64 && S.at < S.cmds.size(); ++guard) {
        const bool first = !S.began;
        if (first) {
            S.began = true, S.began_at = t, S.phase = 0, S.tries = 0;
            const Command& c = S.cmds[S.at];
            std::string text = c.verb;
            for (const auto& w : c.args) text += " " + w;
            log("line %d at retrace %llu: %s", c.line, (unsigned long long)t, text.c_str());
        }
        if (!runCommand(S.cmds[S.at], first, S.began_at)) return;
        ++S.at, S.began = false;
    }
    if (S.at >= S.cmds.size() && !S.finished) {
        log("done at retrace %llu", (unsigned long long)t);
        S.finished = true, S.finished_at = t;
    }
}

bool testScriptPad(uint16_t* buttons, int8_t* stickX, int8_t* stickY, int8_t* cstickX, int8_t* cstickY) {
    if (S.cmds.empty()) return false;
    std::lock_guard g(S.lock);
    const bool pulsing = now() < S.pulse_until;
    unsigned b = S.held | (pulsing ? S.pulse : 0);
    int x = S.hx, y = S.hy, cx = S.hcx, cy = S.hcy;
    if (pulsing) {
        if (S.px || S.py) x = S.px, y = S.py;
        if (S.pcx || S.pcy) cx = S.pcx, cy = S.pcy;
    }
    if (b & kUp) y = 100;
    if (b & kDown) y = -100;
    if (b & kLeft) x = -100;
    if (b & kRight) x = 100;
    *buttons = uint16_t(b), *stickX = int8_t(x), *stickY = int8_t(y), *cstickX = int8_t(cx), *cstickY = int8_t(cy);
    return b || x || y || cx || cy;
}

bool testScriptShot(char* out, unsigned size) {
    if (S.cmds.empty()) return false;
    std::lock_guard g(S.lock);
    if (!S.shot_pending) return false;
    std::snprintf(out, size, "%s", S.shot_path.c_str());
    S.shot_pending = false;
    return true;
}
bool testScriptSnap() { return S.snap.exchange(false); }
}  // namespace rt::bod
