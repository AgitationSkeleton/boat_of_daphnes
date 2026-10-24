// The Tingle Tuner's Game Boy Advance inside the app: Boat of Daphnes' gba_link.c (mGBA running the free emibios,
// GPL-3.0-or-later; src/bod/runtime/host/src) joined to the game's serial interface as its host does (its main.c
// host_si_gba_transfer, host_gba_retrace). This runtime has no SI register model - the pads are read through PADRead -
// so the two SI calls the game's GBA library makes (SIGetTypeAsync and SITransfer, dolphin/gba/GBAXfer.c) are answered
// here for the GBA's channel: a GBA is there, and each transfer goes to the emulated GBA on the guest's clock, its
// callback run from the interrupt path as the SI's transfer-complete interrupt would.
#include "runtime/bod.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include "gba_link.h"  // src/bod/runtime/host/src

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

extern "C" int bluewake_gba_set_mode(int mode);

namespace rt {
u64 viRetraces();  // hle_hw.cpp
u16 padButtons(int port);  // frontend.cpp
}

namespace rt::bod {
namespace {
std::atomic<int> s_channel{-1};  // the SI channel the GBA is on (-1: none)
constexpr u32 kTypeGba = 0x00040000u;      // SI_GBA
constexpr u32 kErrorNoResponse = 0x0008u;  // SI_ERROR_NO_RESPONSE

u64 guestCycles() { return timebase() * 12; }  // (the time base is the CPU clock / 12)

// The GBA's buttons from its channel's pad, KEYINPUT order, as Dolphin maps a GameCube pad to the GBA:
// A B Select(Z) Start Right Left Up Down R L.
u16 gbaKeys(u16 b) {
    u16 keys = 0;
    if (b & 0x0100) keys |= 1u << 0;
    if (b & 0x0200) keys |= 1u << 1;
    if (b & 0x0010) keys |= 1u << 2;
    if (b & 0x1000) keys |= 1u << 3;
    if (b & 0x0002) keys |= 1u << 4;
    if (b & 0x0001) keys |= 1u << 5;
    if (b & 0x0008) keys |= 1u << 6;
    if (b & 0x0004) keys |= 1u << 7;
    if (b & 0x0020) keys |= 1u << 8;
    if (b & 0x0040) keys |= 1u << 9;
    return keys;
}
// A transfer in flight: one at a time (the game's GBA library has one per channel and waits for its callback).
struct Transfer {
    s32 chan;
    u32 input, inBytes, callback;
    u64 due;  // guest cycles
    int posted;
};

// The answer of a transfer (gba_link_transfer_end), written to the game's buffer and the callback run as an interrupt.
void finish(const Transfer& t) {
    u8 in[8] = {};
    const int got = t.posted ? gba_link_transfer_end(in) : 0;
    const u32 sr = got > 0 ? 0u : kErrorNoResponse;
    sched::postIrq([t, in, got, sr](Ctx& ic) {
        for (u32 i = 0; got > 0 && i < std::min<u32>(t.inBytes, u32(got)); i++) wr8(t.input + i, in[i]);
        if (!t.callback) return;
        ic.r[3] = u32(t.chan);
        ic.r[4] = sr;
        ic.r[5] = 0;  // (the interrupted context: the GBA library does not read it)
        ic.call(t.callback);
    });
}

std::mutex s_xferLock;
std::condition_variable s_xferWake;
std::optional<Transfer> s_xfer;
bool s_xferThread = false;

// The end of the wire time, on a host thread: sleeping most of the way, then spinning (a transfer is a few hundred
// microseconds; the Tuner's upload is thousands of them).
void xferMain() {
    std::unique_lock lk(s_xferLock);
    for (;;) {
        s_xferWake.wait(lk, [] { return s_xfer.has_value(); });
        const Transfer t = *s_xfer;
        for (;;) {
            const u64 now = guestCycles();
            if (now >= t.due) break;
            const u64 left = t.due - now;  // guest cycles (486 MHz)
            if (left > 486000 * 3) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::microseconds((left - 486000 * 2) / 486));
                lk.lock();
            } else {
                lk.unlock();
                std::this_thread::yield();
                lk.lock();
            }
        }
        s_xfer.reset();
        lk.unlock();
        finish(t);
        lk.lock();
    }
}

void schedule(const Transfer& t) {
    std::lock_guard lk(s_xferLock);
    if (!s_xferThread) {
        s_xferThread = true;
        std::thread(xferMain).detach();
    }
    s_xfer = t;
    s_xferWake.notify_one();
}

// BLUEWAKE_GBA_KEYS="retrace:buttons:frames,..." (testing): the GBA's pad (GameCube button bits) held from a retrace.
u16 scriptButtons() {
    static int count = -1;
    static struct { u64 at, len; u16 buttons; } presses[64];
    if (count < 0) {
        count = 0;
        const char* env = std::getenv("BLUEWAKE_GBA_KEYS");
        while (env && *env && count < 64) {
            char* end = nullptr;
            presses[count].at = std::strtoull(env, &end, 0);
            if (*end != ':') break;
            presses[count].buttons = u16(std::strtoul(end + 1, &end, 0));
            if (*end != ':') break;
            presses[count].len = std::strtoull(end + 1, &end, 0);
            count++;
            if (*end != ',') break;
            env = end + 1;
        }
    }
    const u64 now = viRetraces();
    u16 b = 0;
    for (int i = 0; i < count; i++)
        if (now >= presses[i].at && now < presses[i].at + presses[i].len) b |= presses[i].buttons;
    return b;
}
}  // namespace

// Each retrace, on the game thread: the GBA's keys, and its clock caught up to the guest's.
void gbaRetrace() {
    // BLUEWAKE_TINGLE_TUNER (the settings screen sets it from its Mode; the scenarios set it): inapp starts the GBA now,
    // auto when the save holds the Tingle Tuner (inventory slot 7, item 0x21; g_dComIfG_gameInfo + 0x3C), as the GBA
    // costs a core thread.
    static int s_auto = -1;  // 0 off, 1 auto, 2 inapp (now)
    if (s_auto < 0) {
        const char* m = std::getenv("BLUEWAKE_TINGLE_TUNER");
        s_auto = !m ? 0 : !std::strcmp(m, "inapp") ? 2 : !std::strcmp(m, "auto") ? 1 : 0;
    }
    if (s_auto && s_channel < 0 && (s_auto == 2 || ((viRetraces() & 15) == 0 && rd8(0x803C4C08 + 0x3C + 7) == 0x21))) {
        log("[gba] starting the in-app GBA (%s)\n", s_auto == 2 ? "asked for" : "the save has the Tingle Tuner");
        s_auto = 0;
        bluewake_gba_set_mode(1);
    }
    const int ch = s_channel;
    if (ch < 0 || !gba_link_running()) return;
    gba_link_set_keys(gbaKeys(padButtons(ch) | bod::scriptButtons()));
    gba_link_sync(guestCycles());
}

}  // namespace rt::bod

using namespace rt;

// SIProbe(chan) / SIGetType(chan): what the game's GBA thread asks first (JUTGba::gbaThread_Probe, _InitProbe). This
// runtime has no SI register model, so the real ones would answer "nothing there" (or "busy") for ever.
HLE(SIProbe) {
    if (s32(c.r[3]) != bod::s_channel || !gba_link_running()) return callOriginal(c, "SIProbe");
    c.r[3] = bod::kTypeGba;
}
HLE(SIGetType) {
    if (s32(c.r[3]) != bod::s_channel || !gba_link_running()) return callOriginal(c, "SIGetType");
    c.r[3] = bod::kTypeGba;
}

// SIGetTypeAsync(chan, callback): the GBA's channel answers that a GBA is there.
HLE(SIGetTypeAsync) {
    const s32 chan = s32(c.r[3]);
    const u32 callback = c.r[4];
    if (chan != bod::s_channel || !gba_link_running()) return callOriginal(c, "SIGetTypeAsync");
    if (callback)
        sched::postIrq([chan, callback](Ctx& ic) {
            ic.r[3] = u32(chan);
            ic.r[4] = bod::kTypeGba;
            ic.call(callback);
        });
    c.r[3] = bod::kTypeGba;
}

// SITransfer(chan, output, outputBytes, input, inputBytes, callback, delay): to the emulated GBA. The command goes to
// the GBA now; the answer is taken, and the callback run as the SI's transfer-complete interrupt, when the transfer's
// time on the wire is over (GC 200 kbit/s out, GBA 250 kbit/s back). Completing at once let the game send its next word
// every few microseconds while the emulated GBA, which is given the wire time for each, fell behind.
HLE(SITransfer) {
    const s32 chan = s32(c.r[3]);
    if (chan != bod::s_channel || !gba_link_running()) return callOriginal(c, "SITransfer");
    const u32 output = c.r[4], outBytes = c.r[5], input = c.r[6], inBytes = c.r[7], callback = c.r[8];
    u8 out[8] = {};
    const u32 n = std::min<u32>(outBytes, sizeof out);
    for (u32 i = 0; i < n; i++) out[i] = u8(rd8(output + i));
    const u64 now = bod::guestCycles();
    if (!gba_link_transfer_begin(now, out, n)) {
        bod::finish({chan, input, inBytes, callback, 0, 0});  // (no GBA: no response, at once)
    } else {
        bod::schedule({chan, input, inBytes, callback, now + gba_link_wire_cycles(out[0]), 1});
    }
    c.r[3] = 1;
}

// Its settings menu's Tingle Tuner mode: 1 the in-app GBA (on channel 1, the second controller port), else off. (Its
// "Real GBA" mode, a JoyBus bridge to a GBA cable, has no host here.)
extern "C" int bluewake_gba_set_mode(int mode) {
    if (mode == 1) {
        if (!gba_link_running() && !gba_link_start(std::getenv("BLUEWAKE_GBA_BIOS"))) {
            log("[gba] the in-app GBA did not start\n");
            return 0;
        }
        bod::s_channel = 1;
        log("[gba] the in-app GBA is on channel 1\n");
        return 1;
    }
    bod::s_channel = -1;
    gba_link_stop();
    return 0;
}
extern "C" int bluewake_gba_channel(void) { return bod::s_channel; }
