# TFU Converter

This is the TFU adaptation of the converter from `portal2` commit `a06456f`.
It retains the AppKit drag-and-drop UI, cancellable conversion engine, verified
Apple runtime download/cache, and atomic publication without overwriting an
existing converted app. The original Portal 2 converter is left unchanged.

## Build and use

```sh
make -C native GAME=tfu tfu-converter
open native/build/TFU-Converter.app
```

Drop your Mac game app or choose its Steam game folder. Choose where to create
`TFU-Compat.app`. The converter accepts:

- Steam's Mac app with `Contents/GameData` (tested with Mac 1.3.0).
- Retail Mac apps with an `Assets` folder beside them (tested with Mac 1.2).
- The containing game folder, `common`, `steamapps`, or Steam library folder.

Windows executables cannot be converted. Other Mac distributions are accepted
based on their Mach-O executable and data layout, without a store/version/hash
allowlist. Before downloading or copying, the bundled loader must uniquely
identify the required code routines. Missing or ambiguous signatures produce an
error, rather than applying patches to guessed addresses. This does not promise
compatibility with every untested build or installer/DRM wrapper.

The converter is universal arm64/x86_64; the game runtime runs as x86_64 and needs
Rosetta on Apple silicon. macOS 11 or later is required. End users do not need
Xcode, Make, Python or this repository. The converter contains no game assets or
Apple libraries. The first conversion downloads Apple's 4.72 GB Lion archive,
verifies its SHA-256, and extracts two i386 C++ libraries into a private cache.
Later conversions reuse the verified libraries. Nothing is installed systemwide.

Conversion creates a separate app and leaves the original installation and
existing converted copies untouched. TFU uses its distribution's existing save location: retail uses
`~/Documents/Aspyr/Star Wars The Force Unleashed`, while Steam uses
`~/Library/Application Support/Star Wars The Force Unleashed`. No save is bundled,
migrated, overwritten, or deleted by the converter. Copied game files cannot be symlinks to external source files.

Steam and retail also use separate preferences (`com.aspyr.swtfu.steam` and
`com.aspyr.SWTFU`). Copying a save does not transfer resolution, graphics, or
controller mode. Check those in Game Settings when switching distributions.
The original Steam Mac app has no Steam API library. For Steam builds, the
compatibility runtime connects its own game process to the installed, running
Steam client using Valve's versioned SteamClient020 interface and verifies TFU's app ID (32430). Finder launches therefore show Running/Stop and play time
in Steam. No Steam binaries are shipped or copied from other games. Retail
builds and headless/background tests skip this connection. If Steam is closed
or its required interface is unavailable, the game still launches; start Steam
before launching TFU to enable tracking. Missing Steam is logged without a
popup or launch block; `make -C native test-tfu-steam` covers this fallback,
retail copies, and diagnostic runs without stopping the real Steam client.
This does not add achievements or
overlay integration, or redirect Steam's Play button to the converted bundle.

The same engine has a command-line entry point:

```sh
native/build/TFU-Converter.app/Contents/MacOS/TFUConverter --convert \
  "$HOME/Library/Application Support/Steam/steamapps/common/Star Wars The Force Unleashed" \
  "$HOME/Applications"
```

`--cache DIRECTORY` optionally selects a download cache. Cancel, Ctrl-C, or
SIGTERM removes the unfinished conversion. Concurrent conversions serialize
runtime preparation and publish to distinct names (`TFU-Compat 2.app`, etc.).

## Developer bundle builder

```sh
make -C native GAME=tfu tfu-runtime
make -C native GAME=tfu bundle BUNDLE=build/TFU-Steam.app
# Or select a retail/CD installation:
make -C native GAME=tfu bundle SOURCE_APP='/path/to/Star Wars The Force Unleashed.app'
```

The default source prefers the local Steam Mac installation. `ASSETS_DIR` can
override the asset directory for a retail installation. The Makefile and GUI use
the same Swift source discovery/copy code, and both run the loader's read-only
`--validate-tfu IMAGE` preflight before bundling.

## Checks

```sh
make -C native GAME=tfu test-tfu-converter test-tfu-layout test-tfu-movie-end
LP32_CONTINUE_BUNDLE="$PWD/native/build/TFU-Steam.app" \
LP32_TEST_LOADER="$PWD/native/build/TFU-Steam.app/Contents/MacOS/TFUCompat" \
  python3 native/tests/test_tfu_continue.py
```

The Continue harness copies the local checkpoint and isolates its preferences
and user-data directory. The layout test requires local game images and tests
both arbitrary relocation and rejection of damaged/duplicate routines. See
`../TFU.md` for other runtime diagnostics.

Validated locally on September 9, 2026 with Steam Mac 1.3.0 and retail Mac 1.2:
conversion, strict bundle signature verification, intro playback, and continuing
a copied checkpoint into gameplay. The Steam launcher was also checked in
windowed and fullscreen modes, and the converter completed a Steam conversion
through its GUI. Controller and F/right-mouse Grip alias self-tests passed for
both executables. Original save files remained unchanged.

The Steam keybinding dialog uses AppKit controls populated from the original
Carbon NIB and calls the game's own binding handlers. UI checks covered Cancel,
Escape, repeated reopening, keyboard and primary/secondary mouse capture,
discarding edits, saving/reopening an edit, and Defaults confirmation. The
standard alerts also use AppKit. The `initWithWindowRef:` adapter preserves init
ownership so releasing a dialog does not invalidate its persistent handle.
Other distribution builds have not been tested; signature discovery rejects
unsupported executable layouts safely.

The Carbon event pump installs the native quit handler and services pending
Apple events, including external quit requests while TFU is backgrounded.
`make -C native test-app-termination` sends a real quit Apple event to an
isolated test process and verifies the production handler exits. The Steam
build's C `_Exit` import is also handled by the process-exit bridge.
