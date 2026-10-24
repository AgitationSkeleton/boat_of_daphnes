// Menu Theme > Classic: Boat of Daphnes' own menu (AgitationSkeleton/boat-of-daphnes windows/src/ship_menu.cpp), as it
// was, beside the Wind Waker menu (ship_menu.cpp). See ship_menu.h. The layout follows Ship of Harkinian's port menu: a dark
// overlay over the whole window; a centred block (90 percent of the window
// when it is large, never wider than 16:9); headers and a search box on top,
// Quit, Restart and Close at the right; a white rule; the sidebar; a white
// rule; the page's columns.
#include "ship_menu.h"

#include "ship/cvar.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace shipmenu {

namespace {

struct ThemeEntry {
    const char* name;
    ImVec4 color;
};

const ThemeEntry kThemes[] = {
    {"Red", ImVec4(0.55f, 0.0f, 0.0f, 1.0f)},        {"Dark Red", ImVec4(0.3f, 0.0f, 0.0f, 1.0f)},
    {"Orange", ImVec4(0.85f, 0.55f, 0.0f, 1.0f)},    {"Green", ImVec4(0.0f, 0.55f, 0.0f, 1.0f)},
    {"Dark Green", ImVec4(0.0f, 0.3f, 0.0f, 1.0f)},  {"Light Blue", ImVec4(0.0f, 0.24f, 0.8f, 1.0f)},
    {"Blue", ImVec4(0.08f, 0.03f, 0.65f, 1.0f)},     {"Dark Blue", ImVec4(0.03f, 0.0f, 0.5f, 1.0f)},
    {"Indigo", ImVec4(0.35f, 0.0f, 0.87f, 1.0f)},    {"Violet", ImVec4(0.5f, 0.0f, 0.9f, 1.0f)},
    {"Purple", ImVec4(0.31f, 0.0f, 0.67f, 1.0f)},    {"Brown", ImVec4(0.37f, 0.18f, 0.0f, 1.0f)},
    {"Gray", ImVec4(0.45f, 0.45f, 0.45f, 1.0f)},     {"Dark Gray", ImVec4(0.15f, 0.15f, 0.15f, 1.0f)},
};
constexpr int kDefaultTheme = 5; // Light Blue

const ImVec4 kRed(0.55f, 0.0f, 0.0f, 1.0f);

ImVec4 accent() { return theme_color(CVarGetInteger("gSettings.Menu.Theme", kDefaultTheme)); }

ImVec4 with_alpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

void push_button_style(const ImVec4& c) {
    ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(c, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(c, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, with_alpha(c, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
}

void pop_button_style() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

void push_frame_style(const ImVec4& c) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, with_alpha(c, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, with_alpha(c, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, with_alpha(c, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1, 1, 1, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1, 1, 1, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1, 1, 1, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Header, with_alpha(c, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, with_alpha(c, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, with_alpha(c, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.12f, 0.12f, 0.12f, 0.98f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 3.0f);
}

void pop_frame_style() {
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(10);
}

std::string lower_no_spaces(const std::string& s) {
    std::string out;
    for (const char c : s)
        if (c != ' ')
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A clickable label centred in the sidebar; filled when chosen or hovered.
bool sidebar_entry(const std::string& label, bool chosen) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 size = ImGui::CalcTextSize(label.c_str());
    ImVec2 pos = window->DC.CursorPos;
    pos.y += style.FramePadding.y;
    pos.x = window->WorkRect.GetCenter().x - size.x * 0.5f;
    const ImRect bb(ImVec2(pos.x - style.FramePadding.x, pos.y - style.FramePadding.y),
                    ImVec2(pos.x + size.x + style.FramePadding.x, pos.y + size.y + style.FramePadding.y));
    const ImGuiID id = window->GetID((label + "##Sidebar").c_str());
    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id))
        return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const ImVec4 c = accent();
    if (chosen || hovered)
        window->DrawList->AddRectFilled(bb.Min, bb.Max,
                                        ImGui::GetColorU32(with_alpha(c, chosen ? 1.0f : held ? 0.6f : 0.8f)), 3.0f);
    window->DrawList->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    return pressed;
}

void tooltip(const std::string& text, float scale) {
    if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        return;
    ImGui::BeginTooltip();
    (void)scale;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace

extern float g_menu_scale;  // ship_menu.cpp's

void classic_push_theme() {
    push_frame_style(accent());
    push_button_style(accent());
}

void classic_pop_theme() {
    pop_button_style();
    pop_frame_style();
}

void Menu::DrawWidgetClassic(WidgetInfo& w, float width) {
    w.disabledReasons.clear();
    w.hidden = false;
    if (w.preFunc)
        w.preFunc(w);
    if (w.hidden)
        return;
    if (w.sameLine)
        ImGui::SameLine();
    const bool disabled = !w.disabledReasons.empty();
    std::string tip = w.tooltip;
    if (disabled) {
        tip = "This setting is disabled because:";
        for (const auto& reason : w.disabledReasons)
            tip += "\n- " + reason;
    }
    const ImVec4 c = accent();
    ImGui::PushID(w.name.c_str());
    ImGui::BeginDisabled(disabled);
    bool changed = false;
    switch (w.type) {
    case WidgetType::CVarCheckbox: {
        bool value = CVarGetInteger(w.cvar.c_str(), w.defaultInt) != 0;
        push_frame_style(c);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, with_alpha(c, 1.0f));
        if (ImGui::Checkbox(w.name.c_str(), &value)) {
            CVarSetInteger(w.cvar.c_str(), value ? 1 : 0);
            changed = true;
        }
        ImGui::PopStyleColor();
        pop_frame_style();
        tooltip(tip, mScale);
        break;
    }
    case WidgetType::CVarInputText: {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(w.name.c_str());
        tooltip(tip, mScale);
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, "%s", CVarGetString(w.cvar.c_str(), w.defaultText.c_str()));
        push_frame_style(c);
        ImGui::SetNextItemWidth(width);
        const int limit = std::clamp(w.maxInt > 0 ? w.maxInt : 255, 1, 255);
        if (ImGui::InputText("##text", buffer, static_cast<size_t>(limit) + 1u)) {
            CVarSetString(w.cvar.c_str(), buffer);
            changed = true;
        }
        pop_frame_style();
        break;
    }
    case WidgetType::CVarColorPicker: {
        const CVarColor d{static_cast<uint8_t>(w.defaultColor.x * 255.f), static_cast<uint8_t>(w.defaultColor.y * 255.f),
                          static_cast<uint8_t>(w.defaultColor.z * 255.f),
                          static_cast<uint8_t>(w.useAlpha ? w.defaultColor.w * 255.f : 255.f)};
        const CVarColor v = CVarGetColor(w.cvar.c_str(), d);
        float rgba[4] = {v.r / 255.f, v.g / 255.f, v.b / 255.f, w.useAlpha ? v.a / 255.f : 1.f};
        push_frame_style(c);
        const bool edited = w.useAlpha ? ImGui::ColorEdit4(w.name.c_str(), rgba,
                                                           ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)
                                       : ImGui::ColorEdit3(w.name.c_str(), rgba, ImGuiColorEditFlags_NoInputs);
        if (edited) {
            CVarSetColor(w.cvar.c_str(), CVarColor{static_cast<uint8_t>(std::lround(rgba[0] * 255.f)),
                                                   static_cast<uint8_t>(std::lround(rgba[1] * 255.f)),
                                                   static_cast<uint8_t>(std::lround(rgba[2] * 255.f)),
                                                   static_cast<uint8_t>(w.useAlpha ? std::lround(rgba[3] * 255.f) : 255)});
            changed = true;
        }
        pop_frame_style();
        tooltip(tip, mScale);
        break;
    }
    case WidgetType::CVarCombobox: {
        int value = CVarGetInteger(w.cvar.c_str(), w.defaultInt);
        value = std::clamp(value, 0, std::max(0, static_cast<int>(w.items.size()) - 1));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(w.name.c_str());
        tooltip(tip, mScale);
        push_frame_style(c);
        ImGui::SetNextItemWidth(width);
        const char* preview = w.items.empty() ? "" : w.items[value].c_str();
        if (ImGui::BeginCombo("##combo", preview)) {
            for (int i = 0; i < static_cast<int>(w.items.size()); ++i) {
                if (ImGui::Selectable(w.items[i].c_str(), i == value)) {
                    CVarSetInteger(w.cvar.c_str(), i);
                    changed = true;
                }
                if (i == value)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        pop_frame_style();
        tooltip(tip, mScale);
        break;
    }
    case WidgetType::CVarSliderInt:
    case WidgetType::CVarSliderFloat: {
        const bool isInt = w.type == WidgetType::CVarSliderInt;
        int iv = CVarGetInteger(w.cvar.c_str(), w.defaultInt);
        float fv = CVarGetFloat(w.cvar.c_str(), w.defaultFloat);
        char label[256];
        if (isInt)
            std::snprintf(label, sizeof label, w.name.c_str(), iv);
        else
            std::snprintf(label, sizeof label, w.name.c_str(), w.percent ? fv * 100.0f : fv);
        ImGui::TextUnformatted(label);
        tooltip(tip, mScale);
        push_button_style(c);
        const float button = ImGui::GetFrameHeight();
        if (ImGui::Button("-", ImVec2(button, button))) {
            if (isInt)
                iv = std::max(w.minInt, iv - w.stepInt);
            else
                fv = std::max(w.minFloat, fv - w.stepFloat);
            changed = true;
        }
        pop_button_style();
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        push_frame_style(c);
        ImGui::SetNextItemWidth(std::max(10.0f, width - 2.0f * (button + ImGui::GetStyle().ItemInnerSpacing.x)));
        if (isInt)
            changed |= ImGui::SliderInt("##slider", &iv, w.minInt, w.maxInt, "", ImGuiSliderFlags_AlwaysClamp);
        else
            changed |= ImGui::SliderFloat("##slider", &fv, w.minFloat, w.maxFloat, "", ImGuiSliderFlags_AlwaysClamp);
        pop_frame_style();
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        push_button_style(c);
        if (ImGui::Button("+", ImVec2(button, button))) {
            if (isInt)
                iv = std::min(w.maxInt, iv + w.stepInt);
            else
                fv = std::min(w.maxFloat, fv + w.stepFloat);
            changed = true;
        }
        pop_button_style();
        if (changed) {
            if (isInt) {
                if (w.stepInt > 1)
                    iv = w.minInt + (iv - w.minInt + w.stepInt / 2) / w.stepInt * w.stepInt;
                CVarSetInteger(w.cvar.c_str(), std::clamp(iv, w.minInt, w.maxInt));
            } else {
                fv = std::round(fv / w.stepFloat) * w.stepFloat;
                CVarSetFloat(w.cvar.c_str(), std::clamp(fv, w.minFloat, w.maxFloat));
            }
        }
        break;
    }
    case WidgetType::Button:
    case WidgetType::WindowButton: {
        bool open = w.type == WidgetType::WindowButton && CVarGetInteger(w.cvar.c_str(), 0) != 0;
        push_button_style(open ? with_alpha(c, 0.6f) : c);
        if (ImGui::Button(w.name.c_str(), ImVec2(w.sameLine ? 0.0f : width, 0.0f))) {
            if (w.type == WidgetType::WindowButton)
                CVarSetInteger(w.cvar.c_str(), open ? 0 : 1);
            changed = true;
        }
        pop_button_style();
        tooltip(tip, mScale);
        break;
    }
    case WidgetType::Window: {
        // As in Ship of Harkinian's menu: the tool is drawn in the page; Popout detaches it into its own window.
        const bool out = CVarGetInteger(w.cvar.c_str(), 0) != 0;
        push_button_style(out ? with_alpha(c, 0.6f) : c);
        if (ImGui::Button(out ? "Dock Back Into Menu" : "Popout", ImVec2(160.0f * mScale, 0.0f))) {
            CVarSetInteger(w.cvar.c_str(), out ? 0 : 1);
            CVarSaveNextFrame();
            changed = true;
        }
        pop_button_style();
        tooltip(out ? "Brings the window back into the menu." : tip, mScale);
        ImGui::Separator();
        if (out) {
            ImGui::TextDisabled("%s is open in its own window. Closing that window brings it back here.", w.name.c_str());
        } else if (w.custom) {
            ImGui::BeginChild("##window", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, 360.0f * mScale)),
                              ImGuiChildFlags_None);
            w.custom(w);
            ImGui::EndChild();
        }
        break;
    }
    case WidgetType::Text:
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        if (w.color.w > 0.0f)
            ImGui::TextColored(w.color, "%s", w.name.c_str());
        else
            ImGui::TextUnformatted(w.name.c_str());
        ImGui::PopTextWrapPos();
        tooltip(tip, mScale);
        break;
    case WidgetType::SeparatorText:
        ImGui::Spacing();
        if (w.color.w > 0.0f)
            ImGui::PushStyleColor(ImGuiCol_Text, w.color);
        ImGui::SeparatorText(w.name.c_str());
        if (w.color.w > 0.0f)
            ImGui::PopStyleColor();
        break;
    case WidgetType::Separator:
        ImGui::Separator();
        break;
    case WidgetType::Custom:
        if (w.custom)
            w.custom(w);
        break;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    if (changed) {
        CVarSaveNextFrame();
        if (w.callback)
            w.callback(w);
    }
}

void Menu::DrawSearchResultsClassic(const std::string& query, float width) {
    const std::string q = lower_no_spaces(query);
    int found = 0;
    const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    for (int column = 0; column < 2; ++column) {
        if (column == 1)
            ImGui::SameLine();
        ImGui::BeginChild(column == 0 ? "##results0" : "##results1", ImVec2(half, 0), ImGuiChildFlags_AutoResizeY);
        int index = 0;
        for (const auto& headerName : mOrder) {
            Header& h = mHeaders[headerName];
            for (const auto& sidebarName : h.order) {
                for (auto& col : h.sidebars[sidebarName].widgets) {
                    for (auto& w : col) {
                        if (w.hideInSearch || w.type == WidgetType::Separator ||
                            w.type == WidgetType::SeparatorText || w.type == WidgetType::Text ||
                            w.type == WidgetType::Window || w.type == WidgetType::Custom)
                            continue;
                        const std::string hay = lower_no_spaces(w.name + w.tooltip);
                        if (hay.find(q) == std::string::npos)
                            continue;
                        if (index++ % 2 != column)
                            continue;
                        ++found;
                        ImGui::TextDisabled("%s > %s", headerName.c_str(), sidebarName.c_str());
                        const bool same = w.sameLine;
                        w.sameLine = false;
                        DrawWidgetClassic(w, half - ImGui::GetStyle().ItemSpacing.x);
                        w.sameLine = same;
                        ImGui::Spacing();
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    if (found == 0)
        ImGui::TextColored(ImVec4(1, 1, 1, 0.4f), "No results found");
}

void classic_popout_window(const char* title, const char* cvar, ImVec2 size, float background, float fontScale,
                  const std::function<void()>& body, const std::function<void()>& onClose) {
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    bool open = true;
    classic_push_theme();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, background));
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetWindowFontScale(fontScale);
        body();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    classic_pop_theme();
    if (!open) {
        CVarSetInteger(cvar, 0);
        CVarSaveNextFrame();
        if (onClose)
            onClose();
    }
}

void Menu::DrawClassic(bool* open, float scale, float fontScale) {
    mScale = scale;
    g_menu_scale = scale;
    ImGuiIO& io = ImGui::GetIO();
    const float savedFontScale = io.FontGlobalScale;
    io.FontGlobalScale = fontScale;
    if (CVarGetInteger("gSettings.Menu.ControllerNavigation", 0))
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const bool popout = CVarGetInteger("gSettings.Menu.Popout", 0) != 0;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
    if (popout) {
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.75f, vp->WorkSize.y * 0.75f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        flags |= ImGuiWindowFlags_NoScrollbar;
    } else {
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        // The overlay stays behind the windows it opens (the Cosmetics Editor,
        // Controller Bindings), as SoH's does.
        flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0, 0, 0, CVarGetFloat("gSettings.Menu.BackgroundOpacity", 0.85f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool visible = ImGui::Begin("Main Menu", popout ? open : nullptr, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!visible) {
        ImGui::End();
        io.FontGlobalScale = savedFontScale;
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float windowW = window->WorkRect.GetWidth();
    const float windowH = window->WorkRect.GetHeight();

    // The block: the whole window when small; 90 percent, at most 16:9, when large.
    ImVec2 block(windowW, windowH);
    if (windowW > 1280.0f * scale)
        block.x = std::min(windowW * 0.9f, windowH * 1.77f);
    if (windowH > 800.0f * scale)
        block.y = windowH * 0.9f;
    ImVec2 origin = window->WorkRect.Min;
    origin.x += (windowW - block.x) * 0.5f;
    origin.y += (windowH - block.y) * 0.5f;
    ImGui::SetCursorScreenPos(origin);
    ImGui::BeginChild("##block", block, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    // Headers, search, and the three buttons.
    const std::string headerCVar = "gSettings.Menu.ActiveHeader";
    std::string active = CVarGetString(headerCVar.c_str(), mOrder.empty() ? "" : mOrder.front().c_str());
    if (!mHeaders.count(active) && !mOrder.empty())
        active = mOrder.front();
    ImGui::SetWindowFontScale(1.2f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 8.0f * scale));
    const ImVec4 c = accent();
    for (size_t i = 0; i < mOrder.size(); ++i) {
        if (i)
            ImGui::SameLine();
        const bool chosen = mOrder[i] == active;
        push_button_style(chosen ? c : ImVec4(0, 0, 0, 0));
        if (!chosen)
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(c, 0.8f));
        if (ImGui::Button(mOrder[i].c_str())) {
            CVarSetString(headerCVar.c_str(), mOrder[i].c_str());
            CVarSaveNextFrame();
            mSearch[0] = '\0';
            active = mOrder[i];
        }
        if (!chosen)
            ImGui::PopStyleColor();
        pop_button_style();
    }
    // Settings > General > Search In Sidebar moves the search box from the
    // headers' row to the top of the sidebar.
    const bool searchInSidebar = CVarGetInteger("gSettings.Menu.SearchInSidebar", 0) != 0;
    if (!searchInSidebar) {
        ImGui::SameLine();
        push_frame_style(c);
        ImGui::SetNextItemWidth(200.0f * scale);
        ImGui::InputTextWithHint("##search", "Search...", mSearch, sizeof mSearch);
        pop_frame_style();
    }
    const float iconW = ImGui::CalcTextSize("Restart").x + style.FramePadding.x * 2.0f;
    const float closeW = ImGui::CalcTextSize("Close").x + style.FramePadding.x * 2.0f;
    const float quitW = ImGui::CalcTextSize("Quit").x + style.FramePadding.x * 2.0f;
    ImGui::SameLine(block.x - (iconW + closeW + quitW + style.ItemSpacing.x * 2.0f) - style.WindowPadding.x);
    push_button_style(kRed);
    if (ImGui::Button("Quit"))
        Ask({"Quit", "Are you sure you want to quit?", "Quit", "Cancel", onQuit});
    pop_button_style();
    tooltip("Quit the game.", scale);
    ImGui::SameLine();
    push_button_style(kRed);
    if (ImGui::Button("Restart"))
        Ask({"Restart", "Back to the title screen? Unsaved progress will be lost.", "Restart", "Cancel", onRestart});
    pop_button_style();
    tooltip("Back to the title screen (the game's own reset, as holding B+X+Start).", scale);
    ImGui::SameLine();
    push_button_style(c);
    if (ImGui::Button("Close"))
        *open = false;
    pop_button_style();
    tooltip("Close Menu (Esc)", scale);
    ImGui::PopStyleVar();

    // The rule under the headers.
    ImVec2 pos = ImGui::GetCursorScreenPos();
    window = ImGui::GetCurrentWindow();
    window->DrawList->AddRectFilled(pos, ImVec2(pos.x + block.x - style.WindowPadding.x * 2.0f, pos.y + 4.0f * scale),
                                    IM_COL32_WHITE, 2.0f);
    ImGui::Dummy(ImVec2(0, 4.0f * scale));
    ImGui::Spacing();

    Header& header = mHeaders[active];
    float sidebarW = block.x > 1600.0f * scale ? block.x * 0.15f : 200.0f * scale;
    for (const auto& name : header.order)  // (wide enough for its longest name, whatever the UI Scale: not clipped)
        sidebarW = std::max(sidebarW, ImGui::CalcTextSize(name.c_str()).x + style.FramePadding.x * 2.0f + 28.0f * scale);
    const float contentH = ImGui::GetContentRegionAvail().y;
    std::string page = CVarGetString(header.sidebarCVar.c_str(), header.order.empty() ? "" : header.order.front().c_str());
    if (!header.sidebars.count(page) && !header.order.empty())
        page = header.order.front();

    ImGui::BeginChild("##sidebar", ImVec2(sidebarW, contentH), ImGuiChildFlags_None);
    if (searchInSidebar) {
        push_frame_style(c);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search...", mSearch, sizeof mSearch);
        pop_frame_style();
    }
    for (const auto& name : header.order) {
        if (sidebar_entry(name, name == page)) {
            CVarSetString(header.sidebarCVar.c_str(), name.c_str());
            CVarSaveNextFrame();
            mSearch[0] = '\0';
            page = name;
        }
    }
    ImGui::EndChild();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    pos = ImGui::GetCursorScreenPos();
    window->DrawList->AddRectFilled(pos, ImVec2(pos.x + 4.0f * scale, pos.y + contentH), IM_COL32_WHITE, 2.0f);
    ImGui::Dummy(ImVec2(4.0f * scale, contentH));
    ImGui::SameLine();

    const float contentW = ImGui::GetContentRegionAvail().x;
    std::string query = mSearch;
    query.erase(std::remove(query.begin(), query.end(), ' '), query.end());
    ImGui::BeginChild("##content", ImVec2(contentW, contentH), ImGuiChildFlags_None);
    if (!query.empty()) {
        ImGui::TextUnformatted("Search Results");
        ImGui::SameLine();
        push_button_style(c);
        if (ImGui::SmallButton("Clear Search"))
            mSearch[0] = '\0';
        pop_button_style();
        ImGui::Separator();
        DrawSearchResultsClassic(mSearch, ImGui::GetContentRegionAvail().x);
    } else if (header.sidebars.count(page)) {
        Sidebar& sidebar = header.sidebars[page];
        int columns = sidebar.columns;
        if (contentW < 600.0f * scale)
            columns = 1;
        const float columnW = (contentW - style.ItemSpacing.x * (columns - 1)) / columns;
        if (columns == 1) {
            for (auto& col : sidebar.widgets)
                for (auto& w : col)
                    DrawWidgetClassic(w, columnW - style.WindowPadding.x);
        } else {
            for (int i = 0; i < static_cast<int>(sidebar.widgets.size()); ++i) {
                if (i)
                    ImGui::SameLine();
                ImGui::PushID(i);
                ImGui::BeginChild("##column", ImVec2(columnW, 0), ImGuiChildFlags_AutoResizeY);
                for (auto& w : sidebar.widgets[i])
                    DrawWidgetClassic(w, columnW - style.WindowPadding.x * 2.0f);
                ImGui::EndChild();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndChild(); // block

    // The confirmation popup.
    if (mModalOpen) {
        ImGui::OpenPopup(mModal.title.c_str());
        mModalOpen = false;
    }
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(mModal.title.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(mModal.text.c_str());
        ImGui::Spacing();
        push_button_style(kRed);
        if (ImGui::Button(mModal.yes.c_str(), ImVec2(120.0f * scale, 0))) {
            ImGui::CloseCurrentPopup();
            if (mModal.onYes)
                mModal.onYes();
        }
        pop_button_style();
        ImGui::SameLine();
        push_button_style(c);
        if (ImGui::Button(mModal.no.c_str(), ImVec2(120.0f * scale, 0)))
            ImGui::CloseCurrentPopup();
        pop_button_style();
        ImGui::EndPopup();
    }
    ImGui::End();
    io.FontGlobalScale = savedFontScale;
}

} // namespace shipmenu
