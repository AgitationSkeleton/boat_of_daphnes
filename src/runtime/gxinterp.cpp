// In-between frames for displays faster than the game's frame rate (The Wind Waker's 30 fps; 60, 120, 144 Hz...). The game runs as it
// always does; the renderer shows each frame up to one frame late and, between two frames, draws
// the newer one again with what moves (src/runtime/gx.h: GxPatch) blended from the older one's:
//   - matrices split into rotation, scale and position (rotation slerped, the rest lerped);
//   - texture matrices, lights and the projection lerped;
//   - vertex positions sent with draws (sprites, CPU-skinned models) and vertex arrays the game
//     rewrites, vertex by vertex.
// Values pair up by tag: the game object (task) that drew them and their place in what it drew -
// never by guessing from the picture. A value that jumped (an object teleported, a buffer reused
// for something else) is not blended; when most of the 3D view jumps at once (a camera cut, in
// cutscenes above all), the newer frame is shown as it is.
#include "runtime/gx.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace rt {

std::atomic<int> g_interpJitter{0};
std::atomic<bool> g_interpBaseline{false};

namespace {

u32 rdBe32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
void wrBe32(u8* p, u32 v) {
    p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v);
}
float rdF(const u8* p) { return __builtin_bit_cast(float, rdBe32(p)); }
void wrF(u8* p, float f) { wrBe32(p, __builtin_bit_cast(u32, f)); }

// ---- matrices ----
struct Quat {
    float x, y, z, w;
};
// A 3x3 part (rows r0..r2, stride `rs` floats apart, 3 columns) as rotation * scale; false if it
// is not one (shear, a mirror, a zero axis).
bool decompose(const float* m, int rs, Quat& q, float s[3]) {
    float c[3][3];  // columns
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) c[j][i] = m[i * rs + j];
        s[j] = std::sqrt(c[j][0] * c[j][0] + c[j][1] * c[j][1] + c[j][2] * c[j][2]);
        if (s[j] < 1e-8f) return false;
        for (int i = 0; i < 3; i++) c[j][i] /= s[j];
    }
    auto dot = [&](int a, int b) { return c[a][0] * c[b][0] + c[a][1] * c[b][1] + c[a][2] * c[b][2]; };
    if (std::fabs(dot(0, 1)) > 0.05f || std::fabs(dot(0, 2)) > 0.05f || std::fabs(dot(1, 2)) > 0.05f) return false;
    const float det = c[0][0] * (c[1][1] * c[2][2] - c[2][1] * c[1][2]) - c[1][0] * (c[0][1] * c[2][2] - c[2][1] * c[0][2]) +
                      c[2][0] * (c[0][1] * c[1][2] - c[1][1] * c[0][2]);
    if (det < 0) return false;
    // R[i][j] = c[j][i]
    const float r00 = c[0][0], r01 = c[1][0], r02 = c[2][0], r10 = c[0][1], r11 = c[1][1], r12 = c[2][1], r20 = c[0][2],
                r21 = c[1][2], r22 = c[2][2];
    const float tr = r00 + r11 + r22;
    if (tr > 0) {
        const float k = 0.5f / std::sqrt(tr + 1.f);
        q = {(r21 - r12) * k, (r02 - r20) * k, (r10 - r01) * k, 0.25f / k};
    } else if (r00 > r11 && r00 > r22) {
        const float k = 2.f * std::sqrt(1.f + r00 - r11 - r22);
        q = {0.25f * k, (r01 + r10) / k, (r02 + r20) / k, (r21 - r12) / k};
    } else if (r11 > r22) {
        const float k = 2.f * std::sqrt(1.f + r11 - r00 - r22);
        q = {(r01 + r10) / k, 0.25f * k, (r12 + r21) / k, (r02 - r20) / k};
    } else {
        const float k = 2.f * std::sqrt(1.f + r22 - r00 - r11);
        q = {(r02 + r20) / k, (r12 + r21) / k, 0.25f * k, (r10 - r01) / k};
    }
    return true;
}
Quat slerp(Quat a, Quat b, float t, float* angle) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) b = {-b.x, -b.y, -b.z, -b.w}, d = -d;
    d = std::min(d, 1.f);
    if (angle) *angle = 2.f * std::acos(d);
    float wa, wb;
    if (d > 0.9995f) wa = 1 - t, wb = t;
    else {
        const float th = std::acos(d), sn = std::sin(th);
        wa = std::sin((1 - t) * th) / sn, wb = std::sin(t * th) / sn;
    }
    Quat q{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
void compose(const Quat& q, const float s[3], float* m, int rs) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i * rs + j] = r[i][j] * s[j];
}

enum Verdict { Same, Blend, Jump, Unknown };
constexpr u32 kNewCopy = ~0u;  // a copy of a part with none in the frame before it

u64 mix64(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

// A position (or normal: no translation) matrix of 3 rows of `cols` floats: how the two relate, and
// the blend at t into `out`.
Verdict blendMatrix(const float* a, const float* b, int cols, float t, float* out) {
    Quat qa, qb;
    float sa[3], sb[3];
    if (!decompose(a, cols, qa, sa) || !decompose(b, cols, qb, sb)) {
        // not rotation * scale: element by element, if close
        float maxd = 0, maxv = 1e-6f;
        for (int i = 0; i < 3 * cols; i++) maxd = std::max(maxd, std::fabs(a[i] - b[i])), maxv = std::max(maxv, std::fabs(b[i]));
        if (maxd > 0.5f * maxv) return Jump;
        for (int i = 0; i < 3 * cols; i++) out[i] = a[i] + (b[i] - a[i]) * t;
        return Blend;
    }
    float angle = 0;
    const Quat q = slerp(qa, qb, t, &angle);
    if (angle > 2.5f) return Jump;  // turned more than ~140 degrees in a frame (spinners turn ~100)
    for (int j = 0; j < 3; j++)
        if (sa[j] > 2.f * sb[j] || sb[j] > 2.f * sa[j]) return Jump;
    // Rotation and scale blend separately only where that is what the matrices are and they turned
    // far enough for it to matter. Skinned models' weighted (envelope) matrices mix joints - a little
    // shear, which taking them apart drops: the in-between shape differed from both frames' and the
    // skinned parts shook (the King of Red Lions' hull). Small turns blend value by value, exactly.
    auto exact = [&](const Quat& qq, const float* ss, const float* m) {
        float r[12];
        compose(qq, ss, r, cols);
        float e = 0, mag = 1e-6f;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) e = std::max(e, std::fabs(r[i * cols + j] - m[i * cols + j])), mag = std::max(mag, std::fabs(m[i * cols + j]));
        return e <= 1e-3f * mag;
    };
    if (angle < 0.1f || !exact(qa, sa, a) || !exact(qb, sb, b)) {
        for (int i = 0; i < 3 * cols; i++) out[i] = a[i] + (b[i] - a[i]) * t;
    } else {
        float s[3];
        for (int j = 0; j < 3; j++) s[j] = sa[j] + (sb[j] - sa[j]) * t;
        compose(q, s, out, cols);
    }
    if (cols == 4) {
        float da = 0, db = 0, dd = 0;
        for (int i = 0; i < 3; i++) {
            const float ta = a[i * 4 + 3], tb = b[i * 4 + 3];
            da += ta * ta, db += tb * tb, dd += (tb - ta) * (tb - ta);
            out[i * 4 + 3] = ta + (tb - ta) * t;
        }
        // moved more than 60% of its distance from the eye (and more than a few units)
        if (std::sqrt(dd) > std::max(30.f, 0.6f * std::sqrt(std::max(da, db)))) return Jump;
    }
    return Blend;
}

// 3x4 affine matrices (rows of 4: rotation/scale | translation)
bool invertAffine(const float* m, float* o);
// The camera at t: its rotation slerped (always - a camera is a pure rotation, and blending its values shrinks the picture
// a little mid-turn), and its eye moving straight from one place to the other. The view's own translation column is the
// eye seen through the rotation, so blending it directly swings the eye around as the camera turns.
bool blendView(const float* a, const float* b, float t, float* out) {
    Quat qa, qb;
    float sa[3], sb[3], ia[12], ib[12];
    if (!decompose(a, 4, qa, sa) || !decompose(b, 4, qb, sb) || !invertAffine(a, ia) || !invertAffine(b, ib))
        return blendMatrix(a, b, 4, t, out) == Blend;
    float angle = 0;
    const Quat q = slerp(qa, qb, t, &angle);
    if (angle > 1.5f) return false;  // turned more than ~85 degrees in a frame: a cut
    float sc[3];
    for (int j = 0; j < 3; j++) sc[j] = sa[j] + (sb[j] - sa[j]) * t;
    compose(q, sc, out, 4);
    float eye[3];
    for (int i = 0; i < 3; i++) eye[i] = ia[i * 4 + 3] + (ib[i * 4 + 3] - ia[i * 4 + 3]) * t;
    for (int i = 0; i < 3; i++) out[i * 4 + 3] = -(out[i * 4] * eye[0] + out[i * 4 + 1] * eye[1] + out[i * 4 + 2] * eye[2]);
    return true;
}
void mulAffine(const float* x, const float* y, float* o) {  // o = x * y
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            float s = c == 3 ? x[r * 4 + 3] : 0.f;
            for (int k = 0; k < 3; k++) s += x[r * 4 + k] * y[k * 4 + c];
            o[r * 4 + c] = s;
        }
}
bool invertAffine(const float* m, float* o) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), Cc = d * h - e * g;
    const float det = a * A + b * B + c * Cc;
    if (std::fabs(det) < 1e-12f) return false;
    const float k = 1.f / det;
    const float r[9] = {A * k, -(b * i - c * h) * k, (b * f - c * e) * k,
                        B * k, (a * i - c * g) * k, -(a * f - c * d) * k,
                        Cc * k, -(a * h - b * g) * k, (a * e - b * d) * k};
    for (int row = 0; row < 3; row++) {
        o[row * 4 + 0] = r[row * 3 + 0], o[row * 4 + 1] = r[row * 3 + 1], o[row * 4 + 2] = r[row * 3 + 2];
        o[row * 4 + 3] = -(r[row * 3 + 0] * m[3] + r[row * 3 + 1] * m[7] + r[row * 3 + 2] * m[11]);
    }
    return true;
}

// ---- vertex components ----
float rdComp(const u8* p, u32 fmt, float scale);
void wrComp(u8* p, u32 fmt, float v, float scale);
u32 compBytes(u32 fmt);
// A vertex array the CPU rewrote (a skinned model's positions): vertex by vertex - the snapshot runs
// past the vertices drawn (it is rounded up), into data that may be anything; a vertex that moved
// far (or is not a position) keeps the newer frame's value, the rest blend.
Verdict blendArray(const u8* pa, const u8* pb, u8* out, u32 n, u32 stride, u32 comps, u32 fmt, u32 shift, float t) {
    const float scale = fmt == 4 ? 1.f : 1.f / float(1u << shift);
    const u32 cb = compBytes(fmt);
    bool any = false;
    for (u32 v = 0; v < n; v++) {
        float a[3], b[3], d = 0;
        bool ok = true;
        for (u32 c = 0; c < comps; c++) {
            a[c] = rdComp(pa + v * stride + c * cb, fmt, scale), b[c] = rdComp(pb + v * stride + c * cb, fmt, scale);
            ok = ok && std::fabs(a[c]) < 1e7f && std::fabs(b[c]) < 1e7f;
            d = std::max(d, std::fabs(b[c] - a[c]));
        }
        if (!ok || d == 0 || d > 300.f) continue;
        for (u32 c = 0; c < comps; c++) wrComp(out + v * stride + c * cb, fmt, a[c] + (b[c] - a[c]) * t, scale);
        any = true;
    }
    return any ? Blend : Same;
}
float rdComp(const u8* p, u32 fmt, float scale) {
    switch (fmt) {
    case 0: return p[0] * scale;
    case 1: return s8(p[0]) * scale;
    case 2: return u16(p[0] << 8 | p[1]) * scale;
    case 3: return s16(p[0] << 8 | p[1]) * scale;
    default: return rdF(p);
    }
}
void wrComp(u8* p, u32 fmt, float v, float scale) {
    const float x = std::nearbyint(v / scale);
    switch (fmt) {
    case 0: p[0] = u8(std::clamp(x, 0.f, 255.f)); break;
    case 1: p[0] = u8(s8(std::clamp(x, -128.f, 127.f))); break;
    case 2: { const u16 u = u16(std::clamp(x, 0.f, 65535.f)); p[0] = u8(u >> 8), p[1] = u8(u); break; }
    case 3: { const s16 u = s16(std::clamp(x, -32768.f, 32767.f)); p[0] = u8(u16(u) >> 8), p[1] = u8(u); break; }
    default: wrF(p, v); break;
    }
}
u32 compBytes(u32 fmt) { return fmt == 4 ? 4 : fmt >= 2 ? 2 : 1; }

// Positions of n vertices (a at pa, b at pb, `stride` apart): Jump if one moved far more than the
// size of the whole (a teleport, a buffer reused for something else); else blended into out.
Verdict blendPositions(const u8* pa, const u8* pb, u8* out, u32 n, u32 stride, u32 comps, u32 fmt, u32 shift, float t) {
    const float scale = fmt == 4 ? 1.f : 1.f / float(1u << shift);
    const u32 cb = compBytes(fmt);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, maxd = 0;
    bool same = true;
    for (u32 v = 0; v < n; v++) {
        for (u32 c = 0; c < comps; c++) {
            const float x = rdComp(pa + v * stride + c * cb, fmt, scale), y = rdComp(pb + v * stride + c * cb, fmt, scale);
            if (!(std::fabs(x) < 1e9f && std::fabs(y) < 1e9f)) return Jump;  // not positions after all
            lo[c] = std::min(lo[c], y), hi[c] = std::max(hi[c], y);
            maxd = std::max(maxd, std::fabs(y - x));
            same = same && x == y;
        }
    }
    if (same) return Same;
    float ext = 0;
    for (u32 c = 0; c < comps; c++) ext = std::max(ext, hi[c] - lo[c]);
    if (maxd > std::max(2.f * ext, 64.f)) return Jump;
    for (u32 v = 0; v < n; v++)
        for (u32 c = 0; c < comps; c++) {
            const u8* x = pa + v * stride + c * cb;
            const u8* y = pb + v * stride + c * cb;
            const float a = rdComp(x, fmt, scale), b = rdComp(y, fmt, scale);
            wrComp(out + v * stride + c * cb, fmt, a + (b - a) * t, scale);
        }
    return Blend;
}

// n vertices (float x, y, z, `stride` apart) through the affine matrix m.
void moveVertices(const u8* src, u8* dst, u32 n, u32 stride, const float* m) {
    for (u32 v = 0; v < n; v++, src += stride, dst += stride) {
        const float x = rdF(src), y = rdF(src + 4), z = rdF(src + 8);
        wrF(dst, m[0] * x + m[1] * y + m[2] * z + m[3]);
        wrF(dst + 4, m[4] * x + m[5] * y + m[6] * z + m[7]);
        wrF(dst + 8, m[8] * x + m[9] * y + m[10] * z + m[11]);
    }
}
// The points of a trail (camera-space float vertices) in two frames, A's (na) and B's (nb), into out at t: each of B's is blended
// (in the world) from A's point at the same place along the trail - the best shift of the index, A's nearest end where B has more.
void blendWorldVertices(const u8* pa, u32 na, u32 sa, const u8* pb, u32 nb, u32 sb, u8* out, const float* invA, const float* invB,
                        const float* vt, float t) {
    if (!na || !nb) return;
    std::vector<float> wa(size_t(na) * 3), wb(size_t(nb) * 3);
    auto toWorld = [](const u8* p, u32 n, u32 stride, const float* inv, float* w) {
        for (u32 v = 0; v < n; v++, p += stride) {
            const float x = rdF(p), y = rdF(p + 4), z = rdF(p + 8);
            for (int r = 0; r < 3; r++) w[v * 3 + r] = inv[r * 4] * x + inv[r * 4 + 1] * y + inv[r * 4 + 2] * z + inv[r * 4 + 3];
        }
    };
    if (invA) {
        toWorld(pa, na, sa, invA, wa.data());
        toWorld(pb, nb, sb, invB, wb.data());
    } else {  // already positions in the world
        auto copy = [](const u8* p, u32 n, u32 stride, float* w) {
            for (u32 v = 0; v < n; v++, p += stride) w[v * 3] = rdF(p), w[v * 3 + 1] = rdF(p + 4), w[v * 3 + 2] = rdF(p + 8);
        };
        copy(pa, na, sa, wa.data());
        copy(pb, nb, sb, wb.data());
    }
    constexpr int kMaxShift = 12;
    auto cost = [&](int s) {
        double sum = 0;
        int n = 0;
        for (int i = 0; i < int(nb); i++) {
            const int j = i + s;
            if (j < 0 || j >= int(na)) continue;
            double d = 0;
            for (int r = 0; r < 3; r++) d += double(wb[i * 3 + r] - wa[j * 3 + r]) * (wb[i * 3 + r] - wa[j * 3 + r]);
            sum += std::sqrt(d), n++;
        }
        return n * 2 >= int(std::min(na, nb)) ? sum / n : 1e30;
    };
    int best = 0;
    double bc = cost(0);
    for (int s = -kMaxShift; s <= kMaxShift; s++)
        if (const double c = cost(s); c < bc * 0.7) best = s, bc = c;  // (0 unless another is clearly better)
    float m[3];
    for (u32 i = 0; i < nb; i++) {
        const int j = std::clamp(int(i) + best, 0, int(na) - 1);
        float w[3];
        for (int r = 0; r < 3; r++) {
            const float a = wa[size_t(j) * 3 + r], b = wb[size_t(i) * 3 + r];
            w[r] = std::fabs(b - a) > 300.f ? b : a + (b - a) * t;
        }
        if (vt) {
            for (int r = 0; r < 3; r++) m[r] = vt[r * 4] * w[0] + vt[r * 4 + 1] * w[1] + vt[r * 4 + 2] * w[2] + vt[r * 4 + 3];
        } else {
            for (int r = 0; r < 3; r++) m[r] = w[r];
        }
        for (int r = 0; r < 3; r++) wrF(out + i * sb + r * 4, m[r]);
    }
}

// A texture matrix that is a 2D rotation, scale and scroll (J3D's texture SRT, the particles' texture animation): rows of 4, the
// 2x2 part in [0][0..1] and [1][0..1] (columns 0 and 1 of rows 0 and 1), the scroll in [0][3], [1][3]. Blended as what it is:
// the angle (the short way round), the scale, and the point it turns about - blending the values themselves shrinks the picture
// mid-turn (a spinning ray or glint pulses at the frame rate) - and a scroll that wrapped around (0.9 -> 0.1) carries on past 1.
// false: not one (a projection, a mirror, almost no change): the values blend as they are.
bool blendTexSrt(const float* a, const float* b, u32 count, float t, float* out) {
    for (const float* m : {a, b}) {
        if (m[2] != 0.f || m[6] != 0.f) return false;
        if (count == 12 && (m[8] != 0.f || m[9] != 0.f || m[10] != 1.f || m[11] != 0.f)) return false;
    }
    auto sim = [](const float* m, float& s, float& th) {
        s = std::hypot(m[0], m[4]);
        th = std::atan2(m[4], m[0]);
        return s > 1e-6f && std::fabs(m[0] - m[5]) <= 1e-3f * s && std::fabs(m[1] + m[4]) <= 1e-3f * s;
    };
    for (u32 q = 0; q < count; q++) out[q] = a[q] + (b[q] - a[q]) * t;
    // the scroll that wrapped: carried on, past 1, the way the texture repeats
    for (const u32 q : {3u, 7u}) {
        const float d = b[q] - a[q];
        if (std::fabs(d) > 0.5f) out[q] = a[q] + (d - std::round(d)) * t;
    }
    float sa, ta, sb, tb;
    if (!sim(a, sa, ta) || !sim(b, sb, tb)) return true;  // (scroll and scale only: the values blend)
    float dth = tb - ta;
    while (dth > 3.14159265f) dth -= 6.2831853f;
    while (dth < -3.14159265f) dth += 6.2831853f;
    if (std::fabs(dth) < 0.02f) return true;
    // the point each turns about: (I - M) c = scroll
    auto pivot = [](const float* m, float* c) {
        const float det = (1 - m[0]) * (1 - m[5]) - m[1] * m[4];
        if (std::fabs(det) < 1e-3f) return false;
        c[0] = ((1 - m[5]) * m[3] + m[1] * m[7]) / det;
        c[1] = (m[4] * m[3] + (1 - m[0]) * m[7]) / det;
        return std::fabs(c[0]) < 100.f && std::fabs(c[1]) < 100.f;
    };
    float ca[2], cb[2];
    if (!pivot(a, ca) || !pivot(b, cb)) return true;
    const float s = sa + (sb - sa) * t, th = ta + dth * t;
    const float c0 = ca[0] + (cb[0] - ca[0]) * t, c1 = ca[1] + (cb[1] - ca[1]) * t;
    const float co = s * std::cos(th), si = s * std::sin(th);
    out[0] = co, out[1] = -si, out[4] = si, out[5] = co;
    out[3] = (1 - co) * c0 + si * c1;
    out[7] = -si * c0 + (1 - co) * c1;
    return true;
}

struct Shown {
    std::shared_ptr<GxFrame> f;
    std::vector<int> match;  // per patch: the previous frame's patch it blends from (-1: none, -2: unchanged)
    bool cut = false;
    double pts = 0;          // when it is shown (steady 60 Hz steps following the game's frames)
};
std::deque<Shown> s_hist;  // oldest first
u32 s_starved = 0;         // builds past the newest frame's time (for the stats)
double s_gap = 1.0 / 59.94;  // time between frames (smoothed)
double s_lateMax = 0.004;    // how long after its retrace a frame arrives, at most lately

// One copy of a model part in a frame: which draw of it, and where (view space).
struct Copy {
    u32 occ;
    float t[3];
};

// Pairs the copies of a part in two frames (B's copy -> A's, or kNewCopy). They come in drawing
// order, which mostly holds from frame to frame while some come and go: aligned like two texts
// (an edit distance, in a band around the diagonal - effects draw thousands). Pairing two costs
// how far it moved against how far it may move in a frame (blendMatrix's jump: 60% of its distance
// from the eye, at least 30 units; farther cannot pair); leaving one out costs half that.
void alignCopies(const std::vector<Copy>& va, const std::vector<Copy>& vb, std::unordered_map<u32, u32>& map) {
    const int na = int(va.size()), nb = int(vb.size());
    const int band = std::min(96, 16 + std::abs(na - nb)), width = 2 * band + 1;
    constexpr float kGap = 0.5f, kInf = 1e30f;
    auto center = [&](int i) { return nb ? int(s64(i) * na / nb) : 0; };
    std::vector<float> cost(size_t(nb + 1) * width, kInf);
    std::vector<u8> from(size_t(nb + 1) * width, 0);  // 0 paired, 1 B's copy new, 2 A's copy gone
    auto cell = [&](int i, int j) -> int {
        const int k = j - center(i) + band;
        return i < 0 || j < 0 || j > na || k < 0 || k >= width ? -1 : i * width + k;
    };
    auto at = [&](int i, int j) { const int c = cell(i, j); return c < 0 ? kInf : cost[size_t(c)]; };
    for (int i = 0; i <= nb; i++)
        for (int j = std::max(0, center(i) - band); j <= std::min(na, center(i) + band); j++) {
            const int c = cell(i, j);
            if (i == 0 && j == 0) {
                cost[size_t(c)] = 0;
                continue;
            }
            float best = kInf;
            u8 how = 0;
            if (i > 0 && j > 0) {
                const Copy &x = vb[size_t(i - 1)], &y = va[size_t(j - 1)];
                float d = 0, r = 0;
                for (int k = 0; k < 3; k++) d += (x.t[k] - y.t[k]) * (x.t[k] - y.t[k]), r += x.t[k] * x.t[k];
                const float rel = std::sqrt(d) / std::max(30.f, 0.6f * std::sqrt(r));
                if (rel <= 1.f) best = at(i - 1, j - 1) + rel, how = 0;
            }
            if (i > 0 && at(i - 1, j) + kGap < best) best = at(i - 1, j) + kGap, how = 1;
            if (j > 0 && at(i, j - 1) + kGap < best) best = at(i, j - 1) + kGap, how = 2;
            cost[size_t(c)] = best, from[size_t(c)] = how;
        }
    if (at(nb, na) >= kInf) {  // counts too far apart for the band: all of B's are new
        for (const Copy& c : vb) map[c.occ] = kNewCopy;
        return;
    }
    for (int i = nb, j = na; i > 0 || j > 0;) {
        const int c = cell(i, j);
        const u8 how = c < 0 ? (i > 0 ? 1 : 2) : from[size_t(c)];
        if (how == 0) map[vb[size_t(i - 1)].occ] = va[size_t(j - 1)].occ, i--, j--;
        else if (how == 1) map[vb[size_t(i - 1)].occ] = kNewCopy, i--;
        else j--;
    }
}

void matchFrames(const GxFrame& a, Shown& bs) {
    const GxFrame& b = *bs.f;
    bs.match.assign(b.patches.size(), -1);
    std::unordered_map<u64, int> byKey, byKey2;
    byKey.reserve(a.patches.size() * 2);
    byKey2.reserve(a.patches.size() * 2);
    for (size_t i = 0; i < a.patches.size(); i++) {
        byKey.emplace(a.patches[i].key, int(i));
        if (a.patches[i].key2 != a.patches[i].key) byKey2.emplace(a.patches[i].key2, int(i));
    }
    // Copies of one model part an object draws (rings, repeated pieces, a skinned model's batches,
    // particles) pair by position along their drawing order (alignCopies): one copy culled or
    // spawned must not shift every pairing after it.
    auto copies = [](const GxFrame& f) {
        // Where each copy is: its first position matrix's translation, or (drawn without one) its
        // first vertex.
        std::unordered_map<u64, std::vector<Copy>> out;
        for (const GxPatch& p : f.patches) {
            if (!p.group || p.flat) continue;
            Copy c{p.occ, {}};
            if (p.kind == GxPatch::PosMtx && p.count == 12) {
                const u8* m = f.cmds.data() + p.at;
                c.t[0] = rdF(m + 12), c.t[1] = rdF(m + 28), c.t[2] = rdF(m + 44);
            } else if (p.kind == GxPatch::Vertex && p.count) {
                const float scale = p.fmt == 4 ? 1.f : 1.f / float(1u << p.shift);
                for (u32 k = 0; k < p.comps && k < 3; k++) c.t[k] = rdComp(f.cmds.data() + p.at + p.posOff + k * compBytes(p.fmt), p.fmt, scale);
            } else continue;
            auto& v = out[p.group];
            if (!v.empty() && v.back().occ == p.occ) continue;  // the copy's first one stands for it
            v.push_back(c);
        }
        return out;
    };
    const auto ca = copies(a), cb = copies(b);
    std::unordered_map<u64, std::unordered_map<u32, u32>> occMap;  // group -> B copy -> A copy
    for (const auto& [g, vb] : cb) {
        const auto ia = ca.find(g);
        if (ia == ca.end()) continue;
        const auto& va = ia->second;
        auto& map = occMap[g];
        if (va.size() == 1 && vb.size() == 1) {
            map[vb[0].occ] = va[0].occ;
            continue;
        }
        alignCopies(va, vb, map);
    }
    std::unordered_map<u64, int> byCopy;  // group, copy, kind, XF address / stride, count -> A patch
    // (Vertex arrays: not by size - the extent snapshotted depends on which draw read the array first.)
    auto copyKey = [](const GxPatch& p, u32 occ) {
        return mix64(p.group ^ mix64(u64(occ) << 40 ^ u64(p.kind) << 32 ^ u64(p.stride) << 12 ^ (p.kind == GxPatch::Array ? 0 : p.count)));
    };
    for (size_t i = 0; i < a.patches.size(); i++)
        if (a.patches[i].group) byCopy.emplace(copyKey(a.patches[i], a.patches[i].occ), int(i));
    u32 moved = 0, jumped = 0;
    float tmp[12];
    // SA2_INTERP_DEBUG=<frame>: that frame's jumps; =cut: every cut frame's.
    static const char* s_debugEnv = std::getenv("SA2_INTERP_DEBUG");
    static const bool s_debug = s_debugEnv != nullptr;
    static const u64 s_debugFrame = s_debug ? std::strtoull(s_debugEnv, nullptr, 10) : 0;
    std::vector<std::string> debugLines;
    u32 unpaired[7] = {};  // per kind: values with nothing to blend from
    // Geometry the CPU rebuilds from particles each frame (wakes, wave fans, ripples) pairs by the emitter's draw, not by values.
    std::unordered_map<u64, int> worldA;
    for (size_t i = 0; i < a.patches.size(); i++)
        if (a.patches[i].world && a.patches[i].group) worldA.emplace(mix64(a.patches[i].group ^ mix64(a.patches[i].occ)), int(i));
    for (size_t i = 0; i < b.patches.size(); i++) {
        const GxPatch& pb = b.patches[i];
        if (pb.world && !g_interpBaseline) {
            if (pb.group)
                if (const auto w = worldA.find(mix64(pb.group ^ mix64(pb.occ))); w != worldA.end()) bs.match[i] = w->second;
            continue;
        }
        if (pb.flat) continue;  // 2D: drawn as the game drew it
        if (pb.height && (pb.kind == GxPatch::Array || pb.kind == GxPatch::Vertex)) {
            bs.match[i] = -3;  // a height field: blended by position when drawn (gxInterpBuild)
            continue;
        }
        // By model part and copy; a part the CPU rebuilt elsewhere this frame (a skinned mesh's
        // buffer) by its place in what the object drew.
        int from = -1;
        bool placed = false;  // a copy paired by position (or, left out, new)
        if (pb.group)
            if (const auto g = occMap.find(pb.group); g != occMap.end())
                if (const auto o = g->second.find(pb.occ); o != g->second.end()) {
                    placed = true;
                    if (o->second != kNewCopy)
                        if (const auto c = byCopy.find(copyKey(pb, o->second)); c != byCopy.end()) from = c->second;
                }
        if (from < 0 && !placed) {  // not a placed copy (no matrix of its own; a buffer that moves)
            if (const auto k = byKey.find(pb.key); k != byKey.end()) from = k->second;
            else if (pb.kind == GxPatch::Vertex || pb.kind == GxPatch::Array)
                if (const auto k2 = byKey2.find(pb.key2); k2 != byKey2.end()) from = k2->second;
        }
        if (from < 0) {
            unpaired[pb.kind]++;
            // A placed copy with none to pair with moved too far (or is new): at a camera cut, most.
            if (placed && pb.kind == GxPatch::PosMtx && pb.count == 12 && !pb.flat) moved++, jumped++;
            continue;
        }
        const GxPatch& pa = a.patches[size_t(from)];
        if (pa.kind != pb.kind || pa.stride != pb.stride || pa.fmt != pb.fmt) continue;
        if (pa.count != pb.count && pb.kind != GxPatch::Array) continue;  // (arrays: their shared extent blends)
        const u8* va = pa.kind == GxPatch::Array ? pa.data : a.cmds.data() + pa.at;
        const u8* vb = pb.kind == GxPatch::Array ? pb.data : b.cmds.data() + pb.at;
        const size_t bytes = pb.kind == GxPatch::Array ? std::min(pa.count, pb.count) : pb.kind == GxPatch::Vertex ? size_t(pb.count) * pb.stride : size_t(pb.count) * 4;
        if (!std::memcmp(va, vb, bytes)) {  // nothing moved (2: left as it is, not a value with no partner)
            bs.match[i] = -2;
            continue;
        }
        if (pb.kind == GxPatch::PosMtx && pb.count == 12) {
            float ma[12], mb[12];
            for (int k = 0; k < 12; k++) ma[k] = rdF(va + 4 * k), mb[k] = rdF(vb + 4 * k);
            const Verdict v = blendMatrix(ma, mb, 4, 0.5f, tmp);
            const bool flat = std::fabs(mb[8]) < 1e-6f && std::fabs(mb[9]) < 1e-6f;  // 2D (sprite) matrices don't count
            if (!flat) moved++, jumped += v == Jump;
            if (s_debug && v == Jump && jumped <= 8) {
                char line[300];
                std::snprintf(line, sizeof line,
                              "[interp] jump: group %016llX copy %u (B copy %u; copies %zu, %zu) xf %X: T %.1f %.1f %.1f -> %.1f %.1f %.1f; row0 %.3f %.3f %.3f -> "
                              "%.3f %.3f %.3f\n",
                              (unsigned long long)pb.group, pa.occ, pb.occ, ca.count(pb.group) ? ca.at(pb.group).size() : size_t(0),
                              cb.count(pb.group) ? cb.at(pb.group).size() : size_t(0), pb.stride, ma[3], ma[7], ma[11], mb[3], mb[7], mb[11], ma[0], ma[1],
                              ma[2], mb[0], mb[1], mb[2]);
                debugLines.push_back(line);
                for (const auto* side : {&ca, &cb})  // where that part's copies were, in each frame
                    if (const auto g = side->find(pb.group); g != side->end() && g->second.size() <= 8) {
                        std::string s = side == &ca ? "[interp]   A copies:" : "[interp]   B copies:";
                        for (const Copy& c : g->second) {
                            std::snprintf(line, sizeof line, " %u@(%.0f %.0f %.0f)", c.occ, c.t[0], c.t[1], c.t[2]);
                            s += line;
                        }
                        debugLines.push_back(s + "\n");
                    }
            }
            if (v == Jump) continue;
        }
        bs.match[i] = from;
    }
    // Most of the view jumped at once: a camera cut.
    bs.cut = moved >= 8 && jumped * 10 > moved * 4;
    static const bool s_debugAll = s_debug && !std::strcmp(s_debugEnv, "all");  // =all: every frame's
    if (s_debug && (b.index == s_debugFrame || (!s_debugFrame && bs.cut) || (s_debugAll && !bs.cut)))
        for (const std::string& l : debugLines) log("%s", l.c_str());
    // SA2_INTERP_STATS: per kind, values / blended (moved and paired), every 300 frames.
    static const bool s_stats = std::getenv("SA2_INTERP_STATS") != nullptr;
    if (s_stats && (b.index % 300 == 0 || bs.cut)) {
        static const char* names[] = {"pos", "nrm", "tex", "light", "proj", "vtx", "array"};
        u32 tot[7] = {}, got[7] = {};
        for (size_t i = 0; i < b.patches.size(); i++) tot[b.patches[i].kind]++, got[b.patches[i].kind] += bs.match[i] >= 0;
        std::string s;
        char buf[48];
        for (int k = 0; k < 7; k++) {
            std::snprintf(buf, sizeof buf, " %s %u/%u (%u unpaired)", names[k], got[k], tot[k], unpaired[k]);
            s += buf;
        }
        log("[interp] frame %llu:%s; 3D matrices moved %u, jumped %u%s\n", (unsigned long long)b.index, s.c_str(), moved, jumped,
            bs.cut ? " -> CUT" : "");
    }
}

}  // namespace

void gxInterpPush(std::shared_ptr<GxFrame> f) {
    Shown s;
    s.f = std::move(f);
    // A frame is shown at the retrace its work began on (the game starts each frame when the
    // retrace count moves): even steps however long each frame took, so the blend moves evenly.
    // It arrives that frame's work later; the display runs far enough behind for the slowest
    // recent frame (gxInterpDelay).
    const double vi = s.f->vi > 0 ? s.f->vi : s.f->time;
    s.pts = s_hist.empty() ? vi : std::max(vi, s_hist.back().pts + 0.001);
    const double late = s.f->time - s.pts;
    s_lateMax = std::clamp(std::max(late, s_lateMax - 0.00002), 0.0, 0.050);  // forgets 1.2 ms a second
    if (!s_hist.empty()) {
        const double gap = s.pts - s_hist.back().pts;
        if (gap < 0.1) s_gap = 0.95 * s_gap + 0.05 * gap;
        // SA2_INTERP_STATS: how late frames come after their retrace, and how often the display
        // ran out of frames (showed the newest one with nothing to blend towards).
        static const bool s_stats = std::getenv("SA2_INTERP_STATS") != nullptr;
        static double s_lateSum = 0;
        static u32 s_n = 0;
        s_lateSum += late, s_n++;
        if (s_stats && s_n == 300) {
            log("[interp] timing: step %.2f ms, frames %.2f ms after their retrace on average, delay %.2f ms; display starved %u "
                "times\n",
                s_gap * 1000, s_lateSum / s_n * 1000, gxInterpDelay() * 1000, s_starved);
            s_lateSum = 0, s_n = 0, s_starved = 0;
        }
    }
    if (!s_hist.empty()) matchFrames(*s_hist.back().f, s);
    s_hist.push_back(std::move(s));
    while (s_hist.size() > 4) s_hist.pop_front();
}

void gxInterpReset() { s_hist.clear(); }

const GxFrame* gxInterpNewest() { return s_hist.empty() ? nullptr : s_hist.back().f.get(); }

double gxInterpNewestTime() { return s_hist.empty() ? 0 : s_hist.back().pts; }

double gxInterpNewestStep() { return s_hist.size() < 2 ? s_gap : s_hist.back().pts - s_hist[s_hist.size() - 2].pts; }

double gxInterpDelay() { return s_gap + s_lateMax + 0.0015; }

double gxInterpShowTime(double now) {
    // The display's clock runs `delay` behind; when the delay changes it catches up gently (up to
    // 10% faster or slower than real time), never stepping back - a step back would move
    // everything backwards for a moment.
    static double s_when = -1, s_lastNow = 0;
    const double target = now - gxInterpDelay(), dt = now - s_lastNow;
    s_lastNow = now;
    if (s_when < 0 || std::fabs(target - s_when) > 0.1 || dt <= 0 || dt > 0.1) return s_when = target;
    s_when += dt + std::clamp(target - (s_when + dt), -0.1 * dt, 0.1 * dt);
    return s_when;
}

static std::shared_ptr<GxFrame> buildImpl(double when, std::vector<u8>& out, std::vector<std::unique_ptr<u8[]>>& keep, float* shownT) {
    if (s_hist.empty()) return nullptr;
    // The two frames `when` falls between: the newer drawn with the older's values blended in.
    size_t k = s_hist.size() - 1;
    float t = 1.f;
    static double s_lastWhen = 0;
    if (when >= s_hist.back().pts && s_lastWhen < s_hist.back().pts) s_starved++;
    s_lastWhen = when;
    if (when < s_hist.back().pts) {
        k = 0;
        while (k + 1 < s_hist.size() && s_hist[k].pts <= when) k++;
        if (k > 0 && s_hist[k].pts > s_hist[k - 1].pts)
            t = float((when - s_hist[k - 1].pts) / (s_hist[k].pts - s_hist[k - 1].pts));
        t = std::clamp(t, 0.f, 1.f);
        if (k == 0) t = 1.f;
    }
    const Shown& sb = s_hist[k];
    const GxFrame& b = *sb.f;
    if (shownT) *shownT = t;
    out.resize(b.preamble.size() + b.cmds.size());
    std::memcpy(out.data(), b.preamble.data(), b.preamble.size());
    std::memcpy(out.data() + b.preamble.size(), b.cmds.data(), b.cmds.size());
    if (t >= 1.f || k == 0 || sb.cut) return sb.f;
    const GxFrame& a = *s_hist[k - 1].f;
    u8* cmds = out.data() + b.preamble.size();
    // The camera at t, and what takes B's camera there (C = V(t) * V(B)^-1): applied to 3D matrices
    // with nothing to blend from (particles, effects come and gone, a value that jumped), so they
    // stay where they are in the world instead of moving with B's camera against everything else.
    bool haveC = false;
    float C[12];
    // The camera is blended once for the whole frame (vt), and each 3D matrix of the main view is blended as the object's
    // place in the world (the camera taken off both frames' matrices), then seen through vt: everything that stands still
    // in the world moves exactly with the one camera. Blending each camera-times-object matrix on its own turned the
    // objects by slightly different amounts mid-turn (some slerped, some blended value by value): the scene shook
    // whenever the camera turned.
    bool haveV = false;
    float vt[12], invA[12], invB[12];
    if (a.mainView >= 0 && b.mainView >= 0) {
        const auto& va = a.views[size_t(a.mainView)];
        const auto& vb = b.views[size_t(b.mainView)];
        if (blendView(va.data(), vb.data(), t, vt) && invertAffine(vb.data(), invB)) {
            mulAffine(vt, invB, C);
            haveC = true;
            haveV = invertAffine(va.data(), invA);
        }
    }
    // Height fields (the sea): the previous frame's heights by world x, z (bit patterns: the grid's
    // points recur exactly as it scrolls).
    std::unordered_map<u64, float> heights;
    bool heightsBuilt = false;
    auto heightOf = [&](const GxPatch& p, const u8* data, u32 v, float* xyz) {
        const float scale = p.fmt == 4 ? 1.f : 1.f / float(1u << p.shift);
        const u32 cb = compBytes(p.fmt);
        const u8* q = data + v * p.stride + (p.kind == GxPatch::Vertex ? p.posOff : 0);
        for (u32 c = 0; c < 3; c++) xyz[c] = rdComp(q + c * cb, p.fmt, scale);
        return q;
    };
    auto hkey = [](float x, float z) { return u64(__builtin_bit_cast(u32, x)) << 32 | __builtin_bit_cast(u32, z); };
    for (size_t i = 0; i < b.patches.size(); i++) {
        const int j = b.patches[i].world && g_interpBaseline ? -1 : sb.match[i];
        if (j == -3) {
            const GxPatch& pb = b.patches[i];
            if (pb.comps != 3) continue;
            if (!heightsBuilt) {
                heightsBuilt = true;
                for (const GxPatch& pa : a.patches) {
                    if (!pa.height || pa.comps != 3) continue;
                    const u8* data = pa.kind == GxPatch::Array ? pa.data : a.cmds.data() + pa.at;
                    const u32 n = pa.kind == GxPatch::Array ? pa.count / pa.stride : pa.count;
                    for (u32 v = 0; v < n; v++) {
                        float p[3];
                        heightOf(pa, data, v, p);
                        if (std::fabs(p[1]) < 1e7f) heights[hkey(p[0], p[2])] = p[1];
                    }
                }
            }
            if (heights.empty()) continue;
            const float scale = pb.fmt == 4 ? 1.f : 1.f / float(1u << pb.shift);
            const u32 cb = compBytes(pb.fmt);
            auto blendInto = [&](u8* dst, const u8* src, u32 n) {
                for (u32 v = 0; v < n; v++) {
                    float p[3];
                    heightOf(pb, src, v, p);
                    const auto h = heights.find(hkey(p[0], p[2]));
                    if (h == heights.end() || std::fabs(p[1] - h->second) > 300.f) continue;
                    u8* q = dst + v * pb.stride + (pb.kind == GxPatch::Vertex ? pb.posOff : 0);
                    wrComp(q + cb, pb.fmt, h->second + (p[1] - h->second) * t, scale);
                }
            };
            if (pb.kind == GxPatch::Vertex) {
                blendInto(cmds + pb.at, b.cmds.data() + pb.at, pb.count);
            } else {
                std::unique_ptr<u8[]> mem(new u8[pb.count]);
                std::memcpy(mem.get(), pb.data, pb.count);
                blendInto(mem.get(), pb.data, pb.count / pb.stride);
                const u64 ptr = u64(reinterpret_cast<uintptr_t>(mem.get()));
                wrBe32(cmds + pb.at, u32(ptr >> 32));
                wrBe32(cmds + pb.at + 4, u32(ptr));
                keep.push_back(std::move(mem));
            }
            continue;
        }
        if (j < 0) {
            const GxPatch& pb = b.patches[i];
            // SA2_INTERP_HIDE=1 (debugging): 3D position matrices with nothing to blend from are
            // drawn collapsed - what stays visible in an in-between frame is what blends.
            static const bool s_hide = std::getenv("SA2_INTERP_HIDE") != nullptr;
            if (s_hide && j == -1 && pb.kind == GxPatch::PosMtx && pb.count == 12 && !pb.flat) {
                std::memset(cmds + pb.at, 0, 48);
                continue;
            }
            if (j == -1 && haveC && (!pb.flat || (pb.world && !g_interpBaseline)) && pb.view == b.mainView) {
                if (pb.kind == GxPatch::PosMtx && pb.count == 12) {
                    float mb[12], mo[12];
                    u8* vo = cmds + pb.at;
                    for (int q = 0; q < 12; q++) mb[q] = rdF(b.cmds.data() + pb.at + 4 * q);
                    mulAffine(C, mb, mo);
                    for (int q = 0; q < 12; q++) wrF(vo + 4 * q, mo[q]);
                } else if (pb.kind == GxPatch::Vertex && pb.viewSpace && pb.fmt == 4 && pb.comps == 3) {
                    // positions the CPU put in the camera's space (stripe particles: wakes, waves)
                    moveVertices(b.cmds.data() + pb.at + pb.posOff, cmds + pb.at + pb.posOff, pb.count, pb.stride, C);
                }
            }
            continue;
        }
        const GxPatch& pb = b.patches[i];
        const GxPatch& pa = a.patches[size_t(j)];
        if (pb.world) {
            // A trail / fan of world positions: the points of one frame are the last frame's, shifted along as new ones were
            // added at one end - paired by where they are in the world (the shift that fits best), the new end growing out of
            // the old one's last point, so it follows what it trails behind.
            const bool fits = pa.fmt == 4 && pb.fmt == 4 && pa.comps == 3 && pb.comps == 3 && pa.viewSpace == pb.viewSpace;
            if (fits && pb.worldSpace && pa.worldSpace)  // (the camera's matrix goes with them, blended with the rest)
                blendWorldVertices(a.cmds.data() + pa.at + pa.posOff, pa.count, pa.stride, b.cmds.data() + pb.at + pb.posOff, pb.count, pb.stride,
                                   cmds + pb.at + pb.posOff, nullptr, nullptr, nullptr, t);
            else if (fits && haveV && pb.view == b.mainView && pa.view == a.mainView)
                blendWorldVertices(a.cmds.data() + pa.at + pa.posOff, pa.count, pa.stride, b.cmds.data() + pb.at + pb.posOff, pb.count, pb.stride,
                                   cmds + pb.at + pb.posOff, invA, invB, vt, t);
            else if (haveC && pb.viewSpace)
                moveVertices(b.cmds.data() + pb.at + pb.posOff, cmds + pb.at + pb.posOff, pb.count, pb.stride, C);
            continue;
        }
        const u8* va = a.cmds.data() + pa.at;
        const u8* vb = b.cmds.data() + pb.at;
        u8* vo = cmds + pb.at;
        switch (pb.kind) {
        case GxPatch::PosMtx:
        case GxPatch::NrmMtx: {
            const int cols = pb.kind == GxPatch::PosMtx ? 4 : 3;
            if (pb.count != u32(3 * cols)) break;
            float ma[12], mb[12], mo[12];
            for (int q = 0; q < 3 * cols; q++) ma[q] = rdF(va + 4 * q), mb[q] = rdF(vb + 4 * q);
            if (cols == 4 && haveV && !pb.flat && pb.view == b.mainView && pa.view == a.mainView) {
                float wa[12], wb[12], wt[12];  // the object in the world in each frame, and at t
                mulAffine(invA, ma, wa);
                mulAffine(invB, mb, wb);
                if (blendMatrix(wa, wb, 4, t, wt) != Blend) break;
                mulAffine(vt, wt, mo);
            } else if (blendMatrix(ma, mb, cols, t, mo) != Blend) {
                break;
            }
            for (int q = 0; q < 3 * cols; q++) wrF(vo + 4 * q, mo[q]);
            break;
        }
        case GxPatch::TexMtx: {
            // A scrolling texture wrapping around jumps by about its whole value (0.98 -> 0.02); a
            // projection (shadows, reflections: world units scaled) moves by a small share of its own.
            if (pb.count == 8 || pb.count == 12) {
                float fa[12], fb[12], fo[12];
                for (u32 q = 0; q < pb.count; q++) fa[q] = rdF(va + 4 * q), fb[q] = rdF(vb + 4 * q);
                if (blendTexSrt(fa, fb, pb.count, t, fo)) {
                    for (u32 q = 0; q < pb.count; q++) wrF(vo + 4 * q, fo[q]);
                    break;
                }
            }
            bool wrap = false;
            for (u32 q = 0; q < pb.count; q++) {
                const float x = rdF(va + 4 * q), y = rdF(vb + 4 * q);
                wrap = wrap || (std::fabs(y - x) > 0.5f && std::fabs(y - x) > 0.5f * std::max(std::fabs(x), std::fabs(y)));
            }
            if (wrap) break;
            for (u32 q = 0; q < pb.count; q++) {
                const float x = rdF(va + 4 * q), y = rdF(vb + 4 * q);
                wrF(vo + 4 * q, x + (y - x) * t);
            }
            break;
        }
        case GxPatch::Light: {
            for (u32 q = 0; q < pb.count; q++) {
                const u32 role = (pb.stride + q - 0x600) % 16;  // stride holds the XF address
                if (role < 10) continue;                         // colour and attenuation stay
                const float x = rdF(va + 4 * q), y = rdF(vb + 4 * q);
                if (std::fabs(y - x) > 0.5f * std::max(std::fabs(x), std::fabs(y)) + 50.f) continue;
                wrF(vo + 4 * q, x + (y - x) * t);
            }
            break;
        }
        case GxPatch::Proj: {
            if (pb.count < 7 || rdBe32(va + 24) != rdBe32(vb + 24)) break;  // perspective <-> orthographic
            bool far = false;
            for (int q = 0; q < 6; q++) {
                const float x = rdF(va + 4 * q), y = rdF(vb + 4 * q);
                far = far || std::fabs(y - x) > 0.5f * std::max(std::fabs(x), std::fabs(y)) + 1e-6f;
            }
            if (far) break;
            for (int q = 0; q < 6; q++) {
                const float x = rdF(va + 4 * q), y = rdF(vb + 4 * q);
                wrF(vo + 4 * q, x + (y - x) * t);
            }
            break;
        }
        case GxPatch::Vertex:
            blendPositions(va + pa.posOff, vb + pb.posOff, vo + pb.posOff, pb.count, pb.stride, pb.comps, pb.fmt, pb.shift, t);
            break;
        case GxPatch::Array: {
            const u32 n = std::min(pa.count, pb.count) / pb.stride;  // the extent both frames have
            std::unique_ptr<u8[]> mem(new u8[pb.count]);
            std::memcpy(mem.get(), pb.data, pb.count);
            if (blendArray(pa.data, pb.data, mem.get(), n, pb.stride, pb.comps, pb.fmt, pb.shift, t) != Blend) break;
            const u64 ptr = u64(reinterpret_cast<uintptr_t>(mem.get()));
            wrBe32(vo, u32(ptr >> 32));
            wrBe32(vo + 4, u32(ptr));
            keep.push_back(std::move(mem));
            break;
        }
        }
    }
    return sb.f;
}

// SA2_INTERP_JITTER=<file> (debugging, with SA2_INTERP_TEST/STEPS): per build, where each drawn thing is on screen, as
// "frame t key patches x y" (its 3D position matrices' and view-space vertices' mean, in the view's x/z, y/z). A thing that moves
// smoothly steps evenly from build to build; one that sits still for a game frame and then jumps is a jitter
// (tools/interpjitter.py lists them).
static void jitterLog(const GxFrame& b, const std::vector<u8>& out, float t) {
    static FILE* f = [] {
        const char* p = std::getenv("SA2_INTERP_JITTER");
        return p ? std::fopen(p, "a") : nullptr;
    }();
    if (!f || !b.jitter) return;
    struct Acc {
        double x = 0, y = 0;
        u32 n = 0, mtx = 0, vtx = 0, world = 0, grouped = 0, paired = 0, same = 0, unpaired = 0;
    };
    std::unordered_map<u64, Acc> acc;
    const u8* cmds = out.data() + b.preamble.size();
    const std::vector<int>* match = nullptr;  // how each patch paired with the frame before it
    for (const Shown& s : s_hist)
        if (s.f.get() == &b) match = &s.match;
    auto add = [&](u64 key, float x, float y, float z, const GxPatch& p) {
        if (z > -50.f) return;
        Acc& a = acc[key];
        if (match) {
            const int m = (*match)[size_t(&p - b.patches.data())];
            (m >= 0 ? a.paired : m == -2 ? a.same : a.unpaired)++;
        }
        a.x += x / -z, a.y += y / -z, a.n++;
        (p.kind == GxPatch::PosMtx ? a.mtx : a.vtx)++, a.world += p.world, a.grouped += p.group != 0;
    };
    for (const GxPatch& p : b.patches) {
        if ((p.flat && !p.world) || p.view != b.mainView) continue;
        const u64 key = p.group ? p.group : p.key;
        if (p.kind == GxPatch::PosMtx && p.count == 12) {
            add(key, rdF(cmds + p.at + 12), rdF(cmds + p.at + 28), rdF(cmds + p.at + 44), p);
        } else if (p.kind == GxPatch::Vertex && p.viewSpace && p.fmt == 4 && p.comps == 3 && p.count) {
            float x = 0, y = 0, z = 0;
            for (u32 v = 0; v < p.count; v++) {
                const u8* q = cmds + p.at + v * p.stride + p.posOff;
                x += rdF(q), y += rdF(q + 4), z += rdF(q + 8);
            }
            add(key ^ 0x5eed, x / p.count, y / p.count, z / p.count, p);
        }
    }
    for (const auto& [k, a] : acc)
        std::fprintf(f, "%d %llu %.3f %016llx %u %.5f %.5f m%uv%uw%ug%up%us%uu%u\n", b.jitter + (g_interpBaseline ? 1000 : 0), (unsigned long long)b.index, t,
                     (unsigned long long)k, a.n, a.x / a.n, a.y / a.n, a.mtx, a.vtx, a.world, a.grouped, a.paired, a.same, a.unpaired);
    std::fflush(f);
}

std::shared_ptr<GxFrame> gxInterpBuild(double when, std::vector<u8>& out, std::vector<std::unique_ptr<u8[]>>& keep, float* shownT) {
    float t = 1.f;
    auto r = buildImpl(when, out, keep, &t);
    if (shownT) *shownT = t;
    // SA2_INTERP_HIDEKEY=<hex group,...> (debugging): those groups' position matrices are collapsed - what disappears is what they drew.
    static const std::vector<u64> s_hideKeys = [] {
        std::vector<u64> v;
        for (const char* p = std::getenv("SA2_INTERP_HIDEKEY"); p && *p;) {
            char* e;
            v.push_back(std::strtoull(p, &e, 16));
            p = *e ? e + 1 : e;
        }
        return v;
    }();
    if (r && !s_hideKeys.empty())
        for (const GxPatch& p : r->patches)
            if (p.kind == GxPatch::PosMtx && p.count == 12 && std::find(s_hideKeys.begin(), s_hideKeys.end(), p.group ? p.group : p.key) != s_hideKeys.end())
                std::memset(out.data() + r->preamble.size() + p.at, 0, 48);
    if (r) jitterLog(*r, out, t);
    return r;
}

}  // namespace rt
