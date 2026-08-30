#pragma once

#include "tasks/Task.h"
#include "BaseInstance.h"
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QFutureWatcher>
#include <QAtomicInteger>
#include <QMap>

class SyncedInstanceUploadTask : public Task {
    Q_OBJECT
public:
    SyncedInstanceUploadTask(BaseInstance* instance, const QString& accessKey, const QString& secretKey, const QStringList& selectedFiles, const QString& bannerImagePath = QString(), bool forceConfigOverwrite = false, bool forceVoxyRedownload = false);
    virtual ~SyncedInstanceUploadTask() = default;

    bool abort() override;

    void setForceConfigOverwrite(bool force) { m_forceConfigOverwrite = force; }
    bool forceConfigOverwrite() const { return m_forceConfigOverwrite; }

    void setForceVoxyRedownload(bool force) { m_forceVoxyRedownload = force; }
    bool forceVoxyRedownload() const { return m_forceVoxyRedownload; }

protected:
    void executeTask() override;

private slots:
    void remoteManifestFetched();
    void diffComputed();
    void actionFinished();
    void manifestUploaded();
    void bannerUploaded();
    void registryFetched();

private:
    BaseInstance* m_instance;
    QString m_accessKey;
    QString m_secretKey;
    QString m_bannerImagePath;
    bool m_forceConfigOverwrite = false;
    bool m_forceVoxyRedownload = false;
    QString m_remoteVoxyResetVersion;

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
        QString localOverridePath;
        qint64 size = 0;
    };

    struct DiffResult {
        bool success = false;
        QString error;
        QJsonArray finalFiles;
        QList<SyncAction> actions;
        QJsonObject voxyCacheObj;
    };

    QFutureWatcher<DiffResult> m_diffWatcher;
    QAtomicInteger<bool> m_aborted = false;

    QList<SyncAction> m_actions;
    int m_actionIndex = 0;
    qint64 m_totalBytes = 0;
    qint64 m_completedBytes = 0;

    QString m_loaderType;
    QString m_gameVersion;
    QJsonArray m_finalFiles; // To build the manifest at the end
    QJsonObject m_voxyCacheObj; // Voxy cache single-zip metadata
    QByteArray m_manifestData;       // Generated manifest content

    void fetchRemoteManifest();
    DiffResult computeDiff(const QMap<QString, QString>& remoteHashes);
    void performSync();
    void uploadManifest();
    void uploadBanner();
    void fetchRegistry();
    void updateRegistry();
    void uploadRegistry(const QByteArray& registryData);
};
