#include "SyncedInstanceUpdateTask.h"
#include "SyncPlan.h"
#include "Application.h"
#include "FileSystem.h"
#include "settings/SettingsObject.h"
#include "modplatform/helpers/HashUtils.h"
#include "net/ChecksumValidator.h"
#include "net/Download.h"
#include "MMCZip.h"
#include "StringUtils.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QDateTime>

#include <QSet>
#include <memory>

class RealSyncFileSource : public ISyncFileSource {
   public:
    RealSyncFileSource(const QString& instanceRoot, SyncedInstanceUpdateTask* task)
        : m_root(instanceRoot), m_task(task)
    {
    }

    LocalFileState stat(const QString& relPath) const override
    {
        QString absPath = FS::PathCombine(m_root, relPath);
        QFileInfo info(absPath);
        if (!info.exists())
            return {false, 0};
        return {true, info.size()};
    }

    QString hash(const QString& relPath) const override
    {
        QString absPath = FS::PathCombine(m_root, relPath);
        return m_task->getOrComputeHash(relPath, absPath);
    }

    QStringList listFiles() const override
    {
        QStringList result;
        QDirIterator it(m_root, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString absPath = it.next();
            result.append(QDir(m_root).relativeFilePath(absPath));
        }
        return result;
    }

    bool dirHasContent(const QString& relPath) const override
    {
        QString absPath = FS::PathCombine(m_root, relPath);
        QDir dir(absPath);
        return dir.exists() && !dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    }

   private:
    QString m_root;
    SyncedInstanceUpdateTask* m_task;
};

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

    QString tmpDirPath = FS::PathCombine(m_instance->instanceRoot(), ".tmp");
    if (QDir(tmpDirPath).exists()) {
        QDir(tmpDirPath).removeRecursively();
    }

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

    if (!doc.isObject()) {
        emitFailed(tr("Modpack manifest is malformed."));
        return;
    }

    QJsonObject obj = doc.object();
    m_targetVersion = obj["version"].toString();
    QJsonArray files = obj["files"].toArray();
    QJsonObject voxyCacheObj = obj["voxy_cache"].toObject();

    // Apply memory/JVM overrides (side effects, stay here)
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

    // Build SyncManifest from JSON
    SyncManifest manifest;
    manifest.version = m_targetVersion;
    manifest.forceConfigOverwrite = obj["force_config_overwrite"].toBool(false);
    manifest.forceVoxyRedownload = obj["force_voxy_redownload"].toBool(false);
    manifest.voxyResetVersion = obj["voxy_cache_reset_version"].toString();
    manifest.hasVoxyCacheZip = !voxyCacheObj.isEmpty();
    if (manifest.hasVoxyCacheZip) {
        manifest.voxyZipFile = voxyCacheObj["file"].toString("voxy_cache.zip");
        manifest.voxyZipHash = voxyCacheObj["hash"].toString().toLower();
        manifest.voxyZipSize = voxyCacheObj.contains("size") ? voxyCacheObj["size"].toVariant().toLongLong() : 0;
    }

    for (int i = 0; i < files.size(); ++i) {
        QJsonObject fileObj = files[i].toObject();
        SyncManifestFile f;
        f.path = fileObj["path"].toString();
        f.hash = fileObj["hash"].toString().toLower();
        f.hasSize = fileObj.contains("size");
        f.size = f.hasSize ? fileObj["size"].toVariant().toLongLong() : 0;
        manifest.files.append(f);
    }

    QString currentVersion = m_instance->settings()->get("SyncVersion").toString();

    // Build file source — load hash cache first for the non-fast-path
    loadHashCache();

    RealSyncFileSource fileSource(m_instance->instanceRoot(), this);
    SyncPlan plan = computeSyncPlan(manifest, currentVersion, m_forceRepair, fileSource);

    if (plan.aborted) {
        emitFailed(tr(plan.abortReason.toUtf8().constData()));
        return;
    }

    if (plan.upToDate) {
        qDebug() << "Instance version matches and all files exist with expected size. Fast-path up to date.";
        emitSucceeded();
        return;
    }

    // Purge voxy hash cache entries when forced redownload is active
    bool shouldForceVoxyRedownload = !plan.voxyDirsToDelete.isEmpty() || plan.downloadVoxyZip;
    if (shouldForceVoxyRedownload) {
        for (auto it = m_hashCache.begin(); it != m_hashCache.end();) {
            if (it.key().contains(".voxy", Qt::CaseInsensitive)) {
                it = m_hashCache.erase(it);
                m_cacheDirty = true;
            } else {
                ++it;
            }
        }
    }

    // Execute voxy dir deletions
    for (const QString& relDir : plan.voxyDirsToDelete) {
        QString absDir = FS::PathCombine(m_instance->instanceRoot(), relDir);
        QDir d(absDir);
        if (d.exists()) {
            qDebug() << "Deleting local Voxy cache directory:" << absDir;
            d.removeRecursively();
        }
    }

    // Execute orphan deletions
    for (const QString& relPath : plan.toDelete) {
        QString absPath = FS::PathCombine(m_instance->instanceRoot(), relPath);
        qDebug() << "Removing orphaned synced file:" << absPath;
        QFile::remove(absPath);
        m_hashCache.remove(relPath);
        m_cacheDirty = true;
    }

    saveHashCache();

    // Build download list
    QList<FileDownloadItem> filesToDownload;
    for (const auto& f : plan.toDownload) {
        filesToDownload.append({f.path, f.hash, f.size});
    }

    // Voxy zip download
    if (plan.downloadVoxyZip) {
        m_hasVoxyZipDownload = true;
        m_voxyZipPath = FS::PathCombine(m_instance->instanceRoot(), plan.voxyZipRelPath);
        m_voxyExtractDir = FS::PathCombine(m_instance->instanceRoot(), plan.voxyExtractDir);
        filesToDownload.append({plan.voxyZipRelPath, manifest.voxyZipHash, manifest.voxyZipSize});
    }

    if (filesToDownload.isEmpty()) {
        qDebug() << "No files to download, up to date.";
        m_instance->settings()->set("SyncVersion", m_targetVersion);
        m_instance->settings()->set("SyncVersionName", m_targetVersion);
        m_instance->saveNow();
        emitSucceeded();
        return;
    }

    m_downloadedFiles = filesToDownload;

    QString shortcode = m_instance->settings()->get("SyncShortcode").toString();
    QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    m_totalDownloadBytes = 0;
    m_completedDownloadBytes = 0;
    for (const auto& item : filesToDownload) {
        m_totalDownloadBytes += item.size;
    }

    int totalFiles = filesToDownload.size();
    if (m_totalDownloadBytes > 0) {
        setStatus(tr("Downloading updates: 0 B / %1").arg(StringUtils::humanReadableFileSize(m_totalDownloadBytes)));
        setProgress(0, m_totalDownloadBytes);
    } else {
        setStatus(tr("Downloading updates (0/%1)...").arg(totalFiles));
        setProgress(0, totalFiles);
    }

    m_downloadJob.reset(new NetJob(tr("Downloading pack updates"), APPLICATION->network()));

    auto currentFileIndex = std::make_shared<int>(0);
    auto activeBytes = std::make_shared<QVector<qint64>>(filesToDownload.size(), 0);

    for (int i = 0; i < filesToDownload.size(); ++i) {
        const auto& item = filesToDownload[i];
        int fileIdx = i;
        QString relPath = item.relPath;
        QString hash = item.hash;
        qint64 expectedSize = item.size;
        QString fileName = QFileInfo(relPath).fileName();

        QString destPath = m_instance->instanceRoot() + "/" + relPath;
        QFileInfo info(destPath);
        QDir().mkpath(info.dir().absolutePath());

        QString fileUrl = publicUrl + "packs/" + shortcode + "/" + (relPath.startsWith(".tmp/") ? relPath.mid(5) : relPath);
        auto download = Net::Download::makeFile(QUrl(fileUrl), destPath);
        download->setExpectedSize(expectedSize);
        download->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, hash));

        connect(download.get(), &Task::started, this, [this, relPath, fileName, currentFileIndex, totalFiles]() {
            (*currentFileIndex)++;
            setDetails(relPath);
        });

        connect(download.get(), &Task::progress, this, [this, fileName, expectedSize, activeBytes, fileIdx](qint64 current, qint64 total) {
            if (current >= 0) {
                qint64 fileTotal = (expectedSize > 0) ? expectedSize : total;
                fileTotal = std::max({fileTotal, current, expectedSize});
                (*activeBytes)[fileIdx] = current;

                if (fileTotal > 0) {
                    QString curStr = StringUtils::humanReadableFileSize(current);
                    QString totStr = StringUtils::humanReadableFileSize(fileTotal);
                    setDetails(tr("%1 (%2 / %3)").arg(fileName, curStr, totStr));
                }

                if (m_totalDownloadBytes > 0) {
                    qint64 currentTotal = 0;
                    for (qint64 b : *activeBytes) {
                        currentTotal += b;
                    }
                    currentTotal = qBound<qint64>(0, currentTotal, m_totalDownloadBytes);
                    QString curTotalStr = StringUtils::humanReadableFileSize(currentTotal);
                    QString maxTotalStr = StringUtils::humanReadableFileSize(m_totalDownloadBytes);
                    setStatus(tr("Downloading updates: %1 / %2").arg(curTotalStr, maxTotalStr));
                    setProgress(currentTotal, m_totalDownloadBytes);
                }
            }
        });

        connect(download.get(), &Task::succeeded, this, [this, expectedSize, activeBytes, fileIdx]() {
            (*activeBytes)[fileIdx] = (expectedSize > 0) ? expectedSize : (*activeBytes)[fileIdx];
            if (m_totalDownloadBytes > 0) {
                qint64 currentTotal = 0;
                for (qint64 b : *activeBytes) {
                    currentTotal += b;
                }
                currentTotal = qBound<qint64>(0, currentTotal, m_totalDownloadBytes);
                setProgress(currentTotal, m_totalDownloadBytes);
            }
        });

        m_downloadJob->addNetAction(download);
    }

    connect(m_downloadJob.get(), &NetJob::succeeded, this, &SyncedInstanceUpdateTask::downloadSucceeded);
    connect(m_downloadJob.get(), &NetJob::failed, this, &SyncedInstanceUpdateTask::downloadFailed);

    m_downloadJob->start();
}

void SyncedInstanceUpdateTask::downloadSucceeded()
{
    if (m_hasVoxyZipDownload && QFile::exists(m_voxyZipPath)) {
        setStatus(tr("Extracting Voxy cache..."));
        setDetails(tr("Extracting %1 into game directory...").arg("voxy_cache.zip"));
        setProgress(0, 0);
        qDebug() << "Extracting Voxy cache from" << m_voxyZipPath << "to" << m_voxyExtractDir;
        QDir(m_voxyExtractDir).removeRecursively();
        QDir().mkpath(m_voxyExtractDir);
        MMCZip::extractDir(m_voxyZipPath, m_voxyExtractDir);
        QFile::remove(m_voxyZipPath);
    }

    QString tmpDirPath = FS::PathCombine(m_instance->instanceRoot(), ".tmp");
    if (QDir(tmpDirPath).exists()) {
        QDir(tmpDirPath).removeRecursively();
    }

    for (const auto& item : m_downloadedFiles) {
        QString relPath = item.relPath;
        if (relPath.startsWith(".tmp/")) {
            continue;
        }
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
    QString tmpDirPath = FS::PathCombine(m_instance->instanceRoot(), ".tmp");
    if (QDir(tmpDirPath).exists()) {
        QDir(tmpDirPath).removeRecursively();
    }
    emitFailed(reason);
}

void SyncedInstanceUpdateTask::downloadProgress(qint64 current, qint64 total)
{
    if (m_totalDownloadBytes <= 0 && total > 0 && current >= 0) {
        setProgress(current, total);
        QString currentStr = StringUtils::humanReadableFileSize(current);
        QString totalStr = StringUtils::humanReadableFileSize(total);
        int percent = qBound(0, static_cast<int>((current * 100) / total), 100);
        setStatus(tr("Downloading updates: %1 / %2 (%3%)").arg(currentStr, totalStr, QString::number(percent)));
    }
}
