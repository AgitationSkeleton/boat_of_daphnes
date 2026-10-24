// See ship_menu.h. Dusklight's structure (TwilitRealm/dusklight, CC0: res/rml/theme.rcss, window.rcss, tabbing.rcss,
// controls.rcss, popup.rcss; src/dusk/ui/select_button.cpp, bool_button.cpp, number_button.cpp, pane.cpp), drawn with
// The Wind Waker's own art, read from the player's disc while the game runs (fs_art.h; nothing of the game is shipped):
//   - every panel is the menus' paper (res/Msg/menures.arc: note_02, a grey grain the game tints per panel between two
//     colours of its screen layouts), with the torn strips along its sides (note_03_1), as the game builds its windows;
//   - the page's window in the sea chart's colours (fmapres: its paper E0C15F-FEF0BE on its frame 7B4E2F-D2AA28), the
//     menu bar in the options screen's green (235F19-78A537), the rows the file select's plates (FFF0BE-FFFFFF), the
//     row in focus their green (649632-B4DC64) with the pause menu's cursor at its corners (cursor_00_01);
//   - the game's own lettering (res/Msg/fontres.arc: rock_24_20_4i_usa.bfn; the file select's outlined title font);
//   - the Hylian script faint in the tabs: each tab's own name, written as the game writes Hylian (by its kana).
// Before the disc is open the same shapes are drawn in flat colours.
#ifdef rename
#undef rename  // bw_posix_compat.h's, which <filesystem> (fs_art.h) must not see
#endif

#include "ship_menu.h"

#include "fs_art.h"
#include "ship/cvar.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace shipmenu {

namespace {

// ---- The Wind Waker's palette ----
// Ink and paper for ImGui's own widgets (the right pane's controls, the windows the menu opens), and the plates' greens.
const ImVec4 kParchment(0xF6 / 255.f, 0xE6 / 255.f, 0xB4 / 255.f, 1.f);
const ImVec4 kParchmentLight(0xFF / 255.f, 0xF6 / 255.f, 0xDC / 255.f, 1.f);
const ImVec4 kParchmentDark(0xE0 / 255.f, 0xC1 / 255.f, 0x5F / 255.f, 1.f);
const ImVec4 kParchmentEdge(0x7B / 255.f, 0x4E / 255.f, 0x2F / 255.f, 1.f);
const ImVec4 kInk(0x46 / 255.f, 0x2D / 255.f, 0x14 / 255.f, 1.f);
const ImVec4 kGreen(0x28 / 255.f, 0x6E / 255.f, 0x1E / 255.f, 1.f);
const ImVec4 kGreenLight(0x64 / 255.f, 0x96 / 255.f, 0x32 / 255.f, 1.f);
const ImVec4 kGold(0xD2 / 255.f, 0xAA / 255.f, 0x28 / 255.f, 1.f);
const ImVec4 kGoldBright(0xFF / 255.f, 0xDC / 255.f, 0x3C / 255.f, 1.f);
const ImVec4 kSea(0x23 / 255.f, 0x5F / 255.f, 0x19 / 255.f, 1.f);
const ImVec4 kCream(0xFF / 255.f, 0xFB / 255.f, 0xEC / 255.f, 1.f);
const ImVec4 kRed(0x9E / 255.f, 0x2B / 255.f, 0x1E / 255.f, 1.f);

ImVec4 with_alpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }
ImU32 u32(ImVec4 c) { return ImGui::GetColorU32(c); }
ImU32 u32(ImVec4 c, float a) { return ImGui::GetColorU32(with_alpha(c, a)); }

// A panel's two colours, as the game's screen layouts give them (0xRRGGBBAA): the grain's dark and light.
struct Tint {
    uint32_t black, white;
};
constexpr Tint kChartPaper{0xE0C15F00u, 0xFEF0BEFFu};   // the sea chart's paper
constexpr Tint kChartFrame{0x7B4E2F00u, 0xD2AA28FFu};   // the sea chart's frame
constexpr Tint kBarWood{0x3A220E00u, 0x7A5226FFu};      // the menu bar: the chart frame's brown, darker
constexpr Tint kPlate{0xFFF0BE00u, 0xFFFFFFFFu};        // the file select's plates
constexpr Tint kPlateFocus{0x64963200u, 0xB4DC64FFu};   // a plate's green
constexpr Tint kPlateChosen{0x286E1E00u, 0x8CB450FFu};  // the file select's green plate
constexpr Tint kTabPaper{0xC8A04A00u, 0xF0D898FFu};     // a tab not chosen: the chart's paper, darker
constexpr Tint kLetter{0xF0D79000u, 0xFFF8E2FFu};       // the right pane: a letter's paper
constexpr const char* kMenuArc = "res/Msg/menures.arc";

ImU32 rgba(uint32_t c, float a = 1.f) {
    return IM_COL32(c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, int(float(c & 0xFF) * std::clamp(a, 0.f, 1.f)));
}
// A tint's colour where the grain is half way (the flat colour before the disc is open).
ImU32 flat(Tint t, float a = 1.f) {
    auto ch = [&](int shift) { return (int((t.black >> shift) & 0xFF) + int((t.white >> shift) & 0xFF)) / 2; };
    return IM_COL32(ch(24) + (255 - ch(24)) / 3, ch(16) + (255 - ch(16)) / 3, ch(8) + (255 - ch(8)) / 3, int(255 * std::clamp(a, 0.f, 1.f)));
}

float g_s = 1.f;  // the menu's scale while it draws

ImU32 mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int sh) { return int(float((a >> sh) & 0xFF) + (float((b >> sh) & 0xFF) - float((a >> sh) & 0xFF)) * t); };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), 255);
}

// ---- the game's paper ----
// note_02 between the tint's colours over a-b, at the grain's own scale where it fits (the game's panels are a few hundred
// pixels; a big one stretches it, as J2DPicture does).
void paper(ImDrawList* dl, ImVec2 a, ImVec2 b, Tint t, float alpha, float rounding = 0.f) {
    const fsart::Image im = fsart::Tinted(kMenuArc, "note_02.bti", t.black, t.white);
    if (im.tex == 0) {
        dl->AddRectFilled(a, b, flat(t, alpha), rounding);
        return;
    }
    const float span = 256.f * g_s;
    const float u = std::min(1.f, (b.x - a.x) / span), v = std::min(1.f, (b.y - a.y) / span);
    const float u0 = std::fmod(std::fabs(a.x) * 0.0007f, 1.f - u + 1e-4f), v0 = std::fmod(std::fabs(a.y) * 0.0011f, 1.f - v + 1e-4f);
    dl->AddImageRounded(im.tex, a, b, ImVec2(u0, v0), ImVec2(u0 + u, v0 + v), IM_COL32(255, 255, 255, int(alpha * 255.f)), rounding);
}

// The torn strips along a panel's sides (note_03_1, laid as the game's message windows lay them: the torn edge outwards,
// the sides the whole height, the top and bottom between), repeated along each side.
void torn_edges(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, Tint tint, float alpha) {
    const fsart::Image im = fsart::Tinted(kMenuArc, "note_03_1.bti", tint.black, tint.white);
    if (im.tex == 0)
        return;
    const ImU32 col = IM_COL32(255, 255, 255, int(alpha * 255.f));
    const float L = t * 8.f;  // the strip is 128 x 16
    for (float x = a.x + t; x < b.x - t - 0.5f; x += L) {
        const float seg = std::min(L, b.x - t - x), u = seg / L;
        dl->AddImage(im.tex, ImVec2(x, a.y), ImVec2(x + seg, a.y + t), ImVec2(0, 0), ImVec2(u, 1), col);
        dl->AddImage(im.tex, ImVec2(x, b.y - t), ImVec2(x + seg, b.y), ImVec2(0, 1), ImVec2(u, 0), col);
    }
    for (float y = a.y; y < b.y - 0.5f; y += L) {
        const float seg = std::min(L, b.y - y), u = seg / L;
        dl->AddImageQuad(im.tex, ImVec2(a.x, y), ImVec2(a.x + t, y), ImVec2(a.x + t, y + seg), ImVec2(a.x, y + seg), ImVec2(0, 0), ImVec2(0, 1),
                         ImVec2(u, 1), ImVec2(u, 0), col);
        dl->AddImageQuad(im.tex, ImVec2(b.x - t, y), ImVec2(b.x, y), ImVec2(b.x, y + seg), ImVec2(b.x - t, y + seg), ImVec2(0, 1), ImVec2(0, 0),
                         ImVec2(u, 0), ImVec2(u, 1), col);
    }
}

// A window as the game builds one: the paper with torn strips round it.
void panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, Tint tint, float alpha = 1.f) {
    if (fsart::Tinted(kMenuArc, "note_02.bti", tint.black, tint.white).tex == 0) {
        dl->AddRectFilled(a, b, flat(tint, alpha), t * 0.5f);
        return;
    }
    paper(dl, ImVec2(a.x + t * 0.5f, a.y + t * 0.5f), ImVec2(b.x - t * 0.5f, b.y - t * 0.5f), tint, alpha);
    torn_edges(dl, a, b, t, tint, alpha);
}

// A plate (the file select's): rounded, the paper's grain, a fine darker edge.
void plate(ImDrawList* dl, ImVec2 a, ImVec2 b, Tint t, float alpha, bool shadow = true) {
    const float r = 9.f * g_s;
    if (shadow) dl->AddRectFilled(ImVec2(a.x + 2.f * g_s, a.y + 3.f * g_s), ImVec2(b.x + 2.f * g_s, b.y + 3.f * g_s), IM_COL32(60, 35, 10, int(50 * alpha)), r);
    paper(dl, a, b, t, alpha, r);
    dl->AddRect(a, b, IM_COL32(110, 75, 30, int(90 * alpha)), r, 0, 1.2f * g_s);
}

// The pause menu's cursor: its corner piece (white, outlined) at the four corners of a-b, tinted gold, bobbing.
void cursor(ImDrawList* dl, ImVec2 a, ImVec2 b, float alpha = 1.f) {
    if (alpha <= 0.01f) return;
    const fsart::Image im = fsart::ArcImage(kMenuArc, "cursor_00_01.bti");
    const float s = g_s, k = 22.f * s;
    // it comes in from a little further out as it appears
    const float bob = (1.5f + 2.f * (0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 7.f)) + (1.f - alpha) * 8.f) * s;
    const ImU32 col = IM_COL32(255, 222, 70, int(255 * alpha));
    if (im.tex == 0) {
        dl->AddRect(ImVec2(a.x - bob, a.y - bob), ImVec2(b.x + bob, b.y + bob), col, 9.f * s, 0, 3.f * s);
        return;
    }
    const float o = k * 0.35f + bob;
    dl->AddImage(im.tex, ImVec2(a.x - o, a.y - o), ImVec2(a.x - o + k, a.y - o + k), ImVec2(0, 0), ImVec2(1, 1), col);
    dl->AddImage(im.tex, ImVec2(b.x + o - k, a.y - o), ImVec2(b.x + o, a.y - o + k), ImVec2(1, 0), ImVec2(0, 1), col);
    dl->AddImage(im.tex, ImVec2(a.x - o, b.y + o - k), ImVec2(a.x - o + k, b.y + o), ImVec2(0, 1), ImVec2(1, 0), col);
    dl->AddImage(im.tex, ImVec2(b.x + o - k, b.y + o - k), ImVec2(b.x + o, b.y + o), ImVec2(1, 1), ImVec2(0, 0), col);
}

// ---- the menu's motion ----
// A value per item that eases towards 1 while `on` and back to 0 (a hover's green, a choice's), at `speed` per second.
float ease_to(ImGuiID id, bool on, float speed = 14.f) {
    static std::map<ImGuiID, float> values;
    float& v = values[id];
    const float k = 1.f - std::exp(-speed * ImGui::GetIO().DeltaTime);
    v += ((on ? 1.f : 0.f) - v) * k;
    if (std::fabs(v - (on ? 1.f : 0.f)) < 0.002f) v = on ? 1.f : 0.f;
    return v;
}
float ease_out(float t) { t = std::clamp(t, 0.f, 1.f); return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
// What a draw list got since vertex `from`, moved by (dx, dy) and faded to alpha.
void move_fade(ImDrawList* dl, int from, float dx, float dy, float alpha) {
    if (dx == 0.f && dy == 0.f && alpha >= 1.f) return;
    for (int i = from; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.pos.x += dx, v.pos.y += dy;
        const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        v.col = (v.col & ~IM_COL32_A_MASK) | (ImU32(float(a) * std::clamp(alpha, 0.f, 1.f)) << IM_COL32_A_SHIFT);
    }
}
// Between two colours, with an alpha.
ImU32 blend(ImU32 a, ImU32 b, float t, float alpha) {
    auto ch = [&](int sh) { return int(float((a >> sh) & 0xFF) + (float((b >> sh) & 0xFF) - float((a >> sh) & 0xFF)) * t); };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), int(255.f * std::clamp(alpha, 0.f, 1.f)));
}

ImFont* g_body = nullptr;
ImFont* g_heading = nullptr;
ImFont* g_title = nullptr;
void push_font(ImFont* f) { ImGui::PushFont(f ? f : ImGui::GetFont()); }

// ---- the game's lettering ----
// Font 0 is the message font, 1 the file select's title font (its letters outlined). Words the game's fonts have no
// letters for (anything past ASCII) are drawn in the menu's own font (RocknRoll One) instead.
bool ascii(const std::string& s) {
    for (const unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}
ImFont* body_font() { return g_body ? g_body : ImGui::GetFont(); }
float text_width(const std::string& s, float size, int font = 0) {
    if (ascii(s) && fsart::FontReady(font)) return fsart::TextWidth(size, s.c_str(), font);
    return body_font()->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.f, s.c_str()).x;
}
float text(ImDrawList* dl, ImVec2 at, float size, ImU32 col, const std::string& s, int font = 0) {
    if (ascii(s) && fsart::FontReady(font)) return fsart::DrawStringGradient(dl, at, size, col, col, s.c_str(), font);
    dl->AddText(body_font(), size * 0.8f, ImVec2(at.x, at.y + size * 0.1f), col, s.c_str());
    return body_font()->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.f, s.c_str()).x;
}
// With the game's soft shadow under it (its white text on green).
float text_shadowed(ImDrawList* dl, ImVec2 at, float size, ImU32 col, const std::string& s, int font = 0) {
    const int a = int((col >> IM_COL32_A_SHIFT) & 0xFF);
    text(dl, ImVec2(at.x + 1.5f * g_s, at.y + 1.5f * g_s), size, IM_COL32(30, 20, 8, a * 2 / 3), s, font);
    return text(dl, at, size, col, s, font);
}
// Words wrapped to `width`, lines `lineH` apart; returns the height used.
float text_wrapped(ImDrawList* dl, ImVec2 at, float size, ImU32 col, const std::string& s, float width, float lineH) {
    float y = at.y;
    std::string line, word;
    auto flush = [&] {
        text(dl, ImVec2(at.x, y), size, col, line);
        y += lineH;
        line.clear();
    };
    auto add = [&] {
        if (word.empty()) return;
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && text_width(next, size) > width) {
            flush();
            line = word;
        } else {
            line = next;
        }
        word.clear();
    };
    for (const char c : s) {
        if (c == ' ') add();
        else if (c == '\n') add(), flush();
        else word += c;
    }
    add();
    if (!line.empty()) flush();
    return y - at.y;
}

// ---- the Hylian script ----
// The Wind Waker writes Hylian by Japanese kana: a glyph per kana, of straight stems that flare at their feet, bars and
// a filled "eye" (the in-game letters, as charted by the Zelda wikis; redrawn here as strokes). Each glyph is strokes in
// a 0-10 box: "H y x0 x1" a bar, "V x y0 y1" a stem, "D x y" the eye. A voiced kana (g, z, d, b) has two short bars over
// the glyph, a p one a small hollow box.
struct Kana {
    const char* key;
    const char* strokes;
};
const Kana kKana[] = {
    {"a", "H0,1,8 V7.5,0,6 V2.5,1.5,10"},          {"i", "H0,0.5,6 V1.5,0,10"},
    {"u", "V1.5,0,6 H2,1.5,8.5 V7.5,2,10"},        {"e", "H0.5,1,7 V4,0.5,9.5 H9.5,0.5,8.5"},
    {"o", "H2,0,9 V8.5,0,10 V2.5,3,10"},           {"ka", "H1,1,9 V1.5,1,10 V8,1,9.5 D3.5,2.5"},
    {"ki", "V4.5,0,10 H2.2,0.5,9 H5,2,8"},         {"ku", "H1,0,10 V8,1,10 V1,1,5 D3,3"},
    {"ke", "V1.5,0,4.5 H4.5,1.5,8 V8,0,10"},       {"ko", "H0.5,0,9 V8.5,0.5,9.5 H9.5,1,9"},
    {"sa", "V0.5,0,6 H2,0.5,8.5 V8,0,10"},         {"si", "H0.5,0,7 H2.5,1,7 V7,0,10"},
    {"su", "H0.5,0,8 V0.5,0.5,9.5 V7,3,9.5"},      {"se", "H0,0,9 V0.5,0,10 H10,0,10 V8.5,0,3 V9.5,8,10 D3,2"},
    {"so", "V6.5,0,10 V1.5,3.5,7"},                {"ta", "H0.5,0,8 V1,0.5,4 H4,1,8 V7.5,0.5,10"},
    {"ti", "H0.5,0,10 V5,2.5,10 H5,2,8"},          {"tu", "V6,0,10 H2,1,6 H5,1,6"},
    {"te", "H0.5,0,9 H4.5,1.5,7 V4.5,4.5,10"},     {"to", "V3.5,0,4.5 H4.5,0.5,8 V1.5,4.5,10"},
    {"na", "V4,0,10 H2.5,0.5,8"},                  {"ni", "H1,0,9 V4.5,4,9 H9.5,0,9"},
    {"nu", "H1,0,5 V5,1,10 H5,1,9"},               {"ne", "V4.5,0.5,2 H2.5,0,10 H5,1,8 V4.5,5,10"},
    {"no", "V1.5,0,10"},                           {"ha", "V0.5,0,10 V6.5,0,10 H3.5,0.5,6.5"},
    {"hi", "V0,0,10 H2.7,0,7 H9.5,0,8"},           {"hu", "H0.5,0,10 V7.5,0.5,10"},
    {"he", "V1,0,10 V5.5,3.5,10"},                 {"ho", "H1,0,10 V1,1,9.5 V8,1,9.5 V4.5,3,10"},
    {"ma", "H0.3,0,10 V8,0,9.5 V2.5,3.5,9.5"},     {"mi", "H0.3,0,8 V8,0,6 H2.5,0,8 V0.5,2.5,10 H10,0,8"},
    {"mu", "V1,0,10 H9.5,1,10 V9.5,7,9.5"},        {"me", "V4.5,0,10 H6.5,0.5,9"},
    {"mo", "V0,1,10 H1,0,9 H4.5,0,7 H9.5,0,10 D3,3"}, {"ya", "V0.5,0,10 H2.5,0.5,8 V8,2.5,5.5"},
    {"yu", "H0.5,0,5 V4,0.5,10 H9.5,3,10"},        {"yo", "H0.5,1,10 V8.5,0.5,10 H4.5,0,9 V1,4,10 H10,0,9"},
    {"ra", "H0.5,0,10 V1,0.5,2 H3,1,10 V8,3,10"},  {"ri", "V1,0,4 V6,1,10"},
    {"ru", "V2,0.5,10 V7,0,10"},                   {"re", "V1,0,10 H9.5,1,10"},
    {"ro", "H0.3,0,9 V0.5,0,10 V8,0.3,8 H10,0,9 D3,2"}, {"wa", "H0.5,0,9 V0.5,0.5,3 V8,0.5,10 H10,2,10 V2.5,7,10 D4,3"},
    {"wo", "H1,0,10 H4.5,1.5,10 V8.5,1,10"},       {"n", "V6.5,0,10 H4.5,1,6 H2.5,1,4.5"},
    {"-", "H5.5,0,10"},
};
const char* kana_strokes(const std::string& key) {
    for (const Kana& k : kKana)
        if (key == k.key) return k.strokes;
    return nullptr;
}

// One glyph in the box at-(at + w, h).
void hylian_glyph(ImDrawList* dl, const char* strokes, ImVec2 at, float w, float h, ImU32 col) {
    if (dl == nullptr) return;
    const float t = std::max(1.5f, h * 0.15f);
    auto X = [&](float x) { return at.x + t * 0.5f + x / 10.f * (w - t); };
    auto Y = [&](float y) { return at.y + t * 0.5f + y / 10.f * (h - t); };
    for (const char* p = strokes; *p;) {
        const char kind = *p++;
        float v[3] = {0, 0, 0};
        int n = 0;
        while (*p && *p != ' ' && n < 3) {
            v[n++] = std::strtof(p, const_cast<char**>(&p));
            if (*p == ',') ++p;
        }
        if (*p == ' ') ++p;
        if (kind == 'H') {
            dl->AddRectFilled(ImVec2(X(v[1]) - t * 0.5f, Y(v[0]) - t * 0.5f), ImVec2(X(v[2]) + t * 0.5f, Y(v[0]) + t * 0.5f), col);
        } else if (kind == 'V') {
            dl->AddRectFilled(ImVec2(X(v[0]) - t * 0.5f, Y(v[1]) - t * 0.5f), ImVec2(X(v[0]) + t * 0.5f, Y(v[2]) + t * 0.5f), col);
            if (v[2] >= 9.f)  // the foot flares
                dl->AddRectFilled(ImVec2(X(v[0]) - t * 0.85f, Y(v[2]) - t * 0.1f), ImVec2(X(v[0]) + t * 0.85f, Y(v[2]) + t * 0.5f), col);
        } else if (kind == 'D') {
            dl->AddCircleFilled(ImVec2(X(v[0]), Y(v[1])), t * 0.65f, col, 8);
        }
    }
}

// English, as the game's Japanese writes a foreign word: its syllables by kana ("Settings": se-t-ti-n-gu-su).
struct Syllable {
    std::string key;
    int mark = 0;        // 1 voiced, 2 p
    bool small = false;  // a small tsu: the next consonant doubled
};
std::vector<Syllable> to_kana(const std::string& word) {
    std::vector<Syllable> out;
    auto vowel = [](char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; };
    std::string w;
    for (const char c : word)
        if (std::isalpha(static_cast<unsigned char>(c))) w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    char lastVowel = 0;
    for (size_t i = 0; i < w.size();) {
        const char c = w[i];
        const char next = i + 1 < w.size() ? w[i + 1] : 0;
        if (vowel(c) || (c == 'y' && !vowel(next))) {
            const char v = c == 'y' ? 'i' : c;
            out.push_back({v == lastVowel ? "-" : std::string(1, v)});
            lastVowel = v;
            ++i;
            continue;
        }
        lastVowel = 0;
        if (c == 'h' && i > 0 && !vowel(w[i - 1])) { ++i; continue; }  // sh, ch, th, ph: the h is in the first
        if (c == 'c' && next == 'k') { ++i; continue; }
        if (c == 'n' && !vowel(next) && next != 'y') { out.push_back({"n"}); ++i; continue; }
        if (next == c) { out.push_back({"tu", 0, true}); ++i; continue; }
        char row = 'k';
        int mark = 0;
        switch (c) {
        case 'k': case 'c': case 'q': row = 'k'; break;
        case 'g': row = 'k', mark = 1; break;
        case 's': case 'x': row = 's'; break;
        case 'z': case 'j': row = 's', mark = 1; break;
        case 't': row = 't'; break;
        case 'd': row = 't', mark = 1; break;
        case 'n': row = 'n'; break;
        case 'h': case 'f': row = 'h'; break;
        case 'b': case 'v': row = 'h', mark = 1; break;
        case 'p': row = 'h', mark = 2; break;
        case 'm': row = 'm'; break;
        case 'y': row = 'y'; break;
        case 'r': case 'l': row = 'r'; break;
        case 'w': row = 'w'; break;
        default: break;
        }
        char v = 0;
        if (vowel(next)) v = next, i += 2, lastVowel = next;
        else v = (row == 't') ? 'o' : 'u', i += 1;
        std::string key = std::string(1, row) + v;
        if (!kana_strokes(key)) key = std::string(1, v);  // yi, ye, wi, wu, we: the vowel
        out.push_back({key, mark});
    }
    return out;
}

// Text in Hylian, `h` tall, from `at`; returns the width written.
float hylian(ImDrawList* dl, ImVec2 at, float h, ImU32 col, const std::string& textIn, float gapK = 0.34f, float spaceK = 0.8f) {
    const float w = h * 0.62f, gap = h * gapK;
    float x = at.x;
    size_t start = 0;
    while (start <= textIn.size()) {
        size_t end = textIn.find(' ', start);
        if (end == std::string::npos) end = textIn.size();
        for (const Syllable& syl : to_kana(textIn.substr(start, end - start))) {
            const char* strokes = kana_strokes(syl.key);
            if (!strokes) continue;
            if (syl.small) {
                hylian_glyph(dl, strokes, ImVec2(x, at.y + h * 0.4f), w * 0.6f, h * 0.6f, col);
                x += w * 0.6f + gap;
                continue;
            }
            if (syl.mark == 0) {
                hylian_glyph(dl, strokes, ImVec2(x, at.y), w, h, col);
            } else {
                const float t = std::max(1.f, h * 0.1f);
                if (dl == nullptr) {
                } else if (syl.mark == 1) {
                    dl->AddRectFilled(ImVec2(x + w * 0.3f, at.y), ImVec2(x + w * 0.7f, at.y + t), col);
                    dl->AddRectFilled(ImVec2(x + w * 0.3f, at.y + t * 2.f), ImVec2(x + w * 0.7f, at.y + t * 3.f), col);
                } else {
                    dl->AddRect(ImVec2(x + w * 0.3f, at.y), ImVec2(x + w * 0.7f, at.y + t * 3.f), col, 0.f, 0, t * 0.9f);
                }
                hylian_glyph(dl, strokes, ImVec2(x, at.y + h * 0.36f), w, h * 0.64f, col);
            }
            x += w + gap;
        }
        x += w * spaceK;
        start = end + 1;
    }
    return x - at.x;
}

std::string lower_no_spaces(const std::string& s) {
    std::string out;
    for (const char c : s)
        if (c != ' ')
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::string upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// A row's key: the name without a value format ("Internal Resolution: %d %%" -> "Internal Resolution").
std::string key_of(const WidgetInfo& w) {
    std::string k = w.name;
    if (const size_t p = k.find(':'); p != std::string::npos && k.find('%', p) != std::string::npos) k.erase(p);
    if (const size_t p = k.find("%"); p != std::string::npos) k.erase(p);
    while (!k.empty() && (k.back() == ' ' || k.back() == ':')) k.pop_back();
    return k;
}

void tooltip(const std::string& text) {
    if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// A plate button (the right pane's choices, the bar's buttons): the file select's plate, its green under the pointer with
// the cursor round it, the chosen one green.
bool plate_button(const char* id, const std::string& label, ImVec2 size, bool on, bool disabled = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    ImGui::BeginDisabled(disabled);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hot = !disabled && (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible));
    ImGui::EndDisabled();
    const float alpha = disabled ? 0.45f : 1.f;
    const ImGuiID iid = ImGui::GetItemID();
    const float hotA = ease_to(iid, hot), onA = ease_to(iid + 1, on);
    plate(dl, a, b, kPlate, alpha);
    if (hotA > 0.01f) plate(dl, a, b, kPlateFocus, alpha * hotA, false);
    if (onA > 0.01f) plate(dl, a, b, kPlateChosen, alpha * onA, false);
    const float g = std::max(hotA, onA);
    const float ts = 20.f * g_s;
    const float tw = text_width(label, ts);
    const ImVec2 at(a.x + (size.x - tw) * 0.5f, a.y + (size.y - ts) * 0.5f);
    if (g > 0.01f) text(dl, ImVec2(at.x + 1.5f * g_s, at.y + 1.5f * g_s), ts, IM_COL32(30, 20, 8, int(170 * g * alpha)), label);
    text(dl, at, ts, blend(u32(kInk), IM_COL32_WHITE, g, alpha), label);
    cursor(dl, a, b, hotA);
    return pressed && !disabled;
}

// The paper look for ImGui's own widgets (the right pane's controls, the windows the menu opens).
int g_styleDepth = 0;
void push_parchment_style() {
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, with_alpha(kInk, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, with_alpha(kParchment, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, with_alpha(kParchmentLight, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, with_alpha(kParchmentEdge, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, with_alpha(kCream, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0xE4 / 255.f, 0xF2 / 255.f, 0xC4 / 255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0xD2 / 255.f, 0xEA / 255.f, 0xA4 / 255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kSea);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kSea);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, kSea);
    ImGui::PushStyleColor(ImGuiCol_Button, kGreenLight);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0x8C / 255.f, 0xB4 / 255.f, 0x50 / 255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGreen);
    ImGui::PushStyleColor(ImGuiCol_Header, with_alpha(kGreenLight, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, with_alpha(kGreenLight, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, with_alpha(kGreen, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kGreen);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, kGreen);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kGreenLight);
    ImGui::PushStyleColor(ImGuiCol_Separator, with_alpha(kParchmentEdge, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, with_alpha(kParchmentEdge, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, with_alpha(kGreenLight, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, kGreen);
    ImGui::PushStyleColor(ImGuiCol_Tab, with_alpha(kParchmentDark, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, with_alpha(kGreenLight, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, kParchmentLight);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, kGoldBright);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
    g_styleDepth++;
}
void pop_parchment_style() {
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(30);
    g_styleDepth--;
}

// The Menu Theme setting's colours (kept: its combobox lists them).
struct ThemeEntry {
    const char* name;
    ImVec4 color;
};
const ThemeEntry kThemes[] = {
    {"Red", ImVec4(0.55f, 0.0f, 0.0f, 1.0f)},        {"Dark Red", ImVec4(0.3f, 0.0f, 0.0f, 1.0f)},
    {"Orange", ImVec4(0.85f, 0.55f, 0.0f, 1.0f)},    {"Green", ImVec4(0.0f, 0.55f, 0.0f, 1.0f)},
    {"Dark Green", ImVec4(0.0f, 0.3f, 0.0f, 1.0f)},  {"Light Blue", ImVec4(0.0f, 0.24f, 0.8f, 1.0f)},
    {"Blue", ImVec4(0.08f, 0.03f, 0.65f, 1.0f)},     {"Dark Blue", ImVec4(0.03f, 0.0f, 0.5f, 1.0f)},
    {"Indigo", ImVec4(0.35f, 0.0f, 0.87f, 1.0f)},    {"Violet", ImVec4(0.5f, 0.0f, 0.9f, 1.0f)},
    {"Purple", ImVec4(0.31f, 0.0f, 0.67f, 1.0f)},    {"Brown", ImVec4(0.37f, 0.18f, 0.0f, 1.0f)},
    {"Gray", ImVec4(0.45f, 0.45f, 0.45f, 1.0f)},     {"Dark Gray", ImVec4(0.15f, 0.15f, 0.15f, 1.0f)},
};

// Lines of the Hylian script for a backdrop: names from the Great Sea.
const char* const kSeaWords[] = {"Hyrule", "Outset", "Windfall", "Dragon Roost", "Forest Haven", "Tetra", "Medli", "Makar",
                                 "Valoo", "Jabun", "Komali", "Daphnes", "Triforce", "Great Sea", "Red Lions", "Wind Waker",
                                 "Ganondorf", "Tingle", "Aryll", "Zephos", "Cyclos", "Fairy", "Rupee", "Courage"};

} // namespace

int theme_count() { return static_cast<int>(sizeof kThemes / sizeof kThemes[0]); }
const char* theme_name(int index) { return kThemes[std::clamp(index, 0, theme_count() - 1)].name; }
ImVec4 theme_color(int index) { return kThemes[std::clamp(index, 0, theme_count() - 1)].color; }

void SetFonts(ImFont* body, ImFont* heading, ImFont* title) {
    g_body = body;
    g_heading = heading;
    g_title = title;
}

void DrawHylianBackdrop(ImDrawList* dl, ImVec2 min, ImVec2 max, float scale, float alpha) {
    // A page written in Hylian: small letters in the chart's brown ink, close together, lines evenly spaced and wrapped at
    // the margin, a short line now and then where a passage ends. Sentences of the Great Sea's names, in order.
    const float h = 15.f * scale, lineStep = h * 1.9f, gapK = 0.24f, spaceK = 0.55f;
    const ImU32 col = u32(kInk, alpha);
    const size_t words = sizeof kSeaWords / sizeof kSeaWords[0];
    dl->PushClipRect(min, max, true);
    size_t word = 0;
    int line = 0;
    for (float y = min.y; y + h <= max.y; y += lineStep, ++line) {
        float x = min.x;
        const bool lastOfPassage = line % 5 == 4;
        const float end = lastOfPassage ? min.x + (max.x - min.x) * 0.55f : max.x;
        for (;;) {
            const char* wd = kSeaWords[word % words];
            const float ww = hylian(nullptr, ImVec2(x, y), h, col, wd, gapK, spaceK);
            if (x > min.x && x + ww - h * 0.62f * spaceK > end) break;
            hylian(dl, ImVec2(x, y), h, col, wd, gapK, spaceK);
            x += ww;
            ++word;
        }
    }
    dl->PopClipRect();
}

void classic_push_theme();  // ship_menu_classic.cpp
void classic_pop_theme();
void classic_popout_window(const char* title, const char* cvar, ImVec2 size, float background, float fontScale,
                           const std::function<void()>& body, const std::function<void()>& onClose);

bool ClassicTheme() { return CVarGetInteger("gSettings.Menu.Style", 0) == 1; }

namespace {
std::vector<bool> g_themeStack;  // which look each PushThemeStyle pushed (the theme may change between push and pop)
}
void PushThemeStyle() {
    g_themeStack.push_back(ClassicTheme());
    if (g_themeStack.back()) classic_push_theme();
    else push_parchment_style();
}
void PopThemeStyle() {
    const bool classic = !g_themeStack.empty() && g_themeStack.back();
    if (!g_themeStack.empty()) g_themeStack.pop_back();
    if (classic) classic_pop_theme();
    else pop_parchment_style();
}

Header& Menu::AddHeader(const std::string& label) {
    auto [it, added] = mHeaders.try_emplace(label);
    if (added) {
        it->second.label = label;
        std::string key;
        for (const char c : label)
            if (std::isalnum(static_cast<unsigned char>(c)))
                key += c;
        it->second.sidebarCVar = "gSettings.Menu." + key + "SidebarSection";
        mOrder.push_back(label);
    }
    return it->second;
}

Sidebar& Menu::AddSidebar(const std::string& header, const std::string& name, int columns) {
    Header& h = AddHeader(header);
    auto [it, added] = h.sidebars.try_emplace(name);
    if (added) {
        h.order.push_back(name);
        it->second.columns = std::clamp(columns, 1, 3);
        it->second.widgets.resize(it->second.columns);
    }
    return it->second;
}

WidgetInfo& Menu::AddWidget(const std::string& header, const std::string& sidebar, int column,
                            const std::string& name, WidgetType type) {
    Sidebar& s = AddSidebar(header, sidebar, column + 1);
    if (static_cast<int>(s.widgets.size()) <= column) {
        s.widgets.resize(column + 1);
        s.columns = std::max(s.columns, column + 1);
    }
    s.widgets[column].push_back(WidgetInfo{});
    WidgetInfo& w = s.widgets[column].back();
    w.name = name;
    w.type = type;
    return w;
}

bool Menu::IsRow(const WidgetInfo& w) const {
    switch (w.type) {
    case WidgetType::CVarCheckbox:
    case WidgetType::CVarCombobox:
    case WidgetType::CVarSliderInt:
    case WidgetType::CVarSliderFloat:
    case WidgetType::CVarInputText:
    case WidgetType::CVarColorPicker:
    case WidgetType::Button:
    case WidgetType::WindowButton:
        return true;
    default:
        return false;
    }
}

// The ImGui controls of a widget, as before: the right pane's options for a chosen row, the parts drawn in a page as
// they are (custom tools, windows, text), and the search's fallbacks.
void Menu::DrawWidget(WidgetInfo& w, float width) {
    w.disabledReasons.clear();
    w.hidden = false;
    if (w.preFunc)
        w.preFunc(w);
    if (w.hidden)
        return;
    if (w.sameLine)
        ImGui::SameLine();
    const bool disabled = !w.disabledReasons.empty();
    std::string tip = w.tooltip;
    if (disabled) {
        tip = "This setting is disabled because:";
        for (const auto& reason : w.disabledReasons)
            tip += "\n- " + reason;
    }
    ImGui::PushID(w.name.c_str());
    ImGui::BeginDisabled(disabled);
    bool changed = false;
    switch (w.type) {
    case WidgetType::CVarCheckbox: {
        bool value = CVarGetInteger(w.cvar.c_str(), w.defaultInt) != 0;
        if (ImGui::Checkbox(key_of(w).c_str(), &value)) {
            CVarSetInteger(w.cvar.c_str(), value ? 1 : 0);
            changed = true;
        }
        tooltip(tip);
        break;
    }
    case WidgetType::CVarInputText: {
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, "%s", CVarGetString(w.cvar.c_str(), w.defaultText.c_str()));
        ImGui::SetNextItemWidth(width);
        const int limit = std::clamp(w.maxInt > 0 ? w.maxInt : 255, 1, 255);
        if (ImGui::InputText("##text", buffer, static_cast<size_t>(limit) + 1u)) {
            CVarSetString(w.cvar.c_str(), buffer);
            changed = true;
        }
        break;
    }
    case WidgetType::CVarColorPicker: {
        const CVarColor d{static_cast<uint8_t>(w.defaultColor.x * 255.f), static_cast<uint8_t>(w.defaultColor.y * 255.f),
                          static_cast<uint8_t>(w.defaultColor.z * 255.f),
                          static_cast<uint8_t>(w.useAlpha ? w.defaultColor.w * 255.f : 255.f)};
        const CVarColor v = CVarGetColor(w.cvar.c_str(), d);
        float rgba[4] = {v.r / 255.f, v.g / 255.f, v.b / 255.f, w.useAlpha ? v.a / 255.f : 1.f};
        // (a swatch that opens the picker, as the Cosmetics Editor's rows do)
        const ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
        const bool edited = w.useAlpha ? ImGui::ColorEdit4("##colour", rgba, flags | ImGuiColorEditFlags_AlphaBar)
                                       : ImGui::ColorEdit3("##colour", rgba, flags);
        ImGui::SameLine();
        ImGui::TextUnformatted(key_of(w).c_str());
        if (std::memcmp(&v, &d, sizeof v) != 0) {
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                CVarSetColor(w.cvar.c_str(), d);
                changed = true;
            }
        }
        (void)width;
        if (edited) {
            CVarSetColor(w.cvar.c_str(), CVarColor{static_cast<uint8_t>(std::lround(rgba[0] * 255.f)),
                                                   static_cast<uint8_t>(std::lround(rgba[1] * 255.f)),
                                                   static_cast<uint8_t>(std::lround(rgba[2] * 255.f)),
                                                   static_cast<uint8_t>(w.useAlpha ? std::lround(rgba[3] * 255.f) : 255)});
            changed = true;
        }
        break;
    }
    case WidgetType::CVarCombobox: {
        // (the right pane: Dusklight's option buttons, the chosen one marked)
        int value = std::clamp(CVarGetInteger(w.cvar.c_str(), w.defaultInt), 0, std::max(0, static_cast<int>(w.items.size()) - 1));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f * mScale, 10.f * mScale));
        for (int i = 0; i < static_cast<int>(w.items.size()); ++i) {
            if (plate_button(("##opt" + std::to_string(i)).c_str(), w.items[i], ImVec2(width, 36.f * mScale), i == value)) {
                CVarSetInteger(w.cvar.c_str(), i);
                changed = true;
            }
        }
        ImGui::PopStyleVar();
        break;
    }
    case WidgetType::CVarSliderInt:
    case WidgetType::CVarSliderFloat: {
        const bool isInt = w.type == WidgetType::CVarSliderInt;
        int iv = CVarGetInteger(w.cvar.c_str(), w.defaultInt);
        float fv = CVarGetFloat(w.cvar.c_str(), w.defaultFloat);
        const float button = ImGui::GetFrameHeight();
        if (ImGui::Button("-", ImVec2(button, button))) {
            if (isInt) iv = std::max(w.minInt, iv - w.stepInt);
            else fv = std::max(w.minFloat, fv - w.stepFloat);
            changed = true;
        }
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(std::max(10.0f, width - 2.0f * (button + ImGui::GetStyle().ItemInnerSpacing.x)));
        if (isInt) changed |= ImGui::SliderInt("##slider", &iv, w.minInt, w.maxInt, "%d", ImGuiSliderFlags_AlwaysClamp);
        else changed |= ImGui::SliderFloat("##slider", &fv, w.minFloat, w.maxFloat, w.percent ? "" : "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button("+", ImVec2(button, button))) {
            if (isInt) iv = std::min(w.maxInt, iv + w.stepInt);
            else fv = std::min(w.maxFloat, fv + w.stepFloat);
            changed = true;
        }
        if (changed) {
            if (isInt) {
                if (w.stepInt > 1) iv = w.minInt + (iv - w.minInt + w.stepInt / 2) / w.stepInt * w.stepInt;
                CVarSetInteger(w.cvar.c_str(), std::clamp(iv, w.minInt, w.maxInt));
            } else {
                fv = std::round(fv / w.stepFloat) * w.stepFloat;
                CVarSetFloat(w.cvar.c_str(), std::clamp(fv, w.minFloat, w.maxFloat));
            }
        }
        break;
    }
    case WidgetType::Button:
    case WidgetType::WindowButton: {
        const bool open = w.type == WidgetType::WindowButton && CVarGetInteger(w.cvar.c_str(), 0) != 0;
        if (ImGui::Button(w.name.c_str(), ImVec2(w.sameLine ? 0.0f : width, 0.0f))) {
            if (w.type == WidgetType::WindowButton)
                CVarSetInteger(w.cvar.c_str(), open ? 0 : 1);
            changed = true;
        }
        tooltip(tip);
        break;
    }
    case WidgetType::Window: {
        const bool out = CVarGetInteger(w.cvar.c_str(), 0) != 0;
        if (ImGui::Button(out ? "Dock Back Into Menu" : "Popout", ImVec2(180.0f * mScale, 0.0f))) {
            CVarSetInteger(w.cvar.c_str(), out ? 0 : 1);
            CVarSaveNextFrame();
            changed = true;
        }
        tooltip(out ? "Brings the window back into the menu." : tip);
        ImGui::Separator();
        if (out) {
            ImGui::TextDisabled("%s is open in its own window. Closing that window brings it back here.", w.name.c_str());
        } else if (w.custom) {
            ImGui::BeginChild("##window", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, 360.0f * mScale)), ImGuiChildFlags_None);
            w.custom(w);
            ImGui::EndChild();
        }
        break;
    }
    case WidgetType::Text:
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        if (w.color.w > 0.0f) ImGui::TextColored(ImVec4(w.color.x * 0.6f, w.color.y * 0.6f, w.color.z * 0.6f, 1.f), "%s", w.name.c_str());
        else ImGui::TextUnformatted(w.name.c_str());
        ImGui::PopTextWrapPos();
        tooltip(tip);
        break;
    case WidgetType::SeparatorText: {
        // Dusklight's section heading, a little space above: in the game's letters, the chart's green, a dotted rule after
        ImGui::Dummy(ImVec2(0, 6.f * mScale));
        const float size = 22.f * mScale;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float tw = text(dl, at, size, u32(kGreen), w.name);
        for (float x = at.x + tw + 12.f * mScale; x < at.x + width - 4.f * mScale; x += 9.f * mScale)
            dl->AddCircleFilled(ImVec2(x, at.y + size * 0.55f), 1.6f * mScale, u32(kParchmentEdge, 0.45f), 6);
        ImGui::Dummy(ImVec2(width, size + 2.f * mScale));
        break;
    }
    case WidgetType::Separator:
        ImGui::Separator();
        break;
    case WidgetType::Custom:
        if (w.custom)
            w.custom(w);
        break;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    if (changed) {
        CVarSaveNextFrame();
        if (w.callback)
            w.callback(w);
    }
}

// One row: Dusklight's select-button (key | value), with its states (controls.rcss): a faint ring at rest, the green
// and a 2px ring under the pointer or the focus, a deeper green when chosen; a value changed from its default reads
// "• value" in bold. A checkbox's value is On or Off and a click turns it; a combobox's or slider's value steps with
// Left and Right; a button does its thing.
void Menu::DrawRow(WidgetInfo& w, float width) {
    w.disabledReasons.clear();
    w.hidden = false;
    if (w.preFunc) w.preFunc(w);
    if (w.hidden) return;
    const bool disabled = !w.disabledReasons.empty();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImDrawList* dl = window->DrawList;
    const float s = mScale;
    const float h = 40.f * s;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImRect bb(at, ImVec2(at.x + width, at.y + h));
    ImGui::PushID(&w);
    const ImGuiID id = window->GetID("##row");
    ImGui::ItemSize(bb);
    const bool visible = ImGui::ItemAdd(bb, id);
    bool hovered = false, held = false;
    const bool pressed = visible && !disabled && ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (disabled) hovered = ImGui::IsMouseHoveringRect(bb.Min, bb.Max);
    const bool focused = ImGui::GetFocusID() == id && ImGui::GetIO().NavVisible;
    if (hovered || focused) mHover = &w;
    const bool chosen = mChosen == &w;
    // the value, and whether it is not its default
    std::string value;
    bool changedFromDefault = false;
    int step = 0;
    if (!disabled && (focused || hovered) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    bool changed = false;
    switch (w.type) {
    case WidgetType::CVarCheckbox: {
        int v = CVarGetInteger(w.cvar.c_str(), w.defaultInt) != 0;
        if (pressed || step) {
            v = !v;
            CVarSetInteger(w.cvar.c_str(), v);
            changed = true;
        }
        value = v ? "On" : "Off";
        changedFromDefault = v != (w.defaultInt != 0);
        break;
    }
    case WidgetType::CVarCombobox: {
        const int n = std::max(1, static_cast<int>(w.items.size()));
        int v = std::clamp(CVarGetInteger(w.cvar.c_str(), w.defaultInt), 0, n - 1);
        if (step) {
            v = (v + step + n) % n;
            CVarSetInteger(w.cvar.c_str(), v);
            changed = true;
        }
        value = w.items.empty() ? "" : w.items[v];
        changedFromDefault = v != w.defaultInt;
        if (pressed) mChosen = &w;
        break;
    }
    case WidgetType::CVarSliderInt: {
        int v = CVarGetInteger(w.cvar.c_str(), w.defaultInt);
        if (step) {
            v = std::clamp(v + step * w.stepInt, w.minInt, w.maxInt);
            CVarSetInteger(w.cvar.c_str(), v);
            changed = true;
        }
        char b[128];
        std::snprintf(b, sizeof b, w.name.c_str(), v);
        const char* colon = std::strchr(b, ':');
        value = colon ? colon + 1 : std::to_string(v);
        changedFromDefault = v != w.defaultInt;
        if (pressed) mChosen = &w;
        break;
    }
    case WidgetType::CVarSliderFloat: {
        float v = CVarGetFloat(w.cvar.c_str(), w.defaultFloat);
        if (step) {
            v = std::clamp(v + float(step) * w.stepFloat, w.minFloat, w.maxFloat);
            CVarSetFloat(w.cvar.c_str(), v);
            changed = true;
        }
        char b[128];
        std::snprintf(b, sizeof b, w.name.c_str(), w.percent ? v * 100.0f : v);
        const char* colon = std::strchr(b, ':');
        value = colon ? colon + 1 : b;
        changedFromDefault = std::fabs(v - w.defaultFloat) > w.stepFloat * 0.5f;
        if (pressed) mChosen = &w;
        break;
    }
    case WidgetType::CVarInputText:
        value = CVarGetString(w.cvar.c_str(), w.defaultText.c_str());
        changedFromDefault = value != w.defaultText;
        if (pressed) mChosen = &w;
        break;
    case WidgetType::CVarColorPicker: {
        if (pressed) mChosen = &w;
        break;
    }
    case WidgetType::Button:
    case WidgetType::WindowButton:
        if (pressed) {
            if (w.type == WidgetType::WindowButton) CVarSetInteger(w.cvar.c_str(), CVarGetInteger(w.cvar.c_str(), 0) ? 0 : 1);
            changed = true;
        }
        break;
    default:
        break;
    }
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    // the row: the file select's plate; its green under the pointer or the focus, with the pause menu's cursor round it;
    // the deeper green when chosen. The key at the left and the value at the right in the game's letters; a value changed
    // from its default reads bold, a diamond before it.
    const float alpha = disabled ? 0.45f : 1.f;
    const bool lit = (hovered || focused) && !disabled;
    const float litA = ease_to(id, lit), chosenA = ease_to(id + 1, chosen);
    plate(dl, bb.Min, bb.Max, kPlate, alpha);
    if (litA > 0.01f) plate(dl, bb.Min, bb.Max, kPlateFocus, alpha * litA, false);
    if (chosenA > 0.01f) plate(dl, bb.Min, bb.Max, kPlateChosen, alpha * chosenA, false);
    const float green = std::max(litA, chosenA);
    const bool onGreen = green > 0.5f;
    const ImU32 ink = blend(u32(kInk), IM_COL32_WHITE, green, alpha);
    auto put = [&](ImVec2 p, float size, ImU32 col, const std::string& str) {
        if (green > 0.01f) text(dl, ImVec2(p.x + 1.5f * s, p.y + 1.5f * s), size, IM_COL32(30, 20, 8, int(170 * green * alpha)), str);
        return text(dl, p, size, col, str);
    };
    const float size = 21.f * s;
    const float ty = bb.Min.y + (h - size) * 0.5f;
    if (w.type == WidgetType::Button || w.type == WidgetType::WindowButton) {
        const float tw = text_width(w.name, size);
        put(ImVec2(bb.Min.x + (width - tw) * 0.5f, ty), size, ink, w.name);
    } else {
        put(ImVec2(bb.Min.x + 16.f * s, ty), size, ink, key_of(w));
        if (w.type == WidgetType::CVarColorPicker) {
            const CVarColor c = CVarGetColor(w.cvar.c_str(), CVarColor{uint8_t(w.defaultColor.x * 255), uint8_t(w.defaultColor.y * 255),
                                                                          uint8_t(w.defaultColor.z * 255), 255});
            const float sw = h - 16.f * s;
            const ImVec2 a(bb.Max.x - 16.f * s - sw * 1.6f, bb.Min.y + 8.f * s), b(bb.Max.x - 16.f * s, bb.Max.y - 8.f * s);
            dl->AddRectFilled(a, b, IM_COL32(c.r, c.g, c.b, 255), 4.f * s);
            dl->AddRect(a, b, u32(kInk, 0.6f), 4.f * s, 0, 1.5f * s);
        } else if (!value.empty()) {
            const ImU32 vcol = changedFromDefault ? (onGreen ? IM_COL32(255, 236, 110, int(255 * alpha)) : u32(kGreen, alpha)) : ink;
            const float vw = text_width(value, size) + (changedFromDefault ? 1.f : 0.f);
            const ImVec2 vat(bb.Max.x - 16.f * s - vw, ty);
            put(vat, size, vcol, value);
            if (changedFromDefault) {
                put(ImVec2(vat.x + 1.f, vat.y), size, vcol, value);
                const ImVec2 c(vat.x - 11.f * s, bb.Min.y + h * 0.5f);
                const float k = 4.f * s;
                dl->AddQuadFilled(ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y), ImVec2(c.x, c.y + k), ImVec2(c.x - k, c.y), vcol);
            }
        }
    }
    cursor(dl, bb.Min, bb.Max, litA);
    ImGui::PopID();
    if (changed) {
        CVarSaveNextFrame();
        if (w.callback) w.callback(w);
    }
}

// The right pane: the hovered (else chosen) row's name and help - or why it is disabled - and a chosen row's options.
void Menu::DrawHelpPane(float width, float height) {
    WidgetInfo* w = mHover ? mHover : mChosen;
    ImGui::BeginChild("##help", ImVec2(width, height), ImGuiChildFlags_None);
    const float s = mScale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int helpFrom = dl->VtxBuffer.Size;
    const float now = static_cast<float>(ImGui::GetTime());
    if (w != mHelpFor) mHelpFor = w, mHelpStart = now;
    const float helpIn = ease_out((now - mHelpStart) / 0.16f);
    const float wrap = width - 16.f * s;
    ImGui::Dummy(ImVec2(0, 4.f * s));
    ImVec2 at = ImGui::GetCursorScreenPos();
    float used = 0.f;
    const float body = 19.f * s, line = 27.f * s;
    if (w != nullptr) {
        used += text_wrapped(dl, at, 26.f * s, u32(kGreen), key_of(*w), wrap, 32.f * s) + 6.f * s;
        if (!w->disabledReasons.empty()) {
            used += text_wrapped(dl, ImVec2(at.x, at.y + used), body, u32(kRed), "This setting is unavailable because:", wrap, line);
            for (const auto& reason : w->disabledReasons)
                used += text_wrapped(dl, ImVec2(at.x + 14.f * s, at.y + used), body, u32(kInk), "- " + reason, wrap - 14.f * s, line);
        } else if (!w->tooltip.empty()) {
            used += text_wrapped(dl, ImVec2(at.x, at.y + used), body, u32(kInk), w->tooltip, wrap, line);
        }
    } else {
        used += text_wrapped(dl, at, body, u32(kInk, 0.6f),
                             "Point at a setting to read about it. Click one to see its choices here; Left and Right change it.", wrap, line);
    }
    ImGui::Dummy(ImVec2(wrap, used));
    // under the help, while nothing is chosen: lines of the Hylian script, as on a letter of the game
    const bool controls = mChosen != nullptr && (w == mChosen || mHover == nullptr) && mChosen->disabledReasons.empty();
    if (!controls) {
        const ImVec2 from = ImGui::GetCursorScreenPos();
        const ImGuiWindow* win = ImGui::GetCurrentWindow();
        DrawHylianBackdrop(dl, ImVec2(from.x, from.y + 24.f * s), ImVec2(win->InnerRect.Max.x - 8.f * s, win->InnerRect.Max.y - 6.f * s), s, 0.11f);
    }
    // the chosen row's controls (a combobox's choices, a slider, a text field, a colour)
    if (mChosen != nullptr && (w == mChosen || mHover == nullptr) && mChosen->disabledReasons.empty() &&
        (mChosen->type == WidgetType::CVarCombobox || mChosen->type == WidgetType::CVarSliderInt ||
         mChosen->type == WidgetType::CVarSliderFloat || mChosen->type == WidgetType::CVarInputText ||
         mChosen->type == WidgetType::CVarColorPicker)) {
        ImGui::Dummy(ImVec2(0, 8.f * s));
        at = ImGui::GetCursorScreenPos();
        for (float x = at.x; x < at.x + wrap; x += 9.f * s)
            dl->AddCircleFilled(ImVec2(x, at.y), 1.6f * s, u32(kParchmentEdge, 0.45f), 6);
        ImGui::Dummy(ImVec2(0, 14.f * s));
        const bool same = mChosen->sameLine;
        mChosen->sameLine = false;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.f * s);
        DrawWidget(*mChosen, wrap - 28.f * s);
        mChosen->sameLine = same;
    }
    move_fade(dl, helpFrom, 0.f, (1.f - helpIn) * 6.f * s, helpIn);
    ImGui::EndChild();
}

void Menu::DrawSearchResults(const std::string& query, float width) {
    const std::string q = lower_no_spaces(query);
    int found = 0;
    for (const auto& headerName : mOrder) {
        Header& h = mHeaders[headerName];
        for (const auto& sidebarName : h.order) {
            bool shownPlace = false;
            for (auto& col : h.sidebars[sidebarName].widgets) {
                for (auto& w : col) {
                    if (w.hideInSearch || !IsRow(w)) continue;
                    if (lower_no_spaces(w.name + w.tooltip).find(q) == std::string::npos) continue;
                    if (!shownPlace) {
                        push_font(g_heading);
                        ImGui::PushStyleColor(ImGuiCol_Text, with_alpha(kInk, 0.55f));
                        ImGui::TextUnformatted(upper(headerName + "  \xE2\x80\xBA  " + sidebarName).c_str());
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                        shownPlace = true;
                    }
                    ++found;
                    DrawRow(w, width);
                }
            }
        }
    }
    if (found == 0) ImGui::TextColored(with_alpha(kInk, 0.5f), "Nothing matches that.");
}

bool Menu::FireCallback(const std::string& cvar) {
    for (auto& [headerName, header] : mHeaders)
        for (auto& [sidebarName, sidebar] : header.sidebars)
            for (auto& column : sidebar.widgets)
                for (auto& w : column)
                    if (w.cvar == cvar && w.callback) {
                        w.callback(w);
                        return true;
                    }
    return false;
}

float g_menu_scale = 1.f;

float MenuScale() { return g_menu_scale; }

void PopoutWindow(const char* title, const char* cvar, ImVec2 size, float background, float fontScale,
                  const std::function<void()>& body, const std::function<void()>& onClose) {
    if (ClassicTheme()) {
        classic_popout_window(title, cvar, size, background, fontScale, body, onClose);
        return;
    }
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    bool open = true;
    push_parchment_style();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, with_alpha(kParchment, std::max(0.6f, background)));
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetWindowFontScale(fontScale);
        const ImGuiWindow* win = ImGui::GetCurrentWindow();
        DrawHylianBackdrop(ImGui::GetWindowDrawList(), win->InnerRect.Min, win->InnerRect.Max, g_menu_scale, 0.06f);
        body();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    pop_parchment_style();
    if (!open) {
        CVarSetInteger(cvar, 0);
        CVarSaveNextFrame();
        if (onClose)
            onClose();
    }
}

void Menu::Draw(bool* open, float scale, float fontScale, float appear, bool closing) {
    if (ClassicTheme()) {  // Boat of Daphnes' own menu: no slide, it comes and goes at once
        if (!closing) DrawClassic(open, scale, fontScale);
        return;
    }
    mScale = scale;
    g_menu_scale = scale;
    ImGuiIO& io = ImGui::GetIO();
    const float savedFontScale = io.FontGlobalScale;
    io.FontGlobalScale = fontScale;
    if (CVarGetInteger("gSettings.Menu.ControllerNavigation", 0))
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    push_font(g_body);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = scale;
    // The whole screen: the game dimmed, Dusklight's menu bar over the top.
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, CVarGetFloat("gSettings.Menu.BackgroundOpacity", 0.85f) * 0.7f * appear));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin("Main Menu", nullptr,
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (!visible) {
        ImGui::End();
        ImGui::PopFont();
        io.FontGlobalScale = savedFontScale;
        return;
    }
    push_parchment_style();
    if (closing) ImGui::BeginDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 view = vp->WorkSize;
    const float time = static_cast<float>(ImGui::GetTime());
    // Sliding down from above as it opens, settling with a little give (an ease out with overshoot); back up, quickening,
    // as it closes.
    float drop;
    if (!closing) {
        const float c1 = 1.2f, c3 = c1 + 1.f, t = std::clamp(appear, 0.f, 1.f) - 1.f;
        drop = 1.f + c3 * t * t * t + c1 * t * t;
    } else {
        drop = 1.f - (1.f - appear) * (1.f - appear);
    }
    const ImVec2 origin(vp->WorkPos.x, vp->WorkPos.y - (1.f - drop) * (view.y + 40.f * s));

    g_s = s;
    (void)time;
    // ---- the menu bar (popup.rcss): the options screen's green paper with its torn edge below; the headers in the game's
    // outlined letters, the shown one in the yellow the game marks a choice with ----
    const float barH = 60.f * s;
    paper(dl, origin, ImVec2(origin.x + view.x, origin.y + barH), kBarWood, 1.f);
    dl->AddRectFilled(ImVec2(origin.x, origin.y + barH - 3.f * s), ImVec2(origin.x + view.x, origin.y + barH), IM_COL32(210, 170, 40, 255));
    dl->AddRectFilled(ImVec2(origin.x, origin.y + barH), ImVec2(origin.x + view.x, origin.y + barH + 2.f * s), IM_COL32(40, 22, 8, 160));
    const std::string headerCVar = "gSettings.Menu.ActiveHeader";
    std::string active = CVarGetString(headerCVar.c_str(), mOrder.empty() ? "" : mOrder.front().c_str());
    if (!mHeaders.count(active) && !mOrder.empty()) active = mOrder.front();
    const ImU32 kYellow = IM_COL32(255, 232, 90, 255);
    // what fits: the headers between the title and Restart, Quit and Close; the title goes first when they don't, then the
    // headers' letters get smaller
    float barText = 23.f * s;
    const float rightBlock = barH + text_width("Restart", barText, 1) + text_width("Quit", barText, 1) + 64.f * s;
    const float titleSize = 30.f * s, titleW = text_width("Boat of Daphnes", titleSize, 1) + 36.f * s;
    float headersW = 0.f;
    for (const std::string& h : mOrder) headersW += text_width(h, barText, 1) + 32.f * s;
    const float room = view.x - 28.f * s - rightBlock - 12.f * s;
    const bool showTitle = titleW + headersW <= room;
    if (!showTitle && headersW > room && headersW > 0.f) {
        const float pad = 32.f * s * float(mOrder.size());
        barText = std::max(14.f * s, barText * std::max(0.f, room - pad) / std::max(1.f, headersW - pad));
    }
    float x = origin.x + 28.f * s;
    if (showTitle)
        x += text_shadowed(dl, ImVec2(x, origin.y + (barH - titleSize) * 0.5f), titleSize, IM_COL32_WHITE, "Boat of Daphnes", 1) + 36.f * s;
    const float smallText = 23.f * s;
    static float s_arrow = -1.f;  // where the arrow under the shown header is (from the bar's left)
    float arrowTo = -1.f;
    auto barTab = [&](const std::string& label, bool selected, ImU32 colour) {
        const float tw = text_width(label, barText, 1);
        const ImRect bb(ImVec2(x, origin.y), ImVec2(x + tw + 32.f * s, origin.y + barH));
        ImGui::SetCursorScreenPos(bb.Min);
        const bool pressed = ImGui::InvisibleButton(("##bar" + label).c_str(), bb.GetSize());
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        const float hotA = ease_to(ImGui::GetItemID(), hot), selA = ease_to(ImGui::GetItemID() + 1, selected);
        if (hotA > 0.01f)
            paper(dl, ImVec2(bb.Min.x + 4.f * s, bb.Min.y + 10.f * s), ImVec2(bb.Max.x - 4.f * s, bb.Max.y - 10.f * s), Tint{0x7A522600u, 0xB8894AFFu},
                  0.9f * hotA, 8.f * s);
        const ImU32 rest = (colour & ~IM_COL32_A_MASK) | (215u << IM_COL32_A_SHIFT);
        const ImU32 col = blend(blend(rest, IM_COL32_WHITE, hotA, 1.f), kYellow, selA, 1.f);
        text_shadowed(dl, ImVec2(bb.Min.x + 16.f * s, bb.Min.y + (barH - barText) * 0.5f), barText, col, label, 1);
        if (selected) arrowTo = bb.GetCenter().x - origin.x;
        x = bb.Max.x;
        return pressed;
    };
    for (const std::string& h : mOrder)
        if (barTab(h, h == active, IM_COL32_WHITE)) {
            CVarSetString(headerCVar.c_str(), h.c_str());
            CVarSaveNextFrame();
            mSearch[0] = '\0';
            active = h;
        }
    if (arrowTo >= 0.f) {  // the game's little arrow under the shown header, gliding to a new one
        s_arrow = s_arrow < 0.f ? arrowTo : s_arrow + (arrowTo - s_arrow) * (1.f - std::exp(-16.f * io.DeltaTime));
        const float cx = origin.x + s_arrow, by = origin.y + barH - 6.f * s, k = 6.f * s;
        dl->AddTriangleFilled(ImVec2(cx - k, by), ImVec2(cx + k, by), ImVec2(cx, by - k), kYellow);
    }
    // the right end: Restart, Quit and Close
    {
        const float closeW = barH;
        barText = smallText;
        float right = origin.x + view.x - closeW;
        const float quitW = text_width("Quit", barText, 1) + 32.f * s, restartW = text_width("Restart", barText, 1) + 32.f * s;
        right -= quitW + restartW;
        x = right;
        if (barTab("Restart", false, IM_COL32(255, 214, 200, 255)))
            Ask({"Restart", "Back to the title screen? Unsaved progress will be lost.", "Restart", "Cancel", onRestart});
        if (barTab("Quit", false, IM_COL32(255, 214, 200, 255))) Ask({"Quit", "Are you sure you want to quit?", "Quit", "Cancel", onQuit});
        // close: an X, outlined as the game's letters are
        const ImRect cb(ImVec2(origin.x + view.x - closeW, origin.y), ImVec2(origin.x + view.x, origin.y + barH));
        ImGui::SetCursorScreenPos(cb.Min);
        if (ImGui::InvisibleButton("##close", cb.GetSize())) *open = false;
        const bool hot = ImGui::IsItemHovered();
        const ImVec2 c = cb.GetCenter();
        const float k = 9.f * s;
        for (int pass = 0; pass < 2; ++pass) {
            const ImU32 col = pass == 0 ? IM_COL32(40, 22, 8, 210) : (hot ? kYellow : IM_COL32_WHITE);
            const float th = (pass == 0 ? 6.f : 3.f) * s;
            dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, th);
            dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, th);
        }
    }

    // ---- the page's window (window.rcss): the sea chart's paper on its frame, torn at the edges ----
    const float margin = 44.f * s;
    const float sideM = std::max(margin, (view.x - 1280.f * s) * 0.5f);
    const ImVec2 winMin(origin.x + sideM, origin.y + barH + 34.f * s);
    const ImVec2 winMax(origin.x + view.x - sideM, origin.y + view.y - 30.f * s);
    const float frameW = 12.f * s;
    dl->AddRectFilled(ImVec2(winMin.x - frameW + 6.f * s, winMin.y - frameW + 8.f * s), ImVec2(winMax.x + frameW + 6.f * s, winMax.y + frameW + 8.f * s),
                      IM_COL32(0, 0, 0, 70), 10.f * s);  // its shadow
    {
        const ImVec2 fa(winMin.x - frameW, winMin.y - frameW), fb(winMax.x + frameW, winMax.y + frameW);
        paper(dl, fa, fb, kChartFrame, 1.f, 10.f * s);
        dl->AddRect(fa, fb, IM_COL32(70, 42, 18, 220), 10.f * s, 0, 2.f * s);
    }
    panel(dl, ImVec2(winMin.x + 2.f * s, winMin.y + 2.f * s), ImVec2(winMax.x - 2.f * s, winMax.y - 2.f * s), 11.f * s, kChartPaper);

    Header& header = mHeaders[active];
    std::string page = CVarGetString(header.sidebarCVar.c_str(), header.order.empty() ? "" : header.order.front().c_str());
    if (!header.sidebars.count(page) && !header.order.empty()) page = header.order.front();

    // ---- the tab bar (tabbing.rcss): little torn leaves of paper, the chosen one the file select's green; each with its
    // name faint behind it in Hylian ----
    const float tabH = 46.f * s;
    float tabBarH = tabH;  // more when the tabs take two rows
    const ImVec2 tabMin(winMin.x + 22.f * s, winMin.y + 20.f * s);
    std::string query = mSearch;
    query.erase(std::remove(query.begin(), query.end(), ' '), query.end());
    {
        // the search, at the tab row's right end: a cream field on the chart
        const float searchW = std::min(230.f * s, (winMax.x - tabMin.x) * 0.3f);
        const float tabsEnd = winMax.x - 30.f * s - searchW - 14.f * s;
        ImGui::SetCursorScreenPos(ImVec2(tabsEnd + 14.f * s, tabMin.y + (tabH - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##search", "Search...", mSearch, sizeof mSearch);
        query = mSearch;
        query.erase(std::remove(query.begin(), query.end(), ' '), query.end());
        // the tabs laid out in rows: those that don't fit on one go on to the next, every tab whole
        const float tabText = 21.f * s, tabGap = 6.f * s, rowGap = 8.f * s;
        std::vector<ImRect> tabRects;
        {
            float tx = tabMin.x, ty = tabMin.y;
            for (const std::string& name : header.order) {
                const float tw = text_width(name, tabText, 1) + 44.f * s;
                if (tx > tabMin.x && tx + tw > tabsEnd) tx = tabMin.x, ty += tabH + rowGap;
                tabRects.emplace_back(ImVec2(tx, ty), ImVec2(tx + tw, ty + tabH));
                tx += tw + tabGap;
            }
        }
        tabBarH = tabRects.empty() ? tabH : tabRects.back().Max.y - tabMin.y;
        for (size_t ti = 0; ti < header.order.size(); ++ti) {
            const std::string& name = header.order[ti];
            const bool selected = name == page && query.empty();
            const ImRect bb = tabRects[ti];
            ImGui::SetCursorScreenPos(bb.Min);
            const bool pressed = ImGui::InvisibleButton(("##tab" + name).c_str(), bb.GetSize());
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const float hotA = ease_to(ImGui::GetItemID(), hot), selA = ease_to(ImGui::GetItemID() + 1, selected);
            plate(dl, bb.Min, bb.Max, kTabPaper, 1.f);
            if (hotA > 0.01f) plate(dl, bb.Min, bb.Max, kPlateFocus, hotA, false);
            if (selA > 0.01f) plate(dl, bb.Min, bb.Max, kPlateChosen, selA, false);
            // the Hylian, faint: the tab's own name
            dl->PushClipRect(ImVec2(bb.Min.x + 5.f * s, bb.Min.y + 5.f * s), ImVec2(bb.Max.x - 5.f * s, bb.Max.y - 5.f * s), true);
            const float hh = tabH * 0.62f;
            hylian(dl, ImVec2(bb.Min.x + 9.f * s, bb.Min.y + (tabH - hh) * 0.5f), hh,
                   selected || hot ? IM_COL32(16, 52, 8, 26) : u32(kInk, 0.07f), name);
            dl->PopClipRect();
            const float g = std::max(hotA, selA);
            const ImVec2 at(bb.Min.x + 22.f * s, bb.Min.y + (tabH - tabText) * 0.5f);
            // ink in the message font on paper, white in the outlined title font on green, crossing over as it turns
            if (g < 0.99f) text(dl, at, tabText, u32(kInk, 1.f - g), name, 0);
            if (g > 0.01f) {
                text(dl, ImVec2(at.x + 1.5f * s, at.y + 1.5f * s), tabText, IM_COL32(30, 20, 8, int(170 * g)), name, 1);
                text(dl, at, tabText, IM_COL32(255, 255, 255, int(255 * g)), name, 1);
            }
            if (pressed) {
                CVarSetString(header.sidebarCVar.c_str(), name.c_str());
                CVarSaveNextFrame();
                mSearch[0] = '\0';
                page = name;
                query.clear();
            }
        }
        // a dotted rule under them (the chart's dotted lines, f_tensen)
        const float ry = tabMin.y + tabBarH + 12.f * s;
        for (float rx = winMin.x + 26.f * s; rx < winMax.x - 26.f * s; rx += 10.f * s)
            dl->AddCircleFilled(ImVec2(rx, ry), 1.8f * s, u32(kParchmentEdge, 0.5f), 6);
    }
    if (page != mPage) mChosen = nullptr, mPage = page;
    if (const std::string key = active + "|" + page + "|" + query; key != mPageKey) mPageKey = key, mPageStart = time;
    const float pageIn = ease_out((time - mPageStart) / 0.24f);
    const float pageDx = (1.f - pageIn) * 28.f * s;

    // ---- the body: two panes, or the whole width for a page that is one tool (the Cosmetics Editor and the like) ----
    const ImVec2 bodyMin(winMin.x + 28.f * s, tabMin.y + tabBarH + 28.f * s);
    const ImVec2 bodyMax(winMax.x - 28.f * s, winMax.y - 26.f * s);
    const float bodyW = bodyMax.x - bodyMin.x, bodyH = bodyMax.y - bodyMin.y;
    ImGui::SetCursorScreenPos(bodyMin);
    mHover = nullptr;
    Sidebar* sidebar = header.sidebars.count(page) ? &header.sidebars[page] : nullptr;
    bool tool = false;
    if (sidebar && query.empty())
        for (auto& col : sidebar->widgets)
            for (auto& w : col)
                if (w.type == WidgetType::Window) tool = true;
    if (tool) {
        ImGui::BeginChild("##tool", ImVec2(bodyW, bodyH), ImGuiChildFlags_None);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const int from = cdl->VtxBuffer.Size;
        for (auto& col : sidebar->widgets)
            for (auto& w : col) DrawWidget(w, bodyW - 16.f * s);
        move_fade(cdl, from, pageDx, 0.f, pageIn);
        ImGui::EndChild();
    } else {
        const float leftW = std::floor(bodyW * 0.56f), rightW = bodyW - leftW - 28.f * s;
        ImGui::BeginChild("##left", ImVec2(leftW, bodyH), ImGuiChildFlags_None);
        // (a scenario's `menu set gTest.MenuScroll=N`: the page scrolled to N points once, for captures of its lower rows)
        static int s_testScroll = 0;
        if (const int scroll = CVarGetInteger("gTest.MenuScroll", 0); scroll != s_testScroll) {
            s_testScroll = scroll;
            ImGui::SetScrollY(float(scroll) * s);
        }
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const int leftFrom = ldl->VtxBuffer.Size;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f * s, 9.f * s));
        // room round the rows for the cursor's corners
        ImGui::Indent(12.f * s);
        ImGui::Dummy(ImVec2(0, 6.f * s));
        const float rowW = leftW - 38.f * s;
        if (!query.empty()) {
            DrawSearchResults(mSearch, rowW);
        } else if (sidebar) {
            for (auto& col : sidebar->widgets)
                for (auto& w : col) {
                    if (IsRow(w)) DrawRow(w, rowW);
                    else {
                        const bool same = w.sameLine;
                        w.sameLine = false;
                        DrawWidget(w, rowW);
                        w.sameLine = same;
                    }
                }
        }
        ImGui::Dummy(ImVec2(0, 8.f * s));
        ImGui::Unindent(12.f * s);
        ImGui::PopStyleVar();
        move_fade(ldl, leftFrom, pageDx, 0.f, pageIn);
        ImGui::EndChild();
        ImGui::SameLine(0, 28.f * s);
        // the right pane: a letter laid on the chart, the Hylian faint on it
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        const ImVec2 rpMax(rp.x + rightW, rp.y + bodyH);
        dl->AddRectFilled(ImVec2(rp.x + 4.f * s, rp.y + 6.f * s), ImVec2(rpMax.x + 4.f * s, rpMax.y + 6.f * s), IM_COL32(60, 35, 10, 40), 6.f * s);
        plate(dl, rp, rpMax, kLetter, 1.f);
        ImGui::SetCursorScreenPos(ImVec2(rp.x + 22.f * s, rp.y + 18.f * s));
        DrawHelpPane(rightW - 40.f * s, bodyH - 34.f * s);
    }
    pop_parchment_style();

    // ---- the confirmation: a message window as the game draws one (its paper, torn edges), plates for the answers ----
    if (mModalOpen) {
        ImGui::OpenPopup(mModal.title.c_str());
        mModalOpen = false;
    }
    push_parchment_style();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.f * s, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36.f * s, 30.f * s));
    if (ImGui::BeginPopupModal(mModal.title.c_str(), nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGuiWindow* mw = ImGui::GetCurrentWindow();
        ImDrawList* mdl = ImGui::GetWindowDrawList();
        mdl->ChannelsSplit(2);
        mdl->ChannelsSetCurrent(1);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float inner = 520.f * s - 72.f * s;
        float used = text_wrapped(mdl, at, 26.f * s, u32(kGreen), mModal.title, inner, 32.f * s) + 6.f * s;
        used += text_wrapped(mdl, ImVec2(at.x, at.y + used), 20.f * s, u32(kInk), mModal.text, inner, 28.f * s);
        ImGui::Dummy(ImVec2(inner, used + 14.f * s));
        const float bw = (inner - 16.f * s) * 0.5f;
        if (plate_button("##yes", mModal.yes, ImVec2(bw, 40.f * s), false)) {
            ImGui::CloseCurrentPopup();
            if (mModal.onYes) mModal.onYes();
        }
        ImGui::SameLine(0, 16.f * s);
        if (plate_button("##no", mModal.no, ImVec2(bw, 40.f * s), false)) ImGui::CloseCurrentPopup();
        mdl->ChannelsSetCurrent(0);
        panel(mdl, mw->Pos, ImVec2(mw->Pos.x + mw->Size.x, mw->Pos.y + mw->Size.y), 16.f * s, kChartPaper);
        mdl->ChannelsMerge();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    pop_parchment_style();
    if (closing) ImGui::EndDisabled();
    ImGui::End();
    ImGui::PopFont();
    io.FontGlobalScale = savedFontScale;
}

} // namespace shipmenu
