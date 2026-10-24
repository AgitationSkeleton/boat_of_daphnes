// First-run setup, inside the game's own program: the player clicks the .exe and it loads. Nothing translated from the
// game ships with Boat of Daphnes; the first time (and after an update or another disc), before the game starts, its
// code is built here from the player's own disc:
//
//   1. Check      the disc: The Wind Waker (USA, GZLE01), its program the one the recompiler is made for
//   2. Extract    main.dol and every code module (rels/ and RELS.arc: Yaz0, RARC)
//   3. Recompile  the PowerPC code to C++ (src/recompiler, linked into the program)
//   4. Compile    that C++ with the bundled clang (tools/llvm), one process per core
//   5. Link       tww_game.dll with the bundled lld-link, against boat-of-daphnes.lib (the program's exports);
//                 on Linux tww_game.so with ld.lld, which binds the program's exported names when it is loaded
// and, when the randomizer's generator is beside the program (rando/), the same for wwrando's patched program:
//   6. Patch      wwrando's reference run of the disc (its code, seed independent) to game/rando/GZLE99.iso
//   7-10.         extracted, recompiled, compiled and linked as tww_game_rando.dll
//   11. Extras    what switching between the two needs (the functions and the disc files they have differently), the
//                 Cosmetics Editor's Link colors and Swift Sail's textures
// Later starts find it all made (tww_game.txt / tww_game_rando.txt say from what) and go straight to the game.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace setup {

class FirstRun {
public:
    enum Step { Check, Extract, Recompile, Compile, Link, RandoPatch, RandoRecompile, RandoCompile, RandoLink, Extras, kSteps };
    static const char* stepName(int s);

    struct Status {
        bool running = false, done = false, failed = false, cancelled = false;
        int step = Check;
        float stepFrac = 0;   // 0-1 within the step
        float overall = 0;    // 0-1 over every step
        std::string detail;   // what the step is on
        std::string error;    // why it failed
        std::string note;     // worth telling, not an error
        float elapsed = 0;    // seconds
        float eta = -1;       // seconds left (-1: not known yet)
        bool rando = false;   // the randomizer is being built too
    };

    ~FirstRun();
    // Whether the game's code has to be built (again) from this disc: missing, made by another version, from another
    // disc; or the randomizer's, when its generator is there and its module is not made.
    static bool needed(const std::filesystem::path& exeDir, const std::filesystem::path& disc);
    // A quick look at a disc image: The Wind Waker (USA)? (its id, before anything is built)
    static bool looksRight(const std::filesystem::path& disc, std::string& why);

    void start(const std::filesystem::path& exeDir, const std::filesystem::path& disc);
    void cancel();
    Status status() const;
    bool running() const { return m_running.load(); }

private:
    struct Program;
    void run();
    bool buildGame(int stepBase, const std::filesystem::path& disc, const char* dll, const char* gen, bool rando, std::string& sha1);
    bool extract(int step, const std::filesystem::path& disc, Program& p);
    bool recompile(int step, Program& p, const std::filesystem::path& gen);
    bool compile(int step, const std::filesystem::path& gen, const std::filesystem::path& obj, bool rando);
    bool link(int step, const std::filesystem::path& obj, const char* dll);
    bool patchRando();
    bool extras();
    void set(int step, float frac, const std::string& detail);
    bool fail(const std::string& why);
    void log(const std::string& line);

    std::filesystem::path m_exeDir, m_disc, m_work, m_logFile;
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_cancel{false};
    mutable std::mutex m_lock;
    Status m_status;
    std::chrono::steady_clock::time_point m_t0;
    std::vector<std::filesystem::path> m_objects;
    std::filesystem::path m_clang, m_linker;                    // the compiler and the linker (findTools)
    std::vector<std::pair<std::string, std::string>> m_toolEnv;  // their environment, added to ours
};

}  // namespace setup
