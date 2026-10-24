# Boat of Daphnes

**The Legend of Zelda: The Wind Waker on PC: a native static recompilation of the GameCube original, with
smooth in-between frames, widescreen, a seamless randomizer, online co-op, player models and a menu made from the
game's own art.**

![The title screen](docs/images/title.png)

> [!IMPORTANT]
> **Bring your own disc.** Boat of Daphnes needs your own copy of *The Wind Waker* for GameCube, USA version
> (`GZLE01`, revision 0), as an `.iso`, `.gcm`, `.ciso`, `.rvz` or `.wia` disc image. This repository and its
> releases contain no game code, assets or saves: the game's code is built on your PC from your disc the first time
> you start it.

## Play

1. Download `boat-of-daphnes-<version>-windows-x64.zip` from the [Releases](../../releases) page and unzip it anywhere.
2. Start `boat-of-daphnes.exe` and pick your disc image when it asks.
3. The first start builds the game and the randomizer from your disc. It takes about 10 to 20 minutes on a modern PC,
   with a progress bar, and happens once. After that the program goes straight to the game.

Press **F1** (or **Esc**) for the menu. Settings, saves, mods and logs live beside the program.

You need Windows 10 or 11, a GPU with Direct3D 12 or Vulkan, and a CPU with AVX2.

### Linux

1. Download `boat-of-daphnes-<version>-linux-x86_64.tar.gz` from the [Releases](../../releases) page and unpack it
   anywhere you can write to (not `/usr` or `/opt`: the first start puts the game beside the program).
2. Run `./boat-of-daphnes` and pick your disc image when it asks. The first start builds the game and the randomizer
   from your disc, with the compiler in the download (`tools/llvm`): you do not install one. It happens once.
3. Optional: `./install-menu-entry.sh` adds it to your application menu; for the GameCube adapter, copy
   `51-gcadapter.rules` to `/etc/udev/rules.d/` and replug it.

You need a 64-bit Linux with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, SteamOS 3 and later), a CPU with
AVX2, a Vulkan driver (Mesa or NVIDIA), and these libraries, which every desktop has: libSDL3's own dependencies
(X11 or Wayland, ALSA/PulseAudio/PipeWire), `libssl3`, `libpng16`, `libfreetype6`, `libxml2` (for the bundled linker;
without it the setup uses your own `clang` and `ld.lld`/`lld` if installed). The disc chooser uses your desktop's file
dialog (an XDG portal, or `zenity`); `--iso FILE` skips it. Settings, saves, mods and logs live beside the program as on
Windows.

## What's in it

### Smooth motion and a modern picture

The game runs at its own 30 frames a second; on a faster display the renderer draws in-between frames, so the picture
moves at 60, 120 or 144 Hz without changing how the game plays. The camera is blended once per frame and every object
is placed in the world and seen through it, so nothing shakes as the camera turns.

![30 fps against in-between frames](docs/images/interpolation.gif)

([WebM version](docs/images/interpolation.webm))

Also: widescreen with the HUD at its proper place, any window size or fullscreen, internal resolution, anisotropic
filtering, integer scaling and an FPS counter.

### A menu made from The Wind Waker

The menu follows the layout of [Dusklight](https://github.com/TwilitRealm/dusklight)'s menus, drawn with the game's
own art read from your disc: the sea chart's paper, the file select's plates, the pause menu's cursor and the game's
lettering, with Hylian script on the tabs and the help pane. It slides in with F1, and everything in it eases between
states. Prefer the original look? **Settings › General › Menu Theme › Classic** brings back Boat of Daphnes' first menu.

| Wind Waker theme | Classic theme |
| --- | --- |
| ![Wind Waker theme](docs/images/menu-wind-waker.png) | ![Classic theme](docs/images/menu-classic.png) |

### Enhancements

Better Wind Waker's switches and more, each on its own:

- **Skips and speed-ups:** Swift Sail and Brisk Sail, instant text, Text Speed (1x to 5x), Skip Text (hold B), faster
  rolling, grappling, block pushing, climbing and Ballad of Gales, quick doors, faster transitions, Skip Intro with
  Start, Remove Title and Ending Videos.
- **Quality of life:** Tingle Chests without the Tingle Tuner, Reveal Sea Chart (it changes only what the chart shows,
  never your save), treasure lights up close, Forest Water helpers, unrestricted boat and camera, Switch Targeting.
- **Difficulty and fixes:** Hero Mode, no low-health beep, wwrando's vanilla-bug fixes, and a fix for the freezes after
  some warps (a missing start point now starts Link at the room's first point).
- **The Bunny Hood:** a new item, worn from the Hero's Charm slot, that makes Link run faster, with its own colours.

![Enhancements](docs/images/enhancements.png)

### Randomizer

[The Wind Waker Randomizer](https://github.com/LagoLunatic/wwrando) is built in. Choose your options and generate a
seed in the menu, then pick **Quest: Randomizer** on the file select: the game switches into the seed **without
restarting**, and back to the normal game the same way. Seed hash icons, a spoiler log, an item tracker and a check
tracker are included.

![Randomizer](docs/images/randomizer-menu.png)

### Cosmetics

The Cosmetics Editor recolours Link's Hero's and Outset clothes, his hair, eyes and skin, sword trails, the HUD's
buttons, Tetra, Princess Zelda, the King of Red Lions and **every piece of equipment**: swords, scabbards, shields,
bow and arrows, boomerang, hookshot, Deku Leaf, bombs, telescope and Power Bracelets. Each colour has Random, Rainbow
and Lock. Equipment that comes from a player model mod takes the same colours. Link's voice pitch and a Player Gender
setting (how the game's text refers to you) are here too.

![Cosmetics Editor: equipment](docs/images/cosmetics-equipment.png)

### Mods and player models

Drop folders or `.zip`, `.7z` and `.rar` archives into `mods\`. Boat of Daphnes reads player models in wwrando's
format (Link, his equipment and his voice can each come from a different mod), file replacements and Dolphin-format
texture packs, and swaps Link's model without a restart.

**Browse Models** lists The Wind Waker's player models on [GameBanana](https://gamebanana.com/games/6173) with their
pictures. Pick one and **Download & Install** fetches it through GameBanana's own link into your mods folder and
puts it on Link. Models belong to their authors: the browser shows each one's author and licence and links to its page.

| Browse Models | Mods |
| --- | --- |
| ![Browse Models](docs/images/browse-models.png) | ![Mods](docs/images/mods.png) |

![Young Link by ColonelBirdstrong, from GameBanana](docs/images/young-link-mod.png)

*[Young Link](https://gamebanana.com/mods/250753) by ColonelBirdstrong, installed from the browser.*

### Online co-op (Anchor)

Play together over the internet: other players appear as Link (in their own models, colours and voices) in your
world and on your Sea Chart, progress is shared with your team, switches sync live, you can carry each other, and PvP
can be turned on per room. Self-host the server from [`server/anchor`](server/anchor).

![Network](docs/images/network.png)

### And

The Tingle Tuner works with a built-in Game Boy Advance (mGBA), GameCube controller adapters are supported with rumble,
buttons can be rebound per port, an input viewer shows what the game receives, and the Dev Tools have a save editor,
flag viewer and actor viewer.

## Build it yourself

You need Windows with Visual Studio 2022 (Desktop development with C++, plus its Clang tools), CMake, Ninja, Git and
Python 3.

~~~bat
git clone --recursive https://github.com/AgitationSkeleton/boat_of_daphnes
cd boat_of_daphnes
cmake --preset windows-clang
cmake --build --preset windows-clang --target tww
~~~

This builds `build\windows-clang\boat-of-daphnes.exe`. Aurora (the renderer, `extern/aurora`) is patched from
`patches/aurora` automatically. Then:

- `python tools\rando\rando_runtime.py` fetches the randomizer's generator: an embedded Python, wwrando and gclib,
  each checked against a pinned hash.
- `python tools\rando\install_dev.py` sets up the generator, the randomizer's reference disc and the extras beside the
  program you built, for development. A player's first start does the same through `src/setup`.
- `powershell -File tools\package.ps1 -Version <version>` makes the release zip in `dist\`. It carries the program, the
  bundled compiler the first start builds the game with, and the randomizer's generator, and it refuses to include
  anything made from a disc.

### Linux

Needs clang 17 or newer, CMake 3.25+, Ninja, Git, Python 3, and the development packages below (Ubuntu 24.04 names).

~~~sh
sudo apt install git cmake ninja-build clang lld python3 pkg-config g++-13 libstdc++-13-dev libssl-dev libudev-dev \
    zlib1g-dev libpng-dev libfreetype-dev libwayland-dev libxkbcommon-dev libx11-dev libx11-xcb-dev libxrandr-dev \
    libxext-dev libxi-dev libxcursor-dev libxss-dev libxtst-dev libgl1-mesa-dev libvulkan-dev libasound2-dev \
    libpulse-dev libdbus-1-dev
git clone --recursive https://github.com/AgitationSkeleton/boat_of_daphnes
cd boat_of_daphnes
cmake --preset linux-clang -DCMAKE_C_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13 \
    -DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13
cmake --build --preset linux-clang --target tww
~~~

(The `--gcc-install-dir` flags point clang 18 and older at gcc 13's C++ headers, which they can read; with clang 19 or
newer leave them out.) This builds `build/linux-clang/boat-of-daphnes` and `libSDL3.so.0` beside it. Configure with
`-DBOD_STATIC_STDLIB=ON` to link libstdc++ into the program, as the release does. `python3 tools/rando/rando_runtime.py`
fetches the randomizer's generator (python-build-standalone, wwrando, gclib), `python3 tools/rando/install_dev.py` sets
up the extras beside the program for development, and `tools/package.sh --version <version>` makes
`dist/boat-of-daphnes-<version>-linux-x86_64.tar.gz` with the compiler (LLVM's release of clang and lld, fetched and
checked against a pinned hash) and the generator. `tools/linux/toolchain_selftest.sh tools/llvm build/linux-clang/boat-of-daphnes`
checks that the bundled compiler builds the game module's glue and that the program exports what a module binds to.

How it works, in short: `src/recompiler` translates the game's PowerPC code (`main.dol` and its 415 modules) to C++,
which is compiled into `tww_game.dll`; `src/runtime` is the console: CPU state, the OS and hardware replacements, the
DSP's audio, the disc, and a GX translator feeding Aurora. Boat of Daphnes' features live in `src/bod` (its menus,
editors, online play and host features) and `src/runtime/bod_*.cpp` (their game-side hooks).

## Credits

- **Boat of Daphnes** by [AgitationSkeleton](https://github.com/AgitationSkeleton): the features, menus, editors,
  online play, Bunny Hood and randomizer integration this port carries.
- **[The Wind Waker Randomizer](https://github.com/LagoLunatic/wwrando)** and
  **[gclib](https://github.com/LagoLunatic/gclib)** by LagoLunatic (MIT).
- **[zeldaret/tww](https://github.com/zeldaret/tww)**, The Wind Waker's decompilation, for names and structures.
- **[Aurora](https://github.com/encounter/aurora)** by encounter, the renderer.
- **[Dolphin](https://dolphin-emu.org/)** (GPL-2.0-or-later): the Zelda audio microcode and GBA link are ported from it.
- **[Better Wind Waker](https://github.com/WideBoner/betterww)**, whose switches the enhancements follow.
- **[Dusklight](https://github.com/TwilitRealm/dusklight)** (CC0), whose menu layout the Wind Waker theme follows.
- **[mGBA](https://mgba.io/)** (MPL-2.0) with [emibios](https://github.com/coolbho3k/emibios) (LGPL-3.0-or-later);
  [libarchive](https://www.libarchive.org/) (BSD-2-Clause); [stb_image](https://github.com/nothings/stb) (public
  domain); [picojson](https://github.com/kazuho/picojson) (BSD-2-Clause);
  [RocknRoll One](https://fonts.google.com/specimen/RocknRoll+One) by Takashi Funayama (OFL-1.1).
- Player models and texture packs belong to their authors on GameBanana and elsewhere; none are included.

## License

Boat of Daphnes is licensed under the GNU General Public License, version 3 or later: see [LICENSE.md](LICENSE.md).
Third-party parts keep their own licenses, listed above and in their folders. The Legend of Zelda and The Wind Waker
are Nintendo's; this project is not affiliated with Nintendo, and it ships nothing of the game.
