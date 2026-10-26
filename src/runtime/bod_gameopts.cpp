// Game options: Boat of Daphnes' switches (its mods/betterww/options.txt - Better Wind Waker's
// settings, wwrando's Hero Mode, low-health beep and vanilla-bug fixes; GPL-3.0-or-later, the fixes
// MIT from wwrando) on this runtime. Each option is
//   sites:  instructions replaced while it is on - interpreter patches (the functions holding them
//           run in the interpreter while patched)
//   hooks:  native code at an instruction, before or after it - interpreter hook words
//   writes: values written while it is on, the memory's own put back when it goes off
// A REL's sites, hooks and writes are applied when it links and dropped when it unlinks. Which are on
// is chosen as Boat of Daphnes' game_options.c chooses: at start from BLUEWAKE_OPTIONS (the settings'
// list, bw_settings_apply_launch; "none" first in safe mode), then live from the menu
// (bluewake_game_options_set); applied once a frame on the game thread. Hook bodies are ported from
// its runtime/host/src/game_options.c.
#include "runtime/bod.h"
#include "runtime/bod_bridge.h"
#include "runtime/interp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {
extern std::atomic<bool> g_relLinked[kMaxModules];  // src/runtime/dispatch.cpp
}

namespace rt::bod {

namespace {
struct Where {
    u32 module, section, offset;
};
struct OptionDef {
    const char* name;
    int index;
    bool listOn;  // Better Wind Waker's own default (the menu's default is off for most: kDefaults)
    const char* title;
};
struct SiteDef {
    int option;
    Where at;
    bool imm16;
    u32 value;
};
struct HookDef {
    int option;
    Where at;
    bool after;
    int id;
};
struct WriteDef {
    int option;
    Where at;
    u32 size;
    u32 bits;
};
#include "runtime/bod_options.inc"

constexpr size_t kOptionCount = sizeof kOptions / sizeof kOptions[0];

// Options this runtime has: widescreen and its HUD are the community widescreen code here
// (src/runtime/twwwide.cpp); this renderer reads modules' arrays and textures by their full address
// already (gx_array_source); Reveal Sea Chart is chart_reveal.c now, which never writes the save.
bool supported(const char* name) {
    for (const char* no : {"widescreen_culling", "hud_edges", "gx_array_source", "reveal_sea_chart"})
        if (!std::strcmp(name, no)) return false;
    return true;
}
// Boat of Daphnes' "named" (game_options.c): the list's word for the option, "-name" for off.
bool named(const char* list, const char* name, bool* on) {
    const size_t len = std::strlen(name);
    bool found = false;
    for (const char* p = list; p != nullptr && *p != '\0';) {
        const char* end = std::strchr(p, ',');
        size_t n = end ? size_t(end - p) : std::strlen(p);
        const char* word = p;
        bool value = true;
        if (n > 0 && *word == '-') value = false, ++word, --n;
        if (n == len && std::strncmp(word, name, n) == 0) *on = value, found = true;
        p = end ? end + 1 : nullptr;
    }
    return found;
}
std::atomic<bool> s_introSkipStart{true};  // bluewake_intro_skip_set
std::atomic<float> s_heroMultiplier{2.f};  // bluewake_game_options_set_hero_multiplier (a seed's Hero Mode)
std::vector<std::string> s_forced;         // options a randomizer seed turns on (its hero mode, invert camera)

u32 resolve(const Where& w) {
    if (w.module == 0) return w.offset;
    if (w.module >= kMaxModules || !g_relLinked[w.module] || w.section >= 32) return 0;
    const u32 base = g_relBase[w.module][w.section];
    return base ? base + w.offset : 0;
}

float flt32(u32 b) { return __builtin_bit_cast(float, b); }
void setSingle(Ctx& c, int reg, float v) { c.f[reg] = v, c.ps1[reg] = v; }
s32 truncS32(float v) {  // fctiwz: toward zero, saturating
    if (v != v) return s32(0x80000000u);
    if (v >= 2147483647.0f) return 0x7FFFFFFF;
    if (v <= -2147483648.0f) return s32(0x80000000u);
    return s32(v);
}
s32 s16At(u32 a) { return s32(s16(rd16(a))); }

std::array<bool, kOptionCount> s_on{};       // as applied
std::array<bool, kOptionCount> s_want{};     // as the settings ask (BLUEWAKE_OPTIONS at start, then the menu)
bool s_chosen = false;                       // s_want made from BLUEWAKE_OPTIONS
int optionIndex(const char* name) {
    for (size_t i = 0; i < kOptionCount; i++)
        if (!std::strcmp(kOptions[i].name, name)) return int(i);
    return -1;
}
bool on(const char* name) {
    const int i = optionIndex(name);
    return i >= 0 && s_on[size_t(i)];
}

// ---- the hooks (Boat of Daphnes' ids) ----
constexpr u32 kGameInfo = 0x803C4C08;  // g_dComIfG_gameInfo

// 1: turn while swinging (procRopeSwing): the stick turns Link by up to the rope's base speed a
// frame, past a quarter of the stick. r31 the player.
u32 hookSwingTurn(Ctx& c) {
    const float stick = flt32(rd32(0x803A4DF0)), base = flt32(rd32(0x803FA2E8));
    const s32 velocity = truncS32(base * stick), threshold = s32(u32(truncS32(base)) >> 2);
    if (velocity >= threshold || velocity <= -threshold) {
        const u32 player = c.r[31];
        const u16 angle = u16(rd16(player + 0x20E) - u32(velocity));
        wr16(player + 0x20E, angle);
        wr16(player + 0x206, angle);
    }
    return 0;
}

// 3: Swift Sail's wind, after KoRL's sail sound call: from behind him, to the nearest eighth of a
// turn, through the game's dKyw_tact_wind_set(0, angle).
u32 hookSailWind(Ctx& c) {
    const u32 korl = rd32(0x803CA75C);
    if (!korl) return 0;
    const s32 facing = -s32(s16(rd16(korl + 0x206))) + 0x4000 + 0x1000;
    guestCall(c, 0x8008A7D0, {0u, u32(facing) & 0xFFFFE000u});
    return 0;
}

// 14: Start skips the intro story told after a new game starts (dScnOpen_c::execute, r31 the
// scene, r3 its process): the story's stream stops (bgmStop(20)), and once its buffer is free
// (mDoAud_isUsedHeapForStreamBuffer) the process is set to its last state, the game's own fade
// to Outset.
enum { kIntroIdle, kIntroWaiting };
int s_introStep = kIntroIdle;
u32 hookIntroSkip(Ctx& c) {
    constexpr u32 kPadTrig = 0x803A4DF0 + 0x33, kOpenScene = 14;
    constexpr s32 kStateEnd = 44;
    const u32 scene = c.r[31];
    if (scene < 0x80000000u || scene >= 0x81800000u || rd16(scene + 0x8) != kOpenScene) {
        s_introStep = kIntroIdle;
        return 0;
    }
    const u32 proc = rd32(scene + 0x1D0);  // dScnOpen_c::mpProc
    const s32 state = s32(rd32(proc + 0x2B0));
    if (s_introStep == kIntroIdle) {
        if (!s_introSkipStart || state < 5 || state >= kStateEnd || !(rd8(kPadTrig) & 0x10)) return 0;
        log("[options] intro story skipped with Start (state %d)\n", state);
        guestCall(c, 0x802A4658, {rd32(c.r[13] - 0x69D0), 20u, 0u});  // JAIZelBasic::bgmStop
        s_introStep = kIntroWaiting;
    } else if (guestCall(c, 0x80006DC0, {}) == 0) {  // mDoAud_isUsedHeapForStreamBuffer
        wr32(proc + 0x2B0, u32(kStateEnd));
        s_introStep = kIntroIdle;
    }
    c.r[3] = proc;
    return 0;
}

bool canDefend(u32 player) { return rd16(player + 0x3560) == 0x33 || rd8(kGameInfo + 0xF) != 0xFF; }

// 18 (main.dol, told apart by address) and 19-26 (RELs): wwrando's Hero Mode, low-health beep and
// vanilla-bug fixes (asm/patches/hero_mode.asm, remove_low_health_beep_anim.asm, fix_vanilla_bugs.asm).
u32 hookFix(Ctx& c, int id, u32 pc) {
    switch (id == 18 ? pc : u32(id)) {
    case 0x801102D0: {  // daPy_lk_c::setDamagePoint: damage (negative) doubled (a seed's multiplier: 4)
        const float amount = float(c.f[1]);
        if (amount < 0.f) setSingle(c, 1, amount * s_heroMultiplier.load());
        return 0;
    }
    case 0x802A2FDC:  // processHeartGaugeSound: no one-heart alarm with a one-heart maximum
        return s16At(kGameInfo) < 8 ? 0x802A3038u : 0u;
    case 0x80111EE0: {  // checkRestHPAnime: none until two quarter hearts are missing
        const s32 life = s16At(kGameInfo + 2), whole = s16At(kGameInfo) & ~3;
        if (whole - life < 2) c.r[3] = 0;
        return 0;
    }
    case 0x800F13A8:  // Helmaroc King's mask shards: no pointer in r30
        return c.r[30] == 0 ? 0x800F13C0u : 0u;
    case 0x800C3420:  // the Deluxe Picto Box replaces the Picto Box on X, Y and Z
        for (u32 i = 0; i < 3; i++)
            if (rd8(0x803CA7DB + i) == 0x23) wr8(0x803CA7DB + i, 0x26);
        return 0;
    case 19: {  // Stalfos: the lower body dies to light arrows with the upper (r3)
        const u32 upper = c.r[3];
        if (upper && rd8(upper + 0x1FAE) && !rd8(c.r[31] + 0x1FAE)) wr8(c.r[31] + 0x1FAE, 1);
        return 0;
    }
    case 20: {  // Miniblin (r29): a single one sets its switch when light arrows kill it
        const u32 mini = c.r[29];
        if (rd8(mini + 0x2B4)) wr8(mini + 0x995, rd8(mini + 0x2B8));
        return 0;
    }
    case 21: {  // Jalhalla's Poes: dead when HP is 0 or less, or light arrows are killing it
        const u32 poe = c.r[31];
        if (s8(rd8(poe + 0x285)) <= 0 || rd8(poe + 0x88A) > 0) return 0;
        c.r[3] = 0;
        return pc + 0x97C - 0x904;  // the epilogue, r3 = 0
    }
    case 22:
        wr8(c.r[31] + 0x88A, 0);
        return 0;
    case 23:  // Magtail: light arrows on a respawning head
        if (rd32(c.r[3] + 0x10) & 0x00100000u) wr8(c.r[30] + 0x1CBC, 1);
        return 0;
    case 24: {  // Phantom Ganon: the fight starts only when Link is less than the limit above
        const float dy = flt32(rd32(c.r[1] + 0x1C)), limit = flt32(rd32(c.r[31] + 0x88));
        setSingle(c, 4, dy);
        setSingle(c, 3, limit);
        return dy >= limit ? pc + 0x5204 - 0x4F50 : 0u;
    }
    case 0x80041AF4: {  // dStage_playerInit: a start point the room's list lacks is an OSPanic; the first point instead
        constexpr u32 kStartStage = 0x803C9D3C;  // play.mCurStage: name[8], point s16 +8, room +0xA, layer +0xB
        if (rd32(0x803CA754) != 0) return 0;     // dComIfGp_getPlayer(0): the function returns at once
        const s32 point = s16(rd16(kStartStage + 8));
        const u32 data = c.r[4];
        if (point < 0 || data < 0x80000000u || data >= 0x81800000u) return 0;
        const u32 num = rd32(data + 4), entries = rd32(data + 8);
        if (num == 0 || num > 256 || entries < 0x80000000u || entries + 0x20 * num > 0x81800000u) return 0;
        for (u32 i = 0; i < num; i++)
            if (rd8(entries + 0x20 * i + 0x1D) == u8(point)) return 0;
        const u8 first = u8(rd8(entries + 0x1D));
        char stage[9] = {};
        for (u32 i = 0; i < 8; i++) stage[i] = char(rd8(kStartStage + i));
        log("[stage] start point %d is not among the %u of %s room %d layer %d: Link starts at point %u\n", point, num, stage,
            s8(rd8(kStartStage + 0xA)), s8(rd8(kStartStage + 0xB)), first);
        wr16(kStartStage + 8, first);
        return 0;
    }
    case 0x80235340:  // dScnPly_Delete: the miniboss music stops (JAIZelBasic::subBgmStop)
        guestCall(c, 0x802A4CDC, {rd32(0x803F7710)});
        return 0;
    case 0x800D6194:  // daArrow_c::procMove: the arrow's hit callback cleared
        wr32(c.r[31] + 0x3F4, 0);
        return 0;
    case 0x8010E288:  // the Skull Hammer can guard
        return canDefend(c.r[31]) ? 0x8010E29Cu : 0x8010E2A4u;
    case 0x8010E504:
        return canDefend(c.r[31]) ? 0x8010E518u : 0x8010E554u;
    case 0x800D4EBC:  // Ganondorf (r3): reflecting Zelda's arrow only from phase 3
        return s8(rd8(c.r[3] + 0x285)) > 25 ? 0x800D4F68u : 0u;
    case 25:  // Helmaroc King: the landing timer starts
        wr16(c.r[30] + 0xB0, 0);
        return 0;
    case 26: {  // ... 10 s trying to land: fly up and try again
        const u32 king = c.r[30];
        const s32 sub = s16At(king + 0x2C8);
        if (sub > 0 && sub < 3) {
            const s32 timer = s16At(king + 0xB0) + 1;
            wr16(king + 0xB0, u32(timer));
            if (timer > 10 * 30) wr16(king + 0x2C6, 1), wr16(king + 0x2C8, 0);
        }
        return 0;
    }
    default:
        return 0;
    }
}

// fopAcM_SearchByID(id, out) at its entry, answered as it answers (game_options.c hook_search_by_id): an id still
// being created gives NULL and TRUE; else the actor of that id and TRUE, or NULL and FALSE.
u32 hookSearchById(Ctx& c) {
    constexpr u32 kCreateQueue = 0x80372690, kActorQueue = 0x80372028;
    const u32 id = c.r[3], out = c.r[4];
    if (out < 0x80000000u || out >= 0x81800000u || (out & 3)) return 0;
    bool creating = false;
    u32 found = 0;
    if (id != 0xFFFFFFFFu) {
        u32 n = rd32(kCreateQueue);
        for (u32 k = 0; k < 4096 && n >= 0x80000000u && n < 0x81800000u; k++, n = rd32(n + 8)) {
            const u32 req = rd32(n + 0xC);
            if (req >= 0x80000000u && req < 0x81800000u && rd32(req + 0x3C) == id) {
                creating = true;
                break;
            }
        }
        n = creating ? 0 : rd32(kActorQueue);
        for (u32 k = 0; k < 8192 && n >= 0x80000000u && n < 0x81800000u; k++, n = rd32(n + 8)) {
            const u32 proc = rd32(n + 0xC);
            if (proc >= 0x80000000u && proc < 0x81800000u && rd32(proc + 4) == id) {
                found = proc;
                break;
            }
        }
    }
    wr32(out, found);
    c.r[3] = (creating || found) ? 1 : 0;
    return kInterpReturn;  // (returned from: the caller goes on)
}
// fopAcIt_Judge(judge, data) at its entry: with fpcSch_JudgeByID, the actor of that id (or NULL) found here.
u32 hookJudgeById(Ctx& c) {
    constexpr u32 kJudgeById = 0x80040068, kActorQueue = 0x80372028;
    const u32 data = c.r[4];
    if (c.r[3] != kJudgeById || data < 0x80000000u || data >= 0x81800000u || (data & 3)) return 0;
    const u32 id = rd32(data);
    u32 found = 0;
    if (id != 0xFFFFFFFFu) {
        u32 n = rd32(kActorQueue);
        for (u32 k = 0; k < 8192 && n >= 0x80000000u && n < 0x81800000u; k++, n = rd32(n + 8)) {
            const u32 proc = rd32(n + 0xC);
            if (proc >= 0x80000000u && proc < 0x81800000u && rd32(proc + 4) == id) {
                found = proc;
                break;
            }
        }
    }
    c.r[3] = found;
    return kInterpReturn;  // (returned from: the caller goes on)
}

// ---- Ocarina of Time controls: hooks 90 (movement), 81 (instant turning), 82 (the draw) ----
// OoT-style movement (90) takes over setSpeedAndAngleNormal, the walking procs' speed and heading step, with Ocarina of
// Time's own run and idle actions (z_player.c: func_80840BC8 idle, func_80842180 run, func_80836FAC the stick's speed,
// func_8083DF68 the speed and heading steps, func_8083C484 the reversal), turned from its 20 Hz into Wind Waker's 30 fps
// and from its top speed (6) into Wind Waker's. Instant turning (81, the function's second instruction: it runs when 90
// did not take over) only turns a standing Link to the stick. B slashes at once is the site 0x8010D6DC (a nop) and,
// for its second level, hook 82. Each acts only on the ground, outside events and demos, in the procs that walk (never
// swimming, sailing, climbing, crawling, carrying, with Iron Boots, on ice, locked on); everything else is Wind Waker's.
enum OotKnob {
    kKnobFullAt,
    kKnobTopSpeed,
    kKnobTurnRate,
    kKnobNoDrawDelay,
    kKnobStabSpeed,
    kKnobStabInherit,
    kKnobFlipsCosmetic,
    kKnobStabIsg,
    kKnobStabTalk,
    kKnobStabDraw,
    kKnobCount
};
constexpr const char* kKnobNames[kKnobCount] = {"oot_move_full_at", "oot_move_top_speed", "oot_move_turn_rate",
                                                "b_slash_no_draw_delay", "oot_stab_speed", "oot_stab_inherit_cut",
                                                "mm_flips_cosmetic", "oot_stab_isg", "oot_stab_talk",
                                                "oot_stab_draw"};
// The stick's travel that is full speed (OoT's N64 stick: its 60 of about 80, 0.75); the top speed as a share of Wind
// Waker's; the running heading's turn in degrees a second (OoT: 2000 units a tick, 220); 1: the sword appears with the
// press; the crouch stab's pace (1: OoT's) and whether a power stab keeps the last attack's kind; 1: MM's flips are only
// their animation (the jump attack still comes).
std::atomic<float> s_knob[kKnobCount] = {0.75f, 1.f, 220.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f};
float knob(OotKnob k) { return s_knob[k].load(); }
u32 floatBits(float v) { return __builtin_bit_cast(u32, v); }
bool ootLog() {
    static const bool on = std::getenv("BLUEWAKE_OOT_LOG") != nullptr;
    return on;
}

constexpr u32 kEventRunning = 0x803C9EA2;  // dComIfGp_event_runCheck's byte
bool ootGround(u32 p) { return rd8(kEventRunning) == 0 && rd16(p + 0x304) == 0; }  // no event, no player demo
bool ootLocked(Ctx& c, u32 p) {  // checkAttentionLock
    const u32 att = rd32(p + 0x3480);
    if (att < 0x80000000u || att >= 0x81800000u) return false;
    if (rd32(att + 0x20) & 0x20000000u) return true;
    return (guestCall(c, 0x8009FA10, {att}) & 0xFF) != 0;  // dAttention_c::LockonTruth
}
bool ootHeavy(Ctx& c, u32 p) { return guestCall(c, 0x80111EFC, {p}) != 0; }  // checkHeavyStateOn
// The walking procs: wait, free wait, move.
bool ootWalking(u32 proc) { return proc >= 4 && proc <= 6; }
// Where the OoT controls may act on Link's walking (r3 at setSpeedAndAngleNormal): on the ground, in a walking proc, free
// (not locked on, nothing carried, no Iron Boots), not on ice (Wind Waker's slide stays).
bool ootFree(Ctx& c, u32 p) {
    if (!ootGround(p) || !ootWalking(rd32(p + 0x31D8))) return false;
    if (rd32(p + 0x3190) != 0 || rd32(p + 0x3584) == 0x0F) return false;  // something carried; on ice (dBgS_Attr_ICE_e)
    return !ootLocked(c, p) && !ootHeavy(c, p);
}

constexpr float kOotTick = 20.f / 30.f;  // OoT's player runs at 20 Hz: a frame here is two thirds of its tick
constexpr float kOotTop = 6.f;           // its running speed (R_RUN_SPEED_LIMIT / 100)

// OoT's speed for a stick distance (0..1), 0 to 6 (func_80836FAC with its idle/run argument): the stick's 60 at the knob's
// share of the travel, nothing up to 20, then the cosine curve that stays slow until the stick is well out.
float ootSpeed(float p) {
    const float mag = std::min(60.f, p * 60.f / std::clamp(knob(kKnobFullAt), 0.5f, 1.f)) - 20.f;
    if (mag <= 0.f) return 0.f;
    const float t = 1.f - std::cos(mag * 450.f * 6.2831853f / 65536.f);
    return std::min(kOotTop, (t * t * 30.f + 7.f) * 0.14f);
}
// Math_StepToF / Math_AsymStepToF / Math_ScaledStepToS.
float stepTo(float v, float to, float step) { return v < to ? std::min(to, v + step) : std::max(to, v - step); }
float asymStepTo(float v, float to, float up, float down) { return v < to ? std::min(to, v + up) : std::max(to, v - down); }
// The hooks below call the game (lock-on, heavy state), which takes r3 and r4: the function they sit in still needs them.
struct KeepArgs {
    Ctx& c;
    u32 r3, r4;
    explicit KeepArgs(Ctx& ctx) : c(ctx), r3(ctx.r[3]), r4(ctx.r[4]) {}
    ~KeepArgs() { c.r[3] = r3, c.r[4] = r4; }
};
s16 stepToS(s16 v, s16 to, s32 step) {
    const s32 d = s16(to - v);
    return std::abs(d) <= step ? to : s16(v + (d > 0 ? step : -step));
}

// 90: setSpeedAndAngleNormal's entry (r3 the player). Returns to the caller with the speed (mNormalSpeed 0x35BC), the
// heading (current.angle.y 0x206) and the facing (shape_angle.y 0x20E) set as OoT sets them; Wind Waker's own when not free.
u32 hookOotMove(Ctx& c) {
    const KeepArgs keep(c);
    const u32 p = c.r[3];
    if (!ootFree(c, p)) return 0;
    const float stick = flt32(rd32(p + 0x35B0));
    const bool input = stick > 0.05f;  // (Wind Waker's "the stick is idle")
    const s16 stickYaw = s16(rd16(p + 0x34E8));
    s16 heading = s16(rd16(p + 0x206)), shape = s16(rd16(p + 0x20E));
    float speed = flt32(rd32(p + 0x35BC));
    const float top = flt32(rd32(p + 0x2A8)) * std::clamp(knob(kKnobTopSpeed), 0.3f, 1.5f);
    const float unit = top / kOotTop * kOotTick;  // one of OoT's speed units a tick, in this frame's speed change
    const float target = input ? ootSpeed(stick) / kOotTop * top : 0.f;
    const s32 turn = std::max(1, s32(std::clamp(knob(kKnobTurnRate), 60.f, 720.f) / 30.f * 65536.f / 360.f));
    const char* what = "run";
    if (std::fabs(speed) <= 0.001f) {  // standing (checkNextMode's test)
        if (target > 0.f) {
            // The idle action: past the dead zone Link faces the stick at once and runs (func_8083C8DC), speeding up
            // from this frame.
            heading = shape = stickYaw;
            speed = std::min(target, 2.f * unit);
            what = "start";
        } else if (input) {
            // Inside the dead zone: he turns on the spot, 1200 a tick (func_8083CD54), and does not move.
            shape = stepToS(shape, stickYaw, s32(1200.f * kOotTick));
            heading = shape;
            speed = 0.f;
            what = "turn on the spot";
        } else {
            return 0;  // no input: Wind Waker's own (nothing moves)
        }
    } else if (input && std::abs(s32(s16(stickYaw - heading))) > 0x6000) {
        // A reversal (more than 135 degrees): OoT's run stops in a tick (func_8083C484, 8.0 a tick from at most 6) and the
        // idle takes the stick's direction. Done in this frame, so the game never sees a run pointing away from the stick
        // (its skid and turn-around).
        heading = shape = stickYaw;
        speed = 0.f;
        what = "reversal";
    } else {
        // Running: speed up 2.0 and slow down 1.5 a tick toward the stick's speed, the heading turns at a fixed rate
        // (2000 a tick), the facing follows it at the same rate (func_808368EC).
        speed = asymStepTo(speed, target, 2.f * unit, 1.5f * unit);
        if (input) heading = stepToS(heading, stickYaw, turn);
        shape = stepToS(shape, heading, turn);
    }
    wr16(p + 0x206, u16(heading));
    wr16(p + 0x20E, u16(shape));
    wr32(p + 0x35BC, floatBits(speed));
    if (ootLog())
        log("[oot] move %s: stick %.3f target %.2f speed %.2f heading %04X shape %04X stick yaw %04X proc %u\n", what, stick,
            target, speed, u16(heading), u16(shape), u16(stickYaw), rd32(p + 0x31D8));
    return kInterpReturn;
}

// 81: setSpeedAndAngleNormal's second instruction (r3 the player): a standing Link pushed past a light touch faces the stick
// at once (Wind Waker turns him on the spot first, or plays its turn-around); the function then starts him running.
u32 hookOotTurn(Ctx& c) {
    const KeepArgs keep(c);
    const u32 p = c.r[3];
    if (!ootFree(c, p) || std::fabs(flt32(rd32(p + 0x35BC))) > 0.001f || !(flt32(rd32(p + 0x35B0)) > 0.25f)) return 0;
    const s16 target = s16(rd16(p + 0x34E8));
    if (std::abs(s32(s16(target - s16(rd16(p + 0x206))))) <= 0x400 && std::abs(s32(s16(target - s16(rd16(p + 0x20E))))) <= 0x400)
        return 0;
    wr16(p + 0x206, u16(target));
    wr16(p + 0x20E, u16(target));
    if (ootLog()) log("[oot] turn snap %04X proc %u\n", u16(target), rd32(p + 0x31D8));
    return 0;
}

// 82: checkItemChangeFromButton, after B started the sword's draw (r31 the player): the draw's animation (BCK_REST, on
// the second upper layer, played backwards from frame 14, the sword appears when its next step crosses 7) starts at 9.7:
// the frame's own update brings it to 7.9, and the next frame's checkItemAction sees the crossing, so the sword is out,
// and the slash starts, a frame after the press.
u32 hookBDraw(Ctx& c) {
    const u32 p = c.r[31];
    if (knob(kKnobNoDrawDelay) < 0.5f || rd16(p + 0x301C) != 0xD7) return 0;
    wr32(p + 0x308C, floatBits(9.7f));
    if (ootLog()) log("[oot] b-slash: draw skipped ahead\n");
    return 0;
}

// ---- Crouch Stab and Power Crouch Stab (hooks 83, 85, 88, 89, 91, 92) ----
// While Link crouches behind his shield (R held, procCrouchDefense) B stabs, as in Ocarina of Time (z_player.c:
// func_808428D8): its stab animation (gPlayerAnim_link_normal_defense_kiru, 5 frames that play one a frame here) on
// Wind Waker's crouch. The animation is OoT's own, moved over bone by bone: for each of 16 bones (torso, head, both arms,
// both legs) the turn it makes in the world from OoT's shield crouch (defense_wait) is made by the matching joint here
// from Wind Waker's crouch (hook 89, in the model's joint pass), and the body moves forward with OoT's root. The table
// was taken from the animation data with local/scratch's oot_stab_table.py. The sword's three capsules are on for its
// first three frames as procCutExA has them (the two reset flags), the trail follows the blade itself (hooks 91, 92,
// after the game fills it from a cut's recorded path), Link's face is the cut's (TCUT), and B stabs again when a stab
// ends, as in OoT. Power Crouch Stab does not give the stab attack values of its own: it re-issues the last sword
// attack's setAtParam (OoT's stab never writes the sword's damage type, so it keeps the last swing's), cut type included
// unless the knob says otherwise.
constexpr u32 kPlayerPtr = 0x803CA754, kEventModeAddr = 0x803C9EA2, kSelectSword = 0x803C4C16;
constexpr u32 kProcCrouchDefense = 0xC, kEquipSword = 0x103;
constexpr u32 kPCurProc = 0x31D8, kPEquipItem = 0x3560, kPItemTrigger = 0x34C8, kPResetFlg0 = 0x2A4, kPNoResetFlg0 = 0x29C;
constexpr u32 kPCutType = 0x290, kPModeFlg = 0x3618, kPFaceFrame = 0x3530, kPSwordTop = 0x2C8, kPSwordRoot = 0x36C4;
constexpr u32 kPSwBlur = 0x37E4;  // daPy_swBlur_c: +0x14 points drawn, +0x20 the tip's extension, +0x34 tips[60], +0x304 roots[60]
constexpr u32 kFnCheckEquipAnime = 0x8010C528, kFnVoiceStart = 0x8010314C, kFnSeStartSwordCut = 0x80103104;
constexpr u32 kFnSetNormalCutAtParam = 0x801555FC, kFnSetAtParam = 0x8011D070, kFnSetTextureAnime = 0x80104A08;
constexpr u32 kCutExA = 0x1A;
constexpr u32 kFnSetDoStatus = 0x801088E8, kFnOrderTalk = 0x8010D8B0;
constexpr u32 kFnCheckGuardAccept = 0x80112280, kFnProcCrouchDefenseInit = 0x80113D30, kFnSetAnimeEquipSword = 0x8010C010;
constexpr u32 kFnChangeCutReverseProc = 0x80155C3C, kAnmCutRel = 0x15;  // (daPy_ANM ANM_CUTREL)
constexpr u32 kFaceCut = 0x1E, kFaceCrouch = 0x02;      // mTexAnmIndexTable: TCUT (ANM_CUTA's), TMABAB (ANM_DIFENCE's)
constexpr u32 kFaceModeFlags = 0x100 | 0x400;           // playTextureAnime's blink / body-synced face: off while stabbing
// OoT's 5 frames after a lead-in frame halfway to its first (OoT snaps from the crouch to it in one 20 Hz tick; here the
// frames drawn between two game frames would blend those two far-apart poses into a fold); the sword hits in the first
// three of OoT's.
constexpr float kStabLead = 1.f, kStabFrames = kStabLead + 5.f, kStabHitUntil = kStabLead + 3.f;

// Wind Waker Link's joints (cl.bdl): each one's parent.
constexpr s8 kJointParent[42] = {-1, 0,  1,  2,  3,  4,  5,  6,  7,  4,  9,  10, 11, 4,  4,  14, 15, 15, 15, 15, 15,
                                 15, 15, 22, 15, 24, 15, 26, 27, 1,  29, 30, 31, 32, 33, 34, 30, 36, 37, 38, 39, 0};
// OoT's bones on Wind Waker's joints: waist -> center, lower -> waist_jnt, upper -> body_chn, head -> neck_jnt, the
// sword arm (L_shoulder, L_forearm, L_hand -> LarmA, LarmB, cl_LhandA), the shield arm (R_... -> RarmA, RarmB,
// cl_RhandA), the legs (R_thigh, R_shin, R_foot -> RlegA, RlegB, Rfoot; L_... -> LlegA, LlegB, Lfoot). Both Links hold
// the sword in the left hand.
constexpr int kStabJoint[16] = {1, 30, 2, 14, 6, 7, 8, 10, 11, 12, 37, 38, 39, 32, 33, 34};
// Each frame's world turn of those bones from the shield crouch, as quaternions (x, y, z, w) in the model's space (+y up,
// +z where Link faces); both games turn bones Z, Y, X.
constexpr float kStabTurn[5][16][4] = {
    {{0.2337f, -0.1440f, 0.0574f, 0.9599f}, {0.2337f, -0.1440f, 0.0574f, 0.9599f}, {0.2043f, -0.3837f, -0.3164f, 0.8432f}, {0.1526f, -0.0046f, -0.0237f, 0.9880f}, {-0.4075f, -0.4762f, -0.2210f, 0.7472f}, {-0.2560f, -0.3863f, -0.1441f, 0.8743f}, {0.2480f, 0.0645f, -0.4635f, 0.8482f}, {0.1205f, -0.6265f, -0.4275f, 0.6405f}, {0.1310f, -0.5644f, -0.4134f, 0.7025f}, {0.1317f, -0.6273f, -0.4149f, 0.6458f}, {0.1002f, -0.0828f, 0.0121f, 0.9914f}, {0.0453f, -0.0554f, 0.0105f, 0.9974f}, {0.0233f, -0.0468f, -0.0052f, 0.9986f}, {0.3242f, 0.2383f, -0.3100f, 0.8614f}, {0.0742f, -0.1989f, -0.2100f, 0.9544f}, {0.0559f, -0.1980f, -0.2107f, 0.9556f}},
    {{0.2534f, -0.1888f, 0.0870f, 0.9448f}, {0.2534f, -0.1888f, 0.0870f, 0.9448f}, {0.2164f, -0.4062f, -0.3438f, 0.8185f}, {0.1745f, 0.0750f, 0.0538f, 0.9803f}, {-0.7102f, -0.5633f, -0.1310f, 0.4015f}, {-0.4870f, -0.3546f, -0.0495f, 0.7967f}, {0.1236f, 0.0211f, -0.4918f, 0.8617f}, {0.1296f, -0.6626f, -0.4399f, 0.5921f}, {0.1369f, -0.6271f, -0.4321f, 0.6335f}, {0.1367f, -0.6674f, -0.4273f, 0.5944f}, {0.1002f, -0.0967f, 0.0519f, 0.9889f}, {-0.0642f, -0.0223f, 0.0441f, 0.9967f}, {0.0003f, -0.0259f, 0.0110f, 0.9996f}, {0.3417f, 0.2999f, -0.3503f, 0.8189f}, {-0.0037f, -0.2643f, -0.2031f, 0.9428f}, {-0.0184f, -0.2634f, -0.2045f, 0.9426f}},
    {{0.2320f, -0.1579f, 0.0747f, 0.9569f}, {0.2320f, -0.1579f, 0.0747f, 0.9569f}, {0.1499f, -0.3102f, -0.2317f, 0.9098f}, {0.0837f, 0.1504f, 0.0948f, 0.9805f}, {-0.4771f, -0.4886f, -0.1670f, 0.7112f}, {-0.3750f, -0.4169f, -0.1177f, 0.8195f}, {0.0267f, 0.0870f, -0.3562f, 0.9300f}, {0.1046f, -0.6124f, -0.3251f, 0.7130f}, {0.1050f, -0.5927f, -0.3211f, 0.7311f}, {0.1096f, -0.6150f, -0.3174f, 0.7135f}, {0.0153f, 0.0192f, 0.0321f, 0.9992f}, {-0.2373f, 0.1376f, 0.0381f, 0.9609f}, {-0.0186f, 0.1225f, -0.0009f, 0.9923f}, {0.1979f, 0.1295f, -0.2994f, 0.9243f}, {0.0335f, -0.1491f, -0.2082f, 0.9661f}, {0.0245f, -0.1486f, -0.2080f, 0.9665f}},
    {{0.1202f, -0.0538f, 0.0303f, 0.9908f}, {0.1202f, -0.0538f, 0.0303f, 0.9908f}, {0.0564f, -0.1364f, -0.0861f, 0.9853f}, {-0.0008f, 0.0830f, 0.0441f, 0.9956f}, {-0.1736f, -0.0547f, -0.0411f, 0.9824f}, {-0.2636f, -0.1191f, -0.0592f, 0.9554f}, {-0.0822f, 0.0936f, -0.1927f, 0.9733f}, {-0.0129f, -0.3408f, -0.2481f, 0.9067f}, {-0.0131f, -0.3341f, -0.2471f, 0.9095f}, {-0.0105f, -0.3415f, -0.2462f, 0.9070f}, {-0.0640f, 0.0762f, 0.0051f, 0.9950f}, {-0.2086f, 0.1488f, 0.0117f, 0.9665f}, {-0.0129f, 0.1358f, 0.0096f, 0.9906f}, {0.0708f, 0.0379f, -0.1373f, 0.9873f}, {0.0322f, -0.0516f, -0.1020f, 0.9929f}, {0.0289f, -0.0518f, -0.1016f, 0.9931f}},
    {{0.0065f, -0.0019f, 0.0014f, 1.0000f}, {0.0065f, -0.0019f, 0.0014f, 1.0000f}, {0.0025f, -0.0067f, -0.0038f, 1.0000f}, {-0.0045f, 0.0036f, 0.0018f, 1.0000f}, {0.0002f, -0.0083f, -0.0044f, 1.0000f}, {0.0642f, 0.0371f, 0.0138f, 0.9972f}, {0.0651f, 0.0378f, 0.0140f, 0.9971f}, {0.0025f, -0.0067f, -0.0038f, 1.0000f}, {0.0025f, -0.0067f, -0.0038f, 1.0000f}, {0.0025f, -0.0067f, -0.0038f, 1.0000f}, {-0.0316f, 0.0116f, -0.0172f, 0.9993f}, {-0.0177f, 0.0041f, -0.0172f, 0.9997f}, {0.0352f, 0.0026f, 0.0136f, 0.9993f}, {-0.0200f, 0.0223f, 0.0169f, 0.9994f}, {-0.0217f, 0.0159f, 0.0199f, 0.9994f}, {-0.0218f, 0.0159f, 0.0199f, 0.9994f}},
};
// The root's move from the crouch (OoT's model units, a hundredth of its world's) and its size against Wind Waker's Link
// (hips 33.8 high standing there, 45 here).
constexpr float kStabMove[5][3] = {{-30.0f, 151.0f, 1147.0f}, {-39.0f, 375.0f, 1261.0f}, {-29.0f, 386.0f, 738.0f}, {-12.0f, 170.0f, 288.0f}, {-1.0f, 15.0f, 18.0f}};
constexpr float kOotToWw = 0.01f * 45.f / 33.77f;

struct Q {
    float x, y, z, w;
};
Q qmul(Q a, Q b) {
    return Q{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q qconj(Q a) { return Q{-a.x, -a.y, -a.z, a.w}; }
Q qslerp(Q a, Q b, float t) {  // (nlerp: the steps are small)
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.f) b = Q{-b.x, -b.y, -b.z, -b.w};
    Q r{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    return Q{r.x / n, r.y / n, r.z / n, r.w / n};
}
void qrotate(Q q, const float v[3], float out[3]) {
    const Q r = qmul(qmul(q, Q{v[0], v[1], v[2], 0.f}), qconj(q));
    out[0] = r.x, out[1] = r.y, out[2] = r.z;
}

struct StabKnobs {
    float speed;
    bool inheritCut;
};
StabKnobs stabKnobs() {
    StabKnobs k;
    k.speed = std::clamp(knob(kKnobStabSpeed), 0.5f, 2.f);
    k.inheritCut = knob(kKnobStabInherit) != 0.f;
    return k;
}

struct StabState {
    u32 player = 0, pid = 0;  // whose it is (the actor and its process id: a new stage makes a new player)
    bool active = false;
    float t = 0.f;            // the stab's frames so far (OoT's animation frames)
    StabKnobs knobs{};
    u32 faceModes = 0;        // the face's mode flags while it was not stabbing
    u8 serial = 0, kind = 0, flags = 0;  // the last stab: for the pose's swing block and PvP
    u64 startedAt = 0;                   // retrace
    // The last sword attack setAtParam was asked for (hook 88), re-issued by a power stab.
    u32 recPid = 0, recAtp = 0, recSpl = 0, recSe = 0, recHit = 0, recCut = 0;
    float recRadius = 0.f;
    bool recValid = false;
    bool ownAtCall = false;  // our own setAtParam / setNormalCutAtParam: not recorded
    int stabs = 0;
    u64 drawStabUntil = 0;  // Draw Into a Stab: B in the plain crouch; the stab comes when the sword is out (until this retrace)
    // The joint pass (hook 89): each joint's turn in the model as the crouch has it and as the stab makes it.
    Q crouch[42], stab[42];
    // The trail: the blade's (extended tip, root) in the world over the last frames, newest first.
    float trail[8][2][3];
    int trailFrames = 0;
} s_stab;

// Allow Infinite Sword Glitch (hooks 85, 100; the trail hooks 91, 92 and the stab's start). Ocarina of Time's crouch stab
// (z_player.c func_80843188) sets meleeWeaponState (func_80833A20) on its first frame and never clears it itself: the
// shield action clears it (func_80832318) only in func_8083C2B0 / the R-released branch, reached after func_80842964, the
// talk / check / pick-up / chest test. When one of those takes over at the end of the thrust the state stays set: the
// sword's quads keep hitting, the blur keeps its vertices (z_player_lib.c), Link cannot walk off a ledge (z_player.c
// 5038), until a swing or another shield action clears it, or the scene is left. Here: a talk / pick-up / item proc
// starting while the stab is in its retract frames keeps the sword live (hook 100, before setCollision, sets the two
// reset flags every frame; the trail follows the blade) until a cut proc or a new stab starts, the sword is put away, or a
// new stage makes a new player.
struct IsgState {
    bool live = false;
    u32 player = 0, pid = 0;
    u64 pendingUntil = 0;  // retrace: a stab cut short (R let go) in its retract frames: a talk / pick-up proc up to then counts
} s_isg;
// Wind Waker does not let Link talk or lift with R held (the crouch), where Ocarina of Time does: the way here to a stab cut
// short by one is letting R go and pressing A within the stab's retract, so the interrupting proc may come a few frames
// after the stab's proc ended.
constexpr u64 kIsgGrace = 40;

// The procs a stab is "interrupted" by: lifting (OoT's pick-up), and the talk / check / chest / item-get kind of event procs.
bool isgInterruptProc(u32 proc) {
    switch (proc) {
        case 0x6E:  // daPyProc_GRAB_READY: lifting a pot, a stone, a bomb
        case 0x6F:  // GRAB_UP
        case 0xA9:  // DEMO_TOOL
        case 0xAA:  // DEMO_TALK
        case 0xAC:  // DEMO_HOLDUP
        case 0xAD:  // DEMO_OPEN_TREASURE
        case 0xAE:  // DEMO_GET_ITEM
        case 0xC3:  // DEMO_PRESENT
        case 0xCC:  // DEMO_LETTER_OPEN
        case 0xCD:  // DEMO_LETTER_READ
        case 0xD3:  // DEMO_OPEN_SALVAGE_TREASURE
            return true;
    }
    return false;
}
bool isgCutProc(u32 proc) { return proc >= 0x41 && proc <= 0x5E; }  // CUT_A .. BT_VERTICAL_JUMP_LAND: a real sword swing

bool stabEventsQuiet() { return rd8(kEventModeAddr) == 0; }

void wrfloat(u32 a, float v) { wr32(a, __builtin_bit_cast(u32, v)); }

// The stab's turn of table bone b and the root's move at its time t: the lead-in from halfway, OoT's frames one after
// another, the last one back to the crouch.
Q stabTurn(int b, float t) {
    if (t < kStabLead) {
        const float* a = kStabTurn[0][b];
        return qslerp(Q{0.f, 0.f, 0.f, 1.f}, Q{a[0], a[1], a[2], a[3]}, 0.5f + 0.5f * t / kStabLead);
    }
    t -= kStabLead;
    const int f = std::clamp(int(t), 0, 4);
    const float frac = std::clamp(t - float(f), 0.f, 1.f);
    const float* a = kStabTurn[f][b];
    const Q qa{a[0], a[1], a[2], a[3]};
    if (f == 4) return qslerp(qa, Q{0.f, 0.f, 0.f, 1.f}, frac);
    const float* n = kStabTurn[f + 1][b];
    return qslerp(qa, Q{n[0], n[1], n[2], n[3]}, frac);
}
void stabMove(float t, float out[3]) {
    if (t < kStabLead) {
        for (int i = 0; i < 3; i++) out[i] = kStabMove[0][i] * kOotToWw * (0.5f + 0.5f * t / kStabLead);
        return;
    }
    t -= kStabLead;
    const int f = std::clamp(int(t), 0, 4);
    const float frac = std::clamp(t - float(f), 0.f, 1.f);
    for (int i = 0; i < 3; i++) {
        const float a = kStabMove[f][i], b = f == 4 ? 0.f : kStabMove[f + 1][i];
        out[i] = (a + (b - a) * frac) * kOotToWw;
    }
}
// The face: the cut's shout (TCUT frames 6 to 9, mouth open and brows down) and its close (14) on the last frame.
u32 stabFaceFrame(float t) { return t < kStabLead + 4.f ? 5 + u32(t) : 14; }

// The stab ends (its time is up, its proc ended, or the option went off): the crouch's face comes back.
void stabEnd(Ctx& c) {
    if (!s_stab.active) return;
    s_stab.active = false;
    const u32 p = s_stab.player;
    if (p != rd32(kPlayerPtr)) return;
    wr32(p + kPModeFlg, (rd32(p + kPModeFlg) & ~kFaceModeFlags) | s_stab.faceModes);
    guestCall(c, kFnSetTextureAnime, {p, kFaceCrouch, 0});
}

void stabStart(Ctx& c, u32 player, const StabKnobs& k) {
    const bool master = rd8(kSelectSword) != 0x38;
    const bool power = on("oot_stab_power");
    s_stab.ownAtCall = true;
    bool inherited = false;
    if (power && s_stab.recValid && s_stab.recPid == s_stab.pid) {
        setSingle(c, 1, s_stab.recRadius);
        guestCall(c, kFnSetAtParam, {player, 2, s_stab.recAtp, s_stab.recSpl, s_stab.recSe, s_stab.recHit, s_stab.recCut});
        if (!k.inheritCut) wr8(player + kPCutType, kCutExA);
        inherited = true;
    } else {
        guestCall(c, kFnSetNormalCutAtParam, {player, kCutExA});
    }
    s_stab.ownAtCall = false;
    guestCall(c, kFnVoiceStart, {player, inherited ? 1u : 0u});
    // The cut's face, its frame set here each frame (the blink and the body-synced face are off meanwhile).
    s_stab.faceModes = rd32(player + kPModeFlg) & kFaceModeFlags;
    wr32(player + kPModeFlg, rd32(player + kPModeFlg) & ~kFaceModeFlags);
    guestCall(c, kFnSetTextureAnime, {player, kFaceCut, stabFaceFrame(0.f)});
    s_stab.active = true;
    s_isg.live = false, s_isg.pendingUntil = 0;  // (a new stab ends a lingering sword)
    s_stab.t = 0.f;
    s_stab.knobs = k;
    s_stab.trailFrames = 0;
    s_stab.kind = inherited ? 2 : 1;
    s_stab.flags = master ? 1 : 0;
    s_stab.serial++;
    s_stab.startedAt = retraceCount();
    s_stab.stabs++;
    if (ootLog())
        log("[oot] stab %d starts: speed %.2f, %s: atp %u cut 0x%02X%s\n", s_stab.stabs, k.speed, master ? "master" : "hero's",
            rd8(player + 0x44E4 + 0x14), rd8(player + kPCutType), inherited ? " (inherited)" : "");
}

// The option went off (its hooks with it) while a stab played: the face's modes come back (its texture with the next
// animation).
void stabSwitchedOff() {
    if (!s_stab.active) return;
    s_stab.active = false;
    s_isg.live = false;
    const u32 p = s_stab.player;
    if (p == rd32(kPlayerPtr)) wr32(p + kPModeFlg, (rd32(p + kPModeFlg) & ~kFaceModeFlags) | s_stab.faceModes);
}

// 83: procCrouchDefense at its entry (r3 the player), each frame: the stab's time, its hit frames and face, and B.
u32 hookStabProc(Ctx& c) {
    const KeepArgs keep(c);
    const u32 player = c.r[3];
    if (player != rd32(kPlayerPtr) || !on("oot_crouch_stab")) return 0;
    if (s_stab.player != player || s_stab.pid != rd32(player + 4)) {
        s_stab.player = player;  // (a new stage made a new player: no stab runs, the last attack record is checked by pid)
        s_stab.pid = rd32(player + 4);
        s_stab.active = false;
    }
    // Talk and Read While Defending: the crouch does not look at A; here it shows Speak / Read / Check for what Link
    // faces and A starts the talk, as checkNextActionFromButton does on foot (setDoStatus, orderTalk). A talk started
    // during a stab's pull-back is then the Infinite Sword Glitch's own way in, as in Ocarina of Time.
    if (knob(kKnobStabTalk) != 0.f && stabEventsQuiet()) {
        guestCall(c, kFnSetDoStatus, {player});
        if (guestCall(c, kFnOrderTalk, {player}) & 0xFF) {
            if (ootLog()) log("[oot] talk from the crouch (stab %s t %.1f)\n", s_stab.active ? "active" : "idle", s_stab.t);
            return 0;
        }
    }
    if (s_stab.active) {
        s_stab.t += s_stab.knobs.speed;  // (this game frame's step)
        if (s_stab.t >= kStabFrames) {
            stabEnd(c);
        } else {
            wr16(player + kPFaceFrame, u16(stabFaceFrame(s_stab.t)));
            wr16(player + kPFaceFrame + 2, u16(stabFaceFrame(s_stab.t)));
        }
    }
    const bool drawStab = s_stab.drawStabUntil != 0 && retraceCount() < s_stab.drawStabUntil;
    if (rd16(player + kPEquipItem) == kEquipSword && ((rd8(player + kPItemTrigger) & 2) || drawStab) && stabEventsQuiet() &&
        !s_stab.active && guestCall(c, kFnCheckEquipAnime, {player}) == 0) {
        s_stab.drawStabUntil = 0;
        stabStart(c, player, stabKnobs());
    }
    if (ootLog() && s_stab.active && s_stab.t < kStabHitUntil)
        log("[oot] stab t %.1f: cut flag %u\n", s_stab.t, (rd32(player + kPNoResetFlg0) >> 6) & 1);
    if (s_stab.active && s_stab.t < kStabHitUntil && (rd32(player + kPNoResetFlg0) & 0x40) &&
        guestCall(c, kFnChangeCutReverseProc, {player, kAnmCutRel})) {
        // The blade met a wall or a shield (its hit was on since the last frame): the cuts' own rebound, its clang,
        // sparks and stagger back (procCutReverse; the stab ends with the crouch's proc). The crouch's proc is not run.
        if (ootLog()) log("[oot] stab rebounds\n");
        return kInterpReturn;
    }
    if (s_stab.active && s_stab.t < kStabHitUntil) {
        if (!(rd32(player + kPNoResetFlg0) & 0x40)) {  // the first frame of the sword's hit: the swish
            wr32(player + kPResetFlg0, rd32(player + kPResetFlg0) | 1);
            guestCall(c, kFnSeStartSwordCut, {player, 0x2800});
        }
        wr32(player + kPResetFlg0, rd32(player + kPResetFlg0) | 2);
    }
    if (ootLog() && s_stab.active) {  // the blade's place relative to Link, along his facing and up
        const float a = float(s16(rd16(player + 0x20E))) * 3.14159265f / 32768.f, fx = std::sin(a), fz = std::cos(a);
        auto rel = [&](u32 at, float* fwd, float* up, float* side) {
            const float dx = flt32(rd32(at)) - flt32(rd32(player + 0x1F8)), dy = flt32(rd32(at + 4)) - flt32(rd32(player + 0x1FC)),
                        dz = flt32(rd32(at + 8)) - flt32(rd32(player + 0x200));
            *fwd = dx * fx + dz * fz, *up = dy, *side = dx * fz - dz * fx;
        };
        float tf, tu, ts, rf, ru, rs;
        rel(player + kPSwordTop, &tf, &tu, &ts);
        rel(player + kPSwordRoot, &rf, &ru, &rs);
        log("[oot] stab t %.1f (last frame's blade) tip fwd %.0f up %.0f side %.0f  root fwd %.0f up %.0f side %.0f\n", s_stab.t,
            tf, tu, ts, rf, ru, rs);
    }
    return 0;
}

// 120: procCrouch at its entry (r3 the player): Draw Into a Stab. With the sword sheathed R is Wind Waker's plain crouch;
// as in Ocarina of Time (where R raises the shield with an empty sword hand and B draws into the stab), B here goes to the
// shield crouch, draws the sword at once (the draw skipped ahead as B Slashes at Once's no-delay does) and the stab comes
// as the sword is out (hook 83). Needs a sword and a shield on Link, the crouch's own guard rules and no event.
u32 hookStabDraw(Ctx& c) {
    const KeepArgs keep(c);
    const u32 p = c.r[3];
    if (p != rd32(kPlayerPtr) || knob(kKnobStabDraw) == 0.f || !on("oot_crouch_stab") || !stabEventsQuiet()) return 0;
    if (!(rd8(p + kPItemTrigger) & 2) || rd16(p + kPEquipItem) != 0x100 || rd8(kSelectSword) == 0xFF || rd8(kSelectSword + 1) == 0xFF)
        return 0;
    // checkGuardAccept's stage rules (no guarding in houses but two), asked with its "can guard" mode flag (0x40, which the
    // plain crouch has off) on for the question
    const u32 modes = rd32(p + kPModeFlg);
    wr32(p + kPModeFlg, modes | 0x40);
    const bool guard = (guestCall(c, kFnCheckGuardAccept, {p}) & 0xFF) != 0;
    wr32(p + kPModeFlg, modes);
    if (!guard) return 0;
    guestCall(c, kFnProcCrouchDefenseInit, {p});
    guestCall(c, kFnSetAnimeEquipSword, {p, 1});
    if (rd16(p + 0x301C) == 0xD7) wr32(p + 0x308C, floatBits(9.7f));  // (BCK_REST played back from 14: the sword is out next frame)
    s_stab.drawStabUntil = retraceCount() + 20;
    if (ootLog()) log("[oot] draw into a stab\n");
    c.r[3] = 1;
    return kInterpReturn;
}

// 121: setAttentionPos's end (the player is Link): Talk and Read While Defending. The crouch puts Link's attention point (from
// which the game measures what A can talk to or read) at 46.25 above his feet instead of his standing head height (92.5): a
// post box's slot or a sign is then out of the talk window. In the shield crouch it is put back at his standing height.
u32 hookStabTalkReach(Ctx& c) {
    const u32 p = rd32(kPlayerPtr);
    if (p < 0x80000000u || knob(kKnobStabTalk) == 0.f || rd32(p + kPCurProc) != kProcCrouchDefense) return 0;
    wrfloat(p + 0x278, flt32(rd32(p + 0x1FC)) + 92.5f);  // attention_info.position.y (0x26C + 0x8 + 4) = current.pos.y + 92.5
    return 0;
}

// 85: commonProcInit at its entry (r3 the player): the stab ends with the proc.
u32 hookStabProcInit(Ctx& c) {
    const KeepArgs keep(c);
    const u32 proc = c.r[4];
    if (knob(kKnobStabIsg) != 0.f && c.r[3] == rd32(kPlayerPtr) && c.r[3] == s_stab.player) {
        if (ootLog()) log("[isg] proc %02X starts (stab %s t %.1f, live %d)\n", proc, s_stab.active ? "active" : "idle", s_stab.t, int(s_isg.live));
        if (s_isg.live && isgCutProc(proc)) {
            s_isg.live = false, s_isg.pendingUntil = 0;
            if (ootLog()) log("[isg] a sword swing (proc %02X) ends it\n", proc);
        } else if (rd16(c.r[3] + kPEquipItem) == kEquipSword) {
            const bool retract = s_stab.active && s_stab.t >= kStabLead + 2.f && rd32(c.r[3] + kPCurProc) == kProcCrouchDefense;
            if (isgInterruptProc(proc) && (retract || retraceCount() <= s_isg.pendingUntil)) {
                s_isg.live = true, s_isg.player = c.r[3], s_isg.pid = rd32(c.r[3] + 4);
                s_isg.pendingUntil = 0;
                if (ootLog()) log("[isg] stab cut short (t %.1f) by proc %02X: the sword stays live\n", s_stab.t, proc);
            } else if (retract) {  // the crouch let go in the retract: a talk or a lift may follow at once
                s_isg.pendingUntil = retraceCount() + kIsgGrace;
            }
        }
    }
    if (s_stab.active && c.r[3] == s_stab.player) stabEnd(c);
    return 0;
}

// 102: setCollision at its entry (r3 the player), each frame: a sword kept live by the glitch is given the flags that put its
// capsules (and the trail) on, as the stab's hit frames do (first frame: both, then only the second). While the sword is
// not in his hand (Wind Waker puts it away for an item-get or a talk) it rests: the glitch stays, and the sword is live
// again if it comes back before a swing.
u32 hookIsgTick(Ctx& c) {
    if (!s_isg.live) return 0;
    const u32 p = c.r[3];
    if (p != rd32(kPlayerPtr) || p != s_isg.player || rd32(p + 4) != s_isg.pid || knob(kKnobStabIsg) == 0.f) {
        if (ootLog()) log("[isg] ends: player %08X/%08X pid %08X/%08X\n", p, s_isg.player, rd32(p + 4), s_isg.pid);
        s_isg.live = false;
        return 0;
    }
    if (s_stab.active || rd16(p + kPEquipItem) != kEquipSword) return 0;  // (the stab's own flags / the sword is away)
    u32 flags = rd32(p + kPResetFlg0) | 2;
    if (!(rd32(p + kPNoResetFlg0) & 0x40)) flags |= 1;
    wr32(p + kPResetFlg0, flags);
    if (ootLog()) log("[isg] live: proc %02X flags %X, noreset %X\n", rd32(p + kPCurProc), flags, rd32(p + kPNoResetFlg0));
    return 0;
}

// 103: dProcTalk_init at its call of setAnimeUnequip (0x8012DAAC, r3 the player): the talk puts the sword away; a sword kept live
// by the glitch stays in his hand through the talk, as Ocarina of Time's does (that is what the glitch shows).
u32 hookIsgKeepSword(Ctx& c) {
    const u32 p = c.r[3];
    if (!s_isg.live || p != s_isg.player || p != rd32(kPlayerPtr) || knob(kKnobStabIsg) == 0.f) return 0;
    if (rd32(p + kPCurProc) != 0xAA || rd16(p + kPEquipItem) != kEquipSword) return 0;  // DEMO_TALK only
    if (ootLog()) log("[isg] the talk leaves the sword in his hand\n");
    return 0x8012DAB0;  // (past the call)
}

// 89: jointBeforeCB at its entry (r3 the player, r4 the joint, r5 its J3DTransformInfo, r6 its Quaternion), joints in
// order, parents first. Each joint's turn in the model is followed down the tree as the crouch has it and as the stab
// makes it: a table bone takes OoT's world turn on top of the crouch's, the others keep their own turn under their
// parent; the joint is then given the turn that puts it there under its (stabbing) parent.
u32 hookStabBone(Ctx& c) {
    const u32 player = c.r[3], joint = c.r[4], info = c.r[5], quat = c.r[6];
    if (!s_stab.active || player != s_stab.player || joint >= 42 || rd32(player + kPCurProc) != kProcCrouchDefense) return 0;
    const Q local{flt32(rd32(quat)), flt32(rd32(quat + 4)), flt32(rd32(quat + 8)), flt32(rd32(quat + 12))};
    const int parent = kJointParent[joint];
    const Q crouchParent = parent < 0 ? Q{0.f, 0.f, 0.f, 1.f} : s_stab.crouch[parent];
    const Q stabParent = parent < 0 ? Q{0.f, 0.f, 0.f, 1.f} : s_stab.stab[parent];
    s_stab.crouch[joint] = qmul(crouchParent, local);
    int bone = -1;
    for (int b = 0; b < 16; b++)
        if (kStabJoint[b] == int(joint)) bone = b;
    if (bone < 0) {
        s_stab.stab[joint] = qmul(stabParent, local);
        return 0;
    }
    s_stab.stab[joint] = qmul(stabTurn(bone, s_stab.t), s_stab.crouch[joint]);
    const Q turned = qmul(qconj(stabParent), s_stab.stab[joint]);
    wrfloat(quat, turned.x), wrfloat(quat + 4, turned.y), wrfloat(quat + 8, turned.z), wrfloat(quat + 12, turned.w);
    if (joint == 1) {  // center: OoT's root move, in link_root's space
        float move[3], inRoot[3];
        stabMove(s_stab.t, move);
        qrotate(qconj(s_stab.crouch[0]), move, inRoot);
        for (int i = 0; i < 3; i++) wrfloat(info + 0x14 + 4 * i, flt32(rd32(info + 0x14 + 4 * i)) + inRoot[i]);
    }
    return 0;
}

// 91, 92: right after setCollision's initSwBlur / copySwBlur (the trail's first and later frames of a cut), which fill it
// from the cut's recorded path: while stabbing it is filled from the blade itself instead, the newest frames first, six
// steps between two frames.
u32 hookStabTrail(Ctx& c, int id) {
    const u32 p = rd32(kPlayerPtr);
    if ((!s_stab.active && !s_isg.live) || p != s_stab.player) return 0;
    const u32 blur = p + kPSwBlur;
    const float rate = flt32(rd32(blur + 0x20));
    float tip[3], root[3];
    for (int i = 0; i < 3; i++) tip[i] = flt32(rd32(p + kPSwordTop + 4 * i)), root[i] = flt32(rd32(p + kPSwordRoot + 4 * i));
    for (int f = std::min(s_stab.trailFrames, 7); f > 0; f--) std::memcpy(s_stab.trail[f], s_stab.trail[f - 1], sizeof s_stab.trail[0]);
    for (int i = 0; i < 3; i++) s_stab.trail[0][0][i] = tip[i] + (tip[i] - root[i]) * rate, s_stab.trail[0][1][i] = root[i];
    s_stab.trailFrames = std::min(s_stab.trailFrames + 1, 8);
    constexpr int kSteps = 6;
    int n = 0;
    for (int f = 0; f < s_stab.trailFrames && n < 58; f++) {
        const int steps = f + 1 < s_stab.trailFrames ? kSteps : 1;
        for (int s = 0; s < steps && n < 58; s++, n++) {
            const float k = float(s) / kSteps;
            for (int e = 0; e < 2; e++)
                for (int i = 0; i < 3; i++) {
                    const float a = s_stab.trail[f][e][i], b = steps > 1 ? s_stab.trail[f + 1][e][i] : a;
                    wrfloat(blur + (e == 0 ? 0x34 : 0x304) + 12 * n + 4 * i, a + (b - a) * k);
                }
        }
    }
    wr32(blur + 0x14, u32(std::max(0, n - 2)));  // (draw() makes a quad from each point and the next, the count's own too)
    if (ootLog())
        log("[oot] trail hook %d t %.1f: %d points, tip %.0f %.0f %.0f root %.0f %.0f %.0f, Link at %.0f %.0f %.0f\n", id, s_stab.t, n,
            tip[0], tip[1], tip[2], root[0], root[1], root[2], flt32(rd32(p + 0x1F8)), flt32(rd32(p + 0x1FC)), flt32(rd32(p + 0x200)));
    return 0;
}

// ---- Majora's Mask ledge momentum (hooks 93, 95, 96, 97 and a site) and flips (hooks 98, 99) ----
// MM Ledge Momentum (mm-main z_player.c): Link keeps his speed off a ledge. MM jumps from any run or roll faster than half
// the run (3 of 6) facing the way he goes (func_808373F8, called where he leaves the ground) and keeps that speed in the
// air, which only bleeds slowly, faster above 1.5 times the run (func_8083CBC4: speed steps 0.05 up / 0.1 down a tick,
// x4 over 1.5x, a reversal brakes 1.0 a tick, x4 over 1.5x, the heading turns 0xC8 a tick). Here: hook 93 takes
// changeAutoJumpProc's jump test (after its life, state and carried-object tests) with MM's, hook 95 at procAutoJump_init's
// end gives back the speed its clamp to the run took (the jump's height stays Wind Waker's), hooks 96 and 97 are MM's air
// control in procAutoJump (instead of its chase to the run speed) and procFall, and the site keeps a plain fall's speed
// (procFall_init(0) instead of (1), which stopped him dead). Wind Waker's ledge hang and climb-down are left as they are.
// MM Jump Flips: a running jump is, a third each, the regular jump, MM's front flip (link_normal_newroll_jump_20f) or its
// side somersault (newside_jump_20f), with their landings (MM picks them from the floor's collision, which Wind Waker's
// floors do not have; soh_fd rolls them the same way). The animations are MM's, moved over bone by bone onto Wind Waker's
// own jump and landing as the crouch stab's are: each bone's world turn from MM's regular running jump (and its landing)
// at the same frame (local/scratch's mm_flip_table.py). In MM a flip has no jump attack (Player_Action_25 skips
// func_80839770 for actionVar1 4 and 5): hook 99 does the same, unless the flips are cosmetic, where the attack still
// comes and ends the flip.
constexpr u32 kProcAutoJump = 0x24, kProcLand = 0x25, kProcFall = 0x27, kPSpeedF = 0x254, kPCurrentAngle = 0x206, kPShapeAngle = 0x20E;
constexpr u32 kAutoJumpTake = 0x801111F4, kAutoJumpSkip = 0x80111218, kAutoJumpAfterChase = 0x801160FC;

struct MmState {
    bool jumpPending = false;  // hook 93 sent him into procAutoJump_init: its speed before the clamp
    float jumpSpeed = 0.f;
} s_mm;

bool mmFree(u32 p) { return ootGround(p) && rd32(p + 0x3190) == 0; }  // (no event or demo, nothing carried)

// MM's air control (func_8083CBC4 with Player_Action_25's 1.0, 0.05, 0.1, 0xC8, the speed target of its linear stick
// mode), in this frame's steps.
void mmAirControl(u32 p) {
    const float top = flt32(rd32(p + 0x2A8));
    const float unit = top / kOotTop * kOotTick;
    const float stick = flt32(rd32(p + 0x35B0));
    const bool input = stick > 0.05f;
    const s16 stickYaw = s16(rd16(p + 0x34E8));
    s16 heading = s16(rd16(p + kPCurrentAngle)), shape = s16(rd16(p + kPShapeAngle));
    float speed = flt32(rd32(p + 0x35BC));
    const float target = input ? std::min(1.f, stick * 80.f * 0.8f * 0.14f / kOotTop) * top : 0.f;
    float brake = 1.f * unit, accel = 0.05f * unit, decel = 0.1f * unit;
    if (std::fabs(speed) > 1.5f * top) brake *= 4.f, decel *= 4.f;
    if (input && std::abs(s32(s16(stickYaw - heading))) > 0x6000) {
        speed = stepTo(speed, 0.f, brake);
        if (speed == 0.f) heading = shape = stickYaw;
    } else {
        speed = asymStepTo(speed, target, accel, decel);
        if (input) {
            const s16 before = heading;
            heading = stepToS(heading, stickYaw, s32(0xC8 * kOotTick));
            shape = s16(shape + s16(heading - before));
        }
    }
    wr16(p + kPCurrentAngle, u16(heading));
    wr16(p + kPShapeAngle, u16(shape));
    wr32(p + 0x35BC, floatBits(speed));
}

// 93: changeAutoJumpProc, at its jump test (r29 the player, f28 mNormalSpeed, r31 the way he moves): MM's.
u32 hookMmLedgeJump(Ctx& c) {
    const u32 p = c.r[29];
    if (!mmFree(p)) return 0;
    const float speed = float(c.f[28]);
    const s32 off = std::abs(s32(s16(s16(c.r[31]) - s16(rd16(p + kPShapeAngle)))));
    const float half = flt32(rd32(p + 0x2A8)) * 0.5f;
    const bool jump = speed > half && off < 0x2000;
    if (jump) s_mm.jumpPending = true, s_mm.jumpSpeed = flt32(rd32(p + kPSpeedF));
    if (ootLog()) log("[mm] ledge: speed %.1f off %04X -> %s\n", speed, u32(off), jump ? "jump" : "Wind Waker's hang or a fall that keeps its speed");
    return jump ? kAutoJumpTake : kAutoJumpSkip;
}

// 95: procAutoJump_init's end (r30 the player... r3 its result): the speed the clamp to the run took comes back.
u32 hookMmJumpSpeed(Ctx& c) {
    const u32 p = rd32(kPlayerPtr);
    if (!s_mm.jumpPending) return 0;
    s_mm.jumpPending = false;
    const float clamped = flt32(rd32(p + kPSpeedF));
    if (s_mm.jumpSpeed > clamped && clamped > 0.f) {
        wr32(p + 0x35BC, floatBits(flt32(rd32(p + 0x35BC)) * s_mm.jumpSpeed / clamped));
        wr32(p + kPSpeedF, floatBits(s_mm.jumpSpeed));
    }
    if (ootLog()) log("[mm] jump keeps speed %.1f (Wind Waker's %.1f): horizontal %.1f\n", s_mm.jumpSpeed, clamped, flt32(rd32(p + 0x35BC)));
    return 0;
}

// 94: procFrontRoll after its prologue (r31 the player): MM's roll speed (Player_Action_26: the stick's curved speed times
// 1.5, at least half the run, stepped 2.0 up / 1.5 down a tick), so a roll off a ledge carries MM's speed.
u32 hookMmRoll(Ctx& c) {
    const u32 p = c.r[31];
    if (!mmFree(p) || flt32(rd32(p + 0x303C)) >= 20.f) return 0;  // (the roll's own frames; its end is Wind Waker's)
    const float top = flt32(rd32(p + 0x2A8)), unit = top / kOotTop * kOotTick, stick = flt32(rd32(p + 0x35B0));
    const float target = std::max(0.5f * top, (stick > 0.05f ? ootSpeed(stick) / kOotTop * top : 0.f) * 1.5f);
    wr32(p + 0x35BC, floatBits(asymStepTo(flt32(rd32(p + 0x35BC)), target, 2.f * unit, 1.5f * unit)));
    return 0;
}

// 96: procAutoJump after its prologue (r30 the player): MM's air control instead of the chase to the run speed (not while
// Link is carried up by a bird or Medli, m34D0 > 0).
u32 hookMmJumpAir(Ctx& c) {
    const u32 p = c.r[30];
    if (s16(rd16(p + 0x34D0)) > 0 || !mmFree(p)) return 0;
    mmAirControl(p);
    return kAutoJumpAfterChase;
}

// 97: procFall after its prologue (r31 the player): MM's air control (Wind Waker's fall keeps its speed and heading).
u32 hookMmFallAir(Ctx& c) {
    const u32 p = c.r[31];
    if (!mmFree(p) || rd16(p + 0x34D6) != 0) return 0;  // (34D6: the Hover Boots' own slow-down)
    mmAirControl(p);
    return 0;
}

// MM's flips and landings, each frame's world turn of the 16 bones from MM's regular running jump (run_jump) or its
// landing (run_jump_end) at the same frame, and the root's move (MM's model units).
constexpr float kFlipFrontTurn[13][16][4] = {
    {{0.1350f, 0.0000f, -0.0000f, 0.9908f}, {0.1350f, 0.0000f, -0.0000f, 0.9908f}, {-0.0336f, 0.0125f, -0.0028f, 0.9994f}, {-0.0751f, 0.0255f, -0.0106f, 0.9968f}, {0.5148f, -0.0225f, 0.0141f, 0.8569f}, {0.8486f, 0.2675f, 0.3100f, 0.3351f}, {0.8866f, 0.1842f, 0.2370f, 0.3519f}, {0.4034f, 0.1195f, 0.0612f, 0.9051f}, {0.7924f, -0.3699f, -0.1563f, 0.4593f}, {0.8141f, -0.1784f, -0.1577f, 0.5297f}, {0.1463f, 0.0644f, 0.0583f, 0.9854f}, {0.0331f, 0.0744f, 0.0457f, 0.9956f}, {0.0373f, 0.1816f, 0.0506f, 0.9814f}, {-0.2131f, 0.0090f, -0.0550f, 0.9754f}, {-0.2232f, 0.0078f, -0.0548f, 0.9732f}, {0.1415f, -0.0601f, -0.0711f, 0.9856f}},
    {{0.1169f, 0.0000f, -0.0000f, 0.9931f}, {0.1169f, 0.0000f, -0.0000f, 0.9931f}, {0.0651f, 0.0086f, -0.0270f, 0.9975f}, {-0.0735f, 0.0252f, -0.0502f, 0.9957f}, {0.5388f, 0.0012f, 0.1799f, 0.8230f}, {0.7435f, 0.3436f, 0.4624f, 0.3395f}, {0.8003f, 0.2985f, 0.3008f, 0.4242f}, {0.3008f, 0.1980f, -0.1609f, 0.9189f}, {0.6267f, -0.3389f, -0.2744f, 0.6458f}, {0.6473f, -0.1768f, -0.2828f, 0.6854f}, {0.1387f, 0.0123f, 0.0765f, 0.9873f}, {-0.2345f, 0.0517f, 0.0503f, 0.9694f}, {-0.0306f, 0.1401f, 0.0983f, 0.9848f}, {0.0400f, 0.0356f, -0.0537f, 0.9971f}, {-0.3635f, -0.0089f, -0.0324f, 0.9310f}, {0.0692f, -0.0584f, -0.1007f, 0.9908f}},
    {{0.2175f, 0.0000f, 0.0000f, 0.9761f}, {0.2175f, 0.0000f, 0.0000f, 0.9761f}, {0.3134f, -0.0061f, -0.0480f, 0.9484f}, {0.0903f, 0.0169f, -0.0816f, 0.9924f}, {0.5830f, 0.0111f, 0.2259f, 0.7803f}, {0.7007f, 0.3183f, 0.5017f, 0.3950f}, {0.7806f, 0.2985f, 0.2107f, 0.5071f}, {0.3026f, 0.2204f, -0.2722f, 0.8865f}, {0.5437f, -0.2298f, -0.3717f, 0.7166f}, {0.5499f, -0.1021f, -0.3713f, 0.7411f}, {-0.0539f, -0.0473f, 0.0322f, 0.9969f}, {-0.3264f, -0.0215f, 0.0273f, 0.9446f}, {-0.0318f, 0.0573f, 0.0911f, 0.9937f}, {0.2129f, 0.0189f, -0.0666f, 0.9746f}, {-0.4791f, -0.0525f, -0.0196f, 0.8759f}, {-0.1460f, -0.0949f, -0.1031f, 0.9793f}},
    {{0.5269f, 0.0000f, 0.0000f, 0.8499f}, {0.5269f, 0.0000f, 0.0000f, 0.8499f}, {0.6878f, -0.0081f, -0.0606f, 0.7233f}, {0.4890f, 0.0386f, -0.0795f, 0.8678f}, {0.6363f, 0.1113f, -0.0073f, 0.7633f}, {0.7598f, 0.2143f, 0.1355f, 0.5987f}, {0.7277f, 0.1673f, -0.2802f, 0.6033f}, {0.4408f, 0.2041f, -0.0310f, 0.8735f}, {0.5925f, 0.0466f, -0.1231f, 0.7947f}, {0.5499f, 0.1399f, -0.1081f, 0.8163f}, {-0.3109f, -0.0606f, 0.0022f, 0.9485f}, {-0.2101f, -0.0684f, 0.0005f, 0.9753f}, {0.0743f, 0.0122f, 0.0763f, 0.9942f}, {0.0368f, -0.0291f, -0.0576f, 0.9972f}, {-0.2289f, -0.0571f, -0.0328f, 0.9712f}, {-0.0637f, -0.0840f, -0.1144f, 0.9878f}},
    {{0.8600f, 0.0000f, 0.0000f, 0.5103f}, {0.8600f, 0.0000f, 0.0000f, 0.5103f}, {0.9535f, 0.0036f, -0.0732f, 0.2922f}, {0.8763f, 0.0642f, -0.0406f, 0.4757f}, {0.7140f, 0.1530f, -0.2487f, 0.6363f}, {0.7090f, 0.1521f, -0.2526f, 0.6406f}, {0.5200f, 0.1717f, -0.6506f, 0.5261f}, {0.7372f, -0.0156f, 0.1645f, 0.6551f}, {0.7571f, -0.0270f, 0.1484f, 0.6356f}, {0.7039f, 0.0334f, 0.1760f, 0.6874f}, {-0.3537f, -0.0300f, 0.0284f, 0.9345f}, {-0.0357f, -0.0436f, 0.0070f, 0.9984f}, {0.2283f, 0.0289f, 0.0916f, 0.9688f}, {0.1809f, -0.0795f, -0.0521f, 0.9789f}, {0.0800f, -0.0903f, -0.0395f, 0.9919f}, {0.1169f, -0.1180f, -0.1035f, 0.9807f}},
    {{0.9941f, 0.0000f, 0.0000f, 0.1086f}, {0.9941f, 0.0000f, 0.0000f, 0.1086f}, {-0.9770f, -0.0247f, 0.0919f, 0.1906f}, {-0.9969f, -0.0586f, 0.0025f, 0.0531f}, {0.9492f, 0.1768f, -0.0749f, 0.2494f}, {0.9390f, 0.1779f, -0.0943f, 0.2788f}, {0.7321f, 0.4787f, -0.3575f, 0.3272f}, {0.9833f, -0.0906f, 0.0434f, 0.1515f}, {0.9911f, -0.0965f, -0.0079f, 0.0909f}, {0.9833f, -0.0565f, 0.0473f, 0.1664f}, {-0.1288f, 0.0069f, 0.1475f, 0.9806f}, {0.3384f, 0.0517f, 0.0737f, 0.9367f}, {0.5471f, 0.1020f, 0.1584f, 0.8156f}, {0.5270f, -0.1368f, -0.0306f, 0.8382f}, {0.6670f, -0.1206f, -0.0456f, 0.7338f}, {0.6110f, -0.1492f, -0.0918f, 0.7720f}},
    {{-0.9611f, -0.0000f, -0.0000f, 0.2760f}, {-0.9611f, -0.0000f, -0.0000f, 0.2760f}, {-0.7691f, -0.0729f, 0.1015f, 0.6268f}, {-0.8178f, -0.0364f, -0.0182f, 0.5741f}, {-0.9528f, 0.0119f, -0.0178f, 0.3030f}, {-0.9516f, 0.0132f, -0.0205f, 0.3063f}, {-0.9289f, -0.3535f, -0.0807f, 0.0754f}, {-0.8880f, -0.0046f, 0.0211f, 0.4593f}, {-0.8111f, -0.0765f, 0.1472f, 0.5609f}, {-0.8573f, -0.0758f, 0.0663f, 0.5049f}, {0.5640f, 0.0409f, -0.1720f, 0.8066f}, {0.8500f, -0.0192f, -0.2268f, 0.4750f}, {0.9372f, -0.1109f, -0.1194f, 0.3085f}, {-0.9963f, 0.0636f, 0.0465f, 0.0335f}, {-0.9367f, 0.0203f, 0.0466f, 0.3463f}, {-0.9728f, 0.0232f, 0.0939f, 0.2105f}},
    {{-0.8151f, 0.0000f, -0.0000f, 0.5793f}, {-0.8151f, 0.0000f, -0.0000f, 0.5793f}, {-0.4508f, -0.1329f, 0.0861f, 0.8785f}, {-0.4600f, -0.0193f, -0.0172f, 0.8875f}, {-0.6711f, 0.0995f, 0.1973f, 0.7077f}, {-0.6337f, 0.1525f, 0.1604f, 0.7412f}, {-0.8510f, -0.0307f, -0.0499f, 0.5218f}, {-0.5042f, -0.0659f, -0.0766f, 0.8577f}, {-0.3239f, -0.3228f, 0.0481f, 0.8880f}, {-0.3874f, -0.2690f, -0.0316f, 0.8812f}, {-0.9507f, -0.1180f, 0.2816f, 0.0536f}, {0.8759f, 0.1775f, -0.1690f, 0.4157f}, {0.9601f, 0.0802f, -0.1066f, 0.2459f}, {-0.5080f, -0.0207f, 0.0256f, 0.8607f}, {-0.7264f, 0.0046f, 0.0394f, 0.6861f}, {-0.8260f, -0.0074f, 0.0873f, 0.5568f}},
    {{-0.6608f, 0.0000f, -0.0000f, 0.7505f}, {-0.6608f, 0.0000f, -0.0000f, 0.7505f}, {-0.2637f, -0.1797f, 0.0675f, 0.9453f}, {-0.2224f, -0.0172f, -0.0070f, 0.9748f}, {-0.4436f, -0.0863f, 0.3802f, 0.8069f}, {-0.3796f, 0.0483f, 0.3475f, 0.8560f}, {-0.6915f, -0.0119f, 0.1774f, 0.7001f}, {-0.2035f, -0.0153f, -0.1698f, 0.9641f}, {-0.0031f, -0.4465f, -0.1384f, 0.8840f}, {-0.0503f, -0.3617f, -0.2012f, 0.9090f}, {-0.7394f, -0.1878f, 0.2571f, 0.5932f}, {0.9538f, 0.2044f, -0.0135f, 0.2197f}, {0.9940f, 0.1066f, 0.0156f, 0.0177f}, {0.0746f, -0.0135f, -0.0118f, 0.9970f}, {-0.5924f, 0.0080f, 0.0351f, 0.8048f}, {-0.6725f, -0.0244f, 0.0865f, 0.7346f}},
    {{-0.5556f, 0.0000f, -0.0000f, 0.8314f}, {-0.5556f, 0.0000f, -0.0000f, 0.8314f}, {-0.2101f, -0.2161f, 0.0573f, 0.9518f}, {-0.1279f, -0.0225f, 0.0035f, 0.9915f}, {-0.3667f, -0.3123f, 0.4005f, 0.7795f}, {-0.3003f, -0.1323f, 0.3976f, 0.8569f}, {-0.6205f, -0.1382f, 0.2772f, 0.7204f}, {-0.0912f, 0.0183f, -0.1961f, 0.9762f}, {0.0964f, -0.5213f, -0.2486f, 0.8107f}, {0.0632f, -0.4247f, -0.3050f, 0.8501f}, {-0.5275f, -0.1989f, 0.2095f, 0.7989f}, {-0.9876f, -0.1461f, -0.0462f, 0.0346f}, {-0.9465f, -0.0362f, -0.0159f, 0.3204f}, {0.2067f, 0.0227f, -0.0397f, 0.9773f}, {-0.4161f, 0.0111f, -0.0073f, 0.9092f}, {-0.3806f, -0.0469f, 0.0411f, 0.9226f}},
    {{-0.4488f, 0.0000f, -0.0000f, 0.8936f}, {-0.4488f, 0.0000f, -0.0000f, 0.8936f}, {-0.1913f, -0.2440f, 0.0435f, 0.9497f}, {-0.0665f, -0.0313f, 0.0067f, 0.9973f}, {-0.2871f, -0.5076f, 0.3880f, 0.7137f}, {-0.2356f, -0.2973f, 0.4269f, 0.8209f}, {-0.5474f, -0.2884f, 0.3430f, 0.7068f}, {-0.0958f, -0.0285f, -0.2466f, 0.9640f}, {0.0751f, -0.6503f, -0.3142f, 0.6876f}, {0.0595f, -0.5525f, -0.3782f, 0.7404f}, {-0.2700f, -0.1674f, 0.1954f, 0.9278f}, {-0.9568f, -0.0694f, -0.0517f, 0.2775f}, {-0.7747f, 0.0416f, 0.0491f, 0.6290f}, {0.3237f, 0.0521f, -0.0743f, 0.9418f}, {-0.2424f, 0.0191f, -0.0692f, 0.9675f}, {-0.0616f, -0.0553f, -0.0242f, 0.9963f}},
    {{-0.3020f, 0.0000f, -0.0000f, 0.9533f}, {-0.3020f, 0.0000f, -0.0000f, 0.9533f}, {-0.1723f, -0.2581f, 0.0227f, 0.9504f}, {0.0159f, -0.0408f, -0.0036f, 0.9990f}, {-0.1497f, -0.5509f, 0.3913f, 0.7218f}, {-0.1162f, -0.3086f, 0.4689f, 0.8194f}, {-0.4212f, -0.3142f, 0.4578f, 0.7171f}, {-0.1392f, -0.0677f, -0.3475f, 0.9248f}, {-0.0147f, -0.7461f, -0.3846f, 0.5433f}, {-0.0005f, -0.6607f, -0.4513f, 0.5998f}, {-0.0260f, -0.1176f, 0.1320f, 0.9839f}, {-0.6395f, -0.0028f, 0.0192f, 0.7685f}, {-0.3175f, 0.0777f, 0.1586f, 0.9317f}, {0.4802f, 0.0813f, -0.0653f, 0.8710f}, {0.3534f, 0.0683f, -0.0735f, 0.9301f}, {0.5288f, -0.0218f, -0.0629f, 0.8461f}},
    {{-0.1674f, 0.0000f, -0.0000f, 0.9859f}, {-0.1674f, 0.0000f, -0.0000f, 0.9859f}, {-0.2521f, -0.2419f, 0.0239f, 0.9367f}, {0.0739f, -0.0411f, -0.0384f, 0.9957f}, {-0.1817f, -0.4742f, 0.3621f, 0.7817f}, {-0.1469f, -0.2184f, 0.4282f, 0.8645f}, {-0.4202f, -0.2001f, 0.5007f, 0.7299f}, {-0.2683f, -0.1181f, -0.3901f, 0.8729f}, {-0.1246f, -0.8066f, -0.3504f, 0.4594f}, {-0.0798f, -0.7547f, -0.3991f, 0.5145f}, {0.1703f, -0.0979f, 0.1511f, 0.9688f}, {-0.5040f, 0.0349f, 0.0439f, 0.8619f}, {-0.4386f, 0.1546f, 0.0908f, 0.8806f}, {0.5721f, 0.0986f, -0.0677f, 0.8114f}, {0.4494f, 0.0848f, -0.0777f, 0.8859f}, {0.3896f, -0.0104f, -0.0722f, 0.9181f}},
};
constexpr float kFlipFrontMove[13][3] = {{-56.0f, 41.0f, -22.0f}, {-59.0f, 391.0f, 60.0f}, {-64.0f, 503.0f, 71.0f}, {-68.0f, 472.0f, 71.0f}, {-72.0f, 526.0f, 135.0f}, {-78.0f, 574.0f, 210.0f}, {-85.0f, 618.0f, 292.0f}, {-92.0f, 653.0f, 381.0f}, {-97.0f, 680.0f, 472.0f}, {-102.0f, 698.0f, 566.0f}, {-104.0f, 702.0f, 657.0f}, {-104.0f, 693.0f, 747.0f}, {-100.0f, 671.0f, 829.0f}};
constexpr float kFlipSideTurn[14][16][4] = {
    {{0.4857f, 0.3439f, 0.1193f, 0.7947f}, {0.4857f, 0.3439f, 0.1193f, 0.7947f}, {0.3765f, 0.1017f, 0.3447f, 0.8539f}, {0.1519f, -0.0490f, 0.2614f, 0.9519f}, {0.6730f, 0.4126f, 0.0849f, 0.6080f}, {0.8362f, 0.5089f, 0.1361f, 0.1523f}, {0.8742f, 0.4469f, 0.0965f, 0.1636f}, {0.6242f, 0.2183f, -0.0002f, 0.7501f}, {0.8260f, -0.3282f, -0.4055f, 0.2134f}, {0.8794f, -0.1287f, -0.3850f, 0.2486f}, {-0.1360f, 0.2765f, 0.0377f, 0.9506f}, {-0.1165f, 0.2754f, 0.0362f, 0.9536f}, {-0.1164f, 0.3859f, 0.0202f, 0.9149f}, {0.0619f, 0.4040f, 0.1029f, 0.9068f}, {0.1766f, 0.4328f, 0.0546f, 0.8823f}, {0.6263f, 0.3886f, -0.2850f, 0.6129f}},
    {{0.4738f, 0.5548f, 0.4746f, 0.4925f}, {0.4738f, 0.5548f, 0.4746f, 0.4925f}, {0.6986f, 0.2449f, 0.4257f, 0.5203f}, {0.5644f, 0.3116f, 0.4688f, 0.6038f}, {0.7923f, 0.2742f, 0.4338f, 0.3301f}, {-0.6672f, -0.4367f, -0.5559f, 0.2348f}, {-0.6824f, -0.4651f, -0.5239f, 0.2088f}, {0.6093f, 0.4389f, 0.3318f, 0.5710f}, {0.9255f, 0.1307f, -0.2957f, 0.1972f}, {0.8946f, 0.2987f, -0.2447f, 0.2249f}, {0.0862f, 0.5507f, 0.0989f, 0.8243f}, {-0.1518f, 0.5451f, 0.2233f, 0.7937f}, {0.1056f, 0.6388f, 0.1449f, 0.7482f}, {0.4617f, 0.5663f, 0.4741f, 0.4913f}, {0.1027f, 0.0912f, 0.6793f, 0.7209f}, {0.3769f, 0.3747f, 0.5017f, 0.6825f}},
    {{0.4540f, 0.5405f, 0.5237f, 0.4769f}, {0.4540f, 0.5405f, 0.5237f, 0.4769f}, {0.8079f, 0.2338f, 0.4339f, 0.3230f}, {0.6850f, 0.1753f, 0.5400f, 0.4566f}, {0.7121f, 0.1328f, 0.6880f, 0.0435f}, {-0.2436f, -0.3326f, -0.7803f, 0.4702f}, {-0.2309f, -0.3970f, -0.8082f, 0.3687f}, {0.5165f, 0.4436f, 0.3928f, 0.6182f}, {0.9184f, 0.1990f, -0.1841f, 0.2883f}, {0.8654f, 0.3318f, -0.1617f, 0.3389f}, {0.2601f, 0.5910f, 0.0916f, 0.7581f}, {-0.3727f, 0.3877f, 0.5411f, 0.6465f}, {0.0164f, 0.6265f, 0.3614f, 0.6904f}, {0.4943f, 0.5470f, 0.4987f, 0.4558f}, {0.1018f, -0.0469f, 0.6988f, 0.7065f}, {0.2217f, 0.0817f, 0.6261f, 0.7431f}},
    {{0.5086f, 0.4058f, 0.6625f, 0.3711f}, {0.5086f, 0.4058f, 0.6625f, 0.3711f}, {0.8434f, 0.1935f, 0.4933f, 0.0886f}, {0.7385f, 0.0709f, 0.5924f, 0.3140f}, {0.7247f, 0.0491f, 0.6839f, 0.0689f}, {-0.2664f, -0.2751f, -0.8379f, 0.3891f}, {-0.2361f, -0.3403f, -0.8814f, 0.2273f}, {0.5970f, 0.3813f, 0.4405f, 0.5516f}, {0.9413f, 0.2232f, -0.1579f, 0.1980f}, {0.8936f, 0.3366f, -0.1336f, 0.2651f}, {0.3366f, 0.5964f, 0.3060f, 0.6613f}, {-0.1712f, 0.2313f, 0.6834f, 0.6709f}, {0.1266f, 0.5210f, 0.5670f, 0.6253f}, {0.5773f, 0.4582f, 0.5888f, 0.3319f}, {0.2492f, -0.2229f, 0.6858f, 0.6465f}, {0.2679f, -0.2114f, 0.6392f, 0.6892f}},
    {{0.6003f, 0.2829f, 0.7306f, 0.1608f}, {0.6003f, 0.2829f, 0.7306f, 0.1608f}, {0.7936f, 0.1667f, 0.5842f, 0.0341f}, {0.6888f, 0.0064f, 0.6687f, 0.2798f}, {0.7738f, -0.0729f, 0.6034f, 0.1783f}, {-0.4412f, -0.1795f, -0.8499f, 0.2254f}, {-0.3961f, -0.2328f, -0.8873f, 0.0390f}, {0.5888f, 0.2598f, 0.5701f, 0.5108f}, {0.9616f, 0.2263f, -0.0242f, 0.1531f}, {0.9146f, 0.3226f, 0.0069f, 0.2438f}, {0.4395f, 0.5012f, 0.3970f, 0.6309f}, {-0.0791f, 0.0792f, 0.6900f, 0.7151f}, {0.1844f, 0.3557f, 0.6509f, 0.6448f}, {0.7017f, 0.4171f, 0.5726f, 0.0762f}, {0.4696f, -0.3579f, 0.6004f, 0.5394f}, {0.4620f, -0.3792f, 0.5484f, 0.5849f}},
    {{0.6606f, 0.2512f, 0.7074f, 0.0147f}, {0.6606f, 0.2512f, 0.7074f, 0.0147f}, {-0.7885f, -0.1385f, -0.5980f, 0.0378f}, {0.6876f, -0.0260f, 0.6868f, 0.2343f}, {0.8629f, -0.2654f, 0.4025f, 0.1518f}, {-0.6479f, 0.0934f, -0.7442f, 0.1330f}, {0.6214f, -0.0507f, 0.7792f, 0.0644f}, {0.5494f, 0.1896f, 0.6759f, 0.4532f}, {0.9589f, 0.2384f, 0.0814f, 0.1304f}, {0.9084f, 0.3206f, 0.1211f, 0.2397f}, {0.5570f, 0.4073f, 0.4188f, 0.5903f}, {0.0185f, -0.0302f, 0.6450f, 0.7633f}, {0.2589f, 0.2069f, 0.6608f, 0.6735f}, {-0.7261f, -0.3596f, -0.5322f, 0.2453f}, {0.7470f, -0.2095f, 0.5986f, 0.1996f}, {0.7519f, -0.2299f, 0.5615f, 0.2579f}},
    {{-0.7097f, -0.2350f, -0.6595f, 0.0780f}, {-0.7097f, -0.2350f, -0.6595f, 0.0780f}, {-0.7939f, -0.0899f, -0.5985f, 0.0583f}, {0.6631f, -0.0619f, 0.7102f, 0.2282f}, {0.8711f, -0.4356f, 0.2201f, 0.0545f}, {-0.7430f, 0.3592f, -0.5535f, 0.1120f}, {0.7327f, -0.3495f, 0.5776f, 0.0862f}, {0.5025f, 0.1689f, 0.7108f, 0.4623f}, {0.9524f, 0.2112f, 0.1072f, 0.1919f}, {0.8928f, 0.2910f, 0.1506f, 0.3091f}, {0.7098f, 0.3031f, 0.4668f, 0.4317f}, {0.1998f, -0.1763f, 0.5996f, 0.7546f}, {0.3996f, 0.0176f, 0.6699f, 0.6255f}, {-0.5997f, -0.3121f, -0.4884f, 0.5518f}, {-0.8133f, 0.1164f, -0.5470f, 0.1607f}, {-0.8383f, 0.1075f, -0.5208f, 0.1206f}},
    {{-0.7241f, -0.1799f, -0.6546f, 0.1216f}, {-0.7241f, -0.1799f, -0.6546f, 0.1216f}, {-0.7997f, -0.0132f, -0.5966f, 0.0663f}, {0.5609f, -0.0980f, 0.7856f, 0.2421f}, {-0.8526f, 0.5117f, -0.0984f, 0.0399f}, {-0.7791f, 0.4951f, -0.3596f, 0.1362f}, {0.7676f, -0.5224f, 0.3679f, 0.0501f}, {0.5213f, 0.1279f, 0.6953f, 0.4778f}, {0.9631f, 0.1446f, 0.0665f, 0.2171f}, {0.9051f, 0.2314f, 0.1155f, 0.3377f}, {0.7900f, 0.1492f, 0.5182f, 0.2917f}, {0.4407f, -0.2480f, 0.5506f, 0.6642f}, {0.5894f, -0.1040f, 0.6388f, 0.4835f}, {-0.4168f, -0.2740f, -0.4559f, 0.7371f}, {-0.7734f, 0.1054f, -0.4896f, 0.3887f}, {-0.7918f, 0.0613f, -0.4709f, 0.3840f}},
    {{-0.6691f, -0.0284f, -0.7335f, 0.1164f}, {-0.6691f, -0.0284f, -0.7335f, 0.1164f}, {-0.7780f, 0.0940f, -0.6095f, 0.1201f}, {0.4180f, -0.2039f, 0.8603f, 0.2090f}, {-0.8553f, 0.4988f, -0.0144f, 0.1395f}, {-0.8071f, 0.5145f, -0.2048f, 0.2049f}, {-0.7939f, 0.5731f, -0.1999f, 0.0368f}, {0.5533f, -0.0216f, 0.6872f, 0.4702f}, {0.9848f, 0.0340f, 0.0641f, 0.1579f}, {0.9430f, 0.1169f, 0.1190f, 0.2880f}, {0.7655f, -0.0769f, 0.6156f, 0.1707f}, {0.6808f, -0.2479f, 0.5991f, 0.3408f}, {0.7317f, -0.1303f, 0.6592f, 0.1143f}, {-0.2763f, -0.1021f, -0.5785f, 0.7607f}, {-0.5120f, 0.1093f, -0.5611f, 0.6411f}, {-0.5115f, 0.0421f, -0.5599f, 0.6505f}},
    {{-0.5266f, 0.2073f, -0.8219f, 0.0655f}, {-0.5266f, 0.2073f, -0.8219f, 0.0655f}, {-0.7087f, 0.2322f, -0.6343f, 0.2037f}, {0.2926f, -0.4028f, 0.8625f, 0.0902f}, {-0.8521f, 0.4529f, 0.0099f, 0.2623f}, {-0.8008f, 0.4872f, -0.1488f, 0.3150f}, {-0.7946f, 0.5689f, -0.1244f, 0.1719f}, {0.5247f, -0.2795f, 0.6587f, 0.4611f}, {0.9804f, -0.1485f, 0.1209f, 0.0469f}, {0.9611f, -0.0860f, 0.1719f, 0.1984f}, {-0.6651f, 0.3038f, -0.6572f, 0.1827f}, {-0.6593f, 0.4693f, -0.5869f, 0.0259f}, {-0.6103f, 0.3698f, -0.6616f, 0.2304f}, {0.0300f, 0.1608f, -0.8739f, 0.4578f}, {-0.1626f, 0.4763f, -0.7397f, 0.4466f}, {-0.1784f, 0.4106f, -0.7670f, 0.4596f}},
    {{0.3428f, -0.4582f, 0.8201f, 0.0013f}, {0.3428f, -0.4582f, 0.8201f, 0.0013f}, {-0.6149f, 0.3708f, -0.6363f, 0.2820f}, {-0.1700f, 0.5762f, -0.7993f, 0.0118f}, {-0.8381f, 0.4146f, -0.0258f, 0.3536f}, {-0.7674f, 0.4643f, -0.1853f, 0.4014f}, {-0.7686f, 0.5528f, -0.1424f, 0.2888f}, {0.4409f, -0.5717f, 0.5518f, 0.4174f}, {-0.9095f, 0.3597f, -0.1759f, 0.1121f}, {0.9181f, -0.3291f, 0.2133f, 0.0575f}, {-0.3667f, 0.4540f, -0.6906f, 0.4271f}, {-0.4423f, 0.7945f, -0.3901f, 0.1447f}, {-0.3419f, 0.7324f, -0.5121f, 0.2907f}, {0.0837f, 0.2847f, -0.9528f, 0.0642f}, {-0.0373f, 0.7490f, -0.6526f, 0.1083f}, {-0.0637f, 0.6979f, -0.7012f, 0.1313f}},
    {{0.2101f, -0.6435f, 0.7334f, 0.0619f}, {0.2101f, -0.6435f, 0.7334f, 0.0619f}, {-0.5966f, 0.4712f, -0.5699f, 0.3118f}, {-0.1751f, 0.6696f, -0.7216f, 0.0135f}, {-0.8453f, 0.4076f, -0.0295f, 0.3441f}, {-0.7633f, 0.4722f, -0.2086f, 0.3884f}, {-0.7642f, 0.5508f, -0.1593f, 0.2954f}, {0.4193f, -0.7507f, 0.3826f, 0.3381f}, {-0.8288f, 0.4761f, -0.1270f, 0.2651f}, {-0.8649f, 0.4623f, -0.1634f, 0.1072f}, {-0.0569f, 0.4055f, -0.8049f, 0.4295f}, {-0.2026f, 0.9767f, -0.0525f, 0.0482f}, {-0.0874f, 0.9663f, -0.2095f, 0.1211f}, {-0.0685f, -0.3476f, 0.9331f, 0.0616f}, {0.0145f, -0.7684f, 0.6398f, 0.0087f}, {-0.0363f, 0.7144f, -0.6987f, 0.0139f}},
    {{0.1572f, -0.7390f, 0.6432f, 0.1247f}, {0.1572f, -0.7390f, 0.6432f, 0.1247f}, {-0.5956f, 0.5612f, -0.4793f, 0.3171f}, {0.2280f, -0.7391f, 0.6336f, 0.0160f}, {-0.8445f, 0.4337f, -0.0263f, 0.3132f}, {-0.7570f, 0.5096f, -0.2198f, 0.3449f}, {-0.7548f, 0.5763f, -0.1662f, 0.2655f}, {0.5343f, -0.7720f, 0.2248f, 0.2610f}, {-0.8200f, 0.4441f, 0.0305f, 0.3598f}, {-0.8696f, 0.4209f, -0.0364f, 0.2557f}, {0.1270f, 0.7439f, -0.6466f, 0.1112f}, {-0.0929f, -0.9128f, -0.3879f, 0.0883f}, {-0.1925f, -0.9476f, -0.2238f, 0.1219f}, {-0.0575f, -0.4312f, 0.8851f, 0.1652f}, {-0.0252f, -0.7836f, 0.6125f, 0.1007f}, {-0.0088f, -0.7193f, 0.6893f, 0.0867f}},
    {{0.1271f, -0.7731f, 0.5893f, 0.1972f}, {0.1271f, -0.7731f, 0.5893f, 0.1972f}, {-0.4830f, 0.6905f, -0.4220f, 0.3344f}, {0.1832f, -0.8418f, 0.5041f, 0.0607f}, {-0.7304f, 0.5720f, -0.0656f, 0.3675f}, {-0.6262f, 0.6435f, -0.2538f, 0.3596f}, {-0.6193f, 0.7015f, -0.1885f, 0.2979f}, {0.3376f, -0.8639f, 0.1194f, 0.3542f}, {-0.6790f, 0.6574f, -0.0112f, 0.3265f}, {-0.7376f, 0.6392f, -0.0476f, 0.2124f}, {-0.1011f, -0.8765f, 0.4536f, 0.1253f}, {-0.2231f, -0.7392f, -0.5895f, 0.2372f}, {-0.2789f, -0.7795f, -0.4619f, 0.3183f}, {-0.0142f, -0.6013f, 0.7400f, 0.3011f}, {-0.0523f, -0.8717f, 0.4303f, 0.2288f}, {-0.0282f, -0.8432f, 0.4916f, 0.2158f}},
};
constexpr float kFlipSideMove[14][3] = {{-38.0f, -399.0f, 83.0f}, {-40.0f, 112.0f, 386.0f}, {-47.0f, 1252.0f, 437.0f}, {-52.0f, 1263.0f, 330.0f}, {-56.0f, 1257.0f, 272.0f}, {-62.0f, 1184.0f, 221.0f}, {-72.0f, 1089.0f, 190.0f}, {-85.0f, 1013.0f, 188.0f}, {-104.0f, 952.0f, 207.0f}, {-127.0f, 878.0f, 233.0f}, {-149.0f, 796.0f, 269.0f}, {-167.0f, 713.0f, 316.0f}, {-176.0f, 635.0f, 376.0f}, {-170.0f, 445.0f, 458.0f}};
constexpr float kFlipFrontLandTurn[13][16][4] = {
    {{-0.0273f, 0.0000f, -0.0000f, 0.9996f}, {-0.0273f, 0.0000f, -0.0000f, 0.9996f}, {-0.1622f, 0.0690f, -0.0024f, 0.9843f}, {0.2387f, 0.0533f, -0.0615f, 0.9677f}, {-0.1388f, -0.3019f, 0.1071f, 0.9371f}, {-0.1118f, -0.0561f, 0.1941f, 0.9730f}, {-0.4054f, -0.0363f, 0.2592f, 0.8759f}, {-0.2139f, 0.0327f, -0.3337f, 0.9175f}, {-0.1085f, -0.5109f, -0.4411f, 0.7298f}, {0.1564f, -0.6284f, -0.2497f, 0.7199f}, {0.5370f, -0.0001f, 0.1461f, 0.8308f}, {0.1962f, 0.0380f, 0.0725f, 0.9771f}, {0.2299f, 0.1285f, 0.0474f, 0.9635f}, {0.5518f, -0.1149f, -0.1058f, 0.8192f}, {0.2779f, -0.1467f, -0.0512f, 0.9480f}, {0.2865f, -0.2387f, -0.0091f, 0.9278f}},
    {{-0.0242f, 0.0000f, -0.0000f, 0.9997f}, {-0.0242f, 0.0000f, -0.0000f, 0.9997f}, {-0.0491f, 0.1426f, 0.0239f, 0.9883f}, {0.2879f, 0.0397f, -0.1065f, 0.9509f}, {-0.0835f, -0.2904f, -0.1213f, 0.9455f}, {-0.0238f, -0.0837f, -0.0121f, 0.9961f}, {-0.3178f, -0.0096f, 0.1110f, 0.9416f}, {-0.1744f, 0.0252f, -0.1909f, 0.9657f}, {-0.0325f, -0.3716f, -0.4209f, 0.8268f}, {0.3547f, -0.5328f, -0.2345f, 0.7317f}, {0.4863f, 0.0854f, 0.1479f, 0.8570f}, {0.2921f, 0.1130f, 0.0966f, 0.9448f}, {0.0297f, 0.2105f, 0.0602f, 0.9753f}, {0.5140f, -0.1710f, -0.1223f, 0.8317f}, {0.3749f, -0.1917f, -0.0808f, 0.9034f}, {0.0301f, -0.2758f, -0.0393f, 0.9599f}},
    {{-0.0960f, -0.0000f, 0.0000f, 0.9954f}, {-0.0960f, -0.0000f, 0.0000f, 0.9954f}, {0.0175f, 0.1810f, 0.0694f, 0.9809f}, {0.2502f, 0.0527f, -0.1309f, 0.9579f}, {-0.1048f, -0.1826f, -0.3184f, 0.9243f}, {-0.0008f, -0.0069f, -0.2071f, 0.9783f}, {-0.2593f, 0.1149f, 0.0007f, 0.9589f}, {-0.1365f, -0.0170f, -0.0042f, 0.9905f}, {0.0204f, -0.2497f, -0.3084f, 0.9177f}, {0.5329f, -0.3566f, -0.2167f, 0.7361f}, {0.4218f, 0.0672f, 0.1329f, 0.8944f}, {0.4151f, 0.0684f, 0.1317f, 0.8976f}, {-0.0203f, 0.1858f, 0.0291f, 0.9820f}, {0.4316f, -0.1806f, -0.1075f, 0.8772f}, {0.4583f, -0.1760f, -0.1161f, 0.8634f}, {-0.0084f, -0.2638f, -0.0169f, 0.9644f}},
    {{-0.1569f, -0.0000f, -0.0000f, 0.9876f}, {-0.1569f, -0.0000f, -0.0000f, 0.9876f}, {-0.0077f, 0.1954f, 0.1004f, 0.9755f}, {0.1270f, 0.0470f, -0.1355f, 0.9815f}, {-0.1993f, -0.0502f, -0.4020f, 0.8923f}, {-0.0526f, 0.1144f, -0.3069f, 0.9434f}, {-0.2457f, 0.2808f, -0.0255f, 0.9274f}, {-0.1415f, -0.0464f, 0.1648f, 0.9750f}, {-0.0075f, -0.1745f, -0.1632f, 0.9710f}, {0.5722f, -0.1409f, -0.2036f, 0.7818f}, {0.3718f, 0.0471f, 0.1379f, 0.9168f}, {0.4565f, 0.0296f, 0.1460f, 0.8772f}, {-0.0282f, 0.2123f, 0.0319f, 0.9763f}, {0.3742f, -0.2037f, -0.1002f, 0.8991f}, {0.4809f, -0.1824f, -0.1387f, 0.8463f}, {-0.0195f, -0.2637f, -0.0462f, 0.9633f}},
    {{-0.1773f, -0.0000f, -0.0000f, 0.9842f}, {-0.1773f, -0.0000f, -0.0000f, 0.9842f}, {-0.0922f, 0.1983f, 0.1051f, 0.9701f}, {-0.0536f, 0.0050f, -0.1316f, 0.9898f}, {-0.2185f, 0.0614f, -0.3222f, 0.9191f}, {-0.0316f, 0.2175f, -0.2318f, 0.9476f}, {-0.1856f, 0.4421f, 0.0328f, 0.8769f}, {-0.0494f, -0.1431f, 0.2439f, 0.9579f}, {0.0036f, -0.2794f, -0.0860f, 0.9563f}, {0.5186f, -0.1145f, -0.3206f, 0.7843f}, {0.3382f, 0.1583f, 0.1376f, 0.9174f}, {0.2861f, 0.1668f, 0.1278f, 0.9349f}, {-0.0267f, 0.3053f, 0.0385f, 0.9511f}, {0.3306f, -0.2777f, -0.1433f, 0.8905f}, {0.3051f, -0.2813f, -0.1320f, 0.9002f}, {-0.0240f, -0.3195f, -0.0663f, 0.9449f}},
    {{-0.1854f, -0.0000f, -0.0000f, 0.9827f}, {-0.1854f, -0.0000f, -0.0000f, 0.9827f}, {-0.2319f, 0.1365f, 0.0604f, 0.9612f}, {-0.1784f, 0.0089f, -0.0713f, 0.9813f}, {-0.0858f, 0.1616f, -0.2397f, 0.9535f}, {0.0668f, 0.2879f, -0.1303f, 0.9464f}, {-0.0991f, 0.5168f, 0.0248f, 0.8500f}, {-0.1822f, -0.1456f, 0.1804f, 0.9555f}, {-0.1177f, -0.3220f, -0.1127f, 0.9326f}, {0.3916f, -0.1780f, -0.3309f, 0.8399f}, {0.3437f, 0.2291f, 0.1557f, 0.8973f}, {0.0332f, 0.2685f, 0.0724f, 0.9600f}, {0.0171f, 0.3317f, 0.0883f, 0.9391f}, {0.3035f, -0.3242f, -0.1570f, 0.8821f}, {0.1092f, -0.3503f, -0.0548f, 0.9286f}, {0.0640f, -0.3728f, -0.1024f, 0.9200f}},
    {{-0.1830f, -0.0000f, -0.0000f, 0.9831f}, {-0.1830f, -0.0000f, -0.0000f, 0.9831f}, {-0.3820f, 0.0162f, 0.0074f, 0.9240f}, {-0.2763f, -0.0202f, -0.0643f, 0.9587f}, {0.0048f, 0.1072f, -0.1839f, 0.9771f}, {0.0920f, 0.2218f, -0.0794f, 0.9675f}, {-0.0808f, 0.4221f, -0.0262f, 0.9025f}, {-0.3281f, -0.2235f, 0.0335f, 0.9172f}, {-0.2067f, -0.4311f, -0.1989f, 0.8555f}, {0.2890f, -0.3084f, -0.3134f, 0.8504f}, {0.4113f, 0.2147f, 0.1859f, 0.8661f}, {-0.1639f, 0.2856f, 0.0127f, 0.9441f}, {0.0045f, 0.3590f, 0.0838f, 0.9295f}, {0.2978f, -0.3204f, -0.1757f, 0.8819f}, {-0.0078f, -0.3628f, -0.0087f, 0.9318f}, {0.1132f, -0.3647f, -0.1287f, 0.9152f}},
    {{-0.1720f, -0.0000f, -0.0000f, 0.9851f}, {-0.1720f, -0.0000f, -0.0000f, 0.9851f}, {-0.4150f, -0.0607f, -0.0045f, 0.9078f}, {-0.2674f, -0.0161f, -0.0249f, 0.9631f}, {0.1771f, 0.0130f, -0.0253f, 0.9838f}, {0.2287f, 0.1072f, 0.0512f, 0.9662f}, {0.0866f, 0.2858f, 0.0679f, 0.9519f}, {-0.2783f, -0.2665f, -0.0231f, 0.9225f}, {-0.1559f, -0.4000f, -0.1621f, 0.8885f}, {0.2637f, -0.2263f, -0.2159f, 0.9125f}, {0.3602f, 0.1927f, 0.1787f, 0.8951f}, {-0.1749f, 0.2591f, 0.0116f, 0.9498f}, {0.0604f, 0.3304f, 0.1021f, 0.9363f}, {0.2686f, -0.2999f, -0.1634f, 0.9007f}, {-0.0443f, -0.3462f, 0.0035f, 0.9371f}, {0.1243f, -0.3216f, -0.1207f, 0.9309f}},
    {{-0.1541f, -0.0000f, -0.0000f, 0.9881f}, {-0.1541f, -0.0000f, -0.0000f, 0.9881f}, {-0.3942f, -0.0749f, -0.0025f, 0.9160f}, {-0.2393f, 0.0068f, 0.0080f, 0.9709f}, {0.3373f, -0.0488f, 0.0973f, 0.9351f}, {0.3835f, 0.0297f, 0.1477f, 0.9112f}, {0.2965f, 0.2068f, 0.1698f, 0.9168f}, {-0.1986f, -0.2422f, 0.0131f, 0.9496f}, {-0.0975f, -0.3238f, -0.0713f, 0.9384f}, {0.2403f, -0.1171f, -0.0839f, 0.9600f}, {0.2419f, 0.1697f, 0.1390f, 0.9452f}, {-0.1058f, 0.2170f, 0.0264f, 0.9701f}, {0.1387f, 0.2510f, 0.1227f, 0.9501f}, {0.2716f, -0.2654f, -0.1535f, 0.9123f}, {-0.0964f, -0.3273f, 0.0357f, 0.9393f}, {0.0683f, -0.2782f, -0.0797f, 0.9548f}},
    {{-0.1310f, -0.0000f, -0.0000f, 0.9914f}, {-0.1310f, -0.0000f, -0.0000f, 0.9914f}, {-0.3869f, -0.0555f, -0.0001f, 0.9205f}, {-0.2602f, 0.0213f, 0.0094f, 0.9653f}, {0.3328f, -0.0621f, 0.0835f, 0.9372f}, {0.3907f, 0.0085f, 0.1174f, 0.9130f}, {0.3611f, 0.1670f, 0.1419f, 0.9064f}, {-0.2060f, -0.1734f, 0.0922f, 0.9586f}, {-0.0280f, -0.3004f, -0.0307f, 0.9529f}, {0.2541f, -0.0927f, -0.0167f, 0.9626f}, {0.1701f, 0.1282f, 0.0874f, 0.9731f}, {-0.0621f, 0.1683f, 0.0102f, 0.9837f}, {0.1338f, 0.1603f, 0.0750f, 0.9751f}, {0.2629f, -0.2317f, -0.1376f, 0.9264f}, {-0.1205f, -0.3033f, 0.0488f, 0.9440f}, {-0.0015f, -0.2308f, -0.0420f, 0.9721f}},
    {{-0.1045f, -0.0000f, -0.0000f, 0.9945f}, {-0.1045f, -0.0000f, -0.0000f, 0.9945f}, {-0.3661f, -0.0279f, 0.0013f, 0.9302f}, {-0.2838f, 0.0233f, -0.0010f, 0.9586f}, {0.2304f, -0.0406f, -0.0085f, 0.9722f}, {0.3037f, 0.0235f, 0.0171f, 0.9523f}, {0.3141f, 0.1396f, 0.0414f, 0.9382f}, {-0.2163f, -0.1043f, 0.1601f, 0.9574f}, {0.0668f, -0.2843f, -0.0149f, 0.9563f}, {0.3028f, -0.0815f, 0.0034f, 0.9496f}, {0.1897f, 0.0674f, 0.0529f, 0.9781f}, {-0.1008f, 0.1315f, -0.0440f, 0.9852f}, {0.0242f, 0.0870f, -0.0161f, 0.9958f}, {0.1933f, -0.2077f, -0.0689f, 0.9564f}, {-0.0592f, -0.2652f, 0.0462f, 0.9613f}, {-0.0008f, -0.1688f, -0.0439f, 0.9847f}},
    {{-0.0764f, -0.0000f, -0.0000f, 0.9971f}, {-0.0764f, -0.0000f, -0.0000f, 0.9971f}, {-0.3276f, -0.0054f, 0.0006f, 0.9448f}, {-0.2856f, 0.0179f, -0.0088f, 0.9581f}, {0.1301f, -0.0319f, -0.0785f, 0.9879f}, {0.2125f, 0.0230f, -0.0591f, 0.9751f}, {0.2426f, 0.1034f, -0.0346f, 0.9640f}, {-0.2196f, -0.0305f, 0.1968f, 0.9550f}, {0.1629f, -0.2541f, -0.0218f, 0.9531f}, {0.3542f, -0.0704f, -0.0073f, 0.9325f}, {0.1727f, 0.0317f, 0.0597f, 0.9827f}, {-0.1077f, 0.0900f, -0.0282f, 0.9897f}, {0.0116f, 0.0206f, -0.0028f, 0.9997f}, {0.1865f, -0.1737f, -0.0624f, 0.9650f}, {-0.0738f, -0.2342f, 0.0451f, 0.9683f}, {-0.0020f, -0.1269f, -0.0321f, 0.9914f}},
    {{-0.0484f, -0.0000f, -0.0000f, 0.9988f}, {-0.0484f, -0.0000f, -0.0000f, 0.9988f}, {-0.2717f, 0.0000f, 0.0000f, 0.9624f}, {-0.2453f, 0.0130f, -0.0090f, 0.9693f}, {0.0927f, -0.0277f, -0.0766f, 0.9924f}, {0.1660f, 0.0159f, -0.0604f, 0.9841f}, {0.1966f, 0.0779f, -0.0373f, 0.9767f}, {-0.2031f, 0.0130f, 0.1836f, 0.9617f}, {0.2428f, -0.2353f, -0.0466f, 0.9400f}, {0.3925f, -0.0803f, -0.0387f, 0.9154f}, {0.1632f, 0.0001f, 0.0547f, 0.9851f}, {-0.1232f, 0.0589f, -0.0260f, 0.9903f}, {-0.0109f, -0.0231f, -0.0064f, 0.9997f}, {0.1758f, -0.1411f, -0.0646f, 0.9721f}, {-0.0956f, -0.2012f, 0.0351f, 0.9743f}, {-0.0007f, -0.1021f, -0.0010f, 0.9948f}},
};
constexpr float kFlipFrontLandMove[13][3] = {{2.0f, 1177.0f, 1003.0f}, {24.0f, 814.0f, 934.0f}, {37.0f, 408.0f, 798.0f}, {43.0f, 143.0f, 641.0f}, {27.0f, 401.0f, 415.0f}, {14.0f, 886.0f, 166.0f}, {52.0f, 1303.0f, 18.0f}, {102.0f, 1483.0f, -67.0f}, {145.0f, 1285.0f, -127.0f}, {153.0f, 779.0f, -130.0f}, {7.0f, 505.0f, -97.0f}, {-19.0f, 459.0f, -56.0f}, {7.0f, 422.0f, -36.0f}};
constexpr float kFlipSideLandTurn[16][16][4] = {
    {{0.0601f, -0.7823f, 0.5353f, 0.3129f}, {0.0601f, -0.7823f, 0.5353f, 0.3129f}, {-0.2196f, 0.8756f, -0.4266f, 0.0561f}, {0.0893f, -0.8386f, 0.4861f, 0.2290f}, {-0.6005f, 0.7360f, -0.2799f, 0.1391f}, {-0.4496f, 0.7784f, -0.4344f, 0.0574f}, {-0.4580f, 0.8129f, -0.3596f, 0.0102f}, {0.0383f, -0.9732f, -0.1068f, 0.2001f}, {-0.2882f, 0.9159f, -0.0661f, 0.2714f}, {-0.2336f, 0.8312f, -0.3801f, 0.3316f}, {0.0162f, -0.8100f, 0.4930f, 0.3172f}, {-0.0384f, -0.9142f, 0.1139f, 0.3871f}, {-0.0027f, -0.8631f, 0.2977f, 0.4078f}, {0.1974f, -0.5878f, 0.7635f, 0.1806f}, {-0.0776f, -0.9598f, 0.2604f, 0.0703f}, {0.0367f, -0.8695f, 0.4884f, 0.0641f}},
    {{-0.0580f, -0.8172f, 0.3490f, 0.4550f}, {-0.0580f, -0.8172f, 0.3490f, 0.4550f}, {0.0705f, -0.8285f, 0.5236f, 0.1855f}, {-0.0383f, -0.7325f, 0.5550f, 0.3923f}, {0.4448f, -0.7694f, 0.4147f, 0.1951f}, {0.2756f, -0.7267f, 0.5667f, 0.2735f}, {0.2619f, -0.7694f, 0.4985f, 0.3015f}, {0.0077f, -0.9889f, -0.0091f, 0.1481f}, {-0.3248f, 0.9227f, -0.1272f, 0.1642f}, {-0.2832f, 0.7469f, -0.5338f, 0.2776f}, {0.0940f, -0.8099f, 0.3758f, 0.4405f}, {0.0703f, -0.8457f, 0.1968f, 0.4910f}, {0.0079f, -0.8422f, 0.0241f, 0.5386f}, {0.2691f, -0.5315f, 0.7408f, 0.3105f}, {-0.1226f, -0.9053f, 0.3718f, 0.1650f}, {0.0084f, -0.9909f, 0.0267f, 0.1313f}},
    {{-0.1701f, -0.8252f, 0.1711f, 0.5107f}, {-0.1701f, -0.8252f, 0.1711f, 0.5107f}, {0.0300f, -0.9007f, 0.3448f, 0.2627f}, {-0.0407f, -0.7943f, 0.3857f, 0.4677f}, {0.3768f, -0.8340f, 0.3040f, 0.2648f}, {0.2088f, -0.7762f, 0.4691f, 0.3659f}, {0.1930f, -0.8070f, 0.4026f, 0.3866f}, {0.0395f, -0.9511f, -0.2352f, 0.1964f}, {-0.3269f, 0.9289f, 0.1577f, 0.0739f}, {-0.3600f, 0.8294f, -0.3673f, 0.2182f}, {0.2022f, -0.9191f, 0.0652f, 0.3317f}, {0.2027f, -0.9200f, 0.0827f, 0.3250f}, {-0.0367f, -0.9171f, -0.0024f, 0.3970f}, {0.1351f, -0.5366f, 0.7114f, 0.4333f}, {-0.1413f, -0.7775f, 0.5415f, 0.2868f}, {-0.0266f, -0.9680f, 0.0341f, 0.2473f}},
    {{-0.1811f, -0.8512f, 0.1001f, 0.4823f}, {-0.1811f, -0.8512f, 0.1001f, 0.4823f}, {0.0199f, -0.9452f, 0.1222f, 0.3022f}, {-0.0617f, -0.7830f, 0.2346f, 0.5728f}, {0.3281f, -0.8867f, 0.1594f, 0.2842f}, {0.1695f, -0.8220f, 0.3522f, 0.4142f}, {0.1539f, -0.8395f, 0.2899f, 0.4331f}, {0.0197f, -0.8727f, -0.4545f, 0.1771f}, {-0.2953f, 0.8626f, 0.4012f, 0.0880f}, {-0.4026f, 0.8725f, -0.1832f, 0.2077f}, {0.2214f, -0.9154f, 0.0846f, 0.3254f}, {0.2294f, -0.9165f, 0.1623f, 0.2847f}, {-0.0164f, -0.9113f, -0.0023f, 0.4114f}, {0.1158f, -0.6113f, 0.6839f, 0.3810f}, {-0.0865f, -0.7834f, 0.5543f, 0.2677f}, {0.0073f, -0.9616f, -0.0182f, 0.2738f}},
    {{-0.1131f, -0.8477f, 0.0489f, 0.5159f}, {-0.1131f, -0.8477f, 0.0489f, 0.5159f}, {-0.0281f, -0.9134f, -0.0325f, 0.4048f}, {-0.1324f, -0.6621f, 0.1630f, 0.7194f}, {0.1736f, -0.8806f, 0.1248f, 0.4230f}, {0.0520f, -0.7522f, 0.3591f, 0.5500f}, {0.0343f, -0.7589f, 0.3065f, 0.5735f}, {-0.0883f, -0.8890f, -0.4485f, 0.0286f}, {-0.1876f, 0.8350f, 0.4382f, 0.2748f}, {-0.4447f, 0.8246f, -0.1103f, 0.3320f}, {0.1085f, -0.7992f, 0.2625f, 0.5297f}, {0.0878f, -0.8063f, 0.1698f, 0.5598f}, {-0.0832f, -0.8066f, 0.0676f, 0.5813f}, {0.2292f, -0.7054f, 0.5881f, 0.3223f}, {-0.0980f, -0.8950f, 0.3913f, 0.1904f}, {0.0155f, -0.9694f, 0.0128f, 0.2447f}},
    {{-0.0721f, -0.8114f, -0.0543f, 0.5775f}, {-0.0721f, -0.8114f, -0.0543f, 0.5775f}, {-0.1227f, -0.8243f, -0.1621f, 0.5284f}, {-0.1656f, -0.5399f, 0.0823f, 0.8212f}, {0.0534f, -0.7106f, 0.0966f, 0.6949f}, {-0.0193f, -0.5043f, 0.3567f, 0.7861f}, {-0.0338f, -0.4975f, 0.3131f, 0.8083f}, {-0.1886f, -0.8671f, -0.4510f, 0.0961f}, {-0.1099f, 0.8602f, 0.4306f, 0.2500f}, {-0.4012f, 0.8824f, -0.0637f, 0.2375f}, {0.0572f, -0.6872f, 0.3556f, 0.6310f}, {-0.0210f, -0.7114f, 0.0284f, 0.7019f}, {-0.1038f, -0.7548f, 0.0167f, 0.6475f}, {0.2498f, -0.8439f, 0.3775f, 0.2879f}, {-0.0769f, -0.9502f, 0.2164f, 0.2108f}, {0.0689f, -0.9653f, 0.0281f, 0.2505f}},
    {{-0.1030f, -0.7675f, -0.1723f, 0.6088f}, {-0.1030f, -0.7675f, -0.1723f, 0.6088f}, {-0.2355f, -0.7308f, -0.2705f, 0.5807f}, {-0.2175f, -0.4331f, -0.0276f, 0.8743f}, {0.0538f, -0.5366f, -0.0303f, 0.8416f}, {0.0288f, -0.2836f, 0.2401f, 0.9280f}, {0.0159f, -0.2690f, 0.2049f, 0.9410f}, {-0.2462f, -0.8473f, -0.4453f, 0.1520f}, {-0.0897f, 0.8985f, 0.3676f, 0.2227f}, {-0.3492f, 0.9264f, -0.0685f, 0.1230f}, {0.0749f, -0.6541f, 0.4034f, 0.6355f}, {-0.0340f, -0.6761f, -0.0351f, 0.7352f}, {-0.0598f, -0.7188f, 0.0086f, 0.6926f}, {0.2314f, -0.9133f, 0.1845f, 0.2799f}, {0.0557f, -0.9577f, 0.1040f, 0.2626f}, {0.0400f, -0.9398f, 0.2251f, 0.2542f}},
    {{-0.1628f, -0.6502f, -0.1331f, 0.7301f}, {-0.1628f, -0.6502f, -0.1331f, 0.7301f}, {-0.3101f, -0.6758f, -0.2542f, 0.6185f}, {-0.2837f, -0.3568f, -0.0347f, 0.8894f}, {0.0905f, -0.4519f, 0.0127f, 0.8874f}, {0.1335f, -0.1439f, 0.2641f, 0.9443f}, {0.1097f, -0.1115f, 0.2508f, 0.9553f}, {-0.2380f, -0.8512f, -0.3697f, 0.2867f}, {-0.1724f, 0.9487f, 0.2526f, 0.0803f}, {0.3689f, -0.9185f, 0.1305f, 0.0571f}, {0.1696f, -0.5327f, 0.4125f, 0.7192f}, {-0.0369f, -0.5361f, -0.0235f, 0.8430f}, {-0.0005f, -0.5764f, 0.0574f, 0.8151f}, {0.4085f, -0.8064f, 0.0611f, 0.4232f}, {-0.0546f, -0.8939f, -0.0708f, 0.4392f}, {0.0019f, -0.8858f, 0.0027f, 0.4640f}},
    {{-0.1419f, -0.5126f, -0.0952f, 0.8415f}, {-0.1419f, -0.5126f, -0.0952f, 0.8415f}, {-0.3051f, -0.6296f, -0.2679f, 0.6623f}, {-0.2676f, -0.3099f, -0.0506f, 0.9109f}, {0.2091f, -0.4410f, 0.0885f, 0.8683f}, {0.3308f, -0.1175f, 0.3209f, 0.8797f}, {0.3016f, -0.0469f, 0.3360f, 0.8910f}, {-0.2249f, -0.7970f, -0.3378f, 0.4473f}, {0.2321f, -0.9497f, -0.2003f, 0.0641f}, {0.3752f, -0.8969f, 0.1395f, 0.1882f}, {0.1847f, -0.4137f, 0.3323f, 0.8272f}, {-0.0123f, -0.4068f, 0.0674f, 0.9110f}, {0.0752f, -0.4441f, 0.1563f, 0.8790f}, {0.3954f, -0.7012f, -0.0086f, 0.5932f}, {-0.0828f, -0.7851f, -0.0623f, 0.6106f}, {0.0256f, -0.7594f, 0.0033f, 0.6502f}},
    {{-0.1384f, -0.4015f, -0.0360f, 0.9046f}, {-0.1384f, -0.4015f, -0.0360f, 0.9046f}, {-0.3469f, -0.5333f, -0.1998f, 0.7453f}, {-0.2967f, -0.1782f, -0.0156f, 0.9381f}, {0.2446f, -0.3551f, 0.1018f, 0.8965f}, {0.4412f, -0.0417f, 0.2903f, 0.8482f}, {0.4227f, 0.0514f, 0.3221f, 0.8455f}, {-0.2839f, -0.6914f, -0.2095f, 0.6305f}, {0.2550f, -0.9166f, -0.1172f, 0.2849f}, {0.3991f, -0.8150f, 0.1644f, 0.3866f}, {0.1777f, -0.3129f, 0.2313f, 0.9039f}, {0.0510f, -0.2997f, 0.1095f, 0.9464f}, {0.1765f, -0.3254f, 0.1914f, 0.9090f}, {0.3346f, -0.5992f, -0.0407f, 0.7261f}, {-0.0797f, -0.6729f, -0.0376f, 0.7345f}, {0.0395f, -0.6261f, 0.0089f, 0.7787f}},
    {{-0.0770f, -0.2988f, -0.0107f, 0.9511f}, {-0.0770f, -0.2988f, -0.0107f, 0.9511f}, {-0.3306f, -0.4200f, -0.1512f, 0.8315f}, {-0.2646f, -0.0489f, 0.0070f, 0.9631f}, {0.2721f, -0.2688f, 0.0198f, 0.9237f}, {0.5129f, -0.0018f, 0.1959f, 0.8358f}, {0.5097f, 0.0834f, 0.2379f, 0.8226f}, {-0.2837f, -0.5544f, -0.1121f, 0.7743f}, {0.3102f, -0.8297f, -0.0961f, 0.4541f}, {0.4681f, -0.7024f, 0.1273f, 0.5208f}, {0.2993f, -0.2565f, 0.2200f, 0.8923f}, {-0.0998f, -0.1752f, -0.0731f, 0.9767f}, {0.0103f, -0.0887f, 0.0225f, 0.9957f}, {0.3294f, -0.4690f, -0.0422f, 0.8184f}, {-0.2056f, -0.5647f, 0.0120f, 0.7992f}, {-0.0444f, -0.5037f, 0.0099f, 0.8627f}},
    {{-0.0496f, -0.2835f, -0.0165f, 0.9576f}, {-0.0496f, -0.2835f, -0.0165f, 0.9576f}, {-0.3138f, -0.4107f, -0.1593f, 0.8411f}, {-0.2489f, -0.0480f, -0.0400f, 0.9665f}, {0.2328f, -0.3279f, -0.0541f, 0.9140f}, {0.4925f, -0.1085f, 0.1333f, 0.8532f}, {0.5005f, -0.0348f, 0.1774f, 0.8467f}, {-0.2914f, -0.4977f, -0.0731f, 0.8137f}, {0.3252f, -0.7889f, -0.0420f, 0.5196f}, {0.4669f, -0.6563f, 0.1278f, 0.5788f}, {0.2928f, -0.2727f, 0.1975f, 0.8949f}, {-0.1829f, -0.1691f, -0.1226f, 0.9607f}, {0.0241f, -0.0887f, -0.0026f, 0.9958f}, {0.2512f, -0.4332f, -0.0103f, 0.8655f}, {-0.1729f, -0.5183f, 0.0294f, 0.8370f}, {0.0203f, -0.4369f, 0.0077f, 0.8993f}},
    {{-0.0321f, -0.2113f, -0.0096f, 0.9768f}, {-0.0321f, -0.2113f, -0.0096f, 0.9768f}, {-0.3007f, -0.3601f, -0.1295f, 0.8736f}, {-0.2394f, -0.0070f, -0.0573f, 0.9692f}, {0.1599f, -0.3326f, -0.1124f, 0.9226f}, {0.4296f, -0.1576f, 0.0615f, 0.8870f}, {0.4454f, -0.0983f, 0.1013f, 0.8841f}, {-0.2969f, -0.3924f, -0.0188f, 0.8704f}, {0.3218f, -0.6953f, -0.0100f, 0.6426f}, {0.4512f, -0.5636f, 0.0931f, 0.6856f}, {0.2215f, -0.2171f, 0.1455f, 0.9395f}, {-0.2152f, -0.1317f, -0.0858f, 0.9638f}, {0.0363f, -0.0610f, -0.0205f, 0.9973f}, {0.2197f, -0.3513f, -0.0330f, 0.9095f}, {-0.0127f, -0.4038f, -0.0048f, 0.9147f}, {0.1680f, -0.3132f, -0.0068f, 0.9347f}},
    {{-0.0174f, -0.1172f, -0.0041f, 0.9929f}, {-0.0174f, -0.1172f, -0.0041f, 0.9929f}, {-0.2496f, -0.2729f, -0.0811f, 0.9256f}, {-0.2032f, -0.0437f, -0.0576f, 0.9765f}, {0.1005f, -0.2804f, -0.1390f, 0.9444f}, {0.3658f, -0.1414f, 0.0057f, 0.9199f}, {0.3842f, -0.0975f, 0.0383f, 0.9173f}, {-0.2578f, -0.2563f, 0.0325f, 0.9310f}, {0.3471f, -0.5506f, -0.0171f, 0.7590f}, {0.4539f, -0.4350f, 0.0293f, 0.7771f}, {0.1949f, -0.1518f, 0.0975f, 0.9641f}, {-0.2091f, -0.0736f, -0.0546f, 0.9736f}, {0.0197f, -0.0888f, -0.0165f, 0.9957f}, {0.3050f, -0.1963f, -0.0605f, 0.9299f}, {-0.1414f, -0.2968f, 0.0216f, 0.9442f}, {0.0023f, -0.2242f, 0.0255f, 0.9742f}},
    {{-0.0023f, -0.0351f, -0.0009f, 0.9994f}, {-0.0023f, -0.0351f, -0.0009f, 0.9994f}, {-0.1717f, -0.1017f, -0.0247f, 0.9796f}, {-0.1475f, -0.0127f, -0.0256f, 0.9886f}, {0.0474f, -0.1183f, -0.0901f, 0.9877f}, {0.1989f, -0.0449f, -0.0378f, 0.9783f}, {0.2172f, -0.0134f, -0.0184f, 0.9759f}, {-0.1843f, -0.0529f, 0.0774f, 0.9784f}, {0.3946f, -0.3373f, -0.0796f, 0.8510f}, {0.4697f, -0.2499f, -0.0730f, 0.8436f}, {0.1046f, -0.0861f, 0.0411f, 0.9899f}, {-0.1247f, -0.0410f, -0.0167f, 0.9912f}, {0.0103f, -0.0927f, -0.0088f, 0.9956f}, {0.1660f, -0.1086f, -0.0413f, 0.9793f}, {-0.0872f, -0.1644f, 0.0194f, 0.9823f}, {0.0031f, -0.1073f, 0.0129f, 0.9941f}},
    {{0.0147f, 0.0000f, -0.0000f, 0.9999f}, {0.0147f, 0.0000f, -0.0000f, 0.9999f}, {-0.0934f, 0.0000f, -0.0000f, 0.9956f}, {-0.0858f, 0.0034f, -0.0032f, 0.9963f}, {0.0299f, -0.0104f, -0.0206f, 0.9993f}, {0.0551f, 0.0024f, -0.0143f, 0.9984f}, {0.0673f, 0.0225f, -0.0039f, 0.9975f}, {-0.1217f, 0.0592f, 0.0771f, 0.9878f}, {0.4524f, -0.2172f, -0.1384f, 0.8538f}, {0.4951f, -0.1626f, -0.1429f, 0.8414f}, {0.0380f, -0.0670f, 0.0137f, 0.9969f}, {-0.0716f, -0.0451f, -0.0067f, 0.9964f}, {-0.0170f, -0.0976f, -0.0072f, 0.9951f}, {0.0611f, -0.0605f, -0.0192f, 0.9961f}, {-0.0429f, -0.0827f, 0.0064f, 0.9956f}, {0.0026f, -0.0453f, -0.0080f, 0.9989f}},
};
constexpr float kFlipSideLandMove[16][3] = {{-51.0f, 979.0f, 761.0f}, {-13.0f, 536.0f, 782.0f}, {20.0f, -52.0f, 1042.0f}, {40.0f, -67.0f, 989.0f}, {48.0f, 452.0f, 677.0f}, {40.0f, 880.0f, 398.0f}, {6.0f, 980.0f, 421.0f}, {-62.0f, 986.0f, 554.0f}, {-197.0f, 915.0f, 628.0f}, {-358.0f, 802.0f, 673.0f}, {-507.0f, 722.0f, 598.0f}, {-392.0f, 648.0f, 454.0f}, {-243.0f, 572.0f, 178.0f}, {-136.0f, 459.0f, 75.0f}, {-46.0f, 270.0f, 14.0f}, {2.0f, 122.0f, -13.0f}};

// The flip's tables: its frames one a frame, then its landing's.
struct FlipTable {
    const float (*turn)[16][4];
    const float (*move)[3];
    int frames;
};
const FlipTable kFlipAir[2] = {{kFlipFrontTurn, kFlipFrontMove, 13}, {kFlipSideTurn, kFlipSideMove, 14}};
const FlipTable kFlipLand[2] = {{kFlipFrontLandTurn, kFlipFrontLandMove, 13}, {kFlipSideLandTurn, kFlipSideLandMove, 16}};
constexpr float kMmToWw = 0.01f * 45.f / 23.76f;  // MM's human Link (hips 23.76 high standing) against Wind Waker's (45)
constexpr int kFlipBodyBones = 4;               // the table's first four: center, waist_jnt, body_chn, neck_jnt
constexpr int kFlipFrontUpright = 10;           // the front flip's frame that is round and upright
constexpr int kFlipFade = 4;                     // frames to let go of a flip (another action cut it short, or it ends in the air)

struct FlipState {
    u32 player = 0, lastProc = 0;
    int kind = 0;       // 0 none, 1 the front flip, 2 the side somersault
    int phase = 0;      // 0 in the air, 1 the landing, 2 letting go
    int t = 0, fade = 0;
    int heldPhase = 0, heldT = 0;  // what is let go of
    u32 seed = 0x1234567;
    int jumps = 0;
    Q pose[42], posed[42];  // the joint pass: each joint's turn in the model, as the animation has it and as flipped
    // What the flip changed, given back after each joint's matrix (hook 100): the animation keeps its own values, from
    // which Wind Waker blends into its next animation (the fall at the jump's top would blend from a flipped body).
    Q orig[42];
    float origMove[3];
    u64 changed = 0;
} s_flip;

bool flipGroundProc(u32 proc) { return proc == kProcLand || (proc >= 4 && proc <= 7); }  // landing, wait, move, targeted move

u32 flipRandom() { return s_flip.seed = s_flip.seed * 1103515245u + 12345u, s_flip.seed >> 16; }

// Once a frame (the joint pass's first joint): a running jump starting picks its flip, the flip moves on.
void flipFrame(Ctx& c, u32 p) {
    const u32 proc = rd32(p + kPCurProc);
    if (p != s_flip.player) s_flip = FlipState{}, s_flip.player = p, s_flip.seed ^= u32(retraceCount());
    if (proc == kProcAutoJump && s_flip.lastProc != kProcAutoJump) {
        // soh_fd's running jump: facing the way he goes and faster than 4 (of OoT's 6)
        const bool running = flt32(rd32(p + kPSpeedF)) > flt32(rd32(p + 0x2A8)) * 4.f / 6.f &&
                             std::abs(s32(s16(rd16(p + kPCurrentAngle) - rd16(p + kPShapeAngle)))) < 0x1000;
        static const char* const force = std::getenv("BLUEWAKE_MM_FLIP");  // testing: 1 the front flip, 2 the side somersault
        s_flip.kind = !running ? 0 : force ? std::clamp(std::atoi(force), 0, 2) : int(flipRandom() % 3);
        s_flip.phase = 0, s_flip.t = 0, s_flip.jumps++;
        if (ootLog()) log("[mm] jump %d: %s\n", s_flip.jumps, s_flip.kind == 1 ? "front flip" : s_flip.kind == 2 ? "side somersault" : "the regular jump");
    } else if (s_flip.kind != 0) {
        // The flip and its landing play one after the other, the landing on the ground (a short hop rolls out of it there,
        // as MM's) or, for the side somersault, in the air too; any other action (a jump attack, a ledge grab) lets go of it.
        const bool goesOn = proc == kProcAutoJump || proc == kProcFall || flipGroundProc(proc);
        if (s_flip.phase != 2 && !goesOn) {
            s_flip.heldPhase = s_flip.phase, s_flip.heldT = s_flip.t, s_flip.phase = 2, s_flip.fade = 0;
        } else if (s_flip.phase == 0) {
            ++s_flip.t;
            // The roll's whoosh (BCK_ROLLF's own sound, JA_SE_LK_SW_KAZEKIRI_L), as MM's front flip has its roll sound
            if (s_flip.t == (s_flip.kind == 1 ? 4 : 2)) guestCall(c, 0x80103074, {p, 0x2801});  // seStartOnlyReverb
            if (s_flip.kind == 1 && s_flip.t >= kFlipFrontUpright && !flipGroundProc(proc)) {
                // The front flip is round and upright by frame 10 (its last frames get the legs ready for the ground, which
                // over Wind Waker's fall pitch him forward): still in the air, Link eases back into Wind Waker's fall.
                s_flip.heldPhase = 0, s_flip.heldT = s_flip.t, s_flip.phase = 2, s_flip.fade = 0;
            } else if (s_flip.t >= kFlipAir[s_flip.kind - 1].frames) {
                s_flip.phase = 1, s_flip.t = 0;  // its landing (the side somersault's ends its turn, in the air too)
            }
        } else if (s_flip.phase == 1) {
            if (++s_flip.t >= kFlipLand[s_flip.kind - 1].frames) s_flip.kind = 0;
        } else if (++s_flip.fade >= kFlipFade) {
            s_flip.kind = 0;
        }
    }
    s_flip.lastProc = proc;
}

// The flip's turn of table bone b and its root move now, and how much of it (1, less while letting go).
float flipNow(int b, Q* turn, float move[3]) {
    const int phase = s_flip.phase == 2 ? s_flip.heldPhase : s_flip.phase;
    const int t = s_flip.phase == 2 ? s_flip.heldT : s_flip.t;
    const FlipTable& table = (phase == 0 ? kFlipAir : kFlipLand)[s_flip.kind - 1];
    const int f = std::clamp(t, 0, table.frames - 1);
    const float* q = table.turn[f][b];
    *turn = Q{q[0], q[1], q[2], q[3]};
    for (int i = 0; i < 3; i++) move[i] = table.move[f][i] * kMmToWw;
    return s_flip.phase == 2 ? 1.f - float(s_flip.fade + 1) / float(kFlipFade + 1) : 1.f;
}

// 98: jointBeforeCB's second instruction (r3 the player, r4 the joint, r5 its J3DTransformInfo, r6 its Quaternion), as
// hook 89 for the crouch stab: a table bone takes the flip's world turn on top of the jump's, the others follow.
u32 hookFlipBone(Ctx& c) {
    const u32 player = c.r[3], joint = c.r[4], info = c.r[5], quat = c.r[6];
    if (player != rd32(kPlayerPtr) || joint >= 42) return 0;
    if (joint == 0) {
        const KeepArgs keep(c);
        flipFrame(c, player);
    }
    if (s_flip.kind == 0) return 0;
    const Q local{flt32(rd32(quat)), flt32(rd32(quat + 4)), flt32(rd32(quat + 8)), flt32(rd32(quat + 12))};
    const int parent = kJointParent[joint];
    const Q poseParent = parent < 0 ? Q{0.f, 0.f, 0.f, 1.f} : s_flip.pose[parent];
    const Q posedParent = parent < 0 ? Q{0.f, 0.f, 0.f, 1.f} : s_flip.posed[parent];
    s_flip.pose[joint] = qmul(poseParent, local);
    // The flip turns the body only (center and waist, torso, neck): the limbs keep Wind Waker's own jump pose. MM's limb
    // turns are measured against MM's own jumping arms and legs, which are not Wind Waker's, and twisted them.
    int bone = -1;
    for (int b = 0; b < kFlipBodyBones; b++)
        if (kStabJoint[b] == int(joint)) bone = b;
    if (bone < 0) {
        s_flip.posed[joint] = qmul(posedParent, local);
        return 0;
    }
    Q turn;
    float move[3];
    const float amount = flipNow(bone, &turn, move);
    if (amount < 1.f) turn = qslerp(Q{0.f, 0.f, 0.f, 1.f}, turn, amount);
    s_flip.posed[joint] = qmul(turn, s_flip.pose[joint]);
    const Q turned = qmul(qconj(posedParent), s_flip.posed[joint]);
    s_flip.orig[joint] = local, s_flip.changed |= 1ull << joint;
    wrfloat(quat, turned.x), wrfloat(quat + 4, turned.y), wrfloat(quat + 8, turned.z), wrfloat(quat + 12, turned.w);
    if (joint == 1 && ootLog())
        log("[mm] flip %d phase %d t %d fade %d: waist turn %.0f deg, proc %X\n", s_flip.kind, s_flip.phase, s_flip.t, s_flip.fade,
            2.f * std::acos(std::min(1.f, std::fabs(turn.w))) * 57.29578f, rd32(player + kPCurProc));
    if (joint == 1) {  // center: MM's root move, in link_root's space
        float inRoot[3];
        for (int i = 0; i < 3; i++) move[i] *= amount;
        qrotate(qconj(s_flip.pose[0]), move, inRoot);
        for (int i = 0; i < 3; i++) {
            s_flip.origMove[i] = flt32(rd32(info + 0x14 + 4 * i));
            wrfloat(info + 0x14 + 4 * i, s_flip.origMove[i] + inRoot[i]);
        }
    }
    return 0;
}

// 100: jointAfterCB's entry (r3 the player, r4 the joint, r5 its J3DTransformInfo, r6 its Quaternion): a joint the flip
// turned gets its own values back, as the game does for its own turns (m34C6 / m3658: when the game will put back a value
// of its own, it is given the animation's).
u32 hookFlipBoneAfter(Ctx& c) {
    const u32 player = c.r[3], joint = c.r[4], info = c.r[5], quat = c.r[6];
    if (player != s_flip.player || joint >= 42 || !(s_flip.changed & (1ull << joint))) return 0;
    s_flip.changed &= ~(1ull << joint);
    const Q& q = s_flip.orig[joint];
    const u32 to = (rd8(player + 0x34C6) & 1) ? player + 0x3658 : quat;
    wrfloat(to, q.x), wrfloat(to + 4, q.y), wrfloat(to + 8, q.z), wrfloat(to + 12, q.w);
    if (joint == 1)
        for (int i = 0; i < 3; i++) wrfloat(info + 0x14 + 4 * i, s_flip.origMove[i]);
    return 0;
}

// 99: checkJumpCutFromButton's entry (r3 the player): no jump attack during an MM flip (not when they are cosmetic).
u32 hookFlipNoJumpCut(Ctx& c) {
    if (s_flip.kind == 0 || s_flip.phase != 0 || c.r[3] != s_flip.player || knob(kKnobFlipsCosmetic) != 0.f) return 0;
    c.r[3] = 0;
    return kInterpReturn;
}

// 88: setAtParam at its entry (r3 the player, r4 type, r5 atp, r6 spl, r7 se, r8 hit mark, r9 cut type, f1 radius):
// the last sword attack, for a power stab.
u32 hookStabAtParam(Ctx& c) {
    if (ootLog() && c.r[3] == rd32(kPlayerPtr) && !s_stab.ownAtCall) log("[oot] setAtParam type %u atp %u cut 0x%02X\n", c.r[4], c.r[5], c.r[9]);
    if (s_stab.ownAtCall || c.r[3] != rd32(kPlayerPtr) || c.r[4] != 2 /* AT_TYPE_SWORD */) return 0;
    s_stab.recPid = rd32(c.r[3] + 4);
    s_stab.recAtp = c.r[5], s_stab.recSpl = c.r[6], s_stab.recSe = c.r[7], s_stab.recHit = c.r[8], s_stab.recCut = c.r[9];
    s_stab.recRadius = float(c.f[1]);
    s_stab.recValid = true;
    return 0;
}

// ---- Keep Held Weapons (104-106) ----
// A melee weapon in Link's hand is a d_a_boko actor (stick, machete, mace, Darknut sword, spear, Phantom Ganon's sword):
// daPy_lk_c::execute drops it for a door (104 skips that), and a scene change deletes it with the stage (105 notes what he
// held as the player is deleted, 106 makes it again in the new stage and puts it in his hand as a pick-up ends).
constexpr u32 kBokoProc = 0x1D5, kEquipNone = 0x100, kEquipBoko = 0x101, kBokoPgSword = 5, kBokoLitParam = 7;
constexpr u32 kPKeepEquip = 0x317C;  // daPy_actorKeep_c mActorKeepEquip: the actor's id, then its pointer
constexpr u32 kFnCreateActor = 0x8002451C, kFnDeleteActorById = 0x800244B8, kFnSearchById = 0x800241C0;
constexpr u32 kFnSetCarryNow = 0x80027E28, kFnKeepSetData = 0x8010848C, kFnSetWeaponBlur = 0x8013A8B4, kFnCheckNextMode = 0x8010E828;
constexpr u64 kKeepTimeout = 1800;  // retraces after the scene change the weapon waits for a calm moment (about 30 s)
struct KeepWeapon {
    u32 player = 0;  // the player seen holding a weapon last frame (the actors are gone when playerDelete runs)
    u32 type = 0;
    s32 flame = 0;
    bool held = false;
    bool dead = false;     // his hearts were at 0 since this player began
    bool pending = false;  // a weapon to make in the new stage
    u32 pType = 0;
    s32 pFlame = 0;
    u64 since = 0;
    u32 pid = 0;      // the made actor's id (0: not made yet)
    u32 scratch = 0;  // 16 bytes of guest memory for fopAcM_SearchByID's out pointer
} s_keep;

// 104: daPy_lk_c::execute, the door's drop of the weapon (the equip test at 0x80121B10): past it, he walks through with it.
u32 hookKeepDoor(Ctx& c) {
    if (c.r[31] != rd32(kPlayerPtr)) return 0;
    return 0x80121B54;
}

// 122: deleteEquipItem's entry (r3 the player, r4 whether it is put away with a sound): while a door's event runs (eventInfo's
// command is the door's, checkCommandDoor) the weapon stays in his hand. The door's own drop in execute is 104's; a door whose
// event plays Link's demo (the Forsaken Fortress's) drops it through setDemoData and changeDemoProc instead (user 2026-10-25).
u32 hookKeepDoorDelete(Ctx& c) {
    const u32 p = c.r[3];
    if (p != rd32(kPlayerPtr) || rd16(p + kPEquipItem) != kEquipBoko || rd16(p + 0xF8) != 3) return 0;
    if (const u32 actor = rd32(p + kPKeepEquip + 4); actor == 0 || rd16(actor + 8) != kBokoProc) return 0;
    static u32 s_lastCaller;
    if (c.lr != s_lastCaller) s_lastCaller = c.lr, log("[keep] a door's event: his weapon stays in hand (deleteEquipItem from %08X)\n", c.lr);
    return kInterpReturn;
}

// 105: playerDelete's entry (r3 the player): a scene change, with Link alive, takes what he held to the next stage.
u32 hookKeepDelete(Ctx& c) {
    const u32 p = c.r[3];
    const bool take = s_keep.held && s_keep.player == p && rd16(p + kPEquipItem) == kEquipBoko && s_keep.type != kBokoPgSword &&
                      !s_keep.dead && rd16(kGameInfo + 2) != 0;  // (not a game over)
    s_keep.pending = take, s_keep.pid = 0, s_keep.held = false, s_keep.player = 0, s_keep.dead = false;
    if (take) {
        s_keep.pType = s_keep.type, s_keep.pFlame = s_keep.flame, s_keep.since = retraceCount();
        log("[keep] the scene changes: his weapon (type %u, flame %d) goes along\n", s_keep.type, s_keep.flame);
    }
    return 0;
}

// 106: daPy_lk_c::setActorPointer's entry (r3 the player), each frame: notes the weapon he holds; with one waiting for the new
// stage, makes it once he is free (no event, standing or walking), then, when it exists, puts it in his hand as a pick-up does.
u32 hookKeepTick(Ctx& c) {
    const KeepArgs keep(c);
    const u32 p = c.r[3];
    if (p != rd32(kPlayerPtr)) return 0;
    const bool holding = rd16(p + kPEquipItem) == kEquipBoko;
    const u32 actor = rd32(p + kPKeepEquip + 4);
    if (rd16(kGameInfo + 2) == 0) s_keep.dead = true;  // a game over's continue refills the hearts before the scene changes
    if (holding && actor != 0 && rd32(actor + 4) == rd32(p + kPKeepEquip) && rd16(actor + 8) == kBokoProc) {
        s_keep.held = !s_keep.dead, s_keep.player = p;
        s_keep.type = rd32(actor + 0xB0), s_keep.flame = s16At(actor + 0x2C4);
    } else {
        s_keep.held = false;
    }
    if (!s_keep.pending) return 0;
    const u64 now = retraceCount();
    if (now - s_keep.since > kKeepTimeout || (s_keep.pid != 0 && rd16(p + kPEquipItem) != kEquipNone)) {
        // too long, or he took something else in hand while it was being made: given up
        if (s_keep.pid != 0) guestCall(c, kFnDeleteActorById, {s_keep.pid});
        log("[keep] the weapon is given up\n");
        s_keep.pending = false, s_keep.pid = 0;
        return 0;
    }
    if (!ootGround(p)) return 0;
    const u32 proc = rd32(p + kPCurProc);
    if (s_keep.pid == 0) {
        if ((proc != 4 && proc != 5 && proc != 6) || rd16(p + kPEquipItem) != kEquipNone) return 0;  // WAIT, FREE_WAIT, MOVE
        const u32 params = (s_keep.pType == 0 && s_keep.pFlame > 0) ? kBokoLitParam : s_keep.pType;
        const u32 pid = guestCall(c, kFnCreateActor, {kBokoProc, params, p + 0x1F8, u32(s32(s8(rd8(p + 0x20A)))), 0u, 0u, 0xFFFFFFFFu, 0u});
        if (pid == 0 || pid == 0xFFFFFFFFu) {
            log("[keep] the weapon could not be made\n");
            s_keep.pending = false;
            return 0;
        }
        s_keep.pid = pid;
        return 0;
    }
    if (proc != 4 && proc != 5 && proc != 6) return 0;
    if (s_keep.scratch == 0) s_keep.scratch = hleAlloc(16, 16);
    wr32(s_keep.scratch, 0);
    if (guestCall(c, kFnSearchById, {s_keep.pid, s_keep.scratch}) == 0) return 0;
    const u32 boko = rd32(s_keep.scratch);
    if (boko == 0 || rd8(boko + 0xC) != 2) return 0;  // (still being made)
    guestCall(c, kFnSetCarryNow, {boko, 1});
    wr8(boko + 0x2B8, 3);  // Mode_PLAYER_CARRY_e
    if (s_keep.pType == 0 && s_keep.pFlame > 0) wr16(boko + 0x2C4, u16(s_keep.pFlame));
    wr16(p + kPEquipItem, kEquipBoko);
    guestCall(c, kFnKeepSetData, {p + kPKeepEquip, boko});
    guestCall(c, kFnSetWeaponBlur, {p});
    guestCall(c, kFnCheckNextMode, {p, 0});  // (the pick-up's own end: the weapon-holding stance)
    log("[keep] type %u back in his hand (flame %d)\n", s_keep.pType, s_keep.pFlame);
    s_keep.pending = false, s_keep.pid = 0;
    return 0;
}

// Native code at a hook site: 0 goes on with the instruction there, else where to go.
u32 runHook(Ctx& c, int id, u32 pc) {
    static u64 s_seen = 0;  // each hook's first run is logged: a setting's code ran
    if (id < 64 && !(s_seen & (1ull << id))) s_seen |= 1ull << id, log("[options] hook %d first ran at %08X\n", id, pc);
    switch (id) {
    case 1: return hookSwingTurn(c);
    case 2:  // after the C-stick's horizontal axis is loaded for the camera
        c.f[1] = -c.f[1];
        return 0;
    case 3: return hookSailWind(c);
    case 4:  // cLib_addCalc's steps when A stops the boat
        setSingle(c, 3, on("brisk_sail") ? 3.f : 2.f);
        setSingle(c, 4, 0.2f);
        return 0;
    case 5:  // ... and when Link is knocked off a moving boat
        setSingle(c, 3, on("brisk_sail") ? 4.f : 2.f);
        setSingle(c, 4, on("brisk_sail") ? 0.2f : 0.1f);
        return 0;
    case 12: return treasureGlowHook(c, pc);
    case 14: return hookIntroSkip(c);
    case 13: return playerModelHook(c);
    case 36: return hookSearchById(c);
    case 40: return hookJudgeById(c);
    // runtime_fixes: DVDCheckDisk, OSResetSystem and SelectThread's idle loop are this runtime's own (hle_os, hle_dvd)
    case 39:
    case 41:
    case 42: return 0;
    // the host's features (bod_host: online puppets and PvP, the Bunny Hood, Reveal Sea Chart, the autosave, Link's
    // voice pitch, switch sync)
    case 30:
    case 31:
    case 34:
    case 37:
    case 38:
    case 51:
    case 70:
    case 71:
    case 72:
    case 73:
    case 74: return hostHook(c, unsigned(id), pc);
    case 50: return randoUiHook(c);
    case 81: return hookOotTurn(c);
    case 82: return hookBDraw(c);
    case 90: return hookOotMove(c);
    case 83: return hookStabProc(c);
    case 85: return hookStabProcInit(c);
    case 88: return hookStabAtParam(c);
    case 89: return hookStabBone(c);
    case 91:
    case 92: return hookStabTrail(c, id);
    case 93: return hookMmLedgeJump(c);
    case 94: return hookMmRoll(c);
    case 95: return hookMmJumpSpeed(c);
    case 96: return hookMmJumpAir(c);
    case 97: return hookMmFallAir(c);
    case 98: return hookFlipBone(c);
    case 99: return hookFlipNoJumpCut(c);
    case 100: return hookFlipBoneAfter(c);
    case 120: return hookStabDraw(c);
    case 121: return hookStabTalkReach(c);
    case 122: return hookKeepDoorDelete(c);
    case 102: return hookIsgTick(c);
    case 103: return hookIsgKeepSword(c);
    case 104: return hookKeepDoor(c);
    case 105: return hookKeepDelete(c);
    case 106: return hookKeepTick(c);
    default:
        if (id >= 18 && id <= 26) return hookFix(c, id, pc);
        return 0;
    }
}

// ---- applying ----
std::vector<u32> s_hookWords;  // per kHooks entry: its interpreter hook word
u32 hookWord(size_t i) {
    if (s_hookWords.empty()) {
        for (size_t k = 0; k < sizeof kHooks / sizeof kHooks[0]; k++) {
            const HookDef d = kHooks[k];
            s_hookWords.push_back(interpHookWord([d](Ctx& c, u32 pc) -> u32 {
                if (d.after) {  // the instruction, then the native code
                    const u32 next = interpStep(c, pc, rd32(pc));
                    const u32 r = runHook(c, d.id, pc);
                    return r ? r : next;
                }
                const u32 r = runHook(c, d.id, pc);
                return r ? r : kInterpOriginal;
            }));
        }
    }
    return s_hookWords[i];
}

struct Journal {
    u32 addr, size;
    u8 before[4];
};
std::unordered_map<size_t, Journal> s_written;  // kWrites index -> what it replaced (while applied)

void put(u32 a, u32 size, const u8* b) {
    if (size == 4 && !(a & 3)) wr32(a, u32(b[0]) << 24 | u32(b[1]) << 16 | u32(b[2]) << 8 | b[3]);
    else if (size == 2 && !(a & 1)) wr16(a, u32(b[0]) << 8 | b[1]);
    else
        for (u32 i = 0; i < size; i++) wr8(a + i, b[i]);
}

// One option's pieces in one module (module ~0u: every module) on or off.
void apply(size_t opt, bool enable, u32 module) {
    for (const SiteDef& s : kSites) {
        if (size_t(s.option) != opt || (module != ~0u && s.at.module != module)) continue;
        const u32 a = resolve(s.at);
        if (!a) continue;
        if (enable) {
            interpPatch(a, s.imm16 ? ((rd32(a) & 0xFFFF0000u) | (s.value & 0xFFFF)) : s.value);
            interpOverrideAt(a);
        } else {
            interpUnpatch(a);
        }
    }
    for (size_t k = 0; k < sizeof kHooks / sizeof kHooks[0]; k++) {
        const HookDef& h = kHooks[k];
        if (size_t(h.option) != opt || (module != ~0u && h.at.module != module)) continue;
        const u32 a = resolve(h.at);
        if (!a) continue;
        if (enable) {
            interpPatch(a, hookWord(k));
            interpOverrideAt(a);
        } else {
            interpUnpatch(a);
        }
    }
    for (size_t k = 0; k < sizeof kWrites / sizeof kWrites[0]; k++) {
        const WriteDef& w = kWrites[k];
        if (size_t(w.option) != opt || (module != ~0u && w.at.module != module)) continue;
        const u32 a = resolve(w.at);
        if (!a) continue;
        const auto it = s_written.find(k);
        if (enable && it == s_written.end()) {
            Journal j{a, w.size, {}};
            for (u32 i = 0; i < w.size; i++) j.before[i] = u8(rd8(a + i));
            const u8 now[4] = {u8(w.bits >> ((w.size - 1) * 8)), u8(w.bits >> ((w.size - 2) * 8)), u8(w.bits >> 8), u8(w.bits)};
            u8 bytes[4];
            for (u32 i = 0; i < w.size; i++) bytes[i] = u8(w.bits >> (8 * (w.size - 1 - i)));
            (void)now;
            put(a, w.size, bytes);
            s_written[k] = j;
        } else if (!enable && it != s_written.end()) {
            put(it->second.addr, it->second.size, it->second.before);
            s_written.erase(it);
        }
    }
}

// ---- instant text and the sail's name: the loaded message files (MESGbmg1), patched in memory ----
struct BmgChange {
    u32 offset;
    u8 before, after;
};
struct BmgCopy {
    u32 address = 0;
    std::vector<BmgChange> changes;
};
std::vector<BmgCopy> s_textCopies, s_sailCopies;
bool s_textApplied = false;
int s_sailMode = 0;  // 0 vanilla, 1 Swift, 2 Brisk: what s_sailCopies hold
u64 s_frame = 0;

void bmgPut(BmgCopy& copy, u32 offset, u8 value) {
    const u32 a = copy.address + offset;
    copy.changes.push_back({offset, u8(rd8(a)), value});
    wr8(a, value);
}
bool bmgIntact(const BmgCopy& copy) {
    if (rd32(copy.address) != 0x4D455347u || rd32(copy.address + 4) != 0x626D6731u) return false;
    return copy.changes.empty() || (rd8(copy.address + copy.changes.front().offset) == copy.changes.front().after &&
                                    rd8(copy.address + copy.changes.back().offset) == copy.changes.back().after);
}
void bmgUnpatch(const BmgCopy& copy) {
    if (!bmgIntact(copy)) return;
    for (const BmgChange& ch : copy.changes)
        if (rd8(copy.address + ch.offset) != ch.after) return;
    for (size_t i = copy.changes.size(); i-- > 0;) wr8(copy.address + copy.changes[i].offset, copy.changes[i].before);
}
template <class F> void forEachBmg(F f) {  // every loaded message file in main memory
    const u8* ram = hostp(0x80000000u);
    for (u32 off = 0; off + 8 <= kMem1Size;) {
        const void* hit = std::memchr(ram + off, 'M', kMem1Size - off - 7);
        if (!hit) break;
        off = u32(static_cast<const u8*>(hit) - ram);
        if (!std::memcmp(ram + off, "MESGbmg1", 8)) f(0x80000000u + off);
        off++;
    }
}

// Every message draws its whole box at once (draw type 1) and its timed waits wait no time
// (Boat of Daphnes' bmg_patch.h): the whole table is checked before a byte is changed.
bool instantPatch(u32 bmg, BmgCopy& copy) {
    const u8* d = hostp(bmg);
    const size_t avail = 0x80000000u + kMem1Size - bmg;
    auto u32at = [&](size_t o) { return u32(d[o]) << 24 | u32(d[o + 1]) << 16 | u32(d[o + 2]) << 8 | d[o + 3]; };
    auto u16at = [&](size_t o) { return u16(d[o] << 8 | d[o + 1]); };
    if (avail < 32) return false;
    const u32 blocks = u32at(8), sections = u32at(12);
    if (blocks < 2 || blocks > 0x00400000u / 32 || sections == 0 || sections > 16 || d[16] == 2) return false;
    const size_t size = size_t(blocks) * 32;
    if (size > avail) return false;
    size_t inf = 0, infSize = 0, dat = 0, datSize = 0, at = 32;
    for (u32 s = 0; s < sections; s++) {
        if (at > size || size - at < 8) return false;
        const u32 magic = u32at(at);
        const size_t ss = u32at(at + 4);
        if (ss < 8 || ss > size - at) return false;
        if (magic == 0x494E4631u) {
            if (inf || ss < 16) return false;
            inf = at, infSize = ss;
        } else if (magic == 0x44415431u) {
            if (dat) return false;
            dat = at, datSize = ss;
        }
        at += ss;
    }
    if (!inf || !dat) return false;
    const u32 messages = u16at(inf + 8);
    const size_t entry = u16at(inf + 10);
    if (entry < 14 || messages > (infSize - 16) / entry) return false;
    const size_t textStart = dat + 8, textEnd = dat + datSize;
    copy.address = bmg;
    for (int pass = 0; pass < 2; pass++) {
        for (u32 m = 0; m < messages; m++) {
            const size_t e = inf + 16 + size_t(m) * entry;
            const size_t offset = u32at(e);
            if (offset >= datSize - 8) return false;
            if (pass) bmgPut(copy, u32(e + 13), 1);
            // A message with a sound (tag group 1) or an animation (group 3, which brings a voice) keeps its waits: the
            // tag plays when the text reaches it, so without them every sound of the box played as it opened, early in
            // a cutscene (user 2026-10-25). Its text still draws at once between them.
            bool timed = false;
            for (size_t q = textStart + offset; q + 2 < textEnd && d[q] != 0;) {
                if (d[q] != 0x1A) { q++; continue; }
                if (d[q + 1] < 2) break;
                if (d[q + 2] == 1 || d[q + 2] == 3) { timed = true; break; }
                q += d[q + 1];
            }
            size_t p = textStart + offset;
            while (p < textEnd && d[p] != 0) {
                if (d[p] != 0x1A) {
                    p++;
                    continue;
                }
                if (textEnd - p < 2) return false;
                const size_t length = d[p + 1];
                if (length < 2 || length > textEnd - p) return false;
                if (pass && !timed && length == 7 && d[p + 2] == 0 && d[p + 3] == 0) {
                    const u8 command = d[p + 4];
                    if ((command == 7 || command == 3) && (command != 7 || d[p + 5] != 0 || d[p + 6] != 0)) {
                        bmgPut(copy, u32(p + 4), 7);
                        bmgPut(copy, u32(p + 5), 0);
                        bmgPut(copy, u32(p + 6), 0);
                    }
                }
                p += length;
            }
            if (p == textEnd) return false;
        }
    }
    return true;
}

void instantTextFrame() {
    const bool want = on("instant_text");
    if (!want) {
        if (s_textApplied || !s_textCopies.empty()) {
            for (size_t i = s_textCopies.size(); i-- > 0;) bmgUnpatch(s_textCopies[i]);
            s_textCopies.clear();
            log("[options] instant text off: message files put back\n");
        }
        s_textApplied = false;
        return;
    }
    const size_t before = s_textCopies.size();
    std::erase_if(s_textCopies, [](const BmgCopy& c) { return !bmgIntact(c); });
    const bool first = !s_textApplied;
    s_textApplied = true;
    // Again once a second, and at once when a patched file went (mounted again: the randomizer's switch remounts
    // bmgres.arc, and scenes load their own): any message file not yet patched is. (It used to look again only when
    // none was patched, so a second file kept the reloaded one unpatched.)
    // (and once a second for a few seconds after an archive opened: ArchiveWatch, not all memory every second)
    static ArchiveWatch s_watch;
    if (!first && s_textCopies.size() == before && !(s_frame % 60 == 0 && s_watch.due())) return;
    forEachBmg([](u32 a) {
        for (const BmgCopy& c : s_textCopies)
            if (c.address == a) return;
        BmgCopy copy;
        if (s_textCopies.size() < 8 && instantPatch(a, copy)) {
            log("[options] instant text: message file at %08X patched (%zu bytes)\n", a, copy.changes.size());
            s_textCopies.push_back(std::move(copy));
        } else {
            bmgUnpatch(copy);  // (a table found bad part way through: nothing it wrote stays)
        }
    });
}

// The Sail's name: "Swift Sail" / "Brisk Sail" (message 0x1CF, whose offset is pointed at the
// file header's 15 unused bytes holding the new name), and "boat's sail" in the item-get text (221).
bool sailPatch(u32 bmg, int mode, BmgCopy& copy) {
    if (bmg + 64 >= 0x80000000u + kMem1Size) return false;
    const u32 blocks = rd32(bmg + 8), sections = rd32(bmg + 12);
    if (blocks < 2 || blocks > 0x00400000u / 32 || !sections || sections > 16 || bmg + blocks * 32 > 0x80000000u + kMem1Size) return false;
    u32 inf = 0, dat = 0, at = bmg + 32;
    for (u32 i = 0; i < sections; i++) {
        const u32 magic = rd32(at), size = rd32(at + 4);
        if (size < 8 || at + size > bmg + blocks * 32) return false;
        if (magic == 0x494E4631u) inf = at;
        else if (magic == 0x44415431u) dat = at;
        at += size;
    }
    if (!inf || !dat) return false;
    const u32 entries = rd16(inf + 8), entrySize = rd16(inf + 10);
    if (entrySize < 14 || !entries) return false;
    auto entryOf = [&](u16 message) -> u32 {
        for (u32 i = 0; i < entries; i++)
            if (rd16(inf + 16 + i * entrySize + 4) == message) return inf + 16 + i * entrySize;
        return 0;
    };
    const u32 nameEntry = entryOf(0x1CF);
    if (!nameEntry) return false;
    const u32 nameText = dat + 8 + rd32(nameEntry);
    if (rd32(nameText) != 0x5361696Cu || rd8(nameText + 4) != 0) return false;  // "Sail": the English names
    for (u32 i = 0; i < 15; i++)
        if (rd8(bmg + 0x11 + i)) return false;
    copy.address = bmg;
    const char* name = mode == 2 ? "Brisk Sail" : "Swift Sail";
    for (u32 i = 0; i < 11; i++) bmgPut(copy, 0x11 + i, u8(name[i]));
    const u32 offset = (bmg + 0x11) - (dat + 8);  // wraps: the pad lies before the text
    for (u32 i = 0; i < 4; i++) bmgPut(copy, nameEntry - bmg + i, u8(offset >> (24 - 8 * i)));
    if (const u32 get = entryOf(221)) {
        const u32 text = dat + 8 + rd32(get);
        for (u32 p = text; p < text + 400 && rd8(p); p++) {
            static const char vanilla[] = "boat's sail";
            u32 k = 0;
            while (k < 11 && rd8(p + k) == u8(vanilla[k])) k++;
            if (k == 11) {
                for (u32 j = 0; j < 10; j++) bmgPut(copy, p - bmg + j, u8(name[j]));
                bmgPut(copy, p - bmg + 10, ' ');
                break;
            }
        }
    }
    return true;
}

void sailNameFrame() {
    const int want = on("brisk_sail") ? 2 : on("swift_sail") ? 1 : 0;
    const bool changed = want != s_sailMode;
    if (!changed && (want == 0 || s_frame % 60 != 0)) return;
    if (changed) {
        for (size_t i = s_sailCopies.size(); i-- > 0;) bmgUnpatch(s_sailCopies[i]);
        s_sailCopies.clear();
        s_sailMode = want;
        if (!want) return;
    }
    const size_t had = s_sailCopies.size();
    std::erase_if(s_sailCopies, [](const BmgCopy& c) { return !bmgIntact(c); });
    if (!s_sailCopies.empty()) return;
    static ArchiveWatch s_watch;  // (the file not found yet: looked for again only after an archive opened)
    if (!changed && had == 0 && !s_watch.due()) return;
    forEachBmg([want](u32 a) {
        BmgCopy copy;
        if (s_sailCopies.size() < 8 && sailPatch(a, want, copy)) {
            log("[options] sail name: \"%s\" in the message file at %08X\n", want == 2 ? "Brisk Sail" : "Swift Sail", a);
            s_sailCopies.push_back(std::move(copy));
        }
    });
}
}  // namespace

std::string optionCvar(const char* name) { return std::string("gEnhancements.BetterWW.") + name; }
bool optionSupported(const char* name) { return supported(name); }
bool optionOn(const char* name) { return on(name); }
bool optionSet(const char* name, bool enable) {  // the menu's switch (bluewake_game_options_set): the next frame applies it
    const int i = optionIndex(name);
    if (i < 0) return false;
    if (!std::strcmp(name, "widescreen_culling")) g_wideCulling = enable;
    if (!std::strcmp(name, "hud_edges")) g_wideHud = enable;
    s_want[size_t(i)] = enable && supported(name);
    return true;
}
bool optionHas(const char* name) { return optionIndex(name) >= 0; }
// The last crouch stab, for the pose's swing block and PvP: kind 1 stab / 2 power stab while it is recent (about 0.7 s,
// so that a lost pose does not lose the swing), 0 otherwise; the serial of the swings; bit 0 of flags the Master Sword;
// whether the thrust is still playing.
void stabSwingState(unsigned char* kind, unsigned char* serial, unsigned char* flags, bool* active) {
    const bool recent = s_stab.stabs > 0 && retraceCount() - s_stab.startedAt < 40;
    *kind = recent ? s_stab.kind : 0;
    *serial = s_stab.serial;
    *flags = s_stab.flags;
    *active = s_stab.active;
}
bool optionValue(const char* name, float v) {  // a knob of the Ocarina of Time controls (bluewake_game_options_set_value)
    for (int k = 0; k < kKnobCount; k++)
        if (!std::strcmp(kKnobNames[k], name)) return s_knob[k] = v, true;
    return false;
}
void introSkipSet(bool v) { s_introSkipStart = v; }
void optionForceClear() {
    s_forced.clear();
    s_heroMultiplier = 2.f;
}
void optionForce(const char* name, float heroMultiplier) {
    s_forced.push_back(name);
    if (heroMultiplier > 0.f) s_heroMultiplier = heroMultiplier;
}
bool optionDescribe(unsigned position, const char** name, const char** title, bool* defaultOn, bool* isOn) {
    if (position >= kOptionCount) return false;
    *name = kOptions[position].name, *title = kOptions[position].title;
    *defaultOn = kOptions[position].listOn, *isOn = s_on[position];
    return true;
}

void optionsFrame() {
    s_frame++;
    // At start, as game_options.c's bluewake_game_options_enable: each option's list default unless the list begins
    // "none", the list's word for it, Better Wind Waker's (1-15) only with its mod (BLUEWAKE_MODS: none in safe mode).
    if (!s_chosen) {
        const char* wanted = std::getenv("BLUEWAKE_OPTIONS");
        if (wanted == nullptr) return;  // the settings have not run yet (bw_settings_apply_launch)
        s_chosen = true;
        const bool none = std::strncmp(wanted, "none", 4) == 0;
        const char* mods = std::getenv("BLUEWAKE_MODS");
        const bool modOn = mods == nullptr || std::strstr(mods, "betterww") != nullptr;
        std::string list;
        for (size_t i = 0; i < kOptionCount; i++) {
            bool want = kOptions[i].listOn && !none;
            named(wanted, kOptions[i].name, &want);
            if (kOptions[i].index < 16) want = want && modOn;
            if (!std::strcmp(kOptions[i].name, "widescreen_culling")) g_wideCulling = want;
            if (!std::strcmp(kOptions[i].name, "hud_edges")) g_wideHud = want;
            s_want[i] = want && supported(kOptions[i].name);
            if (s_want[i]) list += (list.empty() ? "" : ",") + std::string(kOptions[i].name);
        }
        log("[options] %s\n", list.empty() ? "none" : list.c_str());
        // BLUEWAKE_OOT_KNOBS=name=value,... (testing): the Ocarina of Time controls' knobs, over the settings'.
        if (const char* knobs = std::getenv("BLUEWAKE_OOT_KNOBS"))
            for (const char* q = knobs; *q;) {
                const char* end = std::strchr(q, ',');
                const std::string word = end ? std::string(q, end) : std::string(q);
                if (const size_t eq = word.find('='); eq != std::string::npos)
                    optionValue(word.substr(0, eq).c_str(), std::strtof(word.c_str() + eq + 1, nullptr));
                q = end ? end + 1 : q + word.size();
            }
    }
    for (size_t i = 0; i < kOptionCount; i++)
        for (const std::string& f : s_forced)
            if (f == kOptions[i].name) s_want[i] = true;
    // Brisk Sail is Swift Sail with harder braking: it needs Swift Sail's sites.
    const int brisk = optionIndex("brisk_sail"), swift = optionIndex("swift_sail");
    if (brisk >= 0 && swift >= 0 && s_want[size_t(brisk)]) s_want[size_t(swift)] = true;
    for (size_t i = 0; i < kOptionCount; i++)
        if (s_want[i] != s_on[i]) {
            apply(i, s_want[i], ~0u);
            if (!s_want[i] && !std::strcmp(kOptions[i].name, "oot_crouch_stab")) stabSwitchedOff();
            s_on[i] = s_want[i];
            log("[options] %s %s\n", kOptions[i].name, s_on[i] ? "on" : "off");
        }
    instantTextFrame();
    sailNameFrame();
    apiOptionsRetrace();  // its game_options retrace's host part: preferences, the Bunny Hood, the player-gender text
}

void optionsLinked(u32 module) {
    for (size_t i = 0; i < kOptionCount; i++)
        if (s_on[i]) apply(i, true, module);
}

void optionsUnlinked(u32 module) {
    // The module's code and data go: its patches are dropped (another module may be linked at the
    // same addresses), and what was written there is not put back.
    for (const SiteDef& s : kSites)
        if (s.at.module == module)
            if (const u32 a = resolve(s.at)) interpUnpatch(a);
    for (const HookDef& h : kHooks)
        if (h.at.module == module)
            if (const u32 a = resolve(h.at)) interpUnpatch(a);
    for (size_t k = 0; k < sizeof kWrites / sizeof kWrites[0]; k++)
        if (kWrites[k].at.module == module) s_written.erase(k);
}

}  // namespace rt::bod
