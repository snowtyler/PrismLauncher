#include "SyncedInstanceUpdateTask.h"
#include "Application.h"
#include "FileSystem.h"
#include "settings/SettingsObject.h"
#include "modplatform/helpers/HashUtils.h"
#include "net/ChecksumValidator.h"
#include "net/Download.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QDateTime>

#include <QSet>
#include <memory>

SyncedInstanceUpdateTask::SyncedInstanceUpdateTask(BaseInstance* instance)
    : Task(true), m_instance(instance)
{
}

bool SyncedInstanceUpdateTask::abort()
{
    if (m_manifestReply) {
        m_manifestReply->abort();
        return true;
    }
    if (m_downloadJob) {
        return m_downloadJob->abort();
    }
    return false;
}

void SyncedInstanceUpdateTask::loadHashCache()
{
    m_hashCache.clear();
    m_cacheDirty = false;

    QString cachePath = m_instance->instanceRoot() + "/.synced_cache.json";
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (doc.isNull() || !doc.isObject()) {
        return;
    }

    QJsonObject rootObj = doc.object();
    QJsonObject filesObj = rootObj["files"].toObject();
    for (auto it = filesObj.begin(); it != filesObj.end(); ++it) {
        QJsonObject fileInfo = it.value().toObject();
        CachedFileInfo info;
        info.size = fileInfo["size"].toVariant().toLongLong();
        info.mtime = fileInfo["mtime"].toVariant().toLongLong();
        info.hash = fileInfo["hash"].toString().toLower();
        m_hashCache.insert(it.key(), info);
    }
}

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

    QJsonDocument doc(rootObj);
    QString cachePath = m_instance->instanceRoot() + "/.synced_cache.json";
    QFile file(cachePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson(QJsonDocument::Compact));
        file.close();
        m_cacheDirty = false;
    }
}

QString SyncedInstanceUpdateTask::getOrComputeHash(const QString& relPath, const QString& absPath)
{
    QFileInfo info(absPath);
    if (!info.exists()) {
        return QString();
    }

    qint64 size = info.size();
    qint64 mtime = info.lastModified().toMSecsSinceEpoch();

    if (m_hashCache.contains(relPath)) {
        const auto& cached = m_hashCache[relPath];
        if (cached.size == size && cached.mtime == mtime && !cached.hash.isEmpty()) {
            return cached.hash;
        }
    }

    QString calculatedHash = Hashing::hash(absPath, Hashing::Algorithm::Sha1).toLower();
    if (!calculatedHash.isEmpty()) {
        m_hashCache[relPath] = CachedFileInfo{ size, mtime, calculatedHash };
        m_cacheDirty = true;
    }
    return calculatedHash;
}

void SyncedInstanceUpdateTask::executeTask()
{
    if (!m_instance || !m_instance->settings()) {
        emitFailed(tr("Instance reference or settings object is invalid."));
        return;
    }

    setStatus(tr("Checking for modpack updates..."));

    QString shortcode = m_instance->settings()->get("SyncShortcode").toString();
    if (shortcode.isEmpty()) {
        emitFailed(tr("Instance does not have a synchronization shortcode configured."));
        return;
    }

    QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    QUrl url(publicUrl + "shortcodes/" + shortcode + ".json?t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    qDebug() << "Fetching manifest from:" << url.toString();

    m_manifestReply = APPLICATION->network()->get(QNetworkRequest(url));
    connect(m_manifestReply, &QNetworkReply::finished, this, &SyncedInstanceUpdateTask::manifestFetched);
}

void SyncedInstanceUpdateTask::manifestFetched()
{
    if (!m_manifestReply) {
        return;
    }

    m_manifestReply->deleteLater();
    auto reply = m_manifestReply;
    m_manifestReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        emitFailed(tr("Failed to fetch modpack manifest: %1").arg(reply->errorString()));
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (doc.isNull()) {
        emitFailed(tr("Failed to parse modpack manifest: %1").arg(err.errorString()));
        return;
    }

    QJsonObject obj = doc.object();
    m_targetVersion = obj["version"].toString();
    QJsonArray files = obj["files"].toArray();
    bool forceConfigOverwrite = obj["force_config_overwrite"].toBool(false);
    bool forceVoxyRedownload = obj["force_voxy_redownload"].toBool(false);

    bool overrideMem = obj["override_memory"].toBool(false);
    if (overrideMem) {
        m_instance->settings()->set("OverrideMemory", true);
        if (obj.contains("min_memory")) {
            m_instance->settings()->set("MinMemAlloc", obj["min_memory"].toInt(1024));
        }
        if (obj.contains("max_memory")) {
            m_instance->settings()->set("MaxMemAlloc", obj["max_memory"].toInt(4096));
        }
    }

    bool overrideArgs = obj["override_java_args"].toBool(false);
    if (overrideArgs) {
        m_instance->settings()->set("OverrideJavaArgs", true);
        if (obj.contains("jvm_args")) {
            m_instance->settings()->set("JvmArgs", obj["jvm_args"].toString().trimmed());
        }
    }
    m_instance->saveNow();

    QString currentVersion = m_instance->settings()->get("SyncVersion").toString();
    qDebug() << "Local version:" << currentVersion << "Target version:" << m_targetVersion
             << "Force config overwrite:" << forceConfigOverwrite << "Force Voxy redownload:" << forceVoxyRedownload;

    if (forceVoxyRedownload) {
        qDebug() << "Force Voxy cache redownload requested. Deleting local Voxy cache directories...";
        QSet<QString> voxyDirsToDelete;
        voxyDirsToDelete.insert(FS::PathCombine(m_instance->instanceRoot(), "minecraft/.voxy/saves/cozycreations.modpack.gg"));
        voxyDirsToDelete.insert(FS::PathCombine(m_instance->instanceRoot(), ".voxy/saves/cozycreations.modpack.gg"));
        for (int i = 0; i < files.size(); ++i) {
            QJsonObject fileObj = files[i].toObject();
            QString relPath = fileObj["path"].toString();
            QString cleanPath = QString(relPath).replace('\\', '/');
            int idx = cleanPath.indexOf(".voxy/saves/", 0, Qt::CaseInsensitive);
            if (idx != -1) {
                int nextSlash = cleanPath.indexOf('/', idx + 12);
                QString voxySub = (nextSlash != -1) ? cleanPath.left(nextSlash) : cleanPath;
                voxyDirsToDelete.insert(FS::PathCombine(m_instance->instanceRoot(), voxySub));
            }
        }
        for (const QString& dirPath : voxyDirsToDelete) {
            QDir d(dirPath);
            if (d.exists()) {
                qDebug() << "Deleting local Voxy cache directory:" << dirPath;
                d.removeRecursively();
            }
        }
    }

    // Helper to check if Voxy cache directory has already been seeded on disk
    auto isVoxyCacheSeeded = [this]() {
        QString path1 = FS::PathCombine(m_instance->instanceRoot(), "minecraft/.voxy/saves/cozycreations.modpack.gg");
        QString path2 = FS::PathCombine(m_instance->instanceRoot(), ".voxy/saves/cozycreations.modpack.gg");
        QDir dir1(path1);
        QDir dir2(path2);
        if (dir1.exists() && !dir1.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            return true;
        }
        if (dir2.exists() && !dir2.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            return true;
        }
        return false;
    };

    bool voxySeeded = isVoxyCacheSeeded();
    qDebug() << "Voxy cache seeded status:" << voxySeeded;

    // Fast-Path: If local version matches target version and non-empty, check file existence & size (unless force repair or force voxy redownload is requested)
    if (!m_forceRepair && !forceVoxyRedownload && !currentVersion.isEmpty() && currentVersion == m_targetVersion) {
        bool allPresentAndMatchingSize = true;
        for (int i = 0; i < files.size(); ++i) {
            QJsonObject fileObj = files[i].toObject();
            QString relPath = fileObj["path"].toString();
            QString cleanPath = QString(relPath).replace('\\', '/');

            bool isCacheFolder = cleanPath.contains(".voxy/saves/cozycreations.modpack.gg", Qt::CaseInsensitive);

            // If Voxy cache folder is already seeded on disk, skip existence/size checks for files inside it
            if (isCacheFolder && voxySeeded) {
                continue;
            }

            QString localPath = FS::PathCombine(m_instance->instanceRoot(), relPath);
            QFileInfo info(localPath);
            if (!info.exists()) {
                allPresentAndMatchingSize = false;
                break;
            }

            bool isOptionsFile = cleanPath.endsWith("options.txt", Qt::CaseInsensitive);

            if (!isCacheFolder && !(isOptionsFile && !forceConfigOverwrite) && fileObj.contains("size")) {
                qint64 expectedSize = fileObj["size"].toVariant().toLongLong();
                if (info.size() != expectedSize) {
                    allPresentAndMatchingSize = false;
                    break;
                }
            }
        }

        if (allPresentAndMatchingSize) {
            qDebug() << "Instance version matches and all files exist with expected size. Fast-path up to date.";
            emitSucceeded();
            return;
        }
    }

    loadHashCache();
    if (forceVoxyRedownload) {
        for (auto it = m_hashCache.begin(); it != m_hashCache.end(); ) {
            if (it.key().contains(".voxy", Qt::CaseInsensitive)) {
                it = m_hashCache.erase(it);
                m_cacheDirty = true;
            } else {
                ++it;
            }
        }
    }

    QString shortcode = m_instance->settings()->get("SyncShortcode").toString();
    QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    QStringList filesToKeep;
    QList<QPair<QString, QString>> filesToDownload; // pair: <relPath, hash>

    for (int i = 0; i < files.size(); ++i) {
        QJsonObject fileObj = files[i].toObject();
        QString relPath = fileObj["path"].toString();
        QString hash = fileObj["hash"].toString().toLower();

        filesToKeep.append(relPath);

        QString cleanPath = QString(relPath).replace('\\', '/');
        QString localPath = FS::PathCombine(m_instance->instanceRoot(), relPath);
        QFileInfo info(localPath);
        bool needsDownload = false;
        bool isCacheFolder = cleanPath.contains(".voxy/saves/cozycreations.modpack.gg", Qt::CaseInsensitive);
        bool isOptionsFile = cleanPath.endsWith("options.txt", Qt::CaseInsensitive);

        if (isCacheFolder && voxySeeded && !m_forceRepair && !forceVoxyRedownload) {
            // Voxy cache directory is already seeded & present on disk:
            // Do not re-download individual files inside it (RocksDB compaction removes old .sst files)
            needsDownload = false;
        } else if (!info.exists()) {
            needsDownload = true;
        } else if (!m_forceRepair && !forceVoxyRedownload && isCacheFolder) {
            // Cache files exist locally and force repair is not active: preserve local modifications
            needsDownload = false;
        } else if (!m_forceRepair && isOptionsFile && !forceConfigOverwrite) {
            // options.txt exists locally, force repair is not active, and update does not force config overwrite: preserve local modifications
            needsDownload = false;
        } else {
            if (fileObj.contains("size")) {
                qint64 expectedSize = fileObj["size"].toVariant().toLongLong();
                if (info.size() != expectedSize) {
                    needsDownload = true;
                }
            }

            if (!needsDownload) {
                QString localHash = getOrComputeHash(relPath, localPath);
                if (localHash != hash) {
                    needsDownload = true;
                }
            }
        }

        if (needsDownload) {
            filesToDownload.append({relPath, hash});
        }
    }

    // Clean up any files that are no longer in the manifest
    deleteOrphanedFiles(filesToKeep);
    saveHashCache();

    if (filesToDownload.isEmpty()) {
        qDebug() << "No files to download, up to date.";
        m_instance->settings()->set("SyncVersion", m_targetVersion);
        m_instance->settings()->set("SyncVersionName", m_targetVersion);
        m_instance->saveNow();
        emitSucceeded();
        return;
    }

    m_downloadedFiles = filesToDownload;

    int totalFiles = filesToDownload.size();
    setStatus(tr("Downloading updates (0/%1)...").arg(totalFiles));
    setProgress(0, totalFiles);

    m_downloadJob.reset(new NetJob(tr("Downloading pack updates"), APPLICATION->network()));

    auto currentFileIndex = std::make_shared<int>(0);

    for (const auto& pair : filesToDownload) {
        QString relPath = pair.first;
        QString hash = pair.second;
        QString fileName = QFileInfo(relPath).fileName();

        QString destPath = m_instance->instanceRoot() + "/" + relPath;
        QFileInfo info(destPath);
        QDir().mkpath(info.dir().absolutePath());

        QString fileUrl = publicUrl + "packs/" + shortcode + "/" + relPath;
        auto download = Net::Download::makeFile(QUrl(fileUrl), destPath);
        download->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, hash));

        connect(download.get(), &Task::started, this, [this, relPath, fileName, currentFileIndex, totalFiles]() {
            (*currentFileIndex)++;
            setStatus(tr("Downloading (%1/%2): %3").arg(*currentFileIndex).arg(totalFiles).arg(fileName));
            setDetails(relPath);
        });

        m_downloadJob->addNetAction(download);
    }

    connect(m_downloadJob.get(), &NetJob::succeeded, this, &SyncedInstanceUpdateTask::downloadSucceeded);
    connect(m_downloadJob.get(), &NetJob::failed, this, &SyncedInstanceUpdateTask::downloadFailed);
    connect(m_downloadJob.get(), &NetJob::progress, this, &SyncedInstanceUpdateTask::downloadProgress);

    m_downloadJob->start();
}

void SyncedInstanceUpdateTask::deleteOrphanedFiles(const QStringList& filesToKeep)
{
    QSet<QString> filesToKeepSet;
    for (const QString& path : filesToKeep) {
        filesToKeepSet.insert(path.toLower());
    }

    QDirIterator it(m_instance->instanceRoot(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString absPath = it.next();
        QString relPath = QDir(m_instance->instanceRoot()).relativeFilePath(absPath);

        // Exclude patterns
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

        // Only cleanup within mods, config, resourcepacks, shaderpacks
        if (!relPath.startsWith("minecraft/mods/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/config/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/resourcepacks/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/shaderpacks/", Qt::CaseInsensitive)) {
            continue;
        }

        if (!filesToKeepSet.contains(relPath.toLower())) {
            qDebug() << "Removing orphaned synced file:" << absPath;
            QFile::remove(absPath);
            m_hashCache.remove(relPath);
            m_cacheDirty = true;
        }
    }
}

void SyncedInstanceUpdateTask::downloadSucceeded()
{
    for (const auto& pair : m_downloadedFiles) {
        QString relPath = pair.first;
        QString absPath = m_instance->instanceRoot() + "/" + relPath;
        QFileInfo info(absPath);
        if (info.exists()) {
            getOrComputeHash(relPath, absPath);
        }
    }
    saveHashCache();

    m_instance->settings()->set("SyncVersion", m_targetVersion);
    m_instance->settings()->set("SyncVersionName", m_targetVersion);
    m_instance->saveNow();
    emitSucceeded();
}

void SyncedInstanceUpdateTask::downloadFailed(QString reason)
{
    emitFailed(reason);
}

void SyncedInstanceUpdateTask::downloadProgress(qint64 current, qint64 total)
{
    setProgress(current, total);
}

