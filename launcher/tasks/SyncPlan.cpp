#include "SyncPlan.h"
#include "Version.h"

#include <QSet>

SyncPlan computeSyncPlan(const SyncManifest& manifest,
                         const QString& currentVersion,
                         bool forceRepair,
                         const ISyncFileSource& local)
{
    SyncPlan plan;

    if (manifest.files.isEmpty()) {
        plan.aborted = true;
        plan.abortReason = "Modpack manifest contains no files; refusing to sync.";
        return plan;
    }

    bool isNewVersion = currentVersion.isEmpty() || currentVersion != manifest.version;
    bool shouldForceConfigOverwrite = manifest.forceConfigOverwrite && (isNewVersion || forceRepair);

    bool voxySeeded = local.dirHasContent("minecraft/.voxy/saves/cozycreations.modpack.gg") ||
                      local.dirHasContent(".voxy/saves/cozycreations.modpack.gg");

    bool shouldForceVoxyRedownload = false;
    if (forceRepair || !voxySeeded) {
        shouldForceVoxyRedownload = true;
    } else if (isNewVersion) {
        if (!manifest.voxyResetVersion.isEmpty()) {
            if (currentVersion.isEmpty() || Version(currentVersion) < Version(manifest.voxyResetVersion)) {
                shouldForceVoxyRedownload = true;
            }
        } else if (manifest.forceVoxyRedownload) {
            shouldForceVoxyRedownload = true;
        }
    }

    // Legacy Voxy dir collection (when forced redownload without zip)
    if (shouldForceVoxyRedownload && !manifest.hasVoxyCacheZip) {
        QSet<QString> voxyDirs;
        voxyDirs.insert("minecraft/.voxy/saves/cozycreations.modpack.gg");
        voxyDirs.insert(".voxy/saves/cozycreations.modpack.gg");
        for (const auto& file : manifest.files) {
            QString cleanPath = QString(file.path).replace('\\', '/');
            int idx = cleanPath.indexOf(".voxy/saves/", 0, Qt::CaseInsensitive);
            if (idx != -1) {
                int nextSlash = cleanPath.indexOf('/', idx + 12);
                QString voxySub = (nextSlash != -1) ? cleanPath.left(nextSlash) : cleanPath;
                voxyDirs.insert(voxySub);
            }
        }
        plan.voxyDirsToDelete = voxyDirs.values();
    }

    // Fast path
    if (!forceRepair && !shouldForceVoxyRedownload && !currentVersion.isEmpty() && currentVersion == manifest.version) {
        bool allPresent = true;
        for (const auto& file : manifest.files) {
            QString cleanPath = QString(file.path).replace('\\', '/');
            bool isCacheFolder = cleanPath.contains(".voxy/saves/cozycreations.modpack.gg", Qt::CaseInsensitive);

            if (isCacheFolder && voxySeeded) {
                continue;
            }

            LocalFileState state = local.stat(file.path);
            if (!state.exists) {
                allPresent = false;
                break;
            }

            bool isOptionsFile = cleanPath.endsWith("options.txt", Qt::CaseInsensitive);
            if (!isCacheFolder && !(isOptionsFile && !shouldForceConfigOverwrite) && file.hasSize) {
                if (state.size != file.size) {
                    allPresent = false;
                    break;
                }
            }
        }

        if (allPresent) {
            plan.upToDate = true;
            return plan;
        }
    }

    // Build keep set for orphan detection
    QSet<QString> filesToKeepSet;

    // Per-file download decisions
    for (const auto& file : manifest.files) {
        filesToKeepSet.insert(file.path.toLower());

        QString cleanPath = QString(file.path).replace('\\', '/');
        bool needsDownload = false;
        bool isCacheFolder = cleanPath.contains(".voxy", Qt::CaseInsensitive) || cleanPath.endsWith(".sst", Qt::CaseInsensitive);
        bool isOptionsFile = cleanPath.endsWith("options.txt", Qt::CaseInsensitive);

        LocalFileState state = local.stat(file.path);

        if (manifest.hasVoxyCacheZip && isCacheFolder) {
            needsDownload = false;
        } else if (isCacheFolder && voxySeeded && !forceRepair && !shouldForceVoxyRedownload) {
            needsDownload = false;
        } else if (isCacheFolder && shouldForceVoxyRedownload) {
            needsDownload = true;
        } else if (!state.exists) {
            needsDownload = true;
        } else if (!forceRepair && !shouldForceVoxyRedownload && isCacheFolder) {
            needsDownload = false;
        } else if (!forceRepair && isOptionsFile && !shouldForceConfigOverwrite) {
            needsDownload = false;
        } else {
            if (file.hasSize) {
                if (state.size != file.size) {
                    needsDownload = true;
                }
            }

            if (!needsDownload) {
                QString localHash = local.hash(file.path);
                if (localHash != file.hash) {
                    needsDownload = true;
                }
            }
        }

        if (needsDownload) {
            plan.toDownload.append(file);
        }
    }

    // Voxy zip download
    if (manifest.hasVoxyCacheZip && shouldForceVoxyRedownload) {
        plan.downloadVoxyZip = true;
        plan.voxyZipRelPath = ".tmp/" + (manifest.voxyZipFile.isEmpty() ? "voxy_cache.zip" : manifest.voxyZipFile);
        if (local.dirHasContent("minecraft")) {
            plan.voxyExtractDir = "minecraft/.voxy/saves/cozycreations.modpack.gg";
        } else {
            plan.voxyExtractDir = ".voxy/saves/cozycreations.modpack.gg";
        }
    }

    // Orphan detection
    QStringList allLocalFiles = local.listFiles();
    for (const QString& relPath : allLocalFiles) {
        if (relPath.startsWith("minecraft/saves/", Qt::CaseInsensitive) ||
            relPath.startsWith("minecraft/screenshots/", Qt::CaseInsensitive) ||
            relPath.startsWith("minecraft/logs/", Qt::CaseInsensitive) ||
            relPath.startsWith("minecraft/crash-reports/", Qt::CaseInsensitive) ||
            relPath.startsWith("minecraft/backups/", Qt::CaseInsensitive) ||
            relPath.startsWith("minecraft/local/", Qt::CaseInsensitive) ||
            relPath.startsWith(".tmp/", Qt::CaseInsensitive) ||
            relPath.endsWith("options.txt", Qt::CaseInsensitive) ||
            relPath.endsWith("optionsshaders.txt", Qt::CaseInsensitive) ||
            relPath.endsWith("optionsof.txt", Qt::CaseInsensitive) ||
            relPath.endsWith("servers.dat", Qt::CaseInsensitive) ||
            relPath.endsWith("servers.dat_old", Qt::CaseInsensitive) ||
            relPath.endsWith("instance.cfg", Qt::CaseInsensitive) ||
            relPath.endsWith(".synced_cache.json", Qt::CaseInsensitive)) {
            continue;
        }

        if (!relPath.startsWith("minecraft/mods/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/config/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/resourcepacks/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/shaderpacks/", Qt::CaseInsensitive)) {
            continue;
        }

        if (!filesToKeepSet.contains(relPath.toLower())) {
            plan.toDelete.append(relPath);
        }
    }

    return plan;
}
