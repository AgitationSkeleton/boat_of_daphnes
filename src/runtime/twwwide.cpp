// Widescreen, done in the game the way Dolphin users get it: the community 16:9 Gecko code, for any aspect (below).
// It widens the camera's aspect - the projection and the view culling both, so nothing at the sides of the wide
// picture is culled as off screen - and moves the HUD to the edges through the game's own 2D bounds and layout
// values. The picture it renders is anamorphic: Aurora stretches it over the window (src/runtime/frontend.cpp)
// instead of widening.
//
// The code's parts, as applied here:
//   writes into main.dol's code   -> interpreter patches (the patched functions run interpreted)
//   code written to free memory   -> written there once, and run by the interpreter (its caves)
//   writes to data                -> each frame, as a Gecko handler does (the HUD tuning structures
//                                    are set by constructors and menus), the 4:3 values kept to
//                                    restore when widescreen goes off
//
// Any aspect: the code is the 16:9 one with its constants made a function of the aspect. Boat of Daphnes' 16:10
// derivation of it (scripts/mods/widescreen_aspect.py) shows what that function is: each value the code writes is
// linear between the game's own value at 4:3 and the code's at 16:9 (the 16:10 data values are exactly that, and
// its code constants the nearest of the game's constants to it). With t = (aspect - 4/3) / (16/9 - 4/3), a value is
//   v(t) = original + (v16:9 - original) * t
// (t = 1 at 16:9: its code unchanged; t = 0 at 4:3: the game's own). The code's constants that name an existing
// float of the game's pool (114, 206, 79, 480, 4/3 ...) are loaded by instructions that now run native code (hook
// words) giving the value for the aspect; the written data (the pool slots the code overwrites, the HUD tuning
// halfwords) is recomputed every frame. Two values are not linear: the Picto Box view's width, 640 / (1 + t/3),
// and its offset (the 4:3 picture stays centred in the wide frame).
// Two settings: HUD at the Screen Edges (off: the HUD's values are those of t = 0, in the 4:3 middle) and Widescreen
// Actor Culling (off: the view's clipper takes the 4:3 aspect, so actors at the sides are culled as on a TV).
#include "runtime/interp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rt {

std::atomic<float> g_wideAspect{4.f / 3.f};
std::atomic<bool> g_wideCulling{true}, g_wideHud{true};

namespace {
#include "runtime/twwwide.inc"

constexpr u32 kCodeBegin = 0x80003100, kCodeEnd = 0x80338620;  // main.dol .init and .text

struct Write {
    u32 addr, value, size;  // size 2 or 4
};
struct Code {
    std::vector<std::pair<u32, u32>> patches;  // instruction words
    std::vector<std::pair<u32, std::vector<u8>>> caves;
    std::vector<Write> data;
};

Code parse(const char* text) {
    std::vector<u32> w;
    for (const char* p = text; *p;) {
        char* end;
        const u32 v = u32(std::strtoul(p, &end, 16));
        if (end == p) break;
        w.push_back(v);
        p = end;
        while (*p == ' ') p++;
    }
    Code code;
    for (size_t i = 0; i + 1 < w.size();) {
        const u32 type = w[i] >> 24, addr = 0x80000000u | (w[i] & 0x00FFFFFF), v = w[i + 1];
        i += 2;
        if (type == 0x04) {
            if (addr >= kCodeBegin && addr < kCodeEnd) code.patches.push_back({addr, v});
            else code.data.push_back({addr, v, 4});
        } else if (type == 0x02) {
            for (u32 n = 0; n <= v >> 16; n++) code.data.push_back({addr + 2 * n, v & 0xFFFF, 2});
        } else if (type == 0x06) {
            std::vector<u8> bytes;
            for (u32 n = 0; n < v; n++) bytes.push_back(u8(w[i + n / 4] >> (24 - 8 * (n % 4))));
            i += (v + 7) / 8 * 2;
            if (addr >= kCodeBegin && addr < kCodeEnd) code.caves.push_back({addr, std::move(bytes)});
            else  // (data: floats, four bytes each)
                for (u32 n = 0; n + 4 <= v; n += 4)
                    code.data.push_back({addr + n, u32(bytes[n]) << 24 | u32(bytes[n + 1]) << 16 | u32(bytes[n + 2]) << 8 | bytes[n + 3], 4});
        } else {
            log("[wide] unknown Gecko code type %02X\n", type);
            break;
        }
    }
    return code;
}

u32 readSized(u32 a, u32 size) { return size == 4 ? rd32(a) : rd16(a); }
void writeSized(u32 a, u32 v, u32 size) {
    if (size == 4) wr32(a, v);
    else wr16(a, v);
}
float asFloat(u32 b) { return __builtin_bit_cast(float, b); }
u32 asBits(float f) { return __builtin_bit_cast(u32, f); }
s32 asS16(u32 v) { return s32(s16(v & 0xFFFF)); }

// The HUD tuning halfwords (in .bss, set by the game's constructors and menus): their 4:3 values.
struct Halfword {
    u32 addr;
    s32 original;
};
constexpr Halfword kHuds[] = {{0x803E68E4, 36}, {0x803E68E8, -181}, {0x803E68F0, 589}, {0x803E6958, 6}, {0x803E69A4, 0}};

// ---- the aspect ----
// t for the screen (the 3D view, the frame's rectangle, the pictures) and for the HUD (0 with the HUD kept in the 4:3 middle).
std::atomic<float> s_t{0.f}, s_th{0.f};

// The values of the instructions that run native code: what each loads or sets for a t.
enum class V : u8 {
    ExtraEdge,    // f: 114 t (the HUD's offset to the window's edge)
    Scale,        // f: 1 + t/3 (the 3D view's 2D overlay, widened about the middle)
    EdgeFrom320,  // f: 320 - 114 t
    ViewX,        // f: the Picto Box picture's x
    ViewW,        // f: its width
    NameLeft,     // r: the bits of -(9 + 114 t) (the stage name's left edge)
    MapW,         // r: 116 + 37 t (the map's scissor)
    MapY,         // r: 32 t
    HudInt,       // r: 8 + 102 t
};
struct Site {
    u32 addr;
    V what;
    int reg;
    bool hud;   // t of the HUD, else of the screen
    bool cave;  // in the code's own caves
};
constexpr Site kSites[] = {
    {0x800037E4, V::NameLeft, 0, true, true},
    {0x80004060, V::Scale, 3, false, true},
    {0x800040C4, V::ExtraEdge, 0, true, true},
    {0x800040D8, V::ExtraEdge, 0, true, true},
    {0x800040EC, V::ExtraEdge, 0, true, true},
    {0x801F0678, V::ExtraEdge, 1, true, false},
    {0x801F0694, V::ExtraEdge, 1, true, false},
    {0x801F0700, V::ExtraEdge, 1, true, false},
    {0x801FC28C, V::EdgeFrom320, 0, true, false},
    {0x80201D8C, V::ExtraEdge, 4, true, false},
    {0x801F0B70, V::HudInt, 0, true, false},
    {0x8004A428, V::MapW, 3, true, false},
    {0x8004A444, V::MapY, 4, true, false},
    {0x80227A18, V::ViewX, 1, false, false},
    {0x80227A20, V::ViewW, 3, false, false},
};
constexpr u32 kClipperSite = 0x8017BF90;  // before mDoLib_clipper::setup in view_setup: f2 the view's aspect

void setSingle(Ctx& c, int reg, float v) { c.f[reg] = v, c.ps1[reg] = v; }

u32 runSite(Ctx& c, u32 pc, const Site& s) {
    const float t = s.hud ? s_th.load() : s_t.load();
    switch (s.what) {
    case V::ExtraEdge: setSingle(c, s.reg, 114.f * t); break;
    case V::Scale: setSingle(c, s.reg, 1.f + t / 3.f); break;
    case V::EdgeFrom320: setSingle(c, s.reg, 320.f - 114.f * t); break;
    case V::ViewW: setSingle(c, s.reg, 640.f / (1.f + t / 3.f)); break;
    case V::ViewX: setSingle(c, s.reg, 79.f * (640.f - 640.f / (1.f + t / 3.f)) / 160.f); break;
    case V::NameLeft: c.r[s.reg] = asBits(-(9.f + 114.f * t)); break;
    case V::MapW: c.r[s.reg] = u32(s32(std::lround(116.f + 37.f * t))); break;
    case V::MapY: c.r[s.reg] = u32(s32(std::lround(32.f * t))); break;
    case V::HudInt: c.r[s.reg] = u32(s32(std::lround(8.f + 102.f * t))); break;
    }
    return pc + 4;
}
std::vector<u32> s_siteWords;  // per kSites entry: its interpreter hook word
u32 s_clipperWord = 0;

const Code& code169() {
    static const Code s_169 = parse(kWide16x9);
    return s_169;
}
bool s_on = false;
bool s_clipperPatched = false;
std::vector<Write> s_original;  // the 4:3 values the data writes replaced (a 4:3 game's own)

s32 hudOriginal(u32 addr, bool* is) {
    for (const Halfword& h : kHuds)
        if (h.addr == addr) return *is = true, h.original;
    return *is = false, 0;
}
bool isHud(u32 addr) {
    bool is;
    hudOriginal(addr, &is);
    return is;
}

void apply(bool on) {
    const Code& code = code169();
    if (s_siteWords.empty()) {
        for (size_t k = 0; k < sizeof kSites / sizeof kSites[0]; k++) {
            const Site site = kSites[k];
            s_siteWords.push_back(interpHookWord([site](Ctx& c, u32 pc) -> u32 { return runSite(c, pc, site); }));
        }
        s_clipperWord = interpHookWord([](Ctx& c, u32) -> u32 {
            setSingle(c, 2, 4.f / 3.f);
            return kInterpOriginal;
        });
    }
    if (s_on) {
        for (const auto& patch : code.patches) interpUnpatch(patch.first);
        for (const Site& site : kSites) interpUnpatch(site.addr);
    }
    if (!on) {
        for (const Write& o : s_original) writeSized(o.addr, o.value, o.size);
    } else {
        if (s_original.empty())
            for (const Write& d : code.data) {
                bool hud;
                const s32 known = hudOriginal(d.addr, &hud);
                s_original.push_back({d.addr, hud ? u32(known) & 0xFFFF : readSized(d.addr, d.size), d.size});
            }
        for (const auto& [a, bytes] : code.caves) {
            for (size_t n = 0; n < bytes.size(); n++) wr8(a + u32(n), bytes[n]);
            interpStubRange(a, a + u32(bytes.size()));
        }
        for (const auto& [a, word] : code.patches) {
            interpPatch(a, word);
            interpOverrideAt(a);
        }
        for (size_t k = 0; k < sizeof kSites / sizeof kSites[0]; k++) {
            interpPatch(kSites[k].addr, s_siteWords[k]);
            if (!kSites[k].cave) interpOverrideAt(kSites[k].addr);
        }
    }
    log("[wide] widescreen code %s\n", on ? "on" : "off (4:3)");
    s_on = on;
    if (s_clipperPatched) interpUnpatch(kClipperSite);
    s_clipperPatched = false;  // (setClipper puts it back when the culling setting asks)
}

void setClipper(bool patched) {
    if (patched == s_clipperPatched) return;
    s_clipperPatched = patched;
    if (patched) {
        interpPatch(kClipperSite, s_clipperWord);
        interpOverrideAt(kClipperSite);
    } else {
        interpUnpatch(kClipperSite);
    }
}

// What a data write holds at a t.
u32 valueAt(const Write& d, u32 original, float t) {
    if (d.size == 2) {
        const float v = float(asS16(original)) + float(asS16(d.value) - asS16(original)) * t;
        return u32(s32(std::lround(v))) & 0xFFFF;
    }
    const float o = asFloat(original);
    return asBits(o + (asFloat(d.value) - o) * t);
}
}  // namespace

void twwWideFrame() {
    const float aspect = std::clamp(g_wideAspect.load(), 4.f / 3.f, 8.f);
    const float t = (aspect - 4.f / 3.f) / (16.f / 9.f - 4.f / 3.f);
    const bool on = t > 0.02f;
    s_t = on ? t : 0.f;
    s_th = on && g_wideHud.load() ? t : 0.f;
    if (on != s_on) apply(on);
    if (!s_on) return;
    const Code& code = code169();
    for (size_t i = 0; i < code.data.size(); i++) {
        const Write& d = code.data[i];
        writeSized(d.addr, valueAt(d, s_original[i].value, isHud(d.addr) ? s_th.load() : s_t.load()), d.size);
    }
    setClipper(!g_wideCulling.load());
}

}  // namespace rt
