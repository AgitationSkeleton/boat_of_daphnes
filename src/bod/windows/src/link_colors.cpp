// Link's custom colors: see link_colors.h.
// The Windows build force-includes a POSIX layer whose rename() macro breaks
// <filesystem>; this file has no use for it.
#ifdef rename
#undef rename
#endif

#include "link_colors.h"

#include "model_colors.h"
#include "gx_texture.h"
#include "rarc.h"

#define PICOJSON_USE_INT64
#include "picojson.h"

#include <aurora/texture.hpp>
#include <dolphin/gx/GXEnum.h>
#include <SDL3/SDL.h>

#include "platform/bod_platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

extern "C" bool bluewake_cosmetics_get(const char* id, uint8_t rgba[4]);

namespace link_colors {
namespace {

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bytes; // RGBA8, or one byte a pixel for a mask
};

struct Texture {
    std::string name;
    aurora::texture::TextureSourceKey key;
    Image base;
    // The recolored pixels while registered: the renderer keeps a view of
    // them, not a copy, so they live here until the registration is removed.
    std::vector<uint8_t> live;
    std::optional<aurora::texture::ReplacementRegistration> registered;
};

struct Mask {
    bool casual = false;
    std::string color; // wwrando's name, "Belt Buckle"
    std::string texture;
    Image image;
};

// wwrando's base colors (assets/link_metadata.txt): what each region is in
// the original textures, the reference the exchange shifts from. The editor's
// ids are "Link.<Hero|Casual>.<name without spaces>".
struct BaseColor {
    bool casual;
    const char* name;
    uint32_t rgb;
};
const BaseColor kBase[] = {
    {false, "Hair", 0xFFEF10},  {false, "Skin", 0xF7DB9C},       {false, "Mouth", 0xF74963},
    {false, "Eyes", 0x10514A},  {false, "Sclera", 0xFFFFFF},     {false, "Hat", 0x5AB24A},
    {false, "Tunic", 0x5AB24A}, {false, "Undershirt", 0xADE342}, {false, "Pants", 0xFFFFFF},
    {false, "Boots", 0x944908}, {false, "Belt", 0x633008},       {false, "Belt Buckle", 0xFFEF10},
    {true, "Hair", 0xFFEF10},   {true, "Skin", 0xF7DB9C},        {true, "Mouth", 0xF74963},
    {true, "Eyes", 0x10514A},   {true, "Sclera", 0xFFFFFF},      {true, "Shirt", 0x4A75AD},
    {true, "Shirt Emblem", 0xFFFFD6}, {true, "Armbands", 0x63696B}, {true, "Pants", 0xFFA608},
    {true, "Shoes", 0x63696B},  {true, "Shoe Soles", 0xD6BA39},
};

// Equipment (swords and shields): whole textures whose hue shifts from a base color to the chosen one.
// The ids are the Cosmetics Editor's ("Equip.<Item>.<Part>"); the base is the part's color in the game.
struct EquipPart {
    const char* id;
    uint32_t base;
    std::vector<const char*> textures;
    float window = 0.f; // see exchange
    float min_sat = 25.f;
};
const EquipPart kEquip[] = {
    {"Equip.HeroSword.Blade", 0xF0F5D8, {"SWAS3TC"}},
    {"Equip.HeroSword.Hilt", 0xBAA674, {"SWgripA"}},
    {"Equip.MasterSword.Blade", 0xE8E8E8, {"SWMSA", "SWMSBS3TC"}},
    {"Equip.MasterSword.Hilt", 0x476BC7, {"SWgripAMSCI4", "SWgripBMS"}},
    {"Equip.HeroShield.Face", 0x986951, {"SHA"}},
    {"Equip.HeroShield.Back", 0xC89D68, {"SHinA", "SHgripAS3TC"}},
    {"Equip.MirrorShield.Face", 0xD5D7C3, {"SHMSCCI8"}},
    {"Equip.MirrorShield.Frame", 0x4B6EC9, {"SHMSACI4", "SHMSBI4"}},
    // The King of Red Lions (Ship.arc): the hull's and figurehead's red wood, the sail's teal picture.
    {"Equip.Ship.Hull", 0xC44B29, {"ShipHull"}, 30.f},
    {"Equip.Ship.Head", 0xC84321, {"ShipHead"}, 30.f},
    {"Equip.Ship.Sail", 0x236E6B, {"ShipSail"}, 40.f},
    // the rest of Link's gear
    {"Equip.Scabbard.Body", 0x926B48, {"podAS3TC"}},
    {"Equip.MasterScabbard.Body", 0x4467C1, {"podMSCI4"}},
    {"Equip.Bow.Body", 0xDDA308, {"BowS3TC"}},
    {"Equip.Arrow.Body", 0xC2A978, {"arrowS3TC"}},
    {"Equip.Boomerang.Body", 0xEFF054, {"Boomerang"}},
    {"Equip.Hookshot.Body", 0x769BF6, {"hookshotCI8"}},
    {"Equip.DekuLeaf.Leaf", 0x14941B, {"fanAS3TC", "fanBS3TC", "fanCS3TC"}},
    {"Equip.Bomb.Body", 0xF8B740, {"Bomb"}},
    {"Equip.Telescope.Body", 0xC56121, {"Telescoe"}},
    {"Equip.PowerBracelets.Body", 0xEEE349, {"PRingS3TC"}},
};
std::vector<Texture> g_equip; // the textures of kEquip, from index.json "equipment"

// A mod's equipment (UseEquipmentArc): each equipment model of its Link.arc and the part its textures take. A texture named
// as the game's goes to that texture's part (the shields' faces and backs); any other texture of the model, to the model's.
struct EquipModel {
    const char* member;
    const char* part;
};
const EquipModel kEquipModels[] = {
    {"swa.bdl", "Equip.HeroSword.Blade"},       {"swgripa.bdl", "Equip.HeroSword.Hilt"},
    {"swms.bdl", "Equip.MasterSword.Blade"},    {"swgripms.bdl", "Equip.MasterSword.Hilt"},
    {"sha.bdl", "Equip.HeroShield.Face"},       {"shms.bdl", "Equip.MirrorShield.Face"},
    {"podms.bdl", "Equip.MasterScabbard.Body"}, {"bow.bdl", "Equip.Bow.Body"},
    {"arrow.bdl", "Equip.Arrow.Body"},          {"boomerang.bdl", "Equip.Boomerang.Body"},
    {"hookshot.bdl", "Equip.Hookshot.Body"},    {"fan.bdl", "Equip.DekuLeaf.Leaf"},
    {"fanb.bdl", "Equip.DekuLeaf.Leaf"},        {"fansmall.bdl", "Equip.DekuLeaf.Leaf"},
    {"bomb.bdl", "Equip.Bomb.Body"},            {"telescope.bdl", "Equip.Telescope.Body"},
    {"pring.bdl", "Equip.PowerBracelets.Body"},
};
// Textures of those models that are effects or lighting, never coloured.
bool equip_skip(const std::string& name) {
    for (const char* n : {"ZBtoonEX", "highL", "toonICE", "tipIceCI4"})
        if (name == n) return true;
    return false;
}
struct ModEquip {
    Texture tex;
    const char* part = nullptr;
    uint32_t own = 0;  // its main colour
};
std::vector<ModEquip> g_mod_equip;

// Tetra and Princess Zelda (index.json "chars"): each texture has masks, one per part ("Equip.Tetra.Skin"), and the part's
// color in the original texture; the chosen color is shifted in exactly like Link's.
struct CharPart {
    std::string id;
    uint32_t base = 0;
    Image mask;
};
struct CharTexture {
    Texture tex;
    std::vector<CharPart> parts;
};
std::vector<CharTexture> g_chars;

bool g_loaded, g_tried, g_dirty;
Uint64 g_last_apply;
std::vector<Texture> g_textures;
std::vector<Mask> g_masks;

// A player model's recolored textures while registered (the renderer keeps a view of the pixels).
struct ModelLive {
    std::string label;
    std::vector<uint8_t> pixels;
    std::vector<aurora::texture::ReplacementRegistration> registered;
};
const model_colors::Set* g_model;
std::map<std::string, uint32_t> g_model_chosen; // model color id -> 0xRRGGBB, the colors that differ from the model's
std::vector<ModelLive> g_model_live;

std::filesystem::path assets_folder() {
    // The release makes Link's data from the player's disc on first launch, into the prepared game folder.
    if (const char* made = std::getenv("BLUEWAKE_EXTRAS_DIR")) {
        const std::filesystem::path folder = std::filesystem::u8path(made) / "assets" / "link_colors";
        std::error_code ec;
        if (std::filesystem::exists(folder / "index.json", ec))
            return folder;
    }
    const std::filesystem::path exe = bodplat::ExeFolder();
    return exe / "assets" / "link_colors";
}

bool read_image(const std::filesystem::path& path, uint32_t bytes_per_pixel, Image& out) {
    std::ifstream in(path, std::ios::binary);
    uint32_t header[2] = {};
    if (!in.read(reinterpret_cast<char*>(header), sizeof header))
        return false;
    out.width = header[0];
    out.height = header[1];
    if (out.width == 0 || out.height == 0 || out.width > 4096 || out.height > 4096)
        return false;
    out.bytes.resize(static_cast<size_t>(out.width) * out.height * bytes_per_pixel);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.bytes.data()), out.bytes.size()));
}

// "tex1_<W>x<H>_<hash>[_<tlut hash>]_<format>"
std::optional<aurora::texture::TextureSourceKey> parse_key(const std::string& s) {
    unsigned w = 0, h = 0, fmt = 0;
    unsigned long long hash = 0, tlut = 0;
    char tail[64] = {};
    if (std::sscanf(s.c_str(), "tex1_%ux%u_%16llx_%63s", &w, &h, &hash, tail) != 4)
        return std::nullopt;
    aurora::texture::TextureSourceKey key{};
    key.textureHash = hash;
    key.width = w;
    key.height = h;
    if (std::strchr(tail, '_') != nullptr) {
        if (std::sscanf(tail, "%16llx_%u", &tlut, &fmt) != 2)
            return std::nullopt;
        key.tlutHash = tlut;
        key.hasTlut = true;
    } else if (std::sscanf(tail, "%u", &fmt) != 1) {
        return std::nullopt;
    }
    key.format = fmt;
    return key;
}

// HSV as wwrando uses it: hue 0-359, saturation and value 0-100.
struct Hsv {
    float h, s, v;
};

Hsv to_hsv(float r, float g, float b) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    float h = 0.f;
    if (d > 0.f) {
        if (mx == r)
            h = std::fmod((g - b) / d, 6.f);
        else if (mx == g)
            h = (b - r) / d + 2.f;
        else
            h = (r - g) / d + 4.f;
        h *= 60.f;
        if (h < 0.f)
            h += 360.f;
    }
    return {h, mx > 0.f ? d / mx * 100.f : 0.f, mx * 100.f};
}

void from_hsv(Hsv c, uint8_t* out) {
    const float s = std::clamp(c.s, 0.f, 100.f) / 100.f, v = std::clamp(c.v, 0.f, 100.f) / 100.f;
    const float cc = v * s, x = cc * (1.f - std::fabs(std::fmod(c.h / 60.f, 2.f) - 1.f)), m = v - cc;
    float r = 0, g = 0, b = 0;
    const int sector = static_cast<int>(c.h / 60.f) % 6;
    switch (sector) {
    case 0: r = cc; g = x; break;
    case 1: r = x; g = cc; break;
    case 2: g = cc; b = x; break;
    case 3: g = x; b = cc; break;
    case 4: r = x; b = cc; break;
    default: r = cc; b = x; break;
    }
    out[0] = static_cast<uint8_t>(std::lround((r + m) * 255.f));
    out[1] = static_cast<uint8_t>(std::lround((g + m) * 255.f));
    out[2] = static_cast<uint8_t>(std::lround((b + m) * 255.f));
}

Hsv to_hsv(uint32_t rgb) { return to_hsv((rgb >> 16 & 255) / 255.f, (rgb >> 8 & 255) / 255.f, (rgb & 255) / 255.f); }

// wwrando's color_exchange for one region: each pixel takes the new hue, and
// its saturation and value move by the new color's difference from the base.
// A grey pixel first takes the base color's saturation. ignore_bright leaves
// pixels whose channels are all above 128 (an eye's white).
// window > 0: only pixels of the base color's hue (within window degrees) and at least 25 % saturated change, so a
// texture that mixes the part with other colors (the King of Red Lions' hull's patches, the sail's cream) keeps them.
void exchange(Image& img, const Image* mask, uint32_t base, uint32_t chosen, bool ignore_bright, float window = 0.f,
              float min_sat = 25.f, const Image* ref = nullptr) {
    const Hsv b = to_hsv(base), n = to_hsv(chosen);
    const size_t count = static_cast<size_t>(img.width) * img.height;
    for (size_t i = 0; i < count; ++i) {
        if (mask != nullptr && mask->bytes[i] == 0)
            continue;
        uint8_t* p = &img.bytes[i * 4];
        if (ignore_bright && p[0] > 128 && p[1] > 128 && p[2] > 128)
            continue;
        Hsv c = to_hsv(p[0] / 255.f, p[1] / 255.f, p[2] / 255.f);
        if (window > 0.f) {
            // The test reads the untouched texture (ref), so a part changed first does not fall into a later part's window.
            const uint8_t* q = ref != nullptr ? &ref->bytes[i * 4] : p;
            const Hsv w = to_hsv(q[0] / 255.f, q[1] / 255.f, q[2] / 255.f);
            float d = std::fabs(w.h - b.h);
            d = std::min(d, 360.f - d);
            if (d > window || w.s < min_sat)
                continue;
        }
        if (c.s == 0.f)
            c.s = b.s;
        c.h = n.h;
        c.s += n.s - b.s;
        c.v += n.v - b.v;
        from_hsv(c, p);
    }
}

std::string id_of(bool casual, const std::string& name) {
    std::string compact;
    for (char ch : name)
        if (ch != ' ')
            compact += ch;
    return std::string("Link.") + (casual ? "Casual." : "Hero.") + compact;
}

// The chosen color for a region, when it differs from the game's own.
const std::map<std::string, uint32_t>* g_override; // a puppet's colors while its textures are made (id -> 0xRRGGBB)

std::optional<uint32_t> chosen(bool casual, const char* name, uint32_t base) {
    if (g_override != nullptr) {
        const auto it = g_override->find(id_of(casual, name));
        if (it == g_override->end() || it->second == base)
            return std::nullopt;
        return it->second;
    }
    uint8_t rgba[4];
    if (!bluewake_cosmetics_get(id_of(casual, name).c_str(), rgba))
        return std::nullopt;
    const uint32_t rgb = (uint32_t(rgba[0]) << 16) | (uint32_t(rgba[1]) << 8) | rgba[2];
    if (rgb == base)
        return std::nullopt;
    return rgb;
}

// Which outfit a texture shared by both (hands, eyes, mouths) takes its colors
// from: Hero's Clothes unless only the Outset ones were changed.
bool shared_casual() {
    bool hero = false, casual = false;
    for (const BaseColor& c : kBase)
        if (chosen(c.casual, c.name, c.rgb))
            (c.casual ? casual : hero) = true;
    return casual && !hero;
}

void release_vanilla() {
    for (Texture& t : g_textures) {
        if (t.registered)
            aurora::texture::unregister_replacement(*t.registered);
        t.registered.reset();
        t.live.clear();
    }
}

void release_model() {
    for (ModelLive& l : g_model_live)
        for (const auto& r : l.registered)
            aurora::texture::unregister_replacement(r);
    g_model_live.clear();
}

// The model's textures recolored the way wwrando's customizer would (model_colors::Recolor), under the keys of
// the model's own textures.
void apply_model() {
    release_model();
    if (g_model == nullptr)
        return;
    const auto result = model_colors::Recolor(*g_model, [](bool casual, const std::string& color) -> std::optional<uint32_t> {
        const auto it = g_model_chosen.find(model_colors::ColorId(*g_model, casual, color));
        if (it == g_model_chosen.end())
            return std::nullopt;
        return it->second;
    });
    g_model_live.reserve(result.size());
    for (const model_colors::Recolored& r : result) {
        g_model_live.emplace_back();
        ModelLive& l = g_model_live.back();
        l.label = r.texture;
        l.pixels = r.image.bytes;
        for (const std::string& k : r.keys) {
            const auto key = parse_key(k);
            if (!key)
                continue;
            aurora::texture::RawTextureReplacement raw{};
            raw.bytes = std::span<const uint8_t>(l.pixels.data(), l.pixels.size());
            raw.width = r.image.width;
            raw.height = r.image.height;
            raw.mipCount = 1;
            raw.gxFormat = GX_TF_RGBA8_PC;
            raw.label = l.label;
            l.registered.push_back(aurora::texture::register_replacement(*key, raw, {.priority = 1}));
        }
    }
}

// Swords and shields: each texture takes the chosen color of its part (the part's id looked up in kEquip).
void apply_equipment() {
    for (Texture& t : g_equip) {
        Image img = t.base;
        bool any = false;
        for (const EquipPart& part : kEquip) {
            bool mine = false;
            for (const char* name : part.textures)
                mine = mine || t.name == name;
            if (!mine)
                continue;
            uint8_t rgba[4];
            if (!bluewake_cosmetics_get(part.id, rgba))
                continue;
            const uint32_t rgb = (uint32_t(rgba[0]) << 16) | (uint32_t(rgba[1]) << 8) | rgba[2];
            if (rgb == part.base)
                continue;
            exchange(img, nullptr, part.base, rgb, false, part.window, part.min_sat, &t.base);
            any = true;
        }
        if (t.registered) {
            aurora::texture::unregister_replacement(*t.registered);
            t.registered.reset();
        }
        t.live.clear();
        if (!any)
            continue;
        t.live = std::move(img.bytes);
        aurora::texture::RawTextureReplacement raw{};
        raw.bytes = std::span<const uint8_t>(t.live.data(), t.live.size());
        raw.width = img.width;
        raw.height = img.height;
        raw.mipCount = 1;
        raw.gxFormat = GX_TF_RGBA8_PC;
        raw.label = t.name;
        t.registered = aurora::texture::register_replacement(t.key, raw, {.priority = 1});
    }
    // a mod's equipment: the same parts, each texture from its own main colour to the chosen one
    for (ModEquip& m : g_mod_equip) {
        Texture& t = m.tex;
        if (t.registered) {
            aurora::texture::unregister_replacement(*t.registered);
            t.registered.reset();
        }
        t.live.clear();
        uint32_t base = 0;
        for (const EquipPart& part : kEquip)
            if (!std::strcmp(part.id, m.part)) base = part.base;
        uint8_t rgba[4];
        if (!bluewake_cosmetics_get(m.part, rgba)) continue;
        const uint32_t rgb = (uint32_t(rgba[0]) << 16) | (uint32_t(rgba[1]) << 8) | rgba[2];
        if (rgb == base) continue;  // the part is as the game has it: the mod's own colours stay
        Image img = t.base;
        exchange(img, nullptr, m.own, rgb, false, 0.f, 25.f, &t.base);
        t.live = std::move(img.bytes);
        aurora::texture::RawTextureReplacement raw{};
        raw.bytes = std::span<const uint8_t>(t.live.data(), t.live.size());
        raw.width = img.width;
        raw.height = img.height;
        raw.mipCount = 1;
        raw.gxFormat = GX_TF_RGBA8_PC;
        raw.label = t.name;
        t.registered = aurora::texture::register_replacement(t.key, raw, {.priority = 1});
    }
}

// A texture's main colour: the mean of its saturated pixels around the strongest hue (as tools/rando's base colours).
uint32_t main_colour(const Image& img) {
    float hist[36] = {};
    auto hsv = [](const uint8_t* p, float& h, float& s, float& v) {
        const float r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f;
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
        v = mx, s = mx > 0.f ? d / mx : 0.f, h = 0.f;
        if (d > 0.f) {
            if (mx == r) h = std::fmod((g - b) / d, 6.f);
            else if (mx == g) h = (b - r) / d + 2.f;
            else h = (r - g) / d + 4.f;
            h = h / 6.f;
            if (h < 0.f) h += 1.f;
        }
    };
    const size_t n = size_t(img.width) * img.height;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.bytes[i * 4];
        float h, s, v;
        hsv(p, h, s, v);
        if (p[3] > 128 && s > 0.25f && v > 0.15f) hist[int(h * 36.f) % 36] += s * v;
    }
    const int peak = int(std::max_element(hist, hist + 36) - hist);
    uint64_t sum[3] = {}, count = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.bytes[i * 4];
        float h, s, v;
        hsv(p, h, s, v);
        const int bin = int(h * 36.f) % 36, dist = std::min(std::abs(bin - peak), 36 - std::abs(bin - peak));
        if (p[3] > 128 && s > 0.25f && v > 0.15f && dist <= 2) sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], ++count;
    }
    if (count == 0) return 0;
    return uint32_t(sum[0] / count) << 16 | uint32_t(sum[1] / count) << 8 | uint32_t(sum[2] / count);
}

// Every texture of a J3D model, by name.
std::vector<std::pair<std::string, gx_texture::Texture>> bdl_all_textures(const std::vector<uint8_t>& blob) {
    std::vector<std::pair<std::string, gx_texture::Texture>> found;
    auto u32 = [&](size_t a) { return a + 4 <= blob.size() ? uint32_t(blob[a]) << 24 | uint32_t(blob[a + 1]) << 16 | uint32_t(blob[a + 2]) << 8 | blob[a + 3] : 0u; };
    auto u16 = [&](size_t a) { return a + 2 <= blob.size() ? uint32_t(blob[a]) << 8 | blob[a + 1] : 0u; };
    if (blob.size() < 32 || std::memcmp(blob.data(), "J3D2", 4) != 0) return found;
    size_t at = 32;
    for (uint32_t c = 0, count = u32(12); c < count && at + 8 <= blob.size(); ++c) {
        const size_t size = u32(at + 4);
        if (size < 8) break;
        if (std::memcmp(&blob[at], "TEX1", 4) == 0) {
            const size_t hdr = u32(at + 12), tbl = at + u32(at + 16);
            for (size_t i = 0, n = u16(tbl); i < n && tbl + 6 + i * 4 < blob.size(); ++i) {
                std::string s;
                for (size_t p = tbl + u16(tbl + 6 + i * 4); p < blob.size() && blob[p] != 0; ++p) s += char(blob[p]);
                gx_texture::Texture t;
                if (gx_texture::Parse(blob.data(), blob.size(), at + hdr + i * 32, t, nullptr)) found.emplace_back(s, std::move(t));
            }
        }
        at += size;
    }
    return found;
}

void apply_chars() {
    for (CharTexture& c : g_chars) {
        Image img = c.tex.base;
        bool any = false;
        for (const CharPart& part : c.parts) {
            uint8_t rgba[4];
            if (!bluewake_cosmetics_get(part.id.c_str(), rgba))
                continue;
            const uint32_t rgb = (uint32_t(rgba[0]) << 16) | (uint32_t(rgba[1]) << 8) | rgba[2];
            if (rgb == part.base)
                continue;
            exchange(img, &part.mask, part.base, rgb, false);
            any = true;
        }
        Texture& t = c.tex;
        if (t.registered) {
            aurora::texture::unregister_replacement(*t.registered);
            t.registered.reset();
        }
        t.live.clear();
        if (!any)
            continue;
        t.live = std::move(img.bytes);
        aurora::texture::RawTextureReplacement raw{};
        raw.bytes = std::span<const uint8_t>(t.live.data(), t.live.size());
        raw.width = img.width;
        raw.height = img.height;
        raw.mipCount = 1;
        raw.gxFormat = GX_TF_RGBA8_PC;
        raw.label = t.name;
        t.registered = aurora::texture::register_replacement(t.key, raw, {.priority = 1});
    }
}

// One texture recolored with the colors `chosen` gives (the local player's, or a puppet's override); false when none of
// them touches it.
bool recolor_one(const Texture& t, bool share_casual, Image& img_out) {
    const bool body_hero = t.name == "linktexS3TC";
    const bool body_casual = t.name == "linktexbci4";
    const bool hair = t.name == "katsuraS3TC";
    Image img = t.base;
    bool any = false;
    bool masked_type = false;
    for (const Mask& m : g_masks) {
        if (m.texture != t.name)
            continue;
        masked_type = true;
        const bool outfit = body_hero ? false : body_casual ? true : share_casual;
        if (m.casual != outfit)
            continue;
        for (const BaseColor& c : kBase) {
            if (c.casual != m.casual || m.color != c.name)
                continue;
            if (auto rgb = chosen(c.casual, c.name, c.rgb)) {
                exchange(img, &m.image, c.rgb, *rgb, false);
                any = true;
            }
        }
    }
    // Textures with no masks of their own: wwrando's defaults (hands and
    // mouths take Skin, the eyes take Eyes and keep their whites).
    if (!masked_type && !body_hero && !body_casual && !hair) {
        const bool eyes = t.name == "hitomi";
        const char* name = eyes ? "Eyes" : "Skin";
        for (const BaseColor& c : kBase)
            if (c.casual == share_casual && std::strcmp(c.name, name) == 0)
                if (auto rgb = chosen(c.casual, c.name, c.rgb)) {
                    exchange(img, nullptr, c.rgb, *rgb, eyes);
                    any = true;
                }
    }
    // The Outset clothes' back hair: its top-left 8x8 is the hair color.
    if (hair) {
        if (auto rgb = chosen(true, "Hair", 0xFFEF10)) {
            for (uint32_t y = 0; y < std::min(8u, img.height); ++y)
                for (uint32_t x = 0; x < std::min(8u, img.width); ++x) {
                    uint8_t* p = &img.bytes[(y * img.width + x) * 4];
                    p[0] = uint8_t(*rgb >> 16);
                    p[1] = uint8_t(*rgb >> 8);
                    p[2] = uint8_t(*rgb);
                }
            any = true;
        }
    }
    img_out = std::move(img);
    return any;
}

void apply() {
    const bool share_casual = shared_casual();
    for (Texture& t : g_textures) {
        Image img;
        const bool any = recolor_one(t, share_casual, img);
        if (t.registered) {
            aurora::texture::unregister_replacement(*t.registered);
            t.registered.reset();
        }
        t.live.clear();
        if (any) {
            t.live = std::move(img.bytes);
            aurora::texture::RawTextureReplacement raw{};
            raw.bytes = std::span<const uint8_t>(t.live.data(), t.live.size());
            raw.width = img.width;
            raw.height = img.height;
            raw.mipCount = 1;
            raw.gxFormat = GX_TF_RGBA8_PC;
            raw.label = t.name;
            // Above a texture pack's version of Link's textures.
            t.registered = aurora::texture::register_replacement(t.key, raw, {.priority = 1});
        }
    }
}

} // namespace

bool Load() {
    if (g_tried)
        return g_loaded;
    g_tried = true;
    const auto root = assets_folder();
    std::ifstream in(root / "index.json", std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[link-colors] no %s: Link's colors stay the game's\n", (root / "index.json").string().c_str());
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    picojson::value doc;
    if (!picojson::parse(doc, text.str()).empty() || !doc.is<picojson::object>())
        return false;
    const auto& o = doc.get<picojson::object>();
    if (o.count("textures") && o.at("textures").is<picojson::array>())
        for (const auto& v : o.at("textures").get<picojson::array>()) {
            if (!v.is<picojson::object>())
                continue;
            const auto& e = v.get<picojson::object>();
            Texture t;
            t.name = e.count("name") ? e.at("name").to_str() : "";
            const auto key = parse_key(e.count("key") ? e.at("key").to_str() : "");
            if (!key || !read_image(root / (e.count("file") ? e.at("file").to_str() : ""), 4, t.base)) {
                std::fprintf(stderr, "[link-colors] skipped texture %s\n", t.name.c_str());
                continue;
            }
            t.key = *key;
            g_textures.push_back(std::move(t));
        }
    if (o.count("masks") && o.at("masks").is<picojson::array>())
        for (const auto& v : o.at("masks").get<picojson::array>()) {
            if (!v.is<picojson::object>())
                continue;
            const auto& e = v.get<picojson::object>();
            Mask m;
            m.casual = e.count("outfit") && e.at("outfit").to_str() == "casual";
            m.color = e.count("color") ? e.at("color").to_str() : "";
            m.texture = e.count("texture") ? e.at("texture").to_str() : "";
            if (!read_image(root / (e.count("file") ? e.at("file").to_str() : ""), 1, m.image))
                continue;
            const auto t = std::find_if(g_textures.begin(), g_textures.end(),
                                        [&](const Texture& x) { return x.name == m.texture; });
            if (t == g_textures.end() || t->base.width != m.image.width || t->base.height != m.image.height)
                continue;
            g_masks.push_back(std::move(m));
        }
    if (o.count("equipment") && o.at("equipment").is<picojson::array>())
        for (const auto& v : o.at("equipment").get<picojson::array>()) {
            if (!v.is<picojson::object>())
                continue;
            const auto& e = v.get<picojson::object>();
            Texture t;
            t.name = e.count("name") ? e.at("name").to_str() : "";
            const auto key = parse_key(e.count("key") ? e.at("key").to_str() : "");
            if (!key || !read_image(root / (e.count("file") ? e.at("file").to_str() : ""), 4, t.base))
                continue;
            t.key = *key;
            g_equip.push_back(std::move(t));
        }
    if (o.count("chars") && o.at("chars").is<picojson::array>())
        for (const auto& v : o.at("chars").get<picojson::array>()) {
            if (!v.is<picojson::object>())
                continue;
            const auto& e = v.get<picojson::object>();
            CharTexture c;
            c.tex.name = e.count("name") ? e.at("name").to_str() : "";
            const auto key = parse_key(e.count("key") ? e.at("key").to_str() : "");
            if (!key || !read_image(root / (e.count("file") ? e.at("file").to_str() : ""), 4, c.tex.base))
                continue;
            c.tex.key = *key;
            if (e.count("parts") && e.at("parts").is<picojson::array>())
                for (const auto& pv : e.at("parts").get<picojson::array>()) {
                    if (!pv.is<picojson::object>())
                        continue;
                    const auto& pe = pv.get<picojson::object>();
                    CharPart part;
                    part.id = pe.count("id") ? pe.at("id").to_str() : "";
                    part.base = pe.count("base") ? uint32_t(std::strtoul(pe.at("base").to_str().c_str(), nullptr, 16)) : 0;
                    if (part.id.empty() || !read_image(root / (pe.count("mask") ? pe.at("mask").to_str() : ""), 1, part.mask) ||
                        part.mask.width != c.tex.base.width || part.mask.height != c.tex.base.height)
                        continue;
                    c.parts.push_back(std::move(part));
                }
            if (!c.parts.empty())
                g_chars.push_back(std::move(c));
        }
    g_loaded = !g_textures.empty();
    std::fprintf(stderr, "[link-colors] %zu textures, %zu masks from %s\n", g_textures.size(), g_masks.size(),
                 root.string().c_str());
    g_dirty = true;
    return g_loaded;
}


// ------------------------------------------------------------ puppets ----
// Another player's puppet draws Link's own display lists, so it shows whatever Link's textures show. The renderer instead
// finds, for a draw made under a texture variant (BP 0x79: the puppet host's hook sets the player's id before each of the
// puppet's materials), a replacement registered with that variant in its key: here, every recolorable texture in that
// player's colors (or the game's own, when they set none).
namespace {
struct PlayerLive {
    std::string spec;
    std::deque<std::vector<uint8_t>> pixels;
    std::vector<aurora::texture::ReplacementRegistration> registered;
    std::shared_ptr<const model_colors::Set> model; // kept while registrations view its pixels
};
std::map<int, PlayerLive> g_players;
std::map<int, std::shared_ptr<const model_colors::Set>> g_player_models;

std::map<std::string, uint32_t> parse_spec(const std::string& spec) {
    std::map<std::string, uint32_t> out;
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq + 7 != item.size())
            continue;
        const std::string name = item.substr(0, eq); // "Hero.Tunic" (Link's colors) or "Equip.Ship.Hull" (the gear's)
        out[name.rfind("Equip.", 0) == 0 ? name : "Link." + name] = static_cast<uint32_t>(std::strtoul(item.c_str() + eq + 1, nullptr, 16));
    }
    return out;
}
} // namespace

std::string LocalSpec() {
    std::string out;
    char buf[16];
    for (const BaseColor& c : kBase)
        if (auto rgb = chosen(c.casual, c.name, c.rgb)) {
            std::snprintf(buf, sizeof buf, "=%06x", *rgb & 0xFFFFFFu);
            out += (out.empty() ? "" : ",") + id_of(c.casual, c.name).substr(5) + buf;
        }
    // The colors of a player model in use ("M.Hero.Tunic=5ab24a": the model's own color names, spaces out); the puppet of a
    // player with the same model takes them (SetPlayerModel).
    if (g_model != nullptr) {
        const std::string prefix = "Link." + g_model->id_name + ".";
        for (const auto& [id, rgb] : g_model_chosen) {
            if (id.rfind(prefix, 0) != 0)
                continue;
            const std::string tail = id.substr(prefix.size());
            if (tail.find_first_of(",=") != std::string::npos)
                continue;
            std::snprintf(buf, sizeof buf, "=%06x", rgb & 0xFFFFFFu);
            out += (out.empty() ? "" : ",") + ("M." + tail) + buf;
        }
    }
    // The gear's colors (swords, shields, the King of Red Lions, ...): the others' puppets and boats wear them too.
    for (const EquipPart& part : kEquip) {
        uint8_t rgba[4];
        if (!bluewake_cosmetics_get(part.id, rgba))
            continue;
        const uint32_t rgb = (uint32_t(rgba[0]) << 16) | (uint32_t(rgba[1]) << 8) | rgba[2];
        if (rgb == part.base)
            continue;
        std::snprintf(buf, sizeof buf, "=%06x", rgb);
        out += (out.empty() ? "" : ",") + std::string(part.id) + buf;
    }
    return out;
}

void ForgetPlayer(int id) {
    const auto it = g_players.find(id);
    if (it == g_players.end())
        return;
    for (const auto& r : it->second.registered)
        aurora::texture::unregister_replacement(r);
    g_players.erase(it);
}

void SetPlayerModel(int id, std::shared_ptr<const model_colors::Set> set) {
    if (set)
        g_player_models[id] = std::move(set);
    else
        g_player_models.erase(id);
    ForgetPlayer(id); // rebuilt with the model at the next SetPlayerColors
}

void SetPlayerColors(int id, const std::string& spec) {
    const auto model = g_player_models.find(id);
    if (id <= 0 || (!Load() && model == g_player_models.end()))
        return;
    const auto have = g_players.find(id);
    if (have != g_players.end() && have->second.spec == spec)
        return;
    ForgetPlayer(id);
    PlayerLive& live = g_players[id];
    live.spec = spec;
    const auto colors = parse_spec(spec);
    g_override = &colors;
    const bool share_casual = shared_casual();
    for (const Texture& t : g_textures) {
        Image img;
        if (!recolor_one(t, share_casual, img))
            img = t.base;
        live.pixels.push_back(std::move(img.bytes));
        aurora::texture::RawTextureReplacement raw{};
        raw.bytes = std::span<const uint8_t>(live.pixels.back().data(), live.pixels.back().size());
        raw.width = t.base.width;
        raw.height = t.base.height;
        raw.mipCount = 1;
        raw.gxFormat = GX_TF_RGBA8_PC;
        raw.label = t.name;
        auto key = t.key;
        key.variant = uint32_t(id);
        live.registered.push_back(aurora::texture::register_replacement(key, raw, {.priority = 1}));
    }
    // The gear (swords, shields, the ship): every equipment texture under this player's variant, in their color for its part or
    // as the game has it, so a boat or a sword is never the local player's color (the unvaried replacement is the local's).
    for (const Texture& t : g_equip) {
        Image img = t.base;
        for (const EquipPart& part : kEquip) {
            bool mine = false;
            for (const char* name : part.textures)
                mine = mine || t.name == name;
            const auto it = mine ? colors.find(part.id) : colors.end();
            if (it != colors.end() && it->second != part.base)
                exchange(img, nullptr, part.base, it->second, false, part.window, part.min_sat, &t.base);
        }
        live.pixels.push_back(std::move(img.bytes));
        aurora::texture::RawTextureReplacement raw{};
        raw.bytes = std::span<const uint8_t>(live.pixels.back().data(), live.pixels.back().size());
        raw.width = t.base.width;
        raw.height = t.base.height;
        raw.mipCount = 1;
        raw.gxFormat = GX_TF_RGBA8_PC;
        raw.label = t.name;
        auto key = t.key;
        key.variant = uint32_t(id);
        live.registered.push_back(aurora::texture::register_replacement(key, raw, {.priority = 1}));
    }
    g_override = nullptr;
    // The player's own model: every one of its textures, in the model colors the spec holds (unchanged ones too, so that a
    // puppet is never drawn with the local player's recolor of the same texture). Above the vanilla replacements.
    if (model != g_player_models.end()) {
        live.model = model->second;
        const model_colors::Set& set = *live.model;
        const auto result = model_colors::Recolor(set, [&](bool casual, const std::string& color) -> std::optional<uint32_t> {
            const std::string full = model_colors::ColorId(set, casual, color);
            const auto it = colors.find("Link.M." + full.substr(std::string("Link.").size() + set.id_name.size() + 1));
            if (it == colors.end())
                return std::nullopt;
            return it->second;
        });
        for (const model_colors::Texture& t : set.textures) {
            const model_colors::Image* image = &t.rgba;
            for (const model_colors::Recolored& r : result)
                if (r.texture == t.name)
                    image = &r.image;
            // An unchanged texture is viewed in the model's own data (kept by live.model); a recolored one has its pixels held here.
            const std::vector<uint8_t>* pixels = &image->bytes;
            if (image != &t.rgba) {
                live.pixels.push_back(image->bytes);
                pixels = &live.pixels.back();
            }
            for (const std::string& k : t.keys) {
                const auto key = parse_key(k);
                if (!key)
                    continue;
                aurora::texture::RawTextureReplacement raw{};
                raw.bytes = std::span<const uint8_t>(pixels->data(), pixels->size());
                raw.width = image->width;
                raw.height = image->height;
                raw.mipCount = 1;
                raw.gxFormat = GX_TF_RGBA8_PC;
                raw.label = t.name;
                auto variant_key = *key;
                variant_key.variant = uint32_t(id);
                live.registered.push_back(aurora::texture::register_replacement(variant_key, raw, {.priority = 2}));
            }
        }
        std::fprintf(stderr, "[link_colors] player %d: the model %s, %zu textures, %zu recolored\n", id, set.name.c_str(), set.textures.size(), result.size());
    }
    std::fprintf(stderr, "[link_colors] player %d: %zu textures and %zu equipment textures in colors [%s]\n", id, g_textures.size(), g_equip.size(),
                 spec.c_str());
}

void MarkDirty() { g_dirty = true; }

void UseEquipmentArc(const std::vector<uint8_t>* arc) {
    for (ModEquip& m : g_mod_equip)
        if (m.tex.registered) aurora::texture::unregister_replacement(*m.tex.registered);
    g_mod_equip.clear();
    g_dirty = true;
    if (arc == nullptr) return;
    Load();
    std::vector<aurora::texture::TextureSourceKey> vanilla;  // the game's own: a texture the mod kept as it was is coloured already
    for (const Texture& t : g_equip) vanilla.push_back(t.key);
    for (const EquipModel& em : kEquipModels) {
        std::vector<uint8_t> blob;
        if (!rarc::Extract(*arc, em.member, blob)) continue;
        rarc::Yaz0Expand(blob);
        for (auto& [name, gt] : bdl_all_textures(blob)) {
            if (equip_skip(name)) continue;
            std::string error;
            const std::string key = gx_texture::Key(gt, &error);
            const auto parsed = parse_key(key);
            if (key.empty() || !parsed || std::find(vanilla.begin(), vanilla.end(), *parsed) != vanilla.end()) continue;
            if (std::any_of(g_mod_equip.begin(), g_mod_equip.end(), [&](const ModEquip& m) { return m.tex.key == *parsed; })) continue;
            ModEquip m;
            if (!gx_texture::DecodeRgba(gt, m.tex.base.bytes, &error)) continue;
            m.tex.base.width = gt.width, m.tex.base.height = gt.height;
            m.tex.name = name;
            m.tex.key = *parsed;
            m.part = em.part;
            for (const EquipPart& part : kEquip)  // a texture named as one of the game's takes that texture's part
                for (const char* n : part.textures)
                    if (name == n) m.part = part.id;
            m.own = main_colour(m.tex.base);
            if (m.own == 0) continue;  // no colour to shift (grey metal)
            g_mod_equip.push_back(std::move(m));
        }
    }
    std::fprintf(stderr, "[link_colors] the mod's equipment: %zu textures take the Equipment colours\n", g_mod_equip.size());
}

void UseModel(const model_colors::Set* set) {
    release_vanilla();
    release_model();
    g_model = set;
    g_dirty = true;
}

void SetModelColor(const std::string& id, uint32_t rgb, bool changed) {
    if (changed)
        g_model_chosen[id] = rgb;
    else
        g_model_chosen.erase(id);
    g_dirty = true;
}

void Frame() {
    if (!g_dirty)
        return;
    const bool have = Load(); // Link's own data (equipment too); a player model brings its own textures
    if (!have && g_model == nullptr)
        return;
    const Uint64 now = SDL_GetTicks();
    if (now - g_last_apply < 100)
        return;
    g_last_apply = now;
    g_dirty = false;
    if (g_model != nullptr)
        apply_model();
    else
        apply();
    if (have) {
        apply_equipment();
        apply_chars();
    }
}

} // namespace link_colors
