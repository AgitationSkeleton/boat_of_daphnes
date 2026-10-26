// Notifications ("toasts"), laid out and set as Ship of Harkinian's: small
// windows in a corner of the screen, newest nearest the corner, each an
// optional item icon and a coloured prefix, a message and a coloured suffix,
// fading over the last seconds. Position, duration, background opacity and
// size are options (Settings > General > Notifications). Written from SoH's
// behaviour, with code of its own.
#pragma once

#include <imgui.h>

#include <string>

namespace shipmenu {
class Menu;
}

namespace notifications {

struct Options {
    ImTextureID icon = 0;
    ImTextureID portrait = 0; // a player's round head (chat), drawn before the text; ringed in the prefix color
    std::string prefix;
    ImVec4 prefixColor{0.5f, 0.5f, 1.0f, 1.0f};
    std::string message;
    ImVec4 messageColor{0.7f, 0.7f, 0.7f, 1.0f};
    std::string suffix;
    ImVec4 suffixColor{1.0f, 0.5f, 0.5f, 1.0f};
    float remaining = 0.f; // seconds; 0 takes the Duration option
};

// Shows a notification.
void Emit(Options options);
// Once a frame, in the ImGui frame: ages and draws them. `scale` is the UI
// scale (sizes), `fontScale` the font's.
void Frame(float scale, float fontScale);
// The Notifications section of a settings page.
void BuildMenu(shipmenu::Menu& menu, const char* header, const char* sidebar, int column);

} // namespace notifications
