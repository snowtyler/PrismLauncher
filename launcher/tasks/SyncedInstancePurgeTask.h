#pragma once

#include "tasks/Task.h"
#include <QNetworkReply>
#include <QJsonArray>
#include <QStringList>

class SyncedInstancePurgeTask : public Task {
    Q_OBJECT
public:
    SyncedInstancePurgeTask(const QString& shortcode, const QString& accessKey, const QString& secretKey);
    virtual ~SyncedInstancePurgeTask() = default;

    bool abort() override;

protected:
    void executeTask() override;

private slots:
    void manifestFetched();
    void registryFetched();
    void deleteFinished();

private:
    QString m_shortcode;
    QString m_accessKey;
    QString m_secretKey;
    QString m_endpoint;
    QString m_bucket;
    QString m_publicUrl;

    QNetworkReply* m_currentReply = nullptr;

    QStringList m_deleteKeys;
    int m_deleteIndex = 0;

    void fetchManifest();
    void fetchRegistry();
    void performDeletes();
    void uploadRegistry(const QByteArray& registryData);

    QUrl buildS3Url(const QString& key);
    void signedDelete(const QUrl& url);
};
