#include "ModpackUpdateCheckTask.h"
#include "Application.h"
#include "settings/SettingsObject.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>

ModpackUpdateCheckTask::ModpackUpdateCheckTask(QList<BaseInstance*> instances)
    : Task(true), m_instancesToCheck(instances)
{
}

bool ModpackUpdateCheckTask::abort()
{
    for (auto* reply : m_replies) {
        if (reply && reply->isRunning()) {
            reply->abort();
        }
    }
    m_replies.clear();
    return true;
}

void ModpackUpdateCheckTask::executeTask()
{
    setStatus(tr("Checking for modpack updates..."));

    QList<BaseInstance*> syncedInstances;
    for (auto* inst : m_instancesToCheck) {
        if (!inst)
            continue;
        if (inst->settings()->get("IsSyncedInstance").toBool()) {
            QString shortcode = inst->settings()->get("SyncShortcode").toString();
            if (!shortcode.isEmpty()) {
                syncedInstances.append(inst);
            }
        }
    }

    if (syncedInstances.isEmpty()) {
        qDebug() << "No synced instances to check for updates.";
        emitSucceeded();
        return;
    }

    m_pendingCount = syncedInstances.size();
    setProgress(0, m_pendingCount);

    for (auto* inst : syncedInstances) {
        checkInstance(inst);
    }
}

void ModpackUpdateCheckTask::checkInstance(BaseInstance* inst)
{
    QString shortcode = inst->settings()->get("SyncShortcode").toString();
    QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    QUrl url(publicUrl + "shortcodes/" + shortcode + ".json?t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    qDebug() << "Checking modpack update for" << inst->name() << "at" << url.toString();

    QNetworkReply* reply = APPLICATION->network()->get(QNetworkRequest(url));
    m_replies.append(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, inst]() {
        m_replies.removeAll(reply);
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray data = reply->readAll();
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(data, &err);
            if (!doc.isNull() && doc.isObject()) {
                QJsonObject obj = doc.object();
                QString remoteVersion = obj["version"].toString();
                QString currentVersion = inst->settings()->get("SyncVersion").toString();

                qDebug() << "Instance:" << inst->name() << "Local ver:" << currentVersion << "Remote ver:" << remoteVersion;

                if (!remoteVersion.isEmpty() && remoteVersion != currentVersion) {
                    inst->setHasModpackUpdate(true, remoteVersion);
                    m_updatedInstances.append(inst);
                } else {
                    inst->setHasModpackUpdate(false);
                }
            }
        } else {
            qWarning() << "Failed to check update for" << inst->name() << ":" << reply->errorString();
        }

        checkNextOrFinish();
    });
}

void ModpackUpdateCheckTask::checkNextOrFinish()
{
    m_pendingCount--;
    if (m_pendingCount <= 0) {
        qDebug() << "Finished checking for modpack updates. Found updates for" << m_updatedInstances.size() << "instances.";
        emitSucceeded();
    }
}
