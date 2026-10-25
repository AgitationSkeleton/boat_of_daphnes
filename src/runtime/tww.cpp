// The Wind Waker's own changes to its code (the runtime's game-specific part): what is patched in
// main.dol at startup and in each module when it links, and per frame on the game thread.
// (Widescreen, the frame rate and the enhancements land here as they come.)
#include "runtime/bod.h"
#include "runtime/gx.h"
#include "runtime/runtime.h"

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
    static const GuestFn s_orig = originalOf("draw__12J3DMatPacketFv");
    gxTaskEnter(c.r[3], 0x802DB494);
    s_orig(c);
    gxTaskLeave();
}
HLE(drawFast__14J3DShapePacketFv) {
    static const GuestFn s_orig = originalOf("drawFast__14J3DShapePacketFv");
    gxTaskEnter(c.r[3], 0x802DB8F8);
    s_orig(c);
    gxTaskLeave();
}
HLE(draw__14J3DShapePacketFv) {
    static const GuestFn s_orig = originalOf("draw__14J3DShapePacketFv");
    gxTaskEnter(c.r[3], 0x802DB890);
    s_orig(c);
    gxTaskLeave();
}
// A real-time shadow: the actor's silhouette rendered to a texture, projected onto the ground
// polygons under it. Those polygons are collected anew each frame (as many as are under it now):
// they stay put, only the projection follows the actor - so they are not blended.
HLE(draw__18dDlst_shadowReal_cFv) {
    static const GuestFn s_orig = originalOf("draw__18dDlst_shadowReal_cFv");
    gxTaskEnter(c.r[3], 0x8008398C);
    gxStaticVerticesEnter();
    s_orig(c);
    gxStaticVerticesLeave();
    gxTaskLeave();
}
// A particle emitter's drawing (JPADraw is part of its emitter): its particles pair with that
// emitter's last frame's, not with every particle in the scene drawn with the same quad.
HLE(draw__7JPADrawFPA4_f) {
    static const GuestFn s_orig = originalOf("draw__7JPADrawFPA4_f");
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
        static const GuestFn s_orig = originalOf(#name);        \
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
    static const GuestFn s_orig = originalOf("draw__14daSea_packet_cFv");
    gxStaticVerticesEnter();
    gxHeightFieldEnter();
    s_orig(c);
    gxHeightFieldLeave();
    gxStaticVerticesLeave();
}
// The frame's start: JFWDisplay waits here for the retrace that begins it (every second one at 30 fps).
HLE(waitForTick__FUlUs) {
    static const GuestFn s_orig = originalOf("waitForTick__FUlUs");
    s_orig(c);
    gxFrameStarts();
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
