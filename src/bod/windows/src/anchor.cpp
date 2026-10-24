// Online play: see anchor.h. SoH publishes no license; this follows the
// behaviour of its Anchor (docs/fork/research/anchor.md) with code of its own.
#include "anchor.h"
#include "check_tracker.h"
#include "rando_flow.h"

#include "anchor_client.h"
#include "anchor_content.h"
#include "anchor_carry.h"
#include "link_colors.h"
#include "mods.h"
#include "player_parts.h"
#include "voice_bank.h"

#include <memory>
#include <mutex>
#include "item_found.h"
#include "notifications.h"
#include "item_found.h"
#include "notifications.h"
#include "rarc.h"
#include "ship_menu.h"
#include "ship/cvar.h"

#define PICOJSON_USE_INT64
#include "picojson.h"

#include <chrono>
#include <cmath>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include "sea_chart.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <random>
#include "platform/bod_platform.h"

extern "C" {
#include "switch_sync.h"
#include "game_state.h"
#include <aurora/aurora.h>
#include "puppet.h"
#include "puppet_pvp.h"
#include "team_sync.h"
}

extern "C" void dol_aurora_audio_mix_voice(const int16_t* mono, uint32_t frames, float gain_left, float gain_right, float step);
extern "C" int bluewake_audio_volume_sfx_percent(void);
extern "C" bool bw_settings_menu_is_open(void);  // win_settings.cpp

namespace online {
namespace {

using shipmenu::WidgetInfo;
using shipmenu::WidgetType;

constexpr int kProtocol = 1;

struct Player {
    int id = 0;
    std::string name, team, stage;
    int room = -1;
    bool online = false, self = false;
    ImVec4 color{1, 1, 1, 1};
    std::string content; // the manifest hash of their custom player content (empty: none)
    int voice_pitch = 0; // their Link Voice Pitch in percent (0: normal), shared with the content
    int link_model = -1;      // 0 the game's Link, 1 a mod's that is shared, 2 a mod's that is not; -1 not told (see client_state)
    std::string link_colors; // their Link colors (link_colors::LocalSpec), shared with the content; empty: the game's own
    std::string client_version; // their Anchor protocol (kAnchorProtocol); a different one cannot play together
    std::string seed;           // their randomizer seed ("" in a vanilla session)
    bool save_loaded = false;   // in a game, not at the title or the file select
    // The last pose (PLAYER_UPDATE): where the puppet or marker is drawn.
    std::string pose_stage;
    int pose_room = -1;
    float pos[3]{};
    Uint64 pose_time = 0;
    bool sea_known = false; // where they are on the Sea Chart (world x, z), from CHART_UPDATE
    float sea[2]{};
    int angle_y = 0;        // their facing (the game's s16 shape angle)
    Uint64 sea_time = 0;
};

// The Anchor protocol, sent as clientVersion and compared in the room list: raise it when Anchor's messages change in a
// way older builds cannot follow.
constexpr const char* kAnchorProtocol = "bod-anchor-2026.10.18";

std::map<int, Player> g_players;
void puppet_gone(int id); // below, with the puppet content
int g_self, g_owner;
anchor::Status g_last_status = anchor::Status::Disconnected;
std::string g_notice; // the last message from the server
BluewakeLocation g_sent_location;
Uint64 g_last_state_sent;
// Team sync: on while connected and the room syncs; the team's state merged once.
bool g_sync_on, g_team_state_merged, g_team_state_requested;
Uint64 g_last_team_state_sent;
Uint64 g_last_pose_sent;
uint32_t g_last_pose_seq;
int g_room_sync = 1; // the room's syncItemsAndFlags
int g_room_teleport = 1; // the room's teleportMode: 0 nobody, 1 teammates, 2 anyone
int g_teleport_asked;    // the player whose TELEPORT_TO we wait for
// The room's player-interaction settings (roomState pvp, pvpScale, pvpFriendlyFire, playerCollision), which the owner changes.
int g_room_pvp = BLUEWAKE_PVP_OFF;
int g_room_pvp_scale = 100; // percent of the damage a hit does
bool g_room_friendly_fire;  // hits between players of one team
bool g_room_collision = true;
bool g_room_carry;          // players may lift each other (roomState allowCarrying)
bool g_item_baseline;    // item_found has the save as sync started with
Uint64 g_last_save_toast; // SYNC_SAVE's "Save updated from team", at most every 20 s

std::string cvar_string(const char* name, const char* fallback) {
    std::string s = CVarGetString(name, fallback);
    return s.empty() ? fallback : s;
}

std::string hex_color(CVarColor c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

ImVec4 parse_color(const std::string& s) {
    unsigned rgb = 0xFFFFFF;
    if (s.size() == 7 && s[0] == '#')
        std::sscanf(s.c_str() + 1, "%6x", &rgb);
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1.f);
}

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = (data[i] << 16) | (i + 1 < n ? data[i + 1] << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += i + 1 < n ? kBase64[(v >> 6) & 63] : '=';
        out += i + 2 < n ? kBase64[v & 63] : '=';
    }
    return out;
}

std::vector<unsigned char> base64_decode(const std::string& s) {
    std::vector<unsigned char> out;
    unsigned acc = 0;
    int bits = 0;
    for (const char c : s) {
        const char* at = c == '=' ? nullptr : std::strchr(kBase64, c);
        if (at == nullptr || c == '\0')
            continue;
        acc = (acc << 6) | static_cast<unsigned>(at - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 255));
        }
    }
    return out;
}

// --- Chat -----------------------------------------------------------------------------------------------------
// A text chat for the room: the CHAT message {text} goes through the room like any other (the server relays what it does not
// know). The history lasts the session; a line is the sender's name in their Anchor colour, ": ", the text in white. The Chat
// window (Network > Anchor, or popped out: gOpenWindows.Chat) shows it; while that is not open an incoming line is a toast.
constexpr size_t kChatMax = 200;
struct ChatLine {
    std::string name, text;
    ImVec4 color{1, 1, 1, 1};
};
std::vector<ChatLine> g_chat;
bool g_chat_scroll = true;
Uint64 g_chat_last_sent = 0;

void chat_add(const std::string& name, ImVec4 color, const std::string& text) {
    g_chat.push_back({name, text, color});
    if (g_chat.size() > 500)
        g_chat.erase(g_chat.begin(), g_chat.begin() + 100);
    g_chat_scroll = true;
}

bool in_global_room() { return cvar_string("gNetwork.Anchor.Room", "ww-global") == "ww-global"; }

void toast(const std::string& prefix, const std::string& message) {
    notifications::Options o;
    o.prefix = prefix;
    o.message = message;
    notifications::Emit(std::move(o));
}

// Network > Anchor > Share Custom Player Content (on by default): a player's own custom Link (model,
// voice, equipment) goes to the server and others' comes from it. Off, nothing is sent or fetched, and
// everyone else's puppet is Link's own.
bool share_content() { return CVarGetInteger("gNetwork.Anchor.ShareContent", 1) != 0; }

// The server's address for content: its WebSocket host over HTTP (host:port is a server on this PC, no TLS).
std::string http_base() {
    const std::string host = cvar_string("gNetwork.Anchor.Host", "wwserver.viosarcade.xyz");
    return (host.find(':') != std::string::npos ? "http://" : "https://") + host;
}

int local_voice_pitch() { return std::clamp(CVarGetInteger("gCosmetics.LinkVoicePitch", 0), -50, 100); }

// My colour for other players (their lists, minimap arrows, chat names): Link's hero tunic colour from the Cosmetics
// Editor, or the game's own green when it is not changed. There is no separate Anchor colour any more.
CVarColor my_color() {
    const CVarColor green{90, 178, 74, 255};
    return CVarGetInteger("gCosmetics.Link.Hero.Tunic.Changed", 0) ? CVarGetColor("gCosmetics.Link.Hero.Tunic.Value", green) : green;
}

picojson::object client_state() {
    const BluewakeLocation where = bluewake_game_location();
    picojson::object s;
    s["name"] = picojson::value(cvar_string("gNetwork.Anchor.Name", "Link"));
    s["color"] = picojson::value(hex_color(my_color()));
    s["teamId"] = picojson::value(cvar_string("gNetwork.Anchor.Team", "default"));
    s["version"] = picojson::value(std::string(BLUEWAKE_APP_NAME));
    // What SoH's room list compares (AnchorRoomWindow): the protocol, raised when Anchor's messages change, and the seed.
    s["clientVersion"] = picojson::value(std::string(kAnchorProtocol));
    s["seed"] = picojson::value(randoflow::Active() ? randoflow::Seed() : std::string());
    s["stage"] = picojson::value(std::string(where.stage));
    s["room"] = picojson::value(static_cast<int64_t>(where.room));
    s["point"] = picojson::value(static_cast<int64_t>(where.point));
    s["isSaveLoaded"] = picojson::value(where.stage[0] != '\0');
    s["content"] = picojson::value(share_content() ? content::LocalHash() : std::string());
    // Which body the expression sent with the pose is for: 0 Link as the game has him, 1 a mod's Link that is shared (the content's
    // body), 2 a mod's Link that is not (others cannot show its expression on any model they have). An old client sends nothing.
    s["linkModel"] = picojson::value(static_cast<int64_t>(!mods::LocalState().link ? 0 : (share_content() && content::LocalSharesBody() ? 1 : 2)));
    // Link Voice Pitch (Cosmetics Editor), shared as the content is: others play this player's puppet's voice at it. An old
    // client ignores the field.
    s["voicePitch"] = picojson::value(static_cast<int64_t>(share_content() ? local_voice_pitch() : 0));
    // Link's colors from the Cosmetics Editor (only the changed ones), for the puppet: others' puppets wear them. An old client
    // ignores the field.
    s["linkColors"] = picojson::value(share_content() ? link_colors::LocalSpec() : std::string());
    return s;
}

void send(picojson::object message) { anchor::Send(picojson::value(message).serialize()); }

// This install's token for the server (room.js handshake): a game that crashes or restarts gets its seat back instead of
// a new id beside a stale offline entry. A random value kept in the settings, mixed with the PC's machine id (MachineGuid) so a
// settings file copied to another PC is not the same player. Only the server sees it.
std::string install_token() {
    std::string saved = cvar_string("gNetwork.Anchor.Token", "");
    if (saved.size() != 32) {
        std::random_device rd;
        char hex[33];
        for (int i = 0; i < 4; ++i)
            std::snprintf(hex + 8 * i, 9, "%08x", rd());
        saved.assign(hex, 32);
        CVarSetString("gNetwork.Anchor.Token", saved.c_str());
        CVarSaveNextFrame();
    }
    const std::string guid = bodplat::MachineId(); // the Windows MachineGuid; /etc/machine-id on Linux
    uint64_t h = 1469598103934665603ull; // FNV-1a over the saved value and the machine's
    for (const char c : saved + "|" + guid)
        h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(h));
    return saved.substr(0, 16) + out;
}

void handshake() {
    picojson::object m;
    m["type"] = picojson::value("HANDSHAKE");
    m["protocol"] = picojson::value(static_cast<int64_t>(kProtocol));
    if (g_self)
        m["clientId"] = picojson::value(static_cast<int64_t>(g_self));
    static const std::string token = install_token();
    m["token"] = picojson::value(token);
    m["clientState"] = picojson::value(client_state());
    send(m);
    g_sent_location = bluewake_game_location();
    g_last_state_sent = SDL_GetTicks();
}

void send_state() {
    picojson::object m;
    m["type"] = picojson::value("UPDATE_CLIENT_STATE");
    m["state"] = picojson::value(client_state());
    send(m);
    g_sent_location = bluewake_game_location();
    g_last_state_sent = SDL_GetTicks();
}

// The owner's change to the room's player-interaction settings: to the server, which keeps them and sends them to everyone
// (they come back through UPDATE_ROOM_STATE, so the globals above are the server's word).
void send_room_state() {
    picojson::object r;
    r["pvp"] = picojson::value(std::string(g_room_pvp == BLUEWAKE_PVP_ON ? "on" : g_room_pvp == BLUEWAKE_PVP_NO_DAMAGE ? "no_damage" : "off"));
    r["pvpScale"] = picojson::value(g_room_pvp_scale / 100.0);
    r["pvpFriendlyFire"] = picojson::value(static_cast<int64_t>(g_room_friendly_fire ? 1 : 0));
    r["playerCollision"] = picojson::value(static_cast<int64_t>(g_room_collision ? 1 : 0));
    r["allowCarrying"] = picojson::value(static_cast<int64_t>(g_room_carry ? 1 : 0));
    picojson::object m;
    m["type"] = picojson::value("UPDATE_ROOM_STATE");
    m["roomState"] = picojson::value(r);
    send(m);
}

void connect() {
    anchor::Options o;
    std::string host = cvar_string("gNetwork.Anchor.Host", "wwserver.viosarcade.xyz");
    // "host:port" for a local server (ws://, no TLS).
    if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        o.port = static_cast<unsigned short>(std::atoi(host.c_str() + colon + 1));
        host.resize(colon);
        o.secure = false;
    }
    o.host = host;
    o.room = cvar_string("gNetwork.Anchor.Room", "ww-global");
    anchor::Start(o);
    CVarSetInteger("gNetwork.Anchor.Enabled", 1);
    CVarSaveNextFrame();
}

void disconnect() {
    anchor::Stop();
    g_players.clear();
    g_self = g_owner = 0;
    g_room_pvp = BLUEWAKE_PVP_OFF;
    g_room_pvp_scale = 100;
    g_room_friendly_fire = false;
    g_room_collision = true;
    CVarSetInteger("gNetwork.Anchor.Enabled", 0);
    CVarSaveNextFrame();
}

picojson::array changes_json(const BluewakeSyncChange* c, unsigned n) {
    picojson::array a;
    for (unsigned i = 0; i < n; ++i) {
        picojson::array pair;
        pair.emplace_back(static_cast<int64_t>(c[i].offset));
        pair.emplace_back(static_cast<int64_t>(c[i].value));
        a.emplace_back(pair);
    }
    return a;
}

void give_changes(const picojson::value& v) {
    if (!v.is<picojson::array>())
        return;
    std::vector<BluewakeSyncChange> changes;
    for (const auto& e : v.get<picojson::array>()) {
        if (!e.is<picojson::array>() || e.get<picojson::array>().size() != 2)
            continue;
        const auto& pair = e.get<picojson::array>();
        if (!pair[0].is<int64_t>() || !pair[1].is<int64_t>())
            continue;
        changes.push_back(BluewakeSyncChange{static_cast<uint16_t>(pair[0].get<int64_t>()),
                                             static_cast<uint8_t>(pair[1].get<int64_t>())});
    }
    if (!changes.empty()) {
        checktracker::NoteTeamSave();
        bluewake_team_sync_give(changes.data(), static_cast<unsigned>(changes.size()));
    }
}

// This game's seed for team sync ("" for a vanilla save), and whether a message names another (older builds send none).
std::string my_seed() { return randoflow::Active() ? randoflow::Seed() : std::string(); }
bool other_seed(const picojson::object& m) {
    return m.count("seed") && m.at("seed").is<std::string>() && m.at("seed").get<std::string>() != my_seed();
}

void send_team_state(int target) {
    BluewakeSyncChange snapshot[BLUEWAKE_SYNC_SNAPSHOT_MAX];
    const unsigned n = bluewake_team_sync_snapshot(snapshot, BLUEWAKE_SYNC_SNAPSHOT_MAX);
    if (n == 0)
        return;
    picojson::object m;
    m["type"] = picojson::value("UPDATE_TEAM_STATE");
    m["seed"] = picojson::value(my_seed());
    m["state"] = picojson::value(changes_json(snapshot, n));
    if (target)
        m["targetClientId"] = picojson::value(static_cast<int64_t>(target));
    send(m);
    g_last_team_state_sent = SDL_GetTicks();
}

void handle(const std::string& text);

// Whether we and that player may teleport to each other (the room's rule, both in game).
bool can_teleport_between(const Player& p) {
    if (g_room_teleport == 0 || p.self || !p.online || p.stage.empty())
        return false;
    if (bluewake_game_location().stage[0] == '\0')
        return false;
    if (g_room_teleport == 1) {
        const auto self = g_players.find(g_self);
        if (self == g_players.end() || self->second.team != p.team)
            return false;
    }
    return true;
}

void request_teleport(int id) {
    // One a few seconds: a double click on the arrow asked twice, and the second teleport landed in the middle of the
    // first's scene change (the host drops such a one too).
    static Uint64 last_ask;
    if (last_ask != 0 && SDL_GetTicks() - last_ask < 3000)
        return;
    last_ask = SDL_GetTicks();
    picojson::object m;
    m["type"] = picojson::value("REQUEST_TELEPORT");
    m["targetClientId"] = picojson::value(static_cast<int64_t>(id));
    send(m);
    g_teleport_asked = id;
}

// Whether a hit between me and that player counts: the room has PvP on, and they are not on my team unless the room allows
// friendly fire.
bool in_my_scene(const Player& p, const BluewakeLocation& where, Uint64 now);
bool pvp_allowed(const Player& p);

carry::Env carry_env() {
    carry::Env e;
    e.send = [](picojson::object m) { send(std::move(m)); };
    e.in_my_scene = [](int id) {
        const auto it = g_players.find(id);
        return it != g_players.end() && in_my_scene(it->second, bluewake_game_location(), SDL_GetTicks());
    };
    e.hurt_allowed = [](int id) {
        const auto it = g_players.find(id);
        return it != g_players.end() && g_room_pvp == BLUEWAKE_PVP_ON && pvp_allowed(it->second);
    };
    e.name = [](int id) {
        const auto it = g_players.find(id);
        return it != g_players.end() ? it->second.name : std::string("Player");
    };
    e.toast = [](const std::string& who, const std::string& what) { toast(who, what); };
    e.hit = [](int from, const float pos[3]) { bluewake_pvp_incoming(from, 1, 0x2u, pos); };
    return e;
}

bool pvp_allowed(const Player& p) {
    if (g_room_pvp == BLUEWAKE_PVP_OFF || p.self)
        return false;
    return g_room_friendly_fire || p.team != cvar_string("gNetwork.Anchor.Team", "default");
}

void handle(const std::string& text) {
    picojson::value v;
    if (!picojson::parse(v, text).empty() || !v.is<picojson::object>())
        return;
    const auto& m = v.get<picojson::object>();
    const std::string type = m.count("type") ? m.at("type").to_str() : "";
    if (type == "ALL_CLIENT_STATE" && m.count("state") && m.at("state").is<picojson::array>()) {
        const std::map<int, Player> before = std::move(g_players);
        g_players.clear();
        for (const auto& e : m.at("state").get<picojson::array>()) {
            if (!e.is<picojson::object>())
                continue;
            const auto& c = e.get<picojson::object>();
            Player p;
            p.id = c.count("clientId") ? static_cast<int>(c.at("clientId").get<int64_t>()) : 0;
            p.name = c.count("name") ? c.at("name").to_str() : "?";
            p.team = c.count("teamId") ? c.at("teamId").to_str() : "default";
            p.stage = c.count("stage") ? c.at("stage").to_str() : "";
            p.room = c.count("room") && c.at("room").is<int64_t>() ? static_cast<int>(c.at("room").get<int64_t>()) : -1;
            p.online = c.count("online") && c.at("online").is<bool>() && c.at("online").get<bool>();
            p.self = c.count("self") && c.at("self").is<bool>() && c.at("self").get<bool>();
            p.color = parse_color(c.count("color") ? c.at("color").to_str() : "");
            p.content = c.count("content") && c.at("content").is<std::string>() ? c.at("content").get<std::string>() : "";
            p.voice_pitch = c.count("voicePitch") && c.at("voicePitch").is<int64_t>()
                                ? std::clamp(static_cast<int>(c.at("voicePitch").get<int64_t>()), -50, 100) : 0;
            p.link_model = c.count("linkModel") && c.at("linkModel").is<int64_t>() ? static_cast<int>(c.at("linkModel").get<int64_t>()) : -1;
            p.link_colors = c.count("linkColors") && c.at("linkColors").is<std::string>() ? c.at("linkColors").get<std::string>() : "";
            p.client_version = c.count("clientVersion") && c.at("clientVersion").is<std::string>() ? c.at("clientVersion").get<std::string>() : "";
            p.seed = c.count("seed") && c.at("seed").is<std::string>() ? c.at("seed").get<std::string>() : "";
            p.save_loaded = c.count("isSaveLoaded") && c.at("isSaveLoaded").is<bool>() && c.at("isSaveLoaded").get<bool>();
            if (p.self)
                g_self = p.id;
            if (const auto old = before.find(p.id); old != before.end()) {
                p.pose_stage = old->second.pose_stage;
                p.pose_room = old->second.pose_room;
                std::copy(std::begin(old->second.pos), std::end(old->second.pos), p.pos);
                p.pose_time = old->second.pose_time;
            }
            g_players[p.id] = p;
        }
        for (const auto& [id, old] : before)
            if (g_players.find(id) == g_players.end())
                puppet_gone(id);
        // Offline players keep their place in the list but not a puppet slot (16): a player who reconnects comes back
        // under a new id, and the old ids' slots filled up until a newcomer got only a nametag.
        for (const auto& [id, p] : g_players)
            if (!p.online && !p.self)
                puppet_gone(id);
        // The session log: who came, went online or offline.
        for (const auto& [id, p] : g_players) {
            const auto old = before.find(id);
            // SoH's toast: not in the global room, not for ourselves, not for who was here when we came.
            if (!p.self && !before.empty() && !in_global_room() &&
                (old == before.end() ? p.online : old->second.online != p.online))
                toast(p.name, p.online ? "Connected" : "Disconnected");
            if (old == before.end() || old->second.online != p.online)
                std::fprintf(stderr, "[anchor] player %d %s%s (team %s) is %s\n", id, p.name.c_str(),
                             p.self ? " (you)" : "", p.team.c_str(), p.online ? "online" : "offline");
        }
    } else if (type == "UPDATE_ROOM_STATE") {
        if (m.count("ownerClientId") && m.at("ownerClientId").is<int64_t>())
            g_owner = static_cast<int>(m.at("ownerClientId").get<int64_t>());
        if (m.count("roomState") && m.at("roomState").is<picojson::object>()) {
            const auto& r = m.at("roomState").get<picojson::object>();
            if (r.count("syncItemsAndFlags") && r.at("syncItemsAndFlags").is<int64_t>())
                g_room_sync = static_cast<int>(r.at("syncItemsAndFlags").get<int64_t>());
            if (r.count("teleportMode") && r.at("teleportMode").is<int64_t>())
                g_room_teleport = static_cast<int>(r.at("teleportMode").get<int64_t>());
            // A room from before player interaction has none of these: off, normal damage, solid players.
            const std::string pvp = r.count("pvp") && r.at("pvp").is<std::string>() ? r.at("pvp").get<std::string>() : "off";
            g_room_pvp = pvp == "on" ? BLUEWAKE_PVP_ON : pvp == "no_damage" ? BLUEWAKE_PVP_NO_DAMAGE : BLUEWAKE_PVP_OFF;
            g_room_pvp_scale = r.count("pvpScale") && r.at("pvpScale").is<double>()
                                   ? std::clamp(static_cast<int>(std::lround(r.at("pvpScale").get<double>() * 100.0)), 25, 400) : 100;
            g_room_friendly_fire = r.count("pvpFriendlyFire") && r.at("pvpFriendlyFire").is<int64_t>() && r.at("pvpFriendlyFire").get<int64_t>() != 0;
            g_room_collision = !r.count("playerCollision") || !r.at("playerCollision").is<int64_t>() || r.at("playerCollision").get<int64_t>() != 0;
            g_room_carry = r.count("allowCarrying") && r.at("allowCarrying").is<int64_t>() && r.at("allowCarrying").get<int64_t>() != 0;
        }
    } else if (type == "PLAYER_UPDATE") {
        const int from = m.count("clientId") && m.at("clientId").is<int64_t>() ? static_cast<int>(m.at("clientId").get<int64_t>()) : 0;
        const auto it = g_players.find(from);
        if (it == g_players.end() || !m.count("pos") || !m.at("pos").is<picojson::array>())
            return;
        const auto& p = m.at("pos").get<picojson::array>();
        if (p.size() != 3)
            return;
        for (int i = 0; i < 3; ++i)
            it->second.pos[i] = p[i].is<double>() ? static_cast<float>(p[i].get<double>()) : 0.f;
        it->second.pose_stage = m.count("stage") ? m.at("stage").to_str() : "";
        it->second.pose_room = m.count("room") && m.at("room").is<int64_t>() ? static_cast<int>(m.at("room").get<int64_t>()) : -1;
        it->second.pose_time = SDL_GetTicks();
        // The joint matrices, for the puppet (older clients send none: a marker).
        if (m.count("pose") && m.at("pose").is<std::string>()) {
            std::vector<unsigned char> wire = base64_decode(m.at("pose").get<std::string>());
            carry::fix_pose(from, wire); // a player I carry is shown at my hands
            if (!wire.empty())
                bluewake_puppet_remote_update(from, wire.data(), wire.size());
        }
    } else if (type == "CARRY_START" || type == "CARRY_POS" || type == "CARRY_END") {
        const int from = m.count("clientId") && m.at("clientId").is<int64_t>() ? static_cast<int>(m.at("clientId").get<int64_t>()) : 0;
        if (g_players.count(from) != 0 && !g_players.at(from).self)
            carry::handle(type, m, from, carry_env());
    } else if (type == "PVP_HIT") {
        // Another player's game says it hit mine. The room's mode and the teams' friendly-fire setting decide here too (the
        // server drops hits while PvP is off, but a hit is the target's game's to take or leave).
        const int from = m.count("clientId") && m.at("clientId").is<int64_t>() ? static_cast<int>(m.at("clientId").get<int64_t>()) : 0;
        const auto it = g_players.find(from);
        if (it == g_players.end() || it->second.self || g_room_pvp == BLUEWAKE_PVP_OFF || !pvp_allowed(it->second))
            return;
        const int power = m.count("damage") && m.at("damage").is<int64_t>() ? std::clamp(static_cast<int>(m.at("damage").get<int64_t>()), 0, 64) : 1;
        const uint32_t hit_type = m.count("hitType") && m.at("hitType").is<int64_t>() ? static_cast<uint32_t>(m.at("hitType").get<int64_t>()) : 0u;
        float at[3];
        std::copy(std::begin(it->second.pos), std::end(it->second.pos), at);
        if (m.count("pos") && m.at("pos").is<picojson::array>() && m.at("pos").get<picojson::array>().size() == 3)
            for (int i = 0; i < 3; ++i) {
                const picojson::value& c = m.at("pos").get<picojson::array>()[i];
                at[i] = c.is<double>() ? static_cast<float>(c.get<double>()) : at[i];
            }
        std::fprintf(stderr, "[anchor] PVP_HIT from %s (%d): power %d, type %08X\n", it->second.name.c_str(), from, power, hit_type);
        bluewake_pvp_incoming(from, power, hit_type, at);
    } else if (type == "CHART_UPDATE") {
        // Where a player is on the Sea Chart, wherever they are (PLAYER_UPDATE reaches only the same stage).
        const int from = m.count("clientId") && m.at("clientId").is<int64_t>() ? static_cast<int>(m.at("clientId").get<int64_t>()) : 0;
        const auto it = g_players.find(from);
        if (it == g_players.end() || !m.count("sea") || !m.at("sea").is<picojson::array>() || m.at("sea").get<picojson::array>().size() != 2)
            return;
        const auto& s = m.at("sea").get<picojson::array>();
        if (!s[0].is<double>() || !s[1].is<double>())
            return;
        Player& pl = it->second;
        pl.sea[0] = static_cast<float>(s[0].get<double>());
        pl.sea[1] = static_cast<float>(s[1].get<double>());
        pl.angle_y = m.count("angle") && m.at("angle").is<int64_t>() ? static_cast<int>(m.at("angle").get<int64_t>()) : 0;
        pl.sea_known = true;
        pl.sea_time = SDL_GetTicks();
    } else if (type == "REQUEST_TELEPORT") {
        // Another player asks where we are: answer with Link's place.
        const int from = m.count("clientId") && m.at("clientId").is<int64_t>() ? static_cast<int>(m.at("clientId").get<int64_t>()) : 0;
        const auto it = g_players.find(from);
        const BluewakeLocation where = bluewake_game_location();
        if (it != g_players.end() && can_teleport_between(it->second) && where.has_player) {
            picojson::object a;
            a["type"] = picojson::value("TELEPORT_TO");
            a["targetClientId"] = picojson::value(static_cast<int64_t>(from));
            a["stage"] = picojson::value(std::string(where.stage));
            // Where Link last stood on the ground (not mid-jump, on a ledge's edge in the air or over a pit), in the room he
            // stood in: a teleport to the place he is just now could land in the air and fall into the void.
            const bool ground = where.has_ground;
            const float* at = ground ? where.ground_pos : where.pos;
            const int room = ground && where.ground_room >= 0 ? where.ground_room : where.room;
            a["room"] = picojson::value(static_cast<int64_t>(room));
            a["point"] = picojson::value(static_cast<int64_t>(where.point));
            a["pos"] = picojson::value(picojson::array{picojson::value(double(at[0])), picojson::value(double(at[1])),
                                                       picojson::value(double(at[2]))});
            a["angle"] = picojson::value(static_cast<int64_t>(ground ? where.ground_angle_y : where.angle_y));
            if (where.sailing) {
                // Sailing: the answer is the ship's place (beside it, so the arriving player's ship does not overlap), and
                // the arriving player is put on a ship there, not in the water.
                a["sailing"] = picojson::value(true);
                a["shipPos"] = picojson::value(picojson::array{picojson::value(double(where.ship_pos[0] + 250.f)), picojson::value(double(where.ship_pos[1])),
                                                              picojson::value(double(where.ship_pos[2]))});
                a["shipAngle"] = picojson::value(static_cast<int64_t>(where.ship_angle_y));
            }
            send(a);
        }
    } else if (type == "TELEPORT_TO") {
        const int from = m.count("clientId") && m.at("clientId").is<int64_t>() ? static_cast<int>(m.at("clientId").get<int64_t>()) : 0;
        if (from != g_teleport_asked || !m.count("stage") || !m.count("pos") || !m.at("pos").is<picojson::array>())
            return; // only the answer we asked for
        g_teleport_asked = 0;
        const auto& p = m.at("pos").get<picojson::array>();
        const std::string stage = m.at("stage").to_str();
        if (p.size() != 3 || stage.empty() || stage.size() > 7)
            return;
        float pos[3];
        for (int i = 0; i < 3; ++i)
            pos[i] = p[i].is<double>() ? static_cast<float>(p[i].get<double>()) : 0.f;
        const int room = m.count("room") && m.at("room").is<int64_t>() ? static_cast<int>(m.at("room").get<int64_t>()) : 0;
        const int angle = m.count("angle") && m.at("angle").is<int64_t>() ? static_cast<int>(m.at("angle").get<int64_t>()) : 0;
        if (m.count("sailing") && m.at("sailing").is<bool>() && m.at("sailing").get<bool>() && m.count("shipPos") &&
            m.at("shipPos").is<picojson::array>() && m.at("shipPos").get<picojson::array>().size() == 3) {
            const auto& sp = m.at("shipPos").get<picojson::array>();
            float ship_pos[3];
            for (int i = 0; i < 3; ++i)
                ship_pos[i] = sp[i].is<double>() ? static_cast<float>(sp[i].get<double>()) : 0.f;
            const int ship_angle = m.count("shipAngle") && m.at("shipAngle").is<int64_t>() ? static_cast<int>(m.at("shipAngle").get<int64_t>()) : 0;
            bluewake_request_teleport_ship(stage.c_str(), room, pos, angle, ship_pos, ship_angle);
        } else
            bluewake_request_teleport(stage.c_str(), room, pos, angle);
        std::fprintf(stderr, "[anchor] teleporting to player %d: %s room %d\n", from, stage.c_str(), room);
    } else if (type == "ITEM_FOUND") {
        // A teammate got an item (its own save's diff named it): "<name> found <icon> <item>".
        const int from = m.count("clientId") && m.at("clientId").is<int64_t>() ? static_cast<int>(m.at("clientId").get<int64_t>()) : 0;
        const auto it = g_players.find(from);
        if (it == g_players.end() || !m.count("itemId") || !m.at("itemId").is<int64_t>())
            return;
        const int item = static_cast<int>(m.at("itemId").get<int64_t>());
        if (item < 0 || item > 0xFF || item_found::IsJunk(item))
            return;
        notifications::Options o;
        o.icon = item_found::Icon(item);
        o.prefix = it->second.name;
        o.message = "found";
        o.suffix = item_found::Name(item);
        notifications::Emit(std::move(o));
    } else if (type == "CHAT") {
        const int from = m.count("clientId") && m.at("clientId").is<int64_t>() ? static_cast<int>(m.at("clientId").get<int64_t>()) : 0;
        const auto it = g_players.find(from);
        std::string text = m.count("text") ? m.at("text").to_str() : "";
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return c < 32; }), text.end());
        if (text.empty())
            return;
        if (text.size() > kChatMax)
            text.resize(kChatMax);
        const std::string name = it != g_players.end() ? it->second.name : "?";
        const ImVec4 color = it != g_players.end() ? it->second.color : ImVec4(1, 1, 1, 1);
        chat_add(name, color, text);
        if (CVarGetInteger("gOpenWindows.Chat", 0) == 0) {
            notifications::Options o;
            o.prefix = name;
            o.prefixColor = color;
            o.message = text;
            o.messageColor = ImVec4(1, 1, 1, 1);
            notifications::Emit(std::move(o));
        }
    } else if ((type == "SYNC_SAVE" || type == "UPDATE_TEAM_STATE") && other_seed(m)) {
        // A teammate's save of another seed (or a vanilla save against a randomizer one): its items and flags are not
        // this game's, and merging them broke a new file (user 2026-10-17: sword and shield unequipped, Hero's Charm).
        std::fprintf(stderr, "[anchor] %s ignored: from seed \"%s\", this game's is \"%s\"\n", type.c_str(),
                     m.at("seed").to_str().c_str(), my_seed().c_str());
    } else if (type == "SET_SWITCH") {
        // A teammate set or cleared a switch: set here too when this game is in the same place (switch_sync.c).
        if (g_sync_on && !other_seed(m) && m.count("stage") && m.count("no") && m.at("no").is<int64_t>()) {
            BluewakeSwitchEvent e{};
            const std::string stage = m.at("stage").to_str();
            std::snprintf(e.stage, sizeof e.stage, "%s", stage.c_str());
            e.room = static_cast<int8_t>(m.count("room") && m.at("room").is<int64_t>() ? m.at("room").get<int64_t>() : -1);
            e.no = static_cast<uint8_t>(m.at("no").get<int64_t>());
            e.on = !m.count("on") || !m.at("on").is<bool>() || m.at("on").get<bool>();
            bluewake_switch_sync_give(&e);
        }
    } else if (type == "SYNC_SAVE") {
        std::fprintf(stderr, "[anchor] SYNC_SAVE from %lld%s\n",
                     m.count("clientId") && m.at("clientId").is<int64_t>() ? (long long)m.at("clientId").get<int64_t>() : 0LL,
                     g_sync_on ? "" : " (sync off: ignored)");
        if (g_sync_on && m.count("changes")) {
            give_changes(m.at("changes"));
            if (SDL_GetTicks() - g_last_save_toast > 20000) {
                g_last_save_toast = SDL_GetTicks();
                toast("", "Save updated from team");
            }
        }
    } else if (type == "UPDATE_TEAM_STATE") {
        // The team's state (a teammate's, or the server's copy) and what was queued since.
        if (g_sync_on) {
            if (m.count("state"))
                give_changes(m.at("state"));
            if (m.count("queue") && m.at("queue").is<picojson::array>())
                for (const auto& q : m.at("queue").get<picojson::array>())
                    if (q.is<std::string>())
                        handle(q.get<std::string>());
            g_team_state_merged = true;
            toast("", "Save updated from team"); // SoH's: after the team's state is applied
            std::fprintf(stderr, "[anchor] team state merged\n");
        }
    } else if (type == "REQUEST_TEAM_STATE") {
        if (g_sync_on && m.count("clientId") && m.at("clientId").is<int64_t>())
            send_team_state(static_cast<int>(m.at("clientId").get<int64_t>()));
    } else if (type == "ERROR" || type == "SERVER_MESSAGE") {
        g_notice = m.count("message") ? m.at("message").to_str() : type;
        if (type == "SERVER_MESSAGE") {
            notifications::Options o;
            o.prefix = "Server:";
            o.prefixColor = ImVec4(1.0f, 0.5f, 0.5f, 1.0f);
            o.message = g_notice;
            notifications::Emit(std::move(o));
        }
        std::fprintf(stderr, "[anchor] %s: %s\n", type.c_str(), g_notice.c_str());
    }
    // Item, flag and pose messages arrive here once N3 sends them.
}

// Ten times a second while in play: where Link is, for the other players on
// this stage (their markers, later their puppets).
void send_pose() {
    const BluewakeLocation where = bluewake_game_location();
    if (!where.has_player || SDL_GetTicks() - g_last_pose_sent < 100)
        return;
    picojson::object m;
    m["type"] = picojson::value("PLAYER_UPDATE");
    m["stage"] = picojson::value(std::string(where.stage));
    m["room"] = picojson::value(static_cast<int64_t>(where.stay_room)); // the room Link stands in
    m["pos"] = picojson::value(picojson::array{picojson::value(double(where.pos[0])), picojson::value(double(where.pos[1])),
                                               picojson::value(double(where.pos[2]))});
    m["angle"] = picojson::value(picojson::array{picojson::value(int64_t{0}), picojson::value(static_cast<int64_t>(where.angle_y)),
                                                 picojson::value(int64_t{0})});
    // Link's joint matrices as the game has them, for the other players' puppets.
    uint8_t wire[BLUEWAKE_PUPPET_MAX_WIRE];
    uint32_t seq = 0;
    const size_t n = bluewake_puppet_local_pose(wire, sizeof wire, &seq);
    if (n != 0 && seq != g_last_pose_seq) {
        g_last_pose_seq = seq;
        m["pose"] = picojson::value(base64_encode(wire, n));
    }
    send(m);
    g_last_pose_sent = SDL_GetTicks();
}

// Five times a second: where Link is on the Sea Chart (at sea his place, ashore the island's square) and which way he
// faces, for the other players' charts. A message of its own because PLAYER_UPDATE reaches only the same stage.
void send_chart() {
    static Uint64 last;
    const BluewakeLocation where = bluewake_game_location();
    if (!where.has_player || !where.sea_known || SDL_GetTicks() - last < 200)
        return;
    last = SDL_GetTicks();
    picojson::object m;
    m["type"] = picojson::value("CHART_UPDATE");
    m["sea"] = picojson::value(picojson::array{picojson::value(double(where.sea_x)), picojson::value(double(where.sea_z))});
    m["angle"] = picojson::value(static_cast<int64_t>(where.angle_y));
    send(m);
}

// Whether a remote player is in the scene the local player is in: on this
// stage, in this room (at sea the room is the sector, and neighbours show), and
// heard from lately.
bool in_my_scene(const Player& p, const BluewakeLocation& where, Uint64 now) {
    if (p.self || !p.online || where.stage[0] == '\0' || p.pose_stage != where.stage || now - p.pose_time > 2000)
        return false;
    return p.pose_room < 0 || p.pose_room == where.stay_room || p.pose_stage == "sea";
}

// The other players on this stage. With Link models on, each shows as a
// puppet and the marker is only its name above its head; where no puppet is
// drawn (the setting off, or one not made yet) a dot in the player's color
// above where they stand, and the name (the camera's matrix from the game, a
// frame behind).
void draw_markers() {
    if (!CVarGetInteger("gNetwork.Anchor.ShowPlayers", 1))
        return;
    const BluewakeLocation where = bluewake_game_location();
    if (!where.has_view || where.stage[0] == '\0')
        return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const Uint64 now = SDL_GetTicks();
    for (const auto& [id, p] : g_players) {
        if (!in_my_scene(p, where, now))
            continue;
        float head[3] = {p.pos[0], p.pos[1] + 150.f, p.pos[2]}; // above Link's head
        const bool model = bluewake_puppet_drawn(id, head);
        const float* m = where.proj_view;
        const float v[4] = {head[0], head[1], head[2], 1.f};
        float clip[4];
        for (int r = 0; r < 4; ++r)
            clip[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * v[3];
        ImVec2 at((clip[0] / clip[3] * 0.5f + 0.5f) * size.x, (0.5f - clip[1] / clip[3] * 0.5f) * size.y);
        // A puppet's tag follows what the renderer shows: the head against the camera of each of the last two game frames
        // (not this frame's head with the camera of the frame before), at the weight of the frame being drawn between them
        // (the renderer's own blend: 0.5 for the first in-between frame at 60 Hz, 0.25 / 0.5 / 0.75 at 120 Hz, 1 for a real one).
        float ndc[3];
        const float tag_weight = aurora_get_frame_interpolation() ? aurora_get_frame_blend() : 1.f;
        if (model && bluewake_puppet_tag(id, tag_weight, ndc)) {
            static const bool log_weights = std::getenv("BLUEWAKE_TAG_LOG") != nullptr; // testing: the weights the tags were drawn at
            static int logged_weights;
            if (log_weights && logged_weights++ < 40)
                std::fprintf(stderr, "[anchor] tag weight %.3f\n", static_cast<double>(tag_weight));
            clip[3] = ndc[2];
            at = ImVec2((ndc[0] * 0.5f + 0.5f) * size.x, (0.5f - ndc[1] * 0.5f) * size.y);
        }
        if (!(clip[3] > 1.f))
            continue; // behind the camera
        if (at.x < -100 || at.x > size.x + 100 || at.y < -100 || at.y > size.y + 100)
            continue;
        const float radius = model ? 0.f : std::clamp(4000.f / clip[3], 4.f, 12.f) * size.y / 480.f;
        if (!model) {
            const ImU32 color = ImGui::ColorConvertFloat4ToU32(p.color);
            draw->AddCircleFilled(at, radius, color);
            draw->AddCircle(at, radius, IM_COL32(0, 0, 0, 200), 0, 2.f);
        }
        // The name tag, as a small window of its own (text in the background draw list comes out tiny).
        ImGui::SetNextWindowPos(ImVec2(at.x, at.y - radius - 2.f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
        ImGui::SetNextWindowBgAlpha(0.45f);
        const std::string window_name = "##anchor_tag" + std::to_string(id);
        const ImGuiWindowFlags tag_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.f, 2.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
        if (ImGui::Begin(window_name.c_str(), nullptr, tag_flags)) {
            ImGui::SetWindowFontScale(1.35f);
            ImGui::PushStyleColor(ImGuiCol_Text, model ? ImVec4(p.color.x * 0.4f + 0.6f, p.color.y * 0.4f + 0.6f, p.color.z * 0.4f + 0.6f, 1.f)
                                                       : ImVec4(1, 1, 1, 1));
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        const float fs = ImGui::GetFontSize();
        static std::map<int, bool> logged; // the first marker, and the first puppet, per player, for the scenarios
        if (const auto at_log = logged.find(id); at_log == logged.end() || (model && !at_log->second)) {
            logged[id] = model;
            std::fprintf(stderr, "[anchor] tag: display %.0fx%.0f, font %.1f, tag %.1f\n", size.x, size.y, ImGui::GetFontSize(), fs);
            std::fprintf(stderr, "[anchor] %s for %s at %.0f, %.0f\n", model ? "puppet" : "marker", p.name.c_str(), at.x, at.y);
        }
    }
}

// Each frame: the room's player-interaction rules to the host, and the hits my attacks made on puppets to the players hit
// (PVP_HIT). BLUEWAKE_PVP_TEST=<mode>[,<scale %>] (testing, with BLUEWAKE_PUPPET_SELF): the copy of Link is solid, targetable
// and hittable under those rules, and the hits are only logged; BLUEWAKE_PVP_TEST_HIT=<seconds> also has the copy hit me
// that often.
void pvp_frame(bool connected, bool self_set) {
    static const char* test = std::getenv("BLUEWAKE_PVP_TEST");
    carry::set_room_allowed(connected && g_room_carry);
    carry::frame(connected, carry_env());
    {
        // A bomb another player threw went off beside me (the host made a harmless copy of it): it hurts only where the room's PvP
        // rules let that player hit me (on, and not a teammate unless friendly fire is on); a bomb's hit is the game's own fixed one.
        int from = 0;
        float at[3];
        if (bluewake_puppet_take_blast(&from, at) && connected) {
            const auto it = g_players.find(from);
            if (it != g_players.end() && g_room_pvp == BLUEWAKE_PVP_ON && pvp_allowed(it->second)) {
                std::fprintf(stderr, "[anchor] player %d's bomb went off beside me: PvP is on, I am hit\n", from);
                bluewake_pvp_incoming(from, 1, 0x20u, at);
            } else
                std::fprintf(stderr, "[anchor] player %d's bomb went off beside me: no hit under the room's rules\n", from);
        }
    }
    if (connected) {
        bluewake_pvp_set_rules(g_room_collision, g_room_pvp, g_room_pvp_scale);
        const BluewakeLocation where = bluewake_game_location();
        const Uint64 now = SDL_GetTicks();
        for (const auto& [id, p] : g_players) {
            if (p.self)
                continue;
            if (p.online && in_my_scene(p, where, now))
                bluewake_pvp_set_target(id, pvp_allowed(p));
            else
                bluewake_pvp_remove(id);
        }
    } else if (self_set && test != nullptr) {
        int mode = 0, scale = 100;
        std::sscanf(test, "%d,%d", &mode, &scale);
        bluewake_pvp_set_rules(true, mode, scale);
        bluewake_pvp_set_target(1000, mode != BLUEWAKE_PVP_OFF);
        static const char* hit_every = std::getenv("BLUEWAKE_PVP_TEST_HIT");
        static Uint64 last_hit = SDL_GetTicks();
        if (hit_every != nullptr && SDL_GetTicks() - last_hit > static_cast<Uint64>(std::atof(hit_every) * 1000.0)) {
            last_hit = SDL_GetTicks();
            const BluewakeLocation where = bluewake_game_location();
            const float from[3] = {where.pos[0] + 120.f, where.pos[1], where.pos[2]};
            bluewake_pvp_incoming(1000, 2, 0x2u, from);
        }
    } else {
        bluewake_pvp_set_rules(false, BLUEWAKE_PVP_OFF, 100);
    }
    BluewakePvpHit hit;
    while (bluewake_pvp_take_hit(&hit)) {
        if (!connected)
            continue; // logged by the host
        picojson::object m;
        m["type"] = picojson::value("PVP_HIT");
        m["targetClientId"] = picojson::value(static_cast<int64_t>(hit.target));
        m["damage"] = picojson::value(static_cast<int64_t>(hit.damage));
        m["hitType"] = picojson::value(static_cast<int64_t>(hit.type));
        m["pos"] = picojson::value(picojson::array{picojson::value(double(hit.pos[0])), picojson::value(double(hit.pos[1])),
                                                   picojson::value(double(hit.pos[2]))});
        m["direction"] = picojson::value(picojson::array{picojson::value(double(hit.direction[0])), picojson::value(double(hit.direction[1]))});
        send(m);
        std::fprintf(stderr, "[anchor] PVP_HIT sent to player %d: power %d, type %08X\n", hit.target, hit.damage, hit.type);
    }
}

// Each frame: the puppets. The host reads Link's pose while online, and draws
// ----------------------------------------------------- puppet content ----
// What each puppet is made of, as far as the app knows: the player's own content (from the server) where there is
// some, and else -- on a PC whose own Link or equipment is a mod's, so the game's resident models are not stock -- the
// game's own models from the disc, so a stock player stays stock for the others' eyes.
struct PuppetContent {
    bool body = false, equipment = false;          // the player's own content provides it
    bool supplied_body = false, supplied_equipment = false, supplied_hands = false; // the disc's models are set for it
    bool voice_set = false;                        // the puppet has been told whose voice it has
    std::string hash;                              // the manifest the content here came from
};
std::map<int, PuppetContent> g_puppet_content;

// The voice banks of the players whose own bank is decoded (Ready::bank), for their puppets' voices. Read by the game thread
// (a puppet starts a voice) and written by the app's.
std::mutex g_bank_mutex;
std::map<int, std::shared_ptr<const voice_bank::Bank>> g_player_bank;
std::map<int, std::string> g_player_voice;

// The base level of Link's voice at the output (measured in the game: a voice sweep, tests in docs/fork/anchor-puppets.md) times
// the Sound Effects setting, the distance and the side from puppet.c.
bool play_puppet_voice(int player, int voice, float left, float right, float pitch) {
    std::shared_ptr<const voice_bank::Bank> bank;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_bank_mutex);
        const auto it = g_player_bank.find(player);
        if (it == g_player_bank.end() || !it->second)
            return false;
        bank = it->second;
        name = g_player_voice[player];
    }
    // Decoded on its first use (the bank keeps the compressed waves): a voice this bank has no wave for, or that the memory
    // caps refuse, is the local bank's.
    const std::vector<int16_t>* decoded = bank->Voice(voice);
    if (decoded == nullptr)
        return false;
    std::vector<int16_t> tone;
    if (std::getenv("BLUEWAKE_PUPPET_VOICE_TONE") != nullptr) { // testing: a 1 kHz tone of the voice's length in place of the voice
        tone.resize(decoded->size());
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = static_cast<int16_t>(10000.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(i) / 32000.0));
    }
    const std::vector<int16_t>& pcm = tone.empty() ? *decoded : tone;
    const float sfx = static_cast<float>(bluewake_audio_volume_sfx_percent()) / 100.0f;
    // BLUEWAKE_PUPPET_VOICE_GAIN=<x> (testing): the base level times x, to find the voices in a capture.
    static const float test_gain = std::getenv("BLUEWAKE_PUPPET_VOICE_GAIN") != nullptr ? static_cast<float>(std::atof(std::getenv("BLUEWAKE_PUPPET_VOICE_GAIN"))) : 1.0f;
    const float base = 0.12f * sfx * test_gain;
    dol_aurora_audio_mix_voice(pcm.data(), static_cast<uint32_t>(pcm.size()), left * base, right * base, pitch);
    std::fprintf(stderr, "[voice] player %d voice id %d from its own bank %.12s (%zu samples), gain L %.3f R %.3f, pitch %.2f\n", player, voice,
                 name.c_str(), pcm.size(), static_cast<double>(left * base), static_cast<double>(right * base), static_cast<double>(pitch));
    return true;
}

int equipment_slot(const std::string& name) {
    static const char* const kNames[] = {"swgripa.bdl", "swgripms.bdl", "sha.bdl", "shms.bdl", "swa.bdl", "swms.bdl", "hands.bdl", "katsura.bdl"};
    for (int i = 0; i < 8; ++i)
        if (name == kNames[i])
            return i;
    return -1;
}

void puppet_gone(int id) {
    carry::player_gone(id);
    g_puppet_content.erase(id);
    link_colors::SetPlayerModel(id, nullptr);
    link_colors::ForgetPlayer(id);
    {
        std::lock_guard<std::mutex> lock(g_bank_mutex);
        g_player_bank.erase(id);
        g_player_voice.erase(id);
    }
    bluewake_puppet_remote_remove(id);
}

// A puppet with the game's own voice. Where this PC's voice is the game's, the local bank is that voice. Where it is a mod's, the
// local bank would make a stock player sound like the mod, so the disc's own bank (decoded as a peer's is) plays instead. False
// when that bank is wanted but the disc is not open yet: asked again.
bool set_stock_voice(int id) {
    if (content::LocalVoiceId().empty()) {
        {
            std::lock_guard<std::mutex> lock(g_bank_mutex);
            g_player_bank.erase(id);
            g_player_voice.erase(id);
        }
        bluewake_puppet_remote_voice(id, "", true);
        return true;
    }
    std::string vid;
    bool pending = false;
    const std::shared_ptr<const voice_bank::Bank> bank = content::StockVoiceBank(&vid, &pending);
    if (pending)
        return false;
    {
        std::lock_guard<std::mutex> lock(g_bank_mutex);
        if (bank) {
            g_player_bank[id] = bank;
            g_player_voice[id] = vid;
        } else {
            g_player_bank.erase(id);
            g_player_voice.erase(id);
        }
    }
    bluewake_puppet_remote_voice(id, bank ? vid.c_str() : "", false);
    std::fprintf(stderr, "[anchor] player %d is stock and this PC's voice is a mod's: their puppet plays %s\n", id,
                 bank ? "the game's own voice bank from the disc" : "the local bank (the disc's could not be decoded)");
    return true;
}

// The game's own models for a puppet that has none of its own, when this PC's resident ones are a mod's.
void sync_stock_models(int id) {
    PuppetContent& pc = g_puppet_content[id];
    if (!pc.voice_set) // stock players: the game's voice
        pc.voice_set = set_stock_voice(id);
    const mods::LocalParts local = mods::LocalState();
    const bool want_body = !pc.body && local.link;
    const bool want_equipment = !pc.equipment && local.equipment;
    const bool want_hands = !pc.body && local.link;
    if (want_body == pc.supplied_body && want_equipment == pc.supplied_equipment && want_hands == pc.supplied_hands)
        return;
    std::vector<unsigned char> body;
    std::map<std::string, std::vector<unsigned char>> equipment;
    if ((want_body || want_equipment || want_hands) && !mods::VanillaPuppetParts(body, equipment))
        return; // the disc is not open yet: tried again
    if (want_body != pc.supplied_body) {
        if (want_body)
            bluewake_puppet_remote_model(id, body.data(), body.size());
        else
            bluewake_puppet_remote_model(id, nullptr, 0);
    }
    for (const char* name : {"swgripa.bdl", "swgripms.bdl", "sha.bdl", "shms.bdl", "swa.bdl", "swms.bdl", "hands.bdl", "katsura.bdl"}) {
        const int slot = equipment_slot(name);
        const bool hands = slot >= 6; // the hands and the hair go with the body
        const bool want = hands ? want_hands : want_equipment;
        const bool had = hands ? pc.supplied_hands : pc.supplied_equipment;
        if (want == had)
            continue;
        const auto it = equipment.find(name);
        if (want && it != equipment.end())
            bluewake_puppet_remote_equipment(id, slot, it->second.data(), it->second.size());
        else
            bluewake_puppet_remote_equipment(id, slot, nullptr, 0);
    }
    if (want_body || want_equipment || want_hands)
        std::fprintf(stderr, "[anchor] player %d is stock on this PC's modded Link: the game's own %s%s%s are used for the puppet\n", id,
                     want_body ? "body " : "", want_equipment ? "equipment " : "", want_hands ? "hands" : "");
    pc.supplied_body = want_body;
    pc.supplied_equipment = want_equipment;
    pc.supplied_hands = want_hands;
}

// A peer's content arrived: set it on the puppet (it is made again with it).
void apply_content(const content::Ready& r) {
    PuppetContent& pc = g_puppet_content[r.id];
    pc.body = r.has_body;
    pc.hash = r.hash;
    pc.equipment = r.has_equipment;
    link_colors::SetPlayerModel(r.id, r.colors); // their model's colors (null: none)
    pc.supplied_body = pc.supplied_equipment = pc.supplied_hands = false; // replaced below or by sync_stock_models
    pc.voice_set = true; // (set below: the pack's voice, or the game's)
    if (r.has_body)
        bluewake_puppet_remote_model(r.id, r.body.data(), r.body.size());
    else
        bluewake_puppet_remote_model(r.id, nullptr, 0);
    for (int slot = 0; slot < 8; ++slot)
        bluewake_puppet_remote_equipment(r.id, slot, nullptr, 0);
    for (const auto& [name, bytes] : r.equipment) {
        const int slot = equipment_slot(name);
        if (slot >= 0 && slot < 6 && r.has_equipment)
            bluewake_puppet_remote_equipment(r.id, slot, bytes.data(), bytes.size());
    }
    // The body's own hands and hair (a custom body's skeleton is Link's, and its hands are made for it).
    if (r.has_body && !r.hands.empty())
        bluewake_puppet_remote_equipment(r.id, 6, r.hands.data(), r.hands.size());
    if (r.has_body && !r.hair.empty())
        bluewake_puppet_remote_equipment(r.id, 7, r.hair.data(), r.hair.size());
    if (r.voice.empty()) {
        pc.voice_set = set_stock_voice(r.id); // (a pack without a voice: the game's)
    } else {
        bluewake_puppet_remote_voice(r.id, r.voice.c_str(), r.voice == content::LocalVoiceId());
        std::lock_guard<std::mutex> lock(g_bank_mutex);
        g_player_bank[r.id] = r.bank;
        g_player_voice[r.id] = r.voice;
    }
    std::fprintf(stderr, "[anchor] player %d's puppet: body %s, equipment %s (%zu models), voice %s\n", r.id, r.has_body ? "custom" : "stock",
                 r.has_equipment ? "custom" : "stock", r.equipment.size(), r.voice.empty() ? "stock" : r.voice.c_str());
    sync_stock_models(r.id);
}

// Their content went (or sharing was turned off): the puppet is stock again.
void clear_content(int id) {
    PuppetContent& pc = g_puppet_content[id];
    pc = PuppetContent{};
    link_colors::SetPlayerModel(id, nullptr);
    bluewake_puppet_remote_model(id, nullptr, 0);
    for (int slot = 0; slot < 8; ++slot)
        bluewake_puppet_remote_equipment(id, slot, nullptr, 0);
    pc.voice_set = set_stock_voice(id);
    std::fprintf(stderr, "[anchor] player %d's puppet is stock again\n", id);
    sync_stock_models(id);
}

// the puppets of the players in this scene (when the setting is on).
void update_puppets(bool connected) {
    {   // the tunic colour changed in the Cosmetics Editor: tell the room
        static uint32_t sent = 0;
        const CVarColor c = my_color();
        const uint32_t now = (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b;
        if (connected && now != sent) {
            sent = now;
            send_state();
        }
    }
    static const char* self = std::getenv("BLUEWAKE_PUPPET_SELF"); // testing: a copy of Link, this many units aside
    static bool self_set;
    if (self != nullptr && !self_set) {
        bluewake_puppet_self_test(static_cast<float>(std::atof(self)));
        // Testing: that Link.arc (a custom player model) is the copy's body.
        if (const char* path = std::getenv("BLUEWAKE_PUPPET_SELF_MODEL")) {
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> arc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            rarc::Yaz0Expand(arc);
            std::vector<uint8_t> body;
            if (rarc::ValidateLinkArc(arc, &body)) {
                bluewake_puppet_remote_model(1000, body.data(), body.size());
                std::fprintf(stderr, "[anchor] test: the copy's model is %s (%zu bytes)\n", path, body.size());
            } else
                std::fprintf(stderr, "[anchor] test: %s is not a usable Link.arc\n", path);
        }
        self_set = true;
    }
    {
        // Testing: the copy of Link wears these colors ("Hero.Tunic=ff0000").
        static const char* self_colors = std::getenv("BLUEWAKE_PUPPET_SELF_COLORS");
        if (self_colors != nullptr)
            link_colors::SetPlayerColors(1000, self_colors);
        // My colors changed in the Cosmetics Editor: tell the room.
        static std::string sent_colors;
        const std::string mine = share_content() ? link_colors::LocalSpec() : std::string();
        if (connected && mine != sent_colors)
            send_state();
        sent_colors = mine;
    }
    const bool puppets = true; // always Link models (the setting was removed)
    bluewake_puppet_enable(connected || self_set, puppets || self_set);
    bluewake_puppet_set_sounds(CVarGetInteger("gNetwork.Anchor.PuppetSounds", 1) != 0);
    bluewake_puppet_set_voice_player(play_puppet_voice);
    pvp_frame(connected, self_set);
    // Custom player content: published, advertised, and fetched in the background.
    content::Tick(http_base(), share_content() && puppets, connected);
    {
        // The pitch of Link's voice is part of the state others hear us by.
        static int sent_pitch = 0;
        const int pitch = share_content() ? local_voice_pitch() : 0;
        if (connected && pitch != sent_pitch)
            send_state();
        sent_pitch = pitch;
    }
    if (content::LocalChanged() && connected)
        send_state();
    for (const int id : content::TakeCleared())
        clear_content(id);
    for (const content::Ready& r : content::TakeReady())
        apply_content(r);
    if (!connected)
        return;
    for (const auto& [id, p] : g_players)
        if (!p.self && p.online) {
            content::Want(id, p.content);
            sync_stock_models(id);
        }
    const BluewakeLocation where = bluewake_game_location();
    const Uint64 now = SDL_GetTicks();
    for (const auto& [id, p] : g_players) {
        bluewake_puppet_remote_show(id, puppets && in_my_scene(p, where, now));
        if (!p.self) // an arrow on my minimap while they are in my stage and room (seen in runs, 11d)
            bluewake_puppet_remote_map(id, in_my_scene(p, where, now),
                                       uint8_t(std::lround(p.color.x * 255.f)),
                                       uint8_t(std::lround(p.color.y * 255.f)), uint8_t(std::lround(p.color.z * 255.f)));
        if (!p.self) // an arrow on my Sea Chart wherever they are (pause menu), in their color
            bluewake_puppet_remote_chart(id, p.online && p.sea_known && now - p.sea_time < 5000 &&
                                             CVarGetInteger("gNetwork.Anchor.SeaChartPlayers", 1) != 0,
                                         p.sea[0], p.sea[1], p.angle_y, uint8_t(std::lround(p.color.x * 255.f)),
                                         uint8_t(std::lround(p.color.y * 255.f)), uint8_t(std::lround(p.color.z * 255.f)));
        if (!p.self)
            link_colors::SetPlayerColors(id, p.link_colors); // this player's puppet wears their colors (variant = their id)
        if (!p.self) {
            // Their expression (face texture numbers) is for their own model: the puppet shows it when it wears that model (or
            // both are the game's Link).
            const PuppetContent& pc = g_puppet_content[id];
            const bool face_ok = p.link_model == 2 ? false : p.link_model == 1 ? (pc.body && pc.hash == p.content) : true;
            bluewake_puppet_remote_face_ok(id, face_ok);
        }
        if (!p.self)
            bluewake_puppet_remote_voice_pitch(id, p.voice_pitch != 0 ? 1.0f + float(p.voice_pitch) / 100.0f : 0.0f);
    }
}

const char* status_text() {
    switch (anchor::GetStatus()) {
    case anchor::Status::Connected:
        return "Connected";
    case anchor::Status::Connecting:
        return "Connecting...";
    case anchor::Status::Failed:
        return "Not connected";
    default:
        return "Disconnected";
    }
}

namespace {
// The developers' stage select's places (the same table as Dev Tools > Warp), to name where a player is.
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
} // namespace

// "Outset Island", from the stage code and room: the place of that room, else the first place on that stage, else the code.
std::string place_name(const std::string& stage, int room) {
    // The sea: its rooms are the Sea Chart's 49 squares, row by row from the top left (room 1 A1 ... room 44 B7), named
    // as the chart names them, "Outset Island (B7)". The stage-select names numbered them in hex ("Sector 2C").
    if (stage == "sea")
        return sea_chart::Name(room);
    if (stage == "Name")
        return "File Select";
    if (stage == "sea_T")
        return "Title Screen";
    // Elsewhere the developers' room names, under the place they belong to (their stage-select groups): a dungeon by
    // its name alone (its rooms are named by floor), a town's or island's rooms after it ("Windfall Island - Bomb shop").
    struct Group { int id; const char* place; bool rooms; };
    static const Group kGroups[] = {
        {1, "Forsaken Fortress", false}, {2, "Forsaken Fortress", false}, {3, "Forsaken Fortress", false},
        {4, "Dragon Roost Cavern", false}, {5, "Forbidden Woods", false}, {6, "Tower of the Gods", false},
        {7, "Earth Temple", false}, {8, "Wind Temple", false}, {9, "Ganon's Tower", false}, {10, "Ganon's Tower", false},
        {11, "Ganon's Tower", false}, {14, "Savage Labyrinth", false}, {22, "Outset Island", true},
        {23, "Windfall Island", true}, {24, "Dragon Roost Island", true}, {25, "Forest Haven", true},
        {26, "Private Oasis", true}, {27, "Pirate Ship", true}, {28, "Pirate Ship", true}, {30, "Hyrule Castle", true},
    };
    {
        const StagePlace* found = nullptr;
        for (const StagePlace& p : kStagePlaces)
            if (stage == p.stage && (found == nullptr || p.room == room)) {
                found = &p;
                if (p.room == room)
                    break;
            }
        if (found != nullptr)
            for (const Group& g : kGroups)
                if (g.id == found->group)
                    return g.rooms ? std::string(g.place) + " - " + found->name : std::string(g.place);
    }
    const StagePlace* any = nullptr;
    for (const StagePlace& p : kStagePlaces) {
        if (stage != p.stage)
            continue;
        if (p.room == room)
            return p.name;
        if (any == nullptr)
            any = &p;
    }
    if (any != nullptr)
        return any->name;
    return stage;
}

// The room's player list as Ship of Harkinian's Anchor Room window draws it (AnchorRoomWindow::DrawElement): teams under
// a heading when there is more than one, the room's owner marked, each name (offline ones dimmed), where they are after
// it, and a button beside it to teleport to them.
void draw_players(bool) {
    if (g_players.empty()) {
        ImGui::TextDisabled(anchor::GetStatus() == anchor::Status::Connected ? "Nobody else is here yet." : "Not connected.");
        return;
    }
    if (cvar_string("gNetwork.Anchor.Room", "ww-global") == "ww-global") { // the global room shows only a count
        int online = 0;
        for (const auto& [id, p] : g_players)
            online += p.online;
        ImGui::Text("Players Online: %d", online);
        return;
    }
    std::set<std::string> teams;
    for (const auto& [id, p] : g_players)
        teams.insert(p.team);
    for (const std::string& team : teams) {
        if (teams.size() > 1)
            ImGui::SeparatorText(team.c_str());
        for (const auto& [id, p] : g_players) {
            if (p.team != team)
                continue;
            ImGui::PushID(id);
            if (id == g_owner) {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "[H]");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Owner of the room");
                ImGui::SameLine();
            }
            if (p.self) {
                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "%s", cvar_string("gNetwork.Anchor.Name", "Link").c_str());
            } else if (!p.online) {
                ImGui::TextColored(ImVec4(1, 1, 1, 0.3f), "%s - offline", p.name.c_str());
                ImGui::PopID();
                continue;
            } else {
                ImGui::Text("%s", p.name.c_str());
            }
            if (!p.stage.empty() && (p.self || p.save_loaded)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 1, 1, 0.5f), "- %s", place_name(p.stage, p.room).c_str());
            }
            if (can_teleport_between(p)) {
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                const float side = 20.0f * ImGui::GetIO().FontGlobalScale;
                if (ImGui::Button("##tp", ImVec2(side, side)))
                    request_teleport(id);
                // The location arrow, drawn on the button.
                const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                const float cx = (lo.x + hi.x) * 0.5f, cy = (lo.y + hi.y) * 0.5f, r = side * 0.3f;
                ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx + r, cy - r), ImVec2(cx - r * 0.9f, cy + r * 0.1f),
                                                              ImVec2(cx - r * 0.1f, cy + r * 0.9f),
                                                              ImGui::GetColorU32(ImGuiCol_Text));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Teleport to %s", p.name.c_str());
                ImGui::PopStyleVar();
            }
            const ImVec4 red(1, 0, 0, 1);
            if (!p.self && p.online && p.client_version != kAnchorProtocol) {
                ImGui::SameLine();
                ImGui::TextColored(red, "(!)");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    // A build from before 2026-10-17 sends no version: its messages are the same less two fields (the
                    // version and the seed), so it plays together; a named other version may not.
                    if (p.client_version.empty()) {
                        ImGui::Text("Older build: it sends no version.");
                        ImGui::Text("It should still play together; use the same release to be safe.");
                    } else {
                        ImGui::Text("Different version: may not work together.");
                        ImGui::Text("Theirs: %s", p.client_version.c_str());
                    }
                    ImGui::Text("Yours: %s", kAnchorProtocol);
                    ImGui::EndTooltip();
                }
            }
            const std::string my_seed = randoflow::Active() ? randoflow::Seed() : std::string();
            if (!p.self && p.online && p.save_loaded && bluewake_game_location().stage[0] != '\0' &&
                !p.client_version.empty() && p.seed != my_seed) {  // an older build sends no seed: nothing to compare
                ImGui::SameLine();
                ImGui::TextColored(red, "(!)");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Seed mismatch! Continuing will break things!");
                    ImGui::Text("Yours: %s", my_seed.empty() ? "vanilla" : my_seed.c_str());
                    ImGui::Text("Theirs: %s", p.seed.empty() ? "vanilla" : p.seed.c_str());
                    ImGui::EndTooltip();
                }
            }
            ImGui::PopID();
        }
    }
}

void chat_send(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return c < 32; }), text.end());
    if (text.size() > kChatMax)
        text.resize(kChatMax);
    if (text.empty() || anchor::GetStatus() != anchor::Status::Connected || SDL_GetTicks() - g_chat_last_sent < 700)
        return;
    g_chat_last_sent = SDL_GetTicks();
    picojson::object m;
    m["type"] = picojson::value("CHAT");
    m["text"] = picojson::value(text);
    send(m);
    chat_add(cvar_string("gNetwork.Anchor.Name", "Link"), ImVec4(my_color().r / 255.f, my_color().g / 255.f, my_color().b / 255.f, 1.f), text);
}

// The chat's body: the history, then the input line.
// fill: the Chat window's log takes the height that is left. In the menu (a scrolling column) it has a fixed height:
// "the rest" there grew with the box every frame, so the Network tab grew without end (user 2026-10-19).
void embed_chat(float scale, bool fill) {
    if (anchor::GetStatus() != anchor::Status::Connected) {
        ImGui::TextDisabled("Not connected to Anchor. Turn Anchor on in Network > Anchor to chat.");
        return;
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() + 4 * scale;
    if (ImGui::BeginChild("##chatlog", ImVec2(0, fill ? -footer : 160 * scale), ImGuiChildFlags_Borders)) {
        ImGui::PushTextWrapPos(0.f);
        for (const ChatLine& l : g_chat) {
            ImGui::TextColored(l.color, "%s", l.name.c_str());
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(": ");
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(l.text.c_str());
        }
        ImGui::PopTextWrapPos();
        if (g_chat_scroll) {
            ImGui::SetScrollHereY(1.0f);
            g_chat_scroll = false;
        }
    }
    ImGui::EndChild();
    static char input[kChatMax + 1];
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##chatinput", "Say something (Enter sends)", input, sizeof input, ImGuiInputTextFlags_EnterReturnsTrue)) {
        chat_send(input);
        input[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
}

// Network > Anchor > Room Settings: how players in this room meet. The room's owner changes them (the server refuses anyone
// else); everyone sees them.
void draw_room_settings() {
    if (anchor::GetStatus() != anchor::Status::Connected) {
        ImGui::TextDisabled("Connect to a room to see its settings.");
        return;
    }
    const bool owner = g_self != 0 && g_self == g_owner;
    bool changed = false;
    ImGui::BeginDisabled(!owner);
    static const char* const kModes[] = {"Off", "No Damage", "On"};
    int mode = g_room_pvp;
    if (ImGui::Combo("PvP", &mode, kModes, 3)) {
        g_room_pvp = mode;
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Off: players cannot hit each other. No Damage: hits land (knockback, the hurt animation and "
                          "sound) but nobody loses a heart. On: hits hurt, as an enemy's would.");
    if (g_room_pvp == BLUEWAKE_PVP_ON) {
        static const char* const kScales[] = {"Half", "Normal", "Double"};
        int scale = g_room_pvp_scale <= 75 ? 0 : g_room_pvp_scale >= 150 ? 2 : 1;
        if (ImGui::Combo("PvP Damage", &scale, kScales, 3)) {
            g_room_pvp_scale = scale == 0 ? 50 : scale == 2 ? 200 : 100;
            changed = true;
        }
    }
    if (g_room_pvp != BLUEWAKE_PVP_OFF) {
        if (ImGui::Checkbox("Friendly Fire Within a Team", &g_room_friendly_fire))
            changed = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Players on the same team can hit each other.");
    }
    if (ImGui::Checkbox("Player Collision", &g_room_collision))
        changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Players are solid: they push each other apart instead of walking through, and Z can target one.");
    if (ImGui::Checkbox("Allow Carrying Players", &g_room_carry))
        changed = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Players can lift each other like pots (A beside a player) and throw or put them down. The lifted "
                          "player is held at the carrier's hands with no control, and can break free with B. Nobody is hurt "
                          "unless PvP is On.");
    ImGui::EndDisabled();
    if (!owner) {
        const auto it = g_players.find(g_owner);
        ImGui::TextDisabled("Only the room's owner%s can change these.",
                            it != g_players.end() ? (" (" + it->second.name + ")").c_str() : "");
    }
    if (changed && owner)
        send_room_state();
}

void draw_chat_window(float scale, float fontScale) {
    shipmenu::PopoutWindow("Chat", "gOpenWindows.Chat", ImVec2(420 * scale, 280 * scale), 0.92f, fontScale, [&] { embed_chat(scale, true); });
}

// --- The chat hotkey overlay -------------------------------------------------------------------------------------
// While connected, the chat key (T; gNetwork.Anchor.ChatKey, an ImGuiKey, set in Network > Anchor) opens a small input
// line over the game with the last few chat lines above it, focused for typing. The game's pad and keyboard are blocked
// while it is open (win_settings.cpp does that from ChatOverlayOpen). Enter sends and stays open for the next line, Enter
// on an empty line closes it, and Esc closes it (the host's Esc hotkey is routed here instead of opening the menu).
bool g_overlay_open = false;
int g_overlay_frames = 0;  // frames since it opened
char g_overlay_text[kChatMax + 1];

void overlay_close() {
    g_overlay_open = false;
    g_overlay_text[0] = 0;
}

int chat_key() { return CVarGetInteger("gNetwork.Anchor.ChatKey", ImGuiKey_T); }

int overlay_char_filter(ImGuiInputTextCallbackData* data) {
    // The key that opened the box also queues its letter; it must not be typed into the box.
    if (g_overlay_frames <= 3) {
        const char* name = ImGui::GetKeyName(static_cast<ImGuiKey>(chat_key()));
        if (name != nullptr && name[0] != 0 && name[1] == 0 &&
            std::tolower(static_cast<unsigned char>(name[0])) == std::tolower(static_cast<int>(data->EventChar)))
            return 1;
    }
    return 0;
}

void draw_overlay(float scale, float fontScale) {
    const bool connected = anchor::GetStatus() == anchor::Status::Connected;
    ImGuiIO& io = ImGui::GetIO();
    if (!g_overlay_open) {
        const int key = chat_key();
        if (connected && key != ImGuiKey_None && !bw_settings_menu_is_open() && !io.WantTextInput &&
            ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) {
            g_overlay_open = true;
            g_overlay_frames = 0;
            g_overlay_text[0] = 0;
            g_chat_scroll = true;
            std::fprintf(stderr, "[chat] overlay opened (game input blocked)\n");
        } else {
            return;
        }
    }
    if (!connected || bw_settings_menu_is_open()) {
        overlay_close();
        return;
    }
    const ImVec2 display = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(16 * scale, display.y - 24 * scale), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(std::min(460 * scale, display.x - 32 * scale), 0));
    ImGui::SetNextWindowBgAlpha(0.8f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize;
    shipmenu::PushThemeStyle();
    if (ImGui::Begin("##chatoverlay", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PushTextWrapPos(0.f);
        constexpr size_t kShown = 6;
        const size_t first = g_chat.size() > kShown ? g_chat.size() - kShown : 0;
        if (g_chat.empty())
            ImGui::TextDisabled("No messages yet.");
        for (size_t i = first; i < g_chat.size(); ++i) {
            ImGui::TextColored(g_chat[i].color, "%s", g_chat[i].name.c_str());
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(": ");
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(g_chat[i].text.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1);
        if (g_overlay_frames == 0 || !ImGui::IsAnyItemActive())
            ImGui::SetKeyboardFocusHere();  // focused on opening, and a click elsewhere in the box must not lose the line
        if (ImGui::InputTextWithHint("##chatoverlayinput", "Say something (Enter sends, Esc closes)", g_overlay_text,
                                     sizeof g_overlay_text,
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
                                     overlay_char_filter)) {
            if (g_overlay_text[0] == 0) {
                overlay_close();
            } else {
                chat_send(g_overlay_text);
                g_overlay_text[0] = 0;
                g_chat_scroll = true;
                ImGui::SetKeyboardFocusHere(-1);
            }
        }
    }
    ImGui::End();
    shipmenu::PopThemeStyle();
    ++g_overlay_frames;
}

// BLUEWAKE_TEST_CHAT_OVERLAY=<text> (testing): a few seconds after the connection, presses the chat key through ImGui's
// input queue, types the text, and presses Enter, so the overlay is driven the way a player drives it. With
// BLUEWAKE_TEST_CHAT_OVERLAY_CLOSE set it then presses Enter on the empty line, which closes the box.
void test_overlay() {
    static const char* text = std::getenv("BLUEWAKE_TEST_CHAT_OVERLAY");
    if (text == nullptr || anchor::GetStatus() != anchor::Status::Connected)
        return;
    static Uint64 since = SDL_GetTicks();
    static int step;
    const Uint64 t = SDL_GetTicks() - since;
    ImGuiIO& io = ImGui::GetIO();
    if (step == 0 && t > 6000) {
        io.AddKeyEvent(static_cast<ImGuiKey>(chat_key()), true);
        io.AddInputCharactersUTF8("t");
        step = 1;
    } else if (step == 1 && t > 6100) {
        io.AddKeyEvent(static_cast<ImGuiKey>(chat_key()), false);
        step = 2;
    } else if (step == 2 && t > 8000) {
        io.AddInputCharactersUTF8(text);
        step = 3;
    } else if (step == 3 && t > 11000) {
        io.AddKeyEvent(ImGuiKey_Enter, true);
        step = 4;
    } else if (step == 4 && t > 11100) {
        io.AddKeyEvent(ImGuiKey_Enter, false);
        step = 5;
    } else if (step == 5 && t > 15000 && std::getenv("BLUEWAKE_TEST_CHAT_OVERLAY_CLOSE") != nullptr) {
        io.AddKeyEvent(ImGuiKey_Enter, true);
        step = 6;
    } else if (step == 6 && t > 15100) {
        io.AddKeyEvent(ImGuiKey_Enter, false);
        step = 7;
    }
}

// Open (gOpenWindows.AnchorRoom): the list floats over the game as Ship of Harkinian's does: no title bar, sized to its
// content, on the notifications' translucent background, and only while connected.
void draw_room_window(float scale, float fontScale) {
    if (anchor::GetStatus() != anchor::Status::Connected)
        return;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, CVarGetFloat("gSettings.Notifications.BgOpacity", 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    // Below the hearts and the magic bar (the game's HUD has the top-left corner; the list over it let the hearts show
    // through its text), a fifth of the way down.
    ImGui::SetNextWindowPos(ImVec2(16.0f * scale, ImGui::GetIO().DisplaySize.y * 0.2f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Anchor Room", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::SetWindowFontScale(fontScale);
        draw_players(false);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

bool connected_or_connecting() {
    const auto s = anchor::GetStatus();
    return s == anchor::Status::Connected || s == anchor::Status::Connecting;
}

} // namespace

void BuildMenu(shipmenu::Menu& m) {
    const char* H = "Network";
    m.AddSidebar(H, "Anchor", 2);
    m.AddWidget(H, "Anchor", 0, "Player", WidgetType::SeparatorText);
    m.AddWidget(H, "Anchor", 0, "Name", WidgetType::CVarInputText)
        .CVar("gNetwork.Anchor.Name")
        .DefaultText("Link")
        .Range(0, 24)
        .Tooltip("The name other players see.")
        .Callback([](WidgetInfo&) {
            if (anchor::GetStatus() == anchor::Status::Connected)
                send_state();
        });
    m.AddWidget(H, "Anchor", 0, "Room", WidgetType::SeparatorText);
    m.AddWidget(H, "Anchor", 0, "Room Name", WidgetType::CVarInputText)
        .CVar("gNetwork.Anchor.Room")
        .DefaultText("ww-global")
        .Range(0, 64)
        .Tooltip("Players who enter the same room name play together. The name is the room's password; "
                 "ww-global is open to everyone.")
        .PreFunc([](WidgetInfo& w) {
            if (connected_or_connecting())
                w.disabledReasons.push_back("Disconnect to change the room");
        });
    m.AddWidget(H, "Anchor", 0, "Team", WidgetType::CVarInputText)
        .CVar("gNetwork.Anchor.Team")
        .DefaultText("default")
        .Range(0, 24)
        .Tooltip("Players on the same team share items and progress. Other teams in the room are only seen.")
        .PreFunc([](WidgetInfo& w) {
            if (connected_or_connecting())
                w.disabledReasons.push_back("Disconnect to change the team");
        });
    m.AddWidget(H, "Anchor", 0, "Connect", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        if (connected_or_connecting()) {
            if (ImGui::Button("Disconnect", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                disconnect();
        } else if (ImGui::Button("Connect", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            connect();
        }
        ImGui::Text("Status: %s", status_text());
        const std::string error = anchor::LastError();
        if (anchor::GetStatus() == anchor::Status::Failed && !error.empty())
            ImGui::TextDisabled("%s", error.c_str());
        if (!g_notice.empty())
            ImGui::TextDisabled("%s", g_notice.c_str());
    });
    m.AddWidget(H, "Anchor", 0, "Server", WidgetType::SeparatorText);
    m.AddWidget(H, "Anchor", 0, "Server Address", WidgetType::CVarInputText)
        .CVar("gNetwork.Anchor.Host")
        .DefaultText("wwserver.viosarcade.xyz")
        .Range(0, 128)
        .Tooltip("The online play server. host:port connects without encryption (a server on this PC, for "
                 "testing).")
        .PreFunc([](WidgetInfo& w) {
            if (connected_or_connecting())
                w.disabledReasons.push_back("Disconnect to change the server");
        });
    m.AddWidget(H, "Anchor", 1, "Players", WidgetType::SeparatorText);
    // Other players are always drawn as Link (the marker-only option is gone); this shows or hides the
    // names above them.
    m.AddWidget(H, "Anchor", 1, "Show Nametags", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Anchor.ShowPlayers")
        .DefaultValue(true)
        .Tooltip("The name of each player on the same stage as you, above their head.");
    m.AddWidget(H, "Anchor", 1, "Players on the Sea Chart", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Anchor.SeaChartPlayers")
        .DefaultValue(true)
        .Tooltip("The other players in your room on the Sea Chart, as Link's chart arrow in their colour: at sea where they "
                 "are, on an island or in a dungeon at that island's square.");
    m.AddWidget(H, "Anchor", 1, "Puppet Voice and Sounds", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Anchor.PuppetSounds")
        .DefaultValue(true)
        .Tooltip("Other players' Link makes his voice and sound effects where they are: a cry when he rolls, jumps, cuts or is "
                 "hurt. It uses your own game's voice, whatever voice pack the other player has.");
    m.AddWidget(H, "Anchor", 1, "Share Custom Player Content", WidgetType::CVarCheckbox)
        .CVar("gNetwork.Anchor.ShareContent")
        .DefaultValue(true)
        .Tooltip("Shares your custom player model, voice and equipment (the top Player Model in the Mod Menu) with the "
                 "other players in your room, and shows theirs on their characters: you can play as vanilla Link and "
                 "see a Samus and a Young Link. What others share is downloaded in the background and kept in the "
                 "cache folder beside the game (up to 500 MB). Off: yours is not sent, theirs is not downloaded or "
                 "used, and other players are drawn as the game's own Link.");
    m.AddWidget(H, "Anchor", 1, "Room Settings", WidgetType::SeparatorText);
    m.AddWidget(H, "Anchor", 1, "Player Interaction", WidgetType::Custom)
        .HideInSearch()
        .CustomFunction([](WidgetInfo&) { draw_room_settings(); });
    m.AddWidget(H, "Anchor", 1, "Sync from Team", WidgetType::Button)
        .Tooltip("Asks the team for its progress again and merges it into your save now (as Ship of Harkinian's): for when "
                 "items or flags a teammate got have not arrived. Needs a save in play.")
        .Callback([](WidgetInfo&) {
            if (!g_sync_on) {
                toast("", "Sync from Team needs a save in play, connected to a room that syncs.");
                return;
            }
            g_team_state_requested = false; // asked for again on the next frame
            std::fprintf(stderr, "[anchor] sync from team asked for\n");
        });
    m.AddWidget(H, "Anchor", 1, "Chat", WidgetType::Window)
        .CVar("gOpenWindows.Chat")
        .HideInSearch()
        .Tooltip("A text chat with the room. Popout puts it in a window of its own; while it is not open, what others say is a notification.")
        .CustomFunction([](WidgetInfo&) { embed_chat(shipmenu::MenuScale(), false); });
    m.AddWidget(H, "Anchor", 1, "Chat Hotkey", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        static bool waiting;
        const ImGuiKey cur = static_cast<ImGuiKey>(chat_key());
        const std::string name = cur == ImGuiKey_None ? "None" : ImGui::GetKeyName(cur);
        const std::string label = (waiting ? std::string("Press a key...") : name) + "###chatkey";
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Chat Hotkey");
        ImGui::SameLine();
        if (ImGui::Button(label.c_str(), ImVec2(140 * shipmenu::MenuScale(), 0)))
            waiting = !waiting;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("While connected, this key opens a chat line over the game. Click, then press the key to "
                              "use (Esc cancels, Backspace clears it).");
        if (!waiting)
            return;
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            const ImGuiKey key = static_cast<ImGuiKey>(k);
            if ((key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickRight) ||
                (key >= ImGuiKey_MouseLeft && key < ImGuiKey_NamedKey_END) || key == ImGuiKey_LeftCtrl ||
                key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
                key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper ||
                key == ImGuiKey_RightSuper || key == ImGuiKey_Enter)
                continue;
            if (!ImGui::IsKeyPressed(key, false))
                continue;
            waiting = false;
            if (key != ImGuiKey_Escape) {
                CVarSetInteger("gNetwork.Anchor.ChatKey", key == ImGuiKey_Backspace ? int(ImGuiKey_None) : int(key));
                CVarSaveNextFrame();
            }
            break;
        }
    });
    m.AddWidget(H, "Anchor", 1, "Current Room", WidgetType::SeparatorText);
    m.AddWidget(H, "Anchor", 1, "Anchor Room", WidgetType::Custom)
        .HideInSearch()
        .CustomFunction([](WidgetInfo&) {
            // As Ship of Harkinian's Anchor menu: a button shows or hides the room's list floating over the game, and
            // while it is hidden the list is drawn here.
            if (anchor::GetStatus() != anchor::Status::Connected) {
                ImGui::TextDisabled("Not connected.");
                return;
            }
            const bool shown = CVarGetInteger("gOpenWindows.AnchorRoom", 0) != 0;
            if (ImGui::Button(shown ? "Hide Anchor Room Window" : "Toggle Anchor Room Window")) {
                CVarSetInteger("gOpenWindows.AnchorRoom", shown ? 0 : 1);
                CVarSaveNextFrame();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A list of the room's players floating over the game: their team, where they are, "
                                  "and a button to teleport to them.");
            ImGui::Spacing();
            if (!shown)
                draw_players(true);
        });
}

void Launch() {
    if (CVarGetInteger("gNetwork.Anchor.Enabled", 0))
        connect();
}

// BLUEWAKE_TEST_ANCHOR_TELEPORT=<name>@<seconds> (testing): that many seconds
// after connecting, teleport to the player of that name, once (the Teleport
// button, which a scenario cannot press).
void test_teleport() {
    static bool done;
    static Uint64 since;
    const char* env = std::getenv("BLUEWAKE_TEST_ANCHOR_TELEPORT");
    if (done || env == nullptr)
        return;
    const std::string spec = env;
    const size_t at = spec.rfind('@');
    if (since == 0)
        since = SDL_GetTicks();
    if (SDL_GetTicks() - since < 1000ull * std::strtoull(spec.c_str() + (at == std::string::npos ? spec.size() : at + 1), nullptr, 10))
        return;
    const std::string name = spec.substr(0, at);
    for (const auto& [id, p] : g_players)
        if (p.name == name && can_teleport_between(p)) {
            std::fprintf(stderr, "[anchor] test: teleport to %s\n", name.c_str());
            request_teleport(id);
            done = true;
        }
}

// BLUEWAKE_TEST_ANCHOR_CYCLE=<seconds> (testing): that long after the first connection, disconnect, connect again 3 s
// later, and disconnect once more 3 s after that connection; each disconnect logs how long it held this thread.
void test_cycle(anchor::Status status) {
    static int step;
    static Uint64 at;
    const char* env = std::getenv("BLUEWAKE_TEST_ANCHOR_CYCLE");
    if (env == nullptr || step >= 4)
        return;
    const Uint64 now = SDL_GetTicks();
    const bool on = status == anchor::Status::Connected;
    if (on && at == 0)
        at = now + (step == 0 ? 1000ull * std::strtoull(env, nullptr, 10) : 3000ull);
    if (at == 0 || now < at || (step % 2 == 0 && !on))
        return;
    if (step % 2 == 0) {
        const auto t0 = std::chrono::steady_clock::now();
        disconnect();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[anchor] test: disconnect %d held the frame %.2f ms\n", step / 2 + 1, ms);
        at = now + 3000;
    } else {
        std::fprintf(stderr, "[anchor] test: connect again\n");
        connect();
        at = 0; // the next disconnect waits for this connection, then 3 s
    }
    ++step;
}

void Frame(float scale, float fontScale) {
    const anchor::Status status = anchor::GetStatus();
    test_cycle(status);
    if (status == anchor::Status::Connected && g_last_status != anchor::Status::Connected)
        handshake();
    if (status != anchor::Status::Connected && g_last_status == anchor::Status::Connected) {
        g_players.clear();
        bluewake_puppet_remote_clear();
        bluewake_pvp_clear();
        g_room_pvp = BLUEWAKE_PVP_OFF;
        g_room_pvp_scale = 100;
        g_room_friendly_fire = false;
        g_room_collision = true;
        g_room_carry = false;
        carry::reset();
    }
    g_last_status = status;
    if (status == anchor::Status::Connected) {
        const BluewakeLocation where = bluewake_game_location();
        const bool moved = std::string(where.stage) != g_sent_location.stage || where.room != g_sent_location.room;
        if (moved && SDL_GetTicks() - g_last_state_sent > 250)
            send_state();
        send_pose();
        send_chart();
        test_teleport();
    }
    // Testing: BLUEWAKE_TEST_CHAT=<text> says it once, a few seconds after the connection.
    if (static const char* test = std::getenv("BLUEWAKE_TEST_CHAT"); test != nullptr && status == anchor::Status::Connected) {
        static Uint64 since = SDL_GetTicks();
        static bool said;
        if (!said && SDL_GetTicks() - since > 6000) {
            said = true;
            chat_send(test);
        }
    }
    for (const std::string& m : anchor::Poll())
        handle(m);
    // Team sync while connected, in a room that syncs, with a save in play: the play scene with Link in it, not the title
    // (sea_T) or the file select (Name), whose stages are not a player's save. Syncing there merged the team's state into
    // the title's placeholder save (lost when the file loaded) and took the found-items baseline from it (no "found"
    // toasts after a reconnect, user 2026-10-17). A file loaded into play starts sync anew: the team state is asked for
    // then, so a player who reconnects or crashes and comes back gets the team's inventory.
    const BluewakeLocation sync_here = bluewake_game_location();
    const bool in_game = sync_here.scene_proc == BLUEWAKE_SCENE_PLAY && sync_here.has_player &&
                         std::strcmp(sync_here.stage, "sea_T") != 0;
    const bool want_sync = status == anchor::Status::Connected && g_room_sync != 0 && in_game;
    if (want_sync != g_sync_on) {
        g_sync_on = want_sync;
        bluewake_team_sync_enable(want_sync);
        bluewake_switch_sync_enable(want_sync);
        g_team_state_merged = g_team_state_requested = g_item_baseline = false;
    }
    if (g_sync_on) {
        if (!g_team_state_requested) {
            picojson::object m;
            m["type"] = picojson::value("REQUEST_TEAM_STATE");
            send(m);
            g_team_state_requested = true;
        }
        if (!g_item_baseline) {
            BluewakeSyncChange snapshot[BLUEWAKE_SYNC_SNAPSHOT_MAX];
            if (const unsigned k = bluewake_team_sync_snapshot(snapshot, BLUEWAKE_SYNC_SNAPSHOT_MAX); k != 0) {
                item_found::SetBaseline(snapshot, k);
                g_item_baseline = true;
            }
        }
        {
            // This game's switches, to the team (SET_SWITCH; the server relays it to the team).
            BluewakeSwitchEvent sw[32];
            const unsigned k = bluewake_switch_sync_take(sw, 32);
            for (unsigned i = 0; i < k; ++i) {
                picojson::object m;
                m["type"] = picojson::value("SET_SWITCH");
                m["stage"] = picojson::value(std::string(sw[i].stage));
                m["room"] = picojson::value(static_cast<int64_t>(sw[i].room));
                m["no"] = picojson::value(static_cast<int64_t>(sw[i].no));
                m["on"] = picojson::value(sw[i].on);
                m["seed"] = picojson::value(my_seed());
                m["targetTeamId"] = picojson::value(cvar_string("gNetwork.Anchor.Team", "default"));
                send(m);
            }
        }
        BluewakeSyncChange changes[256];
        const unsigned n = bluewake_team_sync_take(changes, 256);
        if (n != 0) {
            // What this player got, by name, for the teammates' toasts (the save diff, no hook in the game).
            std::fprintf(stderr, "[anchor] %u own changes, baseline %d\n", n, int(g_item_baseline));
            if (g_item_baseline)
                for (const int item : item_found::FromChanges(changes, n)) {
                    picojson::object f;
                    f["type"] = picojson::value("ITEM_FOUND");
                    f["itemId"] = picojson::value(static_cast<int64_t>(item));
                    f["name"] = picojson::value(std::string(item_found::Name(item)));
                    f["targetTeamId"] = picojson::value(cvar_string("gNetwork.Anchor.Team", "default"));
                    send(f);
                    std::fprintf(stderr, "[anchor] ITEM_FOUND sent: 0x%02X %s\n", item, item_found::Name(item));
                }
            picojson::object m;
            m["type"] = picojson::value("SYNC_SAVE");
            m["seed"] = picojson::value(my_seed());
            m["changes"] = picojson::value(changes_json(changes, n));
            m["targetTeamId"] = picojson::value(cvar_string("gNetwork.Anchor.Team", "default"));
            m["addToQueue"] = picojson::value(true);
            send(m);
        }
        // The server's copy of the team's state, for teammates who come later.
        if (g_team_state_merged && SDL_GetTicks() - g_last_team_state_sent > 30000)
            send_team_state(0);
    }
    update_puppets(status == anchor::Status::Connected);
    if (status == anchor::Status::Connected)
        draw_markers();
    notifications::Frame(scale, fontScale);
    if (CVarGetInteger("gOpenWindows.AnchorRoom", 0))
        draw_room_window(scale, fontScale);
    if (CVarGetInteger("gOpenWindows.Chat", 0))
        draw_chat_window(scale, fontScale);
    test_overlay();
    draw_overlay(scale, fontScale);
}

bool ChatOverlayOpen() { return g_overlay_open; }

void CloseChatOverlay() { overlay_close(); }

void Shutdown() {
    anchor::Stop();
    content::Shutdown();
}

} // namespace online
