#pragma once
// The first-run window (src/setup/setup_ui.cpp): before the game starts, when its code is not built yet.
#include <filesystem>

namespace setup {

// Shows the window until the game can start: true with `disc` set (chosen there, or the remembered one), false when the
// player quit. `rememberedDisc`: `disc` holds the disc used before (the build starts at once).
bool runFirstRun(const std::filesystem::path& exeDir, std::filesystem::path& disc, bool rememberedDisc);

// The same build without a window (--setup --iso FILE): progress lines on standard output, 0 when the game is built (or was),
// 1 when the build failed, 2 when the disc is not the right one. For servers, scripts and packagers.
int runFirstRunCli(const std::filesystem::path& exeDir, const std::filesystem::path& disc);

}  // namespace setup
