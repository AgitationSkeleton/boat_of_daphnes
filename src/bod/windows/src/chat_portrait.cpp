// See chat_portrait.h.
#include <cstdio>
#undef rename // bw_posix_compat.h; <filesystem> declares rename()
#include <filesystem>

#include "chat_portrait.h"

#include "anchor_content.h"
#include "head_render.h"
#include "link_colors.h"
#include "model_colors.h"
#include "mods.h"
#include "png_decode.h"
#include "rarc.h"
#include "ship/cvar.h"

#include "platform/bod_platform.h"

#include <aurora/imgui.h>
#include <SDL3/SDL.h>

#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace chat_portrait {
namespace {

namespace fs = std::filesystem;

constexpr int kSize = 64;          // pixels on a side
constexpr size_t kMaxTextures = 160; // the renderer cannot free an ImGui texture: a session makes at most this many
constexpr const char* kVersion = "p2"; // changes when the picture's look does (the cache then starts again)

fs::path folder() {
    static const fs::path dir = [] {
        fs::path p = bodplat::DataFolder();
        if (p.empty())
            p = fs::current_path();
        return p / "cache" / "anchor-portraits"; // beside cache\anchor-content (anchor_content.cpp)
    }();
    return dir;
}

struct Entry {
    ImTextureID texture = 0;
    bool failed = false;
    Uint64 tried = 0;
};
std::map<std::string, Entry> g_entries;
size_t g_made;
std::vector<uint8_t> g_stock_body;

bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

ImTextureID upload(const std::vector<uint8_t>& png) {
    png_decode::Image image;
    if (!png_decode::Decode(png.data(), png.size(), image, nullptr) || image.width != uint32_t(kSize) || image.height != uint32_t(kSize))
        return 0;
    return aurora_imgui_add_texture(image.width, image.height, image.rgba.data());
}

} // namespace

bool Enabled() { return CVarGetInteger("gNetwork.Anchor.ChatPortraits", 1) != 0; }

ImTextureID Get(const Source& s) {
    const std::string text = std::string(kVersion) + "|" + std::to_string(kSize) + "|" + s.model + "|" + s.spec;
    const std::string name = content::Sha256Hex(reinterpret_cast<const uint8_t*>(text.data()), text.size()).substr(0, 24);
    Entry& e = g_entries[name];
    if (e.texture != 0)
        return e.texture;
    // A failure is tried again after a few seconds (the disc may not be open yet, the model's colors may be coming).
    if (e.failed && SDL_GetTicks() - e.tried < 5000)
        return 0;
    if (g_made >= kMaxTextures)
        return 0;
    e.failed = true;
    e.tried = SDL_GetTicks();
    std::vector<uint8_t> png;
    const fs::path path = folder() / (name + ".png");
    if (read_file(path, png)) {
        e.texture = upload(png);
        if (e.texture != 0) {
            e.failed = false;
            ++g_made;
            return e.texture;
        }
    }
    // Draw it: the model's head with the textures the colors make.
    const std::vector<uint8_t>* body = s.body.get();
    if (body == nullptr) {
        if (g_stock_body.empty()) {
            std::vector<unsigned char> stock;
            std::map<std::string, std::vector<unsigned char>> equipment;
            if (!mods::VanillaPuppetParts(stock, equipment))
                return 0; // the disc is not open yet
            g_stock_body.assign(stock.begin(), stock.end());
        }
        body = &g_stock_body;
    }
    head_render::Replacements replace;
    link_colors::PortraitTextures(s.spec, s.colors.get(), replace);
    std::vector<uint8_t> rgba;
    std::string error;
    if (!head_render::Render(*body, replace, kSize, rgba, &error)) {
        std::fprintf(stderr, "[portrait] %s (%s): %s\n", name.c_str(), s.model.empty() ? "the game's Link" : s.model.c_str(), error.c_str());
        return 0;
    }
    head_render::EncodePng(rgba.data(), kSize, kSize, png);
    std::error_code ec;
    fs::create_directories(folder(), ec);
    {
        const fs::path tmp = folder() / (name + ".part");
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
        out.close();
        fs::rename(tmp, path, ec);
    }
    e.texture = aurora_imgui_add_texture(kSize, kSize, rgba.data());
    if (e.texture != 0) {
        e.failed = false;
        ++g_made;
    }
    std::fprintf(stderr, "[portrait] made %s for %s in [%s]%s\n", name.c_str(), s.model.empty() ? "the game's Link" : s.model.c_str(), s.spec.c_str(),
                 ec ? " (not saved)" : "");
    return e.texture;
}

int PurgeDisk() {
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(folder(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code rc;
        if (it->is_regular_file(rc) && fs::remove(it->path(), rc))
            ++n;
    }
    return n;
}

} // namespace chat_portrait
