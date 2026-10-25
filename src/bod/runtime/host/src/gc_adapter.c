#include "gc_adapter.h"

#include "gc_adapter_proto.h"
#include "pad_wire.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Everything below the lock is shared by the worker, the game thread (overlay, rumble) and the settings page.
static SDL_Mutex* g_lock;
static GcaState g_state;
static bool g_first_is_p1 = true;
static bool g_classic_on[GCA_PORTS]; // by channel
static bool g_enhanced_on;
static uint8_t g_rumble_target[GCA_RUMBLE_SIZE]; // what the ports should be doing
static uint8_t g_rumble_written[GCA_RUMBLE_SIZE]; // what the adapter was last told (all off after a start)
static BluewakeGcaStatus g_status = BLUEWAKE_GCA_NOT_FOUND;
static SDL_AtomicInt g_any; // a controller is on some port: the overlay has work
static bool g_started;

// The worker.
static SDL_Thread* g_thread;
static SDL_AtomicInt g_quit;

// The replay (testing).
static bool g_replay;
static GcaReplayEntry* g_entries;
static int g_entry_count;
static int g_entry_applied = -1;

static void lock(void) { SDL_LockMutex(g_lock); }
static void unlock(void) { SDL_UnlockMutex(g_lock); }

static void log_events(int events, const GcaState* state) {
    for (int p = 0; p < GCA_PORTS; p++) {
        if (events & (int)GCA_EVENT_CONNECT(p)) {
            const GcaPort* port = &state->port[p];
            fprintf(stderr, "[gcadapter] port %d: %s controller connected%s\n", p + 1,
                    port->type == GCA_WIRELESS ? "wireless" : "wired",
                    port->type == GCA_WIRELESS ? ""
                    : port->powered            ? " (rumble power on)"
                                               : " (no second USB cable: no rumble)");
        }
        if (events & (int)GCA_EVENT_DISCONNECT(p))
            fprintf(stderr, "[gcadapter] port %d: controller removed\n", p + 1);
    }
}

// Logs origins once they are taken (a port that connected with zeros waits for the next report).
static bool g_origin_logged[GCA_PORTS];
static void log_origins(const GcaState* state) {
    for (int p = 0; p < GCA_PORTS; p++) {
        const GcaPort* port = &state->port[p];
        if (port->type == GCA_NONE) {
            g_origin_logged[p] = false;
        } else if (port->have_origin && !g_origin_logged[p]) {
            g_origin_logged[p] = true;
            fprintf(stderr, "[gcadapter] port %d: origin stick %u,%u c-stick %u,%u triggers %u,%u\n", p + 1,
                    port->origin[0], port->origin[1], port->origin[2], port->origin[3], port->origin[4],
                    port->origin[5]);
        }
    }
}

// With the lock held: what the ports should be doing now. In replay the "write" is a log line.
static void retarget_locked(void) {
    int channel[GCA_PORTS];
    bool want[GCA_PORTS];
    gca_map_ports(&g_state, g_first_is_p1, channel);
    for (int p = 0; p < GCA_PORTS; p++)
        want[p] = g_classic_on[channel[p]] || g_enhanced_on;
    gca_build_rumble(&g_state, want, g_rumble_target);
    if (g_replay && memcmp(g_rumble_target, g_rumble_written, sizeof g_rumble_target) != 0) {
        memcpy(g_rumble_written, g_rumble_target, sizeof g_rumble_written);
        fprintf(stderr, "[gcadapter] rumble out: %02X %02X %02X %02X %02X\n", g_rumble_target[0], g_rumble_target[1],
                g_rumble_target[2], g_rumble_target[3], g_rumble_target[4]);
    }
}

static void publish_any_locked(void) {
    int any = 0;
    for (int p = 0; p < GCA_PORTS; p++)
        any |= g_state.port[p].type != GCA_NONE;
    SDL_SetAtomicInt(&g_any, any);
}

// --- the device ---------------------------------------------------------------------------------------------

static void close_device(SDL_hid_device* dev) {
    if (dev != NULL) {
        static const uint8_t off[GCA_RUMBLE_SIZE] = {GCA_CMD_RUMBLE, 0, 0, 0, 0};
        SDL_hid_write(dev, off, sizeof off);
        SDL_hid_close(dev);
    }
}

static bool start_polling(SDL_hid_device* dev) {
    const uint8_t init = GCA_CMD_INIT;
    if (SDL_hid_write(dev, &init, 1) != 1) {
        fprintf(stderr, "[gcadapter] could not start the adapter: %s\n", SDL_GetError());
        return false;
    }
    SDL_Delay(10); // the adapter needs a moment before its first report (SDL waits the same)
    return true;
}

static void set_status(BluewakeGcaStatus status) {
    lock();
    g_status = status;
    unlock();
}

static void adapter_lost(void) {
    lock();
    gca_reset(&g_state);
    memset(g_rumble_written, 0, sizeof g_rumble_written);
    memset(g_rumble_target, 0, sizeof g_rumble_target);
    g_rumble_target[0] = g_rumble_written[0] = GCA_CMD_RUMBLE;
    g_status = BLUEWAKE_GCA_NOT_FOUND;
    publish_any_locked();
    unlock();
}

static int SDLCALL worker(void* unused) {
    (void)unused;
    SDL_hid_device* dev = NULL;
    Uint32 last_count = 0;
    bool scanned = false;
    Uint64 next_scan = 0;
    Uint64 last_read = 0;
    BluewakeGcaStatus last_logged = (BluewakeGcaStatus)-1;
    // Not in the first seconds: SDL's own start-up (the window, the gamepad subsystem and its HID layer, on other threads)
    // races a first SDL_hid_* call from here, and 2 headless boots in 24 died in SDL3 (an access violation, a heap check).
    while (!SDL_GetAtomicInt(&g_quit) && SDL_GetTicks() < 4000)
        SDL_Delay(50);
    while (!SDL_GetAtomicInt(&g_quit)) {
        const Uint64 now = SDL_GetTicks();
        if (dev == NULL) {
            // The device list changes on plug and unplug (SDL counts arrivals); a program that held the adapter
            // when it was scanned gives no event when it lets go, so a refused open is tried again every 2 s,
            // and every 5 s whatever happens as a net.
            const Uint32 count = SDL_hid_device_change_count();
            if (!scanned || count != last_count || now >= next_scan) {
                scanned = true;
                last_count = count;
                next_scan = now + 5000;
                BluewakeGcaStatus status = BLUEWAKE_GCA_NOT_FOUND;
                SDL_hid_device_info* info = SDL_hid_enumerate(GCA_VENDOR_ID, GCA_PRODUCT_ID);
                if (info != NULL) {
                    dev = SDL_hid_open_path(info->path);
                    if (dev == NULL) {
                        status = BLUEWAKE_GCA_IN_USE;
                        next_scan = now + 2000;
                        if (last_logged != status)
                            fprintf(stderr, "[gcadapter] the adapter is listed but cannot be opened (in use by "
                                            "another program?): %s\n", SDL_GetError());
                    } else if (!start_polling(dev)) {
                        SDL_hid_close(dev);
                        dev = NULL;
                        status = BLUEWAKE_GCA_IN_USE;
                        next_scan = now + 2000;
                    } else {
                        status = BLUEWAKE_GCA_CONNECTED;
                        last_read = SDL_GetTicks();
                        fprintf(stderr, "[gcadapter] adapter opened and started\n");
                    }
                    SDL_hid_free_enumeration(info);
                } else if (last_logged != status) {
                    fprintf(stderr, "[gcadapter] no adapter found (not plugged in, or no WinUSB driver on it)\n");
                }
                last_logged = status;
                set_status(status);
            }
            if (dev == NULL)
                SDL_Delay(50);
            continue;
        }

        Uint8 buf[64];
        const int n = SDL_hid_read_timeout(dev, buf, sizeof buf, 10);
        if (n < 0) {
            fprintf(stderr, "[gcadapter] adapter lost: %s\n", SDL_GetError());
            SDL_hid_close(dev); // gone: nothing to write rumble to
            dev = NULL;
            scanned = false;
            last_logged = (BluewakeGcaStatus)-1;
            adapter_lost();
            continue;
        }
        if (n > 0) {
            lock();
            const int events = gca_parse_report(&g_state, buf, (size_t)n);
            if (events >= 0) {
                last_read = now;
                if (events != 0)
                    log_events(events, &g_state);
                log_origins(&g_state);
                publish_any_locked();
            }
            unlock();
        } else if (now - last_read > 500) {
            // Quiet for half a second: start it again (Dolphin does the same).
            last_read = now;
            fprintf(stderr, "[gcadapter] no reports for 500 ms; starting the adapter again\n");
            if (!start_polling(dev)) {
                SDL_hid_close(dev);
                dev = NULL;
                scanned = false;
                last_logged = (BluewakeGcaStatus)-1;
                adapter_lost();
                continue;
            }
        }
        lock();
        retarget_locked();
        uint8_t out[GCA_RUMBLE_SIZE];
        const bool send = memcmp(g_rumble_target, g_rumble_written, sizeof out) != 0;
        memcpy(out, g_rumble_target, sizeof out);
        unlock();
        if (send) {
            if (SDL_hid_write(dev, out, sizeof out) == (int)sizeof out) {
                lock();
                memcpy(g_rumble_written, out, sizeof out);
                unlock();
            }
        }
    }
    close_device(dev);
    return 0;
}

// --- the replay ---------------------------------------------------------------------------------------------

static bool load_replay(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "[gcadapter] replay file %s cannot be opened\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = (char*)malloc((size_t)size + 1);
    if (text == NULL || size < 0) {
        fclose(f);
        free(text);
        return false;
    }
    const size_t got = fread(text, 1, (size_t)size, f);
    text[got] = '\0';
    fclose(f);
    enum { kMax = 4096 };
    g_entries = (GcaReplayEntry*)calloc(kMax, sizeof *g_entries);
    char error[128];
    g_entry_count = g_entries != NULL ? gca_replay_parse(text, g_entries, kMax, error, sizeof error) : -1;
    free(text);
    if (g_entry_count < 0) {
        fprintf(stderr, "[gcadapter] replay file %s: %s\n", path, g_entries != NULL ? error : "out of memory");
        free(g_entries);
        g_entries = NULL;
        g_entry_count = 0;
        return false;
    }
    fprintf(stderr, "[gcadapter] replay: %d recorded reports from %s\n", g_entry_count, path);
    return true;
}

// With the lock held: the reports up to `retrace`, in order, as the device would have delivered them.
static void replay_advance_locked(uint64_t retrace) {
    const int target = gca_replay_index(g_entries, (size_t)g_entry_count, retrace);
    while (g_entry_applied < target) {
        const GcaReplayEntry* entry = &g_entries[++g_entry_applied];
        if (entry->unplug) {
            fprintf(stderr, "[gcadapter] replay retrace=%llu: adapter unplugged\n", (unsigned long long)entry->retrace);
            gca_reset(&g_state);
            g_status = BLUEWAKE_GCA_REPLAY;
            memset(g_origin_logged, 0, sizeof g_origin_logged);
        } else {
            const int events = gca_parse_report(&g_state, entry->report, GCA_REPORT_SIZE);
            if (events > 0) {
                fprintf(stderr, "[gcadapter] replay retrace=%llu:\n", (unsigned long long)entry->retrace);
                log_events(events, &g_state);
            }
            log_origins(&g_state);
        }
        publish_any_locked();
        retarget_locked();
    }
}

// --- the module ---------------------------------------------------------------------------------------------

void bluewake_gc_adapter_start(void) {
    if (g_started)
        return;
    const char* off = getenv("BLUEWAKE_GCADAPTER");
    if (off != NULL && off[0] == '0') {
        fprintf(stderr, "[gcadapter] off (BLUEWAKE_GCADAPTER=0)\n");
        return;
    }
    g_lock = SDL_CreateMutex();
    if (g_lock == NULL)
        return;
    gca_reset(&g_state);
    g_rumble_target[0] = g_rumble_written[0] = GCA_CMD_RUMBLE;
    g_started = true;
    const char* replay = getenv("BLUEWAKE_TEST_GCADAPTER_REPLAY");
    if (replay != NULL && replay[0] != '\0') {
        g_replay = load_replay(replay);
        if (g_replay) {
            g_status = BLUEWAKE_GCA_REPLAY;
            return;
        }
        // An unreadable replay file leaves the adapter module idle rather than reading a real device the test
        // did not ask about.
        g_started = false;
        return;
    }
    fprintf(stderr, "[gcadapter] native adapter input on (SDL hidapi, device %04X:%04X)\n", GCA_VENDOR_ID,
            GCA_PRODUCT_ID);
    g_thread = SDL_CreateThread(worker, "gc-adapter", NULL);
    if (g_thread == NULL)
        fprintf(stderr, "[gcadapter] could not start the reader thread: %s\n", SDL_GetError());
}

void bluewake_gc_adapter_stop(void) {
    if (!g_started)
        return;
    if (g_thread != NULL) {
        SDL_SetAtomicInt(&g_quit, 1);
        SDL_WaitThread(g_thread, NULL);
        g_thread = NULL;
    }
    g_started = false;
}

void bluewake_gc_adapter_set_first_is_p1(bool on) {
    if (!g_started)
        return;
    lock();
    g_first_is_p1 = on;
    unlock();
}

void bluewake_gc_adapter_overlay(DolPadState live[4], uint64_t retrace) {
    if (!g_started || (!g_replay && SDL_GetAtomicInt(&g_any) == 0))
        return;
    GcaState state;
    bool first;
    lock();
    if (g_replay)
        replay_advance_locked(retrace);
    state = g_state;
    first = g_first_is_p1;
    unlock();
    int channel[GCA_PORTS];
    gca_map_ports(&state, first, channel);
    const GcaConfig config = {0};
    for (int p = 0; p < GCA_PORTS; p++) {
        GcaPad pad;
        if (!gca_port_pad(&state, p, &config, &pad))
            continue;
        DolPadState adapter = {0};
        adapter.button = pad.buttons;
        adapter.stick_x = pad.stick_x;
        adapter.stick_y = pad.stick_y;
        adapter.substick_x = pad.c_x;
        adapter.substick_y = pad.c_y;
        adapter.trigger_left = pad.trigger_l;
        adapter.trigger_right = pad.trigger_r;
        DolPadState merged;
        bluewake_pad_merge(&live[channel[p]], &adapter, &merged);
        live[channel[p]] = merged;
    }
}

void bluewake_gc_adapter_rumble_classic(unsigned channel, unsigned motor) {
    if (!g_started || channel >= GCA_PORTS)
        return;
    lock();
    g_classic_on[channel] = motor == 1u; // 2 is the hard stop: off
    retarget_locked();
    unlock();
}

void bluewake_gc_adapter_rumble_enhanced(bool on) {
    if (!g_started)
        return;
    lock();
    if (g_enhanced_on != on) {
        g_enhanced_on = on;
        retarget_locked();
    }
    unlock();
}

BluewakeGcaStatus bluewake_gc_adapter_status(void) {
    if (!g_started)
        return BLUEWAKE_GCA_NOT_FOUND;
    lock();
    const BluewakeGcaStatus status = g_status;
    unlock();
    return status;
}

unsigned bluewake_gc_adapter_ports(void) {
    if (!g_started)
        return 0;
    unsigned mask = 0;
    lock();
    for (int p = 0; p < GCA_PORTS; p++)
        if (g_state.port[p].type != GCA_NONE)
            mask |= 1u << p;
    unlock();
    return mask;
}
