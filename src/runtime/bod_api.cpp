// Boat of Daphnes' host API (its RecompCore's and its Aurora fork's, as its menus and windows call
// them), built against its own headers. What this runtime has is passed on (bod_bridge.h); what it
// does not have yet answers as a host without it would, so the menus stay as Boat of Daphnes has them.
#include "runtime/bod_bridge.h"

#include "../bod/shim/bod_compat.h"
#include "gc_adapter.h"
#include "gxruntime/aurora_backend.h"

#include <dolphin/pad.h>

namespace bod = rt::bod;

namespace {
DolAuroraHoldFn s_hold = nullptr;
void* s_holdUser = nullptr;
std::atomic<bool> s_holdRedraw{false};
}  // namespace

extern "C" {
// ---- its Aurora fork ----
void dol_aurora_set_overlay(DolAuroraOverlayFn draw, void* user) { bod::setOverlay(draw, user); }
// (frame buffer scale and aspect, integer scaling, forced anisotropy: this runtime's Aurora, extern/aurora, has them)
void aurora_set_present_suppressed(bool suppressed) { bod::g_presentSuppressed = suppressed; }
void aurora_set_frame_interpolation(bool enabled) { bod::g_interp = enabled; }
bool aurora_get_frame_interpolation(void) { return bod::g_interp; }
void aurora_set_frame_interp_steps(int steps) { bod::g_interpSteps = steps; }
int aurora_get_frame_interp_steps(void) { return bod::g_interpSteps; }
void aurora_set_fps_overlay(bool enabled) { bod::g_fpsOverlay = enabled; }
unsigned long long aurora_get_shown_frames(void) { return bod::g_shown; }
void aurora_get_frame_interp_totals(AuroraFrameInterpTotals* out) {
    if (out) *out = AuroraFrameInterpTotals{};
}
void dol_aurora_set_hold(DolAuroraHoldFn should_hold, void* user) {
    s_holdUser = user;
    s_hold = should_hold;
}
void dol_aurora_set_hold_redraw(bool redraw) { s_holdRedraw = redraw; }
void dol_aurora_frame_timing(DolAuroraFrameTiming* out) {
    if (!out) return;
    *out = DolAuroraFrameTiming{};
    out->presents = out->shown = bod::g_shown;
}
void dol_aurora_audio_interval(DolAuroraAudioStats* out) {
    if (out) *out = DolAuroraAudioStats{};
}
void dol_aurora_audio_set_master_volume(float volume) { bod::g_masterVolume = volume; }

// ---- its host: what is not ported yet answers as a host without it would ----
void bluewake_save_state_hotkey(bool) {}
bool bluewake_game_options_set(const char* name, bool on) { return bod::optionSet(name, on); }
void bluewake_intro_skip_set(bool on) { bod::introSkipSet(on); }
}

namespace aurora::gfx {
float calculate_game_fps() noexcept { return rt::bod::g_gameFps; }
}  // namespace aurora::gfx

// ---- its runtime (GXRuntime's CPU support): guest memory as its CPUState sees it ----
#include "core/cpu.h"
#include "cosmetics.h"
#include "game_state.h"
#include "ship/cvar.h"
extern "C" {  // (C headers without their own guards)
#include "haptics.h"
#include "widescreen.h"
#include "fast_load.h"
#include "forest_water.h"
#include "quick_doors.h"
#include "treasure_glow.h"
#include "team_sync.h"
#include "puppet.h"
#include "player_model.h"
#include "puppet_pvp.h"
#include "bunny_hood.h"
#include "chart_reveal.h"
#include "autosave.h"
#include "voice_pitch.h"
#include "switch_sync.h"
#include "text_speed.h"
#include "text_swap.h"
#include "audio_volume.h"
#include "blank_thp.h"
#include "gxruntime/dvd.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
PPCMemWriteJournal g_mem_write_journal = nullptr;
void* g_mem_write_journal_user = nullptr;
bool g_ppc_guest_aliases_overlap_mem1 = false;
bool ppc_guest_alias_resolve(u32, u32, u8**, u32*) { return false; }

const char* bluewake_game_options_describe(uint32_t position, const char** title, bool* default_on, bool* on) {
    const char* name = nullptr;
    const char* t = nullptr;
    bool d = false, o = false;
    if (!bod::optionDescribe(position, &name, &t, &d, &o)) return nullptr;
    if (title) *title = t;
    if (default_on) *default_on = d;
    if (on) *on = o;
    return name;
}
bool bluewake_game_options_get(const char* name) {
    for (unsigned i = 0;; i++) {
        const char* n = nullptr;
        const char* t = nullptr;
        bool d = false, o = false;
        if (!bod::optionDescribe(i, &n, &t, &d, &o)) return false;
        if (!std::strcmp(n, name)) return o;
    }
}
unsigned long long bluewake_host_retrace_count(void) { return bod::retraceCount(); }
// Its Faster Transitions' fast-forward (fast_load.c): retraces without the wall clock (hle_hw.cpp), no presents.
void dol_aurora_set_fast_forward(bool on) {
    bod::g_fastForward = on;
    bod::g_presentSuppressed = on;
}
// Its platform's rumble (haptics.c, Enhanced mode): Aurora's, as its platform does.
void dol_platform_pad_control_motor(u32 channel, u32 command) {
    if (channel < 4) PADControlMotor(s32(channel), command);
}
float dol_aurora_widescreen_squeeze(void) { return 1.f; }  // (the community widescreen code here, not its renderer squeeze)
void dol_aurora_set_backdrop_source(uint32_t) {}
void dol_platform_gx_write(u64, u8) {}  // (its renderer's markers: this renderer has none to read)
// Its HUD-at-the-edges switch: off here - the community widescreen code places the HUD.
bool bluewake_game_options_hud_edges(void) { return false; }
void dol_aurora_set_backdrop_source2(uint32_t) {}  // (the same: the community widescreen code)
void dol_guest_memory_dirty_mark(u32, u32) {}      // (its MEM2 puppet pool's: this runtime gives puppets the game's heaps)
bool bluewake_game_options_has(const char* name) { return bod::optionHas(name); }
bool bluewake_host_reset_requested(void) { return bod::g_resetRequested.load(); }
// A puppet's own voice (anchor.cpp, from its player's voice bank), mixed into the game's sound (audio.cpp).
void dol_aurora_audio_mix_voice(const int16_t* mono, uint32_t frames, float gain_left, float gain_right, float step) {
    bod::mixVoice(mono, frames, gain_left, gain_right, step);
}
}

// ---- its game_options.c preferences (Controls and Enhancements, in every session) ----
// Use 'Switch' Targeting Mode holds the game's targeting byte (g_dComIfG_gameInfo + 0x1A6) at 1, giving Hold back once
// when turned off; Remove Title and Ending Videos serves wwrando's blank movie (blank_thp.h) for the two from the overlay.
namespace {
std::atomic<bool> s_prefSwitchTargeting{false}, s_prefRemoveVideos{false};
bool s_switchTargetingForced = false;
}
extern "C" void bluewake_pref_switch_targeting_set(bool on) { s_prefSwitchTargeting = on; }
extern "C" void bluewake_pref_remove_videos_set(bool on) { s_prefRemoveVideos = on; }

// Its player models without a restart (player_model.c's app side): the swap is asked for here and made by hook 13 at
// the next play scene's start (bod_extras: playerModelHook); a teleport to where Link stands starts one now.
extern "C" bool bluewake_player_model_live(void) { return bod::playerModelLive(); }
extern "C" void bluewake_player_model_swap(void) {
    bod::playerModelRequest();
    const BluewakeLocation where = bluewake_game_location();
    if (where.has_player) bluewake_request_teleport(where.stage, where.room, where.pos, where.angle_y);
}
extern "C" int bluewake_player_model_state(void) { return bod::playerModelState(); }

// Link's voice changed while the game runs (bod_voice.cpp; voice_swap.h).
extern "C" bool bluewake_voice_swap_live(void) { return bod::voiceSwapLive(); }
extern "C" bool bluewake_voice_swap(const void* old_aaf, size_t old_n, const void* new_aaf, size_t new_n, const char* const* names,
                                    const void* const* data, const size_t* sizes, int count) {
    return bod::voiceSwapRequest(old_aaf, old_n, new_aaf, new_n, names, data, sizes, count);
}
extern "C" int bluewake_voice_swap_state(void) { return bod::voiceSwapState(); }

// (its dynamic ImGui textures: this runtime's Aurora, extern/aurora, has them)

namespace {
CPUState s_cpu{};
CPUState* cpu() {
    if (!s_cpu.ram) {
        s_cpu.ram = bod::guestRam();
        s_cpu.ram_size = 0x02000000u;  // the 32 MB window (ctx.h kHostMemSize): the game's 24 MB, then the puppets' fast pool
    }
    return &s_cpu;
}
void load(const bod::Regs& g) {
    CPUState* c = cpu();
    std::memcpy(c->gpr, g.r, sizeof c->gpr);
    std::memcpy(c->fpr, g.f, sizeof c->fpr);
    std::memcpy(c->ps1, g.ps1, sizeof c->ps1);
    c->lr = *g.lr, c->ctr = *g.ctr;
    if (g.cr) {
        u32 cr = 0;
        for (int i = 0; i < 8; i++) cr |= u32(g.cr[i] & 0xF) << (28 - 4 * i);
        c->cr = cr;
    }
}
void store(const bod::Regs& g) {
    std::memcpy(g.r, s_cpu.gpr, sizeof s_cpu.gpr);
    std::memcpy(g.f, s_cpu.fpr, sizeof s_cpu.fpr);
    std::memcpy(g.ps1, s_cpu.ps1, sizeof s_cpu.ps1);
    *g.lr = s_cpu.lr, *g.ctr = s_cpu.ctr;
    if (g.cr)
        for (int i = 0; i < 8; i++) g.cr[i] = u8((s_cpu.cr >> (28 - 4 * i)) & 0xF);
}
const bod::Regs* s_regs = nullptr;  // the hook in progress (its guest calls: bod_guest_call_cpu)
}  // namespace

// A guest call from its host code (guest_coro_sync.c): the CPUState it works on is this file's, the hook's registers.
extern "C" void bod_guest_call_cpu(CPUState* c, u32 function) {
    if (!s_regs || c != &s_cpu) return;
    store(*s_regs);
    bod::callGuest(function);
    load(*s_regs);
}


namespace rt {
u32 hleAlloc(u32 size, u32 align);  // (runtime.h)
}

namespace rt::bod {
namespace {
// Testing: BLUEWAKE_TEST_GIVE=<retrace>:<item no> drops that item at Link's feet (fopAcM_createItem): he picks it up and holds it
// up as he does for an item-get, which the puppets of the other players then show (a scenario cannot open a chest).
void testGive(CPUState* c) {
    static const char* const spec = std::getenv("BLUEWAKE_TEST_GIVE");
    static bool done;
    if (!spec || done) return;
    const unsigned long long at = std::strtoull(spec, nullptr, 10);
    const char* colon = std::strchr(spec, ':');
    const u32 player = mem_read32(c, 0x803CA754u);
    if (!colon || retraceCount() < at || player < 0x80000000u || player >= 0x81800000u) return;
    done = true;
    const u32 pos = hleAlloc(16, 16);
    for (u32 i = 0; i < 3; ++i) mem_write32(c, pos + 4 * i, mem_read32(c, player + 0x1F8u + 4 * i));
    c->gpr[3] = pos;
    c->gpr[4] = u32(std::strtoul(colon + 1, nullptr, 0));
    c->gpr[5] = 0xFFFFFFFFu;  // no item bit
    c->gpr[6] = 0xFFFFFFFFu;  // room: the current one
    c->gpr[7] = 0;            // param
    c->gpr[8] = 0;            // angle
    c->gpr[9] = 0;            // gravity type
    c->gpr[10] = 0;           // scale
    bod_guest_call_cpu(c, 0x80026ADCu);
    std::fprintf(stderr, "[test] item %lu dropped at Link's feet\n", std::strtoul(colon + 1, nullptr, 0));
}
}  // namespace
// As its host's boot (main.c): the features' attach, then each retrace their retrace work.
void apiAttach() {
    bluewake_widescreen_attach(cpu());
    bluewake_gc_adapter_start();
    bluewake_haptics_attach(cpu());
    bluewake_forest_water_reload();
    bluewake_fast_load_attach(cpu());
    bluewake_treasure_glow_attach();
    bluewake_quick_doors_attach(cpu());
    // Testing (headless runs have no settings screen to turn it on): BLUEWAKE_TEST_BUNNY=1 / BLUEWAKE_TEST_CHART=1 switch the Bunny Hood / Reveal Sea Chart on.
    if (std::getenv("BLUEWAKE_TEST_BUNNY")) bluewake_bunny_hood_enable(true);
    if (std::getenv("BLUEWAKE_TEST_CHART")) bluewake_chart_reveal_enable(true);
}
void apiRetrace() {
    // Testing: BLUEWAKE_TEST_FMAP=<retrace> opens the Sea Chart in its warp mode at that retrace (mFmapOpen = 1, as the Ballad of Gales does).
    static const long s_fmapAt = std::getenv("BLUEWAKE_TEST_FMAP") ? std::atol(std::getenv("BLUEWAKE_TEST_FMAP")) : 0;
    static bool s_fmapDone = false;
    if (s_fmapAt > 0 && !s_fmapDone && retraceCount() >= u64(s_fmapAt)) s_fmapDone = true, mem_write8(cpu(), 0x803CA7F9u, 1);
    bluewake_cosmetics_retrace(cpu());
    bluewake_game_state_retrace(cpu());
    bluewake_fast_load_retrace(0);
    bluewake_quick_doors_retrace();
    bluewake_haptics_retrace();
    bluewake_text_speed_retrace(cpu());    // Text Speed (text_speed.c: characters a frame, no hook site)
    bluewake_audio_volume_retrace(cpu());  // Music / Sound Effects volume
    bluewake_autosave_retrace(cpu());
    bluewake_team_sync_retrace(cpu());  // online: the team's shared progress (team_sync.c)
    bluewake_switch_sync_retrace(cpu());   // online: switches shared live
}
void apiOptionsRetrace() {
    CPUState* c = cpu();
    constexpr u32 kTargeting = 0x803C4DAE;
    if (s_prefSwitchTargeting) {
        if (mem_read8(c, kTargeting) != 1) mem_write8(c, kTargeting, 1);
        s_switchTargetingForced = true;
    } else if (s_switchTargetingForced) {
        mem_write8(c, kTargeting, 0);
        s_switchTargetingForced = false;
    }
    static unsigned tick;
    if ((++tick % 60u) == 1u) {  // about once a second (an in-place randomizer switch drops the overlays: made again)
        static const char* const kVideos[] = {"thpdemo/title_loop.thp", "thpdemo/end_st_epilogue.thp"};
        const bool want = s_prefRemoveVideos;
        for (const char* v : kVideos) {
            const bool has = dvd_overlay_has(v);
            if (want && !has) dvd_overlay_set(v, kBlankThp, u32(sizeof kBlankThp));
            else if (!want && has) dvd_overlay_clear(v);
        }
    }
    bluewake_bunny_hood_retrace(c);
    bluewake_text_swap_retrace(c);
}
// The pads as read (Aurora's PADStatus, laid out as its DolPadState): the GameCube adapter's
// controllers merged in, as its host_read_live_pads does.
void apiPads(void* pads, unsigned long long retrace) {
    bluewake_gc_adapter_set_first_is_p1(CVarGetInteger("gSettings.Controls.FirstConnectedIsPlayer1", 1) != 0);
    bluewake_gc_adapter_overlay(static_cast<DolPadState*>(pads), retrace);
}
// PADControlMotor: the game's own rumble command, forwarded while its Rumble Mode says (Classic).
bool apiMotorForward(unsigned channel, unsigned motor) {
    if (!bluewake_haptics_forward_motor()) return false;
    bluewake_gc_adapter_rumble_classic(channel, motor);
    return true;
}
unsigned apiFeatureEnter(Regs regs, unsigned address) {
    load(regs);
    s_cpu.pc = address;
    bluewake_forest_water_dispatch(&s_cpu, address);
    bluewake_quick_doors_dispatch(&s_cpu, address);
    store(regs);
    return 0;
}
unsigned apiTreasureGlow(Regs regs, unsigned pc) {
    load(regs);
    s_cpu.pc = pc;
    const unsigned r = bluewake_treasure_glow_hook(&s_cpu);
    store(regs);
    return r;
}
// Its J2DPane::draw hook (7, widescreen.c): the HUD buttons' and the file select's pane colours (with
// the squeeze 1 here its HUD moves are none).
unsigned apiPaneHook(Regs regs, unsigned pc) {
    load(regs);
    s_cpu.pc = pc;
    const unsigned r = bluewake_widescreen_native_hook(&s_cpu, 7);
    store(regs);
    return r;
}
// As its game_options.c native_hook sends them (with this runtime's guest calls made at once, no feature has one in flight
// across frames, so each runs in turn).
unsigned apiHostHook(Regs regs, unsigned id, unsigned pc) {
    load(regs);
    const bod::Regs* const outer = s_regs;
    s_regs = &regs;
    CPUState* c = cpu();
    c->pc = pc;
    const u32 lr = c->lr;
    unsigned r = 0;
    switch (id) {
    case 30:  // dScnPly_Draw: the gyro aiming, Reveal Sea Chart's panes, the puppets, the Bunny Hood, the autosave
        bod::gyroFrame();
        testGive(c);
        r = bluewake_chart_reveal_hook(c, 0x80234F5Cu);
        if (!r) r = bluewake_puppet_hook(c, id);
        if (!r) r = bluewake_bunny_hood_draw_hook(c, 0x80234F5Cu);
        if (!r) r = bluewake_autosave_hook(c, 0x80234F5Cu);
        break;
    case 31:  // dScnPly_Delete
        r = bluewake_puppet_hook(c, id);
        if (!r) r = bluewake_bunny_hood_delete_hook(c, 0x802350BCu);
        break;
    case 34: r = bluewake_voice_pitch_apply(c); break;
    case 37:
    case 38: r = bluewake_switch_sync_hook(c, id == 37); break;
    case 51: r = bluewake_pvp_execute_hook(c); break;
    case 70:
    case 71:
    case 72:
    case 73: r = bluewake_bunny_hood_hook(c, id); break;
    default: break;
    }
    s_regs = outer;
    store(regs);
    // Its hooks return from the function by going to the link register; this runtime's interpreter says so with its own word.
    return r != 0 && r == lr ? 0xFFFFFFFFu /* kInterpReturn */ : r;
}
void apiForestWaterLinked(unsigned textStart) { bluewake_forest_water_set_ftree_text(textStart); }
unsigned apiForestWaterTreeCheck() { return bluewake_forest_water_tree_timer_check; }
}  // namespace rt::bod
