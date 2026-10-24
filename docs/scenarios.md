# Scenarios: jump straight into a game situation

`tools/scenario.py` starts the game from a chosen save, sets up a situation, scripts it, takes pictures and checks the log.
A scenario is a short text file in `tools/scenarios/`; the ones there double as regression tests.

    python tools/scenario.py --list
    python tools/scenario.py windfall_picto_photo
    python tools/scenario.py shop_exit_warp reveal_sea_chart --windowed

Before the first run: build the program (`boat-of-daphnes.exe`), and put the game's modules beside it
(`python tools/scenario.py --prepare <a packaged release's folder>` copies `tww_game.dll`, `game\`, `rando\` ... from one;
they work while the runtime ABI is unchanged). `--iso` (or `BOD_DISC`) is the player's disc image. The saves are never in the
repository: `--saves <folder>` (or `BOD_SAVES`) is a folder of `.gci` files, for example the CryZe/WindWakerSaves 100% route.

Each run writes `build/scenarios/<name>/` (`--out`): `run.log` (the game's log), `script.txt`, `card.raw` (the memory card the
game used), `shots/*.png` and `result.json` (`passed`, each `expect`, the `peek` lines, problems). The exit code is 1 when a
scenario fails. Runs are headless unless the scenario says `windowed` (the menu and its captures need a window); the lock
wrapper `local/bod2/rungame.py` is used when it exists, so only one game window is ever open on the PC.

## The file

One command a line, `#` starts a comment. The setup lines are read by the tool; everything else is the script the game works
through (`BLUEWAKE_TEST_SCRIPT`, `src/runtime/bod_testscript.cpp`).

### Setup (tool)

| line | meaning |
|---|---|
| `description TEXT` | shown by `--list` |
| `save NAME` | the base save: a path, or a name in the saves folder (`2-Pictobox` finds `2-Pictobox.gci`). Japanese saves are converted to the US game, a memory card is built. `save new` (or no `save`) is an empty formatted card |
| `patch OFFSET=VALUE ...` / `OFFSET\|VALUE` | bytes of save file 1 set / or-ed (offsets from `g_dComIfG_gameInfo`, checksums redone) |
| `cvar NAME VALUE` | a setting in `boat-of-daphnes.json` for this run (restored after) |
| `env NAME VALUE` | an environment variable for the game |
| `options LIST` | `BLUEWAKE_OPTIONS` (game options on/off; the default is the options' defaults) |
| `size WxH` | the window / render size |
| `windowed` | a window instead of headless |
| `timeout SECONDS`, `frames N` | wall-clock and retrace limits |
| `expect REGEX` / `expect_not REGEX` | the log must / must not match |

A run also fails when the script did not reach its end, a wait timed out (`[script] TIMEOUT`) or a command is unknown.

### Script (game)

Times are retraces (60 a second; the game runs 30 frames a second on them), counted from the start of the command. A command
that takes time holds the following ones; the others run at once. Edits wait for a safe moment first (a play scene with Link in
it, no event, no scene change, steady for 20 retraces).

| command | meaning |
|---|---|
| `start load [SLOT]` | through the title and file select to the loaded save (0 to 2, default 0), decided from what the game shows, not from times |
| `start new [SLOT]` | a new game: name entry, then A through the opening until Link is playable |
| `wait N` | N retraces |
| `wait link [TIMEOUT]` | until Link is playable |
| `wait stage NAME [ROOM]` | until Link is playable in that stage / room |
| `wait event` / `wait noevent` | until a cutscene or text starts / ends |
| `give ITEM [COUNT]`, `equip X\|Y\|Z ITEM\|none`, `collect songs\|pearls\|shards\|swords` | the Dev Tools console's commands (`items` lists the names). `equip` needs the item in the inventory |
| `rupees N`, `health N` (quarter hearts), `maxhealth N`, `magic N`, `bombs N`, `arrows N` | status |
| `flag set\|clear\|get event\|tmp ID` | an event flag (`ID` as 0x0A04: byte, then bit mask) |
| `time DEGREES [hold]` | time of day in degrees, 0 to 360 (15 an hour, as the Save Editor's slider); `hold` keeps it |
| `console TEXT` | any Dev Tools console command (`spawn`, `actors`, `pos`, ...) |
| `warp STAGE ROOM [POINT [LAYER [WIPE]]]` | a scene change as a door makes one; waits for it, asks again if the game ignored the first |
| `ride STAGE ROOM [POINT [LAYER [WIPE]]]` | `warp`, but Link arrives on the King of Red Lions (the restart's last scene mode 6; a sea stage, after MET_KORL). A mid-story save plays a short cutscene on arrival |
| `teleport STAGE ROOM X Y Z [ANGLE]` | Link to a place (the restart record), as Anchor's teleport |
| `press BUTTON[+BUTTON] [FRAMES]` | A B X Y START Z R L UP DOWN LEFT RIGHT, held 8 retraces by default |
| `hold BUTTON` / `release [BUTTON\|all]` | held until released |
| `stick X Y [FRAMES]`, `cstick X Y [FRAMES]` | -128..127; without FRAMES kept until the next one |
| `peek ADDR[/SIZE] [LABEL]` | logs `[script] peek 0x... = 0x...` |
| `poke ADDR=VALUE[/SIZE]` | writes guest memory |
| `shot [NAME]` | a frame dump of the game's picture to `shots/NAME.png` |
| `menu HEADER/PAGE` / `menu close` | the Boat of Daphnes menu on a page (windowed); `winshot NAME` then captures the windows |
| `winshot NAME` | PNGs of the game's windows (the menu too) in `shots/` |
| `reveal 0\|1`, `bunny 0\|1` | the Reveal Sea Chart / Bunny Hood features (a headless run has no settings screen) |
| `log TEXT` | a marker line `[script] TEXT` to `expect` on |
| `quit` | end the run (it ends 45 retraces after the last command anyway, so a `shot` is written) |

Every command logs `[script] line N at retrace R: ...`; the rest of the log is the game's, so `expect` can use any of it
(`\[efb\] photo judged`, `\[scene\] stage=...`).

## Notes

- The state edits go through the Dev Tools code (`src/bod/windows/src/dev_windows.cpp`, `RunCommand`), so a new console
  command is a new scenario command (`console ...`) without more work.
- Headless runs have no menu; the Boat of Daphnes settings are read from the `boat-of-daphnes.json` beside the exe, which
  `cvar` lines change for the duration of the run.
- Do not run scenarios while measuring performance, and never two at once: the lock wrapper serialises them.
