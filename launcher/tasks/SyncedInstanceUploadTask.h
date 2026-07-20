#pragma once

#include "tasks/Task.h"
#include "BaseInstance.h"
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>

class SyncedInstanceUploadTask : public Task {
    Q_OBJECT
public:
    SyncedInstanceUploadTask(BaseInstance* instance, const QString& accessKey, const QString& secretKey, const QStringList& selectedFiles, const QString& bannerImagePath = QString());
    virtual ~SyncedInstanceUploadTask() = default;

    bool abort() override;

protected:
    void executeTask() override;

private slots:
    void remoteManifestFetched();
    void actionFinished();
    void manifestUploaded();
    void bannerUploaded();
    void registryFetched();

private:
    BaseInstance* m_instance;
    QString m_accessKey;
    QString m_secretKey;
    QString m_bannerImagePath;

    QNetworkReply* m_manifestReply = nullptr;
    QNetworkReply* m_currentActionReply = nullptr;

    QString m_shortcode;
    QString m_endpoint;
    QString m_bucket;
    QString m_publicUrl;
    QStringList m_selectedFiles;

    struct SyncAction {
        QString type; // "PUT" or "DELETE"
        QString relPath;
        QByteArray data; // For PUT
    };

    QList<SyncAction> m_actions;
    int m_actionIndex = 0;

    QJsonArray m_finalFiles; // To build the manifest at the end
    QByteArray m_manifestData;       // Generated manifest content

    void fetchRemoteManifest();
    void performSync();
    void uploadManifest();
    void uploadBanner();
    void fetchRegistry();
    void updateRegistry();
    void uploadRegistry(const QByteArray& registryData);
};
