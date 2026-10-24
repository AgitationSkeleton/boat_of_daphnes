# Boat of Daphnes on Linux

The same program as the Windows one: the settings menu, the randomizer and its trackers, Anchor online play, the model
browser, mods and the in-app Game Boy Advance. The game itself is not included: the first start builds it from your own
disc on your own PC (`src/setup/firstrun.cpp`), as on Windows, and the result stays yours.

## What you need

- 64-bit Linux, glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39, current Arch and SteamOS), a CPU with AVX2 and a
  Vulkan driver (Mesa for AMD and Intel, the NVIDIA driver).
- Libraries every desktop has: `libssl3`, `libpng16`, `libfreetype6`, SDL3's own (X11 or Wayland, ALSA, PulseAudio or
  PipeWire, libudev) and `libxml2` for the bundled linker.
- Your own USA disc image of The Wind Waker (`GZLE01`, revision 0): `.iso`, `.gcm`, `.ciso`, `.rvz` or `.wia`.
- About 4 GB of free space in `~/.cache` for the build (it is removed when the build ends) and a few GB beside the
  program; 8 GB of memory (the compiler lowers its parallel jobs by itself when memory is short).
- A folder you can write to for the program: the game's code is built into it. Not `/usr` or `/opt`.

## The first start

`./boat-of-daphnes` asks for the disc (your desktop's file chooser, an XDG portal or `zenity`; or start with
`--iso FILE`), then builds the game and the randomizer: it checks the disc, translates the PowerPC code to C++, compiles it
with the bundled clang (`tools/llvm`) and links `tww_game.so` and `tww_game_rando.so` with the bundled `ld.lld`. The windows
shows progress and has a cancel button; `setup.log` beside the program has the details. It takes 10 to 20 minutes and
happens once.

The compiler is LLVM's own release of clang and lld (statically linked, they need glibc 2.34 and, for lld, `libxml2.so.2`).
If `tools/llvm` is missing or its linker does not start on your system, the setup uses `clang` and `ld.lld` from your
`PATH` (any version 14 or newer; `apt install clang lld`).

## Where your files are

Beside the program, as on Windows: `saves/` (the memory card), `mods/`, `Randomizer/`, `logs/`, `game/` (made from your
disc), `boat-of-daphnes.json` (settings). The program starts from its own folder whatever the current directory is.
Session logs are `logs/session-*.log`; a crash adds a report with the faulting address and a stack
(`addr2line -e boat-of-daphnes` names the frames of the program, `-e tww_game.so` those of the game).

## Controls and the GameCube adapter

F1 or Esc opens the menu, F11 toggles fullscreen. Game controllers work through SDL3; the menu's Controls page binds them.
The official GameCube adapter (Wii U, `057e:0337`) is read by the program through hidraw and needs the udev rule in
`51-gcadapter.rules` (copy it to `/etc/udev/rules.d/`, then `sudo udevadm control --reload-rules && sudo udevadm trigger`
and replug it). Rumble needs the adapter's second USB cable.

## Wayland and X11

SDL3 picks Wayland when the session is one and X11 otherwise; `SDL_VIDEODRIVER=x11` or `wayland` forces one. The window is
matched to `BoatOfDaphnes.desktop` by its app id (`./install-menu-entry.sh` adds the entry to your menu). If fullscreen
misbehaves under a compositor, press F11 again or start with `--window WxH`.

## Building it

See the README. The program is one CMake project (`cmake --preset linux-clang`); what differs from Windows is in
`src/bod/windows/src/platform/` (`platform_posix.cpp`, `net_posix.cpp` with OpenSSL for TLS, HTTP downloads and WebSocket),
`src/setup/firstrun.cpp` (child processes), `src/runtime/game.cpp` (`dlopen` of the modules) and `CMakeLists.txt` (the
modules' build flags: `-ffreestanding -nostdlib -fvisibility=hidden`, bound to the program's exported names when loaded).
`tools/package.sh` makes the release tarball; `.github/workflows/linux.yml` builds and packages it on Ubuntu 24.04.

## Not on Linux yet

An AppImage (the game's code is built next to the program, which an AppImage's read-only mount cannot take), the GameCube
adapter driver install (not needed), and a minidump on a freeze (the stack report stands in).
