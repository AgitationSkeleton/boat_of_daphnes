// Entry point (tww): load main.dol into guest memory, set up the boot block the IPL would leave
// behind, and run the recompiled __start.
//   tww --iso <disc image> [--dol <main.dol>] [--headless] [--mute] [--volume 0-100] [--frames N] [--trace]
// (without --dol the program is read from the disc, as the console's apploader does; the disc image
// may be .iso/.gcm, .ciso, .rvz or .wia: src/disc)
#include "disc/discimage.h"
#include "runtime/bod.h"
#include "runtime/modswitch.h"
#include "runtime/interp.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"
#include "setup/firstrun.h"
#include "setup/setup_ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <ctime>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <SDL3/SDL_filesystem.h>

namespace rt {
void dvdOpenDisc(const std::string& path);  // src/runtime/hle_dvd.cpp
bool dvdOpenUnion(const std::string& randoIso, const std::string& gameIso, const std::string& changed);
bool loadSecondModule();  // src/runtime/game.cpp
void dumpAllThreads();                       // src/runtime/cpu.cpp
void installCrashHandler();                  // src/runtime/crash.cpp
void installWatch(u32 guest, u32 len);       // src/runtime/crash.cpp
int runFrontend(int argc, char** argv, bool headless, u64 maxFrames, void (*startGuest)());  // frontend.cpp
void audioInit(bool enabled, float volume);  // src/runtime/audio.cpp
}

using namespace rt;

static bool readFile(const std::string& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// main.dol from the disc image: the header's 0x420 is where it starts; its size is where its last
// section ends.
static bool readDiscDol(const std::string& iso, std::vector<u8>& out) {
    std::string err;
    const auto img = disc::open(std::filesystem::path(iso), err);
    if (!img) return false;
    auto be = [](const u8* p) { return u32(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
    u8 h[4], dh[0x100];
    if (!img->read(0x420, h, 4)) return false;
    const u32 at = be(h);
    if (!img->read(at, dh, sizeof dh)) return false;
    u32 size = 0x100;
    for (int i = 0; i < 18; i++) size = std::max(size, be(dh + i * 4) + be(dh + 0x90 + i * 4));
    out.resize(size);
    return img->read(at, out.data(), size);
}

// The boot block (OSBootInfo at 0x80000000 and friends) as the IPL leaves it for a retail disc.
static void setupLowMem(const std::vector<u8>& discHeader) {
    std::memcpy(hostp(0x80000000), discHeader.data(), 0x20);  // game id, maker, disc, version
    wr32(0x8000001C, 0xC2339F3D);  // DVD magic
    wr32(0x80000020, 0x0D15EA5E);  // normal boot
    wr32(0x80000024, 1);           // boot info version
    wr32(0x80000028, kMem1Size);   // physical memory size
    wr32(0x8000002C, 0x00000003);  // console type: retail
    wr32(0x80000030, 0);           // arena lo: linker default
    wr32(0x80000034, 0x80000000 + kMem1Size);  // arena hi: lowered to the FST by dvdOpenDisc
    wr32(0x80000038, 0);                        // FST location, size: set by dvdOpenDisc
    wr32(0x8000003C, 0);
    wr32(0x800000CC, 0);           // TV mode: NTSC
    wr32(0x800000F0, kMem1Size);   // simulated memory size
    wr32(0x800000F8, 162000000);   // bus clock
    wr32(0x800000FC, 486000000);   // CPU clock
    wr16(0x800030E6, 0x8003);      // device code: retail
}

namespace rt {
void hostTimingInit();               // src/runtime/hosttime.cpp
void logToFile(const char* path);    // src/runtime/cpu.cpp
bool loadGameModule(bool quiet, bool rando);  // src/runtime/game.cpp
void gameModuleMemory();             // src/runtime/game.cpp
}

namespace {
std::filesystem::path exeFolder() {
    const char* base = SDL_GetBasePath();
    return base ? std::filesystem::u8path(base) : std::filesystem::current_path();
}
}  // namespace

int main(int argc, char** argv) {
    hostTimingInit();
    std::string dolPath, isoPath;
    int watchSecs = 0;
    bool headless = false, setupOnly = false, mute = std::getenv("SA2_MUTE") != nullptr;
    u64 maxFrames = 0;
    float volume = std::getenv("SA2_VOLUME") ? float(std::atof(std::getenv("SA2_VOLUME"))) / 100.f : 1.f;  // percent
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--dol" && i + 1 < argc) dolPath = argv[++i];
        else if (a == "--iso" && i + 1 < argc) isoPath = argv[++i];
        else if (a == "--trace") g_traceOn = true;
        else if (a == "--watch" && i + 1 < argc) watchSecs = std::atoi(argv[++i]);
        else if (a == "--headless") headless = true;
        else if (a == "--setup") setupOnly = true;  // build the game's code from the disc without a window, then exit
        else if (a == "--mute") mute = true;
        else if (a == "--volume" && i + 1 < argc) volume = float(std::atof(argv[++i])) / 100.f;
#if defined(_WIN32)
        else if (a == "--fast-disc") _putenv("SA2_FAST_DISC=1");  // read by the drive model (hle_dvd.cpp)
#else
        else if (a == "--fast-disc") setenv("SA2_FAST_DISC", "1", 1);
#endif
        else if (a == "--frames" && i + 1 < argc) maxFrames = std::strtoull(argv[++i], nullptr, 10);
    }
    // Boat of Daphnes' settings first: they say whether this session is a randomizer one (its file select restarted
    // the game into it), which runs the randomizer's module on the randomizer's disc (game/rando/ beside the program).
    bod::hostSettings();
    if (setupOnly) {
        const std::string saved = bod::cvarString("gSettings.DiscPath", "");
        return setup::runFirstRunCli(exeFolder(), std::filesystem::u8path(isoPath.empty() ? saved : isoPath));
    }
    if (!headless) bod::launchBegin();
    // The disc: the command line's, else the one used before. The first time (or after an update), the game's code is
    // built from it before anything else (src/setup): the player only clicks the program.
    if (!headless) {
        std::filesystem::path disc = isoPath.empty() ? std::filesystem::u8path(bod::cvarString("gSettings.DiscPath", "")) : std::filesystem::u8path(isoPath);
        std::error_code ec;
        const bool known = !disc.empty() && std::filesystem::exists(disc, ec);
        if (!known || setup::FirstRun::needed(exeFolder(), disc)) {
            if (!setup::runFirstRun(exeFolder(), disc, known)) return 0;
        }
        isoPath = disc.string();
        bod::cvarSetString("gSettings.DiscPath", std::string(reinterpret_cast<const char*>(disc.u8string().c_str())));
        bod::cvarSave();
    }
    {   // the seed generator's disc (Boat of Daphnes' randomizer menu): a plain ISO, the setup's copy when the disc is not one
        std::error_code ec;
        const std::filesystem::path plain = exeFolder() / "game" / "GZLE01.iso";
        const std::string seedDisc = std::filesystem::exists(plain, ec) ? plain.string() : isoPath;
#if defined(_WIN32)
        if (!seedDisc.empty() && !std::getenv("BLUEWAKE_DISC")) _putenv_s("BLUEWAKE_DISC", seedDisc.c_str());
#else
        if (!seedDisc.empty()) setenv("BLUEWAKE_DISC", seedDisc.c_str(), 0);
#endif
    }
    // The randomizer beside the game (game/rando/ and tww_game_rando.dll): both programs loaded, switched while the
    // game runs (src/runtime/modswitch.cpp). Without the function table, the file select restarts the game into it.
    const std::filesystem::path randoDir = exeFolder() / "game" / "rando";
    const std::string randoIso = (randoDir / "GZLE99.iso").string();
    bool both = !isoPath.empty() && std::filesystem::exists(randoIso) && std::filesystem::exists(randoDir / "changed.txt") &&
                std::filesystem::exists(exeFolder() / "tww_game_rando.diff");
    bool rando = !both && bod::randoSessionWanted();
    if (rando) {
        const std::filesystem::path disc = exeFolder() / "game" / "rando" / "GZLE99.iso";
        if (std::filesystem::exists(disc) && std::filesystem::exists(exeFolder() / "tww_game_rando.dll")) {
            isoPath = disc.string();
            dolPath.clear();
        } else {
            std::fprintf(stderr, "[rando] the randomizer's module or disc is missing (%s): a vanilla session\n", disc.string().c_str());
            rando = false;
        }
    }
    // The game's code: tww_game.dll (.so on Linux), which the launcher builds from the player's disc.
    if (!loadGameModule(headless, rando)) return 1;
    both = both && loadSecondModule();
    // Played sessions keep their log; test runs (headless) capture
    // the console instead and leave it alone.
    // (logs\session-<date>.log in the data folder, which Settings > Open Logs Folder opens; a crash's report goes in it,
    // src/runtime/crash.cpp; the newest ten sessions are kept)
    if (!headless) {
        std::error_code ec;
        const std::filesystem::path logs = std::filesystem::current_path() / "logs";
        std::filesystem::create_directories(logs, ec);
        std::vector<std::filesystem::path> old;
        for (const auto& e : std::filesystem::directory_iterator(logs, ec))
            if (e.path().filename().string().rfind("session-", 0) == 0) old.push_back(e.path());
        std::sort(old.begin(), old.end());
        for (size_t i = 0; i + 19 < old.size(); i++) std::filesystem::remove(old[i], ec);
        char name[64];
        const std::time_t now = std::time(nullptr);
        std::strftime(name, sizeof name, "session-%Y%m%d-%H%M%S.log", std::localtime(&now));
        logToFile((logs / name).string().c_str());
        // Boat of Daphnes' own messages (its menus, the randomizer's file select, online play: stderr) beside it
        std::string host = name;
        host.replace(host.size() - 4, 4, "-host.log");
        if (!std::freopen((logs / host).string().c_str(), "w", stderr)) log("[log] the host log could not be opened\n");
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    installCrashHandler();
    audioInit(!headless && !mute, volume);  // headless runs are always silent
    if (!memInit()) return std::fprintf(stderr, "out of memory\n"), 1;
    gameModuleMemory();
    std::vector<u8> dol;
    if (dolPath.empty() && !isoPath.empty()) {
        if (!readDiscDol(isoPath, dol)) return std::fprintf(stderr, "can't read the program from %s\n", isoPath.c_str()), 1;
        dolPath = "main.dol (disc)";
    } else {
        if (dolPath.empty()) dolPath = "orig/main.dol";
        if (!readFile(dolPath, dol)) return std::fprintf(stderr, "can't read %s\n", dolPath.c_str()), 1;
    }
    std::vector<u8> randoDol;
    if (both && !readDiscDol(randoIso, randoDol)) both = false;
    u32 entry = 0;
    if (!loadDol(dol, entry)) return std::fprintf(stderr, "bad DOL %s\n", dolPath.c_str()), 1;
    std::vector<u8> header(0x20, 0);
    if (!isoPath.empty()) {
        std::string err;
        if (const auto img = disc::open(std::filesystem::path(isoPath), err)) img->read(0, header.data(), 0x20);
    } else {
        std::memcpy(header.data(), "GZLE01", 6);
    }
    // A randomizer disc says GZLE99; its files share the card with vanilla ones (Boat of Daphnes keeps one card and a
    // list of which files are randomizer ones), so the game is told GZLE01.
    if (rando) std::memcpy(header.data(), "GZLE01", 6);
    setupLowMem(header);
    if (both) {
        // (The randomizer's added section, 0x803FCFA8 on, is the bottom of the game's boot stack, which the boot thread -
        // suspended in main, near the stack's top - never reaches: a switch writes it there, as Boat of Daphnes' does.)
        both = dvdOpenUnion(randoIso, isoPath, (randoDir / "changed.txt").string());
    }
    if (!isoPath.empty() && !both) dvdOpenDisc(isoPath);  // also places the FST and records it in the boot block
    registerDol();
    resolveHooks();
    if (both) both = modswitchInit((exeFolder() / "tww_game_rando.diff").string(), dol, randoDol);
    interpInit();
    bod::randoSessionStarts(rando, both);
    gameFixesInit();
    if (const char* w = std::getenv("SA2_WATCH")) {  // SA2_WATCH=<hex address>[:<length>]
        const char* colon = std::strchr(w, ':');
        installWatch(u32(std::strtoul(w, nullptr, 16)), colon ? u32(std::strtoul(colon + 1, nullptr, 0)) : 4);
    }
    log("[boot] %s entry %08X\n", dolPath.c_str(), entry);
    if (const char* e = std::getenv("SA2_THREADS_EVERY")) {  // <seconds>: where every guest thread is, now and then
        const int secs = std::max(1, std::atoi(e));
        std::thread([secs] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(secs));
                sched::debugDump();
            }
        }).detach();
    }
    if (watchSecs > 0) {
        g_traceOn = true;
        std::thread([watchSecs] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(watchSecs));
                dumpAllThreads();
                sched::debugDump();
            }
        }).detach();
    }
    static u32 s_entry;
    s_entry = entry;
    // The game runs on its own host thread; this one owns the window.
    return runFrontend(argc, argv, headless, maxFrames, [] {
        static Ctx boot;
        boot.msr = 0;  // __start runs with interrupts off; OSInit enables them
        boot.call(s_entry);
        log("[boot] __start returned\n");
    });
}
