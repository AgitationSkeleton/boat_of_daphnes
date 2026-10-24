// Reveal Sea Chart: see chart_reveal.h.
#include "chart_reveal.h"

#include "guest_coro.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DRAW_LIST = 0x803C4C08u + 0x5D1Cu, // g_dComIfG_gameInfo.drawlist (dDlst_list_c)
    DRAWLIST_2DOPA = 0x19Cu,           // dDlst_list_c::mp2DOpa and mp2DOpaEnd (puppet.c)
    CHART_DLST_VT = 0x80392594u,       // __vt__12dDlst_FMAP_c

    // dMenu_Fmap_c (d_menu_fmap.h)
    FMAP_DLST = 0x1Cu,        // fmapDl (dDlst_FMAP_c: scrn at +4)
    FMAP_SV = 0x2878u,        // fmapSv (zoom +1)
    FMAP_TSW1 = 0x2F54u,      // fopMsgM_pane_class (0x38 bytes each; the J2DPane* is first)
    FMAP_R01B = 0x2F8Cu,
    FMAP_KTX1 = 0x2FFCu,
    FMAP_KTX2 = 0x3034u,
    FMAP_CLB2 = 0x306Cu,
    FMAP_R0X = 0x39D4u,       // mR0xPanes[11]: the islands
    FMAP_CHKPNT = 0x5111u,    // s8 mCurChkPntNo: the island of the square under the cursor, -1 none
    FMAP_MAIN_PROC = 0x5113u, // 0: the chart (1: the comparison screen)
    FMAP_BTN_ICON = 0x5181u,  // mButtonIconMode
    FMAP_MODE = 0x516Bu,      // mFmapMode: 0 normal, 1 warp (Ballad of Gales), 2 wallpaper, 3 fishman
    FMAP_AREA_BUF = 0x5140u,  // mAreaTxtBufIdx
    FMAP_AREA_CHANGING = 0x5142u, // mAreaTxtChanging
    FMAP_TXT_NAME = 0x5148u,  // char* mTxtName[3]: the island-name text of the cursor (the panes' own strings)
    SV_CUR_X = 4u,            // dMenu_FmapSv_c: curX, curY (the sea chart cursor), curWX, curWY (the warp cursor)
    SV_CUR_WX = 6u,
    FN_MESSAGE_GET = 0x8002BB78u, // fopMsgM_messageGet(char* dest, u32 msgNo)
    MSG_ISLAND_FIRST = 0x31A6u,   // the message of the chart square `grid`: 0x31A6 + grid
    PANE_SIZE = 0x38u,
    ISLANDS = 11,
    SQUARES = 49,
    BTN_SECTOR = 1,
    BTN_LOCKED = 3,

    // J2DPane
    J2D_TAG = 0x08u,
    J2D_VISIBLE = 0xAAu,
    J2D_TREE = 0xB0u,         // JSUTree: the first child's link at +0; a pane's own link is at +0xBC (object, list, prev, next)
    J2D_LINK_NEXT = 0x0Cu,    // link + 0xC
    TREE_MAX = 2048,

    // slots: the squares' fog, the islands, then the zoom panes and the button mode
    S_FOG = 0,
    S_ISLAND = S_FOG + SQUARES,
    S_TSW1 = S_ISLAND + ISLANDS,
    S_R01B,
    S_KTX1,
    S_KTX2,
    S_CLB2,
    S_BTN,
    SLOTS,
};

static atomic_bool g_on;
void bluewake_chart_reveal_enable(bool on) { atomic_store(&g_on, on); }
bool bluewake_chart_reveal_enabled(void) { return atomic_load(&g_on); }

static bool guest_ptr(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

// What the pass knows of the chart it is working on (reset when the chart is another object).
typedef struct Slot {
    u32 addr;      // where the byte is (a pane's visible flag, or the button mode); 0: none
    bool engaged;  // `mine` was written
    uint8_t orig;  // the game's own value when last seen
    uint8_t mine;  // what the pass wrote
} Slot;
static Slot g_slot[SLOTS];
static u32 g_self;
static bool g_engaged;

static void slots_reset(void) {
    memset(g_slot, 0, sizeof g_slot);
    g_self = 0u;
    g_engaged = false;
}

// The drawn chart's dMenu_Fmap_c, or 0.
static u32 find_chart(CPUState* cpu, u32* screen) {
    const u32 list = DRAW_LIST;
    const u32 top = mem_read32(cpu, list + DRAWLIST_2DOPA);
    const u32 end = mem_read32(cpu, list + DRAWLIST_2DOPA + 4u);
    const u32 first = list + 0x9Cu;
    if (!guest_ptr(top) || top < first || top + 4u > end)
        return 0u;
    for (u32 p = first; p < top; p += 4u) {
        const u32 o = mem_read32(cpu, p);
        if (guest_ptr(o) && mem_read32(cpu, o) == CHART_DLST_VT) {
            *screen = mem_read32(cpu, o + 4u);
            return guest_ptr(*screen) ? o - FMAP_DLST : 0u;
        }
    }
    return 0u;
}

// The squares' fog panes ('mR01'..'mR31') are found once by walking the screen's tree.
static void locate_panes(CPUState* cpu, u32 self, u32 screen) {
    slots_reset();
    g_self = self;
    u32 stack[TREE_MAX];
    int sp = 0, visited = 0;
    stack[sp++] = screen;
    while (sp > 0 && visited++ < TREE_MAX) {
        const u32 pane = stack[--sp];
        const u32 tag = mem_read32(cpu, pane + J2D_TAG);
        if ((tag >> 24) == 'm' && ((tag >> 16) & 0xFFu) == 'R') {
            char hex[3] = {(char)((tag >> 8) & 0xFFu), (char)(tag & 0xFFu), 0};
            char* endp = NULL;
            const long n = strtol(hex, &endp, 16);
            if (endp == hex + 2 && n >= 1 && n <= SQUARES && g_slot[S_FOG + n - 1].addr == 0u)
                g_slot[S_FOG + n - 1].addr = pane + J2D_VISIBLE;
        }
        for (u32 link = mem_read32(cpu, pane + J2D_TREE); guest_ptr(link) && sp < TREE_MAX - 1; link = mem_read32(cpu, link + J2D_LINK_NEXT)) {
            const u32 child = mem_read32(cpu, link);
            if (guest_ptr(child))
                stack[sp++] = child;
        }
    }
    for (int i = 0; i < ISLANDS; ++i) {
        const u32 pane = mem_read32(cpu, self + FMAP_R0X + PANE_SIZE * (u32)i);
        if (guest_ptr(pane))
            g_slot[S_ISLAND + i].addr = pane + J2D_VISIBLE;
    }
    static const struct {
        int slot;
        u32 at;
    } zoom[] = {
        {S_TSW1, FMAP_TSW1}, {S_R01B, FMAP_R01B}, {S_KTX1, FMAP_KTX1}, {S_KTX2, FMAP_KTX2}, {S_CLB2, FMAP_CLB2},
    };
    for (size_t i = 0; i < sizeof zoom / sizeof zoom[0]; ++i) {
        const u32 pane = mem_read32(cpu, self + zoom[i].at);
        if (guest_ptr(pane))
            g_slot[zoom[i].slot].addr = pane + J2D_VISIBLE;
    }
    g_slot[S_BTN].addr = self + FMAP_BTN_ICON;
    static bool said;
    if (!said) {
        said = true;
        int fog = 0;
        for (int i = 0; i < SQUARES; ++i)
            fog += g_slot[S_FOG + i].addr != 0u;
        fprintf(stderr, "[chart] Reveal Sea Chart: %d of %d fog panes found\n", fog, SQUARES);
    }
}

// Put `value` in the slot's byte; remember the game's own value there. The game sets these once (when the chart or a zoom
// opens), so a value that is not the one written last is the game's new one.
static void slot_force(CPUState* cpu, int i, uint8_t value) {
    Slot* s = &g_slot[i];
    if (s->addr == 0u)
        return;
    const uint8_t now = (uint8_t)mem_read8(cpu, s->addr);
    if (!s->engaged || now != s->mine)
        s->orig = now;
    if (now != value)
        mem_write8(cpu, s->addr, value);
    s->mine = value;
    s->engaged = true;
}

static void slot_release(CPUState* cpu, int i) {
    Slot* s = &g_slot[i];
    if (s->addr == 0u || !s->engaged)
        return;
    if ((uint8_t)mem_read8(cpu, s->addr) == s->mine)
        mem_write8(cpu, s->addr, s->orig);
    s->engaged = false;
}

static void release_all(CPUState* cpu) {
    for (int i = 0; i < SLOTS; ++i)
        slot_release(cpu, i);
    g_engaged = false;
}

// The island-name text. The game empties it for a square the save has not recorded as arrived at (changeIslandName,
// areaTextChangeAnimeInit); while the reveal is on the same message is put in, with the game's own message call. The
// save is not read or written. The name's colour stays the plain one.
static GuestCoro* g_coro_name;
static u32 g_name_buf, g_name_msg; // set by the pass, used by the fiber
static u32 g_name_key;             // the last (buffer, message) tried that left the text empty: not tried again until it fills or the cursor moves

static void name_pass(CPUState* cpu, u32 self, unsigned zoom) {
    g_name_buf = 0u;
    if (zoom != 0u)
        return; // the zoomed views name their island without the save
    const unsigned mode = mem_read8(cpu, self + FMAP_MODE);
    const u32 sv = mem_read32(cpu, self + FMAP_SV);
    if (!guest_ptr(sv) || mode > 2u)
        return;
    int buf;
    s8 gx, gy;
    if (mode == 1u) {
        buf = 1; // areaTextChangeAnimeInit always writes buffer 1
        gx = (s8)mem_read8(cpu, sv + SV_CUR_WX);
        gy = (s8)mem_read8(cpu, sv + SV_CUR_WX + 1u);
    } else {
        const int idx = mem_read8(cpu, self + FMAP_AREA_BUF) & 1;
        buf = mem_read8(cpu, self + FMAP_AREA_CHANGING) != 0u ? idx ^ 1 : idx; // changeIslandName(idx ^ 1) while changing
        gx = (s8)mem_read8(cpu, sv + SV_CUR_X);
        gy = (s8)mem_read8(cpu, sv + SV_CUR_X + 1u);
    }
    const int grid = gx + (gy + 3) * 7 + 3;
    if (gx < -3 || gx > 3 || gy < -3 || gy > 3 || grid < 0 || grid >= SQUARES)
        return;
    const u32 text = mem_read32(cpu, self + FMAP_TXT_NAME + 4u * (u32)buf);
    if (!guest_ptr(text))
        return;
    if (mem_read8(cpu, text) != 0u) {
        g_name_key = 0u; // there is a name: when the game empties the text again (the chart opened again, a cursor move) it is put in again
        return;
    }
    const u32 msg = MSG_ISLAND_FIRST + (u32)grid;
    const u32 key = text ^ (msg << 24) ^ 1u;
    if (key == g_name_key)
        return;
    g_name_key = key;
    g_name_buf = text;
    g_name_msg = msg;
}

static void name_body(GuestCoro* co, void* arg) {
    (void)arg;
    bluewake_guest_call(co, FN_MESSAGE_GET, g_name_buf, g_name_msg, 0, 0, 0, 0);
    fprintf(stderr, "[chart] Reveal Sea Chart: island name for message %04X put in\n", g_name_msg);
}

static void pass_panes(CPUState* cpu);

bool bluewake_chart_reveal_active(void) { return g_coro_name != NULL && bluewake_coro_active(g_coro_name); }

u32 bluewake_chart_reveal_hook(CPUState* cpu, u32 site) {
    if (g_coro_name == NULL)
        g_coro_name = bluewake_coro_create(name_body);
    if (bluewake_coro_active(g_coro_name))
        return bluewake_coro_step(g_coro_name, cpu, site, NULL);
    g_name_buf = 0u;
    pass_panes(cpu);
    if (g_name_buf == 0u)
        return 0u;
    return bluewake_coro_step(g_coro_name, cpu, site, NULL);
}

static void pass_panes(CPUState* cpu) {
    const bool on = atomic_load(&g_on);
    if (!on && !g_engaged)
        return;
    u32 screen = 0u;
    const u32 self = find_chart(cpu, &screen);
    static unsigned dbg;
    if (getenv("BLUEWAKE_CHART_REVEAL_LOG") != NULL && ++dbg % 60 == 1)
        fprintf(stderr, "[chart] pass: self=%08X screen=%08X on=%d\n", self, screen, (int)on);
    if (self == 0u) {
        slots_reset(); // the chart is gone: nothing of it is left to put back
        return;
    }
    if (mem_read8(cpu, self + FMAP_MAIN_PROC) != 0u) {
        if (g_engaged)
            release_all(cpu);
        return;
    }
    if (self != g_self)
        locate_panes(cpu, self, screen);
    if (!on) {
        release_all(cpu);
        return;
    }
    g_engaged = true;
    const u32 sv = mem_read32(cpu, self + FMAP_SV);
    const unsigned zoom = guest_ptr(sv) ? mem_read8(cpu, sv + 1u) : 0u;
    name_pass(cpu, self, zoom);
    // The whole sea: no fog, every island.
    for (int i = 0; i < SQUARES; ++i)
        slot_force(cpu, S_FOG + i, 0);
    for (int i = 0; i < ISLANDS; ++i)
        slot_force(cpu, S_ISLAND + i, 1);
    // A square on show (zoom 1 and 2): the island's picture, no "unexplored" cover or text, and A zooms in.
    if (zoom >= 1u) {
        if ((s8)mem_read8(cpu, self + FMAP_CHKPNT) != -1) {
            slot_force(cpu, S_TSW1, 1);
            slot_force(cpu, S_R01B, 1);
        }
        slot_force(cpu, S_KTX1, 0);
        slot_force(cpu, S_KTX2, 0);
        slot_force(cpu, S_CLB2, 0);
        if (mem_read8(cpu, g_slot[S_BTN].addr) == BTN_LOCKED)
            slot_force(cpu, S_BTN, BTN_SECTOR);
    }
}
