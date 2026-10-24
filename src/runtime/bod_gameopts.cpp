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

#include <array>
#include <atomic>
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
            size_t p = textStart + offset;
            while (p < textEnd && d[p] != 0) {
                if (d[p] != 0x1A) {
                    p++;
                    continue;
                }
                if (textEnd - p < 2) return false;
                const size_t length = d[p + 1];
                if (length < 2 || length > textEnd - p) return false;
                if (pass && length == 7 && d[p + 2] == 0 && d[p + 3] == 0) {
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
    if (!first && s_frame % 60 != 0 && s_textCopies.size() == before) return;
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
    std::erase_if(s_sailCopies, [](const BmgCopy& c) { return !bmgIntact(c); });
    if (!s_sailCopies.empty()) return;
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
