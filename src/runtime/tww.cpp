// The Wind Waker's own changes to its code (the runtime's game-specific part): what is patched in
// main.dol at startup and in each module when it links, and per frame on the game thread.
// (Widescreen, the frame rate and the enhancements land here as they come.)
#include "runtime/bod.h"
#include "runtime/gx.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace rt {

std::atomic<float> g_wideExtra{0.f};  // logical 640x480 units shown beyond each side of the 4:3 screen

// ---- bring-up diagnostics: heap allocations that fail (SA2_HEAP_LOG: every one) ----
namespace {
const bool s_heapLog = std::getenv("SA2_HEAP_LOG") != nullptr;
}
HLE(alloc__7JKRHeapFUli) {  // JKRHeap::alloc(this, size, align)
    const u32 heap = c.r[3], size = c.r[4], align = c.r[5], lr = c.lr;
    callOriginal(c, "alloc__7JKRHeapFUli");
    if (!c.r[3] || s_heapLog)
        log("[heap] %08X->alloc(%X, %d) = %08X lr %08X\n", heap, size, s32(align), c.r[3], lr);
}
HLE(create__10JKRExpHeapFUlP7JKRHeapb) {  // JKRExpHeap::create(size, parent, errorFlag)
    const u32 size = c.r[3], parent = c.r[4], lr = c.lr;
    callOriginal(c, "create__10JKRExpHeapFUlP7JKRHeapb");
    log("[heap] JKRExpHeap::create(%X, parent %08X) = %08X lr %08X\n", size, parent, c.r[3], lr);
}

// ---- in-between frames: what drew each moving value (src/runtime/gx.cpp's tags) ----
// The game draws its models from J3D's draw buffers, sorted (by depth, for some) anew every frame,
// so the order of draws says little about what drew them. Each model keeps its packets from frame
// to frame: a material packet (its material, with its shapes' packets) and a shape packet for each
// shape - named by those, a model's matrices pair with its own last frame's however the order goes.
HLE(draw__12J3DMatPacketFv) {
    static OriginalFn s_orig("draw__12J3DMatPacketFv");
    gxTaskEnter(c.r[3], 0x802DB494);
    s_orig(c);
    gxTaskLeave();
}
HLE(drawFast__14J3DShapePacketFv) {
    static OriginalFn s_orig("drawFast__14J3DShapePacketFv");
    gxTaskEnter(c.r[3], 0x802DB8F8);
    s_orig(c);
    gxTaskLeave();
}
HLE(draw__14J3DShapePacketFv) {
    static OriginalFn s_orig("draw__14J3DShapePacketFv");
    gxTaskEnter(c.r[3], 0x802DB890);
    s_orig(c);
    gxTaskLeave();
}
// A real-time shadow: the actor's silhouette rendered to a texture, projected onto the ground
// polygons under it. Those polygons are collected anew each frame (as many as are under it now):
// they stay put, only the projection follows the actor - so they are not blended.
HLE(draw__18dDlst_shadowReal_cFv) {
    static OriginalFn s_orig("draw__18dDlst_shadowReal_cFv");
    gxTaskEnter(c.r[3], 0x8008398C);
    gxStaticVerticesEnter();
    s_orig(c);
    gxStaticVerticesLeave();
    gxTaskLeave();
}
// A particle emitter's drawing (JPADraw is part of its emitter): its particles pair with that
// emitter's last frame's, not with every particle in the scene drawn with the same quad.
HLE(draw__7JPADrawFPA4_f) {
    static OriginalFn s_orig("draw__7JPADrawFPA4_f");
    gxTaskEnter(c.r[3], 0x80268634);
    s_orig(c);
    gxTaskLeave();
}
// Water effects drawn from their particles' positions (world space): the wave around a floating
// boat (a fan through each wave particle), its wake, ripples. Particles come and go every frame, so
// vertex i is not the same particle from one frame to the next - blended by index, the shape would
// bend between unrelated points. Not blended by index: gxinterp.cpp pairs the points of one frame with the last frame's by where
// they are in the world (the trail's points stay put as the boat moves on; the new end grows out of the old one's).
#define TWW_TRAIL_DRAW(name)                                    \
    HLE(name) {                                                 \
        static OriginalFn s_orig(#name);        \
        gxTrailVerticesEnter();                                 \
        s_orig(c);                                              \
        gxTrailVerticesLeave();                                 \
    }
TWW_TRAIL_DRAW(draw__17dPa_waveEcallBackFP14JPABaseEmitter)
TWW_TRAIL_DRAW(draw__18dPa_trackEcallBackFP14JPABaseEmitter)
TWW_TRAIL_DRAW(draw__19dPa_rippleEcallBackFP14JPABaseEmitter)
TWW_TRAIL_DRAW(draw__19dPa_ripplePcallBackFP14JPABaseEmitterP15JPABaseParticle)
#undef TWW_TRAIL_DRAW
// The sea (daSea, the open ocean's): a grid the game builds around the camera each frame, in world
// space, snapped to its cells - when the camera moves on, vertex i is another point of the sea, so
// it is not blended by index but as a height field: each point's height with the last frame's at the
// same x, z. (Its geometry goes into a display list the game plays later: gx.cpp carries the scope
// to that list's playback.)
HLE(draw__14daSea_packet_cFv) {
    static OriginalFn s_orig("draw__14daSea_packet_cFv");
    gxStaticVerticesEnter();
    gxHeightFieldEnter();
    s_orig(c);
    gxHeightFieldLeave();
    gxStaticVerticesLeave();
}
u64 viRetraces();  // hle_hw.cpp
// The frame's start: JFWDisplay waits here for the retrace that begins it (every second one at 30 fps).
// The menu's Restart (bluewake_request_soft_reset): what JUTGamePad does once B+X+Start has been held 0.5 s, at a frame's
// start - the reset noted for port 0 and the game's reset callback (mDoRst_resetCallBack) called. Holding the buttons
// for it worked, but the game took the B first and Link drew his sword (user 2026-10-25).
std::atomic<bool> g_softReset{false};
void softResetNow(Ctx& c) {
    constexpr u32 kResetOccurred = 0x803F7899, kResetPort = 0x803F789C, kCallback = 0x803F7884, kCallbackArg = 0x803F7888;
    if (rd8(kResetOccurred)) return;  // (one under way)
    wr8(kResetOccurred, 1);
    wr32(kResetPort, 0);
    const u32 callback = rd32(kCallback);
    log("[reset] the menu's Restart: the 3-button reset on port 0 (callback %08X)\n", callback);
    if (callback) guestCall(c, callback, {0u, rd32(kCallbackArg)});
}
HLE(waitForTick__FUlUs) {
    static OriginalFn s_orig("waitForTick__FUlUs");
    // BLUEWAKE_FRAME_STATS (testing): every 300 frames, how many retraces the frames took and the longest work between waits
    static const bool s_stats = std::getenv("BLUEWAKE_FRAME_STATS") != nullptr;
    static u64 s_lastEnd = 0, s_lastTick = 0, s_n = 0, s_hist[5] = {}, s_workMax = 0, s_waitMax = 0, s_workSum = 0;
    const u64 t0 = timebase();
    s_orig(c);
    gxFrameStarts();
    if (g_softReset.exchange(false)) softResetNow(c);
    if (!s_stats) return;
    const u64 t1 = timebase(), tick = viRetraces();
    if (s_lastEnd) {
        const u64 work = t0 - s_lastEnd, wait = t1 - t0;
        s_workMax = std::max(s_workMax, work), s_waitMax = std::max(s_waitMax, wait), s_workSum += work;
        s_hist[std::min<u64>(4, tick - s_lastTick)]++;
        if (++s_n == 300) {
            log("[frames] 300: retraces 1:%llu 2:%llu 3:%llu 4+:%llu; work avg %.2f max %.2f ms, wait max %.2f ms\n", s_hist[1],
                s_hist[2], s_hist[3], s_hist[4], s_workSum * 1000.0 / kTimebaseHz / 300, s_workMax * 1000.0 / kTimebaseHz,
                s_waitMax * 1000.0 / kTimebaseHz);
            s_n = 0, s_workMax = s_waitMax = s_workSum = 0;
            for (u64& h : s_hist) h = 0;
        }
    }
    s_lastEnd = t1, s_lastTick = tick;
}

void gameFixesInit() {}  // (Boat of Daphnes' settings are loaded first thing, in main)
void gameFixesLinked(u32 module) {
    bod::optionsLinked(module);
    bod::hostLinked(module);
}
void gameFixesUnlinked(u32 module) {
    bod::optionsUnlinked(module);
    bod::hostUnlinked(module);
}
void gameFixesFrame() {
    twwWideFrame();
    bod::optionsFrame();
}

}  // namespace rt

extern "C" void bluewake_request_soft_reset(void) { rt::g_softReset = true; }
