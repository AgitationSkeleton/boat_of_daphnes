// Settings > Updates: looks for a newer release of Boat of Daphnes on GitHub (the public repo's "latest release", the
// unauthenticated API, no account or token), tells the player with a toast, and installs it: downloads the release's
// archive for this operating system, unpacks it beside the program and swaps the files in when the game closes, then starts
// the new version. The player's own files (saves, settings, logs, mods, the game built from their disc) are never touched.
#pragma once

#include <string>

namespace shipmenu {
class Menu;
}

namespace updater {

// Settings > Updates.
void BuildMenu(shipmenu::Menu& menu);

// Once a frame from the UI thread: the check a few seconds after the start, the toast, the background download.
// `restart` is the menu's own restart (saves, then quits so that the program starts again).
void Frame(void (*restart)());

// When the game has closed, before it is started again (or ends): swaps in a downloaded update if the player asked for
// it (Update Now) or "Install Updates Automatically" is on, and stops the worker thread. Safe to call twice.
void OnExit();

// Whether release tag `latest` is newer than `current` ("v2026.10.24c" or "2026.10.24c"); false when either does not parse.
bool IsNewer(const std::string& latest, const std::string& current);
// Whether the path (relative to the program's folder, '/' separated) belongs to the player and must stay as it is.
bool IsPlayerFile(const std::string& relative, bool existsAlready);

} // namespace updater
