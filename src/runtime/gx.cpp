// GX bridge: the game's own (recompiled) GX library writes the hardware command stream to the
// write-gather pipe (0xCC008000). This file parses that stream command by command and hands it to
// Aurora's command processor, rewriting the commands that carry console addresses into Aurora's
// pointer-carrying GX_AURORA forms:
//   CP 0xA0-0xAF array bases      -> GX_AURORA_LOAD_ARRAYBASE (host pointer into guest memory)
//   BP image3 (texture address)   -> GX_AURORA_LOAD_TEXOBJ (content-hashed by Aurora)
//   BP 0x64/0x65 TLUT loads       -> GX_AURORA_LOAD_TLUT
//   BP 0x49/0x4A/0x4B/0x4D + 0x52 -> GX_AURORA_LOAD_COPY_SRC/DST/DEST + the copy
//   CALL_DL                       -> the display list's commands, inline
// Draw-done (BP 0x45) and token (BP 0x47/0x48) raise the PE interrupts as soon as they are written.
// While the CPU FIFO is redirected away from the GP FIFO (GXBeginDisplayList), writes go to guest
// memory at the PI write pointer instead, like the hardware.
// Frames are handed to the main (render) thread at VISetNextFrameBuffer, each standing alone: it
// starts with the whole GX state as it began (a preamble), and records the values that move from
// frame to frame, named by the game object that drew them, so the renderer can draw in-between
// frames (src/runtime/gxinterp.cpp).
#include "runtime/gx.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <dolphin/gx.h>


#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

namespace rt {

const u8* (*g_texSubstitute)(u32 addr, u32 format, u32 width, u32 height) = nullptr;

extern std::atomic<u64> g_relLinkFrame[kMaxModules];  // src/runtime/dispatch.cpp
u32 mmioGet16(u32 off);
u32 mmioGet32(u32 off);
u64 profileGuestCycles();  // src/runtime/profile.cpp
void profileLogThreads(double hz);
double viLastRetrace();  // src/runtime/hle_hw.cpp
double s_frameStartVi = 0;  // the retrace that started the frame being built (gxFrameStarts)
int s_staticVertices = 0;   // gxStaticVerticesEnter depth
int s_trailVertices = 0;    // gxTrailVerticesEnter depth
int s_heightField = 0;      // gxHeightFieldEnter depth
u64 viRetraces();
inline u64 viRetraceCount() { return viRetraces(); }
void mmioSet16(u32 off, u32 v);

namespace {
enum : u8 {
    CMD_NOP = 0x00,
    CMD_LOAD_CP = 0x08,
    CMD_LOAD_XF = 0x10,
    CMD_INDX_A = 0x20,
    CMD_INDX_B = 0x28,
    CMD_INDX_C = 0x30,
    CMD_INDX_D = 0x38,
    CMD_CALL_DL = 0x40,
    CMD_METRICS = 0x44,
    CMD_INVAL_VC = 0x48,
    CMD_AURORA = 0x50,
    CMD_LOAD_BP = 0x61,
};
enum : u16 {
    AUR_ARRAYBASE = 0x0010,
    AUR_LOAD_TEXOBJ = 0x0030,
    AUR_LOAD_TLUT = 0x0031,
    AUR_COPY_SRC = 0x0035,
    AUR_COPY_DST = 0x0036,
    AUR_COPY_DEST = 0x0037,
};
enum : u32 { IRQ_PE_TOKEN = 18, IRQ_PE_FINISH = 19 };

// ---- output stream (Aurora format, big-endian) ----
std::vector<u8> s_out;
void put8(u32 v) { s_out.push_back(u8(v)); }
void put16(u32 v) {
    s_out.push_back(u8(v >> 8));
    s_out.push_back(u8(v));
}
void put32(u32 v) {
    s_out.push_back(u8(v >> 24));
    s_out.push_back(u8(v >> 16));
    s_out.push_back(u8(v >> 8));
    s_out.push_back(u8(v));
}
void put64(u64 v) {
    put32(u32(v >> 32));
    put32(u32(v));
}
void putBytes(const u8* p, size_t n) { s_out.insert(s_out.end(), p, p + n); }
void aurora(u16 sub) {
    put8(CMD_AURORA);
    put16(sub);
}
u64 hostAddr(u32 physAddr) { return u64(reinterpret_cast<uintptr_t>(hostp(0x80000000u | (physAddr & 0x01FFFFFF)))); }

// ---- CP vertex format state (for command lengths) ----
u32 s_vcdLo = 0, s_vcdHi = 0, s_vatA[8], s_vatB[8], s_vatC[8];
const u8 kComp[8] = {1, 1, 2, 2, 4, 0, 0, 0};
const u8 kClr[8] = {2, 3, 4, 2, 3, 4, 0, 0};
u32 vertexSize(u32 fmt) {
    const u32 lo = s_vcdLo, hi = s_vcdHi, a = s_vatA[fmt], b = s_vatB[fmt], c = s_vatC[fmt];
    u32 size = 0;
    for (u32 i = 0; i < 9; i++) size += (lo >> i) & 1;  // position/normal + 8 texture matrix indices
    auto attr = [&](u32 type, u32 direct) { size += type == 1 ? direct : type == 2 ? 1 : type == 3 ? 2 : 0; };
    attr((lo >> 9) & 3, ((a & 1) ? 3 : 2) * kComp[(a >> 1) & 7]);
    if (u32 t = (lo >> 11) & 3) {
        const bool nbt = (a >> 9) & 1;
        if (t == 1) size += (nbt ? 9 : 3) * kComp[(a >> 10) & 7];
        else size += (t == 2 ? 1 : 2) * ((nbt && (a >> 31)) ? 3 : 1);
    }
    attr((lo >> 13) & 3, kClr[(a >> 14) & 7]);
    attr((lo >> 15) & 3, kClr[(a >> 18) & 7]);
    const u32 tex[8][2] = {{(a >> 21) & 1, (a >> 22) & 7}, {b & 1, (b >> 1) & 7},          {(b >> 9) & 1, (b >> 10) & 7},
                           {(b >> 18) & 1, (b >> 19) & 7}, {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
                           {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7}};
    for (u32 i = 0; i < 8; i++) attr((hi >> (2 * i)) & 3, (tex[i][0] ? 2 : 1) * kComp[tex[i][1]]);
    return size;
}

u32 s_arraysSet = 0;  // CP arrays whose base has been loaded (bit = array index)

// Debugging: SA2_SKIP_DRAWS=i,j-k,... drops those draws (counted from 0 in every frame).
u32 s_drawIndex = 0;
const std::set<u32> s_skipDraws = [] {
    std::set<u32> s;
    for (const char* p = std::getenv("SA2_SKIP_DRAWS"); p && *p;) {
        char* end;
        const u32 from = u32(std::strtoul(p, &end, 10));
        u32 to = from;
        if (*end == '-') to = u32(std::strtoul(end + 1, &end, 10));
        for (u32 i = from; i <= to && s.size() < 1000000; i++) s.insert(i);
        if (end == p) break;
        p = *end == ',' ? end + 1 : end;
    }
    return s;
}();

// Debugging: SA2_SKIP_BLEND=hex,... drops draws made with those (effective) BP 0x41 blend modes.
const std::set<u32> s_skipBlend = [] {
    std::set<u32> s;
    for (const char* p = std::getenv("SA2_SKIP_BLEND"); p && *p;) {
        char* end;
        s.insert(u32(std::strtoul(p, &end, 16)));
        if (end == p) break;
        p = *end == ',' ? end + 1 : end;
    }
    return s;
}();

// Debugging: SA2_SKIP_TEX=WxHfF,... drops draws whose texture map 0 is that size and format (hex).
const std::set<u32> s_skipTex = [] {
    std::set<u32> s;
    for (const char* p = std::getenv("SA2_SKIP_TEX"); p && *p;) {
        unsigned w = 0, h = 0, f = 0;
        int used = 0;
        if (std::sscanf(p, "%ux%uf%x%n", &w, &h, &f, &used) != 3) break;
        s.insert((w - 1) | (h - 1) << 10 | f << 20);
        p += used;
        if (*p == ',') p++;
    }
    return s;
}();

// Debugging: SA2_SKIP_TEXADDR=hex,... drops draws whose texture map 0 is at that address.
const std::set<u32> s_skipTexAddr = [] {
    std::set<u32> s;
    for (const char* p = std::getenv("SA2_SKIP_TEXADDR"); p && *p;) {
        char* end;
        s.insert(u32(std::strtoul(p, &end, 16)) & 0x01FFFFFF);
        if (end == p) break;
        p = *end == ',' ? end + 1 : end;
    }
    return s;
}();

// ---- snapshots ----
// Aurora reads array data when the render thread processes the frame, after the game has moved on
// (and, since draw-done is signalled at once, possibly rewritten its per-frame vertex and matrix
// buffers). So the arrays a draw uses are copied when the draw is issued, into memory that lives
// until the frame has been rendered.
struct Arena {
    std::vector<std::unique_ptr<u8[]>> chunks;
    size_t used = 0, cap = 0;
    u8* alloc(size_t n) {
        n = (n + 31) & ~size_t(31);
        if (used + n > cap) {
            cap = std::max<size_t>(n, size_t(1) << 20);
            chunks.emplace_back(new u8[cap]);
            used = 0;
        }
        u8* p = chunks.back().get() + used;
        used += n;
        return p;
    }
};
Arena s_arena;  // snapshots of the frame being built
const bool s_noRecord = std::getenv("SA2_NO_RECORD") != nullptr;  // (measuring what recording costs)

u32 s_xf[0x1058];  // XF registers and matrix memory, as the game wrote them
u32 s_bp[256];     // BP registers, as the game wrote them

// ---- tags: what drew each moving value ----
// The task system's display loop (f_80071998, children f_80071900) calls each task's display
// function after storing it at r13-0x7878; Ctx::call sees a call of that function and reports the
// task (r3) here. A value is named by the task, its display function, which time this frame the
// task is displayed, the viewport (split screen draws the world twice), and what it is used to draw:
// the model part of the draw that follows it (its display list, or its position array and first
// vertex), and which time in the task that part is drawn. Model parts the CPU builds every frame
// (sprites, skinned meshes) have no lasting address: those values are named by their place in
// what the task drew instead (also every value's fallback name). Values drawn outside any task
// count as one task of their own.
struct TaskScope {
    u32 task, fn, occurrence;
    u32 seq[3];  // per kind group: XF loads, vertices sent with draws, vertex arrays
    std::unordered_map<u64, u32> parts;  // model part -> times drawn so far
};
std::vector<TaskScope> s_tasks;  // innermost last
TaskScope s_noTask{0, 0, 0, {0, 0, 0}, {}};
u64 viewKey() {  // the viewport and scissor box (split screen views differ in at least one)
    u64 h = 0;
    for (u32 a = 0x101A; a < 0x1020; a++) h = h * 0x100000001B3ull ^ s_xf[a];
    for (u32 r : {0x20u, 0x21u, 0x59u}) h = h * 0x100000001B3ull ^ s_bp[r];
    return h;
}
std::unordered_map<u64, u32> s_occurrences;  // task << 32 | fn -> times displayed this frame
u64 mix(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}
u64 scopeKey(const TaskScope& t) { return mix(mix(u64(t.task) << 32 | t.fn) ^ u64(t.occurrence) << 40 ^ viewKey()); }
u64 tag(int group, u32 detail) {
    TaskScope& t = s_tasks.empty() ? s_noTask : s_tasks.back();
    const u32 seq = t.seq[group]++;
    return mix(scopeKey(t) ^ (u64(group) << 32 | seq) ^ u64(detail) << 36);
}

std::shared_ptr<GxFrame> s_frame = std::make_shared<GxFrame>();  // the frame being built
std::vector<size_t> s_pendingXf;  // XF values loaded since the last draw (named by it)
std::vector<size_t> s_drawXf;     // those of the draw being translated
bool s_draw2D = false;            // the draw being translated is 2D (place2D)
u64 s_drawPart = 0;               // the draw being translated: its model part's name (0: none)
u64 s_drawGroup = 0;              // ... without the copy number
u32 s_drawOcc = 0;                // ... the copy number
u32 s_curDl = 0;                  // the display list being inlined (0: none)
// Display lists recorded under a scope (gxHeightFieldEnter...): played under it.
struct ScopedDl {
    u32 lo, hi;
    bool height, staticV;
};
std::vector<ScopedDl> s_scopedDls;  // lists recorded under a scope (the latest at each address)

// ---- vertex/matrix arrays ----
// The hardware has no array sizes, but Aurora copies each array a draw reads into a per-frame
// storage buffer, so a base is sent only once a draw uses it, as a snapshot of the extent the draws
// have reached so far; it is resent larger (doubling) when a later draw indexes further. Every frame
// starts over (s_arraySent = 0). A base the game sets again in the same frame - J3D sets its model's
// arrays before every shape - gets the snapshot it already has: copying them again for each shape
// cost the game thread and Aurora (which copies each new snapshot to the GPU) milliseconds a frame in
// busy scenes. (The hardware reads arrays when it runs the frame, after the CPU has written them.)
u32 s_arrayBase[16], s_arrayStride[16], s_arraySent[16];
struct ArraySnap {
    u8* copy;
    u32 size;
};
std::unordered_map<u64, ArraySnap> s_arraySnaps;  // (slot, stride, base) -> this frame's snapshot
u8* s_arrayCopy[16];   // the current base's snapshot (none yet: null)
u8* s_arrayShown[16];  // the snapshot Aurora has for the slot

u64 arraySnapKey(u32 i) { return u64(i) << 56 | u64(s_arrayStride[i]) << 32 | (s_arrayBase[i] & 0x01FFFFFF); }

// The slot's base or stride changed: its snapshot this frame, if it has one.
void arrayBaseChanged(u32 i) {
    const auto it = s_arraySnaps.find(arraySnapKey(i));
    s_arrayCopy[i] = it != s_arraySnaps.end() ? it->second.copy : nullptr;
    s_arraySent[i] = it != s_arraySnaps.end() ? it->second.size : 0;
}

// Points Aurora's slot at a snapshot.
void sendArray(u32 i, u8* copy, u32 size, u32 vat) {
    aurora(u16(AUR_ARRAYBASE | i));
    if (i == 0 && s_arrayStride[0] && !s_noRecord) {  // positions: the CPU rewrites some every frame (skinned models)
        GxPatch p{};
        p.kind = GxPatch::Array, p.at = u32(s_out.size()), p.count = size, p.stride = s_arrayStride[0], p.data = copy;
        p.fmt = u8((vat >> 1) & 7), p.comps = u8((vat & 1) ? 3 : 2), p.shift = u8((vat >> 4) & 31);
        p.key2 = tag(2, p.stride << 8 | p.fmt);
        p.key = s_drawPart ? mix(s_drawPart ^ 0xA77A7) : p.key2;
        p.group = s_drawGroup, p.occ = s_drawOcc;
        p.flat = s_staticVertices > 0 && !s_heightField;
        p.height = s_heightField > 0;
        s_frame->patches.push_back(p);
    }
    put64(u64(reinterpret_cast<uintptr_t>(copy)));
    put32(size);
    put8(0);  // big-endian
    s_arrayShown[i] = copy;
}

// `vat`: the draw's VAT A word (array 0, positions: what the renderer needs to blend them).
void needArray(u32 i, u32 bytes, u32 vat = 0) {
    if (bytes <= s_arraySent[i]) {
        if (s_arrayShown[i] != s_arrayCopy[i]) sendArray(i, s_arrayCopy[i], s_arraySent[i], vat);  // (back to it)
        return;
    }
    const u32 limit = kHostMemSize - (s_arrayBase[i] & 0x01FFFFFF);
    const u32 size = std::min((std::max(bytes, s_arraySent[i] * 2) + 63) & ~63u, limit);
    s_arraySent[i] = size;
    u8* copy = s_arena.alloc(size);
    std::memcpy(copy, hostp(0x80000000u | (s_arrayBase[i] & 0x01FFFFFF)), size);
    s_arrayCopy[i] = copy;
    s_arraySnaps[arraySnapKey(i)] = {copy, size};
    sendArray(i, copy, size, vat);
}

// Reads the indices of a draw's vertices and makes sure every array they index is sent big enough.
void sizeArrays(const u8* p, u32 count, u32 fmt) {
    const u32 lo = s_vcdLo, hi = s_vcdHi, a = s_vatA[fmt], b = s_vatB[fmt], c = s_vatC[fmt];
    struct Op {
        u8 skip, idxSize, array, elem;
    };
    Op ops[24];
    u32 nops = 0, mtx = 0;
    for (u32 i = 0; i < 9; i++) mtx += (lo >> i) & 1;
    if (mtx) ops[nops++] = {u8(mtx), 0, 0, 0};
    auto attr = [&](u32 type, u32 array, u32 elem, u32 indices = 1) {
        if (type == 1) ops[nops++] = {u8(elem), 0, 0, 0};
        else if (type >= 2)
            for (u32 k = 0; k < indices; k++) ops[nops++] = {0, u8(type == 2 ? 1 : 2), u8(array), u8(elem)};
    };
    attr((lo >> 9) & 3, 0, ((a & 1) ? 3 : 2) * kComp[(a >> 1) & 7]);
    const bool nbt = (a >> 9) & 1, nbt3 = nbt && (a >> 31);
    const u32 nrmType = (lo >> 11) & 3;
    if (nrmType == 1) attr(1, 1, (nbt ? 9 : 3) * kComp[(a >> 10) & 7]);
    else attr(nrmType, 1, (nbt && !nbt3 ? 9 : 3) * kComp[(a >> 10) & 7], nbt3 ? 3 : 1);
    attr((lo >> 13) & 3, 2, kClr[(a >> 14) & 7]);
    attr((lo >> 15) & 3, 3, kClr[(a >> 18) & 7]);
    const u32 tex[8][2] = {{(a >> 21) & 1, (a >> 22) & 7}, {b & 1, (b >> 1) & 7},          {(b >> 9) & 1, (b >> 10) & 7},
                           {(b >> 18) & 1, (b >> 19) & 7}, {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
                           {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7}};
    for (u32 i = 0; i < 8; i++) attr((hi >> (2 * i)) & 3, 4 + i, (tex[i][0] ? 2 : 1) * kComp[tex[i][1]]);

    static const bool s_check = std::getenv("SA2_GX_CHECK") != nullptr;
    if (s_check) {
        u32 total = 0;
        for (u32 k = 0; k < nops; k++) total += ops[k].skip ? ops[k].skip : ops[k].idxSize;
        static int s_bad = 0;
        if (total != vertexSize(fmt) && s_bad++ < 10)
            log("[gx] layout mismatch: ops %u bytes, vertexSize %u (fmt %u VCD %08X/%08X VAT %08X %08X %08X)\n", total,
                vertexSize(fmt), fmt, lo, hi, a, b, c);
    }
    s32 maxIdx[12];
    u32 elemOf[12] = {};
    for (s32& m : maxIdx) m = -1;
    for (u32 v = 0; v < count; v++) {
        for (u32 k = 0; k < nops; k++) {
            const Op& o = ops[k];
            if (o.skip) {
                p += o.skip;
                continue;
            }
            const u32 idx = o.idxSize == 1 ? p[0] : u32(p[0]) << 8 | p[1];
            p += o.idxSize;
            // An all-ones position index makes the hardware skip the vertex; it reads nothing.
            if (o.array == 0 && idx == (o.idxSize == 1 ? 0xFFu : 0xFFFFu)) continue;
            if (s32(idx) > maxIdx[o.array]) maxIdx[o.array] = s32(idx);
            elemOf[o.array] = o.elem;
        }
    }
    for (u32 i = 0; i < 12; i++)
        if (maxIdx[i] >= 0) needArray(i, u32(maxIdx[i]) * s_arrayStride[i] + elemOf[i], a);
}

// Indexed attributes in the current vertex descriptor whose array was never given a base.
u32 missingArrays() {
    u32 need = 0;
    for (u32 i = 0; i < 4; i++)  // position, normal, color0, color1
        if (((s_vcdLo >> (9 + 2 * i)) & 3) >= 2) need |= 1u << i;
    for (u32 i = 0; i < 8; i++)
        if (((s_vcdHi >> (2 * i)) & 3) >= 2) need |= 1u << (4 + i);
    return need & ~s_arraysSet;
}

// ---- BP state the translation needs ----
u32 s_texImage0[8], s_texMode1[8], s_tlutRegion[8];
// Textures are loaded when a draw uses them, with all their registers written: the SDK writes the
// address (image3) before the TLUT register, J3D's display lists write it before the size and format.
u32 s_texDirty = 0, s_texImage3[8];
u32 s_tlutSrc = 0;
u32 s_copySrcTL = 0, s_copySrcWH = 0, s_copyDest = 0, s_copyStride = 0;
u64 s_frames = 0;
u32 s_displayCopies = 0;  // display copies since the last frame handoff

int texMapOf(u32 reg, u32 base) {  // maps 0-3 at base 0x80.., 4-7 at 0xA0..
    if (reg >= 0x80 + base && reg < 0x84 + base) return int(reg - 0x80 - base);
    if (reg >= 0xA0 + base && reg < 0xA4 + base) return int(reg - 0xA0 - base + 4);
    return -1;
}

u32 texBytes(u32 fmt, u32 w, u32 h) {
    // Block sizes (width x height of a 32-byte tile) per GX texture format.
    u32 bw = 8, bh = 8, bytes = 32;
    switch (fmt) {
    case 0x0: case 0x8: case 0xE: bw = 8; bh = 8; break;          // I4, C4, CMPR
    case 0x1: case 0x2: case 0x9: bw = 8; bh = 4; break;          // I8, IA4, C8
    case 0x3: case 0x4: case 0x5: case 0xA: bw = 4; bh = 4; break; // IA8, RGB565, RGB5A3, C14X2
    case 0x6: bw = 4; bh = 4; bytes = 64; break;                   // RGBA8
    default: break;
    }
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

// TLUT slots: TMEM offsets (512-byte units past 0x80000) of the SDK's default regions, GX_TLUT0-15
// at 0xC0000 + 0x2000*n and GX_BIGTLUT0-1 at 0xE0000 + 0x8000*n, and J3D's own: the palette of texture map n at
// 0xF0000 + 0x2000*n (J3DTevs.cpp), slots 20-27. In the SDK's 0x8000 steps J3D's maps 0-3 were all one slot, so a
// material with two palettes drew one with the other's (the Helmaroc King's mask: user 2026-10-25).
constexpr u32 kTlutSlots = 32;  // Aurora's MaxTluts (patches/aurora/0009)
u32 tlutSlot(u32 tmem) {
    if (tmem >= 0x380) return 20 + (tmem - 0x380) / 0x10;
    if (tmem >= 0x300) return 16 + (tmem - 0x300) / 0x40;
    return tmem >= 0x200 ? (tmem - 0x200) / 0x10 : 0;
}
struct TlutLoad {
    u32 src = 0, count = 0;
};
TlutLoad s_tluts[kTlutSlots];

void emitTlut(u32 slot, u32 format) {
    if (slot >= kTlutSlots || !s_tluts[slot].count) return;
    aurora(AUR_LOAD_TLUT);
    put8(slot);
    put64(hostAddr(s_tluts[slot].src));
    put32(format);
    put16(s_tluts[slot].count);
    put32(0);  // object id 0: content-hashed
    put32(0);
}

void loadTexture(int map, u32 image3) {
    const u32 img0 = s_texImage0[map];
    const u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 0xF;
    const u32 maxLod = (s_texMode1[map] >> 8) & 0xFF;
    const u32 slot = tlutSlot(s_tlutRegion[map] & 0x3FF);
    if (fmt == 0x8 || fmt == 0x9 || fmt == 0xA) emitTlut(slot, (s_tlutRegion[map] >> 10) & 3);  // CI: palette + its format
    const u8* substitute = g_texSubstitute ? g_texSubstitute(0x80000000u | ((image3 & 0x00FFFFFF) << 5), fmt, w, h) : nullptr;
    aurora(AUR_LOAD_TEXOBJ);
    put8(u32(map));
    put64(substitute ? u64(reinterpret_cast<uintptr_t>(substitute)) : hostAddr((image3 & 0x00FFFFFF) << 5));
    put32(w);
    put32(h);
    put32(fmt);
    put32(slot);
    put8(maxLod > 0 ? 1 : 0);
    put32(0);  // object id 0: Aurora caches by content
    put32(0);
    put32(s_bp[0x79] & 0xFFFFFF);  // the texture variant (Boat of Daphnes' puppets write BP 0x79 before their draws)
    (void)texBytes;
}

// ---- widescreen: which commands draw to the screen, and where 2D draws sit ----
// Aurora (patches/aurora/0004) widens perspective views spanning the screen and keeps everything
// else at its aspect, moved toward the edge it is near. The bridge marks each stretch of commands
// between EFB copies as drawing to the screen (1) or into a texture (0: decided when the copy that
// ends it turns out to be a small texture copy - shadow and reflection passes), and classifies each
// 2D (orthographic) draw by where it lands on the 640-wide logical screen.
enum : u16 { AUR_WIDE_SEGMENT = 0x0060, AUR_WIDE_2D = 0x0061 };
size_t s_segmentAt = SIZE_MAX;  // offset in s_out of the current segment's mode byte
int s_last2D = -1;
u32 s_cpMtxA = 0;           // CP matrix index A: the default position matrix
u32 s_genMode = 0;          // BP GENMODE (number of textures in bits 0-3)
u32 s_tex0 = 0;             // address of the texture in map 0
std::set<u32> s_copyDests;  // texture copy destinations this frame (screen effects draw from them)

void beginSegment() {
    aurora(AUR_WIDE_SEGMENT);
    s_segmentAt = s_out.size();
    put8(1);
    s_last2D = -1;
}

float xff(u32 a) { return __builtin_bit_cast(float, s_xf[a]); }

// Where a draw lands, for widescreen placement: its screen extent (logical units) and eye-space
// depth range. SA2 draws its 2D (HUD, menus, title art) with Ninja's sprite routines under the
// game's perspective projection: flat quads in screen pixels, at a depth given by the sprite's
// priority, through Ninja's 2D matrix [-a 0 320a 0 | 0 a -240a 0 | 0 0 1 0], which takes pixels at
// any depth to the eye space that projects them back onto those pixels. Such sprites and
// orthographic draws are 2D: full-screen art and backdrop tiles cover the screen, fades stretch,
// the HUD goes to the edges, the rest stays centred. Everything else is 3D, placed by Aurora from
// its viewport.
enum : int { WIDE_CENTRE, WIDE_LEFT, WIDE_RIGHT, WIDE_STRETCH, WIDE_3D, WIDE_COVER };
std::set<float> s_backdropDepths[2];  // depths of backdrop tiles in the previous and this frame
std::vector<std::pair<u32, float>> s_bleed;  // the draw's vertices to move out: offset in its vertex data, new x
u32 s_zmode = 0;  // BP ZMODE (bit 0: depth test)
u64 s_wideLogFrame = ~0ull, s_wideLogTo = 0;  // one frame, or 2D draws over a range of frames

// Where the game's HUD is drawn from (return addresses in its call chain): its sprites move to the
// screen edges, everything else 2D stays centred. (These were SA2B's; The Wind Waker's are still
// to be found - until then none match and all 2D stays centred, laid out in 4:3.)
constexpr u32 kHudReturns[] = {0};
// The game's main display routines, seen from the code they call.
constexpr u32 kStageDisplayReturns[] = {0};
// HUD pieces drawn on their own, laid out against an edge's HUD in 4:3, moved with it as a whole.
struct AnchoredRoutine {
    u32 lo, hi;
    int place;
};
constexpr AnchoredRoutine kAnchoredRoutines[] = {{0, 0, WIDE_LEFT}};

// The HUD's placement of a 2D draw: 0 = not HUD (stays centred), -1 = by where it is, else that.
int drawingHud(float minx, float maxx) {
    const Ctx* c = sched::currentCtx();
    if (!c) return 0;
    u32 sp = c->r[1], chain[12] = {};
    int n = 0;
    bool inStage = false;
    for (; n < 12 && (sp >> 25) == 0x40; n++) {  // the back chain's LR save words
        if (!(sp = rd32(sp))) break;
        const u32 ret = chain[n] = rd32(sp + 4);
        if (!ret) break;  // the chain's end (0 is also the tables' "none yet")
        for (u32 r : kHudReturns)
            if (ret == r) return -1;
        for (const AnchoredRoutine& a : kAnchoredRoutines)
            if (ret >= a.lo && ret < a.hi) return a.place;
        for (u32 r : kStageDisplayReturns) inStage = inStage || ret == r;
    }
    // Other 2D in a stage (trick ratings, prompts, boss gauges...) stays centred; each distinct
    // drawing path is logged once, so the ones that belong at the edges can be found and added.
    if (inStage && n >= 2) {
        static std::set<u64> s_seen;
        if (s_seen.size() < 64 && s_seen.insert(u64(chain[0]) << 32 | chain[1]).second)
            log("[wide] in-stage 2D at x %.0f..%.0f drawn from %08X %08X %08X %08X\n", minx, maxx, chain[0], chain[1],
                n > 2 ? chain[2] : 0, n > 3 ? chain[3] : 0);
    }
    return 0;
}

bool ninja2DMatrix(u32 m) {
    const float a = xff(m + 5);
    if (a == 0.f || std::fabs(xff(m) + a) > 1e-4f * std::fabs(a) || xff(m + 10) != 1.f) return false;
    for (u32 z : {1u, 3u, 4u, 7u, 8u, 9u, 11u})
        if (xff(m + z) != 0.f) return false;
    return std::fabs(xff(m + 2) - 320.f * a) <= 1e-3f * std::fabs(320.f * a) &&
           std::fabs(xff(m + 6) + 240.f * a) <= 1e-3f * std::fabs(240.f * a);
}

void place2D(const u8* p, u32 count, u32 fmt) {
    const u32 lo = s_vcdLo, a = s_vatA[fmt];
    const u32 posType = (lo >> 9) & 3;
    if (!posType) return;
    const bool ortho = s_xf[0x1026] == 1;
    // Under a perspective projection, only Ninja's 2D matrix draws 2D: a draw whose first vertex
    // uses another matrix is 3D, without looking at its vertices.
    const bool logging = s_frames == s_wideLogFrame || (s_frames >= s_wideLogFrame && s_frames <= s_wideLogTo);
    if (!ortho && !logging) {
        const u32 m0 = ((lo & 1) ? p[0] : (s_cpMtxA & 0x3F)) * 4;
        if (m0 + 11 < 0x100 && !ninja2DMatrix(m0)) {
            if (s_last2D != WIDE_3D) {
                aurora(AUR_WIDE_2D);
                put8(u8(WIDE_3D));
                s_last2D = WIDE_3D;
            }
            return;
        }
    }
    u32 pre = 0;  // matrix index bytes before the position
    for (u32 i = 0; i < 9; i++) pre += (lo >> i) & 1;
    const u32 vsize = vertexSize(fmt);
    const u32 comps = (a & 1) ? 3 : 2, cfmt = (a >> 1) & 7, shift = (a >> 4) & 31;
    const float scale = 1.f / float(1u << shift);
    auto comp = [&](const u8* q, u32 i) -> float {
        switch (cfmt) {
        case 0: return q[i] * scale;
        case 1: return s8(q[i]) * scale;
        case 2: return u16(q[i * 2] << 8 | q[i * 2 + 1]) * scale;
        case 3: return s16(q[i * 2] << 8 | q[i * 2 + 1]) * scale;
        default: return __builtin_bit_cast(float, u32(q[i * 4]) << 24 | u32(q[i * 4 + 1]) << 16 | u32(q[i * 4 + 2]) << 8 | q[i * 4 + 3]);
        }
    };
    const float p0 = xff(0x1020), p1 = xff(0x1021), p2 = xff(0x1022), p3 = xff(0x1023);
    const float sx = xff(0x101A), ox = xff(0x101D) - 342.f, sy = xff(0x101B), oy = xff(0x101E) - 342.f;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, minz = 1e9f, maxz = -1e9f;
    bool ninja2D = true;
    s_bleed.clear();
    float vxScreen[256], vxPerUnit[256];  // each vertex's screen x, and screen units per unit of its x
    for (u32 v = 0; v < count && v < 256; v++) {
        vxScreen[v] = 1e9f, vxPerUnit[v] = 0.f;
        const u8* vp = p + v * vsize;
        const u32 m = ((lo & 1) ? vp[0] : (s_cpMtxA & 0x3F)) * 4;  // matrix rows are 4 words
        if (m + 11 >= 0x100) continue;
        const u8* pos = vp + pre;
        if (posType >= 2) {
            const u32 idx = posType == 2 ? pos[0] : u32(pos[0]) << 8 | pos[1];
            pos = hostp(0x80000000u | ((s_arrayBase[0] + idx * s_arrayStride[0]) & 0x01FFFFFF));
        }
        ninja2D = ninja2D && ninja2DMatrix(m);
        const float x = comp(pos, 0), y = comp(pos, 1), z = comps == 3 ? comp(pos, 2) : 0.f;
        const float ex = xff(m) * x + xff(m + 1) * y + xff(m + 2) * z + xff(m + 3);
        const float ey = xff(m + 4) * x + xff(m + 5) * y + xff(m + 6) * z + xff(m + 7);
        const float ez = xff(m + 8) * x + xff(m + 9) * y + xff(m + 10) * z + xff(m + 11);
        float ndcX, ndcY;
        if (ortho) ndcX = p0 * ex + p1, ndcY = p2 * ey + p3;
        else if (ez < -1e-6f) ndcX = (p0 * ex + p1 * ez) / -ez, ndcY = (p2 * ey + p3 * ez) / -ez;
        else continue;  // behind the eye
        const float screenX = ndcX * sx + ox, screenY = ndcY * sy + oy;
        vxScreen[v] = screenX;
        if (posType == 1 && cfmt == 4) vxPerUnit[v] = (ortho ? p0 : p0 / -ez) * xff(m) * sx;
        minx = std::min(minx, screenX);
        maxx = std::max(maxx, screenX);
        miny = std::min(miny, screenY);
        maxy = std::max(maxy, screenY);
        minz = std::min(minz, ez);
        maxz = std::max(maxz, ez);
    }
    int place = WIDE_3D;
    s_draw2D = ortho;
    if (minx <= maxx) {
        const bool flat = maxz - minz <= 1e-3f * std::max(1.f, -minz);
        const bool sprite = ortho || (flat && ninja2D);
        s_draw2D = sprite;
        const float w = maxx - minx, h = maxy - miny, cx = (minx + maxx) * 0.5f;
        if (sprite) {
            if (minx <= 16.f && maxx >= 624.f)  // full screen: art fills it, fades and screen effects stretch
                place = (s_genMode & 0xF) && !s_copyDests.count(s_tex0) ? WIDE_COVER : WIDE_STRETCH;
            // Backdrops: the farthest layers (sprite priority, as depth, 5000 or more). Nearer layers
            // carry things placed on them (the stage select's map and its stage icons) and stay
            // centred with the rest.
            else if (minz <= -5000.f && h >= 120.f && minx >= -1.f && maxx <= 641.f && (minx <= 1.f || maxx >= 639.f)) {
                place = WIDE_COVER;  // a tall piece against an edge: a tile of a backdrop
                s_backdropDepths[1].insert(minz);
            } else if (minz <= -5000.f && w >= 64.f && h >= 64.f && (s_backdropDepths[0].count(minz) || s_backdropDepths[1].count(minz)))
                place = WIDE_COVER;  // a tile inside a backdrop: Ninja draws them all at one priority (depth)
            else if (const int hud = drawingHud(minx, maxx); hud == 0) place = WIDE_CENTRE;
            else if (hud > 0) place = hud;
            else if (cx < 192.f) place = WIDE_LEFT;
            else if (cx > 448.f) place = WIDE_RIGHT;
            else place = WIDE_CENTRE;
            // A full-height panel that runs off the 4:3 screen's side (the stage title card's band)
            // runs off the wide screen's: its outer edge moves out by the width added there.
            const float extra = g_wideExtra.load();
            const bool offLeft = minx < -1.f && maxx < 320.f, offRight = maxx > 641.f && minx > 320.f;
            if (place == WIDE_CENTRE && extra > 0.f && miny <= 1.f && maxy >= 479.f && (offLeft || offRight) && posType == 1 &&
                cfmt == 4 && count <= 256) {
                const float edge = offLeft ? minx : maxx, by = offLeft ? -(extra + 2.f) : extra + 2.f;
                for (u32 v = 0; v < count; v++)
                    if (std::fabs(vxScreen[v] - edge) < 0.5f && std::fabs(vxPerUnit[v]) > 1e-6f) {
                        const float x = comp(p + v * vsize + pre, 0);
                        s_bleed.push_back({v * vsize + pre, x + by / vxPerUnit[v]});
                    }
            }
        }
        if (s_frames == s_wideLogFrame || (s_frames >= s_wideLogFrame && s_frames <= s_wideLogTo && flat)) {
            const u32 m = ((lo & 1) ? p[0] : (s_cpMtxA & 0x3F)) * 4;
            std::string chain;  // the guest code drawing it: LR, then the back chain's LR save words
            if (const Ctx* c = sched::currentCtx()) {
                char b[16];
                std::snprintf(b, sizeof b, " %08X", c->lr);
                chain += b;
                u32 sp = c->r[1];
                for (int i = 0; i < 8 && (sp >> 25) == 0x40; i++) {
                    if (!(sp = rd32(sp))) break;
                    std::snprintf(b, sizeof b, " %08X", rd32(sp + 4));
                    chain += b;
                }
            }
            log("[wide] %llu %s draw: x %.0f..%.0f y %.0f..%.0f z %.2f..%.2f, %u verts, mtx %u [%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g], zmode %x, tex %u @%08X%s -> %d; stack%s\n",
                (unsigned long long)s_frames, ortho ? "ortho" : "persp", minx, maxx, miny, maxy, minz, maxz, count, m / 4,
                xff(m), xff(m + 1), xff(m + 2), xff(m + 3), xff(m + 4), xff(m + 5), xff(m + 6), xff(m + 7), xff(m + 8),
                xff(m + 9), xff(m + 10), xff(m + 11), s_zmode, s_genMode & 0xF, s_tex0, s_copyDests.count(s_tex0) ? " (copy)" : "", place,
                chain.c_str());
        }
    }
    if (place != s_last2D) {
        aurora(AUR_WIDE_2D);
        put8(u8(place));
        s_last2D = place;
    }
}

// ---- state shadows (for each frame's preamble) ----
u32 s_bpMask = 0xFFFFFF;
std::bitset<256> s_bpSet;
u32 s_cp[256];
std::bitset<256> s_cpSet;
std::bitset<0x1058> s_xfSet;

// ---- texture copies the CPU reads (gxCopyReadbackArm) ----
// The game's screen capture (the Picto Box's photo) copies the EFB to RAM and then reads the picture. The copy
// stays on the GPU here; for these copies Aurora's read-back (GX_AURORA_READBACK_CAPTURE_COPIES) brings the
// bytes, laid out as GXCopyTex writes them, a few frames later. The draw-sync token that tells the game the copy
// is done is held until the bytes are in guest memory.
struct CopyWait {
    u64 key;
    u32 guest;  // physical destination
    u32 size;
    u8 fill;    // what the buffer gets if the picture never comes
    u32 fmt, w, h;
    double since;
};
std::mutex s_cwLock;
std::vector<CopyWait> s_cwPending;
std::deque<u16> s_cwHeld;  // tokens written while a copy is awaited, in order
std::deque<u16> s_cwOut;   // tokens released, as the interrupts will deliver them
bool s_cwArmed = false;
u64 s_cwKey = 0, s_cwSeq = 0, s_cwStream = 0;
constexpr double kCopyWaitSeconds = 0.6;  // (the game's own capture time-out is 1 s)

// The size of a copy's data in the texture format (GXGetTexBufferSize); 0 for formats that are not read back.
u32 copyBytes(u32 fmt, u32 w, u32 h) {
    static const u8 tw[7] = {8, 8, 8, 4, 4, 4, 4}, th[7] = {8, 4, 4, 4, 4, 4, 4}, tb[7] = {32, 32, 32, 32, 32, 32, 64};
    if (fmt > 6 || !w || !h) return 0;
    return (w + tw[fmt] - 1) / tw[fmt] * ((h + th[fmt] - 1) / th[fmt]) * tb[fmt];
}

void releaseHeldTokens() {
    std::deque<u16> held;
    {
        std::lock_guard lk(s_cwLock);
        held.swap(s_cwHeld);
        for (u16 t : held) s_cwOut.push_back(t);
    }
    for (size_t i = 0; i < held.size(); i++)
        raiseInterrupt(IRQ_PE_TOKEN, [] {
            std::lock_guard lk(s_cwLock);
            mmioSet16(0x100E, s_cwOut.front());
            s_cwOut.pop_front();
            mmioSet16(0x100A, mmioGet16(0x100A) | 0x4);
        });
}

// A token with interrupt (BP 0x48): held back while a copy the CPU reads is on its way. true: held.
bool holdToken(u16 token) {
    std::lock_guard lk(s_cwLock);
    if (s_cwPending.empty()) return false;
    s_cwHeld.push_back(token);
    return true;
}

// SA2_EFB_IMG=<dir> (testing): the delivered picture as a PPM (I8 as grey, RGB565, RGB5A3, RGBA8).
void dumpCopy(const CopyWait& cw, const u8* b) {
    const char* dir = std::getenv("SA2_EFB_IMG");
    if (!dir) return;
    static const u8 tw[7] = {8, 8, 8, 4, 4, 4, 4}, th[7] = {8, 4, 4, 4, 4, 4, 4};
    if (cw.fmt < 1 || cw.fmt == 2 || cw.fmt == 3) return;
    const u32 bw = tw[cw.fmt], bh = th[cw.fmt], tx = (cw.w + bw - 1) / bw;
    std::string path = std::string(dir) + "/capture_" + std::to_string(cw.key) + "_f" + std::to_string(cw.fmt) + ".ppm";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", cw.w, cw.h);
    for (u32 y = 0; y < cw.h; y++)
        for (u32 x = 0; x < cw.w; x++) {
            const u32 tile = (y / bh) * tx + x / bw, ix = x % bw, iy = y % bh;
            u8 rgb[3] = {0, 0, 0};
            if (cw.fmt == 1) rgb[0] = rgb[1] = rgb[2] = b[tile * 32 + iy * 8 + ix];
            else if (cw.fmt == 4 || cw.fmt == 5) {
                const u8* q = b + tile * 32 + (iy * 4 + ix) * 2;
                const u32 v = q[0] << 8 | q[1];
                if (cw.fmt == 4) rgb[0] = u8((v >> 11) * 255 / 31), rgb[1] = u8(((v >> 5) & 63) * 255 / 63), rgb[2] = u8((v & 31) * 255 / 31);
                else if (v & 0x8000) rgb[0] = u8(((v >> 10) & 31) * 255 / 31), rgb[1] = u8(((v >> 5) & 31) * 255 / 31), rgb[2] = u8((v & 31) * 255 / 31);
                else rgb[0] = u8(((v >> 8) & 15) * 17), rgb[1] = u8(((v >> 4) & 15) * 17), rgb[2] = u8((v & 15) * 17);
            } else if (cw.fmt == 6) {
                const u8* q = b + tile * 64 + (iy * 4 + ix) * 2;
                rgb[0] = q[1], rgb[1] = q[32], rgb[2] = q[33];
            }
            std::fwrite(rgb, 1, 3, f);
        }
    std::fclose(f);
}

bool redirectedToMemory();

// Records the read-back of the texture copy just written to `dest` (host key = Aurora's copy destination).
void captureCopy(u32 destPhys, u32 fmt, u32 w, u32 h) {
    const u32 size = copyBytes(fmt, w, h);
    if (!size || !s_frame || redirectedToMemory()) return;
    if (!s_cwStream) s_cwStream = AuroraReadbackCreateStream();
    if (!s_cwStream) return;
    CopyWait cw{++s_cwKey, destPhys, size, u8(0x80), fmt, w, h, gxNow()};
    {
        std::lock_guard lk(s_cwLock);
        s_cwPending.push_back(cw);
    }
    aurora(0x0051);  // GX_AURORA_READBACK_CAPTURE_COPIES
    put64(s_cwStream);
    put64(++s_cwSeq);
    put32(1);
    put64(cw.key);
    put64(hostAddr(destPhys));
    if (std::getenv("SA2_EFB_LOG")) log("[efb] screen capture copy: format %u, %u x %u, %u bytes at %08X\n", fmt, w, h, size, 0x80000000u | destPhys);
}

void translateBp(u32 v) {
    const u32 reg = v >> 24, val = v & 0xFFFFFF;
    if (reg == 0xFE) s_bpMask = val;
    else {
        s_bp[reg] = (s_bp[reg] & ~s_bpMask) | (val & s_bpMask);
        s_bpSet.set(reg);
        s_bpMask = 0xFFFFFF;
    }
    int map;
    if (reg == 0x00) s_genMode = val;
    if (reg == 0x40) s_zmode = val;
    if (reg >= 0x80 && reg < 0xC0) {  // texture registers
        if (texMapOf(reg, 0x14) == 0) s_tex0 = (s_bp[reg] & 0xFFFFFF) << 5;
        if ((map = texMapOf(reg, 0x08)) >= 0) s_texImage0[map] = s_bp[reg];  // (as masked: J3D writes some masked)
        if ((map = texMapOf(reg, 0x04)) >= 0) s_texMode1[map] = s_bp[reg];
        if ((map = texMapOf(reg, 0x18)) >= 0) s_tlutRegion[map] = s_bp[reg];
        if ((map = texMapOf(reg, 0x14)) >= 0) {  // image3: the texture's address
            s_texImage3[map] = s_bp[reg];
            s_texDirty |= 1u << map;
        }
        // A change to a loaded map's size, format, LOD range or palette reloads it at its next draw.
        if ((map = texMapOf(reg, 0x08)) >= 0 || (map = texMapOf(reg, 0x04)) >= 0 || (map = texMapOf(reg, 0x18)) >= 0)
            if (s_texImage3[map]) s_texDirty |= 1u << map;
    }
    switch (reg) {
    case 0x45:  // PE_DONE: everything before it has been "drawn"
        raiseInterrupt(IRQ_PE_FINISH, [] { mmioSet16(0x100A, mmioGet16(0x100A) | 0x8); });
        break;
    case 0x47:  // PE token (no interrupt)
        mmioSet16(0x100E, val & 0xFFFF);
        break;
    case 0x48:  // PE token with interrupt
        if (holdToken(u16(val))) break;  // (a screen capture's bytes are still on their way)
        mmioSet16(0x100E, val & 0xFFFF);
        raiseInterrupt(IRQ_PE_TOKEN, [] { mmioSet16(0x100A, mmioGet16(0x100A) | 0x4); });
        break;
    case 0x49: s_copySrcTL = val; break;
    case 0x4A: s_copySrcWH = val; break;
    case 0x4B: s_copyDest = val; break;
    case 0x4D: s_copyStride = val; break;
    case 0x64: s_tlutSrc = val; break;
    case 0x65: {  // TLUT load: from s_tlutSrc in main memory into a TMEM region
        const u32 slot = tlutSlot(val & 0x3FF);
        if (slot < kTlutSlots) s_tluts[slot] = {(s_tlutSrc & 0x1FFFFF) << 5, ((val >> 10) & 0x7FF) * 16};
        emitTlut(slot, 1);  // RGB565 until a texture names the format
        break;
    }
    case 0x52: {  // EFB copy
        u32 capFmt = 0, capW = 0, capH = 0;
        bool capture = false;
        if ((val >> 14) & 1) s_displayCopies++;
        if (!((val >> 14) & 1)) {  // texture copy (display copies just present the EFB)
            aurora(AUR_COPY_SRC);
            // The game's screen capture (the Picto Box's photo) names the viewfinder's window in the 4:3 screen's 640 columns;
            // on a wide window the 2D picture is widened about the middle (twwwide.cpp) while the 3D picture fills the
            // columns, so the same part of it is the window's picture narrowed by that scale.
            s32 srcX = s32(s_copySrcTL & 0x3FF), srcW = s32(s_copySrcWH & 0x3FF) + 1;
            if (s_cwArmed) {
                const float scale = twwWideScale();
                if (scale > 1.001f) {
                    srcX = s32(std::lround(320.f + (float(srcX) - 320.f) / scale));
                    srcW = std::max<s32>(1, s32(std::lround(float(srcW) / scale)));
                }
            }
            put32(srcX);
            put32((s_copySrcTL >> 10) & 0x3FF);
            put32(srcW);
            put32(((s_copySrcWH >> 10) & 0x3FF) + 1);
            const bool half = (val >> 9) & 1;
            const u32 fmt = ((val >> 4) & 7) | (((val >> 3) & 1) << 3);
            aurora(AUR_COPY_DST);
            put32(((s_copySrcWH & 0x3FF) + 1) >> (half ? 1 : 0));
            put32((((s_copySrcWH >> 10) & 0x3FF) + 1) >> (half ? 1 : 0));
            put32(fmt);
            put8(half ? 1 : 0);
            aurora(AUR_COPY_DEST);
            put64(hostAddr((s_copyDest & 0x1FFFFF) << 5));
            s_copyDests.insert((s_copyDest & 0x1FFFFF) << 5);
            if (s_cwArmed) capture = true, capFmt = fmt, capW = ((s_copySrcWH & 0x3FF) + 1) >> (half ? 1 : 0), capH = (((s_copySrcWH >> 10) & 0x3FF) + 1) >> (half ? 1 : 0);
            // A copy of less than the screen ends a render-to-texture pass (shadow textures:
            // 128 and 256 square in stages, 480 square in Chao World); its segment keeps plain
            // mapping. Copies of the whole screen (640 wide) capture the scene for screen effects.
            const u32 w = (s_copySrcWH & 0x3FF) + 1, h = ((s_copySrcWH >> 10) & 0x3FF) + 1;
            if ((w < 576 || h < 360) && s_segmentAt < s_out.size()) s_out[s_segmentAt] = 0;
        }
        put8(CMD_LOAD_BP);
        put32(v);
        if (capture) captureCopy((s_copyDest & 0x1FFFFF) << 5, capFmt, capW, capH);
        beginSegment();
        // The display copy (GXCopyDisp) is where a frame ends: the frame goes to the renderer here,
        // in the command stream. (The Wind Waker's JFWDisplay calls VISetNextFrameBuffer from its
        // draw-done and retrace callbacks, while the game thread is already well into the next
        // frame's commands: cut there, frames came out split - half-drawn ones and black ones.)
        if ((val >> 14) & 1) gxEndFrame();
        return;
    }
    default: break;
    }
    put8(CMD_LOAD_BP);
    put32(v);
}

void translateCp(u32 reg, u32 v) {
    s_cp[reg & 0xFF] = v;
    s_cpSet.set(reg & 0xFF);
    if (reg == 0x50) s_vcdLo = v;
    else if (reg == 0x60) s_vcdHi = v;
    else if (reg >= 0x70 && reg < 0x78) s_vatA[reg & 7] = v;
    else if (reg >= 0x80 && reg < 0x88) s_vatB[reg & 7] = v;
    else if (reg >= 0x90 && reg < 0x98) s_vatC[reg & 7] = v;
    else if (reg == 0x30) s_cpMtxA = v;
    if (reg >= 0xA0 && reg < 0xB0) {  // array base: physical address, sent when a draw uses it
        s_arraysSet |= 1u << (reg & 0xF);
        s_arrayBase[reg & 0xF] = v;
        arrayBaseChanged(reg & 0xF);
        return;
    }
    if (reg >= 0xB0 && reg < 0xC0) {  // array stride
        s_arrayStride[reg & 0xF] = v & 0xFF;
        arrayBaseChanged(reg & 0xF);
    }
    put8(CMD_LOAD_CP);
    put8(reg);
    put32(v);
}

// The game's camera: J3D's view matrix (j3dSys.mViewMtx, The Wind Waker's j3dSys at 0x803EDA58),
// as an index into the frame's views (added when it changes).
constexpr u32 kViewMtxAddr = 0x803EDA58;
s16 currentView() {
    std::array<float, 12> m;
    for (u32 i = 0; i < 12; i++) m[i] = __builtin_bit_cast(float, rd32(kViewMtxAddr + 4 * i));
    auto& v = s_frame->views;
    if (!v.empty() && v.back() == m) return s16(v.size() - 1);
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == m) return s16(i);
    if (v.size() >= 32000) return -1;
    v.push_back(m);
    return s16(v.size() - 1);
}

// An XF load of [addr, addr+count) whose words start at `at` in the frame: the moving ones.
void recordXf(u32 addr, u32 count, u32 at) {
    if (s_noRecord) return;
    GxPatch::Kind kind;
    if (addr < 120) kind = GxPatch::PosMtx;             // position matrices (GX_PNMTX0-9)
    else if (addr < 0x100) kind = GxPatch::TexMtx;      // texture matrices
    else if (addr >= 0x400 && addr < 0x460) kind = GxPatch::NrmMtx;
    else if (addr >= 0x500 && addr < 0x600) kind = GxPatch::TexMtx;  // post-transform
    else if (addr >= 0x600 && addr < 0x680) kind = GxPatch::Light;
    else if (addr >= 0x1020 && addr <= 0x1026) kind = GxPatch::Proj;
    else return;
    GxPatch p{};
    p.kind = kind, p.at = at, p.count = count, p.stride = addr, p.key = p.key2 = tag(0, addr << 8 | count);
    if (kind == GxPatch::PosMtx) p.view = currentView();
    s_pendingXf.push_back(s_frame->patches.size());
    s_frame->patches.push_back(p);
}

// Whether a draw's texture coordinates 0 (sent with it, as floats) stay within one copy of the texture.
bool spriteWithinTexture(const u8* p, u32 count, u32 fmt) {
    const u32 lo = s_vcdLo, a = s_vatA[fmt];
    if ((s_vcdHi & 3) != 1 || ((a >> 22) & 7) != 4) return false;  // texcoord 0 direct, f32
    u32 off = 0;
    for (u32 i = 0; i < 9; i++) off += (lo >> i) & 1;
    auto direct = [&](u32 type, u32 bytes) { off += type == 1 ? bytes : type == 2 ? 1 : type == 3 ? 2 : 0; };
    direct((lo >> 9) & 3, ((a & 1) ? 3 : 2) * kComp[(a >> 1) & 7]);
    if (const u32 t = (lo >> 11) & 3) direct(t, (((a >> 9) & 1) ? 9 : 3) * kComp[(a >> 10) & 7]);
    direct((lo >> 13) & 3, kClr[(a >> 14) & 7]);
    direct((lo >> 15) & 3, kClr[(a >> 18) & 7]);
    const u32 comps = ((a >> 21) & 1) ? 2 : 1, vsize = vertexSize(fmt);
    for (u32 v = 0; v < count; v++)
        for (u32 c = 0; c < comps; c++) {
            const u8* q = p + v * vsize + off + 4 * c;
            const float x = __builtin_bit_cast(float, u32(q[0]) << 24 | u32(q[1]) << 16 | u32(q[2]) << 8 | q[3]);
            if (!(x >= -0.002f && x <= 1.002f)) return false;
        }
    return true;
}

// A draw: its model part (a display list's address; or its position array and first vertex), and
// the values loaded for it are named by that.
void nameDraw(const u8* p, u32 fmt) {
    if (s_noRecord) return;
    u64 part = 0;
    if (s_curDl) part = u64(s_curDl & 0x01FFFFFF) << 1 | 1;
    else if (const u32 posType = (s_vcdLo >> 9) & 3; posType >= 2) {
        u32 pre = 0;
        for (u32 i = 0; i < 9; i++) pre += (s_vcdLo >> i) & 1;
        const u32 idx = posType == 2 ? p[pre] : u32(p[pre]) << 8 | p[pre + 1];
        part = (u64(s_arrayBase[0] & 0x01FFFFFF) << 24 | idx) << 1;
    } else {
        // Positions sent with the draw (particles, effects): no lasting part, so draws of one
        // vertex format in the object count as copies of one part - paired by position.
        part = 1ull << 62 | u64(fmt) << 20 | (s_vcdLo & 0x1FFFF);
    }
    s_drawPart = s_drawGroup = 0;
    s_drawOcc = 0;
    if (part) {
        TaskScope& t = s_tasks.empty() ? s_noTask : s_tasks.back();
        const u32 n = t.parts[part]++;
        s_drawPart = mix(scopeKey(t) ^ mix(part ^ u64(n) << 56));
        s_drawGroup = mix(scopeKey(t) ^ mix(part)) | 1;
        s_drawOcc = n;
    }
    for (size_t n = 0; n < s_pendingXf.size(); n++) {
        GxPatch& x = s_frame->patches[s_pendingXf[n]];
        // A load that a later one overwrote before this draw (a reset to identity, then the real
        // matrix) never reached a draw: left alone, so it cannot take the real one's name.
        bool overwritten = false;
        for (size_t m = n + 1; m < s_pendingXf.size() && !overwritten; m++) {
            const GxPatch& y = s_frame->patches[s_pendingXf[m]];
            overwritten = y.stride <= x.stride && y.stride + y.count >= x.stride + x.count;
        }
        if (overwritten) {
            x.flat = true;
            continue;
        }
        if (s_drawPart) x.key = mix(s_drawPart ^ (u64(x.stride) << 8 | x.count)), x.group = s_drawGroup, x.occ = s_drawOcc;
    }
    s_drawXf.swap(s_pendingXf);
    s_pendingXf.clear();
}

// The whole GX state as it is now, as commands (the start of the next frame).
void buildPreamble(std::vector<u8>& out) {
    out.clear();
    if (s_noRecord) return;
    s_out.swap(out);
    for (u32 r = 0; r < 256; r++)
        if (s_cpSet[r] && !(r >= 0xA0 && r < 0xB0)) {  // array bases: each frame sends its own
            put8(CMD_LOAD_CP);
            put8(r);
            put32(s_cp[r]);
        }
    for (u32 a = 0; a < 0x1058;) {
        if (!s_xfSet[a]) {
            a++;
            continue;
        }
        u32 n = 0;
        while (a + n < 0x1058 && s_xfSet[a + n] && n < 64) n++;
        put8(CMD_LOAD_XF);
        put16(n - 1);
        put16(a);
        for (u32 i = 0; i < n; i++) put32(s_xf[a + i]);
        a += n;
    }
    for (u32 r = 0; r < 256; r++) {
        if (!s_bpSet[r]) continue;
        // Writes that do something rather than set state: draw done, tokens, copies, TLUT loads,
        // texture cache invalidation, the mask; and texture addresses (loaded below).
        if (r == 0x45 || r == 0x46 || r == 0x47 || r == 0x48 || r == 0x52 || r == 0x65 || r == 0x66 || r == 0x67 ||
            r == 0xFE || texMapOf(r, 0x14) >= 0)
            continue;
        put8(CMD_LOAD_BP);
        put32(r << 24 | (s_bp[r] & 0xFFFFFF));
    }
    for (int map = 0; map < 8; map++) {
        const u32 reg = map < 4 ? 0x94 + u32(map) : 0xB4 + u32(map - 4);
        if (!s_bpSet[reg]) continue;
        put8(CMD_LOAD_BP);
        put32(reg << 24 | (s_bp[reg] & 0xFFFFFF));
        loadTexture(map, s_bp[reg] & 0xFFFFFF);
    }
    s_out.swap(out);
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u32 be16(const u8* p) { return u32(p[0]) << 8 | p[1]; }

// Translates whole commands from [p, p+n); returns the bytes consumed (a partial command stays).
size_t parse(const u8* p, size_t n, int depth);

// A command longer than what has arrived (a draw's vertices, an XF load): the FIFO is parsed
// again only once it has all of it, not on every write.
size_t s_needBytes = 0;
size_t needMore(size_t len, int depth) {
    if (depth == 0) s_needBytes = len;
    return 0;
}

size_t command(const u8* p, size_t n, int depth) {
    if (n < 1) return 0;
    const u8 op = p[0];
    switch (op) {
    case CMD_NOP: {  // padding (display lists are padded to 32 bytes): nothing to pass on
        size_t k = 1;
        while (k < n && p[k] == CMD_NOP) k++;
        return k;
    }
    case CMD_INVAL_VC:
    case CMD_METRICS:
        put8(op);
        return 1;
    case CMD_LOAD_CP:
        if (n < 6) return 0;
        translateCp(p[1], be32(p + 2));
        return 6;
    case CMD_LOAD_XF: {
        if (n < 5) return 0;
        const u32 count = (be16(p + 1)) + 1;
        const size_t len = 5 + 4 * size_t(count);
        if (n < len) return needMore(len, depth);
        const u32 addr = be16(p + 3);
        for (u32 i = 0; i < count; i++)
            if (addr + i < 0x1058) s_xf[addr + i] = be32(p + 5 + i * 4), s_xfSet.set(addr + i);
        recordXf(addr, count, u32(s_out.size()) + 5);
        putBytes(p, len);
        return len;
    }
    case CMD_INDX_A: case CMD_INDX_B: case CMD_INDX_C: case CMD_INDX_D: {
        // Indexed XF load: (index, length-1 << 12 | XF address) from arrays 12-15.
        if (n < 5) return 0;
        // Sent as the immediate load it amounts to, so the values are in the frame (to blend).
        const u32 array = 12 + (op - CMD_INDX_A) / 8;
        const u32 src = s_arrayBase[array] + be16(p + 1) * s_arrayStride[array], xa = be16(p + 3) & 0xFFF;
        const u32 count = (be16(p + 3) >> 12) + 1;
        put8(CMD_LOAD_XF);
        put16(count - 1);
        put16(xa);
        recordXf(xa, count, u32(s_out.size()));
        for (u32 i = 0; i < count; i++) {
            const u32 w = rd32(0x80000000u | ((src + i * 4) & 0x01FFFFFF));
            if (xa + i < 0x1058) s_xf[xa + i] = w, s_xfSet.set(xa + i);
            put32(w);
        }
        return 5;
    }
    case CMD_CALL_DL: {
        if (n < 9) return 0;
        const u32 addr = be32(p + 1), size = be32(p + 5);
        const u32 outer = s_curDl;
        s_curDl = addr;
        // A list recorded under a scope (gxHeightFieldEnter...) plays under it.
        const u32 a = 0x80000000u | (addr & 0x01FFFFFF);
        int height = 0, staticV = 0;
        for (const ScopedDl& d : s_scopedDls)
            if (a >= d.lo && a < d.hi) height += d.height, staticV += d.staticV || d.height;
        s_heightField += height, s_staticVertices += staticV;
        if (depth < 4) parse(hostp(0x80000000u | (addr & 0x01FFFFFF)), size, depth + 1);
        s_heightField -= height, s_staticVertices -= staticV;
        s_curDl = outer;
        return 9;
    }
    case CMD_LOAD_BP:
        if (n < 5) return 0;
        translateBp(be32(p + 1));
        return 5;
    default:
        if (op >= 0x80 && op < 0xC0) {
            if (n < 3) return 0;
            for (u32 m = 0; s_texDirty && m < 8; m++)
                if (s_texDirty & (1u << m)) {
                    s_texDirty &= ~(1u << m);
                    loadTexture(int(m), s_texImage3[m]);
                }
            if (const u32 miss = missingArrays()) {
                static int s_warned = 0;
                if (s_warned++ < 5) log("[gx] draw %02X uses arrays %03X with no base (VCD %08X/%08X)\n", op, miss, s_vcdLo, s_vcdHi);
            }
            const u32 count = be16(p + 1);
            const size_t len = 3 + size_t(count) * vertexSize(op & 7);
            if (n < len) return needMore(len, depth);
            if (s_skipDraws.count(s_drawIndex++)) return len;  // SA2_SKIP_DRAWS (debugging)
            if (!s_skipBlend.empty() && s_skipBlend.count(s_bp[0x41] & 0xFFFFFF)) return len;
            if (!s_skipTex.empty() && (s_genMode & 0xF) && s_skipTex.count(s_texImage0[0] & 0xFFFFFF)) return len;
            if (!s_skipTexAddr.empty() && (s_genMode & 0xF) && s_skipTexAddr.count(s_tex0 & 0x01FFFFFF)) return len;
            // Too few vertices for one primitive draws nothing on the hardware. The game's model
            // code emits such draws (e.g. empty triangle strips); Aurora would turn them into a
            // bogus strip that the next draw merges into, drawing its triangles as a strip.
            const u32 prim = op & 0xF8;
            const u32 minCount = prim == 0x80 || prim == 0x88 ? 4 : prim >= 0xA8 && prim < 0xB8 ? 2 : prim == 0xB8 ? 1 : 3;
            if (count < minCount) return len;
            s_drawXf.clear();
            nameDraw(p + 3, op & 7);
            const size_t firstPatch = s_frame->patches.size();
            sizeArrays(p + 3, be16(p + 1), op & 7);
            // 2D: orthographic draws (the HUD, menus, the title logo). (SA2B's place2D, which also
            // placed 2D for Aurora's widescreen, is not needed: The Wind Waker's widescreen is its own.)
            s_draw2D = s_xf[0x1026] == 1;
            (void)place2D;
            if (s_draw2D) {  // 2D: what it uses is not blended
                for (size_t i : s_drawXf) s_frame->patches[i].flat = true;
                for (size_t i = firstPatch; i < s_frame->patches.size(); i++) s_frame->patches[i].flat = true;
            }
            if (!s_noRecord && ((s_vcdLo >> 9) & 3) == 1) {  // positions sent with the draw (sprites, skinned meshes)
                const u32 a = s_vatA[op & 7];
                GxPatch v{};
                v.kind = GxPatch::Vertex, v.at = u32(s_out.size()) + 3, v.count = count, v.stride = vertexSize(op & 7);
                for (u32 i = 0; i < 9; i++) v.posOff += (s_vcdLo >> i) & 1;
                v.fmt = u8((a >> 1) & 7), v.comps = u8((a & 1) ? 3 : 2), v.shift = u8((a >> 4) & 31);
                v.key2 = tag(1, count << 8 | v.stride);
                v.key = s_drawPart ? mix(s_drawPart ^ 0x7E47) : v.key2;
                v.group = s_drawGroup, v.occ = s_drawOcc;
                v.flat = s_draw2D || (s_staticVertices > 0 && !s_heightField);
                v.height = s_heightField > 0;
                v.view = currentView();
                {  // the draw's position matrix: identity -> its positions are in the camera's space
                    const u32 m = ((s_vcdLo & 1) ? p[3] : (s_cpMtxA & 0x3F)) * 4;
                    bool id = m + 11 < 0x100;
                    for (u32 q = 0; id && q < 12; q++) id = xff(m + q) == ((q == 0 || q == 5 || q == 10) ? 1.f : 0.f);
                    v.viewSpace = id;
                    // (or through the camera's own matrix: positions in the world - the wake's and wave's, from the particles')
                    bool cam = !id && m + 11 < 0x100;
                    for (u32 q = 0; cam && q < 12; q++) cam = xff(m + q) == __builtin_bit_cast(float, rd32(kViewMtxAddr + 4 * q));
                    v.worldSpace = cam;
                    v.world = (id || cam) && s_trailVertices > 0 && s_staticVertices > 0 && !s_heightField && !s_draw2D;
                }
                s_frame->patches.push_back(v);
            }
            // A 2D sprite drawing one copy of its texture (coordinates within 0..1) samples it clamped
            // at the edges: with the texture set to repeat, filtering at more than 640x480 would blend
            // in the opposite edge - lines between the tiles of backdrops and maps.
            const bool clampSprite = s_draw2D && spriteWithinTexture(p + 3, count, op & 7) && (s_bp[0x80] & 0xF);
            if (clampSprite) {
                put8(CMD_LOAD_BP);
                put32(0x80u << 24 | (s_bp[0x80] & 0xFFFFF0));
            }
            const size_t drawAt = s_out.size();
            putBytes(p, len);
            for (const auto& [off, x] : s_bleed) {  // (the copy: a display list in guest memory stays as it is)
                const u32 b = __builtin_bit_cast(u32, x);
                u8* q = s_out.data() + drawAt + 3 + off;
                q[0] = u8(b >> 24), q[1] = u8(b >> 16), q[2] = u8(b >> 8), q[3] = u8(b);
            }
            s_bleed.clear();
            if (clampSprite) {
                put8(CMD_LOAD_BP);
                put32(0x80u << 24 | (s_bp[0x80] & 0xFFFFFF));
            }
            return len;
        }
        static int s_bad = 0;
        if (s_bad++ < 8) log("[gx] unknown command %02X (depth %d)\n", op, depth);
        return n;  // resync impossible: drop the rest
    }
}

size_t parse(const u8* p, size_t n, int depth) {
    size_t done = 0;
    while (done < n) {
        const size_t c = command(p + done, n - done, depth);
        if (!c) break;
        done += c;
    }
    return done;
}

// ---- write-gather pipe input ----
std::vector<u8> s_in;
size_t s_inPos = 0;
u64 s_gxCycles = 0;  // time stamp counter cycles spent translating (SA2_GX_TIMING)
const bool s_gxTiming = std::getenv("SA2_GX_TIMING") != nullptr;
u32 s_gpFifoBase = 0;  // CP FIFO base: writes go to the GPU while the CPU FIFO is the same one

bool redirectedToMemory() {
    const u32 piBase = mmioGet32(0x300C) & 0x03FFFFFF;  // (straight from the register store: every GX write asks)
    return s_gpFifoBase && piBase != s_gpFifoBase;
}

// ---- frame handoff to the render thread ----
std::mutex s_frameLock;
std::condition_variable s_frameCv;
std::deque<std::shared_ptr<GxFrame>> s_ready;
}  // namespace

void gxTaskEnter(u32 task, u32 fn) {
    const u32 occ = s_occurrences[u64(task) << 32 | fn]++;
    s_tasks.push_back({task, fn, occ, {0, 0, 0}, {}});
}
void gxTaskLeave() {
    if (!s_tasks.empty()) s_tasks.pop_back();
}

void gxWrite(u32 v, int size) {
    if (redirectedToMemory()) {
        u32 wp = rd32(0xCC003014) & 0x03FFFFFF;
        for (int i = size - 1; i >= 0; i--) *hostp(0x80000000u | (wp++ & 0x01FFFFFF)) = u8(v >> (8 * i));
        const u32 end = rd32(0xCC003010) & 0x03FFFFFF;
        if (wp >= end && end) wp |= 0x04000000;  // overflow ("wrap") flag
        wr32(0xCC003014, wp);
        return;
    }
    for (int i = size - 1; i >= 0; i--) s_in.push_back(u8(v >> (8 * i)));
    if (s_in.size() - s_inPos < s_needBytes) return;
    s_needBytes = 0;
    const u64 c0 = s_gxTiming ? hostCycles() : 0;
    s_inPos += parse(s_in.data() + s_inPos, s_in.size() - s_inPos, 0);
    if (s_gxTiming) s_gxCycles += hostCycles() - c0;
    if (s_inPos > (1u << 16)) {
        s_in.erase(s_in.begin(), s_in.begin() + ptrdiff_t(s_inPos));
        s_inPos = 0;
    }
}

void gxNoteCpRegister(u32 off, u32 v) {
    // CP FIFO base lo/hi (0xCC000020/22): remember which buffer the GP reads.
    if (off == 0x20) s_gpFifoBase = (s_gpFifoBase & 0xFFFF0000) | (v & 0xFFFF);
    else if (off == 0x22) s_gpFifoBase = (s_gpFifoBase & 0xFFFF) | ((v & 0x3FF) << 16);
}

void gxFrameStarts() { s_frameStartVi = viLastRetrace(); }
// A scope that records a display list (the CPU FIFO pointed into memory - the sea builds its DL
// in its draw and the game plays it later): the scope's flags go with the list, to its playback.
namespace {
std::vector<u32> s_scopeDlStart;    // per open scope: where the CPU FIFO was writing (0: not recording)
u32 fifoWritePtr() { return 0x80000000u | (rd32(0xCC003014) & 0x01FFFFFF); }
void scopeEnter() { s_scopeDlStart.push_back(redirectedToMemory() ? fifoWritePtr() : 0); }
void scopeLeave(bool height) {
    const u32 start = s_scopeDlStart.empty() ? 0 : s_scopeDlStart.back();
    if (!s_scopeDlStart.empty()) s_scopeDlStart.pop_back();
    if (!start || !redirectedToMemory()) return;
    const u32 end = fifoWritePtr();
    if (end <= start) return;
    std::erase_if(s_scopedDls, [&](const ScopedDl& d) { return d.lo < end && start < d.hi; });  // rewritten
    s_scopedDls.push_back({start, end, height, !height});
    if (s_scopedDls.size() > 64) s_scopedDls.erase(s_scopedDls.begin());
}
}  // namespace
void gxHeightFieldEnter() { s_heightField++, scopeEnter(); }
void gxHeightFieldLeave() { s_heightField--, scopeLeave(true); }
void gxStaticVerticesEnter() { s_staticVertices++, scopeEnter(); }
void gxStaticVerticesLeave() { s_staticVertices--, scopeLeave(false); }
void gxTrailVerticesEnter() { s_trailVertices++, gxStaticVerticesEnter(); }
void gxTrailVerticesLeave() { gxStaticVerticesLeave(), s_trailVertices--; }

void gxEndFrame() {
    gameFixesFrame();
    s_frames++;
    // SA2_SLOW=<from>:<to>:<ms> (timing experiments): the game takes that much longer per frame there.
    static unsigned s_slow[3] = {0, 0, 0};
    static const bool s_slowSet = std::getenv("SA2_SLOW") && std::sscanf(std::getenv("SA2_SLOW"), "%u:%u:%u", &s_slow[0], &s_slow[1], &s_slow[2]) == 3;
    if (s_slowSet && s_frames >= s_slow[0] && s_frames <= s_slow[1]) std::this_thread::sleep_for(std::chrono::milliseconds(s_slow[2]));
    // SA2_PACE_FILE=<file> SA2_PACE_REL=<module> SA2_PACE_FROM=<frames after link> (timing
    // experiments): from then on, frame k is held until it has taken the k-th number of retraces in
    // the file (one per line - e.g. a reference run's frame durations).
    if (const char* pf = std::getenv("SA2_PACE_FILE")) {
        static std::vector<u32> pace;
        static bool loaded = false;
        static u64 lastVi = 0;
        if (!loaded) {
            loaded = true;
            if (std::FILE* f = std::fopen(pf, "r")) {
                unsigned v;
                while (std::fscanf(f, "%u", &v) == 1) pace.push_back(v);
                std::fclose(f);
            }
            log("[pace] %zu frame durations from %s\n", pace.size(), pf);
        }
        static const int rel = std::getenv("SA2_PACE_REL") ? std::atoi(std::getenv("SA2_PACE_REL")) : 80;
        static const u64 from = std::getenv("SA2_PACE_FROM") ? std::strtoull(std::getenv("SA2_PACE_FROM"), nullptr, 10) : 0;
        const u64 link = g_relLinkFrame[rel & (kMaxModules - 1)].load();
        if (link && s_frames >= link - 1 + from && s_frames - (link - 1 + from) < pace.size()) {
            const u32 want = pace[s_frames - (link - 1 + from)];
            sched::withCpuReleased([&] {
                while (viRetraces() < lastVi + want) std::this_thread::sleep_for(std::chrono::microseconds(500));
            });
        }
        lastVi = viRetraces();
    }
    // SA2_PEEK=<hex addr>,...[@<every>] (timing experiments): those words (as ints and floats) every
    // <every> frames (default 1), with the time and the VI retrace count.
    if (const char* pk = std::getenv("SA2_PEEK")) {
        static std::vector<u32> addrs;
        static u32 every = 1;
        if (addrs.empty()) {
            for (const char* p = pk; *p && *p != '@';) {
                char* end;
                addrs.push_back(u32(std::strtoul(p, &end, 16)));
                p = *end == ',' ? end + 1 : end;
            }
            if (const char* at = std::strchr(pk, '@')) every = std::max(1u, u32(std::atoi(at + 1)));
        }
        if (s_frames % every == 0) {
            std::string s;
            char b[64];
            for (u32 a : addrs) {
                const u32 v = rd32(a);
                std::snprintf(b, sizeof b, " %08X=%d/%.3f", a, s32(v), __builtin_bit_cast(float, v));
                s += b;
            }
            log("[peek] frame %llu t %.3f vi %llu:%s\n", (unsigned long long)s_frames, gxNow(), (unsigned long long)viRetraceCount(), s.c_str());
        }
    }
    // SA2_SCAN=<f1>:<f2>:<f3> (timing experiments): MEM1 words that grow steadily over those frames -
    // counters (ints) and clocks (floats) - with how much they grew in each interval, and the time.
    if (const char* sc = std::getenv("SA2_SCAN")) {
        static unsigned f[3] = {0, 0, 0};
        static const bool ok = std::sscanf(sc, "%u:%u:%u", &f[0], &f[1], &f[2]) == 3;
        static std::vector<u32> snap[3];
        static double when[3];
        for (int k = 0; ok && k < 3; k++)
            if (s_frames == f[k]) {
                snap[k].resize(0x01800000 / 4);
                for (u32 i = 0; i < snap[k].size(); i++) snap[k][i] = rd32(0x80000000u + i * 4);
                when[k] = gxNow();
                if (k == 2) {
                    log("[scan] frames %u, %u, %u at %.3f s apart, %.3f s\n", f[0], f[1], f[2], when[1] - when[0], when[2] - when[1]);
                    u32 shown = 0;
                    for (u32 i = 0; i < snap[0].size() && shown < 6000; i++) {
                        const u32 a = snap[0][i], b = snap[1][i], c = snap[2][i];
                        if (a == b || b == c) continue;
                        const s64 di = s64(s32(b)) - s32(a), dj = s64(s32(c)) - s32(b);
                        const float fa = __builtin_bit_cast(float, a), fb = __builtin_bit_cast(float, b), fc = __builtin_bit_cast(float, c);
                        const bool intLike = di > 0 && dj > 0 && di < 100000 && dj < 100000;
                        const bool floatLike = std::isfinite(fa) && std::isfinite(fc) && fb > fa && fc > fb && fb - fa < 1e6f &&
                                               std::fabs(fa) > 1e-3f && std::fabs(fa) < 1e7f;
                        if (!intLike && !floatLike) continue;
                        log("[scan] %08X: int +%lld +%lld | float %.4f +%.4f +%.4f\n", 0x80000000u + i * 4, (long long)di, (long long)dj, fa,
                            fb - fa, fc - fb);
                        shown++;
                    }
                    for (auto& s : snap) std::vector<u32>().swap(s);
                }
            }
    }
    // SA2_GX_TIMING: the game thread's time translating GX commands, and the frame handoff.
    static const bool s_timing = std::getenv("SA2_GX_TIMING") != nullptr;
    static u64 s_tc0 = hostCycles();
    static double s_wall0 = gxNow();
    static u64 s_endCycles = 0;
    const u64 e0 = hostCycles();
    struct EndTimer {
        u64 e0;
        ~EndTimer() { s_endCycles += hostCycles() - e0; }
    } endTimer{e0};
    // The game's own threads' CPU time per frame: how close the game runs to its 16.7 ms.
    static u64 s_busy0 = 0, s_busyMax = 0, s_busySum = 0;
    static u32 s_busyOver = 0;
    if (s_timing) {
        const u64 b = profileGuestCycles();
        if (s_busy0) s_busySum += b - s_busy0, s_busyMax = std::max(s_busyMax, b - s_busy0), s_busyOver += (b - s_busy0) > 35000000;
        s_busy0 = b;
    }
    if (s_timing && s_frames % 300 == 0) {
        const double wall = gxNow() - s_wall0;
        const double hz = double(hostCycles() - s_tc0) / std::max(wall, 1e-6);
        log("[gx] last 300 frames: translating %.2f ms/frame, handoff %.2f ms/frame (%.0f fps); game CPU %.2f ms/frame, max %.2f, "
            "%u over 14 ms\n",
            double(s_gxCycles) / hz * 1000 / 300, double(s_endCycles) / hz * 1000 / 300, 300 / std::max(wall, 1e-6),
            double(s_busySum) / hz * 1000 / 300, double(s_busyMax) / hz * 1000, s_busyOver);
        s_gxCycles = s_endCycles = 0;
        s_busySum = s_busyMax = 0, s_busyOver = 0;
        profileLogThreads(hz);
        s_tc0 = hostCycles();
        s_wall0 = gxNow();
    }
    if (s_out.size() > (2u << 20) || s_displayCopies > 1)
        log("[gx] frame %llu: %zu bytes, %u display copies\n", (unsigned long long)s_frames, s_out.size(), s_displayCopies);
    s_displayCopies = 0;
    std::shared_ptr<GxFrame> f = std::move(s_frame);
    f->cmds.swap(s_out);
    s_out.clear();
    for (auto& chunk : s_arena.chunks) f->snapshots.push_back(std::move(chunk));
    s_arena = {};
    f->index = s_frames;
    f->jitter = g_interpJitter.load();
    {  // the main camera: the one most 3D position matrices were loaded under
        std::vector<u32> n(f->views.size(), 0);
        for (const GxPatch& p : f->patches)
            if (p.kind == GxPatch::PosMtx && p.view >= 0 && !p.flat) n[size_t(p.view)]++;
        if (!n.empty()) f->mainView = int(std::max_element(n.begin(), n.end()) - n.begin());
    }
    f->time = gxNow();
    // (At 30 fps a frame spans two retraces: the one it started on keeps the steps even.)
    f->vi = s_frameStartVi > 0 ? s_frameStartVi : viLastRetrace();
    std::unique_lock lk(s_frameLock);
    // The game never waits for the display: it runs at the console's 60 Hz whatever the monitor
    // does (some of its timing follows the music clock, some the frame count; holding frames back
    // pulled them apart in cutscenes). Only when the renderer has fallen several frames behind
    // (a stall, a minimised window) is the game held, briefly, with the guest CPU released.
    // (A test run logging in-between frames (SA2_INTERP_JITTER) holds the game for every frame: each is built and drawn in
    // turn while the game's memory is as it was.)
    const size_t holdAt = g_interpJitter ? 1 : 6;
    if (s_ready.size() >= holdAt) {
        lk.unlock();
        sched::withCpuReleased([&] {
            std::unique_lock w(s_frameLock);
            s_frameCv.wait_for(w, std::chrono::milliseconds(g_interpJitter ? 3000 : 50), [&] { return s_ready.size() < holdAt; });
        });
        lk.lock();
    }
    s_ready.push_back(std::move(f));
    lk.unlock();
    // The next frame: the state it starts from, then its commands.
    s_frame = std::make_shared<GxFrame>();
    buildPreamble(s_frame->preamble);
    s_tasks.clear();
    s_noTask = {0, 0, 0, {0, 0, 0}, {}};
    s_occurrences.clear();
    s_pendingXf.clear();
    std::fill(std::begin(s_arraySent), std::end(s_arraySent), 0u);  // next frame snapshots afresh
    std::fill(std::begin(s_arrayCopy), std::end(s_arrayCopy), nullptr);
    std::fill(std::begin(s_arrayShown), std::end(s_arrayShown), nullptr);
    s_arraySnaps.clear();
    s_drawIndex = 0;
    s_copyDests.clear();
    s_backdropDepths[0].swap(s_backdropDepths[1]);
    s_backdropDepths[1].clear();
    beginSegment();  // the next frame starts drawing to the screen
    // SA2_WIDE_LOG=<game frame> or r<module>:<frames after it linked>: log that frame's placements;
    // <from>-<to>: those frames' flat draws (2D candidates).
    if (const char* wl = std::getenv("SA2_WIDE_LOG")) {
        if (wl[0] == 'r') {
            const u32 mod = u32(std::atoi(wl + 1));
            const char* c = std::strchr(wl, ':');
            const u64 link = g_relLinkFrame[mod & (kMaxModules - 1)].load();
            if (link && c) s_wideLogFrame = link - 1 + std::strtoull(c + 1, nullptr, 10);
            if (const char* d = c ? std::strchr(c, '-') : nullptr; link && d) s_wideLogTo = link - 1 + std::strtoull(d + 1, nullptr, 10);
        } else {
            s_wideLogFrame = std::strtoull(wl, nullptr, 10);
            if (const char* d = std::strchr(wl, '-')) s_wideLogTo = std::strtoull(d + 1, nullptr, 10);
        }
    }
    s_frameCv.notify_all();
}

bool gxTakeFrames(std::vector<std::shared_ptr<GxFrame>>& out, int waitMs) {
    out.clear();
    std::unique_lock lk(s_frameLock);
    if (!s_frameCv.wait_for(lk, std::chrono::milliseconds(waitMs), [] { return !s_ready.empty(); })) return false;
    for (auto& f : s_ready) out.push_back(std::move(f));
    s_ready.clear();
    s_frameCv.notify_all();
    return true;
}

u64 gxFrames() { return s_frames; }

double gxNow() {
    static const auto s_t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
}


void gxCopyReadbackArm(bool on) { s_cwArmed = on; }

void gxCopyReadbackPoll() {
    bool released = false;
    {
        std::lock_guard lk(s_cwLock);
        if (s_cwPending.empty()) return;
    }
    const double now = gxNow();
    for (size_t i = 0;; ) {
        CopyWait cw;
        {
            std::lock_guard lk(s_cwLock);
            if (i >= s_cwPending.size()) break;
            cw = s_cwPending[i];
        }
        std::vector<u8> bytes(cw.size);
        AuroraReadbackDest d{cw.key, bytes.data(), cw.size, GX_FALSE};
        const bool got = AuroraReadbackGet(s_cwStream, &d, 1) != GX_FALSE && d.found != GX_FALSE;
        if (!got && now - cw.since < kCopyWaitSeconds) {
            i++;
            continue;
        }
        if (!got) {  // no picture (the frame was dropped, or this GPU cannot read back): a plain grey one, not zeros
            log("[efb] the screen capture's picture never came back (%u bytes at %08X)\n", cw.size, 0x80000000u | cw.guest);
            std::memset(bytes.data(), cw.fill, cw.size);
        }
        std::memcpy(hostp(0x80000000u | cw.guest), bytes.data(), cw.size);
        dumpCopy(cw, bytes.data());
        if (std::getenv("SA2_EFB_LOG")) log("[efb] screen capture copy delivered after %.3f s (%s)\n", now - cw.since, got ? "read back" : "timed out");
        std::lock_guard lk(s_cwLock);
        s_cwPending.erase(s_cwPending.begin() + long(i));
        released = s_cwPending.empty();
    }
    if (released) releaseHeldTokens();
}

bool gxCapturePeeks(u64 stream, u64 sequence, const GxPeek* peeks, u32 count) {
    if (!s_frame || count == 0 || redirectedToMemory()) return false;
    if (std::getenv("SA2_EFB_LOG") && !peeks[0].depth)
        log("[efb] capture at viewport %.1f x %.1f origin %.1f, %.1f; scissor %06X %06X; proj %.4f %.4f %.4f %.4f\n", xff(0x101A) * 2, xff(0x101B) * -2,
            xff(0x101D) - 342.f, xff(0x101E) - 342.f, s_bp[0x20], s_bp[0x21], xff(0x1020), xff(0x1021), xff(0x1022), xff(0x1023));
    aurora(0x0050);  // GX_AURORA_READBACK_CAPTURE_PEEKS (extern/aurora GXAurora.h)
    put64(stream);
    put64(sequence);
    put32(count);
    put8(2);  // GX_READ_NONE: alpha as stored
    for (u32 i = 0; i < count; i++) {
        put64(peeks[i].key);
        put8(peeks[i].depth ? 0 : 1);  // AURORA_READBACK_PEEK_Z, _ARGB
        put16(peeks[i].x);
        put16(peeks[i].y);
    }
    return true;
}

bool gxProject(const float eye[3], float& x, float& y) {
    if (s_xf[0x1026] != 0 || eye[2] > -1e-3f) return false;  // (orthographic, or behind the eye)
    const float ndcX = (xff(0x1020) * eye[0] + xff(0x1021) * eye[2]) / -eye[2];
    const float ndcY = (xff(0x1022) * eye[1] + xff(0x1023) * eye[2]) / -eye[2];
    x = ndcX * xff(0x101A) + xff(0x101D) - 342.f;
    y = ndcY * xff(0x101B) + xff(0x101E) - 342.f;
    return true;
}

}  // namespace rt
