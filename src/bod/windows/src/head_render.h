// A small software renderer for a player model's head: the front-facing portrait Anchor shows beside a player's chat lines.
//
// It reads the model's cl.bdl (a J3D binary display list model: the game's own, or a player model's from the content server)
// at its rest pose, puts the textures the player's colours make in place of the model's (found by the renderer's replacement
// key, gx_texture::Key, exactly as the game's own recolour finds them), and draws the head from the front with a z-buffer.
// No game data lives here; the model and the textures come in as bytes. Pure code with no game, UI or SDL dependencies, so
// tests/head_render_test.cpp builds it alone. A port of the geometry reader of tools/rando/bunny/j3d_geom.py.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace head_render {

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, top row first
};

// Replacement textures by the renderer's key (gx_texture::Key of the model's own texture): the recoloured ones.
using Replacements = std::map<std::string, Image>;

// The head of the model in `bdl`, size x size pixels (transparent where there is no model). False (and *error) when the model
// cannot be read or has no head.
bool Render(const std::vector<uint8_t>& bdl, const Replacements& replace, int size, std::vector<uint8_t>& rgba, std::string* error);

// The picture as a PNG (8 bit RGBA, not compressed beyond what a stored deflate gives: portraits are a few KB).
void EncodePng(const uint8_t* rgba, uint32_t width, uint32_t height, std::vector<uint8_t>& out);

} // namespace head_render
