// The mods folder: see mods.h. SoH publishes no license; this follows the
// behaviour of its mod menu (docs/fork/research/player-models-colors.md
// section 4.1) with code of its own.
#undef rename // bw_posix_compat.h's, which <filesystem> must not see

#include "mods.h"
#include "platform/bod_platform.h"
#include "rando_flow.h"

#include "player_parts.h"
#include "rarc.h"
#include "ship_menu.h"
#include "ship/cvar.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>

extern "C" {
#include "gxruntime/dvd.h"
#include "mod_archive.h"
#include "mods_host.h"
#include "player_model.h"
#include "voice_swap.h"
#include "game_state.h"
void bw_settings_restart_now(void); // win_settings.cpp
unsigned long long bluewake_host_retrace_count(void); // main.c
}

namespace mods {
namespace {

namespace fs = std::filesystem;
using shipmenu::WidgetInfo;
using shipmenu::WidgetType;

constexpr const char* kOrderCVar = "gSettings.Mods.Order";       // every entry, lowest priority first, '|'
constexpr const char* kDisabledCVar = "gSettings.Mods.Disabled"; // the entries switched off, '|'
constexpr const char* kEnabledCVar = "gSettings.Mods.Enabled";   // the master switch
constexpr const char* kPartsCVar = "gSettings.Mods.Parts";       // per player model: "name:L|E|B:voice(-1,0,1)", '|' joined

// One file an entry gives the disc.
struct Mapped {
    std::string disc;   // the disc path ("res/Object/Link.arc")
    std::string member; // the path in the entry (archive member or relative file)
};

struct Entry {
    std::string name; // the folder's or archive's name in mods\ (the list key)
    fs::path path;
    bool archive = false;
    std::string kind;  // "Player Model", "Disc Files", "Textures", "Empty"
    std::string model; // a player model's name
    std::vector<Mapped> files;
    std::string note;  // why parts are not used, shown in the list
    bool enabled = true;
    // Player models: what the Link.arc and the sound files replace (playerparts bits), found by comparing the
    // archive's members with the game's own; the part chosen to use ('L' Link, 'E' Equipment, 'B' both; 0 the default,
    // both); whether the voice is used (-1 the default: when Link is, 0 no, 1 yes).
    unsigned parts = 0;
    bool detected = false;   // parts is known (needs the disc)
    bool unreadable = false; // the Link.arc is not an archive this build can compose from
    char use = 0;
    int voice = -1;
    // A pack holds several models (a folder each with its Link.arc): the folders, their names, and the one in use.
    std::vector<std::string> roots, root_names;
    int root_index = 0;
};

// What the player chose for each player model, kept in gSettings.Mods.Parts.
struct Saved {
    char use = 0;
    int voice = -1;
    std::string root;
};
std::map<std::string, Saved> g_saved;

std::mutex g_mutex; // the list, against the host's disc-ready call
fs::path g_folder;
bool g_read_only; // a test folder: nothing written there
std::vector<Entry> g_entries; // lowest priority first
std::set<std::string> g_applied; // disc paths the last Apply set
std::map<std::string, std::string> g_winner; // disc path -> the entry that gives it
bool g_disc_open;
bool g_dirty; // the list changed since the last Apply
std::string g_status;
std::string g_link_from = "-"; // the entry Link.arc came from at the last Apply ("" the disc's)
std::string g_color_model;     // the player model entry whose Link.arc the last Apply put in effect ("" none)
std::string g_boot_voice_id; // the voice files the game has loaded (at start; a live swap updates it; see LoadedVoiceId)
bool g_boot_voice_set;
// The voice swapped in while the game runs (voice_swap.h): the JaiInit.aaf and wave files the game holds now, the id and
// files being swapped in, and whether the swap was refused (the Mod Menu then offers a restart).
struct VoiceFiles {
    std::vector<unsigned char> aaf;
    std::map<std::string, std::vector<unsigned char>> aws; // by file name: voice_0.aw, GanonT_0.aw
};
VoiceFiles g_voice_loaded, g_voice_target;
std::string g_voice_target_id;
bool g_voice_pending;       // a swap was asked for and has not finished
std::chrono::steady_clock::time_point g_voice_asked;
bool g_voice_needs_restart; // the swap was refused: the voice is the old one until the game starts again
bool g_voice_later;         // "Later" was chosen for this change
std::string g_plan_link, g_plan_equip, g_plan_voice; // the entries that gave Link, Equipment and the voice at the last Apply ("" the game's)
void (*g_model_listener)();

// Link.arc's room in the archive heap. With a player model in the list at
// start, every Link.arc is served at the game's expanded size plus wwrando's
// 0.202 MiB allowance (customizer.py:23-30), zero-padded, so a model swapped
// in during play fits where the last one was. Without one, the game's own
// file is left alone and a model fits only within its expanded size.
constexpr size_t kLinkGame = 1308608u;
constexpr size_t kLinkReserved = (kLinkGame + 211812u + 31u) & ~size_t{31};
constexpr const char* kLinkArc = "res/Object/Link.arc";
size_t g_link_slot; // set at the first Apply

bool is_player_archive(const std::string& disc) {
    return disc == kLinkArc || disc == "res/Object/LkAnm.arc" || disc == "res/Object/LkD00.arc" ||
           disc == "res/Object/LkD01.arc" || disc == "res/Object/Ship.arc";
}

// A disc file as the game reads it (Yaz0 expanded).
std::vector<unsigned char> disc_file(const char* path) {
    u32 size = 0;
    std::vector<unsigned char> out;
    if (u8* data = dvd_read_image_file(path, &size)) {
        out.assign(data, data + size);
        std::free(data);
    }
    rarc::Yaz0Expand(out);
    return out;
}

// Link.arc padded to its slot. *fits: it fits the slot loaded now (a live swap).
void fit_link(std::vector<unsigned char>& bytes, std::string& note, bool* fits) {
    *fits = bytes.size() <= g_link_slot;
    if (*fits)
        bytes.resize(g_link_slot, 0);
    else if (bytes.size() > kLinkReserved)
        note = "Link.arc is more than 0.2 MiB larger than the game's, which wwrando does not allow; it may not fit.";
}

// A model's archive as wwrando installs it: its bone animations (.bck) are
// the game's, since they can change gameplay (customizer.py:313-320); Link.arc
// padded to its slot. *fits: it fits the slot loaded now (a live swap).
void prepare_player_archive(const std::string& disc, std::vector<unsigned char>& bytes, std::string& note,
                            bool* fits) {
    rarc::Yaz0Expand(bytes);
    const std::vector<unsigned char> vanilla = disc_file(disc.c_str());
    int reverted = 0;
    if (!vanilla.empty() &&
        !rarc::ReplaceMembers(bytes, vanilla, [](const std::string& n) {
            return n.size() > 4 && (n.compare(n.size() - 4, 4, ".bck") == 0 || n.compare(n.size() - 4, 4, ".BCK") == 0);
        }, &reverted))
        note = disc + ": its animations could not be checked (not a plain RARC); used as is.";
    if (reverted)
        std::fprintf(stderr, "[mods] %s: %d bone animations are the game's\n", disc.c_str(), reverted);
    if (disc == kLinkArc)
        fit_link(bytes, note, fits);
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size()) {
        const size_t bar = s.find('|', at);
        const std::string item = s.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
        if (!item.empty())
            out.push_back(item);
        if (bar == std::string::npos)
            break;
        at = bar + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& items) {
    std::string s;
    for (const auto& i : items)
        s += (s.empty() ? "" : "|") + i;
    return s;
}

bool is_archive(const fs::path& p) {
    const std::string ext = lower(p.extension().string());
    return ext == ".zip" || ext == ".rar" || ext == ".7z";
}

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The files of an entry, relative with '/'.
std::vector<std::string> list_files(const Entry& e) {
    std::vector<std::string> out;
    if (e.archive) {
        if (ModArchive* a = mod_archive_open(utf8(e.path).c_str())) {
            for (unsigned i = 0; i < mod_archive_count(a); ++i)
                out.push_back(mod_archive_name(a, i));
            mod_archive_close(a);
        }
        return out;
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(e.path, fs::directory_options::follow_directory_symlink, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            std::string rel = utf8(fs::relative(it->path(), e.path, ec));
            std::replace(rel.begin(), rel.end(), '\\', '/');
            out.push_back(rel);
        }
    }
    return out;
}

std::string dir_of(const std::string& p) {
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? "" : p.substr(0, slash + 1);
}

std::string base_of(const std::string& p) {
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// What an entry gives: a wwrando player model (the files wwrando's installer
// writes, customizer.py:198-311), or disc files under res/ or Audiores/.
void classify(Entry& e) {
    e.files.clear();
    e.note.clear();
    e.model.clear();
    const std::vector<std::string> files = list_files(e);
    std::map<std::string, std::string> by_lower; // lower path -> path
    for (const auto& f : files)
        by_lower[lower(f)] = f;
    // A model: a folder with Link.arc (or, for a voice alone, with sound/JaiInit.aaf). A pack holds several such folders:
    // the entry lists them (e.roots, shallowest first) and uses the one chosen (e.root_index; the first by default).
    std::vector<std::string> roots;
    static const std::string kVoiceTail = "sound/jaiinit.aaf";
    for (const auto& [low, f] : by_lower)
        if (base_of(low) == "link.arc")
            roots.push_back(dir_of(f));
    if (roots.empty())
        for (const auto& [low, f] : by_lower)
            if (low.size() >= kVoiceTail.size() && low.compare(low.size() - kVoiceTail.size(), kVoiceTail.size(), kVoiceTail) == 0 &&
                (low.size() == kVoiceTail.size() || low[low.size() - kVoiceTail.size() - 1] == '/'))
                roots.push_back(f.substr(0, f.size() - kVoiceTail.size()));
    std::stable_sort(roots.begin(), roots.end(), [](const std::string& x, const std::string& y) {
        return std::count(x.begin(), x.end(), '/') < std::count(y.begin(), y.end(), '/');
    });
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    if (!roots.empty()) {
        e.kind = "Player Model";
        e.roots = roots;
        const auto saved = g_saved.find(e.name);
        e.root_index = 0;
        if (saved != g_saved.end())
            for (size_t i = 0; i < roots.size(); ++i)
                if (roots[i] == saved->second.root)
                    e.root_index = static_cast<int>(i);
        const std::string root = roots[static_cast<size_t>(e.root_index)];
        const auto leaf = [&](const std::string& r) {
            return r.empty() ? fs::path(e.name).stem().string() : base_of(r.substr(0, r.size() - 1));
        };
        e.model = leaf(root);
        e.root_names.clear();
        for (const auto& r : roots)
            e.root_names.push_back(leaf(r));
        static const std::pair<const char*, const char*> kModelFiles[] = {
            {"Link.arc", "res/Object/Link.arc"},
            {"LkAnm.arc", "res/Object/LkAnm.arc"},
            {"LkD00.arc", "res/Object/LkD00.arc"},
            {"LkD01.arc", "res/Object/LkD01.arc"},
            {"Ship.arc", "res/Object/Ship.arc"},
            {"sound/JaiInit.aaf", "Audiores/JaiInit.aaf"},
            {"sound/voice_0.aw", "Audiores/Banks/voice_0.aw"},
            {"sound/GanonT_0.aw", "Audiores/Banks/GanonT_0.aw"},
        };
        for (const auto& [rel, disc] : kModelFiles) {
            const auto it = by_lower.find(lower(root + rel));
            if (it != by_lower.end())
                e.files.push_back({disc, it->second});
        }
        // wwrando needs both voice files together (customizer.py:296-311).
        const auto has = [&](const char* disc) {
            return std::any_of(e.files.begin(), e.files.end(), [&](const Mapped& m) { return m.disc == disc; });
        };
        if (has("Audiores/JaiInit.aaf") != has("Audiores/Banks/voice_0.aw")) {
            e.files.erase(std::remove_if(e.files.begin(), e.files.end(),
                                         [](const Mapped& m) { return m.disc.rfind("Audiores/", 0) == 0; }),
                          e.files.end());
            e.note = "Voice skipped: it needs both JaiInit.aaf and voice_0.aw.";
        }
        if (e.files.empty()) {
            e.kind = "Empty";
            e.model.clear();
        }
        return;
    }
    // Disc files: from a res/ or Audiores/ folder at any depth (an optional
    // files/ above it, as in an extracted disc).
    bool textures = false;
    for (const auto& f : files) {
        const std::string low = lower(f);
        size_t at = std::string::npos;
        for (const char* top : {"res/", "audiores/"}) {
            const size_t p = low.rfind(top) == 0 ? 0 : low.find(std::string("/") + top);
            if (p != std::string::npos) {
                at = p == 0 ? 0 : p + 1;
                break;
            }
        }
        if (at != std::string::npos) {
            std::string disc = f.substr(at);
            if (lower(disc).rfind("audiores/", 0) == 0)
                disc = "Audiores/" + disc.substr(9);
            e.files.push_back({disc, f});
        } else if (base_of(low).rfind("tex1_", 0) == 0) {
            textures = true;
        }
    }
    if (!e.files.empty())
        e.kind = "Disc Files";
    else if (textures) {
        e.kind = "Textures";
        e.note = "Texture packs go in mods" BOD_SEP "textures.";
    } else {
        e.kind = "Empty";
        e.note = "No disc files or player model found.";
    }
}

// One file of an entry: an archive member or a loose file. *a is an archive opened on the first need (the caller
// closes it), so several files of one archive are read with one open.
bool read_entry_file(const Entry& e, ModArchive*& a, const std::string& member, std::vector<unsigned char>& out) {
    out.clear();
    if (e.archive) {
        if (a == nullptr)
            a = mod_archive_open(utf8(e.path).c_str());
        const int index = a ? mod_archive_find(a, member.c_str()) : -1;
        unsigned long long size = 0;
        if (unsigned char* data = index >= 0 ? mod_archive_read(a, static_cast<unsigned>(index), &size) : nullptr) {
            out.assign(data, data + size);
            std::free(data);
        }
    } else {
        std::ifstream in(e.path / from_utf8(member), std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return !out.empty();
}

const Mapped* find_disc(const Entry& e, const char* disc) {
    for (const auto& f : e.files)
        if (f.disc == disc)
            return &f;
    return nullptr;
}

// The game's own Link.arc (Yaz0 expanded), read from the disc once it is open.
std::vector<unsigned char> g_vanilla_link;
const std::vector<unsigned char>& vanilla_link() {
    if (g_vanilla_link.empty() && g_disc_open)
        g_vanilla_link = disc_file(kLinkArc);
    return g_vanilla_link;
}

// What a player model replaces, at the RARC entry level (playerparts): Link.arc's members against the game's own.
void detect(Entry& e) {
    e.parts = 0;
    e.unreadable = false;
    e.detected = false;
    if (e.kind != "Player Model")
        return;
    if (find_disc(e, "Audiores/JaiInit.aaf"))
        e.parts |= playerparts::kVoice;
    if (const Mapped* link = find_disc(e, kLinkArc)) {
        const std::vector<unsigned char>& vanilla = vanilla_link();
        if (vanilla.empty())
            return; // the disc is not open yet: asked again when it is
        ModArchive* a = nullptr;
        std::vector<unsigned char> bytes;
        read_entry_file(e, a, link->member, bytes);
        if (a)
            mod_archive_close(a);
        rarc::Yaz0Expand(bytes);
        bool readable = false;
        const unsigned p = playerparts::DetectArc(bytes, vanilla, &readable);
        if (!readable) {
            e.unreadable = true;
            e.note = "Link.arc is not a plain RARC archive this build can read (or is damaged); this mod is not used.";
        } else {
            e.parts |= p & (playerparts::kLink | playerparts::kEquipment);
        }
    }
    e.detected = true;
    std::fprintf(stderr, "[mods] %s: replaces %s%s\n", e.name.c_str(), playerparts::Describe(e.parts).c_str(),
                 e.unreadable ? " (Link.arc unreadable)" : "");
}

// The parts of a player model that are used: the chosen one(s) among those it has. Until the disc is open (parts
// unknown) every part counts as present, as a whole Link.arc was used before parts existed.
unsigned used_parts(const Entry& e) {
    if (e.kind != "Player Model" || e.unreadable)
        return 0;
    const unsigned has = e.detected ? e.parts & (playerparts::kLink | playerparts::kEquipment)
                                    : playerparts::kLink | playerparts::kEquipment;
    unsigned want = e.use == 'L' ? playerparts::kLink : e.use == 'E' ? playerparts::kEquipment : has;
    want &= has;
    return want != 0 ? want : has;
}

bool voice_used(const Entry& e) {
    if (e.kind != "Player Model" || !find_disc(e, "Audiores/JaiInit.aaf"))
        return false;
    // By default the voice goes with Link (and a mod that is only a voice is used for it).
    return e.voice < 0 ? (used_parts(e) & playerparts::kLink) != 0 || (e.detected && e.parts == playerparts::kVoice)
                       : e.voice == 1;
}

std::string escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '%' || c == '|' || c == ':') {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        } else
            out += c;
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else
            out += s[i];
    }
    return out;
}

// gSettings.Mods.Parts: "name:use:voice:root" for each player model changed from the defaults, '|' joined, with
// ':', '|' and '%' in names written as %3A, %7C and %25.
void load_saved() {
    g_saved.clear();
    for (const std::string& item : split(CVarGetString(kPartsCVar, ""))) {
        std::vector<std::string> field;
        size_t at = 0;
        while (true) {
            const size_t colon = item.find(':', at);
            field.push_back(item.substr(at, colon == std::string::npos ? std::string::npos : colon - at));
            if (colon == std::string::npos)
                break;
            at = colon + 1;
        }
        if (field.size() < 3)
            continue;
        Saved s;
        s.use = field[1].size() == 1 && std::strchr("LEB", field[1][0]) ? field[1][0] : '\0';
        s.voice = field[2] == "1" ? 1 : field[2] == "0" ? 0 : -1;
        s.root = field.size() > 3 ? unescape(field[3]) : "";
        g_saved[unescape(field[0])] = s;
    }
}

void apply_saved() {
    for (auto& e : g_entries) {
        const auto it = g_saved.find(e.name);
        if (it != g_saved.end()) {
            e.use = it->second.use;
            e.voice = it->second.voice;
        }
    }
}

void save_parts() {
    std::vector<std::string> items;
    g_saved.clear();
    for (const auto& e : g_entries) {
        if (e.kind != "Player Model")
            continue;
        Saved s;
        s.use = e.use;
        s.voice = e.voice;
        s.root = e.roots.empty() ? "" : e.roots[static_cast<size_t>(e.root_index)];
        const bool changed = e.use != 0 || e.voice >= 0 || e.root_index != 0;
        if (!changed)
            continue;
        g_saved[e.name] = s;
        items.push_back(escape(e.name) + ":" + (e.use ? std::string(1, e.use) : std::string("-")) + ":" +
                        (e.voice < 0 ? "d" : std::to_string(e.voice)) + ":" + escape(s.root));
    }
    CVarSetString(kPartsCVar, join(items).c_str());
    CVarSaveNextFrame();
}

void save_order() {
    std::vector<std::string> order, disabled;
    for (const auto& e : g_entries) {
        order.push_back(e.name);
        if (!e.enabled)
            disabled.push_back(e.name);
    }
    CVarSetString(kOrderCVar, join(order).c_str());
    CVarSetString(kDisabledCVar, join(disabled).c_str());
    CVarSaveNextFrame();
}

// mods\ again: entries in the saved order, new ones on top (enabled, as in
// SoH), vanished ones dropped.
void scan() {
    std::error_code ec;
    const fs::path note = g_folder / "custom_mod_files_go_here.txt";
    if (!g_read_only) {
        fs::create_directories(g_folder, ec);
        if (!fs::exists(note, ec))
            std::ofstream(note) << "Folders and .zip, .rar or .7z archives here are mods (Settings > Mod Menu).\n"
                                   "Dolphin-format texture packs go in the textures folder.\n";
    }
    std::vector<Entry> found;
    for (fs::directory_iterator it(g_folder, ec), end; !ec && it != end; it.increment(ec)) {
        Entry e;
        e.path = it->path();
        e.name = utf8(it->path().filename());
        if (it->is_directory(ec)) {
            if (lower(e.name) == "textures")
                continue;
        } else if (!is_archive(e.path)) {
            continue;
        }
        e.archive = !it->is_directory(ec);
        found.push_back(std::move(e));
    }
    const std::vector<std::string> order = split(CVarGetString(kOrderCVar, ""));
    const std::vector<std::string> off = split(CVarGetString(kDisabledCVar, ""));
    const auto rank = [&](const std::string& name) {
        const auto it = std::find(order.begin(), order.end(), name);
        return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
    };
    std::stable_sort(found.begin(), found.end(), [&](const Entry& a, const Entry& b) {
        const size_t ra = rank(a.name), rb = rank(b.name);
        return ra != rb ? ra < rb : lower(a.name) < lower(b.name);
    });
    load_saved();
    for (auto& e : found) {
        e.enabled = std::find(off.begin(), off.end(), e.name) == off.end();
        classify(e);
    }
    g_entries = std::move(found);
    apply_saved();
    for (auto& e : g_entries)
        detect(e);
    save_order();
}

// Which entry gives Link (body), Equipment and the voice: the highest enabled player model that has the part and
// uses it. Parts of different mods compose one Link.arc.
struct Plan {
    const Entry* link = nullptr;
    const Entry* equip = nullptr;
    const Entry* voice = nullptr;
};

Plan make_plan() {
    Plan plan;
    if (!CVarGetInteger(kEnabledCVar, 1))
        return plan;
    for (const auto& e : g_entries) {
        if (!e.enabled || e.kind != "Player Model")
            continue;
        const unsigned u = used_parts(e);
        if (u & playerparts::kLink)
            plan.link = &e;
        if (u & playerparts::kEquipment)
            plan.equip = &e;
        if (voice_used(e))
            plan.voice = &e;
    }
    return plan;
}

// The id peers compare voices by (see LoadedVoiceId).
std::string voice_id(const Entry& e) {
    const Mapped* init = find_disc(e, "Audiores/JaiInit.aaf");
    const Mapped* wave = find_disc(e, "Audiores/Banks/voice_0.aw");
    if (init == nullptr || wave == nullptr)
        return "";
    ModArchive* a = nullptr;
    std::vector<unsigned char> i, w;
    read_entry_file(e, a, init->member, i);
    read_entry_file(e, a, wave->member, w);
    if (a)
        mod_archive_close(a);
    if (i.empty() || w.empty())
        return "";
    const auto hex = [](const std::vector<unsigned char>& b) {
        uint8_t digest[32] = {0};
        std::string out;
        if (bodplat::Sha256(b.data(), b.size(), digest))
            for (int k = 0; k < 8; ++k) {
                static const char d[] = "0123456789abcdef";
                out += d[digest[k] >> 4];
                out += d[digest[k] & 15];
            }
        return out;
    };
    return hex(i) + hex(w);
}

const char* const kLinkGroup[] = {"res/Object/LkAnm.arc", "res/Object/LkD00.arc", "res/Object/LkD01.arc",
                                  "res/Object/Ship.arc"};

// The overlay as the list says: each disc path from the highest enabled
// entry that has it. Paths no longer given return to the disc's.
std::string voice_id_of_plan(const Plan& plan) { return plan.voice ? voice_id(*plan.voice) : std::string(); }

// The voice files a voice entry (null: the game's) gives, each from the mod, else from the disc as the game would read it.
bool voice_files(const Entry* voice, VoiceFiles& out) {
    static const std::pair<const char*, const char*> kFiles[] = {{"Audiores/Banks/voice_0.aw", "voice_0.aw"},
                                                                  {"Audiores/Banks/GanonT_0.aw", "GanonT_0.aw"}};
    out = VoiceFiles{};
    ModArchive* a = nullptr;
    const auto get = [&](const char* disc, std::vector<unsigned char>& bytes) {
        const Mapped* f = voice ? find_disc(*voice, disc) : nullptr;
        if (f != nullptr)
            read_entry_file(*voice, a, f->member, bytes);
        if (bytes.empty())
            bytes = disc_file(disc);
    };
    get("Audiores/JaiInit.aaf", out.aaf);
    for (const auto& [disc, name] : kFiles)
        get(disc, out.aws[name]);
    if (a)
        mod_archive_close(a);
    return !out.aaf.empty() && !out.aws["voice_0.aw"].empty();
}

// Asks the runtime to put the plan's voice in effect now (voice_swap.h). The result comes back through Frame().
void start_voice_swap(const Plan& plan, std::string& note) {
    if (!voice_files(plan.voice && !g_plan_voice.empty() ? plan.voice : nullptr, g_voice_target)) {
        g_voice_needs_restart = true;
        note += "; the voice changes at the next start";
        return;
    }
    std::vector<const char*> names;
    std::vector<const void*> data;
    std::vector<size_t> sizes;
    for (const auto& [name, bytes] : g_voice_target.aws) {
        names.push_back(name.c_str());
        data.push_back(bytes.data());
        sizes.push_back(bytes.size());
    }
    g_voice_target_id = voice_id_of_plan(plan);
    g_voice_later = false;
    g_voice_needs_restart = false;
    if (bluewake_voice_swap(g_voice_loaded.aaf.data(), g_voice_loaded.aaf.size(), g_voice_target.aaf.data(),
                            g_voice_target.aaf.size(), names.data(), data.data(), sizes.data(), static_cast<int>(names.size()))) {
        g_voice_pending = true;
        g_voice_asked = std::chrono::steady_clock::now();
        note += "; the voice changes now";
    } else {
        g_voice_needs_restart = true;
        note += "; the voice changes at the next start";
    }
}

void apply() {
    std::map<std::string, std::pair<const Entry*, std::string>> want; // disc path -> entry, member
    if (g_link_slot == 0) // the first Apply, before the game runs: Link's room for this session
        g_link_slot = std::any_of(g_entries.begin(), g_entries.end(),
                                  [](const Entry& e) { return e.kind == "Player Model"; })
                          ? kLinkReserved
                          : kLinkGame;
    bool link_fits = true;
    for (auto& e : g_entries)
        if (!e.detected && e.kind == "Player Model")
            detect(e);
    const Plan plan = make_plan();
    for (const auto& e : g_entries)
        if (e.enabled && e.kind == "Player Model" && &e != plan.link && &e != plan.equip && &e != plan.voice)
            std::fprintf(stderr, "[mods] %s: not used, other mods above it give what it replaces\n", e.name.c_str());
    if (CVarGetInteger(kEnabledCVar, 1))
        for (const auto& e : g_entries)
            if (e.enabled && e.kind != "Player Model") {
                for (const auto& f : e.files) {
                    const auto old = want.find(f.disc);
                    if (old != want.end())
                        std::fprintf(stderr, "[mods] %s: %s overrides %s\n", f.disc.c_str(), e.name.c_str(),
                                     old->second.first->name.c_str());
                    want[f.disc] = {&e, f.member};
                }
            }
    // The player model's companions: Link's animation and ship archives from the Link mod, the voice files from the voice mod.
    if (plan.link)
        for (const char* disc : kLinkGroup)
            if (const Mapped* f = find_disc(*plan.link, disc))
                want[f->disc] = {plan.link, f->member};
    if (plan.voice)
        for (const auto& f : plan.voice->files)
            if (f.disc.rfind("Audiores/", 0) == 0)
                want[f.disc] = {plan.voice, f.member};
    for (const auto& path : g_applied)
        if (!want.count(path))
            dvd_overlay_clear(path.c_str());
    g_applied.clear();
    g_winner.clear();
    int ok = 0, failed = 0;
    std::map<const Entry*, ModArchive*> open;
    for (const auto& [disc, source] : want) {
        const Entry* e = source.first;
        std::vector<unsigned char> bytes;
        read_entry_file(*e, open[e], source.second, bytes);
        std::string note;
        bool unused_fits = true;
        if (!bytes.empty() && e->kind == "Player Model" && is_player_archive(disc))
            prepare_player_archive(disc, bytes, note, &unused_fits);
        if (!note.empty())
            std::fprintf(stderr, "[mods] %s: %s\n", e->name.c_str(), note.c_str());
        if (!bytes.empty() && dvd_overlay_set(disc.c_str(), bytes.data(), static_cast<u32>(bytes.size()))) {
            g_applied.insert(disc);
            g_winner[disc] = e->name;
            ++ok;
        } else {
            std::fprintf(stderr, "[mods] %s from %s: not applied (not on the disc, or unreadable)\n", disc.c_str(),
                         e->name.c_str());
            ++failed;
        }
    }
    // Link.arc: one mod's whole archive when it gives both parts; else the game's own with the Link members of one mod
    // and the Equipment members of another put in (a Young Link's body with Samus's equipment).
    if (plan.link || plan.equip) {
        std::vector<unsigned char> bytes;
        std::string note;
        bool have = false;
        if (plan.link == plan.equip) {
            const Mapped* f = find_disc(*plan.link, kLinkArc);
            if (f != nullptr && read_entry_file(*plan.link, open[plan.link], f->member, bytes)) {
                prepare_player_archive(kLinkArc, bytes, note, &link_fits);
                have = true;
            }
        } else {
            bytes = vanilla_link();
            std::vector<unsigned char> link_donor, equip_donor;
            const Mapped* lf = plan.link ? find_disc(*plan.link, kLinkArc) : nullptr;
            const Mapped* ef = plan.equip ? find_disc(*plan.equip, kLinkArc) : nullptr;
            if (lf)
                read_entry_file(*plan.link, open[plan.link], lf->member, link_donor);
            if (ef)
                read_entry_file(*plan.equip, open[plan.equip], ef->member, equip_donor);
            rarc::Yaz0Expand(link_donor);
            rarc::Yaz0Expand(equip_donor);
            if (!bytes.empty() && playerparts::Compose(bytes, link_donor.empty() ? nullptr : &link_donor,
                                                       equip_donor.empty() ? nullptr : &equip_donor)) {
                fit_link(bytes, note, &link_fits);
                have = true;
            } else
                note = "Link.arc could not be put together from the parts.";
        }
        const std::string from = (plan.link ? plan.link->name : std::string("the game")) + " (Link) + " +
                                 (plan.equip ? plan.equip->name : std::string("the game")) + " (Equipment)";
        if (!note.empty())
            std::fprintf(stderr, "[mods] Link.arc: %s\n", note.c_str());
        if (have && dvd_overlay_set(kLinkArc, bytes.data(), static_cast<u32>(bytes.size()))) {
            g_applied.insert(kLinkArc);
            g_winner[kLinkArc] = plan.link ? plan.link->name : plan.equip->name;
            ++ok;
            std::fprintf(stderr, "[mods] Link.arc: %s, %zu bytes\n", from.c_str(), bytes.size());
        } else {
            std::fprintf(stderr, "[mods] Link.arc from %s: not applied\n", from.c_str());
            ++failed;
        }
    }
    for (auto& [e, a] : open)
        if (a != nullptr)
            mod_archive_close(a);
    // Without a model, the game's Link.arc in the reserved slot (when there is one).
    if (!g_applied.count(kLinkArc) && g_link_slot == kLinkReserved) {
        std::vector<unsigned char> link = disc_file(kLinkArc);
        if (!link.empty() && link.size() <= g_link_slot) {
            link.resize(g_link_slot, 0);
            if (dvd_overlay_set(kLinkArc, link.data(), static_cast<u32>(link.size())))
                g_applied.insert(kLinkArc);
        }
    }
    // What Link.arc is made of now, to see when it changes (and to tell Anchor).
    const bool applied_link = g_winner.count(kLinkArc) != 0;
    const std::string link_from =
        applied_link ? (plan.link ? plan.link->name : std::string()) + "|" + (plan.equip ? plan.equip->name : std::string())
                     : "|";
    std::string model_note;
    if (g_link_from != "-" && link_from != g_link_from) {
        if (bluewake_player_model_live() && link_fits) {
            bluewake_player_model_swap();
            model_note = "; Link changes as the area reloads";
        } else {
            model_note = "; Link's model changes at the next start";
        }
    }
    g_link_from = link_from;
    g_plan_link = applied_link && plan.link ? plan.link->name : "";
    g_plan_equip = applied_link && plan.equip ? plan.equip->name : "";
    g_plan_voice = plan.voice && g_winner.count("Audiores/JaiInit.aaf") ? plan.voice->name : "";
    if (!g_boot_voice_set && g_disc_open) { // the first Apply is before the game's audio starts
        g_boot_voice_set = true;
        g_boot_voice_id = plan.voice && !g_plan_voice.empty() ? voice_id(*plan.voice) : "";
        if (!g_boot_voice_id.empty())
            std::fprintf(stderr, "[mods] voice at start: %s (%s)\n", plan.voice->name.c_str(), g_boot_voice_id.c_str());
        voice_files(plan.voice && !g_plan_voice.empty() ? plan.voice : nullptr, g_voice_loaded); // what the game holds
    } else {
        // A voice other than the loaded one is put in effect now (the runtime swaps the wave tables and files); when
        // that is not possible the Mod Menu offers a restart. A swap still waiting for a change that was undone is
        // replaced by one back to what is loaded.
        const std::string wanted = plan.voice && !g_plan_voice.empty() ? voice_id_of_plan(plan) : std::string();
        if (wanted != (g_voice_pending ? g_voice_target_id : g_boot_voice_id))
            start_voice_swap(plan, model_note);
        else if (!g_voice_pending)
            g_voice_needs_restart = false; // (changed back by hand)
    }
    // The colors follow the model whose body is in effect.
    const std::string color_model = g_plan_link;
    if (color_model != g_color_model) {
        g_color_model = color_model;
        if (g_model_listener)
            g_model_listener();
    }
    char buf[200];
    std::snprintf(buf, sizeof buf, "%d file%s from mods in use%s%s", ok, ok == 1 ? "" : "s",
                  failed ? "; some files were not applied (see the log)" : "", model_note.c_str());
    g_status = buf;
    g_dirty = false;
    std::fprintf(stderr, "[mods] %s\n", g_status.c_str());
}

void on_disc_ready() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_disc_open = true;
    apply();
    randoflow::OnDiscReady();
}

// The "Replaces" column: what the mod holds, found in its archives.
void draw_replaces(const Entry& e) {
    if (e.kind != "Player Model") {
        ImGui::TextUnformatted(e.kind.c_str());
        return;
    }
    if (e.unreadable) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f), "Unsupported");
    } else if (!e.detected) {
        ImGui::TextDisabled("Checking...");
    } else if (e.parts == 0) {
        ImGui::TextDisabled("Same as the game's");
    } else {
        ImGui::TextUnformatted(playerparts::Describe(e.parts).c_str());
    }
}

// The "Use" column of a player model: which of its parts to use (only those it has) and its voice.
// True when the choice changed.
bool draw_use(Entry& e) {
    if (e.kind != "Player Model")
        return false;
    bool changed = false;
    if (e.roots.size() > 1) { // a pack of several models: which one
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::BeginCombo("##model", e.root_names[static_cast<size_t>(e.root_index)].c_str())) {
            for (size_t i = 0; i < e.roots.size(); ++i)
                if (ImGui::Selectable(e.root_names[i].c_str(), static_cast<int>(i) == e.root_index) && static_cast<int>(i) != e.root_index) {
                    Saved& s = g_saved[e.name];
                    s.use = e.use;
                    s.voice = e.voice;
                    s.root = e.roots[i];
                    classify(e);
                    detect(e);
                    changed = true;
                    break;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This archive holds several models: which one to use.");
        ImGui::SameLine();
    }
    if (e.unreadable || !e.detected)
        return changed;
    const unsigned has = e.parts & (playerparts::kLink | playerparts::kEquipment);
    const bool voice = (e.parts & playerparts::kVoice) != 0;
    struct Choice {
        const char* label;
        char code;
        unsigned need;
    };
    const Choice choices[] = {{"Link", 'L', playerparts::kLink},
                              {"Equipment", 'E', playerparts::kEquipment},
                              {"Both", 'B', playerparts::kLink | playerparts::kEquipment}};
    if (has != 0) {
        const unsigned u = used_parts(e);
        const char* current = u == (playerparts::kLink | playerparts::kEquipment) ? "Both" : u == playerparts::kLink ? "Link" : "Equipment";
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("Equipment").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFrameHeight());
        if (ImGui::BeginCombo("##use", current)) {
            for (const Choice& c : choices) {
                if ((c.need & has) != c.need)
                    continue; // only what the mod has
                if (ImGui::Selectable(c.label, u == c.need)) {
                    e.use = c.code;
                    // The voice follows Link unless it was chosen: Equipment alone does not bring a voice.
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Which part of this mod to use: Link (the body, hair, hands), Equipment (sword, shield, bow, ...)\nor both. A part comes from the highest enabled mod that uses it, so parts of different mods mix.");
    }
    if (voice) {
        if (has != 0)
            ImGui::SameLine();
        bool on = voice_used(e);
        if (ImGui::Checkbox("Voice", &on)) {
            e.voice = on ? 1 : 0;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This mod's voice files. By default the voice goes with Link. The game reads its voice once, "
                              "so a change is swapped in while it runs, when no voice of Link's is playing; if that cannot "
                              "be done the Mod Menu offers a restart.");
    }
    return changed;
}

void draw_list() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ImGui::TextWrapped("Mod priority is top to bottom: a mod overrides the ones below it.");
    if (g_entries.empty()) {
        ImGui::TextDisabled("No mods. Put folders or .zip, .rar or .7z archives in the mods folder, then Rescan.");
        return;
    }
    int move_from = -1, move_to = -1;
    bool changed_parts = false;
    if (ImGui::BeginTable("##mods", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Mod");
        ImGui::TableSetupColumn("Replaces");
        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        const int n = static_cast<int>(g_entries.size());
        for (int row = 0; row < n; ++row) {
            const int i = n - 1 - row; // shown highest first
            Entry& e = g_entries[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &e.enabled)) {
                save_order();
                g_dirty = true;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name.c_str());
            if (ImGui::IsItemHovered()) {
                std::string tip = e.model.empty() ? e.name : e.name + " (" + e.model + ")";
                for (const auto& f : e.files) {
                    const auto w = g_winner.find(f.disc);
                    tip += "\n" + f.disc;
                    if (w != g_winner.end() && w->second != e.name)
                        tip += "  (overridden by " + w->second + ")";
                }
                if (!e.note.empty())
                    tip += "\n" + e.note;
                ImGui::SetTooltip("%s", tip.c_str());
            }
            ImGui::TableNextColumn();
            draw_replaces(e);
            ImGui::TableNextColumn();
            changed_parts |= draw_use(e);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(row == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up))
                move_from = i, move_to = i + 1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(row == n - 1);
            if (ImGui::ArrowButton("##down", ImGuiDir_Down))
                move_from = i, move_to = i - 1;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (move_from >= 0) {
        std::swap(g_entries[static_cast<size_t>(move_from)], g_entries[static_cast<size_t>(move_to)]);
        save_order();
        g_dirty = true;
    }
    if (changed_parts && g_disc_open) { // a part chosen takes effect at once: the overlay again, the model swapped live
        save_parts();
        apply();
    }
}

} // namespace

void Launch(const std::string& folder, bool read_only) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_folder = from_utf8(folder);
        g_read_only = read_only;
        scan();
    }
    bluewake_mods_set_disc_ready(on_disc_ready);
    // BLUEWAKE_TEST_MODS_TOP=<retrace>:<entry> (testing): at that retrace, the entry goes on
    // top of the list and Apply runs, as the Mod Menu does (the player model swap without a restart).
    if (const char* env = std::getenv("BLUEWAKE_TEST_MODS_TOP"); env != nullptr && std::strchr(env, ':') != nullptr) {
        const std::string spec = env;
        const unsigned long long retrace = std::strtoull(spec.c_str(), nullptr, 10);
        const std::string name = spec.substr(spec.find(':') + 1);
        std::thread([retrace, name] {
            while (bluewake_host_retrace_count() < retrace) // a retrace number (the scenarios' clock)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = std::find_if(g_entries.begin(), g_entries.end(), [&](const Entry& e) { return e.name == name; });
            if (it == g_entries.end() || !g_disc_open)
                return;
            std::rotate(it, it + 1, g_entries.end());
            std::fprintf(stderr, "[mods] test: %s on top, Apply\n", name.c_str());
            apply();
        }).detach();
    }
    // BLUEWAKE_TEST_MODS_PARTS=<retrace>:<entry>:<L|E|B|V1|V0>[:<model in a pack>],... (testing): at that retrace the
    // entry's part choice is changed as its dropdown does, and Apply runs (the live swap, no restart).
    if (const char* env = std::getenv("BLUEWAKE_TEST_MODS_PARTS"); env != nullptr && env[0] != '\0') {
        const std::string spec = env;
        std::thread([spec] {
            std::vector<std::string> steps = split(std::regex_replace(spec, std::regex(","), "|"));
            for (const std::string& step : steps) {
                std::vector<std::string> f;
                size_t at = 0;
                for (;;) {
                    const size_t c = step.find(':', at);
                    f.push_back(step.substr(at, c == std::string::npos ? std::string::npos : c - at));
                    if (c == std::string::npos)
                        break;
                    at = c + 1;
                }
                if (f.size() < 3)
                    continue;
                const unsigned long long retrace = std::strtoull(f[0].c_str(), nullptr, 10);
                while (bluewake_host_retrace_count() < retrace)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto it = std::find_if(g_entries.begin(), g_entries.end(), [&](const Entry& e) { return e.name == f[1]; });
                if (it == g_entries.end() || !g_disc_open)
                    continue;
                if (f.size() > 3) { // the model of a pack
                    for (size_t i = 0; i < it->roots.size(); ++i)
                        if (it->root_names[i] == f[3]) {
                            Saved& sv = g_saved[it->name];
                            sv.use = it->use;
                            sv.voice = it->voice;
                            sv.root = it->roots[i];
                            classify(*it);
                            detect(*it);
                        }
                }
                if (f[2] == "V1" || f[2] == "V0")
                    it->voice = f[2] == "V1" ? 1 : 0;
                else if (f[2] == "L" || f[2] == "E" || f[2] == "B")
                    it->use = f[2][0];
                save_parts();
                std::fprintf(stderr, "[mods] test: %s uses %s%s, Apply\n", it->name.c_str(), f[2].c_str(), f.size() > 3 ? (" of " + f[3]).c_str() : "");
                apply();
            }
        }).detach();
    }
}

std::string ActiveModel() { return ShareSignature(); }

// A model's files for its colors: Link.arc, metadata.txt and color_masks\*.png, next to the Link.arc in use.
bool ActiveModelFiles(model_colors::Files& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = std::find_if(g_entries.begin(), g_entries.end(),
                                 [](const Entry& e) { return e.name == g_color_model && e.kind == "Player Model"; });
    if (g_color_model.empty() || it == g_entries.end())
        return false;
    const Entry& e = *it;
    const auto link = std::find_if(e.files.begin(), e.files.end(), [](const Mapped& m) { return m.disc == kLinkArc; });
    if (link == e.files.end())
        return false;
    const std::string root = lower(dir_of(link->member));
    std::vector<std::string> wanted; // the entry's files, in its own spelling
    for (const std::string& f : list_files(e)) {
        const std::string low = lower(f);
        if (low == lower(link->member) || low == root + "metadata.txt" ||
            (low.rfind(root + "color_masks/", 0) == 0 && low.size() > 4 && low.compare(low.size() - 4, 4, ".png") == 0))
            wanted.push_back(f);
    }
    ModArchive* a = e.archive ? mod_archive_open(utf8(e.path).c_str()) : nullptr;
    if (e.archive && a == nullptr)
        return false;
    // Archives are read in the order of their entries (a RAR or 7z is one stream).
    std::vector<std::pair<int, std::string>> order;
    for (const std::string& f : wanted)
        order.emplace_back(a ? mod_archive_find(a, f.c_str()) : 0, f);
    std::sort(order.begin(), order.end());
    out = model_colors::Files{};
    out.name = e.model;
    for (const auto& [index, f] : order) {
        std::vector<unsigned char> bytes;
        if (a) {
            unsigned long long size = 0;
            if (unsigned char* data = index >= 0 ? mod_archive_read(a, static_cast<unsigned>(index), &size) : nullptr) {
                bytes.assign(data, data + size);
                std::free(data);
            }
        } else {
            std::ifstream in(e.path / from_utf8(f), std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string low = lower(f);
        if (low == lower(link->member))
            out.link_arc = std::move(bytes);
        else if (low == root + "metadata.txt")
            out.metadata.assign(bytes.begin(), bytes.end());
        else
            out.masks[low.substr(root.size())] = std::move(bytes);
    }
    if (a)
        mod_archive_close(a);
    return !out.link_arc.empty();
}

// What the active player content is made of, for sharing it (see mods.h): the Link source's Link.arc ("model"), the
// Equipment source's Link.arc when it is another mod's ("equipment"), and the voice source's sound files ("voice").
bool ActivePlayerPack(std::string& name, std::vector<PackFile>& out, SharedParts& parts) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.clear();
    parts = SharedParts{};
    const auto find = [&](const std::string& n) -> const Entry* {
        const auto it = std::find_if(g_entries.begin(), g_entries.end(),
                                     [&](const Entry& e) { return e.name == n && e.kind == "Player Model"; });
        return n.empty() || it == g_entries.end() ? nullptr : &*it;
    };
    const Entry *link = find(g_plan_link), *equip = find(g_plan_equip), *voice = find(g_plan_voice);
    const auto read = [&](const Entry& e, const char* disc, const char* role, const char* path) {
        const Mapped* f = find_disc(e, disc);
        if (f == nullptr)
            return false;
        ModArchive* a = nullptr;
        PackFile file;
        file.role = role;
        file.path = path;
        std::error_code ec;
        const bool big = !e.archive && fs::file_size(e.path / from_utf8(f->member), ec) > 20ull * 1024 * 1024;
        if (!big)
            read_entry_file(e, a, f->member, file.bytes);
        if (a)
            mod_archive_close(a);
        if (file.bytes.empty() || file.bytes.size() > 20ull * 1024 * 1024)
            return false;
        out.push_back(std::move(file));
        return true;
    };
    if (link && read(*link, kLinkArc, "model", "Link.arc")) {
        parts.link = true;
        name = link->model;
        if (equip == link)
            parts.equipment = "model";
    }
    if (equip && equip != link && read(*equip, kLinkArc, "equipment", "equipment/Link.arc")) {
        parts.equipment = "equipment";
        if (name.empty())
            name = equip->model;
    }
    if (voice) {
        static const std::pair<const char*, const char*> kVoice[] = {{"Audiores/JaiInit.aaf", "sound/JaiInit.aaf"},
                                                                    {"Audiores/Banks/voice_0.aw", "sound/voice_0.aw"},
                                                                    {"Audiores/Banks/GanonT_0.aw", "sound/GanonT_0.aw"}};
        size_t before = out.size();
        for (const auto& [disc, path] : kVoice)
            read(*voice, disc, "voice", path);
        parts.voice = out.size() >= before + 2;
        if (!parts.voice)
            out.resize(before);
        else if (name.empty())
            name = voice->model;
    }
    return !out.empty();
}

void Frame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_voice_pending)
        return;
    int state = bluewake_voice_swap_state();
    // The game makes the swap in its frames (a title or a loading screen with no frames, a switch of the program in
    // between): not made in a minute, it is not coming.
    if (state == 1 && std::chrono::steady_clock::now() - g_voice_asked > std::chrono::seconds(60))
        state = -1;
    if (state == 2) {
        g_voice_pending = false;
        g_voice_needs_restart = false;
        g_boot_voice_id = g_voice_target_id;
        g_voice_loaded = std::move(g_voice_target);
        g_status = "The voice is changed.";
        std::fprintf(stderr, "[mods] voice changed live (%s)\n", g_boot_voice_id.empty() ? "the game's" : g_boot_voice_id.c_str());
    } else if (state < 0) {
        g_voice_pending = false;
        g_voice_needs_restart = true;
        g_voice_later = false;
        g_status = "The voice changes at the next start.";
        std::fprintf(stderr, "[mods] the voice could not be changed live; it changes at the next start\n");
    }
}

std::string LoadedVoiceId() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_boot_voice_id;
}

std::string ShareSignature() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_plan_link.empty() && g_plan_equip.empty() && g_plan_voice.empty())
        return "";
    return g_plan_link + "|" + g_plan_equip + "|" + g_plan_voice;
}

LocalParts LocalState() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return {!g_plan_link.empty(), !g_plan_equip.empty(), !g_plan_voice.empty()};
}

bool VanillaPuppetParts(std::vector<unsigned char>& body, std::map<std::string, std::vector<unsigned char>>& equipment) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::vector<unsigned char>& v = vanilla_link();
    body.clear();
    equipment.clear();
    if (v.empty() || !rarc::Extract(v, "cl.bdl", body))
        return false;
    std::vector<std::string> names = playerparts::PuppetEquipmentMembers();
    names.push_back("hands.bdl");
    names.push_back("katsura.bdl");
    for (const std::string& n : names) {
        std::vector<unsigned char> bytes;
        if (rarc::Extract(v, n, bytes))
            equipment[n] = std::move(bytes);
    }
    return true;
}

bool VanillaVoiceFiles(std::vector<unsigned char>& aaf, std::vector<unsigned char>& aw) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_disc_open)
        return false;
    aaf = disc_file("Audiores/JaiInit.aaf");
    aw = disc_file("Audiores/Banks/voice_0.aw");
    return !aaf.empty() && !aw.empty();
}

void SetModelListener(void (*fn)()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_model_listener = fn;
}

std::string Folder() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::u8string u = g_folder.u8string();  // (UTF-8)
    return std::string(u.begin(), u.end());
}

void RescanAndApply() {
    std::lock_guard<std::mutex> lock(g_mutex);
    scan();
    g_dirty = true;
    if (g_disc_open)
        apply();
}

void BuildMenu(shipmenu::Menu& m, const char* H) {
    m.AddWidget(H, "Mod Menu", 0, "Mods", WidgetType::SeparatorText);
    m.AddWidget(H, "Mod Menu", 0, "Enable Mods", WidgetType::CVarCheckbox)
        .CVar(kEnabledCVar)
        .DefaultValue(1)
        .Tooltip("Uses the enabled mods in the list. Off, the game reads only its disc.")
        .Callback([](WidgetInfo&) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_dirty = true;
        });
    m.AddWidget(H, "Mod Menu", 0, "Mod List", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        draw_list();
        std::lock_guard<std::mutex> lock(g_mutex);
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Rescan", ImVec2(half, 0))) {
            scan();
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!g_disc_open);
        if (ImGui::Button(g_dirty ? "Apply *" : "Apply", ImVec2(half, 0)))
            apply();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Puts the list in effect. A file the game has loaded already changes when it loads it "
                              "again (the next area for most; Link's model at the next game start).");
        if (!g_status.empty())
            ImGui::TextDisabled("%s", g_status.c_str());
        if (g_voice_needs_restart && !g_voice_later) {
            ImGui::Spacing();
            ImGui::TextWrapped("Voice changes need the game to restart.");
            if (ImGui::Button("Restart now", ImVec2(half, 0)))
                bw_settings_restart_now();
            ImGui::SameLine();
            if (ImGui::Button("Later", ImVec2(half, 0)))
                g_voice_later = true;
        }
    });
}

} // namespace mods
