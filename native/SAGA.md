# LEGO Star Wars: The Complete Saga

## Steam build (default)

The pipeline packages the Steam 1.2.1 RC4 release (build 124676.25672):

```sh
cd native
make GAME=saga SOURCE_APP="/path/to/LEGO Star Wars Saga.app" bundle
```

`SOURCE_APP` is required; the pipeline does not assume a Steam library location.
Data comes from the supplied app's sibling `LEGOStarWarsSagaData` directory; override it with
`SAGA_DATA_SOURCE=/path/to/LEGOStarWarsSagaData` if necessary. Required archives,
audio, movies, shaders and support files are checked before packaging.

Output: `build/LEGOCompleteSaga-Steam-Compat.app` plus the sibling
`build/LEGOStarWarsSagaData`. Keep these together. The build stages clean
directories before replacing its generated output, preserving the Steam source,
the retail compatibility bundle, and all other games' bundles. No retail ZIP,
mounted DVD image or retail product key is used by this build.

The publisher's executable is copied unchanged. Its Steam SDK's x86_64 slice is
staged and signed for the native bridge; modern signing rejects the original
SDK's i386 slice. The source framework's missing QuincyKit version symlinks are
restored in the output. `steam_appid.txt` contains public app ID 32440; the
publisher's Steam SDK retains initialization and entitlement checks.

**Runtime status:** the Steam release reaches the original launcher, initializes
Steam successfully, renders the title and main menu, and starts a new game in
the cantina, with keyboard movement and the pause menu verified. Verification
uses short muted probes on a secondary display, with
separate test preferences and storage. Save/reload, the full campaign, controllers, audible mixing quality and
long-session stability have not been verified.

The shared compatibility runtime now supplies the i386 libc++ string/stream and
thread-local storage layouts, fragile Objective-C callbacks, native dispatch,
Steam interfaces, CGL functions and removed Sound Manager calls used by this
release. Sound Manager PCM channels use native AudioQueue playback and preserve
queued completion callbacks when muted. Temporary Cocoa objects are reclaimed
at guest autorelease-pool boundaries; a stress test covers 100,000 temporary
dates without exhausting the proxy table.

The loader supports both `LC_MAIN` and `LC_UNIXTHREAD`, binds indirect symbols
defined inside the image to their guest implementations, and keeps Steam in its
own profile. Steam receives no retail or other-title address patches. Existing
import thunk addresses and file/audio/controller token ranges remain unchanged;
extra thunks use a separate reserved region.

Validation includes strict bundle signing, original executable/data copy
verification, ABI bridge tests, and heap/file/thread checks against Steam,
retail 1.1.1, Pirates, Clone Wars and Marvel. These checks protect shared runtime
behavior; they do not substitute for playing through those games.

## Retail build (optional)

The Feral 1.1.1 update accepts and saves a product key through
its original launcher handlers, and its activation request reaches Feral's
servers. The available key received a maximum-activations response. Disc-free
launch and gameplay have **not** been verified; the bundle is not yet a playable
port. A successful HTTP response or the existence of `Licence.txt` does not
establish successful activation.

```sh
cd native
make GAME=saga SAGA_EDITION=retail SAGA_UPDATE_ZIP="/path/to/legostarwarssaga_111_update.zip" \
    SAGA_DATA_SOURCE="/path/to/Data" bundle
```

The ZIP is extracted under `build/saga-update`. The compatibility app is
`build/LEGOCompleteSaga-Compat.app`. The original app and the other titles'
bundles are not modified. For an already updated application, pass
`SAGA_EDITION=retail SAGA_UPDATE_ZIP= SOURCE_APP="/path/to/LEGO Star Wars Saga.app"`.

The update contains launcher resources, not the full game data. The original
`SagaData.dmg` contains a `Data` directory with `GAME.DAT`, six episode archives,
audio and movies. Mount that image read-only and pass its Data directory as
`SAGA_DATA_SOURCE`. The build copies the data locally into `build/Data` and
checks the required files. Keep the app and Data directory beside each other.
Once staged, subsequent builds reuse `build/Data` by default.

Both the original 1.0 R17 image and the 1.1.1 RC4 image have executable
fingerprints. The update's readme describes product-key activation for disc-free
play. The original image requires its DVD. The loader preserves these checks;
a disk image is not reported as a physical DVD. No product key belongs in source
files, build recipes, or logs. The publisher stores its serial and license files
under `/Users/Shared/Feral Interactive/LEGO Star Wars Saga`.

## Compatibility work

The shared loader handles the larger image/import table, GCC string and STL
operations, i386 runtime casts, Resource Manager data, Carbon references and
callbacks, CFNetwork HTTP streams, and native run loops. An AppKit frontend
presents Carbon controls and forwards text changes and button commands to the
guest's original handlers. Sheets stay attached to their parent window.

The guest requests noncomposited Carbon windows and uses window-port control
coordinates. The bridge translates these to the parent-relative coordinates
required by modern composited windows. Games requesting compositing retain
parent-relative behavior. Command-event records, retained EventRefs, window
resize limits and standard-alert records also need explicit ABI conversion.

Signal callbacks that require asynchronous guest contexts currently return
`SIG_ERR`/`ENOTSUP`, leaving the host handler intact. The crypto library's CPU
probe takes its normal portable fallback. Guest C++ exception unwinding remains
unsupported; normal stream destructors can query `std::uncaught_exception()`.
The QuickTime bridge uses AVFoundation/ImageIO; its retail movie/AGL path
remains unverified. Some retail Carbon launcher controls remain incomplete.
The Sound Manager bridge currently supports PCM buffers, with a process limit
of 1,024 channel creations; compressed codecs and graceful draining on channel
disposal remain incomplete.

## Validation

```sh
make test-macho-entry test-game-profile test-resource-bridge test-stl-bridge test-rtti-bridge \
    test-cfnetwork-bridge test-libcpp-bridge test-libcpp-stream-bridge \
    test-tlv-bridge test-gl-core-bridge test-sound-manager-bridge
make GAME=saga SAGA_EDITION=retail test-carbon-geometry
```

These cover profile isolation, resource parsing and handles, STL tree/list
invariants, runtime casts, HTTP messages, stream and timer callbacks with balanced
context ownership, Carbon coordinate conventions, and alert/command-event ABI
conversion. Heap, file and thread self-tests also pass with the Pirates, Clone
Wars, Marvel and Complete Saga images. They do not replace playtesting.

Use `LP32_MUTE_AUDIO=1 LP32_BACKGROUND_TEST=1` for probes. Set
`LP32_TEST_DISPLAY` to an active Core Graphics display ID to keep the launcher
and attached sheets on that display without repeatedly stealing focus. The
AppKit frontend enforces this placement during its modal loop.

`LP32_TEST_HOME_DIR` isolates the game's home-directory preferences and saves;
it does not redirect the publisher's shared license store or native system
preferences. Use an external timeout: `LP32_TEST_EXIT_AFTER_SECONDS` depends on
the older OpenGLView presentation path and does not stop the Steam CGL game
or a modal Carbon launcher. For isolated Steam probes, also set
`LP32_STEAM_TEST_DATA_DIR` and `LP32_APPLICATION_SUPPORT_DIR` to test directories.
`LP32_TEST_KEY_FIFO` accepts `keycode hold_seconds` lines for targeted background
keyboard tests without sending input to other applications.

`LP32_TRACE_NETWORK=1` logs hostnames, stream events and HTTP status codes, never
URL paths, queries, headers or bodies. `LP32_TRACE_FILES=2` includes Carbon path
lookups and can be noisy. Keep detailed Carbon and filesystem tracing to short
probes.
