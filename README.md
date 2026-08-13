<img width="1619" height="972" alt="ChatGPT Image 6 8 2026 klo 14 49 43" src="https://github.com/user-attachments/assets/54598671-b872-4302-bf12-987459b078bc" />

# Null Sector Demo Engine

NOTE: this project is still a work in progress and not all functionality
has been properly tested.

A data-driven C++17/OpenGL demoscene production engine. The show is **data** —
a `.nsd` script describes scenes, cameras, effects, animations, audio and
post-processing — and the engine just plays it. New productions need little or
no C++: you write a script, drop in shaders/models/textures, and the player,
editor and packager handle the rest.

The repository ships two full productions and one miniature example:

| Production | Script | Length | Notes |
|---|---|---|---|
| **Null Sector Demo Engine** | `data/demo.nsd` | 125 s | flagship show, 10 sections, fully self-contained |
| **NEURAL DUST** | `data/neural_dust.nsd` | 346.8 s (5:47) | 10 scenes @ 128 BPM, 185 bars, generated soundtrack |
| **Example Demo** | `data/examples/ExampleDemo.nsd` | 42 s | demonstrates the whole `.nsd` format on one page |

## Feature highlights

**Script-driven production (`.nsd`)**
- Plain-text show description: `demo` header (title / bpm / duration), `scene`
  blocks with cameras and setup, and top-level `at` events scheduling scene
  changes, markers, animations and transitions.
- Times accept seconds, `mm:ss`, or bar/beat units resolved against the header
  tempo (`bar 12`, `24 beat`, `32bars`, `2m04`).
- Keyframable parameters with linear / smooth / cubic / ease-in / ease-out /
  bounce / elastic interpolators, on post values, effect uniforms, camera
  properties and scene-graph node transforms.
- Built-in effects (`intro`, `cathedral`, `neuralnet`, `infinitemachine`,
  `tunnel`, `ghostformation`, `greetings`…) plus a plugin system for dropping
  in new ones as shared libraries.

**Realtime rendering**
- OpenGL 3.3 core renderer: shader manager with hot reload, textures,
  framebuffers, UBOs, forward-lit 3D models, particle systems, text (TrueType
  or embedded bitmap font) and a fullscreen post stack.
- 3D asset pipeline: built-in OBJ import **and** glTF 2.0 `.glb` import
  (indexed POSITION/NORMAL/TEXCOORD, triangle/strip/fan primitives, node
  transforms, PBR base material factors), with data-driven materials and
  directional/point lights.
- **Shadertoy importer**: drop in `shadertoy/*.glsl` files and script them
  with `load shadertoy foo.glsl` — multi-pass buffers included.
- Post-processing presets (`legacy`, `cinematic`, `clean`, `nd_boot`…),
  scene-level sugar like `bloom 0.8`, and keyframable per-effect values
  (bloom, exposure, DoF…).

**Audio-reactive show clock**
- `miniaudio` decode (WAV/MP3/OGG/FLAC), FFT analysis, kick/snare detection
  and a beat clock that drives shader uniforms (`uBass`, `uMid`, `uTreble`,
  `uKick`, `uBeat`, `uBar`, `uVolume`…) — in sync while playing, scrubbing
  **or** exporting.
- Timeline stays audio-driven, so exports are sample-accurately synced to the
  music.

**Live editing**
- Edit `demo.nsd`, any shader, or any asset while the app runs: scripts reload
  on `F2`, shaders hot-reload on save (a broken edit keeps the last good
  program), and the editor writes data files that apply immediately.

**Demo editor (Dear ImGui)**
- A real document model over the `.nsd` AST with snapshot **undo/redo**, a
  keyframe **curve editor**, editable **markers**, a video-editor-style
  **timeline** (scrub, grid snap, audio-aware fit-all, horizontal scrollbar,
  middle-drag panning and wheel zoom), scene browser, hierarchy, inspector,
  live viewport with fly camera, drag-and-drop asset imports and a
  **Shader Lab** for typography shaders. The toolbar's **+ Scene** button adds
  a uniquely named scene and activation block without hand-editing the script.
- Project workflows: New / Load / Save / Save As `.nsd`, **Export MP4**
  (in-process ffmpeg capture with audio mux) and **Package Project**
  (`.nsp` + self-contained exe + `launch.bat` + portable ZIP).

**AI-assisted shader authoring**
- Standalone **AI Shader Generator** workstation (`ns_shader_ai.exe`):
  prompt → GLSL, live preview with audio reactivity, parameter sliders,
  generation history, compile-error repair, project files (`.nsshad`) and
  export back into the engine's shader path. Ships with an offline built-in
  provider; any OpenAI-compatible endpoint can be configured. Provider settings
  include a configurable request timeout (default 600 seconds), which is useful
  for reasoning models that may take minutes before returning a response.

**Virtual filesystem & packaging**
- Every runtime asset read goes through a VFS, so the same production runs
  from the dev tree or from a single `.nsp` package (`--play`), with no
  `data/` directory needed. The packer walks the script's referenced assets,
  compresses with DEFLATE only when smaller, and verifies FNV-1a integrity
  hashes.

**CI-friendly validation**
- Headless checks for the production, every shader, every model, every
  Shadertoy file, hot reload and audio, plus framework unit tests with no GL
  dependency — all exit 0/1 for pipelines.

## Architecture

```
production (.nsd) ──► ScriptParser ──► Timeline ──► Director (DemoApp) ──► Effects ──► GL
                        │                                                     ▲
                        └── scenes/sections, cameras, anims, post, markers ────┘
```

Four layers, only the bottom two touch OpenGL:

```
src/main.cpp         shell — window + GL context, audio engine, show clock, hotkeys, flags
src/app/             director — DemoApp, effect registry, shader manager, post stack,
                                Shadertoy importer, model renderer, packer, preflights
src/framework/       GL-free core — .nsd parser + writer, timeline, animation, scene graph,
                                camera rigs, JSON, assets, file watcher, VFS, ffmpeg pipe
src/engine/          the GL layer — renderer, camera, PostFX, text, audio, UBOs, video
data/                the show — edit this to make a demo
```

The engine contains **no show logic**: scenes, cameras, effects, animations,
post and the section schedule all live in `data/*.nsd` plus the JSON, shaders,
models and textures it references.

## Quick start

```bash
# configure + build (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# run the flagship production (or the example, or NEURAL DUST)
./build/Release/ns_demo.exe
./build/Release/ns_demo.exe --demo=data/examples/ExampleDemo.nsd
./build/Release/ns_demo.exe --demo=data/neural_dust.nsd --track=data/neural_dust/track.wav
# note: only NEURAL DUST ships a soundtrack; the flagship runs silent unless
# you drop a track next to data/demo.nsd or pass --track=FILE

# open the standalone demo editor (document + undo, curves, markers, timeline, drops…)
./build/Release/ns_editor.exe --demo=data/demo.nsd
#   Ctrl+S saves the .nsd document, Ctrl+Z / Ctrl+Y undo / redo
#   View > Shader Lab for typography presets, live GLSL and audio-reactive preview
#   File > Export MP4...   File > Package Project...

# launch the standalone AI shader workstation
./build/Release/ns_shader_ai.exe
# verify its offline generation/compile/recovery/project workflow
./build/Release/ns_shader_ai.exe --smoke

# convert a Shadertoy .glsl (single-pass or multi-pass with `// pass:` markers)
# or a Shadertoy JSON API export to a single Null Sector fragment shader
# (buffers folded, textures as samplers); data/shadertoy/multipass_example.glsl
# is a shipped multi-pass example
./build/Release/ns_shader_ai.exe --convert-shadertoy=data/shadertoy/multipass_example.glsl --out=multipass_example.frag

# export the show to an MP4 (real-time, music-synced; ffmpeg must be on PATH)
./build/Release/ns_demo.exe --demo=data/demo.nsd --windowed --window=1920x1080 \
    --export-mp4=ghost.mp4

# package a production into a single .nsp and play it back without data/
./build/Release/ns_demo.exe --pack data/demo.nsd --output NullSectorDemoEngine.nsp
./build/Release/ns_demo.exe --play NullSectorDemoEngine.nsp
```

A rendered 1080p copy of NEURAL DUST is included at the repo root as
`neural_dust_full_1080p.mp4`, and a packaged distribution as
`neural_dust_distribution.zip`.

## Command-line reference

All flags are shared by the player (`ns_demo.exe`) and the standalone editor
(`ns_editor.exe`) unless noted.

| Flag | Description |
|---|---|
| `--demo=PATH` | demo script to run (default `data/demo.nsd`) |
| `--check-production[=P]` | headless production validation (no GL): parse the `.nsd`, build the timeline, verify every scene/effect/shadertoy/model/material/preset/rig reference resolves; prints a checklist, exits 0/1 |
| `--check-shaders` | compile every engine + app shader program, then exit (with `--render`: also render every self-contained content shader offscreen and flag ones that never drew / render a solid color / go near-black) |
| `--check-models` | 3D pipeline preflight: OBJ/GLB → lit shader → draw readback, plus shipped models/materials |
| `--check-shadertoy` | render every `data/shadertoy/*.glsl` offscreen with pixel readback, then exit |
| `--check-hotreload` | live-reload smoke: break + fix a temp shader, verify keep-previous + recompile |
| `--smoke-audio` | decode the track headless + analyser self-test |
| `--track=F1,F2,..` | play F1 at boot; `T` / `Shift+T` cycles the comma list (default: scan for playable tracks) |
| `--no-track` | run with no music file (silent) |
| `--font=FILE` | TrueType font for text (default: `assets/fonts/*.ttf`, else embedded 8×8 bitmap) |
| `--plugin=DIR` | effect plugin directory (default `data/plugins`) |
| `--windowed` / `--fullscreen` | window vs fullscreen (fullscreen is the default — it's a show) |
| `--window=WxH` | window size when windowed (default 1600×900) |
| `--editor` | dockable demo editor (ImGui); always enabled in `ns_editor.exe` — `ns_demo` is runtime-only and rejects the flag with a hint |
| `--editor-seconds=N` | with `--editor`: auto-close after N seconds (CI smoke) |
| `--shot=SEC:FILE.bmp` | seek to SEC, save one presented frame as a 24-bit BMP, exit |
| `--shot-noseek` | with `--shot`: run from 0 and capture when the clock reaches SEC |
| `--export-mp4=FILE` | render the show once to an H.264 MP4 (real-time, music-synced; ffmpeg must be on PATH; muxes the playing track; `--window=WxH` sets the resolution) |
| `--export-fps=N` | export capture rate (default 60, clamped 1–240) |
| `--export-seconds=N` | stop the export after N seconds (previews/CI) |
| `--perf-json[=PATH]` | write one GPU-time sample per effect + the post stack at exit (default `perf.json`) |
| `--perf-csv[=PATH]` | append one row per second (t, kind, name, context, ms) per active timed effect (default `perf.csv`) |
| `--perf-raw[=PATH]` | append every un-smoothed collected sample — the spikes the per-second EMA hides (default `perf.raw.csv`) |
| `--perf-seconds=N` | auto-exit after N s + dump (scripted A/B perf runs) |
| `--pack=PATH` | package a production headlessly (no GL/window); walks the script's referenced assets |
| `--output=FILE` | output `.nsp` path for `--pack` |
| `--play=FILE` | mount an `.nsp` package as the runtime VFS and play it (no `data/` needed) |
| `--root=DIR` | root directory for `--pack` asset discovery |
| `--help` / `-h` | print the full flag reference and exit |

Environment variables `NULLSECTOR_SHADER_DIR`, `NULLSECTOR_DATA_DIR` and
`NULLSECTOR_ASSET_DIR` override the baked-in resource directories at runtime
(see `src/engine/paths.hpp`).

## Player controls

| Key | Action |
|---|---|
| `Esc` | quit |
| `Space` | pause / resume |
| `←` / `→` | scrub one bar (audio stays in sync) |
| `0` / `1` | jump to previous / next section |
| `R` | restart at 0:00 |
| `F2` | reload the demo script |
| `F11` | toggle fullscreen |
| `M` | mark a cue |
| `L` | toggle section loop |
| `+` / `-` | speed up / slow down the show clock (timescale) |
| `T` / `Shift+T` | switch to next / previous track (async decode, show keeps playing) |

The editor additionally uses `Ctrl+S` (save), `Ctrl+Shift+S` (save as),
`Ctrl+Z` / `Ctrl+Y` (undo / redo), `.` (step one frame), `F` / `Home` (timeline
fit-all), `Ctrl+Alt+C` (create camera from current view), middle-mouse drag
(timeline pan), mouse-wheel over the timeline (zoom), and right-mouse drag in
the viewport (fly camera). In editor mode, `Space` pauses/resumes both the
show clock and loaded audio.

## The .nsd language

A production is one file. The header sets the title, tempo and length; scenes
describe what happens; top-level `at` events schedule when.

```nsd
demo "MY DEMO" { bpm 140; duration 60 }        // header: tempo + length

scene Intro {                                   // a scene block
    bars 4   title "Wake"                       // meta: length in bars, label
    camera IntroCam { rig static; pos (0,0,2.4); fov 55 }
    show intro
    fade in 1
}

at 0     { show Intro;  marker Wake }           // events: absolute time or
at bar 4 { show Plasma; marker Plasma }         // bar/beat units ("bar 12",
                                                // "24 beat", "32bars", "2m04")
```

**Commands** — `show`/`hide`, `load` (shadertoy / model / material / plugin /
effect), `shader FILE`, `camera NAME { rig … }`, `play`, `fade in|out`,
`transition`, `post { … }` (or scene sugar like `bloom 0.8`), `anim`, `marker`,
`speed`, `loop`, `jump`, and scene-graph nodes (`mesh`/`sprite`/`image`/`text`/
`light`/`particles`/`empty`). Times accept seconds, `mm:ss`, or bar/beat units
resolved against the header tempo.

**Keyframes** — parameters can be animated with a choice of interpolators:

```nsd
Tunnel { bloom { 0s = 0.2; 4s = 1.0; 8s = 0.4 } }   // linear interpolation;
                                                    // smooth / cubic /
                                                    // ease-in / ease-out /
                                                    // bounce / elastic too
```

Keyframes can target post values (`post.bloom`, `post.exposure`), effect
uniforms (`effect:cathedral.uniform:uMode`) and scene-graph transforms
(`node:gem.euler`, `node:caption.opacity`).

**Camera rigs** — `static`, `nave`, `orbit`, `fly`, `drift`, `hover`, plus
handheld shake, sway, FOV sweeps and depth-of-field parameters:

```nsd
camera NaveCam { rig nave; pos (0,6.5,18); target (0,5.5,0); speed 6.2;
                 fov 58; sway (2.5,1,0); freq 0.21; handheld 0.05;
                 dofFocus 12; dofAperture 0.03 }
```

Parser errors carry `file:line:col`, a description, and a "did you mean"
suggestion when a name is close:

```
demo.nsd:42:11  Unknown property 'blom' — did you mean 'bloom'?
```

See `docs/dsl.html` for the full language and `docs/cheatsheet.html` for the
one-page reference. The NSD **writer** (`src/framework/script/nsdwriter.cpp`)
is the exact inverse of the parser: it round-trips real productions with
exact float equality and idempotent output, which is what lets the editor
serialize your edits back to disk.

## Audio reactivity

The audio engine decodes the track (miniaudio), runs an FFT, detects kicks and
snare hits, and derives a beat clock from the timeline. Every shader receives
the result as uniforms — effects stay in sync while playing, scrubbing **or**
exporting:

| Uniform | Meaning |
|---|---|
| `uBass`, `uMid`, `uTreble` / `uHigh` | FFT band energy |
| `uKick`, `uSnare` | kick / snare detection |
| `uBeat`, `uBar` | beat pulse and bar (downbeat) pulse |
| `uVolume` / `uAudioLevel` | overall level |

The demo loop also supports runtime track switching (`T` / `Shift+T`) with an
async decode — the show keeps playing until the swap commits — and shows a
short on-screen readout of the loaded track. In the editor, Space/Play/Pause
controls the audio device and show clock together; loading or replacing a track
preserves the active shader/source document. `--track=F1,F2,...` pins an
explicit A/B cycle list, and track auto-discovery is scoped to the
production's own directory so sibling shows' music never leaks in.

## Rendering & assets

**Shaders** — engine + production shaders live in `shaders/` and hot-reload on
save. `--check-shaders` compiles every stage up front. Text shaders support
TrueType fonts and audio uniforms.

**3D models** — OBJ import is built in; `.glb` (glTF 2.0 binary) is supported
with chunk validation, indexed geometry, triangle/strip/fan primitives, node
transforms and PBR base material factors. Models are scripted with `load model`
+ `mesh` nodes, lit by the forward renderer with data-driven materials
(`load material chrome`) and lights:

```nsd
mesh gem { model gem.obj; material neon; pos (2.2,1.4,0); scale (0.8,0.8,0.8) }
light sun { type directional; color (1,0.92,0.78); intensity 2 }
light fill { type point; color (0.4,0.6,1); intensity 6; range 14 }
```

**Shadertoy imports** — `shadertoy/*.glsl` files load through the importer,
including multi-pass buffer setups, and are scripted like any other effect:

```nsd
load shadertoy plasma.glsl
show shadertoy:plasma.glsl
```

**Post-processing** — a fullscreen post stack with named presets
(`post preset cinematic`), scene-level shorthand (`bloom 0.8`), keyframable
values (`post.bloom`, `post.exposure`) and DoF driven by the camera.

**Video nodes** — scene nodes support realtime video textures:

```nsd
scene Trailer {
    video intro.mp4 { size (2,1.125,1); width 1280; height 720; fps 30; loop true }
}
```

The player decodes frames through the system `ffmpeg` executable, uploads them
on the render thread into a reusable RGBA texture, and drops old frames rather
than blocking the show. Video files resolve from `data/video/`; the editor
exposes the decoder and quad properties in its Inspector, and packaged `.nsp`
productions extract video payloads to a temporary file (cleaned up at the end).

**Image nodes & transitions** — `image` is a texture-friendly alias for
`sprite` (`image poster.png`), with data-driven `fade`, `crossfade`, `zoom`
and directional slide entrances at configurable durations.

## Live editing

Everything the show reads can be edited while it runs:

- **Scripts** — `F2` reloads the `.nsd` (the editor also watches the file and
  re-adopts the document).
- **Shaders** — saved files hot-reload; a broken edit keeps the last good
  program, and compile errors show inside the editor with file/line
  diagnostics.
- **Assets** — the file watcher covers textures, models, materials and audio;
  the editor's `+ Scene`, `+ Asset`, track loader and drag-and-drop flows write
  data files and apply them immediately. Fragment shaders can be dragged
  directly from `.frag` rows in the docked Assets panel, not only from the
  Open Asset browser.

## The demo editor

`ns_editor.exe` opens a dockable Dear ImGui shell around the running engine
(`ns_demo` is deliberately runtime-only; launch the editor binary instead): live viewport, scene hierarchy, inspector, timeline,
console, assets and profiler panels (View menu toggles each, layouts persist in
`imgui.ini`).

**Document model & undo** — the editor manipulates a real document: the parsed
`.nsd` AST (`src/editor/document.cpp`). Authoring operations mutate the AST,
mark it dirty, and either push a lightweight live update to the runtime
(keyframe drags) or commit the document to disk + reload the show (save,
add-scene, undo). Undo/redo is snapshot-based, one step per gesture, with a
dirty `*` in the window title that clears on Ctrl+S.

**Timeline** — video-editor-style transport: scrub (audio stays in sync), grid
snap, section boundaries, audio-aware fit-all (`F` / `Home`), loop and
step-one-frame. A loaded track longer than the NSD show extends the timeline
content range; use the bottom scrollbar or middle-drag to pan and the mouse
wheel to zoom around the cursor. Views persist per project file.

**Keyframe curves** (View > Curves) — a channel list over the `anim` commands
in the document: draggable keys with multi-select, double-click to add,
delete/copy/paste, per-key interpolator, beat/bar grid snap and a live
playhead. Edits preview live into the runtime animation library. Inspector
rows (Position / Rotation / Scale) carry a keyframe diamond that keys the
current value.

**Markers** — first-class, editable document objects: click to jump the
transport, drag to move (live preview, grid snap), double-click to rename /
retime / delete — all undoable and persisted to the `.nsd`.

**Scenes & nodes** — the Hierarchy lists every scene declaration with its
scheduled start/end; clicking one jumps the transport. The Inspector edits
title, bars, duration, intensity, chapter, visibility and a multiline
setup-command editor, plus camera rig properties, camera type, transitions and
an NSD command palette. Selected text nodes expose a visible screen-space
handle in the viewport — dragging it updates normalized `pos`, persists to the
project and records one undoable edit.

**Project files** — File > New Project / Load Project / Save / Save As
(`Ctrl+Shift+S`) with a validated starter production, a native picker, and a
discard-unsaved warning. Projects open with the complete show fitted in the
timeline.

**Export MP4** — File > Export MP4… opens a save dialog (path, fps, audio mux
toggle) and runs the capture pipeline in-process: the show restarts at 0:00,
each capture boundary is read back from the presented framebuffer and handed to
a background writer thread piping raw RGB into ffmpeg (H.264 + the playing
track muxed with `-shortest`), with live progress + cancel and a bounded buffer
pool that drops frames when the encoder falls behind instead of stalling the
editor. Unsaved document edits are included in the export.

**Package Project** — File > Package Project… creates a verified `.nsp` asset
package, copies the running engine as `<project>.exe`, writes a `launch.bat`
using `--play=<project>.nsp --fullscreen`, and emits a portable ZIP
distribution.

**Shader Lab** (View > Shader Lab) — a dockable demoscene typography workspace:
live OpenGL preview, GLSL source editing with compile diagnostics, multi-font
selection, textured and procedural colour fills, text/font/layout controls,
audio/timeline uniforms, metadata-driven parameter sliders, twelve text
presets, shader asset export, and one-click NSD timeline insertion. Exported
shaders are ordinary `.frag` assets on the existing runtime `shader`/`SceneFX`
path.

## AI shader workstation

`ns_shader_ai.exe` is a standalone GLSL generation and preview tool that reuses
the engine's real Shader, VFS and fullscreen-triangle path:

- Describe the look in plain language (or pick a quick idea), choose fragment /
  vertex / pair, and generate.
- Live 16:9 preview with **simulated or real audio reactivity** — load a track
  and the FFT drives the shader; six audio sliders simulate levels without one.
  Play/Pause and Space control the preview clock together with loaded audio,
  and loading audio preserves the current shader source/editor buffer.
- Parameter sliders from `// @param` declarations, a rough ALU/texture/loop
  performance estimate, and compile diagnostics with click-to-line.
- Version history with restore, an "Ask AI to Fix" repair flow that feeds the
  compile error back to the model, and `.nsshad` project files (prompt,
  spec, source, history, preview settings).
- Export as `.frag`/`.vert` (or copy the NSD `shader` snippet) straight into
  the engine. Ships with an offline built-in provider; configure any
  OpenAI-compatible endpoint in View > Settings. Use Save settings to retain
  the provider configuration; Windows protects the API key with the current
  user's DPAPI credentials, and keys are never written to project files.
- **Headless Shadertoy conversion** — `ns_shader_ai.exe --convert-shadertoy=in.glsl
  --out=out.frag` (no window opens; `--out` defaults to `in.glsl.frag`) turns a
  Shadertoy shader — single-pass, or the engine's multi-pass `// pass:` marker
  format — into one portable Null Sector fragment shader: buffer passes fold
  into `vec4` helper functions, texture channels become samplers the caller
  binds at runtime (printed as `Bind:` notes), and the standard uniform shim
  (`uResolution`, `uTime`, `uBass`, …) is emitted so it compiles anywhere
  those uniforms exist. Multi-channel imports work in all three forms:
  standard Shadertoy `#iChannelN "spec"` resource lines, `// channel:`
  comments, and code that samples `iChannelN` with no wiring comment at all
  (every sampled-but-unwired channel is inferred as a bindable sampler
  instead of reading black). In the editor, channel textures referenced by
  `https://` URLs (and Shadertoy `/media/` `/presets/` asset paths) are
  downloaded automatically to `data/textures/` on a background thread and
  bound to the preview the moment they land, with re-imports served from the
  local cache. Transient failures retry automatically with a tunable backoff
  (attempts and wait set in Provider settings) and the active attempt shows
  as `retrying N/M...` in the Texture Channels row. Shadertoy **JSON API exports**
  (`{"Shader": {"renderpass": [...]}}`) are accepted too: the per-pass code
  blocks are mapped onto the same passes (`Common`, `Image`, `Buffer A`–`D`),
  each pass's `inputs` array is wired to channels (textures → samplers,
  buffers → folded calls, audio/keyboard → stubs), `Sound`/`Cube` passes are
  skipped with a note, and JSON that isn't a Shadertoy export is rejected
  with an error instead of producing broken
  GLSL. A worked example ships at `data/shadertoy/multipass_example.glsl`
  (common + two buffers + an image pass with `// channel:` wiring) and is
  verified by `--check-shadertoy` every run.

## Virtual filesystem & packaging

All runtime asset reads go through a virtual filesystem, so the same `.nsd`
production runs from the development tree or from a single packaged `.nsp`
file. The VFS is selected once at startup — the renderer, shaders and asset
loaders never know where the bytes came from.

```bash
# Development (unchanged)
./build/Release/ns_demo.exe
./build/Release/ns_demo.exe --demo=data/demo.nsd
./build/Release/ns_editor.exe --demo=data/demo.nsd

# Create a package (headless; walks the .nsd's referenced assets)
./build/Release/ns_demo.exe --pack data/demo.nsd --output NullSectorDemoEngine.nsp

# Playback: mount the package — no data/ directory needed
./build/Release/ns_demo.exe --play NullSectorDemoEngine.nsp
```

The `.nsp` format is a simple versioned container (magic `NSPK`, file table,
FNV-1a integrity hashes) defined in `src/framework/vfs/`. Payloads are stored
as-is or DEFLATE-compressed (public-domain miniz, vendored under
`src/framework/vfs/miniz/`): the packer chooses by extension, keeps the
compressed form only when it is actually smaller, and the integrity hash always
covers the uncompressed bytes. The reader validates magic, version, manifest
bounds, offsets/sizes and checksums, rejects malformed packages safely, and
keeps only the header + manifest in RAM (payloads are read on demand), so a
multi-GB package doesn't cost its whole size in memory. Native plugins are
skipped by the packer (documented limitation). See `docs/packaging.html` for
the full format and conventions.

## Performance profiling

The show can measure itself for renderScale tuning and A/B comparisons:

```bash
# one GPU-time sample per effect + post stack at exit
./build/Release/ns_demo.exe --perf-json --perf-seconds=125 --no-track

# per-second EMA rows for every active timed effect (plottable over time)
./build/Release/ns_demo.exe --perf-csv --perf-seconds=30 --no-track

# every raw (un-smoothed) sample — the spikes the EMA hides
./build/Release/ns_demo.exe --perf-raw --perf-seconds=30 --no-track
```

Combined with `renderScale` options on effects and `--window=WxH`, these make
scripted A/B runs possible; `docs/plot_perf.py` plots the CSV output. The
editor has its own Profiler panel.

## Validation & preflight (CI-friendly)

```bash
./build/Release/ns_fw_tests.exe                          # framework unit tests (no GL needed)
./build/Release/ns_demo.exe --check-shaders              # compile every shader
./build/Release/ns_demo.exe --check-production           # production checklist
./build/Release/ns_demo.exe --check-models               # OBJ/GLB -> lit -> readback
./build/Release/ns_demo.exe --check-shadertoy            # every data/shadertoy/*.glsl
./build/Release/ns_demo.exe --check-hotreload --no-track # break + fix live reload
./build/Release/ns_demo.exe --smoke-audio --track=data/neural_dust/track.wav   # any WAV/MP3 works
```

The editor adds end-to-end smoke modes driven by environment variables:
`NS_EDITOR_DOC_SMOKE` (add → undo → redo → write → runtime derivation),
`NS_EDITOR_SCENE_SMOKE`, `NS_EDITOR_ASSET_SMOKE`, `NS_EDITOR_SCRUB_SMOKE`,
`NS_EDITOR_FLY_SMOKE`, `NS_EDITOR_AUDIO_SMOKE`, `NS_EDITOR_SHADERLAB_SMOKE`,
`NS_EDITOR_EXPORT_SMOKE=out.mp4` (plus `NS_EDITOR_EXPORT_SECONDS`) and
`NS_EDITOR_PACKAGE_SMOKE=out.zip` — each auto-runs its flow at editor boot and
prints an OK/FAIL verdict for CI. The AI workstation has `--smoke`, and the
packer is exercised end-to-end by packaged-playback tests.

## Project layout

```
src/
  engine/     GL wrapper, shader manager (+hot reload), textures, framebuffers,
              audio (decode + FFT/beat analysis), post-processing, UBOs, video, paths
  framework/  GL-free core: script parser + writer (.nsd), timeline, animation,
              scene graph, camera rigs, JSON, assets, file watcher, VFS (dev
              tree + .nsp packages), ffmpeg pipe
  effects/    built-in effects: intro, greetings, scene, splash, tunnel,
              neural-network geometry…
  app/        DemoApp director, effect registry, shadertoy importer, model
              renderer, packer, prodcheck / modelcheck / shadertoycheck
  editor/     Dear ImGui demo editor: document model + undo, keyframe curves,
              markers, timeline, hierarchy, inspector, asset drops, Shader Lab,
              MP4 export, packaging
  main.cpp            thin runtime shell: flags, window, render loop
  editor_main.cpp     standalone editor entry point
  shader_ai*.cpp      standalone AI GLSL generator/workstation
shaders/      engine + production shaders
assets/       fonts, textures
data/         productions: demo.nsd, neural_dust.nsd, scenes/, materials/, post/,
              models/, shadertoy/, textures/, timelines/, examples/, neural_dust/
tests/        framework unit tests (ns_fw_tests)
docs/         HTML documentation site (index, cheatsheet, dsl, editor, build…)
tools/        content generators: gen_models.py, gen_neural_dust_track.py
third_party/  vendored headers (miniaudio, stb, miniz) + Dear ImGui (docking)
```

## Build options & dependencies

CMake ≥ 3.20, C++17. Targets: `ns_framework` (static lib), `ns_fw_tests`,
`ns_demo`, `ns_editor`, `ns_shader_ai`.

```bash
cmake -S . -B build -DNS_BUILD_APP=OFF   # framework + tests only (no GLFW needed)
cmake -S . -B build -DNS_BUILD_TESTS=OFF # skip unit tests
```

GLFW is found via `find_package(glfw3)` and fetched from GitHub at configure
time if missing. `miniaudio`, `stb` and `miniz` are vendored under
`third_party/` / `src/framework/vfs/miniz/`; Dear ImGui (docking branch) is
vendored under `third_party/imgui/`. No Boost, no other runtime dependencies.
ffmpeg is optional and only needed for MP4 export and video nodes.

## Tools

- `tools/gen_neural_dust_track.py` — generates the NEURAL DUST soundtrack
  (128 BPM, full 5:47 show length).
- `tools/gen_models.py` — generates the shipped OBJ models.

## Documentation

The `docs/` folder is a browsable HTML site (`docs/index.html`):

- **cheatsheet.html** — one-page command / time / rig / interpolator reference
- **build.html** — build, flags, validation battery
- **dsl.html** — the .nsd language
- **timeline.html**, **animation.html**, **scene.html**, **cameras.html** — the
  timeline, animation system, scene graph and camera rigs
- **shaders.html**, **shadertoy.html**, **postfx.html**, **assets.html** —
  shader manager, Shadertoy import, post FX and assets/reload
- **editor.html** — standalone editor, scene authoring, video/image nodes and
  export workflows
- **shader-ai.html** — AI shader generation, compatibility, metadata and export
- **plugins.html**, **first-effect.html** — plugin system and writing an effect
- **packaging.html** — the virtual filesystem and `.nsp` package format
- **troubleshooting.html** — common failures and how to diagnose them

## License

See `LICENSE` (not yet present — to be added before public release).
