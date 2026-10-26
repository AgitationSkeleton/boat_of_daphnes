// The pad as the app's overlay panels see it (the randomizer's file-select menu). Every pad sample the host hands the
// guest passes through bluewake_ui_pad_filter: the panel gets its edges, and while it has focus the game gets no buttons.
#ifndef BLUEWAKE_UI_PAD_H
#define BLUEWAKE_UI_PAD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Game thread: the buttons the guest is about to read (a GameCube pad word); returns what it may read.
uint16_t bluewake_ui_pad_filter(uint16_t buttons);
// App thread: buttons newly pressed since the last call.
uint16_t bluewake_ui_pad_take_pressed(void);
// Focus: while set, the guest reads no buttons. Releasing focus keeps them hidden until every button is up.
void bluewake_ui_pad_set_focus(bool focus);
bool bluewake_ui_pad_focused(void);
// Game thread: called with the buttons that just went down (edges) before the guest sees them. Returning true keeps
// that press, and everything held with it, from the game until every button is up (the randomizer's menu takes
// the A that would start a new file). One gate; NULL clears it.
typedef bool (*BluewakeUiPadGate)(unsigned edges, void* user);
void bluewake_ui_pad_set_gate(BluewakeUiPadGate gate, void* user);
// Game thread: the Control Stick as the guest is about to read it. The stick crossing 30 counts as a D-pad press for
// the panel (as the game's own menus take it), and while the panel has focus the game reads a centred stick.
void bluewake_ui_pad_filter_axes(int8_t* stick_x, int8_t* stick_y);
// Buttons to add to the guest's input for a number of samples (the resume at the title screen).
void bluewake_ui_pad_inject(uint16_t buttons, unsigned samples);
// Exactly these buttons, whatever is held, for `ms` milliseconds (the menu's Restart holds the game's reset pattern).
void bluewake_ui_pad_hold(uint16_t buttons, unsigned ms);

#ifdef __cplusplus
}
#endif

#endif
