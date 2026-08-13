# Changelog

All notable changes to the Null Sector Demo Engine are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Shader Lab** — a dockable demoscene typography workspace with live OpenGL preview, GLSL source editing, compile diagnostics, multi-font selection, textured and procedural colour fills, text/font/layout controls, audio/timeline uniforms, metadata-driven parameter sliders, twelve text presets, shader asset export, and one-click NSD timeline insertion. Exported shaders remain ordinary `.frag` assets and use the existing runtime `shader`/`SceneFX` path.
- **Editor text authoring** — selected text nodes now expose a visible screen-space handle in the live Viewport. Dragging the handle updates normalized NSD `pos` coordinates, persists the change to the project, and records the complete gesture as one undoable edit; the selected node is safely re-acquired after the runtime reload.
- **Editor scene authoring** — the NSD command palette, camera-rig inspector, camera-type selector, and scene transition in/out controls make previously script-only scene properties editable without leaving the editor.
- **Video playback** — a new `video` scene command (`video clip.mp4 { width 1280; height 720; fps 30; loop true; opacity 1; size (2,1.125,1) }`) plays a movie as a scene-graph node texture. `VideoPlayer` (`src/engine/video.{hpp,cpp}`) decodes through the same ffmpeg pipe MP4 export uses — a worker thread produces scaled RGBA frames that the main thread uploads into one reusable GL texture via `glTexSubImage2D`, so the runtime player ships no codec library. Videos render in the sprite/text pass with per-node opacity and size; the editor has a Video browse category, drag/drop, and an Inspector panel; the live-reload watcher and `--check-production` recognize video files; and the packer collects `video` references (video containers are never DEFLATE'd). Packaged `.nsp` playback extracts video payloads to short-lived temp files (the ffmpeg pipe is filename-based) and removes them when the scene ends.
- **Standalone editor and runtime-only player** — the build now produces three executables: `ns_demo` (playback / render / packaging only — no editor code, built with `NULLSECTOR_RUNTIME_ONLY`), `ns_editor` (the player plus the full authoring layer; a tiny `src/editor_main.cpp` entry forces `--editor`), and `ns_shader_ai` (below). `ns_demo` no longer links the editor or Dear ImGui. Docs and the packaging page now point at `ns_editor.exe`.
- **AI Shader Generator** — `ns_shader_ai.exe` is a standalone prompt→GLSL workstation that reuses the real engine Shader / VFS / fullscreen-triangle path: text prompt, live OpenGL preview with audio-reactive uniforms, parameter sliders, generation history, compile-error repair, `.nsshad` project files, and export into the engine shader path. Provider-neutral (`src/shader_ai.{hpp,cpp}`): ships with an offline built-in provider, any OpenAI-compatible endpoint can be configured, the request timeout defaults to 600 s (for reasoning models), and channel-texture downloads retry transient failures with linear backoff. New docs page `docs/shader-ai.html` added to the docs nav.
- **Editor timeline pan/zoom and event editing** — middle-drag pans the time window and the mouse wheel zooms around the cursor, working across the ruler, lanes and audio strip. Timeline events (commands/markers) are now selectable, draggable (a tooltip shows the new time while dragging), `Ctrl+D` duplicates and `Del` deletes — the per-scene block parser is kept local so delete/duplicate only touch the command's own block.
- **Viewport guides and camera tools** — the View menu toggles Grid, world Axes, Safe frame and center Crosshair overlays (persisted in editor state); `Create Camera From View` (`Ctrl+Alt+C`, or the Inspector button) writes a camera node from the current fly view; `F` frames the selection (or all visible content). Scrubbing now refreshes the preview on the same frame, so the viewport is never one frame behind the playhead.
- **Audio pause/play and safer track swapping** — `AudioEngine::setPlaying()` pauses/resumes both the audio device and the show clock (Space now play/pauses in the editor). `loadTrack`/`swapTrack` decode before touching the live source (a failed load keeps the currently playing track), stop the device before moving the buffer, and loading while paused updates the source without starting playback.
- **Text alignment** — text commands accept an `align` option (left/center/right), wired through cinetext and the Shader Lab text controls.
- **Shader Lab authoring upgrades** — line-numbered source editor, editable shader asset name, Save As, Revert saved / Revert compiled, Reset parameters (back to the metadata defaults), a dirty `*` indicator on the preset label, and compile-now-validates-without-saving semantics.
- **New content** — `shaders/audioreactive_oscilloscope.frag` (+ the `data/oscilloscope.nsd` starter production), `shaders/cyberpunk.frag`, and `data/shadertoy/multipass_example.glsl`; plus `data/Demo_Test_First.nsd` and the `update_root_exe.bat` convenience script.

### Changed

- CMake splits runtime / editor / AI sources into the `ns_demo`, `ns_editor` and `ns_shader_ai` targets; `ns_shader_ai` is built from `src/shader_ai_main.cpp` + the engine Shader/audio/GL path.
- `src/shadertoy_convert.{hpp,cpp}` moved into `ns_framework` so the Shadertoy importer and the AI workstation share one converter.
- README rewritten (production table, feature highlights, layered architecture); `docs/build.html`, `docs/editor.html`, `docs/packaging.html` and the `docs/docs.js` nav updated for the new binaries and editor workflows.
- `data/demo.nsd` gains a `bpm` header and the Intro camera rig moves to `nave`.

### Fixed

- **AI Shader Generator flat outputs** — remote generations that ignored the pixel position (time-only colors like `fragColor = vec4(fract(uTime), …)`) previously compiled and rendered as a flashing solid screen with no warning. The provider prompt now requires per-pixel coordinates (`in vec2 vUV` or `gl_FragCoord.xy / uResolution`) and forbids uniform time-only output; extracted sources are hardened (explicit `#version 300 es`, `gl_FragColor` → `out vec4 fragColor`, output declaration injected) so they can never silently compile as legacy GLSL; and every successful compile is rendered to a 64×64 target and read back at two instants — a shader whose pixels are all identical is flagged with an amber warning and a repair note instead of being accepted silently (the note also enables "Ask AI to Fix", which previously required a compile error). Covered by new smoke assertions.
- Editor scrub preview lagged one frame (UI input is processed after the engine renders) — a queued seek now triggers an explicit same-frame refresh (`refreshSeekPreview`).
- A failed audio track load previously cleared the currently playing track; the old source is now retained and playback state (paused or running) is preserved across swaps.

## [0.2.0] — 2026-08-09

### Added

- **GLB model import** — `.glb` files now load through the existing model pipeline, including glTF 2.0 binary chunk validation, indexed POSITION/NORMAL/TEXCOORD geometry, triangle/strip/fan primitives, node transforms, and PBR base material factors. The editor model picker, asset browser, live-reload watcher, and model preflight now recognize `.glb` alongside `.obj`.
- **NSD image nodes and transitions** — added the `image` scene-graph command as a texture-friendly alias for `sprite`, with one-argument (`image poster.png`) and explicit node/file forms. Images support data-driven `fade`, `crossfade`, `zoom`, and directional slide entrance transitions with configurable durations; packer, production checks, parser tests, and DSL documentation cover the feature.
- **Editor NSD scene browser and authoring** — the Hierarchy now lists every scene declaration from the loaded `.nsd`, shows its scheduled start/end, and jumps the transport to a scene boundary when clicked. Selecting a scene opens an Inspector with editable title, bars, duration, intensity, chapter, visibility, and a multiline setup-command editor; metadata changes and applied setup edits are serialized through the document model with undo support.
- **Editor project files** — File > "New Project (.nsd)..." creates a validated starter
  production from scratch, while "Save Project As..." writes the current document to
  a new `.nsd`, switches the active runtime project, and keeps timeline views separate
  per file. `Ctrl+Shift+S` is the Save As shortcut; creating a new project warns before
  discarding unsaved edits.
- **Editor project loading + timeline fit** — File > "Load Project (.nsd)..." now opens
  the native project picker and switches the editor only after the selected script
  parses successfully. New projects open with the complete show fitted in the
  timeline; the zoom range follows the production duration, the horizontal
  scrollbar provides video-editor-style panning, and `F` / `Home` toggles fit-all
  and the previous zoomed view. Timeline views remain persisted per project.
- **Editor project packaging** — File > "Package Project..." creates a verified
  `.nsp` asset package, copies the running engine as `<project>.exe`, writes a
  `launch.bat` using `--play=<project>.nsp --fullscreen`, and emits a portable
  ZIP distribution. The editor reuses the existing production dependency packer,
  verifies the NSP hashes before archiving, and includes `NS_EDITOR_PACKAGE_SMOKE`
  for an end-to-end packaging/playback check.
- **Editor MP4 export** — File > "Export MP4..." in the demo editor
  opens a save dialog (path, fps, audio mux toggle) and runs the capture
  pipeline in-process: the show restarts at 0:00, one frame per capture
  boundary is read back from the presented framebuffer and handed to a
  background writer thread that pipes raw RGB into ffmpeg (H.264 + the
  playing track muxed with `-shortest`, so the video is sample-accurately
  synced and unsaved document edits are included). Live progress + cancel
  in the dialog; a bounded buffer pool drops (and counts) frames when the
  encoder falls behind instead of stalling the editor. The ffmpeg command
  is now built by a shared framework helper (`framework/core/ffmpegpipe`),
  also used by the CLI `--export-mp4` path. Smoke:
  `NS_EDITOR_EXPORT_SMOKE=out.mp4` auto-starts an export at editor boot
  (`NS_EDITOR_EXPORT_SECONDS` caps it) and prints an OK/FAIL verdict.
- **Editor document model** — the editor now manipulates a real document
  (the parsed `.nsd` AST, `src/editor/document.{hpp,cpp}`) instead of
  editing raw text / runtime structures and reconstructing a script
  afterwards. Authoring operations mutate the AST, mark it dirty, and either
  push a lightweight live update to the runtime (keyframe drags) or commit
  the document to disk + reload the show (save, add-scene, undo):
  - `EditorDocument` — parsed `Script` AST + path + dirty state; `load` /
    `adopt` / `serialize` / `save` / `write`, marker ops (add/remove/rename/
    move with shared-block splitting) and `anim`-command queries
  - **Undo / redo** (Ctrl+Z / Ctrl+Y, Edit menu) — snapshot-based, one undo
    step per gesture (drags coalesce), no-op gestures skipped; dirty `*` in
    the window title clears on Ctrl+S
  - **`+ Scene` is a document op** — appends a `SceneDef` + activation block
    to the AST, extends the header duration, writes the file and reloads
    (replaces the raw text-append implementation)
  - External reloads (file watcher / F2 / script switch) re-adopt the
    document from the freshly parsed script, so the .nsd on disk stays the
    source of truth
- **NSD writer** — `src/framework/script/nsdwriter.{hpp,cpp}`: the inverse
  of `ScriptParser::parse`. `nsdSerialize` round-trips every production
  (demo.nsd, neural_dust.nsd, example.nsd) with exact float equality and
  idempotent output; numbers print as their shortest exact decimal
  (`77.8`, not `77.8000031`)
- **Keyframe curve editor** (View > Curves) — a UI over the existing
  AnimationSystem data (`anim` commands in the document): channel list,
  draggable keys with multi-select, double-click to add (curve-sampled
  value), Delete, copy/paste, per-key interpolator, beat/bar grid snap
  (follows the scrub-quantize grid) and a live playhead; edits push a live
  preview into the runtime animation library (`DemoApp::editorApplyAnim`)
- **Inspector keyframe buttons** — a small keyframe diamond on the
  Position / Rotation / Scale rows keyframes `node:<name>.<prop>` in the
  active scene at the current scene-relative time and value
- **First-class production markers** — markers are editable document
  objects: click jumps the transport, drag moves (live preview, grid
  snap), double-click opens an edit dialog (name / time / delete), all
  undoable and persisted to the .nsd
- **`NS_EDITOR_DOC_SMOKE`** — CI proof of the document pipeline inside the
  running editor (add → undo → redo → write → runtime derivation)
- **Framework tests** — NSD writer round-trip (structural equality +
  idempotency, incl. the real productions) and `EditorDocument` marker
  ops / dirty / undo — 690 checks total

- **Virtual Filesystem (VFS)** — a GL-free abstraction under `src/framework/vfs/`
  that all runtime asset reads go through, so the same `.nsd` production runs
  from the development tree or from a packaged `.nsp` file:
  - `VirtualFileSystem` interface (`exists` / `read` / `readText` / `stat` /
    `list`) with a process-wide `runtimeFS()` the loaders use
  - `DirectoryFileSystem` — mounts virtual prefixes onto real directories
    (`data/`, `shaders/`, `assets/`, root catch-all); normalizes separators and
    rejects traversal (`../`), absolute paths and drive letters
  - `PackageFileSystem` — the same VFS API backed by an `.nsp` package, with a
    reserved slot for a future `EmbeddedFileSystem`
- **`.nsp` package format** — simple versioned container (`NSPK` magic,
  manifest with virtual path / offset / sizes / compression method / FNV-1a
  hash, file data). The reader validates magic, version, manifest bounds
  (truncation), entry offsets/sizes and checksums, and rejects malformed
  packages safely
- **Production packer** — `ns_demo.exe --pack data/demo.nsd --output X.nsp`:
  headless (no GL/window), parses the production with the existing script
  engine, walks every referenced asset (shaders, Shadertoy files, textures,
  models, materials + their maps, post presets, fonts, scripts, audio), and
  prints a discovery report. Native plugins are skipped with a warning
- **Packaged playback** — `ns_demo.exe --play X.nsp` mounts the package as the
  runtime VFS at startup (single selection point, no scattered mode checks);
  runs standalone without the `data/` directory
- **Track auto-discovery scoped to the production** — the boot track and the
  packer look in the production's own directory and the folder named after the
  script (`data/foo.nsd` + `data/foo/track.mp3`), so sibling productions'
  music never leaks into a show or a package
- **Framework tests** — DirectoryFS (existing/missing, text & binary reads,
  normalization, traversal rejection), package format round-trip (multiple /
  empty / binary / large files, invalid header, invalid version, invalid
  offsets, truncated package, checksum failure), dev↔package equivalence and
  FNV-1a known-vector regression (canonical isthe.com vectors incl. an
  embedded-NUL case), compressed round-trip, keep-only-if-smaller, corrupt
  compressed payloads — 652 checks total
- **Package reader hardening** — open() keeps only the header + manifest in
  RAM and reads payloads on demand (seek + read per asset), so a multi-GB
  package no longer costs its whole size in memory; all bounds arithmetic is
  overflow-safe; nonzero header/entry flags and reserved fields are rejected,
  `dataOffset` must be 8-aligned, and overlapping payload ranges are detected
- **Package directory index** — built once at open(); `PackageFileSystem`
  `list()`/`stat()` answer from the index in O(children) instead of scanning
  every packaged file
- **DEFLATE compression** — .nsp method 1 via the vendored public-domain
  miniz (pinned commit, `src/framework/vfs/miniz/`). The packer gives every
  file a compress hint by extension (already-compressed audio/image/archive
  formats stay stored) and the writer keeps the DEFLATE form only when it is
  genuinely smaller, so the policy can never bloat a package. The integrity
  hash always covers the UNCOMPRESSED bytes, so checks mean the same thing
  for both methods; the reader decompresses transparently behind the same
  VFS API, and the packer report now shows raw/stored sizes, the overall
  compression percentage and a per-method breakdown
- **Documentation** — `docs/packaging.html` (VFS architecture, `.nsp` format,
  packing & playback, virtual path conventions, internals) linked from the
  docs nav, plus a VFS/packaging section in `README.md`
- **MP4 export** — `--export-mp4=OUT.mp4` renders the show once (no loop) to an
  H.264 MP4 in real time, piping the presented frame (RGB24, default framebuffer
  before swap) into an ffmpeg process on stdin at a fixed capture rate
  (`--export-fps`, default 60). The show clock stays audio-driven, so the video
  is sample-accurately synced to the track, which ffmpeg muxes as a second input
  (a package-internal track with no real path degrades to video-only with a
  warning). Frame capture is boundary-based: a slow frame duplicates, never
  drops, and the loop idle-paces to the next boundary. `--export-seconds=N`
  caps the render for previews/CI; `--window=WxH` sets the resolution.

### Changed

- Migrated all runtime asset reads to the VFS: shader loading, shader manager
  (+hot reload), textures/models/materials, fonts, audio, `.nsd` scripts,
  Shadertoy files, post-processing presets and the splash/logo assets
- `main.cpp` — new `--pack`, `--play`, `--root` and `--output` flags; the
  runtime VFS is selected once near startup (`--play` → package, otherwise →
  dev tree)

### Fixed

- MP4 export frames were vertically flipped: `glReadPixels` returns rows
  bottom-up (row 0 = image bottom) while ffmpeg rawvideo expects top-down.
  Both the CLI `--export-mp4` and the editor's Export MP4 capture paths now
  flip rows after readback via the shared `framework/core/ffmpegpipe::
  flipRowsInPlace` (the BMP `--shot` path was unaffected - BMP is bottom-up
  too). Verified end-to-end: an exported frame's row-brightness profile
  correlates +0.999/+1.000 with a `--shot` reference frame.
- Dev-tree track discovery walked nothing at the virtual root — `list("")` on
  the directory filesystem now resolves the catch-all mount (while unsafe
  paths like `../x` are still rejected)
- FNV-1a 64 offset basis was missing its final digit
  (`1469598103934665603` → canonical `14695981039346656037`) in the hash
  function and the empty-vector overload — both agree now and match the
  reference vectors
- Package integrity checks could wrap on crafted offsets (`offset + size` over
  2^64) — now checked overflow-safe against the file size
- `verifyAll` reported a spurious short read for empty payloads (stale
  `gcount()` from the previous entry)
- **NEURAL DUST frame drops** — the ocean reveal (`ndnet`) was a per-pixel
  SDF raymarch at ~205 ms/frame (≈ 4 fps for ~40 s of the show). It is now a
  geometry effect (`src/effects/network.cpp`, `shaders/nd_net.{vert,frag,
  void.frag}`): 144 node point sprites + camera-facing synapse quads drawn
  additively over the live particle ocean, node positions computed per-vertex
  from the exact same wave field the raymarch used (~0.3 ms/frame, and the
  below-network “void” look — base tint, traveling pulse glow, beat-locked
  glitch tears — is preserved by a cheap fullscreen pass ported from the old
  shader’s miss path). The core scene’s ambient node glow was gated to the
  miss path (its own comment said “miss path only” — every hit pixel was
  paying 56 exp()/sqrt() for a halo it can’t see): median 12.2 → 5.2 ms.
  `ndboot`/`ndcore` renderScale moved 0.52/0.6 → 0.45/0.5 in
  `data/neural_dust.nsd` (the failure-mode core already ran at 0.5). The
  reveal now holds 60 fps; the core fly-through remains the scene’s heaviest
  passage (inherent to the 56-node march from inside the shell).

## [0.1.0] — 2026-08-09

Initial commit: data-driven C++17/OpenGL demoscene engine + **NEURAL DUST**
production.

### Added

- Scripted scenes, cameras, animations, audio reactivity, timeline sync and
  post-processing, all driven by `.nsd` data files
- GLSL shader manager with hot reload, texture/model/font/audio loading,
  Shadertoy importer, built-in effect registry and plugin support
- **NEURAL DUST** — complete realtime production: 10 scenes (Boot, Memory
  Core, Tunnel, Lost City, Corruption, Dream, Neural Ocean, System Failure,
  Final Reconstruction, New Reality) with a full 346 s soundtrack and
  data-driven timeline
- Dear ImGui demo editor (timeline, hierarchy, inspector, asset drops)
- Validation suite: `--check-shaders`, `--check-production`, `--check-models`,
  `--check-shadertoy`, `--check-hotreload`, `--smoke-audio`, plus framework
  unit tests (`ns_fw_tests`)
- HTML documentation site under `docs/`
