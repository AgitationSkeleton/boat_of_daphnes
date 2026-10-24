// Link's custom colors in the game: the Cosmetics Editor's Link colors applied
// by recoloring Link's textures on the PC and handing them to the renderer as
// replacements under the textures' own keys, so a change shows at once.
//
// The textures and wwrando's region masks come from the player's disc and
// wwrando download at build time (scripts/windows/link_colors.py) into
// assets\link_colors beside the executable. The recolor is wwrando's color
// exchange (customizer.py, texture_utils.color_exchange; MIT, LagoLunatic).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace model_colors {
struct Set;
}

namespace link_colors {

// Loads assets\link_colors (once); false when it is not there.
bool Load();
// The colors changed: recolor at the next Frame.
void MarkDirty();
// Once a frame: recolors and registers what changed (at most 10 times a second).
void Frame();

// A player model's colors instead of the vanilla Link's: its own textures and masks (model_colors.h)
// recolored with the colors set by SetModelColor, registered as replacements at the next Frame. The
// vanilla replacements are removed meanwhile. nullptr is the vanilla Link again. The set must stay
// alive until the next call.
void UseModel(const model_colors::Set* set);
// The Link.arc the player's equipment comes from when it is a mod's (the model's own, or another mod's Equipment part):
// its swords, shields and items take the Equipment colours too, each texture shifted from its own main colour. Null:
// the game's equipment (the colours apply to the disc's textures).
void UseEquipmentArc(const std::vector<uint8_t>* arc);
// The editor's color for a model color id (model_colors::ColorId): its value, and whether the player
// changed it from the model's own.
void SetModelColor(const std::string& id, uint32_t rgb, bool changed);

// The local player's changed Link colors as text ("Hero.Tunic=5ab24a,Casual.Hair=ffef10"; "" when all are the game's own).
std::string LocalSpec();
// Another player's puppet (variant = their player id): every recolorable texture in the colors of `spec` (LocalSpec's
// format), or the game's own for "". Cheap when the spec is unchanged.
void SetPlayerColors(int id, const std::string& spec);
// Another player's puppet wears a player model of its own (its Link.arc, metadata and masks, received through the content
// server and built by the content worker): the model's textures are then recolored in the model colors of the player's spec
// ("M.Hero.Tunic=5ab24a": the entries LocalSpec writes while a model is in use) and registered under the player's variant,
// as the vanilla ones are. nullptr: the puppet has no model of its own again. Takes effect at the next SetPlayerColors.
void SetPlayerModel(int id, std::shared_ptr<const model_colors::Set> set);
void ForgetPlayer(int id);

} // namespace link_colors
