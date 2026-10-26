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
#include <cstdlib>
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
    ViewX,        // f: the Picto Box picture's x (the viewport of dJle_Pb_c::pictureDraw: the whole screen)
    ViewW,        // f: its width
    OrthoL,       // f: the left edge of the picture's orthographic projection (widened to match the viewport)
    OrthoR,       // f: its right edge
    NameLeft,     // r: the bits of -(9 + 114 t) (the stage name's left edge)
    MapW,         // r: 116 + 37 t (the map's scissor)
    MapY,         // r: 32 t
    HudInt,       // r: 8 + 102 t
    NoPillar,     // r3: the address of a clear colour (a black box the code fills at each side of the Picto Box's view)
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
    {0x80227BA8, V::OrthoL, 3, false, false},
    {0x80227BAC, V::OrthoR, 4, false, false},
    {0x80004080, V::NoPillar, 3, false, true},
    {0x80004098, V::NoPillar, 3, false, true},
};
// The scope view (telescope, Picto Box viewfinder) darkens around its window with four black panes laid out in the 640x480
// screen, so on a wide window they stop at the 4:3 edges. At dDlst_2DSCP_c::draw (0x80237568) their bounds are made to
// reach far past the screen (the top and bottom bars both ways, the side bars outwards; the bars' own edges are kept so
// nothing overlaps while the scope fades in).
constexpr u32 kScopeDrawSite = 0x80237568;
constexpr u32 kScopeBlackPanes = 0x3DC + 4 * 0x38;  // sub_scp_class::mWpxxPanes[4..7] (fopMsgM_pane_class, 0x38 each)
constexpr float kFar = 1000.f;
// A pane of a J2DScreen by its tag (a four-character name), searched through its tree.
u32 findPane(u32 pane, u32 tag, int depth = 0) {
    if (rd32(pane + 8) == tag) return pane;
    for (u32 link = rd32(pane + 0xB0); link && depth < 8; link = rd32(link + 0xC))
        if (const u32 found = findPane(rd32(link), tag, depth + 1)) return found;
    return 0;
}
// A pane's bounds edge moved outwards; its matrix holds the position it was last moved to, so the top left is set there too.
void setLeft(u32 pane, float x) { wr32(pane + 0x0C, asBits(x)), wr32(pane + 0x48, asBits(x)); }
void setTop(u32 pane, float y) { wr32(pane + 0x10, asBits(y)), wr32(pane + 0x58, asBits(y)); }
void setRight(u32 pane, float x) { wr32(pane + 0x14, asBits(x)); }
void setBottom(u32 pane, float y) { wr32(pane + 0x18, asBits(y)); }
void widenScopeBlack(u32 self) {
    const u32 scp = rd32(self + 4);
    if (!scp) return;
    for (int i = 0; i < 4; i++) {  // wp04 above, wp05 below, wp07 left, wp06 right
        const u32 pane = rd32(scp + kScopeBlackPanes + 0x38 * i);
        if (!pane) continue;
        if (i < 2) setLeft(pane, -kFar), setRight(pane, 640.f + kFar);
        if (i == 0) setTop(pane, -kFar);
        if (i == 1) setBottom(pane, 480.f + kFar);
        if (i == 2) setLeft(pane, -kFar);
        if (i == 3) setRight(pane, 640.f + kFar);
    }
}
// Leaf panes spanning the whole 640 columns (a full-screen backdrop or band): their sides are moved out. A pane keeps its
// own alpha, colour and vertical extent, so it fades and moves with the screen's own animation.
void widenFullWidth(u32 pane, float ox = 0.f, float oy = 0.f, int depth = 0) {
    const float left = asFloat(rd32(pane + 0x0C)), top = asFloat(rd32(pane + 0x10)), right = asFloat(rd32(pane + 0x14)), bottom = asFloat(rd32(pane + 0x18));
    const u32 first = rd32(pane + 0xB0);
    if (!first) {
        const float gl = ox + left, gr = ox + right;
        const bool fullHeight = oy + top <= 0.5f && oy + bottom >= 479.5f;
        if (depth > 0 && right - left < kFar) {  // (not yet moved out)
            const bool toLeft = gl <= 0.5f && (gr >= 639.5f || fullHeight), toRight = gr >= 639.5f && (gl <= 0.5f || fullHeight);
            if (toLeft) setLeft(pane, left - kFar);
            if (toRight) setRight(pane, right + kFar);
        }
        return;
    }
    for (u32 link = first; link && depth < 8; link = rd32(link + 0xC)) widenFullWidth(rd32(link), ox + left, oy + top, depth + 1);
}
// The Picto Box's viewfinder (dJle_Pb_c::draw, 0x8022B320; its screen wipe_01_01 is at +4) darkens around its window with
// four black panes (wp01 below, wp02 right, wp03 left, wp04 above) that reach just past the 640x480 screen.
void widenPictoBlack(u32 self) {
    const u32 scrn = rd32(self + 4);
    if (!scrn) return;
    for (u32 n = 1; n <= 4; n++) {
        const u32 pane = findPane(scrn, 0x77703030 + n);
        if (!pane) continue;
        if (n == 1 || n == 4) setLeft(pane, -kFar), setRight(pane, 640.f + kFar);
        if (n == 1) setBottom(pane, 480.f + kFar);
        if (n == 2) setRight(pane, 640.f + kFar);
        if (n == 3) setLeft(pane, -kFar);
        if (n == 4) setTop(pane, -kFar);
    }
    // The album's backdrop and its darkening (full-width panes without children, in both screens) reach the window's sides too.
    widenFullWidth(scrn);
    if (const u32 scrn1 = rd32(self + 8)) widenFullWidth(scrn1);
}
u32 s_scopeWord = 0, s_pbWord = 0;
bool s_scopePatched = false;

constexpr u32 kClipperSite = 0x8017BF90;  // before mDoLib_clipper::setup in view_setup: f2 the view's aspect

void setSingle(Ctx& c, int reg, float v) { c.f[reg] = v, c.ps1[reg] = v; }

// dJle_Pb_c::pictureDraw (0x80227944) draws a photo through an orthographic projection of 659 units (-9..650) over a 640 wide
// viewport. The game's 4:3 mapping puts that viewport in the 4:3 middle of a wide window (width 640/s, s the aspect over 4/3)
// and anything outside it is clipped: the album's side photos lie out in the wide layout. The viewport is the whole screen
// and the projection reaches as far out as the viewport did, so each photo keeps its size and place.
float pictureViewW(float t) { return 640.f / (1.f + t / 3.f); }
float pictureViewX(float t) { return 79.f * (640.f - pictureViewW(t)) / 160.f; }
u32 runSite(Ctx& c, u32 pc, const Site& s) {
    const float t = s.hud ? s_th.load() : s_t.load();
    switch (s.what) {
    case V::ExtraEdge: setSingle(c, s.reg, 114.f * t); break;
    case V::Scale: setSingle(c, s.reg, 1.f + t / 3.f); break;
    case V::EdgeFrom320: setSingle(c, s.reg, 320.f - 114.f * t); break;
    case V::ViewW: setSingle(c, s.reg, 640.f); break;
    case V::ViewX: setSingle(c, s.reg, 0.f); break;
    case V::OrthoL: setSingle(c, s.reg, -9.f - pictureViewX(t) * 659.f / pictureViewW(t)); break;
    case V::OrthoR: setSingle(c, s.reg, 650.f + (640.f - pictureViewX(t) - pictureViewW(t)) * 659.f / pictureViewW(t)); break;
    case V::NameLeft: c.r[s.reg] = asBits(-(9.f + 114.f * t)); break;
    case V::MapW: c.r[s.reg] = u32(s32(std::lround(116.f + 37.f * t))); break;
    case V::MapY: c.r[s.reg] = u32(s32(std::lround(32.f * t))); break;
    case V::HudInt: c.r[s.reg] = u32(s32(std::lround(8.f + 102.f * t))); break;
    case V::NoPillar:  // (the code's two opaque boxes, at once black on the sides, are the four panes' job now: they fade with the view)
        wr32(c.r[1] + 8, 0);
        c.r[s.reg] = c.r[1] + 8;
        break;
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
        s_pbWord = interpHookWord([](Ctx& c, u32) -> u32 {
            widenPictoBlack(c.r[3]);
            return kInterpOriginal;
        });
        s_scopeWord = interpHookWord([](Ctx& c, u32) -> u32 {
            widenScopeBlack(c.r[3]);
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
    if (s_scopePatched) interpUnpatch(kScopeDrawSite);
    s_scopePatched = on;
    if (on) {
        interpPatch(0x8022B320, s_pbWord);
        interpOverrideAt(0x8022B320);
        interpPatch(kScopeDrawSite, s_scopeWord);
        interpOverrideAt(kScopeDrawSite);
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

float twwWideScale() { return 1.f + s_t.load() / 3.f; }

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
