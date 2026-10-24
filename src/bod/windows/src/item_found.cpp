// Item names, icons and the save diff's items: see item_found.h.
// The Windows build force-includes a POSIX layer whose rename() macro breaks
// <filesystem> (which aurora's headers include).
#ifdef rename
#undef rename
#endif

#include "item_found.h"
#include "platform/bod_platform.h"

#include "gx_texture.h"
#include "png_decode.h"
#include "rarc.h"

#include <aurora/imgui.h>
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "gxruntime/dvd.h"
bool bluewake_game_options_get(const char* name);
bool bluewake_bunny_hood_enabled(void);
}

namespace item_found {
namespace {

struct Info {
    int id;
    const char* name;
    const char* icon; // in res/Msg/itemicon.arc (d_item_data.cpp's item_resource, mTexture)
};

// Items a save diff can name (tww include/d/d_item_data.h; the icon file is the
// game's own for that item).
const Info kItems[] = {
    {0x07, "Piece of Heart", "heart_up_02.bti"},
    {0x08, "Heart Container", "heart_up_01.bti"},
    {0x15, "Small Key", "get_key.bti"},
    {0x20, "Telescope", "telescope.bti"},
    {0x21, "Tingle Tuner", "whistle.bti"},
    {0x22, "Wind Waker", "baton.bti"},
    {0x23, "Picto Box", "camera.bti"},
    {0x24, "Spoils Bag", "coverofbeast.bti"},
    {0x25, "Grappling Hook", "rope.bti"},
    {0x26, "Deluxe Picto Box", "camera_2.bti"},
    {0x27, "Hero's Bow", "bow_01.bti"},
    {0x28, "Power Bracelets", "gloves_00.bti"},
    {0x29, "Iron Boots", "boots_00.bti"},
    {0x2A, "Magic Armor", "shield_02.bti"},
    {0x2C, "Bait Bag", "coverofbait.bti"},
    {0x2D, "Boomerang", "boomerang.bti"},
    {0x2F, "Hookshot", "hookshot.bti"},
    {0x30, "Delivery Bag", "delivery.bti"},
    {0x31, "Bomb Bag", "bomb_00.bti"},
    {0x33, "Skull Hammer", "hammer_01.bti"},
    {0x34, "Deku Leaf", "fan.bti"},
    {0x35, "Fire and Ice Arrows", "arrow_power_01.bti"},
    {0x36, "Light Arrows", "arrow_power_02.bti"},
    {0x38, "Hero's Sword", "sword_00.bti"},
    {0x39, "Master Sword", "sword_01.bti"},
    {0x3A, "Master Sword (Half Power)", "sword_02.bti"},
    {0x3B, "Hero's Shield", "shield_00.bti"},
    {0x3C, "Mirror Shield", "shield_01.bti"},
    {0x3E, "Master Sword (Full Power)", "sword_03.bti"},
    {0x42, "Pirate's Charm", "amulet_00.bti"},
    {0x43, "Hero's Charm", "amulet_01.bti"},
    {0x44, "Bunny Hood", "@bunnyhood_icon.png"}, // not in the game: the generated picture in assets/bunnyhood
    {0x4C, "Dungeon Map", "dungeon_map.bti"},
    {0x4D, "Compass", "compass.bti"},
    {0x4E, "Big Key", "boss_key.bti"},
    {0x50, "Empty Bottle", "bottle_00.bti"},
    {0x51, "Red Potion", "bottle_01.bti"},
    {0x52, "Green Potion", "bottle_02.bti"},
    {0x53, "Blue Potion", "bottle_03.bti"},
    {0x54, "Elixir Soup (Half)", "bottle_09.bti"},
    {0x55, "Elixir Soup", "bottle_04.bti"},
    {0x56, "Bottled Water", "bottle_05.bti"},
    {0x57, "Fairy in a Bottle", "bottle_06.bti"},
    {0x58, "Forest Firefly", "bottle_07.bti"},
    {0x59, "Forest Water", "bottle_08.bti"},
    {0x61, "Triforce Shard 1", "triforce_00.bti"},
    {0x62, "Triforce Shard 2", "triforce_01.bti"},
    {0x63, "Triforce Shard 3", "triforce_02.bti"},
    {0x64, "Triforce Shard 4", "triforce_03.bti"},
    {0x65, "Triforce Shard 5", "triforce_04.bti"},
    {0x66, "Triforce Shard 6", "triforce_05.bti"},
    {0x67, "Triforce Shard 7", "triforce_06.bti"},
    {0x68, "Triforce Shard 8", "triforce_07.bti"},
    {0x69, "Nayru's Pearl", "god_symbol_02.bti"},
    {0x6A, "Din's Pearl", "god_symbol_00.bti"},
    {0x6B, "Farore's Pearl", "god_symbol_01.bti"},
    {0x6D, "Wind's Requiem", "baton.bti"},
    {0x6E, "Ballad of Gales", "baton.bti"},
    {0x6F, "Command Melody", "baton.bti"},
    {0x70, "Earth God's Lyric", "baton.bti"},
    {0x71, "Wind God's Aria", "baton.bti"},
    {0x72, "Song of Passing", "baton.bti"},
    {0x78, "Sail", "sail_00.bti"},
    {0xAB, "Giant Wallet", "big_purse.bti"},
    {0xAC, "Tycoon Wallet", "max_purse.bti"},
    {0xAD, "Bomb Bag (60)", "bombpouch_1.bti"},
    {0xAE, "Bomb Bag (99)", "bombpouch_2.bti"},
    {0xAF, "Quiver (60)", "arrowcase_1.bti"},
    {0xB0, "Quiver (99)", "arrowcase_2.bti"},
    {0xB1, "Magic Meter", ""},
    {0xB2, "Double Magic Meter", ""},
};

const Info* find(int id) {
    for (const Info& i : kItems)
        if (i.id == id)
            return &i;
    return nullptr;
}

// Charts: collect map n (1-based) is item 0xFF - n (d_item.cpp item_func_collectmap).
int chart_item(int n) { return 0xFF - n; }

bool is_chart(int item) { return item >= 0xFF - 49 && item <= 0xFE; }

const char* chart_name(int item) {
    const int n = 0xFF - item;
    if (n >= 1 && n <= 8)
        return "Triforce Chart";
    if (n == 35)
        return "Tingle's Chart";
    if (n == 36)
        return "Ghost Ship Chart";
    return "Treasure Chart";
}

const char* chart_icon(int item) {
    const int n = 0xFF - item;
    return n <= 8 ? "cmap_tri2.bti" : n == 35 ? "cmap_tingle2.bti" : n == 36 ? "cmap_phantomship2.bti" : "cmap_treasure2.bti";
}

int g_last_max_life = -1;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// res/Msg/itemicon.arc, read once the disc is open.
std::vector<uint8_t> g_arc;
std::vector<rarc::File> g_files;
size_t g_data_start;
bool g_arc_ok, g_arc_failed;
Uint64 g_arc_tried;

bool load_arc() {
    if (g_arc_ok)
        return true;
    if (g_arc_failed || SDL_GetTicks() - g_arc_tried < 1000)
        return false;
    g_arc_tried = SDL_GetTicks();
    u32 size = 0;
    u8* data = dvd_read_image_file("res/Msg/itemicon.arc", &size);
    if (data == nullptr)
        return false; // the disc is not open yet: again in a second
    g_arc.assign(data, data + size);
    std::free(data);
    rarc::Yaz0Expand(g_arc);
    if (!rarc::Parse(g_arc, g_files) || g_arc.size() < 0x20) {
        std::fprintf(stderr, "[toast] itemicon.arc is not readable: item toasts have no icons\n");
        g_arc_failed = true;
        return false;
    }
    g_data_start = 0x20u + be32(&g_arc[0x0C]);
    g_arc_ok = true;
    return true;
}

} // namespace

void SetBaseline(const BluewakeSyncChange* snapshot, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (snapshot[i].offset == 0x001)
            g_last_max_life = snapshot[i].value;
}

std::vector<int> FromChanges(const BluewakeSyncChange* c, unsigned n) {
    enum { COLLECT = 0x0B4, CHARTS = 0x0D4, MEMORY = 0x380, MEMORY_SIZE = 0x24, STAGES = 16, DUNGEON_ITEM = 0x21 };
    std::vector<int> out;
    auto add_bits = [&](uint8_t bits, int first_item, int count) {
        for (int b = 0; b < count; ++b)
            if (bits & (1 << b))
                out.push_back(first_item + b);
    };
    for (unsigned i = 0; i < n; ++i) {
        const unsigned o = c[i].offset;
        const uint8_t v = c[i].value;
        if (o >= 0x03C && o < 0x03C + 21) {
            const bool bottle = o >= 0x03C + 14 && o <= 0x03C + 17;
            if (!bottle && v != 0xFF && find(v) != nullptr)
                out.push_back(v); // an item slot filled or upgraded
        } else if (o == 0x001) {
            if (g_last_max_life >= 0 && v >= g_last_max_life + 4)
                out.push_back(0x08);
            g_last_max_life = v;
        } else if (o == 0x012) {
            if (v == 1 || v == 2)
                out.push_back(0xAA + v); // wallet size
        } else if (o == 0x013) {
            if (v == 16 || v == 32)
                out.push_back(v == 16 ? 0xB1 : 0xB2); // max magic
        } else if (o == 0x06F) {
            if (v == 60 || v == 99)
                out.push_back(v == 60 ? 0xAF : 0xB0); // arrow maximum
        } else if (o == 0x070) {
            if (v == 60 || v == 99)
                out.push_back(v == 60 ? 0xAD : 0xAE); // bomb maximum
        } else if (o >= COLLECT && o < COLLECT + 13) {
            // dSv_player_collect_c: swords, shields, bracelets, charms, ..., songs, shards, pearls.
            switch (o - COLLECT) {
            case 0:
                if (v & 1) out.push_back(0x38);
                if (v & 2) out.push_back(0x39);
                if (v & 4) out.push_back(0x3A);
                if (v & 8) out.push_back(0x3E);
                break;
            case 1:
                if (v & 1) out.push_back(0x3B);
                if (v & 2) out.push_back(0x3C);
                break;
            case 2: if (v & 1) out.push_back(0x28); break;
            case 3: if (v & 1) out.push_back(0x42); break;
            case 4:
                if (v & 1) out.push_back(0x43);
                if ((v & 4) && bluewake_bunny_hood_enabled()) out.push_back(0x44); // the Bunny Hood (runtime/host/src/bunny_hood.h)
                break;
            case 9: add_bits(v, 0x6D, 6); break;  // songs
            case 10: add_bits(v, 0x61, 8); break; // Triforce shards
            case 11: add_bits(v, 0x69, 3); break; // pearls
            default: break;
            }
        } else if (o >= CHARTS && o < CHARTS + 16) {
            // The "got" bits of the charts: 32 bit big-endian words, chart n is bit n - 1.
            const unsigned word = (o - CHARTS) / 4, byte_in_word = (o - CHARTS) % 4;
            for (int b = 0; b < 8; ++b)
                if (v & (1 << b)) {
                    const int bit = int(word * 32 + (3 - byte_in_word) * 8 + b);
                    if (bit < 49)
                        out.push_back(chart_item(bit + 1));
                }
        } else if (o >= MEMORY && o < MEMORY + MEMORY_SIZE * STAGES && (o - MEMORY) % MEMORY_SIZE == DUNGEON_ITEM) {
            add_bits(v, 0x4C, 3); // map, compass, big key
        } else if (o >= 0x0F00 && o < 0x0F00 + 16) {
            if (int8_t(v) > 0)
                out.push_back(0x15); // a small key (a negative difference is one used)
        }
    }
    return out.size() > 8 ? std::vector<int>{} : out; // a save loaded, not a pick-up
}

const char* Name(int item) {
    // The Sail reads as the option that changes it says, live (the game's own text is patched the same way).
    if (item == 0x78) {
        if (bluewake_game_options_get("brisk_sail"))
            return "Brisk Sail";
        if (bluewake_game_options_get("swift_sail"))
            return "Swift Sail";
    }
    if (const Info* i = find(item))
        return i->name;
    if (is_chart(item))
        return chart_name(item);
    static char buf[16];
    std::snprintf(buf, sizeof buf, "Item 0x%02X", item & 0xFF);
    return buf;
}

bool IsJunk(int item) {
    return item <= 0x07 || item == 0x09 || item == 0x0A || (item >= 0x0B && item <= 0x12) || item == 0x16 ||
           item == 0x1A || item == 0x1E || (item >= 0xB3 && item <= 0xB8);
}

namespace {

// A texture of the archive's member `file`, made once; faded is the dim grey version (for what the
// player does not have yet). 0 while the disc is not open or when the member is missing.
ImTextureID png_texture(const std::string& name, bool faded);

ImTextureID member_texture(const std::string& file, bool faded) {
    static std::map<std::string, ImTextureID> cache; // 0: none
    const std::string key = (faded ? "~" : "") + file;
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    if (!file.empty() && file[0] == '@') {
        const ImTextureID id = png_texture(file.substr(1), faded);
        cache[key] = id;
        return id;
    }
    if (!load_arc())
        return 0; // not cached: again when the disc is open
    ImTextureID id = 0;
    for (const rarc::File& f : g_files) {
        if (f.name != file)
            continue;
        gx_texture::Texture t;
        std::string error;
        std::vector<uint8_t> rgba, bti;
        if (g_data_start + f.offset + f.size <= g_arc.size()) {
            bti.assign(g_arc.begin() + long(g_data_start + f.offset), g_arc.begin() + long(g_data_start + f.offset + f.size));
            rarc::Yaz0Expand(bti); // the archive's members are each compressed
            if (gx_texture::Parse(bti.data(), bti.size(), 0, t, &error) && gx_texture::DecodeRgba(t, rgba, &error)) {
                if (faded)
                    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                        // Grey and see-through, as Ship of Harkinian's "_Faded" tracker icons are.
                        const unsigned grey = (rgba[i] * 30u + rgba[i + 1] * 59u + rgba[i + 2] * 11u) / 100u * 7u / 10u;
                        rgba[i] = rgba[i + 1] = rgba[i + 2] = static_cast<uint8_t>(grey);
                        rgba[i + 3] = static_cast<uint8_t>(rgba[i + 3] * 6u / 10u);
                    }
                id = aurora_imgui_add_texture(t.width, t.height, rgba.data());
            }
        }
        if (id == 0)
            std::fprintf(stderr, "[toast] icon %s: %s\n", file.c_str(), error.c_str());
        break;
    }
    cache[key] = id;
    return id;
}

// A picture of the generated extras (assets/bunnyhood/<name>, made on first launch; $BLUEWAKE_EXTRAS_DIR first).
// 0 when it is not there: the item then has no icon.
ImTextureID png_texture(const std::string& name, bool faded) {
    std::vector<std::string> folders;
    if (name.size() > 2 && name[1] == ':')  // a full path (the Swift Sail icon)
        folders.push_back("");
    else if (const char* made = std::getenv("BLUEWAKE_EXTRAS_DIR"))
        folders.push_back(std::string(made) + "/assets/bunnyhood/");
    if (const char* base = SDL_GetBasePath(); base != nullptr && !(name.size() > 2 && name[1] == ':'))
        folders.push_back(std::string(base) + "assets/bunnyhood/");
    for (const std::string& folder : folders) {
        std::FILE* f = std::fopen((folder + name).c_str(), "rb");
        if (f == nullptr)
            continue;
        std::vector<uint8_t> bytes;
        uint8_t chunk[4096];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;)
            bytes.insert(bytes.end(), chunk, chunk + n);
        std::fclose(f);
        png_decode::Image image;
        std::string error;
        if (!png_decode::Decode(bytes.data(), bytes.size(), image, &error)) {
            std::fprintf(stderr, "[toast] icon %s: %s\n", name.c_str(), error.c_str());
            return 0;
        }
        if (faded)
            for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
                const unsigned grey = (image.rgba[i] * 30u + image.rgba[i + 1] * 59u + image.rgba[i + 2] * 11u) / 100u * 7u / 10u;
                image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = static_cast<uint8_t>(grey);
                image.rgba[i + 3] = static_cast<uint8_t>(image.rgba[i + 3] * 6u / 10u);
            }
        return aurora_imgui_add_texture(image.width, image.height, image.rgba.data());
    }
    return 0;
}

const char* icon_file(int item) {
    if (const Info* i = find(item))
        return i->icon;
    if (is_chart(item))
        return chart_icon(item);
    return "";
}

} // namespace

ImTextureID IconFile(const char* file, bool faded) {
    return file == nullptr || *file == '\0' ? 0 : member_texture(file, faded);
}

namespace {

// While Swift or Brisk Sail is on, the Sail's own icon is the one the game shows (wwrando's palette swap, the 48 x 48 replacement
// texture first launch makes in assets\swift_sail): "" when the option is off or the file is not there.
std::string swift_sail_icon() {
    if (!bluewake_game_options_get("swift_sail") && !bluewake_game_options_get("brisk_sail"))
        return {};
    static std::string path;
    static bool tried;
    if (!tried) {
        tried = true;
        const char* name = "assets" BOD_SEP "swift_sail" BOD_SEP "tex1_48x48_22e7a81247c4caa5_fe1212280187026e_9.png";
        std::vector<std::string> bases;
        if (const char* extras = std::getenv("BLUEWAKE_EXTRAS_DIR"))
            bases.push_back(std::string(extras) + BOD_SEP);
        if (const char* base = SDL_GetBasePath())
            bases.push_back(base);
        for (const std::string& b : bases) {
            const std::string candidate = b + name;
            if (std::FILE* f = std::fopen(candidate.c_str(), "rb")) {
                std::fclose(f);
                path = candidate;
                break;
            }
        }
    }
    return path;
}

} // namespace

ImTextureID Icon(int item) {
    if (item == 0x78) {
        const std::string icon = swift_sail_icon();
        if (!icon.empty())
            return IconFile(("@" + icon).c_str(), false);
    }
    return IconFile(icon_file(item), false);
}

ImTextureID IconFaded(int item) {
    if (item == 0x78) {
        const std::string icon = swift_sail_icon();
        if (!icon.empty())
            return IconFile(("@" + icon).c_str(), true);
    }
    return IconFile(icon_file(item), true);
}

} // namespace item_found
