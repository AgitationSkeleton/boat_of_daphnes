// The settings and the in-game menu (Esc or F1, or a controller's Back), the
// desktop hotkeys (F11 and Alt+Enter fullscreen, F9 the frame rate, F7 the item tracker, F4 the check tracker,
// F6/F8 save states when enabled) and the window's placement.
//
// Settings are CVars (runtime/host/src/ship/cvar.h), saved in
// boat-of-daphnes.json beside the executable, with Ship of Harkinian's
// prefixes: gSettings.*, gEnhancements.*, gCheats.*, gOpenWindows.*. The menu
// is ship_menu.h, laid out as SoH's port menu: Settings, Enhancements and Dev
// Tools, each a sidebar of pages. Every setting applies at once, while the
// game runs (the Audio Mode, texture packs and Precompile Shaders too).
//
// At launch the settings that the host reads once become its variables
// (BLUEWAKE_*, DOL_*) unless the command line already set one, so the command
// line wins for that session.
// The Windows build force-includes a POSIX layer whose rename() macro breaks <filesystem> (aurora/texture.hpp).
#ifdef rename
#undef rename
#endif

#include "win_settings.h"
#include "win_crash.h"
#include "game_state.h"
#include "controller_face_swap.h"
#include "restart_request.h"
#include "rando_flow.h"
#include "drop_path.h"
#include "launch_marker.h"
#include "ship_menu.h"
#include "cosmetics_editor.h"
#include "anchor.h"
#include "mods.h"
#include "gb_browser.h"
#include "hud_colors.h"
#include "notifications.h"
#include "randomizer_menu.h"
#include "item_tracker.h"
#include "check_tracker.h"
#include "input_viewer.h"
#include "ship/cvar.h"

#include "platform/bod_platform.h"

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <aurora/imgui.h>
#include <aurora/texture.hpp>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include "input_editor.h"
#include "dev_windows.h"
#include "adapter_driver.h"
#include "gc_adapter.h"
#include "gba_window.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gxruntime/aurora_backend.h"
#include "save_state.h"

extern "C" {
// Host functions declared here with plain types: their headers bring in the
// guest CPU's, which this file has no use for.
void bluewake_haptics_reload(void);
void bluewake_forest_water_reload(void);
void bluewake_haptics_block(bool blocked);
const char* bluewake_game_options_describe(uint32_t position, const char** title, bool* default_on, bool* on);
bool bluewake_game_options_set(const char* name, bool on);
bool bluewake_game_options_has(const char* name);
bool bluewake_host_reset_requested(void); // game_options.c: the game asked for a console reset
void bluewake_treasure_glow_set(int mode);
void bluewake_intro_skip_set(bool on);
void bluewake_pref_switch_targeting_set(bool on);
void bluewake_pref_remove_videos_set(bool on);
void bluewake_rando_pref_set(const char* option, bool on);
void bluewake_dsp_request_mode(int lle);
void bluewake_fast_load_reload(void);
void bluewake_quick_doors_reload(void);
void dol_aurora_audio_set_master_volume(float volume);
void bluewake_audio_volume_set(int music_percent, int sfx_percent, int fanfare_percent);
void bluewake_autosave_configure(int interval_minutes, bool on_new_item);
bool bluewake_autosave_take_done(int* slot, bool* ok);
void bluewake_text_speed_set(int speed);
}

// Aurora's frame counters (lib/gfx/common.hpp, linked in statically), for the
// session log and Dev Tools: every present, and the game's own frames.
extern "C" void bluewake_bunny_hood_enable(bool on);
extern "C" void bluewake_chart_reveal_enable(bool on); // runtime/host/src/chart_reveal.h
extern "C" void bluewake_text_swap_enable(bool on);
extern "C" void bluewake_text_swap_set_female(bool on);
extern "C" void bluewake_host_set_pad_unfocused_block(bool block);

namespace aurora::gfx {
float calculate_fps() noexcept;
float calculate_game_fps() noexcept;
}  // namespace aurora::gfx

#define CVAR_SETTING(name) "gSettings." name
#define CVAR_ENHANCEMENT(name) "gEnhancements." name
#define CVAR_CHEAT(name) "gCheats." name
#define CVAR_WINDOW(name) "gOpenWindows." name

namespace {

using shipmenu::WidgetInfo;
using shipmenu::WidgetType;

std::string g_data_dir;
RestartRequest g_restart;
bool g_safe_mode;
bool g_menu_open;
bool g_text_capture;  // the chat line over the game is open: typing is not the game's
bool g_toggle_menu, g_toggle_fullscreen;  // from the hotkeys, done in the frame
Uint64 g_first_frame_at;
bool g_placed;
float g_font_scale = 1.0f;  // the scale the UI font was drawn at (see load_font)
shipmenu::Menu g_menu;
bool g_menu_built;
void sync_live_textures();
void start_shader_warmup();

constexpr int kAnisotropy[] = {1, 2, 4, 8, 16};
// Force Aspect Ratio: Off, then the presets; the last (6) is the custom X:Y.
constexpr float kAspects[] = {0.0f, 4.0f / 3.0f, 1.6f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f};
constexpr int kAspectCustom = 6;
// Pixel Count Presets: the picture's height in pixels.
constexpr int kPixelPresets[] = {480, 960, 720, 1080, 1440, 2160};

// Better Wind Waker's options (mods/betterww/options.txt): where each goes in
// the menu, as SoH would label it.
struct BetterWwRow {
    const char* name;
    const char* sidebar;
    int column;
    const char* section;
    const char* label;
    const char* tooltip;
    int def = 0;       // the setting's value when the file has none
};
const BetterWwRow kBetterWw[] = {
    {"tingle_chests", "Quality of Life", 0, "Convenience", "Tingle Chests Without Tuner",
     "Allows Tingle Chests to be opened with normal Bombs, without the Tingle Tuner."},
    {"skip_intro_movie", "Skips & Speed-ups", 0, "Cutscenes", "Skip Opening Movie",
     "Skips the story told when a new game starts (and the one on the title screen): the game goes straight to "
     "Outset Island."},
    {"no_song_replays", "Skips & Speed-ups", 0, "Cutscenes", "Skip Song Replays",
     "Skips the replay of a song after it is played with the Wind Waker."},
    {"instant_text", "Skips & Speed-ups", 0, "Text", "Instant Text",
     "Makes text boxes appear at once, and lets holding B advance text as soon as it can."},
    {"faster_movement", "Skips & Speed-ups", 1, "Animations", "Faster Rolling",
     "Makes Link's roll faster, as in Wind Waker HD."},
    {"faster_block_push", "Skips & Speed-ups", 1, "Animations", "Faster Block Pushing",
     "Speeds up pushing and pulling blocks."},
    {"faster_animations", "Skips & Speed-ups", 1, "Animations", "Faster Crawling and Climbing",
     "Speeds up crawling, climbing ladders and vines, and the camera zoom when talking to characters."},
    {"faster_ballad", "Skips & Speed-ups", 1, "Animations", "Faster Ballad of Gales",
     "Speeds up the Ballad of Gales warp."},
    {"swift_sail", "Items", 0, "Sailing", "Swift Sail",
     "Makes the Sail twice as fast, red, and turns the wind behind the boat whenever the Sail is up, as in "
     "Wind Waker HD. Red sail textures from the Wind Waker Randomizer."},
    {"brisk_sail", "Items", 0, "Sailing", "Brisk Sail",
     "Swift Sail with harder braking. Turns Swift Sail on as well."},
    {"unrestricted_boat", "Items", 0, "Sailing", "Unrestricted Boat",
     "Allows the boat to sail into places it normally cannot."},
    {"faster_grapple", "Items", 1, "Grappling Hook", "Faster Grappling Hook",
     "Speeds up throwing and pulling in the Grappling Hook."},
    {"swing_turn", "Items", 1, "Grappling Hook", "Turn While Swinging",
     "Allows turning with the Control Stick while swinging on the Grappling Hook."},
    // From wwrando (https://github.com/LagoLunatic/wwrando, MIT) and Better Wind Waker.
    {"hero_mode", "Quality of Life", 0, "Difficulty", "Hero Mode",
     "You take double damage, and hearts no longer drop from enemies, pots or grass. From Better Wind Waker's "
     "Gecko codes, built as wwrando's Hero Mode patch.", 0},
    {"no_low_hp_beep", "Quality of Life", 0, "Difficulty", "No Low-Health Beep",
     "Stops the low-health beep with a one-heart maximum, and the low-health animation until two quarter hearts "
     "are missing. From wwrando.", 0},
    {"fix_vanilla_bugs", "Quality of Life", 1, "Fixes", "Fix Vanilla Bugs",
     "Fixes bugs of the original game, as wwrando does: Grandma's soup message crash, the Helmaroc King mask "
     "shards Hookshot crash, Orca without a sword, Beedle buying Blue Chu Jelly, the Deluxe Picto Box not "
     "equipping, Morths falling out of bounds, light arrows on Stalfos, Miniblins, Jalhalla's Poes and Magtails, "
     "Phantom Ganon's trigger, the Skull Hammer guard, the miniboss music that kept playing, and the arrow "
     "crash.", 1},
    {"blur_off", "Graphics", 0, "Camera", "Blur Off",
     "Removes the distance blur. From Better Wind Waker's Gecko codes (Dolphin's Remove Distance Blur).", 0},
    {"unrestricted_camera", "Graphics", 0, "Camera", "Unrestricted Camera",
     "Lets the C-Stick camera zoom out farther and tilt higher. From Better Wind Waker's Gecko codes.", 0},
};

std::string better_ww_cvar(const char* name) { return std::string(CVAR_ENHANCEMENT("BetterWW.")) + name; }

void changed() { CVarSaveNextFrame(); }

// --- the launch -------------------------------------------------------------

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0';
}

void env_default(const char* name, const std::string& value) {
    if (!env_set(name))
        bodplat::SetEnv(name, value.c_str());
}

// A window a good size for this screen: the tallest multiple of 240 lines that
// fits in 80 percent of the primary screen's work area, at 4:3.
void default_window(int* w, int* h) {
    int work_w = 1280, work_h = 1024;
    bodplat::PrimaryWorkArea(&work_w, &work_h);
    int height = std::max(480, static_cast<int>(work_h * 0.8) / 240 * 240);
    int width = height * 4 / 3;
    if (width > work_w * 9 / 10) {
        width = work_w * 9 / 10;
        height = width * 3 / 4;
    }
    *w = width;
    *h = height;
}

// BLUEWAKE_TEXTURES_DIR (testing): another folder of texture packs.
std::string texture_folder() {
    if (const char* dir = std::getenv("BLUEWAKE_TEXTURES_DIR"); dir != nullptr && dir[0] != '\0')
        return dir;
    return g_data_dir + "mods" BOD_SEP "textures";
}

// The executable's folder, ending in a separator.
std::string exe_folder() {
    const auto folder = bodplat::ExeFolder().u8string();
    if (folder.empty())
        return g_data_dir;
    return std::string(folder.begin(), folder.end()) + BOD_SEP;
}

// Internal Resolution is a percentage of the window's pixels, as in SoH. The
// renderer takes a multiple of the game's 480 lines (0: the window's own).
float render_scale_for(int pixel_height) {
    // Set Fixed Vertical Resolution: that many lines whatever the window.
    if (CVarGetInteger(CVAR_SETTING("Graphics.FixedVerticalResolution"), 0))
        return static_cast<float>(std::clamp(CVarGetInteger(CVAR_SETTING("Graphics.VerticalResolution"), 480), 240, 4320)) /
               480.0f;
    // Pixel Perfect Mode: the game's own 480 lines, shown at a whole multiple.
    if (CVarGetInteger(CVAR_SETTING("Graphics.PixelPerfect"), 0))
        return 1.0f;
    const int percent = CVarGetInteger(CVAR_SETTING("Graphics.InternalResolution"), 100);
    if (percent == 100 || pixel_height <= 0)
        return 0.0f;
    return static_cast<float>(pixel_height) * static_cast<float>(percent) / 100.0f / 480.0f;
}

// --- the window -------------------------------------------------------------

SDL_Window* game_window() {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* window = windows != nullptr && count > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    return window;
}

bool is_fullscreen(SDL_Window* w) { return (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0; }

void set_fullscreen(SDL_Window* w, bool on) {
    if (w == nullptr)
        return;
    SDL_SetWindowFullscreen(w, on);
    CVarSetInteger(CVAR_SETTING("Graphics.Fullscreen"), on ? 1 : 0);
    changed();
    std::fprintf(stderr, "[windows] fullscreen %s\n", on ? "on" : "off");
}

// Once the window exists: where it was last time, if that is still on a
// screen with its title bar showing, else centred.
void place_window(SDL_Window* w) {
    if (is_fullscreen(w))
        return;
    if (std::getenv("BLUEWAKE_WINDOW_POS") != nullptr)
        return; // a test run's place (window.cpp)
    const int x = CVarGetInteger(CVAR_SETTING("Window.X"), INT_MIN);
    const int y = CVarGetInteger(CVAR_SETTING("Window.Y"), INT_MIN);
    bool restored = false;
    if (x != INT_MIN && y != INT_MIN) {
        const SDL_Point title{x + 60, y - 16};
        const SDL_Point corner{x + 60, y + 60};
        if (SDL_GetDisplayForPoint(&title) != 0 && SDL_GetDisplayForPoint(&corner) != 0) {
            SDL_SetWindowPosition(w, x, y);
            restored = true;
        }
    }
    if (!restored)
        SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

// Remember the window's place and size as the player leaves them.
void track_window(SDL_Window* w) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(w);
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED))
        return;
    int x = 0, y = 0, width = 0, height = 0;
    if (!SDL_GetWindowPosition(w, &x, &y) || !SDL_GetWindowSize(w, &width, &height))
        return;
    if (x != CVarGetInteger(CVAR_SETTING("Window.X"), INT_MIN) || y != CVarGetInteger(CVAR_SETTING("Window.Y"), INT_MIN) ||
        width != CVarGetInteger(CVAR_SETTING("Window.Width"), 0) ||
        height != CVarGetInteger(CVAR_SETTING("Window.Height"), 0)) {
        CVarSetInteger(CVAR_SETTING("Window.X"), x);
        CVarSetInteger(CVAR_SETTING("Window.Y"), y);
        CVarSetInteger(CVAR_SETTING("Window.Width"), width);
        CVarSetInteger(CVAR_SETTING("Window.Height"), height);
        changed();
    }
}

void reset_window(SDL_Window* w) {
    if (w == nullptr)
        return;
    if (is_fullscreen(w))
        set_fullscreen(w, false);
    int width = 0, height = 0;
    default_window(&width, &height);
    SDL_SetWindowSize(w, width, height);
    SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

// --- settings that apply at once ------------------------------------------------

// The camera inversion on every port's mapping (input_editor.cpp).
void apply_controller() { input_editor::ApplyCamera(); }

const char* rumble_name(int mode) { return mode == 0 ? "off" : mode == 1 ? "classic" : "enhanced"; }

void apply_rumble() {
    bodplat::SetEnv("BLUEWAKE_HAPTICS", rumble_name(CVarGetInteger(CVAR_SETTING("Controls.Rumble.Mode"), 1)));
    bodplat::SetEnv("BLUEWAKE_HAPTICS_STRENGTH",
              std::to_string(CVarGetInteger(CVAR_SETTING("Controls.Rumble.Strength"), 80)).c_str());
    bodplat::SetEnv("BLUEWAKE_HAPTICS_TRIGGERS", CVarGetInteger(CVAR_SETTING("Controls.Rumble.Triggers"), 1) ? "1" : "0");
    bluewake_haptics_reload();
}

void apply_forest_water() {
    bodplat::SetEnv("BLUEWAKE_FOREST_WATER_KEEP_TREES", CVarGetInteger(CVAR_ENHANCEMENT("KeepWateredTrees"), 0) ? "1" : "0");
    bodplat::SetEnv("BLUEWAKE_FOREST_WATER_30_MINUTES",
              CVarGetInteger(CVAR_ENHANCEMENT("LongerForestWaterTimer"), 0) ? "1" : "0");
    bluewake_forest_water_reload();
}

void apply_resolution(SDL_Window* w) {
    int pw = 0, ph = 0;
    if (w != nullptr)
        SDL_GetWindowSizeInPixels(w, &pw, &ph);
    aurora_set_frame_buffer_scale(render_scale_for(ph));
}

// Interpolation FPS (SoH's Settings > Graphics > Current FPS): the game runs at
// 30 frames a second; above 30, frames are made between its frames (Aurora's
// frame interpolation, n in-between frames for 30 * (n + 1)). 30 is off.
void apply_interpolation() {
    const int fps = std::clamp(CVarGetInteger(CVAR_SETTING("Graphics.InterpolationFPS"), 30), 30, 240);
    const int steps = fps / 30 - 1;
    if (steps > 0)
        aurora_set_frame_interp_steps(steps);
    aurora_set_frame_interpolation(steps > 0);
}

// The PC's hardware, read at launch: written to the session log every time, and on the first launch (no Interpolation
// FPS saved yet) it picks Interpolation FPS. Frames between the game's run on worker threads beside the game thread,
// so a 4-core laptop at 120 starved the game itself (a friend's log, 2026-10-12).
struct Hardware {
    int cores = 0, threads = 0, ram_gb = 0, hz = 60;
    std::string gpus;
};

Hardware read_hardware(SDL_Window* w) {
    Hardware hw;
    const bodplat::CpuCounts cpu = bodplat::Cpu();
    hw.cores = cpu.cores;
    hw.threads = cpu.threads;
    if (hw.cores == 0)
        hw.cores = hw.threads = static_cast<int>(std::thread::hardware_concurrency());
    if (const uint64_t memory = bodplat::TotalMemoryBytes())
        hw.ram_gb = static_cast<int>((memory + (512ull << 20)) >> 30);
    if (w != nullptr)
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)))
            if (mode->refresh_rate > 0.f)
                hw.hz = static_cast<int>(mode->refresh_rate + 0.5f);
    hw.gpus = bodplat::GpuNames();
    return hw;
}

// 6+ cores: the display's rate up to 120; 4-5 cores: up to 60; fewer: 30 (off).
int recommended_interpolation(const Hardware& hw) {
    const int cap = hw.cores >= 6 ? 120 : hw.cores >= 4 ? 60 : 30;
    return std::clamp(std::min(hw.hz, cap) / 30 * 30, 30, 240);
}

void profile_hardware(SDL_Window* w, bool apply) {
    const Hardware hw = read_hardware(w);
    const int rec = recommended_interpolation(hw);
    const char* key = CVAR_SETTING("Graphics.InterpolationFPS");
    // Automated runs (scenarios, the release check) keep the default, so their timing stays comparable.
    const bool first = !CVarExists(key) && std::getenv("BLUEWAKE_NO_DIALOG") == nullptr;
    std::fprintf(stderr, "[hw] %d cores / %d threads, %d GB RAM, display %d Hz, GPU %s; recommended Interpolation FPS %d%s\n",
                 hw.cores, hw.threads, hw.ram_gb, hw.hz, hw.gpus.c_str(), rec,
                 first ? " (first launch: set)" : "");
    if (!first && !apply) {
        const int fps = CVarGetInteger(key, 30);
        if (fps > rec)
            std::fprintf(stderr, "[hw] Interpolation FPS %d is above the recommended %d for this PC\n", fps, rec);
        return;
    }
    CVarSetInteger(key, rec);
    CVarSaveNextFrame();
    notifications::Options o;
    o.prefix = "Graphics";
    o.message = "Interpolation FPS set to " + std::to_string(rec) + " for this PC (Settings > Graphics).";
    notifications::Emit(std::move(o));
}

// The aspect the picture is held to (0: it fills the window). Widescreen Off
// is the game's own 4:3; otherwise Force Aspect Ratio decides.
float picture_aspect() {
    if (!CVarGetInteger(CVAR_SETTING("Graphics.Widescreen"), 1))
        return 4.0f / 3.0f;
    const int aspect = std::clamp(CVarGetInteger(CVAR_SETTING("Graphics.ForceAspectRatio"), 0), 0, kAspectCustom);
    if (aspect == kAspectCustom) {
        const int x = std::clamp(CVarGetInteger(CVAR_SETTING("Graphics.AspectRatioX"), 16), 1, 64);
        const int y = std::clamp(CVarGetInteger(CVAR_SETTING("Graphics.AspectRatioY"), 9), 1, 64);
        return static_cast<float>(x) / static_cast<float>(y);
    }
    return kAspects[aspect];
}

void apply_graphics(SDL_Window* w) {
    apply_resolution(w);
    apply_interpolation();
    const int aniso = CVarGetInteger(CVAR_SETTING("Graphics.Anisotropy"), 0);
    aurora_set_forced_anisotropy(static_cast<unsigned>(kAnisotropy[std::clamp(aniso, 0, 4)]));
    aurora_enable_vsync(CVarGetInteger(CVAR_SETTING("Graphics.Vsync"), 1) != 0);
    aurora_set_fps_overlay(CVarGetInteger(CVAR_SETTING("Graphics.ShowFPS"), 0) != 0);
    aurora_set_frame_buffer_aspect(picture_aspect());
    aurora_set_integer_scale(!CVarGetInteger(CVAR_SETTING("Graphics.PixelPerfect"), 0)           ? 0
                             : CVarGetInteger(CVAR_SETTING("Graphics.AutoIntegerScale"), 1) != 0 ? -1
                             : std::clamp(CVarGetInteger(CVAR_SETTING("Graphics.IntegerScaleFactor"), 2), 1, 10));
    aurora_set_resampler(CVarGetInteger(CVAR_SETTING("Graphics.OutputResampling"), 1) ? SAMPLER_AREA : SAMPLER_BILINEAR);
}

void apply_audio() {
    dol_aurora_audio_set_master_volume(
        static_cast<float>(std::clamp(CVarGetInteger(CVAR_SETTING("Audio.MasterVolume"), 100), 0, 100)) / 100.0f);
    // The three category volumes are made in the game's own audio (runtime/host/src/audio_volume.c).
    bluewake_audio_volume_set(CVarGetInteger(CVAR_SETTING("Audio.MusicVolume"), 100),
                              CVarGetInteger(CVAR_SETTING("Audio.SfxVolume"), 100),
                              CVarGetInteger(CVAR_SETTING("Audio.FanfareVolume"), 100));
}

// Enhancements > Autosave: the host saves the way the pause menu does (runtime/host/src/autosave.c).
void apply_autosave() {
    bluewake_autosave_configure(std::clamp(CVarGetInteger(CVAR_ENHANCEMENT("Autosave.Minutes"), 0), 0, 30),
                                CVarGetInteger(CVAR_ENHANCEMENT("Autosave.OnNewItem"), 0) != 0);
}

// Once a frame: a save the host has finished is a toast (and, in a randomizer session, the file's seed noted).
void poll_autosave() {
    int slot = 0;
    bool ok = false;
    while (bluewake_autosave_take_done(&slot, &ok)) {
        notifications::Options o;
        o.prefix = "Autosave";
        o.prefixColor = ok ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.5f, 0.5f, 1.0f);
        o.message = ok ? "Saved" : "Could not save";
        o.suffix = "file " + std::to_string(slot + 1);
        o.suffixColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        notifications::Emit(std::move(o));
        if (ok)
            randoflow::OnSaved(slot);
    }
}

// Faster Transitions and Quick Doors: the host's switches, read again.
void apply_fast_transitions() {
    const bool fast = CVarGetInteger(CVAR_ENHANCEMENT("FasterTransitions"), 0) != 0;
    bodplat::SetEnv("BLUEWAKE_FAST_FORWARD", fast ? "1" : "0");
    bodplat::SetEnv("BLUEWAKE_FADE_FRAMES", fast ? "6" : "0");
    bluewake_fast_load_reload();
    std::fprintf(stderr, "[settings] faster transitions %s\n", fast ? "on" : "off");
}

void apply_quick_doors() {
    const bool on = CVarGetInteger(CVAR_ENHANCEMENT("QuickDoors"), 0) != 0;
    bodplat::SetEnv("BLUEWAKE_QUICK_DOORS", on ? "1" : "0");
    bluewake_quick_doors_reload();
    std::fprintf(stderr, "[settings] quick doors %s\n", on ? "on" : "off");
}

// A Better Wind Waker switch that works on the running game (its instructions
// and values are switched in the translated code).
bool better_ww_wanted(const char* name) {
    int def = 0;
    for (const auto& row : kBetterWw)
        if (std::strcmp(row.name, name) == 0)
            def = row.def;
    return CVarGetInteger(better_ww_cvar(name).c_str(), def) != 0;
}

bool is_sail_option(const char* name) {
    return std::strcmp(name, "swift_sail") == 0 || std::strcmp(name, "brisk_sail") == 0;
}

// Brisk Sail is Swift Sail with harder braking, so it needs Swift Sail's sites:
// they run when either is chosen (the host does the same at launch). Their
// red textures follow in sync_live_textures.
void apply_sail_options() {
    const bool brisk = better_ww_wanted("brisk_sail");
    bluewake_game_options_set("brisk_sail", brisk);
    bluewake_game_options_set("swift_sail", brisk || better_ww_wanted("swift_sail"));
}

void apply_better_ww(const char* name) {
    if (is_sail_option(name))
        apply_sail_options();
    else
        bluewake_game_options_set(name, better_ww_wanted(name));
}

// The player's preferences that used to sit in the Randomizer page: they follow the settings in every session (vanilla and
// randomizer), the seed's own values are put aside (runtime/host/src/rando_host.c).
void apply_preferences() {
    bluewake_pref_switch_targeting_set(CVarGetInteger(CVAR_SETTING("Controls.SwitchTargeting"), 0) != 0);
    bluewake_rando_pref_set("switch_targeting_mode", CVarGetInteger(CVAR_SETTING("Controls.SwitchTargeting"), 0) != 0);
    bluewake_pref_remove_videos_set(CVarGetInteger(CVAR_ENHANCEMENT("RemoveTitleAndEndingVideos"), 0) != 0);
    bluewake_rando_pref_set("invert_sea_compass_x_axis", CVarGetInteger(CVAR_ENHANCEMENT("InvertSeaCompassX"), 0) != 0);
    // Music is a volume now (Audio > Music Volume, 0 = none), made in the game's audio at any moment. The seed's own
    // wwrando "remove music" patch is therefore kept off in every session: the player's slider decides.
    bluewake_rando_pref_set("remove_music", false);
    bluewake_game_options_set("invert_sea_compass_x", CVarGetInteger(CVAR_ENHANCEMENT("InvertSeaCompassX"), 0) != 0);
}

// Tooltips of the two preferences whose code sites a vanilla session needs a module built with them for.
void module_tooltip(WidgetInfo& wi, const char* text, const char* option) {
    wi.tooltip = std::string(text) + "\n\n" + (bluewake_game_options_has(option)
        ? "Works in vanilla and randomizer sessions."
        : "Randomizer sessions only: this game module has no switch for it yet.");
}

// Reveal Sea Chart: the open chart is drawn as if every square were discovered; the save is not touched.
void apply_reveal_sea_chart() {
    bluewake_chart_reveal_enable(CVarGetInteger(CVAR_ENHANCEMENT("RevealSeaChart"), 0) != 0);
}

void apply_port_options() {
    apply_preferences();
    apply_reveal_sea_chart();
    for (const auto& row : kBetterWw)
        if (!is_sail_option(row.name))
            bluewake_game_options_set(row.name, CVarGetInteger(better_ww_cvar(row.name).c_str(), row.def) != 0);
    apply_sail_options();
    apply_autosave();
    bluewake_text_speed_set(std::clamp(CVarGetInteger(CVAR_ENHANCEMENT("TextSpeed"), 1), 1, 5));
    bluewake_game_options_set("widescreen_culling", CVarGetInteger(CVAR_ENHANCEMENT("WidescreenActorCulling"), 1) != 0);
    bluewake_game_options_set("hud_edges", CVarGetInteger(CVAR_ENHANCEMENT("HudAtScreenEdges"), 1) != 0);
}

void apply_live(SDL_Window* w) {
    apply_graphics(w);
    apply_audio();
    aurora_set_pause_on_focus_lost(CVarGetInteger(CVAR_SETTING("PauseOnFocusLost"), 0) != 0);
    aurora_set_background_input(CVarGetInteger(CVAR_SETTING("AllowBackgroundInputs"), 1) != 0);
    apply_controller();
    apply_rumble();
    apply_forest_water();
    apply_port_options();
    bluewake_text_swap_enable(CVarGetInteger(CVAR_ENHANCEMENT("Crenando"), 0) != 0);
    cosmetics::ApplyAll();
}

void set_menu_open(bool open) {
    if (open == g_menu_open)
        return;
    g_menu_open = open;
    PADBlockInput(open || g_text_capture);
    // The menu's keys and clicks are not the game's.
    input_editor::ApplyKeyboard(open || g_text_capture);
    bluewake_haptics_block(open);
    std::fprintf(stderr, "[windows] settings menu %s\n", open ? "open" : "closed");
    if (!open)
        CVarSave();
}

void open_folder(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path), ec);
    bodplat::OpenPath(std::filesystem::u8path(path));
}

void quit() {
    CVarSave();
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

// The menu's Restart, as Ship of Harkinian's: back to the title screen with the game's own fade, the game's soft
// reset (holding B+X+Start: JUTGamePad's three-button reset, then dComIfG_resetToOpening), the process kept. Before
// the game has a scene (the boot) the process is started again instead.
extern "C" void bluewake_ui_pad_inject(uint16_t buttons, unsigned samples); // ui_pad.h
void restart();
void soft_restart() {
    const BluewakeLocation here = bluewake_game_location();
    if (here.scene_proc == BLUEWAKE_SCENE_PLAY || here.scene_proc == BLUEWAKE_SCENE_NAME ||
        here.scene_proc == BLUEWAKE_SCENE_TITLE) {
        std::fprintf(stderr, "[windows] restart: the game's soft reset (B+X+Start)\n");
        set_menu_open(false);
        bluewake_ui_pad_inject(0x0200u | 0x0400u | 0x1000u, 90u); // B, X, Start, held well past the 0.5 s it needs
        return;
    }
    restart();
}

void restart() {
    CVarSave();
    bool ok = g_restart.request(true, [] {
        SDL_Event quit_event{};
        quit_event.type = SDL_EVENT_QUIT;
        return SDL_PushEvent(&quit_event);
    });
    if (!ok)
        std::fprintf(stderr, "[windows] restart refused: the quit request failed\n");
}

// Start the game again, with the command line and environment it began with,
// so the settings file decides what the new session is.
bool relaunch() { return bodplat::Relaunch(); }

// --- presets ------------------------------------------------------------------------
// Settings > Presets: a named set of the Enhancements. Applying one sets every
// setting it manages (the ones it does not list go to their defaults) and runs
// each widget's own callback, so what can change at once does. Their contents
// are listed in docs/fork/settings-menu.md.
const char* const kPresetNames[] = {"Default", "Vanilla Plus", "Enhanced", "Randomizer"};
const char* const kPresetInfo[] = {
    "Every enhancement off, as the original game plays.",
    "Quality of life that keeps the game's feel: Tingle Chests Without Tuner, Reveal Sea Chart, Keep "
    "Watered Trees, Longer Forest Water Timer, Treasure Light Up Close (Treasure).",
    "Vanilla Plus and the speed-ups: Instant Text, Faster Rolling, Faster Block Pushing, Faster Crawling and Climbing, "
    "Faster Ballad of Gales, Skip Song Replays, Skip Opening Movie, Faster Grappling Hook, Turn While Swinging, Faster "
    "Transitions, Quick Doors, Swift Sail.",
    "What suits a randomizer seed, nothing that changes where a place can be reached: the Enhanced speed-ups without "
    "Quick Doors, and with Reveal Sea Chart, Keep Watered Trees and Treasure Light Up Close (All). Tingle "
    "Chests Without Tuner and Unrestricted Boat stay off.",
};

std::vector<std::pair<std::string, int>> preset_values(int preset) {
    std::map<std::string, int> v;
    for (const auto& row : kBetterWw)
        v[better_ww_cvar(row.name)] = 0;
    v[CVAR_ENHANCEMENT("RevealSeaChart")] = 0;
    v[CVAR_ENHANCEMENT("FasterTransitions")] = 0;
    v[CVAR_ENHANCEMENT("QuickDoors")] = 0;
    v[CVAR_ENHANCEMENT("KeepWateredTrees")] = 0;
    v[CVAR_ENHANCEMENT("LongerForestWaterTimer")] = 0;
    v[CVAR_ENHANCEMENT("TreasureLightUpClose")] = 0;
    v[CVAR_ENHANCEMENT("SkipIntroWithStart")] = 1;
    auto on = [&](const char* name) { v[better_ww_cvar(name)] = 1; };
    auto flag = [&](const char* cvar, int value = 1) { v[cvar] = value; };
    if (preset >= 1) {
        flag(CVAR_ENHANCEMENT("KeepWateredTrees"));
        flag(CVAR_ENHANCEMENT("LongerForestWaterTimer"));
        flag(CVAR_ENHANCEMENT("TreasureLightUpClose"), preset == 3 ? 3 : 1);
        flag(CVAR_ENHANCEMENT("RevealSeaChart"));
        if (preset != 3)
            on("tingle_chests");
    }
    if (preset >= 2) {
        for (const char* name : {"instant_text", "faster_movement", "faster_block_push", "faster_animations",
                                 "faster_ballad", "no_song_replays", "skip_intro_movie", "faster_grapple", "swing_turn",
                                 "swift_sail"})
            on(name);
        flag(CVAR_ENHANCEMENT("FasterTransitions"));
        if (preset == 2)
            flag(CVAR_ENHANCEMENT("QuickDoors"));
    }
    return {v.begin(), v.end()};
}

void apply_preset(int preset) {
    preset = std::clamp(preset, 0, 3);
    for (const auto& [cvar, value] : preset_values(preset)) {
        CVarSetInteger(cvar.c_str(), value);
        g_menu.FireCallback(cvar);
    }
    changed();
    std::fprintf(stderr, "[settings] preset %s applied\n", kPresetNames[preset]);
}

// Settings > Controls > GameCube Adapter: the official Wii U adapter (USB
// 057E:0337) as Windows sees it. SDL reads it directly only once WinUSB drives
// it, so "Driver Needed" is a plugged-in adapter under another driver.
using AdapterState = bodplat::GcAdapterState;

AdapterState adapter_state() { return bodplat::GcAdapterDriverState(); }

const char* adapter_status() {
    static Uint64 checked;
    static AdapterState state = AdapterState::NotFound;
    static const char* last = nullptr;
    const Uint64 now = SDL_GetTicks();
    if (checked == 0 || now - checked > 2000) {
        checked = now;
        state = adapter_state();
    }
    // What the app's own reader found (gc_adapter.c) is what counts: it opened the device or could not. Windows' device
    // list only explains a missing adapter (no driver bound) when the reader has nothing.
    const BluewakeGcaStatus reader = bluewake_gc_adapter_status();
    const char* text = reader == BLUEWAKE_GCA_CONNECTED || reader == BLUEWAKE_GCA_REPLAY ? "Connected"
                       : reader == BLUEWAKE_GCA_IN_USE                                    ? "In Use by Another Program"
                       : state == AdapterState::Ready          ? "Connected"
                       : state == AdapterState::DriverNeeded ? "Driver Needed"
                                                             : "Not Found";
    if (text != last) {
        last = text;
        std::fprintf(stderr, "[settings] GameCube adapter: %s\n", text);
    }
    return text;
}

// --- the menu's pages -------------------------------------------------------------

void disable_unless(WidgetInfo& w, bool condition, const char* reason) {
    if (!condition)
        w.disabledReasons.push_back(reason);
}

void build_settings() {
    using shipmenu::Menu;
    Menu& m = g_menu;
    const char* H = "Settings";

    // General
    m.AddSidebar(H, "General", 2);
    m.AddWidget(H, "General", 0, "Menu Settings", WidgetType::SeparatorText);
    {
        std::vector<std::string> themes;
        for (int i = 0; i < shipmenu::theme_count(); ++i)
            themes.emplace_back(shipmenu::theme_name(i));
        m.AddWidget(H, "General", 0, "Menu Theme", WidgetType::CVarCombobox)
            .CVar(CVAR_SETTING("Menu.Style"))
            .Items({"Wind Waker", "Classic"})
            .DefaultValue(0)
            .Tooltip("Wind Waker: the menu drawn with the game's own paper, plates and lettering.\n"
                     "Classic: Boat of Daphnes' original menu.");
        m.AddWidget(H, "General", 0, "Classic Menu Color", WidgetType::CVarCombobox)
            .CVar(CVAR_SETTING("Menu.Theme"))
            .Items(themes)
            .DefaultValue(5)
            .PreFunc([](shipmenu::WidgetInfo& w) { w.hidden = !shipmenu::ClassicTheme(); })
            .Tooltip("Changes the color of the Classic menu's widgets.");
    }
    m.AddWidget(H, "General", 0, "Menu Controller Navigation", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Menu.ControllerNavigation"))
        .Tooltip("Allows controller navigation of the menu.\n\nD-pad to move between items, A to select, B to move "
                 "up in scope.");
    m.AddWidget(H, "General", 0, "Search In Sidebar", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Menu.SearchInSidebar"))
        .Tooltip("Displays the search field at the top of the sidebar instead of in the header.");
    m.AddWidget(H, "General", 0, "Menu Background Opacity: %.0f %%", WidgetType::CVarSliderFloat)
        .CVar(CVAR_SETTING("Menu.BackgroundOpacity"))
        .RangeFloat(0.2f, 1.0f, 0.05f)
        .DefaultFloat(0.85f)
        .Percent()
        .Tooltip("Sets how much of the game shows through the menu.");
    m.AddWidget(H, "General", 0, "General Settings", WidgetType::SeparatorText);
    m.AddWidget(H, "General", 0, "Pause on Focus Lost", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("PauseOnFocusLost"))
        .Tooltip("Pauses the game when the window is not the focused window.")
        .Callback([](WidgetInfo&) { aurora_set_pause_on_focus_lost(CVarGetInteger(CVAR_SETTING("PauseOnFocusLost"), 0)); });
    m.AddWidget(H, "General", 0, "Allow Background Inputs", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("AllowBackgroundInputs"))
        .DefaultValue(1)
        .Tooltip("Allows controller inputs to be picked up by the game even when the window is not focused. Off: "
                 "controllers, the GameCube adapter and the keyboard only reach the game while its window is focused.")
        .Callback([](WidgetInfo&) {
            aurora_set_background_input(CVarGetInteger(CVAR_SETTING("AllowBackgroundInputs"), 1) != 0);
        });
    m.AddWidget(H, "General", 0, "Open App Files Folder", WidgetType::Button)
        .Tooltip("Opens the folder that holds the settings, Save, mods, Randomizer and logs folders.")
        .Callback([](WidgetInfo&) { open_folder(g_data_dir); });
    m.AddWidget(H, "General", 0, "Open Logs Folder", WidgetType::Button)
        .Tooltip("Opens the folder of session and crash logs. Attach the newest when reporting a problem.")
        .Callback([](WidgetInfo&) { open_folder(g_data_dir + "logs"); });
    // (Precompile Shaders is not offered: this renderer makes its shaders while you play, from its pipeline cache)
    m.AddWidget(H, "General", 1, "About", WidgetType::SeparatorText);
    m.AddWidget(H, "General", 1,
                BLUEWAKE_APP_NAME " is a static recompilation of The Legend of Zelda: The Wind Waker for the GameCube "
                "(USA), built on your PC from your own copy of the game.",
                WidgetType::Text);
    m.AddWidget(H, "General", 1, "Settings are kept in boat-of-daphnes.json beside the executable.",
                WidgetType::Text);
    notifications::BuildMenu(m, H, "General", 1);
    m.AddWidget(H, "General", 1, "Credits", WidgetType::SeparatorText);
    m.AddWidget(H, "General", 1,
                "Boat of Daphnes by AgitationSkeleton. The Wind Waker Randomizer by LagoLunatic. Better Wind Waker by "
                "WideBoner. Swift Sail textures from the Wind Waker Randomizer (textures by Hypatia, design by "
                "MelonSpeedruns). Aurora by encounter; zeldaret/tww's decompilation; Dolphin's Zelda audio.",
                WidgetType::Text);

    // Audio
    m.AddSidebar(H, "Audio", 2);
    m.AddWidget(H, "Audio", 0, "Master Volume: %d %%", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Audio.MasterVolume"))
        .Range(0, 100, 1)
        .DefaultValue(100)
        .Tooltip("Adjusts the overall volume of the game.")
        .Callback([](WidgetInfo&) { apply_audio(); });
    m.AddWidget(H, "Audio", 0, "Music Volume: %d %%", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Audio.MusicVolume"))
        .Range(0, 100, 1)
        .DefaultValue(100)
        .Tooltip("Adjusts the volume of the background music, inside the game's own audio. 0 plays the game without "
                 "music (sound effects and voices stay).")
        .Callback([](WidgetInfo&) { apply_audio(); });
    m.AddWidget(H, "Audio", 0, "Sound Effects Volume: %d %%", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Audio.SfxVolume"))
        .Range(0, 100, 1)
        .DefaultValue(100)
        .Tooltip("Adjusts the volume of the sound effects and voices, inside the game's own audio.")
        .Callback([](WidgetInfo&) { apply_audio(); });
    m.AddWidget(H, "Audio", 0, "Fanfare Volume: %d %%", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Audio.FanfareVolume"))
        .Range(0, 100, 1)
        .DefaultValue(100)
        .Tooltip("Adjusts the volume of the short jingles (item get, heart, songs, chests, game over, race end), inside "
                 "the game's own audio.")
        .Callback([](WidgetInfo&) { apply_audio(); });
    m.AddWidget(H, "Audio", 0, "Audio Mode", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Audio.Mode"))
        .Items({"HLE (Fast)", "LLE (Accurate)"})
        .Callback([](WidgetInfo&) { bluewake_dsp_request_mode(CVarGetInteger(CVAR_SETTING("Audio.Mode"), 0) == 1); })
        .Tooltip("Sets how the GameCube's sound chip is run.\n\nHLE: Dolphin's high-level Zelda sound, fast.\n"
                 "LLE: the sound chip's own program, exact but slower.\n\nChanging it while the game runs takes "
                 "effect at the next gap between two audio frames, and the sound goes on. (Not from a save state "
                 "loaded at start: restart for that.)");

    // Graphics
    m.AddSidebar(H, "Graphics", 2);
    m.AddWidget(H, "Graphics", 0, "Graphics Options", WidgetType::SeparatorText);
    m.AddWidget(H, "Graphics", 0, "Toggle Fullscreen", WidgetType::Button)
        .Tooltip("Toggles fullscreen on and off. (F11 or Alt+Enter)")
        .Callback([](WidgetInfo&) {
            if (SDL_Window* w = game_window())
                set_fullscreen(w, !is_fullscreen(w));
        });
    m.AddWidget(H, "Graphics", 0, "Internal Resolution: %d %%", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Graphics.InternalResolution"))
        .Range(50, 400, 5)
        .DefaultValue(100)
        .Tooltip("Multiplies your output resolution by the value set, as a more demanding but effective form of "
                 "anti-aliasing.")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, !CVarGetInteger(CVAR_SETTING("Graphics.FixedVerticalResolution"), 0),
                           "Set Fixed Vertical Resolution is on");
            disable_unless(w, !CVarGetInteger(CVAR_SETTING("Graphics.PixelPerfect"), 0), "Pixel Perfect Mode is on");
        })
        .Callback([](WidgetInfo&) { apply_resolution(game_window()); });
    m.AddWidget(H, "Graphics", 0, "Interpolation FPS: %d", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Graphics.InterpolationFPS"))
        .Range(30, 240, 30)
        .DefaultValue(30)
        .Tooltip("Smooths motion by making frames between the game's own (the game runs at 30 frames a second). "
                 "30 is the original; 60 or more needs a display that refreshes that fast.")
        .Callback([](WidgetInfo&) { apply_interpolation(); });
    m.AddWidget(H, "Graphics", 0, "Match Refresh Rate", WidgetType::Button)
        .Tooltip("Sets Interpolation FPS to your display's refresh rate.")
        .Callback([](WidgetInfo&) {
            int hz = 60;
            if (SDL_Window* w = game_window())
                if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)))
                    if (mode->refresh_rate > 0.f)
                        hz = static_cast<int>(mode->refresh_rate + 0.5f);
            CVarSetInteger(CVAR_SETTING("Graphics.InterpolationFPS"), std::clamp((hz + 15) / 30 * 30, 30, 240));
            CVarSaveNextFrame();
            apply_interpolation();
        });
    m.AddWidget(H, "Graphics", 0, "Recommended for This PC", WidgetType::Button)
        .Tooltip("Sets Interpolation FPS from this PC's processor and display: the display's rate up to 120 with 6 or "
                 "more cores, up to 60 with 4 or 5, otherwise 30. The first launch does this on its own.")
        .Callback([](WidgetInfo&) {
            profile_hardware(game_window(), true);
            apply_interpolation();
        });
    m.AddWidget(H, "Graphics", 0, "Renderer", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Graphics.Backend"))
        .Items({"Auto", "Direct3D 12", "Vulkan", "Direct3D 11"})
        .DefaultValue(0)
        .Tooltip("The graphics API the game draws with. Auto tries Direct3D 12, then Vulkan, then Direct3D 11. If the "
                 "game stutters or runs slowly on your GPU, try another one. Takes effect after a restart.");
    m.AddWidget(H, "Graphics", 0, "Enable Vsync", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Graphics.Vsync"))
        .DefaultValue(1)
        .Tooltip("Removes tearing, but clamps your frame rate to your display's refresh rate.")
        .Callback([](WidgetInfo&) { aurora_enable_vsync(CVarGetInteger(CVAR_SETTING("Graphics.Vsync"), 1) != 0); });
    m.AddWidget(H, "Graphics", 0, "Anisotropic Filtering", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Graphics.Anisotropy"))
        .Items({"Off", "2x", "4x", "8x", "16x"})
        .Tooltip("Sharpens textures seen at an angle, such as the ground into the distance.")
        .Callback([](WidgetInfo&) { apply_graphics(game_window()); });
    m.AddWidget(H, "Graphics", 0, "Show FPS", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Graphics.ShowFPS"))
        .Tooltip("Shows the frame rate in a corner of the screen. (F9)")
        .Callback([](WidgetInfo&) { aurora_set_fps_overlay(CVarGetInteger(CVAR_SETTING("Graphics.ShowFPS"), 0) != 0); });
    m.AddWidget(H, "Graphics", 0, "Widescreen", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Graphics.Widescreen"))
        .Items({"Off (4:3)", "Match Window"})
        .DefaultValue(1)
        .Tooltip("Sets the shape of the picture.\n\nOff: the game's own 4:3, with black bars on the sides.\nMatch "
                 "Window: the picture fills the window, showing more of the scene to the sides.")
        .Callback([](WidgetInfo&) { apply_graphics(game_window()); });
    m.AddWidget(H, "Graphics", 0, "Reset Window Size", WidgetType::Button)
        .Tooltip("Returns the window to its starting size, centred.")
        .Callback([](WidgetInfo&) { reset_window(game_window()); });
    m.AddWidget(H, "Graphics", 1, "Advanced Graphics Options", WidgetType::SeparatorText);
    m.AddWidget(H, "Graphics", 1, "Resolution", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        int ww = 0, wh = 0;
        if (SDL_Window* w = game_window())
            SDL_GetWindowSizeInPixels(w, &ww, &wh);
        u32 rw = 0, rh = 0;
        AuroraGetRenderSize(&rw, &rh);
        ImGui::Text("Window: %d x %d", ww, wh);
        ImGui::Text("Internal resolution: %u x %u", rw, rh);
    });
    m.AddWidget(H, "Graphics", 1, "Force Aspect Ratio", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Graphics.ForceAspectRatio"))
        .Items({"Off", "Original (4:3)", "16:10", "Widescreen (16:9)", "Ultrawide (21:9)", "Super Ultrawide (32:9)",
                "Custom"})
        .Tooltip("Keeps the picture at this shape, with black bars filling the rest of the window. Off lets the "
                 "picture fill the window.")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, CVarGetInteger(CVAR_SETTING("Graphics.Widescreen"), 1) != 0, "Widescreen is Off");
        })
        .Callback([](WidgetInfo&) { apply_graphics(game_window()); });
    for (const char* axis : {"X", "Y"}) {
        const bool x = axis[0] == 'X';
        m.AddWidget(H, "Graphics", 1, x ? "Aspect Ratio X: %d" : "Aspect Ratio Y: %d", WidgetType::CVarSliderInt)
            .CVar(x ? CVAR_SETTING("Graphics.AspectRatioX") : CVAR_SETTING("Graphics.AspectRatioY"))
            .Range(1, 64, 1)
            .DefaultValue(x ? 16 : 9)
            .Tooltip(x ? "Sets the width of the custom aspect ratio." : "Sets the height of the custom aspect ratio.")
            .PreFunc([](WidgetInfo& w) {
                w.hidden = CVarGetInteger(CVAR_SETTING("Graphics.ForceAspectRatio"), 0) != kAspectCustom ||
                           !CVarGetInteger(CVAR_SETTING("Graphics.Widescreen"), 1);
            })
            .Callback([](WidgetInfo&) { apply_graphics(game_window()); });
    }
    m.AddWidget(H, "Graphics", 1, "Set Fixed Vertical Resolution", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Graphics.FixedVerticalResolution"))
        .Tooltip("Renders the picture at a fixed number of lines whatever the window's size, and scales it to the "
                 "window. The width follows the picture's shape.")
        .Callback([](WidgetInfo&) { apply_resolution(game_window()); });
    m.AddWidget(H, "Graphics", 1, "Vertical Resolution: %d Pixels", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Graphics.VerticalResolution"))
        .Range(240, 4320, 10)
        .DefaultValue(480)
        .Tooltip("Sets the number of lines the picture is rendered with.")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, CVarGetInteger(CVAR_SETTING("Graphics.FixedVerticalResolution"), 0) != 0,
                           "Set Fixed Vertical Resolution is off");
        })
        .Callback([](WidgetInfo&) { apply_resolution(game_window()); });
    m.AddWidget(H, "Graphics", 1, "Pixel Count Presets", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Graphics.PixelCountPreset"))
        .Items({"Native (480p)", "2x (960p)", "720p", "1080p", "1440p", "4K (2160p)"})
        .Tooltip("Sets the Vertical Resolution to a common size.")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, CVarGetInteger(CVAR_SETTING("Graphics.FixedVerticalResolution"), 0) != 0,
                           "Set Fixed Vertical Resolution is off");
        })
        .Callback([](WidgetInfo&) {
            const int preset = std::clamp(CVarGetInteger(CVAR_SETTING("Graphics.PixelCountPreset"), 0), 0, 5);
            CVarSetInteger(CVAR_SETTING("Graphics.VerticalResolution"), kPixelPresets[preset]);
            apply_resolution(game_window());
        });
    m.AddWidget(H, "Graphics", 1, "Pixel Perfect Mode", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Graphics.PixelPerfect"))
        .Tooltip("Shows the picture at a whole-number multiple of its rendered size, so every pixel is the same size, "
                 "with black around it. The picture is rendered at the game's own 480 lines unless Set Fixed Vertical "
                 "Resolution is on.")
        .Callback([](WidgetInfo&) {
            apply_resolution(game_window());
            apply_graphics(game_window());
        });
    m.AddWidget(H, "Graphics", 1, "Automatically Scale Image to Fit Viewport", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Graphics.AutoIntegerScale"))
        .DefaultValue(1)
        .Tooltip("Uses the largest whole-number multiple that fits in the window.")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, CVarGetInteger(CVAR_SETTING("Graphics.PixelPerfect"), 0) != 0, "Pixel Perfect Mode is off");
        })
        .Callback([](WidgetInfo&) { apply_graphics(game_window()); });
    m.AddWidget(H, "Graphics", 1, "Integer Scale Factor: %d", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Graphics.IntegerScaleFactor"))
        .Range(1, 10, 1)
        .DefaultValue(2)
        .Tooltip("Sets the multiple the picture is shown at, or the largest that fits if the window is smaller.")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, CVarGetInteger(CVAR_SETTING("Graphics.PixelPerfect"), 0) != 0, "Pixel Perfect Mode is off");
            disable_unless(w, !CVarGetInteger(CVAR_SETTING("Graphics.AutoIntegerScale"), 1),
                           "Automatically Scale Image to Fit Viewport is on");
        })
        .Callback([](WidgetInfo&) { apply_graphics(game_window()); });
    m.AddWidget(H, "Graphics", 1, "Output Resampling", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Graphics.OutputResampling"))
        .Items({"Bilinear", "Area"})
        .DefaultValue(1)
        .Tooltip("Sets how the internal resolution is scaled to the window.\n\nBilinear: one sample per pixel, "
                 "softer when scaled far down.\nArea: averages every covered pixel, sharper when the internal "
                 "resolution is above the window's.")
        .Callback([](WidgetInfo&) { apply_graphics(game_window()); });

    // Controls
    m.AddSidebar(H, "Controls", 2);
    m.AddSidebar(H, "Controller Bindings", 1);
    m.AddWidget(H, "Controller Bindings", 0, "Controller Bindings", WidgetType::Window)
        .CVar(CVAR_WINDOW("ControllerBindings"))
        .HideInSearch()
        .Tooltip("Detaches the Controller Bindings into its own window.")
        .CustomFunction([](WidgetInfo&) { input_editor::DrawEmbedded(shipmenu::MenuScale()); });
    m.AddWidget(H, "Controls", 0, "Controller", WidgetType::SeparatorText);
    m.AddWidget(H, "Controls", 0, "Controller Bindings", WidgetType::Button)
        .Tooltip("Opens the Controller Bindings page: the controller for each port and the buttons, sticks and keys "
                 "that drive each GameCube control.")
        .Callback([](WidgetInfo&) {
            CVarSetString("gSettings.Menu.SettingsSidebarSection", "Controller Bindings");
            CVarSaveNextFrame();
        });
    m.AddWidget(H, "Controls", 0, "Invert Camera X Axis", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Controls.InvertCameraX"))
        .Tooltip("Inverts the camera's horizontal movement on the C-Stick.")
        .Callback([](WidgetInfo&) { apply_controller(); });
    m.AddWidget(H, "Controls", 0, "Invert Camera Y Axis", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Controls.InvertCameraY"))
        .Tooltip("Inverts the camera's vertical movement on the C-Stick.")
        .Callback([](WidgetInfo&) { apply_controller(); });
    m.AddWidget(H, "Controls", 0, "Use 'Switch' Targeting Mode", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Controls.SwitchTargeting"))
        .Tooltip("Z-Targeting toggles on a press of the target button instead of lasting only while it is held. Holds the "
                 "game's own Z-Targeting option (Options menu) at Switch while on; off, that option governs again. Works in "
                 "vanilla and randomizer sessions.")
        .Callback([](WidgetInfo&) { apply_preferences(); });
    m.AddWidget(H, "Controls", 0, "GameCube Adapter", WidgetType::SeparatorText);
    m.AddWidget(H, "Controls", 0, "First Connected Port Is Player 1", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Controls.FirstConnectedIsPlayer1"))
        .DefaultValue(1)
        .Tooltip("Makes the first connected controller player 1 even when it is plugged into a later adapter port. "
                 "Wind Waker only reads port 1 for Link.")
        .Callback([](WidgetInfo&) { input_editor::ApplyPorts(); });
    m.AddWidget(H, "Controls", 0, "Controllers Connected", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        ImGui::TextUnformatted(input_editor::Summary());
    });
    m.AddWidget(H, "Controls", 0, "Adapter Status", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        const char* status = adapter_status();
        ImGui::Text("Status: %s", status);
        if (const unsigned ports = bluewake_gc_adapter_ports()) {
            char line[64] = "Controllers on the adapter: port";
            for (unsigned p = 0; p < 4; ++p)
                if (ports & (1u << p))
                    std::snprintf(line + std::strlen(line), sizeof line - std::strlen(line), " %u", p + 1);
            ImGui::TextUnformatted(line);
        }
        if (std::strcmp(status, "In Use by Another Program") == 0)
            ImGui::TextWrapped("The adapter is plugged in and has its driver, but another program has it open (Dolphin, "
                               "Steam or Slippi, for example). Close that program, or unplug and plug the adapter in "
                               "again.");
        if (std::strcmp(status, "Driver Needed") == 0)
            ImGui::TextWrapped("The adapter is plugged in, but Windows is not using the WinUSB driver for it, so the "
                               "game cannot read it. Bind it to WinUSB once (for example with Zadig).");
    });
    m.AddWidget(H, "Controls", 0, "Install Adapter Driver", WidgetType::Button)
        .Tooltip("Installs the WinUSB driver the official adapter needs so the game can read it. Windows asks for "
                 "permission once. The driver package is signed with a certificate made on this PC, which is added to "
                 "Windows' trusted publishers.")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, !adapter_driver::Busy(), "An install is already running");
            const char* status = adapter_status();
            disable_unless(w, std::strcmp(status, "Not Found") != 0, "The adapter is not plugged in");
            disable_unless(w, std::strcmp(status, "Connected") != 0, "The driver is already installed");
            disable_unless(w, std::strcmp(status, "In Use by Another Program") != 0, "The driver is installed; another program has the adapter");
        })
        .Callback([](WidgetInfo&) {
            g_menu.Ask({"Install Adapter Driver",
                        "Install the WinUSB driver for the GameCube adapter?\n\nWindows will ask for permission. The "
                        "driver package is signed with a certificate made on this PC, and that certificate is added to "
                        "Windows' trusted publishers.",
                        "Install", "Cancel", [] { adapter_driver::Install(); }});
        });
    m.AddWidget(H, "Controls", 0, "Install Status", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        const std::string status = adapter_driver::Status();
        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
    });
    m.AddWidget(H, "Controls", 1, "Rumble", WidgetType::SeparatorText);
    m.AddWidget(H, "Controls", 1, "Rumble Mode", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Controls.Rumble.Mode"))
        .Items({"Off", "Classic", "Enhanced"})
        .DefaultValue(1)
        .Tooltip("Sets how the controller vibrates.\n\nClassic: the motor on and off, as the game asks of a "
                 "GameCube controller.\nEnhanced: hits, falls, explosions and quakes shaped by their strength, "
                 "with trigger rumble.")
        .Callback([](WidgetInfo&) { apply_rumble(); });
    m.AddWidget(H, "Controls", 1, "Rumble Strength: %d %%", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("Controls.Rumble.Strength"))
        .Range(0, 100, 5)
        .DefaultValue(80)
        .Tooltip("Sets how strongly Enhanced rumble vibrates.")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, CVarGetInteger(CVAR_SETTING("Controls.Rumble.Mode"), 1) == 2, "Rumble Mode is not Enhanced");
        })
        .Callback([](WidgetInfo&) { apply_rumble(); });
    m.AddWidget(H, "Controls", 1, "Trigger Rumble", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Controls.Rumble.Triggers"))
        .DefaultValue(1)
        .Tooltip("Uses the triggers' own motors on controllers that have them (Xbox impulse triggers, DualSense).")
        .PreFunc([](WidgetInfo& w) {
            disable_unless(w, CVarGetInteger(CVAR_SETTING("Controls.Rumble.Mode"), 1) == 2, "Rumble Mode is not Enhanced");
        })
        .Callback([](WidgetInfo&) { apply_rumble(); });

    // Tingle Tuner
    m.AddSidebar(H, "Tingle Tuner", 2);
    m.AddWidget(H, "Tingle Tuner", 0, "Game Boy Advance", WidgetType::SeparatorText);
    m.AddWidget(H, "Tingle Tuner", 0, "Mode", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("TingleTuner.Mode"))
        .Items({"Off", "In-App GBA", "Real GBA (JoyBus bridge)"})
        .DefaultValue(1)
        .Tooltip("Where the Tingle Tuner's Game Boy Advance is.\n\nOff: no GBA; using the Tuner says none was found.\n"
                 "In-App GBA: an emulated GBA on controller port 2 with a free BIOS, shown in a floating window. "
                 "The game sends the Tingle Tuner program to it, as it does to a real one; nothing else is needed.\n"
                 "Real GBA: a Game Boy Advance on a GameCube link cable. The Wii U GameCube adapter cannot carry that "
                 "(it only reports controllers), so this needs a JoyBus bridge, which is not built yet.")
        .Callback([](WidgetInfo&) { gba_window::ApplyMode(); });
    m.AddWidget(H, "Tingle Tuner", 0, "Tingle Tuner Status", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        ImGui::TextWrapped("%s", gba_window::Status());
    });
    m.AddWidget(H, "Tingle Tuner", 0, "GBA Window", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("TingleTuner.Window"))
        .Items({"When In Use", "Always", "Never"})
        .DefaultValue(0)
        .Tooltip("When the floating Game Boy Advance window is shown. When In Use opens it when the game starts "
                 "talking to the GBA and hides it a few seconds after that stops.");
    m.AddWidget(H, "Tingle Tuner", 0, "Integer Scaling", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("TingleTuner.IntegerScale"))
        .Tooltip("Sizes the window to whole multiples of the GBA's 240 x 160 screen. The window always keeps the "
                 "screen's 3:2 shape.");
    m.AddWidget(H, "Tingle Tuner", 0, "Restart GBA", WidgetType::Button)
        .Tooltip("Starts the emulated GBA again from its BIOS. The Tingle program is gone; call Tingle again.")
        .Callback([](WidgetInfo&) { gba_window::ResetGba(); });
    m.AddWidget(H, "Tingle Tuner", 1, "Sound", WidgetType::SeparatorText);
    m.AddWidget(H, "Tingle Tuner", 1, "Mute GBA", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("TingleTuner.Mute"))
        .Tooltip("Silences the Game Boy Advance's own sound (Tingle's music and effects).");
    m.AddWidget(H, "Tingle Tuner", 1, "GBA Volume: %d %%", WidgetType::CVarSliderInt)
        .CVar(CVAR_SETTING("TingleTuner.Volume"))
        .Range(0, 100, 5)
        .DefaultValue(60)
        .Tooltip("Sets how loud the Game Boy Advance is next to the game.");
    m.AddWidget(H, "Tingle Tuner", 1, "Controls", WidgetType::SeparatorText);
    m.AddWidget(H, "Tingle Tuner", 1, "GBA Controller", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        input_editor::DrawGbaController(shipmenu::MenuScale());
    });
    m.AddWidget(H, "Tingle Tuner", 1, "GBA Controller Bindings", WidgetType::Button)
        .Callback([](WidgetInfo&) {
            CVarSetString("gSettings.Menu.SettingsSidebarSection", "Controller Bindings");
            CVarSaveNextFrame();
        })
        .Tooltip("Opens the bindings page. The GBA is player 2's controller: pick its controller and keys on the "
                 "Port 2 tab. A and B are A and B, Z is Select, Start is Start, and L and R are L and R.");
    m.AddWidget(H, "Tingle Tuner", 1, "Use Default Keyboard Layout", WidgetType::Button)
        .Tooltip("Gives port 2 the keyboard: arrows, X = A, Z = B, Right Shift = Select, Enter = Start, A = L, "
                 "S = R.")
        .Callback([](WidgetInfo&) { gba_window::UseDefaultKeys(); });

    // Presets
    m.AddSidebar(H, "Presets", 1);
    m.AddWidget(H, "Presets", 0, "Enhancement Presets", WidgetType::SeparatorText);
    m.AddWidget(H, "Presets", 0, "Preset", WidgetType::CVarCombobox)
        .CVar(CVAR_SETTING("Presets.Selected"))
        .Items({kPresetNames[0], kPresetNames[1], kPresetNames[2], kPresetNames[3]})
        .DefaultValue(1)
        .Tooltip("Chooses a set of enhancements to apply. The list under it shows what it changes.");
    m.AddWidget(H, "Presets", 0, "Preset Contents", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        const int preset = std::clamp(CVarGetInteger(CVAR_SETTING("Presets.Selected"), 1), 0, 3);
        ImGui::TextWrapped("%s", kPresetInfo[preset]);
    });
    m.AddWidget(H, "Presets", 0, "Apply Preset", WidgetType::Button)
        .Tooltip("Sets every enhancement as the chosen preset has it. Graphics, controls and audio are not changed.")
        .Callback([](WidgetInfo&) {
            const int preset = std::clamp(CVarGetInteger(CVAR_SETTING("Presets.Selected"), 1), 0, 3);
            g_menu.Ask({"Apply Preset", std::string("Apply the ") + kPresetNames[preset] + " preset? It replaces your enhancement settings.",
                        "Apply", "Cancel", [preset] { apply_preset(preset); }});
        });

    // Mod Menu
    m.AddSidebar(H, "Mod Menu", 1);
    mods::BuildMenu(m, H);
    // GameBanana's Wind Waker player models, browsed and installed in place (gb_browser.cpp)
    m.AddSidebar(H, "Browse Models", 1);
    m.AddWidget(H, "Browse Models", 0, "Browse Models", WidgetType::Window)
        .CVar(CVAR_WINDOW("ModelBrowser"))
        .HideInSearch()
        .Tooltip("Detaches the model browser into its own window.")
        .CustomFunction([](WidgetInfo&) { gbbrowser::DrawEmbedded(shipmenu::MenuScale()); });
    m.AddWidget(H, "Mod Menu", 0, "Texture Packs", WidgetType::SeparatorText);
    m.AddWidget(H, "Mod Menu", 0, "Enable Texture Packs", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Mods.TexturePacks"))
        .Tooltip("Loads the Dolphin-format texture packs in the mods\\textures folder. They load and unload at "
                 "once, in the background; the picture takes them up as it is drawn again.")
        .Callback([](WidgetInfo&) { sync_live_textures(); });
    m.AddWidget(H, "Mod Menu", 0, "Open Mods Folder", WidgetType::Button)
        .Tooltip("Opens the mods folder beside the executable.")
        .Callback([](WidgetInfo&) { open_folder(g_data_dir + "mods"); });
}

void build_enhancements() {
    using shipmenu::Menu;
    Menu& m = g_menu;
    const char* H = "Enhancements";
    m.AddSidebar(H, "Quality of Life", 2);
    m.AddSidebar(H, "Skips & Speed-ups", 3);
    m.AddSidebar(H, "Items", 2);
    m.AddSidebar(H, "Graphics", 2);
    m.AddSidebar(H, "Cheats", 1);
    m.AddSidebar(H, "Cosmetics Editor", 1);
    m.AddWidget(H, "Cosmetics Editor", 0, "Cosmetics Editor", WidgetType::Window)
        .CVar(CVAR_WINDOW("CosmeticsEditor"))
        .HideInSearch()
        .Tooltip("Detaches the Cosmetics Editor into its own window.")
        .CustomFunction([](WidgetInfo&) { cosmetics::DrawEmbedded(shipmenu::MenuScale()); });

    // Better Wind Waker's switches, each in its place; a section heading the
    // first time a section appears in a column.
    std::map<std::string, bool> sections;
    for (const auto& row : kBetterWw) {
        const std::string key = std::string(row.sidebar) + "/" + std::to_string(row.column) + "/" + row.section;
        if (!sections[key]) {
            sections[key] = true;
            m.AddWidget(H, row.sidebar, row.column, row.section, WidgetType::SeparatorText);
        }
        auto& widget = m.AddWidget(H, row.sidebar, row.column,
                                   row.label, WidgetType::CVarCheckbox)
                           .CVar(better_ww_cvar(row.name))
                           .DefaultValue(row.def)
                           .Tooltip(row.tooltip);
        widget.Callback([name = row.name](WidgetInfo&) { apply_better_ww(name); });
        if (std::strcmp(row.name, "tingle_chests") == 0)
            m.AddWidget(H, row.sidebar, row.column, "Reveal Sea Chart", WidgetType::CVarCheckbox)
                .CVar(CVAR_ENHANCEMENT("RevealSeaChart"))
                .DefaultValue(0)
                .Tooltip("Shows the whole sea chart. Turning it off shows your file's own progress again; your save is "
                         "never changed.")
                .Callback([](WidgetInfo&) { apply_reveal_sea_chart(); });
        if (std::strcmp(row.name, "instant_text") == 0)
            m.AddWidget(H, row.sidebar, row.column, "Text Speed: %dx", WidgetType::CVarSliderInt)
                .CVar(CVAR_ENHANCEMENT("TextSpeed"))
                .Range(1, 5, 1)
                .DefaultValue(1)
                .Tooltip("How many characters a text box types out each frame, from 1x (the game's speed) to 5x. Waits, "
                         "sounds, choices and names still work as in the game. Instant Text, when on, comes first.")
                .Callback([](WidgetInfo&) { apply_port_options(); });
        if (std::strcmp(row.name, "instant_text") == 0)
            m.AddWidget(H, row.sidebar, row.column, "Skip Text", WidgetType::CVarCheckbox)
                .CVar(CVAR_ENHANCEMENT("SkipText"))
                .Tooltip("Holding B shows each text box at once and moves on to the next by itself. Choices still wait "
                         "for you.");
        if (std::strcmp(row.name, "skip_intro_movie") == 0)
            m.AddWidget(H, row.sidebar, row.column, "Remove Title and Ending Videos", WidgetType::CVarCheckbox)
                .CVar(CVAR_ENHANCEMENT("RemoveTitleAndEndingVideos"))
                .Tooltip("Replaces the movie that plays when the title screen sits idle and the one in the ending with a "
                         "blank one (wwrando's tweak; saves nothing but the wait). Applies the next time the game opens "
                         "the movie.")
                .Callback([](WidgetInfo&) { apply_preferences(); });
        if (std::strcmp(row.name, "unrestricted_boat") == 0)
            m.AddWidget(H, row.sidebar, row.column, "Invert Sea Compass X-Axis", WidgetType::CVarCheckbox)
                .CVar(CVAR_ENHANCEMENT("InvertSeaCompassX"))
                .Tooltip("Flips the sea compass's left and right on the sailing HUD (wwrando's tweak).")
                .PreFunc([](WidgetInfo& wi) {
                    module_tooltip(wi, "Flips the sea compass's left and right on the sailing HUD (wwrando's tweak).",
                                   "invert_sea_compass_x");
                })
                .Callback([](WidgetInfo&) { apply_preferences(); });
        if (std::strcmp(row.name, "skip_intro_movie") == 0)
            m.AddWidget(H, row.sidebar, row.column, "Skip Intro Story with Start", WidgetType::CVarCheckbox)
                .CVar(CVAR_ENHANCEMENT("SkipIntroWithStart"))
                .DefaultValue(1)
                .Tooltip("Press Start during the story told when a new game starts to fade out of it and go on to "
                         "Outset Island.")
                .Callback([](WidgetInfo&) {
                    bluewake_intro_skip_set(CVarGetInteger(CVAR_ENHANCEMENT("SkipIntroWithStart"), 1) != 0);
                });
    }
    m.AddWidget(H, "Items", 0, "Treasure Light Up Close", WidgetType::CVarCombobox)
        .CVar(CVAR_ENHANCEMENT("TreasureLightUpClose"))
        .Items({"Off", "On (Treasure)", "On (Triforce Pieces)", "On (All)"})
        .Tooltip("Keeps the light over sunken treasure on as the boat comes close, instead of fading out.\n\n"
                 "Treasure: Treasure Chart spots.\nTriforce Pieces: Triforce Chart spots.\nAll: every light, "
                 "the plain glints at sea too.")
        .Callback([](WidgetInfo&) {
            bluewake_treasure_glow_set(CVarGetInteger(CVAR_ENHANCEMENT("TreasureLightUpClose"), 0));
        });
    m.AddWidget(H, "Quality of Life", 1, "Forest Water", WidgetType::SeparatorText);
    m.AddWidget(H, "Quality of Life", 1, "Keep Watered Trees", WidgetType::CVarCheckbox)
        .CVar(CVAR_ENHANCEMENT("KeepWateredTrees"))
        .Tooltip("Keeps the trees already watered when the Forest Water runs out. Refill and carry on with the "
                 "trees still to water.")
        .Callback([](WidgetInfo&) { apply_forest_water(); });
    m.AddWidget(H, "Quality of Life", 1, "Longer Forest Water Timer", WidgetType::CVarCheckbox)
        .CVar(CVAR_ENHANCEMENT("LongerForestWaterTimer"))
        .Tooltip("Gives Forest Water 30 minutes instead of 20. Applies the next time Link scoops it.")
        .Callback([](WidgetInfo&) { apply_forest_water(); });
    m.AddWidget(H, "Skips & Speed-ups", 2, "Misc", WidgetType::SeparatorText);
    m.AddWidget(H, "Skips & Speed-ups", 2, "Faster Transitions", WidgetType::CVarCheckbox)
        .CVar(CVAR_ENHANCEMENT("FasterTransitions"))
        .Tooltip("Shortens the fade between areas and skips through the black screen while an area loads.")
        .Callback([](WidgetInfo&) { apply_fast_transitions(); });
    m.AddWidget(H, "Skips & Speed-ups", 2, "Quick Doors", WidgetType::CVarCheckbox)
        .CVar(CVAR_ENHANCEMENT("QuickDoors"))
        .Tooltip("Skips Link's walk through doors and the door closing behind him.\nNot accounted for in "
                 "Randomizer logic.")
        .Callback([](WidgetInfo&) { apply_quick_doors(); });
    m.AddWidget(H, "Graphics", 0, "Widescreen", WidgetType::SeparatorText);
    m.AddWidget(H, "Graphics", 0, "Widescreen Actor Culling", WidgetType::CVarCheckbox)
        .CVar(CVAR_ENHANCEMENT("WidescreenActorCulling"))
        .DefaultValue(1)
        .Tooltip("Draws and runs actors in the extra width of a widescreen window, so they do not pop in at the "
                 "edges of the original 4:3 view.\nChanges which actors run; may affect certain glitches.")
        .Callback([](WidgetInfo&) { apply_port_options(); });
    m.AddWidget(H, "Graphics", 0, "HUD at Screen Edges", WidgetType::CVarCheckbox)
        .CVar(CVAR_ENHANCEMENT("HudAtScreenEdges"))
        .DefaultValue(1)
        .Tooltip("Moves the hearts, magic, map and buttons to the edges of a widescreen window. Otherwise they "
                 "stay where they are in the original 4:3 view.")
        .Callback([](WidgetInfo&) { apply_port_options(); });
    m.AddWidget(H, "Quality of Life", 1, "Autosave", WidgetType::SeparatorText);
    m.AddWidget(H, "Quality of Life", 1, "Autosave Every: %d min (0 = off)", WidgetType::CVarSliderInt)
        .CVar(CVAR_ENHANCEMENT("Autosave.Minutes"))
        .Range(0, 30, 1)
        .DefaultValue(0)
        .Tooltip("Saves the current file by itself every so many minutes of play (0 is off), the same save as the pause "
                 "menu's, into the file's own memory card slot. It waits for a quiet moment: no cutscene or menu, "
                 "Link on his feet or at the helm, no scene change.")
        .Callback([](WidgetInfo&) { apply_autosave(); });
    m.AddWidget(H, "Quality of Life", 1, "Autosave When Getting a New Item", WidgetType::CVarCheckbox)
        .CVar(CVAR_ENHANCEMENT("Autosave.OnNewItem"))
        .Tooltip("Saves by itself after Link gets a new item, a heart container, a song or a pearl, as soon as the "
                 "moment is quiet. Works with or without the timer above.")
        .Callback([](WidgetInfo&) { apply_autosave(); });
    m.AddWidget(H, "Quality of Life", 1, "Text", WidgetType::SeparatorText);
    m.AddWidget(H, "Quality of Life", 1, "Crenando", WidgetType::CVarCheckbox)
        .CVar(CVAR_ENHANCEMENT("Crenando"))
        .Tooltip("Ganondorf -> Crenando")
        .Callback([](WidgetInfo&) { bluewake_text_swap_enable(CVarGetInteger(CVAR_ENHANCEMENT("Crenando"), 0) != 0); });
    m.AddWidget(H, "Cheats", 0, "Save States", WidgetType::SeparatorText);
    m.AddWidget(H, "Cheats", 0, "I Understand, Enable Save States", WidgetType::CVarCheckbox)
        .CVar(CVAR_CHEAT("SaveStates"))
        .Tooltip("F6 to save a state, F8 to load the latest one.\n*WARNING*: States depend on this build of the "
                 "game. Keep normal saves too.");
}

void build_dev_tools() {
    shipmenu::Menu& m = g_menu;
    const char* H = "Dev Tools";
    m.AddSidebar(H, "General", 2);
    m.AddWidget(H, "General", 0, "Menu", WidgetType::SeparatorText);
    m.AddWidget(H, "General", 0, "Popout Menu", WidgetType::CVarCheckbox)
        .CVar(CVAR_SETTING("Menu.Popout"))
        .Tooltip("Changes the menu display from an overlay to a movable window.");
    m.AddWidget(H, "General", 0, "Save States", WidgetType::SeparatorText);
    m.AddWidget(H, "General", 0, "Save State (F6)", WidgetType::Button)
        .Tooltip("Saves a state now.")
        .PreFunc([](WidgetInfo& w) { disable_unless(w, CVarGetInteger(CVAR_CHEAT("SaveStates"), 0) != 0, "Save States are not enabled (Enhancements > Cheats)"); })
        .Callback([](WidgetInfo&) { bluewake_save_state_hotkey(false); });
    m.AddWidget(H, "General", 0, "Load Latest State (F8)", WidgetType::Button)
        .Tooltip("Loads the latest save state.")
        .PreFunc([](WidgetInfo& w) { disable_unless(w, CVarGetInteger(CVAR_CHEAT("SaveStates"), 0) != 0, "Save States are not enabled (Enhancements > Cheats)"); })
        .Callback([](WidgetInfo&) { bluewake_save_state_hotkey(true); });
    m.AddWidget(H, "General", 1, "Stats", WidgetType::SeparatorText);
    m.AddWidget(H, "General", 1, "Frame Rate", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        ImGui::Text("Frames shown: %.1f a second", aurora::gfx::calculate_fps());
        ImGui::Text("Game frames: %.1f a second", aurora::gfx::calculate_game_fps());
        ImGui::Text("Widescreen squeeze: %.3f", dol_aurora_widescreen_squeeze());
    });
    devtools::BuildMenu(m, H);
}

void build_menu() {
    if (g_menu_built)
        return;
    g_menu_built = true;
    build_settings();
    build_enhancements();
    randomizer::BuildMenu(g_menu);
    itemtracker::BuildMenu(g_menu);
    checktracker::BuildMenu(g_menu);
    inputviewer::BuildMenu(g_menu);
    online::BuildMenu(g_menu);
    build_dev_tools();
    g_menu.onQuit = quit;
    g_menu.onRestart = soft_restart;
}

// The UI's size for this window: the display's scale (Windows' own, or more
// on a big screen at 100 percent), smaller when the window is too small for
// the menu. No larger than the font was drawn at.
float ui_scale(SDL_Window* w) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float display = w != nullptr ? SDL_GetWindowDisplayScale(w) : 1.0f;
    const float wanted = std::max({1.0f, display, size.y / 900.0f});
    const float fits = std::min(size.x / 660.0f, size.y / 560.0f);
    return std::clamp(std::min(wanted, fits), 0.75f, g_font_scale);
}

// On the first frame: a clear font drawn at the screen's scale, the default
// for everything ImGui draws from the next frame on. It has an atlas of its
// own, handed to Aurora as an ImGui texture (aurora_imgui_add_texture copies
// it on the render worker), so it can be made inside the frame.
void load_font(SDL_Window* w) {
    float scale = std::max(1.0f, SDL_GetWindowDisplayScale(w));
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)))
        scale = std::max(scale, mode->h / 900.0f);
    scale = std::min(scale, 4.0f);
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x205E, 0x2190, 0x2193, 0};
    auto* atlas = new ImFontAtlas();  // for the life of the process
    atlas->Flags |= ImFontAtlasFlags_NoMouseCursors;
    ImFont* font = nullptr;
    const std::string face = bodplat::SystemUiFontPath();
    if (!face.empty())
        font = atlas->AddFontFromFileTTF(face.c_str(), std::round(16.0f * scale), nullptr, ranges);
    if (font == nullptr) {
        scale = std::max(1.0f, std::round(scale));
        ImFontConfig pixel;
        pixel.SizePixels = 13.0f * scale;
        font = atlas->AddFontDefault(&pixel);
    }
    // The menu's faces: RocknRoll One (OFL, assets/fonts), the free rounded face nearest The Wind Waker's own.
    {
        const std::filesystem::path rr = bodplat::ExeFolder() / "assets" / "fonts" / "RocknRollOne-Regular.ttf";
        std::error_code ec;
        if (std::filesystem::exists(rr, ec)) {
            const std::string p = rr.string();
            ImFont* body = atlas->AddFontFromFileTTF(p.c_str(), std::round(17.0f * scale), nullptr, ranges);
            ImFont* heading = atlas->AddFontFromFileTTF(p.c_str(), std::round(19.0f * scale), nullptr, ranges);
            ImFont* title = atlas->AddFontFromFileTTF(p.c_str(), std::round(24.0f * scale), nullptr, ranges);
            shipmenu::SetFonts(body, heading, title);
        }
    }
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        std::fprintf(stderr, "[windows] the UI font did not build; keeping ImGui's\n");
        return;
    }
    atlas->SetTexID(aurora_imgui_add_texture(static_cast<uint32_t>(width), static_cast<uint32_t>(height), pixels));
    atlas->ClearTexData();
    ImGui::GetIO().FontDefault = font;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 4.0f;
    style.ItemSpacing = ImVec2(8.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ScaleAllSizes(scale);
    g_font_scale = scale;
    std::fprintf(stderr, "[windows] UI scale %.2f (font atlas %dx%d)\n", scale, width, height);
}

// --- replacement textures that follow their settings -------------------------------
// The Swift Sail's red textures (with Swift Sail or Brisk Sail) and the texture
// packs (Enable Texture Packs) are registered with the renderer while their
// setting is on and taken out when it is off. Registering bumps the renderer's
// replacement epoch, so the textures already decoded are looked up again. A
// folder is read on a thread of its own (a big pack has thousands of files),
// and the game goes on meanwhile.
struct LiveTextureSet {
    struct Job {
        std::atomic<bool> done{false};
        aurora::texture::ReplacementGroup group;
    };
    const char* kind;            // for the log
    int32_t priority;
    std::shared_ptr<Job> job;    // reading the folder
    bool registered = false;
    std::string folder;          // the folder being read or registered
    aurora::texture::ReplacementGroup group;
};

// want_folder is empty when the set should not be registered.
void sync_texture_set(LiveTextureSet& set, const std::string& want_folder) {
    if (set.job) {
        if (!set.job->done.load(std::memory_order_acquire))
            return;
        set.group = std::move(set.job->group);
        set.job.reset();
        set.registered = true;
        std::fprintf(stderr, "[mods] %s=%s replacements=%zu\n", set.kind, set.folder.c_str(),
                     set.group.registrations.size());
    }
    if (set.registered && set.folder != want_folder) {
        const size_t count = set.group.registrations.size();
        aurora::texture::unregister_replacements(set.group);
        set.group = {};
        set.registered = false;
        std::fprintf(stderr, "[mods] %s off: %zu replacements removed\n", set.kind, count);
    }
    if (!set.registered && !set.job && !want_folder.empty()) {
        set.folder = want_folder;
        set.job = std::make_shared<LiveTextureSet::Job>();
        std::fprintf(stderr, "[mods] %s: reading %s\n", set.kind, want_folder.c_str());
        std::thread([job = set.job, folder = want_folder, priority = set.priority] {
            job->group = aurora::texture::load_replacement_directory(
                std::filesystem::path(folder), aurora::texture::ReplacementOptions{.priority = priority});
            job->done.store(true, std::memory_order_release);
        }).detach();
    }
}

bool game_option_on(const char* name) {
    for (uint32_t i = 0;; ++i) {
        bool on = false;
        const char* option = bluewake_game_options_describe(i, nullptr, nullptr, &on);
        if (option == nullptr)
            return false;
        if (std::strcmp(option, name) == 0)
            return on;
    }
}

std::string swift_sail_folder() {
    std::string sail = exe_folder() + "assets" BOD_SEP "swift_sail";
    // The release makes them from the player's disc on first launch, into the prepared game folder.
    if (const char* made = std::getenv("BLUEWAKE_EXTRAS_DIR")) {
        const std::string prepared = std::string(made) + BOD_SEP "assets" BOD_SEP "swift_sail";
        std::error_code exists_ec;
        if (std::filesystem::exists(std::filesystem::u8path(prepared), exists_ec))
            sail = prepared;
    }
    return sail;
}

void sync_live_textures() {
    static LiveTextureSet sail{"builtin-textures", -1};  // below a texture pack, so a pack's texture wins
    static LiveTextureSet pack{"texture-pack", 0};
    static bool sail_missing_reported;
    std::string sail_folder;
    // What runs is the option's own switch (set from the setting, or by the command line).
    if (game_option_on("swift_sail") || game_option_on("brisk_sail")) {
        const std::string folder = swift_sail_folder();
        std::error_code folder_ec;
        if (std::filesystem::exists(std::filesystem::u8path(folder), folder_ec))
            sail_folder = folder;
        else if (!sail_missing_reported) {
            sail_missing_reported = true;
            std::fprintf(stderr, "[windows] Swift Sail textures not found (%s); the sail keeps its colours\n",
                         folder.c_str());
        }
    }
    sync_texture_set(sail, sail_folder);
    const bool packs = !g_safe_mode && CVarGetInteger(CVAR_SETTING("Mods.TexturePacks"), 0) != 0;
    sync_texture_set(pack, packs ? texture_folder() : std::string());
}

// Precompile Shaders. The renderer compiles the pipelines its cache lists in
// the background from the start; with the setting on, the game is also held
// at its first present until they are done (below). Turning it on while the
// game runs starts the same watch now, without holding anything: a counter in
// the corner counts the pipelines still compiling.
bool g_warmup;
Uint64 g_warmup_since;
uint32_t g_warmup_first;

void start_shader_warmup() {
    if (!CVarGetInteger(CVAR_SETTING("PrecompileShaders"), 0)) {
        g_warmup = false;
        std::fprintf(stderr, "[windows] precompile shaders off\n");
        return;
    }
    const AuroraStats* stats = aurora_get_stats();
    const uint32_t left = stats != nullptr ? stats->queuedPipelines : 0u;
    std::fprintf(stderr, "[windows] precompile shaders: warm-up started in the background, %u shaders to compile\n",
                 left);
    if (left == 0u) {
        std::fprintf(stderr, "[windows] shaders compiled in background: none were waiting\n");
        return;
    }
    g_warmup = true;
    g_warmup_since = SDL_GetTicks();
    g_warmup_first = left;
}

// Once a frame: ends the warm-up when nothing is left (or after two minutes).
void draw_shader_warmup(SDL_Window* w) {
    if (!g_warmup)
        return;
    const AuroraStats* stats = aurora_get_stats();
    const uint32_t left = stats != nullptr ? stats->queuedPipelines : 0u;
    const Uint64 waited = SDL_GetTicks() - g_warmup_since;
    if (!CVarGetInteger(CVAR_SETTING("PrecompileShaders"), 0) || left == 0u || waited > 120000) {
        g_warmup = false;
        std::fprintf(stderr, "[windows] shaders compiled in background: %u in %.1f s%s\n", g_warmup_first,
                     waited / 1000.0, left != 0u ? " (stopped waiting)" : "");
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    const float scale = ui_scale(w);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12 * scale, 12 * scale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##shaderwarmup", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale / g_font_scale);
        ImGui::Text("Compiling shaders: %u to go", left);
    }
    ImGui::End();
}

// Precompile Shaders (at start): the game is held at its first present (dol_aurora_set_hold,
// the picture redrawn under this overlay) until every pipeline the cache queued
// at start is compiled, or two minutes have passed.
bool g_shader_wait;
Uint64 g_shader_wait_since;
uint32_t g_shader_wait_first;

bool shader_wait_hold(void*) {
    if (!g_shader_wait)
        return false;
    const AuroraStats* stats = aurora_get_stats();
    const uint32_t left = stats != nullptr ? stats->queuedPipelines : 0u;
    if (g_shader_wait_since == 0) {
        g_shader_wait_since = SDL_GetTicks();
        g_shader_wait_first = left;
    }
    const Uint64 waited = SDL_GetTicks() - g_shader_wait_since;
    if (left == 0u || waited > 120000) {
        g_shader_wait = false;
        std::fprintf(stderr, "[windows] shaders compiled before play: %u in %.1f s%s\n", g_shader_wait_first,
                     waited / 1000.0, left != 0u ? " (stopped waiting)" : "");
        return false;
    }
    return true;
}

void draw_shader_wait(SDL_Window* w) {
    if (!g_shader_wait)
        return;
    const AuroraStats* stats = aurora_get_stats();
    const uint32_t left = stats != nullptr ? stats->queuedPipelines : 0u;
    ImGuiIO& io = ImGui::GetIO();
    const float scale = ui_scale(w);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##shaders", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale / g_font_scale);
        ImGui::Text("Compiling shaders: %u to go", left);
        if (g_shader_wait_first > 0u)
            ImGui::ProgressBar(1.f - static_cast<float>(left) / static_cast<float>(g_shader_wait_first),
                               ImVec2(260.f * scale, 0.f), "");
    }
    ImGui::End();
}

// For the first few seconds: how to open the menu.
void draw_hint(SDL_Window* w) {
    const Uint64 shown = SDL_GetTicks() - g_first_frame_at;
    if (shown > 7000 || g_menu_open)
        return;
    ImGuiIO& io = ImGui::GetIO();
    const float scale = ui_scale(w);
    ImGui::SetNextWindowPos(ImVec2(12 * scale, io.DisplaySize.y - 12 * scale), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(shown > 6000 ? 0.6f * (7000 - shown) / 1000.0f : 0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##hint", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale / g_font_scale);
        ImGui::TextUnformatted("Press Esc or F1 for the menu");
    }
    ImGui::End();
}

// Testing: BLUEWAKE_TEST_SET="<frame>:<cvar>=<value>,..." changes settings as
// the menu would, that many frames after the first, running the widget's own
// callback, so a scenario can see a setting take effect in a running game. A
// value that is not a number sets a string CVar (the menu's page, say). Two
// names are not settings: gSettings.Presets.Apply applies the preset numbered
// by the value, and gTest.Menu opens (1) or closes (0) the menu.
void apply_test_changes() {
    struct Change {
        unsigned frame;
        std::string cvar;
        int value;
        std::string text;
        bool is_text;
        bool done;
    };
    static std::vector<Change> changes;
    static bool parsed;
    static unsigned frames;
    if (!parsed) {
        parsed = true;
        const char* list = std::getenv("BLUEWAKE_TEST_SET");
        for (const char* p = list; p != nullptr && *p != '\0';) {
            char* colon = nullptr;
            const unsigned long frame = std::strtoul(p, &colon, 10);
            const char* eq = colon != nullptr && *colon == ':' ? std::strchr(colon, '=') : nullptr;
            if (eq == nullptr)
                break;
            const char* stop = std::strchr(eq, ',');
            const std::string value(eq + 1, stop != nullptr ? stop : eq + 1 + std::strlen(eq + 1));
            char* end = nullptr;
            const long number = std::strtol(value.c_str(), &end, 10);
            const bool is_text = value.empty() || *end != '\0';
            changes.push_back({static_cast<unsigned>(frame), std::string(static_cast<const char*>(colon + 1), eq),
                               static_cast<int>(number), value, is_text, false});
            p = stop != nullptr ? stop + 1 : "";
        }
    }
    if (changes.empty())
        return;
    ++frames;
    for (auto& change : changes) {
        if (change.done || frames < change.frame)
            continue;
        change.done = true;
        build_menu();
        if (change.cvar == "gSettings.Presets.Apply") {
            apply_preset(change.value);
            continue;
        }
        if (change.cvar == "gTest.CosmeticsPreset") {
            cosmetics::TestApplyPreset(change.text);
            continue;
        }
        if (change.cvar == "gTest.Menu") {
            set_menu_open(change.value != 0);
            continue;
        }
        if (change.is_text) {
            CVarSetString(change.cvar.c_str(), change.text.c_str());
            changed();
            std::fprintf(stderr, "[test] set %s=%s at frame %u\n", change.cvar.c_str(), change.text.c_str(), frames);
            continue;
        }
        CVarSetInteger(change.cvar.c_str(), change.value);
        changed();
        const bool fired = g_menu.FireCallback(change.cvar);
        std::fprintf(stderr, "[test] set %s=%d at frame %u (callback %s)\n", change.cvar.c_str(), change.value, frames,
                     fired ? "ran" : "none");
    }
}

// Every presented frame, on the main thread, inside Aurora's frame.
void frame(void*) {
    SDL_Window* w = game_window();
    if (w == nullptr)
        return;
    // Allow Background Inputs off: no controller input reaches the game while the window has no focus.
    bluewake_host_set_pad_unfocused_block(!CVarGetInteger(CVAR_SETTING("AllowBackgroundInputs"), 1) &&
                                          (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) == 0);
    if (g_first_frame_at == 0) {
        g_first_frame_at = SDL_GetTicks();
        profile_hardware(w, false);
        apply_live(w);
        adapter_status(); // logs the GameCube adapter's state
        input_editor::ApplyKeyboard(false);
        input_editor::TestSetup();
        if (std::getenv("BLUEWAKE_TEST_ADAPTER_DRY")) // builds the package unsigned and never installs
            std::fprintf(stderr, "[test] adapter driver dry run: %s\n", adapter_driver::DryRun().c_str());
        online::Launch();
        randomizer::Launch();
        load_font(w);
        return;  // the font is ImGui's default from the next frame
    }
    if (!g_placed) {
        g_placed = true;
        place_window(w);
    }
    apply_test_changes();
    bluewake_text_swap_set_female(CVarGetInteger("gCosmetics.PlayerGender", 0) != 0); // Cosmetics Editor > Link > Player Gender (live; also a test's BLUEWAKE_TEST_SET)
    if (g_toggle_fullscreen) {
        g_toggle_fullscreen = false;
        set_fullscreen(w, !is_fullscreen(w));
    }
    {
        // Testing: BLUEWAKE_TEST_MENU="Settings/Browse Models" opens the menu on that page once the game shows its first
        // frames (BLUEWAKE_TEST_MENU_STYLE=0/1: the Wind Waker or the Classic theme), for captures (the scenarios cannot click).
        static const char* const test_menu = std::getenv("BLUEWAKE_TEST_MENU");
        static int test_frames;
        if (test_menu != nullptr && ++test_frames == 240) {
            const std::string spec = test_menu;
            const size_t slash = spec.find('/');
            const std::string header = spec.substr(0, slash);
            CVarSetString(CVAR_SETTING("Menu.ActiveHeader"), header.c_str());
            if (slash != std::string::npos) {
                std::string key;
                for (const char c : header)
                    if (std::isalnum(static_cast<unsigned char>(c))) key += c;
                CVarSetString(("gSettings.Menu." + key + "SidebarSection").c_str(), spec.substr(slash + 1).c_str());
            }
            if (const char* style = std::getenv("BLUEWAKE_TEST_MENU_STYLE"))
                CVarSetInteger(CVAR_SETTING("Menu.Style"), std::atoi(style));
            set_menu_open(true);
        }
    }
    if (g_toggle_menu) {
        g_toggle_menu = false;
        if (online::ChatOverlayOpen())
            online::CloseChatOverlay();  // Esc (or F1) closes the chat line; the menu stays shut
        else
            set_menu_open(!g_menu_open);
    }
    track_window(w);
    bw_crash_heartbeat();  // the hang watchdog (win_crash.cpp)
    {
        // A console reset the game asked for (OSResetSystem, game_options.c): started again, as the console reboots.
        static bool restarting;
        if (!restarting && bluewake_host_reset_requested()) {
            restarting = true;
            restart();
        }
    }
    {
        // BLUEWAKE_TEST_HANG_UI=<seconds> (a test of the watchdog): the UI thread stops for 15 s that long after start.
        static const char* hang = std::getenv("BLUEWAKE_TEST_HANG_UI");
        static bool hung;
        if (hang != nullptr && !hung && SDL_GetTicks() >= 1000ull * (Uint64)std::atoi(hang)) {
            hung = true;
            std::fprintf(stderr, "[test] the UI thread stops for 15 s\n");
            SDL_Delay(15000);
        }
    }
    // Internal Resolution follows the window's size.
    static int pixel_height;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    if (ph != pixel_height) {
        pixel_height = ph;
        apply_resolution(w);
    }
    // The frame rate in the session log, a line a second beside the host's
    // [perf] line, with where the game thread waited in that second.
    static Uint64 fps_logged;
    static DolAuroraFrameTiming timing_before;
    const Uint64 now = SDL_GetTicks();
    if (now - fps_logged >= 1000) {
        DolAuroraFrameTiming timing{};
        dol_aurora_frame_timing(&timing);
        // Audio counters for the same second (reading them also resets them).
        DolAuroraAudioStats audio{};
        dol_aurora_audio_interval(&audio);
        if (fps_logged != 0)
            std::fprintf(stderr, "[fps] shown=%.1f game=%.1f drain_ms=%.0f present_ms=%.0f end_frame_ms=%.0f"
                         " audio_q_ms=%d..%d audio_underruns=%u audio_silent_ms=%u audio_starved=%u"
                         " audio_gap_ms=%u audio_stretch=%.2f\n",
                         aurora::gfx::calculate_fps(), aurora::gfx::calculate_game_fps(),
                         (timing.drain_us - timing_before.drain_us) / 1000.0,
                         (timing.present_us - timing_before.present_us) / 1000.0,
                         (timing.end_frame_us - timing_before.end_frame_us) / 1000.0,
                         audio.queue_min_ms, audio.queue_max_ms, audio.underruns, audio.underrun_ms,
                         audio.starved, audio.max_gap_ms, audio.stretch_x100 / 100.0);
        fps_logged = now;
        timing_before = timing;
    }
    // A controller that connects or leaves: its camera inversion and the port rule.
    static unsigned frames;
    if ((++frames % 30u) == 0u)
        input_editor::Poll();
    static unsigned healthy_frames;
    if (!g_menu_open && ++healthy_frames == 600u && !bw_launch_clear((g_data_dir + "launch.pending").c_str()))
        std::fprintf(stderr, "[safe-mode] could not clear launch marker\n");
    if (g_safe_mode && SDL_GetTicks() - g_first_frame_at < 15000) {
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("Launch recovery", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted("The game didn't finish starting last time, so this time it starts with HLE audio, "
                               "no enhancements and no texture packs. Your settings are "
                               "unchanged.");
        ImGui::PopTextWrapPos();
        ImGui::End();
    }
    draw_shader_wait(w);
    draw_shader_warmup(w);
    sync_live_textures();
    // The menu slides down when it opens and back up when it closes: drawn until it has gone.
    static float menu_in = 0.f;
    const float menu_dt = ImGui::GetIO().DeltaTime;
    menu_in = g_menu_open ? std::min(1.f, menu_in + menu_dt / 0.34f) : std::max(0.f, menu_in - menu_dt / 0.22f);
    if (g_menu_open || menu_in > 0.f) {
        build_menu();
        bool open = true;
        const float scale = ui_scale(w);
        g_menu.Draw(&open, scale, scale / g_font_scale, menu_in, !g_menu_open);
        if (!open && g_menu_open)
            set_menu_open(false);
    }
    {
        const float scale = ui_scale(w);
        cosmetics::Frame(scale, scale / g_font_scale);
        gbbrowser::Frame(scale, scale / g_font_scale);
        hud_colors::Frame();
        online::Frame(scale, scale / g_font_scale);
        if (online::ChatOverlayOpen() != g_text_capture) {
            g_text_capture = online::ChatOverlayOpen();
            PADBlockInput(g_menu_open || g_text_capture);
            input_editor::ApplyKeyboard(g_menu_open || g_text_capture);
            std::fprintf(stderr, "[windows] chat line %s, game input %s\n", g_text_capture ? "open" : "closed",
                         g_text_capture ? "blocked" : "restored");
        }
        input_editor::Frame(scale, scale / g_font_scale);
        gba_window::Frame(scale, scale / g_font_scale);
        devtools::Frame(scale, scale / g_font_scale);
        itemtracker::Frame(scale, scale / g_font_scale);
        checktracker::Frame(scale, scale / g_font_scale);
        inputviewer::Frame(scale, scale / g_font_scale);
        randoflow::Frame(scale);
        poll_autosave();
    }
    draw_hint(w);
    CVarTick();
}

void save_at_exit() {
    online::Shutdown();
    CVarSave();
}

}  // namespace

extern "C" void bw_settings_capture_environment(void) { bodplat::CaptureEnvironment(); }

extern "C" void bw_settings_load(const char* data_dir) {
    g_data_dir = data_dir;
    CVarInit((g_data_dir + "boat-of-daphnes.json").c_str());
    // Remove Music became Music Volume 0 (once): a saved Remove Music file keeps its silence.
    if (CVarExists(CVAR_SETTING("Audio.RemoveMusic"))) {
        if (CVarGetInteger(CVAR_SETTING("Audio.RemoveMusic"), 0) != 0 && !CVarExists(CVAR_SETTING("Audio.MusicVolume")))
            CVarSetInteger(CVAR_SETTING("Audio.MusicVolume"), 0);
        CVarClear(CVAR_SETTING("Audio.RemoveMusic"));
        CVarSaveNextFrame();
        std::fprintf(stderr, "[settings] Remove Music is now Music Volume (%d)\n",
                     CVarGetInteger(CVAR_SETTING("Audio.MusicVolume"), 100));
    }
    g_safe_mode = env_set("BLUEWAKE_SAFE_MODE") || bw_launch_pending((g_data_dir + "launch.pending").c_str());
    if (g_safe_mode) {
        // This session only: the settings file is left as it is.
        bodplat::SetEnv("BLUEWAKE_DSP_MODE", "hle");
        bodplat::SetEnv("BLUEWAKE_MODS", "none");
        bodplat::SetEnv("BLUEWAKE_OPTIONS", "none");
        bodplat::SetEnv("BLUEWAKE_FAST_FORWARD", "0");
        bodplat::SetEnv("BLUEWAKE_FADE_FRAMES", "0");
        bodplat::SetEnv("BLUEWAKE_QUICK_DOORS", "0");
        std::fprintf(stderr, "[safe-mode] recovering a pending launch; HLE audio and texture packs off\n");
    }
}

extern "C" void bw_settings_apply_launch(void) {
    // Enhancements the host reads once.
    const bool fast = CVarGetInteger(CVAR_ENHANCEMENT("FasterTransitions"), 0) != 0;
    env_default("BLUEWAKE_FAST_FORWARD", fast ? "1" : "0");
    env_default("BLUEWAKE_FADE_FRAMES", fast ? "6" : "0");
    env_default("BLUEWAKE_QUICK_DOORS", CVarGetInteger(CVAR_ENHANCEMENT("QuickDoors"), 0) ? "1" : "0");
    // The option-site variant runs with each switch from its own setting: no
    // Better Wind Waker switch is on unless chosen (vanilla by default), the
    // port's own options as set.
    env_default("BLUEWAKE_MODS", "betterww");
    // Graphics > Renderer: the graphics API Aurora starts with (read once, at start).
    static const char* const kBackends[] = {"auto", "d3d12", "vulkan", "d3d11"};
    env_default("DOL_AURORA_BACKEND", kBackends[std::clamp(CVarGetInteger(CVAR_SETTING("Graphics.Backend"), 0), 0, 3)]);
    // A randomizer session (Quest: Randomizer, chosen on the file select): its mod, card and seed pack.
    randoflow::Launch(g_data_dir.c_str());
    std::string options = "none";
    for (const auto& row : kBetterWw)
        if (CVarGetInteger(better_ww_cvar(row.name).c_str(), row.def))
            options += std::string(",") + row.name;
    if (CVarGetInteger(CVAR_ENHANCEMENT("WidescreenActorCulling"), 1))
        options += ",widescreen_culling";
    if (CVarGetInteger(CVAR_ENHANCEMENT("HudAtScreenEdges"), 1))
        options += ",hud_edges";
    options += ",treasure_glow"; // its hook does nothing until a mode is chosen
    options += ",player_models"; // its hook does nothing until a model is applied
    options += ",intro_skip_start"; // its switch is the menu's (live)
    if (CVarGetInteger(CVAR_ENHANCEMENT("InvertSeaCompassX"), 0))
        options += ",invert_sea_compass_x";
    options += ",anchor_puppets"; // its hooks do nothing until Anchor is on (or BLUEWAKE_PUPPET_SELF)
    options += ",anchor_pvp"; // other players' bodies, targeting and hits (puppet_pvp.c); idle until a puppet is drawn; a module built before it has no such option
    options += ",rando_file_select"; // the randomizer's file-select sounds (a module built before it has no such option)
    options += ",gx_array_source"; // the renderer's full array pointers (the cloth's static arrays); a module without it has no such option
    options += ",link_voice_pitch"; // Cosmetics Editor > Link Voice Pitch (its hook; a module built before it has no such option)
    options += ",native_search"; // fopAcM_SearchByID answered natively (no setting; a module built before it has no such option)
    options += ",anchor_switch_sync"; // Anchor: switches shared live (on only while team sync is; switch_sync.c)
    options += ",runtime_fixes"; // DVDCheckDisk: a disc is in (the three-button reset)
    bluewake_bunny_hood_enable(CVarGetInteger("gExperimental.BunnyHood", 1) != 0); // on by default (11c)
    bluewake_text_swap_enable(CVarGetInteger(CVAR_ENHANCEMENT("Crenando"), 0) != 0);
    bluewake_text_swap_set_female(CVarGetInteger("gCosmetics.PlayerGender", 0) != 0);
    options += ",bunny_hood"; // the Bunny Hood's hooks (docs/fork/bunny-hood.md); they do nothing until the hood is owned; a module without it has no such option
    bluewake_intro_skip_set(CVarGetInteger(CVAR_ENHANCEMENT("SkipIntroWithStart"), 1) != 0);
    apply_preferences(); // before the game starts: a randomizer session applies its seed at boot
    env_default("BLUEWAKE_TREASURE_GLOW", std::to_string(CVarGetInteger(CVAR_ENHANCEMENT("TreasureLightUpClose"), 0)));
    env_default("BLUEWAKE_OPTIONS", options);
    // The session's settings in one line, for reading a log.
    std::fprintf(stderr,
                 "[settings] options=%s interpolation_fps=%d internal_resolution=%d%% force_aspect=%d vsync=%d "
                 "anisotropy=%d resampling=%d treasure_glow=%d anchor=%d mods=%d texture_packs=%d audio_mode=%d\n",
                 options.c_str(), CVarGetInteger(CVAR_SETTING("Graphics.InterpolationFPS"), 30),
                 CVarGetInteger(CVAR_SETTING("Graphics.InternalResolution"), 100),
                 CVarGetInteger(CVAR_SETTING("Graphics.ForceAspectRatio"), 0),
                 CVarGetInteger(CVAR_SETTING("Graphics.Vsync"), 1), CVarGetInteger(CVAR_SETTING("Graphics.Anisotropy"), 0),
                 CVarGetInteger(CVAR_SETTING("Graphics.OutputResampling"), 1),
                 CVarGetInteger(CVAR_ENHANCEMENT("TreasureLightUpClose"), 0),
                 CVarGetInteger("gNetwork.Anchor.Enabled", 0), CVarGetInteger("gSettings.Mods.Enabled", 1),
                 CVarGetInteger(CVAR_SETTING("Mods.TexturePacks"), 0), CVarGetInteger(CVAR_SETTING("Audio.Mode"), 0));
    // The Swift Sail's red textures and the texture packs are registered while their settings are on
    // (sync_live_textures, every frame).
    // The Tingle Tuner's in-app GBA (the host reads it at boot; the menu switches it live).
    env_default("BLUEWAKE_TINGLE_TUNER", CVarGetInteger(CVAR_SETTING("TingleTuner.Mode"), 1) == 1 ? "auto" : "off");
    // Rumble and Forest Water (read at attach, then live).
    env_default("BLUEWAKE_HAPTICS", rumble_name(CVarGetInteger(CVAR_SETTING("Controls.Rumble.Mode"), 1)));
    env_default("BLUEWAKE_HAPTICS_STRENGTH", std::to_string(CVarGetInteger(CVAR_SETTING("Controls.Rumble.Strength"), 80)));
    env_default("BLUEWAKE_HAPTICS_TRIGGERS", CVarGetInteger(CVAR_SETTING("Controls.Rumble.Triggers"), 1) ? "1" : "0");
    env_default("BLUEWAKE_FOREST_WATER_KEEP_TREES", CVarGetInteger(CVAR_ENHANCEMENT("KeepWateredTrees"), 0) ? "1" : "0");
    env_default("BLUEWAKE_FOREST_WATER_30_MINUTES",
                CVarGetInteger(CVAR_ENHANCEMENT("LongerForestWaterTimer"), 0) ? "1" : "0");
    // Texture packs, audio.
    std::error_code mods_ec;
    std::filesystem::create_directory(std::filesystem::u8path(g_data_dir + "mods"), mods_ec);
    std::filesystem::create_directory(std::filesystem::u8path(texture_folder()), mods_ec);
    // BLUEWAKE_MODS_DIR (testing): another folder in place of mods (read only).
    if (const char* dir = std::getenv("BLUEWAKE_MODS_DIR"); dir != nullptr && dir[0] != '\0')
        mods::Launch(dir, true);
    else
        mods::Launch(g_data_dir + "mods", false);
    if (CVarGetInteger(CVAR_SETTING("Audio.Mode"), 0) == 1)
        env_default("BLUEWAKE_DSP_MODE", "lle");
    // The window and the picture.
    if (CVarGetInteger(CVAR_SETTING("Graphics.Fullscreen"), 0))
        env_default("DOL_AURORA_FULLSCREEN", "1");
    int width = CVarGetInteger(CVAR_SETTING("Window.Width"), 0), height = CVarGetInteger(CVAR_SETTING("Window.Height"), 0);
    if (width < 320 || height < 240)
        default_window(&width, &height);
    env_default("DOL_AURORA_WINDOW", std::to_string(width) + "x" + std::to_string(height));
    char scale[32];
    std::snprintf(scale, sizeof scale, "%.4f", render_scale_for(height));
    env_default("DOL_AURORA_RENDER_SCALE", scale);
    const int aniso = CVarGetInteger(CVAR_SETTING("Graphics.Anisotropy"), 0);
    if (aniso > 0)
        env_default("DOL_AURORA_FORCE_ANISO", std::to_string(kAnisotropy[std::clamp(aniso, 0, 4)]));
    if (!env_set("DOL_AURORA_SHOW_FPS"))
        aurora_set_fps_overlay(CVarGetInteger(CVAR_SETTING("Graphics.ShowFPS"), 0) != 0);
}

extern "C" void bw_settings_install(void) {
    dol_aurora_set_overlay(frame, nullptr);
    // BLUEWAKE_SHADERS_FIRST=0/1 overrides the setting (testing).
    const char* first = std::getenv("BLUEWAKE_SHADERS_FIRST");
    g_shader_wait = first != nullptr && first[0] != '\0' ? first[0] != '0'
                                                         : CVarGetInteger(CVAR_SETTING("PrecompileShaders"), 0) != 0;
    if (g_shader_wait) {
        dol_aurora_set_hold(shader_wait_hold, nullptr);
        dol_aurora_set_hold_redraw(true);
    }
    std::atexit(save_at_exit);
}

// The Windows virtual-key numbers bw_settings_key takes (windows.h has them; on Linux the keys come from SDL's events, below).
#ifndef VK_ESCAPE
enum { VK_ESCAPE = 0x1B, VK_RETURN = 0x0D, VK_F1 = 0x70, VK_F4 = 0x73, VK_F6 = 0x75, VK_F7 = 0x76, VK_F8 = 0x77, VK_F9 = 0x78, VK_F11 = 0x7A };
#endif

#ifndef _WIN32
static unsigned virtual_key_of(SDL_Keycode key) {
    switch (key) {
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_RETURN: return VK_RETURN;
    case SDLK_F1: return VK_F1;
    case SDLK_F4: return VK_F4;
    case SDLK_F6: return VK_F6;
    case SDLK_F7: return VK_F7;
    case SDLK_F8: return VK_F8;
    case SDLK_F9: return VK_F9;
    case SDLK_F11: return VK_F11;
    default: return 0;
    }
}
#endif

extern "C" int bw_settings_key(unsigned virtual_key, int alt) {
    switch (virtual_key) {
    case VK_F1:
    case VK_ESCAPE:
        g_toggle_menu = true;
        return 1;
    case VK_F11:
        g_toggle_fullscreen = true;
        return 1;
    case VK_RETURN:
        if (!alt)
            return 0;
        g_toggle_fullscreen = true;
        return 1;
    case VK_F6:
    case VK_F8:
        if (!CVarGetInteger(CVAR_CHEAT("SaveStates"), 0))
            return 0;
        bluewake_save_state_hotkey(virtual_key == VK_F8);
        return 1;
    case VK_F7:
        itemtracker::Toggle();
        return 1;
    case VK_F4:
        if (alt)
            return 0; // Alt+F4 closes the window
        checktracker::Toggle();
        return 1;
    case VK_F9: {
        const int on = CVarGetInteger(CVAR_SETTING("Graphics.ShowFPS"), 0) ? 0 : 1;
        CVarSetInteger(CVAR_SETTING("Graphics.ShowFPS"), on);
        aurora_set_fps_overlay(on != 0);
        changed();
        return 1;
    }
    default:
        return 0;
    }
}

extern "C" void bw_settings_restart_now(void) { restart(); }

extern "C" void bw_settings_open_menu_at(const char* header, const char* page) {
    std::string key;
    for (const char* c = header; *c != 0; ++c)
        if (std::isalnum(static_cast<unsigned char>(*c)))
            key += *c;
    CVarSetString("gSettings.Menu.ActiveHeader", header);
    CVarSetString(("gSettings.Menu." + key + "SidebarSection").c_str(), page);
    set_menu_open(true);
}

extern "C" bool bw_settings_menu_is_open(void) { return g_menu_open; }

extern "C" int bw_settings_relaunch(void) {
    return !g_restart.take() || relaunch() ? 0 : 1;
}

extern "C" bool bluewake_settings_menu_event(const void* sdl_event) {
    const SDL_Event* event = static_cast<const SDL_Event*>(sdl_event);
    if (input_editor::OnEvent(event))
        return true;
    if (event->type == SDL_EVENT_DROP_FILE || event->type == SDL_EVENT_DROP_TEXT) {
        // A spoiler log or seed.json on the window: the randomizer file select takes it as the seed (rando_flow.cpp).
        // Under Proton/Wine the drop may arrive as text (a file:// URI list) or as a Unix path (drop_path.h).
#ifdef _WIN32
        const bool dos = true;
#else
        const bool dos = false;
#endif
        const char* data = event->drop.data != nullptr ? event->drop.data : "";
        const std::string path = droppath::to_path(data, dos);
        std::fprintf(stderr, "[drop] %s: %s%s%s\n", event->type == SDL_EVENT_DROP_FILE ? "file" : "text", data,
                     path.empty() ? " (no path in it)" : " -> ", path.c_str());
        if (!path.empty())
            randoflow::OnDrop(path.c_str());
        return true;
    }
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
        set_menu_open(!g_menu_open);
        return true;
    }
#ifndef _WIN32
    // The hotkeys (Windows sees them in a keyboard hook before SDL, win_entry.c).
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        const unsigned key = virtual_key_of(event->key.key);
        if (key != 0 && bw_settings_key(key, (event->key.mod & SDL_KMOD_ALT) != 0))
            return true;
    }
#endif
    return g_menu_open || g_text_capture;
}
