# Portal 2 Converter development

`make portal2-converter` builds a universal AppKit application with a small
drag-and-drop window. `App.swift` owns the UI and runs `Conversion.swift` on a
worker queue. Cancellation stops the current child process and removes only
the operation's private staging directory. Quitting waits for that cleanup.
There is no shell-interpolated command execution and no developer-tool
dependency in the shipped app. Apple's supplied macOS tools perform copying,
archive extraction and code signing; Swift handles SHA-256 and i386 thinning.

`Conversion.swift` follows the existing `GAME=portal2 bundle` recipe and
`tools/prepare_portal2_runtime.py`. Keep the URL, archive/library digests and
bundle layout in sync with those paths. It intentionally uses the stock
loader and Rosetta configuration, including the existing performance fixes.

Run the independent regression checks with:

```sh
make test-portal2-converter
```

The built converter also exposes the same engine without opening any GUI:

```sh
build/Portal2-Converter.app/Contents/MacOS/Portal2Converter \
  --convert "/path/to/Portal 2.app" "/path/to/output folder"
```

An optional `--cache /path/to/cache` isolates integration tests from the
user's normal cache. To exercise extraction without downloading another
4.72 GB, put the verified Lion download there as `InstallMacOSX-Lion.dmg`.
The code still checks its SHA-256 and extracts both libraries. SIGINT/SIGTERM
use the same cancellation mechanism as the window's Cancel button.

For the full test with a real game and the existing download (Python is only
needed to run this development test):

```sh
python3 tests/test_portal2_converter_integration.py \
  --source "/path/to/Portal 2.app" \
  --installer build/runtime-downloads/InstallMacOSX-Lion.dmg
```

This test builds in a temporary folder under `build/`, checks the produced
bundle and loader without launching the game, repeats with cached libraries,
cancels a copy, and checks that the source and previous output are untouched.
It removes its own test files afterward. It does not automate the UI. Manual
UI checks: window and Dock-icon drops, Choose Game, folder-picker dismissal,
Cancel during download/copy, Quit during conversion, error recovery, Open
Game and Show in Finder. A public release needs Developer ID signing and
notarization separately from the local ad-hoc build target.
