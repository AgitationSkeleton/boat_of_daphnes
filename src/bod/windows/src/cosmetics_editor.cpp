// The Cosmetics Editor: see cosmetics_editor.h. SoH publishes no license; this
// follows its layout and behaviour (docs/fork/research/soh-cosmetics.md) with
// code of its own. Link's colors are wwrando's names and base colors
// (assets/link_metadata.txt, MIT); the trail colors are the game's. With a player
// model active, the Link tab shows the model's own colors (model_colors.h).
#include "cosmetics_editor.h"

#include "ship_menu.h"
#include "link_colors.h"
#include "model_colors.h"
#include "mods.h"
#include "rarc.h"
#include "ship/cvar.h"

#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <iterator>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

extern "C" void bluewake_voice_pitch_set_local(float ratio);
extern "C" void bluewake_text_swap_set_female(bool on);
extern "C" bool bluewake_bunny_hood_enabled(void);
extern "C" bool bluewake_cosmetics_get(const char* id, uint8_t rgba[4]);
extern "C" void bluewake_bunny_hood_enable(bool on);
extern "C" void bluewake_cosmetics_set(const char* id, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool changed);

namespace cosmetics {
namespace {

enum Group { HERO, CASUAL, SWORDS, SHIELDS, TRAILS, BUTTONS, HOOD, SHIP, TETRA, ZELDA, ITEMS, GROUP_COUNT };
const char* const kGroupLabels[GROUP_COUNT] = {"Hero's Clothes", "Outset Clothes", "Swords", "Shields", "Trails", "Buttons", "Bunny Hood", "King of Red Lions", "Tetra", "Princess Zelda", "Items and Gear"};

struct Option {
    const char* id;    // gCosmetics.<id>, and the id the game side knows
    const char* label;
    Group group;
    uint32_t color;    // the game's own, 0xRRGGBB
    bool model = false; // a color of the active player model (its id, label and color are g_strings')
};

const Option kOptions[] = {
    {"Link.Hero.Hair", "Hair", HERO, 0xFFEF10},
    {"Link.Hero.Skin", "Skin", HERO, 0xF7DB9C},
    {"Link.Hero.Mouth", "Mouth", HERO, 0xF74963},
    {"Link.Hero.Eyes", "Eyes", HERO, 0x10514A},
    {"Link.Hero.Sclera", "Sclera", HERO, 0xFFFFFF},
    {"Link.Hero.Hat", "Hat", HERO, 0x5AB24A},
    {"Link.Hero.Tunic", "Tunic", HERO, 0x5AB24A},
    {"Link.Hero.Undershirt", "Undershirt", HERO, 0xADE342},
    {"Link.Hero.Pants", "Pants", HERO, 0xFFFFFF},
    {"Link.Hero.Boots", "Boots", HERO, 0x944908},
    {"Link.Hero.Belt", "Belt", HERO, 0x633008},
    {"Link.Hero.BeltBuckle", "Belt Buckle", HERO, 0xFFEF10},
    {"Link.Casual.Hair", "Hair", CASUAL, 0xFFEF10},
    {"Link.Casual.Skin", "Skin", CASUAL, 0xF7DB9C},
    {"Link.Casual.Mouth", "Mouth", CASUAL, 0xF74963},
    {"Link.Casual.Eyes", "Eyes", CASUAL, 0x10514A},
    {"Link.Casual.Sclera", "Sclera", CASUAL, 0xFFFFFF},
    {"Link.Casual.Shirt", "Shirt", CASUAL, 0x4A75AD},
    {"Link.Casual.ShirtEmblem", "Shirt Emblem", CASUAL, 0xFFFFD6},
    {"Link.Casual.Armbands", "Armbands", CASUAL, 0x63696B},
    {"Link.Casual.Pants", "Pants", CASUAL, 0xFFA608},
    {"Link.Casual.Shoes", "Shoes", CASUAL, 0x63696B},
    {"Link.Casual.ShoeSoles", "Shoe Soles", CASUAL, 0xD6BA39},
    {"Trails.Sword", "Sword", TRAILS, 0xFFFFFF},
    {"Trails.SwordSoup", "Sword (Elixir Soup)", TRAILS, 0xFFFF7B},
    {"Trails.SwordParry", "Sword (Parry)", TRAILS, 0x5AFF5A},
    {"Trails.Boomerang", "Boomerang", TRAILS, 0xFFFF7B},
    // Spin attack effects (runtime/host/src/cosmetics.c): the particles' emitters take the color while they play.
    {"Trails.SpinCharge", "Spin Attack (Charge)", TRAILS, 0xFFFFFF},
    {"Trails.SpinAttack", "Spin Attack", TRAILS, 0xFFFFFF},
    // Equipment: the textures' hue shifts from the color shown to the one chosen; grey metal ("tinted") takes the color over
    // its own light and shade (windows/src/link_colors.cpp).
    {"Equip.HeroSword.Blade", "Hero's Sword Blade", SWORDS, 0xF0F5D8},
    {"Equip.HeroSword.Hilt", "Hero's Sword Hilt", SWORDS, 0xBAA674},
    {"Equip.MasterSword.Blade", "Master Sword Blade", SWORDS, 0xE8E8E8},
    {"Equip.MasterSword.Hilt", "Master Sword Hilt", SWORDS, 0x476BC7},
    {"Equip.HeroShield.Face", "Hero's Shield Face", SHIELDS, 0x986951},
    {"Equip.HeroShield.Back", "Hero's Shield Back and Grip", SHIELDS, 0xC89D68},
    {"Equip.MirrorShield.Face", "Mirror Shield Face", SHIELDS, 0xD5D7C3},
    {"Equip.MirrorShield.Frame", "Mirror Shield Frame", SHIELDS, 0x4B6EC9},
    {"Equip.Scabbard.Body", "Hero's Sword Scabbard", SWORDS, 0x926B48},
    {"Equip.MasterScabbard.Body", "Master Sword Scabbard", SWORDS, 0x4467C1},
    {"Equip.Bow.Body", "Hero's Bow (Zelda's too)", ITEMS, 0xDDA308},
    {"Equip.Arrow.Body", "Arrows", ITEMS, 0xC2A978},
    {"Equip.Boomerang.Body", "Boomerang", ITEMS, 0xEFF054},
    {"Equip.Hookshot.Body", "Grappling Hook (tinted)", ITEMS, 0x7D8AB0},
    {"Equip.DekuLeaf.Leaf", "Deku Leaf", ITEMS, 0x14941B},
    {"Equip.Bomb.Body", "Bombs", ITEMS, 0xF8B740},
    {"Equip.Telescope.Body", "Telescope", ITEMS, 0xC56121},
    {"Equip.PowerBracelets.Body", "Power Bracelets", ITEMS, 0xEEE349},
    {"Equip.SkullHammer.Body", "Skull Hammer (tinted)", ITEMS, 0x6B665B},
    {"Equip.IronBoots.Body", "Iron Boots (tinted)", ITEMS, 0xB7B7B7},
    {"Equip.PictoBox.Body", "Picto Box", ITEMS, 0xBC270A},
    {"Equip.TingleTuner.Body", "Tingle Tuner", ITEMS, 0x028731},
    {"Equip.Baton.Body", "Wind Waker (tinted)", ITEMS, 0xCCCCCC},
    {"Equip.HyoiPear.Body", "Hyoi Pear", ITEMS, 0xD8B33B},
    // The King of Red Lions (Ship.arc's textures, windows/src/link_colors.cpp): the hull and figurehead's red wood, the sail's picture.
    {"Equip.Ship.Hull", "Hull", SHIP, 0xC44B29},
    {"Equip.Ship.Head", "Figurehead", SHIP, 0xC84321},
    {"Equip.Ship.Sail", "Sail", SHIP, 0x236E6B},
    // Tetra and Princess Zelda (Zl.arc, Pz.arc): parts found in their textures and cut out with masks (scripts/windows/link_colors.py,
    // windows/src/link_colors.cpp). The colors shown are the parts' own, as that script measures them.
    {"Equip.Tetra.Hair", "Hair and Eyebrows", TETRA, 0xC6FF00},
    {"Equip.Tetra.Skin", "Skin", TETRA, 0xB59A5A},
    {"Equip.Tetra.Eyes", "Eyes", TETRA, 0x104770},
    {"Equip.Tetra.Sclera", "Sclera", TETRA, 0xEFF7FF},
    {"Equip.Tetra.Lips", "Lips", TETRA, 0x997C42},
    {"Equip.Tetra.Bandana", "Bandana", TETRA, 0xFF0042},
    {"Equip.Tetra.Shirt", "Shirt", TETRA, 0xEFF7FF},
    {"Equip.Tetra.Trim", "Pants and Blue Trim", TETRA, 0x184994},
    {"Equip.Tetra.Leather", "Leather (Belt, Boots)", TETRA, 0x5A3C18},
    {"Equip.Tetra.Gold", "Gold (Jewelry)", TETRA, 0xEFBE00},
    {"Equip.Zelda.Hair", "Hair and Eyebrows", ZELDA, 0xC6F700},
    {"Equip.Zelda.Skin", "Skin", ZELDA, 0xFFE3D6},
    {"Equip.Zelda.Eyes", "Eyes", ZELDA, 0x104573},
    {"Equip.Zelda.Sclera", "Sclera", ZELDA, 0xEFF7FF},
    {"Equip.Zelda.Lips", "Lips", ZELDA, 0xD6A2AD},
    {"Equip.Zelda.Eyeshadow", "Eyeshadow", ZELDA, 0x9CC7EF},
    {"Equip.Zelda.Dress", "Dress", ZELDA, 0xE7AEEF},
    {"Equip.Zelda.Gold", "Gold (Jewelry, Crest)", ZELDA, 0xCEB600},
    {"Equip.Zelda.Blue", "Blue Accents", ZELDA, 0x4271AD},
    {"Equip.Zelda.Cloth", "White Cloth", ZELDA, 0xDEE7EF},
    {"Equip.Zelda.Emblem", "Emblem", ZELDA, 0xEFF7FF},
    {"Hud.AButton", "A Button", BUTTONS, 0x35C39B},
    {"Hud.BButton", "B Button", BUTTONS, 0xFA3A1C},
    {"Hud.XButton", "X Button", BUTTONS, 0x6E5082},
    {"Hud.YButton", "Y Button", BUTTONS, 0x6E5082},
    {"Hud.ZButton", "Z Button", BUTTONS, 0x9839D3},
    {"Hud.RButton", "R Button", BUTTONS, 0x323232},
    // The Bunny Hood (runtime/host/src/bunny_hood.c): the model's own colours are the defaults shown here.
    {"BunnyHood.Body", "Body", HOOD, 0xC5B46C},
    {"BunnyHood.InnerEars", "Inner Ears", HOOD, 0x73518C},
    {"BunnyHood.Band", "Band", HOOD, 0x9C8E4A},
};

struct Tab {
    const char* label;
    std::vector<Group> groups;
};
const Tab kTabs[] = {
    {"Link", {HERO, CASUAL}},
    {"Equipment", {SWORDS, SHIELDS, ITEMS}},
    {"King of Red Lions", {SHIP}},
    {"Characters", {TETRA, ZELDA}},
    {"Effects", {TRAILS}},
    {"HUD", {BUTTONS}},
    {"Bunny Hood", {HOOD}},
};

// The player model whose colors the Link tab shows (null: Link's own, kOptions). Its rows are the
// model's colors per outfit in the metadata's order; the ids carry the model's name, so switching
// models keeps each one's palette (gCosmetics.Link.<Model>.Hero.<Color>.*).
std::unique_ptr<model_colors::Set> g_model;
std::deque<std::string> g_strings; // owns the ids and labels of the model's rows
std::vector<Option> g_options;     // every row shown: the table, with the model's in place of Link's
std::atomic<bool> g_model_changed{true}; // mods tells it the player model changed (true: look at start)

void rebuild_options() {
    g_options.clear();
    g_strings.clear();
    bool model_rows = false;
    for (const Option& o : kOptions) {
        if (g_model && (o.group == HERO || o.group == CASUAL)) {
            if (!model_rows) { // where Link's rows were
                model_rows = true;
                for (int outfit = 0; outfit < 2; ++outfit)
                    for (const model_colors::Color& c : g_model->outfits[outfit].colors) {
                        g_strings.push_back(model_colors::ColorId(*g_model, outfit == 1, c.name));
                        const char* id = g_strings.back().c_str();
                        g_strings.push_back(c.name);
                        g_options.push_back({id, g_strings.back().c_str(), outfit == 1 ? CASUAL : HERO, c.base, true});
                    }
            }
            continue;
        }
        g_options.push_back(o);
    }
}

const std::vector<Option>& options() {
    if (g_options.empty())
        rebuild_options();
    return g_options;
}

std::string cvar(const Option& o, const char* field) { return std::string("gCosmetics.") + o.id + "." + field; }

CVarColor game_color(const Option& o) {
    return CVarColor{static_cast<uint8_t>(o.color >> 16), static_cast<uint8_t>(o.color >> 8),
                     static_cast<uint8_t>(o.color), 255};
}

CVarColor value(const Option& o) { return CVarGetColor(cvar(o, "Value").c_str(), game_color(o)); }
bool changed(const Option& o) { return CVarGetInteger(cvar(o, "Changed").c_str(), 0) != 0; }
bool locked(const Option& o) { return CVarGetInteger(cvar(o, "Locked").c_str(), 0) != 0; }
bool rainbow(const Option& o) { return CVarGetInteger(cvar(o, "Rainbow").c_str(), 0) != 0; }

// The color to where it is used: the game (by id), or for a model's, the recolor.
void push(const Option& o, CVarColor c, bool is_changed) {
    if (o.model)
        link_colors::SetModelColor(o.id, uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | c.b, is_changed);
    else
        bluewake_cosmetics_set(o.id, c.r, c.g, c.b, c.a, is_changed);
}

// Whether the color reaches the game through link_colors' textures (Link's clothes, swords, shields).
bool uses_link_colors(const Option& o) {
    return o.group == HERO || o.group == CASUAL || o.group == SWORDS || o.group == SHIELDS || o.group == SHIP || o.group == TETRA ||
           o.group == ZELDA || o.group == ITEMS;
}

void apply(const Option& o) {
    push(o, value(o), changed(o));
    if (uses_link_colors(o))
        link_colors::MarkDirty();
}

void set_color(const Option& o, CVarColor c) {
    CVarSetColor(cvar(o, "Value").c_str(), c);
    CVarSetInteger(cvar(o, "Changed").c_str(), 1);
    apply(o);
    CVarSaveNextFrame();
}

void reset(const Option& o) {
    CVarClear(cvar(o, "Value").c_str());
    CVarClear(cvar(o, "Changed").c_str());
    CVarClear(cvar(o, "Rainbow").c_str());
    CVarClear(cvar(o, "Locked").c_str());
    apply(o);
    CVarSaveNextFrame();
}

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

void randomize(const Option& o) {
    std::uniform_int_distribution<int> d(0, 255);
    set_color(o, CVarColor{static_cast<uint8_t>(d(rng())), static_cast<uint8_t>(d(rng())),
                           static_cast<uint8_t>(d(rng())), 255});
}

void set_flag(const Option& o, const char* field, bool on) {
    if (on)
        CVarSetInteger(cvar(o, field).c_str(), 1);
    else
        CVarClear(cvar(o, field).c_str());
    CVarSaveNextFrame();
}

// Rainbow: three phase-shifted sines over a cycle of 360 frames at speed 1
// (a higher Rainbow Speed is faster here: frames per step = 1 / speed).
CVarColor rainbow_color(size_t index, uint64_t frame) {
    const float speed = std::clamp(CVarGetFloat("gCosmetics.RainbowSpeed", 0.6f), 0.01f, 1.0f);
    const bool sync = CVarGetInteger("gCosmetics.RainbowSync", 0) != 0;
    const float t = static_cast<float>(frame) * speed / 360.0f * 6.2831853f + (sync ? 0.f : index * 0.7f);
    const auto channel = [&](float phase) {
        return static_cast<uint8_t>(std::lround((std::sin(t + phase) * 0.5f + 0.5f) * 255.0f));
    };
    return CVarColor{channel(0.f), channel(2.0944f), channel(4.1888f), 255};
}

void for_all(Group group, bool skip_locked, void (*fn)(const Option&)) {
    for (const Option& o : options())
        if ((group == GROUP_COUNT || o.group == group) && !(skip_locked && locked(o)))
            fn(o);
}

// A row: swatch and label, Random, Rainbow, Locked, Reset (while changed).
void draw_row(const Option& o, float label_width, float scale) {
    ImGui::PushID(o.id);
    CVarColor c = value(o);
    float rgb[3] = {c.r / 255.f, c.g / 255.f, c.b / 255.f};
    if (ImGui::ColorEdit3("##color", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        set_color(o, CVarColor{static_cast<uint8_t>(std::lround(rgb[0] * 255)),
                               static_cast<uint8_t>(std::lround(rgb[1] * 255)),
                               static_cast<uint8_t>(std::lround(rgb[2] * 255)), 255});
    ImGui::SameLine();
    ImGui::TextUnformatted(o.label);
    ImGui::SameLine(label_width);
    if (ImGui::Button("Random", ImVec2(80 * scale, 0)))
        randomize(o);
    ImGui::SameLine();
    bool r = rainbow(o);
    if (ImGui::Checkbox("Rainbow", &r)) {
        set_flag(o, "Rainbow", r);
        if (r)
            CVarSetInteger(cvar(o, "Changed").c_str(), 1);
        apply(o);
    }
    ImGui::SameLine();
    bool l = locked(o);
    if (ImGui::Checkbox("Locked", &l))
        set_flag(o, "Locked", l);
    if (changed(o)) {
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(80 * scale, 0)))
            reset(o);
    }
    ImGui::PopID();
}

// wwrando's Link color presets (assets/link_metadata.txt of the wwrando release the randomizer uses, MIT, LagoLunatic), in its
// order: each lists only the colors it changes. Red, Blue and Purple also come without their hair change, below the original.
struct StockPreset {
    const char* name;
    std::vector<std::pair<const char*, uint32_t>> hero, casual;
};
const std::vector<StockPreset>& stock_presets() {
    static const std::vector<StockPreset> presets = {
        {"Dark Link",
         {{"Hair", 0x000000}, {"Skin", 0x000000}, {"Mouth", 0x000000}, {"Eyes", 0xFF0000}, {"Sclera", 0x000000}, {"Hat", 0x000000}, {"Tunic", 0x000000}, {"Undershirt", 0x000000}, {"Pants", 0x000000}, {"Boots", 0x000000}, {"Belt", 0x000000}, {"Belt Buckle", 0x000000}},
         {{"Hair", 0x000000}, {"Skin", 0x000000}, {"Mouth", 0x000000}, {"Eyes", 0xFF0000}, {"Sclera", 0x000000}, {"Shirt", 0x000000}, {"Shirt Emblem", 0x000000}, {"Armbands", 0x000000}, {"Pants", 0x000000}, {"Shoes", 0x000000}, {"Shoe Soles", 0x000000}}},
        {"Smash Dark Link",
         {{"Hair", 0xD8D9DB}, {"Skin", 0x736B69}, {"Mouth", 0x736B69}, {"Eyes", 0xFF0000}, {"Sclera", 0x000000}, {"Hat", 0x333333}, {"Tunic", 0x333333}, {"Undershirt", 0x62656A}, {"Pants", 0x898E91}, {"Boots", 0x464646}, {"Belt", 0xA4A09D}, {"Belt Buckle", 0xBABB93}},
         {{"Hair", 0xD8D9DB}, {"Skin", 0x736B69}, {"Mouth", 0x736B69}, {"Eyes", 0xFF0000}, {"Sclera", 0x000000}, {"Shirt", 0x333333}, {"Shirt Emblem", 0xD8D9DB}, {"Armbands", 0x736B69}, {"Pants", 0x898E91}, {"Shoes", 0x464646}, {"Shoe Soles", 0x464646}}},
        {"Green Link",
         {{"Hair", 0x9CF543}},
         {{"Hair", 0x9CF543}, {"Shirt", 0x5AB24A}, {"Pants", 0xADE342}, {"Armbands", 0x638F5A}}},
        {"Red Link",
         {{"Hair", 0xFF532E}, {"Hat", 0xE52B36}, {"Tunic", 0xE52B36}, {"Undershirt", 0xE38981}},
         {{"Hair", 0xFF532E}, {"Shirt", 0xE52B36}, {"Pants", 0xE38981}, {"Armbands", 0x8F5D5A}}},
        {"Red Link (Default Hair)",
         {{"Hat", 0xE52B36}, {"Tunic", 0xE52B36}, {"Undershirt", 0xE38981}},
         {{"Shirt", 0xE52B36}, {"Pants", 0xE38981}, {"Armbands", 0x8F5D5A}}},
        {"Blue Link",
         {{"Hair", 0x3F64DB}, {"Hat", 0x698DB9}, {"Tunic", 0x698DB9}, {"Undershirt", 0xAAD3E0}},
         {{"Hair", 0x3F64DB}, {"Shirt", 0x698DB9}, {"Pants", 0xAAD3E0}, {"Armbands", 0x5B8391}}},
        {"Blue Link (Default Hair)",
         {{"Hat", 0x698DB9}, {"Tunic", 0x698DB9}, {"Undershirt", 0xAAD3E0}},
         {{"Shirt", 0x698DB9}, {"Pants", 0xAAD3E0}, {"Armbands", 0x5B8391}}},
        {"Purple Link",
         {{"Hair", 0xD649FD}, {"Hat", 0x9A4F9F}, {"Tunic", 0x9A4F9F}, {"Undershirt", 0xCC8FB1}},
         {{"Hair", 0xD649FD}, {"Shirt", 0x9A4F9F}, {"Pants", 0xCC8FB1}, {"Armbands", 0x8B5A8F}}},
        {"Purple Link (Default Hair)",
         {{"Hat", 0x9A4F9F}, {"Tunic", 0x9A4F9F}, {"Undershirt", 0xCC8FB1}},
         {{"Shirt", 0x9A4F9F}, {"Pants", 0xCC8FB1}, {"Armbands", 0x8B5A8F}}},
        {"Shadow Link",
         {{"Hair", 0xA3338D}, {"Hat", 0x444444}, {"Tunic", 0x444444}, {"Undershirt", 0xFFFFFF}, {"Boots", 0x392437}, {"Belt", 0x791009}},
         {{"Hair", 0xA3338D}, {"Shirt", 0x444444}, {"Shirt Emblem", 0xFFFFFF}, {"Pants", 0xFFFFFF}, {"Shoes", 0x392437}, {"Shoe Soles", 0x392437}}},
    };
    return presets;
}

// A preset (null: the outfit's own colors) on an outfit: each color takes the preset's, or its own.
using Colors = std::vector<std::pair<std::string, uint32_t>>;
void apply_colors(Group g, const Colors* preset) {
    for (const Option& o : options()) {
        if (o.group != g || locked(o))
            continue;
        uint32_t rgb = o.color;
        if (preset != nullptr)
            for (const auto& p : *preset)
                if (p.first == o.label)
                    rgb = p.second;
        if (rgb != o.color)
            set_color(o, CVarColor{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
                                   static_cast<uint8_t>(rgb), 255});
        else
            reset(o);
    }
}

// The presets of an outfit: a model's own, or wwrando's for Link.
void draw_presets(Group g, float scale) {
    ImGui::SetNextItemWidth(250 * scale);
    if (!ImGui::BeginCombo("Preset", "Choose a preset"))
        return;
    if (ImGui::Selectable("Default"))
        apply_colors(g, nullptr);
    if (g_model) {
        for (const model_colors::Preset& p : g_model->outfits[g == CASUAL ? 1 : 0].presets)
            if (ImGui::Selectable(p.name.c_str()))
                apply_colors(g, &p.colors);
    } else {
        for (const StockPreset& p : stock_presets()) {
            if (!ImGui::Selectable(p.name))
                continue;
            Colors colors;
            for (const auto& c : g == CASUAL ? p.casual : p.hero)
                colors.emplace_back(c.first, c.second);
            apply_colors(g, &colors);
        }
    }
    ImGui::EndCombo();
}

// The Bunny Hood's presets: body, inner ears, band. "Default" gives the model's own colours back.
struct HoodPreset {
    const char* name;
    uint32_t body, ears, band;
};
// "Default" (the combo's first row) restores the model's own colours; this is the only other preset. MM Styled follows the
// Majora's Mask / Ocarina of Time Bunny Hood: a tan hood and outer ears, white inner ears, a dark brown headband (the eyes are
// the model's own black ovals with a white highlight, not recolourable).
const HoodPreset kHoodPresets[] = {
    {"MM Styled", 0xD2B48C, 0xF6F0EA, 0x4A3424},
};

void apply_hood_preset(const HoodPreset* preset) {
    for (const Option& o : options()) {
        if (o.group != HOOD || locked(o))
            continue;
        if (preset == nullptr) {
            reset(o);
            continue;
        }
        const uint32_t rgb = std::strcmp(o.label, "Body") == 0 ? preset->body
                             : std::strcmp(o.label, "Inner Ears") == 0 ? preset->ears : preset->band;
        set_color(o, CVarColor{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
                               static_cast<uint8_t>(rgb), 255});
    }
}

// The preset the three hood colours stand at: Default when none is set, a preset's name when they are its colours.
const char* current_hood_preset() {
    uint8_t c[3][4];
    const char* const ids[3] = {"BunnyHood.Body", "BunnyHood.InnerEars", "BunnyHood.Band"};
    int set = 0;
    for (int i = 0; i < 3; ++i)
        set += bluewake_cosmetics_get(ids[i], c[i]) ? 1 : 0;
    if (set == 0)
        return "Default";
    if (set == 3)
        for (const HoodPreset& p : kHoodPresets) {
            const uint32_t want[3] = {p.body, p.ears, p.band};
            bool same = true;
            for (int i = 0; i < 3; ++i)
                same &= c[i][0] == (want[i] >> 16 & 0xFF) && c[i][1] == (want[i] >> 8 & 0xFF) && c[i][2] == (want[i] & 0xFF);
            if (same)
                return p.name;
        }
    return "Custom";
}

void draw_hood_presets(float scale) {
    ImGui::SetNextItemWidth(250 * scale);
    if (ImGui::BeginCombo("Preset", current_hood_preset())) {
        if (ImGui::Selectable("Default"))
            apply_hood_preset(nullptr);
        for (const HoodPreset& p : kHoodPresets)
            if (ImGui::Selectable(p.name))
                apply_hood_preset(&p);
        ImGui::EndCombo();
    }
}

// The boat's presets: hull, figurehead, sail. "Default" gives the game's own colours back.
struct ShipPreset {
    const char* name;
    uint32_t hull, head, sail;
};
const ShipPreset kShipPresets[] = {
    {"Royal Blue", 0x2F5FB8, 0x2F5FB8, 0xD2B040},
    {"Forest Green", 0x3F8F3C, 0x3F8F3C, 0xB8302A},
    {"Pirate Purple", 0x7A3FA8, 0x7A3FA8, 0xD8A030},
};

void apply_ship_preset(const ShipPreset* preset) {
    for (const Option& o : options()) {
        if (o.group != SHIP || locked(o))
            continue;
        if (preset == nullptr) {
            reset(o);
            continue;
        }
        const uint32_t rgb = std::strcmp(o.label, "Hull") == 0 ? preset->hull
                             : std::strcmp(o.label, "Figurehead") == 0 ? preset->head : preset->sail;
        set_color(o, CVarColor{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
                               static_cast<uint8_t>(rgb), 255});
    }
}

const char* current_ship_preset() {
    uint8_t c[3][4];
    const char* const ids[3] = {"Equip.Ship.Hull", "Equip.Ship.Head", "Equip.Ship.Sail"};
    int set = 0;
    for (int i = 0; i < 3; ++i)
        set += bluewake_cosmetics_get(ids[i], c[i]) ? 1 : 0;
    if (set == 0)
        return "Default";
    if (set == 3)
        for (const ShipPreset& p : kShipPresets) {
            const uint32_t want[3] = {p.hull, p.head, p.sail};
            bool same = true;
            for (int i = 0; i < 3; ++i)
                same &= c[i][0] == (want[i] >> 16 & 0xFF) && c[i][1] == (want[i] >> 8 & 0xFF) && c[i][2] == (want[i] & 0xFF);
            if (same)
                return p.name;
        }
    return "Custom";
}

void draw_ship_presets(float scale) {
    ImGui::SetNextItemWidth(250 * scale);
    if (ImGui::BeginCombo("Preset", current_ship_preset())) {
        if (ImGui::Selectable("Default"))
            apply_ship_preset(nullptr);
        for (const ShipPreset& p : kShipPresets)
            if (ImGui::Selectable(p.name))
                apply_ship_preset(&p);
        ImGui::EndCombo();
    }
}

// Tetra's and Zelda's presets: colors in the group's row order (Tetra: Hair, Skin, Eyes, Sclera, Lips, Bandana, Shirt, Trim,
// Leather, Gold; Zelda: Hair, Skin, Eyes, Sclera, Lips, Eyeshadow, Dress, Gold, Blue, Cloth, Emblem).
struct CharPreset {
    Group group;
    const char* name;
    uint32_t c[11];
};
const CharPreset kCharPresets[] = {
    {TETRA, "Blue Bandana", {0xC6FF00, 0xB59A5A, 0x104770, 0xEFF7FF, 0x997C42, 0x2F5FB8, 0xEFF7FF, 0xB8302A, 0x5A3C18, 0xEFBE00}},
    {TETRA, "Pirate Black", {0x303030, 0xB59A5A, 0x8B1A1A, 0xEFF7FF, 0x7A3A3A, 0x3A3A3A, 0xE8E8E8, 0xB8302A, 0x6A4A2A, 0xC0C0C0}},
    {TETRA, "Moonlight", {0xE8E8FF, 0xD9C8E8, 0x40D0FF, 0xEFF7FF, 0xAA5090, 0x6A3FD0, 0xC8D8FF, 0x202070, 0x303060, 0xFFFFFF}},
    {ZELDA, "Royal Blue", {0xC6F700, 0xFFE3D6, 0x104573, 0xEFF7FF, 0xD6A2AD, 0x9CC7EF, 0x9FB4E8, 0xCEB600, 0x4271AD, 0xE8E8E8, 0xEFF7FF}},
    {ZELDA, "Crimson", {0xC6F700, 0xFFE3D6, 0x104573, 0xEFF7FF, 0xD6A2AD, 0x9CC7EF, 0xE79AA0, 0xC7AF03, 0x8A1F2B, 0xDEE7EF, 0xEFF7FF}},
    {ZELDA, "Forest Queen", {0x8A4A20, 0xFFE3D6, 0x30A030, 0xEFF7FF, 0xD6A2AD, 0x60C080, 0x8FD08F, 0xE0E0E0, 0x2F7A4A, 0xF0FFF0, 0xFFE070}},
};

void apply_character_preset(Group g, const CharPreset* preset) {
    int row = 0;
    for (const Option& o : options()) {
        if (o.group != g)
            continue;
        const int i = row++;
        if (locked(o))
            continue;
        if (preset == nullptr) {
            reset(o);
            continue;
        }
        const uint32_t rgb = preset->c[i];
        set_color(o, CVarColor{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
                               static_cast<uint8_t>(rgb), 255});
    }
}

const char* current_character_preset(Group g) {
    int rows = 0, set = 0;
    bool same[std::size(kCharPresets)];
    std::fill(std::begin(same), std::end(same), true);
    for (const Option& o : options()) {
        if (o.group != g)
            continue;
        uint8_t c[4];
        const bool has = bluewake_cosmetics_get(o.id, c);
        set += has ? 1 : 0;
        for (size_t k = 0; k < std::size(kCharPresets); ++k) {
            const uint32_t w = kCharPresets[k].c[rows];
            same[k] = same[k] && has && c[0] == (w >> 16 & 0xFF) && c[1] == (w >> 8 & 0xFF) && c[2] == (w & 0xFF);
        }
        ++rows;
    }
    if (set == 0)
        return "Default";
    for (size_t k = 0; k < std::size(kCharPresets); ++k)
        if (kCharPresets[k].group == g && same[k])
            return kCharPresets[k].name;
    return "Custom";
}

void draw_character_presets(Group g, float scale) {
    ImGui::SetNextItemWidth(250 * scale);
    if (ImGui::BeginCombo("Preset", current_character_preset(g))) {
        if (ImGui::Selectable("Default"))
            apply_character_preset(g, nullptr);
        for (const CharPreset& p : kCharPresets)
            if (p.group == g && ImGui::Selectable(p.name))
                apply_character_preset(g, &p);
        ImGui::EndCombo();
    }
}

void draw_group(Group g, float label_width, float scale) {
    // The model may have no Outset clothes, and names them (28 characters at most) when it has.
    if (g_model && g == CASUAL && g_model->disable_casual)
        return;
    ImGui::PushID(g);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(g_model && g == CASUAL && !g_model->casual_text.empty() ? g_model->casual_text.c_str()
                                                                                  : kGroupLabels[g]);
    ImGui::SameLine(label_width);
    if (ImGui::Button("Random", ImVec2(80 * scale, 0)))
        for_all(g, true, randomize);
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(80 * scale, 0)))
        for_all(g, true, reset);
    if (g == HERO || g == CASUAL)
        draw_presets(g, scale);
    if (g == HOOD)
        draw_hood_presets(scale);
    if (g == SHIP)
        draw_ship_presets(scale);
    if (g == TETRA || g == ZELDA)
        draw_character_presets(g, scale);
    for (const Option& o : options())
        if (o.group == g)
            draw_row(o, label_width, scale);
    ImGui::PopID();
}

// Link Voice Pitch: a percent change of the frequency of Link's voice (and, over Anchor, of how others hear it).
constexpr int kVoiceMin = -50, kVoiceMax = 100;
void set_voice_pitch(int percent) {
    percent = std::clamp(percent, kVoiceMin, kVoiceMax);
    CVarSetInteger("gCosmetics.LinkVoicePitch", percent);
    bluewake_voice_pitch_set_local(1.0f + float(percent) / 100.0f);
    CVarSaveNextFrame();
}

void draw_voice(float scale) {
    struct VoicePreset {
        const char* name;
        int percent;
    };
    static const VoicePreset kPresets[] = {{"Normal (Green Link)", 0},
                                           {"10% Higher (Red Link)", 10},
                                           {"10% Deeper (Blue Link)", -10},
                                           {"15% Deeper (Violet Link)", -15}};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Link Voice Pitch");
    int percent = std::clamp(CVarGetInteger("gCosmetics.LinkVoicePitch", 0), kVoiceMin, kVoiceMax);
    ImGui::SetNextItemWidth(250 * scale);
    if (ImGui::SliderInt("Pitch Change (%)", &percent, kVoiceMin, kVoiceMax, "%+d%%"))
        set_voice_pitch(percent);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Changes the pitch of Link's voice by this percent. Ctrl+click to type a value. Other players hear\nyour voice at this pitch too while Share Custom Player Content is on.");
    const char* shown = "Choose a preset";
    for (const VoicePreset& p : kPresets)
        if (p.percent == percent)
            shown = p.name;
    ImGui::SetNextItemWidth(250 * scale);
    if (ImGui::BeginCombo("Voice Preset", shown)) {
        for (const VoicePreset& p : kPresets)
            if (ImGui::Selectable(p.name, p.percent == percent))
                set_voice_pitch(p.percent);
        ImGui::EndCombo();
    }
}

// Player Gender (Male/Female): how the game's text refers to the player (runtime/host/src/text_swap.c), live.
void set_player_gender(int gender) {
    CVarSetInteger("gCosmetics.PlayerGender", gender != 0 ? 1 : 0);
    bluewake_text_swap_set_female(gender != 0);
    CVarSaveNextFrame();
}

void draw_gender(float scale) {
    static const char* const kGenders[] = {"Male", "Female"};
    int gender = CVarGetInteger("gCosmetics.PlayerGender", 0) != 0 ? 1 : 0;
    ImGui::SetNextItemWidth(250 * scale);
    if (ImGui::BeginCombo("Player Gender", kGenders[gender])) {
        for (int i = 0; i < 2; ++i)
            if (ImGui::Selectable(kGenders[i], i == gender))
                set_player_gender(i);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For use with custom player models; Changes how NPCs refer to the player.");
}

// Testing (BLUEWAKE_TEST_SET gTest.CosmeticsPreset=hero:Red Link): a stock preset by its name, as the Preset list applies it.
void apply_stock_preset_named(const std::string& spec) {
    const size_t colon = spec.find(':');
    if (colon == std::string::npos)
        return;
    if (spec.compare(0, colon, "equip") == 0) { // equip:Equip.SkullHammer.Body=ff0000;Equip.Hookshot.Body=00ff00
        for (size_t at = colon + 1; at < spec.size();) {
            const size_t end = std::min(spec.find(';', at), spec.size()), eq = spec.find('=', at);
            if (eq == std::string::npos || eq > end)
                break;
            const std::string id = spec.substr(at, eq - at);
            const unsigned long rgb = std::strtoul(spec.substr(eq + 1, end - eq - 1).c_str(), nullptr, 16);
            for (const Option& o : options())
                if (id == o.id) {
                    set_color(o, CVarColor{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb), 255});
                    std::fprintf(stderr, "[test] %s set to %06lx\n", o.id, rgb);
                }
            at = end + 1;
        }
        return;
    }
    if (spec.compare(0, colon, "hood") == 0) { // hood:MM Styled
        for (const HoodPreset& p : kHoodPresets)
            if (spec.compare(colon + 1, std::string::npos, p.name) == 0) {
                apply_hood_preset(&p);
                std::fprintf(stderr, "[test] cosmetics preset %s applied to the Bunny Hood\n", p.name);
            }
        return;
    }
    const bool casual = spec.compare(0, colon, "casual") == 0;
    const std::string name = spec.substr(colon + 1);
    for (const StockPreset& p : stock_presets()) {
        if (name != p.name)
            continue;
        Colors colors;
        for (const auto& c : casual ? p.casual : p.hero)
            colors.emplace_back(c.first, c.second);
        apply_colors(casual ? CASUAL : HERO, &colors);
        std::fprintf(stderr, "[test] cosmetics preset %s applied to the %s outfit\n", p.name, casual ? "casual" : "hero");
    }
}

void embed_window(float scale) {
    float speed = CVarGetFloat("gCosmetics.RainbowSpeed", 0.6f);
    ImGui::SetNextItemWidth(250 * scale);
    if (ImGui::SliderFloat("Rainbow Speed", &speed, 0.01f, 1.0f, "%.2f")) {
        CVarSetFloat("gCosmetics.RainbowSpeed", speed);
        CVarSaveNextFrame();
    }
    bool sync = CVarGetInteger("gCosmetics.RainbowSync", 0) != 0;
    if (ImGui::Checkbox("Sync Rainbow Colors", &sync)) {
        CVarSetInteger("gCosmetics.RainbowSync", sync ? 1 : 0);
        CVarSaveNextFrame();
    }
    const ImVec2 wide(250 * scale, 0);
    if (ImGui::Button("Randomize All", wide))
        for_all(GROUP_COUNT, true, randomize);
    ImGui::SameLine();
    if (ImGui::Button("Reset All", wide))
        for_all(GROUP_COUNT, true, reset);
    if (ImGui::Button("Lock All", wide))
        for_all(GROUP_COUNT, false, [](const Option& o) { set_flag(o, "Locked", true); });
    ImGui::SameLine();
    if (ImGui::Button("Unlock All", wide))
        for_all(GROUP_COUNT, false, [](const Option& o) { set_flag(o, "Locked", false); });
    if (ImGui::Button("Rainbow All", wide))
        for_all(GROUP_COUNT, true, [](const Option& o) {
            set_flag(o, "Rainbow", true);
            CVarSetInteger(cvar(o, "Changed").c_str(), 1);
        });
    ImGui::SameLine();
    if (ImGui::Button("Un-Rainbow All", wide))
        for_all(GROUP_COUNT, true, [](const Option& o) {
            set_flag(o, "Rainbow", false);
            apply(o);
        });
    ImGui::Separator();
    float label_width = ImGui::CalcTextSize("Hero's Shield Back and Grip").x + 60 * scale;
    for (const Option& o : options())
        if (o.model)
            label_width = std::max(label_width, ImGui::CalcTextSize(o.label).x + 60 * scale);
    if (ImGui::BeginTabBar("CosmeticsTabs")) {
        for (const Tab& tab : kTabs) {
            if (tab.groups[0] == HOOD && !bluewake_bunny_hood_enabled())
                continue; // experimental (gExperimental.BunnyHood)
            // Testing: BLUEWAKE_TEST_COSMETICS_TAB=Bunny Hood opens that tab (the scenarios cannot click).
            static const char* const test_tab = std::getenv("BLUEWAKE_TEST_COSMETICS_TAB");
            static bool test_tab_done;
            const bool pick = test_tab != nullptr && !test_tab_done && std::strcmp(test_tab, tab.label) == 0;
            if (ImGui::BeginTabItem(tab.label, nullptr, pick ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (pick)
                    test_tab_done = true;
                if (g_model && tab.groups[0] == HERO) {
                    ImGui::TextWrapped("Player model: %s%s%s", g_model->name.c_str(),
                                       g_model->author.empty() ? "" : " by ", g_model->author.c_str());
                    if (g_model->outfits[0].colors.empty() && g_model->outfits[1].colors.empty())
                        ImGui::TextDisabled("This model has no custom colors (no metadata.txt).");
                    ImGui::Separator();
                }
                if (tab.groups[0] == HERO) {
                    draw_voice(scale);
                    ImGui::Separator();
                    draw_gender(scale);
                    ImGui::Separator();
                }
                for (size_t i = 0; i < tab.groups.size(); ++i) {
                    if (i != 0)
                        ImGui::Separator();
                    draw_group(tab.groups[i], label_width, scale);
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

void draw_window(float scale, float fontScale) {
    shipmenu::PopoutWindow("Cosmetics Editor", "gOpenWindows.CosmeticsEditor", ImVec2(620 * scale, 560 * scale), 0.97f, fontScale,
                           [&] { embed_window(scale); });
}

} // namespace

void TestApplyPreset(const std::string& spec) { apply_stock_preset_named(spec); }

void ApplyAll() {
    bluewake_text_swap_set_female(CVarGetInteger("gCosmetics.PlayerGender", 0) != 0);
    bluewake_voice_pitch_set_local(1.0f + float(std::clamp(CVarGetInteger("gCosmetics.LinkVoicePitch", 0), -50, 100)) / 100.0f);
    for (const Option& o : options())
        apply(o);
}

namespace {

// The player model changed (an Apply in the mods list): its colors replace Link's in the Link tab and in
// the game; each model keeps its own palette in its CVars.
void sync_model() {
    static bool listening;
    if (!listening) {
        listening = true;
        mods::SetModelListener([] { g_model_changed = true; });
    }
    if (!g_model_changed.exchange(false))
        return;
    model_colors::Files files;
    std::unique_ptr<model_colors::Set> set;
    if (mods::ActiveModelFiles(files)) {
        std::string error;
        set = std::make_unique<model_colors::Set>();
        if (!model_colors::Build(files, *set, &error)) {
            std::fprintf(stderr, "[cosmetics] %s: no colors: %s\n", files.name.c_str(), error.c_str());
            set.reset();
        } else {
            for (const std::string& w : set->warnings)
                std::fprintf(stderr, "[cosmetics] %s: %s\n", files.name.c_str(), w.c_str());
            std::fprintf(stderr, "[cosmetics] %s: %zu Hero's and %zu Outset colors, %zu textures, %zu masks\n",
                         files.name.c_str(), set->outfits[0].colors.size(), set->outfits[1].colors.size(),
                         set->textures.size(), set->masks.size());
        }
    }
    std::unique_ptr<model_colors::Set> old = std::move(g_model);
    g_model = std::move(set);
    link_colors::UseModel(g_model.get());
    {   // the equipment's arc when it is a mod's: the model's own Link.arc, or the Equipment part's mod
        std::string name;
        std::vector<mods::PackFile> pack;
        mods::SharedParts parts;
        const std::vector<uint8_t>* equip = nullptr;
        std::vector<uint8_t> bytes;
        if (mods::ActivePlayerPack(name, pack, parts) && !parts.equipment.empty())
            for (const mods::PackFile& f : pack)
                if (f.role == parts.equipment) {  // "model" or "equipment"
                    bytes.assign(f.bytes.begin(), f.bytes.end());
                    rarc::Yaz0Expand(bytes);
                    equip = &bytes;
                }
        link_colors::UseEquipmentArc(equip);
    }
    rebuild_options();
    ApplyAll();
}

} // namespace

void Frame(float scale, float fontScale) {
    static uint64_t frame;
    ++frame;
    bluewake_bunny_hood_enable(CVarGetInteger("gExperimental.BunnyHood", 1) != 0);
    sync_model();
    const std::vector<Option>& all = options();
    for (size_t i = 0; i < all.size(); ++i) {
        const Option& o = all[i];
        if (rainbow(o)) {
            const CVarColor c = rainbow_color(i, frame);
            push(o, c, true);
            if (uses_link_colors(o))
                link_colors::MarkDirty();
        }
    }
    link_colors::Frame();
    if (CVarGetInteger("gOpenWindows.CosmeticsEditor", 0))
        draw_window(scale, fontScale);
}

void DrawEmbedded(float scale) { embed_window(scale); }

} // namespace cosmetics
