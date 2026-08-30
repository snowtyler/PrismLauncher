# Implementation Plan: Atomic Hash-Cache Write

**Status:** Not started. Self-contained; hand to a fresh session.
**Branch:** `feature/prism-sync`
**Scope:** `.synced_cache.json` load/save in `launcher/tasks/SyncedInstanceUpdateTask.cpp`.
**Size:** ~40 lines including tests. This is small.

---

## 1. Severity — read this first

**This is not a data-loss bug.** The hash cache is a pure optimization: `getOrComputeHash()` falls
back to computing the hash whenever the cache misses, so an empty or corrupt cache produces
*correct* results, just slower. Truncated JSON cannot deserialize into a valid-but-wrong object
(unbalanced braces fail to parse), so the corrupt case degrades safely to "cache empty".

The actual symptom of a crash mid-write is: **the next sync silently rehashes the entire instance**,
with no indication why. On a pack with a large Voxy cache that is a long, confusing stall.

Ranked third behind the sync-diff tests and below the empty-manifest guard. Worth doing because it
is cheap and the fix is already sitting in the codebase — not because it is urgent.

---

## 2. Current state

[`saveHashCache()`](launcher/tasks/SyncedInstanceUpdateTask.cpp#L115) writes with a raw `QFile`:

```cpp
QFile file(cachePath);
if (file.open(QIODevice::WriteOnly)) {
    file.write(doc.toJson(QJsonDocument::Compact));   // return value unchecked
    file.close();
    m_cacheDirty = false;
}
```

Two defects:

1. **Non-atomic.** `WriteOnly` truncates immediately. Between truncate and the final byte the file on
   disk is invalid. `saveHashCache()` is called at three points, including immediately after
   `deleteOrphanedFiles()` and before downloads begin — a crash or power loss in that window leaves
   a truncated file.
2. **Unchecked write.** `file.write()`'s return is ignored, so a short write still sets
   `m_cacheDirty = false`, marking a partially-persisted cache as clean.

---

## 3. The fix: use what already exists

Do **not** hand-roll temp-file-and-rename. [`FS::write()`](launcher/FileSystem.cpp#L179) already does
it correctly:

```cpp
void write(const QString& filename, const QByteArray& data)
{
    ensureExists(QFileInfo(filename).dir());
    PSaveFile file(filename);                    // QSaveFile subclass
    if (!file.open(PSaveFile::WriteOnly)) { throw FileSystemException(...); }
    if (data.size() != file.write(data))        { throw FileSystemException(...); }
    if (!file.commit())                          { throw FileSystemException(...); }
}
```

`PSaveFile` ([PSaveFile.h:48](launcher/PSaveFile.h#L48)) is `QSaveFile` plus registration of the
in-flight path with `APPLICATION`, so the launcher's file watcher ignores the transient temp file.
`QSaveFile` writes `<name>.XXXXXX` alongside the target and renames on `commit()`.

`FileSystem.h` is already included at
[SyncedInstanceUpdateTask.cpp:3](launcher/tasks/SyncedInstanceUpdateTask.cpp#L3). No new dependency.

### Two things that must not change

- **`FS::write` throws.** The current code fails silently. Keep that behavior — a failed *cache*
  write must never fail the sync. Catch `FileSystemException`, log it, return.
- **`m_cacheDirty` must stay `true` on failure**, so a later `saveHashCache()` retries. The current
  code gets this right by accident (assignment is inside the `if`); the new code must get it right
  on purpose.

### The corrected save logic

This is the body that lands in `saveSyncHashCache()` in the new file created in §4 — it is shown
here against the current signature so the diff against today's code is readable. Do not write it
into `SyncedInstanceUpdateTask` and then move it; go straight to §4's layout.

```cpp
void SyncedInstanceUpdateTask::saveHashCache()
{
    if (!m_cacheDirty) {
        return;
    }

    QJsonObject filesObj;
    for (auto it = m_hashCache.begin(); it != m_hashCache.end(); ++it) {
        QJsonObject fileObj;
        fileObj["size"] = it.value().size;
        fileObj["mtime"] = it.value().mtime;
        fileObj["hash"] = it.value().hash;
        filesObj.insert(it.key(), fileObj);
    }

    QJsonObject rootObj;
    rootObj["files"] = filesObj;

    QString cachePath = m_instance->instanceRoot() + "/.synced_cache.json";
    try {
        FS::write(cachePath, QJsonDocument(rootObj).toJson(QJsonDocument::Compact));
        m_cacheDirty = false;
    } catch (const FileSystemException& e) {
        // Cache is an optimization; a failed write must not fail the sync.
        qWarning() << "Failed to persist sync hash cache to" << cachePath << ":" << e.cause();
        // m_cacheDirty stays true so a later save retries.
    }
}
```

> Verify the exception accessor before committing — Prism's `FileSystemException` derives from
> `Exception`; use `e.cause()` if present, otherwise `e.what()`.

### Also: make a corrupt cache say so

[`loadHashCache()`](launcher/tasks/SyncedInstanceUpdateTask.cpp#L83) returns silently on parse
failure, so "cache corrupt" and "no cache yet" are indistinguishable — which is exactly the
confusion described in §1. One line fixes it:

```cpp
QJsonDocument doc = QJsonDocument::fromJson(data, &err);
if (doc.isNull() || !doc.isObject()) {
    qWarning() << "Sync hash cache at" << cachePath << "is unreadable, rebuilding:" << err.errorString();
    return;   // empty cache — correct, just slow
}
```

Leave the empty-file case quiet; only warn when data was present but unparseable.

---

## 4. Extract for testability

**Decided: do this.** Without a seam there is nothing to assert, because cache I/O reaches through
`m_instance->instanceRoot()`. The `SyncPlan_test` harness already exists, so the marginal cost is
two small tests.

Create `launcher/tasks/SyncHashCache.{h,cpp}`. Move `CachedFileInfo` out of the task's private
section into the header (it is currently a private nested struct, which is why it cannot be tested).

```cpp
#pragma once
#include <QMap>
#include <QString>

struct CachedFileInfo {
    qint64 size = 0;
    qint64 mtime = 0;
    QString hash;
};

using SyncHashCache = QMap<QString, CachedFileInfo>;

/// Returns an empty cache on missing/corrupt file. Never throws.
SyncHashCache loadSyncHashCache(const QString& path);

/// Atomic. Returns false on failure (caller keeps its dirty flag set). Never throws.
bool saveSyncHashCache(const QString& path, const SyncHashCache& cache);
```

Add both to `LOGIC_SOURCES` in `launcher/CMakeLists.txt` beside the existing `tasks/SyncPlan.*`
entries. The task keeps `m_hashCache`, `m_cacheDirty`, and `getOrComputeHash()`; its
`loadHashCache()`/`saveHashCache()` become thin wrappers that supply the path and manage the flag.

> `FS::write` is safe to call from a test binary: `PSaveFile`'s destructor guards with
> `if (auto app = APPLICATION_DYN)`, so a null `APPLICATION` is handled.

### Tests — append to `tests/SyncPlan_test.cpp`, or a new `SyncHashCache_test.cpp`

Use `QTemporaryDir` for the target path.

| # | Case | Expect |
| --- | --- | --- |
| H1 | Save a 3-entry cache, load it back | round-trips exactly (size, mtime, hash) |
| H2 | Load from a path that does not exist | empty map, no crash |
| H3 | Write `{"files":{"a":` (truncated), then load | empty map, no crash |
| H4 | Write `not json at all`, then load | empty map, no crash |
| H5 | Save over an existing valid cache, kill nothing, reload | new content, no leftover `.XXXXXX` temp files in the dir |
| H6 | Save to a path whose parent is read-only | returns `false`, does not throw |

H3/H4 are the point of the exercise: they lock the self-healing property that makes this bug
low-severity in the first place. H5 catches a botched `commit()`.

If registering a separate binary:

```cmake
ecm_add_test(SyncHashCache_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME SyncHashCache)
```

---

## 5. Implementation order

One commit per step so a bisect can find a mistake.

1. **Create `SyncHashCache.{h,cpp}`** with the structs and both functions, using `FS::write` per §3.
   Move `CachedFileInfo` out of `SyncedInstanceUpdateTask`'s private section into the new header.
   Add both files to `LOGIC_SOURCES` in `launcher/CMakeLists.txt` beside `tasks/SyncPlan.*`.
   Do not wire it into the task yet.
2. **Add the tests** from §4 (H1–H6) and get them green against the new functions.
3. **Cut `SyncedInstanceUpdateTask` over.** Its `loadHashCache()`/`saveHashCache()` become thin
   wrappers that supply the path and manage `m_cacheDirty`; delete the old `QFile` bodies. Add the
   `qWarning()` on the load path per §3.
4. **Smoke test in the real app** — see §6.

---

## 6. Verification

```bash
cmake --build --preset windows_msvc --config Debug
```

```bash
ctest --preset windows_msvc --output-on-failure
```

`build_installer.bat` runs `ctest` between compile and install, so a failure blocks the installer.

Manual check: sync an instance, confirm `.synced_cache.json` is written and the *second* sync is fast
(cache hit). Then corrupt the file by hand, sync again — expect a warning in the log, a full rehash,
and a valid cache afterward.

---

## 7. Out of scope

- Moving the cache out of the instance root. It lives at `instanceRoot/.synced_cache.json` and is
  excluded from orphan cleanup at [SyncPlan.cpp:160](launcher/tasks/SyncPlan.cpp#L160). Leave it.
- Switching the cache to SQLite. Defensible eventually; not now, and not in the same change.
- The upload task's file enumeration. `QSaveFile`'s temp file is transient and the two never run
  concurrently.

---

## 8. House rules

- `clang-format` (`.clang-format`) on every changed file before committing.
- `PascalCase` types, `m_camelCase` members, `camelCase` methods.
- **Do not add a `Signed-off-by` line.** If AI assisted, the human adds
  `Assisted-by: AGENT_NAME:MODEL_VERSION`.
