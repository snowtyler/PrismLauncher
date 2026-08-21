# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A fork of [Prism Launcher](https://github.com/PrismLauncher/PrismLauncher) (Qt6/C++ Minecraft launcher), branded **PrismSync**, adding a Cloudflare R2-backed modpack sync system. Fork work lives on `feature/prism-sync`; upstream integration branch is `develop`. Version is defined in `CMakeLists.txt` (`Launcher_VERSION_MAJOR/MINOR/PATCH`) and is bumped in each "Release vX.Y.Z" commit.

## Build / test

CMake presets drive everything (`CMakePresets.json`): `linux`, `macos`, `macos_universal`, `windows_mingw`, `windows_msvc`. Generator is Ninja Multi-Config; build dir is `build/`, install dir `install/`.

```bash
cmake --preset windows_msvc && cmake --build --preset windows_msvc --config Debug
```

Local Windows build + NSIS installer (sets `Launcher_BUILD_ARTIFACT=PrismSync`, the updater repo, and the Qt prefix path — edit it if Qt moves). Release builds are handled by GitHub Actions; locally, always use Debug:

```bash
./build_installer.bat Debug
```

Tests are Qt Test binaries registered via `ecm_add_test` in `tests/CMakeLists.txt` (gated on `BUILD_TESTING`, ON by default):

```bash
ctest --preset windows_msvc
```

Run a single test by name:

```bash
ctest --preset windows_msvc -R FileSystem_test
```

## Code style (from CONTRIBUTING.md)

- Run `clang-format` (config in `.clang-format`) on changed files before committing; `.clang-tidy` encodes most naming rules.
- `PascalCase` types; `m_camelCase` private/protected members, `s_camelCase` private statics, `camelCase` public members and methods, `SCREAMING_SNAKE_CASE` for macros/`const` globals/`static const` members, `PascalCase` enum constants.
- No invented abbreviations. Avoid `[[nodiscard]]` except where ignoring the return really causes a bug.
- Don't rename existing non-conforming names unless already refactoring the whole class.
- Upstream requires a human `Signed-off-by` (DCO) — an AI agent must never add one. If AI assisted, the human adds `Assisted-by: AGENT_NAME:MODEL_VERSION` to the commit message.

## Architecture

**`launcher/Application.cpp`** is the hub: a `QApplication` subclass reachable everywhere via the `APPLICATION` macro. It owns the settings object (all defaults are `registerSetting` calls here), the metadata index, the HTTP/metacache layer, accounts, icon/theme managers, and the instance list. When adding a new configurable behavior, register its default here.

**Settings** are two-tier: global settings on `APPLICATION->settings()`, per-instance settings registered in `MinecraftInstance::MinecraftInstance` (`launcher/minecraft/MinecraftInstance.cpp`) and persisted in each instance's `instance.cfg`.

**Tasks** (`launcher/tasks/Task.h`) are the universal async unit — everything long-running subclasses `Task`, overrides `executeTask()`, and reports through `emitSucceeded`/`emitFailed`/`setProgress`. `SequentialTask`, `ConcurrentTask`, and `MultipleOptionsTask` compose them; the UI runs them through `ProgressDialog`. Networking uses `NetJob`/`NetRequest` (`launcher/net/`), which layers caching, hashing, and progress on `QNetworkAccessManager`.

**Instances**: `BaseInstance` → `MinecraftInstance`, held by `InstanceList`; per-instance UI pages come from `InstancePageProvider` / `launcher/ui/pages/instance/`. Third-party pack sources live under `launcher/modplatform/` (flame, modrinth, ftb, atlauncher, technic, packwiz), each exposing a `ResourceAPI` implementation plus a page under `launcher/ui/pages/modplatform/`.

### Fork-specific: the sync system

Instances synced from R2 carry per-instance settings `SyncShortcode` and `SyncVersion`. Global R2 config (`SyncR2Endpoint`, `SyncR2Bucket`, `SyncR2PublicUrl`, `SyncR2AccessKey`, `SyncR2SecretKey`) is registered in `Application.cpp` and edited on `launcher/ui/pages/global/LauncherPage`.

Bucket layout, read over the public URL:

- `registry.json` — list of published packs, driving the browse UI
- `shortcodes/<code>.json` — a pack's manifest: version, file list with hashes/sizes, banner
- `packs/<code>/<relpath>` — the actual file contents

The three moving parts:

- **`SyncedInstanceUpdateTask`** — downloads a manifest, diffs it against the local instance using a persisted hash cache (size+mtime keyed, so unchanged files are never rehashed), fetches only changed files via `NetJob`, deletes orphans, and handles the Voxy cache as a single ZIP that is extracted after download. Player-owned files such as `options.txt` and `servers.dat` are deliberately excluded from cleanup. `setForceRepair(true)` bypasses the cache.
- **`SyncedInstanceUploadTask`** — the publish side. Takes S3 credentials, computes a PUT/DELETE action list against the remote manifest, uploads files, banner, manifest, and updates `registry.json`. Driven from `UploadConfirmDialog` (file selection, force-config-overwrite, force-Voxy-redownload).
- **`ModpackUpdateCheckTask`** — polls manifests for all synced instances at startup (`CheckModpackUpdatesOnStartup`) and before launch in `LaunchController`, surfacing available updates.

The browse/install UI is `launcher/ui/widgets/ModpackDashboard.cpp` + `ModpackCard`, hosted in `MainWindow`.
