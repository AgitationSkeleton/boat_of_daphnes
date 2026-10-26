// Boat of Daphnes' randomizer on this runtime (its runtime/host/src rando_seed.c, rando_host.c and rando_ui.c, as
// its main.c drives them; GPL-3.0-or-later).
//
// A randomizer session runs another game module, tww_game_rando.dll: wwrando's patched code (seed independent, the
// reference run of tools/rando/patch_game.py) recompiled, on that run's disc. The file select chooses it: Quest:
// Randomizer restarts the game into it (Boat of Daphnes' own route for a module that cannot switch in place), and
// main() picks the module and the disc from its settings before anything runs. What differs per seed reaches the game
// as its host has it (tools/rando/variant_sites.py's tables):
//   * the seed's code sites (li and compares holding an item id) - here the instruction is replaced by the one the
//     seed wants, which is what its hook after the instruction computes;
//   * the item bytes of data tables and wwrando's game variables (starting gear, hearts...), written into memory: a
//     REL's when it links;
//   * the option words a seed has off (rando_no_<option>): the vanilla instruction put back.
// Its file-select hook 50 plays the sounds and music its menu queues, through the game's own audio calls.
#include "runtime/bod.h"
#include "runtime/interp.h"
#include "runtime/runtime.h"

#include "runtime/modswitch.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

extern "C" {
// src/bod/shim/rando_bridge.c (its rando_seed.c)
typedef void (*BodRandoWrite)(void* user, unsigned address, const unsigned char* bytes, unsigned size);
typedef unsigned (*BodRandoSection)(void* user, unsigned module, unsigned section);
unsigned bod_rando_site_word(unsigned id, int* ok);
unsigned bod_rando_apply(BodRandoWrite write, BodRandoSection section, unsigned char* flags, unsigned* skipped);
int bod_rando_loaded(void);
const char* bod_rando_value(const char* option);
unsigned bod_rando_flag_for_option(const char* option);
unsigned bod_rando_targeting_symbol(void);
float bod_rando_damage(void);
const char* bod_rando_name(void);
const char* bod_rando_hash(void);
void bluewake_rando_set_state(bool module_has_variant, bool session_runs_it);
}

namespace rt {
extern std::atomic<bool> g_relLinked[kMaxModules];  // src/runtime/dispatch.cpp
}

namespace rt::bod {

bool hostInstalled();  // bod_host.cpp

namespace {
struct Where {
    u32 module, section, offset;
};
struct OptionDef {
    const char* name;
    int index;
    bool listOn;
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
#include "runtime/bod_rando_options.inc"

bool s_session = false;            // this session runs the randomizer's module
std::atomic<bool> s_applied{false};
unsigned char s_flags[256];        // its option switches (dolrecomp_option_flags), as the seed sets them

u32 resolve(const Where& w) {
    if (w.module == 0) return w.offset;
    if (w.module >= kMaxModules || !g_relLinked[w.module] || w.section >= 32) return 0;
    const u32 base = g_relBase[w.module][w.section];
    return base ? base + w.offset : 0;
}

// What the seed's writes replaced in main.dol's memory, for taking it out again (a REL's data goes with the REL).
std::unordered_map<u32, u8> s_undo;
void writeGuest(void*, unsigned address, const unsigned char* bytes, unsigned size) {
    for (unsigned i = 0; i < size; i++) {
        if (address + i < 0x80400000u) s_undo.emplace(address + i, u8(rd8(address + i)));
        wr8(address + i, bytes[i]);
    }
}
unsigned sectionStart(void*, unsigned module, unsigned section) {
    if (module >= kMaxModules || section >= 32 || !g_relLinked[module]) return 0;
    return g_relBase[module][section];
}

// The seed's instructions in one module (~0u: every one linked).
void patchSites(u32 module) {
    for (const SiteDef& s : kSites) {
        if (module != ~0u && s.at.module != module) continue;
        if (!s_flags[kOptions[s.option].index]) continue;  // the seed has the option on: wwrando's word stays
        if (const u32 a = resolve(s.at)) {
            interpPatch(a, s.value);
            interpOverrideAt(a);
        }
    }
    if (!s_flags[kOptions[0].index]) return;  // rando_seed: the seed's hooks
    for (const HookDef& h : kHooks) {
        if (module != ~0u && h.at.module != module) continue;
        int ok = 0;
        const u32 word = bod_rando_site_word(unsigned(h.id), &ok);
        const u32 a = resolve(h.at);
        if (!ok || !a) continue;
        interpPatch(a, word);
        interpOverrideAt(a);
    }
}

// The seed's instructions taken out (every one, linked modules' included).
void unpatchSites() {
    for (const SiteDef& s : kSites)
        if (const u32 a = resolve(s.at)) interpUnpatch(a);
    for (const HookDef& h : kHooks)
        if (const u32 a = resolve(h.at)) interpUnpatch(a);
}

// ---- the player's preferences over a seed (its rando_host.c: bluewake_rando_pref_set) ----
std::atomic<bool> s_prefMusic{false}, s_prefCompass{false}, s_prefSwitch{false}, s_prefsDirty{false};
// The sea compass's and music's switches are clear while wwrando's word (the option on) is in force, set for the game's own.
void applyPrefFlags() {
    const unsigned music = bod_rando_flag_for_option("remove_music"), compass = bod_rando_flag_for_option("invert_sea_compass_x_axis");
    if (music != 0 && music < 256) s_flags[music] = s_prefMusic ? 0 : 1;
    if (compass != 0 && compass < 256) s_flags[compass] = s_prefCompass ? 0 : 1;
}
void writeTargeting() {
    if (const unsigned symbol = bod_rando_targeting_symbol()) wr8(symbol, s_prefSwitch ? 1 : 0);
}

void unapplySeed() {
    unpatchSites();
    for (const auto& [a, v] : s_undo) wr8(a, v);
    log("[rando] seed put away: %zu bytes of main.dol restored\n", s_undo.size());
    s_undo.clear();
    optionForceClear();
    std::memset(s_flags, 0, sizeof s_flags);
}

void applySeed() {
    unsigned skipped = 0;
    std::memset(s_flags, 0, sizeof s_flags);
    const unsigned writes = bod_rando_apply(writeGuest, sectionStart, s_flags, &skipped);
    // The player's preferences decide the options that are the player's, not the seed's (its rando_host.c): Hero Mode and the
    // camera's invert are Better Wind Waker's and the Controls setting's own; Remove Music, Invert Sea Compass X-Axis and Switch
    // Targeting follow the settings menu (bluewake_rando_pref_set), live.
    applyPrefFlags();
    patchSites(~0u);
    writeTargeting();
    log("[rando] seed %s (%s): %u writes, %u data bytes not placed yet (their RELs are not linked)\n", bod_rando_name(),
        bod_rando_hash(), writes, skipped);
}

// ---- the file select's sounds and music (its rando_ui.c) ----
enum : u32 {
    FN_SE_START = 0x800F0D10u,      // mDoAud_seStart(id, pos, mapinfo, reverb)
    FN_BGM_START = 0x802A34A4u,     // JAIZelBasic::bgmStart(this, id, 0, 0)
    FN_LOAD_1ST = 0x80007424u,      // mDoAud_load1stDynamicWave(): 1 once the scene's wave banks are in
    FN_SET_SCENE = 0x800073D8u,     // mDoAud_setSceneName(name, room, layer)
    SPOT_DIR_NAME = 0x8039C5B8u,    // JAIZelBasic::spot_dir_name
    FN_SUBBGM_START = 0x802A47B8u,  // JAIZelBasic::subBgmStart(this, id)
    KIND_BGM = 0x40000000u,
    KIND_SUBBGM = 0x10000000u,
    KIND_SCENE = 0x20000000u,
    KIND_AFTER_SUB = 0x08000000u,  // with KIND_BGM: start once the sub-BGM (a jingle) has ended, so the music follows it at once
    QUEUE = 8,
};
std::atomic<unsigned> s_queue[QUEUE];
std::atomic<unsigned> s_head{0}, s_tail{0};
bool s_wavePending = false;
unsigned s_origScene = 0xFFFFFFFFu;

void queue(unsigned sound) {
    const unsigned tail = s_tail.load();
    if (tail - s_head.load() >= QUEUE) return;
    s_queue[tail % QUEUE] = sound;
    s_tail = tail + 1;
}

bool guestPtr(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

void runQueue(Ctx& c) {
    for (;;) {
        // A setSceneName was made: the game ignores every further one until mDoAud_load1stDynamicWave answers 1 (its
        // play scene does that for the scenes it makes): finished here, a frame's call at a time.
        if (s_wavePending) {
            if (guestCall(c, FN_LOAD_1ST) == 0) return;
            s_wavePending = false;
            log("[rando] file select audio scene loaded (mDoAud_load1stDynamicWave)\n");
        }
        const unsigned head = s_head.load();
        if (head == s_tail.load()) return;
        const unsigned sound = s_queue[head % QUEUE];
        const u32 audio = rd32(c.r[13] - 0x69D0u);  // the audio interface (JAIZelBasic), as d_s_name reaches it
        if ((sound & KIND_BGM) && (sound & KIND_AFTER_SUB)) {
            static int waited;
            const u32 sub = guestPtr(audio) ? rd32(audio + 0x74u) : 0xFFFFFFFFu;
            if (sub != 0xFFFFFFFFu && sub != 0u && ++waited < 900) return;  // the jingle is still playing: the entry waits
            if (waited > 0) log("[rando] the jingle ended after %d frames: the music follows\n", waited);
            waited = 0;
        }
        s_head = head + 1;
        if (sound & KIND_SCENE) {
            unsigned index = sound & 0xFFFFu;
            if (s_origScene == 0xFFFFFFFFu && guestPtr(audio)) s_origScene = rd8(audio + 0x220u);
            // "None" is the scene the file select had, so the new game's setSceneName finds what it expects.
            if (index == 0xFFFFu && s_origScene != 0xFFFFFFFFu && s_origScene != 0u) index = s_origScene - 1u;
            const u32 name = index == 0xFFFFu ? 0u : rd32(SPOT_DIR_NAME + index * 4u);
            guestCall(c, FN_SET_SCENE, {name, 0u, 0u});
            s_wavePending = true;
            log("[rando] file select audio scene %s (mDoAud_setSceneName)\n", sound == (KIND_SCENE | 0xFFFFu) ? "back" : "spot");
        } else if (sound & KIND_SUBBGM) {
            guestCall(c, FN_SUBBGM_START, {audio, 0x80000000u | (sound & 0xFFFFu)});
            log("[rando] file select sub-BGM 0x%08X started\n", 0x80000000u | (sound & 0xFFFFu));
        } else if (sound & KIND_BGM) {
            guestCall(c, FN_BGM_START, {audio, 0x80000000u | (sound & 0xFFFFu), 0u, 0u});
            log("[rando] file select music 0x%08X started\n", 0x80000000u | (sound & 0xFFFFu));
        } else {
            guestCall(c, FN_SE_START, {sound, 0u, 0u, 0u});
        }
    }
}
}  // namespace

bool randoSessionWanted() {
    // As its file select's Launch decides: only a restart the file select asked for comes back in the randomizer.
    const char* forced = std::getenv("BLUEWAKE_RANDOMIZER_MODE");
    if (forced && forced[0] == '1') return true;
    return cvarInt("gRandomizer.Mode", 0) == 1 && cvarInt("gRandomizer.ResumeAtFileSelect", 0) != 0;
}

namespace {
bool s_canSwitch = false;          // both programs are loaded: the file select switches while the game runs
std::atomic<bool> s_hookRan{false};
std::atomic<int> s_request{-1}, s_result{0};
}  // namespace

void randoSessionStarts(bool rando, bool canSwitch) {
    s_session = rando;
    s_canSwitch = canSwitch;
    bluewake_rando_set_state(rando || canSwitch, rando);
    log("[rando] this session is a %s one\n", rando ? "randomizer" : "vanilla");
}

bool randoSession() { return s_session; }

void randoRetrace() {
    // A preference changed while a seed is in force: its sites again, with the new switches.
    if (s_prefsDirty.exchange(false) && s_session && s_applied && bod_rando_loaded()) {
        unpatchSites();
        applyPrefFlags();
        patchSites(~0u);
        writeTargeting();
    }
    // Its pack is set by the file select's Launch, in the launch settings (the render thread, hostInstall).
    if (!s_session || s_applied || !hostInstalled()) return;
    s_applied = true;
    if (!bod_rando_loaded()) {
        log("[rando] the randomizer's module runs but no seed is loaded\n");
        return;
    }
    applySeed();
}

void randoLinked(u32 module) {
    if (!s_session || !s_applied || !bod_rando_loaded()) return;
    // The seed's bytes again (a REL's data tables can be placed now; the main program's are the same values) and the
    // module's sites.
    unsigned skipped = 0;
    unsigned char flags[256];
    bod_rando_apply(writeGuest, sectionStart, flags, &skipped);
    patchSites(module);
}

void randoUnlinked(u32 module) {
    if (!s_session || !s_applied) return;
    for (const SiteDef& s : kSites)
        if (s.at.module == module)
            if (const u32 a = resolve(s.at)) interpUnpatch(a);
    for (const HookDef& h : kHooks)
        if (h.at.module == module)
            if (const u32 a = resolve(h.at)) interpUnpatch(a);
}

namespace {
enum : u32 {
    FN_CCC_INIT = 0x800227A0u,       // cCc_Init(): the dynamic module controls from DynamicNameTable (c_dylink.cpp)
    DMC_INITIALIZED = 0x803F69C0u,   // c_dylink.cpp's DMC_initialized
    FN_ARCHIVE_HEAP = 0x80011AB4u,   // mDoExt_getArchiveHeap()
    FN_ARCHIVE_MOUNT = 0x802B8008u,  // JKRArchive::mount(path, mode, heap, direction)
    FN_LOADER_UNMOUNT = 0x802B6854u, // JKRFileLoader::unmount(this)
    PLAY_ITEMICON = 0x803CA680u,     // g_dComIfG_gameInfo.play.mpItemIconArchive: itemicon.arc
    PLAY_MSGDT = 0x803CA6A0u,        // ... mpEnglishTextArchive: bmgres.arc
    DMC_ARCHIVE = 0x803F7314u,       // DynamicModuleControl::sArchive: RELS.arc
    OS_MODULE_LIST = 0x800030C8u,    // __OSModuleInfoList (head, tail; each module's link: next at +4)
    FN_OS_UNLINK = 0x803056BCu,      // OSUnlink(OSModuleInfo*)
    FN_DMC_CT = 0x802405BCu,         // DynamicModuleControl::DynamicModuleControl(const char* name)
    FN_DMC_LINK = 0x8024019Cu,       // DynamicModuleControlBase::link()
};

u32 guestString(const char* text) {
    const u32 n = u32(std::strlen(text)) + 1;
    const u32 a = hleAlloc(n);
    std::memcpy(hostp(a), text, n);
    return a;
}

// A resident archive mounted again, so it is read from the disc as the program in force has it (its rando_ui.c reload_one).
void remount(Ctx& c, const char* path, u32 slot) {
    const u32 old = rd32(slot);
    if (old < 0x80000000u || old >= 0x81800000u) return;
    static std::unordered_map<std::string, u32> s_paths;
    u32& text = s_paths[path];
    if (!text) text = guestString(path);
    const u32 heap = guestCall(c, FN_ARCHIVE_HEAP);
    guestCall(c, FN_LOADER_UNMOUNT, {old});
    const u32 fresh = guestCall(c, FN_ARCHIVE_MOUNT, {text, 1u, heap, 1u});
    wr32(slot, fresh);
    log("[rando] resident archive %s mounted again: %08X -> %08X%s\n", path, old, fresh, fresh ? "" : " (FAILED)");
}

// The name entry's title is message 40 ("Name Entry"; a seed's file adds "Seed hash:" and the hash under it, wwrando's
// show_seed_hash_on_name_entry_screen), taken once as the Name scene was made (dName_c::screenSet). A switch made in that
// scene left it as the other message file had it: a vanilla file was named under the seed's hash (user 2026-10-25). It is
// taken again from the file now mounted: dScnName_c +0x424 its dName_c, whose titlePane (+0x20AC) is the J2DTextBox.
void refreshNameTitle(Ctx& c) {
    constexpr u32 kSceneList = 0x80372150, kNameScene = 0x000C, kDName = 0x424, kTitlePane = 0x20AC;
    constexpr u32 FN_MESSAGE_GET = 0x8002BB78, FN_TEXTBOX_SET_STRING = 0x802D5AAC;
    auto guest = [](u32 a) { return a >= 0x80000000u && a < 0x81800000u; };
    const u32 node = rd32(kSceneList);
    const u32 scene = guest(node) ? rd32(node + 0xC) : 0;
    if (!guest(scene) || rd16(scene + 8) != kNameScene) return;
    const u32 name = rd32(scene + kDName);
    const u32 box = guest(name) ? rd32(name + kTitlePane) : 0;
    if (!guest(box)) return;
    static const u32 s_text = hleAlloc(1024);
    wr8(s_text, 0);
    guestCall(c, FN_MESSAGE_GET, {s_text, 0x28});
    if (rd8(s_text) == 0) return;
    guestCall(c, FN_TEXTBOX_SET_STRING, {box, s_text});
    log("[rando] the name entry's title taken again from the message file now mounted\n");
}

// Its host's in-place switch (main.c host_rando_inplace_retrace and rando_ui.c's rebuild), here in one go on the file
// select's frame: the seed out, the other program in force, the game's module controls and resident archives made again
// for it, the seed in.
bool switchNow(Ctx& c, bool on) {
    if (on == s_session) {  // the same mode: another seed (the app has set its pack and its files)
        unapplySeed();
        if (on && bod_rando_loaded()) applySeed();
        // The new seed's message and item icon files (its overlay) are read only when mounted: the name entry's "Seed
        // hash" text, shop and hint texts.
        remount(c, "/res/Msg/bmgres.arc", PLAY_MSGDT);
        remount(c, "/res/Msg/itemicon.arc", PLAY_ITEMICON);
        refreshNameTitle(c);
        log("[rando] seed changed while the game runs: %s (%s)\n", bod_rando_name(), bod_rando_hash());
        return true;
    }
    if (!on) unapplySeed();
    if (!modswitchTo(on ? 1 : 0)) return false;
    s_session = on;
    s_applied = on;
    bluewake_rando_set_state(true, on);
    // RELS.arc, bmgres.arc and itemicon.arc stay mounted from boot: mounted again from the disc as it is now.
    if (const u32 rels = rd32(DMC_ARCHIVE); rels >= 0x80000000u && rels < 0x81800000u) {
        // As DynamicModuleControl::mountCallback mounts it (compressed, into the archive heap); the randomizer's is
        // stored uncompressed and bigger than the room the heap has by the file select, so then it is mounted to be
        // read from the disc a module at a time (JKRDvdArchive).
        static u32 s_name = 0;
        if (!s_name) s_name = guestString("RELS.arc");
        guestCall(c, FN_LOADER_UNMOUNT, {rels});
        const u32 heap = guestCall(c, FN_ARCHIVE_HEAP);
        u32 fresh = guestCall(c, FN_ARCHIVE_MOUNT, {s_name, 4u /* MOUNT_COMP */, heap, 1u});
        const bool fromDisc = !fresh;
        if (fromDisc) fresh = guestCall(c, FN_ARCHIVE_MOUNT, {s_name, 3u /* MOUNT_DVD */, heap, 1u});
        wr32(DMC_ARCHIVE, fresh);
        log("[rando] RELS.arc mounted again%s: %08X -> %08X\n", fromDisc ? " (read from the disc)" : "", rels, fresh);
    }
    // The profile list (module 1, f_pc_profile_lst, linked at boot and kept): every actor's profile, its pointers into
    // the actor modules filled by OSLink as each links - from its own relocation table, which places them as its
    // program lays the modules out. The other program's copy put the randomizer's re-laid chest module's profile at a
    // vanilla address (a garbage size, no chest): the copy of the program in force is linked in its place.
    {
        u32 info = rd32(OS_MODULE_LIST);
        for (int guard = 0; info && rd32(info) != 1 && guard < 1024; guard++) info = rd32(info + 4);
        if (info) guestCall(c, FN_OS_UNLINK, {info});
        static u32 s_name = 0;
        if (!s_name) s_name = guestString("f_pc_profile_lst");
        const u32 dmc = hleAlloc(0x80);  // a DynamicModuleControl of its own, as cDyl_InitCallback makes one
        guestCall(c, FN_DMC_CT, {dmc, s_name});
        const u32 ok = guestCall(c, FN_DMC_LINK, {dmc});
        log("[rando] the profile list linked again from the %s disc: %s\n", on ? "randomizer's" : "game's", ok ? "ok" : "FAILED");
    }
    // The program in force's cDyl code has its own module controls (the randomizer's: a larger name table and array, in
    // its added section): cCc_Init builds them (it asserts it has not run).
    wr8(DMC_INITIALIZED, 0);
    guestCall(c, FN_CCC_INIT);
    remount(c, "/res/Msg/bmgres.arc", PLAY_MSGDT);
    remount(c, "/res/Msg/itemicon.arc", PLAY_ITEMICON);
    refreshNameTitle(c);
    if (on) {
        if (bod_rando_loaded()) applySeed();
        else log("[rando] the randomizer is in force but no seed is loaded\n");
    }
    log("[rando] the randomizer is %s while the game runs\n", on ? "on" : "off");
    return true;
}
}  // namespace

u32 randoUiHook(Ctx& c) {
    s_hookRan = true;
    // Testing: BLUEWAKE_TEST_RANDO_SWITCH=<n>[,<m>] switches to the randomizer at the file select's n-th frame (and back at
    // the m-th), as its Quest selector does.
    static const char* s_test = std::getenv("BLUEWAKE_TEST_RANDO_SWITCH");
    if (s_test) {
        static unsigned s_calls = 0;
        const unsigned at = unsigned(std::atoi(s_test)), back = std::strchr(s_test, ',') ? unsigned(std::atoi(std::strchr(s_test, ',') + 1)) : 0;
        ++s_calls;
        if (s_calls == at) s_request = 1;
        if (back && s_calls == back) s_request = 0;
    }
    if (const int want = s_request.exchange(-1); want >= 0) {
        const Ctx saved = c;
        s_result = switchNow(c, want == 1) ? 1 : 2;
        c = saved;
    }
    if (!s_wavePending && s_head.load() == s_tail.load()) return 0;
    // The calls run on the file select's own thread, at the top of dFile_select_c::_move: its registers come back as they were.
    const Ctx saved = c;
    runQueue(c);
    c = saved;
    return 0;
}

}  // namespace rt::bod

// ---- what its file select calls (rando_ui.h, its main.c's in-place switch) ----
extern "C" {
bool bluewake_rando_inplace_supported(void) { return rt::bod::s_canSwitch; }
bool bluewake_rando_inplace_ready(void) { return rt::bod::s_canSwitch && rt::bod::s_hookRan; }
bool bluewake_rando_inplace_can_leave(void) { return rt::bod::s_canSwitch; }
void bluewake_rando_inplace_request(bool rando) {
    rt::bod::s_result = 0;
    rt::bod::s_request = rando ? 1 : 0;
}
int bluewake_rando_inplace_poll(void) { return rt::bod::s_result; }  // 0 pending, 1 done, 2 failed
void bluewake_rando_ui_sfx(unsigned sound) { rt::bod::queue(sound); }
void bluewake_rando_ui_scene(unsigned index) { rt::bod::queue(rt::bod::KIND_SCENE | (index & 0xFFFFu)); }
void bluewake_rando_ui_subbgm(unsigned sequence) { rt::bod::queue(rt::bod::KIND_SUBBGM | (sequence & 0xFFFFu)); }
void bluewake_rando_ui_bgm(unsigned sequence) { rt::bod::queue(rt::bod::KIND_BGM | (sequence & 0xFFFFu)); }
void bluewake_rando_ui_bgm_after_subbgm(unsigned sequence) {
    rt::bod::queue(rt::bod::KIND_BGM | rt::bod::KIND_AFTER_SUB | (sequence & 0xFFFFu));
}
bool bluewake_rando_ui_hook_ran(void) { return rt::bod::s_hookRan; }
void bluewake_rando_pref_set(const char* option, bool on) {
    using namespace rt::bod;
    if (!std::strcmp(option, "remove_music")) s_prefMusic = on;
    else if (!std::strcmp(option, "invert_sea_compass_x_axis")) s_prefCompass = on;
    else if (!std::strcmp(option, "switch_targeting_mode")) s_prefSwitch = on;
    else return;
    s_prefsDirty = true;
}
}
