// Discord Rich Presence over Discord's local IPC (a named pipe on Windows, a Unix socket on Linux), no Discord SDK. Shows
// "Playing Boat of Daphnes (Wind Waker PC)" with where Link is, single player or online (a count only), and the time
// played. Own thread: the game never waits for Discord, which may not be running.
#pragma once

namespace shipmenu {
class Menu;
}

namespace discord {

// Network > Discord.
void BuildMenu(shipmenu::Menu& menu);

// Once a frame from the UI thread: reads the game's state and hands the activity to the worker thread.
void Frame();
// At exit: clears the activity (a short bounded wait) and stops the thread.
void Shutdown();

} // namespace discord
