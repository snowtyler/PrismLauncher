#include "SyncedInstanceUploadTask.h"
#include "Application.h"
#include "settings/SettingsObject.h"
#include "modplatform/helpers/HashUtils.h"
#include "net/R2Signer.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/Component.h"
#include "FileSystem.h"
#include "MMCZip.h"
#include "archive/ArchiveWriter.h"
#include "StringUtils.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QDateTime>

#include <QtConcurrent>
#include <QThreadPool>

SyncedInstanceUploadTask::SyncedInstanceUploadTask(BaseInstance* instance, const QString& accessKey, const QString& secretKey, const QStringList& selectedFiles, const QString& bannerImagePath, bool forceConfigOverwrite, bool forceVoxyRedownload)
    : Task(true), m_instance(instance), m_accessKey(accessKey), m_secretKey(secretKey), m_bannerImagePath(bannerImagePath), m_forceConfigOverwrite(forceConfigOverwrite), m_forceVoxyRedownload(forceVoxyRedownload), m_selectedFiles(selectedFiles)
{
    m_shortcode = m_instance->settings()->get("SyncShortcode").toString();
    m_bucket = APPLICATION->settings()->get("SyncR2Bucket").toString();
    m_publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();

    m_endpoint = APPLICATION->settings()->get("SyncR2Endpoint").toString();
    if (m_endpoint.endsWith('/')) {
        m_endpoint.chop(1);
    }
    QString bucketSuffix = "/" + m_bucket;
    if (m_endpoint.endsWith(bucketSuffix, Qt::CaseInsensitive)) {
        m_endpoint.chop(bucketSuffix.length());
    }
}

bool SyncedInstanceUploadTask::abort()
{
    m_aborted = true;
    m_diffWatcher.cancel();
    if (m_manifestReply) {
        m_manifestReply->abort();
        return true;
    }
    if (m_currentActionReply) {
        m_currentActionReply->abort();
        return true;
    }
    return true;
}

void SyncedInstanceUploadTask::executeTask()
{
    if (m_shortcode.isEmpty()) {
        emitFailed(tr("Instance does not have a synchronization shortcode configured."));
        return;
    }
    if (m_accessKey.isEmpty() || m_secretKey.isEmpty()) {
        emitFailed(tr("Cloudflare R2 Access Key and Secret Key are required for uploading."));
        return;
    }

    setStatus(tr("Fetching remote manifest to compute diff..."));
    fetchRemoteManifest();
}

void SyncedInstanceUploadTask::fetchRemoteManifest()
{
    QString publicUrl = m_publicUrl;
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    QUrl url(publicUrl + "shortcodes/" + m_shortcode + ".json?t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    m_manifestReply = APPLICATION->network()->get(req);
    connect(m_manifestReply, &QNetworkReply::finished, this, &SyncedInstanceUploadTask::remoteManifestFetched);
}

void SyncedInstanceUploadTask::remoteManifestFetched()
{
    if (!m_manifestReply) {
        return;
    }

    m_manifestReply->deleteLater();
    auto reply = m_manifestReply;
    m_manifestReply = nullptr;

    if (m_aborted) {
        emitAborted();
        return;
    }

    QMap<QString, QString> remoteHashes;

    // A 404 error is acceptable; it means this is a new modpack upload
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject remoteObj = doc.object();
            QString remoteVoxyResetVer = remoteObj["voxy_cache_reset_version"].toString();
            bool remoteForceVoxy = remoteObj["force_voxy_redownload"].toBool(false);
            QString remoteVersion = remoteObj["version"].toString();
            if (remoteVoxyResetVer.isEmpty() && remoteForceVoxy && !remoteVersion.isEmpty()) {
                remoteVoxyResetVer = remoteVersion;
            }
            m_remoteVoxyResetVersion = remoteVoxyResetVer;

            QJsonArray files = remoteObj["files"].toArray();
            for (int i = 0; i < files.size(); ++i) {
                QJsonObject fileObj = files[i].toObject();
                remoteHashes[fileObj["path"].toString()] = fileObj["hash"].toString().toLower();
            }
        }
    } else {
        qDebug() << "Manifest fetch returned error or 404 (acceptable for new packs):" << reply->errorString();
    }

    setStatus(tr("Comparing files and computing checksums..."));
    setProgress(0, m_selectedFiles.size());

    connect(&m_diffWatcher, &QFutureWatcher<DiffResult>::finished, this, &SyncedInstanceUploadTask::diffComputed, Qt::UniqueConnection);
    QFuture<DiffResult> future = QtConcurrent::run(QThreadPool::globalInstance(), [this, remoteHashes]() {
        return computeDiff(remoteHashes);
    });
    m_diffWatcher.setFuture(future);
}

SyncedInstanceUploadTask::DiffResult SyncedInstanceUploadTask::computeDiff(const QMap<QString, QString>& remoteHashes)
{
    DiffResult result;
    QSet<QString> localRelPaths;
    int total = m_selectedFiles.size();

    // Check if any selected files are inside Voxy cache directory
    QString voxyRelPrefix;
    for (const QString& relPath : m_selectedFiles) {
        QString clean = QString(relPath).replace('\\', '/');
        if (clean.contains(".voxy/saves/cozycreations.modpack.gg", Qt::CaseInsensitive)) {
            if (clean.startsWith("minecraft/", Qt::CaseInsensitive)) {
                voxyRelPrefix = "minecraft/.voxy/saves/cozycreations.modpack.gg";
            } else {
                voxyRelPrefix = ".voxy/saves/cozycreations.modpack.gg";
            }
            break;
        }
    }

    if (!voxyRelPrefix.isEmpty()) {
        QString voxyAbsDir = FS::PathCombine(m_instance->instanceRoot(), voxyRelPrefix);
        if (QDir(voxyAbsDir).exists()) {
            setStatus(tr("Packaging Voxy cache into archive..."));
            QString tempDir = FS::PathCombine(m_instance->instanceRoot(), ".tmp");
            QDir().mkpath(tempDir);
            QString tempZipPath = FS::PathCombine(tempDir, "voxy_cache.zip");
            FS::deletePath(tempZipPath);

            QFileInfoList voxyFilesList;
            MMCZip::collectFileListRecursively(voxyAbsDir, nullptr, &voxyFilesList, nullptr);

            if (!voxyFilesList.isEmpty()) {
                MMCZip::ArchiveWriter zip(tempZipPath);
                if (zip.open()) {
                    QDir baseDir(voxyAbsDir);
                    for (const auto& fi : voxyFilesList) {
                        QString entryRel = baseDir.relativeFilePath(fi.absoluteFilePath());
                        zip.addFile(fi.absoluteFilePath(), entryRel);
                    }
                    zip.close();

                    QString zipHash = Hashing::hash(tempZipPath, Hashing::Algorithm::Sha1).toLower();
                    qint64 zipSize = QFileInfo(tempZipPath).size();

                    result.voxyCacheObj["file"] = "voxy_cache.zip";
                    result.voxyCacheObj["hash"] = zipHash;
                    result.voxyCacheObj["size"] = zipSize;

                    localRelPaths.insert("voxy_cache.zip");

                    if (!remoteHashes.contains("voxy_cache.zip") || remoteHashes["voxy_cache.zip"] != zipHash || m_forceVoxyRedownload) {
                        SyncAction voxyAction;
                        voxyAction.type = "PUT";
                        voxyAction.relPath = "voxy_cache.zip";
                        voxyAction.localOverridePath = tempZipPath;
                        voxyAction.size = zipSize;
                        result.actions.append(voxyAction);
                    }
                }
            }
        }
    }

    for (int i = 0; i < total; ++i) {
        if (m_aborted) {
            result.error = tr("Upload aborted.");
            return result;
        }

        const QString& relPath = m_selectedFiles[i];
        QString cleanPath = QString(relPath).replace('\\', '/');

        // Exclude individual Voxy cache files (they are packaged into voxy_cache.zip)
        if (cleanPath.contains(".voxy/saves/cozycreations.modpack.gg", Qt::CaseInsensitive)) {
            continue;
        }

        QString absPath = m_instance->instanceRoot() + "/" + relPath;
        QFileInfo fi(absPath);
        if (!fi.exists() || !fi.isFile()) {
            continue;
        }

        QString localHash = Hashing::hash(absPath, Hashing::Algorithm::Sha1).toLower();
        if (localHash.isEmpty()) {
            continue;
        }

        localRelPaths.insert(relPath);

        QJsonObject fileObj;
        fileObj["path"] = relPath;
        fileObj["hash"] = localHash;
        fileObj["size"] = fi.size();
        result.finalFiles.append(fileObj);

        if (!remoteHashes.contains(relPath) || remoteHashes[relPath] != localHash) {
            result.actions.append({"PUT", relPath, QString(), fi.size()});
        }

        if (i % 5 == 0 || i == total - 1) {
            setStatus(tr("Comparing files (%1/%2): %3").arg(QString::number(i + 1), QString::number(total), fi.fileName()));
            setDetails(relPath);
            setProgress(i + 1, total);
        }
    }

    // Determine files to delete (files in remote manifest not in local selected files)
    for (auto it = remoteHashes.begin(); it != remoteHashes.end(); ++it) {
        if (m_aborted) {
            result.error = tr("Upload aborted.");
            return result;
        }
        QString relPath = it.key();
        QString clean = QString(relPath).replace('\\', '/');
        // Delete any old individual Voxy SST files from remote storage
        if (clean.contains(".voxy/saves/cozycreations.modpack.gg", Qt::CaseInsensitive)) {
            result.actions.append({"DELETE", relPath, QString(), 0});
            continue;
        }
        if (!localRelPaths.contains(relPath)) {
            result.actions.append({"DELETE", relPath, QString(), 0});
        }
    }

    result.success = true;
    return result;
}

void SyncedInstanceUploadTask::diffComputed()
{
    if (m_aborted) {
        emitAborted();
        return;
    }

    DiffResult result = m_diffWatcher.result();
    if (!result.success) {
        emitFailed(result.error.isEmpty() ? tr("Failed to compute file diff.") : result.error);
        return;
    }

    m_finalFiles = result.finalFiles;
    m_voxyCacheObj = result.voxyCacheObj;
    m_actions = result.actions;

    m_totalBytes = 0;
    m_completedBytes = 0;
    for (const auto& act : m_actions) {
        if (act.type == "PUT") {
            m_totalBytes += act.size;
        }
    }

    qDebug() << "Sync plan generated: total actions =" << m_actions.size() << "total upload bytes =" << m_totalBytes;
    m_actionIndex = 0;
    performSync();
}

void SyncedInstanceUploadTask::performSync()
{
    if (m_aborted) {
        emitAborted();
        return;
    }

    if (m_actionIndex >= m_actions.size()) {
        qDebug() << "Syncing files complete, now uploading manifest.";
        uploadManifest();
        return;
    }

    SyncAction action = m_actions[m_actionIndex];
    QString fileName = QFileInfo(action.relPath).fileName();

    if (m_totalBytes > 0) {
        setProgress(m_completedBytes, m_totalBytes);
    } else {
        setProgress(m_actionIndex, m_actions.size());
    }

    if (action.type == "PUT") {
        QString sizeStr = StringUtils::humanReadableFileSize(action.size);
        setStatus(tr("Uploading (%1/%2): %3 (0 B / %4, 0%)")
                  .arg(QString::number(m_actionIndex + 1), QString::number(m_actions.size()), fileName, sizeStr));
    } else {
        setStatus(tr("Deleting (%1/%2): %3")
                  .arg(QString::number(m_actionIndex + 1), QString::number(m_actions.size()), fileName));
    }
    setDetails(action.relPath);

    // Build signed PUT/DELETE request using canonical percent-encoding
    QStringList segments = (m_bucket + "/packs/" + m_shortcode + "/" + action.relPath).split('/', Qt::SkipEmptyParts);
    QByteArray encodedPath;
    for (const QString& seg : segments) {
        encodedPath += "/" + QUrl::toPercentEncoding(seg);
    }
    QUrl url = QUrl::fromEncoded(m_endpoint.toUtf8() + encodedPath);

    if (action.type == "PUT") {
        QString absPath = action.localOverridePath.isEmpty() ? (m_instance->instanceRoot() + "/" + action.relPath) : action.localOverridePath;
        auto* file = new QFile(absPath);
        if (!file->open(QFile::ReadOnly)) {
            delete file;
            emitFailed(tr("Failed to read file for upload: %1").arg(action.relPath));
            return;
        }

        SigV4::SignedRequest signedReq = SigV4::sign(action.type, url, "UNSIGNED-PAYLOAD", m_accessKey, m_secretKey, "auto", "s3", true);

        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        req.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
        for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
            req.setRawHeader(it.key(), it.value());
        }

        m_currentActionReply = APPLICATION->network()->put(req, file);
        file->setParent(m_currentActionReply);

        connect(m_currentActionReply, &QNetworkReply::sslErrors, this, [this](const QList<QSslError>& errors) {
            for (const auto& err : errors) {
                qWarning() << "SSL Error during upload:" << err.errorString();
            }
        });

        connect(m_currentActionReply, &QNetworkReply::uploadProgress, this, [this, action, fileName](qint64 bytesSent, qint64 bytesTotal) {
            qint64 fileTotal = (bytesTotal > 0) ? bytesTotal : action.size;
            if (fileTotal > 0) {
                QString sentStr = StringUtils::humanReadableFileSize(bytesSent);
                QString totalStr = StringUtils::humanReadableFileSize(fileTotal);
                int percent = qBound(0, static_cast<int>((bytesSent * 100) / fileTotal), 100);

                setStatus(tr("Uploading (%1/%2): %3 (%4 / %5, %6%)")
                          .arg(QString::number(m_actionIndex + 1), QString::number(m_actions.size()), fileName, sentStr, totalStr, QString::number(percent)));
                setDetails(action.relPath);

                if (m_totalBytes > 0) {
                    qint64 overallProgress = qMin(m_totalBytes, m_completedBytes + bytesSent);
                    setProgress(overallProgress, m_totalBytes);
                }
            }
        });
    } else {
        SigV4::SignedRequest signedReq = SigV4::sign(action.type, url, QByteArray(), m_accessKey, m_secretKey);

        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
            req.setRawHeader(it.key(), it.value());
        }

        m_currentActionReply = APPLICATION->network()->deleteResource(req);
    }

    connect(m_currentActionReply, &QNetworkReply::finished, this, &SyncedInstanceUploadTask::actionFinished);
}

void SyncedInstanceUploadTask::actionFinished()
{
    if (!m_currentActionReply) {
        return;
    }

    SyncAction action = m_actions[m_actionIndex];
    if (action.type == "PUT") {
        m_completedBytes += action.size;
    }

    m_currentActionReply->deleteLater();
    auto reply = m_currentActionReply;
    m_currentActionReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        emitFailed(tr("Sync action failed (%1): %2").arg(reply->url().toString(), reply->errorString()));
        return;
    }

    m_actionIndex++;
    performSync();
}

void SyncedInstanceUploadTask::uploadManifest()
{
    setStatus(tr("Uploading manifest file..."));
    setDetails("shortcodes/" + m_shortcode + ".json");

    // Build the manifest JSON
    QJsonObject docObj;
    docObj["shortcode"] = m_shortcode;
    docObj["name"] = m_instance->name();
    docObj["version"] = m_instance->settings()->get("ExportVersion").toString();
    if (docObj["version"].toString().isEmpty()) {
        docObj["version"] = "1.0.0";
    }
    QString uploadVersion = docObj["version"].toString();
    if (m_forceVoxyRedownload) {
        docObj["voxy_cache_reset_version"] = uploadVersion;
        docObj["force_voxy_redownload"] = true;
    } else if (!m_remoteVoxyResetVersion.isEmpty()) {
        docObj["voxy_cache_reset_version"] = m_remoteVoxyResetVersion;
        docObj["force_voxy_redownload"] = false;
    } else {
        docObj["force_voxy_redownload"] = false;
    }
    if (!m_voxyCacheObj.isEmpty()) {
        docObj["voxy_cache"] = m_voxyCacheObj;
    }
    docObj["force_config_overwrite"] = m_forceConfigOverwrite;
    docObj["files"] = m_finalFiles;

    // If it's a MinecraftInstance we can try to extract loader configurations
    QString loaderType = "Vanilla";
    QString loaderVersion = "";
    auto mcInst = dynamic_cast<MinecraftInstance*>(m_instance);
    if (mcInst) {
        for (const auto& trait : mcInst->traits()) {
            if (trait == "fabric") {
                loaderType = "Fabric";
            } else if (trait == "forge") {
                loaderType = "Forge";
            } else if (trait == "neoforge") {
                loaderType = "NeoForge";
            } else if (trait == "quilt") {
                loaderType = "Quilt";
            }
        }
        // Grab version details
        auto components = mcInst->getPackProfile();
        if (components) {
            auto component = components->getComponent("net.fabricmc.fabric-loader");
            if (component) {
                loaderVersion = component->getVersion();
            } else {
                component = components->getComponent("net.minecraftforge");
                if (component) {
                    loaderVersion = component->getVersion();
                } else {
                    component = components->getComponent("org.neoforged.neoforge");
                    if (component) {
                        loaderVersion = component->getVersion();
                    } else {
                        component = components->getComponent("org.quiltmc.quilt-loader");
                        if (component) {
                            loaderVersion = component->getVersion();
                        }
                    }
                }
            }
        }
    }

    docObj["game_version"] = m_instance->settings()->get("IntendedVersion").toString();
    if (docObj["game_version"].toString().isEmpty()) {
        docObj["game_version"] = "1.21.1";
    }
    docObj["loader"] = loaderType;
    docObj["loader_version"] = loaderVersion;

    bool overrideMem = m_instance->settings()->get("OverrideMemory").toBool();
    docObj["override_memory"] = overrideMem;
    if (overrideMem) {
        docObj["min_memory"] = m_instance->settings()->get("MinMemAlloc").toInt();
        docObj["max_memory"] = m_instance->settings()->get("MaxMemAlloc").toInt();
    }

    bool overrideArgs = m_instance->settings()->get("OverrideJavaArgs").toBool();
    docObj["override_java_args"] = overrideArgs;
    if (overrideArgs) {
        docObj["jvm_args"] = m_instance->settings()->get("JvmArgs").toString().trimmed();
    }

    QJsonDocument doc(docObj);
    m_manifestData = doc.toJson(QJsonDocument::Compact);

    QStringList segments = (m_bucket + "/shortcodes/" + m_shortcode + ".json").split('/', Qt::SkipEmptyParts);
    QByteArray encodedPath;
    for (const QString& seg : segments) {
        encodedPath += "/" + QUrl::toPercentEncoding(seg);
    }
    QUrl url = QUrl::fromEncoded(m_endpoint.toUtf8() + encodedPath);

    SigV4::SignedRequest signedReq = SigV4::sign("PUT", url, m_manifestData, m_accessKey, m_secretKey);

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setHeader(QNetworkRequest::ContentLengthHeader, m_manifestData.size());
    for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    m_currentActionReply = APPLICATION->network()->put(req, m_manifestData);
    connect(m_currentActionReply, &QNetworkReply::uploadProgress, this, [this](qint64 bytesSent, qint64 bytesTotal) {
        if (bytesTotal > 0) {
            QString sentStr = StringUtils::humanReadableFileSize(bytesSent);
            QString totalStr = StringUtils::humanReadableFileSize(bytesTotal);
            setStatus(tr("Uploading manifest (%1 / %2)...").arg(sentStr, totalStr));
        }
    });
    connect(m_currentActionReply, &QNetworkReply::finished, this, &SyncedInstanceUploadTask::manifestUploaded);
}

void SyncedInstanceUploadTask::manifestUploaded()
{
    if (!m_currentActionReply) {
        return;
    }

    m_currentActionReply->deleteLater();
    auto reply = m_currentActionReply;
    m_currentActionReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        emitFailed(tr("Failed to upload manifest: %1").arg(reply->errorString()));
        return;
    }

    if (!m_bannerImagePath.isEmpty()) {
        uploadBanner();
    } else {
        fetchRegistry();
    }
}

void SyncedInstanceUploadTask::uploadBanner()
{
    setStatus(tr("Uploading banner image..."));
    setDetails("banners/" + m_shortcode + ".png");

    QFile file(m_bannerImagePath);
    if (!file.open(QFile::ReadOnly)) {
        emitFailed(tr("Failed to open banner image: %1").arg(m_bannerImagePath));
        return;
    }
    QByteArray bannerData = file.readAll();

    // Construct url: /bucket/banners/shortcode.png using QUrl::fromEncoded for standard percent-encoding
    QByteArray encodedPath;
    QStringList segments = (m_bucket + "/banners/" + m_shortcode + ".png").split('/', Qt::SkipEmptyParts);
    for (const QString& seg : segments) {
        encodedPath += "/" + QUrl::toPercentEncoding(seg);
    }
    QUrl url = QUrl::fromEncoded(m_endpoint.toUtf8() + encodedPath);

    SigV4::SignedRequest signedReq = SigV4::sign("PUT", url, bannerData, m_accessKey, m_secretKey);

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setHeader(QNetworkRequest::ContentLengthHeader, bannerData.size());
    for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    m_currentActionReply = APPLICATION->network()->put(req, bannerData);
    connect(m_currentActionReply, &QNetworkReply::uploadProgress, this, [this](qint64 bytesSent, qint64 bytesTotal) {
        if (bytesTotal > 0) {
            QString sentStr = StringUtils::humanReadableFileSize(bytesSent);
            QString totalStr = StringUtils::humanReadableFileSize(bytesTotal);
            setStatus(tr("Uploading banner (%1 / %2)...").arg(sentStr, totalStr));
        }
    });
    connect(m_currentActionReply, &QNetworkReply::finished, this, &SyncedInstanceUploadTask::bannerUploaded);
}

void SyncedInstanceUploadTask::bannerUploaded()
{
    if (!m_currentActionReply) {
        return;
    }
    m_currentActionReply->deleteLater();
    auto reply = m_currentActionReply;
    m_currentActionReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        emitFailed(tr("Failed to upload banner: %1").arg(reply->errorString()));
        return;
    }

    fetchRegistry();
}

void SyncedInstanceUploadTask::fetchRegistry()
{
    setStatus(tr("Fetching registry to update list..."));
    setDetails("registry.json");

    QString publicUrl = m_publicUrl;
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    QUrl url(publicUrl + "registry.json?t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    m_manifestReply = APPLICATION->network()->get(req);
    connect(m_manifestReply, &QNetworkReply::finished, this, &SyncedInstanceUploadTask::registryFetched);
}

void SyncedInstanceUploadTask::registryFetched()
{
    if (!m_manifestReply) {
        return;
    }

    m_manifestReply->deleteLater();
    auto reply = m_manifestReply;
    m_manifestReply = nullptr;

    QJsonObject registryObj;
    QJsonArray packs;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isNull() && doc.isObject()) {
            registryObj = doc.object();
            packs = registryObj["packs"].toArray();
        }
    } else {
        qDebug() << "Registry fetch returned error or 404 (generating new registry):" << reply->errorString();
    }

    QString name = m_instance->name();
    QString desc = m_instance->settings()->get("ExportSummary").toString().trimmed();
    QString ver = m_instance->settings()->get("ExportVersion").toString();
    if (ver.isEmpty()) {
        ver = "1.0.0";
    }
    QString mcVer = m_instance->settings()->get("IntendedVersion").toString();
    if (mcVer.isEmpty()) {
        mcVer = "1.21.1";
    }

    // Default banners/icons based on shortcode if not set
    QString publicUrl = m_publicUrl;
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    QString bannerUrl = publicUrl + "assets/" + m_shortcode + "-banner.png";
    if (!m_bannerImagePath.isEmpty()) {
        bannerUrl = publicUrl + "banners/" + m_shortcode + ".png";
    }
    QString iconUrl = publicUrl + "assets/" + m_shortcode + "-icon.png";

    bool found = false;
    for (int i = 0; i < packs.size(); ++i) {
        QJsonObject pack = packs[i].toObject();
        if (pack["shortcode"].toString() == m_shortcode) {
            pack["name"] = name;

            // If local description is left empty, default to existing remote description on server
            if (desc.isEmpty() && pack.contains("description") && !pack["description"].toString().trimmed().isEmpty()) {
                desc = pack["description"].toString().trimmed();
                m_instance->settings()->set("ExportSummary", desc);
            }
            if (desc.isEmpty()) {
                desc = tr("A synced custom modpack.");
            }

            pack["description"] = desc;
            pack["version"] = ver;
            pack["game_version"] = mcVer;
            // keep existing banners/icons if they were custom
            if (m_bannerImagePath.isEmpty() && pack.contains("banner_url")) bannerUrl = pack["banner_url"].toString();
            if (pack.contains("icon_url")) iconUrl = pack["icon_url"].toString();
            pack["banner_url"] = bannerUrl;
            pack["icon_url"] = iconUrl;
            pack["is_private"] = m_instance->settings()->get("SyncIsPrivate").toBool();
            packs[i] = pack;
            found = true;
            break;
        }
    }

    if (!found) {
        if (desc.isEmpty()) {
            desc = tr("A synced custom modpack.");
        }
        QJsonObject pack;
        pack["shortcode"] = m_shortcode;
        pack["name"] = name;
        pack["description"] = desc;
        pack["version"] = ver;
        pack["game_version"] = mcVer;
        pack["banner_url"] = bannerUrl;
        pack["icon_url"] = iconUrl;
        pack["is_private"] = m_instance->settings()->get("SyncIsPrivate").toBool();
        packs.append(pack);
    }

    registryObj["packs"] = packs;
    QJsonDocument doc(registryObj);
    uploadRegistry(doc.toJson(QJsonDocument::Compact));
}

void SyncedInstanceUploadTask::uploadRegistry(const QByteArray& registryData)
{
    setStatus(tr("Uploading registry file..."));
    setDetails("registry.json");

    QStringList segments = (m_bucket + "/registry.json").split('/', Qt::SkipEmptyParts);
    QByteArray encodedPath;
    for (const QString& seg : segments) {
        encodedPath += "/" + QUrl::toPercentEncoding(seg);
    }
    QUrl url = QUrl::fromEncoded(m_endpoint.toUtf8() + encodedPath);

    SigV4::SignedRequest signedReq = SigV4::sign("PUT", url, registryData, m_accessKey, m_secretKey);

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setHeader(QNetworkRequest::ContentLengthHeader, registryData.size());
    for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    m_currentActionReply = APPLICATION->network()->put(req, registryData);
    connect(m_currentActionReply, &QNetworkReply::finished, this, [this]() {
        if (!m_currentActionReply) {
            return;
        }
        m_currentActionReply->deleteLater();
        auto reply = m_currentActionReply;
        m_currentActionReply = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            emitFailed(tr("Failed to upload registry: %1").arg(reply->errorString()));
            return;
        }

        m_instance->settings()->set("SyncVersion", m_instance->settings()->get("ExportVersion").toString());
        m_instance->settings()->set("SyncVersionName", m_instance->settings()->get("ExportVersion").toString());
        m_instance->saveNow();

        setStatus(tr("Syncing completed successfully!"));
        setDetails("");
        emitSucceeded();
    });
}
