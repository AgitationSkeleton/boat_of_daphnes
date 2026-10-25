// The Input Editor window and the controller rules around it. See input_editor.h.
extern "C" {
#include "game_options.h"
}
#include "input_editor.h"
#include "gba_window.h"
#include "ship_menu.h"
#include "ship/cvar.h"

#include <SDL3/SDL.h>
#include <dolphin/pad.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CVAR_SETTING(name) "gSettings." name

namespace input_editor {
namespace {

enum class Kind { None, Button, Axis, KeyButton, KeyAxis };

struct Capture {
    Kind kind = Kind::None;
    unsigned port = 0;
    unsigned index = 0;  // the pad button's bit (Button, KeyButton) or the axis (Axis, KeyAxis)
    Uint64 started = 0;
};
Capture g_capture;
int g_virtual_target = -1;  // testing: the port the virtual pad is put on
SDL_JoystickID g_virtual_id = 0;
SDL_Joystick* g_virtual_joystick = nullptr;
int g_virtual_press = -1;      // testing: the controller button the virtual pad holds
unsigned g_virtual_bind = 0;   // testing: the GameCube button bound as soon as the pad is on its port
bool g_menu_is_open = false;
Uint64 g_virtual_attached = 0;

const char* kPortNames[4] = {"Port 1", "Port 2", "Port 3", "Port 4"};

std::string port_cvar(unsigned port, const char* name) {
    return std::string(CVAR_SETTING("Controls.Port")) + std::to_string(port + 1) + "." + name;
}

bool keyboard_wanted(unsigned port) {
    return CVarGetInteger(port_cvar(port, "Keyboard").c_str(), port == 0 ? 1 : 0) != 0;
}

void save_mappings() {
    PADSerializeMappings();
    CVarSaveNextFrame();
}

// The keyboard's bindings of a port are only handed out while it is active.
template <typename F>
void with_keyboard(unsigned port, F&& f) {
    PADSetKeyboardActive(port, TRUE);
    f();
}

const char* key_name(s32 scancode) {
    switch (scancode) {
    case PAD_KEY_INVALID: return "None";
    case PAD_KEY_MOUSE_LEFT: return "Mouse Left";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse Middle";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse Right";
    case PAD_KEY_MOUSE_X1: return "Mouse X1";
    case PAD_KEY_MOUSE_X2: return "Mouse X2";
    default: break;
    }
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name != nullptr && name[0] != '\0' ? name : "?";
}

std::string native_axis_name(const PADAxisMapping& m) {
    if (m.nativeAxis.nativeAxis >= 0) {
        const char* name = PADGetNativeAxisName(m.nativeAxis);
        return name != nullptr ? name : "?";
    }
    if (m.nativeButton >= 0) {
        const char* name = PADGetNativeButtonName(static_cast<u32>(m.nativeButton));
        return name != nullptr ? name : "?";
    }
    return "None";
}

void begin_capture(Kind kind, unsigned port, unsigned index) {
    g_capture = {kind, port, index, SDL_GetTicks()};
}

bool capturing(Kind kind, unsigned port, unsigned index) {
    return g_capture.kind == kind && g_capture.port == port && g_capture.index == index;
}

// A binding's button: its current control, or a prompt while it is being set.
bool binding_button(const char* id, const std::string& text, bool waiting, float width) {
    const std::string label = (waiting ? std::string("Press a control...") : text) + "###" + id;
    return ImGui::Button(label.c_str(), ImVec2(width, 0));
}

void poll_capture() {
    if (g_capture.kind == Kind::None)
        return;
    if (SDL_GetTicks() - g_capture.started > 6000) {
        g_capture = {};
        return;
    }
    const unsigned port = g_capture.port;
    if (g_capture.kind == Kind::Button) {
        const s32 pressed = PADGetNativeButtonPressed(port);
        // L and R are the GameCube's triggers: on Switch-style pads (ZL/ZR) and others the big triggers are axes, not
        // buttons, so a pull while L or R is being bound binds that axis to the GameCube trigger (its digital click follows
        // the trigger's activation zone, as with the default mapping).
        if (pressed < 0 && (g_capture.index == PAD_TRIGGER_L || g_capture.index == PAD_TRIGGER_R)) {
            const PADSignedNativeAxis pulled = PADGetNativeAxisPulled(port);
            if (pulled.nativeAxis >= 0) {
                const PADAxis axis = g_capture.index == PAD_TRIGGER_L ? PAD_AXIS_TRIGGER_L : PAD_AXIS_TRIGGER_R;
                PADSetAxisMapping(port, PADAxisMapping{pulled, SDL_GAMEPAD_BUTTON_INVALID, axis});
                std::fprintf(stderr, "[input] port %u: %s bound to controller axis %s (trigger)\n", port + 1,
                             PADGetButtonName(static_cast<PADButton>(g_capture.index)), PADGetNativeAxisName(pulled));
                g_capture = {};
                save_mappings();
                return;
            }
        }
        if (pressed >= 0) {
            PADSetButtonMapping(port, PADButtonMapping{static_cast<u32>(pressed), static_cast<PADButton>(g_capture.index)});
            std::fprintf(stderr, "[input] port %u: %s bound to controller button %s\n", port + 1,
                         PADGetButtonName(static_cast<PADButton>(g_capture.index)), PADGetNativeButtonName(pressed));
            g_capture = {};
            save_mappings();
        }
    } else if (g_capture.kind == Kind::Axis) {
        const PADSignedNativeAxis pulled = PADGetNativeAxisPulled(port);
        if (pulled.nativeAxis >= 0) {
            PADSetAxisMapping(port, PADAxisMapping{pulled, SDL_GAMEPAD_BUTTON_INVALID, static_cast<PADAxis>(g_capture.index)});
            std::fprintf(stderr, "[input] port %u: %s bound to controller axis %s\n", port + 1,
                         PADGetAxisName(static_cast<PADAxis>(g_capture.index)), PADGetNativeAxisName(pulled));
            g_capture = {};
            save_mappings();
        } else if (const s32 button = PADGetNativeButtonPressed(port); button >= 0) {
            PADSetAxisMapping(port, PADAxisMapping{{-1, AXIS_SIGN_POSITIVE}, button, static_cast<PADAxis>(g_capture.index)});
            g_capture = {};
            save_mappings();
        }
    }
}

void controller_section(unsigned port, float scale) {
    // Which controller drives this port.
    const u32 count = PADCount();
    const s32 current = PADGetIndexForPort(port);
    std::string preview = "None";
    if (current >= 0)
        if (const char* name = PADGetNameForControllerIndex(static_cast<u32>(current)))
            preview = name;
    ImGui::SetNextItemWidth(320 * scale);
    if (ImGui::BeginCombo("Controller", preview.c_str())) {
        if (ImGui::Selectable("None", current < 0))
            PADClearPort(port);
        for (u32 i = 0; i < count; ++i) {
            const char* name = PADGetNameForControllerIndex(i);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(name != nullptr ? name : "Controller", static_cast<s32>(i) == current)) {
                PADSetPortForIndex(i, port);
                std::fprintf(stderr, "[input] controller %u (%s) put on port %u\n", i, name != nullptr ? name : "?", port + 1);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (current < 0) {
        ImGui::TextDisabled(count == 0 ? "No controller is connected." : "No controller on this port.");
        return;
    }
    const float wide = 220 * scale;
    if (ImGui::CollapsingHeader("Buttons", ImGuiTreeNodeFlags_DefaultOpen)) {
        u32 n = 0;
        PADButtonMapping* buttons = PADGetButtonMappings(port, &n);
        for (u32 i = 0; buttons != nullptr && i < n; ++i) {
            const PADButtonMapping m = buttons[i];
            const char* gc = PADGetButtonName(m.padButton);
            const char* native = m.nativeButton == PAD_NATIVE_BUTTON_INVALID ? "None" : PADGetNativeButtonName(m.nativeButton);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%-10s", gc != nullptr ? gc : "?");
            ImGui::SameLine(110 * scale);
            const std::string id = "btn" + std::to_string(port) + "_" + std::to_string(m.padButton);
            if (binding_button(id.c_str(), native != nullptr ? native : "?", capturing(Kind::Button, port, m.padButton), wide))
                begin_capture(Kind::Button, port, m.padButton);
        }
    }
    if (ImGui::CollapsingHeader("Sticks and Triggers", ImGuiTreeNodeFlags_DefaultOpen)) {
        u32 n = 0;
        PADAxisMapping* axes = PADGetAxisMappings(port, &n);
        for (u32 i = 0; axes != nullptr && i < n; ++i) {
            const PADAxisMapping m = axes[i];
            const char* gc = PADGetAxisDirectionLabel(m.padAxis);
            const char* axis = PADGetAxisName(m.padAxis);
            std::string label = axis != nullptr ? axis : "?";
            if (gc != nullptr)
                label += std::string(" ") + gc;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine(190 * scale);
            const std::string value = native_axis_name(m) + (m.nativeAxis.nativeAxis >= 0 ? (m.nativeAxis.sign < 0 ? " (-)" : " (+)") : "");
            const std::string id = "axis" + std::to_string(port) + "_" + std::to_string(m.padAxis);
            if (binding_button(id.c_str(), value, capturing(Kind::Axis, port, m.padAxis), wide))
                begin_capture(Kind::Axis, port, m.padAxis);
        }
    }
    if (ImGui::CollapsingHeader("Dead Zones")) {
        if (PADDeadZones* zones = PADGetDeadZones(port)) {
            auto percent = [&](const char* label, u16* value) {
                int p = static_cast<int>(*value * 100 / 32767);
                ImGui::SetNextItemWidth(260 * scale);
                if (ImGui::SliderInt(label, &p, 0, 50, "%d %%")) {
                    *value = static_cast<u16>(p * 32767 / 100);
                    save_mappings();
                }
            };
            bool use = zones->useDeadzones;
            if (ImGui::Checkbox("Use Dead Zones", &use)) {
                zones->useDeadzones = use;
                save_mappings();
            }
            percent("Left Stick", &zones->stickDeadZone);
            percent("Right Stick", &zones->substickDeadZone);
            percent("Left Trigger Activation", &zones->leftTriggerActivationZone);
            percent("Right Trigger Activation", &zones->rightTriggerActivationZone);
        }
    }
    if (ImGui::Button("Reset Controller to Defaults", ImVec2(wide, 0))) {
        PADRestoreDefaultMapping(port);
        save_mappings();
        ApplyCamera();
    }
}

void keyboard_section(unsigned port, float scale) {
    bool wanted = keyboard_wanted(port);
    if (ImGui::Checkbox("Use Keyboard", &wanted)) {
        CVarSetInteger(port_cvar(port, "Keyboard").c_str(), wanted ? 1 : 0);
        CVarSaveNextFrame();
        ApplyKeyboard(g_menu_is_open);
    }
    if (!wanted)
        return;
    const float wide = 220 * scale;
    if (ImGui::CollapsingHeader("Keyboard Buttons")) {
        with_keyboard(port, [&] {
            u32 n = 0;
            PADKeyButtonBinding* keys = PADGetKeyButtonBindings(port, &n);
            for (u32 i = 0; keys != nullptr && i < n; ++i) {
                const PADKeyButtonBinding b = keys[i];
                const char* gc = PADGetButtonName(b.padButton);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%-10s", gc != nullptr ? gc : "?");
                ImGui::SameLine(110 * scale);
                const std::string id = "key" + std::to_string(port) + "_" + std::to_string(b.padButton);
                if (binding_button(id.c_str(), key_name(b.scancode), capturing(Kind::KeyButton, port, b.padButton), wide))
                    begin_capture(Kind::KeyButton, port, b.padButton);
            }
        });
    }
    if (ImGui::CollapsingHeader("Keyboard Sticks and Triggers")) {
        with_keyboard(port, [&] {
            u32 n = 0;
            PADKeyAxisBinding* keys = PADGetKeyAxisBindings(port, &n);
            for (u32 i = 0; keys != nullptr && i < n; ++i) {
                const PADKeyAxisBinding b = keys[i];
                const char* axis = PADGetAxisName(b.padAxis);
                const char* dir = PADGetAxisDirectionLabel(b.padAxis);
                std::string label = axis != nullptr ? axis : "?";
                if (dir != nullptr)
                    label += std::string(" ") + dir;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label.c_str());
                ImGui::SameLine(190 * scale);
                const std::string id = "kaxis" + std::to_string(port) + "_" + std::to_string(b.padAxis);
                if (binding_button(id.c_str(), key_name(b.scancode), capturing(Kind::KeyAxis, port, b.padAxis), wide))
                    begin_capture(Kind::KeyAxis, port, b.padAxis);
            }
        });
    }
    if (ImGui::Button("Reset Keyboard to Defaults", ImVec2(wide, 0))) {
        PADClearKeyBindings(port);
        PADSetKeyboardActive(port, TRUE);
        save_mappings();
    }
}

void embed_window(float scale) {
    ImGui::TextWrapped("Pick the controller for each port, then click a binding and press the control to use. "
                       "Choosing a controller that is on another port moves it.");
    if (ImGui::BeginTabBar("##ports")) {
        for (unsigned port = 0; port < 4; ++port) {
            if (ImGui::BeginTabItem(kPortNames[port])) {
                ImGui::PushID(static_cast<int>(port));
                controller_section(port, scale);
                ImGui::Separator();
                keyboard_section(port, scale);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    if (g_capture.kind != Kind::None) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Waiting for a control... (Esc cancels)");
    }
}

void draw_window(float scale, float fontScale) {
    shipmenu::PopoutWindow("Controller Bindings", "gOpenWindows.ControllerBindings", ImVec2(560 * scale, 620 * scale), 0.97f, fontScale,
                           [&] { embed_window(scale); }, [] { g_capture = {}; });
}

// The camera axes' signs, as the CVars ask.
bool camera_inverted_x(unsigned port, bool* known) {
    u32 n = 0;
    PADAxisMapping* axes = PADGetAxisMappings(port, &n);
    *known = false;
    for (u32 i = 0; axes != nullptr && i < n; ++i)
        if (axes[i].padAxis == PAD_AXIS_RIGHT_X_POS && axes[i].nativeAxis.nativeAxis >= 0) {
            *known = true;
            return axes[i].nativeAxis.sign < 0;
        }
    return false;
}

bool camera_inverted_y(unsigned port, bool* known) {
    u32 n = 0;
    PADAxisMapping* axes = PADGetAxisMappings(port, &n);
    *known = false;
    for (u32 i = 0; axes != nullptr && i < n; ++i)
        if (axes[i].padAxis == PAD_AXIS_RIGHT_Y_POS && axes[i].nativeAxis.nativeAxis >= 0) {
            *known = true;
            return axes[i].nativeAxis.sign > 0;  // SDL's up is negative: the default sign is
        }
    return false;
}

// Flip the signs of the C-stick's two directions on one axis.
void flip_camera(unsigned port, bool x) {
    u32 n = 0;
    PADAxisMapping* axes = PADGetAxisMappings(port, &n);
    std::vector<PADAxisMapping> changed;
    for (u32 i = 0; axes != nullptr && i < n; ++i) {
        const PADAxis a = axes[i].padAxis;
        const bool mine = x ? (a == PAD_AXIS_RIGHT_X_POS || a == PAD_AXIS_RIGHT_X_NEG)
                            : (a == PAD_AXIS_RIGHT_Y_POS || a == PAD_AXIS_RIGHT_Y_NEG);
        if (mine && axes[i].nativeAxis.nativeAxis >= 0) {
            PADAxisMapping m = axes[i];
            m.nativeAxis.sign = m.nativeAxis.sign == AXIS_SIGN_POSITIVE ? AXIS_SIGN_NEGATIVE : AXIS_SIGN_POSITIVE;
            changed.push_back(m);
        }
    }
    for (const PADAxisMapping& m : changed)
        PADSetAxisMapping(port, m);
}

void apply_camera_to(unsigned port, bool log) {
    if (PADGetIndexForPort(port) < 0)
        return;
    const bool want_x_setting = CVarGetInteger(CVAR_SETTING("Controls.InvertCameraX"), 0) != 0;
    // With the module's camera hook (option invert_camera_x, dCamera_c::updatePad) only the camera turns the other way;
    // the C-stick itself stays as it is, so the Wind Waker's conducting and everything else read it normally. A module
    // without the hook falls back to inverting the stick.
    const bool camera_hook = bluewake_game_options_has("invert_camera_x");
    if (camera_hook)
        bluewake_game_options_set("invert_camera_x", want_x_setting);
    const bool want_x = camera_hook ? false : want_x_setting;
    const bool want_y = CVarGetInteger(CVAR_SETTING("Controls.InvertCameraY"), 0) != 0;
    bool known = false;
    bool changed = false;
    if (camera_inverted_x(port, &known) != want_x && known) {
        flip_camera(port, true);
        changed = true;
    }
    if (camera_inverted_y(port, &known) != want_y && known) {
        flip_camera(port, false);
        changed = true;
    }
    if (changed) {
        if (log)
            std::fprintf(stderr, "[input] port %u camera: invert x=%d y=%d\n", port + 1, want_x, want_y);
        PADSerializeMappings();
    }
}

}  // namespace

void ApplyCamera() {
    for (unsigned port = 0; port < 4; ++port)
        apply_camera_to(port, true);
}

void ApplyPorts() {
    if (!CVarGetInteger(CVAR_SETTING("Controls.FirstConnectedIsPlayer1"), 1))
        return;
    if (PADGetIndexForPort(0) >= 0)
        return;
    for (unsigned port = 1; port < 4; ++port) {
        if (port == 1 && CVarGetInteger(CVAR_SETTING("TingleTuner.GbaPad"), 0) != 0)
            continue;  // the pad chosen for the Tingle Tuner's GBA stays on port 2
        const s32 index = PADGetIndexForPort(port);
        if (index >= 0) {
            PADSetPortForIndex(static_cast<u32>(index), 0);
            std::fprintf(stderr, "[input] first connected controller (port %u) is now player 1\n", port + 1);
            return;
        }
    }
}

void ApplyKeyboard(bool menuOpen) {
    g_menu_is_open = menuOpen;
    for (unsigned port = 0; port < 4; ++port) {
        const bool on = keyboard_wanted(port) && !(port == 0 && menuOpen);
        PADSetKeyboardActive(port, on ? TRUE : FALSE);
    }
}

void TestGbaPad();

void Poll() {
    TestGbaPad();
    static SDL_JoystickID seen[4];
    static bool first = true;
    bool changed = first;
    first = false;
    for (unsigned port = 0; port < 4; ++port) {
        SDL_JoystickID id = 0;
        const s32 index = PADGetIndexForPort(port);
        if (index >= 0)
            if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index)))
                id = SDL_GetGamepadID(pad);
        if (id != seen[port]) {
            seen[port] = id;
            changed = true;
            if (id != 0)
                apply_camera_to(port, false);
        }
    }
    if (changed)
        ApplyPorts();
    static bool placed;
    // Testing: a virtual pad put on a port (the adapter's third port, say).
    if (g_virtual_target >= 0 && g_virtual_id != 0 && PADCount() > 0 && g_virtual_attached != 0) {
        if (!placed) {
            placed = true;
            for (unsigned port = 0; port < 4; ++port)
                if (PADGetIndexForPort(port) >= 0)
                    PADClearPort(port);
            PADSetPortForIndex(0, static_cast<unsigned>(g_virtual_target));
            std::fprintf(stderr, "[test] virtual pad put on port %d (PADCount=%u)\n", g_virtual_target + 1, PADCount());
            if (g_virtual_press >= 0) {
                g_virtual_joystick = SDL_OpenJoystick(g_virtual_id);
                if (g_virtual_joystick != nullptr)
                    SDL_SetJoystickVirtualButton(g_virtual_joystick, g_virtual_press, true);
            }
        }
        // Once it is on a port (the port rule may move it): bind a button with it.
        static bool bound;
        if (placed && g_virtual_bind != 0 && !bound && g_capture.kind == Kind::None)
            for (unsigned port = 0; port < 4; ++port)
                if (PADGetIndexForPort(port) >= 0 && SDL_GetTicks() - g_virtual_attached > 3000) {
                    bound = true;
                    begin_capture(Kind::Button, port, g_virtual_bind);
                    break;
                }
    }
}

bool OnEvent(const SDL_Event* event) {
    if (g_capture.kind != Kind::KeyButton && g_capture.kind != Kind::KeyAxis)
        return false;
    if (event->type != SDL_EVENT_KEY_DOWN)
        return false;
    if (event->key.scancode == SDL_SCANCODE_ESCAPE) {
        g_capture = {};
        return true;
    }
    const s32 scancode = static_cast<s32>(event->key.scancode);
    with_keyboard(g_capture.port, [&] {
        if (g_capture.kind == Kind::KeyButton)
            PADSetKeyButtonBinding(g_capture.port, PADKeyButtonBinding{scancode, static_cast<PADButton>(g_capture.index)});
        else {
            u32 n = 0;
            PADKeyAxisBinding* keys = PADGetKeyAxisBindings(g_capture.port, &n);
            for (u32 i = 0; keys != nullptr && i < n; ++i)
                if (keys[i].padAxis == g_capture.index) {
                    PADKeyAxisBinding b = keys[i];
                    b.scancode = scancode;
                    PADSetKeyAxisBinding(g_capture.port, b);
                }
        }
    });
    std::fprintf(stderr, "[input] port %u: key %s bound\n", g_capture.port + 1, key_name(scancode));
    g_capture = {};
    save_mappings();
    return true;
}

void Frame(float scale, float fontScale) {
    Poll();
    poll_capture();
    if (CVarGetInteger("gOpenWindows.ControllerBindings", 0))
        draw_window(scale, fontScale);
}

void DrawEmbedded(float scale) { embed_window(scale); }

namespace {
const char* nso_label(SDL_Gamepad* pad) {
    if (pad == nullptr || SDL_GetGamepadVendor(pad) != 0x057E)
        return nullptr;
    switch (SDL_GetGamepadProduct(pad)) {
    case 0x2017: return "SNES";
    case 0x2019: return "N64";
    case 0x201E: return "Genesis";
    default: return nullptr;
    }
}

constexpr unsigned kGbaPort = 1;

void select_gba_controller(int choice) {  // -2 Keyboard, -1 None, otherwise a controller index
    if (choice < 0) {
        PADClearPort(kGbaPort);
        CVarSetInteger(CVAR_SETTING("TingleTuner.GbaPad"), 0);
        if (choice == -2) {
            gba_window::UseDefaultKeys();
        } else {
            CVarSetInteger(port_cvar(kGbaPort, "Keyboard").c_str(), 0);
            std::fprintf(stderr, "[gba] controller: none\n");
        }
        ApplyKeyboard(g_menu_is_open);
        CVarSaveNextFrame();
        return;
    }
    const u32 index = static_cast<u32>(choice);
    const char* name = PADGetNameForControllerIndex(index);
    PADSetPortForIndex(index, kGbaPort);
    CVarSetInteger(CVAR_SETTING("TingleTuner.GbaPad"), 1);
    CVarSetInteger(port_cvar(kGbaPort, "Keyboard").c_str(), 0);
    ApplyKeyboard(g_menu_is_open);
    u32 vid = 0, pid = 0;
    PADGetVidPid(kGbaPort, &vid, &pid);
    std::fprintf(stderr, "[gba] controller: '%s' (%04x:%04x, index %u) bound to the in-app GBA input (port 2)\n",
                 name != nullptr ? name : "?", vid, pid, index);
    save_mappings();
}
}  // namespace

void DrawGbaController(float scale) {
    const u32 count = PADCount();
    const s32 current = PADGetIndexForPort(kGbaPort);
    std::string preview = keyboard_wanted(kGbaPort) ? "Keyboard" : "None";
    if (current >= 0)
        if (const char* name = PADGetNameForControllerIndex(static_cast<u32>(current)))
            preview = name;
    ImGui::SetNextItemWidth(320 * scale);
    if (ImGui::BeginCombo("GBA Controller", preview.c_str())) {
        if (ImGui::Selectable("Keyboard", current < 0 && keyboard_wanted(kGbaPort)))
            select_gba_controller(-2);
        if (ImGui::Selectable("None", current < 0 && !keyboard_wanted(kGbaPort)))
            select_gba_controller(-1);
        for (u32 i = 0; i < count; ++i) {
            const char* name = PADGetNameForControllerIndex(i);
            const char* nso = nso_label(PADGetSDLGamepadForIndex(i));
            std::string label = name != nullptr ? name : "Controller";
            if (nso != nullptr)
                label += std::string("  (Nintendo Switch Online ") + nso + ", recommended)";
            if (PADGetIndexForPort(0) == static_cast<s32>(i))
                label += "  (now player 1)";
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), static_cast<s32>(i) == current))
                select_gba_controller(static_cast<int>(i));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    bool any_nso = false, any_xinput = false;
    for (u32 i = 0; i < count; ++i) {
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(i);
        any_nso = any_nso || nso_label(pad) != nullptr;
        any_xinput = any_xinput || (pad != nullptr && SDL_GetGamepadVendor(pad) == 0x045E && SDL_GetGamepadProduct(pad) == 0x028E);
    }
    if (!any_nso && any_xinput)
        ImGui::TextDisabled("A Nintendo Switch Online pad that shows up only as \"Xbox 360 Controller\" is being held by Steam Input.\n"
                            "Turn off Steam Input for Switch controllers (Steam > Settings > Controller) or close Steam, and it is read natively.");
    if (current < 0)
        return;
    ImGui::TextDisabled("Click a GBA button, then press the control to use.");
    struct Row {
        const char* label;
        PADButton button;
    };
    static const Row rows[] = {{"A", PAD_BUTTON_A},          {"B", PAD_BUTTON_B},
                               {"L", PAD_TRIGGER_L},         {"R", PAD_TRIGGER_R},
                               {"Start", PAD_BUTTON_START},  {"Select", PAD_TRIGGER_Z},
                               {"D-pad Up", PAD_BUTTON_UP},  {"D-pad Down", PAD_BUTTON_DOWN},
                               {"D-pad Left", PAD_BUTTON_LEFT}, {"D-pad Right", PAD_BUTTON_RIGHT}};
    u32 n = 0;
    PADButtonMapping* buttons = PADGetButtonMappings(kGbaPort, &n);
    for (const Row& row : rows) {
        const char* native = "None";
        for (u32 i = 0; buttons != nullptr && i < n; ++i)
            if (buttons[i].padButton == row.button && buttons[i].nativeButton != PAD_NATIVE_BUTTON_INVALID)
                native = PADGetNativeButtonName(buttons[i].nativeButton);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%-10s", row.label);
        ImGui::SameLine(110 * scale);
        const std::string id = std::string("gba_") + row.label;
        if (binding_button(id.c_str(), native != nullptr ? native : "?", capturing(Kind::Button, kGbaPort, row.button),
                           220 * scale))
            begin_capture(Kind::Button, kGbaPort, row.button);
    }
    if (ImGui::Button("Reset GBA Buttons to Defaults", ImVec2(220 * scale, 0))) {
        PADRestoreDefaultMapping(kGbaPort);
        save_mappings();
    }
}

// Testing: BLUEWAKE_TEST_GBA_PAD=1 attaches a virtual pad that looks like the NSO SNES controller (057E:2017), picks it in
// the Tingle Tuner's GBA controller list and, after a moment, holds its A button (SDL's EAST) so the log shows the
// GBA's keys ("[gba] keys", with BLUEWAKE_GBA_KEYS_LOG=1).
static SDL_JoystickID g_snes_id = 0;
static SDL_Joystick* g_snes_joy = nullptr;
static Uint64 g_snes_attached = 0;
static bool g_snes_picked = false;
static bool g_snes_pressed = false;

void TestGbaPad() {
    if (g_snes_id == 0 || g_snes_attached == 0)
        return;
    const Uint64 age = SDL_GetTicks() - g_snes_attached;
    if (!g_snes_picked && age > 2500) {
        for (u32 i = 0; i < PADCount(); ++i) {
            SDL_Gamepad* pad = PADGetSDLGamepadForIndex(i);
            if (pad != nullptr && SDL_GetGamepadID(pad) == g_snes_id) {
                g_snes_picked = true;
                std::fprintf(stderr, "[test] GBA controller list offers '%s' %04x:%04x\n", PADGetNameForControllerIndex(i),
                             SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad));
                select_gba_controller(static_cast<int>(i));
                g_snes_joy = SDL_OpenJoystick(g_snes_id);
                break;
            }
        }
    }
    if (g_snes_picked && !g_snes_pressed && age > 15000 && g_snes_joy != nullptr) {
        g_snes_pressed = true;
        SDL_SetJoystickVirtualButton(g_snes_joy, SDL_GAMEPAD_BUTTON_EAST, true);
        std::fprintf(stderr, "[test] virtual SNES A held\n");
    }
}

void TestSetup() {
    if (const char* gba = std::getenv("BLUEWAKE_TEST_GBA_PAD"); gba != nullptr && gba[0] == '1') {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x057E;
        desc.product_id = 0x2017;
        desc.naxes = 0;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "SNES Controller (test)";
        g_snes_id = SDL_AttachVirtualJoystick(&desc);
        g_snes_attached = SDL_GetTicks();
        std::fprintf(stderr, "[test] virtual SNES pad %s (id %u)\n", g_snes_id != 0 ? "attached" : "FAILED", g_snes_id);
    }
    const char* spec = std::getenv("BLUEWAKE_TEST_VIRTUAL_PAD");
    if (spec == nullptr || spec[0] == '\0')
        return;
    g_virtual_target = std::atoi(spec);
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x1234;
    desc.product_id = 0x5678;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Test Pad";
    g_virtual_id = SDL_AttachVirtualJoystick(&desc);
    g_virtual_attached = SDL_GetTicks();
    std::fprintf(stderr, "[test] virtual pad %s (id %u)\n", g_virtual_id != 0 ? "attached" : "FAILED", g_virtual_id);
    if (const char* comma = std::strchr(spec, ','))
        g_virtual_press = std::atoi(comma + 1);
    if (const char* bind = std::getenv("BLUEWAKE_TEST_BIND"))
        g_virtual_bind = static_cast<unsigned>(std::strtoul(bind, nullptr, 0));
}

const char* Summary() {
    static char text[128];
    unsigned ports = 0;
    for (unsigned port = 0; port < 4; ++port)
        ports += PADGetIndexForPort(port) >= 0 ? 1u : 0u;
    std::snprintf(text, sizeof text, "%u controller%s connected, %u on a port.", PADCount(), PADCount() == 1 ? "" : "s", ports);
    return text;
}

}  // namespace input_editor
