// Picking up other players: see puppet_carry.h.
#include "puppet_carry.h"

#include "gxruntime/platform.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
    PLAYER = 0x803CA754u,       // play.mpPlayerPtr[0]
    PLAYER_PROC = 0x31D8u,      // daPy_lk_c::mCurProc
    STAY_ROOM = 0x803F6A78u,
    EVENT_MODE = 0x803C9EA2u,   // play.mEvtCtrl.mMode: 0 when no event runs
    NEXT_STAGE_GO = 0x803C9D48u + 0xCu,
    SAVE_LIFE = 0x803C4C08u + 2u,
    // g_mDoCPd_cpadInfo[0]: sticks, c-stick and analogs in the first 0x30 bytes; the buttons held (u16) at +0x30, pressed at +0x32.
    PAD = 0x803A4DF0u,
    PAD_HOLD = 0x30u, PAD_TRIG = 0x32u, PAD_R_ANALOG = 0x2Cu, PAD_HOLD_LOCK_R = 0x37u, PAD_TRIG_LOCK_R = 0x38u,
    // The words are controller_pad_buttons' bitfields, most significant first: left 0x8000, right, down, up, z, r 0x0400, l, a 0x0100,
    // b 0x0080, x, y, start 0x0010 (a scripted B press showed 0x0080). Link's R is the pad's hold-lock byte below.
    BTN_START = 0x0010u, BTN_B = 0x0080u,

    // fopAc_ac_c
    AC_STATUS = 0x1C4u, AC_OLD = 0x1E4u, AC_CURRENT = 0x1F8u, AC_CUR_ANGLE_Y = 0x206u, AC_SHAPE_Y = 0x20Eu, AC_SPEED = 0x220u,
    // daPy_lk_c::execute starts with current.pos = l_debug_keep_pos, shape_angle = l_debug_shape_angle, current.angle =
    // l_debug_current_angle (copies made at the end of its last execute), so a place is set in those too.
    KEEP_POS = 0x803E440Cu, KEEP_CUR_ANGLE = 0x803F6F10u, KEEP_SHAPE_ANGLE = 0x803F6F18u,
    // daPy_lk_c::mAcch (dBgS_LinkAcch) at +0x46C, its m_flags at +0x28: GROUND_HIT (0x20). With it clear and the ground more than
    // 30 units below, changeAutoJumpProc (every frame) puts Link into his fall: the held Link is kept "on the ground".
    PLAYER_ACCH_FLAGS = 0x46Cu + 0x28u, ACCH_GROUND_HIT = 0x20u,
    AC_SPEED_F = 0x254u, AC_GRAVITY = 0x258u, AC_ATTN_DIST = 0x26Cu,
    STTS_CARRY = 0x00002000u, STTS_HEAVY = 0x00010000u | 0x08000000u,
    ATTN_ACTION_CARRY = 0x00000010u,
    ATTN_TYPE_CARRY = 4,
    ATTN_DIST_CARRY = 9, // the barrel's: 100 units around, in front

    PROC_CROUCH = 0x0E,
    FN_PROC_CROUCH_INIT = 0x801143D4u, // daPy_lk_c::procCrouch_init
    FN_FREE_GRAB_ITEM = 0x8013AF84u,   // daPy_lk_c::freeGrabItem

    MAX_PEERS = 16,
    QUEUE = 48,
    HOLD_DROP = 0,             // the held player's feet below the carrier's hands (the held thing's origin is a hand's height above Link's head: at 62 the
                               // body overlapped the carrier's, BR-0036)
    POS_EVERY = 2,             // frames between CARRY_POS while held
    TIMEOUT_MS = 3000,
    HOLD_MAX_MS = 180000,
    THROW_MIN_SPEED = 2,       // a release with this speed or more is a throw
    FLIGHT_MAX = 120,
    // Against the stage (the game's own ground check, dBgS_ObjGndChk_Func, run from the draw fiber): an arc is walked in steps of this
    // many units, each point's ground read from that far above it; a rise of more than STEP_RISE above the point is a wall or a pillar
    // the arc stops before.
    FN_NEW = 0x802B0C38u,      // operator new(size)
    FN_GND_HEIGHT = 0x800A4564u, // dBgS_ObjGndChk_Func(cXyz&): the ground height below a point (f1), -1e9 for none
    FN_NEW_ALIGNED = 0x802B0C60u,   // operator new(size, align)
    FN_LINCHK_CT = 0x800F0078u,     // dBgS_LinChk::dBgS_LinChk()
    FN_LINCHK_SET = 0x800A5678u,    // dBgS_LinChk::Set(cXyz* start, cXyz* end, fopAc_ac_c* actor)
    FN_BGS_LINE_CROSS = 0x8024676Cu, // cBgS::LineCross(cBgS_LinChk*), on the play's dBgS
    BGS_INSTANCE = 0x803C5EA8u,     // g_dComIfG_gameInfo.play.mBgS (dComIfG_Bgsp())
    LINCHK_END = 0x30u,             // cBgS_LinChk::mLin's end (cM3dGLin at +0x24): the crossing after a hit
    SCRATCH_SIZE = 0x120,
    SCRATCH_CHK = 0x40,             // the line check object (0x6C bytes) in the scratch memory
    WALL_BACKOFF = 12,              // an arc that meets a wall ends this far short of it
    TRACE_STEP = 12,
    TRACE_MAX_STEPS = 40,
    TRACE_LOOK_UP = 50,
    STEP_RISE = 35,
    SETTLE_FRAMES = 4,
};

typedef struct Req {
    int kind; // 1 start, 2 pos, 3 end, 4 gone, 5 reset
    int from, angle, phase, reason, hurt;
    float pos[3];
} Req;

typedef struct Peer {
    int id;
    int state;          // 0 none, 1 held by me, 2 flying
    float feet[3];      // where the held player's feet are
    float vel[3];
    float gravity, floor_y;
    int angle, frames, since_pos;
    bool blocked_said;
} Peer;

static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static bool g_allowed;
static Req g_req[QUEUE];
static int g_req_n;
static BluewakeCarryEvent g_ev[QUEUE];
static int g_ev_n;
static Peer g_peer[MAX_PEERS];
static uint32_t g_scene;
static uint32_t g_frame;
static bool g_force_release;   // the draw fiber lets go of what Link holds
static int g_hurt_from;
static float g_hurt_pos[3];
static bool g_hurt_pending;

// I am carried.
static struct {
    bool active;
    int carrier;
    float target[3], cur[3];
    int angle, phase;
    double last_ms, start_ms;
    int room;
    uint32_t frames;
    // A throw flown against the stage: the last point known to be free.
    float valid[3];
} g_held;
static int g_settle;       // frames left to check that Link let go after a throw is not inside the ground

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
    }
}
static void unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    return 0.0;
#endif
}

extern u32 bluewake_puppet_mem2_end, bluewake_puppet_mem1_end; // puppet.c
static bool guest_ptr(u32 p) {
    return ((p >= 0x80000000u && p < bluewake_puppet_mem1_end) || (p >= 0x90000000u && p < bluewake_puppet_mem2_end)) && (p & 3u) == 0u;
}

static float rd_f(CPUState* cpu, u32 address) {
    const u32 bits = mem_read32(cpu, address);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
static void wr_f(CPUState* cpu, u32 address, float f) {
    u32 bits;
    memcpy(&bits, &f, 4);
    mem_write32(cpu, address, bits);
}
static void wr_vec(CPUState* cpu, u32 address, const float v[3]) {
    for (int i = 0; i < 3; ++i)
        wr_f(cpu, address + 4u * (u32)i, v[i]);
}
static void rd_vec(CPUState* cpu, u32 address, float v[3]) {
    for (int i = 0; i < 3; ++i)
        v[i] = rd_f(cpu, address + 4u * (u32)i);
}

// ------------------------------------------------------------- the stage ----

static bool g_trace_known; // the last point walked had ground below it
static u32 g_scratch;      // guest memory for the checks (from operator new, kept): points, then the line check object

// The ground height below (x, y, z), -1e9 for none: the game's own check.
static float ground_at(GuestCoro* co, CPUState* cpu, float x, float y, float z) {
    if (g_scratch == 0u) {
        const u32 block = bluewake_guest_call(co, FN_NEW_ALIGNED, SCRATCH_SIZE, 0x20u, 0, 0, 0, 0);
        if (!guest_ptr(block))
            return -1.0e9f;
        g_scratch = block;
        bluewake_guest_call(co, FN_LINCHK_CT, g_scratch + SCRATCH_CHK, 0, 0, 0, 0, 0); // dBgS_LinChk::dBgS_LinChk(), made once
    }
    wr_f(cpu, g_scratch, x);
    wr_f(cpu, g_scratch + 4u, y);
    wr_f(cpu, g_scratch + 8u, z);
    bluewake_guest_call(co, FN_GND_HEIGHT, g_scratch, 0, 0, 0, 0, 0);
    return (float)cpu->fpr[1];
}

// Whether the segment crosses the stage's polygons (a wall, a floor, a ceiling: the game's line check, dBgS_LinChk through cBgS::LineCross),
// and where (the check's line end is moved to the crossing).
static bool line_cross(GuestCoro* co, CPUState* cpu, const float a[3], const float b[3], float at[3]) {
    if (g_scratch == 0u)
        return false;
    wr_vec(cpu, g_scratch + 0x10u, a);
    wr_vec(cpu, g_scratch + 0x20u, b);
    const u32 chk = g_scratch + SCRATCH_CHK;
    bluewake_guest_call(co, FN_LINCHK_SET, chk, g_scratch + 0x10u, g_scratch + 0x20u, 0u, 0, 0);
    const bool hit = (bluewake_guest_call(co, FN_BGS_LINE_CROSS, BGS_INSTANCE, chk, 0, 0, 0, 0) & 0xFFu) != 0u;
    if (hit)
        rd_vec(cpu, chk + LINCHK_END, at);
    return hit;
}

// BLUEWAKE_CARRY_NOSTAGE=1 (testing): arcs know no stage, as before (the thrower's floor is where one lands).
static bool nostage(void) {
    static int off = -1;
    if (off < 0)
        off = getenv("BLUEWAKE_CARRY_NOSTAGE") != NULL;
    return off != 0;
}

// Walks an arc's segment from `from` to `to` against the stage. 0: free (out = to); 1: stopped before a wall or pillar (out = a place a
// little short of it); 2: reached the ground (out = the point on it).
static int trace_arc(GuestCoro* co, CPUState* cpu, const float from[3], const float to[3], float out[3]) {
    if (nostage()) {
        g_trace_known = false;
        memcpy(out, to, 3 * sizeof(float));
        return 0;
    }
    float d[3], len = 0.0f;
    for (int i = 0; i < 3; ++i) {
        d[i] = to[i] - from[i];
        len += d[i] * d[i];
    }
    int steps = (int)ceilf(sqrtf(len) / (float)TRACE_STEP);
    steps = steps < 1 ? 1 : steps > TRACE_MAX_STEPS ? TRACE_MAX_STEPS : steps;
    float prev[3] = {from[0], from[1], from[2]};
    for (int s = 1; s <= steps; ++s) {
        const float t = (float)s / (float)steps;
        const float p[3] = {from[0] + d[0] * t, from[1] + d[1] * t, from[2] + d[2] * t};
        float g = ground_at(co, cpu, p[0], p[1] + (float)TRACE_LOOK_UP, p[2]); // (first: makes the scratch memory the line check needs)
        g_trace_known = g > -1.0e8f;
        float cross[3];
        if (line_cross(co, cpu, prev, p, cross)) {
            // A floor under the arc is the landing, anything else a wall or a ceiling: the arc ends a little short of it.
            const float cg = ground_at(co, cpu, cross[0], cross[1] + (float)TRACE_LOOK_UP, cross[2]);
            if (cg > -1.0e8f && cross[1] <= cg + (float)STEP_RISE / 3.0f && d[1] <= 0.0f) {
                out[0] = cross[0];
                out[1] = cg;
                out[2] = cross[2];
                return 2;
            }
            const float seg = sqrtf((p[0] - prev[0]) * (p[0] - prev[0]) + (p[1] - prev[1]) * (p[1] - prev[1]) + (p[2] - prev[2]) * (p[2] - prev[2]));
            const float back = seg > 1.0f ? (float)WALL_BACKOFF / seg : 0.0f;
            for (int i = 0; i < 3; ++i)
                out[i] = cross[i] + (prev[i] - p[i]) * back; // short of the wall, along the way it came
            return 1;
        }
        if (g > -1.0e8f) {
            if (g > p[1] + (float)STEP_RISE) { // a rise it could not have flown over (a pillar the line check did not meet)
                memcpy(out, prev, sizeof prev);
                return 1;
            }
            if (p[1] <= g && d[1] <= 0.0f) {
                out[0] = p[0];
                out[1] = g;
                out[2] = p[2];
                return 2;
            }
        }
        memcpy(prev, p, sizeof prev);
    }
    memcpy(out, to, 3 * sizeof(float));
    return 0;
}

static const char* const kReasons[BLUEWAKE_CARRY_REASON_COUNT] = {"put", "thrown", "refused", "busy", "breakfree", "area", "event", "timeout", "left"};
const char* bluewake_carry_reason_name(int reason) { return reason >= 0 && reason < BLUEWAKE_CARRY_REASON_COUNT ? kReasons[reason] : "left"; }
int bluewake_carry_reason_from_name(const char* name) {
    for (int i = 0; i < BLUEWAKE_CARRY_REASON_COUNT; ++i)
        if (name != NULL && strcmp(name, kReasons[i]) == 0)
            return i;
    return BLUEWAKE_CARRY_REASON_LEFT;
}

// ---------------------------------------------------------------- events ----

static void emit(int kind, int peer, const float pos[3], int angle, int phase, int reason, int hurt) {
    lock();
    if (g_ev_n < QUEUE) {
        BluewakeCarryEvent* e = &g_ev[g_ev_n++];
        memset(e, 0, sizeof *e);
        e->kind = kind;
        e->peer = peer;
        if (pos != NULL)
            memcpy(e->pos, pos, sizeof e->pos);
        e->angle = angle;
        e->phase = phase;
        e->reason = reason;
        e->hurt = hurt;
    }
    unlock();
}

bool bluewake_carry_take_event(BluewakeCarryEvent* ev) {
    lock();
    const bool any = g_ev_n > 0;
    if (any) {
        *ev = g_ev[0];
        memmove(&g_ev[0], &g_ev[1], sizeof g_ev[0] * (size_t)(--g_ev_n));
    }
    unlock();
    return any;
}

bool bluewake_carry_take_hurt(int* from, float pos[3]) {
    lock();
    const bool any = g_hurt_pending;
    if (any) {
        *from = g_hurt_from;
        memcpy(pos, g_hurt_pos, sizeof g_hurt_pos);
        g_hurt_pending = false;
    }
    unlock();
    return any;
}

static void push_req(const Req* r) {
    lock();
    if (g_req_n < QUEUE)
        g_req[g_req_n++] = *r;
    unlock();
}

void bluewake_carry_set_room(bool allowed) {
    lock();
    g_allowed = allowed;
    unlock();
}

void bluewake_carry_peer_start(int from, const float pos[3], int angle) {
    Req r = {.kind = 1, .from = from, .angle = angle};
    memcpy(r.pos, pos, sizeof r.pos);
    push_req(&r);
}
void bluewake_carry_peer_pos(int from, const float pos[3], int angle, int phase) {
    Req r = {.kind = 2, .from = from, .angle = angle, .phase = phase};
    memcpy(r.pos, pos, sizeof r.pos);
    push_req(&r);
}
void bluewake_carry_peer_end(int from, int reason, const float pos[3], int angle, bool hurt) {
    Req r = {.kind = 3, .from = from, .angle = angle, .reason = reason, .hurt = hurt};
    if (pos != NULL)
        memcpy(r.pos, pos, sizeof r.pos);
    else
        r.angle = 0x7FFFFFFF; // no place
    push_req(&r);
}
void bluewake_carry_peer_gone(int id, int reason) {
    Req r = {.kind = 4, .from = id, .reason = reason, .angle = 0x7FFFFFFF};
    push_req(&r);
}

void bluewake_carry_reset(void) {
    Req r = {.kind = 5};
    push_req(&r);
}

int bluewake_carry_state(int* peer) {
    lock();
    int state = 0, who = 0;
    if (g_held.active) {
        state = 2;
        who = g_held.carrier;
    } else {
        for (int i = 0; i < MAX_PEERS; ++i)
            if (g_peer[i].id != 0 && g_peer[i].state != 0) {
                state = 1;
                who = g_peer[i].id;
            }
    }
    unlock();
    if (peer != NULL)
        *peer = who;
    return state;
}

bool bluewake_carry_puppet_pos(int id, float pos[3]) {
    bool held = false;
    lock();
    for (int i = 0; i < MAX_PEERS; ++i)
        if (g_peer[i].id == id && g_peer[i].state != 0) {
            memcpy(pos, g_peer[i].feet, sizeof g_peer[i].feet);
            held = true;
        }
    unlock();
    return held;
}

bool bluewake_carry_no_body(int id) {
    float ignored[3];
    return bluewake_carry_puppet_pos(id, ignored);
}

bool bluewake_carry_wants_actors(void) {
    lock();
    const bool want = g_allowed;
    unlock();
    return want;
}

// ------------------------------------------------------------ game state ----

static bool safe_now(CPUState* cpu) {
    if (mem_read8(cpu, EVENT_MODE) != 0u || mem_read8(cpu, NEXT_STAGE_GO) != 0u)
        return false;
    const u32 player = mem_read32(cpu, PLAYER);
    return guest_ptr(player) && mem_read16(cpu, SAVE_LIFE) != 0u;
}

// What Link may be doing when lifted: standing, walking, turning, side-stepping, ready to guard or crouching.
static bool proc_ok(u32 proc) {
    return (proc >= 0x04u && proc <= 0x0Cu) || proc == 0x0Eu || proc == 0x17u || proc == 0x18u;
}

static Peer* find_peer(int id, bool create) {
    for (int i = 0; i < MAX_PEERS; ++i)
        if (g_peer[i].id == id && id != 0)
            return &g_peer[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_PEERS; ++i)
        if (g_peer[i].id == 0) {
            memset(&g_peer[i], 0, sizeof g_peer[i]);
            g_peer[i].id = id;
            return &g_peer[i];
        }
    return NULL;
}

static bool carrying_anyone(void) {
    for (int i = 0; i < MAX_PEERS; ++i)
        if (g_peer[i].id != 0 && g_peer[i].state != 0)
            return true;
    return false;
}

// ----------------------------------------------------------- the lifted me ----

// Link's place (and, with angle >= 0, his facing) as the game will keep it.
static void set_link_place(CPUState* cpu, u32 player, const float pos[3], int angle) {
    wr_vec(cpu, player + AC_CURRENT, pos);
    wr_vec(cpu, player + AC_OLD, pos);
    wr_vec(cpu, KEEP_POS, pos);
    if (angle >= 0) {
        mem_write16(cpu, player + AC_SHAPE_Y, (u16)angle);
        mem_write16(cpu, player + AC_CUR_ANGLE_Y, (u16)angle);
        mem_write16(cpu, KEEP_SHAPE_ANGLE + 2u, (u16)angle);
        mem_write16(cpu, KEEP_CUR_ANGLE + 2u, (u16)angle);
    }
}

static void held_end(CPUState* cpu, int reason, bool tell, const float* place) {
    if (!g_held.active)
        return;
    lock();
    g_held.active = false;
    unlock();
    const u32 player = mem_read32(cpu, PLAYER);
    if (guest_ptr(player)) {
        if (place != NULL)
            set_link_place(cpu, player, place, -1);
        const float zero[3] = {0, 0, 0};
        wr_vec(cpu, player + AC_SPEED, zero);
        wr_f(cpu, player + AC_SPEED_F, 0.0f);
    }
    fprintf(stderr, "[carry] I am let go by player %d (%s) after %u frames\n", g_held.carrier, bluewake_carry_reason_name(reason),
            g_held.frames);
    if (tell)
        emit(BLUEWAKE_CARRY_EV_END, g_held.carrier, NULL, 0, 0, reason, 0);
}

static void held_begin(CPUState* cpu, const Req* r) {
    const u32 player = mem_read32(cpu, PLAYER);
    if (g_held.active && g_held.carrier != r->from) {
        emit(BLUEWAKE_CARRY_EV_END, r->from, NULL, 0, 0, BLUEWAKE_CARRY_REASON_BUSY, 0);
        return;
    }
    if (g_held.active)
        return; // the same player's CARRY_START again
    if (!g_allowed || g_scene == 0u || !safe_now(cpu) || !proc_ok(mem_read32(cpu, player + PLAYER_PROC)) || carrying_anyone()) {
        fprintf(stderr, "[carry] player %d's lift refused (allowed %d, safe %d, proc %u)\n", r->from, g_allowed, safe_now(cpu),
                guest_ptr(player) ? (unsigned)mem_read32(cpu, player + PLAYER_PROC) : 0u);
        emit(BLUEWAKE_CARRY_EV_END, r->from, NULL, 0, 0, BLUEWAKE_CARRY_REASON_REFUSED, 0);
        return;
    }
    lock();
    memset(&g_held, 0, sizeof g_held);
    g_held.active = true;
    g_held.carrier = r->from;
    memcpy(g_held.target, r->pos, sizeof r->pos);
    memcpy(g_held.cur, r->pos, sizeof r->pos);
    g_held.angle = r->angle;
    g_held.last_ms = g_held.start_ms = now_ms();
    g_held.room = (s8)mem_read8(cpu, STAY_ROOM);
    unlock();
    fprintf(stderr, "[carry] I am lifted by player %d at %.0f %.0f %.0f\n", r->from, r->pos[0], r->pos[1], r->pos[2]);
}

static void held_frame(CPUState* cpu) {
    const u32 player = mem_read32(cpu, PLAYER);
    if (!guest_ptr(player)) {
        held_end(cpu, BLUEWAKE_CARRY_REASON_AREA, true, NULL);
        return;
    }
    const double now = now_ms();
    const u16 trig = (u16)mem_read16(cpu, PAD + PAD_TRIG);
    if ((trig & ~BTN_START) != 0u || ((u16)mem_read16(cpu, PAD + PAD_HOLD) & ~BTN_START) != 0u)
        fprintf(stderr, "[carry] my buttons while held: pressed %04X, held %04X\n", trig, (unsigned)mem_read16(cpu, PAD + PAD_HOLD));
    if ((trig & BTN_B) != 0u) {
        held_end(cpu, BLUEWAKE_CARRY_REASON_BREAKFREE, true, NULL);
        return;
    }
    if (mem_read8(cpu, EVENT_MODE) != 0u || mem_read8(cpu, NEXT_STAGE_GO) != 0u || mem_read16(cpu, SAVE_LIFE) == 0u) {
        held_end(cpu, BLUEWAKE_CARRY_REASON_EVENT, true, NULL);
        return;
    }
    if ((s8)mem_read8(cpu, STAY_ROOM) != g_held.room) {
        held_end(cpu, BLUEWAKE_CARRY_REASON_AREA, true, NULL);
        return;
    }
    if (now - g_held.last_ms > TIMEOUT_MS || now - g_held.start_ms > HOLD_MAX_MS) {
        held_end(cpu, BLUEWAKE_CARRY_REASON_TIMEOUT, true, NULL);
        return;
    }
    ++g_held.frames;
    if (g_held.frames % 30u == 1u) {
        float now_pos[3];
        rd_vec(cpu, player + AC_CURRENT, now_pos);
        fprintf(stderr, "[carry] held frame %u: Link at %.0f %.0f %.0f (I put him at %.0f %.0f %.0f), target %.0f %.0f %.0f, proc %u\n", g_held.frames,
                now_pos[0], now_pos[1], now_pos[2], g_held.cur[0], g_held.cur[1], g_held.cur[2], g_held.target[0], g_held.target[1],
                g_held.target[2], (unsigned)mem_read32(cpu, player + PLAYER_PROC));
    }
    // Follow the carrier's hands: close the gap in a few frames, at once when far.
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
        d2 += (g_held.target[i] - g_held.cur[i]) * (g_held.target[i] - g_held.cur[i]);
    const float k = d2 > 400.0f * 400.0f ? 1.0f : 0.6f;
    for (int i = 0; i < 3; ++i)
        g_held.cur[i] += (g_held.target[i] - g_held.cur[i]) * k;
    set_link_place(cpu, player, g_held.cur, g_held.angle & 0xFFFF);
    const float zero[3] = {0, 0, 0};
    wr_vec(cpu, player + AC_SPEED, zero);
    wr_f(cpu, player + AC_SPEED_F, 0.0f);
    wr_f(cpu, player + AC_GRAVITY, 0.0f);
    mem_write32(cpu, player + PLAYER_ACCH_FLAGS, mem_read32(cpu, player + PLAYER_ACCH_FLAGS) | ACCH_GROUND_HIT);
    // No input but the pause button; R held down keeps the game's own crouch.
    const u16 hold = (u16)mem_read16(cpu, PAD + PAD_HOLD);
    mem_write16(cpu, PAD + PAD_HOLD, (u16)(hold & BTN_START));
    mem_write16(cpu, PAD + PAD_TRIG, (u16)(trig & BTN_START));
    for (u32 off = 0u; off < PAD_R_ANALOG; off += 4u)
        mem_write32(cpu, PAD + off, 0u); // sticks, c-stick, analogs
    wr_f(cpu, PAD + PAD_R_ANALOG, 1.0f);
    mem_write8(cpu, PAD + PAD_HOLD_LOCK_R, 1u);
    mem_write8(cpu, PAD + PAD_TRIG_LOCK_R, g_held.frames == 1u ? 1u : 0u);
}

// ------------------------------------------------------------ the lifter ----

static void peer_release(Peer* p, int reason, bool tell, bool hurt) {
    if (p->state == 0)
        return;
    fprintf(stderr, "[carry] player %d is let go (%s) at %.0f %.0f %.0f after %d frames\n", p->id, bluewake_carry_reason_name(reason),
            p->feet[0], p->feet[1], p->feet[2], p->frames);
    if (tell)
        emit(BLUEWAKE_CARRY_EV_END, p->id, p->feet, p->angle, 0, reason, hurt);
    lock();
    p->state = 0;
    unlock();
}

static void process_requests(CPUState* cpu) {
    Req req[QUEUE];
    int n;
    lock();
    n = g_req_n;
    memcpy(req, g_req, sizeof(Req) * (size_t)n);
    g_req_n = 0;
    unlock();
    for (int i = 0; i < n; ++i) {
        const Req* r = &req[i];
        switch (r->kind) {
        case 1:
            held_begin(cpu, r);
            break;
        case 2:
            if (g_held.active && g_held.carrier == r->from) {
                if (r->phase == BLUEWAKE_CARRY_PHASE_THROWN && g_held.phase != BLUEWAKE_CARRY_PHASE_THROWN) {
                    memcpy(g_held.valid, g_held.cur, sizeof g_held.cur); // the arc starts where he is held
                }
                memcpy(g_held.target, r->pos, sizeof r->pos);
                g_held.angle = r->angle;
                g_held.phase = r->phase;
                g_held.last_ms = now_ms();
            }
            break;
        case 3:
        case 4: {
            if (g_held.active && g_held.carrier == r->from) {
                const bool place = r->kind == 3 && r->angle != 0x7FFFFFFF;
                if (place && g_held.phase == BLUEWAKE_CARRY_PHASE_THROWN)
                    g_settle = SETTLE_FRAMES; // the place a carrier's arc gives is checked against the ground by the fiber
                held_end(cpu, r->reason, false, place ? r->pos : NULL);
                if (r->hurt && r->kind == 3) {
                    lock();
                    g_hurt_pending = true;
                    g_hurt_from = r->from;
                    memcpy(g_hurt_pos, r->pos, sizeof g_hurt_pos);
                    unlock();
                }
            }
            Peer* p = find_peer(r->from, false);
            if (p != NULL && p->state != 0) {
                // The lifted player refused, broke free, left or changed area: I let go.
                peer_release(p, r->reason, false, false);
                g_force_release = true;
            }
            break;
        }
        case 5:
            held_end(cpu, BLUEWAKE_CARRY_REASON_LEFT, false, NULL);
            for (int k = 0; k < MAX_PEERS; ++k)
                if (g_peer[k].state != 0) {
                    lock();
                    g_peer[k].state = 0;
                    unlock();
                    g_force_release = true;
                }
            break;
        }
    }
}

// Called by the pvp execute loop for each target with a Dummy actor, before the loop sets the actor's place.
bool bluewake_carry_target(CPUState* cpu, int id, uint32_t actor, bool here, const float pos[3]) {
    (void)pos;
    lock();
    const bool allowed = g_allowed;
    unlock();
    Peer* p = find_peer(id, allowed && here);
    if (p == NULL)
        return false;
    const u32 player = mem_read32(cpu, PLAYER);
    const u32 status = guest_ptr(actor) ? mem_read32(cpu, actor + AC_STATUS) : 0u;
    const bool carried = (status & STTS_CARRY) != 0u;
    if (p->state == 0) {
        if (!carried || !guest_ptr(player))
            return false;
        if (!allowed || !safe_now(cpu) || g_held.active) {
            fprintf(stderr, "[carry] lift of player %d not allowed (room %d, safe %d): let go\n", id, allowed, safe_now(cpu));
            g_force_release = true;
            return false;
        }
        float hands[3];
        rd_vec(cpu, actor + AC_CURRENT, hands);
        lock();
        p->state = 1;
        p->frames = 0;
        p->since_pos = 0;
        memcpy(p->feet, hands, sizeof hands);
        p->feet[1] -= HOLD_DROP;
        p->angle = (s16)mem_read16(cpu, actor + AC_SHAPE_Y);
        unlock();
        fprintf(stderr, "[carry] I lift player %d: hands at %.0f %.0f %.0f\n", id, hands[0], hands[1], hands[2]);
        emit(BLUEWAKE_CARRY_EV_START, id, p->feet, p->angle, 0, 0, 0);
        return true;
    }
    if (p->state == 1) {
        ++p->frames;
        if (carried && guest_ptr(player)) {
            if (!safe_now(cpu)) {
                peer_release(p, BLUEWAKE_CARRY_REASON_EVENT, true, false);
                g_force_release = true;
                return false;
            }
            float hands[3];
            rd_vec(cpu, actor + AC_CURRENT, hands);
            lock();
            memcpy(p->feet, hands, sizeof hands);
            p->feet[1] -= HOLD_DROP;
            p->angle = (s16)mem_read16(cpu, actor + AC_SHAPE_Y);
            unlock();
            if (++p->since_pos >= POS_EVERY) {
                p->since_pos = 0;
                emit(BLUEWAKE_CARRY_EV_POS, id, p->feet, p->angle, BLUEWAKE_CARRY_PHASE_HELD, 0, 0);
            }
            return true;
        }
        // The game's carry status is off: put down, or thrown (Link gave the actor a speed).
        const float speed_f = rd_f(cpu, actor + AC_SPEED_F), speed_y = rd_f(cpu, actor + AC_SPEED + 4u);
        const int ang = (s16)mem_read16(cpu, actor + AC_CUR_ANGLE_Y);
        float at[3];
        rd_vec(cpu, actor + AC_CURRENT, at);
        wr_f(cpu, actor + AC_SPEED_F, 0.0f);
        if (speed_f >= (float)THROW_MIN_SPEED || fabsf(speed_y) >= (float)THROW_MIN_SPEED) {
            const float rad = (float)ang * 3.14159265f / 32768.0f;
            const float g = rd_f(cpu, actor + AC_GRAVITY);
            lock();
            p->vel[0] = sinf(rad) * speed_f;
            p->vel[1] = speed_y;
            p->vel[2] = cosf(rad) * speed_f;
            p->gravity = g < -0.5f ? g : -3.0f;
            memcpy(p->feet, at, sizeof at);
            p->feet[1] -= HOLD_DROP;
            p->floor_y = guest_ptr(player) ? rd_f(cpu, player + AC_CURRENT + 4u) : p->feet[1];
            p->angle = ang;
            p->frames = 0;
            p->blocked_said = false;
            p->state = 2;
            unlock();
            fprintf(stderr, "[carry] player %d thrown: speed %.1f up %.1f angle %d, gravity %.1f\n", id, speed_f, speed_y, ang, p->gravity);
            emit(BLUEWAKE_CARRY_EV_POS, id, p->feet, ang, BLUEWAKE_CARRY_PHASE_THROWN, 0, 0);
            return true;
        }
        lock();
        memcpy(p->feet, at, sizeof at);
        unlock();
        peer_release(p, BLUEWAKE_CARRY_REASON_PUT, true, false);
        return false;
    }
    return true; // flying: the puppet is shown where the arc is (bluewake_carry_puppet_pos), the stand-in follows it
}

// The arcs of the players I threw, one step a frame from the draw fiber (it asks the stage): the puppet flies on the speed Link gave it
// and the game's gravity, stops before a wall, lands on the ground under it (not on the thrower's own floor: a ledge, a slope or the
// water's edge is where the ground is), and each place goes to that player as a CARRY_POS.
static void flights(GuestCoro* co, CPUState* cpu) {
    for (int i = 0; i < MAX_PEERS; ++i) {
        Peer* p = &g_peer[i];
        if (p->id == 0 || p->state != 2)
            continue;
        ++p->frames;
        float next[3], out[3];
        for (int k = 0; k < 3; ++k)
            next[k] = p->feet[k] + p->vel[k];
        const int hit = trace_arc(co, cpu, p->feet, next, out);
        lock();
        memcpy(p->feet, out, sizeof out);
        p->vel[1] += p->gravity;
        if (hit == 1) {
            p->vel[0] = 0.0f; // against a wall: it drops from there
            p->vel[2] = 0.0f;
        }
        unlock();
        // (with no ground below at all, the thrower's floor is the old landing)
        const bool no_ground = hit == 0 && !g_trace_known && p->feet[1] <= p->floor_y && p->vel[1] <= 0.0f;
        const bool landed = hit == 2 || no_ground;
        if (hit == 1 && !p->blocked_said) {
            p->blocked_said = true;
            fprintf(stderr, "[carry] player %d's arc meets a wall at %.0f %.0f %.0f\n", p->id, p->feet[0], p->feet[1], p->feet[2]);
        }
        if (landed || p->frames >= FLIGHT_MAX || !safe_now(cpu)) {
            if (p->feet[1] < p->floor_y && !g_trace_known)
                p->feet[1] = p->floor_y;
            peer_release(p, BLUEWAKE_CARRY_REASON_THROWN, true, true);
            continue;
        }
        emit(BLUEWAKE_CARRY_EV_POS, p->id, p->feet, p->angle, BLUEWAKE_CARRY_PHASE_THROWN, 0, 0);
    }
}

uint32_t bluewake_carry_attention(CPUState* cpu, int id, uint32_t actor, bool here) {
    if (!guest_ptr(actor))
        return 0u;
    lock();
    const bool allowed = g_allowed;
    unlock();
    Peer* p = find_peer(id, false);
    const bool busy = p != NULL && p->state != 0;
    const u32 status = mem_read32(cpu, actor + AC_STATUS);
    if ((status & STTS_HEAVY) != 0u)
        mem_write32(cpu, actor + AC_STATUS, status & ~STTS_HEAVY); // a light thing: Link lifts it with his bare hands
    if (!allowed || !here || busy || g_held.active || carrying_anyone() || !safe_now(cpu))
        return 0u;
    mem_write8(cpu, actor + AC_ATTN_DIST + ATTN_TYPE_CARRY, ATTN_DIST_CARRY);
    return ATTN_ACTION_CARRY;
}

void bluewake_carry_execute(CPUState* cpu, uint32_t scene_id) {
    ++g_frame;
    if (scene_id != g_scene) {
        if (g_scene != 0u)
            bluewake_carry_scene_end(cpu);
        g_scene = scene_id;
    }
    process_requests(cpu);
    if (g_held.active)
        held_frame(cpu);
}

void bluewake_carry_scene_end(CPUState* cpu) {
    if (g_held.active)
        held_end(cpu, BLUEWAKE_CARRY_REASON_AREA, true, NULL);
    for (int i = 0; i < MAX_PEERS; ++i)
        if (g_peer[i].id != 0 && g_peer[i].state != 0)
            peer_release(&g_peer[i], BLUEWAKE_CARRY_REASON_AREA, true, false); // the stand-in actors go with the scene
    g_scene = 0;
}

// ----------------------------------------------------------------- fiber ----

bool bluewake_carry_wants_fiber(void) {
    if (g_force_release || g_settle > 0)
        return true;
    for (int i = 0; i < MAX_PEERS; ++i)
        if (g_peer[i].id != 0 && g_peer[i].state == 2)
            return true; // a flight to step against the stage
    if (g_held.active && g_held.phase == BLUEWAKE_CARRY_PHASE_THROWN)
        return true;
    return g_held.active && g_held.frames >= 1u && g_held.frames % 4u == 0u;
}

void bluewake_carry_fiber(GuestCoro* co, CPUState* cpu) {
    const u32 player = mem_read32(cpu, PLAYER);
    if (!guest_ptr(player))
        return;
    flights(co, cpu);
    // I am thrown: my place this frame (set by the carrier's last position) is walked from the last free one against the stage; a
    // wall or the ground stops the arc there, and he stays until the carrier lets go.
    if (g_held.active && g_held.phase == BLUEWAKE_CARRY_PHASE_THROWN) {
        float out[3];
        const int hit = trace_arc(co, cpu, g_held.valid, g_held.cur, out);
        if (hit != 0) {
            // Where it stopped is a free place: I am let go there (the carrier is told; the game's own gravity does the rest), and the
            // landing hurts as the carrier's own CARRY_END would have said (PvP on).
            fprintf(stderr, "[carry] my arc %s at %.0f %.0f %.0f: I am let go there\n", hit == 1 ? "meets a wall" : "reaches the ground", out[0], out[1], out[2]);
            const int carrier = g_held.carrier;
            held_end(cpu, BLUEWAKE_CARRY_REASON_THROWN, true, out);
            lock();
            g_hurt_pending = true;
            g_hurt_from = carrier;
            memcpy(g_hurt_pos, out, sizeof out);
            unlock();
        } else {
            memcpy(g_held.valid, g_held.cur, sizeof g_held.cur);
        }
    }
    // Let go after a throw at a place the carrier's arc gave: not inside the ground (the ground below is read from above him).
    if (g_settle > 0 && !g_held.active && !nostage()) {
        --g_settle;
        float at[3];
        rd_vec(cpu, player + AC_CURRENT, at);
        const float g = ground_at(co, cpu, at[0], at[1] + (float)TRACE_LOOK_UP, at[2]);
        if (g > at[1] + 1.0f) {
            at[1] = g;
            set_link_place(cpu, player, at, -1);
            fprintf(stderr, "[carry] I was let go inside the ground: raised to %.0f\n", g);
            g_settle = 0;
        }
    }
    if (g_force_release) {
        g_force_release = false;
        bluewake_guest_call(co, FN_FREE_GRAB_ITEM, player, 0, 0, 0, 0, 0);
        fprintf(stderr, "[carry] what Link held is let go\n");
    }
    if (g_held.active) {
        const u32 proc = mem_read32(cpu, player + PLAYER_PROC);
        if (proc != PROC_CROUCH && proc_ok(proc)) {
            bluewake_guest_call(co, FN_PROC_CROUCH_INIT, player, 0, 0, 0, 0, 0);
            static unsigned n;
            if (n++ < 4u)
                fprintf(stderr, "[carry] my Link crouches (procCrouch_init, was %u)\n", (unsigned)proc);
        }
    }
}
