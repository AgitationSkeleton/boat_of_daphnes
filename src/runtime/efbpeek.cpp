// EFB read-back for the game's GXPeekARGB / GXPeekZ. The hardware answers a peek at once; Aurora's
// GPU renders behind the game, so the values are asked for in the frame's commands (gx.cpp,
// gxCapturePeeks: the EFB as it is at that place) and come back a few frames later through Aurora's
// asynchronous read-back (extern/aurora GXAurora.h). Two users:
//  * the Picto Box (dSnap_packet::Judge): the game draws every photographed subject in a flat ID
//    colour (the EFB's alpha) and counts the shutter area's pixels per subject. Judge waits for the
//    pixels, then runs as the game wrote it, its peeks answered from them. The photo is judged some
//    frames after the shutter - the game reads the result only when the player confirms the picture.
//  * depth peeks (the sun's lens flare, the lantern's glow, the fireflies): the game's choice of
//    visible or hidden follows the previous frame's depth, as it does on a busy console.
#include "runtime/bod_bridge.h"
#include "runtime/gx.h"
#include "runtime/runtime.h"

#include <dolphin/gx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

namespace {
// A stream with the sequence numbers of its captures (Aurora's own counter is never used).
struct Stream {
    u64 id = 0;
    u64 seq = 0;
};
bool create(Stream& s) {
    if (!s.id) s.id = AuroraReadbackCreateStream();
    return s.id != 0;
}
bool get(Stream& s, const std::vector<u64>& keys, std::vector<u32>& values, std::vector<u8>& found) {
    values.assign(keys.size(), 0);
    found.assign(keys.size(), 0);
    if (!s.id || keys.empty()) return false;
    std::vector<AuroraReadbackDest> d(keys.size());
    for (size_t i = 0; i < keys.size(); i++) d[i] = {keys[i], &values[i], sizeof(u32), GX_FALSE};
    const bool any = AuroraReadbackGet(s.id, d.data(), u32(d.size())) != GX_FALSE;
    for (size_t i = 0; i < keys.size(); i++) found[i] = d[i].found != GX_FALSE;
    return any;
}

// ---- depth ----
Stream s_z;
std::unordered_map<u64, u32> s_zLast;  // the latest values by key
bool s_zAny = false;
std::vector<GxPeek> s_zPending;  // GXPeekZ calls since the frame's flush (keyed by their order)
const bool s_log = std::getenv("SA2_EFB_LOG") != nullptr;
const bool s_off = std::getenv("SA2_EFB_OFF") != nullptr;  // (testing: the game's own peeks, as before)
constexpr u32 kFar = 0xFFFFFF;  // nothing in front of the point
constexpr u64 kOrderKey = 1ull << 40;

// Collects the latest values of `keys` into s_zLast.
void zCollect(const std::vector<u64>& keys) {
    std::vector<u32> v;
    std::vector<u8> f;
    if (!get(s_z, keys, v, f)) return;
    s_zAny = true;
    for (size_t i = 0; i < keys.size(); i++)
        if (f[i]) s_zLast[keys[i]] = v[i];
}
}  // namespace

// dDlst_peekZ_c::peekData: the frame's queued depth peeks (the sun, the lantern's glow: each into the
// word the caller named) and the GXPeekZ calls made since the last one (fireflies).
HLE(peekData__13dDlst_peekZ_cFv) {
    const u32 self = c.r[3];
    const u32 count = rd8(self);
    if (s_off) {
        std::string z;
        for (u32 i = 0; s_log && i < count && i < 50; i++) z += " " + std::to_string(rd32(rd32(self + 4 + 8 * i + 4)));
        callOriginal(c, "peekData__13dDlst_peekZ_cFv");
        if (s_log && gxFrames() % 120 == 0 && count)
            log("[efb] frame %llu: (peeks off) depth words before %s\n", (unsigned long long)gxFrames(), z.c_str());
        for (u32 i = 0; s_log && gxFrames() % 120 == 0 && i < count && i < 50; i++)
            log("[efb]   after: %u\n", rd32(rd32(self + 4 + 8 * i + 4)));
        return;
    }
    if (s_log && gxFrames() % 120 == 0) log("[efb] frame %llu: peekData, %u queued\n",(unsigned long long)gxFrames(), count);
    std::vector<GxPeek> peeks;
    std::vector<u64> keys;
    std::vector<u32> dsts;
    for (u32 i = 0; i < count && i < 50; i++) {
        const u32 e = self + 4 + 8 * i;
        const u64 key = rd32(e + 4);  // the word it fills
        if (!key) continue;
        peeks.push_back({key, true, u16(rd16(e)), u16(rd16(e + 2))});
        dsts.push_back(u32(key));
    }
    for (const GxPeek& p : s_zPending) peeks.push_back(p);
    s_zPending.clear();
    for (const GxPeek& p : peeks) keys.push_back(p.key);
    if (create(s_z)) {
        zCollect(keys);
        for (u32 d : dsts) {
            auto it = s_zLast.find(d);
            if (it != s_zLast.end()) wr32(d, it->second);
            else if (!s_zAny) wr32(d, kFar);  // (nothing has come back yet: not hidden)
        }
        // Points off the EFB are not asked for.
        std::vector<GxPeek> ok;
        for (const GxPeek& p : peeks)
            if (p.x < 640 && p.y < 528) ok.push_back(p);
        if (!ok.empty() && gxCapturePeeks(s_z.id, ++s_z.seq, ok.data(), u32(ok.size()))) {
            if (s_log && gxFrames() % 60 == 0) log("[efb] frame %llu: %zu depth peeks asked\n", (unsigned long long)gxFrames(), ok.size());
        }
    } else {
        for (u32 d : dsts) wr32(d, kFar);
    }
    if (s_log && gxFrames() % 120 == 0 && !dsts.empty()) {
        std::string z;
        for (u32 d : dsts) z += " " + std::to_string(rd32(d));
        log("[efb] frame %llu: depth values the game holds:%s\n", (unsigned long long)gxFrames(), z.c_str());
    }
    wr8(self, 0);
}

// GXPeekZ(x, y, &z) called by the game itself: answered from the last flush's value for the
// call of the same order, and asked for again with the next flush.
HLE(GXPeekZ) {
    if (s_off) return callOriginal(c, "GXPeekZ");
    const u32 x = c.r[3] & 0xFFFF, y = c.r[4] & 0xFFFF, dst = c.r[5];
    const u64 key = kOrderKey | s_zPending.size();
    auto it = s_zLast.find(key);
    wr32(dst, it != s_zLast.end() ? it->second : kFar);
    if (s_zPending.size() < 200) s_zPending.push_back({key, true, u16(x), u16(y)});
}

// ---- the Picto Box ----
namespace {
constexpr int kLeft = 174, kRight = 468, kTop = 86, kBottom = 284;  // d_snap.cpp's shutter area
constexpr u32 kW = kRight - kLeft, kH = kBottom - kTop;
constexpr u32 kPixels = kW * kH;
constexpr u32 kPerStream = 4096;  // Aurora's limit for one capture
constexpr u32 kStreams = (kPixels + kPerStream - 1) / kPerStream;
constexpr u32 kTableFrom = 0x10, kTableTo = 0xEE8;  // dSnap_packet: mPhotoNo .. mResultDetail
constexpr u32 kFlag = 0xEDC, kResult = 0xEE0;

struct Snap {
    bool pending = false;
    u32 round = 0;
    u64 asked = 0;  // gxFrames() when the capture was recorded
    Stream streams[kStreams];
    std::vector<u8> table;   // the packet's subjects as they were at the capture
    std::vector<u32> pixels;  // when the values are known: ARGB by position in the shutter area
    bool answering = false;
} s_snap;

// The capture's pixel `i` of the shutter area, in round `r`: unique over rounds.
u64 pixelKey(u32 round, u32 i) { return u64(round) << 32 | i; }

bool snapAsk(u32 self) {
    for (Stream& s : s_snap.streams)
        if (!create(s)) return false;
    s_snap.round++;
    s_snap.table.resize(kTableTo - kTableFrom);
    for (u32 i = 0; i < s_snap.table.size(); i++) s_snap.table[i] = u8(rd8(self + kTableFrom + i));
    std::vector<GxPeek> peeks;
    for (u32 s = 0; s < kStreams; s++) {
        peeks.clear();
        for (u32 i = s * kPerStream; i < kPixels && i < (s + 1) * kPerStream; i++)
            peeks.push_back({pixelKey(s_snap.round, i), false, u16(kLeft + i % kW), u16(kTop + i / kW)});
        for (GxPeek& p : peeks) p.depth = false;
        if (!gxCapturePeeks(s_snap.streams[s].id, ++s_snap.streams[s].seq, peeks.data(), u32(peeks.size()))) return false;
    }
    s_snap.asked = gxFrames();
    s_snap.pending = true;
    return true;
}

// The values of the latest capture's pixels, if every stream has delivered them.
bool snapCollect() {
    s_snap.pixels.assign(kPixels, 0);
    std::vector<u64> keys;
    std::vector<u32> v;
    std::vector<u8> f;
    for (u32 s = 0; s < kStreams; s++) {
        keys.clear();
        for (u32 i = s * kPerStream; i < kPixels && i < (s + 1) * kPerStream; i++) keys.push_back(pixelKey(s_snap.round, i));
        get(s_snap.streams[s], keys, v, f);
        for (size_t k = 0; k < keys.size(); k++) {
            if (!f[k]) return false;
            const u32 i = s * kPerStream + u32(k);
            const int x = kLeft + int(i % kW), y = kTop + int(i / kW);
            // The game clears the ID plane (the EFB's alpha) only over the quad ClearAlphaBuffer draws:
            // x 174-467, y 131-328. The rest of the shutter area (its top 45 rows) holds whatever the
            // scene left in the alpha bits - undefined, a scatter of IDs on the console - and ours reads
            // 0, which would credit all of it to the first subject. It reads as "no subject" (ID 63).
            const bool cleared = x >= 0xAE && x < 0x1D4 && y >= 0x83 && y < 0x149;
            s_snap.pixels[i] = cleared ? v[k] : 0xFC000000u;
        }
    }
    return true;
}
void logJudged(u32 self) {
    if (!s_log) return;
    log("[efb] photo judged: result %d, %d subjects\n", s32(rd32(self + kResult)), s32(rd32(self + 0x14)));
    for (u32 col = 0; col < rd32(self + 0x14) && col < 0x3F; col++) {
        const u32 o = self + 0x18 + col * 0x3C;
        if (rd32(o + 0x20) || std::getenv("SA2_EFB_IMG"))
            log("[efb]   subject %u at %.1f %.1f %.1f radius %.1f height %.1f flags %02X %02X %02X\n", col, rdf32(o), rdf32(o + 4), rdf32(o + 8), rdf32(o + 0xC), rdf32(o + 0x10), rd8(o + 0x19), rd8(o + 0x1A), rd8(o + 0x1B));
        if (rd32(o + 0x20))
            log("[efb]   subject %u: photo %u, %d pixels, ratio %.3f\n", col, rd8(o + 0x18), s32(rd32(o + 0x20)), rdf32(o + 0x24));
    }
}
}  // namespace

// dSnap_packet::Judge. The first call after the shutter asks for the pixels; later calls (the game
// draws and registers the subjects every frame while the shutter is held) look for them.
HLE(Judge__12dSnap_packetFv) {
    static const GuestFn s_orig = originalOf("Judge__12dSnap_packetFv");
    const u32 self = c.r[3];
    if (s_off || !(rd32(self + kFlag) & 1)) {
        const bool judging = rd32(self + kFlag) & 1;
        s_snap.pending = false;
        s_orig(c);
        if (judging) logJudged(self);
        return;
    }
    if (!s_snap.pending) {
        if (!snapAsk(self)) {  // (no read-back here: the game judges what the EFB gives)
            s_orig(c);
            return;
        }
        if (s_log) log("[efb] frame %llu: photo asked (%u pixels)\n", (unsigned long long)gxFrames(), kPixels);
        return;
    }
    const bool got = snapCollect();
    const u64 waited = gxFrames() - s_snap.asked;
    if (!got && waited < 90) return;
    if (!got) {  // the frame was dropped, or this GPU cannot read back: once more, then nothing
        if (s_snap.round < 3 && snapAsk(self)) return;
        log("[efb] the photo's pixels never came back: judged empty\n");
        s_snap.pixels.assign(kPixels, 0xFC000000);  // (ID 63: no subject)
    }
    if (s_log) {
        u32 hist[64] = {};
        for (u32 p : s_snap.pixels) hist[p >> 26]++;
        std::string s;
        for (u32 i = 0; i < 64; i++)
            if (hist[i]) s += " " + std::to_string(i) + ":" + std::to_string(hist[i]);
        if (const char* dir = std::getenv("SA2_EFB_IMG")) {  // the ids as a picture, one grey level and tint per subject
            std::string path = std::string(dir) + "/photo_" + std::to_string(gxFrames()) + ".ppm";
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", kW, kH);
                for (u32 p : s_snap.pixels) {
                    const u32 id = p >> 26;
                    const u8 rgb[3] = {u8(id * 37), u8(id * 91 + 40), u8(id * 53 + 90)};
                    std::fwrite(id == 63 ? "\0\0\0" : (const char*)rgb, 1, 3, f);
                }
                std::fclose(f);
            }
        }
        log("[efb] frame %llu: photo came back after %llu frames; ids:%s\n", (unsigned long long)gxFrames(), (unsigned long long)waited, s.c_str());
    }
    for (u32 i = 0; i < s_snap.table.size(); i++) wr8(self + kTableFrom + i, s_snap.table[i]);  // the subjects of that frame
    s_snap.answering = true;
    s_orig(c);
    s_snap.answering = false;
    s_snap.pending = false;
    logJudged(self);
}

// mDoGph_screenCapture: the photo (Picto Box) and the other screen captures copy the EFB to RAM (GXCopyTex), and the game
// reads the picture back (encode_s3tc compresses it, asserting its I8 values are 16..235). The copies made inside are read
// back from the GPU into guest memory, and the draw-sync callback waits for them (gx.cpp, gxCopyReadbackArm).
HLE(mDoGph_screenCapture__Fv) {
    static const GuestFn s_orig = originalOf("mDoGph_screenCapture__Fv");
    gxCopyReadbackArm(!s_off);
    s_orig(c);
    gxCopyReadbackArm(false);
}

// SA2_SNAP=<frame>[,<frame>...] (testing): the shutter is released at those game frames, as the Picto Box does.
HLE(dSnap_Execute__Fv) {
    static const GuestFn s_orig = originalOf("dSnap_Execute__Fv");
    static std::vector<u64> s_at = [] {
        std::vector<u64> v;
        if (const char* e = std::getenv("SA2_SNAP"))
            for (const char* p = e; *p;) {
                char* end;
                v.push_back(std::strtoull(p, &end, 10));
                if (end == p) break;
                p = *end == ',' ? end + 1 : end;
            }
        return v;
    }();
    if (s_log && gxFrames() % 120 == 0) log("[efb] frame %llu: dSnap_Execute runs\n", (unsigned long long)gxFrames());
    bool released = false;
    if (rt::bod::testScriptSnap()) released = true, guestCall(c, guestSymbol("dSnap_ReleaseShutter__Fv"));  // (a scenario's `snap`)
    for (u64 f : s_at)
        if (f == gxFrames()) released = true, guestCall(c, guestSymbol("dSnap_ReleaseShutter__Fv"));
    s_orig(c);
    // SA2_SNAP_FAKE=1: Link himself is registered as a photo subject (a figurine type), for scenes with nobody to photograph.
    if (released && std::getenv("SA2_SNAP_FAKE")) {
        const u32 link = guestCall(c, guestSymbol("daPy_getPlayerLinkActorClass__Fv"));
        c.f[1] = c.f[2] = c.f[3] = 1.0;
        if (link) guestCall(c, guestSymbol("dSnap_RegistFig__FUcP10fopAc_ac_cfff"), {0x49, link});
    }
}

// GXPeekARGB(x, y, &color): answered from the photo's pixels while Judge runs.
HLE(GXPeekARGB) {
    static const GuestFn s_orig = originalOf("GXPeekARGB");
    const u32 x = c.r[3] & 0xFFFF, y = c.r[4] & 0xFFFF;
    if (!s_off && s_snap.answering && x >= u32(kLeft) && x < u32(kRight) && y >= u32(kTop) && y < u32(kBottom)) {
        wr32(c.r[5], s_snap.pixels[(y - kTop) * kW + (x - kLeft)]);
        return;
    }
    s_orig(c);
}

}  // namespace rt
