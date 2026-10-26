// See ui_pad.h.
#include "ui_pad.h"

#include <stdatomic.h>
#include <time.h>

static atomic_bool g_focus;
static atomic_bool g_hide_until_release;
static atomic_uint g_pressed;
static atomic_uint g_inject_buttons;
static atomic_uint g_inject_samples;
static unsigned g_previous;
static BluewakeUiPadGate g_gate;
static void* g_gate_user;
static atomic_bool g_axes_hide_until_centre;
static int g_axis_dir;  // the stick's direction bits at the last sample
static unsigned g_last_sample_id;
static atomic_uint g_hold_buttons;
static _Atomic long long g_hold_until_ms;

static long long now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint16_t bluewake_ui_pad_filter(uint16_t buttons) {
    unsigned raw = buttons;
    // The host reads a pad through more than one path per poll; an edge is a button down that was not down at the
    // previous sample, so extra samples of the same state add none.
    const unsigned edges = raw & ~g_previous;
    atomic_fetch_or(&g_pressed, edges);
    g_previous = raw;
    if (edges != 0u && g_gate != NULL && !atomic_load(&g_focus) && g_gate(edges, g_gate_user))
        atomic_store(&g_hide_until_release, true);
    // A hold (the menu's Restart: the game's B+X+Start reset) is exactly its buttons for its time, whatever else is held: the
    // game resets only on exactly that pattern held 0.5 s, and a count of samples ran out early where the host reads the
    // pad more than once a frame (only B came through: the sword was drawn; user 2026-10-25).
    if (atomic_load(&g_hold_until_ms) > now_ms())
        return (uint16_t)atomic_load(&g_hold_buttons);
    const unsigned inject = atomic_load(&g_inject_samples);
    if (inject > 0u) {
        raw |= atomic_load(&g_inject_buttons);
        atomic_store(&g_inject_samples, inject - 1u);
    }
    (void)g_last_sample_id;
    if (atomic_load(&g_focus))
        return 0u;
    if (atomic_load(&g_hide_until_release)) {
        if (buttons != 0u)
            return 0u;
        atomic_store(&g_hide_until_release, false);
    }
    return (uint16_t)raw;
}

uint16_t bluewake_ui_pad_take_pressed(void) { return (uint16_t)atomic_exchange(&g_pressed, 0u); }

void bluewake_ui_pad_set_focus(bool focus) {
    const bool was = atomic_exchange(&g_focus, focus);
    if (was && !focus)
        atomic_store(&g_hide_until_release, true);
}

bool bluewake_ui_pad_focused(void) { return atomic_load(&g_focus); }

void bluewake_ui_pad_inject(uint16_t buttons, unsigned samples) {
    atomic_store(&g_inject_buttons, buttons);
    atomic_store(&g_inject_samples, samples);
}

void bluewake_ui_pad_hold(uint16_t buttons, unsigned ms) {
    atomic_store(&g_hold_buttons, buttons);
    atomic_store(&g_hold_until_ms, now_ms() + (long long)ms);
}

void bluewake_ui_pad_set_gate(BluewakeUiPadGate gate, void* user) {
    g_gate_user = user;
    g_gate = gate;
}

void bluewake_ui_pad_filter_axes(int8_t* stick_x, int8_t* stick_y) {
    const int x = *stick_x, y = *stick_y;
    int dir = 0;
    if (x < -30) dir |= 0x1;   // left
    if (x > 30) dir |= 0x2;    // right
    if (y < -30) dir |= 0x4;   // down
    if (y > 30) dir |= 0x8;    // up
    atomic_fetch_or(&g_pressed, (unsigned)(dir & ~g_axis_dir));
    g_axis_dir = dir;
    if (atomic_load(&g_focus)) {
        atomic_store(&g_axes_hide_until_centre, true);
    } else if (atomic_load(&g_axes_hide_until_centre) && dir == 0 && x > -12 && x < 12 && y > -12 && y < 12) {
        atomic_store(&g_axes_hide_until_centre, false);
    }
    if (atomic_load(&g_axes_hide_until_centre)) {
        *stick_x = 0;
        *stick_y = 0;
    }
}
