#include "SyncedInstanceUploadTask.h"
#include "Application.h"
#include "settings/SettingsObject.h"
#include "modplatform/helpers/HashUtils.h"
#include "net/R2Signer.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/Component.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QDateTime>

SyncedInstanceUploadTask::SyncedInstanceUploadTask(BaseInstance* instance, const QString& accessKey, const QString& secretKey, const QStringList& selectedFiles, const QString& bannerImagePath)
    : Task(true), m_instance(instance), m_accessKey(accessKey), m_secretKey(secretKey), m_bannerImagePath(bannerImagePath), m_selectedFiles(selectedFiles)
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
    if (m_manifestReply) {
        m_manifestReply->abort();
        return true;
    }
    if (m_currentActionReply) {
        m_currentActionReply->abort();
        return true;
    }
    return false;
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
    m_manifestReply = APPLICATION->network()->get(QNetworkRequest(url));
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

    QMap<QString, QString> remoteHashes;

    // A 404 error is acceptable; it means this is a new modpack upload
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isNull() && doc.isObject()) {
            QJsonArray files = doc.object()["files"].toArray();
            for (int i = 0; i < files.size(); ++i) {
                QJsonObject fileObj = files[i].toObject();
                remoteHashes[fileObj["path"].toString()] = fileObj["hash"].toString().toLower();
            }
        }
    } else {
        qDebug() << "Manifest fetch returned error or 404 (acceptable for new packs):" << reply->errorString();
    }

    m_actions.clear();
    m_finalFiles = QJsonArray();

    QSet<QString> localRelPaths;

    // Use selected files
    for (const QString& relPath : m_selectedFiles) {
        QString absPath = m_instance->instanceRoot() + "/" + relPath;
        QFile file(absPath);
        if (!file.open(QFile::ReadOnly)) {
            continue;
        }
        QByteArray data = file.readAll();
        localRelPaths.insert(relPath);

        QString localHash = Hashing::hash(data, Hashing::Algorithm::Sha1).toLower();

        QJsonObject fileObj;
        fileObj["path"] = relPath;
        fileObj["hash"] = localHash;
        fileObj["size"] = data.size();
        m_finalFiles.append(fileObj);

        if (!remoteHashes.contains(relPath) || remoteHashes[relPath] != localHash) {
            m_actions.append({"PUT", relPath, data});
        }
    }

    // Determine files to delete (any file that was in the remote manifest but is NOT in the selected files list)
    for (auto remoteIt = remoteHashes.begin(); remoteIt != remoteHashes.end(); ++remoteIt) {
        QString relPath = remoteIt.key();
        if (!localRelPaths.contains(relPath)) {
            m_actions.append({"DELETE", relPath, QByteArray()});
        }
    }

    qDebug() << "Sync plan generated: PUT actions =" << m_actions.size();
    m_actionIndex = 0;
    performSync();
}

void SyncedInstanceUploadTask::performSync()
{
    if (m_actionIndex >= m_actions.size()) {
        qDebug() << "Syncing files complete, now uploading manifest.";
        uploadManifest();
        return;
    }

    SyncAction action = m_actions[m_actionIndex];
    setStatus(tr("Syncing files (%1/%2): %3 %4")
              .arg(QString::number(m_actionIndex + 1), QString::number(m_actions.size()), action.type, action.relPath));
    setProgress(m_actionIndex, m_actions.size());

    // Build signed PUT/DELETE request using canonical percent-encoding
    QStringList segments = (m_bucket + "/packs/" + m_shortcode + "/" + action.relPath).split('/', Qt::SkipEmptyParts);
    QByteArray encodedPath;
    for (const QString& seg : segments) {
        encodedPath += "/" + QUrl::toPercentEncoding(seg);
    }
    QUrl url = QUrl::fromEncoded(m_endpoint.toUtf8() + encodedPath);

    SigV4::SignedRequest signedReq = SigV4::sign(action.type, url, action.data, m_accessKey, m_secretKey);

    QNetworkRequest req(url);
    for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    if (action.type == "PUT") {
        m_currentActionReply = APPLICATION->network()->put(req, action.data);
    } else {
        m_currentActionReply = APPLICATION->network()->deleteResource(req);
    }

    connect(m_currentActionReply, &QNetworkReply::finished, this, &SyncedInstanceUploadTask::actionFinished);
}

void SyncedInstanceUploadTask::actionFinished()
{
    if (!m_currentActionReply) {
        return;
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

    // Build the manifest JSON
    QJsonObject docObj;
    docObj["shortcode"] = m_shortcode;
    docObj["name"] = m_instance->name();
    docObj["version"] = m_instance->settings()->get("ExportVersion").toString();
    if (docObj["version"].toString().isEmpty()) {
        docObj["version"] = "1.0.0";
    }
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
    for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    m_currentActionReply = APPLICATION->network()->put(req, m_manifestData);
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
    for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    m_currentActionReply = APPLICATION->network()->put(req, bannerData);
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

    QString publicUrl = m_publicUrl;
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    QUrl url(publicUrl + "registry.json?t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    m_manifestReply = APPLICATION->network()->get(QNetworkRequest(url));
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
    QString desc = m_instance->settings()->get("ExportSummary").toString();
    if (desc.isEmpty()) {
        desc = "A synced custom modpack.";
    }
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

    QStringList segments = (m_bucket + "/registry.json").split('/', Qt::SkipEmptyParts);
    QByteArray encodedPath;
    for (const QString& seg : segments) {
        encodedPath += "/" + QUrl::toPercentEncoding(seg);
    }
    QUrl url = QUrl::fromEncoded(m_endpoint.toUtf8() + encodedPath);

    SigV4::SignedRequest signedReq = SigV4::sign("PUT", url, registryData, m_accessKey, m_secretKey);

    QNetworkRequest req(url);
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
        emitSucceeded();
    });
}
