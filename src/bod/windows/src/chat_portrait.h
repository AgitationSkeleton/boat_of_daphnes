// The small front-facing head portrait Anchor shows beside a player's chat lines and chat toasts (Network > Anchor > Chat
// Portraits): the player's own model (the game's Link, or the custom model they share) in their colors, drawn once by
// head_render.cpp and kept as a PNG in cache\anchor-portraits beside the downloaded player models (cache\anchor-content), named
// by a hash of the model and the colors, so the same player looks the same next session without drawing again.
#pragma once

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace model_colors {
struct Set;
}

namespace chat_portrait {

// What a portrait is made of.
struct Source {
    std::string model;                                 // "" the game's Link; else what identifies the model (the content's manifest hash)
    std::string spec;                                  // their Link colors (link_colors::LocalSpec's format), "" the game's own
    std::shared_ptr<const std::vector<uint8_t>> body;  // the model's cl.bdl (null with model "")
    std::shared_ptr<const model_colors::Set> colors;   // the model's colors (null: none)
};

// Network > Anchor > Chat Portraits (gNetwork.Anchor.ChatPortraits, on by default).
bool Enabled();
// The portrait as an ImGui texture; 0 when it cannot be had yet (the disc is not open) or at all. Drawn on first use.
ImTextureID Get(const Source& source);
// Removes the portraits from the cache folder (sharing custom player content was turned off). Returns the files removed.
int PurgeDisk();

} // namespace chat_portrait
