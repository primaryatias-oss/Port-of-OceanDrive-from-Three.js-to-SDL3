# Ocean Drive: the C port

A port of [Ocean Drive](https://github.com/StarKnightt/ocean-drive) by Prasenjit (StarKnightt),
the Three.js walk along Miami Beach at sunrise ([play it in the browser](https://starknightt.github.io/ocean-drive/)), to plain
**C23 with SDL3 and nothing else**. There is no other C library: no math, image, font, audio or
UI dependency. Whatever the browser and three.js provided, this port rebuilds from scratch: the
renderer, the 2D canvas, the fonts, the Web Audio engine and the page UI.

Like the original, everything is generated in code at start-up. The hotels, palms, cars, sand,
ocean, sky, signs, people, vehicles and every sound are built procedurally, and the program loads
no image, model, font or audio file.

The goal is not "looks similar". It is **the same scene, number for number**. Geometry,
simulation and audio are checked against the running JavaScript version, and most modules match
it bit for bit.

## Building

You need a C23 compiler (GCC or Clang) and SDL3, found through `pkg-config sdl3`.

```sh
git clone https://github.com/primaryatias-oss/Port-of-OceanDrive-from-Three.js-to-SDL3.git
cd Port-of-OceanDrive-from-Three.js-to-SDL3
make -j$(nproc)            # release build -> build/release/oceandrive
./build/release/oceandrive
```

| Command | What it does |
| --- | --- |
| `make` | Optimized build (`-O2 -g -ffp-contract=off`, warnings as errors) |
| `make DEBUG=1` | `-O0` with AddressSanitizer + UBSan (uses clang) -> `build/debug/` |
| `make test` | Builds and runs the headless tests |
| `make shaders` | Regenerates `gen/` from `shaders/` (needs `glslangValidator`) |
| `make clean` | Removes `build/` |

The generated shader code in `gen/` is committed, so a normal build needs only the compiler and
SDL3. `glslangValidator` is a build-time tool, required only when you edit a shader.

`-ffp-contract=off` matters: it stops the compiler from fusing `a * b + c` into FMA instructions.
JavaScript rounds after every operation, and the port must round the same way to stay bit-exact.

## Running

Click to capture the mouse and walk. WASD moves, the mouse looks, Space jumps, Shift strolls
faster, E rides the bike or the ATV, M mutes and Esc releases the mouse. On a touchscreen, the
first touch brings up a joystick, drag-to-look and on-screen buttons.

| Flag | Effect |
| --- | --- |
| `--quality low\|medium\|high`, `--ultra` | Quality tier (detected automatically otherwise) |
| `--size WxH` | Window size |
| `--hud` | fps / draw calls / triangles / position line |
| `--autostart` | Start walking and hearing without the click |
| `--dynres 0`, `--gpuguard 0` | Turn off dynamic resolution / the GPU time guard |
| `--nofs` | No fullscreen on the first tap |
| `--frames N` | Quit after N frames |

## What is ported

Everything below runs in the live program. The notes say how each part was checked against
the JavaScript original.

**Renderer (SDL_GPU, Vulkan / SPIR-V).** A scene graph with meshes, instanced, batched and skinned
meshes and points. It uses three.js's WebGL conventions (GL projection, clip-space remap, shadow
bias), shadow maps, PMREM environment maps and the post chain (bloom, grade, FXAA, output).
Shaders are generated from the exact GLSL three.js produced in the browser. Every captured GPU
program is byte-identical to its capture at every tier.

**World.**

| Module | Checked against JS |
| --- | --- |
| Hotels, signs | Geometry exact (sign sizes follow the port's own font metrics) |
| Palms (227 trees, 6000 fronds) | Bit-exact |
| Street, colliders | Bit-exact |
| Cars (200 meshes) | Bit-exact |
| Sand, surf, beach (162 meshes) | Bit-exact, including the baked sand texture and swash |
| Ocean | Bit-exact grid at every tier |
| Birds | Bit-exact after the 360-step settle, every tier |
| People (skinned figures with IK) | All 10 meshes and their bone matrices bit-exact |
| Bike and ATV (physics + models) | 11 scripted rides and 16 meshes bit-exact |
| Walker (collision, steps, jumps) | 8 scripted walks bit-exact |

Rendered screenshots are compared with the browser's at the same camera. PSNR is mostly
41–67 dB. It drops to 30–34 dB for views dominated by sign lettering or sub-texel road
speckle, where the port's own font and the GPU's rasterization differ from the browser.

**Audio: a Web Audio engine written from scratch.** Audio parameters with automation, gain,
biquad filters, band-limited oscillators, buffer sources, a wave shaper with Chromium-style 2×
oversampling, Chromium's compressor, a partitioned-FFT convolver, panners and delays. For HRTF
panning, the port uses a stand-in filter model whose response is fitted to measurements of
Chrome. All sounds are ported: waves, wind, gulls, cars, footsteps, music and vehicles. The unit
tests match Chrome exactly, and whole-scene loudness is within Chrome's own run-to-run variation.

**2D canvas and fonts.** A software canvas, the subset of HTML Canvas 2D the scene uses, with
exact-coverage rasterization, gradients, clipping and blur. It is used for every procedural
texture. The fonts are hand-drawn stroke skeletons, including an upper- and lowercase UI set.

**Frame pacing.** Pixel-ratio sizing, a half-resolution start that ramps up, dynamic resolution,
a GPU time guard, a 0.2 s shadow-redraw gap and a warm-up render. The GPU guard measures real GPU
time from fences completed on a watcher thread, so CPU stalls don't count as GPU time.

**Page UI.** The HTML/CSS layer is redrawn with the canvas and composited on the GPU: the loading
title card with its sunrise exit animation, the start caption, notes, the "E ride" prompt, the
HUD, touch controls, and the graphics-driver-reset screen. After a lost GPU device, the program
relaunches itself at a lighter quality tier. CSS transitions become opacity and position tweens,
so a fade never redraws a canvas.

## How it was verified

The port is checked against the JavaScript app running in Chromium, not by eye. The reference
scripts in `tools/ref` expect a checkout of the [original project](https://github.com/StarKnightt/ocean-drive)
served by its Vite dev server at `http://localhost:5173`, with Playwright installed. Reference
captures they produced (browser shaders, screenshots) are in `tools/ref/out`.

- **Checksums:** `oceandrive --dump NAME` prints per-mesh checksums: sums of every attribute plus
  FNV-1a hashes of the raw position and index bits. `tools/ref/dump-group.mjs` prints the same
  for the browser scene.
- **Simulation:** scripted walks and rides are replayed in both versions and compared
  (`tools/ref/walk-test.mjs`, `tools/ref/ride-test.mjs`).
- **Screenshots:** `tools/ref/ref-shot.mjs` renders a reference frame in the browser.
  `tools/cmp-shot.sh` renders the same camera in C and diffs them.
- **Audio:** `--audio-test NAME` renders a sound offline and prints peak / RMS, compared with the
  browser's offline render.
- **Probing:** `tools/ref/probe.mjs 'EXPR'` evaluates an expression in the JS page.
- **Validation:** live runs are clean under Vulkan validation (`OCEAN_GPU_DEBUG=1`) and under
  ASan/UBSan (`make DEBUG=1`).

Getting to bit-exact meant honouring JavaScript's arithmetic everywhere:
- `x ** 2` becomes `x * x` and other powers go through `pow`.
- Values are rounded to float32 exactly where the JS stores them in typed arrays.
- Random numbers are drawn in JS evaluation order.
- Sorts are stable, like `Array.sort`.
- `Math.round`, `Math.hypot` and `Math.sign` are reproduced exactly.

## Layout

```
  src/
    core/ math/ geom/   containers, math, geometry builders
    gfx/                SDL_GPU renderer, materials, post, PMREM, textures
    canvas/             software 2D canvas and stroke fonts
    world/              sky, hotels, palms, street, cars, beach, surf, ocean, birds, people
    player/             walker and touch controls
    vehicles/           bike / ATV simulation, models, riding
    audio/              Web Audio engine (wa.c) and every sound
    ui/                 the page UI layer
    main.c              boot, frame loop, pacing, dumps and shots
  shaders/              GLSL sources (three.js captures + hand-written)
  gen/                  generated SPIR-V and program tables (committed)
  tools/                shader generator, image diff, JS reference scripts (tools/ref)
  tests/                headless tests
```

## Still open

- The loading screen's sun rays don't slowly rotate (one turn per 300 s in the original).
- It is unverified whether SDL's Vulkan backend draws the water spray particles larger than one
  pixel (wide points).
- The port has only been built and run on Linux (Fedora, AMD Radeon 680M, Vulkan).

## License

MIT: see [LICENSE](LICENSE).

This is a derivative of the original Ocean Drive, © 2026 Prasenjit (StarKnightt), MIT
([LICENSE-ocean-drive](LICENSE-ocean-drive)). The GLSL in `shaders/` and `gen/` was captured
from three.js's shader chunks and generated programs, © 2010-2026 three.js authors, MIT
([LICENSE-three.js](LICENSE-three.js)).
