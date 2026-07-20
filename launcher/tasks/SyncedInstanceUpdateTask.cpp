#include "SyncedInstanceUpdateTask.h"
#include "Application.h"
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

void SyncedInstanceUpdateTask::executeTask()
{
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

    QString currentVersion = m_instance->settings()->get("SyncVersion").toString();
    qDebug() << "Local version:" << currentVersion << "Target version:" << m_targetVersion;

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

        QString localPath = m_instance->instanceRoot() + "/" + relPath;
        bool needsDownload = true;

        if (QFile::exists(localPath)) {
            QString localHash = Hashing::hash(localPath, Hashing::Algorithm::Sha1).toLower();
            if (localHash == hash) {
                needsDownload = false;
            }
        }

        if (needsDownload) {
            filesToDownload.append({relPath, hash});
        }
    }

    // Clean up any files that are no longer in the manifest
    deleteOrphanedFiles(filesToKeep);

    if (filesToDownload.isEmpty()) {
        qDebug() << "No files to download, up to date.";
        m_instance->settings()->set("SyncVersion", m_targetVersion);
        m_instance->settings()->set("SyncVersionName", m_targetVersion);
        m_instance->saveNow();
        emitSucceeded();
        return;
    }

    setStatus(tr("Downloading updates..."));
    setProgress(0, filesToDownload.size());

    m_downloadJob.reset(new NetJob(tr("Downloading pack updates"), APPLICATION->network()));

    for (const auto& pair : filesToDownload) {
        QString relPath = pair.first;
        QString hash = pair.second;

        QString destPath = m_instance->instanceRoot() + "/" + relPath;
        QFileInfo info(destPath);
        QDir().mkpath(info.dir().absolutePath());

        QString fileUrl = publicUrl + "packs/" + shortcode + "/" + relPath;
        auto download = Net::Download::makeFile(QUrl(fileUrl), destPath);
        download->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, hash));
        m_downloadJob->addNetAction(download);
    }

    connect(m_downloadJob.get(), &NetJob::succeeded, this, &SyncedInstanceUpdateTask::downloadSucceeded);
    connect(m_downloadJob.get(), &NetJob::failed, this, &SyncedInstanceUpdateTask::downloadFailed);
    connect(m_downloadJob.get(), &NetJob::progress, this, &SyncedInstanceUpdateTask::downloadProgress);

    m_downloadJob->start();
}

void SyncedInstanceUpdateTask::deleteOrphanedFiles(const QStringList& filesToKeep)
{
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
            relPath.endsWith("instance.cfg", Qt::CaseInsensitive)) {
            continue;
        }

        // Only cleanup within mods, config, resourcepacks, shaderpacks, options.txt, servers.dat
        if (!relPath.startsWith("minecraft/mods/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/config/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/resourcepacks/", Qt::CaseInsensitive) &&
            !relPath.startsWith("minecraft/shaderpacks/", Qt::CaseInsensitive) &&
            relPath != "minecraft/options.txt" &&
            relPath != "minecraft/servers.dat") {
            continue;
        }

        if (!filesToKeep.contains(relPath, Qt::CaseInsensitive)) {
            qDebug() << "Removing orphaned synced file:" << absPath;
            QFile::remove(absPath);
        }
    }
}

void SyncedInstanceUpdateTask::downloadSucceeded()
{
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
