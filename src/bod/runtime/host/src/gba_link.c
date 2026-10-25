// SPDX-License-Identifier: GPL-3.0-or-later
// See gba_link.h. Structure after Dolphin's HW::GBA::Core (GPL-2.0-or-later).
#include "gba_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

/* The worker's lock, wake-ups and thread: the Win32 calls on Windows, pthreads elsewhere. */
typedef CRITICAL_SECTION GlMutex;
typedef CONDITION_VARIABLE GlCond;
typedef HANDLE GlThread;
typedef DWORD GlWorkerResult;
#define GL_WORKER_CALL WINAPI
static void gl_lock_init(GlMutex* m) { InitializeCriticalSection(m); }
static void gl_cond_init(GlCond* c) { InitializeConditionVariable(c); }
static void gl_lock(GlMutex* m) { EnterCriticalSection(m); }
static void gl_unlock(GlMutex* m) { LeaveCriticalSection(m); }
static void gl_wait(GlCond* c, GlMutex* m) { SleepConditionVariableCS(c, m, INFINITE); }
static void gl_wake_all(GlCond* c) { WakeAllConditionVariable(c); }
static bool gl_thread_start(GlThread* t, GlWorkerResult(GL_WORKER_CALL* fn)(void*), void* arg) {
    *t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    return *t != NULL;
}
static void gl_thread_join(GlThread* t) {
    WaitForSingleObject(*t, INFINITE);
    CloseHandle(*t);
    *t = NULL;
}
#else
#include <pthread.h>

typedef pthread_mutex_t GlMutex;
typedef pthread_cond_t GlCond;
typedef pthread_t GlThread;
typedef void* GlWorkerResult;
#define GL_WORKER_CALL
static void gl_lock_init(GlMutex* m) { pthread_mutex_init(m, NULL); }
static void gl_cond_init(GlCond* c) { pthread_cond_init(c, NULL); }
static void gl_lock(GlMutex* m) { pthread_mutex_lock(m); }
static void gl_unlock(GlMutex* m) { pthread_mutex_unlock(m); }
static void gl_wait(GlCond* c, GlMutex* m) { pthread_cond_wait(c, m); }
static void gl_wake_all(GlCond* c) { pthread_cond_broadcast(c); }
static bool gl_thread_start(GlThread* t, GlWorkerResult(GL_WORKER_CALL* fn)(void*), void* arg) {
    return pthread_create(t, NULL, fn, arg) == 0;
}
static void gl_thread_join(GlThread* t) { pthread_join(*t, NULL); }
#endif

#include <mgba-util/vfs.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>

extern const unsigned char bluewake_emibios[];
extern const unsigned bluewake_emibios_size;

#define AUDIO_RING_FRAMES 16384u
#define GBA_AUDIO_BUFFER_SAMPLES 512

typedef struct GbaLink {
    struct mCore* core;
    struct GBASIODriver sio;
    struct mTimingEvent event;
    struct mAVStream stream;
    bool waiting; /* the sync event has not fired yet (worker thread only) */
    bool link_enabled; /* the GBA's serial port is in JoyBus mode (worker thread writes, anyone reads) */
    uint64_t last_gc; /* guest cycles the GBA has been run to (worker only) */
    uint64_t gc_remainder;
    uint32_t frequency; /* GBA cycles per second */
    uint32_t* video; /* the core's frame buffer, worker only */

    GlMutex lock;
    GlCond wake; /* worker waits for work */
    GlCond done; /* guest waits for a command's answer */
    GlThread thread;
    bool quit;
    bool started;
    /* work, under the lock */
    uint64_t sync_target; /* run to this guest time (0: nothing new) */
    uint16_t keys;
    bool cmd_pending;
    uint8_t cmd_buf[8];
    unsigned cmd_len;
    uint64_t cmd_at;
    unsigned cmd_transfer; /* guest cycles the transfer takes on the wire */
    uint8_t resp[8];
    unsigned resp_len;
    bool reset_pending;
    bool epoch_pending; /* started live: the GBA's clock begins at the guest's next call, not at guest time 0 */
    /* output, under the lock */
    uint8_t frame[2][GBA_LINK_WIDTH * GBA_LINK_HEIGHT * 4];
    int frame_front;
    uint32_t frame_seq;
    int16_t audio[AUDIO_RING_FRAMES * 2];
    size_t audio_head, audio_tail; /* frames, monotonically growing */
    unsigned audio_rate;
    GbaLinkStats stats;
} GbaLink;

static GbaLink g_gl;
static bool gl_log_enabled(void);

static void gl_logger_log(struct mLogger* l, int category, enum mLogLevel level, const char* fmt, va_list args) {
    (void)l;
    if (!gl_log_enabled() || (level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN | mLOG_STUB)) == 0)
        return;
    fprintf(stderr, "[mgba:%s] ", mLogCategoryId(category));
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}
static struct mLogger g_gl_logger = {.log = gl_logger_log};
static bool g_gl_inited;
static int g_gl_log = -1;

static bool gl_log_enabled(void) {
    if (g_gl_log < 0) {
        const char* e = getenv("BLUEWAKE_GBA_LOG");
        g_gl_log = (e != NULL && e[0] != '0') ? (e[0] == '2' ? 2 : 1) : 0;
    }
    return g_gl_log != 0;
}

/* ---- the worker's side: only this thread touches the core ---- */

static void gl_event_cb(struct mTiming* timing, void* context, uint32_t late) {
    (void)timing;
    (void)late;
    ((GbaLink*)context)->waiting = false;
}

static void gl_run_until(GbaLink* g, uint64_t gc) {
    if ((int64_t)(gc - g->last_gc) <= 0)
        return;
    mTimingSchedule(g->core->timing, &g->event,
                    (int32_t)((gc - g->last_gc) * g->frequency / GBA_LINK_GUEST_HZ));
    g->waiting = true;
    const int32_t begin = mTimingCurrentTime(g->core->timing);
    while (g->waiting)
        g->core->runLoop(g->core);
    const int32_t end = mTimingCurrentTime(g->core->timing);
    const uint64_t d = (uint64_t)(uint32_t)(end - begin) * GBA_LINK_GUEST_HZ + g->gc_remainder;
    g->last_gc += d / g->frequency;
    g->gc_remainder = d % g->frequency;
}

static bool gl_sio_init(struct GBASIODriver* d) { (void)d; return true; }
static void gl_sio_off(struct GBASIODriver* d) { g_gl.link_enabled = false; g_gl.stats.joybus = false; (void)d; }
static void gl_sio_set_mode(struct GBASIODriver* d, enum GBASIOMode mode) {
    (void)d;
    g_gl.link_enabled = mode == GBA_SIO_JOYBUS;
    g_gl.stats.joybus = g_gl.link_enabled;
    if (gl_log_enabled())
        fprintf(stderr, "[gba] sio mode %d%s\n", (int)mode, g_gl.link_enabled ? " (JoyBus)" : "");
}
static bool gl_sio_handles(struct GBASIODriver* d, enum GBASIOMode mode) { (void)d; return mode == GBA_SIO_JOYBUS; }
static int gl_sio_devices(struct GBASIODriver* d) { (void)d; return 1; }

static void gl_keys_read(void* ctx) {
    GbaLink* g = ctx;
    g->core->setKeys(g->core, g->keys);
}

static void gl_frame_ended(void* ctx) {
    GbaLink* g = ctx;
    gl_lock(&g->lock);
    const int back = g->frame_front ^ 1;
    uint8_t* dst = g->frame[back];
    const uint8_t* src = (const uint8_t*)g->video;
    for (unsigned i = 0; i < GBA_LINK_WIDTH * GBA_LINK_HEIGHT; i++) {
        dst[i * 4 + 0] = src[i * 4 + 0];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 2];
        dst[i * 4 + 3] = 0xFF;
    }
    g->frame_front = back;
    g->frame_seq++;
    g->stats.frames++;
    gl_unlock(&g->lock);
}

static void gl_audio_drain(GbaLink* g, struct mAudioBuffer* buf) {
    int16_t tmp[GBA_AUDIO_BUFFER_SAMPLES];
    const size_t want = GBA_AUDIO_BUFFER_SAMPLES / 2;
    for (;;) {
        const size_t n = mAudioBufferRead(buf, tmp, want);
        if (n == 0)
            break;
        gl_lock(&g->lock);
        for (size_t i = 0; i < n; i++) {
            if (g->audio_head - g->audio_tail >= AUDIO_RING_FRAMES)
                g->audio_tail++; /* overrun: drop the oldest */
            const size_t slot = g->audio_head % AUDIO_RING_FRAMES;
            g->audio[slot * 2] = tmp[i * 2];
            g->audio[slot * 2 + 1] = tmp[i * 2 + 1];
            g->audio_head++;
        }
        gl_unlock(&g->lock);
    }
}

static void gl_audio_rate(struct mAVStream* s, unsigned rate) {
    (void)s;
    struct mAudioBuffer* buf = g_gl.core->getAudioBuffer(g_gl.core);
    gl_audio_drain(&g_gl, buf);
    g_gl.audio_rate = rate;
}

static void gl_audio_post(struct mAVStream* s, struct mAudioBuffer* buf) {
    (void)s;
    gl_audio_drain(&g_gl, buf);
}

static void gl_video_dims(struct mAVStream* s, unsigned w, unsigned h) { (void)s; (void)w; (void)h; }

static bool gl_core_create(GbaLink* g, const char* bios_path) {
    g->core = mCoreCreate(mPLATFORM_GBA);
    if (!g->core) {
        fprintf(stderr, "[gba] mCoreCreate failed\n");
        return false;
    }
    g->core->init(g->core);
    mCoreInitConfig(g->core, "bluewake");
    mCoreConfigSetValue(&g->core->config, "idleOptimization", "detect");
    mCoreConfigSetIntValue(&g->core->config, "useBios", 0);
    mCoreConfigSetIntValue(&g->core->config, "skipBios", 0);

    struct VFile* vf = NULL;
    if (bios_path && bios_path[0])
        vf = VFileOpen(bios_path, O_RDONLY);
    if (!vf && bios_path && bios_path[0])
        fprintf(stderr, "[gba] cannot open BIOS %s, using the bundled one\n", bios_path);
    if (!vf) {
        /* The bundled emibios, with one value changed in a copy: its post-receive linger (the retail "about
           3 s" before the program takes over the port, "mb_boot_linger": push {r4,lr}; movs r4,#180) is 8
           frames. The game's link manager starts its handshake half a second after the upload and gives up
           after two tries, so the program has to be running by then. */
        static unsigned char patched[16384];
        memcpy(patched, bluewake_emibios, bluewake_emibios_size < sizeof patched ? bluewake_emibios_size : sizeof patched);
        static const unsigned char linger[] = {0x10, 0xb5, 0xb4, 0x24};
        if (bluewake_emibios_size == sizeof patched && memcmp(patched + 0x2e06, linger, sizeof linger) == 0) {
            const char* frames = getenv("BLUEWAKE_GBA_LINGER");
            patched[0x2e08] = (unsigned char)(frames ? atoi(frames) : 8);
        }
        vf = VFileFromConstMemory(patched, sizeof patched);
    }
    if (!vf || !g->core->loadBIOS(g->core, vf, 0)) {
        fprintf(stderr, "[gba] BIOS rejected\n");
        if (vf)
            vf->close(vf);
        mCoreConfigDeinit(&g->core->config);
        g->core->deinit(g->core);
        g->core = NULL;
        return false;
    }

    g->sio.init = gl_sio_init;
    g->sio.deinit = gl_sio_off;
    g->sio.reset = gl_sio_off;
    g->sio.setMode = gl_sio_set_mode;
    g->sio.handlesMode = gl_sio_handles;
    g->sio.connectedDevices = gl_sio_devices;
    GBASIOSetDriver(&((struct GBA*)g->core->board)->sio, &g->sio);

    unsigned w = 0, h = 0;
    g->core->currentVideoSize(g->core, &w, &h);
    g->video = calloc((size_t)w * h, sizeof(uint32_t));
    g->core->setVideoBuffer(g->core, (mColor*)g->video, w);
    g->core->setAudioBufferSize(g->core, GBA_AUDIO_BUFFER_SAMPLES);

    memset(&g->stream, 0, sizeof g->stream);
    g->stream.videoDimensionsChanged = gl_video_dims;
    g->stream.audioRateChanged = gl_audio_rate;
    g->stream.postAudioBuffer = gl_audio_post;
    g->core->setAVStream(g->core, &g->stream);

    struct mCoreCallbacks cb = {0};
    cb.context = g;
    cb.keysRead = gl_keys_read;
    cb.videoFrameEnded = gl_frame_ended;
    g->core->addCoreCallbacks(g->core, &cb);

    memset(&g->event, 0, sizeof g->event);
    g->event.context = g;
    g->event.name = "BlueWake sync";
    g->event.callback = gl_event_cb;
    g->event.priority = 0x80;

    g->frequency = (uint32_t)g->core->frequency(g->core);
    g->core->reset(g->core);
    return true;
}

static void gl_core_destroy(GbaLink* g) {
    if (g->core) {
        mCoreConfigDeinit(&g->core->config);
        g->core->deinit(g->core);
        g->core = NULL;
    }
    free(g->video);
    g->video = NULL;
}

/* BLUEWAKE_GBA_LOG: twice a guest second, where the GBA is; BLUEWAKE_GBA_DUMP=<file.ppm>: its screen, as it is then. */
static void gl_debug_tick(GbaLink* g) {
    static uint64_t next;
    if (!gl_log_enabled() && !getenv("BLUEWAKE_GBA_DUMP"))
        return;
    if (g->last_gc < next)
        return;
    next = g->last_gc + GBA_LINK_GUEST_HZ / 2;
    struct ARMCore* cpu = g->core->cpu;
    if (gl_log_enabled())
        fprintf(stderr, "[gba] pc=%08x cpsr=%08x dispcnt=%04x rcnt=%04x siocnt=%04x joycnt=%04x joystat=%04x ie=%04x t=%llu\n",
                cpu->gprs[15], cpu->cpsr.packed, g->core->busRead16(g->core, 0x4000000),
                g->core->busRead16(g->core, 0x4000134), g->core->busRead16(g->core, 0x4000128),
                g->core->busRead16(g->core, 0x4000140), g->core->busRead16(g->core, 0x4000158),
                g->core->busRead16(g->core, 0x4000200), (unsigned long long)g->last_gc);
    if (gl_log_enabled() && g_gl_log >= 2)
        fprintf(stderr, "[gba]   done=%08x phase=%u gate=%u off=%x trans=%08x recv=%08x\n",
                g->core->busRead32(g->core, 0x03007F08), g->core->busRead32(g->core, 0x0300170C),
                g->core->busRead32(g->core, 0x03001714), g->core->busRead32(g->core, 0x03001700),
                g->core->busRead32(g->core, 0x04000154), g->core->busRead32(g->core, 0x04000150));
    const char* dump = getenv("BLUEWAKE_GBA_DUMP");
    if (dump) {
        /* A "%u" in the name numbers the dumps (one per call, twice a guest second). */
        char numbered[1024];
        static unsigned dump_count;
        if (strstr(dump, "%u") != NULL) {
            snprintf(numbered, sizeof numbered, dump, dump_count++);
            dump = numbered;
        }
        FILE* f = fopen(dump, "wb");
        if (f) {
            fprintf(f, "P6\n240 160\n255\n");
            gl_lock(&g->lock);
            const uint8_t* px = g->frame[g->frame_front];
            for (unsigned i = 0; i < 240 * 160; i++)
                fwrite(px + i * 4, 1, 3, f);
            gl_unlock(&g->lock);
            fclose(f);
        }
    }
}

static GlWorkerResult GL_WORKER_CALL gl_worker(void* arg) {
    GbaLink* g = arg;
    gl_lock(&g->lock);
    for (;;) {
        while (!g->quit && !g->cmd_pending && g->sync_target == 0 && !g->reset_pending)
            gl_wait(&g->wake, &g->lock);
        if (g->quit)
            break;
        if (g->reset_pending) {
            g->reset_pending = false;
            gl_unlock(&g->lock);
            g->core->reset(g->core);
            gl_lock(&g->lock);
            continue;
        }
        if (g->cmd_pending) {
            const uint64_t at = g->cmd_at;
            const unsigned transfer = g->cmd_transfer;
            uint8_t buf[8];
            memcpy(buf, g->cmd_buf, sizeof buf);
            gl_unlock(&g->lock);
            gl_run_until(g, at);
            unsigned n = 0;
            /* The multiboot receiver in emibios cancels the pending boot when the GameCube sends a JoyBus RESET
               in its post-receive window (MB_DONE_MARK == 0xB7, still in BIOS code). The game's link manager
               sends one half a second after the upload, long before the program starts, so there a RESET is
               answered as a status poll: the program then starts and finds the link waiting for it. */
            if (buf[0] == 0xFF && g->core->busRead16(g->core, 0x03007F08) == 0x00B7 &&
                ((struct ARMCore*)g->core->cpu)->gprs[15] < 0x4000)
                buf[0] = 0x00;
            if (g->link_enabled)
                n = (unsigned)GBASIOJOYSendCommand(&g->sio, (enum GBASIOJOYCommand)buf[0], &buf[1]);
            gl_lock(&g->lock);
            g->resp_len = n;
            memcpy(g->resp, &buf[1], sizeof g->resp - 1);
            g->cmd_pending = false;
            gl_wake_all(&g->done);
            gl_unlock(&g->lock);
            gl_run_until(g, g->last_gc + transfer);
            gl_lock(&g->lock);
            continue;
        }
        const uint64_t target = g->sync_target;
        g->sync_target = 0;
        gl_unlock(&g->lock);
        gl_run_until(g, target);
        gl_debug_tick(g);
        gl_lock(&g->lock);
    }
    gl_unlock(&g->lock);
    return 0;
}

/* ---- the guest's side ---- */

bool gba_link_start(const char* bios_path) {
    GbaLink* g = &g_gl;
    if (g->started)
        return true;
    if (!g_gl_inited) {
        gl_lock_init(&g->lock);
        gl_cond_init(&g->wake);
        gl_cond_init(&g->done);
        g_gl_inited = true;
    }
    mLogSetDefaultLogger(&g_gl_logger);
    memset(&g->stats, 0, sizeof g->stats);
    g->last_gc = 0;
    g->gc_remainder = 0;
    g->epoch_pending = true;
    g->keys = 0;
    g->sync_target = 0;
    g->cmd_pending = false;
    g->frame_seq = 0;
    g->audio_head = g->audio_tail = 0;
    g->audio_rate = 32768;
    g->quit = false;
    g->link_enabled = false;
    if (!gl_core_create(g, bios_path))
        return false;
    if (!gl_thread_start(&g->thread, gl_worker, g)) {
        gl_core_destroy(g);
        return false;
    }
    g->started = true;
    g->stats.running = true;
    fprintf(stderr, "[gba] started (%u Hz)\n", g->frequency);
    return true;
}

void gba_link_stop(void) {
    GbaLink* g = &g_gl;
    if (!g->started)
        return;
    gl_lock(&g->lock);
    g->quit = true;
    gl_wake_all(&g->wake);
    gl_unlock(&g->lock);
    gl_thread_join(&g->thread);
    gl_lock(&g->lock);
    g->started = false; /* a guest transfer waiting for the worker gets "no response" */
    g->cmd_pending = false;
    g->resp_len = 0;
    gl_wake_all(&g->done);
    gl_unlock(&g->lock);
    gl_core_destroy(g);
    g->started = false;
    g->stats.running = false;
    g->stats.joybus = false;
}

bool gba_link_running(void) { return g_gl.started; }

void gba_link_reset(void) {
    GbaLink* g = &g_gl;
    if (!g->started)
        return;
    gl_lock(&g->lock);
    g->reset_pending = true;
    gl_wake_all(&g->wake);
    gl_unlock(&g->lock);
}

unsigned gba_link_wire_cycles(uint8_t command) {
    /* Dolphin SIDevice_GetGBATransferTime: the GC sends at 200 kbit/s, the GBA answers at 250 kbit/s, plus the stop bits. */
    unsigned gc_bytes = 1, gba_bytes = 1;
    switch (command) {
    case 0xFF:
    case 0x00: gba_bytes = 3; break;
    case 0x14: gba_bytes = 5; break;
    case 0x15: gc_bytes = 5; break;
    default: gba_bytes = 0; break;
    }
    const uint64_t hz = GBA_LINK_GUEST_HZ;
    return (unsigned)((gba_bytes * 8ull * hz / 250000ull) + (gc_bytes * 8ull * hz / 200000ull) +
                      (20500ull * hz / 1000000000ull));
}

/* The guest's side of a transfer, in two halves (the guest thread only): begin posts the command and returns at
   once; end, at the end of the transfer's wire time, takes the answer, waiting only if the worker has not reached it
   yet. Waiting for the answer at the start blocked the game for every word of the Tingle Tuner's stream while the
   GBA was run up to it: a tenth of the game thread in Windfall (__SITransfer), the game 29% slower per frame. */
static uint8_t g_gl_out[8];
static unsigned g_gl_out_len;
static uint64_t g_gl_out_at;
static bool g_gl_posted;

int gba_link_transfer_end(uint8_t* in);

bool gba_link_transfer_begin(uint64_t guest_cycles, const uint8_t* out, unsigned out_len) {
    GbaLink* g = &g_gl;
    if (g_gl_posted) {
        uint8_t dropped[8];
        (void)gba_link_transfer_end(dropped); /* a transfer started before the last one's answer was taken */
    }
    if (!g->started || out_len == 0)
        return false;
    const unsigned transfer = gba_link_wire_cycles(out[0]);
    memset(g_gl_out, 0, sizeof g_gl_out);
    memcpy(g_gl_out, out, out_len < sizeof g_gl_out ? out_len : sizeof g_gl_out);
    g_gl_out_len = out_len;
    g_gl_out_at = guest_cycles;
    gl_lock(&g->lock);
    if (g->epoch_pending) {
        g->epoch_pending = false;
        g->last_gc = guest_cycles;
    }
    memcpy(g->cmd_buf, g_gl_out, sizeof g->cmd_buf);
    g->cmd_len = out_len;
    g->cmd_at = guest_cycles;
    g->cmd_transfer = transfer;
    g->cmd_pending = true;
    gl_wake_all(&g->wake);
    gl_unlock(&g->lock);
    g_gl_posted = true;
    return true;
}

int gba_link_transfer_end(uint8_t* in) {
    GbaLink* g = &g_gl;
    if (!g_gl_posted)
        return 0;
    g_gl_posted = false;
    const uint8_t* out = g_gl_out;
    const unsigned out_len = g_gl_out_len;
    gl_lock(&g->lock);
    while (g->cmd_pending)
        gl_wait(&g->done, &g->lock);
    const unsigned n = g->resp_len;
    memcpy(in, g->resp, 7);
    g->stats.transfers++;
    if (n == 0)
        g->stats.no_response++;
    else if (n >= 3 && (out[0] == 0x00 || out[0] == 0xFF))
        g->stats.last_joystat = g->resp[2];
    gl_unlock(&g->lock);
    if (gl_log_enabled()) {
        /* Repeats of the same command and answer are folded into a count (BLUEWAKE_GBA_LOG=2: every transfer). */
        static uint8_t last[12];
        static unsigned repeats;
        uint8_t sig[12] = {out[0], 0, 0, 0, 0, (uint8_t)n, in[0], in[1], in[2], in[3], in[4], 0};
        if (g_gl_log == 2 || memcmp(sig, last, sizeof sig) != 0) {
            if (repeats)
                fprintf(stderr, "[gba]   (x%u more)\n", repeats);
            fprintf(stderr, "[gba] cmd %02x out=%02x %02x %02x %02x len=%u -> %u bytes %02x %02x %02x %02x %02x (t=%llu)\n", out[0],
                    out_len > 1 ? out[1] : 0, out_len > 2 ? out[2] : 0, out_len > 3 ? out[3] : 0, out_len > 4 ? out[4] : 0,
                    out_len, n, in[0], in[1], in[2], in[3], in[4], (unsigned long long)g_gl_out_at);
            memcpy(last, sig, sizeof sig);
            repeats = 0;
        } else {
            repeats++;
        }
    }
    return (int)n;
}

int gba_link_transfer(uint64_t guest_cycles, const uint8_t* out, unsigned out_len, uint8_t* in) {
    if (!gba_link_transfer_begin(guest_cycles, out, out_len))
        return 0;
    return gba_link_transfer_end(in);
}

void gba_link_sync(uint64_t guest_cycles) {
    GbaLink* g = &g_gl;
    if (!g->started)
        return;
    gl_lock(&g->lock);
    if (g->epoch_pending) {
        /* A GBA started live begins its clock now: running it from guest cycle 0 would be minutes of catch-up
           (and the 32-bit timing schedule overflows past about 70 guest seconds). */
        g->epoch_pending = false;
        g->last_gc = guest_cycles;
    }
    g->sync_target = guest_cycles ? guest_cycles : 1;
    gl_wake_all(&g->wake);
    gl_unlock(&g->lock);
}

void gba_link_set_keys(uint16_t keys) {
    static uint16_t logged;
    if (keys != logged && getenv("BLUEWAKE_GBA_KEYS_LOG") != NULL) {
        /* KEYINPUT order: A B Select Start Right Left Up Down R L (a set bit is a pressed key) */
        fprintf(stderr, "[gba] keys 0x%04x (was 0x%04x)\n", (unsigned)keys, (unsigned)logged);
    }
    logged = keys;
    g_gl.keys = keys; /* a 16-bit store; the worker reads it at the GBA's key poll */
}

uint32_t gba_link_copy_frame(uint8_t* rgba, uint32_t seen) {
    GbaLink* g = &g_gl;
    if (!g->started)
        return 0;
    gl_lock(&g->lock);
    const uint32_t seq = g->frame_seq;
    if (seq != 0 && seq != seen)
        memcpy(rgba, g->frame[g->frame_front], sizeof g->frame[0]);
    gl_unlock(&g->lock);
    return seq;
}

size_t gba_link_read_audio(int16_t* out, size_t max_frames) {
    GbaLink* g = &g_gl;
    if (!g->started)
        return 0;
    size_t n = 0;
    gl_lock(&g->lock);
    while (n < max_frames && g->audio_tail < g->audio_head) {
        const size_t slot = g->audio_tail % AUDIO_RING_FRAMES;
        out[n * 2] = g->audio[slot * 2];
        out[n * 2 + 1] = g->audio[slot * 2 + 1];
        g->audio_tail++;
        n++;
    }
    gl_unlock(&g->lock);
    return n;
}

unsigned gba_link_audio_rate(void) { return g_gl.audio_rate ? g_gl.audio_rate : 32768; }

void gba_link_stats(GbaLinkStats* out) {
    GbaLink* g = &g_gl;
    if (!g_gl_inited) {
        memset(out, 0, sizeof *out);
        return;
    }
    gl_lock(&g->lock);
    *out = g->stats;
    out->running = g->started;
    gl_unlock(&g->lock);
}
