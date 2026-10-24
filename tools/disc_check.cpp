// Opens a disc image of any supported kind (src/disc: .iso, NKit, .ciso, .gcz, .rvz, .wia) and prints what is on it, as
// text a test compares between formats (tools/test_disc_formats.py): the format, the game id, main.dol's SHA-1, and the
// SHA-1 of every file in its file table. Builds with the project (target tww_disc_check); no disc data is kept anywhere.
#include "disc/discimage.h"
#include "setup/sha1.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string sha1Range(disc::Image& img, uint64_t at, uint64_t size) {
    setup::Sha1 h;
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t done = 0; done < size;) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), size - done));
        if (!img.read(at + done, buf.data(), n)) return "UNREADABLE";
        h.add(buf.data(), n);
        done += n;
    }
    return h.hex();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return std::fprintf(stderr, "usage: disc_check <disc image>\n"), 2;
    std::string err;
    auto img = disc::open(std::filesystem::u8path(argv[1]), err);
    if (!img) return std::printf("ERROR %s\n", err.c_str()), 1;
    std::vector<uint8_t> hdr(0x440);
    if (!img->read(0, hdr.data(), hdr.size())) return std::printf("ERROR can't read the header\n"), 1;
    std::printf("format %s\nid %.6s\n", img->format(), reinterpret_cast<const char*>(hdr.data()));
    // main.dol: its size from its section table
    const uint32_t dolAt = be32(&hdr[0x420]);
    uint8_t dh[0x100];
    if (!img->read(dolAt, dh, sizeof dh)) return std::printf("ERROR can't read main.dol\n"), 1;
    uint32_t dolSize = 0x100;
    for (int i = 0; i < 18; i++) dolSize = std::max(dolSize, be32(dh + i * 4) + be32(dh + 0x90 + i * 4));
    std::printf("main.dol %s\n", sha1Range(*img, dolAt, dolSize).c_str());
    const uint32_t fstAt = be32(&hdr[0x424]), fstSize = be32(&hdr[0x428]);
    std::vector<uint8_t> fst(fstSize);
    if (!fstSize || !img->read(fstAt, fst.data(), fstSize)) return std::printf("ERROR can't read the file table\n"), 1;
    const uint32_t n = be32(&fst[8]);
    const size_t strs = size_t(n) * 12;
    int bad = 0;
    std::function<void(uint32_t, uint32_t, const std::string&)> walk = [&](uint32_t i, uint32_t end, const std::string& prefix) {
        while (i < end) {
            const std::string name(reinterpret_cast<const char*>(&fst[strs + (be32(&fst[i * 12]) & 0xFFFFFF)]));
            if (fst[i * 12]) {
                const uint32_t next = be32(&fst[i * 12 + 8]);
                walk(i + 1, next, prefix + name + "/");
                i = next;
            } else {
                const std::string sum = sha1Range(*img, be32(&fst[i * 12 + 4]), be32(&fst[i * 12 + 8]));
                bad += sum == "UNREADABLE";
                std::printf("file %s%s %u %s\n", prefix.c_str(), name.c_str(), be32(&fst[i * 12 + 8]), sum.c_str());
                i++;
            }
        }
    };
    walk(1, n, "");
    std::printf("unreadable %d\n", bad);
    return bad ? 1 : 0;
}
