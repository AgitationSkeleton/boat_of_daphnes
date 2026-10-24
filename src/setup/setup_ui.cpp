// The first run (src/setup/firstrun.h), kept plain: the system's file picker asks for the disc, a small window shows the
// build's progress, then the game starts. Problems are message boxes. Drawn with Dear ImGui on an SDL renderer, before
// the game's own window and renderer exist.
#include "setup/firstrun.h"
#include "setup/setup_ui.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace setup {

namespace fs = std::filesystem;

namespace {
const char* kTitle = "Boat of Daphnes";
const SDL_DialogFileFilter kFilters[] = {{"GameCube disc image", "iso;gcm;ciso;rvz;wia"}, {"All files", "*"}};

std::mutex s_pickLock;
std::string s_picked;      // the file the picker came back with
std::atomic<bool> s_picking{false};

void SDLCALL onPicked(void*, const char* const* files, int) {
    std::lock_guard g(s_pickLock);
    s_picked = files && files[0] ? files[0] : "";
    s_picking = false;
}

// The system's file picker, waited for: "" when the player closed it.
fs::path pickDisc(SDL_Window* parent) {
    s_picking = true;
    SDL_ShowOpenFileDialog(onPicked, nullptr, parent, kFilters, 2, nullptr, false);
    while (s_picking) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {}
        SDL_Delay(15);
    }
    std::lock_guard g(s_pickLock);
    return s_picked.empty() ? fs::path() : fs::u8path(s_picked);
}

// A message box with buttons; the chosen one's index (-1 closed).
int ask(SDL_Window* parent, SDL_MessageBoxFlags kind, const std::string& text, std::initializer_list<const char*> buttons) {
    SDL_MessageBoxButtonData data[4] = {};
    int n = 0;
    for (const char* b : buttons) {
        data[n].buttonID = n;
        data[n].text = b;
        if (n == 0) data[n].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        if (n == int(buttons.size()) - 1) data[n].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        ++n;
    }
    const SDL_MessageBoxData box{kind | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT, parent, kTitle, text.c_str(), n, data, nullptr};
    int hit = -1;
    if (!SDL_ShowMessageBox(&box, &hit)) return -1;
    return hit;
}

std::string clock(float s) {
    char b[32];
    const int t = int(s + 0.5f);
    if (t >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

ImFont* loadFont(const fs::path& exeDir, float size) {
    ImGuiIO& io = ImGui::GetIO();
    std::error_code ec;
    for (const fs::path& p : {fs::path("C:/Windows/Fonts/segoeui.ttf"), exeDir / "assets" / "fonts" / "RocknRollOne-Regular.ttf"})
        if (fs::exists(p, ec)) return io.Fonts->AddFontFromFileTTF(p.string().c_str(), size);
    ImFontConfig cfg;
    cfg.SizePixels = size;
    return io.Fonts->AddFontDefault(&cfg);
}
}  // namespace

bool runFirstRun(const fs::path& exeDir, fs::path& disc, bool rememberedDisc) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return false;

    // The disc: the remembered one, else asked for until a right one is picked (or the player gives up).
    if (!rememberedDisc) disc.clear();
    for (;;) {
        if (disc.empty()) {
            disc = pickDisc(nullptr);
            if (disc.empty()) {
                SDL_Quit();
                return false;
            }
        }
        std::string why;
        if (FirstRun::looksRight(disc, why)) break;
        disc.clear();
        if (ask(nullptr, SDL_MESSAGEBOX_WARNING, why + "\n\nBoat of Daphnes needs a disc image of The Wind Waker (USA).",
                {"Choose another", "Quit"}) != 0) {
            SDL_Quit();
            return false;
        }
    }
    if (!FirstRun::needed(exeDir, disc)) {
        SDL_Quit();
        return true;
    }

    // The build, in a small window.
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    SDL_Window* win = SDL_CreateWindow(kTitle, int(520 * scale), int(170 * scale), 0);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!ren) {
        SDL_Quit();
        return false;
    }
    SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsLight();
    ImGui::GetStyle().ScaleAllSizes(scale);
    loadFont(exeDir, 17.f * scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    FirstRun build;
    build.start(exeDir, disc);
    bool start = false, quit = false;
    while (!start && !quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
        }
        const FirstRun::Status st = build.status();
        if (st.done) {
            start = true;
            break;
        }
        if ((st.failed || st.cancelled) && !build.running()) {
            if (st.cancelled) {
                quit = true;
                break;
            }
            const int hit = ask(win, SDL_MESSAGEBOX_ERROR, "The game could not be built from your disc:\n\n" + st.error +
                                "\n\nThe details are in setup.log, beside the program.", {"Try again", "Choose another disc", "Quit"});
            if (hit == 1) {
                const fs::path other = pickDisc(win);
                std::string why;
                if (!other.empty() && FirstRun::looksRight(other, why)) disc = other;
                else if (!other.empty()) ask(win, SDL_MESSAGEBOX_WARNING, why, {"OK"});
            }
            if (hit == 0 || hit == 1) build.start(exeDir, disc);
            else quit = true;
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(st.rando ? "Building the game and the randomizer from your disc. This happens once."
                                        : "Building the game from your disc. This happens once.");
        ImGui::Spacing();
        char line[160];
        std::snprintf(line, sizeof line, "%s (step %d of %d)", FirstRun::stepName(st.step), st.step + 1,
                      st.rando ? int(FirstRun::kSteps) : int(FirstRun::RandoPatch));
        ImGui::TextUnformatted(line);
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d%%", int(st.overall * 100));
        ImGui::ProgressBar(st.overall, ImVec2(-1, 0), pct);
        std::snprintf(line, sizeof line, "%s elapsed%s%s", clock(st.elapsed).c_str(), st.eta >= 0 ? ", about " : "",
                      st.eta >= 0 ? (clock(st.eta) + " left").c_str() : "");
        ImGui::TextDisabled("%s", line);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - 90 * scale);
        if (ImGui::Button("Cancel", ImVec2(90 * scale, 0))) build.cancel();
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 240, 240, 240, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }
    if (build.running()) build.cancel();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return start;
}

int runFirstRunCli(const fs::path& exeDir, const fs::path& disc) {
    std::string why;
    if (disc.empty()) {
        std::fprintf(stderr, "setup: no disc image (--iso FILE)\n");
        return 2;
    }
    if (!FirstRun::looksRight(disc, why)) {
        std::fprintf(stderr, "setup: %s\n", why.c_str());
        return 2;
    }
    if (!FirstRun::needed(exeDir, disc)) {
        std::printf("setup: the game is already built\n");
        return 0;
    }
    FirstRun build;
    build.start(exeDir, disc);
    int step = -1;
    float lastPrint = -100;
    for (;; SDL_Delay(500)) {
        const FirstRun::Status st = build.status();
        if (st.done) {
            std::printf("setup: done in %s\n", clock(st.elapsed).c_str());
            return 0;
        }
        if ((st.failed || st.cancelled) && !build.running()) {
            std::fprintf(stderr, "setup: failed: %s (setup.log has the details)\n", st.error.c_str());
            return 1;
        }
        if (st.step != step || st.elapsed - lastPrint >= 60) {
            std::printf("setup: %s, step %d of %d, %d%% (%s), %s\n", FirstRun::stepName(st.step), st.step + 1,
                        st.rando ? int(FirstRun::kSteps) : int(FirstRun::RandoPatch), int(st.overall * 100), st.detail.c_str(),
                        clock(st.elapsed).c_str());
            std::fflush(stdout);
            step = st.step, lastPrint = st.elapsed;
        }
    }
}

}  // namespace setup
