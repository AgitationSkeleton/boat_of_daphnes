// A software renderer for a player model's head: see head_render.h.
#include "head_render.h"

#include "gx_texture.h"

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace head_render {
namespace {

constexpr size_t kMaxTriangles = 400000;
constexpr float kPi = 3.14159265358979f;

// --- reading -------------------------------------------------------------------------------------------------------
// Big endian reads that never go outside the file: a read past the end gives 0 and marks the reader bad.
struct Reader {
    const uint8_t* p;
    size_t n;
    mutable bool bad = false;
    uint8_t u8(size_t o) const { return o < n ? p[o] : (bad = true, uint8_t(0)); }
    uint16_t u16(size_t o) const { return o + 2 <= n ? uint16_t(p[o] << 8 | p[o + 1]) : (bad = true, uint16_t(0)); }
    uint32_t u32(size_t o) const {
        return o + 4 <= n ? (uint32_t(p[o]) << 24 | uint32_t(p[o + 1]) << 16 | uint32_t(p[o + 2]) << 8 | p[o + 3]) : (bad = true, 0u);
    }
    int16_t s16(size_t o) const { return int16_t(u16(o)); }
    float f32(size_t o) const {
        const uint32_t v = u32(o);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
};

struct Chunk {
    size_t off = 0, size = 0;
    bool has() const { return size != 0; }
};

// The J3D file's chunks by magic.
std::map<std::string, Chunk> find_chunks(const Reader& r) {
    std::map<std::string, Chunk> out;
    const uint32_t count = r.u32(12);
    size_t off = 0x20;
    for (uint32_t i = 0; i < count && off + 8 <= r.n && i < 64; ++i) {
        const std::string magic(reinterpret_cast<const char*>(r.p + off), 4);
        const uint32_t size = r.u32(off + 4);
        if (size < 8 || off + size > r.n)
            break;
        out[magic] = Chunk{off, size};
        off += size;
    }
    return out;
}

std::vector<std::string> read_names(const Reader& r, size_t chunk, uint32_t off) {
    std::vector<std::string> names;
    if (off == 0)
        return names;
    const size_t table = chunk + off;
    const uint16_t count = r.u16(table);
    for (uint16_t i = 0; i < count && i < 4096; ++i) {
        const size_t s = table + r.u16(table + 4 + i * 4ull + 2);
        std::string name;
        for (size_t k = s; k < r.n && r.p[k] != 0 && name.size() < 64; ++k)
            name += char(r.p[k]);
        names.push_back(name);
    }
    return names;
}

// --- maths ---------------------------------------------------------------------------------------------------------
struct Mat {
    float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
};

Mat mul(const Mat& a, const Mat& b) {
    Mat o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            o.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0.f);
        }
    return o;
}

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 xf(const Mat& a, V3 p) {
    return {a.m[0][0] * p.x + a.m[0][1] * p.y + a.m[0][2] * p.z + a.m[0][3], a.m[1][0] * p.x + a.m[1][1] * p.y + a.m[1][2] * p.z + a.m[1][3],
            a.m[2][0] * p.x + a.m[2][1] * p.y + a.m[2][2] * p.z + a.m[2][3]};
}
V3 xf_dir(const Mat& a, V3 p) {
    return {a.m[0][0] * p.x + a.m[0][1] * p.y + a.m[0][2] * p.z, a.m[1][0] * p.x + a.m[1][1] * p.y + a.m[1][2] * p.z,
            a.m[2][0] * p.x + a.m[2][1] * p.y + a.m[2][2] * p.z};
}
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-9f ? a * (1.f / l) : V3{0, 1, 0};
}

// --- the model -------------------------------------------------------------------------------------------------------
struct Attr {
    int comps = 0;
    std::vector<float> v;
    size_t count() const { return comps ? v.size() / size_t(comps) : 0; }
};

struct Joint {
    float scale[3] = {1, 1, 1};
    float rot[3] = {0, 0, 0};
    float trans[3] = {0, 0, 0};
    int parent = -1;
};

struct Shape {
    std::vector<std::pair<uint32_t, uint32_t>> desc; // (attribute, type)
    struct Group {
        std::vector<uint16_t> table;
        std::vector<uint8_t> dl;
    };
    std::vector<Group> groups;
};

struct SceneNode {
    int joint, material, shape;
};

struct Vtx {
    V3 p, n;
    float u = 0, v = 0;
    float c[4] = {1, 1, 1, 1};
};

struct Tri {
    Vtx v[3];
    int material;
    bool decal; // not part of the body's own skin: a feature on it (eyes, brows, nose)
};

struct Texture {
    std::string key, name;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    int wrap_s = 1, wrap_t = 1; // 0 clamp, 1 repeat, 2 mirror
};

struct Material {
    std::string name;
    int tex0 = -1, tex1 = -1;
    float tex1_shift = 0.f; // the second texture matrix's S translation
    uint32_t color = 0xFFFFFFFFu;
    Texture composed;       // eyes: the shape times the iris
    bool use_composed = false;
};

struct Model {
    std::vector<Joint> joints;
    std::vector<std::string> joint_names;
    std::vector<SceneNode> scene;
    Attr pos, nrm, uv;
    std::vector<std::array<float, 4>> clr;
    std::vector<std::pair<uint8_t, uint16_t>> drw;
    std::vector<std::pair<std::vector<uint16_t>, std::vector<float>>> evp;
    std::vector<Mat> inv;
    std::vector<Shape> shapes;
    std::vector<Texture> textures;
    std::vector<Material> materials;
};

Mat joint_local(const Joint& j) {
    const float cx = std::cos(j.rot[0]), sx = std::sin(j.rot[0]);
    const float cy = std::cos(j.rot[1]), sy = std::sin(j.rot[1]);
    const float cz = std::cos(j.rot[2]), sz = std::sin(j.rot[2]);
    // Rz * Ry * Rx
    const float r[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                           {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                           {-sy, cy * sx, cy * cx}};
    Mat m;
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k)
            m.m[i][k] = r[i][k] * j.scale[k];
        m.m[i][3] = j.trans[i];
    }
    return m;
}

bool parse_vertices(const Reader& r, const Chunk& c, Model& m) {
    const uint32_t fmt_off = r.u32(c.off + 8);
    uint32_t arr[13];
    for (int i = 0; i < 13; ++i)
        arr[i] = r.u32(c.off + 0x0C + 4ull * i);
    struct Fmt {
        uint32_t cnt, type;
        uint8_t frac;
    };
    std::map<uint32_t, Fmt> fmts;
    for (size_t o = fmt_off; fmts.size() < 32; o += 16) {
        const uint32_t attr = r.u32(c.off + o);
        if (attr == 0xFF || r.bad)
            break;
        fmts[attr] = {r.u32(c.off + o + 4), r.u32(c.off + o + 8), r.u8(c.off + o + 12)};
    }
    std::vector<uint32_t> bounds;
    for (uint32_t o : arr)
        if (o != 0)
            bounds.push_back(o);
    bounds.push_back(uint32_t(c.size));
    std::sort(bounds.begin(), bounds.end());
    auto end_of = [&](uint32_t start) {
        for (uint32_t b : bounds)
            if (b > start)
                return b;
        return uint32_t(c.size);
    };
    auto load = [&](uint32_t attr, int index, Attr& out) {
        const auto f = fmts.find(attr);
        if (f == fmts.end() || arr[index] == 0)
            return;
        const uint32_t start = arr[index], end = std::min<uint32_t>(end_of(start), uint32_t(c.size));
        const int comps = attr == 9 ? 2 + int(f->second.cnt) : attr == 10 ? 3 : 1 + int(f->second.cnt);
        const uint32_t type = f->second.type;
        if (type > 4 || comps < 1 || comps > 4)
            return;
        const int bs = type <= 1 ? 1 : type <= 3 ? 2 : 4;
        const size_t n = (end - start) / size_t(comps * bs);
        out.comps = comps;
        out.v.resize(n * size_t(comps));
        const float scale = type == 4 ? 1.f : 1.f / float(1u << std::min<uint8_t>(f->second.frac, 30));
        for (size_t i = 0; i < out.v.size(); ++i) {
            const size_t o = c.off + start + i * size_t(bs);
            float x = 0;
            switch (type) {
            case 0: x = r.u8(o); break;
            case 1: x = float(int8_t(r.u8(o))); break;
            case 2: x = r.u16(o); break;
            case 3: x = r.s16(o); break;
            default: x = r.f32(o); break;
            }
            out.v[i] = x * scale;
        }
    };
    load(9, 0, m.pos);
    load(10, 1, m.nrm);
    load(13, 5, m.uv);
    // colour 0
    if (const auto f = fmts.find(11); f != fmts.end() && arr[3] != 0) {
        static const int stride[6] = {2, 3, 4, 2, 3, 4};
        const uint32_t type = f->second.type;
        if (type <= 5) {
            const uint32_t start = arr[3], end = std::min<uint32_t>(end_of(start), uint32_t(c.size));
            const size_t n = (end - start) / size_t(stride[type]);
            for (size_t i = 0; i < n; ++i) {
                const size_t o = c.off + start + i * size_t(stride[type]);
                if (type == 5)
                    m.clr.push_back({r.u8(o) / 255.f, r.u8(o + 1) / 255.f, r.u8(o + 2) / 255.f, r.u8(o + 3) / 255.f});
                else if (type == 1 || type == 2)
                    m.clr.push_back({r.u8(o) / 255.f, r.u8(o + 1) / 255.f, r.u8(o + 2) / 255.f, 1.f});
                else
                    m.clr.push_back({1, 1, 1, 1});
            }
        }
    }
    return m.pos.count() > 0 && !r.bad;
}

bool parse_skeleton(const Reader& r, const std::map<std::string, Chunk>& ch, Model& m) {
    const auto jnt = ch.find("JNT1");
    const auto inf = ch.find("INF1");
    if (jnt == ch.end() || inf == ch.end())
        return false;
    {
        const Chunk& c = jnt->second;
        const uint16_t n = r.u16(c.off + 8);
        const uint32_t jo = r.u32(c.off + 0x0C), no = r.u32(c.off + 0x14);
        if (n == 0 || n > 512)
            return false;
        m.joints.resize(n);
        m.joint_names = read_names(r, c.off, no);
        for (uint16_t i = 0; i < n; ++i) {
            const size_t o = c.off + jo + 0x40ull * i;
            Joint& j = m.joints[i];
            for (int k = 0; k < 3; ++k) {
                j.scale[k] = r.f32(o + 4 + 4 * k);
                j.rot[k] = float(r.s16(o + 0x10 + 2 * k)) * kPi / 32768.f;
                j.trans[k] = r.f32(o + 0x18 + 4 * k);
            }
        }
    }
    {
        const Chunk& c = inf->second;
        size_t o = c.off + r.u32(c.off + 0x14);
        std::vector<int> stack{-1};
        int pending = -1, last_mat = -1;
        for (int guard = 0; guard < 20000 && !r.bad; ++guard) {
            const uint16_t t = r.u16(o), i = r.u16(o + 2);
            o += 4;
            if (t == 0)
                break;
            if (t == 1)
                stack.push_back(pending);
            else if (t == 2) {
                if (stack.size() > 1)
                    stack.pop_back();
            } else if (t == 0x10) {
                if (i >= m.joints.size())
                    return false;
                m.joints[i].parent = stack.back();
                pending = i;
            } else if (t == 0x11)
                last_mat = i;
            else if (t == 0x12)
                m.scene.push_back({pending, last_mat, i});
        }
    }
    return !r.bad && !m.scene.empty();
}

void parse_skin(const Reader& r, const std::map<std::string, Chunk>& ch, Model& m) {
    if (const auto it = ch.find("DRW1"); it != ch.end()) {
        const Chunk& c = it->second;
        const uint16_t n = r.u16(c.off + 8);
        const uint32_t wo = r.u32(c.off + 0x0C), io = r.u32(c.off + 0x10);
        for (uint16_t i = 0; i < n; ++i)
            m.drw.push_back({r.u8(c.off + wo + i), r.u16(c.off + io + 2ull * i)});
    }
    if (const auto it = ch.find("EVP1"); it != ch.end()) {
        const Chunk& c = it->second;
        const uint16_t n = r.u16(c.off + 8);
        if (n != 0) {
            const uint32_t co = r.u32(c.off + 0x0C), io = r.u32(c.off + 0x10), wo = r.u32(c.off + 0x14), mo = r.u32(c.off + 0x18);
            size_t ip = 0;
            for (uint16_t i = 0; i < n; ++i) {
                const uint8_t k = r.u8(c.off + co + i);
                std::vector<uint16_t> js;
                std::vector<float> ws;
                for (uint8_t j = 0; j < k; ++j) {
                    js.push_back(r.u16(c.off + io + 2 * (ip + j)));
                    ws.push_back(r.f32(c.off + wo + 4 * (ip + j)));
                }
                ip += k;
                m.evp.push_back({js, ws});
            }
            const size_t nm = c.size > mo ? (c.size - mo) / 48 : 0;
            for (size_t i = 0; i < nm && i < 512; ++i) {
                Mat inv;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 4; ++b)
                        inv.m[a][b] = r.f32(c.off + mo + 48 * i + 4 * (a * 4 + b));
                m.inv.push_back(inv);
            }
        }
    }
}

bool parse_shapes(const Reader& r, const std::map<std::string, Chunk>& ch, Model& m) {
    const auto it = ch.find("SHP1");
    if (it == ch.end())
        return false;
    const Chunk& c = it->second;
    const uint16_t n = r.u16(c.off + 8);
    const uint32_t so = r.u32(c.off + 0x0C), ao = r.u32(c.off + 0x18), mto = r.u32(c.off + 0x1C), po = r.u32(c.off + 0x20),
                   mdo = r.u32(c.off + 0x24), pko = r.u32(c.off + 0x28);
    if (n > 512)
        return false;
    for (uint16_t i = 0; i < n; ++i) {
        const size_t s = c.off + so + 0x28ull * i;
        Shape sh;
        const uint16_t ngroups = r.u16(s + 2), aoff = r.u16(s + 4), fmi = r.u16(s + 6), fpi = r.u16(s + 8);
        size_t a = c.off + ao + aoff;
        for (int guard = 0; guard < 32; ++guard) {
            const uint32_t at = r.u32(a), ty = r.u32(a + 4);
            a += 8;
            if (at == 0xFF || r.bad)
                break;
            sh.desc.push_back({at, ty});
        }
        for (uint16_t g = 0; g < ngroups && g < 256; ++g) {
            Shape::Group grp;
            const uint16_t cnt = r.u16(c.off + mdo + 8ull * (fmi + g) + 2);
            const uint32_t first = r.u32(c.off + mdo + 8ull * (fmi + g) + 4);
            for (uint16_t k = 0; k < cnt; ++k)
                grp.table.push_back(r.u16(c.off + mto + 2ull * (first + k)));
            const uint32_t size = r.u32(c.off + pko + 8ull * (fpi + g)), poff = r.u32(c.off + pko + 8ull * (fpi + g) + 4);
            const size_t start = c.off + po + poff;
            if (r.bad || start + size > c.off + c.size)
                return false;
            grp.dl.assign(r.p + start, r.p + start + size);
            sh.groups.push_back(std::move(grp));
        }
        m.shapes.push_back(std::move(sh));
    }
    return !r.bad;
}

// --- textures and materials ---------------------------------------------------------------------------------------
Image resized(const Image& in, uint32_t w, uint32_t h) {
    Image out;
    out.width = w;
    out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            std::memcpy(&out.rgba[(size_t(y) * w + x) * 4], &in.rgba[(size_t(y * in.height / h) * in.width + x * in.width / w) * 4], 4);
    return out;
}

bool parse_textures(const Reader& r, const std::map<std::string, Chunk>& ch, const Replacements& rep, Model& m) {
    const auto it = ch.find("TEX1");
    if (it == ch.end())
        return false;
    const Chunk& c = it->second;
    const uint16_t n = r.u16(c.off + 8);
    const uint32_t ho = r.u32(c.off + 0x0C), no = r.u32(c.off + 0x10);
    const std::vector<std::string> names = read_names(r, c.off, no);
    if (n > 512)
        return false;
    for (uint16_t i = 0; i < n; ++i) {
        Texture t;
        t.name = i < names.size() ? names[i] : "";
        gx_texture::Texture g;
        std::string error;
        const size_t at = c.off + ho + 0x20ull * i;
        t.wrap_s = r.u8(at + 6);
        t.wrap_t = r.u8(at + 7);
        if (t.wrap_s > 2) t.wrap_s = 1;
        if (t.wrap_t > 2) t.wrap_t = 1;
        if (gx_texture::Parse(r.p, r.n, at, g, &error) && gx_texture::DecodeRgba(g, t.rgba, &error)) {
            t.w = g.width;
            t.h = g.height;
            t.key = gx_texture::Key(g, &error);
            if (const auto f = rep.find(t.key); f != rep.end() && f->second.width > 0 && f->second.height > 0 &&
                                                 f->second.rgba.size() == size_t(f->second.width) * f->second.height * 4) {
                t.w = f->second.width;
                t.h = f->second.height;
                t.rgba = f->second.rgba;
            }
            if (std::getenv("BLUEWAKE_PORTRAIT_LOG") != nullptr) // testing
                std::fprintf(stderr, "[portrait] texture %s %s: %s\n", t.name.c_str(), t.key.c_str(), rep.count(t.key) ? "replaced" : "kept");
        }
        m.textures.push_back(std::move(t));
    }
    return true;
}

void parse_materials(const Reader& r, const std::map<std::string, Chunk>& ch, Model& m) {
    const auto it = ch.find("MAT3");
    if (it == ch.end())
        return;
    const Chunk& c = it->second;
    const uint16_t n = r.u16(c.off + 8);
    const uint32_t init = r.u32(c.off + 0x0C), ind = r.u32(c.off + 0x10), nam = r.u32(c.off + 0x14), colors = r.u32(c.off + 0x20),
                   texmtx = r.u32(c.off + 0x40), texno = r.u32(c.off + 0x48);
    const std::vector<std::string> names = read_names(r, c.off, nam);
    for (uint16_t i = 0; i < n && i < 512; ++i) {
        Material mat;
        mat.name = i < names.size() ? names[i] : "";
        const size_t e = c.off + init + 0x14Cull * r.u16(c.off + ind + 2ull * i);
        auto texture = [&](int slot) {
            const uint16_t idx = r.u16(e + 0x84 + 2ull * slot);
            if (idx == 0xFFFF)
                return -1;
            const uint16_t tex = r.u16(c.off + texno + 2ull * idx);
            return tex < m.textures.size() ? int(tex) : -1;
        };
        mat.tex0 = texture(0);
        mat.tex1 = texture(1);
        if (const uint16_t cidx = r.u16(e + 8); cidx != 0xFFFF && colors != 0)
            mat.color = r.u32(c.off + colors + 4ull * cidx);
        if (const uint16_t tm = r.u16(e + 0x48 + 2); tm != 0xFFFF && texmtx != 0)
            mat.tex1_shift = r.f32(c.off + texmtx + 0x64ull * tm + 0x1C);
        // The game's eye: the eye shape (IA8) times the iris page, the iris shifted by the second texture matrix.
        if (mat.name.rfind("eye", 0) == 0 && mat.tex0 >= 0 && mat.tex1 >= 0) {
            const Texture& shape = m.textures[size_t(mat.tex0)];
            const Texture& iris = m.textures[size_t(mat.tex1)];
            if (!shape.rgba.empty() && !iris.rgba.empty()) {
                Image si{shape.w, shape.h, shape.rgba}, ii{iris.w, iris.h, iris.rgba};
                if (ii.width != si.width || ii.height != si.height)
                    ii = resized(ii, si.width, si.height);
                Texture t = shape;
                const int shift = int(std::lround(-mat.tex1_shift * float(si.width)));
                for (uint32_t y = 0; y < si.height; ++y)
                    for (uint32_t x = 0; x < si.width; ++x) {
                        const uint32_t sx = uint32_t(((int(x) - shift) % int(si.width) + int(si.width)) % int(si.width));
                        const uint8_t* a = &si.rgba[(size_t(y) * si.width + x) * 4];
                        const uint8_t* b = &ii.rgba[(size_t(y) * si.width + sx) * 4];
                        uint8_t* o = &t.rgba[(size_t(y) * si.width + x) * 4];
                        for (int k = 0; k < 3; ++k)
                            o[k] = uint8_t(a[k] * b[k] / 255);
                        o[3] = a[3];
                    }
                mat.composed = std::move(t);
                mat.use_composed = true;
            }
        }
        m.materials.push_back(std::move(mat));
    }
}

// --- geometry --------------------------------------------------------------------------------------------------------
struct Prim {
    uint8_t cmd;
    std::vector<std::vector<uint32_t>> verts; // per vertex, per descriptor entry
};

// The display list's primitives; vertices carry the raw value of each attribute of the shape's descriptor.
bool parse_display_list(const Shape& sh, const Shape::Group& g, std::vector<Prim>& prims) {
    size_t o = 0;
    const std::vector<uint8_t>& dl = g.dl;
    while (o < dl.size()) {
        const uint8_t cmd = dl[o];
        if (cmd == 0) {
            ++o;
            continue;
        }
        if (cmd != 0x80 && cmd != 0x90 && cmd != 0x98 && cmd != 0xA0 && cmd != 0xA8 && cmd != 0xB0)
            break;
        if (o + 3 > dl.size())
            return false;
        const uint16_t cnt = uint16_t(dl[o + 1] << 8 | dl[o + 2]);
        o += 3;
        Prim p;
        p.cmd = cmd;
        for (uint16_t i = 0; i < cnt; ++i) {
            std::vector<uint32_t> v(sh.desc.size(), 0);
            for (size_t k = 0; k < sh.desc.size(); ++k) {
                const uint32_t ty = sh.desc[k].second;
                if (ty == 0)
                    continue;
                if (ty == 1 || ty == 2) {
                    if (o + 1 > dl.size()) return false;
                    v[k] = dl[o];
                    o += 1;
                } else {
                    if (o + 2 > dl.size()) return false;
                    v[k] = uint32_t(dl[o] << 8 | dl[o + 1]);
                    o += 2;
                }
            }
            p.verts.push_back(std::move(v));
        }
        prims.push_back(std::move(p));
    }
    return true;
}

template <class F>
void each_triangle(const Prim& p, F&& f) {
    const size_t n = p.verts.size();
    if (p.cmd == 0x90) {
        for (size_t i = 0; i + 2 < n; i += 3)
            f(i, i + 1, i + 2);
    } else if (p.cmd == 0x98) {
        for (size_t i = 0; i + 2 < n; ++i) {
            if (i % 2 == 0)
                f(i, i + 1, i + 2);
            else
                f(i + 1, i, i + 2);
        }
    } else if (p.cmd == 0xA0) {
        for (size_t i = 1; i + 1 < n; ++i)
            f(size_t(0), i, i + 1);
    } else if (p.cmd == 0x80) {
        for (size_t i = 0; i + 3 < n; i += 4) {
            f(i, i + 1, i + 2);
            f(i, i + 2, i + 3);
        }
    }
}

bool build_triangles(const Model& m, const std::vector<Mat>& world, std::vector<Tri>& out) {
    for (const SceneNode& node : m.scene) {
        if (node.shape < 0 || size_t(node.shape) >= m.shapes.size() || node.joint < 0 || size_t(node.joint) >= world.size() ||
            node.material < 0 || size_t(node.material) >= m.materials.size())
            continue;
        const Shape& sh = m.shapes[size_t(node.shape)];
        if (m.materials[size_t(node.material)].name.find("dam") != std::string::npos)
            continue; // the damage flash
        const bool decal = node.joint != 0 && m.joint_names.size() > size_t(node.joint) &&
                           (m.joint_names[size_t(node.joint)].rfind("cl_eye", 0) == 0 || m.joint_names[size_t(node.joint)].rfind("cl_mayu", 0) == 0 ||
                            m.joint_names[size_t(node.joint)].rfind("cl_hana", 0) == 0);
        std::map<size_t, uint16_t> slots;
        for (const Shape::Group& g : sh.groups) {
            for (size_t k = 0; k < g.table.size(); ++k)
                if (g.table[k] != 0xFFFF)
                    slots[k] = g.table[k];
            std::vector<Prim> prims;
            if (!parse_display_list(sh, g, prims))
                return false;
            auto desc_index = [&](uint32_t attr) {
                for (size_t k = 0; k < sh.desc.size(); ++k)
                    if (sh.desc[k].first == attr)
                        return int(k);
                return -1;
            };
            const int d_mtx = desc_index(0), d_pos = desc_index(9), d_nrm = desc_index(10), d_clr = desc_index(11), d_uv = desc_index(13);
            if (d_pos < 0)
                continue;
            for (const Prim& p : prims) {
                each_triangle(p, [&](size_t a, size_t b, size_t c) {
                    if (out.size() >= kMaxTriangles)
                        return;
                    Tri t;
                    t.material = node.material;
                    t.decal = decal;
                    const size_t ids[3] = {a, b, c};
                    for (int q = 0; q < 3; ++q) {
                        const std::vector<uint32_t>& v = p.verts[ids[q]];
                        Vtx& o = t.v[q];
                        const size_t pi = v[size_t(d_pos)];
                        if (pi >= m.pos.count())
                            return;
                        V3 pos{m.pos.v[pi * size_t(m.pos.comps)], m.pos.v[pi * size_t(m.pos.comps) + 1],
                               m.pos.comps > 2 ? m.pos.v[pi * size_t(m.pos.comps) + 2] : 0.f};
                        Mat M = world[size_t(node.joint)];
                        bool skinned = false;
                        V3 skin_pos;
                        if (d_mtx >= 0 && !slots.empty()) {
                            const auto s = slots.find(v[size_t(d_mtx)] / 3);
                            if (s != slots.end() && s->second < m.drw.size()) {
                                const auto [weighted, idx] = m.drw[s->second];
                                if (weighted) {
                                    if (idx < m.evp.size()) {
                                        for (size_t j = 0; j < m.evp[idx].first.size(); ++j) {
                                            const uint16_t jj = m.evp[idx].first[j];
                                            if (jj >= world.size() || jj >= m.inv.size())
                                                continue;
                                            const V3 w = xf(mul(world[jj], m.inv[jj]), pos);
                                            skin_pos = skin_pos + w * m.evp[idx].second[j];
                                        }
                                        skinned = true;
                                    }
                                } else if (idx < world.size())
                                    M = world[idx];
                            }
                        }
                        o.p = skinned ? skin_pos : xf(M, pos);
                        o.n = {0, 1, 0};
                        if (d_nrm >= 0 && m.nrm.count() > v[size_t(d_nrm)]) {
                            const size_t ni = v[size_t(d_nrm)] * 3;
                            const V3 n{m.nrm.v[ni], m.nrm.v[ni + 1], m.nrm.v[ni + 2]};
                            o.n = skinned ? n : xf_dir(M, n);
                        }
                        if (d_uv >= 0 && m.uv.count() > v[size_t(d_uv)]) {
                            o.u = m.uv.v[v[size_t(d_uv)] * size_t(m.uv.comps)];
                            o.v = m.uv.comps > 1 ? m.uv.v[v[size_t(d_uv)] * size_t(m.uv.comps) + 1] : 0.f;
                        }
                        if (d_clr >= 0 && m.clr.size() > v[size_t(d_clr)])
                            std::memcpy(o.c, m.clr[v[size_t(d_clr)]].data(), sizeof o.c);
                    }
                    out.push_back(t);
                });
            }
        }
    }
    return true;
}

// --- drawing ---------------------------------------------------------------------------------------------------------
float wrap(float u, int mode) {
    if (mode == 0)
        return std::clamp(u, 0.f, 1.f);
    if (mode == 2) {
        const float f = std::fmod(std::fabs(u), 2.f);
        return f > 1.f ? 2.f - f : f;
    }
    return u - std::floor(u);
}

void sample(const Texture& t, float u, float v, float out[4]) {
    if (t.rgba.empty() || t.w == 0 || t.h == 0) {
        out[0] = out[1] = out[2] = out[3] = 1.f;
        return;
    }
    const float x = wrap(u, t.wrap_s) * float(t.w) - 0.5f, y = wrap(v, t.wrap_t) * float(t.h) - 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    auto px = [&](int xx, int yy, int k) {
        xx = t.wrap_s == 1 ? ((xx % int(t.w)) + int(t.w)) % int(t.w) : std::clamp(xx, 0, int(t.w) - 1);
        yy = t.wrap_t == 1 ? ((yy % int(t.h)) + int(t.h)) % int(t.h) : std::clamp(yy, 0, int(t.h) - 1);
        return float(t.rgba[(size_t(yy) * t.w + size_t(xx)) * 4 + size_t(k)]) / 255.f;
    };
    for (int k = 0; k < 4; ++k)
        out[k] = px(x0, y0, k) * (1 - fx) * (1 - fy) + px(x0 + 1, y0, k) * fx * (1 - fy) + px(x0, y0 + 1, k) * (1 - fx) * fy +
                 px(x0 + 1, y0 + 1, k) * fx * fy;
}

struct Buffer {
    int w, h;
    std::vector<float> rgb, depth;
    std::vector<uint8_t> cover;
    Buffer(int w_, int h_) : w(w_), h(h_), rgb(size_t(w_) * h_ * 3, 0.f), depth(size_t(w_) * h_, 1e30f), cover(size_t(w_) * h_, 0) {}
};

struct Camera {
    V3 centre, right, up, back; // the head's place and the view's axes (back points to the viewer)
    float scale;                // pixels per unit
    float width;                // the picture's side in pixels
};

void draw(const Model& m, const std::vector<Tri>& tris, const Camera& cam, Buffer& buf, bool decals) {
    const V3 light = norm(cam.back * 0.55f + cam.up * 0.55f + cam.right * -0.35f);
    for (const Tri& t : tris) {
        if (t.decal != decals)
            continue;
        const Material& mat = m.materials[size_t(t.material)];
        const Texture* tex = mat.use_composed ? &mat.composed : mat.tex0 >= 0 ? &m.textures[size_t(mat.tex0)] : nullptr;
        float sx[3], sy[3], sz[3];
        for (int i = 0; i < 3; ++i) {
            const V3 d = t.v[i].p - cam.centre;
            sx[i] = dot(d, cam.right) * cam.scale + cam.width * 0.5f;
            sy[i] = cam.width * 0.5f - dot(d, cam.up) * cam.scale;
            sz[i] = -dot(d, cam.back) - (t.decal ? 2.5f : 0.f); // nearer is smaller; a feature is lifted off the skin
        }
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (std::fabs(area) < 1e-6f)
            continue;
        const int x0 = std::max(0, int(std::floor(std::min({sx[0], sx[1], sx[2]})))), x1 = std::min(buf.w - 1, int(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
        const int y0 = std::max(0, int(std::floor(std::min({sy[0], sy[1], sy[2]})))), y1 = std::min(buf.h - 1, int(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                const float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                const float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                const float w2 = 1.f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                const float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                const size_t at = size_t(y) * size_t(buf.w) + size_t(x);
                if (z >= buf.depth[at])
                    continue;
                float c[4] = {1, 1, 1, 1};
                if (tex != nullptr) {
                    sample(*tex, w0 * t.v[0].u + w1 * t.v[1].u + w2 * t.v[2].u, w0 * t.v[0].v + w1 * t.v[1].v + w2 * t.v[2].v, c);
                    if (c[3] < 0.5f)
                        continue;
                } else {
                    c[0] = float((mat.color >> 24) & 255) / 255.f;
                    c[1] = float((mat.color >> 16) & 255) / 255.f;
                    c[2] = float((mat.color >> 8) & 255) / 255.f;
                }
                const V3 n = norm(t.v[0].n * w0 + t.v[1].n * w1 + t.v[2].n * w2);
                const float lit = 0.66f + 0.34f * std::max(0.f, std::fabs(dot(n, light)));
                for (int k = 0; k < 3; ++k) {
                    const float vc = w0 * t.v[0].c[k] + w1 * t.v[1].c[k] + w2 * t.v[2].c[k];
                    buf.rgb[at * 3 + size_t(k)] = std::min(1.f, c[k] * vc * lit);
                }
                buf.depth[at] = z;
                buf.cover[at] = 1;
            }
    }
}

} // namespace

bool Render(const std::vector<uint8_t>& bdl, const Replacements& replace, int size, std::vector<uint8_t>& rgba, std::string* error) {
    auto fail = [&](const char* why) {
        if (error != nullptr)
            *error = why;
        return false;
    };
    if (size < 8 || size > 512 || bdl.size() < 0x40 || std::memcmp(bdl.data(), "J3D2bdl", 7) != 0)
        return fail("not a J3D model");
    const Reader r{bdl.data(), bdl.size()};
    const auto ch = find_chunks(r);
    Model m;
    if (ch.find("VTX1") == ch.end() || !parse_vertices(r, ch.at("VTX1"), m))
        return fail("no vertices");
    if (!parse_skeleton(r, ch, m))
        return fail("no skeleton");
    parse_skin(r, ch, m);
    if (!parse_shapes(r, ch, m))
        return fail("no shapes");
    if (!parse_textures(r, ch, replace, m))
        return fail("no textures");
    parse_materials(r, ch, m);
    if (r.bad)
        return fail("the model is cut short");

    std::vector<Mat> world(m.joints.size());
    std::vector<bool> done(m.joints.size(), false);
    for (size_t pass = 0; pass < m.joints.size(); ++pass)
        for (size_t i = 0; i < m.joints.size(); ++i) {
            const int p = m.joints[i].parent;
            if (done[i] || (p >= 0 && !done[size_t(p)]))
                continue;
            world[i] = p < 0 ? joint_local(m.joints[i]) : mul(world[size_t(p)], joint_local(m.joints[i]));
            done[i] = true;
        }
    int head = -1;
    for (size_t i = 0; i < m.joint_names.size(); ++i)
        if (m.joint_names[i] == "head_jnt")
            head = int(i);
    if (head < 0)
        return fail("no head joint");

    std::vector<Tri> tris;
    if (!build_triangles(m, world, tris) || tris.empty())
        return fail("no triangles");

    // The head joint's frame: +X up, +Y to the front, Z to the side (the same as tools/rando/bunny/linkww.py).
    const Mat& H = world[size_t(head)];
    Camera cam;
    const V3 origin{H.m[0][3], H.m[1][3], H.m[2][3]};
    const V3 up = norm({H.m[0][0], H.m[1][0], H.m[2][0]});
    const V3 front = norm({H.m[0][1], H.m[1][1], H.m[2][1]});
    cam.up = up;
    cam.back = front;
    cam.right = norm(cross(up, front));
    cam.centre = origin + up * 17.f + front * 0.f;
    constexpr int kSuper = 4;
    const float span = 62.f; // the units the picture shows across
    cam.width = float(size * kSuper);
    cam.scale = cam.width / span;
    Buffer buf(size * kSuper, size * kSuper);
    draw(m, tris, cam, buf, false);
    draw(m, tris, cam, buf, true);

    rgba.assign(size_t(size) * size_t(size) * 4, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float acc[3] = {0, 0, 0};
            int covered = 0;
            for (int sy = 0; sy < kSuper; ++sy)
                for (int sx = 0; sx < kSuper; ++sx) {
                    const size_t at = size_t(y * kSuper + sy) * size_t(buf.w) + size_t(x * kSuper + sx);
                    if (!buf.cover[at])
                        continue;
                    ++covered;
                    for (int k = 0; k < 3; ++k)
                        acc[k] += buf.rgb[at * 3 + size_t(k)];
                }
            uint8_t* o = &rgba[(size_t(y) * size_t(size) + size_t(x)) * 4];
            if (covered > 0) {
                for (int k = 0; k < 3; ++k)
                    o[k] = uint8_t(std::clamp(acc[k] / float(covered), 0.f, 1.f) * 255.f + 0.5f);
                o[3] = uint8_t(covered * 255 / (kSuper * kSuper));
            }
        }
    return true;
}

namespace {
uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ d[i]) & 255] ^ (crc >> 8);
    return ~crc;
}
void put32(std::vector<uint8_t>& o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        o.push_back(uint8_t(v >> s));
}
void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
    put32(o, uint32_t(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    o.insert(o.end(), body.begin(), body.end());
    put32(o, crc32(body.data(), body.size()));
}
} // namespace

void EncodePng(const uint8_t* rgba, uint32_t width, uint32_t height, std::vector<uint8_t>& out) {
    out.assign({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    std::vector<uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(out, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * width * 4, rgba + size_t(y + 1) * width * 4);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n & 255));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n & 255));
        z.push_back(uint8_t((~n >> 8) & 255));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        pos += n;
        if (last)
            break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
}

} // namespace head_render
