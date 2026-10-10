# MW3 shader preparation experiment

Upfront shader preparation works for an extracted and captured subset of MW3.
It does not yet cover every shader or remove all gameplay stalls. This is a
local prototype, not an automatic converter step.

## Extraction

The installed single-player bundle contains 71 per-level recipe plists with
5,034 unique combinations of vertex shader UID, pixel shader UID and two
32-byte translation keys. These are translator inputs, not GLSL sources.
MW3 generates GLSL from DX9 bytecode and those keys at runtime.

`tools/extract_mw3_shaders.py` reads the bundle without launching it:

```sh
python3 native/tools/extract_mw3_shaders.py \
  --app /path/to/MW3-Compat.app --output /path/to/local/extracted-assets
```

It merges the recipe lists, validates the original binary shader cache,
extracts missing records from unsigned and signed fastfiles, and writes a
merged recipe plist, an expanded bytecode cache and a coverage manifest.
Output stays outside the source app. Shader data belongs in local ignored
storage and is not included in the source repository.

The UID checksum is Adler-32 of the shader token stream, verified against all
8,765 original bundled shader entries. A fastfile record contains a token
count, a NUL-terminated HLSL filename and the tokens. The extractor validates
stage, alignment, the END token and UID checksum. Checksum aliases under
other names are recorded separately and ambiguous bytecode matches are
rejected. Signed payloads use authentication/data chunks as described by
[CoD-FF-Tools](https://github.com/primetime43/CoD-FF-Tools/blob/main/docs/MW2_PC_FastFile_Format.md);
this layout was checked against the installed files.

The final scan read all 258 English/DLC fastfiles without decompression errors.
It processed 13.02 GB of expanded data while retaining at most 69,259 bytes
in its scan buffer. It recovered 931 exact missing shader records and recorded
188 checksum aliases. The resulting 9,884 bytecode entries all passed
stage/END/checksum validation. Inputs cover 4,991 of 5,034 listed recipes,
99.15%. All required vertex UIDs are available; 37 pixel UIDs remain unresolved.
A fresh full extraction reproduced both output plists byte-for-byte.
A further scan of all shader filenames, the image, dylibs and resource plists
found no remaining target checksums. This establishes coverage of known
recipes, not every shader variant the game can generate.

## Capturing and compiling exact requests

A private APFS clone used the merged recipe lists and expanded binary cache.
The original game assets and saves were unchanged. Its preload-job budget
was increased from 1 to 25 ms, after checking the instruction bytes, solely
in that disposable clone. The original app and compatibility loader retain
their existing budget. Tests used muted private profiles and left the user's
other game alone.

The engine still performs translation and link calls. GLMetal's opt-in
`GLMETAL_DUMP_REQUESTS` records their exact sources, ordered attribute/output
bindings, feedback fields and variant masks. The two private runs recorded
1,675 and 2,243 requests, with 3,281 distinct requests across both runs.
The second run reached the Berlin checkpoint. A listed recipe does not
necessarily correspond one-to-one with a distinct captured program; complete
translation of all 4,991 available recipes has not been established.

Create the request directory before recording. `GLMETAL_DUMP_REQUESTS` must
be set when launching the app, and it records cache hits as well as misses.
Request files preserve the complete compile inputs; source pairs alone do
not preserve bindings or variant-dependent cache keys.

The generic native utility replays requests through the production compiler
and cache APIs. It compiles serially and never submits rendering work:

```sh
make -C native/glmetal build/glmetal-precompile
native/glmetal/build/glmetal-precompile \
  --request-dir /path/to/requests --metal-libraries --pause-ms 25
```

The fresh isolated-cache run completed all 3,281 requests and 6,562 Metal
stage libraries without failure in 196.37 seconds. Measured phases were
4.49 seconds of validation, 18.41 seconds of GLSL translation/linking and
80.26 seconds of Metal library work. The total includes deliberate pauses
and other overhead; phase totals should not be treated as gameplay FPS.

Every generated `.prog` and `.ok` file matched an existing game-generated
file exactly, 7,328 files and 77.12 MB total. No existing cache file needed
replacement. A warm replay hit all 3,281 program caches and completed the
same 6,562 library lookups. Validation took 0.11 seconds, frontend/cache
loading 0.81 seconds and Metal library work 0.45 seconds. That run used
5 ms pauses, so its 22.33-second total is not directly comparable with the
25 ms-paced cold total.

The CLI regression checks cold/hot hits, attribute-binding and feedback-mode
variants, exact JSON/binary key identity and malformed input rejection.
Ownership uses ARC and retains JSON source strings through each request.

## Ordinary checkpoint verification

A second private clone retained the original image and shader resources.
Only the generic request-recording driver/helper changed. It resumed the
copied Berlin checkpoint with normal preload settings, muted background
input and a 30 FPS limit. Renderer captures confirmed the helicopter scene.
The test stopped its own process after 20 seconds of gameplay. It did not
crash, and all 243 original profile files still matched their saved hashes.

The initial gameplay burst used 280 exact requests already in the prepared
set. By the end of the observation, the game had requested 317 unique
programs. Of these, 316 matched the prepared request payloads byte-for-byte;
one additional request was new. Its first CPU replay against the isolated
prepared cache confirmed 316 hits and one miss, with no failures. The new
request took 11.83 ms of frontend compilation and 23.75 ms of validation.
This shows why a finite capture is not complete-game coverage.

The two large bursts linked 91 and 88 programs. Application-thread program
work took 23 ms in each burst, and pipeline creation took 61 and 41 ms.
Total frames still took 474 and 377 ms. The startup frame took 464 ms.
These measurements include concurrent user gameplay and warm native caches;
they cannot isolate a performance improvement or establish an FPS ranking.
They confirm that the prepared programs are reusable while other stalls
remain. Compiler categories overlap and must not be added independently.

## Limits

The utility prepares `.ok` and `.prog` files and warms Apple's device cache
through actual Metal library compilation. It does not export standalone
`.metallib` files or precompile all render pipeline descriptors. Pipeline
state also depends on formats, samples, blending, vertex inputs and function
specialization. Apple's [binary archive documentation](https://developer.apple.com/documentation/metal/metal-binary-archives)
describes the additional pipeline information needed for GPU-specific binaries.
New program variants and pipeline states can still compile during gameplay;
asset streaming and other CPU/GPU work can also stall.

The ordinary local MW3 app can reuse the prepared caches because the compiler
identity is unchanged. Broad preload assets and the experimental budget patch
remain in the disposable app only. The prototype is not a complete-game
precompile or a stutter-free guarantee.
