# Changelog

All notable changes to the Null Sector Demo Engine are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

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
  offsets, truncated package, checksum failure) and dev↔package equivalence
  (603 checks total)
- **Documentation** — `docs/packaging.html` (VFS architecture, `.nsp` format,
  packing & playback, virtual path conventions, internals) linked from the
  docs nav, plus a VFS/packaging section in `README.md`

### Changed

- Migrated all runtime asset reads to the VFS: shader loading, shader manager
  (+hot reload), textures/models/materials, fonts, audio, `.nsd` scripts,
  Shadertoy files, post-processing presets and the splash/logo assets
- `main.cpp` — new `--pack`, `--play`, `--root` and `--output` flags; the
  runtime VFS is selected once near startup (`--play` → package, otherwise →
  dev tree)

### Fixed

- Dev-tree track discovery walked nothing at the virtual root — `list("")` on
  the directory filesystem now resolves the catch-all mount (while unsafe
  paths like `../x` are still rejected)

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
- **Ghost In The Machine** — flagship production shipping as `data/demo.nsd`
- Dear ImGui demo editor (timeline, hierarchy, inspector, asset drops)
- Validation suite: `--check-shaders`, `--check-production`, `--check-models`,
  `--check-shadertoy`, `--check-hotreload`, `--smoke-audio`, plus framework
  unit tests (`ns_fw_tests`)
- HTML documentation site under `docs/`
