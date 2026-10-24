// A standalone check of the colour pack validators (content_validate.cpp) on a real model folder and on damaged copies of its
// masks, and of the manifest's "colors" section. Build with any C++20 compiler, from the folder that holds the sources:
//   clang++ -std=c++20 -I<picojson's folder> -I. -DPICOJSON_USE_INT64 content_colors_check.cpp content_validate.cpp rarc.cpp
//           player_parts.cpp png_decode.cpp
// (copy or add the include paths for src/bod/windows/src and src/bod/include), then
//   content_colors_check <a manifest with a colour pack, as a file> <a model folder: metadata.txt and color_masks\>
// (the manifest: curl the content hash a client with a model advertises from the server, see server/anchor/test/colors.mjs).
#include "content_validate.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static Bytes read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    int failures = 0;
    auto check = [&](const char* what, bool ok) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    };
    // The manifest of a newer client, read by this code.
    {
        const Bytes m = read(argv[1]);
        content_validate::Manifest man;
        std::string why;
        const bool ok = content_validate::ParseManifest(std::string(m.begin(), m.end()), man, &why);
        std::printf("manifest: %d, %zu files, colors metadata %zu bytes, %zu masks (%s)\n", ok, man.files.size(), man.metadata.size, man.masks.size(), why.c_str());
        check("the manifest with a colour pack parses", ok && man.metadata.size != 0 && !man.masks.empty());
    }
    // Every mask of the model: sanitized, then valid; and damaged copies are not.
    int masks = 0, valid = 0, damaged_ok = 0, sanitized_valid = 0, texts_removed = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(argv[2]) / "color_masks")) {
        if (!e.is_regular_file() || e.path().extension() != ".png")
            continue;
        ++masks;
        Bytes png = read(e.path());
        const size_t before = png.size();
        std::string why;
        const bool raw_valid = content_validate::ValidateMask(png, &why);
        if (!content_validate::SanitizeMask(png, &why))
            continue;
        texts_removed += png.size() < before;
        sanitized_valid += content_validate::ValidateMask(png, &why);
        valid += raw_valid;
        for (size_t i = 8; i < png.size(); i += png.size() / 40 + 1) {
            Bytes bad = png;
            bad[i] ^= 0x55;
            damaged_ok += content_validate::ValidateMask(bad, &why);
        }
        for (size_t n = 0; n < png.size(); n += png.size() / 25 + 1) {
            Bytes cut(png.begin(), png.begin() + static_cast<std::ptrdiff_t>(n));
            damaged_ok += content_validate::ValidateMask(cut, &why);
        }
        Bytes trailing = png;
        trailing.push_back(0);
        damaged_ok += content_validate::ValidateMask(trailing, &why);
    }
    std::printf("%d masks: %d valid as they are, %d valid after sanitizing (%d had chunks removed), %d damaged copies accepted\n", masks, valid, sanitized_valid, texts_removed, damaged_ok);
    check("every mask is valid after sanitizing", masks > 0 && sanitized_valid == masks);
    check("no damaged or truncated mask passes", damaged_ok == 0);
    const Bytes meta = read(fs::path(argv[2]) / "metadata.txt");
    std::string why;
    check("metadata.txt is valid", content_validate::ValidateMetadata(meta, &why));
    Bytes nul = meta;
    nul[3] = 0;
    check("metadata with a NUL is refused", !content_validate::ValidateMetadata(nul, &why));
    Bytes bad8 = meta;
    bad8[3] = 0xC0;
    check("metadata that is not UTF-8 is refused", !content_validate::ValidateMetadata(bad8, &why));
    check("a mask path is checked", content_validate::MaskPathOk("color_masks/hero_hair.png") && content_validate::MaskPathOk("color_masks/mouths/mouths3tc.1_skin.png") &&
                                       !content_validate::MaskPathOk("color_masks/../x.png") && !content_validate::MaskPathOk("color_masks/A.png") &&
                                       !content_validate::MaskPathOk("other/a.png") && !content_validate::MaskPathOk("color_masks/a.exe"));
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
