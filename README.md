# Null Sector Demo Engine

A data-driven C++17/OpenGL demoscene production engine. The show is **data** — a
`.nsd` script describes scenes, cameras, effects, animations, audio and
post-processing — and the engine just plays it. New productions need little or
no C++.

The flagship production, **Ghost In The Machine**, ships as `data/demo.nsd`
plus the shaders and assets it references. A tiny second production,
`data/examples/ExampleDemo.nsd`, demonstrates the whole format on one page.

```
production (.nsd) ──► ScriptParser ──► Timeline ──► Director (DemoApp) ──► Effects ──► GL
                        │                                                     ▲
                        └── scenes/sections, cameras, anims, post, markers ────┘
```

## Quick start

```bash
# configure + build (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# run the flagship production (or the example)
./build/Release/ns_demo.exe
./build/Release/ns_demo.exe --demo=data/examples/ExampleDemo.nsd

# open the editor (timeline, viewport, inspector, audio, drops…)
./build/Release/ns_demo.exe --editor
```

GLFW is found via `find_package(glfw3)` and fetched from GitHub at configure
time if missing. `miniaudio` and `stb` are vendored under `third_party/`. No
other dependencies — no Boost, no Assimp yet (OBJ is built in; the asset
architecture is prepared for glTF/GLB).

## Project layout

```
src/
  engine/     GL wrapper, shader manager (+hot reload), textures, framebuffers,
              audio (decode + FFT/beat analysis), post-processing, UBOs, paths
  framework/  GL-free core: script parser (.nsd), timeline, animation,
              scene graph, camera rigs, JSON, assets, file watcher, logging
  effects/    built-in effects: intro, greetings, scene, splash, tunnel…
  app/        DemoApp director, effect registry, shadertoy importer, prodcheck
  editor/     Dear ImGui demo editor (timeline, hierarchy, inspector, drops)
  main.cpp    thin shell: flags, window, render loop
shaders/      engine + production shaders
assets/       fonts, textures
data/         the production: demo.nsd, scenes/, materials/, post/,
              models/, shadertoy/, textures/, timelines/, examples/
tests/        framework unit tests (ns_fw_tests)
docs/         HTML documentation (index, dsl, cheatsheet, build, first-effect…)
third_party/  vendored headers (miniaudio, stb) + Dear ImGui
```

## The .nsd format in 30 seconds

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

Commands: `show`/`hide`, `load` (shadertoy / model / material / plugin /
effect), `shader FILE`, `camera NAME { rig … }`, `play`, `fade in|out`,
`transition`, `post { … }` (or scene sugar like `bloom 0.8`), `anim`, `marker`,
`speed`, `loop`, `jump`, and scene-graph nodes (`mesh`/`sprite`/`text`/`light`/
`particles`/`empty`). Times accept seconds, `mm:ss`, or bar/beat units resolved
against the header tempo. Parameters can be keyframed:

```nsd
Tunnel { bloom { 0s = 0.2; 4s = 1.0; 8s = 0.4 } }   // linear interpolation;
                                                    // smooth / cubic /
                                                    // ease-in / ease-out /
                                                    // bounce / elastic too
```

See `docs/dsl.html` for the full language and `docs/cheatsheet.html` for the
one-page reference.

## Validation & preflight (CI-friendly)

```bash
./build/Release/ns_fw_tests.exe                          # framework unit tests
./build/Release/ns_demo.exe --check-shaders              # compile every shader
./build/Release/ns_demo.exe --check-production           # production checklist
./build/Release/ns_demo.exe --check-models               # OBJ -> lit -> readback
./build/Release/ns_demo.exe --check-shadertoy            # every data/shadertoy/*.glsl
./build/Release/ns_demo.exe --check-hotreload --no-track # break + fix live reload
./build/Release/ns_demo.exe --smoke-audio --track=music.mp3
```

Parser errors carry `file:line:col`, a description, and a "did you mean"
suggestion when a name is close:

```
demo.nsd:42:11  Unknown property 'blom' — did you mean 'bloom'?
```

## Audio reactivity

The analyser exposes `uBass`, `uMid`, `uTreble`/`uHigh`, `uKick`, `uBeat`,
`uBar` (downbeat pulse) and `uVolume` as shader uniforms, driven by FFT bands,
kick detection and the timeline's beat clock — so effects stay in sync while
scrubbing or exporting, not just during realtime playback.

## Live editing

Edit `demo.nsd`, any shader, or any referenced asset while the app runs:
scripts reload on `F2`, shaders hot-reload on save (a broken edit keeps the
last good program), and the editor's `+ Scene`, `+ Asset`, track loader and
drag-and-drop flows write data files and apply them immediately. Shader compile
errors are shown inside the editor with file/line diagnostics.

## Documentation

The `docs/` folder is a browsable HTML site (`docs/index.html`):

- **dsl.html** — the .nsd language
- **cheatsheet.html** — one-page command / time / rig / interpolator reference
- **build.html** — build, flags, validation battery
- **first-effect.html** — writing an effect, registering it, scripting it
- **troubleshooting.html** — common failures and how to diagnose them

## License

See `LICENSE` (not yet present — to be added before public release).
