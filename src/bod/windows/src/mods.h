// The mods folder (mods\ beside the exe), as Ship of Harkinian's mod list:
// each folder or archive (.zip, .rar, .7z, read in place) is one entry, new
// entries are enabled, and the list's order is the priority (an entry
// overrides those below it, per file). Entries are disc files served through
// the DVD overlay: loose files under res\ or Audiores\ at their disc paths,
// and wwrando-format player models (Link.arc beside metadata.txt) at the
// paths wwrando's installer writes. mods\textures keeps the Dolphin-format
// texture packs. See docs/fork/research/player-models-colors.md section 4.
#pragma once

#include "model_colors.h"

#include <map>
#include <string>
#include <vector>

namespace shipmenu {
class Menu;
}

namespace mods {

// At launch: the folder, the first scan, and the overlay set when the host
// opens the disc.
void Launch(const std::string& folder, bool read_only);
// Settings > Mod Menu: the list, its order and toggles, Rescan and Apply.
void BuildMenu(shipmenu::Menu& m, const char* header);
// The mods folder; and the list made again from it and put in effect (a model just downloaded there goes on top, enabled).
std::string Folder();
void RescanAndApply();
// Each frame: finishes a voice change asked for while the game runs (see LoadedVoiceId).
void Frame();
// The player model the enabled list gives (the top entry with one), "" for Link.
std::string ActiveModel();
// The files of the player model whose Link.arc the last Apply put in effect (Link.arc, metadata.txt,
// color_masks\*.png), for its colors; false when the game has the disc's Link.
bool ActiveModelFiles(model_colors::Files& out);
// What the active player content holds, for sharing it with other players (Network > Anchor). It can come from
// several mods: Link (the body) from one, Equipment from another, the voice from a third. Each file's role
// ("model": the Link source's Link.arc; "equipment": the Equipment source's Link.arc when it is a different mod's, path
// "equipment/Link.arc"; "voice": sound\JaiInit.aaf, voice_0.aw, GanonT_0.aw) and its bytes. Files over 20 MB are left out.
struct PackFile {
    std::string role, path;
    std::vector<unsigned char> bytes;
};
struct SharedParts {
    bool link = false;         // the body is a mod's
    std::string equipment;     // "" the game's, "model" the model file's, "equipment" the equipment file's
    bool voice = false;        // the voice files are a mod's
};
// False when the game has the disc's Link, equipment and voice.
bool ActivePlayerPack(std::string& name, std::vector<PackFile>& out, SharedParts& parts);
// The id of the voice the game has loaded (its audio init reads the voice files once; a change made while it runs is
// swapped in live, runtime/bod_voice.cpp): "" for the game's own, else
// the first 16 hex digits of the SHA-256 of JaiInit.aaf and of voice_0.aw, joined. A peer with the same id sounds
// right through the local voice bank.
std::string LoadedVoiceId();
// Changes whenever the shared choice does ("" for the game's own).
std::string ShareSignature();
// Which of Link, Equipment and the voice come from mods on this PC (the rest are the game's).
struct LocalParts {
    bool link, equipment, voice;
};
LocalParts LocalState();
// The game's own body model (cl.bdl) and the models a puppet draws as equipment (swgripa.bdl, swa.bdl, sha.bdl,
// swgripms.bdl, swms.bdl, shms.bdl), read from the disc's Link.arc: what a vanilla player's puppet is made of on a PC
// whose own Link is a mod's. False until the disc is open.
bool VanillaPuppetParts(std::vector<unsigned char>& body, std::map<std::string, std::vector<unsigned char>>& equipment);
// fn is called after an Apply that changed the player model (the first one too, when it gives a model),
// from the thread that applied: it must not call back into mods. Colors follow the model through it.
void SetModelListener(void (*fn)());
// The game's own voice files (Audiores/JaiInit.aaf and Audiores/Banks/voice_0.aw) read from the disc: what a stock player's
// puppet sounds from on a PC whose own voice is a mod's. False until the disc is open.
bool VanillaVoiceFiles(std::vector<unsigned char>& aaf, std::vector<unsigned char>& aw);

} // namespace mods
