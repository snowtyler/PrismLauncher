# Implementation Plan: Tests for the Sync Diff Logic

**Status:** Not started. Hand this to a fresh session; it is self-contained.
**Branch:** `feature/prism-sync`
**Scope:** `launcher/tasks/SyncedInstanceUpdateTask.{h,cpp}` and a new test binary.

---

## 1. Why this work exists

`SyncedInstanceUpdateTask` deletes files from users' instance directories (`deleteOrphanedFiles`,
[SyncedInstanceUpdateTask.cpp:503](launcher/tasks/SyncedInstanceUpdateTask.cpp#L503)). It has **zero test coverage** —
`tests/CMakeLists.txt` registers 21 test binaries and none touch any fork-specific code.

The cost has already been paid in production:

- `v12.0.4` — *"Prevent deleting options.txt and servers.dat during sync cleanup"* (data loss, shipped)
- `v12.0.6` — *"Scope force Voxy cache redownload to version updates and repair"*

Both are decision-logic bugs in this one function. The goal is to make that logic assertable so the
next one is caught before release.

**Non-goal:** rewriting the download/network path, the UI, or the upload side. Do not touch
`SyncedInstanceUploadTask`, `ModpackDashboard`, or `NetJob` wiring in this pass.

---

## 2. The problem: the logic is not reachable from a test

Everything worth testing lives inside `SyncedInstanceUpdateTask::executeTask()` /
`manifestFetched()`, which are entangled with four things a unit test cannot supply:

| Entanglement | Where |
| --- | --- |
| `APPLICATION->settings()` global singleton | `:326` |
| `m_instance->settings()` / `saveNow()` | `:189-207`, `:405` |
| Real filesystem (`QDirIterator`, `QFileInfo`, `QDir`) | `:288`, `:510` |
| `QNetworkReply` + `NetJob` | `:160`, `:429` |

The decision logic itself is pure: *given a manifest, a local file state, and some flags, produce a
list of files to download and a list of files to delete.* The refactor extracts exactly that and
nothing more.

---

## 3. The seam

Create **`launcher/tasks/SyncPlan.h`** and **`launcher/tasks/SyncPlan.cpp`**. No `Application.h`, no
`NetJob`, no `QNetworkReply`, no `QWidget` includes. Qt core types (`QString`, `QList`, `QMap`) are fine.

```cpp
#pragma once
#include <QString>
#include <QStringList>
#include <QList>

struct SyncManifestFile {
    QString path;
    QString hash;        // lowercase sha1
    qint64 size = 0;
    bool hasSize = false;
};

struct SyncManifest {
    QString version;
    QList<SyncManifestFile> files;
    bool forceConfigOverwrite = false;
    bool forceVoxyRedownload = false;
    QString voxyResetVersion;
    bool hasVoxyCacheZip = false;
    QString voxyZipFile;
    QString voxyZipHash;
    qint64 voxyZipSize = 0;
};

struct LocalFileState {
    bool exists = false;
    qint64 size = 0;
};

/// Abstracts the instance directory. Production impl wraps the real FS + hash cache;
/// tests supply an in-memory fake.
class ISyncFileSource {
   public:
    virtual ~ISyncFileSource() = default;
    /// relPath is relative to instanceRoot, '/'-separated.
    virtual LocalFileState stat(const QString& relPath) const = 0;
    /// Lowercase sha1, or empty string if unreadable. May be cached by the impl.
    virtual QString hash(const QString& relPath) const = 0;
    /// Every file under instanceRoot, recursively, as '/'-separated relative paths.
    virtual QStringList listFiles() const = 0;
    /// True if the dir exists AND is non-empty (matches isVoxyCacheSeeded semantics).
    virtual bool dirHasContent(const QString& relPath) const = 0;
};

struct SyncPlan {
    bool upToDate = false;              // fast path hit; caller should emitSucceeded()
    bool aborted = false;               // refused to plan (e.g. empty manifest) — see abortReason
    QString abortReason;
    QList<SyncManifestFile> toDownload;
    QStringList toDelete;               // relPaths, orphan cleanup
    QStringList voxyDirsToDelete;       // relPaths of dirs to removeRecursively()
    bool downloadVoxyZip = false;
    QString voxyZipRelPath;             // ".tmp/voxy_cache.zip"
    QString voxyExtractDir;             // relPath
    QStringList filesToKeep;
};

SyncPlan computeSyncPlan(const SyncManifest& manifest,
                         const QString& currentVersion,
                         bool forceRepair,
                         const ISyncFileSource& local);
```

### Behavior to move into `computeSyncPlan`, verbatim

Port these from `manifestFetched()` **without changing semantics** in this pass. Preserving current
behavior exactly is what makes the tests trustworthy; fixes come after the tests are green.

1. `isNewVersion` / `shouldForceConfigOverwrite` derivation — `:208-209`
2. `isVoxyCacheSeeded` → `local.dirHasContent(...)` on both candidate paths — `:212-226`
3. `shouldForceVoxyRedownload` derivation incl. `voxyResetVersion` comparison — `:228-239`
4. Legacy Voxy dir collection — `:247-270` → emit into `voxyDirsToDelete` (do **not** delete here)
5. Fast path — `:274-311`
6. Per-file download decision — `:334-383`
7. Voxy zip download item — `:385-397`
8. Orphan detection — the filtering half of `deleteOrphanedFiles` `:503-548` → emit into `toDelete`

### What stays in `SyncedInstanceUpdateTask`

Manifest JSON parsing, settings reads/writes, hash-cache load/save, the `ISyncFileSource`
implementation, and *executing* the plan (NetJob, `QFile::remove`, `removeRecursively`, zip extract).

The task becomes: parse → build source → `computeSyncPlan(...)` → execute.

> **Note on `Version` comparison:** step 3 uses `Version(currentVersion) < Version(voxyResetVersion)`
> from `launcher/Version.h`. That header is already in `Launcher_logic`; keep using it.

---

## 4. Implementation order

Do these as separate commits so a bisect can find a mistake.

1. **Add `SyncPlan.{h,cpp}` with the structs and a `computeSyncPlan` that is a direct
   copy-paste-and-adapt of the logic above.** Do not wire it in yet. Add both files to
   `LOGIC_SOURCES` in `launcher/CMakeLists.txt` next to the existing entries at
   [launcher/CMakeLists.txt:56](launcher/CMakeLists.txt#L56):
   ```cmake
   tasks/SyncPlan.h
   tasks/SyncPlan.cpp
   ```
2. **Add `tests/SyncPlan_test.cpp` + the in-memory fake, and write the tests in §5.** They should pass
   against the copied logic. Register in `tests/CMakeLists.txt`, matching the existing style:
   ```cmake
   ecm_add_test(SyncPlan_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
       TEST_NAME SyncPlan)
   ```
3. **Cut `SyncedInstanceUpdateTask` over to `computeSyncPlan`** and delete the now-duplicated logic.
   Real-app smoke test: sync one instance, confirm no behavior change.
4. **Only then** fix the empty-manifest bug in §6, with its test.

---

## 5. Test cases

The fake implements `ISyncFileSource` over a `QMap<QString, {size, hash}>` plus a mutable
`mutable int hashCallCount = 0;` so tests can assert cache avoidance.

### Group A — orphan cleanup (the v12.0.4 regression class)

| # | Case | Expect |
| --- | --- | --- |
| A1 | Disk has `minecraft/options.txt`, not in manifest | **not** in `toDelete` |
| A2 | Disk has `minecraft/servers.dat` and `servers.dat_old`, not in manifest | **not** in `toDelete` |
| A3 | Disk has `minecraft/optionsshaders.txt`, `optionsof.txt` | **not** in `toDelete` |
| A4 | Disk has `instance.cfg`, `.synced_cache.json` | **not** in `toDelete` |
| A5 | Files under `minecraft/saves/`, `screenshots/`, `logs/`, `crash-reports/`, `backups/`, `local/`, `.tmp/` | **not** in `toDelete` |
| A6 | `minecraft/journeymap/data.dat` not in manifest (outside the four managed dirs) | **not** in `toDelete` |
| A7 | `minecraft/mods/orphan.jar` not in manifest | **is** in `toDelete` |
| A8 | Orphans in `config/`, `resourcepacks/`, `shaderpacks/` | **are** in `toDelete` |
| A9 | Manifest lists `minecraft/mods/Foo.jar`, disk has `minecraft/mods/foo.jar` | **not** in `toDelete` (keep-set is lowercased — this matters on Windows) |

A1–A5 are the regression lock. Write them first; they are the reason this task exists.

### Group B — download decisions

| # | Case | Expect |
| --- | --- | --- |
| B1 | Manifest file missing on disk | in `toDownload` |
| B2 | Present, size differs | in `toDownload` |
| B3 | Present, size matches, hash differs | in `toDownload` |
| B4 | Present, size and hash match | **not** in `toDownload` |
| B5 | Manifest path uses `\` separators | normalized; matches disk file at `/` path |
| B6 | `options.txt` present, hash differs, `forceConfigOverwrite=false` | **not** in `toDownload` |
| B7 | Same but `forceConfigOverwrite=true` **and** `isNewVersion` | **is** in `toDownload` |
| B8 | Same, `forceConfigOverwrite=true`, version unchanged, `forceRepair=false` | **not** in `toDownload` (gate is `isNewVersion \|\| forceRepair`) |
| B9 | `options.txt` differs, `forceRepair=true` | **is** in `toDownload` |

### Group C — fast path

| # | Case | Expect |
| --- | --- | --- |
| C1 | `currentVersion == manifest.version`, all files present with matching size | `upToDate == true`, `toDownload` and `toDelete` both empty |
| C2 | Same but one file missing | `upToDate == false` |
| C3 | Same but one file wrong size | `upToDate == false` |
| C4 | Versions match, `forceRepair=true` | `upToDate == false` |
| C5 | `currentVersion` empty | `upToDate == false` |
| C6 | Fast path hit with an orphan present in `mods/` | `toDelete` empty — **documents that the fast path skips cleanup**. If that is wrong, fix it deliberately after this lands, not during. |

### Group D — Voxy (the v12.0.6 regression class)

| # | Case | Expect |
| --- | --- | --- |
| D1 | `hasVoxyCacheZip=true`, manifest also lists individual `.voxy/**` files | individual cache files **not** in `toDownload` |
| D2 | Voxy dir seeded, `forceRepair=false`, no forced redownload | `.voxy/**` and `*.sst` **not** in `toDownload` (RocksDB compaction case) |
| D3 | Voxy dir **not** seeded | `shouldForceVoxyRedownload` path taken |
| D4 | `voxyResetVersion="12.0.5"`, `currentVersion="12.0.4"`, new version | forced redownload |
| D5 | `voxyResetVersion="12.0.5"`, `currentVersion="12.0.6"`, new version | **not** forced |
| D6 | `voxyResetVersion` empty, `forceVoxyRedownload=true`, new version | forced |
| D7 | `voxyResetVersion` empty, `forceVoxyRedownload=true`, version **unchanged**, seeded | **not** forced |
| D8 | Forced redownload + `hasVoxyCacheZip=true` | `downloadVoxyZip == true`, `voxyZipRelPath == ".tmp/voxy_cache.zip"` |
| D9 | Forced redownload + no zip (legacy) | `voxyDirsToDelete` contains both candidate dirs |
| D10 | `voxyExtractDir` when `minecraft/` exists vs not | picks `minecraft/.voxy/...` vs `.voxy/...` |

### Group E — hash cache avoidance

| # | Case | Expect |
| --- | --- | --- |
| E1 | File whose size differs from manifest | `hashCallCount == 0` for it (size check short-circuits) |
| E2 | Missing file | `hashCallCount == 0` |
| E3 | Voxy file that is skipped by D1/D2 | `hashCallCount == 0` |

E1–E3 pin the performance property the hash cache exists to provide. They will catch a future
refactor that accidentally rehashes the whole instance on every check.

---

## 6. Latent bug to fix (step 4, after tests are green)

`manifestFetched()` checks `doc.isNull()` at [:173](launcher/tasks/SyncedInstanceUpdateTask.cpp#L173)
but never checks `doc.isObject()`. A response that is valid JSON but not the expected shape — a bare
array, a scalar, an HTML error page that happens to parse, or an object missing `files` — yields:

```
files          = []          // obj["files"].toArray() on a missing key
filesToKeep    = []
deleteOrphanedFiles([])      // deletes EVERY file under mods/, config/, resourcepacks/, shaderpacks/
```

That wipes the managed instance content. Same family as v12.0.4, not yet triggered because the
bucket has always served well-formed manifests.

**Fix:** treat an empty or absent `files` array as a hard failure, not as "delete everything."

```cpp
if (!doc.isObject()) {
    emitFailed(tr("Modpack manifest is malformed."));
    return;
}
...
if (files.isEmpty()) {
    emitFailed(tr("Modpack manifest contains no files; refusing to sync."));
    return;
}
```

Mirror it in `computeSyncPlan` via `SyncPlan::aborted` so it is testable:

| # | Case | Expect |
| --- | --- | --- |
| F1 | `manifest.files` empty, disk has files in `mods/` | `aborted == true`, `toDelete` empty |

Add a matching guard test at the JSON layer if a manifest-parse seam gets extracted later; not
required for this pass.

---

## 7. Verification

```bash
cmake --preset windows_msvc && cmake --build --preset windows_msvc --config Debug
```

```bash
ctest --preset windows_msvc -R SyncPlan --output-on-failure
```

Full suite must stay green:

```bash
ctest --preset windows_msvc
```

Then a manual smoke test before merging: sync a real instance, force-repair it, and confirm
`options.txt` and `servers.dat` survive both.

---

## 8. House rules

- Run `clang-format` (config at `.clang-format`) on every changed file before committing.
- Naming: `PascalCase` types, `m_camelCase` members, `camelCase` methods.
- **Do not add a `Signed-off-by` line.** If AI assisted, the human adds
  `Assisted-by: AGENT_NAME:MODEL_VERSION` to the commit message.
- Tests link `Launcher_logic`; keep `SyncPlan.cpp` free of `Application.h` or the build will drag the
  world into the test binary.
