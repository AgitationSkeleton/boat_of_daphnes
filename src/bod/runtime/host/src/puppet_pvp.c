// Other players you can bump into, target and fight: see puppet_pvp.h.
#include "puppet_pvp.h"
#include "puppet_carry.h"
#include "game_options.h"

#include "gxruntime/platform.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
    PLAYER = 0x803CA754u,     // play.mpPlayerPtr[0]
    PLAYER_POS = 0x1F8u,      // fopAc_ac_c::current.pos
    PLAYER_STTS = 0x3FE8u,    // daPy_lk_c::mStts (dCcD_Stts, 0x3C)
    PLAYER_CYL = 0x4024u,     // daPy_lk_c::mCyl (dCcD_Cyl, 0x130): what bodies are copies of
    STAY_ROOM = 0x803F6A78u,
    EVENT_MODE = 0x803C9EA2u, // play.mEvtCtrl.mMode: 0 when no event runs
    NEXT_STAGE_GO = 0x803C9D48u + 0xCu, // play.mNextStage enable
    SAVE_LIFE = 0x803C4C08u + 2u,       // dSv_player_status_a_c::mLife (quarter hearts)
    CCS = 0x803C5EA8u + 0x1404u,        // g_dComIfG_gameInfo.play.mCcS (dCcS)

    // cCcS: pointer arrays and their counts.
    CCS_AT = 0x0000u, CCS_TG = 0x0400u, CCS_CO = 0x1000u, CCS_OBJ = 0x1400u,
    CCS_N_AT = 0x2800u, CCS_N_TG = 0x2804u, CCS_N_CO = 0x2808u, CCS_N_OBJ = 0x280Cu,
    CCS_MAX_AT = 0x100, CCS_MAX_TG = 0x300, CCS_MAX_CO = 0x100, CCS_MAX_OBJ = 0x500,

    // dCcD_Cyl (0x130) and dCcD_Stts (0x3C).
    O_AT_SPRM = 0x00u, O_AT_TYPE = 0x10u, O_AT_ATP = 0x14u,
    O_TG_SPRM = 0x18u, O_TG_RPRM = 0x1Cu, O_TG_HIT = 0x20u, O_TG_TYPE = 0x28u,
    O_CO_SPRM = 0x2Cu, O_CO_RPRM = 0x30u, O_CO_HIT = 0x34u,
    O_FLAGS = 0x40u, O_STTS = 0x44u,
    G_AT = 0x50u, G_TG = 0x94u, G_CO = 0xDCu, // dCcD_GObjAt, Tg, Co: sprm +0, rprm +4, callback +8, apid +0xC, actor +0x10, counter +0x14
    G_AT_SE = 0x6Cu, G_AT_HITMARK = 0x6Eu, G_AT_SPL = 0x6Fu, G_AT_VEC = 0x7Cu,
    G_TG_SPL = 0xB3u,
    C_CENTER = 0x118u, C_RADIUS = 0x124u, C_HEIGHT = 0x128u,
    CYL_SIZE = 0x130u, STTS_SIZE = 0x3Cu,
    S_MOVE = 0x00u, S_ACTOR = 0x0Cu, S_APID = 0x10u, S_WEIGHT = 0x14u, S_DMG = 0x16u,
    S_AT_SPL = 0x20u, S_ROOM = 0x22u, S_PERF = 0x24u, S_APIDS = 0x28u, S_FLAG = 0x38u,

    // cCcD_*SPrm
    AT_SET = 0x01u, AT_VS_PLAYER = 0x04u,
    TG_SET = 0x01u, TG_IS_ENEMY = 0x02u, TG_IS_OTHER = 0x08u,
    CO_SET = 0x01u, CO_IS_PLAYER = 0x04u, CO_VS_ALL = 0x70u,

    // AT_TYPE_*
    AT_BOMB = 0x00000020u, AT_STALFOS_MACE = 0x01000000u,
    AT_FROM_LINK = 0x00000002u | 0x00000008u | 0x00000020u | 0x00000040u | 0x00004000u | 0x00008000u | 0x00010000u |
                   0x00040000u | 0x00080000u | 0x00100000u | 0x02000000u | 0x08000000u,

    BODY_SIZE = 0x170u,   // one cylinder, its stts
    BODY_STTS = 0x130u,
    MAX_TARGETS = 16,
    BLOCK_SIZE = BODY_SIZE * (MAX_TARGETS + 1) + 0x80u, // the bodies, the attack collider, a scratch area
    BLOCK_AT = BODY_SIZE * MAX_TARGETS,
    BLOCK_SCRATCH = BODY_SIZE * (MAX_TARGETS + 1),

    BODY_RADIUS_X = 30, BODY_HEIGHT_X = 125, // Link's own (l_cyl_src)
    BODY_WEIGHT = 0xFEu,                     // rank 9: Link is pushed away, the body does not move
    HIT_RADIUS = 45,                         // the attack collider's, laid on Link's side facing the attacker
    HIT_PUSH = 12,                           // its vector's length (knockback speed)
    HIT_COOLDOWN_MS = 700,
    STAB_HIT_COOLDOWN_MS = 350,              // between the hits of two different crouch-stab swings (a stab swings about every 270 ms)
    EVENT_GRACE_MS = 1500,                   // and for this long after an event ends
    SAFE_MS = 3000,                          // no hits for this long after a scene starts

    // Guest functions (GZLE01 symbols.txt).
    FN_NEW = 0x802B0C38u,          // operator new(size)
    FN_DELETE = 0x802B0D28u,       // operator delete(ptr)
    FN_ACTOR_CREATE = 0x80024598u, // fopAcM_create(name, params, pos, room, angle, scale, callback)
    FN_SEARCH_BY_ID = 0x800241C0u, // fopAcM_SearchByID(id, &actor)
    FN_DELETE_ACTOR = 0x800244B8u, // fopAcM_delete(id)

    // fopAc_ac_c
    AC_PID = 0x04u, AC_HOME = 0x1D0u, AC_OLD = 0x1E4u, AC_CURRENT = 0x1F8u, AC_SPEED = 0x220u, AC_EYE = 0x260u,
    AC_ATTN_POS = 0x274u, AC_ATTN_FLAGS = 0x280u,
    ATTN_LOCKON_MISC = 0x1u,       // fopAc_Attn_LOCKON_MISC_e: Z-targetable, no talk and no battle music
    ATTN_HEIGHT = 100,
    ACTOR_WAIT_FRAMES = 600,       // a Dummy that has not shown up this long is asked for again
};

typedef struct Target {
    int id;
    bool hit_ok;              // set by the app
    uint32_t noted;           // the draw frame its puppet was last drawn
    float pos[3];
    uint32_t cyl, stts;       // the body (guest), 0: none
    uint32_t armed;           // the frame its body was last put into the checks
    bool tg_armed;
    double last_hit_ms;
    unsigned hit_swing;       // the crouch stab's serial of the last hit, plus one (0: it was not a stab)
    uint32_t pid, actor;      // its Dummy actor
    uint32_t pid_frame;
} Target;

typedef struct Incoming {
    int from, damage;
    uint32_t type;
    float pos[3];
} Incoming;

static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static Target g_target[MAX_TARGETS];
static bool g_collision = true;
static int g_mode, g_scale = 100;
static BluewakePvpHit g_out[8];
static int g_out_n;
static Incoming g_in[4];
static int g_in_n;

static uint32_t g_scene;        // the play scene the objects below belong to
static uint32_t g_frame;        // draw frames
static uint32_t g_menu;
static double g_scene_ms;
static double g_event_ms;
static bool g_calm; // the last execute's answer: no bodies or actors are made until the world is calm
static uint32_t g_block;        // guest memory: the bodies, the attack collider, scratch
static bool g_broken;           // the layouts did not match Link's: nothing is done in this scene
static uint32_t g_watch = 0;    // frames left to watch the effect of a hit applied

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

static void wr_vec(CPUState* cpu, u32 address, float x, float y, float z) {
    wr_f(cpu, address, x);
    wr_f(cpu, address + 4u, y);
    wr_f(cpu, address + 8u, z);
}

static u32 call(GuestCoro* co, u32 fn, u32 a, u32 b, u32 c, u32 d) { return bluewake_guest_call(co, fn, a, b, c, d, 0, 0); }

// ---------------------------------------------------------------- the app ----

static Target* find_target(int id, bool create) {
    for (int i = 0; i < MAX_TARGETS; ++i)
        if (g_target[i].id == id && id != 0)
            return &g_target[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_TARGETS; ++i)
        if (g_target[i].id == 0) {
            memset(&g_target[i], 0, sizeof g_target[i]);
            g_target[i].id = id;
            return &g_target[i];
        }
    return NULL;
}

void bluewake_pvp_set_rules(bool collision, int mode, int scale_percent) {
    lock();
    g_collision = collision;
    g_mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    g_scale = scale_percent < 25 ? 25 : scale_percent > 400 ? 400 : scale_percent;
    unlock();
}

void bluewake_pvp_set_target(int id, bool hit_ok) {
    lock();
    Target* t = find_target(id, true);
    if (t != NULL)
        t->hit_ok = hit_ok;
    unlock();
}

// A player who left or went elsewhere: their slot is given up once their Dummy actor is (draw frames, below).
void bluewake_pvp_remove(int id) {
    lock();
    Target* t = find_target(id, false);
    if (t != NULL)
        t->noted = 0u;
    unlock();
}

void bluewake_pvp_clear(void) {
    lock();
    for (int i = 0; i < MAX_TARGETS; ++i)
        g_target[i].noted = 0u;
    unlock();
}

bool bluewake_pvp_take_hit(BluewakePvpHit* hit) {
    lock();
    const bool any = g_out_n > 0;
    if (any) {
        *hit = g_out[0];
        memmove(&g_out[0], &g_out[1], sizeof g_out[0] * (size_t)(--g_out_n));
    }
    unlock();
    return any;
}

void bluewake_pvp_incoming(int from, int damage, uint32_t type, const float from_pos[3]) {
    lock();
    if (g_in_n < (int)(sizeof g_in / sizeof g_in[0])) {
        Incoming* in = &g_in[g_in_n++];
        in->from = from;
        in->damage = damage;
        in->type = type;
        memcpy(in->pos, from_pos, sizeof in->pos);
    }
    unlock();
}

void bluewake_pvp_note(int id, const float pos[3]) {
    lock();
    Target* t = find_target(id, true);
    if (t != NULL) {
        t->noted = g_frame;
        memcpy(t->pos, pos, sizeof t->pos);
    }
    unlock();
}

// ------------------------------------------------------------- game state ----

// Whether the game takes hits now: no event (a cutscene, a talk, a door), no menu, no scene change, not in the first
// seconds of a scene (a spawn or a respawn), and Link alive.
static bool safe_now(CPUState* cpu) {
    if (g_menu != 0u || mem_read8(cpu, EVENT_MODE) != 0u || mem_read8(cpu, NEXT_STAGE_GO) != 0u)
        return false;
    if (now_ms() - g_scene_ms < SAFE_MS)
        return false;
    const u32 player = mem_read32(cpu, PLAYER);
    return guest_ptr(player) && mem_read16(cpu, SAVE_LIFE) != 0u;
}

// cCcS::Set for an object whose parts are known.
static void ccs_set(CPUState* cpu, u32 obj, bool at, bool tg, bool co) {
    const int n = (int)mem_read32(cpu, CCS + CCS_N_OBJ);
    if (n >= CCS_MAX_OBJ)
        return;
    if (at) {
        const int c = (int)mem_read32(cpu, CCS + CCS_N_AT);
        if (c < CCS_MAX_AT) {
            mem_write32(cpu, CCS + CCS_AT + 4u * (u32)c, obj);
            mem_write32(cpu, CCS + CCS_N_AT, (u32)(c + 1));
        }
    }
    if (tg) {
        const int c = (int)mem_read32(cpu, CCS + CCS_N_TG);
        if (c < CCS_MAX_TG) {
            mem_write32(cpu, CCS + CCS_TG + 4u * (u32)c, obj);
            mem_write32(cpu, CCS + CCS_N_TG, (u32)(c + 1));
        }
    }
    if (co) {
        const int c = (int)mem_read32(cpu, CCS + CCS_N_CO);
        if (c < CCS_MAX_CO) {
            mem_write32(cpu, CCS + CCS_CO + 4u * (u32)c, obj);
            mem_write32(cpu, CCS + CCS_N_CO, (u32)(c + 1));
        }
    }
    mem_write32(cpu, CCS + CCS_OBJ + 4u * (u32)n, obj);
    mem_write32(cpu, CCS + CCS_N_OBJ, (u32)(n + 1));
}

// Takes my objects out of the checks' lists (the memory is about to go).
static void ccs_purge(CPUState* cpu) {
    if (g_block == 0u)
        return;
    static const struct { u32 array, count, max; } kLists[4] = {{CCS_AT, CCS_N_AT, CCS_MAX_AT}, {CCS_TG, CCS_N_TG, CCS_MAX_TG},
                                                                  {CCS_CO, CCS_N_CO, CCS_MAX_CO}, {CCS_OBJ, CCS_N_OBJ, CCS_MAX_OBJ}};
    for (int l = 0; l < 4; ++l) {
        const int n = (int)mem_read32(cpu, CCS + kLists[l].count);
        int kept = 0;
        for (int i = 0; i < n && i < kLists[l].max; ++i) {
            const u32 obj = mem_read32(cpu, CCS + kLists[l].array + 4u * (u32)i);
            if (obj >= g_block && obj < g_block + BLOCK_SIZE)
                continue;
            mem_write32(cpu, CCS + kLists[l].array + 4u * (u32)kept++, obj);
        }
        mem_write32(cpu, CCS + kLists[l].count, (u32)kept);
    }
}

static void forget_scene(void) {
    g_scene = 0;
    g_calm = false;
    g_block = 0;
    g_broken = false;
    g_watch = 0;
    lock();
    for (int i = 0; i < MAX_TARGETS; ++i) {
        Target* t = &g_target[i];
        t->cyl = t->stts = 0;
        t->pid = t->actor = 0;
        t->tg_armed = false;
    }
    unlock();
}

void bluewake_pvp_scene_end(CPUState* cpu) {
    bluewake_carry_scene_end(cpu); // players held end (puppet_carry.c)
    ccs_purge(cpu);
    // The block itself is given back by the delete fiber; the actors go with the scene.
    lock();
    for (int i = 0; i < MAX_TARGETS; ++i) {
        g_target[i].cyl = g_target[i].stts = 0;
        g_target[i].pid = g_target[i].actor = 0;
        g_target[i].tg_armed = false;
    }
    unlock();
    g_scene = 0;
}

bool bluewake_pvp_has_block(void) { return g_block != 0u; }

void bluewake_pvp_fiber_delete(GuestCoro* co, CPUState* cpu) {
    (void)cpu;
    if (g_block != 0u) {
        const u32 block = g_block;
        g_block = 0;
        call(co, FN_DELETE, block, 0, 0, 0);
    }
    g_broken = false;
}

// ----------------------------------------------------------------- the fiber ----

static void clone_words(CPUState* cpu, u32 dst, u32 src, u32 bytes) {
    for (u32 i = 0; i < bytes; i += 4u)
        mem_write32(cpu, dst + i, mem_read32(cpu, src + i));
}

// A stts as dCcD_Stts::Init(weight, 0xFF, NULL) leaves it: no actor, the weight given.
static void init_stts(CPUState* cpu, u32 stts, u32 weight) {
    wr_vec(cpu, stts + S_MOVE, 0.0f, 0.0f, 0.0f);
    mem_write32(cpu, stts + S_ACTOR, 0u);
    mem_write32(cpu, stts + S_APID, 0xFFFFFFFFu);
    mem_write8(cpu, stts + S_WEIGHT, (u8)weight);
    mem_write8(cpu, stts + S_WEIGHT + 1u, 0xFFu);
    mem_write8(cpu, stts + S_DMG, 0u);
    mem_write8(cpu, stts + S_AT_SPL, 0u);
    mem_write8(cpu, stts + S_AT_SPL + 1u, 0u);
    mem_write8(cpu, stts + S_ROOM, 0u);
    mem_write16(cpu, stts + S_PERF, 0xFFFFu);
    for (u32 i = 0; i < 4u; ++i)
        mem_write32(cpu, stts + S_APIDS + 4u * i, 0xFFFFFFFFu);
    mem_write32(cpu, stts + S_FLAG, 1u); // dCcD_GStts::Stts_NoActor_e
}

// The collider part of a copy of Link's mCyl: nothing registered, no hit remembered.
static void init_cyl(CPUState* cpu, u32 cyl, u32 stts) {
    mem_write32(cpu, cyl + O_STTS, stts);
    mem_write32(cpu, cyl + O_FLAGS, 0u);
    mem_write32(cpu, cyl + O_AT_SPRM, 0u);
    mem_write32(cpu, cyl + O_TG_SPRM, 0u);
    mem_write32(cpu, cyl + O_CO_SPRM, 0u);
    // What a hit leaves in the object itself (the result bit, the object that hit) and in the game's part of it.
    static const u32 kHit[6] = {0x04u, 0x08u, O_TG_RPRM, O_TG_HIT, O_CO_RPRM, O_CO_HIT};
    for (int k = 0; k < 6; ++k)
        mem_write32(cpu, cyl + kHit[k], 0u);
    const u32 parts[3] = {G_AT, G_TG, G_CO};
    for (int p = 0; p < 3; ++p) {
        mem_write32(cpu, cyl + parts[p] + 0x00u, 0u);          // special parameters
        mem_write32(cpu, cyl + parts[p] + 0x04u, 0u);          // result bits
        mem_write32(cpu, cyl + parts[p] + 0x08u, 0u);          // hit callback
        mem_write32(cpu, cyl + parts[p] + 0x0Cu, 0xFFFFFFFFu); // the other actor's process id
        mem_write32(cpu, cyl + parts[p] + 0x10u, 0u);          // the other actor
        mem_write8(cpu, cyl + parts[p] + 0x14u, 0u);           // effect counter
    }
    mem_write8(cpu, cyl + G_AT_HITMARK, 0u); // none: the attack collider shows no spark of its own
    mem_write8(cpu, cyl + G_AT_SE, 0u);
    mem_write8(cpu, cyl + G_AT_SPL, 0u);
    mem_write8(cpu, cyl + G_TG_SPL, 0u);
    wr_vec(cpu, cyl + G_AT_VEC, 0.0f, 0.0f, 0.0f);
}

static bool make_bodies(CPUState* cpu, u32 player, u32 block) {
    const u32 link_cyl = player + PLAYER_CYL, link_stts = player + PLAYER_STTS;
    // Link's own collider is what the layout above describes: its stts is his, and the stts is of his actor.
    if (mem_read32(cpu, link_cyl + O_STTS) != link_stts || mem_read32(cpu, link_stts + S_ACTOR) != player) {
        fprintf(stderr, "[pvp] layout check failed: Link's cylinder has stts 0x%08X (his is 0x%08X), its actor 0x%08X\n",
                mem_read32(cpu, link_cyl + O_STTS), link_stts, mem_read32(cpu, link_stts + S_ACTOR));
        return false;
    }
    const float radius = rd_f(cpu, link_cyl + C_RADIUS), height = rd_f(cpu, link_cyl + C_HEIGHT);
    fprintf(stderr, "[pvp] Link's cylinder: radius %.1f height %.1f, tg type %08X, sprm at %02X tg %02X co %02X\n", radius, height,
            mem_read32(cpu, link_cyl + O_TG_TYPE), mem_read32(cpu, link_cyl + O_AT_SPRM), mem_read32(cpu, link_cyl + O_TG_SPRM),
            mem_read32(cpu, link_cyl + O_CO_SPRM));
    for (u32 n = 0; n <= MAX_TARGETS; ++n) {
        const u32 cyl = block + BODY_SIZE * n, stts = cyl + BODY_STTS;
        clone_words(cpu, cyl, link_cyl, CYL_SIZE);
        clone_words(cpu, stts, link_stts, STTS_SIZE);
        init_stts(cpu, stts, n == MAX_TARGETS ? 0u : BODY_WEIGHT);
        init_cyl(cpu, cyl, stts);
        wr_f(cpu, cyl + C_RADIUS, n == MAX_TARGETS ? (float)HIT_RADIUS : (float)BODY_RADIUS_X);
        wr_f(cpu, cyl + C_HEIGHT, (float)BODY_HEIGHT_X);
    }
    return true;
}

bool bluewake_pvp_wants_fiber(void) {
    if (g_broken || g_scene == 0u || !g_calm)
        return false;
    lock();
    bool want = false;
    const bool active = g_collision || g_mode != BLUEWAKE_PVP_OFF || bluewake_carry_wants_actors();
    if (active && g_block == 0u)
        want = true;
    for (int i = 0; i < MAX_TARGETS && !want; ++i) {
        const Target* t = &g_target[i];
        if (t->id <= 0)
            continue;
        const bool here = active && g_frame - t->noted <= 2u;
        if (here && (t->actor == 0u))
            want = true;
        else if (!here && t->pid != 0u && g_frame - t->noted > 90u)
            want = true; // gone: its actor is given back
    }
    unlock();
    return want;
}

void bluewake_pvp_fiber(GuestCoro* co, CPUState* cpu) {
    const u32 player = mem_read32(cpu, PLAYER);
    if (g_broken || g_scene == 0u || !guest_ptr(player))
        return;
    bool active;
    lock();
    active = g_collision || g_mode != BLUEWAKE_PVP_OFF || bluewake_carry_wants_actors();
    unlock();
    if (active && g_block == 0u) {
        const u32 block = call(co, FN_NEW, BLOCK_SIZE, 0, 0, 0);
        if (!guest_ptr(block)) {
            fprintf(stderr, "[pvp] no memory for the bodies\n");
            g_broken = true;
            return;
        }
        if (!make_bodies(cpu, player, block)) {
            call(co, FN_DELETE, block, 0, 0, 0);
            g_broken = true;
            return;
        }
        g_block = block;
        fprintf(stderr, "[pvp] bodies made at 0x%08X\n", block);
    }
    if (g_block == 0u)
        return;
    const u32 scratch = g_block + BLOCK_SCRATCH;
    for (int i = 0; i < MAX_TARGETS; ++i) {
        Target* t = &g_target[i];
        if (t->id <= 0)
            continue;
        lock();
        const bool here = active && g_frame - t->noted <= 2u;
        const uint32_t slot_frame = g_frame;
        const float pos[3] = {t->pos[0], t->pos[1], t->pos[2]};
        unlock();
        if (t->cyl == 0u) {
            t->cyl = g_block + BODY_SIZE * (u32)i;
            t->stts = t->cyl + BODY_STTS;
        }
        if (here && t->actor == 0u) {
            if (t->pid == 0u || slot_frame - t->pid_frame > ACTOR_WAIT_FRAMES) {
                // A "Dummy" actor (stage object TagD1) at the puppet: it does nothing; the host puts its attention point
                // on the puppet each frame, and Z targets it.
                wr_vec(cpu, scratch, pos[0], pos[1], pos[2]);
                mem_write32(cpu, scratch + 0x10u, 0u);
                mem_write16(cpu, scratch + 0x14u, 0u);
                static const char kName[] = "TagD1";
                for (u32 k = 0; k < sizeof kName; ++k)
                    mem_write8(cpu, scratch + 0x40u + k, (u8)kName[k]);
                cpu->gpr[9] = 0u; // no create callback
                t->pid = bluewake_guest_call(co, FN_ACTOR_CREATE, scratch + 0x40u, 0u, scratch, (u32)(s32)(s8)mem_read8(cpu, STAY_ROOM),
                                             scratch + 0x10u, 0u);
                t->pid_frame = slot_frame;
                if (t->pid == 0xFFFFFFFFu)
                    t->pid = 0u;
                else
                    fprintf(stderr, "[pvp] Dummy actor for player %d: process id %08X\n", t->id, t->pid);
            } else {
                mem_write32(cpu, scratch + 0x30u, 0u);
                call(co, FN_SEARCH_BY_ID, t->pid, scratch + 0x30u, 0, 0);
                const u32 actor = mem_read32(cpu, scratch + 0x30u);
                if (guest_ptr(actor) && mem_read32(cpu, actor + AC_PID) == t->pid) {
                    t->actor = actor;
                    fprintf(stderr, "[pvp] Dummy actor for player %d is at 0x%08X\n", t->id, actor);
                }
            }
        } else if (!here && t->pid != 0u && slot_frame - t->noted > 90u) {
            call(co, FN_DELETE_ACTOR, t->pid, 0, 0, 0);
            t->pid = t->actor = 0u;
        }
    }
}

// --------------------------------------------------------------- the frames ----

void bluewake_pvp_draw(CPUState* cpu, uint32_t scene_id, uint32_t menu_flag) {
    if (scene_id != g_scene) {
        forget_scene();
        g_scene = scene_id;
        g_scene_ms = now_ms();
    }
    ++g_frame;
    g_menu = menu_flag;
    const u32 player = mem_read32(cpu, PLAYER);
    if (g_block == 0u || g_broken || !guest_ptr(player))
        return;
    const double now = now_ms();
    lock();
    const int mode = g_mode;
    for (int i = 0; i < MAX_TARGETS; ++i) {
        Target* t = &g_target[i];
        if (t->id != 0 && t->pid == 0u && g_frame - t->noted > 600u) {
            memset(t, 0, sizeof *t); // gone for ten seconds, its Dummy actor given back: the slot is free
            continue;
        }
        if (t->id <= 0 || t->cyl == 0u || !t->tg_armed)
            continue;
        t->tg_armed = false;
        // The checks of this frame's draw have run: was the body hit by Link's attack?
        if ((mem_read32(cpu, t->cyl + O_TG_RPRM) & 1u) == 0u)
            continue;
        const u32 obj = mem_read32(cpu, t->cyl + O_TG_HIT);
        if (!guest_ptr(obj))
            continue;
        const u32 type = mem_read32(cpu, obj + O_AT_TYPE);
        const u32 atp = mem_read8(cpu, obj + O_AT_ATP);
        const u32 stts = mem_read32(cpu, obj + O_STTS);
        const u32 actor = guest_ptr(stts) ? mem_read32(cpu, stts + S_ACTOR) : 0u;
        // Link's own colliders (his sword), and the things he throws and shoots; not what a stage or an enemy hits with.
        if (actor != player && (type & AT_FROM_LINK) == 0u)
            continue;
        // A crouch stab (mine, still playing) is one hit per swing: the next swing needs only the short wait. Everything else
        // keeps the one wait of 700 ms for a player (an older build's too: its hits all count as one swing there).
        uint8_t swing_kind, swing_serial, swing_flags;
        bool stabbing;
        bluewake_swing_state(&swing_kind, &swing_serial, &swing_flags, &stabbing);
        double cooldown = HIT_COOLDOWN_MS;
        if (stabbing && t->hit_swing != (unsigned)swing_serial + 1u)
            cooldown = STAB_HIT_COOLDOWN_MS;
        if (mode == BLUEWAKE_PVP_OFF || !t->hit_ok || !safe_now(cpu) || now - t->last_hit_ms < cooldown)
            continue;
        t->last_hit_ms = now;
        t->hit_swing = stabbing ? (unsigned)swing_serial + 1u : 0u;
        if (g_out_n >= (int)(sizeof g_out / sizeof g_out[0]))
            continue;
        BluewakePvpHit* h = &g_out[g_out_n++];
        h->target = t->id;
        h->damage = atp == 0u ? 1 : (int)atp;
        h->type = type;
        for (int k = 0; k < 3; ++k)
            h->pos[k] = rd_f(cpu, player + PLAYER_POS + 4u * (u32)k);
        float dx = t->pos[0] - h->pos[0], dz = t->pos[2] - h->pos[2];
        const float len = sqrtf(dx * dx + dz * dz);
        if (len > 0.5f) {
            dx /= len;
            dz /= len;
        } else {
            dx = 0.0f;
            dz = 1.0f;
        }
        h->direction[0] = dx;
        h->direction[1] = dz;
        fprintf(stderr, "[pvp] my attack hit player %d: attack power %d, type %08X (attacker 0x%08X)\n", t->id, h->damage, type, actor);
    }
    unlock();
    // After a hit was applied to me: what Link's collider and his life did.
    if (g_watch > 0u) {
        --g_watch;
        const u32 link_cyl = player + PLAYER_CYL;
        const bool hit = (mem_read32(cpu, link_cyl + O_TG_RPRM) & 1u) != 0u;
        if (hit || g_watch == 0u || g_watch == 20u)
            fprintf(stderr, "[pvp] after the hit: Link's collider %s, life %u, frames left %u\n", hit ? "was hit" : "not hit",
                    (unsigned)mem_read16(cpu, SAVE_LIFE), (unsigned)g_watch);
    }
}

static void put_body(CPUState* cpu, Target* t, bool collision, bool tg) {
    const u32 cyl = t->cyl;
    wr_vec(cpu, cyl + C_CENTER, t->pos[0], t->pos[1], t->pos[2]);
    wr_f(cpu, cyl + C_RADIUS, (float)BODY_RADIUS_X);
    wr_f(cpu, cyl + C_HEIGHT, (float)BODY_HEIGHT_X);
    mem_write32(cpu, cyl + O_AT_SPRM, 0u);
    mem_write32(cpu, cyl + O_TG_SPRM, tg ? TG_SET | TG_IS_ENEMY | TG_IS_OTHER : 0u);
    mem_write32(cpu, cyl + O_CO_SPRM, collision ? CO_SET | CO_IS_PLAYER | CO_VS_ALL : 0u);
    wr_vec(cpu, t->stts + S_MOVE, 0.0f, 0.0f, 0.0f);
    if (collision || tg)
        ccs_set(cpu, cyl, false, tg, collision);
    t->tg_armed = tg;
}

// The hit on me, as an enemy's attack collider laid beside Link on the attacker's side for this frame.
static void put_attack(CPUState* cpu, u32 player, const Incoming* in, int mode, int scale) {
    const u32 cyl = g_block + BLOCK_AT, stts = cyl + BODY_STTS;
    float lx = rd_f(cpu, player + PLAYER_POS), ly = rd_f(cpu, player + PLAYER_POS + 4u), lz = rd_f(cpu, player + PLAYER_POS + 8u);
    float dx = lx - in->pos[0], dz = lz - in->pos[2];
    const float len = sqrtf(dx * dx + dz * dz);
    if (len > 0.5f) {
        dx /= len;
        dz /= len;
    } else {
        const s16 yaw = (s16)mem_read16(cpu, player + 0x20Eu); // shape_angle.y: he is knocked back from where he faces
        dx = -sinf((float)yaw * 3.14159265f / 32768.0f);
        dz = -cosf((float)yaw * 3.14159265f / 32768.0f);
    }
    // What damages Link: the attacker's power times the scale (no damage: none; a bomb is a fixed quarter heart in Link's own
    // code, so it is not passed on in that mode). The type is one Link's collider takes.
    int damage = mode == BLUEWAKE_PVP_NO_DAMAGE ? 0 : (in->damage * scale + 50) / 100;
    if (mode == BLUEWAKE_PVP_ON && damage < 1)
        damage = 1;
    u32 type = in->type & mem_read32(cpu, player + PLAYER_CYL + O_TG_TYPE);
    if (type == 0u || (mode == BLUEWAKE_PVP_NO_DAMAGE && (type & AT_BOMB) != 0u))
        type = AT_STALFOS_MACE;
    mem_write32(cpu, cyl + O_AT_SPRM, AT_SET | AT_VS_PLAYER);
    mem_write32(cpu, cyl + O_AT_TYPE, type);
    mem_write8(cpu, cyl + O_AT_ATP, (u8)(damage > 255 ? 255 : damage));
    mem_write32(cpu, cyl + O_TG_SPRM, 0u);
    mem_write32(cpu, cyl + O_CO_SPRM, 0u);
    wr_vec(cpu, cyl + G_AT_VEC, dx * HIT_PUSH, 0.0f, dz * HIT_PUSH);
    wr_vec(cpu, cyl + C_CENTER, lx - dx * 30.0f, ly, lz - dz * 30.0f);
    wr_f(cpu, cyl + C_RADIUS, (float)HIT_RADIUS);
    wr_f(cpu, cyl + C_HEIGHT, (float)BODY_HEIGHT_X);
    mem_write32(cpu, cyl + O_STTS, stts);
    ccs_set(cpu, cyl, true, false, false);
    fprintf(stderr, "[pvp] hit from player %d applied: power %d x %d%% -> %d, type %08X, life %u\n", in->from, in->damage, scale, damage,
            type, (unsigned)mem_read16(cpu, SAVE_LIFE));
    g_watch = 40u;
}

// Whether the puppets' bodies and attention points may be in the game's world now: no event runs (and none ended a
// moment ago), no scene change is pending, and the scene has been up for a few seconds. During the Ballad of Gales
// warp (an event with the cyclone catching Link and the ship) a body registered into the collision system stopped the
// event from ever ending (2026-10-19); a cutscene must not see them either.
static bool calm_now(CPUState* cpu) {
    const double now = now_ms();
    if (mem_read8(cpu, EVENT_MODE) != 0u || mem_read8(cpu, NEXT_STAGE_GO) != 0u)
        g_event_ms = now;
    const bool calm = now - g_event_ms >= EVENT_GRACE_MS && now - g_scene_ms >= SAFE_MS;
    if (calm != g_calm)
        fprintf(stderr, "[pvp] bodies and attention points %s (%s)\n", calm ? "back" : "held back",
                calm ? "calm" : mem_read8(cpu, EVENT_MODE) != 0u ? "an event runs" : "a scene change");
    g_calm = calm;
    return calm;
}

static void execute(CPUState* cpu) {
    if (g_scene == 0u || g_broken)
        return;
    const bool calm = calm_now(cpu);
    if (g_block == 0u)
        return;
    const u32 player = mem_read32(cpu, PLAYER);
    if (!guest_ptr(player))
        return;
    lock();
    const bool collision = g_collision;
    const int mode = g_mode, scale = g_scale;
    Incoming in;
    bool have_in = false;
    if (g_in_n > 0) {
        in = g_in[0];
        memmove(&g_in[0], &g_in[1], sizeof g_in[0] * (size_t)(--g_in_n));
        have_in = true;
    }
    for (int i = 0; i < MAX_TARGETS; ++i) {
        Target* t = &g_target[i];
        if (t->id <= 0 || t->cyl == 0u)
            continue;
        const bool here = g_frame - t->noted <= 2u;
        t->tg_armed = false;
        if (here && calm && !bluewake_carry_no_body(t->id))
            put_body(cpu, t, collision, mode != BLUEWAKE_PVP_OFF && t->hit_ok);
        // The attention point of its Dummy actor, where Z-targeting looks.
        if (t->actor != 0u) {
            if (!guest_ptr(t->actor) || mem_read32(cpu, t->actor + AC_PID) != t->pid) {
                t->actor = 0u; // the game removed it (the fiber asks for another)
                t->pid = 0u;
            } else if (!calm) {
                mem_write32(cpu, t->actor + AC_ATTN_FLAGS, 0u); // no attention point under an event or a scene change
            } else {
                const bool held = bluewake_carry_target(cpu, t->id, t->actor, here, t->pos); // the game holds it (puppet_carry.c)
                const bool on = here && (collision || mode != BLUEWAKE_PVP_OFF);
                const float y = t->pos[1];
                if (!held) {
                    wr_vec(cpu, t->actor + AC_CURRENT, t->pos[0], y, t->pos[2]);
                    wr_vec(cpu, t->actor + AC_OLD, t->pos[0], y, t->pos[2]);
                    wr_vec(cpu, t->actor + AC_HOME, t->pos[0], y, t->pos[2]);
                }
                wr_vec(cpu, t->actor + AC_EYE, t->pos[0], y + ATTN_HEIGHT, t->pos[2]);
                wr_vec(cpu, t->actor + AC_ATTN_POS, t->pos[0], y + ATTN_HEIGHT, t->pos[2]);
                wr_vec(cpu, t->actor + AC_SPEED, 0.0f, 0.0f, 0.0f);
                mem_write32(cpu, t->actor + AC_ATTN_FLAGS, (on ? ATTN_LOCKON_MISC : 0u) | bluewake_carry_attention(cpu, t->id, t->actor, here));
            }
        }
    }
    unlock();
    if (have_in) {
        if (mode == BLUEWAKE_PVP_OFF || !safe_now(cpu))
            fprintf(stderr, "[pvp] hit from player %d ignored (mode %d, %s)\n", in.from, mode, safe_now(cpu) ? "safe" : "not safe now");
        else
            put_attack(cpu, player, &in, mode, scale);
    }
}

// The play scene's process id as the execute hook last saw it (0: not yet): the draw hook's site has no register
// holding the scene.
static u32 g_exec_scene_id;
uint32_t bluewake_pvp_scene_id(void) { return g_exec_scene_id; }

u32 bluewake_pvp_execute_hook(CPUState* cpu) {
    const u32 scene = cpu->gpr[3];
    // The draw hook's scene id is not this one's (at that site r30 is not the play scene in every module build), so the
    // execute hook keeps its own: the first frame of a new play scene is skipped, the bodies follow the draw's frames.
    static u32 last_id;
    const u32 id = guest_ptr(scene) ? mem_read32(cpu, scene + 4u) : 0u;
    g_exec_scene_id = id;
    if (id != last_id) {
        last_id = id;
        return 0u;
    }
    if (id != 0u) {
        execute(cpu);
        bluewake_carry_execute(cpu, id); // the lifted player and the arcs of throws (puppet_carry.c)
    }
    return 0u;
}

void bluewake_pvp_test_execute(CPUState* cpu) {
    static uint32_t done;
    if (done == g_frame) // once a frame
        return;
    done = g_frame;
    execute(cpu);
}
