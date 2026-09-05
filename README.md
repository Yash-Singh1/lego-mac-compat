# 32-bit LEGO games on modern macOS

A native loader that runs the original 32-bit Intel Mac LEGO games on
current macOS (Apple Silicon through Rosetta 2, or Intel). It maps the
game's i386 executable into a 64-bit process, switches into a 32-bit code
segment to run it, and bridges the OS, OpenGL, Cg, CoreAudio and controller
interfaces the game expects. No SIP changes, kernel extensions, VMs or Wine.
You need your own copy of the game.

Support for the original 32-bit Mac releases:

| Game | Mac release | Status |
| --- | --- | --- |
| LEGO Star Wars II: The Original Trilogy | Aspyr, 2006 | - |
| LEGO Batman: The Videogame | Feral, 2009 | - |
| LEGO Indiana Jones 2: The Adventure Continues | Feral, 2009 | - |
| LEGO Star Wars III: The Clone Wars | Feral, 2011 | ✓\* |
| LEGO Pirates of the Caribbean | TransGaming, 2011 | ✓\* |
| LEGO Batman 2: DC Super Heroes | Feral, 2012 | - |
| LEGO Harry Potter: Years 5-7 | Feral, 2012 | - |
| LEGO Marvel Super Heroes | Feral, 2014 | Experimental |
| LEGO Batman 3: Beyond Gotham | Feral, 2014 | - |
| The LEGO Movie Videogame | Feral, 2014 | - |

\* Not verified with a 100% run yet, report any bugs or problems in the issues tab.

## Building

Requirements: macOS 11 or later, Xcode command-line tools (`xcode-select
--install`), Rosetta 2 on Apple Silicon (`softwareupdate --install-rosetta`),
and the original game application. Pirates additionally needs Python 3 (the
one that comes with the command-line tools is fine) and network access the
first time, to fetch the `unicorn` package.

```sh
cd native
make GAME=pirates   SOURCE_APP="/path/to/LEGO Pirates of the Caribbean.app" bundle
make GAME=clonewars SOURCE_APP="/path/to/LEGO Star Wars III.app"           bundle
make GAME=marvel    SOURCE_APP="/path/to/LEGO Marvel Super Heroes.app"     bundle
```

This compiles the loader and assembles a self-contained app in
`native/build/` (`LEGOPirates-Compat.app`, `LEGOCloneWars-Compat.app`, or
`LEGOMarvel-Compat.app`) with
the game's data, resources and Cg framework copied in. The original app is
only read. Open the built app from Finder or the Dock.

Pirates ships with a SecuROM-packed executable. The build recovers the plain
Mach-O from it automatically: `native/tools/unpack_securom.py` emulates the
packer's stub with Unicorn and writes `native/build/LEGOPirates.unpacked.macbin`
(the activation code is left intact, nothing is bypassed). The first build
creates a Python virtual environment in `native/build/venv` and installs
`unicorn` into it; to use an interpreter that already has `unicorn`, pass
`PYTHON=/path/to/python3`. Clone Wars and Marvel use their shipped binaries directly.

Marvel supports the Feral 1.0.1 i386 build (`LEGOMarvel.macbin`). Its bundle
includes the original x86_64 Steam API and uses normal Steam initialization;
keep Steam running with access to the game. The same loader detects each
title, with Marvel's thread and structure-return conventions kept in its
own profile. Building Marvel leaves the other compatibility apps in place.
Verified so far: menus, the opening sequence, and keyboard movement and
Hulk/Bruce Banner transformation in Sand Central Station. Checkpoint files
persist on disk; a bundled storage-library enumeration defect hid later slots
after relaunch. The shared loader now repairs that directory walker, and fresh
processes discover and fully read the existing saves. In-game resume and the
full campaign remain unverified.
The loader supplies the i386 character tables and Cg metadata queries needed
for Marvel's shader constants. On the first launch after this fix, it backs up
Marvel's `CachedShadersGL` folder alongside the original and rebuilds the cache;
this corrects black intro logos and missing brick meshes. Saves are unaffected.

The storage repair is selected by the SDK library's UUID and a SHA-256 match
of the entire defective routine, independently of the game profile. It keeps
the parent directory path intact during recursion; it does not invent slot
names or replace the SDK catalog. Only private process memory changes, and
unrecognized library builds are left intact. Run `make -C native
test-steam-storage-fix` for native filesystem regressions using temporary
fixtures (override `STEAM_STORAGE_LIBRARY` to select the affected dylib).
`LP32_STEAM_STORAGE_PROBE=1` on a compatibility app initializes its normal SDK,
enumerates and reads saves without running the game or issuing save writes,
and preserves the player's `last-run.log`.

Other targets: `make GAME=<game> promote-loader` replaces only the loader in
an existing bundle (no source app needed), `make icons` regenerates the Dock
icons from `native/icons/`, and `make` alone builds the loader and probes.

Saves and settings stay where the original games put them
(`~/Library/Application Support/...`). Controllers supported by the
GameController framework work through the games' Xbox 360 mapping, including
two-player co-op and controllers connected during play. Button prompts show PlayStation glyphs (✕ ○ □ △, L1/R1,
Select/Start) when the pad connected at launch is a DualShock/DualSense, and
Xbox glyphs otherwise; `LP32_BUTTON_GLYPHS=playstation|xbox` forces one.
Marvel currently uses its shipped Xbox controller mapping and prompts.

For silent testing, launch the bundle's executable with `LP32_MUTE_AUDIO=1`.

Marvel automatically records occasional frame hitches during normal play in
`~/Library/Logs/LEGOMarvelCompat/hitches-<pid>-<timestamp>.log`. Relaunch after
updating the loader to enable it. Each report contains 32 preceding frames,
the slow frame, and 8 following frames, with draw counts, CPU time in draws,
resource uploads, shader compilation, file I/O, waits and audio calls. It also
records the three slowest measured calls per frame (guest caller address,
vertex/fragment program IDs and draw count), plus recent slow worker calls.
Presentation time is split into work, drawable flush, and deliberate pacing.
These are CPU wall timings; they do not measure GPU execution or replay draws.
Steam storage calls are included in I/O attribution. Save requests, filenames,
byte counts, enumeration results and SDK return values also appear as
`compat32: save ...` lines in `last-run.log` for normal Finder/Dock launches
(stderr for terminal launches), capped at 1024 lines per session. These logs
do not contain save payloads and do not change storage behavior.

The recorder uses fixed memory and a background log writer, with no screenshot
capture or GPU readback. Reports trigger above 25 ms (or 1.5 times an intentional
frame cap, whichever is larger), with a five-second cooldown and a limit of 128
reports per session. Inactive frames do not trigger reports. A report finishes
after its following frames arrive; abrupt termination can lose the pending
report. `LP32_HITCH_MS=35` changes the threshold; `LP32_HITCH_LOG=0` disables it,
or set `LP32_HITCH_LOG` to an unused absolute file path to redirect it. Other
titles leave it disabled unless explicitly enabled.
`make -C native test-hitch-recorder` runs synthetic timing tests without launching a game.
This mutes only that process and does not change game settings or system volume.
