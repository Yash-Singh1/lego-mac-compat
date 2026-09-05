# 32-bit Mac games on modern macOS

A native loader that runs original 32-bit Intel Mac games on
current macOS (Apple Silicon through Rosetta 2, or Intel). It maps the
game's i386 executable into a 64-bit process, switches into a 32-bit code
segment to run it, and bridges the OS, OpenGL, Cg, CoreAudio and controller
interfaces the game expects. No SIP changes, kernel extensions, VMs or Wine.
You need your own copy of the game.

Supported and candidate games:

| Game | Mac release | Status |
| --- | --- | --- |
| LEGO Star Wars II: The Original Trilogy | Aspyr, 2006 | - |
| LEGO Batman: The Videogame | Feral, 2009 | - |
| LEGO Indiana Jones 2: The Adventure Continues | Feral, 2009 | - |
| LEGO Star Wars III: The Clone Wars | Feral, 2011 | ✓\* |
| LEGO Pirates of the Caribbean | TransGaming, 2011 | ✓\* |
| LEGO Batman 2: DC Super Heroes | Feral, 2012 | - |
| LEGO Harry Potter: Years 5-7 | Feral, 2012 | - |
| LEGO Batman 3: Beyond Gotham | Feral, 2014 | - |
| The LEGO Movie Videogame | Feral, 2014 | - |
| Portal 2 | Valve, 2011 | Experimental: main menu and first level render; gameplay checks ongoing |

\* Not verified with a 100% run yet, report any bugs or problems in the issues tab.

## Building

Requirements: macOS 11 or later, Xcode command-line tools (`xcode-select
--install`), Rosetta 2 on Apple Silicon (`softwareupdate --install-rosetta`),
and the original game application. Pirates additionally needs Python 3 (the
one that comes with the command-line tools is fine) and network access the
first time, to fetch the `unicorn` package. Portal 2 needs Python 3 and a
download of Apple's legacy C++ runtime, described below.

```sh
cd native
make GAME=pirates   SOURCE_APP="/path/to/LEGO Pirates of the Caribbean.app" bundle
make GAME=clonewars SOURCE_APP="/path/to/LEGO Star Wars III.app"           bundle
make portal2-runtime
make GAME=portal2   SOURCE_APP="/path/to/Portal 2.app"   bundle
```

This compiles the loader and assembles a self-contained app in
`native/build/` (`LEGOPirates-Compat.app`, `LEGOCloneWars-Compat.app`) with
the game's data, resources and Cg framework copied in. The original app is
only read. Open the built app from Finder or the Dock. Portal 2 builds
`Portal2-Compat.app`; its current limitations and development checks are
documented in [native/PORTAL2.md](native/PORTAL2.md).

Pirates ships with a SecuROM-packed executable. The build recovers the plain
Mach-O from it automatically: `native/tools/unpack_securom.py` emulates the
packer's stub with Unicorn and writes `native/build/LEGOPirates.unpacked.macbin`
(the activation code is left intact, nothing is bypassed). The first build
creates a Python virtual environment in `native/build/venv` and installs
`unicorn` into it; to use an interpreter that already has `unicorn`, pass
`PYTHON=/path/to/python3`. Clone Wars uses its shipped binary directly.

Other targets: `make GAME=<game> promote-loader` replaces only the loader in
an existing bundle (no source app needed), `make icons` regenerates the Dock
icons from `native/icons/`, and `make` alone builds the loader and probes.

Saves and settings stay where the original games put them
(`~/Library/Application Support/...`). For the LEGO profiles, controllers supported by the
GameController framework work through the games' Xbox 360 mapping, including
two-player co-op and controllers connected during play. Button prompts show PlayStation glyphs (✕ ○ □ △, L1/R1,
Select/Start) when the pad connected at launch is a DualShock/DualSense, and
Xbox glyphs otherwise; `LP32_BUTTON_GLYPHS=playstation|xbox` forces one.
