// The Dev Tools windows. See dev_tools.h.
#include "sea_chart.h"
#include "dev_windows.h"
#include "cosmetics_editor.h"
#include "item_found.h"
#include "ship_menu.h"
#include "ship/cvar.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
bool bluewake_bunny_hood_enabled(void);
bool bluewake_bunny_hood_give(void);
unsigned long long bluewake_host_retrace_count(void);
bool bluewake_dev_spawn(const char* name, unsigned params, float distance);
bool bluewake_dev_spawn_at(const char* name, unsigned params, const float* pos);
#include "dev_tools.h"  // the host's (runtime/host/src)
#include "game_state.h"
}

namespace aurora::gfx {
float calculate_fps() noexcept;
float calculate_game_fps() noexcept;
}  // namespace aurora::gfx

#include "ww_names.inc"

namespace devtools {
namespace {

using shipmenu::WidgetInfo;
using shipmenu::WidgetType;

// g_dComIfG_gameInfo (dSv_info_c), the tww decomp's d_save.h.
constexpr unsigned kInfo = 0x803C4C08u;
constexpr unsigned kInfoBytes = 0x1260;   // through mTmp (+0x1158, 0x100 bytes)
constexpr unsigned kMemory = 0x380;       // dSv_save_c::mMemory[16], 0x24 each
constexpr unsigned kEvent = 0x624;        // dSv_save_c::mEvent (256 bytes)
constexpr unsigned kCurrentMemory = 0x778; // dSv_info_c::mMemory (the stage Link is in)
constexpr unsigned kRestart = 0x1128;    // dSv_info_c::mRestart
constexpr unsigned kTmpEvent = 0x1158;    // dSv_info_c::mTmp
constexpr unsigned kPlayerPtr = 0x803CA754u; // play.mpPlayerPtr[0]
constexpr unsigned kNextStage = 0x803C9D48u; // play.mNextStage

uint8_t g_snap[kInfoBytes];
bool g_live;  // the snapshot holds the game's memory

uint32_t get(unsigned offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; ++i)
        v = (v << 8) | g_snap[offset + i];
    return v;
}

void set(unsigned offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; ++i)
        g_snap[offset + i] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
    bluewake_dev_write(kInfo + offset, value, size);
}

const char* item_name(int id) {
    for (const auto& item : kItemNames)
        if (item.id == id)
            return item.name;
    return nullptr;
}

std::string item_label(int id) {
    char text[96];
    if (id == 0xFF)
        return "Empty (0xFF)";
    const char* name = item_name(id);
    std::snprintf(text, sizeof text, "%s (0x%02X)", name != nullptr ? name : "Unknown", id);
    return text;
}

const char* proc_name(int id) {
    for (const auto& proc : kProcNames)
        if (proc.id == id)
            return proc.name;
    return nullptr;
}

// --- widgets over the snapshot ----------------------------------------------------

bool slider(const char* label, unsigned offset, unsigned size, int lo, int hi, float scale) {
    int value = static_cast<int>(size == 2 ? static_cast<int16_t>(get(offset, 2)) : get(offset, size));
    if (size == 2)
        value = static_cast<int>(get(offset, 2));
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::SliderInt(label, &value, lo, hi)) {
        set(offset, size, static_cast<uint32_t>(value));
        return true;
    }
    return false;
}

bool check_bit(const char* label, unsigned offset, unsigned bit) {
    bool on = (g_snap[offset] >> bit) & 1;
    if (ImGui::Checkbox(label, &on)) {
        const unsigned value = on ? (g_snap[offset] | (1u << bit)) : (g_snap[offset] & ~(1u << bit));
        set(offset, 1, value);
        return true;
    }
    return false;
}

// Bit n of an array of big-endian 32-bit words at offset.
bool word_bit(unsigned offset, unsigned n) { return (get(offset + 4 * (n / 32), 4) >> (n % 32)) & 1; }

void set_word_bit(unsigned offset, unsigned n, bool on) {
    const unsigned at = offset + 4 * (n / 32);
    const uint32_t word = get(at, 4);
    set(at, 4, on ? (word | (1u << (n % 32))) : (word & ~(1u << (n % 32))));
}

void bit_grid(const char* title, unsigned offset, unsigned bits, float scale) {
    ImGui::TextUnformatted(title);
    for (unsigned n = 0; n < bits; ++n) {
        if (n % 16 != 0)
            ImGui::SameLine();
        ImGui::PushID(static_cast<int>(n));
        bool on = word_bit(offset, n);
        char label[8];
        std::snprintf(label, sizeof label, "##%u", n);
        if (ImGui::Checkbox(label, &on))
            set_word_bit(offset, n, on);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s bit %u (0x%02X)", title, n, n);
        ImGui::PopID();
    }
    (void)scale;
}

bool game_running() {
    if (!g_live)
        ImGui::TextDisabled("The game is not running yet.");
    return g_live;
}

// --- Save Editor ------------------------------------------------------------------

struct Slot {
    const char* name;
    std::vector<int> items;
};
const Slot kSlots[21] = {
    {"Telescope", {0x20}}, {"Sail", {0x78}}, {"Wind Waker", {0x22}}, {"Grappling Hook", {0x25}},
    {"Spoils Bag", {0x24}}, {"Boomerang", {0x2D}}, {"Deku Leaf", {0x34}}, {"Tingle Tuner", {0x21}},
    {"Picto Box", {0x23, 0x26}}, {"Iron Boots", {0x29}}, {"Magic Armor", {0x2A}}, {"Bait Bag", {0x2C, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88}},
    {"Bow", {0x27, 0x35, 0x36}}, {"Bombs", {0x31}}, {"Bottle 1", {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x59, 0x5A, 0x5B}}, {"Bottle 2", {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x59, 0x5A, 0x5B}},
    {"Bottle 3", {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x59, 0x5A, 0x5B}}, {"Bottle 4", {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x59, 0x5A, 0x5B}}, {"Delivery Bag", {0x30}}, {"Hookshot", {0x2F}},
    {"Skull Hammer", {0x33}},
};

void save_player(float scale) {
    ImGui::SeparatorText("Status");
    int hearts = static_cast<int>(get(0x00, 2)) / 4;
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::SliderInt("Max Hearts", &hearts, 3, 20))
        set(0x00, 2, static_cast<uint32_t>(hearts * 4));
    slider("Health (quarter hearts)", 0x02, 2, 0, 80, scale);
    slider("Rupees", 0x04, 2, 0, 9999, scale);
    const char* wallets[] = {"200 (Wallet)", "1000 (Big Wallet)", "5000 (Giant Wallet)"};
    int wallet = std::min<int>(get(0x12, 1), 2);
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::Combo("Wallet", &wallet, wallets, 3))
        set(0x12, 1, static_cast<uint32_t>(wallet));
    slider("Max Magic", 0x13, 1, 0, 32, scale);
    slider("Magic", 0x14, 1, 0, 32, scale);
    slider("Death Count", 0x148 + 0x10, 2, 0, 9999, scale);
    float time = 0;
    {
        const uint32_t bits = get(0x18 + 0xC, 4);
        std::memcpy(&time, &bits, sizeof time);
    }
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::SliderFloat("Time of Day (degrees)", &time, 0.0f, 360.0f, "%.0f")) {
        uint32_t bits;
        std::memcpy(&bits, &time, sizeof bits);
        set(0x18 + 0xC, 4, bits);
    }
    ImGui::SeparatorText("Name");
    char name[17];
    std::memcpy(name, g_snap + 0x148 + 0x14, 16);
    name[16] = '\0';
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::InputText("Player Name", name, sizeof name)) {
        for (unsigned i = 0; i < 16; ++i)
            g_snap[0x148 + 0x14 + i] = static_cast<uint8_t>(name[i]);
        bluewake_dev_write_bytes(kInfo + 0x148 + 0x14, name, 17);
    }
    ImGui::SeparatorText("Equipment");
    struct Equip {
        const char* label;
        unsigned offset;
        std::vector<int> choices;
    };
    const Equip equips[] = {
        {"Sword", 0x0E, {0x38, 0x39, 0x3A, 0x3E}},
        {"Shield", 0x0F, {0x3B, 0x3C}},
        {"Bracelet", 0x10, {0x28}},
    };
    for (const Equip& e : equips) {
        const int current = static_cast<int>(get(e.offset, 1));
        ImGui::SetNextItemWidth(260 * scale);
        if (ImGui::BeginCombo(e.label, item_label(current).c_str())) {
            if (ImGui::Selectable("Empty (0xFF)", current == 0xFF))
                set(e.offset, 1, 0xFF);
            for (int id : e.choices)
                if (ImGui::Selectable(item_label(id).c_str(), current == id))
                    set(e.offset, 1, static_cast<uint32_t>(id));
            ImGui::EndCombo();
        }
    }
}

// The pause menu's item screen, as a grid: seven columns, the slots in the game's order, each a picture of its item (faded
// while the slot is empty). A click opens the choice of what that slot can hold, with their pictures.
void inventory_grid(float scale) {
    const float cell = 46 * scale;
    for (unsigned slot = 0; slot < 21; ++slot) {
        if (slot % 7 != 0)
            ImGui::SameLine();
        const int current = static_cast<int>(get(0x3C + slot, 1));
        const bool has = current != 0xFF;
        const int shown = has ? current : (kSlots[slot].items.empty() ? 0x50 : kSlots[slot].items[0]);
        ImGui::PushID(static_cast<int>(slot));
        const ImTextureID tex = has ? item_found::Icon(shown) : item_found::IconFaded(shown);
        const bool clicked = tex != 0 ? ImGui::ImageButton("##slot", tex, ImVec2(cell, cell))
                                      : ImGui::Button(has ? "?" : "-", ImVec2(cell + 8, cell + 8));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: %s", kSlots[slot].name, has ? item_label(current).c_str() : "empty");
        if (clicked)
            ImGui::OpenPopup("choose");
        if (ImGui::BeginPopup("choose")) {
            ImGui::TextDisabled("%s", kSlots[slot].name);
            if (ImGui::Selectable("Empty (0xFF)", current == 0xFF))
                set(0x3C + slot, 1, 0xFF);
            for (int id : kSlots[slot].items) {
                if (const ImTextureID icon = item_found::Icon(id)) {
                    ImGui::Image(icon, ImVec2(26 * scale, 26 * scale));
                    ImGui::SameLine();
                }
                if (ImGui::Selectable(item_label(id).c_str(), current == id))
                    set(0x3C + slot, 1, static_cast<uint32_t>(id));
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

void save_inventory(float scale) {
    static bool raw = false;
    ImGui::Checkbox("Raw values (debug)", &raw);
    ImGui::SeparatorText("Items");
    if (!raw)
        inventory_grid(scale);
    for (unsigned slot = 0; raw && slot < 21; ++slot) {
        const int current = static_cast<int>(get(0x3C + slot, 1));
        ImGui::PushID(static_cast<int>(slot));
        ImGui::SetNextItemWidth(260 * scale);
        if (ImGui::BeginCombo(kSlots[slot].name, item_label(current).c_str())) {
            if (ImGui::Selectable("Empty (0xFF)", current == 0xFF))
                set(0x3C + slot, 1, 0xFF);
            for (int id : kSlots[slot].items)
                if (ImGui::Selectable(item_label(id).c_str(), current == id))
                    set(0x3C + slot, 1, static_cast<uint32_t>(id));
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        int custom = current;
        ImGui::SetNextItemWidth(90 * scale);
        if (ImGui::InputInt("##id", &custom, 0, 0, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue))
            set(0x3C + slot, 1, static_cast<uint32_t>(custom) & 0xFFu);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Any item number, in hex, then Enter.");
        ImGui::PopID();
    }
    ImGui::SeparatorText("Counts");
    slider("Arrows", 0x66 + 3, 1, 0, 99, scale);
    slider("Max Arrows", 0x6E + 1, 1, 0, 99, scale);
    slider("Bombs", 0x66 + 4, 1, 0, 99, scale);
    slider("Max Bombs", 0x6E + 2, 1, 0, 99, scale);
    slider("Pictures", 0x66 + 2, 1, 0, 3, scale);
}

void save_collectibles() {
    ImGui::SeparatorText("Songs");
    const char* songs[] = {"Wind's Requiem", "Ballad of Gales", "Command Melody", "Earth God's Lyric",
                           "Wind God's Aria", "Song of Passing"};
    for (unsigned i = 0; i < 6; ++i)
        check_bit(songs[i], 0xB4 + 9, i);
    ImGui::SeparatorText("Pearls");
    const char* pearls[] = {"Nayru's Pearl", "Din's Pearl", "Farore's Pearl"};
    for (unsigned i = 0; i < 3; ++i)
        check_bit(pearls[i], 0xB4 + 0xB, i);
    ImGui::SeparatorText("Triforce Shards");
    for (unsigned i = 0; i < 8; ++i) {
        char label[32];
        std::snprintf(label, sizeof label, "Triforce Shard %u", i + 1);
        check_bit(label, 0xB4 + 0xA, i);
        if (i % 2 == 0)
            ImGui::SameLine(ImGui::GetCursorStartPos().x + ImGui::GetFontSize() * 14.0f); // scales with the menu's font, so no label runs into the next box
    }
    ImGui::SeparatorText("Swords and Shields (collected)");
    check_bit("Hero's Sword", 0xB4 + 0, 0);
    check_bit("Master Sword (unpowered)", 0xB4 + 0, 1);
    check_bit("Master Sword (half power)", 0xB4 + 0, 2);
    check_bit("Master Sword (full power)", 0xB4 + 0, 3);
    check_bit("Hero's Shield", 0xB4 + 1, 0);
    check_bit("Mirror Shield", 0xB4 + 1, 1);
    // The charms share one byte (dSv_player_collect_c::mCollect[4]): the Hero's Charm owns bits 0 and 1, the
    // Bunny Hood bits 2 and 3, and bit 4 is the slot's choice (docs/fork/bunny-hood.md).
    if (!bluewake_bunny_hood_enabled())
        return; // experimental (gExperimental.BunnyHood)
    ImGui::SeparatorText("Charms and the Bunny Hood");
    check_bit("Pirate's Charm", 0xB4 + 3, 0);
    check_bit("Hero's Charm", 0xB4 + 4, 0);
    ImGui::SameLine(ImGui::GetCursorStartPos().x + ImGui::GetFontSize() * 14.0f); // scales with the menu's font, so no label runs into the next box
    check_bit("Hero's Charm worn", 0xB4 + 4, 1);
    // The way the game gives an item: Link holds it up with its text; the item function then sets the bit.
    if (ImGui::Button("Give Bunny Hood"))
        bluewake_bunny_hood_give();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Link picks the hood up as an item: the get-item pose with the held model and the text.");
    ImGui::PushID("bunnyhood");
    check_bit("Bunny Hood owned", 0xB4 + 4, 2);
    ImGui::SameLine(ImGui::GetCursorStartPos().x + ImGui::GetFontSize() * 14.0f); // scales with the menu's font, so no label runs into the next box
    check_bit("Bunny Hood worn", 0xB4 + 4, 3);
    ImGui::PopID();
    // Testing: BLUEWAKE_TEST_SAVE_TAB=collect keeps the tab scrolled to its end (the hood rows), for a capture.
    static const char* const test_tab = std::getenv("BLUEWAKE_TEST_SAVE_TAB");
    if (test_tab != nullptr && std::strcmp(test_tab, "collect") == 0)
        ImGui::SetScrollHereY(1.0f);
}

void embed_save_editor(float scale) {
    if (game_running() && ImGui::BeginTabBar("##save")) {
        if (ImGui::BeginTabItem("Player")) {
            save_player(scale);
            ImGui::EndTabItem();
        }
        // Testing: BLUEWAKE_TEST_SAVE_TAB=1 opens the Inventory tab (the scenarios cannot click).
        // (BLUEWAKE_TEST_SAVE_TAB=collect opens the Collectibles tab instead.)
        static const char* const test_tab = std::getenv("BLUEWAKE_TEST_SAVE_TAB");
        static const bool inventory_first = test_tab != nullptr && std::strcmp(test_tab, "collect") != 0;
        static const bool collect_first = test_tab != nullptr && std::strcmp(test_tab, "collect") == 0;
        static bool inventory_forced, collect_forced;
        const bool force = inventory_first && !inventory_forced;
        if (ImGui::BeginTabItem("Inventory", nullptr, force ? ImGuiTabItemFlags_SetSelected : 0)) {
            inventory_forced = true;
            save_inventory(scale);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Collectibles", nullptr, collect_first && !collect_forced ? ImGuiTabItemFlags_SetSelected : 0)) {
            collect_forced = true;
            save_collectibles();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void draw_save_editor(float scale, float fontScale) {
    shipmenu::PopoutWindow("Save Editor", "gOpenWindows.SaveEditor", ImVec2(560 * scale, 620 * scale), 0.97f, fontScale,
                           [&] { embed_save_editor(scale); });
}

// --- Flag Viewer ------------------------------------------------------------------

const char* const kStageTables[16] = {"Sea", "Sea (2)", "Forsaken Fortress", "Dragon Roost Cavern", "Forbidden Woods",
                                      "Tower of the Gods", "Earth Temple", "Wind Temple", "Ganon's Tower", "Hyrule",
                                      "Ship", "Misc (interiors)", "Sub-dungeon", "Sub-dungeon (new)", "Blue Chu Jelly",
                                      "Test"};

void event_flags(unsigned base, const char* kind, float scale) {
    static char filter[16];
    ImGui::SetNextItemWidth(160 * scale);
    ImGui::InputTextWithHint("##filter", "Flag id in hex (0x0A04)...", filter, sizeof filter);
    unsigned wanted = 0xFFFFFFFFu;
    if (filter[0] != '\0')
        wanted = static_cast<unsigned>(std::strtoul(filter, nullptr, 16));
    ImGui::TextDisabled("A flag's id is its byte (high) and the bit mask in it (low), as the game names them.");
    ImGuiListClipper clipper;
    clipper.Begin(256);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            if (wanted != 0xFFFFFFFFu && (wanted >> 8) != static_cast<unsigned>(row))
                continue;
            ImGui::PushID(row);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("0x%02X", row);
            for (int bit = 7; bit >= 0; --bit) {
                ImGui::SameLine();
                char label[8];
                std::snprintf(label, sizeof label, "##%d", bit);
                bool on = (g_snap[base + row] >> bit) & 1;
                if (ImGui::Checkbox(label, &on)) {
                    const unsigned value = on ? (g_snap[base + row] | (1u << bit)) : (g_snap[base + row] & ~(1u << bit));
                    set(base + row, 1, value);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s flag 0x%02X%02X", kind, row, 1 << bit);
            }
            ImGui::PopID();
        }
    }
}

void stage_memory(float scale) {
    static int table = 0;  // 0-15 a table; 16 the stage Link is in
    std::string preview = table == 16 ? "Current stage" : kStageTables[table];
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::BeginCombo("Stage", preview.c_str())) {
        if (ImGui::Selectable("Current stage", table == 16))
            table = 16;
        for (int i = 0; i < 16; ++i)
            if (ImGui::Selectable(kStageTables[i], table == i))
                table = i;
        ImGui::EndCombo();
    }
    const unsigned base = table == 16 ? kCurrentMemory : kMemory + 0x24u * static_cast<unsigned>(table);
    bit_grid("Treasure Chests", base + 0x00, 32, scale);
    bit_grid("Switches", base + 0x04, 128, scale);
    bit_grid("Items", base + 0x14, 32, scale);
    bit_grid("Visited Rooms", base + 0x18, 64, scale);
    ImGui::Spacing();
    ImGui::SetNextItemWidth(160 * scale);
    int keys = static_cast<int>(get(base + 0x20, 1));
    if (ImGui::SliderInt("Small Keys", &keys, 0, 9))
        set(base + 0x20, 1, static_cast<uint32_t>(keys));
    const char* items[] = {"Map", "Compass", "Boss Key", "Boss Defeated", "Heart Container Taken", "Boss Demo Seen"};
    for (unsigned i = 0; i < 6; ++i)
        check_bit(items[i], base + 0x21, i);
}

void embed_flag_viewer(float scale) {
    if (game_running() && ImGui::BeginTabBar("##flags")) {
        if (ImGui::BeginTabItem("Event Flags")) {
            event_flags(kEvent, "Event", scale);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Temporary Flags")) {
            event_flags(kTmpEvent, "Temporary", scale);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stage Memory")) {
            stage_memory(scale);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void draw_flag_viewer(float scale, float fontScale) {
    shipmenu::PopoutWindow("Flag Viewer", "gOpenWindows.FlagViewer", ImVec2(600 * scale, 560 * scale), 0.97f, fontScale,
                           [&] { embed_flag_viewer(scale); });
}

// --- Actor Viewer -----------------------------------------------------------------

void embed_actor_viewer(float scale) {
    static std::vector<BluewakeDevActor> actors(2048);
    static unsigned count;
    static unsigned selected;
    static char filter[48];
    if (g_live)
        count = bluewake_dev_actors(actors.data(), static_cast<unsigned>(actors.size()));
    else
        count = 0;
    ImGui::SetNextItemWidth(220 * scale);
    ImGui::InputTextWithHint("##filter", "Filter by name...", filter, sizeof filter);
    ImGui::SameLine();
    ImGui::Text("%u actors", count);
    const BluewakeDevActor* chosen = nullptr;
    if (ImGui::BeginChild("##list", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("##actors", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Actor");
            ImGui::TableSetupColumn("Params");
            ImGui::TableSetupColumn("Position");
            ImGui::TableSetupColumn("Health");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            std::string needle = filter;
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });
            for (unsigned i = 0; i < count; ++i) {
                const BluewakeDevActor& a = actors[i];
                const char* name = proc_name(a.proc);
                char fallback[24];
                if (name == nullptr) {
                    std::snprintf(fallback, sizeof fallback, "Process 0x%04X", static_cast<unsigned>(a.proc) & 0xFFFFu);
                    name = fallback;
                }
                if (!needle.empty()) {
                    std::string lower = name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (lower.find(needle) == std::string::npos)
                        continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char address[16];
                std::snprintf(address, sizeof address, "%08X", a.address);
                if (ImGui::Selectable(address, selected == a.address, ImGuiSelectableFlags_SpanAllColumns))
                    selected = a.address;
                if (selected == a.address)
                    chosen = &a;
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ImGui::Text("%08X", a.params);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f, %.0f, %.0f", a.pos[0], a.pos[1], a.pos[2]);
                ImGui::TableNextColumn();
                ImGui::Text("%d", a.health);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    if (chosen == nullptr)
        for (unsigned i = 0; i < count; ++i)
            if (actors[i].address == selected)
                chosen = &actors[i];
    if (chosen != nullptr) {
        const char* name = proc_name(chosen->proc);
        ImGui::SeparatorText(name != nullptr ? name : "Actor");
        ImGui::Text("Address 0x%08X   Process 0x%04X   Parameters 0x%08X   Set ID %u   Group %u   Status 0x%08X",
                    chosen->address, static_cast<unsigned>(chosen->proc) & 0xFFFFu, chosen->params, chosen->set_id,
                    chosen->group, chosen->status);
        float pos[3] = {chosen->pos[0], chosen->pos[1], chosen->pos[2]};
        ImGui::SetNextItemWidth(360 * scale);
        if (ImGui::DragFloat3("Position", pos, 10.0f)) {
            for (unsigned i = 0; i < 3; ++i) {
                uint32_t bits;
                std::memcpy(&bits, &pos[i], sizeof bits);
                bluewake_dev_write(chosen->address + 0x1F8 + 4 * i, bits, 4);
                bluewake_dev_write(chosen->address + 0x1E4 + 4 * i, bits, 4);
            }
        }
        if (ImGui::Button("Teleport Link Here")) {
            unsigned player = 0;
            unsigned char raw[4] = {0};
            if (bluewake_dev_read(kPlayerPtr, raw, 4))
                player = (static_cast<unsigned>(raw[0]) << 24) | (raw[1] << 16) | (raw[2] << 8) | raw[3];
            if (player >= 0x80000000u && player != chosen->address)
                for (unsigned i = 0; i < 3; ++i) {
                    uint32_t bits;
                    std::memcpy(&bits, &chosen->pos[i], sizeof bits);
                    bluewake_dev_write(player + 0x1F8 + 4 * i, bits, 4);
                    bluewake_dev_write(player + 0x1E4 + 4 * i, bits, 4);
                }
        }
    }
}

void draw_actor_viewer(float scale, float fontScale) {
    shipmenu::PopoutWindow("Actor Viewer", "gOpenWindows.ActorViewer", ImVec2(760 * scale, 520 * scale), 0.97f, fontScale,
                           [&] { embed_actor_viewer(scale); });
}

// --- Console ----------------------------------------------------------------------

struct Line {
    std::string text;
    bool command;
};
std::vector<Line> g_console;
std::vector<std::string> g_history;

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        size_t j = i;
        while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j])))
            ++j;
        if (j > i)
            words.push_back(line.substr(i, j - i));
        i = j;
    }
    return words;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool number(const std::string& word, long* out) {
    char* end = nullptr;
    *out = std::strtol(word.c_str(), &end, 0);
    return end != word.c_str() && *end == '\0';
}

// An item by name (case and underscores ignored) or number.
int find_item(const std::string& word) {
    long n;
    if (number(word, &n))
        return static_cast<int>(n);
    std::string wanted;
    for (char c : lower(word))
        if (c != '_' && c != '-')
            wanted += c;
    for (const auto& item : kItemNames) {
        std::string name;
        for (char c : lower(item.name))
            if (c != '_')
                name += c;
        if (name == wanted)
            return item.id;
    }
    return -1;
}

int slot_of(int item) {
    for (unsigned slot = 0; slot < 21; ++slot)
        for (int id : kSlots[slot].items)
            if (id == item)
                return static_cast<int>(slot);
    return -1;
}

float to_float(const std::string& s) { return std::strtof(s.c_str(), nullptr); }

std::string help_text() {
    return "Commands:\n"
           "  help                              this list\n"
           "  clear                             empties the console\n"
           "  pos                               the stage, room and Link's place\n"
           "  warp <stage> <room> [point] [layer]   a scene change, as a door makes one\n"
           "  ride <stage> <room> [point] [mode]   a scene change that starts Link on the King of Red Lions\n"
           "  teleport <stage> <room> <x> <y> <z> [angle]   Link to a place in a stage\n"
           "  flag <get|set|clear> <event|tmp> <id>   an event flag (id as 0x0A04: byte, then bit mask)\n"
           "  give <item> [count]               an item in its slot (name or number; count for bombs and arrows)\n"
           "  equip <X|Y|Z> <item|none>         an item on a button (give it first)\n"
           "  collect <songs|pearls|shards|swords>   every song, pearl, triforce shard or sword and shield\n"
           "  set <health|maxhealth|rupees|magic|maxmagic> <n>   (health in quarter hearts)\n"
           "  cosmetic equip:<id>=<rrggbb>[;...]   Cosmetics Editor colours by option id (testing)\n"
           "  items [filter]                    the item names\n"
           "  actors [filter]                   the live actors\n"
           "  peek <address> [size]             guest memory\n"
           "  poke <address> <value> [size]";
}

}  // namespace

std::string RunCommand(const std::string& line) {
    const std::vector<std::string> w = split(line);
    if (w.empty())
        return "";
    const std::string cmd = lower(w[0]);
    char out[512];
    if (cmd == "help")
        return help_text();
    if (cmd == "clear") {
        g_console.clear();
        return "";
    }
    if (cmd == "pos") {
        const BluewakeLocation l = bluewake_game_location();
        std::snprintf(out, sizeof out, "stage %s room %d point %d layer %d, Link %s at %.0f, %.0f, %.0f facing %d", l.stage,
                      l.room, l.point, l.layer, l.has_player ? "is" : "is not here", l.pos[0], l.pos[1], l.pos[2], l.angle_y);
        return out;
    }
    if (cmd == "spawn") {
        // spawn NAME [params] [distance]: an actor by its profile name (as in a room's actor list, e.g. Zl, Pz, Ship) in
        // front of Link and facing him, to look at with C-up (testing models and recolours anywhere).
        if (w.size() < 2)
            return "usage: spawn <name> [params] [distance]";
        const unsigned params = w.size() > 2 ? static_cast<unsigned>(std::strtoul(w[2].c_str(), nullptr, 0)) : 0xFFFFFFFFu;
        const float distance = w.size() > 3 ? std::strtof(w[3].c_str(), nullptr) : 300.f;
        if (!bluewake_dev_spawn(w[1].c_str(), params, distance))
            return "spawn: a name of up to 15 characters";
        return "spawning " + w[1] + " (the log's [test] spawn line has its process id)";
    }
    if (cmd == "spawnat") {
        // spawnat NAME PARAMS X Y Z: the same at a place in the stage (a test's bomb at a wall's collider: Bomb2, params 0, goes off at once).
        if (w.size() < 6)
            return "usage: spawnat <name> <params> <x> <y> <z>";
        const float at[3] = {std::strtof(w[3].c_str(), nullptr), std::strtof(w[4].c_str(), nullptr), std::strtof(w[5].c_str(), nullptr)};
        if (!bluewake_dev_spawn_at(w[1].c_str(), static_cast<unsigned>(std::strtoul(w[2].c_str(), nullptr, 0)), at))
            return "spawnat: a name of up to 15 characters";
        return "spawning " + w[1] + " at " + w[3] + " " + w[4] + " " + w[5];
    }
    if (cmd == "actors") {
        // actors [name part]: the live actors whose name holds it, with where they are (a test can check that one exists).
        static std::vector<BluewakeDevActor> list(2048);
        const unsigned count = bluewake_dev_actors(list.data(), static_cast<unsigned>(list.size()));
        std::string needle = w.size() > 1 ? w[1] : "";
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });
        std::string result;
        unsigned shown = 0;
        for (unsigned i = 0; i < count; ++i) {
            const char* name = proc_name(list[i].proc);
            std::string lower = name != nullptr ? name : "";
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            if (!needle.empty() && lower.find(needle) == std::string::npos)
                continue;
            char row[160];
            std::snprintf(row, sizeof row, "%s%s (0x%04X) %08X at %.0f %.0f %.0f", shown++ ? "; " : "", name ? name : "?",
                          static_cast<unsigned>(list[i].proc) & 0xFFFFu, list[i].address, list[i].pos[0], list[i].pos[1],
                          list[i].pos[2]);
            result += row;
        }
        std::fprintf(stderr, "[console] actors %s: %u of %u: %s\n", needle.c_str(), shown, count, result.c_str());
        return std::to_string(shown) + " actors" + (result.empty() ? "" : ": " + result);
    }
    if (cmd == "warp" || cmd == "ride") {
        // ride: the same scene change, but the player's start is "arriving by ship" (dSv_restart_c::mLastMode 6, what
        // dComIfGp_setNextStage's caller passes when the ship carries Link over), so Link comes up on the King of Red
        // Lions (daPy_lk_c::create: the ship is moved under him). Needs the ship's owner to have met him (MET_KORL).
        long room, point = 0, layer = -1;
        if (w.size() < 3 || w[1].size() > 8 || !number(w[2], &room))
            return cmd == "ride" ? "usage: ride <stage> <room> [point] [mode]" : "usage: warp <stage> <room> [point] [layer]";
        if (w.size() > 3)
            number(w[3], &point);
        if (cmd == "ride") {
            long mode = 6;
            if (w.size() > 4)
                number(w[4], &mode);
            bluewake_dev_write(kInfo + kRestart + 0x2C, static_cast<unsigned>(mode), 4);
        } else if (w.size() > 4)
            number(w[4], &layer);
        char name[8] = {0};
        std::memcpy(name, w[1].c_str(), w[1].size());
        bluewake_dev_write_bytes(kNextStage, name, 8);
        bluewake_dev_write(kNextStage + 0x8, static_cast<unsigned>(point) & 0xFFFFu, 2);
        bluewake_dev_write(kNextStage + 0xA, static_cast<unsigned>(room) & 0xFFu, 1);
        bluewake_dev_write(kNextStage + 0xB, static_cast<unsigned>(layer) & 0xFFu, 1);
        bluewake_dev_write(kNextStage + 0xD, 0, 1);
        bluewake_dev_write(kNextStage + 0xC, 1, 1);
        std::snprintf(out, sizeof out, "warping to %s room %ld point %ld", w[1].c_str(), room, point);
        std::fprintf(stderr, "[console] %s\n", out);
        return out;
    }
    if (cmd == "teleport") {
        long room;
        if (w.size() < 6 || w[1].size() > 8 || !number(w[2], &room))
            return "usage: teleport <stage> <room> <x> <y> <z> [angle]";
        const float pos[3] = {to_float(w[3]), to_float(w[4]), to_float(w[5])};
        long angle = 0;
        if (w.size() > 6)
            number(w[6], &angle);
        bluewake_request_teleport(w[1].c_str(), static_cast<int>(room), pos, static_cast<int>(angle));
        std::snprintf(out, sizeof out, "teleport to %s room %ld requested (it runs when no event is playing)", w[1].c_str(), room);
        return out;
    }
    if (cmd == "flag") {
        long id;
        if (w.size() < 4 || !number(w[3], &id) || id < 0 || id > 0xFFFF)
            return "usage: flag <get|set|clear> <event|tmp> <id>";
        const bool tmp = lower(w[2]) == "tmp";
        if (!tmp && lower(w[2]) != "event")
            return "the kind is event or tmp";
        const unsigned address = kInfo + (tmp ? kTmpEvent : kEvent) + static_cast<unsigned>(id >> 8);
        unsigned char byte = 0;
        if (!bluewake_dev_read(address, &byte, 1))
            return "the game is not running";
        const unsigned mask = static_cast<unsigned>(id) & 0xFFu;
        const std::string op = lower(w[1]);
        if (op == "get") {
            std::snprintf(out, sizeof out, "%s flag 0x%04lX is %s", tmp ? "tmp" : "event", id, (byte & mask) ? "set" : "clear");
            return out;
        }
        if (op != "set" && op != "clear")
            return "usage: flag <get|set|clear> <event|tmp> <id>";
        bluewake_dev_write(address, op == "set" ? (byte | mask) : (byte & ~mask), 1);
        std::snprintf(out, sizeof out, "%s flag 0x%04lX %s", tmp ? "tmp" : "event", id, op == "set" ? "set" : "cleared");
        std::fprintf(stderr, "[console] %s\n", out);
        return out;
    }
    if (cmd == "give") {
        if (w.size() < 2)
            return "usage: give <item> [count]";
        const int item = find_item(w[1]);
        if (item < 0)
            return "no such item (items lists the names)";
        const int slot = slot_of(item);
        if (slot < 0)
            return "that item has no inventory slot; the Save Editor sets collectibles and equipment";
        bluewake_dev_write(kInfo + 0x3C + static_cast<unsigned>(slot), static_cast<unsigned>(item), 1);
        long count = -1;
        if (w.size() > 2)
            number(w[2], &count);
        if (count >= 0 && (item == 0x31 || item == 0x27 || item == 0x35 || item == 0x36)) {
            const bool bombs = item == 0x31;
            bluewake_dev_write(kInfo + (bombs ? 0x66 + 4 : 0x66 + 3), static_cast<unsigned>(count), 1);
            bluewake_dev_write(kInfo + (bombs ? 0x6E + 2 : 0x6E + 1), std::max<unsigned>(static_cast<unsigned>(count), 30), 1);
        }
        std::snprintf(out, sizeof out, "gave %s in slot %d", item_label(item).c_str(), slot);
        std::fprintf(stderr, "[console] %s\n", out);
        return out;
    }
    if (cmd == "equip") {
        // equip X|Y|Z ITEM: the button's slot in the save and the play scene's copy of the item number (what the HUD shows)
        const std::string button = w.size() > 1 ? lower(w[1]) : "";
        const unsigned which = button == "x" ? 0u : button == "y" ? 1u : button == "z" ? 2u : 3u;
        if (w.size() < 3 || which > 2)
            return "usage: equip <X|Y|Z> <item|none>";
        const bool none = lower(w[2]) == "none";
        const int item = none ? 0xFF : find_item(w[2]);
        const int slot = none ? 0xFF : slot_of(item);
        if (item < 0 || slot < 0)
            return "no such item with an inventory slot (items lists the names)";
        bluewake_dev_write(kInfo + 0x09 + which, static_cast<unsigned>(slot), 1);
        bluewake_dev_write(kInfo + 0x12A0 + 0x4933 + which, static_cast<unsigned>(item), 1);
        std::snprintf(out, sizeof out, "%s on %s (slot %d)", none ? "nothing" : item_label(item).c_str(), w[1].c_str(), slot);
        std::fprintf(stderr, "[console] equip %s\n", out);
        return out;
    }
    if (cmd == "cosmetic") { // testing: cosmetic equip:Trails.SpinAttack=00ff00;Equip.Ship.Hull=ff00ff (the editor's own set_color)
        if (w.size() < 2) return "usage: cosmetic equip:<option id>=<rrggbb>[;...]";
        cosmetics::TestApplyPreset(w[1]);
        return "cosmetics set";
    }
    if (cmd == "collect") {
        const std::string what = w.size() > 1 ? lower(w[1]) : "";
        if (what == "songs") bluewake_dev_write(kInfo + 0xB4 + 9, 0x3F, 1);
        else if (what == "pearls") bluewake_dev_write(kInfo + 0xB4 + 0xB, 0x07, 1);
        else if (what == "shards") bluewake_dev_write(kInfo + 0xB4 + 0xA, 0xFF, 1);
        else if (what == "swords") {
            bluewake_dev_write(kInfo + 0xB4 + 0, 0x0F, 1);
            bluewake_dev_write(kInfo + 0xB4 + 1, 0x03, 1);
        } else return "usage: collect <songs|pearls|shards|swords>";
        std::fprintf(stderr, "[console] collected %s\n", what.c_str());
        return "collected " + what;
    }
    if (cmd == "set") {
        long n;
        if (w.size() < 3 || !number(w[2], &n))
            return "usage: set <health|maxhealth|rupees|magic|maxmagic> <n>";
        const std::string what = lower(w[1]);
        unsigned offset = 0, size = 2;
        if (what == "health") offset = 0x02;
        else if (what == "maxhealth") offset = 0x00;
        else if (what == "rupees") offset = 0x04;
        else if (what == "magic") { offset = 0x14; size = 1; }
        else if (what == "maxmagic") { offset = 0x13; size = 1; }
        else return "usage: set <health|maxhealth|rupees|magic|maxmagic> <n>";
        bluewake_dev_write(kInfo + offset, static_cast<unsigned>(n), size);
        std::snprintf(out, sizeof out, "%s set to %ld", what.c_str(), n);
        std::fprintf(stderr, "[console] %s\n", out);
        return out;
    }
    if (cmd == "items") {
        const std::string filter = w.size() > 1 ? lower(w[1]) : "";
        std::string text;
        int shown = 0;
        for (const auto& item : kItemNames) {
            if (!filter.empty() && lower(item.name).find(filter) == std::string::npos)
                continue;
            char one[64];
            std::snprintf(one, sizeof one, "0x%02X %s%s", item.id, item.name, slot_of(item.id) >= 0 ? " [slot]" : "");
            text += (shown++ ? "\n" : "") + std::string(one);
            if (shown >= 60) {
                text += "\n...";
                break;
            }
        }
        return text.empty() ? "none" : text;
    }
    if (cmd == "actors") {
        std::vector<BluewakeDevActor> actors(2048);
        const unsigned count = bluewake_dev_actors(actors.data(), static_cast<unsigned>(actors.size()));
        const std::string filter = w.size() > 1 ? lower(w[1]) : "";
        std::string text;
        unsigned shown = 0;
        for (unsigned i = 0; i < count; ++i) {
            const char* name = proc_name(actors[i].proc);
            char fallback[24];
            if (name == nullptr) {
                std::snprintf(fallback, sizeof fallback, "0x%04X", static_cast<unsigned>(actors[i].proc) & 0xFFFFu);
                name = fallback;
            }
            if (!filter.empty() && lower(name).find(filter) == std::string::npos)
                continue;
            if (shown++ >= 40) {
                text += "\n...";
                break;
            }
            char one[160];
            std::snprintf(one, sizeof one, "%08X %s params %08X at %.0f, %.0f, %.0f", actors[i].address, name,
                          actors[i].params, actors[i].pos[0], actors[i].pos[1], actors[i].pos[2]);
            text += (text.empty() ? "" : "\n") + std::string(one);
        }
        std::snprintf(out, sizeof out, "%u actors, %u shown", count, std::min(shown, 40u));
        return (text.empty() ? std::string() : text + "\n") + out;
    }
    if (cmd == "peek") {
        long address, size = 4;
        if (w.size() < 2 || !number(w[1], &address))
            return "usage: peek <address> [size]";
        if (w.size() > 2)
            number(w[2], &size);
        if (size != 1 && size != 2 && size != 4)
            return "the size is 1, 2 or 4";
        unsigned char raw[4] = {0};
        if (!bluewake_dev_read(static_cast<unsigned>(address), raw, static_cast<unsigned>(size)))
            return "that is not in main memory";
        unsigned v = 0;
        for (long i = 0; i < size; ++i)
            v = (v << 8) | raw[i];
        std::snprintf(out, sizeof out, "0x%08lX = 0x%0*X", address, static_cast<int>(size * 2), v);
        return out;
    }
    if (cmd == "poke") {
        long address, value, size = 4;
        if (w.size() < 3 || !number(w[1], &address) || !number(w[2], &value))
            return "usage: poke <address> <value> [size]";
        if (w.size() > 3)
            number(w[3], &size);
        if (size != 1 && size != 2 && size != 4)
            return "the size is 1, 2 or 4";
        bluewake_dev_write(static_cast<unsigned>(address), static_cast<unsigned>(value), static_cast<unsigned>(size));
        std::snprintf(out, sizeof out, "0x%08lX <- 0x%lX", address, value);
        std::fprintf(stderr, "[console] %s\n", out);
        return out;
    }
    return "unknown command (help lists them)";
}

namespace {

void embed_console(float scale) {
    static char input[256];
    static bool focus;
    static int history_at = -1;
    const float footer = ImGui::GetFrameHeightWithSpacing() + 4 * scale;
    if (ImGui::BeginChild("##scroll", ImVec2(0, -footer), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (g_console.empty())
            ImGui::TextDisabled("Type help for the commands.");
        for (const Line& line : g_console) {
            if (line.command)
                ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "> %s", line.text.c_str());
            else
                ImGui::TextUnformatted(line.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1);
    if (focus) {
        ImGui::SetKeyboardFocusHere();
        focus = false;
    }
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
    auto history = [](ImGuiInputTextCallbackData* data) {
        if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || g_history.empty())
            return 0;
        if (data->EventKey == ImGuiKey_UpArrow)
            history_at = history_at < 0 ? static_cast<int>(g_history.size()) - 1 : std::max(0, history_at - 1);
        else if (data->EventKey == ImGuiKey_DownArrow)
            history_at = history_at < 0 ? -1 : (history_at + 1 >= static_cast<int>(g_history.size()) ? -1 : history_at + 1);
        data->DeleteChars(0, data->BufTextLen);
        if (history_at >= 0)
            data->InsertChars(0, g_history[static_cast<size_t>(history_at)].c_str());
        return 0;
    };
    if (ImGui::InputText("##input", input, sizeof input, flags, history)) {
        const std::string line = input;
        input[0] = '\0';
        history_at = -1;
        focus = true;
        if (!line.empty()) {
            g_history.push_back(line);
            g_console.push_back({line, true});
            const std::string answer = RunCommand(line);
            if (!answer.empty())
                g_console.push_back({answer, false});
            if (g_console.size() > 500)
                g_console.erase(g_console.begin(), g_console.begin() + 100);
        }
    }
}

void draw_console(float scale, float fontScale) {
    shipmenu::PopoutWindow("Console", "gOpenWindows.Console", ImVec2(680 * scale, 380 * scale), 0.97f, fontScale,
                           [&] { embed_console(scale); });
}

// --- Stats ------------------------------------------------------------------------

struct StageGroup {
    int id;
    const char* name;
};
struct StagePlace {
    int group;
    const char* name;
    const char* stage;
    int room, point, layer;
    const char* note;
};
#include "stage_names.inc"

// Warp: every place of the developers' stage select, by group, with its internal stage and room; a confirm step,
// then the console's own warp.
void embed_warp(float scale) {
    static int group = 22; // Outset places
    static int place = -1;
    static char filter[64];
    auto group_name = [](int id) -> const char* {
        for (const StageGroup& g : kStageGroups)
            if (g.id == id)
                return g.name;
        return "?";
    };
    // A sea square shows its island's name and chart square, then the developers' own words for the spot ("Sector 2F
    // push-pull idea" is "Angular Isles (E7) - push-pull idea").
    auto shown_name = [](const StagePlace& p) {
        if (std::strcmp(p.stage, "sea") != 0 || sea_chart::Island(p.room) == nullptr)
            return std::string(p.name);
        std::string rest = p.name;
        if (rest.rfind("Sector ", 0) == 0) {
            const size_t space = rest.find(' ', 7);
            rest = space == std::string::npos ? std::string() : rest.substr(space + 1);
        }
        return sea_chart::Name(p.room) + (rest.empty() ? "" : " - " + rest);
    };
    auto label = [shown_name](const StagePlace& p) {
        char text[200];
        std::snprintf(text, sizeof text, "%s%s%s%s   (%s, room %d)", shown_name(p).c_str(), p.note[0] ? " (" : "",
                      p.note, p.note[0] ? ")" : "", p.stage, p.room);
        return std::string(text);
    };
    ImGui::TextWrapped("Pick a place and Warp: a scene change there, as a door makes one. Places marked hangs or crash "
                       "do that in the original game too.");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##group", group_name(group))) {
        for (const StageGroup& g : kStageGroups)
            if (ImGui::Selectable(g.name, g.id == group)) {
                group = g.id;
                place = -1;
            }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Search every group (name or stage)", filter, sizeof filter);
    const std::string want = [&] {
        std::string f = filter;
        for (char& ch : f)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return f;
    }();
    auto matches = [&](const StagePlace& p) {
        if (want.empty())
            return p.group == group;
        std::string hay = std::string(p.name) + " " + shown_name(p) + " " + p.stage + " " + group_name(p.group);
        for (char& ch : hay)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return hay.find(want) != std::string::npos;
    };
    const float list_h = ImGui::GetTextLineHeightWithSpacing() * 12;
    if (ImGui::BeginChild("##places", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(std::size(kStagePlaces)); ++i) {
            const StagePlace& p = kStagePlaces[i];
            if (!matches(p))
                continue;
            ImGui::PushID(i);
            if (ImGui::Selectable(label(p).c_str(), place == i))
                place = i;
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    const bool can = place >= 0 && g_live;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Warp", ImVec2(-1, 0)))
        ImGui::OpenPopup("Confirm warp");
    ImGui::EndDisabled();
    if (!g_live)
        ImGui::TextDisabled("Start a game first.");
    if (ImGui::BeginPopupModal("Confirm warp", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (place >= 0) {
            const StagePlace& p = kStagePlaces[place];
            ImGui::Text("Warp to %s?", p.name);
            ImGui::TextDisabled("%s, room %d, point %d", p.stage, p.room, p.point);
            if (ImGui::Button("Warp", ImVec2(120 * scale, 0))) {
                char cmd[96];
                std::snprintf(cmd, sizeof cmd, "warp %s %d %d %d", p.stage, p.room, p.point, p.layer);
                RunCommand(cmd);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Cancel", ImVec2(120 * scale, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void embed_stats(float scale) {
    ImGui::Text("Frames shown: %.1f a second", aurora::gfx::calculate_fps());
    ImGui::Text("Game frames: %.1f a second", aurora::gfx::calculate_game_fps());
    const BluewakeLocation l = bluewake_game_location();
    ImGui::Separator();
    ImGui::Text("Stage %s, room %d, point %d, layer %d", l.stage[0] ? l.stage : "-", l.room, l.point, l.layer);
    if (l.has_player) {
        ImGui::Text("Link at %.0f, %.0f, %.0f", l.pos[0], l.pos[1], l.pos[2]);
        ImGui::Text("Facing %d (%.0f degrees)", l.angle_y, static_cast<float>(l.angle_y) * 360.0f / 65536.0f);
    }
}

void draw_stats(float scale, float fontScale) {
    shipmenu::PopoutWindow("Stats", "gOpenWindows.Stats", ImVec2(360 * scale, 200 * scale), 0.9f, fontScale,
                           [&] { embed_stats(scale); });
}

void draw_warp(float scale, float fontScale) {
    shipmenu::PopoutWindow("Warp", "gOpenWindows.Warp", ImVec2(560 * scale, 420 * scale), 0.97f, fontScale,
                           [&] { embed_warp(scale); });
}

}  // namespace

void BuildMenu(shipmenu::Menu& m, const char* H) {
    struct Page {
        const char* sidebar;
        const char* cvar;
        const char* tooltip;
        void (*body)(float);
    };
    const Page pages[] = {
        {"Save Editor", "gOpenWindows.SaveEditor",
         "Edit the current save: health, rupees, magic, name, items, equipment, songs, pearls and Triforce shards.",
         embed_save_editor},
        {"Flag Viewer", "gOpenWindows.FlagViewer",
         "See and change the event flags, the temporary flags and each stage's chests, switches, items, rooms and keys.",
         embed_flag_viewer},
        {"Actor Viewer", "gOpenWindows.ActorViewer",
         "Lists the actors in the scene, to move one or bring Link to it.", embed_actor_viewer},
        {"Console", "gOpenWindows.Console",
         "A console: warp, teleport, flag, give, set, peek and poke. Type help in it.", embed_console},
        {"Warp", "gOpenWindows.Warp", "Every place in the game by name and internal stage, to warp to.", embed_warp},
        {"Stats", "gOpenWindows.Stats", "The frame rates, the stage, the room and where Link is.", embed_stats},
    };
    for (const Page& page : pages) {
        void (*body)(float) = page.body;
        m.AddSidebar(H, page.sidebar, 1);
        m.AddWidget(H, page.sidebar, 0, page.sidebar, WidgetType::Window)
            .CVar(page.cvar)
            .HideInSearch()
            .Tooltip(page.tooltip)
            .CustomFunction([body](shipmenu::WidgetInfo&) {
                g_live = bluewake_dev_read(kInfo, g_snap, sizeof g_snap);
                body(shipmenu::MenuScale());
            });
    }
}

void Frame(float scale, float fontScale) {
    const bool save = CVarGetInteger("gOpenWindows.SaveEditor", 0) != 0;
    const bool flags = CVarGetInteger("gOpenWindows.FlagViewer", 0) != 0;
    const bool actors = CVarGetInteger("gOpenWindows.ActorViewer", 0) != 0;
    const bool console = CVarGetInteger("gOpenWindows.Console", 0) != 0;
    const bool stats = CVarGetInteger("gOpenWindows.Stats", 0) != 0;
    const bool warp = CVarGetInteger("gOpenWindows.Warp", 0) != 0;
    TestRun();
    if (!save && !flags && !actors && !console && !stats && !warp)
        return;
    g_live = bluewake_dev_read(kInfo, g_snap, sizeof g_snap);
    if (save)
        draw_save_editor(scale, fontScale);
    if (flags)
        draw_flag_viewer(scale, fontScale);
    if (actors)
        draw_actor_viewer(scale, fontScale);
    if (console)
        draw_console(scale, fontScale);
    if (stats)
        draw_stats(scale, fontScale);
    if (warp)
        draw_warp(scale, fontScale);
}

void TestRun() {
    static bool parsed;
    static std::vector<std::string> commands;
    static Uint64 next;
    static size_t at;
    if (!parsed) {
        parsed = true;
        const char* list = std::getenv("BLUEWAKE_TEST_CONSOLE");
        for (const char* p = list; p != nullptr && *p != '\0';) {
            const char* end = std::strchr(p, ';');
            commands.emplace_back(p, end != nullptr ? static_cast<size_t>(end - p) : std::strlen(p));
            p = end != nullptr ? end + 1 : "";
        }
        next = SDL_GetTicks() + 1500;
    }
    if (at >= commands.size() || SDL_GetTicks() < next)
        return;
    // "retrace:command" waits for that retrace (a scenario's timeline); a plain command runs a second after the last.
    std::string line = commands[at];
    const size_t colon = line.find(':');
    if (colon != std::string::npos && colon > 0 &&
        line.find_first_not_of("0123456789") == colon) {
        if (bluewake_host_retrace_count() < std::strtoull(line.c_str(), nullptr, 10))
            return;
        line = line.substr(colon + 1);
    }
    ++at;
    next = SDL_GetTicks() + 1000;
    std::fprintf(stderr, "[test] console> %s\n", line.c_str());
    std::string answer = RunCommand(line);
    for (size_t i = 0; i < answer.size();) {
        const size_t end = answer.find('\n', i);
        std::fprintf(stderr, "[console] %s\n", answer.substr(i, end == std::string::npos ? end : end - i).c_str());
        i = end == std::string::npos ? answer.size() : end + 1;
    }
}

}  // namespace devtools
