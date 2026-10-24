// Custom player content shared through the online play server (docs/fork/research/anchor-puppets.md,
// "Custom player content", and docs/fork/anchor-content-security.md): the player's effective choice -- a body (Link)
// from one mod, equipment from the same or another, a voice from a third -- is hashed, uploaded to the server if it
// lacks it, and advertised by its manifest hash in the client state; what other players advertise is downloaded in the
// background into cache\anchor-content\<sha256>, checked (content_validate.cpp), and handed to their puppets. Nothing
// downloaded is ever run: archives are data for the game's model loader, wave banks for the DSP.
//
// The setting Share Custom Player Content (gNetwork.Anchor.ShareContent) governs all of it: off, nothing is uploaded,
// advertised or downloaded, every puppet is stock again, the downloaded cache is purged, and a download that finishes
// after the switch is thrown away.
//
// Used from the app's thread each frame; the network and hashing run on a worker thread.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace voice_bank {
struct Bank;
}
namespace model_colors {
struct Set;
}

namespace content {

// A peer's content, downloaded and checked.
struct Ready {
    int id = 0;
    std::string hash;                         // the manifest's
    std::string name;                         // the pack's name, for the log
    bool has_body = false;                    // body is a mod's Link
    std::vector<uint8_t> body;                // cl.bdl
    std::vector<uint8_t> hands, hair;         // hands.bdl and katsura.bdl of the body's archive (with a custom body)
    bool has_equipment = false;               // equipment is a mod's
    std::map<std::string, std::vector<uint8_t>> equipment; // swgripa.bdl, swa.bdl, sha.bdl, ... (those the pack has)
    // The body's colour data (its metadata.txt and colour masks, which the manifest's optional "colors" section lists), built
    // into the colours the model can take: null when the pack has none (an older client's, or a model without masks).
    std::shared_ptr<const model_colors::Set> colors;
    std::string voice;                        // "" for the game's voice; else an id (hash of the voice files)
    // The voices decoded to PCM at the output rate (voice_bank.h), shared by the players who have the same voice; null when
    // the bank could not be decoded or the memory for banks (32 MB) is full: the puppet then keeps the local voice bank.
    std::shared_ptr<const voice_bank::Bank> bank;
};

// Each frame: the server's HTTP base ("https://host" or "http://host:port"), whether sharing is on, and
// whether the connection is up. Starts the work that is due.
void Tick(const std::string& http_base, bool share, bool connected);
// The manifest hash of the player's own content, "" for none (not sharing, or nothing custom, or still
// uploading); and whether it changed since the last call (the client state is to be sent again).
std::string LocalHash();
// Whether the advertised content has the player's own body model (their Link is a mod's and shared).
bool LocalSharesBody();
bool LocalChanged();
// The id of the player's own voice files as peers see it ("" for the game's); the same ids as Ready::voice.
std::string LocalVoiceId();
// The game's own voice bank, decoded from the disc's Audiores/JaiInit.aaf and Banks/voice_0.aw as a peer's bank is: what a
// stock player's puppet sounds from on a PC whose own voice is a mod's. *pending: the disc is not open yet (ask again).
// *id is the bank's voice id ("disc-" and the hash's first 16 digits). Null when it cannot be had (the puppet then uses the
// local bank).
std::shared_ptr<const voice_bank::Bank> StockVoiceBank(std::string* id, bool* pending);
// A peer advertised this manifest ("" for none).
void Want(int id, const std::string& hash);
void Forget(int id);
// Content that finished downloading since the last call.
std::vector<Ready> TakeReady();
// Peers whose content is not to be used any longer (it went, or sharing was turned off): their puppet is stock again.
std::vector<int> TakeCleared();

// Deletes everything downloaded from other players. (Never the player's own mods.) Returns the files removed.
int PurgeCache();

// At quit: stops the worker thread (it must not be joinable when the process exits).
void Shutdown();

// SHA-256 as lower case hex.
std::string Sha256Hex(const uint8_t* data, size_t size);

} // namespace content
