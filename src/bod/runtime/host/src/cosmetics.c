// Cosmetics: see cosmetics.h.
#include "cosmetics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { kMaxColors = 128 };

typedef struct CosmeticColor {
    char id[48];
    uint8_t rgba[4];
    bool changed;
} CosmeticColor;

static CosmeticColor g_colors[kMaxColors];
static unsigned g_count;

static CosmeticColor* find(const char* id, bool add) {
    for (unsigned i = 0; i < g_count; ++i)
        if (strcmp(g_colors[i].id, id) == 0)
            return &g_colors[i];
    if (!add || g_count >= kMaxColors || strlen(id) >= sizeof g_colors[0].id)
        return NULL;
    CosmeticColor* c = &g_colors[g_count++];
    snprintf(c->id, sizeof c->id, "%s", id);
    return c;
}

void bluewake_cosmetics_set(const char* id, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool changed) {
    CosmeticColor* c = find(id, true);
    if (c == NULL)
        return;
    c->rgba[0] = r;
    c->rgba[1] = g;
    c->rgba[2] = b;
    c->rgba[3] = a;
    c->changed = changed;
}

bool bluewake_cosmetics_get(const char* id, uint8_t rgba[4]) {
    const CosmeticColor* c = find(id, false);
    if (c == NULL || !c->changed)
        return false;
    memcpy(rgba, c->rgba, 4);
    return true;
}

// Trails: static GXColor data in main.dol that the draw code loads each time
// it draws (d_a_player_particle.inc:292-300, d_a_boomerang.cpp:128). The
// alpha stays the game's (0x96).
typedef struct TrailColor {
    const char* id;
    u32 address;
    u32 vanilla; // RRGGBBAA
} TrailColor;

static const TrailColor kTrails[] = {
    {"Trails.Sword", 0x803F62ACu, 0xFFFFFF96u},       // n_color0: a normal slash
    {"Trails.SwordSoup", 0x803F62B0u, 0xFFFF7B96u},   // y_color0: after Elixir Soup
    {"Trails.SwordParry", 0x803F62B4u, 0x5AFF5A96u},  // g_color0: a parry
    {"Trails.Boomerang", 0x803F6268u, 0xFFFF7B96u},   // the Boomerang's trail
};

// Spin attack effects: JPA emitters the player actor owns (daPy_lk_c m32E4 / m32F0, d_a_player_sword.inc).
// The game makes them with no colour of its own, so the emitter's global prm/env colour (JPABaseEmitter
// +0x1FC / +0x200) is the host's to set. Charging (procCutTurnMove, 0x59): CHARGEPOWER00 and 01. The
// spin itself (procCutTurn, 0x55): ROUNDATTACKTOE (m32E4; m32F0 is grass or sea spray, left alone).
enum {
    kPlayerPointer = 0x803CA754u, // play.mpPlayerPtr[0]
    kPlayerProc = 0x31D8u,        // daPy_lk_c::mCurProc
    kEmitterA = 0x32E4u + 4u,     // m32E4.mpEmitter
    kEmitterB = 0x32F0u + 4u,     // m32F0.mpEmitter
    kProcCutTurn = 0x55u,
    kProcCutTurnMove = 0x59u,
    kEmitterPrm = 0x1FCu,
    kEmitterEnv = 0x200u,
};

// The spin's big swirl (ROUNDATTACK00/01/02, d_a_player_main.cpp setP1 with static GXColors in .sdata2): the game hands the
// emitters pointers to these, so the host rewrites the data. Each of the three sword variants has a dark prm0, a light prm1
// and an env colour; the chosen colour is scaled to the same three levels (percent).
typedef struct SpinSwirl {
    u32 address;
    u32 vanilla; // RRGGBBAA
    u32 percent;
} SpinSwirl;

static const SpinSwirl kSpinSwirl[] = {
    {0x803FA544u, 0x406040FFu, 30u}, {0x803FA548u, 0xDCE6DCFFu, 90u}, {0x803FA54Cu, 0xA0E6A0FFu, 65u}, // n_turn: prm0, prm1, env
    {0x803FA550u, 0x404060FFu, 30u}, {0x803FA554u, 0xDCDCE6FFu, 90u}, {0x803FA558u, 0xA0A0E6FFu, 65u}, // nm_turn
    {0x803FA55Cu, 0x4040FFFFu, 30u}, {0x803FA560u, 0xDCDCFFFFu, 90u}, {0x803FA564u, 0x7878FFFFu, 65u}, // fm_turn
};

static bool guest_pointer(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

static void paint_emitter(CPUState* cpu, u32 emitter, const uint8_t rgba[4], bool env_dim) {
    if (!guest_pointer(emitter))
        return;
    u32 prm = ((u32)rgba[0] << 24) | ((u32)rgba[1] << 16) | ((u32)rgba[2] << 8) | (mem_read32(cpu, emitter + kEmitterPrm) & 0xFFu);
    u32 env = env_dim ? (((u32)(rgba[0] * 3u / 5u) << 24) | ((u32)(rgba[1] * 3u / 5u) << 16) | ((u32)(rgba[2] * 3u / 5u) << 8) |
                         (mem_read32(cpu, emitter + kEmitterEnv) & 0xFFu))
                      : prm;
    if (mem_read32(cpu, emitter + kEmitterPrm) != prm)
        mem_write32(cpu, emitter + kEmitterPrm, prm);
    if (mem_read32(cpu, emitter + kEmitterEnv) != env)
        mem_write32(cpu, emitter + kEmitterEnv, env);
}

static void spin_effects(CPUState* cpu) {
    const u32 player = mem_read32(cpu, kPlayerPointer);
    if (!guest_pointer(player))
        return;
    const u32 proc = mem_read32(cpu, player + kPlayerProc);
    uint8_t rgba[4];
    if (proc == kProcCutTurnMove && bluewake_cosmetics_get("Trails.SpinCharge", rgba)) {
        paint_emitter(cpu, mem_read32(cpu, player + kEmitterA), rgba, true);
        paint_emitter(cpu, mem_read32(cpu, player + kEmitterB), rgba, true);
    } else if (proc == kProcCutTurn && bluewake_cosmetics_get("Trails.SpinAttack", rgba)) {
        paint_emitter(cpu, mem_read32(cpu, player + kEmitterA), rgba, true);
    }
}

void bluewake_cosmetics_retrace(CPUState* cpu) {
    if (cpu == NULL || cpu->ram == NULL)
        return;
    spin_effects(cpu);
    {
        uint8_t rgba[4];
        const bool on = bluewake_cosmetics_get("Trails.SpinAttack", rgba);
        for (unsigned i = 0; i < sizeof kSpinSwirl / sizeof kSpinSwirl[0]; ++i) {
            u32 value = kSpinSwirl[i].vanilla;
            if (on)
                value = ((u32)(rgba[0] * kSpinSwirl[i].percent / 100u) << 24) | ((u32)(rgba[1] * kSpinSwirl[i].percent / 100u) << 16) |
                        ((u32)(rgba[2] * kSpinSwirl[i].percent / 100u) << 8) | 0xFFu;
            if (mem_read32(cpu, kSpinSwirl[i].address) != value)
                mem_write32(cpu, kSpinSwirl[i].address, value);
        }
    }
    for (unsigned i = 0; i < sizeof kTrails / sizeof kTrails[0]; ++i) {
        uint8_t rgba[4];
        u32 value = kTrails[i].vanilla;
        if (bluewake_cosmetics_get(kTrails[i].id, rgba))
            value = ((u32)rgba[0] << 24) | ((u32)rgba[1] << 16) | ((u32)rgba[2] << 8) | (kTrails[i].vanilla & 0xFFu);
        if (mem_read32(cpu, kTrails[i].address) != value)
            mem_write32(cpu, kTrails[i].address, value);
    }
}

// HUD buttons: J2DPicture panes of the meter screen (found with the pane log,
// BLUEWAKE_WS_PANE_LOG). A picture draws its texture's intensity between
// mColorBlack (+0x108) and mColorWhite (+0x104). The chosen color replaces
// the base color as wwrando's color exchange does: the new hue, saturation
// and value moved by the chosen color's difference from the base. A white
// tint goes halfway to the chosen color, so a pale button (X, Y) reads as it.
// The A button's base is its disc's color (the disc itself is a texture,
// windows/src/hud_colors.cpp; this pane is its ring).
typedef struct HudPane {
    u32 tag;
    const char* id;
    u32 white, black; // the game's, 0xRRGGBB
    u32 base;         // the color the editor shows as the game's
} HudPane;

static const HudPane kHudPanes[] = {
    {0x62617432u, "Hud.AButton", 0xA3FFDBu, 0x3C640Au, 0x35C39Bu}, // bat2
    {0x62303031u, "Hud.XButton", 0xFFFFFFu, 0x6E5082u, 0x6E5082u}, // b001
    {0x62303032u, "Hud.YButton", 0xFFFFFFu, 0x6E5082u, 0x6E5082u}, // b002
    {0x62613072u, "Hud.RButton", 0xFFFFFFu, 0x323232u, 0x323232u}, // ba0r
};

typedef struct Hsv {
    float h, s, v; // 0-360, 0-100, 0-100
} Hsv;

static Hsv to_hsv(u32 rgb) {
    const float r = (float)(rgb >> 16 & 255u) / 255.f, g = (float)(rgb >> 8 & 255u) / 255.f, b = (float)(rgb & 255u) / 255.f;
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    float h = 0.f;
    if (d > 0.f) {
        if (mx == r)
            h = fmodf((g - b) / d, 6.f);
        else if (mx == g)
            h = (b - r) / d + 2.f;
        else
            h = (r - g) / d + 4.f;
        h *= 60.f;
        if (h < 0.f)
            h += 360.f;
    }
    Hsv out = {h, mx > 0.f ? d / mx * 100.f : 0.f, mx * 100.f};
    return out;
}

static float clamp100(float x) { return x < 0.f ? 0.f : x > 100.f ? 100.f : x; }

static u32 from_hsv(Hsv c) {
    const float s = clamp100(c.s) / 100.f, v = clamp100(c.v) / 100.f;
    const float cc = v * s, x = cc * (1.f - fabsf(fmodf(c.h / 60.f, 2.f) - 1.f)), m = v - cc;
    float r = 0.f, g = 0.f, b = 0.f;
    switch ((int)(c.h / 60.f) % 6) {
    case 0: r = cc; g = x; break;
    case 1: r = x; g = cc; break;
    case 2: g = cc; b = x; break;
    case 3: g = x; b = cc; break;
    case 4: r = x; b = cc; break;
    default: r = cc; b = x; break;
    }
    return (u32)lroundf((r + m) * 255.f) << 16 | (u32)lroundf((g + m) * 255.f) << 8 | (u32)lroundf((b + m) * 255.f);
}

static u32 exchange(u32 rgb, u32 base, u32 chosen) {
    if (rgb == 0xFFFFFFu) {
        u32 out = 0;
        for (int shift = 0; shift < 24; shift += 8)
            out |= ((255u + (chosen >> shift & 255u) + 1u) / 2u) << shift;
        return out;
    }
    const Hsv b = to_hsv(base), n = to_hsv(chosen);
    Hsv c = to_hsv(rgb);
    if (c.s == 0.f)
        c.s = b.s;
    c.h = n.h;
    c.s += n.s - b.s;
    c.v += n.v - b.v;
    return from_hsv(c);
}

// Whether a color the game set is the button's (or the button's dimmed):
// near its hue and saturation, so another screen's pane with the same tag
// keeps its own.
static bool like(u32 rgb, u32 vanilla) {
    const Hsv a = to_hsv(rgb), b = to_hsv(vanilla);
    if (b.s < 8.f)
        return a.s < 8.f;
    const float dh = fabsf(a.h - b.h);
    return fminf(dh, 360.f - dh) < 20.f && fabsf(a.s - b.s) < 25.f;
}

// What we wrote last, per pane, so a color the game left alone is not
// exchanged twice (the game sets some tints once, others every frame).
typedef struct PaneMemo {
    u32 pane;
    u32 game[2], written[2]; // white, black (RRGGBBAA)
} PaneMemo;
static PaneMemo g_memo[16];
static unsigned g_memo_next;

static PaneMemo* memo_for(u32 pane) {
    for (unsigned i = 0; i < 16u; ++i)
        if (g_memo[i].pane == pane)
            return &g_memo[i];
    PaneMemo* m = &g_memo[g_memo_next++ % 16u];
    memset(m, 0, sizeof *m);
    m->pane = pane;
    return m;
}

void bluewake_cosmetics_pane(CPUState* cpu, u32 pane) {
    const u32 tag = mem_read32(cpu, pane + 0x8u);
    const HudPane* hp = NULL;
    for (unsigned i = 0; i < sizeof kHudPanes / sizeof kHudPanes[0] && hp == NULL; ++i)
        if (kHudPanes[i].tag == tag)
            hp = &kHudPanes[i];
    if (hp == NULL)
        return;
    uint8_t rgba[4];
    const bool on = bluewake_cosmetics_get(hp->id, rgba);
    PaneMemo* m = memo_for(pane);
    const u32 chosen = (u32)rgba[0] << 16 | (u32)rgba[1] << 8 | rgba[2];
    for (int k = 0; k < 2; ++k) {
        const u32 at = pane + (k == 0 ? 0x104u : 0x108u);
        const u32 now = mem_read32(cpu, at);
        if (now != m->written[k] || m->written[k] == 0u)
            m->game[k] = now; // the game set it (or this pane is new to us)
        const u32 game = m->game[k];
        u32 value = game;
        const u32 vanilla = k == 0 ? hp->white : hp->black;
        if (on && like(game >> 8, vanilla))
            value = exchange(game >> 8, hp->base, chosen) << 8 | (game & 0xFFu);
        if (value != now)
            mem_write32(cpu, at, value);
        m->written[k] = value;
    }
}
