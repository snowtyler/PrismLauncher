#include "SyncedInstancePurgeTask.h"
#include "Application.h"
#include "settings/SettingsObject.h"
#include "net/R2Signer.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

SyncedInstancePurgeTask::SyncedInstancePurgeTask(const QString& shortcode, const QString& accessKey, const QString& secretKey)
    : Task(true), m_shortcode(shortcode), m_accessKey(accessKey), m_secretKey(secretKey)
{
    m_endpoint = APPLICATION->settings()->get("SyncR2Endpoint").toString();
    if (m_endpoint.endsWith('/'))
        m_endpoint.chop(1);
    m_bucket = APPLICATION->settings()->get("SyncR2Bucket").toString();
    QString bucketSuffix = "/" + m_bucket;
    if (m_endpoint.endsWith(bucketSuffix, Qt::CaseInsensitive))
        m_endpoint.chop(bucketSuffix.length());
    m_publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
    if (!m_publicUrl.endsWith('/'))
        m_publicUrl += '/';
}

bool SyncedInstancePurgeTask::abort()
{
    if (m_currentReply) {
        m_currentReply->abort();
    }
    return Task::abort();
}

void SyncedInstancePurgeTask::executeTask()
{
    setStatus(tr("Fetching manifest for %1...").arg(m_shortcode));
    fetchManifest();
}

QUrl SyncedInstancePurgeTask::buildS3Url(const QString& key)
{
    QStringList segments = (m_bucket + "/" + key).split('/', Qt::SkipEmptyParts);
    QByteArray encodedPath;
    for (const QString& seg : segments) {
        encodedPath += "/" + QUrl::toPercentEncoding(seg);
    }
    return QUrl::fromEncoded(m_endpoint.toUtf8() + encodedPath);
}

void SyncedInstancePurgeTask::fetchManifest()
{
    QUrl url(m_publicUrl + "shortcodes/" + m_shortcode + ".json?t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    m_currentReply = APPLICATION->network()->get(QNetworkRequest(url));
    connect(m_currentReply, &QNetworkReply::finished, this, &SyncedInstancePurgeTask::manifestFetched);
}

void SyncedInstancePurgeTask::manifestFetched()
{
    if (!m_currentReply)
        return;

    m_currentReply->deleteLater();
    auto reply = m_currentReply;
    m_currentReply = nullptr;

    m_deleteKeys.clear();

    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isNull() && doc.isObject()) {
            QJsonArray files = doc.object()["files"].toArray();
            for (int i = 0; i < files.size(); ++i) {
                QJsonObject f = files[i].toObject();
                QString relPath = f["path"].toString();
                if (!relPath.isEmpty()) {
                    m_deleteKeys.append("packs/" + m_shortcode + "/" + relPath);
                }
            }

            QJsonObject voxyCache = doc.object()["voxy_cache"].toObject();
            if (!voxyCache.isEmpty()) {
                QString voxyPath = voxyCache["path"].toString();
                if (!voxyPath.isEmpty()) {
                    m_deleteKeys.append("packs/" + m_shortcode + "/" + voxyPath);
                }
            }
        }
    }

    m_deleteKeys.append("shortcodes/" + m_shortcode + ".json");
    m_deleteKeys.append("banners/" + m_shortcode + ".png");
    m_deleteKeys.append("assets/" + m_shortcode + "-banner.png");
    m_deleteKeys.append("assets/" + m_shortcode + "-icon.png");

    m_deleteIndex = 0;
    setStatus(tr("Deleting %1 remote files...").arg(m_deleteKeys.size()));
    performDeletes();
}

void SyncedInstancePurgeTask::performDeletes()
{
    if (m_deleteIndex >= m_deleteKeys.size()) {
        setStatus(tr("Updating registry..."));
        fetchRegistry();
        return;
    }

    const QString& key = m_deleteKeys[m_deleteIndex];
    setProgress(m_deleteIndex, m_deleteKeys.size());
    setDetails(key);

    QUrl url = buildS3Url(key);
    SigV4::SignedRequest signedReq = SigV4::sign("DELETE", url, QByteArray(), m_accessKey, m_secretKey);

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    m_currentReply = APPLICATION->network()->deleteResource(req);
    connect(m_currentReply, &QNetworkReply::finished, this, &SyncedInstancePurgeTask::deleteFinished);
}

void SyncedInstancePurgeTask::deleteFinished()
{
    if (!m_currentReply)
        return;

    m_currentReply->deleteLater();
    auto reply = m_currentReply;
    m_currentReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "Purge: failed to delete" << m_deleteKeys[m_deleteIndex] << ":" << reply->errorString()
                 << "HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }

    m_deleteIndex++;
    performDeletes();
}

void SyncedInstancePurgeTask::fetchRegistry()
{
    QUrl url(m_publicUrl + "registry.json?t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    m_currentReply = APPLICATION->network()->get(QNetworkRequest(url));
    connect(m_currentReply, &QNetworkReply::finished, this, &SyncedInstancePurgeTask::registryFetched);
}

void SyncedInstancePurgeTask::registryFetched()
{
    if (!m_currentReply)
        return;

    m_currentReply->deleteLater();
    auto reply = m_currentReply;
    m_currentReply = nullptr;

    QJsonObject registryObj;
    QJsonArray packs;

    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isNull() && doc.isObject()) {
            registryObj = doc.object();
            packs = registryObj["packs"].toArray();
        }
    }

    QJsonArray filtered;
    for (int i = 0; i < packs.size(); ++i) {
        QJsonObject pack = packs[i].toObject();
        if (pack["shortcode"].toString() != m_shortcode) {
            filtered.append(pack);
        }
    }

    registryObj["packs"] = filtered;
    QJsonDocument doc(registryObj);
    uploadRegistry(doc.toJson(QJsonDocument::Compact));
}

void SyncedInstancePurgeTask::uploadRegistry(const QByteArray& registryData)
{
    setStatus(tr("Uploading updated registry..."));
    setDetails("registry.json");

    QUrl url = buildS3Url("registry.json");
    SigV4::SignedRequest signedReq = SigV4::sign("PUT", url, registryData, m_accessKey, m_secretKey);

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setHeader(QNetworkRequest::ContentLengthHeader, registryData.size());
    for (auto it = signedReq.headers.begin(); it != signedReq.headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    m_currentReply = APPLICATION->network()->put(req, registryData);
    connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
        if (!m_currentReply)
            return;
        m_currentReply->deleteLater();
        auto reply = m_currentReply;
        m_currentReply = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            emitFailed(tr("Failed to upload registry: %1").arg(reply->errorString()));
            return;
        }
        emitSucceeded();
    });
}
