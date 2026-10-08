# MW3 performance measurements

MW3 shares MW2's runtime, so MW2's render-thread and dispatch optimizations
(see [MW2-PERFORMANCE.md](MW2-PERFORMANCE.md)) apply to it unchanged. MW2's
byte-checked code patches are address-specific, so the MW3 single-player and
multiplayer executables each have their own copy at their Steam addresses.

## Measurement

`build/mw3-performance/bench.py LABEL [--sample]` copies the user's MW3 settings
and save into a private home, presses Return until Resume Game starts loading,
and reports the mean frame rate for 5 to 55 seconds of gameplay after the
briefing video. The benchmark ran at 1920x1200, 2x MSAA, vsync off, resuming
the opening of `sp_ny_manhattan`. Run-to-run noise is about ±4 FPS.
`LP32_NO_CODE_PATCHES=1` disables a title's code patches for A/B runs.

| Build | Gameplay FPS |
| --- | --- |
| No code patches | 33.5, 35.4 |
| MW2-equivalent patches | 40.8, 48.8, 45.3 |
| Plus render-thread sleep removal | 43.9, 45.9, 44.3, 43.2 |

## Patches

- `ASLCriticalSection::Lock` and `EnterCriticalSection` block on contended
  locks instead of spinning on cross-ABI `pthread_mutex_trylock`.
- `EventHANDLE::WaitForSingleObject` treats a 1 ms timeout as a timed wait,
  not a poll.
- `Load_CreateMaterialPixelShader`/`VertexShader` no longer `Sys_Sleep(1)`
  during shader creation.
- `RB_EndFrame` no longer `Sys_Sleep(1)` before every present. This removed
  the render thread's ~3.7% of samples in `nanosleep`. The change is below the
  benchmark's noise; the swap fence still limits frames in flight.

Before the patches, the game and worker threads spent about half their time
blocked in or waking from contended mutexes. Afterwards they mostly wait for
the render thread.

## Mission loading

`LP32_TRACE_GUEST_MARKS=addr[,addr...]` logs the first call of each listed
guest function with a timestamp, then runs it unmodified. With markers on
`SV_SpawnServer` (`0x3821ec`), `SV_InitGameProgs` (`0x381cb5`) and
`CL_SetActive` (`0x1c10d4`), `bench.py` reports `load_seconds` (Resume
Game "Yes" to playable) and `zone_seconds` (fastfile loading). The mission
intro video that follows is not included. `BENCH_SAMPLE_ON_MARK=0x3821ec`
samples the load.

| Build | Load time (s) |
| --- | --- |
| No code patches | 32.8 (zone loading 29.4) |
| Code patches, blocking locks | 10.1, 10.8, 9.2 |
| Code patches, brief lock spin | 9.4, 8.3, 9.0 |

The shader-creation `Sys_Sleep(1)` patches remove most of the difference: the
loader slept after every 10 ms of creating its ~1,700 GLSL shaders.

During the load, the `Database` thread does the work. The loading-video thread
polls a GPU query about 580 times a second and `R_SyncGpu` polls
`TestCooperativeLevel`, both under the D3D device lock. Each holds it only
briefly, but the loader lost ~15% of its time to block/wake handoffs. For MW2
and MW3, a contended guest `pthread_mutex_lock` now retries for up to 16 µs
before blocking. `LP32_NO_MUTEX_SPIN=1` disables it. Gameplay frame rates were
within noise either way (machine shared with other workloads).

The rest of the loader thread is translated game code (~40%) and Apple's GLSL
compiler (~23%). The 1,685 shaders compiled during a load are all distinct
(4 duplicates). Each compile is followed at once by a status query and a
program link, so compiling off-thread would require faking results.

## Remaining cost

The render (`Backend`) thread is saturated. It spends about 35% in Apple's GL
driver for ~3,100 `glDrawRangeElements` per frame, about 25% in translated game
code and about 13% in the Rosetta runtime. The D3D device lock (`028ba6e0`,
taken by worker vertex/index buffer locks and `TestCooperativeLevel`) is 95%
contended but costs the render thread about 4%. The per-draw index shadow
queries (`CGLGetShareGroup`, `glGetIntegerv`) cost under 1%.

## Multiplayer status

MW3 multiplayer starts, and its menus work with the keyboard: Down/Up
highlights an item, then Return selects it. Play logs in to Steam and Demonware
(`mw3-mac-auth.prod.demonware.net`), connects to the Mac lobby
(`mw3-mac-lobby.prod.demonware.net`), resolves the STUN servers and exchanges
about 70 KB. Then the lobby server closes the connection, and after a retry the
game shows "The Call of Duty: Modern Warfare 3 server is not available at this
time". No anti-cheat challenge was answered before the disconnect, and the
`iw-*-reflector.cloudapp.net` hosts no longer resolve. This matches the
server-side error other players report, so online play is untested.
`LP32_TRACE_SOCKETS[=lines]` logs lookups, connects, packet sizes and the
first bytes of TCP messages.
