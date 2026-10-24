// The host API Boat of Daphnes' code calls - its RecompCore runtime's and its Aurora fork's - on
// this runtime: its menus, windows and settings (src/bod, its own sources, unchanged) run on top.
// Where this runtime does a thing differently, the call is translated; where it has no equivalent
// yet, the call is accepted and does nothing (the menu stays as Boat of Daphnes has it).
#include "runtime/bod.h"
#include "runtime/bod_bridge.h"
#include "runtime/gx.h"
#include "runtime/runtime.h"

#include "platform/bod_platform.h"
#include "win_settings.h"
#include "launch_marker.h"

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

extern "C" {
bool bw_settings_menu_is_open(void);
bool bluewake_settings_menu_event(const void* sdl_event);
void bluewake_mods_disc_ready(void);
int bw_settings_relaunch(void);
}

namespace rt::bod {

namespace {
void (*s_overlay)(void*) = nullptr;
void* s_overlayUser = nullptr;
std::atomic<bool> s_installed{false};
void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
}  // namespace

void hostSettings() {
    bw_settings_capture_environment();
#if !defined(_WIN32)
    // The program's folder is the working one (settings, saves/, logs/, mods/): a launcher or a shell may start it from anywhere.
    {
        std::error_code ec;
        const std::filesystem::path exe = bodplat::ExeFolder();
        if (access(exe.c_str(), W_OK) == 0) std::filesystem::current_path(exe, ec);
    }
    const std::string dir = std::filesystem::current_path().string() + "/";
#else
    const std::string dir = std::filesystem::current_path().string() + "\\";
#endif
    // Its game folder (BLUEWAKE_RELS_DIR's parent): game\ beside the program, where game\rando\ holds the
    // randomizer's disc and its logo (tools/rando/install_dev.py).
    if (!std::getenv("BLUEWAKE_RELS_DIR")) {
#if defined(_WIN32)
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::filesystem::path game = std::filesystem::path(std::wstring(exe, n)).parent_path() / "game" / "rels";
#else
        const std::filesystem::path game = bodplat::ExeFolder() / "game" / "rels";
#endif
        setEnv("BLUEWAKE_RELS_DIR", game.string());
    }
    // Its data folder is the working one (settings, Save\): the seed generator writes Randomizer\ there too, where the
    // file select reads it (it otherwise writes beside the program).
    if (!std::getenv("BLUEWAKE_DATA_DIR")) setEnv("BLUEWAKE_DATA_DIR", dir);
    // Its card path (BLUEWAKE_CARD_PATH, its launcher's Save\GZLE01.card): this runtime's card (src/runtime/exi.cpp,
    // SA2_CARD or saves\MemoryCardA.raw). The randomizer keeps which files are a seed's beside it (rando_flow.cpp:
    // MemoryCardA.slots.json), so they stay randomizer files from one start to the next.
    if (!std::getenv("BLUEWAKE_CARD_PATH")) {
        const char* card = std::getenv("SA2_CARD");
        const std::filesystem::path path = std::filesystem::current_path() / (card ? card : "saves/MemoryCardA.raw");
        setEnv("BLUEWAKE_CARD_PATH", path.string());
    }
    bw_settings_load(dir.c_str());
}

bool hostInstalled() { return s_installed; }

// Launch recovery (Boat of Daphnes' win_entry.c): a marker written as the game starts and removed once it has run a while
// (win_settings: 600 healthy frames) or quits cleanly. A start that finds it still there - the last one died starting -
// comes up in safe mode (bw_settings_load: no mods, no options, HLE audio).
namespace {
std::string launchMarker() { return (std::filesystem::current_path() / "launch.pending").string(); }
}
void launchBegin() {
    if (!bw_launch_begin(launchMarker().c_str())) std::fprintf(stderr, "[safe-mode] could not create the launch marker\n");
}

void hostShutdown() {
    bw_launch_clear(launchMarker().c_str());
    if (bw_settings_relaunch() != 0) std::fprintf(stderr, "[bod] the restart could not start the game again\n");
}

void hostInstall() {
    bw_settings_apply_launch();
    bw_settings_install();
    s_installed = true;
    bluewake_mods_disc_ready();  // the disc is open (its file table in memory): mods may serve their files
}

void countShown();
void drawFpsOverlay();
void hostOverlay() {
    countShown();
    drawFpsOverlay();
    if (s_overlay) s_overlay(s_overlayUser);
}

namespace {
// SDL keys to the Windows virtual keys bw_settings_key takes (Boat of Daphnes' game-thread hook).
unsigned virtualKey(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + unsigned(sc - SDL_SCANCODE_F1);  // VK_F1..
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x0D;
    default: return 0;
    }
}
}  // namespace

void hostEvent(const void* e) {
    if (!s_installed) return;
    const SDL_Event& ev = *static_cast<const SDL_Event*>(e);
    if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat)
        if (const unsigned vk = virtualKey(ev.key.scancode)) bw_settings_key(vk, (ev.key.mod & SDL_KMOD_ALT) ? 1 : 0);
    bluewake_settings_menu_event(e);
}

bool menuOpen() { return s_installed && bw_settings_menu_is_open(); }

}  // namespace rt::bod

using namespace rt;

// ---- Boat of Daphnes' Aurora fork ----
namespace rt::bod {
void setOverlay(void (*fn)(void*), void* user) {
    s_overlayUser = user;
    s_overlay = fn;
}
}  // namespace rt::bod


// ---- what Boat of Daphnes' Aurora fork keeps: render settings and counters (bod_api.cpp sets them) ----
namespace rt::bod {
std::atomic<bool> g_interp{false}, g_fpsOverlay{false};
std::atomic<int> g_interpSteps{1};
std::atomic<float> g_bufferScale{0.f}, g_bufferAspect{0.f}, g_masterVolume{1.f};
std::atomic<int> g_integerScale{0};
std::atomic<unsigned> g_anisotropy{1};
std::atomic<unsigned long long> g_shown{0};
std::atomic<float> g_gameFps{0.f};
std::atomic<float> g_blendT{1.f};
std::atomic<bool> g_fastForward{false}, g_presentSuppressed{false};
std::atomic<float> s_shownFps{0.f};

bool interpolationWanted() { return g_interp; }

// Each frame drawn: the frame counter, and the game's frame rate once a second.
void countShown() {
    g_shown++;
    static auto s_at = std::chrono::steady_clock::now();
    static u64 s_frames = gxFrames();
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - s_at).count();
    static u64 s_shownAt = 0;
    if (dt >= 1.0) {
        const u64 f = gxFrames();
        g_gameFps = float(double(f - s_frames) / dt);
        s_shownFps = float(double(g_shown - s_shownAt) / dt);
        s_frames = f, s_at = now, s_shownAt = g_shown;
    }
}

// Its Show FPS (its Aurora fork's draw_fps_overlay): every present, and the game's own frames under it when in-between
// frames make the two differ, at the top centre (the corners hold the game's HUD).
void drawFpsOverlay() {
    if (!g_fpsOverlay) return;
    const float shown = s_shownFps, game = g_gameFps;
    ImGui::SetNextWindowPos(ImVec2{ImGui::GetIO().DisplaySize.x * 0.5f, 12.f}, ImGuiCond_Always, ImVec2{0.5f, 0.f});
    ImGui::SetNextWindowBgAlpha(0.6f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##aurora-fps", nullptr, flags)) {
        ImGui::SetWindowFontScale(1.5f);
        if (game > 0.f && shown > game + 5.f) ImGui::Text("%.0f FPS (game %.0f)", shown, game);
        else ImGui::Text("%.0f FPS", shown);
    }
    ImGui::End();
}
}  // namespace rt::bod

// ---- its host code on this runtime: retraces, intercepts ----
#include "runtime/interp.h"

namespace rt {
u64 viRetraces();  // src/runtime/hle_hw.cpp
}

namespace rt::bod {
unsigned char* guestRam() { return hostp(0x80000000u); }
unsigned long long retraceCount() { return viRetraces(); }

namespace {
Regs regsOf(Ctx& c) { return Regs{c.r, c.f, c.ps1, &c.lr, &c.ctr, c.cr}; }
thread_local Ctx* t_hookCtx = nullptr;  // the context a hook into its code runs on (callGuest's)
// Its intercepts at function entries (its feature_dispatch: Quick Doors at dStage_actorCreate,
// Forest Water at resetTimer and setEventReg): the function's own code then runs.
u32 s_featureWord = 0;
u32 featureWord() {
    if (!s_featureWord)
        s_featureWord = interpHookWord([](Ctx& c, u32 pc) -> u32 {
            apiFeatureEnter(regsOf(c), pc);
            return kInterpOriginal;
        });
    return s_featureWord;
}
void hookAt(u32 address) {
    interpPatch(address, featureWord());
    interpOverrideAt(address);
}
u32 s_treeCheck = 0;  // d_a_obj_ftree's timer check, while linked
}  // namespace

void hostAttach() {
    apiAttach();
    for (u32 a : {0x80041628u, 0x8005987Cu, 0x8005CB58u}) hookAt(a);
    // Its J2DPane::draw hook (after PSMTXConcat, 0x802D02B8): HUD button and file-select colours.
    const u32 pane = interpHookWord([](Ctx& c, u32 pc) -> u32 {
        const u32 r = apiPaneHook(regsOf(c), pc);
        return r ? r : kInterpOriginal;
    });
    interpPatch(0x802D02B8, pane);
    interpOverrideAt(0x802D02B8);
}

void hostRetrace() {
    static bool s_attached = false;
    if (!s_attached) s_attached = true, hostAttach();
    apiRetrace();
    randoRetrace();
    gbaRetrace();
}

void hostLinked(u32 module) {
    randoLinked(module);
    if (module == 317 && g_relBase[317][1]) {  // d_a_obj_ftree: Forest Water's tree timer check
        apiForestWaterLinked(g_relBase[317][1]);
        if ((s_treeCheck = apiForestWaterTreeCheck())) hookAt(s_treeCheck);
    }
}
void hostUnlinked(u32 module) {
    randoUnlinked(module);
    if (module == 317 && s_treeCheck) interpUnpatch(s_treeCheck), s_treeCheck = 0;
}

unsigned treasureGlowHook(Ctx& c, u32 pc) { return apiTreasureGlow(regsOf(c), pc); }

// A guest function called from its host code in a hook (its guest_coro bodies): on the hook's context, the link register
// kept.
void callGuest(unsigned function) {
    if (!t_hookCtx) return;
    const u32 lr = t_hookCtx->lr;
    t_hookCtx->call(function);
    t_hookCtx->lr = lr;
}

// Its hooks 30 and 31 (online players' Link models: puppet.c).
unsigned hostHook(Ctx& c, unsigned id, u32 pc) {
    Ctx* const outer = t_hookCtx;
    t_hookCtx = &c;
    const unsigned r = apiHostHook(regsOf(c), id, pc);
    t_hookCtx = outer;
    return r;
}
}  // namespace rt::bod
