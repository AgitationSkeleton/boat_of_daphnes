// Notifications: see notifications.h.
#ifdef rename
#undef rename
#endif

#include "notifications.h"

#include "item_found.h"
#include "ship_menu.h"
#include "ship/cvar.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace notifications {
namespace {

using shipmenu::WidgetInfo;
using shipmenu::WidgetType;

constexpr const char* kPosition = "gSettings.Notifications.Position";
constexpr const char* kDuration = "gSettings.Notifications.Duration";
constexpr const char* kBgOpacity = "gSettings.Notifications.BgOpacity";
constexpr const char* kSize = "gSettings.Notifications.Size";

constexpr float kMargin = 30.f;   // from the screen's edge
constexpr float kPadding = 10.f;  // between notifications
constexpr float kIcon = 24.f;     // the item icon, square
constexpr float kFadeTime = 4.f;  // fades over the last four seconds (to nothing at the last one)

struct Notification {
    unsigned id = 0;
    Options o;
    float remaining = 0.f;
    float height = 0.f; // measured last frame
};

std::vector<Notification> g_list; // oldest first
unsigned g_next_id = 1;

} // namespace

void Emit(Options options) {
    Notification n;
    n.id = g_next_id++;
    n.remaining = options.remaining > 0.f ? options.remaining : CVarGetFloat(kDuration, 10.f);
    n.o = std::move(options);
    std::fprintf(stderr, "[toast] %s|%s|%s (%zu shown)\n", n.o.prefix.c_str(), n.o.message.c_str(), n.o.suffix.c_str(), g_list.size());
    g_list.push_back(std::move(n));
}

void Frame(float scale, float fontScale) {
    const float dt = ImGui::GetIO().DeltaTime;
    for (size_t i = 0; i < g_list.size();) {
        g_list[i].remaining -= dt;
        if (g_list[i].remaining <= 0.f)
            g_list.erase(g_list.begin() + static_cast<long>(i));
        else
            ++i;
    }
    const int position = CVarGetInteger(kPosition, 3);
    if (position < 0 || position > 3 || g_list.empty())
        return; // hidden (they still run out)
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float margin = kMargin * scale;
    const bool right = position == 1 || position == 3, bottom = position >= 2;
    const ImVec2 anchor(vp->Pos.x + (right ? vp->Size.x - margin : margin),
                        vp->Pos.y + (bottom ? vp->Size.y - margin : margin));
    const float size = CVarGetFloat(kSize, 1.8f), bg = CVarGetFloat(kBgOpacity, 0.5f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    // The newest nearest the corner, the older ones stacked outward.
    float offset = 0.f;
    for (size_t k = g_list.size(); k-- > 0;) {
        Notification& n = g_list[k];
        // Until it has been drawn once its height is a line of text and the window's padding.
        const float height = n.height > 0.f ? n.height : ImGui::GetFontSize() * size * fontScale + 16.f * scale;
        ImGui::SetNextWindowPos(ImVec2(anchor.x, anchor.y + (bottom ? -offset : offset)), ImGuiCond_Always,
                                ImVec2(right ? 1.f : 0.f, bottom ? 1.f : 0.f));
        offset += height + kPadding * scale;
        const float alpha = n.remaining < kFadeTime ? std::clamp((n.remaining - 1.f) / 3.f, 0.f, 1.f) : 1.f;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f * scale);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, bg));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
        char name[32];
        std::snprintf(name, sizeof name, "notification#%u", n.id);
        if (ImGui::Begin(name, nullptr, flags)) {
            ImGui::SetWindowFontScale(size * fontScale);
            bool any = false;
            if (n.o.icon != 0) {
                ImGui::Image(n.o.icon, ImVec2(kIcon * scale, kIcon * scale));
                any = true;
            }
            if (n.o.portrait != 0) {
                const float h = ImGui::GetTextLineHeight() * 1.6f;
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(h, h));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 centre(at.x + h * 0.5f, at.y + h * 0.5f);
                const ImVec4& c = n.o.prefixColor;
                dl->AddCircleFilled(centre, h * 0.5f, ImGui::GetColorU32(ImVec4(c.x * 0.35f, c.y * 0.35f, c.z * 0.35f, 0.75f * alpha)));
                dl->AddImageRounded(n.o.portrait, at, ImVec2(at.x + h, at.y + h), ImVec2(0, 0), ImVec2(1, 1),
                                    ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), h * 0.5f);
                dl->AddCircle(centre, h * 0.5f, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, alpha)), 0, std::max(1.5f, 1.5f * scale));
                any = true;
            }
            // With a portrait the text is placed at its middle, in a group: begun on the portrait's line (SameLine), the group kept
            // that line's top and every later SameLine went back there, so the rest rode higher than the name (user 2026-10-25).
            const bool grouped = n.o.portrait != 0;
            ImVec2 below;
            if (grouped) {
                const float h = ImGui::GetTextLineHeight() * 1.6f;
                const ImVec2 at = ImGui::GetItemRectMin();
                below = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(at.x + h + ImGui::GetStyle().ItemSpacing.x, at.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
                ImGui::BeginGroup();
                any = false;
            }
            auto part = [&](const std::string& text, const ImVec4& color) {
                if (text.empty())
                    return;
                if (any)
                    ImGui::SameLine();
                ImGui::TextColored(color, "%s", text.c_str());
                any = true;
            };
            part(n.o.prefix, n.o.prefixColor);
            part(n.o.message, n.o.messageColor);
            part(n.o.suffix, n.o.suffixColor);
            if (grouped) {
                ImGui::EndGroup();
                const float y = ImGui::GetCursorScreenPos().y;
                if (below.y - y - ImGui::GetStyle().ItemSpacing.y > 0)
                    ImGui::Dummy(ImVec2(0, below.y - y - ImGui::GetStyle().ItemSpacing.y));
            }
            n.height = ImGui::GetWindowSize().y;
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

void BuildMenu(shipmenu::Menu& m, const char* H, const char* S, int column) {
    m.AddWidget(H, S, column, "Notifications", WidgetType::SeparatorText);
    m.AddWidget(H, S, column, "Position", WidgetType::CVarCombobox)
        .CVar(kPosition)
        .Items({"Top Left", "Top Right", "Bottom Left", "Bottom Right", "Hidden"})
        .DefaultValue(3)
        .Tooltip("Which corner of the screen notifications appear in.");
    m.AddWidget(H, S, column, "Duration: %.1f seconds", WidgetType::CVarSliderFloat)
        .CVar(kDuration)
        .RangeFloat(3.f, 30.f, 0.1f)
        .DefaultFloat(10.f)
        .Tooltip("How long notifications are displayed for.");
    m.AddWidget(H, S, column, "Background Opacity: %.0f %%", WidgetType::CVarSliderFloat)
        .CVar(kBgOpacity)
        .RangeFloat(0.f, 1.f, 0.05f)
        .DefaultFloat(0.5f)
        .Percent()
        .Tooltip("How opaque the background of notifications is.");
    m.AddWidget(H, S, column, "Size: %.1f", WidgetType::CVarSliderFloat)
        .CVar(kSize)
        .RangeFloat(1.f, 5.f, 0.1f)
        .DefaultFloat(1.8f)
        .Tooltip("How large notifications are.");
    m.AddWidget(H, S, column, "Test Notification", WidgetType::Button)
        .Tooltip("Displays a test notification.")
        .Callback([](WidgetInfo&) {
            Options o;
            o.icon = item_found::Icon(0x22); // the Wind Waker (SoH shows a gold Skulltula token)
            o.prefix = "This";
            o.message = "is a";
            o.suffix = "test.";
            Emit(std::move(o));
        });
}

} // namespace notifications
