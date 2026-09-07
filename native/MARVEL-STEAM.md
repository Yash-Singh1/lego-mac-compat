# Marvel: genuine Steam build

Build from an installed Steam copy, with Steam running and access to app 249130:

```sh
make -C native GAME=marvel \
  SOURCE_APP="/path/to/steamapps/common/LEGO Marvel Super Heroes/LEGO Marvel Super Heroes.app" \
  BUNDLE=build/LEGOMarvel-Steam-Compat.app bundle
```

Open `native/build/LEGOMarvel-Steam-Compat.app`. The explicit output path keeps
the older compatibility bundle separate; the source installation is only read.
Use the same `SOURCE_APP` and `BUNDLE` overrides with `promote-loader` when
updating only the runtime. No automatic save migration is performed.

## What was wrong

The earlier source's `libsteam_api.dylib` identifies itself as a Steam emulator.
Its successful initialization and local file storage were not evidence of a
connection to Steam. The genuine Steam executable is byte-identical to the
supported Feral 1.0.1 executable, so its guest code does not need a new profile.

There was also a real startup bug independent of the library replacement:
Feral changes CWD to `Contents/Resources` before Steam startup, while the copied
`steam_appid.txt` is in `Contents/MacOS`. The SDK reads that file relative to
CWD. With neither that file nor `SteamAppId` available, it requests a relaunch;
Marvel skips `SteamAPI_Init` but continues running. Its achievement submitter
then dereferences a NULL stats pointer at `0x249374`.

The loader now supplies `SteamAppId=249130` from the recognized Marvel profile
before guest initializers run. The official bundled SDK supports this identity
mechanism. A conflicting existing ID is rejected rather than silently changing
the game associated with a Steam session. Steam initialization, account access,
relaunch decisions, and achievement results are still handled by the library.
The separate NULL guard remains a safety net when stats are unavailable; it
does not queue missed achievements or report them as uploaded.

## Verified on 2026-09-07

- Steam completed installation of build 329428, depots 249132 and 249133.
- Source and bundled game executable SHA-256:
  `258e5a18769722375b27a1663cc57fbf66b38683c68af5c59782b682a52e18b8`.
- Source and bundled official Steam dylib SHA-256:
  `c53ba7add8ec967777927486b999870db571aa4d47629abb6507b9916fefb9e5`.
- Strict recursive bundle-signature verification passes.
- A bounded launch from `/tmp`, with `SteamAppId` absent from the incoming
  environment, automatically sets the identity and reports:
  `SteamAPI_Init succeeded`, `RequestCurrentStats accepted`, and
  `UserStatsReceived game=249130 result=1` (success).
- That real callback reaches the original guest achievement handler, which
  issues 48 `GetAchievement` calls. Rendering starts and the timed smoke test
  exits successfully without the reported NULL crash.
- `test-steam-bridge` covers identity setup/conflicts, actual forwarding of
  success/failure for RequestCurrentStats/SetAchievement/StoreStats using
  mocks, packed callback payloads, SteamID return conventions, and storage.
- `test-steam-achievement-guard` executes the mapped guest routine with NULL,
  disabled, working, and failing mock interfaces. Its `unpatched` negative
  control reproduces the original fault at exactly `0x249374`.

No achievement was manually unlocked or cleared. A newly earned achievement's
server upload is not yet verified. The bridge logs SetAchievement and StoreStats
results, plus UserStatsStored callbacks, to make that next gameplay test
observable. Successful SetAchievement alone is not an upload acknowledgement.
Steam Cloud save/reload and full gameplay with the genuine library remain
untested. GUI capture was unavailable, so this run does not claim a visual
inspection of the rendered scene.
