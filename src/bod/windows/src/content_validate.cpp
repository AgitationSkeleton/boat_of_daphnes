// See content_validate.h and docs/fork/anchor-content-security.md.
#include "content_validate.h"

#include "player_parts.h"
#include "png_decode.h"
#include "rarc.h"

#define PICOJSON_USE_INT64
#include "picojson.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace content_validate {
namespace {

uint32_t be32(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 |
           static_cast<uint32_t>(b[at + 2]) << 8 | b[at + 3];
}
uint16_t be16(const std::vector<uint8_t>& b, size_t at) { return static_cast<uint16_t>(b[at] << 8 | b[at + 1]); }

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr)
        *error = message;
    return false;
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Names that may be shown in the log: no control characters or markup, short.
std::string clean_name(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (out.size() >= 60)
            break;
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')')
            out += c;
    }
    return out;
}

} // namespace

bool IsHex64(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool SafeRelPath(const std::string& path) {
    if (path.empty() || path.size() > 160 || path[0] == '/')
        return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    for (const char c : path) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

bool AllowedPath(const std::string& path, const std::string& role) {
    if (role == "model")
        return path == "Link.arc";
    if (role == "equipment")
        return path == "equipment/Link.arc";
    if (role == "voice")
        return path == "sound/JaiInit.aaf" || path == "sound/voice_0.aw" || path == "sound/GanonT_0.aw";
    return false;
}

bool ParseManifest(const std::string& text, Manifest& out, std::string* error) {
    out = Manifest{};
    if (text.empty() || text.size() > kMaxManifest)
        return fail(error, "the manifest is empty or over the size limit");
    picojson::value v;
    if (!picojson::parse(v, text).empty() || !v.is<picojson::object>())
        return fail(error, "the manifest is not a JSON object");
    if (!v.contains("v") || !v.get("v").is<int64_t>() || v.get("v").get<int64_t>() != 1)
        return fail(error, "the manifest is not version 1");
    if (!v.contains("files") || !v.get("files").is<picojson::array>())
        return fail(error, "the manifest has no file list");
    if (v.contains("name") && v.get("name").is<std::string>())
        out.name = clean_name(v.get("name").get<std::string>());
    const auto& list = v.get("files").get<picojson::array>();
    if (list.empty() || list.size() > kMaxManifestFiles)
        return fail(error, "the manifest lists too many or no files");
    std::set<std::string> seen;
    size_t total = 0;
    for (const auto& e : list) {
        if (!e.is<picojson::object>() || !e.contains("path") || !e.get("path").is<std::string>() || !e.contains("sha256") ||
            !e.get("sha256").is<std::string>() || !e.contains("size") || !e.get("size").is<int64_t>())
            return fail(error, "a file entry is malformed");
        File f;
        f.path = e.get("path").get<std::string>();
        f.sha256 = e.get("sha256").get<std::string>();
        const int64_t size = e.get("size").get<int64_t>();
        f.role = e.contains("role") && e.get("role").is<std::string>() ? e.get("role").get<std::string>() : "";
        if (!SafeRelPath(f.path))
            return fail(error, "a file path is not allowed");
        if (!IsHex64(f.sha256))
            return fail(error, "a file hash is not a SHA-256");
        if (size < 0 || static_cast<uint64_t>(size) > kMaxFile)
            return fail(error, "a file size is out of bounds");
        f.size = static_cast<size_t>(size);
        if (!seen.insert(lower(f.path)).second)
            return fail(error, "a path is listed twice");
        total += f.size;
        if (total > kMaxPack)
            return fail(error, "the pack is over the size limit");
        if (!AllowedPath(f.path, f.role))
            continue; // a file this version does not use (older clients list others): never fetched
        out.files.push_back(std::move(f));
    }
    if (v.contains("colors") && v.get("colors").is<picojson::object>()) {
        const auto& c = v.get("colors");
        const auto file = [](const picojson::value& e, size_t max, ColorFile& f) {
            if (!e.is<picojson::object>() || !e.contains("sha256") || !e.get("sha256").is<std::string>() || !e.contains("size") ||
                !e.get("size").is<int64_t>())
                return false;
            f.sha256 = e.get("sha256").get<std::string>();
            const int64_t size = e.get("size").get<int64_t>();
            if (!IsHex64(f.sha256) || size <= 0 || static_cast<uint64_t>(size) > max)
                return false;
            f.size = static_cast<size_t>(size);
            return true;
        };
        if (c.contains("metadata")) {
            ColorFile f;
            if (file(c.get("metadata"), kMaxMetadata, f))
                out.metadata = std::move(f);
        }
        if (c.contains("masks") && c.get("masks").is<picojson::array>()) {
            std::set<std::string> paths;
            size_t total = 0;
            for (const auto& e : c.get("masks").get<picojson::array>()) {
                ColorFile f;
                if (out.masks.size() >= kMaxMasks || !file(e, kMaxMaskFile, f) || !e.contains("path") || !e.get("path").is<std::string>())
                    continue;
                f.path = e.get("path").get<std::string>();
                total += f.size;
                if (!MaskPathOk(f.path) || total > kMaxMasksTotal || !paths.insert(f.path).second)
                    continue;
                out.masks.push_back(std::move(f));
            }
        }
    }
    if (v.contains("parts") && v.get("parts").is<picojson::object>()) {
        const auto& p = v.get("parts");
        out.has_parts = true;
        out.link = p.contains("link") && p.get("link").is<bool>() && p.get("link").get<bool>();
        out.voice = p.contains("voice") && p.get("voice").is<bool>() && p.get("voice").get<bool>();
        if (p.contains("equipment") && p.get("equipment").is<std::string>()) {
            out.equipment = p.get("equipment").get<std::string>();
            if (out.equipment != "model" && out.equipment != "equipment" && !out.equipment.empty())
                return fail(error, "the equipment part is not known");
        }
    }
    return true;
}

// ------------------------------------------------------------------ J3D ----

bool ValidateJ3d(const std::vector<uint8_t>& f, const std::string& ext, std::string* error) {
    static const struct {
        const char* ext;
        const char* magics[3];
    } kMagic[] = {{"bdl", {"J3D2bdl3", "J3D2bdl4", nullptr}}, {"bmd", {"J3D2bmd2", "J3D2bmd3", nullptr}},
                  {"bck", {"J3D1bck1", nullptr, nullptr}},    {"brk", {"J3D1brk1", nullptr, nullptr}},
                  {"btk", {"J3D1btk1", nullptr, nullptr}}};
    if (f.size() < 0x40 || f.size() > kMaxMember)
        return fail(error, "a J3D file has an impossible size");
    bool magic = false;
    for (const auto& k : kMagic)
        if (ext == k.ext)
            for (const char* m : k.magics)
                magic |= m != nullptr && std::memcmp(f.data(), m, 8) == 0;
    if (!magic)
        return fail(error, "a J3D file has the wrong magic for its type");
    const uint32_t size = be32(f, 8);
    // The size in the header is the file's, or a little less (the archive pads files).
    if (size < 0x40 || size > f.size() || size + 64u < f.size())
        return fail(error, "a J3D file's size field does not match");
    const uint32_t chunks = be32(f, 12);
    if (chunks == 0 || chunks > 32)
        return fail(error, "a J3D file has an impossible chunk count");
    // Models: the chunks must fill the file exactly, as the game's loader walks them. Animations (bck, brk, btk) are
    // not that tidy in the game's own files (the size of the last chunk is off by a few bytes): their chunks must
    // start inside the file and be at least a header long.
    const bool animation = ext != "bdl" && ext != "bmd";
    size_t at = 0x20;
    for (uint32_t i = 0; i < chunks; ++i) {
        if (at + 8 > size)
            return fail(error, "a J3D chunk runs past the file");
        const uint32_t chunk = be32(f, at + 4);
        if (chunk < 8 || (!animation && chunk > size - at) || (animation && chunk > size + 64u))
            return fail(error, "a J3D chunk size is out of bounds");
        at += chunk;
    }
    if (!animation && at != size)
        return fail(error, "a J3D file's chunks do not fill it");
    return true;
}

bool ValidateBti(const std::vector<uint8_t>& f, std::string* error) {
    if (f.size() < 0x20 || f.size() > kMaxMember)
        return fail(error, "a texture has an impossible size");
    const uint8_t format = f[0];
    const uint32_t width = be16(f, 2), height = be16(f, 4);
    if (width == 0 || height == 0 || width > 2048 || height > 2048)
        return fail(error, "a texture has impossible dimensions");
    uint32_t bw = 4, bh = 4, bytes = 32;
    switch (format) {
    case 0: bw = 8; bh = 8; break;          // I4
    case 1: bw = 8; bh = 4; break;          // I8
    case 2: bw = 8; bh = 4; break;          // IA4
    case 3: break;                          // IA8
    case 4: break;                          // RGB565
    case 5: break;                          // RGB5A3
    case 6: bytes = 64; break;              // RGBA32
    case 8: bw = 8; bh = 8; break;          // C4
    case 9: bw = 8; bh = 4; break;          // C8
    case 10: break;                         // C14X2
    case 14: bw = 8; bh = 8; break;         // CMPR
    default: return fail(error, "a texture has an unknown format");
    }
    const uint32_t mips = f[0x18] == 0 ? 1u : f[0x18]; // some tools write 0 for a single image
    if (mips > 12)
        return fail(error, "a texture has an impossible mip count");
    const uint32_t image = be32(f, 0x1C);
    if (image < 0x20 || image > f.size())
        return fail(error, "a texture's image offset is out of bounds");
    uint64_t need = 0;
    uint32_t w = width, h = height;
    for (uint32_t i = 0; i < mips; ++i) {
        need += static_cast<uint64_t>((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
        if (w == 1 && h == 1)
            break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    if (image + need > f.size())
        return fail(error, "a texture's image runs past the file");
    if (f[8] != 0) { // palette
        const uint32_t colors = be16(f, 0x0A), palette = be32(f, 0x0C);
        if (colors == 0 || colors > 16384 || palette < 0x20 || static_cast<uint64_t>(palette) + colors * 2u > f.size())
            return fail(error, "a texture's palette is out of bounds");
    }
    return true;
}

// ----------------------------------------------------------------- RARC ----

bool ValidateRarc(const std::vector<uint8_t>& arc, std::string* error) {
    if (arc.size() < 0x40 || arc.size() > kMaxArchive)
        return fail(error, "the archive has an impossible size");
    if (std::memcmp(arc.data(), "RARC", 4) != 0)
        return fail(error, "not a RARC archive");
    const uint64_t size = arc.size();
    if (be32(arc, 4) > size || be32(arc, 8) != 0x20)
        return fail(error, "the RARC header is wrong");
    const uint64_t data = 0x20ull + be32(arc, 0x0C), data_length = be32(arc, 0x10);
    if (be32(arc, 0x14) != data_length || be32(arc, 0x18) != 0 || be32(arc, 0x1C) != 0)
        return fail(error, "the RARC is not kept whole in main memory");
    if (data + data_length > size)
        return fail(error, "the RARC data runs past the file");
    const uint32_t nodes = be32(arc, 0x20), files = be32(arc, 0x28), strings_size = be32(arc, 0x30);
    const uint64_t node_at = 0x20ull + be32(arc, 0x24), entry_at = 0x20ull + be32(arc, 0x2C), strings = 0x20ull + be32(arc, 0x34);
    if (nodes == 0 || nodes > kMaxNodes || files == 0 || files > kMaxMembers || strings_size == 0 || strings_size > kMaxStrings)
        return fail(error, "the RARC tables have impossible counts");
    if (node_at + static_cast<uint64_t>(nodes) * 0x10 > size || entry_at + static_cast<uint64_t>(files) * 0x14 > size ||
        strings + strings_size > size)
        return fail(error, "a RARC table runs past the file");
    const auto name_at = [&](uint32_t offset, std::string& name) {
        if (offset >= strings_size)
            return false;
        size_t n = 0;
        while (offset + n < strings_size && arc[strings + offset + n] != 0)
            ++n;
        if (offset + n >= strings_size || n == 0 || n > 64) // not terminated inside the table, or empty, or long
            return false;
        name.assign(reinterpret_cast<const char*>(&arc[strings + offset]), n);
        return true;
    };
    for (uint32_t i = 0; i < nodes; ++i) {
        const uint64_t n = node_at + static_cast<uint64_t>(i) * 0x10;
        std::string name;
        if (!name_at(be32(arc, n + 4), name))
            return fail(error, "a RARC folder has a bad name");
        const uint64_t first = be32(arc, n + 0x0C), count = be16(arc, n + 0x0A);
        if (first + count > files)
            return fail(error, "a RARC folder lists files that are not there");
    }
    std::set<std::string> seen;
    uint64_t total = 0;
    int members = 0;
    for (uint32_t i = 0; i < files; ++i) {
        const uint64_t e = entry_at + static_cast<uint64_t>(i) * 0x14;
        std::string name;
        if (!name_at(be16(arc, e + 6), name))
            return fail(error, "a RARC entry has a bad name");
        const uint8_t type = arc[e + 4];
        const uint32_t offset = be32(arc, e + 8), length = be32(arc, e + 12);
        if (type & 0x02) { // a folder: only the dot entries and named folders, pointing at a folder node
            if (offset != 0xFFFFFFFFu && offset >= nodes)
                return fail(error, "a RARC folder entry points outside the folder table");
            for (const char c : name)
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-'))
                    return fail(error, "a RARC folder name has characters that are not allowed");
            continue;
        }
        // A file: loaded into main memory as it is. Compressed, ARAM and DVD files (flags 0x04, 0x20, 0x40, 0x80) would
        // make the game's archive code decompress or stream something we did not check.
        if (type & 0xE4)
            return fail(error, "a RARC member is compressed or not in main memory");
        for (const char c : name)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-'))
                return fail(error, "a RARC member name has characters that are not allowed");
        if (!seen.insert(lower(name)).second)
            return fail(error, "a RARC member name is listed twice");
        if (static_cast<uint64_t>(offset) + length > data_length || length > kMaxMember)
            return fail(error, "a RARC member runs past the data");
        total += length;
        if (total > kMaxArchive || ++members > static_cast<int>(kMaxMembers))
            return fail(error, "the RARC members are too many or too large");
        const size_t dot = name.rfind('.');
        const std::string ext = dot == std::string::npos ? "" : lower(name.substr(dot + 1));
        const std::vector<uint8_t> bytes(arc.begin() + static_cast<std::ptrdiff_t>(data + offset),
                                         arc.begin() + static_cast<std::ptrdiff_t>(data + offset + length));
        std::string why;
        if (ext == "bdl" || ext == "bmd" || ext == "bck" || ext == "brk" || ext == "btk") {
            if (!ValidateJ3d(bytes, ext, &why))
                return fail(error, "member " + name + ": " + why);
        } else if (ext == "bti") {
            if (!ValidateBti(bytes, &why))
                return fail(error, "member " + name + ": " + why);
        } else {
            return fail(error, "a RARC member has a file type the game does not read from Link.arc");
        }
    }
    if (members == 0)
        return fail(error, "the RARC holds no files");
    return true;
}

bool ExpandArchive(std::vector<uint8_t>& data, std::string* error) {
    if (data.size() > kMaxFile)
        return fail(error, "the archive is over the size limit");
    if (data.size() >= 4 && std::memcmp(data.data(), "Yaz0", 4) == 0) {
        if (!rarc::Yaz0Expand(data, kMaxArchive))
            return fail(error, "the Yaz0 data is damaged or declares too much output");
    }
    return ValidateRarc(data, error);
}

bool ExtractBody(const std::vector<uint8_t>& arc, std::vector<uint8_t>& bdl, std::string* error) {
    if (!rarc::Extract(arc, "cl.bdl", bdl))
        return fail(error, "the archive has no cl.bdl");
    return true;
}

bool ExtractBodyParts(const std::vector<uint8_t>& arc, std::vector<uint8_t>& hands, std::vector<uint8_t>& hair) {
    hands.clear();
    hair.clear();
    rarc::Extract(arc, "hands.bdl", hands);
    rarc::Extract(arc, "katsura.bdl", hair);
    return !hands.empty() || !hair.empty();
}

bool ExtractEquipment(const std::vector<uint8_t>& arc, std::map<std::string, std::vector<uint8_t>>& models, std::string* error) {
    models.clear();
    for (const std::string& name : playerparts::PuppetEquipmentMembers()) {
        std::vector<uint8_t> bytes;
        if (rarc::Extract(arc, name, bytes))
            models[name] = std::move(bytes);
    }
    if (models.empty())
        return fail(error, "the archive has none of the equipment models");
    return true;
}

// ----------------------------------------------------------- colour pack ----

bool MaskPathOk(const std::string& path) {
    const std::string dir = "color_masks/";
    if (path.size() < dir.size() + 5 || path.size() > 160 || path.compare(0, dir.size(), dir) != 0 ||
        path.compare(path.size() - 4, 4, ".png") != 0)
        return false;
    std::string name = path.substr(dir.size(), path.size() - dir.size() - 4);
    if (name.compare(0, 7, "mouths/") == 0)
        name = name.substr(7);
    if (name.empty() || name.size() > 80)
        return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' || c == '-'))
            return false;
    return true;
}

bool ValidateMetadata(const std::vector<uint8_t>& f, std::string* error) {
    if (f.empty() || f.size() > kMaxMetadata)
        return fail(error, "metadata.txt has an impossible size");
    size_t lines = 1, line = 0;
    for (size_t i = 0; i < f.size();) {
        const uint8_t c = f[i];
        size_t n = 1;
        if (c >= 0x80) { // UTF-8, strictly: no overlong forms, surrogates or values past U+10FFFF
            n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            if (c < 0xC2 || c > 0xF4 || i + n > f.size())
                return fail(error, "metadata.txt is not UTF-8 text");
            uint32_t cp = c & (0xFFu >> (n + 1));
            for (size_t k = 1; k < n; ++k) {
                if ((f[i + k] & 0xC0) != 0x80)
                    return fail(error, "metadata.txt is not UTF-8 text");
                cp = cp << 6 | (f[i + k] & 0x3Fu);
            }
            if ((n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
                return fail(error, "metadata.txt is not UTF-8 text");
        } else if (c == '\n' || c == '\r') {
            if (!(c == '\n' && i > 0 && f[i - 1] == '\r') && ++lines > 4000)
                return fail(error, "metadata.txt has too many lines");
            line = 0;
            ++i;
            continue;
        } else if ((c < 0x20 && c != '\t') || c == 0x7F) {
            return fail(error, "metadata.txt has control characters");
        }
        i += n;
        if (++line > 2000)
            return fail(error, "metadata.txt has a line that is too long");
    }
    return true;
}

namespace {

uint32_t crc32(const uint8_t* p, size_t n) {
    static const std::vector<uint32_t> table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// The ancillary chunks a colour mask may carry (harmless, small); the critical ones are handled by name.
bool mask_chunk_kept(const char* type) {
    for (const char* k : {"gAMA", "cHRM", "sRGB", "sBIT", "pHYs", "bKGD", "tIME"})
        if (std::memcmp(type, k, 4) == 0)
            return true;
    return false;
}

} // namespace

bool ValidateMask(const std::vector<uint8_t>& f, std::string* error) {
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const size_t n = f.size();
    if (n < 57 || n > kMaxMaskFile || std::memcmp(f.data(), sig, 8) != 0)
        return fail(error, "a mask is not a PNG of an acceptable size");
    size_t at = 8;
    uint32_t w = 0, h = 0, depth = 0, ctype = 0;
    bool first = true, ended = false, plte = false, trns = false, idat = false, idat_over = false;
    size_t palette = 0;
    while (at < n) {
        if (ended || at + 12 > n)
            return fail(error, "a PNG has data after its end, or a chunk that runs past the file");
        const uint32_t len = be32(f, at);
        if (len > n - at - 12)
            return fail(error, "a PNG chunk runs past the file");
        const char* type = reinterpret_cast<const char*>(&f[at + 4]);
        if (crc32(&f[at + 4], 4 + len) != be32(f, at + 8 + len))
            return fail(error, "a PNG chunk fails its checksum");
        const size_t d = at + 8;
        const auto is = [&](const char* t) { return std::memcmp(type, t, 4) == 0; };
        if (first != is("IHDR"))
            return fail(error, "a PNG header is missing or repeated");
        if (is("IHDR")) {
            w = be32(f, d), h = be32(f, d + 4), depth = f[d + 8], ctype = f[d + 9];
            const bool depth_ok = ctype == 0   ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)
                                  : ctype == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                                               : (ctype == 2 || ctype == 4 || ctype == 6) && (depth == 8 || depth == 16);
            if (len != 13 || w < 1 || h < 1 || w > kMaxMaskSide || h > kMaxMaskSide || !depth_ok || f[d + 10] != 0 || f[d + 11] != 0 ||
                f[d + 12] > 1)
                return fail(error, "the PNG header is not acceptable");
        } else if (is("PLTE")) {
            if (plte || idat || ctype == 0 || ctype == 4 || len == 0 || len % 3 != 0 || len > 768 || (ctype == 3 && len / 3 > (1u << depth)))
                return fail(error, "a PNG palette is misplaced or malformed");
            plte = true;
            palette = len / 3;
        } else if (is("tRNS")) {
            if (trns || idat || ctype == 4 || ctype == 6 || (ctype == 0 && len != 2) || (ctype == 2 && len != 6) ||
                (ctype == 3 && (!plte || len < 1 || len > palette)))
                return fail(error, "a PNG transparency chunk is misplaced or malformed");
            trns = true;
        } else if (is("IDAT")) {
            if (idat_over)
                return fail(error, "a PNG's image data is not contiguous");
            idat = true;
        } else if (is("IEND")) {
            if (len != 0 || !idat)
                return fail(error, "a PNG ends without image data");
            ended = true;
        } else if (mask_chunk_kept(type)) {
            if (idat || len > 1024)
                return fail(error, "a PNG chunk is misplaced or too large");
        } else {
            return fail(error, "a PNG carries a chunk a colour mask must not");
        }
        if (!is("IDAT") && idat)
            idat_over = true;
        first = false;
        at += 12 + len;
    }
    if (!ended || (ctype == 3 && !plte))
        return fail(error, "a PNG has no end, or an indexed one no palette");
    // The deflate stream and the scanlines: the decoder walks all of it (a short stream is refused; the size is bounded
    // by the sides above).
    png_decode::Image image;
    std::string why;
    if (!png_decode::Decode(f.data(), f.size(), image, &why))
        return fail(error, "a PNG does not decode: " + why);
    if (image.width != w || image.height != h)
        return fail(error, "a PNG decodes to another size than its header's");
    return true;
}

bool SanitizeMask(std::vector<uint8_t>& f, std::string* error) {
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (f.size() < 57 || std::memcmp(f.data(), sig, 8) != 0)
        return fail(error, "not a PNG");
    std::vector<uint8_t> out(f.begin(), f.begin() + 8);
    size_t at = 8;
    while (at + 12 <= f.size()) {
        const uint32_t len = be32(f, at);
        if (len > f.size() - at - 12)
            return fail(error, "a PNG chunk runs past the file");
        const char* type = reinterpret_cast<const char*>(&f[at + 4]);
        const bool critical = !std::memcmp(type, "IHDR", 4) || !std::memcmp(type, "PLTE", 4) || !std::memcmp(type, "IDAT", 4) ||
                              !std::memcmp(type, "IEND", 4) || !std::memcmp(type, "tRNS", 4);
        if (critical || mask_chunk_kept(type))
            out.insert(out.end(), f.begin() + static_cast<std::ptrdiff_t>(at), f.begin() + static_cast<std::ptrdiff_t>(at + 12 + len));
        const bool end = !std::memcmp(type, "IEND", 4);
        at += 12 + len;
        if (end) { // anything after IEND is dropped
            f = std::move(out);
            return true;
        }
    }
    return fail(error, "a PNG has no end");
}

// ---------------------------------------------------------------- audio ----

bool ValidateAaf(const std::vector<uint8_t>& f, std::string* error) {
    if (f.size() < 16 || f.size() > kMaxVoiceInit || f.size() % 4 != 0)
        return fail(error, "JaiInit.aaf has an impossible size");
    const size_t words = f.size() / 4;
    const auto w = [&](size_t i) { return be32(f, i * 4); };
    const auto region = [&](size_t offset, size_t length) { return offset <= f.size() && length <= f.size() - offset; };
    size_t r = 0;
    for (int record = 0; record < 64; ++record) {
        if (r >= words)
            return fail(error, "JaiInit.aaf ends without its terminator");
        const uint32_t type = w(r++);
        switch (type) {
        case 0:
            return true;
        case 1: case 5: case 6: case 7: case 8:
            if (r + 3 > words)
                return fail(error, "a JaiInit.aaf record runs past the file");
            if (!region(w(r), type == 8 ? (w(r + 1) & 0xFFF0u) + 16u : w(r + 1)))
                return fail(error, "a JaiInit.aaf record points outside the file");
            r += 3;
            break;
        case 2: case 3: {
            size_t entries = 0;
            while (r < words && w(r) != 0) { // (offset, size, id) triples ended by a zero offset
                if (r + 3 > words || ++entries > 256)
                    return fail(error, "a JaiInit.aaf table is too long or runs past the file");
                if (!region(w(r), w(r + 1)))
                    return fail(error, "a JaiInit.aaf table entry points outside the file");
                r += 3;
            }
            if (r >= words)
                return fail(error, "a JaiInit.aaf table is not terminated");
            ++r;
            break;
        }
        case 4:
            if (r + 3 > words)
                return fail(error, "a JaiInit.aaf record runs past the file");
            r += 3;
            break;
        default: // a record this version does not know: the game skips to the next zero word
            while (r < words && w(r) != 0)
                ++r;
            if (r >= words)
                return fail(error, "an unknown JaiInit.aaf record is not terminated");
            ++r;
            break;
        }
    }
    return fail(error, "JaiInit.aaf has too many records");
}

bool ValidateWave(const std::vector<uint8_t>& f, std::string* error) {
    if (f.size() < 32 || f.size() > kMaxVoiceWave || f.size() % 32 != 0)
        return fail(error, "a wave bank has an impossible size");
    return true;
}

} // namespace content_validate
