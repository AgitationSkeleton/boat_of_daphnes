// The Item Tracker: see item_tracker.h.
#ifdef rename
#undef rename // bw_posix_compat.h's, which <filesystem> (aurora's headers) must not see
#endif

#include "item_tracker.h"

#include "item_found.h"
#include "ship_menu.h"
#include "ship/cvar.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "dev_tools.h"  // the host's (runtime/host/src)
#include "game_state.h"
}

extern "C" bool bluewake_game_options_get(const char* name);
extern "C" bool bluewake_bunny_hood_enabled(void);

namespace itemtracker {
namespace {

using shipmenu::WidgetInfo;
using shipmenu::WidgetType;

#define TRACKER_CVAR(name) "gTrackers.ItemTracker." name
constexpr const char* kWindowOpen = "gOpenWindows.ItemTracker";

// --- Options (the CVars' values) ---------------------------------------------------

enum { WINDOW_FLOATING, WINDOW_WINDOW };
enum { DISPLAY_ALWAYS, DISPLAY_COMBO_BUTTON };
enum { SECTION_HIDDEN, SECTION_MAIN_WINDOW, SECTION_SEPARATE };
enum { NUMBER_NONE, NUMBER_CURRENT_CAPACITY_ONLY, NUMBER_CURRENT_AMMO_ONLY, NUMBER_CAPACITY, NUMBER_AMMO };
enum { KEYS_COLLECTED_MAX, KEYS_CURRENT_COLLECTED_MAX, KEYS_CURRENT_MAX };

// The buttons a combo can hold, as the game keeps them: g_mDoCPd_cpadInfo[0].mButtonHold is a bit field
// (c_API_controller_pad.h, most significant bit first: left, right, down, up, Z, R, L, A, then B, X, Y, Start).
struct ComboButton {
    const char* label;
    unsigned mask;
};
const ComboButton kComboButtons[] = {
    {"A Button", 0x0100}, {"B Button", 0x0080}, {"X Button", 0x0040}, {"Y Button", 0x0020},
    {"Z Button", 0x0800}, {"L Button", 0x0200}, {"R Button", 0x0400}, {"Start", 0x0010},
    {"D-Up", 0x1000},     {"D-Down", 0x2000},   {"D-Left", 0x8000},   {"D-Right", 0x4000},
};
constexpr int kComboL = 5, kComboR = 6;

// --- The game's memory (GZLE01; tww decomp d_save.h) --------------------------------

constexpr unsigned kInfo = 0x803C4C08u;       // g_dComIfG_gameInfo (dSv_info_c)
constexpr unsigned kSnapshot = 0x7A0u;        // through the current stage's memory (0x778, 0x24)
constexpr unsigned kMaxLife = 0x00, kRupees = 0x04, kWallet = 0x12, kMaxMagic = 0x13, kMagic = 0x14;
constexpr unsigned kItems = 0x3C;             // dSv_player_item_c::mItems[21], by inventory slot
constexpr unsigned kArrows = 0x69, kBombs = 0x6A;     // dSv_player_item_record_c
constexpr unsigned kMaxArrows = 0x6F, kMaxBombs = 0x70; // dSv_player_item_max_c
constexpr unsigned kCollect = 0xB4;           // dSv_player_collect_c: swords, shields, bracelets, charms, ..., songs, shards, pearls
constexpr unsigned kMemoryTable = 0x380;      // dSv_save_c::mMemory[16], 0x24 each
constexpr unsigned kCurrentMemory = 0x778;    // dSv_info_c::mMemory, the stage Link is in
constexpr unsigned kKeyNum = 0x20, kDungeonItem = 0x21; // in a dSv_memBit_c
constexpr unsigned kStageInfoPtr = 0x803C9DA0u; // play.mStageData.mpStagInfo (play 0x803C5EA8 + 0x3EB0 + 0x48)
constexpr unsigned kMenuPause = 0x803F7097u;  // dMenu_pause: non-zero while the pause menu is up
constexpr unsigned kPadHold = 0x803A4E20u;    // g_mDoCPd_cpadInfo[0].mButtonHold (+0x30 of 0x803A4DF0)
constexpr int kNoItem = 0xFF;

struct Save {
    uint8_t b[kSnapshot];
    unsigned u8(unsigned o) const { return b[o]; }
    unsigned u16(unsigned o) const { return unsigned(b[o]) << 8 | b[o + 1]; }
};

bool read_u32(unsigned address, unsigned* out) {
    unsigned char raw[4];
    if (!bluewake_dev_read(address, raw, 4))
        return false;
    *out = unsigned(raw[0]) << 24 | unsigned(raw[1]) << 16 | unsigned(raw[2]) << 8 | raw[3];
    return true;
}

// The save table of the stage Link is in (dStage_stagInfo_GetSaveTbl), -1 when unknown.
int current_stage_table() {
    unsigned info = 0;
    if (!read_u32(kStageInfoPtr, &info) || info < 0x80000000u || info > 0x81800000u)
        return -1;
    unsigned char prop = 0;
    if (!bluewake_dev_read(info + 9, &prop, 1))
        return -1;
    return (prop >> 1) & 0x7F;
}

// --- Dungeons ---------------------------------------------------------------------

struct Dungeon {
    const char* shortName;
    const char* name;
    int table;      // dSv_save_c::SaveStageTbl
    int maxKeys;    // small keys in the dungeon; 0: it has none (nor a big key)
};
// In the order the game goes. Forsaken Fortress holds a map and a compass only.
const Dungeon kDungeons[] = {
    {"DRC", "Dragon Roost Cavern", 3, 4}, {"FW", "Forbidden Woods", 4, 1}, {"TOTG", "Tower of the Gods", 5, 2},
    {"FF", "Forsaken Fortress", 2, 0},    {"ET", "Earth Temple", 6, 3},    {"WT", "Wind Temple", 7, 2},
};
constexpr int kDungeonCount = int(sizeof kDungeons / sizeof kDungeons[0]);

struct DungeonState {
    int keys = 0;
    int collected = 0; // keys picked up (a high-water mark: the save keeps no such count)
    bool map = false, compass = false, bossKey = false;
};

struct KeyTrack {
    int collected = 0;
    int previous = -1;
};
KeyTrack g_track[kDungeonCount];
int g_track_slot = -1;
int g_current_table = -1;

DungeonState dungeon_state(const Save& s, int index) {
    const Dungeon& d = kDungeons[index];
    const bool live = d.table == g_current_table;
    const unsigned base = live ? kCurrentMemory : kMemoryTable + unsigned(d.table) * 0x24u;
    DungeonState st;
    st.keys = int(s.u8(base + kKeyNum));
    const unsigned bits = s.u8(base + kDungeonItem);
    st.map = (bits & 1u) != 0;
    st.compass = (bits & 2u) != 0;
    st.bossKey = (bits & 4u) != 0;
    KeyTrack& t = g_track[index];
    // Keys come up only in the dungeon Link is in, where the save is the live one; a drop is a key used.
    if (live && t.previous >= 0 && st.keys > t.previous)
        t.collected += st.keys - t.previous;
    t.previous = live ? st.keys : -1;
    t.collected = std::max(t.collected, st.keys);
    st.collected = t.collected;
    return st;
}

// --- Cells ------------------------------------------------------------------------

enum class K : uint8_t {
    Blank, Slot, Sword, Shield, Bracelets, PirateCharm, HeroCharm, BunnyHood, Wallet, BombBag, Quiver, Magic, Hearts, Pieces,
    Pearl, Shard, Song, DKey, DBoss, DMap, DCompass,
};

struct Cell {
    K kind = K::Blank;
    int a = 0;          // slot, pearl, shard or song number, dungeon index
    bool label = false; // the dungeon's short name over the icon
};

// What a cell shows.
struct Count {
    bool show = false;
    bool keys = false;         // a small key count: current, collected, max
    bool fixedCapacity = false; // hearts and pieces: the count is the ammo whatever the mode
    int capacity = 0, maxCapacity = 0, ammo = 0;
    int current = 0, collected = 0, max = 0;
};

struct Look {
    int item = -1;            // an item number (item_found.h), or
    const char* file = nullptr; // a member of itemicon.arc
    bool has = false;
    std::string tip;
    unsigned tint = 0xFFFFFFFFu;
    const char* abbrev = nullptr; // text over the icon (songs)
    bool magicBar = false;
    Count count;
};

// The inventory slots (dInvSlot) and the item each shows when empty.
constexpr int kSlotItem[21] = {0x20, 0x78, 0x22, 0x25, 0x24, 0x2D, 0x34, 0x21, 0x23, 0x29, 0x2A,
                               0x2C, 0x27, 0x31, 0x50, 0x50, 0x50, 0x50, 0x30, 0x2F, 0x33};
constexpr int kSlotBow = 12, kSlotBombs = 13;

constexpr const char* kSongNames[6] = {"Wind's Requiem", "Ballad of Gales", "Command Melody",
                                       "Earth God's Lyric", "Wind God's Aria", "Song of Passing"};
constexpr const char* kSongAbbrev[6] = {"WR", "BG", "CM", "EL", "WA", "SP"};
// The conductor's tint of each song (the baton's glow in the game).
constexpr unsigned kSongTint[6] = {IM_COL32(110, 230, 120, 255), IM_COL32(110, 160, 255, 255),
                                   IM_COL32(255, 110, 110, 255), IM_COL32(255, 180, 90, 255),
                                   IM_COL32(255, 235, 100, 255), IM_COL32(200, 130, 255, 255)};

const char* kShardFiles[8] = {"triforce_00.bti", "triforce_01.bti", "triforce_02.bti", "triforce_03.bti",
                              "triforce_04.bti", "triforce_05.bti", "triforce_06.bti", "triforce_07.bti"};

struct Options {
    int countType = NUMBER_CURRENT_CAPACITY_ONLY;
    int keyType = KEYS_COLLECTED_MAX;
    bool alignLeft = false;
};

Look look_of(const Cell& c, const Save& s, const Options& o) {
    Look l;
    switch (c.kind) {
    case K::Slot: {
        const int item = int(s.u8(kItems + unsigned(c.a)));
        l.has = item != kNoItem;
        l.item = l.has ? item : kSlotItem[c.a];
        l.tip = item_found::Name(l.item);
        if (l.has && (c.a == kSlotBow || c.a == kSlotBombs)) {
            const bool bombs = c.a == kSlotBombs;
            l.count.show = true;
            l.count.capacity = int(s.u8(bombs ? kMaxBombs : kMaxArrows));
            l.count.maxCapacity = 99;
            l.count.ammo = int(s.u8(bombs ? kBombs : kArrows));
        }
        break;
    }
    case K::Sword: {
        const unsigned bits = s.u8(kCollect + 0);
        const int level = (bits & 8) ? 3 : (bits & 4) ? 2 : (bits & 2) ? 1 : 0;
        static const char* files[4] = {"sword_00.bti", "sword_01.bti", "sword_02.bti", "sword_03.bti"};
        static const int items[4] = {0x38, 0x39, 0x3A, 0x3E};
        l.has = (bits & 0xF) != 0;
        l.file = files[level];
        l.tip = item_found::Name(items[level]);
        break;
    }
    case K::Shield: {
        const unsigned bits = s.u8(kCollect + 1);
        const bool mirror = (bits & 2) != 0;
        l.has = (bits & 3) != 0;
        l.file = mirror ? "shield_01.bti" : "shield_00.bti";
        l.tip = item_found::Name(mirror ? 0x3C : 0x3B);
        break;
    }
    case K::Bracelets:
        l.has = (s.u8(kCollect + 2) & 1) != 0;
        l.item = 0x28;
        l.tip = item_found::Name(0x28);
        break;
    case K::PirateCharm:
        l.has = (s.u8(kCollect + 3) & 1) != 0;
        l.item = 0x42;
        l.tip = item_found::Name(0x42);
        break;
    case K::HeroCharm:
        l.has = (s.u8(kCollect + 4) & 1) != 0;
        l.item = 0x43;
        l.tip = item_found::Name(0x43);
        break;
    case K::BunnyHood: // bit 2 of the Hero's Charm's byte (runtime/host/src/bunny_hood.h)
        l.has = (s.u8(kCollect + 4) & 4) != 0;
        l.item = 0x44;
        l.tip = item_found::Name(0x44);
        break;
    case K::Wallet: {
        const unsigned size = std::min(s.u8(kWallet), 2u);
        static const char* files[3] = {"get_rupy.bti", "big_purse.bti", "max_purse.bti"};
        static const int caps[3] = {200, 1000, 5000};
        static const char* names[3] = {"Wallet", "Giant Wallet", "Tycoon Wallet"};
        l.has = true;
        l.file = files[size];
        l.tip = names[size];
        if (size == 0)
            l.tint = IM_COL32(70, 230, 90, 255); // the plain rupee icon is white: tinted green as the game does
        l.count = {true, false, false, caps[size], 5000, int(s.u16(kRupees)), 0, 0, 0};
        break;
    }
    case K::BombBag: {
        const int cap = int(s.u8(kMaxBombs));
        l.has = int(s.u8(kItems + kSlotBombs)) != kNoItem;
        l.file = cap >= 99 ? "bombpouch_2.bti" : cap >= 60 ? "bombpouch_1.bti" : "bomb_00.bti";
        l.tip = cap >= 99 ? "Bomb Bag (99)" : cap >= 60 ? "Bomb Bag (60)" : "Bomb Bag (30)";
        l.count = {l.has, false, false, cap, 99, int(s.u8(kBombs)), 0, 0, 0};
        break;
    }
    case K::Quiver: {
        const int cap = int(s.u8(kMaxArrows));
        l.has = int(s.u8(kItems + kSlotBow)) != kNoItem;
        l.file = cap >= 99 ? "arrowcase_2.bti" : cap >= 60 ? "arrowcase_1.bti" : "arrow_00.bti";
        l.tip = cap >= 99 ? "Quiver (99)" : cap >= 60 ? "Quiver (60)" : "Quiver (30)";
        l.count = {l.has, false, false, cap, 99, int(s.u8(kArrows)), 0, 0, 0};
        break;
    }
    case K::Magic: {
        const int cap = int(s.u8(kMaxMagic));
        l.has = cap > 0;
        l.magicBar = true;
        l.tip = cap >= 32 ? "Double Magic Meter" : "Magic Meter";
        l.count = {l.has, false, false, cap, 32, int(s.u8(kMagic)), 0, 0, 0};
        break;
    }
    case K::Hearts: {
        const int quarters = int(s.u16(kMaxLife));
        l.has = true;
        l.item = 0x08;
        l.tip = "Heart Containers";
        l.count = {true, false, true, 20, 20, quarters / 4, 0, 0, 0};
        break;
    }
    case K::Pieces: {
        const int pieces = int(s.u16(kMaxLife)) % 4;
        l.has = pieces > 0;
        l.item = 0x07;
        l.tip = "Pieces of Heart";
        l.count = {true, false, true, 4, 4, pieces, 0, 0, 0};
        break;
    }
    case K::Pearl: {
        static const int items[3] = {0x69, 0x6A, 0x6B}; // Nayru, Din, Farore (dSymbol bits 0..2)
        l.has = (s.u8(kCollect + 0xB) & (1u << c.a)) != 0;
        l.item = items[c.a];
        l.tip = item_found::Name(items[c.a]);
        break;
    }
    case K::Shard:
        l.has = (s.u8(kCollect + 0xA) & (1u << c.a)) != 0;
        l.file = kShardFiles[c.a];
        l.item = 0x61 + c.a;
        l.tip = item_found::Name(0x61 + c.a);
        break;
    case K::Song:
        l.has = (s.u8(kCollect + 9) & (1u << c.a)) != 0;
        l.item = 0x6D + c.a;
        l.tint = kSongTint[c.a];
        l.abbrev = kSongAbbrev[c.a];
        l.tip = kSongNames[c.a];
        break;
    case K::DKey: {
        const Dungeon& d = kDungeons[c.a];
        const DungeonState st = dungeon_state(s, c.a);
        l.has = st.keys > 0 || st.collected > 0;
        l.item = 0x15;
        l.tip = std::string(d.name) + " Small Key";
        l.count.show = true;
        l.count.keys = true;
        l.count.current = st.keys;
        l.count.collected = st.collected;
        l.count.max = d.maxKeys;
        break;
    }
    case K::DBoss:
        l.has = dungeon_state(s, c.a).bossKey;
        l.item = 0x4E;
        l.tip = std::string(kDungeons[c.a].name) + " Big Key";
        break;
    case K::DMap:
        l.has = dungeon_state(s, c.a).map;
        l.item = 0x4C;
        l.tip = std::string(kDungeons[c.a].name) + " Dungeon Map";
        break;
    case K::DCompass:
        l.has = dungeon_state(s, c.a).compass;
        l.item = 0x4D;
        l.tip = std::string(kDungeons[c.a].name) + " Compass";
        break;
    case K::Blank:
        break;
    }
    (void)o;
    return l;
}

// --- Sections ---------------------------------------------------------------------

using Cells = std::vector<Cell>;

Cells inventory_cells() {
    Cells c;
    for (int i = 0; i < 21; ++i)
        c.push_back({K::Slot, i, false});
    return c;
}

Cells equipment_cells() {
    if (!bluewake_bunny_hood_enabled()) // experimental (gExperimental.BunnyHood): the row as it was
        return {{K::Sword, 0, false}, {K::Shield, 0, false}, {K::Bracelets, 0, false}, {K::HeroCharm, 0, false},
                {K::PirateCharm, 0, false}};
    return {{K::Sword, 0, false}, {K::Shield, 0, false}, {K::Bracelets, 0, false}, {K::HeroCharm, 0, false},
            {K::BunnyHood, 0, false}, {K::PirateCharm, 0, false}};
}

Cells capacity_cells() {
    return {{K::Wallet, 0, false}, {K::BombBag, 0, false}, {K::Quiver, 0, false},
            {K::Magic, 0, false},  {K::Hearts, 0, false},  {K::Pieces, 0, false}};
}

// The three pearls, then the eight Triforce shards: each on its own line (see Block).
Cells pearl_cells() {
    Cells c;
    for (int i = 0; i < 3; ++i)
        c.push_back({K::Pearl, i, false});
    return c;
}

Cells shard_cells() {
    Cells c;
    for (int i = 0; i < 8; ++i)
        c.push_back({K::Shard, i, false});
    return c;
}

Cells song_cells() {
    Cells c;
    for (int i = 0; i < 6; ++i)
        c.push_back({K::Song, i, false});
    return c;
}

// Each dungeon is a column of its items. Every column has the same rows (small keys, big key, map,
// compass) so each kind sits on one line across the dungeons, aligned under their labels; a dungeon
// without that item leaves its place empty. The columns come in bands of `columns`.
Cells dungeon_cells(int columns, bool maps) {
    std::vector<K> rows = {K::DKey, K::DBoss};
    if (maps) {
        rows.push_back(K::DMap);
        rows.push_back(K::DCompass);
    }
    Cells cells;
    for (int first = 0; first < kDungeonCount; first += columns) {
        for (size_t r = 0; r < rows.size(); ++r) {
            for (int i = first; i < first + columns; ++i) {
                if (i >= kDungeonCount) {
                    cells.push_back(Cell{});
                    continue;
                }
                const bool has = kDungeons[i].maxKeys > 0 || (rows[r] != K::DKey && rows[r] != K::DBoss);
                Cell c{has ? rows[r] : K::Blank, i, r == 0};
                cells.push_back(c);
            }
        }
    }
    return cells;
}

// A run of cells in `columns` columns. A section is one or more blocks stacked, so a category that fits
// one line is on one line, whatever the window's other grids are.
struct Block {
    Cells cells;
    int columns;
};
using Blocks = std::vector<Block>;

// --- Drawing ----------------------------------------------------------------------

struct Style {
    float icon, spacing, text; // in screen pixels
    int type;                  // WINDOW_*
    Options opt;
};

constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kGreen = IM_COL32(0, 255, 0, 255);
constexpr ImU32 kGray = IM_COL32(155, 155, 155, 255);

void shadow_text(ImDrawList* dl, ImVec2 at, float px, ImU32 color, const std::string& text) {
    if (text.empty())
        return;
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, px, ImVec2(at.x + 1.f, at.y + 1.f), IM_COL32(0, 0, 0, 220), text.c_str());
    dl->AddText(font, px, at, color, text.c_str());
}

float text_width(float px, const std::string& text) {
    return ImGui::GetFont()->CalcTextSizeA(px, 1e9f, 0.f, text.c_str()).x;
}

// A count over the bottom of an icon: the current part white (green when full), the maximum green.
void draw_pair(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Style& st, bool alignLeft, ImU32 curColor,
               const std::string& cur, const std::string& max) {
    const float width = text_width(st.text, cur) + text_width(st.text, max);
    const float x = alignLeft ? p0.x : p0.x + (p1.x - p0.x) * 0.5f - width * 0.5f;
    const float y = p1.y - st.text - 1.f;
    shadow_text(dl, ImVec2(x, y), st.text, curColor, cur);
    shadow_text(dl, ImVec2(x + text_width(st.text, cur), y), st.text, kGreen, max);
}

void draw_count(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Style& st, const Count& c) {
    const Options& o = st.opt;
    if (!c.show)
        return;
    if (c.keys) {
        std::string cur;
        if (o.keyType == KEYS_CURRENT_COLLECTED_MAX || o.keyType == KEYS_CURRENT_MAX)
            cur += std::to_string(c.current) + "/";
        if (o.keyType == KEYS_COLLECTED_MAX || o.keyType == KEYS_CURRENT_COLLECTED_MAX)
            cur += std::to_string(c.collected) + "/";
        draw_pair(dl, p0, p1, st, false, kWhite, cur, std::to_string(c.max));
        return;
    }
    if (o.countType == NUMBER_NONE || c.capacity <= 0)
        return;
    const bool alignLeft = o.alignLeft && o.countType != NUMBER_CAPACITY && o.countType != NUMBER_AMMO;
    const bool ammo = o.countType == NUMBER_AMMO || o.countType == NUMBER_CURRENT_AMMO_ONLY || c.fixedCapacity;
    const bool showMax = !(o.countType == NUMBER_CURRENT_CAPACITY_ONLY || o.countType == NUMBER_CURRENT_AMMO_ONLY);
    ImU32 color = kWhite;
    std::string cur, max;
    if (ammo) {
        cur = std::to_string(c.ammo);
        if (c.ammo >= c.capacity)
            color = kGreen;
        if (showMax) {
            cur += "/";
            max = std::to_string(c.capacity);
        }
        if (c.ammo <= 0)
            color = kGray;
    } else {
        cur = std::to_string(c.capacity);
        if (c.capacity >= c.maxCapacity)
            color = kGreen;
        else if (showMax) {
            cur += "/";
            max = std::to_string(c.maxCapacity);
        }
    }
    draw_pair(dl, p0, p1, st, alignLeft, color, cur, max);
}

void draw_magic(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool has, const Count& c) {
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    const ImVec2 a(p0.x + w * 0.34f, p0.y + h * 0.08f), b(p0.x + w * 0.66f, p0.y + h * 0.92f);
    const ImU32 line = has ? IM_COL32(235, 235, 235, 255) : IM_COL32(150, 150, 150, 150);
    dl->AddRectFilled(a, b, IM_COL32(20, 30, 20, has ? 200 : 110), 3.f);
    if (has && c.capacity > 0) {
        const float frac = std::clamp(float(c.ammo) / float(std::max(c.capacity, 1)), 0.f, 1.f);
        const float top = b.y - (b.y - a.y) * frac;
        dl->AddRectFilled(ImVec2(a.x + 1.f, top), ImVec2(b.x - 1.f, b.y - 1.f), IM_COL32(60, 210, 80, 255), 2.f);
    }
    dl->AddRect(a, b, line, 3.f, 0, 2.f);
    if (has && c.capacity >= 32)
        dl->AddRect(ImVec2(a.x - 3.f, a.y - 3.f), ImVec2(b.x + 3.f, b.y + 3.f), IM_COL32(120, 230, 140, 255), 4.f);
}

void draw_cell(const Cell& cell, const Save& save, const Style& st, ImVec2 at) {
    ImGui::SetCursorPos(at);
    ImGui::Dummy(ImVec2(st.icon, st.icon));
    if (cell.kind == K::Blank) {
        if (cell.label) { // a dungeon without that item still names its column
            const ImVec2 q0 = ImGui::GetItemRectMin(), q1 = ImGui::GetItemRectMax();
            const char* name = kDungeons[cell.a].shortName;
            const float w = text_width(st.text, name);
            shadow_text(ImGui::GetWindowDrawList(), ImVec2(q0.x + (q1.x - q0.x - w) * 0.5f, q0.y + 1.f), st.text, kWhite, name);
        }
        return;
    }
    const Look l = look_of(cell, save, st.opt);
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!l.tip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", l.tip.c_str());
    if (l.magicBar) {
        draw_magic(dl, p0, p1, l.has, l.count);
    } else {
        ImTextureID tex = 0;
        if (l.file != nullptr)
            tex = item_found::IconFile(l.file, !l.has);
        else if (l.item >= 0)
            tex = l.has ? item_found::Icon(l.item) : item_found::IconFaded(l.item);
        if (tex != 0) {
            // A song is the baton lit in its own color; the rest as the game draws them.
            const ImU32 tint = l.has ? l.tint : kWhite;
            dl->AddImage(tex, p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint);
        } else {
            // The disc is not open yet (or the icon is missing): an outline in its place.
            dl->AddRect(p0, p1, IM_COL32(200, 200, 200, l.has ? 200 : 90), 4.f);
        }
    }
    if (l.abbrev != nullptr) {
        const float w = text_width(st.text, l.abbrev);
        shadow_text(dl, ImVec2(p0.x + (p1.x - p0.x - w) * 0.5f, p0.y + (p1.y - p0.y) * 0.5f - st.text * 0.5f), st.text,
                    l.has ? kWhite : kGray, l.abbrev);
    }
    if (cell.label) {
        const char* name = kDungeons[cell.a].shortName;
        const float w = text_width(st.text, name);
        shadow_text(dl, ImVec2(p0.x + (p1.x - p0.x - w) * 0.5f, p0.y + 1.f), st.text, kWhite, name);
    }
    draw_count(dl, p0, p1, st, l.count);
}

void draw_grid(const Cells& cells, int columns, const Save& save, const Style& st) {
    const ImVec2 origin = ImGui::GetCursorPos();
    const float step = st.icon + st.spacing;
    for (size_t i = 0; i < cells.size(); ++i) {
        const int row = int(i) / columns, col = int(i) % columns;
        draw_cell(cells[i], save, st, ImVec2(origin.x + float(col) * step, origin.y + float(row) * step));
    }
    const int rows = (int(cells.size()) + columns - 1) / columns;
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + float(rows) * step));
}

void draw_blocks(const Blocks& blocks, const Save& save, const Style& st) {
    for (const Block& b : blocks)
        draw_grid(b.cells, b.columns, save, st);
}

// --- Windows ----------------------------------------------------------------------

int g_window_index;

// Begins one tracker window; the caller draws, then calls end_window.
void begin_window(const char* name, float scale, float fontScale) {
    const int type = CVarGetInteger(TRACKER_CVAR("WindowType"), WINDOW_FLOATING);
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (type == WINDOW_FLOATING) {
        flags |= ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;
        if (!CVarGetInteger(TRACKER_CVAR("Draggable"), 0))
            flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    }
    const CVarColor bg = CVarGetColor(TRACKER_CVAR("BgColor"), CVarColor{0, 0, 0, 0});
    // The position is kept as a fraction of the screen, one pair of CVars for each window.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    char kx[64], ky[64];
    std::snprintf(kx, sizeof kx, TRACKER_CVAR("Position.%d.X"), g_window_index);
    std::snprintf(ky, sizeof ky, TRACKER_CVAR("Position.%d.Y"), g_window_index);
    const float fx = CVarGetFloat(kx, 0.015f), fy = CVarGetFloat(ky, 0.2f + 0.1f * float(g_window_index));
    // Placed again whenever the screen size changes: the window starts at 4:3 and is resized to the saved size a
    // moment later, and a place set only once stayed where the 4:3 fraction put it.
    static ImVec2 placed_for[8];
    const int slot = std::clamp(g_window_index, 0, 7);
    const bool resized = placed_for[slot].x != display.x || placed_for[slot].y != display.y;
    placed_for[slot] = display;
    ImGui::SetNextWindowPos(ImVec2(fx * display.x, fy * display.y), resized ? ImGuiCond_Always : ImGuiCond_Once);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg.r / 255.f, bg.g / 255.f, bg.b / 255.f, bg.a / 255.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f * scale);
    ImGui::Begin(name, nullptr, flags);
    ImGui::SetWindowFontScale(fontScale);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImVec2 pos = ImGui::GetWindowPos();
        if (display.x > 0 && display.y > 0 &&
            (std::fabs(pos.x / display.x - fx) > 1e-4f || std::fabs(pos.y / display.y - fy) > 1e-4f)) {
            CVarSetFloat(kx, pos.x / display.x);
            CVarSetFloat(ky, pos.y / display.y);
            CVarSaveNextFrame();
        }
    }
    ++g_window_index;
}

void end_window() {
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

bool combo_held() {
    unsigned char raw[2];
    if (!bluewake_dev_read(kPadHold, raw, 2))
        return false;
    const unsigned held = unsigned(raw[0]) << 8 | raw[1];
    const int b1 = std::clamp(CVarGetInteger(TRACKER_CVAR("ComboButton1"), kComboL), 0, 11);
    const int b2 = std::clamp(CVarGetInteger(TRACKER_CVAR("ComboButton2"), kComboR), 0, 11);
    const bool both = (held & kComboButtons[b1].mask) != 0 && (held & kComboButtons[b2].mask) != 0;
    static int last = -1;
    if (int(both) != last) {
        last = int(both);
        std::fprintf(stderr, "[tracker] combo %s (held 0x%04X)\n", both ? "held" : "released", held);
    }
    return both;
}

int section_type(const char* name, int fallback) {
    char key[64];
    std::snprintf(key, sizeof key, TRACKER_CVAR("DisplayType.%s"), name);
    return std::clamp(CVarGetInteger(key, fallback), int(SECTION_HIDDEN), int(SECTION_SEPARATE));
}

} // namespace

void Toggle() {
    CVarSetInteger(kWindowOpen, CVarGetInteger(kWindowOpen, 0) ? 0 : 1);
    CVarSaveNextFrame();
}

void Frame(float scale, float fontScale) {
    if (!CVarGetInteger(kWindowOpen, 0))
        return;
    // In game only: not on the title or the file select, nor before a save is in play.
    const BluewakeLocation where = bluewake_game_location();
    if (where.scene_proc != BLUEWAKE_SCENE_PLAY || where.stage[0] == '\0') {
        g_track_slot = -1;
        return;
    }
    static Save save;
    if (!bluewake_dev_read(kInfo, save.b, kSnapshot))
        return;
    if (where.data_num != g_track_slot) {
        g_track_slot = where.data_num; // another file: the keys picked up start again
        for (KeyTrack& t : g_track)
            t = KeyTrack{};
    }
    const int table = current_stage_table();
    if (table != g_current_table) {
        std::fprintf(stderr, "[tracker] stage %s, save table %d\n", where.stage, table);
        g_current_table = table;
    }

    const int type = CVarGetInteger(TRACKER_CVAR("WindowType"), WINDOW_FLOATING);
    unsigned char pause = 0;
    const bool paused = bluewake_dev_read(kMenuPause, &pause, 1) && pause != 0;
    static int last_paused = -1;
    if (int(paused) != last_paused) {
        last_paused = int(paused);
        std::fprintf(stderr, "[tracker] pause menu %s\n", paused ? "up" : "down");
    }
    if (type == WINDOW_FLOATING) {
        if (CVarGetInteger(TRACKER_CVAR("ShowOnlyPaused"), 0) && !paused)
            return;
        if (CVarGetInteger(TRACKER_CVAR("DisplayType.Main"), DISPLAY_ALWAYS) == DISPLAY_COMBO_BUTTON && !combo_held())
            return;
    }

    Style st;
    st.icon = float(std::clamp(CVarGetInteger(TRACKER_CVAR("IconSize"), 36), 25, 128)) * scale;
    st.spacing = float(std::clamp(CVarGetInteger(TRACKER_CVAR("IconSpacing"), 12), -5, 50)) * scale;
    st.text = float(std::clamp(CVarGetInteger(TRACKER_CVAR("TextSize"), 13), 1, 30)) * scale;
    st.type = type;
    st.opt.countType = std::clamp(CVarGetInteger(TRACKER_CVAR("ItemCountType"), NUMBER_CURRENT_CAPACITY_ONLY), 0, 4);
    st.opt.keyType = std::clamp(CVarGetInteger(TRACKER_CVAR("KeyCounts"), KEYS_COLLECTED_MAX), 0, 2);
    st.opt.alignLeft = CVarGetInteger(TRACKER_CVAR("ItemCountAlignLeft"), 0) != 0;

    const int inventory = section_type("Inventory", SECTION_MAIN_WINDOW);
    const int equipment = section_type("Equipment", SECTION_MAIN_WINDOW);
    const int capacity = section_type("Capacity", SECTION_MAIN_WINDOW);
    const int rewards = section_type("DungeonRewards", SECTION_MAIN_WINDOW);
    const int songs = section_type("Songs", SECTION_MAIN_WINDOW);
    const int dungeons = section_type("DungeonItems", SECTION_MAIN_WINDOW);
    const bool maps = CVarGetInteger(TRACKER_CVAR("DungeonItems.DisplayMaps"), 1) != 0;
    const bool horizontal = CVarGetInteger(TRACKER_CVAR("DungeonItems.Layout"), 1) != 0;

    auto inventory_blocks = [&] { return Blocks{{inventory_cells(), 7}}; };
    auto equipment_blocks = [&] { return Blocks{{equipment_cells(), bluewake_bunny_hood_enabled() ? 6 : 5}}; };
    auto capacity_blocks = [&] { return Blocks{{capacity_cells(), 6}}; };
    auto reward_blocks = [&] { return Blocks{{pearl_cells(), 3}, {shard_cells(), 8}}; };
    auto song_blocks = [&] { return Blocks{{song_cells(), 6}}; };
    auto dungeon_blocks = [&](int columns) { return Blocks{{dungeon_cells(columns, maps), columns}}; };
    Blocks main;
    auto add_main = [&](Blocks blocks) { main.insert(main.end(), blocks.begin(), blocks.end()); };
    if (inventory == SECTION_MAIN_WINDOW)
        add_main(inventory_blocks());
    if (equipment == SECTION_MAIN_WINDOW)
        add_main(equipment_blocks());
    if (capacity == SECTION_MAIN_WINDOW)
        add_main(capacity_blocks());
    if (rewards == SECTION_MAIN_WINDOW)
        add_main(reward_blocks());
    if (songs == SECTION_MAIN_WINDOW)
        add_main(song_blocks());
    if (dungeons == SECTION_MAIN_WINDOW)
        add_main(dungeon_blocks(kDungeonCount));

    g_window_index = 0;
    if (!main.empty()) {
        begin_window("Item Tracker", scale, fontScale);
        draw_blocks(main, save, st);
        end_window();
    } else {
        ++g_window_index;
    }
    auto separate = [&](int mode, const char* title, const Blocks& blocks) {
        if (mode == SECTION_SEPARATE) {
            begin_window(title, scale, fontScale);
            draw_blocks(blocks, save, st);
            end_window();
        } else {
            ++g_window_index;
        }
    };
    separate(inventory, "Inventory Items Tracker", inventory_blocks());
    separate(equipment, "Equipment Items Tracker", equipment_blocks());
    separate(capacity, "Capacity Items Tracker", capacity_blocks());
    separate(rewards, "Dungeon Rewards Tracker", reward_blocks());
    separate(songs, "Songs Tracker", song_blocks());
    separate(dungeons, "Dungeon Items Tracker", dungeon_blocks(horizontal ? kDungeonCount : 3));
}

void BuildMenu(shipmenu::Menu& m) {
    const char* H = "Randomizer";
    const char* S = "Item Tracker";
    m.AddSidebar(H, S, 2);
    m.AddWidget(H, S, 0, "Item Tracker", WidgetType::SeparatorText);
    m.AddWidget(H, S, 0, "Toggle Item Tracker", WidgetType::WindowButton)
        .CVar(kWindowOpen)
        .HideInSearch()
        .Tooltip("Toggles the Item Tracker (hotkey F7). It shows while a game is in play, not on the title screen "
                 "or the file select.");
    m.AddWidget(H, S, 0, "Item Tracker Settings", WidgetType::SeparatorText);
    auto floating = [](WidgetInfo& w) {
        w.hidden = CVarGetInteger(TRACKER_CVAR("WindowType"), WINDOW_FLOATING) != WINDOW_FLOATING;
    };
    m.AddWidget(H, S, 0, "Window Type", WidgetType::CVarCombobox)
        .CVar(TRACKER_CVAR("WindowType"))
        .Items({"Floating", "Window"})
        .DefaultValue(WINDOW_FLOATING)
        .Tooltip("Floating: a borderless overlay on the screen. Window: an ordinary movable window with a title bar.");
    m.AddWidget(H, S, 0, "Enable Dragging", WidgetType::CVarCheckbox)
        .CVar(TRACKER_CVAR("Draggable"))
        .PreFunc(floating)
        .Tooltip("Lets the mouse move the floating tracker; the position is kept.");
    m.AddWidget(H, S, 0, "Only Enable While Paused", WidgetType::CVarCheckbox)
        .CVar(TRACKER_CVAR("ShowOnlyPaused"))
        .PreFunc(floating)
        .Tooltip("Shows the floating tracker only while the pause menu is open.");
    m.AddWidget(H, S, 0, "Display Mode", WidgetType::CVarCombobox)
        .CVar(TRACKER_CVAR("DisplayType.Main"))
        .Items({"Always", "Combo Button Hold"})
        .DefaultValue(DISPLAY_ALWAYS)
        .PreFunc(floating);
    std::vector<std::string> buttons;
    for (const ComboButton& b : kComboButtons)
        buttons.emplace_back(b.label);
    auto combo = [](WidgetInfo& w) {
        w.hidden = CVarGetInteger(TRACKER_CVAR("WindowType"), WINDOW_FLOATING) != WINDOW_FLOATING ||
                   CVarGetInteger(TRACKER_CVAR("DisplayType.Main"), DISPLAY_ALWAYS) != DISPLAY_COMBO_BUTTON;
    };
    m.AddWidget(H, S, 0, "Combo Button 1", WidgetType::CVarCombobox)
        .CVar(TRACKER_CVAR("ComboButton1"))
        .Items(buttons)
        .DefaultValue(kComboL)
        .PreFunc(combo);
    m.AddWidget(H, S, 0, "Combo Button 2", WidgetType::CVarCombobox)
        .CVar(TRACKER_CVAR("ComboButton2"))
        .Items(buttons)
        .DefaultValue(kComboR)
        .PreFunc(combo);
    m.AddWidget(H, S, 0, "Background Color", WidgetType::CVarColorPicker)
        .CVar(TRACKER_CVAR("BgColor"))
        .DefaultColor(ImVec4(0.f, 0.f, 0.f, 0.f))
        .UseAlpha()
        .Tooltip("The color and opacity behind the icons.");
    m.AddWidget(H, S, 0, "Icon size : %dpx", WidgetType::CVarSliderInt)
        .CVar(TRACKER_CVAR("IconSize"))
        .Range(25, 128)
        .DefaultValue(36);
    m.AddWidget(H, S, 0, "Icon margins : %dpx", WidgetType::CVarSliderInt)
        .CVar(TRACKER_CVAR("IconSpacing"))
        .Range(-5, 50)
        .DefaultValue(12);
    m.AddWidget(H, S, 0, "Text size : %dpx", WidgetType::CVarSliderInt)
        .CVar(TRACKER_CVAR("TextSize"))
        .Range(1, 30)
        .DefaultValue(13);
    m.AddWidget(H, S, 0, "Ammo/Capacity Tracking", WidgetType::CVarCombobox)
        .CVar(TRACKER_CVAR("ItemCountType"))
        .Items({"No Numbers", "Current Capacity", "Current Ammo", "Current Capacity / Max Capacity",
                "Current Ammo / Current Capacity"})
        .DefaultValue(NUMBER_CURRENT_CAPACITY_ONLY)
        .Tooltip("Customize what the numbers under each item are tracking.\n\nNote: items without capacity upgrades "
                 "will track ammo even in capacity mode");
    m.AddWidget(H, S, 0, "Align count to left side", WidgetType::CVarCheckbox)
        .CVar(TRACKER_CVAR("ItemCountAlignLeft"))
        .PreFunc([](WidgetInfo& w) {
            const int t = CVarGetInteger(TRACKER_CVAR("ItemCountType"), NUMBER_CURRENT_CAPACITY_ONLY);
            w.hidden = t != NUMBER_CURRENT_CAPACITY_ONLY && t != NUMBER_CURRENT_AMMO_ONLY;
        });
    m.AddWidget(H, S, 0, "Key Count Tracking", WidgetType::CVarCombobox)
        .CVar(TRACKER_CVAR("KeyCounts"))
        .Items({"Collected / Max", "Current / Collected / Max", "Current / Max"})
        .DefaultValue(KEYS_COLLECTED_MAX)
        .Tooltip("Customize what numbers are shown for key tracking.\n\nThe save does not count the keys picked up, "
                 "so Collected counts them while the tracker is open.");

    m.AddWidget(H, S, 1, "Section Settings", WidgetType::SeparatorText);
    const std::vector<std::string> types = {"Hidden", "Main Window", "Separate"};
    struct Section {
        const char* label;
        const char* key;
        const char* tooltip;
    };
    const Section sections[] = {
        {"Inventory", "Inventory", "The item screen: telescope to Skull Hammer, three rows of seven."},
        {"Equipment", "Equipment", "The sword, shield, Power Bracelets, Hero's Charm, Pirate's Charm and Bunny Hood."},
        {"Capacity", "Capacity", "The wallet, bomb bag, quiver, magic meter, heart containers and pieces of heart."},
        {"Dungeon Rewards", "DungeonRewards", "The three pearls and the eight Triforce shards."},
        {"Songs", "Songs", "The Wind Waker's six songs."},
        {"Dungeon Items", "DungeonItems", "Each dungeon's small keys, big key, map and compass."},
    };
    for (const Section& s : sections) {
        std::string cvar = std::string(TRACKER_CVAR("DisplayType.")) + s.key;
        m.AddWidget(H, S, 1, s.label, WidgetType::CVarCombobox)
            .CVar(cvar)
            .Items(types)
            .DefaultValue(SECTION_MAIN_WINDOW)
            .Tooltip(s.tooltip);
    }
    m.AddWidget(H, S, 1, "Horizontal display", WidgetType::CVarCheckbox)
        .CVar(TRACKER_CVAR("DungeonItems.Layout"))
        .DefaultValue(1)
        .PreFunc([](WidgetInfo& w) {
            w.hidden = CVarGetInteger(TRACKER_CVAR("DisplayType.DungeonItems"), SECTION_MAIN_WINDOW) != SECTION_SEPARATE;
        })
        .Tooltip("All the dungeons side by side; off, three to a row.");
    m.AddWidget(H, S, 1, "Maps and compasses", WidgetType::CVarCheckbox)
        .CVar(TRACKER_CVAR("DungeonItems.DisplayMaps"))
        .DefaultValue(1)
        .PreFunc([](WidgetInfo& w) {
            w.hidden = CVarGetInteger(TRACKER_CVAR("DisplayType.DungeonItems"), SECTION_MAIN_WINDOW) == SECTION_HIDDEN;
        });
}

} // namespace itemtracker
