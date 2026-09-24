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
the cantina, with keyboard movement and the pause menu verified. Chapter 1,
Negotiations, also reaches its opening cutscene, first playable room and the
first door/enemy encounter.
Load Game now opens the empty-save list without crashing, and returning from
Negotiations to the cantina is verified. The reported key-up crash was a native
AppKit observer invoking a null handler: guest i386 event-monitor blocks now get
native wrappers that copy captures, translate event arguments/results and honor
removal. A regression test sends real AppKit key-up events through those wrappers.

The Steam controller resources now include the Sony DualShock 4 v2 (054c:09cc),
whose product ID was missing from the supplied v1 preset. The launcher recognized
the connected controller after installing the additional profile, but the game's
per-device control map remained empty. The HID bridge now fills that map from the
shipped profile when this device has no controls. With the real connected DS4's
HID elements and injected values, Start advanced past the input-device prompt,
the D-pad moved the menu selection, and Cross entered New Game and the cantina.
Input reports also release their temporary proxy after delivery, instead of
consuming a permanent object-table entry for every axis/button report. Physical
in-game controller play still needs verification.

Unprofiled gamepads that macOS exposes through GameController now use a single
virtual "Standard Gamepad" profile. The HID bridge presents that profile when
Saga enumerates the device, then forwards GameController's named buttons,
sticks, triggers and D-pad through Saga's existing per-device input callback.
The Menu button is latched between frames so a short press still reaches the
game. The original HID path remains in use for pads with publisher profiles.
The Xbox probe exposed two failures in that callback: it discarded Menu presses
until the controller reconnected, and it stored stick values in the 0..255
source range while Saga reads signed 16-bit stick controls. The semantic bridge
now also writes the mapped per-device control state after callback delivery,
including the game's signed stick range and Menu edge. A map and state layout
self-test covers the control slots and range endpoints. With both controllers
connected before startup, the DualShock selected player one, the saved game
loaded, and Xbox Menu joined player two without reconnecting. The user confirmed
the Xbox stick now moves player two in gameplay.

Verification uses muted probes on a secondary display and separate test
preferences. Title, main menu, Load Game, New Game, cantina movement, the pause
menu and the idle attract movie were rechecked after the event-delivery fix.
Save/reload, the full campaign, physical in-game controller play, audible mixing quality and
long-session stability have not been verified.
The first Save Game attempt after Negotiations stopped at an unhandled
`CFDateFormatterCreate` import, with an empty save file open. The bridge now
handles that call and both date-string creation variants. A focused bridge test
passes; saving and loading through the game still need an in-game retry.
An audit of the Steam executable found 234 linked Core Foundation function
imports. `make test-cf-import-coverage` now checks that each has a
dispatch case. The audit added the previously missing formatting, calendar,
localization, stream, network and callback functions, plus linked object and
numeric constants. Focused date/number/calendar and CFNetwork bridge tests
pass. The coverage check does not prove every guest argument or optional game
path works; the full save/load flow still needs an in-game test.

The Steam build's "From Screen-Res" setting uses the selected render-mode
ratio for the world camera, but its automatic HUD camera reads a separate
16:9 constant. The bridge changes that constant to the selected mode's
height divided by width and keeps the renderer's camera ratio at width
divided by height. The explicit 16:10 setting produces the same HUD overlap.
The stud icon is a 3D panel model, while the counter is text. Steam Saga's
`InitPanel` selects between fixed 4:3 and 16:9 model scales, with no 16:10
scale. A first attempt changed the scale outputs after presentation, but the
user's next 16:10 run showed the same HUD. The bridge now interpolates the
original scales from the selected render mode's aspect and updates both source
presets, which `InitPanel` copies into the 3D HUD transform. This targets the
shared transform rather than individual stud or heart coordinates. The next
16:10 screenshot showed that the stud and number now align, but the stud
still nearly touches the hearts. An original 4:3 gameplay image shows the
same narrow gap, so the remaining crowding comes from the game's original
row spacing. The bridge increases the score-to-heart row separation by one
third of its original span in the game's panel coordinates. This moves the
heart row and its player-two prompt together, and scales with the HUD at
each render resolution. No guest instruction is replaced.
`LP32_SCREEN_ASPECT_SELFTEST=1` checks the addresses and original code bytes.
The aspect data check validates the game's unmodified HUD and panel scales.
Earlier versions that changed the guest instructions armed delayed Carbon
callbacks and eventually crashed with signal 10. The data-only build stayed
responsive in the paused hub for over four hours with timer tracing enabled;
neither callback was armed. The test process later received SIGTERM from outside
the game. The exact guest check that scheduled those callbacks has not been
identified.

**Menu and gameplay frame rate.** Feral's MacDoze layer runs the game on a
`WinMain` thread and forwards window messages to the main thread with
`SendMessage`. Each call posts an application-defined `NSEvent` to `NSApp`, then
spins until the main thread's `-sendEvent:` override handles it. Current AppKit
returns such queued events from `-nextEventMatchingMask:` only on its next
display-cycle pass, about 16 ms later. This applies whether the event is posted
from the game thread or re-posted on the main thread. With two or three round
trips per frame, the title and menus were capped near 30–35 FPS. Startup
loading was far slower, the "10 FPS menu" users reported.

The bridge now hands those cross-thread wake-up events to a main-thread
run-loop source. When the main thread is in `-[NSApplication run]`'s default
wait, the source delivers them straight to `-[NSApp sendEvent:]`, the same
method AppKit would call. Modal and tracking run-loop modes keep AppKit's queue.
So do guests pumping `-nextEventMatchingMask:` themselves. Setting
`LP32_QUEUED_APP_EVENTS=1` restores the old path for comparison. Paired muted
runs on an M5 Pro at 1728×1117 with Enhanced Graphics measured:

| Scene (uncapped) | Queued events | Direct delivery |
|---|---:|---:|
| Title/main menu | 34–35 FPS | 75–79 FPS |
| Cantina gameplay | 29–30 FPS | 77–82 FPS |
| Reaching full speed after launch | about 8 s | about 4 s |

Those uncapped numbers show the bridge is no longer the limit. Shipped builds
cap presentation at 60 FPS, or the display's rate if lower, and measure a
steady 60 FPS in the menus and cantina. This is the same pacer the
`NSOpenGLView` titles use. The TT engine's simulation is frame-rate bound, and
the PC release is documented to break physics and puzzles above 60 FPS.
`LP32_MAX_FPS=<n>` overrides the cap for testing; 0 removes it. These are background runs, not
controlled benchmarks. `make test-app-events` checks ordering, prompt delivery
and guest-pump dequeuing without opening a window.

The attract movie that plays after idling on the title screen was black. Two
bugs caused it. First, guest Objective-C methods called through the legacy class
bridge returned only EAX, but i386 returns 8-byte values such as `CGSize` in
EDX:EAX. `FRLAVFMovie -naturalSize` therefore reported a 1280×0 movie, and the
decoder skipped every frame. Guest calls now return the full register pair for
8-byte result types, and `make test-import-return` checks it. Second, the first
decoded frame called the missing `CGColorSpaceCopyICCProfile` import, which
stopped the game thread. It is now bridged; the decoder uses the profile to
choose the Rec. 709 matrix. The movie plays in color at its native 30 FPS.
The later frozen frame had a separate cause: each decoded frame kept a guest
copy of its pixel data until an autorelease pool drained. After about 900
frames, the guest heap ran out and OpenGL kept uploading the same frame. Pixel
copies now release when the game unlocks each frame. A full 61-second muted
replay advanced through the formerly frozen section and returned to the title.

Complete Saga records frame hitches during normal play in
`~/Library/Logs/LEGOCompleteSagaSteamCompat/hitches-<pid>-<timestamp>.log`.
The recorder starts on launch, so an already running game cannot report earlier
frames. It captures frames over 25 ms, subject to the frame-cap threshold and
five-second cooldown described in the main README.

In a level 2 cutscene, single `glTexParameterf` calls took 26–90 ms, and one
frame spent 164 ms on GL state. The game sets texture filters and mip ranges
through `glTexParameterf` as well as `glTexParameteri`. The float path rescanned
every mip level on every call. The integer path cached its scan, but any texture
upload cleared that cache for all textures, and cutscene streaming uploads
constantly. Each texture now tracks its own upload generation, both paths share
the cache, and restating the maximum level on a clamped texture keeps the clamp
instead of undoing and reapplying it. `LP32_GL_TEXTURE_SELFTEST=1` checks the
retained clamp and that an upload to one texture leaves another's cache valid.
The later 161 ms frame spent 126 ms in 31 `glTexParameterf` calls. The bridge
now drops unchanged filter and mip-range writes after validating the texture,
so those calls do not force another driver state change. The texture self-test
checks this case; the in-game frame time still needs a replay after relaunch.

Native benchmarks showed these parameter changes and level queries cost
microseconds, even on textures in use and during uploads from a shared
context. The long calls were waiting on the bridge's own cache mutex.
`glDeleteTextures` scanned all 8,192 cache slots per deleted name while holding
it, so a large delete on the loader thread stalled the render thread. Deletion
now hashes to each entry and shifts the rest of its probe run back.

`SndDoImmediate` stop, flush, pause and resume, and `SndDisposeChannel`, no
longer wait for the channel worker. They return nothing, and one 7 ms call
waited behind queue setup. They still run in order with other commands on that
channel.

Hitch reports now list the five imports with the most total time in each slow
frame. They also classify `dispatch_semaphore_wait` and `usleep` as waits,
which keeps worker semaphore waits out of the worker-event list.

During the fade into the lobby, the main thread spent 50–100 ms per frame in
Feral's own mutex, polling a semaphore about 200 times a frame. The loader
thread held that mutex while reading level data and compiling ARB programs.
Compiles were about 10% of that time. The bridge's ARB rewriting costs 0.04 ms
per program; Apple's parser costs about 1 ms, and the Metal compile at first
draw is cached by macOS after the first run. An unload frame made 34,000
`pthread_setspecific`, 24,000 `pthread_equal`, 22,000 `operator delete`,
14,000 `CGLSetCurrentContext` and 12,000 `pthread_self` calls, at 300–470 ns
each through the named import chain. These, together with `malloc`, `free`,
`operator new` and `pthread_getspecific`, now have direct handlers. Restating
the current CGL context skips the host call.

The loader reads each shader as a separate file, `FeralShaders/<hash>_*.Idx9`,
with an `lstat` and `fread` apiece, while holding that engine lock. The first
cantina load after a reboot stalled 2.2 s (33 frames); an immediate reload of
the same transition stalled 0.6 s (3 frames). Individual `lstat` and `fread`
calls took 2–5 ms on a cold cache. At launch, a utility-priority thread now
reads `feralshaders/` (4,276 files, 17 MiB) and `game.dat` (625 MiB) from
`LEGOStarWarsSagaData` into the page cache. The diagnostic log reports the
file count and time. Set `LP32_NO_DATA_PREFETCH=1` to disable it.

With the data prefetched, the level 2 cutscene still stalled: the loader
compiled one vertex/fragment pair per 70 ms frame, and `glProgramStringARB`
accounted for only 4.5 ms of it. Feral translates each Direct3D shader to ARB
text at load time, building it with `std::string`. The bridge's `push_back`
and `append` grew capacity to exactly the new length and copied through two
temporary buffers, so building a string copied it again on every call.
Appends now write in place and capacity doubles as in libc++. Common string
methods have direct handlers. A gateway benchmark building a 20 KB string
fell from 1,480 ns to 220 ns per call. Hitch reports now also list
`wtotal` lines: per-import totals for all other threads during the slow frame.

A paired silent startup check with Enhanced Graphics enabled found nearly
18 million `strcmp` calls in the initial loading frames. Routing that import
directly to the same native implementation roughly halved the first 120-frame
interval. Gateway tests verify comparison results, including negative results
and high-bit bytes, for Pirates, Clone Wars, Marvel and Steam Saga.

The next clean level 2 cutscene log still showed 350,000–440,000 `strcmp`
calls on the loader thread in each 65–81 ms frame. Those calls spent 8.5–11 ms
in the native bridge while the render thread waited on the loader. `_strcmp`
now points to a 32-bit comparison routine in the guest bridge page, avoiding
the mode switch on each call. Static stubs, function pointers and dynamic
callbacks use the same routine. The import-return self-test checks signed
results and the static import destinations. With `LP32_BENCH_STRCMP=1`, a
repeated guest-call test on a matching 24-byte string measured 78–84 ns/call
through the new route and 145–146 ns/call through the native gateway. This
includes host-to-guest test overhead, so it is not a cutscene frame-time
measurement. Set `LP32_NO_GUEST_STRCMP=1` to restore the native bridge for
comparison. The running game still needs a relaunch before a cutscene replay
can measure the frame-time change.

`LP32_CAPTURE_TRIGGER=/path` makes the CGL path dump the next presented frame to
`/path.ppm` after `touch /path`, for checking screens without screen-recording
access.

The shared compatibility runtime now supplies the i386 libc++ string/stream and
thread-local storage layouts, fragile Objective-C callbacks, native dispatch,
Steam interfaces, CGL functions and removed Sound Manager calls used by this
release. Sound Manager PCM channels play through one in-process mixer and
preserve queued completion callbacks when muted. Temporary Cocoa objects are reclaimed
at guest autorelease-pool boundaries; a stress test covers 100,000 temporary
dates without exhausting the proxy table.

The i386 `ExtSoundHeader` path now describes its PCM samples as little-endian.
The previous bridge marked them big-endian, turning the game's decoded 16-bit
audio into static. Offline buffer captures confirmed the byte-order mismatch;
device-free regression tests check actual queue formats and sample bytes.
Compressed-header PCM retains its explicit `sowt`/`twos` byte order. Muted queues
receive silence bytes as well as zero gain, while retaining playback timing.
Short Sound Manager buffers now notify the guest after AudioQueue returns their
final buffer for reuse. Earlier playback notifications recycled menu voices
before the queued samples had passed through AudioQueue. Playback
and parameter updates enter the channel's serial worker in order, while getters
wait for prior updates. This keeps AudioQueue setup off the render thread. In a
normal hub run before that change, recurring 27–40 ms frames contained
18–31 ms Sound Manager calls. The same idle hub produced no over-25 ms hitch
triggers during a 36-second run after the change. Startup and save-load waits
are separate from these audio hitches.

The continuous title music consists of separate PCM segments. The bridge used
to stop and drain the AudioQueue after every segment before calling the guest
completion handler. A live trace measured about 107 ms between the final
buffer callback and the next segment's submission on each 4-second segment.
The bridge now calls the completion handler when that final buffer is reusable,
allowing the next segment into the running queue. Short effects still drain
before notifying the game. In the same title sequence, the handoff took less
than 0.2 ms. Device-free tests check both the gapless music handoff and the
short-effect drain.

Music streaming also reads guest PCM progressively, using two approximately
20 ms queue buffers. Copying an entire multi-second buffer at submission was
too early: the game refills its later portions during playback, so the old
bridge played stale passages and silence. A complete 84-second title-music
capture now matches the source waveform on both channels, within decoder
rounding. The device-free regression test changes upcoming samples after
submission and verifies fresh data and one completion per original command.

The paragraphs above describe the earlier AudioQueue design, one queue per
channel. In the Mos Eisley cantina, breaking a chair plays dozens of stud and
lightsaber sounds at once, and the lobby music broke up. A device stress test
reproduced it: AudioQueue serializes calls through one connection to the
system audio service. With 40 effect channels bursting, their stops averaged
515 ms and starts 197 ms, and the music channel's buffer enqueues blocked
behind them for up to 435 ms, well past its 40 ms of queued audio.

All Sound Manager channels now mix into one default output unit. The render
callback reads guest PCM at render time, with linear interpolation for rate
and multiplier changes. Playing, stopping and pausing a sound change mixer state
only; guest callbacks still run on each channel's serial worker, triggered
by a notifier thread. A short buffer completes after its final sample is
mixed. A buffer of at least 0.75 s completes 100 ms before its end, after its
remaining samples are copied, so the guest can queue the next segment and
refill the reported one without a gap. In the same stress test, 1 s and 3 s
music segments completed within 2 ms of their exact lengths during effect
bursts, and all 800 effect completions arrived. `make test-sound-manager-pcm`
renders offline to check decoding, volume, rate, live data, the copied tail,
gapless handoff, completion timing, `quietCmd` and 48 simultaneous voices.

Native proxy handles have an atomic lookup path, avoiding the shared ownership
mutex during high-frequency semaphore polling. Recyclable owned handles clear
the published pointer before releasing or recycling their slot; callers must
still own a valid reference. Autoreleased objects retain their lifetime checks.

The AVFoundation movie path bridges asynchronous completion blocks, counted
object arrays, CMTime structures, Core Media sample/pixel buffers, and i386
AudioQueue buffers/callbacks. Device tests enforce zero queue gain even when the
guest changes volume.

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
The Sound Manager bridge currently supports PCM buffers, with up to 1,024
channels open at once. Disposed channel contexts remain allocated until exit
because worker blocks can still reference them. Compressed codecs and graceful
draining on channel disposal remain incomplete.

## Validation

```sh
make test-macho-entry test-game-profile test-resource-bridge test-stl-bridge test-rtti-bridge \
    test-cfnetwork-bridge test-libcpp-bridge test-libcpp-stream-bridge \
    test-tlv-bridge test-gl-core-bridge test-sound-manager-bridge \
    test-sound-manager-pcm test-dispatch-fast test-silent-bundle \
    test-app-events test-import-return
make GAME=saga SAGA_EDITION=retail test-carbon-geometry
```

These cover profile isolation, resource parsing and handles, STL tree/list
invariants, runtime casts, HTTP messages, stream and timer callbacks with balanced
context ownership, Carbon coordinate conventions, and alert/command-event ABI
conversion. Heap, file and thread self-tests also pass with the Pirates, Clone
Wars, Marvel and Complete Saga images. They do not replace playtesting.

Use a separate test copy of the generated bundle with the **boolean**
`LP32SilentTest` key set to true in its `Contents/Info.plist`, then re-sign that
copy. The loader enforces `LP32_MUTE_AUDIO=1 LP32_BACKGROUND_TEST=1` before guest
initialization, including launches through Finder/Launch Services that omit the
shell environment. Verify with `LP32_SILENT_TEST_SELFTEST=1` when invoking that
copy's executable directly; it exits before loading a game or opening audio.
`make test-silent-bundle` tests missing and explicitly zero environment values.
Keep this flag out of normal release bundles. Set
`LP32_TEST_DISPLAY` to an active Core Graphics display ID to keep the launcher
and attached sheets on that display without repeatedly stealing focus. The
AppKit frontend enforces this placement during its modal loop.

`LP32_TEST_HOME_DIR` redirects the game's home-directory preferences and local
save paths; it does not redirect the publisher's shared license store or native system
preferences. Use an external timeout: `LP32_TEST_EXIT_AFTER_SECONDS` depends on
the older OpenGLView presentation path and does not stop the Steam CGL game
or a modal Carbon launcher. For isolated Steam probes, also set
`LP32_STEAM_TEST_DATA_DIR` and `LP32_APPLICATION_SUPPORT_DIR` to test directories.
The former overrides Steam's user-data-folder query, not genuine Steam Remote
Storage/cloud operations; these flags do not guarantee cloud-save isolation.
`LP32_TEST_KEY_FIFO` accepts `keycode hold_seconds` lines for targeted keyboard
tests through both the NSOpenGLView and CGL presentation paths. It updates the
guest's Carbon and Core Graphics key polls as well as posting an AppKit event.

`LP32_TRACE_GL_FRAMES=1` reports a presentation counter, monotonic `end` time,
viewport and actual swap interval every 120 CGL frames. `max_ms` is the longest
presentation interval in that reporting window; `slow50` counts intervals over
50 ms. Compute FPS from counter differences divided by time differences, and
keep menu, loading, paused and gameplay measurements separate. For short CPU
investigations, also set `LP32_FRAME_STATS` and
`LP32_FRAME_STATS_IMPORTS` to report import costs at those boundaries. Detailed
profiling affects performance; worker wait times can overlap, so do not add them
to infer frame time or use profiled runs as gameplay FPS measurements.

`LP32_TRACE_SOUND=1` records at most 2,048 Sound Manager events per launch:
command enqueue/execution, PCM buffer duration and buffer completion, with
monotonic timestamps and channel/generation IDs. It does not record audio data.
It can identify explicit stop commands and timing failures; correct durations
alone do not establish that the PCM contents are correct.

`LP32_TRACE_NETWORK=1` logs hostnames, stream events and HTTP status codes, never
URL paths, queries, headers or bodies. `LP32_TRACE_FILES=2` includes Carbon path
lookups and can be noisy. Keep detailed Carbon and filesystem tracing to short
probes.
